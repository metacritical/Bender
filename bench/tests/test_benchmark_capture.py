"""Tests for tools/benchmark_capture.py.

Every child is a tiny synthetic Python process, so the capture contract is
exercised without building or running a real benchmark: byte-exact streaming,
exact status, exclusive evidence directories, environment layering and the two
independent resource reports.
"""

import hashlib
import json
import os
import shutil
import sys
import tempfile
import time
import unittest
from contextlib import redirect_stderr, redirect_stdout
from io import StringIO

# Importing the tool would otherwise leave ``tools/__pycache__`` in the tree;
# this package keeps test bytecode out of source, so suppress it here too.
sys.dont_write_bytecode = True

from tools import benchmark_capture as capture_tool


def py(source):
    """A child argv running ``source`` on this interpreter."""
    return [sys.executable, "-c", source]


class CaptureHarness(unittest.TestCase):
    """Shared fixture: a temp root and helpers to read a finished capture."""

    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="benchmark-capture-test-")
        self.addCleanup(shutil.rmtree, self.tmp, True)

    def evidence(self, name="evidence"):
        return os.path.join(self.tmp, name)

    def request(self, argv, cwd=None, **kwargs):
        return capture_tool.CaptureRequest(
            argv=tuple(argv), cwd=cwd or self.tmp, evidence_dir=self.evidence(), **kwargs
        )

    def run_capture(self, argv, cwd=None, **kwargs):
        return capture_tool.capture(self.request(argv, cwd, **kwargs))

    def read_bytes(self, name):
        with open(os.path.join(self.evidence(), name), "rb") as handle:
            return handle.read()

    def read_record(self):
        with open(os.path.join(self.evidence(), "capture.json"), encoding="utf-8") as handle:
            return json.load(handle)

    def assert_clean_exit(self, record, status):
        """The child exited on its own with exactly this status."""
        self.assertEqual(record["result"]["kind"], "exited")
        self.assertEqual(record["result"]["exit_status"], status)
        self.assertIsNone(record["result"]["signal"])
        self.assertEqual(capture_tool.cli_status(record), status)

    def assert_stream_matches_file(self, record, stream):
        """The recorded digest and size describe the bytes actually on disk."""
        payload = self.read_bytes(record["streams"][stream]["file"])
        self.assertEqual(record["streams"][stream]["bytes"], len(payload))
        self.assertEqual(
            record["streams"][stream]["sha256"],
            capture_tool.hash_and_count(
                os.path.join(self.evidence(), record["streams"][stream]["file"])
            )["sha256"],
        )
        return payload


class SuccessTests(CaptureHarness):
    def test_successful_command_records_exact_streams(self):
        record = self.run_capture(
            py("import sys; sys.stdout.write('out\\n'); sys.stderr.write('err\\n')")
        )
        self.assertTrue(record["complete"])
        self.assertEqual(self.assert_stream_matches_file(record, "stdout"), b"out\n")
        self.assertEqual(self.assert_stream_matches_file(record, "stderr"), b"err\n")
        self.assertEqual(record["result"]["kind"], "exited")
        self.assertEqual(record["result"]["exit_status"], 0)
        self.assertIsNone(record["result"]["signal"])

    def test_an_untouched_stream_records_empty_digests(self):
        record = self.run_capture(py("print('hash me')"))
        self.assert_stream_matches_file(record, "stdout")
        self.assertEqual(record["streams"]["stderr"]["bytes"], 0)
        self.assertEqual(
            record["streams"]["stderr"]["sha256"],
            capture_tool.hash_and_count(os.devnull)["sha256"],
        )

    def test_command_and_timing_are_recorded(self):
        record = self.run_capture(py("pass"), label="unit")
        self.assertEqual(record["schema"], capture_tool.SCHEMA)
        self.assertEqual(record["label"], "unit")
        self.assertEqual(record["command"]["argv"], py("pass"))
        self.assertEqual(record["command"]["cwd"], os.path.abspath(self.tmp))
        self.assertGreaterEqual(record["timing"]["wall_s"], 0.0)
        self.assertLess(
            record["timing"]["started_at"], record["timing"]["finished_at"]
        )
        self.assertTrue(record["command"]["executable"]["sha256"])


class FailureTests(CaptureHarness):
    def test_failing_command_keeps_its_exact_exit_status(self):
        record = self.run_capture(py("import sys; sys.exit(7)"))
        self.assertTrue(record["complete"])
        self.assert_clean_exit(record, 7)

    def test_signal_death_is_recorded_as_a_signal_not_an_exit(self):
        argv = py("import os, signal; os.kill(os.getpid(), signal.SIGTERM)")
        record = self.run_capture(argv)
        self.assertEqual(record["result"]["kind"], "signaled")
        self.assertEqual(record["result"]["signal_name"], "SIGTERM")
        self.assertIsNone(record["result"]["exit_status"])
        self.assertEqual(capture_tool.cli_status(record), 128 + 15)

    def test_unspawnable_command_is_a_capture_failure(self):
        argv = [os.path.join(self.tmp, "definitely-not-here")]
        record = self.run_capture(argv)
        self.assertTrue(record["complete"])
        self.assertIn("FileNotFoundError", record["result"]["spawn_error"])
        self.assertEqual(capture_tool.cli_status(record), 2)


class ByteExactTests(CaptureHarness):
    def test_invalid_utf8_stdout_is_preserved_and_hashed_raw(self):
        # Lone continuation bytes and a truncated multi-byte sequence: legal
        # bytes for a program, illegal as UTF-8 text.
        source = (
            "import sys; sys.stdout.buffer.write(b'ok:\\xff\\xfe\\x80 mid \\xe2\\x82 tail\\n')"
        )
        record = self.run_capture(py(source))
        payload = self.assert_stream_matches_file(record, "stdout")
        self.assertEqual(payload, b"ok:\xff\xfe\x80 mid \xe2\x82 tail\n")

    def test_large_streams_are_written_whole(self):
        record = self.run_capture(py("import sys; sys.stdout.write('x' * 300000)"))
        self.assertEqual(self.assert_stream_matches_file(record, "stdout"), b"x" * 300000)

    def test_stdout_and_stderr_never_interleave(self):
        source = (
            "import sys\n"
            "for i in range(50):\n"
            "    sys.stdout.write(f'o{i}\\n'); sys.stdout.flush()\n"
            "    sys.stderr.write(f'e{i}\\n'); sys.stderr.flush()\n"
        )
        self.run_capture(py(source))
        out = self.read_bytes("stdout.log").splitlines()
        err = self.read_bytes("stderr.log").splitlines()
        self.assertEqual(out, [f"o{i}".encode() for i in range(50)])
        self.assertEqual(err, [f"e{i}".encode() for i in range(50)])


class OverwriteTests(CaptureHarness):
    def test_existing_evidence_directory_is_rejected_before_the_child_runs(self):
        marker = os.path.join(self.tmp, "child-ran")
        source = f"open({marker!r}, 'w').close()"
        os.makedirs(self.evidence())
        with self.assertRaises(capture_tool.CaptureError):
            self.run_capture(py(source))
        self.assertFalse(os.path.exists(marker), "the child must not run")
        self.assertFalse(os.path.exists(os.path.join(self.evidence(), "capture.json")))

    def test_previous_evidence_is_never_overwritten(self):
        self.run_capture(py("print('first')"))
        first = self.read_bytes("stdout.log")
        with self.assertRaises(capture_tool.CaptureError):
            self.run_capture(py("print('second')"))
        self.assertEqual(self.read_bytes("stdout.log"), first)

    def test_cli_reports_the_rejection_without_running_the_child(self):
        os.makedirs(self.evidence())
        marker = os.path.join(self.tmp, "cli-child-ran")
        with redirect_stdout(StringIO()), redirect_stderr(StringIO()):
            code = capture_tool.main(
                [
                    "--evidence-dir",
                    self.evidence(),
                    "--cwd",
                    self.tmp,
                    "--",
                    *py(f"open({marker!r}, 'w').close()"),
                ]
            )
        self.assertEqual(code, 2)
        self.assertFalse(os.path.exists(marker))

    def test_record_itself_is_never_overwritten(self):
        self.run_capture(py("print('first')"))
        original = self.read_record()
        with self.assertRaises(FileExistsError):
            capture_tool.write_record(self.evidence(), {"complete": True, "label": "second"})
        self.assertEqual(self.read_record(), original)


class IncompleteCaptureTests(CaptureHarness):
    """A capture-level failure must be recorded, never dressed up as a run."""

    def test_failed_capture_records_complete_false_and_reraises(self):
        with self.assertRaises(capture_tool.CaptureError):
            self.run_capture(py("print('never')"), resource_tool="definitely-not-a-tool")
        record = self.read_record()
        self.assertFalse(record["complete"])
        self.assertIn("CaptureError", record["error"])
        self.assertEqual(record["command"]["argv"], py("print('never')"))
        self.assertNotIn("streams", record)
        self.assertFalse(os.path.exists(os.path.join(self.evidence(), "stdout.log")))

    def test_incomplete_capture_exits_two_at_the_cli(self):
        original = capture_tool.TIME_FLAGS
        capture_tool.TIME_FLAGS = {}
        self.addCleanup(setattr, capture_tool, "TIME_FLAGS", original)
        with redirect_stdout(StringIO()), redirect_stderr(StringIO()):
            code = capture_tool.main(
                [
                    "--evidence-dir",
                    self.evidence(),
                    "--cwd",
                    self.tmp,
                    "--resource-tool",
                    "time",
                    "--",
                    *py("pass"),
                ]
            )
        self.assertEqual(code, 2)
        self.assertFalse(self.read_record()["complete"])


class EnvironmentTests(CaptureHarness):
    def test_overrides_reach_the_child_and_are_recorded(self):
        record = self.run_capture(
            py("import os; print(os.environ['CAPTURE_PROBE'])"),
            env_overrides={"CAPTURE_PROBE": "value-with spaces=and=equals"},
        )
        self.assertEqual(self.read_bytes("stdout.log"), b"value-with spaces=and=equals\n")
        self.assertEqual(
            record["command"]["env_overrides"],
            {"CAPTURE_PROBE": "value-with spaces=and=equals"},
        )

    def test_overrides_layer_on_the_inherited_environment(self):
        # PATH is inherited from this process, not supplied by the caller.
        record = self.run_capture(
            py("import os; print(bool(os.environ.get('PATH')), os.environ.get('CAPTURE_LAYER'))"),
            env_overrides={"CAPTURE_LAYER": "override"},
        )
        self.assertEqual(self.read_bytes("stdout.log"), b"True override\n")
        self.assertEqual(record["command"]["env_overrides"], {"CAPTURE_LAYER": "override"})

    def test_cwd_is_honoured_and_recorded(self):
        work = os.path.join(self.tmp, "work")
        os.makedirs(work)
        record = self.run_capture(py("import os; print(os.getcwd())"), cwd=work)
        self.assertEqual(self.read_bytes("stdout.log").strip().decode(), os.path.realpath(work))
        self.assertEqual(record["command"]["cwd"], os.path.abspath(work))

    def test_malformed_override_is_refused(self):
        with self.assertRaises(capture_tool.CaptureError):
            capture_tool.parse_env_overrides(["NO_EQUALS_HERE"])

    def test_missing_cwd_is_refused_before_anything_is_created(self):
        missing = os.path.join(self.tmp, "not-a-directory")
        with self.assertRaises(capture_tool.CaptureError):
            self.run_capture(py("pass"), cwd=missing)
        self.assertFalse(os.path.exists(self.evidence()))


class CliTests(CaptureHarness):
    def run_cli(self, argv):
        """Invoke the CLI quietly and return its status code."""
        with redirect_stdout(StringIO()), redirect_stderr(StringIO()):
            return capture_tool.main(argv)

    def test_cli_exits_with_the_child_status(self):
        code = self.run_cli(
            ["--evidence-dir", self.evidence(), "--cwd", self.tmp, "--", *py("import sys; sys.exit(9)")]
        )
        self.assertEqual(code, 9)
        self.assertEqual(self.read_record()["result"]["exit_status"], 9)

    def test_cli_rejects_a_missing_child_command(self):
        with redirect_stderr(StringIO()):
            with self.assertRaises(SystemExit) as raised:
                capture_tool.main(["--evidence-dir", self.evidence()])
        self.assertEqual(raised.exception.code, 2)

    def test_cli_records_the_child_it_was_given(self):
        code = self.run_cli(
            [
                "--evidence-dir",
                self.evidence(),
                "--cwd",
                self.tmp,
                "--label",
                "via-cli",
                "--env",
                "CLI_PROBE=1",
                "--",
                *py("import os, sys; print(os.environ['CLI_PROBE']); sys.exit(3)"),
            ]
        )
        self.assertEqual(code, 3)
        record = self.read_record()
        self.assertEqual(record["label"], "via-cli")
        self.assertEqual(record["command"]["env_overrides"], {"CLI_PROBE": "1"})
        self.assertEqual(self.read_bytes("stdout.log"), b"1\n")


class ResourceTests(CaptureHarness):
    def test_rusage_is_recorded_for_the_child(self):
        record = self.run_capture(py("x = bytearray(2 * 1024 * 1024); print(len(x))"))
        resource = record["resource"]
        self.assertIsNotNone(resource)
        self.assertEqual(resource["source"], "wait4 rusage")
        self.assertGreater(resource["maxrss_bytes"], 0)
        self.assertIn(resource["maxrss_unit"], ("bytes", "kilobytes"))
        self.assertGreaterEqual(resource["utime_s"], 0.0)
        self.assertGreaterEqual(resource["page_reclaims"], 0)
        self.assertIsNone(record.get("resource_detail"))

    def test_time_tool_report_is_separate_from_child_stderr(self):
        if sys.platform not in capture_tool.TIME_FLAGS or shutil.which("time") is None:
            self.skipTest("no platform time tool available")
        record = self.run_capture(
            py("import sys; sys.stdout.write('out\\n'); sys.stderr.write('child-err\\n')"),
            resource_tool="time",
        )
        self.assertEqual(self.read_bytes("stderr.log"), b"child-err\n")
        self.assertEqual(record["streams"]["stderr"]["bytes"], len(b"child-err\n"))
        report = os.path.join(self.evidence(), capture_tool.TIME_REPORT_NAME)
        self.assertTrue(os.path.isfile(report))
        self.assertEqual(record["resource_detail"]["file"], capture_tool.TIME_REPORT_NAME)
        self.assertIsNone(record["resource_detail"]["error"])
        self.assertTrue(record["resource_detail"]["metrics"])
        self.assertTrue(record["timing"]["wall_includes_wrapper"])


class TimeoutTests(CaptureHarness):
    """A requested deadline must stop the whole child tree, not just a wrapper."""

    def test_timeout_records_expiry_and_kills_the_child(self):
        record = self.run_capture(py("import time; time.sleep(10)"), timeout=0.5)
        self.assertTrue(record["complete"], "the capture itself succeeded")
        self.assertTrue(record["timeout"]["expired"])
        self.assertEqual(record["timeout"]["seconds"], 0.5)
        self.assertEqual(record["timeout"]["kill_signal"], "SIGKILL")
        self.assertIsNone(record["timeout"]["kill_error"])
        # The status is what the wait actually said: killed, not "exited".
        self.assertEqual(record["result"]["kind"], "signaled")
        self.assertEqual(record["result"]["signal_name"], "SIGKILL")
        self.assertEqual(capture_tool.cli_status(record), capture_tool.TIMEOUT_STATUS)

    def test_timeout_is_bounded_by_the_requested_deadline(self):
        record = self.run_capture(py("import time; time.sleep(10)"), timeout=0.5)
        self.assertLess(record["timing"]["wall_s"], 10.0)

    def test_the_whole_process_group_is_killed(self):
        # The child spawns a grandchild in the same group and prints its pid;
        # a pid-only kill would leave the grandchild alive.
        source = (
            "import subprocess, sys, time\n"
            "kid = subprocess.Popen([sys.executable, '-c', 'import time; time.sleep(10)'])\n"
            "print(kid.pid, flush=True)\n"
            "time.sleep(10)\n"
        )
        record = self.run_capture(py(source), timeout=1.0)
        self.assertTrue(record["timeout"]["expired"])
        grandchild = int(self.read_bytes("stdout.log").split()[0])
        self.assertTrue(
            self._reaped(grandchild), f"grandchild {grandchild} survived the group kill"
        )

    def _reaped(self, pid, attempts=50):
        """Wait (briefly) for a pid to disappear from the process table."""
        for _ in range(attempts):
            try:
                os.kill(pid, 0)
            except ProcessLookupError:
                return True
            except PermissionError:
                return False  # exists, just not ours
            time.sleep(0.1)
        return False

    def test_a_fast_child_under_a_generous_timeout_is_untouched(self):
        record = self.run_capture(py("print('done')"), timeout=60)
        self.assertFalse(record["timeout"]["expired"])
        self.assertIsNone(record["timeout"]["kill_signal"])
        self.assert_clean_exit(record, 0)
        self.assertEqual(self.read_bytes("stdout.log"), b"done\n")

    def test_a_failing_child_under_a_timeout_keeps_its_own_status(self):
        record = self.run_capture(py("import sys; sys.exit(5)"), timeout=60)
        self.assertFalse(record["timeout"]["expired"])
        self.assert_clean_exit(record, 5)

    def test_without_a_timeout_no_deadline_is_recorded(self):
        record = self.run_capture(py("print('plain')"))
        self.assertIsNone(record["timeout"]["seconds"])
        self.assertFalse(record["timeout"]["expired"])
        self.assert_clean_exit(record, 0)

    def test_a_non_positive_timeout_is_refused(self):
        for bad in (0, -1):
            with self.assertRaises(capture_tool.CaptureError):
                capture_tool.CaptureRequest(
                    argv=tuple(py("pass")), cwd=self.tmp, evidence_dir=self.evidence(), timeout=bad
                ).resolved()

    def test_cli_timeout_exits_with_the_timeout_status(self):
        with redirect_stdout(StringIO()), redirect_stderr(StringIO()):
            code = capture_tool.main(
                [
                    "--evidence-dir",
                    self.evidence(),
                    "--cwd",
                    self.tmp,
                    "--timeout",
                    "0.5",
                    "--",
                    *py("import time; time.sleep(10)"),
                ]
            )
        self.assertEqual(code, capture_tool.TIMEOUT_STATUS)
        self.assertEqual(code, 124)
        self.assertTrue(self.read_record()["timeout"]["expired"])


class WrapperSupervisionTests(CaptureHarness):
    """Under --resource-tool time the reaped pid is the WRAPPER's.

    BSD time appends its own diagnostics to the child's stderr on abnormal
    death and rewrites a signal death into exit 1, so the record must say which
    process the status describes instead of passing the wrapper's off as the
    child's own outcome.
    """

    def setUp(self):
        super().setUp()
        if sys.platform not in capture_tool.TIME_FLAGS or shutil.which("time") is None:
            self.skipTest("no platform time tool available")

    def test_clean_run_is_not_flagged_as_abnormal(self):
        record = self.run_capture(py("import sys; sys.stderr.write('e\\n')"), resource_tool="time")
        self.assertFalse(record["wrapper_supervision"]["child_signal_unrecoverable"])
        self.assertFalse(record["wrapper_supervision"]["stderr_noise"]["present"])
        self.assertEqual(self.read_bytes("stderr.log"), b"e\n")

    def test_wrapper_noise_is_identified_not_mistaken_for_child_output(self):
        record = self.run_capture(
            py(
                "import os, signal, sys\n"
                "sys.stderr.write('child-own\\n'); sys.stderr.flush()\n"
                "os.kill(os.getpid(), signal.SIGTERM)\n"
            ),
            resource_tool="time",
        )
        noise = record["wrapper_supervision"]["stderr_noise"]
        self.assertTrue(noise["present"])
        # The raw stream still keeps every byte, wrapper text included.
        self.assertIn(b"child-own\n", self.read_bytes("stderr.log"))
        self.assertIn(b"time: ", self.read_bytes("stderr.log"))
        # ...and the child's own contribution is separately quantified.
        self.assertEqual(noise["clean_bytes"], len(b"child-own\n"))
        self.assertEqual(noise["clean_sha256"], hashlib.sha256(b"child-own\n").hexdigest())

    def test_sigterm_under_the_wrapper_is_still_reported_as_a_signal(self):
        record = self.run_capture(
            py("import os, signal; os.kill(os.getpid(), signal.SIGTERM)"), resource_tool="time"
        )
        self.assertEqual(record["result"]["kind"], "signaled")
        self.assertEqual(record["result"]["signal_name"], "SIGTERM")
        self.assertFalse(record["wrapper_supervision"]["child_signal_unrecoverable"])

    def test_a_signal_the_wrapper_cannot_report_is_flagged_not_invented(self):
        # BSD time rewrites a SIGKILL death into "exit 1", so the child's real
        # fate is unrecoverable from the wrapper status. The record must say so
        # rather than presenting the wrapper's exit code as the child's.
        record = self.run_capture(
            py(
                "import os, signal, sys\n"
                "sys.stderr.write('before\\n'); sys.stderr.flush()\n"
                "os.kill(os.getpid(), signal.SIGKILL)\n"
            ),
            resource_tool="time",
        )
        self.assertTrue(record["wrapper_supervision"]["child_signal_unrecoverable"])
        self.assertEqual(record["result"].get("reaped_process"), "time-wrapper")
        self.assertEqual(record["result"].get("child_status_reported_by"), "wrapper")
        # The wrapper's exit code is NOT claimed as the child's exit status.
        self.assertIn(record["result"]["kind"], ("exited", "signaled"))
        self.assertIn(b"before\n", self.read_bytes("stderr.log"))

    def test_a_clean_exit_keeps_its_own_status_under_the_wrapper(self):
        record = self.run_capture(py("import sys; sys.exit(7)"), resource_tool="time")
        self.assertEqual(record["result"]["kind"], "exited")
        self.assertEqual(record["result"]["exit_status"], 7)
        self.assertEqual(capture_tool.cli_status(record), 7)
        self.assertFalse(record["wrapper_supervision"]["child_signal_unrecoverable"])

    def test_unwrapped_runs_carry_no_wrapper_block(self):
        record = self.run_capture(py("print('plain')"))
        self.assertNotIn("wrapper_supervision", record)

    def test_the_recorded_kind_stays_inside_the_schema(self):
        record = self.run_capture(
            py("import sys; sys.stderr.write('x\\n'); sys.exit(1)"), resource_tool="time"
        )
        self.assertIn(record["result"]["kind"], ("exited", "signaled"))


if __name__ == "__main__":
    unittest.main()
