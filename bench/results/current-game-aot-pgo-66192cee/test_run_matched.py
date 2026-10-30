#!/usr/bin/env python3
"""Narrow tests for run-matched.py. No game is run here.

Parser and gate are exercised against the REAL report text of the four
committed prior arms, plus negatives derived from that same text. Integration
uses /bin/sh and /bin/sleep only.

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
capture = rm.benchmark_capture


# Real report text, verbatim from the committed prior arm records. A parser
# tested against invented text proves nothing.
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


class FixtureProvenance(unittest.TestCase):
    def test_fixtures_match_committed_summaries(self):
        repo = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
        base = os.path.join(repo, "bench/results/current-game-6a4f008b-c07042a7")
        for name, fixture in (("baseline-loaded.json", BASELINE_LOADED),
                              ("tip-loaded.json", TIP_LOADED),
                              ("baseline-loaded-long.json", BASELINE_LONG),
                              ("tip-loaded-long.json", TIP_LONG)):
            path = os.path.join(base, name)
            if not os.path.isfile(path):
                self.skipTest(f"committed record absent: {path}")
            with open(path, encoding="utf-8") as handle:
                record = json.load(handle)
            interesting = [line for line in record["summary"]
                           if line.startswith(("frames:", "scene:", "steady:",
                                               "worst frame:", "worldgen:"))]
            self.assertEqual(interesting, fixture.strip().splitlines(),
                             f"{name}: fixture drifted from the committed record")


class ParserRealText(unittest.TestCase):
    def test_baseline_loaded(self):
        p = rm.parse_output(BASELINE_LOADED)
        self.assertEqual(p["frames"]["frames"], 600)
        self.assertEqual(p["frames"]["avg_fps"], 157.3)
        self.assertEqual(p["frames"]["spikes"], 4)
        self.assertEqual(p["frames"]["cap_mode"], "uncapped")
        self.assertEqual(p["scene"]["drawn"], 53)
        self.assertEqual(p["scene"]["culling"], "frustum culled")
        self.assertEqual(p["steady"]["frames"], 558)
        self.assertEqual(p["worst_frame"]["frame"], 420)
        self.assertEqual(p["worst_frame"]["busy_pct"], 5)
        self.assertEqual(p["startup"]["worldgen"]["chunks"], 25)
        self.assertEqual(p["startup"]["worldgen"]["ms"], 73.0)

    def test_tip_long_is_exact(self):
        p = rm.parse_output(TIP_LONG)
        self.assertEqual(p["frames"]["frames"], 6000)
        self.assertEqual(p["frames"]["avg_fps"], 176.1)
        self.assertEqual(p["scene"]["drawn"], 64)
        self.assertEqual(p["steady"]["frames"], 5958)

    def test_startup_is_not_mixed_with_frames(self):
        p = rm.parse_output(BASELINE_LONG)
        self.assertEqual(p["frames"]["avg_ms"], 6.07)
        self.assertNotEqual(p["frames"]["avg_ms"], p["startup"]["worldgen"]["ms"])

    def test_absent_lines_are_none_not_zero(self):
        p = rm.parse_output("nothing here\n")
        for key in ("frames", "scene", "steady", "worst_frame"):
            self.assertIsNone(p[key])


class GatesOnRealText(unittest.TestCase):
    def test_prior_arms_meet_the_6000_gate_only_when_they_reach_it(self):
        for text, frames, ok in ((BASELINE_LOADED, 600, False), (TIP_LOADED, 600, False),
                                 (BASELINE_LONG, 6000, True), (TIP_LONG, 6000, True)):
            gate = rm.gate(0, rm.parse_output(text))
            self.assertEqual(gate["passed"], ok, f"{frames} frames: {gate}")

    def test_committed_long_arms_pass(self):
        for text in (BASELINE_LONG, TIP_LONG):
            self.assertTrue(rm.gate(0, rm.parse_output(text))["passed"])


class GatesNegative(unittest.TestCase):
    def _fails(self, text, field, status=0):
        gate = rm.gate(status, rm.parse_output(text))
        self.assertFalse(gate["passed"])
        self.assertIn(field, " ".join(gate["failures"]))
        return gate

    def test_short_frame_count_names_the_field(self):
        gate = self._fails(TIP_LONG.replace("frames: 6000", "frames: 4631", 1), "frames")
        self.assertIn("4631", " ".join(gate["failures"]))

    def test_wrong_radius(self):
        self._fails(TIP_LONG.replace("radius 16", "radius 9", 1), "scene.radius")

    def test_streaming_in(self):
        self._fails(TIP_LONG.replace("streamed in 0", "streamed in 3", 1), "scene.streamed_in")

    def test_streaming_out(self):
        self._fails(TIP_LONG.replace("streamed in 0 out 0", "streamed in 0 out 2", 1),
                    "scene.streamed_out")

    def test_loaded_short(self):
        self._fails(TIP_LONG.replace("loaded 64", "loaded 41", 1), "scene.loaded")

    def test_nonzero_exit(self):
        self._fails(TIP_LONG, "exit_status", status=3)

    def test_missing_scene_line_is_unproven(self):
        text = "\n".join(l for l in TIP_LONG.splitlines() if not l.startswith("scene:"))
        self._fails(text, "scene")

    def test_capped_run(self):
        self._fails(TIP_LONG.replace("uncapped", "vsync+60fps cap", 1), "cap_mode")

    def test_missing_steady(self):
        text = "\n".join(l for l in TIP_LONG.splitlines() if not l.startswith("steady:"))
        self._fails(text, "steady")

    def test_every_failure_names_its_field(self):
        for failure in rm.gate(1, rm.parse_output(""))["failures"]:
            self.assertIn(":", failure)


class ParserRobustness(unittest.TestCase):
    def test_scene_without_culling_suffix(self):
        lines = TIP_LONG.splitlines()
        text = "\n".join([lines[1], lines[3],
                          "scene:  radius 16  loaded 64  drawn 64  wanted 64"
                          "  streamed in 0 out 0"]) + "\n"
        p = rm.parse_output(text)
        self.assertIsNone(p["scene"]["culling"])
        self.assertTrue(rm.gate(0, p)["passed"])

    def test_first_matching_line_wins(self):
        extra = ("frames: 1  avg 1.0 FPS (1000.00 ms)  best 1.00 ms  worst 1.0 ms"
                 "  spikes >33ms: 0 (0.00%)  edits: 0  uncapped\n")
        self.assertEqual(rm.parse_output(TIP_LONG + extra)["frames"]["frames"], 6000)

    def test_comment_mentioning_frames_is_not_a_report(self):
        self.assertIsNone(rm.parse_output("# frames: 6000 not a report\n")["frames"])


class ManifestValidation(unittest.TestCase):
    def test_committed_manifests_have_expected_sizes(self):
        with open(rm.RUNTIME_MANIFEST, encoding="utf-8") as h:
            self.assertEqual(len(json.load(h)), 228)
        with open(rm.WORLD_MANIFEST, encoding="utf-8") as h:
            self.assertEqual(len(json.load(h)), 214)

    def test_tampered_tree_fails_and_names_the_file(self):
        tmp = tempfile.mkdtemp()
        try:
            good = os.path.join(tmp, "a.txt")
            with open(good, "w") as h:
                h.write("original")
            entries = [{"path": "a.txt", "sha256": rm.sha256_file(good)},
                       {"path": "gone.txt", "sha256": "0" * 64}]
            self.assertTrue(rm.verify(tmp, entries[:1])["passed"])
            with open(good, "w") as h:
                h.write("MUTATED")
            after = rm.verify(tmp, entries)
            self.assertFalse(after["passed"])
            self.assertIn("a.txt", after["mismatched"])
            self.assertIn("gone.txt", after["missing"])
        finally:
            shutil.rmtree(tmp)

    def test_real_frozen_tree_validates(self):
        if not os.path.isdir(rm.GAME_DIR):
            self.skipTest("frozen copy absent")
        result = rm.validate(rm.GAME_DIR)
        self.assertTrue(result["passed"], result)
        self.assertEqual(result["runtime"]["ok"], 228)
        self.assertEqual(result["world"]["ok"], 214)


class CacheFreshness(unittest.TestCase):
    """A warm cache makes two arms unequal, so it must be refused."""

    def _run(self, cache_root, evidence):
        return subprocess.run(
            [sys.executable, DRIVER, "--label", "x", "--executable", "/bin/true",
             "--evidence-dir", evidence, "--cache-root", cache_root, "--dry-run"],
            capture_output=True, text=True)

    def test_populated_cache_root_is_refused(self):
        tmp = tempfile.mkdtemp()
        try:
            cache = os.path.join(tmp, "cache")
            os.makedirs(cache)
            with open(os.path.join(cache, "leftover.jdn"), "w") as h:
                h.write("stale")
            result = self._run(cache, os.path.join(tmp, "ev"))
            self.assertEqual(result.returncode, 2)
            self.assertIn("not fresh", result.stderr)
            self.assertIn("leftover.jdn", result.stderr)
        finally:
            shutil.rmtree(tmp)

    def test_absent_cache_root_is_accepted_and_recorded(self):
        tmp = tempfile.mkdtemp()
        try:
            result = self._run(os.path.join(tmp, "absent"), os.path.join(tmp, "ev"))
            self.assertEqual(result.returncode, 0, result.stderr)
            plan = json.loads(result.stdout)
            self.assertTrue(plan["cache_fresh"])
            self.assertEqual(plan["cache_prior_entries"], 0)
        finally:
            shutil.rmtree(tmp)

    def test_empty_existing_cache_root_is_accepted(self):
        tmp = tempfile.mkdtemp()
        try:
            cache = os.path.join(tmp, "empty")
            os.makedirs(cache)
            result = self._run(cache, os.path.join(tmp, "ev"))
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(json.loads(result.stdout)["cache_prior_entries"], 0)
        finally:
            shutil.rmtree(tmp)

    def test_cache_root_that_is_a_file_is_refused(self):
        tmp = tempfile.mkdtemp()
        try:
            path = os.path.join(tmp, "afile")
            with open(path, "w") as h:
                h.write("x")
            result = self._run(path, os.path.join(tmp, "ev"))
            self.assertEqual(result.returncode, 2)
            self.assertIn("not a directory", result.stderr)
        finally:
            shutil.rmtree(tmp)


class CliRefusals(unittest.TestCase):
    def test_missing_game_dir_is_refused(self):
        result = subprocess.run(
            [sys.executable, DRIVER, "--label", "x", "--executable", "/bin/true",
             "--evidence-dir", tempfile.mkdtemp() + "/ev",
             "--cache-root", "/tmp/c", "--game-dir", "/nonexistent"],
            capture_output=True, text=True)
        self.assertEqual(result.returncode, 2)
        self.assertIn("refused", result.stderr)

    def test_wrong_workload_refused_before_spawning(self):
        tmp = tempfile.mkdtemp()
        try:
            result = subprocess.run(
                [sys.executable, DRIVER, "--label", "x", "--executable", "/bin/true",
                 "--evidence-dir", os.path.join(tmp, "ev"),
                 "--cache-root", os.path.join(tmp, "c"), "--game-dir", tmp],
                capture_output=True, text=True)
            self.assertEqual(result.returncode, 2)
            self.assertIn("mismatch", result.stderr)
            self.assertFalse(os.path.exists(os.path.join(tmp, "ev")))
        finally:
            shutil.rmtree(tmp)


class DryRun(unittest.TestCase):
    def test_dry_run_spawns_nothing(self):
        tmp = tempfile.mkdtemp()
        try:
            evidence = os.path.join(tmp, "ev")
            result = subprocess.run(
                [sys.executable, DRIVER, "--label", "plan", "--executable", "/bin/true",
                 "--evidence-dir", evidence, "--cache-root", os.path.join(tmp, "c"),
                 "--dry-run"], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            plan = json.loads(result.stdout)
            self.assertEqual(plan["gates"]["frames"], 6000)
            self.assertEqual(plan["gates"]["scene"]["radius"], 16)
            self.assertFalse(plan["rss_enforced_mid_run"])
            self.assertFalse(os.path.exists(evidence))
        finally:
            shutil.rmtree(tmp)


class CaptureIntegration(unittest.TestCase):
    def test_capture_runs_one_child(self):
        tmp = tempfile.mkdtemp()
        try:
            evidence = os.path.join(tmp, "ev")
            record = capture.capture(capture.CaptureRequest(
                argv=("/bin/sh", "-c", "echo hello; exit 0"), cwd=tmp,
                evidence_dir=evidence, label="it"))
            self.assertTrue(record["complete"])
            self.assertEqual(record["result"]["exit_status"], 0)
            with open(os.path.join(evidence, "stdout.log")) as h:
                self.assertIn("hello", h.read())
        finally:
            shutil.rmtree(tmp)

    def test_timeout_kills_the_child(self):
        tmp = tempfile.mkdtemp()
        try:
            record = capture.capture(capture.CaptureRequest(
                argv=("/bin/sleep", "30"), cwd=tmp,
                evidence_dir=os.path.join(tmp, "ev"), label="t", timeout=1.0))
            self.assertTrue(record["timeout"]["expired"])
            self.assertLess(record["timing"]["wall_s"], 10.0)
        finally:
            shutil.rmtree(tmp)

    def test_evidence_dir_never_overwritten(self):
        tmp = tempfile.mkdtemp()
        try:
            request = capture.CaptureRequest(
                argv=("/bin/sh", "-c", "exit 0"), cwd=tmp,
                evidence_dir=os.path.join(tmp, "ev"), label="once")
            capture.capture(request)
            with self.assertRaises(capture.CaptureError):
                capture.capture(request)
        finally:
            shutil.rmtree(tmp)


if __name__ == "__main__":
    unittest.main(verbosity=2)
