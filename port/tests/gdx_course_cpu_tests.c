#include "../gdx_course_cpu.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#endif

static void test_interval_properties(void) {
    GdxCpuHazard hazards[GDX_CPU_MAX_HAZARDS];
    unsigned int seed = 0x19770829;
    int sample;

    for (sample = 0; sample < 2048; sample++) {
        int count = sample % (GDX_CPU_MAX_HAZARDS + 1);
        int i;
        float desired;
        float target;
        int safe;
        int gridSafe = 0;
        float gridDistance = 10000.0f;

        for (i = 0; i < count; i++) {
            seed = seed * 1664525u + 1013904223u;
            hazards[i].min = (float) (seed % 600) - 300;
            seed = seed * 1664525u + 1013904223u;
            hazards[i].max = hazards[i].min + (float) (seed % 200);
        }
        seed = seed * 1664525u + 1013904223u;
        desired = (float) (seed % 429) - 214;
        safe = gdx_cpu_avoid_hazards(desired, 260, 260, 46, hazards, count, &target);
        assert(target >= -214 && target <= 214);
        for (i = -214; i <= 214; i++) {
            int j;
            int clear = 1;

            for (j = 0; j < count; j++) {
                if (i >= hazards[j].min - 46 && i <= hazards[j].max + 46) {
                    clear = 0;
                }
                if (safe) {
                    assert(target < hazards[j].min - 46 || target > hazards[j].max + 46);
                }
            }
            if (clear) {
                float distance = fabsf(i - desired);
                gridSafe = 1;
                if (distance < gridDistance) {
                    gridDistance = distance;
                }
            }
        }
        assert(!gridSafe || safe);
        assert(!gridSafe || fabsf(target - desired) <= gridDistance + 0.02f);
    }
}

static double benchmark_now(void) {
#ifdef _WIN32
    LARGE_INTEGER now;
    LARGE_INTEGER frequency;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&frequency);
    return (double) now.QuadPart / frequency.QuadPart;
#else
    return (double) clock() / CLOCKS_PER_SEC;
#endif
}

static void benchmark(void) {
    GdxCpuHazard hazards[8];
    static volatile float sink;
    int i;
    int mode;
    int sample;
    int repeat;

    for (i = 0; i < 8; i++) {
        hazards[i].min = -200 + i * 50.0f;
        hazards[i].max = hazards[i].min + 10;
    }
    for (mode = 0; mode < 2; mode++) {
        double start = 0;
        for (sample = -32; sample < 200; sample++) {
            if (sample == 0) {
                start = benchmark_now();
            }
            for (repeat = 0; repeat < 1000; repeat++) {
                float target;
                gdx_cpu_avoid_hazards(-145, 260, 260, 10, hazards, mode ? 8 : 0, &target);
                sink = target;
            }
        }
        printf("CPU interval benchmark %s: %.3f us/op, 32 warmups, 200 samples x 1000 operations\n",
               mode ? "eight-hazards" : "zero-hazard-control",
               (benchmark_now() - start) * 1000000.0 / 200000.0);
    }
}

int main(void) {
    GdxCpuHazard h[3];
    float target = 123.0f;
    assert(gdx_cpu_hazard_interval(GDX_CPU_HAZARD_STRIP, 0, 5000, 260, 260, 80, &h[0]));
    assert(gdx_cpu_avoid_hazards(-150, 260, 260, 46, h, 1, &target));
    assert(target > 46 && target <= 214);
    assert(gdx_cpu_avoid_hazards(150, 260, 260, 46, h, 1, &target) && target == 150);
    assert(gdx_cpu_hazard_interval(GDX_CPU_HAZARD_STRIP, -5000, 5000, 260, 260, 80, &h[0]));
    assert(!gdx_cpu_avoid_hazards(30, 260, 260, 46, h, 1, &target) && target == 30);
    assert(gdx_cpu_hazard_interval(GDX_CPU_HAZARD_MAGNET_LEFT, 220, 260, 260, 260, 200, &h[0]));
    assert(fabsf(h[0].max + 130) < 0.001f);
    assert(gdx_cpu_hazard_interval(GDX_CPU_HAZARD_MAGNET_RIGHT, -260, -220, 260, 260, 10, &h[1]));
    assert(fabsf(h[1].min - 250) < 0.001f);
    assert(gdx_cpu_hazard_interval(GDX_CPU_HAZARD_MAGNET_MIDDLE, -40, 40, 200, 400, 200, &h[0]));
    assert(h[0].min == -50 && h[0].max == 250);
    assert(gdx_cpu_avoid_hazards(100, 200, 400, 46, h, 1, &target));
    assert(target < -96 || target > 296);
    h[0].min = -180; h[0].max = -100;
    h[1].min = -90; h[1].max = 0;
    h[2].min = 30; h[2].max = 100;
    assert(gdx_cpu_avoid_hazards(-80, 260, 260, 20, h, 3, &target));
    assert(target < -200 || target > 120);
    assert(gdx_cpu_avoid_hazards(42, 260, 260, 46, NULL, 0, &target) && target == 42);
    assert(!gdx_cpu_avoid_hazards(42, 260, 260, 46, h, 9, &target) && target == 42);
    assert(!gdx_cpu_hazard_interval(99, 0, 1, 260, 260, 80, h));
    assert(!gdx_cpu_avoid_hazards(42, NAN, 260, 46, h, 1, &target));
    test_interval_properties();
    benchmark();
    puts("All CPU hazard interval tests passed");
    return 0;
}
