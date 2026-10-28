# Cross-lane fast-post: attempted, reportedly measured, rejected

> **Correction notice (docs audit).** The implementation this file describes
> was reverted before commit and its source is not in the repository. Therefore
> **everything below about the ring, the handshake and the instrumented output
> is a record of a prior session's narrative, not a finding readable in current
> code.** An earlier revision presented these as verified implementation detail.
> Where a claim can be checked against the current tree it has been checked and
> cited; where it cannot, it is marked **transcript-only**.
>
> The two raw microbenchmarks (`bench/pipe-rt.c`, `bench/thread-handoff.c`) *do*
> exist and are cited — but they are now shown to measure something other than
> what the earlier revision used them to conclude.

## What the plan expected (transcript-only)

The phase-4 diagnosis claimed a 6.9 us cross-lane hop was "poll-wakeup latency",
and proposed a per-VM lock-free inbox drained by the event loop at the top of
every turn, with the self-pipe used only as a nudge for a parked loop. Expected
outcome: ~7 us down toward ~1 us.

That premise is itself provisional — see
`docs/internals/phase4-comparison.md` ("Cross-lane hop cost: provisional, not
root-caused"). A fix cannot have been rejected on the strength of a diagnosis
that was not established.

## What was reportedly implemented (transcript-only)

No part of this design survives in the tree: `git log -S JANET_LANE_FASTPOST --
src` returns nothing, and `git diff de84e900..HEAD -- src` is empty — the whole
range from the "root cause" commit through HEAD contains no `src/` change at all.
So there is no reverted blob to diff against and no commit message describing the
mechanism; the description below exists only in the prior session's transcript.

As described in the prior session, the change was:

- a per-VM power-of-two ring of `JanetSelfPipeEvent` (`fastq`), described as
  multi-producer single-consumer with the slot `state` published last under
  release ordering;
- `janet_ev_post_event` pushing to the ring first and writing the pipe only as a
  nudge when the target announced itself parked;
- `janet_loop1` draining the ring at the top of every turn and again after poll;
- a two-step parked handshake (announce parked, then re-check the ring) intended
  to close a lost-wakeup window;
- opt-in via `JANET_LANE_FASTPOST=1`, pipe path untouched when off;
- a new `janet_atomic_store` with release semantics for publication.

Three details of that description are self-inconsistent and are recorded here
because they bear on the rejection, not because they were ever verified:

1. **It is not a correct MPSC reservation.** A producer that loads the ring head
   and then `atomic_inc`s it does not obtain a unique slot: two producers can
   read the same head and both publish into it. A correct bounded MPSC queue needs a unique reservation protocol
   (for example an appropriate atomic RMW plus slot ownership/capacity handling). As described, the ring can silently drop or overwrite posts under
   concurrent producers — which is a plausible mechanical explanation for the
   reported hang, independent of any wakeup-handshake defect.
2. **Release/acquire on the slot alone does not guarantee the consumer ever
   looks.** Publication ordering makes a published slot *visible*; it does not
   make the parked consumer *runnable*. If the parked arm's own re-check races
   with the publisher's park observation, the post can land in a ring the
   consumer already decided to stop examining. The described two-step handshake
   narrows that window; nothing in the description closes it.
3. **Setup and teardown were reportedly duplicated/absent.** The self-pipe setup
   would have been duplicated rather than consolidated, and the ring reportedly
   had no cleanup path — so a VM that never drained the ring could retain its
   entries. The current code has exactly one setup/cleanup pair per event-loop
   flavor (`src/core/ev.c:2648` `janet_ev_setup_selfpipe`, `:2690`
   `janet_ev_cleanup_selfpipe`, called at `:2906`/`:2927`, `:3098`/`:3117`,
   `:3257`/`:3273`), which is what "duplicated setup" would have broken.

Two further signal-path notes were reported and are transcript-only: the post
path was described as allocating on the signal path, and instrumentation
counters (`[fastq] FIRST PUSH`, `[fastq] FULL`) were plain non-atomic increments
touched from multiple lanes — a data race in the diagnostics themselves, so
those counts are not trustworthy even as diagnostics.

The current POSIX path uses a stack event (`src/core/ev.c:3307-3310`).
That does not verify or refute the transcript observation that the rejected
fast path allocated or used non-async-signal-safe operations from signal callers.
The rejected source is unavailable, so this remains a transcript-only concern.

## Measurements as reported (no retained artifacts)

**1. It reportedly did not remove the latency.**

```
baseline (pipe) : 6.80 us/op
with fastq      : 7.82 - 10.90 us/op    (no improvement, sometimes worse)
```

**2. It reportedly lost wakeups under load.** Described as correct at n=300 and
n=2000, then hanging from roughly n=5000 onward. The ring never reported full
(`[fastq] FULL` count: 0), which was read as "a genuine missed wake".

Two caveats on that reading:

- The `FULL` counter was itself one of the racy diagnostics above, so "the ring
  never reported full" is not evidence that capacity was never exceeded.
- A hang at larger n is equally consistent with the broken reservation (two
  producers overwriting one slot) as with a missed wake. The observation does
  not discriminate between them.

Nothing in `bench/` produces a 6.80 us/op baseline or a 7.82-10.90 us/op fastq
figure, and no output files for either are retained. These are unverified
reports.

## Why the idea is now considered unlikely to have worked (provisional)

The earlier revision presented a microbenchmark table and then a mechanism:

| primitive | cost |
|---|---|
| pipe write + read round trip | 0.70 us |
| two-thread spin handoff | 0.094 us |

Both numbers are reported, not retained. More importantly, **neither program
measures the operation the conclusion is about**, and this is checkable in the
sources that do exist:

- `bench/pipe-rt.c` writes and then reads on the **same thread**: the pipe is
  created at line 7 and the only loop (line 10) is
  `write(p[1],...); read(p[0],...);` with no other thread in the program. A
  same-thread write followed by an immediate read does not measure a blocked consumer becoming runnable; there is no
  cross-thread handoff or measured scheduler wake, and no
  wake of another OS thread. It measures syscall overhead, not a wake. The file's
  own header comment (line 5) calls it "the floor for any pipe-based wake" — the
  comment asserts what the program does not measure. Using it to argue "the
  self-pipe was never the expensive part, so ~6 us is OS wake cost" does not
  follow: the program contains no wake.
- `bench/thread-handoff.c` is a pure spin pair (`atomic_int req, ack` at line 6;
  worker spinning on `req` at line 9; main spinning on `ack` at line 18). It
  measures a handoff where **neither side ever sleeps**. That is a measurement
  of a dedicated-spin design, not the cost of the park/wake path the event loop
  actually uses. The earlier conclusion "only busy-spinning (0.094 us) avoids
  that" is a statement about the benchmark, not about the runtime.
- Alongside those two, **neither program partitions a threaded wave's wall
  time.** One measures a syscall pair, the other a spin handoff; neither runs a
  process chain, so neither can attribute any fraction of a per-hop cost.

So the earlier revision's central inference — "the pipe is 10% of it ... the
target lane's event loop **must** be woken and scheduled by the kernel" — is not
supported by these two programs. It is also contradicted in spirit by the
retained GC profile: `local8000.sample.txt` has **zero** `kevent` frames and
4,148 of 4,151 main-thread samples under `janet_collect_fiber_minor`, whereas
the only retained `kevent`-heavy sample
(`lanes2-100k.sample.txt`) is from a process the runner itself killed
(`lanes2-100k-profile.json`, `returncode: -9`) and therefore shows an idle loop
waiting for a chain, not the cost of a hop.

A defensible statement of what the rejected design would have had to beat:
*the reported channel RTTs differ, but their causal partition is unmeasured;
the retained N-chain instead runs faster with two lanes than locally.*
The lane-off controlled data (95 ms wave at N=1000, i.e. ~95 us/hop) shows that
"same-VM is ~0.17-0.46 us/hop" does not hold for the N-chain either — 0.46
us/hop is not reproduced by any retained artifact and conflicts with the
controlled table in `bench/results/phase4-controlled-35671ede/legacy-scale.jsonl`.

Also note that a cross-lane round trip and a chain hop are not interchangeable
quantities. A ping-pong RTT includes request and reply; its count of actual OS wakeups
is not established. Dividing the reported 6.9 us RTT by two to infer a
chain-hop floor would require equivalence that `bench/lane-rt.janet`
(buffered `ev/thread-chan 64`, lines 13-14) does not establish.

## Decision

The change was reverted and **nothing shipped**. That part is verifiable and
holds: `git diff de84e900..HEAD -- src` is empty, so `src/core/ev.c`,
`src/core/state.h`, `src/core/capi.c` and `src/include/janet.h` are identical to
`de84e900`, and `git log -S JANET_LANE_FASTPOST -- src` finds no commit.

Two corrections to the earlier wording:

- "the baseline re-measures at 6.80 us/op" — no such re-measurement is retained.
  The revert is verifiable; the re-measurement is not.
- "the tree is clean" — the *source* tree matches `de84e900`, but the working
  tree is not clean: it carries untracked files including `ML/`, `erl_crash.dump`
  and several `*_plan.md` files. Say "no source change", not "clean".

## What the next attempt should target (provisional, in priority order)

1. **Measure before designing.** Re-run a lane-off 2-proc echo and a routed
   2-proc echo at the same N and placement, retained, with an assertion. That
   single pair discriminates "traversal/collection" from "cross-lane transport"
   — the question this file was written to answer and never answered.
2. **Do not optimize the round trip; avoid it.** If the cost is per-message,
   batching N messages per wake amortizes it. This is a caller design change,
   not a scheduler change.
3. **Pin related work to the same lane.** The retained controlled data shows
   placement moves wall time by more than 10x at fixed N, which makes placement
   the highest-leverage known knob — independent of any transport theory.
4. **Spin-then-park with a bounded spin** remains a plausible way to beat a
   park/wake floor, but its value depends on the unmeasured share of that floor.

The earlier revision closed by asserting Go's advantage "is ~1.3 us/hop against
Janet's 6.9, and the same ~6 us OS wake cost applies to any C-level loop that
sleeps between messages". All three parts are unverified here: 1.3 us/hop is a
division of a single unretained Go total by N, 6.9 us is from a buffered-channel
harness, and the generality about "any C-level loop" is not tested at all.
Codegen quality (AOT, PGO, type-guard elision) is a separate, better-evidenced
track — see `bench/cross-runtime/pgo-results.md` — but it is not established as
"the remaining lever for beating Go" by anything in this file.
