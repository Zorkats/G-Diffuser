/* port/gdx_course_registry.c -- Course Edit element registry and GXE1 sidecar codec.
 *
 * Contract, model and format live in gdx_course_registry.h. This file is pure C with no decomp
 * headers, so tests/gdx_course_registry_tests.c builds it on its own. The stock tables mirror the
 * enums named above each table; the unit tests check that table index == stock ID.
 */

#include "gdx_course_registry.h"

#include <stdio.h>
#include <string.h>

/* ---------------------------------------------------------------------------------
 * Stock tables. Fields: key, display name, kind, stock ID (== table index), isExtension.
 * ------------------------------------------------------------------------------- */

/* Venue, decomp/include/fzx_course.h:105-117 (11 values, VENUE_ENDING last). */
static const GdxCourseElement sVenueStock[] = {
    { "venue.mute_city", "Mute City", GDX_COURSE_KIND_VENUE, 0, 0 },
    { "venue.port_town", "Port Town", GDX_COURSE_KIND_VENUE, 1, 0 },
    { "venue.big_blue", "Big Blue", GDX_COURSE_KIND_VENUE, 2, 0 },
    { "venue.sand_ocean", "Sand Ocean", GDX_COURSE_KIND_VENUE, 3, 0 },
    { "venue.devils_forest", "Devil's Forest", GDX_COURSE_KIND_VENUE, 4, 0 },
    { "venue.white_land", "White Land", GDX_COURSE_KIND_VENUE, 5, 0 },
    { "venue.sector", "Sector", GDX_COURSE_KIND_VENUE, 6, 0 },
    { "venue.red_canyon", "Red Canyon", GDX_COURSE_KIND_VENUE, 7, 0 },
    { "venue.fire_field", "Fire Field", GDX_COURSE_KIND_VENUE, 8, 0 },
    { "venue.silence", "Silence", GDX_COURSE_KIND_VENUE, 9, 0 },
    { "venue.ending", "Ending", GDX_COURSE_KIND_VENUE, 10, 0 },
};

/* Skybox, decomp/include/fzx_course.h:119-128 (8 values). */
static const GdxCourseElement sSkyboxStock[] = {
    { "skybox.purple", "Purple", GDX_COURSE_KIND_SKYBOX, 0, 0 },
    { "skybox.turquoise", "Turquoise", GDX_COURSE_KIND_SKYBOX, 1, 0 },
    { "skybox.desert", "Desert", GDX_COURSE_KIND_SKYBOX, 2, 0 },
    { "skybox.blue", "Blue", GDX_COURSE_KIND_SKYBOX, 3, 0 },
    { "skybox.night", "Night", GDX_COURSE_KIND_SKYBOX, 4, 0 },
    { "skybox.orange", "Orange", GDX_COURSE_KIND_SKYBOX, 5, 0 },
    { "skybox.sunset", "Sunset", GDX_COURSE_KIND_SKYBOX, 6, 0 },
    { "skybox.sky_blue", "Sky Blue", GDX_COURSE_KIND_SKYBOX, 7, 0 },
};

/* Raw BgmId, decomp/include/sfx.h:98-115, including the expansion-kit BGM_NEW_04 (13). The menu
 * order differs from the raw IDs; gBGMOptionToCourseBGM (expansion_kit/A3AE0.c:839) maps them. */
static const GdxCourseElement sBgmStock[] = {
    { "bgm.mute_city", "Mute City", GDX_COURSE_KIND_BGM, 0, 0 },
    { "bgm.silence", "Silence", GDX_COURSE_KIND_BGM, 1, 0 },
    { "bgm.sand_ocean", "Sand Ocean", GDX_COURSE_KIND_BGM, 2, 0 },
    { "bgm.port_town", "Port Town", GDX_COURSE_KIND_BGM, 3, 0 },
    { "bgm.big_blue", "Big Blue", GDX_COURSE_KIND_BGM, 4, 0 },
    { "bgm.devils_forest", "Devil's Forest", GDX_COURSE_KIND_BGM, 5, 0 },
    { "bgm.red_canyon", "Red Canyon", GDX_COURSE_KIND_BGM, 6, 0 },
    { "bgm.sector", "Sector", GDX_COURSE_KIND_BGM, 7, 0 },
    { "bgm.white_land", "White Land", GDX_COURSE_KIND_BGM, 8, 0 },
    { "bgm.rainbow_road", "Rainbow Road", GDX_COURSE_KIND_BGM, 9, 0 },
    { "bgm.new_03", "New 03", GDX_COURSE_KIND_BGM, 10, 0 },
    { "bgm.new_02", "New 02", GDX_COURSE_KIND_BGM, 11, 0 },
    { "bgm.new_01", "New 01", GDX_COURSE_KIND_BGM, 12, 0 },
    { "bgm.new_04", "New 04", GDX_COURSE_KIND_BGM, 13, 0 },
};

/* TrackPartStyle, decomp/include/fzx_expansion_kit.h:53-63 (9 values). */
static const GdxCourseElement sPartStyleStock[] = {
    { "part.pit", "Pit", GDX_COURSE_KIND_PART_STYLE, 0, 0 },
    { "part.dash", "Dash", GDX_COURSE_KIND_PART_STYLE, 1, 0 },
    { "part.dirt", "Dirt", GDX_COURSE_KIND_PART_STYLE, 2, 0 },
    { "part.slip", "Slip", GDX_COURSE_KIND_PART_STYLE, 3, 0 },
    { "part.jump", "Jump", GDX_COURSE_KIND_PART_STYLE, 4, 0 },
    { "part.trap", "Trap", GDX_COURSE_KIND_PART_STYLE, 5, 0 },
    { "part.gate", "Gate", GDX_COURSE_KIND_PART_STYLE, 6, 0 },
    { "part.building", "Building", GDX_COURSE_KIND_PART_STYLE, 7, 0 },
    { "part.sign", "Sign", GDX_COURSE_KIND_PART_STYLE, 8, 0 },
};

/* CourseEffects, decomp/include/fzx_racer.h:16-25, including the expansion-kit LAVA (5). */
static const GdxCourseElement sTrackEffectStock[] = {
    { "effect.none", "None", GDX_COURSE_KIND_TRACK_EFFECT, 0, 0 },
    { "effect.pit", "Pit", GDX_COURSE_KIND_TRACK_EFFECT, 1, 0 },
    { "effect.dirt", "Dirt", GDX_COURSE_KIND_TRACK_EFFECT, 2, 0 },
    { "effect.dash", "Dash", GDX_COURSE_KIND_TRACK_EFFECT, 3, 0 },
    { "effect.ice", "Ice", GDX_COURSE_KIND_TRACK_EFFECT, 4, 0 },
    { "effect.lava", "Lava", GDX_COURSE_KIND_TRACK_EFFECT, 5, 0 },
};

/* ---------------------------------------------------------------------------------
 * Runtime state. Extension entries point their key and name at buffers inside the same struct,
 * so the arrays are static and fixed size and never move.
 * ------------------------------------------------------------------------------- */

typedef struct GdxCourseExtEntry {
    GdxCourseElement view;
    char keyBuf[GDX_COURSE_KEY_MAX + 1];
    char nameBuf[GDX_COURSE_DISPLAY_MAX + 1];
    char pathBuf[GDX_COURSE_PATH_MAX];
} GdxCourseExtEntry;

static GdxCourseExtEntry sExtEntries[GDX_COURSE_KIND_COUNT][GDX_COURSE_EXT_PER_KIND];
static int sExtCount[GDX_COURSE_KIND_COUNT];

typedef struct GdxCourseStoreSlot {
    int used;
    char courseId[GDX_COURSE_FILE_ID_MAX + 1];
    int count;
    GdxCourseExtRecord records[GDX_COURSE_EXT_MAX_RECORDS];
} GdxCourseStoreSlot;

static GdxCourseStoreSlot sStore[GDX_COURSE_STORE_SLOTS];

typedef struct GdxCourseTitleSlot {
    int used;
    char courseId[GDX_COURSE_FILE_ID_MAX + 1];
    char title[GDX_COURSE_TITLE_MAX + 1];
} GdxCourseTitleSlot;

static GdxCourseTitleSlot sTitles[GDX_COURSE_TITLE_SLOTS];

/* Rises on every change to the extension or title store. See gdx_course_store_revision(). */
static unsigned long sStoreRevision = 0;

static void store_touch(void) {
    sStoreRevision++;
}

/* ---------------------------------------------------------------------------------
 * Validation helpers. Every string check reads at most one byte past its limit, so an
 * unterminated caller buffer cannot run away.
 * ------------------------------------------------------------------------------- */

static int kind_valid(int kind) {
    return kind >= 0 && kind < GDX_COURSE_KIND_COUNT;
}

static int scope_valid(int scope) {
    return scope == GDX_COURSE_SCOPE_COURSE || (scope >= 0 && scope < GDX_COURSE_SCOPE_POINTS);
}

static int key_char_ok(unsigned char c) {
    return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
}

static int display_name_valid(const char* name) {
    size_t i = 0;

    if (name == NULL) {
        return 0;
    }
    while (i <= GDX_COURSE_DISPLAY_MAX && name[i] != '\0') {
        if ((unsigned char) name[i] < 0x20 || (unsigned char) name[i] > 0x7E) {
            return 0;
        }
        i++;
    }
    return i >= 1 && i <= GDX_COURSE_DISPLAY_MAX;
}

static int course_id_valid(const char* courseId) {
    size_t i = 0;

    if (courseId == NULL) {
        return 0;
    }
    while (i <= GDX_COURSE_FILE_ID_MAX && courseId[i] != '\0') {
        i++;
    }
    return i >= 1 && i <= GDX_COURSE_FILE_ID_MAX;
}

int gdx_course_key_is_valid(const char* key) {
    size_t i = 0;

    if (key == NULL) {
        return 0;
    }
    while (i <= GDX_COURSE_KEY_MAX && key[i] != '\0') {
        if (!key_char_ok((unsigned char) key[i])) {
            return 0;
        }
        i++;
    }
    return i >= 1 && i <= GDX_COURSE_KEY_MAX;
}

/* Stock table and count for a kind. An invalid kind yields an empty table. */
static void stock_table(int kind, const GdxCourseElement** outTable, int* outCount) {
    *outTable = NULL;
    *outCount = 0;
    switch (kind) {
    case GDX_COURSE_KIND_VENUE:
        *outTable = sVenueStock;
        *outCount = (int) (sizeof(sVenueStock) / sizeof(sVenueStock[0]));
        break;
    case GDX_COURSE_KIND_SKYBOX:
        *outTable = sSkyboxStock;
        *outCount = (int) (sizeof(sSkyboxStock) / sizeof(sSkyboxStock[0]));
        break;
    case GDX_COURSE_KIND_BGM:
        *outTable = sBgmStock;
        *outCount = (int) (sizeof(sBgmStock) / sizeof(sBgmStock[0]));
        break;
    case GDX_COURSE_KIND_PART_STYLE:
        *outTable = sPartStyleStock;
        *outCount = (int) (sizeof(sPartStyleStock) / sizeof(sPartStyleStock[0]));
        break;
    case GDX_COURSE_KIND_TRACK_EFFECT:
        *outTable = sTrackEffectStock;
        *outCount = (int) (sizeof(sTrackEffectStock) / sizeof(sTrackEffectStock[0]));
        break;
    default:
        break;
    }
}

/* Returns GDX_COURSE_OK when a record's fields are legal, otherwise the specific error. */
static int record_check(const GdxCourseExtRecord* rec) {
    const GdxCourseElement* table = NULL;
    int stockCount = 0;

    if (!kind_valid(rec->kind)) {
        return GDX_COURSE_ERR_KIND;
    }
    if (!scope_valid(rec->scope)) {
        return GDX_COURSE_ERR_SCOPE;
    }
    if (!gdx_course_key_is_valid(rec->key)) {
        return GDX_COURSE_ERR_KEY;
    }
    stock_table(rec->kind, &table, &stockCount);
    if (rec->fallbackId < 0 || rec->fallbackId >= stockCount) {
        return GDX_COURSE_ERR_FALLBACK;
    }
    return GDX_COURSE_OK;
}

/* ---------------------------------------------------------------------------------
 * Registry queries.
 * ------------------------------------------------------------------------------- */

int gdx_course_registry_stock_count(GdxCourseKind kind) {
    const GdxCourseElement* table = NULL;
    int count = 0;

    if (!kind_valid(kind)) {
        return 0;
    }
    stock_table(kind, &table, &count);
    return count;
}

int gdx_course_registry_count(GdxCourseKind kind) {
    if (!kind_valid(kind)) {
        return 0;
    }
    return gdx_course_registry_stock_count(kind) + sExtCount[kind];
}

const GdxCourseElement* gdx_course_registry_at(GdxCourseKind kind, int index) {
    const GdxCourseElement* table = NULL;
    int stockCount = 0;

    if (!kind_valid(kind) || index < 0) {
        return NULL;
    }
    stock_table(kind, &table, &stockCount);
    if (index < stockCount) {
        return &table[index];
    }
    index -= stockCount;
    if (index < sExtCount[kind]) {
        return &sExtEntries[kind][index].view;
    }
    return NULL;
}

const GdxCourseElement* gdx_course_registry_find_key(GdxCourseKind kind, const char* key) {
    const GdxCourseElement* table = NULL;
    int stockCount = 0;
    int i;

    /* The key check also guarantees a terminator within the limit, so strcmp below is bounded. */
    if (!kind_valid(kind) || !gdx_course_key_is_valid(key)) {
        return NULL;
    }
    stock_table(kind, &table, &stockCount);
    for (i = 0; i < stockCount; i++) {
        if (strcmp(table[i].key, key) == 0) {
            return &table[i];
        }
    }
    for (i = 0; i < sExtCount[kind]; i++) {
        if (strcmp(sExtEntries[kind][i].view.key, key) == 0) {
            return &sExtEntries[kind][i].view;
        }
    }
    return NULL;
}

const GdxCourseElement* gdx_course_registry_find_stock(GdxCourseKind kind, int stockId) {
    const GdxCourseElement* table = NULL;
    int stockCount = 0;
    int i;

    if (!kind_valid(kind)) {
        return NULL;
    }
    stock_table(kind, &table, &stockCount);
    for (i = 0; i < stockCount; i++) {
        if (table[i].stockId == stockId) {
            return &table[i];
        }
    }
    return NULL;
}

/* ---------------------------------------------------------------------------------
 * Runtime extension registration and resolution.
 * ------------------------------------------------------------------------------- */

static int register_entry(GdxCourseKind kind, const char* key, const char* displayName,
                          int fallbackStockId, const char* sourcePath) {
    const GdxCourseElement* table = NULL;
    GdxCourseExtEntry* entry;
    int stockCount = 0;

    if (!kind_valid(kind)) {
        return GDX_COURSE_ERR_KIND;
    }
    if (key == NULL || displayName == NULL) {
        return GDX_COURSE_ERR_ARGS;
    }
    if (!gdx_course_key_is_valid(key)) {
        return GDX_COURSE_ERR_KEY;
    }
    if (!display_name_valid(displayName)) {
        return GDX_COURSE_ERR_NAME;
    }
    stock_table(kind, &table, &stockCount);
    if (fallbackStockId < 0 || fallbackStockId >= stockCount) {
        return GDX_COURSE_ERR_FALLBACK;
    }
    if (gdx_course_registry_find_key(kind, key) != NULL) {
        return GDX_COURSE_ERR_DUPLICATE;
    }
    if (sExtCount[kind] >= GDX_COURSE_EXT_PER_KIND) {
        return GDX_COURSE_ERR_FULL;
    }

    entry = &sExtEntries[kind][sExtCount[kind]];
    snprintf(entry->keyBuf, sizeof(entry->keyBuf), "%s", key);
    snprintf(entry->nameBuf, sizeof(entry->nameBuf), "%s", displayName);
    if (sourcePath != NULL) {
        snprintf(entry->pathBuf, sizeof(entry->pathBuf), "%s", sourcePath);
        entry->view.sourcePath = entry->pathBuf;
    } else {
        entry->pathBuf[0] = '\0';
        entry->view.sourcePath = NULL;
    }
    entry->view.key = entry->keyBuf;
    entry->view.displayName = entry->nameBuf;
    entry->view.kind = kind;
    entry->view.stockId = fallbackStockId;
    entry->view.isExtension = 1;
    sExtCount[kind]++;
    return GDX_COURSE_OK;
}

int gdx_course_registry_register(GdxCourseKind kind, const char* key, const char* displayName,
                                 int fallbackStockId) {
    return register_entry(kind, key, displayName, fallbackStockId, NULL);
}

int gdx_course_registry_register_file(GdxCourseKind kind, const char* key, const char* displayName,
                                      int fallbackStockId, const char* sourcePath) {
    size_t len = 0;

    if (sourcePath == NULL) {
        return GDX_COURSE_ERR_ARGS;
    }
    while (len < GDX_COURSE_PATH_MAX && sourcePath[len] != '\0') {
        len++;
    }
    if (len == 0 || len >= GDX_COURSE_PATH_MAX) {
        return GDX_COURSE_ERR_PATH;
    }
    return register_entry(kind, key, displayName, fallbackStockId, sourcePath);
}

/* Copies the stem into out as the key or label body. The character rule decides what each input byte
 * becomes; the copy stops when out is full. Returns the number of characters written. */
static size_t music_stem_key_body(const char* stem, char* out, size_t outCap, size_t start) {
    size_t written = start;
    size_t i = 0;

    while (stem[i] != '\0' && written + 1 < outCap) {
        unsigned char c = (unsigned char) stem[i];
        if (c >= 'A' && c <= 'Z') {
            c = (unsigned char) (c - 'A' + 'a');
        }
        out[written++] = key_char_ok(c) ? (char) c : '_';
        i++;
    }
    out[written] = '\0';
    return written;
}

int gdx_course_registry_music_key(const char* fileStem, char* out, size_t outCap) {
    size_t prefixLen = sizeof(GDX_COURSE_MUSIC_KEY_PREFIX) - 1;
    size_t written;

    if (fileStem == NULL || out == NULL || outCap < prefixLen + 2 || fileStem[0] == '\0') {
        return 0;
    }
    if (outCap > GDX_COURSE_KEY_MAX + 1) {
        outCap = GDX_COURSE_KEY_MAX + 1;
    }
    memcpy(out, GDX_COURSE_MUSIC_KEY_PREFIX, prefixLen);
    out[prefixLen] = '\0';
    written = music_stem_key_body(fileStem, out, outCap, prefixLen);
    return written > prefixLen ? 1 : 0;
}

int gdx_course_registry_music_fingerprint_key(const char* musicKey, uint32_t contentCrc, char* out, size_t outCap) {
    static const char kHex[] = "0123456789abcdef";
    size_t prefixLen = sizeof(GDX_COURSE_MUSIC_KEY_PREFIX) - 1;
    size_t bodyMax = GDX_COURSE_KEY_MAX - prefixLen - 9; /* room for '_' and eight hex digits */
    size_t bodyLen;
    size_t pos;
    int shift;

    if (musicKey == NULL || out == NULL || !gdx_course_key_is_valid(musicKey) ||
        strncmp(musicKey, GDX_COURSE_MUSIC_KEY_PREFIX, prefixLen) != 0 || musicKey[prefixLen] == '\0') {
        return 0;
    }
    bodyLen = strlen(musicKey + prefixLen);
    if (bodyLen > bodyMax) {
        bodyLen = bodyMax;
    }
    if (outCap < prefixLen + bodyLen + 9 + 1) {
        return 0;
    }
    memcpy(out, musicKey, prefixLen + bodyLen);
    pos = prefixLen + bodyLen;
    out[pos++] = '_';
    for (shift = 28; shift >= 0; shift -= 4) {
        out[pos++] = kHex[(contentCrc >> shift) & 0xF];
    }
    out[pos] = '\0';
    return 1;
}

int gdx_course_registry_music_display(const char* fileStem, char* out, size_t outCap) {
    size_t written = 0;
    size_t limit;
    int pendingSpace = 0;
    size_t i = 0;

    if (fileStem == NULL || out == NULL || outCap < 2) {
        return 0;
    }
    limit = outCap - 1 < GDX_COURSE_DISPLAY_MAX ? outCap - 1 : GDX_COURSE_DISPLAY_MAX;
    while (fileStem[i] != '\0') {
        unsigned char c = (unsigned char) fileStem[i];
        int drawable = 0;

        if (c >= 'a' && c <= 'z') {
            c = (unsigned char) (c - 'a' + 'A');
        }
        drawable = (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '&' || c == '+' ||
                   c == '-' || c == '.' || c == ',' || c == '(' || c == ')' || c == ':';
        if (!drawable) {
            if (written > 0) {
                pendingSpace = 1;
            }
        } else {
            size_t need = (pendingSpace ? 1 : 0) + 1;
            if (written + need > limit) {
                break;
            }
            if (pendingSpace) {
                out[written++] = ' ';
                pendingSpace = 0;
            }
            out[written++] = (char) c;
        }
        i++;
    }
    if (written == 0) {
        if (outCap < sizeof("CUSTOM")) {
            return 0;
        }
        memcpy(out, "CUSTOM", sizeof("CUSTOM"));
        return 1;
    }
    out[written] = '\0';
    return 1;
}

int gdx_course_registry_resolve(GdxCourseKind kind, const char* key, int* outMissing) {
    const GdxCourseElement* element = gdx_course_registry_find_key(kind, key);

    if (element == NULL) {
        if (outMissing != NULL) {
            *outMissing = 1;
        }
        return GDX_COURSE_DEFAULT_STOCK_ID;
    }
    if (outMissing != NULL) {
        *outMissing = 0;
    }
    return element->stockId;
}

void gdx_course_registry_clear_extensions(void) {
    memset(sExtEntries, 0, sizeof(sExtEntries));
    memset(sExtCount, 0, sizeof(sExtCount));
    memset(sStore, 0, sizeof(sStore));
    memset(sTitles, 0, sizeof(sTitles));
}

/* ---------------------------------------------------------------------------------
 * Per-course extension store.
 * ------------------------------------------------------------------------------- */

static int store_find_slot(const char* courseId) {
    int i;

    for (i = 0; i < GDX_COURSE_STORE_SLOTS; i++) {
        if (sStore[i].used && strcmp(sStore[i].courseId, courseId) == 0) {
            return i;
        }
    }
    return -1;
}

static int store_free_slot(void) {
    int i;

    for (i = 0; i < GDX_COURSE_STORE_SLOTS; i++) {
        if (!sStore[i].used) {
            return i;
        }
    }
    return -1;
}

int gdx_course_ext_set(const char* courseId, GdxCourseKind kind, int scope, const char* key) {
    const GdxCourseElement* element;
    GdxCourseStoreSlot* slot;
    GdxCourseExtRecord* record = NULL;
    int slotIndex;
    int i;

    if (!course_id_valid(courseId) || key == NULL) {
        return GDX_COURSE_ERR_ARGS;
    }
    if (!kind_valid(kind)) {
        return GDX_COURSE_ERR_KIND;
    }
    if (!scope_valid(scope)) {
        return GDX_COURSE_ERR_SCOPE;
    }
    element = gdx_course_registry_find_key(kind, key);
    if (element == NULL) {
        return GDX_COURSE_ERR_UNKNOWN;
    }
    if (!element->isExtension) {
        return GDX_COURSE_ERR_NOT_EXTENSION;
    }

    slotIndex = store_find_slot(courseId);
    if (slotIndex < 0) {
        slotIndex = store_free_slot();
        if (slotIndex < 0) {
            return GDX_COURSE_ERR_FULL;
        }
        slot = &sStore[slotIndex];
        slot->used = 1;
        slot->count = 0;
        snprintf(slot->courseId, sizeof(slot->courseId), "%s", courseId);
    }
    slot = &sStore[slotIndex];

    for (i = 0; i < slot->count; i++) {
        if (slot->records[i].kind == (int) kind && slot->records[i].scope == scope) {
            record = &slot->records[i];
            break;
        }
    }
    if (record == NULL) {
        if (slot->count >= GDX_COURSE_EXT_MAX_RECORDS) {
            return GDX_COURSE_ERR_FULL;
        }
        record = &slot->records[slot->count];
        slot->count++;
    }
    record->kind = (int) kind;
    record->scope = scope;
    record->fallbackId = element->stockId;
    snprintf(record->key, sizeof(record->key), "%s", element->key);
    store_touch();
    return GDX_COURSE_OK;
}

int gdx_course_ext_clear(const char* courseId, GdxCourseKind kind, int scope) {
    GdxCourseStoreSlot* slot;
    int slotIndex;
    int i;

    if (!course_id_valid(courseId)) {
        return GDX_COURSE_ERR_ARGS;
    }
    if (!kind_valid(kind)) {
        return GDX_COURSE_ERR_KIND;
    }
    if (!scope_valid(scope)) {
        return GDX_COURSE_ERR_SCOPE;
    }
    slotIndex = store_find_slot(courseId);
    if (slotIndex < 0) {
        return GDX_COURSE_ERR_UNKNOWN;
    }
    slot = &sStore[slotIndex];
    for (i = 0; i < slot->count; i++) {
        if (slot->records[i].kind == (int) kind && slot->records[i].scope == scope) {
            slot->records[i] = slot->records[slot->count - 1];
            slot->count--;
            if (slot->count == 0) {
                slot->used = 0;
            }
            store_touch();
            return GDX_COURSE_OK;
        }
    }
    return GDX_COURSE_ERR_UNKNOWN;
}

void gdx_course_ext_clear_course(const char* courseId) {
    int slotIndex;

    if (!course_id_valid(courseId)) {
        return;
    }
    slotIndex = store_find_slot(courseId);
    if (slotIndex >= 0) {
        sStore[slotIndex].used = 0;
        sStore[slotIndex].count = 0;
        store_touch();
    }
}

int gdx_course_ext_count(const char* courseId) {
    int slotIndex;

    if (!course_id_valid(courseId)) {
        return 0;
    }
    slotIndex = store_find_slot(courseId);
    return slotIndex < 0 ? 0 : sStore[slotIndex].count;
}

const GdxCourseExtRecord* gdx_course_ext_records(const char* courseId, int* outCount) {
    int slotIndex;

    if (outCount != NULL) {
        *outCount = 0;
    }
    if (!course_id_valid(courseId)) {
        return NULL;
    }
    slotIndex = store_find_slot(courseId);
    if (slotIndex < 0) {
        return NULL;
    }
    if (outCount != NULL) {
        *outCount = sStore[slotIndex].count;
    }
    return sStore[slotIndex].records;
}

/* ---------------------------------------------------------------------------------
 * GXE1 codec. Hand-packed little-endian; no struct is ever written to or read from bytes.
 * ------------------------------------------------------------------------------- */

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
    return (uint32_t) p[0] | ((uint32_t) p[1] << 8) | ((uint32_t) p[2] << 16) |
           ((uint32_t) p[3] << 24);
}

uint32_t gdx_course_registry_crc32(const unsigned char* data, size_t length) {
    uint32_t crc = 0xFFFFFFFFu;
    size_t i;
    int bit;

    for (i = 0; i < length; i++) {
        crc ^= data[i];
        for (bit = 0; bit < 8; bit++) {
            crc = (crc & 1u) ? ((crc >> 1) ^ 0xEDB88320u) : (crc >> 1);
        }
    }
    return crc ^ 0xFFFFFFFFu;
}

int gdx_course_ext_encode(const GdxCourseExtRecord* records, int count, uint32_t payloadCrc32,
                          unsigned char* out, size_t outCap) {
    size_t need = GDX_COURSE_EXT_HEADER_SIZE + 4;
    size_t keyLen;
    size_t pos;
    int check;
    int i;
    int j;

    if (out == NULL || count < 0 || count > GDX_COURSE_EXT_MAX_RECORDS || (count > 0 && records == NULL)) {
        return GDX_COURSE_ERR_ARGS;
    }
    /* Validate everything before the first byte is written, so a rejected block leaves no partial
     * output behind. */
    for (i = 0; i < count; i++) {
        check = record_check(&records[i]);
        if (check != GDX_COURSE_OK) {
            return check;
        }
        for (j = 0; j < i; j++) {
            if (records[j].kind == records[i].kind && records[j].scope == records[i].scope) {
                return GDX_COURSE_ERR_DUPLICATE;
            }
        }
        need += 4 + strlen(records[i].key);
    }
    if (need > outCap) {
        return GDX_COURSE_ERR_FULL;
    }

    out[0] = 'G';
    out[1] = 'X';
    out[2] = 'E';
    out[3] = '1';
    put_u16le(out + 4, GDX_COURSE_EXT_VERSION);
    put_u16le(out + 6, (uint32_t) count);
    put_u32le(out + 8, payloadCrc32);

    pos = GDX_COURSE_EXT_HEADER_SIZE;
    for (i = 0; i < count; i++) {
        keyLen = strlen(records[i].key);
        out[pos] = (unsigned char) records[i].kind;
        out[pos + 1] = (unsigned char) records[i].scope;
        out[pos + 2] = (unsigned char) records[i].fallbackId;
        out[pos + 3] = (unsigned char) keyLen;
        memcpy(out + pos + 4, records[i].key, keyLen);
        pos += 4 + keyLen;
    }
    put_u32le(out + pos, gdx_course_registry_crc32(out, pos));
    pos += 4;
    return (int) pos;
}

int gdx_course_ext_decode(const unsigned char* data, size_t length, uint32_t payloadCrc32,
                          GdxCourseExtBlock* out) {
    GdxCourseExtRecord* rec;
    uint32_t count;
    uint32_t keyLen;
    size_t end;
    size_t pos;
    int i;
    int j;
    int k;

    if (data == NULL || out == NULL) {
        return GDX_COURSE_ERR_ARGS;
    }
    out->count = 0;
    if (length < GDX_COURSE_EXT_MIN_BYTES) {
        return GDX_COURSE_EXT_ERR_TRUNCATED;
    }
    if (data[0] != 'G' || data[1] != 'X' || data[2] != 'E' || data[3] != '1') {
        return GDX_COURSE_EXT_ERR_MAGIC;
    }
    if (get_u16le(data + 4) != GDX_COURSE_EXT_VERSION) {
        return GDX_COURSE_EXT_ERR_VERSION;
    }
    count = get_u16le(data + 6);
    if (count > GDX_COURSE_EXT_MAX_RECORDS) {
        return GDX_COURSE_EXT_ERR_COUNT;
    }
    /* The CRC covers every byte before the trailing four, so one check catches truncation and any
     * bit flip before the content is trusted. */
    end = length - 4;
    if (get_u32le(data + end) != gdx_course_registry_crc32(data, end)) {
        return GDX_COURSE_EXT_ERR_CRC;
    }
    if (get_u32le(data + 8) != payloadCrc32) {
        return GDX_COURSE_EXT_ERR_BINDING;
    }

    pos = GDX_COURSE_EXT_HEADER_SIZE;
    for (i = 0; i < (int) count; i++) {
        rec = &out->records[i];
        if (end - pos < 4) {
            return GDX_COURSE_EXT_ERR_LAYOUT;
        }
        rec->kind = data[pos];
        rec->scope = data[pos + 1];
        rec->fallbackId = data[pos + 2];
        keyLen = data[pos + 3];
        pos += 4;
        if (keyLen == 0 || keyLen > GDX_COURSE_KEY_MAX) {
            return GDX_COURSE_EXT_ERR_RECORD;
        }
        if (keyLen > end - pos) {
            return GDX_COURSE_EXT_ERR_LAYOUT;
        }
        /* Checked byte by byte so an embedded NUL cannot shorten the key. */
        for (k = 0; k < (int) keyLen; k++) {
            if (!key_char_ok(data[pos + (size_t) k])) {
                return GDX_COURSE_EXT_ERR_RECORD;
            }
        }
        memcpy(rec->key, data + pos, keyLen);
        rec->key[keyLen] = '\0';
        pos += keyLen;
        if (record_check(rec) != GDX_COURSE_OK) {
            return GDX_COURSE_EXT_ERR_RECORD;
        }
        for (j = 0; j < i; j++) {
            if (out->records[j].kind == rec->kind && out->records[j].scope == rec->scope) {
                return GDX_COURSE_EXT_ERR_RECORD;
            }
        }
    }
    if (pos != end) {
        return GDX_COURSE_EXT_ERR_LAYOUT;
    }
    out->count = (int) count;
    return GDX_COURSE_OK;
}

/* ---------------------------------------------------------------------------------
 * Edit Cup short names (8A).
 * ------------------------------------------------------------------------------- */

/* A byte MFS accepts in a name on its own: printable ASCII, minus the characters MFS_VERSION_B
 * refuses ('/', '.', ':'). Multi-byte and high bytes are never copied into a short name. */
static int mfs_name_char_ok(unsigned char c) {
    return c >= 0x20 && c < 0x7F && c != '/' && c != '.' && c != ':';
}

static int title_valid(const char* title) {
    size_t i = 0;

    if (title == NULL) {
        return 0;
    }
    while (i <= GDX_COURSE_TITLE_MAX && title[i] != '\0') {
        if ((unsigned char) title[i] < 0x20 || (unsigned char) title[i] > 0x7E) {
            return 0;
        }
        i++;
    }
    return i >= 1 && i <= GDX_COURSE_TITLE_MAX;
}

static int name_taken(const char* candidate, const char* const* taken, int takenCount) {
    int i;

    for (i = 0; i < takenCount; i++) {
        if (taken[i] != NULL && strcmp(taken[i], candidate) == 0) {
            return 1;
        }
    }
    return 0;
}

int gdx_course_short_name(const char* longName, const char* const* taken, int takenCount, char* out,
                          size_t outCap) {
    char base[GDX_COURSE_SHORT_NAME_MAX + 1];
    char candidate[GDX_COURSE_SHORT_NAME_MAX + 1];
    size_t length = 0;
    size_t baseLen = 0;
    size_t i = 0;
    int n;

    if (longName == NULL || out == NULL || outCap < GDX_COURSE_SHORT_NAME_MAX + 1 || takenCount < 0 ||
        (takenCount > 0 && taken == NULL)) {
        return 0;
    }
    while (length <= GDX_COURSE_TITLE_MAX && longName[length] != '\0') {
        length++;
    }
    if (length == 0 || length > GDX_COURSE_TITLE_MAX) {
        return 0;
    }

    /* The first 8 characters MFS accepts, skipping the rest. */
    while (longName[i] != '\0' && baseLen < GDX_COURSE_SHORT_NAME_MAX) {
        if (mfs_name_char_ok((unsigned char) longName[i])) {
            base[baseLen++] = longName[i];
        }
        i++;
    }
    if (baseLen == 0) {
        return 0;
    }
    base[baseLen] = '\0';

    for (n = 0; n <= 999; n++) {
        if (n == 0) {
            snprintf(candidate, sizeof(candidate), "%s", base);
        } else {
            /* The "~N" suffix replaces the trailing characters, so the name stays at 8. */
            char suffix[8];
            size_t suffixLen;
            size_t keep;

            snprintf(suffix, sizeof(suffix), "~%d", n);
            suffixLen = strlen(suffix);
            keep = baseLen < GDX_COURSE_SHORT_NAME_MAX - suffixLen ? baseLen : GDX_COURSE_SHORT_NAME_MAX - suffixLen;
            memcpy(candidate, base, keep);
            memcpy(candidate + keep, suffix, suffixLen + 1);
        }
        if (!name_taken(candidate, taken, takenCount)) {
            snprintf(out, outCap, "%s", candidate);
            return 1;
        }
    }
    return 0;
}

/* ---------------------------------------------------------------------------------
 * Session title store.
 * ------------------------------------------------------------------------------- */

static int title_find_slot(const char* courseId) {
    int i;

    for (i = 0; i < GDX_COURSE_TITLE_SLOTS; i++) {
        if (sTitles[i].used && strcmp(sTitles[i].courseId, courseId) == 0) {
            return i;
        }
    }
    return -1;
}

int gdx_course_title_set(const char* courseId, const char* title) {
    int slotIndex;
    int i;

    if (!course_id_valid(courseId) || title == NULL) {
        return GDX_COURSE_ERR_ARGS;
    }
    if (!title_valid(title)) {
        return GDX_COURSE_ERR_NAME;
    }
    slotIndex = title_find_slot(courseId);
    if (slotIndex < 0) {
        for (i = 0; i < GDX_COURSE_TITLE_SLOTS; i++) {
            if (!sTitles[i].used) {
                slotIndex = i;
                break;
            }
        }
        if (slotIndex < 0) {
            return GDX_COURSE_ERR_FULL;
        }
        sTitles[slotIndex].used = 1;
        snprintf(sTitles[slotIndex].courseId, sizeof(sTitles[slotIndex].courseId), "%s", courseId);
    }
    snprintf(sTitles[slotIndex].title, sizeof(sTitles[slotIndex].title), "%s", title);
    store_touch();
    return GDX_COURSE_OK;
}

void gdx_course_title_clear(const char* courseId) {
    int slotIndex;

    if (!course_id_valid(courseId)) {
        return;
    }
    slotIndex = title_find_slot(courseId);
    if (slotIndex >= 0) {
        sTitles[slotIndex].used = 0;
        sTitles[slotIndex].courseId[0] = '\0';
        sTitles[slotIndex].title[0] = '\0';
        store_touch();
    }
}

int gdx_course_title_get(const char* courseId, char* out, size_t outCap) {
    int slotIndex;

    if (out == NULL || outCap == 0 || !course_id_valid(courseId)) {
        return 0;
    }
    slotIndex = title_find_slot(courseId);
    if (slotIndex < 0 || strlen(sTitles[slotIndex].title) >= outCap) {
        return 0;
    }
    snprintf(out, outCap, "%s", sTitles[slotIndex].title);
    return 1;
}

int gdx_course_title_find_course(const char* title, char* outCourseId, size_t outCap) {
    int i;

    if (title == NULL || outCourseId == NULL || outCap < GDX_COURSE_FILE_ID_MAX + 1) {
        return 0;
    }
    for (i = 0; i < GDX_COURSE_TITLE_SLOTS; i++) {
        if (sTitles[i].used && strcmp(sTitles[i].title, title) == 0) {
            snprintf(outCourseId, outCap, "%s", sTitles[i].courseId);
            return 1;
        }
    }
    return 0;
}

/* ---------------------------------------------------------------------------------
 * GXT1 title codec.
 * ------------------------------------------------------------------------------- */

int gdx_course_title_encode(const char* title, uint32_t payloadCrc32, unsigned char* out, size_t outCap) {
    size_t length = 0;
    size_t need;

    if (out == NULL) {
        return GDX_COURSE_ERR_ARGS;
    }
    if (!title_valid(title)) {
        return GDX_COURSE_ERR_NAME;
    }
    length = strlen(title);
    need = GDX_COURSE_TITLE_HEADER_SIZE + length + 4;
    if (need > outCap) {
        return GDX_COURSE_ERR_FULL;
    }

    out[0] = 'G';
    out[1] = 'X';
    out[2] = 'T';
    out[3] = '1';
    put_u16le(out + 4, GDX_COURSE_TITLE_VERSION);
    put_u16le(out + 6, (uint32_t) length);
    put_u32le(out + 8, payloadCrc32);
    memcpy(out + GDX_COURSE_TITLE_HEADER_SIZE, title, length);
    put_u32le(out + GDX_COURSE_TITLE_HEADER_SIZE + length,
              gdx_course_registry_crc32(out, GDX_COURSE_TITLE_HEADER_SIZE + length));
    return (int) need;
}

int gdx_course_title_decode(const unsigned char* data, size_t length, uint32_t payloadCrc32, char* outTitle,
                            size_t outCap) {
    size_t titleLen;
    size_t end;
    size_t i;

    if (data == NULL || outTitle == NULL) {
        return GDX_COURSE_ERR_ARGS;
    }
    if (length < GDX_COURSE_TITLE_HEADER_SIZE + 1 + 4) {
        return GDX_COURSE_TITLE_ERR_TRUNCATED;
    }
    if (data[0] != 'G' || data[1] != 'X' || data[2] != 'T' || data[3] != '1') {
        return GDX_COURSE_TITLE_ERR_MAGIC;
    }
    if (get_u16le(data + 4) != GDX_COURSE_TITLE_VERSION) {
        return GDX_COURSE_TITLE_ERR_VERSION;
    }
    titleLen = get_u16le(data + 6);
    if (titleLen < 1 || titleLen > GDX_COURSE_TITLE_MAX || length != GDX_COURSE_TITLE_HEADER_SIZE + titleLen + 4) {
        return GDX_COURSE_TITLE_ERR_LENGTH;
    }
    end = length - 4;
    if (get_u32le(data + end) != gdx_course_registry_crc32(data, end)) {
        return GDX_COURSE_TITLE_ERR_CRC;
    }
    if (get_u32le(data + 8) != payloadCrc32) {
        return GDX_COURSE_TITLE_ERR_BINDING;
    }
    for (i = 0; i < titleLen; i++) {
        unsigned char c = data[GDX_COURSE_TITLE_HEADER_SIZE + i];
        if (c < 0x20 || c > 0x7E) {
            return GDX_COURSE_TITLE_ERR_TEXT;
        }
    }
    if (outCap < titleLen + 1) {
        return GDX_COURSE_ERR_ARGS;
    }
    memcpy(outTitle, data + GDX_COURSE_TITLE_HEADER_SIZE, titleLen);
    outTitle[titleLen] = '\0';
    return GDX_COURSE_OK;
}

/* ---------------------------------------------------------------------------------
 * Restore and store-wide helpers (plan slice 9).
 * ------------------------------------------------------------------------------- */

int gdx_course_ext_restore(const char* courseId, const GdxCourseExtRecord* records, int count) {
    GdxCourseStoreSlot* slot;
    int slotIndex;
    int check;
    int i;
    int j;

    if (!course_id_valid(courseId) || count < 0 || count > GDX_COURSE_EXT_MAX_RECORDS ||
        (count > 0 && records == NULL)) {
        return GDX_COURSE_ERR_ARGS;
    }
    for (i = 0; i < count; i++) {
        check = record_check(&records[i]);
        if (check != GDX_COURSE_OK) {
            return check;
        }
        for (j = 0; j < i; j++) {
            if (records[j].kind == records[i].kind && records[j].scope == records[i].scope) {
                return GDX_COURSE_ERR_DUPLICATE;
            }
        }
    }

    slotIndex = store_find_slot(courseId);
    if (count == 0) {
        if (slotIndex >= 0) {
            sStore[slotIndex].used = 0;
            sStore[slotIndex].count = 0;
            store_touch();
        }
        return GDX_COURSE_OK;
    }
    if (slotIndex < 0) {
        slotIndex = store_free_slot();
        if (slotIndex < 0) {
            return GDX_COURSE_ERR_FULL;
        }
        sStore[slotIndex].used = 1;
        snprintf(sStore[slotIndex].courseId, sizeof(sStore[slotIndex].courseId), "%s", courseId);
    }
    slot = &sStore[slotIndex];
    memcpy(slot->records, records, (size_t) count * sizeof(GdxCourseExtRecord));
    slot->count = count;
    store_touch();
    return GDX_COURSE_OK;
}

void gdx_course_store_clear(void) {
    memset(sStore, 0, sizeof(sStore));
    memset(sTitles, 0, sizeof(sTitles));
    store_touch();
}

unsigned long gdx_course_store_revision(void) {
    return sStoreRevision;
}

/* ---------------------------------------------------------------------------------
 * GXS1 state file codec. Parsing runs twice when a visitor is given: the first pass validates the
 * whole file without calling anything, the second walks it. The scratch block and id table are
 * static because the codec runs on the menu thread only.
 * ------------------------------------------------------------------------------- */

static GdxCourseExtBlock sStateScratch;
static char sStateIds[GDX_COURSE_STATE_MAX_ENTRIES][GDX_COURSE_FILE_ID_MAX + 1];
static GdxCourseStateEntry sStateEntries[GDX_COURSE_STATE_MAX_ENTRIES];

static int state_id_char_ok(unsigned char c) {
    return c >= 0x20 && c <= 0x7E;
}

static int state_parse(const unsigned char* data, size_t length, GdxCourseStateVisit visit, void* ctx) {
    GdxCourseStateEntry entry;
    uint32_t count;
    uint32_t bindCrc;
    unsigned int flags;
    size_t idLen;
    size_t blockLen;
    size_t end;
    size_t pos;
    int check;
    int i;
    int j;
    int k;

    if (data == NULL) {
        return GDX_COURSE_ERR_ARGS;
    }
    if (length < GDX_COURSE_STATE_HEADER_SIZE + 4) {
        return GDX_COURSE_STATE_ERR_TRUNCATED;
    }
    if (data[0] != 'G' || data[1] != 'X' || data[2] != 'S' || data[3] != '1') {
        return GDX_COURSE_STATE_ERR_MAGIC;
    }
    if (get_u16le(data + 4) != GDX_COURSE_STATE_VERSION) {
        return GDX_COURSE_STATE_ERR_VERSION;
    }
    count = get_u16le(data + 6);
    if (count > GDX_COURSE_STATE_MAX_ENTRIES) {
        return GDX_COURSE_STATE_ERR_COUNT;
    }
    if (get_u32le(data + 8) != (uint32_t) length) {
        return GDX_COURSE_STATE_ERR_LENGTH;
    }
    end = length - 4;
    if (get_u32le(data + end) != gdx_course_registry_crc32(data, end)) {
        return GDX_COURSE_STATE_ERR_CRC;
    }

    pos = GDX_COURSE_STATE_HEADER_SIZE;
    for (i = 0; i < (int) count; i++) {
        idLen = data[pos];
        if (idLen < 1 || idLen > GDX_COURSE_FILE_ID_MAX || end - pos < idLen + 6) {
            return GDX_COURSE_STATE_ERR_LENGTH;
        }
        for (k = 0; k < (int) idLen; k++) {
            if (!state_id_char_ok(data[pos + 1 + (size_t) k])) {
                return GDX_COURSE_STATE_ERR_ENTRY;
            }
        }
        memcpy(sStateIds[i], data + pos + 1, idLen);
        sStateIds[i][idLen] = '\0';
        for (j = 0; j < i; j++) {
            if (strcmp(sStateIds[j], sStateIds[i]) == 0) {
                return GDX_COURSE_STATE_ERR_ENTRY;
            }
        }
        pos += 1 + idLen;
        bindCrc = get_u32le(data + pos);
        pos += 4;
        flags = data[pos];
        pos += 1;
        if ((flags & ~0x03u) != 0) {
            return GDX_COURSE_STATE_ERR_ENTRY;
        }

        memset(&entry, 0, sizeof(entry));
        snprintf(entry.courseId, sizeof(entry.courseId), "%s", sStateIds[i]);
        entry.bindCrc = bindCrc;

        if ((flags & 0x01u) != 0) {
            if (end - pos < 2) {
                return GDX_COURSE_STATE_ERR_LENGTH;
            }
            blockLen = get_u16le(data + pos);
            pos += 2;
            if (end - pos < blockLen) {
                return GDX_COURSE_STATE_ERR_LENGTH;
            }
            check = gdx_course_ext_decode(data + pos, blockLen, bindCrc, &sStateScratch);
            if (check != GDX_COURSE_OK) {
                return check;
            }
            entry.records = sStateScratch.records;
            entry.recordCount = sStateScratch.count;
            pos += blockLen;
        }
        if ((flags & 0x02u) != 0) {
            if (end - pos < 2) {
                return GDX_COURSE_STATE_ERR_LENGTH;
            }
            blockLen = get_u16le(data + pos);
            pos += 2;
            if (end - pos < blockLen) {
                return GDX_COURSE_STATE_ERR_LENGTH;
            }
            check = gdx_course_title_decode(data + pos, blockLen, bindCrc, entry.title, sizeof(entry.title));
            if (check != GDX_COURSE_OK) {
                return check;
            }
            entry.hasTitle = 1;
            pos += blockLen;
        }

        if (visit != NULL) {
            check = visit(ctx, &entry);
            if (check != GDX_COURSE_OK) {
                return check;
            }
        }
    }
    if (pos != end) {
        return GDX_COURSE_STATE_ERR_LENGTH;
    }
    return GDX_COURSE_OK;
}

int gdx_course_state_decode(const unsigned char* data, size_t length, GdxCourseStateVisit visit, void* ctx) {
    int check = state_parse(data, length, NULL, NULL);

    if (check != GDX_COURSE_OK || visit == NULL) {
        return check;
    }
    return state_parse(data, length, visit, ctx);
}

int gdx_course_state_encode(const GdxCourseStateEntry* entries, int count, unsigned char* out, size_t outCap) {
    const GdxCourseStateEntry* entry;
    size_t idLen;
    size_t pos;
    size_t room;
    int written;
    int flags;
    int i;
    int j;

    if (out == NULL || count < 0 || count > GDX_COURSE_STATE_MAX_ENTRIES || (count > 0 && entries == NULL)) {
        return GDX_COURSE_ERR_ARGS;
    }
    if (outCap < GDX_COURSE_STATE_HEADER_SIZE + 4) {
        return GDX_COURSE_ERR_FULL;
    }
    pos = GDX_COURSE_STATE_HEADER_SIZE;
    for (i = 0; i < count; i++) {
        entry = &entries[i];
        if (!course_id_valid(entry->courseId)) {
            return GDX_COURSE_STATE_ERR_ENTRY;
        }
        idLen = strlen(entry->courseId);
        for (j = 0; j < (int) idLen; j++) {
            if (!state_id_char_ok((unsigned char) entry->courseId[j])) {
                return GDX_COURSE_STATE_ERR_ENTRY;
            }
        }
        for (j = 0; j < i; j++) {
            if (strcmp(entries[j].courseId, entry->courseId) == 0) {
                return GDX_COURSE_STATE_ERR_ENTRY;
            }
        }
        if (entry->recordCount < 0 || entry->recordCount > GDX_COURSE_EXT_MAX_RECORDS ||
            (entry->recordCount > 0 && entry->records == NULL)) {
            return GDX_COURSE_ERR_ARGS;
        }
        flags = 0;
        if (entry->recordCount > 0) {
            flags |= 0x01;
        }
        if (entry->hasTitle) {
            flags |= 0x02;
        }

        room = outCap - 4; /* the trailing CRC is written last */
        if (pos + idLen + 6 > room) { /* length byte, id, bindCrc, flags */
            return GDX_COURSE_ERR_FULL;
        }
        out[pos] = (unsigned char) idLen;
        memcpy(out + pos + 1, entry->courseId, idLen);
        pos += 1 + idLen;
        put_u32le(out + pos, entry->bindCrc);
        pos += 4;
        out[pos] = (unsigned char) flags;
        pos += 1;

        if ((flags & 0x01) != 0) {
            room = outCap - 4;
            if (room < pos + 2) {
                return GDX_COURSE_ERR_FULL;
            }
            written = gdx_course_ext_encode(entry->records, entry->recordCount, entry->bindCrc, out + pos + 2,
                                            room - pos - 2);
            if (written < 0) {
                return written;
            }
            put_u16le(out + pos, (uint32_t) written);
            pos += 2 + (size_t) written;
        }
        if ((flags & 0x02) != 0) {
            room = outCap - 4;
            if (room < pos + 2) {
                return GDX_COURSE_ERR_FULL;
            }
            written = gdx_course_title_encode(entry->title, entry->bindCrc, out + pos + 2, room - pos - 2);
            if (written < 0) {
                return written;
            }
            put_u16le(out + pos, (uint32_t) written);
            pos += 2 + (size_t) written;
        }
    }
    if (pos + 4 > outCap) {
        return GDX_COURSE_ERR_FULL;
    }

    out[0] = 'G';
    out[1] = 'X';
    out[2] = 'S';
    out[3] = '1';
    put_u16le(out + 4, GDX_COURSE_STATE_VERSION);
    put_u16le(out + 6, (uint32_t) count);
    put_u32le(out + 8, (uint32_t) (pos + 4));
    put_u32le(out + pos, gdx_course_registry_crc32(out, pos));
    return (int) (pos + 4);
}

/* Builds the entry list of the in-memory store: every course with records, with its title when it has one,
 * then every title whose course has no records. */
static int store_entries(void) {
    int n = 0;
    int i;

    for (i = 0; i < GDX_COURSE_STORE_SLOTS; i++) {
        GdxCourseStateEntry* entry;
        int titleSlot;

        if (!sStore[i].used || n >= GDX_COURSE_STATE_MAX_ENTRIES) {
            continue;
        }
        entry = &sStateEntries[n];
        memset(entry, 0, sizeof(*entry));
        snprintf(entry->courseId, sizeof(entry->courseId), "%s", sStore[i].courseId);
        entry->bindCrc = gdx_course_registry_crc32((const unsigned char*) sStore[i].courseId,
                                                   strlen(sStore[i].courseId));
        entry->records = sStore[i].records;
        entry->recordCount = sStore[i].count;
        titleSlot = title_find_slot(sStore[i].courseId);
        if (titleSlot >= 0) {
            entry->hasTitle = 1;
            snprintf(entry->title, sizeof(entry->title), "%s", sTitles[titleSlot].title);
        }
        n++;
    }
    for (i = 0; i < GDX_COURSE_TITLE_SLOTS; i++) {
        GdxCourseStateEntry* entry;

        if (!sTitles[i].used || store_find_slot(sTitles[i].courseId) >= 0 || n >= GDX_COURSE_STATE_MAX_ENTRIES) {
            continue;
        }
        entry = &sStateEntries[n];
        memset(entry, 0, sizeof(*entry));
        snprintf(entry->courseId, sizeof(entry->courseId), "%s", sTitles[i].courseId);
        entry->bindCrc = gdx_course_registry_crc32((const unsigned char*) sTitles[i].courseId,
                                                   strlen(sTitles[i].courseId));
        entry->hasTitle = 1;
        snprintf(entry->title, sizeof(entry->title), "%s", sTitles[i].title);
        n++;
    }
    return n;
}

int gdx_course_store_encode_file(unsigned char* out, size_t outCap) {
    return gdx_course_state_encode(sStateEntries, store_entries(), out, outCap);
}

typedef struct GdxCourseRestoreCtx {
    int commit;  /* 0 = verify and count, 1 = write into the store */
    int records; /* courses with records (verify pass) */
    int titles;  /* courses with titles (verify pass) */
} GdxCourseRestoreCtx;

static int store_restore_visit(void* ctxPtr, const GdxCourseStateEntry* entry) {
    GdxCourseRestoreCtx* ctx = (GdxCourseRestoreCtx*) ctxPtr;
    int check;

    if (ctx->commit) {
        if (entry->recordCount > 0) {
            check = gdx_course_ext_restore(entry->courseId, entry->records, entry->recordCount);
            if (check != GDX_COURSE_OK) {
                return check;
            }
        }
        if (entry->hasTitle) {
            check = gdx_course_title_set(entry->courseId, entry->title);
            if (check != GDX_COURSE_OK) {
                return check;
            }
        }
        return GDX_COURSE_OK;
    }
    /* A store entry must be bound to its own course id, so a block cannot be moved to another course. */
    if (entry->bindCrc != gdx_course_registry_crc32((const unsigned char*) entry->courseId, strlen(entry->courseId))) {
        return GDX_COURSE_STATE_ERR_ENTRY;
    }
    if (entry->recordCount > 0) {
        ctx->records++;
    }
    if (entry->hasTitle) {
        ctx->titles++;
    }
    return GDX_COURSE_OK;
}

int gdx_course_store_restore_file(const unsigned char* data, size_t length) {
    GdxCourseRestoreCtx ctx;
    int check;

    memset(&ctx, 0, sizeof(ctx));
    check = gdx_course_state_decode(data, length, store_restore_visit, &ctx);
    if (check != GDX_COURSE_OK) {
        return check;
    }
    if (ctx.records > GDX_COURSE_STORE_SLOTS || ctx.titles > GDX_COURSE_TITLE_SLOTS) {
        return GDX_COURSE_ERR_FULL;
    }
    gdx_course_store_clear();
    ctx.commit = 1;
    return gdx_course_state_decode(data, length, store_restore_visit, &ctx);
}

/* ---------------------------------------------------------------------------------
 * Pack manifest line parser (elements.txt).
 * ------------------------------------------------------------------------------- */

static int manifest_fail(char* reason, size_t reasonCap, int lineNumber, const char* what, int code) {
    if (reason != NULL && reasonCap > 0) {
        snprintf(reason, reasonCap, "line %d: %s", lineNumber, what);
    }
    return code;
}

static int ends_with_ci(const char* text, const char* suffix) {
    size_t textLen = strlen(text);
    size_t suffixLen = strlen(suffix);
    size_t i;

    if (textLen < suffixLen) {
        return 0;
    }
    for (i = 0; i < suffixLen; i++) {
        unsigned char c = (unsigned char) text[textLen - suffixLen + i];
        if (c >= 'A' && c <= 'Z') {
            c = (unsigned char) (c + 32);
        }
        if (c != (unsigned char) suffix[i]) {
            return 0;
        }
    }
    return 1;
}

/* A music path is relative to the pack folder, uses forward slashes, has no "..", and names an audio
 * file the streamer can open. The file itself is checked by the pack loader, not here. */
static int manifest_path_ok(const char* path) {
    if (path[0] == '/' || path[0] == '\\' || strchr(path, ':') != NULL || strchr(path, '\\') != NULL) {
        return 0;
    }
    if (strstr(path, "..") != NULL) {
        return 0;
    }
    return ends_with_ci(path, ".ogg") || ends_with_ci(path, ".flac") || ends_with_ci(path, ".wav");
}

int gdx_course_manifest_parse_line(const char* line, size_t lineLength, int lineNumber,
                                   GdxCourseManifestEntry* out, char* reason, size_t reasonCap) {
    char text[GDX_COURSE_MANIFEST_LINE_MAX + 1];
    char* field[5];
    GdxCourseKind kind = GDX_COURSE_KIND_VENUE;
    size_t len;
    size_t i;
    int fieldCount = 1;
    int value = 0;
    int digits;

    if (reason != NULL && reasonCap > 0) {
        reason[0] = '\0';
    }
    if (line == NULL || out == NULL) {
        return GDX_COURSE_MANIFEST_ERR_ARGS;
    }
    len = lineLength;
    while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == '\n')) {
        len--;
    }
    if (len > GDX_COURSE_MANIFEST_LINE_MAX) {
        return manifest_fail(reason, reasonCap, lineNumber, "line is longer than 256 bytes",
                             GDX_COURSE_MANIFEST_ERR_LONG);
    }
    for (i = 0; i < len; i++) {
        unsigned char c = (unsigned char) line[i];
        if (c < 0x20 || c > 0x7E) {
            return manifest_fail(reason, reasonCap, lineNumber, "line has a byte outside printable ASCII",
                                 GDX_COURSE_MANIFEST_ERR_TEXT);
        }
    }

    /* Blank and comment lines are skipped. */
    for (i = 0; i < len && line[i] == ' '; i++) {
    }
    if (i == len || line[i] == '#') {
        return GDX_COURSE_MANIFEST_SKIP;
    }

    memcpy(text, line, len);
    text[len] = '\0';
    field[0] = text;
    for (i = 0; i < len; i++) {
        if (text[i] == '|') {
            if (fieldCount == 5) {
                return manifest_fail(reason, reasonCap, lineNumber, "more than 5 fields separated by |",
                                     GDX_COURSE_MANIFEST_ERR_FIELDS);
            }
            text[i] = '\0';
            field[fieldCount++] = text + i + 1;
        }
    }
    if (fieldCount != 4 && fieldCount != 5) {
        return manifest_fail(reason, reasonCap, lineNumber, "expected 4 or 5 fields: kind|key|name|fallback[|path]",
                             GDX_COURSE_MANIFEST_ERR_FIELDS);
    }

    if (strcmp(field[0], "venue") == 0) {
        kind = GDX_COURSE_KIND_VENUE;
    } else if (strcmp(field[0], "skybox") == 0) {
        kind = GDX_COURSE_KIND_SKYBOX;
    } else if (strcmp(field[0], "bgm") == 0) {
        kind = GDX_COURSE_KIND_BGM;
    } else if (strcmp(field[0], "part") == 0) {
        kind = GDX_COURSE_KIND_PART_STYLE;
    } else if (strcmp(field[0], "effect") == 0) {
        kind = GDX_COURSE_KIND_TRACK_EFFECT;
    } else {
        return manifest_fail(reason, reasonCap, lineNumber, "kind must be venue, skybox, bgm, part or effect",
                             GDX_COURSE_MANIFEST_ERR_KIND);
    }

    if (!gdx_course_key_is_valid(field[1])) {
        return manifest_fail(reason, reasonCap, lineNumber, "key must be 1..48 characters of [a-z0-9._-]",
                             GDX_COURSE_MANIFEST_ERR_KEY);
    }
    if (!display_name_valid(field[2])) {
        return manifest_fail(reason, reasonCap, lineNumber, "name must be 1..64 printable characters",
                             GDX_COURSE_MANIFEST_ERR_NAME);
    }

    digits = 0;
    for (i = 0; field[3][i] != '\0'; i++) {
        if (field[3][i] < '0' || field[3][i] > '9' || digits >= 3) {
            return manifest_fail(reason, reasonCap, lineNumber, "fallback must be a decimal stock id",
                                 GDX_COURSE_MANIFEST_ERR_FALLBACK);
        }
        value = value * 10 + (field[3][i] - '0');
        digits++;
    }
    if (digits == 0 || gdx_course_registry_find_stock(kind, value) == NULL) {
        return manifest_fail(reason, reasonCap, lineNumber, "fallback is not a stock id of this kind",
                             GDX_COURSE_MANIFEST_ERR_FALLBACK);
    }

    out->kind = kind;
    snprintf(out->key, sizeof(out->key), "%s", field[1]);
    snprintf(out->displayName, sizeof(out->displayName), "%s", field[2]);
    out->fallbackId = value;
    out->path[0] = '\0';
    if (fieldCount == 5) {
        if (kind != GDX_COURSE_KIND_BGM) {
            return manifest_fail(reason, reasonCap, lineNumber, "a path is only allowed on bgm lines",
                                 GDX_COURSE_MANIFEST_ERR_PATH);
        }
        if (field[4][0] == '\0' || strlen(field[4]) >= GDX_COURSE_PATH_MAX) {
            return manifest_fail(reason, reasonCap, lineNumber, "path is empty or too long",
                                 GDX_COURSE_MANIFEST_ERR_PATH);
        }
        if (!manifest_path_ok(field[4])) {
            return manifest_fail(reason, reasonCap, lineNumber,
                                 "path must be relative, use /, have no .., and end in .ogg, .flac or .wav",
                                 GDX_COURSE_MANIFEST_ERR_PATH);
        }
        snprintf(out->path, sizeof(out->path), "%s", field[4]);
    }
    return 1;
}

/* Plan slice 9, rename hook: the file of oldId was renamed to newId, so newId now holds its keys and
   title. Copies the records and the title of oldId to newId (replacing whatever newId held) and then
   clears oldId. A call whose oldId has nothing still clears newId, because the renamed file replaces
   it. Returns GDX_COURSE_OK, or a negative code when an id is invalid or the store has no room. On a
   store-full error nothing has moved. Callers flush the state file afterwards (gdx_course_state_flush). */
int gdx_course_store_rename(const char* oldId, const char* newId) {
    static GdxCourseExtRecord scratch[GDX_COURSE_EXT_MAX_RECORDS];
    static char title[GDX_COURSE_TITLE_MAX + 1];
    const GdxCourseExtRecord* records;
    int count = 0;
    int hasTitle;
    int rc;

    if (!course_id_valid(oldId) || !course_id_valid(newId)) {
        return GDX_COURSE_ERR_ARGS;
    }
    if (strcmp(oldId, newId) == 0) {
        return GDX_COURSE_OK;
    }
    records = gdx_course_ext_records(oldId, &count);
    if (records != NULL && count > 0) {
        memcpy(scratch, records, (size_t) count * sizeof(GdxCourseExtRecord));
    } else {
        count = 0;
    }
    hasTitle = gdx_course_title_get(oldId, title, sizeof(title));

    rc = gdx_course_ext_restore(newId, count > 0 ? scratch : NULL, count);
    if (rc != GDX_COURSE_OK) {
        return rc;
    }
    if (hasTitle) {
        rc = gdx_course_title_set(newId, title);
        if (rc != GDX_COURSE_OK) {
            return rc;
        }
    } else {
        gdx_course_title_clear(newId);
    }
    gdx_course_ext_clear_course(oldId);
    gdx_course_title_clear(oldId);
    return GDX_COURSE_OK;
}
