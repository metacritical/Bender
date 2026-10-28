"""Tests for bench/wave-controlled-run.py.

These use tiny synthetic Python children, so they exercise the runner's
process handling, parsing and evidence layout without building or running a
real wave. Every child is ``sys.executable -c "..."``.
"""

import importlib.util
import json
import os
import shutil
import sys
import tempfile
import textwrap
import time
import unittest
from contextlib import redirect_stderr, redirect_stdout
from io import StringIO

HERE = os.path.dirname(os.path.abspath(__file__))
SCRIPT = os.path.join(os.path.dirname(HERE), "wave-controlled-run.py")


def load_runner():
    spec = importlib.util.spec_from_file_location("wave_controlled_run", SCRIPT)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


runner = load_runner()

GOOD = (
    "import sys;n=int(sys.argv[1]);"
    "print(f'procs={n} result={n} spawn=4ms wave=5ms total=9ms');"
    "sys.exit(0)"
)


def child(source):
    """A synthetic child argv running `source` with the requested n appended."""
    return [sys.executable, "-c", textwrap.dedent(source).strip(), "{n}"]


def good_child():
    return child(GOOD)


class RunnerTestCase(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp(prefix="wave-runner-test-")
        self.addCleanup(shutil.rmtree, self.tmp, True)

    # -- helpers ---------------------------------------------------------
    def write_manifest(self, variants, **top):
        manifest = {
            "repo": self.tmp,
            "output_dir": os.path.join(self.tmp, "out"),
            "sizes": [5],
            "repeats": 1,
            "timeout_s": 20,
        }
        manifest.update(top)
        manifest["variants"] = variants
        path = os.path.join(self.tmp, "manifest.json")
        with open(path, "w", encoding="utf-8") as handle:
            json.dump(manifest, handle)
        return path

    def run_manifest(self, path, *args):
        stdout, stderr = StringIO(), StringIO()
        with redirect_stdout(stdout), redirect_stderr(stderr):
            code = runner.main([path, *args])
        return code, stdout.getvalue(), stderr.getvalue()

    def run_single(self, source, name="probe", **variant_kw):
        """Run one synthetic-child variant and return (exit code, first trial)."""
        manifest = {"name": name, "argv": child(source)}
        manifest.update(variant_kw)
        code, _, _ = self.run_manifest(self.write_manifest([manifest]))
        return code, self.read_out("trials.jsonl")[0]

    def read_out(self, name):
        with open(os.path.join(self.tmp, "out", name), encoding="utf-8") as handle:
            if name.endswith(".jsonl"):
                return [json.loads(line) for line in handle if line.strip()]
            return json.load(handle)

    def trial_dirs(self):
        root = os.path.join(self.tmp, "out")
        found = []
        for variant in os.listdir(root):
            variant_dir = os.path.join(root, variant)
            if not os.path.isdir(variant_dir):
                continue
            for n_dir in sorted(os.listdir(variant_dir)):
                for rep in sorted(os.listdir(os.path.join(variant_dir, n_dir))):
                    found.append(os.path.join(variant_dir, n_dir, rep))
        return found

    # -- success and evidence -------------------------------------------
    def test_success_retains_every_repeat_and_logs(self):
        path = self.write_manifest(
            [
                {
                    "name": "good",
                    "argv": good_child(),
                    "sizes": [5, 7],
                    "repeats": 3,
                }
            ],
            repeats=3,
        )
        code, out, _ = self.run_manifest(path)

        self.assertEqual(code, 0, out)
        trials = self.read_out("trials.jsonl")
        self.assertEqual(len(trials), 6)
        self.assertTrue(all(trial["ok"] for trial in trials))
        self.assertEqual(
            sorted({(t["n"], t["repeat"]) for t in trials}),
            sorted({(n, rep) for n in (5, 7) for rep in (1, 2, 3)}),
        )

        summary = self.read_out("summary.json")
        self.assertTrue(summary["ok"])
        self.assertEqual(len(summary["groups"]), 2)
        for group in summary["groups"]:
            self.assertEqual(group["trials"], 3)
            self.assertEqual(len(group["samples"]), 3)
            self.assertEqual(group["metrics"]["result"]["count"], 3)
            self.assertIn("median", group["metrics"]["result"])
            self.assertIn("stdev", group["metrics"]["result"])

        # Per-trial artifacts exist for every trial.
        dirs = self.trial_dirs()
        self.assertEqual(len(dirs), 6)
        for trial_dir in dirs:
            for name in ("stdout.log", "stderr.log", "trial.json"):
                self.assertTrue(
                    os.path.isfile(os.path.join(trial_dir, name)),
                    f"missing {name} in {trial_dir}",
                )

    def test_run_json_records_provenance(self):
        path = self.write_manifest([{"name": "good", "argv": good_child()}])
        code, _, _ = self.run_manifest(path)
        self.assertEqual(code, 0)

        run_meta = self.read_out("run.json")
        self.assertEqual(run_meta["schema"], runner.SCHEMA)
        self.assertRegex(run_meta["manifest_sha256"], r"^[0-9a-f]{64}$")
        self.assertEqual(
            run_meta["manifest_sha256"],
            runner.sha256_file(path),
        )
        self.assertIn("python", run_meta["machine"])
        self.assertIn("cpu_count", run_meta["machine"])
        # git may be absent in a sandbox, but the field is always present.
        self.assertIn("git", run_meta)
        self.assertIn("rev", run_meta["git"])
        self.assertIn("dirty", run_meta["git"])

        executables = run_meta["executables"]
        self.assertTrue(executables)
        digest = executables[str(runner.os.path.abspath(sys.executable))]
        self.assertRegex(digest, r"^[0-9a-f]{64}$")
        self.assertEqual(digest, runner.sha256_file(sys.executable))

        # ...and the digest is repeated on the trial itself.
        trial = self.read_out("trials.jsonl")[0]
        self.assertEqual(trial["executable"]["sha256"], digest)

    # -- correctness failures -------------------------------------------
    def test_wrong_result_is_a_failure(self):
        code, trial = self.run_single(
            "import sys;n=int(sys.argv[1]);"
            "print(f'procs={n} result={n + 1} spawn=1ms wave=1ms total=2ms')",
            name="wrong",
        )
        self.assertEqual(code, 1)
        self.assertFalse(trial["ok"])
        self.assertIn("result_mismatch", trial["failures"])
        self.assertEqual(trial["exit_status"], 0)
        summary = self.read_out("summary.json")
        self.assertFalse(summary["ok"])
        self.assertIn("result_mismatch", summary["groups"][0]["failures"])

    def test_wrong_result_prints_failure_verdict(self):
        path = self.write_manifest([{"name": "wrong", "argv": child(GOOD)}])
        code, out, _ = self.run_manifest(path, "--only", "wrong")
        self.assertEqual(code, 0)
        self.assertIn("all passed", out)
        # A separate run whose result is wrong must say so in the summary line.
        path = self.write_manifest(
            [
                {
                    "name": "wrong",
                    "argv": child(
                        "import sys;n=int(sys.argv[1]);"
                        "print(f'procs={n} result={n + 1}')"
                    ),
                }
            ]
        )
        code, out, _ = self.run_manifest(path)
        self.assertEqual(code, 1)
        self.assertIn("FAILURES PRESENT", out)

    def test_procs_mismatch_is_a_failure(self):
        code, trial = self.run_single(
            "import sys;n=int(sys.argv[1]);print(f'procs={n + 2} result={n}')",
            name="procs",
        )
        self.assertEqual(code, 1)
        self.assertIn("procs_mismatch", trial["failures"])

    def test_missing_result_is_a_failure(self):
        code, trial = self.run_single("print('nothing useful')", name="silent")
        self.assertEqual(code, 1)
        self.assertIn("missing_result", trial["failures"])

    def test_nonzero_exit_is_a_failure_even_with_right_result(self):
        code, trial = self.run_single(
            "import sys;n=int(sys.argv[1]);"
            "print(f'procs={n} result={n}');sys.exit(3)",
            name="nonzero",
        )
        self.assertEqual(code, 1)
        self.assertIn("nonzero_exit", trial["failures"])
        self.assertEqual(trial["exit_status"], 3)
        # The correct result does not launder a nonzero exit.
        self.assertEqual(trial["metrics"]["result"], 5)

    def test_signal_is_recorded_not_hidden(self):
        code, trial = self.run_single(
            "import os, signal;os.kill(os.getpid(), signal.SIGTERM)",
            name="sigterm",
        )
        self.assertEqual(code, 1)
        self.assertIn("signal", trial["failures"])
        self.assertFalse(trial["timed_out"])
        self.assertIsNone(trial["exit_status"])
        self.assertEqual(trial["term_signal"], 15)
        self.assertEqual(trial["term_signal_name"], "SIGTERM")

    def test_timeout_kills_child_and_records_it(self):
        code, trial = self.run_single(
            "import time; time.sleep(30)", name="hang", timeout_s=0.4
        )
        self.assertEqual(code, 1)
        self.assertTrue(trial["timed_out"])
        self.assertEqual(trial["failures"], ["timeout"])
        self.assertLess(trial["wall_s"], 10)
        # The timeout kill is SIGKILL to the child's process group.
        self.assertEqual(trial["term_signal"], 9)
        self.assertEqual(trial["term_signal_name"], "SIGKILL")
        self.assertIsNone(trial["kill_error"])

    def test_timeout_kills_the_whole_child_process_group(self):
        """A surviving grandchild would prove the kill reached too narrowly."""
        marker_pid = os.path.join(self.tmp, "grandchild.pid")
        source = (
            "import subprocess, sys, time;"
            "p = subprocess.Popen("
            "[sys.executable, '-c', 'import time; time.sleep(60)']);"
            f"open({marker_pid!r}, 'w').write(str(p.pid));"
            "time.sleep(60)"
        )
        code, trial = self.run_single(source, name="tree", timeout_s=0.6)
        self.assertEqual(code, 1)
        self.assertTrue(trial["timed_out"])
        with open(marker_pid, encoding="utf-8") as handle:
            grandchild = int(handle.read().strip())
        deadline = time.monotonic() + 5
        while time.monotonic() < deadline:
            try:
                os.kill(grandchild, 0)
            except ProcessLookupError:
                return
            time.sleep(0.05)
        self.fail("grandchild survived the process-group kill")

    def test_missing_executable_is_one_clean_failure(self):
        path = self.write_manifest(
            [{"name": "ghost", "argv": ["./no-such-binary", "{n}"]}]
        )
        code, _, _ = self.run_manifest(path)
        self.assertEqual(code, 1)
        trial = self.read_out("trials.jsonl")[0]
        # One cause, not a cascade of missing_result/procs noise.
        self.assertEqual(trial["failures"], ["spawn_error"])
        self.assertTrue(trial["spawn_error"])
        self.assertEqual(trial["exit_status"], None)
        self.assertEqual(trial["metrics"], {})
        # Evidence is still written for a trial that never started.
        self.assertTrue(os.path.isfile(trial["stdout_path"]))
        self.assertTrue(os.path.isfile(trial["stderr_path"]))

    # -- argv and parsing ------------------------------------------------
    def test_argv_is_literal_never_a_shell(self):
        path = self.write_manifest(
            [
                {
                    "name": "literal",
                    "argv": [
                        sys.executable,
                        "-c",
                        "import sys;n=int(sys.argv[1]);"
                        "print(f'procs={n} result={n}');"
                        "print('|'.join(sys.argv[2:]))",
                        "{n}",
                        "a;b",
                        "$HOME",
                        "`whoami`",
                        "x y",
                    ],
                }
            ]
        )
        code, _, _ = self.run_manifest(path)
        self.assertEqual(code, 0, "a shell would have mangled the argv entries")
        trial = self.read_out("trials.jsonl")[0]
        with open(trial["stdout_path"], encoding="utf-8") as handle:
            body = handle.read()
        self.assertIn("a;b|$HOME|`whoami`|x y", body)

    def test_placeholders_expand_n_repeat_and_variant(self):
        path = self.write_manifest(
            [
                {
                    "name": "ph",
                    "argv": [
                        sys.executable,
                        "-c",
                        "import sys;n=int(sys.argv[1]);"
                        "print(f'procs={n} result={n}');"
                        "print('|'.join(sys.argv[2:]))",
                        "{n}",
                        "rep={repeat}",
                        "variant={variant}",
                    ],
                    "repeats": 2,
                }
            ]
        )
        code, _, _ = self.run_manifest(path)
        self.assertEqual(code, 0)
        for trial in self.read_out("trials.jsonl"):
            self.assertEqual(trial["argv"][-2:],
                             [f"rep={trial['repeat']}",
                              f"variant={trial['variant']}"])

    def test_unknown_placeholder_is_a_manifest_error(self):
        path = self.write_manifest(
            [{"name": "bad", "argv": [sys.executable, "-c", "pass", "{nodes}"]}]
        )
        code, _, err = self.run_manifest(path)
        self.assertEqual(code, 2)
        self.assertIn("{nodes}", err)
        self.assertIn("unknown placeholder", err)

    def test_legacy_go_style_ms_suffix_parses(self):
        code, trial = self.run_single(
            "import sys;n=int(sys.argv[1]);"
            "print(f'procs={n} result={n} spawn=7ms wave=8ms total=15ms')",
            name="go",
        )
        self.assertEqual(code, 0)
        metrics = trial["metrics"]
        self.assertEqual(metrics["spawn_ms"], 7)
        self.assertEqual(metrics["wave_ms"], 8)
        self.assertEqual(metrics["total_ms"], 15)

    def test_janet_split_stdout_and_stderr_records_merge(self):
        code, trial = self.run_single(
            "import sys;n=int(sys.argv[1]);"
            "print(f'procs={n} result={n}');"
            "print(f'procs={n} result={n} spawn_ms=3 wave_ms=4 total_ms=7',"
            " file=sys.stderr)",
            name="janet",
        )
        self.assertEqual(code, 0)
        self.assertEqual(trial["metrics"]["spawn_ms"], 3)
        self.assertEqual(trial["metrics"]["total_ms"], 7)
        self.assertEqual(trial["metric_conflicts"], [])

    def test_conflicting_result_records_are_a_failure(self):
        code, trial = self.run_single(
            "import sys;n=int(sys.argv[1]);"
            "print(f'procs={n} result={n}');"
            "print(f'procs={n} result={n - 1}', file=sys.stderr)",
            name="conflict",
        )
        self.assertEqual(code, 1)
        self.assertEqual(len(trial["metric_conflicts"]), 1)
        self.assertIn("metric_conflict", trial["failures"])

    def test_memory_extra_metrics_tolerated(self):
        # No `result` key at all is a missing_result failure; that is correct,
        # but the extra metrics must still be parsed and retained.
        code, trial = self.run_single(
            "import sys;n=int(sys.argv[1]);"
            "print('live_procs=%d mem_mb=12.5 base_mb=2.5' % n)",
            name="memory",
        )
        self.assertEqual(code, 1)
        metrics = trial["metrics"]
        self.assertEqual(metrics["live_procs"], 5)
        self.assertEqual(metrics["mem_mb"], 12.5)
        self.assertEqual(metrics["base_mb"], 2.5)
        self.assertIn("missing_result", trial["failures"])

    # -- statistics ------------------------------------------------------
    def test_summary_is_not_min_only(self):
        path = self.write_manifest(
            [
                {
                    "name": "spread",
                    "argv": child(
                        "import os, sys;n=int(sys.argv[1]);"
                        "print(f'procs={n} result={n} "
                        "spawn_ms={int(os.environ[\"SPREAD\"])}')"
                    ),
                    "repeats": 3,
                    "env": {"SPREAD": "0"},
                }
            ],
            env={"SPREAD": "0"},
        )
        code, _, _ = self.run_manifest(path)
        self.assertEqual(code, 0)
        summary = self.read_out("summary.json")
        group = summary["groups"][0]
        # All three runs produce the same metric, so the spread is over wall time;
        # what matters is that every sample is retained, not collapsed.
        self.assertEqual(len(group["samples"]), 3)
        self.assertEqual(group["metrics"]["spawn_ms"]["count"], 3)
        self.assertEqual(group["metrics"]["spawn_ms"]["min"], 0)
        self.assertEqual(group["metrics"]["spawn_ms"]["max"], 0)

    def test_stats_helper_reports_full_distribution(self):
        stats = runner._stats([5.0, 1.0, 3.0])
        self.assertEqual(stats["count"], 3)
        self.assertEqual(stats["min"], 1.0)
        self.assertEqual(stats["max"], 5.0)
        self.assertEqual(stats["median"], 3.0)
        self.assertAlmostEqual(stats["mean"], 3.0)
        self.assertIsNotNone(stats["stdev"])
        self.assertIsNone(runner._stats([]))

    # -- manifest and CLI -------------------------------------------------
    def test_example_manifest_is_valid_and_runs_dry(self):
        normalized = runner.normalize_manifest(runner.EXAMPLE_MANIFEST, self.tmp)
        self.assertEqual(len(normalized["variants"]), 4)
        self.assertTrue(
            all(v["sizes"] == [10000, 100000] for v in normalized["variants"])
        )
        self.assertEqual(runner.total_trials(normalized), 4 * 2 * 3)

    def test_checked_in_example_manifest_loads(self):
        path = os.path.join(HERE, "wave-manifest.example.json")
        manifest = runner.load_manifest(path)
        self.assertEqual(
            [v["name"] for v in manifest["variants"]],
            ["janet-script-lanes0", "janet-script-lanes11", "go", "erlang"],
        )
        self.assertEqual(runner.total_trials(manifest), 24)
        self.assertEqual(manifest["repo"], os.path.dirname(os.path.dirname(HERE)))

    def test_duplicate_variant_names_rejected(self):
        path = self.write_manifest(
            [
                {"name": "dup", "argv": good_child()},
                {"name": "dup", "argv": good_child()},
            ]
        )
        code, _, err = self.run_manifest(path)
        self.assertEqual(code, 2)
        self.assertIn("duplicate variant name", err)

    def test_missing_sizes_rejected(self):
        path = os.path.join(self.tmp, "manifest.json")
        with open(path, "w", encoding="utf-8") as handle:
            json.dump({"variants": [{"name": "x", "argv": good_child()}]}, handle)
        code, _, err = self.run_manifest(path)
        self.assertEqual(code, 2)
        self.assertIn("sizes", err)

    def test_only_selects_one_variant(self):
        path = self.write_manifest(
            [
                {"name": "a", "argv": good_child()},
                {"name": "b", "argv": good_child()},
            ]
        )
        code, _, _ = self.run_manifest(path, "--only", "a")
        self.assertEqual(code, 0)
        trials = self.read_out("trials.jsonl")
        self.assertEqual([t["variant"] for t in trials], ["a"])

    def test_print_example_manifest(self):
        stdout, stderr = StringIO(), StringIO()
        with redirect_stdout(stdout), redirect_stderr(stderr):
            code = runner.main(["--print-example-manifest"])
        self.assertEqual(code, 0)
        payload = json.loads(stdout.getvalue())
        self.assertIn("variants", payload)

    def test_missing_manifest_exits_with_usage(self):
        stdout, stderr = StringIO(), StringIO()
        with redirect_stdout(stdout), redirect_stderr(stderr):
            with self.assertRaises(SystemExit) as caught:
                runner.main([])
        self.assertEqual(caught.exception.code, 2)


if __name__ == "__main__":
    unittest.main()
