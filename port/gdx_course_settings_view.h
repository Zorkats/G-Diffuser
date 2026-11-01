/* port/gdx_course_settings_view.h -- gdx_course_settings.h in plain C types, for the decomp side.
 *
 * Like gdx_course_branch_view.h, nothing here includes <stddef.h> or <stdint.h>, so global.h includers can use
 * it. Points are control point indices: the race rotates its arrays so the start segment is index 0, so the
 * caller maps a race segment back first (gdx_course_branch_control_point).
 */

#ifndef GDX_COURSE_SETTINGS_VIEW_H
#define GDX_COURSE_SETTINGS_VIEW_H

#ifdef __cplusplus
extern "C" {
#endif

/* Makes courseId's stored values the active ones. Values saved for another ring length keep only the values
 * for the whole track. NULL clears. */
void gdx_settings_view_select_course(const char* courseId, int ringCount);

/* Percent of the road, 1..100, or 0 when the point has no width of its own. */
int gdx_settings_view_pit_width(int point);
int gdx_settings_view_dirt_width(int point);
int gdx_settings_view_ice_width(int point);

#define GDX_SETTINGS_VIEW_SHAPE_SIDE 0 /* one strip from a road edge */
#define GDX_SETTINGS_VIEW_SHAPE_BOTH 1 /* a strip from each road edge */
#define GDX_SETTINGS_VIEW_SHAPE_MIDDLE 2 /* one strip around the center line */

/* The edge distance a strip is built with, for percent of the road. A side strip grows in from its edge, a pair
 * grows until the two meet on the center line, a middle strip grows out from that line. 100 covers the road
 * whatever its width, since the draw clamps a strip to the road. 0 returns stockDistance. The widths are the full
 * widths of the segment's two ends. */
float gdx_settings_view_part_edge(float stockDistance, int percent, int shape, float width, float nextWidth);

/* GDX_SETTINGS_LAVA_*: 0 follow the menu, 1 lava kills, 2 lava never kills. */
#define GDX_SETTINGS_VIEW_LAVA_KILLS 1
#define GDX_SETTINGS_VIEW_LAVA_NEVER_KILLS 2
int gdx_settings_view_lava_mode(void);

/* The point's own value, else the track's, else 0. */
float gdx_settings_view_magnet_strength(int point);
float gdx_settings_view_magnet_range(int point);

#ifdef __cplusplus
}
#endif

#endif /* GDX_COURSE_SETTINGS_VIEW_H */
