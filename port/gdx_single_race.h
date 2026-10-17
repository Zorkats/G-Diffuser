/* port/gdx_single_race.h -- Single Race rules for the one-player Practice race.
 *
 * Rationale and the full rule list live in gdx_single_race.c. Decomp translation units declare
 * these entry points inline with `extern` rather than including this header, matching the
 * convention used by the other port hooks.
 */
#ifndef GDX_SINGLE_RACE_H
#define GDX_SINGLE_RACE_H

#ifdef __cplusplus
extern "C" {
#endif

/* Racer slots the stock game allocates (gRacers[30]). */
#define GDX_SINGLE_RACE_MAX_RACERS 30
/* Lap-time slots kept per racer by a finite Single Race (Laps up to 9). */
#define GDX_SINGLE_RACE_MAX_LAPS 9

/* Called once per race start from Racer_Init. `eligible` is non-zero for a one-player Practice
 * race outside the title demo. Reads the Enhancements > Single Race options and fixes the rules
 * for this race. Every other entry point reports the snapshot taken here. */
void gdx_single_race_begin_race(int eligible);

/* Non-zero while the race in progress runs under Single Race rules. */
int gdx_single_race_active(void);

/* Non-zero while an active Single Race has a finite lap count (3, 5, 7 or 9). */
int gdx_single_race_finite(void);

/* Lap count for the race in progress: the chosen count for a finite Single Race, otherwise 3. */
int gdx_single_race_lap_count(void);

/* Racer count for the race in progress: 1 + CPU count while active, otherwise 30. */
int gdx_single_race_racer_count(void);

/* Racer index of the random rival, or -1 when random rivals are off. */
int gdx_single_race_random_rival(void);

/* Stores a completed lap time while a Single Race is active. Ignored otherwise. */
void gdx_single_race_store_lap_time(int racerId, int lapIndex, int lapTime);

/* Returns a lap time stored during the current Single Race, or 0 when out of range. */
int gdx_single_race_get_lap_time(int racerId, int lapIndex);

#ifdef __cplusplus
}
#endif

#endif /* GDX_SINGLE_RACE_H */
