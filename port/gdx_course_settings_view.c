/* port/gdx_course_settings_view.c -- see gdx_course_settings_view.h. */

#include "gdx_course_settings_view.h"

#include "gdx_course_settings.h"

#include <string.h>

typedef char GdxSettingsViewLavaKillsMatches[(GDX_SETTINGS_VIEW_LAVA_KILLS == GDX_SETTINGS_LAVA_KILLS) ? 1 : -1];
typedef char GdxSettingsViewLavaNeverMatches[(GDX_SETTINGS_VIEW_LAVA_NEVER_KILLS == GDX_SETTINGS_LAVA_NEVER_KILLS) ? 1 : -1];

static const GdxPointExtras* view_point(int point) {
    const GdxCourseSettings* settings = gdx_settings_active();

    if ((settings == NULL) || (point < 0) || (point >= settings->ringCount)) {
        return NULL;
    }
    return &settings->ring[point];
}

float gdx_settings_view_part_edge(float stockDistance, int percent, int shape, float width, float nextWidth) {
    const float halfWidth = ((width < nextWidth) ? width : nextWidth) / 2.0f;
    const float cover = (float) percent / 100.0f;

    if (percent <= 0) {
        return stockDistance;
    }
    switch (shape) {
        case GDX_SETTINGS_VIEW_SHAPE_MIDDLE:
            return (percent >= 100) ? 5000.0f : (cover * halfWidth);
        case GDX_SETTINGS_VIEW_SHAPE_BOTH:
            return halfWidth * (1.0f - cover);
        default:
            return (percent >= 100) ? -5000.0f : (halfWidth * (1.0f - (2.0f * cover)));
    }
}

void gdx_settings_view_select_course(const char* courseId, int ringCount) {
    static GdxCourseSettings sLoaded;

    if ((courseId == NULL) || !gdx_settings_store_get(courseId, &sLoaded)) {
        gdx_settings_set_active(NULL);
        return;
    }
    if (sLoaded.ringCount != ringCount) {
        memset(sLoaded.ring, 0, sizeof(sLoaded.ring));
        memset(sLoaded.nodes, 0, sizeof(sLoaded.nodes));
        sLoaded.ringCount = 0;
    }
    gdx_settings_set_active(&sLoaded);
}

int gdx_settings_view_pit_width(int point) {
    const GdxPointExtras* extras = view_point(point);

    return (extras != NULL) ? extras->pitWidth : 0;
}

int gdx_settings_view_dirt_width(int point) {
    const GdxPointExtras* extras = view_point(point);

    return (extras != NULL) ? extras->dirtWidth : 0;
}

int gdx_settings_view_ice_width(int point) {
    const GdxPointExtras* extras = view_point(point);

    return (extras != NULL) ? extras->iceWidth : 0;
}

int gdx_settings_view_lava_mode(void) {
    const GdxCourseSettings* settings = gdx_settings_active();

    return (settings != NULL) ? settings->lavaMode : GDX_SETTINGS_LAVA_FOLLOW_MENU;
}

float gdx_settings_view_magnet_strength(int point) {
    const GdxCourseSettings* settings = gdx_settings_active();
    const GdxPointExtras* extras = view_point(point);
    int stored = 0;

    if (settings != NULL) {
        stored = ((extras != NULL) && (extras->magnetStrength != 0)) ? extras->magnetStrength : settings->magnetStrength;
    }
    return (float) stored / GDX_SETTINGS_STRENGTH_SCALE;
}

float gdx_settings_view_magnet_range(int point) {
    const GdxCourseSettings* settings = gdx_settings_active();
    const GdxPointExtras* extras = view_point(point);

    if (settings == NULL) {
        return 0.0f;
    }
    return (float) (((extras != NULL) && (extras->magnetRange != 0)) ? extras->magnetRange : settings->magnetRange);
}
