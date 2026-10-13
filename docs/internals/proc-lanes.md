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
