#!/usr/bin/env python3
"""Convert a GLideN64 hi-res texture cache (.htc) into a G-Diffuser .o2r texture pack.

Quick start:

    python -I tools/gen_htc_pack.py PACK.htc --base fzerox.o2r --base fzerox-disk.o2r OUT.o2r --name "My HD Pack" --author you

    # plan and report only, writes no pack:
    python -I tools/gen_htc_pack.py PACK.htc --base fzerox.o2r --name "My HD Pack" --author you --check

Matching:
  Every native OTEX texture in the --base archives gets its Rice CRC, the N64 texel checksum that
  GLideN64 uses as the low 32 bits of its cache key. The high 32 bits are a palette CRC, 0 for
  non-CI textures. .htc entries are matched on the low 32 bits. When several --base archives hold
  the same path, the later one wins, as the game mounts them in order.

Output:
  Each match becomes an OTEX v1 resource at textures/pack/<path>, the default override channel, in
  one of two encodings (the report's "encoding" field):
    native  RGBA16, IA8 and IA4 base textures are re-encoded in their own format at the pack size,
            with hByteScale and vPixelScale set to the whole-number upscale per axis, flags 0. This
            is the renderer's scaled HD pipeline (IsScaledLinearTexture, interpreter.cpp), the one
            gen_texture_pack.py packs use. It serves partial loads too: one glyph out of a digit
            strip, one tile out of an atlas. RGBA16 is 5551, so alpha becomes 1 bit and colour
            5 bits per channel.
    raw     every other format (CI4, CI8, I4, ...) becomes RGBA32 with TEX_FLAG_LOAD_AS_RAW
            (flags 1). derive_scale sets the scale fields so ImportTextureRaw uploads it directly.
            That branch only lines up for a load of the whole image from its first texel, so a raw
            texture the game loads in pieces comes out striped.
            One-row strips (the 64x1 skybox gradients) are raw whatever their type: the game loads
            them whole and stretches them across the screen, where 5551 colour shows as bands.
  A pack size that is not a whole multiple of the native size is box averaged to round(ratio)
  times the native size and marked resampled in the report. An axis that is one texel long in the
  base texture stays one texel long (see pack_scale).

Palette variants:
  A CI base texture (CI4 or CI8) the .htc holds under palette CRCs gets raw RGBA32 images named by
  palette, at textures/pack/<path>.pal/<CC>-<XXXXXXXX>: CC is the highest colour index the texture
  uses (2 hex digits) and XXXXXXXX the palette CRC (8 hex digits). Two cases:
    several palette CRCs  one variant per palette and no plain textures/pack/<path> entry. At draw
                          time the game hashes the loaded palette the same way (port/gdx_palette_crc.c)
                          and uses the matching image, or the stock texture when none matches, as
                          GLideN64 does.
    one palette CRC       the plain textures/pack/<path> override, as for any texture, plus one
                          variant for that palette. Both carry the same bytes. The game uses the
                          variant when the drawn palette matches and the plain override otherwise.
  Both cases need a colour index of 1 or more (cimax at least 1). A texture that only uses index 0
  is skipped (ci_palette_variants) when the .htc holds it under several palettes, and keeps only
  the plain override when it has one.

Channels:
  default        textures/pack/<path>. The game consults it for textures drawn by resource path and
                 for whole images copied into RDRAM (common_assets_compressed/*).
  --alt          alt/textures/pack/<path>. Used only while Workshop > Use alternate assets is on,
                 so a player can switch between the pack and stock without a reload.
  --direct-keys  the bare <path>, shadowing the base resource. Whole images copied into RDRAM at
                 load keep their stock bytes, because an upscaled replacement does not fit the
                 stock buffer, so prefer the default. Palette variants stay under textures/pack/,
                 since the bare path is the stock resource.

Skipped .htc entries carry one reason code:
  no_match             no base texture has this Rice CRC
  unsupported_format   not GL_RGBA with GL_UNSIGNED_BYTE
  ci_palette_variants  a CI base texture with several palette variants only uses colour index 0, so no
                       palette CRC can tell them apart (GLideN64 hashes at least two colours)
  duplicate            a later entry with the same key; the first in file order wins
  bad_data             truncated entry, zlib data that does not inflate, or a size that is not w*h*4
  unsupported_layout   the base texture's rows do not fill whole N64 tile lines, so the direct branch
                       cannot hit exactly (see read_base_texture)

The output is deterministic: sorted entries, fixed 1980 timestamps, and the zip layout and metadata
rules of tools/gen_texture_pack.py. --check writes no pack; --json still writes its report.
"""
import argparse
import gzip
import hashlib
import json
import os
import struct
import sys
import types
import zipfile
import zlib

TOOLS_DIR = os.path.dirname(os.path.abspath(__file__))


def load_packer():
    """Import tools/gen_texture_pack.py. That module imports Pillow when it loads, and this tool
    never decodes PNGs, so a stand-in PIL module is installed only when Pillow is missing."""
    if TOOLS_DIR not in sys.path:
        sys.path.insert(0, TOOLS_DIR)  # python -I keeps the script directory off sys.path
    try:
        import PIL  # noqa: F401
    except ImportError:
        pil = types.ModuleType("PIL")
        pil.Image = types.ModuleType("PIL.Image")
        sys.modules.setdefault("PIL", pil)
        sys.modules.setdefault("PIL.Image", pil.Image)
    import gen_texture_pack
    return gen_texture_pack


packer = load_packer()

GL_RGBA = 0x1908
GL_UNSIGNED_BYTE = 0x1401
HTC_VERSIONED_MARKER = 0x08000000
HTC_ZLIB_FLAG = 0x80000000
TEX_FLAG_LOAD_AS_RAW = 1  # libultraship Texture.h
ALT_PREFIX = "alt/"
PACK_PREFIX = "textures/pack/"
PACK_CHANNEL = "pack"
DIRECT_CHANNEL = "direct"
ALT_CHANNEL = "alt"
# Palette variants of one CI texture: <pack path>.pal/<cimax, 2 hex digits>-<palette CRC, 8 hex digits>.
# The game reads the same names (port/gdx_palette_crc.h, gdx_palette_variant_parse).
PALETTE_SUFFIX = ".pal/"
REASONS = ("no_match", "unsupported_format", "ci_palette_variants", "duplicate", "bad_data",
           "unsupported_layout")

# Fast::TextureType -> (name, bits per texel, N64 size code used by the Rice CRC).
NATIVE_INFO = {
    packer.TT_RGBA32: ("RGBA32", 32, 3),
    packer.TT_RGBA16: ("RGBA16", 16, 2),
    packer.TT_IA16: ("IA16", 16, 2),
    packer.TT_PAL8: ("CI8", 8, 1),
    packer.TT_I8: ("I8", 8, 1),
    packer.TT_IA8: ("IA8", 8, 1),
    packer.TT_PAL4: ("CI4", 4, 0),
    packer.TT_I4: ("I4", 4, 0),
    packer.TT_IA4: ("IA4", 4, 0),
}
CI_TYPES = (packer.TT_PAL4, packer.TT_PAL8)
NATIVE_ENCODING = "native"
RAW_ENCODING = "raw"
# Native types the renderer's scaled HD pipeline accepts (IsScaledLinearTexture,
# libultraship/src/fast/interpreter.cpp), with the packer's encoder for each.
NATIVE_HD_ENCODERS = {
    packer.TT_RGBA16: packer.enc_rgba16,
    packer.TT_IA8: packer.enc_ia8,
    packer.TT_IA4: packer.enc_ia4,
}


def rice_crc(buf, width, height, size, stride):
    """Rice CRC over an N64 texel block, as GLideN64 computes it (rewritten, not copied).
    Returns None when a row is narrower than one word or the buffer is too short."""
    bpl = (width << size) >> 1
    if bpl < 4:
        return None
    crc = 0
    y = height - 1
    off = 0
    while y >= 0:
        x = bpl - 4
        esi = 0
        if off + bpl > len(buf):
            return None
        while x >= 0:
            esi = struct.unpack_from(">I", buf, off + x)[0] ^ x
            crc = ((crc << 4) | (crc >> 28)) & 0xFFFFFFFF
            crc = (crc + esi) & 0xFFFFFFFF
            x -= 4
        esi ^= y
        crc = (crc + esi) & 0xFFFFFFFF
        off += stride
        y -= 1
    return crc


def make_entry(index, key, width, height, texfmt, pixtype, compressed, payload, reason, detail):
    return {
        "index": index,
        "key": key,
        "low": None if key is None else key & 0xFFFFFFFF,
        "high": None if key is None else key >> 32,
        "width": width,
        "height": height,
        "texfmt": texfmt,
        "pixtype": pixtype,
        "compressed": compressed,
        "payload": payload,
        "reason": reason,
        "detail": detail,
    }


def decode_htc_payload(entry):
    """Return the RGBA8 bytes of an entry, inflating zlib data when flagged. Raises ValueError."""
    if entry["compressed"]:
        try:
            data = zlib.decompress(entry["payload"])
        except zlib.error as exc:
            raise ValueError("zlib data does not inflate (%s)" % exc)
    else:
        data = entry["payload"]
    width, height = entry["width"], entry["height"]
    if width == 0 or height == 0:
        raise ValueError("zero texture size %dx%d" % (width, height))
    want = width * height * 4
    if len(data) != want:
        raise ValueError("%d bytes for %dx%d RGBA8 (expected %d)" % (len(data), width, height, want))
    return data


def classify_entry(entry):
    """Set the reason when the format is not GL_RGBA8 or the payload does not decode."""
    if entry["texfmt"] != GL_RGBA or entry["pixtype"] != GL_UNSIGNED_BYTE:
        entry["reason"] = "unsupported_format"
        entry["detail"] = "texture format 0x%04X, pixel type 0x%04X" % (entry["texfmt"], entry["pixtype"])
        return
    try:
        decode_htc_payload(entry)
    except ValueError as exc:
        entry["reason"] = "bad_data"
        entry["detail"] = str(exc)


def parse_htc(raw):
    """Parse a .htc cache file. Returns (versioned, entries).

    Each entry is a dict: index, key, low, high, width, height, texfmt, pixtype, compressed,
    payload, reason, detail. reason is None for a usable entry. Raises ValueError when the gzip
    container is bad. A truncated entry is recorded as bad_data and ends the scan, because the
    entries after it cannot be located."""
    try:
        data = gzip.decompress(raw)
    except (OSError, EOFError, zlib.error) as exc:
        raise ValueError("not a gzip-compressed .htc (%s)" % exc)
    if len(data) < 4:
        raise ValueError(".htc container is too short to hold a header")
    versioned = struct.unpack_from("<I", data, 0)[0] == HTC_VERSIONED_MARKER
    pos = 8 if versioned else 4  # a versioned file stores a config word after the marker
    formatsize = 2 if versioned else 0  # versioned entries carry a u16 formatsize
    head = struct.Struct("<QIIIHHB")
    body = memoryview(data)
    entries = []
    while pos < len(data):
        index = len(entries)
        if pos + head.size + formatsize + 4 > len(data):
            entries.append(make_entry(index, None, 0, 0, 0, 0, False, b"", "bad_data",
                                      "truncated entry header"))
            break
        key, width, height, fmt, texfmt, pixtype, _is_hires = head.unpack_from(data, pos)
        p = pos + head.size + formatsize
        dsz = struct.unpack_from("<I", data, p)[0]
        p += 4
        if p + dsz > len(data):
            entries.append(make_entry(index, key, width, height, texfmt, pixtype, False, b"",
                                      "bad_data", "payload of %d bytes runs past the end" % dsz))
            break
        entry = make_entry(index, key, width, height, texfmt, pixtype, bool(fmt & HTC_ZLIB_FLAG),
                           body[p:p + dsz], None, "")
        classify_entry(entry)
        entries.append(entry)
        pos = p + dsz
    return versioned, entries


def read_base_texture(name, data, archive):
    """Describe one base resource if it is a native OTEX texture (v0 or v1), else None.

    layout_ok is True only when every row fills whole N64 tile lines: the native row is
    width * bits / 8 bytes, a multiple of 8 (16 for RGBA32, whose tile line is doubled at
    interpreter.cpp 1994-1995). Padded tile lines (interpreter.cpp 4757-4759) break the direct
    branch arithmetic, so those textures keep their CRC but cannot be converted."""
    if len(data) < packer.OTR_HEADER_SIZE + 4:
        return None
    if struct.unpack_from("<I", data, 4)[0] != packer.OTR_TYPE_TEXTURE:
        return None
    version = struct.unpack_from("<I", data, 8)[0]
    try:
        if version == 0:
            tex_type, width, height, size = struct.unpack_from("<IIII", data, 64)
            start = 80
        elif version == 1:
            tex_type, width, height, _flags, _hs, _vs, size = struct.unpack_from("<IIIIffI", data, 64)
            start = 92
        else:
            return None
    except struct.error:
        return None
    if tex_type not in NATIVE_INFO or width == 0 or height == 0:
        return None
    type_name, bits, siz = NATIVE_INFO[tex_type]
    payload = data[start:start + size]
    unit = 16 if tex_type == packer.TT_RGBA32 else 8
    row_bits = width * bits
    line = row_bits // 8 if row_bits % 8 == 0 else None
    if line is None:
        layout_detail = "%d texels of %d bits do not fill whole bytes" % (width, bits)
    elif line % unit != 0:
        layout_detail = "rows are %d bytes, not a multiple of %d (N64 tile line)" % (line, unit)
    elif size != line * height or len(payload) != size:
        layout_detail = "payload is %d bytes, expected %d" % (len(payload), line * height)
    else:
        layout_detail = ""
    stride = (width << siz) >> 1
    return {
        "path": name,
        "archive": archive,
        "type": tex_type,
        "type_name": type_name,
        "is_ci": tex_type in CI_TYPES,
        "width": width,
        "height": height,
        "crc": rice_crc(payload, width, height, siz, stride),
        "cimax": ci_max(tex_type, payload),
        "layout_ok": layout_detail == "",
        "layout_detail": layout_detail,
    }


def ci_max(tex_type, payload):
    """Highest colour index a CI texture uses, as GLideN64 finds it (CalculateMaxCI4b and
    CalculateMaxCI8b, rewritten, not copied); None for other types. The palette CRC covers the
    entries 0..cimax, so the texture decides how much of the palette counts."""
    if tex_type == packer.TT_PAL4:
        return max((max(b >> 4, b & 0xF) for b in payload), default=0)
    if tex_type == packer.TT_PAL8:
        return max(payload, default=0)
    return None


def palette_crc(palette, entries):
    """GLideN64's palette CRC: RiceCRC32 over entries 16-bit colours as one row, read the way
    GLideN64 reads its RDRAM copy (rewritten, not copied).

    GLideN64 keeps RDRAM as host 32-bit words, so host byte i is N64 byte i ^ 3, and RiceCRC32 reads
    little-endian words at x = row bytes - 4, - 8, ... down to x >= 0. With an odd entry count the
    row is 2 mod 4 bytes long, every read is half a word off, and its bytes come from two N64 words.
    palette is the N64-order palette (32 bytes for CI4, 512 for CI8). Returns None when entries is
    below 2 (GLideN64 would read before the palette) or the reads run past the palette: the first
    read covers host bytes row - 4 .. row - 1, so it touches N64 bytes up to (row - 1) | 3."""
    row = entries * 2
    if entries < 2 or ((row - 1) | 3) >= len(palette):
        return None
    crc = 0
    esi = 0
    x = row - 4
    while x >= 0:
        esi = 0
        for k in range(4):
            esi |= palette[(x + k) ^ 3] << (8 * k)
        esi ^= x
        crc = ((crc << 4) | (crc >> 28)) & 0xFFFFFFFF
        crc = (crc + esi) & 0xFFFFFFFF
        x -= 4
    return (crc + esi) & 0xFFFFFFFF  # esi ^ y with y = 0: one row


def read_base_textures(archive_paths):
    """Index every native OTEX texture in the base archives by archive path. A later archive
    replaces an earlier one for the same path. Returns (textures, seen)."""
    textures = {}
    seen = 0
    for archive in archive_paths:
        with zipfile.ZipFile(archive, "r") as z:
            for name in z.namelist():
                rec = read_base_texture(name, z.read(name), os.path.basename(archive))
                if rec is None:
                    continue
                seen += 1
                textures[name] = rec
    return textures, seen


def plan_conversion(entries, textures):
    """Match usable .htc entries to base textures.

    Returns (matches, skipped). matches is a list of {path, base, entry} dicts sorted by path, then
    palette. A palette variant also carries palette and cimax, and a CI texture under one palette CRC
    has two matches (the plain override and its variant) that share one entry.
    skipped has one dict per .htc entry that is not converted, in file order."""
    by_crc = {}
    for path in sorted(textures):
        rec = textures[path]
        if rec["crc"] is not None:
            by_crc.setdefault(rec["crc"], []).append(rec)
    usable_by_low = {}
    for entry in entries:
        if entry["reason"] is None and entry["low"] in by_crc:
            usable_by_low.setdefault(entry["low"], []).append(entry)

    decided = {}  # htc index -> (reason, detail, native type name); "converted" when matched
    matches = []
    for low in sorted(usable_by_low):
        cands = usable_by_low[low]
        recs = by_crc[low]
        usable = [r for r in recs if r["layout_ok"]]
        if not usable:
            detail = "base texture %s: %s" % (recs[0]["path"], recs[0]["layout_detail"])
            for e in cands:
                decided[e["index"]] = ("unsupported_layout", detail, recs[0]["type_name"])
            continue
        is_ci = any(r["is_ci"] for r in usable)
        highs = sorted(set(e["high"] for e in cands))
        if is_ci and len(highs) > 1:
            plan_palette_variants(cands, usable, decided, matches)
            continue
        winner = cands[0]
        for e in cands[1:]:
            decided[e["index"]] = ("duplicate", "same key as entry %d" % winner["index"],
                                   usable[0]["type_name"])
        for rec in usable:
            matches.append({"path": rec["path"], "base": rec, "entry": winner})
            if rec["is_ci"] and rec["cimax"] >= 1:
                # One palette CRC: the plain override also ships as the variant for that palette.
                matches.append(variant_match(rec, winner, winner["high"]))
        decided[winner["index"]] = ("converted", "", usable[0]["type_name"])
    matches.sort(key=lambda m: (m["path"], m.get("palette", -1)))

    skipped = []
    for entry in entries:
        if entry["reason"] is not None:
            skipped.append(skip_row(entry, entry["reason"], entry["detail"], None))
            continue
        info = decided.get(entry["index"])
        if info is None:
            skipped.append(skip_row(entry, "no_match", "no base texture has this Rice CRC", None))
        elif info[0] != "converted":
            skipped.append(skip_row(entry, info[0], info[1], info[2]))
    return matches, skipped


def variant_match(rec, entry, palette):
    """A palette variant of the base record rec, carried by the .htc entry that holds its palette."""
    return {"path": rec["path"], "base": rec, "entry": entry, "palette": palette, "cimax": rec["cimax"]}


def plan_palette_variants(cands, usable, decided, matches):
    """One variant per palette for a CI base texture the .htc holds under several palette CRCs.

    GLideN64 keys a CI texture on its texels and the palette it is drawn with, so each palette CRC
    (the key's high 32 bits) gets its own image, written at <path>.pal/<cimax>-<palette CRC>. The
    game picks the one whose CRC matches the palette loaded at draw time, and draws stock when none
    does. The first entry per palette CRC wins; later ones are duplicates. Only CI records with a
    cimax of 1 or more get variants: a non-CI record that shares the Rice CRC has no palette, and
    gets no match from this group."""
    type_name = usable[0]["type_name"]
    ci = [rec for rec in usable if rec["is_ci"] and rec["cimax"] >= 1]
    if not ci:
        detail = "palette CRC needs at least two colours (the texture only uses index 0)"
        for e in cands:
            decided[e["index"]] = ("ci_palette_variants", detail, type_name)
        return
    winners = {}
    for e in cands:
        winner = winners.get(e["high"])
        if winner is not None:
            decided[e["index"]] = ("duplicate", "same key as entry %d" % winner["index"], type_name)
            continue
        winners[e["high"]] = e
        for rec in ci:
            matches.append(variant_match(rec, e, e["high"]))
        decided[e["index"]] = ("converted", "", type_name)


def palette_leaf(cimax, palette):
    """The variant name under <path>.pal/: cimax as 2 hex digits, a dash, the palette CRC as 8."""
    return "%02X-%08X" % (cimax, palette)


def skip_row(entry, reason, detail, native_type):
    return {
        "htc_index": entry["index"],
        "key": None if entry["key"] is None else "0x%016X" % entry["key"],
        "reason": reason,
        "native_type": native_type,
        "detail": detail,
    }


def pack_scale(pack_w, pack_h, native_w, native_h):
    """Whole-number upscale per axis: round(ratio), at least 1x (round half up).

    An axis that is one texel long in the base texture stays 1x. The game wraps such a texture
    along that axis (the 64x1 skybox strips), so every native texel row looks the same. Upscaled
    rows that differ from each other would repeat as bands, so they are averaged into one."""
    kw = 1 if native_w == 1 else max(1, (2 * pack_w + native_w) // (2 * native_w))
    kh = 1 if native_h == 1 else max(1, (2 * pack_h + native_h) // (2 * native_h))
    return kw, kh


def match_encoding(tex_type, pack_w, native_h=None):
    """NATIVE_ENCODING when the scaled HD pipeline can take this base type at this pack width, else
    RAW_ENCODING. The pipeline needs every row to fill whole bytes (ScaledTextureDimensions).

    A base texture one row tall is always RAW_ENCODING. Those are the skybox strips, which
    background.c loads whole from their first texel and stretches across the screen, so the raw
    upload lines up and keeps 8 bits per channel where 5551 would band a dark gradient."""
    if native_h == 1 or tex_type not in NATIVE_HD_ENCODERS:
        return RAW_ENCODING
    _name, bits, _siz = NATIVE_INFO[tex_type]
    return NATIVE_ENCODING if (pack_w * bits) % 8 == 0 else RAW_ENCODING


def derive_scale(tex_type, native_w, native_h, pack_w, pack_h):
    """Return (hByteScale, vPixelScale) for a raw RGBA32 replacement of a native texture of tex_type
    at pack_w x pack_h. Native encodings use the plain upscale instead (describe_match).

    The direct branch of ImportTextureRaw (libultraship interpreter.cpp 1992-2005) computes
      resultOrigLineSize = tile line_size_bytes, doubled for 32b (1992-1997)
      resultOrigHeight   = orig_size_bytes / resultOrigLineSize (1998)
      resultNewLineSize  = resultOrigLineSize * hByteScale (1999)
      resultNewHeight    = resultOrigHeight * vPixelScale (2000)
    and uploads the replacement directly when resultNewLineSize == 4 * pack_w and
    resultNewHeight == pack_h. The tile line is line * 8 (GfxDpSetTile, 4423) and orig_size_bytes
    is the native texel count in bytes (GfxDpLoadBlock, 4713-4714), so the native line is
    native_w * bits / 8 bytes over native_h rows. Solving gives
      hByteScale = 4 * pack_w / line   and   vPixelScale = pack_h / native_h.
    For a whole-number upscale kw this is 32 * kw / bits: RGBA32 kw, RGBA16 and IA16 2 * kw,
    8-bit (CI8, I8, IA8) 4 * kw, 4-bit (CI4, I4, IA4) 8 * kw.
    """
    name, bits, _siz = NATIVE_INFO[tex_type]
    if (native_w * bits) % 8 != 0:
        raise ValueError("%s rows of %d texels are not whole bytes" % (name, native_w))
    line = native_w * bits // 8
    return float(4 * pack_w) / float(line), float(pack_h) / float(native_h)


def describe_match(match):
    """Add the pack size, encoding, scale fields and resample flag to a match."""
    base, entry = match["base"], match["entry"]
    kw, kh = pack_scale(entry["width"], entry["height"], base["width"], base["height"])
    pack_w, pack_h = kw * base["width"], kh * base["height"]
    encoding = match_encoding(base["type"], pack_w, base["height"])
    if encoding == NATIVE_ENCODING:
        # The scaled HD pipeline reads both fields as texel multiples (ScaledTextureDimensions).
        h_scale, v_scale = float(kw), float(kh)
    else:
        h_scale, v_scale = derive_scale(base["type"], base["width"], base["height"], pack_w, pack_h)
    match.update({
        "encoding": encoding,
        "pack_w": pack_w,
        "pack_h": pack_h,
        "h_scale": h_scale,
        "v_scale": v_scale,
        "resampled": (pack_w, pack_h) != (entry["width"], entry["height"]),
    })
    return match


def area_taps(src_len, dst_len):
    """For each destination pixel, the (source index, weight) pairs it covers. Weights are exact
    integer overlaps of the source and destination pixel spans, so one destination pixel's
    weights sum to src_len."""
    taps = []
    for j in range(dst_len):
        lo = j * src_len
        hi = lo + src_len
        row = []
        i = lo // dst_len
        while i < src_len and i * dst_len < hi:
            a = max(lo, i * dst_len)
            b = min(hi, (i + 1) * dst_len)
            if b > a:
                row.append((i, b - a))
            i += 1
        taps.append(row)
    return taps


def resample_rgba(src, sw, sh, dw, dh):
    """Box-average an RGBA8 image (rows top-down) from sw x sh to dw x dh.

    Integer arithmetic only: a horizontal pass, then a vertical pass, divided by sw * sh and
    rounded half up. Colour and alpha are averaged alike (no premultiplication)."""
    xtaps = area_taps(sw, dw)
    ytaps = area_taps(sh, dh)
    rows = []
    for sy in range(sh):
        base = sy * sw * 4
        cols = []
        for taps in xtaps:
            acc = [0, 0, 0, 0]
            for sx, wgt in taps:
                p = base + sx * 4
                acc[0] += src[p] * wgt
                acc[1] += src[p + 1] * wgt
                acc[2] += src[p + 2] * wgt
                acc[3] += src[p + 3] * wgt
            cols.append(acc)
        rows.append(cols)
    div = sw * sh
    out = bytearray(dw * dh * 4)
    o = 0
    for taps in ytaps:
        for x in range(dw):
            acc = [0, 0, 0, 0]
            for sy, wgt in taps:
                cell = rows[sy][x]
                acc[0] += cell[0] * wgt
                acc[1] += cell[1] * wgt
                acc[2] += cell[2] * wgt
                acc[3] += cell[3] * wgt
            for c in range(4):
                out[o + c] = (2 * acc[c] + div) // (2 * div)
            o += 4
    return bytes(out)


def key_channel(args):
    if args.alt:
        return ALT_CHANNEL
    return DIRECT_CHANNEL if args.direct_keys else PACK_CHANNEL


def archive_path(path, channel, palette=None, cimax=None):
    """Where a replacement for the base resource at path sits in the pack.

    A palette variant goes to <pack path>.pal/<leaf>. --direct-keys has no bare form for variants
    (the bare path is the stock resource), so they use the default textures/pack/ prefix."""
    if palette is not None:
        prefix = ALT_PREFIX + PACK_PREFIX if channel == ALT_CHANNEL else PACK_PREFIX
        return prefix + path + PALETTE_SUFFIX + palette_leaf(cimax, palette)
    if channel == DIRECT_CHANNEL:
        return path
    if channel == ALT_CHANNEL:
        return ALT_PREFIX + PACK_PREFIX + path
    return PACK_PREFIX + path


class RgbaPixels:
    """px[x, y] -> (r, g, b, a) over RGBA8 rows, the pixel access the packer's encoders expect."""

    def __init__(self, rgba, width):
        self.rgba = rgba
        self.width = width

    def __getitem__(self, xy):
        p = (xy[1] * self.width + xy[0]) * 4
        return tuple(self.rgba[p:p + 4])


def encode_matches(matches, channel):
    """Encode every match as an OTEX v1 resource. Returns (archive path, bytes) pairs sorted by path."""
    out = []
    for m in matches:
        entry = m["entry"]
        rgba = decode_htc_payload(entry)
        if m["resampled"]:
            rgba = resample_rgba(rgba, entry["width"], entry["height"], m["pack_w"], m["pack_h"])
        arc = archive_path(m["path"], channel, m.get("palette"), m.get("cimax"))
        if m["encoding"] == NATIVE_ENCODING:
            encoder = NATIVE_HD_ENCODERS[m["base"]["type"]]
            data, tex_type = encoder(RgbaPixels(rgba, m["pack_w"]), m["pack_w"], m["pack_h"])
            blob = packer.build_otex_v1(tex_type, m["pack_w"], m["pack_h"], m["h_scale"], m["v_scale"], data)
        else:
            blob = packer.build_otex_v1(packer.TT_RGBA32, m["pack_w"], m["pack_h"], m["h_scale"],
                                        m["v_scale"], bytes(rgba), flags=TEX_FLAG_LOAD_AS_RAW)
        out.append((arc, blob))
    out.sort(key=lambda e: e[0])
    return out


def pack_metadata(args):
    """Metadata from the packer's own synthesizer, with key_channel set for this pack."""
    shim = argparse.Namespace(
        manifest=os.devnull, input_dir=None, output_o2r=args.output_o2r,
        name=args.name, author=args.author, version=args.version,
        game_version=None, key_scheme_version=None, id=None, direct_keys=args.direct_keys)
    raw, _source = packer.resolve_manifest_bytes(shim, {})
    manifest = json.loads(raw.decode("utf-8"))
    manifest["key_channel"] = key_channel(args)
    return manifest


def write_pack(out_path, entries, manifest):
    """Write the packer's zip layout: sorted entries, fixed 1980 timestamps, and workshop.json
    with a files[] sha256 for every payload entry."""
    manifest = dict(manifest)
    manifest["files"] = [{"path": arc, "sha256": hashlib.sha256(data).hexdigest()}
                         for arc, data in sorted(entries)]
    manifest_bytes = json.dumps(manifest, indent=2, sort_keys=True).encode("utf-8")
    arc_entries = list(entries) + [("workshop.json", manifest_bytes)]
    arc_entries.sort(key=lambda e: e[0])
    with zipfile.ZipFile(out_path, "w", zipfile.ZIP_DEFLATED) as z:
        for arc, data in arc_entries:
            info = zipfile.ZipInfo(arc, date_time=packer.FIXED_DATE)
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o644 << 16
            z.writestr(info, data)


def build_report(args, versioned, entries, textures, seen, matches, skipped, pack_bytes):
    by_reason = {r: 0 for r in REASONS}
    by_reason_and_type = {r: {} for r in REASONS}
    for row in skipped:
        by_reason[row["reason"]] += 1
        type_key = row["native_type"] or "none"
        counts = by_reason_and_type[row["reason"]]
        counts[type_key] = counts.get(type_key, 0) + 1
    matched_by_type = {}
    for m in matches:
        name = m["base"]["type_name"]
        matched_by_type[name] = matched_by_type.get(name, 0) + 1
    matched_entries = set(m["entry"]["index"] for m in matches)
    resampled = sum(1 for m in matches if m["resampled"])
    native = sum(1 for m in matches if m["encoding"] == NATIVE_ENCODING)
    variants = sum(1 for m in matches if m.get("palette") is not None)
    layout_bad = sum(1 for t in textures.values() if not t["layout_ok"])
    with_crc = sum(1 for t in textures.values() if t["crc"] is not None)
    return {
        "htc": os.path.basename(args.htc),
        "htc_versioned": versioned,
        "bases": [os.path.basename(p) for p in args.base],
        "key_channel": key_channel(args),
        "output": None if args.check else os.path.basename(args.output_o2r),
        "totals": {
            "htc_entries": len(entries),
            "matched_entries": len(matched_entries),
            "matched_paths": len(matches),
            "resampled_paths": resampled,
            "native_paths": native,
            "raw_paths": len(matches) - native,
            "palette_variant_paths": variants,
            "skipped_entries": len(skipped),
            "base_native_textures_read": seen,
            "base_textures": len(textures),
            "base_textures_with_crc": with_crc,
            "base_textures_unsupported_layout": layout_bad,
            "pack_bytes": pack_bytes,
        },
        "by_reason": by_reason,
        "by_reason_and_type": by_reason_and_type,
        "matched_by_type": matched_by_type,
        "matched": [{
            "path": m["path"],
            "archive": m["base"]["archive"],
            "native_type": m["base"]["type_name"],
            "native_width": m["base"]["width"],
            "native_height": m["base"]["height"],
            "encoding": m["encoding"],
            "pack_width": m["pack_w"],
            "pack_height": m["pack_h"],
            "h_byte_scale": m["h_scale"],
            "v_pixel_scale": m["v_scale"],
            "resampled": m["resampled"],
            "htc_index": m["entry"]["index"],
            "key": "0x%016X" % m["entry"]["key"],
            "palette": None if m.get("palette") is None else palette_leaf(m["cimax"], m["palette"]),
        } for m in matches],
        "skipped": skipped,
    }


def print_summary(report, pack_bytes):
    totals = report["totals"]
    print("htc: %s (%d entries, %s layout)" % (report["htc"], totals["htc_entries"],
                                               "versioned" if report["htc_versioned"] else "original"))
    print("bases: %d archive(s), %d native texture(s) read, %d unique, %d with a Rice CRC, %d unsupported layout"
          % (len(report["bases"]), totals["base_native_textures_read"], totals["base_textures"],
             totals["base_textures_with_crc"], totals["base_textures_unsupported_layout"]))
    print("matched: %d entry(ies) -> %d path(s) (%d native, %d raw RGBA32, %d palette variant(s)), %d resampled"
          % (totals["matched_entries"], totals["matched_paths"], totals["native_paths"],
             totals["raw_paths"], totals["palette_variant_paths"], totals["resampled_paths"]))
    if report["matched_by_type"]:
        print("  by native type: " + ", ".join("%s=%d" % kv for kv in sorted(report["matched_by_type"].items())))
    print("skipped: %d entry(ies): " % totals["skipped_entries"]
          + ", ".join("%s=%d" % (r, report["by_reason"][r]) for r in REASONS))
    if pack_bytes is None:
        print("check: nothing written")
    else:
        print("wrote %s (%d bytes, key_channel %s)" % (report["output"], pack_bytes, report["key_channel"]))


def main(argv=None):
    ap = argparse.ArgumentParser(
        description="Convert a GLideN64 .htc texture cache into a G-Diffuser .o2r texture pack",
        epilog="Examples:\n"
               "  gen_htc_pack.py PACK.htc --base fzerox.o2r --base fzerox-disk.o2r OUT.o2r --name \"My HD Pack\" --author you\n"
               "  gen_htc_pack.py PACK.htc --base fzerox.o2r --name \"My HD Pack\" --author you --check",
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("htc", help="GLideN64 .htc texture cache")
    ap.add_argument("output_o2r", nargs="?", default=None, help="output .o2r path (omit with --check)")
    ap.add_argument("--base", action="append", default=[], metavar="ARCHIVE.o2r",
                    help="base archive to match against; repeat for several (later archives win)")
    ap.add_argument("--name", required=True, help="pack name for the metadata")
    ap.add_argument("--author", required=True, help="pack author for the metadata")
    ap.add_argument("--version", default=None, help="pack version for the metadata (default 0.1)")
    channel = ap.add_mutually_exclusive_group()
    channel.add_argument("--alt", action="store_true",
                         help="write alt/textures/pack/<path>, used while Use alternate assets is on")
    channel.add_argument("--direct-keys", action="store_true",
                         help="write the bare <path>, shadowing the base resource (advanced)")
    ap.add_argument("--check", action="store_true", help="plan and report only; write no pack")
    ap.add_argument("--json", default=None, metavar="REPORT.json", help="write a structured report")
    args = ap.parse_intermixed_args(argv)

    if not args.base:
        sys.stderr.write("error: at least one --base archive is required\n")
        return 2
    if not args.check and not args.output_o2r:
        sys.stderr.write("error: output_o2r is required (or pass --check to plan only)\n")
        return 2
    if not os.path.isfile(args.htc):
        sys.stderr.write("error: .htc file not found: %s\n" % args.htc)
        return 2
    for path in args.base:
        if not os.path.isfile(path):
            sys.stderr.write("error: base archive not found: %s\n" % path)
            return 2

    try:
        with open(args.htc, "rb") as fh:
            versioned, entries = parse_htc(fh.read())
    except ValueError as exc:
        sys.stderr.write("error: %s\n" % exc)
        return 2
    try:
        textures, seen = read_base_textures(args.base)
    except (zipfile.BadZipFile, OSError) as exc:
        sys.stderr.write("error: base archive could not be read (%s)\n" % exc)
        return 2

    matches, skipped = plan_conversion(entries, textures)
    for m in matches:
        describe_match(m)
    if not args.check and not matches:
        sys.stderr.write("error: nothing to pack (no .htc entry matched a base texture); nothing written\n")
        return 2

    pack_bytes = None
    if not args.check:
        try:
            manifest = pack_metadata(args)
        except ValueError as exc:
            sys.stderr.write("error: %s\n" % exc)
            return 2
        out_dir = os.path.dirname(os.path.abspath(args.output_o2r))
        os.makedirs(out_dir, exist_ok=True)
        write_pack(args.output_o2r, encode_matches(matches, key_channel(args)), manifest)
        pack_bytes = os.path.getsize(args.output_o2r)

    report = build_report(args, versioned, entries, textures, seen, matches, skipped, pack_bytes)
    if args.json:
        with open(args.json, "w", encoding="utf-8") as fh:
            json.dump(report, fh, indent=2, sort_keys=True)
            fh.write("\n")
    print_summary(report, pack_bytes)
    print("OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
