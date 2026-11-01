# Why BEAM's spawn is flat, and what Bender must change

Read from OTP source at `/Users/pankajdoharey/Development/Projects/ML/otp`
(read-only), not inferred from benchmarks. Companion to
`docs/internals/proc-spawn-scaling.md`, which holds the measurements.

## Measured starting point

Trampoline wave (`bench/cross-runtime/wave.ben`), matched alternating runs at
~3.5 GiB swap, `BENDER_LANES=11`:

| path | Bender | BEAM (OTP 17.1.1) | gap |
|---|---:|---:|---:|
| spawn, routed across lanes | 8.56 us | 0.58 us | **14.8x** |
| spawn, local (`BENDER_LANES=0`) | 1.87 us | 0.58 us | **3.2x** |
| per live proc | 2,847 B | 1,709 B | 1.67x |

Two different gaps with two different causes. Treating them as one is how the
earlier "routed spawn does extra work" note stayed unactionable.

## Why BEAM is flat: three source-level facts

**1. A pid is an immediate, not a pointer.** `erts_ptab_make_id`
(`erl_ptab.h:319-326`) shifts the data and ORs a tag; `erts_ptab_id2pix`
shifts back to recover the table slot. A pid is a tagged integer term, so
holding one creates **no GC edge** and one process's heap can never join
another's traced object graph. `erl_gc.c` contains zero references to the
process table, so collecting one process cannot walk another.

**2. The process table is pre-allocated and never resizes.**
`erts_ptab_init_table` performs a single
`erts_alloc_permanent_cache_aligned` for the configured maximum
(`erl_ptab.c:385-397`); our benchmark passes `+P 8000000`, so the slot array
is reserved up front. `erts_ptab_new_element` is an atomic counter increment
plus a free-id slot exchange (`erts_atomic32_inc_read_acqb` on `count`, then
`aid_ix`, then `erts_atomic_xchg_nob` on `free_id_data[ix]`) -- O(1), with no
list walk, no rehash and no growth.

**3. A process heap is ONE right-sized allocation.** In
`erl_create_process`, `sz = erts_next_heap_size(heap_need, 0)` binary-searches
a precomputed size table (`erl_gc.c`), then
`p->heap = ERTS_HEAP_ALLOC(ERTS_ALC_T_HEAP, sizeof(Eterm)*sz)`.
`H_DEFAULT_SIZE` is 233 words = **1,864 bytes** on 64-bit, which is
essentially the 1,709 B/proc marginal measured for BEAM -- the per-process
footprint IS the initial heap, with no page-granularity tax.

So flatness has one cause in two halves: **identity is decoupled from
reachability**, so the live set never enters the spawn path; and **allocation
is right-sized per process**, so footprint does not round up to a page.

## What Bender does differently

| concern | BEAM | Bender |
|---|---|---|
| pid | immediate, tagged integer | heap 4-tuple `[:pid lane serial ctl-chan]` carrying a channel abstract -- a real GC edge |
| pid -> process | shift to a slot in a pre-sized array | per-lane `serials` table, plus a walk of the closure's captures (`bender_proc_find_pid_lane`) to pick a lane |
| per-process allocations | 2 (PCB slot from the fixed table, one heap block) | 4 (fiber block 408 B, a separate `bender_realloc` stack 128 B, proc abstract 144 B, eager sup channel 240 B) |
| spawn placement | always the current scheduler | the current lane IF the spawner is a lane (`proc.c:295-300`); otherwise marshal + lane-control message |

The placement row is the important one. Bender **already implements BEAM's
rule** -- `proc.c:295-300`: "a process spawning from a scheduler/lane keeps
the child on that lane. No closure marshal, no control-channel round trip."
The main VM is simply not a lane, so a top-level script can never reach it. In
BEAM, the context the main program runs in IS a scheduler.

## Candidates, in measured-value order

**(a) Let top-level spawns take the local path.** Run the main script on a
lane, or let the main VM act as one, with migration to distribute. Targets the
whole 8.56 -> ~1.87 us, a ~4.5x spawn win on this workload, and is the
BEAM-shaped answer rather than a workaround.
Gate: the wave phase, not just spawn. `BENDER_LANES=0` shows same-VM hops are
pathological (0.149 -> 1.86 ms/hop as N grows, a separate O(N^2) tracked as
Phase 10), so co-location currently buys the wave what it costs the spawn.
That interaction must be measured before this is adopted.

**(b) Batch routed spawns per target lane.** Measured ceiling, which is what
justifies writing it. `tools/push-split-probe.patch` on
`bench/spawn-only-chain.ben` (where pushes == spawns exactly: 200,016 for
n=200,000) splits the routed cost by WHAT IT IS PER:

| component | us/spawn | per | batchable |
|---|---:|---|---|
| `bender_loop1_poll_now` drain | 3.22 | message | yes |
| push remainder (lock, queue, pending reader) | ~1.0 | message | yes |
| `bender_ev_post_event` pipe post | 0.119 | message | yes |
| `bender_chan_pack` closure marshal | **1.089** | **closure** | **no** |

So ~4.3 us of ~8.5 us is per-message and amortises with the batch factor,
while 1.089 us is per-closure and does not. Ceiling is therefore roughly
**1.5-2.5 us/spawn**, i.e. a 3.4-5.7x spawn win landing at about the 1.87 us
local-spawn floor. Measure against that floor, not against zero.

This does NOT contradict the rejected drain-tuning experiment recorded in
`proc-spawn-scaling.md`. That experiment cut the drain's FREQUENCY while event
production stayed at one per spawn, so the cost moved into `push` and wall time
got worse. Batching cuts event PRODUCTION, so the self-pipe pressure that makes
the drain necessary falls proportionally rather than being deferred. The two
changes act on different variables and the earlier negative result does not
transfer.

`bender_proc_find_pid_lane` already routes an entire chain to one lane, which
is the ideal case for batching.

### Prototype result: the win is real, the backpressure path is not solved

A working prototype (reverted, not shipped) confirmed the ceiling and then
failed at scale. Shape: a gcrooted thread-local array accumulating
`[:spawn serial cap entry]` tuples, one `[:spawn-batch 0 0 entries]` control
message per `BENDER_SPAWN_BATCH_MAX` = 16, a dispatcher arm
`:spawn-batch (each entry y (handle-spawn entry))`, and the flush hooked into
`bender_channel_push` itself -- one choke point that no future caller can
bypass -- so no other message can overtake a pending spawn.

Measured spawn phase on `wave.ben`, `result == N` verified each time:

| N | before | prototype | us/spawn |
|---|---:|---:|---:|
| 20,000 | 114 ms | **38 ms** | 1.90 |
| 30,000 | -- | 59 ms | 1.97 |
| 50,000 | 291 ms | **130 ms** | 2.60 |
| 100,000 | 761 ms | **194 ms** | **1.94** |

So ~4x at 100,000, landing at the 1.87 us local-spawn floor and matching the
predicted 1.5-2.5 us window exactly. The per-message/per-closure split was the
right model.

**Why it was reverted.** Above ~100,000 the ctl channel fills even at 1/16th
the message count, and the flush must then apply backpressure. Two attempts
both failed:

1. Reuse the single-spawn "plan" mechanism (return a tuple the boot wrapper
   completes). Unsound for a batch: the plan carried `bender_wrap_array` of
   the SAME array that the flush then cleared, so it would re-push zero
   entries and lose all 16 spawns silently. Worse, a batch has no frame to
   defer to -- the other 15 pids were already returned to the program.
2. Try-push (mode 2) in a `bender_loop1_poll_now` retry loop, to avoid parking
   entirely. Segfaults at N=5.

`bender_await` is not available as a third option: it is `bender_signalv`,
i.e. a longjmp, so calling it from inside `bender_channel_push` unwinds out of
a push in progress.

### The segfault: DIAGNOSED -- mode 2 is not a try-push

Attempt 2's crash had nothing to do with batching. Backtrace under `lldb`
(ASAN is useless here: it warns "ASan is ignoring requested
`__asan_handle_no_return`" because Bender switches fiber stacks itself, and its
reports land inside `bender_continue_no_check`):

    thread #2  EXC_BAD_ACCESS (address=0xffffffff)
      frame #0  0x00000000ffffffff                       <- called garbage
      frame #1  bender_mark                 gc.c:251     <- mark_abstract
      frame #2  bender_mark_fiber           gc.c:614     <- mark(fiber->proc)
      frame #3  bender_mark                 gc.c:248
      frame #4  bender_mark_roots           gc.c:2712
      frame #5  bender_collect              gc.c:3444

A global collect on a LANE thread marks a rooted fiber, follows
`fiber->proc`, and calls that abstract's mark function through a garbage type
pointer. The proc abstract was already freed: a premature free, not a wild
write.

Bisected with everything else held constant:

| variant | N=5 | N=1,000 |
|---|---|---|
| `BENDER_SPAWN_BATCH_MAX` 16, push mode 2 | segfault | segfault |
| `BENDER_SPAWN_BATCH_MAX` **1**, push mode 2 | segfault | segfault |
| `BENDER_SPAWN_BATCH_MAX` 1, push **mode 3** | ok | ok |

A batch of ONE still crashes, so the dispatcher's multi-spawn `each` loop is
innocent. The variable is the push MODE.

Mechanism, from `bender_channel_push_with_lock`: **mode 3 is the try-push.**
It is the branch that reports a full queue "without buffering" and calls
`bender_chan_unpack(channel, &x, 1)` so a refused push does not leave the
marshal buffer unreconciled. **Mode 2 is choice-write**
(`BENDER_CP_MODE_CHOICE_WRITE`, select semantics) and its own comment says
those pushes "ignore the return value". Using mode 2 as a try-push therefore
leaves a packed payload whose ownership was never settled; the lane's
unmarshalled copy points into a dead marshal buffer, and the first collection
that walks it dies exactly as above. The original single-spawn code used
mode 3 for the ctl; attempt 2 swapped a correct mode for a select primitive.

**So the next attempt is unblocked and small:** keep mode 3, and give its
"full" return a frame to suspend on by flushing BEFORE appending the current
entry. At that instant no pid is outstanding, so the existing
`:proc-spawn-write` plan tuple can carry the batch exactly as it carries a
single spawn today -- and the message must stay a tuple SNAPSHOT, since a plan
that aliased the live array would re-push zero entries after the flush cleared
it. The ordering flush belongs in the named callers (`proc/send`, kill,
monitor, stop), each of which already owns an `bender_await`-capable frame,
rather than in `bender_channel_push`, which has none.

### Attempt 3 (mode 3): the win is confirmed at every scale, and the blocker
### is now exactly one thing -- the liveness trigger

With mode 3 and the message gcrooted across the retry, batching works and the
measured win is large. Spawn phase, `result == N` verified at every point:

| N | pre-batch | batched | us/spawn | vs pre-batch |
|---|---:|---:|---:|---:|
| 20,000 | 113 ms | **34 ms** | 1.70 | 3.4x |
| 50,000 | 291 ms | **84 ms** | 1.68 | 3.5x |
| 100,000 | 761 ms | **167 ms** | **1.67** | 4.6x |
| 200,000 | 1,329 ms | **538 ms** | 2.69 | 2.5x |
| 500,000 | 4,116 ms | **2,186 ms** | 4.37 | 1.9x |
| 1,000,000 | 8,487 ms | **4,554 ms** | 4.55 | 1.9x |

1,000,000 processes complete on both arms. At 1,000,000: total 10,410 ->
6,159 ms, peak RSS 2,972 -> 2,995 MB (unchanged). Erlang on the same box at
1,000,000 is spawn 675 ms (0.675 us/spawn), wave 937 ms, total 1,612 ms,
RSS 2,295 MB -- so batching takes the spawn gap from 12.6x to **6.7x**, total
to 3.8x, memory to 1.31x.

`us/spawn` is flat at ~1.68 to 100,000 and then rises to ~4.5. That is a
DIFFERENT limit: the whole chain co-locates on ONE lane
(`bender_proc_find_pid_lane`), so a single lane thread performs every proc
creation serially. At 1.87 us of local spawn each, 1,000,000 procs is a ~1.9 s
floor on that one thread, and the measured 4.55 s is about 2.4x it. Observing
the process confirms it: one busy thread of twelve. Past this point the lever
is distribution (candidate (a)), not the spawner.

**The blocker is the LIVENESS trigger, and only that.** A batch must be
flushed before the spawner waits, or its procs never reach their lane. Four
placements were tried:

| trigger | result |
|---|---|
| batch full, different lane, or any push to the same ctl | **deadlock**: `suite-lane-chan.ben` spawns ONE proc then `ev/sleep`s on an unrelated channel; nothing triggers, 0% CPU hang |
| above, plus a flush at the top of `bender_loop1` | correct, but 200,000 spawn = **7,224 ms**, worse than no batching: it fires on every iteration, collapsing the average batch to ~1 while each flush still pays a snapshot tuple and a gcroot pair |
| above, moved to the idle point in `bender_loop1` (reached only when nothing was runnable) | still **696 ms at 20,000** (vs 34 ms). During a ROUTED burst the main VM's queue is legitimately empty -- the procs are on the lane -- so "nothing runnable" is not a proxy for "about to wait" |
| gate on a blocking turn via `bender_loop1_impl(has_timeout, ...)` | `bender_loop1_poll_now` calls `bender_loop1_impl` DIRECTLY, bypassing `bender_loop1`, yet the flush still fired throughout a burst, so `bender_loop1` is itself re-entered constantly |

So the trigger must distinguish "this fiber is about to wait" from "the
scheduler was entered", and no loop-level predicate does.

### Attempt 4: liveness SOLVED at `bender_await`; blocked on the BACKPRESSURE
### contract instead

`bender_await` (`ev.c:930`) is `bender_signalv(BENDER_SIGNAL_EVENT, nil)` and
is the single path every event wait takes (15 call sites). Flushing there is
exactly "this fiber is about to park", and a spawn burst never reaches it,
because the burst runs a loop and its periodic drain does not suspend. That
works: `suite-lane-chan` 9/9 with the win kept -- 20,000 spawns in 42 ms
(113 pre-batch), 100,000 in 181 ms (761 pre-batch).

**The liveness problem is therefore solved. Three further things were found,
and the third is the new blocker.**

**Found and fixed: the drain interval must track EVENT PRODUCTION, not spawn
count.** Widening it from every 4 spawns to every 16 is only valid where
batching posts one message per 16. Under `:steal`, where batching is disabled
(below), production stays at one message per spawn and the wider interval
starves the self-pipe -- a hang, not a slowdown. `tick_mask` must be chosen
per policy. `test/suite-lane-steal.ben` caught this.

**Found: batching must not apply under `:steal`.** Batching leaves a message
queued in the target ctl, which makes `bender_channel_count(ctl) >= 1` true and
so ACTIVATES the overflow branch that has never run. It does not work once
live: with its mode-2 push the pulling lane gets a corrupted task ("expected
... got 0" in `handle-spawn`, via the `:pull` arm), and correcting that to
mode 3 only moves the failure -- the `[:pull]` wakeup nudge is itself a mode-2
push, the queue never drains, and the run parks at 0% CPU. Both the mode fix
and its revert are recorded below. A spawn-throughput change must not smuggle
in a repair of an unrelated broken feature, so `:steal` keeps the
one-message-per-spawn path.

**BLOCKER: a spawn into a FULL control channel must SUSPEND the calling
fiber.** `test/suite-proc-spawn-return.ben` pins this deliberately: it installs
a gated lane worker that never consumes, fills the ctl to capacity, spawns from
two fibers and asserts both are `:suspended`. That is what the single-spawn
"plan" return (`[:proc-spawn-write pid ctl msg]`) exists for -- the boot wrapper
performs a blocking give, so the FIBER parks. It is a contract, not an
implementation detail.

Attempt 4 replaced it with a C-level drain-and-retry loop
(`while (push(ctl, msg, 3)) bender_loop1_poll_now();`), which spins in C
instead of suspending -- and against a gated lane would spin forever. Also
`suite-lanes.ben` regressed ("kids sum over 2 lanes": a wrong total, so procs
lost or misdelivered), which is NOT yet diagnosed.

### Attempt 5: the `suite-lanes` wrong sum is a SEMANTIC bug, and it
### invalidates the headline numbers

The full-ctl bypass works and the suspend contract is satisfiable: bypass
batching when `bender_channel_count(ctl) >= 4096` (the ctl's capacity) or under
`:steal`, taking the original single-spawn path with its plan return verbatim.
That is not the blocker.

The blocker is `suite-lanes`' wrong sum, and it is a correctness violation, not
a lost message. Minimal probe -- five procs, each closing over the spawner's
loop variable, sending `(* i i)` back:

| `BENDER_SPAWN_BATCH` | values |
|---|---|
| 1 | `[0 1 4 9 16]` -- correct |
| 16 | `[1 4 9 16 25]` -- **every proc sees `i+1`** |

`spawn_msg` holds `bender_wrap_function(func)`, a LIVE reference to a closure
whose environment is the spawner's. The MARSHAL is what snapshots captured
values, and it happens inside `bender_chan_pack` during the push. Deferring the
push therefore defers the snapshot, so every batched closure observes a later
value of any variable the spawner mutates afterwards -- here shifted by exactly
one increment. Batching as designed changes observable program behaviour.

**This also invalidates the attempt-3 and attempt-4 speed figures.** The
intra-message funcdef dedup that made batching look 4.6x faster at 100,000 is
the SAME deferral that breaks captures: those runs were fast partly because
they were wrong. No measurement taken with a deferred marshal can be quoted.

**Corrected design, with an honest ceiling.** The closure must be marshalled
EAGERLY, at spawn time, and the batch must carry the resulting buffers:
`[:spawn serial cap <marshalled-buffer>]`, with the dispatcher unmarshalling
each entry. Then:

- capture semantics are preserved, because the snapshot happens where it does
  today;
- the per-MESSAGE costs still amortise -- ctl lock, queue push, self-pipe
  event, and the spawner-side drain they necessitate, ~4.3 of ~8.5 us/spawn;
- the per-CLOSURE marshal (1.089 us) is unchanged, as it always was;
- **the funcdef dedup is LOST**, because each closure is marshalled separately,
  so footprint does not improve and `marsh.c`'s `seen_defs` no longer applies
  across a batch.

Expected spawn phase is therefore roughly 8.5 -> ~4.2-4.5 us/spawn: about
**2x, not 4.6x**, with no footprint change. Still worth doing -- it is the
largest remaining spawn item -- but it must be measured fresh after the eager
marshal, and the earlier numbers must not be carried over.

A cross-message marshal registry becomes interesting again in that design,
since intra-message dedup no longer applies; but it carries the
wrong-code-on-desync risk recorded above, so it is a later question.

Reverted again rather than shipped. Tree verified: `suite-lanes`,
`suite-proc-spawn-return`, `suite-lane-chan` and `suite-lane-steal` all pass,
wave correct at 200,000.

Reverted rather than shipped: a 4x spawn win is not worth a runtime that
deadlocks on one spawn followed by a wait. Tree verified after revert: wave
correct at 200,000, `suite-lane-chan` 9/9.

### The `:steal` mode-2 push: NOT a live defect, because `:steal` is inert

`proc.c` pushes a closure-bearing `[:spawn serial cap entry]` tuple to the
threaded `bender_lane_overflow` channel with mode 2 and reads the result as
"0 = refused, nonzero = enqueued" -- the try-push convention mode 2 does not
implement, i.e. the same misuse that crashed batching attempt 2. Flagged as a
possible shipped defect; a reproducer now exists
(`test/suite-lane-steal.ben`) and the flag is **withdrawn**.

The overflow push is unreachable in practice. It is gated on
`bender_channel_count(ctl) >= 1`, and the lane dispatcher sits parked on
`(ev/take ctl)`, so a push hands off directly to the parked reader and the
queue count never rises above zero. Measured `:overflow-pushed 0` at 20,000
procs across 4 lanes, 5,000 across 4, and 3,000 on a single lane with
deliberately busy proc bodies. Every proc still ran and reported, 3/3 runs.

Two consequences:

- The mode-2 misuse sits in code that does not execute, so it is a latent
  trap rather than a bug. `test/suite-lane-steal.ben` prints the overflow
  counters, so whoever makes the path live sees it here first instead of
  meeting it as a premature free inside a collector walk.
- **`:steal` never steals.** It silently behaves as `:round-robin`. The
  surrounding comment also claims the trigger is "high water (3/4 capacity)"
  while the code tests `>= 1`; neither description matches the behaviour,
  because neither threshold is ever crossed. Whether `:steal` should be fixed
  or withdrawn is a product decision, not a correctness one.

Separately, this reproducer re-confirmed the documented self-pipe ceiling: at
N=100,000 with 4 lanes it aborts with "failed to write event to self-pipe",
matching `proc-spawn-backpressure.md` ("50000 and 80000 complete, 100000
aborts"). The suite is pinned at 20,000 to stay below it, since that ceiling is
unrelated to the policy under test.

**(c) Make a pid an immediate.** The deepest fix and the one that makes the
model genuinely BEAM-shaped: a pid becomes a tagged index into a pre-sized
lane/serial table rather than a heap tuple holding a channel pointer. It
removes a per-spawn allocation, removes a GC edge per pid, and removes the
reason `find_pid_lane` must walk closure captures at all. It is also an
observable change to `proc/spawn`'s return value, so it needs its own
compatibility story -- `docs/internals/lanes-and-captured-state.md:69` already
documents that enabling lanes changes that return type.

**(d) Shrink the per-proc allocation count from 4 toward 2.** Make the
supervisor channel lazy the way the mailbox already is (240 B; it is eager
only for the late-monitor semantic that `suite-proc` test 11 pins), and fold
the fiber into the proc (~80 B plus ~17% of per-proc mark visits -- see the
merge analysis in `proc-spawn-scaling.md`). This is the footprint and
mark-work lever, not the spawn-latency lever.

## Copy-on-write: considered and REJECTED; use the marshal registry instead

Asked whether processes should get copy-on-write mechanics to cut the
marshal cost. No -- CoW is the wrong mechanism for this cost, on two grounds.

**The dominant item is immutable by construction.** The per-proc `BenderFuncDef`
(169 B, the largest removable row in the inventory) is bytecode, constants,
arity, environment layout and source map. It is never written after
compilation, so CoW's on-write branch is unreachable: what it needs is
SHARING, not copy-on-write.

**Nothing mutable crosses a lane anyway.** `bender_proc_find_mutable_capture`
(`proc.c:288`) detects a mutable capture and places the proc on the LOCAL VM
instead, precisely because marshalling would hand the child a detached copy.
So everything a routed spawn marshals is immutable BY POLICY, and CoW would
add a write barrier, ownership tracking and a copy path to defend against
writes the placement policy already prevents.

The case where CoW semantics would genuinely be needed is shared MUTABLE state
across lanes -- which this design deliberately refuses, and that refusal is
what keeps a scoped sweep sound. Allowing it would reintroduce exactly the
cross-heap tracing that made spawn O(N^2) in the first place. That is an
architecture decision, not a footprint optimisation.

> **CORRECTION, and it reverses the recommendation below.** `marsh.c` already
> de-duplicates repeated objects WITHIN one marshal call: `st->seen` (`:35`),
> `st->seen_envs` (`:211-219`) and `st->seen_defs` (`:262-270`), the last of
> which emits a reference for a funcdef it has already written. So batching
> 16 spawns into one control message makes the marshal write the shared
> funcdef ONCE plus 15 backreferences, and the unmarshal resolve them to a
> single object -- dropping the funcdef from 169 B/proc to ~11 B/proc for
> free. That is most of what a cross-message registry would buy, and it also
> explains the ~10% RSS improvement observed in the batched build that was
> otherwise unattributed.
>
> A registry is therefore NOT the high-value item, and it is NOT a flag flip:
> `rreg` and `reg` must be symmetric with agreed indices, which is why
> Janet pre-populates them for image load. Incremental cross-thread population
> needs a protocol where the first message carries the funcdef and installs it
> at a sender-assigned index, and a sender/receiver desync makes a lane run
> the WRONG CODE -- a worse failure than the segfault that stopped attempt 2.
> Its remaining value over batching is ~11 B/proc. **Do batching's liveness
> trigger first; revisit a registry only if cross-message sharing still shows
> up in a census afterwards.**

**The mechanism exists and is unused.** Both halves of the
marshal API take a registry:

    bender_marshal(BenderBuffer *buf, Bender x, BenderTable *rreg, int flags)
    bender_unmarshal(data, count, flags, reg, next)

and the spawn path passes NULL to both:

    bender_marshal(buf, bender_wrap_function(func), NULL, BENDER_MARSHAL_UNSAFE);
    bender_unmarshal(buf->data, buf->count, BENDER_MARSHAL_UNSAFE, NULL, NULL);

A persistent PER-LANE registry makes a funcdef the receiving lane has already
seen encode as a reference rather than a fresh copy: one copy per
(lane x distinct closure) instead of one per spawn. On the wave that is 1
instead of 200,000.

It would move both axes -- footprint and the 1.089 us/spawn marshal -- but see
the correction above: intra-message dedup already captures most of it once
spawns are batched, leaving a registry worth roughly 11 B/proc for a protocol
whose failure mode is executing the wrong code. The 1.089 us marshal figure
was measured at ONE spawn per message; batching changes what it contains,
since 15 of every 16 funcdef writes become backreferences.

Gates, none of which may be assumed:
- **Registry lifetime.** It pins every funcdef it has seen, so it must be
  per-lane and cleared at lane stop, or it is an unbounded leak keyed on the
  number of distinct closures ever spawned.
- **No mutable dedup.** Guaranteed today by the placement policy above, but
  the gate must ASSERT it rather than depend on it staying true.
- **Both axes together**, since sharing defers reclamation exactly as the
  hybrid nursery cursor did.

## Three-way comparison, and the "why not compile to Go" question

Same `wave.ben` / `wave.erl` / `wave.go` trampoline chain, same machine, fresh
builds (Bender `-O2` at `e35b8117`, Go 1.27.3, OTP 17.1.1), min of 2 each:

| N | runtime | spawn us/proc | wave ms | total ms | RSS MB |
|---|---|---:|---:|---:|---:|
| 100,000 | bender | 7.97 | 132 | 929 | 326 |
| 100,000 | erlang | 0.57 | 104 | 161 | 440 |
| 100,000 | **go** | **0.63** | **35** | **98** | **277** |
| 200,000 | bender | 6.95 | 320 | 1,709 | 662 |
| 200,000 | erlang | 0.59 | 189 | 308 | 694 |
| 200,000 | **go** | **0.65** | **72** | **202** | **550** |
| 500,000 | bender | 8.50 | 838 | 5,090 | 1,514 |
| 500,000 | erlang | 0.61 | 457 | 763 | 1,457 |
| 500,000 | **go** | **0.67** | **203** | **538** | **1,367** |
| 1,000,000 | bender | 8.96 | 2,072 | 11,032 | 2,533 |
| 1,000,000 | erlang | 0.65 | 790 | **1,443** | 2,728 |
| 1,000,000 | **go** | 1.05 | **749** | 1,802 | **2,410** |

Marginal bytes per process over 100,000 -> 1,000,000: **go 2,485, bender 2,571,
erlang 2,666** -- all within measurement noise of each other.

Go matches BEAM on spawn, is the FASTEST of the three on channel hops, and has
the lowest footprint. On this benchmark Go wins three of four axes, spawn by
14x over Bender's routed path.

**So would compiling to Go have been the better route?** On these numbers,
mostly yes -- and the reason it is still not a settled question is semantic,
not numeric:

- **Go has ONE shared heap.** There are no per-process heaps and no isolated
  collection. The entire mechanism this document and
  `proc-spawn-scaling.md` are about -- `gc_owner`, `heap_lists`, per-owner
  scoped minors, the barrier and remembered set that make them sound -- has no
  counterpart in Go and cannot be built on top of it. Go marks the whole live
  set concurrently behind write barriers; that works well, but its pause and
  throughput profile becomes a permanent inherited property.
- Compiling to Go therefore buys BEAM's SCHEDULER and cheap growable stacks,
  not BEAM's MEMORY MODEL. It would mean implementing Erlang's API over Go's
  isolation story, which is none. For an AOT game compiler, frame-time
  determinism under a shared concurrent collector is exactly the hard case.
- The ceiling is asymmetric: Go's GC tuning, stack-growth policy and absence
  of per-process heaps all become unfixable, and the toolchain becomes a hard
  dependency for a language whose stated goal is native AOT output.

**Honest accounting of the C path.** Footprint is DONE -- 2,571 B/proc sits
between Go and Erlang. Spawn is flat to 1,000,000 (`N^1.11`). The whole
remaining gap is one localised item, the routed spawn path: local spawn is
already 1.87 us/spawn, within 3x of Go. That is a bounded piece of work, not an
open-ended grind.

The decision therefore rests on whether per-process heap isolation is a core
semantic of Bender or a negotiable one. If negotiable, Go was the cheaper
route to these numbers. If not, Go cannot supply it at any price.

## Non-goals

Not a claim that BEAM has no global synchronisation, nor that every BEAM spawn
is strictly constant-time. The claim is narrow and sourced: BEAM's ordinary
spawn/GC design makes neither the live process set nor the parent's heap part
of a spawn's cost, and Bender's routed path makes both of them part of it.
