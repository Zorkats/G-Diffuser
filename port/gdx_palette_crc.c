// port/gdx_palette_crc.c: palette CRC and variant names for palette-aware hi-res textures. See gdx_palette_crc.h.
// Copyright (c) Zorkats

#include "gdx_palette_crc.h"

#include <string.h>

// The little-endian word GLideN64 reads at host byte x of its word-swapped RDRAM copy: host byte i is N64 byte
// i ^ 3. The caller has checked that every byte read is inside the palette.
static uint32_t host_word(const uint8_t* palette, size_t x) {
    uint32_t word = 0;
    for (size_t k = 0; k < 4; k++) {
        word |= (uint32_t)palette[(x + k) ^ 3u] << (8u * k);
    }
    return word;
}

int gdx_palette_crc(const uint8_t* palette, size_t size, unsigned entries, uint32_t* outCrc) {
    if (palette == NULL || outCrc == NULL || entries < 2) {
        return 0;
    }
    const size_t row = (size_t)entries * 2u;
    if (((row - 1u) | 3u) >= size) {
        return 0;
    }
    uint32_t crc = 0;
    uint32_t word = 0;
    // x runs row - 4, row - 8, ... down to 0 or 2; a signed loop would read the same bytes.
    for (size_t x = row - 4u;; x -= 4u) {
        word = host_word(palette, x) ^ (uint32_t)x;
        crc = (crc << 4) | (crc >> 28);
        crc += word;
        if (x < 4u) {
            break;
        }
    }
    *outCrc = crc + word; // the closing "word ^ y" of RiceCRC32, with y = 0 for one row
    return 1;
}

static int hex_digit(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    return -1;
}

static int parse_hex(const char* text, size_t len, uint32_t* out) {
    uint32_t value = 0;
    for (size_t i = 0; i < len; i++) {
        const int d = hex_digit(text[i]);
        if (d < 0) {
            return 0;
        }
        value = (value << 4) | (uint32_t)d;
    }
    *out = value;
    return 1;
}

int gdx_palette_variant_parse_leaf(const char* leaf, size_t len, unsigned* outCimax, uint32_t* outCrc) {
    if (leaf == NULL || outCimax == NULL || outCrc == NULL || len != GDX_PALETTE_VARIANT_LEAF_LEN || leaf[2] != '-') {
        return 0;
    }
    uint32_t cimax = 0;
    uint32_t crc = 0;
    if (!parse_hex(leaf, 2, &cimax) || !parse_hex(leaf + 3, 8, &crc) || cimax == 0) {
        return 0;
    }
    *outCimax = (unsigned)cimax;
    *outCrc = crc;
    return 1;
}

int gdx_palette_variant_split(const char* path, size_t* outKeyLen, const char** outLeaf) {
    if (path == NULL || outKeyLen == NULL || outLeaf == NULL) {
        return 0;
    }
    const size_t dirLen = sizeof(GDX_PALETTE_VARIANT_DIR) - 1u;
    const char* last = NULL;
    for (const char* p = strstr(path, GDX_PALETTE_VARIANT_DIR); p != NULL; p = strstr(p + 1, GDX_PALETTE_VARIANT_DIR)) {
        last = p;
    }
    if (last == NULL || last == path || strchr(last + dirLen, '/') != NULL) {
        return 0;
    }
    *outKeyLen = (size_t)(last - path);
    *outLeaf = last + dirLen;
    return 1;
}
