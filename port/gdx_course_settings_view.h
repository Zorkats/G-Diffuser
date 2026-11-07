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

/* The dash pad at a point: GDX_SETTINGS_VIEW_PAD_*, its width percent (0 for the stock plate), and a Super pad's
 * speed floor as a multiple of the stock dash (1.1 to 3.0; 1.0 for any other pad). */
#define GDX_SETTINGS_VIEW_PAD_NORMAL 0
#define GDX_SETTINGS_VIEW_PAD_SUPER 1
#define GDX_SETTINGS_VIEW_PAD_BRAKE 2
int gdx_settings_view_pad_kind(int point);
int gdx_settings_view_pad_width(int point);
float gdx_settings_view_pad_boost(int point);

/* A point's wind, as stored (gdx_wind_resolve applies it): strength 0 (none) or 1..100, and the compass angle
 * 0..359. Wind is per segment only. */
int gdx_settings_view_wind_strength(int point);
int gdx_settings_view_wind_angle(int point);
/* 1 when any point has wind, so the race loads the streak art. */
int gdx_settings_view_wind_any(void);

/* The multiplier the track's CPU aggression preset puts on the AI aggression option: 1 with no preset. Normal gives 1
 * here too, but it replaces the option, so the race uses gdx_settings_view_ai_resolve. */
float gdx_settings_view_ai_scale(void);

/* The AI aggression scale for the race: the track's preset applied to optionScale, the player's option as a scale
 * (the option as a fraction of 100). Track keeps optionScale and Normal is 1 whatever it is. No preset, and no
 * active settings, keep optionScale. */
float gdx_settings_view_ai_resolve(float optionScale);

#ifdef __cplusplus
}
#endif

#endif /* GDX_COURSE_SETTINGS_VIEW_H */
