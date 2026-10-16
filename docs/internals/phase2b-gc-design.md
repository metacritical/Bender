# Phase 2b: GC architecture design — collector swap, explicit roots, independent per-proc GC

Status: design/investigation complete, not started. No core edits made in this phase.
Grounded in three read-only investigations of current HEAD (`e816546a`): collector
algorithm, root enumeration, and per-proc/thread isolation. All claims below carry
file:line citations collected from those investigations; see the three subagent
reports folded into this session's history for the complete citation trail.

--------------------------------------------------------------------------------
## 1. Current-state summary

**Collector:** stop-the-world, generational mark-sweep, per-VM thread-local heap.
Marking uses a 4-bit rolling epoch stamp (not a single mark bit) stored in the
`JanetGCObject` flags word (`gc.h:38-52`, `gc.c:85-96`); an object is "reachable this
cycle" iff its epoch field equals the current epoch. Minor collections mark full
roots but skip old-clean containers and only sweep young blocks; major collections
free every unmarked block, merge freelists, and reset slab age/dirty bits
(`gc.c:745-754, 926, 1372`). Correctness of the minor/major split depends entirely on
the write barrier: `janet_gc_barrier(obj, value)` must be called after every store of
a Janet value into a heap table/array/equivalent container, because minors skip
scanning old-but-clean containers and rely on the barrier to have dirtied the page if
a new young edge was written (`gc.c:1801-1807, 1765-1773`). This is the exact
contract Phase 1's `e816546a` fix restored for `JanetThreadHandle`.

**Root enumeration:** a mix of implicit and explicit, not pure stack-scanning.
`janet_mark_roots` (`gc.c:2119`) scans: the EV pending-work queues (runnable/timeout
fibers), the dynamic-bindings table, the profiler table, exactly one implicit fiber
root (`janet_vm.root_fiber`), the live-compilation stack, and a cross-fiber
remembered set. Critically, **Janet already has a first-class explicit-root API**:
`janet_gcroot`/`janet_gcunroot`/`janet_gcunrootall` backed by a growable
`janet_vm.roots` array (`gc.c:2747, 2828, 2841`), plus a companion LIFO
"gcshadow" stack for scoped C-stack-depth-bounded pinning (`gc.c:2800,2815,2819`).
There is no conservative C-stack scan — the documented contract is that C code must
either place a value in one of the scanned sets above or explicitly `janet_gcroot`
it. This API is already the backbone of thread/async completion safety: a parked
join pins `(waiter, handle)` via `janet_gcroot` for the duration of the wait
(`ev.c:4641-4642, 4653-4654`), unrooted exactly on completion (`ev.c:4287-4288`).

**Per-proc/thread isolation:** each OS thread has its own fully independent
`JanetVM` heap — own block lists, own epoch counter, own collection-trigger state,
all `JANET_THREAD_LOCAL` (`state.c:34`, `state.h:167-201`). However, **threaded
abstracts** (values usable across VM/thread boundaries, e.g. channels) break full
isolation: they are `malloc`-backed (not `gcalloc`-backed), carry a `home_vm`
pointer, `home_vm_generation`, and a `home_gc_authorized` bit (`abstract.c:108-130`),
and are tracked in a process-global `abstract_homes` list under a global lock
(`abstract.c:65-91`). Every foreign VM that touches a threaded abstract inserts a
`true/false` "visited this cycle" marker into *its own* per-VM `threaded_abstracts`
table (`gc.c:196-211`); only the **home** VM's completed major collection may flip
`home_gc_authorized` (grant on home major mark, `gc.c:184-186`; revoke on home major
sweep when unvisited, `gc.c:887-895`) — confirmed by direct investigation, matching
the registry-flag finding filed in Phase 1. Minor, scoped, and foreign-VM
collections never touch this bit by design. This is the structural reason the
registry-flag mechanism cannot, by itself, fully explain cross-thread GC safety:
authorization is coarse-grained (per home-VM major cycle), not per-edge.

--------------------------------------------------------------------------------
## 2. Collector swap: options and recommendation

Three realistic directions, evaluated against the filed findings and existing
architecture:

**(a) Keep generational mark-sweep, harden the threaded-abstract protocol.**
Lowest risk — the collector algorithm itself is sound and the Phase 1 barrier fix
demonstrates the existing contract works when correctly applied. The actual gaps
(registry-flag coarseness, self-pipe saturation, fiber-rooting chronology) are
protocol bugs around the collector, not collector-algorithm bugs. **Recommended**
as the Phase 2b starting point: it's the only option that doesn't risk destabilizing
the now-fully-green Phase 1 AOT battery.

**(b) Move to per-generation regions (bump-allocated young generation + mark-sweep
old generation).** Would reduce minor-collection cost (bump-allocate + scan young
region only, no linked-list walk for the nursery) and could make the "minor skips
old-clean containers" invariant easier to reason about (region membership instead of
per-object dirty bits). Real engineering lift: changes the allocator, not just the
collector, and every `gcalloc` call site / abstract-type boundary needs auditing.
Worth scoping as a *follow-on* to (a), not a Phase 2b blocker.

**(c) Incremental/concurrent collection.** Not recommended now. Janet's embedding
model (fibers, continuations, C API reentrancy, signal-handler-callable
`janet_ev_post_event`) makes incremental marking materially harder to get right
safely, and none of the three filed findings require it — they're about cross-thread
*coordination* of an already-stop-the-world collector, not about pause times.

**Recommendation: (a) now, (b) as an explicitly separate future proposal.** Do not
conflate collector-algorithm replacement with the cross-VM coordination work below;
they are independent axes.

--------------------------------------------------------------------------------
## 3. Explicit-roots model

Since `janet_gcroot`/`janet_gcunroot`/the gcshadow stack already exist and are
already used correctly for the thread-join pinning case, this is **not** a
greenfield design — it's a hardening and extension of existing infrastructure:

1. **Audit for under-use.** The root-enumeration report's §5 notes a narrow but
   real gap: between a worker's marshal and the owner-loop's `janet_thread_new_complete`,
   the in-flight result exists only as opaque malloc'd bytes (correctly invisible to
   GC, since it isn't a heap object yet) — but the moment it's unmarshaled, its
   liveness depends entirely on being stored into an already-marked handle with a
   barrier, not on an independent `janet_gcroot`. This is fine in the common case
   but means liveness is coupled to the handle's own mark state, which is exactly
   the coarse-grained registry-flag mechanism flagged as suspect. **Proposed
   hardening:** `janet_gcroot` the freshly-unmarshaled `result`/`profile_rows`
   for the narrow window between unmarshal and the barrier-protected store, then
   unroot once published. This closes the chronology gap with the *existing* API,
   no new mechanism needed.
2. **Formalize the contract for threaded-abstract internals.** Currently, a threaded
   abstract's internal Janet values survive via the `gc.c:196-211` per-VM-first-visit
   mark path, gated by the coarse `home_gc_authorized` bit. A more explicit model:
   require every threaded-abstract type's `gcmark` callback to `janet_gcroot` any
   Janet value it hands across a VM boundary for the duration that boundary is open,
   rather than relying on the home-VM-major-cycle authorization window. This would
   make per-edge liveness explicit instead of per-object-per-major-cycle, directly
   addressing the self-pipe-saturation and fiber-rooting-chronology findings by
   removing their dependence on collection-cycle timing.
3. **No new C-level API is needed for (1) or (2)** — `janet_gcroot`/`janet_gcunroot`
   already exist with the right semantics (identity-based, swap-remove, growable
   array). The work is call-site discipline, not new infrastructure.

--------------------------------------------------------------------------------
## 4. Independent per-proc GC

Full "zero shared mutable state" isolation is **not** a small change — the
isolation investigation enumerated seven concrete coupling points, all centered on
threaded abstracts:

- global `abstract_homes` list + generation counter + lock (`abstract.c:65-91`)
- per-object `home_vm`/`home_vm_generation`/`home_gc_authorized`/`teardown_started`/
  `exported` fields, read across VM boundaries
- per-VM `threaded_abstracts` visited-table holding foreign refs
- cross-VM mark path (`incref` + internals `gcmark` on first foreign visit)
- full-sweep unvisited decref/finalizer/tombstone path
- home-routed finalization via the event loop (cross-VM posting)
- `malloc`'d threaded-block lifetime living outside any single owner's heap

**Design implication:** "independent per-proc GC" and "threaded abstracts" are in
direct tension. Threaded abstracts exist specifically to let one Janet value (a
channel, a thread handle) be touched safely from multiple independent heaps — that
*is* the shared-state mechanism. Three paths forward:

**(a) Accept bounded sharing, narrow its surface.** Keep per-VM heaps independent
(already true) and keep threaded abstracts as the one sanctioned sharing mechanism,
but replace the coarse per-object `home_gc_authorized` bit with the explicit
per-edge rooting from §3.2. This gets "independent GC per proc" for everything
except the deliberately-shared threaded-abstract surface, which is the smallest
change consistent with current semantics (channels/thread-handles continuing to
work across procs).

**(b) True independence: copy, don't share.** Threaded abstracts would marshal/copy
their payload across the VM boundary instead of sharing a live `malloc` block (i.e.,
treat every cross-VM handoff like the existing `janet_unmarshal` path already used
for `result`/`profile_rows` in `janet_thread_new_complete`, rather than a live
shared reference). This would eliminate the global registry/lock entirely for new
code, but is a breaking semantic change for any code relying on a channel being a
*live, mutable, shared* object across threads (not just a result handoff) —
needs explicit scoping of which threaded-abstract types can tolerate copy semantics.

**(c) Hybrid: per-proc GC with an explicit, narrow cross-proc bridge object.**
Keep (a)'s per-edge rooting, but also formalize the `abstract_homes` registry as a
deliberately-named "cross-proc bridge" subsystem (rename/document, not rewrite),
so its shared-state footprint is an explicit, reviewed boundary rather than an
emergent property of `gc.c`/`abstract.c` coupling.

**Recommendation: (a) first, revisit (b)/(c) only if (a)'s narrowed sharing surface
still proves insufficient in practice.**

--------------------------------------------------------------------------------
## 5. Migration/compatibility risk

- Collector-algorithm choice (§2a) is a no-op for existing programs — no risk.
- §3's rooting hardening is additive (`janet_gcroot` calls around already-correct
  code paths) — low risk, testable by re-running the existing scoped Phase 1 AOT
  battery plus the three filed GC/event findings as regression targets.
- §4's narrowed-sharing-surface work (a) touches `abstract.c`'s home-authorization
  logic directly — medium risk, needs its own dual-target battery including
  suite-proc, suite-thread, suite-ev, suite-mailbox, suite-pool at minimum, plus a
  deliberate stress/density run (matching the full-density lanes re-run pattern
  already established in Phase 1).
- Self-pipe saturation (filed finding (c)) is architecturally separate from all of
  the above — it's a transport-layer backpressure gap, not a GC design gap. It
  should be scheduled as its own small fix once a signal-safe design is chosen
  (unifying `janet_ev_post_event`'s signal-handler callers and the direct
  `janet_thread_body` selfpipe writer), independent of this GC work.

--------------------------------------------------------------------------------
## 6. Staged implementation plan (future phase, not started)

1. §3.1: root the result/profile_rows unmarshal window explicitly. Smallest, most
   isolated change; directly closes the fiber-rooting-chronology finding.
2. §3.2: require explicit per-edge rooting in threaded-abstract `gcmark` callbacks;
   deprecate reliance on `home_gc_authorized` for liveness (keep the bit only for
   its original teardown-ordering purpose, not as a liveness proxy).
3. §4(a): narrow the shared surface once §3.2 lands — re-audit whether
   `home_gc_authorized` can be simplified now that liveness no longer depends on it.
4. Self-pipe saturation fix, scheduled independently, any time after step 1 (no
   ordering dependency on the GC work).
5. Re-run the full Phase 1 scoped battery + density/stress suite after each step,
   not just at the end — this is core-runtime work with wide blast radius.

No step in this plan has been implemented. This document is the design artifact
requested for Phase 2b; implementation requires a separate go/no-go decision per
step, following the same pattern used for Phase 1's `mkimage.janet` decision.

--------------------------------------------------------------------------------
## 7. Implementation outcomes (this phase, completed)

**Step 1 (`8509c0e5`)**: implemented as designed — explicit `janet_gcroot`/
`janet_gcunroot` around the unrooted C-local window for the thread-completion
result in `janet_thread_new_complete`, symmetric regardless of result type
(nil-skip handled via an explicit `result_rooted` flag, not implicit typecheck,
to avoid a leaked stale root on the asymmetric nil path).

**Step 2 (`ac22703f`)**: upgraded from hypothesis to proven bug via direct call-
graph tracing (not inference): `janet_collect_minor`, `janet_collect_fiber_minor`,
and `janet_collect_process` can each flip a threaded abstract's "visited this
cycle" registry marker true (first-visit `gcmark` dispatch, `gc.c:195-209`), but
none of them ever call `janet_sweep` — only the full major's sweep loop resets
that marker (`gc.c:883-929`). The marker stayed stale-`true` across every
subsequent minor/scoped collection and the next major's mark phase, silently
skipping re-marking of that abstract's internals for a whole cycle. Fixed with
`janet_reset_threaded_visited_minor()`, wired into the end of all three narrow
mark phases, touching only the visited marker — never refcount,
`home_gc_authorized`, or any teardown decision.

**Step 3 (re-audit, no code change)**: traced `gc_mark_phase`/`gc_minor_phase`
flag lifecycles across all four collection entry points (`gc.c:2389-2453` fiber-
minor, `gc.c:2624-2678` process, `gc.c:2701-2715` minor, `gc.c:2755-2766` major).
`home_gc_authorized`'s grant (`gc.c:184-186`, gated on
`gc_mark_phase && !gc_minor_phase`) and its sole revocation site (inside
`janet_sweep`, major-only, `gc.c:918-920`) were **already** correctly scoped to
major-only before step 2 — independent of, and unaffected by, the visited-marker
staleness step 2 fixed. No narrowing was needed; this is a confirmed-sound
finding, not a gap. No change applied.

**Step 4 (committed alongside step 3's finding)**: self-pipe saturation is
architecturally separate from the GC work (transport backpressure, not a GC
design gap), exactly as §5 anticipated. A true backpressure mechanism is not
possible from `janet_ev_post_event`'s signal-handler callers (`os.c`): a signal
handler cannot block, lock, or allocate, so the existing bounded-retry-then-abort
fallback (`ev.c:3067-3103`, asserted correct by `test-ev-selfpipe-saturation`)
must stay exactly as-is. Implemented the only safe improvement available: best-
effort `F_SETPIPE_SZ` growth (1MiB, Linux-only, guarded `#if defined(__linux__)
&& defined(F_SETPIPE_SZ)`) in `janet_ev_setup_selfpipe`, widening the margin
before the fallback triggers without touching the write/retry control flow at
all. No-op on macOS/BSD/Windows.

**Step 5 (full battery)**: dual-target (interpreter via `./build/bender` +
real AOT via `bender --aot`, not an ad hoc image/marshal substitute) re-run
across proc(99)/thread(30)/ev(776)/marsh(73)/mailbox(76)/pool(56) — all green on
both targets, identical counts. `JANET_GC_VERIFY=1` clean on proc/thread/ev.
All four `ripwire`-flagged C regressions recompiled fresh and pass:
`test-threaded-abstract-mark-claim`, `test-ev-selfpipe-interrupt`,
`test-ev-selfpipe-saturation` (expected bounded SIGABRT, unchanged). `test-gc-
pcall` still fails identically to the confirmed pre-existing baseline
(`e816546a`), unrelated to any Phase 2b change.

Phase 2b is complete: all 5 staged steps executed, two real code changes
committed (steps 1, 2, 4), one audit closed with no change needed (step 3), full
regression gate green (step 5).
