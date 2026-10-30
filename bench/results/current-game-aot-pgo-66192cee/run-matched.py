#!/usr/bin/env python3
"""Run ONE arm of the frozen current-game workload and write local evidence.

This driver exists so a matched arm is reproducible from committed evidence
alone. It never runs more than one child, never runs the game by itself in
tests, and never writes into the frozen tree: the frozen tree is read-only
input, and both a pre-flight and a post-run validation prove it stayed that way.

Two arm kinds share one code path:

  BENCH  profiling off (JANET_NO_PROFILE=1). This is the measurable arm.
  TRAIN  explicit profile collection (JANET_PROFILE=1 + JANET_PROFILE_OUT)
         with JANET_NO_PROFILE deliberately ABSENT, so the dump is written
         into this arm's evidence directory.

The capture implementation is not reimplemented here. `tools/benchmark_capture`
is imported and used as-is, and its sha256 is recorded in every record so a
change to that module cannot silently change what "captured" means.

Usage:
  run-matched.py --label tip-bench --executable <path> [--script <path>]
                 --evidence-dir <new dir> --cache-root <dir> [--train]

Exit status:
  0  the arm completed and every gate passed
  1  a gate failed (the record names the failing field)
  2  the driver refused to run (bad arguments, missing/incorrect workload)
"""

import argparse
import hashlib
import json
import os
import platform
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO_ROOT = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
TOOLS_DIR = os.path.join(REPO_ROOT, "tools")

# The committed reference manifests. These are the ONLY definition of the
# workload; a tree that does not match them is not this benchmark.
REFERENCE_EVIDENCE = os.path.join(
    REPO_ROOT, "bench", "results", "current-game-6a4f008b-c07042a7"
)
RUNTIME_MANIFEST = os.path.join(REFERENCE_EVIDENCE, "runtime-source-manifest.json")
WORLD_MANIFEST = os.path.join(REFERENCE_EVIDENCE, "saved-world-manifest.json")

# The saved-world manifest's paths are relative to the SLOT directory, not to
# the world root. Anchoring them at worlds-v1 reports all 214 entries missing.
WORLD_SLOT = "15f2268da8ec2a4105ef2deaf6ae0b19"

FRAMES = 6000
WALL_CAP_S = 180.0
RSS_CAP_BYTES = 1024 ** 3  # 1 GiB, asserted on the observed peak after the run

EXPECTED_SCENE = {
    "radius": 16,
    "loaded": 64,
    "drawn": 64,
    "wanted": 64,
    "streamed_in": 0,
    "streamed_out": 0,
}

FAILED = 1
REFUSED = 2


# --------------------------------------------------------------------------
# capture utility
# --------------------------------------------------------------------------


def load_capture():
    """Import the shared capture module, and pin the exact bytes imported."""
    if TOOLS_DIR not in sys.path:
        sys.path.insert(0, TOOLS_DIR)
    import benchmark_capture  # noqa: E402  (path set immediately above)

    path = os.path.abspath(benchmark_capture.__file__)
    with open(path, "rb") as handle:
        digest = hashlib.sha256(handle.read()).hexdigest()
    return benchmark_capture, {"path": path, "sha256": digest}


def capture_module():
    """The capture module alone, for callers that do not need the pin."""
    return load_capture()[0]


# --------------------------------------------------------------------------
# workload validation
# --------------------------------------------------------------------------


def load_manifest(path):
    with open(path, "r", encoding="utf-8") as handle:
        return json.load(handle)


def verify_manifest(root, entries, sha256_of):
    """Compare a tree against one manifest; report counts and named failures.

    Returns a dict, never raises for a mismatch: a mismatched tree is a verdict
    the caller reports, not an exception that hides which file was wrong.
    """
    ok = 0
    missing = []
    mismatched = []
    for entry in entries:
        target = os.path.join(root, entry["path"])
        if not os.path.isfile(target):
            missing.append(entry["path"])
            continue
        if sha256_of(target) != entry["sha256"]:
            mismatched.append(entry["path"])
        else:
            ok += 1
    return {
        "root": root,
        "expected": len(entries),
        "ok": ok,
        "missing": missing,
        "mismatched": mismatched,
        "passed": not missing and not mismatched and ok == len(entries),
    }


def sha256_file(path):
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def validate_workload(game_dir):
    """Validate the frozen runtime tree and the saved world against the manifests."""
    runtime_entries = load_manifest(RUNTIME_MANIFEST)
    world_entries = load_manifest(WORLD_MANIFEST)
    runtime = verify_manifest(game_dir, runtime_entries, sha256_file)
    world = verify_manifest(
        os.path.join(game_dir, "worlds-v1", WORLD_SLOT), world_entries, sha256_file
    )
    return {
        "runtime_source_manifest": {
            "path": RUNTIME_MANIFEST,
            "sha256": sha256_file(RUNTIME_MANIFEST),
            **runtime,
        },
        "saved_world_manifest": {
            "path": WORLD_MANIFEST,
            "sha256": sha256_file(WORLD_MANIFEST),
            "path_root": "worlds-v1/" + WORLD_SLOT,
            **world,
        },
        "passed": runtime["passed"] and world["passed"],
    }


# --------------------------------------------------------------------------
# parsing the game's own output
# --------------------------------------------------------------------------

RE_FRAMES = re.compile(
    r"^frames:\s*(?P<frames>\d+)\s+avg\s+(?P<avg_fps>[\d.]+)\s+FPS\s+"
    r"\((?P<avg_ms>[\d.]+)\s*ms\)\s+best\s+(?P<best_ms>[\d.]+)\s*ms\s+"
    r"worst\s+(?P<worst_ms>[\d.]+)\s*ms\s+spikes\s*>\s*33ms:\s*(?P<spikes>\d+)\s+"
    r"\((?P<spike_pct>[\d.]+)%\)\s+edits:\s*(?P<edits>\d+)\s+(?P<cap_mode>.+?)\s*$"
)
RE_SCENE = re.compile(
    r"^scene:\s+radius\s+(?P<radius>\d+)\s+loaded\s+(?P<loaded>\d+)\s+"
    r"drawn\s+(?P<drawn>\d+)\s+wanted\s+(?P<wanted>\d+)\s+"
    r"streamed in\s+(?P<streamed_in>\d+)\s+out\s+(?P<streamed_out>\d+)"
    r"(?:\s+\((?P<culling>[^)]*)\))?\s*$"
)
RE_STEADY = re.compile(
    r"^steady:\s+(?P<fps>[\d.]+)\s+FPS\s+\((?P<ms>[\d.]+)\s*ms\)\s+"
    r"over\s+(?P<frames>\d+)\s+frames,\s+reached at frame\s+(?P<reached>\d+)\s*$"
)
RE_WORST = re.compile(
    r"^worst frame:\s+#(?P<frame>\d+)\s+(?P<wall_ms>[\d.]+)\s*ms wall,\s+"
    r"(?P<cpu_ms>[\d.]+)\s*ms CPU\s+\((?P<busy_pct>\d+)%\s+busy\),\s+"
    r"mesh queue\s+(?P<mesh_queue>\d+)\s*$"
)
RE_WORLDGEN = re.compile(
    r"^worldgen:\s+(?P<chunks>\d+)\s+spawn chunks in\s+(?P<ms>[\d.]+)\s*ms"
    r"(?P<suffix>.*)$"
)
RE_STALLS = re.compile(r"^stalls >100ms:\s*(?P<count>\d+)\s*$")


def _float(value):
    return float(value)


def parse_output(text):
    """Parse the game's report lines into separate, independently-gated fields.

    Startup (worldgen/mesher) is kept apart from frame statistics on purpose:
    wall time is not frame time, and a startup number in a frame comparison is
    how a scheduling fact gets misread as an engine cost.
    """
    frames = scene = steady = worst = worldgen = None
    stalls = None
    for line in text.splitlines():
        line = line.rstrip()
        if frames is None and (m := RE_FRAMES.match(line)):
            frames = {
                "frames": int(m["frames"]),
                "avg_fps": _float(m["avg_fps"]),
                "avg_ms": _float(m["avg_ms"]),
                "best_ms": _float(m["best_ms"]),
                "worst_ms": _float(m["worst_ms"]),
                "spikes": int(m["spikes"]),
                "spike_pct": _float(m["spike_pct"]),
                "edits": int(m["edits"]),
                "cap_mode": m["cap_mode"].strip(),
            }
        elif scene is None and (m := RE_SCENE.match(line)):
            scene = {
                "radius": int(m["radius"]),
                "loaded": int(m["loaded"]),
                "drawn": int(m["drawn"]),
                "wanted": int(m["wanted"]),
                "streamed_in": int(m["streamed_in"]),
                "streamed_out": int(m["streamed_out"]),
                "culling": (m["culling"] or "").strip() or None,
            }
        elif steady is None and (m := RE_STEADY.match(line)):
            steady = {
                "fps": _float(m["fps"]),
                "ms": _float(m["ms"]),
                "frames": int(m["frames"]),
                "reached_at_frame": int(m["reached"]),
            }
        elif worst is None and (m := RE_WORST.match(line)):
            worst = {
                "frame": int(m["frame"]),
                "wall_ms": _float(m["wall_ms"]),
                "cpu_ms": _float(m["cpu_ms"]),
                "busy_pct": int(m["busy_pct"]),
                "mesh_queue": int(m["mesh_queue"]),
            }
        elif worldgen is None and (m := RE_WORLDGEN.match(line)):
            worldgen = {
                "chunks": int(m["chunks"]),
                "ms": _float(m["ms"]),
                "source": (m["suffix"] or "").strip() or None,
            }
        elif stalls is None and (m := RE_STALLS.match(line)):
            stalls = int(m["count"])
    return {
        "startup": {"worldgen": worldgen},
        "frames": frames,
        "steady": steady,
        "worst_frame": worst,
        "stalls_over_100ms": stalls,
        "scene": scene,
    }


# --------------------------------------------------------------------------
# gates
# --------------------------------------------------------------------------


def gate_record(exit_status, parsed, expected_frames=FRAMES, expected_scene=EXPECTED_SCENE):
    """Decide whether this arm is admissible, naming every failing field.

    A bare False is not a useful verdict: the caller must be able to say which
    number was wrong and what it was, not merely that the arm "failed".
    """
    failures = []

    if exit_status != 0:
        failures.append(f"exit_status: expected 0, got {exit_status!r}")

    frames = parsed.get("frames")
    if frames is None:
        failures.append("frames: line absent from stdout (the game never reported)")
    else:
        if frames["frames"] != expected_frames:
            failures.append(
                f"frames: reached {frames['frames']} of {expected_frames}"
            )
        if frames["cap_mode"] != "uncapped":
            failures.append(f"cap_mode: expected 'uncapped', got {frames['cap_mode']!r}")

    scene = parsed.get("scene")
    if scene is None:
        failures.append("scene: line absent from stdout; identity is unproven")
    else:
        for field, want in expected_scene.items():
            got = scene.get(field)
            if got != want:
                failures.append(f"scene.{field}: expected {want}, got {got!r}")

    if parsed.get("steady") is None:
        failures.append("steady: line absent; the stream never reached steady state")

    return {"passed": not failures, "failures": failures}


# --------------------------------------------------------------------------
# host snapshot (no secrets: no paths, no usernames, no arguments)
# --------------------------------------------------------------------------


def host_snapshot():
    try:
        load1, load5, load15 = os.getloadavg()
    except OSError:
        load1 = load5 = load15 = None
    return {
        "loadavg": {"1m": load1, "5m": load5, "15m": load15},
        "cpu_count": os.cpu_count(),
        "platform": platform.platform(),
        "machine": platform.machine(),
    }


# --------------------------------------------------------------------------
# environment
# --------------------------------------------------------------------------


def scrub_process_environment():
    """Delete every inherited JANET_/BENDER_ key from THIS process.

    The capture utility merges its overrides over the inherited environment, so
    it can add a key but cannot remove one. Scrubbing here is what makes
    "JANET_* is gone unless this driver set it" true for the child. The removed
    NAMES are returned for the record; their values are not, because an
    inherited value is not evidence and may be private.
    """
    dropped = sorted(k for k in os.environ if k.startswith(("JANET_", "BENDER_")))
    for key in dropped:
        del os.environ[key]
    return dropped


def build_environment(cache_root, evidence_dir, train):
    """The small override set this driver adds back after scrubbing."""
    overrides = {
        "JANET_LANES": "0",
        "XDG_CACHE_HOME": os.path.abspath(cache_root),
    }
    if train:
        # JANET_NO_PROFILE must be ABSENT here. With it set the dump is
        # suppressed, and the arm would claim a profile it never wrote.
        overrides["JANET_PROFILE"] = "1"
        overrides["JANET_PROFILE_OUT"] = os.path.join(
            os.path.abspath(evidence_dir), "profile.jdn"
        )
    else:
        overrides["JANET_NO_PROFILE"] = "1"
    return overrides


# --------------------------------------------------------------------------
# main
# --------------------------------------------------------------------------


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Run one matched arm of the frozen current-game workload.",
    )
    parser.add_argument("--label", required=True, help="arm name recorded in evidence")
    parser.add_argument("--executable", required=True, help="already-built binary to run")
    parser.add_argument(
        "--script",
        default=None,
        help="game script; omit for an AOT binary that embeds it",
    )
    parser.add_argument(
        "--train",
        action="store_true",
        help="collect a profile instead of measuring (writes profile.jdn to evidence)",
    )
    parser.add_argument("--evidence-dir", required=True, help="new directory for this arm")
    parser.add_argument("--cache-root", required=True, help="XDG_CACHE_HOME for this arm")
    parser.add_argument(
        "--game-dir",
        default=None,
        help="frozen game directory (defaults to the durable copy under build/)",
    )
    args = parser.parse_args(argv)

    game_dir = args.game_dir or os.path.join(
        REPO_ROOT, "build", "current-game-aot-pgo-66192cee", "game-source"
    )
    game_dir = os.path.abspath(game_dir)

    if not os.path.isdir(game_dir):
        print(f"refused: frozen game directory missing: {game_dir}", file=sys.stderr)
        return REFUSED

    print(f"validating workload against committed manifests: {game_dir}", file=sys.stderr)
    pre = validate_workload(game_dir)
    if not pre["passed"]:
        for key in ("runtime_source_manifest", "saved_world_manifest"):
            block = pre[key]
            if block["passed"]:
                continue
            print(
                f"refused: {key} mismatch "
                f"(ok {block['ok']}/{block['expected']}, "
                f"missing {len(block['missing'])}, mismatched {len(block['mismatched'])})",
                file=sys.stderr,
            )
            for name in (block["missing"] + block["mismatched"])[:10]:
                print(f"  {name}", file=sys.stderr)
        return REFUSED

    benchmark_capture, capture_identity = load_capture()

    # Scrub BEFORE building the override set, so nothing inherited can survive
    # into the child and the recorded overrides are the whole JANET_/BENDER_ story.
    dropped = scrub_process_environment()
    env_overrides = build_environment(args.cache_root, args.evidence_dir, args.train)

    child_argv = [os.path.abspath(args.executable)]
    if args.script:
        child_argv.append(os.path.abspath(args.script))
    child_argv += [
        "--frames", str(FRAMES),
        "--uncapped",
        "--world-slot", WORLD_SLOT,
        "--world-root", os.path.join(game_dir, "worlds-v1"),
    ]

    host_before = host_snapshot()
    request = benchmark_capture.CaptureRequest(
        argv=tuple(child_argv),
        cwd=game_dir,
        evidence_dir=args.evidence_dir,
        env_overrides=env_overrides,
        label=args.label,
        resource_tool="time",
        timeout=WALL_CAP_S,
    )

    try:
        record = benchmark_capture.capture(request)
    except benchmark_capture.CaptureError as exc:
        print(f"refused: {exc}", file=sys.stderr)
        return REFUSED

    host_after = host_snapshot()

    # Concatenate the two streams for parsing: the game writes its report to
    # stdout, but a diagnostic that explains a missing line goes to stderr, and
    # a parse that ignores stderr reports "line absent" with no clue why.
    evidence = record["evidence_dir"]
    text = ""
    for name in ("stdout.log", "stderr.log"):
        path = os.path.join(evidence, name)
        if os.path.isfile(path):
            with open(path, "r", encoding="utf-8", errors="replace") as handle:
                text += handle.read() + "\n"

    parsed = parse_output(text)

    result = record.get("result") or {}
    exit_status = result.get("exit_status")
    if exit_status is None and result.get("kind") == "signaled":
        exit_status = -(result.get("signal") or 0)
    gated = gate_record(exit_status if exit_status is not None else -1, parsed)

    # The frozen tree must be untouched: a bounded arm must write nothing, and
    # this is the check that proves it rather than asserting it.
    post = validate_workload(game_dir)

    # RSS is a post-hoc verdict, never a mid-run cap: the capture utility
    # exposes only ru_maxrss from wait4, with no live sampling.
    usage = record.get("resource") or {}
    peak_bytes = usage.get("maxrss_bytes")
    rss = {
        "threshold_bytes": RSS_CAP_BYTES,
        "observed_peak_bytes": peak_bytes,
        "enforced_mid_run": False,
        "note": (
            "peak RSS is read from wait4 rusage AFTER the child exits; the "
            "capture utility offers no live sampling, so a breach is detected "
            "here as a verdict, not prevented during the run"
        ),
    }
    if peak_bytes is None:
        rss["verdict"] = "unknown"
    elif peak_bytes > RSS_CAP_BYTES:
        rss["verdict"] = "EXCEEDED"
        gated["failures"].append(
            f"peak_rss: {peak_bytes} bytes exceeds {RSS_CAP_BYTES}"
        )
        gated["passed"] = False
    else:
        rss["verdict"] = "within"

    if record.get("timeout", {}).get("expired"):
        gated["failures"].append(f"wall: exceeded {WALL_CAP_S}s cap (killed)")
        gated["passed"] = False

    profile_path = env_overrides.get("JANET_PROFILE_OUT")
    profile = None
    if profile_path:
        exists = os.path.isfile(profile_path) and os.path.getsize(profile_path) > 0
        profile = {
            "requested": True,
            "path": os.path.basename(profile_path),
            "written": exists,
            "bytes": os.path.getsize(profile_path) if exists else 0,
            "sha256": sha256_file(profile_path) if exists else None,
        }
        if not exists:
            # Say it plainly rather than letting an empty "profile" field imply
            # a profile was collected.
            profile["note"] = (
                "no dump was written; this binary likely lacks profile support, "
                "so this arm is NOT a training arm"
            )
            gated["failures"].append("profile: JANET_PROFILE_OUT produced no dump")
            gated["passed"] = False

    summary = {
        "schema": "janet/run-matched/1",
        "label": args.label,
        "mode": "train" if args.train else "bench",
        "passed": gated["passed"],
        "failures": gated["failures"],
        "game_dir": game_dir,
        "command": record.get("command"),
        "capture_utility": capture_identity,
        "workload": {"before": pre, "after": post},
        "environment": {
            "scrubbed_prefixes": ["JANET_", "BENDER_"],
            "scrubbed_variables_dropped": dropped,
            "overrides": env_overrides,
        },
        "profile": profile,
        "host": {"before": host_before, "after": host_after},
        "resource": {"peak_rss": rss},
        "timing": record.get("timing"),
        "capture_result": record.get("result"),
        "capture_timeout": record.get("timeout"),
        "parsed": parsed,
    }
    summary_path = os.path.join(evidence, "summary.json")
    with open(summary_path, "x", encoding="utf-8") as handle:
        json.dump(summary, handle, indent=2)
        handle.write("\n")

    if gated["passed"]:
        print(f"{args.label}: PASS ({evidence})")
        return 0
    print(f"{args.label}: FAIL ({evidence})", file=sys.stderr)
    for failure in gated["failures"]:
        print(f"  - {failure}", file=sys.stderr)
    return FAILED


if __name__ == "__main__":
    sys.exit(main())
