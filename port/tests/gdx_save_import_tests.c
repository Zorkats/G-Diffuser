// Unit tests for port/gdx_save_import.c, compiled together with that file and nothing else.
//
// Every image here is synthetic. Field values are a fixed function of their offset, and the filler
// is a fixed pattern, so no save file, ROM, or game data is involved.
#include "../gdx_save_import.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

// Layout restated from decomp/include/fzx_save.h. It is kept apart from the module's table on
// purpose: the round-trip tests pass only when both descriptions agree on every scalar.
#define IMAGE_SIZE 0x8000UL
#define SRM_SIZE 0x48800UL
#define SRM_BLOCK_OFFSET 0x20800UL
#define PROFILE_STRIDE 0x19E0UL
#define DEATH_RACE_BASE 0x10UL
#define COURSE_BASE 0x20UL
#define COURSE_STRIDE 0x110UL
#define UNION_BASE 0x19A0UL
#define GHOST_BASE 0x33C0UL
#define GHOST_DATA_BASE 0x3400UL
#define CHARACTER_BASE 0x7380UL
#define CHARACTER_STRIDE 0x80UL
#define CUP_BASE 0x7F80UL

#define RUN_OP(offset, count, size) \
    { GDX_SAVE_IMPORT_OP_RUN, (offset), (count), (size), 0, NULL, 0 }
#define REPEAT_OP(offset, count, stride, sub) \
    { GDX_SAVE_IMPORT_OP_REPEAT, (offset), (count), 0, (stride), (sub), \
      (unsigned long) (sizeof(sub) / sizeof((sub)[0])) }

#define CHECK(condition)                                                \
    do {                                                                \
        if (!(condition)) {                                             \
            printf("FAILED line %d: %s\n", __LINE__, #condition);       \
            sFailures++;                                                \
        }                                                               \
    } while (0)

static int sFailures;

#define UNION_SIZE 0x40UL

static unsigned char sHost[IMAGE_SIZE];
static unsigned char sHostClearedCups[IMAGE_SIZE];
static unsigned char sBigEndian[IMAGE_SIZE];
static unsigned char sWordSwapped[IMAGE_SIZE];
static unsigned char sSrmBigEndian[SRM_SIZE];
static unsigned char sSrmWordSwapped[SRM_SIZE];
static unsigned char sOut[IMAGE_SIZE];
static unsigned char sWork[IMAGE_SIZE];
static unsigned char sAlias[IMAGE_SIZE];
static unsigned char sRandom[IMAGE_SIZE];
static unsigned char sRandomSrm[SRM_SIZE];
static unsigned char sZero[SRM_SIZE];

// Values never have equal bytes, so a missed or doubled swap cannot hide behind a palindrome.
// Each value depends only on its offset, which lets the typed checks recompute what they expect.
static uint16_t sample16(unsigned long offset) {
    return (uint16_t) ((((unsigned int) offset & 0xFFu) * 0x0101u) ^ 0x5A3Cu);
}

static uint32_t sample32(unsigned long offset) {
    return ((uint32_t) sample16(offset) << 16) | sample16(offset + 2);
}

// Consecutive filler bytes differ by 31 modulo 256, so no two neighbours match.
static unsigned char filler(unsigned long offset) {
    return (unsigned char) ((offset * 31u) + 0x5Au);
}

// Host-order and big-endian writers share every call site. Only the byte order differs, so the two
// images differ exactly at scalar fields and nowhere else.
static void putU16(unsigned char* image, unsigned long offset, uint16_t value, int bigEndian) {
    if (bigEndian) {
        image[offset] = (unsigned char) (value >> 8);
        image[offset + 1] = (unsigned char) (value & 0xFFu);
    } else {
        memcpy(image + offset, &value, sizeof(value));
    }
}

static void putU32(unsigned char* image, unsigned long offset, uint32_t value, int bigEndian) {
    if (bigEndian) {
        image[offset] = (unsigned char) (value >> 24);
        image[offset + 1] = (unsigned char) (value >> 16);
        image[offset + 2] = (unsigned char) (value >> 8);
        image[offset + 3] = (unsigned char) (value & 0xFFu);
    } else {
        memcpy(image + offset, &value, sizeof(value));
    }
}

static void scalar16(unsigned char* image, unsigned long offset, int bigEndian) {
    putU16(image, offset, sample16(offset), bigEndian);
}

static void scalar32(unsigned char* image, unsigned long offset, int bigEndian) {
    putU32(image, offset, sample32(offset), bigEndian);
}

static uint16_t getHostU16(const unsigned char* image, unsigned long offset) {
    uint16_t value;

    memcpy(&value, image + offset, sizeof(value));
    return value;
}

static uint32_t getHostU32(const unsigned char* image, unsigned long offset) {
    uint32_t value;

    memcpy(&value, image + offset, sizeof(value));
    return value;
}

// The stored checksum is the game's u16 byte sum over [sumFrom, sumTo) of one instance. Byte order
// does not change a sum of bytes, so the same value is sealed in both byte orders.
static void sealChecksum(unsigned char* image, unsigned long base, unsigned long sumFrom,
                         unsigned long sumTo, unsigned long checksumAt, int bigEndian) {
    unsigned long i;
    unsigned long sum = 0;

    for (i = base + sumFrom; i < base + sumTo; i++) {
        sum += image[i];
    }
    putU16(image, base + checksumAt, (uint16_t) (sum & 0xFFFFu), bigEndian);
}

static const unsigned char kSettingsName[8] = { 'F', '-', 'Z', 'E', 'R', 'O', ' ', 'X' };

// One builder for both byte orders. Every scalar is written at its real offset, then every
// checksum is sealed over the finished bytes.
static void buildSave(unsigned char* image, int bigEndian) {
    unsigned long i;
    unsigned long p;
    unsigned long c;
    unsigned long k;

    for (i = 0; i < IMAGE_SIZE; i++) {
        image[i] = filler(i);
    }

    for (p = 0; p < 2; p++) {
        unsigned long profile = p * PROFILE_STRIDE;

        memcpy(image + profile, kSettingsName, sizeof(kSettingsName));
        scalar16(image, profile + DEATH_RACE_BASE + 2, bigEndian);
        scalar32(image, profile + DEATH_RACE_BASE + 4, bigEndian);
        for (c = 0; c < 24; c++) {
            unsigned long course = profile + COURSE_BASE + c * COURSE_STRIDE;

            scalar16(image, course + 2, bigEndian);
            for (k = 0; k < 12; k++) {
                scalar32(image, course + 4 + k * 4, bigEndian);
            }
        }
        scalar16(image, profile + UNION_BASE + 2, bigEndian);
    }

    scalar16(image, GHOST_BASE + 2, bigEndian);
    scalar32(image, GHOST_BASE + 4, bigEndian);
    scalar32(image, GHOST_BASE + 8, bigEndian);
    scalar32(image, GHOST_BASE + 12, bigEndian);
    scalar16(image, GHOST_BASE + 16, bigEndian);

    scalar16(image, GHOST_DATA_BASE + 2, bigEndian);
    for (k = 0; k < 7; k++) {
        scalar32(image, GHOST_DATA_BASE + 4 + k * 4, bigEndian);
    }

    for (c = 0; c < 24; c++) {
        unsigned long character = CHARACTER_BASE + c * CHARACTER_STRIDE;

        scalar16(image, character + 2, bigEndian);
        for (k = 0; k < 31; k++) {
            scalar32(image, character + 4 + k * 4, bigEndian);
        }
    }

    for (k = 0; k < 40; k++) {
        scalar16(image, CUP_BASE + 16 + k * 2, bigEndian);
    }

    for (p = 0; p < 2; p++) {
        unsigned long profile = p * PROFILE_STRIDE;

        sealChecksum(image, profile, 0, 0x0E, 0x0E, bigEndian);
        sealChecksum(image, profile + DEATH_RACE_BASE, 2, 0x10, 0, bigEndian);
        for (c = 0; c < 24; c++) {
            sealChecksum(image, profile + COURSE_BASE + c * COURSE_STRIDE, 2, COURSE_STRIDE, 0,
                         bigEndian);
        }
        sealChecksum(image, profile + UNION_BASE, 2, 0x40, 0, bigEndian);
    }
    sealChecksum(image, GHOST_BASE, 2, 0x40, 0, bigEndian);
    sealChecksum(image, GHOST_DATA_BASE, 2, 0x3F80, 0, bigEndian);
    for (c = 0; c < 24; c++) {
        sealChecksum(image, CHARACTER_BASE + c * CHARACTER_STRIDE, 2, CHARACTER_STRIDE, 0,
                     bigEndian);
    }
    sealChecksum(image, CUP_BASE, 2, 0x60, 0, bigEndian);
}

// The RetroArch layout puts the 0x8000 SRAM block at SRM_BLOCK_OFFSET. The bytes around it are
// filler that the converter must ignore.
static void buildSrm(unsigned char* srm, const unsigned char* block) {
    unsigned long i;

    for (i = 0; i < SRM_SIZE; i++) {
        srm[i] = (unsigned char) (0xC3u ^ (i * 7u));
    }
    memcpy(srm + SRM_BLOCK_OFFSET, block, IMAGE_SIZE);
}

// Test-side word swap. The module's reverseWords must undo exactly this.
static void swapWords(unsigned char* dst, const unsigned char* src, unsigned long size) {
    unsigned long i;

    for (i = 0; i < size; i += 4) {
        dst[i] = src[i + 3];
        dst[i + 1] = src[i + 2];
        dst[i + 2] = src[i + 1];
        dst[i + 3] = src[i];
    }
}

static void fillRandom(unsigned char* buf, unsigned long size, uint32_t seed) {
    unsigned long i;

    for (i = 0; i < size; i++) {
        seed = seed * 1664525u + 1013904223u;
        buf[i] = (unsigned char) (seed >> 24);
    }
}

static int isFilledWith(const unsigned char* buf, unsigned long size, unsigned char value) {
    unsigned long i;

    for (i = 0; i < size; i++) {
        if (buf[i] != value) {
            return 0;
        }
    }
    return 1;
}

// What an emulator import must produce: the host image with both profile unions cleared to empty
// DD cups, because emulator saves carry the cartridge's Edit Cup there.
static void clearUnions(unsigned char* image) {
    unsigned long p;

    for (p = 0; p < 2; p++) {
        memset(image + p * PROFILE_STRIDE + UNION_BASE, 0, UNION_SIZE);
    }
}

static void buildFixtures(void) {
    buildSave(sHost, 0);
    memcpy(sHostClearedCups, sHost, IMAGE_SIZE);
    clearUnions(sHostClearedCups);
    buildSave(sBigEndian, 1);
    swapWords(sWordSwapped, sBigEndian, IMAGE_SIZE);
    buildSrm(sSrmBigEndian, sBigEndian);
    buildSrm(sSrmWordSwapped, sWordSwapped);
}

// Converts and checks the result against the host-order fixture: unchanged for a native image,
// with cleared DD cups for every emulator format.
static void expectConverts(const char* label, const unsigned char* in, unsigned long size,
                           GdxSaveImportFormat format) {
    const unsigned char* expected = (format == GDX_SAVE_IMPORT_FORMAT_NATIVE) ? sHost : sHostClearedCups;
    GdxSaveImportInfo info;
    int result;

    memset(sOut, 0xA5, IMAGE_SIZE);
    memset(&info, 0xEE, sizeof(info));
    result = gdx_save_import_convert(in, size, sOut, &info);
    if (result != 1) {
        printf("FAILED %s: convert returned %d\n", label, result);
        sFailures++;
        return;
    }
    CHECK(info.format == format);
    CHECK(info.validSections == GDX_SAVE_IMPORT_SECTION_ALL);
    CHECK(memcmp(sOut, expected, IMAGE_SIZE) == 0);
}

// A vanilla cartridge save with a built Edit Cup: track names fill the union, and the game's
// Save_SaveEditCup zeroes unk_02, unk_04 and unk_08. The checksum covers the same bytes in both union
// views, so it validates, and the import must still not turn the names into DD cup clears.
static void testCartridgeEditCupBecomesEmptyDDCups(void) {
    GdxSaveImportInfo info;
    unsigned long p;
    unsigned long i;
    int cleared = 1;

    buildSave(sWork, 1);
    for (p = 0; p < 2; p++) {
        unsigned long editCup = p * PROFILE_STRIDE + UNION_BASE;

        memset(sWork + editCup + 2, 0, 8);
        for (i = 0x0A; i < UNION_SIZE; i++) {
            sWork[editCup + i] = (unsigned char) (0x21u + i);
        }
        sealChecksum(sWork, editCup, 2, UNION_SIZE, 0, 1);
    }
    memset(sOut, 0xA5, IMAGE_SIZE);
    CHECK(gdx_save_import_convert(sWork, IMAGE_SIZE, sOut, &info) == 1);
    CHECK(info.format == GDX_SAVE_IMPORT_FORMAT_SRA_BIG_ENDIAN);
    for (p = 0; p < 2; p++) {
        if (!isFilledWith(sOut + p * PROFILE_STRIDE + UNION_BASE, UNION_SIZE, 0)) {
            cleared = 0;
        }
    }
    CHECK(cleared);
    // Everything outside the unions converts exactly as the plain fixture does.
    clearUnions(sOut);
    CHECK(memcmp(sOut, sHostClearedCups, IMAGE_SIZE) == 0);
}

static void testEveryByteOrderConverts(void) {
    expectConverts("native", sHost, IMAGE_SIZE, GDX_SAVE_IMPORT_FORMAT_NATIVE);
    expectConverts("big-endian sra", sBigEndian, IMAGE_SIZE, GDX_SAVE_IMPORT_FORMAT_SRA_BIG_ENDIAN);
    expectConverts("word-swapped sra", sWordSwapped, IMAGE_SIZE,
                   GDX_SAVE_IMPORT_FORMAT_SRA_WORD_SWAPPED);
    expectConverts("srm big-endian", sSrmBigEndian, SRM_SIZE, GDX_SAVE_IMPORT_FORMAT_SRM_PARALLEL);
    expectConverts("srm word-swapped", sSrmWordSwapped, SRM_SIZE,
                   GDX_SAVE_IMPORT_FORMAT_SRM_PARALLEL);
}

static void testAliasedInputAndOutput(void) {
    GdxSaveImportInfo info;

    memcpy(sAlias, sBigEndian, IMAGE_SIZE);
    CHECK(gdx_save_import_convert(sAlias, IMAGE_SIZE, sAlias, &info) == 1);
    CHECK(memcmp(sAlias, sHostClearedCups, IMAGE_SIZE) == 0);
}

static void testNullInfoIsAccepted(void) {
    memset(sOut, 0, IMAGE_SIZE);
    CHECK(gdx_save_import_convert(sBigEndian, IMAGE_SIZE, sOut, NULL) == 1);
    CHECK(memcmp(sOut, sHostClearedCups, IMAGE_SIZE) == 0);
}

// After conversion, host-order readers must see the value written at each offset.
static void testTypedFieldsAreHostOrder(void) {
    GdxSaveImportInfo info;
    unsigned long course = PROFILE_STRIDE + COURSE_BASE + 5 * COURSE_STRIDE;

    memset(sOut, 0, IMAGE_SIZE);
    CHECK(gdx_save_import_convert(sBigEndian, IMAGE_SIZE, sOut, &info) == 1);
    CHECK(getHostU16(sOut, course + 2) == sample16(course + 2));
    CHECK(getHostU32(sOut, course + 4) == sample32(course + 4));
    CHECK(getHostU32(sOut, course + 0x2C) == sample32(course + 0x2C));
    CHECK(getHostU32(sOut, GHOST_BASE + 0x0C) == sample32(GHOST_BASE + 0x0C));
    CHECK(getHostU32(sOut, CHARACTER_BASE + 0x08) == sample32(CHARACTER_BASE + 0x08));
    CHECK(getHostU16(sOut, CUP_BASE + 0x10) == sample16(CUP_BASE + 0x10));
}

// The two byte orders must genuinely differ, or the round-trip tests prove nothing.
static void testFixturesDiffer(void) {
    CHECK(memcmp(sBigEndian, sHost, IMAGE_SIZE) != 0);
    CHECK(memcmp(sWordSwapped, sBigEndian, IMAGE_SIZE) != 0);
    CHECK(memcmp(sSrmBigEndian, sSrmWordSwapped, SRM_SIZE) != 0);
}

static void testWrongSizesRejected(void) {
    static const unsigned long sizes[] = { 0UL, 0x7FFFUL, 0x8001UL, 0x48000UL, 0x48801UL, 0x28800UL };
    GdxSaveImportInfo info;
    size_t i;

    for (i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        memset(sOut, 0xA5, IMAGE_SIZE);
        memset(&info, 0xEE, sizeof(info));
        CHECK(gdx_save_import_convert(sHost, sizes[i], sOut, &info) == 0);
        CHECK(info.format == GDX_SAVE_IMPORT_FORMAT_NONE);
        CHECK(info.validSections == 0);
        CHECK(isFilledWith(sOut, IMAGE_SIZE, 0xA5));
    }
}

static void testNullPointersRejected(void) {
    GdxSaveImportInfo info;

    memset(sOut, 0xA5, IMAGE_SIZE);
    CHECK(gdx_save_import_convert(NULL, IMAGE_SIZE, sOut, &info) == 0);
    CHECK(gdx_save_import_convert(sBigEndian, IMAGE_SIZE, NULL, &info) == 0);
    CHECK(isFilledWith(sOut, IMAGE_SIZE, 0xA5));
}

// Flipping one bit of a course checksum fails exactly the COURSES section (bit 0x04).
static void testCorruptedChecksumRejected(void) {
    GdxSaveImportInfo info;
    unsigned long checksumAt = PROFILE_STRIDE + COURSE_BASE + 5 * COURSE_STRIDE;

    memcpy(sWork, sHost, IMAGE_SIZE);
    sWork[checksumAt] = (unsigned char) (sWork[checksumAt] ^ 0x01u);
    memset(sOut, 0xA5, IMAGE_SIZE);
    CHECK(gdx_save_import_convert(sWork, IMAGE_SIZE, sOut, &info) == 0);
    CHECK(info.format == GDX_SAVE_IMPORT_FORMAT_NONE);
    CHECK(info.validSections == 0xFBu);
    CHECK(isFilledWith(sOut, IMAGE_SIZE, 0xA5));
}

// A flipped byte inside cupCompletion breaks only the CUP section (bit 0x80).
static void testCorruptedDataRejected(void) {
    GdxSaveImportInfo info;

    memcpy(sWork, sHost, IMAGE_SIZE);
    sWork[CUP_BASE + 0x20] = (unsigned char) (sWork[CUP_BASE + 0x20] ^ 0x40u);
    memset(sOut, 0xA5, IMAGE_SIZE);
    CHECK(gdx_save_import_convert(sWork, IMAGE_SIZE, sOut, &info) == 0);
    CHECK(info.validSections == 0x7Fu);
    CHECK(isFilledWith(sOut, IMAGE_SIZE, 0xA5));
}

// Only the name is wrong here, and the checksum is resealed, so the SETTINGS bit (0x01) is the one
// that fails.
static void testNameMismatchRejected(void) {
    GdxSaveImportInfo info;

    memcpy(sWork, sHost, IMAGE_SIZE);
    sWork[PROFILE_STRIDE + 7] = 'Y';
    sealChecksum(sWork, PROFILE_STRIDE, 0, 0x0E, 0x0E, 0);
    memset(sOut, 0xA5, IMAGE_SIZE);
    CHECK(gdx_save_import_convert(sWork, IMAGE_SIZE, sOut, &info) == 0);
    CHECK(info.validSections == 0xFEu);
    CHECK(isFilledWith(sOut, IMAGE_SIZE, 0xA5));
}

static void testRandomBytesRejected(void) {
    GdxSaveImportInfo info;

    fillRandom(sRandom, IMAGE_SIZE, 1u);
    memset(sOut, 0xA5, IMAGE_SIZE);
    CHECK(gdx_save_import_convert(sRandom, IMAGE_SIZE, sOut, &info) == 0);
    CHECK(info.format == GDX_SAVE_IMPORT_FORMAT_NONE);
    CHECK(isFilledWith(sOut, IMAGE_SIZE, 0xA5));

    fillRandom(sRandomSrm, SRM_SIZE, 2u);
    memset(sOut, 0xA5, IMAGE_SIZE);
    CHECK(gdx_save_import_convert(sRandomSrm, SRM_SIZE, sOut, &info) == 0);
    CHECK(info.format == GDX_SAVE_IMPORT_FORMAT_NONE);
    CHECK(isFilledWith(sOut, IMAGE_SIZE, 0xA5));
}

// Blank SRAM passes every checksum (0 == 0). Only the name check rejects it, so the SETTINGS bit
// (0x01) is the one that is cleared and every other bit is set (0xFE).
static void testAllZeroRejected(void) {
    GdxSaveImportInfo info;

    memset(sOut, 0xA5, IMAGE_SIZE);
    CHECK(gdx_save_import_convert(sZero, IMAGE_SIZE, sOut, &info) == 0);
    CHECK(info.validSections == 0xFEu);
    CHECK(isFilledWith(sOut, IMAGE_SIZE, 0xA5));

    memset(sOut, 0xA5, IMAGE_SIZE);
    CHECK(gdx_save_import_convert(sZero, SRM_SIZE, sOut, &info) == 0);
    CHECK(info.format == GDX_SAVE_IMPORT_FORMAT_NONE);
    CHECK(info.validSections == 0xFEu);
    CHECK(isFilledWith(sOut, IMAGE_SIZE, 0xA5));
}

static void testBuiltInLayoutIsConsistent(void) {
    CHECK(gdx_save_import_layout_is_consistent() == 1);
}

static void testCheckerAcceptsValidTables(void) {
    static const GdxSaveImportOp flat[] = { RUN_OP(0, 4, 1), RUN_OP(4, 2, 2) };
    static const GdxSaveImportOp repeatRun[] = { RUN_OP(0, 1, 4) };
    static const GdxSaveImportOp repeated[] = { REPEAT_OP(0, 2, 4, repeatRun) };

    CHECK(gdx_save_import_check_layout(flat, 2, 8) == 1);
    CHECK(gdx_save_import_check_layout(repeated, 1, 8) == 1);
}

static void testCheckerRejectsBadTables(void) {
    static const GdxSaveImportOp gap[] = { RUN_OP(0, 2, 1), RUN_OP(3, 1, 1) };
    static const GdxSaveImportOp overlap[] = { RUN_OP(0, 4, 1), RUN_OP(2, 2, 1) };
    static const GdxSaveImportOp shortCover[] = { RUN_OP(0, 4, 1) };
    static const GdxSaveImportOp overrun[] = { RUN_OP(0, 8, 1) };
    static const GdxSaveImportOp badSize[] = { RUN_OP(0, 4, 3) };
    static const GdxSaveImportOp zeroCount[] = { RUN_OP(0, 0, 1) };
    static const GdxSaveImportOp fourBytes[] = { RUN_OP(0, 4, 1) };
    static const GdxSaveImportOp strideLong[] = { REPEAT_OP(0, 2, 5, fourBytes) };
    static const GdxSaveImportOp strideShort[] = { REPEAT_OP(0, 2, 3, fourBytes) };

    CHECK(gdx_save_import_check_layout(gap, 2, 4) == 0);
    CHECK(gdx_save_import_check_layout(overlap, 2, 6) == 0);
    CHECK(gdx_save_import_check_layout(shortCover, 1, 6) == 0);
    CHECK(gdx_save_import_check_layout(overrun, 1, 6) == 0);
    CHECK(gdx_save_import_check_layout(badSize, 1, 12) == 0);
    CHECK(gdx_save_import_check_layout(zeroCount, 1, 0) == 0);
    CHECK(gdx_save_import_check_layout(strideLong, 1, 10) == 0);
    CHECK(gdx_save_import_check_layout(strideShort, 1, 6) == 0);
    CHECK(gdx_save_import_check_layout(NULL, 1, 8) == 0);
    CHECK(gdx_save_import_check_layout(fourBytes, 0, 4) == 0);
}

int main(void) {
    buildFixtures();
    testFixturesDiffer();

    testEveryByteOrderConverts();
    testCartridgeEditCupBecomesEmptyDDCups();
    testAliasedInputAndOutput();
    testNullInfoIsAccepted();
    testTypedFieldsAreHostOrder();
    testWrongSizesRejected();
    testNullPointersRejected();
    testCorruptedChecksumRejected();
    testCorruptedDataRejected();
    testNameMismatchRejected();
    testRandomBytesRejected();
    testAllZeroRejected();
    testBuiltInLayoutIsConsistent();
    testCheckerAcceptsValidTables();
    testCheckerRejectsBadTables();

    if (sFailures != 0) {
        printf("gdx_save_import_tests: %d failure(s)\n", sFailures);
        return 1;
    }
    printf("gdx_save_import_tests: all passed\n");
    return 0;
}
