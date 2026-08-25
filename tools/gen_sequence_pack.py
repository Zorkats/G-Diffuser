#!/usr/bin/env python3
"""Pack a directory of <name>.seq sequence files into a deterministic .o2r sequence pack.

Quick start:

    python tools/gen_sequence_pack.py my-seq-dir/ my-seq-pack.o2r --name "My Music" --author you

    # validate a source folder or an existing pack without writing anything:
    python tools/gen_sequence_pack.py my-seq-dir/ --check
    python tools/gen_sequence_pack.py my-seq-pack.o2r --check

Input:
  <input_dir>/
    <name>.seq         raw native sequence (aseq) bytes, one file per replaced sequence.
                       <name> must be one of the 23 known sequence names (listed by --names;
                       unknown names are warned about and skipped).
    workshop.json      (optional) pack metadata: {name, version, author, game_version, id,
                       depends, conflicts}. If absent, one is synthesized from the --name /
                       --author / --version / --id flags (this packer never errors just because
                       the metadata file is missing).

Output: one .o2r (a ZIP, like tools/gen_texture_pack.py) holding each <name>.seq at archive path
"audio/seq/<name>" (the key convention the runtime resolves in port/gdx_audio_seq_packs.cpp), plus
the metadata at the archive root as workshop.json.

Runtime rules worth knowing (the packer does not enforce these — see docs/MODDING_GUIDE.md):
  - Overrides only apply while the "Sequence packs" master switch is on (off by default).
  - An override must FIT the stock sequence's heap slot; oversized entries are ignored at
    runtime and the stock sequence loads instead.
  - Overrides apply on the next song/scene load or after Reload packs.

The archive is written deterministically (sorted entries, fixed 1980 timestamps) so identical
inputs produce byte-identical output.
"""
import argparse
import json
import os
import sys
import zipfile

FIXED_DATE = (1980, 1, 1, 0, 0, 0)

# The 23 replaceable sequences, in SeqId order (decomp/include/sfx.h, EXPANSION_KIT branch:
# SEQ_GUITAR=0 .. SEQ_DDBGM_EAD_DEMO=22). The runtime table in port/gdx_audio_seq_packs.cpp
# must match this list.
SEQ_NAMES = [
    "guitar",
    "sound_effects",
    "ddbgm_mute_city",
    "ddbgm_silence",
    "ddbgm_sand_ocean",
    "ddbgm_port_town",
    "ddbgm_big_blue",
    "ddbgm_devils_forest",
    "ddbgm_red_canyon",
    "ddbgm_sector",
    "ddbgm_white_land",
    "ddbgm_rainbow_road",
    "ddbgm_new_03",
    "ddbgm_new_02",
    "ddbgm_new_01",
    "ddbgm_new_04",
    "ddbgm_title",
    "ddbgm_select",
    "ddbgm_option",
    "ddbgm_deathrace",
    "ddbgm_course_editor",
    "ddbgm_machine_editor",
    "ddbgm_ead_demo",
]
SEQ_NAME_SET = frozenset(SEQ_NAMES)

# Stock sequence sizes from decomp/src/audio/disk/audio_tables.c (gSequenceTable) and
# decomp/assets/yaml/jp/ek/audio_seq_dd.yaml. Overrides must fit this slot at runtime;
# a stock-extraction override should normally match the stock size byte-for-byte.
SEQ_STOCK_SIZES = {
    "guitar": 0x30,
    "sound_effects": 0xB50,
    "ddbgm_mute_city": 0x40,
    "ddbgm_silence": 0x40,
    "ddbgm_sand_ocean": 0x40,
    "ddbgm_port_town": 0x40,
    "ddbgm_big_blue": 0x40,
    "ddbgm_devils_forest": 0x40,
    "ddbgm_red_canyon": 0x40,
    "ddbgm_sector": 0x40,
    "ddbgm_white_land": 0x40,
    "ddbgm_rainbow_road": 0x40,
    "ddbgm_new_03": 0x40,
    "ddbgm_new_02": 0x40,
    "ddbgm_new_01": 0x40,
    "ddbgm_new_04": 0x40,
    "ddbgm_title": 0x30,
    "ddbgm_select": 0x30,
    "ddbgm_option": 0x30,
    "ddbgm_deathrace": 0x40,
    "ddbgm_course_editor": 0x1930,
    "ddbgm_machine_editor": 0x13B0,
    "ddbgm_ead_demo": 0x40,
}


def _stock_size_mismatch(name, blob):
    """Return a warning string when the .seq size differs from the stock sequence size."""
    expected = SEQ_STOCK_SIZES.get(name)
    if expected is None or len(blob) == expected:
        return None
    # Common extraction mistake: title/select/option are 48 bytes (0x30); the race/menu
    # DD BGM tracks are 64 bytes (0x40). Point this out so authors spot a swapped ROM dump.
    hint = ""
    if len(blob) == 0x30 and expected == 0x40:
        hint = " (this is the size of ddbgm_title/ddbgm_select/ddbgm_option; " \
               "did you extract the wrong ROM sequence?)"
    elif len(blob) == 0x40 and expected == 0x30:
        hint = " (this is the size of the 64-byte race/menu tracks; " \
               "did you extract the wrong ROM sequence?)"
    return "%s: file is %d bytes but the stock sequence is %d bytes%s" % (
        name, len(blob), expected, hint)


def validate_pack_id(pid):
    """Pack ids join into comma-separated lists at runtime (load order, disable list), so an id
    must be a non-empty string with no commas. Raises ValueError with a clear message."""
    if not isinstance(pid, str) or not pid.strip():
        raise ValueError('pack metadata "id" must be a non-empty string')
    if "," in pid:
        raise ValueError('pack id "%s" must not contain commas (the runtime stores ids in '
                         'comma-joined lists)' % pid)


def resolve_manifest_bytes(args):
    """Return (manifest_bytes, source_description). Prefer an on-disk metadata file
    (workshop.json, then legacy manifest.json); otherwise synthesize one from the CLI flags so a
    missing metadata file is never a fatal error."""
    if args.manifest:
        candidates = [args.manifest]
    else:
        candidates = [os.path.join(args.input_dir, "workshop.json"),
                      os.path.join(args.input_dir, "manifest.json")]

    base = {}
    source = "synthesized from flags"
    for path in candidates:
        if os.path.isfile(path):
            with open(path, "r", encoding="utf-8") as fh:
                base = json.load(fh)  # raises on malformed JSON — surfaced to the caller
            source = os.path.basename(path)
            break

    default_name = (os.path.splitext(os.path.basename(args.output_o2r))[0]
                    if args.output_o2r else "sequence-pack")
    manifest = {
        "name": args.name or base.get("name") or default_name,
        "version": args.version or base.get("version") or "0.1",
        "author": args.author or base.get("author") or "unknown",
        "game_version": args.game_version or base.get("game_version") or "us.rev0",
        # Stable pack identity (load order, disable list); defaults to the output filename stem,
        # which is also the runtime's fallback for old packs with no id.
        "id": args.id or base.get("id") or default_name,
        # What this pack carries, so the Workshop tab and future tooling can tell pack kinds apart.
        "pack_type": "sequence",
    }
    # Preserve any extra fields the modder added to their metadata file (depends, conflicts, ...).
    for k, v in base.items():
        manifest.setdefault(k, v)
    validate_pack_id(manifest["id"])
    return json.dumps(manifest, indent=2, sort_keys=True).encode("utf-8"), source


def _font_name_to_seq_id(text):
    """Normalize a .font sidecar payload to a SeqId/FontId index, or None if unknown."""
    if text is None:
        return None
    name = text.strip()
    if name.upper().startswith("FONT_"):
        name = name[5:]
    name = name.lower()
    if name in SEQ_NAME_SET:
        return name
    return None


def collect_seqs(input_dir):
    """All <name>.seq and <name>.font files directly inside input_dir (seq keys are flat)."""
    seqs = []
    fonts = {}
    for f in sorted(os.listdir(input_dir)):
        full = os.path.join(input_dir, f)
        if not os.path.isfile(full):
            continue
        lower = f.lower()
        if lower.endswith(".seq"):
            seqs.append((f[:-len(".seq")], full))
        elif lower.endswith(".font"):
            fonts[f[:-len(".font")]] = full
    return seqs, fonts


def plan_pack(input_dir):
    """Validate every .seq against the known sequence names. Returns (entries, errors, warnings)
    where entries is the list of (arc_path, blob) ready to write. Collects ALL problems."""
    entries = []
    errors = []
    warnings = []

    seqs, fonts = collect_seqs(input_dir)
    if not seqs:
        errors.append("no .seq files found in %s" % input_dir)
        return entries, errors, warnings

    seen = set()
    for name, full in seqs:
        if name in seen:
            warnings.append("%s: duplicate (case-insensitive .seq match) - skipped" % name)
            continue
        seen.add(name)
        if name not in SEQ_NAME_SET:
            warnings.append("%s: not a known sequence name (see --names) - skipped" % name)
            continue
        with open(full, "rb") as fh:
            blob = fh.read()
        if not blob:
            errors.append("%s: empty file (a 0-byte override would never resolve)" % name)
            continue
        mismatch = _stock_size_mismatch(name, blob)
        if mismatch:
            warnings.append(mismatch)
        entries.append(("audio/seq/" + name, blob))

        font_path = fonts.pop(name, None)
        if font_path is not None:
            with open(font_path, "rb") as fh:
                font_blob = fh.read()
            font_name = _font_name_to_seq_id(font_blob.decode("utf-8", errors="replace"))
            if font_name is None:
                warnings.append("%s: %s.font has unknown font reference '%s' - skipped"
                                % (name, name, font_blob.decode("utf-8", errors="replace").strip()))
            else:
                entries.append(("audio/seq/" + name + ".font", font_blob))

    for orphan_name in sorted(fonts):
        warnings.append("%s: .font sidecar without a matching .seq - skipped" % orphan_name)

    missing = sorted(SEQ_NAME_SET - seen)
    if missing:
        print("info:  %d sequence(s) have no .seq in this pack (left as stock): %s"
              % (len(missing), ", ".join(missing)))
    return entries, errors, warnings


def check_pack(path):
    """Validate an existing .o2r pack without writing. Returns process exit code."""
    if not zipfile.is_zipfile(path):
        sys.stderr.write("error: %s is not a valid .o2r (zip) archive\n" % path)
        return 2
    errors = []
    warnings = []
    seq_count = 0
    font_count = 0
    with zipfile.ZipFile(path, "r") as z:
        names = z.namelist()
        if "workshop.json" not in names and "manifest.json" not in names:
            errors.append("no workshop.json (pack metadata) at archive root")
        for name in names:
            if name in ("workshop.json", "manifest.json"):
                continue
            if not name.startswith("audio/seq/"):
                errors.append("%s: not under audio/seq/ (unknown entry)" % name)
                continue
            tail = name[len("audio/seq/"):]
            blob = z.read(name)
            if len(blob) == 0:
                errors.append("%s: empty entry" % name)
                continue
            if tail.lower().endswith(".font"):
                seq_name = tail[:-len(".font")]
                if seq_name not in SEQ_NAME_SET:
                    errors.append("%s: not a known sequence name" % name)
                    continue
                font_ref = _font_name_to_seq_id(blob.decode("utf-8", errors="replace"))
                if font_ref is None:
                    errors.append("%s: unknown font reference '%s'" %
                                  (name, blob.decode("utf-8", errors="replace").strip()))
                    continue
                base_key = "audio/seq/" + seq_name
                if base_key not in names:
                    warnings.append("%s: .font sidecar without matching %s" % (name, base_key))
                font_count += 1
                continue
            seq_name = tail
            if seq_name not in SEQ_NAME_SET:
                errors.append("%s: not a known sequence name" % name)
                continue
            mismatch = _stock_size_mismatch(seq_name, blob)
            if mismatch:
                warnings.append(mismatch)
            seq_count += 1
        meta = None
        if "workshop.json" in names:
            try:
                meta = json.loads(z.read("workshop.json"))
            except Exception as exc:  # noqa: BLE001
                errors.append("workshop.json is not valid JSON (%s)" % exc)
        if isinstance(meta, dict):
            pid = meta.get("id")
            if not isinstance(pid, str) or not pid.strip():
                errors.append('workshop.json has no usable "id" (old packs fall back to the '
                              'archive basename; add an "id" like "author.packname")')
            elif "," in pid:
                errors.append('workshop.json "id" "%s" must not contain commas (ids are stored '
                              'in comma-joined lists)' % pid)
    print("pack: %s" % path)
    print("  %d sequence override(s), %d font sidecar(s)" % (seq_count, font_count))
    if seq_count == 0:
        errors.append("no sequence override entries found — pack overrides nothing")
    for msg in warnings:
        print("warn:  %s" % msg)
    for msg in errors:
        sys.stderr.write("error: %s\n" % msg)
    if errors:
        print("FAILED: %d problem(s)" % len(errors))
        return 1
    if warnings:
        print("OK (with %d warning(s))" % len(warnings))
        return 0
    print("OK")
    return 0


def check_dir(input_dir):
    """Validate a pack source directory without writing. Returns process exit code."""
    entries, errors, warnings = plan_pack(input_dir)
    seq_count = sum(1 for e in entries if not e[0].endswith(".font"))
    font_count = sum(1 for e in entries if e[0].endswith(".font"))
    for msg in warnings:
        print("warn:  %s" % msg)
    for msg in errors:
        sys.stderr.write("error: %s\n" % msg)
    print("check: %s" % input_dir)
    print("  %d sequence(s) and %d font sidecar(s) would be packed, %d warning(s), %d error(s)"
          % (seq_count, font_count, len(warnings), len(errors)))
    if errors:
        print("FAILED")
        return 1
    print("OK")
    return 0


def main():
    ap = argparse.ArgumentParser(
        description="Pack <name>.seq sequences into a deterministic .o2r sequence pack",
        epilog="Examples:\n"
               "  gen_sequence_pack.py seqs/ my-music.o2r --name \"My Music\" --author you\n"
               "  gen_sequence_pack.py seqs/ --check\n"
               "  gen_sequence_pack.py my-music.o2r --check",
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("input", nargs="?", default=None,
                    help="pack source directory (or an .o2r pack when --check)")
    ap.add_argument("output_o2r", nargs="?", default=None,
                    help="output .o2r path (omit with --check)")
    ap.add_argument("--check", action="store_true",
                    help="validate the input directory or an existing .o2r pack; write nothing")
    ap.add_argument("--names", action="store_true",
                    help="list the 23 known sequence names and exit")
    ap.add_argument("--manifest", default=None,
                    help="pack metadata json (default <input>/workshop.json or manifest.json)")
    ap.add_argument("--name", default=None, help="pack name for synthesized metadata")
    ap.add_argument("--author", default=None, help="pack author for synthesized metadata")
    ap.add_argument("--version", default=None, help="pack version for synthesized metadata")
    ap.add_argument("--game-version", default=None, help="target game build (default us.rev0)")
    ap.add_argument("--id", default=None,
                    help="stable pack identifier for synthesized metadata, e.g. author.packname "
                         "(default: output filename stem); commas are not allowed")
    args = ap.parse_args()

    if args.names:
        for i, name in enumerate(SEQ_NAMES):
            print("%2d %s" % (i, name))
        return 0

    if args.input is None:
        sys.stderr.write("error: input is required (or pass --names to list sequence names)\n")
        return 2

    # --check on an existing pack file.
    if args.check and os.path.isfile(args.input) and args.input.lower().endswith(".o2r"):
        return check_pack(args.input)

    # From here on the input is treated as a directory. Keep args.input_dir compatible with helpers.
    args.input_dir = args.input

    if args.check:
        if not os.path.isdir(args.input_dir):
            sys.stderr.write("error: input directory not found: %s\n" % args.input_dir)
            return 2
        return check_dir(args.input_dir)

    if not args.output_o2r:
        sys.stderr.write("error: output_o2r is required (or pass --check to validate only)\n")
        return 2
    if not os.path.isdir(args.input_dir):
        sys.stderr.write("error: input directory not found: %s\n" % args.input_dir)
        return 2

    try:
        manifest_bytes, manifest_source = resolve_manifest_bytes(args)
    except json.JSONDecodeError as exc:
        sys.stderr.write("error: pack metadata is not valid JSON (%s)\n" % exc)
        return 2
    except ValueError as exc:
        sys.stderr.write("error: %s\n" % exc)
        return 2

    entries, errors, warnings = plan_pack(args.input_dir)
    for msg in warnings:
        print("warn:  %s" % msg)
    for msg in errors:
        sys.stderr.write("error: %s\n" % msg)
    if errors:
        sys.stderr.write("error: %d problem(s) must be fixed before packing; nothing written\n"
                         % len(errors))
        return 1
    if not entries:
        sys.stderr.write("error: nothing to pack (every .seq was skipped - see warnings above)\n")
        return 2

    # Metadata is stored as workshop.json: "manifest.json" is libultraship's reserved archive
    # manifest (numeric game_version schema) and our string game_version made LUS throw on mount.
    arc_entries = list(entries)
    arc_entries.append(("workshop.json", manifest_bytes))
    arc_entries.sort(key=lambda e: e[0])

    with zipfile.ZipFile(args.output_o2r, "w", zipfile.ZIP_DEFLATED) as z:
        for arc, data in arc_entries:
            info = zipfile.ZipInfo(arc, date_time=FIXED_DATE)
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o644 << 16
            z.writestr(info, data)

    seq_entries = [e for e in entries if not e[0].endswith(".font")]
    font_entries = [e for e in entries if e[0].endswith(".font")]
    print("wrote %s (%d sequence override(s), %d font sidecar(s), metadata %s)"
          % (args.output_o2r, len(seq_entries), len(font_entries), manifest_source))
    for msg in warnings:
        print("  skipped: %s" % msg)
    return 0


if __name__ == "__main__":
    sys.exit(main())
