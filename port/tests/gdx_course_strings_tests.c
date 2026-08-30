/* Unit tests for port/gdx_course_strings.c, compiled UNMODIFIED alongside this file.
 *
 * The module reaches four decomp tables and one archive reader by extern declaration, so this
 * translation unit defines all five. That is what lets gdx_course_strings_apply() run end to end
 * here with no archive, no ROM and no game: the stub reader hands back whatever payload the test
 * built, and the tables are ordinary arrays the assertions can read afterwards.
 *
 * Payloads are assembled by hand rather than captured from a real .o2r, so a change to the wire
 * format shows up as a failing test rather than as a silently ignored archive entry.
 */
#include "../gdx_course_strings.h"

#include <stdio.h>
#include <string.h>

/* --- the decomp tables the module overrides ------------------------------------------------- */

char* gTrackNames[55];
const char* sTrackSubtitles[GDX_COURSE_STRINGS_SUB_MAX];
unsigned char D_800CF4D8[GDX_COURSE_STRINGS_BGM_MAX];
unsigned char D_i2_80106F14[GDX_COURSE_STRINGS_BGM_MAX];

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

    if (!sStubPresent || (strcmp(key, GDX_COURSE_STRINGS_KEY) != 0)) {
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

#define BUILD_MAX 8192

static unsigned char sBuild[BUILD_MAX];
static size_t sBuildLen;

/* Set by the empty-string test; buildPayload emits "" for entry 0 when it is on. */
static int sBlankFirstName;

static void putU32(size_t at, unsigned int value) {
    sBuild[at + 0] = (unsigned char) (value & 0xFFu);
    sBuild[at + 1] = (unsigned char) ((value >> 8) & 0xFFu);
    sBuild[at + 2] = (unsigned char) ((value >> 16) & 0xFFu);
    sBuild[at + 3] = (unsigned char) ((value >> 24) & 0xFFu);
}

static void appendU32(unsigned int value) {
    putU32(sBuildLen, value);
    sBuildLen += 4;
}

static void appendString(const char* text) {
    size_t length = strlen(text);

    appendU32((unsigned int) length);
    memcpy(sBuild + sBuildLen, text, length);
    sBuildLen += length;
    while ((sBuildLen % 4) != 0) {
        sBuild[sBuildLen++] = 0;
    }
}

/* A well-formed payload: `names` course names, `subs` subtitles, 24 BGM ids counting up from 0.
 * Entry i gets the name "name<i>" and the subtitle "SUB<i>" unless overridden by the caller
 * afterwards, which is what the malformed-payload tests do. */
static void buildPayload(unsigned int names, unsigned int subs, unsigned int bgm, unsigned int stringMax) {
    unsigned int i;
    char scratch[GDX_COURSE_STRING_MAX];

    memset(sBuild, 0, sizeof(sBuild));
    sBuildLen = 0x40;
    sBuild[0] = 0; /* little endian */
    putU32(0x04, 0x58435354u);
    putU32(0x08, 0u);

    appendU32(names);
    appendU32(subs);
    appendU32(bgm);
    appendU32(stringMax);

    for (i = 0; i < names; i++) {
        if ((i == 0) && sBlankFirstName) {
            appendString("");
            continue;
        }
        sprintf(scratch, "name%u", i);
        appendString(scratch);
    }
    for (i = 0; i < subs; i++) {
        sprintf(scratch, "SUB%u", i);
        appendString(scratch);
    }
    for (i = 0; i < bgm; i++) {
        sBuild[sBuildLen++] = (unsigned char) i;
    }
    while ((sBuildLen % 4) != 0) {
        sBuild[sBuildLen++] = 0;
    }
}

/* --- assertions ----------------------------------------------------------------------------- */

static int sFailures;

static void expectInt(const char* label, long actual, long expected) {
    if (actual != expected) {
        fprintf(stderr, "FAIL: %s: got %ld, expected %ld\n", label, actual, expected);
        sFailures++;
        return;
    }
    printf("PASS: %s\n", label);
}

static void expectString(const char* label, const char* actual, const char* expected) {
    if ((actual == 0) || (strcmp(actual, expected) != 0)) {
        fprintf(stderr, "FAIL: %s: got \"%s\", expected \"%s\"\n", label, (actual != 0) ? actual : "(null)",
                expected);
        sFailures++;
        return;
    }
    printf("PASS: %s\n", label);
}

static void expectReject(const char* label) {
    GdxCourseStrings decoded;

    memset(&decoded, 0xAA, sizeof(decoded));
    if (gdx_course_strings_parse(sBuild, (unsigned long) sBuildLen, &decoded)) {
        fprintf(stderr, "FAIL: %s: payload was accepted\n", label);
        sFailures++;
        return;
    }
    /* A rejected payload must leave nothing half-decoded behind. */
    if ((decoded.nameCount != 0u) || (decoded.subtitleCount != 0u) || (decoded.bgmCount != 0u)) {
        fprintf(stderr, "FAIL: %s: rejected but counts were left set\n", label);
        sFailures++;
        return;
    }
    printf("PASS: %s\n", label);
}

/* --- tests ---------------------------------------------------------------------------------- */

static void testAcceptsAWellFormedPayload(void) {
    GdxCourseStrings decoded;

    buildPayload(32, 31, 24, GDX_COURSE_STRING_MAX);
    expectInt("accepts a full payload", gdx_course_strings_parse(sBuild, (unsigned long) sBuildLen, &decoded), 1);
    expectInt("name count", (long) decoded.nameCount, 32);
    expectInt("subtitle count", (long) decoded.subtitleCount, 31);
    expectInt("bgm count", (long) decoded.bgmCount, 24);
    expectString("first name", decoded.names[0], "name0");
    expectString("last name", decoded.names[31], "name31");
    expectString("first subtitle", decoded.subtitles[0], "SUB0");
    expectString("last subtitle", decoded.subtitles[30], "SUB30");
    expectInt("first bgm id", decoded.bgm[0], 0);
    expectInt("last bgm id", decoded.bgm[23], 23);
}

static void testAcceptsEmptyStrings(void) {
    GdxCourseStrings decoded;

    /* sTrackSubtitles pads with "" past the last cup, so a zero-length entry is ordinary data and
     * must not be mistaken for the end of the block. */
    sBlankFirstName = 1;
    buildPayload(32, 31, 24, GDX_COURSE_STRING_MAX);
    sBlankFirstName = 0;

    expectInt("accepts an empty string", gdx_course_strings_parse(sBuild, (unsigned long) sBuildLen, &decoded), 1);
    expectString("empty name decodes to nothing", decoded.names[0], "");
    expectString("the entry after it is unshifted", decoded.names[1], "name1");
    expectString("the subtitle block still lines up", decoded.subtitles[0], "SUB0");
}

static void testRejectsMalformedPayloads(void) {
    buildPayload(32, 31, 24, GDX_COURSE_STRING_MAX);
    putU32(0x04, 0x58435253u); /* XCRS: a real Torch type, but the wrong one */
    expectReject("rejects the wrong resource type");

    buildPayload(32, 31, 24, GDX_COURSE_STRING_MAX);
    putU32(0x08, 1u);
    expectReject("rejects a future version");

    buildPayload(32, 31, 24, GDX_COURSE_STRING_MAX);
    sBuild[0] = 1; /* Torch::Endianness::Big */
    expectReject("rejects a big-endian payload");

    buildPayload(32, 31, 24, GDX_COURSE_STRING_MAX + 1);
    expectReject("rejects a string bound this build cannot store");

    buildPayload(23, 31, 24, GDX_COURSE_STRING_MAX);
    expectReject("rejects too few names");

    buildPayload(32, 23, 24, GDX_COURSE_STRING_MAX);
    expectReject("rejects too few subtitles");

    buildPayload(32, 31, 23, GDX_COURSE_STRING_MAX);
    expectReject("rejects too few bgm ids");

    buildPayload(32, 31, 24, GDX_COURSE_STRING_MAX);
    putU32(0x40 + 16, 4096u);
    expectReject("rejects a length that runs past the buffer");

    buildPayload(32, 31, 24, GDX_COURSE_STRING_MAX);
    sBuild[0x40 + 16 + 4] = 0x07; /* a bell inside "name0" */
    expectReject("rejects a control byte inside a string");

    buildPayload(32, 31, 24, GDX_COURSE_STRING_MAX);
    sBuildLen -= 8;
    expectReject("rejects a truncated payload");

    buildPayload(32, 31, 24, GDX_COURSE_STRING_MAX);
    sBuildLen = 0x40;
    expectReject("rejects a header with no payload");
}

/* --- apply path ----------------------------------------------------------------------------- */

static const char* const kRetailNames[GDX_COURSE_STRINGS_APPLY_COUNT] = {
    "mute city",  "silence",     "sand ocean",       "devil's forest", "big blue",     "port town",
    "sector a",   "red canyon",  "devil's forest 2", "mute city 2",    "big blue 2",   "white land",
    "fire field", "silence 2",   "sector b",         "red canyon 2",   "white land 2", "mute city 3",
    "rainbow road", "devil's forest 3", "space plant", "sand ocean 2", "port town 2", "big hand",
};

static const char* const kRetailSubtitles[GDX_COURSE_STRINGS_APPLY_COUNT] = {
    "FIGURE EIGHT", "HIGH SPEED",  "PIPE",      "CORKSCREW",   "CYLINDER",           "HIGH JUMP",
    "INVERTED LOOP", "MULTI JUMP", "UP AND DOWN", "TECHNIQUE", "QUICK TURN",         "DANGEROUS STEPS",
    "ZIG-ZAG JUMP", "WAVY ROAD",   "DOUBLE SOMERSAULT", "SLIM LINE", "HALF PIPE",    "JUMPS OF DOOM",
    "PSYCHEDELIC EXPERIENCE", "MIRROR ROAD", "CYLINDER & HIGH JUMP", "WAVE PANIC", "SNAKE ROAD", "DEADLY CURVES",
};

/* Puts the tables back where func_8007D9D0() and the compiled-in arrays would leave them. */
static void resetTables(void) {
    unsigned int i;

    for (i = 0; i < GDX_COURSE_STRINGS_APPLY_COUNT; i++) {
        gTrackNames[i] = (char*) kRetailNames[i];
        sTrackSubtitles[i] = kRetailSubtitles[i];
        D_800CF4D8[i] = 99;
        D_i2_80106F14[i] = 99;
    }
}

static void testApplyIsANoOpWithoutAnEntry(void) {
    resetTables();
    sStubPresent = 0;
    gdx_course_strings_apply();

    expectString("no entry leaves the name alone", gTrackNames[0], "mute city");
    expectString("no entry leaves the subtitle alone", sTrackSubtitles[0], "FIGURE EIGHT");
    expectInt("no entry leaves the bgm id alone", D_800CF4D8[0], 99);
}

static void testApplyIgnoresAMalformedEntry(void) {
    resetTables();
    buildPayload(32, 31, 24, GDX_COURSE_STRING_MAX);
    putU32(0x08, 1u); /* a version this build does not know */
    sStubPayload = sBuild;
    sStubSize = sBuildLen;
    sStubPresent = 1;
    gdx_course_strings_apply();

    expectString("a malformed entry leaves the name alone", gTrackNames[0], "mute city");
    expectInt("a malformed entry leaves the bgm id alone", D_800CF4D8[0], 99);
}

static void testApplyOverridesTheTables(void) {
    resetTables();
    buildPayload(32, 31, 24, GDX_COURSE_STRING_MAX);
    sStubPayload = sBuild;
    sStubSize = sBuildLen;
    sStubPresent = 1;
    gdx_course_strings_apply();

    expectString("first name replaced", gTrackNames[0], "name0");
    expectString("last applied name replaced", gTrackNames[23], "name23");
    expectString("first subtitle replaced", sTrackSubtitles[0], "SUB0");
    expectString("last applied subtitle replaced", sTrackSubtitles[23], "SUB23");
    expectInt("first bgm id replaced", D_800CF4D8[0], 0);
    expectInt("last applied bgm id replaced", D_800CF4D8[23], 23);
    /* The Expansion Kit preload table has to agree with the one racer.c starts from. */
    expectInt("the expansion kit twin agrees", D_i2_80106F14[23], D_800CF4D8[23]);

    /* Nothing past the cart's own courses may be touched: gTrackNames[24..29] belong to the
     * Course Edit cup and are filled from the save file. */
    expectInt("the course edit slot is untouched", (gTrackNames[24] == 0) ? 1 : 0, 1);
    expectString("subtitles past the applied range are untouched", sTrackSubtitles[24], "");
}

static void testApplyRunsOnlyOnce(void) {
    /* The previous test already applied. A second call with different data must be ignored:
     * gTrackNames[] hands out borrowed pointers for the whole run and re-pointing them mid-session
     * is not something any caller is prepared for. */
    buildPayload(32, 31, 24, GDX_COURSE_STRING_MAX);
    memcpy(sBuild + 0x40 + 16 + 4, "ZZZZZ", 5);
    sStubPayload = sBuild;
    sStubSize = sBuildLen;
    sStubPresent = 1;
    gdx_course_strings_apply();

    expectString("a second apply is ignored", gTrackNames[0], "name0");
}

int main(void) {
    /* sTrackSubtitles past the applied range starts empty, matching the retail array. */
    unsigned int i;
    for (i = GDX_COURSE_STRINGS_APPLY_COUNT; i < GDX_COURSE_STRINGS_SUB_MAX; i++) {
        sTrackSubtitles[i] = "";
    }

    testAcceptsAWellFormedPayload();
    testAcceptsEmptyStrings();
    testRejectsMalformedPayloads();
    testApplyIsANoOpWithoutAnEntry();
    testApplyIgnoresAMalformedEntry();
    testApplyOverridesTheTables();
    testApplyRunsOnlyOnce();

    if (sFailures != 0) {
        fprintf(stderr, "%d course-strings test(s) failed\n", sFailures);
        return 1;
    }
    printf("All course-strings tests passed\n");
    return 0;
}
