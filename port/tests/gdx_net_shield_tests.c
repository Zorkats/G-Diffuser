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

static int bar_is(const GdxNetShieldBar* bar, int health, int healthColour, int shield, int shieldColour) {
    return (bar->health == health) && (bar->healthColour == healthColour) && (bar->shield == shield) &&
           (bar->shieldColour == shieldColour);
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

/* Lap 1: the stock red health bar over a full shield bar, the super shield. */
static void test_bar_lap_one(void) {
    GdxNetShield shield = full_shield();
    GdxNetShieldBar bar;

    gdx_net_shield_bar(&shield, 1, 50.0f, 100.0f, 68, &bar);
    CHECK(bar_is(&bar, 34, GDX_NET_SHIELD_BAR_RED, 68, GDX_NET_SHIELD_BAR_BLUE));
    /* The pool plays no part on lap 1: the shield bar stays full. */
    shield.pool = 0.0f;
    gdx_net_shield_bar(&shield, 1, 100.0f, 100.0f, 68, &bar);
    CHECK(bar_is(&bar, 68, GDX_NET_SHIELD_BAR_RED, 68, GDX_NET_SHIELD_BAR_BLUE));
    gdx_net_shield_bar(&shield, 1, 0.0f, 100.0f, 68, &bar);
    CHECK(bar_is(&bar, 0, GDX_NET_SHIELD_BAR_RED, 68, GDX_NET_SHIELD_BAR_BLUE));
}

/* From lap 2 each bar shows its own number, whichever is the larger: the case of the second brother test,
   where a shield longer than the health hid the health's edge, is the second one here. */
static void test_bar_states(void) {
    GdxNetShield shield = full_shield();
    GdxNetShieldBar bar;

    gdx_net_shield_bar(&shield, 0, 100.0f, 100.0f, 68, &bar);
    CHECK(bar_is(&bar, 68, GDX_NET_SHIELD_BAR_GREEN, 68, GDX_NET_SHIELD_BAR_BLUE));

    shield.pool = 80.0f;
    gdx_net_shield_bar(&shield, 0, 25.0f, 100.0f, 68, &bar);
    CHECK(bar_is(&bar, 17, GDX_NET_SHIELD_BAR_GREEN, 54, GDX_NET_SHIELD_BAR_BLUE));

    shield.pool = 25.0f;
    gdx_net_shield_bar(&shield, 0, 75.0f, 100.0f, 68, &bar);
    CHECK(bar_is(&bar, 51, GDX_NET_SHIELD_BAR_GREEN, 17, GDX_NET_SHIELD_BAR_BLUE));

    shield.pool = 0.0f;
    gdx_net_shield_bar(&shield, 0, 50.0f, 100.0f, 68, &bar);
    CHECK(bar_is(&bar, 34, GDX_NET_SHIELD_BAR_GREEN, 0, GDX_NET_SHIELD_BAR_BLUE));
    gdx_net_shield_bar(&shield, 0, 0.0f, 100.0f, 68, &bar);
    CHECK(bar_is(&bar, 0, GDX_NET_SHIELD_BAR_GREEN, 0, GDX_NET_SHIELD_BAR_BLUE));
}

static void test_bar_flash_and_edges(void) {
    GdxNetShield shield = full_shield();
    GdxNetShieldBar bar;

    /* A hit turns the shield bar yellow and leaves the health its own colour. */
    shield.pool = 50.0f;
    shield.flash = 1;
    gdx_net_shield_bar(&shield, 0, 100.0f, 100.0f, 68, &bar);
    CHECK(bar_is(&bar, 68, GDX_NET_SHIELD_BAR_GREEN, 34, GDX_NET_SHIELD_BAR_YELLOW));
    gdx_net_shield_bar(&shield, 1, 50.0f, 100.0f, 68, &bar);
    CHECK(bar_is(&bar, 34, GDX_NET_SHIELD_BAR_RED, 68, GDX_NET_SHIELD_BAR_YELLOW));

    shield.flash = 0;
    shield.pool = 100.0f;
    gdx_net_shield_bar(&shield, 0, 100.0f, 100.0f, 0, &bar);
    CHECK(bar.health == 0 && bar.shield == 0);
    gdx_net_shield_bar(&shield, 1, 100.0f, 100.0f, -4, &bar);
    CHECK(bar.health == 0 && bar.shield == 0);
    /* Rounded to the nearest pixel and kept inside the bar. */
    gdx_net_shield_bar(&shield, 0, 150.0f, 100.0f, 68, &bar);
    CHECK(bar.health == 68);
    gdx_net_shield_bar(&shield, 0, 0.75f, 100.0f, 68, &bar);
    CHECK(bar.health == 1);
    gdx_net_shield_bar(&shield, 0, 0.5f, 100.0f, 68, &bar);
    CHECK(bar.health == 0 && bar.shield == 68);
    /* The split screen's narrower bar: 51 pixels. */
    shield.pool = 50.0f;
    gdx_net_shield_bar(&shield, 0, 100.0f, 100.0f, 51, &bar);
    CHECK(bar_is(&bar, 51, GDX_NET_SHIELD_BAR_GREEN, 26, GDX_NET_SHIELD_BAR_BLUE));
    /* A machine of no max energy draws nothing rather than dividing by 0. */
    gdx_net_shield_reset(&shield, 0.0f);
    gdx_net_shield_bar(&shield, 0, 0.0f, 0.0f, 68, &bar);
    CHECK(bar.health == 0 && bar.shield == 0);
}

/* The game-side calls do nothing while the rule is off, and keep one shield for each racer. */
static void test_racers(void) {
    int out[4] = { -1, -1, -1, -1 };

    sRule = 0;
    CHECK(gdx_net_shield_on() == 0);
    gdx_net_shield_racer_init(3, 100.0f);
    CHECK(gdx_net_shield_racer_take(3, 0, 100.0f, 30.0f, GDX_NET_SHIELD_HIT) == 30.0f);
    CHECK(gdx_net_shield_racer_take(3, 1, 10.0f, 40.0f, GDX_NET_SHIELD_HIT) == 40.0f);
    CHECK(gdx_net_shield_racer_bar(3, 0, 100.0f, 100.0f, 68, out) == 0);
    CHECK(out[0] == -1 && out[3] == -1);

    sRule = 1;
    CHECK(gdx_net_shield_on() == 1);
    gdx_net_shield_racer_init(3, 100.0f);
    gdx_net_shield_racer_init(4, 100.0f);
    CHECK(gdx_net_shield_racer_take(3, 0, 100.0f, 30.0f, GDX_NET_SHIELD_HIT) == 0.0f);
    CHECK(gdx_net_shield_racer_take(3, 0, 100.0f, 80.0f, GDX_NET_SHIELD_HIT) == 10.0f);
    /* Racer 4 keeps its own full shield. */
    CHECK(gdx_net_shield_racer_take(4, 0, 100.0f, 30.0f, GDX_NET_SHIELD_HIT) == 0.0f);
    CHECK(gdx_net_shield_racer_bar(4, 0, 100.0f, 100.0f, 68, out) == 1);
    CHECK(out[0] == 68 && out[1] == GDX_NET_SHIELD_BAR_GREEN && out[2] == 48 && out[3] == GDX_NET_SHIELD_BAR_YELLOW);
    {
        int i;
        for (i = 0; i < GDX_NET_SHIELD_FLASH_FRAMES; i++) {
            gdx_net_shield_racer_frame(4);
        }
    }
    CHECK(gdx_net_shield_racer_bar(4, 0, 100.0f, 100.0f, 68, out) == 1);
    CHECK(out[2] == 48 && out[3] == GDX_NET_SHIELD_BAR_BLUE);
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
