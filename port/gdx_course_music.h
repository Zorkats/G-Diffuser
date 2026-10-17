/* port/gdx_course_music.h -- custom soundtracks for Course Edit tracks (plan section 7A).
 *
 * Music files in music/custom/ and mods/music/custom/ (*.ogg, *.flac, *.wav) are registered as BGM
 * extension entries with the key "bgm.custom.<stem>". The Course Edit BGM menu lists them after the
 * 14 stock entries. A choice is stored per course in the element store (gdx_course_registry.h); the
 * course's own BGM byte keeps the stock fallback. On a race, gdx_course_music_arm_for_course sets the
 * stream course override, and the stock start then plays the file through gdx_music_stream.
 *
 * Threading: all functions run on the game thread (menu, race start, export and import).
 */

#ifndef GDX_COURSE_MUSIC_H
#define GDX_COURSE_MUSIC_H

#ifdef __cplusplus
extern "C" {
#endif

/* Registers the music files found in mods/music/custom/ and music/custom/ beside the exe. The first
 * call scans; later calls return the count without rescanning. Returns the number of extension BGM
 * entries (0 when there are none). Boots the rest of the course element store (gdx_course_state.h). */
int gdx_course_music_scan(void);

/* The file-scan step of gdx_course_boot only. Scans once; later calls do nothing. Use gdx_course_music_scan
 * from other code. */
void gdx_course_music_scan_files(void);

/* Number of extension BGM entries, scanning first if needed. Menu row N >= 14 is extension N - 14. */
int gdx_course_music_count(void);

/* Display name of extension entry extIndex (0-based), or NULL when out of range. */
const char* gdx_course_music_display_at(int extIndex);

/* Builds "NAME.EXT" from a course file name (up to 20 bytes, an MFS name) and extension (5 bytes), each
 * read up to its first NUL. Returns 1 when out holds the id, 0 for an empty part or a short outCap.
 * Plain int sizes keep this header free of <stddef.h>, which the decomp include path shadows. */
int gdx_course_music_course_id(const char* name, const char* extension, char* out, int outCap);

/* Extension index the course has chosen, or -1 when it plays a stock track (or has no choice). */
int gdx_course_music_choice(const char* courseId);

/* Stores the choice for the course. extIndex < 0 removes the choice and returns GDX_COURSE_OK. For an
 * extension index it returns the stock BGM byte the course must hold (0 or more) or a negative
 * GDX_COURSE_ERR_* code. */
int gdx_course_music_set_choice(const char* courseId, int extIndex);

/* Starts the streamed preview of extension entry extIndex. Returns 1 when the stream accepted it. */
int gdx_course_music_preview(int extIndex);

/* Race start: sets the stream course override for bgmId to the course's chosen file, or clears it when
 * the course has no extension BGM choice. courseId may be NULL, which clears it. */
void gdx_course_music_arm_for_course(const char* courseId, int bgmId);

/* Export: copies the file of every extension BGM choice of the course into directory, named after its
 * key suffix. Returns the number copied. Failures are logged and skipped. */
int gdx_course_music_export_course(const char* courseId, const char* directory);

/* Import: makes the song for an extension BGM key available and writes the key the course must store
 * into outKey (outCap bytes, GDX_COURSE_KEY_MAX + 1 is enough). The song is <gdxcDirectory>/<suffix>.<ext>.
 * - No song beside the .gdxc: returns 1 when the key is registered, else 0.
 * - The key's own file holds the same bytes, or there is none yet: the song is copied into music/custom/
 *   under its suffix when absent, and outKey is key.
 * - The key's file holds a different song: the song is stored under
 *   gdx_course_registry_music_fingerprint_key instead (reused when that file already holds the same
 *   bytes), and outKey is that key.
 * Returns 1 when outKey is registered, and -1 when the song is over 32 MB, could not be copied, or its
 * fingerprint name holds yet another song (reported in the log). Non-music keys return 0. outKey holds
 * key whenever the return is not 1. */
int gdx_course_music_adopt(const char* gdxcPath, const char* key, char* outKey, int outCap);

#ifdef __cplusplus
}
#endif

#endif /* GDX_COURSE_MUSIC_H */
