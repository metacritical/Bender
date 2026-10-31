#!/usr/bin/env python3
"""Incremental trampoline-wave benchmark across runtimes.

Four arms, each building a chain of N units where unit i receives a value,
adds 1 and forwards it to unit i+1; the sink reports the final value, which
must equal N:

  janet-fibers  ev/go + ev/chan        (VM-local, single VM by construction)
  janet-procs   proc/spawn + mailbox   (BENDER_LANES lanes)
  erlang        spawn + receive/!
  go            goroutine + channel

Every arm is a DEPENDENCY CHAIN: one hop at a time. It measures spawn cost and
per-hop handoff/scheduling, never parallel speedup, so a faster arm here is not
"more parallel".

Each run is launched sequentially with explicit caps. The measured process is
the runtime itself, and only its own process group is signalled on a breach.
A run counts as a result ONLY when it exits 0, prints result == N, and did not
breach a cap; otherwise it is recorded as timeout/cap/error and the arm is
skipped at larger sizes rather than escalated.

Usage:
  python3 run-wave-scale.py --plan
  python3 run-wave-scale.py --execute [--sizes 10000,20000] [--wall 120]
"""

from __future__ import annotations

import argparse
import json
import os
import pathlib
import re
import signal
import subprocess
import time

HERE = pathlib.Path(__file__).resolve().parent
REPO = HERE.parent.parent
OUT = HERE / "wave-scale-results.json"

DEFAULT_SIZES = [10_000, 20_000, 200_000, 400_000, 500_000, 1_000_000]
WALL_CAP_S = 120.0
RSS_CAP_KIB = int(os.environ.get("WAVE_RSS_CAP_KIB", 5 * 1024 * 1024))  # default 5 GiB
SAMPLE_S = 0.25
LANES = os.environ.get("BENDER_LANES", "11")

RE_RESULT = re.compile(r"\bresult=(\d+)")
RE_PHASE = re.compile(
    r"spawn_ms=(?P<spawn>[\d.]+)\s+wave_ms=(?P<wave>[\d.]+)\s+total_ms=(?P<total>[\d.]+)")


def arms(bender: pathlib.Path) -> list[dict]:
    return [
        {"tag": "janet-fibers", "argv": [str(bender), str(HERE / "wave-fiber.ben")],
         "env": {"BENDER_NO_PROFILE": "1"}, "unit": "fiber"},
        {"tag": "janet-procs", "argv": [str(bender), str(HERE / "wave.ben")],
         "env": {"BENDER_NO_PROFILE": "1", "BENDER_LANES": LANES}, "unit": "proc"},
        {"tag": "erlang",
         "argv": ["erl", "-noshell", "+P", "8000000", "-pa", str(HERE),
                  "-eval"], "unit": "process", "erlang": True},
        {"tag": "go", "argv": [str(HERE / "wave-go")], "unit": "goroutine"},
    ]


def build_env(extra: dict) -> dict:
    env = {k: v for k, v in os.environ.items()
           if not k.startswith(("JANET_", "BENDER_"))}
    env.update(extra)
    return env


def run_one(arm: dict, n: int, wall_cap: float) -> dict:
    if arm.get("erlang"):
        cmd = arm["argv"] + [f'wave:main(["{n}"]),halt().']
    else:
        cmd = arm["argv"] + [str(n), "--uncapped"][:1]
    env = build_env(arm.get("env", {}))
    started = time.monotonic()
    peak = 0
    reason = None
    proc = subprocess.Popen(cmd, cwd=str(HERE), env=env,
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                            text=True, start_new_session=True)
    while proc.poll() is None:
        q = subprocess.run(["/bin/ps", "-o", "rss=", "-p", str(proc.pid)],
                           capture_output=True, text=True).stdout.strip()
        if q:
            peak = max(peak, int(q))
        if peak > RSS_CAP_KIB:
            reason = f"sampled RSS > {RSS_CAP_KIB} KiB"
        elif time.monotonic() - started > wall_cap:
            reason = f"wall > {wall_cap:g}s"
        if reason:
            os.killpg(os.getpgid(proc.pid), signal.SIGKILL)
            break
        try:
            proc.wait(timeout=SAMPLE_S)
        except subprocess.TimeoutExpired:
            pass
    out, err = proc.communicate()
    rc = proc.returncode
    wall = time.monotonic() - started
    text = f"{out}\n{err}"
    mr = RE_RESULT.search(text)
    mp = RE_PHASE.search(text)
    result = int(mr[1]) if mr else None
    row = {
        "tag": arm["tag"], "unit": arm["unit"], "n": n, "argv": cmd,
        "exit": rc, "termination_reason": reason,
        "wall_s": round(wall, 3), "peak_sampled_rss_kib": peak,
        "result": result, "correct": result == n,
        "spawn_ms": float(mp["spawn"]) if mp else None,
        "wave_ms": float(mp["wave"]) if mp else None,
        "total_ms": float(mp["total"]) if mp else None,
        "stderr_tail": err.strip().splitlines()[-3:] if err.strip() else [],
    }
    row["ok"] = rc == 0 and reason is None and row["correct"]
    return row


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--bender", default=str(REPO / "build" / "bender"))
    ap.add_argument("--sizes", default=",".join(str(s) for s in DEFAULT_SIZES))
    ap.add_argument("--wall", type=float, default=WALL_CAP_S)
    ap.add_argument("--out", default=str(OUT))
    ap.add_argument("--plan", action="store_true")
    ap.add_argument("--execute", action="store_true")
    args = ap.parse_args()

    bender = pathlib.Path(args.bender)
    sizes = [int(s) for s in args.sizes.split(",") if s.strip()]
    plan = arms(bender)

    print(f"bender: {bender} (exists={bender.is_file()})")
    print(f"sizes: {sizes}")
    print(f"caps: wall {args.wall:g}s, sampled RSS {RSS_CAP_KIB} KiB, lanes {LANES}")
    for a in plan:
        print(f"  arm {a['tag']:<13} {' '.join(a['argv'])} <n>")
    if not args.execute:
        print("\n--plan only: nothing launched.")
        return 0

    # Append to existing results: a sweep split across several invocations
    # previously overwrote the file and discarded the earlier sizes.
    out_path = pathlib.Path(args.out)
    rows: list[dict] = []
    if out_path.is_file():
        try:
            rows = json.loads(out_path.read_text())
        except (OSError, ValueError):
            rows = []
    # Arms that failed in a PREVIOUS invocation are unknown here, so each
    # invocation re-tests them. Deliberate (a cap breach can come from machine
    # load) but it means a repeat failure is re-measured, never assumed.
    dead: set[str] = set()
    for n in sizes:
        for a in plan:
            if a["tag"] in dead:
                rows.append({"tag": a["tag"], "n": n, "skipped":
                             "previous size did not complete", "ok": False})
                print(f"{a['tag']:<13} n={n:<8} SKIPPED (earlier size failed)")
                continue
            row = run_one(a, n, args.wall)
            rows.append(row)
            if row["ok"]:
                print(f"{row['tag']:<13} n={n:<8} total={row['total_ms']:>9.0f}ms "
                      f"spawn={row['spawn_ms']:>8.0f} wave={row['wave_ms']:>9.0f} "
                      f"rss={row['peak_sampled_rss_kib']:>8}KiB OK")
            else:
                why = row.get("termination_reason") or f"exit {row['exit']}"
                print(f"{row['tag']:<13} n={n:<8} FAILED ({why}, result={row['result']})")
                dead.add(a["tag"])
            pathlib.Path(args.out).write_text(json.dumps(rows, indent=2))
    pathlib.Path(args.out).write_text(json.dumps(rows, indent=2))
    print(f"\nwrote {args.out}")
    return 0 if all(r.get("ok") for r in rows) else 1


if __name__ == "__main__":
    raise SystemExit(main())
