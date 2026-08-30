/* port/gdx_rom_courses.c -- ROM course table reader and CRSD payload converter.
 *
 * Contract and provenance live in gdx_rom_courses.h. Same host-CRT target split as
 * gdx_mfs_image.c and gdx_content_io.c: this TU mirrors CourseData with size static_asserts
 * rather than including decomp headers, touches no game globals, has no write path, and is
 * fully reentrant.
 *
 * Byte order: N64 ROMs ship in three interleavings and the course table has to be read as
 * big-endian regardless, so the whole image is normalised to big-endian at load. The payload
 * handed back is host order, because gdx_content_import.c memcpy's it straight into a
 * GdxImportCourseData and reads the fields natively (gdx_content_import.c:234).
 */

#define _CRT_SECURE_NO_WARNINGS /* plain fopen/fread below; harmless on non-MSVC */

#include "gdx_rom_courses.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------------------------
 * CourseData mirror. Field offsets match fzx_course.h:14-34; the static_asserts below are the
 * same guard gdx_content_import.c:70-72 uses, so a layout drift is a build failure.
 * ------------------------------------------------------------------------------- */

typedef struct GdxRomVec3f {
    float x;
    float y;
    float z;
} GdxRomVec3f;

typedef struct GdxRomControlPoint {
    GdxRomVec3f pos;
    int16_t radiusLeft;
    int16_t radiusRight;
    int32_t trackSegmentInfo;
} GdxRomControlPoint;

typedef struct GdxRomCourseData {
    uint8_t creatorId;
    int8_t controlPointCount;
    int8_t venue;
    int8_t skybox;
    uint32_t checksum;
    uint8_t flag;
    char fileName[22];
    int8_t bgm;
    GdxRomControlPoint controlPoint[64];
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
} GdxRomCourseData;

typedef char gdx_romcourse_size_check_controlPoint[(sizeof(GdxRomControlPoint) == 0x14) ? 1 : -1];
typedef char gdx_romcourse_size_check_courseData[(sizeof(GdxRomCourseData) == GDX_ROMCOURSE_STRIDE) ? 1 : -1];
typedef char gdx_romcourse_size_check_bgmOffset[(offsetof(GdxRomCourseData, bgm) == 0x1F) ? 1 : -1];
typedef char gdx_romcourse_size_check_bankOffset[(offsetof(GdxRomCourseData, bankAngle) == 0x520) ? 1 : -1];

/* Field offsets inside the raw big-endian block, so the reader never depends on host padding. */
#define GDX_ROMCOURSE_OFF_CHECKSUM 0x004
#define GDX_ROMCOURSE_OFF_FILENAME 0x009
#define GDX_ROMCOURSE_OFF_BGM 0x01F
#define GDX_ROMCOURSE_OFF_CONTROLPOINTS 0x020
#define GDX_ROMCOURSE_OFF_BANKANGLE 0x520
#define GDX_ROMCOURSE_CONTROLPOINT_SIZE 0x14

/* CREATOR_NINTENDO, fzx_course.h:101. Every retail course carries it, and so does every FZEP
 * course, because FZEP writes into these same slots. */
#define GDX_ROMCOURSE_CREATOR_NINTENDO 4

/* The smallest ROM that could hold the table at all. Retail US rev0 is 16 MiB. */
#define GDX_ROMCOURSE_MIN_ROM_BYTES (GDX_ROMCOURSE_TABLE_OFFSET + GDX_ROMCOURSE_TABLE_BYTES)

/* The N64 header's two CRC words (CRC1 at 0x10, CRC2 at 0x14). Excluded from the non-course
 * comparison; see the comment at that comparison for why. */
#define GDX_ROMCOURSE_HEADER_CRC_OFFSET 0x10
#define GDX_ROMCOURSE_HEADER_CRC_BYTES 8

/* Stock slot names, in table order (decomp/assets/yaml/us/rev0/course_data.yaml). Reported for
 * display so a player can tell which course a hack replaced. */
static const char* const sRomCourseSymbols[GDX_ROMCOURSE_COUNT] = {
    "aCourseMuteCity1",  "aCourseSilence1",     "aCourseSandOcean1", "aCourseDevilsForest1",
    "aCourseBigBlue1",   "aCoursePortTown1",    "aCourseSectorAlpha", "aCourseRedCanyon1",
    "aCourseDevilsForest2", "aCourseMuteCity2", "aCourseBigBlue2",   "aCourseWhiteLand1",
    "aCourseFireField",  "aCourseSilence2",     "aCourseSectorBeta", "aCourseRedCanyon2",
    "aCourseWhiteLand2", "aCourseMuteCity3",    "aCourseRainbowRoad", "aCourseDevilsForest3",
    "aCourseSpacePlant", "aCourseSandOcean2",   "aCoursePortTown2",  "aCourseBigHand",
    "aCourseBattle",     "aCourseEnding"
};

struct GdxRomCourseSet {
    uint8_t* patched; /* whole image, normalised big-endian */
    int64_t patchedSize;
    uint8_t* clean; /* baseline, or NULL */
    int64_t cleanSize;
    int hasBaseline;
    int touchesNonCourse;
    int headerChecksumChanged;
    int changedCount;
    int changed[GDX_ROMCOURSE_COUNT];
};

/* ---------------------------------------------------------------------------------
 * Loading and byte-order normalisation
 * ------------------------------------------------------------------------------- */

static uint32_t gdx_romcourse_rd32be(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static uint16_t gdx_romcourse_rd16be(const uint8_t* p) {
    return (uint16_t)(((uint16_t)p[0] << 8) | p[1]);
}

/* .z64 is already big-endian; .v64 has each 16-bit word swapped; .n64 has each 32-bit word
 * reversed. Detect by the ROM header magic and rewrite in place so everything downstream can
 * assume big-endian. Returns 0 when the magic is none of the three. */
static int gdx_romcourse_normalise(uint8_t* data, int64_t size) {
    int64_t i;

    if (size < 4) {
        return 0;
    }
    if (data[0] == 0x80 && data[1] == 0x37 && data[2] == 0x12 && data[3] == 0x40) {
        return 1; /* .z64, big-endian already */
    }
    if (data[0] == 0x37 && data[1] == 0x80 && data[2] == 0x40 && data[3] == 0x12) {
        for (i = 0; i + 1 < size; i += 2) { /* .v64 */
            uint8_t t = data[i];
            data[i] = data[i + 1];
            data[i + 1] = t;
        }
        return 1;
    }
    if (data[0] == 0x40 && data[1] == 0x12 && data[2] == 0x37 && data[3] == 0x80) {
        for (i = 0; i + 3 < size; i += 4) { /* .n64 */
            uint8_t a = data[i];
            uint8_t b = data[i + 1];
            data[i] = data[i + 3];
            data[i + 1] = data[i + 2];
            data[i + 2] = b;
            data[i + 3] = a;
        }
        return 1;
    }
    return 0;
}

static int gdx_romcourse_load(const char* path, uint8_t** outData, int64_t* outSize) {
    FILE* f;
    long size;
    uint8_t* data;

    *outData = NULL;
    *outSize = 0;

    f = fopen(path, "rb");
    if (f == NULL) {
        return GDX_ROMCOURSE_ERR_IO;
    }
    if (fseek(f, 0, SEEK_END) != 0 || (size = ftell(f)) < 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return GDX_ROMCOURSE_ERR_IO;
    }
    if ((int64_t)size < GDX_ROMCOURSE_MIN_ROM_BYTES) {
        fclose(f);
        return GDX_ROMCOURSE_ERR_NOT_ROM;
    }
    data = (uint8_t*)malloc((size_t)size);
    if (data == NULL) {
        fclose(f);
        return GDX_ROMCOURSE_ERR_IO;
    }
    if (fread(data, 1, (size_t)size, f) != (size_t)size) {
        free(data);
        fclose(f);
        return GDX_ROMCOURSE_ERR_IO;
    }
    fclose(f);

    if (!gdx_romcourse_normalise(data, size)) {
        free(data);
        return GDX_ROMCOURSE_ERR_NOT_ROM;
    }
    *outData = data;
    *outSize = size;
    return GDX_ROMCOURSE_OK;
}

/* ---------------------------------------------------------------------------------
 * Course parsing
 * ------------------------------------------------------------------------------- */

static const uint8_t* gdx_romcourse_block(const GdxRomCourseSet* set, int index) {
    return set->patched + GDX_ROMCOURSE_TABLE_OFFSET + (int64_t)index * GDX_ROMCOURSE_STRIDE;
}

/* A slot is plausible when it looks like CourseData at all: Nintendo creator id (which is what
 * both retail and FZEP write) and a control-point count inside the struct's own array bounds.
 * Everything stricter -- checksum, geometry, BGM range, name rules -- is left to the shared
 * validation chain in gdx_content_import.c, so there is exactly one place that decides whether a
 * track may be installed. */
static int gdx_romcourse_slot_plausible(const uint8_t* block) {
    int8_t count = (int8_t)block[1];
    if (block[0] != GDX_ROMCOURSE_CREATOR_NINTENDO) {
        return 0;
    }
    if (count < 0 || count > GDX_ROMCOURSE_MAX_CONTROL_POINTS) {
        return 0;
    }
    return 1;
}

static void gdx_romcourse_fill_entry(const GdxRomCourseSet* set, int index, GdxRomCourseEntry* out) {
    const uint8_t* block = gdx_romcourse_block(set, index);
    int i;

    memset(out, 0, sizeof(*out));
    out->index = index;
    out->symbol = sRomCourseSymbols[index];
    out->controlPointCount = (int8_t)block[1];
    out->venue = (int8_t)block[2];
    out->skybox = (int8_t)block[3];
    out->checksum = gdx_romcourse_rd32be(block + GDX_ROMCOURSE_OFF_CHECKSUM);
    out->bgm = (int8_t)block[GDX_ROMCOURSE_OFF_BGM];
    out->changed = set->changed[index];

    /* fileName is fixed-width and is NOT zero-filled past its terminator in retail ROMs, so the
     * copy stops at the first NUL and then trims padding spaces. */
    for (i = 0; i < GDX_ROMCOURSE_NAME_LEN; i++) {
        char c = (char)block[GDX_ROMCOURSE_OFF_FILENAME + i];
        if (c == '\0') {
            break;
        }
        out->name[i] = c;
    }
    out->name[i] = '\0';
    while (i > 0 && out->name[i - 1] == ' ') {
        out->name[--i] = '\0';
    }
}

/* ---------------------------------------------------------------------------------
 * Public API
 * ------------------------------------------------------------------------------- */

int gdx_romcourse_open(const char* patchedPath, const char* cleanPath, GdxRomCourseSet** out) {
    GdxRomCourseSet* set;
    int rc;
    int i;
    int plausible = 0;

    if (patchedPath == NULL || out == NULL) {
        return GDX_ROMCOURSE_ERR_BAD_ARGS;
    }
    *out = NULL;

    set = (GdxRomCourseSet*)calloc(1, sizeof(GdxRomCourseSet));
    if (set == NULL) {
        return GDX_ROMCOURSE_ERR_IO;
    }

    rc = gdx_romcourse_load(patchedPath, &set->patched, &set->patchedSize);
    if (rc != GDX_ROMCOURSE_OK) {
        gdx_romcourse_close(set);
        return rc;
    }

    for (i = 0; i < GDX_ROMCOURSE_COUNT; i++) {
        if (gdx_romcourse_slot_plausible(gdx_romcourse_block(set, i))) {
            plausible++;
        }
    }
    /* The table is one contiguous run of 26 slots; if none of them parses, this is either not
     * F-Zero X or not the US rev0 layout the offsets describe. */
    if (plausible == 0) {
        gdx_romcourse_close(set);
        return GDX_ROMCOURSE_ERR_NO_COURSES;
    }

    if (cleanPath != NULL && cleanPath[0] != '\0') {
        rc = gdx_romcourse_load(cleanPath, &set->clean, &set->cleanSize);
        if (rc != GDX_ROMCOURSE_OK) {
            gdx_romcourse_close(set);
            return GDX_ROMCOURSE_ERR_NO_BASELINE;
        }
        set->hasBaseline = 1;

        for (i = 0; i < GDX_ROMCOURSE_COUNT; i++) {
            int64_t off = GDX_ROMCOURSE_TABLE_OFFSET + (int64_t)i * GDX_ROMCOURSE_STRIDE;
            if (off + GDX_ROMCOURSE_STRIDE > set->cleanSize ||
                memcmp(set->patched + off, set->clean + off, GDX_ROMCOURSE_STRIDE) != 0) {
                set->changed[i] = 1;
                set->changedCount++;
            }
        }

        /* Anything altered outside the table means the hack is not course-data-only. A size
         * change alone already proves it.
         *
         * One documented exception: the two ROM header CRC words. FZEP's "Recalculate Header
         * Checksum" (its Help/Patch.html) rewrites them so the patched ROM still boots on
         * hardware and on emulators that check, and a great many distributed course hacks carry
         * that recalculation. Those bytes say nothing about whether the hack changes content, so
         * counting them would raise the "this changes more than courses" warning on ordinary
         * course-only hacks. They are reported separately instead, and every other byte of the
         * header -- internal name, media/region codes, boot address -- is still compared. */
        if (set->patchedSize != set->cleanSize) {
            set->touchesNonCourse = 1;
        } else {
            int64_t tableStart = GDX_ROMCOURSE_TABLE_OFFSET;
            int64_t tableEnd = tableStart + GDX_ROMCOURSE_TABLE_BYTES;
            int64_t crcStart = GDX_ROMCOURSE_HEADER_CRC_OFFSET;
            int64_t crcEnd = crcStart + GDX_ROMCOURSE_HEADER_CRC_BYTES;

            set->headerChecksumChanged =
                (memcmp(set->patched + crcStart, set->clean + crcStart, GDX_ROMCOURSE_HEADER_CRC_BYTES) != 0);

            if (memcmp(set->patched, set->clean, (size_t)crcStart) != 0 ||
                memcmp(set->patched + crcEnd, set->clean + crcEnd, (size_t)(tableStart - crcEnd)) != 0 ||
                (set->patchedSize > tableEnd &&
                 memcmp(set->patched + tableEnd, set->clean + tableEnd,
                        (size_t)(set->patchedSize - tableEnd)) != 0)) {
                set->touchesNonCourse = 1;
            }
        }
    }

    *out = set;
    return GDX_ROMCOURSE_OK;
}

void gdx_romcourse_close(GdxRomCourseSet* set) {
    if (set == NULL) {
        return;
    }
    free(set->patched);
    free(set->clean);
    free(set);
}

const char* gdx_romcourse_strerror(int err) {
    switch (err) {
        case GDX_ROMCOURSE_OK:
            return "ok";
        case GDX_ROMCOURSE_ERR_BAD_ARGS:
            return "bad arguments";
        case GDX_ROMCOURSE_ERR_IO:
            return "could not read the ROM file";
        case GDX_ROMCOURSE_ERR_NOT_ROM:
            return "not an N64 ROM (expected a .z64, .v64 or .n64 image large enough to hold the "
                   "course table)";
        case GDX_ROMCOURSE_ERR_NO_COURSES:
            return "no course table found (this does not look like F-Zero X US rev0)";
        case GDX_ROMCOURSE_ERR_NO_BASELINE:
            return "could not read your clean ROM, so changed courses cannot be identified";
        default:
            return "unknown error";
    }
}

int gdx_romcourse_list(GdxRomCourseSet* set, GdxRomCourseEntry* out, int capacity, int changedOnly) {
    int i;
    int count = 0;

    if (set == NULL || capacity < 0 || (out == NULL && capacity != 0)) {
        return GDX_ROMCOURSE_ERR_BAD_ARGS;
    }
    for (i = 0; i < GDX_ROMCOURSE_COUNT; i++) {
        if (!gdx_romcourse_slot_plausible(gdx_romcourse_block(set, i))) {
            continue;
        }
        if (changedOnly && !set->changed[i]) {
            continue;
        }
        if (out != NULL && count < capacity) {
            gdx_romcourse_fill_entry(set, i, &out[count]);
        }
        count++;
    }
    return count;
}

int gdx_romcourse_payload(GdxRomCourseSet* set, int index, uint8_t* buf, int32_t bufSize) {
    const uint8_t* block;
    int i;

    if (set == NULL || buf == NULL || index < 0 || index >= GDX_ROMCOURSE_COUNT) {
        return GDX_ROMCOURSE_ERR_BAD_ARGS;
    }
    if (bufSize < GDX_ROMCOURSE_PAYLOAD_SIZE) {
        return GDX_ROMCOURSE_ERR_BAD_ARGS;
    }
    block = gdx_romcourse_block(set, index);
    if (!gdx_romcourse_slot_plausible(block)) {
        return GDX_ROMCOURSE_ERR_NO_COURSES;
    }

    /* CourseContext = CourseData then GhostSave[3] then SaveCourseRecords. A course lifted out of
     * a ROM carries no ghosts and no records, so the tail is zero -- which is exactly what the
     * editor writes for a track that has never been raced. */
    memset(buf, 0, (size_t)GDX_ROMCOURSE_PAYLOAD_SIZE);

    /* Single-byte fields and the fixed-width name copy straight across; only the multi-byte
     * fields need swapping. */
    memcpy(buf, block, GDX_ROMCOURSE_OFF_CONTROLPOINTS);
    {
        uint32_t checksum = gdx_romcourse_rd32be(block + GDX_ROMCOURSE_OFF_CHECKSUM);
        GdxRomCourseData* dst = (GdxRomCourseData*)buf;
        dst->checksum = checksum;
    }

    for (i = 0; i < GDX_ROMCOURSE_MAX_CONTROL_POINTS; i++) {
        const uint8_t* src = block + GDX_ROMCOURSE_OFF_CONTROLPOINTS + (size_t)i * GDX_ROMCOURSE_CONTROLPOINT_SIZE;
        GdxRomCourseData* dst = (GdxRomCourseData*)buf;
        uint32_t bits;

        bits = gdx_romcourse_rd32be(src + 0);
        memcpy(&dst->controlPoint[i].pos.x, &bits, sizeof(bits));
        bits = gdx_romcourse_rd32be(src + 4);
        memcpy(&dst->controlPoint[i].pos.y, &bits, sizeof(bits));
        bits = gdx_romcourse_rd32be(src + 8);
        memcpy(&dst->controlPoint[i].pos.z, &bits, sizeof(bits));
        dst->controlPoint[i].radiusLeft = (int16_t)gdx_romcourse_rd16be(src + 12);
        dst->controlPoint[i].radiusRight = (int16_t)gdx_romcourse_rd16be(src + 14);
        dst->controlPoint[i].trackSegmentInfo = (int32_t)gdx_romcourse_rd32be(src + 16);
    }

    for (i = 0; i < GDX_ROMCOURSE_MAX_CONTROL_POINTS; i++) {
        GdxRomCourseData* dst = (GdxRomCourseData*)buf;
        dst->bankAngle[i] = (int16_t)gdx_romcourse_rd16be(block + GDX_ROMCOURSE_OFF_BANKANGLE + (size_t)i * 2);
    }

    /* The nine per-segment attribute arrays are byte-sized, so they are layout-identical. */
    memcpy(buf + 0x5A0, block + 0x5A0, GDX_ROMCOURSE_STRIDE - 0x5A0);

    return GDX_ROMCOURSE_OK;
}

int gdx_romcourse_touches_non_course(const GdxRomCourseSet* set) {
    return (set != NULL) ? set->touchesNonCourse : 0;
}

int gdx_romcourse_changed_count(const GdxRomCourseSet* set) {
    return (set != NULL) ? set->changedCount : 0;
}

int gdx_romcourse_has_baseline(const GdxRomCourseSet* set) {
    return (set != NULL) ? set->hasBaseline : 0;
}

int gdx_romcourse_header_checksum_changed(const GdxRomCourseSet* set) {
    return (set != NULL) ? set->headerChecksumChanged : 0;
}
