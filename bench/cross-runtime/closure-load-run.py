#!/usr/bin/env python3
"""Measure already-built boxed/native capturing loops, serial alternating pairs.
Usage: python3 closure-load-run.py BOXED NATIVE OUTDIR [N ROUNDS]
Build and entry-counter acceptance commands are documented in closure-load.md.
"""
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import time

boxed, native, dest = Path(sys.argv[1]).resolve(), Path(sys.argv[2]).resolve(), Path(sys.argv[3])
n, rounds = map(int, sys.argv[4:6]) if len(sys.argv) > 4 else (10000, 100)
assert 1 <= n <= 1000000 and 1 <= rounds <= 1000 and n * rounds <= 10000000
# A unique directory prevents accidental deletion/overwriting of prior evidence.
dest.mkdir(parents=True, exist_ok=False)
expected = f"n={n} rounds={rounds} checksum={rounds * (n * (n - 1) + 10 * n)}\n"
env = {k: v for k, v in os.environ.items() if not k.startswith(('BENDER_', 'JANET_'))}
rows = []
for rep in range(3):
    state = {}
    for cmd in (['/usr/sbin/sysctl', '-n', 'vm.swapusage'], ['/usr/bin/uptime']):
        q = subprocess.run(cmd, capture_output=True, text=True, timeout=10)
        state[' '.join(cmd)] = q.stdout.strip()
    for tag, exe in ([('boxed', boxed), ('native', native)] if rep % 2 == 0
                     else [('native', native), ('boxed', boxed)]):
        argv = [str(exe), str(n), str(rounds)]
        start = time.monotonic()
        # Direct files cannot deadlock on full stdout/stderr pipes. Timeout kills
        # this direct AOT child only; this workload does not spawn processes.
        with (dest / f'{rep}-{tag}.out').open('w') as out, (dest / f'{rep}-{tag}.err').open('w') as err:
            p = subprocess.run(argv, env=env, stdout=out, stderr=err, timeout=120)
        elapsed = time.monotonic() - start
        assert p.returncode == 0, (argv, p.returncode)
        assert (dest / f'{rep}-{tag}.out').read_text() == expected
        stderr = (dest / f'{rep}-{tag}.err').read_text()
        phase = re.search(r'work_ms=([\d.]+)', stderr)
        assert phase, stderr
        row = dict(rep=rep, tag=tag, argv=argv, exit=p.returncode,
                   wall_ms=elapsed * 1000, work_ms=float(phase[1]),
                   sha256=hashlib.sha256(exe.read_bytes()).hexdigest(),
                   bytes=exe.stat().st_size, stdout=expected, machine=state)
        rows.append(row)
        (dest / 'results.json').write_text(json.dumps(rows, indent=2))
        print(json.dumps(row), flush=True)
