#include "gdx_course_edit_input.h"

static GdxCourseEditInt32 sDragStartPending;
static GdxCourseEditInt32 sDragStartPoint;
static GdxCourseEditInt32 sDragCommitPending;
static GdxCourseEditInt32 sDragCancelPending;
static GdxCourseEditInt32 sDeletePending;
static GdxCourseEditMask sDeleteSelection;
static GdxCourseEditInt32 sDeleteCount;
static GdxCourseEditInt32 sSelectionPending;
static GdxCourseEditInt32 sSelectionPoint;
static GdxCourseEditInt32 sSelectionToggle;
static GdxCourseEditInt32 sTestEscDown;
static GdxCourseEditInt32 sTestEscOwned;
static GdxCourseEditInt32 sTestEscPending;
static GdxCourseEditInt32 sMoveTargetPending;
static GdxCourseEditInt32 sMoveTargetOption;
static GdxCourseEditVec3 sMoveTarget;
static GdxCourseEditInt32 sScalarTargetPending;
static GdxCourseEditInt32 sScalarTargetOption;
static GdxCourseEditInt32 sScalarTargetOffset;

void gdx_course_edit_input_request_drag_start(GdxCourseEditInt32 point_index) {
    sDragStartPoint = point_index;
    sDragStartPending = 1;
}

GdxCourseEditInt32 gdx_course_edit_input_take_drag_start(GdxCourseEditInt32* point_index) {
    if (!sDragStartPending) {
        return 0;
    }
    if (point_index != 0) {
        *point_index = sDragStartPoint;
    }
    sDragStartPending = 0;
    return 1;
}

void gdx_course_edit_input_request_drag_commit(void) {
    sDragCommitPending = 1;
}

GdxCourseEditInt32 gdx_course_edit_input_take_drag_commit(void) {
    GdxCourseEditInt32 pending = sDragCommitPending;
    sDragCommitPending = 0;
    return pending;
}

void gdx_course_edit_input_request_drag_cancel(void) {
    sDragCancelPending = 1;
}

GdxCourseEditInt32 gdx_course_edit_input_take_drag_cancel(void) {
    GdxCourseEditInt32 pending = sDragCancelPending;
    sDragCancelPending = 0;
    return pending;
}

void gdx_course_edit_input_request_delete(GdxCourseEditMask selection, GdxCourseEditInt32 count) {
    sDeleteSelection = selection;
    sDeleteCount = count;
    sDeletePending = 1;
}

GdxCourseEditInt32 gdx_course_edit_input_take_delete(GdxCourseEditMask selection, GdxCourseEditInt32 count) {
    GdxCourseEditInt32 pending = sDeletePending && selection != 0 &&
        selection == sDeleteSelection && count == sDeleteCount;
    sDeletePending = 0;
    return pending;
}

void gdx_course_edit_input_request_selection(GdxCourseEditInt32 point, GdxCourseEditInt32 toggle) {
    sSelectionPoint = point;
    sSelectionToggle = toggle;
    sSelectionPending = 1;
}

GdxCourseEditInt32 gdx_course_edit_input_take_selection(GdxCourseEditInt32* point, GdxCourseEditInt32* toggle) {
    if (!sSelectionPending) {
        return 0;
    }
    *point = sSelectionPoint;
    *toggle = sSelectionToggle;
    sSelectionPending = 0;
    return 1;
}

GdxCourseEditInt32 gdx_course_edit_input_drag_start_pending(void) {
    return sDragStartPending;
}

GdxCourseEditInt32 gdx_course_edit_input_test_esc(GdxCourseEditInt32 down, GdxCourseEditInt32 active) {
    GdxCourseEditInt32 owned = sTestEscOwned;
    if (down && !sTestEscDown && active) {
        sTestEscPending = 1;
        sTestEscOwned = 1;
        owned = 1;
    }
    sTestEscDown = down;
    if (!down) {
        sTestEscOwned = 0;
    }
    return owned;
}

GdxCourseEditInt32 gdx_course_edit_input_take_test_esc(GdxCourseEditInt32 active) {
    GdxCourseEditInt32 pending = sTestEscPending && active;
    sTestEscPending = 0;
    return pending;
}

void gdx_course_edit_input_clear_test_esc(void) {
    sTestEscPending = 0;
}

void gdx_course_edit_input_request_move_target(GdxCourseEditInt32 move_option, GdxCourseEditVec3 target) {
    sMoveTargetOption = move_option;
    sMoveTarget = target;
    sMoveTargetPending = 1;
}

GdxCourseEditInt32 gdx_course_edit_input_take_move_target(GdxCourseEditInt32* move_option,
                                                          GdxCourseEditVec3* target) {
    if (!sMoveTargetPending) {
        return 0;
    }
    if (move_option != 0) {
        *move_option = sMoveTargetOption;
    }
    if (target != 0) {
        *target = sMoveTarget;
    }
    sMoveTargetPending = 0;
    return 1;
}

void gdx_course_edit_input_request_scalar_target(GdxCourseEditInt32 move_option,
                                                 GdxCourseEditInt32 target_offset) {
    sScalarTargetOption = move_option;
    sScalarTargetOffset = target_offset;
    sScalarTargetPending = 1;
}

GdxCourseEditInt32 gdx_course_edit_input_take_scalar_target(GdxCourseEditInt32* move_option,
                                                            GdxCourseEditInt32* target_offset) {
    if (!sScalarTargetPending) {
        return 0;
    }
    if (move_option != 0) {
        *move_option = sScalarTargetOption;
    }
    if (target_offset != 0) {
        *target_offset = sScalarTargetOffset;
    }
    sScalarTargetPending = 0;
    return 1;
}

void gdx_course_edit_input_clear_drag(void) {
    sDragStartPending = 0;
    sDragCommitPending = 0;
    sDragCancelPending = 0;
    sMoveTargetPending = 0;
    sScalarTargetPending = 0;
}

void gdx_course_edit_input_clear(void) {
    gdx_course_edit_input_clear_drag();
    sDeletePending = 0;
    sSelectionPending = 0;
}
