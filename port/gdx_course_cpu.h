#ifndef GDX_COURSE_CPU_H
#define GDX_COURSE_CPU_H

#ifdef __cplusplus
extern "C" {
#endif

#define GDX_CPU_MAX_HAZARDS 8
#define GDX_CPU_HAZARD_STRIP 0
#define GDX_CPU_HAZARD_MAGNET_LEFT 1
#define GDX_CPU_HAZARD_MAGNET_RIGHT 2
#define GDX_CPU_HAZARD_MAGNET_MIDDLE 3

typedef struct GdxCpuHazard {
    float min;
    float max;
} GdxCpuHazard;

int gdx_cpu_hazard_interval(int kind, float rightEdge, float leftEdge, float radiusLeft,
                            float radiusRight, float range, GdxCpuHazard* out);
int gdx_cpu_avoid_hazards(float desired, float radiusLeft, float radiusRight, float margin,
                        const GdxCpuHazard* hazards, int count, float* target);

#ifdef __cplusplus
}
#endif
#endif
