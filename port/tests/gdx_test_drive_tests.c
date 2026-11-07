/* Unit tests for port/gdx_test_drive.c, compiled UNMODIFIED alongside this file. */
#include "../gdx_test_drive.h"

#include <stdio.h>

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

static void test_solo(void) {
    CHECK(gdx_test_drive_cpus() == 0, "no test drive, no CPUs");
    CHECK(gdx_test_drive_total_racers() == 1, "no test drive is the player alone");
    CHECK(gdx_test_drive_begin(0, 3, 1) == 1, "a solo test drive keeps the game's difficulty");
    CHECK(gdx_test_drive_cpus() == 0, "a solo test drive has no CPUs");
    CHECK(gdx_test_drive_total_racers() == 1, "a solo test drive is the stock single racer");
    CHECK(gdx_test_drive_end(1) == 1, "ending a solo test drive leaves the difficulty alone");
}

static void test_cpus(void) {
    CHECK(gdx_test_drive_begin(5, 3, 1) == 3, "a test drive with CPUs races at the menu's difficulty");
    CHECK(gdx_test_drive_cpus() == 5, "the CPU count holds while the drive runs");
    CHECK(gdx_test_drive_total_racers() == 6, "the player plus five CPUs");
    CHECK(gdx_test_drive_end(3) == 1, "ending puts the game's difficulty back");
    CHECK(gdx_test_drive_cpus() == 0, "no CPUs after the drive ends");
    CHECK(gdx_test_drive_end(2) == 2, "a second end changes nothing");

    CHECK(gdx_test_drive_begin(99, 0, 2) == 0, "Novice is a valid choice");
    CHECK(gdx_test_drive_cpus() == GDX_TEST_DRIVE_MAX_CPUS, "the count is capped at 29");
    CHECK(gdx_test_drive_total_racers() == 30, "a full field is 30 racers");
    gdx_test_drive_end(0);

    CHECK(gdx_test_drive_begin(-4, 2, 0) == 0, "a negative count is a solo drive");
    gdx_test_drive_end(0);
    CHECK(gdx_test_drive_begin(3, 9, 0) == GDX_TEST_DRIVE_DEFAULT_DIFFICULTY, "an unknown difficulty is Expert");
    gdx_test_drive_end(GDX_TEST_DRIVE_DEFAULT_DIFFICULTY);

    /* A restart re-runs the race setup, not the drive start, but a repeated begin must not lose the save. */
    gdx_test_drive_begin(4, 3, 0);
    gdx_test_drive_begin(4, 3, 3);
    CHECK(gdx_test_drive_end(3) == 0, "a repeated begin keeps the first saved difficulty");
}

static void test_pole(void) {
    int grid[5] = { 5, 1, 2, 3, 4 };
    int middle[4] = { 2, 1, 3, 4 };
    int one[1] = { 1 };

    gdx_test_drive_pole(grid, 5);
    CHECK((grid[0] == 1) && (grid[1] == 2) && (grid[2] == 3) && (grid[3] == 4) && (grid[4] == 5),
          "the player moves from the back to pole and the field moves back one");
    gdx_test_drive_pole(middle, 4);
    CHECK((middle[0] == 1) && (middle[1] == 2) && (middle[2] == 3) && (middle[3] == 4),
          "only racers that started ahead of the player move");
    gdx_test_drive_pole(one, 1);
    CHECK(one[0] == 1, "a solo grid stays as it is");
    gdx_test_drive_pole(0, 3);
    CHECK(1, "a missing grid is ignored");
}

static void test_roster(void) {
    CHECK(gdx_test_drive_cpu_character(7, 7, 0) == 0, "a CPU on the player's pinned machine takes the left-out one");
    CHECK(gdx_test_drive_cpu_character(7, 3, 0) == 7, "other CPUs keep their machine");
    CHECK(gdx_test_drive_cpu_character(4, 4, 4) == 4, "nothing to swap when the player raced the left-out machine");
    CHECK(gdx_test_drive_cpu_character(7, 7, 31) == 7, "a custom last-used machine left no stock slot to give");
    CHECK(gdx_test_drive_cpu_character(7, 7, -1) == 7, "nor does an unset one");
}

static void test_cpu_line(void) {
    short* line = gdx_test_drive_cpu_line();

    line[0] = 69;
    line[GDX_TEST_DRIVE_CPU_LINE_ENTRIES - 1] = 1;
    CHECK((line != 0) && (gdx_test_drive_cpu_line() == line), "the CPU speed table has fixed storage");
}

int main(void) {
    test_solo();
    test_cpus();
    test_pole();
    test_roster();
    test_cpu_line();
    if (sFailures != 0) {
        printf("%d test drive test(s) failed\n", sFailures);
        return 1;
    }
    printf("All test drive tests passed\n");
    return 0;
}
