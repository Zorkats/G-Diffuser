#!/usr/bin/env python3
"""Offline "Dump All" — decode the game's named assets straight from the extracted archive, with no
running game and no display.

R8 Step 3 core: this is the offline mirror of the in-game Workshop texture dump
(port/gdx_workshop.cpp). It walks the SAME asset recipe yamls the binding generator consumes, forms
the SAME `<yaml_stem>/<symbol>` o2r keys, pulls each asset's bytes from the extracted archive
(assets/extracted/generic.o2r), and decodes textures with a standalone decoder that mirrors the
libultraship Fast3D interpreter's ImportTexture* math EXACTLY (5->8 bit scaling, alpha bits,
big-endian CI palette loads). Output is `<dump>/<key>.png` + `<dump>/manifest.tsv` rows shaped
identically to the in-game dump, so tools/gen_texture_pack.py consumes an offline dump unchanged.

CLASS REGISTRY (Step 4b hangs off this): every dumpable asset kind is a class in CLASS_REGISTRY.
`textures` is implemented in this file; `coursedata` / `dlists` / `vertexdata` / `tables` (Tier 1) and
`ghosts` / `fonts` (Tier 2) are implemented in gen_dump_all_extra.py (registered below, imported
after CLASS_REGISTRY exists) so this file's architecture stays untouched. `models` / `audio` / `midi`
still slot in later by registering another DumpClass. `--classes textures[,coursedata,...]` selects
which run.

JP FUTURE-PROOFING (plan rule): everything derives from a (recipe tree + archive) pair chosen by
`--profile us/rev0` (default) / `--profile jp/rev0`; there are no hardcoded asset paths beyond the
profile defaults resolved through gen_asset_bindings.resolve_paths().

LOAD-BEARING CHECKS (both mandatory, both in this tool):
  (a) key self-check: every emitted key is asserted against the o2r keys in the checked-in
      port/gen/AssetBindings.c (sCommonAssetRomMap / sAssetSegmentMap rows). A key the generator
      would not emit is a hard failure — the offline walk must not drift from the binding tables.
  (b) --verify: given a dump dir that already holds in-game play-session PNGs, decode the same keys
      offline and compare pixels. This is the offline == runtime acceptance test. (PNG *container*
      bytes differ by encoder — stb in-game vs Pillow here — so the comparison is over decoded RGBA
      pixels, which is the load-bearing equivalence; identical pixels re-encode identically.)

Machinery is IMPORTED, never re-implemented: gen_asset_bindings.py owns yaml resolution and the recipe
walk conventions; gen_texture_pack.py owns the N64 texel ENCODERS (this decoder is their inverse, and
--round-trip proves it by re-encoding a decoded PNG and byte-comparing against the source archive
bytes).
"""
import argparse
import glob
import os
import re
import struct
import sys
import zipfile

# Import the binding generator so yaml resolution + recipe-walk conventions live in exactly one place.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gen_asset_bindings as gab  # noqa: E402  (path insert must precede import)

try:
    import yaml  # noqa: F401  (gab already requires it; surfaced here for a clean error)
except ImportError:
    sys.stderr.write("gen_dump_all: PyYAML is required: pip install pyyaml\n")
    raise

try:
    from PIL import Image
except ImportError:
    sys.stderr.write("gen_dump_all: Pillow (PIL) is required for PNG I/O: pip install Pillow\n")
    raise

REPO = gab.REPO

# ── archive framing ─────────────────────────────────────────────────────────────────────────────────
# Every archive entry is a 64-byte OTR header + a per-type sub-header + payload. For a Torch Texture
# resource the sub-header is 16 bytes: [u32 texType][u32 width][u32 height][u32 dataSize]; the raw
# texel payload therefore starts at 0x50 (verified against assets/extracted/generic.o2r and matched by
# port/AssetLoader.cpp's "TextureFactory sets ImageData = buffer + 0x50" note). The payload is exactly
# width*height*bpp bytes with a tight row stride (no padding), so a single linear index over the texels
# equals the interpreter's y*fullImageLineSize + x once fullImageLineSize == the natural row width.
OTR_HEADER_SIZE = 64
TORCH_TEXTURE_SUBHEADER_SIZE = 16
TORCH_TEXTURE_PAYLOAD_OFFSET = OTR_HEADER_SIZE + TORCH_TEXTURE_SUBHEADER_SIZE  # 0x50

# Formats this standalone decoder mirrors. TLUT is intentionally absent: standalone TLUT entries are
# palettes, not images, and are skipped (they are consumed as the CI palette side-channel instead).
DECODABLE_FORMATS = {"RGBA16", "RGBA32", "IA16", "IA8", "IA4", "I8", "I4", "CI4", "CI8"}
BITS_PER_TEXEL = {
    "RGBA16": 16, "RGBA32": 32, "IA16": 16, "IA8": 8, "IA4": 4,
    "I8": 8, "I4": 4, "CI4": 4, "CI8": 8, "TLUT": 16,
}


def texel_bytes(fmt, count):
    """Byte count for `count` texels of `fmt` (rounds 4bpp up, matching packed nibble rows)."""
    return (count * BITS_PER_TEXEL[fmt] + 7) // 8


# ── scale macros (verbatim from interpreter.cpp lines 60-65) ──────────────────────────────────────────
def scale_5_8(v):
    return (v * 0xFF) // 0x1F


def scale_4_8(v):
    return v * 0x11


def scale_3_8(v):
    return v * 0x24


# ── standalone decoders (each mirrors the matching Interpreter::ImportTexture* body) ──────────────────
# Every decoder returns width*height*4 RGBA bytes. `payload` is the tight-packed texel data; `pal` is
# the CI palette payload (raw big-endian RGBA5551 entries) or None.
def _dec_rgba16(payload, w, h, pal=None):
    out = bytearray(w * h * 4)
    for i in range(w * h):
        c = (payload[2 * i] << 8) | payload[2 * i + 1]  # big-endian 16-bit word
        a = c & 1
        o = 4 * i
        out[o] = scale_5_8(c >> 11)
        out[o + 1] = scale_5_8((c >> 6) & 0x1F)
        out[o + 2] = scale_5_8((c >> 1) & 0x1F)
        out[o + 3] = 255 if a else 0
    return bytes(out)


def _dec_rgba32(payload, w, h, pal=None):
    # Direct 8-8-8-8 copy (ImportTextureRgba32).
    return bytes(payload[: w * h * 4])


def _dec_ia16(payload, w, h, pal=None):
    out = bytearray(w * h * 4)
    for i in range(w * h):
        intensity = payload[2 * i]
        alpha = payload[2 * i + 1]
        o = 4 * i
        out[o] = out[o + 1] = out[o + 2] = intensity  # no scaling: already 8-bit
        out[o + 3] = alpha
    return bytes(out)


def _dec_ia8(payload, w, h, pal=None):
    out = bytearray(w * h * 4)
    for i in range(w * h):
        byte = payload[i]
        intensity = scale_4_8(byte >> 4)
        alpha = scale_4_8(byte & 0xF)
        o = 4 * i
        out[o] = out[o + 1] = out[o + 2] = intensity
        out[o + 3] = alpha
    return bytes(out)


def _dec_ia4(payload, w, h, pal=None):
    out = bytearray(w * h * 4)
    for i in range(w * h):
        byte = payload[i // 2]
        part = (byte >> (4 - (i % 2) * 4)) & 0xF
        intensity = scale_3_8(part >> 1)  # 3-bit intensity
        alpha = part & 1
        o = 4 * i
        out[o] = out[o + 1] = out[o + 2] = intensity
        out[o + 3] = 255 if alpha else 0
    return bytes(out)


def _dec_i8(payload, w, h, pal=None):
    out = bytearray(w * h * 4)
    for i in range(w * h):
        intensity = payload[i]  # ImportTextureI8: R=G=B=A=intensity (alpha follows intensity)
        o = 4 * i
        out[o] = out[o + 1] = out[o + 2] = out[o + 3] = intensity
    return bytes(out)


def _dec_i4(payload, w, h, pal=None):
    out = bytearray(w * h * 4)
    for i in range(w * h):
        byte = payload[i // 2]
        part = (byte >> (4 - (i % 2) * 4)) & 0xF
        v = scale_4_8(part)  # R=G=B=A=SCALE_4_8(intensity)
        o = 4 * i
        out[o] = out[o + 1] = out[o + 2] = out[o + 3] = v
    return bytes(out)


def _decode_ci_pixel(out, o, col16):
    a = col16 & 1
    out[o] = scale_5_8(col16 >> 11)
    out[o + 1] = scale_5_8((col16 >> 6) & 0x1F)
    out[o + 2] = scale_5_8((col16 >> 1) & 0x1F)
    out[o + 3] = 255 if a else 0


def _dec_ci4(payload, w, h, pal):
    out = bytearray(w * h * 4)
    for i in range(w * h):
        byte = payload[i // 2]
        idx = (byte >> (4 - (i % 2) * 4)) & 0xF
        col16 = (pal[idx * 2] << 8) | pal[idx * 2 + 1]  # big-endian palette load
        _decode_ci_pixel(out, 4 * i, col16)
    return bytes(out)


def _dec_ci8(payload, w, h, pal):
    out = bytearray(w * h * 4)
    for i in range(w * h):
        idx = payload[i]
        col16 = (pal[idx * 2] << 8) | pal[idx * 2 + 1]  # big-endian palette load
        _decode_ci_pixel(out, 4 * i, col16)
    return bytes(out)


DECODERS = {
    "RGBA16": _dec_rgba16,
    "RGBA32": _dec_rgba32,
    "IA16": _dec_ia16,
    "IA8": _dec_ia8,
    "IA4": _dec_ia4,
    "I8": _dec_i8,
    "I4": _dec_i4,
    "CI4": _dec_ci4,
    "CI8": _dec_ci8,
}


# ── archive access ────────────────────────────────────────────────────────────────────────────────────
class ArchiveSource:
    """Reads per-asset texel payloads from the extracted .o2r, with an optional raw-ROM fallback for
    keys the archive does not carry (uncompressed segment entries only)."""

    def __init__(self, archive_path, rom_path=None):
        self.zip = zipfile.ZipFile(archive_path)
        self.names = set(self.zip.namelist())
        self.rom = None
        if rom_path is not None and os.path.isfile(rom_path):
            with open(rom_path, "rb") as fh:
                self.rom = fh.read()

    def payload(self, key):
        """Return the raw texel payload for an archive key (header stripped), or None."""
        if key not in self.names:
            return None
        data = self.zip.read(key)
        if len(data) < TORCH_TEXTURE_PAYLOAD_OFFSET:
            return None
        return data[TORCH_TEXTURE_PAYLOAD_OFFSET:]

    def rom_slice(self, abs_offset, nbytes):
        """Raw ROM-slice fallback: only valid for uncompressed entries; returns None when unavailable."""
        if self.rom is None or abs_offset < 0 or abs_offset + nbytes > len(self.rom):
            return None
        return self.rom[abs_offset:abs_offset + nbytes]


# ── EK (Expansion Kit) archive access ───────────────────────────────────────────────────────────────
# The 64DD disk archive (fzerox-disk.o2r, key "ek/<symbol>") frames its per-asset entries DIFFERENTLY
# from the cart archive: EK entries are RAW / headerless, not OTR_HEADER + Torch-texture-subheader.
# Verified empirically against every one of the 310 texture-typed rows in port/gen/ek_slice_manifest.txt
# (both against the checked-in build/x64/port/Release/fzerox-disk.o2r and re-derivable any time the tool
# runs): each `zip.read("ek/<symbol>")` byte length equals the manifest's `len` column EXACTLY --
# uncompressed TEXTURE rows match width*height*bpp/8 bit-for-bit, and COMPRESSED_TEXTURE rows are a raw
# MIO0 blob (magic bytes "MIO0" at offset 0) whose OWN header/backref stream is the only structure
# present. So: trust the byte-length check against the manifest, not the cart archive's framing
# assumptions -- do NOT reuse ArchiveSource.payload() (which unconditionally strips a 0x50-byte header)
# for EK keys.
class EkArchiveSource:
    """Raw (headerless) reader for the 64DD Expansion Kit disk archive."""

    def __init__(self, archive_path):
        self.zip = zipfile.ZipFile(archive_path)
        self.names = set(self.zip.namelist())

    def raw(self, key):
        if key not in self.names:
            return None
        return self.zip.read(key)


# EK asset recipe tree (fzerox-expansion-kit decomp yaml source) -- NOT part of the cart profile tree
# gen_asset_bindings.resolve_paths() resolves; the 64DD EK disk is the same physical archive regardless
# of --profile (it always carries the JP/translated-disk asset layout, see the ek_slice_manifest.txt
# header comment: "serves both retail JP and the fan-translated disk"). Resolved once, overridable via
# --ek-yaml-dir, so nothing here is a hardcoded path beyond this profile-agnostic default.
EK_YAML_DIR = os.path.join(REPO, "fzerox-expansion-kit", "assets", "yaml", "jp")


def parse_ek_manifest(manifest_path):
    """Yield (symbol, offset, length, type, fmt, width, height, tlut) for every TEXTURE /
    COMPRESSED_TEXTURE row of port/gen/ek_slice_manifest.txt (v1 format: `symbol offset(hex) len(dec)
    type format width(dec) height(dec) tlut(hex)`, absent fields '-'). Non-texture rows (GFX / BLOB /
    VTX / FZX:LIMB / ...) and header/version/count lines are skipped by the 8-field/type filter."""
    rows = []
    with open(manifest_path, encoding="utf-8") as fh:
        for line in fh:
            parts = line.split()
            if len(parts) != 8 or parts[3] not in ("TEXTURE", "COMPRESSED_TEXTURE"):
                continue
            sym, off_hex, len_dec, typ, fmt, w, h, tlut_hex = parts
            rows.append((
                sym, int(off_hex, 16), int(len_dec), typ, fmt,
                None if w == "-" else int(w),
                None if h == "-" else int(h),
                None if tlut_hex == "-" else int(tlut_hex, 16),
            ))
    return rows


def ek_recipe_index(yaml_dir):
    """Walk the EK asset recipe tree recursively (EK yamls nest under courses/, ghosts/, overlays/**,
    unlike the flat cart yaml_dir -- gen_dump_all_extra._yaml_dir_files() is intentionally flat and is
    NOT reused here) and return:
      tlut_by_addr  -- {full_address -> TLUT symbol}, built with the SAME formula walk_textures() uses
                       for the cart side (segment<<24 | offset, or a bare offset when the yaml carries
                       no `:config: segments:` block). Verified against the source tree: EK's segment-
                       configured expansion_kit_textures.yaml (segment 7) has aExpansionKitNameEntryTex's
                       `tlut: 0x700FF80` == (7<<24)|0xFF80 == aExpansionKitNameEntryPalette's own
                       `offset: 0xFF80`; EK's segment-less overlays/records/records_assets.yaml has
                       aMenuCopyGhostTex's `tlut: 0x101258` == aMenuCopyGhostPalette's own bare
                       `offset: 0x101258` (no segment shift) -- both are the same formula.
      recipe_symbols -- every TEXTURE/COMPRESSED_TEXTURE symbol (any format) found in the tree; the
                       self-check ground truth for EK keys (see TextureDumpClass.self_check_ek_keys):
                       ek_slice_manifest.txt is itself generated FROM this recipe tree (+ the
                       EkAssetBindings.c fill table it produces), so cross-checking every emitted
                       "ek/<symbol>" key's symbol against this set is the EK-side analogue of the cart
                       self-check (which asserts emitted keys against AssetBindings.c) -- EkAssetBindings.c
                       carries no `"ek/<symbol>"` string literals to grep (it only emits `u8 sym[N];`
                       array *definitions*), so the recipe tree is the closest available ground-truth
                       table, and is exactly what tools/gen_ek_assets.py itself consults to build both
                       EkAssetBindings.c and this manifest.
    """
    tlut_by_addr = {}
    recipe_symbols = set()
    for path in sorted(glob.glob(os.path.join(yaml_dir, "**", "*.yaml"), recursive=True)):
        with open(path, encoding="utf-8") as fh:
            data = yaml.safe_load(fh) or {}
        config = data.get(":config", {}) or {}
        segs = config.get("segments") or []
        segment_id = None
        if segs and isinstance(segs[0], (list, tuple)) and len(segs[0]) >= 2:
            segment_id = int(segs[0][0])
        for key, val in data.items():
            if not isinstance(val, dict) or str(key).startswith(":"):
                continue
            if val.get("type") not in ("TEXTURE", "COMPRESSED_TEXTURE"):
                continue
            fmt = val.get("format")
            if fmt is None or val.get("offset") is None:
                continue
            sym = val.get("symbol", key)
            recipe_symbols.add(sym)
            if fmt == "TLUT":
                off = int(val["offset"])
                full = (segment_id << 24) | off if segment_id is not None else off
                tlut_by_addr[full] = sym
    return tlut_by_addr, recipe_symbols


def walk_ek_textures(yaml_dir, manifest_path):
    """Build the EK texture item list. port/gen/ek_slice_manifest.txt is the PRIMARY source (it IS the
    archive's "ek/<symbol>" key index -- symbol/type/format/width/height/tlut all come from it); the EK
    recipe tree is consulted only to resolve each CI8 row's `tlut` column to its palette symbol (the
    manifest carries physical .ndd byte offsets, not the segment-relative addresses `tlut:` encodes) and
    to supply the self-check ground-truth symbol set.

    Returns (texture_items, palette_items, recipe_symbols):
      texture_items  -- TextureItem list for every renderable row (RGBA16/I4/I8/IA8/CI8/... -- any
                        DECODABLE_FORMATS member)
      palette_items  -- (key, sym) list for every standalone TLUT row (a palette, not an image -- same
                        as the cart side, which never decodes a TLUT as a texture; dumped separately as
                        a swatch, see decode_ek_swatch())
      recipe_symbols -- passthrough from ek_recipe_index(), for self_check_ek_keys()
    """
    tlut_by_addr, recipe_symbols = ek_recipe_index(yaml_dir)
    texture_items = []
    palette_items = []
    for sym, off, length, typ, fmt, w, h, tlut in parse_ek_manifest(manifest_path):
        key = "ek/%s" % sym
        if fmt == "TLUT":
            palette_items.append((key, sym))
            continue
        if fmt not in DECODABLE_FORMATS:
            continue  # defensive: every EK texture row observed uses RGBA16/I4/I8/IA8/CI8
        palette_key = None
        if fmt in ("CI4", "CI8"):
            if tlut is None:
                continue  # a CI texture with no palette reference cannot be decoded; skip defensively
            pal_sym = tlut_by_addr.get(tlut)
            if pal_sym is None:
                pal_sym = tlut_by_addr.get(tlut & 0x00FFFFFF)
            if pal_sym is None:
                continue
            palette_key = "ek/%s" % pal_sym
        texture_items.append(TextureItem(
            key=key, sym=sym, yaml_stem="ek", fmt=fmt, width=w, height=h,
            tlut_addr=tlut, palette_key=palette_key,
            segment=None, rom_base=None, offset=off,
            compressed=int(typ == "COMPRESSED_TEXTURE"),
        ))
    return texture_items, palette_items, recipe_symbols


def decode_ek_texture(item, source, mio0_decompress):
    """Decode one EK TextureItem to width*height*4 RGBA bytes. `mio0_decompress` is passed in (imported
    by the caller from gen_dump_all_models, never duplicated here). Empirically verifies the archive
    payload's byte length against the format's texel need BEFORE decoding -- the load-bearing framing
    check for EK entries, which carry no header to trust blindly."""
    raw = source.raw(item.key)
    if raw is None:
        raise ValueError("no EK archive entry for %s" % item.key)
    if item.compressed:
        if raw[:4] != b"MIO0":
            raise ValueError("%s: COMPRESSED_TEXTURE entry is not a MIO0 blob (magic %r)"
                             % (item.key, raw[:4]))
        raw = mio0_decompress(raw)
    need = texel_bytes(item.fmt, item.width * item.height)
    if len(raw) < need:
        raise ValueError("%s: payload %dB < needed %dB for %dx%d %s"
                         % (item.key, len(raw), need, item.width, item.height, item.fmt))
    pal = None
    if item.palette_key is not None:
        pal = source.raw(item.palette_key)
        if pal is None:
            raise ValueError("%s: palette entry %s missing from EK archive" % (item.key, item.palette_key))
    return DECODERS[item.fmt](raw, item.width, item.height, pal)


def decode_ek_swatch(key, source):
    """Decode a standalone TLUT row to an Nx1 RGBA swatch (N = archive payload length / 2 palette
    entries, taken from the ACTUAL archive bytes, not the yaml `colors:` hint -- verified one EK TLUT's
    `colors:` field understates its true archive length, e.g. aRecordsInsertDiskToCopyToPalette declares
    `colors: 17` but its archive entry is 424 bytes == 212 entries; the byte length is ground truth).
    This defines the swatch convention (also used for cart TLUTs, see decode_cart_swatch): each entry
    is decoded with the exact RGBA5551-unpack math _dec_rgba16 already uses (a TLUT entry occupies the
    same big-endian 16-bit word as an RGBA16 texel), laid out as a single Nx1 row so the swatch is
    trivially inspectable."""
    raw = source.raw(key)
    if raw is None:
        raise ValueError("no EK archive entry for %s" % key)
    n = len(raw) // 2
    if n == 0:
        raise ValueError("%s: empty TLUT payload" % key)
    return _dec_rgba16(raw, n, 1), n


def decode_cart_swatch(key, source):
    """Cart-side analogue of decode_ek_swatch(): same Nx1 RGBA5551 swatch decode, but sourced through
    ArchiveSource.payload() (which strips the 0x50 OTR+Torch-texture header) instead of the EK raw
    reader. Dumped so the packer can quantize CI replacements back into the original palette
    (gen_texture_pack.py reads the swatch named by the manifest's palette_key column)."""
    raw = source.payload(key)
    if raw is None:
        raise ValueError("no archive entry for %s" % key)
    n = len(raw) // 2
    if n == 0:
        raise ValueError("%s: empty TLUT payload" % key)
    return _dec_rgba16(raw, n, 1), n


# ── AssetBindings.c key table (load-bearing self-check target) ─────────────────────────────────────────
def load_binding_keys(binding_c_path):
    """Every `"category/symbol"` o2r-key literal from the checked-in binding tables."""
    with open(binding_c_path, encoding="utf-8") as fh:
        text = fh.read()
    return set(re.findall(r'"([A-Za-z0-9_]+/[A-Za-z0-9_]+)"', text))


# ── recipe walk (mirrors gen_asset_bindings.generate emission conditions) ─────────────────────────────
class TextureItem:
    __slots__ = ("key", "sym", "yaml_stem", "fmt", "width", "height", "tlut_addr", "palette_key",
                 "segment", "rom_base", "offset", "compressed")

    def __init__(self, **kw):
        for k in self.__slots__:
            setattr(self, k, kw.get(k))


def walk_textures(yaml_dir):
    """Yield a TextureItem for every decodable, canonically-bound texture in the recipe tree.

    Emission mirrors gen_asset_bindings exactly: a texture gets an o2r key (and is therefore emitted)
    only when it is a common_assets_compressed entry with an offset, OR it lives in a yaml carrying a
    `:config: segments:` block (segment_id + rom_base known). Textures in a yaml with no segment config
    (boot_textures, super_textures) receive no binding row and are correctly skipped. Standalone TLUT
    entries and non-texture types (GFX / VTX / BLOB / ARRAY / range-table symbols) never qualify.
    """
    items = []
    for path in sorted(glob.glob(os.path.join(yaml_dir, "*.yaml"))):
        fname = os.path.basename(path)
        yaml_stem = os.path.splitext(fname)[0]
        if fname in gab.BLOB_RECIPE_FILENAMES:
            continue
        is_common = (fname == "common_assets_compressed.yaml")

        with open(path) as fh:
            data = yaml.safe_load(fh) or {}

        config = data.get(":config", {}) or {}
        segs = config.get("segments") or []
        segment_id = rom_base = None
        if segs and isinstance(segs[0], (list, tuple)) and len(segs[0]) >= 2:
            segment_id = int(segs[0][0])
            rom_base = int(segs[0][1])
        compressed = int(bool((config.get("compression") or {}).get("offset") is not None))

        # Palette (TLUT) address map for this yaml: full N64 address -> symbol. CI textures reference a
        # TLUT by its full address (segment<<24 | offset); build the reverse map so the join is exact.
        tlut_by_addr = {}
        for key, val in data.items():
            if not isinstance(val, dict) or str(key).startswith(":"):
                continue
            if val.get("format") == "TLUT" and val.get("offset") is not None:
                off = int(val["offset"])
                full = (segment_id << 24) | off if segment_id is not None else off
                tlut_by_addr[full] = val.get("symbol", key)

        for key, val in data.items():
            if not isinstance(val, dict) or str(key).startswith(":"):
                continue
            if val.get("type") not in ("TEXTURE", "COMPRESSED_TEXTURE"):
                continue
            fmt = val.get("format")
            if fmt not in DECODABLE_FORMATS:
                continue  # skips TLUT-only entries and any non-image texture format
            if val.get("offset") is None:
                continue
            # Generator emission guard: must be common, or belong to a segment-configured yaml.
            if not is_common and (segment_id is None or rom_base is None):
                continue

            sym = val.get("symbol", key)
            o2r_key = "{}/{}".format(yaml_stem, sym)  # SAME string join as gen_asset_bindings

            palette_key = None
            tlut_addr = val.get("tlut")
            if fmt in ("CI4", "CI8"):
                if tlut_addr is None:
                    continue  # a CI texture with no palette cannot be decoded; skip defensively
                pal_sym = tlut_by_addr.get(int(tlut_addr))
                if pal_sym is None:
                    pal_sym = tlut_by_addr.get(int(tlut_addr) & 0x00FFFFFF)
                if pal_sym is None:
                    continue
                palette_key = "{}/{}".format(yaml_stem, pal_sym)

            items.append(TextureItem(
                key=o2r_key, sym=sym, yaml_stem=yaml_stem, fmt=fmt,
                width=int(val["width"]), height=int(val["height"]),
                tlut_addr=int(tlut_addr) if tlut_addr is not None else None,
                palette_key=palette_key,
                segment=segment_id, rom_base=rom_base, offset=int(val["offset"]),
                compressed=compressed,
            ))
    return items


def decode_texture(item, source):
    """Decode one TextureItem to width*height*4 RGBA bytes, or raise ValueError on a hard problem."""
    payload = source.payload(item.key)
    if payload is None:
        # ROM-slice fallback (uncompressed segment entries only; the archive is normally complete).
        need = texel_bytes(item.fmt, item.width * item.height)
        if item.compressed == 0 and item.rom_base is not None:
            payload = source.rom_slice(item.rom_base + item.offset, need)
        if payload is None:
            raise ValueError("no archive entry and no ROM-slice fallback for %s" % item.key)

    need = texel_bytes(item.fmt, item.width * item.height)
    if len(payload) < need:
        raise ValueError("%s: payload %dB < needed %dB for %dx%d %s"
                         % (item.key, len(payload), need, item.width, item.height, item.fmt))

    pal = None
    if item.palette_key is not None:
        pal = source.payload(item.palette_key)
        if pal is None:
            raise ValueError("%s: palette entry %s missing from archive" % (item.key, item.palette_key))
    return DECODERS[item.fmt](payload, item.width, item.height, pal)


# ── manifest.tsv (shape-identical to the in-game dump: key<TAB>w<TAB>h<TAB>fmt, plus a 5th
# palette_key column on CI4/CI8 rows naming the dumped TLUT swatch; "-" when absent) ────────────────
def read_manifest_keys(manifest_path):
    keys = set()
    if not os.path.isfile(manifest_path):
        return keys
    with open(manifest_path, encoding="utf-8") as fh:
        for line in fh:
            line = line.rstrip("\n")
            if not line or line.startswith("#"):
                continue
            parts = line.split("\t")
            if parts:
                keys.add(parts[0])
    return keys


def append_manifest_rows(manifest_path, rows):
    """Append (key, w, h, fmt, palette_key) rows, writing the header comment when the file is new
    (in-game format, extended with the palette_key column)."""
    if not rows:
        return
    is_new = not os.path.isfile(manifest_path)
    with open(manifest_path, "a", encoding="utf-8", newline="\n") as fh:
        if is_new:
            fh.write("# key\tnative_w\tnative_h\tn64_fmt\tpalette_key   (one row per dumped texture;"
                     " palette_key = TLUT swatch key for CI4/CI8, - otherwise)\n")
        for key, w, h, fmt, palette_key in rows:
            fh.write("%s\t%d\t%d\t%s\t%s\n" % (key, w, h, fmt, palette_key))


# ── per-tile atlas band slicing (key scheme 2) ───────────────────────────────────────────────────
# Buffers the game loads as one tall strip and samples per-tile (devdocs/POST_1_0_SCOPING.md
# "Per-tile atlas overrides"). key -> (tile_w, tile_h); tiles stack vertically, so band i starts at
# byte offset i * texel_bytes(fmt, tile_w * tile_h) — the offset the runtime's containing-range
# lookup keys on. Mirrors kAtlasTileGeometry in torch/src/gdx/dump_textures.cpp.
ATLAS_TILE_GEOMETRY = {
    "machine_custom_gfx/aTimerSymbolsTex": (8, 16),
    "machine_custom_gfx/aSpeedDigitsTex": (12, 16),
}


def dump_atlas_bands(it, rgba, out_dir, existing):
    """Emit the scheme-2 "atlas/<baseKey>/o<byteOffset>/<FMT>/<W>x<H>" rows for a dumped atlas
    strip: the whole-atlas key (o0 at full W x H — the band geometry whole-atlas LoadTextureBlock
    loads produce at runtime) plus one sliced PNG + row per tile. `rgba` is the freshly decoded
    whole-atlas RGBA bytes, or None on the already-dumped path (the base PNG is read back, so
    re-running this tooling over a pre-scheme-2 dump still produces the band slices). Returns
    (rows, written) with rows ready for append_manifest_rows."""
    tile_w, tile_h = ATLAS_TILE_GEOMETRY[it.key]
    if it.width != tile_w or tile_h <= 0 or it.height % tile_h != 0:
        sys.stderr.write("  warn: %s: atlas geometry %dx%d tiles do not tile %dx%d - no band rows\n"
                         % (it.key, tile_w, tile_h, it.width, it.height))
        return [], 0
    if rgba is None:
        try:
            img = Image.open(os.path.join(out_dir, it.key + ".png"))
            img.load()
            if img.size != (it.width, it.height):
                return [], 0
            rgba = img.convert("RGBA").tobytes()
        except Exception:  # noqa: BLE001 - unreadable base PNG: no bands, base row already reported
            return [], 0
    base = "atlas/%s" % it.key
    band_bytes = texel_bytes(it.fmt, tile_w * tile_h)
    row_stride = it.width * 4
    # (byte offset, w, h, first source row); the whole-atlas key comes first.
    candidates = [(0, it.width, it.height, 0)]
    for i in range(it.height // tile_h):
        candidates.append((i * band_bytes, tile_w, tile_h, i * tile_h))
    rows = []
    written = 0
    for byte_off, w, h, row0 in candidates:
        key = "%s/o%d/%s/%dx%d" % (base, byte_off, it.fmt, w, h)
        if key in existing:
            continue
        png_path = os.path.join(out_dir, key + ".png")
        if not os.path.exists(png_path):
            band = bytearray()
            for y in range(row0, row0 + h):
                band += rgba[y * row_stride:(y + 1) * row_stride]
            os.makedirs(os.path.dirname(png_path), exist_ok=True)
            Image.frombytes("RGBA", (w, h), bytes(band)).save(png_path)
            written += 1
        rows.append((key, w, h, it.fmt, it.palette_key or "-"))
    return rows, written


# ── dump class registry ───────────────────────────────────────────────────────────────────────────────
class DumpContext:
    def __init__(self, profile, yaml_dir, binding_c, source, dump_dir, binding_keys,
                 ek_archive=None, ek_manifest=None, ek_yaml_dir=None):
        self.profile = profile
        self.yaml_dir = yaml_dir
        self.binding_c = binding_c
        self.source = source
        self.dump_dir = dump_dir
        self.binding_keys = binding_keys
        # EK (Expansion Kit) resolution -- independent of --profile (the 64DD disk archive/recipe tree
        # is the same regardless of the chosen cart profile); may be None when the EK build artifacts
        # are not present (dev machine without an EK build), in which case EK dumping is skipped, not
        # hard-failed (mirrors gen_dump_all_models.py's `models` EK handling).
        self.ek_archive = ek_archive
        self.ek_manifest = ek_manifest
        self.ek_yaml_dir = ek_yaml_dir


class DumpClass:
    """Base class for one dumpable asset kind. Subclasses set `name` / `subdir` and implement collect()
    + a decode/compare surface. `subdir` places output under dump/<subdir>/ ("" == the pack-compatible
    dump root that gen_texture_pack.py consumes)."""
    name = "?"
    subdir = ""

    def out_dir(self, ctx):
        return ctx.dump_dir if not self.subdir else os.path.join(ctx.dump_dir, self.subdir)


class TextureDumpClass(DumpClass):
    name = "textures"
    subdir = ""  # textures live at the dump root so tools/gen_texture_pack.py consumes it unchanged

    def collect(self, ctx):
        return walk_textures(ctx.yaml_dir)

    # -- key self-check (load-bearing check a) --
    def self_check_keys(self, ctx, items):
        bad = [it.key for it in items if it.key not in ctx.binding_keys]
        return bad

    # -- EK key self-check: same idea, EK-side ground truth (see ek_recipe_index() docstring for why
    # the recipe tree, not EkAssetBindings.c string-grepping, is the right EK analogue) --
    def self_check_ek_keys(self, texture_items, palette_items, recipe_symbols):
        bad = [it.key for it in texture_items if it.sym not in recipe_symbols]
        bad += [key for key, sym in palette_items if sym not in recipe_symbols]
        return bad

    # -- EK resolution: archive + manifest + recipe tree, all optional (graceful skip when the EK build
    # artifacts are absent, same posture as gen_dump_all_models.py's `models` EK handling). Imports
    # gen_dump_all_models locally (not at module scope) to reuse its MIO0 decoder + EK archive candidate
    # list without duplicating either -- safe because gen_dump_all.py already fully imports that sibling
    # module before any DumpClass method runs (see the import-after-registry block at end of file); the
    # local import here just binds the (already-loaded) module object, same pattern round_trip() uses
    # for gen_texture_pack. --
    def _ek_ready(self, ctx):
        if ctx.ek_archive is None or not os.path.isfile(ctx.ek_archive):
            print("  ek textures: no EK disk archive found -- skipping EK texture dump")
            return None
        if ctx.ek_manifest is None or not os.path.isfile(ctx.ek_manifest):
            print("  ek textures: no EK slice manifest found -- skipping EK texture dump")
            return None
        if ctx.ek_yaml_dir is None or not os.path.isdir(ctx.ek_yaml_dir):
            print("  ek textures: no EK asset recipe tree found -- skipping EK texture dump")
            return None
        import gen_dump_all_models as gda_models
        texture_items, palette_items, recipe_symbols = walk_ek_textures(ctx.ek_yaml_dir, ctx.ek_manifest)
        source = EkArchiveSource(ctx.ek_archive)
        return source, texture_items, palette_items, recipe_symbols, gda_models.mio0_decompress

    def run(self, ctx):
        items = self.collect(ctx)
        # Palette keys are bound symbols too (TLUT rows carry a binding row, same as textures), so
        # they join the self-check alongside the texture keys that reference them.
        palette_keys = sorted({it.palette_key for it in items if it.palette_key})
        bad = self.self_check_keys(ctx, items)
        bad += [key for key in palette_keys if key not in ctx.binding_keys]
        if bad:
            raise SystemExit("KEY SELF-CHECK FAILED: %d emitted key(s) absent from %s (e.g. %s)"
                             % (len(bad), os.path.relpath(ctx.binding_c, REPO), ", ".join(bad[:5])))
        print("  key self-check PASS: all %d emitted keys present in %s"
              % (len(items) + len(palette_keys), os.path.relpath(ctx.binding_c, REPO)))

        out_dir = self.out_dir(ctx)
        os.makedirs(out_dir, exist_ok=True)
        manifest_path = os.path.join(out_dir, "manifest.tsv")
        existing = read_manifest_keys(manifest_path)

        dumped = skipped = failed = 0
        atlas_dumped = 0
        new_rows = []
        for it in sorted(items, key=lambda x: x.key):
            png_path = os.path.join(out_dir, it.key + ".png")
            rgba = None
            if os.path.exists(png_path):  # idempotent + first-seen-wins across runs and play sessions
                skipped += 1
                if it.key not in existing:
                    new_rows.append((it.key, it.width, it.height, it.fmt, it.palette_key or "-"))
            else:
                try:
                    rgba = decode_texture(it, ctx.source)
                except ValueError as exc:
                    sys.stderr.write("  warn: %s\n" % exc)
                    failed += 1
                    continue
                os.makedirs(os.path.dirname(png_path), exist_ok=True)
                Image.frombytes("RGBA", (it.width, it.height), rgba).save(png_path)
                dumped += 1
                if it.key not in existing:
                    new_rows.append((it.key, it.width, it.height, it.fmt, it.palette_key or "-"))
            if it.key in ATLAS_TILE_GEOMETRY:
                a_rows, a_written = dump_atlas_bands(it, rgba, out_dir, existing)
                new_rows.extend(a_rows)
                atlas_dumped += a_written
        # Cart TLUT swatches (mirror of the EK palette dump below): every palette a dumped cart CI
        # texture references lands as an Nx1 RGBA swatch PNG + a TLUT manifest row, so the packer can
        # quantize CI replacements back into the original palette offline.
        for key in palette_keys:
            png_path = os.path.join(out_dir, key + ".png")
            if os.path.exists(png_path):
                skipped += 1
                if key not in existing:
                    sw, sh = Image.open(png_path).size
                    new_rows.append((key, sw, sh, "TLUT", "-"))
                continue
            try:
                rgba, n = decode_cart_swatch(key, ctx.source)
            except ValueError as exc:
                sys.stderr.write("  warn: %s\n" % exc)
                failed += 1
                continue
            os.makedirs(os.path.dirname(png_path), exist_ok=True)
            Image.frombytes("RGBA", (n, 1), rgba).save(png_path)
            dumped += 1
            if key not in existing:
                new_rows.append((key, n, 1, "TLUT", "-"))
        append_manifest_rows(manifest_path, new_rows)
        print("  textures: %d dumped, %d skipped (already present), %d failed -> %s"
              % (dumped, skipped, failed, os.path.relpath(out_dir, os.getcwd()) if out_dir.startswith(os.getcwd()) else out_dir))
        if atlas_dumped:
            print("  atlas bands: %d per-tile slice(s) written (key scheme 2)" % atlas_dumped)
        result = {"class": self.name, "dumped": dumped, "skipped": skipped, "failed": failed,
                  "total": len(items) + len(palette_keys)}

        ek = self._ek_ready(ctx)
        if ek is None:
            result.update({"ek_dumped": 0, "ek_skipped": 0, "ek_failed": 0, "ek_total": 0})
            return result
        source, texture_items, palette_items, recipe_symbols, mio0_decompress = ek

        ek_bad = self.self_check_ek_keys(texture_items, palette_items, recipe_symbols)
        if ek_bad:
            raise SystemExit("EK KEY SELF-CHECK FAILED: %d emitted key(s) absent from the EK recipe "
                             "tree (%s) (e.g. %s)"
                             % (len(ek_bad), os.path.relpath(ctx.ek_yaml_dir, REPO), ", ".join(ek_bad[:5])))
        print("  EK key self-check PASS: all %d emitted keys present in the EK recipe tree (%s)"
              % (len(texture_items) + len(palette_items), os.path.relpath(ctx.ek_yaml_dir, REPO)))

        ek_dumped = ek_skipped = ek_failed = 0
        ek_new_rows = []
        for it in sorted(texture_items, key=lambda x: x.key):
            png_path = os.path.join(out_dir, it.key + ".png")
            if os.path.exists(png_path):
                ek_skipped += 1
                if it.key not in existing:
                    ek_new_rows.append((it.key, it.width, it.height, it.fmt, it.palette_key or "-"))
                continue
            try:
                rgba = decode_ek_texture(it, source, mio0_decompress)
            except ValueError as exc:
                sys.stderr.write("  warn: %s\n" % exc)
                ek_failed += 1
                continue
            os.makedirs(os.path.dirname(png_path), exist_ok=True)
            Image.frombytes("RGBA", (it.width, it.height), rgba).save(png_path)
            ek_dumped += 1
            if it.key not in existing:
                ek_new_rows.append((it.key, it.width, it.height, it.fmt, it.palette_key or "-"))
        for key, sym in sorted(palette_items):
            png_path = os.path.join(out_dir, key + ".png")
            if os.path.exists(png_path):
                ek_skipped += 1
                if key not in existing:
                    # width (palette entry count) isn't a static field anywhere -- read it back from the
                    # already-dumped swatch (cheap: 1-row PNG) instead of re-touching the archive.
                    sw, sh = Image.open(png_path).size
                    ek_new_rows.append((key, sw, sh, "TLUT", "-"))
                continue
            try:
                rgba, n = decode_ek_swatch(key, source)
            except ValueError as exc:
                sys.stderr.write("  warn: %s\n" % exc)
                ek_failed += 1
                continue
            os.makedirs(os.path.dirname(png_path), exist_ok=True)
            Image.frombytes("RGBA", (n, 1), rgba).save(png_path)
            ek_dumped += 1
            if key not in existing:
                ek_new_rows.append((key, n, 1, "TLUT", "-"))
        append_manifest_rows(manifest_path, ek_new_rows)
        print("  ek textures: %d dumped, %d skipped (already present), %d failed -> %s"
              % (ek_dumped, ek_skipped, ek_failed,
                 os.path.relpath(out_dir, os.getcwd()) if out_dir.startswith(os.getcwd()) else out_dir))
        result.update({"ek_dumped": ek_dumped, "ek_skipped": ek_skipped, "ek_failed": ek_failed,
                       "ek_total": len(texture_items) + len(palette_items)})
        return result

    # -- offline == runtime acceptance test (load-bearing check b) --
    def verify(self, ctx):
        items = self.collect(ctx)
        out_dir = self.out_dir(ctx)
        overlap = matched = diverged = missing = errored = 0
        divergences = []
        for it in sorted(items, key=lambda x: x.key):
            png_path = os.path.join(out_dir, it.key + ".png")
            if not os.path.exists(png_path):
                missing += 1
                continue
            overlap += 1
            try:
                rgba = decode_texture(it, ctx.source)
            except ValueError as exc:
                errored += 1
                divergences.append("%s: decode error (%s)" % (it.key, exc))
                continue
            on_disk = Image.open(png_path).convert("RGBA")
            if on_disk.size != (it.width, it.height):
                diverged += 1
                divergences.append("%s: size %s != offline %dx%d"
                                   % (it.key, on_disk.size, it.width, it.height))
                continue
            if on_disk.tobytes() == rgba:
                matched += 1
            else:
                diverged += 1
                divergences.append("%s: pixel mismatch" % it.key)

        ek = self._ek_ready(ctx)
        if ek is not None:
            source, texture_items, palette_items, _recipe_symbols, mio0_decompress = ek
            for it in sorted(texture_items, key=lambda x: x.key):
                png_path = os.path.join(out_dir, it.key + ".png")
                if not os.path.exists(png_path):
                    missing += 1
                    continue
                overlap += 1
                try:
                    rgba = decode_ek_texture(it, source, mio0_decompress)
                except ValueError as exc:
                    errored += 1
                    divergences.append("%s: decode error (%s)" % (it.key, exc))
                    continue
                on_disk = Image.open(png_path).convert("RGBA")
                if on_disk.size != (it.width, it.height):
                    diverged += 1
                    divergences.append("%s: size %s != offline %dx%d"
                                       % (it.key, on_disk.size, it.width, it.height))
                    continue
                if on_disk.tobytes() == rgba:
                    matched += 1
                else:
                    diverged += 1
                    divergences.append("%s: pixel mismatch" % it.key)
            for key, _sym in sorted(palette_items):
                png_path = os.path.join(out_dir, key + ".png")
                if not os.path.exists(png_path):
                    missing += 1
                    continue
                overlap += 1
                try:
                    rgba, n = decode_ek_swatch(key, source)
                except ValueError as exc:
                    errored += 1
                    divergences.append("%s: decode error (%s)" % (key, exc))
                    continue
                on_disk = Image.open(png_path).convert("RGBA")
                if on_disk.size != (n, 1):
                    diverged += 1
                    divergences.append("%s: size %s != offline %dx1" % (key, on_disk.size, n))
                    continue
                if on_disk.tobytes() == rgba:
                    matched += 1
                else:
                    diverged += 1
                    divergences.append("%s: pixel mismatch" % key)

        return {"overlap": overlap, "matched": matched, "diverged": diverged,
                "missing": missing, "errored": errored, "divergences": divergences}

    # -- decoder proof (round-trip): decode -> re-encode via gen_texture_pack -> byte-compare source --
    def round_trip(self, ctx, count):
        import gen_texture_pack as gtp
        # Non-CI only: CI encoders take the palette as a 4th argument (quantize-into-original-palette)
        # and duplicated palette colors make their byte-compare non-deterministic under lowest-index
        # tie-breaking.
        items = [it for it in self.collect(ctx) if it.fmt in gtp.ENCODERS and it.fmt not in gtp.CI_FORMATS]
        results = []
        for it in sorted(items, key=lambda x: x.key):
            if len(results) >= count:
                break
            src_payload = ctx.source.payload(it.key)
            if src_payload is None:
                continue
            need = texel_bytes(it.fmt, it.width * it.height)
            src_texels = src_payload[:need]
            try:
                rgba = decode_texture(it, ctx.source)
            except ValueError:
                continue
            img = Image.frombytes("RGBA", (it.width, it.height), rgba)
            reencoded, _ttype = gtp.ENCODERS[it.fmt](img.load(), it.width, it.height)
            ok = (bytes(reencoded) == bytes(src_texels))
            results.append((it.key, it.fmt, it.width, it.height, ok))
        return results


CLASS_REGISTRY = {
    TextureDumpClass.name: TextureDumpClass,
    # Step 4 slots additional kinds in here, one class each, no other wiring:
    #   "models":     ModelDumpClass,      # DLs + vertices -> OBJ/glTF   (subdir "models")
    #   "audio":      SampleDumpClass,     # VADPCM -> WAV                (subdir "audio")
    #   "midi":       SequenceDumpClass,   # sequences -> MIDI            (subdir "music")
}

# Tier-1 typed dumps (coursedata/dlists/vertexdata/tables) + Tier-2 ghosts/fonts: registered from
# gen_dump_all_extra.py so this file's architecture stays untouched (imported lazily, after
# CLASS_REGISTRY exists, to avoid a circular import -- gen_dump_all_extra imports THIS module).
import gen_dump_all_extra as gda_extra  # noqa: E402
CLASS_REGISTRY.update(gda_extra.EXTRA_CLASSES)

# audio (VADPCM -> WAV) + midi (sequences -> MIDI): registered from gen_dump_all_audio.py, same
# import-after-registry pattern (that module imports gen_dump_all_extra to reuse its table parser).
import gen_dump_all_audio as gda_audio  # noqa: E402
CLASS_REGISTRY.update(gda_audio.EXTRA_AUDIO_CLASSES)

# models (F3DEX2 display lists + vertices + texture bindings -> Wavefront OBJ/MTL): registered from
# gen_dump_all_models.py, same import-after-registry pattern (that module imports gen_dump_all_extra to
# reuse the F3DEX2 opcode table + ROM/manifest helpers).
import gen_dump_all_models as gda_models  # noqa: E402
CLASS_REGISTRY.update(gda_models.MODEL_CLASSES)


def build_context(args):
    yaml_dir, binding_c = gab.resolve_paths(args.profile, None)
    if not os.path.isdir(yaml_dir):
        raise SystemExit("profile %s: asset yaml dir not found: %s" % (args.profile, yaml_dir))
    if not os.path.isfile(binding_c):
        raise SystemExit("profile %s: binding file not found (run gen_asset_bindings.py first): %s"
                         % (args.profile, binding_c))
    if not os.path.isfile(args.archive):
        raise SystemExit("archive not found: %s" % args.archive)
    source = ArchiveSource(args.archive, args.rom)
    binding_keys = load_binding_keys(binding_c)

    # EK resolution: same candidate-list-based auto-discovery gen_dump_all_models.py already uses for
    # the disk archive/manifest (imported, not duplicated), each overridable via its own CLI flag so
    # nothing here is a hardcoded path -- profile-agnostic (the EK disk is not part of the --profile
    # cart tree; it is the one, JP-only, 64DD disk regardless of the cart profile chosen).
    import gen_dump_all_models as gda_models
    ek_archive = args.ek_archive or gda_models._find_first_existing(gda_models._EK_ARCHIVE_CANDIDATES)
    ek_manifest = args.ek_manifest or gda_models._EK_MANIFEST
    ek_yaml_dir = args.ek_yaml_dir or EK_YAML_DIR

    return DumpContext(args.profile, yaml_dir, binding_c, source, args.dump_dir, binding_keys,
                       ek_archive=ek_archive, ek_manifest=ek_manifest, ek_yaml_dir=ek_yaml_dir)


def resolve_classes(spec):
    names = [c.strip() for c in spec.split(",") if c.strip()]
    unknown = [n for n in names if n not in CLASS_REGISTRY]
    if unknown:
        raise SystemExit("unknown/unimplemented class(es): %s (available: %s)"
                         % (", ".join(unknown), ", ".join(sorted(CLASS_REGISTRY))))
    return [CLASS_REGISTRY[n]() for n in names]


def main():
    ap = argparse.ArgumentParser(
        description="Offline Dump All — decode named game assets from the extracted archive (no game).",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="Examples:\n"
               "  gen_dump_all.py --classes textures\n"
               "  gen_dump_all.py --classes textures --verify        # offline == runtime acceptance test\n"
               "  gen_dump_all.py --classes textures --round-trip 3   # prove the decoder is exact\n")
    ap.add_argument("--classes", default="textures",
                    help="comma-separated dump classes to run (default: textures). Only `textures` is "
                         "implemented; the registry is shaped for models/audio/midi/... later.")
    ap.add_argument("--profile", default="us/rev0", choices=["us/rev0", "jp/rev0"],
                    help="asset recipe profile (recipe tree + binding file); default us/rev0")
    ap.add_argument("--archive", default=os.path.join(REPO, "assets", "extracted", "generic.o2r"),
                    help="extracted per-asset archive (.o2r) to source texel bytes from")
    ap.add_argument("--rom", default=None, help="optional raw ROM for the uncompressed-slice fallback")
    ap.add_argument("--dump-dir", default=os.path.join(os.getcwd(), "dump"),
                    help="output dump directory (default ./dump; textures land at its root)")
    ap.add_argument("--ek-archive", default=None,
                    help="64DD Expansion Kit disk archive (fzerox-disk.o2r); default: auto-discovered "
                         "from the same build-tree candidate list gen_dump_all_models.py uses")
    ap.add_argument("--ek-manifest", default=None,
                    help="EK slice manifest; default port/gen/ek_slice_manifest.txt")
    ap.add_argument("--ek-yaml-dir", default=None,
                    help="EK asset recipe tree (for CI8->TLUT palette resolution + the EK key "
                         "self-check); default fzerox-expansion-kit/assets/yaml/jp")
    ap.add_argument("--verify", action="store_true",
                    help="do not write; decode each key whose PNG already exists in --dump-dir and "
                         "compare decoded pixels (offline == runtime acceptance test)")
    ap.add_argument("--round-trip", type=int, default=0, metavar="N",
                    help="decode N non-CI textures, re-encode via gen_texture_pack, and byte-compare "
                         "against the source archive bytes (0 = off)")
    ap.add_argument("--list-classes", action="store_true",
                    help="print one registered dump-class name per line and exit (for tooling/UI)")
    args = ap.parse_args()

    # --list-classes: trivial machine-readable enumeration for the in-game UI / external tooling.
    if args.list_classes:
        for name in sorted(CLASS_REGISTRY):
            print(name)
        return 0

    ctx = build_context(args)
    classes = resolve_classes(args.classes)
    print("profile %s | archive %s | dump %s"
          % (args.profile, os.path.relpath(args.archive, REPO), args.dump_dir))

    rc = 0
    if args.verify:
        for cls in classes:
            if not hasattr(cls, "verify"):
                print("  %s: no verify support" % cls.name)
                continue
            r = cls.verify(ctx)
            print("  %s verify: %d overlapping key(s), %d matched, %d diverged, %d decode-error, "
                  "%d not-yet-dumped" % (cls.name, r["overlap"], r["matched"], r["diverged"],
                                          r["errored"], r["missing"]))
            for line in r["divergences"][:20]:
                print("    DIVERGENCE: %s" % line)
            if r["diverged"] or r["errored"]:
                rc = 1
        return rc

    if args.round_trip > 0:
        for cls in classes:
            if not hasattr(cls, "round_trip"):
                continue
            print("  %s round-trip (decode -> re-encode -> compare source bytes):" % cls.name)
            for key, fmt, w, h, ok in cls.round_trip(ctx, args.round_trip):
                print("    %-58s %-7s %4dx%-4d %s" % (key, fmt, w, h,
                      "BYTE-IDENTICAL" if ok else "MISMATCH"))
                if not ok:
                    rc = 1

    for cls in classes:
        print("[%s]" % cls.name)
        cls.run(ctx)
    return rc


if __name__ == "__main__":
    sys.exit(main())
