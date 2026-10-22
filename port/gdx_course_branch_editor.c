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
    /* The ring changed, so a fork picked by a first Link press may now be another fork. */
    sLinkFork = -1;
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
    if (gdx_course_state_edit_file_id(courseId, sizeof(courseId))) {
        gdx_branch_edit_select(courseId, D_802CB6D0.controlPointCount);
    } else {
        gdx_branch_edit_clear();
    }
}

/* The editor started a new course: it has no forks. */
void gdx_course_branch_editor_new_course(void) {
    sLinkFork = -1;
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

/* SPLIT design: forks segment (s, s+1) into two paths, one point each, offset to either side of the
 * curve midpoint. On a segment of a fork it switches the editor between path A and path B. */
static void editor_split(void) {
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
    s32 segment = D_800D6CA0.unk_0C;
    s32 count = D_802CB6D0.controlPointCount;
    s32 bankAverage;
    s32 rc;
    s32 i;

    rc = gdx_branch_edit_split_check(segment, count, reason, sizeof(reason));
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
    if ((Math_VectorGetDistance(activeSeg->pos, pathA) < 498.0f) ||
        (Math_VectorGetDistance(pathA, nextSeg->pos) < 498.0f) ||
        (Math_VectorGetDistance(activeSeg->pos, pathB) < 498.0f) ||
        (Math_VectorGetDistance(pathB, nextSeg->pos) < 498.0f)) {
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
