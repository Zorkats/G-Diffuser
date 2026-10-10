/* Runtime half of FZX:COURSE_STRINGS. See port/gdx_course_strings.h for why this exists.
 *
 * The trick is the one gdx_ek_strings.c already uses for Expansion Kit text: these tables are
 * arrays of char*, so nothing has to be patched in place. Decoded strings go into static storage
 * owned by this file and the array entries are REPOINTED at it. The BGM table is raw bytes and is
 * simply overwritten.
 *
 * Applied once and never freed, deliberately: gTrackNames[] holds borrowed pointers for the whole
 * run and every menu that draws a course name dereferences them.
 */
#include "gdx_course_strings.h"

#include "port_log.h"

#include <stddef.h>
#include <string.h>

/* Archive-file reader, declared inline the way every other consumer does (see port/gdx_audio_lle.c
 * and decomp/src/overlays/ovl_i2/save.c). It hands back the untouched entry -- 0x40-byte Torch
 * header then payload -- because the port registers no libultraship factory for the
 * F-Zero-X-specific resource types. */
extern int GDiffuser_LoadArchiveFileBytes(const char* key, void* out, size_t outSize, size_t* copiedSize);

/* The three tables this module overrides, declared the way the decomp's own translation units
 * reach each other's data. D_i2_80106F14 is the Expansion Kit twin of D_800CF4D8: the EK path
 * PRELOADS a sequence from it (game.c) and STARTS a sequence from D_800CF4D8 (racer.c), so the
 * two must agree or the wrong song is cued. Retail ships them identical; overriding both keeps
 * that true. */
extern char* gTrackNames[];
extern const char* sTrackSubtitles[];
extern unsigned char D_800CF4D8[];
extern unsigned char D_i2_80106F14[];

/* Torch resource header: 0x40 bytes, byte 0 the endianness, 0x04 the type, 0x08 the version. */
#define GDX_TORCH_HEADER_SIZE 0x40u
#define GDX_TORCH_ENDIAN_LITTLE 0u
#define GDX_COURSE_STRINGS_RESTYPE 0x58435354u /* XCST */
#define GDX_COURSE_STRINGS_VERSION 0u

/* Payload counts, four words immediately after the header. */
#define GDX_COURSE_STRINGS_FIELDS 4u

/* Storage the repointed table entries borrow for the rest of the run. */
static char sNameStore[GDX_COURSE_STRINGS_NAME_MAX][GDX_COURSE_STRING_MAX];
static char sSubtitleStore[GDX_COURSE_STRINGS_SUB_MAX][GDX_COURSE_STRING_MAX];
static int sApplied = 0;

static unsigned int gdxReadU32Le(const unsigned char* p) {
    return (unsigned int) p[0] | ((unsigned int) p[1] << 8) | ((unsigned int) p[2] << 16) |
           ((unsigned int) p[3] << 24);
}

/* Round up to the next 4-byte boundary, matching the padding the exporter writes between
 * variable-length strings. */
static unsigned long gdxAlign4(unsigned long value) {
    return (value + 3u) & ~3ul;
}

/* Same test the factory applies before it writes a string: control codes cannot occur inside one
 * of these names, and these strings reach Font_DrawString without further checking. Bytes above
 * 0x7E stay legal because the retail name "sector \243\301" encodes two glyphs that way. */
static int gdxIsRenderableByte(unsigned char b) {
    return (b >= 0x20u) && (b != 0x7Fu);
}

/* Reads `count` length-prefixed strings starting at *cursor into `dest`, advancing the cursor
 * past the padding after each. Returns 1 on success. Every failure mode is a malformed payload
 * rather than a recoverable condition, so there is no partial success to report: a caller that
 * sees 0 must discard the whole struct.
 *
 * `slots` is how many rows `dest` actually has. Strings past that are still consumed from the
 * buffer so the cursor stays correct for the block that follows, they are just not stored -- the
 * recipe may describe more entries than this build applies. */
static int gdxReadStringBlock(const unsigned char* bytes, unsigned long size, unsigned long* cursor,
                              unsigned int count, unsigned int stringMax, char (*dest)[GDX_COURSE_STRING_MAX],
                              unsigned int slots) {
    unsigned int i;

    for (i = 0; i < count; i++) {
        unsigned long length;
        unsigned long j;

        if ((*cursor + 4ul) > size) {
            return 0;
        }
        length = gdxReadU32Le(bytes + *cursor);
        *cursor += 4ul;

        /* stringMax counts the terminator, so the longest storable body is one byte shorter. */
        if (length >= (unsigned long) stringMax) {
            return 0;
        }
        if ((*cursor + length) > size) {
            return 0;
        }

        for (j = 0; j < length; j++) {
            if (!gdxIsRenderableByte(bytes[*cursor + j])) {
                return 0;
            }
        }

        if (i < slots) {
            memcpy(dest[i], bytes + *cursor, (size_t) length);
            dest[i][length] = '\0';
        }

        *cursor = gdxAlign4(*cursor + length);
    }

    return 1;
}

int gdx_course_strings_parse(const unsigned char* bytes, unsigned long size, GdxCourseStrings* out) {
    unsigned long cursor;
    unsigned int nameCount;
    unsigned int subtitleCount;
    unsigned int bgmCount;
    unsigned int stringMax;
    unsigned int i;

    if (out == NULL) {
        return 0;
    }
    memset(out, 0, sizeof(*out));

    if (bytes == NULL) {
        return 0;
    }
    if (size < (GDX_TORCH_HEADER_SIZE + (GDX_COURSE_STRINGS_FIELDS * 4u))) {
        return 0;
    }
    /* Torch stamps the writer's byte order into the header. The port only ever reads little
     * endian, so an archive built on a big-endian host is rejected rather than misread. */
    if (bytes[0] != GDX_TORCH_ENDIAN_LITTLE) {
        return 0;
    }
    if (gdxReadU32Le(bytes + 0x04) != GDX_COURSE_STRINGS_RESTYPE) {
        return 0;
    }
    if (gdxReadU32Le(bytes + 0x08) != GDX_COURSE_STRINGS_VERSION) {
        return 0;
    }

    cursor = GDX_TORCH_HEADER_SIZE;
    nameCount = gdxReadU32Le(bytes + cursor + 0x00);
    subtitleCount = gdxReadU32Le(bytes + cursor + 0x04);
    bgmCount = gdxReadU32Le(bytes + cursor + 0x08);
    stringMax = gdxReadU32Le(bytes + cursor + 0x0C);
    cursor += GDX_COURSE_STRINGS_FIELDS * 4ul;

    /* A payload whose strings can be longer than a slot was written by a newer torch. Refusing is
     * the honest response: silently truncating a course name is worse than showing the retail one. */
    if ((stringMax == 0u) || (stringMax > GDX_COURSE_STRING_MAX)) {
        return 0;
    }
    /* Too few entries to fill the cart's courses means this is not the table it claims to be. */
    if ((nameCount < GDX_COURSE_STRINGS_APPLY_COUNT) || (subtitleCount < GDX_COURSE_STRINGS_APPLY_COUNT) ||
        (bgmCount < GDX_COURSE_STRINGS_APPLY_COUNT)) {
        return 0;
    }
    if (bgmCount > GDX_COURSE_STRINGS_BGM_MAX) {
        return 0;
    }

    if (!gdxReadStringBlock(bytes, size, &cursor, nameCount, stringMax, out->names,
                            GDX_COURSE_STRINGS_NAME_MAX)) {
        memset(out, 0, sizeof(*out));
        return 0;
    }
    if (!gdxReadStringBlock(bytes, size, &cursor, subtitleCount, stringMax, out->subtitles,
                            GDX_COURSE_STRINGS_SUB_MAX)) {
        memset(out, 0, sizeof(*out));
        return 0;
    }

    if ((cursor + bgmCount) > size) {
        memset(out, 0, sizeof(*out));
        return 0;
    }
    for (i = 0; i < bgmCount; i++) {
        out->bgm[i] = bytes[cursor + i];
    }

    /* Report what was STORED, not what was declared, so a caller can loop on these counts without
     * knowing the capacities. */
    out->nameCount = (nameCount < GDX_COURSE_STRINGS_NAME_MAX) ? nameCount : GDX_COURSE_STRINGS_NAME_MAX;
    out->subtitleCount = (subtitleCount < GDX_COURSE_STRINGS_SUB_MAX) ? subtitleCount : GDX_COURSE_STRINGS_SUB_MAX;
    out->bgmCount = bgmCount;
    return 1;
}

/* Comfortably above the worst case the format allows: 63 strings of GDX_COURSE_STRING_MAX-1 bytes
 * each, with their length words and padding, come to roughly 4.3 KB. Static because the struct
 * alone is ~4 KB and this runs on the game thread. */
#define GDX_COURSE_STRINGS_BUFFER 8192u

static unsigned char sPayload[GDX_COURSE_STRINGS_BUFFER];
static GdxCourseStrings sDecoded;

void gdx_course_strings_apply(void) {
    size_t copied = 0;
    unsigned int i;
    unsigned int names = 0;
    unsigned int subtitles = 0;
    unsigned int songs = 0;

    /* Latched on SUCCESS, not on entry. Game init calls this exactly once, so the distinction
     * never shows in the running game; it exists so "no entry in the archive" stays a
     * repeatable no-op rather than a state change. */
    if (sApplied) {
        return;
    }

    if (!GDiffuser_LoadArchiveFileBytes(GDX_COURSE_STRINGS_KEY, sPayload, sizeof(sPayload), &copied)) {
        /* The ordinary case for an archive built before this recipe existed. The compiled-in
         * tables are already correct for the retail game, so there is nothing to say. */
        return;
    }

    if (copied == sizeof(sPayload)) {
        /* A full buffer means the entry was clamped, not read. Worth naming: the parse below will
         * fail and the reason would otherwise look like corruption. */
        gdx_port_logf("[course-strings] entry is larger than the %u-byte read buffer; ignoring it\n",
                      (unsigned) sizeof(sPayload));
        return;
    }

    if (!gdx_course_strings_parse(sPayload, (unsigned long) copied, &sDecoded)) {
        gdx_port_logf("[course-strings] %s is malformed (%u bytes); keeping the built-in tables\n",
                      GDX_COURSE_STRINGS_KEY, (unsigned) copied);
        return;
    }

    sApplied = 1;

    for (i = 0; i < GDX_COURSE_STRINGS_APPLY_COUNT; i++) {
        /* An empty slot is a real value in these tables -- sTrackSubtitles pads with "" past the
         * last cup -- so only the names are guarded. A blank course name would leave the menu with
         * nothing to draw, and the retail string is a better answer than none. */
        if (sDecoded.names[i][0] != '\0') {
            memcpy(sNameStore[i], sDecoded.names[i], GDX_COURSE_STRING_MAX);
            gTrackNames[i] = sNameStore[i];
            names++;
        }

        memcpy(sSubtitleStore[i], sDecoded.subtitles[i], GDX_COURSE_STRING_MAX);
        sTrackSubtitles[i] = sSubtitleStore[i];
        subtitles++;

        D_800CF4D8[i] = sDecoded.bgm[i];
        D_i2_80106F14[i] = sDecoded.bgm[i];
        songs++;
    }

    gdx_port_logf("[course-strings] applied %u names, %u subtitles, %u bgm ids from %s (first \"%s\")\n", names,
                  subtitles, songs, GDX_COURSE_STRINGS_KEY, gTrackNames[0]);
}
