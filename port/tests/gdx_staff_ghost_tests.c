/* port/tests/gdx_staff_ghost_tests.c -- the staff ghost codec (gdx_staff_ghost.h). */

#include "../gdx_staff_ghost.h"

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

static GdxStaffGhost sGhost;
static GdxStaffGhost sOut;
static unsigned char sBytes[GDX_STAFF_GHOST_VALUE_MAX_BYTES + 16];

static void make_ghost(GdxStaffGhost* g, uint16_t replaySize) {
    int i;

    memset(g, 0, sizeof(*g));
    g->raceTime = 61234;
    g->lapTimes[0] = 21000;
    g->lapTimes[1] = 20100;
    g->lapTimes[2] = 20134;
    g->replaySize = replaySize;
    g->replayEnd = replaySize - 1;
    g->machine[GDX_STAFF_GHOST_MACHINE_CHARACTER] = 29;
    g->machine[GDX_STAFF_GHOST_MACHINE_CUSTOM_TYPE] = 1;
    g->machine[GDX_STAFF_GHOST_MACHINE_FRONT] = 6;
    g->machine[GDX_STAFF_GHOST_MACHINE_REAR] = 6;
    g->machine[GDX_STAFF_GHOST_MACHINE_WING] = 6;
    g->machine[GDX_STAFF_GHOST_MACHINE_LOGO] = 7;
    g->machine[GDX_STAFF_GHOST_MACHINE_NUMBER] = 30;
    g->machine[GDX_STAFF_GHOST_MACHINE_DECAL] = 4;
    for (i = 8; i < GDX_STAFF_GHOST_MACHINE_BYTES; i++) {
        g->machine[i] = (unsigned char) (0xF0 + i); /* colours take any value */
    }
    for (i = 0; i < replaySize; i++) {
        g->replay[i] = (unsigned char) (i * 7 + 3);
    }
}

static int ghost_equal(const GdxStaffGhost* a, const GdxStaffGhost* b) {
    return (a->raceTime == b->raceTime) && (memcmp(a->lapTimes, b->lapTimes, sizeof(a->lapTimes)) == 0) &&
           (a->replayEnd == b->replayEnd) && (a->replaySize == b->replaySize) &&
           (memcmp(a->machine, b->machine, sizeof(a->machine)) == 0) &&
           (memcmp(a->replay, b->replay, a->replaySize) == 0);
}

static int all_zero(const void* p, size_t n) {
    const unsigned char* b = (const unsigned char*) p;
    size_t i;

    for (i = 0; i < n; i++) {
        if (b[i] != 0) {
            return 0;
        }
    }
    return 1;
}

static void test_roundtrip(void) {
    uint32_t raceTime = 0;
    int len;

    make_ghost(&sGhost, 900);
    len = gdx_staff_ghost_encode(&sGhost, sBytes, sizeof(sBytes));
    CHECK(len == GDX_STAFF_GHOST_HEADER_BYTES + 900, "the stored form is the header plus the replay");
    CHECK(GDX_STAFF_GHOST_HEADER_BYTES == 0x2A, "the header is 42 bytes");
    CHECK((sBytes[0] == 0x32) && (sBytes[1] == 0xEF) && (sBytes[2] == 0) && (sBytes[3] == 0),
          "the race time is little-endian");
    CHECK((sBytes[0x14] == (900 & 0xFF)) && (sBytes[0x15] == (900 >> 8)), "the replay size sits at 0x14");
    CHECK(sBytes[0x16] == 29, "the machine starts at 0x16");
    CHECK((gdx_staff_ghost_check(sBytes, (size_t) len, &raceTime) == GDX_STAFF_GHOST_OK) && (raceTime == 61234),
          "check reads the race time in place");
    CHECK((gdx_staff_ghost_decode(sBytes, (size_t) len, &sOut) == GDX_STAFF_GHOST_OK) && ghost_equal(&sGhost, &sOut),
          "decode gives back what was encoded");

    make_ghost(&sGhost, GDX_STAFF_GHOST_REPLAY_MAX);
    len = gdx_staff_ghost_encode(&sGhost, sBytes, sizeof(sBytes));
    CHECK(len == GDX_STAFF_GHOST_VALUE_MAX_BYTES, "a full 16200-byte replay encodes at the largest size");
    CHECK((gdx_staff_ghost_decode(sBytes, (size_t) len, &sOut) == GDX_STAFF_GHOST_OK) && ghost_equal(&sGhost, &sOut),
          "a full replay reads back");
    CHECK(gdx_staff_ghost_encode(&sGhost, sBytes, (size_t) len - 1) == GDX_STAFF_GHOST_ERR_FULL,
          "a buffer one byte short is refused");
}

static void test_layout(void) {
    int len;

    make_ghost(&sGhost, 100);
    len = gdx_staff_ghost_encode(&sGhost, sBytes, sizeof(sBytes));
    CHECK(gdx_staff_ghost_check(sBytes, (size_t) len - 1, NULL) == GDX_STAFF_GHOST_ERR_LAYOUT,
          "a stored form one byte short of its replay size is refused");
    CHECK(gdx_staff_ghost_check(sBytes, (size_t) len + 1, NULL) == GDX_STAFF_GHOST_ERR_LAYOUT,
          "a stored form with a trailing byte is refused");
    CHECK(gdx_staff_ghost_check(sBytes, GDX_STAFF_GHOST_HEADER_BYTES - 1, NULL) == GDX_STAFF_GHOST_ERR_LAYOUT,
          "a stored form shorter than the header is refused");
    CHECK(gdx_staff_ghost_check(NULL, 0, NULL) == GDX_STAFF_GHOST_ERR_ARGS, "NULL data is refused");
    memset(&sOut, 0x55, sizeof(sOut));
    CHECK((gdx_staff_ghost_decode(sBytes, (size_t) len - 1, &sOut) == GDX_STAFF_GHOST_ERR_LAYOUT) &&
              all_zero(&sOut, sizeof(sOut)),
          "a failed decode leaves out zeroed");
}

/* Encodes sGhost after a change, expecting it to be refused, and checks that a stored form with the same change
 * is refused by check too. */
static void expect_refused(const char* text) {
    GdxStaffGhost good;
    int len;

    CHECK(gdx_staff_ghost_validate(&sGhost) == GDX_STAFF_GHOST_ERR_VALUE, text);
    CHECK(gdx_staff_ghost_encode(&sGhost, sBytes, sizeof(sBytes)) == GDX_STAFF_GHOST_ERR_VALUE, text);
    /* The same bytes forged past the encoder. */
    good = sGhost;
    make_ghost(&sGhost, 100);
    len = gdx_staff_ghost_encode(&sGhost, sBytes, sizeof(sBytes));
    sBytes[0] = (unsigned char) good.raceTime;
    sBytes[1] = (unsigned char) (good.raceTime >> 8);
    sBytes[2] = (unsigned char) (good.raceTime >> 16);
    sBytes[3] = (unsigned char) (good.raceTime >> 24);
    sBytes[4] = (unsigned char) good.lapTimes[0];
    sBytes[5] = (unsigned char) (good.lapTimes[0] >> 8);
    sBytes[6] = (unsigned char) (good.lapTimes[0] >> 16);
    sBytes[7] = (unsigned char) (good.lapTimes[0] >> 24);
    sBytes[0x10] = (unsigned char) good.replayEnd;
    sBytes[0x11] = (unsigned char) (good.replayEnd >> 8);
    sBytes[0x12] = (unsigned char) (good.replayEnd >> 16);
    sBytes[0x13] = (unsigned char) (good.replayEnd >> 24);
    memcpy(sBytes + 0x16, good.machine, GDX_STAFF_GHOST_MACHINE_BYTES);
    CHECK(gdx_staff_ghost_check(sBytes, (size_t) len, NULL) == GDX_STAFF_GHOST_ERR_VALUE, text);
}

static void test_limits(void) {
    static const struct {
        int offset;
        unsigned char value;
        const char* text;
    } kMachine[] = {
        { GDX_STAFF_GHOST_MACHINE_CHARACTER, 30, "a 31st machine is refused" },
        { GDX_STAFF_GHOST_MACHINE_CUSTOM_TYPE, 5, "a custom type past the Super Cat is refused" },
        { GDX_STAFF_GHOST_MACHINE_FRONT, 7, "an eighth front part is refused" },
        { GDX_STAFF_GHOST_MACHINE_REAR, 7, "an eighth rear part is refused" },
        { GDX_STAFF_GHOST_MACHINE_WING, 7, "an eighth wing is refused" },
        { GDX_STAFF_GHOST_MACHINE_LOGO, 8, "a ninth logo is refused" },
        { GDX_STAFF_GHOST_MACHINE_NUMBER, 31, "a 32nd number is refused" },
        { GDX_STAFF_GHOST_MACHINE_DECAL, 5, "a sixth decal is refused" },
    };
    size_t i;

    make_ghost(&sGhost, 100);
    CHECK(gdx_staff_ghost_validate(&sGhost) == GDX_STAFF_GHOST_OK, "every machine byte at its largest value passes");
    CHECK(gdx_staff_ghost_validate(NULL) == GDX_STAFF_GHOST_ERR_ARGS, "a NULL ghost is refused");

    make_ghost(&sGhost, 100);
    sGhost.raceTime = 0;
    sGhost.lapTimes[0] = 0;
    expect_refused("a race time of 0 is refused");
    make_ghost(&sGhost, 100);
    sGhost.raceTime = GDX_STAFF_GHOST_TIME_MAX + 1;
    expect_refused("a race time at the game's timer limit is refused");
    make_ghost(&sGhost, 100);
    sGhost.lapTimes[0] = 0;
    expect_refused("a lap of 0 ms is refused");
    make_ghost(&sGhost, 100);
    sGhost.lapTimes[0] = sGhost.raceTime + 1;
    expect_refused("a lap longer than the race is refused");
    make_ghost(&sGhost, 100);
    sGhost.replayEnd = 101;
    expect_refused("a replay end past the replay size is refused");
    for (i = 0; i < sizeof(kMachine) / sizeof(kMachine[0]); i++) {
        make_ghost(&sGhost, 100);
        sGhost.machine[kMachine[i].offset] = kMachine[i].value;
        expect_refused(kMachine[i].text);
    }

    make_ghost(&sGhost, 100);
    sGhost.replaySize = 0;
    sGhost.replayEnd = 0;
    CHECK(gdx_staff_ghost_validate(&sGhost) == GDX_STAFF_GHOST_ERR_VALUE, "an empty replay is refused");
    make_ghost(&sGhost, 100);
    sGhost.replaySize = GDX_STAFF_GHOST_REPLAY_MAX + 1;
    CHECK(gdx_staff_ghost_validate(&sGhost) == GDX_STAFF_GHOST_ERR_VALUE, "a replay past 16200 bytes is refused");
    {
        /* A forged size past the buffer, with the bytes to match. */
        unsigned char big[GDX_STAFF_GHOST_HEADER_BYTES + GDX_STAFF_GHOST_REPLAY_MAX + 1];
        int len;

        make_ghost(&sGhost, 100);
        len = gdx_staff_ghost_encode(&sGhost, big, sizeof(big));
        CHECK(len > 0, "a small ghost encodes into a large buffer");
        big[0x14] = (unsigned char) ((GDX_STAFF_GHOST_REPLAY_MAX + 1) & 0xFF);
        big[0x15] = (unsigned char) ((GDX_STAFF_GHOST_REPLAY_MAX + 1) >> 8);
        CHECK(gdx_staff_ghost_check(big, sizeof(big), NULL) == GDX_STAFF_GHOST_ERR_VALUE,
              "a stored replay size past 16200 is refused even when the bytes are there");
    }
}

int main(void) {
    test_roundtrip();
    test_layout();
    test_limits();
    if (sFailures != 0) {
        printf("%d check(s) failed\n", sFailures);
        return 1;
    }
    printf("all checks passed\n");
    return 0;
}
