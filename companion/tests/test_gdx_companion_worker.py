# Copyright (c) Zorkats
"""Tests for the Companion packer worker, protocol 1.

Run with: python -I -m unittest discover -s companion/tests -v
"""

import json
import os
import subprocess
import sys
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
WORKER = os.path.normpath(os.path.join(HERE, "..", "worker", "gdx_companion_worker.py"))
MAX_LINE = 64 * 1024


def run_worker(payload, timeout=20):
    """Runs the worker with payload on stdin. Returns (exit code, decoded event dicts, stderr text)."""
    result = subprocess.run(
        [sys.executable, "-I", WORKER],
        input=payload,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        timeout=timeout,
    )
    events = []
    for raw in result.stdout.split(b"\n"):
        if raw:
            events.append(json.loads(raw.decode("ascii")))
    return result.returncode, events, result.stderr.decode("utf-8", "replace")


class HelloAndShutdown(unittest.TestCase):
    def test_hello_reports_version_protocol_and_ops(self):
        code, events, _ = run_worker(b'{"v":1,"op":"hello"}\n{"v":1,"op":"shutdown"}\n')
        self.assertEqual(code, 0)
        self.assertEqual(
            events[0],
            {
                "v": 1,
                "event": "hello",
                "worker_version": "0.4.0",
                "protocol": 1,
                "ops": ["hello", "shutdown", "check", "plan", "build", "init", "names", "inspect", "unpack"],
            },
        )

    def test_shutdown_is_acknowledged_and_exits_zero(self):
        code, events, _ = run_worker(b'{"v":1,"op":"shutdown"}\n')
        self.assertEqual(code, 0)
        self.assertEqual(events, [{"v": 1, "event": "shutdown"}])

    def test_end_of_input_exits_zero_without_output(self):
        code, events, _ = run_worker(b"")
        self.assertEqual(code, 0)
        self.assertEqual(events, [])

    def test_hello_after_shutdown_is_not_served(self):
        code, events, _ = run_worker(b'{"v":1,"op":"shutdown"}\n{"v":1,"op":"hello"}\n')
        self.assertEqual(code, 0)
        self.assertEqual(events, [{"v": 1, "event": "shutdown"}])

    def test_crlf_line_endings_are_accepted(self):
        code, events, _ = run_worker(b'{"v":1,"op":"hello"}\r\n{"v":1,"op":"shutdown"}\r\n')
        self.assertEqual(code, 0)
        self.assertEqual(events[0]["event"], "hello")
        self.assertEqual(events[1], {"v": 1, "event": "shutdown"})

    def test_final_line_without_newline_is_served(self):
        code, events, _ = run_worker(b'{"v":1,"op":"hello"}')
        self.assertEqual(code, 0)
        self.assertEqual(events[0]["event"], "hello")


class RefusedRequests(unittest.TestCase):
    def assert_refused(self, line, code):
        _, events, stderr = run_worker(line + b'{"v":1,"op":"hello"}\n')
        self.assertEqual(events[0]["event"], "error", events)
        self.assertEqual(events[0]["code"], code)
        self.assertEqual(events[0]["v"], 1)
        self.assertEqual(events[1]["event"], "hello", "worker must keep serving after a refusal")
        self.assertIn("refused", stderr)

    def test_malformed_json_is_refused(self):
        self.assert_refused(b"{not json}\n", "malformed")

    def test_non_finite_number_is_refused(self):
        self.assert_refused(b'{"v":1,"op":"hello","x":NaN}\n', "malformed")
        self.assert_refused(b'{"v":1,"op":"hello","x":Infinity}\n', "malformed")

    def test_duplicate_key_is_refused(self):
        self.assert_refused(b'{"v":1,"v":1,"op":"hello"}\n', "malformed")

    def test_top_level_array_is_refused(self):
        self.assert_refused(b"[1,2]\n", "malformed")

    def test_invalid_utf8_is_refused(self):
        self.assert_refused(b'{"v":1,"op":"\xff"}\n', "malformed")

    def test_missing_version_is_refused(self):
        self.assert_refused(b'{"op":"hello"}\n', "malformed")

    def test_boolean_version_is_refused(self):
        self.assert_refused(b'{"v":true,"op":"hello"}\n', "malformed")

    def test_missing_op_is_refused(self):
        self.assert_refused(b'{"v":1}\n', "malformed")

    def test_version_mismatch_is_reported(self):
        self.assert_refused(b'{"v":2,"op":"hello"}\n', "version_mismatch")

    def test_deep_nesting_is_refused_without_crashing(self):
        self.assert_refused(b"[" * 5000 + b"]" * 5000 + b"\n", "malformed")

    def test_unknown_operation_is_reported_with_its_name(self):
        code, events, _ = run_worker(b'{"v":1,"op":"explode"}\n{"v":1,"op":"shutdown"}\n')
        self.assertEqual(code, 0)
        self.assertEqual(events[0]["code"], "unknown_op")
        self.assertIn("explode", events[0]["message"])

    def test_long_unknown_operation_name_is_truncated_in_the_reply(self):
        op = "x" * 5000
        code, events, _ = run_worker(('{"v":1,"op":"%s"}\n' % op).encode("ascii") + b'{"v":1,"op":"shutdown"}\n')
        self.assertEqual(code, 0)
        self.assertEqual(events[0]["code"], "unknown_op")
        self.assertLess(len(events[0]["message"]), 200)


class LineLimit(unittest.TestCase):
    def padded_hello(self, total_content_bytes):
        # Content is the JSON object without its LF. Pad with a string value to the exact size.
        prefix = b'{"v":1,"op":"hello","pad":"'
        suffix = b'"}'
        pad = total_content_bytes - len(prefix) - len(suffix)
        return prefix + b"a" * pad + suffix

    def test_line_of_exactly_the_limit_is_served(self):
        line = self.padded_hello(MAX_LINE)
        self.assertEqual(len(line), MAX_LINE)
        code, events, _ = run_worker(line + b"\n" + b'{"v":1,"op":"shutdown"}\n')
        self.assertEqual(code, 0)
        self.assertEqual(events[0]["event"], "hello")

    def test_line_of_the_limit_plus_crlf_is_served(self):
        line = self.padded_hello(MAX_LINE)
        code, events, _ = run_worker(line + b"\r\n" + b'{"v":1,"op":"shutdown"}\n')
        self.assertEqual(code, 0)
        self.assertEqual(events[0]["event"], "hello")

    def test_one_byte_over_the_limit_is_refused_and_next_line_is_served(self):
        line = self.padded_hello(MAX_LINE + 1)
        code, events, stderr = run_worker(line + b"\n" + b'{"v":1,"op":"hello"}\n')
        self.assertEqual(code, 0)
        self.assertEqual(events[0], {"v": 1, "event": "error", "code": "line_too_long",
                                     "message": "request longer than 65536 bytes was discarded"})
        self.assertEqual(events[1]["event"], "hello")

    def test_far_over_the_limit_is_discarded_to_the_next_lf(self):
        line = self.padded_hello(MAX_LINE * 3)
        code, events, _ = run_worker(line + b"\n" + b'{"v":1,"op":"hello"}\n')
        self.assertEqual(code, 0)
        self.assertEqual(events[0]["code"], "line_too_long")
        self.assertEqual(len(events), 2)
        self.assertEqual(events[1]["event"], "hello")

    def test_oversized_line_at_end_of_input_is_refused_once(self):
        line = self.padded_hello(MAX_LINE * 2)
        code, events, _ = run_worker(line)
        self.assertEqual(code, 0)
        self.assertEqual(len(events), 1)
        self.assertEqual(events[0]["code"], "line_too_long")


if __name__ == "__main__":
    unittest.main()
