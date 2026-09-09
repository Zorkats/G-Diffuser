// G-Diffuser — first-boot setup + per-user data directory resolution. See gdx_firstboot.h.
//
// This TU is part of the G-Diffuser exe target (not the decomp game library), so it may freely use
// the host CRT, <filesystem>, and the Win32 common-dialog picker (Comdlg32 is already linked for
// rom_buffer.cpp's picker). It runs before libultraship is constructed, so it logs through the port's
// own gdx_port_logf and touches no LUS state.

#include "gdx_firstboot.h"
#include "gdx_extract_launch.h" // GdxExtractFileSha256 / GdxExtractRecordManagedDisk
#include "port_log.h"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <commdlg.h>
#include <cwchar>
#else
#include <cstdlib>    // getenv
#include <unistd.h>   // readlink
#endif

namespace fs = std::filesystem;

namespace gdx {
namespace {

// ── Structural validation constants ──────────────────────────────────────────────────────────────
constexpr std::uintmax_t kRomMinBytes = 16u * 1024u * 1024u;   // F-Zero X images are 16 MiB.
constexpr std::uintmax_t kDiskExactBytes = 64931840u;          // Retail/translated 64DD image size.
constexpr std::uintmax_t kIplMinBytes = 4u * 1024u * 1024u;    // 64DD IPL dumps are 4 MiB.

// Canonical on-disk names inside the data directory. These match what the existing loaders search
// for (rom_buffer.cpp / disk_buffer.cpp), so copying a user's pick to these names lets the stock
// resolvers find it with no further wiring.
constexpr const char* kRomName = "baserom.us.rev0.z64";
constexpr const char* kDiskName = "baserom.translated.ek.ndd";
constexpr const char* kIplName = "N64DDIPLROM.n64";
constexpr const char* kGameArchiveName = "fzerox.o2r";
// The dedicated IPL archive (gdx_extract_launch.cpp's kIplArchiveName / main.cpp's mount group).
// Only a presence probe for the missing-input diagnostic needs it, so it stays local rather than
// becoming a cross-TU export.
constexpr const char* kIplArchiveName = "n64ddipl.o2r";
// The dedicated EK disk archive (gdx_extract_launch.cpp's kDiskArchiveName / main.cpp's mount group).
// A valid fzerox-disk.o2r satisfies the disk requirement exactly like the managed copy does
// (gdx_disk_load is archive-first once it is mounted), so the raw .ndd AND the managed copy become
// deletable once a boot has reconstructed and verified from it.
constexpr const char* kDiskArchiveName = "fzerox-disk.o2r";
// The dev-tree default output name (tools/gen_f3d_o2r.py / Torch's default), still used unrenamed by
// the in-tree archive at assets/extracted/generic.o2r. GdxFirstBootDescribeMissing accepts either
// name as satisfying the game-archive input.
constexpr const char* kGameArchiveDevName = "generic.o2r";

// ── Known-good SHA-1 identity tables (region/dump recognition) ──────────────────────────────────────
// Named tables so the future JP build can reuse the same sets (it would accept kRomSha1JpRev0 as
// VerifiedKnown and reject the US hash with the mirror message). All lowercase hex. The US-rev0 ROM's
// accepted hash is deliberately NOT duplicated here: it lives in decomp-recipes/config.yml (recipes
// are the single source of truth) and is read at runtime via GdxExtractExpectedRomSha1.
constexpr const char* kRomSha1JpRev0 = "a418b0151521b76691fa03f8658c8b567c69498b"; // F-Zero X (Japan)
// Alternate canonical filenames for the Japanese dumps, accepted directly so a JP test folder needs
// no renaming. The JP ROM boots RAW (experimental; the US recipe tree cannot extract it, so no
// archives exist for it) — see GdxRecognizeInput's jpRom handling and FirstBootRun's raw-boot branch.
constexpr const char* kRomNameJp = "baserom.jp.rev0.z64";
constexpr const char* kDiskNameJp = "baserom.jp.ek.ndd";

constexpr const char* kRomSha1PalRev0 = "af9038bcb543a2b4faaab876dbdab4663859e092"; // F-Zero X (Europe)
// Alternate canonical filenames for the PAL dumps, accepted directly so a PAL test folder needs no
// renaming. PAL extraction uses the pal/rev0 recipe tree and produces fzerox-pal.o2r.
constexpr const char* kRomNamePal = "baserom.pal.rev0.z64";
constexpr const char* kRomNameEuPal = "baserom.eu.rev0.z64";
// Accepted alternate filename for the US prototype IPL dump, so a folder holding it under its
// original dump name needs no renaming — see SetupIplFileNameUsProto() in gdx_firstboot.h.
constexpr const char* kIplNameUsProto = "64DD_IPL_US_MJR.n64";
struct KnownDiskDump {
    const char* sha1;
    const char* label;  // shown as the OK row header: "OK (<label>)"
    bool japanese;      // Japanese-region dump — refused unless kAllowJapaneseInputs (see below)
};
constexpr KnownDiskDump kKnownDiskDumps[] = {
    { "fde9fa6f29a52be0144bda74caf8583c036c20ce", "translated Expansion Kit disk", false },
    { "7e8badf857f1fce8aa59307c0fd318128c44418b", "retail Japanese Expansion Kit disk", true },
};
// Known-good 64DD IPL dumps (mirrors KnownDiskDump). The US prototype and JP retail dumps are
// labelled distinctly so the setup UI never confuses the two.
struct KnownIplDump {
    const char* sha1;
    const char* label; // shown as the OK row header: "OK (<label>)"
};
constexpr KnownIplDump kKnownIplDumps[] = {
    { "bf861922dcb78c316360e3e742f4f70ff63c9bc3", "64DD IPL (Japan retail)" },       // N64DDIPLROM.n64
    { "3c5b93ca231550c68693a14f03cea8d5dbd1be9e", "64DD IPL (USA prototype)" },      // 64DD_IPL_US_MJR.n64
};

// ── Japanese-region input gate (CMake option GDX_ALLOW_JP_INPUTS, default OFF) ───────────────────
// The full rationale lives with the option in port/CMakeLists.txt. Short version: this binary
// accepts the Japanese cartridge dump today on an EXPERIMENTAL raw boot with no asset extraction,
// and that boot was OBSERVED in hands-on testing to produce loud static and blasting sounds (the
// cause is unconfirmed; the leading suspect is US-profile asset binding offsets applied to a
// Japanese image). That is an ear-safety hazard, so a plain (release) configure refuses Japanese
// game data outright instead of half-booting it.
//
// This is a GATE, NOT A REMOVAL. Japanese support ships in a later release, so every piece of JP
// identification below stays intact — kRomSha1JpRev0, the retail-JP row in kKnownDiskDumps, and the
// JP alternate filenames (kRomNameJp/kDiskNameJp, still probed by the wizard) — precisely so the
// refusal can say "this is the Japanese release" instead of "unrecognised file".
//
// A constexpr bool rather than an #ifdef at each call site, so both paths keep compiling in both
// configurations (the JP path cannot bit-rot behind the gate) and the compiler folds the dead branch.
#ifdef GDX_ALLOW_JP_INPUTS
constexpr bool kAllowJapaneseInputs = true;
#else
constexpr bool kAllowJapaneseInputs = false;
#endif

// Refusal text shown verbatim on the wizard's row. Deliberately plain and non-alarming: the user's
// file is not corrupt or suspect, this build simply does not enable the region yet.
constexpr const char* kJpRefusedRomMessage =
    "This is the Japanese release of F-Zero X. Japanese-region support is not enabled in this "
    "build of G-Diffuser — it is planned for a later release.\n"
    "Please use the North American (US rev0) cartridge dump.";
constexpr const char* kJpRefusedDiskMessage =
    "This is the retail Japanese Expansion Kit disk. Japanese-region support is not enabled in "
    "this build of G-Diffuser — it is planned for a later release.\n"
    "Please use the English (translated) Expansion Kit disk image.";

// Subdirectory of dataDir holding the managed disk copy, under the same canonical leaf name so it
// satisfies gdx_disk_load's search AND preserves the existing .gdd save key, which derives from the
// leaf name only (see ManagedDiskPath in gdx_firstboot.h).
constexpr const char* kManagedMediaSubdir = "media";

// ROM candidate names probed for DEV-layout detection (mirrors rom_buffer.cpp's next-to-exe list).
const char* const kRomDevCandidates[] = { "baserom.us.rev0.z64", "baserom.pal.rev0.z64", "baserom.eu.rev0.z64", "fzerox.z64", "f-zero-x.z64" };

// ── Path helpers ─────────────────────────────────────────────────────────────────────────────────

fs::path executableDir(const char* argv0) {
    std::error_code ec;
#ifdef _WIN32
    wchar_t buf[MAX_PATH] = {};
    DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
        fs::path p(buf);
        return p.parent_path();
    }
#else
    char buf[4096];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n > 0) {
        buf[n] = '\0';
        fs::path p(buf);
        return p.parent_path();
    }
#endif
    if (argv0 != nullptr && argv0[0] != '\0') {
        fs::path p = fs::absolute(fs::path(argv0), ec);
        if (!ec) {
            return p.parent_path();
        }
    }
    return fs::current_path(ec);
}

// G-Diffuser is always portable: the game folder is the data directory on every platform, and
// AppData/XDG are never touched.

bool fileExists(const fs::path& p) {
    std::error_code ec;
    return !p.empty() && fs::is_regular_file(p, ec);
}

bool developmentTreeProvidesArchive(const fs::path& exeDir, const fs::path& cwd) {
    for (const fs::path& base : { exeDir, cwd }) {
        fs::path probe = base;
        for (int up = 0; up < 6 && !probe.empty(); ++up, probe = probe.parent_path()) {
            if (fileExists(probe / "assets" / "extracted" / "generic.o2r")) {
                return true;
            }
            if (probe == probe.root_path()) {
                break;
            }
        }
    }
    return false;
}

std::uintmax_t fileSize(const fs::path& p) {
    std::error_code ec;
    std::uintmax_t s = fs::file_size(p, ec);
    return ec ? 0u : s;
}

// True if the file starts with the big-endian z64 magic (80 37 12 40).
bool hasZ64Magic(const fs::path& p) {
    FILE* f = nullptr;
#ifdef _MSC_VER
    if (fopen_s(&f, p.string().c_str(), "rb") != 0) {
        return false;
    }
#else
    f = fopen(p.string().c_str(), "rb");
#endif
    if (f == nullptr) {
        return false;
    }
    unsigned char m[4] = {};
    size_t got = fread(m, 1, 4, f);
    fclose(f);
    return got == 4 && m[0] == 0x80 && m[1] == 0x37 && m[2] == 0x12 && m[3] == 0x40;
}

// Country code from the standard N64 ROM header (offset 0x3E — 'J' Japan, 'E' North America).
// Only meaningful for a NATIVE big-endian .z64 image, so the magic word is re-checked here and a
// byte/word-swapped dump reports 0 rather than a garbage code. One 64-byte read; no hashing.
char romHeaderCountryCode(const fs::path& p) {
    FILE* f = nullptr;
#ifdef _MSC_VER
    if (fopen_s(&f, p.string().c_str(), "rb") != 0) {
        return '\0';
    }
#else
    f = fopen(p.string().c_str(), "rb");
#endif
    if (f == nullptr) {
        return '\0';
    }
    unsigned char hdr[0x40] = {};
    size_t got = fread(hdr, 1, sizeof(hdr), f);
    fclose(f);
    if (got != sizeof(hdr) || hdr[0] != 0x80 || hdr[1] != 0x37 || hdr[2] != 0x12 || hdr[3] != 0x40) {
        return '\0';
    }
    return static_cast<char>(hdr[0x3E]);
}

// True when `p` is a Japanese-region cartridge image AND this build does not accept Japanese inputs.
// This catches every Japanese dump, including ones whose SHA-1 this build has never seen, which is
// why it is worth having alongside GdxRecognizeInput's exact-hash check. One 64-byte read, cheap
// enough for the dev-tree boot path.
bool romIsRefusedJapanese(const fs::path& p) {
    return !kAllowJapaneseInputs && romHeaderCountryCode(p) == 'J';
}

bool validateRom(const fs::path& p, std::string& why) {
    if (fileSize(p) < kRomMinBytes) {
        why = "not a complete 16 MiB image";
        return false;
    }
    if (!hasZ64Magic(p)) {
        why = "missing big-endian .z64 magic (80 37 12 40) — is this a byte-swapped .n64/.v64?";
        return false;
    }
    return true;
}

bool validateDisk(const fs::path& p, std::string& why) {
    std::uintmax_t s = fileSize(p);
    if (s != kDiskExactBytes) {
        why = "wrong size for a 64DD disk image (expected exactly 64,931,840 bytes)";
        return false;
    }
    return true;
}

bool validateIpl(const fs::path& p, std::string& why) {
    if (fileSize(p) < kIplMinBytes) {
        why = "too small for a 64DD IPL ROM (expected >= 4 MiB)";
        return false;
    }
    return true;
}

bool copyInto(const fs::path& src, const fs::path& dstDir, const char* dstName) {
    std::error_code ec;
    fs::path dst = dstDir / dstName;
    fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        gdx_port_logf("[firstboot] ERROR copying %s -> %s: %s\n", src.string().c_str(),
                      dst.string().c_str(), ec.message().c_str());
        return false;
    }
    return true;
}

// ── Managed disk copy (disk internalization) ────────────────────────────────────────────────────
fs::path managedDiskPath(const fs::path& dataDir) {
    return dataDir / kManagedMediaSubdir / kDiskName;
}

// `validatedSrc` MUST already have passed validateDisk. Idempotent: a managed copy already present
// and correctly sized is left untouched, never re-copied or overwritten, so an "ensure" call on a
// warm boot cannot damage a good copy. True iff a valid managed copy exists at `outDst` on return.
bool ensureManagedDiskCopy(const fs::path& dataDir, const fs::path& validatedSrc, fs::path& outDst) {
    std::error_code ec;
    fs::path mediaDir = dataDir / kManagedMediaSubdir;
    outDst = mediaDir / kDiskName;

    if (fileExists(outDst) && fileSize(outDst) == kDiskExactBytes) {
        return true; // already present and correctly sized -- idempotent no-op, no re-copy/hash.
    }

    std::error_code eqEc;
    if (fs::equivalent(validatedSrc, outDst, eqEc) && !eqEc) {
        // The "source" IS the managed copy already (e.g. a DevLayout fallback probe resolved to it).
        return fileSize(outDst) == kDiskExactBytes;
    }

    if (fileSize(validatedSrc) != kDiskExactBytes) {
        gdx_port_logf("[firstboot] cannot create managed disk copy: %s is not a valid disk image\n",
                      validatedSrc.string().c_str());
        return false;
    }

    fs::create_directories(mediaDir, ec);
    ec.clear();

    // Atomic-ish install: copy to a temp name in the SAME directory as the destination, verify size,
    // then rename into place. A crash mid-copy leaves only an orphaned .tmp file, never a truncated
    // file at the canonical managed name that a later boot could mistake for valid.
    fs::path tmp = mediaDir / (std::string(kDiskName) + ".tmp");
    fs::copy_file(validatedSrc, tmp, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        gdx_port_logf("[firstboot] ERROR copying managed disk %s -> %s: %s\n", validatedSrc.string().c_str(),
                      tmp.string().c_str(), ec.message().c_str());
        std::error_code rmEc;
        fs::remove(tmp, rmEc);
        return false;
    }
    if (fileSize(tmp) != kDiskExactBytes) {
        gdx_port_logf(
            "[firstboot] ERROR: managed disk copy staged at %s has the wrong size (%llu bytes); discarding\n",
            tmp.string().c_str(), static_cast<unsigned long long>(fileSize(tmp)));
        std::error_code rmEc;
        fs::remove(tmp, rmEc);
        return false;
    }
    fs::rename(tmp, outDst, ec);
    if (ec) {
        // Cross-device rename or a transient sharing lock (unlikely -- same directory). Fall back to
        // a non-atomic copy+remove rather than leaving a good temp file stranded.
        std::error_code copyEc;
        fs::copy_file(tmp, outDst, fs::copy_options::overwrite_existing, copyEc);
        std::error_code rmEc;
        fs::remove(tmp, rmEc);
        if (copyEc) {
            gdx_port_logf("[firstboot] ERROR: could not install managed disk copy at %s: %s\n",
                          outDst.string().c_str(), copyEc.message().c_str());
            return false;
        }
        // Cross-device copy has no atomic-rename guarantee, so re-verify the destination size the
        // same way the pre-rename staging copy was verified above before trusting it as valid.
        if (fileSize(outDst) != kDiskExactBytes) {
            gdx_port_logf(
                "[firstboot] ERROR: managed disk copy installed at %s has the wrong size (%llu bytes); discarding\n",
                outDst.string().c_str(), static_cast<unsigned long long>(fileSize(outDst)));
            std::error_code rmDstEc;
            fs::remove(outDst, rmDstEc);
            return false;
        }
    }
    gdx_port_logf("[firstboot] managed disk copy created: %s (%llu bytes)\n", outDst.string().c_str(),
                  static_cast<unsigned long long>(kDiskExactBytes));

    // Diagnostic bookkeeping only: the file on disk is always the source of truth, and a failure here
    // never fails the copy. GdxExtractFileSha256 streams the hash, so this is a one-time ~150 ms cost
    // paid only at copy-creation time.
    std::string sha = GdxExtractFileSha256(outDst.string().c_str());
    if (!sha.empty()) {
        GdxExtractRecordManagedDisk(dataDir.string().c_str(), sha.c_str(),
                                    static_cast<unsigned long long>(kDiskExactBytes));
    } else {
        gdx_port_logf(
            "[firstboot] WARNING: could not SHA-256 the managed disk copy for the sidecar (non-fatal; "
            "the copy itself is still valid)\n");
    }
    return true;
}

// ── Simple key=value state file (independent of libultraship Config load order) ──────────────────

fs::path stateFilePath(const fs::path& dataDir) {
    return dataDir / "gdx_firstboot.cfg";
}

struct SetupState {
    bool complete = false;
    std::string romPath;
    std::string diskPath;
    std::string iplPath;
    // SHA-1 of the ROM recorded at setup completion. Lets the completed-setup fast path recognize a
    // Japanese-ROM raw-boot install (which legitimately has NO fzerox.o2r) without weakening the
    // game-archive gate for US installs (an interrupted US extraction must still re-run setup).
    std::string romSha1;
};

SetupState loadState(const fs::path& dataDir) {
    SetupState st;
    FILE* f = nullptr;
    std::string path = stateFilePath(dataDir).string();
#ifdef _MSC_VER
    if (fopen_s(&f, path.c_str(), "rb") != 0) {
        f = nullptr;
    }
#else
    f = fopen(path.c_str(), "rb");
#endif
    if (f == nullptr) {
        return st;
    }
    char line[4096];
    while (fgets(line, sizeof(line), f) != nullptr) {
        std::string s(line);
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) {
            s.pop_back();
        }
        size_t eq = s.find('=');
        if (eq == std::string::npos) {
            continue;
        }
        std::string key = s.substr(0, eq);
        std::string val = s.substr(eq + 1);
        if (key == "Setup.Complete") {
            st.complete = (val == "1");
        } else if (key == "Game.RomPath") {
            st.romPath = val;
        } else if (key == "Game.DiskPath") {
            st.diskPath = val;
        } else if (key == "Game.DdIplPath") {
            st.iplPath = val;
        } else if (key == "Game.RomSha1") {
            st.romSha1 = val;
        }
    }
    fclose(f);
    return st;
}

bool saveState(const fs::path& dataDir, const SetupState& st) {
    FILE* f = nullptr;
    std::string path = stateFilePath(dataDir).string();
#ifdef _MSC_VER
    if (fopen_s(&f, path.c_str(), "wb") != 0) {
        f = nullptr;
    }
#else
    f = fopen(path.c_str(), "wb");
#endif
    if (f == nullptr) {
        gdx_port_logf("[firstboot] WARNING: could not write %s; setup will re-run next launch\n",
                      path.c_str());
        return false;
    }
    fprintf(f, "# G-Diffuser first-boot state. Auto-generated; safe to delete to re-run setup.\n");
    fprintf(f, "Setup.Complete=%d\n", st.complete ? 1 : 0);
    fprintf(f, "Game.RomPath=%s\n", st.romPath.c_str());
    fprintf(f, "Game.DiskPath=%s\n", st.diskPath.c_str());
    fprintf(f, "Game.DdIplPath=%s\n", st.iplPath.c_str());
    fprintf(f, "Game.RomSha1=%s\n", st.romSha1.c_str());
    fclose(f);
    return true;
}

// ── Native pickers / dialogs (Win32; graceful no-op elsewhere) ───────────────────────────────────

#ifdef _WIN32
fs::path pickFile(const wchar_t* title, const wchar_t* filter) {
    wchar_t fileName[MAX_PATH] = {};
    OPENFILENAMEW ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = nullptr;
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = fileName;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = title;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (GetOpenFileNameW(&ofn)) {
        return fs::path(fileName);
    }
    return {};
}
#endif

// Records the acquire-time SHA-256 of the IPL ROM into the extraction sidecar so one file documents
// every verified input. Diagnostic only — a hash failure never fails setup. The dedicated IPL
// extraction step later refreshes ipl_sha256 to the byte-order-normalized identity; for the common
// native (z64) dump the two are equal. Idempotent, so calling it on every boot is fine.
void recordIplIdentity(const fs::path& dataDir, const fs::path& iplPath) {
    if (iplPath.empty() || !fileExists(iplPath)) {
        return;
    }
    std::string sha = GdxExtractFileSha256(iplPath.string().c_str());
    if (!sha.empty()) {
        GdxExtractRecordIpl(dataDir.string().c_str(), sha.c_str());
    }
}

// True when a valid disk archive (fzerox-disk.o2r) is installed under `dataDir`. When the completion
// sidecar recorded a disk_archive_sha256, the file must hash to it before it is accepted, so a
// container swapped for a stale/foreign one is rejected and the disk falls back to the managed copy /
// original. With no recorded hash (sidecar absent or older) acceptance degrades to presence. The
// few-hundred-ms hash runs only on the archive-only disk path, where the original .ndd and the
// managed media/ copy are both already gone.
bool diskArchiveSatisfies(const fs::path& dataDir) {
    fs::path diskArchive = dataDir / kDiskArchiveName;
    if (!fileExists(diskArchive)) {
        return false;
    }
    if (GdxExtractIsArchiveValidatedThisBoot(GdxExtractArchiveKind::Disk)) {
        return true; // already hash-verified this boot (earlier satisfies call) — skip the re-hash.
    }
    std::string recorded = GdxExtractRecordedDiskArchiveSha256(dataDir.string().c_str());
    if (recorded.empty()) {
        return true; // no recorded golden — accept presence (unchanged pre-sidecar behavior).
    }
    std::string actual = GdxExtractFileSha256(diskArchive.string().c_str());
    if (actual.empty()) {
        // Hash READ failure (open/IO error), not a content mismatch: a transient error must not
        // destroy state, so reject for this boot without quarantining the (possibly valid) archive.
        gdx_port_logf("[firstboot] %s could not be hashed (read error); treating as unsatisfied this "
                      "boot without quarantining\n", kDiskArchiveName);
        return false;
    }
    if (actual == recorded) {
        // Latch so ensureDiskArchive's warm-boot check, run later this same boot from
        // GdxExtractEnsureArchive, does not re-hash the same file.
        GdxExtractMarkArchiveValidated(GdxExtractArchiveKind::Disk);
        return true;
    }
    // A mismatched container must be quarantined so main.cpp's presence-based mount list
    // (findArchivePaths) cannot pick it up; otherwise the disk stays mounted archive-first while the
    // wizard nags about setup on top of an already-half-working boot. ensureDiskArchive rebuilds
    // automatically from any source resolveDiskSource accepts (translated/JP .ndd, or the managed
    // copy); with none present, setup re-runs.
    gdx_port_logf(
        "[firstboot] %s failed verification (does not match the recorded disk_archive_sha256); "
        "quarantining it — it is rebuilt automatically when a disk image is available, otherwise "
        "re-run setup with the Expansion Kit disk\n",
        kDiskArchiveName);
    GdxExtractQuarantineArchive(dataDir.string().c_str(), kDiskArchiveName);
    return false;
}

// True when a valid cart archive is installed under `dataDir`. Mirrors diskArchiveSatisfies: against a
// recorded archive_sha256 the file must hash to it, and a container swapped for a stale/foreign/corrupt
// one is rejected and quarantined (renamed <name>.bad) so the wizard re-runs and rebuilds it from the
// original ROM. With no recorded hash, acceptance degrades to presence. The expected archive name is
// driven by the recorded profile so US, JP-raw (no archive), and PAL installs each resolve their own
// container.
bool gameArchiveSatisfies(const fs::path& dataDir) {
    const std::string profile = GdxExtractRecordedCartProfile(dataDir.string().c_str());
    const char* archiveName = kGameArchiveName;
    if (profile == "jp/rev0") {
        archiveName = "fzerox-jp.o2r";
    } else if (profile == "pal/rev0") {
        archiveName = "fzerox-pal.o2r";
    }
    fs::path archive = dataDir / archiveName;
    if (!fileExists(archive)) {
        return false;
    }
    if (GdxExtractIsArchiveValidatedThisBoot(GdxExtractArchiveKind::Cart)) {
        return true; // already hash-verified this boot (earlier satisfies call) — skip the re-hash.
    }
    std::string recorded = GdxExtractRecordedCartArchiveSha256(dataDir.string().c_str());
    if (recorded.empty()) {
        return true; // no recorded golden — accept presence (unchanged pre-sidecar behavior).
    }
    std::string actual = GdxExtractFileSha256(archive.string().c_str());
    if (actual.empty()) {
        gdx_port_logf("[firstboot] %s could not be hashed (read error); treating as unsatisfied this "
                      "boot without quarantining\n", archiveName);
        return false;
    }
    if (actual == recorded) {
        // Latch so ensureCartArchive's warm-boot check, run later this same boot from
        // GdxExtractEnsureArchive, does not re-hash the same file.
        GdxExtractMarkArchiveValidated(GdxExtractArchiveKind::Cart);
        return true;
    }
    gdx_port_logf("[firstboot] %s failed verification (does not match the recorded archive_sha256); "
                  "quarantining it — re-run setup with the original ROM to rebuild it\n", archiveName);
    GdxExtractQuarantineArchive(dataDir.string().c_str(), archiveName);
    return false;
}

// True when a valid IPL archive (n64ddipl.o2r) is installed under `dataDir`. Same contract as
// gameArchiveSatisfies against the recorded ipl_archive_sha256; quarantines a mismatched container.
bool iplArchiveSatisfies(const fs::path& dataDir) {
    fs::path archive = dataDir / kIplArchiveName;
    if (!fileExists(archive)) {
        return false;
    }
    if (GdxExtractIsArchiveValidatedThisBoot(GdxExtractArchiveKind::Ipl)) {
        return true; // already hash-verified this boot (earlier satisfies call) — skip the re-hash.
    }
    std::string recorded = GdxExtractRecordedIplArchiveSha256(dataDir.string().c_str());
    if (recorded.empty()) {
        return true; // no recorded golden — accept presence (legacy fallback).
    }
    std::string actual = GdxExtractFileSha256(archive.string().c_str());
    if (actual.empty()) {
        gdx_port_logf("[firstboot] %s could not be hashed (read error); treating as unsatisfied this "
                      "boot without quarantining\n", kIplArchiveName);
        return false;
    }
    if (actual == recorded) {
        // Latch so ensureIplArchive's warm-boot check, run later this same boot from
        // GdxExtractEnsureArchive, does not re-hash the same file.
        GdxExtractMarkArchiveValidated(GdxExtractArchiveKind::Ipl);
        return true;
    }
    gdx_port_logf("[firstboot] %s failed verification (does not match the recorded ipl_archive_sha256); "
                  "quarantining it — re-run setup with the original IPL to rebuild it\n", kIplArchiveName);
    GdxExtractQuarantineArchive(dataDir.string().c_str(), kIplArchiveName);
    return false;
}

} // namespace

bool GdxFirstbootArchiveSatisfies(GdxFirstbootArchiveKind kind, const std::string& dataDir) {
    const fs::path dir(dataDir);
    switch (kind) {
        case GdxFirstbootArchiveKind::Game: return gameArchiveSatisfies(dir);
        case GdxFirstbootArchiveKind::Ipl:  return iplArchiveSatisfies(dir);
        case GdxFirstbootArchiveKind::Disk: return diskArchiveSatisfies(dir);
    }
    return false;
}

FirstBootResult FirstBootRun(const char* argv0) {
    FirstBootResult result;
    std::error_code ec;

    const fs::path exeDir = executableDir(argv0);
    const fs::path cwd = fs::current_path(ec);
    if (ec) {
        ec.clear();
    }
    // Recorded on every return path (dev, warm, wizard, abort): the runtime O2R extractor reads its
    // packaged gdx-extract child + decomp-recipes from here.
    result.exeDir = exeDir.string();

    // A ROM beside the executable is normal for a portable release and must not bypass first-time
    // setup. Preserve the headless shortcut only for a real source tree that already provides the
    // development generic.o2r archive.
    if (developmentTreeProvidesArchive(exeDir, cwd)) {
        fs::path romCand;
        for (const char* cand : kRomDevCandidates) {
            for (const fs::path& dir : { exeDir, cwd }) {
                fs::path p = dir / cand;
                if (fileExists(p)) {
                    romCand = p;
                    break;
                }
            }
            if (!romCand.empty()) {
                break;
            }
        }
        // The dev fast path hashes nothing (size + z64 magic only), so a Japanese dump sitting under
        // any of kRomDevCandidates' US filenames would boot HEADLESSLY here, past the wizard and past
        // every message. Refuse it by ROM-header country code before anything else in this branch
        // runs, and fall through to the in-window wizard — that is the surface that can explain why.
        if (!romCand.empty() && romIsRefusedJapanese(romCand)) {
            gdx_port_logf(
                "[firstboot] development tree ROM %s is the JAPANESE release; Japanese-region "
                "support is not enabled in this build (configure with -DGDX_ALLOW_JP_INPUTS=ON to "
                "enable it). Refusing the headless dev boot and deferring to the setup screen.\n",
                romCand.string().c_str());
            romCand.clear();
        }
        if (!romCand.empty()) {
            // G-Diffuser is Expansion-Kit-MANDATORY: the game is built EK-only and crashes/degrades
            // without the 64DD disk, so the dev fast path may not bypass the wizard unless a valid EK
            // disk AND IPL are also present. Same direct locations as the ROM candidate search above
            // (exeDir, cwd — no parent walk; only developmentTreeProvidesArchive's generic.o2r probe
            // walks parents).
            fs::path diskCand, iplCand;
            std::string diskWhy, iplWhy;
            bool diskFound = false, iplFound = false;
            for (const fs::path& dir : { exeDir, cwd }) {
                if (diskCand.empty()) {
                    fs::path d = dir / kDiskName;
                    if (fileExists(d)) {
                        diskFound = true;
                        if (validateDisk(d, diskWhy)) {
                            diskCand = d;
                        }
                    }
                }
                if (iplCand.empty()) {
                    // Canonical name, then the accepted US-prototype alt name; shared with
                    // ensureIplArchive and the wizard's Recheck() (see gdx_firstboot.h).
                    std::string found = GdxFindIplSourceInDir(dir.string());
                    if (!found.empty()) {
                        fs::path i(found);
                        iplFound = true;
                        if (validateIpl(i, iplWhy)) {
                            iplCand = i;
                        }
                    }
                }
            }
            // Nothing here CREATES a managed media/ copy: fzerox-disk.o2r is the port's stable copy,
            // and extraction reads the original directly (resolveDiskSource falls back past media/ to
            // the data/exe dirs), so staging 64MB the archive immediately makes redundant is waste.
            // media/ copies from older installs stay honored everywhere they already were — the
            // accept loop below, disk_buffer.cpp's search order, the deletion-gate hash fallback.
            if (diskCand.empty()) {
                for (const fs::path& dir : { exeDir, cwd }) {
                    fs::path managed = managedDiskPath(dir);
                    std::string managedWhy;
                    if (fileExists(managed) && validateDisk(managed, managedWhy)) {
                        diskCand = managed;
                        diskFound = true;
                        gdx_port_logf(
                            "[firstboot] no original EK disk found, but a valid managed copy exists "
                            "at %s; using it\n",
                            managed.string().c_str());
                        break;
                    }
                }
            }

            if (!diskCand.empty() && !iplCand.empty()) {
                result.status = FirstBootStatus::DevLayout;
                result.romPath = fs::absolute(romCand, ec).string();
                result.diskPath = fs::absolute(diskCand, ec).string();
                result.iplPath = fs::absolute(iplCand, ec).string();
                result.dataDir = exeDir.string();
                recordIplIdentity(exeDir, iplCand); // acquire-time IPL identity
                gdx_port_logf("[firstboot] development tree: ROM=%s disk=%s ipl=%s; setup not required\n",
                              result.romPath.c_str(), result.diskPath.c_str(), result.iplPath.c_str());
                return result;
            }
            // Say WHY each validator failed (wrong size, too small) rather than "missing or invalid",
            // so a present-but-corrupt disk/IPL is distinguishable from an absent one.
            std::string reasonSuffix;
            if (diskCand.empty()) {
                reasonSuffix += " disk: ";
                reasonSuffix += diskFound ? diskWhy : "missing";
            }
            if (iplCand.empty()) {
                reasonSuffix += " ipl: ";
                reasonSuffix += iplFound ? iplWhy : "missing";
            }
            gdx_port_logf(
                "[firstboot] development tree found ROM=%s but the EK disk (%s) and/or IPL (%s) are "
                "missing or invalid; G-Diffuser requires the Expansion Kit, so falling through to the "
                "in-window setup instead of a headless boot%s\n",
                romCand.string().c_str(), kDiskName, kIplName, reasonSuffix.c_str());
        }
    }

    // ── Wizard mode: always portable ─────────────────────────────────────────────────────────────
    // The port never writes to a per-user directory (AppData / XDG data). Everything it creates — the
    // extracted fzerox.o2r, saves/, ghosts/, config, requested diagnostics — lives beside the
    // executable, so the whole installation is one folder that moves, backs up, or deletes as a unit.
    fs::path dataDir = exeDir;
    fs::create_directories(dataDir, ec);
    ec.clear();
    result.dataDir = dataDir.string();

    // Move the working directory into the data dir so config, the disk image, and the IPL ROM
    // (which resolve relative to the CWD in libultraship / disk_buffer.cpp) consolidate there.
    if (fs::current_path(dataDir, ec); !ec) {
        result.chdirApplied = true;
        gdx_port_logf("[firstboot] data directory: %s (working directory set)\n",
                      dataDir.string().c_str());
    } else {
        ec.clear();
        gdx_port_logf("[firstboot] WARNING: could not set working directory to %s\n",
                      dataDir.string().c_str());
    }

    SetupState st = loadState(dataDir);

    // Fast path: setup previously completed AND every input is still satisfied. Each canonical input
    // runs an ACCEPTANCE CHAIN, in order: its validated installed ARCHIVE (so the ORIGINAL is
    // deletable and the game boots archive-only, as rom_buffer.cpp / disk_buffer.cpp already do), else
    // the managed media/ copy (disk only), else the original file. Demanding the original ROM and IPL
    // here would drag the wizard back on a fully-installed setup whose originals were deleted.
    fs::path romInData = dataDir / kRomName;
    fs::path diskInData = dataDir / kDiskName;
    if (st.complete) {
        std::string why;
        // Canonical name OR the accepted US-prototype alt name, never just the canonical one. This is
        // also what recovers a stale st.iplPath/Game.DdIplPath entry: the probe re-derives the path
        // from the data dir every boot instead of trusting the recorded value.
        std::string iplFoundPath = GdxFindIplSourceInDir(dataDir.string());
        fs::path iplCheck = iplFoundPath.empty() ? (dataDir / kIplName) : fs::path(iplFoundPath);
        fs::path managedDisk = managedDiskPath(dataDir);
        bool managedDiskValid = fileExists(managedDisk) && fileSize(managedDisk) == kDiskExactBytes;

        // The game asset archive is an independent gate: an interrupted extraction (marker written but
        // fzerox.o2r never produced/validated) must stay in the setup flow rather than boot the raw
        // fallback forever. Being required anyway, it doubles as the ROM input's archive satisfier — a
        // present-AND-VERIFIED fzerox.o2r means the ROM boots archive-only. The re-hash against the
        // recorded archive_sha256 is what makes a bit-rotted/swapped container trigger the wizard.
        bool gameArchiveValid = gameArchiveSatisfies(dataDir);

        // ROM: verified fzerox.o2r (archive-only boot) OR the original US-rev0 ROM.
        bool romSatisfied = gameArchiveValid || (fileExists(romInData) && validateRom(romInData, why));
        // IPL: verified n64ddipl.o2r (gdx_ddipl_load is archive-first, re-hashed against the recorded
        // ipl_archive_sha256, corrupt one quarantined) OR the original IPL ROM.
        bool iplSatisfied = iplArchiveSatisfies(dataDir) || fileExists(iplCheck);
        // Disk: original .ndd OR managed media/ copy OR fzerox-disk.o2r validated against the sidecar
        // disk_archive_sha256 where available (falls back to presence when the sidecar has no hash).
        bool diskSatisfied = fileExists(diskInData) || managedDiskValid || diskArchiveSatisfies(dataDir);

        // Japanese-ROM raw-boot install (experimental): setup completed with the JP dump, for which no
        // fzerox.o2r can exist. It requires the recorded JP ROM itself, re-hashed every boot so a
        // swapped file cannot ride the recorded acceptance, plus the same IPL/disk chains as the US
        // path. An interrupted US extraction cannot reach this branch: those record the US hash or
        // none, never kRomSha1JpRev0.
        //
        // With Japanese inputs disabled the recorded-JP-hash fast path is off, or a gdx_firstboot.cfg
        // written by a JP-enabled dev build (or carried over in a copied game folder) would keep
        // booting the Japanese ROM raw in a release binary with no wizard and no message. Refusing
        // drops through to the "input unsatisfied" branch below and the wizard's ROM row states the
        // real reason.
        bool jpRawBootSatisfied = false;
        if (!gameArchiveValid && st.romSha1 == kRomSha1JpRev0 && !st.romPath.empty() &&
            fileExists(fs::path(st.romPath)) && iplSatisfied && diskSatisfied) {
            if (kAllowJapaneseInputs) {
                jpRawBootSatisfied = (GdxExtractFileSha1(st.romPath.c_str()) == kRomSha1JpRev0);
            } else {
                gdx_port_logf(
                    "[firstboot] this install was set up with the JAPANESE ROM (%s), but "
                    "Japanese-region support is not enabled in this build; re-running setup "
                    "(configure with -DGDX_ALLOW_JP_INPUTS=ON to enable it)\n",
                    st.romPath.c_str());
            }
        }
        if (jpRawBootSatisfied) {
            result.status = FirstBootStatus::SetupComplete;
            result.romPath = st.romPath;
            gdx_port_logf("[firstboot] setup complete; booting F-Zero X (Japan) RAW from %s "
                          "(experimental; no asset archives for the Japanese version)\n",
                          result.romPath.c_str());
            return result;
        }

        if (gameArchiveValid && romSatisfied && iplSatisfied && diskSatisfied) {
            result.status = FirstBootStatus::SetupComplete;
            // Point the caller at the ORIGINAL ROM only when it still exists: rom_buffer.cpp's
            // CLI-arg branch would otherwise be handed a dead path. When the original is gone the ROM
            // is served from fzerox.o2r — leave romPath empty for the archive-only boot (main.cpp
            // threads `archivesValidated` into gdx_init_rom for exactly this case).
            result.romPath = fileExists(romInData) ? fs::absolute(romInData, ec).string() : std::string();
            if (fileExists(iplCheck)) {
                recordIplIdentity(dataDir, iplCheck); // keep the acquire-time IPL identity fresh
            }
            // disk_buffer.cpp is ARCHIVE-FIRST — a mounted fzerox-disk.o2r reconstructs the image
            // before the managed-copy/original search — so report "archive" whenever one is present,
            // even if the managed copy also exists.
            const char* diskSource = fileExists(dataDir / kDiskArchiveName) ? "archive"
                                     : (managedDiskValid ? "managed copy" : "original");
            if (result.romPath.empty()) {
                gdx_port_logf("[firstboot] setup complete; booting archive-only (ROM served from %s; "
                              "disk source: %s)\n", kGameArchiveName, diskSource);
            } else {
                gdx_port_logf("[firstboot] setup complete; booting with configured ROM %s (disk source: %s)\n",
                              result.romPath.c_str(), diskSource);
            }
            return result;
        }
        gdx_port_logf(
            "[firstboot] setup was marked complete but an input is unsatisfied "
            "(rom=%d ipl=%d disk=%d game-archive=%d); re-running setup\n",
            romSatisfied ? 1 : 0, iplSatisfied ? 1 : 0, diskSatisfied ? 1 : 0, gameArchiveValid ? 1 : 0);
        st.complete = false;
    }

    // ── Needs setup ────────────────────────────────────────────────────────────────────────────────
    // Both fast paths missed: the required inputs are absent or invalid. Acquisition happens IN-WINDOW,
    // so resolve nothing further here — main() proceeds through Context/window init without a ROM and
    // then runs the ImGui setup flow (port/gdx_firstboot_gui.{h,cpp}), which reuses the exported
    // validators/copy/state helpers below. result.romPath stays empty until the GUI installs a ROM.
    result.status = FirstBootStatus::NeedsSetup;
    gdx_port_logf("[firstboot] required inputs missing; deferring to the in-window setup flow (%s)\n",
                  dataDir.string().c_str());
    return result;
}

bool DevelopmentTreeProvidesArchive(const std::string& exeDir, const std::string& cwd) {
    return developmentTreeProvidesArchive(fs::path(exeDir), fs::path(cwd));
}

// ── Exported setup helpers (shared with the in-window GUI setup flow) ─────────────────────────────

const char* SetupRomFileName() {
    return kRomName;
}

const char* SetupDiskFileName() {
    return kDiskName;
}

const char* SetupIplFileName() {
    return kIplName;
}

const char* SetupIplFileNameUsProto() {
    return kIplNameUsProto;
}

std::string GdxFindIplSourceInDir(const std::string& dir) {
    std::error_code ec;
    fs::path canonical = fs::path(dir) / kIplName;
    if (fs::is_regular_file(canonical, ec)) {
        return canonical.string();
    }
    ec.clear();
    fs::path alt = fs::path(dir) / kIplNameUsProto;
    if (fs::is_regular_file(alt, ec)) {
        return alt.string();
    }
    return std::string();
}

const char* SetupRomFileNameJp() {
    return kRomNameJp;
}

const char* SetupDiskFileNameJp() {
    return kDiskNameJp;
}

const char* SetupRomFileNamePal() {
    return kRomNamePal;
}

const char* SetupRomFileNameEuPal() {
    return kRomNameEuPal;
}

bool ValidateRomFile(const std::string& path, std::string& why) {
    return validateRom(fs::path(path), why);
}

bool ValidateDiskFile(const std::string& path, std::string& why) {
    return validateDisk(fs::path(path), why);
}

bool ValidateIplFile(const std::string& path, std::string& why) {
    return validateIpl(fs::path(path), why);
}

bool CopyInputInto(const std::string& srcPath, const std::string& dataDir, const char* dstName) {
    return copyInto(fs::path(srcPath), fs::path(dataDir), dstName);
}

const char* SetupGameArchiveFileName() {
    return kGameArchiveName;
}

const char* SetupIplArchiveFileName() {
    return kIplArchiveName;
}

const char* SetupDiskArchiveFileName() {
    return kDiskArchiveName;
}

GdxInputRecognition GdxRecognizeInput(const std::string& canonicalName, const std::string& path,
                                      const std::string& exeDir) {
    GdxInputRecognition r;
    r.sha1 = GdxExtractFileSha1(path.c_str());
    if (r.sha1.empty()) {
        r.verdict = GdxInputVerdict::Rejected;
        r.message = "could not read the file to calculate its SHA-1";
        return r;
    }

    // ROM — STRICT: this build accepts the US-rev0 and PAL-rev0 dumps; the JP dump gets a precise message.
    if (canonicalName == kRomName) {
        const std::string expectedUs = GdxExtractExpectedRomSha1(exeDir.c_str()); // recipe-authoritative
        if (!expectedUs.empty() && r.sha1 == expectedUs) {
            r.verdict = GdxInputVerdict::VerifiedKnown;
            r.message = "SHA-1 verified: " + r.sha1;
        } else if (r.sha1 == kRomSha1JpRev0) {
            if (!kAllowJapaneseInputs) {
                // The dump is RECOGNIZED — the whole point of keeping kRomSha1JpRev0 — so the row
                // names it precisely instead of falling through to the generic mismatch text below.
                r.verdict = GdxInputVerdict::Rejected;
                r.message = kJpRefusedRomMessage;
                return r;
            }
            // Accepted, but it boots RAW: the US recipe tree cannot extract it, so setup skips
            // archive extraction for this ROM entirely — no fzerox.o2r, no archive-only mode, and the
            // ROM file is not deletable.
            r.verdict = GdxInputVerdict::AcceptedUnknownWarn;
            r.jpRom = true;
            r.okHeaderOverride = "OK (F-Zero X (Japan) — experimental)";
            r.message = "Japanese ROM accepted (SHA-1 verified). Experimental: boots directly from "
                        "the ROM; asset extraction and archive features are unavailable for the "
                        "Japanese version, so keep this ROM file in place.";
        } else if (r.sha1 == kRomSha1PalRev0) {
            // PAL is extracted with the pal/rev0 recipe tree and produces fzerox-pal.o2r. Unlike JP,
            // it supports archive-only boot once extraction succeeds.
            r.verdict = GdxInputVerdict::VerifiedKnown;
            r.okHeaderOverride = "OK (F-Zero X (PAL) — experimental)";
            r.message = "SHA-1 verified: " + r.sha1;
        } else if (romIsRefusedJapanese(fs::path(path))) {
            // A Japanese dump this build has no hash for (alternate dump, trimmed/overdumped image, an
            // uncatalogued revision). The header country code still identifies it, so say WHY it is
            // refused rather than "this is not the US cartridge".
            r.verdict = GdxInputVerdict::Rejected;
            r.message = std::string(kJpRefusedRomMessage) + "\n(identified from the ROM header "
                        "country code; SHA-1 " + r.sha1 + ")";
        } else {
            r.verdict = GdxInputVerdict::Rejected;
            r.message = "SHA-1 mismatch — this dump is not the US rev0 cartridge\nyours:    " + r.sha1 +
                        "\nexpected: " + expectedUs;
        }
        return r;
    }

    // Alternate PAL canonical names are also accepted directly (same recognition as the main row).
    if (canonicalName == kRomNamePal || canonicalName == kRomNameEuPal) {
        if (r.sha1 == kRomSha1PalRev0) {
            r.verdict = GdxInputVerdict::VerifiedKnown;
            r.okHeaderOverride = "OK (F-Zero X (PAL) — experimental)";
            r.message = "SHA-1 verified: " + r.sha1;
        } else {
            r.verdict = GdxInputVerdict::Rejected;
            r.message = "SHA-1 mismatch — this dump is not the PAL rev0 cartridge\nyours:    " + r.sha1 +
                        "\nexpected: " + std::string(kRomSha1PalRev0);
        }
        return r;
    }

    // IPL — accept any correctly-sized dump; label the known-good ones (JP retail, US prototype), warn
    // on the rest.
    if (canonicalName == kIplName) {
        for (const KnownIplDump& d : kKnownIplDumps) {
            if (r.sha1 == d.sha1) {
                r.verdict = GdxInputVerdict::VerifiedKnown;
                r.okHeaderOverride = d.label;
                r.message = std::string("Recognized ") + d.label + " — SHA-1 verified: " + r.sha1;
                return r;
            }
        }
        r.verdict = GdxInputVerdict::AcceptedUnknownWarn;
        r.message = "Unrecognized IPL dump (SHA-1 " + r.sha1 +
                    ") — proceeding, but this dump is untested.";
        return r;
    }

    // Disk — the hard size gate already passed inside ValidateDiskFile; label the known region dumps.
    if (canonicalName == kDiskName) {
        for (const KnownDiskDump& d : kKnownDiskDumps) {
            if (r.sha1 == d.sha1) {
                if (d.japanese && !kAllowJapaneseInputs) {
                    // Recognized, then refused, so the row names the disk rather than reporting an
                    // unknown image.
                    r.verdict = GdxInputVerdict::Rejected;
                    r.message = kJpRefusedDiskMessage;
                    return r;
                }
                r.verdict = GdxInputVerdict::VerifiedKnown;
                r.okHeaderOverride = d.label;
                r.message = std::string("OK (") + d.label + ") — SHA-1 " + r.sha1;
                return r;
            }
        }
        // An UNRECOGNIZED but correctly-sized image stays accepted-with-warning in both gate states.
        // A 64DD image carries no usable region marker (the fan-translated disk is itself a modified
        // Japanese disk, so any region field still reads as Japan), so "unknown" cannot be narrowed to
        // "Japanese" without guessing, and refusing every unknown dump would break legitimate
        // re-dumps. The identity gate stays exact-hash only.
        r.verdict = GdxInputVerdict::AcceptedUnknownWarn;
        r.message = "Unrecognized Expansion Kit disk (SHA-1 " + r.sha1 +
                    ") — proceeding, but this dump is untested.";
        return r;
    }

    r.verdict = GdxInputVerdict::VerifiedKnown;
    r.message = "SHA-1: " + r.sha1;
    return r;
}

std::string ManagedDiskPath(const std::string& dataDir) {
    return managedDiskPath(fs::path(dataDir)).string();
}

bool EnsureManagedDiskCopy(const std::string& dataDir, const std::string& validatedDiskPath,
                           std::string& outManagedPath) {
    fs::path dst;
    bool ok = ensureManagedDiskCopy(fs::path(dataDir), fs::path(validatedDiskPath), dst);
    outManagedPath = dst.string();
    return ok;
}

bool WriteSetupComplete(const std::string& dataDir, const std::string& romPath,
                        const std::string& diskPath, const std::string& iplPath) {
    SetupState st;
    st.complete = true;
    st.romPath = romPath;
    st.diskPath = diskPath;
    st.iplPath = iplPath;
    // Lets the completed-setup fast path recognize a Japanese-ROM raw-boot install (no fzerox.o2r ever
    // exists for one) without weakening the US game-archive gate.
    st.romSha1 = GdxExtractFileSha1(romPath.c_str());
    // Best-effort; the dedicated IPL extraction step refreshes it to the normalized identity later.
    recordIplIdentity(fs::path(dataDir), fs::path(iplPath));
    return saveState(fs::path(dataDir), st);
}

// ── Missing-input diagnostic (see doc comment in gdx_firstboot.h) ─────────────────────────────────
std::string GdxFirstBootDescribeMissing(const std::string& dataDirIn) {
    std::error_code ec;
    fs::path dataDir = dataDirIn.empty() ? fs::current_path(ec) : fs::path(dataDirIn);
    ec.clear();

    std::vector<std::string> lines;
    std::string why;

    fs::path romPath = dataDir / kRomName;
    if (!fileExists(romPath)) {
        lines.push_back(std::string(kRomName) + " (F-Zero X ROM): missing.");
    } else if (!validateRom(romPath, why)) {
        lines.push_back(std::string(kRomName) + " (F-Zero X ROM): invalid -- " + why);
    }

    // The raw file OR the dedicated archive satisfies this input: gdx_ddipl_load is archive-first once
    // n64ddipl.o2r is mounted.
    fs::path iplPath = dataDir / kIplName;
    bool iplArchivePresent = fileExists(dataDir / kIplArchiveName);
    if (fileExists(iplPath)) {
        if (!validateIpl(iplPath, why)) {
            lines.push_back(std::string(kIplName) + " (64DD IPL ROM): invalid -- " + why);
        }
    } else if (!iplArchivePresent) {
        lines.push_back(std::string(kIplName) + " (64DD IPL ROM): missing, and no " +
                        kIplArchiveName + " archive found either.");
    }

    // The original OR the managed copy under media/ OR the dedicated disk archive satisfies this
    // input; gdx_disk_load resolves archive → managed copy → original.
    fs::path diskPath = dataDir / kDiskName;
    fs::path managedDisk = managedDiskPath(dataDir);
    bool managedDiskOk = fileExists(managedDisk) && fileSize(managedDisk) == kDiskExactBytes;
    bool diskArchiveOk = fileExists(dataDir / kDiskArchiveName);
    if (fileExists(diskPath)) {
        if (!validateDisk(diskPath, why) && !managedDiskOk && !diskArchiveOk) {
            lines.push_back(std::string(kDiskName) + " (Expansion Kit disk): invalid -- " + why);
        }
    } else if (!managedDiskOk && !diskArchiveOk) {
        lines.push_back(std::string(kDiskName) +
                        " (Expansion Kit disk): missing, and no managed copy in media/ or " +
                        kDiskArchiveName + " either.");
    }

    // Game asset archive (installed name OR the dev-tree default name).
    bool gameArchivePresent =
        fileExists(dataDir / kGameArchiveName) || fileExists(dataDir / kGameArchiveDevName);
    if (!gameArchivePresent) {
        lines.push_back(std::string(kGameArchiveName) +
                        " (game asset archive): not built yet -- run the setup wizard to extract it "
                        "from your ROM.");
    }

    if (lines.empty()) {
        return {};
    }
    std::string out = "Setup is incomplete -- the following need attention:\n";
    for (const std::string& line : lines) {
        out += "  - " + line + "\n";
    }
    return out;
}

bool NativeFilePickerAvailable() {
#ifdef _WIN32
    return true;
#else
    return false;
#endif
}

#ifdef _WIN32
std::string PickRomFile() {
    fs::path p = pickFile(L"Select your F-Zero X ROM (US rev0, .z64)",
                          L"Nintendo 64 ROMs (*.z64;*.n64;*.v64)\0*.z64;*.n64;*.v64\0All files\0*.*\0");
    return p.string();
}

std::string PickDiskFile() {
    fs::path p = pickFile(L"Select your F-Zero X Expansion Kit disk (.ndd)",
                          L"64DD disk images (*.ndd)\0*.ndd\0All files\0*.*\0");
    return p.string();
}

std::string PickIplFile() {
    fs::path p = pickFile(L"Select your 64DD IPL ROM (N64DDIPLROM.n64)",
                          L"64DD IPL ROM (*.n64;*.z64)\0*.n64;*.z64\0All files\0*.*\0");
    return p.string();
}
#else
std::string PickRomFile() {
    return {};
}
std::string PickDiskFile() {
    return {};
}
std::string PickIplFile() {
    return {};
}
#endif

} // namespace gdx
