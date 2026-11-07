# Copyright (c) Zorkats
"""Freezes the G-Diffuser Companion worker with PyInstaller into build/companion-worker/.

Build-only tooling. PyInstaller and Pillow are pinned and installed into a venv under build/, never
into the system Python. Players do not need Python: the frozen worker is copied next to the Companion
by CMake.

Steps:
  1. Recreate build/companion-venv with the interpreter that runs this script (python -I).
  2. Install pyinstaller==6.22.3 and pillow==13.0.0 from wheels only.
  3. Freeze companion/worker/gdx_companion_worker.py one-folder into build/companion-worker/, with
     the job module, the htc, sample, sequence, texture, model and soundfont packers, Pillow, and
     tools/music_targets.json. Pillow's LICENSE is copied to _internal/LICENSES/MIT-CMU-Pillow.txt
     and the interpreter's LICENSE.txt to _internal/LICENSES/PSF-2.0-Python.txt, the repository's
     LICENSES naming (<SPDX id>-<component>.txt).
  4. Smoke test with Python removed from PATH: a hello request, then sequence, sample, htc and texture
     jobs in a temporary folder (the texture PNG is written here with zlib, not Pillow), model and
     soundfont names requests, inspect and unpack of the texture pack, and an import of every bundled
     module. The Pillow license copy must be
     present and match the venv's file.

Usage: python -I tools/build_companion_worker.py
Exit status is 0 only when every step succeeds. Failures print the exact command and its output.
"""

import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib
from pathlib import Path

PYINSTALLER_VERSION = "6.22.3"
# Newest Pillow on the index with a wheel for the build Python (cp313 win_amd64), checked 2026-11-06.
PILLOW_VERSION = "13.0.0"
PILLOW_LICENSE_NAME = "MIT-CMU-Pillow.txt"
# The frozen worker carries the interpreter's DLLs (python3xx, OpenSSL, libffi, bzip2, xz, expat,
# mpdecimal). The base interpreter's LICENSE.txt holds the PSF license and those components' notices.
PYTHON_LICENSE_NAME = "PSF-2.0-Python.txt"
EXE_NAME = "gdx_companion_worker.exe" if os.name == "nt" else "gdx_companion_worker"
PYTHON_NAMES = ("python.exe", "python3.exe", "py.exe", "python", "python3")

ROOT = Path(__file__).resolve().parent.parent
BUILD = ROOT / "build"
VENV = BUILD / "companion-venv"
FREEZE_TMP = BUILD / "companion-freeze"
PYI_WORK = BUILD / "companion-pyi-work"
OUTPUT = BUILD / "companion-worker"
WORKER_SOURCE = ROOT / "companion" / "worker" / "gdx_companion_worker.py"
TOOLS = ROOT / "tools"
# Imported by name at job time, so PyInstaller cannot see them on its own. PIL.PngImagePlugin is listed
# so the PNG decoder that gen_texture_pack reads with is bundled explicitly.
HIDDEN_IMPORTS = ("gdx_companion_jobs", "gen_htc_pack", "gen_texture_pack", "gen_sample_pack",
                  "gen_sequence_pack", "gen_model_pack", "gen_soundfont_pack", "unpack_pack", "n64_texels",
                  "PIL.Image",
                  "PIL.PngImagePlugin")
DATA_FILES = (TOOLS / "music_targets.json",)
# Prints the venv's Pillow LICENSE path: the dist-info copy that RECORD lists.
PILLOW_LICENSE_PROBE = (
    "import importlib.metadata as m\n"
    "files = m.distribution('pillow').files or []\n"
    "print(next((str(f.locate()) for f in files if str(f).endswith('.dist-info/licenses/LICENSE')), ''))\n"
)


def fail(message):
    print("build_companion_worker: FAILED: " + message, file=sys.stderr)
    sys.exit(1)


def run(step, command, env=None, stdin_text=None):
    print("build_companion_worker: " + step)
    print("  command: " + " ".join(str(part) for part in command))
    result = subprocess.run(
        [str(part) for part in command],
        capture_output=True,
        text=True,
        env=env,
        input=stdin_text,
    )
    if result.stdout.strip():
        print(result.stdout.rstrip())
    if result.returncode != 0:
        if result.stderr.strip():
            print(result.stderr.rstrip(), file=sys.stderr)
        fail(step + " exited with status %d" % result.returncode)
    return result


def venv_python():
    if os.name == "nt":
        return VENV / "Scripts" / "python.exe"
    return VENV / "bin" / "python"


def pillow_license(py):
    """Returns the LICENSE file of the Pillow wheel installed in the venv."""
    found = run("find Pillow's LICENSE file", [py, "-I", "-c", PILLOW_LICENSE_PROBE]).stdout.strip()
    if not found or not Path(found).is_file():
        fail("Pillow's LICENSE file was not found in the venv: " + (found or "no path reported"))
    return Path(found)


def png_chunk(kind, data):
    return (struct.pack(">I", len(data)) + kind + data
            + struct.pack(">I", zlib.crc32(kind + data)))


def tiny_png(width, height):
    """A solid 8-bit RGBA PNG, built with zlib so the smoke test does not need Pillow."""
    header = struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)
    rows = b"".join(b"\x00" + b"\x40\x80\xc0\xff" * width for _ in range(height))
    return (b"\x89PNG\r\n\x1a\n" + png_chunk(b"IHDR", header) + png_chunk(b"IDAT", zlib.compress(rows))
            + png_chunk(b"IEND", b""))


def strip_python_from_path(path_value):
    kept = []
    for entry in path_value.split(os.pathsep):
        if not entry:
            continue
        if any(os.path.isfile(os.path.join(entry, name)) for name in PYTHON_NAMES):
            continue
        kept.append(entry)
    return os.pathsep.join(kept)


def frozen_env():
    env = dict(os.environ)
    env["PATH"] = strip_python_from_path(os.environ.get("PATH", ""))
    for name in ("PYTHONHOME", "PYTHONPATH", "PYTHONSTARTUP"):
        env.pop(name, None)
    return env


def ask_frozen_worker(step, requests):
    """Sends requests, one per line, and returns every event the worker wrote except logs."""
    stdin_text = "".join(json.dumps(dict({"v": 1}, **request)) + "\n" for request in requests)
    result = run(step, [OUTPUT / EXE_NAME], env=frozen_env(), stdin_text=stdin_text)
    events = [json.loads(line) for line in result.stdout.splitlines() if line.strip()]
    return [event for event in events if event.get("event") != "log"]


def smoke_test_frozen_worker(pillow_license_source):
    exe = OUTPUT / EXE_NAME
    if not exe.is_file():
        fail("frozen worker missing after freeze: " + str(exe))
    for data in DATA_FILES:
        if not (OUTPUT / "_internal" / data.name).is_file():
            fail("frozen worker is missing its data file " + data.name)
    license_copy = OUTPUT / "_internal" / "LICENSES" / PILLOW_LICENSE_NAME
    if not license_copy.is_file() or license_copy.read_bytes() != pillow_license_source.read_bytes():
        fail("frozen worker is missing Pillow's license text at " + str(license_copy))

    events = ask_frozen_worker("frozen worker hello (PATH without Python)", [{"op": "hello"}])
    expected = {
        "v": 1,
        "event": "hello",
        "worker_version": "0.4.0",
        "protocol": 1,
        "ops": ["hello", "shutdown", "check", "plan", "build", "init", "names", "inspect", "unpack"],
    }
    if not events or events[0] != expected:
        fail("frozen worker hello reply differs from protocol 1: " + json.dumps(events[:1]))
    print("build_companion_worker: hello reply: " + json.dumps(events[0]))

    # A real build plus a check per packer: any packer the freeze left out answers packer_unavailable.
    with tempfile.TemporaryDirectory(prefix="gdx-worker-smoke-") as temp:
        root = Path(temp)
        seq = root / "seq"
        seq.mkdir()
        (seq / "ddbgm_mute_city.seq").write_bytes(bytes([0xD3, 0x80, 0xD7, 0xFF, 0xFF, 0]) + bytes(26))
        samples = root / "samples"
        samples.mkdir()
        audio = root / "audio"
        audio.mkdir()
        cache = root / "pack.htc"
        cache.write_bytes(b"")
        base = root / "base.o2r"
        base.write_bytes(b"")
        tex = root / "tex"
        (tex / "smoke").mkdir(parents=True)
        (tex / "manifest.tsv").write_bytes(b"smoke/tile\t2\t2\tRGBA16\n")
        (tex / "smoke" / "tile.png").write_bytes(tiny_png(4, 4))
        out = root / "music.o2r"
        tex_out = root / "textures.o2r"
        unpacked = root / "textures-unpacked"
        events = ask_frozen_worker("frozen worker jobs (PATH without Python)", [
            {"op": "build", "job": "s1", "packer": "sequence", "input": str(seq), "output": str(out),
             "staging": str(root / ".gdx-companion-staging-s1"), "overwrite": False, "options": {}},
            {"op": "check", "job": "s2", "packer": "sample", "input": str(samples), "catalog": str(audio),
             "options": {}},
            {"op": "check", "job": "s3", "packer": "htc", "input": str(cache),
             "options": {"name": "Smoke", "author": "Smoke", "base": [str(base)]}},
            {"op": "check", "job": "t1", "packer": "texture", "input": str(tex), "options": {}},
            {"op": "build", "job": "t2", "packer": "texture", "input": str(tex), "output": str(tex_out),
             "staging": str(root / ".gdx-companion-staging-t2"), "overwrite": False,
             "options": {"name": "Smoke", "author": "Smoke"}},
            {"op": "names", "job": "m1", "packer": "model"},
            {"op": "names", "job": "f1", "packer": "soundfont"},
            {"op": "inspect", "job": "r1", "packer": "unpack", "input": str(tex_out), "catalog": str(tex)},
            {"op": "unpack", "job": "r2", "packer": "unpack", "input": str(tex_out), "catalog": str(tex),
             "output": str(unpacked), "staging": str(root / ".gdx-companion-staging-r2")},
            {"op": "shutdown"},
        ])
        by_job = {event.get("job"): event for event in events if "job" in event}
        build = by_job.get("s1", {})
        if build.get("event") != "result" or build.get("status") != "ok" or not out.is_file():
            fail("frozen worker sequence build failed: " + json.dumps(build))
        for job, packer in (("s2", "sample"), ("s3", "htc"), ("t1", "texture"), ("t2", "texture")):
            event = by_job.get(job)
            if event is None or event.get("code") == "packer_unavailable":
                fail("frozen worker cannot run the %s packer: %s" % (packer, json.dumps(event)))
        texture_check = by_job.get("t1", {})
        if texture_check.get("event") != "result" or texture_check.get("status") != "ok":
            fail("frozen worker texture check failed: " + json.dumps(texture_check))
        texture_build = by_job.get("t2", {})
        if texture_build.get("event") != "result" or texture_build.get("status") != "ok" \
                or not tex_out.is_file():
            fail("frozen worker texture build failed: " + json.dumps(texture_build))
        for job, packer in (("m1", "model"), ("f1", "soundfont")):
            event = by_job.get(job, {})
            if event.get("event") != "result" or not event.get("names"):
                fail("frozen worker cannot list the %s packer's choices: %s" % (packer, json.dumps(event)))
        inspected = by_job.get("r1", {})
        if inspected.get("event") != "result" or inspected.get("total", 0) < 2:
            fail("frozen worker cannot inspect a pack: " + json.dumps(inspected))
        unpacked_event = by_job.get("r2", {})
        if unpacked_event.get("event") != "result" or unpacked_event.get("failed") != 0 or \
                not (unpacked / "textures" / "smoke" / "tile.png").is_file():
            fail("frozen worker cannot unpack a pack: " + json.dumps(unpacked_event))
        if events[-1].get("event") != "shutdown":
            fail("frozen worker did not confirm shutdown after the jobs")
    print("build_companion_worker: sequence build, sample, htc and texture packers load; "
          "texture check and build ok; model and soundfont list their choices; the texture pack "
          "inspects and unpacks")


def main():
    if not WORKER_SOURCE.is_file():
        fail("worker source not found: " + str(WORKER_SOURCE))

    BUILD.mkdir(exist_ok=True)
    for stale in (VENV, FREEZE_TMP, PYI_WORK, OUTPUT):
        if stale.exists():
            shutil.rmtree(stale)

    run("create venv", [sys.executable, "-I", "-m", "venv", VENV])
    py = venv_python()
    run(
        "install pyinstaller==" + PYINSTALLER_VERSION + " (wheels only)",
        [py, "-I", "-m", "pip", "install", "--disable-pip-version-check",
         "--only-binary=:all:", "pyinstaller==" + PYINSTALLER_VERSION],
    )
    version = run(
        "check PyInstaller version",
        [py, "-I", "-c", "import PyInstaller; print(PyInstaller.__version__)"],
    ).stdout.strip()
    if version != PYINSTALLER_VERSION:
        fail("installed PyInstaller is %s, expected %s" % (version, PYINSTALLER_VERSION))

    run(
        "install pillow==" + PILLOW_VERSION + " (wheels only)",
        [py, "-I", "-m", "pip", "install", "--disable-pip-version-check",
         "--only-binary=:all:", "pillow==" + PILLOW_VERSION],
    )
    pillow_version = run(
        "check Pillow version",
        [py, "-I", "-c", "import PIL; print(PIL.__version__)"],
    ).stdout.strip()
    if pillow_version != PILLOW_VERSION:
        fail("installed Pillow is %s, expected %s" % (pillow_version, PILLOW_VERSION))
    license_source = pillow_license(py)

    freeze = [py, "-I", "-m", "PyInstaller", "--noconfirm", "--clean", "--onedir",
              "--name", "gdx_companion_worker",
              "--distpath", FREEZE_TMP,
              "--workpath", PYI_WORK,
              "--specpath", PYI_WORK,
              "--log-level", "WARN",
              "--paths", WORKER_SOURCE.parent,
              "--paths", TOOLS]
    for module in HIDDEN_IMPORTS:
        freeze += ["--hidden-import", module]
    for data in DATA_FILES:
        freeze += ["--add-data", "%s%s." % (data, os.pathsep)]
    run("freeze worker (one-folder)", freeze + [WORKER_SOURCE])
    frozen = FREEZE_TMP / "gdx_companion_worker"
    if not frozen.is_dir():
        fail("PyInstaller did not produce " + str(frozen))
    shutil.move(str(frozen), str(OUTPUT))
    shutil.rmtree(FREEZE_TMP, ignore_errors=True)
    license_dir = OUTPUT / "_internal" / "LICENSES"
    license_dir.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(str(license_source), str(license_dir / PILLOW_LICENSE_NAME))
    python_license = Path(sys.base_prefix) / "LICENSE.txt"
    if not python_license.is_file():
        fail("the build interpreter has no LICENSE.txt at " + str(python_license))
    shutil.copyfile(str(python_license), str(license_dir / PYTHON_LICENSE_NAME))
    if (license_dir / PYTHON_LICENSE_NAME).read_bytes() != python_license.read_bytes():
        fail("Python's license text was not copied to " + str(license_dir))
    print("build_companion_worker: frozen worker at " + str(OUTPUT))

    smoke_test_frozen_worker(license_source)
    print("build_companion_worker: OK")


if __name__ == "__main__":
    main()
