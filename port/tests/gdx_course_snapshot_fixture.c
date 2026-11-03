#include "global.h"
#include "fzx_course.h"
#include "gdx_course_snapshot_native.h"
#include "gdx_course_disk.h"

void* GdxSnapshotTest_Create(int count) {
    static CourseContext context;
    int i;
    memset(&context, 0, sizeof(context));
    context.courseData.creatorId = CREATOR_NINTENDO;
    context.courseData.controlPointCount = count;
    context.courseData.flag = 1;
    for (i = 0; i < count; ++i) {
        context.courseData.controlPoint[i].pos.x = (f32)(i * 2);
        context.courseData.controlPoint[i].pos.y = (f32)(i * 3);
        context.courseData.controlPoint[i].pos.z = (f32)(-i);
        context.courseData.controlPoint[i].radiusLeft = 100 + i;
        context.courseData.controlPoint[i].radiusRight = 200 - i;
        context.courseData.bankAngle[i] = i;
        context.courseData.pit[i] = -1;
        context.courseData.dash[i] = -1;
        context.courseData.dirt[i] = -1;
        context.courseData.ice[i] = -1;
        context.courseData.jump[i] = -1;
        context.courseData.landmine[i] = -1;
        context.courseData.gate[i] = -1;
        context.courseData.building[i] = -1;
        context.courseData.sign[i] = -1;
    }
    context.courseData.checksum = gdx_course_data_checksum(&context.courseData);
    ((u8*)context.ghostSave)[77] = 19;
    return &context;
}

float gdx_course_edit_max_y(void) {
    return 5000.0f;
}

s32 func_i2_800BE9D4(f32* value) {
    u32 bits;
    memcpy(&bits, value, sizeof(bits));
    return (bits & 0x7F800000u) == 0x7F800000u;
}

CourseContext gCourseCtx;

int GdxSnapshotTest_InvalidValues(int which, int index) {
    CourseContext* runtime = GdxSnapshotTest_Create(127);
    static CourseContextDisk base;
    static CourseContext restored;
    GdxBranchEditPoint tail[63];
    CourseData first;
    int result = 0;
    int i;
    switch (which) {
        case 0: runtime->courseData.sign[index] = 127; break;
        case 1: runtime->courseData.controlPoint[index].trackSegmentInfo = 62; break;
        case 2: runtime->courseData.controlPoint[index].radiusLeft = -1; break;
        case 3: runtime->courseData.venue = 127; break;
        case 4: runtime->courseData.skybox = 127; break;
    }
    runtime->courseData.checksum = gdx_course_data_checksum(&runtime->courseData);
    if (!gdx_course_snapshot_capture(runtime, &base, tail)) {
        result |= 1;
    }
    first = runtime->courseData;
    first.controlPointCount = 64;
    gdx_course_to_disk(&base.courseData, &first);
    for (i = 64; i < 127; ++i) {
        GdxBranchEditPoint* p = &tail[i - 64];
        memset(p, 0, sizeof(*p));
        memcpy(p->pos, &runtime->courseData.controlPoint[i].pos, sizeof(p->pos));
        p->radiusLeft = runtime->courseData.controlPoint[i].radiusLeft;
        p->radiusRight = runtime->courseData.controlPoint[i].radiusRight;
        p->trackSegmentInfo = runtime->courseData.controlPoint[i].trackSegmentInfo;
        p->bankAngle = runtime->courseData.bankAngle[i];
        memset(p->parts, -1, sizeof(p->parts));
        p->parts[8] = runtime->courseData.sign[i];
    }
    memset(&restored, 0x5A, sizeof(restored));
    if (!gdx_course_snapshot_expand(&restored, &base, tail, 127, runtime->courseData.checksum)) {
        for (i = 0; i < sizeof(restored); ++i) {
            if (((unsigned char*)&restored)[i] != 0x5A) {
                return -1;
            }
        }
        result |= 2;
    }
    return result;
}
