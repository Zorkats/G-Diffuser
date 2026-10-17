/* port/gdx_xcup_seed.h -- shareable X Cup seed codes (decision M3) and the queue of codes to use.
 *
 * A generated X Cup course's identity IS the four-integer RNG state captured at generation
 * time: gRandSeed1/gRandMask1 + gRandSeed2/gRandMask2 (POST_1_0_SCOPING M3, see
 * Course_GenerateRandomCourse in course.c; both masks evolve per call, so all four integers are
 * load-bearing). The code is those four u32 (16 bytes) in Crockford-style base32: 26 digits, a
 * dash after every fifth digit, 31 characters in all.
 *
 * Codes cover GENERATED tracks only. Course Edit tracks are hand-authored CourseContext data
 * and share via .gdxc instead (CONTENT_EXPORT.md).
 *
 * Queue: codes waiting to be used, oldest first. Each newly generated X Cup course takes the
 * front entry (course.c, PORT block in Course_GenerateRandomCourse). A re-do of a course takes
 * none, so codes pasted one per line are used by the courses of a cup in order.
 *
 * Cup record: the seeds used for the current cup, one slot per X Cup course (slot 0 is
 * COURSE_X_1). "Copy this cup's codes" exports it. A new cup clears it.
 */

#ifndef GDX_XCUP_SEED_H
#define GDX_XCUP_SEED_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* "XXXXX-XXXXX-XXXXX-XXXXX-XXXXX-X" + NUL */
#define GDX_XCUP_CODE_LENGTH 31
#define GDX_XCUP_CODE_BUF_SIZE 32

/* Codes the queue holds at most. */
#define GDX_XCUP_QUEUE_CAPACITY 64

/* X Cup courses per cup (COURSE_X_1 .. COURSE_X_6). */
#define GDX_XCUP_CUP_SIZE 6

/* Formats the last generated X Cup course's seed (sRandomCourseInit* in course.c) as a code.
 * Returns 0 when no random course has been generated yet this session. */
int gdx_xcup_current_seed_code(char* out, size_t outCap);

/* Parses one code (dashes/spaces optional, case-insensitive) and appends it to the queue.
 * Returns 1 on success, 0 on malformed input or a full queue (nothing queued). */
int gdx_xcup_set_pending_code(const char* text);

/* Queues every code in text, in order. Codes are separated by newlines, spaces, tabs, commas
 * or semicolons; dashes stay inside a code. added and rejected receive the counts (either may
 * be NULL). rejected counts pieces that are not codes and codes that did not fit the queue. */
void gdx_xcup_queue_add_text(const char* text, int* added, int* rejected);

/* Number of codes waiting. */
int gdx_xcup_queue_count(void);

/* Formats queue entry index (0 = the next one used) into out. Returns 1 on success, 0 when the
 * index is out of range or outCap is smaller than GDX_XCUP_CODE_BUF_SIZE. */
int gdx_xcup_queue_code(int index, char* out, size_t outCap);

/* Removes queue entry index; later entries move up one place. Other indices are ignored. */
void gdx_xcup_queue_remove(int index);

/* Empties the queue. The cup record is left alone. */
void gdx_xcup_queue_clear(void);

/* Nonzero while a queued seed is waiting for the next generation. */
int gdx_xcup_has_pending_seed(void);

/* Decomp hook (course.c PORT block, Course_GenerateRandomCourse): when the queue is not empty,
 * fills the four values from the front entry, removes it, and returns 1. Returns 0 otherwise. */
int gdx_xcup_consume_pending_seed(int32_t* seed1, int32_t* mask1, int32_t* seed2, int32_t* mask2);

/* course.c calls this after capturing the seed of a generated course. slot is the course's
 * position in its cup (course index minus COURSE_X_1); values outside 0..5 only mark the
 * capture. newCup is nonzero when the course starts a cup: the cup record is cleared first.
 * The record reads the sRandomCourseInit* values. */
void gdx_xcup_note_seed_captured(int slot, int newCup);

/* Writes the codes of the current cup, one per line in course order, NUL-terminated, and
 * returns how many codes were written. Writes whole codes only, so a small buffer gets fewer. */
int gdx_xcup_cup_codes(char* out, size_t outCap);

#ifdef __cplusplus
}
#endif

#endif /* GDX_XCUP_SEED_H */
