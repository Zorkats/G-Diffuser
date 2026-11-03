/* port/tests/gdx_course_settings_tests.c -- GXK1 block, GKS1 file, GXD1 sidecar and the plain view. */

#include "../gdx_course_settings.h"
#include "../gdx_course_settings_view.h"
#include "../gdx_course_registry.h"

#include <math.h>
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

static unsigned char sBlock[GDX_SETTINGS_BLOCK_MAX_BYTES];
static unsigned char sFile[GDX_SETTINGS_FILE_MAX_BYTES];
static unsigned char sSidecar[GDX_SIDECAR_MAX_BYTES];

static void put32(unsigned char* p, uint32_t v) {
    p[0] = (unsigned char) v;
    p[1] = (unsigned char) (v >> 8);
    p[2] = (unsigned char) (v >> 16);
    p[3] = (unsigned char) (v >> 24);
}

static void reseal(unsigned char* data, size_t length) {
    put32(data + length - 4, gdx_course_registry_crc32(data, length - 4));
}

static void make_settings(GdxCourseSettings* s) {
    memset(s, 0, sizeof(*s));
    s->ringCount = 10;
    s->lavaMode = GDX_SETTINGS_LAVA_NEVER_KILLS;
    s->magnetStrength = 120;
    s->magnetRange = 150;
    s->ring[3].pitWidth = 100;
    s->ring[3].dirtWidth = 25;
    s->ring[9].magnetStrength = 240;
    s->ring[9].magnetRange = 10;
    s->nodes[1][4].iceWidth = 50;
}

static void test_block(void) {
    GdxCourseSettings in;
    GdxCourseSettings out;
    const uint32_t bind = 0xCAFE1234u;
    int len;

    make_settings(&in);
    len = gdx_settings_encode(&in, bind, sBlock, sizeof(sBlock));
    CHECK(len == GDX_SETTINGS_BLOCK_HEADER_SIZE + 3 * GDX_SETTINGS_ENTRY_BYTES + 4, "entries are sparse");
    CHECK((gdx_settings_decode(sBlock, (size_t) len, bind, &out) == GDX_SETTINGS_OK) &&
              (memcmp(&in, &out, sizeof(in)) == 0),
          "a block round trips");
    CHECK(gdx_settings_decode(sBlock, (size_t) len, bind + 1, &out) == GDX_SETTINGS_ERR_BINDING,
          "a block bound elsewhere is refused");
    CHECK(out.ringCount == 0, "a refused block zeroes the result");

    sBlock[8] ^= 1;
    CHECK(gdx_settings_decode(sBlock, (size_t) len, bind, &out) == GDX_SETTINGS_ERR_CRC, "a changed byte is refused");
    sBlock[8] ^= 1;

    sBlock[7] = 3;
    reseal(sBlock, (size_t) len);
    CHECK(gdx_settings_decode(sBlock, (size_t) len, bind, &out) == GDX_SETTINGS_ERR_VALUE, "unknown lava mode refused");
    sBlock[7] = GDX_SETTINGS_LAVA_NEVER_KILLS;

    /* Entry 0 is ring point 3. */
    sBlock[GDX_SETTINGS_BLOCK_HEADER_SIZE + 2] = 101;
    reseal(sBlock, (size_t) len);
    CHECK(gdx_settings_decode(sBlock, (size_t) len, bind, &out) == GDX_SETTINGS_ERR_VALUE, "a width over 100 is refused");
    sBlock[GDX_SETTINGS_BLOCK_HEADER_SIZE + 2] = 100;

    sBlock[GDX_SETTINGS_BLOCK_HEADER_SIZE + 1] = 10;
    reseal(sBlock, (size_t) len);
    CHECK(gdx_settings_decode(sBlock, (size_t) len, bind, &out) == GDX_SETTINGS_ERR_LAYOUT,
          "a ring entry past the ring is refused");
    sBlock[GDX_SETTINGS_BLOCK_HEADER_SIZE + 1] = 9;
    reseal(sBlock, (size_t) len);
    CHECK(gdx_settings_decode(sBlock, (size_t) len, bind, &out) == GDX_SETTINGS_ERR_LAYOUT,
          "entries out of order are refused");
    sBlock[GDX_SETTINGS_BLOCK_HEADER_SIZE + 1] = 3;

    sBlock[GDX_SETTINGS_BLOCK_HEADER_SIZE + 14] = 5;
    reseal(sBlock, (size_t) len);
    CHECK(gdx_settings_decode(sBlock, (size_t) len, bind, &out) == GDX_SETTINGS_ERR_LAYOUT, "an unknown kind is refused");
    sBlock[GDX_SETTINGS_BLOCK_HEADER_SIZE + 14] = 2;
    reseal(sBlock, (size_t) len);
    CHECK(gdx_settings_decode(sBlock, (size_t) len, bind, &out) == GDX_SETTINGS_OK, "the repaired block decodes");

    CHECK(gdx_settings_decode(sBlock, 4, bind, &out) == GDX_SETTINGS_ERR_TRUNCATED, "a short block is refused");
    CHECK(gdx_settings_decode(sBlock, (size_t) len - 1, bind, &out) != GDX_SETTINGS_OK, "a cut block is refused");

    in.magnetStrength = 19;
    CHECK(gdx_settings_encode(&in, bind, sBlock, sizeof(sBlock)) == GDX_SETTINGS_ERR_VALUE,
          "encode refuses a strength under the minimum");
    make_settings(&in);
    in.ring[10].pitWidth = 1;
    CHECK(gdx_settings_validate(&in) == GDX_SETTINGS_ERR_VALUE, "a value past the ring is refused");
}

static void test_store(void) {
    GdxCourseSettings in;
    GdxCourseSettings out;
    GdxCourseSettings empty;
    unsigned long revision;
    int len;

    gdx_settings_store_clear();
    memset(&empty, 0, sizeof(empty));
    make_settings(&in);
    CHECK(gdx_settings_is_empty(&empty) && !gdx_settings_is_empty(&in), "empty is told apart");
    CHECK((gdx_settings_store_set("A.CRSD", &in) == GDX_SETTINGS_OK) && gdx_settings_store_get("A.CRSD", &out) &&
              (memcmp(&in, &out, sizeof(in)) == 0),
          "the store keeps a value");
    empty.ringCount = 10;
    CHECK((gdx_settings_store_set("A.CRSD", &empty) == GDX_SETTINGS_OK) && !gdx_settings_store_get("A.CRSD", &out),
          "an empty value removes the entry");

    gdx_settings_store_set("A.CRSD", &in);
    gdx_settings_store_set("B.CRSD", &in);
    gdx_settings_store_rename("A.CRSD", "C.CRSD");
    CHECK(!gdx_settings_store_get("A.CRSD", &out) && gdx_settings_store_get("C.CRSD", &out), "a rename moves the value");
    gdx_settings_store_rename("NONE.CRSD", "B.CRSD");
    CHECK(!gdx_settings_store_get("B.CRSD", &out), "renaming nothing onto a course clears that course");

    gdx_settings_store_set("D.CRSD", &in);
    len = gdx_settings_store_encode_file(sFile, sizeof(sFile));
    CHECK(len > 0, "the store encodes");
    gdx_settings_store_clear();
    CHECK(gdx_settings_store_restore_file(sFile, (size_t) len) == GDX_SETTINGS_OK &&
              gdx_settings_store_get("D.CRSD", &out) && (memcmp(&in, &out, sizeof(in)) == 0) &&
              gdx_settings_store_get("C.CRSD", &out),
          "the file round trips");

    revision = gdx_settings_store_revision();
    sFile[len - 1] ^= 0x55;
    CHECK(gdx_settings_store_restore_file(sFile, (size_t) len) == GDX_SETTINGS_ERR_CRC, "a bad file is refused");
    CHECK(gdx_settings_store_get("D.CRSD", &out) && (gdx_settings_store_revision() == revision),
          "a bad file leaves the store alone");
    sFile[len - 1] ^= 0x55;

    /* Rename the second id onto the first, then reseal: repeated ids must be refused. */
    {
        int first = GDX_SETTINGS_FILE_HEADER_SIZE + 1;
        int blockLen = (int) sFile[first + 6] | ((int) sFile[first + 7] << 8);
        int second = first + 6 + 2 + blockLen + 1;

        memcpy(sFile + second, sFile + first, 6);
        reseal(sFile, (size_t) len);
        CHECK(gdx_settings_store_restore_file(sFile, (size_t) len) != GDX_SETTINGS_OK, "a repeated or rebound id is refused");
    }
    gdx_settings_store_clear();
    len = gdx_settings_store_encode_file(sFile, sizeof(sFile));
    CHECK((len == GDX_SETTINGS_FILE_HEADER_SIZE + 4) &&
              (gdx_settings_store_restore_file(sFile, (size_t) len) == GDX_SETTINGS_OK),
          "an empty store round trips");
}

typedef struct VisitLog {
    int count;
    char id[8][32];
    uint32_t bind[8];
    int forkCount[8];
    int lava[8];
} VisitLog;

static void visit(const char* courseId, uint32_t bindCrc, const GdxBranchSet* forks, const GdxCourseSettings* settings,
                  void* ctx) {
    VisitLog* log = (VisitLog*) ctx;

    snprintf(log->id[log->count], sizeof(log->id[0]), "%s", courseId);
    log->bind[log->count] = bindCrc;
    log->forkCount[log->count] = (forks != NULL) ? forks->forkCount : -1;
    log->lava[log->count] = (settings != NULL) ? settings->lavaMode : -1;
    log->count++;
}

static void make_forks(GdxBranchSet* set) {
    memset(set, 0, sizeof(*set));
    set->ringCount = 10;
    set->forkCount = 1;
    set->courseChecksum = 77u;
    set->forks[0].forkPoint = 2;
    set->forks[0].mergePoint = 5;
    set->forks[0].nodeCount = 1;
    set->forks[0].nodes[0].pos[0] = 1000.0f;
    set->forks[0].nodes[0].pit = -1;
}

static void test_sidecar(void) {
    GdxSidecarEntry entries[3];
    GdxBranchSet forks;
    GdxBranchSet noForks;
    GdxCourseSettings settings;
    GdxCourseSettings noSettings;
    VisitLog log;
    int len;

    make_forks(&forks);
    make_settings(&settings);
    memset(&noForks, 0, sizeof(noForks));
    memset(&noSettings, 0, sizeof(noSettings));
    memset(entries, 0, sizeof(entries));
    entries[0].courseId = "ONE.CRSD";
    entries[0].bindCrc = 111u;
    entries[0].forks = &forks;
    entries[0].settings = &settings;
    entries[1].courseId = "TWO.CRSD";
    entries[1].bindCrc = 222u;
    entries[1].forks = &noForks;
    entries[1].settings = &noSettings;
    entries[2].courseId = "THREE.CRSD";
    entries[2].bindCrc = 333u;
    entries[2].settings = &settings;

    CHECK(!gdx_course_sidecar_entry_has_data(&entries[1]) && gdx_course_sidecar_entry_has_data(&entries[2]),
          "an entry with nothing to write is told apart");
    CHECK(gdx_course_sidecar_encode(&entries[1], 1, sSidecar, sizeof(sSidecar)) == 0, "nothing to write gives no file");

    len = gdx_course_sidecar_encode(entries, 3, sSidecar, sizeof(sSidecar));
    CHECK(len > 0, "a sidecar encodes");
    memset(&log, 0, sizeof(log));
    CHECK(gdx_course_sidecar_decode(sSidecar, (size_t) len, visit, &log) == GDX_SETTINGS_OK, "a sidecar decodes");
    CHECK(log.count == 2, "the empty entry is left out");
    CHECK((strcmp(log.id[0], "ONE.CRSD") == 0) && (log.bind[0] == 111u) && (log.forkCount[0] == 1) &&
              (log.lava[0] == GDX_SETTINGS_LAVA_NEVER_KILLS),
          "an entry carries both blocks");
    CHECK((strcmp(log.id[1], "THREE.CRSD") == 0) && (log.forkCount[1] == -1) &&
              (log.lava[1] == GDX_SETTINGS_LAVA_NEVER_KILLS),
          "a missing block comes back NULL");

    /* A bad block anywhere means nothing is visited. The first entry's bind follows its id. */
    put32(sSidecar + GDX_SIDECAR_HEADER_SIZE + 1 + 8, 999u);
    reseal(sSidecar, (size_t) len);
    memset(&log, 0, sizeof(log));
    CHECK((gdx_course_sidecar_decode(sSidecar, (size_t) len, visit, &log) != GDX_SETTINGS_OK) && (log.count == 0),
          "a sidecar with a mismatched bind visits nothing");

    len = gdx_course_sidecar_encode(entries, 3, sSidecar, sizeof(sSidecar));
    sSidecar[0] = 'Z';
    CHECK(gdx_course_sidecar_decode(sSidecar, (size_t) len, visit, &log) == GDX_SETTINGS_ERR_MAGIC, "wrong magic refused");
    sSidecar[0] = 'G';
    sSidecar[len - 1] ^= 0x10;
    CHECK(gdx_course_sidecar_decode(sSidecar, (size_t) len, visit, &log) == GDX_SETTINGS_ERR_CRC, "a bad CRC is refused");
    sSidecar[len - 1] ^= 0x10;
    CHECK(gdx_course_sidecar_decode(sSidecar, 3, visit, &log) == GDX_SETTINGS_ERR_TRUNCATED, "a short file is refused");
    CHECK(gdx_course_sidecar_decode(sSidecar, (size_t) len - 1, visit, &log) != GDX_SETTINGS_OK, "a cut file is refused");
}

static void test_part_edge(void) {
    /* A road 400 wide at its narrower end: half width 200. Strips reach from the distance to the road edge. */
    CHECK(gdx_settings_view_part_edge(100.0f, 0, GDX_SETTINGS_VIEW_SHAPE_SIDE, 400.0f, 800.0f) == 100.0f,
          "no width keeps the stock distance");
    CHECK(gdx_settings_view_part_edge(100.0f, 25, GDX_SETTINGS_VIEW_SHAPE_SIDE, 400.0f, 800.0f) == 100.0f,
          "a side strip at 25 percent covers a quarter of the road, the stock pit");
    CHECK(gdx_settings_view_part_edge(100.0f, 50, GDX_SETTINGS_VIEW_SHAPE_SIDE, 800.0f, 400.0f) == 0.0f,
          "a side strip at 50 percent reaches the center line, whichever end is narrower");
    CHECK(gdx_settings_view_part_edge(100.0f, 75, GDX_SETTINGS_VIEW_SHAPE_SIDE, 400.0f, 400.0f) == -100.0f,
          "a side strip past 50 percent crosses the center line");
    CHECK(gdx_settings_view_part_edge(100.0f, 100, GDX_SETTINGS_VIEW_SHAPE_SIDE, 400.0f, 800.0f) == -5000.0f,
          "a side strip at 100 percent covers both ends of a road that widens");
    CHECK(gdx_settings_view_part_edge(100.0f, 50, GDX_SETTINGS_VIEW_SHAPE_BOTH, 400.0f, 400.0f) == 100.0f,
          "a pair at 50 percent leaves the middle half free");
    CHECK(gdx_settings_view_part_edge(100.0f, 100, GDX_SETTINGS_VIEW_SHAPE_BOTH, 400.0f, 400.0f) == 0.0f,
          "a pair at 100 percent meets on the center line");
    CHECK(gdx_settings_view_part_edge(100.0f, 50, GDX_SETTINGS_VIEW_SHAPE_MIDDLE, 400.0f, 400.0f) == 100.0f,
          "a middle strip at 50 percent covers the middle half");
    CHECK(gdx_settings_view_part_edge(100.0f, 100, GDX_SETTINGS_VIEW_SHAPE_MIDDLE, 400.0f, 400.0f) == 5000.0f,
          "a middle strip at 100 percent covers the road");
}

static void test_view(void) {
    GdxCourseSettings in;

    gdx_settings_store_clear();
    gdx_settings_set_active(NULL);
    CHECK((gdx_settings_view_pit_width(3) == 0) && (gdx_settings_view_lava_mode() == 0) &&
              (gdx_settings_view_magnet_strength(9) == 0.0f) && (gdx_settings_view_magnet_range(9) == 0.0f),
          "no active settings reads as not set");

    make_settings(&in);
    gdx_settings_store_set("V.CRSD", &in);
    gdx_settings_view_select_course("V.CRSD", 10);
    CHECK((gdx_settings_view_pit_width(3) == 100) && (gdx_settings_view_dirt_width(3) == 25) &&
              (gdx_settings_view_ice_width(3) == 0),
          "a point reads its widths");
    CHECK(gdx_settings_view_lava_mode() == GDX_SETTINGS_LAVA_NEVER_KILLS, "the track's lava mode reads");
    CHECK((fabsf(gdx_settings_view_magnet_strength(9) - 3.0f) < 0.001f) &&
              (fabsf(gdx_settings_view_magnet_range(9) - 10.0f) < 0.001f),
          "a magnet's own values win");
    CHECK((fabsf(gdx_settings_view_magnet_strength(0) - 1.5f) < 0.001f) &&
              (fabsf(gdx_settings_view_magnet_range(0) - 150.0f) < 0.001f),
          "a point without values reads the track's");
    CHECK((gdx_settings_view_pit_width(-1) == 0) && (gdx_settings_view_pit_width(10) == 0),
          "a point out of the ring reads as not set");

    gdx_settings_view_select_course("V.CRSD", 11);
    CHECK((gdx_settings_view_pit_width(3) == 0) && (gdx_settings_view_lava_mode() == GDX_SETTINGS_LAVA_NEVER_KILLS) &&
              (fabsf(gdx_settings_view_magnet_range(9) - 150.0f) < 0.001f),
          "another ring length keeps only the track's values");

    gdx_settings_view_select_course(NULL, 10);
    CHECK(gdx_settings_active() == NULL, "NULL clears the active settings");
    gdx_settings_view_select_course("MISSING.CRSD", 10);
    CHECK(gdx_settings_active() == NULL, "a course with no values clears them too");
    gdx_settings_store_clear();
}

static void test_branch_view(void) {
    GdxBranchSet forks;
    GdxCourseSettings settings;
    const int first = 256;
    const int node = first + 1;

    make_forks(&forks);
    forks.forks[0].flags |= GDX_BRANCH_FLAG_FIRST_VALUES;
    forks.forks[0].firstExtras.pitWidth = 73;
    forks.forks[0].firstExtras.magnetRange = 144;
    forks.forks[0].firstExtras.magnetStrength = 90;
    gdx_branch_set_active(&forks);
    gdx_settings_set_active(NULL);
    CHECK(gdx_settings_view_pit_width(first) == 73 && gdx_settings_view_magnet_range(first) == 144.0f &&
          fabsf(gdx_settings_view_magnet_strength(first) - 1.125f) < 0.001f,
          "independent first values work without a settings sidecar");
    CHECK(gdx_settings_view_pit_width(node) == 0, "a missing node sidecar has no custom width");
    make_settings(&settings);
    settings.nodes[0][0].pitWidth = 62;
    settings.nodes[0][0].magnetRange = 111;
    gdx_settings_set_active(&settings);
    CHECK(gdx_settings_view_pit_width(node) == 62 && gdx_settings_view_magnet_range(node) == 111.0f,
          "alternate node settings do not alias the main ring");
    CHECK(gdx_settings_view_pit_width(first + 2) == 0 && gdx_settings_view_pit_width(first + 33) == 0 &&
          gdx_settings_view_pit_width(2147483647) == 0,
          "invalid alternate settings handles are bounded");
    gdx_branch_set_active(NULL);
    gdx_settings_set_active(NULL);
}

int main(void) {
    test_branch_view();
    test_block();
    test_store();
    test_sidecar();
    test_part_edge();
    test_view();
    if (sFailures != 0) {
        printf("%d course settings tests failed\n", sFailures);
        return 1;
    }
    printf("All course settings tests passed\n");
    return 0;
}
