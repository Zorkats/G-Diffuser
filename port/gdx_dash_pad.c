/* port/gdx_dash_pad.c -- Super and Brake dash pads. See gdx_dash_pad.h. */
#include "gdx_dash_pad.h"

#include "port_log.h"

#include <math.h>
#include <stddef.h>
#include <string.h>

/* Archive-file reader, declared inline the way every other consumer does (see port/gdx_lap_digits.c). */
extern int GDiffuser_LoadArchiveFileBytes(const char* key, void* out, size_t outSize, size_t* copiedSize);

#define ENTRY_WIDTH 0x44
#define ENTRY_HEIGHT 0x48
#define CHANNEL_MAX 31

static unsigned char sTextures[2][GDX_DASH_PAD_TEX_BYTES];
static unsigned char sStock[GDX_DASH_PAD_TEX_BYTES];
static int sTexturesState; /* 0 not tried, 1 ready, -1 failed */
static unsigned char sEffectKind[GDX_DASH_PAD_EFFECTS];
static float sBoost[GDX_DASH_PAD_RACERS];
static int sBrakeFrames[GDX_DASH_PAD_RACERS];

static unsigned int read_u32le(const unsigned char* bytes) {
    return (unsigned int) bytes[0] | ((unsigned int) bytes[1] << 8) | ((unsigned int) bytes[2] << 16) |
           ((unsigned int) bytes[3] << 24);
}

static int lighten(int channel, int num, int den) {
    return channel + ((CHANNEL_MAX - channel) * num) / den;
}

/* The chevron points toward higher rows, which is forward on the plate. Columns 7 and 8 are its tip. */
static int on_chevron(int x, int row) {
    const int fromCentre = ((x < 8) ? (7 - x) : (x - 8));
    const int arm = 3 + (7 - fromCentre);

    return (row >= arm) && (row <= arm + 2) && (fromCentre <= 6);
}

int gdx_dash_pad_compose(const unsigned char* entry, unsigned long size, int kind, unsigned char* out) {
    const unsigned char* pixels;
    int x;
    int row;

    if ((entry == NULL) || (out == NULL) || (size < GDX_DASH_PAD_ENTRY_BYTES)) {
        return 0;
    }
    if ((kind != GDX_DASH_PAD_SUPER) && (kind != GDX_DASH_PAD_BRAKE)) {
        return 0;
    }
    if ((read_u32le(entry + ENTRY_WIDTH) != GDX_DASH_PAD_TEX_SIDE) ||
        (read_u32le(entry + ENTRY_HEIGHT) != GDX_DASH_PAD_TEX_SIDE)) {
        return 0;
    }
    pixels = entry + GDX_DASH_PAD_ENTRY_PIXELS;
    for (row = 0; row < GDX_DASH_PAD_TEX_SIDE; row++) {
        /* Brake runs backward: its gradient and chevron are the Super layout turned end for end. */
        const int outRow = (kind == GDX_DASH_PAD_BRAKE) ? (GDX_DASH_PAD_TEX_SIDE - 1 - row) : row;

        for (x = 0; x < GDX_DASH_PAD_TEX_SIDE; x++) {
            const unsigned char* in = pixels + ((row * GDX_DASH_PAD_TEX_SIDE) + x) * 2;
            unsigned char* dst = out + ((outRow * GDX_DASH_PAD_TEX_SIDE) + x) * 2;
            const unsigned int value = ((unsigned int) in[0] << 8) | in[1];
            const int r = (int) ((value >> 11) & 0x1F);
            const int g = (int) ((value >> 6) & 0x1F);
            const int b = (int) ((value >> 1) & 0x1F);
            int nr;
            int ng;
            int nb;
            unsigned int packed;

            if (kind == GDX_DASH_PAD_SUPER) {
                /* Yellow to orange becomes cyan to blue. */
                nr = b;
                ng = g;
                nb = r;
            } else {
                nr = r;
                ng = g / 4;
                nb = b / 4;
            }
            if (on_chevron(x, row)) {
                const int num = (kind == GDX_DASH_PAD_SUPER) ? 3 : 1;
                const int den = (kind == GDX_DASH_PAD_SUPER) ? 4 : 2;

                nr = lighten(nr, num, den);
                ng = lighten(ng, num, den);
                nb = lighten(nb, num, den);
            }
            packed = ((unsigned int) nr << 11) | ((unsigned int) ng << 6) | ((unsigned int) nb << 1) | (value & 1u);
            dst[0] = (unsigned char) (packed >> 8);
            dst[1] = (unsigned char) packed;
        }
    }
    return 1;
}

const void* gdx_dash_pad_texture(int kind) {
    if ((kind != GDX_DASH_PAD_SUPER) && (kind != GDX_DASH_PAD_BRAKE)) {
        return NULL;
    }
    if (sTexturesState == 0) {
        static unsigned char sEntry[GDX_DASH_PAD_ENTRY_BYTES];
        size_t copied = 0;

        sTexturesState = -1;
        if (GDiffuser_LoadArchiveFileBytes("course_track_gfx/aDashEffectTex", sEntry, sizeof(sEntry), &copied) &&
            gdx_dash_pad_compose(sEntry, (unsigned long) copied, GDX_DASH_PAD_SUPER, sTextures[0]) &&
            gdx_dash_pad_compose(sEntry, (unsigned long) copied, GDX_DASH_PAD_BRAKE, sTextures[1])) {
            memcpy(sStock, sEntry + GDX_DASH_PAD_ENTRY_PIXELS, sizeof(sStock));
            sTexturesState = 1;
        } else {
            gdx_port_logf("[dash-pad] stock dash texture not readable, Super and Brake pads draw as stock\n");
        }
    }
    if (sTexturesState != 1) {
        return NULL;
    }
    return sTextures[(kind == GDX_DASH_PAD_SUPER) ? 0 : 1];
}

const void* gdx_dash_pad_stock_texture(void) {
    return (gdx_dash_pad_texture(GDX_DASH_PAD_SUPER) != NULL) ? sStock : NULL;
}

void gdx_dash_pad_span(float stockOffset, float stockHalf, int percent, float roadWidth, float* offset, float* half) {
    const float roadHalf = roadWidth / 2.0f;
    float wanted;
    float room;

    *offset = stockOffset;
    *half = stockHalf;
    if ((percent <= 0) || (roadHalf <= 0.0f)) {
        return;
    }
    if (percent > 100) {
        percent = 100;
    }
    wanted = roadHalf * (float) percent / 100.0f;
    if (wanted <= stockHalf) {
        return;
    }
    if (wanted >= roadHalf) {
        *offset = 0.0f;
        *half = roadHalf;
        return;
    }
    room = roadHalf - wanted;
    *half = wanted;
    if (*offset > room) {
        *offset = room;
    } else if (*offset < -room) {
        *offset = -room;
    }
}

int gdx_dash_pad_s_span(int kind, float half, float stockHalf) {
    const int tilesPerStock = ((kind == GDX_DASH_PAD_SUPER) || (kind == GDX_DASH_PAD_BRAKE)) ? 1 : 2;
    int tiles;

    if (stockHalf <= 0.0f) {
        return 0x400;
    }
    tiles = (int) lroundf((half / stockHalf) * (float) tilesPerStock);
    if (tiles < 1) {
        tiles = 1;
    }
    /* tc is a signed 10.5 value: keep the far edge well inside it. */
    if (tiles > 60) {
        tiles = 60;
    }
    return tiles * 0x200;
}

void gdx_dash_pad_set_effect_kind(int effectIndex, int kind) {
    if ((effectIndex >= 0) && (effectIndex < GDX_DASH_PAD_EFFECTS)) {
        sEffectKind[effectIndex] = (unsigned char) (((kind == GDX_DASH_PAD_SUPER) || (kind == GDX_DASH_PAD_BRAKE))
                                                        ? kind
                                                        : GDX_DASH_PAD_NORMAL);
    }
}

int gdx_dash_pad_effect_kind(int effectIndex) {
    return ((effectIndex >= 0) && (effectIndex < GDX_DASH_PAD_EFFECTS)) ? sEffectKind[effectIndex]
                                                                        : GDX_DASH_PAD_NORMAL;
}

int gdx_dash_pad_boost_frames(int stockFrames, float boost) {
    if (boost <= 1.0f) {
        return stockFrames;
    }
    return (int) lroundf((float) stockFrames * (1.0f + ((boost - 1.0f) * 0.25f)));
}

static int racer_ok(int racerId) {
    return (racerId >= 0) && (racerId < GDX_DASH_PAD_RACERS);
}

void gdx_dash_pad_racer_reset(int racerId) {
    if (racer_ok(racerId)) {
        sBoost[racerId] = 1.0f;
        sBrakeFrames[racerId] = 0;
    }
}

void gdx_dash_pad_set_boost(int racerId, float boost) {
    if (racer_ok(racerId)) {
        sBoost[racerId] = (boost > 1.0f) ? boost : 1.0f;
    }
}

float gdx_dash_pad_boost(int racerId) {
    return (racer_ok(racerId) && (sBoost[racerId] > 1.0f)) ? sBoost[racerId] : 1.0f;
}

int gdx_dash_pad_brake_begin(int racerId) {
    int starts;

    if (!racer_ok(racerId)) {
        return 0;
    }
    starts = (sBrakeFrames[racerId] == 0);
    sBrakeFrames[racerId] = GDX_DASH_PAD_BRAKE_FRAMES;
    return starts;
}

int gdx_dash_pad_brake_tick(int racerId) {
    if (!racer_ok(racerId) || (sBrakeFrames[racerId] == 0)) {
        return 0;
    }
    sBrakeFrames[racerId]--;
    return 1;
}

float gdx_dash_pad_brake_share(int racerId) {
    if (!racer_ok(racerId)) {
        return 0.0f;
    }
    return (float) sBrakeFrames[racerId] / (float) GDX_DASH_PAD_BRAKE_FRAMES;
}
