# Cross-lane fast-post: attempted, measured, REJECTED

## What the plan expected

The phase-4 diagnosis said a 6.9 us cross-lane hop was "poll-wakeup latency",
and the proposed fix was a per-VM lock-free inbox that the event loop drains at
the top of every turn, with the self-pipe used only as a nudge for a parked
loop. Expected outcome: ~7 us down toward ~1 us.

## What was implemented

A complete, working implementation was built and exercised:

- per-VM power-of-two ring of `JanetSelfPipeEvent` (`fastq`), multi-producer
  single-consumer, slot `state` published last with release ordering;
- `janet_ev_post_event` pushes to the ring first and writes the pipe only as a
  nudge when the target announces itself parked;
- `janet_loop1` drains the ring at the top of every turn and again after poll;
- a two-step parked handshake (announce parked, then RE-CHECK the ring) intended
  to close the lost-wakeup window;
- opt-in via `JANET_LANE_FASTPOST=1`, pipe path untouched when off;
- added `janet_atomic_store` with release semantics for publication.

Instrumentation confirmed the fast path was genuinely reached with a valid ring
(`[fastq] FIRST PUSH reached, ring=0x...`), so this was not a case of dead code.

## Measurements: two independent failures

**1. It did not remove the latency.**

```
baseline (pipe) : 6.80 us/op
with fastq      : 7.82 - 10.90 us/op    (no improvement, sometimes worse)
```

**2. It lost wakeups under load.** Correct at n=300 and n=2000, then HANGS from
roughly n=5000 onward. The ring never reported full (`[fastq] FULL` count: 0), so
this was not capacity fallback -- it was a genuine missed wake.

## Why the idea cannot work as specified

Raw floors measured on this machine with small C programs:

| primitive | cost |
|---|---|
| pipe write + read round trip | 0.70 us |
| two-thread spin handoff | 0.094 us |

The self-pipe was never the expensive part. A round trip is ~0.7 us in the pipe
and still ~6.9 us end to end, so ~6 us is spent somewhere an inbox cannot reach:
the **target lane's event loop must be woken and scheduled by the kernel** before
it can run the callback, and the sender must be rescheduled for the reply. An
inbox changes WHERE the event is stored; it cannot change the fact that two OS
threads must each be made runnable. Only busy-spinning (the 0.094 us figure)
avoids that, and spinning burns a core per idle lane.

So the plan's premise -- "the pipe round trip is the cost" -- was wrong. The pipe
is 10% of it.

## Decision

Reverted in full. `src/core/ev.c`, `src/core/state.h`, `src/core/capi.c` and
`src/include/janet.h` are back to `de84e900`, the baseline re-measures at
6.80 us/op, and the tree is clean. Nothing shipped.

## What the next attempt should target instead

The honest options, in order of expected value:

1. **Do not optimize the round trip; avoid it.** A game loop needs throughput,
   not ping-pong latency. Batching N messages per wake amortizes the ~6 us over N
   callbacks and can approach the per-message floor. This is a design change in
   the caller, not the scheduler.
2. **Pin related work to the same lane.** A hop that never crosses a lane costs
   0.17-0.46 us already, so placement heuristics beat transport engineering.
3. **Spin-then-park with a bounded spin** (e.g. 20-50 us) before sleeping. It is
   the only thing that can genuinely beat 6.9 us, and it trades a core for it --
   acceptable only when the lane is known to be about to receive.

The remaining lever for "beat Go" is codegen quality (AOT, PGO, type-guard
elision), not message transport: Go's own advantage here is ~1.3 us/hop against
Janet's 6.9, and the same ~6 us OS wake cost applies to any C-level loop that
sleeps between messages.
