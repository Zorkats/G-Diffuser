#include "global.h"
#include "unk_structs.h"
#include "gdx_course_edit_input.h"
#include "gdx_course_edit_undo.h"

extern unk_807B3C20 D_802CB6D0;
extern unk_800D6CA0 D_800D6CA0;
extern unk_80128690 D_80128690[64];
extern s32 D_xk2_80119918;

// Undo backup and the native line-up, both in decomp/src/overlays/course_edit/1A1240.c.
void func_xk2_800EF78C(void);
void func_xk2_800EFCD0(void);

// 498 units squared, so the spacing check needs no sqrt. Same limit as func_xk2_800EFA94.
#define GDX_COURSE_EDIT_LINE_UP_SPACING_SQ (498.0f * 498.0f)

s32 gdx_course_edit_native_context_state(void) {
    return D_800D6CA0.unk_08;
}

s32 gdx_course_edit_native_grab_state(void) {
    return D_800D6CA0.unk_00;
}

s32 gdx_course_edit_native_help_active(void) {
    return D_xk2_80119918;
}

s32 gdx_course_edit_native_active_point(void) {
    return D_800D6CA0.unk_0C;
}

s32 gdx_course_edit_native_point_count(void) {
    s32 count = D_802CB6D0.controlPointCount;
    if (count < 0) {
        return 0;
    }
    if (count > 64) {
        return 64;
    }
    return count;
}

s32 gdx_course_edit_native_point_selected(s32 pointIndex) {
    if ((pointIndex < 0) || (pointIndex >= gdx_course_edit_native_point_count())) {
        return 0;
    }
    return D_80128690[pointIndex].unk_08 != 0;
}

// Box select: adds the masked control points to the selection and leaves the others selected. Writes
// the same fields as gdx_course_edit_select_point (188850.c). Returns how many points became selected;
// the caller raises the native refresh flag when that is non-zero.
s32 gdx_course_edit_native_add_selection_mask(GdxCourseEditMask mask) {
    s32 i;
    s32 added = 0;
    s32 firstAdded = -1;
    s32 count = gdx_course_edit_native_point_count();

    for (i = 0; i < count; i++) {
        if ((((mask >> i) & 1) == 0) || (D_80128690[i].unk_08 != 0)) {
            continue;
        }
        D_80128690[i].unk_08 = 1;
        if (firstAdded < 0) {
            firstAdded = i;
        }
        added++;
    }
    if (firstAdded >= 0) {
        D_800D6CA0.unk_0C = firstAdded;
    }
    return added;
}

GdxCourseEditMask gdx_course_edit_native_selection_mask(void) {
    s32 count = gdx_course_edit_native_point_count();
    GdxCourseEditMask mask = 0;
    s32 i;

    for (i = 0; i < count; i++) {
        if (gdx_course_edit_native_point_selected(i)) {
            mask |= (GdxCourseEditMask)1 << i;
        }
    }
    return mask;
}

// Box select replace: the masked points become the whole selection. Every flag is rewritten, the same way
// func_xk2_800DC3F8 clears them, so flags past the point count cannot linger. The active point moves to the
// lowest masked index when the mask is not empty; an empty mask leaves it alone.
void gdx_course_edit_native_set_selection_mask(GdxCourseEditMask mask) {
    s32 count = gdx_course_edit_native_point_count();
    s32 firstSelected = -1;
    s32 i;

    for (i = 0; i < 64; i++) {
        s32 selected = (i < count) && (((mask >> i) & 1) != 0);

        D_80128690[i].unk_08 = selected;
        if (selected && (firstSelected < 0)) {
            firstSelected = i;
        }
    }
    if (firstSelected >= 0) {
        D_800D6CA0.unk_0C = firstSelected;
    }
}

s32 gdx_course_edit_native_point_position(s32 pointIndex, GdxCourseEditVec3* position) {
    if ((position == NULL) || (pointIndex < 0) || (pointIndex >= gdx_course_edit_native_point_count())) {
        return 0;
    }
    position->x = D_802CB6D0.unk_0000[pointIndex].pos.x;
    position->y = D_802CB6D0.unk_0000[pointIndex].pos.y;
    position->z = D_802CB6D0.unk_0000[pointIndex].pos.z;
    return 1;
}

static GdxCourseEditVec3 segment_position(const CourseSegment* segment) {
    GdxCourseEditVec3 position;

    position.x = segment->pos.x;
    position.y = segment->pos.y;
    position.z = segment->pos.z;
    return position;
}

// Projects point onto the infinite line through a and b, the same projection as func_xk2_800F267C. Returns 0
// when a and b coincide, because the line then has no direction and the decomp would write NaN.
static s32 project_to_line(GdxCourseEditVec3 point, GdxCourseEditVec3 a, GdxCourseEditVec3 b,
                           GdxCourseEditVec3* out) {
    float dx = b.x - a.x;
    float dy = b.y - a.y;
    float dz = b.z - a.z;
    float lengthSq = (dx * dx) + (dy * dy) + (dz * dz);
    float t;

    if (lengthSq == 0.0f) {
        return 0;
    }
    t = ((dx * (point.x - a.x)) + (dy * (point.y - a.y)) + (dz * (point.z - a.z))) / lengthSq;
    out->x = a.x + (dx * t);
    out->y = a.y + (dy * t);
    out->z = a.z + (dz * t);
    return 1;
}

static s32 too_close(GdxCourseEditVec3 a, GdxCourseEditVec3 b) {
    float dx = b.x - a.x;
    float dy = b.y - a.y;
    float dz = b.z - a.z;

    return ((dx * dx) + (dy * dy) + (dz * dz)) < GDX_COURSE_EDIT_LINE_UP_SPACING_SQ;
}

// Finds the run that func_xk2_800EFCD0 would line up and where each of its points would land, without
// writing any point. The anchor, the one-lap ring walk, the strictly-longest rule, and the 3-point and
// 498-unit checks follow func_xk2_800EFCD0 and func_xk2_800EFA94. preview->count is set only for OK.
s32 gdx_course_edit_native_line_up_scan(GdxCourseEditLineUpPreview* preview) {
    GdxCourseEditLineUpPreview scratch;
    GdxCourseEditVec3 startPosition;
    GdxCourseEditVec3 endPosition;
    CourseSegment* cursor;
    CourseSegment* runStart = NULL;
    CourseSegment* bestStart = NULL;
    CourseSegment* bestEnd = NULL;
    s32 count = gdx_course_edit_native_point_count();
    s32 anchor = -1;
    s32 runLength = 0;
    s32 bestLength = 0;
    s32 step;
    s32 i;

    if (preview == NULL) {
        preview = &scratch;
    }
    preview->count = 0;

    for (i = 0; i < count; i++) {
        if (!gdx_course_edit_native_point_selected(i)) {
            anchor = i;
            break;
        }
    }
    if (anchor < 0) {
        return GDX_COURSE_EDIT_LINE_UP_TOO_FEW;
    }

    // One lap of the ring from the first unselected point, so a run never wraps past the anchor. A tie keeps
    // the run found first in this walk.
    cursor = &D_802CB6D0.unk_0000[anchor];
    for (step = 0; step < count; step++) {
        if (gdx_course_edit_native_point_selected(cursor->segmentIndex)) {
            if (runLength == 0) {
                runStart = cursor;
            }
            runLength++;
            if (bestLength < runLength) {
                bestLength = runLength;
                bestStart = runStart;
                bestEnd = cursor;
            }
        } else {
            runLength = 0;
        }
        cursor = cursor->next;
    }
    if (bestLength < 3) {
        return GDX_COURSE_EDIT_LINE_UP_TOO_FEW;
    }

    startPosition = segment_position(bestStart);
    endPosition = segment_position(bestEnd);
    cursor = bestStart;
    for (i = 0; i < bestLength; i++) {
        if (i == bestLength - 1) {
            // The end point stays where it is, as in func_xk2_800EFA94.
            preview->landing[i] = segment_position(cursor);
        } else if (!project_to_line(segment_position(cursor), startPosition, endPosition, &preview->landing[i])) {
            return GDX_COURSE_EDIT_LINE_UP_TOO_CLOSE;
        }
        cursor = cursor->next;
    }

    for (i = 0; i + 1 < bestLength; i++) {
        if (too_close(preview->landing[i], preview->landing[i + 1])) {
            return GDX_COURSE_EDIT_LINE_UP_TOO_CLOSE;
        }
    }

    preview->count = bestLength;
    preview->start = preview->landing[0];
    preview->end = preview->landing[bestLength - 1];
    return GDX_COURSE_EDIT_LINE_UP_OK;
}

void gdx_course_edit_native_play_refusal(void) {
    Audio_TriggerSystemSE(NA_SE_32);
}

// Tick side of Enter in tool 6. The scan runs again here, so the backup is only taken when the line-up is
// certain to go through, and a refusal from the game tick is reported the same way as one from the host.
void gdx_course_edit_native_apply_line_up(void) {
    GdxCourseEditLineUpPreview preview;

    if (!gdx_course_edit_input_take_line_up()) {
        return;
    }
    if (gdx_course_edit_native_line_up_scan(&preview) != GDX_COURSE_EDIT_LINE_UP_OK) {
        gdx_course_edit_native_play_refusal();
        return;
    }
    gdx_course_edit_undo_set_pending_label(GDX_COURSE_EDIT_UNDO_LABEL_LINE_UP);
    func_xk2_800EF78C();
    func_xk2_800EFCD0();
}
