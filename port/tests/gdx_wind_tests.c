/* Unit tests for port/gdx_wind.c, compiled UNMODIFIED alongside this file. */
#include "../gdx_wind.h"

#include <math.h>
#include <stdio.h>

static int sFailures;
static int sChecks;

#define CHECK(cond, msg)                         \
    do {                                         \
        sChecks++;                               \
        if (!(cond)) {                           \
            sFailures++;                         \
            printf("FAIL: %s (line %d)\n", msg, __LINE__); \
        }                                        \
    } while (0)

static int near(float a, float b, float eps) {
    return fabsf(a - b) <= eps;
}

static float dot3(const float a[3], const float b[3]) {
    return (a[0] * b[0]) + (a[1] * b[1]) + (a[2] * b[2]);
}

static void test_resolve(void) {
    int angle = -1;

    CHECK(gdx_wind_resolve(0, 0, 0, &angle) == 0, "a segment without its own wind has none");
    CHECK(gdx_wind_resolve(70, 200, 0, &angle) == 70 && angle == 200, "a segment's own wind blows");
    CHECK(gdx_wind_resolve(70, 200, 1, &angle) == 0, "a tunnel blocks the wind");
    CHECK(gdx_wind_resolve(150, 10, 0, &angle) == GDX_WIND_STRENGTH_MAX, "strength above 100 is capped");
    CHECK(gdx_wind_resolve(-5, 10, 0, &angle) == 0, "a negative strength is no wind");
    CHECK(gdx_wind_resolve(50, 400, 0, &angle) == 50 && angle == 40, "an angle past 359 wraps");
    CHECK(gdx_wind_resolve(50, -90, 0, &angle) == 50 && angle == 270, "a negative angle wraps");
    CHECK(gdx_wind_resolve(50, 90, 0, NULL) == 50, "the angle pointer may be NULL");
}

static void test_direction(void) {
    float x;
    float z;

    gdx_wind_direction(0, &x, &z);
    CHECK(near(x, 0.0f, 1e-5f) && near(z, -1.0f, 1e-5f), "0 blows north, toward world -Z");
    gdx_wind_direction(90, &x, &z);
    CHECK(near(x, 1.0f, 1e-5f) && near(z, 0.0f, 1e-5f), "90 blows east, toward world +X");
    gdx_wind_direction(180, &x, &z);
    CHECK(near(x, 0.0f, 1e-5f) && near(z, 1.0f, 1e-5f), "180 blows south");
    gdx_wind_direction(270, &x, &z);
    CHECK(near(x, -1.0f, 1e-5f) && near(z, 0.0f, 1e-5f), "270 blows west");
    gdx_wind_direction(45, &x, &z);
    CHECK(near((x * x) + (z * z), 1.0f, 1e-5f), "the direction is a unit vector");
}

static void test_grip(void) {
    CHECK(near(gdx_wind_grip_scale(0, 0.0f), 1.0f, 1e-6f), "full grip is the base push");
    CHECK(near(gdx_wind_grip_scale(0, 0.5f), 1.5f, 1e-6f), "half a slide is half again");
    CHECK(near(gdx_wind_grip_scale(0, 1.0f), 2.0f, 1e-6f), "a full slide doubles the push");
    CHECK(near(gdx_wind_grip_scale(0, 3.0f), 2.0f, 1e-6f), "slip is capped at 1");
    CHECK(near(gdx_wind_grip_scale(0, -1.0f), 1.0f, 1e-6f), "negative slip is 0");
    CHECK(near(gdx_wind_grip_scale(1, 0.0f), GDX_WIND_AIRBORNE_SCALE, 1e-6f), "airborne pushes harder");
    CHECK(near(gdx_wind_grip_scale(1, 1.0f), GDX_WIND_AIRBORNE_SCALE, 1e-6f), "airborne ignores slip");
}

static void test_strength_scale(void) {
    CHECK(near(gdx_wind_strength_scale(0), 1.0f, 1e-6f), "no wind has the base scale");
    CHECK(near(gdx_wind_strength_scale(25), 1.25f, 1e-6f), "Weak scales by 1.25");
    CHECK(near(gdx_wind_strength_scale(50), 1.5f, 1e-6f), "Mid scales by 1.5");
    CHECK(near(gdx_wind_strength_scale(75), 1.75f, 1e-6f), "Strong scales by 1.75");
    CHECK(near(gdx_wind_strength_scale(100), GDX_WIND_GALE_SCALE, 1e-6f), "Gale scales by the Gale scale");
    CHECK(near(gdx_wind_strength_scale(250), GDX_WIND_GALE_SCALE, 1e-6f), "the scale stops at 100%");
    CHECK(near(GDX_WIND_GALE_SCALE, 2.0f, 1e-6f), "Gale is twice the straight ramp");
}

static void test_push(void) {
    const float up[3] = { 0.0f, 1.0f, 0.0f };
    const float north[3] = { 0.0f, 0.0f, -1.0f };
    const float still[3] = { 0.0f, 0.0f, 0.0f };
    const float fastNorth[3] = { 0.0f, 0.0f, -10.0f };
    float out[3];
    const float base = GDX_WIND_FORCE_FULL * GDX_WIND_GALE_SCALE; /* the push at 100% */
    const float cap = GDX_WIND_MAX_SIDE_SPEED * GDX_WIND_GALE_SCALE; /* the side speed cap at 100% */

    gdx_wind_push(0, 0, up, north, still, 0, out);
    CHECK(out[0] == 0.0f && out[1] == 0.0f && out[2] == 0.0f, "no strength, no push");

    gdx_wind_push(100, 0, up, north, still, 0, out);
    CHECK(near(out[2], -base * GDX_WIND_ALONG_SHARE, 1e-5f) && near(out[0], 0.0f, 1e-5f),
          "a tailwind pushes forward at the along share");
    gdx_wind_push(100, 180, up, north, still, 0, out);
    CHECK(near(out[2], base * GDX_WIND_ALONG_SHARE, 1e-5f), "a headwind pushes back at the along share");

    gdx_wind_push(100, 90, up, north, still, 0, out);
    CHECK(near(out[0], base, 1e-5f) && near(out[2], 0.0f, 1e-5f), "a crosswind pushes sideways in full");
    gdx_wind_push(50, 90, up, north, still, 0, out);
    CHECK(near(out[0], GDX_WIND_FORCE_FULL * 0.5f * gdx_wind_strength_scale(50), 1e-5f), "strength scales the push");
    CHECK(near(out[0], GDX_WIND_FORCE_FULL * 0.75f, 1e-5f), "Mid pushes 1.5 times its straight-ramp share");
    gdx_wind_push(150, 90, up, north, still, 0, out);
    CHECK(near(out[0], base, 1e-5f), "strength above 100 pushes like 100");
    gdx_wind_push(100, 90, up, north, still, 1, out);
    CHECK(near(out[0], base * GDX_WIND_AIRBORNE_SCALE, 1e-5f), "airborne doubles the push");

    {
        const float drifting[3] = { cap * 0.5f, 0.0f, 0.0f };

        gdx_wind_push(100, 90, up, north, drifting, 0, out);
        /* All speed is sideways: slip 1 doubles the push, half the side speed cap halves it again. */
        CHECK(near(out[0], base * 2.0f * 0.5f, 1e-4f), "the side push fades as side speed builds");
    }
    {
        const float atCap[3] = { cap, 0.0f, -100.0f };

        gdx_wind_push(100, 90, up, north, atCap, 0, out);
        CHECK(near(out[0], 0.0f, 1e-5f), "no side push at the side speed cap");
    }
    {
        const float against[3] = { -cap, 0.0f, -100.0f };

        gdx_wind_push(100, 90, up, north, against, 0, out);
        CHECK(out[0] > base, "moving against the wind keeps the full side push, with slip");
    }
    {
        float slideOut[3];
        float gripOut[3];
        const float slide[3] = { 1.0f, 0.0f, -1.0f };

        gdx_wind_push(100, 90, up, north, fastNorth, 0, gripOut);
        gdx_wind_push(100, 90, up, north, slide, 0, slideOut);
        CHECK(slideOut[0] > gripOut[0], "sliding pushes harder than holding the line");
    }
    {
        /* On a wall: up is +X, so a wind toward +X is all into the wall and has nothing along the surface. */
        const float wallUp[3] = { 1.0f, 0.0f, 0.0f };

        gdx_wind_push(100, 90, wallUp, north, still, 0, out);
        CHECK(near(dot3(out, out), 0.0f, 1e-8f), "wind into the surface does not push");
        gdx_wind_push(100, 45, up, north, fastNorth, 0, out);
        CHECK(near(out[1], 0.0f, 1e-6f), "the push stays on the road plane");
    }
    {
        /* A banked road: up tilted toward +X. The push has no part along up. */
        const float bankUp[3] = { 0.6f, 0.8f, 0.0f };

        gdx_wind_push(100, 90, bankUp, north, still, 0, out);
        CHECK(near(dot3(out, bankUp), 0.0f, 1e-5f), "the push is along the banked surface");
    }
}

static void test_view_shares(void) {
    float side;
    float depth;

    gdx_wind_view_shares(90, 0.0f, -1.0f, &side, &depth);
    CHECK(near(side, 1.0f, 1e-5f) && near(depth, 0.0f, 1e-5f), "facing north, an east wind blows right");
    gdx_wind_view_shares(0, 0.0f, -2.0f, &side, &depth);
    CHECK(near(side, 0.0f, 1e-5f) && near(depth, 1.0f, 1e-5f), "facing north, a north wind blows away");
    gdx_wind_view_shares(180, 0.0f, -1.0f, &side, &depth);
    CHECK(near(depth, -1.0f, 1e-5f), "facing north, a south wind blows at the camera");
    gdx_wind_view_shares(0, 1.0f, 0.0f, &side, &depth);
    CHECK(near(side, -1.0f, 1e-5f) && near(depth, 0.0f, 1e-5f), "facing east, a north wind blows left");
    gdx_wind_view_shares(0, 0.0f, 0.0f, &side, &depth);
    CHECK(side == 0.0f && depth == 0.0f, "no heading gives no shares");
}

static int visible_count(int view) {
    GdxWindStreakRect rect;
    int count = 0;
    int i;

    for (i = 0; i < GDX_WIND_STREAKS; i++) {
        count += gdx_wind_streak_rect(view, i, 320.0f, 240.0f, &rect);
    }
    return count;
}

/* Warms a view up, then steps it once and counts streaks that moved one way minus the other. radial 0 counts
 * rightward moves, radial 1 counts moves away from the view's center. Respawned streaks jump and are skipped. */
static int drift(int view, float side, float depth, int radial) {
    GdxWindStreakRect before[GDX_WIND_STREAKS];
    int shown[GDX_WIND_STREAKS];
    int score = 0;
    int frame;
    int i;

    gdx_wind_streaks_reset();
    for (frame = 0; frame < 60; frame++) {
        gdx_wind_streaks_step(view, 100, side, depth);
    }
    for (i = 0; i < GDX_WIND_STREAKS; i++) {
        shown[i] = gdx_wind_streak_rect(view, i, 320.0f, 240.0f, &before[i]);
    }
    gdx_wind_streaks_step(view, 100, side, depth);
    for (i = 0; i < GDX_WIND_STREAKS; i++) {
        GdxWindStreakRect after;
        float a;
        float b;

        if (!shown[i] || !gdx_wind_streak_rect(view, i, 320.0f, 240.0f, &after)) {
            continue;
        }
        if (radial) {
            const float ax = before[i].left + before[i].width * 0.5f - 160.0f;
            const float ay = before[i].top + before[i].height * 0.5f - 120.0f;
            const float bx = after.left + after.width * 0.5f - 160.0f;
            const float by = after.top + after.height * 0.5f - 120.0f;

            a = sqrtf(ax * ax + ay * ay);
            b = sqrtf(bx * bx + by * by);
        } else {
            a = before[i].left + before[i].width * 0.5f;
            b = after.left + after.width * 0.5f;
        }
        if (fabsf(b - a) > 40.0f) {
            continue;
        }
        score += (b > a + 1e-4f) ? 1 : (b < a - 1e-4f) ? -1 : 0;
    }
    return score;
}

static void test_streaks(void) {
    GdxWindStreakRect rect;
    int i;
    int frame;
    int ok = 1;

    gdx_wind_streaks_reset();
    CHECK(visible_count(0) == 0, "no streaks after a reset");
    for (frame = 0; frame < 120; frame++) {
        gdx_wind_streaks_step(0, 0, 1.0f, 0.0f);
    }
    CHECK(visible_count(0) == 0, "no wind, no streaks");

    for (frame = 0; frame < 120; frame++) {
        gdx_wind_streaks_step(0, 100, 1.0f, 0.0f);
    }
    CHECK(visible_count(0) > GDX_WIND_STREAKS / 2, "a strong wind shows streaks");
    CHECK(visible_count(1) == 0, "other views stay empty");
    for (i = 0; i < GDX_WIND_STREAKS; i++) {
        if (gdx_wind_streak_rect(0, i, 320.0f, 240.0f, &rect)) {
            ok = ok && (rect.width > rect.height * 4.0f) && (rect.alpha > 0) && (rect.alpha <= 255) &&
                 (rect.width < 320.0f);
        }
    }
    CHECK(ok, "streaks are squashed, faded and smaller than the view");

    CHECK(drift(0, 1.0f, 0.0f, 0) > 0, "a right-blowing wind moves the streaks right");
    CHECK(drift(0, -1.0f, 0.0f, 0) < 0, "a left-blowing wind moves the streaks left");
    CHECK(drift(2, 0.0f, 1.0f, 1) < 0, "a tailwind pulls the streaks toward the center");
    CHECK(drift(2, 0.0f, -1.0f, 1) > 0, "a headwind pushes the streaks out from the center");

    {
        int faded = 0;

        gdx_wind_streaks_reset();
        for (frame = 0; frame < 120; frame++) {
            gdx_wind_streaks_step(1, 100, -1.0f, 0.0f);
        }
        CHECK(visible_count(1) > 0, "a left wind shows streaks too");
        for (frame = 0; frame < GDX_WIND_STREAK_LIFE * 3; frame++) {
            gdx_wind_streaks_step(1, 0, -1.0f, 0.0f);
        }
        faded = visible_count(1) == 0;
        CHECK(faded, "streaks fade out when the wind stops");
    }

    {
        float weak = 0.0f;
        float strong = 0.0f;

        gdx_wind_streaks_reset();
        for (frame = 0; frame < 200; frame++) {
            gdx_wind_streaks_step(0, 20, 1.0f, 0.0f);
            gdx_wind_streaks_step(3, 100, 1.0f, 0.0f);
        }
        for (i = 0; i < GDX_WIND_STREAKS; i++) {
            if (gdx_wind_streak_rect(0, i, 320.0f, 240.0f, &rect)) {
                weak += (float) rect.alpha;
            }
            if (gdx_wind_streak_rect(3, i, 320.0f, 240.0f, &rect)) {
                strong += (float) rect.alpha;
            }
        }
        CHECK(strong > weak, "a stronger wind shows stronger streaks");
    }

    CHECK(gdx_wind_streak_rect(-1, 0, 320.0f, 240.0f, &rect) == 0, "a view out of range shows nothing");
    CHECK(gdx_wind_streak_rect(GDX_WIND_VIEWS, 0, 320.0f, 240.0f, &rect) == 0, "a view past the last shows nothing");
    CHECK(gdx_wind_streak_rect(0, GDX_WIND_STREAKS, 320.0f, 240.0f, &rect) == 0, "a streak past the last is hidden");
    CHECK(gdx_wind_streak_rect(0, 0, 320.0f, 240.0f, NULL) == 0, "a NULL rect is refused");
    gdx_wind_streaks_step(-1, 100, 1.0f, 0.0f);
    gdx_wind_streaks_step(GDX_WIND_VIEWS, 100, 1.0f, 0.0f);
    CHECK(1, "stepping a view out of range is ignored");
}

static void test_part_strength(void) {
    CHECK(gdx_wind_part_strength(GDX_WIND_PART_WEAK) == 25, "Weak places 25 percent");
    CHECK(gdx_wind_part_strength(GDX_WIND_PART_MID) == 50, "Mid places 50 percent");
    CHECK(gdx_wind_part_strength(GDX_WIND_PART_STRONG) == 75, "Strong places 75 percent");
    CHECK(gdx_wind_part_strength(GDX_WIND_PART_GALE) == 100, "Gale places 100 percent");
    CHECK(gdx_wind_part_strength(GDX_WIND_PART_REMOVE) == 0, "Remove takes the segment's wind away");
    CHECK(gdx_wind_part_strength(-1) == 0 && gdx_wind_part_strength(GDX_WIND_PART_ROWS) == 0,
          "a row out of range places nothing of its own");
}

static void test_angle_of(void) {
    int angle;
    int mismatches = 0;

    CHECK(gdx_wind_angle_of(0.0f, -10.0f, 1.0f) == 0, "world -Z is north");
    CHECK(gdx_wind_angle_of(10.0f, 0.0f, 1.0f) == 90, "world +X is east");
    CHECK(gdx_wind_angle_of(0.0f, 10.0f, 1.0f) == 180, "world +Z is south");
    CHECK(gdx_wind_angle_of(-10.0f, 0.0f, 1.0f) == 270, "world -X is west");
    CHECK(gdx_wind_angle_of(0.5f, 0.5f, 1.0f) == -1, "a direction shorter than the minimum has no angle");
    CHECK(gdx_wind_angle_of(0.0f, 0.0f, 0.0f) == -1, "a zero direction has no angle");
    for (angle = 0; angle < 360; angle++) {
        float x;
        float z;

        gdx_wind_direction(angle, &x, &z);
        if (gdx_wind_angle_of(x * 100.0f, z * 100.0f, 1.0f) != angle) {
            mismatches++;
        }
    }
    CHECK(mismatches == 0, "the angle of a wind's direction is its angle, for every degree");
}

static int icon_ink(const unsigned char* icon) {
    int i;
    int ink = 0;

    for (i = 0; i < GDX_WIND_ICON_WIDTH * GDX_WIND_ICON_HEIGHT; i++) {
        ink += icon[(i * 2) + 1] & 1; /* RGBA5551 big-endian: alpha is the low bit of the second byte */
    }
    return ink;
}

static void test_part_icon(void) {
    unsigned char icons[GDX_WIND_PART_ROWS][GDX_WIND_ICON_BYTES];
    unsigned char untouched[GDX_WIND_ICON_BYTES];
    int row;
    int i;
    int edges = 0;

    for (row = 0; row <= GDX_WIND_PART_GALE; row++) {
        CHECK(gdx_wind_part_icon(row, icons[row]) == 1, "every row but Remove has an icon");
        CHECK(icon_ink(icons[row]) > 0, "an icon has opaque pixels");
    }
    CHECK(icon_ink(icons[GDX_WIND_PART_WEAK]) < icon_ink(icons[GDX_WIND_PART_MID]), "Mid has more chevrons than Weak");
    CHECK(icon_ink(icons[GDX_WIND_PART_MID]) < icon_ink(icons[GDX_WIND_PART_STRONG]),
          "Strong has more chevrons than Mid");
    CHECK(icon_ink(icons[GDX_WIND_PART_STRONG]) < icon_ink(icons[GDX_WIND_PART_GALE]),
          "Gale has more chevrons than Strong");
    for (i = 0; i < GDX_WIND_ICON_WIDTH; i++) {
        edges += icons[GDX_WIND_PART_GALE][(i * 2) + 1] & 1;
        edges += icons[GDX_WIND_PART_GALE][(((GDX_WIND_ICON_HEIGHT - 1) * GDX_WIND_ICON_WIDTH + i) * 2) + 1] & 1;
    }
    CHECK(edges == 0, "the top and bottom rows stay clear, so the icon sits inside the row");
    for (i = 0; i < GDX_WIND_ICON_BYTES; i++) {
        untouched[i] = 0xA5;
    }
    CHECK(gdx_wind_part_icon(GDX_WIND_PART_REMOVE, untouched) == 0, "Remove has no icon");
    CHECK(gdx_wind_part_icon(GDX_WIND_PART_ROWS, untouched) == 0, "a row out of range has no icon");
    for (i = 0; i < GDX_WIND_ICON_BYTES; i++) {
        if (untouched[i] != 0xA5) {
            break;
        }
    }
    CHECK(i == GDX_WIND_ICON_BYTES, "a row without an icon leaves the buffer alone");
}

static void test_chevrons(void) {
    CHECK(gdx_wind_chevrons(0) == 0 && gdx_wind_chevrons(-3) == 0, "no wind has no chevrons");
    CHECK(gdx_wind_chevrons(1) == 1 && gdx_wind_chevrons(25) == 1, "up to 25 percent shows one chevron");
    CHECK(gdx_wind_chevrons(26) == 2 && gdx_wind_chevrons(50) == 2, "up to 50 percent shows two");
    CHECK(gdx_wind_chevrons(51) == 3 && gdx_wind_chevrons(75) == 3, "up to 75 percent shows three");
    CHECK(gdx_wind_chevrons(76) == 4 && gdx_wind_chevrons(100) == 4, "up to 100 percent shows four");
    CHECK(gdx_wind_chevrons(250) == 4, "a strength past 100 still shows four");
}

static void sub3(const float a[3], const float b[3], float out[3]) {
    out[0] = a[0] - b[0];
    out[1] = a[1] - b[1];
    out[2] = a[2] - b[2];
}

static void test_road_quad(void) {
    const float center[3] = { 100.0f, 50.0f, -200.0f };
    const float flat[3] = { 0.0f, 1.0f, 0.0f };
    const float banked[3] = { 0.5f, 0.8660254f, 0.0f }; /* tilted 30 degrees toward +X */
    const float wall[3] = { 1.0f, 0.0f, 0.0f };
    float quad[4][3];
    float front[3];
    float side[3];
    float d[3];
    float mid[3];
    int k;

    CHECK(gdx_wind_road_quad(90, center, flat, 60.0f, 40.0f, quad) == 1, "a flat road takes a quad");
    for (k = 0; k < 3; k++) {
        mid[k] = (quad[0][k] + quad[1][k] + quad[2][k] + quad[3][k]) * 0.25f;
    }
    CHECK(near(mid[0], center[0], 1e-3f) && near(mid[1], center[1], 1e-3f) && near(mid[2], center[2], 1e-3f),
          "the quad is centered on the point");
    CHECK((quad[0][0] > center[0]) && (quad[1][0] > center[0]) && (quad[2][0] < center[0]) &&
              (quad[3][0] < center[0]),
          "an east wind puts the front corners east of the center");
    sub3(quad[0], quad[2], front);
    sub3(quad[1], quad[0], side);
    CHECK(near(sqrtf(dot3(front, front)), 120.0f, 1e-2f) && near(sqrtf(dot3(side, side)), 80.0f, 1e-2f),
          "the quad is twice the half length long and twice the half width wide");
    CHECK(near(dot3(front, side), 0.0f, 1e-2f), "the quad's sides are square to its length");
    gdx_wind_road_quad(0, center, flat, 60.0f, 40.0f, quad);
    CHECK((quad[0][2] < center[2]) && (quad[2][2] > center[2]), "a north wind puts the front toward world -Z");

    CHECK(gdx_wind_road_quad(90, center, banked, 60.0f, 40.0f, quad) == 1, "a banked road takes a quad");
    for (k = 0; k < 4; k++) {
        sub3(quad[k], center, d);
        CHECK(near(dot3(d, banked), 0.0f, 1e-2f), "every corner lies on the banked road's plane");
    }
    sub3(quad[0], quad[2], front);
    CHECK(near(sqrtf(dot3(front, front)), 120.0f, 1e-2f), "a banked quad keeps its length");

    CHECK(gdx_wind_road_quad(90, center, wall, 60.0f, 40.0f, quad) == 0, "a wall facing the wind takes no quad");
    CHECK(gdx_wind_road_quad(0, center, wall, 60.0f, 40.0f, quad) == 1, "a wall the wind runs along takes one");
}

static int road_alpha(const unsigned char* tex, int x, int y) {
    return tex[(((y * GDX_WIND_ROAD_TEX_WIDTH) + x) * 2) + 1] & 1;
}

/* Separate opaque runs down the middle column: one for each chevron's point. */
static int road_center_runs(const unsigned char* tex) {
    int runs = 0;
    int last = 0;
    int y;

    for (y = 0; y < GDX_WIND_ROAD_TEX_HEIGHT; y++) {
        int alpha = road_alpha(tex, GDX_WIND_ROAD_TEX_WIDTH / 2, y);

        runs += (alpha && !last);
        last = alpha;
    }
    return runs;
}

static int road_first_row(const unsigned char* tex, int x) {
    int y;

    for (y = 0; y < GDX_WIND_ROAD_TEX_HEIGHT; y++) {
        if (road_alpha(tex, x, y)) {
            return y;
        }
    }
    return GDX_WIND_ROAD_TEX_HEIGHT;
}

static void test_road_texture(void) {
    static unsigned char tex[GDX_WIND_ROAD_TEX_BYTES];
    static unsigned char untouched[GDX_WIND_ROAD_TEX_BYTES];
    int count;
    int i;

    for (count = 1; count <= 4; count++) {
        CHECK(gdx_wind_road_texture(count, tex) == 1, "one to four chevrons have a texture");
        CHECK(road_center_runs(tex) == count, "the middle column crosses each chevron once");
        CHECK(!road_alpha(tex, 0, 0) && !road_alpha(tex, GDX_WIND_ROAD_TEX_WIDTH - 1, GDX_WIND_ROAD_TEX_HEIGHT - 1),
              "the corners are clear, so the road shows around the chevrons");
    }
    gdx_wind_road_texture(1, tex);
    CHECK(road_first_row(tex, GDX_WIND_ROAD_TEX_WIDTH / 2) < road_first_row(tex, 6),
          "a chevron's point faces row 0, the front of the quad");
    for (i = 0; i < GDX_WIND_ROAD_TEX_BYTES; i++) {
        untouched[i] = 0xA5;
    }
    CHECK(gdx_wind_road_texture(0, untouched) == 0 && gdx_wind_road_texture(5, untouched) == 0,
          "other counts have no texture");
    for (i = 0; i < GDX_WIND_ROAD_TEX_BYTES; i++) {
        if (untouched[i] != 0xA5) {
            break;
        }
    }
    CHECK(i == GDX_WIND_ROAD_TEX_BYTES, "a count without a texture leaves the buffer alone");
}

int main(void) {
    test_part_strength();
    test_angle_of();
    test_part_icon();
    test_chevrons();
    test_road_quad();
    test_road_texture();
    test_resolve();
    test_direction();
    test_grip();
    test_strength_scale();
    test_push();
    test_view_shares();
    test_streaks();
    if (sFailures != 0) {
        printf("%d of %d wind checks failed\n", sFailures, sChecks);
        return 1;
    }
    printf("All %d wind checks passed\n", sChecks);
    return 0;
}
