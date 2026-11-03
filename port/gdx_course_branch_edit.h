/* port/gdx_course_branch_edit.h -- Course Edit fork editing, in plain C types.
 *
 * The editor owns one fork set (the edit set) and mirrors it into gdx_branch_set_active() after every
 * change, so the race runtime (gdx_course_branch.c) builds the editor's second ring from it.
 *
 * Path B view: the editor edits path B with its stock tools by swapping it into the editor ring. In the
 * B view each fork's ring interior (F+1..M-1) holds path B and the fork's stored nodes hold path A.
 * Swapping again restores the A view. Fork and merge points keep their meaning in both views, so every
 * hook below works on whichever view is showing. Saving and test runs force the A view first.
 *
 * Indices are editor ring indices: control point indices, start segment at 0, no rotation.
 * Like gdx_course_branch_view.h, nothing here includes <stddef.h> or <stdint.h>, so the decomp side
 * (global.h) can include it.
 */

#ifndef GDX_COURSE_BRANCH_EDIT_H
#define GDX_COURSE_BRANCH_EDIT_H

#ifdef __cplusplus
extern "C" {
#endif

#define GDX_BRANCH_EDIT_PARTS 9 /* pit, dash, dirt, ice, jump, landmine, gate, building, sign */
/* "No part" for all nine (PIT_NONE, DASH_NONE and so on in fzx_course.h). 0 is a real part of each. */
#define GDX_BRANCH_EDIT_PART_NONE (-1)
#define GDX_BRANCH_EDIT_MAX_POINTS 127

/* Bytes of one saved edit state (undo snapshots). gdx_course_branch_edit.c checks it at compile time. */
#define GDX_BRANCH_EDIT_STATE_BYTES 12288

/* The values of one point (GdxPointExtras in gdx_course_settings.h), by index. */
#define GDX_BRANCH_EDIT_EXTRA_PIT_WIDTH 0
#define GDX_BRANCH_EDIT_EXTRA_DIRT_WIDTH 1
#define GDX_BRANCH_EDIT_EXTRA_ICE_WIDTH 2
#define GDX_BRANCH_EDIT_EXTRA_MAGNET_STRENGTH 3
#define GDX_BRANCH_EDIT_EXTRA_MAGNET_RANGE 4
#define GDX_BRANCH_EDIT_EXTRA_COUNT 5

/* The values of the whole track. */
#define GDX_BRANCH_EDIT_TRACK_LAVA 0
#define GDX_BRANCH_EDIT_TRACK_STRENGTH 1
#define GDX_BRANCH_EDIT_TRACK_RANGE 2
#define GDX_BRANCH_EDIT_TRACK_COUNT 3

typedef struct GdxBranchEditPoint {
    float pos[3];
    short radiusLeft;
    short radiusRight;
    int trackSegmentInfo;
    short bankAngle;
    signed char parts[GDX_BRANCH_EDIT_PARTS];
} GdxBranchEditPoint;

/* Removes every part from point. A new path B point starts this way. */
void gdx_branch_edit_point_clear_parts(GdxBranchEditPoint* point);

/* Session. clear drops every fork, every setting and shows path A. select loads courseId's stored set when it
 * was saved for a ring of ringCount points (else clears), and removes the parts of any stored node that
 * has all nine set to 0, which older builds gave every new node. It loads courseId's settings too: track values
 * always, point values only for that ring length. Both refresh the active set and settings. */
void gdx_branch_edit_clear(void);
int gdx_branch_edit_select(const char* courseId, int ringCount);

/* Gives every fork without one its own first segment style, taken from the fork point in ring. Call it when
 * a fork appears (split, load), so a later design on that segment reaches one path only. */
void gdx_branch_edit_first_fill(const GdxBranchEditPoint* ring, int ringCount);

/* Stores the edit set and settings under courseId (nothing to store removes the entry). The A view must be
 * showing. ring is the editor ring: a fork whose two paths start with the same style stores no value of its
 * own. NULL keeps every value. Returns 0, or a negative code with reason filled when the set is invalid; then
 * the fork entry is removed so the saved course races path A only. */
int gdx_branch_edit_commit(const char* courseId, const GdxBranchEditPoint* ring, int ringCount,
                           unsigned int courseChecksum, char* reason, int cap);

int gdx_branch_edit_fork_count(void);
int gdx_branch_edit_can_add(int ringCount, int points);
int gdx_branch_edit_view_b(void);

/* Values of the ring point the view shows at point, or of the whole track. which is one of the defines above.
 * Set clamps to the value's range. 0 means not set. Each refreshes the active settings. */
int gdx_branch_edit_point_extra(int point, int which);
void gdx_branch_edit_set_point_extra(int point, int which, int value);
int gdx_branch_edit_track_setting(int which);
void gdx_branch_edit_set_track_setting(int which, int value);

/* Fork whose segments F..M-1 include segment, or -1. */
int gdx_branch_edit_fork_at(int segment);

/* Swaps path A and path B of every fork in ring (ringCount points, room for cap). Returns the new
 * point count. A fork whose ring interior is empty cannot swap and is removed; *dropped counts them. */
int gdx_branch_edit_swap(GdxBranchEditPoint* ring, int ringCount, int cap, int* dropped);

/* Swaps as above so stored node `node` of fork f comes into the ring, and writes its ring index to *point.
 * Returns the new point count, or -1 with nothing changed when there is no such node, a swap would remove any
 * fork, or the swapped ring would not fit. */
int gdx_branch_edit_swap_to_node(GdxBranchEditPoint* ring, int ringCount, int cap, int f, int node, int* point);

/* Fork f's fork and merge points, and the positions of its stored nodes (the path not showing), up to cap of
 * them. Returns the stored node count, or -1 when f is not a fork. */
int gdx_branch_edit_fork_shape(int f, int* forkPoint, int* mergePoint, float (*pos)[3], int cap);

/* Ring change hooks, called after the editor ring changed. An insert inside a symmetric fork in the A view
 * pairs the new point with a B node; an insert in the B view adds to path B only and makes its fork
 * asymmetric. */
void gdx_branch_edit_on_insert(int newIndex, const GdxBranchEditPoint* ring, int ringCount);
void gdx_branch_edit_on_delete(int deletedIndex);
void gdx_branch_edit_on_rotate(int newStart, int ringCount);
void gdx_branch_edit_on_reverse(int ringCount);
/* The whole ring moved by (dx, dz), so the stored nodes move with it. Center course does this. */
void gdx_branch_edit_on_translate(float dx, float dz);

/* Called after every editor rebuild. Removes forks the ring no longer supports (a ring change the hooks
 * did not see, a fork that now touches point 0, too many points) and refreshes the active set.
 * Returns how many forks were removed. */
int gdx_branch_edit_sync(int ringCount);

/* Split on segment (segment, segment+1). check returns 0 when the caller may insert path A's point at
 * segment+1; then split_commit adds the fork with nodeB as path B. check returns 1 (no reason) when the
 * segment is inside a fork, which the Split tool uses to switch the view. */
int gdx_branch_edit_split_check(int segment, int ringCount, char* reason, int cap);
int gdx_branch_edit_split_commit(int segment, const GdxBranchEditPoint* nodeB);

/* Split over ring points forkPoint..mergePoint, whose interior (forkPoint+1..mergePoint-1) is path A as it
 * stands. span_check returns 0 when a fork with one B node per interior point fits, else a negative code
 * with reason filled. span_commit adds that fork, symmetric, with nodesB as path B. */
int gdx_branch_edit_span_check(int forkPoint, int mergePoint, int ringCount, char* reason, int cap);
int gdx_branch_edit_span_commit(int forkPoint, int mergePoint, const GdxBranchEditPoint* nodesB, int count);

/* Link: fork's path B now merges at ring point newMerge. Returns 0, or a negative code when the fork
 * would break (the editor shows "Travel path broken"). */
int gdx_branch_edit_link(int fork, int newMerge, int ringCount);

/* "<n>-A" or "<n>-B" for a point inside a fork's ring interior (by the view showing), else "<n>". */
void gdx_branch_edit_label(int ringPoint, char* out, int cap);

/* Undo. state_save and state_load copy the live edit state to and from GDX_BRANCH_EDIT_STATE_BYTES
 * bytes. backup copies the live state to the backup slot (with the decomp backup, func_xk2_800EF78C),
 * restore copies the backup slot back (func_xk2_800EF8B0). backup_save and backup_load move the backup
 * slot to and from bytes for the undo pool. */
void gdx_branch_edit_state_save(unsigned char* out);
void gdx_branch_edit_state_load(const unsigned char* in);
int gdx_branch_edit_state_equal(const unsigned char* in);
void gdx_branch_edit_backup(void);
void gdx_branch_edit_restore(void);
void gdx_branch_edit_backup_save(unsigned char* out);
void gdx_branch_edit_backup_load(const unsigned char* in);

#ifdef __cplusplus
}
#endif

#endif /* GDX_COURSE_BRANCH_EDIT_H */
