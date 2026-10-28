#!/usr/bin/env python3
"""Emit port/gen/gdx_o2r_expected[.<profile>].h from a validated deterministic archive.

Produces the single code-level contract (C3) between the harness (1-C) and
first-boot validation (1-B):

  #define GDX_O2R_EXPECTED_SHA256      "<hex>"      (US)     / _SHA256_JP      (JP)
  #define GDX_O2R_EXPECTED_ENTRY_COUNT <n>          (US)     / _ENTRY_COUNT_JP (JP)

IMPORTANT: the input MUST be the archive produced by the deterministic
gdx-extract, NOT the legacy build-time archive (which predates determinism and
has an unstable SHA-256).

R5 (C-R5.5) — per-profile goldens:
  --profile us  (default)  -> port/gen/gdx_o2r_expected.h    (GDX_O2R_EXPECTED_*)
  --profile jp             -> port/gen/gdx_o2r_expected.jp.h (GDX_O2R_EXPECTED_*_JP)

The JP ROM is NOT on disk in this repo, so a real JP golden is OWNER-RUN-REQUIRED.
Running `--profile jp` WITHOUT `--archive` emits a clearly-marked OWNER-RUN-REQUIRED
PLACEHOLDER header (all-zero SHA-256, entry count 0) so the JP build compiles and its
runtime extraction gate stays "experimental" (installs without the golden gate). When
the owner runs a real JP extraction and passes `--profile jp --archive <jp.o2r>`, this
writes the real JP golden. This script NEVER fabricates a JP SHA-256 or CRC.
"""

import argparse
import os
import sys

import o2r_common as oc

HEADER_TEMPLATE = """\
/*
 * {basename}
 *
 * GENERATED FILE - do not edit by hand.
 * Generator : tools/o2r_harness/gen_expected_header.py --profile {profile}
 * Source     : deterministic gdx-extract output ({source_name})
 * Source SHA : {sha256}
 * Content    : {content_sha256}
 * Entry count: {count}
 * Version CRC: {crc_str} ({profile} rev0, P0 contract C4)
 *
 * Golden constants for first-boot validation (P0 contract C3/C5). The archive
 * SHA-256 is a compile-time constant ONLY because extraction is deterministic
 * and the ROM is hash-validated. Regenerate via the o2r_harness gauntlet after
 * any change to the extractor, recipes, or ROM profile.
 */
#ifndef {guard}
#define {guard}

#define {macro_sha} "{sha256}"
#define {macro_count} {count}

/*
 * Order-independent content digest: SHA-256 over the sorted (name, CRC-32, size)
 * of every central-directory record. Where the archive can be reproduced
 * byte-for-byte the SHA-256 above is the gate; on a future toolchain proven to
 * vary only the container layout, this is what can stand in.
 * It proves content and completeness; only container layout is left unchecked.
 */
#define {macro_content} "{content_sha256}"

#endif /* {guard} */
"""

JP_PLACEHOLDER_TEMPLATE = """\
/*
 * {basename}
 *
 * OWNER-RUN-REQUIRED PLACEHOLDER (R5 / C-R5.5) - do not edit by hand.
 * Generator : tools/o2r_harness/gen_expected_header.py --profile jp
 *
 * The JP (F-ZERO X JP REV 0) ROM is NOT on disk in this repository, so a real JP
 * golden CANNOT be computed here and is NOT fabricated. This placeholder ships an
 * all-zero SHA-256 and entry count 0, which:
 *   - lets the EXPERIMENTAL VERSION_JP build compile (the macros are defined), and
 *   - keeps the runtime JP extraction gate in "experimental" mode (it installs
 *     fzerox-jp.o2r WITHOUT the SHA-256 / entry-count golden gate, since an all-zero
 *     hash can never match a real archive).
 *
 * TO PRODUCE THE REAL JP GOLDEN (owner, with the JP ROM in hand):
 *   1. Run the deterministic gdx-extract against the JP rev0 recipe tree
 *      (assets/yaml/jp/rev0) to produce a JP generic.o2r.
 *   2. Validate it: validate_archive.py --profile jp --archive <jp.o2r>
 *   3. Regenerate this header:
 *        gen_expected_header.py --profile jp --archive <jp.o2r>
 *
 * JP rev0 ROM SHA-1 (identity from decomp/config.yml, NOT a golden): {rom_sha1}
 */
#ifndef GDX_O2R_EXPECTED_JP_H
#define GDX_O2R_EXPECTED_JP_H

#define {macro_sha} "{placeholder_sha}"
#define {macro_count} 0
#define {macro_content} "{placeholder_sha}"

#endif /* GDX_O2R_EXPECTED_JP_H */
"""


def default_out(profile):
    base = "gdx_o2r_expected.h" if profile == "us" else "gdx_o2r_expected.{}.h".format(profile)
    return os.path.join(os.path.dirname(__file__), "..", "..", "port", "gen", base)


def write_jp_placeholder(out):
    macro_sha, macro_count = oc.profile_macros("jp")
    content = JP_PLACEHOLDER_TEMPLATE.format(
        basename=os.path.basename(out),
        rom_sha1=oc.EXPECTED_ROM_SHA1_JP,
        macro_sha=macro_sha,
        macro_count=macro_count,
        macro_content=macro_sha.replace("_SHA256", "_CONTENT_SHA256"),
        placeholder_sha=oc.PLACEHOLDER_SHA256,
    )
    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    with open(out, "w", encoding="utf-8", newline="\n") as f:
        f.write(content)
    print("Wrote OWNER-RUN-REQUIRED JP placeholder header: %s" % out)
    print("  (all-zero SHA-256 + entry count 0; the JP runtime gate stays experimental until a real")
    print("   JP extraction is run — this script does NOT fabricate a JP golden.)")
    return 0


def main(argv):
    ap = argparse.ArgumentParser(description="Emit port/gen/gdx_o2r_expected[.<profile>].h.")
    ap.add_argument("--profile", choices=["us", "jp"], default="us",
                    help="golden profile (default: us). 'jp' without --archive emits the "
                         "OWNER-RUN-REQUIRED placeholder header.")
    ap.add_argument("--archive", required=False, help="validated deterministic .o2r")
    ap.add_argument("--out", default=None, help="output header path (default: per-profile)")
    ap.add_argument(
        "--force",
        action="store_true",
        help="write the header even if entry-count / version checks fail (NOT recommended)",
    )
    args = ap.parse_args(argv)

    profile = args.profile
    out = args.out or default_out(profile)

    # JP with no archive: emit the OWNER-RUN-REQUIRED placeholder (never fabricate a JP golden).
    if profile == "jp" and not args.archive:
        return write_jp_placeholder(out)

    if not args.archive:
        print("ERROR: --archive is required for --profile %s." % profile, file=sys.stderr)
        return 2
    if not os.path.isfile(args.archive):
        print("ERROR: archive not found: %s" % args.archive, file=sys.stderr)
        return 2

    macro_sha, macro_count = oc.profile_macros(profile)
    macro_content = macro_sha.replace("_SHA256", "_CONTENT_SHA256")
    expected_count = oc.profile_expected_count(profile)
    expected_crc = oc.profile_expected_crc(profile)

    count = oc.record_count(args.archive)  # dup-inclusive central-directory records (C3)
    sha = oc.sha256_file(args.archive)
    content_sha = oc.content_digest(args.archive)
    ver = oc.check_version_entry(args.archive, expected_crc)

    print("profile     : %s" % profile)
    print("archive     : %s" % args.archive)
    if expected_count is not None:
        print("entry count : %d (C3 expects %d)" % (count, expected_count))
    else:
        print("entry count : %d (no expected count for this profile — owner-run)" % count)
    print("sha256      : %s" % sha)
    print("content     : %s" % content_sha)
    if ver.get("crc") is not None:
        print("version CRC : 0x%08X  raw=%s  (%s)" % (ver["crc"], ver["raw_hex"], ver["reason"]))
    else:
        print("version     : %s" % ver["reason"])

    problems = []
    if expected_count is not None and count != expected_count:
        problems.append("entry count %d != C3 expected %d" % (count, expected_count))
    if not ver["ok"]:
        problems.append("version entry check failed: %s" % ver["reason"])

    if problems and not args.force:
        print("", file=sys.stderr)
        for p in problems:
            print("REFUSING to write header: %s" % p, file=sys.stderr)
        print("(use --force to override; this is almost always a bug)", file=sys.stderr)
        return 1

    crc = ver.get("crc") or 0
    crc_str = "0x%08X" % crc if ver.get("crc") is not None else "unknown (owner-run)"
    out_dir = os.path.dirname(os.path.abspath(out))
    os.makedirs(out_dir, exist_ok=True)

    content = HEADER_TEMPLATE.format(
        basename=os.path.basename(out),
        profile=profile,
        source_name=os.path.basename(args.archive),
        sha256=sha,
        content_sha256=content_sha,
        count=count,
        crc_str=crc_str,
        guard="GDX_O2R_EXPECTED_H" if profile == "us" else "GDX_O2R_EXPECTED_{}_H".format(profile.upper()),
        macro_sha=macro_sha,
        macro_count=macro_count,
        macro_content=macro_content,
    )
    with open(out, "w", encoding="utf-8", newline="\n") as f:
        f.write(content)

    print("")
    print("Wrote %s" % out)
    if problems:
        print("WARNING: written under --force despite: %s" % "; ".join(problems))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
