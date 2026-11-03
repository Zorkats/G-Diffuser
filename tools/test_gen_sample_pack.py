import contextlib
import importlib.util
import io
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
import wave


SCRIPT = Path(__file__).with_name("gen_sample_pack.py")
SPEC = importlib.util.spec_from_file_location("sample_pack", SCRIPT)
pack = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(pack)
KEY = "SAMPLE_DDBGM_SELECT__0x0__249084"


class SamplePackTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.audio = self.root / "audio"
        (self.audio / "fonts").mkdir(parents=True)
        (self.audio / "fonts" / "select.json").write_text(json.dumps({
            "samples": {KEY: {"playbackRateHz": 22050}}
        }), encoding="utf-8")
        self.source = self.root / "samples"
        self.source.mkdir()

    def wav(self, key):
        with wave.open(str(self.source / (key + ".wav")), "wb") as out:
            out.setparams((1, 2, 22050, 0, "NONE", "not compressed"))
            out.writeframes(b"\x00\x10" * 32)

    def run_cli(self, *args):
        return subprocess.run([sys.executable, str(SCRIPT), *map(str, args)],
                              capture_output=True, text=True)

    def test_malformed_key_is_not_renamed(self):
        malformed = "SAMPLE_DDBGM_SELECT_0x0_249084"
        self.wav(malformed)
        sidecar = self.source / (malformed + ".loop.json")
        sidecar.write_text('{"start": 0, "end": 32}', encoding="utf-8")
        with contextlib.redirect_stdout(io.StringIO()):
            entries, errors, warnings, _ = pack.plan_pack(
                str(self.source), {KEY: 22050}, {KEY}, 2)
        self.assertFalse(entries)
        self.assertFalse(errors)
        self.assertIn("Did you mean " + KEY, "\n".join(warnings))
        self.assertTrue((self.source / (malformed + ".wav")).exists())
        result = self.run_cli(self.source, self.root / "bad.o2r", "--dump-dir", self.audio)
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
        self.assertIn("0 accepted, 1 skipped, 0 failed", result.stdout)
        self.assertIn("Did you mean " + KEY, result.stdout)
        self.assertFalse((self.root / "bad.o2r").exists())
        self.assertTrue(sidecar.exists())
        self.assertFalse((self.source / (KEY + ".loop.json")).exists())
        result = self.run_cli(self.source, "--check", "--dump-dir", self.audio)
        self.assertEqual(result.returncode, 2)


    def test_unknown_key_has_no_invented_suggestion(self):
        self.wav("SAMPLE_UNKNOWN_0x0_123")
        result = self.run_cli(self.source, self.root / "bad.o2r", "--dump-dir", self.audio)
        self.assertNotEqual(result.returncode, 0)
        self.assertNotIn("Did you mean", result.stdout)
        self.assertFalse((self.root / "bad.o2r").exists())

    def test_explicit_catalog_and_deterministic_archive(self):
        self.wav(KEY)
        first, second = self.root / "one.o2r", self.root / "two.o2r"
        for output in (first, second):
            result = self.run_cli(self.source, output, "--dump-dir", self.audio,
                                  "--name", "Test", "--author", "Test", "--id", "test")
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("1 accepted, 0 skipped, 0 failed", result.stdout)
        self.assertEqual(first.read_bytes(), second.read_bytes())
        result = self.run_cli(first, "--check", "--dump-dir", self.audio)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_missing_catalog_or_rate_is_fatal(self):
        self.wav(KEY)
        result = self.run_cli(self.source, "--check", "--dump-dir", self.root / "missing")
        self.assertNotEqual(result.returncode, 0)
        (self.audio / "fonts" / "select.json").write_text(json.dumps({"samples": {KEY: {}}}))
        result = self.run_cli(self.source, "--check", "--dump-dir", self.audio)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("playback rate", result.stderr)

    def test_target_catalog_matches_runtime_keys(self):
        result = self.run_cli("--targets")
        self.assertEqual(result.returncode, 0, result.stderr)
        catalog = json.loads(result.stdout)
        self.assertEqual(catalog["schema_version"], 1)
        runtime = (SCRIPT.parent.parent / "port" / "gdx_music_files.h").read_text(encoding="utf-8")
        table = runtime.split("kBgmNames[] = {", 1)[1].split("};", 1)[0]
        import re
        keys = re.findall(r'"(bgm_[^"]+)"', table)
        for target in catalog["targets"]:
            self.assertEqual(target["stream_key"], keys[target["bgm_id"]])
            for resources in target["native"].values():
                for resource in resources:
                    self.assertRegex(resource["key"], pack.KEY_RE)
                    expected_rate = 32000 if target["id"] == "big_blue" else 22050
                    self.assertEqual(resource["rate_hz"], expected_rate)
                    self.assertIn(resource["channel"], ("mono", "part_0", "part_1"))
        big_blue = next(t for t in catalog["targets"] if t["id"] == "big_blue")
        self.assertEqual(len(big_blue["native"]["expansion_kit"]), 2)
        self.assertNotIn("cartridge", big_blue["native"])
        self.assertIn("No automatic stereo split", big_blue["native_input_policy"])


if __name__ == "__main__":
    unittest.main()
