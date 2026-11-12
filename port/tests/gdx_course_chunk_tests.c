#include "global.h"
#include "fzx_course.h"
#include "fzx_math.h"

extern int printf(const char*, ...);
extern void exit(int);
#define CHECK(c) do { if (!(c)) { printf("FAIL line %d: %s\n", __LINE__, #c); exit(1); } } while (0)
#define SEGMENT_CHUNK_COUNT 768

typedef struct {
    float textureScale;
    int textureCoordinateMask;
} ChunkTexture;
typedef ChunkTexture unk_800CF528;
static ChunkTexture sTexture = { 10.0f, 1023 };
#define TRACK_TYPE_ROW(shape, type) (&sTexture)
int gInCourseEditor;
int gSegmentChunkCount;
SegmentChunk gSegmentChunks[SEGMENT_CHUNK_COUNT + 1];
SegmentChunk* sLastSegmentChunk;
Mtx3F D_80033840[SEGMENT_CHUNK_COUNT + 1];
static int D_800F8524;
static float D_800F89E0;
static float D_800F89E4;
static float D_800F89E8;
static float gTrackJoinUpperLength[8];

static void reference(SegmentChunk* chunk, float left, float right, Mtx3F* basis,
                      Vec3f* l, Vec3f* r, Vec3f* top, Vec3f* bottom, Vec3f* center) {
    *center = *top = *bottom = chunk->pos;
    *l = chunk->pos;
    *r = chunk->pos;
    l->x += basis->z.x * left;
    l->z += basis->z.z * left;
    r->x -= basis->z.x * right;
    r->z -= basis->z.z * right;
    top->y += 5;
    bottom->y -= 5;
}
#define Course_ChunkCalculateRoadAirReferencePos reference
static void (*sSegmentChunkCalculateReferencePosFuncs[8])(
    SegmentChunk*, float, float, Mtx3F*, Vec3f*, Vec3f*, Vec3f*, Vec3f*, Vec3f*) = {
    reference, reference, reference, reference, reference, reference, reference, reference
};
static void join(SegmentChunk* a, SegmentChunk* b, float t) { (void)a; (void)b; (void)t; }
static void (*sSegmentChunkJoinFuncs[8])(SegmentChunk*, SegmentChunk*, float) = {
    join, join, join, join, join, join, join, join
};
void func_i2_800B0FAC(CourseSegment* segment, Mtx3F* basis) {
    (void)segment;
    (void)basis;
    CHECK(0);
}

/* Road themes are out of this test: every segment wears its venue's road at the row's own scale. */
static s32 sGdxBuildTheme;
static s32 Course_GdxSegmentTheme(const CourseSegment* segment) {
    (void)segment;
    return 0;
}
static f32 Course_GdxRowScale(const unk_800CF528* row, s32 theme) {
    (void)theme;
    return row->textureScale;
}

#include "../../decomp/src/game/course_chunk_build.inc"

static CourseSegment sSegments[GDX_COURSE_POINT_CAPACITY];
static CourseInfo sInfo;
static void circle(int count, float radius) {
    int i;
    memset(&sInfo, 0, sizeof(sInfo));
    memset(sSegments, 0, sizeof(sSegments));
    sInfo.courseSegments = sSegments;
    sInfo.segmentCount = count;
    for (i = 0; i < count; ++i) {
        CourseSegment* s = &sSegments[i];
        float a = i * 6.28318530718f / count;
        s->next = &sSegments[(i + 1) % count];
        s->prev = &sSegments[(i + count - 1) % count];
        s->pos.x = cosf(a) * radius;
        s->pos.z = sinf(a) * radius;
        s->up.y = 1;
        s->radiusLeft = s->radiusRight = 100;
        s->tension = 0.5f;
        s->trackSegmentInfo = i ? ROAD_1 : ROAD_START_LINE;
    }
    for (i = 0; i < count; ++i) {
        CourseSegment* s = &sSegments[i];
        Vec3f p;
        Course_SplineGetPosition(s, .25f, &s->quarterMarkPos);
        Course_SplineGetPosition(s, .5f, &s->halfMarkPos);
        Course_SplineGetPosition(s, .75f, &s->threeQuarterMarkPos);
        p = s->pos;
        s->quarterMarkLength = Course_ChunkPositionDistance(&p, &s->quarterMarkPos);
        s->halfMarkLength = s->quarterMarkLength + Course_ChunkPositionDistance(&s->quarterMarkPos, &s->halfMarkPos);
        s->threeQuarterMarkLength = s->halfMarkLength + Course_ChunkPositionDistance(&s->halfMarkPos, &s->threeQuarterMarkPos);
        s->length = s->threeQuarterMarkLength + Course_ChunkPositionDistance(&s->threeQuarterMarkPos, &s->next->pos);
    }
}

int main(void) {
    int n;
    int mode;
    for (mode = 1; mode <= 2; ++mode) {
        D_800F8524 = mode;
        D_800F89E0 = 20;
        D_800F89E4 = 2000;
        D_800F89E8 = 400;
        for (n = 100; n <= 127; ++n) {
            int i;
            int result;
            circle(n, 5000);
            result = func_800A1954(&sInfo);
            CHECK(!(result & 0x20000));
            CHECK(gSegmentChunkCount >= n && gSegmentChunkCount <= SEGMENT_CHUNK_COUNT);
            CHECK(sLastSegmentChunk == &gSegmentChunks[gSegmentChunkCount]);
            for (i = 0; i < n; ++i) {
                CHECK(sSegments[i].startChunk >= gSegmentChunks);
                CHECK(sSegments[i].endChunk <= sLastSegmentChunk);
            }
        }
    }
    D_800F8524 = 0;
    D_800F89E0 = 1;
    circle(127, 5000);
    CHECK(func_800A1954(&sInfo) & 0x20000);
    CHECK(gSegmentChunkCount == 0);
    CHECK(sLastSegmentChunk == NULL);
    for (n = 0; n < 127; ++n) {
        CHECK(sSegments[n].startChunk == NULL && sSegments[n].endChunk == NULL);
    }
    printf("All native chunk capacity tests passed\n");
    return 0;
}
