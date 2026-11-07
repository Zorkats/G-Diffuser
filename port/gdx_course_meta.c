/* port/gdx_course_meta.c -- see gdx_course_meta.h. */

#include "gdx_course_meta.h"

#include "gdx_course_registry.h"

#include <string.h>

/* Stock course count the CPU target calibration looks at, at most. Both stock tables are shorter. */
#define META_CALIBRATION_MAX 64

typedef struct MetaSlot {
    int used;
    char courseId[GDX_BRANCH_COURSE_ID_MAX + 1];
    GdxCourseMeta meta;
} MetaSlot;

static MetaSlot sStore[GDX_BRANCH_STORE_SLOTS];
static MetaSlot sStage[GDX_BRANCH_STORE_SLOTS];
static unsigned long sRevision;

static void put_u16le(unsigned char* p, uint32_t v) {
    p[0] = (unsigned char) (v & 0xFFu);
    p[1] = (unsigned char) ((v >> 8) & 0xFFu);
}

static void put_u32le(unsigned char* p, uint32_t v) {
    p[0] = (unsigned char) (v & 0xFFu);
    p[1] = (unsigned char) ((v >> 8) & 0xFFu);
    p[2] = (unsigned char) ((v >> 16) & 0xFFu);
    p[3] = (unsigned char) ((v >> 24) & 0xFFu);
}

static uint32_t get_u16le(const unsigned char* p) {
    return (uint32_t) p[0] | ((uint32_t) p[1] << 8);
}

static uint32_t get_u32le(const unsigned char* p) {
    return (uint32_t) p[0] | ((uint32_t) p[1] << 8) | ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24);
}

static int bytes_fit(size_t pos, size_t end, size_t need) {
    return (pos <= end) && (need <= end - pos);
}

static int name_bytes_ok(const unsigned char* bytes, size_t length) {
    size_t i;

    if ((length < 1) || (length > GDX_META_NAME_MAX) || (bytes[0] == ' ') || (bytes[length - 1] == ' ')) {
        return 0;
    }
    for (i = 0; i < length; i++) {
        if ((bytes[i] < 0x20) || (bytes[i] > 0x7E)) {
            return 0;
        }
    }
    return 1;
}

int gdx_meta_name_ok(const char* name) {
    size_t length = 0;

    if (name == NULL) {
        return 0;
    }
    while ((length <= GDX_META_NAME_MAX) && (name[length] != '\0')) {
        length++;
    }
    return (length <= GDX_META_NAME_MAX) && name_bytes_ok((const unsigned char*) name, length);
}

int gdx_meta_is_empty(const GdxCourseMeta* meta) {
    return (meta == NULL) ||
           ((meta->author[0] == '\0') && (meta->editedBy[0] == '\0') && (meta->staffTime == 0) &&
            (meta->staffLaps == 0) && (meta->madeHere == 0) && (meta->hasSignature == 0) &&
            (meta->splitsLength == 0) && (meta->ghostLength == 0));
}

/* 1 when a decoded split record belongs to the staff time: same lap count, last lap end equal to it. */
static int splits_fit_staff(const GdxCourseMeta* meta, const GdxCourseSplits* splits) {
    return (meta->staffTime != 0) && (splits->laps >= 1) && (splits->laps == (int) meta->staffLaps) &&
           (splits->lapEnd[splits->laps - 1] == meta->staffTime);
}

/* 1 when the stored split record decodes and belongs to the staff time. */
static int splits_match(const GdxCourseMeta* meta) {
    GdxCourseSplits splits;

    return (meta->splitsLength <= GDX_META_SPLITS_MAX_BYTES) &&
           (gdx_splits_decode(meta->splits, meta->splitsLength, &splits) == 0) && splits_fit_staff(meta, &splits);
}

static void clear_splits(GdxCourseMeta* meta) {
    meta->splitsLength = 0;
    memset(meta->splits, 0, sizeof(meta->splits));
}

/* 1 when the stored ghost reads and belongs to the staff time: a Time Attack run with the same race time. */
static int ghost_match(const GdxCourseMeta* meta) {
    uint32_t raceTime = 0;

    return (meta->ghostLength <= GDX_META_GHOST_MAX_BYTES) && (meta->staffTime != 0) &&
           (meta->staffLaps == GDX_META_RECORD_LAPS) &&
           (gdx_staff_ghost_check(meta->ghost, meta->ghostLength, &raceTime) == GDX_STAFF_GHOST_OK) &&
           (raceTime == meta->staffTime);
}

static void clear_ghost(GdxCourseMeta* meta) {
    meta->ghostLength = 0;
    memset(meta->ghost, 0, sizeof(meta->ghost));
}

int gdx_meta_validate(const GdxCourseMeta* meta) {
    if (meta == NULL) {
        return GDX_META_ERR_ARGS;
    }
    if ((meta->author[0] != '\0') && !gdx_meta_name_ok(meta->author)) {
        return GDX_META_ERR_VALUE;
    }
    if (meta->editedBy[0] != '\0') {
        if (!gdx_meta_name_ok(meta->editedBy) || (meta->author[0] == '\0') ||
            (strcmp(meta->editedBy, meta->author) == 0)) {
            return GDX_META_ERR_VALUE;
        }
    }
    if ((meta->staffTime == 0) != (meta->staffLaps == 0)) {
        return GDX_META_ERR_VALUE;
    }
    if ((meta->staffTime > GDX_META_STAFF_TIME_MAX) || (meta->staffLaps > GDX_META_LAPS_MAX) ||
        (meta->madeHere > 1)) {
        return GDX_META_ERR_VALUE;
    }
    /* A signature vouches for an author, so it cannot stand alone. */
    if ((meta->hasSignature > 1) || (meta->hasSignature && (meta->author[0] == '\0'))) {
        return GDX_META_ERR_VALUE;
    }
    if ((meta->splitsLength != 0) && !splits_match(meta)) {
        return GDX_META_ERR_VALUE;
    }
    if ((meta->ghostLength != 0) && !ghost_match(meta)) {
        return GDX_META_ERR_VALUE;
    }
    return GDX_META_OK;
}

/* ---------------------------------------------------------------------------------
 * GXM1 block.
 * ------------------------------------------------------------------------------- */

static size_t put_record(unsigned char* out, int tag, const void* value, size_t length) {
    out[0] = (unsigned char) tag;
    put_u16le(out + 1, (uint32_t) length);
    if (length > 0) {
        memcpy(out + GDX_META_RECORD_HEADER_SIZE, value, length);
    }
    return GDX_META_RECORD_HEADER_SIZE + length;
}

int gdx_meta_encode(const GdxCourseMeta* meta, uint32_t bindCrc, unsigned char* out, size_t cap) {
    unsigned char staff[GDX_META_STAFF_BYTES];
    size_t pos = GDX_META_BLOCK_HEADER_SIZE;
    int count = 0;
    int rc;

    if ((meta == NULL) || (out == NULL)) {
        return GDX_META_ERR_ARGS;
    }
    rc = gdx_meta_validate(meta);
    if (rc != GDX_META_OK) {
        return rc;
    }
    if (cap < GDX_META_BLOCK_MAX_BYTES) {
        return GDX_META_ERR_FULL;
    }
    if (meta->author[0] != '\0') {
        pos += put_record(out + pos, GDX_META_TAG_AUTHOR, meta->author, strlen(meta->author));
        count++;
    }
    if (meta->editedBy[0] != '\0') {
        pos += put_record(out + pos, GDX_META_TAG_EDITED_BY, meta->editedBy, strlen(meta->editedBy));
        count++;
    }
    if (meta->staffTime != 0) {
        put_u32le(staff, meta->staffTime);
        staff[4] = meta->staffLaps;
        pos += put_record(out + pos, GDX_META_TAG_STAFF, staff, sizeof(staff));
        count++;
    }
    if (meta->madeHere) {
        pos += put_record(out + pos, GDX_META_TAG_MADE_HERE, NULL, 0);
        count++;
    }
    if (meta->splitsLength != 0) {
        pos += put_record(out + pos, GDX_META_TAG_SPLITS, meta->splits, meta->splitsLength);
        count++;
    }
    if (meta->hasSignature) {
        pos += put_record(out + pos, GDX_META_TAG_SIGNATURE, meta->signature, GDX_META_SIGNATURE_BYTES);
        count++;
    }
    if (meta->ghostLength != 0) {
        pos += put_record(out + pos, GDX_META_TAG_GHOST, meta->ghost, meta->ghostLength);
        count++;
    }
    out[0] = 'G';
    out[1] = 'X';
    out[2] = 'M';
    out[3] = '1';
    put_u16le(out + 4, 1);
    put_u16le(out + 6, (uint32_t) count);
    put_u32le(out + 8, bindCrc);
    put_u32le(out + pos, gdx_course_registry_crc32(out, pos));
    return (int) (pos + 4);
}

static int decode_record(int tag, const unsigned char* value, size_t length, GdxCourseMeta* out) {
    switch (tag) {
        case GDX_META_TAG_AUTHOR:
        case GDX_META_TAG_EDITED_BY: {
            char* name = (tag == GDX_META_TAG_AUTHOR) ? out->author : out->editedBy;

            if (!name_bytes_ok(value, length)) {
                return GDX_META_ERR_VALUE;
            }
            memcpy(name, value, length);
            name[length] = '\0';
            return GDX_META_OK;
        }
        case GDX_META_TAG_STAFF:
            if (length != GDX_META_STAFF_BYTES) {
                return GDX_META_ERR_LAYOUT;
            }
            out->staffTime = get_u32le(value);
            out->staffLaps = value[4];
            if ((out->staffTime == 0) || (out->staffLaps == 0)) {
                return GDX_META_ERR_VALUE;
            }
            return GDX_META_OK;
        case GDX_META_TAG_MADE_HERE:
            if (length != 0) {
                return GDX_META_ERR_LAYOUT;
            }
            out->madeHere = 1;
            return GDX_META_OK;
        case GDX_META_TAG_SPLITS:
            /* Checked against the staff time once the whole block is read. Too long for this build: skipped. */
            if (length <= GDX_META_SPLITS_MAX_BYTES) {
                memcpy(out->splits, value, length);
                out->splitsLength = (uint16_t) length;
            }
            return GDX_META_OK;
        case GDX_META_TAG_SIGNATURE:
            if (length != GDX_META_SIGNATURE_BYTES) {
                return GDX_META_ERR_LAYOUT;
            }
            memcpy(out->signature, value, GDX_META_SIGNATURE_BYTES);
            out->hasSignature = 1;
            return GDX_META_OK;
        case GDX_META_TAG_GHOST:
            /* Checked against the staff time once the whole block is read. Too long for this build: skipped. */
            if ((length >= 1) && (length <= GDX_META_GHOST_MAX_BYTES)) {
                memcpy(out->ghost, value, length);
                out->ghostLength = (uint16_t) length;
            }
            return GDX_META_OK;
        default:
            /* A later build's record. Skipped, not kept. */
            return GDX_META_OK;
    }
}

static int decode_block(const unsigned char* data, size_t length, uint32_t bindCrc, GdxCourseMeta* out) {
    size_t end;
    size_t pos;
    uint32_t count;
    uint32_t i;
    int lastTag = 0;

    if (data == NULL) {
        return GDX_META_ERR_ARGS;
    }
    if (length < GDX_META_BLOCK_HEADER_SIZE + 4) {
        return GDX_META_ERR_TRUNCATED;
    }
    if (length > GDX_META_BLOCK_READ_MAX_BYTES) {
        return GDX_META_ERR_LAYOUT;
    }
    if ((data[0] != 'G') || (data[1] != 'X') || (data[2] != 'M') || (data[3] != '1')) {
        return GDX_META_ERR_MAGIC;
    }
    if (get_u16le(data + 4) != 1) {
        return GDX_META_ERR_VERSION;
    }
    end = length - 4;
    if (get_u32le(data + end) != gdx_course_registry_crc32(data, end)) {
        return GDX_META_ERR_CRC;
    }
    if (get_u32le(data + 8) != bindCrc) {
        return GDX_META_ERR_BINDING;
    }
    count = get_u16le(data + 6);
    pos = GDX_META_BLOCK_HEADER_SIZE;
    for (i = 0; i < count; i++) {
        int tag;
        size_t valueLen;
        int rc;

        if (!bytes_fit(pos, end, GDX_META_RECORD_HEADER_SIZE)) {
            return GDX_META_ERR_LAYOUT;
        }
        tag = data[pos];
        valueLen = get_u16le(data + pos + 1);
        pos += GDX_META_RECORD_HEADER_SIZE;
        if ((tag <= lastTag) || !bytes_fit(pos, end, valueLen)) {
            return GDX_META_ERR_LAYOUT;
        }
        lastTag = tag;
        rc = decode_record(tag, data + pos, valueLen, out);
        if (rc != GDX_META_OK) {
            return rc;
        }
        pos += valueLen;
    }
    if (pos != end) {
        return GDX_META_ERR_LAYOUT;
    }
    /* Splits only tune CPU pacing, so a record that does not fit the staff time costs the splits, not the block. */
    if ((out->splitsLength != 0) && !splits_match(out)) {
        clear_splits(out);
    }
    /* The same for the ghost: it only adds a Time Attack opponent. */
    if ((out->ghostLength != 0) && !ghost_match(out)) {
        clear_ghost(out);
    }
    return gdx_meta_validate(out);
}

int gdx_meta_decode(const unsigned char* data, size_t length, uint32_t bindCrc, GdxCourseMeta* out) {
    int rc;

    if (out == NULL) {
        return GDX_META_ERR_ARGS;
    }
    memset(out, 0, sizeof(*out));
    rc = decode_block(data, length, bindCrc, out);
    if (rc != GDX_META_OK) {
        memset(out, 0, sizeof(*out));
    }
    return rc;
}

/* ---------------------------------------------------------------------------------
 * Editor save and CPU target.
 * ------------------------------------------------------------------------------- */

void gdx_meta_clear_staff(GdxCourseMeta* meta) {
    if (meta == NULL) {
        return;
    }
    meta->staffTime = 0;
    meta->staffLaps = 0;
    clear_splits(meta);
    clear_ghost(meta);
}

void gdx_meta_apply_editor_save(GdxCourseMeta* meta, const char* localName, int keepStaff) {
    if (meta == NULL) {
        return;
    }
    if (!keepStaff) {
        gdx_meta_clear_staff(meta);
    }
    /* The replay drives positions on the layout it was recorded on, which this save replaces. */
    clear_ghost(meta);
    meta->madeHere = 1;
    /* The signed course bytes are about to change. */
    meta->hasSignature = 0;
    memset(meta->signature, 0, sizeof(meta->signature));
    if (!gdx_meta_name_ok(localName)) {
        return;
    }
    if (meta->author[0] == '\0') {
        memcpy(meta->author, localName, strlen(localName) + 1);
        meta->editedBy[0] = '\0';
    } else if (strcmp(meta->author, localName) != 0) {
        memcpy(meta->editedBy, localName, strlen(localName) + 1);
    }
}

static int race_time_ok(int raceLaps, uint32_t raceTime) {
    return (raceLaps >= 1) && (raceLaps <= GDX_META_LAPS_MAX) && (raceTime >= 1) &&
           (raceTime <= GDX_META_STAFF_TIME_MAX);
}

int gdx_meta_staff_offer(const GdxCourseMeta* meta, int raceLaps, uint32_t raceTime) {
    if ((meta == NULL) || !meta->madeHere || (raceLaps != GDX_META_RECORD_LAPS) ||
        !race_time_ok(raceLaps, raceTime)) {
        return 0;
    }
    return (meta->staffTime == 0) || (meta->staffLaps != GDX_META_RECORD_LAPS) || (raceTime < meta->staffTime);
}

int gdx_meta_set_staff(GdxCourseMeta* meta, int raceLaps, uint32_t raceTime) {
    if (meta == NULL) {
        return GDX_META_ERR_ARGS;
    }
    if (!race_time_ok(raceLaps, raceTime)) {
        return GDX_META_ERR_VALUE;
    }
    meta->staffTime = raceTime;
    meta->staffLaps = (uint8_t) raceLaps;
    clear_splits(meta);
    clear_ghost(meta);
    return GDX_META_OK;
}

int gdx_meta_set_splits(GdxCourseMeta* meta, const GdxCourseSplits* splits) {
    unsigned char bytes[GDX_META_SPLITS_MAX_BYTES];
    int length;

    if (meta == NULL) {
        return GDX_META_ERR_ARGS;
    }
    if (splits == NULL) {
        clear_splits(meta);
        return GDX_META_OK;
    }
    length = gdx_splits_encode(splits, bytes, sizeof(bytes));
    if ((length <= 0) || !splits_fit_staff(meta, splits)) {
        return GDX_META_ERR_VALUE;
    }
    clear_splits(meta);
    memcpy(meta->splits, bytes, (size_t) length);
    meta->splitsLength = (uint16_t) length;
    return GDX_META_OK;
}

int gdx_meta_get_splits(const GdxCourseMeta* meta, GdxCourseSplits* out) {
    if (out == NULL) {
        return 0;
    }
    memset(out, 0, sizeof(*out));
    if ((meta == NULL) || (meta->splitsLength == 0) || !splits_match(meta)) {
        return 0;
    }
    return gdx_splits_decode(meta->splits, meta->splitsLength, out) == 0;
}

int gdx_meta_set_ghost(GdxCourseMeta* meta, const GdxStaffGhost* ghost) {
    /* Static: the encoded ghost is up to 16 KB. */
    static unsigned char sBytes[GDX_META_GHOST_MAX_BYTES];
    int length;

    if (meta == NULL) {
        return GDX_META_ERR_ARGS;
    }
    if (ghost == NULL) {
        clear_ghost(meta);
        return GDX_META_OK;
    }
    length = gdx_staff_ghost_encode(ghost, sBytes, sizeof(sBytes));
    if ((length <= 0) || (meta->staffTime == 0) || (meta->staffLaps != GDX_META_RECORD_LAPS) ||
        (ghost->raceTime != meta->staffTime)) {
        return GDX_META_ERR_VALUE;
    }
    clear_ghost(meta);
    memcpy(meta->ghost, sBytes, (size_t) length);
    meta->ghostLength = (uint16_t) length;
    return GDX_META_OK;
}

int gdx_meta_has_ghost(const GdxCourseMeta* meta) {
    return (meta != NULL) && (meta->ghostLength != 0) && ghost_match(meta);
}

int gdx_meta_get_ghost(const GdxCourseMeta* meta, GdxStaffGhost* out) {
    if (out == NULL) {
        return 0;
    }
    if (!gdx_meta_has_ghost(meta)) {
        memset(out, 0, sizeof(*out));
        return 0;
    }
    return gdx_staff_ghost_decode(meta->ghost, meta->ghostLength, out) == GDX_STAFF_GHOST_OK;
}

int gdx_meta_format_time(uint32_t raceTime, char* out, size_t cap) {
    const uint32_t minutes = raceTime / 60000u;
    const uint32_t seconds = (raceTime / 1000u) % 60u;
    const uint32_t millis = raceTime % 1000u;
    /* M'SS"mmm, minutes below 60 so one or two digits. */
    const size_t length = (minutes >= 10) ? 9 : 8;
    size_t pos = 0;

    if ((out == NULL) || (raceTime > GDX_META_STAFF_TIME_MAX) || (cap < length + 1)) {
        return -1;
    }
    if (minutes >= 10) {
        out[pos++] = (char) ('0' + minutes / 10);
    }
    out[pos++] = (char) ('0' + minutes % 10);
    out[pos++] = '\'';
    out[pos++] = (char) ('0' + seconds / 10);
    out[pos++] = (char) ('0' + seconds % 10);
    out[pos++] = '"';
    out[pos++] = (char) ('0' + millis / 100);
    out[pos++] = (char) ('0' + (millis / 10) % 10);
    out[pos++] = (char) ('0' + millis % 10);
    out[pos] = '\0';
    return (int) pos;
}

int32_t gdx_meta_cpu_target(const GdxCourseMeta* meta, int raceLaps, int difficulty, int32_t playerRecord,
                            const uint32_t* stockTargets, const int32_t* stockStaff, int courseCount) {
    double ratios[META_CALIBRATION_MAX];
    double ratio;
    double target;
    int count = 0;
    int c;
    int i;

    if ((meta == NULL) || (meta->staffTime == 0) || (meta->staffLaps == 0) || (raceLaps < 1) ||
        (raceLaps > GDX_META_LAPS_MAX) || (difficulty < 0) || (difficulty > 3) || (stockTargets == NULL) ||
        (stockStaff == NULL) || (courseCount <= 0)) {
        return -1;
    }
    if (courseCount > META_CALIBRATION_MAX) {
        courseCount = META_CALIBRATION_MAX;
    }
    /* Sorted as they come in, for the median. */
    for (c = 0; c < courseCount; c++) {
        const uint32_t stockTarget = stockTargets[c * 4 + difficulty];

        if ((stockStaff[c] <= 0) || (stockTarget == 0)) {
            continue;
        }
        ratio = (double) stockTarget / (double) stockStaff[c];
        for (i = count; (i > 0) && (ratios[i - 1] > ratio); i--) {
            ratios[i] = ratios[i - 1];
        }
        ratios[i] = ratio;
        count++;
    }
    if (count == 0) {
        return -1;
    }
    /* The median, so one odd stock entry cannot move every custom track. */
    ratio = (count & 1) ? ratios[count / 2] : (ratios[count / 2 - 1] + ratios[count / 2]) / 2.0;
    target = (double) meta->staffTime * (double) raceLaps / (double) meta->staffLaps * ratio;
    if ((target < 1.0) || (target > (double) GDX_META_STAFF_TIME_MAX)) {
        return -1;
    }
    /* 2 is EXPERT (decomp/include/fzx_game.h). */
    if ((difficulty >= 2) && (playerRecord > 0) && (raceLaps == GDX_META_RECORD_LAPS) &&
        (meta->staffLaps == GDX_META_RECORD_LAPS) &&
        ((uint32_t) playerRecord < meta->staffTime)) {
        const double refined = (double) playerRecord + (target - (double) meta->staffTime);

        if (refined >= 1.0) {
            target = refined;
        }
    }
    return (int32_t) (target + 0.5);
}

/* ---------------------------------------------------------------------------------
 * Store and GMS1 file.
 * ------------------------------------------------------------------------------- */

static int id_ok(const char* courseId) {
    size_t i = 0;

    if (courseId == NULL) {
        return 0;
    }
    while ((i <= GDX_BRANCH_COURSE_ID_MAX) && (courseId[i] != '\0')) {
        if (((unsigned char) courseId[i] < 0x20) || ((unsigned char) courseId[i] > 0x7E)) {
            return 0;
        }
        i++;
    }
    return (i >= 1) && (i <= GDX_BRANCH_COURSE_ID_MAX);
}

static int store_find(const char* courseId) {
    int i;

    for (i = 0; i < GDX_BRANCH_STORE_SLOTS; i++) {
        if (sStore[i].used && (strcmp(sStore[i].courseId, courseId) == 0)) {
            return i;
        }
    }
    return -1;
}

int gdx_meta_store_set(const char* courseId, const GdxCourseMeta* meta) {
    int index;
    int rc;
    int i;

    if (!id_ok(courseId) || (meta == NULL)) {
        return GDX_META_ERR_ARGS;
    }
    index = store_find(courseId);
    if (gdx_meta_is_empty(meta)) {
        if (index >= 0) {
            memset(&sStore[index], 0, sizeof(sStore[index]));
            sRevision++;
        }
        return GDX_META_OK;
    }
    rc = gdx_meta_validate(meta);
    if (rc != GDX_META_OK) {
        return rc;
    }
    if (index < 0) {
        for (i = 0; i < GDX_BRANCH_STORE_SLOTS; i++) {
            if (!sStore[i].used) {
                index = i;
                break;
            }
        }
        if (index < 0) {
            return GDX_META_ERR_FULL;
        }
        sStore[index].used = 1;
        memcpy(sStore[index].courseId, courseId, strlen(courseId) + 1);
    }
    sStore[index].meta = *meta;
    sRevision++;
    return GDX_META_OK;
}

int gdx_meta_store_get(const char* courseId, GdxCourseMeta* out) {
    int index;

    if (!id_ok(courseId) || (out == NULL)) {
        return 0;
    }
    index = store_find(courseId);
    if (index < 0) {
        return 0;
    }
    *out = sStore[index].meta;
    return 1;
}

int gdx_meta_store_rename(const char* oldId, const char* newId) {
    int oldIndex;
    int newIndex;

    if (!id_ok(oldId) || !id_ok(newId)) {
        return GDX_META_ERR_ARGS;
    }
    if (strcmp(oldId, newId) == 0) {
        return GDX_META_OK;
    }
    oldIndex = store_find(oldId);
    newIndex = store_find(newId);
    if ((oldIndex < 0) && (newIndex < 0)) {
        return GDX_META_OK;
    }
    if (newIndex >= 0) {
        memset(&sStore[newIndex], 0, sizeof(sStore[newIndex]));
    }
    if (oldIndex >= 0) {
        memcpy(sStore[oldIndex].courseId, newId, strlen(newId) + 1);
    }
    sRevision++;
    return GDX_META_OK;
}

void gdx_meta_store_clear(void) {
    memset(sStore, 0, sizeof(sStore));
    sRevision++;
}

unsigned long gdx_meta_store_revision(void) {
    return sRevision;
}

int gdx_meta_store_encode_file(unsigned char* out, size_t cap) {
    /* Static: with a staff ghost a block is about 19 KB. */
    static unsigned char block[GDX_META_BLOCK_MAX_BYTES];
    size_t pos = GDX_META_FILE_HEADER_SIZE;
    int count = 0;
    int i;

    if (out == NULL) {
        return GDX_META_ERR_ARGS;
    }
    for (i = 0; i < GDX_BRANCH_STORE_SLOTS; i++) {
        size_t idLen;
        int written;

        if (!sStore[i].used) {
            continue;
        }
        idLen = strlen(sStore[i].courseId);
        written = gdx_meta_encode(&sStore[i].meta,
                                  gdx_course_registry_crc32((const unsigned char*) sStore[i].courseId, idLen), block,
                                  sizeof(block));
        if (written < 0) {
            return written;
        }
        if (!bytes_fit(pos, cap, 1 + idLen + 2 + (size_t) written + 4)) {
            return GDX_META_ERR_FULL;
        }
        out[pos] = (unsigned char) idLen;
        memcpy(out + pos + 1, sStore[i].courseId, idLen);
        pos += 1 + idLen;
        put_u16le(out + pos, (uint32_t) written);
        pos += 2;
        memcpy(out + pos, block, (size_t) written);
        pos += (size_t) written;
        count++;
    }
    if (!bytes_fit(pos, cap, 4)) {
        return GDX_META_ERR_FULL;
    }
    out[0] = 'G';
    out[1] = 'M';
    out[2] = 'S';
    out[3] = '1';
    put_u16le(out + 4, 1);
    put_u16le(out + 6, (uint32_t) count);
    put_u32le(out + 8, (uint32_t) (pos + 4));
    put_u32le(out + pos, gdx_course_registry_crc32(out, pos));
    return (int) (pos + 4);
}

int gdx_meta_store_restore_file(const unsigned char* data, size_t length) {
    size_t end;
    size_t pos;
    uint32_t count;
    uint32_t i;

    if (data == NULL) {
        return GDX_META_ERR_ARGS;
    }
    if (length < GDX_META_FILE_HEADER_SIZE + 4) {
        return GDX_META_ERR_TRUNCATED;
    }
    if ((data[0] != 'G') || (data[1] != 'M') || (data[2] != 'S') || (data[3] != '1')) {
        return GDX_META_ERR_MAGIC;
    }
    if (get_u16le(data + 4) != 1) {
        return GDX_META_ERR_VERSION;
    }
    count = get_u16le(data + 6);
    if ((count > GDX_BRANCH_STORE_SLOTS) || (get_u32le(data + 8) != (uint32_t) length)) {
        return GDX_META_ERR_LAYOUT;
    }
    end = length - 4;
    if (get_u32le(data + end) != gdx_course_registry_crc32(data, end)) {
        return GDX_META_ERR_CRC;
    }
    memset(sStage, 0, sizeof(sStage));
    pos = GDX_META_FILE_HEADER_SIZE;
    for (i = 0; i < count; i++) {
        size_t idLen;
        size_t blockLen;
        uint32_t j;
        int rc;

        if (!bytes_fit(pos, end, 1)) {
            return GDX_META_ERR_LAYOUT;
        }
        idLen = data[pos];
        pos++;
        if ((idLen < 1) || (idLen > GDX_BRANCH_COURSE_ID_MAX) || !bytes_fit(pos, end, idLen + 2)) {
            return GDX_META_ERR_LAYOUT;
        }
        memcpy(sStage[i].courseId, data + pos, idLen);
        sStage[i].courseId[idLen] = '\0';
        if (!id_ok(sStage[i].courseId)) {
            return GDX_META_ERR_LAYOUT;
        }
        for (j = 0; j < i; j++) {
            if (strcmp(sStage[j].courseId, sStage[i].courseId) == 0) {
                return GDX_META_ERR_LAYOUT;
            }
        }
        pos += idLen;
        blockLen = get_u16le(data + pos);
        pos += 2;
        if (!bytes_fit(pos, end, blockLen)) {
            return GDX_META_ERR_LAYOUT;
        }
        rc = gdx_meta_decode(data + pos, blockLen,
                             gdx_course_registry_crc32((const unsigned char*) sStage[i].courseId, idLen),
                             &sStage[i].meta);
        if (rc != GDX_META_OK) {
            return rc;
        }
        /* store_set treats an empty value as "delete", so a file entry that is empty would break that. */
        if (gdx_meta_is_empty(&sStage[i].meta)) {
            return GDX_META_ERR_LAYOUT;
        }
        sStage[i].used = 1;
        pos += blockLen;
    }
    if (pos != end) {
        return GDX_META_ERR_LAYOUT;
    }
    memcpy(sStore, sStage, sizeof(sStore));
    sRevision++;
    return GDX_META_OK;
}

/* ---------------------------------------------------------------------------------
 * .gdxm sidecar.
 * ------------------------------------------------------------------------------- */

int gdx_meta_sidecar_encode(const GdxMetaSidecarEntry* entries, int count, unsigned char* out, size_t cap) {
    /* Static, like shared: with a staff ghost each is about 19 KB. */
    static unsigned char block[GDX_META_BLOCK_MAX_BYTES];
    static GdxCourseMeta shared;
    size_t pos = GDX_META_SIDECAR_HEADER_SIZE;
    int written = 0;
    int i;

    if ((entries == NULL) || (out == NULL) || (count < 0) || (count > GDX_META_SIDECAR_MAX_ENTRIES)) {
        return GDX_META_ERR_ARGS;
    }
    for (i = 0; i < count; i++) {
        const GdxMetaSidecarEntry* entry = &entries[i];
        size_t idLen;
        int blockLen;

        if (entry->meta == NULL) {
            continue;
        }
        /* The made-here mark belongs to this install only. */
        shared = *entry->meta;
        shared.madeHere = 0;
        if (gdx_meta_is_empty(&shared)) {
            continue;
        }
        if (!id_ok(entry->courseId)) {
            return GDX_META_ERR_ARGS;
        }
        idLen = strlen(entry->courseId);
        blockLen = gdx_meta_encode(&shared, entry->bindCrc, block, sizeof(block));
        if (blockLen < 0) {
            return blockLen;
        }
        if (!bytes_fit(pos, cap, 1 + idLen + 4 + 2 + (size_t) blockLen + 4)) {
            return GDX_META_ERR_FULL;
        }
        out[pos] = (unsigned char) idLen;
        memcpy(out + pos + 1, entry->courseId, idLen);
        pos += 1 + idLen;
        put_u32le(out + pos, entry->bindCrc);
        pos += 4;
        put_u16le(out + pos, (uint32_t) blockLen);
        pos += 2;
        memcpy(out + pos, block, (size_t) blockLen);
        pos += (size_t) blockLen;
        written++;
    }
    if (written == 0) {
        return 0;
    }
    if (!bytes_fit(pos, cap, 4)) {
        return GDX_META_ERR_FULL;
    }
    out[0] = 'G';
    out[1] = 'M';
    out[2] = 'X';
    out[3] = '1';
    put_u16le(out + 4, 1);
    put_u16le(out + 6, (uint32_t) written);
    put_u32le(out + 8, (uint32_t) (pos + 4));
    put_u32le(out + pos, gdx_course_registry_crc32(out, pos));
    return (int) (pos + 4);
}

/* One pass of the file. visit NULL only checks it. */
static int sidecar_walk(const unsigned char* data, size_t length, GdxMetaSidecarVisit visit, void* ctx) {
    static GdxCourseMeta sMeta;
    const size_t end = length - 4;
    const uint32_t count = get_u16le(data + 6);
    size_t pos = GDX_META_SIDECAR_HEADER_SIZE;
    uint32_t i;

    for (i = 0; i < count; i++) {
        char courseId[GDX_BRANCH_COURSE_ID_MAX + 1];
        size_t idLen;
        size_t blockLen;
        uint32_t bind;
        int rc;

        if (!bytes_fit(pos, end, 1)) {
            return GDX_META_ERR_LAYOUT;
        }
        idLen = data[pos];
        pos++;
        if ((idLen < 1) || (idLen > GDX_BRANCH_COURSE_ID_MAX) || !bytes_fit(pos, end, idLen + 4 + 2)) {
            return GDX_META_ERR_LAYOUT;
        }
        memcpy(courseId, data + pos, idLen);
        courseId[idLen] = '\0';
        if (!id_ok(courseId)) {
            return GDX_META_ERR_LAYOUT;
        }
        pos += idLen;
        bind = get_u32le(data + pos);
        pos += 4;
        blockLen = get_u16le(data + pos);
        pos += 2;
        if ((blockLen == 0) || !bytes_fit(pos, end, blockLen)) {
            return GDX_META_ERR_LAYOUT;
        }
        rc = gdx_meta_decode(data + pos, blockLen, bind, &sMeta);
        if (rc != GDX_META_OK) {
            return rc;
        }
        /* A shared file cannot mark a track as made on this install. */
        sMeta.madeHere = 0;
        pos += blockLen;
        if (visit != NULL) {
            visit(courseId, bind, &sMeta, ctx);
        }
    }
    return (pos == end) ? GDX_META_OK : GDX_META_ERR_LAYOUT;
}

int gdx_meta_sidecar_decode(const unsigned char* data, size_t length, GdxMetaSidecarVisit visit, void* ctx) {
    uint32_t count;
    size_t end;
    int rc;

    if (data == NULL) {
        return GDX_META_ERR_ARGS;
    }
    if (length < GDX_META_SIDECAR_HEADER_SIZE + 4) {
        return GDX_META_ERR_TRUNCATED;
    }
    if ((data[0] != 'G') || (data[1] != 'M') || (data[2] != 'X') || (data[3] != '1')) {
        return GDX_META_ERR_MAGIC;
    }
    if (get_u16le(data + 4) != 1) {
        return GDX_META_ERR_VERSION;
    }
    count = get_u16le(data + 6);
    if ((count < 1) || (count > GDX_META_SIDECAR_MAX_ENTRIES) || (get_u32le(data + 8) != (uint32_t) length)) {
        return GDX_META_ERR_LAYOUT;
    }
    end = length - 4;
    if (get_u32le(data + end) != gdx_course_registry_crc32(data, end)) {
        return GDX_META_ERR_CRC;
    }
    rc = sidecar_walk(data, length, NULL, NULL);
    if ((rc != GDX_META_OK) || (visit == NULL)) {
        return rc;
    }
    return sidecar_walk(data, length, visit, ctx);
}
