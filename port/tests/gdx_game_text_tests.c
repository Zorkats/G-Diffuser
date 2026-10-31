/* Unit tests for port/gdx_game_text.c, compiled UNMODIFIED alongside this file.
 *
 * The module reaches the decomp's text and one archive reader by extern declaration, so this
 * translation unit defines them. Every payload is synthetic: the strings are test values chosen to
 * exercise the format, not text from any ROM.
 */
#include "../gdx_game_text.h"

#include <stdio.h>
#include <string.h>

/* --- the decomp text the module overrides ---------------------------------------------------- */

static const char* const sOriginal = "ORIGINAL";

/* One row longer than the cart's counts, the way an Expansion Kit build is. */
const char* gDifficultyNames[GDX_GAME_TEXT_DIFFICULTY_COUNT];
const char* sCupNames[GDX_GAME_TEXT_CUP_COUNT + 2];
const char* gCreditsAttributions[GDX_GAME_TEXT_ROLE_COUNT];
const char* gCreditsNames[GDX_GAME_TEXT_NAME_COUNT + 1];
char sThanksForPlayingStr[GDX_GAME_TEXT_ENDING_MESSAGE_MAX + 1];

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

    if (!sStubPresent || (strcmp(key, GDX_GAME_TEXT_KEY) != 0)) {
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

static void putU32(unsigned int value) {
    sBuild[sBuildLen++] = (unsigned char) (value & 0xFFu);
    sBuild[sBuildLen++] = (unsigned char) ((value >> 8) & 0xFFu);
    sBuild[sBuildLen++] = (unsigned char) ((value >> 16) & 0xFFu);
    sBuild[sBuildLen++] = (unsigned char) ((value >> 24) & 0xFFu);
}

static void putString(const char* value) {
    size_t length = strlen(value);

    putU32((unsigned int) length);
    memcpy(sBuild + sBuildLen, value, length);
    sBuildLen += length;
    while ((sBuildLen % 4u) != 0u) {
        sBuild[sBuildLen++] = 0;
    }
}

static void putHeader(unsigned int tables, unsigned int singles, unsigned int stringMax) {
    memset(sBuild, 0, 0x40);
    sBuild[0x04] = 0x58; /* XGTX, little endian */
    sBuild[0x05] = 0x54;
    sBuild[0x06] = 0x47;
    sBuild[0x07] = 0x58;
    sBuildLen = 0x40;
    putU32(tables);
    putU32(singles);
    putU32(stringMax);
}

/* Rows read "<prefix><row>", so a test can tell any two of them apart. */
static void putTable(const char* prefix, unsigned int count) {
    unsigned int i;
    char row[GDX_GAME_TEXT_STRING_MAX];

    putU32(count);
    for (i = 0; i < count; i++) {
        snprintf(row, sizeof(row), "%s%u", prefix, i);
        putString(row);
    }
}

static void buildPayloadWithMessage(const char* endingMessage) {
    putHeader(4, 3, GDX_GAME_TEXT_STRING_MAX);
    putTable("level", GDX_GAME_TEXT_DIFFICULTY_COUNT);
    putTable("cup", GDX_GAME_TEXT_CUP_COUNT);
    putTable("role", GDX_GAME_TEXT_ROLE_COUNT);
    putTable("name", GDX_GAME_TEXT_NAME_COUNT);
    putString(endingMessage);
    putString("trial label");
    putString("crew");
}

static void buildValidPayload(void) {
    buildPayloadWithMessage("so long");
}

/* --- assertions ----------------------------------------------------------------------------- */

static int sFailures = 0;

static void expectInt(const char* what, int actual, int expected) {
    if (actual != expected) {
        printf("FAIL %s: got %d, expected %d\n", what, actual, expected);
        sFailures++;
    }
}

static void expectStr(const char* what, const char* actual, const char* expected) {
    if (strcmp(actual, expected) != 0) {
        printf("FAIL %s: got \"%s\", expected \"%s\"\n", what, actual, expected);
        sFailures++;
    }
}

static void expectRejected(const char* what) {
    static GdxGameText decoded;
    unsigned int i;

    memset(&decoded, 0xAB, sizeof(decoded));
    expectInt(what, gdx_game_text_parse(sBuild, (unsigned long) sBuildLen, &decoded), 0);
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
    static GdxGameText decoded;

    buildValidPayload();
    expectInt("accepts a valid payload", gdx_game_text_parse(sBuild, (unsigned long) sBuildLen, &decoded), 1);
    expectStr("last difficulty", decoded.difficultyNames[3], "level3");
    expectStr("last cup", decoded.cupNames[5], "cup5");
    expectStr("last role", decoded.creditsRoles[30], "role30");
    expectStr("last name", decoded.creditsNames[35], "name35");
    expectStr("ending message", decoded.endingMessage, "so long");
    expectStr("time attack label", decoded.timeAttack, "trial label");
    expectStr("credits header", decoded.creditsStaff, "crew");
}

static void testAcceptsAnEmptyTableRow(void) {
    static GdxGameText decoded;
    unsigned int i;

    /* A hack may blank one credit line, and the game draws that as nothing. */
    putHeader(4, 3, GDX_GAME_TEXT_STRING_MAX);
    putU32(GDX_GAME_TEXT_DIFFICULTY_COUNT);
    for (i = 0; i < GDX_GAME_TEXT_DIFFICULTY_COUNT; i++) {
        putString((i == 1u) ? "" : "level");
    }
    putTable("cup", GDX_GAME_TEXT_CUP_COUNT);
    putTable("role", GDX_GAME_TEXT_ROLE_COUNT);
    putTable("name", GDX_GAME_TEXT_NAME_COUNT);
    putString("so long");
    putString("trial label");
    putString("crew");

    expectInt("accepts a blank table row", gdx_game_text_parse(sBuild, (unsigned long) sBuildLen, &decoded), 1);
    expectStr("the blank row stays blank", decoded.difficultyNames[1], "");
    expectStr("the next row still reads", decoded.difficultyNames[2], "level");
}

static void testRejectsBadHeaders(void) {
    GdxGameText decoded;

    expectInt("rejects a null buffer", gdx_game_text_parse(NULL, 100, &decoded), 0);
    expectInt("rejects a null output", gdx_game_text_parse(sBuild, 100, NULL), 0);

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

    putHeader(3, 3, GDX_GAME_TEXT_STRING_MAX);
    expectRejected("rejects the wrong table count");

    putHeader(4, 2, GDX_GAME_TEXT_STRING_MAX);
    expectRejected("rejects the wrong string count");

    putHeader(4, 3, GDX_GAME_TEXT_STRING_MAX + 1u);
    expectRejected("rejects a larger string slot");
}

static void testRejectsBadBodies(void) {
    size_t firstStringBody = 0x40u + 12u + 4u + 4u;

    buildValidPayload();
    sBuildLen -= 1u;
    expectRejected("rejects a payload cut short");

    /* The first table's count word. */
    buildValidPayload();
    sBuild[0x40u + 12u] = GDX_GAME_TEXT_DIFFICULTY_COUNT - 1u;
    expectRejected("rejects a table of the wrong length");

    /* The first string's length word. */
    buildValidPayload();
    sBuild[0x40u + 12u + 4u] = GDX_GAME_TEXT_STRING_MAX;
    expectRejected("rejects a string past its slot");

    buildValidPayload();
    sBuild[firstStringBody] = 0x01;
    expectRejected("rejects a control byte");

    buildPayloadWithMessage("twenty-one characters");
    expectRejected("rejects a message past the game's array");

    buildPayloadWithMessage("");
    expectRejected("rejects an empty single string");
}

static void testAcceptsAMessageAtCapacity(void) {
    static GdxGameText decoded;

    buildPayloadWithMessage("exactly twenty chars");
    expectInt("accepts a message at capacity", gdx_game_text_parse(sBuild, (unsigned long) sBuildLen, &decoded), 1);
    expectStr("the message is intact", decoded.endingMessage, "exactly twenty chars");
}

/* --- apply ---------------------------------------------------------------------------------- */

static void resetGameText(void) {
    unsigned int i;

    for (i = 0; i < GDX_GAME_TEXT_DIFFICULTY_COUNT; i++) {
        gDifficultyNames[i] = sOriginal;
    }
    for (i = 0; i < (GDX_GAME_TEXT_CUP_COUNT + 2u); i++) {
        sCupNames[i] = sOriginal;
    }
    for (i = 0; i < GDX_GAME_TEXT_ROLE_COUNT; i++) {
        gCreditsAttributions[i] = sOriginal;
    }
    for (i = 0; i < (GDX_GAME_TEXT_NAME_COUNT + 1u); i++) {
        gCreditsNames[i] = sOriginal;
    }
    strcpy(sThanksForPlayingStr, "ORIGINAL MESSAGE");
}

static void testApplyKeepsTextOnBadInput(void) {
    resetGameText();
    sStubPresent = 0;
    gdx_game_text_apply();
    expectInt("no entry leaves the tables alone", gDifficultyNames[0] == sOriginal, 1);
    expectStr("no entry leaves the label alone", gGdxTextTimeAttack, "TIME ATTACK");

    buildValidPayload();
    sBuild[0x04] ^= 0xFFu;
    sStubPayload = sBuild;
    sStubSize = sBuildLen;
    sStubPresent = 1;
    gdx_game_text_apply();
    expectInt("a malformed entry leaves the tables alone", sCupNames[0] == sOriginal, 1);
    expectStr("a malformed entry leaves the message alone", sThanksForPlayingStr, "ORIGINAL MESSAGE");
    expectStr("a malformed entry leaves the header alone", gGdxTextCreditsStaff, "staff");
}

/* Runs last: a successful apply latches for the rest of the process. */
static void testApplyOverridesText(void) {
    const char* first;

    resetGameText();
    buildValidPayload();
    sStubPayload = sBuild;
    sStubSize = sBuildLen;
    sStubPresent = 1;
    gdx_game_text_apply();

    first = gDifficultyNames[0];
    expectStr("difficulty is repointed", gDifficultyNames[3], "level3");
    expectStr("cup is repointed", sCupNames[5], "cup5");
    expectStr("role is repointed", gCreditsAttributions[30], "role30");
    expectStr("name is repointed", gCreditsNames[35], "name35");
    expectStr("the message is copied", sThanksForPlayingStr, "so long");
    expectStr("the label is replaced", gGdxTextTimeAttack, "trial label");
    expectStr("the header is replaced", gGdxTextCreditsStaff, "crew");

    /* The rows the cart has no text for. */
    expectInt("the Expansion Kit cup rows stay", sCupNames[GDX_GAME_TEXT_CUP_COUNT] == sOriginal, 1);
    expectInt("the Expansion Kit credit row stays", gCreditsNames[GDX_GAME_TEXT_NAME_COUNT] == sOriginal, 1);

    sStubPresent = 0;
    gdx_game_text_apply();
    expectInt("a second apply changes nothing", gDifficultyNames[0] == first, 1);
}

int main(void) {
    testAcceptsValidPayload();
    testAcceptsAnEmptyTableRow();
    testRejectsBadHeaders();
    testRejectsBadBodies();
    testAcceptsAMessageAtCapacity();
    testApplyKeepsTextOnBadInput();
    testApplyOverridesText();

    if (sFailures != 0) {
        printf("%d failure(s)\n", sFailures);
        return 1;
    }
    printf("gdx_game_text_tests: all passed\n");
    return 0;
}
