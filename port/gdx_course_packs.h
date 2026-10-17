/* port/gdx_course_packs.h -- pack manifests (mods/<pack>/elements.txt) and the skybox menu glue of the
 * Course Edit element store (plan slice 9, scopes B and C).
 *
 * Packs: every folder under mods/ that holds an elements.txt is read, folders in name order. Each line is
 * one element (format in gdx_course_registry.h and docs/modding/COURSE_ELEMENTS.md). Accepted lines are
 * registered as extension entries; rejected lines are logged with their line number and skipped.
 *
 * Skyboxes: a skybox extension entry is a manifest line "skybox|key|name|fallback". The Background menu
 * lists them after the 8 stock skyboxes. Selecting one stores its key in the per-course store and sets the
 * course skybox byte to its fallback, a stock skybox (0..7). Custom skybox imagery is not supported.
 *
 * Int-only signatures: decomp files include this header, and the decomp include path shadows <stddef.h>.
 * Threading: game thread only.
 */

#ifndef GDX_COURSE_PACKS_H
#define GDX_COURSE_PACKS_H

#ifdef __cplusplus
extern "C" {
#endif

/* Loads every mods/<pack>/elements.txt beside the exe. Runs once per process (gdx_course_boot calls it).
 * Returns the number of elements registered. */
int gdx_course_packs_load(void);

/* Number of skybox extension entries (menu rows after the 8 stock rows). */
int gdx_course_skybox_count(void);

/* Display name of skybox extension entry extIndex (0-based), or NULL when out of range. */
const char* gdx_course_skybox_display_at(int extIndex);

/* Skybox extension index the course has chosen, or -1 when none. */
int gdx_course_skybox_choice(const char* courseId);

/* Stores (extIndex >= 0) or clears (extIndex < 0) the skybox record of the course. Returns the stock
 * skybox the course byte must hold (0..7) for an extension index, GDX_COURSE_OK when cleared, or a
 * negative GDX_COURSE_ERR_* code. Flushes the state file after a change. */
int gdx_course_skybox_set_choice(const char* courseId, int extIndex);

/* Stock skybox (0..7) a course's record resolves to at course load, or -1 when the course has no skybox
 * record or its key is not registered in this session (the stored skybox byte is then kept). */
int gdx_course_skybox_resolve(const char* courseId);

#ifdef __cplusplus
}
#endif

#endif /* GDX_COURSE_PACKS_H */
