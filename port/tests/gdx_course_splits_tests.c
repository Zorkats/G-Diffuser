// port/tests/gdx_course_splits_tests.c: checks for the split-time module. Plain C, no framework. One PASS or FAIL line
// per check. Exits 0 only when all pass. Expected bytes and times are hand-worked, not taken from the module.

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "../gdx_course_splits.h"

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
#define VALIDATE_IS(code, text) CHECK(gdx_splits_validate(&sWork) == (code), (text))
#define DECODE_IS(bytes, code, text) CHECK(decode_out((bytes), sizeof (bytes)) == (code), (text))

// Hand-written byte vectors.
static const unsigned char kEmpty[] = {0x00, 0x00};
static const unsigned char kLapsOnly[] = {0x02, 0x00, 0x60, 0xea, 0x00, 0x00, 0xc0, 0xd4, 0x01, 0x00};
static const unsigned char kHand[] = {0x01, 0x02, 0x40, 0xe2, 0x01, 0x00, 0x40, 0x9c, 0x60, 0xea};
static const unsigned char kEqualNodes[] = {0x01, 0x02, 0xe8, 0x03, 0x00, 0x00, 0xf4, 0x01, 0x00, 0x00};
static const unsigned char kNodeAtEnd[] = {0x01, 0x01, 0xe8, 0x03, 0x00, 0x00, 0xe8, 0x03};
static const unsigned char kLapEndsFall[] = {0x02, 0x00, 0xd0, 0x07, 0x00, 0x00, 0xe8, 0x03, 0x00, 0x00};
static const unsigned char kLapTooLong[] = {0x01, 0x00, 0x80, 0xee, 0x36, 0x00};
static const unsigned char kNodePastMax[] = {0x02, 0x01, 0x98, 0xea, 0x36, 0x00, 0x7e, 0xee,
                                             0x36, 0x00, 0x01, 0x00, 0xff, 0xff};
static const unsigned char kTenLaps[] = {0x0a, 0x00};
static const unsigned char kNodes128[] = {0x01, 0x80};

static GdxCourseSplits sEmpty, sLaps, sHand, sGrid, sFull, sWork, sOut;
static GdxSplitsRecorder sRec;
static unsigned char sBuf[GDX_SPLITS_VALUE_MAX_BYTES + 2];
static unsigned char sRound[GDX_SPLITS_VALUE_MAX_BYTES + 2];
static uint32_t sRng;

static void reset_work(void) {
    memset(&sWork, 0, sizeof sWork);
}

// Sets sWork to one lap with no nodes that ends at `end` ms.
static void one_lap(uint32_t end) {
    reset_work();
    sWork.laps = 1;
    sWork.lapEnd[0] = end;
}

static int same_model(const GdxCourseSplits* a, const GdxCourseSplits* b) {
    return memcmp(a, b, sizeof *a) == 0;
}

static int encode_buf(const GdxCourseSplits* m) {
    return gdx_splits_encode(m, sBuf, sizeof sBuf);
}

static int decode_out(const unsigned char* buf, size_t len) {
    return gdx_splits_decode(buf, len, &sOut);
}

static int finish_run(void) {
    return gdx_splits_rec_finish(&sRec, &sOut);
}

// Two laps ending at 60000 and 120000 ms, no nodes.
static void fill_laps(GdxCourseSplits* m) {
    memset(m, 0, sizeof *m);
    m->laps = 2;
    m->lapEnd[0] = 60000u;
    m->lapEnd[1] = 120000u;
}

// One lap ending at 123456 ms, with nodes at 40000 and 100000 ms.
static void fill_hand(GdxCourseSplits* m) {
    memset(m, 0, sizeof *m);
    m->laps = 1;
    m->nodes = 2;
    m->lapEnd[0] = 123456u;
    m->node[0][0] = 40000u;
    m->node[0][1] = 100000u;
}

// Two laps of two nodes. Both laps have node offsets of 10000 and 40000 ms from their start.
static void fill_grid(GdxCourseSplits* m) {
    memset(m, 0, sizeof *m);
    m->laps = 2;
    m->nodes = 2;
    m->lapEnd[0] = 60000u;
    m->lapEnd[1] = 120000u;
    m->node[0][0] = 10000u;
    m->node[0][1] = 40000u;
    m->node[1][0] = 70000u;
    m->node[1][1] = 100000u;
}

// Three laps of 127 nodes. Each node is 7000 ms after the one before it.
static void fill_full(GdxCourseSplits* m) {
    int l, n;
    memset(m, 0, sizeof *m);
    m->laps = 3;
    m->nodes = GDX_SPLITS_NODES_MAX;
    for (l = 0; l < 3; l++) {
        uint32_t start = l == 0 ? 0u : m->lapEnd[l - 1];
        m->lapEnd[l] = 1000000u * (uint32_t)(l + 1);
        for (n = 0; n < GDX_SPLITS_NODES_MAX; n++) {
            m->node[l][n] = start + 7000u * (uint32_t)(n + 1);
        }
    }
}

static void check_validate(void) {
    CHECK(gdx_splits_validate(&sEmpty) == 0, "validate accepts the empty model");
    CHECK(gdx_splits_validate(&sLaps) == 0, "validate accepts two laps with no nodes");
    CHECK(gdx_splits_validate(&sHand) == 0, "validate accepts one lap with two nodes");
    CHECK(gdx_splits_validate(&sGrid) == 0, "validate accepts two laps with two nodes");
    CHECK(gdx_splits_validate(&sFull) == 0, "validate accepts three laps of 127 nodes");
    CHECK(gdx_splits_validate(NULL) == GDX_SPLITS_ERR_ARGS, "validate returns ARGS for NULL");

    reset_work(); sWork.laps = 10;
    VALIDATE_IS(GDX_SPLITS_ERR_VALUE, "validate rejects more than 9 laps");
    reset_work(); sWork.laps = -1;
    VALIDATE_IS(GDX_SPLITS_ERR_VALUE, "validate rejects a negative lap count");
    reset_work(); sWork.laps = 1; sWork.nodes = 128;
    VALIDATE_IS(GDX_SPLITS_ERR_VALUE, "validate rejects more than 127 nodes");
    reset_work(); sWork.nodes = 1;
    VALIDATE_IS(GDX_SPLITS_ERR_VALUE, "validate rejects nodes on an empty model");
    reset_work(); sWork.lapEnd[0] = 5u;
    VALIDATE_IS(GDX_SPLITS_ERR_VALUE, "validate rejects a lap end on an empty model");
    one_lap(0u);
    VALIDATE_IS(GDX_SPLITS_ERR_VALUE, "validate rejects a first lap end of zero");
    reset_work(); sWork.laps = 2; sWork.lapEnd[0] = 2000u; sWork.lapEnd[1] = 1000u;
    VALIDATE_IS(GDX_SPLITS_ERR_VALUE, "validate rejects lap ends that fall");
    sWork.lapEnd[1] = 2000u;
    VALIDATE_IS(GDX_SPLITS_ERR_VALUE, "validate rejects lap ends that repeat");
    one_lap(GDX_SPLITS_TIME_MAX + 1u);
    VALIDATE_IS(GDX_SPLITS_ERR_VALUE, "validate rejects a lap end past TIME_MAX");
    sWork.lapEnd[0] = GDX_SPLITS_TIME_MAX;
    VALIDATE_IS(0, "validate accepts a lap end at TIME_MAX");
    one_lap(1000u); sWork.lapEnd[1] = 5u;
    VALIDATE_IS(GDX_SPLITS_ERR_VALUE, "validate rejects a lap end past the lap count");

    fill_hand(&sWork);
    sWork.node[0][1] = sWork.node[0][0];
    VALIDATE_IS(GDX_SPLITS_ERR_VALUE, "validate rejects node times that repeat");
    sWork.node[0][1] = 30000u;
    VALIDATE_IS(GDX_SPLITS_ERR_VALUE, "validate rejects node times that fall");
    fill_hand(&sWork);
    sWork.node[0][0] = 0u;
    VALIDATE_IS(GDX_SPLITS_ERR_VALUE, "validate rejects a first node at time zero");
    reset_work(); sWork.laps = 2; sWork.nodes = 1; sWork.lapEnd[0] = 1000u; sWork.lapEnd[1] = 2000u;
    sWork.node[0][0] = 500u; sWork.node[1][0] = 1000u;
    VALIDATE_IS(GDX_SPLITS_ERR_VALUE, "validate rejects a node at the previous lap end");
    one_lap(1000u); sWork.nodes = 1; sWork.node[0][0] = 1000u;
    VALIDATE_IS(GDX_SPLITS_ERR_VALUE, "validate rejects a node at its lap end");
    sWork.node[0][0] = 999u;
    VALIDATE_IS(0, "validate accepts a node one millisecond before its lap end");
    sWork.node[0][1] = 5u;
    VALIDATE_IS(GDX_SPLITS_ERR_VALUE, "validate rejects a node entry past the node count");
    reset_work(); sWork.laps = 2; sWork.lapEnd[0] = 1000u; sWork.lapEnd[1] = 2000u; sWork.node[1][0] = 5u;
    VALIDATE_IS(GDX_SPLITS_ERR_VALUE, "validate rejects a node time with zero nodes");
}

static void check_codec(void) {
    size_t i;
    int n;
    int ok = 1;
    memset(sBuf, 0x5A, sizeof sBuf);
    n = encode_buf(&sLaps);
    CHECK(n == 10 && memcmp(sBuf, kLapsOnly, sizeof kLapsOnly) == 0, "encode writes the two-lap vector");
    n = encode_buf(&sHand);
    CHECK(n == 10 && memcmp(sBuf, kHand, sizeof kHand) == 0, "encode writes the one-lap two-node vector");
    n = encode_buf(&sEmpty);
    CHECK(n == 2 && memcmp(sBuf, kEmpty, sizeof kEmpty) == 0, "encode writes the empty model as two zero bytes");
    CHECK(decode_out(kHand, sizeof kHand) == 0 && same_model(&sOut, &sHand), "decode reads the one-lap vector");
    CHECK(decode_out(kEmpty, sizeof kEmpty) == 0 && same_model(&sOut, &sEmpty), "decode reads the empty record");

    n = encode_buf(&sLaps);
    CHECK(decode_out(sBuf, (size_t)n) == 0 && same_model(&sOut, &sLaps), "two-lap record round trips");
    n = encode_buf(&sFull);
    CHECK(n == 776, "full three-lap record encodes to 776 bytes");
    CHECK(decode_out(sBuf, (size_t)n) == 0 && same_model(&sOut, &sFull), "full record round trips");
    for (i = 0; i < (size_t)n; i++) {
        int want = i < 2 ? GDX_SPLITS_ERR_TRUNCATED : GDX_SPLITS_ERR_LAYOUT;
        if (decode_out(sBuf, i) != want) {
            ok = 0;
        }
    }
    CHECK(ok, "every shorter prefix of the full record fails: TRUNCATED under 2 bytes, LAYOUT above");
    CHECK(decode_out(sBuf, (size_t)n + 1) == GDX_SPLITS_ERR_LAYOUT, "decode rejects one trailing byte");
    memcpy(sBuf, kLapsOnly, sizeof kLapsOnly);
    sBuf[sizeof kLapsOnly] = 0x00;
    CHECK(decode_out(sBuf, sizeof kLapsOnly + 1) == GDX_SPLITS_ERR_LAYOUT, "decode rejects an extra byte after laps");

    fill_hand(&sWork);
    sWork.node[0][1] = sWork.node[0][0];
    CHECK(encode_buf(&sWork) == GDX_SPLITS_ERR_RANGE, "encode returns RANGE for a zero node delta");
    one_lap(100000u); sWork.nodes = 1; sWork.node[0][0] = 65536u;
    CHECK(encode_buf(&sWork) == GDX_SPLITS_ERR_RANGE, "encode returns RANGE for a node delta of 65536");
    sWork.node[0][0] = 65535u;
    CHECK(encode_buf(&sWork) == 8, "encode accepts a node delta of 65535");

    DECODE_IS(kEqualNodes, GDX_SPLITS_ERR_VALUE, "decode returns VALUE for a zero stored delta");
    DECODE_IS(kNodeAtEnd, GDX_SPLITS_ERR_VALUE, "decode returns VALUE for a node at its lap end");
    DECODE_IS(kLapEndsFall, GDX_SPLITS_ERR_VALUE, "decode returns VALUE for lap ends that fall");
    DECODE_IS(kLapTooLong, GDX_SPLITS_ERR_VALUE, "decode returns VALUE for a lap end past TIME_MAX");
    DECODE_IS(kNodePastMax, GDX_SPLITS_ERR_VALUE, "decode returns VALUE for a node past TIME_MAX");
    DECODE_IS(kTenLaps, GDX_SPLITS_ERR_LAYOUT, "decode returns LAYOUT for 10 laps");
    DECODE_IS(kNodes128, GDX_SPLITS_ERR_LAYOUT, "decode returns LAYOUT for 128 nodes");
    memset(&sOut, 0x5A, sizeof sOut);
    CHECK(decode_out(kLapTooLong, sizeof kLapTooLong) == GDX_SPLITS_ERR_VALUE && same_model(&sOut, &sEmpty),
          "decode leaves the empty model in out on failure");

    memset(sBuf, 0x5A, sizeof sBuf);
    ok = gdx_splits_encode(&sLaps, sBuf, 9) == GDX_SPLITS_ERR_FULL;
    for (i = 0; i < sizeof sBuf; i++) {
        if (sBuf[i] != 0x5A) {
            ok = 0;
        }
    }
    CHECK(ok, "encode returns FULL and leaves the output untouched when cap is one byte short");
    CHECK(gdx_splits_encode(NULL, sBuf, sizeof sBuf) == GDX_SPLITS_ERR_ARGS, "encode returns ARGS for a NULL model");
    CHECK(gdx_splits_encode(&sLaps, NULL, sizeof sBuf) == GDX_SPLITS_ERR_ARGS, "encode returns ARGS for NULL out");
    CHECK(gdx_splits_decode(NULL, 2, &sOut) == GDX_SPLITS_ERR_ARGS, "decode returns ARGS for a NULL input");
    CHECK(gdx_splits_decode(kEmpty, sizeof kEmpty, NULL) == GDX_SPLITS_ERR_ARGS, "decode returns ARGS for NULL out");
}

static void check_recorder(void) {
    int i;
    CHECK(gdx_splits_rec_finish(NULL, &sOut) == GDX_SPLITS_ERR_ARGS, "recorder finish returns ARGS for NULL");

    gdx_splits_rec_begin(&sRec, 2);
    gdx_splits_rec_node(&sRec, 0, 1000u);
    gdx_splits_rec_node(&sRec, 1, 2500u);
    gdx_splits_rec_lap(&sRec, 4000u);
    gdx_splits_rec_node(&sRec, 0, 5000u);
    gdx_splits_rec_node(&sRec, 1, 6200u);
    gdx_splits_rec_lap(&sRec, 7000u);
    CHECK(finish_run() == 0 && sOut.laps == 2 && sOut.nodes == 2, "recorder finishes a two-lap run with two nodes");
    CHECK(sOut.lapEnd[0] == 4000u && sOut.lapEnd[1] == 7000u && sOut.node[0][0] == 1000u &&
              sOut.node[0][1] == 2500u && sOut.node[1][0] == 5000u && sOut.node[1][1] == 6200u,
          "recorder stores every lap end and node time");

    gdx_splits_rec_begin(&sRec, 2);
    gdx_splits_rec_node(&sRec, 0, 1000u);
    gdx_splits_rec_node(&sRec, 0, 1500u);
    gdx_splits_rec_node(&sRec, 1, 2500u);
    gdx_splits_rec_node(&sRec, 0, 9000u);
    gdx_splits_rec_lap(&sRec, 4000u);
    CHECK(finish_run() == 0 && sOut.node[0][0] == 1000u && sOut.node[0][1] == 2500u,
          "repeated and lower node indexes are ignored");

    gdx_splits_rec_begin(&sRec, 2);
    gdx_splits_rec_node(&sRec, 1, 1000u);
    gdx_splits_rec_lap(&sRec, 4000u);
    CHECK(finish_run() == GDX_SPLITS_ERR_VALUE, "a skipped node fails the run");
    gdx_splits_rec_begin(&sRec, 2);
    gdx_splits_rec_node(&sRec, 2, 1000u);
    CHECK(finish_run() == GDX_SPLITS_ERR_VALUE, "a node index out of range fails the run");
    gdx_splits_rec_begin(&sRec, 2);
    gdx_splits_rec_node(&sRec, 0, 2000u);
    gdx_splits_rec_node(&sRec, 1, 1500u);
    gdx_splits_rec_lap(&sRec, 4000u);
    CHECK(finish_run() == GDX_SPLITS_ERR_VALUE, "a node time that goes backwards fails the run");
    gdx_splits_rec_begin(&sRec, 2);
    gdx_splits_rec_node(&sRec, 0, 1000u);
    gdx_splits_rec_node(&sRec, 1, 1000u);
    gdx_splits_rec_lap(&sRec, 4000u);
    CHECK(finish_run() == GDX_SPLITS_ERR_VALUE, "equal node times fail the run");
    gdx_splits_rec_begin(&sRec, 2);
    gdx_splits_rec_node(&sRec, 0, 1000u);
    gdx_splits_rec_lap(&sRec, 5000u);
    CHECK(finish_run() == GDX_SPLITS_ERR_VALUE, "a lap closed before all nodes fails the run");
    gdx_splits_rec_begin(&sRec, 0);
    gdx_splits_rec_lap(&sRec, 5000u);
    gdx_splits_rec_lap(&sRec, 4000u);
    CHECK(finish_run() == GDX_SPLITS_ERR_VALUE, "a lap end that goes backwards fails the run");
    gdx_splits_rec_begin(&sRec, 0);
    gdx_splits_rec_lap(&sRec, GDX_SPLITS_TIME_MAX + 1u);
    CHECK(finish_run() == GDX_SPLITS_ERR_VALUE, "a time past TIME_MAX fails the run");
    gdx_splits_rec_begin(&sRec, 0);
    CHECK(finish_run() == GDX_SPLITS_ERR_LAYOUT, "finish with no closed lap fails");
    gdx_splits_rec_begin(&sRec, 128);
    CHECK(finish_run() == GDX_SPLITS_ERR_VALUE, "more than 127 nodes fails the run");

    gdx_splits_rec_begin(&sRec, 0);
    for (i = 1; i <= 9; i++) {
        gdx_splits_rec_lap(&sRec, 1000u * (uint32_t)i);
    }
    CHECK(finish_run() == 0 && sOut.laps == 9, "nine laps are accepted");
    gdx_splits_rec_lap(&sRec, 10000u);
    CHECK(finish_run() == GDX_SPLITS_ERR_VALUE, "a tenth lap fails the run");

    gdx_splits_rec_begin(&sRec, 1);
    gdx_splits_rec_node(&sRec, 0, 1000u);
    gdx_splits_rec_lap(&sRec, 2000u);
    gdx_splits_rec_node(&sRec, 0, 2500u);
    CHECK(finish_run() == 0 && sOut.laps == 1, "an open lap is left out of the finished record");
}

static int near(double a, double b) {
    return fabs(a - b) < 1e-9;
}

static void check_track(void) {
    static const float kTwo[] = {0.25f, 0.5f};
    static const float kLate[] = {0.9f};
    static const float kClose[] = {0.5f, 0.5001f};

    gdx_splits_rec_begin(&sRec, 2);
    gdx_splits_rec_track(&sRec, kTwo, 0.0, 1000u, 0.2, 2000u);
    CHECK(sRec.nextNode == 0, "track: a frame short of the first node records nothing");
    gdx_splits_rec_track(&sRec, kTwo, 0.2, 2000u, 0.3, 3000u);
    gdx_splits_rec_track(&sRec, kTwo, 0.3, 3000u, 0.2, 3100u);
    CHECK(sRec.nextNode == 1, "track: a frame that goes backwards records nothing");
    gdx_splits_rec_track(&sRec, kTwo, 0.2, 3100u, 0.6, 4100u);
    gdx_splits_rec_lap(&sRec, 5000u);
    /* 0.25 is halfway from 0.2 to 0.3, so 2500. 0.5 is 0.3 / 0.4 of the way from 0.2 to 0.6, so 3100 + 750. */
    CHECK(finish_run() == 0 && sOut.node[0][0] == 2500u && sOut.node[0][1] == 3850u,
          "track: node times are interpolated between frames");

    gdx_splits_rec_begin(&sRec, 2);
    gdx_splits_rec_track(&sRec, kTwo, 0.0, 1000u, 0.6, 1600u);
    gdx_splits_rec_lap(&sRec, 2000u);
    CHECK(finish_run() == 0 && sOut.node[0][0] == 1250u && sOut.node[0][1] == 1500u,
          "track: two nodes passed in one frame are both recorded");

    /* A frame that crosses the line: the old lap runs past 1, the new lap starts below 0. */
    gdx_splits_rec_begin(&sRec, 1);
    gdx_splits_rec_track(&sRec, kLate, 0.8, 1000u, 1.1, 1300u);
    gdx_splits_rec_lap(&sRec, 1200u);
    gdx_splits_rec_track(&sRec, kLate, -0.2, 1000u, 0.1, 1300u);
    CHECK(sRec.nextNode == 0, "track: the new lap's part of a line frame records nothing before its nodes");
    gdx_splits_rec_track(&sRec, kLate, 0.1, 1300u, 0.95, 2150u);
    gdx_splits_rec_lap(&sRec, 2300u);
    CHECK(finish_run() == 0 && sOut.node[0][0] == 1100u && sOut.lapEnd[0] == 1200u && sOut.node[1][0] == 2100u,
          "track: a line frame split in two calls records both laps");

    gdx_splits_rec_begin(&sRec, 2);
    gdx_splits_rec_track(&sRec, kClose, 0.0, 1000u, 1.0, 1002u);
    CHECK(finish_run() == GDX_SPLITS_ERR_VALUE, "track: two nodes rounding to the same millisecond fail the run");

    gdx_splits_rec_begin(&sRec, 2);
    gdx_splits_rec_track(&sRec, NULL, 0.0, 1000u, 0.6, 1600u);
    gdx_splits_rec_track(NULL, kTwo, 0.0, 1000u, 0.6, 1600u);
    CHECK(sRec.nextNode == 0 && !sRec.failed, "track: a NULL recorder or node list is ignored");
    gdx_splits_rec_begin(&sRec, 0);
    gdx_splits_rec_track(&sRec, kTwo, 0.0, 1000u, 0.6, 1600u);
    gdx_splits_rec_lap(&sRec, 2000u);
    CHECK(finish_run() == 0 && sOut.nodes == 0, "track: a lap-only run ignores node positions");
}

static void check_pace(void) {
    static const float kGrid[] = {0.25f, 0.5f};
    static const float kEven[] = {0.25f, 0.5f, 0.75f};
    static const float kFalling[] = {0.5f, 0.25f};
    static const float kAtEnd[] = {0.25f, 1.0f};
    float fullAt[GDX_SPLITS_NODES_MAX];
    double laps = -1.0, last;
    int i, ok;

    /* No nodes: steady inside each lap. */
    CHECK(gdx_splits_pace_laps(&sLaps, NULL, 2, 0, 0.25, &laps) == 0 && near(laps, 0.5),
          "pace: a quarter of two even laps is half a lap");
    CHECK(gdx_splits_pace_laps(&sLaps, NULL, 3, 1, 0.5, &laps) == 0 && near(laps, 2.0),
          "pace: a repeated third lap keeps the last lap's time");
    reset_work();
    sWork.laps = 2;
    sWork.lapEnd[0] = 40000u;
    sWork.lapEnd[1] = 120000u;
    /* Half of 120000 is 60000: 20000 into an 80000 ms second lap. */
    CHECK(gdx_splits_pace_laps(&sWork, NULL, 2, 0, 0.5, &laps) == 0 && near(laps, 1.25),
          "pace: per lap, a slow second lap holds the pace back");

    /* Nodes at a quarter and half of the lap, passed at 10000 and 40000 of 60000 ms. */
    CHECK(gdx_splits_pace_laps(&sGrid, kGrid, 2, 0, 10000.0 / 120000.0, &laps) == 0 && near(laps, 0.25),
          "pace: at node 0's time the run is at node 0");
    CHECK(gdx_splits_pace_laps(&sGrid, kGrid, 2, 0, 25000.0 / 120000.0, &laps) == 0 && near(laps, 0.375),
          "pace: halfway between two nodes is halfway between their positions");
    CHECK(gdx_splits_pace_laps(&sGrid, kGrid, 2, 0, 50000.0 / 120000.0, &laps) == 0 && near(laps, 0.75),
          "pace: after the last node the run heads for the line");
    CHECK(gdx_splits_pace_laps(&sGrid, kGrid, 2, 0, 70000.0 / 120000.0, &laps) == 0 && near(laps, 1.25),
          "pace: the second lap uses its own node times");
    /* Window from 60000 to 180000: 130000 is node 0 of a repeated third lap. */
    CHECK(gdx_splits_pace_laps(&sGrid, kGrid, 3, 1, 70000.0 / 120000.0, &laps) == 0 && near(laps, 2.25),
          "pace: a repeated lap repeats the last lap's node offsets");
    /* A one-lap race on a two-lap record: half of 60000 is 20000 past node 0, of the 30000 to node 1. */
    CHECK(gdx_splits_pace_laps(&sGrid, kGrid, 1, 0, 0.5, &laps) == 0 && near(laps, 0.25 + 0.25 * (20000.0 / 30000.0)),
          "pace: a race shorter than the record ends at its own lap");

    /* Nodes even in time and distance give the stock straight line. */
    reset_work();
    sWork.laps = 1;
    sWork.nodes = 3;
    sWork.lapEnd[0] = 40000u;
    sWork.node[0][0] = 10000u;
    sWork.node[0][1] = 20000u;
    sWork.node[0][2] = 30000u;
    ok = 1;
    for (i = 1; i < 100; i++) {
        const double share = i / 100.0;
        if (gdx_splits_pace_laps(&sWork, kEven, 3, 1, share, &laps) != 0 || !near(laps, 1.0 + 2.0 * share)) {
            ok = 0;
        }
    }
    CHECK(ok, "pace: an even run matches the stock straight line");

    CHECK(gdx_splits_pace_laps(&sLaps, NULL, 3, 1, 0.0, &laps) == 0 && near(laps, 1.0), "pace: share 0 is the window start");
    CHECK(gdx_splits_pace_laps(&sLaps, NULL, 3, 1, 1.0, &laps) == 0 && near(laps, 3.0), "pace: share 1 is the finish");
    CHECK(gdx_splits_pace_laps(&sLaps, NULL, 3, 1, 1.5, &laps) == 0 && near(laps, 4.0),
          "pace: past the finish the window's average pace runs on");
    CHECK(gdx_splits_pace_laps(&sLaps, NULL, 3, 1, -0.5, &laps) == 0 && near(laps, 0.0),
          "pace: before the window the average pace runs back");

    for (i = 0; i < GDX_SPLITS_NODES_MAX; i++) {
        fullAt[i] = (float)(i + 1) / 129.0f;
    }
    ok = 1;
    last = 0.0;
    for (i = 0; i <= 1000; i++) {
        if (gdx_splits_pace_laps(&sFull, fullAt, 5, 0, i / 1000.0, &laps) != 0 || laps < last - 1e-12 || laps > 5.0) {
            ok = 0;
        }
        last = laps;
    }
    CHECK(ok && near(last, 5.0), "pace: a full record paces forward from the start to the finish");

    CHECK(gdx_splits_pace_laps(NULL, NULL, 2, 0, 0.5, &laps) == GDX_SPLITS_ERR_ARGS, "pace: a NULL model is ARGS");
    CHECK(gdx_splits_pace_laps(&sLaps, NULL, 2, 0, 0.5, NULL) == GDX_SPLITS_ERR_ARGS, "pace: a NULL out is ARGS");
    CHECK(gdx_splits_pace_laps(&sEmpty, NULL, 2, 0, 0.5, &laps) == GDX_SPLITS_ERR_VALUE, "pace: the empty model");
    one_lap(0u);
    CHECK(gdx_splits_pace_laps(&sWork, NULL, 2, 0, 0.5, &laps) == GDX_SPLITS_ERR_VALUE, "pace: an invalid model");
    CHECK(gdx_splits_pace_laps(&sGrid, NULL, 2, 0, 0.5, &laps) == GDX_SPLITS_ERR_VALUE, "pace: nodes need positions");
    CHECK(gdx_splits_pace_laps(&sGrid, kFalling, 2, 0, 0.5, &laps) == GDX_SPLITS_ERR_VALUE,
          "pace: falling node positions");
    CHECK(gdx_splits_pace_laps(&sGrid, kAtEnd, 2, 0, 0.5, &laps) == GDX_SPLITS_ERR_VALUE,
          "pace: a node position at the line");
    CHECK(gdx_splits_pace_laps(&sLaps, NULL, 0, 0, 0.5, &laps) == GDX_SPLITS_ERR_VALUE, "pace: zero laps");
    CHECK(gdx_splits_pace_laps(&sLaps, NULL, 256, 0, 0.5, &laps) == GDX_SPLITS_ERR_VALUE, "pace: 256 laps");
    CHECK(gdx_splits_pace_laps(&sLaps, NULL, 3, 3, 0.5, &laps) == GDX_SPLITS_ERR_VALUE, "pace: a window from the finish");
    CHECK(gdx_splits_pace_laps(&sLaps, NULL, 3, -1, 0.5, &laps) == GDX_SPLITS_ERR_VALUE, "pace: a negative window");
    CHECK(gdx_splits_pace_laps(&sLaps, NULL, 3, 1, NAN, &laps) == GDX_SPLITS_ERR_VALUE, "pace: a NaN share");
    CHECK(gdx_splits_pace_laps(&sLaps, NULL, 3, 1, INFINITY, &laps) == GDX_SPLITS_ERR_VALUE, "pace: an endless share");
}

static uint32_t next_rand(void) {
    sRng ^= sRng << 13;
    sRng ^= sRng >> 17;
    sRng ^= sRng << 5;
    return sRng;
}

// Writes a random valid model: 0..9 laps and 0..127 nodes, lap durations of 100000..199999 ms, node gaps within u16.
static void make_structured(GdxCourseSplits* m) {
    uint32_t start = 0u;
    int l, n;
    memset(m, 0, sizeof *m);
    m->laps = (int)(next_rand() % (uint32_t)(GDX_SPLITS_LAPS_MAX + 1));
    if (m->laps > 0) {
        m->nodes = (int)(next_rand() % (uint32_t)(GDX_SPLITS_NODES_MAX + 1));
    }
    for (l = 0; l < m->laps; l++) {
        uint32_t dur = 100000u + next_rand() % 100000u;
        uint32_t gap = (dur - 1u) / (uint32_t)(m->nodes + 1);
        uint32_t t = start;
        if (gap > 65535u) {
            gap = 65535u;
        }
        for (n = 0; n < m->nodes; n++) {
            t += 1u + next_rand() % gap;
            m->node[l][n] = t;
        }
        start += dur;
        m->lapEnd[l] = start;
    }
}

static void fuzz_one(const unsigned char* buf, size_t len, int* decoded, int* bad) {
    int n;
    if (gdx_splits_decode(buf, len, &sOut) != 0) {
        if (!same_model(&sOut, &sEmpty)) {
            (*bad)++;
        }
        return;
    }
    (*decoded)++;
    n = gdx_splits_encode(&sOut, sRound, sizeof sRound);
    if (gdx_splits_validate(&sOut) != 0 || n != (int)len || memcmp(sRound, buf, len) != 0) {
        (*bad)++;
    }
}

static void check_fuzz(void) {
    int i, decoded = 0, bad = 0;
    size_t len, k;
    sRng = 0x2545F491u;
    for (i = 0; i < 4000; i++) {
        if (i % 2 == 0) {
            len = next_rand() % 64u;
            for (k = 0; k < len; k++) {
                sBuf[k] = (unsigned char)next_rand();
            }
        } else {
            make_structured(&sWork);
            len = (size_t)encode_buf(&sWork);
            for (k = next_rand() % 3u; k > 0; k--) {
                sBuf[next_rand() % len] = (unsigned char)next_rand();
            }
            if (next_rand() % 8u == 0u) {
                len--;
            }
        }
        fuzz_one(sBuf, len, &decoded, &bad);
    }
    CHECK(bad == 0, "fuzz: each failed decode leaves out empty, each success re-encodes to the same bytes");
    CHECK(decoded > 100, "fuzz: enough inputs decode to exercise the success path");
}

int main(void) {
    fill_laps(&sLaps);
    fill_hand(&sHand);
    fill_grid(&sGrid);
    fill_full(&sFull);
    check_validate();
    check_codec();
    check_recorder();
    check_track();
    check_pace();
    check_fuzz();
    if (sFailures == 0) {
        printf("all checks passed: 0 failure(s)\n");
        return 0;
    }
    printf("checks failed: %d failure(s)\n", sFailures);
    return 1;
}
