/* Runtime half of FZX:GAME_TEXT. See port/gdx_game_text.h for why this exists.
 *
 * The same trick port/gdx_course_strings.c uses: the tables are arrays of char*, so nothing is
 * patched in place. Decoded strings go into static storage owned by this file and the array entries
 * are REPOINTED at it. sThanksForPlayingStr is a real char array, so it is overwritten instead.
 *
 * Applied once and never freed, deliberately: the tables hold borrowed pointers for the whole run.
 */
#include "gdx_game_text.h"

#include "port_log.h"

#include <stddef.h>
#include <string.h>

/* Archive-file reader, declared inline the way every other consumer does (see
 * port/gdx_course_strings.c). It hands back the untouched entry, 0x40-byte Torch header then
 * payload. */
extern int GDiffuser_LoadArchiveFileBytes(const char* key, void* out, size_t outSize, size_t* copiedSize);

/* The decomp's text, reached the way its own translation units reach each other's data. */
extern const char* gDifficultyNames[];
extern const char* sCupNames[];
extern const char* gCreditsAttributions[];
extern const char* gCreditsNames[];
extern char sThanksForPlayingStr[];

/* Torch resource header: 0x40 bytes, byte 0 the endianness, 0x04 the type, 0x08 the version. */
#define GDX_TORCH_HEADER_SIZE 0x40u
#define GDX_TORCH_ENDIAN_LITTLE 0u
#define GDX_GAME_TEXT_RESTYPE 0x58475458u /* XGTX */
#define GDX_GAME_TEXT_VERSION 0u

/* Payload counts, three words immediately after the header. */
#define GDX_GAME_TEXT_FIELDS 3u
#define GDX_GAME_TEXT_TABLE_COUNT 4u
#define GDX_GAME_TEXT_SINGLE_COUNT 3u

const char* gGdxTextTimeAttack = "TIME ATTACK";
const char* gGdxTextCreditsStaff = "staff";

static unsigned int gdxReadU32Le(const unsigned char* p) {
    return (unsigned int) p[0] | ((unsigned int) p[1] << 8) | ((unsigned int) p[2] << 16) |
           ((unsigned int) p[3] << 24);
}

/* Same test the factory applies before it writes a string. These strings reach Font_DrawString
 * without further checking. */
static int gdxIsRenderableByte(unsigned char b) {
    return (b >= 0x20u) && (b != 0x7Fu);
}

/* Reads one length-prefixed string at *cursor into `dest`, then skips the padding after it. */
static int gdxReadString(const unsigned char* bytes, unsigned long size, unsigned long* cursor, char* dest) {
    unsigned long length;
    unsigned long i;

    if ((*cursor + 4ul) > size) {
        return 0;
    }
    length = gdxReadU32Le(bytes + *cursor);
    *cursor += 4ul;

    if ((length >= GDX_GAME_TEXT_STRING_MAX) || ((*cursor + length) > size)) {
        return 0;
    }
    for (i = 0; i < length; i++) {
        if (!gdxIsRenderableByte(bytes[*cursor + i])) {
            return 0;
        }
    }

    memcpy(dest, bytes + *cursor, (size_t) length);
    dest[length] = '\0';
    *cursor = (*cursor + length + 3ul) & ~3ul;
    return 1;
}

/* A table must be the length this build expects: its rows are applied by position. */
static int gdxReadTable(const unsigned char* bytes, unsigned long size, unsigned long* cursor, unsigned int expected,
                        char (*dest)[GDX_GAME_TEXT_STRING_MAX]) {
    unsigned int i;

    if ((*cursor + 4ul) > size) {
        return 0;
    }
    if (gdxReadU32Le(bytes + *cursor) != expected) {
        return 0;
    }
    *cursor += 4ul;

    for (i = 0; i < expected; i++) {
        if (!gdxReadString(bytes, size, cursor, dest[i])) {
            return 0;
        }
    }
    return 1;
}

int gdx_game_text_parse(const unsigned char* bytes, unsigned long size, GdxGameText* out) {
    unsigned long cursor;

    if (out == NULL) {
        return 0;
    }
    memset(out, 0, sizeof(*out));

    if ((bytes == NULL) || (size < (GDX_TORCH_HEADER_SIZE + (GDX_GAME_TEXT_FIELDS * 4u)))) {
        return 0;
    }
    /* The port only ever reads little endian, so an archive built on a big-endian host is rejected
     * rather than misread. */
    if (bytes[0] != GDX_TORCH_ENDIAN_LITTLE) {
        return 0;
    }
    if ((gdxReadU32Le(bytes + 0x04) != GDX_GAME_TEXT_RESTYPE) || (gdxReadU32Le(bytes + 0x08) != GDX_GAME_TEXT_VERSION)) {
        return 0;
    }

    cursor = GDX_TORCH_HEADER_SIZE;
    if ((gdxReadU32Le(bytes + cursor + 0x00) != GDX_GAME_TEXT_TABLE_COUNT) ||
        (gdxReadU32Le(bytes + cursor + 0x04) != GDX_GAME_TEXT_SINGLE_COUNT) ||
        (gdxReadU32Le(bytes + cursor + 0x08) != GDX_GAME_TEXT_STRING_MAX)) {
        return 0;
    }
    cursor += GDX_GAME_TEXT_FIELDS * 4ul;

    if (!gdxReadTable(bytes, size, &cursor, GDX_GAME_TEXT_DIFFICULTY_COUNT, out->difficultyNames) ||
        !gdxReadTable(bytes, size, &cursor, GDX_GAME_TEXT_CUP_COUNT, out->cupNames) ||
        !gdxReadTable(bytes, size, &cursor, GDX_GAME_TEXT_ROLE_COUNT, out->creditsRoles) ||
        !gdxReadTable(bytes, size, &cursor, GDX_GAME_TEXT_NAME_COUNT, out->creditsNames) ||
        !gdxReadString(bytes, size, &cursor, out->endingMessage) ||
        !gdxReadString(bytes, size, &cursor, out->timeAttack) ||
        !gdxReadString(bytes, size, &cursor, out->creditsStaff)) {
        goto malformed;
    }

    /* The factory refuses empty single strings because they mean its offset landed on padding, and
     * the ending message is copied into an array of fixed size. Neither is trusted across the
     * archive boundary. */
    if ((out->endingMessage[0] == '\0') || (out->timeAttack[0] == '\0') || (out->creditsStaff[0] == '\0') ||
        (strlen(out->endingMessage) > GDX_GAME_TEXT_ENDING_MESSAGE_MAX)) {
        goto malformed;
    }

    return 1;

malformed:
    memset(out, 0, sizeof(*out));
    return 0;
}

/* Worst case the format allows is 77 rows of 31 bytes with their length words and padding: under
 * 3 KB of payload after the header. */
#define GDX_GAME_TEXT_BUFFER 4096u

static unsigned char sPayload[GDX_GAME_TEXT_BUFFER];
static GdxGameText sDecoded;
static int sApplied = 0;

static void gdxRepoint(const char** table, char (*rows)[GDX_GAME_TEXT_STRING_MAX], unsigned int count) {
    unsigned int i;

    for (i = 0; i < count; i++) {
        table[i] = rows[i];
    }
}

void gdx_game_text_apply(void) {
    size_t copied = 0;

    /* Latched on SUCCESS, not on entry, so "no entry in the archive" stays a repeatable no-op. */
    if (sApplied) {
        return;
    }

    if (!GDiffuser_LoadArchiveFileBytes(GDX_GAME_TEXT_KEY, sPayload, sizeof(sPayload), &copied)) {
        /* The ordinary case for an archive built before this recipe existed. The compiled-in text is
         * already correct for the retail game, so there is nothing to say. */
        return;
    }

    if ((copied == sizeof(sPayload)) || !gdx_game_text_parse(sPayload, (unsigned long) copied, &sDecoded)) {
        gdx_port_logf("[game-text] %s is malformed (%u bytes); keeping the built-in text\n", GDX_GAME_TEXT_KEY,
                      (unsigned) copied);
        return;
    }

    sApplied = 1;

    gdxRepoint(gDifficultyNames, sDecoded.difficultyNames, GDX_GAME_TEXT_DIFFICULTY_COUNT);
    gdxRepoint(sCupNames, sDecoded.cupNames, GDX_GAME_TEXT_CUP_COUNT);
    gdxRepoint(gCreditsAttributions, sDecoded.creditsRoles, GDX_GAME_TEXT_ROLE_COUNT);
    gdxRepoint(gCreditsNames, sDecoded.creditsNames, GDX_GAME_TEXT_NAME_COUNT);

    /* The parse bounded this to the array's capacity, and +1 carries the terminator. */
    memcpy(sThanksForPlayingStr, sDecoded.endingMessage, strlen(sDecoded.endingMessage) + 1u);

    gGdxTextTimeAttack = sDecoded.timeAttack;
    gGdxTextCreditsStaff = sDecoded.creditsStaff;

    gdx_port_logf("[game-text] applied from %s (time attack \"%s\", first difficulty \"%s\", first cup \"%s\")\n",
                  GDX_GAME_TEXT_KEY, gGdxTextTimeAttack, gDifficultyNames[0], sCupNames[0]);
}
