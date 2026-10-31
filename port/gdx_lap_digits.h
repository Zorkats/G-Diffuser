/* port/gdx_lap_digits.h -- lap counter digits 6, 7, 8, 9 and 0, composed from the player's own ROM data.
 *
 * aLapCounterSymbolsTex holds the slash and the digits 1 to 5, and a Single Race can run to 99 laps.
 * The missing digits are made of pieces of the stock glyphs, so they are ROM-derived art and must not
 * live in this repository. It holds only a table of pixel coordinates; the pixels are read from the
 * mounted archive once during game init. A ROM hack that repaints the atlas gets matching digits.
 */
#ifndef GDX_LAP_DIGITS_H
#define GDX_LAP_DIGITS_H

/* The stock atlas, as addressed in the archive, and its RGBA16 layout. */
#define GDX_LAP_DIGITS_ATLAS_KEY "hud_gfx/aLapCounterSymbolsTex"
#define GDX_LAP_DIGITS_ATLAS_WIDTH 8
#define GDX_LAP_DIGITS_ATLAS_HEIGHT 72

/* The composed texture: five 8x12 glyphs in the order 6, 7, 8, 9, 0. */
#define GDX_LAP_DIGITS_WIDTH 8
#define GDX_LAP_DIGITS_HEIGHT 60
#define GDX_LAP_DIGITS_SIZE (GDX_LAP_DIGITS_WIDTH * GDX_LAP_DIGITS_HEIGHT * 2)

/* Torch texture entry: 0x40-byte header, then type, width, height and size words, then the pixels. */
#define GDX_LAP_DIGITS_ENTRY_PIXELS 0x50
#define GDX_LAP_DIGITS_ENTRY_SIZE \
    (GDX_LAP_DIGITS_ENTRY_PIXELS + (GDX_LAP_DIGITS_ATLAS_WIDTH * GDX_LAP_DIGITS_ATLAS_HEIGHT * 2))

#ifdef __cplusplus
extern "C" {
#endif

/* Stays all zero, so fully transparent, until gdx_lap_digits_apply succeeds. */
extern unsigned char aLapCounterExtraDigitsTex[GDX_LAP_DIGITS_SIZE];

/* Builds the digits from one archive entry, header included, into `out`. Returns 1 on success and 0
 * when the entry is not an atlas of the expected size. Pure: no archive access and no game state. */
int gdx_lap_digits_compose(const unsigned char* entry, unsigned long size, unsigned char* out);

/* Reads the atlas from the mounted archives and fills aLapCounterExtraDigitsTex. A failure is logged. */
void gdx_lap_digits_apply(void);

#ifdef __cplusplus
}
#endif

#endif /* GDX_LAP_DIGITS_H */
