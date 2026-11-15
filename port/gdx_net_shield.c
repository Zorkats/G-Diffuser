/* port/gdx_net_shield.c -- the G-NET shield rule (slice G4). See gdx_net_shield.h.
 * Copyright (c) Zorkats */

#include "gdx_net_shield.h"

static GdxNetShield sShields[GDX_NET_SHIELD_RACERS];

void gdx_net_shield_reset(GdxNetShield* shield, float maxEnergy) {
    if (maxEnergy < 0.0f) {
        maxEnergy = 0.0f;
    }
    shield->pool = maxEnergy;
    shield->maxPool = maxEnergy;
    shield->flash = 0;
}

float gdx_net_shield_take(GdxNetShield* shield, int lapOne, float energy, float damage, int kind) {
    float absorbed;

    /* Lava drains energy whatever the shield holds, and a push of 0 or less is no hit. */
    if ((kind == GDX_NET_SHIELD_LAVA) || !(damage > 0.0f)) {
        return damage;
    }
    shield->flash = GDX_NET_SHIELD_FLASH_FRAMES;
    if (lapOne) {
        damage *= 0.5f;
        if (energy - damage < GDX_NET_SHIELD_LAP_ONE_FLOOR) {
            damage = energy - GDX_NET_SHIELD_LAP_ONE_FLOOR;
            if (damage < 0.0f) {
                damage = 0.0f; /* already under the floor, by lava or a boost: a hit takes nothing more */
            }
        }
        return damage;
    }
    absorbed = (damage < shield->pool) ? damage : shield->pool;
    shield->pool -= absorbed;
    return damage - absorbed;
}

void gdx_net_shield_pit(GdxNetShield* shield, float energy, float maxEnergy, float energyIncrease, int boosting) {
    if (boosting || (energy < maxEnergy) || !(energyIncrease > 0.0f)) {
        return;
    }
    shield->pool += energyIncrease * 0.5f;
    if (shield->pool > shield->maxPool) {
        shield->pool = shield->maxPool;
    }
}

void gdx_net_shield_frame(GdxNetShield* shield) {
    if (shield->flash > 0) {
        shield->flash--;
    }
}

/* fraction of width, rounded to the nearest pixel and kept inside the bar. */
static int bar_pixels(float value, float maxValue, int width) {
    float pixels;

    if (!(maxValue > 0.0f) || !(value > 0.0f)) {
        return 0;
    }
    pixels = (value / maxValue) * (float) width + 0.5f;
    if (pixels >= (float) width) {
        return width;
    }
    return (int) pixels;
}

void gdx_net_shield_bar(const GdxNetShield* shield, int lapOne, float energy, float maxEnergy, int width,
                        GdxNetShieldBar* out) {
    if (width < 0) {
        width = 0;
    }
    out->health = bar_pixels(energy, maxEnergy, width);
    out->healthColour = lapOne ? GDX_NET_SHIELD_BAR_RED : GDX_NET_SHIELD_BAR_GREEN;
    out->shield = lapOne ? width : bar_pixels(shield->pool, shield->maxPool, width);
    out->shieldColour = (shield->flash > 0) ? GDX_NET_SHIELD_BAR_YELLOW : GDX_NET_SHIELD_BAR_BLUE;
}

int gdx_net_shield_on(void) {
    return gdx_net_race_rule_shield() ? 1 : 0;
}

static GdxNetShield* racer_shield(int racer) {
    if ((racer < 0) || (racer >= GDX_NET_SHIELD_RACERS)) {
        return 0;
    }
    return &sShields[racer];
}

void gdx_net_shield_racer_init(int racer, float maxEnergy) {
    GdxNetShield* shield = racer_shield(racer);

    /* Reset whatever the rule says: a race that starts with the shield off leaves no stale pool behind. */
    if (shield != 0) {
        gdx_net_shield_reset(shield, maxEnergy);
    }
}

float gdx_net_shield_racer_take(int racer, int lapOne, float energy, float damage, int kind) {
    GdxNetShield* shield = racer_shield(racer);

    if ((shield == 0) || !gdx_net_shield_on()) {
        return damage;
    }
    return gdx_net_shield_take(shield, lapOne, energy, damage, kind);
}

void gdx_net_shield_racer_pit(int racer, float energy, float maxEnergy, float energyIncrease, int boosting) {
    GdxNetShield* shield = racer_shield(racer);

    if ((shield != 0) && gdx_net_shield_on()) {
        gdx_net_shield_pit(shield, energy, maxEnergy, energyIncrease, boosting);
    }
}

void gdx_net_shield_racer_frame(int racer) {
    GdxNetShield* shield = racer_shield(racer);

    if ((shield != 0) && gdx_net_shield_on()) {
        gdx_net_shield_frame(shield);
    }
}

int gdx_net_shield_racer_bar(int racer, int lapOne, float energy, float maxEnergy, int width, int out[4]) {
    GdxNetShield* shield = racer_shield(racer);
    GdxNetShieldBar bar;

    if ((shield == 0) || !gdx_net_shield_on()) {
        return 0;
    }
    gdx_net_shield_bar(shield, lapOne, energy, maxEnergy, width, &bar);
    out[0] = bar.health;
    out[1] = bar.healthColour;
    out[2] = bar.shield;
    out[3] = bar.shieldColour;
    return 1;
}
