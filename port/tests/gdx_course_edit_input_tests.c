#include "gdx_course_edit_input.h"
#include "gdx_course_edit_menu.h"

#include <assert.h>
#include <limits.h>
#include <math.h>
#include <string.h>

static void test_one_shot_actions(void) {
    GdxCourseEditInt32 point = -1;

    gdx_course_edit_input_request_drag_start(63);
    assert(gdx_course_edit_input_take_drag_start(&point) == 1);
    assert(point == 63);
    assert(gdx_course_edit_input_take_drag_start(&point) == 0);

    gdx_course_edit_input_request_drag_commit();
    assert(gdx_course_edit_input_take_drag_commit() == 1);
    assert(gdx_course_edit_input_take_drag_commit() == 0);

    gdx_course_edit_input_request_drag_cancel();
    assert(gdx_course_edit_input_take_drag_cancel() == 1);
    assert(gdx_course_edit_input_take_drag_cancel() == 0);

    gdx_course_edit_input_request_delete(gdx_course_mask_low(3), 4);
    assert(gdx_course_edit_input_take_delete(gdx_course_mask_low(3), 4) == 1);
    assert(gdx_course_edit_input_take_delete(gdx_course_mask_low(3), 4) == 0);

    gdx_course_edit_input_request_line_up();
    assert(gdx_course_edit_input_take_line_up() == 1);
    assert(gdx_course_edit_input_take_line_up() == 0);

    GdxCourseEditInt32 fork = -1;
    GdxCourseEditInt32 node = -1;
    gdx_course_edit_input_request_node_pick(2, 5);
    assert(gdx_course_edit_input_take_node_pick(&fork, &node) == 1);
    assert(fork == 2 && node == 5);
    assert(gdx_course_edit_input_take_node_pick(&fork, &node) == 0);

    gdx_course_edit_input_request_node_pick(1, 1);
    gdx_course_edit_input_clear();
    assert(gdx_course_edit_input_take_node_pick(&fork, &node) == 0);
}

static void test_latest_continuous_command_wins(void) {
    GdxCourseEditVec3 target = { 0.0f, 0.0f, 0.0f };
    GdxCourseEditInt32 move_option = -1;
    GdxCourseEditInt32 offset = 0;

    gdx_course_edit_input_request_move_target(0, (GdxCourseEditVec3){ 10.0f, 20.0f, 30.0f });
    gdx_course_edit_input_request_move_target(1, (GdxCourseEditVec3){ 40.0f, 50.0f, 60.0f });
    assert(gdx_course_edit_input_take_move_target(&move_option, &target) == 1);
    assert(move_option == 1);
    assert(target.x == 40.0f && target.y == 50.0f && target.z == 60.0f);
    assert(gdx_course_edit_input_take_move_target(&move_option, &target) == 0);

    gdx_course_edit_input_request_scalar_target(2, 10);
    gdx_course_edit_input_request_scalar_target(4, -30);
    assert(gdx_course_edit_input_take_scalar_target(&move_option, &offset) == 1);
    assert(move_option == 4 && offset == -30);
    assert(gdx_course_edit_input_take_scalar_target(&move_option, &offset) == 0);
}

static void test_clear_drops_stale_commands(void) {
    gdx_course_edit_input_request_drag_start(1);
    gdx_course_edit_input_request_drag_commit();
    gdx_course_edit_input_request_drag_cancel();
    gdx_course_edit_input_request_delete(gdx_course_mask_low(3), 4);
    gdx_course_edit_input_request_move_target(0, (GdxCourseEditVec3){ 1.0f, 2.0f, 3.0f });
    gdx_course_edit_input_request_scalar_target(2, 20);
    gdx_course_edit_input_request_line_up();
    gdx_course_edit_input_clear();

    assert(gdx_course_edit_input_take_drag_start(0) == 0);
    assert(gdx_course_edit_input_take_drag_commit() == 0);
    assert(gdx_course_edit_input_take_drag_cancel() == 0);
    assert(gdx_course_edit_input_take_delete(gdx_course_mask_low(3), 4) == 0);
    assert(gdx_course_edit_input_take_move_target(0, 0) == 0);
    assert(gdx_course_edit_input_take_scalar_target(0, 0) == 0);
    assert(gdx_course_edit_input_take_line_up() == 0);
}

static void test_delete_selection_validation(void) {
    gdx_course_edit_input_request_delete(gdx_course_mask_low(3), 4);
    assert(!gdx_course_edit_input_take_delete(gdx_course_mask_low(1), 4));
    assert(!gdx_course_edit_input_take_delete(gdx_course_mask_low(3), 4));
    gdx_course_edit_input_request_delete(gdx_course_mask_low(3), 4);
    assert(!gdx_course_edit_input_take_delete(gdx_course_mask_low(3), 5));
    gdx_course_edit_input_request_delete(gdx_course_mask_low(0), 4);
    assert(!gdx_course_edit_input_take_delete(gdx_course_mask_low(0), 4));
}

static void test_test_run_escape_edges(void) {
    assert(gdx_course_edit_input_test_esc(1, 1));
    assert(gdx_course_edit_input_test_esc(1, 1));
    assert(gdx_course_edit_input_take_test_esc(1));
    assert(!gdx_course_edit_input_take_test_esc(1));
    assert(gdx_course_edit_input_test_esc(1, 1));
    assert(!gdx_course_edit_input_take_test_esc(1));
    assert(gdx_course_edit_input_test_esc(0, 1));
    assert(gdx_course_edit_input_test_esc(1, 1));
    assert(gdx_course_edit_input_take_test_esc(1));
    assert(gdx_course_edit_input_test_esc(0, 0));
    assert(!gdx_course_edit_input_test_esc(1, 0));
    assert(!gdx_course_edit_input_test_esc(1, 1));
    assert(!gdx_course_edit_input_take_test_esc(1));
    gdx_course_edit_input_test_esc(0, 0);
    gdx_course_edit_input_test_esc(1, 1);
    gdx_course_edit_input_clear_test_esc();
    assert(!gdx_course_edit_input_take_test_esc(1));
    assert(gdx_course_edit_input_test_esc(1, 1));
    assert(!gdx_course_edit_input_take_test_esc(1));
    assert(gdx_course_edit_input_test_esc(0, 0));
    assert(gdx_course_edit_input_test_esc(1, 1));
    assert(gdx_course_edit_input_take_test_esc(1));
    assert(gdx_course_edit_input_test_esc(0, 0));
}

static void test_menu_bounds(void) {
    assert(gdx_course_edit_menu_scroll(0, 12, 16, 1) == 16);
    assert(gdx_course_edit_menu_scroll(16, 12, 16, 8) == 32);
    assert(gdx_course_edit_menu_scroll(32, 12, 16, -8) == 0);
    assert(gdx_course_edit_menu_scroll(16, 10, 16, 1) == 0);
    assert(gdx_course_edit_menu_scroll(16, 12, 0, 1) == 0);
    assert(!gdx_course_edit_menu_visible_row(0, 12, 16, 16));
    assert(gdx_course_edit_menu_visible_row(10, 12, 16, 16));
    assert(!gdx_course_edit_menu_visible_row(11, 12, 16, 16));
    assert(gdx_course_edit_menu_arrow_hit(72, 52, 16, 12, 0, 80, 212));
    assert(!gdx_course_edit_menu_arrow_hit(72, 52, 16, 12, 32, 80, 212));
    assert(!gdx_course_edit_menu_arrow_hit(72, 52, 16, 12, 0, 112, 212));
    assert(!gdx_course_edit_menu_arrow_hit(72, 52, 16, 12, 0, 80, 228));
    assert(gdx_course_edit_list_scroll(0, 20, 8, 8, 13) == 56);
    assert(gdx_course_edit_list_scroll(56, 20, 8, -8, 13) == 0);
    assert(gdx_course_edit_list_scroll(0, 13, 8, 1, 13) == 0);
}

static void test_drag_cancellation_keeps_other_intents(void) {
    GdxCourseEditInt32 point;
    GdxCourseEditInt32 toggle;
    gdx_course_edit_input_request_selection(63, 1);
    gdx_course_edit_input_request_delete(gdx_course_mask_low(3), 4);
    gdx_course_edit_input_request_drag_start(1);
    assert(gdx_course_edit_input_drag_start_pending());
    gdx_course_edit_input_clear_drag();
    assert(!gdx_course_edit_input_drag_start_pending());
    assert(gdx_course_edit_input_take_selection(&point, &toggle));
    assert(point == 63 && toggle == 1);
    assert(!gdx_course_edit_input_take_selection(&point, &toggle));
    assert(gdx_course_edit_input_take_delete(gdx_course_mask_low(3), 4));
    gdx_course_edit_input_request_selection(1, 0);
    gdx_course_edit_input_clear();
    assert(!gdx_course_edit_input_take_selection(&point, &toggle));
}

static void test_bank_snap(void) {
    // Off leaves the bank alone.
    assert(gdx_course_edit_bank_snap_next(7, 0, 1) == 7);
    assert(gdx_course_edit_bank_snap_nearest(7, 0) == 7);

    // Absolute angles: 7 steps to 45, not 52.
    assert(gdx_course_edit_bank_snap_next(7, 45, 1) == 45);
    assert(gdx_course_edit_bank_snap_next(7, 45, -1) == 0);
    assert(gdx_course_edit_bank_snap_next(45, 45, 1) == 90);
    assert(gdx_course_edit_bank_snap_next(45, 45, -1) == 0);

    // Both directions wrap at 360.
    assert(gdx_course_edit_bank_snap_next(0, 45, -1) == 315);
    assert(gdx_course_edit_bank_snap_next(315, 45, 1) == 0);
    assert(gdx_course_edit_bank_snap_next(357, 90, 1) == 0);
    assert(gdx_course_edit_bank_snap_next(0, 180, -1) == 180);
    assert(gdx_course_edit_bank_snap_next(180, 180, 1) == 0);

    // Nearest rounds up at the halfway point and wraps.
    assert(gdx_course_edit_bank_snap_nearest(7, 45) == 0);
    assert(gdx_course_edit_bank_snap_nearest(21, 45) == 0);
    assert(gdx_course_edit_bank_snap_nearest(24, 45) == 45);
    assert(gdx_course_edit_bank_snap_nearest(350, 45) == 0);
    assert(gdx_course_edit_bank_snap_nearest(45, 90) == 90);
    assert(gdx_course_edit_bank_snap_nearest(89, 180) == 0);
    assert(gdx_course_edit_bank_snap_nearest(90, 180) == 180);
    assert(gdx_course_edit_bank_snap_nearest(269, 180) == 180);
    assert(gdx_course_edit_bank_snap_nearest(270, 180) == 0);
}

static void test_bank_value_entry(void) {
    GdxCourseEditInt32 typed = 123;
    GdxCourseEditInt32 bank = 0;
    GdxCourseEditInt32 i;

    // Keys do nothing until the typing starts, and each start is empty.
    assert(!gdx_course_edit_bank_entry_active());
    gdx_course_edit_bank_entry_push_digit(4);
    gdx_course_edit_bank_entry_begin();
    assert(gdx_course_edit_bank_entry_active());
    assert(strcmp(gdx_course_edit_bank_entry_text(), "") == 0);

    // Enter with nothing typed, or only a sign, applies nothing and ends the typing.
    assert(!gdx_course_edit_bank_entry_commit(&typed));
    assert(typed == 123 && !gdx_course_edit_bank_entry_active());
    gdx_course_edit_bank_entry_begin();
    gdx_course_edit_bank_entry_toggle_sign();
    assert(!gdx_course_edit_bank_entry_commit(&typed));
    assert(typed == 123);

    // The sign flips at any point, and the digits stay in order.
    gdx_course_edit_bank_entry_begin();
    gdx_course_edit_bank_entry_push_digit(4);
    gdx_course_edit_bank_entry_toggle_sign();
    gdx_course_edit_bank_entry_push_digit(5);
    assert(strcmp(gdx_course_edit_bank_entry_text(), "-45") == 0);
    assert(gdx_course_edit_bank_entry_commit(&typed));
    assert(typed == -45 && !gdx_course_edit_bank_entry_active());

    gdx_course_edit_bank_entry_begin();
    gdx_course_edit_bank_entry_toggle_sign();
    gdx_course_edit_bank_entry_push_digit(9);
    gdx_course_edit_bank_entry_toggle_sign();
    assert(strcmp(gdx_course_edit_bank_entry_text(), "9") == 0);

    // Backspace removes the last thing typed, stops at empty, and takes a lone sign too.
    gdx_course_edit_bank_entry_backspace();
    gdx_course_edit_bank_entry_backspace();
    assert(strcmp(gdx_course_edit_bank_entry_text(), "") == 0);
    gdx_course_edit_bank_entry_toggle_sign();
    gdx_course_edit_bank_entry_backspace();
    assert(strcmp(gdx_course_edit_bank_entry_text(), "") == 0);

    // Four digits at most, with or without a sign. The sign still fits after them.
    for (i = 1; i <= 6; i++) {
        gdx_course_edit_bank_entry_push_digit(i);
    }
    assert(strcmp(gdx_course_edit_bank_entry_text(), "1234") == 0);
    gdx_course_edit_bank_entry_toggle_sign();
    assert(strcmp(gdx_course_edit_bank_entry_text(), "-1234") == 0);
    gdx_course_edit_bank_entry_push_digit(5);
    assert(strcmp(gdx_course_edit_bank_entry_text(), "-1234") == 0);

    // Cancel and clear both end the typing without applying anything.
    gdx_course_edit_bank_entry_cancel();
    assert(!gdx_course_edit_bank_entry_active());
    assert(!gdx_course_edit_input_take_bank_value(&bank));
    gdx_course_edit_bank_entry_begin();
    gdx_course_edit_input_request_bank_value(90);
    gdx_course_edit_input_clear();
    assert(!gdx_course_edit_bank_entry_active());
    assert(!gdx_course_edit_input_take_bank_value(&bank));

    // The request is one-shot.
    gdx_course_edit_input_request_bank_value(45);
    assert(gdx_course_edit_input_take_bank_value(&bank));
    assert(bank == 45);
    assert(!gdx_course_edit_input_take_bank_value(&bank));

    // Typed numbers wrap to 0 to 359, then land on the nearest 3 degrees.
    assert(gdx_course_edit_bank_value_normalize(0) == 0);
    assert(gdx_course_edit_bank_value_normalize(45) == 45);
    assert(gdx_course_edit_bank_value_normalize(50) == 51);
    assert(gdx_course_edit_bank_value_normalize(46) == 45);
    assert(gdx_course_edit_bank_value_normalize(-45) == 315);
    assert(gdx_course_edit_bank_value_normalize(360) == 0);
    assert(gdx_course_edit_bank_value_normalize(720) == 0);
    assert(gdx_course_edit_bank_value_normalize(359) == 0);
    assert(gdx_course_edit_bank_value_normalize(358) == 357);
    assert(gdx_course_edit_bank_value_normalize(-9999) == 81);
}

static void test_extra_requests(void) {
    GdxCourseEditExtraRequest request;
    GdxCourseEditExtraRequest taken;

    gdx_course_edit_input_clear();
    assert(!gdx_course_edit_input_take_extra(&taken));

    request.is_track = 0;
    request.point = 3;
    request.which = 1;
    request.value = 10;
    request.begin_edit = 1;
    gdx_course_edit_input_request_extra(&request);
    // A slider moves many times a frame apart: the newest value wins and the drag still opens its undo step.
    request.value = 20;
    request.begin_edit = 0;
    gdx_course_edit_input_request_extra(&request);
    assert(gdx_course_edit_input_take_extra(&taken));
    assert(taken.point == 3);
    assert(taken.which == 1);
    assert(taken.value == 20);
    assert(taken.begin_edit == 1);
    assert(!gdx_course_edit_input_take_extra(&taken));

    // The next change of the same drag opens nothing.
    gdx_course_edit_input_request_extra(&request);
    assert(gdx_course_edit_input_take_extra(&taken));
    assert(taken.begin_edit == 0);

    gdx_course_edit_input_request_extra(&request);
    gdx_course_edit_input_clear();
    assert(!gdx_course_edit_input_take_extra(&taken));
}

static void test_absolute_scalar_values(void) {
    float left = 200.0f;
    float right = 300.0f;
    int32_t option;
    int32_t value;

    assert(gdx_course_edit_scalar_value_apply(2, 70, &left, &right));
    assert(left == 300.0f && right == 400.0f);
    assert(gdx_course_edit_scalar_value_apply(4, -10, &left, &right));
    assert(left == 250.0f && right == 450.0f);
    assert(gdx_course_edit_scalar_value_apply(4, 9999, &left, &right));
    assert(left == 650.0f && right == 50.0f);
    assert(gdx_course_edit_scalar_value_apply(2, 10, &left, &right));
    assert(left == 650.0f && right == 50.0f);
    assert(!gdx_course_edit_scalar_value_apply(0, 100, &left, &right));
    assert(!gdx_course_edit_scalar_value_apply(2, 100, 0, &right));
    assert(left == 650.0f && right == 50.0f);
    gdx_course_edit_input_request_scalar_value(2, 70);
    gdx_course_edit_input_request_scalar_value(4, -10);
    assert(gdx_course_edit_input_take_scalar_value(&option, &value));
    assert(option == 4 && value == -10);
    assert(!gdx_course_edit_input_take_scalar_value(&option, &value));
    gdx_course_edit_input_request_scalar_value(2, 70);
    gdx_course_edit_input_clear();
    assert(!gdx_course_edit_input_take_scalar_value(&option, &value));
    left = 1000.0f;
    right = 1000.0f;
    assert(gdx_course_edit_scalar_value_apply(4, INT_MIN, &left, &right));
    assert(left == 50.0f && right == 1950.0f);
    assert(gdx_course_edit_scalar_value_apply(4, INT_MAX, &left, &right));
    assert(left == 1950.0f && right == 50.0f);
    assert(gdx_course_edit_scalar_value_apply(2, INT_MAX, &left, &right));
    assert(left + right == 2000.0f);
    left = 50.0f;
    right = 1940.0f;
    assert(gdx_course_edit_scalar_value_apply(2, 200, &left, &right));
    assert(left >= 50.0f && right >= 50.0f && left + right <= 2000.0f);
    assert(right - left == 1890.0f);
    left = 55.0f;
    right = 1945.0f;
    assert(gdx_course_edit_scalar_value_apply(2, INT_MAX, &left, &right));
    assert(left >= 50.0f && right >= 50.0f && left + right <= 2000.0f);
    left = NAN;
    assert(!gdx_course_edit_scalar_value_apply(2, 100, &left, &right));
    left = INFINITY;
    assert(!gdx_course_edit_scalar_value_apply(4, 0, &left, &right));
}

static void test_path_requests(void) {
    int32_t path = -1;

    gdx_course_edit_input_clear();
    assert(!gdx_course_edit_input_take_path(&path));
    gdx_course_edit_input_request_path(1);
    gdx_course_edit_input_request_path(2);
    gdx_course_edit_input_request_path(3);
    assert(gdx_course_edit_input_take_path(&path) && path == 2);
    assert(!gdx_course_edit_input_take_path(&path));
    gdx_course_edit_input_request_path(1);
    gdx_course_edit_input_clear();
    assert(!gdx_course_edit_input_take_path(&path));
}

static void test_tool_clear_preserves_panel_intents(void) {
    GdxCourseEditExtraRequest request = {0};
    GdxCourseEditExtraRequest taken;
    GdxCourseEditInt32 value;
    GdxCourseEditInt32 option;

    gdx_course_edit_input_clear();
    request.point = 3;
    request.which = 1;
    request.value = 20;
    request.begin_edit = 1;
    gdx_course_edit_input_request_extra(&request);
    gdx_course_edit_input_request_path(2);
    gdx_course_edit_input_request_drag_start(3);
    gdx_course_edit_input_request_scalar_value(2, 50);
    gdx_course_edit_input_request_bank_value(30);
    gdx_course_edit_bank_entry_begin();
    gdx_course_edit_input_clear_tools();
    assert(!gdx_course_edit_input_take_drag_start(&value));
    assert(!gdx_course_edit_input_take_scalar_value(&option, &value));
    assert(!gdx_course_edit_input_take_bank_value(&value));
    assert(!gdx_course_edit_bank_entry_active());
    assert(gdx_course_edit_input_take_path(&value) && value == 2);
    assert(gdx_course_edit_input_take_extra(&taken) && taken.point == 3 && taken.value == 20);
    gdx_course_edit_input_request_path(1);
    gdx_course_edit_input_request_extra(&request);
    gdx_course_edit_input_clear();
    assert(!gdx_course_edit_input_take_path(&value));
    assert(!gdx_course_edit_input_take_extra(&taken));
}

static void test_wide_selection_masks(void) {
    GdxCourseEditMask mask = gdx_course_mask_low(0);
    GdxCourseEditMask taken;
    const int points[] = { 0, 63, 64, 100, 126 };
    int i;

    for (i = 0; i < 5; i++) {
        mask = gdx_course_mask_union(mask, gdx_course_mask_bit(points[i]));
    }
    for (i = 0; i < 127; i++) {
        assert(gdx_course_mask_test(mask, i) == (i == 0 || i == 63 || i == 64 || i == 100 || i == 126));
    }
    assert(gdx_course_mask_empty(gdx_course_mask_bit(-1)));
    assert(gdx_course_mask_empty(gdx_course_mask_bit(127)));
    assert(!gdx_course_mask_test(gdx_course_mask_all(), 127));
    gdx_course_edit_input_clear();
    gdx_course_edit_input_request_delete(mask, 5);
    assert(gdx_course_edit_input_take_delete(mask, 5));
    assert(!gdx_course_edit_input_take_delete(mask, 5));
    gdx_course_edit_input_request_delete(mask, 5);
    assert(!gdx_course_edit_input_take_delete(gdx_course_mask_low(mask.words[0]), 5));
    gdx_course_edit_input_request_selection_add_mask(mask);
    gdx_course_edit_input_request_selection_add_mask(gdx_course_mask_bit(65));
    assert(gdx_course_edit_input_take_selection_add_mask(&taken));
    assert(gdx_course_mask_test(taken, 65) && gdx_course_mask_test(taken, 126));
    gdx_course_edit_input_request_selection_add_mask(mask);
    gdx_course_edit_input_clear();
    assert(!gdx_course_edit_input_take_selection_add_mask(&taken));
}

int main(void) {
    test_wide_selection_masks();
    test_tool_clear_preserves_panel_intents();
    test_absolute_scalar_values();
    test_path_requests();
    test_extra_requests();
    test_bank_snap();
    test_bank_value_entry();
    test_one_shot_actions();
    test_latest_continuous_command_wins();
    test_clear_drops_stale_commands();
    test_delete_selection_validation();
    test_test_run_escape_edges();
    test_menu_bounds();
    test_drag_cancellation_keeps_other_intents();
    return 0;
}
