#!/usr/bin/env python3
"""G-Diffuser asset binding generator (Slice 4c / R2; R5 profile-parameterized).

Emits port/gen/AssetBindings.c (US, default) — which DEFINES every decomp asset symbol as a
(placeholder) array of its declared C type. This keeps the symbols as arrays — matching Torch's
`extern <ctype> <sym>[];` headers — so:
  * the symbols are defined (no undefined-symbol link errors), and
  * their addresses are compile-time constants, so the game's STATIC tables / display lists
    that reference assets still compile (a runtime pointer can't be a static initializer).

Torch's generated headers (with their _WIDTH/_HEIGHT/_COMPRESSED_SIZE #defines) are used as-is;
no shadow headers are produced.

NOTE: arrays are placeholders ([1]). Filling them with real asset data from the .o2r at the
right sizes is R6 work (needs runtime on a real display). GDiffuser_LoadAllAssets() is the hook.

R2b addition: also emits gdx_lookup_common_asset_rom_offset() — a lookup table that maps each
common_assets_compressed stub address to its absolute ROM byte offset. Used by the PORT path of
func_80077CF0 (object.c) so MIO0 decompression reads from the right place in gdx_rom_buffer.

R5 (C-R5.2) — profile parameterization:
  --profile us/rev0  (default)  -> port/gen/AssetBindings.c    from decomp/assets/yaml/us/rev0
  --profile jp/rev0             -> port/gen/AssetBindings.jp.c from decomp/assets/yaml/jp/rev0
  --out <path>                  -> write to an arbitrary path (used to diff generated-pure output
                                   against the checked-in, hand-edited US file without overwriting it)
The default (us/rev0, no --out) writes to the historical output path and matches the checked-in
file's structure. Both the pure generator output and the checked-in file legitimately omit
placeholder symbols for blob-family recipes (segment_blob.yaml, audio_blob.yaml) -- those entries
only populate segment_blob_entries, never a symbol table -- so this is not a divergence; see the
CRITICAL CAVEAT below for the hand-edit classes the generator does NOT reproduce.

CRITICAL CAVEAT (C-R5.2): the checked-in US AssetBindings.c carries HAND-EDITS the generator does
not reproduce (see the JP file's header block). The JP output is generator-pure and therefore
INCOMPLETE in those same ways — it is EXPERIMENTAL until the hand-edit classes are ported.
"""
import argparse
import glob
import os
import re
import sys
import yaml

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# Known hand-edit classes present in the checked-in US AssetBindings.c that the pure generator does
# NOT reproduce (derived by diffing `gen_asset_bindings.py --profile us/rev0 --out <scratch>` against
# the committed port/gen/AssetBindings.c — see docs/investigation R5 report). Emitted into every
# non-US (experimental) profile header so a human porting the JP build knows exactly what to audit.
US_HANDEDIT_CLASSES = [
    "Stub / segment-image SIZE corrections: several sAssetSegmentMap image_size / sym_size",
    "values were hand-tuned past what the yaml '# size =' comments and next-offset inference",
    "produce (real segment extents measured at runtime); pure sizes may under- or over-cover.",
    "",
    "Fixup SPLITS: some sAssetFixups GFX/VTX byte-swap ranges were manually split or trimmed",
    "so a variable-length display list stops fixing up at the true command boundary rather",
    "than the next-yaml-offset estimate.",
    "",
    "Interior-indexed table symbols with hand-added extern / typedef / NULL-o2r-key overrides",
    "whose raw-ROM copy path must not be replaced by a per-symbol o2r resource.",
    "",
    "Manual edits to GDiffuser_LoadAllAssets / lookup helpers made directly in the checked-in C.",
]

FIXUP_NONE = 0
FIXUP_GFX = 1
FIXUP_VP = 2
FIXUP_VTX = 3

TEXTURE_BYTES_PER_PIXEL = {
    "RGBA16": 2,
    "RGBA32": 4,
    "IA4": 0.5,
    "IA8": 1,
    "IA16": 2,
    "I4": 0.5,
    "I8": 1,
    "CI4": 0.5,
    "CI8": 1,
}

# yaml filenames using the flat BLOB-only recipe mechanism (no :config: segments: block).
# Their entries are NOT placeholder symbols; they only populate segment_blob_entries.
BLOB_RECIPE_FILENAMES = {"segment_blob.yaml", "audio_blob.yaml"}


def asset_declared_size(val):
    typ = val.get("type")
    if typ == "VP":
        return 16
    if typ == "VTX":
        return int(val.get("count", 1)) * 16
    if typ == "BLOB":
        return int(val.get("size", 0))
    if typ == "ARRAY":
        count = int(val.get("count", 0))
        elem = val.get("array_type", val.get("ctype", "u8"))
        elem_size = {
            "s8": 1, "u8": 1,
            "s16": 2, "u16": 2,
            "s32": 4, "u32": 4,
            "s64": 8, "u64": 8,
        }.get(elem, 1)
        return count * elem_size
    if typ in ("TEXTURE", "COMPRESSED_TEXTURE"):
        if "size" in val:
            return int(val["size"])
        width = val.get("width")
        height = val.get("height")
        fmt = val.get("format")
        if width is not None and height is not None and fmt in TEXTURE_BYTES_PER_PIXEL:
            return int(int(width) * int(height) * TEXTURE_BYTES_PER_PIXEL[fmt])
    return 0


def asset_definition_type_and_count(val):
    typ = val.get("type")
    if typ == "ARRAY":
        return val.get("array_type", val.get("ctype", "u8")), max(1, int(val.get("count", 1)))
    return val.get("ctype", "u8"), 1


def fixup_kind(val):
    typ = val.get("type")
    if typ == "GFX":
        return FIXUP_GFX
    if typ == "VP":
        return FIXUP_VP
    if typ == "VTX":
        return FIXUP_VTX
    return FIXUP_NONE


def resolve_paths(profile, out_override):
    """Return (asset_yaml_dir, binding_c_path) for a profile string like 'us/rev0'."""
    parts = profile.split("/")
    asset_yaml_dir = os.path.join(REPO, "decomp", "assets", "yaml", *parts)
    if out_override:
        binding_c = os.path.abspath(out_override)
    elif profile == "us/rev0":
        binding_c = os.path.join(REPO, "port", "gen", "AssetBindings.c")
    else:
        # Profile-suffixed output alongside the US file (e.g. AssetBindings.jp.c). The US file KEEPS
        # its name so the default build sees zero churn (C-R5.2).
        suffix = parts[0]
        binding_c = os.path.join(REPO, "port", "gen", "AssetBindings.{}.c".format(suffix))
    return asset_yaml_dir, binding_c


def jp_experimental_banner(profile):
    """Prominent ASCII header comment for a non-US (experimental) generated file (C-R5.2)."""
    region = profile.split("/")[0].upper()
    lines = []
    lines.append("// ===========================================================================")
    lines.append("// EXPERIMENTAL {} BUILD -- GENERATOR-PURE OUTPUT (C-R5.2).".format(profile.upper()))
    lines.append("//")
    lines.append("// Produced by: tools/gen_asset_bindings.py --profile {}".format(profile))
    lines.append("// Source     : decomp/assets/yaml/{}".format(profile))
    lines.append("//")
    lines.append("// This is a PURE generator output and is therefore INCOMPLETE in the SAME ways")
    lines.append("// the pure US output is: the checked-in US port/gen/AssetBindings.c carries")
    lines.append("// hand-edits the generator cannot reproduce. Before shipping this profile, a")
    lines.append("// human MUST port the following hand-edit classes (derived by diffing the")
    lines.append("// generated-pure US output against the checked-in, hand-edited US file):")
    lines.append("//")
    for sub in US_HANDEDIT_CLASSES:
        lines.append("//" if sub == "" else "//   - {}".format(sub) if not sub[0].islower() else "//     {}".format(sub))
    lines.append("//")
    lines.append("// The {} ROM is NOT on disk in this repo; {} extraction, {} goldens, and {} QA".format(region, region, region, region))
    lines.append("// are OWNER-RUN-REQUIRED. Do NOT treat this file as validated until that lands.")
    lines.append("// ===========================================================================")
    return "\n".join(lines) + "\n"


def is_tracked_binding_path(path):
    """True if `path` resolves to the checked-in, hand-maintained AssetBindings.c.

    Comparison is by realpath so both the historical default resolution AND an
    explicit --out that happens to point at the same file are caught -- either
    way, writing there clobbers hand-maintained real array sizes with generator
    stubs (see US_HANDEDIT_CLASSES).
    """
    tracked = os.path.join(REPO, "port", "gen", "AssetBindings.c")
    return os.path.realpath(path) == os.path.realpath(tracked)


def generate(profile, out_override, force_overwrite=False, lint_only=False):
    asset_yaml_dir, binding_c = resolve_paths(profile, out_override)

    if not lint_only and is_tracked_binding_path(binding_c) and not force_overwrite:
        print(
            "ERROR: refusing to overwrite the tracked, hand-maintained {}\n"
            "\n"
            "This file is generator output PLUS hand-maintained real array sizes on\n"
            "top (see the US_HANDEDIT_CLASSES doc in this module) -- a bare regenerate\n"
            "silently truncates every array back to a 1-element stub, which still\n"
            "compiles but corrupts every asset it touches.\n"
            "\n"
            "Safe options:\n"
            "  --lint-only            run the duplicate-offset lint, write nothing\n"
            "  --out <scratch-path>   generate to a scratch file to diff against the\n"
            "                         tracked file before deciding what to apply\n"
            "  --force-overwrite      regenerate the tracked file for real -- you MUST\n"
            "                         re-apply hand-maintained array sizes afterward\n".format(binding_c),
            file=sys.stderr,
        )
        sys.exit(1)

    if not lint_only:
        os.makedirs(os.path.dirname(binding_c), exist_ok=True)

    defs = []
    total = 0

    # Track common_assets_compressed entries for the ROM offset table and O2R key table.
    # Each entry: (symbol_name, absolute_rom_offset, o2r_key)
    common_asset_rom_entries = []

    # Track segment_blob.yaml (R1) and audio_blob.yaml (R2) families. Each entry is a verbatim
    # ROM-slice blob delivered to the port's byte-source shim via containment lookup, NOT a
    # placeholder symbol. Both yamls share the same flat no-`:config: segments:` recipe mechanism
    # and feed the SAME generated containment-lookup table (sSegmentBlobMap / gdx_lookup_segment_blob;
    # C-R2.1 "entries join the SAME generated table").
    # Each entry: (rom_base, size, o2r_key)
    segment_blob_entries = []

    # Track real ROM-backed assets for the display-list bridge. The decomp build
    # keeps asset symbols as one-byte placeholders, so runtime GBI commands contain
    # truncated placeholder addresses. This map lets the port resolve those addresses
    # back into loaded ROM segment images.
    asset_segment_entries = []
    asset_range_entries = []
    asset_fixup_entries = []
    segment_images = {}
    asset_load_entries = []

    # Lint-only (non-fatal): flag two DIFFERENTLY-NAMED symbols that claim the same
    # (offset) within the same image (segment_id, rom_base, compressed). This class of
    # bug is exactly what caused the LINE menu strip to read MARK emblem art on the US
    # cart (EK reconciliation Phase 3/F3): a stale/renamed symbol's yaml `offset:` was
    # never updated and silently landed on another symbol's slot. Keyed per-image
    # because the same numeric offset legitimately repeats across different images
    # (segments/ROM bases). Two entries for the SAME symbol name at the same offset
    # (e.g. re-processed duplicate yaml keys) are not warned about here.
    seen_image_offsets = {}

    for path in sorted(glob.glob(os.path.join(asset_yaml_dir, "*.yaml"))):
        fname = os.path.basename(path)
        yaml_stem = os.path.splitext(fname)[0]
        is_common = (fname == "common_assets_compressed.yaml")
        is_blob_recipe = (fname in BLOB_RECIPE_FILENAMES)

        with open(path) as f:
            yaml_text = f.read()
        data = yaml.safe_load(yaml_text) or {}

        # segment_blob.yaml (R1) / audio_blob.yaml (R2): raw ROM-slice blobs consumed by the port
        # byte-source shim via a containment lookup. These are NOT placeholder symbols, so emit no
        # `defs` array and skip the segment-image / common-asset machinery entirely; just record
        # each blob's ROM span into the shared sSegmentBlobMap table.
        if is_blob_recipe:
            for key, val in data.items():
                if not isinstance(val, dict) or str(key).startswith(":"):
                    continue
                if val.get("type") != "BLOB":
                    continue
                rom_base = int(val.get("offset"))
                size = int(val.get("size", 0))
                o2r_key = "{}/{}".format(yaml_stem, key)
                segment_blob_entries.append((rom_base, size, o2r_key))
            continue

        # Some segment YAMLs record the decoded image size in a trailing comment.
        # Keep it so the final variable-length GFX entry has an upper boundary even
        # though there is no following asset offset from which to infer its size.
        size_matches = re.findall(r"(?m)^\s*#\s*size\s*=\s*(0x[0-9A-Fa-f]+|\d+)", yaml_text)
        segment_declared_size = int(size_matches[-1], 0) if size_matches else 0

        # Extract ROM base offset and segment id from :config: segments.
        rom_base = None
        segment_id = None
        config = data.get(":config", {}) or {}
        segs = config.get("segments") or []
        if segs and isinstance(segs[0], (list, tuple)) and len(segs[0]) >= 2:
            segment_id = segs[0][0]
            rom_base = segs[0][1]  # e.g. 0x2B9EA0

        compressed = bool((config.get("compression") or {}).get("offset") is not None)

        # YAML table declarations describe contiguous symbol ranges whose base
        # names are used directly by game code for pointer arithmetic. They are
        # not regular asset entries, so preserve their placeholder symbol as a
        # token and teach the runtime resolver how offsets from that token map
        # into the real segment image.
        tables = config.get("tables") or {}
        if (not is_common) and segment_id is not None and rom_base is not None:
            for table_name, table_val in tables.items():
                if not isinstance(table_val, dict):
                    continue
                table_range = table_val.get("range")
                if not isinstance(table_range, (list, tuple)) or len(table_range) < 2:
                    continue
                range_start = int(table_range[0])
                range_end = int(table_range[1])
                if range_end <= range_start:
                    continue
                range_segment = (range_start >> 24) & 0xFF
                if range_segment != int(segment_id):
                    continue
                asset_range_entries.append((
                    table_name,
                    int(segment_id),
                    int(rom_base),
                    int(compressed),
                    range_start & 0x00FFFFFF,
                    range_end - range_start,
                    yaml_stem,
                ))

        # GFX entries are variable-length display lists. The next YAML offset is
        # the best source of truth for how many command bytes to endian-fix.
        asset_items = []
        for item_key, item_val in data.items():
            if isinstance(item_val, dict) and not str(item_key).startswith(":") and item_val.get("offset") is not None:
                asset_items.append((int(item_val.get("offset")), item_key, item_val))
        asset_items.sort(key=lambda item: item[0])
        next_offsets = {}
        for idx, (offset, item_key, _item_val) in enumerate(asset_items):
            if idx + 1 < len(asset_items):
                next_offsets[item_key] = asset_items[idx + 1][0]

        for key, val in data.items():
            if not isinstance(val, dict) or str(key).startswith(":"):
                continue
            # Flat BLOB recipes outside BLOB_RECIPE_FILENAMES (e.g. rsp_blob.yaml) are loaded
            # by archive key at runtime; their entries need no placeholder symbol.
            if rom_base is None and val.get("type") == "BLOB":
                continue
            sym = val.get("symbol", key)
            ctype, count = asset_definition_type_and_count(val)
            defs.append("{} {}[{}];".format(ctype, sym, count))
            total += 1

            if is_common and rom_base is not None:
                offset = val.get("offset")
                if offset is not None:
                    o2r_key = "common_assets_compressed/{}".format(sym)
                    common_asset_rom_entries.append((sym, rom_base + offset, o2r_key))
                    if val.get("type") == "ARRAY":
                        asset_load_entries.append((sym, o2r_key))

            # common_assets_compressed is a packed set of individually-compressed
            # payloads used by object.c; it is not a normal segment image.
            if (not is_common) and (segment_id is not None) and (rom_base is not None):
                offset = val.get("offset")
                if offset is not None:
                    offset = int(offset)
                    declared = asset_declared_size(val)
                    if val.get("type") == "GFX":
                        gfx_end = next_offsets.get(key)
                        if gfx_end is None and segment_declared_size > offset:
                            gfx_end = segment_declared_size
                        declared = max(0, int(gfx_end if gfx_end is not None else offset) - offset)

                    image_key = (int(segment_id), int(rom_base), int(compressed))

                    offset_key = (image_key, offset)
                    prior_sym = seen_image_offsets.get(offset_key)
                    if prior_sym is not None and prior_sym != sym:
                        print(
                            "WARNING: duplicate-offset in {}: {} and {} both claim offset "
                            "0x{:X} in image (segment=0x{:X}, rom_base=0x{:X}, compressed={}) "
                            "-- one of them is likely reading the other's data".format(
                                yaml_stem, prior_sym, sym, offset, image_key[0], image_key[1], image_key[2]
                            ),
                            file=sys.stderr,
                        )
                    else:
                        seen_image_offsets[offset_key] = sym

                    image_size = segment_images.get(image_key, 0)
                    if segment_declared_size > 0:
                        image_size = max(image_size, segment_declared_size)
                    if declared > 0:
                        image_size = max(image_size, offset + declared)
                    elif key in next_offsets:
                        image_size = max(image_size, int(next_offsets[key]))
                    else:
                        image_size = max(image_size, offset)
                    segment_images[image_key] = image_size

                    asset_segment_entries.append((sym, int(segment_id), int(rom_base), int(compressed), offset, image_key, yaml_stem))

                    kind = fixup_kind(val)
                    if kind != FIXUP_NONE and declared > 0:
                        asset_fixup_entries.append((int(segment_id), int(rom_base), offset, declared, kind))

    # Build the ROM offset lookup table (PORT only).
    lookup_lines = []
    lookup_lines.append("")
    lookup_lines.append("#ifdef PORT")
    lookup_lines.append("/* PORT: maps each common_assets_compressed stub address to its")
    lookup_lines.append(" * absolute ROM byte offset so func_80077CF0 can read from gdx_rom_buffer. */")
    lookup_lines.append("typedef struct { void* sym; unsigned int rom_offset; const char* o2r_key; } GdxCommonAssetEntry;")
    lookup_lines.append("static const GdxCommonAssetEntry sCommonAssetRomMap[] = {")
    for sym, rom_offset, o2r_key in common_asset_rom_entries:
        lookup_lines.append("    {{ {}, 0x{:08X}U, \"{}\" }},".format(sym, rom_offset, o2r_key))
    lookup_lines.append("    { NULL, 0U, NULL }")
    lookup_lines.append("};")
    lookup_lines.append("")
    lookup_lines.append("unsigned int gdx_lookup_common_asset_rom_offset(unsigned long long sym_addr) {")
    lookup_lines.append("    int i;")
    lookup_lines.append("    for (i = 0; sCommonAssetRomMap[i].sym != NULL; i++) {")
    lookup_lines.append("        if ((unsigned long long)sCommonAssetRomMap[i].sym == sym_addr)")
    lookup_lines.append("            return sCommonAssetRomMap[i].rom_offset;")
    lookup_lines.append("    }")
    lookup_lines.append("    return 0U;")
    lookup_lines.append("}")
    lookup_lines.append("")
    lookup_lines.append("const char* gdx_lookup_common_asset_o2r_key(unsigned long long sym_addr) {")
    lookup_lines.append("    int i;")
    lookup_lines.append("    for (i = 0; sCommonAssetRomMap[i].sym != NULL; i++) {")
    lookup_lines.append("        if ((unsigned long long)sCommonAssetRomMap[i].sym == sym_addr)")
    lookup_lines.append("            return sCommonAssetRomMap[i].o2r_key;")
    lookup_lines.append("    }")
    lookup_lines.append("    return NULL;")
    lookup_lines.append("}")
    lookup_lines.append("#endif /* PORT */")

    asset_lines = []
    asset_lines.append("")
    asset_lines.append("#ifdef PORT")
    asset_lines.append("/* PORT: maps generated one-byte asset placeholder symbols to their")
    asset_lines.append(" * real ROM-backed segment image and offset for the GBI bridge. */")
    asset_lines.append("typedef struct { void* sym; unsigned char segment; unsigned int rom_base; unsigned char compressed; unsigned int offset; unsigned int image_size; unsigned int sym_size; const char* o2r_key; } GdxAssetSegmentEntry;")
    asset_lines.append("static const GdxAssetSegmentEntry sAssetSegmentMap[] = {")
    for sym, segment, rom_base, compressed, offset, image_key, yaml_stem in asset_segment_entries:
        image_size = segment_images.get(image_key, 0)
        if image_size <= 0:
            continue
        o2r_key = "{}/{}".format(yaml_stem, sym)
        asset_lines.append("    {{ {}, 0x{:02X}u, 0x{:08X}U, {}u, 0x{:08X}U, 0x{:08X}U, (unsigned int)sizeof({}), \"{}\" }},".format(
            sym, segment, rom_base, compressed, offset, image_size, sym, o2r_key))
    # YAML table symbols (e.g. aPositionDigitTexs) also get segment rows so exact-base
    # AND interior references resolve through gdx_lookup_asset_segment(_interior).
    # The size is hardcoded from the yaml range (the symbol is only extern'd here;
    # its storage lives in LinkStubs.c, which must define it at this same real size —
    # a 1-byte definition makes the range window swallow every neighboring stub).
    # NULL o2r key: game code indexes these tables at interior offsets, so all
    # references must take the raw ROM-segment copy path, never a per-symbol o2r
    # resource that would only cover the first entry.
    for sym, segment, rom_base, compressed, offset, size, yaml_stem in asset_range_entries:
        image_key = (segment, rom_base, compressed)
        image_size = segment_images.get(image_key, 0)
        if image_size <= 0:
            continue
        asset_lines.append("    {{ {}, 0x{:02X}u, 0x{:08X}U, {}u, 0x{:08X}U, 0x{:08X}U, 0x{:X}u, NULL }},".format(
            sym, segment, rom_base, compressed, offset, image_size, size))
    asset_lines.append("    { NULL, 0u, 0U, 0u, 0U, 0U, 0U, NULL }")
    asset_lines.append("};")
    asset_lines.append("")
    asset_lines.append("typedef struct { void* sym; unsigned char segment; unsigned int rom_base; unsigned char compressed; unsigned int offset; unsigned int size; unsigned int image_size; const char* o2r_key; } GdxAssetRangeEntry;")
    asset_lines.append("static const GdxAssetRangeEntry sAssetRangeMap[] = {")
    for sym, segment, rom_base, compressed, offset, size, yaml_stem in asset_range_entries:
        image_key = (segment, rom_base, compressed)
        image_size = segment_images.get(image_key, 0)
        if image_size <= 0:
            continue
        o2r_key = "{}/{}".format(yaml_stem, sym)
        asset_lines.append("    {{ {}, 0x{:02X}u, 0x{:08X}U, {}u, 0x{:08X}U, 0x{:08X}U, 0x{:08X}U, \"{}\" }},".format(
            sym, segment, rom_base, compressed, offset, size, image_size, o2r_key))
    asset_lines.append("    { NULL, 0u, 0U, 0u, 0U, 0U, 0U, NULL }")
    asset_lines.append("};")
    asset_lines.append("")
    asset_lines.append("/* Symbol lookups run for every translated pointer of every display-list")
    asset_lines.append(" * command each frame. A linear scan of the segment map dominates frame")
    asset_lines.append(" * time, so both lookups binary-search a lazily built sorted index. */")
    asset_lines.append("typedef struct { unsigned int low32; int idx; } GdxAssetIndexEntry;")
    asset_lines.append("static GdxAssetIndexEntry sAssetSegmentIndex[sizeof(sAssetSegmentMap) / sizeof(sAssetSegmentMap[0])];")
    asset_lines.append("static int sAssetSegmentIndexCount = 0;")
    asset_lines.append("static int sAssetSegmentIndexBuilt = 0;")
    asset_lines.append("")
    asset_lines.append("static void gdx_build_asset_index(void) {")
    asset_lines.append("    int n, gap, i, j;")
    asset_lines.append("    if (sAssetSegmentIndexBuilt) return;")
    asset_lines.append("    for (n = 0; sAssetSegmentMap[n].sym != NULL; n++) {")
    asset_lines.append("        sAssetSegmentIndex[n].low32 = (unsigned int)(unsigned long long)sAssetSegmentMap[n].sym;")
    asset_lines.append("        sAssetSegmentIndex[n].idx = n;")
    asset_lines.append("    }")
    asset_lines.append("    for (gap = n / 2; gap > 0; gap /= 2) {")
    asset_lines.append("        for (i = gap; i < n; i++) {")
    asset_lines.append("            GdxAssetIndexEntry t = sAssetSegmentIndex[i];")
    asset_lines.append("            for (j = i; j >= gap && sAssetSegmentIndex[j - gap].low32 > t.low32; j -= gap) {")
    asset_lines.append("                sAssetSegmentIndex[j] = sAssetSegmentIndex[j - gap];")
    asset_lines.append("            }")
    asset_lines.append("            sAssetSegmentIndex[j] = t;")
    asset_lines.append("        }")
    asset_lines.append("    }")
    asset_lines.append("    sAssetSegmentIndexCount = n;")
    asset_lines.append("    sAssetSegmentIndexBuilt = 1;")
    asset_lines.append("}")
    asset_lines.append("")
    asset_lines.append("/* Greatest index entry with low32 <= key, or -1. */")
    asset_lines.append("static int gdx_asset_index_floor(unsigned int key) {")
    asset_lines.append("    int lo = 0, hi, best = -1;")
    asset_lines.append("    gdx_build_asset_index();")
    asset_lines.append("    hi = sAssetSegmentIndexCount - 1;")
    asset_lines.append("    while (lo <= hi) {")
    asset_lines.append("        int mid = lo + (hi - lo) / 2;")
    asset_lines.append("        if (sAssetSegmentIndex[mid].low32 <= key) { best = mid; lo = mid + 1; }")
    asset_lines.append("        else { hi = mid - 1; }")
    asset_lines.append("    }")
    asset_lines.append("    return best;")
    asset_lines.append("}")
    asset_lines.append("")
    asset_lines.append("int gdx_lookup_asset_segment(unsigned int sym_low32, unsigned char* segment, unsigned int* rom_base,")
    asset_lines.append("                             unsigned char* compressed, unsigned int* offset, unsigned int* image_size) {")
    asset_lines.append("    int i;")
    asset_lines.append("    int f = gdx_asset_index_floor(sym_low32);")
    asset_lines.append("    if (f >= 0 && sAssetSegmentIndex[f].low32 == sym_low32) {")
    asset_lines.append("        i = sAssetSegmentIndex[f].idx;")
    asset_lines.append("        if (segment != NULL) *segment = sAssetSegmentMap[i].segment;")
    asset_lines.append("        if (rom_base != NULL) *rom_base = sAssetSegmentMap[i].rom_base;")
    asset_lines.append("        if (compressed != NULL) *compressed = sAssetSegmentMap[i].compressed;")
    asset_lines.append("        if (offset != NULL) *offset = sAssetSegmentMap[i].offset;")
    asset_lines.append("        if (image_size != NULL) *image_size = sAssetSegmentMap[i].image_size;")
    asset_lines.append("        return 1;")
    asset_lines.append("    }")
    asset_lines.append("    for (i = 0; sAssetRangeMap[i].sym != NULL; i++) {")
    asset_lines.append("        unsigned int base = (unsigned int)(unsigned long long)sAssetRangeMap[i].sym;")
    asset_lines.append("        unsigned int delta = sym_low32 - base;")
    asset_lines.append("        if (delta < sAssetRangeMap[i].size) {")
    asset_lines.append("            if (segment != NULL) *segment = sAssetRangeMap[i].segment;")
    asset_lines.append("            if (rom_base != NULL) *rom_base = sAssetRangeMap[i].rom_base;")
    asset_lines.append("            if (compressed != NULL) *compressed = sAssetRangeMap[i].compressed;")
    asset_lines.append("            if (offset != NULL) *offset = sAssetRangeMap[i].offset + delta;")
    asset_lines.append("            if (image_size != NULL) *image_size = sAssetRangeMap[i].image_size;")
    asset_lines.append("            return 1;")
    asset_lines.append("        }")
    asset_lines.append("    }")
    asset_lines.append("    return 0;")
    asset_lines.append("}")
    asset_lines.append("")
    asset_lines.append("/* Interior-pointer resolution: game DLs reference vertex data at symbol+offset")
    asset_lines.append(" * (e.g. gSPVertex(&D_3000C98[64], ...)). Exact matching misses those, and the")
    asset_lines.append(" * pointer would otherwise resolve into the zero-filled placeholder BSS array,")
    asset_lines.append(" * producing origin-vertex spike polygons. Match within each symbol's byte size. */")
    asset_lines.append("int gdx_lookup_asset_segment_interior(unsigned int sym_low32, unsigned char* segment, unsigned int* rom_base,")
    asset_lines.append("                                      unsigned char* compressed, unsigned int* offset, unsigned int* image_size) {")
    asset_lines.append("    int i;")
    asset_lines.append("    unsigned int base, delta;")
    asset_lines.append("    int f = gdx_asset_index_floor(sym_low32);")
    asset_lines.append("    if (f < 0) return 0;")
    asset_lines.append("    /* Placeholder arrays are distinct linker objects, so address ranges never")
    asset_lines.append("       overlap: only the greatest base at or below the pointer can contain it. */")
    asset_lines.append("    i = sAssetSegmentIndex[f].idx;")
    asset_lines.append("    base = sAssetSegmentIndex[f].low32;")
    asset_lines.append("    delta = sym_low32 - base;")
    asset_lines.append("    if (delta != 0u && delta < sAssetSegmentMap[i].sym_size) {")
    asset_lines.append("        if (segment != NULL) *segment = sAssetSegmentMap[i].segment;")
    asset_lines.append("        if (rom_base != NULL) *rom_base = sAssetSegmentMap[i].rom_base;")
    asset_lines.append("        if (compressed != NULL) *compressed = sAssetSegmentMap[i].compressed;")
    asset_lines.append("        if (offset != NULL) *offset = sAssetSegmentMap[i].offset + delta;")
    asset_lines.append("        if (image_size != NULL) *image_size = sAssetSegmentMap[i].image_size;")
    asset_lines.append("        return 1;")
    asset_lines.append("    }")
    asset_lines.append("    return 0;")
    asset_lines.append("}")
    asset_lines.append("")
    asset_lines.append("const char* gdx_lookup_asset_segment_o2r_key(unsigned int sym_low32) {")
    asset_lines.append("    int i;")
    asset_lines.append("    for (i = 0; sAssetSegmentMap[i].sym != NULL; i++) {")
    asset_lines.append("        if ((unsigned int)(unsigned long long)sAssetSegmentMap[i].sym == sym_low32)")
    asset_lines.append("            return sAssetSegmentMap[i].o2r_key;")
    asset_lines.append("    }")
    asset_lines.append("    return NULL;")
    asset_lines.append("}")
    asset_lines.append("")
    asset_lines.append("const char* gdx_find_o2r_key_by_abs_rom_offset(unsigned int abs_rom_offset) {")
    asset_lines.append("    int i;")
    asset_lines.append("    for (i = 0; sAssetSegmentMap[i].sym != NULL; i++) {")
    asset_lines.append("        if (sAssetSegmentMap[i].compressed == 0u &&")
    asset_lines.append("            sAssetSegmentMap[i].rom_base + sAssetSegmentMap[i].offset == abs_rom_offset)")
    asset_lines.append("            return sAssetSegmentMap[i].o2r_key;")
    asset_lines.append("    }")
    asset_lines.append("    return NULL;")
    asset_lines.append("}")
    asset_lines.append("")
    asset_lines.append("/* Every symbol whose image starts at `offset` inside segment `segment`, from index `start` on.")
    asset_lines.append(" * Returns the index to resume from (> 0) and fills *sym_low32, or 0 when there are no more. A raw")
    asset_lines.append(" * segmented SETTIMG from a compiled DL (aCountdownSignDL, aFinishDL) names only segment + offset;")
    asset_lines.append(" * this recovers the symbols so texture packs can key it. */")
    asset_lines.append("int gdx_find_asset_symbol_at_segment_offset(unsigned int segment, unsigned int offset, int start,")
    asset_lines.append("                                            unsigned int* sym_low32) {")
    asset_lines.append("    int i;")
    asset_lines.append("    for (i = start < 0 ? 0 : start; sAssetSegmentMap[i].sym != NULL; i++) {")
    asset_lines.append("        if (sAssetSegmentMap[i].segment == segment && sAssetSegmentMap[i].offset == offset) {")
    asset_lines.append("            if (sym_low32 != NULL) *sym_low32 = (unsigned int)(unsigned long long)sAssetSegmentMap[i].sym;")
    asset_lines.append("            return i + 1;")
    asset_lines.append("        }")
    asset_lines.append("    }")
    asset_lines.append("    return 0;")
    asset_lines.append("}")
    asset_lines.append("")
    asset_lines.append("typedef struct { unsigned char segment; unsigned int rom_base; unsigned int offset; unsigned int size; unsigned char kind; } GdxAssetFixupEntry;")
    asset_lines.append("static const GdxAssetFixupEntry sAssetFixups[] = {")
    for segment, rom_base, offset, size, kind in asset_fixup_entries:
        asset_lines.append("    {{ 0x{:02X}u, 0x{:08X}U, 0x{:08X}U, 0x{:08X}U, {}u }},".format(
            segment, rom_base, offset, size, kind))
    asset_lines.append("    { 0u, 0U, 0U, 0U, 0u }")
    asset_lines.append("};")
    asset_lines.append("")
    asset_lines.append("static unsigned short gdx_bswap16(unsigned short v) {")
    asset_lines.append("    return (unsigned short)((v << 8) | (v >> 8));")
    asset_lines.append("}")
    asset_lines.append("")
    asset_lines.append("static unsigned int gdx_bswap32(unsigned int v) {")
    asset_lines.append("    return ((v & 0x000000FFU) << 24) | ((v & 0x0000FF00U) << 8) |")
    asset_lines.append("           ((v & 0x00FF0000U) >> 8) | ((v & 0xFF000000U) >> 24);")
    asset_lines.append("}")
    asset_lines.append("")
    asset_lines.append("void gdx_fixup_asset_segment_image(unsigned char segment, unsigned int rom_base, unsigned char* data, unsigned int size) {")
    asset_lines.append("    int i;")
    asset_lines.append("    for (i = 0; sAssetFixups[i].kind != 0u; i++) {")
    asset_lines.append("        unsigned int j;")
    asset_lines.append("        unsigned int off = sAssetFixups[i].offset;")
    asset_lines.append("        unsigned int bytes = sAssetFixups[i].size;")
    asset_lines.append("        if (sAssetFixups[i].segment != segment || sAssetFixups[i].rom_base != rom_base) continue;")
    asset_lines.append("        if (off >= size || bytes > size - off) continue;")
    asset_lines.append("        if (sAssetFixups[i].kind == 1u) {")
    asset_lines.append("            for (j = 0; j + 4 <= bytes; j += 4) {")
    asset_lines.append("                unsigned int* p = (unsigned int*)(void*)(data + off + j);")
    asset_lines.append("                *p = gdx_bswap32(*p);")
    asset_lines.append("            }")
    asset_lines.append("        } else if (sAssetFixups[i].kind == 2u) {")
    asset_lines.append("            for (j = 0; j + 2 <= bytes; j += 2) {")
    asset_lines.append("                unsigned short* p = (unsigned short*)(void*)(data + off + j);")
    asset_lines.append("                *p = gdx_bswap16(*p);")
    asset_lines.append("            }")
    asset_lines.append("        } else if (sAssetFixups[i].kind == 3u) {")
    asset_lines.append("            for (j = 0; j + 16 <= bytes; j += 16) {")
    asset_lines.append("                unsigned int k;")
    asset_lines.append("                for (k = 0; k < 12; k += 2) {")
    asset_lines.append("                    unsigned short* p = (unsigned short*)(void*)(data + off + j + k);")
    asset_lines.append("                    *p = gdx_bswap16(*p);")
    asset_lines.append("                }")
    asset_lines.append("            }")
    asset_lines.append("        }")
    asset_lines.append("    }")
    asset_lines.append("}")
    asset_lines.append("")
    asset_lines.append("void gdx_register_asset_segment_command_ranges(unsigned char segment, unsigned int rom_base, unsigned char* data, unsigned int size) {")
    asset_lines.append("    int i;")
    asset_lines.append("    extern void gdx_register_host_n64_command_range(void* ptr, size_t size);")
    asset_lines.append("    for (i = 0; sAssetFixups[i].kind != 0u; i++) {")
    asset_lines.append("        unsigned int off = sAssetFixups[i].offset;")
    asset_lines.append("        unsigned int bytes = sAssetFixups[i].size;")
    asset_lines.append("        if (sAssetFixups[i].kind != 1u) continue;")
    asset_lines.append("        if (sAssetFixups[i].segment != segment || sAssetFixups[i].rom_base != rom_base) continue;")
    asset_lines.append("        if (off >= size || bytes > size - off) continue;")
    asset_lines.append("        gdx_register_host_n64_command_range(data + off, bytes);")
    asset_lines.append("    }")
    asset_lines.append("}")
    asset_lines.append("#endif /* PORT */")

    # R1/R2 shared segment-blob table + containment lookup. These map an absolute ROM read range to
    # the verbatim ROM-slice o2r entry (segment_blob/<family> from R1, audio_blob/<family> from R2 —
    # C-R2.1: "entries join the SAME generated table") so the port byte-source shim can serve any read
    # that falls inside a family span from the archive instead of the raw ROM buffer.
    blob_lines = []
    blob_lines.append("")
    blob_lines.append("#ifdef PORT")
    blob_lines.append("/* PORT (R1/R2): maps an absolute ROM read range to its verbatim segment_blob or")
    blob_lines.append(" * audio_blob o2r entry. Consumers (port/gdx_segment_source.c) resolve a read at")
    blob_lines.append(" * [rom_base, rom_base+size) to the blob whose span fully CONTAINS it (containment")
    blob_lines.append(" * lookup, contract C-R1.3), one shared table for both namespaces (C-R2.1).")
    blob_lines.append(" * Sorted by rom_base for a deterministic table; the lookup below is a linear scan")
    blob_lines.append(" * (not a binary search) -- sorting is for determinism/readability only. */")
    blob_lines.append("typedef struct { unsigned int rom_base; unsigned int size; const char* o2r_key; } GdxSegmentBlobEntry;")
    blob_lines.append("static const GdxSegmentBlobEntry sSegmentBlobMap[] = {")
    for rom_base, size, o2r_key in sorted(segment_blob_entries):
        blob_lines.append("    {{ 0x{:08X}U, 0x{:08X}U, \"{}\" }},".format(rom_base, size, o2r_key))
    blob_lines.append("    { 0U, 0U, NULL }")
    blob_lines.append("};")
    blob_lines.append("")
    blob_lines.append("/* Returns the blob whose [rom_base, rom_base+size) fully contains")
    blob_lines.append(" * [query_rom_base, query_rom_base+size_needed), or NULL. A read anywhere inside")
    blob_lines.append(" * a blob span resolves to that blob (C-R1.3). size_needed==0 always returns NULL")
    blob_lines.append(" * (a zero-length read at a shared span boundary is otherwise ambiguous). */")
    blob_lines.append("const GdxSegmentBlobEntry* gdx_lookup_segment_blob(unsigned int rom_base, unsigned int size_needed) {")
    blob_lines.append("    int i;")
    blob_lines.append("    if (size_needed == 0u) {")
    blob_lines.append("        return NULL; /* closes the shared-boundary tie-break ambiguity */")
    blob_lines.append("    }")
    blob_lines.append("    for (i = 0; sSegmentBlobMap[i].o2r_key != NULL; i++) {")
    blob_lines.append("        unsigned int base = sSegmentBlobMap[i].rom_base;")
    blob_lines.append("        unsigned int end = base + sSegmentBlobMap[i].size;")
    blob_lines.append("        if (rom_base >= base && rom_base <= end &&")
    blob_lines.append("            size_needed <= end - rom_base) {")
    blob_lines.append("            return &sSegmentBlobMap[i];")
    blob_lines.append("        }")
    blob_lines.append("    }")
    blob_lines.append("    return NULL;")
    blob_lines.append("}")
    blob_lines.append("#endif /* PORT */")

    is_us_default = (profile == "us/rev0")
    if lint_only:
        print("LINT-ONLY [{}]: {} asset symbols scanned, nothing written (target would have been {})".format(
            profile, total, binding_c))
        return

    with open(binding_c, "w", encoding="utf-8", newline="\n") as f:
        if not is_us_default:
            f.write(jp_experimental_banner(profile))
        f.write("// AUTO-GENERATED by tools/gen_asset_bindings.py (R2). Do not edit by hand.\n")
        f.write("// Placeholder array definitions for every decomp asset symbol (kept as arrays so\n")
        f.write("// static asset references compile). Real size + .o2r data load = R6.\n")
        f.write('#include "global.h"\n')
        f.write('\n')
        # YAML table ranges are addressable placeholder tokens but are not regular
        # asset entries, so their declarations are not present in Torch headers.
        # Emit them here before sAssetRangeMap takes their addresses.
        for sym in sorted({entry[0] for entry in asset_range_entries}):
            f.write("extern unsigned char {}[];\n".format(sym))
        if asset_range_entries:
            f.write('\n')
        f.write("\n".join(defs))
        f.write("\n\nvoid GDiffuser_LoadAllAssets(void) {\n")
        f.write("#ifdef PORT\n")
        f.write("    size_t copiedSize;\n")
        f.write("    extern int GDiffuser_LoadAssetBytes(const char* key, void* out, size_t outSize, size_t* copiedSize);\n")
        for sym, o2r_key in asset_load_entries:
            f.write("    copiedSize = 0;\n")
            f.write("    (void)GDiffuser_LoadAssetBytes(\"{}\", {}, sizeof({}), &copiedSize);\n".format(o2r_key, sym, sym))
        f.write("#endif\n")
        f.write("}\n")
        f.write("\n".join(lookup_lines))
        f.write("\n".join(asset_lines))
        f.write("\n".join(blob_lines))
        f.write("\n")

    print("R2 [{}]: defined {} asset symbols -> {}".format(profile, total, binding_c))
    print("R1/R2 [{}]: {} blob families (segment_blob + audio_blob) -> gdx_lookup_segment_blob".format(profile, len(segment_blob_entries)))
    print("R2b [{}]: {} common_assets_compressed ROM offset entries".format(profile, len(common_asset_rom_entries)))
    print("R6 [{}]: {} ROM-backed asset segment entries, {} fixups".format(profile, len(asset_segment_entries), len(asset_fixup_entries)))


def main():
    ap = argparse.ArgumentParser(description="Generate port/gen/AssetBindings[.<profile>].c")
    ap.add_argument("--profile", default="us/rev0", choices=["us/rev0", "jp/rev0"],
                    help="asset recipe profile (default: us/rev0, writes to the historical output "
                         "path and matches the checked-in file's structure; see the CRITICAL CAVEAT "
                         "in the module docstring for the separate hand-edit classes the pure "
                         "generator output does not reproduce)")
    ap.add_argument("--out", default=None,
                    help="override output path (e.g. a scratch path to diff generated-pure vs "
                         "the checked-in hand-edited US file without overwriting it)")
    ap.add_argument("--force-overwrite", action="store_true",
                    help="allow writing over the tracked, hand-maintained port/gen/AssetBindings.c. "
                         "Without this, writing to that path is refused because it silently truncates "
                         "every hand-maintained array size back to a 1-element generator stub. You MUST "
                         "re-apply the hand-edited sizes afterward if you use this.")
    ap.add_argument("--lint-only", action="store_true",
                    help="run the duplicate-offset lint (and the rest of the yaml scan) without "
                         "writing any file -- safe to run at any time, including against the tracked "
                         "US output path")
    args = ap.parse_args()
    generate(args.profile, args.out, force_overwrite=args.force_overwrite, lint_only=args.lint_only)


if __name__ == "__main__":
    main()
