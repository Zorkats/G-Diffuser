/* Unit tests for port/gdx_lap_digits.c, compiled UNMODIFIED alongside this file.
 *
 * Every atlas is synthetic: pixels hold test values chosen to show where a digit pixel came from, not
 * art from any ROM.
 */
#include "../gdx_lap_digits.h"

#include <stdio.h>
#include <string.h>

/* --- port_log.h backing --------------------------------------------------------------------- */

void gdx_port_write_log(const char* message) {
    (void) message;
}

int gGdxDevGateCache[64];
void (*gdx_port_log_tap)(const char* message) = 0;

/* --- stub archive reader -------------------------------------------------------------------- */

#define ATLAS_PIXELS (GDX_LAP_DIGITS_ATLAS_WIDTH * GDX_LAP_DIGITS_ATLAS_HEIGHT)

static unsigned char sEntry[GDX_LAP_DIGITS_ENTRY_SIZE];
static size_t sStubSize;
static int sStubPresent;
static int sStubReads;

int GDiffuser_LoadArchiveFileBytes(const char* key, void* out, size_t outSize, size_t* copiedSize) {
    size_t copy;

    sStubReads++;
    if (!sStubPresent || (strcmp(key, GDX_LAP_DIGITS_ATLAS_KEY) != 0)) {
        return 0;
    }

    copy = (sStubSize < outSize) ? sStubSize : outSize;
    memcpy(out, sEntry, copy);
    if (copiedSize != 0) {
        *copiedSize = copy;
    }
    return 1;
}

/* --- entry builder -------------------------------------------------------------------------- */

static void putU32(size_t offset, unsigned int value) {
    sEntry[offset] = (unsigned char) (value & 0xFFu);
    sEntry[offset + 1] = (unsigned char) ((value >> 8) & 0xFFu);
    sEntry[offset + 2] = (unsigned char) ((value >> 16) & 0xFFu);
    sEntry[offset + 3] = (unsigned char) ((value >> 24) & 0xFFu);
}

static void buildHeader(unsigned int width, unsigned int height) {
    memset(sEntry, 0, GDX_LAP_DIGITS_ENTRY_PIXELS);
    putU32(0x44, width);
    putU32(0x48, height);
    sStubSize = sizeof(sEntry);
}

/* Each pixel holds its own atlas index, big-endian, so a composed pixel names its source. */
static void buildIndexAtlas(void) {
    size_t i;

    buildHeader(GDX_LAP_DIGITS_ATLAS_WIDTH, GDX_LAP_DIGITS_ATLAS_HEIGHT);
    for (i = 0; i < ATLAS_PIXELS; i++) {
        sEntry[GDX_LAP_DIGITS_ENTRY_PIXELS + (i * 2)] = (unsigned char) (i >> 8);
        sEntry[GDX_LAP_DIGITS_ENTRY_PIXELS + (i * 2) + 1] = (unsigned char) (i & 0xFFu);
    }
}

/* --- harness -------------------------------------------------------------------------------- */

static int sFailures;

#define CHECK(condition)                                                \
    do {                                                                \
        if (!(condition)) {                                             \
            printf("FAILED line %d: %s\n", __LINE__, #condition);       \
            sFailures++;                                                \
        }                                                               \
    } while (0)

static int isAllZero(const unsigned char* bytes, size_t size) {
    size_t i;

    for (i = 0; i < size; i++) {
        if (bytes[i] != 0) {
            return 0;
        }
    }
    return 1;
}

/* --- tests ---------------------------------------------------------------------------------- */

static void testEveryPixelComesFromTheAtlas(void) {
    unsigned char out[GDX_LAP_DIGITS_SIZE];
    size_t i;

    buildIndexAtlas();
    memset(out, 0xFF, sizeof(out));
    CHECK(gdx_lap_digits_compose(sEntry, (unsigned long) sStubSize, out) == 1);
    for (i = 0; i < GDX_LAP_DIGITS_SIZE; i += 2) {
        CHECK((((size_t) out[i] << 8) | out[i + 1]) < ATLAS_PIXELS);
    }
}

static void testPixelBytesKeepTheirOrder(void) {
    unsigned char out[GDX_LAP_DIGITS_SIZE];
    size_t i;

    buildHeader(GDX_LAP_DIGITS_ATLAS_WIDTH, GDX_LAP_DIGITS_ATLAS_HEIGHT);
    for (i = 0; i < ATLAS_PIXELS; i++) {
        sEntry[GDX_LAP_DIGITS_ENTRY_PIXELS + (i * 2)] = 0xAB;
        sEntry[GDX_LAP_DIGITS_ENTRY_PIXELS + (i * 2) + 1] = 0xCD;
    }
    CHECK(gdx_lap_digits_compose(sEntry, (unsigned long) sStubSize, out) == 1);
    for (i = 0; i < GDX_LAP_DIGITS_SIZE; i += 2) {
        CHECK((out[i] == 0xAB) && (out[i + 1] == 0xCD));
    }
}

static void testWrongEntriesAreRejected(void) {
    unsigned char out[GDX_LAP_DIGITS_SIZE];

    memset(out, 0, sizeof(out));
    buildIndexAtlas();
    CHECK(gdx_lap_digits_compose(NULL, GDX_LAP_DIGITS_ENTRY_SIZE, out) == 0);
    CHECK(gdx_lap_digits_compose(sEntry, GDX_LAP_DIGITS_ENTRY_SIZE - 1, out) == 0);
    buildHeader(16, GDX_LAP_DIGITS_ATLAS_HEIGHT);
    CHECK(gdx_lap_digits_compose(sEntry, GDX_LAP_DIGITS_ENTRY_SIZE, out) == 0);
    buildHeader(GDX_LAP_DIGITS_ATLAS_WIDTH, 60);
    CHECK(gdx_lap_digits_compose(sEntry, GDX_LAP_DIGITS_ENTRY_SIZE, out) == 0);
    CHECK(isAllZero(out, sizeof(out)));
}

/* Runs in this order: apply latches, so the failures have to come before the success. */
static void testApply(void) {
    int readsAfterSuccess;

    sStubPresent = 0;
    gdx_lap_digits_apply();
    CHECK(isAllZero(aLapCounterExtraDigitsTex, GDX_LAP_DIGITS_SIZE));

    buildIndexAtlas();
    sStubPresent = 1;
    sStubSize = GDX_LAP_DIGITS_ENTRY_SIZE - 1;
    gdx_lap_digits_apply();
    CHECK(isAllZero(aLapCounterExtraDigitsTex, GDX_LAP_DIGITS_SIZE));

    /* A failed read is retried, and the first good read fills the texture. */
    sStubSize = GDX_LAP_DIGITS_ENTRY_SIZE;
    gdx_lap_digits_apply();
    CHECK(!isAllZero(aLapCounterExtraDigitsTex, GDX_LAP_DIGITS_SIZE));

    readsAfterSuccess = sStubReads;
    gdx_lap_digits_apply();
    CHECK(sStubReads == readsAfterSuccess);
}

int main(void) {
    testEveryPixelComesFromTheAtlas();
    testPixelBytesKeepTheirOrder();
    testWrongEntriesAreRejected();
    testApply();

    if (sFailures != 0) {
        printf("gdx_lap_digits_tests: %d failure(s)\n", sFailures);
        return 1;
    }
    printf("gdx_lap_digits_tests: all passed\n");
    return 0;
}
