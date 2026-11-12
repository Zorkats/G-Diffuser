/* port/gdx_net_race_words.h -- the words of a G-NET room's quit box, as stock-style textures (slice R6).
 * Copyright (c) Zorkats
 *
 * The quit box (port/gdx_net_race_box.h) stands where the stock Retry box does, and its words are drawn
 * the way the stock box draws its own: 16-row CI8 textures on the stock menu text palette, aMenuTextTLUT,
 * tinted per row by Menus_SetOptionColor. CONTINUE is the stock texture, aMenuContinueTex. The other three
 * are original art, painted for this port on the stock palette's index order, so each pixel here is an
 * index into the player's own aMenuTextTLUT and no pixel comes from the game.
 *
 * The art is kept in gdx_net_race_words.c as text, one character per pixel, and decoded on first use.
 *
 * Pure C with no system headers: decomp/src/overlays/ovl_i3/menus.c includes it.
 *
 * Game thread only. */

#ifndef GDX_NET_RACE_WORDS_H
#define GDX_NET_RACE_WORDS_H

#ifdef __cplusplus
extern "C" {
#endif

/* The words a row of the box can show (gdx_net_race_box_word). */
enum {
    GDX_NET_RACE_WORD_NONE = -1,
    GDX_NET_RACE_WORD_CONTINUE = 0,       /* the stock aMenuContinueTex */
    GDX_NET_RACE_WORD_WATCH = 1,          /* 40 x 16 */
    GDX_NET_RACE_WORD_RETURN_TO_ROOM = 2, /* 64 x 16 */
    GDX_NET_RACE_WORD_LEAVE_THE_ROOM = 3, /* 64 x 16 */
    GDX_NET_RACE_WORD_COUNT = 4
};

/* Every word is as tall as the stock ones. */
#define GDX_NET_RACE_WORDS_HEIGHT 16

/* A word's CI8 texture, width * GDX_NET_RACE_WORDS_HEIGHT bytes, 8-byte aligned, rows top to bottom.
 * Stores its size in width and height when they are not NULL. Returns NULL for CONTINUE, which is the
 * stock texture, and for a word that does not exist. */
const unsigned char* gdx_net_race_words_texture(int word, int* width, int* height);

/* The faults in the art: each cell a row lacks or whose character is not in the legend, and each row
 * longer than the word. 0 for well-formed art; a cell that was wrong decodes to 0xFF. */
int gdx_net_race_words_faults(void);

#ifdef __cplusplus
}
#endif

#endif
