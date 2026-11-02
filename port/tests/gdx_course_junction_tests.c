#include "gdx_course_junction.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <limits.h>
#ifdef _WIN32
#include <windows.h>
#endif

static GdxJunctionWorkspace workspace;
static GdxJunctionTriangle output[GDX_JUNCTION_MAX_PIECES];

static GdxJunctionTriangle triangle(double x, double z, double size, double height, double bank) {
    GdxJunctionTriangle t = {{{{x, height + bank * x, z}, {x, z}},
                              {{x + size, height + bank * (x + size), z}, {x + size, z}},
                              {{x, height + bank * x, z + size}, {x, z + size}}}};
    return t;
}

static double area(const GdxJunctionTriangle* t) {
    return fabs((t->v[1].pos[0] - t->v[0].pos[0]) * (t->v[2].pos[2] - t->v[0].pos[2]) -
                (t->v[2].pos[0] - t->v[0].pos[0]) * (t->v[1].pos[2] - t->v[0].pos[2])) * 0.5;
}

static double total_area(int count) {
    double sum = 0;
    int i;
    assert(count >= 0);
    for (i = 0; i < count; i++) {
        int j;
        sum += area(&output[i]);
        for (j = 0; j < 3; j++) {
            assert(fabs(output[i].v[j].uv[0] - output[i].v[j].pos[0]) < 1e-8);
            assert(fabs(output[i].v[j].uv[1] - output[i].v[j].pos[2]) < 1e-8);
        }
    }
    return sum;
}

static int contains(const GdxJunctionTriangle* t, double x, double z) {
    double d[3];
    int i;
    for (i = 0; i < 3; i++) {
        const double* a = t->v[i].pos;
        const double* b = t->v[(i + 1) % 3].pos;
        d[i] = (b[0] - a[0]) * (z - a[2]) - (b[2] - a[2]) * (x - a[0]);
    }
    return (d[0] > 1e-8 && d[1] > 1e-8 && d[2] > 1e-8) ||
           (d[0] < -1e-8 && d[1] < -1e-8 && d[2] < -1e-8);
}

static void coverage(const GdxJunctionTriangle* b, int count, double bank) {
    int ix, iz, i;
    for (ix = 0; ix < 69; ix++) {
        double x = -2.0 + ix * 0.271731;
        for (iz = 0; iz < 63; iz++) {
            double z = -1.0 + iz * 0.197613;
            int hits = 0;
            int inB = contains(b, x, z);
            int owned = x >= 0 && z >= 0 && x + z <= 10 && fabs(bank * x) <= GDX_JUNCTION_DISTANCE;
            for (i = 0; i < count; i++) {
                hits += contains(&output[i], x, z);
            }
            assert(hits <= 1);
            assert(hits == (inB && !owned));
            if (inB && !hits) {
                // Removed contact remains within the owner's one-unit normal slab.
                assert(fabs(bank * x) <= GDX_JUNCTION_DISTANCE + 1e-8);
            }
        }
    }
}

static void test_winding_and_coverage(void) {
    GdxJunctionTriangle a = triangle(0, 0, 10, 0, 0);
    GdxJunctionTriangle b;
    GdxJunctionCutter cutters[2];
    int reverse, i, count;
    for (reverse = 0; reverse < 2; reverse++) {
        assert(gdx_junction_cutter(&a, &cutters[0]));
        cutters[1] = cutters[0];
        b = triangle(5, 0, 10, 0, 0);
        count = gdx_junction_subtract(&b, cutters, 2, &workspace, output, GDX_JUNCTION_MAX_PIECES);
        assert(fabs(total_area(count) - 37.5) < 1e-8);
        coverage(&b, count, 0);
        b = triangle(0, 0, 10, 0, 0.2);
        count = gdx_junction_subtract(&b, cutters, 2, &workspace, output, GDX_JUNCTION_MAX_PIECES);
        assert(fabs(total_area(count) - 12.5) < 1e-8);
        coverage(&b, count, 0.2);
        for (i = 0; i < count; i++) {
            double signedArea = (output[i].v[1].pos[0] - output[i].v[0].pos[0]) *
                                (output[i].v[2].pos[2] - output[i].v[0].pos[2]) -
                                (output[i].v[2].pos[0] - output[i].v[0].pos[0]) *
                                (output[i].v[1].pos[2] - output[i].v[0].pos[2]);
            assert(signedArea > 0);
        }
        {
            GdxJunctionVertex tmp = a.v[1];
            a.v[1] = a.v[2];
            a.v[2] = tmp;
        }
    }
    // Reversing the emitted winding must not reverse individual clipped fragments.
    b = triangle(5, 0, 10, 0, 0);
    {
        GdxJunctionVertex tmp = b.v[1];
        b.v[1] = b.v[2];
        b.v[2] = tmp;
    }
    count = gdx_junction_subtract(&b, cutters, 1, &workspace, output, GDX_JUNCTION_MAX_PIECES);
    assert(fabs(total_area(count) - 37.5) < 1e-8);
    coverage(&b, count, 0);
    for (i = 0; i < count; i++) {
        assert((output[i].v[1].pos[0] - output[i].v[0].pos[0]) *
               (output[i].v[2].pos[2] - output[i].v[0].pos[2]) -
               (output[i].v[2].pos[0] - output[i].v[0].pos[0]) *
               (output[i].v[1].pos[2] - output[i].v[0].pos[2]) < 0);
    }
}

static void test_vertical_and_bounds(void) {
    GdxJunctionTriangle a = triangle(0, 0, 10, 0, 0);
    GdxJunctionTriangle b = triangle(5, 0, 10, 0, 0);
    GdxJunctionTriangle sentinel;
    GdxJunctionCutter cutter;
    int i, j, count;
    for (i = 0; i < 3; i++) {
        a.v[i].pos[1] = a.v[i].pos[2];
        a.v[i].pos[2] = 0;
        b.v[i].pos[1] = b.v[i].pos[2];
        b.v[i].pos[2] = 0;
    }
    assert(gdx_junction_cutter(&a, &cutter));
    count = gdx_junction_subtract(&b, &cutter, 1, &workspace, output, GDX_JUNCTION_MAX_PIECES);
    for (i = 0; i < count; i++) {
        for (j = 0; j < 3; j++) {
            output[i].v[j].pos[2] = output[i].v[j].pos[1];
            output[i].v[j].pos[1] = 0;
        }
    }
    assert(fabs(total_area(count) - 37.5) < 1e-8);
    memset(&sentinel, 0x5A, sizeof(sentinel));
    output[0] = sentinel;
    assert(gdx_junction_subtract(&b, &cutter, 1, &workspace, output, 0) == -1);
    assert(memcmp(output, &sentinel, sizeof(sentinel)) == 0);
    assert(gdx_junction_subtract(&a, &cutter, 1, &workspace, output, 0) == 0);
    assert(gdx_junction_subtract(&b, &cutter, INT_MAX, &workspace, output, 128) == -1);
    assert(gdx_junction_subtract(&b, NULL, 1, &workspace, output, 128) == -1);
    assert(gdx_junction_subtract(&b, &cutter, -1, &workspace, output, 128) == -1);
    cutter.plane[0][0] = NAN;
    assert(gdx_junction_subtract(&b, &cutter, 1, &workspace, output, 128) == -1);
    a.v[2] = a.v[0];
    assert(!gdx_junction_cutter(&a, &cutter));
    a.v[0].pos[0] = INFINITY;
    assert(!gdx_junction_cutter(&a, &cutter));
    b.v[0].uv[0] = NAN;
    assert(gdx_junction_subtract(&b, NULL, 0, &workspace, output, 128) == -1);
    assert(gdx_junction_quantize(-32769) == -32768);
    assert(gdx_junction_quantize(32768) == 32767);
    assert(gdx_junction_quantize(NAN) == 0);
    assert(gdx_junction_quantize(1.49) == 1);
    assert(gdx_junction_quantize(-1.51) == -2);
}

static volatile double benchmarkSink;

static double benchmark_now(void) {
#ifdef _WIN32
    LARGE_INTEGER counter, frequency;
    QueryPerformanceCounter(&counter);
    QueryPerformanceFrequency(&frequency);
    return (double)counter.QuadPart / (double)frequency.QuadPart;
#else
    return (double)clock() / CLOCKS_PER_SEC;
#endif
}

static void test_workspace_exhaustion(void) {
    GdxJunctionTriangle b = triangle(0, 0, 100, 0, 0);
    GdxJunctionCutter cutters[100];
    GdxJunctionTriangle sentinel;
    int i;
    for (i = 0; i < 100; i++) {
        double x = 0.5 + i * 0.8;
        GdxJunctionTriangle a = triangle(x, -10, 1, 0, 0);
        a.v[1].pos[0] = x + 0.2;
        a.v[2].pos[2] = 200;
        assert(gdx_junction_cutter(&a, &cutters[i]));
    }
    memset(&sentinel, 0x3C, sizeof(sentinel));
    output[0] = sentinel;
    assert(gdx_junction_subtract(&b, cutters, 100, &workspace, output, 128) == -1);
    assert(memcmp(&output[0], &sentinel, sizeof(sentinel)) == 0);
}

static void benchmark(void) {
    GdxJunctionTriangle a = triangle(0, 0, 100, 0, 0);
    GdxJunctionTriangle b = triangle(50, 0, 100, 0, 0);
    GdxJunctionCutter cutter;
    int mode, sample, repeat;
    assert(gdx_junction_cutter(&a, &cutter));
    for (mode = 0; mode < 3; mode++) {
        double start = 0;
        for (sample = -32; sample < 200; sample++) {
            if (sample == 0) {
                start = benchmark_now();
            }
            for (repeat = 0; repeat < 100; repeat++) {
                int count;
                if (mode == 0) {
                    output[0] = b;
                    count = 1;
                } else {
                    GdxJunctionTriangle input = b;
                    if (mode == 2) {
                        int j;
                        for (j = 0; j < 3; j++) {
                            input.v[j].pos[1] = 200;
                        }
                    }
                    count = gdx_junction_subtract(&input, &cutter, 1, &workspace, output, 128);
                }
                assert(count > 0);
                benchmarkSink += output[0].v[0].pos[0];
            }
        }
        printf("junction benchmark %s: %.3f us/op, 32 warmups, 200 samples x 100 operations\n",
               mode == 0 ? "legacy-copy" : (mode == 1 ? "coplanar-cut" : "overpass-reject"),
               (benchmark_now() - start) * 1000000.0 / 20000.0);
    }
}

int main(void) {
    GdxJunctionTriangle a = triangle(0, 0, 10, 0, 0);
    GdxJunctionTriangle b = a;
    GdxJunctionCutter cutter;
    int count;
    assert(gdx_junction_cutter(&a, &cutter));
    count = gdx_junction_subtract(&b, &cutter, 1, &workspace, output, GDX_JUNCTION_MAX_PIECES);
    assert(count == 0);
    b = triangle(5, 0, 10, 0, 0);
    count = gdx_junction_subtract(&b, &cutter, 1, &workspace, output, GDX_JUNCTION_MAX_PIECES);
    assert(fabs(total_area(count) - 37.5) < 1e-8);
    b = triangle(0, 0, 10, 5, 0);
    count = gdx_junction_subtract(&b, &cutter, 1, &workspace, output, GDX_JUNCTION_MAX_PIECES);
    assert(count == 1 && memcmp(&output[0], &b, sizeof(b)) == 0);
    b = triangle(0, 0, 10, 0, 0.5);
    count = gdx_junction_subtract(&b, &cutter, 1, &workspace, output, GDX_JUNCTION_MAX_PIECES);
    assert(count == 1 && memcmp(output, &b, sizeof(b)) == 0);
    test_winding_and_coverage();
    test_vertical_and_bounds();
    test_workspace_exhaustion();
    benchmark();
    puts("All junction geometry tests passed");
    return 0;
}
