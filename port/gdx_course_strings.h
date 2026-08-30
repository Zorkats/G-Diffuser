/* port/gdx_course_strings.h -- per-course track names, subtitles and BGM from the mounted archive.
 *
 * Three tables decide what a course is CALLED and what plays while you race it, and all three are
 * ordinary compiled C rather than addressable assets:
 *
 *   sTrackNames[32]      decomp/src/game/common.c, copied into gTrackNames[] by func_8007D9D0()
 *   sTrackSubtitles[31]  decomp/src/overlays/course_select/course_select.c
 *   D_800CF4D8[24]       decomp/src/game/racer.c, read at race start to pick the song
 *
 * That is why mounting a ROM hack used to give you its courses under the retail game's names,
 * with the retail game's music: an archive can replace any asset, and none of these is one. The
 * FZX:COURSE_STRINGS recipe (decomp/assets/yaml/us/rev0/course_strings.yaml) closes the gap by
 * extracting all three from the ROM into a single archive entry, which this module applies over
 * the compiled-in tables once, during game init.
 *
 * NOTHING HERE IS HACK-SPECIFIC. A vanilla extraction produces the vanilla text, so the override
 * is a no-op on a stock install and the whole feature reduces to ArchiveManager mounting order:
 * the hack archive is mounted last, so its entry is the one that resolves. An archive built
 * before this recipe existed simply has no entry, and the compiled-in tables stand.
 *
 * Only the first 24 entries are applied. Those are the cart's own courses; gTrackNames[24..29]
 * belong to the Course Edit cup and are filled from the save file, and everything above that is
 * derived from the same 32 strings rather than stored.
 */
#ifndef GDX_COURSE_STRINGS_H
#define GDX_COURSE_STRINGS_H

/* Archive key: yaml stem / entry name, matching how every other recipe entry is addressed. */
#define GDX_COURSE_STRINGS_KEY "course_strings/gCourseStrings"

/* Table capacities, matching the retail arrays the recipe reads. */
#define GDX_COURSE_STRINGS_NAME_MAX 32
#define GDX_COURSE_STRINGS_SUB_MAX 31
#define GDX_COURSE_STRINGS_BGM_MAX 24

/* Bytes one string may occupy INCLUDING its terminator. The factory refuses to emit more, so a
 * payload declaring a larger bound was produced by a newer torch than this build understands and
 * is rejected rather than truncated. */
#define GDX_COURSE_STRING_MAX 64

/* Entries actually pushed into the game's tables. See the header comment. */
#define GDX_COURSE_STRINGS_APPLY_COUNT 24

#ifdef __cplusplus
extern "C" {
#endif

typedef struct GdxCourseStrings {
    unsigned int nameCount;
    unsigned int subtitleCount;
    unsigned int bgmCount;
    char names[GDX_COURSE_STRINGS_NAME_MAX][GDX_COURSE_STRING_MAX];
    char subtitles[GDX_COURSE_STRINGS_SUB_MAX][GDX_COURSE_STRING_MAX];
    unsigned char bgm[GDX_COURSE_STRINGS_BGM_MAX];
} GdxCourseStrings;

/* Decodes one FZX:COURSE_STRINGS archive entry, header included, into `out`. Returns 1 when the
 * whole payload is well formed and 0 otherwise; on failure `out` is fully zeroed, so a caller
 * that ignores the result still sees empty counts rather than half-decoded text.
 *
 * Rejects, rather than repairs: a wrong resource type or version, a declared string bound this
 * build cannot store, fewer entries than GDX_COURSE_STRINGS_APPLY_COUNT, any length that runs
 * past the end of the buffer, and any byte the game font cannot draw. The last one matters
 * because these strings reach Font_DrawString directly. */
int gdx_course_strings_parse(const unsigned char* bytes, unsigned long size, GdxCourseStrings* out);

/* Loads GDX_COURSE_STRINGS_KEY from the mounted archives and applies it. Silent no-op when the
 * key is absent, which is the normal case for an archive built before this recipe existed.
 * Called once from game init, straight after func_8007D9D0() has filled gTrackNames[]. */
void gdx_course_strings_apply(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* GDX_COURSE_STRINGS_H */
