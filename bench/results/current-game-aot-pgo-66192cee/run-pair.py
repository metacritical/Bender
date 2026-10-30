#!/usr/bin/env python3
"""Run a matched 6000-frame PAIR, sequentially, and compare the two arms.

This is the supplemental comparison the parent asked for: one interpreter arm
and one plain-AOT arm, both at 6000 frames against the same frozen workload.

It is NOT a PGO result. Neither arm consumes or produces a profile, the native
tier is not involved, and no claim about PGO benefit, kernel emission or
speedup attribution is made. The comparison is one matched observation.

Sequentiality is the point: the two arms never overlap, because a benchmark
whose arms contend for the same GPU and CPU measures the scheduler, not the
runtime. Each arm goes through run-matched.py, so each gets its own evidence
directory, its own cache root, and its own pre/post workload validation.

Usage:
  run-pair.py --root <evidence root> [--cache-root <dir>] [--dry-run]

Exit 0 only when BOTH arms pass their gates AND their scene identity matches.
"""

import argparse
import json
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
DRIVER = os.path.join(HERE, "run-matched.py")
REPO_ROOT = os.path.abspath(os.path.join(HERE, "..", "..", ".."))

BUILD = os.path.join(REPO_ROOT, "build", "current-game-aot-pgo-66192cee")
INTERPRETER = os.path.join(BUILD, "runtime", "build", "bender")
GAME_SCRIPT = os.path.join(BUILD, "game-source", "main.janet")
PLAIN_AOT = os.path.join(BUILD, "artifacts", "plain-aot")

DISCLAIMER = (
    "Supplemental matched interpreter-vs-plain-AOT observation at 6000 frames. "
    "NOT a PGO result: no profile is collected or consumed, the native executor "
    "tier is not exercised, and no speedup, kernel-emission or GC-causality "
    "claim follows from it."
)


def arm_command(label, executable, script, evidence, cache):
    argv = [
        sys.executable, DRIVER,
        "--label", label,
        "--executable", executable,
        "--evidence-dir", evidence,
        "--cache-root", cache,
    ]
    if script:
        argv += ["--script", script]
    return argv


def run_arm(label, executable, script, evidence, cache, dry_run):
    """Run one arm, inheriting stdout/stderr so the parent sees live progress."""
    argv = arm_command(label, executable, script, evidence, cache)
    print(f"--- arm {label}: {executable}", file=sys.stderr, flush=True)
    if dry_run:
        argv.append("--dry-run")
        result = subprocess.run(argv)
        return result.returncode, None
    result = subprocess.run(argv)
    summary_path = os.path.join(evidence, "summary.json")
    summary = None
    if os.path.isfile(summary_path):
        with open(summary_path, "r", encoding="utf-8") as handle:
            summary = json.load(handle)
    return result.returncode, summary


def scene_of(summary):
    if not summary:
        return None
    return (summary.get("parsed") or {}).get("scene")


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Run a matched 6000-frame interpreter/plain-AOT pair, sequentially.",
    )
    parser.add_argument("--root", required=True, help="parent directory for both arm evidence dirs")
    parser.add_argument(
        "--cache-root",
        default=None,
        help="parent directory for the per-arm XDG caches (default: <root>/cache)",
    )
    parser.add_argument("--dry-run", action="store_true", help="print both plans, spawn nothing")
    args = parser.parse_args(argv)

    root = os.path.abspath(args.root)
    cache_parent = os.path.abspath(args.cache_root or os.path.join(root, "cache"))

    for path, what in ((INTERPRETER, "interpreter binary"),
                       (GAME_SCRIPT, "game script"),
                       (PLAIN_AOT, "plain AOT binary")):
        if not os.path.exists(path):
            print(f"refused: {what} missing: {path}", file=sys.stderr)
            return 2

    arms = [
        ("interp-6000", INTERPRETER, GAME_SCRIPT),
        ("plain-aot-6000", PLAIN_AOT, None),
    ]

    if args.dry_run:
        for label, executable, script in arms:
            code, _ = run_arm(
                label, executable, script,
                os.path.join(root, label), os.path.join(cache_parent, label), True,
            )
            if code != 0:
                return code
        return 0

    if os.path.exists(root):
        print(f"refused: evidence root already exists: {root}", file=sys.stderr)
        return 2
    os.makedirs(root)

    codes = {}
    summaries = {}
    # Strictly sequential: each arm completes before the next begins.
    for label, executable, script in arms:
        code, summary = run_arm(
            label, executable, script,
            os.path.join(root, label), os.path.join(cache_parent, label), False,
        )
        codes[label] = code
        summaries[label] = summary
        if code != 0:
            # Stop rather than let a failed arm's contention pollute the next.
            print(f"stopping: arm {label} did not pass (exit {code})", file=sys.stderr)
            break

    interp_scene = scene_of(summaries.get("interp-6000"))
    aot_scene = scene_of(summaries.get("plain-aot-6000"))

    matched = None
    if interp_scene and aot_scene:
        compared = ("radius", "loaded", "drawn", "wanted", "streamed_in", "streamed_out")
        differences = {
            key: {"interpreter": interp_scene.get(key), "aot": aot_scene.get(key)}
            for key in compared
            if interp_scene.get(key) != aot_scene.get(key)
        }
        matched = not differences
    elif len(summaries) == 2:
        differences = {"scene": "one arm produced no scene line"}

    verdict = {
        "schema": "janet/run-pair/1",
        "disclaimer": DISCLAIMER,
        "is_pgo_result": False,
        "matched_pair": True,
        "sequential": True,
        "arms": {
            label: {
                "exit": codes.get(label),
                "passed": (summaries.get(label) or {}).get("passed"),
                "failures": (summaries.get(label) or {}).get("failures"),
                "evidence_dir": os.path.join(root, label),
                "scene": scene_of(summaries.get(label)),
                "frames": ((summaries.get(label) or {}).get("parsed") or {}).get("frames"),
                "steady": ((summaries.get(label) or {}).get("parsed") or {}).get("steady"),
                "worst_frame": ((summaries.get(label) or {}).get("parsed") or {}).get("worst_frame"),
                "startup": ((summaries.get(label) or {}).get("parsed") or {}).get("startup"),
                "peak_rss": ((summaries.get(label) or {}).get("resource") or {}).get("peak_rss"),
            }
            for label in codes
        },
        "scene_identical": matched,
        "scene_differences": (differences if matched is False else {}),
        "both_passed": all(code == 0 for code in codes.values()) and len(codes) == 2,
    }
    verdict["passed"] = bool(verdict["both_passed"] and verdict["scene_identical"])

    path = os.path.join(root, "pair-verdict.json")
    with open(path, "x", encoding="utf-8") as handle:
        json.dump(verdict, handle, indent=2)
        handle.write("\n")

    print(json.dumps({
        "passed": verdict["passed"],
        "both_passed": verdict["both_passed"],
        "scene_identical": verdict["scene_identical"],
        "verdict": path,
    }, indent=2))
    return 0 if verdict["passed"] else 1


if __name__ == "__main__":
    sys.exit(main())
