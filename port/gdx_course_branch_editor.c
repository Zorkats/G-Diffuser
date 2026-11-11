/* port/gdx_course_branch_editor.c -- Course Edit glue of the fork editor (gdx_course_branch_edit.c).
 *
 * The editor ring (D_802CB6D0 plus the per-point arrays of the course data) goes out to the edit library
 * as GdxBranchEditPoint and comes back in after a view swap. The decomp calls the hooks below from the
 * places that change the ring, save, load, test run and undo. The Split and Link tools live here too.
 */

#include "global.h"
#include "fzx_expansion_kit.h"
#include "fzx_course.h"
#include "fzx_camera.h"
#include "gdx_course_branch.h"
#include "gdx_course_branch_edit.h"
#include "gdx_course_branch_editor.h"
#include "gdx_course_edit_input.h"
#include "gdx_course_edit_undo.h"
#include "gdx_wind.h"
#include "gdx_course_bounds.h"

extern unk_807B3C20 D_802CB6D0;
extern unk_807B3C20 D_807B6528; /* the ring as the last backup took it (func_xk2_800EF78C) */
extern unk_80128690 D_80128690[GDX_COURSE_POINT_CAPACITY];
extern unk_800D6CA0 D_800D6CA0;
extern CourseInfo* gCurrentCourseInfo;
extern bool gInCourseEditTestRun;
extern s32 D_xk2_800F7040;
extern s32 D_xk2_800F704C;
extern s32 gCourseEditCursorXPos;
extern s32 gCourseEditCursorYPos;
extern s32 gCreateOption;
extern s32 gDesignStyleOption;
extern s32 gMoveOption;
extern void func_xk2_800E6F9C(void);
extern void func_xk2_800E6CA8(s32 arg0, CourseSegment arg1);
extern void func_xk2_800EF78C(void);
extern s32 func_xk2_800EF090(Vec3f arg0, s32* arg1, s32* arg2);
extern s32 func_i2_800BE8BC(CourseInfo* courseInfo);
extern void gdx_course_edit_select_point(s32 pointIndex, s32 toggle);
extern void gdx_course_edit_flash_message(const char* text);
extern int gdx_course_state_edit_file_id(char* out, int outCap);
extern int gdx_course_state_flush(void);
extern void gdx_course_branch_build_editor(void);
extern s32 gPartsStyleOption;
extern s32 gWindTypeOption;
extern Camera gCameras[];

#define EDITOR_LABEL_CAP 96
/* Squared game pixels, the mouse pick radius of ring points (gdx_course_edit_mouse.cpp). */
#define EDITOR_NODE_PICK_RADIUS_SQ 150.0f

static GdxBranchEditPoint sPoints[GDX_BRANCH_EDIT_MAX_POINTS];
static s32 sLinkFork = -1;
/* The segment a first Split press picked, or -1. */
static s32 sSplitStart = -1;

static s32 editor_swap_to_node(s32 fork, s32 node);

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

static void editor_first_fill(void) {
    editor_export();
    gdx_branch_edit_first_fill(sPoints, D_802CB6D0.controlPointCount);
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

void gdx_course_branch_editor_on_translate(float dx, float dz) {
    gdx_branch_edit_on_translate(dx, dz);
}

/* The editor's X and Z limits for a control point (the point movers in course_edit/188850.c). */
#define EDITOR_POINT_XZ_LIMIT 15000.0f

/* The TRACK row moved every ring point by one offset from the backup the grab made. The stored nodes of the paths
 * not showing take the same offset. The movers kept the ring inside the course's bounds but could not see those
 * nodes, so where one would leave the bounds the offset is cut short and the ring is pulled back by the same
 * amount: the track keeps its shape. */
void gdx_course_branch_editor_track_moved(void) {
    float delta[3];
    float lo[3];
    float hi[3];
    float fit[3];
    s32 i;

    if (D_802CB6D0.controlPointCount < 1) {
        return;
    }
    delta[0] = D_802CB6D0.unk_0000[0].pos.x - D_807B6528.unk_0000[0].pos.x;
    delta[1] = D_802CB6D0.unk_0000[0].pos.y - D_807B6528.unk_0000[0].pos.y;
    delta[2] = D_802CB6D0.unk_0000[0].pos.z - D_807B6528.unk_0000[0].pos.z;
    lo[0] = -EDITOR_POINT_XZ_LIMIT;
    lo[1] = gdx_course_edit_min_y();
    lo[2] = -EDITOR_POINT_XZ_LIMIT;
    hi[0] = EDITOR_POINT_XZ_LIMIT;
    hi[1] = gdx_course_edit_max_y();
    hi[2] = EDITOR_POINT_XZ_LIMIT;
    gdx_branch_edit_fit_move(delta, lo, hi, fit);
    if ((fit[0] != delta[0]) || (fit[1] != delta[1]) || (fit[2] != delta[2])) {
        for (i = 0; i < D_802CB6D0.controlPointCount; i++) {
            D_802CB6D0.unk_0000[i].pos.x += fit[0] - delta[0];
            D_802CB6D0.unk_0000[i].pos.y += fit[1] - delta[1];
            D_802CB6D0.unk_0000[i].pos.z += fit[2] - delta[2];
        }
        gdx_course_edit_flash_message("Move cut short: the other path is at the edge");
        Audio_TriggerSystemSE(NA_SE_32);
    }
    gdx_branch_edit_on_move(fit[0], fit[1], fit[2]);
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

#include "gdx_course_branch_save.inc"

/* The editor ring now holds a loaded course (func_xk2_800EACB0). */
void gdx_course_branch_editor_loaded(void) {
    char courseId[32];

    sLinkFork = -1;
    sSplitStart = -1;
    if (gdx_course_state_edit_file_id(courseId, sizeof(courseId))) {
        gdx_branch_edit_select(courseId, D_802CB6D0.controlPointCount);
        editor_first_fill();
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

int gdx_course_branch_editor_active_point(void) {
    s32 point = D_800D6CA0.unk_0C;

    return ((point >= 0) && (point < D_802CB6D0.controlPointCount)) ? point : -1;
}

int gdx_course_branch_editor_point_parts(int point) {
    const CourseData* courseData = &COURSE_CONTEXT()->courseData;
    s32 parts = 0;

    if ((point < 0) || (point >= D_802CB6D0.controlPointCount)) {
        return 0;
    }
    if ((courseData->pit[point] == PIT_MAGNET_LEFT) || (courseData->pit[point] == PIT_MAGNET_RIGHT) ||
        (courseData->pit[point] == PIT_MAGNET_MIDDLE)) {
        parts |= GDX_BRANCH_EDITOR_PART_MAGNET;
    } else if (courseData->pit[point] != PIT_NONE) {
        parts |= GDX_BRANCH_EDITOR_PART_PIT;
    }
    if (courseData->dirt[point] != DIRT_NONE) {
        parts |= GDX_BRANCH_EDITOR_PART_DIRT;
    }
    if (courseData->ice[point] != ICE_NONE) {
        parts |= GDX_BRANCH_EDITOR_PART_ICE;
    }
    if (courseData->dash[point] != DASH_NONE) {
        parts |= GDX_BRANCH_EDITOR_PART_DASH;
    }
    return parts;
}

/* The WIND part (Parts tab). A click places the picked row's wind on the point's segment. New wind blows the
 * camera's way; a segment that already has wind keeps its angle. The settings panel sets the exact angle. */
static s32 editor_camera_wind_angle(void) {
    s32 angle = gdx_wind_angle_of(gCameras[0].at.x - gCameras[0].eye.x, gCameras[0].at.z - gCameras[0].eye.z, 1.0f);

    return (angle < 0) ? 0 : angle;
}

/* The WIND part's TRACK and TURN rows act on the whole track, so a press anywhere on the course counts. TRACK steps
 * the track's wind through its strengths, and a new one blows the camera's way, on the nearest turn step. TURN
 * turns it one step. The overlay draws the result at the first point (port/gdx_course_edit_mouse.cpp). */
static void editor_track_wind_press(s32 turn) {
    static const char* const kStepped[] = { "Track wind: off", "Track wind: Weak", "Track wind: Mid",
                                            "Track wind: Strong", "Track wind: Gale" };
    s32 strength = gdx_branch_edit_track_setting(GDX_BRANCH_EDIT_TRACK_WIND_STRENGTH);
    s32 next;

    if (gInCourseEditTestRun) {
        Audio_TriggerSystemSE(NA_SE_32);
        return;
    }
    if (turn) {
        if (strength == 0) {
            gdx_course_edit_flash_message("Turn: the track has no wind yet");
            Audio_TriggerSystemSE(NA_SE_32);
            return;
        }
        func_xk2_800EF78C();
        gdx_branch_edit_set_track_setting(
            GDX_BRANCH_EDIT_TRACK_WIND_ANGLE,
            gdx_wind_angle_turn(gdx_branch_edit_track_setting(GDX_BRANCH_EDIT_TRACK_WIND_ANGLE)));
        Audio_TriggerSystemSE(NA_SE_39);
        return;
    }
    next = gdx_wind_track_step(strength);
    func_xk2_800EF78C();
    gdx_branch_edit_set_track_setting(GDX_BRANCH_EDIT_TRACK_WIND_STRENGTH, next);
    if (strength == 0) {
        gdx_branch_edit_set_track_setting(GDX_BRANCH_EDIT_TRACK_WIND_ANGLE,
                                          gdx_wind_angle_snap(editor_camera_wind_angle()));
    }
    gdx_course_edit_flash_message(kStepped[next / 25]);
    Audio_TriggerSystemSE(NA_SE_39);
}

void gdx_course_branch_editor_wind_place(void) {
    s32 point = D_800D6CA0.unk_0C;
    s32 strength;
    s32 current;
    s32 own;

    if (gPartsStyleOption != TRACK_PART_STYLE_WIND) {
        return;
    }
    if ((gWindTypeOption == GDX_WIND_PART_TRACK) || (gWindTypeOption == GDX_WIND_PART_TURN)) {
        editor_track_wind_press(gWindTypeOption == GDX_WIND_PART_TURN);
        return;
    }
    if ((point < 0) || (point >= D_802CB6D0.controlPointCount) || gInCourseEditTestRun) {
        Audio_TriggerSystemSE(NA_SE_32);
        return;
    }
    strength = gdx_wind_part_strength(gWindTypeOption);
    current = gdx_branch_edit_point_extra(point, GDX_BRANCH_EDIT_EXTRA_WIND_STRENGTH);
    if (current == strength) {
        return;
    }
    own = (strength >= 1) && (strength <= GDX_WIND_STRENGTH_MAX);
    func_xk2_800EF78C();
    gdx_branch_edit_set_point_extra(point, GDX_BRANCH_EDIT_EXTRA_WIND_STRENGTH, strength);
    if (own && ((current < 1) || (current > GDX_WIND_STRENGTH_MAX))) {
        gdx_branch_edit_set_point_extra(point, GDX_BRANCH_EDIT_EXTRA_WIND_ANGLE, editor_camera_wind_angle());
    }
    Audio_TriggerSystemSE(NA_SE_39);
}

/* decomp/src/overlays/ovl_i3/background.c: a cup course's stock horizon list, as images and compass angles. */
extern s32 Background_GdxStockList(s32 courseIndex, s32* images, s32* angles, s32 max);

/* Whether a GDX_BRANCH_EDITOR_HORIZON_* change can be made to the list as it is. */
static s32 editor_horizon_can(s32 which, s32 value) {
    s32 count = gdx_branch_edit_background_count();
    s32 images[GDX_BRANCH_EDIT_BACKGROUND_MAX];
    s32 angles[GDX_BRANCH_EDIT_BACKGROUND_MAX];

    if (which == GDX_BRANCH_EDITOR_HORIZON_COPY) {
        return Background_GdxStockList(value, images, angles, GDX_BRANCH_EDIT_BACKGROUND_MAX) >= 0;
    }
    if (which == GDX_BRANCH_EDITOR_HORIZON_REMOVE) {
        return (value >= 0) && (value < count);
    }
    if ((which >= GDX_BRANCH_EDITOR_HORIZON_ANGLE) &&
        (which < GDX_BRANCH_EDITOR_HORIZON_ANGLE + GDX_BRANCH_EDIT_BACKGROUND_MAX)) {
        return (which - GDX_BRANCH_EDITOR_HORIZON_ANGLE) < count;
    }
    if ((which >= GDX_BRANCH_EDITOR_HORIZON_IMAGE) &&
        (which < GDX_BRANCH_EDITOR_HORIZON_IMAGE + GDX_BRANCH_EDIT_BACKGROUND_MAX)) {
        return (which - GDX_BRANCH_EDITOR_HORIZON_IMAGE) <= count;
    }
    return 0;
}

/* Makes a GDX_BRANCH_EDITOR_HORIZON_* change. A new image stands a sixth of a turn past the last one, so a list
 * built by adding images spreads around the track. The race reads the list when it starts. */
static s32 editor_horizon_apply(s32 which, s32 value) {
    s32 count = gdx_branch_edit_background_count();
    s32 images[GDX_BRANCH_EDIT_BACKGROUND_MAX];
    s32 angles[GDX_BRANCH_EDIT_BACKGROUND_MAX];
    int image = 1;
    int angle = 0;
    s32 slot;

    if (!editor_horizon_can(which, value)) {
        return 0;
    }
    if (which == GDX_BRANCH_EDITOR_HORIZON_COPY) {
        count = Background_GdxStockList(value, images, angles, GDX_BRANCH_EDIT_BACKGROUND_MAX);
        gdx_branch_edit_set_backgrounds(images, angles, count);
        return 1;
    }
    if (which == GDX_BRANCH_EDITOR_HORIZON_REMOVE) {
        return gdx_branch_edit_remove_background(value);
    }
    if (which >= GDX_BRANCH_EDITOR_HORIZON_ANGLE) {
        slot = which - GDX_BRANCH_EDITOR_HORIZON_ANGLE;
        gdx_branch_edit_background(slot, &image, &angle);
        return gdx_branch_edit_set_background(slot, image, value);
    }
    slot = which - GDX_BRANCH_EDITOR_HORIZON_IMAGE;
    if (slot == count) {
        angle = (count * 60) % 360;
    } else {
        gdx_branch_edit_background(slot, &image, &angle);
    }
    return gdx_branch_edit_set_background(slot, value, angle);
}

int gdx_course_branch_editor_horizon_change(int which, int value) {
    if (gInCourseEditTestRun || !editor_horizon_can(which, value)) {
        Audio_TriggerSystemSE(NA_SE_32);
        return 0;
    }
    gdx_course_edit_undo_set_pending_label(GDX_COURSE_EDIT_UNDO_LABEL_OPTION);
    func_xk2_800EF78C();
    return editor_horizon_apply(which, value);
}

void gdx_course_branch_editor_apply_extra(void) {
    GdxCourseEditExtraRequest request;
    GdxCourseEditInt32 path;
    GdxCourseEditInt32 fork;
    GdxCourseEditInt32 node;

    if (gCreateOption == CREATE_OPTION_COURSE && gdx_course_edit_input_take_node_pick(&fork, &node) &&
        !gInCourseEditTestRun && gdx_course_edit_native_grab_state() == 0 &&
        gdx_course_edit_native_context_state() == 0 && gdx_course_edit_native_mouse_owned() == 0) {
        gdx_course_branch_editor_show_node(fork, node);
    }

    if (gdx_course_edit_input_take_path(&path) && !gInCourseEditTestRun &&
        gdx_course_edit_native_grab_state() == 0 && gdx_course_edit_native_context_state() == 0 &&
        gdx_course_edit_native_mouse_owned() == 0 && gdx_course_branch_editor_path_showing() != 0 &&
        path != gdx_course_branch_editor_path_showing()) {
        if (editor_swap_to_node(0, 0) >= 0) {
            // Point-indexed commands from the old ring must not edit the replacement ring.
            gdx_course_edit_input_clear();
        }
    }

    if (!gdx_course_edit_input_take_extra(&request)) {
        return;
    }
    if (gInCourseEditTestRun) {
        return;
    }
    if (!request.is_track && ((request.point < 0) || (request.point >= D_802CB6D0.controlPointCount))) {
        return;
    }
    if (request.begin_edit) {
        gdx_course_edit_undo_set_pending_label(GDX_COURSE_EDIT_UNDO_LABEL_OPTION);
        func_xk2_800EF78C();
    }
    if (request.is_track && (request.which >= GDX_BRANCH_EDITOR_HORIZON_IMAGE)) {
        editor_horizon_apply(request.which, request.value);
        return;
    }
    if (request.is_track) {
        /* A track the panel gives wind to starts blowing the camera's way, as a segment does. */
        s32 newWind = (request.which == GDX_BRANCH_EDIT_TRACK_WIND_STRENGTH) && (request.value > 0) &&
                      (gdx_branch_edit_track_setting(GDX_BRANCH_EDIT_TRACK_WIND_STRENGTH) == 0);

        gdx_branch_edit_set_track_setting(request.which, request.value);
        if (newWind) {
            gdx_branch_edit_set_track_setting(GDX_BRANCH_EDIT_TRACK_WIND_ANGLE, editor_camera_wind_angle());
        }
        /* The chunks carry the road theme they were built with. */
        if (request.which == GDX_BRANCH_EDIT_TRACK_ROAD_THEME) {
            D_xk2_800F7040 = 3;
        }
    } else {
        /* A segment the panel gives wind to starts blowing the camera's way. */
        s32 newWind = (request.which == GDX_BRANCH_EDIT_EXTRA_WIND_STRENGTH) && (request.value > 0) &&
                      (gdx_branch_edit_point_extra(request.point, GDX_BRANCH_EDIT_EXTRA_WIND_STRENGTH) == 0);

        gdx_branch_edit_set_point_extra(request.point, request.which, request.value);
        if (newWind) {
            gdx_branch_edit_set_point_extra(request.point, GDX_BRANCH_EDIT_EXTRA_WIND_ANGLE,
                                            editor_camera_wind_angle());
        }
        /* A width or a pad kind changes the strips and plates the effects were built with, and a road theme the
           chunks. */
        if ((request.which == GDX_BRANCH_EDIT_EXTRA_PIT_WIDTH) || (request.which == GDX_BRANCH_EDIT_EXTRA_DIRT_WIDTH) ||
            (request.which == GDX_BRANCH_EDIT_EXTRA_ICE_WIDTH) || (request.which == GDX_BRANCH_EDIT_EXTRA_PAD_KIND) ||
            (request.which == GDX_BRANCH_EDIT_EXTRA_PAD_WIDTH) ||
            (request.which == GDX_BRANCH_EDIT_EXTRA_ROAD_THEME)) {
            D_xk2_800F7040 = 3;
        }
    }
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
        gdx_branch_edit_point_clear_parts(node);
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

    rc = gdx_branch_edit_split_check(segment, count, reason, sizeof(reason));
    if (rc != 0) {
        editor_refuse((rc == 1) ? "Split: this segment is already a fork" : reason);
        return;
    }
    if (count < 4) {
        editor_refuse("Split: the course needs 4 or more points");
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
    gdx_branch_edit_point_clear_parts(&nodeB);
    gdx_branch_edit_split_commit(segment, &nodeB);
    editor_first_fill();

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
    editor_first_fill();
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

static f32 editor_screen_distance_sq(Vec3f pos, f32 x, f32 y) {
    s32 sx;
    s32 sy;

    if (func_xk2_800EF090(pos, &sx, &sy) != 0) {
        return 1.0e9f;
    }
    return SQ(sx - x) + SQ(sy - y);
}

int gdx_course_branch_editor_stored_node_at(float x, float y, int* fork, int* node) {
    static f32 sNodes[GDX_BRANCH_EDIT_MAX_POINTS][3];
    f32 best = EDITOR_NODE_PICK_RADIUS_SQ;
    s32 forkPoint;
    s32 mergePoint;
    s32 nodes;
    s32 found = 0;
    s32 f;
    s32 i;

    /* Line up builds a selection point by point, and a switch of paths would clear it. */
    if (gInCourseEditTestRun || ((gCreateOption == CREATE_OPTION_POINT) && (gMoveOption == MOVE_OPTION_STRAIGHT))) {
        return 0;
    }
    for (i = 0; i < D_802CB6D0.controlPointCount; i++) {
        if (editor_screen_distance_sq(D_802CB6D0.unk_0000[i].pos, x, y) < EDITOR_NODE_PICK_RADIUS_SQ) {
            return 0;
        }
    }
    for (f = 0; f < gdx_branch_edit_fork_count(); f++) {
        nodes = gdx_branch_edit_fork_shape(f, &forkPoint, &mergePoint, sNodes, GDX_BRANCH_EDIT_MAX_POINTS);
        for (i = 0; i < nodes; i++) {
            Vec3f pos = { sNodes[i][0], sNodes[i][1], sNodes[i][2] };
            f32 distance = editor_screen_distance_sq(pos, x, y);

            if (distance < best) {
                best = distance;
                *fork = f;
                *node = i;
                found = 1;
            }
        }
    }
    return found;
}

/* Brings stored node `node` of fork f into the ring. Returns its ring index, or -1. */
static s32 editor_swap_to_node(s32 fork, s32 node) {
    s32 count = D_802CB6D0.controlPointCount;
    int point = -1;
    s32 newCount;

    editor_export();
    newCount = gdx_branch_edit_swap_to_node(sPoints, count, GDX_BRANCH_EDIT_MAX_POINTS, fork, node, &point);
    if (newCount < 0) {
        editor_refuse("Can't switch paths: a fork has no points on one path");
        return -1;
    }
    editor_import(newCount);
    gdx_course_edit_flash_message(gdx_branch_edit_view_b() ? "Showing path B" : "Showing path A");
    return point;
}

void gdx_course_branch_editor_show_node(int fork, int node) {
    s32 point = editor_swap_to_node(fork, node);

    if (point >= 0) {
        gdx_course_edit_input_clear();
        gdx_course_edit_select_point(point, 0);
    }
}

void gdx_course_branch_editor_press_at_cursor(void) {
    int fork;
    int node;

    if (gdx_course_branch_editor_stored_node_at(gCourseEditCursorXPos, gCourseEditCursorYPos, &fork, &node)) {
        editor_swap_to_node(fork, node);
    }
}
