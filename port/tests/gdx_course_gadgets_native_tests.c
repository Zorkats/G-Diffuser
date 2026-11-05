#include "global.h"
#include "fzx_course.h"
#include "gdx_course_branch.h"
#include "gdx_course_gadget_budget.h"
#include "gdx_course_settings_view.h"
extern int printf(const char* format, ...);
extern void exit(int code);
#define assert(condition) do { if (!(condition)) { printf("FAIL line %d: %s\n", __LINE__, #condition); exit(1); } } while (0)

CourseContext gCourseCtx;
unk_807B3C20 D_802CB6D0;
CourseInfo gCourseInfos[56];
s32 gCourseIndex;
CourseFeaturesInfo gCourseFeaturesInfo;
CourseEffectsInfo gCourseEffectsInfo;
CourseFeature gCourseFeatures[120];
CourseEffect gCourseEffects[192];
static CourseSegment sSegments[32];
static signed char sParts[32][9];
static int sCount;

s32 gdx_course_branch_gadget_count(s32 course) {
    (void) course;
    return sCount;
}

CourseSegment* gdx_course_branch_gadget_segment(s32 course, s32 index) {
    (void) course;
    assert(index >= 0 && index < sCount);
    return &sSegments[index];
}

s32 gdx_course_branch_gadget_source(s32 index) {
    return index % 4;
}

s32 gdx_course_branch_gadget_part(s32 index, s32 part, s32 fallback) {
    (void) fallback;
    assert(index >= 0 && index < sCount && part >= 0 && part < 9);
    return sParts[index][part];
}

s32 gdx_course_branch_settings_point(const CourseSegment* segment) {
    return (s32) (segment - sSegments);
}

int gdx_settings_view_pit_width(int point) {
    return point == 4 ? 80 : 0;
}

int gdx_settings_view_dirt_width(int point) {
    (void) point;
    return 0;
}

int gdx_settings_view_ice_width(int point) {
    (void) point;
    return 0;
}

float gdx_settings_view_magnet_range(int point) {
    (void) point;
    return 0.0f;
}

void gdx_dbg_logf(const char* fmt, ...) {
    (void) fmt;
}

float gdx_settings_view_part_edge(float stock, int percent, int shape, float width, float nextWidth) {
    (void) shape;
    (void) width;
    (void) nextWidth;
    return percent == 80 ? 42.0f : stock;
}

s32 Math_Round(f32 value) {
    return (s32) (value >= 0.0f ? value + 0.5f : value - 0.5f);
}

f32 Course_SplineGetTangent(CourseSegment* segment, f32 t, Vec3f* tangent) {
    (void) t;
    tangent->x = segment->length;
    tangent->y = 0.0f;
    tangent->z = 0.0f;
    return segment->length;
}

static void reset(int count) {
    int i;

    memset(&gCourseCtx, 0, sizeof(gCourseCtx));
    memset(sParts, -1, sizeof(sParts));
    memset(sSegments, 0, sizeof(sSegments));
    memset(gCourseFeatures, 0xAB, sizeof(gCourseFeatures));
    memset(gCourseEffects, 0xAB, sizeof(gCourseEffects));
    gCourseCtx.courseData.controlPointCount = 4;
    gCourseFeaturesInfo.features = gCourseFeatures;
    gCourseFeaturesInfo.featureCount = 0;
    gCourseEffectsInfo.effects = gCourseEffects;
    gCourseEffectsInfo.count = 0;
    sCount = count;
    for (i = 0; i < count; i++) {
        sSegments[i].next = &sSegments[(i + 1) % count];
        sSegments[i].prev = &sSegments[(i + count - 1) % count];
        sSegments[i].radiusLeft = 260.0f;
        sSegments[i].radiusRight = 260.0f;
        sSegments[i].length = 1000.0f;
        sSegments[i].trackSegmentInfo = TRACK_SHAPE_ROAD;
    }
}

static void test_native_demand(void) {
    const int maximum[9] = { PIT_MAX, DASH_MAX, DIRT_MAX, ICE_MAX, JUMP_MAX, LANDMINE_MAX,
                             GATE_MAX, BUILDING_MAX, SIGN_MAX };
    const GdxGadgetDemand limits = { 120, 32, 48, 4, 192, 21 };
    int part;
    int value;

    for (part = 0; part < 9; part++) {
        for (value = 0; value < maximum[part]; value++) {
            GdxGadgetDemand expected = { 0 };

            reset(5);
            sParts[4][part] = (signed char) value;
            assert(gdx_gadget_demand_add(&expected, sParts[4], &limits));
            Course_GadgetsInit(0);
            assert(gCourseFeaturesInfo.featureCount == expected.features);
            assert(gCourseFeaturesInfo.jumpCount == expected.jumps);
            assert(gCourseFeaturesInfo.landmineCount == expected.landmines);
            assert(gCourseEffectsInfo.count == expected.effects);
            if (expected.features) {
                assert(gCourseFeatures[0].segmentIndex == 4);
            }
            if (expected.effects) {
                assert(gCourseEffects[0].segmentIndex == 4);
            }
        }
    }
}

extern bool gInCourseEditor;
extern bool gInCourseEditTestRun;

static void test_testdrive_budget(void) {
    int i;

    /* The Expansion Kit holds 8 jumps in the editor and in a race, so every jump the editor takes also races. */
    reset(9);
    gInCourseEditor = true;
    gInCourseEditTestRun = false;
    D_802CB6D0.controlPointCount = 4;
    for (i = 0; i < 8; i++) {
        sParts[i][4] = 0;
    }
    Course_GadgetsInit(0);
    assert(gdx_course_gadget_error == 0);
    assert(gCourseFeaturesInfo.jumpCount == 8);
    assert(Course_GdxCanTestDrive(0));
    sParts[8][4] = 0;
    assert(!Course_GdxCanTestDrive(0));
    Course_GadgetsInit(0);
    assert(gdx_course_gadget_error == 1);
    assert(gCourseFeaturesInfo.jumpCount == 0);
    sParts[8][4] = -1;
    gdx_course_gadget_error = 0;
    assert(Course_GdxCanTestDrive(0));
    gdx_course_gadget_error = 2;
    assert(!Course_GdxCanTestDrive(0));
    gInCourseEditor = false;

    /* A shared course with 6 jumps, raced outside the editor, keeps its gadgets. */
    reset(9);
    for (i = 0; i < 6; i++) {
        sParts[i][4] = 0;
    }
    Course_GadgetsInit(0);
    assert(gdx_course_gadget_error == 0);
    assert(gCourseFeaturesInfo.jumpCount == 6);
}

static void test_combined_and_refusal(void) {
    int i;

    reset(5);
    sParts[0][0] = PIT_LEFT;
    sParts[4][0] = PIT_RIGHT;
    sParts[0][6] = GATE_SQUARE;
    Course_GadgetsInit(0);
    assert(gCourseFeaturesInfo.featureCount == 1);
    assert(gCourseEffectsInfo.count == 2);
    assert(gCourseEffects[0].segmentIndex == 0 && gCourseEffects[1].segmentIndex == 4);
    assert(gCourseEffects[1].leftEdgeDistance == -42.0f);

    reset(9);
    for (i = 0; i < 9; i++) {
        sParts[i][5] = LANDMINE_MIDDLE;
    }
    Course_GadgetsInit(0);
    assert(gCourseFeaturesInfo.featureCount == 0 && gCourseEffectsInfo.count == 0);
    assert(((unsigned char*) gCourseFeatures)[0] == 0xAB);

    reset(9);
    for (i = 0; i < 9; i++) {
        sParts[i][4] = 0;
    }
    Course_GadgetsInit(0);
    assert(gCourseFeaturesInfo.featureCount == 0);

    reset(22);
    for (i = 0; i < 22; i++) {
        sParts[i][1] = 0;
    }
    Course_GadgetsInit(0);
    assert(gCourseEffectsInfo.count == 0);
    assert(((unsigned char*) gCourseEffects)[0] == 0xAB);
}

int main(void) {
    test_native_demand();
    test_testdrive_budget();
    test_combined_and_refusal();
    printf("All native gadget generation tests passed\n");
    return 0;
}
