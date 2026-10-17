#include "global.h"
#include "unk_structs.h"
#include "gdx_course_edit_input.h"

// Use the game's types so a host-side layout guess cannot make both code and fixture pass.
unk_807B3C20 D_802CB6D0;
unk_800D6CA0 D_800D6CA0;
unk_80128690 D_80128690[64];
s32 D_xk2_80119918;

// Stand-ins for the decomp side of the line-up and for the input queue. This test links native.c only.
static s32 sUndoBackups;
static s32 sNativeLineUps;
static s32 sBackupBeforeLineUp;
static u8 sLastSfx;
static s32 sLineUpPending;

void func_xk2_800EF78C(void) {
    sBackupBeforeLineUp = (sNativeLineUps == 0);
    sUndoBackups++;
}

void func_xk2_800EFCD0(void) {
    sNativeLineUps++;
}

void gdx_course_edit_undo_set_pending_label(int label) {
    (void) label;
}

void Audio_TriggerSystemSE(u8 sfxId) {
    sLastSfx = sfxId;
}

GdxCourseEditInt32 gdx_course_edit_input_take_line_up(void) {
    GdxCourseEditInt32 pending = sLineUpPending;

    sLineUpPending = 0;
    return pending;
}

static int expect_int(long actual, long expected) {
    return actual == expected;
}

static int expect_float(float actual, float expected) {
    return actual == expected;
}

// Projected positions are not exact in float, so allow a little slack. Avoids libm, which native.c does not use.
static int expect_near(float actual, float expected) {
    float delta = actual - expected;

    return (delta < 0.01f) && (delta > -0.01f);
}

static void reset_fixture(s32 count) {
    s32 i;

    memset(&D_802CB6D0, 0, sizeof(D_802CB6D0));
    memset(D_80128690, 0, sizeof(D_80128690));
    D_802CB6D0.controlPointCount = count;
    for (i = 0; i < 64; i++) {
        D_802CB6D0.unk_0000[i].pos.x = (f32)(100 + i);
        D_802CB6D0.unk_0000[i].pos.y = (f32)(200 + i);
        D_802CB6D0.unk_0000[i].pos.z = (f32)(300 + i);
        D_80128690[i].unk_08 = (i == 0 || i == 1 || i == 63) ? 1 : 0;
    }
}

static void reset_line_up_stubs(void) {
    sUndoBackups = 0;
    sNativeLineUps = 0;
    sBackupBeforeLineUp = 0;
    sLastSfx = 0xFF;
    sLineUpPending = 0;
}

static void set_position(s32 index, float x, float y, float z) {
    D_802CB6D0.unk_0000[index].pos.x = x;
    D_802CB6D0.unk_0000[index].pos.y = y;
    D_802CB6D0.unk_0000[index].pos.z = z;
}

// Closes the first count segments into a ring in array order, with segmentIndex equal to the array index.
static void link_ring(s32 count) {
    s32 i;

    for (i = 0; i < count; i++) {
        D_802CB6D0.unk_0000[i].segmentIndex = i;
        D_802CB6D0.unk_0000[i].next = &D_802CB6D0.unk_0000[(i + 1) % count];
        D_802CB6D0.unk_0000[i].prev = &D_802CB6D0.unk_0000[(i + count - 1) % count];
    }
}

static void select_mask(GdxCourseEditMask mask) {
    s32 i;

    for (i = 0; i < 64; i++) {
        D_80128690[i].unk_08 = (((mask >> i) & 1) != 0) ? 1 : 0;
    }
}

static int test_count_bounds(void) {
    int passed = 1;

    reset_fixture(0);
    passed &= expect_int(gdx_course_edit_native_point_count(), 0);

    reset_fixture(4);
    passed &= expect_int(gdx_course_edit_native_point_count(), 4);

    reset_fixture(64);
    passed &= expect_int(gdx_course_edit_native_point_count(), 64);

    reset_fixture(65);
    passed &= expect_int(gdx_course_edit_native_point_count(), 64);

    reset_fixture(-1);
    passed &= expect_int(gdx_course_edit_native_point_count(), 0);
    return passed;
}

static int test_context_and_selection_accessors(void) {
    GdxCourseEditVec3 position;
    int passed = 1;

    reset_fixture(64);
    D_800D6CA0.unk_08 = 0x22;
    D_800D6CA0.unk_00 = 1;
    D_800D6CA0.unk_0C = 63;
    passed &= expect_int(gdx_course_edit_native_context_state(), 0x22);
    passed &= expect_int(gdx_course_edit_native_grab_state(), 1);
    passed &= expect_int(gdx_course_edit_native_active_point(), 63);
    passed &= expect_int(gdx_course_edit_native_point_selected(0), 1);
    passed &= expect_int(gdx_course_edit_native_point_selected(1), 1);
    passed &= expect_int(gdx_course_edit_native_point_selected(63), 1);
    passed &= expect_int(gdx_course_edit_native_point_selected(64), 0);
    passed &= expect_int(gdx_course_edit_native_point_selected(-1), 0);
    D_xk2_80119918 = 1;
    passed &= expect_int(gdx_course_edit_native_help_active(), 1);

    memset(&position, 0, sizeof(position));
    passed &= expect_int(gdx_course_edit_native_point_position(63, &position), 1);
    passed &= expect_float(position.x, 163.0f);
    passed &= expect_float(position.y, 263.0f);
    passed &= expect_float(position.z, 363.0f);
    passed &= expect_int(gdx_course_edit_native_point_position(0, &position), 1);
    passed &= expect_float(position.x, 100.0f);
    passed &= expect_int(gdx_course_edit_native_point_position(1, &position), 1);
    passed &= expect_float(position.x, 101.0f);
    passed &= expect_int(gdx_course_edit_native_point_position(64, &position), 0);
    passed &= expect_int(gdx_course_edit_native_point_position(0, (GdxCourseEditVec3*)0), 0);
    return passed;
}

static int test_selection_mask_round_trip(void) {
    int passed = 1;

    reset_fixture(5);
    gdx_course_edit_native_set_selection_mask(0x05);
    passed &= expect_int((long)gdx_course_edit_native_selection_mask(), 0x05);
    passed &= expect_int(gdx_course_edit_native_point_selected(0), 1);
    passed &= expect_int(gdx_course_edit_native_point_selected(1), 0);
    passed &= expect_int(gdx_course_edit_native_point_selected(2), 1);
    passed &= expect_int(gdx_course_edit_native_active_point(), 0);

    // Bits past the point count are dropped, and the stale flag past the count is cleared too.
    D_80128690[63].unk_08 = 1;
    gdx_course_edit_native_set_selection_mask(~(GdxCourseEditMask)0);
    passed &= expect_int((long)gdx_course_edit_native_selection_mask(), 0x1F);
    passed &= expect_int(D_80128690[63].unk_08, 0);

    // An empty mask clears the selection and leaves the active point alone.
    D_800D6CA0.unk_0C = 3;
    gdx_course_edit_native_set_selection_mask(0);
    passed &= expect_int((long)gdx_course_edit_native_selection_mask(), 0);
    passed &= expect_int(gdx_course_edit_native_active_point(), 3);
    return passed;
}

static int test_line_up_scan_needs_three_points_in_a_row(void) {
    GdxCourseEditLineUpPreview preview;
    int passed = 1;

    reset_fixture(6);
    link_ring(6);
    select_mask(0x03);
    passed &= expect_int(gdx_course_edit_native_line_up_scan(&preview), GDX_COURSE_EDIT_LINE_UP_TOO_FEW);
    passed &= expect_int(preview.count, 0);

    // Every point selected leaves no anchor, so there is nothing to line up.
    select_mask(0x3F);
    passed &= expect_int(gdx_course_edit_native_line_up_scan(&preview), GDX_COURSE_EDIT_LINE_UP_TOO_FEW);

    select_mask(0);
    passed &= expect_int(gdx_course_edit_native_line_up_scan(&preview), GDX_COURSE_EDIT_LINE_UP_TOO_FEW);

    reset_fixture(0);
    link_ring(0);
    passed &= expect_int(gdx_course_edit_native_line_up_scan(&preview), GDX_COURSE_EDIT_LINE_UP_TOO_FEW);
    return passed;
}

static int test_line_up_scan_takes_the_longest_run(void) {
    GdxCourseEditLineUpPreview preview;
    int passed = 1;
    s32 i;

    // Ring of 8 points spaced 1000 apart on the x axis. Runs 1-2 and 4-7 are separated by unselected 0 and 3.
    reset_fixture(8);
    link_ring(8);
    for (i = 0; i < 8; i++) {
        set_position(i, 1000.0f * (float)i, 0.0f, 0.0f);
    }
    select_mask(0xF6);

    passed &= expect_int(gdx_course_edit_native_line_up_scan(&preview), GDX_COURSE_EDIT_LINE_UP_OK);
    passed &= expect_int(preview.count, 4);
    passed &= expect_near(preview.start.x, 4000.0f);
    passed &= expect_near(preview.landing[1].x, 5000.0f);
    passed &= expect_near(preview.landing[2].x, 6000.0f);
    passed &= expect_near(preview.end.x, 7000.0f);

    // A NULL preview still reports the result.
    passed &= expect_int(gdx_course_edit_native_line_up_scan(0), GDX_COURSE_EDIT_LINE_UP_OK);
    return passed;
}

static int test_line_up_scan_takes_the_first_of_equal_runs(void) {
    GdxCourseEditLineUpPreview preview;
    int passed = 1;
    s32 i;

    // Two runs of 3, 0-2 and 4-6. The walk starts at the anchor 3, so 4-6 is met first and keeps the tie.
    reset_fixture(8);
    link_ring(8);
    for (i = 0; i < 8; i++) {
        set_position(i, 1000.0f * (float)i, 0.0f, 0.0f);
    }
    select_mask(0x77);

    passed &= expect_int(gdx_course_edit_native_line_up_scan(&preview), GDX_COURSE_EDIT_LINE_UP_OK);
    passed &= expect_int(preview.count, 3);
    passed &= expect_near(preview.start.x, 4000.0f);
    passed &= expect_near(preview.end.x, 6000.0f);
    return passed;
}

static int test_line_up_scan_projects_the_middle_point(void) {
    GdxCourseEditLineUpPreview preview;
    int passed = 1;

    // Run 0-1-2 with point 3 as the anchor. Point 1 sits 400 units off the line from 0 to 2.
    reset_fixture(4);
    link_ring(4);
    set_position(0, 0.0f, 0.0f, 0.0f);
    set_position(1, 1000.0f, 0.0f, 400.0f);
    set_position(2, 2000.0f, 0.0f, 0.0f);
    set_position(3, 500.0f, 0.0f, 900.0f);
    select_mask(0x07);

    passed &= expect_int(gdx_course_edit_native_line_up_scan(&preview), GDX_COURSE_EDIT_LINE_UP_OK);
    passed &= expect_int(preview.count, 3);
    passed &= expect_near(preview.landing[1].x, 1000.0f);
    passed &= expect_near(preview.landing[1].z, 0.0f);
    passed &= expect_near(preview.end.x, 2000.0f);
    return passed;
}

static int test_line_up_scan_refuses_points_under_498_apart(void) {
    GdxCourseEditLineUpPreview preview;
    int passed = 1;

    // The middle point lands 100 units from the start, which is under the 498 spacing limit.
    reset_fixture(4);
    link_ring(4);
    set_position(0, 0.0f, 0.0f, 0.0f);
    set_position(1, 100.0f, 0.0f, 300.0f);
    set_position(2, 1000.0f, 0.0f, 0.0f);
    set_position(3, 500.0f, 0.0f, 900.0f);
    select_mask(0x07);

    passed &= expect_int(gdx_course_edit_native_line_up_scan(&preview), GDX_COURSE_EDIT_LINE_UP_TOO_CLOSE);
    passed &= expect_int(preview.count, 0);
    return passed;
}

static int test_line_up_scan_refuses_a_run_with_no_line(void) {
    GdxCourseEditLineUpPreview preview;
    int passed = 1;

    // Start and end coincide, so the line has no direction to project onto.
    reset_fixture(4);
    link_ring(4);
    set_position(0, 0.0f, 0.0f, 0.0f);
    set_position(1, 1000.0f, 0.0f, 0.0f);
    set_position(2, 0.0f, 0.0f, 0.0f);
    set_position(3, 500.0f, 0.0f, 900.0f);
    select_mask(0x07);

    passed &= expect_int(gdx_course_edit_native_line_up_scan(&preview), GDX_COURSE_EDIT_LINE_UP_TOO_CLOSE);
    passed &= expect_int(preview.count, 0);
    return passed;
}

static int test_line_up_scan_handles_a_run_across_the_array_end(void) {
    GdxCourseEditLineUpPreview preview;
    int passed = 1;

    // Ring of 6 with the run 5, 0, 1. Point 2 is the first unselected point, so the walk starts there.
    reset_fixture(6);
    link_ring(6);
    set_position(5, 0.0f, 0.0f, 0.0f);
    set_position(0, 500.0f, 0.0f, 300.0f);
    set_position(1, 1000.0f, 0.0f, 0.0f);
    select_mask(0x23);

    passed &= expect_int(gdx_course_edit_native_line_up_scan(&preview), GDX_COURSE_EDIT_LINE_UP_OK);
    passed &= expect_int(preview.count, 3);
    passed &= expect_near(preview.start.x, 0.0f);
    passed &= expect_near(preview.landing[1].x, 500.0f);
    passed &= expect_near(preview.landing[1].z, 0.0f);
    passed &= expect_near(preview.end.x, 1000.0f);
    return passed;
}

static int test_apply_line_up_needs_a_request(void) {
    int passed = 1;

    reset_line_up_stubs();
    reset_fixture(4);
    link_ring(4);
    select_mask(0x07);
    gdx_course_edit_native_apply_line_up();
    passed &= expect_int(sUndoBackups, 0);
    passed &= expect_int(sNativeLineUps, 0);
    passed &= expect_int(sLastSfx, 0xFF);
    return passed;
}

static int test_apply_line_up_refusal_plays_the_sound_and_keeps_data(void) {
    int passed = 1;

    reset_line_up_stubs();
    reset_fixture(4);
    link_ring(4);
    select_mask(0x03);
    sLineUpPending = 1;
    gdx_course_edit_native_apply_line_up();
    passed &= expect_int(sUndoBackups, 0);
    passed &= expect_int(sNativeLineUps, 0);
    passed &= expect_int(sLastSfx, NA_SE_32);
    passed &= expect_int(sLineUpPending, 0);
    return passed;
}

static int test_apply_line_up_takes_the_backup_then_lines_up(void) {
    int passed = 1;
    s32 i;

    reset_line_up_stubs();
    reset_fixture(8);
    link_ring(8);
    for (i = 0; i < 8; i++) {
        set_position(i, 1000.0f * (float)i, 0.0f, 0.0f);
    }
    select_mask(0xF6);
    sLineUpPending = 1;
    gdx_course_edit_native_apply_line_up();
    passed &= expect_int(sUndoBackups, 1);
    passed &= expect_int(sBackupBeforeLineUp, 1);
    passed &= expect_int(sNativeLineUps, 1);
    passed &= expect_int(sLastSfx, 0xFF);
    passed &= expect_int(sLineUpPending, 0);
    return passed;
}

int main(void) {
    return !(test_count_bounds() && test_context_and_selection_accessors() && test_selection_mask_round_trip() &&
             test_line_up_scan_needs_three_points_in_a_row() && test_line_up_scan_takes_the_longest_run() &&
             test_line_up_scan_takes_the_first_of_equal_runs() && test_line_up_scan_projects_the_middle_point() &&
             test_line_up_scan_refuses_points_under_498_apart() && test_line_up_scan_refuses_a_run_with_no_line() &&
             test_line_up_scan_handles_a_run_across_the_array_end() && test_apply_line_up_needs_a_request() &&
             test_apply_line_up_refusal_plays_the_sound_and_keeps_data() &&
             test_apply_line_up_takes_the_backup_then_lines_up());
}
