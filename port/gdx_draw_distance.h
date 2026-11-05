// port/gdx_draw_distance.h -- one place that turns the draw distance settings into the scales the
// game applies (issue #45).
//
// "Draw distance (%)" scales each track's own chunk cull (course.c Course_Draw). Alone it stops
// helping at about 150%: the race camera's far plane (8192, camera.c) clips what the cull lets
// through. "Extended draw distance" lifts the slider to 400% and scales, by the same factor, the far
// plane (which also pushes the fog out, since the fog band sits at the far end of the depth range),
// the distance at which machines stop being drawn (racer.c Racer_Draw), and the course vertex and
// display list budgets that the extra chunks need. With Extended off, or at 100%, every value is
// the stock one.

#ifndef GDX_DRAW_DISTANCE_H
#define GDX_DRAW_DISTANCE_H

#ifdef __cplusplus
extern "C" {
#endif

#define GDX_DRAW_DISTANCE_MIN_PERCENT 100
#define GDX_DRAW_DISTANCE_MAX_PERCENT 200
#define GDX_DRAW_DISTANCE_EXTENDED_MAX_PERCENT 400

typedef struct GdxDrawDistance {
    int percent;        // the slider value after clamping
    float cullScale;    // multiplies the course's far chunk cull
    float farScale;     // multiplies the camera far plane; 1.0f unless Extended
    float machineScale; // multiplies the machine draw cutoff; 1.0f unless Extended
    int bigBudget;      // nonzero: the course may use the enlarged vertex and display list budget
} GdxDrawDistance;

// Pure: percent is the raw slider value, extended the raw checkbox value.
void gdx_draw_distance_resolve(int percent, int extended, GdxDrawDistance* out);

// Reads gEnhancements.Graphics.DrawDistance and gEnhancements.Graphics.ExtendedDrawDistance. Extended
// needs the Expansion Kit build, whose course pool has the larger budget.
void gdx_draw_distance_current(GdxDrawDistance* out);

// gdx_draw_distance_current's farScale, for the camera far plane and the machine draw cutoff.
float gdx_draw_distance_far_scale(void);

#ifdef __cplusplus
}
#endif

#endif
