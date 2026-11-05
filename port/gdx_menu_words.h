/* port/gdx_menu_words.h -- menu row labels built from words of the stock menu textures.
 *
 * The Time Attack finish menu draws every row as a 64x16 CI8 ROM texture, so a port-only row drawn in
 * the game font never matches them. "Save Record" is assembled instead from the last word of
 * aMenuGhostSaveTex ("Save") and the last word of aMenuClearRecordTex ("Record"), at runtime, from the
 * textures the game has already decompressed. The repository holds no pixels, only this column scan.
 */
#ifndef GDX_MENU_WORDS_H
#define GDX_MENU_WORDS_H

#define GDX_MENU_WORDS_WIDTH 64
#define GDX_MENU_WORDS_HEIGHT 16
#define GDX_MENU_WORDS_SIZE (GDX_MENU_WORDS_WIDTH * GDX_MENU_WORDS_HEIGHT)

/* Blank columns between the two words, matching the gap between words in the stock rows. */
#define GDX_MENU_WORDS_GAP 3

#ifdef __cplusplus
extern "C" {
#endif

/* Finds the last word of a 64x16 CI8 texture: the run of columns that ends at the rightmost inked column
 * and starts after the nearest fully blank column to its left. Index 0 is the background. Returns 1 and
 * fills first and last (inclusive) on success, 0 when the texture is blank or holds a single word. */
int gdx_menu_words_last_word(const unsigned char* tex, int* first, int* last);

/* Writes the last word of `left`, a gap, then the last word of `right` into `out`, a 64x16 CI8 texture,
 * starting at the column the stock rows start at. Returns 1 on success and 0 when either word is
 * missing or the pair does not fit. `out` is left untouched on failure. Pure: no game state. */
int gdx_menu_words_compose(const unsigned char* left, const unsigned char* right, unsigned char* out);

#ifdef __cplusplus
}
#endif

#endif /* GDX_MENU_WORDS_H */
