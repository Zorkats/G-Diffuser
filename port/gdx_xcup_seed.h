/* port/gdx_xcup_seed.h -- shareable X Cup seed codes (decision M3).
 *
 * A generated X Cup course's identity IS the four-integer RNG state captured at generation
 * time: gRandSeed1/gRandMask1 + gRandSeed2/gRandMask2 (POST_1_0_SCOPING M3, verified at
 * course.c:2659-2704; both masks evolve per call, so all four integers are load-bearing).
 * The code is those four u32 (16 bytes) in Crockford-style base32: 26 chars in 5 dash groups.
 *
 * Codes cover GENERATED tracks only. Course Edit tracks are hand-authored CourseContext data
 * and share via .gdxc instead (CONTENT_EXPORT.md).
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

/* Formats the last generated X Cup course's seed (sRandomCourseInit* in course.c) as a code.
 * Returns 0 when no random course has been generated yet this session. */
int gdx_xcup_current_seed_code(char* out, size_t outCap);

/* Parses a code (dashes/spaces optional, case-insensitive) and queues it for the NEXT X Cup
 * course generation. Returns 1 on success, 0 on malformed input (nothing queued). */
int gdx_xcup_set_pending_code(const char* text);

/* Nonzero while a queued seed is waiting for the next generation. */
int gdx_xcup_has_pending_seed(void);

/* Decomp hook (course.c, top of Course_GenerateRandomCourse): on a queued seed, fills the four
 * values, consumes the queue entry, and returns 1 exactly once. */
int gdx_xcup_consume_pending_seed(int32_t* seed1, int32_t* mask1, int32_t* seed2, int32_t* mask2);

/* course.c calls this after capturing the generation seed, so "current code" stays empty until
 * the first random course exists (the BSS-zero state is indistinguishable from a real seed). */
void gdx_xcup_note_seed_captured(void);

#ifdef __cplusplus
}
#endif

#endif /* GDX_XCUP_SEED_H */
