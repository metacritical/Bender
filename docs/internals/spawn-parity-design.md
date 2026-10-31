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
scheduler was entered", and no loop-level predicate tried so far does. The
promising hook is the fiber SUSPEND path rather than any loop iteration: the
spawner never suspends during its burst (it runs a `while` loop, and the
periodic drain does not suspend it), but it does suspend on `ev/sleep` or
`ev/take`. That is a single, semantically correct hook and it is where the next
attempt should go -- not another loop predicate.

Reverted rather than shipped: a 4x spawn win is not worth a runtime that
deadlocks on one spawn followed by a wait. Tree verified after revert: wave
correct at 200,000, `suite-lane-chan` 9/9.

### Possibly pre-existing: the `:steal` overflow push also uses mode 2

`proc.c` pushes a closure-bearing `[:spawn serial cap entry]` tuple to the
threaded `bender_lane_overflow` channel with mode 2, and reads the result as
"0 = refused, nonzero = enqueued" -- the try-push convention, which the mode-2
branch does not implement. That is the same misuse this prototype died on,
in shipped code, though it is reached only under the non-default `:steal`
lane policy (`2 == bender_lane_policy`), which is why no suite covers it.
NOT verified as a live defect here, and deliberately not fixed as a drive-by:
it needs its own reproducer under the `:steal` policy before any change.

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

## Non-goals

Not a claim that BEAM has no global synchronisation, nor that every BEAM spawn
is strictly constant-time. The claim is narrow and sourced: BEAM's ordinary
spawn/GC design makes neither the live process set nor the parent's heap part
of a spawn's cost, and Bender's routed path makes both of them part of it.
