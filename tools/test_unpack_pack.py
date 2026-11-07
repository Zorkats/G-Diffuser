#!/usr/bin/env python3
# Copyright (c) Zorkats
"""Tests for tools/unpack_pack.py and the table decoders in tools/n64_texels.py. Every fixture is built
here (synthetic textures, palettes and zip archives). No ROM or game data is read.

Run: python -I -B tools/test_unpack_pack.py   (needs Pillow, as gen_texture_pack.py does)
Scratch files go under %TEMP%\\gdx_unpack_out only.
"""
import contextlib
import io
import json
import os
import random
import shutil
import struct
import sys
import tempfile
import unittest
import zipfile

TOOLS_DIR = os.path.dirname(os.path.abspath(__file__))
if TOOLS_DIR not in sys.path:
    sys.path.insert(0, TOOLS_DIR)

from PIL import Image  # noqa: E402

import gen_texture_pack as packer  # noqa: E402
import n64_texels  # noqa: E402
import unpack_pack  # noqa: E402

SCRATCH_ROOT = os.path.join(tempfile.gettempdir(), "gdx_unpack_out")


def pattern(width, height, seed):
    rng = random.Random(seed)
    return [(rng.randrange(256), rng.randrange(256), rng.randrange(256), rng.choice((0, 255)))
            for _ in range(width * height)]


def save_png(path, width, height, pixels):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    image = Image.new("RGBA", (width, height))
    image.putdata(pixels)
    image.save(path)


def write_zip(path, members):
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as z:
        for name in sorted(members):
            z.writestr(name, members[name])


def run_packer(argv):
    out, err = io.StringIO(), io.StringIO()
    saved = sys.argv
    sys.argv = ["gen_texture_pack.py"] + argv
    try:
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            code = packer.main()
    finally:
        sys.argv = saved
    return code, out.getvalue() + err.getvalue()


def quiet(_line):
    pass


class Scratch(unittest.TestCase):
    def setUp(self):
        os.makedirs(SCRATCH_ROOT, exist_ok=True)
        self.dir = tempfile.mkdtemp(prefix="case-", dir=SCRATCH_ROOT)

    def tearDown(self):
        shutil.rmtree(self.dir, ignore_errors=True)

    def path(self, *parts):
        return os.path.join(self.dir, *parts)


class TableDecoders(unittest.TestCase):
    def test_table_decoders_match_the_reference(self):
        rng = random.Random(7)
        for fmt in ("RGBA16", "RGBA32", "IA16", "IA8", "IA4", "I8", "I4"):
            width, height = 6, 5
            payload = bytes(rng.randrange(256) for _ in range(n64_texels.texel_bytes(fmt, width * height)))
            self.assertEqual(n64_texels.decode(fmt, payload, width, height),
                             n64_texels.DECODERS[fmt](payload, width, height), fmt)

    def test_odd_width_four_bit_rows_are_padded_like_the_packer(self):
        pixels = [(17 * (i % 16),) * 3 + (255,) for i in range(15)]
        image = Image.new("RGBA", (5, 3))
        image.putdata(pixels)
        data, _ = packer.enc_i4(image.load(), 5, 3)
        self.assertEqual(len(data), 9)  # 3 bytes per row of 5 texels
        rgba = n64_texels.decode("I4", data, 5, 3)
        self.assertEqual(len(rgba), 5 * 3 * 4)
        self.assertEqual([rgba[i * 4] for i in range(15)], [p[0] for p in pixels])

    def test_indexed_uses_the_palette_or_grey(self):
        colors = [(i, 255 - i, 7, 255) for i in range(16)]
        payload = bytes((0x01, 0x23))
        rgba = n64_texels.decode_indexed("CI4", payload, 4, 1, colors)
        self.assertEqual(rgba, b"".join(bytes(colors[i]) for i in (0, 1, 2, 3)))
        grey = n64_texels.decode_indexed("CI4", payload, 4, 1)
        self.assertEqual(grey[4:8], bytes((17, 17, 17, 255)))

    def test_short_payload_is_refused(self):
        with self.assertRaises(ValueError):
            n64_texels.decode("RGBA16", b"\x00" * 7, 2, 2)


class RoundTrip(Scratch):
    """Pack a synthetic dump, unpack the pack, pack the unpacked folder again: the bytes must match."""

    def make_dump(self):
        dump = self.path("dump")
        rows = []
        save_png(os.path.join(dump, "seg", "aColorTex.png"), 8, 4, pattern(8, 4, 1))
        rows.append("seg/aColorTex\t4\t2\tRGBA16")  # packed at 2x
        save_png(os.path.join(dump, "seg", "aGlowTex.png"), 6, 3, pattern(6, 3, 2))
        rows.append("seg/aGlowTex\t6\t3\tIA8")
        save_png(os.path.join(dump, "seg", "aFontTex.png"), 5, 2, pattern(5, 2, 3))
        rows.append("seg/aFontTex\t5\t2\tI4")
        palette = [(i * 16, 255 - i * 16, (i * 37) % 256, 255) for i in range(16)]
        save_png(os.path.join(dump, "seg", "aPal.png"), 16, 1, palette)
        rows.append("seg/aPal\t16\t1\tTLUT")
        save_png(os.path.join(dump, "seg", "aIconTex.png"), 4, 4, [palette[i % 16] for i in range(16)])
        rows.append("seg/aIconTex\t4\t4\tCI4\tseg/aPal")
        with open(os.path.join(dump, "manifest.tsv"), "w", encoding="utf-8", newline="\n") as fh:
            fh.write("# key\tnative_w\tnative_h\tn64_fmt\n" + "\n".join(rows) + "\n")
        return dump

    def pack_entries(self, path):
        with zipfile.ZipFile(path) as z:
            return {n: z.read(n) for n in z.namelist() if n != "workshop.json"}

    def test_unpacked_textures_pack_back_to_the_same_bytes(self):
        dump = self.make_dump()
        first = self.path("first.o2r")
        code, text = run_packer([dump, first, "--name", "Synthetic", "--author", "tests", "--id", "tests.synth"])
        self.assertEqual(code, 0, text)
        out = self.path("unpacked")
        result = unpack_pack.unpack(first, out, dump, quiet)
        self.assertEqual(result["failed"], 0)
        self.assertEqual(result["title"], "Synthetic")
        self.assertTrue(os.path.isfile(os.path.join(out, "workshop.json")))
        self.assertTrue(os.path.isfile(os.path.join(out, "contents.tsv")))
        textures = os.path.join(out, "textures")
        self.assertTrue(os.path.isfile(os.path.join(textures, "seg", "aIconTex.png")))
        self.assertTrue(os.path.isfile(os.path.join(textures, "seg", "aPal.png")))
        with open(os.path.join(textures, "manifest.tsv"), encoding="utf-8") as fh:
            manifest = fh.read()
        self.assertIn("seg/aColorTex\t4\t2\tRGBA16\t-", manifest)
        self.assertIn("seg/aIconTex\t4\t4\tCI4\tseg/aPal", manifest)
        second = self.path("second.o2r")
        code, text = run_packer([textures, second, "--name", "Again", "--author", "tests", "--id", "tests.again"])
        self.assertEqual(code, 0, text)
        self.assertEqual(self.pack_entries(first), self.pack_entries(second))

    def test_without_the_dump_paletted_textures_are_grey(self):
        dump = self.make_dump()
        pack = self.path("pack.o2r")
        code, text = run_packer([dump, pack, "--name", "Synthetic", "--author", "tests", "--id", "tests.synth"])
        self.assertEqual(code, 0, text)
        listing = unpack_pack.listing_result(unpack_pack.scan(pack))
        icon = [row for row in listing["entries"] if row.startswith("textures/pack/seg/aIconTex\t")]
        self.assertEqual(len(icon), 1)
        self.assertIn("indices shown as grey", icon[0])
        self.assertTrue(any("grey" in line for line in listing["summary"]))
        out = self.path("unpacked")
        unpack_pack.unpack(pack, out, None, quiet)
        with Image.open(os.path.join(out, "textures", "seg", "aIconTex.png")) as image:
            r, g, b, a = image.convert("RGBA").getpixel((1, 0))
        self.assertEqual((r, g, b, a), (17, 17, 17, 255))


class Kinds(Scratch):
    def test_files_are_copied_and_labelled(self):
        pack = self.path("mixed.o2r")
        otex = packer.build_otex_v1(packer.TT_RGBA32, 1, 1, 1.0, 1.0, b"\x01\x02\x03\x04")
        write_zip(pack, {
            "workshop.json": json.dumps({"name": "Mixed", "author": "tests", "id": "tests.mixed",
                                         "depends": ["tests.base"]}),
            "scripts/lap.lua": b"print('lap')\n",
            "audio/sample/x": b"GSMP" + b"\x00" * 60,
            "audio/font/y": b"GFT1" + b"\x00" * 60,
            "alt/textures/pack/seg/aTex": otex,
            "hud/aBareTex": otex,
            "blob.bin": b"\x00\x01",
        })
        listing = unpack_pack.scan(pack)
        kinds = {e["path"]: e["kind"] for e in listing["entries"]}
        self.assertEqual(kinds["scripts/lap.lua"], "Lua script")
        self.assertEqual(kinds["audio/sample/x"], "N64 sample (VADPCM)")
        self.assertEqual(kinds["audio/font/y"], "Soundfont")
        self.assertEqual(kinds["alt/textures/pack/seg/aTex"], "Texture")
        self.assertEqual(kinds["blob.bin"], "Binary file")
        summary = unpack_pack.summary_lines(listing)
        self.assertIn("Needs: tests.base", summary)
        out = self.path("unpacked")
        unpack_pack.unpack(pack, out, None, quiet)
        with open(os.path.join(out, "files", "scripts", "lap.lua"), "rb") as fh:
            self.assertEqual(fh.read(), b"print('lap')\n")
        self.assertTrue(os.path.isfile(os.path.join(out, "alt-textures", "seg", "aTex.png")))
        self.assertTrue(os.path.isfile(os.path.join(out, "textures", "hud", "aBareTex.png")))

    def test_track_package(self):
        package = self.path("cup.gdxp")
        write_zip(package, {"cup.gdxc": b"GDXC", "cup.gcs": b"GCS", "song.ogg": b"OggS"})
        result = unpack_pack.listing_result(unpack_pack.scan(package))
        self.assertEqual(result["format"], "gdxp")
        self.assertEqual(sorted(row.split("\t")[1] for row in result["entries"]),
                         ["Audio", "Track", "Track snapshot"])


class Safety(Scratch):
    def test_unsafe_names_are_skipped_and_nothing_escapes(self):
        pack = self.path("evil.o2r")
        with zipfile.ZipFile(pack, "w") as z:
            for name in ("../escape.txt", "a/../../escape2.txt", "C:/x.txt", "CON.txt", "..\\escape3.txt",
                         "trailing./x.txt", "ok/fine.txt"):
                # On Windows zipfile reads "\" as "/", so "..\" must be refused on every platform.
                info = zipfile.ZipInfo(name)
                info.filename = name
                z.writestr(info, b"x")
        out = self.path("deep", "unpacked")
        os.makedirs(self.path("deep"))
        result = unpack_pack.unpack(pack, out, None, quiet)
        self.assertEqual(result["written"], 1)
        self.assertEqual(result["failed"], 6)
        self.assertFalse(os.path.exists(self.path("escape.txt")))
        self.assertFalse(os.path.exists(self.path("deep", "escape.txt")))
        self.assertFalse(os.path.exists(self.path("deep", "escape3.txt")))
        self.assertTrue(os.path.isfile(os.path.join(out, "files", "ok", "fine.txt")))
        self.assertTrue(any("unsafe" in line for line in result["summary"]))

    def test_names_that_differ_in_case_write_once(self):
        pack = self.path("case.o2r")
        write_zip(pack, {"Docs/A.txt": b"1", "docs/a.txt": b"2"})
        result = unpack_pack.unpack(pack, self.path("out"), None, quiet)
        self.assertEqual((result["written"], result["failed"]), (1, 1))

    def test_existing_output_is_refused(self):
        pack = self.path("p.o2r")
        write_zip(pack, {"a.txt": b"1"})
        os.makedirs(self.path("out"))
        with self.assertRaises(unpack_pack.UnpackError):
            unpack_pack.unpack(pack, self.path("out"), None, quiet)

    def test_not_a_zip(self):
        bogus = self.path("pack.otr")
        with open(bogus, "wb") as fh:
            fh.write(b"MPQ\x1a" + b"\x00" * 64)
        with self.assertRaises(unpack_pack.UnpackError):
            unpack_pack.scan(bogus)

    def test_oversized_entry_is_not_written(self):
        pack = self.path("big.o2r")
        write_zip(pack, {"big.bin": b"\x00" * 4096, "small.bin": b"1"})
        saved = unpack_pack.MAX_ENTRY_BYTES
        unpack_pack.MAX_ENTRY_BYTES = 1024
        try:
            result = unpack_pack.unpack(pack, self.path("out"), None, quiet)
        finally:
            unpack_pack.MAX_ENTRY_BYTES = saved
        self.assertEqual((result["written"], result["failed"]), (1, 1))
        self.assertFalse(os.path.exists(self.path("out", "files", "big.bin")))

    def test_huge_texture_dimensions_are_refused_from_the_header(self):
        pack = self.path("dims.o2r")
        header = packer.build_otex_v1(packer.TT_RGBA32, 100000, 100000, 1.0, 1.0, b"")
        write_zip(pack, {"textures/pack/x": header})
        listing = unpack_pack.scan(pack)
        self.assertIn("outside 1 to", listing["entries"][0]["detail"])
        result = unpack_pack.unpack(pack, self.path("out"), None, quiet)
        self.assertEqual(result["failed"], 0)  # copied raw, never decoded
        self.assertTrue(os.path.isfile(self.path("out", "files", "textures", "pack", "x")))

    def test_listing_stays_small(self):
        pack = self.path("many.o2r")
        write_zip(pack, {"f%05d/%s.txt" % (i, "x" * 80): b"" for i in range(1500)})
        result = unpack_pack.listing_result(unpack_pack.scan(pack))
        self.assertEqual(result["total"], 1500)
        self.assertLess(len(result["entries"]), 1500)
        self.assertLess(len(json.dumps(result)), 60000)


if __name__ == "__main__":
    unittest.main()
