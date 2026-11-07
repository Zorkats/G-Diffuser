/* Unit tests for port/gdx_dash_pad.c, compiled UNMODIFIED alongside this file.
 *
 * The texture is synthetic: a gradient made here, not art from any ROM.
 */
#include "../gdx_dash_pad.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* --- port_log.h backing --------------------------------------------------------------------- */

void gdx_port_write_log(const char* message) {
    (void) message;
}

int gGdxDevGateCache[64];
void (*gdx_port_log_tap)(const char* message) = 0;

/* --- stub archive reader -------------------------------------------------------------------- */

static unsigned char sEntry[GDX_DASH_PAD_ENTRY_BYTES];
static int sStubPresent;
static int sStubReads;

int GDiffuser_LoadArchiveFileBytes(const char* key, void* out, size_t outSize, size_t* copiedSize) {
    size_t copy = sizeof(sEntry);

    sStubReads++;
    if (!sStubPresent || (strcmp(key, "course_track_gfx/aDashEffectTex") != 0)) {
        return 0;
    }
    if (copy > outSize) {
        copy = outSize;
    }
    memcpy(out, sEntry, copy);
    *copiedSize = copy;
    return 1;
}

/* --- helpers -------------------------------------------------------------------------------- */

static int sFailures;

#define CHECK(cond, text)                 \
    do {                                  \
        if (cond) {                       \
            printf("PASS: %s\n", (text)); \
        } else {                          \
            printf("FAIL: %s\n", (text)); \
            sFailures++;                  \
        }                                 \
    } while (0)

static void put_u32le(unsigned char* p, unsigned int v) {
    p[0] = (unsigned char) v;
    p[1] = (unsigned char) (v >> 8);
    p[2] = (unsigned char) (v >> 16);
    p[3] = (unsigned char) (v >> 24);
}

/* Row r is (31, 31 - r, r / 4), alpha set: a made-up yellow-to-orange ramp. */
static void make_entry(void) {
    int row;
    int x;

    memset(sEntry, 0, sizeof(sEntry));
    put_u32le(sEntry + 0x44, GDX_DASH_PAD_TEX_SIDE);
    put_u32le(sEntry + 0x48, GDX_DASH_PAD_TEX_SIDE);
    for (row = 0; row < GDX_DASH_PAD_TEX_SIDE; row++) {
        for (x = 0; x < GDX_DASH_PAD_TEX_SIDE; x++) {
            const unsigned int v = (31u << 11) | ((unsigned int) (31 - row) << 6) | ((unsigned int) (row / 4) << 1) | 1u;
            unsigned char* p = sEntry + GDX_DASH_PAD_ENTRY_PIXELS + ((row * GDX_DASH_PAD_TEX_SIDE) + x) * 2;

            p[0] = (unsigned char) (v >> 8);
            p[1] = (unsigned char) v;
        }
    }
}

static void pixel(const unsigned char* tex, int x, int row, int* r, int* g, int* b, int* a) {
    const unsigned char* p = tex + ((row * GDX_DASH_PAD_TEX_SIDE) + x) * 2;
    const unsigned int v = ((unsigned int) p[0] << 8) | p[1];

    *r = (int) ((v >> 11) & 0x1F);
    *g = (int) ((v >> 6) & 0x1F);
    *b = (int) ((v >> 1) & 0x1F);
    *a = (int) (v & 1u);
}

static int mirrored(const unsigned char* tex) {
    int row;
    int x;

    for (row = 0; row < GDX_DASH_PAD_TEX_SIDE; row++) {
        for (x = 0; x < GDX_DASH_PAD_TEX_SIDE / 2; x++) {
            const unsigned char* left = tex + ((row * GDX_DASH_PAD_TEX_SIDE) + x) * 2;
            const unsigned char* right = tex + ((row * GDX_DASH_PAD_TEX_SIDE) + (GDX_DASH_PAD_TEX_SIDE - 1 - x)) * 2;

            if (memcmp(left, right, 2) != 0) {
                return 0;
            }
        }
    }
    return 1;
}

/* --- tests ---------------------------------------------------------------------------------- */

static void test_compose(void) {
    static unsigned char super[GDX_DASH_PAD_TEX_BYTES];
    static unsigned char brake[GDX_DASH_PAD_TEX_BYTES];
    int r;
    int g;
    int b;
    int a;

    make_entry();
    CHECK(gdx_dash_pad_compose(sEntry, sizeof(sEntry), GDX_DASH_PAD_SUPER, super), "Super composes");
    CHECK(gdx_dash_pad_compose(sEntry, sizeof(sEntry), GDX_DASH_PAD_BRAKE, brake), "Brake composes");
    CHECK(!gdx_dash_pad_compose(sEntry, sizeof(sEntry), GDX_DASH_PAD_NORMAL, super), "a normal pad has no texture");
    CHECK(!gdx_dash_pad_compose(sEntry, sizeof(sEntry) - 1, GDX_DASH_PAD_SUPER, super), "a short entry is refused");

    pixel(super, 0, 0, &r, &g, &b, &a);
    CHECK((r == 0) && (g == 31) && (b == 31) && (a == 1), "Super turns the back of the ramp cyan");
    pixel(super, 0, 15, &r, &g, &b, &a);
    CHECK((r == 3) && (g == 16) && (b == 31), "Super turns the front of the ramp blue");
    CHECK(mirrored(super) && mirrored(brake), "both textures are centred, the same on either side");

    /* The chevron tip is rows 10..12 at the centre columns; a far column at row 11 is off it. */
    pixel(super, 7, 11, &r, &g, &b, &a);
    CHECK((r > 20) && (g > 25) && (b == 31), "Super's chevron is lighter at its tip");
    pixel(super, 0, 11, &r, &g, &b, &a);
    CHECK(r < 10, "the ramp is untouched off the chevron");

    pixel(brake, 0, 15, &r, &g, &b, &a);
    CHECK((r == 31) && (g == 7) && (b == 0), "Brake is red and its ramp runs backward");
    pixel(brake, 7, 15 - 11, &r, &g, &b, &a);
    CHECK((r == 31) && (g > 10), "Brake's chevron is turned backward too");

    sEntry[0x44] = 32;
    CHECK(!gdx_dash_pad_compose(sEntry, sizeof(sEntry), GDX_DASH_PAD_SUPER, super), "a texture of another size is refused");
}

static void test_texture_cache(void) {
    sStubPresent = 0;
    sStubReads = 0;
    CHECK(gdx_dash_pad_texture(GDX_DASH_PAD_NORMAL) == NULL, "a normal pad asks for no texture");
    CHECK(sStubReads == 0, "and reads nothing");
    CHECK(gdx_dash_pad_texture(GDX_DASH_PAD_SUPER) == NULL, "a missing entry draws stock");
    CHECK(gdx_dash_pad_texture(GDX_DASH_PAD_BRAKE) == NULL, "Brake draws stock too");
    CHECK(sStubReads == 1, "a failed read is tried once, not every frame");
}

static void test_span(void) {
    float offset;
    float half;

    gdx_dash_pad_span(80.0f, 50.0f, 0, 400.0f, &offset, &half);
    CHECK((offset == 80.0f) && (half == 50.0f), "0 keeps the stock plate");
    gdx_dash_pad_span(80.0f, 50.0f, 10, 400.0f, &offset, &half);
    CHECK((offset == 80.0f) && (half == 50.0f), "a pad is never narrower than stock");
    gdx_dash_pad_span(80.0f, 50.0f, 100, 400.0f, &offset, &half);
    CHECK((offset == 0.0f) && (half == 200.0f), "100 covers the whole road");
    gdx_dash_pad_span(100.0f, 50.0f, 75, 400.0f, &offset, &half);
    CHECK((half == 150.0f) && (offset == 50.0f), "a side pad moves in to stay on the road");
    gdx_dash_pad_span(-100.0f, 50.0f, 75, 400.0f, &offset, &half);
    CHECK((half == 150.0f) && (offset == -50.0f), "on either side");
    gdx_dash_pad_span(0.0f, 50.0f, 500, 400.0f, &offset, &half);
    CHECK(half == 200.0f, "a percent over 100 is held at 100");

    CHECK(gdx_dash_pad_s_span(GDX_DASH_PAD_NORMAL, 50.0f, 50.0f) == 0x400, "a stock plate keeps the stock texture span");
    CHECK(gdx_dash_pad_s_span(GDX_DASH_PAD_SUPER, 50.0f, 50.0f) == 0x200, "a Super plate shows one chevron across");
    CHECK(gdx_dash_pad_s_span(GDX_DASH_PAD_BRAKE, 200.0f, 50.0f) == 0x800, "a wide plate repeats the chevron");
    CHECK(gdx_dash_pad_s_span(GDX_DASH_PAD_NORMAL, 10000.0f, 50.0f) == 60 * 0x200, "the span stays inside tc's range");
}

static void test_effect_kinds(void) {
    gdx_dash_pad_set_effect_kind(3, GDX_DASH_PAD_SUPER);
    gdx_dash_pad_set_effect_kind(4, GDX_DASH_PAD_BRAKE);
    gdx_dash_pad_set_effect_kind(5, 7);
    gdx_dash_pad_set_effect_kind(GDX_DASH_PAD_EFFECTS, GDX_DASH_PAD_SUPER);
    CHECK((gdx_dash_pad_effect_kind(3) == GDX_DASH_PAD_SUPER) && (gdx_dash_pad_effect_kind(4) == GDX_DASH_PAD_BRAKE),
          "each effect keeps its kind");
    CHECK(gdx_dash_pad_effect_kind(5) == GDX_DASH_PAD_NORMAL, "an unknown kind is a normal pad");
    CHECK(gdx_dash_pad_effect_kind(GDX_DASH_PAD_EFFECTS) == GDX_DASH_PAD_NORMAL, "an effect out of range is normal");
}

static void test_racer(void) {
    int frames = 0;

    CHECK(gdx_dash_pad_boost_frames(100, 1.0f) == 100, "a normal pad keeps the stock boost length");
    CHECK(gdx_dash_pad_boost_frames(100, 1.5f) == 113, "150% boosts a little longer");
    CHECK(gdx_dash_pad_boost_frames(100, 3.0f) == 150, "300% boosts half as long again");

    gdx_dash_pad_racer_reset(2);
    CHECK(gdx_dash_pad_boost(2) == 1.0f, "a reset racer has no extra boost");
    gdx_dash_pad_set_boost(2, 2.5f);
    CHECK(gdx_dash_pad_boost(2) == 2.5f, "a Super pad sets the racer's boost");
    gdx_dash_pad_set_boost(2, 0.2f);
    CHECK(gdx_dash_pad_boost(2) == 1.0f, "a boost never drops under stock");
    CHECK(gdx_dash_pad_boost(99) == 1.0f, "a racer out of range reads stock");

    CHECK(gdx_dash_pad_brake_begin(2) == 1, "the first frame on a Brake pad cuts the speed");
    CHECK(gdx_dash_pad_brake_begin(2) == 0, "more frames on the pad do not cut again");
    CHECK(fabsf(gdx_dash_pad_brake_share(2) - 1.0f) < 0.001f, "the brake starts full");
    while (gdx_dash_pad_brake_tick(2)) {
        frames++;
    }
    CHECK(frames == GDX_DASH_PAD_BRAKE_FRAMES, "the brake holds the drive down for its frames");
    CHECK(gdx_dash_pad_brake_share(2) == 0.0f, "then lets go");
    CHECK(gdx_dash_pad_brake_begin(2) == 1, "the next Brake pad cuts again");
    gdx_dash_pad_racer_reset(2);
    CHECK(gdx_dash_pad_brake_tick(2) == 0, "a reset racer is not braking");
    CHECK(gdx_dash_pad_brake_begin(-1) == 0, "a racer out of range is ignored");
}

int main(void) {
    test_compose();
    test_texture_cache();
    test_span();
    test_effect_kinds();
    test_racer();
    if (sFailures != 0) {
        printf("%d dash pad test(s) failed\n", sFailures);
        return 1;
    }
    printf("All dash pad tests passed\n");
    return 0;
}
