/* port/gdx_course_branch.h -- race runtime of Course Edit forks.
 *
 * The fork data (forkPoint F, mergePoint M, the B nodes) lives in gdx_course_branch_format.h. The stock ring
 * stays untouched: racers that keep to it race path A, and a course with no forks never reaches any code
 * below past its first check.
 *
 * Path B is raced on a second segment ring built beside the stock one ("ring 2"): the stock ring with every
 * fork's A interior (F+1..M-1) replaced by that fork's B nodes. Ring 2 gets the same setup the stock ring
 * gets (tensions, up vectors, lengths, joins, forms) and its own chunk ring, so the stock spline, physics and
 * draw code all work on it unchanged. A racer moves onto ring 2 in a fork's first segment (F) and back onto
 * the stock ring at the merge point. Only ring 2's B spans are drawn, after the stock ring.
 *
 * Lap distance inside a B span is scaled into the stock span F..M, so the lap length, the lap wrap and the
 * ranking see one course length. Ghosts are not recorded on a course with forks: a ghost replays on the
 * stock ring only.
 *
 * Game thread only. Include global.h before this header.
 */

#ifndef GDX_COURSE_BRANCH_H
#define GDX_COURSE_BRANCH_H

/* End of Course_Init: picks the fork set of the course (the store entry of its file, or the editor's live
 * set for the editor and its test run) and builds ring 2. */
void gdx_course_branch_on_course_init(void);

/* Rebuilds ring 2 from gdx_branch_active_set() for gCurrentCourseInfo, whose stock ring and chunk ring must
 * be built. Clears the runtime when the set is empty or does not fit the course. */
void gdx_course_branch_build(void);
/* The same for the Course Edit ring (gdx_course_branch_editor.c), after each editor rebuild. */
void gdx_course_branch_build_editor(void);
void gdx_course_branch_clear(void);

/* 1 when ring 2 is built for the course in gCurrentCourseInfo. */
s32 gdx_course_branch_live(void);

/* Racer_Init: every racer starts on the stock ring with no fork history. */
void gdx_course_branch_reset_racers(void);

/* racer.c, right after a successful func_8009EBEC walk and before func_8009DFA0: moves the racer between
 * the rings at a fork or a merge. */
void gdx_course_branch_racer_walked(s32 racerId, RacerSegmentPositionInfo* info);

/* func_8009DFA0: the distance into a segment, scaled when the segment is part of a B span. */
f32 gdx_course_branch_lap_partial(const CourseSegment* segment, f32 partial);

/* Course_Draw: draws the B spans of ring 2 after the stock ring by swapping ring 2's chunks in and calling
 * drawRing (the stock Course_Draw body) with gGdxCourseChunkDrawMask set. */
Gfx* gdx_course_branch_draw(Gfx* gfx, s32 cameraIndex, Gfx* (*drawRing)(Gfx*, s32));
/* Course Edit's surface draw (func_800A95B4): the same for the editor view. */
Gfx* gdx_course_branch_draw_editor(Gfx* gfx, Gfx* (*drawSurface)(Gfx*));

/* NULL except while ring 2 is drawn: then one byte per chunk, 0 for a chunk the cull pass must skip. */
extern const u8* gGdxCourseChunkDrawMask;

#endif /* GDX_COURSE_BRANCH_H */
