// port/gdx_palette_crc.h: palette-aware hi-res texture keys (plan item HT). Pure C: no IO, no allocation.
// Copyright (c) Zorkats
//
// GLideN64 keys a hi-res texture on 64 bits: the low 32 are the Rice CRC of the texels, the high 32 the CRC of
// the palette the texture is drawn with (0 for non-CI textures). tools/gen_htc_pack.py writes one image per
// palette CRC for a CI texture that a .htc pack holds, at
//
//   textures/pack/<base key>.pal/<CC>-<XXXXXXXX>
//
// CC is cimax, the highest colour index the texture uses, as 2 hex digits. XXXXXXXX is the palette CRC as 8 hex
// digits. Both are uppercase when the converter writes them; either case parses. At draw time the game hashes the
// loaded palette over cimax + 1 entries and uses the image whose CRC matches, or the stock texture when none does.
// A texture held under one palette also ships as the plain override textures/pack/<base key>, which draws under
// any palette; a variant for the active palette still wins over it.
//
// The palette CRC is RiceCRC32 over one row of (cimax + 1) 16-bit colours, read the way GLideN64 reads its RDRAM
// copy. GLideN64 keeps RDRAM as host 32-bit words, so host byte i holds N64 byte i ^ 3, and the CRC reads
// little-endian words at x = row - 4, row - 8, ... while x >= 0. With an odd entry count the row is 2 mod 4 bytes
// long, so every read is half a word off and takes its bytes from two N64 words. The Python twin is
// gen_htc_pack.palette_crc; both are rewritten from the description, not copied.

#ifndef GDX_PALETTE_CRC_H
#define GDX_PALETTE_CRC_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GDX_PALETTE_VARIANT_DIR ".pal/"
#define GDX_PALETTE_VARIANT_LEAF_LEN 11 // "CC-XXXXXXXX"
#define GDX_PALETTE_CI4_BYTES 32        // one 16-entry CI4 palette
#define GDX_PALETTE_CI8_BYTES 512       // the whole 256-entry CI8 palette

// The CRC of the first `entries` colours of `palette` (N64 byte order, `size` bytes). Returns 1 and sets *outCrc,
// or 0 when entries is below 2 (GLideN64 would read before the palette) or the reads would pass the end of the
// buffer: the first read touches N64 bytes up to ((entries * 2 - 1) | 3).
int gdx_palette_crc(const uint8_t* palette, size_t size, unsigned entries, uint32_t* outCrc);

// Parses a variant leaf of exactly GDX_PALETTE_VARIANT_LEAF_LEN characters, "CC-XXXXXXXX". Returns 1 and sets
// *outCimax and *outCrc, or 0 for any other text, and for a cimax of 0 (one colour has no palette CRC).
int gdx_palette_variant_parse_leaf(const char* leaf, size_t len, unsigned* outCimax, uint32_t* outCrc);

// Splits "<key>.pal/<leaf>" at its last GDX_PALETTE_VARIANT_DIR. Returns 1 with *outKeyLen the length of <key>
// and *outLeaf pointing at <leaf>, or 0 when there is no such separator, <key> is empty, or <leaf> contains '/'.
// The leaf is not parsed.
int gdx_palette_variant_split(const char* path, size_t* outKeyLen, const char** outLeaf);

#ifdef __cplusplus
}
#endif

#endif
