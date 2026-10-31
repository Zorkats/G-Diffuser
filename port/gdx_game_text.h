/* port/gdx_game_text.h -- menu, ending and credits text from the mounted archive.
 *
 * Difficulty names, ending cup names, the credits and a few labels are ordinary compiled C, so a ROM
 * hack's own wording could not reach the port: mounting one still drew the retail text. The
 * FZX:GAME_TEXT recipe (decomp/assets/yaml/us/rev0/game_text.yaml) extracts that text into a single
 * archive entry, which this module applies over the compiled-in text once, during game init.
 *
 * NOTHING HERE IS HACK-SPECIFIC. A vanilla extraction decodes to the compiled-in text, so the override
 * is a no-op on a stock install. An archive built before this recipe existed has no entry, and the
 * compiled-in text stands.
 */
#ifndef GDX_GAME_TEXT_H
#define GDX_GAME_TEXT_H

/* Archive key: yaml stem / entry name, matching how every other recipe entry is addressed. */
#define GDX_GAME_TEXT_KEY "game_text/gGameText"

/* Slot size in the decoded storage, terminator included. */
#define GDX_GAME_TEXT_STRING_MAX 32

/* How many entries the cart's ROM holds. sCupNames and gCreditsNames are longer in an Expansion Kit
 * build, whose extra rows have no cart text and keep their compiled-in values. */
#define GDX_GAME_TEXT_DIFFICULTY_COUNT 4
#define GDX_GAME_TEXT_CUP_COUNT 6
#define GDX_GAME_TEXT_ROLE_COUNT 31
#define GDX_GAME_TEXT_NAME_COUNT 36

/* Bytes sThanksForPlayingStr holds before its terminator. */
#define GDX_GAME_TEXT_ENDING_MESSAGE_MAX 20

#ifdef __cplusplus
extern "C" {
#endif

typedef struct GdxGameText {
    char difficultyNames[GDX_GAME_TEXT_DIFFICULTY_COUNT][GDX_GAME_TEXT_STRING_MAX];
    char cupNames[GDX_GAME_TEXT_CUP_COUNT][GDX_GAME_TEXT_STRING_MAX];
    char creditsRoles[GDX_GAME_TEXT_ROLE_COUNT][GDX_GAME_TEXT_STRING_MAX];
    char creditsNames[GDX_GAME_TEXT_NAME_COUNT][GDX_GAME_TEXT_STRING_MAX];
    char endingMessage[GDX_GAME_TEXT_STRING_MAX];
    char timeAttack[GDX_GAME_TEXT_STRING_MAX];
    char creditsStaff[GDX_GAME_TEXT_STRING_MAX];
} GdxGameText;

/* The labels the game draws from bare string literals, so there is no table to repoint. The decomp's
 * call sites read these instead under PORT. */
extern const char* gGdxTextTimeAttack;
extern const char* gGdxTextCreditsStaff;

/* Decodes one FZX:GAME_TEXT archive entry, header included, into `out`. Returns 1 when the whole
 * payload is well formed and 0 otherwise; on failure `out` is fully zeroed. Pure: no archive access
 * and no game state, so it can be tested without either. */
int gdx_game_text_parse(const unsigned char* bytes, unsigned long size, GdxGameText* out);

/* Reads the entry from the mounted archives and points the game's text at it. Silent when no entry
 * exists; a malformed entry is logged and ignored. */
void gdx_game_text_apply(void);

#ifdef __cplusplus
}
#endif

#endif /* GDX_GAME_TEXT_H */
