/* port/gdx_content_import.c -- .gdxc course/machine import (E2), Edit-Cup registration (E3) and
 * cup-bundle import (E4).
 *
 * Contract lives in gdx_content_import.h. Same host-CRT target split as gdx_content_io.c: this
 * TU mirrors the CourseData layout byte-for-byte and declares raw externs rather than including
 * decomp headers (which drag in the PORT/EK macro-gated declarations only gdiffuser_game is
 * compiled with). The size checks below turn mirror drift into a compile error.
 */

#define _CRT_SECURE_NO_WARNINGS /* plain fopen/fread below; harmless on non-MSVC */

#include "gdx_content_import.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <dirent.h>
#include <errno.h>
#endif

/* ---------------------------------------------------------------------------------
 * Mirrored course payload layout (decomp/include/fzx_course.h:7-34). Only CourseData (the
 * first 0x7E0 bytes of the 0xC830 payload) is validated; ghosts/records ride uninspected.
 * ------------------------------------------------------------------------------- */

typedef struct GdxImportVec3f {
    float x;
    float y;
    float z;
} GdxImportVec3f;

/* Mirrors ControlPoint, fzx_course.h:7-12. Size 0x14. */
typedef struct GdxImportControlPoint {
    GdxImportVec3f pos;
    int16_t radiusLeft;
    int16_t radiusRight;
    int32_t trackSegmentInfo;
} GdxImportControlPoint;

/* Mirrors CourseData, fzx_course.h:14-34. Size 0x7E0. */
typedef struct GdxImportCourseData {
    uint8_t creatorId;
    int8_t controlPointCount;
    int8_t venue;
    int8_t skybox;
    uint32_t checksum;
    uint8_t flag;
    char fileName[22];
    int8_t bgm;
    GdxImportControlPoint controlPoint[64];
    int16_t bankAngle[64];
    int8_t pit[64];
    int8_t dash[64];
    int8_t dirt[64];
    int8_t ice[64];
    int8_t jump[64];
    int8_t landmine[64];
    int8_t gate[64];
    int8_t building[64];
    int8_t sign[64];
} GdxImportCourseData;

typedef char gdx_import_size_check_controlPoint[(sizeof(GdxImportControlPoint) == 0x14) ? 1 : -1];
typedef char gdx_import_size_check_courseData[(sizeof(GdxImportCourseData) == 0x7E0) ? 1 : -1];
typedef char gdx_import_size_check_bgmOffset[(offsetof(GdxImportCourseData, bgm) == 0x1F) ? 1 : -1];

/* Game-side constants the load path validates against:
 * CREATOR_NINTENDO (fzx_course.h:103), BGM_NEW_04 (sfx.h:110), MACHINE_STAT_MAX_WEIGHTING
 * (machine_create.h:61). */
#define GDX_IMPORT_CREATOR_NINTENDO 4
#define GDX_IMPORT_BGM_MAX 13
#define GDX_IMPORT_MACHINE_STAT_MAX_WEIGHTING 13

/* TRACK_JOIN_MASK | TRACK_FORM_MASK | TRACK_FLAG_CONTINUOUS (fzx_course.h:327,344,350): the
 * bits Course_CalculateChecksum clears before summing. */
#define GDX_IMPORT_CHECKSUM_MASK_CLEAR (0x600 | 0x38000 | 0x40000000)

/* ---------------------------------------------------------------------------------
 * Decomp externs. All are host-native on the port (see gdx_content_io.c for the same split).
 * ------------------------------------------------------------------------------- */

extern uint16_t gWorkingDirectory;
extern int32_t gMfsError;
extern int32_t gDirectoryEntryCount;

/* EK build: course_context.c:45 defines [6 * 4][9]; the edit cup occupies the first 6 slots. */
extern char gEditCupTrackNames[24][9];

/* machine_create_stats.c:53 -- u8 sMachineStatWeightings[3][5], body/boost/grip x E..A. */
extern uint8_t sMachineStatWeightings[][5];

/* leo/mfs (linked under EXPANSION_KIT; same linkage profile as gdx_content_io.c's loader). */
extern int32_t Mfs_SaveFile(uint16_t parentDirId, char* name, char* extension, uint8_t* buf, uint32_t fileSize,
                            int32_t attr, int32_t copyCount, int writeChanges);
extern int32_t Mfs_DeleteFileInDir(uint16_t dirId, char* name, char* extension, int writeChanges);
extern int32_t Mfs_ValidateFileName(char* name);
extern int32_t Mfs_RamGetFreeSize(void);

/* Disk-thread op-slot posters, decomp/src/sys/disk/75000.c (#ifdef PORT). */
extern int32_t GdxContentImport_EnqueueDiskOp(void);
extern int32_t GdxContentCup_EnqueueDiskOp(void);
extern int32_t GdxContentBundle_EnqueueDiskOp(void);

extern int CVarGetInteger(const char* name, int32_t defaultValue);
extern int gdx_input_in_gameplay(void);

/* ---------------------------------------------------------------------------------
 * Container parse (GXC1, fixed 0x41-byte header; layout documented in gdx_content_io.h).
 * ------------------------------------------------------------------------------- */

#define GDX_CONTENT_HEADER_SIZE 0x41
#define GDX_CONTENT_FORMAT_VERSION 1u

static uint32_t gdx_import_read_u32le(const unsigned char* p) {
    return (uint32_t) p[0] | ((uint32_t) p[1] << 8) | ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24);
}

static uint32_t gdx_import_read_u16le(const unsigned char* p) {
    return (uint32_t) p[0] | ((uint32_t) p[1] << 8);
}

/* Routes container files by contentType without committing to a parse: returns the header's
 * contentType, or 0 when the file is unreadable / not a GXC1 v1 container (the single-item
 * parser then reports the real error). */
static int32_t gdx_import_peek_content_type(const char* path) {
    unsigned char header[0x10];
    FILE* f = fopen(path, "rb");

    if (f == NULL) {
        return 0;
    }
    if (fread(header, 1, sizeof(header), f) != sizeof(header)) {
        fclose(f);
        return 0;
    }
    fclose(f);
    if (memcmp(header, "GXC1", 4) != 0 || gdx_import_read_u32le(header + 0x04) != GDX_CONTENT_FORMAT_VERSION) {
        return 0;
    }
    return (int32_t) gdx_import_read_u32le(header + 0x08);
}

static uint32_t gdx_import_fourcc(const char ext[5]) {
    return (uint32_t) (unsigned char) ext[0] | ((uint32_t) (unsigned char) ext[1] << 8) |
           ((uint32_t) (unsigned char) ext[2] << 16) | ((uint32_t) (unsigned char) ext[3] << 24);
}

static int32_t gdx_import_expected_payload_size(int32_t contentType) {
    return (contentType == GDX_CONTENT_TYPE_TRACK) ? GDX_CONTENT_TRACK_PAYLOAD_SIZE
                                                   : GDX_CONTENT_MACHINE_PAYLOAD_SIZE;
}

/* ---------------------------------------------------------------------------------
 * Game validators. Both are mirrors of the checks the game's own load path runs:
 * course_edit/19C470.c:160-167 (checksum + creatorId + BGM, checksum algorithm at the end of
 * game/course.c) and machine_create_update.c:877-879 (u16 checksum + stats validity). The
 * mirrors add bounds checks the game itself lacks (control point count, stat ranks) because the
 * payload here is untrusted (risk R2).
 * ------------------------------------------------------------------------------- */

/* Mirror of func_i2_800BE9D4 (game/course.c): nonzero when the float bit pattern is NaN/Inf or
 * subnormal-nonzero. */
static int gdx_import_float_invalid(const float* regValue) {
    uint32_t regAsInt;
    int32_t regExp;

    memcpy(&regAsInt, regValue, sizeof(regAsInt));
    regExp = (int32_t) ((regAsInt & 0x7F800000u) >> 0x17) - 0x7F;
    if (((-0x7F < regExp) && (regExp < 0x80)) || (regAsInt == 0)) {
        return 0;
    }
    return 1;
}

/* Mirror of Course_CalculateChecksum's EXPANSION_KIT branch over an arbitrary payload buffer.
 * The float expression is verbatim so host float semantics produce the identical sum. Returns
 * the checksum; *geometryError is set (and the sum aborted, mirroring the game's -1 bail) when a
 * control point leaves the game's accepted float ranges. */
static uint32_t gdx_import_course_checksum(const GdxImportCourseData* courseData, int* geometryError) {
    int32_t i;
    uint32_t checksum = (uint32_t) courseData->controlPointCount;

    *geometryError = 0;
    for (i = 0; i < courseData->controlPointCount; i++) {
        const GdxImportControlPoint* controlPoint = &courseData->controlPoint[i];
        int32_t trackSegmentInfo = controlPoint->trackSegmentInfo & ~GDX_IMPORT_CHECKSUM_MASK_CLEAR;

        if (gdx_import_float_invalid(&controlPoint->pos.x) || controlPoint->pos.x < -15000.0f ||
            controlPoint->pos.x > 15000.0f || gdx_import_float_invalid(&controlPoint->pos.y) ||
            controlPoint->pos.y < -250.0f || controlPoint->pos.y > 5000.0f ||
            gdx_import_float_invalid(&controlPoint->pos.z) || controlPoint->pos.z < -15000.0f ||
            controlPoint->pos.z > 15000.0f) {
            *geometryError = 1;
            return 0;
        }

        checksum += (uint32_t) (int32_t) ((controlPoint->pos.x + ((1.1f + (0.7f * i)) * controlPoint->pos.y)) +
                                          ((2.2f + (1.2f * i)) * controlPoint->pos.z * (4.4f + (0.9f * i))) +
                                          controlPoint->radiusLeft +
                                          ((5.5f + (0.8f * i)) * controlPoint->radiusRight * 4.8f)) +
                    (uint32_t) (trackSegmentInfo * (0xFE - i)) +
                    (uint32_t) (courseData->bankAngle[i] * (0x93DE - i * 2));
    }

    for (i = 0; i < courseData->controlPointCount; i++) {
        checksum += (uint32_t) (courseData->pit[i] * i);
        checksum += (uint32_t) (courseData->dash[i] * (i + 0x10));
        checksum += (uint32_t) (courseData->dirt[i] * (i + 0x80));
        checksum += (uint32_t) (courseData->ice[i] * (i + 0x100));
        checksum += (uint32_t) (courseData->jump[i] * (i + 0x800));
        checksum += (uint32_t) (courseData->landmine[i] * (i + 0x1000));
        checksum += (uint32_t) (courseData->gate[i] * (i + 0x8000));
        checksum += (uint32_t) (courseData->building[i] * (i + 0x10000));
        checksum += (uint32_t) (courseData->sign[i] * (i + 0x80000));
    }

    return checksum;
}

static void gdx_import_validate_track(const uint8_t* payload, uint32_t* errors) {
    GdxImportCourseData courseData;
    uint32_t computed;
    int geometryError;

    /* Work on a stack copy so the (unused) masked-trackSegmentInfo semantics can never leak back
     * into the payload that gets installed. */
    memcpy(&courseData, payload, sizeof(courseData));

    /* The game indexes controlPoint[64] with this s8 without a bounds check; a crafted count
     * would read out of bounds there, so the container refuses it here. */
    if (courseData.controlPointCount < 0 || courseData.controlPointCount > 64) {
        *errors |= GDX_IMPORT_ERR_TRACK_GEOMETRY;
        return;
    }

    computed = gdx_import_course_checksum(&courseData, &geometryError);
    if (geometryError) {
        *errors |= GDX_IMPORT_ERR_TRACK_GEOMETRY;
    } else if (computed != courseData.checksum) {
        *errors |= GDX_IMPORT_ERR_TRACK_CHECKSUM;
    }
    if (courseData.creatorId != GDX_IMPORT_CREATOR_NINTENDO) {
        *errors |= GDX_IMPORT_ERR_TRACK_CREATOR;
    }
    if (courseData.bgm > GDX_IMPORT_BGM_MAX) {
        *errors |= GDX_IMPORT_ERR_TRACK_BGM;
    }
}

static void gdx_import_validate_machine(const uint8_t* payload, uint32_t* errors) {
    uint16_t checksum = 0;
    uint16_t stored;
    uint8_t body = payload[0x00];
    uint8_t boost = payload[0x01];
    uint8_t grip = payload[0x02];
    int i;

    /* MachineCreate_CalculateCustomMachineChecksum: u16 additive over the first 30 bytes. */
    for (i = 0; i < 30; i++) {
        checksum = (uint16_t) (checksum + payload[i]);
    }
    memcpy(&stored, payload + 0x1E, sizeof(stored));
    if (stored != checksum || checksum == 0) {
        *errors |= GDX_IMPORT_ERR_MACHINE_CHECKSUM;
    }

    /* MachineCreate_CustomMachineStatsIsValid. The game indexes the weighting table with the raw
     * stat bytes; crafted ranks would read out of bounds, so the range check comes first. */
    if (body >= 5 || boost >= 5 || grip >= 5 ||
        sMachineStatWeightings[0][body] + sMachineStatWeightings[1][boost] + sMachineStatWeightings[2][grip] >
            GDX_IMPORT_MACHINE_STAT_MAX_WEIGHTING) {
        *errors |= GDX_IMPORT_ERR_MACHINE_STATS;
    }
}

/* ---------------------------------------------------------------------------------
 * Validation chain. Order per CONTENT_EXPORT.md: magic/version -> type/extTag -> size -> CRC ->
 * game validators -> Mfs_ValidateFileName -> cap. Every applicable check runs even after an
 * earlier failure so the UI can list all reasons at once.
 * ------------------------------------------------------------------------------- */

/* Header + payload checks (everything that does not touch live MFS state). When payloadOut is
 * non-NULL and the size checks pass, the validated payload is copied out for staging. */
static uint32_t gdx_import_validate_container(const char* path, GdxContentImportEntry* entry, uint8_t* payloadOut) {
    unsigned char header[GDX_CONTENT_HEADER_SIZE];
    uint8_t* payload = NULL;
    uint32_t errors = 0;
    uint32_t version;
    uint32_t extTag;
    uint32_t payloadSize;
    uint32_t payloadCrc;
    int32_t expectedSize;
    long fileSize;
    FILE* f;
    char name[21];

    f = fopen(path, "rb");
    if (f == NULL) {
        return GDX_IMPORT_ERR_IO;
    }
    if (fseek(f, 0, SEEK_END) != 0 || (fileSize = ftell(f)) < 0 || fseek(f, 0, SEEK_SET) != 0 ||
        fileSize < GDX_CONTENT_HEADER_SIZE || fread(header, 1, sizeof(header), f) != sizeof(header)) {
        fclose(f);
        return GDX_IMPORT_ERR_IO;
    }

    /* magic + version */
    if (memcmp(header, "GXC1", 4) != 0) {
        errors |= GDX_IMPORT_ERR_BAD_MAGIC;
    }
    version = gdx_import_read_u32le(header + 0x04);
    if (version != GDX_CONTENT_FORMAT_VERSION) {
        errors |= GDX_IMPORT_ERR_BAD_VERSION;
    }

    /* contentType + extTag, and their pairing */
    entry->contentType = (int32_t) gdx_import_read_u32le(header + 0x08);
    extTag = gdx_import_read_u32le(header + 0x0C);
    if (entry->contentType != GDX_CONTENT_TYPE_TRACK && entry->contentType != GDX_CONTENT_TYPE_MACHINE) {
        errors |= GDX_IMPORT_ERR_BAD_TYPE;
    }
    if (extTag == gdx_import_fourcc("CRSD")) {
        memcpy(entry->extension, "CRSD", 5);
    } else if (extTag == gdx_import_fourcc("CRSE")) {
        memcpy(entry->extension, "CRSE", 5);
    } else if (extTag == gdx_import_fourcc("CARD")) {
        memcpy(entry->extension, "CARD", 5);
    } else {
        entry->extension[0] = '\0';
        errors |= GDX_IMPORT_ERR_BAD_EXT;
    }
    /* Pairing: tracks carry CRSD/CRSE ('S' at [2]), machines carry CARD; a mismatch is the two
     * sides disagreeing. */
    if ((errors & (GDX_IMPORT_ERR_BAD_TYPE | GDX_IMPORT_ERR_BAD_EXT)) == 0 &&
        ((entry->contentType == GDX_CONTENT_TYPE_TRACK) != (entry->extension[2] == 'S'))) {
        errors |= GDX_IMPORT_ERR_TYPE_EXT_MISMATCH;
    }

    entry->flags = gdx_import_read_u32le(header + 0x25);
    payloadSize = gdx_import_read_u32le(header + 0x29);
    payloadCrc = gdx_import_read_u32le(header + 0x2D);

    /* Exact-size rule, both directions: payloadSize must equal the type's size and the host file
     * must be exactly header + payload (no truncation tolerance, no trailing garbage). */
    expectedSize = (errors & (GDX_IMPORT_ERR_BAD_TYPE | GDX_IMPORT_ERR_BAD_EXT)) == 0
                       ? gdx_import_expected_payload_size(entry->contentType)
                       : -1;
    if (expectedSize < 0 || payloadSize != (uint32_t) expectedSize ||
        (long) (GDX_CONTENT_HEADER_SIZE + payloadSize) != fileSize) {
        errors |= GDX_IMPORT_ERR_BAD_SIZE;
    }

    /* MFS filename rules run on header bytes, so they apply even when earlier checks failed. */
    memcpy(name, header + 0x10, 20);
    name[20] = '\0';
    memcpy(entry->name, name, sizeof(entry->name));
    if (Mfs_ValidateFileName(name) != 0) {
        errors |= GDX_IMPORT_ERR_BAD_NAME;
    }

    if ((errors & GDX_IMPORT_ERR_BAD_SIZE) == 0) {
        payload = (uint8_t*) malloc(payloadSize);
        if (payload == NULL) {
            fclose(f);
            return errors | GDX_IMPORT_ERR_IO;
        }
        if (fseek(f, GDX_CONTENT_HEADER_SIZE, SEEK_SET) != 0 ||
            fread(payload, 1, payloadSize, f) != payloadSize) {
            free(payload);
            fclose(f);
            return errors | GDX_IMPORT_ERR_IO;
        }

        /* CRC before the game validators; a CRC failure does not stop the later checks (they are
         * read-only and bounds-checked) so the report stays complete. */
        if (gdx_content_crc32(payload, payloadSize) != payloadCrc) {
            errors |= GDX_IMPORT_ERR_BAD_CRC;
        }
        if (entry->contentType == GDX_CONTENT_TYPE_TRACK) {
            gdx_import_validate_track(payload, &errors);
        } else {
            gdx_import_validate_machine(payload, &errors);
        }
        if (payloadOut != NULL) {
            memcpy(payloadOut, payload, payloadSize);
        }
        free(payload);
    }

    fclose(f);
    return errors;
}

/* Live-MFS-state checks: quota (the 100-file cap is the import's admission gate, risk R1), free
 * space, and the overwrite/twin/cup warnings (risk R4). Skipped wholesale while the disk thread
 * is busy -- the bits are then unknown, not clean. */
static void gdx_import_check_disk_state(GdxContentImportEntry* entry, uint32_t* errors, uint32_t* warnings) {
    int32_t payloadSize = gdx_import_expected_payload_size(entry->contentType);
    int32_t freeSize;
    int32_t probe;
    int count;
    int warnThreshold;
    const char* classPrefix;
    char twinExt[6];

    if (gdx_content_mfs_busy()) {
        *errors |= GDX_IMPORT_ERR_STATE_UNKNOWN;
        entry->classFileCount = -1;
        return;
    }

    classPrefix = (entry->contentType == GDX_CONTENT_TYPE_TRACK) ? "CRS" : "CAR";
    count = gdx_content_count_extension_class(classPrefix);
    if (count < 0) {
        *errors |= GDX_IMPORT_ERR_STATE_UNKNOWN;
        entry->classFileCount = -1;
        return;
    }
    entry->classFileCount = count;

    probe = gdx_content_file_exists(entry->name, entry->extension);
    if (probe < 0) {
        *errors |= GDX_IMPORT_ERR_STATE_UNKNOWN;
        return;
    }
    if (probe != 0) {
        *warnings |= GDX_IMPORT_WARN_OVERWRITE;
    }

    if (entry->contentType == GDX_CONTENT_TYPE_TRACK) {
        /* The install deletes the opposite-extension twin, mirroring the editor's save op
         * (func_xk2_800EB938, course_edit/19C470.c:478-486). */
        memcpy(twinExt, entry->extension[3] == 'D' ? "CRSE" : "CRSD", 5);
        twinExt[5] = '\0';
        probe = gdx_content_file_exists(entry->name, twinExt);
        if (probe > 0) {
            *warnings |= GDX_IMPORT_WARN_TWIN_DELETE;
        }
        if (entry->extension[3] == 'E') {
            /* CRSE tracks cannot sit in the edit cup; the import clears a slot naming it, so say
             * so up front. */
            int i;
            for (i = 0; i < 6; i++) {
                char slot[10];
                memcpy(slot, gEditCupTrackNames[i], 9);
                slot[9] = '\0';
                if (slot[0] != '\0' && strcmp(slot, entry->name) == 0) {
                    *warnings |= GDX_IMPORT_WARN_CUP_CLEAR;
                    break;
                }
            }
        }
    }

    /* An overwrite replaces a file and does not grow the class, so the cap only gates new
     * names. */
    if ((*warnings & GDX_IMPORT_WARN_OVERWRITE) == 0) {
        if (count >= 100) {
            *errors |= GDX_IMPORT_ERR_QUOTA_FULL;
        } else {
            warnThreshold = CVarGetInteger("gEnhancements.Content.QuotaWarnThreshold", 90);
            if (warnThreshold > 0 && count >= warnThreshold) {
                *warnings |= GDX_IMPORT_WARN_QUOTA;
            }
        }
    }

    freeSize = Mfs_RamGetFreeSize();
    if (freeSize >= 0 && freeSize < payloadSize) {
        *errors |= GDX_IMPORT_ERR_DISK_SPACE;
    }
}

/* ---------------------------------------------------------------------------------
 * E4 cup-bundle validation. Same chain as single items (per-entry CRC -> the game's track
 * validators -> MFS name rules), plus bundle-structure checks and a whole-bundle quota count,
 * all BEFORE any write: a failed entry means no disk writes at all.
 * ------------------------------------------------------------------------------- */

/* Largest legal bundle payload: u16 count + 7 entry headers + CENT + 6 track payloads. */
#define GDX_IMPORT_BUNDLE_MAX_PAYLOAD                                                              \
    (2 + GDX_CONTENT_BUNDLE_ENTRY_HEADER_SIZE * (1 + GDX_CONTENT_BUNDLE_MAX_TRACKS) +              \
     GDX_CONTENT_CENT_PAYLOAD_SIZE + GDX_CONTENT_BUNDLE_MAX_TRACKS * GDX_CONTENT_TRACK_PAYLOAD_SIZE)

/* Live-MFS-state bundle checks: per-track overwrite/twin/quota, dangling CENT slots, free space
 * for the NEW files only, and the cup-replace warning (risk R6). Skipped wholesale while busy. */
static void gdx_import_check_disk_state_bundle(GdxContentImportEntry* entry, const char names[][21],
                                               int32_t trackCount, const uint8_t* cent, uint32_t* errors,
                                               uint32_t* warnings) {
    int32_t freeSize;
    int32_t needed;
    int count;
    int newNames = 0;
    int warnThreshold;
    int i;
    int j;

    if (gdx_content_mfs_busy()) {
        *errors |= GDX_IMPORT_ERR_STATE_UNKNOWN;
        entry->classFileCount = -1;
        return;
    }

    count = gdx_content_count_extension_class("CRS");
    if (count < 0) {
        *errors |= GDX_IMPORT_ERR_STATE_UNKNOWN;
        entry->classFileCount = -1;
        return;
    }
    entry->classFileCount = count;

    for (i = 0; i < trackCount; i++) {
        int crsd = gdx_content_file_exists(names[i], "CRSD");
        int crse = gdx_content_file_exists(names[i], "CRSE");
        if (crsd < 0 || crse < 0) {
            *errors |= GDX_IMPORT_ERR_STATE_UNKNOWN;
            return;
        }
        if (crsd > 0) {
            *warnings |= GDX_IMPORT_WARN_OVERWRITE;
        }
        if (crse > 0) {
            *warnings |= GDX_IMPORT_WARN_TWIN_DELETE;
        }
        /* A CRSE-only name is deleted and re-saved as CRSD: the class count does not grow. */
        if (crsd == 0 && crse == 0) {
            newNames++;
        }
    }

    /* Dangling-slot check: every non-empty Edit-Cup slot in the bundled CENT must name a
     * bundled track or a file already on disk -- anything else the game would silently clear. */
    for (i = 0; i < GDX_CONTENT_CUP_SLOT_COUNT; i++) {
        char slot[10];
        memcpy(slot, cent + i * 9, 9);
        slot[9] = '\0';
        if (slot[0] == '\0') {
            continue;
        }
        for (j = 0; j < trackCount; j++) {
            if (strcmp(slot, names[j]) == 0) {
                break;
            }
        }
        if (j != trackCount) {
            continue;
        }
        if (gdx_content_file_exists(slot, "CRSD") > 0 || gdx_content_file_exists(slot, "CRSE") > 0) {
            continue;
        }
        *errors |= GDX_IMPORT_ERR_BUNDLE_CENT;
    }

    if (newNames > 0) {
        if (count + newNames > 100) {
            *errors |= GDX_IMPORT_ERR_QUOTA_FULL;
        } else {
            warnThreshold = CVarGetInteger("gEnhancements.Content.QuotaWarnThreshold", 90);
            if (warnThreshold > 0 && count + newNames >= warnThreshold) {
                *warnings |= GDX_IMPORT_WARN_QUOTA;
            }
        }
    }

    for (i = 0; i < GDX_CONTENT_CUP_SLOT_COUNT; i++) {
        if (gEditCupTrackNames[i][0] != '\0') {
            *warnings |= GDX_IMPORT_WARN_CUP_REPLACE;
            break;
        }
    }

    freeSize = Mfs_RamGetFreeSize();
    needed = newNames * (int32_t) GDX_CONTENT_TRACK_PAYLOAD_SIZE + GDX_CONTENT_CENT_PAYLOAD_SIZE;
    if (freeSize >= 0 && freeSize < needed) {
        *errors |= GDX_IMPORT_ERR_DISK_SPACE;
    }
}

/* Header + entry-table checks. The manifest (track names + CENT image) is always filled for the
 * disk-state check and the UI; payloads are retained only when outPayloads is non-NULL (the
 * begin path stages them straight into the buffer the disk thread installs). */
static uint32_t gdx_import_validate_bundle_container(const char* path, GdxContentImportEntry* entry,
                                                     int32_t* outTrackCount, char outNames[][21],
                                                     uint8_t* outCent,
                                                     uint8_t (*outPayloads)[GDX_CONTENT_TRACK_PAYLOAD_SIZE]) {
    unsigned char header[GDX_CONTENT_HEADER_SIZE];
    uint8_t* payload = NULL;
    uint32_t errors = 0;
    uint32_t version;
    uint32_t extTag;
    uint32_t payloadSize;
    uint32_t payloadCrc;
    uint32_t entryCount;
    uint32_t i;
    long fileSize;
    FILE* f;
    const unsigned char* cursor;
    const unsigned char* end;
    int32_t trackCount = 0;
    int haveCent = 0;

    *outTrackCount = 0;

    f = fopen(path, "rb");
    if (f == NULL) {
        return GDX_IMPORT_ERR_IO;
    }
    if (fseek(f, 0, SEEK_END) != 0 || (fileSize = ftell(f)) < 0 || fseek(f, 0, SEEK_SET) != 0 ||
        fileSize < GDX_CONTENT_HEADER_SIZE || fread(header, 1, sizeof(header), f) != sizeof(header)) {
        fclose(f);
        return GDX_IMPORT_ERR_IO;
    }

    if (memcmp(header, "GXC1", 4) != 0) {
        errors |= GDX_IMPORT_ERR_BAD_MAGIC;
    }
    version = gdx_import_read_u32le(header + 0x04);
    if (version != GDX_CONTENT_FORMAT_VERSION) {
        errors |= GDX_IMPORT_ERR_BAD_VERSION;
    }

    entry->contentType = (int32_t) gdx_import_read_u32le(header + 0x08);
    extTag = gdx_import_read_u32le(header + 0x0C);
    if (entry->contentType != GDX_CONTENT_TYPE_BUNDLE) {
        errors |= GDX_IMPORT_ERR_BAD_TYPE;
    }
    if (extTag == gdx_import_fourcc("CENT")) {
        memcpy(entry->extension, "CENT", 5);
    } else {
        entry->extension[0] = '\0';
        errors |= GDX_IMPORT_ERR_BAD_EXT;
    }

    memcpy(entry->name, header + 0x10, 20);
    entry->name[20] = '\0';
    entry->flags = gdx_import_read_u32le(header + 0x25);
    payloadSize = gdx_import_read_u32le(header + 0x29);
    payloadCrc = gdx_import_read_u32le(header + 0x2D);

    /* Exact-size rule, both directions, same as single items. */
    if (payloadSize > GDX_IMPORT_BUNDLE_MAX_PAYLOAD ||
        (long) (GDX_CONTENT_HEADER_SIZE + payloadSize) != fileSize) {
        errors |= GDX_IMPORT_ERR_BAD_SIZE;
    }

    if (errors != 0) {
        fclose(f);
        return errors;
    }

    payload = (uint8_t*) malloc(payloadSize);
    if (payload == NULL) {
        fclose(f);
        return GDX_IMPORT_ERR_IO;
    }
    if (fseek(f, GDX_CONTENT_HEADER_SIZE, SEEK_SET) != 0 || fread(payload, 1, payloadSize, f) != payloadSize) {
        free(payload);
        fclose(f);
        return errors | GDX_IMPORT_ERR_IO;
    }
    fclose(f);

    /* Whole-payload CRC first; parsing continues regardless (it is bounds-checked) so the
     * report stays complete, same policy as the single-item chain. */
    if (gdx_content_crc32(payload, payloadSize) != payloadCrc) {
        errors |= GDX_IMPORT_ERR_BAD_CRC;
    }

    if (payloadSize < 2) {
        free(payload);
        return errors | GDX_IMPORT_ERR_BUNDLE_STRUCTURE;
    }
    entryCount = gdx_import_read_u16le(payload);
    /* At least the CENT plus one track; at most one track per Edit-Cup slot. */
    if (entryCount < 2 || entryCount > 1 + GDX_CONTENT_BUNDLE_MAX_TRACKS) {
        errors |= GDX_IMPORT_ERR_BUNDLE_STRUCTURE;
    }

    cursor = payload + 2;
    end = payload + payloadSize;
    for (i = 0; i < entryCount && (errors & GDX_IMPORT_ERR_BUNDLE_STRUCTURE) == 0; i++) {
        uint32_t entExt;
        uint32_t entSize;
        uint32_t entCrc;
        const unsigned char* entPayload;
        char name[21];

        if ((size_t) (end - cursor) < GDX_CONTENT_BUNDLE_ENTRY_HEADER_SIZE) {
            errors |= GDX_IMPORT_ERR_BUNDLE_STRUCTURE;
            break;
        }
        entExt = gdx_import_read_u32le(cursor);
        memcpy(name, cursor + 4, 20);
        name[20] = '\0';
        entSize = gdx_import_read_u32le(cursor + 25);
        entCrc = gdx_import_read_u32le(cursor + 29);
        cursor += GDX_CONTENT_BUNDLE_ENTRY_HEADER_SIZE;
        if (entSize > (uint32_t) (end - cursor)) {
            errors |= GDX_IMPORT_ERR_BUNDLE_STRUCTURE;
            break;
        }
        entPayload = cursor;
        cursor += entSize;

        if (i == 0) {
            /* Entry 0 must be the CENT image the bundle was built around. */
            if (entExt != gdx_import_fourcc("CENT") || strcmp(name, "CRS_ENTRY") != 0 ||
                entSize != GDX_CONTENT_CENT_PAYLOAD_SIZE) {
                errors |= GDX_IMPORT_ERR_BUNDLE_CENT;
                continue;
            }
            if (gdx_content_crc32(entPayload, entSize) != entCrc) {
                errors |= GDX_IMPORT_ERR_BAD_CRC;
            }
            memcpy(outCent, entPayload, GDX_CONTENT_CENT_PAYLOAD_SIZE);
            haveCent = 1;
            continue;
        }

        if (entExt != gdx_import_fourcc("CRSD")) {
            errors |= GDX_IMPORT_ERR_BAD_EXT;
        }
        if (entSize != GDX_CONTENT_TRACK_PAYLOAD_SIZE) {
            errors |= GDX_IMPORT_ERR_BAD_SIZE;
        }
        if (entSize == GDX_CONTENT_TRACK_PAYLOAD_SIZE && gdx_content_crc32(entPayload, entSize) != entCrc) {
            errors |= GDX_IMPORT_ERR_BAD_CRC;
        }
        if (Mfs_ValidateFileName(name) != 0) {
            errors |= GDX_IMPORT_ERR_BAD_NAME;
        }
        /* A cup slot holds 8 chars + NUL; a longer track name can never be cup-registered. */
        if (strlen(name) > GDX_CONTENT_CUP_NAME_MAX) {
            errors |= GDX_IMPORT_ERR_BUNDLE_CENT;
        }
        /* The same track twice in one bundle is malformed (export dedupes by name). */
        {
            int32_t k;
            for (k = 0; k < trackCount; k++) {
                if (strcmp(outNames[k], name) == 0) {
                    errors |= GDX_IMPORT_ERR_BUNDLE_STRUCTURE;
                    break;
                }
            }
        }
        if (entSize == GDX_CONTENT_TRACK_PAYLOAD_SIZE) {
            gdx_import_validate_track(entPayload, &errors);
        }
        if (trackCount < GDX_CONTENT_BUNDLE_MAX_TRACKS) {
            memcpy(outNames[trackCount], name, sizeof(outNames[trackCount]));
            if (outPayloads != NULL && entSize == GDX_CONTENT_TRACK_PAYLOAD_SIZE) {
                memcpy(outPayloads[trackCount], entPayload, GDX_CONTENT_TRACK_PAYLOAD_SIZE);
            }
            trackCount++;
            /* ", "-joined contents for the two-click confirm prompt. */
            {
                size_t used = strlen(entry->bundleContents);
                if (used != 0 && used + 2 < sizeof(entry->bundleContents)) {
                    entry->bundleContents[used++] = ',';
                    entry->bundleContents[used++] = ' ';
                    entry->bundleContents[used] = '\0';
                }
                if (used < sizeof(entry->bundleContents)) {
                    snprintf(entry->bundleContents + used, sizeof(entry->bundleContents) - used, "%s", name);
                }
            }
        }
    }
    if (cursor != end) {
        errors |= GDX_IMPORT_ERR_BUNDLE_STRUCTURE;
    }
    if (!haveCent) {
        errors |= GDX_IMPORT_ERR_BUNDLE_CENT;
    }
    if (trackCount == 0 && (errors & GDX_IMPORT_ERR_BUNDLE_STRUCTURE) == 0) {
        errors |= GDX_IMPORT_ERR_BUNDLE_STRUCTURE; /* CENT-only bundles are not produced or accepted */
    }

    free(payload);
    *outTrackCount = trackCount;
    return errors;
}

static void gdx_import_validate_bundle(const char* path, GdxContentImportEntry* entry, int32_t* outTrackCount,
                                       char outNames[][21], uint8_t* outCent,
                                       uint8_t (*outPayloads)[GDX_CONTENT_TRACK_PAYLOAD_SIZE]) {
    uint32_t warnings = 0;

    entry->errors = gdx_import_validate_bundle_container(path, entry, outTrackCount, outNames, outCent, outPayloads);
    if ((entry->errors & (GDX_IMPORT_ERR_IO | GDX_IMPORT_ERR_BAD_TYPE | GDX_IMPORT_ERR_BAD_EXT |
                          GDX_IMPORT_ERR_BUNDLE_STRUCTURE)) == 0 &&
        *outTrackCount > 0) {
        gdx_import_check_disk_state_bundle(entry, outNames, *outTrackCount, outCent, &entry->errors, &warnings);
    } else {
        entry->errors |= GDX_IMPORT_ERR_STATE_UNKNOWN;
    }
    entry->warnings = warnings;
}

static void gdx_import_validate_all(const char* path, GdxContentImportEntry* entry, uint8_t* payloadOut) {
    uint32_t warnings = 0;

    memset(entry->name, 0, sizeof(entry->name));
    memset(entry->extension, 0, sizeof(entry->extension));
    entry->contentType = 0;
    entry->flags = 0;
    entry->classFileCount = -1;
    entry->bundleTrackCount = -1;
    entry->bundleContents[0] = '\0';

    if (gdx_import_peek_content_type(path) == GDX_CONTENT_TYPE_BUNDLE) {
        int32_t trackCount = 0;
        char names[GDX_CONTENT_BUNDLE_MAX_TRACKS][21];
        uint8_t cent[GDX_CONTENT_CENT_PAYLOAD_SIZE];
        gdx_import_validate_bundle(path, entry, &trackCount, names, cent, NULL);
        entry->bundleTrackCount = trackCount;
        return;
    }

    entry->errors = gdx_import_validate_container(path, entry, payloadOut);
    if ((entry->errors & (GDX_IMPORT_ERR_IO | GDX_IMPORT_ERR_BAD_TYPE | GDX_IMPORT_ERR_BAD_EXT |
                          GDX_IMPORT_ERR_TYPE_EXT_MISMATCH | GDX_IMPORT_ERR_BAD_NAME)) == 0) {
        gdx_import_check_disk_state(entry, &entry->errors, &warnings);
    } else {
        entry->errors |= GDX_IMPORT_ERR_STATE_UNKNOWN;
    }
    entry->warnings = warnings;
}

/* ---------------------------------------------------------------------------------
 * exports/ enumeration (same host-directory pattern as gdx_ghost_io.c's library list).
 * ------------------------------------------------------------------------------- */

int gdx_content_import_list(GdxContentImportEntry* outEntries, int capacity) {
    char directory[1200];
    int count = 0;

    if (outEntries == NULL || capacity < 0) {
        return GDX_CONTENT_ERR_BAD_ARGS;
    }
    if (capacity == 0 || !gdx_content_exports_directory(directory, sizeof(directory), 0)) {
        return 0;
    }

#ifdef _WIN32
    {
        WIN32_FIND_DATAA findData;
        HANDLE findHandle;
        char pattern[1400];
        int n = snprintf(pattern, sizeof(pattern), "%s\\*.gdxc", directory);
        if (n < 0 || (size_t) n >= sizeof(pattern)) {
            return GDX_CONTENT_ERR_IO;
        }
        findHandle = FindFirstFileA(pattern, &findData);
        if (findHandle == INVALID_HANDLE_VALUE) {
            return 0;
        }
        do {
            char path[1600];
            if ((findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0 || count >= capacity) {
                continue;
            }
            n = snprintf(path, sizeof(path), "%s\\%s", directory, findData.cFileName);
            if (n < 0 || (size_t) n >= sizeof(path)) {
                continue;
            }
            snprintf(outEntries[count].fileName, sizeof(outEntries[count].fileName), "%s", findData.cFileName);
            gdx_import_validate_all(path, &outEntries[count], NULL);
            count++;
        } while (FindNextFileA(findHandle, &findData));
        FindClose(findHandle);
    }
#else
    {
        DIR* dir = opendir(directory);
        struct dirent* item;
        if (dir == NULL) {
            return errno == ENOENT ? 0 : GDX_CONTENT_ERR_IO;
        }
        while ((item = readdir(dir)) != NULL && count < capacity) {
            char path[1600];
            size_t nameLen = strlen(item->d_name);
            int n;
            if (nameLen < 6 || strcmp(item->d_name + nameLen - 5, ".gdxc") != 0) {
                continue;
            }
            n = snprintf(path, sizeof(path), "%s/%s", directory, item->d_name);
            if (n < 0 || (size_t) n >= sizeof(path)) {
                continue;
            }
            snprintf(outEntries[count].fileName, sizeof(outEntries[count].fileName), "%s", item->d_name);
            gdx_import_validate_all(path, &outEntries[count], NULL);
            count++;
        }
        closedir(dir);
    }
#endif

    return count;
}

/* ---------------------------------------------------------------------------------
 * Install: staged on the menu thread, executed on the game's disk thread (risk R3). On the
 * port osSendMesg hands off into the priority-30 disk worker synchronously (see the PORT note
 * in decomp/src/sys/disk/75000.c), so a begun import has normally completed before
 * gdx_content_import_begin returns; the state machine still treats it as async.
 * ------------------------------------------------------------------------------- */

enum {
    GDX_IMPORT_IDLE = 0,
    GDX_IMPORT_PENDING = 1,
    GDX_IMPORT_DONE = 2
};

static volatile int32_t sImportState = GDX_IMPORT_IDLE;
static volatile int32_t sImportRc = 0;
static volatile int32_t sImportMfsError = 0;
static volatile int32_t sImportCupCleared = 0;

static char sImportName[21];
static char sImportExtension[6];
static int32_t sImportContentType;
static uint32_t sImportPayloadSize;
static uint8_t sImportPayload[GDX_CONTENT_TRACK_PAYLOAD_SIZE];

/* Runs on the game's disk thread, dispatched from op slot 30 in decomp/src/sys/disk/75000.c.
 * Mfs_SaveFile does the __TMP__ swap for same-name overwrites internally (mfs_save.c:182-186)
 * and the .gdd journal persists the write (n64_leo.c:175-176). */
void GdxContentImport_RunOnDiskThread(void) {
    int32_t rc;
    int32_t cupCleared = 0;

    if (sImportContentType == GDX_CONTENT_TYPE_TRACK) {
        /* Twin-extension delete first, same order as the editor's save op. A missing twin is
         * fine: Mfs_DeleteFileInDir just returns -1/N64DD_NOT_FOUND, and Mfs_SaveFile resets
         * gMfsError on entry. */
        Mfs_DeleteFileInDir(gWorkingDirectory, sImportName,
                            (char*) (sImportExtension[3] == 'D' ? "CRSE" : "CRSD"), 1);
    }

    rc = Mfs_SaveFile(gWorkingDirectory, sImportName, sImportExtension, sImportPayload, sImportPayloadSize, 0, 0xFF,
                      1);

    if (rc == 0 && sImportContentType == GDX_CONTENT_TYPE_TRACK && sImportExtension[3] == 'E') {
        /* CRSE import: clear edit-cup slots naming it, mirroring the editor's post-save clear
         * (19DD60.c case 0x12 -> func_xk2_800EBFE8) and persist CRS_ENTRY.CENT like
         * func_xk2_800EC110. The write happens only when a slot actually matched -- an
         * all-zero (never-loaded) in-memory cup can never match a validated non-empty name, so
         * a stale gEditCupTrackNames is never written back over the user's CENT. */
        int i;
        for (i = 0; i < 6; i++) {
            char slot[10];
            memcpy(slot, gEditCupTrackNames[i], 9);
            slot[9] = '\0';
            if (slot[0] != '\0' && strcmp(slot, sImportName) == 0) {
                gEditCupTrackNames[i][0] = '\0';
                cupCleared++;
            }
        }
        if (cupCleared > 0) {
            Mfs_SaveFile(gWorkingDirectory, "CRS_ENTRY", "CENT", (uint8_t*) gEditCupTrackNames,
                         sizeof(gEditCupTrackNames), 0, 0xFF, 1);
        }
    }

    sImportCupCleared = cupCleared;
    sImportMfsError = gMfsError;
    sImportRc = rc;
    sImportState = GDX_IMPORT_DONE;
}

/* ---------------------------------------------------------------------------------
 * E4 bundle install staging (op 32). The validated payloads land here on the menu thread; the
 * disk thread consumes them. Validation is all-or-nothing, so a begun install has already
 * passed every check; a mid-install MFS failure stops at the failed entry and is reported.
 * ------------------------------------------------------------------------------- */

static int32_t sBundleTrackCount;
static char sBundleNames[GDX_CONTENT_BUNDLE_MAX_TRACKS][21];
static uint8_t sBundleCent[GDX_CONTENT_CENT_PAYLOAD_SIZE];
static uint8_t sBundlePayloads[GDX_CONTENT_BUNDLE_MAX_TRACKS][GDX_CONTENT_TRACK_PAYLOAD_SIZE];
static volatile int32_t sBundleFailedIndex = -1;

/* Runs on the game's disk thread, dispatched from op slot 32 in decomp/src/sys/disk/75000.c. */
void GdxContentBundle_RunOnDiskThread(void) {
    int32_t rc = 0;
    int32_t failedIndex = -1;
    int32_t i;

    for (i = 0; i < sBundleTrackCount; i++) {
        /* Twin CRSE delete first, same order as the editor's save op. A missing twin is fine:
         * Mfs_DeleteFileInDir just returns -1/N64DD_NOT_FOUND, and Mfs_SaveFile resets gMfsError
         * on entry. */
        Mfs_DeleteFileInDir(gWorkingDirectory, sBundleNames[i], "CRSE", 1);
        rc = Mfs_SaveFile(gWorkingDirectory, sBundleNames[i], "CRSD", sBundlePayloads[i],
                          GDX_CONTENT_TRACK_PAYLOAD_SIZE, 0, 0xFF, 1);
        if (rc != 0) {
            failedIndex = i;
            break;
        }
    }
    if (rc == 0) {
        /* Only the 6 Edit-Cup rows merge; rows 6..23 of the in-memory image hold the importer's
         * other cups and keep their state. */
        for (i = 0; i < GDX_CONTENT_CUP_SLOT_COUNT; i++) {
            memcpy(gEditCupTrackNames[i], sBundleCent + i * 9, 9);
        }
        rc = Mfs_SaveFile(gWorkingDirectory, "CRS_ENTRY", "CENT", (uint8_t*) gEditCupTrackNames,
                          sizeof(gEditCupTrackNames), 0, 0xFF, 1);
        if (rc != 0) {
            failedIndex = sBundleTrackCount; /* the CENT write itself failed */
        }
    }

    sBundleFailedIndex = failedIndex;
    sImportMfsError = gMfsError;
    sImportRc = rc;
    sImportState = GDX_IMPORT_DONE;
}

/* ---------------------------------------------------------------------------------
 * E3 Edit-Cup register/unregister staging (op 31). gEditCupTrackNames is mutated only here, on
 * the disk thread -- the game reads it constantly, so a menu-thread write would be a race.
 * ------------------------------------------------------------------------------- */

enum {
    GDX_CUP_IDLE = 0,
    GDX_CUP_PENDING = 1,
    GDX_CUP_DONE = 2
};

static volatile int32_t sCupState = GDX_CUP_IDLE;
static volatile int32_t sCupRc = 0;
static volatile int32_t sCupMfsError = 0;
static char sCupSlotName[GDX_CONTENT_CUP_NAME_MAX + 1]; /* NUL-padded to the full 9-byte slot */
static int32_t sCupSlot = 0;
static int32_t sCupMode = 0; /* 1 = register, 0 = unregister */

/* Runs on the game's disk thread, dispatched from op slot 31 in decomp/src/sys/disk/75000.c.
 * The CENT persist mirrors func_xk2_800EC110 (course_edit/19D4A0.c:176-181), minus the input
 * lock the in-editor flow needs. */
void GdxContentCup_RunOnDiskThread(void) {
    int32_t rc;

    if (sCupMode == 1) {
        memset(gEditCupTrackNames[sCupSlot], 0, sizeof(gEditCupTrackNames[sCupSlot]));
        memcpy(gEditCupTrackNames[sCupSlot], sCupSlotName, sizeof(sCupSlotName));
    } else {
        gEditCupTrackNames[sCupSlot][0] = '\0';
    }
    rc = Mfs_SaveFile(gWorkingDirectory, "CRS_ENTRY", "CENT", (uint8_t*) gEditCupTrackNames,
                      sizeof(gEditCupTrackNames), 0, 0xFF, 1);

    sCupMfsError = gMfsError;
    sCupRc = rc;
    sCupState = GDX_CUP_DONE;
}

int gdx_content_import_begin(const char* fileName) {
    char directory[1200];
    char path[1600];
    GdxContentImportEntry entry;
    int n;

    if (fileName == NULL || fileName[0] == '\0' || strchr(fileName, '/') != NULL ||
        strchr(fileName, '\\') != NULL || strstr(fileName, "..") != NULL) {
        return GDX_CONTENT_ERR_BAD_ARGS;
    }
    if (gdx_input_in_gameplay()) {
        return GDX_CONTENT_IMPORT_ERR_IN_GAMEPLAY;
    }
    if (sImportState == GDX_IMPORT_PENDING || sCupState == GDX_CUP_PENDING) {
        return GDX_CONTENT_IMPORT_ERR_PENDING;
    }
    if (gdx_content_mfs_busy()) {
        return GDX_CONTENT_ERR_BUSY;
    }
    if (!gdx_content_exports_directory(directory, sizeof(directory), 0)) {
        return GDX_CONTENT_ERR_IO;
    }
#ifdef _WIN32
    n = snprintf(path, sizeof(path), "%s\\%s", directory, fileName);
#else
    n = snprintf(path, sizeof(path), "%s/%s", directory, fileName);
#endif
    if (n < 0 || (size_t) n >= sizeof(path)) {
        return GDX_CONTENT_ERR_BAD_ARGS;
    }

    if (gdx_import_peek_content_type(path) == GDX_CONTENT_TYPE_BUNDLE) {
        /* Bundle: re-read and re-validate every entry from disk; validated payloads stage
         * straight into the buffers the disk thread installs. All-or-nothing: any error bit
         * anywhere in the bundle means no writes at all. */
        memset(&entry, 0, sizeof(entry));
        entry.bundleContents[0] = '\0';
        sBundleTrackCount = 0;
        sBundleFailedIndex = -1;
        gdx_import_validate_bundle(path, &entry, &sBundleTrackCount, sBundleNames, sBundleCent, sBundlePayloads);
        if (entry.errors != 0) {
            sBundleTrackCount = 0;
            return GDX_CONTENT_IMPORT_ERR_VALIDATION;
        }
        sImportRc = 0;
        sImportMfsError = 0;
        sImportCupCleared = 0;
        sImportState = GDX_IMPORT_PENDING;
        if (GdxContentBundle_EnqueueDiskOp() != 0) {
            sImportState = GDX_IMPORT_IDLE;
            return GDX_CONTENT_ERR_BUSY;
        }
        return GDX_CONTENT_OK;
    }

    /* Re-read and re-validate from disk: never trust the menu's earlier snapshot. The validated
     * payload lands straight in the staging buffer the disk thread will install. */
    gdx_import_validate_all(path, &entry, sImportPayload);
    if (entry.errors != 0) {
        return GDX_CONTENT_IMPORT_ERR_VALIDATION;
    }

    memcpy(sImportName, entry.name, sizeof(sImportName));
    memcpy(sImportExtension, entry.extension, sizeof(sImportExtension));
    sImportContentType = entry.contentType;
    sImportPayloadSize = (uint32_t) gdx_import_expected_payload_size(entry.contentType);
    sImportRc = 0;
    sImportMfsError = 0;
    sImportCupCleared = 0;
    sImportState = GDX_IMPORT_PENDING;

    if (GdxContentImport_EnqueueDiskOp() != 0) {
        sImportState = GDX_IMPORT_IDLE;
        return GDX_CONTENT_ERR_BUSY;
    }
    return GDX_CONTENT_OK;
}

int gdx_content_import_pending(void) {
    return sImportState == GDX_IMPORT_PENDING ? 1 : 0;
}

int gdx_content_import_result(int32_t* outMfsError) {
    int32_t rc;
    if (sImportState == GDX_IMPORT_PENDING) {
        return GDX_CONTENT_ERR_BUSY;
    }
    if (sImportState != GDX_IMPORT_DONE) {
        return GDX_CONTENT_ERR_NOT_FOUND;
    }
    rc = sImportRc;
    if (outMfsError != NULL) {
        *outMfsError = sImportMfsError;
    }
    sImportState = GDX_IMPORT_IDLE;
    return rc == 0 ? GDX_CONTENT_OK : GDX_CONTENT_ERR_IO;
}

/* After a failed bundle install (result == GDX_CONTENT_ERR_IO): the bundled entry index whose
 * write failed (sBundleTrackCount = the CENT write itself), or -1 when not applicable. Earlier
 * entries were already written -- MFS has no multi-file transaction, so mid-install failures
 * leave a partial install; the admission gate is the pre-write validation. */
int32_t gdx_content_import_bundle_failed_index(void) {
    return sBundleFailedIndex;
}

/* ---------------------------------------------------------------------------------
 * A2 direct-payload import (disk-image source). Content read out of a foreign 64DD image by
 * gdx_mfs_image.c arrives as raw payload bytes, so the container-only checks (magic, version,
 * CRC) do not apply -- the image's own MFS volume checksum and FAT walk already gated the bytes.
 * Everything else runs unchanged and in the container's order: type -> ext -> pairing -> exact
 * size -> Mfs_ValidateFileName -> the game's payload validators -> live disk state.
 * ------------------------------------------------------------------------------- */

static uint32_t gdx_import_validate_direct(const char* name, const char* extension, int32_t contentType,
                                           const uint8_t* payload, int32_t payloadSize, GdxContentImportEntry* entry,
                                           uint32_t* warnings) {
    uint32_t errors = 0;
    int32_t expectedSize;

    entry->contentType = contentType;
    entry->flags = 0;

    if (contentType != GDX_CONTENT_TYPE_TRACK && contentType != GDX_CONTENT_TYPE_MACHINE) {
        errors |= GDX_IMPORT_ERR_BAD_TYPE;
    }
    if (strcmp(extension, "CRSD") == 0 || strcmp(extension, "CRSE") == 0 || strcmp(extension, "CARD") == 0) {
        memcpy(entry->extension, extension, 5);
        entry->extension[5] = '\0';
    } else {
        entry->extension[0] = '\0';
        errors |= GDX_IMPORT_ERR_BAD_EXT;
    }
    /* Pairing: tracks carry CRSD/CRSE ('S' at [2]), machines carry CARD. */
    if ((errors & (GDX_IMPORT_ERR_BAD_TYPE | GDX_IMPORT_ERR_BAD_EXT)) == 0 &&
        ((contentType == GDX_CONTENT_TYPE_TRACK) != (entry->extension[2] == 'S'))) {
        errors |= GDX_IMPORT_ERR_TYPE_EXT_MISMATCH;
    }

    snprintf(entry->name, sizeof(entry->name), "%s", name);
    if (Mfs_ValidateFileName(entry->name) != 0) {
        errors |= GDX_IMPORT_ERR_BAD_NAME;
    }

    expectedSize = (errors & (GDX_IMPORT_ERR_BAD_TYPE | GDX_IMPORT_ERR_BAD_EXT)) == 0
                       ? gdx_import_expected_payload_size(contentType)
                       : -1;
    if (expectedSize < 0 || payloadSize != expectedSize) {
        errors |= GDX_IMPORT_ERR_BAD_SIZE;
    } else if (contentType == GDX_CONTENT_TYPE_TRACK) {
        gdx_import_validate_track(payload, &errors);
    } else {
        gdx_import_validate_machine(payload, &errors);
    }

    /* Same gate as validate_all: live disk state is probed only when the identity checks passed. */
    if ((errors & (GDX_IMPORT_ERR_BAD_TYPE | GDX_IMPORT_ERR_BAD_EXT | GDX_IMPORT_ERR_TYPE_EXT_MISMATCH |
                   GDX_IMPORT_ERR_BAD_NAME)) == 0) {
        gdx_import_check_disk_state(entry, &errors, warnings);
    } else {
        errors |= GDX_IMPORT_ERR_STATE_UNKNOWN;
    }
    return errors;
}

void gdx_content_import_validate_payload(const char* name, const char* extension, int32_t contentType,
                                         const uint8_t* payload, int32_t payloadSize, GdxContentImportEntry* outEntry) {
    uint32_t warnings = 0;

    if (outEntry == NULL) {
        return;
    }
    memset(outEntry, 0, sizeof(*outEntry));
    outEntry->classFileCount = -1;
    outEntry->bundleTrackCount = -1;
    if (name == NULL || name[0] == '\0' || strlen(name) > 20) {
        outEntry->errors = GDX_IMPORT_ERR_BAD_NAME | GDX_IMPORT_ERR_STATE_UNKNOWN;
        return;
    }
    if (extension == NULL) {
        outEntry->errors = GDX_IMPORT_ERR_BAD_EXT | GDX_IMPORT_ERR_STATE_UNKNOWN;
        return;
    }
    if (payload == NULL || payloadSize <= 0) {
        outEntry->errors = GDX_IMPORT_ERR_BAD_SIZE | GDX_IMPORT_ERR_STATE_UNKNOWN;
        return;
    }
    outEntry->errors =
        gdx_import_validate_direct(name, extension, contentType, payload, payloadSize, outEntry, &warnings);
    outEntry->warnings = warnings;
}

int gdx_content_import_begin_payload(const char* name, const char* extension, int32_t contentType,
                                     const uint8_t* payload, int32_t payloadSize) {
    GdxContentImportEntry entry;
    uint32_t warnings = 0;

    if (name == NULL || name[0] == '\0' || strlen(name) > 20 || extension == NULL || payload == NULL ||
        payloadSize <= 0 || payloadSize > GDX_CONTENT_TRACK_PAYLOAD_SIZE) {
        return GDX_CONTENT_ERR_BAD_ARGS;
    }
    if (gdx_input_in_gameplay()) {
        return GDX_CONTENT_IMPORT_ERR_IN_GAMEPLAY;
    }
    if (sImportState == GDX_IMPORT_PENDING || sCupState == GDX_CUP_PENDING) {
        return GDX_CONTENT_IMPORT_ERR_PENDING;
    }
    if (gdx_content_mfs_busy()) {
        return GDX_CONTENT_ERR_BUSY;
    }

    /* Re-validate what was handed in: never trust the menu's earlier snapshot. The payload then
     * stages straight into the buffer the disk thread installs. */
    memset(&entry, 0, sizeof(entry));
    entry.classFileCount = -1;
    entry.bundleTrackCount = -1;
    entry.errors = gdx_import_validate_direct(name, extension, contentType, payload, payloadSize, &entry, &warnings);
    if (entry.errors != 0) {
        return GDX_CONTENT_IMPORT_ERR_VALIDATION;
    }

    memcpy(sImportName, entry.name, sizeof(sImportName));
    memcpy(sImportExtension, entry.extension, sizeof(sImportExtension));
    sImportContentType = entry.contentType;
    sImportPayloadSize = (uint32_t) gdx_import_expected_payload_size(entry.contentType);
    memcpy(sImportPayload, payload, sImportPayloadSize);
    sImportRc = 0;
    sImportMfsError = 0;
    sImportCupCleared = 0;
    sImportState = GDX_IMPORT_PENDING;

    if (GdxContentImport_EnqueueDiskOp() != 0) {
        sImportState = GDX_IMPORT_IDLE;
        return GDX_CONTENT_ERR_BUSY;
    }
    return GDX_CONTENT_OK;
}

/* ---------------------------------------------------------------------------------
 * E3 Edit-Cup registration API (menu-thread entry points; the disk-thread runner sits with the
 * staging above).
 * ------------------------------------------------------------------------------- */

int gdx_content_cup_list(char outNames[GDX_CONTENT_CUP_SLOT_COUNT][GDX_CONTENT_CUP_NAME_MAX + 1]) {
    int i;

    if (outNames == NULL) {
        return GDX_CONTENT_ERR_BAD_ARGS;
    }
    if (gdx_content_mfs_busy()) {
        return GDX_CONTENT_ERR_BUSY;
    }
    if (gDirectoryEntryCount <= 0) {
        return GDX_CONTENT_ERR_NO_FS;
    }
    for (i = 0; i < GDX_CONTENT_CUP_SLOT_COUNT; i++) {
        memcpy(outNames[i], gEditCupTrackNames[i], 9);
        outNames[i][GDX_CONTENT_CUP_NAME_MAX] = '\0'; /* slot is 9 bytes; force the terminator */
    }
    return GDX_CONTENT_OK;
}

static int gdx_content_cup_begin_common(int32_t slot, int32_t mode) {
    if (slot < 0 || slot >= GDX_CONTENT_CUP_SLOT_COUNT) {
        return GDX_CONTENT_CUP_ERR_BAD_SLOT;
    }
    if (gdx_input_in_gameplay()) {
        return GDX_CONTENT_IMPORT_ERR_IN_GAMEPLAY;
    }
    if (sCupState == GDX_CUP_PENDING || sImportState == GDX_IMPORT_PENDING) {
        return GDX_CONTENT_IMPORT_ERR_PENDING;
    }
    if (gdx_content_mfs_busy()) {
        return GDX_CONTENT_ERR_BUSY;
    }
    sCupSlot = slot;
    sCupMode = mode;
    sCupRc = 0;
    sCupMfsError = 0;
    sCupState = GDX_CUP_PENDING;
    if (GdxContentCup_EnqueueDiskOp() != 0) {
        sCupState = GDX_CUP_IDLE;
        return GDX_CONTENT_ERR_BUSY;
    }
    return GDX_CONTENT_OK;
}

int gdx_content_cup_register_begin(const char* name, int32_t slot) {
    size_t nameLen;

    if (name == NULL || name[0] == '\0') {
        return GDX_CONTENT_ERR_BAD_ARGS;
    }
    /* A cup slot is char[9]; mfsStrCpy does no truncation, and Mfs_GetFileIndex compares the
     * full fixed-width name, so a >8-char name would write a slot that never resolves and the
     * game silently clears (course_edit/19D4A0.c:26-32). Refuse it instead. */
    nameLen = strlen(name);
    if (nameLen > GDX_CONTENT_CUP_NAME_MAX) {
        return GDX_CONTENT_CUP_ERR_NAME_TOO_LONG;
    }
    if (Mfs_ValidateFileName((char*) name) != 0) {
        return GDX_CONTENT_ERR_BAD_ARGS;
    }
    /* Pending/busy before the existence probe: gdx_content_file_exists reports a busy disk as
     * an error code, which would otherwise surface as a misleading NOT_FOUND. */
    if (sCupState == GDX_CUP_PENDING || sImportState == GDX_IMPORT_PENDING) {
        return GDX_CONTENT_IMPORT_ERR_PENDING;
    }
    if (gdx_content_mfs_busy()) {
        return GDX_CONTENT_ERR_BUSY;
    }
    if (gdx_content_file_exists(name, "CRSD") != 1) {
        return GDX_CONTENT_ERR_NOT_FOUND; /* CRSE tracks are ineligible (19C470.c:457-461) */
    }

    memset(sCupSlotName, 0, sizeof(sCupSlotName));
    memcpy(sCupSlotName, name, nameLen);
    return gdx_content_cup_begin_common(slot, 1);
}

int gdx_content_cup_unregister_begin(int32_t slot) {
    if (slot >= 0 && slot < GDX_CONTENT_CUP_SLOT_COUNT && !gdx_content_mfs_busy() &&
        gDirectoryEntryCount > 0 && gEditCupTrackNames[slot][0] == '\0') {
        return GDX_CONTENT_CUP_ERR_EMPTY_SLOT;
    }
    return gdx_content_cup_begin_common(slot, 0);
}

int gdx_content_cup_pending(void) {
    return sCupState == GDX_CUP_PENDING ? 1 : 0;
}

int gdx_content_cup_result(int32_t* outMfsError) {
    int32_t rc;
    if (sCupState == GDX_CUP_PENDING) {
        return GDX_CONTENT_ERR_BUSY;
    }
    if (sCupState != GDX_CUP_DONE) {
        return GDX_CONTENT_ERR_NOT_FOUND;
    }
    rc = sCupRc;
    if (outMfsError != NULL) {
        *outMfsError = sCupMfsError;
    }
    sCupState = GDX_CUP_IDLE;
    return rc == 0 ? GDX_CONTENT_OK : GDX_CONTENT_ERR_IO;
}

/* ---------------------------------------------------------------------------------
 * UI strings.
 * ------------------------------------------------------------------------------- */

static void gdx_import_append(char* out, size_t outCap, const char* text) {
    size_t used = strlen(out);
    if (used != 0 && used + 2 < outCap) {
        out[used++] = ';';
        out[used++] = ' ';
        out[used] = '\0';
    }
    if (used < outCap) {
        snprintf(out + used, outCap - used, "%s", text);
    }
}

void gdx_content_import_format_errors(uint32_t errors, char* out, size_t outCap) {
    if (outCap == 0) {
        return;
    }
    out[0] = '\0';
    if (errors == 0) {
        snprintf(out, outCap, "none");
        return;
    }
    if (errors & GDX_IMPORT_ERR_IO) gdx_import_append(out, outCap, "unreadable or truncated file");
    if (errors & GDX_IMPORT_ERR_BAD_MAGIC) gdx_import_append(out, outCap, "not a .gdxc container");
    if (errors & GDX_IMPORT_ERR_BAD_VERSION) gdx_import_append(out, outCap, "unsupported container version");
    if (errors & GDX_IMPORT_ERR_BAD_TYPE) gdx_import_append(out, outCap, "unknown content type");
    if (errors & GDX_IMPORT_ERR_BAD_EXT) gdx_import_append(out, outCap, "unknown extension tag");
    if (errors & GDX_IMPORT_ERR_TYPE_EXT_MISMATCH) gdx_import_append(out, outCap, "type/extension mismatch");
    if (errors & GDX_IMPORT_ERR_BAD_SIZE) gdx_import_append(out, outCap, "wrong payload size");
    if (errors & GDX_IMPORT_ERR_BAD_CRC) gdx_import_append(out, outCap, "payload CRC mismatch (corrupt file)");
    if (errors & GDX_IMPORT_ERR_TRACK_CHECKSUM) gdx_import_append(out, outCap, "course checksum mismatch");
    if (errors & GDX_IMPORT_ERR_TRACK_CREATOR) gdx_import_append(out, outCap, "course creator id mismatch");
    if (errors & GDX_IMPORT_ERR_TRACK_BGM) gdx_import_append(out, outCap, "course BGM out of range");
    if (errors & GDX_IMPORT_ERR_TRACK_GEOMETRY) gdx_import_append(out, outCap, "course geometry out of range");
    if (errors & GDX_IMPORT_ERR_MACHINE_CHECKSUM) gdx_import_append(out, outCap, "machine checksum mismatch");
    if (errors & GDX_IMPORT_ERR_MACHINE_STATS) gdx_import_append(out, outCap, "machine stats invalid");
    if (errors & GDX_IMPORT_ERR_BAD_NAME) gdx_import_append(out, outCap, "name not allowed by the disk filesystem");
    if (errors & GDX_IMPORT_ERR_QUOTA_FULL) gdx_import_append(out, outCap, "disk full: 100-file limit reached, delete something first");
    if (errors & GDX_IMPORT_ERR_DISK_SPACE) gdx_import_append(out, outCap, "not enough free disk space");
    if (errors & GDX_IMPORT_ERR_STATE_UNKNOWN) gdx_import_append(out, outCap, "disk busy; quota and collision not checked");
    if (errors & GDX_IMPORT_ERR_BUNDLE_STRUCTURE) gdx_import_append(out, outCap, "bundle entry table malformed");
    if (errors & GDX_IMPORT_ERR_BUNDLE_CENT) gdx_import_append(out, outCap, "bundle cup data invalid (missing CENT, dangling slot, or name over 8 chars)");
}

void gdx_content_import_format_warnings(uint32_t warnings, char* out, size_t outCap) {
    if (outCap == 0) {
        return;
    }
    out[0] = '\0';
    if (warnings == 0) {
        return;
    }
    if (warnings & GDX_IMPORT_WARN_QUOTA) gdx_import_append(out, outCap, "approaching the 100-file limit");
    if (warnings & GDX_IMPORT_WARN_OVERWRITE) gdx_import_append(out, outCap, "overwrites the existing file with this name");
    if (warnings & GDX_IMPORT_WARN_TWIN_DELETE) gdx_import_append(out, outCap, "deletes the opposite-variant twin on disk");
    if (warnings & GDX_IMPORT_WARN_CUP_CLEAR) gdx_import_append(out, outCap, "clears an Edit Cup slot naming this track");
    if (warnings & GDX_IMPORT_WARN_CUP_REPLACE) gdx_import_append(out, outCap, "replaces the current Edit Cup contents");
}
