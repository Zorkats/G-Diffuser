/* port/gdx_wind.c -- see gdx_wind.h. */

#include "gdx_wind.h"

#include <math.h>
#include <string.h>

#define WIND_PI 3.14159265358979f

static int wrap_angle(int angle) {
    angle %= 360;
    return (angle < 0) ? (angle + 360) : angle;
}

int gdx_wind_resolve(int pointStrength, int pointAngle, int blocked, int* angle) {
    if (blocked || (pointStrength <= 0)) {
        return 0;
    }
    if (angle != NULL) {
        *angle = wrap_angle(pointAngle);
    }
    return (pointStrength > GDX_WIND_STRENGTH_MAX) ? GDX_WIND_STRENGTH_MAX : pointStrength;
}

float gdx_wind_strength_scale(int strength) {
    if (strength <= 0) {
        return 1.0f;
    }
    if (strength > GDX_WIND_STRENGTH_MAX) {
        strength = GDX_WIND_STRENGTH_MAX;
    }
    return 1.0f + ((GDX_WIND_GALE_SCALE - 1.0f) * ((float) strength / (float) GDX_WIND_STRENGTH_MAX));
}

void gdx_wind_direction(int angle, float* x, float* z) {
    const float radians = (float) wrap_angle(angle) * (WIND_PI / 180.0f);

    *x = sinf(radians);
    *z = -cosf(radians);
}

float gdx_wind_grip_scale(int airborne, float slip) {
    if (airborne) {
        return GDX_WIND_AIRBORNE_SCALE;
    }
    if (slip < 0.0f) {
        slip = 0.0f;
    } else if (slip > 1.0f) {
        slip = 1.0f;
    }
    return 1.0f + slip;
}

static float dot3(const float a[3], const float b[3]) {
    return (a[0] * b[0]) + (a[1] * b[1]) + (a[2] * b[2]);
}

/* v minus its part along the unit vector n. */
static void flatten(const float v[3], const float n[3], float out[3]) {
    const float d = dot3(v, n);

    out[0] = v[0] - (d * n[0]);
    out[1] = v[1] - (d * n[1]);
    out[2] = v[2] - (d * n[2]);
}

void gdx_wind_push(int strength, int angle, const float up[3], const float forward[3], const float velocity[3],
                   int airborne, float out[3]) {
    float wind[3];
    float heading[3];
    float along[3];
    float side[3];
    float flatVelocity[3];
    float headingLength;
    float sideLength;
    float sideFade = 1.0f;
    float slip = 0.0f;
    float speed;
    float force;
    float alongShare;
    float scale;
    float sideCap;
    int i;

    out[0] = out[1] = out[2] = 0.0f;
    if (strength <= 0) {
        return;
    }
    if (strength > GDX_WIND_STRENGTH_MAX) {
        strength = GDX_WIND_STRENGTH_MAX;
    }
    scale = gdx_wind_strength_scale(strength);
    sideCap = GDX_WIND_MAX_SIDE_SPEED * scale;
    gdx_wind_direction(angle, &wind[0], &wind[2]);
    wind[1] = 0.0f;
    flatten(wind, up, wind);

    flatten(forward, up, heading);
    headingLength = sqrtf(dot3(heading, heading));
    if (headingLength > 1e-6f) {
        for (i = 0; i < 3; i++) {
            heading[i] /= headingLength;
        }
    } else {
        heading[0] = heading[1] = heading[2] = 0.0f;
    }
    alongShare = dot3(wind, heading);
    for (i = 0; i < 3; i++) {
        along[i] = alongShare * heading[i];
        side[i] = wind[i] - along[i];
    }

    /* Slip: the share of the speed on the road plane that is not along the heading. */
    flatten(velocity, up, flatVelocity);
    speed = sqrtf(dot3(flatVelocity, flatVelocity));
    if (speed > 1e-4f) {
        const float forwardSpeed = dot3(flatVelocity, heading);
        const float sideSquared = (speed * speed) - (forwardSpeed * forwardSpeed);

        slip = (sideSquared > 0.0f) ? (sqrtf(sideSquared) / speed) : 0.0f;
    }

    /* The side push fades as the machine already moves that way, like a magnet's pull. */
    sideLength = sqrtf(dot3(side, side));
    if (sideLength > 1e-6f) {
        const float sideSpeed = dot3(velocity, side) / sideLength;

        if (sideSpeed >= sideCap) {
            sideFade = 0.0f;
        } else if (sideSpeed > 0.0f) {
            sideFade = 1.0f - (sideSpeed / sideCap);
        }
    }

    force = GDX_WIND_FORCE_FULL * ((float) strength / (float) GDX_WIND_STRENGTH_MAX) * scale *
            gdx_wind_grip_scale(airborne, slip);
    for (i = 0; i < 3; i++) {
        out[i] = force * ((GDX_WIND_ALONG_SHARE * along[i]) + (sideFade * side[i]));
    }
}

void gdx_wind_view_shares(int angle, float forwardX, float forwardZ, float* side, float* depth) {
    const float length = sqrtf((forwardX * forwardX) + (forwardZ * forwardZ));
    float x;
    float z;

    *side = 0.0f;
    *depth = 0.0f;
    if (length < 1e-6f) {
        return;
    }
    forwardX /= length;
    forwardZ /= length;
    gdx_wind_direction(angle, &x, &z);
    /* The camera's right on the ground is (-forwardZ, forwardX), as the race map and guLookAt agree. */
    *side = (x * -forwardZ) + (z * forwardX);
    *depth = (x * forwardX) + (z * forwardZ);
}

/* --- streaks ------------------------------------------------------------------------------- */

typedef struct Streak {
    float x;
    float y;
    float length; /* share of the view's width */
    int age;      /* GDX_WIND_STREAK_LIFE or more: not shown */
} Streak;

typedef struct View {
    Streak streaks[GDX_WIND_STREAKS];
    float strength; /* eased toward the wind under the player, 0..1 */
    unsigned int seed;
} View;

static View sViews[GDX_WIND_VIEWS];

static float next_random(View* view) {
    view->seed = (view->seed * 1103515245u) + 12345u;
    return (float) ((view->seed >> 8) & 0xFFFF) / 65535.0f;
}

void gdx_wind_streaks_reset(void) {
    int v;
    int i;

    memset(sViews, 0, sizeof(sViews));
    for (v = 0; v < GDX_WIND_VIEWS; v++) {
        sViews[v].seed = 0x2545F491u + (unsigned int) v * 7919u;
        for (i = 0; i < GDX_WIND_STREAKS; i++) {
            sViews[v].streaks[i].age = GDX_WIND_STREAK_LIFE;
        }
    }
}

static void spawn(View* view, Streak* streak, int staggered) {
    streak->x = 0.05f + (0.9f * next_random(view));
    streak->y = 0.08f + (0.72f * next_random(view));
    streak->length = 0.08f + (0.08f * next_random(view));
    /* Staggered ages keep the first streaks from fading in together. */
    streak->age = staggered ? (int) (next_random(view) * (float) (GDX_WIND_STREAK_LIFE / 2)) : 0;
}

void gdx_wind_streaks_step(int view, int strength, float side, float depth) {
    View* state;
    float target;
    float speed;
    int i;

    if ((view < 0) || (view >= GDX_WIND_VIEWS)) {
        return;
    }
    state = &sViews[view];
    if (state->seed == 0) {
        gdx_wind_streaks_reset();
    }
    target = (strength <= 0) ? 0.0f : ((float) strength / (float) GDX_WIND_STRENGTH_MAX);
    if (target > 1.0f) {
        target = 1.0f;
    }
    state->strength += (target - state->strength) * 0.1f;
    if (state->strength < 0.01f) {
        state->strength = (target > 0.0f) ? state->strength : 0.0f;
    }
    speed = 0.004f + (0.012f * state->strength);
    for (i = 0; i < GDX_WIND_STREAKS; i++) {
        Streak* streak = &state->streaks[i];

        if (streak->age >= GDX_WIND_STREAK_LIFE) {
            if (target > 0.0f) {
                spawn(state, streak, state->strength < 0.5f);
            }
            continue;
        }
        streak->x += side * speed;
        /* A tailwind (depth > 0) blows away from the camera, so the streaks recede toward the center. */
        streak->x -= (streak->x - 0.5f) * depth * speed * 2.0f;
        streak->y -= (streak->y - 0.5f) * depth * speed * 2.0f;
        streak->age++;
        if ((streak->x < -0.1f) || (streak->x > 1.1f) || (streak->y < 0.0f) || (streak->y > 0.95f)) {
            streak->age = GDX_WIND_STREAK_LIFE;
        }
    }
}

int gdx_wind_streak_rect(int view, int i, float viewWidth, float viewHeight, GdxWindStreakRect* out) {
    const View* state;
    const Streak* streak;
    float fade;
    float width;
    int alpha;

    if ((out == NULL) || (view < 0) || (view >= GDX_WIND_VIEWS) || (i < 0) || (i >= GDX_WIND_STREAKS)) {
        return 0;
    }
    state = &sViews[view];
    streak = &state->streaks[i];
    if ((streak->age >= GDX_WIND_STREAK_LIFE) || (state->strength <= 0.0f)) {
        return 0;
    }
    /* Fades in over the first quarter of its life and out over the last half. */
    if (streak->age < GDX_WIND_STREAK_LIFE / 4) {
        fade = (float) (streak->age + 1) / (float) (GDX_WIND_STREAK_LIFE / 4);
    } else if (streak->age > GDX_WIND_STREAK_LIFE / 2) {
        fade = (float) (GDX_WIND_STREAK_LIFE - streak->age) / (float) (GDX_WIND_STREAK_LIFE / 2);
    } else {
        fade = 1.0f;
    }
    alpha = (int) (fade * (60.0f + (110.0f * state->strength)));
    if (alpha <= 0) {
        return 0;
    }
    width = streak->length * viewWidth * (0.6f + (0.4f * state->strength));
    out->width = width;
    out->height = width / 10.0f;
    if (out->height < 2.0f) {
        out->height = 2.0f;
    }
    out->left = (streak->x * viewWidth) - (width * 0.5f);
    out->top = (streak->y * viewHeight) - (out->height * 0.5f);
    out->alpha = (alpha > 255) ? 255 : alpha;
    return 1;
}

int gdx_wind_part_strength(int row) {
    static const int kStrengths[GDX_WIND_PART_ROWS] = { 25, 50, 75, 100, 0 };

    if ((row < 0) || (row >= GDX_WIND_PART_ROWS)) {
        return 0;
    }
    return kStrengths[row];
}

int gdx_wind_angle_of(float x, float z, float minLength) {
    float length = sqrtf((x * x) + (z * z));
    int angle;

    if ((length <= 0.0f) || (length < minLength)) {
        return -1;
    }
    /* gdx_wind_direction gives x = sin and z = -cos of the angle. */
    angle = (int) floorf((atan2f(x, -z) * (180.0f / WIND_PI)) + 0.5f);
    return wrap_angle(angle);
}

/* Icon colors, RGBA5551: a pale sky blue drawn over a dark blue outline, so it reads on the gold rows. */
#define WIND_ICON_RGBA5551(r, g, b) ((unsigned short) (((r) << 11) | ((g) << 6) | ((b) << 1) | 1))
#define WIND_ICON_INK WIND_ICON_RGBA5551(26, 30, 31)
#define WIND_ICON_EDGE WIND_ICON_RGBA5551(2, 6, 14)
#define WIND_CHEVRON_WIDTH 5

/* row + 1 chevrons pointing right, two pixels thick, on rows 2 to 9 of a submenu icon. */
static int wind_icon_ink(int row, int x, int y) {
    int count = row + 1;
    int start = (GDX_WIND_ICON_WIDTH - (count * WIND_CHEVRON_WIDTH)) / 2;
    int k;
    int reach;

    if ((y < 2) || (y > 9)) {
        return 0;
    }
    reach = (y < 6) ? (y - 2) : (9 - y); /* 0 at the ends, 3 at the middle rows */
    for (k = 0; k < count; k++) {
        int tip = start + (k * WIND_CHEVRON_WIDTH) + reach;

        if ((x == tip) || (x == tip + 1)) {
            return 1;
        }
    }
    return 0;
}

/* Paints the ink where ink(shape, x, y) holds and the dark edge on clear pixels touching it, RGBA5551 stored
 * big-endian. */
static void wind_paint(int shape, int width, int height, int (*ink)(int, int, int), unsigned char* out) {
    int x;
    int y;

    for (y = 0; y < height; y++) {
        for (x = 0; x < width; x++) {
            unsigned short pixel = 0;

            if (ink(shape, x, y)) {
                pixel = WIND_ICON_INK;
            } else {
                int dx;
                int dy;

                for (dy = -1; (dy <= 1) && (pixel == 0); dy++) {
                    for (dx = -1; dx <= 1; dx++) {
                        int nx = x + dx;
                        int ny = y + dy;

                        if ((nx >= 0) && (nx < width) && (ny >= 0) && (ny < height) && ink(shape, nx, ny)) {
                            pixel = WIND_ICON_EDGE;
                            break;
                        }
                    }
                }
            }
            out[((y * width) + x) * 2] = (unsigned char) (pixel >> 8);
            out[(((y * width) + x) * 2) + 1] = (unsigned char) (pixel & 0xFF);
        }
    }
}

int gdx_wind_part_icon(int row, unsigned char* out) {
    if ((out == NULL) || (row < 0) || (row > GDX_WIND_PART_GALE)) {
        return 0;
    }
    wind_paint(row, GDX_WIND_ICON_WIDTH, GDX_WIND_ICON_HEIGHT, wind_icon_ink, out);
    return 1;
}

int gdx_wind_chevrons(int strength) {
    if (strength <= 0) {
        return 0;
    }
    if (strength > GDX_WIND_STRENGTH_MAX) {
        strength = GDX_WIND_STRENGTH_MAX;
    }
    return (strength + 24) / 25;
}

int gdx_wind_road_quad(int angle, const float center[3], const float up[3], float halfLength, float halfWidth,
                       float out[4][3]) {
    float wind[3];
    float normal[3];
    float front[3];
    float side[3];
    float length;
    float along;
    int k;

    length = sqrtf(dot3(up, up));
    if (length <= 0.0f) {
        return 0;
    }
    for (k = 0; k < 3; k++) {
        normal[k] = up[k] / length;
    }
    gdx_wind_direction(angle, &wind[0], &wind[2]);
    wind[1] = 0.0f;
    /* The wind turned onto the road's plane. */
    along = dot3(wind, normal);
    for (k = 0; k < 3; k++) {
        front[k] = wind[k] - (along * normal[k]);
    }
    length = sqrtf(dot3(front, front));
    if (length < 0.2f) {
        return 0;
    }
    for (k = 0; k < 3; k++) {
        front[k] /= length;
    }
    side[0] = (normal[1] * front[2]) - (normal[2] * front[1]);
    side[1] = (normal[2] * front[0]) - (normal[0] * front[2]);
    side[2] = (normal[0] * front[1]) - (normal[1] * front[0]);
    for (k = 0; k < 3; k++) {
        out[0][k] = center[k] + (front[k] * halfLength) - (side[k] * halfWidth);
        out[1][k] = center[k] + (front[k] * halfLength) + (side[k] * halfWidth);
        out[2][k] = center[k] - (front[k] * halfLength) - (side[k] * halfWidth);
        out[3][k] = center[k] - (front[k] * halfLength) + (side[k] * halfWidth);
    }
    return 1;
}

/* Road plate chevrons: each a V with its point toward row 0, spread evenly down the plate. */
#define WIND_ROAD_REACH 12.0f /* columns either side of the middle an arm reaches */
#define WIND_ROAD_SLOPE 0.45f /* rows an arm drops for each column out from the middle */
#define WIND_ROAD_THICK 4.0f
#define WIND_ROAD_HEIGHT (WIND_ROAD_THICK + (WIND_ROAD_REACH * WIND_ROAD_SLOPE))

static int wind_road_ink(int count, int x, int y) {
    const float pitch = (float) GDX_WIND_ROAD_TEX_HEIGHT / (float) count;
    const float dx = fabsf(((float) x + 0.5f) - (GDX_WIND_ROAD_TEX_WIDTH * 0.5f));
    int k;

    if (dx > WIND_ROAD_REACH) {
        return 0;
    }
    for (k = 0; k < count; k++) {
        const float point = ((float) k * pitch) + ((pitch - WIND_ROAD_HEIGHT) * 0.5f);
        const float depth = ((float) y + 0.5f) - point - (dx * WIND_ROAD_SLOPE);

        if ((depth >= 0.0f) && (depth < WIND_ROAD_THICK)) {
            return 1;
        }
    }
    return 0;
}

int gdx_wind_road_texture(int count, unsigned char* out) {
    if ((out == NULL) || (count < 1) || (count > 4)) {
        return 0;
    }
    wind_paint(count, GDX_WIND_ROAD_TEX_WIDTH, GDX_WIND_ROAD_TEX_HEIGHT, wind_road_ink, out);
    return 1;
}
