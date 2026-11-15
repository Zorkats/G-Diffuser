/* port/tests/gdx_net_shield_tests.c -- unit tests for the G-NET shield rule (gdx_net_shield.h).
 * Copyright (c) Zorkats
 *
 * Standalone: links gdx_net_shield.c only. The room's rule is the one function below. Expected values are
 * written out by hand from the header: a machine of 100 energy, a pit of 0.5 a frame and a bar of 68 pixels,
 * the stock bar's width.
 */

#include "../gdx_net_shield.h"

#include <stdio.h>

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

/* The room's shield rule, as the module sees it. */
static int sRule;

int gdx_net_race_rule_shield(void) {
    return sRule;
}

/* Every value below is a sum of halves, so float compares exactly. */
static GdxNetShield full_shield(void) {
    GdxNetShield shield;

    gdx_net_shield_reset(&shield, 100.0f);
    return shield;
}

static int span_is(const GdxNetShieldSpan* span, int from, int to, int colour) {
    return (span->from == from) && (span->to == to) && (span->colour == colour);
}

static void test_reset(void) {
    GdxNetShield shield = full_shield();

    CHECK(shield.pool == 100.0f && shield.maxPool == 100.0f && shield.flash == 0);
    gdx_net_shield_reset(&shield, -5.0f);
    CHECK(shield.pool == 0.0f && shield.maxPool == 0.0f);
}

/* From lap 2 the shield takes a hit before energy, and energy takes what is left over. */
static void test_shield_absorbs_first(void) {
    GdxNetShield shield = full_shield();

    CHECK(gdx_net_shield_take(&shield, 0, 100.0f, 30.0f, GDX_NET_SHIELD_HIT) == 0.0f);
    CHECK(shield.pool == 70.0f);
    CHECK(gdx_net_shield_take(&shield, 0, 100.0f, 80.0f, GDX_NET_SHIELD_HIT) == 10.0f);
    CHECK(shield.pool == 0.0f);
    CHECK(gdx_net_shield_take(&shield, 0, 90.0f, 5.0f, GDX_NET_SHIELD_HIT) == 5.0f);
    CHECK(shield.pool == 0.0f);
    /* A hit that empties the shield exactly leaves energy whole. */
    shield = full_shield();
    CHECK(gdx_net_shield_take(&shield, 0, 100.0f, 100.0f, GDX_NET_SHIELD_HIT) == 0.0f);
    CHECK(shield.pool == 0.0f);
}

/* Lap 1 halves a hit and leaves the shield pool alone. */
static void test_lap_one_halves(void) {
    GdxNetShield shield = full_shield();

    CHECK(gdx_net_shield_take(&shield, 1, 100.0f, 40.0f, GDX_NET_SHIELD_HIT) == 20.0f);
    CHECK(shield.pool == 100.0f);
    CHECK(gdx_net_shield_take(&shield, 1, 100.0f, 1.0f, GDX_NET_SHIELD_HIT) == 0.5f);
}

/* No hit on lap 1 takes energy under 1. */
static void test_lap_one_floor(void) {
    GdxNetShield shield = full_shield();

    CHECK(gdx_net_shield_take(&shield, 1, 10.0f, 40.0f, GDX_NET_SHIELD_HIT) == 9.0f);
    CHECK(gdx_net_shield_take(&shield, 1, 10.0f, 18.0f, GDX_NET_SHIELD_HIT) == 9.0f);
    CHECK(gdx_net_shield_take(&shield, 1, 10.0f, 16.0f, GDX_NET_SHIELD_HIT) == 8.0f);
    CHECK(gdx_net_shield_take(&shield, 1, 1.0f, 40.0f, GDX_NET_SHIELD_HIT) == 0.0f);
    /* Under the floor already (lava or a boost took it there): a hit takes nothing, and never adds. */
    CHECK(gdx_net_shield_take(&shield, 1, 0.5f, 40.0f, GDX_NET_SHIELD_HIT) == 0.0f);
    CHECK(gdx_net_shield_take(&shield, 1, 0.0f, 40.0f, GDX_NET_SHIELD_HIT) == 0.0f);
}

/* Lava drains energy past the shield on every lap, so a machine at 0 energy in lava is still knocked out. */
static void test_lava_kills(void) {
    GdxNetShield shield = full_shield();

    CHECK(gdx_net_shield_take(&shield, 1, 0.5f, 2.0f, GDX_NET_SHIELD_LAVA) == 2.0f);
    CHECK(gdx_net_shield_take(&shield, 1, 0.0f, 2.0f, GDX_NET_SHIELD_LAVA) == 2.0f);
    CHECK(gdx_net_shield_take(&shield, 0, 3.0f, 3.0f, GDX_NET_SHIELD_LAVA) == 3.0f);
    CHECK(shield.pool == 100.0f);
    CHECK(shield.flash == 0);
}

/* A push of 0 or less is no hit: it is handed back as it is and starts no flash. */
static void test_no_hit(void) {
    GdxNetShield shield = full_shield();

    CHECK(gdx_net_shield_take(&shield, 0, 100.0f, 0.0f, GDX_NET_SHIELD_HIT) == 0.0f);
    CHECK(gdx_net_shield_take(&shield, 1, 100.0f, -2.0f, GDX_NET_SHIELD_HIT) == -2.0f);
    CHECK(shield.pool == 100.0f && shield.flash == 0);
}

static void test_flash(void) {
    GdxNetShield shield = full_shield();
    int i;

    gdx_net_shield_take(&shield, 0, 100.0f, 1.0f, GDX_NET_SHIELD_HIT);
    CHECK(shield.flash == GDX_NET_SHIELD_FLASH_FRAMES);
    for (i = 0; i < GDX_NET_SHIELD_FLASH_FRAMES - 1; i++) {
        gdx_net_shield_frame(&shield);
    }
    CHECK(shield.flash == 1);
    gdx_net_shield_frame(&shield);
    CHECK(shield.flash == 0);
    gdx_net_shield_frame(&shield);
    CHECK(shield.flash == 0);
    /* A lap-1 hit flashes too, the floor's included. */
    gdx_net_shield_take(&shield, 1, 1.0f, 5.0f, GDX_NET_SHIELD_HIT);
    CHECK(shield.flash == GDX_NET_SHIELD_FLASH_FRAMES);
}

/* The pit refills the shield at half the energy rate, only at full energy and not while boosting. */
static void test_pit(void) {
    GdxNetShield shield = full_shield();

    shield.pool = 50.0f;
    gdx_net_shield_pit(&shield, 100.0f, 100.0f, 0.5f, 0);
    CHECK(shield.pool == 50.25f);
    gdx_net_shield_pit(&shield, 99.5f, 100.0f, 0.5f, 0);
    CHECK(shield.pool == 50.25f);
    gdx_net_shield_pit(&shield, 100.0f, 100.0f, 0.5f, 1);
    CHECK(shield.pool == 50.25f);
    gdx_net_shield_pit(&shield, 100.0f, 100.0f, 0.0f, 0);
    CHECK(shield.pool == 50.25f);
    shield.pool = 99.875f;
    gdx_net_shield_pit(&shield, 100.0f, 100.0f, 0.5f, 0);
    CHECK(shield.pool == 100.0f);
    gdx_net_shield_pit(&shield, 100.0f, 100.0f, 0.5f, 0);
    CHECK(shield.pool == 100.0f);
}

/* Lap 1: energy red over the green super shield. */
static void test_bar_lap_one(void) {
    GdxNetShield shield = full_shield();
    GdxNetShieldSpan spans[2];

    CHECK(gdx_net_shield_bar(&shield, 1, 50.0f, 100.0f, 68, spans) == 2);
    CHECK(span_is(&spans[0], 0, 34, GDX_NET_SHIELD_BAR_RED));
    CHECK(span_is(&spans[1], 34, 68, GDX_NET_SHIELD_BAR_GREEN));
    /* Full energy leaves no green; the shield pool plays no part on lap 1. */
    shield.pool = 0.0f;
    CHECK(gdx_net_shield_bar(&shield, 1, 100.0f, 100.0f, 68, spans) == 1);
    CHECK(span_is(&spans[0], 0, 68, GDX_NET_SHIELD_BAR_RED));
    CHECK(gdx_net_shield_bar(&shield, 1, 0.0f, 100.0f, 68, spans) == 1);
    CHECK(span_is(&spans[0], 0, 68, GDX_NET_SHIELD_BAR_GREEN));
}

/* From lap 2, the four states: both full, shield lower, energy lower, both empty. */
static void test_bar_states(void) {
    GdxNetShield shield = full_shield();
    GdxNetShieldSpan spans[2];

    CHECK(gdx_net_shield_bar(&shield, 0, 100.0f, 100.0f, 68, spans) == 1);
    CHECK(span_is(&spans[0], 0, 68, GDX_NET_SHIELD_BAR_CYAN));

    shield.pool = 50.0f;
    CHECK(gdx_net_shield_bar(&shield, 0, 100.0f, 100.0f, 68, spans) == 2);
    CHECK(span_is(&spans[0], 0, 34, GDX_NET_SHIELD_BAR_CYAN));
    CHECK(span_is(&spans[1], 34, 68, GDX_NET_SHIELD_BAR_GREEN));

    CHECK(gdx_net_shield_bar(&shield, 0, 25.0f, 100.0f, 68, spans) == 2);
    CHECK(span_is(&spans[0], 0, 17, GDX_NET_SHIELD_BAR_CYAN));
    CHECK(span_is(&spans[1], 17, 34, GDX_NET_SHIELD_BAR_BLUE));

    CHECK(gdx_net_shield_bar(&shield, 0, 0.0f, 100.0f, 68, spans) == 1);
    CHECK(span_is(&spans[0], 0, 34, GDX_NET_SHIELD_BAR_BLUE));

    shield.pool = 0.0f;
    CHECK(gdx_net_shield_bar(&shield, 0, 50.0f, 100.0f, 68, spans) == 1);
    CHECK(span_is(&spans[0], 0, 34, GDX_NET_SHIELD_BAR_GREEN));
    CHECK(gdx_net_shield_bar(&shield, 0, 0.0f, 100.0f, 68, spans) == 0);
}

static void test_bar_flash_and_edges(void) {
    GdxNetShield shield = full_shield();
    GdxNetShieldSpan spans[2];

    shield.pool = 50.0f;
    shield.flash = 1;
    CHECK(gdx_net_shield_bar(&shield, 0, 100.0f, 100.0f, 68, spans) == 2);
    CHECK(span_is(&spans[0], 0, 34, GDX_NET_SHIELD_BAR_YELLOW));
    CHECK(span_is(&spans[1], 34, 68, GDX_NET_SHIELD_BAR_YELLOW));
    CHECK(gdx_net_shield_bar(&shield, 1, 50.0f, 100.0f, 68, spans) == 2);
    CHECK(spans[0].colour == GDX_NET_SHIELD_BAR_YELLOW && spans[1].colour == GDX_NET_SHIELD_BAR_YELLOW);

    shield.flash = 0;
    shield.pool = 100.0f;
    CHECK(gdx_net_shield_bar(&shield, 0, 100.0f, 100.0f, 0, spans) == 0);
    /* Rounded to the nearest pixel and kept inside the bar. */
    CHECK(gdx_net_shield_bar(&shield, 0, 150.0f, 100.0f, 68, spans) == 1);
    CHECK(span_is(&spans[0], 0, 68, GDX_NET_SHIELD_BAR_CYAN));
    CHECK(gdx_net_shield_bar(&shield, 0, 0.75f, 100.0f, 68, spans) == 2);
    CHECK(span_is(&spans[0], 0, 1, GDX_NET_SHIELD_BAR_CYAN));
    CHECK(gdx_net_shield_bar(&shield, 0, 0.5f, 100.0f, 68, spans) == 1);
    CHECK(span_is(&spans[0], 0, 68, GDX_NET_SHIELD_BAR_BLUE));
    /* The split screen's narrower bar: 51 pixels. */
    shield.pool = 50.0f;
    CHECK(gdx_net_shield_bar(&shield, 0, 100.0f, 100.0f, 51, spans) == 2);
    CHECK(span_is(&spans[0], 0, 26, GDX_NET_SHIELD_BAR_CYAN));
    CHECK(span_is(&spans[1], 26, 51, GDX_NET_SHIELD_BAR_GREEN));
    /* A machine of no max energy draws nothing rather than dividing by 0. */
    gdx_net_shield_reset(&shield, 0.0f);
    CHECK(gdx_net_shield_bar(&shield, 0, 0.0f, 0.0f, 68, spans) == 0);
}

/* The game-side calls do nothing while the rule is off, and keep one shield for each racer. */
static void test_racers(void) {
    int out[6];

    sRule = 0;
    CHECK(gdx_net_shield_on() == 0);
    gdx_net_shield_racer_init(3, 100.0f);
    CHECK(gdx_net_shield_racer_take(3, 0, 100.0f, 30.0f, GDX_NET_SHIELD_HIT) == 30.0f);
    CHECK(gdx_net_shield_racer_take(3, 1, 10.0f, 40.0f, GDX_NET_SHIELD_HIT) == 40.0f);
    CHECK(gdx_net_shield_racer_bar(3, 0, 100.0f, 100.0f, 68, out) == 0);

    sRule = 1;
    CHECK(gdx_net_shield_on() == 1);
    gdx_net_shield_racer_init(3, 100.0f);
    gdx_net_shield_racer_init(4, 100.0f);
    CHECK(gdx_net_shield_racer_take(3, 0, 100.0f, 30.0f, GDX_NET_SHIELD_HIT) == 0.0f);
    CHECK(gdx_net_shield_racer_take(3, 0, 100.0f, 80.0f, GDX_NET_SHIELD_HIT) == 10.0f);
    /* Racer 4 keeps its own full shield. */
    CHECK(gdx_net_shield_racer_take(4, 0, 100.0f, 30.0f, GDX_NET_SHIELD_HIT) == 0.0f);
    CHECK(gdx_net_shield_racer_bar(4, 0, 100.0f, 100.0f, 68, out) == 2);
    CHECK(out[0] == 0 && out[1] == 48 && out[2] == GDX_NET_SHIELD_BAR_YELLOW);
    CHECK(out[3] == 48 && out[4] == 68 && out[5] == GDX_NET_SHIELD_BAR_YELLOW);
    {
        int i;
        for (i = 0; i < GDX_NET_SHIELD_FLASH_FRAMES; i++) {
            gdx_net_shield_racer_frame(4);
        }
    }
    CHECK(gdx_net_shield_racer_bar(4, 0, 100.0f, 100.0f, 68, out) == 2);
    CHECK(out[2] == GDX_NET_SHIELD_BAR_CYAN && out[5] == GDX_NET_SHIELD_BAR_GREEN);
    gdx_net_shield_racer_pit(3, 100.0f, 100.0f, 0.5f, 0);
    CHECK(gdx_net_shield_racer_take(3, 0, 100.0f, 1.0f, GDX_NET_SHIELD_HIT) == 0.75f);

    /* Out of range: the damage passes, nothing is drawn, nothing is written. */
    CHECK(gdx_net_shield_racer_take(-1, 0, 100.0f, 30.0f, GDX_NET_SHIELD_HIT) == 30.0f);
    CHECK(gdx_net_shield_racer_take(GDX_NET_SHIELD_RACERS, 0, 100.0f, 30.0f, GDX_NET_SHIELD_HIT) == 30.0f);
    CHECK(gdx_net_shield_racer_bar(GDX_NET_SHIELD_RACERS, 0, 100.0f, 100.0f, 68, out) == 0);
    gdx_net_shield_racer_init(-1, 100.0f);
    gdx_net_shield_racer_pit(GDX_NET_SHIELD_RACERS, 100.0f, 100.0f, 0.5f, 0);
    gdx_net_shield_racer_frame(-1);

    /* A race that starts with the rule off still resets the pool, so the next race on starts full. */
    sRule = 0;
    gdx_net_shield_racer_init(3, 100.0f);
    sRule = 1;
    CHECK(gdx_net_shield_racer_take(3, 0, 100.0f, 100.0f, GDX_NET_SHIELD_HIT) == 0.0f);
    sRule = 0;
}

int main(void) {
    test_reset();
    test_shield_absorbs_first();
    test_lap_one_halves();
    test_lap_one_floor();
    test_lava_kills();
    test_no_hit();
    test_flash();
    test_pit();
    test_bar_lap_one();
    test_bar_states();
    test_bar_flash_and_edges();
    test_racers();
    if (sFailures != 0) {
        printf("gdx_net_shield_tests: %d of %d checks failed\n", sFailures, sChecks);
        return 1;
    }
    printf("gdx_net_shield_tests: all %d checks passed\n", sChecks);
    return 0;
}
