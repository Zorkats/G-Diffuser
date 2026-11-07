# Copyright (c) Zorkats
"""N64 texel decoders shared by the offline dump (gen_dump_all.py) and the pack unpacker (unpack_pack.py).

Each decoder mirrors the matching libultraship Fast3D Interpreter::ImportTexture* body, so a decoded
image matches what the game draws. gen_texture_pack.py owns the encoders; these are their inverse.
Standard library only.
"""

import array
import sys

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


# ── table decoders ────────────────────────────────────────────────────────────────────────────────────
# The per-texel loops above are the reference. decode() gives the same bytes much faster: it builds one
# table per format by running the reference decoder over every possible texel word, then maps the
# payload through that table. A 4-bit row is padded to a whole byte, as gen_texture_pack.py packs it;
# for an even width that is the same as the tight packing the reference decoders read.
_TABLES = {}


def _table(fmt):
    table = _TABLES.get(fmt)
    if table is None:
        dec = DECODERS[fmt]
        bits = BITS_PER_TEXEL[fmt]
        if bits == 16:
            table = [dec(bytes((v >> 8, v & 0xFF)), 1, 1) for v in range(65536)]
        elif bits == 8:
            table = [dec(bytes((v,)), 1, 1) for v in range(256)]
        else:
            table = [dec(bytes((v,)), 2, 1) for v in range(256)]  # two texels per byte
        _TABLES[fmt] = table
    return table


def row_bytes(fmt, width):
    """Bytes per row of `width` texels, with a 4-bit row padded to a whole byte."""
    return (width * BITS_PER_TEXEL[fmt] + 7) // 8


def _map_rows(table, data, fmt, width, height, words):
    stride = row_bytes(fmt, width)
    if len(data) < stride * height:
        raise ValueError("%s %dx%d needs %d bytes, the payload has %d"
                         % (fmt, width, height, stride * height, len(data)))
    if BITS_PER_TEXEL[fmt] == 4 and width % 2:
        rows = []
        for y in range(height):
            row = data[y * stride:(y + 1) * stride]
            rows.append(b"".join(map(table.__getitem__, row))[:width * 4])
        return b"".join(rows)
    data = data[:stride * height]
    if words:
        values = array.array("H")
        values.frombytes(data)
        if sys.byteorder == "little":
            values.byteswap()  # texel words are big-endian
        return b"".join(map(table.__getitem__, values))
    return b"".join(map(table.__getitem__, data))


def decode(fmt, payload, width, height):
    """RGBA bytes (width*height*4) for a non-paletted texture, equal to DECODERS[fmt] for even widths."""
    if fmt not in DECODABLE_FORMATS or fmt in ("CI4", "CI8"):
        raise ValueError("decode() takes a non-paletted format, not %s" % fmt)
    data = bytes(payload)
    if fmt == "RGBA32":
        need = width * height * 4
        if len(data) < need:
            raise ValueError("RGBA32 %dx%d needs %d bytes, the payload has %d" % (width, height, need, len(data)))
        return data[:need]
    return _map_rows(_table(fmt), data, fmt, width, height, BITS_PER_TEXEL[fmt] == 16)


def decode_indexed(fmt, payload, width, height, colors=None):
    """RGBA bytes for a CI4 or CI8 texture. colors lists (r, g, b, a) per palette index, as the game's
    dump writes its palette swatch. Without colors the index is shown as an opaque grey level."""
    if fmt not in ("CI4", "CI8"):
        raise ValueError("decode_indexed() takes CI4 or CI8, not %s" % fmt)
    count = 16 if fmt == "CI4" else 256
    if colors is None:
        step = 17 if fmt == "CI4" else 1
        entries = [bytes((i * step, i * step, i * step, 255)) for i in range(count)]
    else:
        entries = [bytes(tuple(colors[i])[:4]) if i < len(colors) else b"\x00\x00\x00\x00" for i in range(count)]
    if fmt == "CI8":
        table = entries
    else:
        table = [entries[v >> 4] + entries[v & 0xF] for v in range(256)]
    return _map_rows(table, bytes(payload), fmt, width, height, False)
