/*
 * gdx_o2r_expected.h
 *
 * GENERATED FILE - do not edit by hand.
 * Generator : tools/o2r_harness/gen_expected_header.py --profile us
 * Source     : deterministic gdx-extract output (generic.o2r)
 * Source SHA : f2983f2f135ccbc96ff601810fac43c5fbdb77a158ed49233a00a8a1d0e26311
 * Content    : 21a5a375000d613a2a9be8375d2f710fb8d8404333c4a9fd304b989aaebdc26c
 * Entry count: 3611
 * Version CRC: 0x78D90EB3 (us rev0, P0 contract C4)
 *
 * Golden constants for first-boot validation (P0 contract C3/C5). The archive
 * SHA-256 is a compile-time constant ONLY because extraction is deterministic
 * and the ROM is hash-validated. Regenerate via the o2r_harness gauntlet after
 * any change to the extractor, recipes, or ROM profile.
 */
#ifndef GDX_O2R_EXPECTED_H
#define GDX_O2R_EXPECTED_H

#define GDX_O2R_EXPECTED_SHA256 "f2983f2f135ccbc96ff601810fac43c5fbdb77a158ed49233a00a8a1d0e26311"
#define GDX_O2R_EXPECTED_ENTRY_COUNT 3611

/*
 * Order-independent content digest: SHA-256 over the sorted (name, CRC-32, size)
 * of every central-directory record. Where the archive can be reproduced
 * byte-for-byte the SHA-256 above is the gate; on a future toolchain proven to
 * vary only the container layout, this is what can stand in.
 * It proves content and completeness; only container layout is left unchecked.
 */
#define GDX_O2R_EXPECTED_CONTENT_SHA256 "21a5a375000d613a2a9be8375d2f710fb8d8404333c4a9fd304b989aaebdc26c"

#endif /* GDX_O2R_EXPECTED_H */
