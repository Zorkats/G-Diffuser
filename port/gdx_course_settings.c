/* port/gdx_course_settings.c -- see gdx_course_settings.h. */

#include "gdx_course_settings.h"

#include "gdx_course_registry.h"

#include <limits.h>
#include <string.h>

#define KIND_RING 0
#define KIND_FORK_FIRST 1

typedef struct SettingsSlot {
    int used;
    char courseId[GDX_BRANCH_COURSE_ID_MAX + 1];
    GdxCourseSettings settings;
} SettingsSlot;

static SettingsSlot sStore[GDX_BRANCH_STORE_SLOTS];
static SettingsSlot sStage[GDX_BRANCH_STORE_SLOTS];
static unsigned long sRevision;
static GdxCourseSettings sActive;
static int sActiveValid;

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

static int extras_has_pad(const GdxPointExtras* extras) {
    return (extras->padKind != 0) || (extras->padWidth != 0) || (extras->padBoost != 0);
}

static int extras_has_wind(const GdxPointExtras* extras) {
    return (extras->windStrength != 0) || (extras->windAngle != 0);
}

static int extras_empty(const GdxPointExtras* extras) {
    return (extras->pitWidth == 0) && (extras->dirtWidth == 0) && (extras->iceWidth == 0) &&
           (extras->magnetStrength == 0) && (extras->magnetRange == 0) && !extras_has_pad(extras) &&
           !extras_has_wind(extras);
}

/* strength 1..100 carries an angle; 0 has angle 0, so one set of values has one encoding. */
static int wind_ok(int strength, int angle) {
    if ((strength >= 1) && (strength <= GDX_SETTINGS_WIND_MAX)) {
        return angle <= GDX_SETTINGS_WIND_ANGLE_MAX;
    }
    return (strength == 0) && (angle == 0);
}

/* What test builds stored as a calm point: no wind on its segment. */
#define WIND_OLD_CALM 255

static int pad_ok(const GdxPointExtras* extras) {
    if ((extras->padKind > GDX_SETTINGS_PAD_BRAKE) || (extras->padWidth > GDX_SETTINGS_WIDTH_MAX)) {
        return 0;
    }
    if (extras->padBoost == 0) {
        return 1;
    }
    return (extras->padKind == GDX_SETTINGS_PAD_SUPER) && (extras->padBoost >= GDX_SETTINGS_PAD_BOOST_MIN) &&
           (extras->padBoost <= GDX_SETTINGS_PAD_BOOST_MAX);
}

static int extras_ok(const GdxPointExtras* extras) {
    return (extras->pitWidth <= GDX_SETTINGS_WIDTH_MAX) && (extras->dirtWidth <= GDX_SETTINGS_WIDTH_MAX) &&
           (extras->iceWidth <= GDX_SETTINGS_WIDTH_MAX) &&
           ((extras->magnetStrength == 0) || ((extras->magnetStrength >= GDX_SETTINGS_STRENGTH_MIN) &&
                                              (extras->magnetStrength <= GDX_SETTINGS_STRENGTH_MAX))) &&
           ((extras->magnetRange == 0) ||
            ((extras->magnetRange >= GDX_SETTINGS_RANGE_MIN) && (extras->magnetRange <= GDX_SETTINGS_RANGE_MAX))) &&
           pad_ok(extras) && wind_ok(extras->windStrength, extras->windAngle);
}

int gdx_settings_is_empty(const GdxCourseSettings* settings) {
    int i;
    int j;

    if ((settings->lavaMode != 0) || (settings->magnetStrength != 0) || (settings->magnetRange != 0) ||
        (settings->aiAggression != 0)) {
        return 0;
    }
    for (i = 0; i < GDX_SETTINGS_MAX_POINTS; i++) {
        if (!extras_empty(&settings->ring[i])) {
            return 0;
        }
    }
    for (i = 0; i < GDX_BRANCH_MAX_FORKS; i++) {
        for (j = 0; j < GDX_BRANCH_MAX_NODES; j++) {
            if (!extras_empty(&settings->nodes[i][j])) {
                return 0;
            }
        }
    }
    return 1;
}

int gdx_settings_validate(const GdxCourseSettings* settings) {
    int i;
    int j;

    if (settings == NULL) {
        return GDX_SETTINGS_ERR_ARGS;
    }
    if ((settings->lavaMode > GDX_SETTINGS_LAVA_NEVER_KILLS) || (settings->ringCount > GDX_SETTINGS_MAX_POINTS) ||
        (settings->aiAggression > GDX_SETTINGS_AI_BRUTAL)) {
        return GDX_SETTINGS_ERR_VALUE;
    }
    if ((settings->magnetStrength != 0) && ((settings->magnetStrength < GDX_SETTINGS_STRENGTH_MIN) ||
                                            (settings->magnetStrength > GDX_SETTINGS_STRENGTH_MAX))) {
        return GDX_SETTINGS_ERR_VALUE;
    }
    if ((settings->magnetRange != 0) &&
        ((settings->magnetRange < GDX_SETTINGS_RANGE_MIN) || (settings->magnetRange > GDX_SETTINGS_RANGE_MAX))) {
        return GDX_SETTINGS_ERR_VALUE;
    }
    for (i = 0; i < GDX_SETTINGS_MAX_POINTS; i++) {
        if (!extras_ok(&settings->ring[i]) || ((i >= settings->ringCount) && !extras_empty(&settings->ring[i]))) {
            return GDX_SETTINGS_ERR_VALUE;
        }
    }
    for (i = 0; i < GDX_BRANCH_MAX_FORKS; i++) {
        for (j = 0; j < GDX_BRANCH_MAX_NODES; j++) {
            if (!extras_ok(&settings->nodes[i][j])) {
                return GDX_SETTINGS_ERR_VALUE;
            }
        }
    }
    return GDX_SETTINGS_OK;
}

static size_t put_entry(unsigned char* out, int kind, int index, const GdxPointExtras* extras, size_t entryBytes) {
    out[0] = (unsigned char) kind;
    out[1] = (unsigned char) index;
    out[2] = extras->pitWidth;
    out[3] = extras->dirtWidth;
    out[4] = extras->iceWidth;
    out[5] = extras->magnetStrength;
    out[6] = extras->magnetRange;
    if (entryBytes >= GDX_SETTINGS_ENTRY_BYTES_V2) {
        out[7] = extras->padKind;
        out[8] = extras->padWidth;
        out[9] = extras->padBoost;
    }
    if (entryBytes == GDX_SETTINGS_ENTRY_BYTES_V3) {
        out[10] = extras->windStrength;
        put_u16le(out + 11, extras->windAngle);
    }
    return entryBytes;
}

float gdx_settings_ai_scale(int aiAggression) {
    switch (aiAggression) {
        case GDX_SETTINGS_AI_CALM:
            return 0.5f;
        case GDX_SETTINGS_AI_FIERCE:
            return 1.5f;
        case GDX_SETTINGS_AI_BRUTAL:
            return 2.0f;
        default:
            return 1.0f;
    }
}

float gdx_settings_ai_resolve(int aiAggression, float optionScale) {
    switch (aiAggression) {
        case GDX_SETTINGS_AI_CALM:
            return optionScale * 0.5f;
        case GDX_SETTINGS_AI_NORMAL:
            return 1.0f;
        case GDX_SETTINGS_AI_FIERCE:
            return optionScale * 1.5f;
        case GDX_SETTINGS_AI_BRUTAL:
            return optionScale * 2.0f;
        default:
            return optionScale; /* Track, and any value out of range */
    }
}

/* The lowest version that holds the values: 4 with a CPU aggression preset, 3 with any wind, 2 with any dash pad
 * value, else 1. A course that does not use them stays readable by older builds. */
static int settings_version(const GdxCourseSettings* settings) {
    int version = 1;
    int i;
    int j;

    if (settings->aiAggression != 0) {
        return 4;
    }
    for (i = 0; i < settings->ringCount; i++) {
        if (extras_has_wind(&settings->ring[i])) {
            return 3;
        }
        if (extras_has_pad(&settings->ring[i])) {
            version = (version < 2) ? 2 : version;
        }
    }
    for (i = 0; i < GDX_BRANCH_MAX_FORKS; i++) {
        for (j = 0; j < GDX_BRANCH_MAX_NODES; j++) {
            if (extras_has_wind(&settings->nodes[i][j])) {
                return 3;
            }
            if (extras_has_pad(&settings->nodes[i][j])) {
                version = (version < 2) ? 2 : version;
            }
        }
    }
    return version;
}

static size_t entry_bytes(uint32_t version) {
    return (version >= 3) ? GDX_SETTINGS_ENTRY_BYTES_V3
                          : (version == 2) ? GDX_SETTINGS_ENTRY_BYTES_V2 : GDX_SETTINGS_ENTRY_BYTES;
}

static size_t header_bytes(uint32_t version) {
    return (version >= 3) ? GDX_SETTINGS_BLOCK_HEADER_SIZE_V3 : GDX_SETTINGS_BLOCK_HEADER_SIZE;
}

int gdx_settings_encode(const GdxCourseSettings* settings, uint32_t bindCrc, unsigned char* out, size_t cap) {
    size_t pos;
    size_t entryBytes;
    int version;
    int entries = 0;
    int rc;
    int i;
    int j;

    if (out == NULL) {
        return GDX_SETTINGS_ERR_ARGS;
    }
    rc = gdx_settings_validate(settings);
    if (rc != GDX_SETTINGS_OK) {
        return rc;
    }
    if (cap < GDX_SETTINGS_BLOCK_MAX_BYTES) {
        return GDX_SETTINGS_ERR_FULL;
    }
    version = settings_version(settings);
    entryBytes = entry_bytes((uint32_t) version);
    pos = header_bytes((uint32_t) version);
    for (i = 0; i < settings->ringCount; i++) {
        if (!extras_empty(&settings->ring[i])) {
            pos += put_entry(out + pos, KIND_RING, i, &settings->ring[i], entryBytes);
            entries++;
        }
    }
    for (i = 0; i < GDX_BRANCH_MAX_FORKS; i++) {
        for (j = 0; j < GDX_BRANCH_MAX_NODES; j++) {
            if (!extras_empty(&settings->nodes[i][j])) {
                pos += put_entry(out + pos, KIND_FORK_FIRST + i, j, &settings->nodes[i][j], entryBytes);
                entries++;
            }
        }
    }
    out[0] = 'G';
    out[1] = 'X';
    out[2] = 'K';
    out[3] = '1';
    put_u16le(out + 4, (uint32_t) version);
    out[6] = settings->ringCount;
    out[7] = settings->lavaMode;
    out[8] = settings->magnetStrength;
    out[9] = settings->magnetRange;
    put_u16le(out + 10, (uint32_t) entries);
    put_u32le(out + 12, bindCrc);
    if (version >= 3) {
        memset(out + 16, 0, 4);
    }
    if (version == 4) {
        out[16] = settings->aiAggression;
    }
    put_u32le(out + pos, gdx_course_registry_crc32(out, pos));
    return (int) (pos + 4);
}

static int decode_block(const unsigned char* data, size_t length, uint32_t bindCrc, GdxCourseSettings* out) {
    size_t end;
    size_t pos;
    size_t entryBytes;
    size_t headerBytes;
    uint32_t version;
    uint32_t entries;
    uint32_t i;
    int lastKey = -1;

    if (data == NULL) {
        return GDX_SETTINGS_ERR_ARGS;
    }
    if (length < GDX_SETTINGS_BLOCK_HEADER_SIZE + 4) {
        return GDX_SETTINGS_ERR_TRUNCATED;
    }
    if ((data[0] != 'G') || (data[1] != 'X') || (data[2] != 'K') || (data[3] != '1')) {
        return GDX_SETTINGS_ERR_MAGIC;
    }
    version = get_u16le(data + 4);
    if ((version < 1) || (version > 4)) {
        return GDX_SETTINGS_ERR_VERSION;
    }
    entryBytes = entry_bytes(version);
    headerBytes = header_bytes(version);
    if (length < headerBytes + 4) {
        return GDX_SETTINGS_ERR_TRUNCATED;
    }
    end = length - 4;
    if (get_u32le(data + end) != gdx_course_registry_crc32(data, end)) {
        return GDX_SETTINGS_ERR_CRC;
    }
    if (get_u32le(data + 12) != bindCrc) {
        return GDX_SETTINGS_ERR_BINDING;
    }
    entries = get_u16le(data + 10);
    if (headerBytes + (size_t) entries * entryBytes != end) {
        return GDX_SETTINGS_ERR_LAYOUT;
    }
    out->ringCount = data[6];
    out->lavaMode = data[7];
    out->magnetStrength = data[8];
    out->magnetRange = data[9];
    if (version == 4) {
        out->aiAggression = data[16];
        if (out->aiAggression == 0) {
            return GDX_SETTINGS_ERR_LAYOUT; /* version 4 is only written for a preset */
        }
    }
    if (out->ringCount > GDX_SETTINGS_MAX_POINTS) {
        return GDX_SETTINGS_ERR_VALUE;
    }
    pos = headerBytes;
    for (i = 0; i < entries; i++, pos += entryBytes) {
        const int kind = data[pos];
        const int index = data[pos + 1];
        const int key = (kind * 256) + index;
        GdxPointExtras extras;

        memset(&extras, 0, sizeof(extras));
        extras.pitWidth = data[pos + 2];
        extras.dirtWidth = data[pos + 3];
        extras.iceWidth = data[pos + 4];
        extras.magnetStrength = data[pos + 5];
        extras.magnetRange = data[pos + 6];
        if (version >= 2) {
            extras.padKind = data[pos + 7];
            extras.padWidth = data[pos + 8];
            extras.padBoost = data[pos + 9];
        }
        if (version >= 3) {
            extras.windStrength = data[pos + 10];
            extras.windAngle = (uint16_t) get_u16le(data + pos + 11);
            if ((extras.windStrength == WIND_OLD_CALM) && (extras.windAngle == 0)) {
                extras.windStrength = 0;
                if (key > lastKey && extras_empty(&extras)) {
                    lastKey = key;
                    continue;
                }
            }
        }
        if ((key <= lastKey) || extras_empty(&extras)) {
            return GDX_SETTINGS_ERR_LAYOUT;
        }
        lastKey = key;
        if (kind == KIND_RING) {
            if (index >= out->ringCount) {
                return GDX_SETTINGS_ERR_LAYOUT;
            }
            out->ring[index] = extras;
        } else if ((kind < KIND_FORK_FIRST + GDX_BRANCH_MAX_FORKS) && (index < GDX_BRANCH_MAX_NODES)) {
            out->nodes[kind - KIND_FORK_FIRST][index] = extras;
        } else {
            return GDX_SETTINGS_ERR_LAYOUT;
        }
    }
    return gdx_settings_validate(out);
}

int gdx_settings_decode(const unsigned char* data, size_t length, uint32_t bindCrc, GdxCourseSettings* out) {
    int rc;

    if (out == NULL) {
        return GDX_SETTINGS_ERR_ARGS;
    }
    memset(out, 0, sizeof(*out));
    rc = decode_block(data, length, bindCrc, out);
    if (rc != GDX_SETTINGS_OK) {
        memset(out, 0, sizeof(*out));
    }
    return rc;
}

int gdx_settings_rebind(const unsigned char* data, size_t length, uint32_t oldBindCrc, uint32_t newBindCrc,
                        unsigned char* out, size_t cap) {
    static GdxCourseSettings scratch;
    int rc;

    if (out == NULL) {
        return GDX_SETTINGS_ERR_ARGS;
    }
    rc = gdx_settings_decode(data, length, oldBindCrc, &scratch);
    if (rc != GDX_SETTINGS_OK) {
        return rc;
    }
    if ((cap < length) || (length > (size_t) INT_MAX)) {
        return GDX_SETTINGS_ERR_FULL;
    }
    memmove(out, data, length);
    put_u32le(out + 12, newBindCrc);
    put_u32le(out + length - 4, gdx_course_registry_crc32(out, length - 4));
    return (int) length;
}

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

int gdx_settings_store_set(const char* courseId, const GdxCourseSettings* settings) {
    int index;
    int rc;
    int i;

    if (!id_ok(courseId) || (settings == NULL)) {
        return GDX_SETTINGS_ERR_ARGS;
    }
    index = store_find(courseId);
    if (gdx_settings_is_empty(settings)) {
        if (index >= 0) {
            memset(&sStore[index], 0, sizeof(sStore[index]));
            sRevision++;
        }
        return GDX_SETTINGS_OK;
    }
    rc = gdx_settings_validate(settings);
    if (rc != GDX_SETTINGS_OK) {
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
            return GDX_SETTINGS_ERR_FULL;
        }
        sStore[index].used = 1;
        memcpy(sStore[index].courseId, courseId, strlen(courseId) + 1);
    }
    sStore[index].settings = *settings;
    sRevision++;
    return GDX_SETTINGS_OK;
}

int gdx_settings_store_get(const char* courseId, GdxCourseSettings* out) {
    int index;

    if (!id_ok(courseId) || (out == NULL)) {
        return 0;
    }
    index = store_find(courseId);
    if (index < 0) {
        return 0;
    }
    *out = sStore[index].settings;
    return 1;
}

int gdx_settings_store_rename(const char* oldId, const char* newId) {
    int oldIndex;
    int newIndex;

    if (!id_ok(oldId) || !id_ok(newId)) {
        return GDX_SETTINGS_ERR_ARGS;
    }
    if (strcmp(oldId, newId) == 0) {
        return GDX_SETTINGS_OK;
    }
    oldIndex = store_find(oldId);
    newIndex = store_find(newId);
    if ((oldIndex < 0) && (newIndex < 0)) {
        return GDX_SETTINGS_OK;
    }
    if (newIndex >= 0) {
        memset(&sStore[newIndex], 0, sizeof(sStore[newIndex]));
    }
    if (oldIndex >= 0) {
        memcpy(sStore[oldIndex].courseId, newId, strlen(newId) + 1);
    }
    sRevision++;
    return GDX_SETTINGS_OK;
}

void gdx_settings_store_clear(void) {
    memset(sStore, 0, sizeof(sStore));
    sRevision++;
}

unsigned long gdx_settings_store_revision(void) {
    return sRevision;
}

int gdx_settings_store_encode_file(unsigned char* out, size_t cap) {
    unsigned char block[GDX_SETTINGS_BLOCK_MAX_BYTES];
    size_t pos = GDX_SETTINGS_FILE_HEADER_SIZE;
    int count = 0;
    int i;

    if (out == NULL) {
        return GDX_SETTINGS_ERR_ARGS;
    }
    for (i = 0; i < GDX_BRANCH_STORE_SLOTS; i++) {
        size_t idLen;
        int written;

        if (!sStore[i].used) {
            continue;
        }
        idLen = strlen(sStore[i].courseId);
        written = gdx_settings_encode(&sStore[i].settings,
                                      gdx_course_registry_crc32((const unsigned char*) sStore[i].courseId, idLen),
                                      block, sizeof(block));
        if (written < 0) {
            return written;
        }
        if (!bytes_fit(pos, cap, 1 + idLen + 2 + (size_t) written + 4)) {
            return GDX_SETTINGS_ERR_FULL;
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
        return GDX_SETTINGS_ERR_FULL;
    }
    out[0] = 'G';
    out[1] = 'K';
    out[2] = 'S';
    out[3] = '1';
    put_u16le(out + 4, 1);
    put_u16le(out + 6, (uint32_t) count);
    put_u32le(out + 8, (uint32_t) (pos + 4));
    put_u32le(out + pos, gdx_course_registry_crc32(out, pos));
    return (int) (pos + 4);
}

int gdx_settings_store_restore_file(const unsigned char* data, size_t length) {
    size_t end;
    size_t pos;
    uint32_t count;
    uint32_t i;

    if (data == NULL) {
        return GDX_SETTINGS_ERR_ARGS;
    }
    if (length < GDX_SETTINGS_FILE_HEADER_SIZE + 4) {
        return GDX_SETTINGS_ERR_TRUNCATED;
    }
    if ((data[0] != 'G') || (data[1] != 'K') || (data[2] != 'S') || (data[3] != '1')) {
        return GDX_SETTINGS_ERR_MAGIC;
    }
    if (get_u16le(data + 4) != 1) {
        return GDX_SETTINGS_ERR_VERSION;
    }
    count = get_u16le(data + 6);
    if ((count > GDX_BRANCH_STORE_SLOTS) || (get_u32le(data + 8) != (uint32_t) length)) {
        return GDX_SETTINGS_ERR_LAYOUT;
    }
    end = length - 4;
    if (get_u32le(data + end) != gdx_course_registry_crc32(data, end)) {
        return GDX_SETTINGS_ERR_CRC;
    }
    memset(sStage, 0, sizeof(sStage));
    pos = GDX_SETTINGS_FILE_HEADER_SIZE;
    for (i = 0; i < count; i++) {
        size_t idLen;
        size_t blockLen;
        uint32_t j;
        int rc;

        if (!bytes_fit(pos, end, 1)) {
            return GDX_SETTINGS_ERR_LAYOUT;
        }
        idLen = data[pos];
        pos++;
        if ((idLen < 1) || (idLen > GDX_BRANCH_COURSE_ID_MAX) || !bytes_fit(pos, end, idLen + 2)) {
            return GDX_SETTINGS_ERR_LAYOUT;
        }
        memcpy(sStage[i].courseId, data + pos, idLen);
        sStage[i].courseId[idLen] = '\0';
        if (!id_ok(sStage[i].courseId)) {
            return GDX_SETTINGS_ERR_LAYOUT;
        }
        for (j = 0; j < i; j++) {
            if (strcmp(sStage[j].courseId, sStage[i].courseId) == 0) {
                return GDX_SETTINGS_ERR_LAYOUT;
            }
        }
        pos += idLen;
        blockLen = get_u16le(data + pos);
        pos += 2;
        if (!bytes_fit(pos, end, blockLen)) {
            return GDX_SETTINGS_ERR_LAYOUT;
        }
        rc = gdx_settings_decode(data + pos, blockLen,
                                 gdx_course_registry_crc32((const unsigned char*) sStage[i].courseId, idLen),
                                 &sStage[i].settings);
        if (rc != GDX_SETTINGS_OK) {
            return rc;
        }
        /* store_set treats an empty value as "delete", so a file entry that is empty would break that. */
        if (gdx_settings_is_empty(&sStage[i].settings)) {
            return GDX_SETTINGS_ERR_LAYOUT;
        }
        sStage[i].used = 1;
        pos += blockLen;
    }
    if (pos != end) {
        return GDX_SETTINGS_ERR_LAYOUT;
    }
    memcpy(sStore, sStage, sizeof(sStore));
    sRevision++;
    return GDX_SETTINGS_OK;
}

const GdxCourseSettings* gdx_settings_active(void) {
    return sActiveValid ? &sActive : NULL;
}

void gdx_settings_set_active(const GdxCourseSettings* settings) {
    if (settings == NULL) {
        memset(&sActive, 0, sizeof(sActive));
        sActiveValid = 0;
        return;
    }
    sActive = *settings;
    sActiveValid = 1;
}

/* ---------------------------------------------------------------------------------
 * .gdxb sidecar.
 * ------------------------------------------------------------------------------- */

static int entry_has_forks(const GdxSidecarEntry* entry) {
    return (entry->forks != NULL) && (entry->forks->forkCount > 0 || entry->forks->extendedCount > 0);
}

static int entry_has_settings(const GdxSidecarEntry* entry) {
    return (entry->settings != NULL) && !gdx_settings_is_empty(entry->settings);
}

int gdx_course_sidecar_entry_has_data(const GdxSidecarEntry* entry) {
    return (entry != NULL) && (entry_has_forks(entry) || entry_has_settings(entry));
}

int gdx_course_sidecar_encode(const GdxSidecarEntry* entries, int count, unsigned char* out, size_t cap) {
    static unsigned char sBlock[GDX_BRANCH_BLOCK_MAX_BYTES + GDX_SETTINGS_BLOCK_MAX_BYTES];
    size_t pos = GDX_SIDECAR_HEADER_SIZE;
    int written = 0;
    int i;

    if ((entries == NULL) || (out == NULL) || (count < 0) || (count > GDX_SIDECAR_MAX_ENTRIES)) {
        return GDX_SETTINGS_ERR_ARGS;
    }
    for (i = 0; i < count; i++) {
        const GdxSidecarEntry* entry = &entries[i];
        size_t idLen;
        int forkLen = 0;
        int settingsLen = 0;

        if (!gdx_course_sidecar_entry_has_data(entry)) {
            continue;
        }
        if (!id_ok(entry->courseId)) {
            return GDX_SETTINGS_ERR_ARGS;
        }
        idLen = strlen(entry->courseId);
        if (entry_has_forks(entry)) {
            forkLen = gdx_branch_encode(entry->forks, entry->bindCrc, sBlock, GDX_BRANCH_BLOCK_MAX_BYTES);
            if (forkLen < 0) {
                return forkLen;
            }
        }
        if (entry_has_settings(entry)) {
            settingsLen = gdx_settings_encode(entry->settings, entry->bindCrc, sBlock + forkLen,
                                              GDX_SETTINGS_BLOCK_MAX_BYTES);
            if (settingsLen < 0) {
                return settingsLen;
            }
        }
        if (!bytes_fit(pos, cap, 1 + idLen + 4 + 2 + (size_t) forkLen + 2 + (size_t) settingsLen + 4)) {
            return GDX_SETTINGS_ERR_FULL;
        }
        out[pos] = (unsigned char) idLen;
        memcpy(out + pos + 1, entry->courseId, idLen);
        pos += 1 + idLen;
        put_u32le(out + pos, entry->bindCrc);
        pos += 4;
        put_u16le(out + pos, (uint32_t) forkLen);
        pos += 2;
        memcpy(out + pos, sBlock, (size_t) forkLen);
        pos += (size_t) forkLen;
        put_u16le(out + pos, (uint32_t) settingsLen);
        pos += 2;
        memcpy(out + pos, sBlock + forkLen, (size_t) settingsLen);
        pos += (size_t) settingsLen;
        written++;
    }
    if (written == 0) {
        return 0;
    }
    if (!bytes_fit(pos, cap, 4)) {
        return GDX_SETTINGS_ERR_FULL;
    }
    out[0] = 'G';
    out[1] = 'X';
    out[2] = 'D';
    out[3] = '1';
    put_u16le(out + 4, 1);
    put_u16le(out + 6, (uint32_t) written);
    put_u32le(out + 8, (uint32_t) (pos + 4));
    put_u32le(out + pos, gdx_course_registry_crc32(out, pos));
    return (int) (pos + 4);
}

/* One pass of the file. visit NULL only checks it. */
static int sidecar_walk(const unsigned char* data, size_t length, GdxSidecarVisit visit, void* ctx) {
    static GdxBranchSet sForks;
    static GdxCourseSettings sSettings;
    const size_t end = length - 4;
    const uint32_t count = get_u16le(data + 6);
    size_t pos = GDX_SIDECAR_HEADER_SIZE;
    uint32_t i;

    for (i = 0; i < count; i++) {
        char courseId[GDX_BRANCH_COURSE_ID_MAX + 1];
        size_t idLen;
        size_t blockLen;
        uint32_t bind;
        int haveForks = 0;
        int haveSettings = 0;
        int rc;

        if (!bytes_fit(pos, end, 1)) {
            return GDX_SETTINGS_ERR_LAYOUT;
        }
        idLen = data[pos];
        pos++;
        if ((idLen < 1) || (idLen > GDX_BRANCH_COURSE_ID_MAX) || !bytes_fit(pos, end, idLen + 4 + 2)) {
            return GDX_SETTINGS_ERR_LAYOUT;
        }
        memcpy(courseId, data + pos, idLen);
        courseId[idLen] = '\0';
        if (!id_ok(courseId)) {
            return GDX_SETTINGS_ERR_LAYOUT;
        }
        pos += idLen;
        bind = get_u32le(data + pos);
        pos += 4;

        blockLen = get_u16le(data + pos);
        pos += 2;
        if (!bytes_fit(pos, end, blockLen + 2)) {
            return GDX_SETTINGS_ERR_LAYOUT;
        }
        if (blockLen > 0) {
            rc = gdx_branch_decode(data + pos, blockLen, bind, &sForks);
            if (rc != GDX_BRANCH_OK) {
                return rc;
            }
            haveForks = 1;
        }
        pos += blockLen;

        blockLen = get_u16le(data + pos);
        pos += 2;
        if (!bytes_fit(pos, end, blockLen)) {
            return GDX_SETTINGS_ERR_LAYOUT;
        }
        if (blockLen > 0) {
            rc = gdx_settings_decode(data + pos, blockLen, bind, &sSettings);
            if (rc != GDX_SETTINGS_OK) {
                return rc;
            }
            haveSettings = 1;
        }
        pos += blockLen;

        if (visit != NULL) {
            visit(courseId, bind, haveForks ? &sForks : NULL, haveSettings ? &sSettings : NULL, ctx);
        }
    }
    return (pos == end) ? GDX_SETTINGS_OK : GDX_SETTINGS_ERR_LAYOUT;
}

int gdx_course_sidecar_decode(const unsigned char* data, size_t length, GdxSidecarVisit visit, void* ctx) {
    uint32_t count;
    size_t end;
    int rc;

    if (data == NULL) {
        return GDX_SETTINGS_ERR_ARGS;
    }
    if (length < GDX_SIDECAR_HEADER_SIZE + 4) {
        return GDX_SETTINGS_ERR_TRUNCATED;
    }
    if ((data[0] != 'G') || (data[1] != 'X') || (data[2] != 'D') || (data[3] != '1')) {
        return GDX_SETTINGS_ERR_MAGIC;
    }
    if (get_u16le(data + 4) != 1) {
        return GDX_SETTINGS_ERR_VERSION;
    }
    count = get_u16le(data + 6);
    if ((count < 1) || (count > GDX_SIDECAR_MAX_ENTRIES) || (get_u32le(data + 8) != (uint32_t) length)) {
        return GDX_SETTINGS_ERR_LAYOUT;
    }
    end = length - 4;
    if (get_u32le(data + end) != gdx_course_registry_crc32(data, end)) {
        return GDX_SETTINGS_ERR_CRC;
    }
    rc = sidecar_walk(data, length, NULL, NULL);
    if ((rc != GDX_SETTINGS_OK) || (visit == NULL)) {
        return rc;
    }
    return sidecar_walk(data, length, visit, ctx);
}
