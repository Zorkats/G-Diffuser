#include "global.h"
#include "../gdx_course_contact.h"

extern int printf(const char*, ...);
extern void exit(int);
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); exit(1); } } while (0)

void func_i2_800B0FAC(CourseSegment* segment, Mtx3F* basis) {
    (void) segment;
    (void) basis;
    CHECK(0);
}

static f32 fabsf(f32 value) {
    return value < 0.0f ? -value : value;
}

static f32 dot(Vec3f* a, Vec3f* b) {
    return a->x*b->x + a->y*b->y + a->z*b->z;
}

int main(void) {
    CourseSegment segments[4];
    RacerSegmentPositionInfo source;
    RacerSegmentPositionInfo result;
    RacerSegmentPositionInfo before;
    Mtx3F basis;
    int i;
    int bank;

    memset(segments, 0, sizeof(segments));
    for (i = 0; i < 4; i++) {
        segments[i].prev = &segments[(i + 3) % 4];
        segments[i].next = &segments[(i + 1) % 4];
        segments[i].pos.x = (i - 1) * 1000.0f;
        segments[i].tension = 0.5f;
        segments[i].up.y = 1.0f;
    }
    memset(&source, 0, sizeof(source));
    source.courseSegment = &segments[0];
    source.segmentTValue = 0.5f;
    source.segmentForward.x = 1.0f;
    source.segmentForwardMagnitude = 1.0f;
    source.pos.x = 500.0f;
    source.pos.y = 3.0f;
    source.pos.z = 20.0f;
    before = source;
    CHECK(gdx_course_contact_project(&result, &source, &segments[1], 12) == 0);
    CHECK(result.courseSegment == &segments[1]);
    CHECK(fabsf(result.segmentTValue - 0.5f) < 0.0001f);
    CHECK(fabsf(result.segmentPos.x - 500.0f) < 0.001f);
    CHECK(memcmp(&source, &before, sizeof(source)) == 0);
    CHECK(memcmp(&result.pos, &source.pos, sizeof(Vec3f)) == 0);
    for (bank = 0; bank < 3; bank++) {
        segments[1].up.y = segments[2].up.y = bank == 0 ? 1.0f : 0.8f;
        segments[1].up.z = segments[2].up.z = bank == 1 ? 0.6f : (bank == 2 ? -0.6f : 0.0f);
        Course_SplineGetBasis(&segments[1], result.segmentTValue, &basis, 0.5f);
        CHECK(fabsf(dot(&basis.x, &basis.x) - 1.0f) < 0.0001f);
        CHECK(fabsf(dot(&basis.y, &basis.y) - 1.0f) < 0.0001f);
        CHECK(fabsf(dot(&basis.z, &basis.z) - 1.0f) < 0.0001f);
        CHECK(fabsf(dot(&basis.x, &basis.y)) < 0.0001f);
        CHECK(fabsf(dot(&basis.x, &basis.z)) < 0.0001f);
        CHECK(fabsf(dot(&basis.y, &basis.z)) < 0.0001f);
    }
    for (i = 0; i < 4; i++) {
        segments[i].pos.y = i == 2 ? 80.0f : 0.0f;
        segments[i].pos.z = i == 0 || i == 3 ? -200.0f : 100.0f;
    }
    source.segmentTValue = 0.35f;
    Course_SplineGetPosition(&segments[1], 0.35f, &source.pos);
    before = source;
    CHECK(gdx_course_contact_project(&result, &source, &segments[1], 12) == 0);
    CHECK(fabsf(result.segmentTValue - 0.35f) < 0.0001f);
    CHECK(memcmp(&source, &before, sizeof(source)) == 0);
    CHECK(memcmp(&result.pos, &source.pos, sizeof(Vec3f)) == 0);
    CHECK(gdx_course_contact_project(NULL, &source, &segments[1], 12) == -1);
    CHECK(gdx_course_contact_project(&result, NULL, &segments[1], 12) == -1);
    CHECK(gdx_course_contact_project(&result, &source, NULL, 12) == -1);
    CHECK(gdx_course_contact_project(&result, &source, &segments[1], 1) == -1);
    CHECK(gdx_course_contact_project(&result, &source, &segments[1], 33) == -1);
    source.segmentTValue = -0.1f;
    CHECK(gdx_course_contact_project(&result, &source, &segments[1], 12) == -1);
    source.segmentTValue = 1.1f;
    CHECK(gdx_course_contact_project(&result, &source, &segments[1], 12) == -1);
    memset(segments, 0, sizeof(segments));
    for (i = 0; i < 4; i++) {
        segments[i].prev = &segments[(i + 3) % 4];
        segments[i].next = &segments[(i + 1) % 4];
    }
    source.segmentTValue = 0.5f;
    CHECK(gdx_course_contact_project(&result, &source, &segments[1], 12) == -1);
    printf("All native contact projection tests passed\n");
    return 0;
}
