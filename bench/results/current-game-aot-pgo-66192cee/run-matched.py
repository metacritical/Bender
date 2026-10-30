#!/usr/bin/env python3
"""Run ONE matched arm of the frozen current-game workload and record evidence.

  run-matched.py --label L --executable BIN [--script GAME] \
                 --evidence-dir NEW_DIR --cache-root DIR [--dry-run]

Exit 0 pass | 1 gate failed (field named) | 2 refused (nothing spawned).
Capture is imported from tools/benchmark_capture and pinned by sha256, not
reimplemented. Nothing writes into the frozen tree; pre- and post-run manifest
checks prove the arm only read it. No profiling arm: --native cannot compile.
"""

import argparse, hashlib, json, os, platform, re, sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
sys.path.insert(0, os.path.join(REPO, "tools"))
import benchmark_capture  # noqa: E402

REF = os.path.join(REPO, "bench/results/current-game-6a4f008b-c07042a7")
RUNTIME_MANIFEST = os.path.join(REF, "runtime-source-manifest.json")
WORLD_MANIFEST = os.path.join(REF, "saved-world-manifest.json")
GAME_DIR = os.path.join(REPO, "build/current-game-aot-pgo-66192cee/game-source")

# Saved-world paths are relative to the SLOT dir; anchored at worlds-v1, all
# 214 entries report missing.
SLOT = "15f2268da8ec2a4105ef2deaf6ae0b19"
FRAMES, WALL_CAP_S, RSS_CAP = 6000, 180.0, 1 << 30
SCENE = {"radius": 16, "loaded": 64, "drawn": 64, "wanted": 64,
         "streamed_in": 0, "streamed_out": 0}

# The game's own report lines. Startup is kept apart from frame statistics:
# wall time is not frame time.
PATTERNS = {
    "frames": (r"^frames:\s*(\d+)\s+avg\s+([\d.]+)\s+FPS\s+\(([\d.]+)\s*ms\)\s+best\s+"
               r"([\d.]+)\s*ms\s+worst\s+([\d.]+)\s*ms\s+spikes\s*>\s*33ms:\s*(\d+)\s+"
               r"\(([\d.]+)%\)\s+edits:\s*(\d+)\s+(.+?)\s*$",
               ("frames", "avg_fps", "avg_ms", "best_ms", "worst_ms", "spikes",
                "spike_pct", "edits", "cap_mode"),
               {"frames", "spikes", "edits"}),
    "scene": (r"^scene:\s+radius\s+(\d+)\s+loaded\s+(\d+)\s+drawn\s+(\d+)\s+wanted\s+"
              r"(\d+)\s+streamed in\s+(\d+)\s+out\s+(\d+)(?:\s+\(([^)]*)\))?\s*$",
              ("radius", "loaded", "drawn", "wanted", "streamed_in", "streamed_out",
               "culling"),
              {"radius", "loaded", "drawn", "wanted", "streamed_in", "streamed_out"}),
    "steady": (r"^steady:\s+([\d.]+)\s+FPS\s+\(([\d.]+)\s*ms\)\s+over\s+(\d+)\s+"
               r"frames,\s+reached at frame\s+(\d+)\s*$",
               ("fps", "ms", "frames", "reached_at_frame"), {"frames", "reached_at_frame"}),
    "worst": (r"^worst frame:\s+#(\d+)\s+([\d.]+)\s*ms wall,\s+([\d.]+)\s*ms CPU\s+"
              r"\((\d+)%\s+busy\),\s+mesh queue\s+(\d+)\s*$",
              ("frame", "wall_ms", "cpu_ms", "busy_pct", "mesh_queue"),
              {"frame", "busy_pct", "mesh_queue"}),
    "worldgen": (r"^worldgen:\s+(\d+)\s+spawn chunks in\s+([\d.]+)\s*ms(.*)$",
                 ("chunks", "ms", "source"), {"chunks"}),
}


def sha256_file(path):
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _value(name, raw, ints):
    """One captured group, typed. An absent optional group is None, not 'None'."""
    if raw is None:
        return None
    if name in ints:
        return int(raw)
    if name in ("cap_mode", "culling", "source"):
        return raw.strip() or None
    return float(raw)


def parse_output(text):
    """Parse the game's report lines into independent, separately-gated fields."""
    found = {}
    for line in text.splitlines():
        for key, (pattern, names, ints) in PATTERNS.items():
            if key in found:
                continue
            match = re.match(pattern, line.rstrip())
            if match:
                found[key] = {name: _value(name, raw, ints)
                              for name, raw in zip(names, match.groups())}
    return {"startup": {"worldgen": found.pop("worldgen", None)},
            "frames": found.get("frames"), "scene": found.get("scene"),
            "steady": found.get("steady"), "worst_frame": found.get("worst")}


def verify(root, entries):
    bad = [e["path"] for e in entries if not os.path.isfile(os.path.join(root, e["path"]))]
    wrong = [e["path"] for e in entries
             if os.path.isfile(os.path.join(root, e["path"]))
             and sha256_file(os.path.join(root, e["path"])) != e["sha256"]]
    return {"expected": len(entries), "ok": len(entries) - len(bad) - len(wrong),
            "missing": bad, "mismatched": wrong, "passed": not bad and not wrong}


def validate(game_dir):
    with open(RUNTIME_MANIFEST, encoding="utf-8") as h:
        runtime = verify(game_dir, json.load(h))
    with open(WORLD_MANIFEST, encoding="utf-8") as h:
        world = verify(os.path.join(game_dir, "worlds-v1", SLOT), json.load(h))
    return {"runtime": runtime, "world": world,
            "passed": runtime["passed"] and world["passed"]}


def gate(exit_status, parsed):
    """Name every failing field; a bare False is not a usable verdict."""
    out = []
    if exit_status != 0:
        out.append(f"exit_status: expected 0, got {exit_status!r}")
    frames = parsed["frames"]
    if frames is None:
        out.append("frames: line absent (the game never reported)")
    else:
        if frames["frames"] != FRAMES:
            out.append(f"frames: reached {frames['frames']} of {FRAMES}")
        if frames["cap_mode"] != "uncapped":
            out.append(f"cap_mode: expected 'uncapped', got {frames['cap_mode']!r}")
    if parsed["scene"] is None:
        out.append("scene: line absent; identity is unproven")
    else:
        out += [f"scene.{k}: expected {v}, got {parsed['scene'].get(k)!r}"
                for k, v in SCENE.items() if parsed["scene"].get(k) != v]
    if parsed["steady"] is None:
        out.append("steady: line absent; the stream never reached steady state")
    return {"passed": not out, "failures": out}


def host_snapshot():
    """Load only: no paths, usernames or argv, so nothing private is recorded."""
    try:
        load = os.getloadavg()
    except OSError:
        load = (None, None, None)
    return {"loadavg": dict(zip(("1m", "5m", "15m"), load)),
            "cpu_count": os.cpu_count(), "machine": platform.machine()}


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--label", required=True)
    p.add_argument("--executable", required=True)
    p.add_argument("--script", default=None, help="omit for an AOT binary")
    p.add_argument("--evidence-dir", required=True, help="new directory")
    p.add_argument("--cache-root", required=True, help="XDG_CACHE_HOME")
    p.add_argument("--game-dir", default=GAME_DIR)
    p.add_argument("--dry-run", action="store_true",
                   help="print the plan and exit without spawning")
    args = p.parse_args(argv)

    game_dir = os.path.abspath(args.game_dir)
    if not os.path.isdir(game_dir):
        print(f"refused: frozen game missing: {game_dir}", file=sys.stderr)
        return 2
    before = validate(game_dir)
    if not before["passed"]:
        for name in ("runtime", "world"):
            block = before[name]
            if not block["passed"]:
                print(f"refused: {name} manifest mismatch (ok {block['ok']}/"
                      f"{block['expected']}, missing {len(block['missing'])}, "
                      f"mismatched {len(block['mismatched'])})", file=sys.stderr)
                for path in (block["missing"] + block["mismatched"])[:10]:
                    print(f"  {path}", file=sys.stderr)
        return 2

    # A warm cache makes two arms unequal: the second reads what the first wrote,
    # so the comparison measures cache state rather than the runtime. Require the
    # root to be absent or empty; checked BEFORE any spawn so refusal is free.
    cache_root = os.path.abspath(args.cache_root)
    prior = []
    if os.path.exists(cache_root):
        if not os.path.isdir(cache_root):
            print(f"refused: cache root is not a directory: {cache_root}",
                  file=sys.stderr)
            return 2
        prior = sorted(os.listdir(cache_root))
        if prior:
            print(f"refused: cache root is not fresh: {cache_root} already holds "
                  f"{len(prior)} entr{'y' if len(prior) == 1 else 'ies'}",
                  file=sys.stderr)
            for name in prior[:10]:
                print(f"  {name}", file=sys.stderr)
            return 2

    # Scrub inherited JANET_/BENDER_ from THIS process: capture merges overrides
    # over the environment, so deletion is the only way to guarantee absence.
    dropped = sorted(k for k in os.environ if k.startswith(("JANET_", "BENDER_")))
    for key in dropped:
        del os.environ[key]
    overrides = {"JANET_LANES": "0", "JANET_NO_PROFILE": "1",
                 "XDG_CACHE_HOME": cache_root}

    child = [os.path.abspath(args.executable)]
    if args.script:
        child.append(os.path.abspath(args.script))
    child += ["--frames", str(FRAMES), "--uncapped", "--world-slot", SLOT,
              "--world-root", os.path.join(game_dir, "worlds-v1")]

    if args.dry_run:
        print(json.dumps({
            "dry_run": True, "label": args.label, "argv": child, "cwd": game_dir,
            "environment_overrides": overrides, "scrubbed_names": dropped,
            "wall_cap_seconds": WALL_CAP_S, "rss_threshold_bytes": RSS_CAP,
            "rss_enforced_mid_run": False,
            "gates": {"frames": FRAMES, "scene": SCENE},
            "workload_passed": before["passed"],
            "cache_root": cache_root, "cache_prior_entries": len(prior),
            "cache_fresh": not prior,
            "executable_sha256": (sha256_file(child[0])
                                  if os.path.isfile(child[0]) else None)}, indent=2))
        return 0

    host_before = host_snapshot()
    try:
        record = benchmark_capture.capture(benchmark_capture.CaptureRequest(
            argv=tuple(child), cwd=game_dir, evidence_dir=args.evidence_dir,
            env_overrides=overrides, label=args.label, resource_tool="time",
            timeout=WALL_CAP_S))
    except benchmark_capture.CaptureError as exc:
        print(f"refused: {exc}", file=sys.stderr)
        return 2
    host_after = host_snapshot()

    text = ""
    for name in ("stdout.log", "stderr.log"):
        path = os.path.join(record["evidence_dir"], name)
        if os.path.isfile(path):
            with open(path, encoding="utf-8", errors="replace") as h:
                text += h.read() + "\n"
    parsed = parse_output(text)

    result = record.get("result") or {}
    status = result.get("exit_status")
    if status is None and result.get("kind") == "signaled":
        status = -(result.get("signal") or 0)
    verdict = gate(-1 if status is None else status, parsed)

    # Post-run: prove the arm wrote nothing into the frozen tree. A changed file
    # is a failed arm, and the failure names it -- "changed" alone would hide
    # which file the run mutated.
    after = validate(game_dir)
    if not after["passed"]:
        changed = []
        for name in ("runtime", "world"):
            block = after[name]
            changed += block["missing"] + block["mismatched"]
        verdict["failures"].append(
            f"frozen tree changed during the arm: {len(changed)} file(s): "
            + ", ".join(changed[:5]) + (" ..." if len(changed) > 5 else ""))
        verdict["passed"] = False

    # Peak RSS comes from wait4 AFTER exit; there is no live sampling, so this
    # is a verdict on the observed peak, never a mid-run cap.
    peak = (record.get("resource") or {}).get("maxrss_bytes")
    rss = {"threshold_bytes": RSS_CAP, "observed_peak_bytes": peak,
           "enforced_mid_run": False,
           "verdict": "unknown" if peak is None
                      else "EXCEEDED" if peak > RSS_CAP else "within"}
    if rss["verdict"] == "EXCEEDED":
        verdict["failures"].append(f"peak_rss: {peak} exceeds {RSS_CAP}")
        verdict["passed"] = False
    if record.get("timeout", {}).get("expired"):
        verdict["failures"].append(f"wall: exceeded {WALL_CAP_S}s cap (killed)")
        verdict["passed"] = False

    with open(os.path.join(record["evidence_dir"], "summary.json"), "x",
              encoding="utf-8") as h:
        json.dump({"schema": "janet/run-matched/1", "label": args.label,
                   "passed": verdict["passed"], "failures": verdict["failures"],
                   "game_dir": game_dir, "command": record.get("command"),
                   "capture_utility": {"sha256": sha256_file(benchmark_capture.__file__)},
                   "workload": {"before": before, "after": after},
                   "environment": {"scrubbed_names": dropped, "overrides": overrides},
                   "cache": {"root": cache_root, "prior_entries": len(prior),
                             "fresh_at_start": not prior},
                   "host": {"before": host_before, "after": host_after},
                   "resource": {"peak_rss": rss}, "timing": record.get("timing"),
                   "capture_timeout": record.get("timeout"), "parsed": parsed},
                  h, indent=2)
        h.write("\n")

    if verdict["passed"]:
        print(f"{args.label}: PASS ({record['evidence_dir']})")
        return 0
    print(f"{args.label}: FAIL ({record['evidence_dir']})", file=sys.stderr)
    for failure in verdict["failures"]:
        print(f"  - {failure}", file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
