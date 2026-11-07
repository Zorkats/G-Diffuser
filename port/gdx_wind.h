/* port/gdx_wind.h -- wind on Course Edit tracks.
 *
 * Course Edit stores wind per segment: any control point can hold a wind for the segment that starts there
 * (gdx_course_settings.h). There is no wind for the whole track. This module holds what the race does with it:
 *
 *   - Direction: a fixed compass angle, the way the wind blows. 0 is north, the top of the race map (world -Z),
 *     and 90 is east (world +X), clockwise. Your heading decides whether it is a tailwind, a headwind or a
 *     crosswind.
 *   - Where: only on the segments that have wind. Tunnels block it.
 *   - Push: along the road surface. The part along your heading is halved, so a tailwind or headwind changes
 *     your speed without taking over the race. The side part stops growing near the side speed cap. Strength
 *     grows faster than its percent: 100% (Gale) pushes twice as hard as a straight ramp would, and its side
 *     speed cap is twice as high. Grip decides how hard it pushes: twice as hard airborne, and up to twice as
 *     hard while sliding sideways.
 *   - Streaks: squashed cloud puffs drift across each player's view in the wind's screen direction, toward the
 *     center for a tailwind and out from it for a headwind. The cloud art is the stock texture from the player's
 *     own ROM; no pixel data is kept in the repo.
 *   - Road arrows: a plate of chevrons painted on the road of each windy segment, pointing the way it blows, one
 *     chevron for each 25 percent. The art is drawn here in code.
 */
#ifndef GDX_WIND_H
#define GDX_WIND_H

#ifdef __cplusplus
extern "C" {
#endif

/* Strength is a percent, 1..100. */
#define GDX_WIND_STRENGTH_MAX 100
#define GDX_WIND_ANGLE_MAX 359

/* Acceleration per frame at 100% before grip, in the race's units. A magnet at its default pulls about 1.0. */
#define GDX_WIND_FORCE_FULL 0.3f
/* Share of the push kept along the heading. */
#define GDX_WIND_ALONG_SHARE 0.5f
/* Sideways speed at which the side push has faded out, before the strength scale. */
#define GDX_WIND_MAX_SIDE_SPEED 5.0f
/* The strength scale at 100%. It ramps from 1 at 0% to this at 100%, and multiplies both the push and the side
 * speed cap, so Gale is twice the old Gale and the lower presets gain less. */
#define GDX_WIND_GALE_SCALE 2.0f
#define GDX_WIND_AIRBORNE_SCALE 2.0f

/* The wind on a segment from its point's stored values: strength 0 (none) or 1..100. blocked is nonzero in a
 * tunnel. Returns the strength 0..100 and writes the angle, 0..359. */
int gdx_wind_resolve(int pointStrength, int pointAngle, int blocked, int* angle);

/* The strength scale for a strength 0..100: 1 + (GDX_WIND_GALE_SCALE - 1) * strength / 100. */
float gdx_wind_strength_scale(int strength);

/* The unit direction the wind blows toward, on the ground plane. */
void gdx_wind_direction(int angle, float* x, float* z);

/* 1 for full grip, up to 2 while sliding (slip is the sideways share of the speed, 0..1), and
 * GDX_WIND_AIRBORNE_SCALE in the air. */
float gdx_wind_grip_scale(int airborne, float slip);

/* One racer's push. strength is 0..100. up is the racer's gravity up, forward its heading and velocity its
 * velocity, all in world space. Writes the acceleration to add this frame; zero for no wind. */
void gdx_wind_push(int strength, int angle, const float up[3], const float forward[3], const float velocity[3],
                   int airborne, float out[3]);

/* Streaks, one set per player view. Positions are 0..1 across the view. */
#define GDX_WIND_VIEWS 4
#define GDX_WIND_STREAKS 14
#define GDX_WIND_STREAK_LIFE 48

typedef struct GdxWindStreakRect {
    float left;
    float top;
    float width;
    float height;
    int alpha; /* 0..255 */
} GdxWindStreakRect;

/* Clears every view. Call at race start. */
void gdx_wind_streaks_reset(void);

/* Moves one view's streaks a frame. strength is 0..100; side is the wind's share toward the view's right
 * (-1..1) and depth its share away from the camera (-1..1). Strength 0 lets the streaks fade out. */
void gdx_wind_streaks_step(int view, int strength, float side, float depth);

/* Streak i of a view, sized for a view viewWidth by viewHeight. Returns 0 when it is not visible. */
int gdx_wind_streak_rect(int view, int i, float viewWidth, float viewHeight, GdxWindStreakRect* out);

/* The camera split used by the step: the share of the wind toward the camera's right and away from it, from
 * the camera's ground heading (forwardX, forwardZ, any length). */
void gdx_wind_view_shares(int angle, float forwardX, float forwardZ, float* side, float* depth);

/* The Course Edit WIND part. Its submenu rows, in order. */
#define GDX_WIND_PART_WEAK 0
#define GDX_WIND_PART_MID 1
#define GDX_WIND_PART_STRONG 2
#define GDX_WIND_PART_GALE 3
#define GDX_WIND_PART_REMOVE 4 /* takes the segment's wind away */
#define GDX_WIND_PART_ROWS 5

/* The point strength a row places: 25, 50, 75 or 100, or 0 (no wind) for Remove and any row out of range. */
int gdx_wind_part_strength(int row);

/* The compass angle 0..359 the ground direction (world x, z) points to, or -1 when it is shorter than
 * minLength. The inverse of gdx_wind_direction. */
int gdx_wind_angle_of(float x, float z, float minLength);

/* The submenu icon of a row, 24x12 RGBA5551 stored big-endian, two bytes a pixel, like the stock menu icons:
 * one to four chevrons for Weak to Gale. Returns 0 and leaves out alone for Remove, which
 * shows the stock CLEAR label instead, and for a row out of range. */
#define GDX_WIND_ICON_WIDTH 24
#define GDX_WIND_ICON_HEIGHT 12
#define GDX_WIND_ICON_BYTES (GDX_WIND_ICON_WIDTH * GDX_WIND_ICON_HEIGHT * 2)
int gdx_wind_part_icon(int row, unsigned char* out);

/* Road arrows. How many chevrons a strength shows: 0 for none, else one for each started 25 percent, up to 4. */
int gdx_wind_chevrons(int strength);

/* The plate on the road around center, lying on the plane whose normal is up, its length along the wind turned
 * onto that plane. out[0] and out[1] are the front corners, the way the wind blows, out[2] and out[3] the back
 * ones; out[0] and out[2] are on the same side. Returns 0 and leaves out alone when the road faces the wind too
 * squarely to lay a plate on it. */
int gdx_wind_road_quad(int angle, const float center[3], const float up[3], float halfLength, float halfWidth,
                       float out[4][3]);

/* The plate's art, RGBA5551 stored big-endian, two bytes a pixel: count chevrons (1..4) pointing toward row 0,
 * pale with a dark edge on a clear background. Returns 0 and leaves out alone for any other count. */
#define GDX_WIND_ROAD_TEX_WIDTH 32
#define GDX_WIND_ROAD_TEX_HEIGHT 48
#define GDX_WIND_ROAD_TEX_BYTES (GDX_WIND_ROAD_TEX_WIDTH * GDX_WIND_ROAD_TEX_HEIGHT * 2)
int gdx_wind_road_texture(int count, unsigned char* out);

#ifdef __cplusplus
}
#endif

#endif /* GDX_WIND_H */
