/* port/gdx_bg_sprites.h -- per-course skyline sprites from the mounted archive.
 *
 * sBackgroundSpriteCourseInitData (decomp/src/overlays/ovl_i3/background.c) decides which sprites
 * stand on each course's horizon and at what angle. It is ordinary compiled C, so a ROM hack's own
 * skylines could not reach the port: mounting one drew the retail skyline for each course slot.
 * The FZX:BG_SPRITES recipe (decomp/assets/yaml/us/rev0/background_sprites.yaml) extracts the table
 * into a single archive entry, which this module applies over the compiled-in table once, during
 * game init.
 *
 * NOTHING HERE IS HACK-SPECIFIC. A vanilla extraction decodes to the compiled-in values, so the
 * override is a no-op on a stock install. An archive built before this recipe existed has no entry,
 * and the compiled-in table stands.
 */
#ifndef GDX_BG_SPRITES_H
#define GDX_BG_SPRITES_H

/* Archive key: yaml stem / entry name, matching how every other recipe entry is addressed. */
#define GDX_BG_SPRITES_KEY "background_sprites/gBackgroundSprites"

/* The cart's own courses, the length of sBackgroundSpriteCourseInitData. */
#define GDX_BG_SPRITES_COURSE_COUNT 24

/* Capacity of sBackgroundSprites, which the game fills without a bounds check. */
#define GDX_BG_SPRITES_MAX_PER_COURSE 6

/* Highest BackgroundSpriteId (decomp/src/overlays/ovl_i3/background.h). 0 is the terminator. */
#define GDX_BG_SPRITES_MAX_ID 55u

#ifdef __cplusplus
extern "C" {
#endif

/* Layout-compatible with the decomp's BackgroundSpriteInitData; this file deliberately does not
 * include decomp headers. */
typedef struct GdxBgSpriteInit {
    short spriteId;
    float angle;
} GdxBgSpriteInit;

typedef struct GdxBgSprites {
    /* One extra row in each list for the terminator the game's copy loop stops on. */
    GdxBgSpriteInit courses[GDX_BG_SPRITES_COURSE_COUNT][GDX_BG_SPRITES_MAX_PER_COURSE + 1];
    unsigned int total;
} GdxBgSprites;

/* Decodes one FZX:BG_SPRITES archive entry, header included, into `out`. Returns 1 when the whole
 * payload is well formed and 0 otherwise; on failure `out` is fully zeroed, which reads as every
 * list being empty. Pure: no archive access and no game state, so it can be tested without either. */
int gdx_bg_sprites_parse(const unsigned char* bytes, unsigned long size, GdxBgSprites* out);

/* Reads the entry from the mounted archives and repoints sBackgroundSpriteCourseInitData at the
 * decoded lists. Silent when no entry exists; a malformed entry is logged and ignored. */
void gdx_bg_sprites_apply(void);

#ifdef __cplusplus
}
#endif

#endif /* GDX_BG_SPRITES_H */
