import hashlib
import json
from pathlib import Path
import struct
import tempfile
import unittest
import zipfile

from repair_hdcube_stats import ASSETS, MATTE, repair_archive, repair_texture, rgba16


def texture(width, height, pixels, version=0):
    offset = 80 if version == 0 else 92
    header = bytearray(offset)
    header[4:8] = b"XETO"
    struct.pack_into("<I", header, 8, version)
    struct.pack_into("<III", header, 64, 2, width, height)
    if version == 1:
        struct.pack_into("<Iff", header, 76, 0, 4, 4)
    struct.pack_into("<I", header, offset - 4, width * height * 2)
    return bytes(header) + struct.pack(f">{len(pixels)}H", *pixels)


class StatsRepairTests(unittest.TestCase):
    def test_only_dark_native_matte_changes(self):
        native = texture(2, 1, [MATTE, 1])
        pixels = [1] * 32
        pixels[0] = 0xFFFF
        pixels[1] = 0x2109
        hd = texture(8, 4, pixels, 1)
        fixed, count = repair_texture(native, hd, (2, 1))
        _, result = rgba16(fixed, (8, 4))
        self.assertEqual(count, 14)
        self.assertEqual(fixed[:92], hd[:92])
        for i, value in enumerate(result):
            expected = MATTE if i % 8 < 4 and i not in (0, 1) else pixels[i]
            self.assertEqual(value, expected)
            self.assertEqual(value & 1, 1)
        self.assertEqual(repair_texture(native, fixed, (2, 1)), (fixed, 0))

    def test_invalid_resources_are_rejected(self):
        native = texture(1, 1, [MATTE])
        hd = texture(4, 4, [1] * 16, 1)
        invalid = [hd[:-1], hd + b"x", b"bad", texture(4, 4, [0] * 16, 1)]
        for offset, value in ((4, b"FAIL"), (64, struct.pack("<I", 3)),
                              (80, struct.pack("<f", 2))):
            data = bytearray(hd)
            data[offset:offset + 4] = value
            invalid.append(bytes(data))
        for data in invalid:
            with self.subTest(data=data[:12]), self.assertRaises(ValueError):
                repair_texture(native, data, (1, 1))

    def test_archive_preserves_inputs_and_unrelated_entries(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            native_path, pack_path = root / "native.o2r", root / "pack.o2r"
            manifest = {"name": "Fixture", "files": []}
            with zipfile.ZipFile(native_path, "w") as native, zipfile.ZipFile(pack_path, "w") as pack:
                for name, (width, height) in ASSETS.items():
                    key = "common_assets_compressed/" + name
                    path = "textures/pack/" + key
                    native.writestr(key, texture(width, height, [MATTE] * (width * height)))
                    data = texture(width * 4, height * 4, [1] * (width * height * 16), 1)
                    pack.writestr(path, data)
                    manifest["files"].append({"path": path, "sha256": hashlib.sha256(data).hexdigest()})
                pack.writestr("unrelated", b"preserve exactly")
                pack.writestr("workshop.json", json.dumps(manifest))
            originals = [path.read_bytes() for path in (native_path, pack_path)]
            self.assertNotIn("output_sha256", repair_archive(native_path, pack_path))
            output, repeated = root / "fixed.o2r", root / "repeated.o2r"
            report = repair_archive(native_path, pack_path, output)
            repair_archive(native_path, pack_path, repeated)
            self.assertEqual(output.read_bytes(), repeated.read_bytes())
            self.assertEqual(len(report["assets"]), 6)
            self.assertEqual(originals, [path.read_bytes() for path in (native_path, pack_path)])
            with zipfile.ZipFile(output) as fixed:
                self.assertEqual(fixed.read("unrelated"), b"preserve exactly")
                metadata = json.loads(fixed.read("workshop.json"))
                self.assertEqual(metadata["name"], "Fixture")
                for entry in metadata["files"]:
                    self.assertEqual(entry["sha256"], hashlib.sha256(fixed.read(entry["path"])).hexdigest())
            for protected in (output, native_path, pack_path):
                with self.assertRaises(ValueError):
                    repair_archive(native_path, pack_path, protected)
            again = repair_archive(native_path, output)
            self.assertTrue(all(entry["changed_pixels"] == 0 for entry in again["assets"]))


if __name__ == "__main__":
    unittest.main()
