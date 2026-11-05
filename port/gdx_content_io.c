/* port/gdx_content_io.c -- .gdxc course/machine export (E1) and cup-bundle export (E4).
 *
 * Format, threading, and validation contract live in gdx_content_io.h. Same host-CRT target
 * split as gdx_ghost_io.c: this TU mirrors the MFS dir-entry layout byte-for-byte and declares
 * raw externs rather than including decomp headers (which drag in the PORT/EK macro-gated
 * declarations only gdiffuser_game is compiled with). The size checks below turn mirror drift
 * into a compile error.
 */

#define _CRT_SECURE_NO_WARNINGS /* plain fopen/fread/fwrite below; harmless on non-MSVC */

#include "gdx_content_io.h"
#include "gdx_content_pack.h"
#include "gdx_course_meta.h"
#include "gdx_course_meta_sign.h"
#include "gdx_course_music.h"
#include "gdx_course_registry.h"
#include "gdx_course_settings.h"
#include "gdx_course_state.h"
#include "gdx_course_snapshot_io.h"
#include "gdx_edit_cup.h"
#include "gdx_profile_state.h"
#include "port_log.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <sys/stat.h>
#endif

/* ---------------------------------------------------------------------------------
 * Mirrored MFS structures (decomp/include/leo/mfs.h).
 * ------------------------------------------------------------------------------- */

typedef struct GdxMfsTimeFormat {
    uint8_t bytes[4];
} GdxMfsTimeFormat;

/* Mirrors MfsRamId, mfs.h:23-36. Size 0x3C. */
typedef struct GdxMfsRamId {
    char diskId[10];
    int8_t fsType[2];
    int8_t fsVersion[2];
    uint8_t attr;
    uint8_t diskType;
    char volumeName[20];
    GdxMfsTimeFormat formatDate;
    uint16_t renewalCounter;
    uint8_t destinationCode;
    int8_t reserve1;
    uint32_t checksum;
    int8_t reserve2[0xC];
} GdxMfsRamId;

/* Mirrors MfsRamDirectoryEntry, mfs.h:38-62. Size 0x30; the unioned fields are read only as
 * their file-variant members. */
typedef struct GdxMfsRamDirectoryEntry {
    uint16_t attr;
    uint16_t parentDirId;
    int8_t companyCode[2];
    int8_t gameCode[4];
    uint16_t fileAllocationTableId; /* dirId for directories; files only here */
    int32_t fileSize;               /* files only */
    char name[20];                  /* NOT necessarily NUL-terminated */
    char extension[5];              /* files only */
    uint8_t copyCount;
    uint8_t renewalCounter;
    int8_t reserve3;
    GdxMfsTimeFormat creationDate;
} GdxMfsRamDirectoryEntry;

#define GDX_MFS_DIRECTORY_ENTRY_COUNT 1112

/* Mirrors MfsRamArea, mfs.h:64-68. Size 0xE730. */
typedef struct GdxMfsRamArea {
    GdxMfsRamId id;
    uint16_t fileAllocationTable[0xB3A];
    GdxMfsRamDirectoryEntry directoryEntry[GDX_MFS_DIRECTORY_ENTRY_COUNT];
} GdxMfsRamArea;

typedef char gdx_content_size_check_ramId[(sizeof(GdxMfsRamId) == 0x3C) ? 1 : -1];
typedef char gdx_content_size_check_dirEntry[(sizeof(GdxMfsRamDirectoryEntry) == 0x30) ? 1 : -1];
typedef char gdx_content_size_check_ramArea[(sizeof(GdxMfsRamArea) == 0xE730) ? 1 : -1];
typedef char gdx_content_size_check_dirOffset[(offsetof(GdxMfsRamArea, directoryEntry) == 0x16B0) ? 1 : -1];

#define GDX_MFS_FILE_ATTR_FILE (1 << 14) /* MFS_FILE_ATTR_FILE, mfs.h:92 */

/* Decomp globals. gMfsRamArea is host-native on the port (the whole MFS RAM area is built and
 * maintained by decomp code compiled for the host). */
extern GdxMfsRamArea gMfsRamArea;
extern int32_t gDirectoryEntryCount;
extern uint16_t gWorkingDirectory;

/* EK build: ovl_i2/course_context.c:45 defines [6 * 4][EDIT_CUP_TRACK_NAME_SIZE] (21 in the port); the
 * Edit Cup occupies the first 6 slots. CRS_ENTRY.CENT keeps the stock 216-byte image and CRS_ENTRY.CENL
 * the names over 8 characters (gdx_edit_cup.h). */
extern char gEditCupTrackNames[GDX_EDIT_CUP_ROWS][GDX_EDIT_CUP_NAME_SIZE];

/* Disk-thread busy flags, decomp/src/sys/disk/75000.c:24-25. D_80794E14 = a ring-buffer op is
 * executing; D_80794E18 = the D_807C6EA8 op slot is mid-dispatch (save/load/flush/...). */
extern volatile uint8_t D_80794E14;
extern volatile uint8_t D_80794E18;

/* Real MFS loader from the decomp's leo/mfs (linked under EXPANSION_KIT; the no-op stubs in
 * port/gen/LinkStubs.c are compiled out there). On the port its disk reads resolve to
 * synchronous bcopy out of gdx_disk_buffer (port/n64_leo.c LeoReadWrite), so a menu-thread call
 * is safe exactly when the busy-guard above says the disk thread is idle. */
extern int32_t Mfs_LoadFileInDir(uint16_t dirId, char* name, char* extension, uint8_t* buf, int32_t sizeToLoad);

/* From decomp/src/overlays/expansion_kit/A8140.c. The Content Library window can be opened from
 * the Workshop menu before the disk thread has mounted MFS; this prepares the working directory
 * on demand, including the root-repair / format fallback already used by the EK browser. */
extern int32_t GdxPrepareWorkingDirectory(void);

extern int CVarGetInteger(const char* name, int32_t defaultValue);

/* ---------------------------------------------------------------------------------
 * GXC1 container: fixed 0x41-byte header, explicit little-endian, hand-packed.
 * ------------------------------------------------------------------------------- */

#define GDX_CONTENT_HEADER_SIZE 0x41
#define GDX_CONTENT_FORMAT_VERSION 1u

/* ghostSave[3] at 0x7E0 (3 x 0x3FC0 = 0xBF40), SaveCourseRecords 0x110 right after; the
 * combined strip region runs to the end of the course file. POST_1_0_SCOPING section 2
 * correction: 0xBF40 ghosts-only, 0xC050 ghosts+records -- the export doc's 0xBF50 is wrong. */
#define GDX_CONTENT_GHOST_REGION_OFFSET 0x7E0
#define GDX_CONTENT_STRIP_REGION_SIZE 0xC050

static void gdx_content_write_u32le(unsigned char* p, uint32_t v) {
    p[0] = (unsigned char) (v & 0xFFu);
    p[1] = (unsigned char) ((v >> 8) & 0xFFu);
    p[2] = (unsigned char) ((v >> 16) & 0xFFu);
    p[3] = (unsigned char) ((v >> 24) & 0xFFu);
}

static uint32_t gdx_content_fourcc(const char ext[5]) {
    return (uint32_t) (unsigned char) ext[0] | ((uint32_t) (unsigned char) ext[1] << 8) |
           ((uint32_t) (unsigned char) ext[2] << 16) | ((uint32_t) (unsigned char) ext[3] << 24);
}

/* CRC-32 (IEEE 802.3 / zlib polynomial 0xEDB88320), reflected, table-based. Same algorithm as
 * gdx_ghost_io.c and disk_savefile.cpp's container CRCs. Non-static: the E2 import validator
 * reuses it for the container payload CRC. */
uint32_t gdx_content_crc32(const unsigned char* data, size_t length) {
    static uint32_t table[256];
    static int tableReady = 0;
    uint32_t crc;
    size_t i;

    if (!tableReady) {
        uint32_t c;
        unsigned int n;
        unsigned int k;
        for (n = 0; n < 256; n++) {
            c = (uint32_t) n;
            for (k = 0; k < 8; k++) {
                if (c & 1u) {
                    c = 0xEDB88320u ^ (c >> 1);
                } else {
                    c = c >> 1;
                }
            }
            table[n] = c;
        }
        tableReady = 1;
    }

    crc = 0xFFFFFFFFu;
    for (i = 0; i < length; i++) {
        crc = table[(crc ^ (uint32_t) data[i]) & 0xFFu] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
}

/* ---------------------------------------------------------------------------------
 * Host paths + filename sanitation.
 * ------------------------------------------------------------------------------- */

static int gdx_content_executable_directory(char* outPath, size_t outCap) {
    if (outPath == NULL || outCap == 0) {
        return 0;
    }
#ifdef _WIN32
    {
        char* slash;
        DWORD n = GetModuleFileNameA(NULL, outPath, (DWORD) outCap);
        if (n == 0 || n >= outCap) {
            return 0;
        }
        slash = strrchr(outPath, '\\');
        if (slash == NULL) {
            return 0;
        }
        *slash = '\0';
        return 1;
    }
#else
    if (outCap < 2) {
        return 0;
    }
    outPath[0] = '.';
    outPath[1] = '\0';
    return 1;
#endif
}

int gdx_content_exports_directory(char* outPath, size_t outCap, int create) {
    char base[1024];
    int n;

    if (!gdx_content_executable_directory(base, sizeof(base))) {
        return 0;
    }
#ifdef _WIN32
    n = snprintf(outPath, outCap, "%s\\exports", base);
#else
    n = snprintf(outPath, outCap, "%s/exports", base);
#endif
    if (n < 0 || (size_t) n >= outCap) {
        return 0;
    }
    if (!create) {
        return 1;
    }
#ifdef _WIN32
    if (!CreateDirectoryA(outPath, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) {
        return 0;
    }
#else
    if (mkdir(outPath, 0755) != 0 && errno != EEXIST) {
        return 0;
    }
#endif
    return 1;
}

/* MFS names allow characters Windows forbids in file names (<>:"/\|?*) and may contain
 * trailing spaces/dots that Windows strips; map those to '_' so the export name round-trips
 * visibly. Empty result falls back to "unnamed". */
static void gdx_content_sanitize_filename(const char* name, char* out, size_t outCap) {
    static const char* kForbidden = "<>:\"/\\|?*";
    size_t i = 0;
    size_t o = 0;

    if (outCap == 0) {
        return;
    }
    while (name[i] != '\0' && i < 20 && o + 1 < outCap) {
        unsigned char c = (unsigned char) name[i];
        if (c < 0x20 || strchr(kForbidden, c) != NULL) {
            c = '_';
        }
        out[o++] = (char) c;
        i++;
    }
    while (o > 0 && (out[o - 1] == ' ' || out[o - 1] == '.')) {
        o--;
    }
    if (o == 0) {
        const char* fallback = "unnamed";
        for (; fallback[o] != '\0' && o + 1 < outCap; o++) {
            out[o] = fallback[o];
        }
    }
    out[o] = '\0';
}

/* ---------------------------------------------------------------------------------
 * MFS access. Both entry points walk gMfsRamArea.directoryEntry[] directly with the game's own
 * predicate (attr FILE + parent == working dir + 3-char extension prefix, matching
 * Mfs_GetFileIndex at mfs_dir_entry_util.c:145 and the browser filter at A8140.c:392) -- never
 * Mfs_GetFilesPreparation/Mfs_GetNextFileInPreparedDir, whose iterator globals are shared with
 * the EK browser (blocker B2).
 * ------------------------------------------------------------------------------- */

int gdx_content_mfs_busy(void) {
    return D_80794E14 != 0 || D_80794E18 != 0 || gdx_snapshot_busy();
}

/* The Content Library can be opened before MFS is mounted (e.g. from the Workshop menu). Wait
 * for any in-flight disk op, then run the same working-directory preparation the EK browser uses
 * so enumeration/export see the mounted filesystem instead of NO_FS. */
static int gdx_content_ensure_mounted(void) {
    if (gdx_content_mfs_busy()) {
        return GDX_CONTENT_ERR_BUSY;
    }
    if (GdxPrepareWorkingDirectory() < 0) {
        return GDX_CONTENT_ERR_NO_FS;
    }
    return GDX_CONTENT_OK;
}

static int gdx_content_classify(const GdxMfsRamDirectoryEntry* dirEntry) {
    if (memcmp(dirEntry->extension, "CRS", 3) == 0) {
        return GDX_CONTENT_TYPE_TRACK;
    }
    if (memcmp(dirEntry->extension, "CAR", 3) == 0) {
        return GDX_CONTENT_TYPE_MACHINE;
    }
    return 0;
}

/* dirEntry name/extension are fixed-width and may be unterminated; copy with an explicit NUL. */
static void gdx_content_fill_entry(const GdxMfsRamDirectoryEntry* dirEntry, GdxContentEntry* out) {
    memcpy(out->name, dirEntry->name, 20);
    out->name[20] = '\0';
    memcpy(out->extension, dirEntry->extension, 5);
    out->extension[5] = '\0';
    out->fileSize = dirEntry->fileSize;
    out->contentType = gdx_content_classify(dirEntry);
}

int gdx_content_list(GdxContentEntry* outEntries, int capacity) {
    int32_t i;
    int count = 0;

    if (outEntries == NULL || capacity < 0) {
        return GDX_CONTENT_ERR_BAD_ARGS;
    }
    {
        int rc = gdx_content_ensure_mounted();
        if (rc != GDX_CONTENT_OK) {
            return rc;
        }
    }
    for (i = 0; i < gDirectoryEntryCount && i < GDX_MFS_DIRECTORY_ENTRY_COUNT; i++) {
        const GdxMfsRamDirectoryEntry* dirEntry = &gMfsRamArea.directoryEntry[i];
        if (!(dirEntry->attr & GDX_MFS_FILE_ATTR_FILE) || dirEntry->parentDirId != gWorkingDirectory ||
            gdx_content_classify(dirEntry) == 0) {
            continue;
        }
        if (count < capacity) {
            gdx_content_fill_entry(dirEntry, &outEntries[count]);
        }
        count++;
    }
    return count;
}

/* Re-find a listed entry by name+extension so export never trusts a stale menu snapshot. */
static int gdx_content_find_entry(const GdxContentEntry* entry, GdxMfsRamDirectoryEntry* outDirEntry) {
    int32_t i;

    for (i = 0; i < gDirectoryEntryCount && i < GDX_MFS_DIRECTORY_ENTRY_COUNT; i++) {
        const GdxMfsRamDirectoryEntry* dirEntry = &gMfsRamArea.directoryEntry[i];
        if (!(dirEntry->attr & GDX_MFS_FILE_ATTR_FILE) || dirEntry->parentDirId != gWorkingDirectory) {
            continue;
        }
        /* Fixed-width compares, not strcmp: MFS names/extensions may fill their fields. */
        if (memcmp(dirEntry->name, entry->name, 20) == 0 && memcmp(dirEntry->extension, entry->extension, 5) == 0) {
            memcpy(outDirEntry, dirEntry, sizeof(*outDirEntry));
            return 1;
        }
    }
    return 0;
}

/* Mfs_LoadFileInDir validates the name with mfsStrLen, so a name that fills all 20 bytes runs on
 * into the extension field and is refused. Load through terminated copies instead. */
static int32_t gdx_content_load_entry(const GdxMfsRamDirectoryEntry* dirEntry, uint8_t* out, int32_t size) {
    char name[sizeof(dirEntry->name) + 1];
    char extension[sizeof(dirEntry->extension) + 1];

    memcpy(name, dirEntry->name, sizeof(dirEntry->name));
    name[sizeof(dirEntry->name)] = '\0';
    memcpy(extension, dirEntry->extension, sizeof(dirEntry->extension));
    extension[sizeof(dirEntry->extension)] = '\0';
    return Mfs_LoadFileInDir(gWorkingDirectory, name, extension, out, size);
}

/* ---------------------------------------------------------------------------------
 * E2 import preflight queries (gdx_content_import.c is the only caller).
 * ------------------------------------------------------------------------------- */

int gdx_content_count_extension_class(const char extensionPrefix[3]) {
    int32_t i;
    int count = 0;

    if (extensionPrefix == NULL) {
        return GDX_CONTENT_ERR_BAD_ARGS;
    }
    {
        int rc = gdx_content_ensure_mounted();
        if (rc != GDX_CONTENT_OK) {
            return rc;
        }
    }
    for (i = 0; i < gDirectoryEntryCount && i < GDX_MFS_DIRECTORY_ENTRY_COUNT; i++) {
        const GdxMfsRamDirectoryEntry* dirEntry = &gMfsRamArea.directoryEntry[i];
        if (!(dirEntry->attr & GDX_MFS_FILE_ATTR_FILE) || dirEntry->parentDirId != gWorkingDirectory) {
            continue;
        }
        if (memcmp(dirEntry->extension, extensionPrefix, 3) == 0) {
            count++;
        }
    }
    return count;
}

int gdx_content_file_exists(const char* name, const char extension[5]) {
    int32_t i;

    if (name == NULL || extension == NULL) {
        return GDX_CONTENT_ERR_BAD_ARGS;
    }
    {
        int rc = gdx_content_ensure_mounted();
        if (rc != GDX_CONTENT_OK) {
            return rc;
        }
    }
    for (i = 0; i < gDirectoryEntryCount && i < GDX_MFS_DIRECTORY_ENTRY_COUNT; i++) {
        const GdxMfsRamDirectoryEntry* dirEntry = &gMfsRamArea.directoryEntry[i];
        char dirName[21];
        if (!(dirEntry->attr & GDX_MFS_FILE_ATTR_FILE) || dirEntry->parentDirId != gWorkingDirectory) {
            continue;
        }
        /* The dir-entry name is fixed-width and may be unterminated; compare as NUL-terminated
         * strings with an explicit terminator. */
        memcpy(dirName, dirEntry->name, 20);
        dirName[20] = '\0';
        if (strcmp(dirName, name) == 0 && memcmp(dirEntry->extension, extension, 4) == 0) {
            return 1;
        }
    }
    return 0;
}

/* ---------------------------------------------------------------------------------
 * Export.
 * ------------------------------------------------------------------------------- */

static int gdx_content_write_container_raw(const char* path, int32_t contentType, uint32_t extTag, const char* name,
                                           uint32_t flags, const unsigned char* payload, uint32_t payloadSize) {
    unsigned char header[GDX_CONTENT_HEADER_SIZE];
    char tempPath[2048];
    FILE* f;
    int n;
    int rc;

    n = snprintf(tempPath, sizeof(tempPath), "%s.tmp", path);
    if (n < 0 || (size_t) n >= sizeof(tempPath)) {
        return GDX_CONTENT_ERR_BAD_ARGS;
    }

    memset(header, 0, sizeof(header));
    header[0] = 'G';
    header[1] = 'X';
    header[2] = 'C';
    header[3] = '1';
    gdx_content_write_u32le(header + 0x04, GDX_CONTENT_FORMAT_VERSION);
    gdx_content_write_u32le(header + 0x08, (uint32_t) contentType);
    gdx_content_write_u32le(header + 0x0C, extTag);
    memset(header + 0x10, 0, 21);
    memcpy(header + 0x10, name, 20);
    gdx_content_write_u32le(header + 0x25, flags);
    gdx_content_write_u32le(header + 0x29, payloadSize);
    gdx_content_write_u32le(header + 0x2D, gdx_content_crc32(payload, payloadSize));
    /* 0x31..0x40 reserved, already zero. */

    f = fopen(tempPath, "wb");
    if (f == NULL) {
        return GDX_CONTENT_ERR_IO;
    }
    rc = GDX_CONTENT_OK;
    if (fwrite(header, 1, sizeof(header), f) != sizeof(header) ||
        fwrite(payload, 1, payloadSize, f) != payloadSize || fflush(f) != 0) {
        rc = GDX_CONTENT_ERR_IO;
    }
    if (fclose(f) != 0) {
        rc = GDX_CONTENT_ERR_IO;
    }
    if (rc != GDX_CONTENT_OK) {
        remove(tempPath);
        return rc;
    }

#ifdef _WIN32
    if (!MoveFileExA(tempPath, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        remove(tempPath);
        return GDX_CONTENT_ERR_IO;
    }
#else
    if (rename(tempPath, path) != 0) {
        remove(tempPath);
        return GDX_CONTENT_ERR_IO;
    }
#endif
    return GDX_CONTENT_OK;
}

static int gdx_content_write_container(const char* path, const GdxContentEntry* entry, uint32_t flags,
                                       const unsigned char* payload, uint32_t payloadSize) {
    return gdx_content_write_container_raw(path, entry->contentType, gdx_content_fourcc(entry->extension), entry->name,
                                           flags, payload, payloadSize);
}

/* Writes bytes to path atomically (tmp + rename), the same way the container is written. */
static int gdx_content_write_bytes_atomic(const char* path, const unsigned char* bytes, size_t length) {
    char tempPath[2048];
    FILE* f;
    int n;
    int rc;

    n = snprintf(tempPath, sizeof(tempPath), "%s.tmp", path);
    if (n < 0 || (size_t) n >= sizeof(tempPath)) {
        return GDX_CONTENT_ERR_BAD_ARGS;
    }

    f = fopen(tempPath, "wb");
    if (f == NULL) {
        return GDX_CONTENT_ERR_IO;
    }
    rc = GDX_CONTENT_OK;
    if (fwrite(bytes, 1, length, f) != length || fflush(f) != 0) {
        rc = GDX_CONTENT_ERR_IO;
    }
    if (fclose(f) != 0) {
        rc = GDX_CONTENT_ERR_IO;
    }
    if (rc != GDX_CONTENT_OK) {
        remove(tempPath);
        return rc;
    }

#ifdef _WIN32
    if (!MoveFileExA(tempPath, path, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        remove(tempPath);
        return GDX_CONTENT_ERR_IO;
    }
#else
    if (rename(tempPath, path) != 0) {
        remove(tempPath);
        return GDX_CONTENT_ERR_IO;
    }
#endif
    return GDX_CONTENT_OK;
}

/* Checks or adds the signature of export member index before its sidecar block is written
 * (gdx_course_meta_sign.h). The local store learns the outcome, so the course select shows it too. *seedState
 * is 0 until the active profile's seed is first needed, then 1 when seed holds it or -1 when it could not be
 * had. */
static void gdx_content_sign_meta(const char* courseId, int index, GdxCourseMeta* meta,
                                  unsigned char seed[GDX_PROFILE_KEYS_SEED_BYTES], int* seedState) {
    const char* signer = gdx_profile_state_active_name();
    GdxMetaSignDigest d;
    int rc;

    d.ok = 0;
    if (!gdx_snapshot_export_content(index, gdx_meta_sign_digest_visit, &d)) {
        d.ok = 0;
    }
    /* The seed is made only for a track this profile will sign, never just because something was exported. */
    if ((*seedState == 0) && d.ok && !meta->hasSignature && meta->madeHere && (signer != NULL) &&
        (strcmp(signer, meta->author) == 0)) {
        rc = gdx_profile_state_active_seed(seed);
        *seedState = (rc == GDX_PROFILE_OK) ? 1 : -1;
        if (rc != GDX_PROFILE_OK) {
            gdx_port_logf("[content] no signing key for the active profile (%d)\n", rc);
        }
    }
    rc = gdx_meta_sign_for_export(meta, d.ok ? d.digest : NULL, signer, (*seedState == 1) ? seed : NULL);
    if ((rc == GDX_META_SIGN_SIGNED) || (rc == GDX_META_SIGN_DROPPED)) {
        gdx_port_logf("[content] %s: %s\n", courseId,
                      (rc == GDX_META_SIGN_SIGNED) ? "signed" : "signature no longer matches, dropped");
        if (gdx_meta_store_set(courseId, meta) != GDX_META_OK) {
            gdx_port_logf("[content] %s: signature state not stored\n", courseId);
        }
    }
}

/* Writes <stem>.gdxm beside gdxcPath when a track has creator metadata, each block bound to its payload CRC. No
 * file at all otherwise: 1.2.3 and older refuse a package member they do not know. Returns 1 when written, 0 when
 * no track has any, -1 on failure. */
static int gdx_content_write_meta(const char* gdxcPath, const char* const* names, const char* extension,
                                  const uint32_t* payloadCrcs, int count) {
    char courseIds[GDX_CONTENT_BUNDLE_MAX_TRACKS][GDX_BRANCH_COURSE_ID_MAX + 1];
    GdxCourseMeta metas[GDX_CONTENT_BUNDLE_MAX_TRACKS];
    GdxMetaSidecarEntry entries[GDX_CONTENT_BUNDLE_MAX_TRACKS];
    unsigned char bytes[GDX_META_SIDECAR_WRITE_MAX_BYTES];
    unsigned char seed[GDX_PROFILE_KEYS_SEED_BYTES];
    char metaPath[1600];
    size_t pathLen = strlen(gdxcPath);
    int seedState = 0;
    int length;
    int i;

    if (count < 1 || count > GDX_CONTENT_BUNDLE_MAX_TRACKS || pathLen >= sizeof(metaPath)) {
        return -1;
    }
    for (i = 0; i < count; i++) {
        if (!gdx_course_music_course_id(names[i], extension, courseIds[i], (int) sizeof(courseIds[i])) ||
            !gdx_meta_store_get(courseIds[i], &metas[i])) {
            memset(&metas[i], 0, sizeof(metas[i]));
        } else {
            /* Export members are numbered in the order this loop walks them. */
            gdx_content_sign_meta(courseIds[i], i, &metas[i], seed, &seedState);
        }
        entries[i].courseId = courseIds[i];
        entries[i].bindCrc = payloadCrcs[i];
        entries[i].meta = &metas[i];
    }
    if (seedState == 1) {
        gdx_profile_keys_wipe(seed, sizeof(seed));
    }
    length = gdx_meta_sidecar_encode(entries, count, bytes, sizeof(bytes));
    if (length <= 0) {
        if (length < 0) {
            gdx_port_logf("[content] creator metadata not written (%d)\n", length);
        }
        return length == 0 ? 0 : -1;
    }
    memcpy(metaPath, gdxcPath, pathLen + 1);
    metaPath[pathLen - 1] = 'm'; /* <stem>.gdxc -> <stem>.gdxm */
    return gdx_content_write_bytes_atomic(metaPath, bytes, (size_t) length) == GDX_CONTENT_OK ? 1 : -1;
}

int gdx_content_track_preview(const char* name, const void* base, float* x, float* z,
                               int capacity, struct GdxBranchSet* forks) {
    if (gdx_content_mfs_busy()) {
        return 0;
    }
    return gdx_snapshot_preview(gWorkingDirectory, name, base, x, z, capacity, forks);
}

int gdx_content_load_track_payload(const char* name, uint8_t* out, size_t outCap) {
    GdxContentEntry probe;
    GdxMfsRamDirectoryEntry dirEntry;
    int rc;

    if (name == NULL || name[0] == '\0' || strlen(name) > 20 || out == NULL ||
        outCap < GDX_CONTENT_TRACK_PAYLOAD_SIZE) {
        return GDX_CONTENT_ERR_BAD_ARGS;
    }
    rc = gdx_content_ensure_mounted();
    if (rc != GDX_CONTENT_OK) {
        return rc;
    }
    if (gdx_content_mfs_busy()) {
        return GDX_CONTENT_ERR_BUSY;
    }
    memset(&probe, 0, sizeof(probe));
    snprintf(probe.name, sizeof(probe.name), "%s", name);
    memcpy(probe.extension, "CRSD", 5);
    probe.contentType = GDX_CONTENT_TYPE_TRACK;
    if (!gdx_content_find_entry(&probe, &dirEntry)) {
        return GDX_CONTENT_ERR_NOT_FOUND;
    }
    if (dirEntry.fileSize != GDX_CONTENT_TRACK_PAYLOAD_SIZE) {
        return GDX_CONTENT_ERR_BAD_SIZE;
    }
    if (gdx_content_load_entry(&dirEntry, out, GDX_CONTENT_TRACK_PAYLOAD_SIZE) != 0) {
        return GDX_CONTENT_ERR_LOAD;
    }
    return GDX_CONTENT_OK;
}

static int gdx_content_join_path(char* out, size_t outCap, const char* dir, const char* name) {
#ifdef _WIN32
    int n = snprintf(out, outCap, "%s\\%s", dir, name);
#else
    int n = snprintf(out, outCap, "%s/%s", dir, name);
#endif
    return n >= 0 && (size_t) n < outCap;
}

/* .gdxp: makes exports/.gdxp-export exist and empty, and writes its path into stageDir. */
static int gdx_content_package_stage(const char* exportsDir, char* stageDir, size_t stageCap) {
    if (!gdx_content_join_path(stageDir, stageCap, exportsDir, GDX_CONTENT_PACK_EXPORT_WORK_DIR) ||
        gdx_content_pack_clear_dir(stageDir, 0) != GDX_CONTENT_PACK_OK) {
        return GDX_CONTENT_ERR_IO;
    }
    return GDX_CONTENT_OK;
}

/* .gdxp: zips the staged files into exports/<stem>.gdxp and removes the staging folder, whatever the
 * outcome. On success outPath receives the package path. */
static int gdx_content_package_finish(const char* exportsDir, const char* stageDir, const char* stem, char* outPath,
                                      size_t outCap) {
    char name[96];
    char packPath[1600];
    int rc = GDX_CONTENT_PACK_ERR_ARGS;
    int n = snprintf(name, sizeof(name), "%s%s", stem, GDX_CONTENT_PACK_EXT);

    if (n >= 0 && (size_t) n < sizeof(name) && gdx_content_join_path(packPath, sizeof(packPath), exportsDir, name)) {
        rc = gdx_content_pack_write_dir(packPath, stageDir);
    }
    gdx_content_pack_clear_dir(stageDir, 1);
    if (rc != GDX_CONTENT_PACK_OK) {
        gdx_port_logf("[content] package %s not written: %s\n", name, gdx_content_pack_error_string(rc));
        return GDX_CONTENT_ERR_PACKAGE;
    }
    if (outPath != NULL && outCap > 0) {
        if (snprintf(outPath, outCap, "%s", packPath) < 0 || strlen(packPath) >= outCap) {
            outPath[0] = '\0';
        }
    }
    return GDX_CONTENT_OK;
}

int gdx_content_export(const GdxContentEntry* entry, char* outPath, size_t outCap) {
    GdxMfsRamDirectoryEntry dirEntry;
    unsigned char* payload;
    char exportsDir[1200];
    char directory[1200];
    char sanitized[64];
    char stem[64];
    char path[1600];
    int32_t expectedSize;
    uint32_t flags = 0;
    int n;
    int rc;

    if (entry == NULL || (entry->contentType != GDX_CONTENT_TYPE_TRACK && entry->contentType != GDX_CONTENT_TYPE_MACHINE)) {
        return GDX_CONTENT_ERR_BAD_ARGS;
    }
    gdx_snapshot_export_begin();
    gdx_course_boot(); /* the store must hold the saved keys before the sidecar reads it */
    {
        int rc = gdx_content_ensure_mounted();
        if (rc != GDX_CONTENT_OK) {
            return rc;
        }
    }
    if (!gdx_content_find_entry(entry, &dirEntry)) {
        return GDX_CONTENT_ERR_NOT_FOUND;
    }

    expectedSize = (entry->contentType == GDX_CONTENT_TYPE_TRACK) ? GDX_CONTENT_TRACK_PAYLOAD_SIZE
                                                                  : GDX_CONTENT_MACHINE_PAYLOAD_SIZE;
    /* Exact-size rule: the game tolerates short loads and only catches them at checksum, so the
     * container refuses anything but the type's exact size. */
    if (dirEntry.fileSize != expectedSize) {
        return GDX_CONTENT_ERR_BAD_SIZE;
    }

    payload = (unsigned char*) malloc((size_t) expectedSize);
    if (payload == NULL) {
        return GDX_CONTENT_ERR_IO;
    }
    /* Re-check the guard immediately before the read: the scratch-sharing hazard (B1) is with
     * the disk thread's writes, not with enumeration. */
    if (gdx_content_mfs_busy()) {
        free(payload);
        return GDX_CONTENT_ERR_BUSY;
    }
    if (gdx_content_load_entry(&dirEntry, payload, expectedSize) != 0) {
        free(payload);
        return GDX_CONTENT_ERR_LOAD;
    }

    if (entry->contentType == GDX_CONTENT_TYPE_TRACK) {
        const int strip = CVarGetInteger("gEnhancements.Content.ExportIncludeGhosts", 0) == 0;
        if (!gdx_snapshot_export_member(gWorkingDirectory, entry->name, entry->extension, payload, strip)) {
            free(payload);
            return GDX_CONTENT_ERR_LOAD;
        }
        flags = GDX_CONTENT_FLAG_SNAPSHOT;
        if (strip) {
            flags |= GDX_CONTENT_FLAG_GHOSTS_STRIPPED | GDX_CONTENT_FLAG_RECORDS_STRIPPED;
        }
    }

    if (!gdx_content_exports_directory(exportsDir, sizeof(exportsDir), 1)) {
        free(payload);
        return GDX_CONTENT_ERR_IO;
    }
    /* A track is staged and then packaged with its sidecars and songs; a machine stays a loose .gdxc. */
    if (entry->contentType == GDX_CONTENT_TYPE_TRACK) {
        rc = gdx_content_package_stage(exportsDir, directory, sizeof(directory));
        if (rc != GDX_CONTENT_OK) {
            free(payload);
            return rc;
        }
    } else {
        snprintf(directory, sizeof(directory), "%s", exportsDir);
    }
    gdx_content_sanitize_filename(entry->name, sanitized, sizeof(sanitized));
    /* Names inside a package are plain ASCII (gdx_content_pack.h). The importer takes the course name
     * from the container header, so only the package itself keeps the full file name. */
    memcpy(stem, sanitized, sizeof(stem));
    if (entry->contentType == GDX_CONTENT_TYPE_TRACK) {
        for (n = 0; stem[n] != '\0'; n++) {
            if ((unsigned char) stem[n] >= 0x7F) {
                stem[n] = '_';
            }
        }
    }
#ifdef _WIN32
    n = snprintf(path, sizeof(path), "%s\\%s.gdxc", directory, stem);
#else
    n = snprintf(path, sizeof(path), "%s/%s.gdxc", directory, stem);
#endif
    if (n < 0 || (size_t) n >= sizeof(path)) {
        free(payload);
        if (entry->contentType == GDX_CONTENT_TYPE_TRACK) {
            gdx_content_pack_clear_dir(directory, 1);
        }
        return GDX_CONTENT_ERR_IO;
    }

    if (entry->contentType == GDX_CONTENT_TYPE_TRACK) {
        const char* names[1] = { entry->name };
        const uint32_t payloadCrc = gdx_content_crc32(payload, expectedSize);
        int meta;

        if (!gdx_snapshot_export_write(path, payloadCrc)) {
            free(payload);
            gdx_content_pack_clear_dir(directory, 1);
            return GDX_CONTENT_ERR_IO;
        }
        meta = gdx_content_write_meta(path, names, entry->extension, &payloadCrc, 1);
        if (meta < 0) {
            free(payload);
            gdx_content_pack_clear_dir(directory, 1);
            return GDX_CONTENT_ERR_IO;
        }
        if (meta > 0) {
            flags |= GDX_CONTENT_FLAG_META;
        }
    }

    rc = gdx_content_write_container(path, entry, flags, payload, (uint32_t) expectedSize);
    free(payload);
    if (entry->contentType == GDX_CONTENT_TYPE_TRACK) {
        if (rc != GDX_CONTENT_OK) {
            gdx_content_pack_clear_dir(directory, 1);
            return rc;
        }
        return gdx_content_package_finish(exportsDir, directory, sanitized, outPath, outCap);
    }
    if (rc == GDX_CONTENT_OK && outPath != NULL && outCap > 0) {
        if (snprintf(outPath, outCap, "%s", path) < 0 || strlen(path) >= outCap) {
            outPath[0] = '\0';
        }
    }
    return rc;
}

/* ---------------------------------------------------------------------------------
 * E4 cup-bundle export (payload layout documented in gdx_content_io.h).
 * ------------------------------------------------------------------------------- */

#define GDX_CONTENT_BUNDLE_NAME "EDIT-CUP"
#define GDX_CONTENT_BUNDLE_FILE_STEM "edit-cup"
#define GDX_CONTENT_BUNDLE_MAX_PAYLOAD                                                        \
    (2 + GDX_CONTENT_BUNDLE_ENTRY_HEADER_SIZE * (2 + GDX_CONTENT_BUNDLE_MAX_TRACKS) +         \
     GDX_CONTENT_CENT_PAYLOAD_SIZE + GDX_CONTENT_CENL_PAYLOAD_SIZE +                         \
     GDX_CONTENT_BUNDLE_MAX_TRACKS * GDX_CONTENT_TRACK_PAYLOAD_SIZE)

/* Finds a CRSD file by NUL-terminated name in the working directory (same comparison shape as
 * gdx_content_file_exists). */
static int gdx_content_find_crsd(const char* name, GdxMfsRamDirectoryEntry* outDirEntry) {
    int32_t i;

    for (i = 0; i < gDirectoryEntryCount && i < GDX_MFS_DIRECTORY_ENTRY_COUNT; i++) {
        const GdxMfsRamDirectoryEntry* dirEntry = &gMfsRamArea.directoryEntry[i];
        char dirName[21];
        if (!(dirEntry->attr & GDX_MFS_FILE_ATTR_FILE) || dirEntry->parentDirId != gWorkingDirectory) {
            continue;
        }
        memcpy(dirName, dirEntry->name, 20);
        dirName[20] = '\0';
        if (strcmp(dirName, name) == 0 && memcmp(dirEntry->extension, "CRSD", 4) == 0) {
            memcpy(outDirEntry, dirEntry, sizeof(*outDirEntry));
            return 1;
        }
    }
    return 0;
}

/* Appends one bundle entry record at *cursor and advances it. The CRC is computed over the
 * final (post-strip) payload bytes so the import side validates exactly what was packed. */
static void gdx_content_pack_bundle_entry(unsigned char** cursor, const char ext[4], const char* name,
                                          const unsigned char* payload, uint32_t payloadSize) {
    unsigned char* p = *cursor;
    size_t nameLen = strlen(name);

    memcpy(p, ext, 4);
    p += 4;
    memset(p, 0, 21);
    memcpy(p, name, nameLen > 20 ? 20 : nameLen);
    p += 21;
    gdx_content_write_u32le(p, payloadSize);
    p += 4;
    gdx_content_write_u32le(p, gdx_content_crc32(payload, payloadSize));
    p += 4;
    memcpy(p, payload, payloadSize);
    p += payloadSize;
    *cursor = p;
}

int gdx_content_export_bundle(char* outPath, size_t outCap) {
    unsigned char cent[GDX_CONTENT_CENT_PAYLOAD_SIZE];
    unsigned char cenl[GDX_CONTENT_CENL_PAYLOAD_SIZE];
    unsigned char* bundlePayload = NULL;
    unsigned char* trackBuf = NULL;
    unsigned char* cursor;
    GdxMfsRamDirectoryEntry dirEntry;
    char packedNames[GDX_CONTENT_BUNDLE_MAX_TRACKS][GDX_EDIT_CUP_NAME_SIZE];
    uint32_t memberCrc[GDX_CONTENT_BUNDLE_MAX_TRACKS];
    int memberRing[GDX_CONTENT_BUNDLE_MAX_TRACKS];
    char exportsDir[1200];
    char directory[1200];
    char path[1600];
    uint32_t flags = 0;
    int stripGhosts;
    int trackCount = 0;
    int haveCenl;
    int n;
    int i;
    int j;
    int rc;

    {
        int rc = gdx_content_ensure_mounted();
        if (rc != GDX_CONTENT_OK) {
            return rc;
        }
    }

    gdx_snapshot_export_begin();
    gdx_course_boot(); /* the store must hold the saved keys before the companion reads it */

    /* The CENT and CENL images come from the in-memory cup state (loaded from CRS_ENTRY.CENT and .CENL
     * at mount and kept in sync by every game-side registration path), packed the way MFS writes them.
     * The CENL entry is added only when a slot name is over 8 characters, so a cup of short names
     * still imports on older builds. */
    gdx_edit_cup_pack_cent((const char*) gEditCupTrackNames, cent);
    haveCenl = gdx_edit_cup_pack_cenl((const char*) gEditCupTrackNames, cenl);
    stripGhosts = CVarGetInteger("gEnhancements.Content.ExportIncludeGhosts", 0) == 0;

    bundlePayload = (unsigned char*) malloc(GDX_CONTENT_BUNDLE_MAX_PAYLOAD);
    trackBuf = (unsigned char*) malloc(GDX_CONTENT_TRACK_PAYLOAD_SIZE);
    if (bundlePayload == NULL || trackBuf == NULL) {
        free(bundlePayload);
        free(trackBuf);
        return GDX_CONTENT_ERR_IO;
    }

    cursor = bundlePayload + 2; /* entryCount patched once the track count is known */
    gdx_content_pack_bundle_entry(&cursor, "CENT", "CRS_ENTRY", cent, sizeof(cent));
    if (haveCenl) {
        gdx_content_pack_bundle_entry(&cursor, "CENL", "CRS_ENTRY", cenl, sizeof(cenl));
    }

    for (i = 0; i < GDX_CONTENT_BUNDLE_MAX_TRACKS; i++) {
        char slot[GDX_EDIT_CUP_NAME_SIZE];
        memcpy(slot, gEditCupTrackNames[i], GDX_EDIT_CUP_NAME_SIZE);
        slot[GDX_EDIT_CUP_NAME_SIZE - 1] = '\0';
        if (slot[0] == '\0') {
            continue;
        }
        /* A track named by two slots is packed once. */
        for (j = 0; j < trackCount; j++) {
            if (strcmp(packedNames[j], slot) == 0) {
                break;
            }
        }
        if (j != trackCount) {
            continue;
        }
        /* Dangling slots (no CRSD on disk) are skipped, same tolerance the game's own cup-slot
         * validation applies (course_edit/19D4A0.c:26-32). */
        if (!gdx_content_find_crsd(slot, &dirEntry)) {
            continue;
        }
        if (dirEntry.fileSize != GDX_CONTENT_TRACK_PAYLOAD_SIZE) {
            free(bundlePayload);
            free(trackBuf);
            return GDX_CONTENT_ERR_BAD_SIZE;
        }
        /* Same scratch-sharing re-check as the single export, immediately before the read. */
        if (gdx_content_mfs_busy()) {
            free(bundlePayload);
            free(trackBuf);
            return GDX_CONTENT_ERR_BUSY;
        }
        if (gdx_content_load_entry(&dirEntry, trackBuf, GDX_CONTENT_TRACK_PAYLOAD_SIZE) != 0) {
            free(bundlePayload);
            free(trackBuf);
            return GDX_CONTENT_ERR_LOAD;
        }
        if (!gdx_snapshot_export_member(gWorkingDirectory, slot, "CRSD", trackBuf, stripGhosts)) {
            free(bundlePayload);
            free(trackBuf);
            return GDX_CONTENT_ERR_LOAD;
        }
        memberCrc[trackCount] = gdx_content_crc32(trackBuf, GDX_CONTENT_TRACK_PAYLOAD_SIZE);
        memberRing[trackCount] = trackBuf[0x001];
        gdx_content_pack_bundle_entry(&cursor, "CRSD", slot, trackBuf, GDX_CONTENT_TRACK_PAYLOAD_SIZE);
        memcpy(packedNames[trackCount], slot, sizeof(packedNames[trackCount]));
        trackCount++;
    }
    free(trackBuf);

    if (trackCount == 0) {
        free(bundlePayload);
        return GDX_CONTENT_ERR_NOT_FOUND; /* no slot resolves to a track on disk */
    }
    flags = GDX_CONTENT_FLAG_SNAPSHOT;
    if (stripGhosts) {
        flags |= GDX_CONTENT_FLAG_GHOSTS_STRIPPED | GDX_CONTENT_FLAG_RECORDS_STRIPPED;
    }
    bundlePayload[0] = (unsigned char) ((1 + haveCenl + trackCount) & 0xFF);
    bundlePayload[1] = (unsigned char) (((1 + haveCenl + trackCount) >> 8) & 0xFF);

    if (!gdx_content_exports_directory(exportsDir, sizeof(exportsDir), 1)) {
        free(bundlePayload);
        return GDX_CONTENT_ERR_IO;
    }
    rc = gdx_content_package_stage(exportsDir, directory, sizeof(directory));
    if (rc != GDX_CONTENT_OK) {
        free(bundlePayload);
        return rc;
    }
#ifdef _WIN32
    n = snprintf(path, sizeof(path), "%s\\%s.gdxc", directory, GDX_CONTENT_BUNDLE_FILE_STEM);
#else
    n = snprintf(path, sizeof(path), "%s/%s.gdxc", directory, GDX_CONTENT_BUNDLE_FILE_STEM);
#endif
    if (n < 0 || (size_t) n >= sizeof(path)) {
        free(bundlePayload);
        gdx_content_pack_clear_dir(directory, 1);
        return GDX_CONTENT_ERR_IO;
    }

    if (!gdx_snapshot_export_write(path, gdx_content_crc32(bundlePayload, (size_t) (cursor - bundlePayload)))) {
        free(bundlePayload);
        gdx_content_pack_clear_dir(directory, 1);
        return GDX_CONTENT_ERR_IO;
    }
    {
        const char* names[GDX_CONTENT_BUNDLE_MAX_TRACKS];
        int meta;

        for (i = 0; i < trackCount; i++) {
            names[i] = packedNames[i];
        }
        meta = gdx_content_write_meta(path, names, "CRSD", memberCrc, trackCount);
        if (meta < 0) {
            free(bundlePayload);
            gdx_content_pack_clear_dir(directory, 1);
            return GDX_CONTENT_ERR_IO;
        }
        if (meta > 0) {
            flags |= GDX_CONTENT_FLAG_META;
        }
    }
    rc = gdx_content_write_container_raw(path, GDX_CONTENT_TYPE_BUNDLE, gdx_content_fourcc("CENT"),
                                         GDX_CONTENT_BUNDLE_NAME, flags, bundlePayload,
                                         (uint32_t) (cursor - bundlePayload));
    free(bundlePayload);
    if (rc != GDX_CONTENT_OK) {
        gdx_content_pack_clear_dir(directory, 1);
        return rc;
    }
    return gdx_content_package_finish(exportsDir, directory, GDX_CONTENT_BUNDLE_FILE_STEM, outPath, outCap);
}

const char* gdx_content_error_string(int rc) {
    switch (rc) {
        case GDX_CONTENT_OK:
            return "ok";
        case GDX_CONTENT_ERR_BAD_ARGS:
            return "bad arguments";
        case GDX_CONTENT_ERR_BUSY:
            return "disk busy (an MFS operation is in flight)";
        case GDX_CONTENT_ERR_IO:
            return "file I/O error";
        case GDX_CONTENT_ERR_NOT_FOUND:
            return "file no longer present in the working directory";
        case GDX_CONTENT_ERR_BAD_SIZE:
            return "unexpected file size for its content type";
        case GDX_CONTENT_ERR_LOAD:
            return "MFS load failed";
        case GDX_CONTENT_ERR_NO_FS:
            return "no disk filesystem mounted";
        case GDX_CONTENT_ERR_PACKAGE:
            return "could not write the .gdxp package";
        default:
            return "unknown error";
    }
}
