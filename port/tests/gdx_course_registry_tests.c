/* port/tests/gdx_course_registry_tests.c -- unit tests for the Course Edit element registry and the
 * GXE1 sidecar codec. Standalone: links gdx_course_registry.c and nothing else.
 */

#include "../gdx_course_registry.h"

#include <stdio.h>
#include <string.h>

static int sFailures;

static void check(int condition, const char* label) {
    if (condition) {
        printf("PASS: %s\n", label);
    } else {
        fprintf(stderr, "FAIL: %s\n", label);
        sFailures++;
    }
}

/* Stock ID of a key, or -1 when the key is unknown. */
static int stock_id_of(GdxCourseKind kind, const char* key) {
    const GdxCourseElement* element = gdx_course_registry_find_key(kind, key);
    return element == NULL ? -1 : element->stockId;
}

/* ---------------------------------------------------------------------------------
 * Stock tables and keys.
 * ------------------------------------------------------------------------------- */

static void test_stock_tables(void) {
    int kind;
    int i;
    int bad = 0;

    check(gdx_course_registry_stock_count(GDX_COURSE_KIND_VENUE) == 11, "venue has 11 stock entries");
    check(gdx_course_registry_stock_count(GDX_COURSE_KIND_SKYBOX) == 8, "skybox has 8 stock entries");
    check(gdx_course_registry_stock_count(GDX_COURSE_KIND_BGM) == 14, "bgm has 14 stock entries (raw 0..13)");
    check(gdx_course_registry_stock_count(GDX_COURSE_KIND_PART_STYLE) == 9, "part style has 9 stock entries");
    check(gdx_course_registry_stock_count(GDX_COURSE_KIND_TRACK_EFFECT) == 6, "track effect has 6 stock entries");
    check(gdx_course_registry_stock_count((GdxCourseKind) 9) == 0, "an invalid kind has no stock entries");

    for (kind = 0; kind < GDX_COURSE_KIND_COUNT; kind++) {
        int count = gdx_course_registry_stock_count((GdxCourseKind) kind);
        for (i = 0; i < count; i++) {
            const GdxCourseElement* byIndex = gdx_course_registry_at((GdxCourseKind) kind, i);
            const GdxCourseElement* byId = gdx_course_registry_find_stock((GdxCourseKind) kind, i);
            const GdxCourseElement* byKey;
            if (byIndex == NULL || byIndex != byId || byIndex->isExtension || byIndex->stockId != i) {
                bad++;
                continue;
            }
            byKey = gdx_course_registry_find_key((GdxCourseKind) kind, byIndex->key);
            if (byKey != byIndex) {
                bad++;
            }
        }
        if (gdx_course_registry_at((GdxCourseKind) kind, count) != NULL) {
            bad++;
        }
    }
    check(bad == 0, "every stock index equals its stock ID and resolves back through its key");

    check(stock_id_of(GDX_COURSE_KIND_VENUE, "venue.ending") == 10, "venue.ending is stock ID 10");
    check(stock_id_of(GDX_COURSE_KIND_SKYBOX, "skybox.sky_blue") == 7, "skybox.sky_blue is stock ID 7");
    check(stock_id_of(GDX_COURSE_KIND_BGM, "bgm.new_04") == 13, "bgm.new_04 is raw BGM ID 13");
    check(stock_id_of(GDX_COURSE_KIND_PART_STYLE, "part.sign") == 8, "part.sign is stock ID 8");
    check(stock_id_of(GDX_COURSE_KIND_TRACK_EFFECT, "effect.lava") == 5, "effect.lava is stock ID 5");
    check(stock_id_of(GDX_COURSE_KIND_SKYBOX, "venue.ending") == -1, "a key only matches within its own kind");
    check(gdx_course_registry_find_stock(GDX_COURSE_KIND_VENUE, 11) == NULL, "find_stock rejects an ID past the table");
}

static void test_keys(void) {
    char longKey[GDX_COURSE_KEY_MAX + 2];

    check(gdx_course_key_is_valid("venue.mute_city") == 1, "a normal key is valid");
    check(gdx_course_key_is_valid("ext-1.a_b.c9") == 1, "digits, dots, dashes and underscores are valid");
    check(gdx_course_key_is_valid("Venue.Mute") == 0, "uppercase letters are rejected");
    check(gdx_course_key_is_valid("has space") == 0, "spaces are rejected");
    check(gdx_course_key_is_valid("") == 0, "an empty key is rejected");
    check(gdx_course_key_is_valid(NULL) == 0, "a NULL key is rejected");

    memset(longKey, 'a', GDX_COURSE_KEY_MAX);
    longKey[GDX_COURSE_KEY_MAX] = '\0';
    check(gdx_course_key_is_valid(longKey) == 1, "a key of exactly GDX_COURSE_KEY_MAX characters is valid");
    memset(longKey, 'a', GDX_COURSE_KEY_MAX + 1);
    longKey[GDX_COURSE_KEY_MAX + 1] = '\0';
    check(gdx_course_key_is_valid(longKey) == 0, "a key one character too long is rejected");
}

/* ---------------------------------------------------------------------------------
 * Resolution and runtime registration.
 * ------------------------------------------------------------------------------- */

static void test_resolve(void) {
    int missing = -1;
    int id;

    id = gdx_course_registry_resolve(GDX_COURSE_KIND_VENUE, "venue.sector", &missing);
    check(id == 6 && missing == 0, "resolve: a stock key returns its own ID and is not missing");

    id = gdx_course_registry_resolve(GDX_COURSE_KIND_VENUE, "venue.unknown", &missing);
    check(id == GDX_COURSE_DEFAULT_STOCK_ID && missing == 1,
          "resolve: an unknown key returns the documented default and sets missing");

    id = gdx_course_registry_resolve(GDX_COURSE_KIND_VENUE, "venue.sector", NULL);
    check(id == 6, "resolve: outMissing may be NULL");
}

static void test_register(void) {
    char key[GDX_COURSE_KEY_MAX + 2];
    char name[GDX_COURSE_DISPLAY_MAX + 2];
    const GdxCourseElement* element;
    int missing = -1;
    int bad = 0;
    int i;

    gdx_course_registry_clear_extensions();
    check(gdx_course_registry_register(GDX_COURSE_KIND_VENUE, "ext.venue.mount_mega", "Mount Mega", 3) ==
              GDX_COURSE_OK,
          "register: a valid extension is accepted");
    element = gdx_course_registry_find_key(GDX_COURSE_KIND_VENUE, "ext.venue.mount_mega");
    check(element != NULL && element->isExtension == 1 && element->stockId == 3 &&
              strcmp(element->displayName, "Mount Mega") == 0,
          "register: find returns the extension with its fallback and display name");
    check(gdx_course_registry_resolve(GDX_COURSE_KIND_VENUE, "ext.venue.mount_mega", &missing) == 3 &&
              missing == 0,
          "resolve: an extension key returns its fallback");
    check(gdx_course_registry_count(GDX_COURSE_KIND_VENUE) == 12, "count includes the extension");
    check(gdx_course_registry_at(GDX_COURSE_KIND_VENUE, 11) == element,
          "iteration reaches the extension after the stock entries");
    check(gdx_course_registry_find_stock(GDX_COURSE_KIND_VENUE, 3) != NULL &&
              gdx_course_registry_find_stock(GDX_COURSE_KIND_VENUE, 3)->isExtension == 0,
          "find_stock never returns an extension entry");

    check(gdx_course_registry_register(GDX_COURSE_KIND_VENUE, "ext.venue.mount_mega", "Again", 3) ==
              GDX_COURSE_ERR_DUPLICATE,
          "register: a repeated extension key is rejected");
    check(gdx_course_registry_register(GDX_COURSE_KIND_VENUE, "venue.sector", "Sector", 6) ==
              GDX_COURSE_ERR_DUPLICATE,
          "register: a stock key cannot be registered again");
    check(gdx_course_registry_register(GDX_COURSE_KIND_VENUE, "ext.fallback_hi", "X", 11) ==
              GDX_COURSE_ERR_FALLBACK,
          "register: a fallback past the stock table is rejected");
    check(gdx_course_registry_register(GDX_COURSE_KIND_VENUE, "ext.fallback_lo", "X", -1) ==
              GDX_COURSE_ERR_FALLBACK,
          "register: a negative fallback is rejected");
    check(gdx_course_registry_register(GDX_COURSE_KIND_VENUE, "Upper.Case", "X", 0) == GDX_COURSE_ERR_KEY,
          "register: an invalid key is rejected");
    check(gdx_course_registry_register(GDX_COURSE_KIND_VENUE, "", "X", 0) == GDX_COURSE_ERR_KEY,
          "register: an empty key is rejected");
    check(gdx_course_registry_register(GDX_COURSE_KIND_VENUE, "ext.empty_name", "", 0) == GDX_COURSE_ERR_NAME,
          "register: an empty display name is rejected");

    memset(name, 'N', GDX_COURSE_DISPLAY_MAX + 1);
    name[GDX_COURSE_DISPLAY_MAX + 1] = '\0';
    check(gdx_course_registry_register(GDX_COURSE_KIND_VENUE, "ext.long_name", name, 0) == GDX_COURSE_ERR_NAME,
          "register: a display name one character too long is rejected");
    check(gdx_course_registry_register((GdxCourseKind) 9, "ext.kind", "X", 0) == GDX_COURSE_ERR_KIND,
          "register: an invalid kind is rejected");
    check(gdx_course_registry_register(GDX_COURSE_KIND_VENUE, NULL, "X", 0) == GDX_COURSE_ERR_ARGS,
          "register: a NULL key is an argument error");

    gdx_course_registry_clear_extensions();
    for (i = 0; i < GDX_COURSE_EXT_PER_KIND; i++) {
        snprintf(key, sizeof(key), "ext.skybox.k%02d", i);
        if (gdx_course_registry_register(GDX_COURSE_KIND_SKYBOX, key, "Test", 0) != GDX_COURSE_OK) {
            bad++;
        }
    }
    check(bad == 0, "capacity: every extension slot of one kind is accepted");
    check(gdx_course_registry_register(GDX_COURSE_KIND_SKYBOX, "ext.skybox.overflow", "Test", 0) ==
              GDX_COURSE_ERR_FULL,
          "capacity: the registration past the per-kind limit is refused");
    check(gdx_course_registry_count(GDX_COURSE_KIND_SKYBOX) == 8 + GDX_COURSE_EXT_PER_KIND,
          "capacity: count is stock plus extensions");
    check(gdx_course_registry_count(GDX_COURSE_KIND_VENUE) == 11, "capacity: other kinds are unaffected");
    gdx_course_registry_clear_extensions();
}

/* ---------------------------------------------------------------------------------
 * Per-course store.
 * ------------------------------------------------------------------------------- */

static void test_store(void) {
    const GdxCourseExtRecord* records;
    char courseId[32];
    int count = -1;
    int bad = 0;
    int i;

    gdx_course_registry_clear_extensions();
    gdx_course_registry_register(GDX_COURSE_KIND_VENUE, "ext.venue.mount_mega", "Mount Mega", 3);
    gdx_course_registry_register(GDX_COURSE_KIND_PART_STYLE, "ext.part.ramp", "Ramp", 0);
    gdx_course_registry_register(GDX_COURSE_KIND_TRACK_EFFECT, "ext.effect.boost", "Boost", 3);

    check(gdx_course_ext_set("MYTRK.CRSD", GDX_COURSE_KIND_VENUE, GDX_COURSE_SCOPE_COURSE,
                             "ext.venue.mount_mega") == GDX_COURSE_OK,
          "store: set accepts a registered extension");
    check(gdx_course_ext_set("MYTRK.CRSD", GDX_COURSE_KIND_VENUE, GDX_COURSE_SCOPE_COURSE,
                             "ext.venue.mount_mega") == GDX_COURSE_OK &&
              gdx_course_ext_count("MYTRK.CRSD") == 1,
          "store: setting the same (kind, scope) again replaces instead of adding");
    check(gdx_course_ext_set("MYTRK.CRSD", GDX_COURSE_KIND_PART_STYLE, 17, "ext.part.ramp") == GDX_COURSE_OK,
          "store: a point-scoped record is a second record for the course");
    records = gdx_course_ext_records("MYTRK.CRSD", &count);
    check(count == 2 && records != NULL && records[0].kind == GDX_COURSE_KIND_VENUE &&
              records[0].scope == GDX_COURSE_SCOPE_COURSE && records[0].fallbackId == 3 &&
              strcmp(records[0].key, "ext.venue.mount_mega") == 0,
          "store: the record holds the canonical key and the entry fallback");

    check(gdx_course_ext_set("MYTRK.CRSD", GDX_COURSE_KIND_VENUE, GDX_COURSE_SCOPE_COURSE, "venue.sector") ==
              GDX_COURSE_ERR_NOT_EXTENSION,
          "store: a stock key is refused");
    check(gdx_course_ext_set("MYTRK.CRSD", GDX_COURSE_KIND_VENUE, GDX_COURSE_SCOPE_COURSE, "ext.nope") ==
              GDX_COURSE_ERR_UNKNOWN,
          "store: an unregistered key is refused");
    check(gdx_course_ext_set("MYTRK.CRSD", GDX_COURSE_KIND_VENUE, 64, "ext.venue.mount_mega") ==
              GDX_COURSE_ERR_SCOPE,
          "store: scope 64 is refused");
    check(gdx_course_ext_set("MYTRK.CRSD", GDX_COURSE_KIND_VENUE, -2, "ext.venue.mount_mega") ==
              GDX_COURSE_ERR_SCOPE,
          "store: a negative scope other than course-wide is refused");
    check(gdx_course_ext_set("MYTRK.CRSD", (GdxCourseKind) 7, 0, "ext.venue.mount_mega") == GDX_COURSE_ERR_KIND,
          "store: an invalid kind is refused");
    check(gdx_course_ext_set("", GDX_COURSE_KIND_VENUE, GDX_COURSE_SCOPE_COURSE, "ext.venue.mount_mega") ==
              GDX_COURSE_ERR_ARGS,
          "store: an empty course id is refused");

    check(gdx_course_ext_clear("MYTRK.CRSD", GDX_COURSE_KIND_PART_STYLE, 17) == GDX_COURSE_OK,
          "store: clear removes one (kind, scope)");
    check(gdx_course_ext_clear("MYTRK.CRSD", GDX_COURSE_KIND_PART_STYLE, 17) == GDX_COURSE_ERR_UNKNOWN,
          "store: clearing a missing record reports unknown");
    check(gdx_course_ext_count("MYTRK.CRSD") == 1, "store: one record remains after the clear");
    gdx_course_ext_clear_course("MYTRK.CRSD");
    check(gdx_course_ext_count("MYTRK.CRSD") == 0 && gdx_course_ext_records("MYTRK.CRSD", &count) == NULL,
          "store: clear_course drops every record of the course");

    for (i = 0; i < GDX_COURSE_STORE_SLOTS; i++) {
        snprintf(courseId, sizeof(courseId), "TRACK%02d.CRSE", i);
        if (gdx_course_ext_set(courseId, GDX_COURSE_KIND_VENUE, GDX_COURSE_SCOPE_COURSE, "ext.venue.mount_mega") !=
            GDX_COURSE_OK) {
            bad++;
        }
    }
    check(bad == 0, "store: one course per slot fits");
    check(gdx_course_ext_set("OVERFLOW.CRSE", GDX_COURSE_KIND_VENUE, GDX_COURSE_SCOPE_COURSE,
                             "ext.venue.mount_mega") == GDX_COURSE_ERR_FULL,
          "store: a course past the slot limit is refused");
    gdx_course_ext_clear_course("TRACK03.CRSE");
    check(gdx_course_ext_set("OVERFLOW.CRSE", GDX_COURSE_KIND_VENUE, GDX_COURSE_SCOPE_COURSE,
                             "ext.venue.mount_mega") == GDX_COURSE_OK,
          "store: a freed slot is reused");

    gdx_course_ext_clear_course("OVERFLOW.CRSE");
    gdx_course_ext_clear_course("TRACK00.CRSE");
    bad = 0;
    for (i = 0; i < GDX_COURSE_EXT_MAX_RECORDS - 1; i++) {
        int scope = i % GDX_COURSE_SCOPE_POINTS;
        GdxCourseKind kind = (i < GDX_COURSE_SCOPE_POINTS) ? GDX_COURSE_KIND_PART_STYLE : GDX_COURSE_KIND_TRACK_EFFECT;
        const char* key = (kind == GDX_COURSE_KIND_PART_STYLE) ? "ext.part.ramp" : "ext.effect.boost";
        if (gdx_course_ext_set("TRACK00.CRSE", kind, scope, key) != GDX_COURSE_OK) {
            bad++;
        }
    }
    check(bad == 0, "store: a course holds one record per point for two kinds");
    check(gdx_course_ext_count("TRACK00.CRSE") == GDX_COURSE_EXT_MAX_RECORDS - 1,
          "store: the per-course count matches the records set");
    check(gdx_course_ext_set("TRACK00.CRSE", GDX_COURSE_KIND_VENUE, GDX_COURSE_SCOPE_COURSE,
                             "ext.venue.mount_mega") == GDX_COURSE_OK,
          "store: the last record slot is still available");
    check(gdx_course_ext_set("TRACK00.CRSE", GDX_COURSE_KIND_BGM, GDX_COURSE_SCOPE_COURSE, "bgm.new_04") ==
              GDX_COURSE_ERR_NOT_EXTENSION,
          "store: a stock key does not consume a record slot");
    gdx_course_registry_clear_extensions();
}

/* ---------------------------------------------------------------------------------
 * GXE1 codec. Helpers build blocks byte by byte, mirroring the layout in the header.
 * ------------------------------------------------------------------------------- */

#define TEST_PAYLOAD_CRC 0x12345678u

/* Writes a valid two-record sample bound to TEST_PAYLOAD_CRC. Returns its length (57). */
static int build_sample(unsigned char* buf, size_t cap) {
    GdxCourseExtRecord recs[2];

    memset(recs, 0, sizeof(recs));
    recs[0].kind = GDX_COURSE_KIND_VENUE;
    recs[0].scope = GDX_COURSE_SCOPE_COURSE;
    recs[0].fallbackId = 3;
    snprintf(recs[0].key, sizeof(recs[0].key), "%s", "ext.venue.mount_mega");
    recs[1].kind = GDX_COURSE_KIND_PART_STYLE;
    recs[1].scope = 17;
    recs[1].fallbackId = 0;
    snprintf(recs[1].key, sizeof(recs[1].key), "%s", "ext.part.ramp");
    return gdx_course_ext_encode(recs, 2, TEST_PAYLOAD_CRC, buf, cap);
}

/* Recomputes the trailing CRC after a deliberate edit, so only the edited content is wrong. */
static void reseal(unsigned char* buf, size_t len) {
    uint32_t crc = gdx_course_registry_crc32(buf, len - 4);
    buf[len - 4] = (unsigned char) (crc & 0xFFu);
    buf[len - 3] = (unsigned char) ((crc >> 8) & 0xFFu);
    buf[len - 2] = (unsigned char) ((crc >> 16) & 0xFFu);
    buf[len - 1] = (unsigned char) ((crc >> 24) & 0xFFu);
}

/* Header (version 1, the test binding) + body + a valid CRC over all of it. */
static size_t craft(unsigned char* out, size_t cap, unsigned count, const unsigned char* body, size_t bodyLen) {
    size_t total = 12 + bodyLen + 4;

    if (total > cap) {
        return 0;
    }
    memcpy(out, "GXE1", 4);
    out[4] = 1;
    out[5] = 0;
    out[6] = (unsigned char) (count & 0xFFu);
    out[7] = (unsigned char) ((count >> 8) & 0xFFu);
    out[8] = (unsigned char) (TEST_PAYLOAD_CRC & 0xFFu);
    out[9] = (unsigned char) ((TEST_PAYLOAD_CRC >> 8) & 0xFFu);
    out[10] = (unsigned char) ((TEST_PAYLOAD_CRC >> 16) & 0xFFu);
    out[11] = (unsigned char) ((TEST_PAYLOAD_CRC >> 24) & 0xFFu);
    if (bodyLen > 0) {
        memcpy(out + 12, body, bodyLen);
    }
    reseal(out, total);
    return total;
}

static void test_codec_round_trip(void) {
    unsigned char buf[GDX_COURSE_EXT_MAX_BYTES];
    GdxCourseExtBlock block;
    int len;

    len = build_sample(buf, sizeof(buf));
    check(len == 57, "codec: two records encode to 57 bytes");
    check(gdx_course_ext_decode(buf, (size_t) len, TEST_PAYLOAD_CRC, &block) == GDX_COURSE_OK && block.count == 2,
          "codec: the round trip decodes both records");
    check(block.records[0].kind == GDX_COURSE_KIND_VENUE && block.records[0].scope == GDX_COURSE_SCOPE_COURSE &&
              block.records[0].fallbackId == 3 && strcmp(block.records[0].key, "ext.venue.mount_mega") == 0,
          "codec: the first decoded record matches the encoded one");
    check(block.records[1].kind == GDX_COURSE_KIND_PART_STYLE && block.records[1].scope == 17 &&
              block.records[1].fallbackId == 0 && strcmp(block.records[1].key, "ext.part.ramp") == 0,
          "codec: the second decoded record matches the encoded one");

    len = gdx_course_ext_encode(NULL, 0, 0xABCDu, buf, sizeof(buf));
    check(len == 16, "codec: an empty block is 16 bytes");
    check(gdx_course_ext_decode(buf, (size_t) len, 0xABCDu, &block) == GDX_COURSE_OK && block.count == 0,
          "codec: an empty block is accepted and decodes to zero records");

    check(gdx_course_registry_crc32((const unsigned char*) "123456789", 9) == 0xCBF43926u,
          "crc32: the check value for 123456789 is 0xCBF43926");
}

static void test_codec_damage(void) {
    unsigned char buf[GDX_COURSE_EXT_MAX_BYTES];
    unsigned char tmp[GDX_COURSE_EXT_MAX_BYTES];
    GdxCourseExtBlock block;
    int len;
    int bad = 0;
    int i;
    int b;
    size_t n;

    len = build_sample(buf, sizeof(buf));
    for (n = 0; n < (size_t) len; n++) {
        if (gdx_course_ext_decode(buf, n, TEST_PAYLOAD_CRC, &block) == GDX_COURSE_OK) {
            bad++;
        }
    }
    check(bad == 0, "truncation: every shorter prefix of the block is rejected");
    check(gdx_course_ext_decode(buf, 15, TEST_PAYLOAD_CRC, &block) == GDX_COURSE_EXT_ERR_TRUNCATED,
          "truncation: fewer than 16 bytes reports TRUNCATED");

    bad = 0;
    for (i = 0; i < len; i++) {
        for (b = 0; b < 8; b++) {
            memcpy(tmp, buf, (size_t) len);
            tmp[i] = (unsigned char) (tmp[i] ^ (1u << b));
            if (gdx_course_ext_decode(tmp, (size_t) len, TEST_PAYLOAD_CRC, &block) == GDX_COURSE_OK) {
                bad++;
            }
        }
    }
    check(bad == 0, "bit flip: every single-bit error anywhere in the block is rejected");

    check(gdx_course_ext_decode(buf, (size_t) len, TEST_PAYLOAD_CRC + 1u, &block) == GDX_COURSE_EXT_ERR_BINDING,
          "binding: a block bound to another payload CRC-32 is rejected");

    memcpy(tmp, buf, (size_t) len);
    tmp[0] = 'X';
    reseal(tmp, (size_t) len);
    check(gdx_course_ext_decode(tmp, (size_t) len, TEST_PAYLOAD_CRC, &block) == GDX_COURSE_EXT_ERR_MAGIC,
          "malformed: a wrong magic with a valid CRC is rejected as MAGIC");

    memcpy(tmp, buf, (size_t) len);
    tmp[4] = 2;
    reseal(tmp, (size_t) len);
    check(gdx_course_ext_decode(tmp, (size_t) len, TEST_PAYLOAD_CRC, &block) == GDX_COURSE_EXT_ERR_VERSION,
          "malformed: a future version with a valid CRC is rejected as VERSION");
}

static void expect_crafted(const char* label, unsigned count, const unsigned char* body, size_t bodyLen,
                           int expected) {
    unsigned char buf[160];
    GdxCourseExtBlock block;
    size_t len = craft(buf, sizeof(buf), count, body, bodyLen);

    check(len != 0 && gdx_course_ext_decode(buf, len, TEST_PAYLOAD_CRC, &block) == expected, label);
}

static void test_codec_malformed(void) {
    static const unsigned char kGood[] = { 0, 255, 3, 3, 'e', 'x', 't' };
    static const unsigned char kBadKind[] = { 9, 255, 3, 3, 'e', 'x', 't' };
    static const unsigned char kBadScope[] = { 0, 64, 3, 3, 'e', 'x', 't' };
    static const unsigned char kBadFallback[] = { 0, 255, 11, 3, 'e', 'x', 't' };
    static const unsigned char kEmptyKey[] = { 0, 255, 3, 0 };
    static const unsigned char kUpperKey[] = { 0, 255, 3, 3, 'E', 'x', 't' };
    static const unsigned char kNulKey[] = { 0, 255, 3, 3, 'e', 0, 't' };
    static const unsigned char kTwiceSameIdentity[] = { 0, 255, 3, 3, 'e', 'x', 't', 0, 255, 3, 3, 'e', 'x', 't' };
    static const unsigned char kTrailingByte[] = { 0, 255, 3, 3, 'e', 'x', 't', 0x00 };
    unsigned char longKeyBody[4 + GDX_COURSE_KEY_MAX + 1];

    expect_crafted("malformed: a valid crafted block is accepted (crafting self-check)", 1, kGood, sizeof(kGood),
                   GDX_COURSE_OK);
    expect_crafted("malformed: a kind outside the five kinds is rejected", 1, kBadKind, sizeof(kBadKind),
                   GDX_COURSE_EXT_ERR_RECORD);
    expect_crafted("malformed: a point index of 64 is rejected", 1, kBadScope, sizeof(kBadScope),
                   GDX_COURSE_EXT_ERR_RECORD);
    expect_crafted("malformed: a venue fallback past the stock table is rejected", 1, kBadFallback,
                   sizeof(kBadFallback), GDX_COURSE_EXT_ERR_RECORD);
    expect_crafted("malformed: a zero-length key is rejected", 1, kEmptyKey, sizeof(kEmptyKey),
                   GDX_COURSE_EXT_ERR_RECORD);
    expect_crafted("malformed: an uppercase key byte is rejected", 1, kUpperKey, sizeof(kUpperKey),
                   GDX_COURSE_EXT_ERR_RECORD);
    expect_crafted("malformed: an embedded NUL in the key is rejected", 1, kNulKey, sizeof(kNulKey),
                   GDX_COURSE_EXT_ERR_RECORD);
    expect_crafted("malformed: a repeated (kind, scope) pair is rejected", 2, kTwiceSameIdentity,
                   sizeof(kTwiceSameIdentity), GDX_COURSE_EXT_ERR_RECORD);
    expect_crafted("malformed: a count larger than the records present is rejected", 2, kGood, sizeof(kGood),
                   GDX_COURSE_EXT_ERR_LAYOUT);
    expect_crafted("malformed: a stray byte after the last record is rejected", 1, kTrailingByte,
                   sizeof(kTrailingByte), GDX_COURSE_EXT_ERR_LAYOUT);
    expect_crafted("malformed: a count above the record limit is rejected", GDX_COURSE_EXT_MAX_RECORDS + 1, kGood,
                   sizeof(kGood), GDX_COURSE_EXT_ERR_COUNT);

    memset(longKeyBody, 0, sizeof(longKeyBody));
    longKeyBody[0] = 0;
    longKeyBody[1] = 255;
    longKeyBody[2] = 3;
    longKeyBody[3] = GDX_COURSE_KEY_MAX + 1;
    memset(longKeyBody + 4, 'a', GDX_COURSE_KEY_MAX + 1);
    expect_crafted("malformed: a key one character past the limit is rejected", 1, longKeyBody,
                   sizeof(longKeyBody), GDX_COURSE_EXT_ERR_RECORD);
}

/* ---------------------------------------------------------------------------------
 * File-backed music entries (7A custom soundtracks).
 * ------------------------------------------------------------------------------- */

static void test_music_entries(void) {
    char path[GDX_COURSE_PATH_MAX + 1];
    char key[GDX_COURSE_KEY_MAX + 8];
    char label[GDX_COURSE_DISPLAY_MAX + 8];
    char source[64];
    const GdxCourseElement* element;
    int missing = -1;

    /* Registration with a source path. */
    snprintf(source, sizeof(source), "%s", "music/custom/ocean_theme.ogg");
    check(gdx_course_registry_register_file(GDX_COURSE_KIND_BGM, "bgm.custom.ocean_theme", "OCEAN THEME", 0,
                                            source) == GDX_COURSE_OK,
          "register_file accepts a BGM extension with a source path");
    source[0] = 'X';
    element = gdx_course_registry_find_key(GDX_COURSE_KIND_BGM, "bgm.custom.ocean_theme");
    check(element != NULL && element->isExtension == 1 && element->stockId == 0,
          "a file-backed BGM entry is an extension with its fallback");
    check(element != NULL && element->sourcePath != NULL &&
              strcmp(element->sourcePath, "music/custom/ocean_theme.ogg") == 0,
          "the source path is copied, so the caller's buffer may change");
    check(gdx_course_registry_resolve(GDX_COURSE_KIND_BGM, "bgm.custom.ocean_theme", &missing) == 0 && missing == 0,
          "a file-backed BGM key resolves to its fallback and is not missing");
    check(gdx_course_registry_find_stock(GDX_COURSE_KIND_BGM, 0) != NULL &&
              gdx_course_registry_find_stock(GDX_COURSE_KIND_BGM, 0)->sourcePath == NULL,
          "stock BGM entries have no source path");
    check(gdx_course_registry_register(GDX_COURSE_KIND_BGM, "bgm.custom.plain", "PLAIN", 1) == GDX_COURSE_OK &&
              gdx_course_registry_find_key(GDX_COURSE_KIND_BGM, "bgm.custom.plain")->sourcePath == NULL,
          "a plain register leaves the source path NULL");

    /* Path and argument rejection. */
    check(gdx_course_registry_register_file(GDX_COURSE_KIND_BGM, "bgm.custom.nopath", "X", 0, NULL) ==
              GDX_COURSE_ERR_ARGS,
          "register_file rejects a NULL path");
    check(gdx_course_registry_register_file(GDX_COURSE_KIND_BGM, "bgm.custom.emptypath", "X", 0, "") ==
              GDX_COURSE_ERR_PATH,
          "register_file rejects an empty path");
    memset(path, 'p', GDX_COURSE_PATH_MAX - 1);
    path[GDX_COURSE_PATH_MAX - 1] = '\0';
    check(gdx_course_registry_register_file(GDX_COURSE_KIND_BGM, "bgm.custom.pathmax", "X", 0, path) ==
              GDX_COURSE_OK,
          "register_file accepts a path of GDX_COURSE_PATH_MAX - 1 characters");
    memset(path, 'p', GDX_COURSE_PATH_MAX);
    path[GDX_COURSE_PATH_MAX] = '\0';
    check(gdx_course_registry_register_file(GDX_COURSE_KIND_BGM, "bgm.custom.toolong", "X", 0, path) ==
              GDX_COURSE_ERR_PATH,
          "register_file rejects a path of GDX_COURSE_PATH_MAX characters");
    check(gdx_course_registry_register_file(GDX_COURSE_KIND_BGM, "bgm.mute_city", "X", 0, source) ==
              GDX_COURSE_ERR_DUPLICATE,
          "register_file rejects a key that is a stock key");
    check(gdx_course_registry_register_file(GDX_COURSE_KIND_BGM, "bgm.custom.ocean_theme", "X", 0, source) ==
              GDX_COURSE_ERR_DUPLICATE,
          "register_file rejects a repeated extension key");
    check(gdx_course_registry_register_file(GDX_COURSE_KIND_BGM, "bgm.custom.fb", "X", 99, source) ==
              GDX_COURSE_ERR_FALLBACK,
          "register_file rejects a fallback outside the stock table");

    /* Key from a file stem. */
    check(gdx_course_registry_music_key("Ocean Theme", key, sizeof(key)) == 1 &&
              strcmp(key, "bgm.custom.ocean_theme") == 0,
          "music_key lowercases the stem and turns spaces into underscores");
    check(gdx_course_registry_music_key("My-Song.v2", key, sizeof(key)) == 1 &&
              strcmp(key, "bgm.custom.my-song.v2") == 0,
          "music_key keeps dashes and dots");
    check(gdx_course_registry_music_key("\xC3\xA9", key, sizeof(key)) == 1 &&
              strcmp(key, "bgm.custom.__") == 0 && gdx_course_key_is_valid(key),
          "music_key replaces non-ASCII bytes and yields a valid key");
    check(gdx_course_registry_music_key("", key, sizeof(key)) == 0, "music_key rejects an empty stem");
    check(gdx_course_registry_music_key("abc", key, 12) == 0,
          "music_key rejects an output too small for the prefix and one character");
    check(gdx_course_registry_music_key("abc", key, 13) == 1 && strcmp(key, "bgm.custom.a") == 0,
          "music_key keeps only what fits in the output");
    {
        char longStem[201];
        memset(longStem, 'x', 200);
        longStem[200] = '\0';
        check(gdx_course_registry_music_key(longStem, key, sizeof(key)) == 1 &&
                  strlen(key) == GDX_COURSE_KEY_MAX && gdx_course_key_is_valid(key),
              "music_key cuts a long stem so the key is exactly GDX_COURSE_KEY_MAX characters");
    }

    /* Fingerprint key for a different song under a used name. */
    check(gdx_course_registry_music_fingerprint_key("bgm.custom.ocean_theme", 0x1A2B3C4Du, key, sizeof(key)) == 1 &&
              strcmp(key, "bgm.custom.ocean_theme_1a2b3c4d") == 0,
          "music_fingerprint_key appends the content CRC in lowercase hex");
    {
        char longStem[201];
        char longKey[GDX_COURSE_KEY_MAX + 1];
        memset(longStem, 'y', 200);
        longStem[200] = '\0';
        check(gdx_course_registry_music_key(longStem, longKey, sizeof(longKey)) == 1 &&
                  gdx_course_registry_music_fingerprint_key(longKey, 0xDEADBEEFu, key, sizeof(key)) == 1 &&
                  strlen(key) == GDX_COURSE_KEY_MAX && gdx_course_key_is_valid(key) &&
                  strcmp(key + GDX_COURSE_KEY_MAX - 9, "_deadbeef") == 0,
              "music_fingerprint_key cuts a long body so the hex digits always fit");
    }
    {
        char stemKey[GDX_COURSE_KEY_MAX + 1];
        check(gdx_course_registry_music_fingerprint_key("bgm.custom.ocean_theme", 0x00000001u, key, sizeof(key)) == 1 &&
                  gdx_course_registry_music_key(key + 11, stemKey, sizeof(stemKey)) == 1 && strcmp(stemKey, key) == 0,
              "music_fingerprint_key yields a suffix that scans back to the same key");
    }
    check(gdx_course_registry_music_fingerprint_key("bgm.stock.x", 1u, key, sizeof(key)) == 0,
          "music_fingerprint_key rejects a key that is not a music key");
    check(gdx_course_registry_music_fingerprint_key("bgm.custom.", 1u, key, sizeof(key)) == 0,
          "music_fingerprint_key rejects a music key with no body");
    check(gdx_course_registry_music_fingerprint_key("bgm.custom.ab", 1u, key, 22) == 0,
          "music_fingerprint_key rejects an output too small for the whole key");

    /* Menu label from a file stem. */
    check(gdx_course_registry_music_display("ocean_theme", label, sizeof(label)) == 1 &&
              strcmp(label, "OCEAN THEME") == 0,
          "music_display uppercases and turns separators into single spaces");
    check(gdx_course_registry_music_display("  my--song!! ", label, sizeof(label)) == 1 &&
              strcmp(label, "MY--SONG") == 0,
          "music_display trims the ends and keeps font-safe punctuation");
    check(gdx_course_registry_music_display("\xC3\xA9\xC3\xA8", label, sizeof(label)) == 1 &&
              strcmp(label, "CUSTOM") == 0,
          "music_display falls back to CUSTOM when no character is drawable");
    {
        char longStem[101];
        memset(longStem, 'A', 100);
        longStem[100] = '\0';
        check(gdx_course_registry_music_display(longStem, label, sizeof(label)) == 1 &&
                  strlen(label) == GDX_COURSE_DISPLAY_MAX,
              "music_display keeps at most GDX_COURSE_DISPLAY_MAX characters");
    }
    check(gdx_course_registry_music_display("ABCDEF", label, 4) == 1 && strcmp(label, "ABC") == 0,
          "music_display truncates to the output size");
    check(gdx_course_registry_music_display("a b", label, 3) == 1 && strcmp(label, "A") == 0,
          "music_display does not leave a trailing space when the next word does not fit");
    check(gdx_course_registry_music_display("abc", label, 1) == 0, "music_display rejects a one byte output");
    check(gdx_course_registry_music_display(NULL, label, sizeof(label)) == 0,
          "music_display rejects a NULL stem");
}

static void test_short_name(void) {
    static const char* const taken[] = { "GROUND Z", "GROUND~1", "GROUND~2", "GROUND~3", "GROUND~4",
                                         "GROUND~5", "GROUND~6", "GROUND~7", "GROUND~8", "GROUND~9" };
    static const char* const withNull[] = { NULL, "GROUND Z" };
    char out[GDX_COURSE_SHORT_NAME_MAX + 1];

    check(gdx_course_short_name("GROUND ZERO EXTENDED", NULL, 0, out, sizeof(out)) == 1 &&
              strcmp(out, "GROUND Z") == 0,
          "short name is the first 8 characters of a long name");
    check(gdx_course_short_name("DUNE SHIFT", NULL, 0, out, sizeof(out)) == 1 && strcmp(out, "DUNE SHI") == 0,
          "short name of a 10 character name is its first 8 characters");
    check(gdx_course_short_name("GROUND ZERO EXTENDED", taken, 1, out, sizeof(out)) == 1 &&
              strcmp(out, "GROUND~1") == 0,
          "short name takes ~1 when the base is taken");
    check(gdx_course_short_name("GROUND ZERO EXTENDED", taken, 2, out, sizeof(out)) == 1 &&
              strcmp(out, "GROUND~2") == 0,
          "short name takes ~2 when ~1 is also taken");
    check(gdx_course_short_name("GROUND ZERO EXTENDED", taken, 10, out, sizeof(out)) == 1 &&
              strcmp(out, "GROUN~10") == 0,
          "short name with a two digit suffix replaces trailing characters to stay at 8");
    check(gdx_course_short_name("GROUND ZERO EXTENDED", withNull, 2, out, sizeof(out)) == 1 &&
              strcmp(out, "GROUND~1") == 0,
          "short name skips NULL entries in the taken list");
    check(gdx_course_short_name("A/B.C:DEFGHIJ", NULL, 0, out, sizeof(out)) == 1 &&
              strcmp(out, "ABCDEFGH") == 0,
          "short name drops the characters MFS refuses");
    check(gdx_course_short_name("", NULL, 0, out, sizeof(out)) == 0, "short name rejects an empty name");
    check(gdx_course_short_name("ABCDEFGHIJKLMNOPQRSTU", NULL, 0, out, sizeof(out)) == 0,
          "short name rejects a 21 character name");
    check(gdx_course_short_name("ABCDEFGHIJKLMNOPQRST", NULL, 0, out, sizeof(out)) == 1 &&
              strcmp(out, "ABCDEFGH") == 0,
          "short name accepts a 20 character name");
    check(gdx_course_short_name("/./:", NULL, 0, out, sizeof(out)) == 0,
          "short name rejects a name with no accepted character");
    check(gdx_course_short_name("ABCDEFGH", NULL, 0, out, 8) == 0, "short name rejects an output of 8 bytes");
    check(gdx_course_short_name(NULL, NULL, 0, out, sizeof(out)) == 0, "short name rejects a NULL name");
    check(gdx_course_short_name("ABC", taken, -1, out, sizeof(out)) == 0,
          "short name rejects a negative taken count");
}

static void test_title_store(void) {
    char buf[GDX_COURSE_TITLE_MAX + 1];
    char id[GDX_COURSE_FILE_ID_MAX + 1];
    char courseId[32];
    int i;

    check(gdx_course_title_set("GROUND~1.CRSD", "GROUND ZERO EXTENDED") == GDX_COURSE_OK,
          "title set accepts a 20 character title");
    check(gdx_course_title_get("GROUND~1.CRSD", buf, sizeof(buf)) == 1 &&
              strcmp(buf, "GROUND ZERO EXTENDED") == 0,
          "title get returns the stored title");
    check(gdx_course_title_find_course("GROUND ZERO EXTENDED", id, sizeof(id)) == 1 &&
              strcmp(id, "GROUND~1.CRSD") == 0,
          "title find_course returns the course for a title");
    check(gdx_course_title_set("GROUND~1.CRSD", "GROUND ZERO X") == GDX_COURSE_OK &&
              gdx_course_title_get("GROUND~1.CRSD", buf, sizeof(buf)) == 1 && strcmp(buf, "GROUND ZERO X") == 0,
          "title set replaces an existing title");
    check(gdx_course_title_find_course("GROUND ZERO EXTENDED", id, sizeof(id)) == 0,
          "title find_course no longer matches a replaced title");
    check(gdx_course_title_set("GROUND~1.CRSD", "ABCDEFGHIJKLMNOPQRSTU") == GDX_COURSE_ERR_NAME,
          "title set rejects a 21 character title");
    check(gdx_course_title_set("GROUND~1.CRSD", "") == GDX_COURSE_ERR_NAME, "title set rejects an empty title");
    check(gdx_course_title_set("GROUND~1.CRSD", "A\tB") == GDX_COURSE_ERR_NAME,
          "title set rejects a control character");
    check(gdx_course_title_set("GROUND~1.CRSD", NULL) == GDX_COURSE_ERR_ARGS, "title set rejects a NULL title");
    check(gdx_course_title_set("", "DUNE SHIFT") == GDX_COURSE_ERR_ARGS, "title set rejects an empty course id");
    check(gdx_course_title_get("GROUND~1.CRSD", buf, 3) == 0, "title get refuses an output that is too small");
    check(gdx_course_title_get("NOPE.CRSD", buf, sizeof(buf)) == 0, "title get misses an unknown course");

    gdx_course_title_clear("GROUND~1.CRSD");
    check(gdx_course_title_get("GROUND~1.CRSD", buf, sizeof(buf)) == 0, "title clear removes the title");

    for (i = 0; i < GDX_COURSE_TITLE_SLOTS; i++) {
        snprintf(courseId, sizeof(courseId), "T%03d.CRSD", i);
        if (gdx_course_title_set(courseId, "DUNE SHIFT") != GDX_COURSE_OK) {
            break;
        }
    }
    check(i == GDX_COURSE_TITLE_SLOTS, "title store holds GDX_COURSE_TITLE_SLOTS courses");
    check(gdx_course_title_set("OVER.CRSD", "DUNE SHIFT") == GDX_COURSE_ERR_FULL,
          "title store refuses a course past its capacity");
    for (i = 0; i < GDX_COURSE_TITLE_SLOTS; i++) {
        snprintf(courseId, sizeof(courseId), "T%03d.CRSD", i);
        gdx_course_title_clear(courseId);
    }
    check(gdx_course_title_set("OVER.CRSD", "DUNE SHIFT") == GDX_COURSE_OK,
          "title store accepts a course again after clears");
    gdx_course_title_clear("OVER.CRSD");
}

static void test_title_codec(void) {
    unsigned char block[GDX_COURSE_TITLE_MAX_BYTES];
    char out[GDX_COURSE_TITLE_MAX + 1];
    uint32_t crc;
    int n;

    n = gdx_course_title_encode("GROUND ZERO EXTENDED", 0x12345678u, block, sizeof(block));
    check(n == GDX_COURSE_TITLE_HEADER_SIZE + 20 + 4, "title encode returns the block length");
    check(n > 0 && gdx_course_title_decode(block, (size_t) n, 0x12345678u, out, sizeof(out)) == GDX_COURSE_OK &&
              strcmp(out, "GROUND ZERO EXTENDED") == 0,
          "title decode round-trips a 20 character title");
    check(n > 0 && gdx_course_title_decode(block, (size_t) n, 0x1u, out, sizeof(out)) ==
                       GDX_COURSE_TITLE_ERR_BINDING,
          "title decode rejects a block bound to another payload");

    block[0] = 'X';
    check(n > 0 && gdx_course_title_decode(block, (size_t) n, 0x12345678u, out, sizeof(out)) ==
                       GDX_COURSE_TITLE_ERR_MAGIC,
          "title decode rejects a bad magic");
    block[0] = 'G';
    block[4] = 2;
    check(n > 0 && gdx_course_title_decode(block, (size_t) n, 0x12345678u, out, sizeof(out)) ==
                       GDX_COURSE_TITLE_ERR_VERSION,
          "title decode rejects an unknown version");
    block[4] = 1;
    check(n > 0 && gdx_course_title_decode(block, 5, 0x12345678u, out, sizeof(out)) ==
                       GDX_COURSE_TITLE_ERR_TRUNCATED,
          "title decode rejects a truncated block");
    check(n > 0 && gdx_course_title_decode(block, (size_t) n - 1, 0x12345678u, out, sizeof(out)) ==
                       GDX_COURSE_TITLE_ERR_LENGTH,
          "title decode rejects a block whose length does not match titleLen");
    block[14] ^= 0x01;
    check(n > 0 && gdx_course_title_decode(block, (size_t) n, 0x12345678u, out, sizeof(out)) ==
                       GDX_COURSE_TITLE_ERR_CRC,
          "title decode rejects a damaged title");
    block[14] ^= 0x01;

    /* A non-printable byte with a correct CRC is refused as text, not as a CRC error. */
    block[12] = 0x7F;
    crc = gdx_course_registry_crc32(block, (size_t) n - 4);
    block[n - 4] = (unsigned char) (crc & 0xFF);
    block[n - 3] = (unsigned char) ((crc >> 8) & 0xFF);
    block[n - 2] = (unsigned char) ((crc >> 16) & 0xFF);
    block[n - 1] = (unsigned char) ((crc >> 24) & 0xFF);
    check(gdx_course_title_decode(block, (size_t) n, 0x12345678u, out, sizeof(out)) == GDX_COURSE_TITLE_ERR_TEXT,
          "title decode rejects a non-printable title byte");

    check(gdx_course_title_encode("", 1u, block, sizeof(block)) == GDX_COURSE_ERR_NAME,
          "title encode rejects an empty title");
    check(gdx_course_title_encode("ABCDEFGHIJKLMNOPQRSTU", 1u, block, sizeof(block)) == GDX_COURSE_ERR_NAME,
          "title encode rejects a 21 character title");
    check(gdx_course_title_encode("DUNE SHIFT", 1u, block, 10) == GDX_COURSE_ERR_FULL,
          "title encode refuses an output that is too small");
    check(gdx_course_title_encode(NULL, 1u, block, sizeof(block)) == GDX_COURSE_ERR_NAME,
          "title encode rejects a NULL title");
}

/* ---------------------------------------------------------------------------------
 * GXS1 state file, restore and revision counter (plan slice 9).
 * ------------------------------------------------------------------------------- */

static unsigned char sStateBuf[GDX_COURSE_STATE_MAX_BYTES];
static unsigned char sStateCopy[GDX_COURSE_STATE_MAX_BYTES];
static unsigned char sStateAgain[GDX_COURSE_STATE_MAX_BYTES];

static void put_le32(unsigned char* p, uint32_t v) {
    p[0] = (unsigned char) (v & 0xFFu);
    p[1] = (unsigned char) ((v >> 8) & 0xFFu);
    p[2] = (unsigned char) ((v >> 16) & 0xFFu);
    p[3] = (unsigned char) ((v >> 24) & 0xFFu);
}

/* Rewrites the length field and the trailing CRC, so the damage made by a test is the only fault. */
static void reseal_state(unsigned char* buf, size_t len) {
    put_le32(buf + 8, (uint32_t) len);
    put_le32(buf + len - 4, gdx_course_registry_crc32(buf, len - 4));
}

typedef struct StateCollect {
    int entries;
    int records;
    int titles;
    char firstKey[GDX_COURSE_KEY_MAX + 1];
    int firstScope;
} StateCollect;

static int state_collect_visit(void* ctx, const GdxCourseStateEntry* entry) {
    StateCollect* c = (StateCollect*) ctx;

    c->entries++;
    if (entry->recordCount > 0) {
        if (c->records == 0) {
            snprintf(c->firstKey, sizeof(c->firstKey), "%s", entry->records[0].key);
            c->firstScope = entry->records[0].scope;
        }
        c->records += entry->recordCount;
    }
    if (entry->hasTitle) {
        c->titles++;
    }
    return GDX_COURSE_OK;
}

/* Sets up a store with one course that has two records, plus a title on another course. */
static void build_registry_store(void) {
    gdx_course_registry_clear_extensions();
    check(gdx_course_registry_register(GDX_COURSE_KIND_BGM, "bgm.custom.dune", "DUNE SHIFT", 3) == GDX_COURSE_OK,
          "state setup registers a bgm extension");
    check(gdx_course_ext_set("MYTRACK.CRSD", GDX_COURSE_KIND_BGM, GDX_COURSE_SCOPE_COURSE, "bgm.custom.dune") == 0,
          "state setup sets a course-wide record");
    check(gdx_course_ext_set("MYTRACK.CRSD", GDX_COURSE_KIND_BGM, 4, "bgm.custom.dune") == 0,
          "state setup sets a point record");
    check(gdx_course_title_set("LONGTRACK.CRSD", "DUNE SHIFT ROAD") == GDX_COURSE_OK,
          "state setup sets a title on a second course");
}

static void test_state_round_trip(void) {
    GdxCourseExtRecord records[2];
    GdxCourseStateEntry entries[2];
    StateCollect collect;
    int n;
    int again;
    int count = 0;

    /* Unknown keys survive: the codec does not consult the registry, so a key that is not registered
     * in this session still decodes with its fields intact. */
    memset(records, 0, sizeof(records));
    memset(entries, 0, sizeof(entries));
    records[0].kind = GDX_COURSE_KIND_BGM;
    records[0].scope = GDX_COURSE_SCOPE_COURSE;
    records[0].fallbackId = 3;
    snprintf(records[0].key, sizeof(records[0].key), "%s", "bgm.custom.gone");
    records[1].kind = GDX_COURSE_KIND_VENUE;
    records[1].scope = 7;
    records[1].fallbackId = 0;
    snprintf(records[1].key, sizeof(records[1].key), "%s", "venue.gone");
    snprintf(entries[0].courseId, sizeof(entries[0].courseId), "%s", "UNKNOWN.CRSD");
    entries[0].bindCrc = gdx_course_registry_crc32((const unsigned char*) "UNKNOWN.CRSD", 12);
    entries[0].records = records;
    entries[0].recordCount = 2;
    snprintf(entries[1].courseId, sizeof(entries[1].courseId), "%s", "TITLED.CRSD");
    entries[1].bindCrc = gdx_course_registry_crc32((const unsigned char*) "TITLED.CRSD", 11);
    entries[1].hasTitle = 1;
    snprintf(entries[1].title, sizeof(entries[1].title), "%s", "SHORT ONE");

    n = gdx_course_state_encode(entries, 2, sStateBuf, sizeof(sStateBuf));
    check(n > GDX_COURSE_STATE_HEADER_SIZE + 4, "state encode writes a file for two entries");
    check(gdx_course_state_decode(sStateBuf, (size_t) n, NULL, NULL) == GDX_COURSE_OK,
          "state decode accepts a file it encoded (validate only)");
    memset(&collect, 0, sizeof(collect));
    check(gdx_course_state_decode(sStateBuf, (size_t) n, state_collect_visit, &collect) == GDX_COURSE_OK,
          "state decode walks the entries");
    check(collect.entries == 2 && collect.records == 2 && collect.titles == 1,
          "state decode reports 2 entries, 2 records and 1 title");
    check(strcmp(collect.firstKey, "bgm.custom.gone") == 0 && collect.firstScope == GDX_COURSE_SCOPE_COURSE,
          "state decode keeps an unknown key and its scope");

    /* Store round trip: encode, wipe the registry and the store, restore, and encode again. The bytes
     * must match, which proves nothing was dropped or reordered. */
    build_registry_store();
    n = gdx_course_store_encode_file(sStateBuf, sizeof(sStateBuf));
    check(n > 0, "store encodes to a state file");
    gdx_course_registry_clear_extensions();
    check(gdx_course_ext_count("MYTRACK.CRSD") == 0, "clearing the registry empties the store");
    check(gdx_course_store_restore_file(sStateBuf, (size_t) n) == GDX_COURSE_OK,
          "store restore accepts its own file after the registry is gone");
    check(gdx_course_ext_count("MYTRACK.CRSD") == 2, "restore brings back both course records");
    check(strcmp(gdx_course_ext_records("MYTRACK.CRSD", &count)[0].key, "bgm.custom.dune") == 0 && count == 2,
          "restored records keep their keys");
    {
        char title[GDX_COURSE_TITLE_MAX + 1];
        check(gdx_course_title_get("LONGTRACK.CRSD", title, sizeof(title)) == 1 &&
                  strcmp(title, "DUNE SHIFT ROAD") == 0,
              "restore brings back the title");
    }
    again = gdx_course_store_encode_file(sStateAgain, sizeof(sStateAgain));
    check(again == n && memcmp(sStateAgain, sStateBuf, (size_t) n) == 0,
          "re-encoding the restored store gives identical bytes");
    gdx_course_registry_clear_extensions();
}

static void test_state_damage(void) {
    int n;
    size_t i;
    GdxCourseStateEntry entry;
    unsigned char* flagsByte;
    size_t idLen;

    build_registry_store();
    n = gdx_course_store_encode_file(sStateBuf, sizeof(sStateBuf));
    check(n > 0, "damage tests start from a valid file");

    memcpy(sStateCopy, sStateBuf, (size_t) n);
    sStateCopy[n / 2] ^= 0x40u;
    check(gdx_course_state_decode(sStateCopy, (size_t) n, NULL, NULL) == GDX_COURSE_STATE_ERR_CRC,
          "a flipped byte is caught by the trailing CRC");
    check(gdx_course_store_restore_file(sStateCopy, (size_t) n) == GDX_COURSE_STATE_ERR_CRC,
          "restore refuses a corrupt file");
    check(gdx_course_ext_count("MYTRACK.CRSD") == 2, "a refused restore leaves the store unchanged");

    check(gdx_course_store_restore_file(sStateBuf, (size_t) n - 1) != GDX_COURSE_OK,
          "restore refuses a truncated file");
    check(gdx_course_store_restore_file(sStateBuf, 15) == GDX_COURSE_STATE_ERR_TRUNCATED,
          "restore refuses a file shorter than the header");

    memcpy(sStateCopy, sStateBuf, (size_t) n);
    sStateCopy[0] = 'X';
    reseal_state(sStateCopy, (size_t) n);
    check(gdx_course_state_decode(sStateCopy, (size_t) n, NULL, NULL) == GDX_COURSE_STATE_ERR_MAGIC,
          "a wrong magic is rejected");

    memcpy(sStateCopy, sStateBuf, (size_t) n);
    sStateCopy[4] = 2;
    reseal_state(sStateCopy, (size_t) n);
    check(gdx_course_state_decode(sStateCopy, (size_t) n, NULL, NULL) == GDX_COURSE_STATE_ERR_VERSION,
          "a wrong version is rejected");

    memcpy(sStateCopy, sStateBuf, (size_t) n);
    put_le32(sStateCopy + 8, (uint32_t) n + 1u);
    check(gdx_course_state_decode(sStateCopy, (size_t) n, NULL, NULL) == GDX_COURSE_STATE_ERR_LENGTH,
          "a fileLength that does not match the bytes is rejected");

    memcpy(sStateCopy, sStateBuf, (size_t) n);
    idLen = sStateCopy[GDX_COURSE_STATE_HEADER_SIZE];
    flagsByte = sStateCopy + GDX_COURSE_STATE_HEADER_SIZE + 1 + idLen + 4;
    *flagsByte |= 0x04u;
    reseal_state(sStateCopy, (size_t) n);
    check(gdx_course_state_decode(sStateCopy, (size_t) n, NULL, NULL) == GDX_COURSE_STATE_ERR_ENTRY,
          "an unknown entry flag is rejected");

    /* A store entry whose bindCrc is not the CRC of its course id is refused before anything is written. */
    memset(&entry, 0, sizeof(entry));
    snprintf(entry.courseId, sizeof(entry.courseId), "%s", "BIND.CRSD");
    entry.bindCrc = 0x1234u;
    entry.hasTitle = 1;
    snprintf(entry.title, sizeof(entry.title), "%s", "BOUND");
    n = gdx_course_state_encode(&entry, 1, sStateCopy, sizeof(sStateCopy));
    check(n > 0, "a state file with a wrong binding can be written for the test");
    check(gdx_course_store_restore_file(sStateCopy, (size_t) n) == GDX_COURSE_STATE_ERR_ENTRY,
          "store restore refuses an entry not bound to its course id");
    {
        char title[GDX_COURSE_TITLE_MAX + 1];
        check(gdx_course_title_get("BIND.CRSD", title, sizeof(title)) == 0,
              "a refused bind leaves no title behind");
    }

    /* The encoder refuses a repeated course id and a non-printable one. */
    memset(&entry, 0, sizeof(entry));
    {
        GdxCourseStateEntry pair[2];
        memset(pair, 0, sizeof(pair));
        snprintf(pair[0].courseId, sizeof(pair[0].courseId), "%s", "SAME.CRSD");
        snprintf(pair[1].courseId, sizeof(pair[1].courseId), "%s", "SAME.CRSD");
        check(gdx_course_state_encode(pair, 2, sStateCopy, sizeof(sStateCopy)) == GDX_COURSE_STATE_ERR_ENTRY,
              "the encoder refuses a repeated course id");
    }
    check(gdx_course_state_encode(NULL, 0, NULL, 0) == GDX_COURSE_ERR_ARGS, "state encode rejects NULL output");

    gdx_course_registry_clear_extensions();
}

static void test_store_revision_and_restore(void) {
    unsigned long before;
    GdxCourseExtRecord bad;

    gdx_course_registry_clear_extensions();
    check(gdx_course_registry_register(GDX_COURSE_KIND_BGM, "bgm.custom.rev", "REV", 0) == GDX_COURSE_OK,
          "revision test registers a bgm extension");

    before = gdx_course_store_revision();
    check(gdx_course_ext_set("REV.CRSD", GDX_COURSE_KIND_BGM, GDX_COURSE_SCOPE_COURSE, "bgm.custom.rev") == 0,
          "a valid ext_set succeeds");
    check(gdx_course_store_revision() > before, "a successful ext_set raises the revision");

    before = gdx_course_store_revision();
    check(gdx_course_ext_set("REV.CRSD", GDX_COURSE_KIND_BGM, GDX_COURSE_SCOPE_COURSE, "bgm.custom.none") ==
              GDX_COURSE_ERR_UNKNOWN,
          "ext_set with an unknown key is refused");
    check(gdx_course_store_revision() == before, "a refused ext_set leaves the revision unchanged");

    memset(&bad, 0, sizeof(bad));
    bad.kind = GDX_COURSE_KIND_BGM;
    bad.scope = 70;
    bad.fallbackId = 0;
    snprintf(bad.key, sizeof(bad.key), "%s", "bgm.custom.rev");
    check(gdx_course_ext_restore("REV.CRSD", &bad, 1) == GDX_COURSE_ERR_SCOPE,
          "ext_restore refuses a scope outside 0..63 and 0xFF");
    check(gdx_course_ext_count("REV.CRSD") == 1, "a refused ext_restore leaves the course unchanged");

    check(gdx_course_ext_restore("REV.CRSD", NULL, 0) == GDX_COURSE_OK, "ext_restore with count 0 is accepted");
    check(gdx_course_ext_count("REV.CRSD") == 0, "ext_restore with count 0 removes the course records");

    gdx_course_registry_clear_extensions();
}

/* Rename hook: the keys and the title move to the new id, the old id is cleared, a same-id rename is a
 * no-op, and a NULL id is refused. */
static void test_store_rename(void) {
    char title[GDX_COURSE_TITLE_MAX + 1];

    gdx_course_registry_clear_extensions();
    check(gdx_course_registry_register(GDX_COURSE_KIND_BGM, "bgm.custom.ren", "REN", 0) == GDX_COURSE_OK,
          "rename test registers a bgm extension");
    check(gdx_course_ext_set("OLDN.CRSD", GDX_COURSE_KIND_BGM, GDX_COURSE_SCOPE_COURSE, "bgm.custom.ren") == 0,
          "rename test sets a key on the old id");
    check(gdx_course_title_set("OLDN.CRSD", "LONG NAME") == GDX_COURSE_OK, "rename test sets a title");

    check(gdx_course_store_rename("OLDN.CRSD", "NEWN.CRSD") == GDX_COURSE_OK, "rename moves the store");
    check(gdx_course_ext_count("NEWN.CRSD") == 1, "the new id holds the moved key");
    check(gdx_course_ext_count("OLDN.CRSD") == 0, "the old id loses its keys");
    check(gdx_course_title_get("NEWN.CRSD", title, sizeof(title)) == 1 && strcmp(title, "LONG NAME") == 0,
          "the new id holds the moved title");
    check(gdx_course_title_get("OLDN.CRSD", title, sizeof(title)) == 0, "the old id loses its title");

    check(gdx_course_store_rename("NEWN.CRSD", "NEWN.CRSD") == GDX_COURSE_OK, "a same-id rename is a no-op");
    check(gdx_course_ext_count("NEWN.CRSD") == 1, "a same-id rename keeps the key");
    check(gdx_course_store_rename(NULL, "X.CRSD") == GDX_COURSE_ERR_ARGS, "rename refuses a NULL id");

    gdx_course_title_clear("NEWN.CRSD");
    gdx_course_registry_clear_extensions();
}

/* ---------------------------------------------------------------------------------
 * Pack manifest lines (elements.txt).
 * ------------------------------------------------------------------------------- */

static int manifest_parse(const char* text, int lineNumber, GdxCourseManifestEntry* e, char* reason) {
    return gdx_course_manifest_parse_line(text, strlen(text), lineNumber, e, reason, GDX_COURSE_MANIFEST_REASON_MAX);
}

static void test_manifest(void) {
    GdxCourseManifestEntry e;
    char reason[GDX_COURSE_MANIFEST_REASON_MAX];
    char longName[GDX_COURSE_DISPLAY_MAX + 2];
    char longLine[GDX_COURSE_MANIFEST_LINE_MAX + 2];

    gdx_course_registry_clear_extensions();
    gdx_course_registry_register(GDX_COURSE_KIND_BGM, "bgm.custom.dune", "DUNE", 0);

    check(manifest_parse("bgm|bgm.custom.dune|Dune Shift|3", 1, &e, reason) == 1,
          "manifest accepts a bgm line");
    check(e.kind == GDX_COURSE_KIND_BGM && strcmp(e.key, "bgm.custom.dune") == 0 &&
              strcmp(e.displayName, "Dune Shift") == 0 && e.fallbackId == 3 && e.path[0] == '\0',
          "manifest copies kind, key, name and fallback");
    check(manifest_parse("skybox|skybox.dusk|Dusk|2\r", 2, &e, reason) == 1,
          "manifest accepts a trailing CR (CRLF files)");
    check(e.kind == GDX_COURSE_KIND_SKYBOX && e.fallbackId == 2, "manifest reads a skybox line");
    check(manifest_parse("venue|venue.x|X|0", 3, &e, reason) == 1 && e.kind == GDX_COURSE_KIND_VENUE,
          "manifest accepts a venue line");
    check(manifest_parse("part|part.x|X|0", 4, &e, reason) == 1 && e.kind == GDX_COURSE_KIND_PART_STYLE,
          "manifest accepts a part line");
    check(manifest_parse("effect|effect.x|X|0", 5, &e, reason) == 1 && e.kind == GDX_COURSE_KIND_TRACK_EFFECT,
          "manifest accepts an effect line");
    check(manifest_parse("bgm|bgm.custom.dune|Dune|3|music/dune.ogg", 6, &e, reason) == 1 &&
              strcmp(e.path, "music/dune.ogg") == 0,
          "manifest accepts a bgm line with a relative .ogg path");
    check(manifest_parse("bgm|bgm.custom.dune|Dune|3|music/DUNE.FLAC", 7, &e, reason) == 1,
          "manifest accepts an upper case .FLAC path");
    check(manifest_parse("bgm|bgm.custom.dune|Dune|3|music/dune.wav", 8, &e, reason) == 1,
          "manifest accepts a .wav path");

    check(manifest_parse("# a comment", 9, &e, reason) == GDX_COURSE_MANIFEST_SKIP, "manifest skips a comment");
    check(manifest_parse("", 10, &e, reason) == GDX_COURSE_MANIFEST_SKIP, "manifest skips an empty line");
    check(manifest_parse("   ", 11, &e, reason) == GDX_COURSE_MANIFEST_SKIP, "manifest skips a blank line");

    check(manifest_parse("bogus|k|n|0", 7, &e, reason) == GDX_COURSE_MANIFEST_ERR_KIND,
          "manifest rejects an unknown kind");
    check(strncmp(reason, "line 7: ", 8) == 0, "kind reject names line 7");
    check(manifest_parse("bgm|BGM.Upper|Name|0", 3, &e, reason) == GDX_COURSE_MANIFEST_ERR_KEY,
          "manifest rejects an upper case key");
    check(strncmp(reason, "line 3: ", 8) == 0, "key reject names line 3");
    check(manifest_parse("bgm|bgm.x|Name|999", 12, &e, reason) == GDX_COURSE_MANIFEST_ERR_FALLBACK,
          "manifest rejects a fallback that is not a stock id");
    check(strncmp(reason, "line 12: ", 9) == 0, "fallback reject names line 12");
    check(manifest_parse("skybox|skybox.x|Name|8", 1, &e, reason) == GDX_COURSE_MANIFEST_ERR_FALLBACK,
          "manifest rejects skybox fallback 8 (stock skyboxes are 0..7)");
    check(manifest_parse("bgm|bgm.x|Name|1a", 1, &e, reason) == GDX_COURSE_MANIFEST_ERR_FALLBACK,
          "manifest rejects a fallback with letters");
    check(manifest_parse("bgm|bgm.x|Name", 1, &e, reason) == GDX_COURSE_MANIFEST_ERR_FIELDS,
          "manifest rejects three fields");
    check(manifest_parse("bgm|bgm.x|Name|0|a|b", 1, &e, reason) == GDX_COURSE_MANIFEST_ERR_FIELDS,
          "manifest rejects six fields");
    check(manifest_parse("bgm|bgm.x|01234567890123456789012345678901234567890123456789012345678901234|0", 1, &e,
                         reason) == GDX_COURSE_MANIFEST_ERR_NAME,
          "manifest rejects a name longer than 64 characters");
    memset(longName, 'N', sizeof(longName) - 1);
    longName[sizeof(longName) - 1] = '\0';
    check(manifest_parse(longName, 1, &e, reason) == GDX_COURSE_MANIFEST_ERR_FIELDS,
          "manifest rejects a long name line without fields");
    check(manifest_parse("skybox|skybox.x|Name|0|music/a.ogg", 1, &e, reason) == GDX_COURSE_MANIFEST_ERR_PATH,
          "manifest rejects a path on a non-bgm line");
    check(manifest_parse("bgm|bgm.x|Name|0|../evil.ogg", 1, &e, reason) == GDX_COURSE_MANIFEST_ERR_PATH,
          "manifest rejects a path with ..");
    check(manifest_parse("bgm|bgm.x|Name|0|C:/evil.ogg", 1, &e, reason) == GDX_COURSE_MANIFEST_ERR_PATH,
          "manifest rejects a drive letter path");
    check(manifest_parse("bgm|bgm.x|Name|0|/abs/evil.ogg", 1, &e, reason) == GDX_COURSE_MANIFEST_ERR_PATH,
          "manifest rejects an absolute path");
    check(manifest_parse("bgm|bgm.x|Name|0|music\\evil.ogg", 1, &e, reason) == GDX_COURSE_MANIFEST_ERR_PATH,
          "manifest rejects a backslash path");
    check(manifest_parse("bgm|bgm.x|Name|0|music/a.mp3", 1, &e, reason) == GDX_COURSE_MANIFEST_ERR_PATH,
          "manifest rejects an unsupported audio extension");
    check(manifest_parse("bgm|bgm.x|Name|0|", 1, &e, reason) == GDX_COURSE_MANIFEST_ERR_PATH,
          "manifest rejects an empty path field");
    check(manifest_parse("bgm|bgm.x|Na\tme|0", 4, &e, reason) == GDX_COURSE_MANIFEST_ERR_TEXT,
          "manifest rejects a control byte");
    check(strncmp(reason, "line 4: ", 8) == 0, "control byte reject names line 4");

    memset(longLine, 'a', sizeof(longLine) - 1);
    longLine[sizeof(longLine) - 1] = '\0';
    check(gdx_course_manifest_parse_line(longLine, sizeof(longLine) - 1, 9, &e, reason,
                                         GDX_COURSE_MANIFEST_REASON_MAX) == GDX_COURSE_MANIFEST_ERR_LONG,
          "manifest rejects a 257 byte line");
    check(strncmp(reason, "line 9: ", 8) == 0, "long line reject names line 9");
    check(gdx_course_manifest_parse_line(NULL, 0, 1, &e, reason, GDX_COURSE_MANIFEST_REASON_MAX) ==
              GDX_COURSE_MANIFEST_ERR_ARGS,
          "manifest refuses a NULL line");

    gdx_course_registry_clear_extensions();
}

int main(void) {
    test_stock_tables();
    test_keys();
    test_resolve();
    test_register();
    test_store();
    test_codec_round_trip();
    test_codec_damage();
    test_codec_malformed();
    test_music_entries();
    test_short_name();
    test_title_store();
    test_title_codec();
    test_state_round_trip();
    test_state_damage();
    test_store_revision_and_restore();
    test_store_rename();
    test_manifest();

    if (sFailures != 0) {
        fprintf(stderr, "%d course registry test(s) failed\n", sFailures);
        return 1;
    }
    printf("All course registry tests passed\n");
    return 0;
}
