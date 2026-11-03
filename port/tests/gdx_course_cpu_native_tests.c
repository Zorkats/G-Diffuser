#include "global.h"
#include "fzx_course.h"
#include "fzx_racer.h"
#include "gdx_course_cpu.h"

extern int printf(const char*, ...);
extern void exit(int);
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); exit(1); } } while (0)

CourseEffectsInfo gCourseEffectsInfo;
Effect gEffects[192];
static int sIgnore;
static float sRange;
static float sPointRange;

s32 CVarGetInteger(const char* name, s32 fallback) {
    (void) name;
    (void) fallback;
    return sIgnore;
}

f32 CVarGetFloat(const char* name, f32 fallback) {
    (void) name;
    (void) fallback;
    return sRange;
}

s32 gdx_course_branch_settings_point(const CourseSegment* segment) {
    (void) segment;
    return 256;
}

float gdx_settings_view_magnet_range(int point) {
    CHECK(point == 256);
    return sPointRange;
}

extern void gdx_course_cpu_target(Racer* racer);

int main(void) {
    Racer racer;
    Racer before;
    CourseSegment segment;
    CourseEffect effects[2];
    float target;

    memset(&racer, 0, sizeof(racer));
    memset(&segment, 0, sizeof(segment));
    memset(effects, 0, sizeof(effects));
    segment.trackSegmentInfo = TRACK_SHAPE_ROAD;
    segment.effectsStart = gEffects;
    segment.effectsEnd = gEffects + 1;
    gCourseEffectsInfo.effects = effects;
    gCourseEffectsInfo.count = 1;
    racer.segmentPositionInfo.courseSegment = &segment;
    racer.segmentPositionInfo.segmentTValue = 0.25f;
    racer.currentRadiusLeft = racer.currentRadiusRight = 260.0f;
    racer.unk_33C = -150.0f;
    effects[0].effectType = COURSE_EFFECT_DIRT;
    effects[0].segmentTValueEnd = 0.9f;
    effects[0].rightEdgeDistance = 0.0f;
    effects[0].leftEdgeDistance = 260.0f;
    sRange = 80.0f;
    before = racer;
    gdx_course_cpu_target(&racer);
    CHECK(racer.unk_33C > 46.0f);
    target = racer.unk_33C;
    racer.unk_33C = before.unk_33C;
    CHECK(memcmp(&racer, &before, sizeof(racer)) == 0);
    racer.unk_33C = target;
    before = racer;
    effects[0].segmentTValueEnd = 0.1f;
    gdx_course_cpu_target(&racer);
    CHECK(memcmp(&racer, &before, sizeof(racer)) == 0);
    effects[0].segmentTValueEnd = 1.0f;
    effects[0].effectType = COURSE_EFFECT_MAGNET_RIGHT;
    effects[0].rightEdgeDistance = -260.0f;
    effects[0].leftEdgeDistance = -220.0f;
    racer.unk_33C = 200.0f;
    sPointRange = 10.0f;
    gdx_course_cpu_target(&racer);
    CHECK(racer.unk_33C == 200.0f);
    sPointRange = 200.0f;
    gdx_course_cpu_target(&racer);
    CHECK(racer.unk_33C < 84.0f);
    sIgnore = 1;
    racer.unk_33C = 200.0f;
    sRange = 10.0f;
    gdx_course_cpu_target(&racer);
    CHECK(racer.unk_33C == 200.0f);
    segment.trackSegmentInfo = TRACK_SHAPE_PIPE;
    sRange = 200.0f;
    gdx_course_cpu_target(&racer);
    CHECK(racer.unk_33C == 200.0f);
    printf("All native CPU adapter tests passed\n");
    return 0;
}
