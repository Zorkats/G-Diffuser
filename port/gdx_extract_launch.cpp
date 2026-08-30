// G-Diffuser — runtime O2R asset extraction launcher. See gdx_extract_launch.h for the contract map.

#include "gdx_extract_launch.h"

#include "gdx_hackmods.h" // GDX_HACKMOD_NAME_MAX + the picker's own basename sanitiser
#include "gdx_firstboot.h" // GdxFindIplSourceInDir -- shared IPL-source alt-name probe
#include "port_log.h"

// Golden-archive expectations, emitted by the o2r harness generator. The entire code-level contract
// between this file and the harness is these two macro names:
//   GDX_O2R_EXPECTED_SHA256       — hex string, the deterministic archive's SHA-256.
//   GDX_O2R_EXPECTED_ENTRY_COUNT  — integer, the archive's zip entry count (4240).
// Without the generated header this TU still compiles against inert placeholders: no real archive can
// match a placeholder SHA-256, so every extraction "fails validation" and boot degrades to the
// raw-ROM fallback rather than breaking.
#if defined(__has_include)
#if __has_include("gen/gdx_o2r_expected.h")
#include "gen/gdx_o2r_expected.h"
#endif
#endif
#ifndef GDX_O2R_EXPECTED_SHA256
#ifdef _MSC_VER
#pragma message("gdx_extract_launch.cpp: gen/gdx_o2r_expected.h not found - using placeholder O2R expectations (extraction always falls back to raw). Run the 1-C harness to generate it.")
#else
#warning "gen/gdx_o2r_expected.h not found - using placeholder O2R expectations (extraction always falls back to raw). Run the 1-C harness to generate it."
#endif
#define GDX_O2R_EXPECTED_SHA256 "0000000000000000000000000000000000000000000000000000000000000000"
#endif
#ifndef GDX_O2R_EXPECTED_ENTRY_COUNT
#define GDX_O2R_EXPECTED_ENTRY_COUNT 0
#endif

// JP-profile golden constants. No validated JP archive exists yet, so the placeholder zeros below
// make the JP path EXPERIMENTAL: it produces and installs fzerox-jp.o2r but SKIPS the SHA-256 /
// entry-count gates and logs the archive loudly as unverified. Generating a real
// gen/gdx_o2r_expected.jp.h (tools/o2r_harness/gen_expected_header.py --profile jp) engages the
// gates automatically.
#if defined(__has_include)
#if __has_include("gen/gdx_o2r_expected.jp.h")
#include "gen/gdx_o2r_expected.jp.h"
#endif
#endif
#ifndef GDX_O2R_EXPECTED_SHA256_JP
#define GDX_O2R_EXPECTED_SHA256_JP "0000000000000000000000000000000000000000000000000000000000000000"
#endif
#ifndef GDX_O2R_EXPECTED_ENTRY_COUNT_JP
#define GDX_O2R_EXPECTED_ENTRY_COUNT_JP 0
#endif

// The IPL archive's golden constants are PER-USER self-consistency checks — no single canonical
// retail IPL hash is assumed — so they are diagnostic only: a dev build warns on drift from the
// reference header, and the runtime IPL step never gates the mount on them (the archive carries its
// own ipl/identity entry, and absence falls back to the raw IPL). Placeholder zeros compile cleanly
// and suppress the drift warning until tools/o2r_harness/gen_ipl_expected.py generates a real header.
#if defined(__has_include)
#if __has_include("gen/gdx_ipl_expected.h")
#include "gen/gdx_ipl_expected.h"
#endif
#endif
#ifndef GDX_IPL_EXPECTED_SHA256
#define GDX_IPL_EXPECTED_SHA256 "0000000000000000000000000000000000000000000000000000000000000000"
#endif
#ifndef GDX_IPL_ARCHIVE_EXPECTED_SHA256
#define GDX_IPL_ARCHIVE_EXPECTED_SHA256 "0000000000000000000000000000000000000000000000000000000000000000"
#endif

// The 64DD EK disk archive's golden constants, like the IPL golden, are PER-USER self-consistency /
// dev-drift diagnostics only — the runtime disk step never gates the mount on them (fzerox-disk.o2r
// carries its own disk/identity entry, and absence falls back to the managed copy / raw .ndd).
// Placeholder zeros compile cleanly and suppress the dev-drift warning until
// tools/o2r_harness/gen_disk_expected.py generates a real header.
#if defined(__has_include)
#if __has_include("gen/gdx_disk_expected.h")
#include "gen/gdx_disk_expected.h"
#endif
#endif
#ifndef GDX_DISK_EXPECTED_SHA256
#define GDX_DISK_EXPECTED_SHA256 "0000000000000000000000000000000000000000000000000000000000000000"
#endif
#ifndef GDX_DISK_ARCHIVE_EXPECTED_SHA256
#define GDX_DISK_ARCHIVE_EXPECTED_SHA256 "0000000000000000000000000000000000000000000000000000000000000000"
#endif
// 2 frozen disk/* entries + the ek/<symbol> per-asset entries. That total grows with the EK slice
// manifest, so it comes from the golden header rather than a hardcoded number, falling back to the
// base 2 when the header is absent.
#ifndef GDX_DISK_EXPECTED_ENTRY_COUNT
#define GDX_DISK_EXPECTED_ENTRY_COUNT 2
#endif

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <string>
#include <vector>
#include <atomic>
#include <mutex>
#include <thread>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <commctrl.h> // PROGRESS_CLASS / PBM_SETMARQUEE (modeless progress dialog)
#include <thread>
#include <mutex>
#include <atomic>
#else
#include <cerrno>
#include <csignal>   // kill() for the extraction hang guard
#include <ctime>     // time() deadline for the hang guard
#include <sys/select.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace gdx {
namespace {

// ── Constants ────────────────────────────────────────────────────────────────────────────────────

// The installed archive is named after the game (matching fzerox.sav and the SoH/Starship convention
// of game-named archives). The extractor child always writes Torch's fixed output name into the temp
// dir; the atomic install renames it to the selected profile's name.
constexpr const char* kArchiveName = "fzerox.o2r";
constexpr const char* kArchiveNameJp = "fzerox-jp.o2r";
constexpr const char* kExtractorOutputName = "generic.o2r";
constexpr const char* kSidecarName = "gdx_extract_state.cfg";
constexpr const char* kRecipesDirName = "decomp-recipes";
constexpr const char* kConfigYmlName = "config.yml";
constexpr const char* kTorchHashName = "torch.hash.yml"; // stray artifact the extractor may leave
constexpr const char* kTempSubdir = ".gdx_extract.tmp";  // same-filesystem staging dir under dataDir

// The dedicated 64DD IPL font-block archive and the raw IPL ROM it is built from. ensureIplArchive is
// independent of the cartridge archive — different source media, own golden, own sidecar keys — and
// its output is unversioned, so it mounts through the HasGameVersion gate.
constexpr const char* kIplArchiveName = "n64ddipl.o2r";
constexpr const char* kIplRomName = "N64DDIPLROM.n64";
constexpr const char* kIplTempSubdir = ".gdx_ipl.tmp";
constexpr int kIplExpectedEntryCount = 2; // ipl/font_block + ipl/identity

// The dedicated 64DD EK disk-image archive. Same shape as the IPL step (own golden, own sidecar key,
// best-effort and non-gating). ensureDiskArchive runs after ensureIplArchive so a completed setup can
// delete the raw .ndd AND the managed copy once a boot proves byte-identity.
constexpr const char* kDiskArchiveName = "fzerox-disk.o2r";
constexpr const char* kDiskTempSubdir = ".gdx_disk.tmp";
// disk/image + disk/identity + the ek/<symbol> per-asset entries; == 2 when the EK slice manifest is
// absent at build/extract time.
constexpr int kDiskExpectedEntryCount = GDX_DISK_EXPECTED_ENTRY_COUNT;
// Shipped alongside the recipes so ensureDiskArchive can pass it to `gdx-extract disk -m`. Generated
// by tools/gen_ek_assets.py and copied into decomp-recipes/ at build time.
constexpr const char* kEkSliceManifestName = "ek_slice_manifest.txt";
constexpr const char* kManagedMediaSubdir = "media"; // managed-copy dir under the data dir
// Managed-copy-preferred order, mirroring port/disk_buffer.cpp's search. The managed copy always uses
// the translated leaf name (gdx_firstboot.cpp copies every source to it), so the .ndd the archive is
// built from resolves under one of these.
const char* const kDiskSourceNames[] = {
    "baserom.translated.ek.ndd",
    "baserom.jp.ek.ndd",
    "baserom.jp.disk",
};

#ifdef _WIN32
constexpr const char* kExtractBinaryName = "gdx-extract.exe";
#else
constexpr const char* kExtractBinaryName = "gdx-extract";
#endif

// Fallback ROM SHA-1s (the keys decomp/config.yml uses). config.yml is authoritative at runtime --
// recipes are the single source of truth -- and these are read only when it cannot be.
constexpr const char* kExpectedRomSha1Fallback = "5f658e88ffa9de23cba6986a8fd3d3a90d7b4340";
constexpr const char* kExpectedRomSha1FallbackJp = "a418b0151521b76691fa03f8658c8b567c69498b";

// The config.yml `path:` selectors that key each recipe tree (SHA1 -> recipe tree).
constexpr const char* kRecipePathUs = "assets/yaml/us/rev0";
constexpr const char* kRecipePathJp = "assets/yaml/jp/rev0";

// Version-entry contract: Torch stamps generic.o2r's game version = the US-rev0 ROM CRC.
constexpr std::uint32_t kExpectedRomCrc = 0x78D90EB3u;
// The JP-rev0 ROM CRC is unknown. 0 is passed through as the -u version arg as-is, and the resulting
// archive is treated as experimental.
constexpr std::uint32_t kExpectedRomCrcJp = 0u;

// Require >= 3x an estimated archive size before spawning. The exact archive size is not a
// compile-time constant here, hence the conservative upper estimate.
constexpr std::uintmax_t kEstimatedArchiveBytes = 128u * 1024u * 1024u; // ~128 MiB estimate.
constexpr std::uintmax_t kRequiredFreeBytes = 3u * kEstimatedArchiveBytes;

// ── Small helpers ────────────────────────────────────────────────────────────────────────────────

std::string toLowerHex(std::string s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'F') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return s;
}

FILE* openBinary(const fs::path& p, const char* mode) {
    FILE* f = nullptr;
#ifdef _MSC_VER
    if (fopen_s(&f, p.string().c_str(), mode) != 0) {
        return nullptr;
    }
#else
    f = std::fopen(p.string().c_str(), mode);
#endif
    return f;
}

bool fileExists(const fs::path& p) {
    std::error_code ec;
    return !p.empty() && fs::is_regular_file(p, ec);
}

// ── Async setup-GUI progress sink ────────────────────────────────────────────────────────────────
// When extraction runs from the in-window setup flow (GdxExtractStartAsync), the background worker
// publishes its latest stage line and last actionable error here for the ImGui screen to poll. The
// blocking pre-window path (DevLayout/SetupComplete) publishes into the same sink with nobody
// polling, which is harmless. Function-local static, so there is exactly one instance regardless of
// translation-unit init order.
constexpr size_t kAsyncLogCapacity = 200;

struct AsyncExtractState {
    std::mutex mtx;
    std::string stage;              // latest stage line (guarded by mtx)
    std::string lastError;          // last actionable error line (guarded by mtx)
    std::deque<std::string> log;    // ring buffer of the last kAsyncLogCapacity stdout lines (guarded by mtx)
    std::atomic<int> entriesSeen{0};  // "- [type] Processing <name>" lines seen in the current sub-stage
    std::atomic<int> subStage{0};     // 0 = cart, 1 = validating cart, 2 = ipl (see ExtractProgress::subStage)
    std::atomic<int> phase{0};      // 0 = Idle, 1 = Running, 2 = Done
    std::atomic<int> outcome{0};    // ExtractOutcome as int; valid only when phase == 2
    std::atomic<bool> suppressDialog{false}; // suppress the Windows marquee dialog when GUI-driven
    std::thread worker;
};

AsyncExtractState& asyncState() {
    static AsyncExtractState s;
    return s;
}

// Heuristic match for Torch's per-asset progress line, "- [type] Processing <name>[ at 0x...]",
// emitted once per YAML node visited (torch/src/Companion.cpp ParseNode). Not 1:1 with archive zip
// entries -- a node that finds no exporter still logs the line but writes no entry -- so the count
// approximates progress. It is monotonic and reaches the expected total on a successful run, which is
// all the progress bar needs.
bool looksLikeEntryProgressLine(const std::string& line) {
    return line.find("- [") != std::string::npos && line.find("] Processing ") != std::string::npos;
}

void gdxAsyncPublishStage(const std::string& line) {
    AsyncExtractState& s = asyncState();
    std::lock_guard<std::mutex> lk(s.mtx);
    s.stage = line;
    s.log.push_back(line);
    if (s.log.size() > kAsyncLogCapacity) {
        s.log.pop_front();
    }
    if (looksLikeEntryProgressLine(line)) {
        s.entriesSeen.fetch_add(1, std::memory_order_relaxed);
    }
}

void gdxAsyncPublishError(const std::string& msg) {
    AsyncExtractState& s = asyncState();
    std::lock_guard<std::mutex> lk(s.mtx);
    s.lastError = msg;
}

// Advances the coarse sub-stage shown by the setup GUI (see ExtractProgress::subStage). Called only
// from the async worker thread, never the pipe-reader thread, and always before that sub-stage's child
// is spawned and its reader started -- which is why the atomics alone suffice.
void gdxAsyncSetSubStage(int subStage, bool resetEntries) {
    AsyncExtractState& s = asyncState();
    s.subStage.store(subStage, std::memory_order_relaxed);
    if (resetEntries) {
        s.entriesSeen.store(0, std::memory_order_relaxed);
    }
}

// ── Vendored SHA-1 (public domain; Steve Reid's reference, condensed) ─────────────────────────────
// ROM identity only, not security. SHA-1 is the identifier the decomp recipes key on, so this matches
// Torch/config.yml exactly.

struct Sha1Ctx {
    std::uint32_t state[5];
    std::uint64_t count; // bits
    std::uint8_t buffer[64];
};

inline std::uint32_t rol32(std::uint32_t v, int b) {
    return (v << b) | (v >> (32 - b));
}

void sha1Transform(std::uint32_t state[5], const std::uint8_t buffer[64]) {
    std::uint32_t w[80];
    for (int i = 0; i < 16; ++i) {
        w[i] = (static_cast<std::uint32_t>(buffer[i * 4]) << 24) |
               (static_cast<std::uint32_t>(buffer[i * 4 + 1]) << 16) |
               (static_cast<std::uint32_t>(buffer[i * 4 + 2]) << 8) |
               (static_cast<std::uint32_t>(buffer[i * 4 + 3]));
    }
    for (int i = 16; i < 80; ++i) {
        w[i] = rol32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }
    std::uint32_t a = state[0], b = state[1], c = state[2], d = state[3], e = state[4];
    for (int i = 0; i < 80; ++i) {
        std::uint32_t f, k;
        if (i < 20) {
            f = (b & c) | ((~b) & d);
            k = 0x5A827999u;
        } else if (i < 40) {
            f = b ^ c ^ d;
            k = 0x6ED9EBA1u;
        } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d);
            k = 0x8F1BBCDCu;
        } else {
            f = b ^ c ^ d;
            k = 0xCA62C1D6u;
        }
        std::uint32_t tmp = rol32(a, 5) + f + e + k + w[i];
        e = d;
        d = c;
        c = rol32(b, 30);
        b = a;
        a = tmp;
    }
    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
}

void sha1Init(Sha1Ctx& ctx) {
    ctx.state[0] = 0x67452301u;
    ctx.state[1] = 0xEFCDAB89u;
    ctx.state[2] = 0x98BADCFEu;
    ctx.state[3] = 0x10325476u;
    ctx.state[4] = 0xC3D2E1F0u;
    ctx.count = 0;
}

void sha1Update(Sha1Ctx& ctx, const std::uint8_t* data, size_t len) {
    size_t idx = static_cast<size_t>((ctx.count >> 3) & 63u);
    ctx.count += static_cast<std::uint64_t>(len) << 3;
    size_t part = 64 - idx;
    size_t i = 0;
    if (len >= part) {
        std::memcpy(&ctx.buffer[idx], data, part);
        sha1Transform(ctx.state, ctx.buffer);
        for (i = part; i + 63 < len; i += 64) {
            sha1Transform(ctx.state, &data[i]);
        }
        idx = 0;
    }
    std::memcpy(&ctx.buffer[idx], &data[i], len - i);
}

void sha1Final(Sha1Ctx& ctx, std::uint8_t out[20]) {
    std::uint8_t finalCount[8];
    for (int i = 0; i < 8; ++i) {
        finalCount[i] = static_cast<std::uint8_t>((ctx.count >> ((7 - i) * 8)) & 0xFF);
    }
    std::uint8_t c = 0x80;
    sha1Update(ctx, &c, 1);
    c = 0x00;
    while ((ctx.count & 504u) != 448u) { // pad to 56 bytes mod 64 (in bits: 448 mod 512)
        sha1Update(ctx, &c, 1);
    }
    sha1Update(ctx, finalCount, 8);
    for (int i = 0; i < 20; ++i) {
        out[i] = static_cast<std::uint8_t>((ctx.state[i >> 2] >> ((3 - (i & 3)) * 8)) & 0xFF);
    }
}

// ── Vendored SHA-256 (public domain reference) ───────────────────────────────────────────────────
// The strongest install gate: archive SHA-256 == GDX_O2R_EXPECTED_SHA256.

struct Sha256Ctx {
    std::uint32_t state[8];
    std::uint64_t count; // bytes
    std::uint8_t buffer[64];
};

const std::uint32_t kSha256K[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u,
    0xab1c5ed5u, 0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu,
    0x9bdc06a7u, 0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu,
    0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
    0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u, 0xa2bfe8a1u, 0xa81a664bu,
    0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u,
    0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u,
    0xc67178f2u};

inline std::uint32_t ror32(std::uint32_t v, int b) {
    return (v >> b) | (v << (32 - b));
}

void sha256Transform(std::uint32_t state[8], const std::uint8_t block[64]) {
    std::uint32_t w[64];
    for (int i = 0; i < 16; ++i) {
        w[i] = (static_cast<std::uint32_t>(block[i * 4]) << 24) |
               (static_cast<std::uint32_t>(block[i * 4 + 1]) << 16) |
               (static_cast<std::uint32_t>(block[i * 4 + 2]) << 8) |
               (static_cast<std::uint32_t>(block[i * 4 + 3]));
    }
    for (int i = 16; i < 64; ++i) {
        std::uint32_t s0 = ror32(w[i - 15], 7) ^ ror32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        std::uint32_t s1 = ror32(w[i - 2], 17) ^ ror32(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    std::uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
    std::uint32_t e = state[4], f = state[5], g = state[6], h = state[7];
    for (int i = 0; i < 64; ++i) {
        std::uint32_t s1 = ror32(e, 6) ^ ror32(e, 11) ^ ror32(e, 25);
        std::uint32_t ch = (e & f) ^ ((~e) & g);
        std::uint32_t t1 = h + s1 + ch + kSha256K[i] + w[i];
        std::uint32_t s0 = ror32(a, 2) ^ ror32(a, 13) ^ ror32(a, 22);
        std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        std::uint32_t t2 = s0 + maj;
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }
    state[0] += a;
    state[1] += b;
    state[2] += c;
    state[3] += d;
    state[4] += e;
    state[5] += f;
    state[6] += g;
    state[7] += h;
}

void sha256Init(Sha256Ctx& ctx) {
    ctx.state[0] = 0x6a09e667u;
    ctx.state[1] = 0xbb67ae85u;
    ctx.state[2] = 0x3c6ef372u;
    ctx.state[3] = 0xa54ff53au;
    ctx.state[4] = 0x510e527fu;
    ctx.state[5] = 0x9b05688cu;
    ctx.state[6] = 0x1f83d9abu;
    ctx.state[7] = 0x5be0cd19u;
    ctx.count = 0;
}

void sha256Update(Sha256Ctx& ctx, const std::uint8_t* data, size_t len) {
    size_t idx = static_cast<size_t>(ctx.count & 63u);
    ctx.count += len;
    size_t i = 0;
    if (idx > 0) {
        size_t part = 64 - idx;
        if (len < part) {
            std::memcpy(&ctx.buffer[idx], data, len);
            return;
        }
        std::memcpy(&ctx.buffer[idx], data, part);
        sha256Transform(ctx.state, ctx.buffer);
        i = part;
    }
    for (; i + 63 < len; i += 64) {
        sha256Transform(ctx.state, &data[i]);
    }
    std::memcpy(ctx.buffer, &data[i], len - i);
}

void sha256Final(Sha256Ctx& ctx, std::uint8_t out[32]) {
    std::uint64_t bits = ctx.count << 3;
    std::uint8_t c = 0x80;
    sha256Update(ctx, &c, 1);
    c = 0x00;
    while ((ctx.count & 63u) != 56u) {
        sha256Update(ctx, &c, 1);
    }
    std::uint8_t lenBytes[8];
    for (int i = 0; i < 8; ++i) {
        lenBytes[i] = static_cast<std::uint8_t>((bits >> ((7 - i) * 8)) & 0xFF);
    }
    sha256Update(ctx, lenBytes, 8);
    for (int i = 0; i < 32; ++i) {
        out[i] = static_cast<std::uint8_t>((ctx.state[i >> 2] >> ((3 - (i & 3)) * 8)) & 0xFF);
    }
}

std::string toHex(const std::uint8_t* bytes, size_t n) {
    static const char* d = "0123456789abcdef";
    std::string out;
    out.reserve(n * 2);
    for (size_t i = 0; i < n; ++i) {
        out.push_back(d[bytes[i] >> 4]);
        out.push_back(d[bytes[i] & 0xF]);
    }
    return out;
}

// Streamed file hashing (avoids loading multi-MiB files into RAM). Returns empty on read error.
std::string sha1File(const fs::path& p) {
    FILE* f = openBinary(p, "rb");
    if (f == nullptr) {
        return {};
    }
    Sha1Ctx ctx;
    sha1Init(ctx);
    std::vector<std::uint8_t> buf(1u << 16);
    for (;;) {
        size_t got = std::fread(buf.data(), 1, buf.size(), f);
        if (got > 0) {
            sha1Update(ctx, buf.data(), got);
        }
        if (got < buf.size()) {
            break;
        }
    }
    bool err = (std::ferror(f) != 0);
    std::fclose(f);
    if (err) {
        return {};
    }
    std::uint8_t out[20];
    sha1Final(ctx, out);
    return toHex(out, 20);
}

std::string sha256File(const fs::path& p) {
    FILE* f = openBinary(p, "rb");
    if (f == nullptr) {
        return {};
    }
    Sha256Ctx ctx;
    sha256Init(ctx);
    std::vector<std::uint8_t> buf(1u << 16);
    for (;;) {
        size_t got = std::fread(buf.data(), 1, buf.size(), f);
        if (got > 0) {
            sha256Update(ctx, buf.data(), got);
        }
        if (got < buf.size()) {
            break;
        }
    }
    bool err = (std::ferror(f) != 0);
    std::fclose(f);
    if (err) {
        return {};
    }
    std::uint8_t out[32];
    sha256Final(ctx, out);
    return toHex(out, 32);
}

// ── config.yml expected-ROM-SHA-1 parse (recipes are the single source of truth) ──────────────────
// The recipe config keys each recipe tree on the ROM SHA-1; the one wanted here is the key whose block
// declares `path: <recipePath>`, which is what selects US vs JP. Lowercase hex, or empty when not
// found or unreadable. The default recipePath keeps existing US callers working unchanged.
std::string expectedRomSha1FromConfig(const fs::path& configYml,
                                      const std::string& recipePath = kRecipePathUs) {
    FILE* f = openBinary(configYml, "rb");
    if (f == nullptr) {
        return {};
    }
    std::string currentKey;
    std::string result;
    char line[4096];
    auto trim = [](std::string s) {
        size_t b = s.find_first_not_of(" \t\r\n");
        size_t e = s.find_last_not_of(" \t\r\n");
        return (b == std::string::npos) ? std::string() : s.substr(b, e - b + 1);
    };
    while (std::fgets(line, sizeof(line), f) != nullptr) {
        std::string raw(line);
        // A top-level key starts at column 0 and ends in ':'. In this file that key is the SHA-1.
        if (!raw.empty() && raw[0] != ' ' && raw[0] != '\t' && raw[0] != '#') {
            std::string t = trim(raw);
            size_t colon = t.find(':');
            if (colon != std::string::npos) {
                currentKey = toLowerHex(trim(t.substr(0, colon)));
            }
            continue;
        }
        std::string t = trim(raw);
        if (t.rfind("path:", 0) == 0) {
            std::string val = trim(t.substr(5));
            if (val == recipePath && currentKey.size() == 40) {
                result = currentKey;
                break;
            }
        }
    }
    std::fclose(f);
    return result;
}

// One per supported ROM dump. The gate hashes the ROM once and matches it against every profile's
// expectedSha1 to pick recipe tree + output archive name + golden. `experimental` marks a profile with
// no real golden yet (JP today), and the install then skips the SHA-256 / entry-count gates.
struct RomProfile {
    const char* key;              // "us/rev0" / "jp/rev0"
    const char* recipePath;       // config.yml `path:` selector
    const char* archiveName;      // fzerox.o2r / fzerox-jp.o2r
    std::string expectedSha1;     // ROM SHA-1 (config.yml, else fallback constant)
    std::string goldenSha256;     // lowercase; all-zeros == no golden (experimental)
    long goldenEntryCount;        // 0 == unknown (experimental)
    std::uint32_t versionCrc;     // -u arg (0 == unknown)
    bool experimental;            // true when goldenSha256 is the placeholder
};

bool isPlaceholderSha256(const std::string& s) {
    if (s.empty()) return true;
    for (char c : s) {
        if (c != '0') return false;
    }
    return true;
}

// Build the US + JP profiles, resolving each ROM SHA-1 from config.yml (fallback to the constants).
std::vector<RomProfile> buildRomProfiles(const fs::path& configYml) {
    auto resolveSha1 = [&](const char* recipePath, const char* fallback) {
        std::string s = expectedRomSha1FromConfig(configYml, recipePath);
        return toLowerHex(s.empty() ? std::string(fallback) : s);
    };
    std::vector<RomProfile> out;
    {
        RomProfile us;
        us.key = "us/rev0";
        us.recipePath = kRecipePathUs;
        us.archiveName = kArchiveName;
        us.expectedSha1 = resolveSha1(kRecipePathUs, kExpectedRomSha1Fallback);
        us.goldenSha256 = toLowerHex(std::string(GDX_O2R_EXPECTED_SHA256));
        us.goldenEntryCount = static_cast<long>(GDX_O2R_EXPECTED_ENTRY_COUNT);
        us.versionCrc = kExpectedRomCrc;
        us.experimental = isPlaceholderSha256(us.goldenSha256);
        out.push_back(us);
    }
    {
        RomProfile jp;
        jp.key = "jp/rev0";
        jp.recipePath = kRecipePathJp;
        jp.archiveName = kArchiveNameJp;
        jp.expectedSha1 = resolveSha1(kRecipePathJp, kExpectedRomSha1FallbackJp);
        jp.goldenSha256 = toLowerHex(std::string(GDX_O2R_EXPECTED_SHA256_JP));
        jp.goldenEntryCount = static_cast<long>(GDX_O2R_EXPECTED_ENTRY_COUNT_JP);
        jp.versionCrc = kExpectedRomCrcJp;
        jp.experimental = isPlaceholderSha256(jp.goldenSha256);
        out.push_back(jp);
    }
    return out;
}

// ── Zip End-Of-Central-Directory entry count ─────────────────────────────────────────────────────
// Scans backwards for the EOCD signature (0x06054b50) and reads "total number of central directory
// records". This is the RAW record count, NOT a unique-name set: the archive keys duplicate names on
// purpose (course_track_gfx/* alone contributes 664 duplicate records over 3,576 unique names, 4,240
// records total), so counting names would report 3,576 and fail a correct archive. 4,240 is well below
// the u16 limit, so no ZIP64 EOCD is expected; a classic field reading 0xFFFF returns -1 (unknown) and
// lets the SHA-256 gate decide.
long zipEntryCount(const fs::path& p) {
    FILE* f = openBinary(p, "rb");
    if (f == nullptr) {
        return -1;
    }
    std::fseek(f, 0, SEEK_END);
    long size = std::ftell(f);
    if (size < 22) { // minimum EOCD size
        std::fclose(f);
        return -1;
    }
    // The EOCD comment can be up to 65535 bytes; scan the last 64 KiB + 22.
    long scan = size < (65535 + 22) ? size : (65535 + 22);
    std::fseek(f, size - scan, SEEK_SET);
    std::vector<std::uint8_t> buf(static_cast<size_t>(scan));
    size_t got = std::fread(buf.data(), 1, buf.size(), f);
    std::fclose(f);
    if (got != buf.size()) {
        return -1;
    }
    for (long i = static_cast<long>(got) - 22; i >= 0; --i) {
        if (buf[i] == 0x50 && buf[i + 1] == 0x4b && buf[i + 2] == 0x05 && buf[i + 3] == 0x06) {
            // EOCD offset 10 (u16) is the total across all disks; offset 8 is the per-disk count and
            // offset 12 is the CD *size*, not a count.
            std::uint16_t total = static_cast<std::uint16_t>(buf[i + 10] | (buf[i + 11] << 8));
            if (total == 0xFFFF) {
                return -1; // ZIP64 sentinel — not expected for this archive.
            }
            return static_cast<long>(total);
        }
    }
    return -1;
}

// ── Completion sidecar: gdx_extract_state.cfg — key=value, same pattern as gdx_firstboot.cfg ─────

struct ExtractState {
    std::string extractorVersion;
    std::string recipeFingerprint;
    std::string romSha1;
    std::string archiveSha256;
    std::uintmax_t romSize = 0;
    std::int64_t romMtime = 0;
    // Managed Expansion Kit disk copy identity (GdxExtractRecordManagedDisk). Diagnostic only --
    // nothing in this file gates boot behavior on these fields.
    std::string diskSha256;
    std::uintmax_t diskSize = 0;
    // SHA-256 of the fzerox-disk.o2r container, authored by ensureDiskArchive. Drives the disk step's
    // warm-boot check; self-consistency only, never gating.
    std::string diskArchiveSha256;
    // iplSha256 is the SHA-256 of the NORMALIZED full IPL (the archive's ipl/identity, authored by the
    // gdx-extract `ipl` step); iplArchiveSha256 is n64ddipl.o2r itself. Self-consistency only.
    std::string iplSha256;
    std::string iplArchiveSha256;
    // Which ROM profile this data-dir's cartridge archive was extracted for ("us/rev0" / "jp/rev0"),
    // so the shared launcher/wizard can confirm it matches the running binary and offer to launch the
    // matching one on a mismatch.
    std::string profile;
    bool valid = false; // true only if the file was read and parsed
};

ExtractState loadSidecar(const fs::path& dataDir) {
    ExtractState st;
    FILE* f = openBinary(dataDir / kSidecarName, "rb");
    if (f == nullptr) {
        return st;
    }
    char line[4096];
    while (std::fgets(line, sizeof(line), f) != nullptr) {
        std::string s(line);
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) {
            s.pop_back();
        }
        if (s.empty() || s[0] == '#') {
            continue;
        }
        size_t eq = s.find('=');
        if (eq == std::string::npos) {
            continue;
        }
        std::string k = s.substr(0, eq);
        std::string v = s.substr(eq + 1);
        if (k == "extractor_version") {
            st.extractorVersion = v;
        } else if (k == "recipe_fingerprint") {
            st.recipeFingerprint = v;
        } else if (k == "rom_sha1") {
            st.romSha1 = toLowerHex(v);
        } else if (k == "archive_sha256") {
            st.archiveSha256 = toLowerHex(v);
        } else if (k == "rom_size") {
            st.romSize = std::strtoull(v.c_str(), nullptr, 10);
        } else if (k == "rom_mtime") {
            st.romMtime = static_cast<std::int64_t>(std::strtoll(v.c_str(), nullptr, 10));
        } else if (k == "disk_sha256") {
            st.diskSha256 = toLowerHex(v);
        } else if (k == "disk_size") {
            st.diskSize = std::strtoull(v.c_str(), nullptr, 10);
        } else if (k == "disk_archive_sha256") {
            st.diskArchiveSha256 = toLowerHex(v);
        } else if (k == "ipl_sha256") {
            st.iplSha256 = toLowerHex(v);
        } else if (k == "ipl_archive_sha256") {
            st.iplArchiveSha256 = toLowerHex(v);
        } else if (k == "profile") {
            st.profile = v;
        }
    }
    std::fclose(f);
    st.valid = true;
    return st;
}

bool saveSidecar(const fs::path& dataDir, const ExtractState& st) {
    FILE* f = openBinary(dataDir / kSidecarName, "wb");
    if (f == nullptr) {
        gdx_port_logf("[extract] WARNING: could not write %s; warm-boot cache will be rebuilt next run\n",
                      (dataDir / kSidecarName).string().c_str());
        return false;
    }
    std::fprintf(f, "# G-Diffuser O2R extraction state. Auto-generated; safe to delete to force re-extract.\n");
    std::fprintf(f, "extractor_version=%s\n", st.extractorVersion.c_str());
    std::fprintf(f, "recipe_fingerprint=%s\n", st.recipeFingerprint.c_str());
    std::fprintf(f, "rom_sha1=%s\n", st.romSha1.c_str());
    std::fprintf(f, "archive_sha256=%s\n", st.archiveSha256.c_str());
    std::fprintf(f, "rom_size=%llu\n", static_cast<unsigned long long>(st.romSize));
    std::fprintf(f, "rom_mtime=%lld\n", static_cast<long long>(st.romMtime));
    std::fprintf(f, "disk_sha256=%s\n", st.diskSha256.c_str());
    std::fprintf(f, "disk_size=%llu\n", static_cast<unsigned long long>(st.diskSize));
    std::fprintf(f, "disk_archive_sha256=%s\n", st.diskArchiveSha256.c_str());
    std::fprintf(f, "ipl_sha256=%s\n", st.iplSha256.c_str());
    std::fprintf(f, "ipl_archive_sha256=%s\n", st.iplArchiveSha256.c_str());
    std::fprintf(f, "profile=%s\n", st.profile.c_str());
    std::fclose(f);
    return true;
}

std::int64_t fileMtime(const fs::path& p) {
    std::error_code ec;
    auto t = fs::last_write_time(p, ec);
    if (ec) {
        return 0;
    }
    return static_cast<std::int64_t>(t.time_since_epoch().count());
}

// ── Atomic install: rename temp -> final, with a Windows sharing-violation retry loop ─────────────
bool atomicReplace(const fs::path& tmp, const fs::path& final) {
#ifdef _WIN32
    std::wstring wt = tmp.wstring();
    std::wstring wf = final.wstring();
    const int kMaxRetries = 12;
    for (int attempt = 0; attempt < kMaxRetries; ++attempt) {
        if (MoveFileExW(wt.c_str(), wf.c_str(),
                        MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED | MOVEFILE_WRITE_THROUGH)) {
            return true;
        }
        DWORD err = GetLastError();
        if (err == ERROR_SHARING_VIOLATION || err == ERROR_ACCESS_DENIED || err == ERROR_LOCK_VIOLATION) {
            // Classic on-access AV lock on a freshly written archive in %APPDATA%. Back off and retry.
            gdx_port_logf("[extract] install retry %d/%d (Windows error %lu; antivirus may be scanning "
                          "the new archive)\n",
                          attempt + 1, kMaxRetries, static_cast<unsigned long>(err));
            Sleep(250 * (attempt + 1));
            continue;
        }
        gdx_port_logf("[extract] ERROR: could not install archive (Windows error %lu)\n",
                      static_cast<unsigned long>(err));
        return false;
    }
    gdx_port_logf("[extract] ERROR: install kept failing with a sharing violation. Your antivirus may "
                  "be blocking G-Diffuser from writing %s.\n",
                  final.string().c_str());
    return false;
#else
    std::error_code ec;
    fs::rename(tmp, final, ec); // atomic within the same filesystem
    if (!ec) {
        return true;
    }
    gdx_port_logf("[extract] ERROR: could not install archive: %s\n", ec.message().c_str());
    return false;
#endif
}

// ── Best-effort parse of extractor stdout for diagnostic sidecar fields ──────────────────────────
// The exact wording belongs to the extractor, so this scans case-insensitively for "fingerprint" /
// "extractor" tokens and takes the trailing value. The golden SHA-256 is the gating authority, so an
// unparsed line never affects correctness.
void scanStdoutLine(const std::string& line, ExtractState& st) {
    auto lower = [](std::string s) {
        for (char& c : s) {
            if (c >= 'A' && c <= 'Z') {
                c = static_cast<char>(c - 'A' + 'a');
            }
        }
        return s;
    };
    std::string lc = lower(line);
    auto tailToken = [&](const std::string& s) {
        size_t e = s.find_last_not_of(" \t\r\n");
        if (e == std::string::npos) {
            return std::string();
        }
        size_t b = s.find_last_of(" \t=:", e);
        return s.substr(b == std::string::npos ? 0 : b + 1, e - (b == std::string::npos ? 0 : b));
    };
    if (st.recipeFingerprint.empty() && lc.find("fingerprint") != std::string::npos) {
        st.recipeFingerprint = tailToken(line);
    } else if (st.iplSha256.empty() && lc.find("ipl identity sha256") != std::string::npos) {
        // "gdx-extract ipl: ipl identity sha256 <64-hex> (fmt N)". Grab the first 64-char hex run.
        auto isHex = [](char c) {
            return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
        };
        for (size_t i = 0; i + 64 <= lc.size(); ++i) {
            size_t j = 0;
            while (j < 64 && isHex(lc[i + j])) {
                ++j;
            }
            if (j == 64 && (i + 64 == lc.size() || !isHex(lc[i + 64]))) {
                st.iplSha256 = toLowerHex(lc.substr(i, 64));
                break;
            }
        }
    } else if (st.extractorVersion.empty() && lc.find("gdx-extract") != std::string::npos &&
               lc.find("version") != std::string::npos) {
        st.extractorVersion = tailToken(line);
    }
}

// ── Child-process launch ─────────────────────────────────────────────────────────────────────────
// Streams gdx-extract's stdout to the log on every platform, and on Windows to a modeless marquee
// progress dialog. True when the process spawned and exited 0; fills exitCode + state.

#ifdef _WIN32

// Latest stage line, reader-thread -> UI-thread. A status string does not need PostMessage
// marshalling, so the refresh just reads it.
struct ProgressShared {
    std::mutex mtx;
    std::string stage;
};

LRESULT CALLBACK progressWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

bool runExtractorWindows(const fs::path& exe, const std::wstring& cmdLine, const fs::path& workDir,
                         int& exitCode, ExtractState& state) {
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;
    sa.lpSecurityDescriptor = nullptr;

    HANDLE readPipe = nullptr;
    HANDLE writePipe = nullptr;
    if (!CreatePipe(&readPipe, &writePipe, &sa, 0)) {
        gdx_port_logf("[extract] ERROR: CreatePipe failed (%lu)\n",
                      static_cast<unsigned long>(GetLastError()));
        return false;
    }
    // The read end must NOT be inherited by the child.
    SetHandleInformation(readPipe, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = writePipe;
    si.hStdError = writePipe;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

    PROCESS_INFORMATION pi{};
    std::wstring mutableCmd = cmdLine; // CreateProcessW may modify the buffer
    std::wstring wWorkDir = workDir.wstring();

    BOOL ok = CreateProcessW(exe.wstring().c_str(), mutableCmd.data(), nullptr, nullptr, TRUE,
                             CREATE_NO_WINDOW, nullptr, wWorkDir.c_str(), &si, &pi);
    CloseHandle(writePipe); // parent keeps only the read end
    if (!ok) {
        gdx_port_logf("[extract] ERROR: could not launch %s (Windows error %lu)\n",
                      exe.string().c_str(), static_cast<unsigned long>(GetLastError()));
        CloseHandle(readPipe);
        return false;
    }

    // ── Modeless marquee progress dialog ─────────────────────────────────────────────────────────
    // Suppressed when the in-window setup screen drives the extraction: that ImGui screen already
    // shows the stage line and an animated indicator, and a second native dialog would steal focus
    // from the game window. The message loop below still runs either way.
    const bool showDialog = !asyncState().suppressDialog.load();
    HWND wnd = nullptr;
    HWND text = nullptr;
    HWND bar = nullptr;
    if (showDialog) {
    INITCOMMONCONTROLSEX icc{};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_PROGRESS_CLASS;
    InitCommonControlsEx(&icc);

    static const wchar_t* kClass = L"GdxExtractProgress";
    HINSTANCE hInst = GetModuleHandleW(nullptr);
    WNDCLASSW wc{};
    wc.lpfnWndProc = progressWndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = kClass;
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)); // IDC_ARROW; explicit W form (build is not UNICODE)
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    RegisterClassW(&wc); // ignore "already registered" on a second run

    const int w = 460;
    const int h = 140;
    int sx = (GetSystemMetrics(SM_CXSCREEN) - w) / 2;
    int sy = (GetSystemMetrics(SM_CYSCREEN) - h) / 2;
    wnd = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW, kClass,
                          L"G-Diffuser — preparing game assets",
                          WS_POPUPWINDOW | WS_CAPTION, sx, sy, w, h, nullptr, nullptr, hInst, nullptr);
    if (wnd != nullptr) {
        text = CreateWindowExW(0, L"STATIC",
                               L"Extracting game assets from your ROM.\nThis happens once and may take "
                               L"a minute…",
                               WS_CHILD | WS_VISIBLE, 16, 12, w - 40, 48, wnd, nullptr, hInst, nullptr);
        bar = CreateWindowExW(0, PROGRESS_CLASSW, nullptr,
                              WS_CHILD | WS_VISIBLE | PBS_MARQUEE, 16, 68, w - 40, 22, wnd, nullptr,
                              hInst, nullptr);
        if (bar != nullptr) {
            SendMessageW(bar, PBM_SETMARQUEE, TRUE, 60);
        }
        ShowWindow(wnd, SW_SHOWNORMAL);
        UpdateWindow(wnd);
    }
    } // if (showDialog)

    ProgressShared shared;
    std::atomic<bool> readerDone{false};
    std::thread reader([&]() {
        std::string acc;
        char buf[4096];
        DWORD n = 0;
        for (;;) {
            if (!ReadFile(readPipe, buf, sizeof(buf), &n, nullptr) || n == 0) {
                break;
            }
            acc.append(buf, n);
            size_t nl;
            while ((nl = acc.find('\n')) != std::string::npos) {
                std::string line = acc.substr(0, nl);
                acc.erase(0, nl + 1);
                while (!line.empty() && (line.back() == '\r')) {
                    line.pop_back();
                }
                if (!line.empty()) {
                    gdx_port_logf("[extract] %s\n", line.c_str());
                    scanStdoutLine(line, state);
                    gdxAsyncPublishStage(line); // feed the in-window setup screen (no-op when unused)
                    std::lock_guard<std::mutex> lk(shared.mtx);
                    shared.stage = line;
                }
            }
        }
        if (!acc.empty()) {
            gdx_port_logf("[extract] %s\n", acc.c_str());
            scanStdoutLine(acc, state);
        }
        readerDone.store(true);
    });

    // Pump the message loop, refreshing the status line, until the child exits. Boot must NEVER block,
    // so a hung (as opposed to failed) extractor is killed after a hard deadline and treated as a
    // failed extraction. Normal extraction takes ~2 seconds; 120s is generously beyond any slow disk.
    const ULONGLONG deadline = GetTickCount64() + 120u * 1000u;
    bool timedOut = false;
    for (;;) {
        DWORD wait = WaitForSingleObject(pi.hProcess, 50);
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (text != nullptr) {
            std::string stage;
            {
                std::lock_guard<std::mutex> lk(shared.mtx);
                stage = shared.stage;
            }
            if (!stage.empty()) {
                std::wstring ws(stage.begin(), stage.end());
                SetWindowTextW(text, ws.c_str());
            }
        }
        if (wait == WAIT_OBJECT_0) {
            break;
        }
        if (GetTickCount64() >= deadline) {
            gdx_port_logf("[extract] ERROR: extractor exceeded the %us deadline; terminating child\n", 120u);
            TerminateProcess(pi.hProcess, 124u);
            WaitForSingleObject(pi.hProcess, 5000);
            timedOut = true;
            break;
        }
    }

    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    exitCode = timedOut ? 124 : static_cast<int>(code);

    if (reader.joinable()) {
        reader.join();
    }
    CloseHandle(readPipe);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    if (wnd != nullptr) {
        DestroyWindow(wnd);
    }
    return exitCode == 0;
}

#else // POSIX

bool runExtractorPosix(const fs::path& exe, const std::vector<std::string>& args, const fs::path& workDir,
                       int& exitCode, ExtractState& state) {
    int pipefd[2];
    if (pipe(pipefd) != 0) {
        gdx_port_logf("[extract] ERROR: pipe() failed: %s\n", std::strerror(errno));
        return false;
    }
    pid_t pid = fork();
    if (pid < 0) {
        gdx_port_logf("[extract] ERROR: fork() failed: %s\n", std::strerror(errno));
        close(pipefd[0]);
        close(pipefd[1]);
        return false;
    }
    if (pid == 0) {
        // Child.
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);
        if (!workDir.empty()) {
            if (chdir(workDir.string().c_str()) != 0) {
                const int launchError = errno;
                dprintf(STDERR_FILENO, "[launcher] chdir(\"%s\") failed: %s (errno=%d)\n",
                        workDir.string().c_str(), std::strerror(launchError), launchError);
                _exit(127);
            }
        }
        std::vector<char*> argv;
        std::string exeStr = exe.string();
        argv.push_back(const_cast<char*>(exeStr.c_str()));
        for (const auto& a : args) {
            argv.push_back(const_cast<char*>(a.c_str()));
        }
        argv.push_back(nullptr);
        execv(exeStr.c_str(), argv.data());
        const int launchError = errno;
        dprintf(STDERR_FILENO, "[launcher] execv(\"%s\") failed: %s (errno=%d)\n", exeStr.c_str(),
                std::strerror(launchError), launchError);
        _exit(127);
    }
    // Parent: stream the child's stdout to the log (the Linux progress UX is log-only). The read is
    // bounded by a hard deadline via select(), so a child that stops producing output and never exits
    // is killed and treated as a failed extraction rather than hanging boot.
    close(pipefd[1]);
    std::string acc;
    char buf[4096];
    const time_t deadline = time(nullptr) + 120;
    bool timedOut = false;
    for (;;) {
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(pipefd[0], &rfds);
        struct timeval tv = { 1, 0 };
        int sel = select(pipefd[0] + 1, &rfds, nullptr, nullptr, &tv);
        if (sel < 0 && errno == EINTR) {
            continue;
        }
        if (sel > 0) {
            ssize_t n = read(pipefd[0], buf, sizeof(buf));
            if (n <= 0) {
                break; // EOF (child closed stdout) or read error
            }
            acc.append(buf, static_cast<size_t>(n));
            size_t nl;
            while ((nl = acc.find('\n')) != std::string::npos) {
                std::string line = acc.substr(0, nl);
                acc.erase(0, nl + 1);
                if (!line.empty()) {
                    gdx_port_logf("[extract] %s\n", line.c_str());
                    scanStdoutLine(line, state);
                    gdxAsyncPublishStage(line); // feed the in-window setup screen (no-op when unused)
                }
            }
        }
        if (time(nullptr) >= deadline) {
            gdx_port_logf("[extract] ERROR: extractor exceeded the 120s deadline; killing child\n");
            kill(pid, SIGKILL);
            timedOut = true;
            break;
        }
    }
    if (!acc.empty()) {
        gdx_port_logf("[extract] %s\n", acc.c_str());
        scanStdoutLine(acc, state);
    }
    close(pipefd[0]);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
    }
    if (timedOut) {
        exitCode = 124;
    } else if (WIFEXITED(status)) {
        exitCode = WEXITSTATUS(status);
    } else {
        exitCode = 1;
    }
    return exitCode == 0;
}

#endif

// ── Cleanup helpers ──────────────────────────────────────────────────────────────────────────────
void removeIfExists(const fs::path& p) {
    std::error_code ec;
    fs::remove_all(p, ec);
}

// ── Extraction orchestration ─────────────────────────────────────────────────────────────────────
// Runs the extractor into a same-filesystem temp dir, validates the output, and atomically installs
// it. Extracted on full success, FailedRawFallback otherwise. Never throws.
ExtractOutcome runExtraction(const fs::path& dataDir, const fs::path& romPath, const fs::path& exeDir,
                             const std::string& romSha1, const RomProfile& profile) {
    std::error_code ec;
    const char* archiveName = profile.archiveName; // fzerox.o2r (US) / fzerox-jp.o2r (JP)

    const fs::path extractBin = exeDir / kExtractBinaryName;
    const fs::path recipesDir = exeDir / kRecipesDirName;
    if (!fileExists(extractBin)) {
        gdx_port_logf("[extract] extractor component missing (%s). Cannot build %s — booting from the "
                      "raw ROM. Reinstall G-Diffuser to restore the extractor.\n",
                      extractBin.string().c_str(), archiveName);
        gdxAsyncPublishError("The extractor component (gdx-extract) is missing. Reinstall G-Diffuser.");
        return ExtractOutcome::FailedRawFallback;
    }
    if (!fs::is_directory(recipesDir, ec)) {
        ec.clear();
        gdx_port_logf("[extract] recipe data missing (%s). Cannot build %s — booting from the raw ROM.\n",
                      recipesDir.string().c_str(), archiveName);
        gdxAsyncPublishError("The recipe data (decomp-recipes) is missing. Reinstall G-Diffuser.");
        return ExtractOutcome::FailedRawFallback;
    }

    // Require >= 3x the estimated archive size.
    auto space = fs::space(dataDir, ec);
    if (!ec && space.available < kRequiredFreeBytes) {
        gdx_port_logf("[extract] not enough free disk space in %s: %.0f MB available, ~%.0f MB needed. "
                      "Free some space and relaunch. Booting from the raw ROM for now.\n",
                      dataDir.string().c_str(),
                      static_cast<double>(space.available) / (1024.0 * 1024.0),
                      static_cast<double>(kRequiredFreeBytes) / (1024.0 * 1024.0));
        gdxAsyncPublishError("Not enough free disk space to build the asset archive. Free some space "
                             "and retry.");
        return ExtractOutcome::FailedRawFallback;
    }
    ec.clear();

    // Fresh temp staging dir on the same filesystem as the final archive (so the install is a rename).
    const fs::path tmpDir = dataDir / kTempSubdir;
    removeIfExists(tmpDir);
    fs::create_directories(tmpDir, ec);
    if (ec) {
        gdx_port_logf("[extract] ERROR: could not create temp dir %s: %s. Booting from the raw ROM.\n",
                      tmpDir.string().c_str(), ec.message().c_str());
        return ExtractOutcome::FailedRawFallback;
    }
    ec.clear();

    // MAX_PATH guard (Windows; harmless elsewhere). The deepest recipe path appends
    // "/assets/yaml/us/rev0/<longest yaml name>" (~54 chars) to the recipes dir, and the classic
    // Windows limit is 260 including the terminator, so a deep install directory pushes the
    // extractor's plain ifstream opens past it -- observed failing at exactly 260 chars. When the
    // projected path is too long, stage a copy of the small (~455 KB) recipe tree under the system
    // temp dir, which is short on real systems, and hand THAT to the extractor instead.
    fs::path effectiveRecipes = recipesDir;
    fs::path stagedRecipes;
    constexpr size_t kDeepestRecipeSuffix = 54; // "/assets/yaml/us/rev0/expansion_kit_textures_beta.yaml"
    if (recipesDir.string().size() + kDeepestRecipeSuffix >= 248) {
        std::error_code sec;
        fs::path shortBase = fs::temp_directory_path(sec);
        if (!sec && shortBase.string().size() + 16 + kDeepestRecipeSuffix < 248) {
            stagedRecipes = shortBase / "gdx-recipes";
            removeIfExists(stagedRecipes);
#ifdef _WIN32
            // The staging copy must itself read >=260-char source paths, and std::filesystem does not
            // add the \\?\ long-path prefix on Windows, so the source gets it explicitly.
            fs::path copySrc = fs::absolute(recipesDir, sec);
            if (!sec) {
                copySrc = fs::path(L"\\\\?\\" + copySrc.wstring());
            } else {
                sec.clear();
                copySrc = recipesDir;
            }
#else
            const fs::path& copySrc = recipesDir;
#endif
            fs::copy(copySrc, stagedRecipes, fs::copy_options::recursive, sec);
            if (!sec) {
                effectiveRecipes = stagedRecipes;
                gdx_port_logf("[extract] recipes path is too deep for MAX_PATH; staged a copy at %s\n",
                              stagedRecipes.string().c_str());
            } else {
                removeIfExists(stagedRecipes);
                stagedRecipes.clear();
                gdx_port_logf("[extract] WARNING: recipes path may exceed MAX_PATH and staging failed "
                              "(%s); attempting extraction in place.\n",
                              sec.message().c_str());
            }
        } else {
            gdx_port_logf("[extract] WARNING: install path is extremely deep; extraction may fail on "
                          "MAX_PATH. Consider moving G-Diffuser to a shorter folder.\n");
        }
    }

    // gdx-extract o2r <rom.z64> -s <recipesDir> -d <tmpDir> -u <version>. NEVER pass -v: its debug
    // mode dumps entries into the CWD. Torch derives the CRC itself; -u only carries the numeric game
    // version, which for the US profile is the US-rev0 ROM CRC.
    char versionArg[16];
    std::snprintf(versionArg, sizeof(versionArg), "%u", static_cast<unsigned>(profile.versionCrc));

    gdx_port_logf("[extract] building %s [%s] from the ROM (one-time). recipes=%s out=%s\n", archiveName,
                  profile.key, recipesDir.string().c_str(), tmpDir.string().c_str());

    ExtractState state;
    int exitCode = 1;
    bool ok = false;
#ifdef _WIN32
    // Build a single command line; quote paths that may contain spaces.
    auto q = [](const std::wstring& s) { return L"\"" + s + L"\""; };
    std::wstring cmd = q(extractBin.wstring());
    cmd += L" o2r ";
    cmd += q(romPath.wstring());
    cmd += L" -s ";
    cmd += q(effectiveRecipes.wstring());
    cmd += L" -d ";
    cmd += q(tmpDir.wstring());
    cmd += L" -u ";
    cmd += std::wstring(versionArg, versionArg + std::strlen(versionArg));
    ok = runExtractorWindows(extractBin, cmd, tmpDir, exitCode, state);
#else
    std::vector<std::string> args = { "o2r",
                                      romPath.string(),
                                      "-s",
                                      effectiveRecipes.string(),
                                      "-d",
                                      tmpDir.string(),
                                      "-u",
                                      versionArg };
    ok = runExtractorPosix(extractBin, args, tmpDir, exitCode, state);
#endif

    // The extractor may drop a stray torch.hash.yml in the output dir and in the CWD; the MAX_PATH
    // staging copy of the recipes also has to go.
    removeIfExists(tmpDir / kTorchHashName);
    removeIfExists(dataDir / kTorchHashName);
    if (!stagedRecipes.empty()) {
        removeIfExists(stagedRecipes);
    }

    if (!ok) {
        gdx_port_logf("[extract] ERROR: extractor exited with code %d. Keeping any previous %s; booting "
                      "from the raw ROM.\n",
                      exitCode, archiveName);
        gdxAsyncPublishError(exitCode == 124
                                 ? std::string("The extractor timed out (exceeded the 120s deadline).")
                                 : ("The extractor failed (exit code " + std::to_string(exitCode) + ")."));
        removeIfExists(tmpDir);
        return ExtractOutcome::FailedRawFallback;
    }

    // ── Validation, in order ─────────────────────────────────────────────────────────────────────
    // The child has exited, so entry counting against GdxExtractExpectedCartEntryCount() is over for
    // this run; flip the setup GUI's stage label to "Validating…" for this fast tail.
    gdxAsyncSetSubStage(1 /* ValidatingCart */, /*resetEntries=*/false);
    const fs::path producedArchive = tmpDir / kExtractorOutputName;
    if (!fileExists(producedArchive)) {
        gdx_port_logf("[extract] ERROR: extractor exited 0 but produced no %s. Booting from the raw ROM.\n",
                      kExtractorOutputName);
        removeIfExists(tmpDir);
        return ExtractOutcome::FailedRawFallback;
    }

    // The two gates below need a validated golden (US today). An experimental profile (JP) has none,
    // so both are skipped and the archive installs with a loud EXPERIMENTAL warning -- deliberately
    // weaker than the US path, because a JP archive is unverified.
    std::string archiveSha = sha256File(producedArchive);
    if (archiveSha.empty()) {
        gdx_port_logf("[extract] ERROR: could not hash the extracted archive. Booting from the raw ROM.\n");
        removeIfExists(tmpDir);
        return ExtractOutcome::FailedRawFallback;
    }

    long entries = zipEntryCount(producedArchive);
    if (profile.experimental) {
        gdx_port_logf("[extract] WARNING: %s profile has no validated golden yet (OWNER-RUN-REQUIRED). "
                      "Installing %s WITHOUT the SHA-256/entry-count golden gates — EXPERIMENTAL, "
                      "unverified.\n  archive sha256: %s\n  entries: %ld\n",
                      profile.key, archiveName, archiveSha.c_str(), entries);
    } else {
        if (entries >= 0 && entries != profile.goldenEntryCount) {
            gdx_port_logf("[extract] ERROR: extracted archive has %ld entries, expected %ld. Discarding; "
                          "booting from the raw ROM.\n",
                          entries, profile.goldenEntryCount);
            gdxAsyncPublishError("The extracted archive failed validation (entry-count mismatch). This "
                                 "build's recipes/extractor may not match.");
            removeIfExists(tmpDir);
            return ExtractOutcome::FailedRawFallback;
        }

        // The strongest gate, and an exact match because extraction is deterministic. Being
        // byte-identical to the golden reference also transitively proves the version entry and full
        // key completeness.
        const std::string& expectedSha = profile.goldenSha256;
        if (archiveSha != expectedSha) {
            gdx_port_logf("[extract] ERROR: extracted archive SHA-256 mismatch.\n"
                          "  got:      %s\n  expected: %s\n"
                          "The archive does not match this build's golden reference (recipe/extractor "
                          "drift, or a corrupt build). Discarding; booting from the raw ROM.\n",
                          archiveSha.c_str(), expectedSha.c_str());
            gdxAsyncPublishError("The extracted archive does not match this build's golden reference "
                                 "(recipe/extractor drift or a corrupt build).");
            removeIfExists(tmpDir);
            return ExtractOutcome::FailedRawFallback;
        }
    }

    // Rename temp -> final, preserving any old archive until this succeeds.
    const fs::path finalArchive = dataDir / archiveName;
    if (!atomicReplace(producedArchive, finalArchive)) {
        gdx_port_logf("[extract] ERROR: install failed; the previous %s (if any) is untouched. Booting "
                      "from the raw ROM.\n",
                      archiveName);
        removeIfExists(tmpDir);
        return ExtractOutcome::FailedRawFallback;
    }
    removeIfExists(tmpDir);

    // `state` was default-constructed for THIS run's rom/archive/version fields, which must start
    // empty (see the .empty() guards in the stdout parser). The managed-disk identity is written
    // independently at disk-commit time, possibly long before or after this runs, so it has to be
    // carried over explicitly or this fresh `state` wipes it back to empty/0.
    ExtractState existingForDisk = loadSidecar(dataDir);
    state.diskSha256 = existingForDisk.diskSha256;
    state.diskSize = existingForDisk.diskSize;
    state.diskArchiveSha256 = existingForDisk.diskArchiveSha256; // disk archive identity (RMW-preserved)
    // Same read-modify-write discipline for the IPL fields, written independently by the IPL step and
    // firstboot's acquire-time record, so a cart re-extraction never wipes them.
    state.iplSha256 = existingForDisk.iplSha256;
    state.iplArchiveSha256 = existingForDisk.iplArchiveSha256;
    state.romSha1 = toLowerHex(romSha1);
    state.archiveSha256 = archiveSha;
    state.romSize = fs::file_size(romPath, ec);
    ec.clear();
    state.romMtime = fileMtime(romPath);
    state.profile = profile.key; // record which ROM profile this archive was built for
    saveSidecar(dataDir, state);

    gdx_port_logf("[extract] installed %s [%s] (%ld entries, %s).\n", archiveName, profile.key,
                  entries >= 0 ? entries : profile.goldenEntryCount,
                  profile.experimental ? "EXPERIMENTAL — golden gate skipped" : "sha256 verified");
    return ExtractOutcome::Extracted;
}

// ── Build (or refresh) n64ddipl.o2r from the 64DD IPL ROM ────────────────────────────────────────
// Independent of the cartridge archive (different source media, own golden, own sidecar keys). Runs
// after the cart step inside GdxExtractEnsureArchive so a completed setup can delete N64DDIPLROM.n64.
// Best-effort and NON-gating: a missing IPL, missing extractor, or failed run leaves no archive and
// the port falls back to the raw IPL file. Never throws.
void ensureIplArchive(const fs::path& dataDir, const fs::path& exeDir) {
    std::error_code ec;

    const fs::path finalArchive = dataDir / kIplArchiveName;

    // Per-boot latch (see header): FirstBootRun's iplArchiveSatisfies already hashed this same file
    // against this same recorded value moments ago and it passed, so skip the re-hash.
    if (fileExists(finalArchive) && GdxExtractIsArchiveValidatedThisBoot(GdxExtractArchiveKind::Ipl)) {
        gdx_port_logf("[extract-ipl] %s already verified this boot; skipping re-hash.\n", kIplArchiveName);
        return;
    }

    // ── Warm-boot validate-only, FIRST and IPL-source-INDEPENDENT ──────────────────────────────────
    // A present archive whose SHA-256 matches the recorded sidecar value is up to date. This MUST run
    // even when the raw N64DDIPLROM.n64 is gone (archive-only boot): validation needs only the archive
    // and the sidecar, and gating it behind a present raw IPL leaves a stale/corrupt n64ddipl.o2r
    // unverified once the original is deleted. Only the REBUILD below needs the raw IPL.
    ExtractState sidecar = loadSidecar(dataDir);
    if (fileExists(finalArchive) && !sidecar.iplArchiveSha256.empty()) {
        std::string actual = sha256File(finalArchive);
        if (!actual.empty() && actual == sidecar.iplArchiveSha256) {
            gdx_port_logf("[extract-ipl] %s already present and matches the recorded state; up to date.\n",
                          kIplArchiveName);
            return;
        }
    }

    // Only a (re)build needs an IPL source: the canonical installed copy in the data dir, then the
    // accepted US-prototype alt name there, then the same two names next to the exe (dev/portable
    // layout). Delegating to the shared GdxFindIplSourceInDir keeps this probe from drifting away from
    // the setup GUI's Recheck() and FirstBootRun's probes, and it is what recovers a stale
    // Game.DdIplPath entry, since the helper re-derives the path from each directory instead of
    // trusting a recorded value.
    std::string foundIpl = GdxFindIplSourceInDir(dataDir.string());
    if (foundIpl.empty()) {
        foundIpl = GdxFindIplSourceInDir(exeDir.string());
    }
    fs::path iplRom = foundIpl.empty() ? fs::path() : fs::path(foundIpl);
    if (iplRom.empty()) {
        // The only place the port can observe "no IPL archive AND no IPL ROM source" at boot, so it
        // has to be loud: quietly skipping here means silent 64DD-font loss with no diagnostic.
        gdx_port_logf("[extract-ipl] WARNING: %s missing and no IPL ROM found (looked for %s, %s) — "
                      "64DD error-screen font unavailable.\n",
                      kIplArchiveName, kIplRomName, SetupIplFileNameUsProto());
        return;
    }

    const fs::path extractBin = exeDir / kExtractBinaryName;
    if (!fileExists(extractBin)) {
        gdx_port_logf("[extract-ipl] extractor component missing (%s); cannot build %s (raw-IPL "
                      "fallback).\n",
                      extractBin.string().c_str(), kIplArchiveName);
        return;
    }

    // Fresh temp staging dir on the same filesystem as the final archive (install is a rename).
    const fs::path tmpDir = dataDir / kIplTempSubdir;
    removeIfExists(tmpDir);
    fs::create_directories(tmpDir, ec);
    if (ec) {
        gdx_port_logf("[extract-ipl] ERROR: could not create temp dir %s: %s (raw-IPL fallback).\n",
                      tmpDir.string().c_str(), ec.message().c_str());
        return;
    }
    ec.clear();

    gdx_port_logf("[extract-ipl] building %s from %s (one-time).\n", kIplArchiveName,
                  iplRom.string().c_str());

    // Its own child process and stdout stream, so entry counting resets and the setup GUI's "N / 2"
    // bar tracks THIS step rather than a carried-over cart-step count.
    gdxAsyncSetSubStage(2 /* Ipl */, /*resetEntries=*/true);

    ExtractState state; // captures the identity SHA from the extractor's stdout (scanStdoutLine)
    int exitCode = 1;
    bool ok = false;
#ifdef _WIN32
    auto q = [](const std::wstring& s) { return L"\"" + s + L"\""; };
    std::wstring cmd = q(extractBin.wstring());
    cmd += L" ipl ";
    cmd += q(iplRom.wstring());
    cmd += L" -d ";
    cmd += q(tmpDir.wstring());
    ok = runExtractorWindows(extractBin, cmd, tmpDir, exitCode, state);
#else
    std::vector<std::string> args = { "ipl", iplRom.string(), "-d", tmpDir.string() };
    ok = runExtractorPosix(extractBin, args, tmpDir, exitCode, state);
#endif

    if (!ok) {
        gdx_port_logf("[extract-ipl] ERROR: extractor exited with code %d; keeping any previous %s "
                      "(raw-IPL fallback).\n",
                      exitCode, kIplArchiveName);
        removeIfExists(tmpDir);
        return;
    }

    const fs::path produced = tmpDir / kIplArchiveName;
    if (!fileExists(produced)) {
        gdx_port_logf("[extract-ipl] ERROR: extractor exited 0 but produced no %s (raw-IPL fallback).\n",
                      kIplArchiveName);
        removeIfExists(tmpDir);
        return;
    }

    // Structural check: exactly the two frozen entries.
    long entries = zipEntryCount(produced);
    if (entries >= 0 && entries != kIplExpectedEntryCount) {
        gdx_port_logf("[extract-ipl] ERROR: %s has %ld entries, expected %d; discarding (raw-IPL "
                      "fallback).\n",
                      kIplArchiveName, entries, kIplExpectedEntryCount);
        removeIfExists(tmpDir);
        return;
    }

    const std::string archiveSha = sha256File(produced);
    if (archiveSha.empty()) {
        gdx_port_logf("[extract-ipl] ERROR: could not hash %s (raw-IPL fallback).\n", kIplArchiveName);
        removeIfExists(tmpDir);
        return;
    }

    // Dev-drift warning ONLY: the IPL golden is per-user self-consistency, so a mismatch just flags
    // that this build's reference header does not match this machine's dump. The placeholder
    // zero-hash suppresses the warning until a real header is generated.
    const std::string expectedArchive = toLowerHex(std::string(GDX_IPL_ARCHIVE_EXPECTED_SHA256));
    static const std::string kIplPlaceholder(64, '0');
    if (expectedArchive != kIplPlaceholder && archiveSha != expectedArchive) {
        gdx_port_logf("[extract-ipl] NOTE: %s SHA-256 %s differs from this build's owner-reference "
                      "golden %s (per-user IPL dump differs — not an error).\n",
                      kIplArchiveName, archiveSha.c_str(), expectedArchive.c_str());
    }

    if (!atomicReplace(produced, finalArchive)) {
        gdx_port_logf("[extract-ipl] ERROR: install failed; previous %s (if any) untouched (raw-IPL "
                      "fallback).\n",
                      kIplArchiveName);
        removeIfExists(tmpDir);
        return;
    }
    removeIfExists(tmpDir);

    // Record the sidecar keys (read-modify-write: preserve the cart/disk fields written elsewhere).
    ExtractState st = loadSidecar(dataDir);
    st.iplArchiveSha256 = archiveSha;
    if (!state.iplSha256.empty()) {
        st.iplSha256 = state.iplSha256; // normalized-IPL identity, authored by the extractor
    }
    saveSidecar(dataDir, st);

    gdx_port_logf("[extract-ipl] installed %s (%ld entries, identity %s).\n", kIplArchiveName,
                  entries >= 0 ? entries : static_cast<long>(kIplExpectedEntryCount),
                  st.iplSha256.empty() ? "unknown" : st.iplSha256.c_str());
}

// ── Resolve the disk source (.ndd) the archive is built from ──────────────────────────────────────
// Managed-copy-preferred, mirroring port/disk_buffer.cpp's search order: <dataDir>/media, the data
// dir, <exeDir>/media, the exe dir. Portable installs keep dataDir == exeDir, so those collapse.
// Empty path when no source is found.
fs::path resolveDiskSource(const fs::path& dataDir, const fs::path& exeDir) {
    const fs::path bases[] = { dataDir / kManagedMediaSubdir, dataDir,
                               exeDir / kManagedMediaSubdir, exeDir };
    for (const fs::path& base : bases) {
        for (const char* name : kDiskSourceNames) {
            fs::path cand = base / name;
            if (fileExists(cand)) {
                return cand;
            }
        }
    }
    return {};
}

// ── Build (or refresh) fzerox-disk.o2r from the 64DD EK disk image ────────────────────────────────
// Independent of the cartridge/IPL archives (own golden, own sidecar key). Runs after ensureIplArchive
// inside GdxExtractEnsureArchive. Best-effort and NON-gating: a missing disk, missing extractor, or
// failed run leaves no archive and the port falls back to the managed copy / raw .ndd. The image is
// stored VERBATIM (the loader never byte-swaps it), so SHA-256(disk/image) equals the managed-copy
// sha — that equivalence is what the boot-time deletion gate proves. Never throws.
void ensureDiskArchive(const fs::path& dataDir, const fs::path& exeDir) {
    std::error_code ec;

    const fs::path finalArchive = dataDir / kDiskArchiveName;

    // Per-boot latch (see header), FIRST and disk-source-INDEPENDENT, mirroring ensureIplArchive's
    // ordering: FirstBootRun's diskArchiveSatisfies already hashed this file against this recorded
    // value moments ago, so skip the ~30-40 MB re-hash. This MUST stay above resolveDiskSource: when
    // the raw .ndd and managed copy are both gone -- the only scenario diskArchiveSatisfies's hash
    // path runs in -- resolveDiskSource finds nothing and returns before the latch is ever consulted.
    if (fileExists(finalArchive) && GdxExtractIsArchiveValidatedThisBoot(GdxExtractArchiveKind::Disk)) {
        gdx_port_logf("[extract-disk] %s already verified this boot; skipping re-hash.\n", kDiskArchiveName);
        return;
    }

    const fs::path diskSrc = resolveDiskSource(dataDir, exeDir);
    if (diskSrc.empty()) {
        gdx_port_logf("[extract-disk] no EK disk image in the data/media or exe dir; skipping %s "
                      "(managed-copy/raw fallback).\n",
                      kDiskArchiveName);
        return;
    }

    // Warm-boot: a present archive whose SHA-256 matches the recorded sidecar value is up to date.
    ExtractState sidecar = loadSidecar(dataDir);
    if (fileExists(finalArchive) && !sidecar.diskArchiveSha256.empty()) {
        std::string actual = sha256File(finalArchive);
        if (!actual.empty() && actual == sidecar.diskArchiveSha256) {
            gdx_port_logf("[extract-disk] %s already present and matches the recorded state; up to "
                          "date.\n",
                          kDiskArchiveName);
            return;
        }
    }

    const fs::path extractBin = exeDir / kExtractBinaryName;
    if (!fileExists(extractBin)) {
        gdx_port_logf("[extract-disk] extractor component missing (%s); cannot build %s "
                      "(managed-copy/raw fallback).\n",
                      extractBin.string().c_str(), kDiskArchiveName);
        return;
    }

    // Disk-space guard: the disk archive deflates the 64.9 MB image to ~30-40 MB; require ~3x headroom.
    auto space = fs::space(dataDir, ec);
    if (!ec && space.available < 3u * 64u * 1024u * 1024u) {
        gdx_port_logf("[extract-disk] not enough free disk space to build %s; skipping "
                      "(managed-copy/raw fallback).\n",
                      kDiskArchiveName);
        return;
    }
    ec.clear();

    // Fresh temp staging dir on the same filesystem as the final archive (install is a rename).
    const fs::path tmpDir = dataDir / kDiskTempSubdir;
    removeIfExists(tmpDir);
    fs::create_directories(tmpDir, ec);
    if (ec) {
        gdx_port_logf("[extract-disk] ERROR: could not create temp dir %s: %s (managed-copy/raw "
                      "fallback).\n",
                      tmpDir.string().c_str(), ec.message().c_str());
        return;
    }
    ec.clear();

    // With the EK slice manifest next to the recipes, the archive also carries the ek/<symbol>
    // per-asset entries. Its absence is tolerated — the subcommand then emits only the two disk/*
    // entries — but the structural gate below expects GDX_DISK_EXPECTED_ENTRY_COUNT, so a
    // manifest-absent build falls back cleanly instead of installing a short archive.
    const fs::path manifestPath = exeDir / kRecipesDirName / kEkSliceManifestName;
    const bool haveManifest = fileExists(manifestPath);
    gdx_port_logf("[extract-disk] building %s from %s (one-time)%s.\n", kDiskArchiveName,
                  diskSrc.string().c_str(),
                  haveManifest ? " with EK slice manifest" : " (no EK manifest; disk/* entries only)");

    ExtractState state;
    int exitCode = 1;
    bool ok = false;
#ifdef _WIN32
    auto q = [](const std::wstring& s) { return L"\"" + s + L"\""; };
    std::wstring cmd = q(extractBin.wstring());
    cmd += L" disk ";
    cmd += q(diskSrc.wstring());
    cmd += L" -d ";
    cmd += q(tmpDir.wstring());
    if (haveManifest) {
        cmd += L" -m ";
        cmd += q(manifestPath.wstring());
    }
    ok = runExtractorWindows(extractBin, cmd, tmpDir, exitCode, state);
#else
    std::vector<std::string> args = { "disk", diskSrc.string(), "-d", tmpDir.string() };
    if (haveManifest) {
        args.push_back("-m");
        args.push_back(manifestPath.string());
    }
    ok = runExtractorPosix(extractBin, args, tmpDir, exitCode, state);
#endif

    if (!ok) {
        gdx_port_logf("[extract-disk] ERROR: extractor exited with code %d; keeping any previous %s "
                      "(managed-copy/raw fallback).\n",
                      exitCode, kDiskArchiveName);
        removeIfExists(tmpDir);
        return;
    }

    const fs::path produced = tmpDir / kDiskArchiveName;
    if (!fileExists(produced)) {
        gdx_port_logf("[extract-disk] ERROR: extractor exited 0 but produced no %s (managed-copy/raw "
                      "fallback).\n",
                      kDiskArchiveName);
        removeIfExists(tmpDir);
        return;
    }

    // The two frozen disk/* entries plus the ek/<symbol> per-asset entries, against the data-driven
    // GDX_DISK_EXPECTED_ENTRY_COUNT golden.
    long entries = zipEntryCount(produced);
    if (entries >= 0 && entries != kDiskExpectedEntryCount) {
        gdx_port_logf("[extract-disk] ERROR: %s has %ld entries, expected %d; discarding "
                      "(managed-copy/raw fallback).\n",
                      kDiskArchiveName, entries, kDiskExpectedEntryCount);
        removeIfExists(tmpDir);
        return;
    }

    const std::string archiveSha = sha256File(produced);
    if (archiveSha.empty()) {
        gdx_port_logf("[extract-disk] ERROR: could not hash %s (managed-copy/raw fallback).\n",
                      kDiskArchiveName);
        removeIfExists(tmpDir);
        return;
    }

    // Dev-drift warning ONLY: the disk golden is per-user self-consistency, so a mismatch just flags
    // that this build's owner-reference header does not match this machine's dump. The placeholder
    // zero-hash suppresses the warning until a real header is generated.
    const std::string expectedArchive = toLowerHex(std::string(GDX_DISK_ARCHIVE_EXPECTED_SHA256));
    static const std::string kDiskPlaceholder(64, '0');
    if (expectedArchive != kDiskPlaceholder && archiveSha != expectedArchive) {
        gdx_port_logf("[extract-disk] NOTE: %s SHA-256 %s differs from this build's owner-reference "
                      "golden %s (per-user disk dump differs — not an error).\n",
                      kDiskArchiveName, archiveSha.c_str(), expectedArchive.c_str());
    }

    if (!atomicReplace(produced, finalArchive)) {
        gdx_port_logf("[extract-disk] ERROR: install failed; previous %s (if any) untouched "
                      "(managed-copy/raw fallback).\n",
                      kDiskArchiveName);
        removeIfExists(tmpDir);
        return;
    }
    removeIfExists(tmpDir);

    // Record the sidecar key (read-modify-write: preserve the cart/disk/ipl fields written elsewhere).
    ExtractState st = loadSidecar(dataDir);
    st.diskArchiveSha256 = archiveSha;
    saveSidecar(dataDir, st);

    gdx_port_logf("[extract-disk] installed %s (%ld entries, container %s).\n", kDiskArchiveName,
                  entries >= 0 ? entries : static_cast<long>(kDiskExpectedEntryCount),
                  archiveSha.c_str());
}

} // namespace

static ExtractOutcome ensureCartArchive(const char* dataDirC, const char* romPathC, const char* exeDirC) {
    if (dataDirC == nullptr || dataDirC[0] == '\0' || exeDirC == nullptr || exeDirC[0] == '\0') {
        gdx_port_logf("[extract] missing data/exe path; skipping extraction (raw-ROM fallback).\n");
        return ExtractOutcome::FailedRawFallback;
    }
    std::error_code ec;
    const fs::path dataDir(dataDirC);
    // The ROM is OPTIONAL: an archive-only (deleted-original) boot passes an empty romPath. The
    // ROM-independent warm-boot checks below still run and validate/accept a present archive; only the
    // REBUILD path further down needs the original. Returning early on an empty romPath would skip the
    // golden hash check entirely on exactly the boots that most need it.
    const bool haveRom = (romPathC != nullptr && romPathC[0] != '\0');
    const fs::path romPath = haveRom ? fs::path(romPathC) : fs::path();
    const fs::path exeDir(exeDirC);

    const fs::path archive = dataDir / kArchiveName;
    // Per-boot latch (see header): FirstBootRun's gameArchiveSatisfies already hashed this file
    // against the recorded archive_sha256 moments ago and it passed, which implies the sidecar was
    // already consistent, so the refresh branch below would be a no-op. Skip straight to UpToDate
    // without re-hashing the ~15 MB archive.
    if (fileExists(archive) && GdxExtractIsArchiveValidatedThisBoot(GdxExtractArchiveKind::Cart)) {
        gdx_port_logf("[extract] %s already verified this boot; skipping re-hash.\n", kArchiveName);
        return ExtractOutcome::UpToDate;
    }

    // ── Warm-boot check, FIRST and ROM-independent ───────────────────────────────────────────────
    // A present archive that hashes to the golden constant is valid regardless of the ROM's state, so
    // accept it before any ROM check. The ordering is load-bearing: an early ROM-related return would
    // leave a stale/corrupt archive in place for the mount path, whose gate gets only as far as the
    // version entry -- which bit rot preserves.
    const std::string expectedSha = toLowerHex(std::string(GDX_O2R_EXPECTED_SHA256));
    if (fileExists(archive)) {
        std::string actual = sha256File(archive);
        if (!actual.empty() && actual == expectedSha) {
            ExtractState sidecar = loadSidecar(dataDir);
            if (!sidecar.valid || sidecar.archiveSha256 != expectedSha) {
                ExtractState st = sidecar;
                st.archiveSha256 = actual;
                if (fileExists(romPath)) { // ROM fields are best-effort bookkeeping
                    st.romSha1 = toLowerHex(sha1File(romPath));
                    st.romSize = fs::file_size(romPath, ec);
                    ec.clear();
                    st.romMtime = fileMtime(romPath);
                }
                saveSidecar(dataDir, st);
                gdx_port_logf("[extract] existing %s matches the golden reference; recorded state.\n",
                              kArchiveName);
            }
            return ExtractOutcome::UpToDate;
        }
        gdx_port_logf("[extract] existing %s does not match this build's golden reference.\n", kArchiveName);
    }

    // ── Resolve supported ROM profiles (US + JP) from the recipe config ────────────────────────────
    const fs::path configYml = exeDir / kRecipesDirName / kConfigYmlName;
    std::vector<RomProfile> profiles = buildRomProfiles(configYml);
    const RomProfile& usProfile = profiles[0];
    const RomProfile& jpProfile = profiles[1];

    // ── JP warm-boot ───────────────────────────────────────────────────────────────────────────────
    // With no JP golden, a present JP archive is checked for self-consistency against the sidecar
    // (recorded profile == jp/rev0, recorded archive SHA-256 == the file's) rather than a golden
    // constant — the same per-user model the IPL step uses.
    {
        const fs::path jpArchive = dataDir / jpProfile.archiveName;
        if (fileExists(jpArchive)) {
            ExtractState sc = loadSidecar(dataDir);
            if (sc.valid && sc.profile == std::string(jpProfile.key) && !sc.archiveSha256.empty()) {
                std::string actual = sha256File(jpArchive);
                if (!actual.empty() && actual == sc.archiveSha256) {
                    gdx_port_logf("[extract] existing %s [%s] matches the recorded sidecar; up to date "
                                  "(EXPERIMENTAL — no golden).\n", jpProfile.archiveName, jpProfile.key);
                    return ExtractOutcome::UpToDate;
                }
            }
        }
    }

    // A non-golden archive that cannot be replaced must never reach the mount path.
    auto quarantine = [&](const char* archiveName) {
        const fs::path a = dataDir / archiveName;
        if (fileExists(a)) {
            const fs::path bad = dataDir / (std::string(archiveName) + ".bad");
            std::error_code qec;
            fs::remove(bad, qec);
            qec.clear();
            fs::rename(a, bad, qec);
            if (qec) {
                gdx_port_logf("[extract] WARNING: could not quarantine the stale %s (%s); the version "
                              "gate is the remaining defense.\n",
                              archiveName, qec.message().c_str());
            } else {
                gdx_port_logf("[extract] quarantined the stale archive as %s.bad (raw-ROM fallback).\n",
                              archiveName);
            }
        }
    };
    // Before a profile match is known, quarantine the US archive by default.
    auto failRawQuarantine = [&]() {
        quarantine(kArchiveName);
        return ExtractOutcome::FailedRawFallback;
    };

    if (!haveRom || !fileExists(romPath)) {
        gdx_port_logf("[extract] no original ROM available (%s); cannot (re)build the archive. The "
                      "warm-boot checks above already accepted any valid installed archive; a mismatch "
                      "here means setup must be re-run with the original ROM (raw-ROM fallback).\n",
                      haveRom ? romPath.string().c_str() : "archive-only boot, original deleted");
        return failRawQuarantine();
    }

    // ── Validate ROM identity BEFORE any spawn; match against every profile's SHA-1 ────────────────
    std::string romSha1 = toLowerHex(sha1File(romPath));
    if (romSha1.empty()) {
        gdx_port_logf("[extract] ERROR: could not hash the ROM at %s; skipping extraction (raw-ROM "
                      "fallback).\n",
                      romPath.string().c_str());
        return failRawQuarantine();
    }
    const RomProfile* matched = nullptr;
    for (const RomProfile& p : profiles) {
        if (romSha1 == p.expectedSha1) {
            matched = &p;
            break;
        }
    }
    if (matched == nullptr) {
        gdx_port_logf("[extract] ERROR: ROM matches no supported profile — extraction skipped, booting "
                      "from the raw ROM.\n"
                      "  ROM sha1:  %s\n"
                      "  supported: US rev0 %s -> fzerox.o2r\n"
                      "             JP rev0 %s -> fzerox-jp.o2r\n"
                      "Extraction supports the big-endian F-Zero X US rev0 or JP rev0 (.z64) cartridge.\n",
                      romSha1.c_str(), usProfile.expectedSha1.c_str(), jpProfile.expectedSha1.c_str());
        gdxAsyncPublishError("This ROM matches neither supported dump. Extraction supports the F-Zero X "
                             "US rev0 or JP rev0 (.z64) cartridge.");
        return failRawQuarantine();
    }
    gdx_port_logf("[extract] ROM matched profile %s -> %s%s.\n", matched->key, matched->archiveName,
                  matched->experimental ? " (EXPERIMENTAL — no golden yet)" : "");

    // ── Extract ──────────────────────────────────────────────────────────────────────────────────
    // Any pre-existing non-golden archive stays in place while the replacement is produced; the atomic
    // install renames over it only after validation. On failure, quarantine the MATCHED profile's
    // archive so a stale one never reaches the mount.
    ExtractOutcome outcome = runExtraction(dataDir, romPath, exeDir, romSha1, *matched);
    if (outcome == ExtractOutcome::FailedRawFallback) {
        quarantine(matched->archiveName);
        return ExtractOutcome::FailedRawFallback;
    }
    return outcome;
}

ExtractOutcome GdxExtractEnsureArchive(const char* dataDirC, const char* romPathC, const char* exeDirC) {
    // Cartridge archive first, since its outcome is what the caller logs and acts on, then the
    // independent IPL step (best-effort, non-gating). The IPL step resolves its own source from the
    // data or exe dir — the caller never passes an IPL path — and runs regardless of the cart outcome:
    // a valid, up-to-date cart archive must not skip IPL provisioning.
    ExtractOutcome outcome = ensureCartArchive(dataDirC, romPathC, exeDirC);
    if (dataDirC != nullptr && dataDirC[0] != '\0' && exeDirC != nullptr && exeDirC[0] != '\0') {
        ensureIplArchive(fs::path(dataDirC), fs::path(exeDirC));
        // After IPL, both best-effort and non-gating. Resolves its own source (managed copy
        // preferred) from dataDir/exeDir — the caller passes no disk path.
        ensureDiskArchive(fs::path(dataDirC), fs::path(exeDirC));
    }
    return outcome;
}

const char* GdxExtractOutcomeString(ExtractOutcome outcome) {
    switch (outcome) {
        case ExtractOutcome::UpToDate:
            return "up to date (fzerox.o2r already valid)";
        case ExtractOutcome::Extracted:
            return "extracted and installed fzerox.o2r";
        case ExtractOutcome::FailedRawFallback:
            return "not available — booting from the raw ROM";
    }
    return "unknown";
}

HackBuildResult GdxExtractBuildHackArchive(const char* romPathC, const char* hackNameC,
                                           const char* hackDirC, const char* exeDirC) {
    HackBuildResult out;

    if (romPathC == nullptr || hackNameC == nullptr || hackDirC == nullptr || exeDirC == nullptr ||
        romPathC[0] == '\0' || hackNameC[0] == '\0' || hackDirC[0] == '\0' || exeDirC[0] == '\0') {
        out.message = "Internal error: missing ROM path, hack name, hack folder or program folder.";
        return out;
    }

    // The basename becomes a file name and, at boot, part of a save file name. It goes through the
    // picker own sanitiser rather than a second, subtly different one.
    char safeName[GDX_HACKMOD_NAME_MAX];
    if (gdx_hackmod_sanitize_name(hackNameC, safeName, sizeof(safeName)) == 0) {
        out.message = "That name has no characters usable in a file name. Use letters, digits, dots, "
                      "dashes or underscores.";
        return out;
    }

    std::error_code ec;
    const fs::path romPath(romPathC);
    const fs::path hackDir(hackDirC);
    const fs::path exeDir(exeDirC);
    const fs::path extractBin = exeDir / kExtractBinaryName;
    const fs::path recipesDir = exeDir / kRecipesDirName;

    if (!fileExists(romPath)) {
        out.message = "That ROM file does not exist.";
        return out;
    }
    if (!fileExists(extractBin)) {
        out.message = "The extractor component (gdx-extract) is missing. Reinstall G-Diffuser.";
        return out;
    }
    if (!fs::is_directory(recipesDir, ec)) {
        ec.clear();
        out.message = "The recipe data (decomp-recipes) is missing. Reinstall G-Diffuser.";
        return out;
    }
    ec.clear();

    // runExtraction stages a short-path copy of the recipes when the install is deep. This path is
    // user-initiated rather than boot-critical, so it refuses with advice instead of duplicating that
    // machinery onto a second code path.
    constexpr size_t kDeepestRecipeSuffix = 54;
    if (recipesDir.string().size() + kDeepestRecipeSuffix >= 248) {
        out.message = "G-Diffuser sits in a folder too deep for the extractor to read its recipes. "
                      "Move the installation somewhere shorter and try again.";
        return out;
    }

    // Identify the ROM before spawning anything. A stock dump is not a hack: it belongs in the normal
    // extraction path, and packaging one here would only shadow the base game with a copy of itself.
    const std::string romSha1 = toLowerHex(sha1File(romPath));
    if (romSha1.empty()) {
        out.message = "Could not read that ROM.";
        return out;
    }

    const std::vector<RomProfile> profiles = buildRomProfiles(recipesDir / "config.yml");
    const RomProfile* base = nullptr;
    for (const RomProfile& p : profiles) {
        if (romSha1 == p.expectedSha1) {
            out.message = std::string("That is the stock ") + p.key +
                          " ROM, not a ROM hack, so nothing would change. Choose a patched ROM.";
            return out;
        }
        if (base == nullptr && std::string(p.key) == "us/rev0") {
            base = &p;
        }
    }
    if (base == nullptr || base->expectedSha1.empty()) {
        out.message = "Could not resolve the base US recipe tree from decomp-recipes/config.yml.";
        return out;
    }
    out.expectedEntryCount = base->goldenEntryCount;

    fs::create_directories(hackDir, ec);
    if (ec) {
        out.message = "Could not create " + hackDir.string() + ": " + ec.message();
        return out;
    }
    ec.clear();

    // Staged inside the destination folder so the install below is a rename on one filesystem.
    const fs::path tmpDir = hackDir / ".gdx_hackbuild.tmp";
    removeIfExists(tmpDir);
    fs::create_directories(tmpDir, ec);
    if (ec) {
        out.message = "Could not create a temporary folder in " + hackDir.string() + ": " + ec.message();
        return out;
    }
    ec.clear();

    char versionArg[16];
    std::snprintf(versionArg, sizeof(versionArg), "%u", static_cast<unsigned>(base->versionCrc));

    gdx_port_logf("[hackbuild] building %s from %s (sha1 %s) using the %s recipe tree.\n", safeName,
                  romPath.string().c_str(), romSha1.c_str(), base->key);

    ExtractState state;
    int exitCode = 1;
    bool ok = false;
#ifdef _WIN32
    auto q = [](const std::wstring& v) { return L"\"" + v + L"\""; };
    std::wstring cmd = q(extractBin.wstring());
    cmd += L" o2r ";
    cmd += q(romPath.wstring());
    cmd += L" -s ";
    cmd += q(recipesDir.wstring());
    cmd += L" -d ";
    cmd += q(tmpDir.wstring());
    cmd += L" -u ";
    cmd += std::wstring(versionArg, versionArg + std::strlen(versionArg));
    // The whole point of this path: a hack's own hash names no recipe tree, so the base game's tree is
    // named explicitly instead.
    cmd += L" --config-key ";
    cmd += std::wstring(base->expectedSha1.begin(), base->expectedSha1.end());
    // The archive is structurally a base-game archive, so its version entry must say so. Without
    // this it carries the HACK ROM's CRC and the mount-time version gate rejects it on sight.
    cmd += L" --version-crc ";
    cmd += std::wstring(versionArg, versionArg + std::strlen(versionArg));
    ok = runExtractorWindows(extractBin, cmd, tmpDir, exitCode, state);
#else
    std::vector<std::string> args = { "o2r",           romPath.string(), "-s", recipesDir.string(),
                                      "-d",            tmpDir.string(),  "-u", versionArg,
                                      "--config-key",  base->expectedSha1,
                                      // See the Windows branch: the version entry describes the
                                      // recipe tree that produced the archive, not the ROM's origin.
                                      "--version-crc", versionArg };
    ok = runExtractorPosix(extractBin, args, tmpDir, exitCode, state);
#endif

    removeIfExists(tmpDir / kTorchHashName);

    const fs::path produced = tmpDir / kExtractorOutputName;
    if (!ok || !fileExists(produced)) {
        // torch exits 0 when it finds no recipe tree at all, so "exited cleanly but produced nothing"
        // is a real case and deserves its own wording rather than blaming the exit code.
        out.message = !ok ? ("The extractor failed (exit code " + std::to_string(exitCode) + ").")
                          : std::string("The extractor exited cleanly but produced no archive, which "
                                        "usually means it could not read this ROM with the base "
                                        "recipes.");
        gdx_port_logf("[hackbuild] ERROR: %s\n", out.message.c_str());
        removeIfExists(tmpDir);
        return out;
    }

    // The recipe tree fixes the entry count regardless of ROM contents, so a shortfall is a real
    // damage signal. It is reported rather than enforced: a hack has no golden to be right against.
    out.entryCount = zipEntryCount(produced);

    const fs::path finalPath = hackDir / (std::string(safeName) + ".o2r");
    if (!atomicReplace(produced, finalPath)) {
        out.message = "Could not install the archive into " + hackDir.string() + ".";
        gdx_port_logf("[hackbuild] ERROR: %s\n", out.message.c_str());
        removeIfExists(tmpDir);
        return out;
    }
    removeIfExists(tmpDir);

    out.ok = true;
    out.archivePath = finalPath.string();

    std::string damage;
    if (out.entryCount >= 0 && out.expectedEntryCount > 0 && out.entryCount != out.expectedEntryCount) {
        damage = " It holds " + std::to_string(out.entryCount) + " entries where the base game has " +
                 std::to_string(out.expectedEntryCount) +
                 ", so some assets could not be read; those fall back to the base game's versions.";
    }
    out.message = "Built " + finalPath.filename().string() + "." + damage +
                  " Select it under ROM Hacks and restart to play it. Only the assets and courses "
                  "come across: code the hack patched is never run.";
    gdx_port_logf("[hackbuild] installed %s (%ld entries, base game has %ld).\n",
                  finalPath.string().c_str(), out.entryCount, out.expectedEntryCount);
    return out;
}

// ── Async driver (see header) ────────────────────────────────────────────────────────────────────

void GdxExtractStartAsync(const char* dataDir, const char* romPath, const char* exeDir,
                          bool suppressNativeDialog) {
    AsyncExtractState& s = asyncState();
    if (s.phase.load() == 1) {
        gdx_port_logf("[extract] async extraction already running; ignoring duplicate start\n");
        return;
    }
    // Reclaim a previously finished worker before reusing the slot (e.g. a Retry after a failure).
    if (s.worker.joinable()) {
        s.worker.join();
    }
    {
        std::lock_guard<std::mutex> lk(s.mtx);
        s.stage.clear();
        s.lastError.clear();
        s.log.clear();
    }
    s.entriesSeen.store(0);
    s.subStage.store(0);
    s.suppressDialog.store(suppressNativeDialog);
    s.outcome.store(static_cast<int>(ExtractOutcome::FailedRawFallback));
    s.phase.store(1);

    std::string d = (dataDir != nullptr) ? dataDir : "";
    std::string r = (romPath != nullptr) ? romPath : "";
    std::string e = (exeDir != nullptr) ? exeDir : "";
    s.worker = std::thread([d, r, e]() {
        ExtractOutcome o = GdxExtractEnsureArchive(d.c_str(), r.c_str(), e.c_str());
        AsyncExtractState& st = asyncState();
        st.outcome.store(static_cast<int>(o));
        st.phase.store(2); // publish outcome before flipping phase to Done
    });
}

ExtractProgress GdxExtractPollStatus() {
    AsyncExtractState& s = asyncState();
    ExtractProgress p;
    const int ph = s.phase.load();
    p.phase = (ph == 2) ? ExtractPhase::Done : (ph == 1) ? ExtractPhase::Running : ExtractPhase::Idle;
    p.outcome = static_cast<ExtractOutcome>(s.outcome.load());
    p.entriesSeen = s.entriesSeen.load(std::memory_order_relaxed);
    p.subStage = s.subStage.load(std::memory_order_relaxed);
    std::lock_guard<std::mutex> lk(s.mtx);
    p.stage = s.stage;
    p.lastError = s.lastError;
    p.log.assign(s.log.begin(), s.log.end());
    return p;
}

int GdxExtractExpectedCartEntryCount() {
    return static_cast<int>(GDX_O2R_EXPECTED_ENTRY_COUNT);
}

int GdxExtractExpectedIplEntryCount() {
    return static_cast<int>(kIplExpectedEntryCount);
}

void GdxExtractResetAsync() {
    AsyncExtractState& s = asyncState();
    if (s.worker.joinable()) {
        s.worker.join();
    }
    s.phase.store(0);
    s.suppressDialog.store(false);
    std::lock_guard<std::mutex> lk(s.mtx);
    s.stage.clear();
    s.lastError.clear();
}

std::string GdxExtractFileSha1(const char* path) {
    if (path == nullptr || path[0] == '\0') {
        return {};
    }
    return toLowerHex(sha1File(fs::path(path)));
}

std::string GdxExtractExpectedRomSha1(const char* exeDir) {
    if (exeDir != nullptr && exeDir[0] != '\0') {
        std::string fromConfig =
            expectedRomSha1FromConfig(fs::path(exeDir) / kRecipesDirName / kConfigYmlName);
        if (!fromConfig.empty()) {
            return toLowerHex(fromConfig);
        }
    }
    return toLowerHex(std::string(kExpectedRomSha1Fallback));
}

std::string GdxExtractFileSha256(const char* path) {
    if (path == nullptr || path[0] == '\0') {
        return {};
    }
    return sha256File(fs::path(path));
}

void GdxExtractRecordManagedDisk(const char* dataDirC, const char* diskSha256, unsigned long long diskSize) {
    if (dataDirC == nullptr || dataDirC[0] == '\0') {
        return;
    }
    const fs::path dataDir(dataDirC);
    ExtractState st = loadSidecar(dataDir); // read-modify-write: preserve rom/archive fields, if any
    st.diskSha256 = (diskSha256 != nullptr) ? toLowerHex(std::string(diskSha256)) : std::string();
    st.diskSize = static_cast<std::uintmax_t>(diskSize);
    if (!saveSidecar(dataDir, st)) {
        gdx_port_logf("[extract] WARNING: could not record the managed disk identity in %s\n", kSidecarName);
    }
}

void GdxExtractRecordIpl(const char* dataDirC, const char* iplSha256) {
    if (dataDirC == nullptr || dataDirC[0] == '\0') {
        return;
    }
    const fs::path dataDir(dataDirC);
    // Read-modify-write: every other sidecar field (cart/disk/archive identity) survives. This is
    // firstboot's acquire-time provenance note. For a native big-endian (z64) dump — the common case —
    // this raw-file SHA already equals the extractor's normalized identity; for a byte-swapped dump the
    // later IPL extraction step refreshes ipl_sha256 to the normalized value, so the sidecar converges
    // on the byte-order-independent identity either way.
    ExtractState st = loadSidecar(dataDir);
    st.iplSha256 = (iplSha256 != nullptr) ? toLowerHex(std::string(iplSha256)) : std::string();
    if (!saveSidecar(dataDir, st)) {
        gdx_port_logf("[extract] WARNING: could not record the IPL identity in %s\n", kSidecarName);
    }
}

// ── Disk deletion-gate helpers (see header) ───────────────────────────────────────────────────────

std::string GdxExtractRecordedDiskSha256(const char* dataDirC) {
    if (dataDirC == nullptr || dataDirC[0] == '\0') {
        return {};
    }
    return loadSidecar(fs::path(dataDirC)).diskSha256; // managed-copy raw SHA-256 (lowercase hex)
}

std::string GdxExtractRecordedDiskArchiveSha256(const char* dataDirC) {
    if (dataDirC == nullptr || dataDirC[0] == '\0') {
        return {};
    }
    return loadSidecar(fs::path(dataDirC)).diskArchiveSha256; // fzerox-disk.o2r container SHA-256
}

std::string GdxExtractRecordedCartArchiveSha256(const char* dataDirC) {
    if (dataDirC == nullptr || dataDirC[0] == '\0') {
        return {};
    }
    return loadSidecar(fs::path(dataDirC)).archiveSha256; // fzerox.o2r container SHA-256 (key archive_sha256)
}

std::string GdxExtractRecordedIplArchiveSha256(const char* dataDirC) {
    if (dataDirC == nullptr || dataDirC[0] == '\0') {
        return {};
    }
    return loadSidecar(fs::path(dataDirC)).iplArchiveSha256; // n64ddipl.o2r container SHA-256
}

bool GdxExtractQuarantineArchive(const char* dataDirC, const char* archiveNameC) {
    if (dataDirC == nullptr || dataDirC[0] == '\0' || archiveNameC == nullptr || archiveNameC[0] == '\0') {
        return false;
    }
    // Allowlist: only the three port-generated archive names may ever be renamed. This is a public
    // cross-TU API called with a caller-supplied name, so without the gate a bug or a future caller
    // could pass an arbitrary path component and rename a file the port never generated.
    const std::string requested(archiveNameC);
    if (requested != kArchiveName && requested != kIplArchiveName && requested != kDiskArchiveName) {
        gdx_port_logf("[extract] ERROR: refusing to quarantine '%s' -- not a recognized port-generated "
                      "archive name.\n", archiveNameC);
        return false;
    }
    // Mirrors the quarantine in ensureCartArchive: rename to <name>.bad so the mount path can never
    // pick up a container that failed verification. NEVER deletes user media -- only the
    // port-generated .o2r is touched. Overwrites a prior .bad.
    const fs::path a = fs::path(dataDirC) / archiveNameC;
    if (!fileExists(a)) {
        return false;
    }
    const fs::path bad = fs::path(dataDirC) / (std::string(archiveNameC) + ".bad");
    std::error_code qec;
    fs::remove(bad, qec);
    qec.clear();
    fs::rename(a, bad, qec);
    if (qec) {
        gdx_port_logf("[extract] WARNING: could not quarantine %s (%s).\n", archiveNameC,
                      qec.message().c_str());
        return false;
    }
    gdx_port_logf("[extract] quarantined %s as %s.bad (failed verification).\n", archiveNameC, archiveNameC);
    return true;
}

// ── Per-boot archive validation latch (see header) ──────────────────────────────────────────────────
// File-static and main-thread only: this TU's ensure* warm-boot checks and gdx_firstboot.cpp's
// *ArchiveSatisfies helpers all run synchronously on the main thread during startup, well before the
// async extraction driver could touch this state.
namespace {
bool gArchiveValidatedThisBoot[3] = { false, false, false };
}

void GdxExtractMarkArchiveValidated(GdxExtractArchiveKind kind) {
    gArchiveValidatedThisBoot[static_cast<int>(kind)] = true;
}

bool GdxExtractIsArchiveValidatedThisBoot(GdxExtractArchiveKind kind) {
    return gArchiveValidatedThisBoot[static_cast<int>(kind)];
}

std::string GdxExtractSha256Bytes(const void* data, unsigned long long len) {
    if (data == nullptr || len == 0) {
        return {};
    }
    Sha256Ctx ctx;
    sha256Init(ctx);
    sha256Update(ctx, static_cast<const std::uint8_t*>(data), static_cast<size_t>(len));
    std::uint8_t out[32];
    sha256Final(ctx, out);
    return toHex(out, 32);
}

} // namespace gdx
