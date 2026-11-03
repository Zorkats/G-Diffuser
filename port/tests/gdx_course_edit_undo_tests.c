#include "global.h"
#include "fzx_expansion_kit.h"
#include "fzx_course.h"
#include "gdx_course_branch_edit.h"
#include "gdx_course_edit_undo.h"

extern int printf(const char*, ...);
extern void exit(int);
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); exit(1); } } while (0)

unk_800D6CA0 D_800D6CA0;
unk_80128690 D_80128690[GDX_COURSE_POINT_CAPACITY];
unk_80128690 D_xk2_80128990[GDX_COURSE_POINT_CAPACITY];
unk_807B3C20 D_802CB6D0;
unk_807B3C20 D_807B6528;
u8 D_80030060[GDX_EK_FILE_NAME_MAX + 1];
u8 D_8003006C[GDX_EK_FILE_NAME_MAX + 1];
CourseData D_8010CF50;
CourseContext gCourseCtx;
s32 D_xk2_80119800;
s32 D_xk2_80104BC0;
s32 gCreateOption = CREATE_OPTION_POINT;
s32 gMoveOption;
s32 gPointOption;
s32 gDesignStyleOption;
s32 gPartsStyleOption;
bool gInCourseEditTestRun;

void Audio_TriggerSystemSE(u8 sound) { (void)sound; }
int gdx_course_edit_native_mouse_owned(void) { return 0; }

// The fixture excludes venue reload and rendering, but uses the native backup copy contract.
void func_xk2_800EF8B0(void) {
    if (!gdx_course_edit_undo_begin_restore()) {
        return;
    }
    memcpy(D_80128690, D_xk2_80128990, sizeof(D_80128690));
    memcpy(D_80030060, D_8003006C, sizeof(D_80030060));
    D_802CB6D0 = D_807B6528;
    gCourseCtx.courseData = D_8010CF50;
    D_800D6CA0.unk_0C = D_xk2_80119800;
    gdx_branch_edit_restore();
    gdx_course_edit_undo_end_restore();
}

static void backup(void) {
    memcpy(D_xk2_80128990, D_80128690, sizeof(D_80128690));
    memcpy(D_8003006C, D_80030060, sizeof(D_80030060));
    D_807B6528 = D_802CB6D0;
    D_8010CF50 = gCourseCtx.courseData;
    D_xk2_80119800 = D_800D6CA0.unk_0C;
    gdx_branch_edit_backup();
    gdx_course_edit_undo_push();
}

static void value(int n) {
    D_802CB6D0.controlPointCount = 127;
    D_802CB6D0.unk_0000[126].pos.x = (float)n;
    gCourseCtx.courseData.controlPointCount = 127;
    gCourseCtx.courseData.controlPoint[126].pos.x = (float)n;
    gCourseCtx.courseData.bankAngle[126] = (s16)n;
    gCourseCtx.courseData.sign[126] = (s8)(n % 5);
    D_80128690[126].unk_00 = n;
    D_800D6CA0.unk_0C = 126;
    gdx_branch_edit_set_point_extra(126, GDX_BRANCH_EDIT_EXTRA_MAGNET_RANGE, n + 30);
}

static void expect(int n) {
    CHECK(D_802CB6D0.controlPointCount == 127);
    CHECK(D_802CB6D0.unk_0000[126].pos.x == (float)n);
    CHECK(gCourseCtx.courseData.controlPointCount == 127);
    CHECK(gCourseCtx.courseData.controlPoint[126].pos.x == (float)n);
    CHECK(gCourseCtx.courseData.bankAngle[126] == n);
    CHECK(gCourseCtx.courseData.sign[126] == n % 5);
    CHECK(D_80128690[126].unk_00 == n);
    CHECK(D_800D6CA0.unk_0C == 126);
    CHECK(gdx_branch_edit_point_extra(126, GDX_BRANCH_EDIT_EXTRA_MAGNET_RANGE) == n + 30);
}

int main(void) {
    int n;
    gdx_branch_edit_select(NULL, 127);
    gdx_course_edit_undo_clear();
    value(1);
    backup();
    CHECK(gdx_course_edit_undo_row_points(0) == 127);
    value(2);
    gdx_course_edit_undo_request_undo();
    gdx_course_edit_undo_tick();
    expect(1);
    gdx_course_edit_undo_request_redo();
    gdx_course_edit_undo_tick();
    expect(2);

    gdx_course_edit_undo_clear();
    for (n = 1; n <= 40; ++n) {
        value(n);
        backup();
    }
    value(41);
    CHECK(gdx_course_edit_undo_undo_count() == 32);
    for (n = 40; n >= 9; --n) {
        gdx_course_edit_undo_request_undo();
        gdx_course_edit_undo_tick();
        expect(n);
    }
    CHECK(gdx_course_edit_undo_undo_count() == 0);
    for (n = 10; n <= 41; ++n) {
        gdx_course_edit_undo_request_redo();
        gdx_course_edit_undo_tick();
        expect(n);
    }

    gInCourseEditTestRun = true;
    backup();
    value(42);
    backup();
    func_xk2_800EF8B0();
    expect(41);
    CHECK(gdx_course_edit_undo_redo_count() == 0);
    gInCourseEditTestRun = false;
    printf("All extended undo tests passed\n");
    return 0;
}
