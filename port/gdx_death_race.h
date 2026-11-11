/* port/gdx_death_race.h -- Death Race rules, course keys, and the best-time records store.
 *
 * The rules are a small state machine. A race starts on a course with a laps limit and ends either
 * cleared (every CPU machine is out) or out of laps (the limit ran out with machines left). Laps 0 is
 * the stock endless run; 1 to 99 is a limit. A winner is shown only for a clear under a limit, and only
 * a clear may be recorded. Course -1 and laps 0 mean that no race is running.
 *
 * Records keep one best time per course key and laps. Stock courses 0..23, the expansion disk cups and
 * fixed tracks 30..41, and 54 key as "stock-NN". Edit courses (24..29, 42..47, and 55 and up) key as
 * "edit-" plus their file id. X Cup courses 48..53 have no key. A key holds at most
 * GDX_DEATH_RACE_KEY_MAX - 1 characters, so an edit file id longer than 26 characters has no key.
 *
 * Wire format (version 1, little-endian, the whole store in one file):
 *   0       4     magic "GDXD"
 *   4       4     version u32, 1
 *   8       4     count u32, 0..512
 *   12      40    entry 0: key[32] NUL-padded, laps u8, 3 zero bytes, time s32 in ms
 *   ...           count entries in all, 40 bytes each
 *   12+40n  4     crc32 u32, CRC-32 (IEEE) over every byte before it
 *
 * Decode checks, in order: argument, length, magic, version, count, exact length, CRC, then each entry
 * (key, laps, time, and a duplicate key with the same laps). Padding bytes are not checked. A file that
 * fails any check is refused whole, and the store is left unchanged.
 */

#ifndef GDX_DEATH_RACE_H
#define GDX_DEATH_RACE_H

/* No system headers here. Decomp translation units include this file, and host <stddef.h> and
 * <stdint.h> clash with the decomp libc headers there, so every type below is a plain C type. */

#ifdef __cplusplus
extern "C" {
#endif

/* Race rules. Course indexes: 0..23 and 30..41 are stock courses (30..41 are the expansion disk cups and
 * fixed tracks), 24..29, 42..47, and 55 and up are edit courses, 48..53 are the X Cup, and 54 is Death Race. */
#define GDX_DEATH_RACE_MAX_LAPS 99
#define GDX_DEATH_RACE_STOCK_COURSE 54
#define GDX_DEATH_RACE_X_FIRST 48
#define GDX_DEATH_RACE_X_LAST 53
#define GDX_DEATH_RACE_STOCK_LAST 23
#define GDX_DEATH_RACE_EDIT_FIRST 24

typedef enum GdxDeathRaceOutcome {
    GDX_DEATH_RACE_RUNNING = 0,
    GDX_DEATH_RACE_CLEARED,     /* every CPU machine is out: the win */
    GDX_DEATH_RACE_OUT_OF_LAPS  /* the laps limit ran out with machines left: the loss */
} GdxDeathRaceOutcome;

/* Starts a race on courseIndex with a laps limit. laps is clamped to 0..99 (0 is endless), and the
 * outcome is reset to RUNNING. */
void gdx_death_race_begin(int courseIndex, int laps);
/* Ends the race: no course (-1), laps 0, outcome RUNNING. */
void gdx_death_race_end(void);
/* Laps limit of the current race. 0 is endless, and 0 is also the value when no race is running. */
int gdx_death_race_laps(void);
/* Course index of the current race, or -1 when none. */
int gdx_death_race_course(void);
/* 1 only for course 54 with endless laps, else 0. */
int gdx_death_race_stock_setup(void);
/* Reports the player's completed laps. Returns 1 only when a limit is set, completedLaps has reached
 * it, and the outcome is RUNNING. Returns 0 otherwise. rivalsStanding is the number of rivals that can
 * still race: not retired, and not already spinning out or falling to their end. That call sets CLEARED
 * when rivalsStanding is 0 or less, because the last rival is destroyed and only its explosion is still
 * playing, and OUT_OF_LAPS when a rival is left. */
int gdx_death_race_check_laps(int completedLaps, int rivalsStanding);
/* Sets CLEARED, but only while the outcome is RUNNING. */
void gdx_death_race_cleared(void);
/* Current outcome. */
GdxDeathRaceOutcome gdx_death_race_outcome(void);
/* 1 when the race is CLEARED under a laps limit, else 0. */
int gdx_death_race_show_winner(void);
/* 1 when the race is OUT_OF_LAPS, else 0. */
int gdx_death_race_show_loser(void);
/* 1 only when the race is CLEARED, so the result may be recorded. */
int gdx_death_race_may_record(void);
/* Completed laps of the player in the current race. gdx_death_race_lap_done adds one lap and returns the new
 * count. gdx_death_race_laps_done returns the count without changing it. begin and end set the count to 0. */
int gdx_death_race_lap_done(void);
int gdx_death_race_laps_done(void);

/* Record keys hold at most GDX_DEATH_RACE_KEY_MAX - 1 characters plus their NUL. */
#define GDX_DEATH_RACE_KEY_MAX 32

/* Writes the record key for a course into out as a NUL-terminated string. Returns 1 on success. Returns 0
 * when there is no key: X Cup, a negative index, an edit course without a file id, a key that does not fit
 * outCap or GDX_DEATH_RACE_KEY_MAX - 1 characters, or a NULL out. On failure out is set to "" when out and
 * outCap > 0. fileId is ignored for stock courses. */
int gdx_death_race_course_key(int courseIndex, const char* fileId, char* out, int outCap);

/* Best times: one per course key and laps (0..99), in milliseconds from 1 to GDX_DEATH_RACE_NO_TIME - 1. */
#define GDX_DEATH_RACE_RECORDS_MAX 512
#define GDX_DEATH_RACE_NO_TIME 3599999
#define GDX_DEATH_RACE_FILE_NAME "death_race_records.bin"

/* Empties the store. The directory and file name are kept. */
void gdx_death_race_records_reset(void);
int gdx_death_race_records_count(void);
/* Best time for key and laps, or GDX_DEATH_RACE_NO_TIME when none is stored or an argument is invalid. */
int gdx_death_race_record_get(const char* key, int laps);
/* Stores timeMs for key and laps when there is no time yet, or when timeMs is strictly faster. Returns 1
 * only when stored. Returns 0 for a slower or equal time, a NULL, empty or over-long key, laps outside
 * 0..99, a time outside 1..GDX_DEATH_RACE_NO_TIME - 1, or a full store with a new key. */
int gdx_death_race_record_submit(const char* key, int laps, int timeMs);

/* Bytes the whole store takes as a file: 16 + 40 * count. */
int gdx_death_race_records_encoded_size(void);
/* Writes the store in the wire format above. Returns the bytes written, or 0 when out is NULL or outCap is
 * smaller than the encoded size. */
int gdx_death_race_records_encode(unsigned char* out, unsigned int outCap);
/* Replaces the store with a file image. Returns 1 only when every check passes. Otherwise returns 0 and the
 * store is unchanged. */
int gdx_death_race_records_decode(const unsigned char* data, unsigned int length);

/* Explicit-path file operations. Save is atomic: the image goes to "<path>.tmp" and is then moved over
 * <path>, so a crash leaves the old file or none. Load of a missing file gives an empty store and returns 1.
 * A corrupt file, or one that cannot be read for another reason, leaves the store unchanged and returns 0.
 * Save returns 1 on success and 0 on failure. */
int gdx_death_race_records_save_to(const char* path);
int gdx_death_race_records_load_from(const char* path);

/* Directory for the default file. Until it is set, the default is the executable's directory on Windows
 * and the working directory elsewhere. */
void gdx_death_race_records_set_directory(const char* directory);
/* Name of the default file. NULL, an empty name, a name too long to hold, or one with a path separator
 * selects death_race_records.bin. */
void gdx_death_race_records_set_file_name(const char* name);
/* The default file is the name set above, in the directory set above. Save and load return 1 on success
 * and 0 on failure. */
int gdx_death_race_records_save(void);
int gdx_death_race_records_load(void);

#ifdef __cplusplus
}
#endif

#endif /* GDX_DEATH_RACE_H */
