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

## ROOT CAUSE FOUND: cross-lane hops cost two poll cycles

The 15us/hop is not marshalling, not GC, and not chain length. Measured
separately with `bench/lane-rt.janet` and `bench/hops.janet`:

| operation | cost |
|---|---|
| marshal a value | 0.12 us |
| marshal + unmarshal | 0.14 us |
| same-VM channel round trip | 0.17 us |
| **cross-lane round trip** | **6.9-7.4 us** |

Data movement is ~0.14 us. The whole penalty is **6.9 us of poll-wakeup
latency**: a cross-lane message must wake the target VM's event loop (self-pipe
-> kevent) and the reply must wake the sender again. `sample` during the run
shows the lane threads parked in `kevent`.

Two hypotheses were tested and REJECTED:

1. **"Live process count makes minors slow."** Hammering a 2-proc chain 20000
   times with 0, 50000, 100000 and 200000 idle live procs gave 8.34, 7.79, 7.57
   and 7.99 us/hop -- flat. Live set size is irrelevant.
2. **"The chain length is the problem."** A 2-proc chain costs the same per hop
   as an N-proc chain, so the earlier superlinear curve was the live-set growth
   of the BUILD phase, not traversal.

Also measured: lanes off is 0.46 us/hop vs 7.4 us with lanes on, i.e. the
cross-lane penalty is ~16x and is FLAT across 2, 4 and 11 lanes -- it is a
per-message cost, not contention.

### What this means for remediation

The lever is **poll-wakeup latency**, not marshalling. Beating Go on a
message-passing chain would require waking the target without a full poll cycle
(e.g. a per-lane lock-free inbox that the scheduler checks every turn, or a
spin-then-park handoff). That is a real, bounded piece of work and is where the
next change should go.

## Remaining candidates (not yet proven)

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
