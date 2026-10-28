/* port/gdx_course_branch_editor.c -- Course Edit glue of the fork editor (gdx_course_branch_edit.c).
 *
 * The editor ring (D_802CB6D0 plus the per-point arrays of the course data) goes out to the edit library
 * as GdxBranchEditPoint and comes back in after a view swap. The decomp calls the hooks below from the
 * places that change the ring, save, load, test run and undo. The Split and Link tools live here too.
 */

#include "global.h"
#include "fzx_expansion_kit.h"
#include "fzx_course.h"
#include "gdx_course_branch.h"
#include "gdx_course_branch_edit.h"
#include "gdx_course_branch_editor.h"
#include "gdx_course_edit_undo.h"

extern unk_807B3C20 D_802CB6D0;
extern unk_80128690 D_80128690[64];
extern unk_800D6CA0 D_800D6CA0;
extern CourseInfo* gCurrentCourseInfo;
extern bool gInCourseEditTestRun;
extern s32 D_xk2_800F7040;
extern s32 D_xk2_800F704C;
extern s32 gCourseEditCursorYPos;
extern s32 gCreateOption;
extern s32 gDesignStyleOption;
extern void func_xk2_800E6F9C(void);
extern void func_xk2_800E6CA8(s32 arg0, CourseSegment arg1);
extern void func_xk2_800EF78C(void);
extern s32 func_i2_800BE8BC(CourseInfo* courseInfo);
extern void gdx_course_edit_flash_message(const char* text);
extern int gdx_course_state_edit_file_id(char* out, int outCap);
extern int gdx_course_state_flush(void);
extern void gdx_course_branch_build_editor(void);

#define EDITOR_LABEL_CAP 96

static GdxBranchEditPoint sPoints[GDX_BRANCH_EDIT_MAX_POINTS];
static s32 sLinkFork = -1;
/* The segment a first Split press picked, or -1. */
static s32 sSplitStart = -1;

static void editor_export(void) {
    CourseData* courseData = &COURSE_CONTEXT()->courseData;
    s32 i;

    for (i = 0; i < D_802CB6D0.controlPointCount; i++) {
        CourseSegment* segment = &D_802CB6D0.unk_0000[i];
        GdxBranchEditPoint* point = &sPoints[i];

        point->pos[0] = segment->pos.x;
        point->pos[1] = segment->pos.y;
        point->pos[2] = segment->pos.z;
        point->radiusLeft = segment->radiusLeft;
        point->radiusRight = segment->radiusRight;
        point->trackSegmentInfo = segment->trackSegmentInfo;
        point->bankAngle = courseData->bankAngle[i];
        point->parts[0] = courseData->pit[i];
        point->parts[1] = courseData->dash[i];
        point->parts[2] = courseData->dirt[i];
        point->parts[3] = courseData->ice[i];
        point->parts[4] = courseData->jump[i];
        point->parts[5] = courseData->landmine[i];
        point->parts[6] = courseData->gate[i];
        point->parts[7] = courseData->building[i];
        point->parts[8] = courseData->sign[i];
    }
}

static void editor_import(s32 count) {
    static const unk_80128690 sFreshSelection;
    CourseData* courseData = &COURSE_CONTEXT()->courseData;
    s32 i;

    for (i = 0; i < count; i++) {
        CourseSegment* segment = &D_802CB6D0.unk_0000[i];
        const GdxBranchEditPoint* point = &sPoints[i];

        segment->pos.x = point->pos[0];
        segment->pos.y = point->pos[1];
        segment->pos.z = point->pos[2];
        segment->up.x = 0.0f;
        segment->up.y = 1.0f;
        segment->up.z = 0.0f;
        segment->radiusLeft = point->radiusLeft;
        segment->radiusRight = point->radiusRight;
        segment->trackSegmentInfo = point->trackSegmentInfo;
        courseData->bankAngle[i] = point->bankAngle;
        courseData->pit[i] = point->parts[0];
        courseData->dash[i] = point->parts[1];
        courseData->dirt[i] = point->parts[2];
        courseData->ice[i] = point->parts[3];
        courseData->jump[i] = point->parts[4];
        courseData->landmine[i] = point->parts[5];
        courseData->gate[i] = point->parts[6];
        courseData->building[i] = point->parts[7];
        courseData->sign[i] = point->parts[8];
        D_80128690[i] = sFreshSelection;
    }
    D_802CB6D0.controlPointCount = count;
    func_xk2_800E6F9C();
    if (D_800D6CA0.unk_0C >= count) {
        D_800D6CA0.unk_0C = count - 1;
    }
    D_xk2_800F704C = -1;
    func_i2_800BE8BC(gCurrentCourseInfo);
    func_80074CE4(gCurrentCourseInfo);
    D_xk2_800F7040 = 3;
}

/* Swaps path A and path B into the editor ring. Returns 1 when the view changed. */
static s32 editor_swap(void) {
    s32 count = D_802CB6D0.controlPointCount;
    s32 newCount;
    int dropped;

    if ((gdx_branch_edit_fork_count() == 0) || (count < 4)) {
        return 0;
    }
    editor_export();
    newCount = gdx_branch_edit_swap(sPoints, count, GDX_BRANCH_EDIT_MAX_POINTS, &dropped);
    if (dropped != 0) {
        gdx_course_edit_flash_message("Fork removed: one of its paths has no points");
    }
    if ((newCount == count) && (dropped == 0) && (gdx_branch_edit_fork_count() == 0)) {
        return 0;
    }
    editor_import(newCount);
    return 1;
}

void gdx_course_branch_editor_on_insert(int newIndex) {
    editor_export();
    gdx_branch_edit_on_insert(newIndex, sPoints, D_802CB6D0.controlPointCount);
}

void gdx_course_branch_editor_on_delete(int index) {
    gdx_branch_edit_on_delete(index);
}

void gdx_course_branch_editor_on_rotate(int newStart) {
    gdx_branch_edit_on_rotate(newStart, D_802CB6D0.controlPointCount);
}

void gdx_course_branch_editor_on_reverse(void) {
    gdx_branch_edit_on_reverse(D_802CB6D0.controlPointCount);
}

/* After func_xk2_800E5B6C rebuilt a valid editor ring: drop forks the ring no longer supports, then build
 * the second ring so the other path shows in the editor. */
void gdx_course_branch_editor_rebuilt(void) {
    if (gInCourseEditTestRun) {
        return;
    }
    /* The ring changed, so a fork or segment picked by a first Link or Split press may now be another. */
    sLinkFork = -1;
    sSplitStart = -1;
    if (gdx_branch_edit_sync(D_802CB6D0.controlPointCount) != 0) {
        gdx_course_edit_flash_message("Fork removed: the course no longer fits it");
    }
    gdx_course_branch_build_editor();
}

/* Save, export and test runs need path A in the ring. */
void gdx_course_branch_editor_force_path_a(void) {
    if (gdx_branch_edit_view_b() && editor_swap()) {
        gdx_course_edit_flash_message("Showing path A");
    }
}

/* The editor saved its course under the current edit file. */
void gdx_course_branch_editor_saved(void) {
    char courseId[32];
    char reason[EDITOR_LABEL_CAP];
    char text[EDITOR_LABEL_CAP + 32];

    if (!gdx_course_state_edit_file_id(courseId, sizeof(courseId))) {
        return;
    }
    if (gdx_branch_edit_commit(courseId, D_802CB6D0.controlPointCount, COURSE_CONTEXT()->courseData.checksum, reason,
                               sizeof(reason)) != 0) {
        sprintf(text, "Forks not saved: %s", reason);
        gdx_course_edit_flash_message(text);
    }
    gdx_course_state_flush();
}

/* The editor ring now holds a loaded course (func_xk2_800EACB0). */
void gdx_course_branch_editor_loaded(void) {
    char courseId[32];

    sLinkFork = -1;
    sSplitStart = -1;
    if (gdx_course_state_edit_file_id(courseId, sizeof(courseId))) {
        gdx_branch_edit_select(courseId, D_802CB6D0.controlPointCount);
    } else {
        gdx_branch_edit_clear();
    }
}

/* The editor started a new course: it has no forks. */
void gdx_course_branch_editor_new_course(void) {
    sLinkFork = -1;
    sSplitStart = -1;
    gdx_branch_edit_clear();
}

static s32 editor_tool_pressed(s32 style) {
    return (gCourseEditCursorYPos >= 0x38) && (D_800D6CA0.unk_08 != 1) && (D_800D6CA0.unk_08 != 3) &&
           (D_800D6CA0.unk_08 != 2) && (D_800D6CA0.unk_08 != 0x10) && (gCreateOption == CREATE_OPTION_DESIGN) &&
           (gDesignStyleOption == style) && (gControllers[gPlayerControlPorts[0]].buttonPressed & BTN_A) &&
           (D_800D6CA0.unk_0C >= 0) && (D_800D6CA0.unk_0C < D_802CB6D0.controlPointCount);
}

static void editor_refuse(const char* text) {
    gdx_course_edit_flash_message(text);
    Audio_TriggerSystemSE(NA_SE_32);
}

/* Least distance between neighboring points, the stock editor's spacing rule (func_xk2_800F07A4). */
#define SPLIT_MIN_SPACING 498.0f

static f32 editor_distance(const f32* a, const f32* b) {
    return sqrtf(SQ(a[0] - b[0]) + SQ(a[1] - b[1]) + SQ(a[2] - b[2]));
}

static void editor_ring_pos(s32 index, f32* out) {
    out[0] = D_802CB6D0.unk_0000[index].pos.x;
    out[1] = D_802CB6D0.unk_0000[index].pos.y;
    out[2] = D_802CB6D0.unk_0000[index].pos.z;
}

static f32 editor_ring_width(s32 index) {
    return D_802CB6D0.unk_0000[index].radiusLeft + D_802CB6D0.unk_0000[index].radiusRight;
}

/* Path B for a split over forkPoint..mergePoint: each path A interior point moved sideways (side 1 left of
 * travel, -1 right) by a road width and a quarter, so the two roads run side by side. Writes the nodes to
 * nodesB and returns the smallest margin left by the spacing rule and by the roads of the rest of the course,
 * other forks included. A negative margin means the new path crowds a point or touches a road. */
static f32 editor_place_path_b(s32 forkPoint, s32 mergePoint, f32 side, GdxBranchEditPoint* nodesB) {
    static f32 sOther[GDX_BRANCH_EDIT_MAX_POINTS][3];
    CourseData* courseData = &COURSE_CONTEXT()->courseData;
    s32 count = D_802CB6D0.controlPointCount;
    s32 interior = mergePoint - forkPoint - 1;
    f32 margin = 1.0e9f;
    f32 prev[3];
    f32 here[3];
    f32 ringPos[3];
    f32 sideX;
    f32 sideZ;
    f32 length;
    f32 width;
    s32 otherFork;
    s32 otherMerge;
    s32 nodes;
    s32 f;
    s32 i;
    s32 j;
    s32 k;

    for (i = 0; i < interior; i++) {
        s32 point = forkPoint + 1 + i;
        GdxBranchEditPoint* node = &nodesB[i];

        editor_ring_pos(point - 1, prev);
        editor_ring_pos(point + 1, here);
        sideX = -(here[2] - prev[2]);
        sideZ = here[0] - prev[0];
        length = sqrtf(SQ(sideX) + SQ(sideZ));
        if (length < 1.0f) {
            return -1.0f;
        }
        width = editor_ring_width(point);
        editor_ring_pos(point, ringPos);
        node->pos[0] = Math_Round(ringPos[0] + side * sideX / length * width * 1.25f);
        node->pos[1] = ringPos[1];
        node->pos[2] = Math_Round(ringPos[2] + side * sideZ / length * width * 1.25f);
        node->radiusLeft = D_802CB6D0.unk_0000[point].radiusLeft;
        node->radiusRight = D_802CB6D0.unk_0000[point].radiusRight;
        node->trackSegmentInfo = D_802CB6D0.unk_0000[point].trackSegmentInfo;
        node->bankAngle = courseData->bankAngle[point];
        for (k = 0; k < GDX_BRANCH_EDIT_PARTS; k++) {
            node->parts[k] = 0;
        }
    }

    for (i = 0; i <= interior; i++) {
        if (i == 0) {
            editor_ring_pos(forkPoint, prev);
        } else {
            prev[0] = nodesB[i - 1].pos[0];
            prev[1] = nodesB[i - 1].pos[1];
            prev[2] = nodesB[i - 1].pos[2];
        }
        if (i == interior) {
            editor_ring_pos(mergePoint, here);
        } else {
            here[0] = nodesB[i].pos[0];
            here[1] = nodesB[i].pos[1];
            here[2] = nodesB[i].pos[2];
        }
        margin = MIN(margin, editor_distance(prev, here) - SPLIT_MIN_SPACING);
    }

    for (i = 0; i < interior; i++) {
        width = nodesB[i].radiusLeft + nodesB[i].radiusRight;
        for (j = 0; j < count; j++) {
            if ((j == forkPoint) || (j == mergePoint)) {
                continue;
            }
            editor_ring_pos(j, ringPos);
            margin = MIN(margin, editor_distance(nodesB[i].pos, ringPos) - (width + editor_ring_width(j)) * 0.5f);
        }
        for (f = 0; f < gdx_branch_edit_fork_count(); f++) {
            nodes = gdx_branch_edit_fork_shape(f, &otherFork, &otherMerge, sOther, GDX_BRANCH_EDIT_MAX_POINTS);
            for (k = 0; k < nodes; k++) {
                margin = MIN(margin, editor_distance(nodesB[i].pos, sOther[k]) - width);
            }
        }
    }
    return margin;
}

/* SPLIT design, one segment: forks segment (s, s+1) into two paths, one point each, offset to either side of
 * the curve midpoint. */
static void editor_split_segment(s32 segment) {
    CourseData* courseData = &COURSE_CONTEXT()->courseData;
    CourseSegment newSeg;
    CourseSegment* activeSeg;
    CourseSegment* nextSeg;
    GdxBranchEditPoint nodeB;
    char reason[EDITOR_LABEL_CAP];
    Vec3f mid;
    Vec3f pathA;
    Vec3f pathB;
    f32 sideX;
    f32 sideZ;
    f32 length;
    f32 offset;
    s32 count = D_802CB6D0.controlPointCount;
    s32 bankAverage;
    s32 rc;
    s32 i;

    rc = gdx_branch_edit_split_check(segment, count, reason, sizeof(reason));
    if (rc != 0) {
        editor_refuse((rc == 1) ? "Split: this segment is already a fork" : reason);
        return;
    }
    if (count < 4) {
        editor_refuse("Split: the course needs 4 or more points");
        return;
    }
    if (count >= 60) {
        editor_refuse("Split: the course already has 60 points");
        return;
    }
    if (gCurrentCourseInfo->length > 300000.0f) {
        editor_refuse("Split: the course is at the length limit");
        return;
    }

    activeSeg = &D_802CB6D0.unk_0000[segment];
    nextSeg = activeSeg->next;
    Course_SplineGetPosition(activeSeg, 0.5f, &mid);
    sideX = -(nextSeg->pos.z - activeSeg->pos.z);
    sideZ = nextSeg->pos.x - activeSeg->pos.x;
    length = sqrtf(SQ(sideX) + SQ(sideZ));
    if (length < 1.0f) {
        editor_refuse("Split: this segment is too short");
        return;
    }
    /* Each path sits most of a road width off the middle, so the two roads do not overlap. */
    offset = (activeSeg->radiusLeft + activeSeg->radiusRight + nextSeg->radiusLeft + nextSeg->radiusRight) * 0.4f;
    pathA.x = Math_Round(mid.x + sideX / length * offset);
    pathA.y = Math_Round(mid.y);
    pathA.z = Math_Round(mid.z + sideZ / length * offset);
    pathB.x = Math_Round(mid.x - sideX / length * offset);
    pathB.y = pathA.y;
    pathB.z = Math_Round(mid.z - sideZ / length * offset);
    if ((Math_VectorGetDistance(activeSeg->pos, pathA) < SPLIT_MIN_SPACING) ||
        (Math_VectorGetDistance(pathA, nextSeg->pos) < SPLIT_MIN_SPACING) ||
        (Math_VectorGetDistance(activeSeg->pos, pathB) < SPLIT_MIN_SPACING) ||
        (Math_VectorGetDistance(pathB, nextSeg->pos) < SPLIT_MIN_SPACING)) {
        editor_refuse("Split: this segment is too short");
        return;
    }

    bankAverage = (courseData->bankAngle[segment] + courseData->bankAngle[nextSeg->segmentIndex]) / 2;

    gdx_course_edit_undo_set_pending_label(GDX_COURSE_EDIT_UNDO_LABEL_SPLIT);
    func_xk2_800EF78C();
    Audio_TriggerSystemSE(NA_SE_39);

    newSeg = *activeSeg;
    newSeg.pos = pathA;
    newSeg.radiusLeft = (activeSeg->radiusLeft + nextSeg->radiusLeft) / 2;
    newSeg.radiusRight = (activeSeg->radiusRight + nextSeg->radiusRight) / 2;
    newSeg.trackSegmentInfo = activeSeg->trackSegmentInfo;
    func_xk2_800E6CA8(segment, newSeg);
    courseData->bankAngle[segment + 1] = (s16) bankAverage;

    nodeB.pos[0] = pathB.x;
    nodeB.pos[1] = pathB.y;
    nodeB.pos[2] = pathB.z;
    nodeB.radiusLeft = newSeg.radiusLeft;
    nodeB.radiusRight = newSeg.radiusRight;
    nodeB.trackSegmentInfo = D_802CB6D0.unk_0000[segment + 1].trackSegmentInfo;
    nodeB.bankAngle = (short) bankAverage;
    for (i = 0; i < GDX_BRANCH_EDIT_PARTS; i++) {
        nodeB.parts[i] = 0;
    }
    gdx_branch_edit_split_commit(segment, &nodeB);

    func_i2_800BE8BC(gCurrentCourseInfo);
    func_80074CE4(gCurrentCourseInfo);
    D_xk2_800F7040 = 3;
}

/* SPLIT design, several segments: path A keeps its points from forkPoint to mergePoint and path B is built
 * beside it, on whichever side leaves more room. */
static void editor_split_span(s32 forkPoint, s32 mergePoint) {
    static GdxBranchEditPoint sLeft[GDX_BRANCH_EDIT_MAX_POINTS];
    static GdxBranchEditPoint sRight[GDX_BRANCH_EDIT_MAX_POINTS];
    char reason[EDITOR_LABEL_CAP];
    f32 leftMargin;
    f32 rightMargin;
    s32 count = D_802CB6D0.controlPointCount;

    if (count < 4) {
        editor_refuse("Split: the course needs 4 or more points");
        return;
    }
    if (gdx_branch_edit_span_check(forkPoint, mergePoint, count, reason, sizeof(reason)) != 0) {
        editor_refuse(reason);
        return;
    }
    leftMargin = editor_place_path_b(forkPoint, mergePoint, 1.0f, sLeft);
    rightMargin = editor_place_path_b(forkPoint, mergePoint, -1.0f, sRight);
    if ((leftMargin < 0.0f) && (rightMargin < 0.0f)) {
        editor_refuse("Split: no room for path B beside path A");
        return;
    }

    gdx_course_edit_undo_set_pending_label(GDX_COURSE_EDIT_UNDO_LABEL_SPLIT);
    func_xk2_800EF78C();
    gdx_branch_edit_span_commit(forkPoint, mergePoint, (leftMargin >= rightMargin) ? sLeft : sRight,
                                mergePoint - forkPoint - 1);
    Audio_TriggerSystemSE(NA_SE_39);
    gdx_course_edit_flash_message("Split: path B added");
    D_xk2_800F7040 = 3;
}

/* SPLIT design: the first press picks the fork's first segment, the second press its last, which may be the
 * same segment. A first press on a segment of a fork switches the editor between path A and path B. */
static void editor_split(void) {
    char reason[EDITOR_LABEL_CAP];
    s32 segment = D_800D6CA0.unk_0C;
    s32 first;
    s32 last;
    s32 rc;

    if (sSplitStart < 0) {
        rc = gdx_branch_edit_split_check(segment, D_802CB6D0.controlPointCount, reason, sizeof(reason));
        if (rc == 1) {
            if (editor_swap()) {
                Audio_TriggerSystemSE(NA_SE_39);
                gdx_course_edit_flash_message(gdx_branch_edit_view_b() ? "Showing path B" : "Showing path A");
            }
            return;
        }
        if (rc != 0) {
            editor_refuse(reason);
            return;
        }
        sSplitStart = segment;
        Audio_TriggerSystemSE(NA_SE_39);
        gdx_course_edit_flash_message("Split: choose the last segment of the fork");
        return;
    }

    first = MIN(sSplitStart, segment);
    last = MAX(sSplitStart, segment);
    sSplitStart = -1;
    if (first == last) {
        editor_split_segment(first);
    } else {
        editor_split_span(first, last + 1);
    }
}

/* LINK design: the first press picks a fork (its path B end), the second press picks the ring point
 * where path B now rejoins. */
static void editor_link(void) {
    static unsigned char sBefore[GDX_BRANCH_EDIT_STATE_BYTES];
    s32 point = D_800D6CA0.unk_0C;
    s32 count = D_802CB6D0.controlPointCount;

    if (sLinkFork < 0) {
        if (gdx_branch_edit_view_b()) {
            editor_refuse("Link: show path A first (Split on a fork)");
            return;
        }
        sLinkFork = gdx_branch_edit_fork_at(point);
        if (sLinkFork < 0) {
            editor_refuse("Choose first node: pick a segment of a fork");
            return;
        }
        Audio_TriggerSystemSE(NA_SE_39);
        gdx_course_edit_flash_message("Link: choose where path B rejoins");
        return;
    }

    /* Try it first, so a refused link leaves no undo entry. */
    gdx_branch_edit_state_save(sBefore);
    if (gdx_branch_edit_link(sLinkFork, point, count) != 0) {
        sLinkFork = -1;
        editor_refuse("Travel path broken");
        return;
    }
    gdx_branch_edit_state_load(sBefore);
    gdx_course_edit_undo_set_pending_label(GDX_COURSE_EDIT_UNDO_LABEL_LINK);
    func_xk2_800EF78C();
    gdx_branch_edit_link(sLinkFork, point, count);
    sLinkFork = -1;
    Audio_TriggerSystemSE(NA_SE_39);
    gdx_course_edit_flash_message("Linked");
    D_xk2_800F7040 = 3;
}

/* Called with the Insert tool from func_xk2_800D8F04 once the segment under the cursor is picked. */
void gdx_course_branch_editor_tools(void) {
    if (!((gCreateOption == CREATE_OPTION_DESIGN) && (gDesignStyleOption == TRACK_DESIGN_STYLE_LINK))) {
        sLinkFork = -1;
    }
    if (!((gCreateOption == CREATE_OPTION_DESIGN) && (gDesignStyleOption == TRACK_DESIGN_STYLE_SPLIT))) {
        sSplitStart = -1;
    }
    if (editor_tool_pressed(TRACK_DESIGN_STYLE_SPLIT)) {
        editor_split();
    } else if (editor_tool_pressed(TRACK_DESIGN_STYLE_LINK)) {
        editor_link();
    }
}

/* Label of the active point for the hint strip: "<n>", "<n>-A" or "<n>-B". */
void gdx_course_branch_editor_active_label(char* out, int cap) {
    if ((D_800D6CA0.unk_0C < 0) || (gdx_branch_edit_fork_count() == 0)) {
        if (cap > 0) {
            out[0] = '\0';
        }
        return;
    }
    gdx_branch_edit_label(D_800D6CA0.unk_0C, out, cap);
}

static s32 editor_mark(GdxBranchMark* out, s32 written, s32 cap, const f32* pos, s32 kind, s32 chain) {
    if (written >= cap) {
        return written;
    }
    out[written].pos[0] = pos[0];
    out[written].pos[1] = pos[1];
    out[written].pos[2] = pos[2];
    out[written].kind = kind;
    out[written].chain = chain;
    return written + 1;
}

int gdx_course_branch_editor_marks(GdxBranchMark* out, int cap) {
    static f32 sNodes[GDX_BRANCH_EDIT_MAX_POINTS][3];
    s32 count = D_802CB6D0.controlPointCount;
    s32 written = 0;
    s32 forkPoint;
    s32 mergePoint;
    s32 nodes;
    s32 f;
    s32 i;
    f32 pos[3];

    if ((out == NULL) || gInCourseEditTestRun || (count < 4)) {
        return 0;
    }
    for (f = 0; f < gdx_branch_edit_fork_count(); f++) {
        nodes = gdx_branch_edit_fork_shape(f, &forkPoint, &mergePoint, sNodes, GDX_BRANCH_EDIT_MAX_POINTS);
        if ((nodes < 0) || (mergePoint >= count)) {
            continue;
        }
        editor_ring_pos(forkPoint, pos);
        written = editor_mark(out, written, cap, pos, GDX_BRANCH_MARK_END, f);
        for (i = 0; i < nodes; i++) {
            written = editor_mark(out, written, cap, sNodes[i], GDX_BRANCH_MARK_NODE, f);
        }
        editor_ring_pos(mergePoint, pos);
        written = editor_mark(out, written, cap, pos, GDX_BRANCH_MARK_END, f);
    }
    if ((sSplitStart >= 0) && (sSplitStart < count)) {
        editor_ring_pos(sSplitStart, pos);
        written = editor_mark(out, written, cap, pos, GDX_BRANCH_MARK_PICK, GDX_BRANCH_EDIT_MAX_POINTS);
        editor_ring_pos((sSplitStart + 1) % count, pos);
        written = editor_mark(out, written, cap, pos, GDX_BRANCH_MARK_PICK, GDX_BRANCH_EDIT_MAX_POINTS);
    }
    return written;
}

int gdx_course_branch_editor_path_showing(void) {
    if (gdx_branch_edit_fork_count() == 0) {
        return 0;
    }
    return gdx_branch_edit_view_b() ? 2 : 1;
}
