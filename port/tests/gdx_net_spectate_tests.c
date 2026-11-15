/* port/tests/gdx_net_spectate_tests.c -- unit tests for WATCH in a room's race (gdx_net_spectate.h).
 * Copyright (c) Zorkats
 *
 * Standalone: links gdx_net_spectate.c only. The race is the two functions below: a field of humans the test
 * sets up, and their names. CPUs are never in the field, as the bridge never lists them. Expected values are
 * written out by hand from the header.
 *
 * The module keeps its state between calls, so every test starts from a reset.
 */

#include "../gdx_net_spectate.h"

#include <stdio.h>
#include <string.h>

static int sFailures;
static int sChecks;

#define CHECK(cond)                                                          \
    do {                                                                     \
        sChecks++;                                                           \
        if (!(cond)) {                                                       \
            sFailures++;                                                     \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);           \
        }                                                                    \
    } while (0)

static GdxNetSpectateField sField;
static int sInRace;

int gdx_net_race_spectate_field(GdxNetSpectateField* field) {
    if (!sInRace) {
        return 0;
    }
    *field = sField;
    return 1;
}

void gdx_net_race_spectate_name(int racer, char* out, int size) {
    if (racer == 7) {
        snprintf(out, (size_t) size, "PLAYER 3"); /* a human the room has no name for */
    } else {
        snprintf(out, (size_t) size, "RACER %d", racer);
    }
}

/* A race of humans: this copy is racer 0. Each human is its racer id, race position and whether it drives. */
static void field(int count, const int (*humans)[3]) {
    int i;

    memset(&sField, 0, sizeof(sField));
    sField.count = count;
    sField.self = -1;
    for (i = 0; i < count; i++) {
        sField.human[i].racer = humans[i][0];
        sField.human[i].position = humans[i][1];
        sField.human[i].driving = humans[i][2];
        if (humans[i][0] == 0) {
            sField.self = i;
        }
    }
    sInRace = 1;
    gdx_net_spectate_reset();
}

static void set_driving(int racer, int driving) {
    int i;

    for (i = 0; i < sField.count; i++) {
        if (sField.human[i].racer == racer) {
            sField.human[i].driving = driving;
        }
    }
}

static void idle(void) {
    gdx_net_spectate_frame(0, 0, 0);
}

/* Four humans, places 1 to 4 by racer 5, 0 (us, out), 7 and 9. */
static const int kFour[4][3] = { { 5, 1, 1 }, { 0, 2, 0 }, { 7, 3, 1 }, { 9, 4, 1 } };

static void test_off_until_watch(void) {
    char label[GDX_NET_SPECTATE_LABEL_BYTES];

    field(4, kFour);
    CHECK(!gdx_net_spectate_on());
    CHECK(gdx_net_spectate_racer(0) == 0);
    CHECK(gdx_net_spectate_racer(2) == 2);
    CHECK(!gdx_net_spectate_cinematic());
    gdx_net_spectate_label(label, sizeof(label));
    CHECK(label[0] == '\0');
    CHECK(strcmp(gdx_net_spectate_hint(), "") == 0);
    /* A frame, presses and all, does nothing while off. */
    gdx_net_spectate_frame(1, 0, 1);
    CHECK(!gdx_net_spectate_on() && !gdx_net_spectate_cinematic());
}

static void test_watch_picks_the_nearest_ahead(void) {
    unsigned generation;

    field(4, kFour);
    generation = gdx_net_spectate_generation();
    CHECK(gdx_net_spectate_watch() == 1);
    CHECK(gdx_net_spectate_on());
    CHECK(gdx_net_spectate_racer(0) == 5);
    /* Only camera 0 follows the watched racer. */
    CHECK(gdx_net_spectate_racer(1) == 1);
    CHECK(!gdx_net_spectate_cinematic());
    CHECK(gdx_net_spectate_generation() != generation);
    /* WATCH again keeps the one being watched. */
    generation = gdx_net_spectate_generation();
    CHECK(gdx_net_spectate_watch() == 1);
    CHECK(gdx_net_spectate_racer(0) == 5);
    CHECK(gdx_net_spectate_generation() == generation);
}

static void test_watch_picks_the_nearest_behind(void) {
    static const int kLeader[3][3] = { { 0, 1, 0 }, { 4, 2, 0 }, { 6, 3, 1 } };

    /* We led and 4 behind us finished too: the nearest who drives is 6. */
    field(3, kLeader);
    CHECK(gdx_net_spectate_watch() == 1);
    CHECK(gdx_net_spectate_racer(0) == 6);
}

static void test_watch_with_nobody_driving(void) {
    static const int kDone[3][3] = { { 3, 1, 0 }, { 0, 2, 0 }, { 8, 3, 0 } };
    static const int kAlone[1][3] = { { 0, 1, 0 } };

    /* Everyone is done: the nearest other human, ahead first. */
    field(3, kDone);
    CHECK(gdx_net_spectate_watch() == 1);
    CHECK(gdx_net_spectate_racer(0) == 3);
    /* A room of one: nobody to watch. */
    field(1, kAlone);
    CHECK(gdx_net_spectate_watch() == 0);
    CHECK(!gdx_net_spectate_on());
    CHECK(gdx_net_spectate_racer(0) == 0);
    /* Outside a room's race nothing is watched. */
    field(4, kFour);
    sInRace = 0;
    CHECK(gdx_net_spectate_watch() == 0);
    CHECK(!gdx_net_spectate_on());
}

static void test_l_and_r_go_round(void) {
    field(4, kFour);
    CHECK(gdx_net_spectate_watch() == 1);
    CHECK(gdx_net_spectate_racer(0) == 5);
    /* R: the next one behind, skipping us (we are out). */
    gdx_net_spectate_frame(0, 1, 0);
    CHECK(gdx_net_spectate_racer(0) == 7);
    gdx_net_spectate_frame(0, 1, 0);
    CHECK(gdx_net_spectate_racer(0) == 9);
    /* Round from the back to the front. */
    gdx_net_spectate_frame(0, 1, 0);
    CHECK(gdx_net_spectate_racer(0) == 5);
    /* L: round from the front to the back, then forward. */
    gdx_net_spectate_frame(1, 0, 0);
    CHECK(gdx_net_spectate_racer(0) == 9);
    gdx_net_spectate_frame(1, 0, 0);
    CHECK(gdx_net_spectate_racer(0) == 7);
    /* L and R on one frame: neither. */
    gdx_net_spectate_frame(1, 1, 0);
    CHECK(gdx_net_spectate_racer(0) == 7);
}

static void test_l_and_r_skip_the_done(void) {
    field(4, kFour);
    CHECK(gdx_net_spectate_watch() == 1);
    set_driving(7, 0);
    /* 5 to 9, as 7 finished. */
    gdx_net_spectate_frame(0, 1, 0);
    CHECK(gdx_net_spectate_racer(0) == 9);
    /* With 9 the only one driving, L and R stay on it. */
    set_driving(5, 0);
    idle();
    gdx_net_spectate_frame(0, 1, 0);
    CHECK(gdx_net_spectate_racer(0) == 9);
    gdx_net_spectate_frame(1, 0, 0);
    CHECK(gdx_net_spectate_racer(0) == 9);
}

static void test_moves_on_when_the_watched_is_done(void) {
    unsigned generation;

    field(4, kFour);
    CHECK(gdx_net_spectate_watch() == 1);
    gdx_net_spectate_frame(0, 1, 0);
    CHECK(gdx_net_spectate_racer(0) == 7);
    /* 7 finishes: the nearest who drives, ahead first: 5 at place 1 over 9 at place 4. */
    generation = gdx_net_spectate_generation();
    set_driving(7, 0);
    idle();
    CHECK(gdx_net_spectate_racer(0) == 5);
    CHECK(gdx_net_spectate_generation() != generation);
    /* 5 finishes: on to 9, the only one left. */
    set_driving(5, 0);
    idle();
    CHECK(gdx_net_spectate_racer(0) == 9);
    /* 9 finishes: nobody left, the camera stays on 9 and nothing changes any more. */
    set_driving(9, 0);
    generation = gdx_net_spectate_generation();
    idle();
    gdx_net_spectate_frame(0, 1, 0);
    CHECK(gdx_net_spectate_racer(0) == 9);
    CHECK(gdx_net_spectate_on());
    CHECK(gdx_net_spectate_generation() == generation);
}

static void test_moves_on_by_place_after_a_retire(void) {
    static const int kSix[6][3] = { { 1, 1, 1 }, { 2, 2, 1 }, { 0, 3, 0 }, { 3, 4, 1 }, { 4, 5, 1 }, { 6, 6, 1 } };

    field(6, kSix);
    CHECK(gdx_net_spectate_watch() == 1);
    CHECK(gdx_net_spectate_racer(0) == 2);
    gdx_net_spectate_frame(0, 1, 0);
    gdx_net_spectate_frame(0, 1, 0);
    CHECK(gdx_net_spectate_racer(0) == 4);
    /* 4 retires at place 5: 3 at place 4 is the nearest ahead. */
    set_driving(4, 0);
    idle();
    CHECK(gdx_net_spectate_racer(0) == 3);
}

static void test_z_toggles_the_cinematic(void) {
    unsigned generation;

    field(4, kFour);
    CHECK(gdx_net_spectate_watch() == 1);
    generation = gdx_net_spectate_generation();
    gdx_net_spectate_frame(0, 0, 1);
    CHECK(gdx_net_spectate_cinematic());
    CHECK(gdx_net_spectate_generation() == generation + 1);
    /* Another racer keeps the camera chosen. */
    gdx_net_spectate_frame(0, 1, 0);
    CHECK(gdx_net_spectate_cinematic() && gdx_net_spectate_racer(0) == 7);
    gdx_net_spectate_frame(0, 0, 1);
    CHECK(!gdx_net_spectate_cinematic());
}

static void test_the_label(void) {
    char label[GDX_NET_SPECTATE_LABEL_BYTES];
    char small[8];

    field(4, kFour);
    CHECK(gdx_net_spectate_watch() == 1);
    gdx_net_spectate_label(label, sizeof(label));
    CHECK(strcmp(label, "WATCHING RACER 5") == 0);
    CHECK(strcmp(gdx_net_spectate_hint(), "L R  PLAYER    Z  CAMERA") == 0);
    gdx_net_spectate_frame(0, 1, 0);
    gdx_net_spectate_label(label, sizeof(label));
    CHECK(strcmp(label, "WATCHING PLAYER 3") == 0);
    /* A short buffer is cut and still ends. */
    gdx_net_spectate_label(small, sizeof(small));
    CHECK(strcmp(small, "WATCHIN") == 0);
}

static void test_reset_and_the_race_ending(void) {
    unsigned generation;

    field(4, kFour);
    CHECK(gdx_net_spectate_watch() == 1);
    gdx_net_spectate_frame(0, 0, 1);
    generation = gdx_net_spectate_generation();
    gdx_net_spectate_reset();
    CHECK(!gdx_net_spectate_on() && !gdx_net_spectate_cinematic());
    CHECK(gdx_net_spectate_racer(0) == 0);
    CHECK(gdx_net_spectate_generation() != generation);
    /* A reset while off changes nothing the camera sees. */
    generation = gdx_net_spectate_generation();
    gdx_net_spectate_reset();
    CHECK(gdx_net_spectate_generation() == generation);
    /* The race ends under a watching camera: the first frame without a field turns it off. */
    CHECK(gdx_net_spectate_watch() == 1);
    sInRace = 0;
    idle();
    CHECK(!gdx_net_spectate_on());
    CHECK(gdx_net_spectate_racer(0) == 0);
}

int main(void) {
    test_off_until_watch();
    test_watch_picks_the_nearest_ahead();
    test_watch_picks_the_nearest_behind();
    test_watch_with_nobody_driving();
    test_l_and_r_go_round();
    test_l_and_r_skip_the_done();
    test_moves_on_when_the_watched_is_done();
    test_moves_on_by_place_after_a_retire();
    test_z_toggles_the_cinematic();
    test_the_label();
    test_reset_and_the_race_ending();
    if (sFailures != 0) {
        printf("gdx_net_spectate_tests: %d of %d checks failed\n", sFailures, sChecks);
        return 1;
    }
    printf("gdx_net_spectate_tests: all %d checks passed\n", sChecks);
    return 0;
}
