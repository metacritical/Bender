# Incremental trampoline wave: Janet fibers, Janet procs, Erlang, Go

Chain of N units; unit i receives a value, adds 1, forwards to unit i+1; the
sink reports the final value. **Every completed run returned result == N.**

A dependency chain, one hop at a time: this measures spawn cost plus per-hop
handoff and scheduling. It is NOT a parallel-throughput test, so a faster arm
here is not "more parallel".

## Read per-unit cost, never cross-N totals

**Comparing a 500,000-unit total against a 1,000,000-unit total is meaningless**
and an earlier version of this file did exactly that. Only the normalized
columns below are comparable, and even those carry the semantic caveats in
"What is not comparable".

| N | janet-fibers | janet-procs | erlang | go |
|---|---|---|---|---|
| 1,000 | 178 us | 10 us | 2.0 us | 1.0 us |
| 10,000 | 183 us | 11 us | 1.9 us | 1.0 us |
| 20,000 | 171 us | 11 us | 2.1 us | 1.2 us |
| 200,000 | **crash** | 26 us | 1.8 us | 1.3 us |
| 400,000 | cap kill | 37 us | 1.5 us | 1.0 us |
| 500,000 | not retried | 75 us | 1.6 us | 1.1 us |
| 1,000,000 | cap kill | cap kill | 4.4 us | 5.3 us |

Totals, for the record only: janet-procs 500,000 = 37,423 ms (spawn 32,937 ms);
erlang 1,000,000 = 4,445 ms; go 1,000,000 = 5,303 ms.

## What the per-unit numbers show

**Erlang and Go stay roughly flat** at 1-2 us per unit through 500,000, rising
to 4-5 us at 1,000,000, and both complete 1,000,000 correctly.

**Janet procs are correct but degrade with the live set**: 11 us at 20,000 to
75 us at 500,000, spawn-dominated (32,937 ms of 37,423 ms at 500,000). An
earlier independent measurement puts spawn at a flat ~3 us/proc up to 20,000
and ~460 us/proc at 1,000,000, so the 66 us/proc spawn seen here at 500,000
sits on an already-known superlinear curve rather than being a new regression.

**Janet fibers are the worst per hop** (~175 us) and **flat, not quadratic**,
across 1,000-20,000 — so the earlier "quadratic fiber wave" description is not
what this harness measures.

## Two distinct failures, not one

These must not be merged:

| Arm | N | Exit | Meaning |
|---|---|---|---|
| janet-fibers | 200,000 | **-11 SIGSEGV** | **a real crash** — a native defect to bisect |
| janet-fibers | 400,000 | -9, wall > 180 s | harness cap kill, RSS only 411,120 KiB |
| janet-fibers | 1,000,000 | -9, wall > 300 s | harness cap kill, RSS 570,400 KiB |
| janet-procs | 1,000,000 | -9, wall > 300 s | harness cap kill, RSS 2,856,928 KiB |

A signal death from the harness (-9) is a cap, not a result. A SIGSEGV (-11) is
a bug. The fiber arm's 200,000 result is **"crashes at 200,000"**, NOT "fibers
do not scale", and no fiber scaling claim should be published before that crash
is bisected on a debug/ASan build.

## The 1,000,000 Janet rows prove nothing about the runtime

My 300 s wall cap was **too small to answer the question**. The wall cap fired
in both Janet arms; sampled RSS at kill was 2.79 GiB for procs, **under** the
6 GiB RSS cap, so memory did not stop them. Against an independently measured
~430-460 s for 1,000,000 spawns *alone*, a 300 s budget guarantees a miss.

To make the 1,000,000 question answerable: wall >= 900 s, RSS cap >= 8 GiB, on
an idle machine, and record which cap fired. Until then these rows are
harness artifacts.

One unresolved discrepancy: a prior analysis reports ~27-36 KB per live proc
(implying ~27 GB for an unsegmented 1,000,000 chain), while this sweep measured
2.33 GiB at 500,000 (~4.9 KB/proc) and 2.79 GiB at the 1,000,000 kill. Those
differ by roughly 6x. **Do not quote either figure as settled** until the
configurations are reconciled.

## What is not comparable

- **`spawn_ms` measures different work in each arm.** `wave.ben` routes
  `proc/spawn` through a lane (closure marshal + ctl message + serial),
  `wave-fiber.ben` uses VM-local `ev/go`, Erlang copies arguments, Go allocates
  a goroutine. Treat the spawn column as within-arm scaling only.
- **Synchronization differs per arm.** `wave.go:41` uses `make(chan int)`, an
  unbuffered true rendezvous per hop. `wave-fiber.ben` uses `(ev/chan 1)` and
  `wave.ben` a mailbox of capacity 8, so the Janet arms can let a sender run
  ahead. Go is the only arm paying full rendezvous cost, which **inflates the
  Janet arms' apparent wave advantage**. Erlang `!` is asynchronous.
- **Single sample per cell**, fixed arm order, no repetition or alternation, and
  the machine carried 12-15 GiB of swap throughout. An earlier attempt saw the
  same code move 4.4-9.5x with machine state alone, so differences under about
  that factor are not safely attributable to the runtime.
- **Sampled RSS is unreliable for Erlang**: `erl` is a wrapper that execs
  `beam.smp`, and the sampler reads only the leader PID. Erlang timings come
  from the runtime's own monotonic clock and are sound; its memory figures are
  indicative only.
- The runner does **not** rebuild `wave.beam` / `wave-go`, so a stale artifact
  can silently be measured. Rebuild both before trusting a sweep.

## Reproduce

```sh
cd bench/cross-runtime && erlc wave.erl && go build -o wave-go wave.go
python3 run-wave-scale.py --plan
python3 run-wave-scale.py --execute --sizes 10000,20000,200000 --wall 120
python3 run-wave-scale.py --execute --sizes 400000,500000 --wall 180
WAVE_RSS_CAP_KIB=8388608 python3 run-wave-scale.py --execute --sizes 1000000 --wall 900
```

Keep `BENDER_NO_PROFILE=1` on every Janet arm — the runner sets it. Profiling-on
runs were measured elsewhere as up to 2.9x slower on this chain and ~2000x on
process teardown, and were the direct cause of a false "quadratic" conclusion in
an earlier attempt.

AOT/PGO optimization of these harnesses is the next phase; this is the
interpreter baseline the compiler work has to improve on.
