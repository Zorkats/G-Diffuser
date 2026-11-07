/* port/gdx_gnet_sign.h -- the G-NET main menu card (plan item O2-9), composed from the player's own mode signs.
 * Copyright (c) Zorkats
 *
 * The ninth card of the main menu opens Online > LAN Lobby. Its art, by Heebeejeebees, is a stock mode sign with
 * new text and colours, so it is ROM-derived and must not live in this repository. port/gdx_gnet_sign_map.c,
 * written by tools/gen_gnet_sign.py, holds only where each pixel comes from in the eight stock signs, plus the
 * colours no stock sign has. The card is rebuilt from the loaded sign textures, so a ROM hack that repaints the
 * signs gets a matching card. Pure C: no IO, no allocation, no game state.
 */
#ifndef GDX_GNET_SIGN_H
#define GDX_GNET_SIGN_H

/* No system headers: decomp translation units include this file. */

#ifdef __cplusplus
extern "C" {
#endif

/* The stock signs and the card: 64x86 RGBA16, big-endian, as loaded from the archive. */
#define GDX_GNET_SIGN_WIDTH 64
#define GDX_GNET_SIGN_HEIGHT 86
#define GDX_GNET_SIGN_PIXELS (GDX_GNET_SIGN_WIDTH * GDX_GNET_SIGN_HEIGHT)
#define GDX_GNET_SIGN_SIZE (GDX_GNET_SIGN_PIXELS * 2)
/* Stock signs, in the mode order of sMenuSignCompTexInfos (decomp/src/overlays/ovl_i6/main_menu.c). */
#define GDX_GNET_SIGN_SOURCES 8

/* Card pixel `pixel` takes source sign `source`'s pixel `from`. Pixels are y * 64 + x. */
typedef struct GdxGnetSignCopy {
    unsigned short pixel;
    unsigned char source;
    unsigned short from;
} GdxGnetSignCopy;

/* Card pixel `pixel` is the RGBA5551 colour `value`, one that no stock sign holds. */
typedef struct GdxGnetSignLiteral {
    unsigned short pixel;
    unsigned short value;
} GdxGnetSignLiteral;

/* The card starts as a copy of sign `base`; then the copies and the literals are applied in order. */
typedef struct GdxGnetSignMap {
    unsigned char base;
    const GdxGnetSignCopy* copies;
    unsigned copyCount;
    const GdxGnetSignLiteral* literals;
    unsigned literalCount;
} GdxGnetSignMap;

/* The generated map, in port/gdx_gnet_sign_map.c. */
extern const GdxGnetSignMap gGdxGnetSignMap;

/* Builds the card from `map` into `out`, GDX_GNET_SIGN_SIZE bytes. sources holds GDX_GNET_SIGN_SOURCES pointers to
 * GDX_GNET_SIGN_SIZE bytes each. Returns 1, or 0 with `out` untouched when an argument is NULL, a source the map
 * uses is NULL, or the map names a source or pixel out of range. */
int gdx_gnet_sign_compose(const GdxGnetSignMap* map, const unsigned char* const* sources, unsigned char* out);

#ifdef __cplusplus
}
#endif

#endif /* GDX_GNET_SIGN_H */
