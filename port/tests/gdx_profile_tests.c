// port/tests/gdx_profile_tests.c: standalone checks for the profile codec, the store and the icon helpers.
// Pure C. The only project header beyond gdx_profile.h is gdx_course_meta.h: its author limit and its real
// gdx_meta_name_ok check, so the profile name rule is proven against the validator tracks actually use.
// Exit code 0 when every check passes, 1 otherwise.

#include "gdx_course_meta.h"
#include "gdx_profile.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef char profile_name_fits_author_field[(GDX_PROFILE_NAME_MAX <= GDX_META_NAME_MAX) ? 1 : -1];
typedef char profile_block_max_pinned[(GDX_PROFILE_BLOCK_MAX_BYTES == 16453) ? 1 : -1];
typedef char profile_store_max_pinned[(GDX_PROFILE_STORE_BLOCK_MAX_BYTES == 131664) ? 1 : -1];

#define CHECK(cond, text)                 \
    do {                                  \
        if (cond) {                       \
            printf("PASS: %s\n", (text)); \
        } else {                          \
            printf("FAIL: %s\n", (text)); \
            sFailures++;                  \
        }                                 \
    } while (0)

#define CUSTOM_PIXEL_BYTES 16384u

static int sFailures = 0;
static uint32_t sRng = 1u;
static uint32_t sCrcTable[256];

// Large objects live in static storage: a GdxProfileStore is about 131 KB.
static GdxProfile sProfileA;
static GdxProfile sProfileB;
static GdxProfile sProfileScratch;
static GdxProfileStore sStore;
static GdxProfileStore sStoreCopy;
static unsigned char sBlock[GDX_PROFILE_BLOCK_MAX_BYTES];
static unsigned char sBlockAgain[GDX_PROFILE_BLOCK_MAX_BYTES];
static unsigned char sStoreBlock[GDX_PROFILE_STORE_BLOCK_MAX_BYTES];
static unsigned char sStoreAgain[GDX_PROFILE_STORE_BLOCK_MAX_BYTES];

typedef struct TestRecord {
    int tag;
    size_t length;
    const unsigned char* value;
} TestRecord;

static uint32_t rng_next(void) {
    sRng ^= sRng << 13;
    sRng ^= sRng >> 17;
    sRng ^= sRng << 5;
    return sRng;
}

static void rng_seed(uint32_t seed) {
    sRng = seed ? seed : 1u;
}

static void rng_fill(unsigned char* p, size_t length) {
    size_t i;

    for (i = 0; i < length; i++) {
        p[i] = (unsigned char)(rng_next() >> 24);
    }
}

static void crc_table_init(void) {
    uint32_t n;
    int k;

    for (n = 0; n < 256; n++) {
        uint32_t c = n;

        for (k = 0; k < 8; k++) {
            c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
        }
        sCrcTable[n] = c;
    }
}

// Table-driven CRC-32, independent of the module's bitwise copy.
static uint32_t crc_ref(const unsigned char* data, size_t length) {
    uint32_t crc = 0xFFFFFFFFu;
    size_t i;

    for (i = 0; i < length; i++) {
        crc = sCrcTable[(crc ^ data[i]) & 0xFFu] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
}

static uint32_t get32(const unsigned char* p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void put16(unsigned char* p, uint32_t value) {
    p[0] = (unsigned char)(value & 0xFFu);
    p[1] = (unsigned char)((value >> 8) & 0xFFu);
}

static void put32(unsigned char* p, uint32_t value) {
    p[0] = (unsigned char)(value & 0xFFu);
    p[1] = (unsigned char)((value >> 8) & 0xFFu);
    p[2] = (unsigned char)((value >> 16) & 0xFFu);
    p[3] = (unsigned char)((value >> 24) & 0xFFu);
}

// Recomputes the trailing CRC of a block whose last four bytes are the CRC.
static void reseal(unsigned char* block, size_t length) {
    put32(block + length - 4, crc_ref(block, length - 4));
}

static int is_zero(const void* p, size_t length) {
    const unsigned char* b = (const unsigned char*)p;
    size_t i;

    for (i = 0; i < length; i++) {
        if (b[i] != 0) {
            return 0;
        }
    }
    return 1;
}

// Builds a container with the given magic and record-count field, then seals it. Returns the block length.
static size_t build_container(unsigned char* out, const char* magic, uint32_t countField, const TestRecord* records, size_t n) {
    size_t pos = 8;
    size_t i;

    memcpy(out, magic, 4);
    put16(out + 4, 1u);
    put16(out + 6, countField);
    for (i = 0; i < n; i++) {
        out[pos] = (unsigned char)records[i].tag;
        put16(out + pos + 1, (uint32_t)records[i].length);
        if (records[i].length > 0) {
            memcpy(out + pos + 3, records[i].value, records[i].length);
        }
        pos += 3 + records[i].length;
    }
    put32(out + pos, crc_ref(out, pos));
    return pos + 4;
}

// Spelled out from the layout in gdx_profile.h. Name "Zor", id 01..10, colour 0x00FF8040, machine 7, stock pilot 3.
static const unsigned char kStockHead[49] = {
    'G', 'P', 'F', '1', 0x01, 0x00, 0x05, 0x00,
    0x01, 0x03, 0x00, 'Z', 'o', 'r',
    0x02, 0x10, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10,
    0x03, 0x04, 0x00, 0x40, 0x80, 0xFF, 0x00,
    0x04, 0x01, 0x00, 0x07,
    0x05, 0x02, 0x00, 0x00, 0x03,
};

static const unsigned char kNameZor[3] = {'Z', 'o', 'r'};
static const unsigned char kIdBytes[16] = {0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08,
                                           0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x10};
static const unsigned char kColourStock[4] = {0x40, 0x80, 0xFF, 0x00};
static const unsigned char kMachineSeven[1] = {0x07};
static const unsigned char kIconStockThree[2] = {0x00, 0x03};
static const unsigned char kIconCustomZero[2] = {0x01, 0x00};
static const unsigned char kIconInitialsZero[2] = {0x02, 0x00};
static const unsigned char kName17[17] = {'0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'a', 'b', 'c', 'd', 'e', 'f', 'g'};

static const TestRecord kStockRecords[5] = {
    {GDX_PROFILE_TAG_NAME, 3, kNameZor},
    {GDX_PROFILE_TAG_ID, 16, kIdBytes},
    {GDX_PROFILE_TAG_COLOUR, 4, kColourStock},
    {GDX_PROFILE_TAG_MACHINE, 1, kMachineSeven},
    {GDX_PROFILE_TAG_ICON, 2, kIconStockThree},
};

static void make_stock_profile(GdxProfile* p) {
    memset(p, 0, sizeof(*p));
    memcpy(p->id, kIdBytes, GDX_PROFILE_ID_BYTES);
    memcpy(p->name, "Zor", sizeof("Zor"));
    p->colour = 0x00FF8040u;
    p->preferredMachine = 7;
    p->iconKind = GDX_PROFILE_ICON_STOCK;
    p->iconIndex = 3;
}

// Random legal profile of the given icon kind. Edge characters avoid spaces; ids are never all zero.
static void make_random_profile(GdxProfile* p, int kind) {
    size_t length = 1u + rng_next() % 16u;
    size_t i;

    memset(p, 0, sizeof(*p));
    rng_fill(p->id, GDX_PROFILE_ID_BYTES);
    p->id[15] |= 0x01u;
    for (i = 0; i < length; i++) {
        int edge = (i == 0) || (i + 1 == length);

        p->name[i] = (char)(edge ? (0x21u + rng_next() % 94u) : (0x20u + rng_next() % 95u));
    }
    p->name[length] = '\0';
    p->colour = rng_next() & 0xFFFFFFu;
    p->preferredMachine = (int)(rng_next() % 31u) - 1;
    p->iconKind = kind;
    p->iconIndex = (kind == GDX_PROFILE_ICON_STOCK) ? (int)(rng_next() % GDX_PROFILE_ROSTER_COUNT) : 0;
    if (kind == GDX_PROFILE_ICON_CUSTOM) {
        rng_fill(p->icon, GDX_PROFILE_ICON_BYTES);
    }
}

static int same_profile(const GdxProfile* a, const GdxProfile* b) {
    if ((strcmp(a->name, b->name) != 0) || (memcmp(a->id, b->id, GDX_PROFILE_ID_BYTES) != 0) ||
        (a->colour != b->colour) || (a->preferredMachine != b->preferredMachine) ||
        (a->iconKind != b->iconKind) || (a->iconIndex != b->iconIndex)) {
        return 0;
    }
    if (a->iconKind == GDX_PROFILE_ICON_CUSTOM) {
        return memcmp(a->icon, b->icon, GDX_PROFILE_ICON_BYTES) == 0;
    }
    return 1;
}

// Expected GPF1 length, written from the layout rather than from the module's macros.
static size_t expected_block_length(const GdxProfile* p) {
    size_t n = 8 + (3 + strlen(p->name)) + (3 + 16) + (3 + 4) + (3 + 1) + (3 + 2) + 4;

    if (p->iconKind == GDX_PROFILE_ICON_CUSTOM) {
        n += 3 + CUSTOM_PIXEL_BYTES;
    }
    return n;
}

// Reference name rule: 1..max printable ASCII bytes, no space at either end.
static int ref_name_ok(const char* s, size_t max) {
    size_t len = strlen(s);
    size_t i;

    if ((len < 1) || (len > max) || (s[0] == ' ') || (s[len - 1] == ' ')) {
        return 0;
    }
    for (i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];

        if ((c < 0x20) || (c > 0x7E)) {
            return 0;
        }
    }
    return 1;
}

static void test_crc_reference(void) {
    static const unsigned char kCheck[9] = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};

    CHECK(crc_ref(kCheck, sizeof(kCheck)) == 0xCBF43926u, "crc reference: check value of 123456789 is 0xCBF43926");
}

static void test_names(void) {
    CHECK(gdx_profile_name_ok("A") == 1, "name: single letter is legal");
    CHECK(gdx_profile_name_ok("0123456789abcdef") == 1, "name: 16 characters is legal");
    CHECK(gdx_profile_name_ok("0123456789abcdefg") == 0, "name: 17 characters is illegal");
    CHECK(gdx_profile_name_ok("") == 0, "name: empty is illegal");
    CHECK(gdx_profile_name_ok(" A") == 0, "name: leading space is illegal");
    CHECK(gdx_profile_name_ok("A ") == 0, "name: trailing space is illegal");
    CHECK(gdx_profile_name_ok("A B") == 1, "name: inner space is legal");
    CHECK(gdx_profile_name_ok("A\x7F") == 0, "name: DEL is illegal");
    CHECK(gdx_profile_name_ok("A\tB") == 0, "name: tab is illegal");
    CHECK(gdx_profile_name_ok("A\xC3\xA9") == 0, "name: non-ASCII bytes are illegal");
    CHECK(gdx_profile_name_ok("~") == 1, "name: tilde is legal");
    CHECK(gdx_profile_name_ok(NULL) == 0, "name: NULL is illegal");
}

static void test_author_alignment(void) {
    char name[32];
    int cases;
    int mismatches = 0;
    int authorMismatches = 0;

    rng_seed(0x5EED1234u);
    for (cases = 0; cases < 20000; cases++) {
        size_t length = rng_next() % 25u;
        size_t k;

        for (k = 0; k < length; k++) {
            uint32_t c = (rng_next() % 10u < 7u) ? (0x20u + rng_next() % 95u) : (1u + rng_next() % 255u);

            name[k] = (char)c;
        }
        name[length] = '\0';
        if (gdx_profile_name_ok(name) != ref_name_ok(name, GDX_PROFILE_NAME_MAX)) {
            mismatches++;
        }
        if (gdx_profile_name_ok(name) && !gdx_meta_name_ok(name)) {
            authorMismatches++;
        }
    }
    CHECK(mismatches == 0, "name rule: 20000 random strings agree with the reference profile rule");
    CHECK(authorMismatches == 0, "name rule: every legal profile name is a legal track author name");
}

static void test_normalise(void) {
    char out[32];

    CHECK((gdx_profile_name_normalise("  Zor  ", out, sizeof(out)) == GDX_PROFILE_OK) && (strcmp(out, "Zor") == 0),
          "normalise: trims spaces from both ends");
    CHECK((gdx_profile_name_normalise("A B  ", out, sizeof(out)) == GDX_PROFILE_OK) && (strcmp(out, "A B") == 0),
          "normalise: keeps inner spaces");
    CHECK((gdx_profile_name_normalise("   ", out, sizeof(out)) == GDX_PROFILE_ERR_VALUE) && (out[0] == '\0'),
          "normalise: all spaces is VALUE and clears out");
    CHECK(gdx_profile_name_normalise(" 0123456789abcdefg ", out, sizeof(out)) == GDX_PROFILE_ERR_VALUE,
          "normalise: 17 characters after trimming is VALUE");
    CHECK((gdx_profile_name_normalise("Zor", out, 3) == GDX_PROFILE_ERR_VALUE) && (out[0] == '\0'),
          "normalise: cap without room for the NUL is VALUE");
    CHECK(gdx_profile_name_normalise("Zor", out, 4) == GDX_PROFILE_OK, "normalise: cap equal to length plus NUL fits");
    CHECK(gdx_profile_name_normalise("\tZor", out, sizeof(out)) == GDX_PROFILE_ERR_VALUE,
          "normalise: tab is not trimmed and is VALUE");
    CHECK(gdx_profile_name_normalise(NULL, out, sizeof(out)) == GDX_PROFILE_ERR_ARGS, "normalise: NULL raw gives ARGS");
    CHECK(gdx_profile_name_normalise("Zor", out, 0) == GDX_PROFILE_ERR_ARGS, "normalise: cap 0 gives ARGS");
}

static void test_validate(void) {
    GdxProfile p;

    make_stock_profile(&p);
    CHECK(gdx_profile_validate(&p) == GDX_PROFILE_OK, "validate: stock profile is legal");
    memset(p.id, 0, GDX_PROFILE_ID_BYTES);
    CHECK(gdx_profile_validate(&p) == GDX_PROFILE_ERR_VALUE, "validate: all-zero id is VALUE");
    make_stock_profile(&p);
    p.colour = 0x01000000u;
    CHECK(gdx_profile_validate(&p) == GDX_PROFILE_ERR_VALUE, "validate: colour above 0xFFFFFF is VALUE");
    make_stock_profile(&p);
    p.preferredMachine = 30;
    CHECK(gdx_profile_validate(&p) == GDX_PROFILE_ERR_VALUE, "validate: machine 30 is VALUE");
    p.preferredMachine = -2;
    CHECK(gdx_profile_validate(&p) == GDX_PROFILE_ERR_VALUE, "validate: machine -2 is VALUE");
    p.preferredMachine = -1;
    CHECK(gdx_profile_validate(&p) == GDX_PROFILE_OK, "validate: machine -1 (none) is legal");
    make_stock_profile(&p);
    p.iconKind = 3;
    CHECK(gdx_profile_validate(&p) == GDX_PROFILE_ERR_VALUE, "validate: icon kind 3 is VALUE");
    make_stock_profile(&p);
    p.iconIndex = GDX_PROFILE_ROSTER_COUNT;
    CHECK(gdx_profile_validate(&p) == GDX_PROFILE_ERR_VALUE, "validate: stock index 30 is VALUE");
    make_stock_profile(&p);
    p.name[0] = ' ';
    CHECK(gdx_profile_validate(&p) == GDX_PROFILE_ERR_VALUE, "validate: name with a leading space is VALUE");
    make_stock_profile(&p);
    p.iconKind = GDX_PROFILE_ICON_CUSTOM;
    p.iconIndex = 1;
    CHECK(gdx_profile_validate(&p) == GDX_PROFILE_ERR_VALUE, "validate: custom icon with index 1 is VALUE");
    make_stock_profile(&p);
    p.iconKind = GDX_PROFILE_ICON_INITIALS;
    p.iconIndex = 0;
    CHECK(gdx_profile_validate(&p) == GDX_PROFILE_OK, "validate: initials icon with index 0 is legal");
    CHECK(gdx_profile_validate(NULL) == GDX_PROFILE_ERR_ARGS, "validate: NULL gives ARGS");
}

static void test_stock_block(void) {
    unsigned char hand[53];
    int n;

    memcpy(hand, kStockHead, sizeof(kStockHead));
    put32(hand + 49, crc_ref(hand, 49));
    n = (int)build_container(sBlockAgain, "GPF1", 5, kStockRecords, 5);
    CHECK((n == 53) && (memcmp(sBlockAgain, hand, 53) == 0), "stock block: test record builder matches the hand vector");
    CHECK(gdx_profile_decode(hand, sizeof(hand), &sProfileA) == GDX_PROFILE_OK,
          "stock block: hand-written 53-byte block decodes");
    CHECK((strcmp(sProfileA.name, "Zor") == 0) && (sProfileA.colour == 0x00FF8040u) &&
              (sProfileA.preferredMachine == 7) && (sProfileA.iconKind == GDX_PROFILE_ICON_STOCK) &&
              (sProfileA.iconIndex == 3) && (sProfileA.id[0] == 0x01) && (sProfileA.id[15] == 0x10),
          "stock block: decoded fields match the hand vector");
    n = gdx_profile_encode(&sProfileA, sBlock, sizeof(sBlock));
    CHECK(n == 53, "stock block: encodes to 53 bytes");
    CHECK((n == 53) && (memcmp(sBlock, hand, 53) == 0), "stock block: encoder output matches the hand vector byte for byte");
    CHECK(gdx_profile_encode(&sProfileA, sBlock, 52) == GDX_PROFILE_ERR_FULL, "stock block: cap 52 gives FULL");
    CHECK(gdx_profile_encode(NULL, sBlock, sizeof(sBlock)) == GDX_PROFILE_ERR_ARGS, "stock block: NULL profile gives ARGS");
    CHECK(gdx_profile_encode(&sProfileA, NULL, 100) == GDX_PROFILE_ERR_ARGS, "stock block: NULL output gives ARGS");
    make_stock_profile(&sProfileScratch);
    sProfileScratch.name[0] = ' ';
    CHECK(gdx_profile_encode(&sProfileScratch, sBlock, sizeof(sBlock)) == GDX_PROFILE_ERR_VALUE,
          "stock block: encoding an invalid profile gives VALUE");
}

static void test_round_trips(void) {
    static const int kinds[3] = {GDX_PROFILE_ICON_STOCK, GDX_PROFILE_ICON_CUSTOM, GDX_PROFILE_ICON_INITIALS};
    static const char* const kTexts[3] = {
        "round trip: stock icons decode, re-encode byte-identical and carry a correct CRC",
        "round trip: custom icons decode, re-encode byte-identical and carry a correct CRC",
        "round trip: initials icons decode, re-encode byte-identical and carry a correct CRC",
    };
    int k;
    uint32_t seed;

    for (k = 0; k < 3; k++) {
        int ok = 1;

        for (seed = 1; seed <= 100; seed++) {
            int n;
            int n2;

            rng_seed(seed * 2654435761u + (uint32_t)k);
            make_random_profile(&sProfileA, kinds[k]);
            n = gdx_profile_encode(&sProfileA, sBlock, sizeof(sBlock));
            if (n < 0) {
                ok = 0;
                continue;
            }
            if ((size_t)n != expected_block_length(&sProfileA) || crc_ref(sBlock, (size_t)n - 4) != get32(sBlock + n - 4)) {
                ok = 0;
            }
            if (gdx_profile_decode(sBlock, (size_t)n, &sProfileB) != GDX_PROFILE_OK || !same_profile(&sProfileA, &sProfileB)) {
                ok = 0;
            }
            n2 = gdx_profile_encode(&sProfileB, sBlockAgain, sizeof(sBlockAgain));
            if ((n2 != n) || (memcmp(sBlockAgain, sBlock, (size_t)n) != 0)) {
                ok = 0;
            }
        }
        CHECK(ok, kTexts[k]);
    }
}

static unsigned char sPixelBytes[GDX_PROFILE_ICON_BYTES];
static unsigned char sStock[53];
static unsigned char sOversize[GDX_PROFILE_BLOCK_MAX_BYTES + 1];

static void build_stock_block(unsigned char* out) {
    memcpy(out, kStockHead, sizeof(kStockHead));
    put32(out + 49, crc_ref(out, 49));
}

// Decodes block into sProfileA, which starts poisoned. Passes when the code matches and failures leave the output zeroed.
static void expect_decode(const unsigned char* block, size_t length, int expected, const char* text) {
    int rc;

    memset(&sProfileA, 0xA5, sizeof(sProfileA));
    rc = gdx_profile_decode(block, length, &sProfileA);
    CHECK((rc == expected) && ((rc == GDX_PROFILE_OK) || is_zero(&sProfileA, sizeof(sProfileA))), text);
}

// Stock record set with one record replaced. index follows kStockRecords: 0 name, 1 id, 2 colour, 3 machine, 4 icon.
static size_t build_with(unsigned char* out, size_t index, TestRecord replacement) {
    TestRecord r[5];

    memcpy(r, kStockRecords, sizeof(r));
    r[index] = replacement;
    return build_container(out, "GPF1", 5, r, 5);
}

static void test_corrupt_profiles(void) {
    static const unsigned char kNameLead[4] = {' ', 'Z', 'o', 'r'};
    static const unsigned char kNameTrail[4] = {'Z', 'o', 'r', ' '};
    static const unsigned char kNameDel[3] = {'Z', 'o', 0x7F};
    static const unsigned char kZeroId[16] = {0};
    static const unsigned char kColour3[3] = {0x40, 0x80, 0xFF};
    static const unsigned char kColourTooBig[4] = {0x00, 0x00, 0x00, 0x01};
    static const unsigned char kMachine2[2] = {0x07, 0x00};
    static const unsigned char kMachine30[1] = {0x1E};
    static const unsigned char kMachineFE[1] = {0xFE};
    static const unsigned char kIconThree[3] = {0x00, 0x03, 0x00};
    static const unsigned char kIconKind3[2] = {0x03, 0x00};
    static const unsigned char kIconStock30[2] = {0x00, 0x1E};
    TestRecord r[6];
    size_t n;

    rng_fill(sPixelBytes, sizeof(sPixelBytes));
    build_stock_block(sStock);
    memset(sOversize, 'G', sizeof(sOversize));

    n = build_with(sBlockAgain, 0, (TestRecord){1, 3, kNameZor});
    expect_decode(sBlockAgain, n, GDX_PROFILE_OK, "corrupt: the stock variant built by the record builder decodes");

    memcpy(sBlockAgain, sStock, 53);
    memcpy(sBlockAgain, "GPX1", 4);
    reseal(sBlockAgain, 53);
    expect_decode(sBlockAgain, 53, GDX_PROFILE_ERR_MAGIC, "corrupt: wrong magic gives MAGIC");

    memcpy(sBlockAgain, sStock, 53);
    put16(sBlockAgain + 4, 2u);
    reseal(sBlockAgain, 53);
    expect_decode(sBlockAgain, 53, GDX_PROFILE_ERR_VERSION, "corrupt: version 2 gives VERSION");

    memcpy(sBlockAgain, sStock, 53);
    sBlockAgain[20] ^= 0x10;
    expect_decode(sBlockAgain, 53, GDX_PROFILE_ERR_CRC, "corrupt: flipped byte without reseal gives CRC");

    expect_decode(sStock, 11, GDX_PROFILE_ERR_TRUNCATED, "corrupt: 11 bytes gives TRUNCATED");
    expect_decode(sStock, 0, GDX_PROFILE_ERR_TRUNCATED, "corrupt: 0 bytes gives TRUNCATED");

    n = build_container(sBlockAgain, "GPF1", 5, NULL, 0);
    expect_decode(sBlockAgain, n, GDX_PROFILE_ERR_LAYOUT, "corrupt: 12 bytes with record count 5 gives LAYOUT");

    n = build_container(sBlockAgain, "GPF1", 4, kStockRecords, 5);
    expect_decode(sBlockAgain, n, GDX_PROFILE_ERR_LAYOUT, "corrupt: record count one short gives LAYOUT");
    n = build_container(sBlockAgain, "GPF1", 6, kStockRecords, 5);
    expect_decode(sBlockAgain, n, GDX_PROFILE_ERR_LAYOUT, "corrupt: record count one long gives LAYOUT");

    {
        TestRecord ordered[5] = {kStockRecords[1], kStockRecords[0], kStockRecords[2], kStockRecords[3], kStockRecords[4]};
        TestRecord duplicate[6] = {kStockRecords[0], kStockRecords[0], kStockRecords[1], kStockRecords[2], kStockRecords[3], kStockRecords[4]};
        TestRecord noColour[4] = {kStockRecords[0], kStockRecords[1], kStockRecords[3], kStockRecords[4]};

        n = build_container(sBlockAgain, "GPF1", 5, ordered, 5);
        expect_decode(sBlockAgain, n, GDX_PROFILE_ERR_LAYOUT, "corrupt: out-of-order tags give LAYOUT");
        n = build_container(sBlockAgain, "GPF1", 6, duplicate, 6);
        expect_decode(sBlockAgain, n, GDX_PROFILE_ERR_LAYOUT, "corrupt: a repeated tag gives LAYOUT");
        n = build_container(sBlockAgain, "GPF1", 4, noColour, 4);
        expect_decode(sBlockAgain, n, GDX_PROFILE_ERR_LAYOUT, "corrupt: missing colour record gives LAYOUT");
    }

    n = build_with(sBlockAgain, 0, (TestRecord){1, 17, kName17});
    expect_decode(sBlockAgain, n, GDX_PROFILE_ERR_VALUE, "corrupt: 17-character name gives VALUE");
    n = build_with(sBlockAgain, 0, (TestRecord){1, 4, kNameLead});
    expect_decode(sBlockAgain, n, GDX_PROFILE_ERR_VALUE, "corrupt: name with a leading space gives VALUE");
    n = build_with(sBlockAgain, 0, (TestRecord){1, 4, kNameTrail});
    expect_decode(sBlockAgain, n, GDX_PROFILE_ERR_VALUE, "corrupt: name with a trailing space gives VALUE");
    n = build_with(sBlockAgain, 0, (TestRecord){1, 3, kNameDel});
    expect_decode(sBlockAgain, n, GDX_PROFILE_ERR_VALUE, "corrupt: name with byte 0x7F gives VALUE");
    n = build_with(sBlockAgain, 0, (TestRecord){1, 0, kNameZor});
    expect_decode(sBlockAgain, n, GDX_PROFILE_ERR_VALUE, "corrupt: empty name gives VALUE");

    n = build_with(sBlockAgain, 1, (TestRecord){2, 15, kIdBytes});
    expect_decode(sBlockAgain, n, GDX_PROFILE_ERR_LAYOUT, "corrupt: 15-byte id gives LAYOUT");
    n = build_with(sBlockAgain, 1, (TestRecord){2, 16, kZeroId});
    expect_decode(sBlockAgain, n, GDX_PROFILE_ERR_VALUE, "corrupt: all-zero id gives VALUE");

    n = build_with(sBlockAgain, 2, (TestRecord){3, 3, kColour3});
    expect_decode(sBlockAgain, n, GDX_PROFILE_ERR_LAYOUT, "corrupt: 3-byte colour gives LAYOUT");
    n = build_with(sBlockAgain, 2, (TestRecord){3, 4, kColourTooBig});
    expect_decode(sBlockAgain, n, GDX_PROFILE_ERR_VALUE, "corrupt: colour 0x01000000 gives VALUE");

    n = build_with(sBlockAgain, 3, (TestRecord){4, 2, kMachine2});
    expect_decode(sBlockAgain, n, GDX_PROFILE_ERR_LAYOUT, "corrupt: 2-byte machine gives LAYOUT");
    n = build_with(sBlockAgain, 3, (TestRecord){4, 1, kMachine30});
    expect_decode(sBlockAgain, n, GDX_PROFILE_ERR_VALUE, "corrupt: machine 30 gives VALUE");
    n = build_with(sBlockAgain, 3, (TestRecord){4, 1, kMachineFE});
    expect_decode(sBlockAgain, n, GDX_PROFILE_ERR_VALUE, "corrupt: machine 0xFE gives VALUE");

    n = build_with(sBlockAgain, 4, (TestRecord){5, 3, kIconThree});
    expect_decode(sBlockAgain, n, GDX_PROFILE_ERR_LAYOUT, "corrupt: 3-byte icon record gives LAYOUT");
    n = build_with(sBlockAgain, 4, (TestRecord){5, 2, kIconKind3});
    expect_decode(sBlockAgain, n, GDX_PROFILE_ERR_VALUE, "corrupt: icon kind 3 gives VALUE");
    n = build_with(sBlockAgain, 4, (TestRecord){5, 2, kIconStock30});
    expect_decode(sBlockAgain, n, GDX_PROFILE_ERR_VALUE, "corrupt: stock pilot index 30 gives VALUE");

    n = build_with(sBlockAgain, 4, (TestRecord){5, 2, kIconCustomZero});
    expect_decode(sBlockAgain, n, GDX_PROFILE_ERR_LAYOUT, "corrupt: custom icon without pixels gives LAYOUT");

    memcpy(r, kStockRecords, sizeof(kStockRecords));
    r[5] = (TestRecord){GDX_PROFILE_TAG_PIXELS, CUSTOM_PIXEL_BYTES, sPixelBytes};
    n = build_container(sBlockAgain, "GPF1", 6, r, 6);
    expect_decode(sBlockAgain, n, GDX_PROFILE_ERR_VALUE, "corrupt: stock icon with pixels gives VALUE");

    memcpy(r, kStockRecords, sizeof(kStockRecords));
    r[4] = (TestRecord){5, 2, kIconCustomZero};
    r[5] = (TestRecord){GDX_PROFILE_TAG_PIXELS, 16383, sPixelBytes};
    n = build_container(sBlockAgain, "GPF1", 6, r, 6);
    expect_decode(sBlockAgain, n, GDX_PROFILE_ERR_LAYOUT, "corrupt: 16383 pixel bytes give LAYOUT");

    n = build_with(sBlockAgain, 0, (TestRecord){0, 3, kNameZor});
    expect_decode(sBlockAgain, n, GDX_PROFILE_ERR_LAYOUT, "corrupt: tag 0 gives LAYOUT");

    memcpy(sBlockAgain, sStock, 49);
    sBlockAgain[49] = 0xEE;
    sBlockAgain[50] = 0xEE;
    put32(sBlockAgain + 51, crc_ref(sBlockAgain, 51));
    expect_decode(sBlockAgain, 55, GDX_PROFILE_ERR_LAYOUT, "corrupt: two junk bytes before the CRC give LAYOUT");

    expect_decode(sOversize, sizeof(sOversize), GDX_PROFILE_ERR_LAYOUT, "corrupt: block longer than the maximum gives LAYOUT");

    CHECK((gdx_profile_decode(NULL, 53, &sProfileA) == GDX_PROFILE_ERR_ARGS) && is_zero(&sProfileA, sizeof(sProfileA)),
          "corrupt: NULL data gives ARGS and zeroes the output");
    CHECK(gdx_profile_decode(sStock, 53, NULL) == GDX_PROFILE_ERR_ARGS, "corrupt: NULL output gives ARGS");
}

static void test_flips_and_truncation(void) {
    static const unsigned char kMasks[2] = {0x01, 0x80};
    size_t cuts[8] = {0, 11, 12, 13, 100, 8000, 0, 0};
    int ok = 1;
    int m;
    size_t offset;
    size_t i;

    build_stock_block(sStock);
    for (m = 0; m < 2; m++) {
        for (offset = 0; offset < 53; offset++) {
            int expected = (offset < 4) ? GDX_PROFILE_ERR_MAGIC : ((offset < 6) ? GDX_PROFILE_ERR_VERSION : GDX_PROFILE_ERR_CRC);
            int rc;

            memcpy(sBlockAgain, sStock, 53);
            sBlockAgain[offset] ^= kMasks[m];
            memset(&sProfileA, 0xA5, sizeof(sProfileA));
            rc = gdx_profile_decode(sBlockAgain, 53, &sProfileA);
            if ((rc != expected) || !is_zero(&sProfileA, sizeof(sProfileA))) {
                ok = 0;
            }
        }
    }
    CHECK(ok, "flips: every byte of the stock block flipped without reseal gives MAGIC, VERSION or CRC by position");

    ok = 1;
    rng_seed(0xF11B5u);
    make_random_profile(&sProfileA, GDX_PROFILE_ICON_CUSTOM);
    i = (size_t)gdx_profile_encode(&sProfileA, sBlock, sizeof(sBlock));
    for (m = 0; m < 64; m++) {
        int expected;
        int rc;

        offset = rng_next() % i;
        memcpy(sBlockAgain, sBlock, i);
        sBlockAgain[offset] ^= (unsigned char)(1u + rng_next() % 255u);
        expected = (offset < 4) ? GDX_PROFILE_ERR_MAGIC : ((offset < 6) ? GDX_PROFILE_ERR_VERSION : GDX_PROFILE_ERR_CRC);
        rc = gdx_profile_decode(sBlockAgain, i, &sProfileB);
        if ((rc != expected) || !is_zero(&sProfileB, sizeof(sProfileB))) {
            ok = 0;
        }
    }
    CHECK(ok, "flips: 64 sampled bytes of a custom block flipped without reseal give MAGIC, VERSION or CRC");

    ok = 1;
    for (offset = 0; offset < 53; offset++) {
        int rc = gdx_profile_decode(sStock, offset, &sProfileA);

        if (offset < 12) {
            if ((rc != GDX_PROFILE_ERR_TRUNCATED) || !is_zero(&sProfileA, sizeof(sProfileA))) {
                ok = 0;
            }
        } else if ((rc == GDX_PROFILE_OK) || !is_zero(&sProfileA, sizeof(sProfileA))) {
            ok = 0;
        }
    }
    CHECK(ok, "truncation: every prefix of the stock block fails with zeroed output, below 12 bytes as TRUNCATED");

    ok = 1;
    cuts[6] = i - 5;
    cuts[7] = i - 1;
    for (m = 0; m < 8; m++) {
        int rc = gdx_profile_decode(sBlock, cuts[m], &sProfileA);

        if ((rc == GDX_PROFILE_OK) || !is_zero(&sProfileA, sizeof(sProfileA))) {
            ok = 0;
        }
        if ((cuts[m] < 12) && (rc != GDX_PROFILE_ERR_TRUNCATED)) {
            ok = 0;
        }
    }
    CHECK(ok, "truncation: sampled prefixes of a custom block fail with zeroed output");
}

static void test_unknown_tags(void) {
    static const unsigned char kJunkA[2] = {0xAB, 0xCD};
    static const unsigned char kJunkB[3] = {0x01, 0x02, 0x03};
    TestRecord r[7];
    size_t n;

    memcpy(r, kStockRecords, sizeof(kStockRecords));
    r[5] = (TestRecord){7, 2, kJunkA};
    r[6] = (TestRecord){200, 3, kJunkB};
    build_stock_block(sStock);
    n = build_container(sBlockAgain, "GPF1", 7, r, 7);
    CHECK(gdx_profile_decode(sBlockAgain, n, &sProfileA) == GDX_PROFILE_OK,
          "unknown tags: records 7 and 200 are skipped without error");
    CHECK((strcmp(sProfileA.name, "Zor") == 0) && (sProfileA.colour == 0x00FF8040u) &&
              (sProfileA.preferredMachine == 7) && (sProfileA.iconIndex == 3),
          "unknown tags: the known fields still decode");
    n = (size_t)gdx_profile_encode(&sProfileA, sBlock, sizeof(sBlock));
    CHECK((n == 53) && (memcmp(sBlock, sStock, 53) == 0),
          "unknown tags: they are not kept, so re-encode is the canonical 53-byte block");
}

// Id with the given tag byte, a fixed second byte and zeros. The tag must be non-zero so that the id is legal.
static void make_id(unsigned char* id, unsigned char tag) {
    memset(id, 0, GDX_PROFILE_ID_BYTES);
    id[0] = tag;
    id[1] = 0x5A;
}

// Stock profile named "Zor" whose id comes from make_id.
static void make_profile_with_id(GdxProfile* p, unsigned char tag) {
    make_stock_profile(p);
    make_id(p->id, tag);
}

// Empty store, then profiles with ids 1, 2 and 3 at indexes 0, 1 and 2, then the given active index.
static void store_with_three(int active) {
    int i;

    gdx_profile_store_clear(&sStore);
    for (i = 1; i <= 3; i++) {
        make_profile_with_id(&sProfileScratch, (unsigned char)i);
        gdx_profile_store_add(&sStore, &sProfileScratch);
    }
    gdx_profile_store_set_active(&sStore, active);
}

static void test_store_operations(void) {
    static const int kBadCounts[5][2] = {{-1, 0}, {9, 0}, {0, 1}, {1, 1}, {2, -1}};
    unsigned char id[GDX_PROFILE_ID_BYTES];
    int i;
    int rc;
    int ok;

    gdx_profile_store_clear(&sStore);
    make_profile_with_id(&sProfileA, 1);
    rc = gdx_profile_store_add(&sStore, &sProfileA);
    CHECK((rc == 0) && (sStore.count == 1) && (sStore.active == 0), "store add: the first profile gets index 0 and is active");
    make_profile_with_id(&sProfileB, 2);
    rc = gdx_profile_store_add(&sStore, &sProfileB);
    CHECK((rc == 1) && (sStore.count == 2) && (sStore.active == 0), "store add: the second profile gets index 1 and the active index stays 0");
    rc = gdx_profile_store_add(&sStore, &sProfileA);
    CHECK((rc == GDX_PROFILE_ERR_DUPLICATE) && (sStore.count == 2), "store add: a stored id gives DUPLICATE and changes nothing");
    make_profile_with_id(&sProfileScratch, 9);
    sProfileScratch.name[0] = ' ';
    rc = gdx_profile_store_add(&sStore, &sProfileScratch);
    CHECK((rc == GDX_PROFILE_ERR_VALUE) && (sStore.count == 2), "store add: an invalid profile gives VALUE");
    make_profile_with_id(&sProfileScratch, 1);
    sProfileScratch.name[0] = ' ';
    rc = gdx_profile_store_add(&sStore, &sProfileScratch);
    CHECK(rc == GDX_PROFILE_ERR_VALUE, "store add: an invalid profile with a stored id gives VALUE, which is checked first");

    ok = 1;
    for (i = 3; i <= 8; i++) {
        make_profile_with_id(&sProfileScratch, (unsigned char)i);
        if (gdx_profile_store_add(&sStore, &sProfileScratch) != i - 1) {
            ok = 0;
        }
    }
    CHECK(ok && (sStore.count == 8), "store add: eight distinct profiles fill the store at indexes 0 to 7");
    make_profile_with_id(&sProfileScratch, 9);
    rc = gdx_profile_store_add(&sStore, &sProfileScratch);
    CHECK((rc == GDX_PROFILE_ERR_FULL) && (sStore.count == 8), "store add: a ninth distinct id gives FULL");
    make_profile_with_id(&sProfileScratch, 4);
    rc = gdx_profile_store_add(&sStore, &sProfileScratch);
    CHECK(rc == GDX_PROFILE_ERR_DUPLICATE, "store add: a stored id on a full store gives DUPLICATE, which is checked before FULL");
    make_profile_with_id(&sProfileScratch, 9);
    sProfileScratch.colour = 0x01000000u;
    rc = gdx_profile_store_add(&sStore, &sProfileScratch);
    CHECK(rc == GDX_PROFILE_ERR_VALUE, "store add: an invalid profile on a full store gives VALUE, which is checked before FULL");
    CHECK(gdx_profile_store_add(&sStore, NULL) == GDX_PROFILE_ERR_ARGS, "store add: NULL profile gives ARGS");

    make_id(id, 5);
    CHECK(gdx_profile_store_find(&sStore, id) == 4, "store find: the profile with id 5 is at index 4");
    make_id(id, 20);
    CHECK(gdx_profile_store_find(&sStore, id) == GDX_PROFILE_ERR_NOT_FOUND, "store find: an absent id gives NOT_FOUND");
    CHECK(gdx_profile_store_find(&sStore, NULL) == GDX_PROFILE_ERR_ARGS, "store find: NULL id gives ARGS");
    CHECK(gdx_profile_store_find(NULL, id) == GDX_PROFILE_ERR_ARGS, "store find: NULL store gives ARGS");

    store_with_three(2);
    rc = gdx_profile_store_remove(&sStore, 0);
    make_id(id, 3);
    CHECK((rc == GDX_PROFILE_OK) && (sStore.count == 2) && (sStore.active == 1) && (gdx_profile_store_find(&sStore, id) == 1),
          "store remove: below the active index moves the active index down with its profile");
    store_with_three(2);
    rc = gdx_profile_store_remove(&sStore, 2);
    CHECK((rc == GDX_PROFILE_OK) && (sStore.count == 2) && (sStore.active == 1),
          "store remove: removing the active last profile makes the new last profile active");
    store_with_three(1);
    rc = gdx_profile_store_remove(&sStore, 1);
    make_id(id, 3);
    CHECK((rc == GDX_PROFILE_OK) && (sStore.count == 2) && (sStore.active == 1) && (gdx_profile_store_find(&sStore, id) == 1),
          "store remove: removing the active middle profile makes its successor active at the same index");
    store_with_three(0);
    rc = gdx_profile_store_remove(&sStore, 2);
    CHECK((rc == GDX_PROFILE_OK) && (sStore.count == 2) && (sStore.active == 0),
          "store remove: a profile after the active one leaves the active index alone");
    CHECK((gdx_profile_store_remove(&sStore, 2) == GDX_PROFILE_ERR_ARGS) && (gdx_profile_store_remove(&sStore, -1) == GDX_PROFILE_ERR_ARGS),
          "store remove: an index outside the store gives ARGS");
    gdx_profile_store_clear(&sStore);
    make_profile_with_id(&sProfileA, 1);
    gdx_profile_store_add(&sStore, &sProfileA);
    rc = gdx_profile_store_remove(&sStore, 0);
    CHECK((rc == GDX_PROFILE_OK) && (sStore.count == 0) && (sStore.active == 0),
          "store remove: removing the only profile gives an empty store with active 0");
    CHECK(gdx_profile_store_remove(&sStore, 0) == GDX_PROFILE_ERR_ARGS, "store remove: any index on an empty store gives ARGS");

    store_with_three(0);
    rc = gdx_profile_store_rename(&sStore, 1, "Ann");
    CHECK((rc == GDX_PROFILE_OK) && (strcmp(sStore.profiles[1].name, "Ann") == 0), "store rename: a legal name replaces the profile name");
    rc = gdx_profile_store_rename(&sStore, 1, " Ann");
    CHECK((rc == GDX_PROFILE_ERR_VALUE) && (strcmp(sStore.profiles[1].name, "Ann") == 0),
          "store rename: an illegal name gives VALUE and keeps the old name");
    rc = gdx_profile_store_rename(&sStore, 1, "0123456789abcdefg");
    CHECK((rc == GDX_PROFILE_ERR_VALUE) && (strcmp(sStore.profiles[1].name, "Ann") == 0),
          "store rename: a 17-character name gives VALUE and keeps the old name");
    CHECK((gdx_profile_store_rename(&sStore, 3, "Ann") == GDX_PROFILE_ERR_ARGS) && (gdx_profile_store_rename(&sStore, 0, NULL) == GDX_PROFILE_ERR_ARGS),
          "store rename: a bad index or a NULL name gives ARGS");

    rc = gdx_profile_store_set_active(&sStore, 2);
    CHECK((rc == GDX_PROFILE_OK) && (sStore.active == 2), "store set_active: index 2 becomes active");
    CHECK((gdx_profile_store_set_active(&sStore, 3) == GDX_PROFILE_ERR_ARGS) && (sStore.active == 2),
          "store set_active: index 3 gives ARGS and keeps the active index");
    CHECK((gdx_profile_store_set_active(&sStore, -1) == GDX_PROFILE_ERR_ARGS) && (sStore.active == 2),
          "store set_active: index -1 gives ARGS and keeps the active index");

    ok = 1;
    for (i = 0; i < 5; i++) {
        make_id(id, 1);
        sStore.count = kBadCounts[i][0];
        sStore.active = kBadCounts[i][1];
        if ((gdx_profile_store_add(&sStore, &sProfileA) != GDX_PROFILE_ERR_ARGS) ||
            (gdx_profile_store_remove(&sStore, 0) != GDX_PROFILE_ERR_ARGS) ||
            (gdx_profile_store_rename(&sStore, 0, "Ann") != GDX_PROFILE_ERR_ARGS) ||
            (gdx_profile_store_set_active(&sStore, 0) != GDX_PROFILE_ERR_ARGS) ||
            (gdx_profile_store_find(&sStore, id) != GDX_PROFILE_ERR_ARGS)) {
            ok = 0;
        }
        if ((gdx_profile_store_clear(&sStore) != GDX_PROFILE_OK) || (sStore.count != 0) || (sStore.active != 0)) {
            ok = 0;
        }
    }
    CHECK(ok, "store unusable: add, remove, rename, set_active and find give ARGS, and clear resets the store");
    CHECK(gdx_profile_store_clear(NULL) == GDX_PROFILE_ERR_ARGS, "store clear: NULL gives ARGS");
}

// Plain model of the store: ids as tag bytes, names as strings, and the removal rule from gdx_profile.h.
typedef struct StoreModel {
    int count;
    int active;
    unsigned char tag[GDX_PROFILE_CAPACITY];
    char name[GDX_PROFILE_CAPACITY][GDX_PROFILE_NAME_MAX + 1];
} StoreModel;

static int model_find(const StoreModel* m, unsigned char tag) {
    int i;

    for (i = 0; i < m->count; i++) {
        if (m->tag[i] == tag) {
            return i;
        }
    }
    return -1;
}

static void model_add(StoreModel* m, unsigned char tag) {
    m->tag[m->count] = tag;
    memcpy(m->name[m->count], "Zor", sizeof("Zor"));
    m->count++;
    if (m->count == 1) {
        m->active = 0;
    }
}

// The active index follows its profile. When the active profile itself goes, its successor takes the index, or the
// new last profile does when the removed one was at the end.
static void model_remove(StoreModel* m, int index) {
    int i;

    for (i = index; i + 1 < m->count; i++) {
        m->tag[i] = m->tag[i + 1];
        memcpy(m->name[i], m->name[i + 1], sizeof(m->name[i]));
    }
    m->count--;
    if (m->count == 0) {
        m->active = 0;
    } else if (index < m->active) {
        m->active--;
    } else if (m->active >= m->count) {
        m->active = m->count - 1;
    }
}

static void model_clear(StoreModel* m) {
    memset(m, 0, sizeof(*m));
}

// True when the store holds exactly the model's count, active index, ids and names.
static int store_matches_model(const GdxProfileStore* s, const StoreModel* m) {
    unsigned char id[GDX_PROFILE_ID_BYTES];
    int i;

    if ((s->count != m->count) || (s->active != m->active)) {
        return 0;
    }
    for (i = 0; i < m->count; i++) {
        make_id(id, m->tag[i]);
        if ((memcmp(s->profiles[i].id, id, GDX_PROFILE_ID_BYTES) != 0) || (strcmp(s->profiles[i].name, m->name[i]) != 0)) {
            return 0;
        }
    }
    return 1;
}

// 3000 random store operations, each compared with the model. Successes and refusals must both occur.
static void test_store_model(void) {
    static const char* const kNames[8] = {"Ann", " Ann", "Ann ", "", "0123456789abcdefg", "0123456789abcdef", "A B", "A\x7F"};
    StoreModel m;
    int step;
    int mismatches = 0;
    int adds = 0;
    int removes = 0;
    int renames = 0;
    int fulls = 0;
    int duplicates = 0;

    model_clear(&m);
    gdx_profile_store_clear(&sStore);
    rng_seed(0x570BE5u);
    for (step = 0; step < 3000; step++) {
        uint32_t r = rng_next() % 100u;
        unsigned char tag = (unsigned char)(1u + rng_next() % 12u);
        unsigned char id[GDX_PROFILE_ID_BYTES];
        const char* text = kNames[rng_next() % 8u];
        int index;
        int expect;
        int rc;

        if (r < 30u) {
            make_profile_with_id(&sProfileScratch, tag);
            if (model_find(&m, tag) >= 0) {
                expect = GDX_PROFILE_ERR_DUPLICATE;
            } else if (m.count == GDX_PROFILE_CAPACITY) {
                expect = GDX_PROFILE_ERR_FULL;
            } else {
                expect = m.count;
            }
            rc = gdx_profile_store_add(&sStore, &sProfileScratch);
            if (rc != expect) {
                mismatches++;
            }
            if (expect >= 0) {
                model_add(&m, tag);
                adds++;
            } else if (expect == GDX_PROFILE_ERR_FULL) {
                fulls++;
            } else {
                duplicates++;
            }
        } else if (r < 45u) {
            index = (int)(rng_next() % (uint32_t)(m.count + 2)) - 1;
            expect = ((index < 0) || (index >= m.count)) ? GDX_PROFILE_ERR_ARGS : GDX_PROFILE_OK;
            rc = gdx_profile_store_remove(&sStore, index);
            if (rc != expect) {
                mismatches++;
            }
            if (expect == GDX_PROFILE_OK) {
                model_remove(&m, index);
                removes++;
            }
        } else if (r < 60u) {
            index = (int)(rng_next() % (uint32_t)(m.count + 1));
            if (index >= m.count) {
                expect = GDX_PROFILE_ERR_ARGS;
            } else if (ref_name_ok(text, GDX_PROFILE_NAME_MAX)) {
                expect = GDX_PROFILE_OK;
            } else {
                expect = GDX_PROFILE_ERR_VALUE;
            }
            rc = gdx_profile_store_rename(&sStore, index, text);
            if (rc != expect) {
                mismatches++;
            }
            if (expect == GDX_PROFILE_OK) {
                snprintf(m.name[index], sizeof(m.name[index]), "%s", text);
                renames++;
            }
        } else if (r < 75u) {
            index = (int)(rng_next() % (uint32_t)(m.count + 2)) - 1;
            expect = ((index < 0) || (index >= m.count)) ? GDX_PROFILE_ERR_ARGS : GDX_PROFILE_OK;
            rc = gdx_profile_store_set_active(&sStore, index);
            if (rc != expect) {
                mismatches++;
            }
            if (expect == GDX_PROFILE_OK) {
                m.active = index;
            }
        } else if (r < 98u) {
            make_id(id, tag);
            index = model_find(&m, tag);
            expect = (index >= 0) ? index : GDX_PROFILE_ERR_NOT_FOUND;
            rc = gdx_profile_store_find(&sStore, id);
            if (rc != expect) {
                mismatches++;
            }
        } else {
            if (gdx_profile_store_clear(&sStore) != GDX_PROFILE_OK) {
                mismatches++;
            }
            model_clear(&m);
        }
        if (!store_matches_model(&sStore, &m)) {
            mismatches++;
        }
    }
    CHECK((mismatches == 0) && store_matches_model(&sStore, &m), "store model: 3000 random operations agree with a parallel model after every step");
    CHECK((adds > 0) && (removes > 0) && (renames > 0) && (fulls > 0) && (duplicates > 0),
          "store model: adds, removes, renames, FULL and DUPLICATE all occur, so the run is not trivial");
}

// Custom-icon profile with a 16-character name and random pixels. The tag byte keeps the ids distinct.
static void make_full_custom(GdxProfile* p, unsigned char tag) {
    size_t k;

    memset(p, 0, sizeof(*p));
    rng_fill(p->id, GDX_PROFILE_ID_BYTES);
    p->id[0] = tag;
    for (k = 0; k < GDX_PROFILE_NAME_MAX; k++) {
        p->name[k] = (char)('A' + (int)((k + tag) % 26u));
    }
    p->name[GDX_PROFILE_NAME_MAX] = '\0';
    p->colour = 0x00102030u + tag;
    p->preferredMachine = (int)(tag % 30u);
    p->iconKind = GDX_PROFILE_ICON_CUSTOM;
    p->iconIndex = 0;
    rng_fill(p->icon, GDX_PROFILE_ICON_BYTES);
}

// Profiles with ids 1 and 2 and active index 1. Writes the store block to out and returns its length or a code.
static int build_two_stock_store(unsigned char* out) {
    gdx_profile_store_clear(&sStore);
    make_profile_with_id(&sProfileA, 1);
    make_profile_with_id(&sProfileB, 2);
    gdx_profile_store_add(&sStore, &sProfileA);
    gdx_profile_store_add(&sStore, &sProfileB);
    gdx_profile_store_set_active(&sStore, 1);
    return gdx_profile_store_encode(&sStore, out, GDX_PROFILE_STORE_BLOCK_MAX_BYTES);
}

// Poisons sStoreCopy, decodes block into it and requires the code, with zeroed output after any failure.
static void expect_store_decode(const unsigned char* block, size_t length, int expected, const char* text) {
    int rc;

    memset(&sStoreCopy, 0xA5, sizeof(sStoreCopy));
    rc = gdx_profile_store_decode(block, length, &sStoreCopy);
    CHECK((rc == expected) && ((rc == GDX_PROFILE_OK) || is_zero(&sStoreCopy, sizeof(sStoreCopy))), text);
}

// Same count, active index and profiles, compared with same_profile.
static int stores_equal(const GdxProfileStore* a, const GdxProfileStore* b) {
    int i;

    if ((a->count != b->count) || (a->active != b->active)) {
        return 0;
    }
    for (i = 0; i < a->count; i++) {
        if (!same_profile(&a->profiles[i], &b->profiles[i])) {
            return 0;
        }
    }
    return 1;
}

static void test_store_codec(void) {
    static const unsigned char kEmptyStore[12] = {'G', 'P', 'S', '1', 0x01, 0x00, 0x01, 0x00, 0x01, 0x01, 0x00, 0x00};
    static const unsigned char kActive0[1] = {0x00};
    static const unsigned char kJunk[3] = {0xAB, 0xCD, 0xEF};
    TestRecord s[3];
    size_t nA;
    int i;
    int rc;
    int ok;

    gdx_profile_store_clear(&sStore);
    rc = gdx_profile_store_encode(&sStore, sStoreBlock, sizeof(sStoreBlock));
    CHECK((rc == 16) && (memcmp(sStoreBlock, kEmptyStore, sizeof(kEmptyStore)) == 0) && (crc_ref(sStoreBlock, 12) == get32(sStoreBlock + 12)),
          "store codec: the empty store encodes to 16 bytes: header, active record 0, CRC");

    rc = build_two_stock_store(sStoreBlock);
    CHECK(rc == 128, "store codec: two stock profiles encode to 128 bytes");
    ok = (gdx_profile_store_decode(sStoreBlock, 128, &sStoreCopy) == GDX_PROFILE_OK) && (sStoreCopy.count == 2) &&
         (sStoreCopy.active == 1) && same_profile(&sStoreCopy.profiles[0], &sProfileA) &&
         same_profile(&sStoreCopy.profiles[1], &sProfileB);
    CHECK(ok, "store codec: two stock profiles decode with the same active index and profiles");
    rc = gdx_profile_store_encode(&sStoreCopy, sStoreAgain, sizeof(sStoreAgain));
    CHECK((rc == 128) && (memcmp(sStoreAgain, sStoreBlock, 128) == 0),
          "store codec: re-encoding the decoded two-profile store is byte-identical");

    gdx_profile_store_clear(&sStore);
    for (i = 0; i < GDX_PROFILE_CAPACITY; i++) {
        make_full_custom(&sProfileA, (unsigned char)(i + 1));
        gdx_profile_store_add(&sStore, &sProfileA);
    }
    rc = gdx_profile_store_encode(&sStore, sStoreBlock, sizeof(sStoreBlock));
    CHECK(rc == 131664, "store codec: eight custom profiles with 16-character names encode to 131664 bytes");
    CHECK(gdx_profile_store_encode(&sStore, sStoreBlock, 131663) == GDX_PROFILE_ERR_FULL, "store codec: a cap one byte short gives FULL");
    rc = gdx_profile_store_decode(sStoreBlock, 131664, &sStoreCopy);
    CHECK((rc == GDX_PROFILE_OK) && stores_equal(&sStore, &sStoreCopy), "store codec: the eight-profile store decodes to the original");
    rc = gdx_profile_store_encode(&sStoreCopy, sStoreAgain, sizeof(sStoreAgain));
    CHECK((rc == 131664) && (memcmp(sStoreAgain, sStoreBlock, 131664) == 0),
          "store codec: re-encoding the eight-profile store is byte-identical");

    make_profile_with_id(&sProfileA, 1);
    nA = (size_t)gdx_profile_encode(&sProfileA, sBlock, sizeof(sBlock));
    s[0] = (TestRecord){1, 1, kActive0};
    s[1] = (TestRecord){2, nA, sBlock};
    s[2] = (TestRecord){10, sizeof(kJunk), kJunk};
    rc = (int)build_container(sStoreAgain, "GPS1", 3, s, 3);
    CHECK((rc == 78) && (gdx_profile_store_decode(sStoreAgain, 78, &sStoreCopy) == GDX_PROFILE_OK) && (sStoreCopy.count == 1) &&
              same_profile(&sStoreCopy.profiles[0], &sProfileA),
          "store codec: tags 10 and above are skipped on decode");
    rc = gdx_profile_store_encode(&sStoreCopy, sStoreAgain, sizeof(sStoreAgain));
    CHECK(rc == 72, "store codec: re-encoding without the skipped record gives the 72-byte canonical block");
}

static unsigned char sStoreOversize[GDX_PROFILE_STORE_BLOCK_MAX_BYTES + 1];

static void test_store_corrupt(void) {
    static const unsigned char kActive0[1] = {0x00};
    static const unsigned char kActive1[1] = {0x01};
    TestRecord ok3[3];
    size_t nA;
    size_t nB;
    size_t n;

    make_profile_with_id(&sProfileA, 1);
    make_profile_with_id(&sProfileB, 2);
    nA = (size_t)gdx_profile_encode(&sProfileA, sBlock, sizeof(sBlock));
    nB = (size_t)gdx_profile_encode(&sProfileB, sBlockAgain, sizeof(sBlockAgain));
    ok3[0] = (TestRecord){1, 1, kActive0};
    ok3[1] = (TestRecord){2, nA, sBlock};
    ok3[2] = (TestRecord){3, nB, sBlockAgain};
    n = build_container(sStoreAgain, "GPS1", 3, ok3, 3);
    expect_store_decode(sStoreAgain, n, GDX_PROFILE_OK, "store corrupt: the record-built two-slot store decodes");

    {
        TestRecord dup[3] = {{1, 1, kActive0}, {2, nA, sBlock}, {3, nA, sBlock}};

        n = build_container(sStoreAgain, "GPS1", 3, dup, 3);
        expect_store_decode(sStoreAgain, n, GDX_PROFILE_ERR_DUPLICATE, "store corrupt: two slots with one id give DUPLICATE");
    }
    {
        TestRecord bad[2] = {{1, 1, kActive1}, {2, nA, sBlock}};

        n = build_container(sStoreAgain, "GPS1", 2, bad, 2);
        expect_store_decode(sStoreAgain, n, GDX_PROFILE_ERR_VALUE, "store corrupt: active index 1 over one slot gives VALUE");
    }
    {
        TestRecord noActive[1] = {{2, nA, sBlock}};

        n = build_container(sStoreAgain, "GPS1", 1, noActive, 1);
        expect_store_decode(sStoreAgain, n, GDX_PROFILE_ERR_LAYOUT, "store corrupt: no active record gives LAYOUT");
    }
    {
        TestRecord gap[3] = {{1, 1, kActive0}, {2, nA, sBlock}, {4, nA, sBlock}};

        n = build_container(sStoreAgain, "GPS1", 3, gap, 3);
        expect_store_decode(sStoreAgain, n, GDX_PROFILE_ERR_LAYOUT, "store corrupt: slot tags 2 then 4 give LAYOUT");
    }
    {
        TestRecord order[2] = {{2, nA, sBlock}, {1, 1, kActive0}};

        n = build_container(sStoreAgain, "GPS1", 2, order, 2);
        expect_store_decode(sStoreAgain, n, GDX_PROFILE_ERR_LAYOUT, "store corrupt: a slot before the active record gives LAYOUT");
    }
    sBlock[20] ^= 0x10;
    n = build_container(sStoreAgain, "GPS1", 3, ok3, 3);
    sBlock[20] ^= 0x10;
    expect_store_decode(sStoreAgain, n, GDX_PROFILE_ERR_CRC, "store corrupt: a slot with a bad inner CRC gives CRC");

    n = build_container(sStoreAgain, "GPS1", 3, ok3, 3);
    memcpy(sStoreAgain, "GPX1", 4);
    reseal(sStoreAgain, n);
    expect_store_decode(sStoreAgain, n, GDX_PROFILE_ERR_MAGIC, "store corrupt: wrong magic gives MAGIC");

    n = build_container(sStoreAgain, "GPS1", 3, ok3, 3);
    put16(sStoreAgain + 4, 2u);
    reseal(sStoreAgain, n);
    expect_store_decode(sStoreAgain, n, GDX_PROFILE_ERR_VERSION, "store corrupt: version 2 gives VERSION");

    n = build_container(sStoreAgain, "GPS1", 3, ok3, 3);
    sStoreAgain[30] ^= 0x04;
    expect_store_decode(sStoreAgain, n, GDX_PROFILE_ERR_CRC, "store corrupt: a flipped byte without reseal gives CRC");

    expect_store_decode(sStoreAgain, 11, GDX_PROFILE_ERR_TRUNCATED, "store corrupt: 11 bytes give TRUNCATED");
    expect_store_decode(sStoreAgain, 0, GDX_PROFILE_ERR_TRUNCATED, "store corrupt: 0 bytes give TRUNCATED");
    memset(sStoreOversize, 'G', sizeof(sStoreOversize));
    expect_store_decode(sStoreOversize, sizeof(sStoreOversize), GDX_PROFILE_ERR_LAYOUT,
                        "store corrupt: a block longer than the maximum gives LAYOUT");

    gdx_profile_store_clear(&sStore);
    gdx_profile_store_add(&sStore, &sProfileA);
    sStore.count = 2;
    sStore.profiles[1] = sStore.profiles[0];
    CHECK(gdx_profile_store_encode(&sStore, sStoreBlock, sizeof(sStoreBlock)) == GDX_PROFILE_ERR_DUPLICATE,
          "store encode: two slots with one id give DUPLICATE");
    gdx_profile_store_clear(&sStore);
    gdx_profile_store_add(&sStore, &sProfileA);
    sStore.active = 1;
    CHECK(gdx_profile_store_encode(&sStore, sStoreBlock, sizeof(sStoreBlock)) == GDX_PROFILE_ERR_VALUE,
          "store encode: an active index outside the store gives VALUE");
    gdx_profile_store_clear(&sStore);
    gdx_profile_store_add(&sStore, &sProfileA);
    sStore.profiles[0].name[0] = ' ';
    CHECK(gdx_profile_store_encode(&sStore, sStoreBlock, sizeof(sStoreBlock)) == GDX_PROFILE_ERR_VALUE,
          "store encode: an invalid stored profile gives VALUE");
    CHECK((gdx_profile_store_encode(NULL, sStoreBlock, sizeof(sStoreBlock)) == GDX_PROFILE_ERR_ARGS) &&
              (gdx_profile_store_encode(&sStore, NULL, 16) == GDX_PROFILE_ERR_ARGS),
          "store encode: a NULL store or a NULL output gives ARGS");
    memset(&sStoreCopy, 0xA5, sizeof(sStoreCopy));
    CHECK((gdx_profile_store_decode(NULL, 16, &sStoreCopy) == GDX_PROFILE_ERR_ARGS) && is_zero(&sStoreCopy, sizeof(sStoreCopy)),
          "store decode: NULL data gives ARGS and zeroes the output");
    CHECK(gdx_profile_store_decode(sStoreAgain, 16, NULL) == GDX_PROFILE_ERR_ARGS, "store decode: NULL output gives ARGS");
}

static unsigned char sIconExpected[GDX_PROFILE_ICON_BYTES];
static unsigned char sIconOut[GDX_PROFILE_ICON_BYTES];

static void test_icons(void) {
    static const unsigned char kQuad[16] = {10, 20, 30, 40, 50, 60, 70, 80, 90, 100, 110, 120, 130, 140, 150, 160};
    static const unsigned char kOne[4] = {9, 8, 7, 6};
    size_t i;
    int ok = 1;

    {
        size_t n = build_with(sBlockAgain, 4, (TestRecord){5, 2, kIconInitialsZero});

        CHECK((gdx_profile_decode(sBlockAgain, n, &sProfileB) == GDX_PROFILE_OK) && (sProfileB.iconKind == GDX_PROFILE_ICON_INITIALS) &&
                  is_zero(sProfileB.icon, sizeof(sProfileB.icon)),
              "profile codec: an initials icon decodes with its pixels left zero");
    }

    CHECK(gdx_profile_icon_solid(sIconOut, 0x123456u) == GDX_PROFILE_OK, "icon solid: a legal colour gives OK");
    for (i = 0; i < GDX_PROFILE_ICON_BYTES; i += 4) {
        if ((sIconOut[i] != 0x12) || (sIconOut[i + 1] != 0x34) || (sIconOut[i + 2] != 0x56) || (sIconOut[i + 3] != 0xFF)) {
            ok = 0;
        }
    }
    CHECK(ok, "icon solid: every pixel is the colour with alpha 255");
    CHECK(gdx_profile_icon_solid(sIconOut, 0x01000000u) == GDX_PROFILE_ERR_VALUE, "icon solid: a colour above 0xFFFFFF gives VALUE");
    CHECK(gdx_profile_icon_solid(NULL, 0x123456u) == GDX_PROFILE_ERR_ARGS, "icon solid: NULL gives ARGS");

    memset(sIconOut, 0x5C, sizeof(sIconOut));
    CHECK((gdx_profile_icon_from_rgba(NULL, 2, 2, sIconOut) == GDX_PROFILE_ERR_ARGS) &&
              (gdx_profile_icon_from_rgba(kQuad, 2, 2, NULL) == GDX_PROFILE_ERR_ARGS) &&
              (gdx_profile_icon_from_rgba(kQuad, 0, 2, sIconOut) == GDX_PROFILE_ERR_ARGS) &&
              (gdx_profile_icon_from_rgba(kQuad, 2, GDX_PROFILE_ICON_SRC_MAX + 1, sIconOut) == GDX_PROFILE_ERR_ARGS) &&
              (gdx_profile_icon_from_rgba(kQuad, -1, 2, sIconOut) == GDX_PROFILE_ERR_ARGS),
          "icon from_rgba: NULL pointers and sizes outside 1..4096 give ARGS");
    ok = 1;
    for (i = 0; i < GDX_PROFILE_ICON_BYTES; i++) {
        if (sIconOut[i] != 0x5C) {
            ok = 0;
        }
    }
    CHECK(ok, "icon from_rgba: the failed calls leave dst untouched");

    CHECK(gdx_profile_icon_from_rgba(kQuad, 2, 2, sIconOut) == GDX_PROFILE_OK, "icon from_rgba: a 2x2 source is accepted");
    CHECK((memcmp(&sIconOut[(31 * 64 + 31) * 4], kQuad, 4) == 0) && (memcmp(&sIconOut[(32 * 64 + 32) * 4], kQuad + 12, 4) == 0) &&
              (memcmp(&sIconOut[(0 * 64 + 63) * 4], kQuad + 4, 4) == 0) && (memcmp(&sIconOut[(63 * 64 + 0) * 4], kQuad + 8, 4) == 0),
          "icon from_rgba: a 2x2 source is enlarged by nearest neighbour to the four expected samples");

    CHECK(gdx_profile_icon_from_rgba(kOne, 1, 1, sIconOut) == GDX_PROFILE_OK, "icon from_rgba: a 1x1 source is accepted");
    ok = 1;
    for (i = 0; i < GDX_PROFILE_ICON_BYTES; i += 4) {
        if (memcmp(&sIconOut[i], kOne, 4) != 0) {
            ok = 0;
        }
    }
    CHECK(ok, "icon from_rgba: a 1x1 source fills the icon with that pixel");
}

// Icon oracles. Every source is built in sSource, which holds the largest case used here.
#define ORACLE_SRC_MAX 300

static unsigned char sSource[ORACLE_SRC_MAX * ORACLE_SRC_MAX * 4];

// Opaque gradient for mode 0. Mode 1 also varies alpha and makes every fourth diagonal pixel fully transparent.
static void fill_pattern(int w, int h, int mode) {
    int x;
    int y;

    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            unsigned char* p = sSource + ((size_t)y * (size_t)w + (size_t)x) * 4u;

            p[0] = (unsigned char)(x * 7 + y * 3);
            p[1] = (unsigned char)((x * 11) ^ (y * 5));
            p[2] = (unsigned char)(x * 13 + y * 17 + 40);
            p[3] = 255;
            if (mode == 1) {
                p[3] = (unsigned char)((x * 37 + y * 11) & 0xFF);
                if (((x + y) % 4) == 0) {
                    p[3] = 0;
                }
            }
        }
    }
}

// Opaque (200,100,50) on alternate pixels and a stray colour at alpha 0 on the rest, so each 2x2 cell is half and half.
static void fill_checker(int w, int h) {
    static const unsigned char kOpaque[4] = {200, 100, 50, 255};
    static const unsigned char kHidden[4] = {9, 9, 9, 0};
    int x;
    int y;

    for (y = 0; y < h; y++) {
        for (x = 0; x < w; x++) {
            memcpy(sSource + ((size_t)y * (size_t)w + (size_t)x) * 4u, (((x + y) & 1) == 0) ? kOpaque : kHidden, 4);
        }
    }
}

static void fill_transparent(int w, int h) {
    int i;

    fill_pattern(w, h, 0);
    for (i = 0; i < w * h; i++) {
        sSource[(size_t)i * 4u + 3u] = 0;
    }
}

static void fill_random(int w, int h) {
    rng_fill(sSource, (size_t)w * (size_t)h * 4u);
}

// Reference for crops up to the icon size: the centred square, sampled at the centre of each output cell.
static void oracle_nearest(int w, int h, unsigned char* out) {
    int side = (w < h) ? w : h;
    int x0 = (w - side) / 2;
    int y0 = (h - side) / 2;
    int oy;
    int ox;

    for (oy = 0; oy < GDX_PROFILE_ICON_SIZE; oy++) {
        int sy = y0 + (int)(((double)oy + 0.5) * side / GDX_PROFILE_ICON_SIZE);

        for (ox = 0; ox < GDX_PROFILE_ICON_SIZE; ox++) {
            int sx = x0 + (int)(((double)ox + 0.5) * side / GDX_PROFILE_ICON_SIZE);

            memcpy(out + ((size_t)oy * GDX_PROFILE_ICON_SIZE + (size_t)ox) * 4u,
                   sSource + ((size_t)sy * (size_t)w + (size_t)sx) * 4u, 4);
        }
    }
}

// Reference for crops above the icon size. Colour is the alpha-weighted mean and alpha the mean over all pixels of
// the cell, both rounded half up. A cell with no alpha is all zero.
static void oracle_box(int w, int h, unsigned char* out) {
    int side = (w < h) ? w : h;
    int x0 = (w - side) / 2;
    int y0 = (h - side) / 2;
    int oy;
    int ox;

    for (oy = 0; oy < GDX_PROFILE_ICON_SIZE; oy++) {
        int ya = y0 + (int)(oy * (double)side / GDX_PROFILE_ICON_SIZE);
        int yb = y0 + (int)((oy + 1) * (double)side / GDX_PROFILE_ICON_SIZE);

        for (ox = 0; ox < GDX_PROFILE_ICON_SIZE; ox++) {
            int xa = x0 + (int)(ox * (double)side / GDX_PROFILE_ICON_SIZE);
            int xb = x0 + (int)((ox + 1) * (double)side / GDX_PROFILE_ICON_SIZE);
            uint64_t sr = 0;
            uint64_t sg = 0;
            uint64_t sb = 0;
            uint64_t sa = 0;
            uint64_t count = 0;
            unsigned char* o = out + ((size_t)oy * GDX_PROFILE_ICON_SIZE + (size_t)ox) * 4u;
            int y;
            int x;

            for (y = ya; y < yb; y++) {
                for (x = xa; x < xb; x++) {
                    const unsigned char* p = sSource + ((size_t)y * (size_t)w + (size_t)x) * 4u;

                    sr += (uint64_t)p[3] * p[0];
                    sg += (uint64_t)p[3] * p[1];
                    sb += (uint64_t)p[3] * p[2];
                    sa += p[3];
                    count++;
                }
            }
            if (sa == 0) {
                memset(o, 0, 4);
            } else {
                o[0] = (unsigned char)((sr + sa / 2) / sa);
                o[1] = (unsigned char)((sg + sa / 2) / sa);
                o[2] = (unsigned char)((sb + sa / 2) / sa);
                o[3] = (unsigned char)((sa + count / 2) / count);
            }
        }
    }
}

// True when the module's output for the w by h source in sSource equals the reference for that size.
static int matches_oracle(int w, int h) {
    int rc;

    if (((w < h) ? w : h) > GDX_PROFILE_ICON_SIZE) {
        oracle_box(w, h, sIconExpected);
    } else {
        oracle_nearest(w, h, sIconExpected);
    }
    rc = gdx_profile_icon_from_rgba(sSource, w, h, sIconOut);
    return (rc == GDX_PROFILE_OK) && (memcmp(sIconOut, sIconExpected, GDX_PROFILE_ICON_BYTES) == 0);
}

static void test_icon_oracles(void) {
    int mismatches = 0;
    int small = 0;
    int large = 0;
    int ok = 1;
    int k;
    size_t i;

    fill_pattern(64, 64, 0);
    CHECK((gdx_profile_icon_from_rgba(sSource, 64, 64, sIconOut) == GDX_PROFILE_OK) &&
              (memcmp(sIconOut, sSource, GDX_PROFILE_ICON_BYTES) == 0),
          "icon oracle: a 64 by 64 source is copied unchanged");
    fill_pattern(64, 80, 0);
    CHECK(matches_oracle(64, 80), "icon oracle: 64 by 80 takes the centred 64 by 64 crop unchanged");
    fill_pattern(100, 40, 0);
    CHECK(matches_oracle(100, 40), "icon oracle: 100 by 40 enlarges the centred 40 by 40 crop");
    fill_pattern(101, 40, 0);
    CHECK(matches_oracle(101, 40), "icon oracle: 101 by 40 crops at x 30, which leaves the extra column on the right");
    fill_pattern(200, 150, 1);
    CHECK(matches_oracle(200, 150), "icon oracle: 200 by 150 with varied alpha matches the box filter");
    fill_pattern(201, 150, 1);
    CHECK(matches_oracle(201, 150), "icon oracle: 201 by 150 crops at x 25 and matches the box filter");
    fill_pattern(128, 128, 1);
    CHECK(matches_oracle(128, 128), "icon oracle: 128 by 128 with varied alpha matches the box filter");

    fill_checker(128, 128);
    CHECK(gdx_profile_icon_from_rgba(sSource, 128, 128, sIconOut) == GDX_PROFILE_OK, "icon box: the checker source is accepted");
    ok = 1;
    for (i = 0; i < GDX_PROFILE_ICON_BYTES; i += 4) {
        if ((sIconOut[i] != 200) || (sIconOut[i + 1] != 100) || (sIconOut[i + 2] != 50) || (sIconOut[i + 3] != 128)) {
            ok = 0;
        }
    }
    CHECK(ok, "icon box: a 128 by 128 checker of opaque and transparent pixels gives (200,100,50,128) in every cell");
    CHECK(matches_oracle(128, 128), "icon box: the checker agrees with the reference");

    fill_transparent(128, 128);
    CHECK((gdx_profile_icon_from_rgba(sSource, 128, 128, sIconOut) == GDX_PROFILE_OK) && is_zero(sIconOut, GDX_PROFILE_ICON_BYTES),
          "icon box: a fully transparent 128 by 128 source gives all-zero output, colours included");

    rng_seed(0x1C0DEu);
    for (k = 0; k < 40; k++) {
        int w = 1 + (int)(rng_next() % 300u);
        int h = 1 + (int)(rng_next() % 300u);

        if ((k & 1) == 0) {
            fill_pattern(w, h, (int)(rng_next() & 1u));
        } else {
            fill_random(w, h);
        }
        if (((w < h) ? w : h) > GDX_PROFILE_ICON_SIZE) {
            large++;
        } else {
            small++;
        }
        if (!matches_oracle(w, h)) {
            mismatches++;
        }
    }
    CHECK(mismatches == 0, "icon oracle: 40 random sizes from 1 to 300, patterned and noisy, agree with the reference");
    CHECK((small > 0) && (large > 0), "icon oracle: the random sizes reach both the box filter and the nearest branch");
}

// Decode failures the module may return. Anything else from a decode is a broken contract.
static int is_decode_failure(int rc) {
    return (rc == GDX_PROFILE_ERR_TRUNCATED) || (rc == GDX_PROFILE_ERR_MAGIC) || (rc == GDX_PROFILE_ERR_VERSION) ||
           (rc == GDX_PROFILE_ERR_CRC) || (rc == GDX_PROFILE_ERR_LAYOUT) || (rc == GDX_PROFILE_ERR_VALUE) ||
           (rc == GDX_PROFILE_ERR_DUPLICATE);
}

// Random legal profiles are encoded, one to four body bytes are changed and the CRC is resealed, so the parser sees a
// valid checksum. Accepted blocks must validate and survive an encode and decode round trip. Failures must zero the
// output and carry a known code.
static void fuzz_profiles(void) {
    static const int kKinds[3] = {GDX_PROFILE_ICON_STOCK, GDX_PROFILE_ICON_CUSTOM, GDX_PROFILE_ICON_INITIALS};
    int accepted = 0;
    int rejected = 0;
    int bad = 0;
    int it;

    rng_seed(0xF0221u);
    for (it = 0; it < 3000; it++) {
        size_t length;
        size_t mutations;
        size_t m;
        int n;
        int n2;
        int rc;

        make_random_profile(&sProfileA, kKinds[rng_next() % 3u]);
        n = gdx_profile_encode(&sProfileA, sBlock, sizeof(sBlock));
        if (n < 0) {
            bad++;
            continue;
        }
        length = (size_t)n;
        mutations = 1u + rng_next() % 4u;
        for (m = 0; m < mutations; m++) {
            size_t pos = rng_next() % (length - 4u);

            sBlock[pos] ^= (unsigned char)(1u + rng_next() % 255u);
        }
        reseal(sBlock, length);
        rc = gdx_profile_decode(sBlock, length, &sProfileB);
        if (rc == GDX_PROFILE_OK) {
            accepted++;
            n2 = gdx_profile_encode(&sProfileB, sBlockAgain, sizeof(sBlockAgain));
            if ((gdx_profile_validate(&sProfileB) != GDX_PROFILE_OK) || (n2 < 0) ||
                (gdx_profile_decode(sBlockAgain, (size_t)n2, &sProfileScratch) != GDX_PROFILE_OK) ||
                !same_profile(&sProfileB, &sProfileScratch)) {
                bad++;
            }
        } else {
            rejected++;
            if (!is_decode_failure(rc) || !is_zero(&sProfileB, sizeof(sProfileB))) {
                bad++;
            }
        }
    }
    CHECK(bad == 0, "profile fuzz: 3000 resealed mutations keep to known codes, accepted blocks validate and round-trip, failures zero the output");
    CHECK((accepted > 0) && (rejected > 0), "profile fuzz: both accepted and rejected blocks occur");
}

static unsigned char sStoreRound[GDX_PROFILE_STORE_BLOCK_MAX_BYTES];

// Reseals the GPF1 slot record that holds byte pos when pos lies in a slot value, so a mutation inside a profile reaches
// the store checks instead of failing the inner CRC. Walks the records the way the decoder does and stops at any record
// that runs past the body.
static void reseal_slot_containing(unsigned char* block, size_t length, size_t pos) {
    size_t at = 8;

    while (at + 3 <= length - 4) {
        size_t len = (size_t)block[at + 1] | ((size_t)block[at + 2] << 8);

        if (at + 3 + len > length - 4) {
            return;
        }
        if ((pos >= at) && (pos < at + 3 + len)) {
            if ((block[at] >= GDX_PROFILE_STORE_TAG_SLOT) && (pos >= at + 3) && (len >= 4)) {
                reseal(block + at + 3, len);
            }
            return;
        }
        at += 3 + len;
    }
}

// Store fuzz: the two-profile store block is mutated and resealed. A mutation inside a slot also reseals that slot, so
// accepted stores hold a usable count and active index and survive an encode and decode round trip.
static void fuzz_store(void) {
    int length;
    int accepted = 0;
    int rejected = 0;
    int bad = 0;
    int it;

    length = build_two_stock_store(sStoreAgain);
    CHECK(length == 128, "store fuzz: the two-profile seed store encodes to 128 bytes");
    rng_seed(0x57F0Au);
    for (it = 0; it < 800; it++) {
        size_t mutations;
        size_t m;
        int n2;
        int rc;

        memcpy(sStoreBlock, sStoreAgain, (size_t)length);
        mutations = 1u + rng_next() % 4u;
        for (m = 0; m < mutations; m++) {
            size_t pos = rng_next() % ((size_t)length - 4u);

            sStoreBlock[pos] ^= (unsigned char)(1u + rng_next() % 255u);
            reseal_slot_containing(sStoreBlock, (size_t)length, pos);
        }
        reseal(sStoreBlock, (size_t)length);
        rc = gdx_profile_store_decode(sStoreBlock, (size_t)length, &sStoreCopy);
        if (rc == GDX_PROFILE_OK) {
            int usable = (sStoreCopy.count >= 0) && (sStoreCopy.count <= GDX_PROFILE_CAPACITY) &&
                         ((sStoreCopy.count == 0) ? (sStoreCopy.active == 0)
                                                  : ((sStoreCopy.active >= 0) && (sStoreCopy.active < sStoreCopy.count)));

            accepted++;
            n2 = gdx_profile_store_encode(&sStoreCopy, sStoreRound, sizeof(sStoreRound));
            if (!usable || (n2 < 0) || (gdx_profile_store_decode(sStoreRound, (size_t)n2, &sStore) != GDX_PROFILE_OK) ||
                !stores_equal(&sStoreCopy, &sStore)) {
                bad++;
            }
        } else {
            rejected++;
            if (!is_decode_failure(rc) || !is_zero(&sStoreCopy, sizeof(sStoreCopy))) {
                bad++;
            }
        }
    }
    CHECK(bad == 0, "store fuzz: 800 resealed mutations keep to known codes, accepted stores are usable and round-trip, failures zero the output");
    CHECK((accepted > 0) && (rejected > 0), "store fuzz: both accepted and rejected stores occur");
}

static void test_fuzz(void) {
    fuzz_profiles();
    fuzz_store();
}

int main(void) {
    crc_table_init();
    test_crc_reference();
    test_names();
    test_author_alignment();
    test_normalise();
    test_validate();
    test_stock_block();
    test_round_trips();
    test_corrupt_profiles();
    test_flips_and_truncation();
    test_unknown_tags();
    test_store_operations();
    test_store_model();
    test_store_codec();
    test_store_corrupt();
    test_icons();
    test_icon_oracles();
    test_fuzz();
    printf("%s: %d failure(s)\n", (sFailures == 0) ? "all checks passed" : "checks failed", sFailures);
    return (sFailures == 0) ? 0 : 1;
}
