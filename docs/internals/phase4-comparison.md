# Phase 4: Janet vs Go vs Erlang on identical work

Measured on a 12-core Apple Silicon machine (8 performance + 4 efficiency), 16 GB.
All three programs build a chain of N processes; `0` enters the head, each
process adds 1 and forwards, the sink reports. Every one is verified by its
`result` field: `result == N` in all runs below.

Sources: `bench/wave.janet`, `bench/wave.go`, `bench/wave.erl`.

## Results

| N | Go | Erlang | Janet |
|---|---|---|---|
| 100,000 | **131 ms** | 182 ms | **1,538 ms** |
| 200,000 | — | — | 4,051 ms |
| 400,000 | — | — | 9,357 ms |
| 1,000,000 | **2,430 ms** | **1,869 ms** | not completed in 600 s |

## The answer to "does Janet beat Go on the wave?"

**No.** At 100k Janet is ~12x slower than Go and ~8x slower than Erlang, and it
gets worse with N because Janet's curve is superlinear while both others are
linear:

```
Janet:   100k 15.4 us/hop   200k 20.3 us/hop   400k 23.4 us/hop
         2x N -> 2.3-2.6x time  (linear would be 2.0x)
Go:      100k -> 1M is 18.5x for 10x N   (linear)
Erlang:  100k -> 1M is 10.3x for 10x N   (linear)
```

Per-hop, Janet spends ~15 us where Go spends ~1.3 us and Erlang ~1.9 us.

## Why this matters, and what it does NOT mean

This benchmark is deliberately hostile to Janet: it is a **sequential
dependency chain**, so no amount of lanes helps (Go and Erlang also spend
roughly one core's worth of time on it). It measures **per-message latency
through a process chain**, not parallel throughput.

That distinction is real and measured:

| workload | Janet | notes |
|---|---|---|
| chain of N (this benchmark) | ~12x slower than Go | latency-bound, single chain |
| independent fan-out (earlier phase) | 6.6-7.4x faster than sequential | uses all lanes |
| trivial units | 8.2 M units/sec | throughput-bound |

So "we beat Go" is true for parallel throughput and false for per-message
latency. Both statements are needed.

## What is actually slow (candidates, not yet proven)

1. **~15 us per hop.** A channel round-trip measured 0.17 us, so the cost is not
   messaging itself. Each hop goes proc/recv -> proc/send, i.e. two mailbox
   operations plus two fiber resumes, so the per-resume cost dominates. That is
   the number to attack.
2. **Superlinear growth.** 2x N costs 2.3-2.6x time, which points at something
   scanning with N (a full-ish collection, or the per-fiber minor touching a
   growing live set). Not yet isolated.
3. **1M does not finish.** At 100k the live set is ~130 MB; at 1M it is ~1.3 GB
   plus the run's working set, and this machine had only ~1-1.5 GB free with
   heavy swap in use. The earlier 1M toolchain measurement (1.31 GB for 1M procs)
   suggests it may be memory pressure rather than an algorithmic wall, but that
   is unproven here.

## Method notes (learn from earlier mistakes)

- min-of-3 for every arm; single runs on this box have been misleading three
  times already.
- `result == N` checked on every run, so a fast-but-wrong result cannot pass.
- Machine state was NOT clean: load average ~4.5 and swap in use throughout.
  Go and Erlang arms are therefore also inflated, so the RATIOS are more
  trustworthy than the absolute numbers. A clean-machine rerun is owed before
  quoting absolutes.
