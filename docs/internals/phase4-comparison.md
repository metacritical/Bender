# Phase 4: Bender vs Go vs Erlang on nominally identical work

> **Correction notice (docs audit).** An earlier revision of this file stated
> several things as established that the preserved evidence does not support.
> Corrected here: the Go source filename; the claim that every run was verified
> by an assertion; the "linear" characterisation of the Go and Erlang curves;
> the 0.46 us/hop lanes-off figure; two "REJECTED" hypotheses that the retained
> data cannot settle; and the 1M "did not complete in 600 s" outcome, whose only
> basis is a session transcript. Every raw number that was reported is kept
> below, but it is now labelled **reported** (no retained artifact) or
> **retained** (artifact in repo). Nothing here is a current-code measurement of
> this repository at HEAD.

Machine: 12 logical cores (8 performance + 4 efficiency), 16 GB RAM.
Verified with `sysctl -n hw.ncpu hw.perflevel0.logicalcpu
hw.perflevel1.logicalcpu hw.memsize` → `12 8 4 17179869184`.

All three programs build a chain of N processes; `0` enters the head, each
process adds 1 and forwards, the last reports.

Sources actually present in the tree:

- Bender: `bench/wave.ben` (also the parity variant `bench/cross-runtime/wave.ben`)
- Go: `bench/wave-go.go` (also `bench/cross-runtime/wave.go`)
- Erlang: `bench/wave.erl`

An earlier revision cited `bench/wave.go`, which does not exist in this
repository.

## Result reporting geometry differs per harness

This matters for reading the table, and it is visible in the source:

- `bench/wave-go.go:21-28` prints separate `spawn=` and `wave=` phases plus a
  `total` that is `spawn + wave`.
- `bench/wave.erl:10-18` prints the same three phases from
  `erlang:monotonic_time`.
- `bench/wave.ben:9-29` prints the same three phases to **stderr** via
  `eprintf` (line 27), and prints nothing to stdout.

Two asymmetries are structural, not measurement noise:

1. **Verification.** The retained `bench/wave.ben` never asserts. It computes
   `result` and prints it (lines 24, 27-28). The `assert (= result n "...")` that
   an earlier revision of this doc described ("`result == N` in all runs below")
   exists only in `bench/cross-runtime/wave.ben:37`, which is a different
   file with a different output format (`spawn_ms=`/`wave_ms=`). So the phase-4
   table cannot have been checked by the assertion it claimed.
2. **Erlang timeout is indistinguishable from success by number alone.**
   `bench/wave.erl:15` is `receive {done, X} -> X after 600000 -> 0 end`, i.e. a
   600-second guard that returns the integer `0` on timeout rather than failing.
   A `result=0` line is therefore a timeout, and a `result` equal to N is the
   only positive evidence. No Erlang raw output from the 1M arm is retained.

## Results (reported, no retained artifact)

These are the numbers the phase-4 run printed. They are not reproducible from
anything committed, and the Bender numbers are not cross-checked by an
assertion (see above).

| N | Go | Erlang | Bender |
|---|---|---|---|
| 100,000 | **131 ms** | 182 ms | **1,538 ms** |
| 200,000 | — | — | 4,051 ms |
| 400,000 | — | — | 9,357 ms |
| 1,000,000 | **2,430 ms** | **1,869 ms** | no retained result |

The Bender 1M cell previously read "not completed in 600 s". That claim rests on
a session transcript, not on an artifact: no timeout record, no killed-process
status, and no partial output for that arm survive in the repository. The
transcript also indicates the run was terminated by the operator's own later
`pkill` and that a shell pipeline returned `0` regardless of the child's
failure, so even the transcript does not establish 600 s of elapsed work. Treat
the 1M Bender arm as **no retained measurement**, not as a timeout.

## The answer to "does Bender beat Go on the wave?"

On the reported numbers, Bender is far slower per hop at 100k: ~12x slower than
Go, ~8x slower than Erlang. The retained controlled evidence below has no Go
or Erlang arm and cannot verify these ratios. The *shape* of the growth is where the earlier
revision overreached.

The earlier revision claimed Bender is superlinear while "both others are
linear". Its own reported numbers contradict that:

```
Bender:   100k 15.4 us/hop   200k 20.3 us/hop   400k 23.4 us/hop
         2x N -> 2.3-2.6x time
Go:      100k -> 1M is 18.5x for 10x N   <- labelled "(linear)"
Erlang:  100k -> 1M is 10.3x for 10x N   <- labelled "(linear)"
```

Go grows 18.5x for 10x N, not linearly; Erlang grows approximately 10.3x,
consistent with approximately linear growth between those two points but not a
proven scaling law. Neither pair establishes a general curve. The harness also
prints total time including spawn (`bench/wave-go.go:25-28`); without the raw
phase records, the table cannot partition build and traversal costs.

Per-hop, the reported split is Bender ~15 us, Go ~1.3 us, Erlang ~1.9 us at 100k.
Those three figures come from dividing single reported totals by N; they are
not independently measured per-hop costs.

## What this benchmark does and does not measure

The chain is a **sequential dependency chain**: every hop waits for the previous
one, so no amount of lanes can parallelise the traversal. It measures
per-message latency through a process chain, not parallel throughput.

One caveat the earlier revision omitted: the reported `total` includes the
**spawn/build phase** (`bench/wave.ben:12-22`, `bench/wave-go.go:12-21`,
`bench/wave.erl:10-13`). A 1M arm therefore reports build cost plus traversal
cost, and the two are not separated in the summary table. `spawn` and `wave` are
printed separately by all three harnesses, but the phase-4 doc kept only the
totals.

The earlier revision's comparison table mixed this benchmark with unrelated
claims:

| workload | Bender | notes |
|---|---|---|
| chain of N (this benchmark) | ~12x slower than Go | latency-bound, single chain |
| independent fan-out (earlier phase) | 6.6-7.4x faster than sequential | uses all lanes |
| trivial units | 8.2 M units/sec | throughput-bound |

The fan-out and "trivial units" rows are **not** comparisons against Go and are
not reproduced here; the 6.6x figure traces to
`docs/internals/concurrency-visual-guide.md:416` ("34 ms parallel vs 223 ms
sequential -- 6.6x on 11 lanes"), which is Bender against Bender. A throughput
number divided over a sequential baseline is not a Go comparison. Quoting the
three rows side by side invited exactly that misreading.

## Retained controlled evidence (the only reproducible Bender data)

`bench/results/phase4-controlled-35671ede/` holds the pinned-revision scaling
run, with `legacy-scale.jsonl` carrying all trials (no selected minima) at
source `35671ede`. Wave phase, three fresh subprocesses per arm, all
`returncode=0` and `correct=true`:

| forwarding actors | `BENDER_LANES=0` wave ms (median [min,max]) | `BENDER_LANES=2` wave ms |
|---:|---:|---:|
| 1,000 | 95 [95, 101] | 6 [5, 8] |
| 2,000 | 380 [377, 385] | 12 [11, 14] |
| 4,000 | 1,515 [1,511, 1,609] | 24 [22, 29] |

Two facts follow directly and are worth stating because they contradict claims
made elsewhere in this file's earlier revision:

- **Placement changes wall time by more than an order of magnitude** at
  identical N and identical source. Lane-off ÷ 2-lane on the medians is 15.8x,
  31.7x and 63.1x at N=1000/2000/4000 (and up to 73x comparing extremes). Any
  single "us/hop" number is therefore only meaningful together with its
  placement.
- **The lane-off N-chain is nowhere near 0.46 us/hop.** 95 ms / 1000 hops is
  ~95 us per hop, and 1515 ms / 4000 is ~379 us per hop. The earlier revision's
  line "lanes off is 0.46 us/hop vs 7.4 us with lanes on" is unsupported by any
  retained artifact and is irreconcilable with this table by two orders of
  magnitude; it has been removed (see next section).

These are *not* evidence about 1M, and the directory's own README
(`bench/results/phase4-controlled-35671ede/README.md`) says as much: it records
that a two-lane N=100,000 profiling attempt was **killed by the runner itself**
after a 2 s sample plus a bounded wait (`lanes2-100k-profile.json`:
`returncode: -9`), and explicitly states that this is "NOT proof of an OS memory
kill, a 600-second timeout, or a scaling law."

## Cross-lane hop cost: provisional, not root-caused

The earlier revision carried the heading "ROOT CAUSE FOUND: cross-lane hops
cost two poll cycles" and asserted that ~6.9 us of every hop is "poll-wakeup
latency ... the target VM's event loop (self-pipe -> kevent)". That is now
downgraded to a **provisional hypothesis**, for these reasons:

The reported micro-table was:

| operation | cost |
|---|---|
| marshal a value | 0.12 us |
| marshal + unmarshal | 0.14 us |
| same-VM channel round trip | 0.17 us |
| **cross-lane round trip** | **6.9-7.4 us** |

- No script in `bench/` produces the 0.12/0.14 us marshal rows, and no retained
  output records them. They are reported only.
- `bench/lane-rt.ben` does not measure a rendezvous hop. `bench/lane-rt.ben:13-14`
  creates `ev/thread-chan 64` — a **buffered** channel — for both directions, so
  the sender need not wait for the receiver to rendezvous, and the loop at
  line 18 measures one request followed by one reply, not streaming throughput. Its same-VM arm
  (line 6, `ev/chan 1`) is also buffered. Its cross-lane echo proc is itself
  lane-routed (`proc/spawn`, line 15); its placement and channel operations are
  part of the measured path.
- `bench/hops.ben:14-29` spawns idle filler procs and hammers a 2-proc echo
  chain. It varies live-set size only; the chain length is fixed at two. It
  cannot separate chain length from live-set growth because the N-chain runs were
  never re-run under this harness.
- A `sample` capture during a run does show the main thread parked in
  `bender_loop1 -> kevent`
  (`bench/results/phase4-controlled-35671ede/lanes2-100k.sample.txt`: 1212 of
  the 1739 main-thread samples sit under `start -> main -> bender_loop1 ->
  kevent`, with most of the remainder under `bender_fiber_shared_prime`; the
  second thread, `bender_lane_thread_body`, is 1344 of 1739 in `kevent` and shows
  only ~15 samples in `bender_thread_chan_cb`/`bender_unmarshal`). But that process
  was **killed by the runner** (`lanes2-100k-profile.json`, `returncode: -9`), so
  those samples are overwhelmingly of idle event loops waiting for a chain that
  never finished — the sample shows the loops are parked, not what a hop costs.
  Idle `kevent` is not causal evidence for the 6.9 us figure.
- The other retained sample,
  `bench/results/phase4-controlled-35671ede/local8000.sample.txt`, has **zero**
  `kevent` frames and puts 4,148 of 4,151 main-thread samples under
  `bender_collect_fiber_minor`. So the one profile that is dense enough to see
  work sees GC, not poll wakeup — in the lane-off placement.

The retained data establishes placement-sensitive wall time, not a partition
into marshalling, scheduling and collection. Indeed the retained small N-chain
is faster with two lanes than locally. The specific attribution to "two poll
cycles / ~6.9 us of kernel wake" is unverified.

### Hypotheses the earlier revision called REJECTED

Both are now **unresolved**, because the experiment does not test what the
conclusion claims:

1. **"Live process count makes minors slow"** — reported 8.34 / 7.79 / 7.57 /
   7.99 us/hop at 0 / 50k / 100k / 200k idle live procs. Reported flat across live-set
   size, a limited observation for *that* harness. But the flat arm is
   a `proc/spawn` (lane-routed) 2-proc echo chain
   (`bench/hops.ben:21-29`; the trailing `8` is the mailbox **capacity**
   argument, `src/core/proc.c:1245-1248`, not a lane index), while the
   superlinear curve came from the lane-off N-chain. The two arms are not the
   same placement, so the flat result does not transfer to the lane-off curve.
2. **"The chain length is the problem"** — the earlier revision wrote "a 2-proc
   chain costs the same per hop as an N-proc chain". That is contradicted by the
   historical figures: the routed 2-proc harness reportedly measured ~7.6-8.3 us/hop, while the
   lane-off N-chain at N=1000 reports ~95 us/hop (95 ms / 1000). These are
   different placements again, and nothing in the repository measures a 2-proc
   chain at the lane-off placement. The comparison as written is not valid; the
   earlier conclusion that the "superlinear curve was BUILD-phase live-set
   growth, not traversal" is therefore **not established**.

## What this means for remediation (provisional)

The earlier revision asserted the lever is poll-wakeup latency and that beating
Go "would require waking the target without a full poll cycle". Given the above,
poll wakeup remains a *plausible* cost centre, but the size of its share (~6.9 us of ~15 us per hop) is unmeasured.
Before spending on transport, the cheap discriminator is a lane-off 2-proc
echo at the same placement as the fast arm: if per-hop cost there matches the
2-proc routed figure, chain length is irrelevant and placement dominates; if it
is far lower, the lane-off N-chain's cost is in traversal or completion-time
collection (which `local8000.sample.txt` independently points at).

## Remaining candidates (not proven)

1. **~15 us per hop (reported).** Not attributable from retained data.
2. **Superlinear growth over 200k-400k (reported).** The 2x-N → 2.3-2.6x-time
   claim comes from unretained single runs; only the 1000-4000 range has
   retained trials, and there the lane-off arm does grow steeply
   (95 → 380 → 1515 ms for 1k → 2k → 4k, i.e. 4x per doubling — steeper than
   the reported 2.3-2.6x). No cause is isolated. `local8000.sample.txt` (4,148
   of 4,151 samples in `bender_collect_fiber_minor`) makes completion-time
   collection the leading suspect for the lane-off placement specifically.
3. **1M does not finish.** No retained artifact exists in either direction. The
   memory argument below is an inference, not a measurement.

## Memory note (context, from a different measurement)

`docs/internals/lanes-and-captured-state.md:208-216` records a **1,311 MB RSS
for 1,000,000 resident procs (1.31 KB/proc)** after the shared-young-page change
at `3401dc30`, with all 1M bodies executed. That is a retained measurement of
residency in a purpose-built harness — it is *not* a measurement of the wave at
1M, and the earlier revision's inference ("the machine had only ~1-1.5 GB free,
so it may have been memory pressure") was never tested. It also does not
establish that the wave would fit; residency and traversal working set differ.

## Method notes (learn from earlier mistakes)

- **min-of-3 was claimed here but is not what the retained evidence does.**
  The controlled scaling run records *all* trials and explicitly states "All
  samples retained, no selected minima"
  (`bench/results/phase4-controlled-35671ede/README.md`). The phase-4 table
  itself has one number per cell with no repeat count recorded.
- **`result == N` was claimed on every run; the retained `bench/wave.ben`
  never asserts it** (lines 24, 27-28 only print). Assertion exists only in
  `bench/cross-runtime/wave.ben:37`. For the phase-4 table, verification is
  unverified.
- Machine state was reported as unclean: load average ~4.5 and swap in use
  throughout. If true, the Go and Erlang arms are inflated too, but neither ratios nor
  absolutes are thereby validated. This is still a report, not a record — the
  controlled run's README notes 43% memory free from `memory_pressure -Q` on a
  different occasion and warns it is "not evidence that the machine was idle".
- A clean-machine rerun with retained artifacts, per-placement per-hop figures,
  and an assertion on every arm is owed before any absolute number from this
  file is quoted.
