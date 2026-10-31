/* Runtime half of the extra lap digits. See port/gdx_lap_digits.h for why this exists.
 *
 * Applied once and never freed: the HUD holds a raw pointer to the texture for the whole run.
 */
#include "gdx_lap_digits.h"

#include "port_log.h"

#include <stddef.h>
#include <string.h>

/* Archive-file reader, declared inline the way every other consumer does (see
 * port/gdx_course_strings.c). It hands back the untouched entry, header included. */
extern int GDiffuser_LoadArchiveFileBytes(const char* key, void* out, size_t outSize, size_t* copiedSize);

/* Where each digit pixel comes from in the atlas, as y * 8 + x. Digit design by Heebeejeebees; the
 * numbers are coordinates, never pixel values. */
static const unsigned short sDigitPixels[GDX_LAP_DIGITS_WIDTH * GDX_LAP_DIGITS_HEIGHT] = {
    /* 6 */
    560, 561, 562, 563, 564, 565, 566, 567,
    296, 297, 298, 299, 300, 301, 302, 303,
    304, 305, 306, 307, 308, 309, 310, 311,
    208, 313, 314, 227, 252, 221, 222, 215,
    208,  36, 418, 515, 324,  44, 326,   7,
    208, 425, 333, 235, 332, 333, 334,   7,
    208, 338, 434, 252, 252, 341, 342, 215,
    536, 537, 538, 539, 540, 541, 542, 543,
    544, 545, 546, 547, 548, 549, 550, 551,
    552, 553, 554, 555, 556, 557, 558, 559,
    560, 561, 562, 563, 564, 565, 566, 567,
    568, 569, 570, 571, 572, 573, 574, 575,
    /* 7 */
    272, 273, 274, 275, 276, 277, 278, 279,
    208, 106, 107, 203,  12, 397, 398, 215,
    208, 497, 210, 211, 212, 213, 214, 215,
    208,  49,  26,   3, 220, 221, 222, 215,
      0,   1,  26, 227, 228, 229, 230, 215,
      0,   1,  26, 235, 140, 237, 206,   7,
      0,  49, 530, 242, 532,   5,   6,   7,
      0,  49, 250, 155, 348,   5,   6,   7,
      0,  49,  66, 163,   4,  45,   6,   7,
      0,  49,  74, 171,   4,  45,   6,   7,
      0,  49,  26,   3,   4,  45,   6,   7,
    568, 569, 570, 571, 572, 573, 574, 575,
    /* 8 */
    560, 561, 562, 563, 564, 565, 566, 567,
    296, 297, 298, 299, 300, 301, 302, 303,
    304, 305, 306, 307, 308, 309, 310, 311,
    208, 313, 314, 410, 410, 221, 222, 215,
      0,  44, 228, 515, 324, 325, 326,   7,
    208, 427, 333, 235, 332, 333, 334, 215,
    208, 338, 434, 244, 244, 341, 342, 215,
    536, 537, 538, 539, 540, 541, 542, 543,
    544, 545, 546, 547, 548, 549, 550, 551,
    552, 553, 554, 555, 556, 557, 558, 559,
    560, 561, 562, 563, 564, 565, 566, 567,
    568, 569, 570, 571, 572, 573, 574, 575,
    /* 9 */
    560, 561, 562, 563, 564, 565, 566, 567,
    296, 297, 298, 299, 300, 301, 302, 303,
    304, 305, 306, 307, 308, 309, 310, 311,
    208, 313, 314, 410, 410, 221, 222, 215,
    208, 513, 228, 513, 513, 325,  36, 215,
      0, 427, 333, 235, 332, 333, 331, 215,
      0, 337, 530,  43, 140, 341, 342, 215,
    208, 249, 250, 244, 244, 349, 350, 215,
    544, 545, 546, 547, 548, 549, 550, 551,
    552, 553, 554, 555, 556, 557, 558, 559,
    560, 561, 562, 563, 564, 565, 566, 567,
    568, 569, 570, 571, 572, 573, 574, 575,
    /* 0 */
    560, 561, 562, 563, 564, 565, 566, 567,
    296, 297, 298, 299, 300, 301, 302, 303,
    304, 305, 306, 307, 308, 309, 310, 311,
    208, 313, 314, 219, 219, 221, 222, 215,
    208, 513, 228,   3,   4, 325, 513, 215,
    208, 425, 333,   3,   4, 333, 331, 215,
    208, 338, 434,   3,   4, 341, 342, 215,
    208, 249, 250, 252, 252, 349, 350, 215,
    544, 545, 546, 547, 548, 549, 550, 551,
    552, 553, 554, 555, 556, 557, 558, 559,
    560, 561, 562, 563, 564, 565, 566, 567,
    568, 569, 570, 571, 572, 573, 574, 575,
};

#define GDX_LAP_DIGITS_ENTRY_WIDTH 0x44
#define GDX_LAP_DIGITS_ENTRY_HEIGHT 0x48

unsigned char aLapCounterExtraDigitsTex[GDX_LAP_DIGITS_SIZE];

static unsigned int gdxReadU32(const unsigned char* bytes) {
    return (unsigned int) bytes[0] | ((unsigned int) bytes[1] << 8) | ((unsigned int) bytes[2] << 16) |
           ((unsigned int) bytes[3] << 24);
}

int gdx_lap_digits_compose(const unsigned char* entry, unsigned long size, unsigned char* out) {
    const unsigned char* atlas;
    size_t i;

    if ((entry == NULL) || (size < GDX_LAP_DIGITS_ENTRY_SIZE)) {
        return 0;
    }
    if ((gdxReadU32(entry + GDX_LAP_DIGITS_ENTRY_WIDTH) != GDX_LAP_DIGITS_ATLAS_WIDTH) ||
        (gdxReadU32(entry + GDX_LAP_DIGITS_ENTRY_HEIGHT) != GDX_LAP_DIGITS_ATLAS_HEIGHT)) {
        return 0;
    }

    atlas = entry + GDX_LAP_DIGITS_ENTRY_PIXELS;
    for (i = 0; i < (sizeof(sDigitPixels) / sizeof(sDigitPixels[0])); i++) {
        memcpy(&out[i * 2], &atlas[sDigitPixels[i] * 2u], 2);
    }
    return 1;
}

void gdx_lap_digits_apply(void) {
    static int sApplied = 0;
    static unsigned char sEntry[GDX_LAP_DIGITS_ENTRY_SIZE];
    size_t copied = 0;

    /* Latched on SUCCESS, so a failed read can still recover on a later init. */
    if (sApplied) {
        return;
    }

    if (!GDiffuser_LoadArchiveFileBytes(GDX_LAP_DIGITS_ATLAS_KEY, sEntry, sizeof(sEntry), &copied) ||
        !gdx_lap_digits_compose(sEntry, (unsigned long) copied, aLapCounterExtraDigitsTex)) {
        gdx_port_logf("[lap-digits] could not read %s (%u bytes); laps with 6 to 9 or 0 draw blank digits\n",
                      GDX_LAP_DIGITS_ATLAS_KEY, (unsigned) copied);
        return;
    }

    sApplied = 1;
    gdx_port_logf("[lap-digits] composed from %s\n", GDX_LAP_DIGITS_ATLAS_KEY);
}
