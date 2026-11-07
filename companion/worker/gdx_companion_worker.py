# Copyright (c) Zorkats
"""G-Diffuser Companion worker, protocol 1.

Reads JSON Lines requests on stdin and writes JSON Lines events on stdout. Diagnostics go to stderr
only. The protocol itself is standard library only; the freeze also bundles Pillow for textures.

Requests:  {"v":1,"op":"hello"}   {"v":1,"op":"shutdown"}
           {"v":1,"op":"check"|"plan"|"build"|"init"|"names","job":"j1","packer":"sequence","input":"C:/abs/in",
            "output":"C:/abs/out.o2r","staging":"C:/abs/.gdx-companion-staging-j1","overwrite":false,
            "options":{"name":"My Music"}}   (see gdx_companion_jobs.py for each packer's fields)
           {"v":1,"op":"inspect"|"unpack","job":"j2","packer":"unpack","input":"C:/abs/pack.o2r",
            "catalog":"C:/abs/dump","output":"C:/abs/pack-unpacked","staging":"C:/abs/.gdx-companion-staging-j2"}
Events:    {"v":1,"event":"hello","worker_version":"0.4.0","protocol":1,"ops":[...]}
           {"v":1,"event":"shutdown"}
           {"v":1,"event":"error","code":"malformed","message":"..."}
           {"v":1,"event":"log","job":"j1","stream":"out"|"err","text":"..."}
           {"v":1,"event":"result","job":"j1","op":"build","status":"ok",...}
           {"v":1,"event":"job_error","job":"j1","code":"packer_failed","message":"..."}

A job runs to its result before the next request is read, so there is one job at a time. The
Companion cancels a job by ending this process.

Limits match the Companion host: 64 KiB per line with the LF not counted, duplicate object keys and
non-finite numbers refused, and input must be UTF-8.
"""

import json
import os
import sys

if not getattr(sys, "frozen", False):
    # python -I keeps the script folder off sys.path. Both folders come from this file's own location.
    _HERE = os.path.dirname(os.path.abspath(__file__))
    for _folder in (_HERE, os.path.join(os.path.dirname(os.path.dirname(_HERE)), "tools")):
        if _folder not in sys.path:
            sys.path.insert(0, _folder)

import gdx_companion_jobs  # noqa: E402

PROTOCOL_VERSION = 1
WORKER_VERSION = "0.4.0"
MAX_LINE_BYTES = 64 * 1024
OPERATIONS = ("hello", "shutdown", "check", "plan", "build", "init", "names", "inspect", "unpack")
JOB_OPERATIONS = ("check", "plan", "build", "init", "names", "inspect", "unpack")


class RefusedLine(Exception):
    """A request that cannot be served. The worker answers with an error event and keeps running."""

    def __init__(self, code, message):
        super().__init__(message)
        self.code = code
        self.message = message


def _reject_constant(name):
    raise ValueError("non-finite number " + name + " is not allowed")


def _reject_duplicate_keys(pairs):
    obj = {}
    for key, value in pairs:
        if key in obj:
            raise ValueError("duplicate key " + repr(key))
        obj[key] = value
    return obj


def read_line(stream):
    """Returns the next line without its terminator, or None at end of input.

    Raises RefusedLine for a line longer than MAX_LINE_BYTES. Its remaining bytes are discarded up to
    the next LF, so the following request is still read correctly.
    """
    raw = stream.readline(MAX_LINE_BYTES + 2)  # room for CR and LF
    if raw == b"":
        return None
    if not raw.endswith(b"\n") and len(raw) >= MAX_LINE_BYTES + 2:
        while raw and not raw.endswith(b"\n"):
            raw = stream.readline(MAX_LINE_BYTES + 2)
        raise RefusedLine("line_too_long", "request longer than 65536 bytes was discarded")
    line = raw[:-1] if raw.endswith(b"\n") else raw
    if line.endswith(b"\r"):
        line = line[:-1]
    if len(line) > MAX_LINE_BYTES:
        raise RefusedLine("line_too_long", "request longer than 65536 bytes was discarded")
    return line


def parse_request(line):
    """Returns (operation name, request object) for a request line, or raises RefusedLine."""
    try:
        text = line.decode("utf-8")
    except UnicodeDecodeError:
        raise RefusedLine("malformed", "request is not valid UTF-8")
    try:
        request = json.loads(
            text,
            parse_constant=_reject_constant,
            object_pairs_hook=_reject_duplicate_keys,
        )
    except (ValueError, RecursionError) as err:
        raise RefusedLine("malformed", "request is not valid JSON: " + str(err))
    if not isinstance(request, dict):
        raise RefusedLine("malformed", "request is not a JSON object")
    version = request.get("v")
    if not isinstance(version, int) or isinstance(version, bool):
        raise RefusedLine("malformed", "request has no integer protocol version 'v'")
    if version != PROTOCOL_VERSION:
        raise RefusedLine(
            "version_mismatch",
            "worker speaks protocol %d, request uses protocol %d" % (PROTOCOL_VERSION, version),
        )
    op = request.get("op")
    if not isinstance(op, str):
        raise RefusedLine("malformed", "request has no string 'op'")
    return op, request


def write_event(stdout, event):
    text = json.dumps(event, separators=(",", ":"), ensure_ascii=True)
    stdout.write((text + "\n").encode("ascii"))
    stdout.flush()


def hello_event():
    return {
        "v": PROTOCOL_VERSION,
        "event": "hello",
        "worker_version": WORKER_VERSION,
        "protocol": PROTOCOL_VERSION,
        "ops": list(OPERATIONS),
    }


def error_event(code, message):
    return {"v": PROTOCOL_VERSION, "event": "error", "code": code, "message": message}


def serve(stdin, stdout):
    """Serves requests until shutdown or end of input. Returns the process exit status."""
    while True:
        try:
            line = read_line(stdin)
            if line is None:
                return 0
            op, request = parse_request(line)
        except RefusedLine as refusal:
            sys.stderr.write("worker: refused request: " + refusal.message + "\n")
            write_event(stdout, error_event(refusal.code, refusal.message))
            continue

        if op == "hello":
            write_event(stdout, hello_event())
        elif op == "shutdown":
            write_event(stdout, {"v": PROTOCOL_VERSION, "event": "shutdown"})
            return 0
        elif op in JOB_OPERATIONS:
            def emit(event):
                write_event(stdout, dict({"v": PROTOCOL_VERSION}, **event))
            emit(gdx_companion_jobs.run(request, op, emit))
        else:
            shown = op[:64]
            write_event(stdout, error_event("unknown_op", "unknown operation '" + shown + "'"))


def main():
    try:
        return serve(sys.stdin.buffer, sys.stdout.buffer)
    except BrokenPipeError:
        return 1


if __name__ == "__main__":
    sys.exit(main())
