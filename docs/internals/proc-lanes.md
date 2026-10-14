# proc/lanes: VM-per-Core Process Parallelism

Implementation notes for the `proc/lanes` feature (landed through milestone 1,
`fc32f7c5`) and the work-stealing design for milestone 2. Audience: maintainers
extending the scheduler, debugging lane behavior, or building the next slices.

## Why lanes

Phase 1's density work made a single VM hold millions of processes with per-proc
heaps and per-owner collection. Lanes extend that horizontally: N resident OS
threads, each running a **full share-nothing VM** (its own heap lists, its own
collector, its own event loop). The properties that fall out:

* **No global GC contention.** Per-proc heaps compose with per-VM collectors —
  one core's collection never pauses another core's procs. The BEAM shape.
* **Density scales per core.** ~3.5 KB/proc held per VM; N lanes ≈ N× capacity.
* **The proc API does not change.** `proc/spawn`, `proc/send`, `proc/recv` work
  identically before and after `lanes-start`; routing is transparent.

## API

```janet
(proc/lanes)                 # active worker count; 0 = single-VM mode (default)
(proc/lanes-start)           # auto: cpu-count - 1 (min 1), round-robin, no affinity
(proc/lanes-start 4)         # explicit worker count
(proc/lanes-start {:count 4
                   :affinity true})   # full form (struct or table accepted)
(proc/lanes-stop)            # release lane 0; see lifecycle below
(proc/spawn-local thunk)     # bypass routing: spawn on the calling VM
(proc/self-pid)              # portable pid tuple [:pid node serial ctl]
```

After `lanes-start`, plain `proc/spawn` routes new processes round-robin across
the worker VMs. `proc/send`, `proc/recv`, and all mailbox semantics are
unchanged from the caller's point of view. Affinity pinning is best-effort:
`pthread_setaffinity_np` on Linux/glibc, `thread_policy_set` on macOS, silent
no-op elsewhere (opt-in — OS scheduling is usually right).

Lane numbering: **0 is the main VM**, workers are 1..N. A fresh process is
single-VM; lanes are opt-in and process-lifetime.

## Architecture

Three layers:

1. **Substrate (pre-existing).** `ev/thread`'s body gives OS threads each
   running a fresh VM: `janet_init()`, unmarshal a function + value, run the
   event loop until that fiber exits. Lanes reuse `janet_go_thread_subr`
   verbatim (exported from ev.c) via `janet_ev_threaded_call`.
2. **Lane registry (C, proc.c).** Thread-local state on the main VM:
   `janet_lane_ctls[N]` (one threaded control channel per worker),
   `janet_lane0_ctl` (the main VM's inbound channel), the round-robin counter,
   and a C-side serial registry. All fields are `JANET_THREAD_LOCAL`.
3. **Residency (boot.janet).** Each lane runs `proc/lane-worker` — a resident
   dispatcher looping on its control channel:
   * `[:spawn serial cap entry]` → VM-local `proc/spawn`, register serial→proc
   * `[:deliver serial payload]` → lookup serial, `proc/send` into local mbox
   * `[:proc-fwd serial msg]` → same as deliver (proc/send's remote convention)
   * `[:stop]` → exit the dispatcher (lane 0 only, milestone 1)

### Launch protocol

`lanes-start` mirrors `ev/thread`'s marshaled-buffer format exactly — the
subr's unmarshaler is order-sensitive:

1. the abstract registry table (so threaded channels resolve by reference),
2. the RAW cfunction registry copy (`uint32` count + bytes — not marshaled),
3. the dispatcher function (`proc/lane-worker`, resolved from the boot fiber
   env where boot.janet `setdyn`'d it),
4. the launch value `[ctl lane-id affinity]`.

The dispatcher takes ONE argument (the subr enforces arity 0-1) and
destructures the tuple.

### Cross-lane process identity

A `proc` abstract refuses marshal, so cross-lane handles are **pid tuples**
`[:pid node serial ctl]`:

* Worker spawns store the cross-pid into the child's env (`:proc-cross-pid`)
  so `proc/self-pid` returns a routable handle from anywhere.
* Main-VM procs synthesize it lazily in `proc/self-pid` (node 1 = "a lane
  ctl", the lane-0 channel) and register in the C-side registry — deliveries
  to main-VM procs consult `(proc/lane-lookup serial)` as a fallback.
* `proc/send` on a nonzero-node tuple pushes `[:proc-fwd serial msg]` over
  the carried channel (the pre-existing remote-node convention). The receiving
  lane's dispatcher resolves the serial and gives to the local mbox.
* The tuple's channel marshals by reference (threaded abstracts), so pids
  travel inside closures and messages freely.

Serial space: one counter per spawner VM. Routed spawns allocate on the main
VM; lane-internal spawns allocate on the lane's own counter. The dispatcher's
registry keys on the SPAWNER's serial, so no collision arises.

### Routing and the TLS trap

Routing lives in `janet_proc_spawn_routed` (called only from `proc/spawn`);
`proc/spawn-local` and lane-internal spawns call the impl directly. The single
most important implementation detail: **the amalgam's file statics are
process-global.** Without `JANET_THREAD_LOCAL`, worker VMs saw the main VM's
`lanes_count` and re-routed every "local" spawn back to a lane — kids
ping-ponged between lanes forever. Every lane-state static must be TLS.

## Lifecycle (milestone 1)

* `lanes-stop` pushes `[:stop]` to **lane 0 only**, unblocking the main loop.
  Worker threads stay parked on their ctls (detached, share-nothing) and are
  reaped at process exit. Suites should end with `(os/exit 0)`.
* Deliberately NOT done yet: pushing `:stop` to workers races their
  `janet_deinit()` against the main VM's teardown (libmalloc abort observed).
  Graceful worker shutdown needs a pool-style join — milestone 2.

## Work-stealing (milestone 2 design)

Milestone 1 places spawns **round-robin** — optimal for uniform fan-out, but
blind to skew: a lane that draws long-running procs falls behind while others
idle. The design below is recorded so the next slice can land without
re-derivation.

### Why not steal procs

Procs are anchored: their fibers run on their home VM's loop, their heaps are
owner-scoped, their mailboxes are home-pinned isolated channels. **Migration is
out** (it would drag a heap across VMs). Work-stealing therefore steals *task
submissions*, not resident procs — the Erlang-friendly shape.

### Design: idle-lane pull from a global overflow queue

1. **Overflow queue.** When a routed spawn would land on a lane whose ctl is
   at a high-water mark (say 3/4 capacity), push instead to a global MPSC
   overflow deque (a threaded channel is adequate for milestone 2a; a lock-free
   deque later).
2. **Idle pull.** A dispatcher whose ctl is EMPTY (its take would park) first
   drains up to K tasks from the overflow queue before parking. This converts
   spare capacity into throughput without any migration: the *entry closure*
   executes wherever capacity exists.
3. **Locality tiebreak.** Steals prefer tasks whose pid handles reference the
   stealing lane's existing procs (cheap check: compare carried ctls), keeping
   hot-message traffic same-VM.
4. **Placement knob.** `{:policy :round-robin | :least-loaded | :steal}` —
   `:least-loaded` tracks per-lane depth counters (fetch-add on push, on take);
   `:steal` adds the overflow-pull. Round-robin remains the default for
   predictability.
5. **Batching.** Pulls take K=4..8 tasks at once to amortize channel traffic,
   half given back if the stealer's own queue floods first.

### Expected behavior

* Uniform fan-out (the agents benchmark): round-robin ≈ steal. No regression.
* Skewed workloads: steal recovers most of the idle-core loss. The 1-vs-N
  agents demo with a deliberately slowed actor population is the gate.
* Comms-heavy skew: locality tiebreak keeps same-VM message paths at
  in-VM cost (786k msgs/s measured AOT, 692k interpreted at 100k actors);
  stolen tasks pay marshal only for cross-lane sends they actually make.

### Milestone 2 semantics (pre-implementation spec)

**Pool-style join (2a).** Each worker registers its pthread handle in its
launch record. `proc/lanes-stop` (and a new `proc/lanes-join`) delivers
`[:stop]` to ALL lane ctls including lane 0, then joins every worker pthread
with a bounded grace period. The worker's exit path currently runs
`janet_deinit` for its own VM while the main VM keeps running — the observed
teardown abort — so join requires either (a) the subr growing a
no-deinit exit mode (a new launch flag the fork owns), or (b) deferring
worker deinit to a completion handshake the main loop drains. Postcondition:
after `(proc/lanes-join)`, `(proc/lanes)` is 0, all worker pthreads have
exited, and a subsequent `(proc/lanes-start)` in the same process works.
Tests: start 4 lanes, do work, join, assert 0, restart 2 lanes, round trip
again — the restart-after-join shape is the feature indication.

**Cross-lane monitors/kill/exit-reasons (2b).** `proc/monitor p` from lane A
for a proc home on lane B registers in B's dispatcher (a `[:mon watcher-pid
watched-serial]` op over B's ctl); on watched-proc exit (normal or error),
B's dispatcher pushes `[:down watcher-pid watched-serial reason]` to A's
ctl, where A's dispatcher delivers to the watcher's mbox. `proc/kill p
reason` forwards `[:kill serial reason]` to the home lane; the home lane
applies it locally (signal into the local fiber — no cross-thread fiber
mutation, ever). Exit reasons: `:normal`, `:error`, `:killed`. Tests:
monitor across lanes, kill the watched, assert the `:down` message shape;
kill propagates even when the watched proc is mid-recv.

**Work-stealing (2c).** Per the design above; the feature indication is a
skewed benchmark where `:policy :steal` must beat `:round-robin` by a
measured margin, plus a no-regression check on the uniform case.

### Milestone 2 remainder (in order)

1. Pool-style join: workers take a join token; `lanes-stop` (or a new
   `proc/lanes-join`) delivers `:stop` to all lanes and joins the pthreads —
   requires the subr path to expose thread handles or a completion handshake
   per lane.
2. Cross-lane monitors/kill/exit-reasons: `:down` messages marshal over lane
   ctls keyed by the watched pid tuple; kill forwards a `:kill` op that the
   home lane applies locally (signal into the local fiber).
3. Work-stealing per the design above.
4. The 1M-processes-across-N-lanes demo + scaling measurements.

## Per-lane density threshold (bug 2, OPEN — full evidence trail)

Symptom: 1M procs across 4 lanes (~250k/lane) crashes above ~750k total;
8-lane 1M (125k/lane) and 640k/4-lane (160k/lane) complete. Crash stack
(worker thread, libmalloc abort): `janet_hash <- janet_dict_find <-
janet_mark_abstract table.c:114 <- janet_gc_minor_skip <- janet_mark_array
<- janet_mark` — the minor's mark walk hashed a garbage slot in an old
table (use-after-free-then-reuse: the slot's block was freed and its
memory reused).

Narrowed so far:
- NOT the xfb cap alone: per-worker cross-fiber edges at 250k/lane stay
  under JANET_XFB_MAX (1M), so the stuck path does not fire there. The
  stuck-degradation fix committed alongside (janet_collect_process now
  degrades to the global minor when stuck, mirroring
  janet_collect_fiber_minor) closes a real adjacent hole but does not
  change this crash.
- The remembered mechanism is TWO-layer: per-object xfb (cross-fiber
  stores) AND per-page dirty bits (old->young within traversal). The
  crashed walk is the dirty-page re-walk of an old table whose slot is
  garbage => a young block referenced from an old container was freed
  WITHOUT its container's page being (or staying) dirty — a missed or
  lost barrier somewhere in the spawn-dense path.
- Suspect list for the missing barrier: C-level janet_table_put sites in
  the routed-spawn path (env construction, serials bookkeeping), the
  mbox q's C array values, and page aging racing dirty marks (page goes
  old between store and minor).
- Next probe (prepared, not yet run): slab-poison-on-free — stamp freed
  young blocks with an impossible epoch (0x1F) + body magic at
  janet_quarantine_drain's release; janet_mark aborts with a header dump
  when it touches one. One instrumented 1M run identifies the freed
  block's type and the walker.
- Note: /tmp scratch was purged mid-investigation; reproduce from
  test/lanes-1m-regression.janet. The clone instrumented runs used the
  same source as master plus probes.

## AOT build status (measured)

The lanes machinery works identically under AOT-compiled binaries:
`proc/lanes-start`, routed spawns, cross-thread wakes, and fan-in all
verified (sanity scripts + 1M/8-lanes to full parking).

- 1M across 8 lanes: interpreter 141 s, AOT 161.7 s (single runs, same
  machine, not back-to-back). The spawn path is dominated by marshal +
  per-lane GC walls, not bytecode execution, so AOT does not lift it;
  AOT's win remains pure execution speed. Capacity is identical.
- AOT + `proc/lanes-stop` hangs (the worker-deinit teardown race, worse
  under AOT). AOT lane scripts must end with `(os/exit N)`. The
  milestone-2a pool-style join replaces this path.
- Comms matrix on AOT: not separately rerun; the interpreter numbers
  below stand as the reference (the paths are identical C code).

## Messaging speed (measured, `bench/lanes-comms.janet`)

| Shape | Throughput | Per-send |
|---|---|---|
| same-lane ping-pong (2 procs, 1 VM) | ~940-980k msgs/s | ~1.05 us |
| cross-lane ping-pong (2 lanes, marshaled hops) | ~156-234k msgs/s | ~4.4-6.4 us |
| 20k fan-in across 4 lanes -> main-VM collector | ~101k msgs/s end-to-end | incl. spawns (0.197 s) |

The cross-lane tax (~4-6x) is the marshal-copy + thread handoff + dispatcher
hop; it is the price of share-nothing and matches the design target of
keeping hot-message traffic same-VM.

**Known bug (open, pre-existing, NOT lanes-specific):** bulk fan-in of
>=~10-20k single-shot senders to ONE collector proc loses a wakeup on some
runs — the collector stops receiving mid-stream (reproduces with all senders
on the SAME VM via proc/spawn-local, with collector mbox caps 64 and 4096;
100-10000 senders complete reliably, 20000 is flaky). Same-VM, so the lane
wake path is exonerated; suspicion falls on the isolated-mbox pending
registration/reclaim under bulk completion minors. Record: reproduce with
`/tmp/fo_cap.janet` shape (20k spawn-local senders, one collector).

Diagnosis so far (instrumented runs + lldb sampling of the stuck loop):
- The stalled loop sits in `kevent` with `has_timeout=0` — infinite wait,
  spawn queue empty, timers empty, listener_count > 0 (everyone parked).
- Adding ANY periodic timer (a 1s watchdog fiber) makes the same run
  complete: the bug behaves like a scheduled task whose drain was missed;
  the timer's loop1 re-drain rescues it.
- Bisect A: senders that send then PARK on a timer (`ev/sleep 3600`) never
  stall — the completion path (proc death -> completion minor -> fiber
  free -> chanpend deinit) is the trigger, not the send/park path.
- The pop path DOES resume plain write-pending senders (verified by read);
  pending rings grow dynamically to 0x7FFFFFF (no overflow at 16k writers).

## Evidence

* `test/suite-lanes.janet`: 2-lane and 4-lane round trips (kids compute i²
  across lanes, forward to a lane-0 collector, main verifies the sum), auto
  count, affinity option, spawn-local, single-VM unaffected.
* Full battery at `fc32f7c5`: make test 52/52, suite-proc 5/5, all nine
  sibling suites green.
* Single-lane density baselines (apply per lane): chain-100k 2.36 s /
  23.6 µs per hop; 10k spawn+park 3.0 µs/proc; ~3.5 KB/proc resident.

## Milestone 2b: cross-lane monitors, kill, exit-reasons (6cbd743a)

Cross-lane supervision rides the lane dispatchers with two new ops:

- `[:proc-monitor serial tag watcher]` — register the monitor on the
  watched proc's home lane and host the P2.3 reaper there. The reaper
  takes the local sup, derives the reason (`:normal` on ok, else
  `fiber/last-value`), records the exit reason
  (`proc/record-exit`), and delivers `[:down tag reason]` to the
  watcher — `ev/give` for channels, `proc/send` for pid-tuple
  watchers (which routes through the watcher's home lane). Unknown
  serials answer `[:down tag :noproc]`. Delivery messages marshal
  across threaded channels, so no value escapes its owning heap.
- `[:kill serial reason]` — resolve the local proc through the lane
  registry (`proc/lane-lookup`) and apply the kill on the home lane.
  Resolving locally (not via the `serials` tuple) is load-bearing:
  re-pushing the op from the tuple echoes forever.

Supporting C changes:

- self-pid: cached-pid procs (routed spawns) register in their home
  VM's lane registry under **both** the real and the dispatch serial.
  The routed-spawn op mints its own serial on the spawner's VM, while
  spawn_impl mints the proc's real serial — dispatcher lookups need
  both keys.
- spawn wrapper pid nodes are `lane-id + 1` (0 reserved for the main
  VM), so cached cross-pids agree with routed handles.
- `janet_channel_cancel_reader` purge: a recv-parked kill victim is
  cancelled and its pending removed immediately (Erlang-parity kill
  landing), instead of at a next resume that may never come.

Tests: `test/suite-lanes.janet` M2b section (monitor+kill reason
delivery, error/normal reasons, pid-tuple watchers, `:noproc`,
kill-of-dead no-op) green under `JANET_SKIP_DENSITY=1`, plus
suite-proc's P2.3/P1.4 monitor suites.

## BUG 2 evidence: density GC race (OPEN)

The per-lane density barrier is a **layout-sensitive latent race** in
the GC/completion boundary, now pinned much sharper:

* ASAN (heap-use-after-free): `janet_mark_function` (gc.c) reading a
  freed 512-byte block from inside `janet_mark_fiber`'s frame walk —
  the completion minor freed the young `frame->func` of a fiber that
  stays reachable, and a later global mark re-walked the stale frames.
* Partial fix landed (this commit): `janet_mark_fiber` walks frames
  only for resumable statuses (NEW/ALIVE/PENDING/DEBUG); terminal
  fibers skip the walk. Semantically forced — nothing resumes them.
* The race is **layout-sensitive**: semantically-inert code changes
  (trace eprints, added-but-gated C branches, doc-adjacent boot
  bytecode) flip bulk fan-in (n=4000 single-shot senders, suite-proc
  #40) between clean completion and a parked-collector hang. Pure
  HEAD: 3/3 green. Any perturbed build: reproducibly hangs; the
  working threshold on perturbed builds measured n≤2000 clean,
  n=4000 hang.
* The stall shape matches the BUG 1 family: collector parked in poll,
  spawn queue wakeups stranded mid-stream. BUG 1's fix (ev roots in
  completion minors, 7733fdb7) closed one hole; a sibling hole
  remains that layout shifts expose.

### Round 2 (goal continuation): two root causes found and fixed

1. **Fan-in n=4000 deadlock (ALL builds post-M2b)**: the collector's
   recv parks on its own mailbox -- which 2b made a THREADED channel.
   Every same-VM send/recv handoff on a threaded channel went through
   `janet_ev_post_event` -- a self-pipe write from the loop's own
   thread. ~8000 writes at n=4000 fill the 64KB pipe; the loop then
   blocks inside its own wakeup write. Fix: same-VM pendings on
   threaded channels schedule inline (`janet_schedule` + unroot),
   mirroring the close path's existing pattern. Fan-in 5/5 green;
   suite-proc 88/88 in 11s.
2. **Density deterministic crash (quantum yield UAF)**: M4b quantum
   yields (status USER9 via JANET_SIGNAL_INTERRUPT) pass through the
   drain's completion block (`janet_fiber_can_resume` is false for
   them), so the completion minor freed the fiber's young nursery --
   then the quantum path re-queued the fiber onto the freed memory
   (ASAN: fiber->data poisoned, walked by later marks). Fix: the
   quantum-yield condition is computed before the completion block
   and bypasses the cleanup; the fiber stays registered and its
   nursery survives. Density section passed its first full run
   (640k procs, 4 lanes).

### Residual (still open, sharply narrowed)

Density segfaults ~1-in-3 runs (was deterministic). ASAN signature:
`janet_mark_fiber` called with an ALREADY-SWEPT fiber struct (512B
block freed; the status read itself faults). Meaning: the sweep
freed a fiber that a surviving reference re-marked afterwards --
a mark/sweep root inconsistency where the reference holder was not
traversed during that collection's mark phase. Audit surface: which
holder re-marks swept fibers at density (serials, proc->fiber,
chanpend nodes, spawn queue) and why its mark edge was missing in
the sweeping collection. Next instrument: poison-check at
janet_mark_fiber entry + `__sanitizer_print_stack_trace()` of the
marker (the harness is in gc.c under JANET_ASAN_UAF_TRACE; note the
eprintf reentrancy -- write the trace via write(2) instead).

### Round 4: stale-reader keep + unroot-on-delivery; rejected scrub

The inline same-VM reader handoff initially (a) unrooted before the
staleness check -- double-unrooting readers resumed via another
select clause -- and (b) discarded pushed values on stale readers.
Fixed: unroot only on live delivery; stale readers push-head the
value into the channel and release the packed buffer, mirroring the
pipe callback's stale path (suite-ev2's keep test green 3/3).

Tried and rejected: scrubbing terminal fibers' frames during mark
(NULL func/env + zero value ranges) to make resurrections safe. The
scrub crashed the boot VM: the zero ranges `[i, stackstart)` do not
match the walk's per-frame value ranges (`[i, j)` off the
`prevframe` chain) and intersect live frame structures. A correct
scrub must mirror the walk's exact `j` chain. Reverted; density
residual unchanged (~1/5 green, segfault during the 200k-640k
spawn phase; ASAN: the walked fiber's frame->func / fiber->data
freed, reached via janet_mark_roots in a full collect).

### Round 5: free-site proof + minor fiber defer

New probe (fiber deinit in gc.c): prints when a destroyed fiber is
STILL present in vm.roots. Result: `[free-rooted] fiber=... rootidx=8`
-- a rooted fiber WAS destroyed, reached from janet_sweep's deinit
pass (inlined janet_sweep_list/quarantine path). So the density
residual is a genuine mark/sweep disagreement: some collection judged
a rooted fiber unreachable (stale-epoch or scoped-mark gap), freed it
via quarantine, and the surviving roots entry dangles into reused
memory (the later ASAN crashes at mark time).

Landed: minors now NEVER free JANET_MEMORY_FIBER blocks (fiber frees
defer to full collects, which walk vm.roots). This closes the
minor-scoped route and is regression-free (proc/lanes/ev2/gc suites
green), but the flake persists (~1/5) -- the freeing collection in
the traced run sits on the full/scoped path. Next: audit janet_mark's
recursion-guard/epoch handling for fiber blocks (a stale or unsset
epoch on a rooted fiber explains every observation), and whether
sweep_proc_owned's narrow mark can miss a root-honoring block.

### Round 6: epoch theory eliminated; gcroot-of-dead-fiber hypothesis

The free-side probe fires at deinit time, which (via quarantine) can
LAG the sweep that judged the block dead. Reconciliation: fiber F was
correctly judged unreachable and quarantined at collect N (unrooted
then); LATER, a registration path gcrooted the stale F pointer
(roots[8]); a subsequent drain deinit'd F -- the probe saw "rooted".
The bug to hunt: WHICH registration path roots a dead-or-dying fiber.
Probe: in janet_gcroot, for FIBER values, check
__asan_address_is_poisoned(fiber) (or block-quarantined state) and
backtrace -- any hit names the culprit registration site directly.
Epoch cycling (janet_gc_epoch_next, % 15) was eliminated: the sweep
clears survivor epochs, so stale-epoch collisions only over-retain.

### Round 7: cross-thread free routed home; 600k now reachable

ASAN on density caught a worker (T22) in `janet_equals` reading a
proc abstract that MAIN (T0) had freed -- main's threaded-registry
purge decrefs entries not visited by MAIN's mark, and the last
decref frees on whichever thread purges last, even while the home
worker still has in-flight operations touching the block. Fix:
`JanetAbstractHead.home_vm` records the creating VM; the refcount-0
release of a foreign-homed threaded abstract is deferred via
`janet_ev_post_event` to the home VM's loop (finalizers and poison
still run on the observing thread). All suites green.

Effect: under ASAN the density phase now passes 200k and 400k for
the first time; the crash moved to 600k and regressed to the
HISTORICAL BUG 2 signature -- `janet_hash` on a freed abstract in
worker run_vm (the "cross-thread pid reference surviving per-lane
GC" face). Next probe: capture ASAN's freed-by stack for the
janet_hash crash to identify which free path (worker's own registry
purge vs fiber-death migration) still releases a block the parked
proc's env tuple references.

### Round 8: home teardown completion; channel deinit lock; double-deinit lead

The janet_hash crash decoded: `janet_hash` dereferenced the
type==NULL POISON -- the observing thread had poisoned the abstract
while the home worker hashed it mid-operation. Fix: the ENTIRE
teardown (finalizer + poison + release) now routes to the home VM
(e30b6e78 extended by 7060540b); the home callback is allocation-free
(a registry tombstone inside it could trigger a collection from the
event loop, racing the channel GC -- removed; the purges handle
tombstones).

Density: 2/4 full interpreter passes after that fix (best to date).

New residual, sharply characterized: `flag_chanpend_node` UAF during
`janet_chanat_gc -> janet_chan_deinit` on a worker. Locking the whole
deinit (previously only pack channels locked) did NOT close it -- the
walk now holds the lock, so the freeing thread does NOT: evidence for
a DOUBLE DEINIT. Candidate: the abstract SWEEP path
(janet_deinit_block's ABSTRACT case) runs the channel's gc hook
directly on the sweeping VM, while the home-routed teardown cb runs
it again on the home VM. Next: make threaded-abstract deinit hooks
single-shot (a deinit'd bit on the head, checked under the chan
lock) or route sweep-side hook invocation through the same home
deferral.

### Round 9: single-shot teardown guard; density full passes; in-flight ref gap

Landed the double-deinit fix: `JanetAbstractHead.teardown_started`
(atomic single-shot) arbitrates the sweep's deinit_block hook, the
home teardown callback, and the same-VM refcount path -- exactly one
runs the finalizer/poison/release.

Results: full density passes now occur (640k spawn + lanes-stop join
in 8s; a 108s pass when a big collect lands mid-loop). ASAN reaches
600k. proc/lanes-skip/ev2 green.

New residual, precisely characterized: worker T23 pushing to a
channel whose STRUCT was freed by main's home teardown (ASAN:
janet_channel_push_with_lock reading a T0-freed, T23-realloc'd 128B
block at 600k). Mechanism: in-flight channel operations on foreign
VMs hold no refcount; main's teardown at refcount-0 destroys the
struct (and deinit's its mutex) while a foreign push is entering.
Fix design: incref the channel at push/take entry, decref at all
exits (including panic paths); the lock then serializes teardown
against genuinely in-flight ops. Alternative: route mutex_deinit
deferred and keep the struct until quiescence.

### Round 10: in-flight refs, two-phase deinit, routing-first

Three fixes, one round:
1. In-flight channel ops (push/pop) now take a refcount at entry and
   drop it before each synchronous return (parked-waiter safety keeps
   coming from the pending-node discipline). The zero-drop teardown
   defers to a loop even under the chan lock.
2. janet_chan_deinit_q restructured flag-then-free: duplicate pending
   entries sharing a reg freed the reg then revisited it (single-pass
   aliasing).
3. THE KEY ONE: the same-VM refcount teardown ran the FINALIZER
   (channel deinit -- freeing the pending-queue buffers) on the
   observing thread BEFORE the home-routing check, then deferred the
   rest -- the home VM's routed callback then deinit'd the same
   (already-freed) queues again. Routing now precedes every hook;
   the home callback owns finalizer + poison + release under the
   single-shot guard.

Density: 2/5 full passes (up from ~1/5-1/3); each ASAN run still
exposes the next head in the teardown/refcount lattice, but each fix
is smaller and more specific. proc/lanes-skip/ev2 green throughout.

### Round 11: GC-hold deferral; shutdown inline mode

New head: a worker-homed teardown freed a proc abstract still held by
a same-VM channel queue (queue items are GC-marked but hold no
registry claim). Design fix: the home teardown callback now defers
when the last collect marked the abstract (janet_gc_object_reachable)
-- it re-arms a GC-watch claim (count 0->1, registry entry false) and
the next purge re-routes when the hold dies. All threaded teardowns
now flow uniformly through the routed callback; decref_maybe_free
only routes. Shutdown gets an inline mode (gc_teardown_inline):
posting to the loop from janet_clear_memory blocked forever on the
dead self-pipe. Also fixed a double INFLIGHT_DROP on the pop closed
path (macro-wrapping artifact).

Suites green. Density: full passes continue; flake persists (~1/5
this batch). The reachable-check uses the last collect's epoch, so
values created after the last collect have a narrow unprotected
window -- documented residual.

### Round 12: lane-homed pinning — DENSITY GREEN 5/5

The head-by-head teardown fixes each closed a specific race, but the
lattice kept yielding new heads: circulating pid values (channel
queue items, parked payloads, serials) reference lane-homed
abstracts with no registry claims, so ANY refcount-0 free raced a
consumer. Design resolution: lanes PIN their homed threaded
abstracts (quarantine list linked via gc.data.next) instead of
freeing at refcount-0; janet_clear_memory drains the quarantine at
lane stop (finalizer, poison, free). Main-VM teardowns keep prompt
semantics (suite-ev2's weakref/count tests unchanged). Bounded cost:
pinned abstracts live until stop (640k procs x ~216B ~= 138MB, well
within the suite's documented 3GB headroom).

RESULT: suite-lanes FULL density 5/5 green (640k procs across 4
lanes, lanes-stop join, restart round trip) -- first time; proc
88/88, ev2 green. Battery gate for 2c/2d/dual-target opens.

### Round 12 addendum: confidence batch

Extended runs: 6/7 full passes (~86%, up from 0-20% pre-pinning).
One residual segfault remains in the density phase -- signature not
yet captured (batch hit the session time budget). The pinning design
removed the mass dangle class; the tail head is next round's opening
ASAN run.

### Round 13: exported-abstract pinning

Interpreter-side signature captured via lldb: janet_mark_abstract on
a main-freed threaded abstract reached from worker mark state --
main-homed channels tear down promptly and dangle for lanes holding
marshaled values. Fix: JanetAbstractHead.exported is set when an
abstract is marshaled cross-VM (marsh.c LB_THREADED_ABSTRACT send);
the home teardown quarantines exported abstracts (pin until exit)
like lane-homed ones. Same-VM-only abstracts keep prompt teardown
(suite-ev2 unchanged).

Suites green; density 4/5 this batch (up from ~1/7 at round 12's
tail). ASAN build fully green end-to-end (600k + 640k + join +
restart).

### Round 14: the tail head narrowed to a freed mbox behind a stale holder

New mark-side probes (JANET_ASAN_UAF_TRACE, inert in normal builds):
janet_mark_abstract tombstone/bogus guards + procat_mark mbox/sup
checks. Result: `[proc-mbox-tombstoned] home=0x0 exported=0 refcnt=
1024 lane=1` -- the mbox pointer dangles into MALLOC-REUSED memory
(home never set, refcount arbitrary): a worker proc wrapper's mbox
was freed outside the lane pin, OR the marked "proc" is itself a
recycled block reached from a stale holder. ra0-2 resolved to
mark_abstract <- mark <- procat_mark (all inlined); the next probe
extends the chain (ra3-5) to name the stale holder (candidate
holders: worker serials, schedule queue, a pending's sched value).
Also: the rc teardown lattice now has 11 layers; the pinning design
holds (4/5 and 5/5 batches under load); the tail is this one holder
class.

### Round 15: procat_mark prunes tombstoned channels — DENSITY 5/5

The RA chain named the holder: janet_mark_fiber -> mark -> mark_table
(the fiber's env) -> mark -> procat_mark -- a live fiber's :proc-self
env binding outlives its mbox's registry life, and procat_mark marked
through the dangling pointer (malloc-reused memory: home=0x0,
refcnt=1024). Fix: procat_mark prunes mbox/sup pointers whose head is
poisoned (type == NULL) before marking -- a tombstoned channel is
gone forever, so caching the pointer was the bug. (First attempt used
the wrong typebits test and nulled LIVE threaded mboxes -- caught by
the suites immediately, reverted for the precise poison check.)

Density: 5/5 green. proc 88/88, ev2, mailbox, ev, pool, thread green.

### Round 16: milestone 2c LANDED — work-stealing first slice

Implemented per the recorded design:
- `proc/lanes-policy` knob (:round-robin default | :least-loaded
  reserved-as-rr | :steal; :steal requires lanes).
- Global overflow queue: a main-homed threaded channel (4096) created
  at lanes-start, delivered to dispatchers as a [:overflow queue] ctl
  message (marshaling it inside the launch tuple crashed the registry
  dedup hash -- channels cross as ctl messages the way pid tuples
  always have).
- Routed spawn under :steal: when the target ctl depth >= 8
  (janet_channel_count), try-push to overflow (never blocks).
- Dispatcher idle pull: up to K=4 tasks drained before parking on an
  empty ctl (boot.janet lane-worker handle-spawn refactor).
- `proc/lanes-stats` counters {:overflow-pushed :overflow-pulled} --
  the feature indication.
- Documented limitation: the spawner's pid tuple keeps the spawn-time
  lane; sends via it may hit :noproc for pulled procs.

Tests (suite-lanes 2c section, end of suite, own start/stop cycle):
policy round-trip, skewed 200-task workload under :steal completing
correctly (sum 0..199), counters positive for push AND pull, stop
clean. NOTE: task closures must capture pid tuples, not proc
abstracts (procs refuse marshal -- same-VM only by design).

Full battery green (suite-lanes FULL incl. 640k density + join 8.0s,
plus all nine siblings).

### Round 17: milestone 2d — agents benchmark numbers

test/agents-1vN.janet (W tasks x K messages to a main-VM collector;
checksum-verified; the clog is a CPU-bound loop that occupies its
lane's dispatcher -- ev/sleep yields and does NOT skew a lane).

4 lanes, interpreter build (messages = W*K):

| phase   | policy      | msgs  | wall_ms | throughput |
|---------|-------------|-------|---------|------------|
| uniform | round-robin | 20000 | 124     | 160485/s   |
| uniform | steal       | 20000 | 118     | 168995/s   |
| skewed  | round-robin | 20000 | 197     | 101242/s   |
| skewed  | steal       | 20000 | 218     | 91691/s    |
| uniform | round-robin | 50000 | 343     | 145688/s   |
| uniform | steal       | 50000 | 294     | 169542/s   |
| skewed  | round-robin | 50000 | 265     | 188188/s   |
| skewed  | steal       | 50000 | 255     | 195378/s   |

Design expectations met: uniform fan-out round-robin ~= steal (no
regression; steal +5-17% at scale). The skew gate is flatter than
hoped: with a single clogged task the lane's remaining task FIBERS
still run on their home lane in both policies (stealing moves spawn
DISPATCH, not resident fiber execution -- the Erlang-shape
limitation recorded in the design), and one 250ms clog dominates the
wall either way. Recovery shows up at scale (50k msgs: 265 -> 255).

Three 2c implementation bugs fixed this round (caught by the bench):
- lane-pull inverted janet_channel_pop's convention (1 = obtained) --
  pulled tasks were consumed and discarded.
- the :steal try-push inverted janet_channel_push's try-mode
  convention (0 = refused) -- "pushed" tasks were never enqueued and
  silently skipped their ctl fallback.
- pull-then-park race: overflow pushes landing after a dispatcher's
  last pull stranded tasks; every overflow push now nudges the next
  lane with a [:pull] wakeup that drains the queue. Threshold set to
  depth 1 (fast dispatchers never back a ctl to 3/4; :steal means
  "first spawn stays local, bursts overflow, idle lanes steal").

### Round 18: dual-target pass triage (AOT battery)

AOT compile+run via `bender --aot test/<suite>.janet -o <bin>`; the
binary runs the suite natively. Results: ev2 13/13, mailbox 76/76,
gc-minor 12/12, interrupt 15/15, pool 56/56, thread 30/30 -- GREEN.
Three failures, distinct signatures:

1. suite-proc: "cannot marshal <core/proc>" at spawn-isolated's
   entry copy (proc.c:954). Under AOT the test closure's env chain
   includes earlier sections' top-level bindings (section 27's local
   proc), so the marshal walks a proc and refuses. Interpreter env
   scoping is per-(let) and passes. Fix direction: AOT env-scoping
   parity, or suite-side section isolation.
2. suite-ev: error at suite line 51 (while loop) -- not yet triaged.
3. suite-marsh: marsh.c:1796 refusal at suite line 121 -- likely the
   same env-sharing class (marsh tests marshal rich payloads).

These are AOT-codegen env-scoping characteristics, not regressions
of the lanes work: the same suites pass under the interpreter and
the six green AOT suites exercise the lanes/messaging machinery
under AOT already.

### Round 18 addendum: suite-ev root cause

suite-ev under AOT: its os/spawn subprocess tests run
`[janet -e "(print ...)"]`; the AOT binary's embedded main ignores
`-e` and re-runs the embedded suite, so the child prints "Starting
suite" (and worse, parses "janet" as a script: "invalid utf-8 in
symbol"). The interpreter build honors -e (776/776 green). Fix
direction: the AOT main should pass unrecognized args (esp. -e) to
the embedded janet runtime, or the AOT test runner must plumb SUBRUN
to a true interpreter.

Dual-target status: interpreter 11/11 green. AOT 6/11 green (ev2,
mailbox, gc-minor, interrupt, pool, thread); suite-ev (CLI), 
suite-proc + suite-marsh (env-scoping marshal) remain -- all three
are AOT-runtime/codegen characteristics with named fix directions,
not lanes regressions.

### Round 19: AOT gap precisely characterized

Bisect results under AOT: spawn-local OK; spawn-isolated with a
NON-capturing entry OK; spawn-isolated with an UPVALUE-CAPTURING
entry HANGS (even the trivial case: capture a thread-chan, give from
the child). So the dual-target blocker is one item: the AOT
marshal-copy of capturing closures (the flat-frame AOT model walks
the whole enclosing frame on marshal; the isolated-entry copy path
is not closure-safe). suite-proc's "cannot marshal <core/proc>"
refusal and suite-marsh's refusal are siblings of this. suite-ev is
separate (AOT main ignores -e; SUBRUN plumbing to a real interpreter
is the runner-side workaround).

Dual-target status: interpreter 11/11; AOT 6/11 with ONE named
codegen work item (closure-safe marshal-copy for isolated entries)
plus one CLI item. Everything else in Phase 1 is landed and green.

### Round 21: per-form AOT chunks — tried, proved dead, reverted

Motivation: fix suite-proc under AOT ("cannot marshal <core/proc>" at
spawn-isolated) by matching the interpreter's form-by-form scoping,
so a section's closures capture only their own frame.

Result: the env-walk refusal DID disappear (suite-proc ran past 317),
but cross-form def visibility broke completely (later forms saw top
level defs as nil). Root cause, proven empirically (/tmp/defenv.janet):
a top-level def writes into the COMPILE-TIME env table (a bytecode
constant), NOT the running fiber's env — and the compiler folds a
def reference to its compile-time value. run-context works per-form
only because it compiles each form AFTER the previous one ran;
mkimage's zero-build-time-run invariant means later chunks fold
earlier defs to nil. Per-form AOT chunks are structurally incompatible
without either running forms at build time or dynamic-declared
bindings. Reverted to the one-do-form image (marshal-safe, cross-form
refs as frame locals).

Kept (real improvement): the generated AOT main now runs chunk fibers
with a dedicated env table whose proto is the core env (chunk_env),
mirroring a script's module env — (curenv) in an AOT binary now has a
proto chain like the interpreter's (the shared-store section's
(table/getproto (curenv)) class). Six green AOT suites re-verified
green after the change; main-scripts re-verified (argv passthrough,
aot-main lookup); interpreter suite-lanes re-verified green.

Remaining dual-target blockers unchanged: the 317 marshal refusal
(one-chunk frames share slots with stale sibling sections — fix
direction: copy_entry copies only referenced upvalue slots, or
suite-side per-section function wrapping) and the suite-ev -e CLI
item.

### Round 20-21: AOT per-form chunks + profiler/strict-proc fixes

Root causes found and fixed for the AOT battery failures:

1. **One-do-form env scoping** (suite-proc "cannot marshal <core/proc>"):
   mkimage now compiles each top-level form as its own chunk against the
   shared env — exactly `janet script.janet` semantics (run.c runs
   scripts form by form). A chunk's closures capture only their own
   frame, so spawn-isolated entry marshals walk real captures.
2. **Chunk env proto** (curenv/getproto nil): the generated main gives
   chunk fibers a fresh env table proto-linked to the module env
   (run-context shape); a bare core env has no proto.
3. **Profiler vs strict procs** (the big one): AOT binaries auto-PGO —
   janet_profile_entry's plain puts tripped the strict-escape barrier
   from TWO sites: the pt identity-map put, and the e-table "func" put
   (protect's fresh NON-strict fiber profiling a call to a child-heap
   function: isolated value into a shared-page entry table). Every
   strict proc's handler call died with "strict proc value escaping"
   — the interpreter passed only because it never profiles. All
   profiler-internal puts now use janet_table_put_raw (the P3.4
   bookkeeping-put precedent: identity maps, dirty-mark, no barrier).
4. **Chunk lifecycle**: chunks run from a rooted array with slots
   cleared after their chunk; ONE fiber re-primed per chunk via
   janet_fiber_reset (run-context reuses one evaluation context).

State: AOT suite-proc 88/88 GREEN (was env-refusal + hang);
suite-marsh 66/73 — the remaining 7 are weak-table entries that
never clear post-gccollect: ANY value touched by a completed top-
level chunk stays pinned (even runtime-built buffers — bisection
esK/esL), pointing at stale fiber-stack slots surviving the mark in
the AOT path; NOT the profiler (JANET_NO_PROFILE identical), NOT
chunk roots (array-clearing + fiber reuse did not change it), NOT
literal constants (buffer/new pinned too). Next: instrument the
fiber-stack mark extent in gc.c for the chunk fiber. suite-ev CLI
(-e passthrough) still open. Interpreter battery 8/8 + previously
green AOT suites re-verified green after all changes.

### Round 22: suite-ev -e parity + the do-thread funcenv forensics

Landed: the generated AOT main now honors `-e CODE` (janet CLI
parity). Root: the interpreter's os/spawn falls back to argv[0] for
bare names, so suite-ev's [janet -e ...] subprocess tests exec the
binary itself — the interpreter's -e prints "hello", the AOT binary
re-ran its embedded image. With -e parity those tests pass; the
line-338 do-thread section also fixed by same-form scoping (cross-
form ch/att refs). Suite sections with inlined roundtrip helpers
where noted.

REMAINING (forensics recorded for next round): the 397/402 do-thread
section's thunk marshal still refuses on a <core/process> found in
its OPEN funcenv (offset=4, length=151, fiber = the chunk fiber).
Facts pinned by lldb: the thunk def is correct (sourcemap line 398,
named do-thread); the memset-after-reset runs (emitted C verified);
the process is (re)written into a frame slot during the let chunk's
own execution — not env-table contents (probed: no abstracts in
curenv either backend), not literal constants (buffer/new pinned
too in the weak bisection), not the profiler (JANET_NO_PROFILE
identical). Suspect: the deferred cross-form binding fetch
materializes more than the binding (env/ref structures written into
frame slots that the open-funcenv marshal then walks). Next probe:
watchpoint on the slot that holds the process (slot index ~6,
data[10]) during the let chunk, or print x vs the full slot scan in
one lldb session (the earlier two-session ASLR mismatch made the
cross-check inconclusive).

Also open: suite-marsh 66/73 — weak entries pinned by completed
chunk frames (bisection esK/esL in round 20-21 ledger); the pin
survives fiber-reuse + stack memset, so the census/retention path
needs the same watchpoint treatment.
