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
#define GDX_BRANCH_EDIT_MAX_POINTS 64

/* Bytes of one saved edit state (undo snapshots). gdx_course_branch_edit.c checks it at compile time. */
#define GDX_BRANCH_EDIT_STATE_BYTES 4352

typedef struct GdxBranchEditPoint {
    float pos[3];
    short radiusLeft;
    short radiusRight;
    int trackSegmentInfo;
    short bankAngle;
    signed char parts[GDX_BRANCH_EDIT_PARTS];
} GdxBranchEditPoint;

/* Session. clear drops every fork and shows path A. select loads courseId's stored set when it was
 * saved for a ring of ringCount points (else clears). Both refresh the active set. */
void gdx_branch_edit_clear(void);
int gdx_branch_edit_select(const char* courseId, int ringCount);

/* Stores the edit set under courseId (a set with no forks removes the entry). The A view must be
 * showing. Returns 0, or a negative code with reason filled when the set is invalid; then the entry is
 * removed so the saved course races path A only. */
int gdx_branch_edit_commit(const char* courseId, int ringCount, unsigned int courseChecksum, char* reason, int cap);

int gdx_branch_edit_fork_count(void);
int gdx_branch_edit_view_b(void);

/* Fork whose segments F..M-1 include segment, or -1. */
int gdx_branch_edit_fork_at(int segment);

/* Swaps path A and path B of every fork in ring (ringCount points, room for cap). Returns the new
 * point count. A fork whose ring interior is empty cannot swap and is removed; *dropped counts them. */
int gdx_branch_edit_swap(GdxBranchEditPoint* ring, int ringCount, int cap, int* dropped);

/* Ring change hooks, called after the editor ring changed. */
void gdx_branch_edit_on_insert(int newIndex, const GdxBranchEditPoint* ring, int ringCount);
void gdx_branch_edit_on_delete(int deletedIndex);
void gdx_branch_edit_on_rotate(int newStart, int ringCount);
void gdx_branch_edit_on_reverse(int ringCount);

/* Called after every editor rebuild. Removes forks the ring no longer supports (a ring change the hooks
 * did not see, a fork that now touches point 0, too many points) and refreshes the active set.
 * Returns how many forks were removed. */
int gdx_branch_edit_sync(int ringCount);

/* Split on segment (segment, segment+1). check returns 0 when the caller may insert path A's point at
 * segment+1; then split_commit adds the fork with nodeB as path B. check returns 1 (no reason) when the
 * segment is inside a fork, which the Split tool uses to switch the view. */
int gdx_branch_edit_split_check(int segment, int ringCount, char* reason, int cap);
int gdx_branch_edit_split_commit(int segment, const GdxBranchEditPoint* nodeB);

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
