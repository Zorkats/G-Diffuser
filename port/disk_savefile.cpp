/* port/disk_savefile.cpp -- durable 64DD disk-save sidecar implementation.
 *
 * See disk_savefile.h for the format, validation rules, and boundary rationale.
 *
 * Sidecar file layout (all multi-byte fields little-endian, host-independent):
 *   Header (24 bytes):
 *     0   4   magic "GDD1"
 *     4   4   format version (currently 1)
 *     8   8   sourceDiskHash  -- CRC64 of the pristine .ndd bytes
 *     16  4   recordCount
 *     20  4   fileCrc32       -- CRC32 over every byte except these 4
 *   Records (recordCount of them, packed back to back):
 *     0   4   byteOffset
 *     4   4   length
 *     8   len data
 *
 * Records are the coalesced dirty byte ranges: overlapping and adjacent ranges
 * are merged so one game save (a bounded write burst from Mfs_SaveFile /
 * Mfs_BackupRamArea) collapses into the smallest set of non-overlapping records.
 */

#define _CRT_SECURE_NO_WARNINGS /* plain fopen/fwrite below; harmless on non-MSVC */

#include "disk_savefile.h"
#include "port_log.h"
#include "libultraship/bridge/consolevariablebridge.h" /* one-shot DD-format CVar (Workshop menu) */
#include "ship/Context.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <sys/stat.h>
#include <cerrno>
#endif

namespace {

constexpr unsigned kHeaderSize = 24;
constexpr uint32_t kFormatVersion = 1u;
/* Idle host frames (about half a second at 60 Hz) before the journal is flushed. A user
 * save is a bounded write burst, so waiting it out coalesces it into one sidecar write. */
constexpr int kDebounceFrames = 30;

struct DirtyRange {
    uint32_t offset;
    std::vector<uint8_t> data;
};

/* One coalesced journal, keyed to the pristine disk it was built against. */
uint64_t g_sourceHash = 0;
unsigned g_diskSize = 0;
bool g_active = false;
std::vector<DirtyRange> g_ranges; /* sorted by offset, non-overlapping */

bool g_pendingFlush = false;
int g_idleFrames = 0;

std::string g_gddPath;
std::string g_bakPath;
std::string g_tmpPath;

/* Workshop-menu status (read-only introspection; see the getters at the bottom). */
bool g_sidecarPresent = false;      /* a valid .gdd/.bak was loaded this boot */
bool g_lastFlushOk = true;
bool g_formatRefusedThisBoot = false; /* MFS uninitialized AND format refused this boot */

/* --- CRC helpers ------------------------------------------------------------ */

/* CRC-32 (zlib polynomial 0xEDB88320), reflected, table-based. Same algorithm
 * as port/gdx_ghost_io.c's container CRC. */
uint32_t crc32(const unsigned char* data, size_t length) {
    static uint32_t table[256];
    static bool ready = false;
    if (!ready) {
        for (unsigned n = 0; n < 256; n++) {
            uint32_t c = n;
            for (int k = 0; k < 8; k++) {
                c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            }
            table[n] = c;
        }
        ready = true;
    }
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < length; i++) {
        crc = table[(crc ^ data[i]) & 0xFFu] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
}

/* CRC-64/XZ (ECMA-182 polynomial, reflected form 0xC96C5795D7870F42), used as a
 * cheap collision-resistant fingerprint of the pristine disk image. */
uint64_t crc64(const unsigned char* data, size_t length) {
    static uint64_t table[256];
    static bool ready = false;
    if (!ready) {
        for (unsigned n = 0; n < 256; n++) {
            uint64_t c = n;
            for (int k = 0; k < 8; k++) {
                c = (c & 1u) ? (0xC96C5795D7870F42ull ^ (c >> 1)) : (c >> 1);
            }
            table[n] = c;
        }
        ready = true;
    }
    uint64_t crc = 0xFFFFFFFFFFFFFFFFull;
    for (size_t i = 0; i < length; i++) {
        crc = table[(crc ^ data[i]) & 0xFFu] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFFFFFFFFFull;
}

/* --- little-endian pack/unpack ---------------------------------------------- */

void putU32(std::vector<uint8_t>& buf, uint32_t v) {
    buf.push_back((uint8_t)(v & 0xFFu));
    buf.push_back((uint8_t)((v >> 8) & 0xFFu));
    buf.push_back((uint8_t)((v >> 16) & 0xFFu));
    buf.push_back((uint8_t)((v >> 24) & 0xFFu));
}

void putU64(std::vector<uint8_t>& buf, uint64_t v) {
    for (int i = 0; i < 8; i++) {
        buf.push_back((uint8_t)((v >> (8 * i)) & 0xFFu));
    }
}

uint32_t getU32(const unsigned char* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

uint64_t getU64(const unsigned char* p) {
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) {
        v |= (uint64_t)p[i] << (8 * i);
    }
    return v;
}

/* --- path resolution (mirrors gdx_ghost_io.c) ------------------------------- */

bool savesDirectory(std::string& out) {
    std::error_code error;
    std::filesystem::path savesPath = Ship::Context::GetPathRelativeToAppDirectory("saves");

    std::filesystem::create_directories(savesPath, error);
    if (error) {
        return false;
    }
    out = savesPath.string();
    return true;
}

void migrateLegacySidecars(const std::string& canonicalGddPath, const std::string& canonicalBakPath) {
#ifndef _WIN32
    std::error_code error;
    const std::filesystem::path canonicalGdd(canonicalGddPath);
    const std::filesystem::path canonicalBak(canonicalBakPath);
    const bool canonicalGddExists = std::filesystem::exists(canonicalGdd, error);
    if (error) {
        return;
    }
    const bool canonicalBakExists = std::filesystem::exists(canonicalBak, error);
    if (error || canonicalGddExists || canonicalBakExists) {
        return;
    }

    const std::filesystem::path currentDirectory = std::filesystem::current_path(error);
    if (error) {
        return;
    }
    const std::filesystem::path legacyDirectory = currentDirectory / "saves";
    const std::filesystem::path absoluteLegacyDirectory = std::filesystem::absolute(legacyDirectory, error);
    if (error) {
        return;
    }
    const std::filesystem::path absoluteCanonicalDirectory =
        std::filesystem::absolute(canonicalGdd.parent_path(), error);
    if (error || absoluteLegacyDirectory.lexically_normal() == absoluteCanonicalDirectory.lexically_normal()) {
        return;
    }

    const std::filesystem::path legacyGdd = legacyDirectory / canonicalGdd.filename();
    const std::filesystem::path legacyBak = legacyGdd.string() + ".bak";

    if (std::filesystem::is_regular_file(legacyGdd, error) &&
        std::filesystem::copy_file(legacyGdd, canonicalGdd, std::filesystem::copy_options::none, error)) {
        gdx_port_logf("[disk-save] migrated legacy sidecar %s to %s\n", legacyGdd.string().c_str(),
                      canonicalGdd.string().c_str());
    }
    error.clear();
    if (std::filesystem::is_regular_file(legacyBak, error) &&
        std::filesystem::copy_file(legacyBak, canonicalBak, std::filesystem::copy_options::none, error)) {
        gdx_port_logf("[disk-save] migrated legacy backup %s to %s\n", legacyBak.string().c_str(),
                      canonicalBak.string().c_str());
    }
#else
    (void) canonicalGddPath;
    (void) canonicalBakPath;
#endif
}

std::string baseName(const char* name) {
    if (name == nullptr) {
        return std::string();
    }
    std::string s(name);
    size_t pos = s.find_last_of("/\\");
    return (pos == std::string::npos) ? s : s.substr(pos + 1);
}

/* --- journal coalescing ----------------------------------------------------- */

void insertRange(uint32_t offset, const unsigned char* data, uint32_t len) {
    if (len == 0) {
        return;
    }
    uint64_t newStart = offset;
    uint64_t newEnd = (uint64_t)offset + len;

    /* Ranges are kept sorted by offset, so everything overlapping or touching the new
     * range is one contiguous run and can be collapsed in a single pass. */
    size_t first = g_ranges.size();
    size_t last = 0;
    bool any = false;
    for (size_t i = 0; i < g_ranges.size(); i++) {
        uint64_t rs = g_ranges[i].offset;
        uint64_t re = rs + g_ranges[i].data.size();
        if (re >= newStart && rs <= newEnd) { /* overlapping or adjacent */
            if (!any) {
                first = i;
                any = true;
            }
            last = i;
            if (rs < newStart) {
                newStart = rs;
            }
            if (re > newEnd) {
                newEnd = re;
            }
        }
    }

    DirtyRange merged;
    merged.offset = (uint32_t)newStart;
    merged.data.assign((size_t)(newEnd - newStart), 0);

    if (any) {
        for (size_t i = first; i <= last; i++) {
            const DirtyRange& r = g_ranges[i];
            memcpy(&merged.data[r.offset - newStart], r.data.data(), r.data.size());
        }
    }
    /* Overlay the freshly written bytes last so they win over stale content. */
    memcpy(&merged.data[offset - newStart], data, len);

    if (any) {
        g_ranges.erase(g_ranges.begin() + first, g_ranges.begin() + last + 1);
        g_ranges.insert(g_ranges.begin() + first, std::move(merged));
    } else {
        size_t pos = 0;
        while (pos < g_ranges.size() && g_ranges[pos].offset < merged.offset) {
            pos++;
        }
        g_ranges.insert(g_ranges.begin() + pos, std::move(merged));
    }
}

/* --- sidecar load ----------------------------------------------------------- */

enum class LoadStatus { Loaded, Missing, BadMagic, BadVersion, HashMismatch, BadCrc, BadRange, Io };

LoadStatus loadSidecar(const std::string& path, std::vector<DirtyRange>& out) {
    FILE* f = fopen(path.c_str(), "rb");
    if (f == nullptr) {
        return LoadStatus::Missing;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < (long)kHeaderSize) {
        fclose(f);
        return LoadStatus::Io;
    }
    std::vector<uint8_t> buf((size_t)sz);
    if (fread(buf.data(), 1, (size_t)sz, f) != (size_t)sz) {
        fclose(f);
        return LoadStatus::Io;
    }
    fclose(f);

    if (memcmp(buf.data(), "GDD1", 4) != 0) {
        return LoadStatus::BadMagic;
    }
    if (getU32(&buf[4]) != kFormatVersion) {
        return LoadStatus::BadVersion;
    }
    uint64_t storedHash = getU64(&buf[8]);
    uint32_t recordCount = getU32(&buf[16]);
    uint32_t storedCrc = getU32(&buf[20]);

    /* The CRC covers every byte except the 4 CRC bytes at offset 20, so recompute over a
     * scratch copy with that field zeroed -- the same shape flush builds. */
    std::vector<uint8_t> scratch(buf);
    scratch[20] = scratch[21] = scratch[22] = scratch[23] = 0;
    if (crc32(scratch.data(), scratch.size()) != storedCrc) {
        return LoadStatus::BadCrc;
    }
    if (storedHash != g_sourceHash) {
        return LoadStatus::HashMismatch;
    }

    std::vector<DirtyRange> parsed;
    size_t pos = kHeaderSize;
    for (uint32_t i = 0; i < recordCount; i++) {
        if (pos + 8 > buf.size()) {
            return LoadStatus::Io;
        }
        uint32_t off = getU32(&buf[pos]);
        uint32_t len = getU32(&buf[pos + 4]);
        pos += 8;
        if (len == 0 || pos + len > buf.size()) {
            return LoadStatus::Io;
        }
        if ((uint64_t)off + len > g_diskSize) {
            return LoadStatus::BadRange;
        }
        DirtyRange r;
        r.offset = off;
        r.data.assign(buf.begin() + pos, buf.begin() + pos + len);
        parsed.push_back(std::move(r));
        pos += len;
    }
    out.swap(parsed);
    return LoadStatus::Loaded;
}

const char* statusText(LoadStatus s) {
    switch (s) {
    case LoadStatus::BadMagic: return "bad magic";
    case LoadStatus::BadVersion: return "unsupported version";
    case LoadStatus::HashMismatch: return "disk fingerprint mismatch";
    case LoadStatus::BadCrc: return "CRC mismatch";
    case LoadStatus::BadRange: return "record out of disk range";
    case LoadStatus::Io: return "truncated or unreadable";
    default: return "unknown";
    }
}

} // namespace

extern "C" {

unsigned long long gdx_disk_crc64(const unsigned char* data, unsigned long long length) {
    return crc64(data, (size_t)length);
}

void gdx_disk_save_init(const char* diskName, const unsigned char* pristine, unsigned int size) {
    g_active = false;
    g_ranges.clear();
    g_pendingFlush = false;
    g_idleFrames = 0;
    g_sourceHash = 0;
    g_diskSize = size;
    g_sidecarPresent = false;
    /* Reset per boot/disk so a stale FAILED from a prior disk cannot leak into this one.
     * The getter is binary, so "no flush attempted yet" and "last flush succeeded" both
     * read as ok in the Workshop panel. */
    g_lastFlushOk = true;

    if (pristine == nullptr || size == 0) {
        gdx_port_logf("[disk-save] no disk image; durable disk save disabled\n");
        return;
    }

    g_sourceHash = crc64(pristine, size);

    std::string dir;
    if (!savesDirectory(dir)) {
        gdx_port_logf("[disk-save] WARNING: could not resolve saves directory; disk writes will not persist\n");
        return;
    }
    std::string leaf = baseName(diskName);
    if (leaf.empty()) {
        leaf = "disk";
    }
#ifdef _WIN32
    const char sep = '\\';
#else
    const char sep = '/';
#endif
    g_gddPath = dir + sep + leaf + ".gdd";
    g_bakPath = g_gddPath + ".bak";
    g_tmpPath = g_gddPath + ".tmp";
    migrateLegacySidecars(g_gddPath, g_bakPath);
    g_active = true;

    /* Primary sidecar, else the rolled backup, else pristine-only. A sidecar whose
     * fingerprint does not match this pristine image is NEVER applied. */
    LoadStatus primary = loadSidecar(g_gddPath, g_ranges);
    if (primary == LoadStatus::Loaded) {
        g_sidecarPresent = true;
        gdx_port_logf("[disk-save] loaded sidecar %s: %zu record(s), disk fingerprint OK\n", g_gddPath.c_str(),
                      g_ranges.size());
        return;
    }
    if (primary != LoadStatus::Missing) {
        gdx_port_logf("[disk-save] sidecar %s rejected (%s); trying backup\n", g_gddPath.c_str(),
                      statusText(primary));
    }

    LoadStatus backup = loadSidecar(g_bakPath, g_ranges);
    if (backup == LoadStatus::Loaded) {
        g_sidecarPresent = true;
        gdx_port_logf("[disk-save] recovered from backup %s: %zu record(s), disk fingerprint OK\n", g_bakPath.c_str(),
                      g_ranges.size());
        return;
    }
    g_ranges.clear();
    if (primary == LoadStatus::Missing && backup == LoadStatus::Missing) {
        gdx_port_logf("[disk-save] no disk save found, pristine (sidecar: %s)\n", g_gddPath.c_str());
    } else {
        gdx_port_logf("[disk-save] backup %s unusable (%s); pristine-only\n", g_bakPath.c_str(),
                      statusText(backup == LoadStatus::Missing ? primary : backup));
    }
}

void gdx_disk_save_apply(unsigned char* buffer) {
    if (!g_active || buffer == nullptr || g_ranges.empty()) {
        return;
    }
    size_t applied = 0;
    for (const DirtyRange& r : g_ranges) {
        if ((uint64_t)r.offset + r.data.size() > g_diskSize) {
            continue; /* defensive: validated on load, but never write past the image */
        }
        memcpy(buffer + r.offset, r.data.data(), r.data.size());
        applied++;
    }
    gdx_port_logf("[disk-save] applied %zu record(s) to disk image\n", applied);
}

void gdx_disk_save_mark_dirty(unsigned int offset, const void* data, unsigned int len) {
    if (!g_active || data == nullptr || len == 0) {
        return;
    }
    if ((uint64_t)offset + len > g_diskSize) {
        gdx_port_logf("[disk-save] WARNING: dirty range out of disk bounds (offset=%u len=%u); ignored\n", offset,
                      len);
        return;
    }
    insertRange(offset, (const unsigned char*)data, len);
    g_pendingFlush = true;
    g_idleFrames = 0;
}

void gdx_disk_save_flush(void) {
    if (!g_active) {
        return;
    }

    /* The file CRC is taken over the whole image with the CRC field zeroed, so the image
     * has to be assembled in memory before the field can be filled in. */
    std::vector<uint8_t> image;
    image.insert(image.end(), { 'G', 'D', 'D', '1' });
    putU32(image, kFormatVersion);
    putU64(image, g_sourceHash);
    putU32(image, (uint32_t)g_ranges.size());
    putU32(image, 0); /* CRC placeholder at offset 20 */
    for (const DirtyRange& r : g_ranges) {
        putU32(image, r.offset);
        putU32(image, (uint32_t)r.data.size());
        image.insert(image.end(), r.data.begin(), r.data.end());
    }
    uint32_t crc = crc32(image.data(), image.size());
    image[20] = (uint8_t)(crc & 0xFFu);
    image[21] = (uint8_t)((crc >> 8) & 0xFFu);
    image[22] = (uint8_t)((crc >> 16) & 0xFFu);
    image[23] = (uint8_t)((crc >> 24) & 0xFFu);

    FILE* f = fopen(g_tmpPath.c_str(), "wb");
    if (f == nullptr) {
        g_lastFlushOk = false;
        gdx_port_logf("[disk-save] WARNING: could not open %s for writing; save not persisted\n", g_tmpPath.c_str());
        return;
    }
    bool ok = fwrite(image.data(), 1, image.size(), f) == image.size();
    if (fflush(f) != 0) {
        ok = false;
    }
    if (fclose(f) != 0) {
        ok = false;
    }
    if (!ok) {
        g_lastFlushOk = false;
        gdx_port_logf("[disk-save] WARNING: failed writing %s; save not persisted\n", g_tmpPath.c_str());
        remove(g_tmpPath.c_str());
        return;
    }

    /* Roll the live sidecar to .bak first, so a failed replace still leaves a good copy
     * behind, then atomically move the temp into place. */
#ifdef _WIN32
    MoveFileExA(g_gddPath.c_str(), g_bakPath.c_str(), MOVEFILE_REPLACE_EXISTING);
    if (!MoveFileExA(g_tmpPath.c_str(), g_gddPath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        g_lastFlushOk = false;
        gdx_port_logf("[disk-save] WARNING: could not replace %s; save not persisted (backup intact)\n",
                      g_gddPath.c_str());
        remove(g_tmpPath.c_str());
        return;
    }
#else
    rename(g_gddPath.c_str(), g_bakPath.c_str()); /* best-effort; missing .gdd is fine */
    if (rename(g_tmpPath.c_str(), g_gddPath.c_str()) != 0) {
        g_lastFlushOk = false;
        gdx_port_logf("[disk-save] WARNING: could not replace %s; save not persisted (backup intact)\n",
                      g_gddPath.c_str());
        remove(g_tmpPath.c_str());
        return;
    }
#endif
    g_lastFlushOk = true;
    g_sidecarPresent = true;
    gdx_port_logf("[disk-save] flushed %zu record(s) to %s\n", g_ranges.size(), g_gddPath.c_str());
}

void gdx_disk_save_tick(void) {
    if (!g_active || !g_pendingFlush) {
        return;
    }
    if (++g_idleFrames < kDebounceFrames) {
        return;
    }
    g_pendingFlush = false;
    g_idleFrames = 0;
    gdx_disk_save_flush();
}

int gdx_disk_allow_format(void) {
    /* DEFAULT FALSE. With the durable sidecar in place, an unprompted format of an
     * uninitialized or foreign MFS RAM area would overwrite the user's prior saved disk
     * state. The Workshop menu's "Initialize DD save area" arms a one-shot flag that this
     * guard consumes exactly once, clearing and re-saving it so a single format runs and
     * the flag never fires again unprompted. The format lands in the in-memory image and,
     * through the dirty-range journal, in the sidecar -- never in the user's .ndd.
     *
     * CONSUME ONLY AFTER MFS PROVES THE SAVE AREA UNUSABLE: terminal invalid-volume paths
     * and the Course Edit working-directory lookup may use this authorization. Transient
     * validation passes must not consume it, or the one-shot could format from a stale cache. */
    if (CVarGetInteger("gEnhancements.Workshop.AllowDDFormatOnce", 0) != 0) {
        CVarSetInteger("gEnhancements.Workshop.AllowDDFormatOnce", 0);
        CVarSave();
        gdx_port_logf("[disk-save] one-shot DD format authorized by the Workshop menu; formatting once\n");
        return 1;
    }
    return 0;
}

void gdx_disk_log_format_refused(void) {
    /* Same terminal-only call sites as gdx_disk_allow_format, after that site observed
     * N64DD_MEDIA_NOT_INIT and the guard returned 0. Keeping the transient wiped-cache
     * first-pass validate out of here is what makes the latch truthful for the Workshop
     * button: a genuinely uninitialized disk, not a normal mount's first-pass miss. */
    g_formatRefusedThisBoot = true;
    static int logged = 0;
    if (logged) {
        return;
    }
    logged = 1;
    gdx_port_logf("[leo] MFS RAM area not initialized; refusing auto-format to protect the disk\n");
}

int gdx_disk_sidecar_present(void) {
    return g_sidecarPresent ? 1 : 0;
}

int gdx_disk_sidecar_record_count(void) {
    return static_cast<int>(g_ranges.size());
}

int gdx_disk_last_flush_ok(void) {
    return g_lastFlushOk ? 1 : 0;
}

int gdx_disk_format_refused_this_boot(void) {
    return g_formatRefusedThisBoot ? 1 : 0;
}

} // extern "C"
