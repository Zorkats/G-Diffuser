#include "gdx_course_cpu.h"

#include <math.h>

int gdx_cpu_hazard_interval(int kind, float rightEdge, float leftEdge, float radiusLeft,
                            float radiusRight, float range, GdxCpuHazard* out) {
    float center;
    float pull;
    float minimum;

    if (out == 0 || !isfinite(rightEdge) || !isfinite(leftEdge) ||
        !isfinite(radiusLeft) || !isfinite(radiusRight) || !isfinite(range) ||
        radiusLeft <= 0 || radiusRight <= 0 || rightEdge > leftEdge) {
        return 0;
    }
    center = (radiusRight - radiusLeft) * 0.5f;
    minimum = (radiusLeft + radiusRight) * 0.25f;
    switch (kind) {
        case GDX_CPU_HAZARD_STRIP:
            out->min = center - leftEdge;
            out->max = center - rightEdge;
            break;
        case GDX_CPU_HAZARD_MAGNET_LEFT:
        case GDX_CPU_HAZARD_MAGNET_RIGHT:
            pull = (kind == GDX_CPU_HAZARD_MAGNET_LEFT ? rightEdge : -leftEdge) + 40.0f - range;
            if (pull < minimum) {
                pull = minimum;
            }
            out->min = kind == GDX_CPU_HAZARD_MAGNET_LEFT ? -radiusLeft : center + pull;
            out->max = kind == GDX_CPU_HAZARD_MAGNET_LEFT ? center - pull : radiusRight;
            break;
        case GDX_CPU_HAZARD_MAGNET_MIDDLE:
            pull = range < minimum ? range : minimum;
            out->min = center - pull;
            out->max = center + pull;
            break;
        default:
            return 0;
    }
    return out->min <= out->max;
}

static int safe_target(float value, const GdxCpuHazard* hazards, int count, float margin) {
    int i;

    for (i = 0; i < count; i++) {
        if (value >= hazards[i].min - margin && value <= hazards[i].max + margin) {
            return 0;
        }
    }
    return 1;
}

int gdx_cpu_avoid_hazards(float desired, float radiusLeft, float radiusRight, float margin,
                        const GdxCpuHazard* hazards, int count, float* target) {
    float min;
    float max;
    float best;
    float distance = 0.0f;
    int found = 0;
    int i;
    int edge;

    if (target == 0) {
        return 0;
    }
    *target = desired;
    if (!isfinite(desired) || !isfinite(radiusLeft) || !isfinite(radiusRight) || !isfinite(margin) ||
        margin < 0 || radiusLeft <= 0 || radiusRight <= 0 || count < 0 || count > GDX_CPU_MAX_HAZARDS ||
        (count != 0 && hazards == 0)) {
        return 0;
    }
    min = -radiusLeft + margin;
    max = radiusRight - margin;
    if (min > max) {
        *target = (radiusRight - radiusLeft) * 0.5f;
        return 0;
    }
    if (*target < min) {
        *target = min;
    } else if (*target > max) {
        *target = max;
    }
    for (i = 0; i < count; i++) {
        if (!isfinite(hazards[i].min) || !isfinite(hazards[i].max) || hazards[i].min > hazards[i].max) {
            return 0;
        }
    }
    if (safe_target(*target, hazards, count, margin)) {
        return 1;
    }
    best = *target;
    // A nearest safe point lies just outside one of the expanded hazard intervals.
    for (i = 0; i < count; i++) {
        for (edge = 0; edge < 2; edge++) {
            float value = edge == 0 ? hazards[i].min - margin - 0.01f : hazards[i].max + margin + 0.01f;
            float delta = fabsf(value - *target);

            if (value >= min && value <= max && (!found || delta < distance) &&
                safe_target(value, hazards, count, margin)) {
                best = value;
                distance = delta;
                found = 1;
            }
        }
    }
    *target = best;
    return found;
}
