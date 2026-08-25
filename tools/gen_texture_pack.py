#!/usr/bin/env python3
"""Pack a directory of <key>.png replacement textures into a deterministic .o2r texture pack.

Quick start (sensible defaults — this is all most modders need):

    python tools/gen_texture_pack.py dump/ my-pack.o2r --name "My Pack" --author you

    # validate a dump folder or an existing pack without writing anything:
    python tools/gen_texture_pack.py dump/ --check
    python tools/gen_texture_pack.py my-pack.o2r --check

Input (mirrors the Workshop dump output):
  <input_dir>/
    <key>.png            one PNG per texture, nested dirs allowed (keys contain '/')
    manifest.tsv         the dump manifest: TAB-separated  key<TAB>nativeW<TAB>nativeH<TAB>fmt
                         (native dimensions + the N64 format the game loads each texture as),
                         plus an optional 5th palette_key column on CI4/CI8 rows naming the dumped
                         TLUT swatch (<palette_key>.png, an Nx1 RGBA strip of the original palette).
                         Produced automatically by the in-game "Dump textures while playing" tool.
    workshop.json        (optional) pack metadata: {name, version, author, game_version,
                         key_scheme_version, id, depends, conflicts}. If absent, one is
                         synthesized from the --name / --author / --version / --id flags (this
                         packer never errors just because the metadata file is missing).

Output: one .o2r (a ZIP, like tools/gen_f3d_o2r.py) where each PNG becomes an OTEX-V1 resource at
archive path "textures/pack/<key>", plus the metadata at the archive root as workshop.json. The
metadata carries a "files" integrity manifest — {path, sha256} over the exact bytes archived for
every payload entry — which --check re-verifies.
With --direct-keys the entry lands at the bare "<key>" instead, shadowing the base archive's
same-key resource outright (last-wins); the manifest records which channel was used.

Validation lists EVERY problem it finds (never dies on the first): PNGs that are not valid images,
pack dimensions that are not an integer multiple of the native size, keys with no manifest.tsv entry,
and native formats this packer cannot encode. With --check it writes nothing and just reports.

IMPORTANT — format matching (verified against libultraship):
  The Fast3D interpreter dispatches texture decoding on the GAME TILE's format/size, NOT the
  replacement's declared type (interpreter.cpp ImportTexture). Therefore a replacement MUST be
  encoded in the SAME N64 format the game uses for that texture. This packer reads that native
  format from the dump manifest (the `fmt` column) and re-encodes the (upscaled) PNG back into it.
  The OTEX scale fields tell the interpreter the replacement is N times larger:
      HByteScale  = packW / nativeW      VPixelScale = packH / nativeH
  (These are the UPSCALE factor, >1 for hi-res. resultNewLineSize = resultOrigLineSize * HByteScale
  in interpreter.cpp, so a bigger replacement needs a scale > 1.) Integer multiples are enforced.

  CI4/CI8 (paletted) textures are the exception: the interpreter decodes a CI replacement's OTEX
  payload as palette indices through the TLUT the GAME loaded (interpreter.cpp ImportTextureCi4/8)
  and ignores VPixelScale for CI — so CI replacements are packed at 1x ONLY, with the edited PNG
  nearest-color quantized into the ORIGINAL palette (the dumped <palette_key>.png swatch). The OTEX
  carries indices only; the palette never ships in the pack. --check prints a per-texture
  quantization report (palette size, distinct source colors, max/mean color error).

  Atlas band overrides (key scheme 2): bands of multi-tile atlas buffers are keyed
  "atlas/<baseKey>/o<byteOffset>/<FMT>/<W>x<H>" and flow through this same pipeline unchanged -
  the dump tooling emits one sliced PNG + manifest row per band, so each band is an ordinary
  replaceable key here. CI atlas bands are dump-only (no palette side-channel at runtime) and are
  skipped with a warning.

The archive is written deterministically (sorted entries, fixed 1980 timestamps) so identical inputs
produce byte-identical output.
"""
import argparse
import hashlib
import json
import os
import struct
import sys
import zipfile

try:
    from PIL import Image
except ImportError:
    sys.stderr.write("gen_texture_pack: Pillow (PIL) is required: pip install Pillow\n")
    raise

# OTR resource header constants (libultraship ResourceLoader::ReadResourceInitDataBinary).
OTR_HEADER_SIZE = 64
OTR_BYTE_ORDER_LITTLE = 0
OTR_IS_CUSTOM = 1
OTR_TYPE_TEXTURE = 0x4F544558  # 'OTEX'
OTR_TEXTURE_VERSION_V1 = 1

# Fast::TextureType enum (libultraship/include/fast/resource/type/Texture.h).
TT_RGBA32 = 1
TT_RGBA16 = 2
TT_PAL4 = 3
TT_PAL8 = 4
TT_I4 = 5      # Grayscale4bpp
TT_I8 = 6      # Grayscale8bpp
TT_IA4 = 7     # GrayscaleAlpha4bpp
TT_IA8 = 8     # GrayscaleAlpha8bpp
TT_IA16 = 9    # GrayscaleAlpha16bpp

FIXED_DATE = (1980, 1, 1, 0, 0, 0)

# Key-scheme version this packer produces. Must match kGdxWorkshopKeySchemeVersion in
# port/gdx_workshop.h — a mismatch is flagged in the in-game Workshop menu. Scheme "2" adds the
# atlas/<baseKey>/o<byteOffset>/<FMT>/<W>x<H> per-band keys; the scheme-"1" keys are unchanged.
KEY_SCHEME_VERSION = "2"
# CI (paletted) formats: fmt -> (addressable palette entries, OTEX texture type). Encoded by
# quantizing into the original palette (see module docstring); 1x scale only.
CI_FORMATS = {"CI4": (16, TT_PAL4), "CI8": (256, TT_PAL8)}
# Native formats this packer can re-encode: everything the dump produces (CI4/CI8 included, via
# palette quantization). Anything else the manifest names is rejected with this note.
UNSUPPORTED_FMT_NOTE = "unknown N64 format"


def luma(r, g, b):
    return (r * 299 + g * 587 + b * 114) // 1000


def enc_rgba32(px, w, h):
    out = bytearray(w * h * 4)
    i = 0
    for y in range(h):
        for x in range(w):
            r, g, b, a = px[x, y]
            out[i] = r; out[i + 1] = g; out[i + 2] = b; out[i + 3] = a
            i += 4
    return bytes(out), TT_RGBA32


def enc_rgba16(px, w, h):
    # N64 RGBA5551, big-endian 16-bit words.
    out = bytearray(w * h * 2)
    i = 0
    for y in range(h):
        for x in range(w):
            r, g, b, a = px[x, y]
            v = ((r >> 3) << 11) | ((g >> 3) << 6) | ((b >> 3) << 1) | (1 if a >= 128 else 0)
            out[i] = (v >> 8) & 0xFF; out[i + 1] = v & 0xFF
            i += 2
    return bytes(out), TT_RGBA16


def enc_ia16(px, w, h):
    out = bytearray(w * h * 2)
    i = 0
    for y in range(h):
        for x in range(w):
            r, g, b, a = px[x, y]
            out[i] = luma(r, g, b); out[i + 1] = a
            i += 2
    return bytes(out), TT_IA16


def enc_ia8(px, w, h):
    out = bytearray(w * h)
    i = 0
    for y in range(h):
        for x in range(w):
            r, g, b, a = px[x, y]
            out[i] = ((luma(r, g, b) >> 4) << 4) | (a >> 4)
            i += 1
    return bytes(out), TT_IA8


def _pack4(vals, w, h):
    # Pack a list of 4-bit values (row-major, w*h of them) two-per-byte, rows padded to whole bytes.
    out = bytearray()
    for y in range(h):
        row = vals[y * w:(y + 1) * w]
        for x in range(0, w, 2):
            hi = row[x] & 0xF
            lo = row[x + 1] & 0xF if x + 1 < w else 0
            out.append((hi << 4) | lo)
    return bytes(out)


def enc_ia4(px, w, h):
    vals = []
    for y in range(h):
        for x in range(w):
            r, g, b, a = px[x, y]
            i3 = luma(r, g, b) >> 5
            a1 = 1 if a >= 128 else 0
            vals.append((i3 << 1) | a1)
    return _pack4(vals, w, h), TT_IA4


def enc_i8(px, w, h):
    out = bytearray(w * h)
    i = 0
    for y in range(h):
        for x in range(w):
            r, g, b, a = px[x, y]
            out[i] = luma(r, g, b)
            i += 1
    return bytes(out), TT_I8


def enc_i4(px, w, h):
    vals = []
    for y in range(h):
        for x in range(w):
            r, g, b, a = px[x, y]
            vals.append(luma(r, g, b) >> 4)
    return _pack4(vals, w, h), TT_I4


def _to_5551(c):
    r, g, b, a = c
    return (r >> 3, g >> 3, b >> 3, 1 if a >= 128 else 0)


def _quantize_indices(px, w, h, palette):
    """Nearest-color map of every pixel into `palette` (8-bit (r,g,b,a) tuples, index order).
    Distance is RGBA5551-space squared error (alpha contributes as its bit-0 value scaled to the
    5-bit range); ties resolve to the lowest index, so the mapping is deterministic. Returns
    (indices, distinct_source_colors, max_err, mean_err) with errors in 5-bit-channel units."""
    pal5551 = [_to_5551(c) for c in palette]
    cache = {}  # source RGBA -> (palette index, error)
    indices = []
    for y in range(h):
        for x in range(w):
            c = px[x, y]
            hit = cache.get(c)
            if hit is None:
                s = _to_5551(c)
                best_i, best_d = 0, None
                for i, p in enumerate(pal5551):
                    d = ((s[0] - p[0]) ** 2 + (s[1] - p[1]) ** 2 + (s[2] - p[2]) ** 2
                         + ((s[3] - p[3]) * 31) ** 2)
                    if best_d is None or d < best_d:
                        best_i, best_d = i, d
                hit = (best_i, best_d ** 0.5)
                cache[c] = hit
            indices.append(hit[0])
    errs = [e for _, e in cache.values()]
    return indices, len(cache), max(errs), sum(errs) / len(errs)


def enc_ci4(px, w, h, palette):
    # 2 texels/byte, high nibble first (same convention as _pack4 for I4/IA4). Only the first 16
    # palette entries are addressable by a 4-bit index.
    indices, distinct, max_err, mean_err = _quantize_indices(px, w, h, palette[:16])
    stats = (min(len(palette), 16), distinct, max_err, mean_err)
    return _pack4(indices, w, h), TT_PAL4, stats


def enc_ci8(px, w, h, palette):
    indices, distinct, max_err, mean_err = _quantize_indices(px, w, h, palette[:256])
    stats = (min(len(palette), 256), distinct, max_err, mean_err)
    return bytes(bytearray(indices)), TT_PAL8, stats


ENCODERS = {
    "RGBA32": enc_rgba32,
    "RGBA16": enc_rgba16,
    "IA16": enc_ia16,
    "IA8": enc_ia8,
    "IA4": enc_ia4,
    "I8": enc_i8,
    "I4": enc_i4,
    # CI encoders take the palette as a 4th argument; plan_pack dispatches on CI_FORMATS.
    "CI4": enc_ci4,
    "CI8": enc_ci8,
}


def build_otex_v1(tex_type, width, height, h_scale, v_scale, image_data):
    """Return the full resource file bytes: 64-byte OTR header + V1 subheader + raw texel data."""
    header = bytearray(OTR_HEADER_SIZE)
    header[0] = OTR_BYTE_ORDER_LITTLE
    header[1] = OTR_IS_CUSTOM
    struct.pack_into("<I", header, 4, OTR_TYPE_TEXTURE)
    struct.pack_into("<I", header, 8, OTR_TEXTURE_VERSION_V1)
    struct.pack_into("<Q", header, 12, 0)  # Id (unused by the reader)
    sub = struct.pack("<IIIIffI", tex_type, width, height, 0, h_scale, v_scale, len(image_data))
    return bytes(header) + sub + image_data


def load_native_manifest(path):
    native = {}
    with open(path, "r", encoding="utf-8") as fh:
        for line in fh:
            line = line.rstrip("\n")
            if not line or line.startswith("#"):
                continue
            parts = line.split("\t")
            if len(parts) < 4:
                continue
            key, w, h, fmt = parts[0], parts[1], parts[2], parts[3].strip()
            # Optional 5th column: palette_key (TLUT swatch key) on CI4/CI8 rows; "-" when absent.
            pal = parts[4].strip() if len(parts) >= 5 else ""
            try:
                native[key] = (int(w), int(h), fmt, pal if pal and pal != "-" else None)
            except ValueError:
                continue
    return native


def collect_pngs(base):
    entries = []
    for root, dirs, files in os.walk(base):
        dirs.sort()
        for f in sorted(files):
            if not f.lower().endswith(".png"):
                continue
            full = os.path.join(root, f)
            rel = os.path.relpath(full, base).replace(os.sep, "/")
            key = rel[:-len(".png")]
            entries.append((key, full))
    return sorted(entries)


def validate_pack_id(pid):
    """Pack ids join into comma-separated lists at runtime (load order, disable list), so an id
    must be a non-empty string with no commas. Raises ValueError with a clear message."""
    if not isinstance(pid, str) or not pid.strip():
        raise ValueError('pack metadata "id" must be a non-empty string')
    if "," in pid:
        raise ValueError('pack id "%s" must not contain commas (the runtime stores ids in '
                         'comma-joined lists)' % pid)


def resolve_manifest_bytes(args, native):
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

    default_name = os.path.splitext(os.path.basename(args.output_o2r))[0] if args.output_o2r else "texture-pack"
    manifest = {
        "name": args.name or base.get("name") or default_name,
        "version": args.version or base.get("version") or "0.1",
        "author": args.author or base.get("author") or "unknown",
        "game_version": args.game_version or base.get("game_version") or "us.rev0",
        "key_scheme_version": args.key_scheme_version or base.get("key_scheme_version") or KEY_SCHEME_VERSION,
        # Stable pack identity (load order, disable list); defaults to the output filename stem,
        # which is also the runtime's fallback for old packs with no id.
        "id": args.id or base.get("id") or default_name,
        # Which archive channel the entries landed on: "pack" (textures/pack/<key>, the default
        # override lookup) or "direct" (<key>, shadowing the base archive's same-key resource).
        "key_channel": "direct" if getattr(args, "direct_keys", False) else "pack",
    }
    # Preserve any extra fields the modder added to their metadata file (depends, conflicts, ...).
    for k, v in base.items():
        manifest.setdefault(k, v)
    validate_pack_id(manifest["id"])
    return json.dumps(manifest, indent=2, sort_keys=True).encode("utf-8"), source


def plan_pack(input_dir, native, direct_keys=False):
    """Validate every PNG against the native manifest. Returns (entries, errors, warnings, infos)
    where entries is the list of (arc_path, blob) ready to write. Collects ALL problems.

    direct_keys=False archives each entry at "textures/pack/<key>" (the override channel the
    interpreter consults per texture). direct_keys=True archives at the bare "<key>" so the entry
    shadows the base archive's same-key resource outright (ArchiveManager is last-wins); hash/
    keys shadow no base resource and are rejected in that mode."""
    entries = []
    errors = []
    warnings = []
    infos = []

    pngs = collect_pngs(input_dir)
    if not pngs:
        errors.append("no .png files found under %s" % input_dir)
        return entries, errors, warnings, infos

    seen_keys = set()
    for key, full in pngs:
        seen_keys.add(key)
        if key not in native:
            warnings.append("%s: no manifest.tsv entry (unknown key) - skipped" % key)
            continue
        nat_w, nat_h, fmt, palette_key = native[key]
        if fmt == "TLUT":
            continue  # palette swatch sidecar, not a replaceable texture
        if key.startswith("atlas/") and fmt in CI_FORMATS:
            warnings.append("%s: CI atlas bands are dump-only (no palette side-channel at "
                            "runtime) - skipped" % key)
            continue
        if fmt not in ENCODERS:
            warnings.append("%s: native format %s is not encodable (%s) - skipped"
                            % (key, fmt, UNSUPPORTED_FMT_NOTE))
            continue
        try:
            img = Image.open(full)
            img.load()
            img = img.convert("RGBA")
        except Exception as exc:  # noqa: BLE001 - report any decode failure, keep going
            errors.append("%s: not a readable PNG (%s)" % (key, exc))
            continue
        pw, ph = img.size
        if nat_w <= 0 or nat_h <= 0:
            errors.append("%s: invalid native size %dx%d in manifest.tsv" % (key, nat_w, nat_h))
            continue
        if pw % nat_w != 0 or ph % nat_h != 0:
            errors.append("%s: pack size %dx%d is not an integer multiple of native %dx%d "
                          "(allowed: %dx%d, %dx%d, %dx%d, ...)"
                          % (key, pw, ph, nat_w, nat_h,
                             nat_w, nat_h, nat_w * 2, nat_h * 2, nat_w * 4, nat_h * 4))
            continue
        px = img.load()
        if fmt in CI_FORMATS:
            # CI replacements decode through the game-loaded TLUT and ignore the scale fields
            # (interpreter.cpp ImportTextureCi4/8), so only 1x is encodable.
            if pw != nat_w or ph != nat_h:
                errors.append("%s: %s textures must be packed at native size %dx%d (got %dx%d) - "
                              "hi-res CI is not supported (the interpreter ignores the OTEX scale "
                              "fields for CI)" % (key, fmt, nat_w, nat_h, pw, ph))
                continue
            if palette_key is None:
                warnings.append("%s: no palette_key in manifest.tsv (re-dump with the updated dump "
                                "tooling to make this CI texture replaceable) - skipped" % key)
                continue
            pal_path = os.path.join(input_dir, palette_key + ".png")
            if not os.path.isfile(pal_path):
                errors.append("%s: palette sidecar %s.png not found (re-dump; the dumper writes the "
                              "TLUT swatch next to the texture)" % (key, palette_key))
                continue
            try:
                pal_img = Image.open(pal_path)
                pal_img.load()
                palette = list(pal_img.convert("RGBA").getdata())
            except Exception as exc:  # noqa: BLE001 - report any decode failure, keep going
                errors.append("%s: palette sidecar %s.png is not a readable PNG (%s)"
                              % (key, palette_key, exc))
                continue
            if not palette:
                errors.append("%s: palette sidecar %s.png has no entries" % (key, palette_key))
                continue
            image_data, tex_type, stats = ENCODERS[fmt](px, pw, ph, palette)
            infos.append("%s: %s quantized into a %d-entry palette (%d distinct source colors, "
                         "max err %.1f, mean err %.2f in RGBA5551 units)" % ((key, fmt) + stats))
        else:
            image_data, tex_type = ENCODERS[fmt](px, pw, ph)
        blob = build_otex_v1(tex_type, pw, ph, float(pw) / float(nat_w), float(ph) / float(nat_h), image_data)
        if direct_keys:
            if key.startswith("hash/"):
                warnings.append("%s: hash keys shadow no base resource (inert as a direct key) - skipped" % key)
                continue
            entries.append((key, blob))
        else:
            entries.append(("textures/pack/" + key, blob))

    # Informational: manifest rows the game recorded but that this pack does not override.
    missing = sorted(k for k in native if k not in seen_keys)
    if missing:
        infos.append("%d texture(s) in manifest.tsv have no PNG in this pack (left as-is)" % len(missing))
    return entries, errors, warnings, infos


def report(errors, warnings, infos):
    for msg in infos:
        print("info:  %s" % msg)
    for msg in warnings:
        print("warn:  %s" % msg)
    for msg in errors:
        sys.stderr.write("error: %s\n" % msg)


def check_pack(path):
    """Validate an existing .o2r pack without writing. Returns process exit code."""
    if not zipfile.is_zipfile(path):
        sys.stderr.write("error: %s is not a valid .o2r (zip) archive\n" % path)
        return 2
    errors = []
    tex_count = 0
    verified = 0
    with zipfile.ZipFile(path, "r") as z:
        names = z.namelist()
        has_meta = "workshop.json" in names or "manifest.json" in names
        if not has_meta:
            errors.append("no workshop.json (pack metadata) at archive root")
        for name in names:
            if name in ("workshop.json", "manifest.json"):
                continue
            tex_count += 1
            data = z.read(name)
            if len(data) < OTR_HEADER_SIZE + 4:
                errors.append("%s: too small to be an OTEX resource" % name)
                continue
            magic = struct.unpack_from("<I", data, 4)[0]
            version = struct.unpack_from("<I", data, 8)[0]
            if magic != OTR_TYPE_TEXTURE:
                errors.append("%s: bad resource magic 0x%08X (expected OTEX)" % (name, magic))
            elif version != OTR_TEXTURE_VERSION_V1:
                errors.append("%s: unexpected OTEX version %d" % (name, version))
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
            for field in ("depends", "conflicts"):
                value = meta.get(field)
                if value is not None and (not isinstance(value, list)
                                          or not all(isinstance(item, str) for item in value)):
                    errors.append('workshop.json "%s" must be an array of pack id strings' % field)
            files = meta.get("files")
            if files is not None:
                if not isinstance(files, list):
                    errors.append('workshop.json "files" must be an array of '
                                  '{"path", "sha256"} entries')
                else:
                    for entry in files:
                        if (not isinstance(entry, dict)
                                or not isinstance(entry.get("path"), str)
                                or not isinstance(entry.get("sha256"), str)):
                            errors.append('workshop.json "files" entries must be '
                                          '{"path", "sha256"} objects')
                            continue
                        fpath, want = entry["path"], entry["sha256"]
                        if fpath not in names:
                            errors.append('files[] entry "%s" is not in the archive' % fpath)
                            continue
                        got = hashlib.sha256(z.read(fpath)).hexdigest()
                        if got != want.lower():
                            errors.append("%s: sha256 mismatch (manifest %s, archive has %s)"
                                          % (fpath, want, got))
                            continue
                        verified += 1
    print("pack: %s" % path)
    print("  %d texture override(s)" % tex_count)
    atlas_count = sum(1 for n in names if n.startswith("textures/pack/atlas/"))
    if atlas_count:
        print("  %d of which are atlas band override(s)" % atlas_count)
    if verified:
        print("  %d file(s) verified against the files[] integrity manifest" % verified)
    if tex_count == 0:
        errors.append("no texture override resources found — pack overrides nothing")
    for msg in errors:
        sys.stderr.write("error: %s\n" % msg)
    if errors:
        print("FAILED: %d problem(s)" % len(errors))
        return 1
    print("OK")
    return 0


def check_dir(input_dir, native_path, direct_keys=False):
    """Validate a dump/pack source directory without writing. Returns process exit code."""
    if not os.path.isfile(native_path):
        sys.stderr.write("error: native manifest not found: %s\n" % native_path)
        return 2
    native = load_native_manifest(native_path)
    entries, errors, warnings, infos = plan_pack(input_dir, native, direct_keys=direct_keys)
    report(errors, warnings, infos)
    print("check: %s" % input_dir)
    print("  %d texture(s) would be packed, %d skipped, %d error(s)"
          % (len(entries), len(warnings), len(errors)))
    if errors:
        print("FAILED")
        return 1
    print("OK")
    return 0


def main():
    ap = argparse.ArgumentParser(
        description="Pack <key>.png textures into a deterministic .o2r OTEX pack",
        epilog="Examples:\n"
               "  gen_texture_pack.py dump/ my-pack.o2r --name \"My Pack\" --author you\n"
               "  gen_texture_pack.py dump/ --check\n"
               "  gen_texture_pack.py my-pack.o2r --check",
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("input", help="dump/pack source directory (or an .o2r pack when --check)")
    ap.add_argument("output_o2r", nargs="?", default=None, help="output .o2r path (omit with --check)")
    ap.add_argument("--check", action="store_true",
                    help="validate the input directory or an existing .o2r pack; write nothing")
    ap.add_argument("--manifest", default=None,
                    help="pack metadata json (default <input>/workshop.json or manifest.json)")
    ap.add_argument("--native-manifest", default=None,
                    help="dump manifest.tsv with native dims/fmt (default <input>/manifest.tsv)")
    ap.add_argument("--name", default=None, help="pack name for synthesized metadata")
    ap.add_argument("--author", default=None, help="pack author for synthesized metadata")
    ap.add_argument("--version", default=None, help="pack version for synthesized metadata")
    ap.add_argument("--game-version", default=None, help="target game build (default us.rev0)")
    ap.add_argument("--key-scheme-version", default=None,
                    help="key-scheme version (default %s)" % KEY_SCHEME_VERSION)
    ap.add_argument("--id", default=None,
                    help="stable pack identifier for synthesized metadata, e.g. author.packname "
                         "(default: output filename stem); commas are not allowed")
    ap.add_argument("--direct-keys", action="store_true",
                    help="archive each entry at its bare <key> so it shadows the base archive's "
                         "same-key resource (last-wins), instead of the default textures/pack/<key> "
                         "override channel. hash/ keys are rejected in this mode")
    args = ap.parse_args()

    # --check on an existing pack file.
    if args.check and os.path.isfile(args.input) and args.input.lower().endswith(".o2r"):
        return check_pack(args.input)

    # From here on the input is treated as a directory. Keep args.input_dir compatible with helpers.
    args.input_dir = args.input
    native_path = args.native_manifest or os.path.join(args.input_dir, "manifest.tsv")

    if args.check:
        return check_dir(args.input_dir, native_path, direct_keys=args.direct_keys)

    if not args.output_o2r:
        sys.stderr.write("error: output_o2r is required (or pass --check to validate only)\n")
        return 2
    if not os.path.isdir(args.input_dir):
        sys.stderr.write("error: input directory not found: %s\n" % args.input_dir)
        return 2
    if not os.path.isfile(native_path):
        sys.stderr.write("error: native manifest not found: %s\n"
                         "  (run the in-game texture dump first; it writes dump/manifest.tsv)\n" % native_path)
        return 2

    native = load_native_manifest(native_path)
    try:
        manifest_bytes, manifest_source = resolve_manifest_bytes(args, native)
    except json.JSONDecodeError as exc:
        sys.stderr.write("error: pack metadata is not valid JSON (%s)\n" % exc)
        return 2
    except ValueError as exc:
        sys.stderr.write("error: %s\n" % exc)
        return 2

    entries, errors, warnings, infos = plan_pack(args.input_dir, native, direct_keys=args.direct_keys)
    report(errors, warnings, infos)
    if errors:
        sys.stderr.write("error: %d problem(s) must be fixed before packing; nothing written\n" % len(errors))
        return 1
    if not entries:
        sys.stderr.write("error: nothing to pack (every PNG was skipped - see warnings above)\n")
        return 2

    arc_entries = list(entries)
    # Integrity manifest: sha256 over the exact bytes archived, one entry per payload (the
    # metadata file itself is excluded — it cannot hash its own final bytes).
    manifest = json.loads(manifest_bytes)
    manifest["files"] = [{"path": arc, "sha256": hashlib.sha256(data).hexdigest()}
                         for arc, data in sorted(entries)]
    manifest_bytes = json.dumps(manifest, indent=2, sort_keys=True).encode("utf-8")
    # Metadata is stored as workshop.json: "manifest.json" is libultraship's reserved archive
    # manifest (numeric game_version schema) and our string game_version made LUS throw on mount.
    arc_entries.append(("workshop.json", manifest_bytes))
    arc_entries.sort(key=lambda e: e[0])

    with zipfile.ZipFile(args.output_o2r, "w", zipfile.ZIP_DEFLATED) as z:
        for arc, data in arc_entries:
            info = zipfile.ZipInfo(arc, date_time=FIXED_DATE)
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o644 << 16
            z.writestr(info, data)

    print("wrote %s (%d texture override(s), metadata %s)"
          % (args.output_o2r, len(entries), manifest_source))
    for msg in warnings:
        print("  skipped: %s" % msg)
    return 0


if __name__ == "__main__":
    sys.exit(main())
