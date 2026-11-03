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

s32 gdx_course_branch_gadget_count(s32 courseIndex);
CourseSegment* gdx_course_branch_gadget_segment(s32 courseIndex, s32 index);
s32 gdx_course_branch_gadget_source(s32 index);
s32 gdx_course_branch_gadget_part(s32 index, s32 part, s32 fallback);
s32 gdx_course_branch_settings_point(const CourseSegment* segment);
f32 gdx_course_branch_gadget_next_t(const CourseSegment* segment, f32 t);
s32 gdx_course_branch_gadget_chunk(const CourseSegment* segment, f32 t);
s32 gdx_course_branch_gadget_chunk_visible(s32 encoded);
s32 gdx_course_branch_gadget_adjacent(const CourseSegment* previous, const CourseSegment* current);
SegmentChunk* gdx_course_branch_gadget_load_chunk(const CourseSegment* segment, const Vec3f* pos);
void gdx_course_branch_sync_gadgets(void);

/* End of Course_SegmentsInit, before the parts are built: picks the course's settings
 * (gdx_course_settings_view.h) and finds which control point race segment 0 came from. */
void gdx_course_branch_on_segments_init(const CourseSegment* ring, s32 count);

/* The control point a race segment came from, which is what the settings are keyed by, or -1 when the ring
 * does not match the course data. The editor's segments are its control points. */
s32 gdx_course_branch_control_point(s32 segmentIndex);

/* Rebuilds ring 2 from gdx_branch_active_set() for gCurrentCourseInfo, whose stock ring and chunk ring must
 * be built. Clears the runtime when the set is empty or does not fit the course. */
void gdx_course_branch_build(void);
/* The same for the Course Edit ring (gdx_course_branch_editor.c), after each editor rebuild. */
void gdx_course_branch_build_editor(void);
void gdx_course_branch_clear(void);

/* 1 when ring 2 is built for the course in gCurrentCourseInfo. */
s32 gdx_course_branch_live(void);
void gdx_course_branch_minimap(u8* texture, f32 scale, s32 fill);

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

Gfx* gdx_course_branch_chunk_mesh(const SegmentChunk* chunk, const SegmentChunk* next);

#endif /* GDX_COURSE_BRANCH_H */
