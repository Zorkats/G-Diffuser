// port/gdx_draw_distance.c -- see gdx_draw_distance.h.

#include "gdx_draw_distance.h"

#include <stddef.h>

extern int CVarGetInteger(const char* name, int defaultValue); // libultraship consolevariablebridge.h

void gdx_draw_distance_resolve(int percent, int extended, GdxDrawDistance* out) {
    const int max = extended ? GDX_DRAW_DISTANCE_EXTENDED_MAX_PERCENT : GDX_DRAW_DISTANCE_MAX_PERCENT;

    if (out == NULL) {
        return;
    }
    // The CVars can be hand-edited, so the range is enforced here and not only by the slider.
    if (percent < GDX_DRAW_DISTANCE_MIN_PERCENT) {
        percent = GDX_DRAW_DISTANCE_MIN_PERCENT;
    } else if (percent > max) {
        percent = max;
    }
    out->percent = percent;
    // 100 / 100.0f is exactly 1.0f, and multiplying by 1.0f is exact, so 100% stays bit-identical.
    out->cullScale = (float) percent / 100.0f;
    out->farScale = extended ? out->cullScale : 1.0f;
    out->machineScale = out->farScale;
    // At 100% even Extended keeps the stock budget, so the stock "drop chunks when full" rule holds.
    out->bigBudget = extended && (percent > GDX_DRAW_DISTANCE_MIN_PERCENT);
}

void gdx_draw_distance_current(GdxDrawDistance* out) {
    int extended = 0;

#ifdef EXPANSION_KIT
    extended = CVarGetInteger("gEnhancements.Graphics.ExtendedDrawDistance", 0) != 0;
#endif
    gdx_draw_distance_resolve(CVarGetInteger("gEnhancements.Graphics.DrawDistance", GDX_DRAW_DISTANCE_MIN_PERCENT),
                              extended, out);
}

float gdx_draw_distance_far_scale(void) {
    GdxDrawDistance current;

    gdx_draw_distance_current(&current);
    return current.farScale;
}
