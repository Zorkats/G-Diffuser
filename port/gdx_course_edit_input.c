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
static GdxCourseEditMask sSelectionAddMask;
static GdxCourseEditInt32 sSelectionAddPending;
static GdxCourseEditInt32 sTestEscDown;
static GdxCourseEditInt32 sTestEscOwned;
static GdxCourseEditInt32 sTestEscPending;
static GdxCourseEditInt32 sMoveTargetPending;
static GdxCourseEditInt32 sMoveTargetOption;
static GdxCourseEditVec3 sMoveTarget;
static GdxCourseEditInt32 sScalarTargetPending;
static GdxCourseEditInt32 sScalarTargetOption;
static GdxCourseEditInt32 sScalarTargetOffset;
static GdxCourseEditInt32 sLineUpPending;
static GdxCourseEditInt32 sNodePickPending;
static GdxCourseEditInt32 sNodePickFork;
static GdxCourseEditInt32 sNodePickNode;
static GdxCourseEditInt32 sExtraPending;
static GdxCourseEditExtraRequest sExtra;
static GdxCourseEditInt32 sBankValuePending;
static GdxCourseEditInt32 sBankValue;
static GdxCourseEditInt32 sBankEntryActive;
static GdxCourseEditInt32 sBankEntryLength;
// A sign, four digits and the end of the string.
static char sBankEntryText[6];

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

void gdx_course_edit_input_request_selection_add_mask(GdxCourseEditMask mask) {
    sSelectionAddMask |= mask;
    sSelectionAddPending = 1;
}

GdxCourseEditInt32 gdx_course_edit_input_take_selection_add_mask(GdxCourseEditMask* mask) {
    if (!sSelectionAddPending) {
        return 0;
    }
    if (mask != 0) {
        *mask = sSelectionAddMask;
    }
    sSelectionAddMask = 0;
    sSelectionAddPending = 0;
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

void gdx_course_edit_input_request_line_up(void) {
    sLineUpPending = 1;
}

GdxCourseEditInt32 gdx_course_edit_input_take_line_up(void) {
    GdxCourseEditInt32 pending = sLineUpPending;
    sLineUpPending = 0;
    return pending;
}

GdxCourseEditInt32 gdx_course_edit_bank_snap_next(GdxCourseEditInt32 bank, GdxCourseEditInt32 step,
                                                  GdxCourseEditInt32 direction) {
    GdxCourseEditInt32 next;

    if (step == 0) {
        return bank;
    }
    if (direction > 0) {
        next = ((bank / step) + 1) * step;
    } else {
        next = (bank % step == 0) ? bank - step : bank - (bank % step);
    }
    return (next + 360) % 360;
}

GdxCourseEditInt32 gdx_course_edit_bank_snap_nearest(GdxCourseEditInt32 bank, GdxCourseEditInt32 step) {
    if (step == 0) {
        return bank;
    }
    return (((bank + (step / 2)) / step) * step) % 360;
}

void gdx_course_edit_bank_entry_begin(void) {
    sBankEntryActive = 1;
    sBankEntryLength = 0;
    sBankEntryText[0] = '\0';
}

void gdx_course_edit_bank_entry_cancel(void) {
    sBankEntryActive = 0;
}

GdxCourseEditInt32 gdx_course_edit_bank_entry_active(void) {
    return sBankEntryActive;
}

void gdx_course_edit_bank_entry_push_digit(GdxCourseEditInt32 digit) {
    GdxCourseEditInt32 digits = sBankEntryLength - ((sBankEntryText[0] == '-') ? 1 : 0);

    if (!sBankEntryActive || digit < 0 || digit > 9 || digits >= 4) {
        return;
    }
    sBankEntryText[sBankEntryLength++] = (char) ('0' + digit);
    sBankEntryText[sBankEntryLength] = '\0';
}

void gdx_course_edit_bank_entry_toggle_sign(void) {
    GdxCourseEditInt32 i;

    if (!sBankEntryActive) {
        return;
    }
    if (sBankEntryText[0] == '-') {
        for (i = 0; i < sBankEntryLength; i++) {
            sBankEntryText[i] = sBankEntryText[i + 1];
        }
        sBankEntryLength--;
    } else {
        for (i = sBankEntryLength + 1; i > 0; i--) {
            sBankEntryText[i] = sBankEntryText[i - 1];
        }
        sBankEntryText[0] = '-';
        sBankEntryLength++;
    }
}

void gdx_course_edit_bank_entry_backspace(void) {
    if (sBankEntryActive && sBankEntryLength > 0) {
        sBankEntryText[--sBankEntryLength] = '\0';
    }
}

const char* gdx_course_edit_bank_entry_text(void) {
    return sBankEntryText;
}

GdxCourseEditInt32 gdx_course_edit_bank_entry_commit(GdxCourseEditInt32* typed) {
    GdxCourseEditInt32 negative = sBankEntryText[0] == '-';
    GdxCourseEditInt32 value = 0;
    GdxCourseEditInt32 i;

    if (!sBankEntryActive || sBankEntryLength == negative) {
        sBankEntryActive = 0;
        return 0;
    }
    for (i = negative; i < sBankEntryLength; i++) {
        value = (value * 10) + (sBankEntryText[i] - '0');
    }
    *typed = negative ? -value : value;
    sBankEntryActive = 0;
    return 1;
}

GdxCourseEditInt32 gdx_course_edit_bank_value_normalize(GdxCourseEditInt32 typed) {
    return gdx_course_edit_bank_snap_nearest(((typed % 360) + 360) % 360, 3);
}

void gdx_course_edit_input_request_bank_value(GdxCourseEditInt32 bank) {
    sBankValue = bank;
    sBankValuePending = 1;
}

GdxCourseEditInt32 gdx_course_edit_input_take_bank_value(GdxCourseEditInt32* bank) {
    if (!sBankValuePending) {
        return 0;
    }
    *bank = sBankValue;
    sBankValuePending = 0;
    return 1;
}

void gdx_course_edit_input_clear_drag(void) {
    sDragStartPending = 0;
    sDragCommitPending = 0;
    sDragCancelPending = 0;
    sMoveTargetPending = 0;
    sScalarTargetPending = 0;
}

void gdx_course_edit_input_request_node_pick(GdxCourseEditInt32 fork, GdxCourseEditInt32 node) {
    sNodePickFork = fork;
    sNodePickNode = node;
    sNodePickPending = 1;
}

GdxCourseEditInt32 gdx_course_edit_input_take_node_pick(GdxCourseEditInt32* fork, GdxCourseEditInt32* node) {
    if (!sNodePickPending) {
        return 0;
    }
    *fork = sNodePickFork;
    *node = sNodePickNode;
    sNodePickPending = 0;
    return 1;
}

void gdx_course_edit_input_request_extra(const GdxCourseEditExtraRequest* request) {
    GdxCourseEditInt32 begin = request->begin_edit || (sExtraPending && sExtra.begin_edit);

    sExtra = *request;
    sExtra.begin_edit = begin;
    sExtraPending = 1;
}

GdxCourseEditInt32 gdx_course_edit_input_take_extra(GdxCourseEditExtraRequest* request) {
    if (!sExtraPending) {
        return 0;
    }
    *request = sExtra;
    sExtraPending = 0;
    return 1;
}

void gdx_course_edit_input_clear(void) {
    sSelectionAddMask = 0;
    sSelectionAddPending = 0;
    gdx_course_edit_input_clear_drag();
    sDeletePending = 0;
    sSelectionPending = 0;
    sLineUpPending = 0;
    sNodePickPending = 0;
    sExtraPending = 0;
    sBankValuePending = 0;
    sBankEntryActive = 0;
}
