/* Unit tests for port/gdx_bg_sprites.c, compiled UNMODIFIED alongside this file.
 *
 * The module reaches one decomp table and one archive reader by extern declaration, so this
 * translation unit defines both. Every payload is synthetic: ids and angles are test values chosen
 * to exercise the format, not data from any ROM.
 */
#include "../gdx_bg_sprites.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

/* --- the decomp table the module overrides -------------------------------------------------- */

static GdxBgSpriteInit sOriginalList[1] = { { 0, 0.0f } };
GdxBgSpriteInit* sBackgroundSpriteCourseInitData[GDX_BG_SPRITES_COURSE_COUNT];

/* --- port_log.h backing --------------------------------------------------------------------- */

void gdx_port_write_log(const char* message) {
    (void) message;
}

int gGdxDevGateCache[64];
void (*gdx_port_log_tap)(const char* message) = 0;

/* --- stub archive reader -------------------------------------------------------------------- */

static const unsigned char* sStubPayload;
static size_t sStubSize;
static int sStubPresent;

int GDiffuser_LoadArchiveFileBytes(const char* key, void* out, size_t outSize, size_t* copiedSize) {
    size_t copy;

    if (!sStubPresent || (strcmp(key, GDX_BG_SPRITES_KEY) != 0)) {
        return 0;
    }

    copy = (sStubSize < outSize) ? sStubSize : outSize;
    memcpy(out, sStubPayload, copy);
    if (copiedSize != 0) {
        *copiedSize = copy;
    }
    return 1;
}

/* --- payload builder ------------------------------------------------------------------------ */

#define BUILD_MAX 4096

static unsigned char sBuild[BUILD_MAX];
static size_t sBuildLen;

static void putU32(unsigned int value) {
    sBuild[sBuildLen++] = (unsigned char) (value & 0xFFu);
    sBuild[sBuildLen++] = (unsigned char) ((value >> 8) & 0xFFu);
    sBuild[sBuildLen++] = (unsigned char) ((value >> 16) & 0xFFu);
    sBuild[sBuildLen++] = (unsigned char) ((value >> 24) & 0xFFu);
}

static void putF32(float value) {
    unsigned int bits;

    memcpy(&bits, &value, sizeof(bits));
    putU32(bits);
}

static void putHeader(unsigned int courses, unsigned int maxPerCourse) {
    memset(sBuild, 0, 0x40);
    sBuild[0x04] = 0x53; /* XBGS, little endian */
    sBuild[0x05] = 0x47;
    sBuild[0x06] = 0x42;
    sBuild[0x07] = 0x58;
    sBuildLen = 0x40;
    putU32(courses);
    putU32(maxPerCourse);
}

/* Course N gets N % 3 sprites with ids N + 1 and N + 2. */
static void buildValidPayload(void) {
    unsigned int course;

    putHeader(GDX_BG_SPRITES_COURSE_COUNT, GDX_BG_SPRITES_MAX_PER_COURSE);
    for (course = 0; course < GDX_BG_SPRITES_COURSE_COUNT; course++) {
        unsigned int count = course % 3u;
        unsigned int i;

        putU32(count);
        for (i = 0; i < count; i++) {
            putU32(course + 1u + i);
            putF32((float) (course * 10u) + (float) i);
        }
    }
}

/* --- assertions ----------------------------------------------------------------------------- */

static int sFailures = 0;

static void expectInt(const char* what, int actual, int expected) {
    if (actual != expected) {
        printf("FAIL %s: got %d, expected %d\n", what, actual, expected);
        sFailures++;
    }
}

static int parseBuilt(GdxBgSprites* out) {
    return gdx_bg_sprites_parse(sBuild, (unsigned long) sBuildLen, out);
}

/* Offset of the first entry's id in a built payload: course 0 has no sprites, so it is course 1's. */
#define FIRST_ID_OFFSET (0x40u + 8u + 4u + 4u)

static void expectRejected(const char* what) {
    static GdxBgSprites decoded;
    unsigned int i;

    memset(&decoded, 0xAB, sizeof(decoded));
    expectInt(what, parseBuilt(&decoded), 0);
    /* A failed parse must leave nothing half decoded behind. */
    for (i = 0; i < sizeof(decoded); i++) {
        if (((const unsigned char*) &decoded)[i] != 0u) {
            printf("FAIL %s: output not zeroed at byte %u\n", what, i);
            sFailures++;
            break;
        }
    }
}

/* --- parse ---------------------------------------------------------------------------------- */

static void testAcceptsValidPayload(void) {
    static GdxBgSprites decoded;

    buildValidPayload();
    expectInt("accepts a valid payload", parseBuilt(&decoded), 1);
    expectInt("course 0 is empty", decoded.courses[0][0].spriteId, 0);
    expectInt("course 1 first id", decoded.courses[1][0].spriteId, 2);
    expectInt("course 1 first angle", (int) decoded.courses[1][0].angle, 10);
    expectInt("course 1 is terminated", decoded.courses[1][1].spriteId, 0);
    expectInt("course 3 is empty", decoded.courses[3][0].spriteId, 0);
    expectInt("course 2 second id", decoded.courses[2][1].spriteId, 4);
    expectInt("course 2 second angle", (int) decoded.courses[2][1].angle, 21);
    expectInt("course 2 is terminated", decoded.courses[2][2].spriteId, 0);
    /* 8 courses each of 0, 1 and 2 sprites. */
    expectInt("total counts every sprite", (int) decoded.total, 24);
}

static void testAcceptsAFullList(void) {
    static GdxBgSprites decoded;
    unsigned int course;

    putHeader(GDX_BG_SPRITES_COURSE_COUNT, GDX_BG_SPRITES_MAX_PER_COURSE);
    for (course = 0; course < GDX_BG_SPRITES_COURSE_COUNT; course++) {
        unsigned int count = (course == 5u) ? GDX_BG_SPRITES_MAX_PER_COURSE : 0u;
        unsigned int i;

        putU32(count);
        for (i = 0; i < count; i++) {
            putU32(GDX_BG_SPRITES_MAX_ID);
            putF32(-720.0f);
        }
    }

    expectInt("accepts a list at capacity", parseBuilt(&decoded), 1);
    expectInt("the last slot is stored", decoded.courses[5][GDX_BG_SPRITES_MAX_PER_COURSE - 1].spriteId,
              GDX_BG_SPRITES_MAX_ID);
    expectInt("the terminator row stays clear", decoded.courses[5][GDX_BG_SPRITES_MAX_PER_COURSE].spriteId, 0);
}

static void testRejectsBadHeaders(void) {
    GdxBgSprites decoded;

    expectInt("rejects a null buffer", gdx_bg_sprites_parse(NULL, 100, &decoded), 0);
    expectInt("rejects a null output", gdx_bg_sprites_parse(sBuild, 100, NULL), 0);

    buildValidPayload();
    sBuildLen = 0x40;
    expectRejected("rejects a truncated header");

    buildValidPayload();
    sBuild[0] = 1;
    expectRejected("rejects a big-endian archive");

    buildValidPayload();
    sBuild[0x04] ^= 0xFFu;
    expectRejected("rejects the wrong resource type");

    buildValidPayload();
    sBuild[0x08] = 1;
    expectRejected("rejects a newer version");

    putHeader(GDX_BG_SPRITES_COURSE_COUNT - 1u, GDX_BG_SPRITES_MAX_PER_COURSE);
    expectRejected("rejects the wrong course count");

    putHeader(GDX_BG_SPRITES_COURSE_COUNT, GDX_BG_SPRITES_MAX_PER_COURSE + 1u);
    expectRejected("rejects a larger capacity");
}

static void testRejectsBadLists(void) {
    float angle;

    buildValidPayload();
    sBuildLen -= 1u;
    expectRejected("rejects a payload cut short");

    /* Course 0 declares more sprites than the game's array holds. */
    buildValidPayload();
    sBuild[0x40u + 8u] = GDX_BG_SPRITES_MAX_PER_COURSE + 1u;
    expectRejected("rejects a list past capacity");

    buildValidPayload();
    sBuild[FIRST_ID_OFFSET] = 0u;
    expectRejected("rejects the terminator id inside a list");

    buildValidPayload();
    sBuild[FIRST_ID_OFFSET] = GDX_BG_SPRITES_MAX_ID + 1u;
    expectRejected("rejects an id past the enum");

    buildValidPayload();
    angle = 721.0f;
    memcpy(sBuild + FIRST_ID_OFFSET + 4u, &angle, sizeof(angle));
    expectRejected("rejects an angle out of range");

    buildValidPayload();
    memset(sBuild + FIRST_ID_OFFSET + 4u, 0xFF, 4u);
    expectRejected("rejects a NaN angle");
}

/* --- apply ---------------------------------------------------------------------------------- */

static void resetTable(void) {
    unsigned int i;

    for (i = 0; i < GDX_BG_SPRITES_COURSE_COUNT; i++) {
        sBackgroundSpriteCourseInitData[i] = sOriginalList;
    }
}

static void testApplyKeepsTableOnBadInput(void) {
    resetTable();
    sStubPresent = 0;
    gdx_bg_sprites_apply();
    expectInt("no entry leaves the table alone", sBackgroundSpriteCourseInitData[1] == sOriginalList, 1);

    buildValidPayload();
    sBuild[0x04] ^= 0xFFu;
    sStubPayload = sBuild;
    sStubSize = sBuildLen;
    sStubPresent = 1;
    gdx_bg_sprites_apply();
    expectInt("a malformed entry leaves the table alone", sBackgroundSpriteCourseInitData[1] == sOriginalList, 1);
}

/* Runs last: a successful apply latches for the rest of the process. */
static void testApplyRepointsTable(void) {
    GdxBgSpriteInit* first;

    resetTable();
    buildValidPayload();
    sStubPayload = sBuild;
    sStubSize = sBuildLen;
    sStubPresent = 1;
    gdx_bg_sprites_apply();

    first = sBackgroundSpriteCourseInitData[1];
    expectInt("apply repoints the table", first != sOriginalList, 1);
    expectInt("the repointed list holds the id", first[0].spriteId, 2);
    expectInt("the repointed list holds the angle", (int) first[0].angle, 10);
    expectInt("the repointed list is terminated", first[1].spriteId, 0);
    expectInt("an empty course reads as terminated", sBackgroundSpriteCourseInitData[0][0].spriteId, 0);

    sStubPresent = 0;
    gdx_bg_sprites_apply();
    expectInt("a second apply changes nothing", sBackgroundSpriteCourseInitData[1] == first, 1);
}

int main(void) {
    testAcceptsValidPayload();
    testAcceptsAFullList();
    testRejectsBadHeaders();
    testRejectsBadLists();
    testApplyKeepsTableOnBadInput();
    testApplyRepointsTable();

    if (sFailures != 0) {
        printf("%d failure(s)\n", sFailures);
        return 1;
    }
    printf("gdx_bg_sprites_tests: all passed\n");
    return 0;
}
