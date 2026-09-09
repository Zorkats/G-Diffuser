#include "global.h"
#include "unk_structs.h"
#include "gdx_course_edit_input.h"

extern unk_807B3C20 D_802CB6D0;
extern unk_800D6CA0 D_800D6CA0;
extern unk_80128690 D_80128690[64];
extern s32 D_xk2_80119918;

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

s32 gdx_course_edit_native_point_position(s32 pointIndex, GdxCourseEditVec3* position) {
    if ((position == NULL) || (pointIndex < 0) || (pointIndex >= gdx_course_edit_native_point_count())) {
        return 0;
    }
    position->x = D_802CB6D0.unk_0000[pointIndex].pos.x;
    position->y = D_802CB6D0.unk_0000[pointIndex].pos.y;
    position->z = D_802CB6D0.unk_0000[pointIndex].pos.z;
    return 1;
}
