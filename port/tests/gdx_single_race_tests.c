/* port/tests/gdx_single_race_tests.c -- lap settings and results paging of gdx_single_race.c. */
#include <assert.h>
#include <string.h>

#include "gdx_single_race.h"

static int sLaps = -1;
static int sLapCount = 0;
static int sUnlimited = -1;
static int sEnabled = 1;

/* Stand-ins for the libultraship CVars gdx_single_race.c reads. */
int CVarGetInteger(const char* name, int defaultValue) {
    if ((strcmp(name, "gEnhancements.SingleRace.Enabled") == 0)) {
        return sEnabled;
    }
    if ((strcmp(name, "gEnhancements.SingleRace.Laps") == 0) && (sLaps >= 0)) {
        return sLaps;
    }
    if ((strcmp(name, "gEnhancements.SingleRace.LapCount") == 0) && (sLapCount != 0)) {
        return sLapCount;
    }
    if ((strcmp(name, "gEnhancements.SingleRace.UnlimitedLaps") == 0) && (sUnlimited >= 0)) {
        return sUnlimited;
    }
    return defaultValue;
}

void CVarSetInteger(const char* name, int value) {
    if (strcmp(name, "gEnhancements.SingleRace.LapCount") == 0) {
        sLapCount = value;
    } else if (strcmp(name, "gEnhancements.SingleRace.UnlimitedLaps") == 0) {
        sUnlimited = value;
    }
}

static void set_settings(int laps, int lapCount, int unlimited) {
    sLaps = laps;
    sLapCount = lapCount;
    sUnlimited = unlimited;
}

static void test_migration(void) {
    /* No old setting: nothing is written, and the slider default applies. */
    set_settings(-1, 0, -1);
    gdx_single_race_migrate_settings();
    assert(sLapCount == 0);
    assert(sUnlimited == -1);

    /* Old index 2 was 7 laps. */
    set_settings(2, 0, -1);
    gdx_single_race_migrate_settings();
    assert(sLapCount == 7);
    assert(sUnlimited == 0);

    /* Old index 4 was Unlimited. */
    set_settings(4, 0, -1);
    gdx_single_race_migrate_settings();
    assert(sLapCount == 3);
    assert(sUnlimited == 1);

    /* A LapCount already set wins, so a later run does not undo the player's slider. */
    set_settings(2, 42, 0);
    gdx_single_race_migrate_settings();
    assert(sLapCount == 42);

    /* A bad old index is ignored. */
    set_settings(9, 0, -1);
    gdx_single_race_migrate_settings();
    assert(sLapCount == 0);
}

static void test_lap_settings(void) {
    set_settings(-1, 0, -1);
    gdx_single_race_begin_race(1);
    assert(gdx_single_race_finite());
    assert(gdx_single_race_lap_count() == 3);

    set_settings(-1, 1, -1);
    gdx_single_race_begin_race(1);
    assert(gdx_single_race_lap_count() == 1);

    set_settings(-1, 99, -1);
    gdx_single_race_begin_race(1);
    assert(gdx_single_race_lap_count() == 99);

    /* A hand-edited value out of range falls back to stock. */
    set_settings(-1, 100, -1);
    gdx_single_race_begin_race(1);
    assert(gdx_single_race_lap_count() == 3);

    set_settings(-1, 50, 1);
    gdx_single_race_begin_race(1);
    assert(gdx_single_race_active());
    assert(!gdx_single_race_finite());

    set_settings(-1, 50, 0);
    gdx_single_race_begin_race(0);
    assert(!gdx_single_race_active());
    assert(gdx_single_race_lap_count() == 3);
}

static void test_lap_table_reaches_lap_99(void) {
    set_settings(-1, 99, -1);
    gdx_single_race_begin_race(1);
    gdx_single_race_store_lap_time(29, 98, 12345);
    assert(gdx_single_race_get_lap_time(29, 98) == 12345);
    gdx_single_race_store_lap_time(29, 99, 777);
    assert(gdx_single_race_get_lap_time(29, 99) == 0);
}

/* The Practice lap count goes up without wrapping and a new race start clears it, with Single Race
 * rules on or off. */
static void test_practice_laps_count_up(void) {
    int lap;

    set_settings(-1, 50, 1);
    gdx_single_race_begin_race(1);
    assert(gdx_single_race_practice_laps_done() == 0);
    for (lap = 1; lap <= 7; lap++) {
        assert(gdx_single_race_practice_lap_done() == lap);
    }
    assert(gdx_single_race_practice_laps_done() == 7);

    gdx_single_race_begin_race(0);
    assert(gdx_single_race_practice_laps_done() == 0);
    assert(gdx_single_race_practice_lap_done() == 1);
}

static void test_results_paging(void) {
    /* Five laps or fewer never page. */
    assert(gdx_single_race_result_rows(3) == 3);
    assert(gdx_single_race_result_rows(5) == 5);
    assert(gdx_single_race_result_first_lap(5, 100000) == 0);
    assert(gdx_single_race_result_hold_frames(5) == 600);

    /* Twelve laps: pages start at laps 0, 5 and 10, and the last page stays. */
    assert(gdx_single_race_result_rows(12) == 5);
    assert(gdx_single_race_result_first_lap(12, 0) == 0);
    assert(gdx_single_race_result_first_lap(12, 169) == 0);
    assert(gdx_single_race_result_first_lap(12, 170) == 5);
    assert(gdx_single_race_result_first_lap(12, 260) == 10);
    assert(gdx_single_race_result_first_lap(12, 100000) == 10);

    /* An exact multiple ends on a full page, not an empty one. */
    assert(gdx_single_race_result_first_lap(10, 100000) == 5);

    /* Ninety-nine laps show every page before the hold ends, inside the 3000 frame timer cap. */
    assert(gdx_single_race_result_first_lap(99, gdx_single_race_result_hold_frames(99) - 1) == 95);
    assert(gdx_single_race_result_hold_frames(99) <= 3000);
}

/* A G-NET room's race takes the room's laps and field for one race, whatever the player's own Single Race
 * settings say, and leaves nothing behind for the race after it. */
static void test_forced_race(void) {
    set_settings(-1, 7, 1); /* the player's own: unlimited laps */
    sEnabled = 0;
    gdx_single_race_force_next(5, 14);
    gdx_single_race_begin_race(1);
    assert(gdx_single_race_active());
    assert(gdx_single_race_finite());
    assert(gdx_single_race_lap_count() == 5);
    assert(gdx_single_race_racer_count() == 14);

    /* The race after it is the player's own again, and their option is off. */
    gdx_single_race_begin_race(1);
    assert(!gdx_single_race_active());
    assert(gdx_single_race_racer_count() == 30);

    /* Laps outside 1 to 99 become three, and the field is never more than the grid's thirty. */
    gdx_single_race_force_next(0, 45);
    gdx_single_race_begin_race(1);
    assert(gdx_single_race_lap_count() == 3);
    assert(gdx_single_race_racer_count() == 30);
    gdx_single_race_force_next(100, 2);
    gdx_single_race_begin_race(1);
    assert(gdx_single_race_lap_count() == 3);
    assert(gdx_single_race_racer_count() == 2);
    gdx_single_race_force_next(99, 1);
    gdx_single_race_begin_race(1);
    assert(gdx_single_race_finite());
    assert(gdx_single_race_lap_count() == 99);
    assert(gdx_single_race_racer_count() == 1);

    /* A race that cannot take it drops it: the next race is the player's own. */
    sEnabled = 1;
    set_settings(-1, 7, 0);
    gdx_single_race_force_next(5, 14);
    gdx_single_race_begin_race(0);
    assert(!gdx_single_race_active());
    gdx_single_race_begin_race(1);
    assert(gdx_single_race_active());
    assert(gdx_single_race_lap_count() == 7);

    /* No racers is no request. */
    gdx_single_race_force_next(5, 0);
    gdx_single_race_begin_race(1);
    assert(gdx_single_race_lap_count() == 7);
    set_settings(-1, 0, -1);
}

int main(void) {
    test_migration();
    test_lap_settings();
    test_lap_table_reaches_lap_99();
    test_practice_laps_count_up();
    test_results_paging();
    test_forced_race();
    return 0;
}
