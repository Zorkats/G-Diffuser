/* port/gdx_net_shield.h -- the G-NET shield rule (slice G4).
 * Copyright (c) Zorkats
 *
 * A room whose rules turn the shield on gives every machine a second pool beside its energy:
 *
 * - Lap 1, the super shield. Every hit does half its damage, and no hit takes energy below
 *   GDX_NET_SHIELD_LAP_ONE_FLOOR. Nobody is knocked out on lap 1 except by lava.
 * - From lap 2, the shield. A pool as large as the machine's max energy takes the damage of every hit before
 *   energy does; what a hit leaves over goes to energy.
 * - Lava drains energy past the shield on every lap, and falling off the course is a crash, not damage, so
 *   both still kill.
 * - The pool refills only in the pit, at half the pit's energy rate, once energy is full and while the
 *   machine is not boosting.
 *
 * "Lap 1" is the game's own state before a machine may boost (RACER_STATE_CAN_BOOST): a room's race is a
 * finite Single Race, which grants the boost when the first lap is done.
 *
 * The core below takes numbers and returns numbers, so the tests need no game code. The game-side calls,
 * per racer, keep one GdxNetShield for each racer and do nothing while the room's rules leave the shield
 * off. Each copy damages only its own machines (a puppet takes no damage), so the rule needs no message.
 *
 * Game thread only. */

#ifndef GDX_NET_SHIELD_H
#define GDX_NET_SHIELD_H

#ifdef __cplusplus
extern "C" {
#endif

/* The lowest energy a hit leaves on lap 1. */
#define GDX_NET_SHIELD_LAP_ONE_FLOOR 1.0f
/* How many game frames the bar stays yellow after a hit. */
#define GDX_NET_SHIELD_FLASH_FRAMES 6
/* The most racers a race has (the game's gRacers). */
#define GDX_NET_SHIELD_RACERS 30

/* Where a damage comes from. A hit is a machine, a wall, the course edge or a mine; lava is the strip's drain. */
#define GDX_NET_SHIELD_HIT 0
#define GDX_NET_SHIELD_LAVA 1

typedef struct GdxNetShield {
    float pool;    /* what is left of the shield, 0 to maxPool */
    float maxPool; /* the machine's max energy */
    int flash;     /* frames left of the yellow flash */
} GdxNetShield;

/* A full shield for a machine with maxEnergy. */
void gdx_net_shield_reset(GdxNetShield* shield, float maxEnergy);
/* The part of damage that energy takes, the shield having taken its own. lapOne is 1 before the machine may
   boost. A hit of more than 0 starts the flash. */
float gdx_net_shield_take(GdxNetShield* shield, int lapOne, float energy, float damage, int kind);
/* One game frame in the pit, after the stock refill: the pool gains half of energyIncrease when energy is
   full and the machine is not boosting. */
void gdx_net_shield_pit(GdxNetShield* shield, float energy, float maxEnergy, float energyIncrease, int boosting);
/* One game frame: the flash counts down. */
void gdx_net_shield_frame(GdxNetShield* shield);

/* The colours of the bar. */
#define GDX_NET_SHIELD_BAR_RED 0    /* energy on lap 1 */
#define GDX_NET_SHIELD_BAR_GREEN 1  /* energy above the shield, or the lap-1 super shield */
#define GDX_NET_SHIELD_BAR_BLUE 2   /* shield above the energy */
#define GDX_NET_SHIELD_BAR_CYAN 3   /* both */
#define GDX_NET_SHIELD_BAR_YELLOW 4 /* a hit just landed */

/* A run of the bar's pixels, from from (included) to to (excluded), counted from its left end. */
typedef struct GdxNetShieldSpan {
    int from;
    int to;
    int colour;
} GdxNetShieldSpan;

/* The bar of width pixels as at most 2 spans, left to right, in spans. Past the last span the bar is empty.
   Returns how many spans there are.
   - Lap 1: energy red, then the super shield green to the bar's end.
   - Later: cyan as far as both reach, then green when energy reaches further, blue when the shield does.
   - While the flash runs, every span is yellow. */
int gdx_net_shield_bar(const GdxNetShield* shield, int lapOne, float energy, float maxEnergy, int width,
                       GdxNetShieldSpan spans[2]);

/* Game side, by racer id. Each does nothing, or hands damage back unchanged, while the room's rules leave
   the shield off (gdx_net_race_rule_shield). */
int gdx_net_shield_on(void);
void gdx_net_shield_racer_init(int racer, float maxEnergy);
float gdx_net_shield_racer_take(int racer, int lapOne, float energy, float damage, int kind);
void gdx_net_shield_racer_pit(int racer, float energy, float maxEnergy, float energyIncrease, int boosting);
void gdx_net_shield_racer_frame(int racer);
/* gdx_net_shield_bar for racer, with the spans as from, to, colour in out, for code that cannot include this
   header (ovl_i3/hud.c). Returns how many spans; 0 also when the shield is off or racer is out of range,
   so the HUD asks gdx_net_shield_on first. */
int gdx_net_shield_racer_bar(int racer, int lapOne, float energy, float maxEnergy, int width, int out[6]);

/* The room's rule, supplied by the race module (net/gdx_net_race.h); the tests supply their own. */
int gdx_net_race_rule_shield(void);

#ifdef __cplusplus
}
#endif

#endif
