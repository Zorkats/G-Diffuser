# Copyright (c) Zorkats
"""Companion jobs: check, plan, build, init, names, inspect and unpack, through the code the CLI runs.

Each packer's own main() runs in this process with an argument vector built from typed options, so
a pack built here is the pack `python tools/gen_<name>_pack.py ...` builds from the same inputs.
Packer output becomes bounded log events. Nothing here starts a shell or another process.

Build writes into a fresh staging folder the Companion names, checks the archive it produced, then
publishes it over the destination. A replaced pack is kept as <destination>.previous, so the last
good pack survives a failed or cancelled build and can be restored. Init writes a new project folder
into the same kind of staging folder and renames it to the destination, which must not exist yet.
The Companion cancels a job by ending this worker; it then deletes the staging folder it named.

Supported packers: htc (GLideN64 cache to HD pack), sample (WAV music samples), sequence (.seq
files), texture (replacement textures; it needs Pillow, which the frozen worker bundles), model
(machine models from a work folder) and soundfont (instrument banks from a font project). Model and
soundfont take subcommands (init, pack, check) and can create a project with init.

Inspect and unpack read someone else's pack (.o2r, or a .gdxp track package) through
tools/unpack_pack.py, the same code as `python tools/unpack_pack.py`. Their packer is "unpack". Inspect
writes nothing. Unpack writes a new folder through the same staging and rename as init.
"""

import contextlib
import hashlib
import io
import json
import os
import shutil
import sys
import zipfile

STAGING_PREFIX = ".gdx-companion-staging-"
PREVIOUS_SUFFIX = ".previous"
MAX_JOB_ID = 64
MAX_LOG_LINE_BYTES = 4 * 1024
MAX_JOB_LOG_BYTES = 256 * 1024
MAX_TEXT_OPTION = 256
MAX_BASES = 8
MAX_NAMES = 256
MAX_RESULT_BYTES = 60 * 1024  # a result event stays under the 64 KiB protocol line


class JobRefused(Exception):
    """A job the worker will not run. code is a stable machine-readable reason."""

    def __init__(self, code, message):
        super().__init__(message)
        self.code = code
        self.message = message


def _load_htc():
    import gen_htc_pack
    return gen_htc_pack


def _load_sample():
    import gen_sample_pack
    return gen_sample_pack


def _load_sequence():
    import gen_sequence_pack
    return gen_sequence_pack


def _load_texture():
    # Importing it needs Pillow; a missing Pillow surfaces as packer_unavailable for this job only.
    import gen_texture_pack
    return gen_texture_pack


def _load_model():
    import gen_model_pack
    return gen_model_pack


def _load_soundfont():
    import gen_soundfont_pack
    return gen_soundfont_pack


def _load_unpack():
    # Standard library only; Pillow, when present, reads the dump's palette swatches.
    import unpack_pack
    return unpack_pack


def _model_names(module):
    return sorted(module.MACHINES)


def _soundfont_names(module):
    return list(module.FONT_NAMES)


# Option name -> kind. "text" is a short string, "path" an existing file or folder, "flag" a bool,
# "paths" a list of existing files.
_METADATA = {"name": "text", "author": "text", "version": "text", "id": "text", "game_version": "text",
             "manifest": "path"}

# style "flat":       check: <input> --check [options]          build: <input> <output> [options]
# style "subcommand": check: check <input> [check options]      build: pack <input> -o <output> [options]
#                     init:  init <choice> -o <folder> --dump-dir <catalog> [init options]
# "check_options" lists the options a subcommand check accepts; a flat check takes them all.
PACKERS = {
    "htc": {
        "load": _load_htc,
        "script": "gen_htc_pack.py",
        "style": "flat",
        "input": "file",
        "options": {"name": "text", "author": "text", "version": "text", "base": "paths", "alt": "flag",
                    "direct_keys": "flag"},
        "required": ("name", "author", "base"),
        "check_pack": False,
    },
    "sample": {
        "load": _load_sample,
        "script": "gen_sample_pack.py",
        "style": "flat",
        "input": "dir",
        "options": dict(_METADATA),
        "catalog": "--dump-dir",
        "required": (),
        "check_pack": True,
    },
    "sequence": {
        "load": _load_sequence,
        "script": "gen_sequence_pack.py",
        "style": "flat",
        "input": "dir",
        "options": dict(_METADATA),
        "required": (),
        "check_pack": True,
    },
    "texture": {
        "load": _load_texture,
        "script": "gen_texture_pack.py",
        "style": "flat",
        "input": "dir",
        "options": dict(_METADATA, key_scheme_version="text", native_manifest="path",
                        direct_keys="flag"),
        "required": (),
        "check_pack": True,
    },
    "model": {
        "load": _load_model,
        "script": "gen_model_pack.py",
        "style": "subcommand",
        "input": "dir",
        "options": {"name": "text", "author": "text", "version": "text", "id": "text", "game_version": "text",
                    "allow_overbudget": "flag"},
        "check_options": ("allow_overbudget",),
        "required": (),
        "check_pack": True,
        "init": {"choice": "machine", "options": {}, "names": _model_names},
    },
    "soundfont": {
        "load": _load_soundfont,
        "script": "gen_soundfont_pack.py",
        "style": "subcommand",
        "input": "dir",
        "options": dict(_METADATA),
        "check_options": (),
        "output_flag": "-o",
        "required": (),
        "check_pack": True,
        "init": {"choice": "font", "options": {"skip_samples": "flag"}, "names": _soundfont_names},
    },
}

# The reader behind inspect and unpack. It is not a packer: it takes no options and builds no pack.
READERS = {"unpack": {"load": _load_unpack}}
READER_OPERATIONS = ("inspect", "unpack")

_FLAG_NAMES = {"game_version": "--game-version", "direct_keys": "--direct-keys",
               "key_scheme_version": "--key-scheme-version", "native_manifest": "--native-manifest",
               "allow_overbudget": "--allow-overbudget", "skip_samples": "--skip-samples"}

_REQUEST_FIELDS = {"v", "op", "job", "packer", "input", "options", "catalog", "output", "staging", "overwrite"}


def _flag(name):
    return _FLAG_NAMES.get(name, "--" + name)


def _job_id(request):
    job = request.get("job")
    if not isinstance(job, str) or not 1 <= len(job) <= MAX_JOB_ID or \
            not all(c.isascii() and (c.isalnum() or c in "-_") for c in job):
        raise JobRefused("bad_job", "job must be 1 to 64 letters, digits, '-' or '_'")
    return job


def _text(request, key, required=True):
    value = request.get(key)
    if value is None and not required:
        return None
    if not isinstance(value, str) or not value or len(value) > 4096 or "\x00" in value:
        raise JobRefused("bad_request", "'" + key + "' must be a non-empty path")
    return value


def _absolute(path, key, code="bad_request"):
    if not os.path.isabs(path):
        raise JobRefused(code, "'" + key + "' must be an absolute path")
    return os.path.normpath(path)


def _short_text(key, value):
    if not isinstance(value, str) or not value or len(value) > MAX_TEXT_OPTION or \
            any(ord(c) < 32 for c in value) or value.startswith("-"):
        raise JobRefused("bad_options", "option '" + key + "' must be 1 to 256 printable characters, "
                         "not starting with '-'")
    return value


def _option_args(packer_name, kinds, options):
    """Validates options against kinds. Returns {key: argv fragment}."""
    if not isinstance(options, dict):
        raise JobRefused("bad_options", "options must be an object")
    fragments = {}
    for key in sorted(options):
        kind = kinds.get(key)
        value = options[key]
        if kind is None:
            raise JobRefused("bad_options", "unknown option '" + key[:64] + "' for " + packer_name)
        if kind == "flag":
            if not isinstance(value, bool):
                raise JobRefused("bad_options", "option '" + key + "' must be true or false")
            fragments[key] = [_flag(key)] if value else []
        elif kind == "text":
            fragments[key] = [_flag(key), _short_text(key, value)]
        elif kind == "path":
            path = _absolute(value, key, "bad_options") if isinstance(value, str) and value else None
            if path is None or not os.path.isfile(path):
                raise JobRefused("bad_options", "option '" + key + "' must name an existing file")
            fragments[key] = [_flag(key), path]
        elif kind == "paths":
            if not isinstance(value, list) or not 1 <= len(value) <= MAX_BASES:
                raise JobRefused("bad_options", "option '" + key + "' must list 1 to %d files" % MAX_BASES)
            fragment = []
            for item in value:
                path = _absolute(item, key, "bad_options") if isinstance(item, str) and item else None
                if path is None or not os.path.isfile(path):
                    raise JobRefused("bad_options", "option '" + key + "' must list existing files")
                fragment += [_flag(key), path]
            fragments[key] = fragment
    return fragments


def _staging(request, beside):
    staging = _absolute(_text(request, "staging"), "staging")
    if os.path.dirname(staging) != os.path.dirname(beside) or \
            not os.path.basename(staging).startswith(STAGING_PREFIX):
        raise JobRefused("bad_request", "staging must be a " + STAGING_PREFIX + "* folder beside the output")
    if os.path.lexists(staging):
        raise JobRefused("staging_exists", "staging folder already exists: " + staging)
    return staging


def _validate_init(request, job, packer_name, spec):
    init = spec.get("init")
    if init is None:
        raise JobRefused("bad_request", packer_name + " cannot create a project")
    for key in ("input", "overwrite"):
        if key in request:
            raise JobRefused("bad_request", "init takes no '" + key + "'")
    dest = _absolute(_text(request, "output"), "output")
    if not os.path.isdir(os.path.dirname(dest)):
        raise JobRefused("missing_output_folder", "output folder not found: " + os.path.dirname(dest))
    if os.path.lexists(dest):
        raise JobRefused("exists", "the project folder already exists: " + dest)
    catalog = _absolute(_text(request, "catalog"), "catalog")
    if not os.path.isdir(catalog):
        raise JobRefused("missing_input", "catalog folder not found: " + catalog)
    options = request.get("options", {})
    if not isinstance(options, dict):
        raise JobRefused("bad_options", "options must be an object")
    choice_key = init["choice"]
    if choice_key not in options:
        raise JobRefused("bad_options", packer_name + " init needs option '" + choice_key + "'")
    rest = {k: v for k, v in options.items() if k != choice_key}
    fragments = _option_args(packer_name, init["options"], rest)
    args = [_short_text(choice_key, options[choice_key])]
    return {"job": job, "op": "init", "packer": packer_name, "output": dest, "catalog": catalog,
            "init_args": args, "init_extra": [a for k in sorted(fragments) for a in fragments[k]],
            "staging": _staging(request, dest)}


def _validate_reader(request, job, op):
    if request.get("packer") not in READERS:
        raise JobRefused("unsupported_packer", op + " takes packer 'unpack'")
    allowed = {"v", "op", "job", "packer", "input", "catalog"}
    if op == "unpack":
        allowed |= {"output", "staging"}
    extra = sorted(set(request) - allowed)
    if extra:
        raise JobRefused("bad_request", op + " takes no '" + extra[0][:64] + "'")
    source = _absolute(_text(request, "input"), "input")
    if not os.path.isfile(source):
        raise JobRefused("missing_input", "pack not found: " + source)
    catalog = None
    if "catalog" in request:
        catalog = _absolute(_text(request, "catalog"), "catalog")
        if not os.path.isdir(catalog):
            raise JobRefused("missing_input", "dump folder not found: " + catalog)
    result = {"job": job, "op": op, "packer": "unpack", "input": source, "catalog": catalog}
    if op == "unpack":
        dest = _absolute(_text(request, "output"), "output")
        if not os.path.isdir(os.path.dirname(dest)):
            raise JobRefused("missing_output_folder", "output folder not found: " + os.path.dirname(dest))
        if os.path.lexists(dest):
            raise JobRefused("exists", "the unpack folder already exists: " + dest)
        result.update({"output": dest, "staging": _staging(request, dest)})
    return result


def validate(request, op):
    """Returns a job dict for one job operation, or raises JobRefused. Touches nothing."""
    job = _job_id(request)
    if op in READER_OPERATIONS:
        return _validate_reader(request, job, op)
    packer_name = request.get("packer")
    if packer_name in READERS:
        raise JobRefused("unsupported_packer", "packer 'unpack' only serves inspect and unpack")
    if packer_name not in PACKERS:
        raise JobRefused("unsupported_packer", "packer must be one of: " + ", ".join(sorted(PACKERS)))
    spec = PACKERS[packer_name]
    unknown = sorted(set(request) - _REQUEST_FIELDS)
    if unknown:
        raise JobRefused("bad_request", "unknown field '" + unknown[0][:64] + "'")

    if op == "names":
        if spec.get("init") is None:
            raise JobRefused("bad_request", packer_name + " has no project choices")
        extra = sorted(set(request) - {"v", "op", "job", "packer"})
        if extra:
            raise JobRefused("bad_request", "names takes no '" + extra[0][:64] + "'")
        return {"job": job, "op": "names", "packer": packer_name}
    if op == "init":
        return _validate_init(request, job, packer_name, spec)

    source = _absolute(_text(request, "input"), "input")
    if spec["input"] == "file" and not os.path.isfile(source):
        raise JobRefused("missing_input", "input file not found: " + source)
    if spec["input"] == "dir" and not os.path.isdir(source) and not (op == "check" and os.path.isfile(source)):
        raise JobRefused("missing_input", "input folder not found: " + source)

    catalog = None
    if "catalog" in spec:
        catalog = _absolute(_text(request, "catalog"), "catalog")
        if not os.path.isdir(catalog):
            raise JobRefused("missing_input", "catalog folder not found: " + catalog)
    elif "catalog" in request:
        raise JobRefused("bad_request", packer_name + " takes no catalog")

    options = request.get("options", {})
    fragments = _option_args(packer_name, spec["options"], options)
    for key in spec["required"]:
        if key not in options:
            raise JobRefused("bad_options", packer_name + " needs option '" + key + "'")
    if options.get("alt") and options.get("direct_keys"):
        raise JobRefused("bad_options", "alt and direct_keys cannot both be on")
    args = [a for key in sorted(fragments) for a in fragments[key]]
    if "check_options" in spec:
        check_args = [a for key in sorted(fragments) if key in spec["check_options"] for a in fragments[key]]
    else:
        check_args = list(args)
    if catalog is not None:
        args += [spec["catalog"], catalog]
        check_args += [spec["catalog"], catalog]

    result = {"job": job, "op": op, "packer": packer_name, "input": source, "args": args,
              "check_args": check_args}
    if op in ("plan", "build"):
        dest = _absolute(_text(request, "output"), "output")
        if not dest.lower().endswith(".o2r"):
            raise JobRefused("bad_request", "output must be an .o2r file")
        if not os.path.isdir(os.path.dirname(dest)):
            raise JobRefused("missing_output_folder", "output folder not found: " + os.path.dirname(dest))
        if os.path.isdir(dest):
            raise JobRefused("bad_request", "output is a folder: " + dest)
        overwrite = request.get("overwrite", False)
        if not isinstance(overwrite, bool):
            raise JobRefused("bad_request", "overwrite must be true or false")
        result.update({"output": dest, "overwrite": overwrite})
    if op == "build":
        result["staging"] = _staging(request, result["output"])
    elif "staging" in request:
        raise JobRefused("bad_request", "only build and init take a staging folder")
    return result


def check_argv(job, target):
    """The packer arguments that check target (a source folder or a built pack)."""
    spec = PACKERS[job["packer"]]
    if spec["style"] == "subcommand":
        return ["check", target] + job["check_args"]
    return [target] + job["check_args"] + ["--check"]


def build_argv(job, staged):
    spec = PACKERS[job["packer"]]
    if spec["style"] == "subcommand":
        return ["pack", job["input"], spec.get("output_flag", "-o"), staged] + job["args"]
    return [job["input"], staged] + job["args"]


class JobLog:
    """Turns packer text into bounded log events for one job."""

    def __init__(self, emit, job, stream):
        self._emit = emit
        self._job = job
        self._stream = stream
        self._pending = ""
        self.budget = None  # shared [bytes left] list, set by the runner

    def write(self, text):
        if not isinstance(text, str):
            text = str(text)
        self._pending += text
        while "\n" in self._pending:
            line, self._pending = self._pending.split("\n", 1)
            self._line(line)
        if len(self._pending) > MAX_LOG_LINE_BYTES:
            self._line(self._pending)
            self._pending = ""
        return len(text)

    def flush(self):
        pass

    def finish(self):
        if self._pending:
            self._line(self._pending)
            self._pending = ""

    def _line(self, line):
        line = line.rstrip("\r")
        data = line.encode("utf-8", "replace")
        if len(data) > MAX_LOG_LINE_BYTES:
            line = data[:MAX_LOG_LINE_BYTES].decode("utf-8", "ignore") + " [cut]"
        left = self.budget[0]
        if left <= 0:
            return
        cost = len(line.encode("utf-8", "replace"))
        if cost > left:
            self._emit({"event": "log", "job": self._job, "stream": "err",
                        "text": "Further output from this job was dropped (256 KiB kept)."})
            self.budget[0] = 0
            return
        self.budget[0] = left - cost
        self._emit({"event": "log", "job": self._job, "stream": self._stream, "text": line})


def run_packer(job, extra_args, emit):
    """Runs the packer's main() with the job's arguments. Returns its exit status."""
    spec = PACKERS[job["packer"]]
    module = spec["load"]()
    out = JobLog(emit, job["job"], "out")
    err = JobLog(emit, job["job"], "err")
    out.budget = err.budget = [MAX_JOB_LOG_BYTES]
    argv = [spec["script"]] + extra_args
    saved_argv = sys.argv
    sys.argv = argv
    try:
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            try:
                status = module.main()
            except SystemExit as stop:
                code = stop.code
                status = code if isinstance(code, int) else (0 if code is None else 1)
            except Exception as exc:  # a packer bug must not take the worker down
                err.write("packer failed: %s: %s\n" % (type(exc).__name__, exc))
                status = 1
    finally:
        sys.argv = saved_argv
        out.finish()
        err.finish()
    return 0 if status is None else int(status)


def _sha256(path):
    digest = hashlib.sha256()
    with open(path, "rb") as fh:
        for block in iter(lambda: fh.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def _archive_problem(path):
    """Returns why the built archive is unusable, or None."""
    try:
        with zipfile.ZipFile(path) as archive:
            bad = archive.testzip()
            if bad is not None:
                return "entry " + bad + " is damaged"
            names = archive.namelist()
            if "workshop.json" not in names:
                return "the pack has no workshop.json"
            if len(names) < 2:
                return "the pack holds no replacements"
            meta = json.loads(archive.read("workshop.json").decode("utf-8"))
            if not isinstance(meta, dict):
                return "workshop.json is not an object"
    except (OSError, zipfile.BadZipFile, ValueError, UnicodeDecodeError) as exc:
        return "the pack cannot be read: " + str(exc)
    return None


def run_check(job, emit):
    status = run_packer(job, check_argv(job, job["input"]), emit)
    return {"event": "result", "job": job["job"], "op": "check", "status": "ok" if status == 0 else "problems",
            "exit": status}


def run_plan(job):
    dest = job["output"]
    exists = os.path.isfile(dest)
    return {"event": "result", "job": job["job"], "op": "plan", "status": "ok", "output": dest,
            "exists": exists, "needs_overwrite": exists and not job["overwrite"],
            "previous": dest + PREVIOUS_SUFFIX if exists else None}


def run_names(job):
    spec = PACKERS[job["packer"]]
    names = [str(n) for n in spec["init"]["names"](spec["load"]())][:MAX_NAMES]
    return {"event": "result", "job": job["job"], "op": "names", "status": "ok", "names": names}


def _fit(event):
    """Drops listed entries from the end until the event fits one protocol line. total stays exact."""
    entries = event.get("entries", [])
    while entries and len(json.dumps(event, separators=(",", ":"))) > MAX_RESULT_BYTES:
        del entries[len(entries) * 7 // 8:]
    return event


def _reader_result(job, op, listing):
    event = {"event": "result", "job": job["job"], "op": op, "status": "ok"}
    event.update(listing)
    return _fit(event)


def run_inspect(job):
    reader = READERS[job["packer"]]["load"]()
    try:
        listing = reader.listing_result(reader.scan(job["input"], job["catalog"]))
    except reader.UnpackError as exc:
        return job_error(job["job"], "unreadable", str(exc))
    except OSError as exc:
        return job_error(job["job"], "io", "cannot read the pack: " + str(exc))
    except Exception as exc:  # a reader bug must not take the worker down
        return job_error(job["job"], "reader_failed", "reading failed: %s: %s" % (type(exc).__name__, exc))
    return _reader_result(job, "inspect", listing)


def run_unpack(job, emit):
    reader = READERS[job["packer"]]["load"]()
    dest = job["output"]
    staging = job["staging"]
    try:
        os.mkdir(staging)
    except OSError as exc:
        return job_error(job["job"], "io", "cannot make the staging folder: " + str(exc))
    log = JobLog(emit, job["job"], "out")
    log.budget = [MAX_JOB_LOG_BYTES]
    try:
        staged = os.path.join(staging, os.path.basename(dest))
        try:
            os.mkdir(staged)
            listing = reader.unpack_into(job["input"], staged, job["catalog"], lambda line: log.write(line + "\n"))
        except reader.UnpackError as exc:
            return job_error(job["job"], "unreadable", str(exc))
        except OSError as exc:
            return job_error(job["job"], "io", "cannot write the unpacked files: " + str(exc))
        except Exception as exc:  # a reader bug must not take the worker down
            return job_error(job["job"], "reader_failed", "unpacking failed: %s: %s" % (type(exc).__name__, exc))
        finally:
            log.finish()
        if os.path.lexists(dest):
            return job_error(job["job"], "exists", "the unpack folder appeared while it was written: " + dest)
        try:
            os.rename(staged, dest)
        except OSError as exc:
            return job_error(job["job"], "io", "cannot move the unpacked folder into place: " + str(exc))
        listing["folder"] = dest
        log.write("unpacked %d of %d entries into %s\n" % (listing["written"], listing["total"], dest))
        return _reader_result(job, "unpack", listing)
    finally:
        shutil.rmtree(staging, ignore_errors=True)


def _publish(staged, dest):
    """Moves staged over dest, keeping the old dest as dest.previous. Returns the previous path or None."""
    previous = None
    if os.path.lexists(dest):
        previous = dest + PREVIOUS_SUFFIX
        os.replace(dest, previous)
    try:
        os.replace(staged, dest)
    except OSError:
        if previous is not None:
            os.replace(previous, dest)
        raise
    return previous


def run_build(job, emit):
    dest = job["output"]
    staging = job["staging"]
    if os.path.lexists(dest) and not job["overwrite"]:
        return job_error(job["job"], "exists", "the output already exists; build again with overwrite")
    try:
        os.mkdir(staging)
    except OSError as exc:
        return job_error(job["job"], "io", "cannot make the staging folder: " + str(exc))
    try:
        staged = os.path.join(staging, os.path.basename(dest))
        status = run_packer(job, build_argv(job, staged), emit)
        if status != 0:
            return job_error(job["job"], "packer_failed", "the packer stopped with status %d" % status, status)
        if not os.path.isfile(staged) or os.path.getsize(staged) == 0:
            return job_error(job["job"], "empty_output", "the packer finished but wrote no pack")
        problem = _archive_problem(staged)
        if problem is None and PACKERS[job["packer"]]["check_pack"]:
            checked = run_packer(job, _pack_check_argv(job, staged), emit)
            if checked != 0:
                problem = "the packer's own check refused the new pack"
        if problem is not None:
            return job_error(job["job"], "invalid_artifact", problem)
        size = os.path.getsize(staged)
        digest = _sha256(staged)
        try:
            previous = _publish(staged, dest)
        except OSError as exc:
            return job_error(job["job"], "io", "cannot replace the output: " + str(exc))
        return {"event": "result", "job": job["job"], "op": "build", "status": "ok",
                "artifacts": [{"path": dest, "bytes": size, "sha256": digest}],
                "replaced": previous is not None, "previous": previous}
    finally:
        shutil.rmtree(staging, ignore_errors=True)


def run_init(job, emit):
    dest = job["output"]
    staging = job["staging"]
    try:
        os.mkdir(staging)
    except OSError as exc:
        return job_error(job["job"], "io", "cannot make the staging folder: " + str(exc))
    try:
        staged = os.path.join(staging, os.path.basename(dest))
        argv = ["init"] + job["init_args"] + ["-o", staged, "--dump-dir", job["catalog"]] + job["init_extra"]
        status = run_packer(job, argv, emit)
        if status != 0:
            return job_error(job["job"], "packer_failed", "the packer stopped with status %d" % status, status)
        if not os.path.isdir(staged) or not os.listdir(staged):
            return job_error(job["job"], "empty_output", "the packer finished but wrote no project")
        if os.path.lexists(dest):
            return job_error(job["job"], "exists", "the project folder appeared while it was made: " + dest)
        try:
            os.rename(staged, dest)
        except OSError as exc:
            return job_error(job["job"], "io", "cannot move the project into place: " + str(exc))
        return {"event": "result", "job": job["job"], "op": "init", "status": "ok", "project": dest}
    finally:
        shutil.rmtree(staging, ignore_errors=True)


def _pack_check_argv(job, staged):
    """Checks a built pack. A flat packer keeps its catalog, so a sample pack is checked like the CLI does."""
    spec = PACKERS[job["packer"]]
    if spec["style"] == "subcommand":
        return ["check", staged] + job["check_args"]
    return [staged, "--check"] + _catalog_args(job)


def _catalog_args(job):
    args = job["args"]
    if "--dump-dir" in args:
        i = args.index("--dump-dir")
        return [args[i], args[i + 1]]
    return []


def job_error(job, code, message, status=None):
    event = {"event": "job_error", "job": job, "code": code, "message": message}
    if status is not None:
        event["exit"] = status
    return event


def run(request, op, emit):
    """Runs one job request and returns its final event (result or job_error)."""
    try:
        job = validate(request, op)
    except JobRefused as refusal:
        try:
            job = _job_id(request)
        except JobRefused:
            job = ""
        return job_error(job, refusal.code, refusal.message)
    try:
        # Loaded before any staging folder exists, so a missing or broken packer only refuses this job.
        (READERS if op in READER_OPERATIONS else PACKERS)[job["packer"]]["load"]()
    except Exception as exc:  # an import failure must not take the worker down
        return job_error(job["job"], "packer_unavailable",
                         "the %s packer could not load: %s: %s" % (job["packer"], type(exc).__name__, exc))
    if op == "inspect":
        return run_inspect(job)
    if op == "unpack":
        return run_unpack(job, emit)
    if op == "check":
        return run_check(job, emit)
    if op == "plan":
        return run_plan(job)
    if op == "names":
        return run_names(job)
    if op == "init":
        return run_init(job, emit)
    return run_build(job, emit)
