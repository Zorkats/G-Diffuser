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
/* Highest lap count of a finite Single Race, and the lap-time slots kept per racer. */
#define GDX_SINGLE_RACE_MAX_LAPS 99

/* Carries the old Laps combo index over to LapCount and UnlimitedLaps. Safe to call every start. */
void gdx_single_race_migrate_settings(void);

/* Called once per race start from Racer_Init. `eligible` is non-zero for a one-player Practice
 * race outside the title demo. Reads the Enhancements > Single Race options and fixes the rules
 * for this race. Every other entry point reports the snapshot taken here. */
void gdx_single_race_begin_race(int eligible);

/* Non-zero while the race in progress runs under Single Race rules. */
int gdx_single_race_active(void);

/* Non-zero while an active Single Race has a finite lap count (1 to 99). */
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

/* Results screen paging. A race longer than the rows above the portraits shows one page of laps at
 * a time. Rows on screen per page: */
int gdx_single_race_result_rows(int totalLaps);

/* Zero-based index of the first lap on the page shown `frame` frames into the results. */
int gdx_single_race_result_first_lap(int totalLaps, int frame);

/* Frames the results stay up before moving on, long enough to show every page once. */
int gdx_single_race_result_hold_frames(int totalLaps);

#ifdef __cplusplus
}
#endif

#endif /* GDX_SINGLE_RACE_H */
