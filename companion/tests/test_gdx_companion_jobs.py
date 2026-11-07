# Copyright (c) Zorkats
"""Tests for Companion C1 jobs: the worker builds the same packs as the command line, and a build
never loses the last good pack.

Every input is made up here, in a fresh temporary folder. Nothing reads a ROM, a dump or a game
install.

Run with: python -I -m unittest discover -s companion/tests -v
"""

import contextlib
import io
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
import wave
import zipfile
import zlib
from unittest import mock

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.normpath(os.path.join(HERE, "..", ".."))
TOOLS = os.path.join(ROOT, "tools")
WORKER_DIR = os.path.join(ROOT, "companion", "worker")
WORKER = os.path.join(WORKER_DIR, "gdx_companion_worker.py")
for _folder in (WORKER_DIR, TOOLS):
    if _folder not in sys.path:
        sys.path.insert(0, _folder)

import gdx_companion_jobs as jobs  # noqa: E402

# Texture tests need Pillow in the interpreter that runs them. The probe runs in a child process,
# because test_gen_htc_pack installs a stand-in PIL module into this one.
HAVE_PILLOW = subprocess.run([sys.executable, "-I", "-c", "import PIL.Image"], stdout=subprocess.DEVNULL,
                             stderr=subprocess.DEVNULL, timeout=120).returncode == 0
PILLOW_SKIP = "Pillow is not importable by " + sys.executable + "; the texture packer tests need it"
import test_gen_htc_pack as htc_fixtures  # noqa: E402

SAMPLE_KEY = "SAMPLE_DDBGM_SELECT__0x0__249084"


def run_cli(script, args):
    result = subprocess.run([sys.executable, "-I", os.path.join(TOOLS, script)] + [str(a) for a in args],
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=120)
    return result.returncode, result.stdout.decode("utf-8", "replace") + result.stderr.decode("utf-8", "replace")


def run_worker(requests, timeout=120):
    """Sends the requests, then shutdown. Returns (exit code, events)."""
    payload = b"".join(json.dumps(dict({"v": 1}, **r)).encode("utf-8") + b"\n" for r in requests)
    payload += b'{"v":1,"op":"shutdown"}\n'
    result = subprocess.run([sys.executable, "-I", WORKER], input=payload, stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE, timeout=timeout)
    events = [json.loads(line) for line in result.stdout.decode("ascii").splitlines() if line]
    return result.returncode, events


def finals(events):
    return [e for e in events if e["event"] in ("result", "job_error")]


def members(path):
    with zipfile.ZipFile(path) as archive:
        return {name: archive.read(name) for name in archive.namelist()}


def png_chunk(kind, data):
    return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))


def png_bytes(width, height):
    """A solid 8-bit RGBA PNG, written by hand so the test does not need Pillow to make its input."""
    header = struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)
    rows = b"".join(b"\x00" + b"\x40\x80\xc0\xff" * width for _ in range(height))
    return (b"\x89PNG\r\n\x1a\n" + png_chunk(b"IHDR", header) + png_chunk(b"IDAT", zlib.compress(rows))
            + png_chunk(b"IEND", b""))


class Scratch(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = self.temp.name

    def path(self, *parts):
        return os.path.join(self.root, *parts)

    def folder(self, *parts):
        path = self.path(*parts)
        os.makedirs(path, exist_ok=True)
        return path

    # Inputs for each packer.
    def sequence_input(self, names=("ddbgm_mute_city",)):
        source = self.folder("seq")
        for i, name in enumerate(names):
            with open(os.path.join(source, name + ".seq"), "wb") as fh:
                fh.write(bytes([0xD3, 0x80, 0xD7, 0xFF, 0xFF, i]) + bytes(26))
        return source

    def sample_input(self):
        catalog = self.folder("audio")
        os.makedirs(os.path.join(catalog, "fonts"), exist_ok=True)
        with open(os.path.join(catalog, "fonts", "select.json"), "w", encoding="utf-8") as fh:
            json.dump({"samples": {SAMPLE_KEY: {"playbackRateHz": 22050}}}, fh)
        source = self.folder("samples")
        with wave.open(os.path.join(source, SAMPLE_KEY + ".wav"), "wb") as out:
            out.setparams((1, 2, 22050, 0, "NONE", "not compressed"))
            out.writeframes(b"\x00\x10\x00\x20" * 64)
        return source, catalog

    def htc_input(self):
        fx = htc_fixtures
        a_payload = bytes(range(16))
        base = self.path("fzerox.o2r")
        fx.write_zip(base, {"textures/a": fx.otex_v1(fx.packer.TT_RGBA16, 4, 2, a_payload)})
        entry = fx.htc_entry(fx.make_key(fx.rice_for(a_payload, 4, 2, 2)), 8, 4, fx.rgba_pattern(8, 4, 1))
        cache = self.path("pack.htc")
        with open(cache, "wb") as fh:
            fh.write(fx.htc_file([entry]))
        return cache, base

    def build(self, packer, source, out_name, options=None, catalog=None, overwrite=False, job="j1",
              staging=None, out_folder="worker"):
        out = os.path.join(self.folder(out_folder), out_name)
        request = {"op": "build", "job": job, "packer": packer, "input": source, "output": out,
                   "staging": staging or os.path.join(os.path.dirname(out), jobs.STAGING_PREFIX + job),
                   "overwrite": overwrite, "options": options or {}}
        if catalog is not None:
            request["catalog"] = catalog
        return out, request

    def texture_input(self, name="textures", size=(4, 4)):
        """A dump folder with one 2x2 RGBA16 texture; its replacement PNG is size (4x4 by default)."""
        source = self.folder(name)
        with open(os.path.join(source, "manifest.tsv"), "wb") as fh:
            fh.write(b"smoke/tile\t2\t2\tRGBA16\n")
        os.makedirs(os.path.join(source, "smoke"), exist_ok=True)
        with open(os.path.join(source, "smoke", "tile.png"), "wb") as fh:
            fh.write(png_bytes(*size))
        return source


class SameAsCommandLine(Scratch):
    """C1 gate: the worker and the command line make the same pack from the same inputs."""

    def assert_same(self, script, cli_args, packer, source, options, catalog=None):
        cli_out = os.path.join(self.folder("cli"), "pack.o2r")
        code, text = run_cli(script, cli_args(cli_out))
        self.assertEqual(code, 0, text)
        out, request = self.build(packer, source, "pack.o2r", options, catalog)
        code, events = run_worker([request])
        self.assertEqual(code, 0)
        final = finals(events)
        self.assertEqual(len(final), 1, events)
        self.assertEqual(final[0]["event"], "result", final[0])
        self.assertEqual(members(out), members(cli_out))
        with open(out, "rb") as a, open(cli_out, "rb") as b:
            self.assertEqual(a.read(), b.read())  # both writers are deterministic
        artifact = final[0]["artifacts"][0]
        self.assertEqual(artifact["path"], out)
        self.assertEqual(artifact["bytes"], os.path.getsize(out))
        self.assertFalse(final[0]["replaced"])
        self.assertFalse(os.path.exists(request["staging"]))
        logs = [e for e in events if e["event"] == "log"]
        self.assertTrue(logs, "packer output should reach the log")
        self.assertTrue(all(e["job"] == "j1" for e in logs))

    def test_sequence(self):
        source = self.sequence_input()
        self.assert_same("gen_sequence_pack.py",
                         lambda out: [source, out, "--name", "My Music", "--author", "me"],
                         "sequence", source, {"name": "My Music", "author": "me"})

    def test_sample(self):
        source, catalog = self.sample_input()
        self.assert_same("gen_sample_pack.py",
                         lambda out: [source, out, "--dump-dir", catalog, "--name", "Test", "--id", "test"],
                         "sample", source, {"name": "Test", "id": "test"}, catalog)

    def test_htc(self):
        cache, base = self.htc_input()
        self.assert_same("gen_htc_pack.py",
                         lambda out: [cache, "--base", base, out, "--name", "HD", "--author", "me"],
                         "htc", cache, {"name": "HD", "author": "me", "base": [base]})

    def test_htc_alt_channel(self):
        cache, base = self.htc_input()
        self.assert_same("gen_htc_pack.py",
                         lambda out: [cache, "--base", base, out, "--name", "HD", "--author", "me", "--alt"],
                         "htc", cache, {"name": "HD", "author": "me", "base": [base], "alt": True})

    @unittest.skipUnless(HAVE_PILLOW, PILLOW_SKIP)
    def test_texture(self):
        source = self.texture_input()
        self.assert_same("gen_texture_pack.py",
                         lambda out: [source, out, "--name", "Tex", "--author", "me"],
                         "texture", source, {"name": "Tex", "author": "me"})

    @unittest.skipUnless(HAVE_PILLOW, PILLOW_SKIP)
    def test_texture_every_option(self):
        source = self.texture_input()
        manifest = os.path.join(source, "manifest.tsv")
        options = {"name": "Tex", "author": "me", "version": "1.2", "id": "me.tex",
                   "game_version": "us.rev0", "key_scheme_version": "2", "native_manifest": manifest,
                   "direct_keys": True}
        self.assert_same("gen_texture_pack.py",
                         lambda out: [source, out, "--name", "Tex", "--author", "me", "--version", "1.2",
                                      "--id", "me.tex", "--game-version", "us.rev0",
                                      "--key-scheme-version", "2", "--native-manifest", manifest,
                                      "--direct-keys"],
                         "texture", source, options)

    def test_check_matches_command_line_status(self):
        good = self.sequence_input()
        bad = self.folder("bad")
        with open(os.path.join(bad, "not_a_song.seq"), "wb") as fh:
            fh.write(b"\x00" * 8)
        for source in (good, bad):
            code, _ = run_cli("gen_sequence_pack.py", [source, "--check"])
            _, events = run_worker([{"op": "check", "job": "c1", "packer": "sequence", "input": source}])
            final = finals(events)[0]
            self.assertEqual(final["event"], "result")
            self.assertEqual(final["exit"], code)
            self.assertEqual(final["status"], "ok" if code == 0 else "problems")


@unittest.skipUnless(HAVE_PILLOW, PILLOW_SKIP)
class Texture(Scratch):
    """The texture packer through the worker: check, plan and a built pack, matching the command line."""

    def test_check_matches_command_line_status(self):
        good = self.texture_input("good")
        bad = self.texture_input("bad", size=(3, 3))  # 3x3 is not a whole multiple of the 2x2 native size
        for source, expected in ((good, 0), (bad, 1)):
            code, text = run_cli("gen_texture_pack.py", [source, "--check"])
            self.assertEqual(code, expected, text)
            _, events = run_worker([{"op": "check", "job": "c1", "packer": "texture", "input": source}])
            final = finals(events)[0]
            self.assertEqual(final["event"], "result", final)
            self.assertEqual(final["exit"], code)
            self.assertEqual(final["status"], "ok" if code == 0 else "problems")

    def test_check_of_a_built_pack_matches_the_command_line(self):
        source = self.texture_input()
        out, request = self.build("texture", source, "pack.o2r", {"name": "Tex", "author": "me"})
        _, events = run_worker([request])
        self.assertEqual(finals(events)[0]["event"], "result")
        code, text = run_cli("gen_texture_pack.py", [out, "--check"])
        self.assertEqual(code, 0, text)
        _, events = run_worker([{"op": "check", "job": "c2", "packer": "texture", "input": out}])
        final = finals(events)[0]
        self.assertEqual((final["event"], final["exit"], final["status"]), ("result", 0, "ok"), final)

    def test_plan_reports_the_destination_without_writing(self):
        source = self.texture_input()
        out = self.path("worker", "textures.o2r")
        self.folder("worker")
        plan = {"op": "plan", "job": "p1", "packer": "texture", "input": source, "output": out}
        _, events = run_worker([plan])
        final = finals(events)[0]
        self.assertEqual(final["event"], "result", final)
        self.assertFalse(final["exists"])
        self.assertFalse(final["needs_overwrite"])
        self.assertIsNone(final["previous"])
        self.assertEqual(os.listdir(self.path("worker")), [])


class Publication(Scratch):
    def first_pack(self):
        source = self.sequence_input()
        out, request = self.build("sequence", source, "music.o2r", {"name": "One"})
        _, events = run_worker([request])
        self.assertEqual(finals(events)[0]["event"], "result")
        with open(out, "rb") as fh:
            return source, out, fh.read()

    def test_existing_output_needs_overwrite(self):
        source, out, first = self.first_pack()
        _, request = self.build("sequence", source, "music.o2r", {"name": "Two"}, job="j2")
        _, events = run_worker([request])
        final = finals(events)[0]
        self.assertEqual((final["event"], final["code"]), ("job_error", "exists"))
        with open(out, "rb") as fh:
            self.assertEqual(fh.read(), first)
        self.assertFalse(os.path.exists(request["staging"]))

    def test_overwrite_keeps_the_previous_pack(self):
        source, out, first = self.first_pack()
        _, request = self.build("sequence", source, "music.o2r", {"name": "Two"}, overwrite=True, job="j2")
        _, events = run_worker([request])
        final = finals(events)[0]
        self.assertEqual(final["event"], "result", final)
        self.assertTrue(final["replaced"])
        self.assertEqual(final["previous"], out + ".previous")
        with open(out + ".previous", "rb") as fh:
            self.assertEqual(fh.read(), first)
        self.assertEqual(json.loads(members(out)["workshop.json"])["name"], "Two")
        self.assertFalse(os.path.exists(request["staging"]))

    def test_failed_build_leaves_the_last_good_pack(self):
        _, out, first = self.first_pack()
        empty = self.folder("empty")
        with open(os.path.join(empty, "not_a_song.seq"), "wb") as fh:
            fh.write(b"\x00" * 8)
        _, request = self.build("sequence", empty, "music.o2r", {"name": "Bad"}, overwrite=True, job="j3")
        _, events = run_worker([request])
        final = finals(events)[0]
        self.assertEqual(final["event"], "job_error")
        self.assertIn(final["code"], ("packer_failed", "empty_output"))
        with open(out, "rb") as fh:
            self.assertEqual(fh.read(), first)
        self.assertFalse(os.path.exists(out + ".previous"))
        self.assertFalse(os.path.exists(request["staging"]))

    def test_plan_reports_the_destination_without_writing(self):
        source, out, _ = self.first_pack()
        before = sorted(os.listdir(os.path.dirname(out)))
        _, events = run_worker([{"op": "plan", "job": "p1", "packer": "sequence", "input": source, "output": out}])
        final = finals(events)[0]
        self.assertEqual(final["event"], "result")
        self.assertTrue(final["exists"])
        self.assertTrue(final["needs_overwrite"])
        self.assertEqual(final["previous"], out + ".previous")
        self.assertEqual(sorted(os.listdir(os.path.dirname(out))), before)

    def denied(self, name, deny):
        """Runs an overwrite build while os.<name> raises PermissionError when deny(args) is true."""
        source, out, first = self.first_pack()
        job = jobs.validate(self.build("sequence", source, "music.o2r", {"name": "Two"}, overwrite=True,
                                       job="j5")[1], "build")
        real = getattr(jobs.os, name)

        def guarded(*args, **kwargs):
            if deny(args):
                raise PermissionError(13, "Access is denied", args[-1])
            return real(*args, **kwargs)

        setattr(jobs.os, name, guarded)
        try:
            final = jobs.run_build(job, lambda event: None)
        finally:
            setattr(jobs.os, name, real)
        self.assertEqual((final["event"], final["code"]), ("job_error", "io"), final)
        self.assertIn("Access is denied", final["message"])
        with open(out, "rb") as fh:
            self.assertEqual(fh.read(), first)
        self.assertFalse(os.path.exists(job["staging"]))
        return out

    def test_denied_staging_folder_leaves_the_pack(self):
        out = self.denied("mkdir", lambda args: os.path.basename(args[0]).startswith(jobs.STAGING_PREFIX))
        self.assertFalse(os.path.exists(out + ".previous"))

    def test_denied_install_puts_the_old_pack_back(self):
        # The old pack has already moved to .previous when the new one cannot take its place.
        out = self.denied("replace", lambda args: args[1].endswith("music.o2r") and
                          jobs.STAGING_PREFIX in args[0])
        self.assertFalse(os.path.exists(out + ".previous"))

    def test_denied_backup_never_touches_the_pack(self):
        self.denied("replace", lambda args: args[1].endswith(jobs.PREVIOUS_SUFFIX))

    def test_damaged_pack_is_never_published(self):
        source, out, first = self.first_pack()
        job = jobs.validate(self.build("sequence", source, "music.o2r", {}, overwrite=True, job="j4")[1], "build")
        real = jobs.run_packer

        def broken(job_, args, emit):
            status = real(job_, args, emit)
            if "--check" not in args:
                with open(args[1], "wb") as fh:
                    fh.write(b"PK not really a zip")
            return status

        jobs.run_packer = broken
        try:
            final = jobs.run_build(job, lambda event: None)
        finally:
            jobs.run_packer = real
        self.assertEqual((final["event"], final["code"]), ("job_error", "invalid_artifact"))
        with open(out, "rb") as fh:
            self.assertEqual(fh.read(), first)
        self.assertFalse(os.path.exists(job["staging"]))


class Refusals(Scratch):
    def refused(self, request, code):
        _, events = run_worker([request])
        final = finals(events)
        self.assertEqual(len(final), 1, events)
        self.assertEqual(final[0]["event"], "job_error")
        self.assertEqual(final[0]["code"], code, final[0])
        return final[0]

    def test_requests_that_never_run(self):
        source = self.sequence_input()
        out, good = self.build("sequence", source, "music.o2r")
        cases = [
            (dict(good, packer="nope"), "unsupported_packer"),
            (dict(good, job="has space"), "bad_job"),
            (dict(good, job=""), "bad_job"),
            (dict(good, job="x" * 65), "bad_job"),
            (dict(good, input="relative/seq"), "bad_request"),
            (dict(good, input=self.path("missing")), "missing_input"),
            (dict(good, output=self.path("worker", "music.zip")), "bad_request"),
            (dict(good, output=self.path("nowhere", "music.o2r")), "missing_output_folder"),
            (dict(good, staging=self.path("elsewhere", jobs.STAGING_PREFIX + "j1")), "bad_request"),
            (dict(good, staging=self.path("worker", "staging-j1")), "bad_request"),
            (dict(good, overwrite="yes"), "bad_request"),
            (dict(good, options={"bogus": "x"}), "bad_options"),
            (dict(good, options={"name": "--check"}), "bad_options"),
            (dict(good, options={"name": "a\nb"}), "bad_options"),
            (dict(good, options={"name": 5}), "bad_options"),
            (dict(good, options={"manifest": "relative.json"}), "bad_options"),
            (dict(good, options=[]), "bad_options"),
            (dict(good, extra=1), "bad_request"),
            (dict(good, catalog=self.root), "bad_request"),
        ]
        for request, code in cases:
            with self.subTest(code=code, request=request):
                self.refused(request, code)
        self.assertFalse(os.path.exists(out))

    def test_staging_folder_must_be_new(self):
        source = self.sequence_input()
        _, request = self.build("sequence", source, "music.o2r")
        os.makedirs(request["staging"])
        with open(os.path.join(request["staging"], "keep.txt"), "w") as fh:
            fh.write("someone else's")
        self.refused(request, "staging_exists")
        self.assertTrue(os.path.isfile(os.path.join(request["staging"], "keep.txt")))

    def test_packer_specific_rules(self):
        cache, base = self.htc_input()
        _, htc = self.build("htc", cache, "hd.o2r", {"name": "HD", "author": "me", "base": [base]})
        self.refused(dict(htc, options={"name": "HD", "author": "me"}), "bad_options")
        self.refused(dict(htc, options={"name": "HD", "author": "me", "base": []}), "bad_options")
        self.refused(dict(htc, options={"name": "HD", "author": "me", "base": [base], "alt": True,
                                        "direct_keys": True}), "bad_options")
        self.refused(dict(htc, options={"name": "HD", "author": "me", "base": [base], "alt": "yes"}),
                     "bad_options")
        source, _ = self.sample_input()
        _, sample = self.build("sample", source, "s.o2r")
        self.refused(sample, "bad_request")  # sample needs an explicit catalog
        self.refused(dict(sample, catalog=self.path("no-audio")), "missing_input")

    def test_worker_keeps_serving_after_a_refusal(self):
        source = self.sequence_input()
        out, good = self.build("sequence", source, "music.o2r")
        _, events = run_worker([dict(good, packer="nope"), good, {"op": "hello"}])
        kinds = [e["event"] for e in events if e["event"] != "log"]
        self.assertEqual(kinds, ["job_error", "result", "hello", "shutdown"])
        self.assertTrue(os.path.isfile(out))

    def test_a_packer_that_cannot_load_only_refuses_its_job(self):
        source = self.sequence_input()
        out, request = self.build("sequence", source, "music.o2r")
        spec = jobs.PACKERS["sequence"]
        real = spec["load"]

        def missing():
            raise ImportError("No module named 'gen_sequence_pack'")

        spec["load"] = missing
        try:
            final = jobs.run(request, "build", lambda event: None)
        finally:
            spec["load"] = real
        self.assertEqual((final["event"], final["job"], final["code"]), ("job_error", "j1", "packer_unavailable"))
        self.assertIn("ImportError", final["message"])
        self.assertFalse(os.path.exists(out))
        self.assertFalse(os.path.exists(request["staging"]))
        self.assertEqual(jobs.run(request, "build", lambda event: None)["event"], "result")

    def test_texture_options_are_typed(self):
        source = self.texture_input()
        pack_file = self.path("pack.o2r")
        with open(pack_file, "wb") as fh:
            fh.write(b"not a pack")
        out, good = self.build("texture", source, "textures.o2r", {"name": "Tex"})
        cases = [
            {"bogus": "x"},
            {"name": "--check"},
            {"name": "a\nb"},
            {"name": 5},
            {"manifest": "relative.json"},
            {"manifest": self.path("missing.json")},
            {"native_manifest": self.path("missing.tsv")},
            {"key_scheme_version": 2},
            {"direct_keys": "yes"},
            {"alt": True},
            {"base": [source]},
        ]
        for options in cases:
            with self.subTest(options=options):
                self.refused(dict(good, options=options), "bad_options")
        self.refused(dict(good, input=pack_file), "missing_input")
        self.assertFalse(os.path.exists(out))

    def test_texture_without_pillow_only_refuses_its_job(self):
        # Runs whether or not Pillow is installed: None in sys.modules makes "import PIL" fail.
        source = self.texture_input()
        out, texture = self.build("texture", source, "textures.o2r")
        _, sequence = self.build("sequence", self.sequence_input(), "music.o2r")
        # patch.dict restores sys.modules on exit; the packer module is dropped so its import runs again.
        with mock.patch.dict(sys.modules, {"PIL": None, "PIL.Image": None}):
            sys.modules.pop("gen_texture_pack", None)
            with contextlib.redirect_stderr(io.StringIO()):
                final = jobs.run(texture, "build", lambda event: None)
        self.assertEqual((final["event"], final["job"], final["code"]), ("job_error", "j1", "packer_unavailable"))
        self.assertIn("PIL", final["message"])
        self.assertFalse(os.path.exists(out))
        self.assertFalse(os.path.exists(texture["staging"]))
        self.assertEqual(jobs.run(sequence, "build", lambda event: None)["event"], "result")


class Log(unittest.TestCase):
    def test_lines_are_split_cut_and_capped(self):
        events = []
        log = jobs.JobLog(events.append, "j", "out")
        log.budget = [100]
        log.write("first\r\nsecond")
        self.assertEqual([e["text"] for e in events], ["first"])
        log.finish()
        self.assertEqual(events[-1]["text"], "second")
        log.write("x" * 200 + "\n")
        self.assertEqual(events[-1]["stream"], "err")
        self.assertIn("dropped", events[-1]["text"])
        count = len(events)
        log.write("more\n")
        self.assertEqual(len(events), count)

    def test_long_line_is_cut_on_a_character(self):
        events = []
        log = jobs.JobLog(events.append, "j", "err")
        log.budget = [jobs.MAX_JOB_LOG_BYTES]
        log.write("é" * jobs.MAX_LOG_LINE_BYTES + "\n")
        text = events[0]["text"]
        self.assertTrue(text.endswith(" [cut]"))
        self.assertLessEqual(len(text.encode("utf-8")), jobs.MAX_LOG_LINE_BYTES + 6)

    def test_every_event_fits_one_protocol_line(self):
        events = []
        log = jobs.JobLog(events.append, "j" * 64, "out")
        log.budget = [jobs.MAX_JOB_LOG_BYTES]
        log.write("☃" * 10000 + "\n")
        for event in events:
            line = json.dumps(dict({"v": 1}, **event), separators=(",", ":"), ensure_ascii=True)
            self.assertLessEqual(len(line), 64 * 1024)


class Projects(Scratch):
    """Model and soundfont: project creation, names, and the same packs as the command line."""

    def model_dump(self):
        dump = self.folder("models")
        with open(os.path.join(dump, "machine_models_D_9001210.obj"), "w", encoding="utf-8", newline="\n") as fh:
            fh.write("mtllib machine_models_D_9001210.mtl\nv 0 0 0 1 1 1\nv 10 0 0 1 1 1\nv 0 10 0 1 1 1\n"
                     "usemtl body--flat\nf 1 2 3\n")
        with open(os.path.join(dump, "machine_models_D_9001210.mtl"), "w", encoding="utf-8", newline="\n") as fh:
            fh.write("newmtl body--flat\nKd 1 0 0\n")
        return dump

    def font_dump(self):
        dump = self.folder("audio")
        os.makedirs(os.path.join(dump, "fonts"), exist_ok=True)
        with open(os.path.join(dump, "fonts", "ek_FONT_GUITAR.json"), "w", encoding="utf-8") as fh:
            json.dump({"fontIndex": 0, "samples": {"S1": {"playbackRateHz": 32000}},
                       "instruments": [{"index": 0, "normalRangeLo": 0, "normalRangeHi": 127,
                                        "adsrDecayIndex": 0, "normalPitch": {"sample": "S1", "tuning": 1.0}}]},
                      fh)
        return dump

    def init(self, packer, choice_key, choice, catalog, name="project", job="i1", options=None):
        dest = os.path.join(self.folder("projects"), name)
        request = {"op": "init", "job": job, "packer": packer, "output": dest, "catalog": catalog,
                   "staging": os.path.join(os.path.dirname(dest), jobs.STAGING_PREFIX + job),
                   "options": dict({choice_key: choice}, **(options or {}))}
        return dest, request

    def tree(self, root):
        found = {}
        for folder, _, files in os.walk(root):
            for name in files:
                path = os.path.join(folder, name)
                with open(path, "rb") as fh:
                    found[os.path.relpath(path, root).replace(os.sep, "/")] = fh.read()
        return found

    def test_model_init_matches_the_command_line(self):
        dump = self.model_dump()
        cli = os.path.join(self.folder("cli"), "work")
        code, text = run_cli("gen_model_pack.py", ["init", "blue_falcon", "-o", cli, "--dump-dir", dump])
        self.assertEqual(code, 0, text)
        dest, request = self.init("model", "machine", "blue_falcon", dump)
        _, events = run_worker([request])
        final = finals(events)[0]
        self.assertEqual((final["event"], final["op"]), ("result", "init"), final)
        self.assertEqual(final["project"], dest)
        self.assertEqual(self.tree(dest), self.tree(cli))
        self.assertFalse(os.path.exists(request["staging"]))

    def test_model_build_and_check_match_the_command_line(self):
        dump = self.model_dump()
        work = os.path.join(self.root, "work")
        code, text = run_cli("gen_model_pack.py", ["init", "blue_falcon", "-o", work, "--dump-dir", dump])
        self.assertEqual(code, 0, text)
        cli_out = os.path.join(self.folder("cli"), "pack.o2r")
        code, text = run_cli("gen_model_pack.py", ["pack", work, "-o", cli_out, "--name", "Falcon", "--author", "me"])
        self.assertEqual(code, 0, text)
        out, request = self.build("model", work, "pack.o2r", {"name": "Falcon", "author": "me"})
        _, events = run_worker([request])
        final = finals(events)[0]
        self.assertEqual(final["event"], "result", final)
        with open(out, "rb") as a, open(cli_out, "rb") as b:
            self.assertEqual(a.read(), b.read())
        for target in (work, out):
            code, _ = run_cli("gen_model_pack.py", ["check", target])
            _, events = run_worker([{"op": "check", "job": "c1", "packer": "model", "input": target,
                                     "options": {"name": "ignored by check", "allow_overbudget": True}}])
            final = finals(events)[0]
            self.assertEqual((final["event"], final["exit"]), ("result", code), final)

    def test_model_over_budget_check_follows_the_flag(self):
        dump = self.model_dump()
        work = os.path.join(self.root, "work")
        run_cli("gen_model_pack.py", ["init", "blue_falcon", "-o", work, "--dump-dir", dump])
        with open(os.path.join(work, "lod1.obj"), "a", encoding="utf-8", newline="\n") as fh:
            fh.write("f 1 2 3\n" * 200)
        for flag in (False, True):
            cli = ["check", work] + (["--allow-overbudget"] if flag else [])
            code, _ = run_cli("gen_model_pack.py", cli)
            self.assertEqual(code, 0 if flag else 1)
            _, events = run_worker([{"op": "check", "job": "c1", "packer": "model", "input": work,
                                     "options": {"allow_overbudget": flag}}])
            self.assertEqual(finals(events)[0]["exit"], code)

    def test_soundfont_init_pack_and_check_match_the_command_line(self):
        dump = self.font_dump()
        cli = os.path.join(self.folder("cli"), "font")
        code, text = run_cli("gen_soundfont_pack.py", ["init", "FONT_GUITAR", "-o", cli, "--dump-dir", dump,
                                                       "--skip-samples"])
        self.assertEqual(code, 0, text)
        dest, request = self.init("soundfont", "font", "FONT_GUITAR", dump, options={"skip_samples": True})
        _, events = run_worker([request])
        self.assertEqual(finals(events)[0]["event"], "result", events)
        self.assertEqual(self.tree(dest), self.tree(cli))
        cli_out = os.path.join(self.folder("cli"), "font.o2r")
        code, text = run_cli("gen_soundfont_pack.py", ["pack", cli, "-o", cli_out, "--name", "Font"])
        self.assertEqual(code, 0, text)
        out, request = self.build("soundfont", dest, "font.o2r", {"name": "Font"})
        _, events = run_worker([request])
        self.assertEqual(finals(events)[0]["event"], "result", events)
        with open(out, "rb") as a, open(cli_out, "rb") as b:
            self.assertEqual(a.read(), b.read())
        _, events = run_worker([{"op": "check", "job": "c1", "packer": "soundfont", "input": out}])
        self.assertEqual((finals(events)[0]["status"], finals(events)[0]["exit"]), ("ok", 0))

    def test_names_lists_the_packer_choices(self):
        import gen_model_pack
        import gen_soundfont_pack
        _, events = run_worker([{"op": "names", "job": "n1", "packer": "model"},
                                {"op": "names", "job": "n2", "packer": "soundfont"},
                                {"op": "names", "job": "n3", "packer": "sequence"}])
        final = finals(events)
        self.assertEqual(final[0]["names"], sorted(gen_model_pack.MACHINES))
        self.assertEqual(final[1]["names"], list(gen_soundfont_pack.FONT_NAMES))
        self.assertEqual((final[2]["event"], final[2]["code"]), ("job_error", "bad_request"))

    def test_init_refusals_touch_nothing(self):
        dump = self.model_dump()
        dest, existing = self.init("model", "machine", "blue_falcon", dump)
        os.makedirs(dest)
        _, good = self.init("model", "machine", "blue_falcon", dump, name="fresh")
        cases = [
            (existing, "exists"),
            (dict(good, output=os.path.join(self.root, "missing", "p"),
                  staging=os.path.join(self.root, "missing", jobs.STAGING_PREFIX + "i1")), "missing_output_folder"),
            (dict(good, catalog=os.path.join(self.root, "nope")), "missing_input"),
            (dict(good, options={}), "bad_options"),
            (dict(good, options={"machine": "blue_falcon", "skip_samples": True}), "bad_options"),
            (dict(good, input=dump), "bad_request"),
            (dict(good, packer="sequence"), "bad_request"),
        ]
        for request, code in cases:
            _, events = run_worker([request])
            final = finals(events)[0]
            self.assertEqual((final["event"], final["code"]), ("job_error", code), (request, final))
        self.assertEqual(os.listdir(dest), [])
        self.assertFalse(os.path.exists(good["output"]))
        self.assertFalse(os.path.exists(good["staging"]))

    def test_failed_init_leaves_no_project(self):
        dump = self.folder("empty-dump")
        dest, request = self.init("model", "machine", "blue_falcon", dump)
        _, events = run_worker([request])
        final = finals(events)[0]
        self.assertEqual((final["event"], final["code"]), ("job_error", "packer_failed"), final)
        self.assertFalse(os.path.exists(dest))
        self.assertFalse(os.path.exists(request["staging"]))


@unittest.skipUnless(HAVE_PILLOW, PILLOW_SKIP)
class Readers(Scratch):
    """Inspect and unpack read someone else's pack through tools/unpack_pack.py."""

    tree = Projects.tree

    def foreign_pack(self, name="theirs.o2r", extra=None):
        source = self.texture_input()
        out, request = self.build("texture", source, name, {"name": "Theirs", "author": "someone"})
        _, events = run_worker([request])
        self.assertEqual(finals(events)[0]["event"], "result", events)
        if extra:
            with zipfile.ZipFile(out, "a") as archive:
                for member, data in extra.items():
                    info = zipfile.ZipInfo(member)
                    info.filename = member
                    archive.writestr(info, data)
        return out

    def unpack_request(self, pack, name="theirs-unpacked", job="u1"):
        dest = os.path.join(self.folder("unpacked"), name)
        return dest, {"op": "unpack", "job": job, "packer": "unpack", "input": pack, "output": dest,
                      "staging": os.path.join(os.path.dirname(dest), jobs.STAGING_PREFIX + job)}

    def test_inspect_matches_the_command_line_listing(self):
        pack = self.foreign_pack(extra={"scripts/lap.lua": b"print(1)\n"})
        code, text = run_cli("unpack_pack.py", [pack, "--list", "--json"])
        self.assertEqual(code, 0, text)
        _, events = run_worker([{"op": "inspect", "job": "r1", "packer": "unpack", "input": pack}])
        final = finals(events)[0]
        self.assertEqual((final["event"], final["op"], final["status"]), ("result", "inspect", "ok"), final)
        listing = json.loads(text)
        for key in ("format", "title", "author", "summary", "entries", "total"):
            self.assertEqual(final[key], listing[key], key)
        self.assertEqual(final["title"], "Theirs")
        self.assertIn("1 x Lua script", final["summary"])
        self.assertEqual(os.listdir(os.path.dirname(pack)), [os.path.basename(pack)])

    def test_unpack_matches_the_command_line_folder(self):
        pack = self.foreign_pack(extra={"scripts/lap.lua": b"print(1)\n"})
        cli = os.path.join(self.folder("cli"), "out")
        code, text = run_cli("unpack_pack.py", [pack, "-o", cli])
        self.assertEqual(code, 0, text)
        dest, request = self.unpack_request(pack)
        _, events = run_worker([request])
        final = finals(events)[0]
        self.assertEqual((final["event"], final["op"]), ("result", "unpack"), final)
        self.assertEqual(final["folder"], dest)
        self.assertEqual(final["failed"], 0)
        self.assertEqual(final["written"], final["total"])
        self.assertEqual(self.tree(dest), self.tree(cli))
        self.assertTrue(any(e["event"] == "log" and "unpacked" in e["text"] for e in events), events)
        self.assertFalse(os.path.exists(request["staging"]))

    def test_unpacked_textures_build_the_same_pack_again(self):
        pack = self.foreign_pack()
        dest, request = self.unpack_request(pack)
        _, events = run_worker([request])
        self.assertEqual(finals(events)[0]["event"], "result", events)
        again, build = self.build("texture", os.path.join(dest, "textures"), "again.o2r",
                                  {"name": "Theirs", "author": "someone"})
        _, events = run_worker([build])
        self.assertEqual(finals(events)[0]["event"], "result", events)
        first, second = members(pack), members(again)
        first.pop("workshop.json")
        second.pop("workshop.json")
        self.assertEqual(first, second)

    def test_unsafe_names_stay_inside_the_folder(self):
        pack = self.foreign_pack(extra={"../escape.txt": b"x", "..\\escape2.txt": b"x"})
        dest, request = self.unpack_request(pack)
        _, events = run_worker([request])
        final = finals(events)[0]
        self.assertEqual(final["failed"], 2, final)
        self.assertFalse(os.path.exists(os.path.join(os.path.dirname(dest), "escape.txt")))
        self.assertFalse(os.path.exists(os.path.join(os.path.dirname(dest), "escape2.txt")))

    def test_reader_refusals_touch_nothing(self):
        pack = self.foreign_pack()
        dest, good = self.unpack_request(pack)
        taken, existing = self.unpack_request(pack, name="taken")
        os.makedirs(taken)
        inspect = {"op": "inspect", "job": "r1", "packer": "unpack", "input": pack}
        cases = [
            (dict(good, packer="texture"), "unsupported_packer"),
            (dict(inspect, packer="texture"), "unsupported_packer"),
            (dict(inspect, output=dest), "bad_request"),
            (dict(inspect, options={}), "bad_request"),
            (dict(inspect, input="relative.o2r"), "bad_request"),
            (dict(inspect, input=self.path("missing.o2r")), "missing_input"),
            (dict(inspect, catalog=self.path("no-dump")), "missing_input"),
            (existing, "exists"),
            (dict(good, output=os.path.join(self.root, "missing", "p"),
                  staging=os.path.join(self.root, "missing", jobs.STAGING_PREFIX + "u1")), "missing_output_folder"),
            (dict(good, staging=os.path.join(self.root, jobs.STAGING_PREFIX + "u1")), "bad_request"),
            ({"op": "check", "job": "c1", "packer": "unpack", "input": pack}, "unsupported_packer"),
        ]
        for request, code in cases:
            with self.subTest(code=code, request=request):
                _, events = run_worker([request])
                final = finals(events)[0]
                self.assertEqual((final["event"], final["code"]), ("job_error", code), final)
        self.assertEqual(os.listdir(taken), [])
        self.assertFalse(os.path.exists(dest))
        self.assertFalse(os.path.exists(good["staging"]))

    def test_a_pack_that_is_not_a_zip_leaves_no_folder(self):
        bogus = self.path("old.otr")
        with open(bogus, "wb") as fh:
            fh.write(b"MPQ\x1a" + b"\x00" * 64)
        _, events = run_worker([{"op": "inspect", "job": "r1", "packer": "unpack", "input": bogus}])
        self.assertEqual(finals(events)[0]["code"], "unreadable")
        dest, request = self.unpack_request(bogus)
        _, events = run_worker([request])
        self.assertEqual(finals(events)[0]["code"], "unreadable")
        self.assertFalse(os.path.exists(dest))
        self.assertFalse(os.path.exists(request["staging"]))

    def test_non_ascii_listing_fits_one_protocol_line(self):
        pack = self.path("wide.o2r")
        with zipfile.ZipFile(pack, "w") as archive:
            for i in range(600):
                archive.writestr("テクスチャ/%04d_%s.txt" % (i, "音" * 60), b"")
        long_name = "映" * 40
        dest = os.path.join(self.folder(long_name), "out")
        request = {"op": "unpack", "job": "u1", "packer": "unpack", "input": pack, "output": dest,
                   "staging": os.path.join(os.path.dirname(dest), jobs.STAGING_PREFIX + "u1")}
        code, events = run_worker([{"op": "inspect", "job": "r1", "packer": "unpack", "input": pack}, request])
        self.assertEqual(code, 0)
        for final in finals(events):
            self.assertEqual(final["event"], "result", final)
            self.assertEqual(final["total"], 600)
            self.assertLess(len(final["entries"]), 600)
            self.assertLessEqual(len(json.dumps(final, separators=(",", ":"))), 64 * 1024)

    def test_a_reader_bug_only_fails_its_job(self):
        pack = self.foreign_pack()
        broken = jobs.READERS["unpack"]["load"]()

        def explode(*args, **kwargs):
            raise KeyError("boom")

        with mock.patch.object(broken, "scan", explode), mock.patch.object(broken, "unpack_into", explode):
            final = jobs.run({"v": 1, "op": "inspect", "job": "r1", "packer": "unpack", "input": pack}, "inspect",
                             lambda event: None)
            self.assertEqual((final["event"], final["code"]), ("job_error", "reader_failed"), final)
            dest, request = self.unpack_request(pack)
            final = jobs.run(dict(request, v=1), "unpack", lambda event: None)
            self.assertEqual((final["event"], final["code"]), ("job_error", "reader_failed"), final)
        self.assertFalse(os.path.exists(dest))
        self.assertFalse(os.path.exists(request["staging"]))

    def test_fit_trims_entries_and_keeps_the_total(self):
        event = {"event": "result", "job": "r1", "op": "unpack", "folder": "映" * 4096,
                 "entries": ["音" * 300] * 400, "total": 400}
        jobs._fit(event)
        self.assertLessEqual(len(json.dumps(event, separators=(",", ":"))), jobs.MAX_RESULT_BYTES)
        self.assertGreater(len(event["entries"]), 0)
        self.assertEqual(event["total"], 400)


if __name__ == "__main__":
    unittest.main()
