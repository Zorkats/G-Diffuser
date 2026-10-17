/* port/gdx_ek_text.h -- Expansion Kit menu text built from the stock textures.
 *
 * Course Edit draws its BGM names and row numbers from fixed I4 images: one 64x8 name per stock track and
 * one 16x16 number badge per stock row (1 to 15). Custom rows have neither, so this module composes them:
 *
 *   - names, from the Expansion Kit font sheet (aExpansionKitFontCharacterSheet1Tex, I4 128x48, ASCII 0x20
 *     to 0x7F in 8x8 cells). The stock name images use the same glyphs; each glyph is cut to its ink
 *     columns and followed by one blank column, which matches the short stock names.
 *   - number badges 16 to 99, from the digits of the stock badges. The two-digit badges 10 to 15 supply
 *     condensed 0 to 5 and the tens 1; 6 to 9 come from the single-digit badges. Every digit is moved so
 *     its top row is row 4, the baseline of badges 12 to 15.
 *
 * Images are N64 I4: row-major, two pixels per byte, the left pixel in the high nibble. Pure C with plain
 * int sizes, so decomp files can include it.
 */

#ifndef GDX_EK_TEXT_H
#define GDX_EK_TEXT_H

#ifdef __cplusplus
extern "C" {
#endif

#define GDX_EK_FONT_SHEET_WIDTH 128
#define GDX_EK_FONT_SHEET_HEIGHT 48
#define GDX_EK_TEXT_HEIGHT 8
/* Widest composed name: 64 characters of at most 8 pixels. A multiple of 16, as 4-bit texture loads want. */
#define GDX_EK_TEXT_MAX_WIDTH 512
#define GDX_EK_TEXT_BYTES(width) ((width) * GDX_EK_TEXT_HEIGHT / 2)
#define GDX_EK_BADGE_SIZE 16
#define GDX_EK_BADGE_BYTES (GDX_EK_BADGE_SIZE * GDX_EK_BADGE_SIZE / 2)
#define GDX_EK_BADGE_STOCK_MAX 15
#define GDX_EK_BADGE_MAX 99

/* Pixel width of text in the font: ink columns plus one blank column between glyphs, and a wider gap for a
 * space. Characters outside 0x20 to 0x7F count as spaces. Returns 0 for NULL or empty text. */
int gdx_ek_text_measure(const unsigned char* fontSheet, const char* text);

/* Clears out (outWidth x 8, outWidth a multiple of 2) and draws text from column xOffset. Glyph columns
 * past outWidth are dropped. Returns the width drawn, which is the measured width clipped to the image. */
int gdx_ek_text_compose(const unsigned char* fontSheet, const char* text, int xOffset, unsigned char* out,
                        int outWidth);

/* Writes the 16x16 badge for number into out. badges[n] is the stock badge for n (1 to 15; badges[0] is
 * unused). 1 to 15 copy the stock image; 16 to 99 are composed. Returns 1 on success, 0 for a number out
 * of range or a missing source badge. */
int gdx_ek_badge_compose(const unsigned char* const badges[GDX_EK_BADGE_STOCK_MAX + 1], int number,
                         unsigned char* out);

/* Scroll offset of a name wider than its box, in pixels, for a frame counter that starts at 0 when the
 * name appears: it holds at the start, scrolls one pixel every two frames up to range, then holds at the
 * end and starts again. range <= 0 (the name fits) gives 0. */
int gdx_ek_marquee_offset(int frame, int range);

#ifdef __cplusplus
}
#endif

#endif /* GDX_EK_TEXT_H */
