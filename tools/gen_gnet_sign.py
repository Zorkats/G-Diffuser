"""Turn the G-NET main menu card into a map over the stock mode signs and write it as C source.

Usage, from the repository root (both inputs are read only):

    python -I tools/gen_gnet_sign.py <card.png> <fzerox.o2r> port/gdx_gnet_sign_map.c

The card is a 64x86 RGBA PNG drawn by Heebeejeebees and shared on the G-Diffuser Discord. It is
built on the game's own mode signs: the sign holder is stock art, only the sign text and some
colours are new. Stock pixels must not live in this repository, and neither the PNG nor the
archive is committed. The output holds, for every pixel of the card:

- nothing, when it equals the base sign's pixel at the same place,
- a coordinate (sign, pixel index), when it equals a pixel of any stock sign,
- a literal RGBA5551 value only when no stock sign holds that colour anywhere, so a literal is
  always one of the artist's new colours, never a stock one.

At run time port/gdx_gnet_sign.c rebuilds the card from the player's loaded sign textures, so a
ROM hack that repaints the signs gets a matching card. Only the Python standard library is used.
"""

import struct
import sys
import zipfile
import zlib

WIDTH = 64
HEIGHT = 86
PIXELS = WIDTH * HEIGHT
ENTRY_HEADER = 0x50
ENTRY_SIZE = ENTRY_HEADER + PIXELS * 2

# The order of sMenuSignCompTexInfos in decomp/src/overlays/ovl_i6/main_menu.c: the mode order.
SIGN_NAMES = (
    "GpRace",
    "TimeAttack",
    "DeathRace",
    "VsBattle",
    "CourseEdit",
    "Practice",
    "Options",
    "CreateMachine",
)
SIGN_KEY = "common_assets_compressed/aMenuSign%sTex"

PNG_MAGIC = b"\x89PNG\r\n\x1a\n"
PNG_CHANNELS = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}
VALUES_PER_LINE = 4


def _paeth(a, b, c):
    p = a + b - c
    pa = abs(p - a)
    pb = abs(p - b)
    pc = abs(p - c)
    if pa <= pb and pa <= pc:
        return a
    return b if pb <= pc else c


def read_png(data):
    """Decode an 8-bit, non-interlaced PNG into rows of (r, g, b, a) tuples."""
    if data[:8] != PNG_MAGIC:
        raise ValueError("not a PNG file")
    pos = 8
    header = None
    palette = b""
    alphas = b""
    compressed = bytearray()
    while pos + 12 <= len(data):
        (length,) = struct.unpack(">I", data[pos : pos + 4])
        kind = data[pos + 4 : pos + 8]
        body = data[pos + 8 : pos + 8 + length]
        pos += 12 + length
        if kind == b"IHDR":
            header = struct.unpack(">IIBBBBB", body)
        elif kind == b"PLTE":
            palette = body
        elif kind == b"tRNS":
            alphas = body
        elif kind == b"IDAT":
            compressed += body
        elif kind == b"IEND":
            break
    if header is None:
        raise ValueError("PNG has no IHDR")
    width, height, depth, colour, _, _, interlace = header
    if depth != 8 or colour not in PNG_CHANNELS or interlace != 0:
        raise ValueError("only 8-bit, non-interlaced PNGs are supported")
    channels = PNG_CHANNELS[colour]
    stride = width * channels
    raw = zlib.decompress(bytes(compressed))
    if len(raw) < height * (stride + 1):
        raise ValueError("PNG image data is truncated")
    rows = []
    previous = bytearray(stride)
    offset = 0
    for _ in range(height):
        method = raw[offset]
        line = bytearray(raw[offset + 1 : offset + 1 + stride])
        offset += stride + 1
        for x in range(stride):
            left = line[x - channels] if x >= channels else 0
            up = previous[x]
            corner = previous[x - channels] if x >= channels else 0
            if method == 1:
                line[x] = (line[x] + left) & 0xFF
            elif method == 2:
                line[x] = (line[x] + up) & 0xFF
            elif method == 3:
                line[x] = (line[x] + ((left + up) >> 1)) & 0xFF
            elif method == 4:
                line[x] = (line[x] + _paeth(left, up, corner)) & 0xFF
            elif method != 0:
                raise ValueError("unknown PNG filter %d" % method)
        previous = line
        row = []
        for x in range(width):
            px = line[x * channels : (x + 1) * channels]
            if colour == 6:
                row.append(tuple(px))
            elif colour == 2:
                row.append((px[0], px[1], px[2], 255))
            elif colour == 3:
                index = px[0]
                r, g, b = palette[index * 3 : index * 3 + 3]
                row.append((r, g, b, alphas[index] if index < len(alphas) else 255))
            elif colour == 4:
                row.append((px[0], px[0], px[0], px[1]))
            else:
                row.append((px[0], px[0], px[0], 255))
        rows.append(row)
    return width, height, rows


def to_rgba5551(pixel):
    r, g, b, a = pixel
    if a < 128:
        return 0
    r, g, b = ((channel * 31 + 127) // 255 for channel in (r, g, b))
    return (r << 11) | (g << 6) | (b << 1) | 1


def read_signs(archive):
    """The eight stock signs as lists of RGBA5551 values, in mode order."""
    signs = []
    for name in SIGN_NAMES:
        entry = archive.read(SIGN_KEY % name)
        if len(entry) != ENTRY_SIZE:
            raise ValueError("%s is %d bytes, expected %d" % (name, len(entry), ENTRY_SIZE))
        signs.append(list(struct.unpack(">%dH" % PIXELS, entry[ENTRY_HEADER:])))
    return signs


def build_map(card, signs):
    """Map the card (PIXELS RGBA5551 values) over the signs.

    Returns (base, copies, literals): copies are (pixel, sign, from) and literals (pixel, value),
    both sorted by pixel. A transparent card pixel takes any stock pixel that is transparent at the
    same place, since only its alpha bit shows.
    """
    if len(card) != PIXELS or len(signs) != len(SIGN_NAMES):
        raise ValueError("card or sign count does not match")
    base = max(range(len(signs)), key=lambda s: (sum(1 for i in range(PIXELS) if signs[s][i] == card[i]), -s))
    first_place = {}
    for s, sign in enumerate(signs):
        for i, value in enumerate(sign):
            first_place.setdefault(value, (s, i))
    copies = []
    literals = []
    for i, value in enumerate(card):
        if value & 1:
            if signs[base][i] == value:
                continue
            same = [s for s in range(len(signs)) if signs[s][i] == value]
            if same:
                copies.append((i, same[0], i))
            elif value in first_place:
                s, j = first_place[value]
                copies.append((i, s, j))
            else:
                literals.append((i, value))
        else:
            if not signs[base][i] & 1:
                continue
            same = [s for s in range(len(signs)) if not signs[s][i] & 1]
            if same:
                copies.append((i, same[0], i))
            else:
                literals.append((i, 0))
    return base, copies, literals


def compose(base, copies, literals, signs):
    """The card the map rebuilds, as the C side does it."""
    out = list(signs[base])
    for pixel, sign, source in copies:
        out[pixel] = signs[sign][source]
    for pixel, value in literals:
        out[pixel] = value
    return out


def render_c(base, copies, literals):
    lines = [
        "/* port/gdx_gnet_sign_map.c -- the G-NET main menu card as a map over the stock mode signs.",
        " * Generated by tools/gen_gnet_sign.py; regenerate instead of editing.",
        " * Copyright (c) Zorkats",
        " *",
        " * Card design by Heebeejeebees. Copies are coordinates into the stock signs, never pixel values;",
        " * a literal is a colour no stock sign holds. See port/gdx_gnet_sign.h.",
        " */",
        '#include "gdx_gnet_sign.h"',
        "",
        "static const GdxGnetSignCopy sCopies[] = {",
    ]
    for start in range(0, len(copies), VALUES_PER_LINE):
        chunk = copies[start : start + VALUES_PER_LINE]
        lines.append("    " + " ".join("{ %d, %d, %d }," % entry for entry in chunk))
    lines.append("};")
    lines.append("")
    lines.append("static const GdxGnetSignLiteral sLiterals[] = {")
    for start in range(0, len(literals), VALUES_PER_LINE):
        chunk = literals[start : start + VALUES_PER_LINE]
        lines.append("    " + " ".join("{ %d, 0x%04X }," % entry for entry in chunk))
    lines.append("};")
    lines.append("")
    lines.append("const GdxGnetSignMap gGdxGnetSignMap = {")
    lines.append("    %d, /* %s */" % (base, SIGN_NAMES[base]))
    lines.append("    sCopies,")
    lines.append("    sizeof(sCopies) / sizeof(sCopies[0]),")
    lines.append("    sLiterals,")
    lines.append("    sizeof(sLiterals) / sizeof(sLiterals[0]),")
    lines.append("};")
    return "\n".join(lines) + "\n"


def main(argv):
    if len(argv) != 4:
        print(__doc__)
        return 2
    with open(argv[1], "rb") as handle:
        width, height, rows = read_png(handle.read())
    if (width, height) != (WIDTH, HEIGHT):
        print("the card must be %dx%d, got %dx%d" % (WIDTH, HEIGHT, width, height))
        return 1
    card = [to_rgba5551(pixel) for row in rows for pixel in row]
    with zipfile.ZipFile(argv[2]) as archive:
        signs = read_signs(archive)
    base, copies, literals = build_map(card, signs)
    rebuilt = compose(base, copies, literals, signs)
    for i in range(PIXELS):
        same_alpha = (rebuilt[i] & 1) == (card[i] & 1)
        if not same_alpha or (card[i] & 1 and rebuilt[i] != card[i]):
            print("internal error: pixel %d does not rebuild" % i)
            return 1
    with open(argv[3], "w", encoding="ascii", newline="\n") as handle:
        handle.write(render_c(base, copies, literals))
    print("base %s, %d copies, %d literals" % (SIGN_NAMES[base], len(copies), len(literals)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
