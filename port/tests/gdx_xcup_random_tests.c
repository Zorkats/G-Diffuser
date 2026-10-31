/* port/tests/gdx_xcup_random_tests.c -- unit tests for the X Cup random course toggle: the pool, the
 * deck, the real course and cup lookups, and the saved copy of a generated course.
 * Standalone: links gdx_xcup_random.c and stubs the one CVar read it makes.
 */

#include "../gdx_xcup_random.h"

#include "../gdx_content_io.h"
#include "../gdx_xcup_seed.h"

#include <stdio.h>
#include <string.h>

#define X_1 48
#define COURSE_LIMIT 56

static int sFailures;
static int sToggle;

int CVarGetInteger(const char* name, int defaultValue) {
    return strcmp(name, GDX_XCUP_RANDOM_CVAR) == 0 ? sToggle : defaultValue;
}

static void check(int condition, const char* label) {
    if (condition) {
        printf("PASS: %s\n", label);
    } else {
        fprintf(stderr, "FAIL: %s\n", label);
        sFailures++;
    }
}

static void test_toggle(void) {
    sToggle = 0;
    check(!gdx_xcup_random_enabled(), "the toggle is off by default");
    sToggle = 1;
    check(gdx_xcup_random_enabled(), "the toggle reads its CVar");
    sToggle = 0;
}

/* Rolling every value in a new cup's first slot reaches the whole pool, and nothing else. */
static void test_pool(int ddAvailable, int expectedSize, const char* sizeLabel, const char* contentLabel) {
    int seen[COURSE_LIMIT] = { 0 };
    int valid = 1;
    int count = 0;
    uint32_t roll;
    int i;

    for (roll = 0; roll < 200; roll++) {
        int course = gdx_xcup_random_pick(0, 1, roll, ddAvailable);
        if (course < 0 || course >= COURSE_LIMIT) {
            valid = 0;
        } else {
            seen[course] = 1;
        }
    }
    for (i = 0; i < COURSE_LIMIT; i++) {
        int inStock = i < GDX_XCUP_RANDOM_STOCK_COUNT;
        int inDd = i >= GDX_XCUP_RANDOM_DD_FIRST && i < GDX_XCUP_RANDOM_DD_FIRST + GDX_XCUP_RANDOM_DD_COUNT;
        count += seen[i];
        if (seen[i] && !(inStock || (ddAvailable && inDd))) {
            valid = 0;
        }
    }
    check(count == expectedSize, sizeLabel);
    check(valid, contentLabel);
}

static void test_pools(void) {
    test_pool(1, 36, "the pool holds 36 courses with the Expansion Kit", "the pool is the stock and Expansion Kit courses");
    test_pool(0, 24, "the pool holds 24 courses without the Expansion Kit", "the pool never reaches an Expansion Kit course without it");
}

static void test_cup_has_no_repeats(void) {
    uint32_t base;
    int distinct = 1;

    for (base = 0; base < 500; base++) {
        int picks[GDX_XCUP_CUP_SIZE];
        int slot;
        int other;
        for (slot = 0; slot < GDX_XCUP_CUP_SIZE; slot++) {
            /* The same roll every time is the hardest case: it would pick one course without a deck. */
            picks[slot] = gdx_xcup_random_pick(slot, slot == 0, base, base % 2);
            for (other = 0; other < slot; other++) {
                if (picks[other] == picks[slot]) {
                    distinct = 0;
                }
            }
        }
    }
    check(distinct, "six courses of a cup are all different");
}

static void test_redo_keeps_the_course(void) {
    int first = gdx_xcup_random_pick(0, 1, 5, 1);
    int second = gdx_xcup_random_pick(1, 0, 9, 1);

    check(gdx_xcup_random_pick(1, 0, 22, 1) == second, "a re-do takes the slot's pick whatever the roll");
    check(gdx_xcup_random_pick(0, 0, 22, 1) == first, "an earlier slot keeps its pick");
}

static void test_new_cup_empties_the_deck(void) {
    int first = gdx_xcup_random_pick(0, 1, 5, 1);
    int again = gdx_xcup_random_pick(0, 1, 5, 1);

    check(first == again, "a new cup can use a course the last cup used");
}

static void test_bad_slots_change_nothing(void) {
    int active = gdx_xcup_random_pick(0, 1, 3, 1);

    check(gdx_xcup_random_pick(-1, 0, 3, 1) == -1, "a negative slot is refused");
    check(gdx_xcup_random_pick(GDX_XCUP_CUP_SIZE, 1, 3, 1) == -1, "a slot past the cup is refused");
    check(gdx_xcup_random_active_course() == active, "a refused slot leaves the active course");
    check(gdx_xcup_random_pick(0, 0, 9, 1) == active, "a refused slot doesn't empty the deck");
}

static void test_real_course(void) {
    int active;

    gdx_xcup_random_clear_active();
    check(gdx_xcup_real_course(X_1) == X_1, "with no active course an X Cup slot stays itself");
    check(gdx_xcup_real_cup(4, X_1) == 4, "with no active course the cup stays itself");

    active = gdx_xcup_random_pick(0, 1, 7, 1);
    check(active == 7, "roll 7 of a new cup is course 7");
    check(gdx_xcup_real_course(X_1) == active, "the first X Cup slot reads the real course");
    check(gdx_xcup_real_course(X_1 + 5) == active, "the last X Cup slot reads the real course");
    check(gdx_xcup_real_course(X_1 - 1) == X_1 - 1, "the course before X Cup stays itself");
    check(gdx_xcup_real_course(X_1 + 6) == X_1 + 6, "Death Race stays itself");
    check(gdx_xcup_real_course(3) == 3, "a stock course stays itself");
    check(gdx_xcup_real_cup(1, 3) == 1, "a stock course keeps its cup");
    check(gdx_xcup_real_cup(4, X_1) == 1, "course 7 is in the Queen Cup");

    gdx_xcup_random_clear_active();
    check(gdx_xcup_random_active_course() == -1, "clearing leaves no active course");
    check(gdx_xcup_real_course(X_1) == X_1, "clearing returns the slot to itself");
}

/* Roll 24 reaches the first Expansion Kit course in a new cup, and 24 + 6 the second cup's first. */
static void test_real_cup_of_dd_courses(void) {
    check(gdx_xcup_random_pick(0, 1, 23, 1) == 23, "the last stock course is course 23");
    check(gdx_xcup_real_cup(4, X_1) == 3, "course 23 is in the Joker Cup");
    check(gdx_xcup_random_pick(0, 1, 24, 1) == 30, "the first Expansion Kit course is course 30");
    check(gdx_xcup_real_cup(4, X_1) == 6, "course 30 is in the first Expansion Kit cup");
    check(gdx_xcup_random_pick(0, 1, 29, 1) == 35, "the sixth Expansion Kit course is course 35");
    check(gdx_xcup_real_cup(4, X_1) == 6, "course 35 is in the first Expansion Kit cup");
    check(gdx_xcup_random_pick(0, 1, 30, 1) == 36, "the seventh Expansion Kit course is course 36");
    check(gdx_xcup_real_cup(4, X_1) == 7, "course 36 is in the second Expansion Kit cup");
    check(gdx_xcup_random_pick(0, 1, 35, 1) == 41, "the last Expansion Kit course is course 41");
    check(gdx_xcup_real_cup(4, X_1) == 7, "course 41 is in the second Expansion Kit cup");
    gdx_xcup_random_clear_active();
}

static void test_generated_copy(void) {
    static unsigned char course[0x7E0];
    static unsigned char tooLarge[GDX_CONTENT_TRACK_PAYLOAD_SIZE + 1];
    const uint8_t* payload;
    int tailZero = 1;
    int i;

    gdx_xcup_random_pick(0, 1, 0, 1);
    check(gdx_xcup_generated_payload() == NULL, "a real course leaves no generated copy");

    memset(course, 0xAB, sizeof(course));
    gdx_xcup_generated_capture(course, sizeof(course));
    payload = gdx_xcup_generated_payload();
    check(payload != NULL, "a capture makes a copy");
    check(payload != NULL && memcmp(payload, course, sizeof(course)) == 0, "the copy starts with the course");
    for (i = (int) sizeof(course); payload != NULL && i < GDX_CONTENT_TRACK_PAYLOAD_SIZE; i++) {
        if (payload[i] != 0) {
            tailZero = 0;
        }
    }
    check(tailZero, "the copy has no ghosts and no records");

    memset(tooLarge, 0xCD, sizeof(tooLarge));
    gdx_xcup_generated_capture(tooLarge, sizeof(tooLarge));
    check(gdx_xcup_generated_payload()[0] == 0xAB, "a capture too large for the copy is refused");
    gdx_xcup_generated_capture(NULL, 16);
    check(gdx_xcup_generated_payload()[0] == 0xAB, "a null capture is refused");

    check(gdx_xcup_random_pick(-1, 0, 0, 1) == -1 && gdx_xcup_generated_payload() != NULL,
          "a refused slot keeps the generated copy");
    gdx_xcup_random_pick(0, 1, 0, 1);
    check(gdx_xcup_generated_payload() == NULL, "picking a real course drops the generated copy");
    gdx_xcup_random_clear_active();
}

int main(void) {
    test_toggle();
    test_pools();
    test_cup_has_no_repeats();
    test_redo_keeps_the_course();
    test_new_cup_empties_the_deck();
    test_bad_slots_change_nothing();
    test_real_course();
    test_real_cup_of_dd_courses();
    test_generated_copy();

    if (sFailures != 0) {
        fprintf(stderr, "%d X Cup random course test(s) failed\n", sFailures);
        return 1;
    }
    printf("All X Cup random course tests passed\n");
    return 0;
}
