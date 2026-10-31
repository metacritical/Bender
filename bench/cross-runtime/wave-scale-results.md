# Incremental trampoline wave: Janet fibers, Janet procs, Erlang, Go

Chain of N units; unit i receives a value, adds 1, forwards to unit i+1; the
sink reports the final value. **Every completed run returned result == N.**

A dependency chain, one hop at a time: this measures spawn cost plus per-hop
handoff and scheduling. It is NOT a parallel-throughput test, so a faster arm
here is not "more parallel".

Runner `run-wave-scale.py`, sequential, per-run caps on wall time and sampled
RSS of the launched process, only that process group signalled on breach.
Janet procs used `BENDER_LANES=11`; Janet fibers are VM-local by construction.
Machine was under heavy memory pressure throughout (12-15 GiB swap in use).

## Total wall time

| N | janet-fibers | janet-procs | erlang | go |
|---|---|---|---|---|
| 1,000 | 178 ms | 10 ms | 2 ms | 1 ms |
| 10,000 | 1,832 ms | 112 ms | 19 ms | 10 ms |
| 20,000 | 3,420 ms | 227 ms | 41 ms | 24 ms |
| 200,000 | **SIGSEGV** | 5,186 ms | 365 ms | 264 ms |
| 400,000 | timeout >180 s | 14,797 ms | 607 ms | 416 ms |
| 500,000 | not retried | 37,423 ms | 783 ms | 564 ms |
| 1,000,000 | timeout >300 s | timeout >300 s | **4,445 ms** | **5,303 ms** |

## Per-hop cost, completed runs

| N | janet-fibers | janet-procs | erlang | go |
|---|---|---|---|---|
| 1,000 | 178 us | 10 us | 2.0 us | 1.0 us |
| 10,000 | 183 us | 11 us | 1.9 us | 1.0 us |
| 20,000 | 171 us | 11 us | 2.1 us | 1.2 us |
| 200,000 | — | 26 us | 1.8 us | 1.3 us |
| 400,000 | — | 37 us | 1.5 us | 1.0 us |
| 500,000 | — | 75 us | 1.6 us | 1.1 us |
| 1,000,000 | — | — | 4.4 us | 5.3 us |

## What the numbers show

**Erlang and Go both complete 1,000,000 hops correctly**, at 4.4 s and 5.3 s.
Their per-hop cost stays roughly flat (1-2 us) up to 500,000 and rises to
4-5 us at 1,000,000.

**Janet procs are correct but far slower and do not reach 1,000,000 here.**
Per-hop cost grows from 11 us to 75 us between 20,000 and 500,000, and spawn
dominates: at 500,000 spawn was 32,937 ms of the 37,423 ms total. At
1,000,000 the process was still running at the 300 s cap with sampled RSS
collapsed to about 1.9 MB, i.e. swapped out rather than computing.

**Janet fibers crash.** At 200,000 the arm died with exit -11 (SIGSEGV) after
completing 20,000, and at 400,000 it exceeded the wall cap. Fiber per-hop cost
is also the worst measured, about 175 us, and it is flat rather than quadratic
across 1,000-20,000 -- so the earlier "quadratic fiber wave" description is not
what this harness measures.

## Caveats, explicitly

- **The 1,000,000 Janet failures are cap breaches on a loaded machine**, not a
  proof of impossibility. Swap was already 12-15 GiB in use. They should be
  re-run on an idle machine before any "cannot do 1M" claim.
- **The SIGSEGV is a real crash and needs a separate diagnosis.** Exit -11 at
  200,000 fibers is reproducible evidence of a defect, but its cause (stack
  growth, channel allocation, GC) has NOT been established. Do not attribute it.
- **Sampled RSS is unreliable for the Erlang arm**: `erl` is a wrapper that
  execs `beam.smp`, so the sampled PID is not always the runtime. Treat Erlang
  memory figures here as indicative only; the timings are from the runtime's own
  monotonic clock and are sound.
- **Per-hop figures are derived** (total / N) from single runs per cell, with no
  repetition, alternation, or thermal control. Ordering is fixed, so later arms
  in a size row ran on a slightly different machine state.
- Earlier invocations of the runner **overwrote** `wave-scale-results.json`
  instead of appending, so that file holds only the final batch; the table above
  is transcribed from the runner's own per-run console output. The append fix is
  in this commit.

## Reproduce

```sh
cd bench/cross-runtime && erlc wave.erl && go build -o wave-go wave.go
python3 run-wave-scale.py --plan
python3 run-wave-scale.py --execute --sizes 10000,20000,200000 --wall 120
python3 run-wave-scale.py --execute --sizes 400000,500000 --wall 180
WAVE_RSS_CAP_KIB=6291456 python3 run-wave-scale.py --execute --sizes 1000000 --wall 300
```

AOT/PGO optimization of these harnesses is the next phase and is not covered
here; this is the interpreter baseline the compiler work has to improve on.
