#include "gdx_course_edit_input.h"
#include "gdx_course_edit_menu.h"

#include <assert.h>

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

    gdx_course_edit_input_request_delete(3, 4);
    assert(gdx_course_edit_input_take_delete(3, 4) == 1);
    assert(gdx_course_edit_input_take_delete(3, 4) == 0);
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
    gdx_course_edit_input_request_delete(3, 4);
    gdx_course_edit_input_request_move_target(0, (GdxCourseEditVec3){ 1.0f, 2.0f, 3.0f });
    gdx_course_edit_input_request_scalar_target(2, 20);
    gdx_course_edit_input_clear();

    assert(gdx_course_edit_input_take_drag_start(0) == 0);
    assert(gdx_course_edit_input_take_drag_commit() == 0);
    assert(gdx_course_edit_input_take_drag_cancel() == 0);
    assert(gdx_course_edit_input_take_delete(3, 4) == 0);
    assert(gdx_course_edit_input_take_move_target(0, 0) == 0);
    assert(gdx_course_edit_input_take_scalar_target(0, 0) == 0);
}

static void test_delete_selection_validation(void) {
    gdx_course_edit_input_request_delete(3, 4);
    assert(!gdx_course_edit_input_take_delete(1, 4));
    assert(!gdx_course_edit_input_take_delete(3, 4));
    gdx_course_edit_input_request_delete(3, 4);
    assert(!gdx_course_edit_input_take_delete(3, 5));
    gdx_course_edit_input_request_delete(0, 4);
    assert(!gdx_course_edit_input_take_delete(0, 4));
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
    gdx_course_edit_input_request_delete(3, 4);
    gdx_course_edit_input_request_drag_start(1);
    assert(gdx_course_edit_input_drag_start_pending());
    gdx_course_edit_input_clear_drag();
    assert(!gdx_course_edit_input_drag_start_pending());
    assert(gdx_course_edit_input_take_selection(&point, &toggle));
    assert(point == 63 && toggle == 1);
    assert(!gdx_course_edit_input_take_selection(&point, &toggle));
    assert(gdx_course_edit_input_take_delete(3, 4));
    gdx_course_edit_input_request_selection(1, 0);
    gdx_course_edit_input_clear();
    assert(!gdx_course_edit_input_take_selection(&point, &toggle));
}

int main(void) {
    test_one_shot_actions();
    test_latest_continuous_command_wins();
    test_clear_drops_stale_commands();
    test_delete_selection_validation();
    test_test_run_escape_edges();
    test_menu_bounds();
    test_drag_cancellation_keeps_other_intents();
    return 0;
}
