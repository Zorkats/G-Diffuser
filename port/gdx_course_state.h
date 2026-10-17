/* port/gdx_course_state.h -- boot order and the persistence file of the Course Edit element store
 * (plan slice 9, scope A).
 *
 * Boot (gdx_course_boot, idempotent, game thread):
 *   1. music scan (mods/music/custom and music/custom, plan 7A),
 *   2. pack manifests (mods/<pack>/elements.txt, gdx_course_packs.h),
 *   3. the state file (course-elements.gdxs beside the exe).
 * Every menu, race, export and import entry point calls it, so the order holds even when no menu ran first.
 *
 * State file: course-elements.gdxs beside the exe, GXS1 format (gdx_course_registry.h). It is read once at
 * boot. It is rewritten by gdx_course_state_flush after a change to the store. A file that fails validation
 * is renamed to course-elements.gdxs.bad and logged; the store then starts empty and nothing is overwritten
 * until the next change.
 *
 * Threading: game thread only. The 64DD disk thread counts: it is a fiber on the game's OS thread
 * (port/n64_sched.c).
 */

#ifndef GDX_COURSE_STATE_H
#define GDX_COURSE_STATE_H

#ifdef __cplusplus
extern "C" {
#endif

/* Runs the boot steps once per process. Later calls return 0 at once. */
int gdx_course_boot(void);

/* Reads the state file into the store. A missing file is not an error. Returns 0 when nothing was
 * restored (no file, or the file was rejected and renamed), 1 when the store was restored, and a negative
 * GDX_COURSE_STATE_ERR_* code for a rejected file. Called by gdx_course_boot. */
int gdx_course_state_load(void);

/* Writes the store to the state file when it changed since the last write or load. Returns 1 when it
 * wrote, 0 when there was nothing to write or the store is not loaded yet, and a negative value on error
 * (reported in the log). Safe to call after every change. */
int gdx_course_state_flush(void);

/* A disk file was renamed (AB150.c disk op 15, after MFS reported success). The keys and the title of
 * the old course id move to the new one, then the state file is rewritten. Names and extensions are MFS
 * strings read up to their first NUL. A file that is not a course holds nothing, so nothing changes. */
void gdx_course_state_on_file_renamed(const char* oldName, const char* oldExtension, const char* newName,
                                      const char* newExtension);

/* A disk file was deleted (AB150.c disk op 16, after MFS reported success). Its keys and title are
 * dropped, so a new file saved under the same name starts with none. */
void gdx_course_state_on_file_deleted(const char* name, const char* extension);

/* The disk file the Course Edit course came from. Custom BGM and skybox choices are stored under its course
 * id. Set when a file is loaded or saved, cleared when the editor starts a course that has no file. A rename
 * or delete of that file (the two hooks above) follows it. The stock D_xk1_8003A598 cannot serve: Load never
 * fills it and Rename points it at whichever file was renamed. */
void gdx_course_state_set_edit_file(const char* name, const char* extension);
void gdx_course_state_clear_edit_file(void);

/* Writes the course id ("NAME.EXT") of the edited file. Returns 0 when the course has no file. */
int gdx_course_state_edit_file_id(char* out, int outCap);

#ifdef __cplusplus
}
#endif

#endif /* GDX_COURSE_STATE_H */
