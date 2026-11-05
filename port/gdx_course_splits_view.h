/* port/gdx_course_splits_view.h -- staff run splits (gdx_course_splits.h) in plain C types, for the decomp side.
 *
 * Like gdx_course_meta_view.h, nothing here includes <stddef.h> or <stdint.h>, so global.h includers can use it.
 *
 * Recording: a single-player Time Attack run on a course file records the player's race time at every checkpoint
 * and at every lap end (decomp/src/game/racer.c). When the run is stored as the staff time
 * (gdx_meta_view_set_staff), its splits are stored with it as GXM1 tag 5.
 *
 * Checkpoints: node n is where the lap distance reaches the start of stock segment n + 1, as a share of the lap.
 * Fork B spans are scaled onto the stock ring (gdx_course_branch.h), so the shares hold on either path.
 *
 * Pacing: on Expert and Master, a track whose CPU target comes from its staff time (cpu.c func_i3_80116974) paces
 * its CPUs from lap 2 on along the creator's run, instead of the stock straight line from the end of lap 1 to the
 * target. When the course's checkpoint count no longer matches the record, or the record has lap times only, the
 * CPUs follow the creator's lap times.
 */

#ifndef GDX_COURSE_SPLITS_VIEW_H
#define GDX_COURSE_SPLITS_VIEW_H

#ifdef __cplusplus
extern "C" {
#endif

/* Racer_Init: starts recording when the race is a single-player Time Attack run on a course file. */
void gdx_splits_view_race_init(void);

/* Every frame the player's lap logic runs (racer.c), after raceDistance is updated. lapShare is lapDistance over
 * the course length. Values are the racer's fields as they stand at the end of that frame. */
void gdx_splits_view_player_moved(int lap, int lapsCompleted, float lapShare, int raceTime, int completedLapsTime);

/* cpu.c func_i3_80116974, once per race: creatorPaced is non-zero when the CPU target came from the raced track's
 * staff time. Loads its splits for gdx_splits_view_cpu_pace on Expert and Master, else turns the pacing off. */
void gdx_splits_view_pace_begin(int creatorPaced, int difficulty);

/* cpu.c, a CPU on lap 2 or later: how far the CPU is behind the creator's pace, now and one CPU step (17 ms)
 * earlier, in course distance. elapsed is the CPU's race time past its first lap and window is its target minus
 * its first lap, as the stock code uses them. Returns 1 when the outputs were written, 0 to keep the stock pace. */
int gdx_splits_view_cpu_pace(int elapsed, int window, float lapLength, float raceDistance, float* outBehind,
                             float* outBehindBefore);

/* gdx_course_meta_view.c: the splits of the run that just finished, for a staff time of raceTime over raceLaps.
 * Prefers the checkpoint record and falls back to lap times. Returns 1 when out holds a record that matches. */
struct GdxCourseSplits;
int gdx_splits_view_finished(int raceLaps, int raceTime, struct GdxCourseSplits* out);

/* Implemented in gdx_course_splits_course.c, which reads the decomp course data. */

/* Writes the node positions of the raced course to nodeAt and returns the node count, 1..cap. Returns 0 when the
 * course has no usable checkpoints. */
int gdx_splits_course_nodes(float* nodeAt, int cap);

/* 1 when the race is a single-player Time Attack run. */
int gdx_splits_course_recordable(void);

/* gTotalLapCount. */
int gdx_splits_course_laps(void);

#ifdef __cplusplus
}
#endif

#endif /* GDX_COURSE_SPLITS_VIEW_H */
