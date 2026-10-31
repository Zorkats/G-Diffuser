/* Runtime half of FZX:BG_SPRITES. See port/gdx_bg_sprites.h for why this exists.
 *
 * The same trick port/gdx_course_strings.c uses: the game's table is an array of pointers, so
 * nothing is patched in place. Decoded lists go into static storage owned by this file and the
 * array entries are REPOINTED at it.
 *
 * Applied once and never freed, deliberately: the table holds borrowed pointers for the whole run.
 */
#include "gdx_bg_sprites.h"

#include "port_log.h"

#include <math.h>
#include <stddef.h>
#include <string.h>

/* Archive-file reader, declared inline the way every other consumer does (see
 * port/gdx_course_strings.c). It hands back the untouched entry, 0x40-byte Torch header then
 * payload. */
extern int GDiffuser_LoadArchiveFileBytes(const char* key, void* out, size_t outSize, size_t* copiedSize);

/* decomp/src/overlays/ovl_i3/background.c, reached the way the decomp's own translation units reach
 * each other's data. */
extern GdxBgSpriteInit* sBackgroundSpriteCourseInitData[];

/* Torch resource header: 0x40 bytes, byte 0 the endianness, 0x04 the type, 0x08 the version. */
#define GDX_TORCH_HEADER_SIZE 0x40u
#define GDX_TORCH_ENDIAN_LITTLE 0u
#define GDX_BG_SPRITES_RESTYPE 0x58424753u /* XBGS */
#define GDX_BG_SPRITES_VERSION 0u

/* Same bound the factory applies before it writes an angle. */
#define GDX_BG_SPRITES_MAX_ANGLE 720.0f

static unsigned int gdxReadU32Le(const unsigned char* p) {
    return (unsigned int) p[0] | ((unsigned int) p[1] << 8) | ((unsigned int) p[2] << 16) |
           ((unsigned int) p[3] << 24);
}

static float gdxReadF32Le(const unsigned char* p) {
    unsigned int bits = gdxReadU32Le(p);
    float value;

    memcpy(&value, &bits, sizeof(value));
    return value;
}

int gdx_bg_sprites_parse(const unsigned char* bytes, unsigned long size, GdxBgSprites* out) {
    unsigned long cursor;
    unsigned int courseCount;
    unsigned int maxPerCourse;
    unsigned int course;

    if (out == NULL) {
        return 0;
    }
    memset(out, 0, sizeof(*out));

    if ((bytes == NULL) || (size < (GDX_TORCH_HEADER_SIZE + 8u))) {
        return 0;
    }
    /* The port only ever reads little endian, so an archive built on a big-endian host is rejected
     * rather than misread. */
    if (bytes[0] != GDX_TORCH_ENDIAN_LITTLE) {
        return 0;
    }
    if ((gdxReadU32Le(bytes + 0x04) != GDX_BG_SPRITES_RESTYPE) ||
        (gdxReadU32Le(bytes + 0x08) != GDX_BG_SPRITES_VERSION)) {
        return 0;
    }

    cursor = GDX_TORCH_HEADER_SIZE;
    courseCount = gdxReadU32Le(bytes + cursor);
    maxPerCourse = gdxReadU32Le(bytes + cursor + 4ul);
    cursor += 8ul;

    if ((courseCount != GDX_BG_SPRITES_COURSE_COUNT) || (maxPerCourse != GDX_BG_SPRITES_MAX_PER_COURSE)) {
        return 0;
    }

    for (course = 0; course < courseCount; course++) {
        unsigned int count;
        unsigned int i;

        if ((cursor + 4ul) > size) {
            goto malformed;
        }
        count = gdxReadU32Le(bytes + cursor);
        cursor += 4ul;

        if ((count > GDX_BG_SPRITES_MAX_PER_COURSE) || ((cursor + (count * 8ul)) > size)) {
            goto malformed;
        }

        for (i = 0; i < count; i++) {
            unsigned int id = gdxReadU32Le(bytes + cursor);
            float angle = gdxReadF32Le(bytes + cursor + 4ul);

            /* These index the game's texture table and feed its trig, so the factory's checks are
             * repeated here rather than trusted across the archive boundary. */
            if ((id == 0u) || (id > GDX_BG_SPRITES_MAX_ID) || !isfinite(angle) ||
                (fabsf(angle) > GDX_BG_SPRITES_MAX_ANGLE)) {
                goto malformed;
            }

            out->courses[course][i].spriteId = (short) id;
            out->courses[course][i].angle = angle;
            out->total++;
            cursor += 8ul;
        }
    }

    return 1;

malformed:
    memset(out, 0, sizeof(*out));
    return 0;
}

/* Worst case the format allows is 24 lists of 6 entries with their counts: under 1.3 KB of payload
 * after the header. */
#define GDX_BG_SPRITES_BUFFER 4096u

static unsigned char sPayload[GDX_BG_SPRITES_BUFFER];
static GdxBgSprites sDecoded;
static int sApplied = 0;

void gdx_bg_sprites_apply(void) {
    size_t copied = 0;
    unsigned int i;

    /* Latched on SUCCESS, not on entry, so "no entry in the archive" stays a repeatable no-op. */
    if (sApplied) {
        return;
    }

    if (!GDiffuser_LoadArchiveFileBytes(GDX_BG_SPRITES_KEY, sPayload, sizeof(sPayload), &copied)) {
        /* The ordinary case for an archive built before this recipe existed. The compiled-in table
         * is already correct for the retail game, so there is nothing to say. */
        return;
    }

    if ((copied == sizeof(sPayload)) || !gdx_bg_sprites_parse(sPayload, (unsigned long) copied, &sDecoded)) {
        gdx_port_logf("[bg-sprites] %s is malformed (%u bytes); keeping the built-in tables\n", GDX_BG_SPRITES_KEY,
                      (unsigned) copied);
        return;
    }

    sApplied = 1;

    for (i = 0; i < GDX_BG_SPRITES_COURSE_COUNT; i++) {
        sBackgroundSpriteCourseInitData[i] = sDecoded.courses[i];
    }

    gdx_port_logf("[bg-sprites] applied %u sprites across %u courses from %s\n", sDecoded.total,
                  (unsigned) GDX_BG_SPRITES_COURSE_COUNT, GDX_BG_SPRITES_KEY);
}
