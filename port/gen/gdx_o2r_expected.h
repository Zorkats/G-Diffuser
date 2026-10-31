/*
 * gdx_o2r_expected.h
 *
 * GENERATED FILE - do not edit by hand.
 * Generator : tools/o2r_harness/gen_expected_header.py --profile us
 * Source     : deterministic gdx-extract output (generic.o2r)
 * Source SHA : 5d2814fbc51cfa1ca8e6b5c771d6774479d408f513dbeb88766a711c585fa82d
 * Content    : 2b71c1c89552d3d4e4df2b3fb911aa462f1ed6cb63fc1ec92f4985c8782a1e56
 * Entry count: 3613
 * Version CRC: 0x78D90EB3 (us rev0, P0 contract C4)
 *
 * Golden constants for first-boot validation (P0 contract C3/C5). The archive
 * SHA-256 is a compile-time constant ONLY because extraction is deterministic
 * and the ROM is hash-validated. Regenerate via the o2r_harness gauntlet after
 * any change to the extractor, recipes, or ROM profile.
 */
#ifndef GDX_O2R_EXPECTED_H
#define GDX_O2R_EXPECTED_H

#define GDX_O2R_EXPECTED_SHA256 "5d2814fbc51cfa1ca8e6b5c771d6774479d408f513dbeb88766a711c585fa82d"
#define GDX_O2R_EXPECTED_ENTRY_COUNT 3613

/*
 * Order-independent content digest: SHA-256 over the sorted (name, CRC-32, size)
 * of every central-directory record. Where the archive can be reproduced
 * byte-for-byte the SHA-256 above is the gate; on a future toolchain proven to
 * vary only the container layout, this is what can stand in.
 * It proves content and completeness; only container layout is left unchecked.
 */
#define GDX_O2R_EXPECTED_CONTENT_SHA256 "2b71c1c89552d3d4e4df2b3fb911aa462f1ed6cb63fc1ec92f4985c8782a1e56"

#endif /* GDX_O2R_EXPECTED_H */
