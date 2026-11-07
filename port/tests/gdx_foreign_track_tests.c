// Copyright (c) Zorkats
//
// gdx_foreign_track.c on its own, with no game symbols linked: the height limit is a parameter,
// the pit repair reports through its callback, and the stored checksum follows the repair.

#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../gdx_foreign_track.h"

static unsigned char sRaw[GDX_FOREIGN_TRACK_PAYLOAD_SIZE];
static unsigned char sHost[GDX_FOREIGN_TRACK_PAYLOAD_SIZE];
static int sLogged[4];
static int sLogCount;

static void record_pit(void* context, int index, int value) {
    assert(context == (void*) sLogged);
    if (sLogCount < 2) {
        sLogged[sLogCount * 2] = index;
        sLogged[sLogCount * 2 + 1] = value;
    }
    sLogCount++;
}

static GdxForeignCourseData make_course(float highestY) {
    GdxForeignCourseData course;
    int invalid;

    memset(&course, 0, sizeof(course));
    course.creatorId = GDX_FOREIGN_CREATOR_NINTENDO;
    course.controlPointCount = 3;
    course.controlPoint[0].pos.x = 1000.0f;
    course.controlPoint[1].pos.z = 500.0f;
    course.controlPoint[2].pos.y = highestY;
    course.bankAngle[2] = -20;
    course.checksum = gdx_foreign_track_checksum(&course, GDX_FOREIGN_TRACK_EXTENDED_MAX_Y, &invalid);
    assert(!invalid);
    return course;
}

static void height_limit_is_a_parameter(void) {
    GdxForeignCourseData course = make_course(6000.0f);
    GdxForeignCourseDiagnostic info;
    uint32_t errors;

    memset(sRaw, 0, sizeof(sRaw));
    memcpy(sRaw, &course, sizeof(course));
    errors = gdx_foreign_track_normalize(sRaw, sizeof(sRaw), GDX_FOREIGN_HOST_ORDER, sHost,
                                         GDX_FOREIGN_TRACK_STOCK_MAX_Y, &info, NULL, NULL);
    assert(errors == GDX_FOREIGN_TRACK_ERR_GEOMETRY && !info.checksumChecked);
    errors = gdx_foreign_track_normalize(sRaw, sizeof(sRaw), GDX_FOREIGN_HOST_ORDER, sHost,
                                         GDX_FOREIGN_TRACK_EXTENDED_MAX_Y, &info, NULL, NULL);
    assert(errors == 0 && info.checksumChecked && info.computedChecksum == course.checksum);
    assert(memcmp(sHost, sRaw, sizeof(sRaw)) == 0);

    course = make_course(4000.0f);
    memcpy(sRaw, &course, sizeof(course));
    assert(gdx_foreign_track_normalize(sRaw, sizeof(sRaw), GDX_FOREIGN_HOST_ORDER, sHost,
                                       GDX_FOREIGN_TRACK_STOCK_MAX_Y, &info, NULL, NULL) == 0);
}

static void pit_repair_logs_and_rechecksums(void) {
    GdxForeignCourseData course = make_course(200.0f);
    GdxForeignCourseData repaired;
    GdxForeignCourseDiagnostic info;
    int invalid;

    course.pit[0] = 40;
    course.pit[2] = -9;
    course.checksum = gdx_foreign_track_checksum(&course, GDX_FOREIGN_TRACK_STOCK_MAX_Y, &invalid);
    memset(sRaw, 0, sizeof(sRaw));
    memcpy(sRaw, &course, sizeof(course));
    sLogCount = 0;
    assert(gdx_foreign_track_normalize(sRaw, sizeof(sRaw), GDX_FOREIGN_HOST_ORDER, sHost,
                                       GDX_FOREIGN_TRACK_STOCK_MAX_Y, &info, record_pit, sLogged) == 0);
    assert(info.sanitizedPits == 2 && sLogCount == 2);
    assert(sLogged[0] == 0 && sLogged[1] == 40 && sLogged[2] == 2 && sLogged[3] == -9);
    memcpy(&repaired, sHost, sizeof(repaired));
    assert(repaired.pit[0] == -1 && repaired.pit[2] == -1);
    assert(repaired.checksum == gdx_foreign_track_checksum(&repaired, GDX_FOREIGN_TRACK_STOCK_MAX_Y, &invalid));
    assert(repaired.checksum != course.checksum);

    // A payload that fails its own checks is never repaired.
    sRaw[0] = 7;
    sLogCount = 0;
    assert(gdx_foreign_track_normalize(sRaw, sizeof(sRaw), GDX_FOREIGN_HOST_ORDER, sHost,
                                       GDX_FOREIGN_TRACK_STOCK_MAX_Y, &info, record_pit, sLogged) ==
           GDX_FOREIGN_TRACK_ERR_CREATOR);
    assert(info.sanitizedPits == 0 && sLogCount == 0 && ((GdxForeignCourseData*) sHost)->pit[0] == 40);
}

static void float_patterns(void) {
    const float values[] = {0.0f, 1.0f, -250.0f, 30000.0f};
    const unsigned int bad[] = {0x7F800000u, 0x7FC00000u, 0x00000001u, 0x80000001u};
    size_t i;

    for (i = 0; i < sizeof(values) / sizeof(values[0]); ++i) {
        assert(!gdx_foreign_track_float_invalid(&values[i]));
    }
    for (i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
        float value;
        memcpy(&value, &bad[i], sizeof(value));
        assert(gdx_foreign_track_float_invalid(&value));
    }
}

int main(void) {
    height_limit_is_a_parameter();
    pit_repair_logs_and_rechecksums();
    float_patterns();
    printf("gdx_foreign_track_tests: all passed\n");
    return 0;
}
