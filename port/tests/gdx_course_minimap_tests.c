#include "gdx_course_minimap.h"
#include <assert.h>
#include <float.h>
#include <math.h>
#include <string.h>

int main(void) {
    const float x[] = {0, 100, 200, 300, 400, 500};
    const float z[] = {0, 100, 0, -100, 0, 100};
    float outX[GDX_PREVIEW_CAPACITY];
    float outZ[GDX_PREVIEW_CAPACITY];
    int next[GDX_PREVIEW_CAPACITY];
    GdxBranchSet forks = {0};
    unsigned char guarded[4098];
    int count;
    int i;

    forks.ringCount = 6;
    forks.forkCount = 1;
    forks.forks[0].forkPoint = 1;
    forks.forks[0].mergePoint = 4;
    forks.forks[0].nodeCount = 1;
    forks.forks[0].nodes[0].pos[0] = 200;
    forks.forks[0].nodes[0].pos[2] = 1000;
    count = gdx_course_preview_build(x, z, 6, &forks, 0, outX, outZ, next, GDX_PREVIEW_CAPACITY);
    assert(count == 72 && next[71] == 0);
    assert(outX[0] == 0 && outZ[0] == 0);
    assert(outX[12] == 100 && outZ[12] == 100);
    // The old preview's uniform Catmull-Rom midpoint between points 1 and 2.
    assert(outX[18] == 150 && outZ[18] == 62.5f);
    count = gdx_course_preview_build(x, z, 6, &forks, 1, outX, outZ, next, GDX_PREVIEW_CAPACITY);
    assert(count == 97 && next[71] == 0 && next[96] == -1);
    assert(outX[72] == x[1] && outZ[72] == z[1]);
    assert(outX[84] == 200 && outZ[84] == 1000);
    assert(outX[96] == x[4] && outZ[96] == z[4]);
    for (i = 0; i < count; i++) {
        assert(next[i] == -1 || (next[i] >= 0 && next[i] < count));
        assert(isfinite(outX[i]) && isfinite(outZ[i]));
    }
    assert(gdx_course_preview_build(x, z, 6, &forks, 1, outX, outZ, next, 96) == 0);
    forks.forks[0].mergePoint = 99;
    assert(gdx_course_preview_build(x, z, 6, &forks, 1, outX, outZ, next, GDX_PREVIEW_CAPACITY) == 0);
    {
        const float extreme[] = {FLT_MAX, -FLT_MAX};
        assert(gdx_course_preview_build(extreme, extreme, 2, NULL, 0,
                                        outX, outZ, next, GDX_PREVIEW_CAPACITY) == 0);
    }
    {
        float ringX[60];
        float ringZ[60];
        float legacyX[72];
        float legacyZ[72];
        int legacyNext[72];
        GdxBranchSet maximum = {0};
        maximum.ringCount = 60;
        maximum.forkCount = 4;
        for (i = 0; i < 60; i++) {
            ringX[i] = (float)i * 100;
            ringZ[i] = (float)(i % 3) * 100;
        }
        for (i = 0; i < 4; i++) {
            maximum.forks[i].forkPoint = (unsigned char)(1 + i * 4);
            maximum.forks[i].mergePoint = (unsigned char)(3 + i * 4);
            maximum.forks[i].nodeCount = 1;
            maximum.forks[i].nodes[0].pos[0] = (float)i * 400;
            maximum.forks[i].nodes[0].pos[2] = 1000;
        }
        count = gdx_course_preview_build(ringX, ringZ, 60, &maximum, 1,
                                         outX, outZ, next, GDX_PREVIEW_CAPACITY);
        assert(count == GDX_PREVIEW_CAPACITY);
        assert(next[719] == 0);
        for (i = 0; i < 4; i++) {
            assert(next[720 + i * 25 + 24] == -1);
        }
        assert(gdx_course_preview_build(ringX, ringZ, 60, &maximum, 1,
                                        outX, outZ, next, GDX_PREVIEW_CAPACITY - 1) == 0);
        assert(gdx_course_preview_build(x, z, 6, NULL, 0, legacyX, legacyZ, legacyNext, 72) == 72);
        // Disabled fork rendering must not depend on the contents of the fork store.
        assert(gdx_course_preview_build(x, z, 6, &forks, 0, outX, outZ, next, GDX_PREVIEW_CAPACITY) == 72);
        assert(memcmp(legacyX, outX, sizeof(legacyX)) == 0);
        assert(memcmp(legacyZ, outZ, sizeof(legacyZ)) == 0);
        assert(memcmp(legacyNext, next, sizeof(legacyNext)) == 0);
    }

    memset(guarded, 0, sizeof(guarded));
    guarded[0] = guarded[4097] = 99;
    gdx_course_minimap_plot(guarded + 1, 0, 0, 1, 0);
    assert(guarded[1 + 32 * 64 + 31] == 1);
    assert(guarded[1 + 32 * 64 + 33] == 1);
    gdx_course_minimap_plot(guarded + 1, 0, 0, 1, 1);
    assert(guarded[1 + 32 * 64 + 32] == 2);
    gdx_course_minimap_plot(guarded + 1, -16000, -16000, 1, 0);
    gdx_course_minimap_plot(guarded + 1, 16000, 16000, 1, 1);
    gdx_course_minimap_plot(guarded + 1, INFINITY, NAN, 1, 1);
    assert(guarded[0] == 99 && guarded[4097] == 99);
    return 0;
}
