/* port/gdx_course_meta_view.h -- gdx_course_meta.h in plain C types, for the decomp side.
 *
 * Like gdx_course_settings_view.h, nothing here includes <stddef.h> or <stdint.h>, so global.h includers can use
 * it. The raced course is the one gdx_racer_course_file_id (decomp/src/game/racer.c) names.
 */

#ifndef GDX_COURSE_META_VIEW_H
#define GDX_COURSE_META_VIEW_H

#ifdef __cplusplus
extern "C" {
#endif

/* gdx_meta_cpu_target for the raced course, or -1 when it has no staff time. */
int gdx_meta_view_cpu_target(int raceLaps, int difficulty, int playerRecord, const unsigned int* stockTargets,
                             const int* stockStaff, int courseCount);

/* A Course Edit save of the edit file finished: marks it made here, and clears its staff time unless
 * gEnhancements.CourseEdit.KeepStaffTime is on (the default). */
void gdx_meta_view_editor_saved(void);

/* Clears the staff time of courseId and writes the state file. 1 when the course had one to clear. */
int gdx_meta_view_clear_staff(const char* courseId);

/* 1 when the finished run should be offered as the raced course's staff time (gdx_meta_staff_offer). */
int gdx_meta_view_staff_offer(int raceLaps, int raceTime);

/* Stores the run as the raced course's staff time and writes the state file. 1 when stored. */
int gdx_meta_view_set_staff(int raceLaps, int raceTime);

/* The lines the course select shows under an Edit Cup track's name, author first and staff time below it. */
#define GDX_META_CAPTION_AUTHOR 0 /* "BY <author>" */
#define GDX_META_CAPTION_STAFF 1  /* "STAFF 1'23"456", plus " 5 LAPS" when the run was not 3 laps */
/* "BY <author> - SIGNED" when the track carries a signature (gdx_course_meta_sign.h), else nothing. Drawn in
 * place of the author line when it fits. */
#define GDX_META_CAPTION_AUTHOR_SIGNED 2

/* One caption line for the slot's NUL-terminated trackName, in characters the game font draws. Returns the
 * length, 0 when that part is not set. */
int gdx_meta_view_caption(const char* trackName, int line, char* out, int cap);

/* "BY <author>" for the raced course, shown under its name during the course intro. Returns the length, 0 when
 * no course file is raced or the course has no author. */
int gdx_meta_view_race_caption(char* out, int cap);

#ifdef __cplusplus
}
#endif

#endif /* GDX_COURSE_META_VIEW_H */
