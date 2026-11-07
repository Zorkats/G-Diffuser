// port/tests/gdx_palette_crc_tests.c: checks for port/gdx_palette_crc.c. Plain C with no framework. Prints one
// PASS or FAIL line per check and exits 0 only when every check passes. Every palette is built here.
// Copyright (c) Zorkats

#include "gdx_palette_crc.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int sFailures = 0;

static void record(int ok, const char* text) {
    if (ok) {
        printf("PASS: %s\n", text);
    } else {
        printf("FAIL: %s\n", text);
        sFailures++;
    }
}

static int crc_is(const uint8_t* palette, size_t size, unsigned entries, uint32_t want) {
    uint32_t got = 0;
    return gdx_palette_crc(palette, size, entries, &got) == 1 && got == want;
}

static int crc_refused(const uint8_t* palette, size_t size, unsigned entries) {
    uint32_t got = 0xA5A5A5A5u;
    return gdx_palette_crc(palette, size, entries, &got) == 0 && got == 0xA5A5A5A5u;
}

static void test_hand_derived_answers(void) {
    const uint8_t two[4] = { 0x12, 0x34, 0x56, 0x78 };
    const uint8_t eight[8] = { 0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77 };

    // One aligned read: the big-endian word 0x12345678; crc = word, then + word.
    record(crc_is(two, sizeof two, 2, 0x2468ACF0u), "2 entries: one aligned read, doubled");
    // One read at x = 2: host bytes 2..5 are N64 bytes 1, 0, 7, 6, so the word is 0x66770011, xor 2.
    record(crc_is(eight, sizeof eight, 3, 0xCCEE0026u), "3 entries: half-word-off read uses the i ^ 3 swap");
    // Reads at x = 4 then 0: 0x44556677 ^ 4, rotate, + 0x00112233, + 0x00112233.
    record(crc_is(eight, sizeof eight, 4, 0x4578AB9Au), "4 entries: two aligned reads with the rotate");
}

// Values from tools/gen_htc_pack.palette_crc over the same pattern, so the two implementations agree.
static void test_matches_the_converter(void) {
    static const struct {
        unsigned entries;
        uint32_t crc;
    } kVectors[] = {
        { 2, 0x1660AAF4u },   { 3, 0xD21C1664u },   { 5, 0x9C56128Bu },  { 7, 0xD388659Cu },
        { 10, 0xE507EBC1u },  { 11, 0xB986CB64u },  { 16, 0x144B601Du }, { 17, 0x59F83146u },
        { 128, 0x7B5779D1u }, { 255, 0xF2A9B33Cu }, { 256, 0x36C6558Eu },
    };
    uint8_t pattern[GDX_PALETTE_CI8_BYTES];
    for (unsigned i = 0; i < sizeof pattern; i++) {
        pattern[i] = (uint8_t)(((i * 37u + 11u) ^ (i >> 3)) & 0xFFu);
    }
    for (size_t v = 0; v < sizeof kVectors / sizeof kVectors[0]; v++) {
        char text[96];
        snprintf(text, sizeof text, "%u entries match gen_htc_pack.palette_crc", kVectors[v].entries);
        record(crc_is(pattern, sizeof pattern, kVectors[v].entries, kVectors[v].crc), text);
    }
    // A CI4 palette is the first 32 bytes; 16 entries fit exactly.
    record(crc_is(pattern, GDX_PALETTE_CI4_BYTES, 16, 0x144B601Du), "16 entries fit a 32-byte CI4 palette");
}

static void test_refusals(void) {
    const uint8_t eight[8] = { 0 };
    uint32_t crc = 0;

    record(crc_refused(eight, sizeof eight, 0), "0 entries refused");
    record(crc_refused(eight, sizeof eight, 1), "1 entry refused: GLideN64 would read before the palette");
    record(crc_refused(eight, 7, 3), "3 entries need 8 bytes: the read at x = 2 touches N64 byte 7");
    record(crc_refused(eight, 3, 2), "2 entries need 4 bytes");
    uint8_t ci4[GDX_PALETTE_CI4_BYTES] = { 0 };
    record(crc_refused(ci4, sizeof ci4, 17), "17 entries do not fit a CI4 palette");
    record(crc_refused(NULL, 8, 2), "null palette refused");
    record(gdx_palette_crc(eight, sizeof eight, 2, NULL) == 0, "null output refused");
    record(gdx_palette_crc(eight, sizeof eight, 3, &crc) == 1, "3 entries accepted in 8 bytes");
}

static void test_leaf_parsing(void) {
    unsigned cimax = 0;
    uint32_t crc = 0;
    int ok = gdx_palette_variant_parse_leaf("0F-52C9D728", 11, &cimax, &crc);
    record(ok == 1 && cimax == 15 && crc == 0x52C9D728u, "uppercase leaf parses");
    ok = gdx_palette_variant_parse_leaf("ff-0000abcd", 11, &cimax, &crc);
    record(ok == 1 && cimax == 255 && crc == 0x0000ABCDu, "lowercase leaf parses");

    static const char* const kBad[] = {
        "00-52C9D728",  // cimax 0 has no palette CRC
        "0F_52C9D728",  // wrong separator
        "0F-52C9D72G",  // not hex
        "0G-52C9D728",  // not hex
        "F-52C9D7280",  // separator in the wrong place
        "+F-52C9D728",  // sign
    };
    for (size_t i = 0; i < sizeof kBad / sizeof kBad[0]; i++) {
        char text[64];
        cimax = 99;
        crc = 99;
        snprintf(text, sizeof text, "bad leaf '%s' refused", kBad[i]);
        record(gdx_palette_variant_parse_leaf(kBad[i], strlen(kBad[i]), &cimax, &crc) == 0 && cimax == 99 &&
                   crc == 99,
               text);
    }
    record(gdx_palette_variant_parse_leaf("0F-52C9D72", 10, &cimax, &crc) == 0, "short leaf refused");
    record(gdx_palette_variant_parse_leaf("0F-52C9D7280", 12, &cimax, &crc) == 0, "long leaf refused");
    record(gdx_palette_variant_parse_leaf(NULL, 11, &cimax, &crc) == 0, "null leaf refused");
}

static void test_split(void) {
    size_t keyLen = 0;
    const char* leaf = NULL;
    const char* path = "common_assets_compressed/D_F228010.pal/09-B579507D";
    int ok = gdx_palette_variant_split(path, &keyLen, &leaf);
    record(ok == 1 && keyLen == strlen("common_assets_compressed/D_F228010") && strcmp(leaf, "09-B579507D") == 0,
           "split finds key and leaf");

    path = "a.pal/b.pal/0F-00000001";
    ok = gdx_palette_variant_split(path, &keyLen, &leaf);
    record(ok == 1 && keyLen == strlen("a.pal/b") && strcmp(leaf, "0F-00000001") == 0, "split uses the last .pal/");

    record(gdx_palette_variant_split("textures/plain", &keyLen, &leaf) == 0, "no .pal/ refused");
    record(gdx_palette_variant_split(".pal/0F-00000001", &keyLen, &leaf) == 0, "empty key refused");
    record(gdx_palette_variant_split("k.pal/sub/0F-00000001", &keyLen, &leaf) == 0, "leaf with a slash refused");
    record(gdx_palette_variant_split("k.pal/x.pal/sub/0F-1", &keyLen, &leaf) == 0,
           "leaf after the last .pal/ with a slash refused");
    record(gdx_palette_variant_split(NULL, &keyLen, &leaf) == 0, "null path refused");
}

int main(void) {
    test_hand_derived_answers();
    test_matches_the_converter();
    test_refusals();
    test_leaf_parsing();
    test_split();
    printf("%s: %d failure(s)\n", sFailures == 0 ? "OK" : "FAILED", sFailures);
    return sFailures == 0 ? 0 : 1;
}
