#include "global.h"
#include "unk_structs.h"
#include "gdx_course_edit_input.h"

// Use the game's types so a host-side layout guess cannot make both code and fixture pass.
unk_807B3C20 D_802CB6D0;
unk_800D6CA0 D_800D6CA0;
unk_80128690 D_80128690[64];
s32 D_xk2_80119918;

static int expect_int(long actual, long expected) {
    return actual == expected;
}

static int expect_float(float actual, float expected) {
    return actual == expected;
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

int main(void) {
    return !(test_count_bounds() && test_context_and_selection_accessors());
}
