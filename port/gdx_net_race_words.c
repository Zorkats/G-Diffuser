/* port/gdx_net_race_words.c -- the words of a G-NET room's quit box. See gdx_net_race_words.h.
 * Copyright (c) Zorkats
 *
 * Each word is 16 rows of text, one character per pixel. A character stands for an index into the stock
 * menu text palette, aMenuTextTLUT, which the player's ROM supplies at run time:
 *
 *   .  0  transparent       H  7  darkest rim
 *   B  1  dark rim          L 11  light grey
 *   C  2  dark grey         M 12  mid grey
 *   E  4  near white        Q 16  shadow grey
 *   F  5  white
 *
 * The characters are the ones Aseprite's ASCII view gives these palette indices, so the source file of the
 * art (gnet_box_words.aseprite, layers word-return, word-leave and word-watch) reads back as these rows.
 * The letters sit on rows 4 to 11 like the stock words, slanted one pixel per four rows, with their grey
 * rims as the gaps between them. */

#include "gdx_net_race_words.h"

static const char* const kWatch[GDX_NET_RACE_WORDS_HEIGHT] = {
    "........................................",
    "........................................",
    "........................................",
    "..MM...MM......MB.......MM..............",
    ".BFFB.BFFB....BFLB.....BFFB.............",
    ".BFFBMBFFBBMMBBFFB.BMMBBFFBB............",
    ".BFFBFBFFBLFFEBFFFBEFFEBFFFEB...........",
    ".BFFBFBFFBBBFFBFFBBFFBFBFFBFB...........",
    "BFFBFBFFBEFFFBFFBBFFB.BFFBFB............",
    "BFFFFFFFBFFCFBFFBBFFBBBFFBFB............",
    "BLFFLFFLBFFCFBFFBBFFBFBFFBFB............",
    ".BFFBFFBBEFFFBEFFBEFFEBFFBFB............",
    "..BB.BB..HQQB.HBB.HQQH.BB.B.............",
    "........................................",
    "........................................",
    "........................................",
};

static const char* const kReturnToRoom[GDX_NET_RACE_WORDS_HEIGHT] = {
    "................................................................",
    "................................................................",
    "................................................................",
    ".BMMMB......MB..................MB.........BMMMB................",
    "BEFFFEB....BFLB................BFLB.......BEFFFEB...............",
    "BFFCFFBBMMBBFFB.MM.M.MMM.MMMB..BFFB.BMMB..BFFCFFBBMMB.BMMB.MMMMB",
    "BFFCFFBEFFEBFFFBFFBFBFFFBFFFEB.BFFFBEFFEB.BFFCFFBEFFEBEFFEBFFFFE",
    "BFFFFLBFFCFBFFBBFFBFBFFBBFFBFB.BFFBBFFCFB.BFFFFLBFFCFBFFCFBFCFCF",
    "FFBFFBFFFFBFFBBFFBFBFFBBFFBFB.BFFBBFFCFB.BFFBFFBFFCFBFFCFBFCFCFB",
    "FFBFFBFFBBBFFBBFFBFBFFBBFFBFB.BFFBBFFCFB.BFFBFFBFFCFBFFCFBFCFCFB",
    "FFBFFBFFBLBFFBBFFCFBFFBBFFBFB.BFFBBFFCFB.BFFBFFBFFCFBFFCFBFCFCFB",
    "FFBFFBEFFEBEFFBEFFFBFFBBFFBFB.BEFFBEFFEB.BFFBFFBEFFEBEFFEBFCFCFB",
    "BB.BB.HQQH.HBB.HQQB.BB..BB.B...HBB.HQQH...BB.BB.HQQH.HQQH.BHBHB.",
    "................................................................",
    "................................................................",
    "................................................................",
};

static const char* const kLeaveTheRoom[GDX_NET_RACE_WORDS_HEIGHT] = {
    "................................................................",
    "................................................................",
    "................................................................",
    ".MM........................MB..MM..........BMMMB................",
    "BFFB......................BFLBBFFB........BEFFFEB...............",
    "BFFB.BMMB.BMMB.MM.M.BMMB..BFFBBFFBB.BMMB..BFFCFFBBMMB.BMMB.MMMMB",
    "BFFBBEFFEBLFFEBFFBFBEFFEB.BFFFBFFFEBEFFEB.BFFCFFBEFFEBEFFEBFFFFE",
    "BFFBBFFCFBBBFFBFFBFBFFCFB.BFFBBFFBFBFFCFB.BFFFFLBFFCFBFFCFBFCFCF",
    "FFBBFFFFBEFFFBFFBFBFFFFB.BFFBBFFBFBFFFFB.BFFBFFBFFCFBFFCFBFCFCFB",
    "FFBBFFBBBFFCFBFFEFBFFBB..BFFBBFFBFBFFBB..BFFBFFBFFCFBFFCFBFCFCFB",
    "FFBBFFBLBFFCFBEFFEBFFBLB.BFFBBFFBFBFFBLB.BFFBFFBFFCFBFFCFBFCFCFB",
    "FFFBEFFEBEFFFBBFFBBEFFEB.BEFFBFFBFBEFFEB.BFFBFFBEFFEBEFFEBFCFCFB",
    "BBB.HQQH.HQQB..BB..HQQH...HBB.BB.B.HQQH...BB.BB.HQQH.HQQH.BHBHB.",
    "................................................................",
    "................................................................",
    "................................................................",
};

#define WORDS_MAX_WIDTH 64

/* u64 keeps every texture 8-byte aligned, as texture loads expect. A 16-row CI8 word is loaded as one
 * block of exactly its own rows (func_8007E410), so nothing past width * 16 bytes is read. */
static unsigned long long sStorage[GDX_NET_RACE_WORD_COUNT][WORDS_MAX_WIDTH * GDX_NET_RACE_WORDS_HEIGHT / 8];
static int sDecoded;
static int sFaults;

static int palette_index(char c) {
    switch (c) {
        case '.':
            return 0;
        case 'B':
            return 1;
        case 'C':
            return 2;
        case 'E':
            return 4;
        case 'F':
            return 5;
        case 'H':
            return 7;
        case 'L':
            return 11;
        case 'M':
            return 12;
        case 'Q':
            return 16;
        default:
            return -1;
    }
}

static const char* const* word_rows(int word) {
    switch (word) {
        case GDX_NET_RACE_WORD_WATCH:
            return kWatch;
        case GDX_NET_RACE_WORD_RETURN_TO_ROOM:
            return kReturnToRoom;
        case GDX_NET_RACE_WORD_LEAVE_THE_ROOM:
            return kLeaveTheRoom;
        default:
            return 0;
    }
}

static int word_width(int word) {
    return word == GDX_NET_RACE_WORD_WATCH ? 40 : WORDS_MAX_WIDTH;
}

/* A row shorter than the width leaves 0xFF in the cells it lacks; a longer one counts as a fault. */
static void decode_word(int word) {
    const char* const* rows = word_rows(word);
    unsigned char* out = (unsigned char*) sStorage[word];
    const int width = word_width(word);
    int y;

    for (y = 0; y < GDX_NET_RACE_WORDS_HEIGHT; y++) {
        const char* row = rows[y];
        int x;
        int ended = 0;

        for (x = 0; x < width; x++) {
            int index = -1;

            if (!ended && row[x] == '\0') {
                ended = 1;
            }
            if (!ended) {
                index = palette_index(row[x]);
            }
            if (index < 0) {
                sFaults++;
                out[y * width + x] = 0xFF;
            } else {
                out[y * width + x] = (unsigned char) index;
            }
        }
        if (!ended && row[width] != '\0') {
            sFaults++;
        }
    }
}

static void decode_all(void) {
    int word;

    if (sDecoded) {
        return;
    }
    sFaults = 0;
    for (word = 0; word < GDX_NET_RACE_WORD_COUNT; word++) {
        if (word_rows(word) != 0) {
            decode_word(word);
        }
    }
    sDecoded = 1;
}

const unsigned char* gdx_net_race_words_texture(int word, int* width, int* height) {
    if (word_rows(word) == 0) {
        return 0;
    }
    decode_all();
    if (width != 0) {
        *width = word_width(word);
    }
    if (height != 0) {
        *height = GDX_NET_RACE_WORDS_HEIGHT;
    }
    return (const unsigned char*) sStorage[word];
}

int gdx_net_race_words_faults(void) {
    decode_all();
    return sFaults;
}
