/* port/gdx_split_gp.h -- split-screen Grand Prix rules (plan item MP2). Pure C: no IO, no allocation.
 * Copyright (c) Zorkats
 *
 * A Grand Prix with two to four players on one screen. Enhancements > Multiplayer > Split-screen Grand Prix
 * lets Start on a free controller join at machine select; the decomp glue lives in decomp/src/game/racer.c.
 * This file holds the rules only, so the tests can drive them without the game.
 *
 * Players are slots 0 .. numPlayers - 1, and a mask bit (1u << slot) stands for each. Every player starts
 * the cup with the stock lives count. A race ends when every player still in the cup has finished or
 * retired. Then each retired player loses a life, or leaves the cup when they have no life left. When any
 * player finished, the race counts and the cup goes on. When nobody finished, the race is run again for the
 * players still in, and the cup is over once nobody is left. A player out of the cup keeps watching their
 * machine race on autopilot and scores no points.
 */

#ifndef GDX_SPLIT_GP_H
#define GDX_SPLIT_GP_H

/* No system headers here. Decomp translation units include this file, and host <stddef.h> and
 * <stdint.h> clash with the decomp libc headers there, so every type below is a plain C type. */

#ifdef __cplusplus
extern "C" {
#endif

#define GDX_SPLIT_GP_MAX_PLAYERS 4
#define GDX_SPLIT_GP_MAX_RACERS 30
#define GDX_SPLIT_GP_FIELD_DEFAULT 30
/* KOs a player needs in one race for an extra life, as in the one-player Grand Prix. */
#define GDX_SPLIT_GP_KO_LIFE 5

/* The outcome of a finished race. */
#define GDX_SPLIT_GP_COUNTS 0    /* someone finished: points are scored and the cup goes on */
#define GDX_SPLIT_GP_RETRY 1     /* nobody finished, someone is still in: run the race again */
#define GDX_SPLIT_GP_GAME_OVER 2 /* nobody is left in the cup */

/* The number of machines in the race: requested, clamped to numPlayers .. 30. numPlayers is clamped to
 * 1 .. 4 first. */
int gdx_split_gp_field(int numPlayers, int requested);

/* 1 when every player not in outMask is in finishedMask or retiredMask, else 0. With every player out the
 * race is over as well. Bits at or above numPlayers are ignored. */
int gdx_split_gp_race_over(int numPlayers, unsigned finishedMask, unsigned retiredMask, unsigned outMask);

/* Applies the end-of-race lives rule and returns GDX_SPLIT_GP_COUNTS, _RETRY or _GAME_OVER. Each player
 * still in who retired and has a life loses one and is set in *lostMask; one with no life left is added to
 * *outMask. lives holds numPlayers entries. *lostMask is cleared first. Returns -1, changing nothing, when
 * an argument is NULL or numPlayers is outside 1 .. 4. */
int gdx_split_gp_resolve(int numPlayers, unsigned finishedMask, unsigned retiredMask, unsigned* outMask,
                         short* lives, unsigned* lostMask);

/* The pause menu's Retry and Settings restart the race, and cost a life to each player still in who has not
 * finished. 1 when every one of them has a life to spend, else 0. Returns 0 for a bad argument. */
int gdx_split_gp_retry_allowed(int numPlayers, unsigned finishedMask, unsigned outMask, const short* lives);

/* Spends that life: each player still in who has not finished loses one, and is set in *lostMask (cleared
 * first). A player at 0 keeps 0. Changes nothing for a bad argument. */
void gdx_split_gp_retry_cost(int numPlayers, unsigned finishedMask, unsigned outMask, short* lives,
                             unsigned* lostMask);

/* The first grid of a cup: the players start at the back, player 1 last, player 2 ahead of them and so on,
 * and the CPU machines keep their order ahead of the players. positions holds total entries, the grid
 * position 1 .. total of each slot, and must be a permutation of 1 .. total. Returns 1 and rewrites it, or
 * 0, changing nothing, when an argument is out of range or positions is not a permutation. */
int gdx_split_gp_back_grid(int* positions, int total, int numPlayers);

/* Decomp glue. Defined in decomp/src/game/racer.c next to the lives and race-end code, except the last one,
 * which decomp/src/overlays/ovl_i3/hud.c defines. The tests above never link them. */

/* 1 while a Grand Prix race runs with two or more players. Only the machine-select drop-in reads the
 * Enhancements toggle, so a cup that started split stays split. */
int gdx_split_gp_active(void);
/* 1 when player slot `player` has left the cup. */
int gdx_split_gp_player_out(int player);
/* The outcome of the race that just ended, GDX_SPLIT_GP_COUNTS, _RETRY or _GAME_OVER, resolving the lives the
 * first time it is asked. -1 while the race still runs. */
int gdx_split_gp_race_outcome(void);
/* The pause menu's Retry and Settings: whether every player still racing has a life to spend, and the
 * spending itself. */
int gdx_split_gp_pause_retry_allowed(void);
void gdx_split_gp_pause_retry_pay(void);
/* hud.c: flashes the life icon player `player` just gained or lost; `previous` is the count before. */
void gdx_split_gp_hud_lives_changed(int player, int previous);

#ifdef __cplusplus
}
#endif

#endif /* GDX_SPLIT_GP_H */
