#!/usr/bin/env python3
"""Tests for tools/gen_htc_pack.py. Every fixture is built here (fake OTEX resources, a fake .htc
cache and tiny zip archives). No ROM or texture data is read.

Run: python -I -B tools/test_gen_htc_pack.py
Scratch files go under %TEMP%\\gdx_htc_out only.
"""
import contextlib
import gzip
import hashlib
import io
import json
import os
import shutil
import struct
import sys
import tempfile
import unittest
import zipfile
import zlib

TOOLS_DIR = os.path.dirname(os.path.abspath(__file__))
if TOOLS_DIR not in sys.path:
    sys.path.insert(0, TOOLS_DIR)

import gen_htc_pack as htc  # noqa: E402

packer = htc.packer
SCRATCH_ROOT = os.path.join(tempfile.gettempdir(), "gdx_htc_out")


def make_key(low, high=0):
    return (high << 32) | low


def otex_v1(tex_type, width, height, payload):
    return packer.build_otex_v1(tex_type, width, height, 1.0, 1.0, payload)


def otex_v0(tex_type, width, height, payload):
    header = bytearray(packer.OTR_HEADER_SIZE)
    header[0] = 0
    header[1] = 1
    struct.pack_into("<I", header, 4, packer.OTR_TYPE_TEXTURE)
    struct.pack_into("<I", header, 8, 0)
    return bytes(header) + struct.pack("<IIII", tex_type, width, height, len(payload)) + payload


def htc_entry(key, width, height, body, fmt=0x00008058, texfmt=htc.GL_RGBA,
              pixtype=htc.GL_UNSIGNED_BYTE, versioned=False):
    out = struct.pack("<QIIIHHB", key, width, height, fmt, texfmt, pixtype, 1)
    if versioned:
        out += struct.pack("<H", 0)  # formatsize
    return out + struct.pack("<I", len(body)) + body


def zlib_entry(key, width, height, payload, versioned=False):
    return htc_entry(key, width, height, zlib.compress(payload), fmt=0x80008058, versioned=versioned)


def htc_file(entries, versioned=False):
    head = struct.pack("<I", 0x40820000)  # config word
    if versioned:
        head = struct.pack("<I", htc.HTC_VERSIONED_MARKER) + head
    return gzip.compress(head + b"".join(entries), mtime=0)


def write_zip(path, members):
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as z:
        for name in sorted(members):
            z.writestr(name, members[name])


def run_quiet(argv):
    out, err = io.StringIO(), io.StringIO()
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
        code = htc.main(argv)
    return code, out.getvalue(), err.getvalue()


def rgba_pattern(width, height, seed=0):
    return bytes((i * 7 + seed) & 0xFF for i in range(width * height * 4))


def rice_for(payload, width, height, siz):
    return htc.rice_crc(payload, width, height, siz, (width << siz) >> 1)


class ScratchCase(unittest.TestCase):
    def setUp(self):
        os.makedirs(SCRATCH_ROOT, exist_ok=True)
        self.dir = tempfile.mkdtemp(prefix="htc_test_", dir=SCRATCH_ROOT)

    def tearDown(self):
        shutil.rmtree(self.dir, ignore_errors=True)

    def path(self, name):
        return os.path.join(self.dir, name)


class RiceCrcTests(unittest.TestCase):
    def test_known_answer_one_row_rgba32(self):
        # RGBA32 2x1: bpl = (2 << 3) >> 1 = 8. Words at offsets 4 then 0 are 2 then 1.
        # Offset 4: esi = 2 ^ 4 = 6, crc = 0 + 6 = 6.
        # Offset 0: esi = 1 ^ 0 = 1, crc = rol4(6) + 1 = 0x60 + 1 = 0x61.
        # Row end: esi = 1 ^ y (y = 0) = 1, crc = 0x61 + 1 = 0x62.
        payload = struct.pack(">II", 1, 2)
        self.assertEqual(htc.rice_crc(payload, 2, 1, 3, 8), 0x62)

    def test_known_answer_two_rows_rgba16(self):
        # RGBA16 2x2: bpl = (2 << 2) >> 1 = 4, stride 4. Row y = 1 reads the first stored row.
        # Row y = 1: esi = 3 ^ 0 = 3, crc = 3; esi ^= 1 gives 2, crc = 5.
        # Row y = 0: esi = 5 ^ 0 = 5, crc = rol4(5) + 5 = 0x50 + 5 = 0x55; esi ^= 0, crc = 0x55 + 5 = 0x5A.
        payload = struct.pack(">II", 3, 5)
        self.assertEqual(htc.rice_crc(payload, 2, 2, 2, 4), 0x5A)

    def test_narrow_rows_and_short_buffers_return_none(self):
        self.assertIsNone(htc.rice_crc(b"\0" * 4, 1, 1, 2, 2))  # bpl = 2 < 4
        self.assertIsNone(htc.rice_crc(b"\0" * 4, 2, 2, 2, 4))  # second row runs past the buffer


class ParseHtcTests(unittest.TestCase):
    def test_original_layout_with_plain_and_zlib_entries(self):
        raw = htc_file([
            htc_entry(make_key(0x1111), 2, 2, bytes(range(16))),
            zlib_entry(make_key(0x2222, 0x7), 1, 1, bytes([9, 8, 7, 6])),
        ])
        versioned, entries = htc.parse_htc(raw)
        self.assertFalse(versioned)
        self.assertEqual([e["reason"] for e in entries], [None, None])
        self.assertEqual((entries[0]["low"], entries[0]["high"]), (0x1111, 0))
        self.assertEqual((entries[1]["low"], entries[1]["high"]), (0x2222, 0x7))
        self.assertEqual(bytes(htc.decode_htc_payload(entries[1])), bytes([9, 8, 7, 6]))

    def test_versioned_layout_reads_the_formatsize_word(self):
        raw = htc_file([
            htc_entry(make_key(0x1111), 2, 2, bytes(range(16)), versioned=True),
            zlib_entry(make_key(0x2222), 1, 1, bytes([1, 2, 3, 4]), versioned=True),
        ], versioned=True)
        versioned, entries = htc.parse_htc(raw)
        self.assertTrue(versioned)
        self.assertEqual([e["reason"] for e in entries], [None, None])
        self.assertEqual(entries[1]["low"], 0x2222)

    def test_unsupported_and_bad_entries_are_flagged_without_stopping_the_scan(self):
        raw = htc_file([
            htc_entry(make_key(0x10), 1, 1, b"\x01\x02\x03", texfmt=0x1907),  # GL_RGB
            htc_entry(make_key(0x20), 1, 1, b"not zlib", fmt=0x80008058),  # flagged, not zlib
            htc_entry(make_key(0x30), 2, 2, bytes(4)),  # 4 bytes for a 2x2 RGBA8 image
            htc_entry(make_key(0x40), 1, 1, bytes([1, 2, 3, 4])),
        ])
        _, entries = htc.parse_htc(raw)
        self.assertEqual([e["reason"] for e in entries],
                         ["unsupported_format", "bad_data", "bad_data", None])
        self.assertIn("0x1907", entries[0]["detail"])

    def test_truncated_tail_is_bad_data_and_ends_the_scan(self):
        tail = struct.pack("<QIIIHHB", make_key(0x50), 1, 1, 0x8058, 0x1908, 0x1401, 1)
        tail += struct.pack("<I", 100) + b"abcd"  # claims 100 bytes, has 4
        raw = htc_file([htc_entry(make_key(0x40), 1, 1, bytes(4)), tail,
                        htc_entry(make_key(0x60), 1, 1, bytes(4))])
        _, entries = htc.parse_htc(raw)
        self.assertEqual(len(entries), 2)
        self.assertEqual(entries[1]["reason"], "bad_data")

    def test_non_gzip_input_raises_value_error(self):
        with self.assertRaises(ValueError):
            htc.parse_htc(b"definitely not gzip")


class ReadBaseTextureTests(unittest.TestCase):
    def test_layout_rules_per_native_type(self):
        ok = htc.read_base_texture("a", otex_v1(packer.TT_RGBA16, 4, 2, bytes(16)), "x.o2r")
        narrow = htc.read_base_texture("b", otex_v1(packer.TT_RGBA16, 2, 1, bytes(4)), "x.o2r")
        i4 = htc.read_base_texture("c", otex_v1(packer.TT_I4, 8, 2, bytes(8)), "x.o2r")
        rgba32 = htc.read_base_texture("d", otex_v1(packer.TT_RGBA32, 4, 1, bytes(16)), "x.o2r")
        rgba32_bad = htc.read_base_texture("e", otex_v1(packer.TT_RGBA32, 2, 1, bytes(8)), "x.o2r")
        self.assertTrue(ok["layout_ok"])
        self.assertFalse(narrow["layout_ok"])  # 4-byte rows, not a multiple of 8
        self.assertFalse(i4["layout_ok"])  # 4-byte rows
        self.assertTrue(rgba32["layout_ok"])  # 16-byte rows, RGBA32 unit is 16
        self.assertFalse(rgba32_bad["layout_ok"])  # 8-byte rows, not a multiple of 16

    def test_v0_and_non_texture_resources(self):
        rec = htc.read_base_texture("t", otex_v0(packer.TT_IA8, 8, 2, bytes(16)), "x.o2r")
        self.assertEqual((rec["type_name"], rec["width"], rec["height"]), ("IA8", 8, 2))
        self.assertIsNone(htc.read_base_texture("n", b"not a resource at all, just bytes", "x.o2r"))


class MatchTests(ScratchCase):
    def build_scenario(self):
        a_payload = bytes(range(16))  # RGBA16 4x2
        b_payload = bytes(range(32, 48))  # IA8 8x2
        c_payload = bytes(range(64, 80))  # CI4 16x2, one palette in the cache, highest index 15
        d_payload = bytes(range(96, 112))  # CI4 16x2, several palettes in the cache, highest index 15
        e_payload = bytes(range(128, 132))  # RGBA16 2x1, 4-byte rows
        crc_a = rice_for(a_payload, 4, 2, 2)
        crc_b = rice_for(b_payload, 8, 2, 1)
        crc_c = rice_for(c_payload, 16, 2, 0)
        crc_d = rice_for(d_payload, 16, 2, 0)
        crc_e = rice_for(e_payload, 2, 1, 2)
        members = {
            "textures/a": otex_v1(packer.TT_RGBA16, 4, 2, a_payload),
            "textures/b": otex_v0(packer.TT_IA8, 8, 2, b_payload),
            "textures/c": otex_v1(packer.TT_PAL4, 16, 2, c_payload),
            "textures/d": otex_v1(packer.TT_PAL4, 16, 2, d_payload),
            "textures/e": otex_v1(packer.TT_RGBA16, 2, 1, e_payload),
            "misc/notes.txt": b"not a texture",
        }
        base = self.path("fzerox.o2r")
        write_zip(base, members)
        entries = [
            htc_entry(make_key(crc_a), 8, 4, rgba_pattern(8, 4, 1)),  # 0: converts textures/a
            htc_entry(make_key(crc_b), 16, 4, rgba_pattern(16, 4, 2)),  # 1: converts textures/b
            htc_entry(make_key(crc_c), 16, 4, rgba_pattern(16, 4, 3)),  # 2: textures/c, plain and variant
            htc_entry(make_key(crc_d, 5), 16, 4, rgba_pattern(16, 4, 4)),  # 3: palette variant
            htc_entry(make_key(crc_d, 6), 16, 4, rgba_pattern(16, 4, 5)),  # 4: palette variant
            htc_entry(make_key(crc_a), 8, 4, rgba_pattern(8, 4, 6)),  # 5: duplicate of 0
            htc_entry(make_key(crc_e), 4, 2, rgba_pattern(4, 2, 7)),  # 6: 4-byte rows
            htc_entry(make_key(0xDEADBEEF), 1, 1, rgba_pattern(1, 1)),  # 7: no base
            htc_entry(make_key(crc_a), 1, 1, b"\x01\x02\x03", texfmt=0x1907),  # 8: GL_RGB
        ]
        htc_path = self.path("pack.htc")
        with open(htc_path, "wb") as fh:
            fh.write(htc_file(entries))
        return htc_path, base

    def test_matching_reasons_and_paths(self):
        htc_path, base = self.build_scenario()
        with open(htc_path, "rb") as fh:
            _, entries = htc.parse_htc(fh.read())
        textures, seen = htc.read_base_textures([base])
        self.assertEqual(seen, 5)
        matches, skipped = htc.plan_conversion(entries, textures)
        self.assertEqual([m["path"] for m in matches],
                         ["textures/a", "textures/b", "textures/c", "textures/c", "textures/d", "textures/d"])
        self.assertEqual([m["entry"]["index"] for m in matches], [0, 1, 2, 2, 3, 4])
        # textures/c: one palette CRC (0), so a plain match and its variant. textures/d: one match per
        # palette CRC. Both carry the texture's highest index (0x4F and 0x6F -> 15).
        self.assertEqual([(m.get("palette"), m.get("cimax")) for m in matches],
                         [(None, None), (None, None), (None, None), (0, 15), (5, 15), (6, 15)])
        by_index = {s["htc_index"]: s for s in skipped}
        self.assertNotIn(3, by_index)
        self.assertNotIn(4, by_index)
        self.assertEqual(by_index[5]["reason"], "duplicate")
        self.assertEqual(by_index[6]["reason"], "unsupported_layout")
        self.assertEqual(by_index[6]["native_type"], "RGBA16")
        self.assertEqual(by_index[7]["reason"], "no_match")
        self.assertIsNone(by_index[7]["native_type"])
        self.assertEqual(by_index[8]["reason"], "unsupported_format")
        self.assertNotIn(0, by_index)
        self.assertNotIn(2, by_index)

    def test_later_base_archive_wins_for_the_same_path(self):
        old_payload = bytes(range(16))
        new_payload = bytes(range(100, 116))
        old_archive = self.path("old.o2r")
        new_archive = self.path("new.o2r")
        write_zip(old_archive, {"textures/a": otex_v1(packer.TT_RGBA16, 4, 2, old_payload)})
        write_zip(new_archive, {"textures/a": otex_v1(packer.TT_RGBA16, 4, 2, new_payload)})
        textures, seen = htc.read_base_textures([old_archive, new_archive])
        self.assertEqual(seen, 2)
        self.assertEqual(textures["textures/a"]["archive"], "new.o2r")
        self.assertEqual(textures["textures/a"]["crc"], rice_for(new_payload, 4, 2, 2))
        entries = [
            htc.make_entry(0, make_key(rice_for(old_payload, 4, 2, 2)), 8, 4, 0x1908, 0x1401,
                           False, rgba_pattern(8, 4), None, ""),
            htc.make_entry(1, make_key(rice_for(new_payload, 4, 2, 2)), 8, 4, 0x1908, 0x1401,
                           False, rgba_pattern(8, 4), None, ""),
        ]
        matches, skipped = htc.plan_conversion(entries, textures)
        self.assertEqual([m["entry"]["index"] for m in matches], [1])
        self.assertEqual([s["reason"] for s in skipped], ["no_match"])


class PaletteTests(ScratchCase):
    def test_ci_max_per_type(self):
        self.assertEqual(htc.ci_max(packer.TT_PAL4, bytes([0x12, 0x30])), 3)
        self.assertEqual(htc.ci_max(packer.TT_PAL4, bytes([0x0F, 0x00])), 15)
        self.assertEqual(htc.ci_max(packer.TT_PAL8, bytes([5, 200, 7])), 200)
        self.assertEqual(htc.ci_max(packer.TT_PAL4, b""), 0)
        self.assertIsNone(htc.ci_max(packer.TT_RGBA16, bytes([0xFF, 0xFF])))

    def test_palette_crc_known_answers(self):
        # Two entries: one aligned read, the big-endian word 0x12345678; crc = word, then + word.
        self.assertEqual(htc.palette_crc(bytes([0x12, 0x34, 0x56, 0x78]), 2), 0x2468ACF0)
        pal = bytes([0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77])
        # Three entries: one read at x = 2, host bytes 2..5 = N64 bytes 1, 0, 7, 6 (i ^ 3), so the
        # word is 0x66770011, xor x = 0x66770013, doubled.
        self.assertEqual(htc.palette_crc(pal, 3), 0xCCEE0026)
        # Four entries: reads at x = 4 then x = 0, both aligned.
        # 0x44556677 ^ 4 = 0x44556673; rotl4 -> 0x45566734; + 0x00112233 = 0x45678967; + 0x00112233.
        self.assertEqual(htc.palette_crc(pal, 4), 0x4578AB9A)

    def test_palette_crc_refusals(self):
        pal = bytes(range(8))
        self.assertIsNone(htc.palette_crc(pal, 1))
        self.assertIsNone(htc.palette_crc(pal, 0))
        self.assertIsNone(htc.palette_crc(pal[:7], 3))  # the read at x = 2 touches N64 byte 7
        self.assertIsNotNone(htc.palette_crc(pal, 3))
        self.assertIsNone(htc.palette_crc(bytes(32), 17))
        self.assertIsNotNone(htc.palette_crc(bytes(32), 16))

    def test_palette_archive_paths(self):
        self.assertEqual(htc.palette_leaf(0x0F, 0x52C9D728), "0F-52C9D728")
        self.assertEqual(htc.archive_path("t/d", htc.PACK_CHANNEL, 0xAB, 5), "textures/pack/t/d.pal/05-000000AB")
        self.assertEqual(htc.archive_path("t/d", htc.ALT_CHANNEL, 0xAB, 5),
                         "alt/textures/pack/t/d.pal/05-000000AB")
        self.assertEqual(htc.archive_path("t/d", htc.DIRECT_CHANNEL, 0xAB, 5), "textures/pack/t/d.pal/05-000000AB")
        self.assertEqual(htc.archive_path("t/d", htc.DIRECT_CHANNEL), "t/d")

    def test_index_zero_only_ci_texture_is_skipped(self):
        payload = bytes(16)  # CI4 16x2, every texel index 0
        base = self.path("fzerox.o2r")
        write_zip(base, {"textures/z": otex_v1(packer.TT_PAL4, 16, 2, payload)})
        crc = rice_for(payload, 16, 2, 0)
        entries = [
            htc.make_entry(0, make_key(crc, 1), 16, 4, 0x1908, 0x1401, False, rgba_pattern(16, 4), None, ""),
            htc.make_entry(1, make_key(crc, 2), 16, 4, 0x1908, 0x1401, False, rgba_pattern(16, 4), None, ""),
        ]
        textures, _ = htc.read_base_textures([base])
        matches, skipped = htc.plan_conversion(entries, textures)
        self.assertEqual(matches, [])
        self.assertEqual([s["reason"] for s in skipped], ["ci_palette_variants", "ci_palette_variants"])
        self.assertIn("at least two colours", skipped[0]["detail"])

    def test_variant_pack_end_to_end(self):
        payload = bytes([0x01, 0x23, 0x45, 0x00] * 4)  # CI4 16x2, highest index 5
        base = self.path("fzerox.o2r")
        write_zip(base, {"textures/d": otex_v1(packer.TT_PAL4, 16, 2, payload)})
        crc = rice_for(payload, 16, 2, 0)
        entries = [
            htc_entry(make_key(crc, 0x52C9D728), 32, 4, rgba_pattern(32, 4, 1)),
            htc_entry(make_key(crc, 0x00000007), 32, 4, rgba_pattern(32, 4, 2)),
            htc_entry(make_key(crc, 0x52C9D728), 32, 4, rgba_pattern(32, 4, 3)),  # duplicate of 0
        ]
        htc_path = self.path("pack.htc")
        with open(htc_path, "wb") as fh:
            fh.write(htc_file(entries))
        out = self.path("out.o2r")
        report = self.path("report.json")
        code, stdout, _ = run_quiet([htc_path, "--base", base, out, "--name", "V", "--author", "t",
                                     "--json", report])
        self.assertEqual(code, 0)
        with zipfile.ZipFile(out, "r") as z:
            members = {name: z.read(name) for name in z.namelist()}
        self.assertEqual(sorted(members), ["textures/pack/textures/d.pal/05-00000007",
                                           "textures/pack/textures/d.pal/05-52C9D728", "workshop.json"])
        blob = members["textures/pack/textures/d.pal/05-52C9D728"]
        # Raw RGBA32 like any CI replacement: kw = kh = 2, hByteScale = 32 * 2 / 4 = 16.
        self.assertEqual(struct.unpack_from("<IIII", blob, 64), (packer.TT_RGBA32, 32, 4, 1))
        self.assertEqual(struct.unpack_from("<ff", blob, 80), (16.0, 2.0))
        self.assertEqual(blob[92:], rgba_pattern(32, 4, 1))
        self.assertEqual(members["textures/pack/textures/d.pal/05-00000007"][92:], rgba_pattern(32, 4, 2))
        with open(report, "r", encoding="utf-8") as fh:
            data = json.load(fh)
        self.assertEqual(data["totals"]["palette_variant_paths"], 2)
        self.assertEqual([m["palette"] for m in data["matched"]], ["05-00000007", "05-52C9D728"])
        self.assertEqual(data["by_reason"]["duplicate"], 1)
        self.assertEqual(data["by_reason"]["ci_palette_variants"], 0)
        self.assertIn("2 palette variant(s)", stdout)
        check_out, check_err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(check_out), contextlib.redirect_stderr(check_err):
            self.assertEqual(packer.check_pack(out), 0)


class SinglePaletteTests(ScratchCase):
    """A CI texture the .htc holds under one palette CRC ships the plain override and one variant."""
    payload = bytes([0x01, 0x23, 0x45, 0x00] * 4)  # CI4 16x2, highest index 5
    palette = 0x52C9D728
    variant = "05-52C9D728"

    def convert(self, members, entries, extra=(), name="out.o2r"):
        """Writes a base archive and a .htc, then runs the converter. Returns (exit code, pack members,
        report). The pack members and report are empty and None when the exit code is not 0."""
        base = self.path("fzerox.o2r")
        write_zip(base, members)
        htc_path = self.path("pack.htc")
        with open(htc_path, "wb") as fh:
            fh.write(htc_file(entries))
        out = self.path(name)
        report = self.path(name + ".json")
        code, _, _ = run_quiet([htc_path, "--base", base, out, "--name", "S", "--author", "t",
                                "--json", report] + list(extra))
        if code != 0:
            return code, {}, None
        with open(report, "r", encoding="utf-8") as fh:
            data = json.load(fh)
        with zipfile.ZipFile(out, "r") as z:
            pack = {n: z.read(n) for n in z.namelist()}
        return code, pack, data

    def test_plain_override_and_variant_ship_the_same_image(self):
        crc = rice_for(self.payload, 16, 2, 0)
        entries = [htc_entry(make_key(crc, self.palette), 32, 4, rgba_pattern(32, 4, 1))]
        code, pack, data = self.convert({"textures/s": otex_v1(packer.TT_PAL4, 16, 2, self.payload)}, entries)
        self.assertEqual(code, 0)
        variant_path = "textures/pack/textures/s.pal/" + self.variant
        self.assertEqual(sorted(pack), ["textures/pack/textures/s", variant_path, "workshop.json"])
        plain = pack["textures/pack/textures/s"]
        self.assertEqual(pack[variant_path], plain)  # the same OTEX bytes
        # Raw RGBA32 at 2x: hByteScale = 32 * 2 / 4 = 16, vPixelScale = 2, flags 1 (TEX_FLAG_LOAD_AS_RAW).
        self.assertEqual(struct.unpack_from("<IIII", plain, 64), (packer.TT_RGBA32, 32, 4, 1))
        self.assertEqual(struct.unpack_from("<ff", plain, 80), (16.0, 2.0))
        self.assertEqual(plain[92:], rgba_pattern(32, 4, 1))
        self.assertEqual([(m["path"], m["palette"], m["encoding"]) for m in data["matched"]],
                         [("textures/s", None, "raw"), ("textures/s", self.variant, "raw")])
        self.assertEqual(data["totals"]["matched_entries"], 1)  # one .htc entry, two rows
        self.assertEqual(data["totals"]["matched_paths"], 2)
        self.assertEqual(data["totals"]["palette_variant_paths"], 1)
        with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(packer.check_pack(self.path("out.o2r")), 0)

    def test_single_palette_with_index_zero_only_keeps_the_plain_override(self):
        zeros = bytes(16)  # CI4 16x2, every texel index 0: no palette CRC can tell colours apart
        crc = rice_for(zeros, 16, 2, 0)
        entries = [htc_entry(make_key(crc, self.palette), 32, 4, rgba_pattern(32, 4, 2))]
        code, pack, data = self.convert({"textures/z": otex_v1(packer.TT_PAL4, 16, 2, zeros)}, entries)
        self.assertEqual(code, 0)
        self.assertEqual(sorted(pack), ["textures/pack/textures/z", "workshop.json"])
        self.assertEqual([m["palette"] for m in data["matched"]], [None])
        self.assertEqual(data["totals"]["palette_variant_paths"], 0)
        self.assertEqual(data["by_reason"]["ci_palette_variants"], 0)

    def test_non_ci_texture_gets_no_variant(self):
        payload = bytes(range(16))  # RGBA16 4x2
        crc = rice_for(payload, 4, 2, 2)
        entries = [htc_entry(make_key(crc), 8, 4, rgba_pattern(8, 4, 1))]
        code, pack, data = self.convert({"textures/a": otex_v1(packer.TT_RGBA16, 4, 2, payload)}, entries)
        self.assertEqual(code, 0)
        self.assertEqual(sorted(pack), ["textures/pack/textures/a", "workshop.json"])
        self.assertEqual([m["palette"] for m in data["matched"]], [None])
        self.assertEqual(data["totals"]["palette_variant_paths"], 0)

    def test_the_pair_on_each_channel(self):
        crc = rice_for(self.payload, 16, 2, 0)
        cases = (
            ([], "textures/pack/textures/s", "textures/pack/textures/s.pal/" + self.variant),
            (["--alt"], "alt/textures/pack/textures/s", "alt/textures/pack/textures/s.pal/" + self.variant),
            (["--direct-keys"], "textures/s", "textures/pack/textures/s.pal/" + self.variant),
        )
        for i, (extra, plain_name, variant_name) in enumerate(cases):
            with self.subTest(channel=" ".join(extra) or "default"):
                entries = [htc_entry(make_key(crc, self.palette), 32, 4, rgba_pattern(32, 4, 1))]
                code, pack, _ = self.convert({"textures/s": otex_v1(packer.TT_PAL4, 16, 2, self.payload)},
                                             entries, extra, name="channel%d.o2r" % i)
                self.assertEqual(code, 0)
                self.assertEqual(sorted(n for n in pack if n != "workshop.json"),
                                 sorted([plain_name, variant_name]))
                self.assertEqual(pack[variant_name], pack[plain_name])


class SharedCrcTests(ScratchCase):
    def test_ci_and_i4_with_the_same_texels_convert_in_either_path_order(self):
        # A CI4 and an I4 texture with the same 4-bit texels share a Rice CRC. The .htc holds the CI4
        # under two palettes. Either path order must plan without error: the CI4 gets its two variants,
        # and the I4 (no palette) gets no match from this group.
        payload = bytes([0x01, 0x23, 0x45, 0x00] * 4)  # 16x2, highest index 5
        crc = rice_for(payload, 16, 2, 0)
        for ci_path, i4_path in (("textures/a", "textures/z"), ("textures/z", "textures/a")):
            with self.subTest(ci=ci_path):
                base = self.path(ci_path.split("/")[-1] + ".o2r")
                write_zip(base, {
                    ci_path: otex_v1(packer.TT_PAL4, 16, 2, payload),
                    i4_path: otex_v1(packer.TT_I4, 16, 2, payload),
                })
                htc_path = self.path(ci_path.split("/")[-1] + ".htc")
                with open(htc_path, "wb") as fh:
                    fh.write(htc_file([
                        htc_entry(make_key(crc, 1), 32, 4, rgba_pattern(32, 4, 1)),
                        htc_entry(make_key(crc, 2), 32, 4, rgba_pattern(32, 4, 2)),
                    ]))
                with open(htc_path, "rb") as fh:
                    _, entries = htc.parse_htc(fh.read())
                textures, _ = htc.read_base_textures([base])
                self.assertEqual(textures[i4_path]["crc"], textures[ci_path]["crc"])
                matches, skipped = htc.plan_conversion(entries, textures)
                self.assertEqual([(m["path"], m["palette"]) for m in matches], [(ci_path, 1), (ci_path, 2)])
                self.assertEqual(skipped, [])
                # The command line must also write the pack: the CI4 variants, and no error from the I4 record.
                out = self.path(ci_path.split("/")[-1] + "_pack.o2r")
                code, _, _ = run_quiet([htc_path, "--base", base, out, "--name", "X", "--author", "t"])
                self.assertEqual(code, 0)
                with zipfile.ZipFile(out, "r") as z:
                    names = sorted(n for n in z.namelist() if n != "workshop.json")
                self.assertEqual(names, ["textures/pack/%s.pal/05-00000001" % ci_path,
                                         "textures/pack/%s.pal/05-00000002" % ci_path])


class ScaleTests(unittest.TestCase):
    def test_pack_scale_rounds_half_up_and_never_goes_below_one(self):
        self.assertEqual(htc.pack_scale(8, 4, 4, 2), (2, 2))
        self.assertEqual(htc.pack_scale(16, 4, 16, 2), (1, 2))
        self.assertEqual(htc.pack_scale(3, 4, 2, 2), (2, 2))  # 1.5 rounds to 2
        self.assertEqual(htc.pack_scale(1, 2, 2, 2), (1, 1))  # 0.5 rounds to 1, not 0

    def test_pack_scale_keeps_one_texel_axes(self):
        self.assertEqual(htc.pack_scale(512, 8, 64, 1), (8, 1))  # the 64x1 skybox strips
        self.assertEqual(htc.pack_scale(8, 512, 1, 64), (1, 8))

    def describe(self, tex_type, native_w, native_h, pack_w, pack_h):
        return htc.describe_match({
            "base": {"type": tex_type, "width": native_w, "height": native_h},
            "entry": {"width": pack_w, "height": pack_h},
        })

    def test_scaled_pipeline_types_are_native_with_texel_scales(self):
        for tex_type in (packer.TT_RGBA16, packer.TT_IA8, packer.TT_IA4):
            with self.subTest(native=htc.NATIVE_INFO[tex_type][0]):
                m = self.describe(tex_type, 8, 224, 64, 1792)  # the timer digit strip at 8x
                self.assertEqual(m["encoding"], "native")
                self.assertEqual((m["h_scale"], m["v_scale"]), (8.0, 8.0))

    def test_other_types_stay_raw_with_byte_scales(self):
        for tex_type in (packer.TT_PAL4, packer.TT_PAL8, packer.TT_I4, packer.TT_RGBA32):
            with self.subTest(native=htc.NATIVE_INFO[tex_type][0]):
                m = self.describe(tex_type, 16, 16, 32, 32)
                self.assertEqual(m["encoding"], "raw")
                self.assertEqual((m["h_scale"], m["v_scale"]),
                                 htc.derive_scale(tex_type, 16, 16, 32, 32))

    def test_one_row_skybox_strip_is_raw_rgba32(self):
        m = self.describe(packer.TT_RGBA16, 64, 1, 512, 8)  # aSkyboxNightTex from a GLideN64 pack
        self.assertEqual(m["encoding"], "raw")
        self.assertEqual((m["pack_w"], m["pack_h"]), (512, 1))
        self.assertEqual((m["h_scale"], m["v_scale"]), (16.0, 1.0))
        self.assertEqual(htc.match_encoding(packer.TT_RGBA16, 512, 1), "raw")
        self.assertEqual(htc.match_encoding(packer.TT_RGBA16, 512, 2), "native")

    def test_ia4_with_half_byte_rows_stays_raw(self):
        self.assertEqual(htc.match_encoding(packer.TT_IA4, 3), "raw")
        self.assertEqual(htc.match_encoding(packer.TT_IA4, 4), "native")

    def test_derive_scale_per_native_type_for_a_2x_upscale(self):
        # Expected hByteScale is 32 * kw / bits. Native 16x16, pack 32x32 (kw = kh = 2).
        expected_h = {
            packer.TT_RGBA32: 2.0, packer.TT_RGBA16: 4.0, packer.TT_IA16: 4.0,
            packer.TT_PAL8: 8.0, packer.TT_I8: 8.0, packer.TT_IA8: 8.0,
            packer.TT_PAL4: 16.0, packer.TT_I4: 16.0, packer.TT_IA4: 16.0,
        }
        for tex_type, want_h in expected_h.items():
            name, bits, _siz = htc.NATIVE_INFO[tex_type]
            with self.subTest(native=name):
                h_scale, v_scale = htc.derive_scale(tex_type, 16, 16, 32, 32)
                self.assertEqual(h_scale, want_h)
                self.assertEqual(v_scale, 2.0)
                line = 16 * bits // 8
                self.assertEqual(line * h_scale, 4 * 32)  # resultNewLineSize == 4 * width
                self.assertEqual(16 * v_scale, 32)  # resultNewHeight == height

    def test_derive_scale_asymmetric_and_non_byte_rows(self):
        self.assertEqual(htc.derive_scale(packer.TT_RGBA16, 16, 8, 32, 24), (4.0, 3.0))
        with self.assertRaises(ValueError):
            htc.derive_scale(packer.TT_PAL4, 3, 2, 6, 4)  # 12 bits per row


class ResampleTests(unittest.TestCase):
    def test_three_to_two_wide_by_hand(self):
        # Red values 30, 30, 210 (alpha 255). Destination 0 covers source 0 fully and half of
        # source 1: (30 * 2 + 30 * 1) / 3 = 30. Destination 1 covers half of source 1 and source 2:
        # (30 * 1 + 210 * 2) / 3 = 150.
        src = bytes([30, 30, 30, 255, 30, 30, 30, 255, 210, 210, 210, 255])
        self.assertEqual(htc.resample_rgba(src, 3, 1, 2, 1),
                         bytes([30, 30, 30, 255, 150, 150, 150, 255]))

    def test_two_by_two_to_one_rounds_half_up(self):
        # Red 0, 100, 200, 255 average 138.75, which rounds to 139.
        src = bytes([0, 0, 0, 255, 100, 0, 0, 255, 200, 0, 0, 255, 255, 0, 0, 255])
        self.assertEqual(htc.resample_rgba(src, 2, 2, 1, 1), bytes([139, 0, 0, 255]))

    def test_same_size_is_identity(self):
        src = bytes(range(16))
        self.assertEqual(htc.resample_rgba(src, 2, 2, 2, 2), src)


class PackTests(ScratchCase):
    def build_inputs(self):
        a_payload = bytes(range(16))  # RGBA16 4x2
        b_payload = bytes(range(32, 48))  # IA8 8x2
        c_payload = bytes(range(64, 80))  # CI4 16x2, single palette (CRC 0), highest index 15
        base = self.path("fzerox.o2r")
        write_zip(base, {
            "textures/a": otex_v1(packer.TT_RGBA16, 4, 2, a_payload),
            "textures/b": otex_v1(packer.TT_IA8, 8, 2, b_payload),
            "textures/c": otex_v1(packer.TT_PAL4, 16, 2, c_payload),
        })
        self.rgba_a = rgba_pattern(8, 4, 1)
        entries = [
            htc_entry(make_key(rice_for(a_payload, 4, 2, 2)), 8, 4, self.rgba_a),
            htc_entry(make_key(rice_for(b_payload, 8, 2, 1)), 16, 4, rgba_pattern(16, 4, 2)),
            htc_entry(make_key(rice_for(c_payload, 16, 2, 0)), 16, 4, rgba_pattern(16, 4, 3)),
        ]
        htc_path = self.path("pack.htc")
        with open(htc_path, "wb") as fh:
            fh.write(htc_file(entries))
        return htc_path, base

    def read_zip(self, path):
        with zipfile.ZipFile(path, "r") as z:
            return {name: z.read(name) for name in z.namelist()}

    def test_end_to_end_pack_has_native_and_raw_otex_and_passes_the_packer_check(self):
        htc_path, base = self.build_inputs()
        out = self.path("out.o2r")
        report = self.path("report.json")
        code, _, _ = run_quiet([htc_path, "--base", base, out, "--name", "Test Pack",
                                "--author", "tester", "--json", report])
        self.assertEqual(code, 0)
        members = self.read_zip(out)
        self.assertEqual(sorted(members), ["textures/pack/textures/a", "textures/pack/textures/b",
                                           "textures/pack/textures/c", "textures/pack/textures/c.pal/0F-00000000",
                                           "workshop.json"])
        blob = members["textures/pack/textures/a"]
        self.assertEqual(struct.unpack_from("<I", blob, 4)[0], packer.OTR_TYPE_TEXTURE)
        self.assertEqual(struct.unpack_from("<I", blob, 8)[0], 1)
        # RGBA16 4x2 native with an 8x4 cache entry: re-encoded as RGBA16 8x4, flags 0, kw = kh = 2.
        self.assertEqual(struct.unpack_from("<IIII", blob, 64), (packer.TT_RGBA16, 8, 4, 0))
        self.assertEqual(struct.unpack_from("<ff", blob, 80), (2.0, 2.0))
        self.assertEqual(struct.unpack_from("<I", blob, 88)[0], 8 * 4 * 2)
        want_a, _ = packer.enc_rgba16(htc.RgbaPixels(self.rgba_a, 8), 8, 4)
        self.assertEqual(blob[92:], want_a)
        # IA8 8x2 native with a 16x4 cache entry: IA8 16x4, flags 0, kw = kh = 2.
        blob_b = members["textures/pack/textures/b"]
        self.assertEqual(struct.unpack_from("<IIII", blob_b, 64), (packer.TT_IA8, 16, 4, 0))
        self.assertEqual(struct.unpack_from("<ff", blob_b, 80), (2.0, 2.0))
        # CI4 16x2 native with a 16x4 cache entry stays raw RGBA32: kw = 1, kh = 2,
        # hByteScale = 32 / 4 = 8.
        blob_c = members["textures/pack/textures/c"]
        self.assertEqual(struct.unpack_from("<IIII", blob_c, 64), (packer.TT_RGBA32, 16, 4, 1))
        self.assertEqual(struct.unpack_from("<ff", blob_c, 80), (8.0, 2.0))
        # The one palette variant of textures/c is the same image as the plain override.
        self.assertEqual(members["textures/pack/textures/c.pal/0F-00000000"], blob_c)
        meta = json.loads(members["workshop.json"])
        self.assertEqual(meta["key_channel"], "pack")
        self.assertEqual(meta["name"], "Test Pack")
        self.assertEqual(meta["id"], "out")
        self.assertEqual(sorted(f["path"] for f in meta["files"]),
                         ["textures/pack/textures/a", "textures/pack/textures/b", "textures/pack/textures/c",
                          "textures/pack/textures/c.pal/0F-00000000"])
        for f in meta["files"]:
            self.assertEqual(f["sha256"], hashlib.sha256(members[f["path"]]).hexdigest())
        check_out, check_err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(check_out), contextlib.redirect_stderr(check_err):
            self.assertEqual(packer.check_pack(out), 0)
        self.assertIn("OK", check_out.getvalue())
        with open(report, "r", encoding="utf-8") as fh:
            data = json.load(fh)
        self.assertEqual(data["totals"]["matched_paths"], 4)
        self.assertEqual(data["totals"]["matched_entries"], 3)  # textures/c's two rows share entry 2
        self.assertEqual(data["totals"]["palette_variant_paths"], 1)
        self.assertEqual((data["totals"]["native_paths"], data["totals"]["raw_paths"]), (2, 2))
        self.assertEqual([m["encoding"] for m in data["matched"]], ["native", "native", "raw", "raw"])
        self.assertEqual([m["palette"] for m in data["matched"]], [None, None, None, "0F-00000000"])
        self.assertEqual([m["htc_index"] for m in data["matched"]], [0, 1, 2, 2])
        self.assertEqual(data["by_reason"]["no_match"], 0)

    def test_resampled_entry_is_marked_and_keeps_a_uniform_colour(self):
        payload = bytes(range(16))
        base = self.path("fzerox.o2r")
        write_zip(base, {"textures/a": otex_v1(packer.TT_RGBA16, 4, 2, payload)})
        uniform = bytes([77, 77, 77, 200]) * (6 * 3)  # 6x3 hi-res, ratio 1.5 on both axes
        htc_path = self.path("pack.htc")
        with open(htc_path, "wb") as fh:
            fh.write(htc_file([htc_entry(make_key(rice_for(payload, 4, 2, 2)), 6, 3, uniform)]))
        out = self.path("out.o2r")
        report = self.path("report.json")
        code, _, _ = run_quiet([htc_path, "--base", base, out, "--name", "R", "--author", "t",
                                "--json", report])
        self.assertEqual(code, 0)
        with open(report, "r", encoding="utf-8") as fh:
            matched = json.load(fh)["matched"][0]
        self.assertTrue(matched["resampled"])
        self.assertEqual((matched["pack_width"], matched["pack_height"]), (8, 4))
        blob = self.read_zip(out)["textures/pack/textures/a"]
        self.assertEqual(struct.unpack_from("<IIII", blob, 64)[1:3], (8, 4))
        # (77, 77, 77, 200) in RGBA5551: 9 << 11 | 9 << 6 | 9 << 1 | 1 = 0x4A53.
        self.assertEqual(blob[92:], bytes([0x4A, 0x53]) * (8 * 4))

    def test_alt_channel_moves_entries_and_labels_metadata(self):
        htc_path, base = self.build_inputs()
        out = self.path("alt.o2r")
        code, _, _ = run_quiet([htc_path, "--base", base, out, "--name", "A", "--author", "t", "--alt"])
        self.assertEqual(code, 0)
        members = self.read_zip(out)
        self.assertEqual(sorted(n for n in members if n != "workshop.json"),
                         ["alt/textures/pack/textures/a", "alt/textures/pack/textures/b",
                          "alt/textures/pack/textures/c", "alt/textures/pack/textures/c.pal/0F-00000000"])
        self.assertEqual(json.loads(members["workshop.json"])["key_channel"], "alt")

    def test_direct_keys_shadow_the_base_paths(self):
        htc_path, base = self.build_inputs()
        out = self.path("direct.o2r")
        code, _, _ = run_quiet([htc_path, "--base", base, out, "--name", "D", "--author", "t", "--direct-keys"])
        self.assertEqual(code, 0)
        members = self.read_zip(out)
        # The bare paths hold the plain entries; the palette variant keeps the textures/pack/ prefix.
        self.assertEqual(sorted(n for n in members if n != "workshop.json"),
                         ["textures/a", "textures/b", "textures/c", "textures/pack/textures/c.pal/0F-00000000"])
        self.assertEqual(members["textures/pack/textures/c.pal/0F-00000000"], members["textures/c"])
        self.assertEqual(json.loads(members["workshop.json"])["key_channel"], "direct")

    def test_alt_and_direct_keys_are_exclusive(self):
        htc_path, base = self.build_inputs()
        with self.assertRaises(SystemExit):
            run_quiet([htc_path, "--base", base, self.path("x.o2r"), "--name", "X", "--author", "t",
                       "--alt", "--direct-keys"])
        self.assertFalse(os.path.exists(self.path("x.o2r")))

    def test_two_runs_are_byte_identical(self):
        htc_path, base = self.build_inputs()
        # Same output file name in two folders: the metadata id comes from the file name.
        first = os.path.join(self.dir, "run1", "pack.o2r")
        second = os.path.join(self.dir, "run2", "pack.o2r")
        for target in (first, second):
            code, _, _ = run_quiet([htc_path, "--base", base, target, "--name", "D", "--author", "t"])
            self.assertEqual(code, 0)
        with open(first, "rb") as fh:
            one = fh.read()
        with open(second, "rb") as fh:
            two = fh.read()
        self.assertEqual(one, two)

    def test_check_writes_no_pack(self):
        htc_path, base = self.build_inputs()
        before = sorted(os.listdir(self.dir))
        code, stdout, _ = run_quiet([htc_path, "--base", base, "--name", "C", "--author", "t", "--check"])
        self.assertEqual(code, 0)
        self.assertIn("check: nothing written", stdout)
        self.assertEqual(sorted(os.listdir(self.dir)), before)

    def test_nothing_to_pack_writes_nothing_and_exits_2(self):
        base = self.path("fzerox.o2r")
        write_zip(base, {"textures/a": otex_v1(packer.TT_RGBA16, 4, 2, bytes(16))})
        htc_path = self.path("pack.htc")
        with open(htc_path, "wb") as fh:
            fh.write(htc_file([htc_entry(make_key(0xDEADBEEF), 1, 1, bytes(4))]))
        out = self.path("empty.o2r")
        code, _, stderr = run_quiet([htc_path, "--base", base, out, "--name", "E", "--author", "t"])
        self.assertEqual(code, 2)
        self.assertIn("nothing to pack", stderr)
        self.assertFalse(os.path.exists(out))

    def test_missing_htc_exits_2(self):
        base = self.path("fzerox.o2r")
        write_zip(base, {"textures/a": otex_v1(packer.TT_RGBA16, 4, 2, bytes(16))})
        code, _, stderr = run_quiet([self.path("missing.htc"), "--base", base,
                                     self.path("x.o2r"), "--name", "M", "--author", "t"])
        self.assertEqual(code, 2)
        self.assertIn(".htc file not found", stderr)


class BuildOtexTests(unittest.TestCase):
    def test_default_flags_keep_the_old_bytes(self):
        data = bytes(range(16))
        header = bytearray(64)
        header[1] = 1
        struct.pack_into("<I", header, 4, packer.OTR_TYPE_TEXTURE)
        struct.pack_into("<I", header, 8, 1)
        expected = bytes(header) + struct.pack("<IIIIffI", 1, 2, 2, 0, 1.0, 1.0, 16) + data
        self.assertEqual(packer.build_otex_v1(1, 2, 2, 1.0, 1.0, data), expected)
        self.assertEqual(packer.build_otex_v1(1, 2, 2, 1.0, 1.0, data, flags=0), expected)

    def test_flags_one_changes_only_the_flags_word(self):
        data = bytes(16)
        plain = packer.build_otex_v1(1, 2, 2, 1.0, 1.0, data)
        raw = packer.build_otex_v1(1, 2, 2, 1.0, 1.0, data, flags=1)
        self.assertEqual(raw[76:80], b"\x01\x00\x00\x00")
        self.assertEqual(raw[:76], plain[:76])
        self.assertEqual(raw[80:], plain[80:])


if __name__ == "__main__":
    unittest.main()
