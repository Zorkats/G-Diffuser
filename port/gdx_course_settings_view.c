/* port/gdx_course_settings_view.c -- see gdx_course_settings_view.h. */

#include "gdx_course_settings_view.h"

#include "gdx_course_settings.h"
#include "gdx_course_branch_view.h"

#include <string.h>

typedef char GdxSettingsViewLavaKillsMatches[(GDX_SETTINGS_VIEW_LAVA_KILLS == GDX_SETTINGS_LAVA_KILLS) ? 1 : -1];
typedef char GdxSettingsViewLavaNeverMatches[(GDX_SETTINGS_VIEW_LAVA_NEVER_KILLS == GDX_SETTINGS_LAVA_NEVER_KILLS) ? 1 : -1];
typedef char GdxSettingsViewPadSuperMatches[(GDX_SETTINGS_VIEW_PAD_SUPER == GDX_SETTINGS_PAD_SUPER) ? 1 : -1];
typedef char GdxSettingsViewPadBrakeMatches[(GDX_SETTINGS_VIEW_PAD_BRAKE == GDX_SETTINGS_PAD_BRAKE) ? 1 : -1];

static const GdxPointExtras* view_point(int point) {
    const GdxCourseSettings* settings = gdx_settings_active();

    if (point >= GDX_BRANCH_SETTINGS_BASE) {
        const GdxBranchSet* branches = gdx_branch_active_set();
        int slot = point - GDX_BRANCH_SETTINGS_BASE;
        int fork = slot / GDX_BRANCH_SETTINGS_STRIDE;
        int node = slot % GDX_BRANCH_SETTINGS_STRIDE - 1;

        if (branches == NULL || fork >= branches->forkCount || node >= branches->forks[fork].nodeCount) {
            return NULL;
        }
        if (node == -1) {
            return (branches->forks[fork].flags & GDX_BRANCH_FLAG_FIRST_VALUES) ?
                       &branches->forks[fork].firstExtras : NULL;
        }
        return settings != NULL ? &settings->nodes[fork][node] : NULL;
    }
    if (settings == NULL || point < 0 || point >= settings->ringCount) {
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

    if (extras != NULL && extras->magnetStrength != 0) {
        stored = extras->magnetStrength;
    } else if (settings != NULL) {
        stored = settings->magnetStrength;
    }
    return (float) stored / GDX_SETTINGS_STRENGTH_SCALE;
}

float gdx_settings_view_magnet_range(int point) {
    const GdxCourseSettings* settings = gdx_settings_active();
    const GdxPointExtras* extras = view_point(point);

    if (extras != NULL && extras->magnetRange != 0) {
        return (float) extras->magnetRange;
    }
    return settings != NULL ? (float) settings->magnetRange : 0.0f;
}

int gdx_settings_view_pad_kind(int point) {
    const GdxPointExtras* extras = view_point(point);

    return (extras != NULL) ? extras->padKind : GDX_SETTINGS_PAD_NORMAL;
}

int gdx_settings_view_pad_width(int point) {
    const GdxPointExtras* extras = view_point(point);

    return (extras != NULL) ? extras->padWidth : 0;
}

float gdx_settings_view_pad_boost(int point) {
    const GdxPointExtras* extras = view_point(point);
    int boost;

    if ((extras == NULL) || (extras->padKind != GDX_SETTINGS_PAD_SUPER)) {
        return 1.0f;
    }
    boost = (extras->padBoost != 0) ? extras->padBoost : GDX_SETTINGS_PAD_BOOST_DEFAULT;
    return 1.0f + (float) boost / 100.0f;
}

int gdx_settings_view_wind_strength(int point) {
    const GdxPointExtras* extras = view_point(point);

    return (extras != NULL) ? extras->windStrength : 0;
}

int gdx_settings_view_wind_angle(int point) {
    const GdxPointExtras* extras = view_point(point);

    return (extras != NULL) ? extras->windAngle : 0;
}

int gdx_settings_view_track_wind_strength(void) {
    const GdxCourseSettings* settings = gdx_settings_active();

    return (settings != NULL) ? settings->windStrength : 0;
}

int gdx_settings_view_track_wind_angle(void) {
    const GdxCourseSettings* settings = gdx_settings_active();

    return (settings != NULL) ? settings->windAngle : 0;
}

int gdx_settings_view_road_theme(int point) {
    const GdxCourseSettings* settings = gdx_settings_active();
    const GdxPointExtras* extras = view_point(point);

    if ((extras != NULL) && (extras->roadTheme != 0)) {
        return extras->roadTheme;
    }
    return (settings != NULL) ? settings->roadTheme : 0;
}

int gdx_settings_view_background_count(void) {
    const GdxCourseSettings* settings = gdx_settings_active();

    if (settings == NULL) {
        return 0;
    }
    return (settings->backgroundCount > GDX_SETTINGS_BACKGROUND_MAX) ? GDX_SETTINGS_BACKGROUND_MAX
                                                                    : settings->backgroundCount;
}

int gdx_settings_view_background(int slot, int* image, int* angle) {
    const GdxCourseSettings* settings = gdx_settings_active();

    if ((slot < 0) || (slot >= gdx_settings_view_background_count())) {
        return 0;
    }
    if (image != NULL) {
        *image = settings->backgrounds[slot].image;
    }
    if (angle != NULL) {
        *angle = settings->backgrounds[slot].angle;
    }
    return 1;
}

float gdx_settings_view_ai_scale(void) {
    const GdxCourseSettings* settings = gdx_settings_active();

    return gdx_settings_ai_scale((settings != NULL) ? settings->aiAggression : GDX_SETTINGS_AI_TRACK);
}

float gdx_settings_view_ai_resolve(float optionScale) {
    const GdxCourseSettings* settings = gdx_settings_active();

    return gdx_settings_ai_resolve((settings != NULL) ? settings->aiAggression : GDX_SETTINGS_AI_TRACK,
                                   optionScale);
}

int gdx_settings_view_wind_any(void) {
    const GdxCourseSettings* settings = gdx_settings_active();
    int i;
    int j;

    if (settings == NULL) {
        return 0;
    }
    if (settings->windStrength != 0) {
        return 1;
    }
    for (i = 0; i < settings->ringCount; i++) {
        if (settings->ring[i].windStrength != 0) {
            return 1;
        }
    }
    for (i = 0; i < GDX_BRANCH_MAX_FORKS; i++) {
        for (j = 0; j < GDX_BRANCH_MAX_NODES; j++) {
            if (settings->nodes[i][j].windStrength != 0) {
                return 1;
            }
        }
    }
    return 0;
}
