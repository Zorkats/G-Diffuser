/* port/tests/gdx_course_meta_tests.c -- GXM1 block, editor save rules, CPU target, GMS1 file and GMX1 sidecar. */

#define _CRT_SECURE_NO_WARNINGS 1
#include "../gdx_course_meta.h"
#include "../gdx_course_registry.h"

#include <stdio.h>
#include <string.h>

static int sFailures;

#define CHECK(cond, text)                 \
    do {                                  \
        if (cond) {                       \
            printf("PASS: %s\n", (text)); \
        } else {                          \
            printf("FAIL: %s\n", (text)); \
            sFailures++;                  \
        }                                 \
    } while (0)

static unsigned char sBlock[GDX_META_BLOCK_READ_MAX_BYTES];
static unsigned char sFile[GDX_META_FILE_MAX_BYTES];
static unsigned char sSidecar[GDX_META_SIDECAR_MAX_BYTES];

static void put16(unsigned char* p, uint32_t v) {
    p[0] = (unsigned char) v;
    p[1] = (unsigned char) (v >> 8);
}

static void put32(unsigned char* p, uint32_t v) {
    p[0] = (unsigned char) v;
    p[1] = (unsigned char) (v >> 8);
    p[2] = (unsigned char) (v >> 16);
    p[3] = (unsigned char) (v >> 24);
}

static void reseal(unsigned char* data, size_t length) {
    put32(data + length - 4, gdx_course_registry_crc32(data, length - 4));
}

static void make_meta(GdxCourseMeta* m) {
    memset(m, 0, sizeof(*m));
    strcpy(m->author, "Heebeejeebees");
    strcpy(m->editedBy, "Stephe");
    m->staffTime = 61234;
    m->staffLaps = 3;
}

static int meta_equal(const GdxCourseMeta* a, const GdxCourseMeta* b) {
    return (strcmp(a->author, b->author) == 0) && (strcmp(a->editedBy, b->editedBy) == 0) &&
           (a->staffTime == b->staffTime) && (a->staffLaps == b->staffLaps) && (a->madeHere == b->madeHere) &&
           (a->hasSignature == b->hasSignature) && (memcmp(a->signature, b->signature, sizeof(a->signature)) == 0) &&
           (a->splitsLength == b->splitsLength) && (memcmp(a->splits, b->splits, a->splitsLength) == 0);
}

static GdxCourseSplits sSplits;
static GdxCourseSplits sSplitsOut;

/* The splits of make_meta's staff run: three laps of two checkpoints, the last lap ending at 61234 ms. */
static void make_splits(GdxCourseSplits* s) {
    memset(s, 0, sizeof(*s));
    s->laps = 3;
    s->nodes = 2;
    s->lapEnd[0] = 20000;
    s->lapEnd[1] = 40000;
    s->lapEnd[2] = 61234;
    s->node[0][0] = 5000;
    s->node[0][1] = 15000;
    s->node[1][0] = 25000;
    s->node[1][1] = 35000;
    s->node[2][0] = 45000;
    s->node[2][1] = 55000;
}

static void add_splits(GdxCourseMeta* m) {
    make_splits(&sSplits);
    if (gdx_meta_set_splits(m, &sSplits) != GDX_META_OK) {
        printf("FAIL: test setup could not add splits\n");
        sFailures++;
    }
}

/* The largest split record: every lap and every checkpoint, 3000 ms apart. */
static void make_full_splits(GdxCourseSplits* s) {
    int l;
    int n;

    memset(s, 0, sizeof(*s));
    s->laps = GDX_SPLITS_LAPS_MAX;
    s->nodes = GDX_SPLITS_NODES_MAX;
    for (l = 0; l < GDX_SPLITS_LAPS_MAX; l++) {
        const uint32_t start = (l == 0) ? 0u : s->lapEnd[l - 1];

        for (n = 0; n < GDX_SPLITS_NODES_MAX; n++) {
            s->node[l][n] = start + 3000u * (uint32_t) (n + 1);
        }
        s->lapEnd[l] = start + 3000u * (GDX_SPLITS_NODES_MAX + 1);
    }
}

/* Hand-built block: header, then each record as given, then the CRC. Returns its length. */
static size_t build_block(unsigned char* out, uint32_t bind, const int* tags, const char* const* values,
                          const size_t* lengths, int count) {
    size_t pos = GDX_META_BLOCK_HEADER_SIZE;
    int i;

    memcpy(out, "GXM1", 4);
    put16(out + 4, 1);
    put16(out + 6, (uint32_t) count);
    put32(out + 8, bind);
    for (i = 0; i < count; i++) {
        out[pos] = (unsigned char) tags[i];
        put16(out + pos + 1, (uint32_t) lengths[i]);
        memcpy(out + pos + 3, values[i], lengths[i]);
        pos += 3 + lengths[i];
    }
    pos += 4;
    reseal(out, pos);
    return pos;
}

static void test_names(void) {
    CHECK(gdx_meta_name_ok("Zorkats"), "a plain name is legal");
    CHECK(gdx_meta_name_ok("ABCDEFGHIJKLMNOPQRST"), "a 20-character name is legal");
    CHECK(!gdx_meta_name_ok("ABCDEFGHIJKLMNOPQRSTU"), "a 21-character name is refused");
    CHECK(!gdx_meta_name_ok(""), "an empty name is refused");
    CHECK(!gdx_meta_name_ok(NULL), "a NULL name is refused");
    CHECK(!gdx_meta_name_ok(" Aaron"), "a leading space is refused");
    CHECK(!gdx_meta_name_ok("Aaron "), "a trailing space is refused");
    CHECK(gdx_meta_name_ok("Mr. X 64"), "inner spaces and punctuation are legal");
    CHECK(!gdx_meta_name_ok("A\tB"), "a control character is refused");
    CHECK(!gdx_meta_name_ok("caf\xC3\xA9"), "non-ASCII bytes are refused");
}

static void test_validate(void) {
    GdxCourseMeta m;

    make_meta(&m);
    CHECK(gdx_meta_validate(&m) == GDX_META_OK, "a full value is valid");
    make_meta(&m);
    m.author[0] = '\0';
    CHECK(gdx_meta_validate(&m) == GDX_META_ERR_VALUE, "Edited by needs an author");
    make_meta(&m);
    strcpy(m.editedBy, m.author);
    CHECK(gdx_meta_validate(&m) == GDX_META_ERR_VALUE, "Edited by must differ from the author");
    make_meta(&m);
    m.staffLaps = 0;
    CHECK(gdx_meta_validate(&m) == GDX_META_ERR_VALUE, "a staff time needs its lap count");
    make_meta(&m);
    m.staffTime = 0;
    CHECK(gdx_meta_validate(&m) == GDX_META_ERR_VALUE, "a lap count needs its staff time");
    make_meta(&m);
    m.staffLaps = GDX_META_LAPS_MAX + 1;
    CHECK(gdx_meta_validate(&m) == GDX_META_ERR_VALUE, "100 laps is refused");
    make_meta(&m);
    m.staffTime = GDX_META_STAFF_TIME_MAX + 1;
    CHECK(gdx_meta_validate(&m) == GDX_META_ERR_VALUE, "a staff time at MAX_TIMER is refused");
    memset(&m, 0, sizeof(m));
    CHECK(gdx_meta_is_empty(&m) && (gdx_meta_validate(&m) == GDX_META_OK), "an empty value is valid and empty");
}

static void test_block(void) {
    const uint32_t bind = 0x1234ABCDu;
    GdxCourseMeta in;
    GdxCourseMeta out;
    int len;

    make_meta(&in);
    len = gdx_meta_encode(&in, bind, sBlock, sizeof(sBlock));
    CHECK(len == GDX_META_BLOCK_HEADER_SIZE + (3 + 13) + (3 + 6) + (3 + GDX_META_STAFF_BYTES) + 4,
          "the block holds exactly the three records");
    CHECK(len <= GDX_META_BLOCK_MAX_BYTES, "the block fits the write maximum");
    CHECK((gdx_meta_decode(sBlock, (size_t) len, bind, &out) == GDX_META_OK) && meta_equal(&in, &out),
          "a block round-trips");
    CHECK(gdx_meta_decode(sBlock, (size_t) len, bind + 1, &out) == GDX_META_ERR_BINDING,
          "a block bound to another course is refused");
    CHECK(out.author[0] == '\0' && out.staffTime == 0, "a refused block leaves the output empty");

    sBlock[GDX_META_BLOCK_HEADER_SIZE + 4] ^= 1;
    CHECK(gdx_meta_decode(sBlock, (size_t) len, bind, &out) == GDX_META_ERR_CRC, "a changed byte fails the CRC");
    sBlock[GDX_META_BLOCK_HEADER_SIZE + 4] ^= 1;
    sBlock[0] = 'Q';
    CHECK(gdx_meta_decode(sBlock, (size_t) len, bind, &out) == GDX_META_ERR_MAGIC, "a wrong magic is refused");
    sBlock[0] = 'G';
    put16(sBlock + 4, 2);
    reseal(sBlock, (size_t) len);
    CHECK(gdx_meta_decode(sBlock, (size_t) len, bind, &out) == GDX_META_ERR_VERSION, "version 2 is refused");
    CHECK(gdx_meta_decode(sBlock, 10, bind, &out) == GDX_META_ERR_TRUNCATED, "a short block is refused");

    memset(&in, 0, sizeof(in));
    strcpy(in.author, "Aaron");
    len = gdx_meta_encode(&in, bind, sBlock, sizeof(sBlock));
    CHECK((gdx_meta_decode(sBlock, (size_t) len, bind, &out) == GDX_META_OK) && meta_equal(&in, &out),
          "an author alone round-trips");
    memset(&in, 0, sizeof(in));
    len = gdx_meta_encode(&in, bind, sBlock, sizeof(sBlock));
    CHECK((len == GDX_META_BLOCK_HEADER_SIZE + 4) && (gdx_meta_decode(sBlock, (size_t) len, bind, &out) == GDX_META_OK),
          "an empty value encodes to a header and a CRC");
    make_meta(&in);
    in.author[0] = '\0';
    CHECK(gdx_meta_encode(&in, bind, sBlock, sizeof(sBlock)) == GDX_META_ERR_VALUE, "encode refuses an invalid value");
}

static void test_unknown_records(void) {
    const uint32_t bind = 77;
    const char staff[5] = { (char) 0x10, (char) 0x27, 0, 0, 2 }; /* 10000 ms over 2 laps */
    char splits[300];
    GdxCourseMeta out;
    size_t len;

    memset(splits, 0x5A, sizeof(splits));
    {
        const int tags[] = { 1, 3, 9 };
        const char* const values[] = { "Aaron", staff, splits };
        const size_t lengths[] = { 5, 5, sizeof(splits) };

        len = build_block(sBlock, bind, tags, values, lengths, 3);
        CHECK((gdx_meta_decode(sBlock, len, bind, &out) == GDX_META_OK) && (strcmp(out.author, "Aaron") == 0) &&
                  (out.staffTime == 10000) && (out.staffLaps == 2),
              "a later build's 300-byte record is skipped and the rest still reads");
    }
    {
        const int tags[] = { 3, 1 };
        const char* const values[] = { staff, "Aaron" };
        const size_t lengths[] = { 5, 5 };

        len = build_block(sBlock, bind, tags, values, lengths, 2);
        CHECK(gdx_meta_decode(sBlock, len, bind, &out) == GDX_META_ERR_LAYOUT, "tags out of order are refused");
    }
    {
        const int tags[] = { 1, 1 };
        const char* const values[] = { "Aaron", "Stephe" };
        const size_t lengths[] = { 5, 6 };

        len = build_block(sBlock, bind, tags, values, lengths, 2);
        CHECK(gdx_meta_decode(sBlock, len, bind, &out) == GDX_META_ERR_LAYOUT, "a repeated tag is refused");
    }
    {
        const int tags[] = { 3 };
        const char* const values[] = { staff };
        const size_t lengths[] = { 4 };

        len = build_block(sBlock, bind, tags, values, lengths, 1);
        CHECK(gdx_meta_decode(sBlock, len, bind, &out) == GDX_META_ERR_LAYOUT, "a staff record of 4 bytes is refused");
    }
    {
        const int tags[] = { 1 };
        const char* const values[] = { "Aaron " };
        const size_t lengths[] = { 6 };

        len = build_block(sBlock, bind, tags, values, lengths, 1);
        CHECK(gdx_meta_decode(sBlock, len, bind, &out) == GDX_META_ERR_VALUE, "an author with a trailing space is refused");
    }
    {
        const int tags[] = { 1 };
        const char* const values[] = { "Aaron" };
        const size_t lengths[] = { 5 };

        len = build_block(sBlock, bind, tags, values, lengths, 1);
        put16(sBlock + GDX_META_BLOCK_HEADER_SIZE + 1, 50);
        reseal(sBlock, len);
        CHECK(gdx_meta_decode(sBlock, len, bind, &out) == GDX_META_ERR_LAYOUT, "a record past the end is refused");
    }
}

static void test_editor_save(void) {
    GdxCourseMeta m;

    memset(&m, 0, sizeof(m));
    m.staffTime = 50000;
    m.staffLaps = 3;
    gdx_meta_apply_editor_save(&m, "Zorkats", 0);
    CHECK((strcmp(m.author, "Zorkats") == 0) && (m.editedBy[0] == '\0') && (m.staffTime == 0) && (m.staffLaps == 0),
          "the first save names the author and clears the staff time");
    CHECK(m.madeHere == 1, "a save marks the track made here");

    gdx_meta_apply_editor_save(&m, "Stephe", 0);
    CHECK((strcmp(m.author, "Zorkats") == 0) && (strcmp(m.editedBy, "Stephe") == 0),
          "a save by someone else keeps the author and records Edited by");

    gdx_meta_apply_editor_save(&m, "Zorkats", 0);
    CHECK((strcmp(m.author, "Zorkats") == 0) && (strcmp(m.editedBy, "Stephe") == 0),
          "the author's own save keeps the last Edited by");

    gdx_meta_apply_editor_save(&m, "Aaron", 0);
    CHECK(strcmp(m.editedBy, "Aaron") == 0, "Edited by is the latest other editor");

    m.staffTime = 40000;
    m.staffLaps = 3;
    gdx_meta_apply_editor_save(&m, "", 0);
    CHECK((strcmp(m.author, "Zorkats") == 0) && (strcmp(m.editedBy, "Aaron") == 0) && (m.staffTime == 0),
          "a save with no creator name only clears the staff time");
    gdx_meta_apply_editor_save(&m, " bad", 0);
    CHECK(strcmp(m.editedBy, "Aaron") == 0, "an illegal creator name changes no name");
    CHECK(gdx_meta_validate(&m) == GDX_META_OK, "every save leaves a valid value");

    memset(&m, 0, sizeof(m));
    m.staffTime = 40000;
    m.staffLaps = 3;
    gdx_meta_apply_editor_save(&m, NULL, 0);
    CHECK((m.author[0] == '\0') && (m.staffTime == 0) && (m.madeHere == 1) && !gdx_meta_is_empty(&m),
          "a save with no creator name still marks the track, so the store keeps it");
}

static void test_editor_save_keeps_staff(void) {
    GdxCourseMeta m;

    memset(&m, 0, sizeof(m));
    gdx_meta_apply_editor_save(&m, "Zorkats", 1);
    CHECK(gdx_meta_set_staff(&m, 3, 61234) == GDX_META_OK, "a staff time is set");
    add_splits(&m);
    CHECK(m.splitsLength != 0, "its splits are attached");
    m.hasSignature = 1;
    m.signature[0] = 0x5A;

    gdx_meta_apply_editor_save(&m, "Zorkats", 1);
    CHECK((m.staffTime == 61234) && (m.staffLaps == 3) && (m.splitsLength != 0),
          "a save with Keep staff time on keeps the staff time and its splits");
    CHECK((m.hasSignature == 0) && (m.signature[0] == 0), "the signature is still dropped");
    CHECK(gdx_meta_validate(&m) == GDX_META_OK, "the kept value stays valid");
    CHECK(!gdx_meta_staff_offer(&m, 3, 70000), "a slower run is not offered over a kept staff time");
    CHECK(gdx_meta_staff_offer(&m, 3, 50000), "a faster run is offered");

    gdx_meta_clear_staff(&m);
    CHECK((m.staffTime == 0) && (m.staffLaps == 0) && (m.splitsLength == 0), "Clear staff time drops the run");
    CHECK((strcmp(m.author, "Zorkats") == 0) && (m.madeHere == 1), "clearing keeps the names and the made-here mark");
    CHECK(gdx_meta_staff_offer(&m, 3, 70000), "after a clear, a slower run is offered");
    CHECK(gdx_meta_validate(&m) == GDX_META_OK, "the cleared value stays valid");

    gdx_meta_clear_staff(NULL);
}

static void test_cpu_target(void) {
    /* Three stock courses, four difficulties each. Course 2 has no staff record. */
    static const uint32_t targets[] = { 110000, 100000, 90000, 80000, 220000, 200000, 180000, 160000,
                                        999999, 999999, 999999, 999999 };
    static const int32_t staff[] = { 50000, 100000, -1 };
    static const int32_t noStaff[] = { -1, -1, -1 };
    GdxCourseMeta m;

    memset(&m, 0, sizeof(m));
    m.staffTime = 60000;
    m.staffLaps = 3;
    /* Novice ratios 2.2 and 2.2, so 60000 * 2.2. */
    CHECK(gdx_meta_cpu_target(&m, 3, 0, -1, targets, staff, 3) == 132000, "Novice follows the stock ratio");
    /* Master ratios 1.6 and 1.6. */
    CHECK(gdx_meta_cpu_target(&m, 3, 3, -1, targets, staff, 3) == 96000, "Master follows the stock ratio");
    CHECK(gdx_meta_cpu_target(&m, 6, 3, -1, targets, staff, 3) == 192000, "twice the laps doubles the target");
    CHECK(gdx_meta_cpu_target(&m, 1, 3, -1, targets, staff, 3) == 32000, "one lap is a third of three");
    CHECK(gdx_meta_cpu_target(&m, 3, 3, -1, targets, noStaff, 3) == -1, "no stock staff record means no target");
    CHECK(gdx_meta_cpu_target(&m, 3, 4, -1, targets, staff, 3) == -1, "difficulty 4 has no table");
    CHECK(gdx_meta_cpu_target(&m, 0, 3, -1, targets, staff, 3) == -1, "0 laps gives no target");
    CHECK(gdx_meta_cpu_target(NULL, 3, 3, -1, targets, staff, 3) == -1, "no metadata gives no target");
    memset(&m, 0, sizeof(m));
    strcpy(m.author, "Aaron");
    CHECK(gdx_meta_cpu_target(&m, 3, 3, -1, targets, staff, 3) == -1, "an author without a staff time gives no target");
    m.staffTime = 3000000;
    m.staffLaps = 1;
    CHECK(gdx_meta_cpu_target(&m, 99, 0, -1, targets, staff, 3) == -1, "a target past MAX_TIMER gives no target");
    {
        /* One odd entry among five: the median ignores it. */
        static const uint32_t five[] = { 100, 100, 100, 100, 100, 100, 100, 100, 100, 100, 100, 100, 100, 100, 100,
                                         100, 1, 1, 1, 1 };
        static const int32_t fiveStaff[] = { 50, 50, 50, 50, 50 };

        memset(&m, 0, sizeof(m));
        m.staffTime = 1000;
        m.staffLaps = 1;
        CHECK(gdx_meta_cpu_target(&m, 1, 0, -1, five, fiveStaff, 5) == 2000, "the median ignores one odd stock entry");
    }
    memset(&m, 0, sizeof(m));
    m.staffTime = 60000;
    m.staffLaps = 3;
    /* Master target 96000; a 55000 record beats the staff time by 5000. */
    CHECK(gdx_meta_cpu_target(&m, 3, 3, 55000, targets, staff, 3) == 91000, "Master moves with a record that beats staff");
    CHECK(gdx_meta_cpu_target(&m, 3, 2, 55000, targets, staff, 3) == 103000, "Expert moves with it too");
    CHECK(gdx_meta_cpu_target(&m, 3, 1, 55000, targets, staff, 3) == 120000, "Standard ignores the record");
    CHECK(gdx_meta_cpu_target(&m, 3, 3, 65000, targets, staff, 3) == 96000, "a record slower than staff changes nothing");
    CHECK(gdx_meta_cpu_target(&m, 5, 3, 55000, targets, staff, 3) == 160000,
          "a race with other laps than the staff time ignores the record");
    m.staffTime = 100000;
    m.staffLaps = 5;
    CHECK(gdx_meta_cpu_target(&m, 5, 3, 55000, targets, staff, 3) == 160000,
          "a staff time over other laps than a record ignores the record");
}

static void test_store_and_file(void) {
    GdxCourseMeta a;
    GdxCourseMeta b;
    GdxCourseMeta out;
    GdxCourseMeta empty;
    unsigned long revision;
    int len;

    memset(&empty, 0, sizeof(empty));
    gdx_meta_store_clear();
    make_meta(&a);
    add_splits(&a);
    memset(&b, 0, sizeof(b));
    strcpy(b.author, "Zorkats");
    b.madeHere = 1; /* the state file keeps the made-here mark */
    CHECK(gdx_meta_store_set("BAD_TRIP.CRSD", &a) == GDX_META_OK, "store set");
    CHECK(gdx_meta_store_set("GROUND ZERO.CRSD", &b) == GDX_META_OK, "store set a second course");
    CHECK(gdx_meta_store_get("BAD_TRIP.CRSD", &out) && meta_equal(&a, &out), "store get");
    CHECK(!gdx_meta_store_get("MISSING.CRSD", &out), "store get of an unknown course");
    a.author[0] = '\0';
    CHECK(gdx_meta_store_set("BAD_TRIP.CRSD", &a) == GDX_META_ERR_VALUE, "store refuses an invalid value");
    make_meta(&a);
    add_splits(&a);

    len = gdx_meta_store_encode_file(sFile, sizeof(sFile));
    CHECK(len > 0, "file encodes");
    gdx_meta_store_clear();
    CHECK((gdx_meta_store_restore_file(sFile, (size_t) len) == GDX_META_OK) &&
              gdx_meta_store_get("BAD_TRIP.CRSD", &out) && meta_equal(&a, &out) &&
              gdx_meta_store_get("GROUND ZERO.CRSD", &out) && meta_equal(&b, &out),
          "file round-trips both courses");

    sFile[20] ^= 1;
    revision = gdx_meta_store_revision();
    CHECK(gdx_meta_store_restore_file(sFile, (size_t) len) == GDX_META_ERR_CRC, "a changed file byte fails the CRC");
    CHECK((gdx_meta_store_revision() == revision) && gdx_meta_store_get("BAD_TRIP.CRSD", &out),
          "a refused file leaves the store as it was");
    sFile[20] ^= 1;
    sFile[1] = 'X';
    CHECK(gdx_meta_store_restore_file(sFile, (size_t) len) == GDX_META_ERR_MAGIC, "a GKS1-like magic is refused");
    sFile[1] = 'M';

    CHECK((gdx_meta_store_rename("BAD_TRIP.CRSD", "GOOD_TRIP.CRSD") == GDX_META_OK) &&
              !gdx_meta_store_get("BAD_TRIP.CRSD", &out) && gdx_meta_store_get("GOOD_TRIP.CRSD", &out) &&
              meta_equal(&a, &out),
          "rename moves the value");
    CHECK((gdx_meta_store_rename("NOTHING.CRSD", "GROUND ZERO.CRSD") == GDX_META_OK) &&
              !gdx_meta_store_get("GROUND ZERO.CRSD", &out),
          "a rename from an id with nothing clears the new id");
    CHECK((gdx_meta_store_set("GOOD_TRIP.CRSD", &empty) == GDX_META_OK) && !gdx_meta_store_get("GOOD_TRIP.CRSD", &out),
          "setting an empty value deletes the entry");

    gdx_meta_store_clear();
    len = gdx_meta_store_encode_file(sFile, sizeof(sFile));
    CHECK((len == GDX_META_FILE_HEADER_SIZE + 4) && (gdx_meta_store_restore_file(sFile, (size_t) len) == GDX_META_OK),
          "an empty store writes and reads an empty file");
}

typedef struct VisitLog {
    int count;
    char ids[GDX_META_SIDECAR_MAX_ENTRIES][GDX_BRANCH_COURSE_ID_MAX + 1];
    uint32_t binds[GDX_META_SIDECAR_MAX_ENTRIES];
    GdxCourseMeta metas[GDX_META_SIDECAR_MAX_ENTRIES];
} VisitLog;

static void visit(const char* courseId, uint32_t bindCrc, const GdxCourseMeta* meta, void* ctx) {
    VisitLog* log = (VisitLog*) ctx;

    if (log->count < GDX_META_SIDECAR_MAX_ENTRIES) {
        strcpy(log->ids[log->count], courseId);
        log->binds[log->count] = bindCrc;
        log->metas[log->count] = *meta;
        log->count++;
    }
}

static void test_sidecar(void) {
    GdxCourseMeta a;
    GdxCourseMeta b;
    GdxCourseMeta empty;
    GdxMetaSidecarEntry entries[3];
    VisitLog log;
    int len;

    make_meta(&a);
    add_splits(&a);
    memset(&b, 0, sizeof(b));
    strcpy(b.author, "Aaron");
    b.staffTime = 90000;
    b.staffLaps = 5;
    memset(&empty, 0, sizeof(empty));
    entries[0].courseId = "ONE.CRSD";
    entries[0].bindCrc = 111;
    entries[0].meta = &a;
    entries[1].courseId = "TWO.CRSD";
    entries[1].bindCrc = 222;
    entries[1].meta = &empty;
    entries[2].courseId = "THREE.CRSD";
    entries[2].bindCrc = 333;
    entries[2].meta = &b;

    len = gdx_meta_sidecar_encode(entries, 3, sSidecar, sizeof(sSidecar));
    CHECK(len > 0, "sidecar encodes");
    memset(&log, 0, sizeof(log));
    CHECK(gdx_meta_sidecar_decode(sSidecar, (size_t) len, visit, &log) == GDX_META_OK, "sidecar decodes");
    CHECK((log.count == 2) && (strcmp(log.ids[0], "ONE.CRSD") == 0) && (log.binds[0] == 111) &&
              meta_equal(&log.metas[0], &a) && (strcmp(log.ids[1], "THREE.CRSD") == 0) && (log.binds[1] == 333) &&
              meta_equal(&log.metas[1], &b),
          "the empty entry is skipped and the others keep their ids, bindings and values");

    sSidecar[len - 10] ^= 1;
    memset(&log, 0, sizeof(log));
    CHECK((gdx_meta_sidecar_decode(sSidecar, (size_t) len, visit, &log) == GDX_META_ERR_CRC) && (log.count == 0),
          "a changed sidecar byte visits nothing");
    sSidecar[len - 10] ^= 1;

    entries[0].meta = &empty;
    entries[2].meta = &empty;
    CHECK(gdx_meta_sidecar_encode(entries, 3, sSidecar, sizeof(sSidecar)) == 0,
          "a sidecar with no metadata writes nothing");
    CHECK(gdx_meta_sidecar_encode(entries, GDX_META_SIDECAR_MAX_ENTRIES + 1, sSidecar, sizeof(sSidecar)) ==
              GDX_META_ERR_ARGS,
          "more entries than a cup holds are refused");
}

static void test_made_here(void) {
    const uint32_t bind = 4242;
    GdxCourseMeta in;
    GdxCourseMeta out;
    GdxCourseMeta shared;
    GdxMetaSidecarEntry entry;
    VisitLog log;
    size_t pos;
    int blockLen;
    int len;

    memset(&in, 0, sizeof(in));
    in.madeHere = 1;
    blockLen = gdx_meta_encode(&in, bind, sBlock, sizeof(sBlock));
    CHECK(blockLen == GDX_META_BLOCK_HEADER_SIZE + GDX_META_RECORD_HEADER_SIZE + 4,
          "the made-here mark is a record with no value");
    CHECK((gdx_meta_decode(sBlock, (size_t) blockLen, bind, &out) == GDX_META_OK) && (out.madeHere == 1),
          "a block keeps the made-here mark for the state file");
    make_meta(&in);
    in.madeHere = 1;
    in.hasSignature = 1;
    make_full_splits(&sSplits);
    in.staffLaps = GDX_SPLITS_LAPS_MAX;
    in.staffTime = sSplits.lapEnd[GDX_SPLITS_LAPS_MAX - 1];
    CHECK((gdx_meta_set_splits(&in, &sSplits) == GDX_META_OK) && (in.splitsLength == GDX_META_SPLITS_MAX_BYTES),
          "the largest split record fills the splits field");
    CHECK(gdx_meta_encode(&in, bind, sBlock, sizeof(sBlock)) == GDX_META_BLOCK_MAX_BYTES - 2 * GDX_META_NAME_MAX +
                                                                   (int) (strlen(in.author) + strlen(in.editedBy)),
          "a block with every record fits the write maximum exactly");
    in.madeHere = 2;
    CHECK(gdx_meta_validate(&in) == GDX_META_ERR_VALUE, "a made-here value other than 0 or 1 is refused");
    {
        const int tags[] = { 4 };
        const char* const values[] = { "x" };
        const size_t lengths[] = { 1 };

        len = (int) build_block(sBlock, bind, tags, values, lengths, 1);
        CHECK(gdx_meta_decode(sBlock, (size_t) len, bind, &out) == GDX_META_ERR_LAYOUT,
              "a made-here record with a value is refused");
    }

    /* Export: the mark stays home. */
    memset(&in, 0, sizeof(in));
    in.madeHere = 1;
    entry.courseId = "MINE.CRSD";
    entry.bindCrc = bind;
    entry.meta = &in;
    CHECK(gdx_meta_sidecar_encode(&entry, 1, sSidecar, sizeof(sSidecar)) == 0,
          "a track with only the made-here mark writes no sidecar");
    in.staffTime = 83456;
    in.staffLaps = 3;
    len = gdx_meta_sidecar_encode(&entry, 1, sSidecar, sizeof(sSidecar));
    memset(&log, 0, sizeof(log));
    CHECK((len > 0) && (gdx_meta_sidecar_decode(sSidecar, (size_t) len, visit, &log) == GDX_META_OK) &&
              (log.count == 1) && (log.metas[0].staffTime == 83456) && (log.metas[0].madeHere == 0),
          "an exported track keeps its staff time and drops the mark");

    /* Import: a hand-made sidecar that carries the mark anyway. */
    blockLen = gdx_meta_encode(&in, bind, sBlock, sizeof(sBlock));
    memcpy(sSidecar, "GMX1", 4);
    put16(sSidecar + 4, 1);
    put16(sSidecar + 6, 1);
    pos = GDX_META_SIDECAR_HEADER_SIZE;
    sSidecar[pos++] = 9;
    memcpy(sSidecar + pos, "MINE.CRSD", 9);
    pos += 9;
    put32(sSidecar + pos, bind);
    pos += 4;
    put16(sSidecar + pos, (uint32_t) blockLen);
    pos += 2;
    memcpy(sSidecar + pos, sBlock, (size_t) blockLen);
    pos += (size_t) blockLen;
    put32(sSidecar + 8, (uint32_t) (pos + 4));
    reseal(sSidecar, pos + 4);
    memset(&log, 0, sizeof(log));
    shared = in;
    shared.madeHere = 0;
    CHECK((gdx_meta_sidecar_decode(sSidecar, pos + 4, visit, &log) == GDX_META_OK) && (log.count == 1) &&
              meta_equal(&log.metas[0], &shared),
          "an imported sidecar cannot mark a track made here");
}

static void test_staff_offer(void) {
    GdxCourseMeta m;
    char text[16];

    memset(&m, 0, sizeof(m));
    CHECK(!gdx_meta_staff_offer(&m, 3, 83456), "a track made elsewhere is never offered");
    m.madeHere = 1;
    CHECK(gdx_meta_staff_offer(&m, 3, 83456), "a track made here with no staff time is offered");
    CHECK(!gdx_meta_staff_offer(&m, 5, 83456), "a run over other than 3 laps is not offered");
    CHECK(!gdx_meta_staff_offer(&m, 3, 0), "a run with no time is not offered");
    CHECK(!gdx_meta_staff_offer(&m, 3, GDX_META_STAFF_TIME_MAX + 1), "a run at MAX_TIMER is not offered");
    CHECK(!gdx_meta_staff_offer(NULL, 3, 83456), "no metadata is not offered");

    CHECK((gdx_meta_set_staff(&m, 3, 83456) == GDX_META_OK) && (m.staffTime == 83456) && (m.staffLaps == 3) &&
              (gdx_meta_validate(&m) == GDX_META_OK),
          "set staff stores the run");
    CHECK(!gdx_meta_staff_offer(&m, 3, 83456), "the same time is not offered again");
    CHECK(!gdx_meta_staff_offer(&m, 3, 90000), "a slower run is not offered");
    CHECK(gdx_meta_staff_offer(&m, 3, 83455), "a faster run is offered");
    m.staffLaps = 5;
    CHECK(gdx_meta_staff_offer(&m, 3, 99000), "a staff time over other laps cannot be compared, so a run is offered");
    m.staffLaps = 3;
    CHECK((gdx_meta_set_staff(&m, 0, 50000) == GDX_META_ERR_VALUE) && (m.staffTime == 83456),
          "set staff refuses 0 laps and leaves the value");
    CHECK((gdx_meta_set_staff(&m, 3, 0) == GDX_META_ERR_VALUE) && (m.staffTime == 83456),
          "set staff refuses a zero time and leaves the value");
    CHECK(gdx_meta_set_staff(NULL, 3, 1) == GDX_META_ERR_ARGS, "set staff refuses NULL");

    CHECK((gdx_meta_format_time(83456, text, sizeof(text)) == 8) && (strcmp(text, "1'23\"456") == 0),
          "a time formats as M'SS\"mmm");
    CHECK((gdx_meta_format_time(0, text, sizeof(text)) == 8) && (strcmp(text, "0'00\"000") == 0),
          "zero formats with every digit");
    CHECK((gdx_meta_format_time(GDX_META_STAFF_TIME_MAX, text, sizeof(text)) == 9) &&
              (strcmp(text, "59'59\"998") == 0),
          "the largest staff time formats with two minute digits");
    CHECK(gdx_meta_format_time(83456, text, 8) == -1, "a buffer with no room for the NUL is refused");
    CHECK(gdx_meta_format_time(GDX_META_STAFF_TIME_MAX + 1, text, sizeof(text)) == -1, "MAX_TIMER is refused");
}

/* Tag 6. The codec carries the 96 bytes; checking them is gdx_course_meta_sign's job. */
static void test_signature(void) {
    const uint32_t bind = 0x5151A7E5u;
    GdxCourseMeta in;
    GdxCourseMeta out;
    GdxMetaSidecarEntry entry;
    int len;
    int i;

    make_meta(&in);
    in.hasSignature = 1;
    for (i = 0; i < GDX_META_SIGNATURE_BYTES; i++) {
        in.signature[i] = (unsigned char) (i * 7 + 1);
    }
    CHECK(gdx_meta_validate(&in) == GDX_META_OK, "a signature with an author is legal");
    CHECK(!gdx_meta_is_empty(&in), "a signed meta is not empty");
    len = gdx_meta_encode(&in, bind, sBlock, sizeof(sBlock));
    CHECK(len > 0, "a signed block encodes");
    CHECK((gdx_meta_decode(sBlock, (size_t) len, bind, &out) == GDX_META_OK) && meta_equal(&in, &out),
          "a block keeps every signature byte");

    out = in;
    out.author[0] = '\0';
    out.editedBy[0] = '\0';
    CHECK(gdx_meta_validate(&out) == GDX_META_ERR_VALUE, "a signature without an author is refused");
    CHECK(gdx_meta_encode(&out, bind, sBlock, sizeof(sBlock)) == GDX_META_ERR_VALUE,
          "a signature without an author is not written");
    out = in;
    out.hasSignature = 2;
    CHECK(gdx_meta_validate(&out) == GDX_META_ERR_VALUE, "a signature flag other than 0 or 1 is refused");
    {
        const int tags[] = { 1, 6 };
        const char* const values[] = { "Zorkats", "0123456789" };
        const size_t lengths[] = { 7, 10 };

        len = (int) build_block(sBlock, bind, tags, values, lengths, 2);
        CHECK(gdx_meta_decode(sBlock, (size_t) len, bind, &out) == GDX_META_ERR_LAYOUT,
              "a signature record of the wrong size is refused");
    }
    {
        static char value[GDX_META_SIGNATURE_BYTES];
        const int tags[] = { 6 };
        const char* const values[] = { value };
        const size_t lengths[] = { GDX_META_SIGNATURE_BYTES };

        len = (int) build_block(sBlock, bind, tags, values, lengths, 1);
        CHECK(gdx_meta_decode(sBlock, (size_t) len, bind, &out) == GDX_META_ERR_VALUE,
              "a block with a signature and no author is refused");
    }

    out = in;
    gdx_meta_apply_editor_save(&out, "Heebeejeebees", 0);
    CHECK((out.hasSignature == 0) && (out.signature[0] == 0) && (out.signature[95] == 0),
          "a Course Edit save clears the signature");
    out = in;
    gdx_meta_apply_editor_save(&out, NULL, 0);
    CHECK(out.hasSignature == 0, "a save with no profile clears the signature too");

    gdx_meta_store_clear();
    CHECK(gdx_meta_store_set("SIGNED.CRSD", &in) == GDX_META_OK, "a signed meta is stored");
    len = gdx_meta_store_encode_file(sFile, sizeof(sFile));
    gdx_meta_store_clear();
    CHECK((len > 0) && (gdx_meta_store_restore_file(sFile, (size_t) len) == GDX_META_OK) &&
              gdx_meta_store_get("SIGNED.CRSD", &out) && meta_equal(&in, &out),
          "the state file keeps the signature");
    gdx_meta_store_clear();

    in.madeHere = 1;
    entry.courseId = "SIGNED.CRSD";
    entry.bindCrc = bind;
    entry.meta = &in;
    len = gdx_meta_sidecar_encode(&entry, 1, sSidecar, sizeof(sSidecar));
    CHECK(len > 0, "a signed track writes a sidecar");
    {
        VisitLog log;

        memset(&log, 0, sizeof(log));
        CHECK((gdx_meta_sidecar_decode(sSidecar, (size_t) len, visit, &log) == GDX_META_OK) &&
                  (log.count == 1) && (log.metas[0].hasSignature == 1) &&
                  (memcmp(log.metas[0].signature, in.signature, GDX_META_SIGNATURE_BYTES) == 0) &&
                  (log.metas[0].madeHere == 0),
              "the sidecar carries the signature and leaves the made-here mark home");
    }
}

static void test_splits(void) {
    const uint32_t bind = 5150;
    GdxCourseMeta in;
    GdxCourseMeta out;
    GdxCourseMeta before;
    unsigned char bytes[GDX_SPLITS_VALUE_MAX_BYTES];
    static char big[GDX_META_SPLITS_MAX_BYTES + 1];
    int splitsLen;
    int len;

    make_meta(&in);
    make_splits(&sSplits);
    CHECK((gdx_meta_set_splits(&in, &sSplits) == GDX_META_OK) && (in.splitsLength > 0) &&
              (gdx_meta_validate(&in) == GDX_META_OK),
          "splits of the staff run are set");
    CHECK(gdx_meta_get_splits(&in, &sSplitsOut) && (memcmp(&sSplitsOut, &sSplits, sizeof(sSplits)) == 0),
          "get_splits returns the splits that were set");
    len = gdx_meta_encode(&in, bind, sBlock, sizeof(sBlock));
    CHECK(len == GDX_META_BLOCK_HEADER_SIZE + (3 + 13) + (3 + 6) + (3 + GDX_META_STAFF_BYTES) + (3 + in.splitsLength) + 4,
          "the block adds a splits record");
    CHECK((gdx_meta_decode(sBlock, (size_t) len, bind, &out) == GDX_META_OK) && meta_equal(&in, &out) &&
              gdx_meta_get_splits(&out, &sSplitsOut) && (memcmp(&sSplitsOut, &sSplits, sizeof(sSplits)) == 0),
          "splits round-trip through a block");

    before = in;
    sSplits.laps = 2;
    sSplits.lapEnd[2] = 0;
    sSplits.node[2][0] = 0;
    sSplits.node[2][1] = 0;
    CHECK((gdx_meta_set_splits(&in, &sSplits) == GDX_META_ERR_VALUE) && meta_equal(&in, &before),
          "splits over another lap count are refused and meta is untouched");
    make_splits(&sSplits);
    sSplits.lapEnd[2] = 61235;
    CHECK((gdx_meta_set_splits(&in, &sSplits) == GDX_META_ERR_VALUE) && meta_equal(&in, &before),
          "splits that end off the staff time are refused");
    make_splits(&sSplits);
    sSplits.node[1][1] = sSplits.node[1][0];
    CHECK((gdx_meta_set_splits(&in, &sSplits) == GDX_META_ERR_VALUE) && meta_equal(&in, &before),
          "splits that do not encode are refused");
    memset(&sSplits, 0, sizeof(sSplits));
    CHECK(gdx_meta_set_splits(&in, &sSplits) == GDX_META_ERR_VALUE, "the empty split record is refused");
    CHECK(gdx_meta_set_splits(NULL, &sSplits) == GDX_META_ERR_ARGS, "a NULL meta is ARGS");
    CHECK((gdx_meta_set_splits(&in, NULL) == GDX_META_OK) && (in.splitsLength == 0) &&
              !gdx_meta_get_splits(&in, &sSplitsOut) && (sSplitsOut.laps == 0),
          "NULL clears the splits");

    memset(&in, 0, sizeof(in));
    strcpy(in.author, "Aaron");
    make_splits(&sSplits);
    CHECK(gdx_meta_set_splits(&in, &sSplits) == GDX_META_ERR_VALUE, "splits need a staff time");
    make_meta(&in);
    add_splits(&in);
    in.staffTime = 0;
    in.staffLaps = 0;
    CHECK(gdx_meta_validate(&in) == GDX_META_ERR_VALUE, "validate refuses splits with no staff time");

    make_meta(&in);
    add_splits(&in);
    CHECK((gdx_meta_set_staff(&in, 3, 60000) == GDX_META_OK) && (in.splitsLength == 0),
          "a new staff time clears the old run's splits");
    make_meta(&in);
    add_splits(&in);
    gdx_meta_apply_editor_save(&in, "Stephe", 0);
    CHECK((in.splitsLength == 0) && (in.staffTime == 0), "a Course Edit save clears the splits");
    make_meta(&in);
    add_splits(&in);
    in.author[0] = '\0';
    in.editedBy[0] = '\0';
    in.staffTime = 0;
    in.staffLaps = 0;
    in.splitsLength = 0;
    CHECK(gdx_meta_is_empty(&in), "a cleared value is empty");
    in.splitsLength = 1;
    CHECK(!gdx_meta_is_empty(&in), "a splits length alone is not empty");

    /* Records a later or broken build might write: each one costs the splits, never the block. */
    make_splits(&sSplits);
    sSplits.laps = 1;
    sSplits.lapEnd[1] = 0;
    sSplits.lapEnd[2] = 0;
    memset(sSplits.node[1], 0, sizeof(sSplits.node[1]));
    memset(sSplits.node[2], 0, sizeof(sSplits.node[2]));
    splitsLen = gdx_splits_encode(&sSplits, bytes, sizeof(bytes));
    {
        const char staff[5] = { (char) 0x10, (char) 0x27, 0, 0, 2 }; /* 10000 ms over 2 laps */
        const int tags[] = { 1, 3, 5 };
        const char* const values[] = { "Aaron", staff, (const char*) bytes };
        const size_t lengths[] = { 5, 5, (size_t) splitsLen };

        len = (int) build_block(sBlock, bind, tags, values, lengths, 3);
        CHECK((splitsLen > 0) && (gdx_meta_decode(sBlock, (size_t) len, bind, &out) == GDX_META_OK) &&
                  (out.splitsLength == 0) && (out.staffTime == 10000) && (strcmp(out.author, "Aaron") == 0),
              "splits that do not match the staff time are dropped and the block still reads");
    }
    {
        const char staff[5] = { (char) 0x10, (char) 0x27, 0, 0, 2 };
        const int tags[] = { 1, 3, 5 };
        const char* const values[] = { "Aaron", staff, "\x02\x00\x5A\x5A" };
        const size_t lengths[] = { 5, 5, 4 };

        len = (int) build_block(sBlock, bind, tags, values, lengths, 3);
        CHECK((gdx_meta_decode(sBlock, (size_t) len, bind, &out) == GDX_META_OK) && (out.splitsLength == 0) &&
                  (out.staffTime == 10000),
              "splits that do not decode are dropped");
    }
    {
        const int tags[] = { 1, 5 };
        const char* const values[] = { "Aaron", (const char*) bytes };
        const size_t lengths[] = { 5, (size_t) splitsLen };

        len = (int) build_block(sBlock, bind, tags, values, lengths, 2);
        CHECK((gdx_meta_decode(sBlock, (size_t) len, bind, &out) == GDX_META_OK) && (out.splitsLength == 0) &&
                  (strcmp(out.author, "Aaron") == 0),
              "splits with no staff time are dropped");
    }
    {
        const char staff[5] = { (char) 0x10, (char) 0x27, 0, 0, 2 };
        const int tags[] = { 1, 3, 5 };
        const char* const values[] = { "Aaron", staff, big };
        const size_t lengths[] = { 5, 5, sizeof(big) };

        memset(big, 0x5A, sizeof(big));
        len = (int) build_block(sBlock, bind, tags, values, lengths, 3);
        CHECK((gdx_meta_decode(sBlock, (size_t) len, bind, &out) == GDX_META_OK) && (out.splitsLength == 0) &&
                  (out.staffTime == 10000),
              "a splits record longer than this build holds is skipped");
    }

    /* The state file and the sidecar both carry the splits. */
    gdx_meta_store_clear();
    make_meta(&in);
    add_splits(&in);
    CHECK(gdx_meta_store_set("SPLITS.CRSD", &in) == GDX_META_OK, "store keeps a value with splits");
    len = gdx_meta_store_encode_file(sFile, sizeof(sFile));
    gdx_meta_store_clear();
    CHECK((len > 0) && (gdx_meta_store_restore_file(sFile, (size_t) len) == GDX_META_OK) &&
              gdx_meta_store_get("SPLITS.CRSD", &out) && meta_equal(&in, &out),
          "splits round-trip through the state file");
    {
        GdxMetaSidecarEntry entry;
        static VisitLog log;

        entry.courseId = "SPLITS.CRSD";
        entry.bindCrc = bind;
        entry.meta = &in;
        len = gdx_meta_sidecar_encode(&entry, 1, sSidecar, sizeof(sSidecar));
        memset(&log, 0, sizeof(log));
        CHECK((len > 0) && (gdx_meta_sidecar_decode(sSidecar, (size_t) len, visit, &log) == GDX_META_OK) &&
                  (log.count == 1) && meta_equal(&log.metas[0], &in),
              "splits travel in the sidecar, so an imported track keeps the creator's pace");
    }
    gdx_meta_store_clear();
}

int main(void) {
    test_names();
    test_validate();
    test_block();
    test_unknown_records();
    test_editor_save();
    test_editor_save_keeps_staff();
    test_cpu_target();
    test_store_and_file();
    test_sidecar();
    test_made_here();
    test_staff_offer();
    test_signature();
    test_splits();
    if (sFailures != 0) {
        printf("%d check(s) failed\n", sFailures);
        return 1;
    }
    printf("all checks passed\n");
    return 0;
}
