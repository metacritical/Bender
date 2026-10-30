#!/usr/bin/env python3
"""Narrow tests for run-matched.py.

No game is run here. The parser and gate are exercised against REAL report text
taken from the four committed prior arms, plus negative fixtures derived from
that same text. The one integration test uses /bin/sleep, never the workload.

Run: python3 bench/results/current-game-aot-pgo-66192cee/test_run_matched.py
"""

import importlib.util
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
DRIVER = os.path.join(HERE, "run-matched.py")


def _load_driver():
    spec = importlib.util.spec_from_file_location("run_matched", DRIVER)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


rm = _load_driver()


# ---------------------------------------------------------------------------
# Real report text, copied verbatim from the committed prior arm records.
# These are the exact lines the game printed; the fixtures must not drift from
# them, because a parser tested against invented text proves nothing.
# ---------------------------------------------------------------------------

BASELINE_LOADED = """\
worldgen: 25 spawn chunks in 73 ms (tile cache)
frames: 600  avg 157.3 FPS (6.36 ms)  best 3.87 ms  worst 81.8 ms  spikes >33ms: 4 (0.68%)  edits: 0  uncapped
worst frame: #420  81.8 ms wall, 4.2 ms CPU (5% busy), mesh queue 0
steady: 156.6 FPS (6.38 ms) over 558 frames, reached at frame 11
scene:  radius 16  loaded 64  drawn 53  wanted 64  streamed in 0 out 0  (frustum culled)
"""

TIP_LOADED = """\
worldgen: 25 spawn chunks in 71 ms (tile cache)
frames: 600  avg 182.5 FPS (5.48 ms)  best 3.66 ms  worst 24.5 ms  spikes >33ms: 0 (0.00%)  edits: 0  uncapped
worst frame: #12  24.5 ms wall, 4.0 ms CPU (16% busy), mesh queue 0
steady: 183.0 FPS (5.46 ms) over 558 frames, reached at frame 11
scene:  radius 16  loaded 64  drawn 53  wanted 64  streamed in 0 out 0  (frustum culled)
"""

BASELINE_LONG = """\
worldgen: 25 spawn chunks in 72 ms (tile cache)
frames: 6000  avg 164.8 FPS (6.07 ms)  best 3.81 ms  worst 178.2 ms  spikes >33ms: 72 (1.20%)  edits: 0  uncapped
worst frame: #4849  178.2 ms wall, 4.1 ms CPU (2% busy), mesh queue 0
steady: 165.0 FPS (6.06 ms) over 5958 frames, reached at frame 11
scene:  radius 16  loaded 64  drawn 64  wanted 64  streamed in 0 out 0  (frustum culled)
"""

TIP_LONG = """\
worldgen: 25 spawn chunks in 68 ms (tile cache)
frames: 6000  avg 176.1 FPS (5.68 ms)  best 3.65 ms  worst 44.6 ms  spikes >33ms: 21 (0.35%)  edits: 0  uncapped
worst frame: #4435  44.6 ms wall, 4.2 ms CPU (9% busy), mesh queue 0
steady: 176.1 FPS (5.68 ms) over 5958 frames, reached at frame 11
scene:  radius 16  loaded 64  drawn 64  wanted 64  streamed in 0 out 0  (frustum culled)
"""


# ---------------------------------------------------------------------------
# Is the fixture text still the committed text?
# ---------------------------------------------------------------------------


class FixtureProvenance(unittest.TestCase):
    """Fail loudly if the fixtures drift from the committed records."""

    def test_fixtures_match_committed_summaries(self):
        repo = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
        base = os.path.join(
            repo, "bench", "results", "current-game-6a4f008b-c07042a7"
        )
        pairs = [
            ("baseline-loaded.json", BASELINE_LOADED),
            ("tip-loaded.json", TIP_LOADED),
            ("baseline-loaded-long.json", BASELINE_LONG),
            ("tip-loaded-long.json", TIP_LONG),
        ]
        for name, fixture in pairs:
            path = os.path.join(base, name)
            if not os.path.isfile(path):
                self.skipTest(f"committed record not present: {path}")
            with open(path, "r", encoding="utf-8") as handle:
                record = json.load(handle)
            interesting = [
                line for line in record["summary"]
                if line.startswith(
                    ("frames:", "scene:", "steady:", "worst frame:", "worldgen:")
                )
            ]
            self.assertEqual(
                interesting,
                fixture.strip().splitlines(),
                f"{name}: fixture text has drifted from the committed record",
            )


# ---------------------------------------------------------------------------
# Parser, against real text
# ---------------------------------------------------------------------------


class ParserRealText(unittest.TestCase):
    def test_baseline_loaded(self):
        p = rm.parse_output(BASELINE_LOADED)
        self.assertEqual(p["frames"]["frames"], 600)
        self.assertEqual(p["frames"]["avg_fps"], 157.3)
        self.assertEqual(p["frames"]["worst_ms"], 81.8)
        self.assertEqual(p["frames"]["spikes"], 4)
        self.assertEqual(p["frames"]["cap_mode"], "uncapped")
        self.assertEqual(p["scene"]["drawn"], 53)
        self.assertEqual(p["scene"]["radius"], 16)
        self.assertEqual(p["scene"]["culling"], "frustum culled")
        self.assertEqual(p["steady"]["frames"], 558)
        self.assertEqual(p["steady"]["reached_at_frame"], 11)
        self.assertEqual(p["worst_frame"]["frame"], 420)
        self.assertEqual(p["worst_frame"]["busy_pct"], 5)
        self.assertEqual(p["startup"]["worldgen"]["chunks"], 25)
        self.assertEqual(p["startup"]["worldgen"]["ms"], 73.0)
        self.assertEqual(p["startup"]["worldgen"]["source"], "(tile cache)")

    def test_tip_long_is_exact(self):
        p = rm.parse_output(TIP_LONG)
        self.assertEqual(p["frames"]["frames"], 6000)
        self.assertEqual(p["frames"]["avg_fps"], 176.1)
        self.assertEqual(p["frames"]["spikes"], 21)
        self.assertEqual(p["scene"]["loaded"], 64)
        self.assertEqual(p["scene"]["drawn"], 64)
        self.assertEqual(p["scene"]["wanted"], 64)
        self.assertEqual(p["steady"]["frames"], 5958)

    def test_startup_is_not_mixed_with_frames(self):
        p = rm.parse_output(BASELINE_LONG)
        self.assertIsNotNone(p["startup"]["worldgen"])
        self.assertEqual(p["frames"]["avg_ms"], 6.07)
        # The startup number must not be mistaken for a frame time.
        self.assertNotEqual(p["frames"]["avg_ms"], p["startup"]["worldgen"]["ms"])

    def test_absent_lines_are_none_not_zero(self):
        p = rm.parse_output("no report here\n")
        self.assertIsNone(p["frames"])
        self.assertIsNone(p["scene"])
        self.assertIsNone(p["steady"])
        self.assertIsNone(p["worst_frame"])


# ---------------------------------------------------------------------------
# Gates, on real text and on negative fixtures derived from it
# ---------------------------------------------------------------------------


class GatesOnRealText(unittest.TestCase):
    def test_the_four_prior_arms_meet_the_6000_gate_only_when_they_reach_it(self):
        # The 600-frame arms are legitimate runs, but NOT 6000-frame arms.
        for text, frames, ok in (
            (BASELINE_LOADED, 600, False),
            (TIP_LOADED, 600, False),
            (BASELINE_LONG, 6000, True),
            (TIP_LONG, 6000, True),
        ):
            parsed = rm.parse_output(text)
            gate = rm.gate_record(0, parsed, expected_frames=frames)
            self.assertEqual(
                gate["passed"], ok,
                f"{frames}-frame fixture: expected passed={ok}, got {gate}",
            )

    def test_committed_long_arms_pass_the_exact_6000_scene_gate(self):
        for text in (BASELINE_LONG, TIP_LONG):
            gate = rm.gate_record(0, rm.parse_output(text))
            self.assertTrue(gate["passed"], gate["failures"])


class GatesNegative(unittest.TestCase):
    def test_short_frame_count_names_the_field(self):
        text = TIP_LONG.replace("frames: 6000", "frames: 4631", 1)
        gate = rm.gate_record(0, rm.parse_output(text))
        self.assertFalse(gate["passed"])
        joined = " ".join(gate["failures"])
        self.assertIn("frames", joined)
        self.assertIn("4631", joined)

    def test_wrong_radius_is_named(self):
        text = TIP_LONG.replace("radius 16", "radius 9", 1)
        gate = rm.gate_record(0, rm.parse_output(text))
        self.assertFalse(gate["passed"])
        self.assertIn("scene.radius", " ".join(gate["failures"]))

    def test_streaming_in_is_rejected(self):
        text = TIP_LONG.replace("streamed in 0 out 0", "streamed in 3 out 0", 1)
        gate = rm.gate_record(0, rm.parse_output(text))
        self.assertFalse(gate["passed"])
        self.assertIn("scene.streamed_in", " ".join(gate["failures"]))

    def test_streaming_out_is_rejected(self):
        text = TIP_LONG.replace("streamed in 0 out 0", "streamed in 0 out 2", 1)
        gate = rm.gate_record(0, rm.parse_output(text))
        self.assertFalse(gate["passed"])
        self.assertIn("scene.streamed_out", " ".join(gate["failures"]))

    def test_loaded_short_is_rejected(self):
        text = TIP_LONG.replace("loaded 64", "loaded 41", 1)
        gate = rm.gate_record(0, rm.parse_output(text))
        self.assertFalse(gate["passed"])
        self.assertIn("scene.loaded", " ".join(gate["failures"]))

    def test_nonzero_exit_is_rejected(self):
        gate = rm.gate_record(3, rm.parse_output(TIP_LONG))
        self.assertFalse(gate["passed"])
        self.assertIn("exit_status", " ".join(gate["failures"]))

    def test_missing_scene_line_is_unproven_not_a_pass(self):
        text = "\n".join(
            line for line in TIP_LONG.splitlines() if not line.startswith("scene:")
        )
        gate = rm.gate_record(0, rm.parse_output(text))
        self.assertFalse(gate["passed"])
        self.assertIn("scene", " ".join(gate["failures"]))

    def test_capped_run_is_rejected(self):
        text = TIP_LONG.replace("uncapped", "vsync+60fps cap", 1)
        gate = rm.gate_record(0, rm.parse_output(text))
        self.assertFalse(gate["passed"])
        self.assertIn("cap_mode", " ".join(gate["failures"]))

    def test_every_failure_is_named_not_a_bare_false(self):
        blank = rm.gate_record(1, rm.parse_output(""))
        self.assertGreaterEqual(len(blank["failures"]), 4)
        for failure in blank["failures"]:
            self.assertIn(":", failure, "a failure must name its field")


# ---------------------------------------------------------------------------
# Parser must not be fooled by lookalike lines
# ---------------------------------------------------------------------------


class ParserRobustness(unittest.TestCase):
    def test_scene_line_without_culling_suffix_parses(self):
        # The culling suffix is optional in the format; its absence must not
        # make an otherwise-complete arm fail.
        text = (
            "frames: 6000  avg 176.1 FPS (5.68 ms)  best 3.65 ms  worst 44.6 ms"
            "  spikes >33ms: 21 (0.35%)  edits: 0  uncapped\n"
            "steady: 176.1 FPS (5.68 ms) over 5958 frames, reached at frame 11\n"
            "scene:  radius 16  loaded 64  drawn 64  wanted 64  streamed in 0 out 0\n"
        )
        parsed = rm.parse_output(text)
        self.assertIsNotNone(parsed["scene"])
        self.assertIsNone(parsed["scene"]["culling"])
        self.assertTrue(rm.gate_record(0, parsed)["passed"])

    def test_first_matching_line_wins(self):
        text = TIP_LONG + "\nframes: 1  avg 1.0 FPS (1000.00 ms)  best 1.00 ms  worst 1.0 ms  spikes >33ms: 0 (0.00%)  edits: 0  uncapped\n"
        parsed = rm.parse_output(text)
        self.assertEqual(parsed["frames"]["frames"], 6000)

    def test_a_comment_mentioning_frames_is_not_a_report(self):
        text = "# frames: 6000 should appear here but does not\n"
        self.assertIsNone(rm.parse_output(text)["frames"])


# ---------------------------------------------------------------------------
# Manifest validation plumbing, against the real committed manifests
# ---------------------------------------------------------------------------


class ManifestValidation(unittest.TestCase):
    def test_committed_manifests_load_and_have_expected_sizes(self):
        runtime = rm.load_manifest(rm.RUNTIME_MANIFEST)
        world = rm.load_manifest(rm.WORLD_MANIFEST)
        self.assertEqual(len(runtime), 228)
        self.assertEqual(len(world), 214)

    def test_a_tampered_tree_fails_and_names_the_file(self):
        tmp = tempfile.mkdtemp()
        try:
            entries = [
                {"path": "a.txt", "sha256": rm.sha256_file(__file__)},
                {"path": "b.txt", "sha256": "0" * 64},
            ]
            with open(os.path.join(tmp, "a.txt"), "w") as handle:
                handle.write("mismatch\n")
            result = rm.verify_manifest(tmp, entries, rm.sha256_file)
            self.assertFalse(result["passed"])
            self.assertIn("a.txt", result["mismatched"])
            self.assertIn("b.txt", result["missing"])
        finally:
            shutil.rmtree(tmp)


# ---------------------------------------------------------------------------
# Environment contract
# ---------------------------------------------------------------------------


class EnvironmentContract(unittest.TestCase):
    def setUp(self):
        self.saved = dict(os.environ)

    def tearDown(self):
        os.environ.clear()
        os.environ.update(self.saved)

    def test_scrub_removes_both_prefixes_and_reports_names_only(self):
        os.environ["JANET_SECRET_TOKEN"] = "hunter2"
        os.environ["BENDER_THING"] = "x"
        os.environ["KEEP_ME"] = "yes"
        dropped = rm.scrub_process_environment()
        self.assertIn("JANET_SECRET_TOKEN", dropped)
        self.assertIn("BENDER_THING", dropped)
        self.assertNotIn("JANET_SECRET_TOKEN", os.environ)
        self.assertNotIn("BENDER_THING", os.environ)
        self.assertEqual(os.environ["KEEP_ME"], "yes")
        self.assertNotIn("hunter2", json.dumps(dropped))

    def test_bench_arm_disables_profiling(self):
        env = rm.build_environment("/tmp/cache", "/tmp/ev", train=False)
        self.assertEqual(env["JANET_NO_PROFILE"], "1")
        self.assertEqual(env["JANET_LANES"], "0")
        self.assertNotIn("JANET_PROFILE", env)
        self.assertNotIn("JANET_PROFILE_OUT", env)

    def test_train_arm_requests_a_profile_into_evidence(self):
        env = rm.build_environment("/tmp/cache", "/tmp/ev", train=True)
        self.assertEqual(env["JANET_PROFILE"], "1")
        self.assertEqual(env["JANET_PROFILE_OUT"], "/tmp/ev/profile.jdn")
        self.assertEqual(env["JANET_LANES"], "0")
        # The dump is suppressed when this is set: TRAIN must never set it.
        self.assertNotIn("JANET_NO_PROFILE", env)


# ---------------------------------------------------------------------------
# Driver CLI refusals (no child spawned)
# ---------------------------------------------------------------------------


class CliRefusals(unittest.TestCase):
    def test_missing_game_dir_is_refused(self):
        result = subprocess.run(
            [sys.executable, DRIVER, "--label", "x", "--executable", "/bin/true",
             "--evidence-dir", tempfile.mkdtemp() + "/ev",
             "--cache-root", "/tmp/c", "--game-dir", "/nonexistent/game"],
            capture_output=True, text=True,
        )
        self.assertEqual(result.returncode, 2)
        self.assertIn("refused", result.stderr)

    def test_wrong_workload_is_refused_before_spawning(self):
        tmp = tempfile.mkdtemp()
        try:
            result = subprocess.run(
                [sys.executable, DRIVER, "--label", "x", "--executable", "/bin/true",
                 "--evidence-dir", os.path.join(tmp, "ev"),
                 "--cache-root", "/tmp/c", "--game-dir", tmp],
                capture_output=True, text=True,
            )
            self.assertEqual(result.returncode, 2)
            self.assertIn("mismatch", result.stderr)
            # Nothing may have been executed.
            self.assertFalse(os.path.exists(os.path.join(tmp, "ev")))
        finally:
            shutil.rmtree(tmp)


# ---------------------------------------------------------------------------
# Integration: the capture path, using /bin/sleep (never the game)
# ---------------------------------------------------------------------------


class CaptureIntegration(unittest.TestCase):
    def test_capture_runs_one_child_and_records_evidence(self):
        tmp = tempfile.mkdtemp()
        try:
            evidence = os.path.join(tmp, "ev")
            record = rm.capture_module().capture(
                rm.capture_module().CaptureRequest(
                    argv=("/bin/sh", "-c", "echo hello; exit 0"),
                    cwd=tmp,
                    evidence_dir=evidence,
                    label="integration",
                )
            )
            self.assertTrue(record["complete"])
            self.assertEqual(record["result"]["exit_status"], 0)
            with open(os.path.join(evidence, "stdout.log")) as handle:
                self.assertIn("hello", handle.read())
            self.assertTrue(os.path.isfile(os.path.join(evidence, "capture.json")))
        finally:
            shutil.rmtree(tmp)

    def test_timeout_kills_the_child_group(self):
        tmp = tempfile.mkdtemp()
        try:
            evidence = os.path.join(tmp, "ev")
            record = rm.capture_module().capture(
                rm.capture_module().CaptureRequest(
                    argv=("/bin/sleep", "30"),
                    cwd=tmp,
                    evidence_dir=evidence,
                    label="timeout",
                    timeout=1.0,
                )
            )
            self.assertTrue(record["timeout"]["expired"])
            self.assertLess(record["timing"]["wall_s"], 10.0)
        finally:
            shutil.rmtree(tmp)

    def test_evidence_dir_is_never_overwritten(self):
        tmp = tempfile.mkdtemp()
        try:
            evidence = os.path.join(tmp, "ev")
            request = rm.capture_module().CaptureRequest(
                argv=("/bin/sh", "-c", "exit 0"),
                cwd=tmp, evidence_dir=evidence, label="once",
            )
            rm.capture_module().capture(request)
            with self.assertRaises(rm.capture_module().CaptureError):
                rm.capture_module().capture(request)
        finally:
            shutil.rmtree(tmp)



# ---------------------------------------------------------------------------
# --dry-run must plan without spawning anything
# ---------------------------------------------------------------------------


class DryRun(unittest.TestCase):
    def test_dry_run_spawns_nothing_and_creates_no_evidence(self):
        tmp = tempfile.mkdtemp()
        try:
            evidence = os.path.join(tmp, "ev")
            result = subprocess.run(
                [sys.executable, DRIVER, "--label", "plan", "--executable", "/bin/true",
                 "--evidence-dir", evidence, "--cache-root", os.path.join(tmp, "c"),
                 "--dry-run"],
                capture_output=True, text=True,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            plan = json.loads(result.stdout)
            self.assertTrue(plan["dry_run"])
            self.assertEqual(plan["gates"]["frames"], 6000)
            self.assertEqual(plan["gates"]["scene"]["radius"], 16)
            self.assertFalse(plan["rss_enforced_mid_run"])
            self.assertFalse(os.path.exists(evidence), "dry run must not create evidence")
            self.assertIn("--frames", plan["argv"])
            self.assertEqual(plan["argv"][-2], "--world-root")
        finally:
            shutil.rmtree(tmp)

    def test_dry_run_still_refuses_a_bad_workload(self):
        tmp = tempfile.mkdtemp()
        try:
            result = subprocess.run(
                [sys.executable, DRIVER, "--label", "plan", "--executable", "/bin/true",
                 "--evidence-dir", os.path.join(tmp, "ev"),
                 "--cache-root", os.path.join(tmp, "c"),
                 "--game-dir", tmp, "--dry-run"],
                capture_output=True, text=True,
            )
            self.assertEqual(result.returncode, 2)
            self.assertIn("mismatch", result.stderr)
        finally:
            shutil.rmtree(tmp)


if __name__ == "__main__":
    unittest.main(verbosity=2)
