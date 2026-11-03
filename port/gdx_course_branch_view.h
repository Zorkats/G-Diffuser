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

/* Returns 1 and fills trackSegmentInfo when the fork's stored path has a first segment style of its own. */
int gdx_branch_view_fork_first_info(int fork, int* trackSegmentInfo);

#define GDX_BRANCH_SETTINGS_BASE 256
#define GDX_BRANCH_SETTINGS_STRIDE 33
int gdx_branch_view_part(int fork, int node, int part, int fallback);
int gdx_branch_view_settings_point(int fork, int node);

/* A racer's place on one road: distance from the center line (positive to the left), distance above the road, and
 * the road's radii there. */
typedef struct GdxBranchLaneSample {
    float lateral;
    float height;
    float radiusLeft;
    float radiusRight;
} GdxBranchLaneSample;

/* 1 when a racer who left the current road's edge is on the other road, so it crosses instead of hitting a wall.
 * A racer still on its road stays on it, however far the other road overlaps. */
int gdx_branch_view_lane_crossing(const GdxBranchLaneSample* current, const GdxBranchLaneSample* other);

/* 1 when point lies on a road chunk, given as the chunk's center, its left and right edges and a point straight
 * below its center. The points are the s16 reference positions of a SegmentChunk. An edge level with the chunk's
 * own edge does not count, which keeps a rail where two roads run together and only one edge is free. */
int gdx_branch_view_chunk_covers_point(const short point[3], const short center[3], const short left[3],
                                       const short right[3], const short below[3]);

#ifdef __cplusplus
}
#endif

#endif /* GDX_COURSE_BRANCH_VIEW_H */
