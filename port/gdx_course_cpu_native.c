#include "global.h"
#include "fzx_course.h"
#include "fzx_racer.h"
#include "gdx_course_branch.h"
#include "gdx_course_cpu.h"
#include "gdx_course_settings_view.h"

extern CourseEffectsInfo gCourseEffectsInfo;
extern Effect gEffects[];
extern s32 CVarGetInteger(const char* name, s32 defaultValue);
extern f32 CVarGetFloat(const char* name, f32 defaultValue);

void gdx_course_cpu_target(Racer* racer) {
    CourseSegment* segment = racer->segmentPositionInfo.courseSegment;
    GdxCpuHazard hazards[GDX_CPU_MAX_HAZARDS];
    s32 first;
    s32 end;
    s32 i;
    s32 count = 0;
    f32 range = 0.0f;
    f32 target;
    s32 kind;
    s32 shape = segment->trackSegmentInfo & TRACK_SHAPE_MASK;

    if (shape != TRACK_SHAPE_ROAD && shape != TRACK_SHAPE_BORDERLESS_ROAD && shape != TRACK_SHAPE_WALLED_ROAD) {
        return;
    }
    if (segment->effectsStart == NULL || segment->effectsEnd == NULL) {
        return;
    }
    if ((u64) segment->effectsStart < (u64) gEffects || (u64) segment->effectsEnd < (u64) segment->effectsStart ||
        (u64) segment->effectsEnd > (u64) (gEffects + 192)) {
        return;
    }
    first = (s32) (segment->effectsStart - gEffects);
    end = (s32) (segment->effectsEnd - gEffects);
    if (end > gCourseEffectsInfo.count) {
        return;
    }
    if (!CVarGetInteger("gEnhancements.Gameplay.IgnoreTrackSettings", 0)) {
        range = gdx_settings_view_magnet_range(gdx_course_branch_settings_point(segment));
    }
    if (!(range > 0.0f)) {
        range = CVarGetFloat("gEnhancements.Gameplay.MagnetPullRange", 80.0f);
    }
    if (!(range >= 10.0f)) {
        range = 10.0f;
    } else if (range > 200.0f) {
        range = 200.0f;
    }
    for (i = first; i < end; i++) {
        const CourseEffect* effect = &gCourseEffectsInfo.effects[i];

        if (effect->segmentTValueEnd < racer->segmentPositionInfo.segmentTValue) {
            continue;
        }
        switch (effect->effectType) {
            case COURSE_EFFECT_DIRT:
            case COURSE_EFFECT_ICE:
#ifdef EXPANSION_KIT
            case COURSE_EFFECT_LAVA:
#endif
                kind = GDX_CPU_HAZARD_STRIP;
                break;
#ifdef EXPANSION_KIT
            case COURSE_EFFECT_MAGNET_LEFT:
                kind = GDX_CPU_HAZARD_MAGNET_LEFT;
                break;
            case COURSE_EFFECT_MAGNET_RIGHT:
                kind = GDX_CPU_HAZARD_MAGNET_RIGHT;
                break;
            case COURSE_EFFECT_MAGNET_MIDDLE:
                kind = GDX_CPU_HAZARD_MAGNET_MIDDLE;
                break;
#endif
            default:
                continue;
        }
        if (count == GDX_CPU_MAX_HAZARDS || !gdx_cpu_hazard_interval(kind, effect->rightEdgeDistance,
                effect->leftEdgeDistance, racer->currentRadiusLeft, racer->currentRadiusRight, range, &hazards[count])) {
            return;
        }
        count++;
    }
    if (gdx_cpu_avoid_hazards(racer->unk_33C, racer->currentRadiusLeft, racer->currentRadiusRight,
                             46.0f, hazards, count, &target)) {
        racer->unk_33C = target;
    }
}
