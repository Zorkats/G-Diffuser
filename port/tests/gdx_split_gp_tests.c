/* port/tests/gdx_split_gp_tests.c -- unit tests for the split-screen Grand Prix rules (gdx_split_gp.h).
 * Copyright (c) Zorkats
 *
 * Standalone: links gdx_split_gp.c only. Every expected value is written out by hand from the rules in the
 * header, never computed with the module itself.
 */

#include "../gdx_split_gp.h"

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

static void test_field(void) {
    CHECK(gdx_split_gp_field(2, 30) == 30);
    CHECK(gdx_split_gp_field(2, 31) == 30);
    CHECK(gdx_split_gp_field(2, 1000) == 30);
    CHECK(gdx_split_gp_field(2, 12) == 12);
    CHECK(gdx_split_gp_field(4, 4) == 4);
    CHECK(gdx_split_gp_field(4, 3) == 4);
    CHECK(gdx_split_gp_field(3, 0) == 3);
    CHECK(gdx_split_gp_field(3, -5) == 3);
    CHECK(gdx_split_gp_field(9, 2) == 4);  /* players clamp to 4 */
    CHECK(gdx_split_gp_field(0, 0) == 1);  /* players clamp to 1 */
}

static void test_race_over(void) {
    /* Two players: over only when both are done. */
    CHECK(gdx_split_gp_race_over(2, 0x0u, 0x0u, 0x0u) == 0);
    CHECK(gdx_split_gp_race_over(2, 0x1u, 0x0u, 0x0u) == 0);
    CHECK(gdx_split_gp_race_over(2, 0x1u, 0x2u, 0x0u) == 1);
    CHECK(gdx_split_gp_race_over(2, 0x0u, 0x3u, 0x0u) == 1);
    CHECK(gdx_split_gp_race_over(2, 0x3u, 0x0u, 0x0u) == 1);
    /* An out player is not waited for, even while their autopilot races. */
    CHECK(gdx_split_gp_race_over(2, 0x1u, 0x0u, 0x2u) == 1);
    CHECK(gdx_split_gp_race_over(3, 0x0u, 0x1u, 0x2u) == 0);
    CHECK(gdx_split_gp_race_over(3, 0x4u, 0x1u, 0x2u) == 1);
    /* Everyone out. */
    CHECK(gdx_split_gp_race_over(4, 0x0u, 0x0u, 0xFu) == 1);
    /* Bits past the player count are ignored. */
    CHECK(gdx_split_gp_race_over(2, 0x4u, 0x8u, 0x0u) == 0);
    CHECK(gdx_split_gp_race_over(2, 0x3u, 0x0u, 0xF0u) == 1);
    /* Bad counts. */
    CHECK(gdx_split_gp_race_over(0, 0xFu, 0xFu, 0x0u) == 0);
    CHECK(gdx_split_gp_race_over(5, 0x1Fu, 0x0u, 0x0u) == 0);
}

static void test_resolve_counts(void) {
    short lives[4] = { 2, 0, 1, 3 };
    unsigned out = 0;
    unsigned lost = 0xFFu;

    /* P1 finished, P2 retired at 0 lives, P3 retired with 1, P4 finished. */
    CHECK(gdx_split_gp_resolve(4, 0x9u, 0x6u, &out, lives, &lost) == GDX_SPLIT_GP_COUNTS);
    CHECK(out == 0x2u);
    CHECK(lost == 0x4u);
    CHECK(lives[0] == 2);
    CHECK(lives[1] == 0);
    CHECK(lives[2] == 0);
    CHECK(lives[3] == 3);
}

static void test_resolve_retry(void) {
    short lives[2] = { 1, 0 };
    unsigned out = 0;
    unsigned lost = 0;

    /* Both retired: P1 spends a life and retries, P2 is out. */
    CHECK(gdx_split_gp_resolve(2, 0x0u, 0x3u, &out, lives, &lost) == GDX_SPLIT_GP_RETRY);
    CHECK(out == 0x2u);
    CHECK(lost == 0x1u);
    CHECK(lives[0] == 0);
    CHECK(lives[1] == 0);

    /* The retry: P1 retires again with no life left; P2's autopilot is not counted. */
    CHECK(gdx_split_gp_resolve(2, 0x2u, 0x1u, &out, lives, &lost) == GDX_SPLIT_GP_GAME_OVER);
    CHECK(out == 0x3u);
    CHECK(lost == 0x0u);
}

static void test_resolve_out_player_finishing(void) {
    short lives[3] = { 0, 0, 0 };
    unsigned out = 0x2u;
    unsigned lost = 0;

    /* P2 is out, and their autopilot finishing does not count the race for the others. */
    CHECK(gdx_split_gp_resolve(3, 0x2u, 0x5u, &out, lives, &lost) == GDX_SPLIT_GP_GAME_OVER);
    CHECK(out == 0x7u);
    CHECK(lost == 0x0u);
}

static void test_resolve_finished_keeps_lives(void) {
    short lives[2] = { 0, 0 };
    unsigned out = 0;
    unsigned lost = 0;

    /* A player who finished never loses a life, even with a retired bit set as well. */
    CHECK(gdx_split_gp_resolve(2, 0x3u, 0x3u, &out, lives, &lost) == GDX_SPLIT_GP_COUNTS);
    CHECK(out == 0x0u);
    CHECK(lost == 0x0u);
}

static void test_resolve_bad_arguments(void) {
    short lives[4] = { 1, 1, 1, 1 };
    unsigned out = 0;
    unsigned lost = 0x55u;

    CHECK(gdx_split_gp_resolve(0, 0x0u, 0x1u, &out, lives, &lost) == -1);
    CHECK(gdx_split_gp_resolve(5, 0x0u, 0x1u, &out, lives, &lost) == -1);
    CHECK(gdx_split_gp_resolve(2, 0x0u, 0x1u, 0, lives, &lost) == -1);
    CHECK(gdx_split_gp_resolve(2, 0x0u, 0x1u, &out, 0, &lost) == -1);
    CHECK(gdx_split_gp_resolve(2, 0x0u, 0x1u, &out, lives, 0) == -1);
    CHECK(out == 0x0u);
    CHECK(lost == 0x55u);
    CHECK(lives[0] == 1);
}

static void test_retry_allowed_and_cost(void) {
    short lives[4] = { 1, 0, 2, 0 };
    unsigned lost = 0xFFu;

    /* P2 has no life and has not finished: no retry. */
    CHECK(gdx_split_gp_retry_allowed(4, 0x0u, 0x8u, lives) == 0);
    /* P2 finished, P4 is out: allowed. */
    CHECK(gdx_split_gp_retry_allowed(4, 0x2u, 0x8u, lives) == 1);
    CHECK(gdx_split_gp_retry_allowed(4, 0x2u, 0x0u, lives) == 0);
    CHECK(gdx_split_gp_retry_allowed(4, 0x2u, 0x8u, 0) == 0);
    CHECK(gdx_split_gp_retry_allowed(0, 0x0u, 0x0u, lives) == 0);

    gdx_split_gp_retry_cost(4, 0x2u, 0x8u, lives, &lost);
    CHECK(lost == 0x5u);
    CHECK(lives[0] == 0);
    CHECK(lives[1] == 0);
    CHECK(lives[2] == 1);
    CHECK(lives[3] == 0);

    /* Nobody has a life: nothing changes, and no one goes below 0. */
    gdx_split_gp_retry_cost(2, 0x0u, 0x0u, lives, &lost);
    CHECK(lost == 0x0u);
    CHECK(lives[0] == 0);
    CHECK(lives[1] == 0);
}

static void test_back_grid(void) {
    /* Six machines, two players: the CPUs held 1, 4, 2, 5 and keep that order ahead of the players. */
    int grid[6] = { 6, 3, 1, 4, 2, 5 };
    int expected[6] = { 6, 5, 1, 3, 2, 4 };
    int bad[4] = { 1, 1, 2, 3 };
    int badCopy[4] = { 1, 1, 2, 3 };
    int zero[3] = { 0, 1, 2 };

    CHECK(gdx_split_gp_back_grid(grid, 6, 2) == 1);
    CHECK(memcmp(grid, expected, sizeof(grid)) == 0);

    /* Only players. */
    {
        int players[4] = { 3, 1, 4, 2 };
        int playersExpected[4] = { 4, 3, 2, 1 };

        CHECK(gdx_split_gp_back_grid(players, 4, 4) == 1);
        CHECK(memcmp(players, playersExpected, sizeof(players)) == 0);
    }
    /* Thirty machines, three players on the front row; the CPUs in slots 3 .. 29 hold 30 down to 4. */
    {
        int full[30];
        int i;

        full[0] = 1;
        full[1] = 2;
        full[2] = 3;
        for (i = 3; i < 30; i++) {
            full[i] = 33 - i; /* 30 .. 4 */
        }
        CHECK(gdx_split_gp_back_grid(full, 30, 3) == 1);
        CHECK(full[0] == 30);
        CHECK(full[1] == 29);
        CHECK(full[2] == 28);
        CHECK(full[3] == 27);
        CHECK(full[29] == 1);
    }

    CHECK(gdx_split_gp_back_grid(bad, 4, 2) == 0);
    CHECK(memcmp(bad, badCopy, sizeof(bad)) == 0);
    CHECK(gdx_split_gp_back_grid(zero, 3, 2) == 0);
    CHECK(gdx_split_gp_back_grid(grid, 1, 2) == 0);
    CHECK(gdx_split_gp_back_grid(grid, 31, 2) == 0);
    CHECK(gdx_split_gp_back_grid(grid, 6, 0) == 0);
    CHECK(gdx_split_gp_back_grid(0, 6, 2) == 0);
}

int main(void) {
    test_field();
    test_race_over();
    test_resolve_counts();
    test_resolve_retry();
    test_resolve_out_player_finishing();
    test_resolve_finished_keeps_lives();
    test_resolve_bad_arguments();
    test_retry_allowed_and_cost();
    test_back_grid();
    if (sFailures != 0) {
        printf("gdx_split_gp_tests: %d of %d checks failed\n", sFailures, sChecks);
        return 1;
    }
    printf("gdx_split_gp_tests: all %d checks passed\n", sChecks);
    return 0;
}
