"""Tests for tools/gen_gnet_sign.py, with synthesized signs and cards only.

Run from the repository root:

    python -I -B tools/test_gen_gnet_sign.py
"""

import io
import os
import struct
import sys
import tempfile
import unittest
import zipfile
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import gen_gnet_sign as gen  # noqa: E402


def encode_png(rows, colour=6):
    """A minimal 8-bit PNG, filter 0 on every row (RGBA for colour 6, RGB for colour 2)."""
    height = len(rows)
    width = len(rows[0])

    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body) & 0xFFFFFFFF)

    raw = bytearray()
    for row in rows:
        raw.append(0)
        for pixel in row:
            raw.extend(pixel if colour == 6 else pixel[:3])
    header = struct.pack(">IIBBBBB", width, height, 8, colour, 0, 0, 0)
    return gen.PNG_MAGIC + chunk(b"IHDR", header) + chunk(b"IDAT", zlib.compress(bytes(raw))) + chunk(b"IEND", b"")


def expand(value):
    """An RGBA5551 value as the 8-bit RGBA an editor would save."""
    if not value & 1:
        return (0, 0, 0, 0)
    return tuple(((value >> shift) & 31) * 255 // 31 for shift in (11, 6, 1)) + (255,)


def synth_signs():
    """Eight signs: transparent left column, a shared body, and per-sign text colours."""
    signs = []
    for s in range(len(gen.SIGN_NAMES)):
        sign = []
        for i in range(gen.PIXELS):
            x = i % gen.WIDTH
            if x == 0:
                sign.append(0x0002)  # transparent, with a colour
            elif x < 40:
                sign.append(((x & 31) << 11) | 1)  # the body every sign shares
            else:
                sign.append(((s + 1) << 6) | ((i & 7) << 1) | 1)  # each sign's own text
        signs.append(sign)
    return signs


def write_archive(path, signs):
    with zipfile.ZipFile(path, "w") as archive:
        for name, sign in zip(gen.SIGN_NAMES, signs):
            entry = bytes(gen.ENTRY_HEADER) + struct.pack(">%dH" % gen.PIXELS, *sign)
            archive.writestr(gen.SIGN_KEY % name, entry)


class MapTests(unittest.TestCase):
    def setUp(self):
        self.signs = synth_signs()
        # The card: sign 6's art, with sign 2's text in one row, sign 3's colour moved elsewhere, two new colours
        # and one transparent pixel where the base is opaque.
        self.card = list(self.signs[6])
        for x in range(40, 64):
            self.card[10 * gen.WIDTH + x] = self.signs[2][10 * gen.WIDTH + x]
        self.card[20 * gen.WIDTH + 50] = self.signs[3][5 * gen.WIDTH + 41]
        self.card[30 * gen.WIDTH + 50] = (31 << 11) | (31 << 1) | 1
        self.card[31 * gen.WIDTH + 50] = (31 << 6) | 1
        self.card[40 * gen.WIDTH + 5] = 0

    def test_base_is_the_closest_sign(self):
        base, _, _ = gen.build_map(self.card, self.signs)
        self.assertEqual(base, 6)

    def test_rebuilds_the_card(self):
        base, copies, literals = gen.build_map(self.card, self.signs)
        rebuilt = gen.compose(base, copies, literals, self.signs)
        for i in range(gen.PIXELS):
            self.assertEqual(rebuilt[i] & 1, self.card[i] & 1, i)
            if self.card[i] & 1:
                self.assertEqual(rebuilt[i], self.card[i], i)

    def test_literals_are_only_new_colours(self):
        _, copies, literals = gen.build_map(self.card, self.signs)
        stock = {value for sign in self.signs for value in sign}
        values = sorted(value for _, value in literals)
        # The two new colours and the transparent pixel no sign has transparent at that place.
        self.assertEqual(values, sorted([0, (31 << 6) | 1, (31 << 11) | (31 << 1) | 1]))
        for value in values:
            if value:
                self.assertNotIn(value, stock)
        self.assertEqual(len(copies), 24 + 1)
        # The first place that colour appears: sign 3, row 0, x 41 (pixel 361 and pixel 41 share i & 7).
        self.assertIn((20 * gen.WIDTH + 50, 3, 41), copies)

    def test_render_c_lists_every_entry(self):
        base, copies, literals = gen.build_map(self.card, self.signs)
        source = gen.render_c(base, copies, literals)
        self.assertIn("6, /* Options */", source)
        self.assertEqual(source.count("{ "), len(copies) + len(literals))
        self.assertIn("Heebeejeebees", source)
        self.assertNotIn("\r", source)

    def test_bad_inputs(self):
        with self.assertRaises(ValueError):
            gen.build_map(self.card[:-1], self.signs)
        with self.assertRaises(ValueError):
            gen.read_png(b"not a png")


class PngTests(unittest.TestCase):
    def test_rgba_and_rgb(self):
        rows = [[(10, 20, 30, 255), (0, 0, 0, 0)], [(255, 255, 255, 255), (1, 2, 3, 128)]]
        width, height, decoded = gen.read_png(encode_png(rows))
        self.assertEqual((width, height), (2, 2))
        self.assertEqual(decoded, rows)
        _, _, rgb = gen.read_png(encode_png(rows, colour=2))
        self.assertEqual(rgb[0][1], (0, 0, 0, 255))

    def test_rgba5551(self):
        self.assertEqual(gen.to_rgba5551((0, 0, 0, 0)), 0)
        self.assertEqual(gen.to_rgba5551((255, 255, 255, 255)), 0xFFFF)
        self.assertEqual(gen.to_rgba5551(expand(0x07DB)), 0x07DB)


class MainTests(unittest.TestCase):
    def test_end_to_end(self):
        signs = synth_signs()
        card = list(signs[1])
        card[100] = (17 << 11) | (3 << 6) | (9 << 1) | 1
        with tempfile.TemporaryDirectory() as folder:
            png = os.path.join(folder, "card.png")
            archive = os.path.join(folder, "signs.o2r")
            out = os.path.join(folder, "map.c")
            rows = [[expand(card[y * gen.WIDTH + x]) for x in range(gen.WIDTH)] for y in range(gen.HEIGHT)]
            with open(png, "wb") as handle:
                handle.write(encode_png(rows))
            write_archive(archive, signs)
            stdout = sys.stdout
            sys.stdout = io.StringIO()
            try:
                self.assertEqual(gen.main(["gen", png, archive, out]), 0)
            finally:
                sys.stdout = stdout
            with open(out, encoding="ascii") as handle:
                source = handle.read()
        self.assertIn("1, /* TimeAttack */", source)
        self.assertIn("{ 100, 0x", source)

    def test_wrong_size_card(self):
        with tempfile.TemporaryDirectory() as folder:
            png = os.path.join(folder, "card.png")
            with open(png, "wb") as handle:
                handle.write(encode_png([[(0, 0, 0, 0)] * 4] * 4))
            stdout = sys.stdout
            sys.stdout = io.StringIO()
            try:
                self.assertEqual(gen.main(["gen", png, "unused", os.path.join(folder, "map.c")]), 1)
            finally:
                sys.stdout = stdout


if __name__ == "__main__":
    unittest.main()
