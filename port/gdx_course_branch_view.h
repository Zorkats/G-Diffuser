/* port/gdx_course_branch_view.h -- the active fork set (gdx_course_branch_format.h) in plain C types.
 *
 * For code that includes the decomp's global.h, whose libc replacement typedefs clash with the system
 * <stddef.h> and <stdint.h> that gdx_course_branch_format.h needs. Nothing here includes either.
 * Every call reads gdx_branch_active_set(); an index out of range returns 0.
 */

#ifndef GDX_COURSE_BRANCH_VIEW_H
#define GDX_COURSE_BRANCH_VIEW_H

#ifdef __cplusplus
extern "C" {
#endif

/* Same value as GDX_BRANCH_MAX_FORKS; gdx_course_branch_view.c checks that at compile time. */
#define GDX_BRANCH_VIEW_MAX_FORKS 4

/* Makes the store entry of courseId the active set, or clears the active set when courseId is NULL or
 * has no entry. Returns 1 when the active set now has forks. */
int gdx_branch_view_select_course(const char* courseId);

/* 0 when there is no active set. */
int gdx_branch_view_fork_count(void);
int gdx_branch_view_ring_count(void);

/* gdx_branch_validate on the active set. Returns 0 when it is valid for ringCount. */
int gdx_branch_view_validate(int ringCount, char* reason, int cap);

/* Returns 1 and fills every output when fork exists. */
int gdx_branch_view_fork(int fork, int* forkPoint, int* mergePoint, int* nodeCount);
int gdx_branch_view_node(int fork, int node, float pos[3], short* radiusLeft, short* radiusRight,
                         int* trackSegmentInfo, short* bankAngle);

#ifdef __cplusplus
}
#endif

#endif /* GDX_COURSE_BRANCH_VIEW_H */
