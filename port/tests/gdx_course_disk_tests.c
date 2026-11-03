#include "global.h"
#include "fzx_course.h"
#include "gdx_course_disk.h"
#include "gdx_course_snapshot_native.h"
extern int printf(const char*, ...);
extern void exit(int);
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); exit(1); } } while (0)

CourseContext gCourseCtx;

f32 gdx_course_edit_max_y(void) {
    return 5000.0f;
}

s32 func_i2_800BE9D4(f32* value) {
    union { f32 number; u32 bits; } input;
    input.number = *value;
    return (input.bits & 0x7F800000u) == 0x7F800000u;
}

static void checksum_round_trips(void) {
    static CourseDataDisk disk;
    s32 expected;
    int count;
    int i;

    for (count = 0; count <= 64; count++) {
        memset(&gCourseCtx, 0, sizeof(gCourseCtx));
        gCourseCtx.courseData.controlPointCount = count;
        for (i = 0; i < count; i++) {
            ControlPoint* point = &gCourseCtx.courseData.controlPoint[i];
            point->pos.x = (f32)(i * 17 - 500);
            point->pos.y = (f32)(i * 3);
            point->pos.z = (f32)(500 - i * 13);
            point->radiusLeft = 200 + i;
            point->radiusRight = 300 - i;
            point->trackSegmentInfo = TRACK_SHAPE_ROAD | (i % 4);
            gCourseCtx.courseData.bankAngle[i] = (s16)(i * 3);
            gCourseCtx.courseData.pit[i] = (s8)(i % 4 - 1);
            gCourseCtx.courseData.dash[i] = (s8)(i % 3 - 1);
            gCourseCtx.courseData.dirt[i] = (s8)(i % 4 - 1);
            gCourseCtx.courseData.ice[i] = (s8)(i % 4 - 1);
            gCourseCtx.courseData.jump[i] = (s8)(i % 3 - 1);
            gCourseCtx.courseData.landmine[i] = (s8)(i % 3 - 1);
            gCourseCtx.courseData.gate[i] = (s8)(i % 3 - 1);
            gCourseCtx.courseData.building[i] = (s8)(i % 4 - 1);
            gCourseCtx.courseData.sign[i] = (s8)(i % 4 - 1);
        }
        expected = Course_CalculateChecksum();
        CHECK(expected != -1);
        CHECK(gdx_course_to_disk(&disk, &gCourseCtx.courseData));
        memset(&gCourseCtx, 0, sizeof(gCourseCtx));
        CHECK(gdx_course_from_disk(&gCourseCtx.courseData, &disk));
        CHECK(Course_CalculateChecksum() == expected);
    }
}

static void snapshot_round_trips(void) {
    static CourseContext source;
    static CourseContext restored;
    static CourseContext before;
    static CourseContextDisk disk;
    static GdxBranchEditPoint tail[63];
    int count;
    int i;
    for (count = 0; count <= 127; ++count) {
        memset(&source, 0, sizeof(source));
        memset(tail, 0, sizeof(tail));
        source.courseData.creatorId = CREATOR_NINTENDO;
        source.courseData.controlPointCount = count;
        source.courseData.flag = 1;
        for (i = 0; i < count; ++i) {
            source.courseData.controlPoint[i].pos.x = (f32)(i * 3);
            source.courseData.controlPoint[i].pos.y = (f32)(i * 2);
            source.courseData.controlPoint[i].pos.z = (f32)(i * -2);
            source.courseData.controlPoint[i].radiusLeft = 100 + i;
            source.courseData.controlPoint[i].radiusRight = 200 - i;
            source.courseData.bankAngle[i] = i;
            source.courseData.pit[i] = i % 4 - 1;
            source.courseData.dash[i] = i % 3 - 1;
            source.courseData.dirt[i] = i % 4 - 1;
            source.courseData.ice[i] = i % 4 - 1;
            source.courseData.jump[i] = i % 3 - 1;
            source.courseData.landmine[i] = i % 3 - 1;
            source.courseData.gate[i] = i % 3 - 1;
            source.courseData.building[i] = i % 4 - 1;
            source.courseData.sign[i] = i % 4 - 1;
        }
        source.courseData.checksum = gdx_course_data_checksum(&source.courseData);
        for (i = 0; i < count; ++i) {
            source.courseData.controlPoint[i].trackSegmentInfo |= TRACK_FLAG_CONTINUOUS;
        }
        ((unsigned char*) source.ghostSave)[0] = 77;
        CHECK(gdx_course_snapshot_capture(&source, &disk, tail));
        CHECK(disk.courseData.controlPointCount == (count > 64 ? 64 : count));
        disk.courseData.flag |= 0x80;
        disk.courseData.checksum = 12345;
        CHECK(gdx_course_snapshot_expand(&restored, &disk, tail, count, source.courseData.checksum));
        CHECK(memcmp(&restored, &source, sizeof(source)) == 0);
        before = restored;
        CHECK(!gdx_course_snapshot_expand(&restored, &disk, tail, count, source.courseData.checksum + 1));
        CHECK(memcmp(&restored, &before, sizeof(before)) == 0);
        disk.courseData.creatorId ^= 1;
        CHECK(!gdx_course_snapshot_expand(&restored, &disk, tail, count, source.courseData.checksum));
        CHECK(memcmp(&restored, &before, sizeof(before)) == 0);
        disk.courseData.creatorId ^= 1;
        disk.courseData.bgm = 14;
        CHECK(!gdx_course_snapshot_expand(&restored, &disk, tail, count, source.courseData.checksum));
        CHECK(memcmp(&restored, &before, sizeof(before)) == 0);
        disk.courseData.bgm = 0;
        if (count > 64) {
            tail[0].bankAngle++;
            CHECK(!gdx_course_snapshot_expand(&restored, &disk, tail, count, source.courseData.checksum));
            CHECK(memcmp(&restored, &before, sizeof(before)) == 0);
        }
    }
}

int main(void) {
    static CourseContextDisk disk;
    static CourseContextDisk result;
    static CourseContext runtime;
    unsigned char* bytes = (unsigned char*) &disk;
    unsigned int i;
    int count;

    CHECK(sizeof(CourseDataDisk) == 0x7E0);
    CHECK(sizeof(CourseContextDisk) == 0xC830);
    CHECK((unsigned char*) disk.ghostSave - bytes == 0x7E0);
    CHECK((unsigned char*) &disk.saveCourseRecord - bytes == 0xC720);
    for (i = 0; i < sizeof(disk); i++) {
        bytes[i] = (unsigned char) (i * 37 + 19);
    }
    for (count = 0; count <= 64; count++) {
        disk.courseData.controlPointCount = count;
        CHECK(gdx_course_context_from_disk(&runtime, &disk));
        CHECK(gdx_course_context_to_disk(&result, &runtime));
        CHECK(memcmp(&disk, &result, sizeof(disk)) == 0);
        CHECK(runtime.courseData.checksum == disk.courseData.checksum);
    }
    memset(&runtime, 0x5A, sizeof(runtime));
    disk.courseData.controlPointCount = -1;
    CHECK(!gdx_course_context_from_disk(&runtime, &disk));
    CHECK(((unsigned char*) &runtime)[0] == 0x5A);
    disk.courseData.controlPointCount = 65;
    CHECK(!gdx_course_context_from_disk(&runtime, &disk));
    CHECK(!gdx_course_from_disk(NULL, &disk.courseData));
    CHECK(!gdx_course_to_disk(&result.courseData, NULL));
    CHECK(gdx_course_context_file("CRSD", sizeof(CourseContext)));
    CHECK(gdx_course_context_file("GOST", sizeof(CourseContext)));
    CHECK(gdx_course_context_file("CRSE", sizeof(CourseContext)));
    CHECK(!gdx_course_context_file("CARD", sizeof(CourseContext)));
    CHECK(!gdx_course_context_file("CRSD", sizeof(CourseContextDisk)));
    CHECK(!gdx_course_context_file("CRSD", 0));
    CHECK(gdx_course_data_checksum(NULL) == -1);
    checksum_round_trips();
    snapshot_round_trips();
    printf("All fixed disk layout and native checksum tests passed\n");
    return 0;
}
