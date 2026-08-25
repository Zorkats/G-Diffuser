#!/usr/bin/env python3
"""Bake custom F-Zero X machine models into a deterministic .o2r model pack.

Quick start:

    # scaffold an editable model work dir from a dumped stock machine:
    python tools/gen_model_pack.py init blue_falcon -o my-falcon

    # edit my-falcon/lod1.obj (and optionally add lod2..lod6 + textures/), then:
    python tools/gen_model_pack.py pack my-falcon -o my-falcon.o2r --name "My Falcon" --author you

    # validate a work dir or an existing pack without writing anything:
    python tools/gen_model_pack.py check my-falcon
    python tools/gen_model_pack.py check my-falcon.o2r

Work-dir layout (what `init` scaffolds and `pack`/`check` consume):

    <dir>/
      model.json       {"machine": "blue_falcon", "scale": 1.0,
                        "lods": {"1": "lod1.obj", ...}, "materials": {}}
      lod<N>.obj       Wavefront OBJ, N in 1..6. Vertex colors are MANDATORY
      lod<N>.mtl       (every `v` line carries r g b, optional 4th = alpha).
      textures/        PNGs referenced by map_Kd (relative to the .mtl).
      workshop.json    pack metadata (synthesized from --name/--author/... if absent).

Pack layout inside the .o2r (one resource per entry):

    models/pack/machine/<machine>/lod<N>        ODLT v0 display list
    models/pack/machine/<machine>/lod<N>.vtx    OVTX v0 vertex blob (shared by the lod's materials)
    models/pack/machine/<machine>/tex/<material> OTEX v1 texture (deduped across lods)
    workshop.json                                metadata + files[] sha256 manifest

Rules enforced (validated as a whole; every problem is listed before failing):
  - Budget: each LOD stays within the stock Lod1 triangle budget (106 tris — the max over
    the 30 dumped D_800CDAD8 machine models in dump/models/manifest.tsv, blue_falcon's
    D_9001210). Over budget is a hard error; --allow-overbudget demotes it to a warning.
  - The game's 64-vertex cache is respected: G_VTX loads are chunked <=32 verts into
    alternating banks, and a triangle whose verts cannot fit the cache window is a hard error.
  - Textures must be power-of-two and fit the 4 KiB TMEM tile budget
    (RGBA16: w*h*2 <= 4096; RGBA32: w*h*4 <= 4096).
  - Positions and texture coords must fit the N64 s16 ranges after scaling; never clamped.
  - Faces: triangles and quads (fan-split) only; ngons are a hard error.

Materials:
  - map_Kd <file>.png  -> textured with the stock "envbody" combine
      (0, ENVIRONMENT, TEXEL0, ENVIRONMENT / COMBINED, 0, SHADE, 0 — decomp
      src/overlays/ovl_i9/machine_draw.c:974), so the game's per-machine gDPSetEnvColor
      body recolor keeps working. RGBA16 by default; per-material override in
      model.json: "materials": {"<mat>": {"format": "rgba32"}}.
  - a material named *--flat -> flat PRIMITIVE-color combine with gDPSetPrimColor from
    the MTL Kd (alpha from `d`, default opaque).
  - neither -> untextured env combine (ENV * SHADE, the machine_draw.c untextured family)
    with a warning. NEVER put a white texture under the envbody template to fake this:
    (0 - ENV) * TEXEL0 + ENV collapses to black when TEXEL0 is white.

Wire-format ground truth (mirrored, not re-derived):
  - OTR 64-byte resource header: libultraship ResourceLoader::ReadResourceInitDataBinary
    (byte0 endianness 0=Little, byte1 is_custom=1, LE u32 type @4, LE u32 version @8).
  - ODLT v0 ('ODLT', version 0): int8 ucode (ucode_f3dex2 = 4), 0xFF pad to 8-byte absolute
    alignment, then LE u32 word pairs; 128-bit expanded hash commands per
    torch DisplayListFactory.cpp:186-220 and torch/src/n64/gbi-otr.h:132. A G_MARKER naming
    the resource's own path hash is prepended, matching torch output.
  - OVTX v0 ('OVTX', version 0): LE u32 count, then count x 16-byte records
    (s16 ob[3], u16 flag=0, s16 tc[2] S10.5, u8 cn[4]) — torch VertexFactory.cpp:18-33.
  - OTEX v1: identical to tools/gen_texture_pack.py build_otex_v1/enc_rgba16 (mirrored
    below; gen_texture_pack cannot be imported because it hard-requires PIL at module top).
  - Resource hashes: CRC64 exactly as libultraship/src/ship/utils/StrHash64.cpp:176-182
    (CRC-64/XZ MSB-first table, init 0xFFFFFFFFFFFFFFFF, NO final complement).
  - LOD display lists contain only mesh data and per-material state (combine,
    texture, prim color). Global RDP setup is the caller's responsibility, just
    like the stock Machine_Draw*Lod1 functions.

Known deviations from the stock game (accepted):
  - The dump has no per-LOD meshes (the runtime per-LOD display lists are built into BSS at
    boot), so `init` scaffolds lod1 only; lods 2-6 must be authored by hand and any missing
    LOD falls back to the stock model at runtime (warning at pack/check time).
  - The emitted gSPTexture(0xFFFF, 0xFFFF) scale gives the rendered U = tc - 1 (the same
    1/32-texel shift the stock machine rendering exhibits); documented, not compensated.
  - CI4/CI8 textures are not supported: pack textures are self-contained with no game TLUT,
    and stock machine body textures are RGBA16. RGBA16/RGBA32 only.
"""
import argparse
import hashlib
import json
import os
import re
import struct
import sys
import zipfile
import zlib

# ---------------------------------------------------------------------------
# OTR resource header (libultraship ResourceLoader::ReadResourceInitDataBinary)
# ---------------------------------------------------------------------------
OTR_HEADER_SIZE = 64
OTR_BYTE_ORDER_LITTLE = 0
OTR_IS_CUSTOM = 1
OTR_TYPE_DISPLAYLIST = 0x4F444C54  # 'ODLT'
OTR_TYPE_VERTEX = 0x4F565458       # 'OVTX'
OTR_TYPE_TEXTURE = 0x4F544558      # 'OTEX'
OTR_TEXTURE_VERSION_V1 = 1
UCODE_F3DEX2 = 4  # torch ucodehandlers.h

# Fast::TextureType (libultraship/include/fast/resource/type/Texture.h)
TT_RGBA32 = 1
TT_RGBA16 = 2

# F3DEX2 opcodes (decomp/include/PR/gbi.h)
G_TRI1 = 0x05
G_TRI2 = 0x06
G_TEXTURE = 0xD7
G_LOADSYNC = 0xE6
G_PIPESYNC = 0xE7
G_SETTILESIZE = 0xF2
G_LOADBLOCK = 0xF3
G_SETTILE = 0xF5
G_SETPRIMCOLOR = 0xFA
G_SETCOMBINE = 0xFC
G_ENDDL = 0xDF

# OTR expanded (128-bit) opcodes: torch/src/n64/gbi-otr.h + libultraship/include/fast/lus_gbi.h
G_SETTIMG_OTR_HASH = 0x20
G_DL_OTR_HASH = 0x31
G_VTX_OTR_HASH = 0x32
G_MARKER = 0x33
G_BRANCH_Z_OTR = 0x35
G_MTX_OTR = 0x36
G_MOVEMEM_OTR = 0x42
EXPANDED_OPCODES = {G_SETTIMG_OTR_HASH, G_DL_OTR_HASH, G_VTX_OTR_HASH,
                    G_MARKER, G_BRANCH_Z_OTR, G_MTX_OTR, G_MOVEMEM_OTR}
# Native opcodes this baker emits; check uses the union as the parse whitelist.
KNOWN_NATIVE_OPCODES = {G_TRI1, G_TRI2, G_TEXTURE, G_LOADSYNC, G_PIPESYNC,
                        G_SETTILESIZE, G_LOADBLOCK, G_SETTILE, G_SETPRIMCOLOR,
                        G_SETCOMBINE, G_ENDDL}

G_IM_FMT_RGBA = 0
G_IM_SIZ_16b = 1
G_IM_SIZ_32b = 2
G_TX_LOADTILE = 7
G_TX_RENDERTILE = 0

FIXED_DATE = (1980, 1, 1, 0, 0, 0)

# Stock Lod1 triangle budget: max `faces` over the 30 machine_models rows of
# dump/models/manifest.tsv that D_800CDAD8 references (blue_falcon D_9001210, 106).
STOCK_LOD1_TRI_BUDGET = 106

TMEM_TILE_BYTES = 4096

# Machine table: character enum order (decomp/include/fzx_game.h:113-143) mapped to the
# dumped D_800CDAD8 model symbols (decomp/src/game/racer.c:122-126, names from the
# sMachineDrawFuncs comments, racer.c:234-299).
MACHINES = {
    "blue_falcon": (0, "D_9001210"),
    "golden_fox": (1, "D_9001DA0"),
    "wild_goose": (2, "D_90027D0"),
    "fire_stingray": (3, "D_9003050"),
    "white_cat": (4, "D_9003870"),
    "red_gazelle": (5, "D_9003F90"),
    "great_star": (6, "D_900CF48"),
    "iron_tiger": (7, "D_90057A8"),
    "deep_claw": (8, "D_90061A0"),
    "twin_noritta": (9, "D_9006A70"),
    "super_piranha": (10, "D_90078F0"),
    "mighty_hurricane": (11, "D_9008060"),
    "little_wyvern": (12, "D_90089A0"),
    "space_angler": (13, "D_9009358"),
    "green_panther": (14, "D_9009980"),
    "black_bull": (15, "D_900A150"),
    "wild_boar": (16, "D_900AC40"),
    "astro_robin": (17, "D_900B288"),
    "king_meteor": (18, "D_900BD28"),
    "queen_meteor": (19, "D_900C550"),
    "wonder_wasp": (20, "D_9004B98"),
    "hyper_speeder": (21, "D_900D898"),
    "death_anchor": (22, "D_900DFF8"),
    "crazy_bear": (23, "D_900E698"),
    "night_thunder": (24, "D_900EFE8"),
    "big_fang": (25, "D_900F790"),
    "mighty_typhoon": (26, "D_90100F8"),
    "mad_wolf": (27, "D_9010C38"),
    "sonic_phantom": (28, "D_90113D8"),
    "blood_hawk": (29, "D_9011EA8"),
}


class BakeError(Exception):
    pass


# ---------------------------------------------------------------------------
# CRC64 — mirrored exactly from libultraship/src/ship/utils/StrHash64.cpp:176-182
# (CRC-64/XZ MSB-first table, init 0xFFFFFFFFFFFFFFFF, NO final complement).
# ---------------------------------------------------------------------------
_CRC64_POLY = 0x42F0E1EBA9EA3693
_CRC64_MASK = 0xFFFFFFFFFFFFFFFF
_CRC64_TABLE = []
for _i in range(256):
    _c = _i << 56
    for _ in range(8):
        if _c & (1 << 63):
            _c = ((_c << 1) ^ _CRC64_POLY) & _CRC64_MASK
        else:
            _c = (_c << 1) & _CRC64_MASK
    _CRC64_TABLE.append(_c)
assert _CRC64_TABLE[1] == _CRC64_POLY


def crc64(data):
    crc = _CRC64_MASK
    for b in data:
        crc = _CRC64_TABLE[((crc >> 56) ^ b) & 0xFF] ^ ((crc << 8) & _CRC64_MASK)
    return crc


def hash_words(path):
    h = crc64(path.encode("utf-8"))
    return (h >> 32) & 0xFFFFFFFF, h & 0xFFFFFFFF


# ---------------------------------------------------------------------------
# Minimal stdlib PNG decoder (8-bit, non-interlaced; color types 0/2/3/4/6).
# ---------------------------------------------------------------------------
def decode_png(path):
    with open(path, "rb") as fh:
        data = fh.read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise BakeError("not a PNG file")
    pos = 8
    idat = bytearray()
    plte = None
    trns = None
    w = h = ct = None
    while pos + 12 <= len(data):
        ln, = struct.unpack(">I", data[pos:pos + 4])
        typ = data[pos + 4:pos + 8]
        chunk = data[pos + 8:pos + 8 + ln]
        pos += 12 + ln
        if typ == b"IHDR":
            w, h, bd, ct, comp, filt, inter = struct.unpack(">IIBBBBB", chunk)
            if bd != 8:
                raise BakeError("unsupported PNG bit depth %d (8-bit only)" % bd)
            if comp != 0 or filt != 0:
                raise BakeError("unsupported PNG compression/filter method")
            if inter != 0:
                raise BakeError("interlaced PNGs are not supported")
            if ct not in (0, 2, 3, 4, 6):
                raise BakeError("unsupported PNG color type %d" % ct)
        elif typ == b"PLTE":
            plte = [tuple(chunk[i:i + 3]) for i in range(0, len(chunk), 3)]
        elif typ == b"tRNS":
            trns = chunk
        elif typ == b"IDAT":
            idat += chunk
        elif typ == b"IEND":
            break
    if w is None or not idat:
        raise BakeError("malformed PNG (missing IHDR/IDAT)")
    channels = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}[ct]
    stride = w * channels
    raw = zlib.decompress(bytes(idat))
    out = bytearray()
    prev = bytearray(stride)
    p = 0
    for _y in range(h):
        if p >= len(raw):
            raise BakeError("truncated PNG image data")
        f = raw[p]
        p += 1
        line = bytearray(raw[p:p + stride])
        p += stride
        if len(line) != stride:
            raise BakeError("truncated PNG image data")
        if f == 1:  # Sub
            for i in range(channels, stride):
                line[i] = (line[i] + line[i - channels]) & 0xFF
        elif f == 2:  # Up
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 0xFF
        elif f == 3:  # Average
            for i in range(stride):
                a = line[i - channels] if i >= channels else 0
                line[i] = (line[i] + ((a + prev[i]) >> 1)) & 0xFF
        elif f == 4:  # Paeth
            for i in range(stride):
                a = line[i - channels] if i >= channels else 0
                b = prev[i]
                c = prev[i - channels] if i >= channels else 0
                pp = a + b - c
                pa, pb, pc = abs(pp - a), abs(pp - b), abs(pp - c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[i] = (line[i] + pr) & 0xFF
        elif f != 0:
            raise BakeError("unknown PNG filter type %d" % f)
        out += line
        prev = line
    pixels = []
    if ct == 6:
        for i in range(0, len(out), 4):
            pixels.append((out[i], out[i + 1], out[i + 2], out[i + 3]))
    elif ct == 2:
        for i in range(0, len(out), 3):
            pixels.append((out[i], out[i + 1], out[i + 2], 255))
    elif ct == 0:
        for v in out:
            pixels.append((v, v, v, 255))
    elif ct == 4:
        for i in range(0, len(out), 2):
            pixels.append((out[i], out[i], out[i], out[i + 1]))
    else:  # palette
        if plte is None:
            raise BakeError("palette PNG without PLTE chunk")
        for idx in out:
            if idx >= len(plte):
                raise BakeError("palette index %d out of range" % idx)
            r, g, b = plte[idx]
            a = trns[idx] if trns is not None and idx < len(trns) else 255
            pixels.append((r, g, b, a))
    return w, h, pixels


# ---------------------------------------------------------------------------
# Texture encoding — mirrored from tools/gen_texture_pack.py (enc_rgba32:112-121,
# enc_rgba16:123-133, build_otex_v1:261-270). gen_texture_pack cannot be imported:
# it hard-requires PIL at module top; this tool is stdlib-only.
# ---------------------------------------------------------------------------
def enc_rgba32(pixels):
    out = bytearray(len(pixels) * 4)
    i = 0
    for r, g, b, a in pixels:
        out[i] = r; out[i + 1] = g; out[i + 2] = b; out[i + 3] = a
        i += 4
    return bytes(out)


def enc_rgba16(pixels):
    # N64 RGBA5551, big-endian 16-bit words.
    out = bytearray(len(pixels) * 2)
    i = 0
    for r, g, b, a in pixels:
        v = ((r >> 3) << 11) | ((g >> 3) << 6) | ((b >> 3) << 1) | (1 if a >= 128 else 0)
        out[i] = (v >> 8) & 0xFF; out[i + 1] = v & 0xFF
        i += 2
    return bytes(out)


def otr_header(res_type, version):
    header = bytearray(OTR_HEADER_SIZE)
    header[0] = OTR_BYTE_ORDER_LITTLE
    header[1] = OTR_IS_CUSTOM
    struct.pack_into("<I", header, 4, res_type)
    struct.pack_into("<I", header, 8, version)
    struct.pack_into("<Q", header, 12, 0)  # Id (unused by the reader)
    return bytes(header)


def build_otex_v1(tex_type, width, height, h_scale, v_scale, image_data):
    sub = struct.pack("<IIIIffI", tex_type, width, height, 0, h_scale, v_scale, len(image_data))
    return otr_header(OTR_TYPE_TEXTURE, OTR_TEXTURE_VERSION_V1) + sub + image_data


# ---------------------------------------------------------------------------
# Display-list command encoding. All OTR payload words are little-endian u32s whose
# VALUE keeps the N64 layout (opcode in the top byte of w0).
# ---------------------------------------------------------------------------
def cmd(w0, w1):
    return struct.pack("<II", w0 & 0xFFFFFFFF, w1 & 0xFFFFFFFF)


def cmd128(w0, w1, hi, lo):
    return struct.pack("<IIII", w0 & 0xFFFFFFFF, w1 & 0xFFFFFFFF, hi, lo)


# Combine mux encodings (decomp/include/PR/gbi.h G_CCMUX/G_ACMUX).
_CC = {"COMBINED": 0, "TEXEL0": 1, "TEXEL1": 2, "PRIMITIVE": 3, "SHADE": 4,
       "ENVIRONMENT": 5, "1": 6, "NOISE": 7, "0": 8}
_CC_C = dict(_CC, **{"0": 31})  # the 5-bit c slots encode constant 0 as 31
_AC = {"COMBINED": 0, "TEXEL0": 1, "TEXEL1": 2, "PRIMITIVE": 3, "SHADE": 4,
       "ENVIRONMENT": 5, "1": 6, "0": 7}


def set_combine(a0, b0, c0, d0, aa0, ab0, ac0, ad0, a1, b1, c1, d1, aa1, ab1, ac1, ad1):
    # gDPSetCombineLERP packing, gbi.h GCCc0w0/GCCc0w1/GCCc1w1 (decomp gbi.h:3126-3175).
    w0 = (G_SETCOMBINE << 24 | _CC[a0] << 20 | _CC_C[c0] << 15 | _AC[aa0] << 12
          | _AC[ac0] << 9 | _CC[a1] << 5 | _CC_C[c1])
    w1 = (_CC[b0] << 28 | _CC[d0] << 15 | _AC[ab0] << 12 | _AC[ad0] << 9
          | _CC[b1] << 24 | _AC[aa1] << 21 | _AC[ac1] << 18 | _CC[d1] << 6
          | _AC[ab1] << 3 | _AC[ad1])
    return cmd(w0, w1)


def combine_envbody():
    # Stock textured body combine (machine_draw.c:974): cycle1 (0-ENV)*TEXEL0+ENV keeps the
    # game's gDPSetEnvColor(bodyR,G,B) recolor; cycle2 COMBINED*SHADE applies vertex colors.
    return set_combine("0", "ENVIRONMENT", "TEXEL0", "ENVIRONMENT",
                       "0", "0", "0", "ENVIRONMENT",
                       "COMBINED", "0", "SHADE", "0",
                       "0", "0", "0", "COMBINED")


def combine_untextured():
    # Untextured family (machine_draw.c): cycle1 = ENVIRONMENT, cycle2 = ENV * SHADE.
    return set_combine("0", "0", "0", "ENVIRONMENT",
                       "0", "0", "0", "ENVIRONMENT",
                       "COMBINED", "0", "SHADE", "0",
                       "0", "0", "0", "COMBINED")


def combine_flat():
    # --flat: cycle1 = PRIMITIVE (color+alpha), cycle2 passes COMBINED through.
    return set_combine("0", "0", "0", "PRIMITIVE",
                       "0", "0", "0", "PRIMITIVE",
                       "COMBINED", "0", "1", "0",
                       "0", "0", "0", "COMBINED")


def _calc_dxt(width, bpp):
    # CALC_DXT (decomp gbi.h:3348): TXL2WORDS = max(1, width*bpp/8).
    t = max(1, width * bpp // 8)
    return ((1 << 11) + t - 1) // t


def texture_block(tex_arc, w, h, siz):
    # gsDPLoadTextureBlock equivalent (gbi.h:3622) with G_TX_WRAP and mask=log2(dim).
    bpp = 2 if siz == G_IM_SIZ_16b else 4
    mask_s = w.bit_length() - 1
    mask_t = h.bit_length() - 1
    hi, lo = hash_words(tex_arc)
    out = bytearray()
    w0 = (G_SETTIMG_OTR_HASH << 24 | G_IM_FMT_RGBA << 21 | siz << 19 | (w - 1))
    out += cmd128(w0, 0, hi, lo)
    out += cmd(G_SETTILE << 24 | G_IM_FMT_RGBA << 21 | siz << 19,  # line=0, tmem=0
               G_TX_LOADTILE << 24 | mask_t << 14 | mask_s << 4)
    out += cmd(G_LOADSYNC << 24, 0)
    out += cmd(G_LOADBLOCK << 24, G_TX_LOADTILE << 24 | ((w * h - 1) << 12) | _calc_dxt(w, bpp))
    out += cmd(G_PIPESYNC << 24, 0)
    line = ((w * bpp) + 7) >> 3
    out += cmd(G_SETTILE << 24 | G_IM_FMT_RGBA << 21 | siz << 19 | line << 9,
               G_TX_RENDERTILE << 24 | mask_t << 14 | mask_s << 4)
    out += cmd(G_SETTILESIZE << 24, ((w - 1) << 2) << 12 | ((h - 1) << 2))
    out += cmd(G_TEXTURE << 24 | 0x2, 0xFFFFFFFF)  # gSPTexture(0xFFFF, 0xFFFF, 0, 0, G_ON)
    return bytes(out)


def emit_geometry(tris, nverts, base, vtx_arc, out):
    """Append G_VTX_OTR_HASH loads + TRI1/TRI2 for `tris` (local indices) to `out`.

    The game's vertex cache is 64 slots; loads are chunks of <=32 verts into alternating
    banks (v0 = 0/32), each load evicting the whole bank it overwrites. Vertices arrive in
    first-use order, so a triangle referencing an evicted vertex, or spanning more than the
    window, cannot be scheduled and is a hard error.
    """
    hi, lo = hash_words(vtx_arc)
    cache = {}   # local vert index -> cache slot
    next_load = 0
    v0 = 0
    buf = []     # buffered slot triples since the last load

    def flush():
        i = 0
        while i + 1 < len(buf):
            (a, b, c), (d, e, f) = buf[i], buf[i + 1]
            out.append(cmd(G_TRI2 << 24 | (a * 2) << 16 | (b * 2) << 8 | (c * 2),
                           (d * 2) << 16 | (e * 2) << 8 | (f * 2)))
            i += 2
        if i < len(buf):
            a, b, c = buf[i]
            out.append(cmd(G_TRI1 << 24 | (a * 2) << 16 | (b * 2) << 8 | (c * 2), 0))
        del buf[:]

    for tri in tris:
        if any(v not in cache for v in tri):
            if min(v for v in tri if v not in cache) < next_load:
                raise BakeError("triangle spans outside the 64-slot vertex-cache window")
            if max(tri) - next_load >= 64:
                raise BakeError("triangle spans outside the 64-slot vertex-cache window")
            flush()
            while any(v not in cache for v in tri):
                evict_lo = next_load - 64
                if evict_lo >= 0 and any(evict_lo <= v < next_load - 32 for v in tri):
                    raise BakeError("triangle spans outside the 64-slot vertex-cache window")
                n = min(32, nverts - next_load)
                out.append(cmd128(G_VTX_OTR_HASH << 24 | (n << 12) | ((v0 + n) << 1),
                                  (base + next_load) * 16, hi, lo))
                cache = {k: s for k, s in cache.items() if not (v0 <= s < v0 + 32)}
                for k in range(next_load, next_load + n):
                    cache[k] = v0 + (k - next_load)
                next_load += n
                v0 ^= 32
        buf.append(tuple(cache[v] for v in tri))
    flush()


def build_odlt(commands, self_arc):
    """ODLT v0: int8 ucode, 0xFF pad to 8-byte absolute alignment, G_MARKER, commands."""
    payload = bytearray()
    payload.append(UCODE_F3DEX2)
    while (OTR_HEADER_SIZE + len(payload)) % 8 != 0:
        payload.append(0xFF)
    hi, lo = hash_words(self_arc)
    payload += cmd128(G_MARKER << 24, 0xBEEFBEEF, hi, lo)
    payload += commands
    return otr_header(OTR_TYPE_DISPLAYLIST, 0) + bytes(payload)


def build_ovtx(records):
    """OVTX v0 (torch VertexFactory.cpp:18-33): u32 count + 16-byte vertex records."""
    out = bytearray(struct.pack("<I", len(records)))
    for x, y, z, tu, tv, r, g, b, a in records:
        out += struct.pack("<hhhHhh4B", x, y, z, 0, tu, tv, r, g, b, a)
    return otr_header(OTR_TYPE_VERTEX, 0) + bytes(out)


# ---------------------------------------------------------------------------
# OBJ / MTL parsing
# ---------------------------------------------------------------------------
class ObjData:
    def __init__(self):
        self.verts = []      # (x, y, z, r, g, b, a) floats, color 0..1
        self.uvs = []        # (u, v)
        self.sections = []   # [(material_name, [((vi, ti), (vi, ti), (vi, ti)), ...])]
        self.mtllibs = []


def _parse_corner(tok, nv, nt, errors, ctx):
    parts = tok.split("/")
    try:
        vi = int(parts[0])
        ti = int(parts[1]) if len(parts) >= 2 and parts[1] else None
    except ValueError:
        errors.append("%s: bad face corner '%s'" % (ctx, tok))
        return None
    if vi < 0 or (ti is not None and ti < 0):
        errors.append("%s: negative (relative) OBJ indices are not supported" % ctx)
        return None
    if vi < 1 or vi > nv:
        errors.append("%s: vertex index %d out of range (1..%d)" % (ctx, vi, nv))
        return None
    if ti is not None and (ti < 1 or ti > nt):
        errors.append("%s: uv index %d out of range (1..%d)" % (ctx, ti, nt))
        return None
    return (vi - 1, ti - 1 if ti is not None else None)


def parse_obj(path):
    obj = ObjData()
    errors = []
    cur_section = None

    def section(name):
        nonlocal cur_section
        for nm, tris in obj.sections:
            if nm == name:
                cur_section = tris
                return
        obj.sections.append((name, []))
        cur_section = obj.sections[-1][1]

    section("none")
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        for ln, raw in enumerate(fh, 1):
            line = raw.strip()
            if not line or line.startswith("#"):
                continue
            parts = line.split()
            k = parts[0].lower()
            ctx = "%s:%d" % (path, ln)
            if k == "v":
                try:
                    vals = [float(x) for x in parts[1:]]
                except ValueError:
                    errors.append("%s: malformed v line" % ctx)
                    continue
                if len(vals) < 6:
                    errors.append("%s: v line is missing vertex colors (need 'v x y z r g b [a]')"
                                  % ctx)
                    continue
                x, y, z, r, g, b = vals[:6]
                a = vals[6] if len(vals) >= 7 else 1.0
                for cname, cval in zip("rgba", (r, g, b, a)):
                    if not (0.0 <= cval <= 1.0):
                        errors.append("%s: vertex color channel %s=%g out of range 0..1"
                                      % (ctx, cname, cval))
                obj.verts.append((x, y, z, r, g, b, a))
            elif k == "vt":
                try:
                    obj.uvs.append((float(parts[1]), float(parts[2])))
                except (ValueError, IndexError):
                    errors.append("%s: malformed vt line" % ctx)
            elif k == "f":
                n = len(parts) - 1
                if n < 3:
                    errors.append("%s: face with fewer than 3 vertices" % ctx)
                    continue
                if n > 4:
                    errors.append("%s: ngon (%d vertices) is not supported; triangulate the mesh "
                                  "first (only tris/quads)" % (ctx, n))
                    continue
                corners = [_parse_corner(tok, len(obj.verts), len(obj.uvs), errors, ctx)
                           for tok in parts[1:]]
                if any(c is None for c in corners):
                    continue
                cur_section.append(tuple(corners[:3]))
                if n == 4:  # quad -> fan
                    cur_section.append((corners[0], corners[2], corners[3]))
            elif k == "usemtl":
                section(" ".join(parts[1:]))
            elif k == "mtllib":
                obj.mtllibs.append(" ".join(parts[1:]))
    return obj, errors


def parse_mtl(path):
    mats = {}
    errors = []
    cur = None
    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        for ln, raw in enumerate(fh, 1):
            line = raw.strip()
            if not line or line.startswith("#"):
                continue
            parts = line.split()
            k = parts[0].lower()
            ctx = "%s:%d" % (path, ln)
            if k == "newmtl":
                cur = {"kd": (0.8, 0.8, 0.8), "d": 1.0, "map_kd": None}
                mats[" ".join(parts[1:])] = cur
            elif cur is not None and k == "kd" and len(parts) >= 4:
                try:
                    cur["kd"] = tuple(float(x) for x in parts[1:4])
                except ValueError:
                    errors.append("%s: malformed Kd line" % ctx)
            elif cur is not None and k == "d" and len(parts) >= 2:
                try:
                    cur["d"] = float(parts[1])
                except ValueError:
                    errors.append("%s: malformed d line" % ctx)
            elif cur is not None and k == "map_kd" and len(parts) >= 2:
                cur["map_kd"] = parts[-1]
    return mats, errors


# ---------------------------------------------------------------------------
# Pack planning / validation
# ---------------------------------------------------------------------------
def normalize_machine_name(name):
    return re.sub(r"[\s\-]+", "_", name.strip().lower())


def sanitize_path_component(name):
    return re.sub(r"[^A-Za-z0-9_.\-]", "_", name)


def pack_paths(machine, lod_no=None, material=None):
    base = "models/pack/machine/%s" % machine
    if material is not None:
        return "%s/tex/%s" % (base, sanitize_path_component(material))
    return "%s/lod%d" % (base, lod_no)


def _check_s16(value, what, ctx, errors):
    if not (-32768 <= value <= 32767):
        errors.append("%s: %s=%d outside the s16 range (-32768..32767); never clamped, "
                      "fix the model or model.json scale" % (ctx, what, value))


def build_registry(lod_mtls, overrides, errors, warnings):
    """Resolve every material across all LODs. lod_mtls: {matname: [(lod_no, matdef, mtl_dir)]}.

    registry entry: {"kind": flat|textured|plain, "kd", "alpha", "fmt",
                     "tex_arc", "tex": (w, h, pixels) or None}
    """
    registry = {}
    tex_names = {}
    for name, defs in lod_mtls.items():
        lod0, m0, dir0 = defs[0]
        flat = name.lower().endswith("--flat")
        map_kd0 = m0["map_kd"]
        for lod_n, mn, dirn in defs[1:]:
            if (mn["map_kd"] is None) != (map_kd0 is None):
                errors.append("material '%s': map_Kd present in one LOD's MTL but absent in "
                              "lod%d's" % (name, lod_n))
            elif mn["map_kd"] and map_kd0:
                p0 = os.path.normpath(os.path.join(dir0, map_kd0))
                pn = os.path.normpath(os.path.join(dirn, mn["map_kd"]))
                if os.path.normcase(p0) != os.path.normcase(pn):
                    errors.append("material '%s': map_Kd differs across LODs ('%s' vs '%s'); "
                                  "one texture per material name" % (name, map_kd0, mn["map_kd"]))
            if mn["kd"] != m0["kd"]:
                warnings.append("material '%s': Kd differs across LODs; using lod%d's"
                                % (name, lod0))
        ov = overrides.get(name, {}) if isinstance(overrides, dict) else {}
        fmt = ov.get("format", "rgba16")
        if fmt not in ("rgba16", "rgba32"):
            errors.append("material '%s': model.json format '%s' (only rgba16/rgba32)"
                          % (name, fmt))
            fmt = "rgba16"
        entry = {"kind": "flat" if flat else ("textured" if map_kd0 else "plain"),
                 "kd": m0["kd"], "alpha": m0["d"], "fmt": fmt,
                 "tex_arc": None, "tex": None}
        if flat and map_kd0:
            warnings.append("material '%s': --flat name suffix with map_Kd; the texture is "
                            "ignored" % name)
        if entry["kind"] == "plain":
            warnings.append("material '%s' has no texture and no --flat suffix; baked with the "
                            "untextured env combine (vertex color x env)" % name)
        if entry["kind"] == "textured":
            tex_path = os.path.normpath(os.path.join(dir0, map_kd0))
            try:
                w, h, pixels = decode_png(tex_path)
            except (OSError, BakeError, zlib.error) as exc:
                errors.append("material '%s': cannot decode texture %s (%s)"
                              % (name, tex_path, exc))
                registry[name] = entry
                continue
            if w & (w - 1) or h & (h - 1):
                errors.append("material '%s': texture %s is %dx%d; dimensions must be powers "
                              "of two" % (name, tex_path, w, h))
            bpp = 2 if fmt == "rgba16" else 4
            if w * h * bpp > TMEM_TILE_BYTES:
                errors.append("material '%s': %dx%d %s texture is %d bytes, over the %d-byte "
                              "TMEM tile budget" % (name, w, h, fmt.upper(), w * h * bpp,
                                                    TMEM_TILE_BYTES))
            entry["tex"] = (w, h, pixels)
            arc = pack_paths("MACHINE", material=name)  # placeholder, fixed at emit time
            safe = sanitize_path_component(name)
            if safe in tex_names and tex_names[safe] != name:
                errors.append("materials '%s' and '%s' collide on texture path component '%s'"
                              % (tex_names[safe], name, safe))
            tex_names[safe] = name
            entry["tex_arc"] = arc
        registry[name] = entry
    return registry


def build_lod(lod_no, obj_path, obj, registry, scale, machine, errors, warnings):
    """Build one LOD. Returns (commands_bytes, ovtx_records, tri_count) or None on errors.

    The LOD list intentionally contains no global RDP setup (geometry mode, cycle type,
    render mode). The game's machine draw functions and the select-machine screen set that
    state before executing D_800CDDB0[slot*6+lod]; emitting it inside the list would make
    the override stomp on the shared state used by the other 29 machines on that screen.
    """
    vtx_arc = pack_paths(machine, lod_no=lod_no) + ".vtx"
    records = []
    out = []
    tri_count = 0
    n_errors_before = len(errors)
    for matname, tris in obj.sections:
        if not tris:
            continue
        mat = registry.get(matname)
        if mat is None:
            errors.append("lod%d (%s): usemtl '%s' has no newmtl in the MTL"
                          % (lod_no, obj_path, matname))
            continue
        base = len(records)
        lmap = {}
        localtris = []
        for tri in tris:
            lt = []
            for vi, ti in tri:
                key = (vi, ti if mat["kind"] == "textured" else None)
                li = lmap.get(key)
                if li is None:
                    li = len(lmap)
                    lmap[key] = li
                    x, y, z, r, g, b, a = obj.verts[vi]
                    ctx = "lod%d (%s) vertex %d" % (lod_no, os.path.basename(obj_path), vi + 1)
                    xi, yi, zi = round(x * scale), round(y * scale), round(z * scale)
                    _check_s16(xi, "position x", ctx, errors)
                    _check_s16(yi, "position y", ctx, errors)
                    _check_s16(zi, "position z", ctx, errors)
                    if mat["kind"] == "textured":
                        if ti is None:
                            errors.append("%s: face in textured material '%s' has no vt"
                                          % (ctx, matname))
                            tu = tv = 0
                        else:
                            u, v = obj.uvs[ti]
                            tw, th, _px = mat["tex"] if mat["tex"] else (0, 0, None)
                            tu = round(u * tw * 32)
                            tv = round((1.0 - v) * th * 32)
                            _check_s16(tu, "texcoord tu (S10.5)", ctx, errors)
                            _check_s16(tv, "texcoord tv (S10.5)", ctx, errors)
                    else:
                        tu = tv = 0
                    records.append((xi, yi, zi, tu, tv,
                                    round(r * 255), round(g * 255), round(b * 255),
                                    round(a * 255)))
                lt.append(li)
            localtris.append(tuple(lt))
        tri_count += len(localtris)
        # combine + prim color + texture state
        if mat["kind"] == "flat":
            out.append(combine_flat())
            kr, kg, kb = mat["kd"]
            ka = mat["alpha"]
            out.append(cmd(G_SETPRIMCOLOR << 24,
                           round(kr * 255) << 24 | round(kg * 255) << 16
                           | round(kb * 255) << 8 | round(ka * 255)))
        elif mat["kind"] == "textured" and mat["tex"] is not None:
            out.append(combine_envbody())
            tw, th, _px = mat["tex"]
            siz = G_IM_SIZ_16b if mat["fmt"] == "rgba16" else G_IM_SIZ_32b
            out.append(texture_block(mat["tex_arc"], tw, th, siz))
        else:
            out.append(combine_untextured())
            out.append(cmd(G_TEXTURE << 24, 0))  # G_OFF: undo a previous material's texture
        try:
            emit_geometry(localtris, len(lmap), base, vtx_arc, out)
        except BakeError as exc:
            errors.append("lod%d material '%s': %s" % (lod_no, matname, exc))
    out.append(cmd(G_ENDDL << 24, 0))
    if len(errors) != n_errors_before:
        return None
    return b"".join(out), records, tri_count


def load_model_json(workdir, errors):
    path = os.path.join(workdir, "model.json")
    if not os.path.isfile(path):
        errors.append("model.json not found in %s (run `gen_model_pack.py init` first)" % workdir)
        return None
    try:
        with open(path, "r", encoding="utf-8") as fh:
            cfg = json.load(fh)
    except (OSError, json.JSONDecodeError) as exc:
        errors.append("model.json is not valid JSON (%s)" % exc)
        return None
    return cfg


def plan_pack(workdir, allow_overbudget):
    """Validate the work dir and return (entries, errors, warnings, infos)."""
    entries = []
    errors, warnings, infos = [], [], []
    cfg = load_model_json(workdir, errors)
    if cfg is None:
        return entries, errors, warnings, infos
    machine = normalize_machine_name(str(cfg.get("machine", "")))
    if machine not in MACHINES:
        errors.append("unknown machine '%s' (valid: %s)"
                      % (cfg.get("machine"), ", ".join(sorted(MACHINES))))
        return entries, errors, warnings, infos
    scale = cfg.get("scale", 1.0)
    if not isinstance(scale, (int, float)) or scale <= 0:
        errors.append("model.json scale must be a positive number")
        scale = 1.0
    lods = cfg.get("lods", {})
    if not isinstance(lods, dict) or not lods:
        errors.append("model.json has no lods")
        return entries, errors, warnings, infos
    lod_files = {}
    for key, rel in lods.items():
        if not re.fullmatch(r"[1-6]", str(key)):
            errors.append("model.json lods key '%s' is not a LOD number 1..6" % key)
            continue
        lod_files[int(key)] = os.path.join(workdir, rel)
    for n in range(1, 7):
        if n not in lod_files:
            warnings.append("lod%d is not packed; the stock model is used for that LOD" % n)

    # Parse all OBJs + MTLs, build the cross-LOD material registry.
    parsed = {}
    lod_mtls = {}
    for lod_no, obj_path in sorted(lod_files.items()):
        if not os.path.isfile(obj_path):
            errors.append("lod%d: OBJ not found: %s" % (lod_no, obj_path))
            continue
        obj, perrors = parse_obj(obj_path)
        errors.extend(perrors)
        if not obj.verts:
            errors.append("lod%d (%s): no vertices" % (lod_no, obj_path))
        if not obj.mtllibs:
            errors.append("lod%d (%s): no mtllib line" % (lod_no, obj_path))
        mtl = {}
        for lib in obj.mtllibs:
            mtl_path = os.path.join(os.path.dirname(obj_path), lib)
            if not os.path.isfile(mtl_path):
                errors.append("lod%d: MTL not found: %s" % (lod_no, mtl_path))
                continue
            mm, merrors = parse_mtl(mtl_path)
            errors.extend(merrors)
            for name, mdef in mm.items():
                mtl.setdefault(name, (mdef, os.path.dirname(mtl_path) or "."))
        for name, _tris in obj.sections:
            if name not in mtl and name != "none":
                errors.append("lod%d (%s): usemtl '%s' has no newmtl in %s"
                              % (lod_no, os.path.basename(obj_path), name,
                                 ", ".join(obj.mtllibs)))
            mdef, mdir = mtl.get(name, ({"kd": (0.8, 0.8, 0.8), "d": 1.0, "map_kd": None},
                                        os.path.dirname(obj_path) or "."))
            lod_mtls.setdefault(name, []).append((lod_no, mdef, mdir))
        parsed[lod_no] = (obj_path, obj)
    registry = build_registry(lod_mtls, cfg.get("materials", {}), errors, warnings)

    # Texture entries (deduped by material across LODs).
    for name in sorted(registry):
        mat = registry[name]
        if mat["kind"] != "textured" or mat["tex"] is None:
            continue
        w, h, pixels = mat["tex"]
        if mat["fmt"] == "rgba16":
            data = enc_rgba16(pixels)
            tt = TT_RGBA16
        else:
            data = enc_rgba32(pixels)
            tt = TT_RGBA32
        arc = pack_paths(machine, material=name)
        entries.append((arc, build_otex_v1(tt, w, h, 1.0, 1.0, data)))
        infos.append("texture %s: %dx%d %s -> %s" % (name, w, h, mat["fmt"], arc))

    # Per-LOD ODLT + OVTX.
    for lod_no in sorted(parsed):
        obj_path, obj = parsed[lod_no]
        # fix the texture arc placeholder now that the machine is known
        for name, _tris in obj.sections:
            mat = registry.get(name)
            if mat and mat["kind"] == "textured":
                mat["tex_arc"] = pack_paths(machine, material=name)
        built = build_lod(lod_no, obj_path, obj, registry, scale, machine, errors, warnings)
        if built is None:
            continue
        commands, records, tris = built
        lod_arc = pack_paths(machine, lod_no=lod_no)
        entries.append((lod_arc + ".vtx", build_ovtx(records)))
        entries.append((lod_arc, build_odlt(commands, lod_arc)))
        infos.append("lod%d: %d verts, %d tris, %d DL bytes -> %s"
                     % (lod_no, len(records), tris, len(commands), lod_arc))
        if tris > STOCK_LOD1_TRI_BUDGET:
            msg = ("lod%d: %d tris exceeds the stock Lod1 budget of %d"
                   % (lod_no, tris, STOCK_LOD1_TRI_BUDGET))
            if allow_overbudget:
                warnings.append(msg + " (--allow-overbudget)")
            else:
                errors.append(msg)
    infos.append("stock Lod1 triangle budget: %d (max over the 30 dumped D_800CDAD8 machine "
                 "models in dump/models/manifest.tsv)" % STOCK_LOD1_TRI_BUDGET)
    return entries, errors, warnings, infos


def resolve_metadata(workdir, args, entries):
    meta = {}
    meta_path = os.path.join(workdir, "workshop.json")
    if os.path.isfile(meta_path):
        with open(meta_path, "r", encoding="utf-8") as fh:
            meta = json.load(fh)
        source = "workshop.json"
    else:
        source = "command line"
    out_stem = os.path.splitext(os.path.basename(getattr(args, "output", None) or "pack.o2r"))[0]
    meta["pack_type"] = "model"
    meta.setdefault("name", getattr(args, "name", None) or out_stem)
    meta.setdefault("id", getattr(args, "id", None) or out_stem)
    meta.setdefault("version", getattr(args, "version", None) or "1.0.0")
    meta.setdefault("author", getattr(args, "author", None) or "")
    meta.setdefault("game_version", getattr(args, "game_version", None) or "us.rev0")
    for flag, key in (("name", "name"), ("id", "id"), ("version", "version"),
                      ("author", "author"), ("game_version", "game_version")):
        val = getattr(args, flag, None)
        if val:
            meta[key] = val
    meta["files"] = [{"path": arc, "sha256": hashlib.sha256(data).hexdigest()}
                     for arc, data in sorted(entries)]
    return json.dumps(meta, indent=2, sort_keys=True).encode("utf-8"), source


def report(errors, warnings, infos):
    for m in infos:
        print("info: %s" % m)
    for m in warnings:
        print("warning: %s" % m)
    for m in errors:
        print("error: %s" % m)


# ---------------------------------------------------------------------------
# check on a built .o2r — re-reads every resource exactly the way
# torch/DisplayListFactory.cpp:186-220 and VertexFactory.cpp:18-33 parse them.
# ---------------------------------------------------------------------------
def _parse_header(data, arc, errors):
    if len(data) < OTR_HEADER_SIZE:
        errors.append("%s: shorter than the 64-byte OTR header" % arc)
        return None
    if data[0] != OTR_BYTE_ORDER_LITTLE:
        errors.append("%s: header byte0 endianness is %d, expected 0 (Little)"
                      % (arc, data[0]))
    res_type, version = struct.unpack_from("<II", data, 4)
    return res_type, version


def check_odlt(data, arc, hashmap, errors, warnings):
    hd = _parse_header(data, arc, errors)
    if hd is None:
        return
    res_type, version = hd
    if res_type != OTR_TYPE_DISPLAYLIST:
        errors.append("%s: type 0x%08X, expected ODLT" % (arc, res_type))
        return
    if version != 0:
        errors.append("%s: ODLT version %d, expected 0" % (arc, version))
    pos = OTR_HEADER_SIZE
    ucode = struct.unpack_from("<b", data, pos)[0]
    pos += 1
    if ucode != UCODE_F3DEX2:
        errors.append("%s: ucode %d, expected %d (f3dex2)" % (arc, ucode, UCODE_F3DEX2))
    while pos % 8 != 0:
        if pos >= len(data) or data[pos] != 0xFF:
            errors.append("%s: bad 0xFF alignment padding after the ucode byte" % arc)
            return
        pos += 1
    n_cmds = 0
    while pos < len(data):
        if pos + 8 > len(data):
            errors.append("%s: truncated command at offset %d" % (arc, pos))
            return
        w0, w1 = struct.unpack_from("<II", data, pos)
        # words are LE u32s whose VALUE keeps the N64 layout: opcode in the top byte of w0
        op = w0 >> 24
        size = 16 if op in EXPANDED_OPCODES else 8
        if pos + size > len(data):
            errors.append("%s: truncated command at offset %d" % (arc, pos))
            return
        if op == G_ENDDL:
            pos += 8
            n_cmds += 1
            break
        if op not in KNOWN_NATIVE_OPCODES and op not in EXPANDED_OPCODES:
            errors.append("%s: unknown opcode 0x%02X at offset %d" % (arc, op, pos))
            return
        if op in (G_VTX_OTR_HASH, G_SETTIMG_OTR_HASH):
            hi, lo = struct.unpack_from("<II", data, pos + 8)
            ref = hashmap.get((hi << 32) | lo)
            if ref is None:
                errors.append("%s: offset %d: %s hash 0x%016X matches no entry in the archive"
                              % (arc, pos, "G_VTX_OTR_HASH" if op == G_VTX_OTR_HASH
                                 else "G_SETTIMG_OTR_HASH", (hi << 32) | lo))
            elif op == G_VTX_OTR_HASH:
                n = (w0 >> 12) & 0xFF
                voff = w1
                if voff % 16 != 0:
                    errors.append("%s: offset %d: G_VTX_OTR_HASH vertex offset %d is not a "
                                  "multiple of 16" % (arc, pos, voff))
                # bounds-checked against the sibling OVTX by the caller via hashmap sizes
        pos += size
        n_cmds += 1
    else:
        errors.append("%s: no G_ENDDL terminator" % arc)
        return
    if pos != len(data):
        errors.append("%s: %d trailing byte(s) after G_ENDDL" % (arc, len(data) - pos))


def check_ovtx(data, arc, errors):
    hd = _parse_header(data, arc, errors)
    if hd is None:
        return
    res_type, version = hd
    if res_type != OTR_TYPE_VERTEX:
        errors.append("%s: type 0x%08X, expected OVTX" % (arc, res_type))
        return
    if version != 0:
        errors.append("%s: OVTX version %d, expected 0" % (arc, version))
    count, = struct.unpack_from("<I", data, OTR_HEADER_SIZE)
    expected = OTR_HEADER_SIZE + 4 + count * 16
    if len(data) != expected:
        errors.append("%s: %d bytes, expected 68 + %d*16 = %d" % (arc, len(data), count,
                                                                  expected))


def check_otex(data, arc, errors):
    hd = _parse_header(data, arc, errors)
    if hd is None:
        return
    res_type, version = hd
    if res_type != OTR_TYPE_TEXTURE:
        errors.append("%s: type 0x%08X, expected OTEX" % (arc, res_type))
        return
    if version != OTR_TEXTURE_VERSION_V1:
        errors.append("%s: OTEX version %d, expected 1" % (arc, version))
    ttype, w, h, flags, hs, vs, dlen = struct.unpack_from("<IIIIffI", data, OTR_HEADER_SIZE)
    bpp = {TT_RGBA16: 2, TT_RGBA32: 4}.get(ttype)
    if bpp is None:
        errors.append("%s: texture type %d (only RGBA16/RGBA32 are supported)" % (arc, ttype))
        return
    if len(data) != OTR_HEADER_SIZE + 28 + dlen or dlen != w * h * bpp:
        errors.append("%s: %dx%d type-%d texture payload is %d bytes, expected %d"
                      % (arc, w, h, ttype, dlen, w * h * bpp))


def check_archive(path):
    errors, warnings, infos = [], [], []
    try:
        z = zipfile.ZipFile(path)
    except (OSError, zipfile.BadZipFile) as exc:
        print("error: %s is not a readable .o2r zip (%s)" % (path, exc))
        return 1
    names = z.namelist()
    if "workshop.json" not in names:
        errors.append("workshop.json missing from the archive")
        meta = {}
    else:
        try:
            meta = json.loads(z.read("workshop.json").decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError) as exc:
            errors.append("workshop.json is not valid JSON (%s)" % exc)
            meta = {}
        else:
            if meta.get("pack_type") != "model":
                errors.append("workshop.json pack_type is %r, expected 'model'"
                              % meta.get("pack_type"))
    payloads = [n for n in names if n != "workshop.json"]
    hashmap = {crc64(n.encode("utf-8")): n for n in names}
    for arc in sorted(payloads):
        data = z.read(arc)
        if len(data) < OTR_HEADER_SIZE:
            errors.append("%s: shorter than the 64-byte OTR header" % arc)
            continue
        res_type, _ver = struct.unpack_from("<II", data, 4)
        if res_type == OTR_TYPE_DISPLAYLIST:
            check_odlt(data, arc, hashmap, errors, warnings)
            infos.append("%s: ODLT parsed clean" % arc)
        elif res_type == OTR_TYPE_VERTEX:
            check_ovtx(data, arc, errors)
            infos.append("%s: OVTX parsed clean" % arc)
        elif res_type == OTR_TYPE_TEXTURE:
            check_otex(data, arc, errors)
            infos.append("%s: OTEX parsed clean" % arc)
        else:
            warnings.append("%s: unknown resource type 0x%08X" % (arc, res_type))
    listed = {f["path"]: f["sha256"] for f in meta.get("files", []) if isinstance(f, dict)}
    for arc in payloads:
        want = listed.pop(arc, None)
        got = hashlib.sha256(z.read(arc)).hexdigest()
        if want is None:
            errors.append("%s: missing from workshop.json files[]" % arc)
        elif want != got:
            errors.append("%s: sha256 mismatch with workshop.json files[]" % arc)
    for arc in listed:
        errors.append("%s: in workshop.json files[] but not in the archive" % arc)
    report(errors, warnings, infos)
    if errors:
        print("check: %d error(s)" % len(errors))
        return 1
    print("check: OK (%d resources)" % len(payloads))
    return 0


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------
def cmd_init(args):
    machine = normalize_machine_name(args.machine)
    if machine not in MACHINES:
        sys.stderr.write("error: unknown machine '%s'\nvalid machines:\n  %s\n"
                         % (args.machine, "\n  ".join(sorted(MACHINES))))
        return 2
    _idx, symbol = MACHINES[machine]
    dump_dir = args.dump_dir
    if dump_dir is None:
        for cand in (os.path.join("dump", "models"), "dump"):
            if os.path.isdir(cand):
                dump_dir = cand
                break
        else:
            sys.stderr.write("error: dump dir not found; pass --dump-dir <path to dump/models>\n")
            return 2
    src_obj = os.path.join(dump_dir, "machine_models_%s.obj" % symbol)
    src_mtl = os.path.join(dump_dir, "machine_models_%s.mtl" % symbol)
    if not os.path.isfile(src_obj) or not os.path.isfile(src_mtl):
        sys.stderr.write("error: dumped stock model not found:\n  %s\n  %s\n"
                         "(run the model dump first; see tools/gen_dump_all_models.py)\n"
                         % (src_obj, src_mtl))
        return 2
    out_dir = args.output
    os.makedirs(out_dir, exist_ok=True)
    with open(src_obj, "r", encoding="utf-8") as fh:
        obj_text = fh.read()
    obj_text = obj_text.replace("mtllib machine_models_%s.mtl" % symbol, "mtllib lod1.mtl", 1)
    with open(os.path.join(out_dir, "lod1.obj"), "w", encoding="utf-8", newline="\n") as fh:
        fh.write(obj_text)
    with open(src_mtl, "r", encoding="utf-8") as fh:
        mtl_text = fh.read()
    mats, merrors = parse_mtl(src_mtl)
    for m in merrors:
        print("warning: %s" % m)
    tex_dir = os.path.join(out_dir, "textures")
    for name, mdef in mats.items():
        if not mdef["map_kd"]:
            continue
        src_tex = os.path.join(dump_dir, mdef["map_kd"])
        if not os.path.isfile(src_tex):
            print("warning: material '%s': map_Kd %s not found in the dump" % (name, src_tex))
            continue
        os.makedirs(tex_dir, exist_ok=True)
        base = os.path.basename(mdef["map_kd"])
        with open(src_tex, "rb") as fi, open(os.path.join(tex_dir, base), "wb") as fo:
            fo.write(fi.read())
        mtl_text = mtl_text.replace(mdef["map_kd"], "textures/" + base)
    with open(os.path.join(out_dir, "lod1.mtl"), "w", encoding="utf-8", newline="\n") as fh:
        fh.write(mtl_text)
    model = {"machine": machine, "scale": 1.0, "lods": {"1": "lod1.obj"}, "materials": {}}
    with open(os.path.join(out_dir, "model.json"), "w", encoding="utf-8", newline="\n") as fh:
        json.dump(model, fh, indent=2)
        fh.write("\n")
    workshop = {"pack_type": "model", "name": machine.replace("_", " ").title(),
                "id": "workshop.%s" % machine, "version": "1.0.0", "author": "",
                "game_version": "us.rev0"}
    with open(os.path.join(out_dir, "workshop.json"), "w", encoding="utf-8", newline="\n") as fh:
        json.dump(workshop, fh, indent=2)
        fh.write("\n")
    obj, _ = parse_obj(src_obj)
    tris = sum(len(t) for _n, t in obj.sections)
    print("initialized %s from %s (%d verts, %d tris; stock Lod1 budget %d)"
          % (out_dir, src_obj, len(obj.verts), tris, STOCK_LOD1_TRI_BUDGET))
    print("note: the dump contains no per-LOD meshes (they are built into BSS at boot), so only")
    print("      lod1 was scaffolded. Author lod2..lod6 by hand and add them to model.json;")
    print("      any missing LOD falls back to the stock model at runtime.")
    return 0


def cmd_pack(args):
    if not os.path.isdir(args.input_dir):
        sys.stderr.write("error: input directory not found: %s\n" % args.input_dir)
        return 2
    entries, errors, warnings, infos = plan_pack(args.input_dir, args.allow_overbudget)
    report(errors, warnings, infos)
    if errors:
        sys.stderr.write("error: %d problem(s) must be fixed before packing; nothing written\n"
                         % len(errors))
        return 1
    if not entries:
        sys.stderr.write("error: nothing to pack\n")
        return 2
    try:
        meta_bytes, meta_source = resolve_metadata(args.input_dir, args, entries)
    except (OSError, json.JSONDecodeError) as exc:
        sys.stderr.write("error: %s\n" % exc)
        return 2
    arc_entries = sorted(entries) + [("workshop.json", meta_bytes)]
    with zipfile.ZipFile(args.output, "w", zipfile.ZIP_DEFLATED) as z:
        for arc, data in arc_entries:
            info = zipfile.ZipInfo(arc, date_time=FIXED_DATE)
            info.compress_type = zipfile.ZIP_DEFLATED
            info.external_attr = 0o644 << 16
            z.writestr(info, data)
    print("wrote %s (%d resource(s), metadata %s)" % (args.output, len(entries), meta_source))
    return 0


def cmd_check(args):
    target = args.input
    if os.path.isfile(target) and target.lower().endswith(".o2r"):
        return check_archive(target)
    if not os.path.isdir(target):
        sys.stderr.write("error: not a directory or .o2r pack: %s\n" % target)
        return 2
    entries, errors, warnings, infos = plan_pack(target, args.allow_overbudget)
    report(errors, warnings, infos)
    if errors:
        print("check: %d error(s)" % len(errors))
        return 1
    print("check: OK (%d resource(s) would be packed)" % len(entries))
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(
        prog="gen_model_pack",
        description="Bake custom machine models (OBJ+MTL+PNG) into a deterministic .o2r pack",
        formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    pi = sub.add_parser("init", help="scaffold a work dir from a dumped stock machine model")
    pi.add_argument("machine", help="machine name, e.g. blue_falcon")
    pi.add_argument("-o", "--output", required=True, help="output work directory")
    pi.add_argument("--dump-dir", default=None,
                    help="dump models dir (default: auto-detect dump/models)")
    pp = sub.add_parser("pack", help="validate a work dir and write the .o2r pack")
    pp.add_argument("input_dir")
    pp.add_argument("-o", "--output", required=True, help="output .o2r path")
    pp.add_argument("--allow-overbudget", action="store_true",
                    help="demote LOD triangle-budget violations to warnings")
    pp.add_argument("--name", default=None)
    pp.add_argument("--author", default=None)
    pp.add_argument("--version", default=None)
    pp.add_argument("--id", default=None)
    pp.add_argument("--game-version", default=None)
    pc = sub.add_parser("check", help="validate a work dir or an existing .o2r pack")
    pc.add_argument("input", help="work directory or pack.o2r")
    pc.add_argument("--allow-overbudget", action="store_true")
    args = ap.parse_args(argv)
    if args.cmd == "init":
        return cmd_init(args)
    if args.cmd == "pack":
        return cmd_pack(args)
    return cmd_check(args)


if __name__ == "__main__":
    sys.exit(main())
