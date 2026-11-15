/* port/tests/gdx_hang_watchdog_tests.c -- unit tests for the hang watchdog's decision and lines.
 * Copyright (c) Zorkats
 *
 * Standalone: links gdx_hang_watchdog.c only. The clock is the seconds each step passes in, so no thread
 * runs and nothing waits. Expected values are written out by hand from gdx_hang_watchdog.h: arming at 120
 * beats, a freeze at 5 s, and 3 dumps a session.
 */

#include "../gdx_hang_watchdog.h"

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

/* A watch whose loop has run past arming and last beat at t = 10 s. */
static void armed_at_ten(GdxHangWatch* watch) {
    gdx_hang_watch_reset(watch);
    CHECK(gdx_hang_watch_step(watch, 500, 10.0, NULL) == GDX_HANG_NONE);
}

static void test_beats_keep_it_quiet(void) {
    GdxHangWatch watch;
    unsigned long long beats = 500;
    double t;

    armed_at_ten(&watch);
    /* 60 s of beats, one a step: never a freeze, however long the run. */
    for (t = 10.25; t < 70.0; t += 0.25) {
        CHECK(gdx_hang_watch_step(&watch, ++beats, t, NULL) == GDX_HANG_NONE);
    }
    CHECK(watch.freezes == 0 && !watch.frozen);
}

static void test_freeze_after_five_seconds(void) {
    GdxHangWatch watch;

    armed_at_ten(&watch);
    CHECK(gdx_hang_watch_step(&watch, 500, 12.0, NULL) == GDX_HANG_NONE);
    CHECK(gdx_hang_watch_step(&watch, 500, 14.99, NULL) == GDX_HANG_NONE);
    CHECK(gdx_hang_watch_step(&watch, 500, 15.0, NULL) == GDX_HANG_FROZE);
    CHECK(watch.frozen && watch.freezes == 1);
    /* Once per freeze: the steps after it stay quiet however long it lasts. */
    CHECK(gdx_hang_watch_step(&watch, 500, 15.25, NULL) == GDX_HANG_NONE);
    CHECK(gdx_hang_watch_step(&watch, 500, 600.0, NULL) == GDX_HANG_NONE);
    CHECK(watch.freezes == 1);
}

static void test_resumed_reports_the_stall(void) {
    GdxHangWatch watch;
    double stalled = -1.0;

    armed_at_ten(&watch);
    CHECK(gdx_hang_watch_step(&watch, 500, 15.0, NULL) == GDX_HANG_FROZE);
    CHECK(gdx_hang_watch_step(&watch, 501, 22.5, &stalled) == GDX_HANG_RESUMED);
    CHECK(stalled == 12.5);
    CHECK(!watch.frozen && watch.freezes == 1);
    /* Beats after the resume are ordinary again, and a second freeze is the second one. */
    CHECK(gdx_hang_watch_step(&watch, 502, 22.75, NULL) == GDX_HANG_NONE);
    CHECK(gdx_hang_watch_step(&watch, 502, 27.75, NULL) == GDX_HANG_FROZE);
    CHECK(watch.freezes == 2);
}

static void test_short_stall_is_no_freeze(void) {
    GdxHangWatch watch;
    double stalled = -1.0;

    armed_at_ten(&watch);
    CHECK(gdx_hang_watch_step(&watch, 500, 14.0, NULL) == GDX_HANG_NONE);
    /* A 4 s window drag, then frames: no freeze, so no resume either, and stalled is untouched. */
    CHECK(gdx_hang_watch_step(&watch, 501, 14.5, &stalled) == GDX_HANG_NONE);
    CHECK(stalled == -1.0);
    CHECK(watch.freezes == 0);
}

static void test_boot_does_not_count(void) {
    GdxHangWatch watch;

    gdx_hang_watch_reset(&watch);
    /* The first step only starts the clock, even at beat 0. */
    CHECK(gdx_hang_watch_step(&watch, 0, 1.0, NULL) == GDX_HANG_NONE);
    /* A 30 s boot load before 120 beats: not armed, no freeze. */
    CHECK(gdx_hang_watch_step(&watch, 0, 31.0, NULL) == GDX_HANG_NONE);
    CHECK(gdx_hang_watch_step(&watch, 119, 31.25, NULL) == GDX_HANG_NONE);
    CHECK(gdx_hang_watch_step(&watch, 119, 40.0, NULL) == GDX_HANG_NONE);
    CHECK(watch.freezes == 0);
    /* At 120 beats it is armed. */
    CHECK(gdx_hang_watch_step(&watch, 120, 40.25, NULL) == GDX_HANG_NONE);
    CHECK(gdx_hang_watch_step(&watch, 120, 45.25, NULL) == GDX_HANG_FROZE);
}

static void test_dump_cap(void) {
    GdxHangWatch watch;
    unsigned long long beats = 500;
    double t = 10.0;
    int freeze;

    armed_at_ten(&watch);
    CHECK(!gdx_hang_watch_may_dump(&watch));
    for (freeze = 1; freeze <= 5; freeze++) {
        t += 5.0;
        CHECK(gdx_hang_watch_step(&watch, beats, t, NULL) == GDX_HANG_FROZE);
        CHECK(watch.freezes == freeze);
        CHECK(gdx_hang_watch_may_dump(&watch) == (freeze <= GDX_HANG_MAX_DUMPS));
        t += 0.25;
        CHECK(gdx_hang_watch_step(&watch, ++beats, t, NULL) == GDX_HANG_RESUMED);
        /* Between freezes nothing may dump. */
        CHECK(!gdx_hang_watch_may_dump(&watch));
    }
}

static void test_lines(void) {
    char line[192];

    CHECK(gdx_hang_format_header(line, sizeof(line), 1, 5.0) > 0);
    CHECK(strcmp(line, "[hang] FREEZE 1: no frame for 5.0 s, main thread stack follows\n") == 0);
    CHECK(gdx_hang_format_frame(line, sizeof(line), 0, "G-Diffuser.exe", 0x1A2B3CULL) > 0);
    CHECK(strcmp(line, "[hang]   #0 G-Diffuser.exe+0x1A2B3C\n") == 0);
    CHECK(gdx_hang_format_frame(line, sizeof(line), 12, "ntdll.dll", 0x9D0E4ULL) > 0);
    CHECK(strcmp(line, "[hang]   #12 ntdll.dll+0x9D0E4\n") == 0);
    CHECK(gdx_hang_format_frame(line, sizeof(line), 3, NULL, 0x7FF600001000ULL) > 0);
    CHECK(strcmp(line, "[hang]   #3 ?+0x7FF600001000\n") == 0);
    CHECK(gdx_hang_format_resumed(line, sizeof(line), 2, 12.5) > 0);
    CHECK(strcmp(line, "[hang] FREEZE 2 ended: frames resumed after 12.5 s\n") == 0);
}

int main(void) {
    test_beats_keep_it_quiet();
    test_freeze_after_five_seconds();
    test_resumed_reports_the_stall();
    test_short_stall_is_no_freeze();
    test_boot_does_not_count();
    test_dump_cap();
    test_lines();
    if (sFailures != 0) {
        printf("gdx_hang_watchdog_tests: %d of %d checks failed\n", sFailures, sChecks);
        return 1;
    }
    printf("gdx_hang_watchdog_tests: all %d checks passed\n", sChecks);
    return 0;
}
