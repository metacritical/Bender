# Combined Plan: Stalin-Inspired ttfx Kernel Perf + Bender Erlang-Parity Messaging

> Historical planning record, reviewed against `5180da8a` on 2026-10-18.
> Instructions, HEAD references, measurements and open/done labels below describe
> the original session, not current execution instructions. Current compiler work:
> [ttfx-driven-compiler-work.md](ttfx-driven-compiler-work.md).
> Subsequent runtime fixes/features include `1bb7aae5`, `88c560f2`, `b8c6caa7`,
> `42d19862`, `09eefd05`, and `fc10caf1`; the generational/sweep policy was implemented.
> Claims below that reachability already performed general pruning were corrected
> by the later DCE strip plan. “Stream A not started” is not current compiler status;
> the performance target has not been established.

Two work streams, two repos, tracked together because both are leverage on the same
underlying Bender (Janet fork) compiler/runtime.

- **Stream A (ttfx/Stalin):** close the 31× rings/logo frame-time gap in ttfx via
  Stalin-style whole-program type inference + representation selection. Not started.
- **Stream B (Bender messaging):** Erlang-parity messaging/GC work in
  `/Users/pankajdoharey/Development/Projects/janet`. **Substantially advanced** by
  the other agent — re-planned below against their full handoff ledger
  (`docs/internals/erlang-parity-handoff.md`, HEAD at handoff `9961fc12`, current
  verified HEAD `a10c5664` — one further doc-correction commit, no code drift).

---

## Stream A — ttfx/Stalin kernel performance (unchanged, independent of Stream B)

### A0. Reconciliation findings
1. RESOLVED: `docs/internals/ttfx-kernel-plan.md`, `aot-plan.md`, `perf-plan.md`
   live in the Bender repo, not ttfx. `ttfx-kernel-plan.md` is untracked there.
2. OPEN: ttfx's `janet-port/src/scene.janet` is not zero-diff — real uncommitted
   optimization work present; confirm keep/commit/revert before touching further.
3. OPEN: `probe-*-tmp.janet`, `TODO-effects.md` still present in ttfx's
   `janet-port/`, contradicting "already cleaned." Do not delete/repurpose.
4. OPEN: `infer.janet` does not exist in ttfx — must be built from scratch, not
   extended.
5. OPEN: `tools/mkimage.janet`, `callgraph.janet`, `src/core/` referenced by the
   mission do not exist under ttfx's `janet-port/` — they live in the Bender repo.
6. CONFIRMED: Stalin reference sources exist with correct line numbers
   (`infer!` :1776, `c:type-set` ~:2175, `move-*`/`widen`/`compile-arithmetic` :877).
7. UNVERIFIED: baseline numbers (Rust 72/18/9%, Janet ~166fps) — reproduce first.

### A1. Reproduce baseline numbers (first execution step, stop and report if the
ground moved)
- Rust: `TTFX_FX=0 TTFX_PROFILE=1 ./target/release/ttfx --parity-dump --seed 42
  --input-file ruby-port/logo.txt rings` (cwd = ttfx repo).
- Janet: `build/bender bin/ttfx-cli.janet rings --seed 42 --bench --input-file
  ../ruby-port/logo.txt` (cwd = janet-port).

### A2. Ordered implementation (one commit per item, gates green per commit)
1. Whole-program type fixpoint: build `infer.janet` (monotone type-set widening +
   worklist, modeled on Stalin's `infer!` :1776); kill `:hint` for
   monomorphic-numeric. First customer: motion loop (`ctx-motion-move`/
   `ctx-path-step`).
2. Representation selection at emission: monomorphic-numeric ⇒ raw double/int64
   locals (Stalin's `c:type-set` squeeze/squish ~:2175, `move-*`/`widen`/
   `compile-arithmetic` :877). First: `easing.janet`'s `ease` + `color.janet`'s
   gradient path (53× worst ratio), then `scene.janet` stepping.
3. Cheap local escape analysis: frame temporaries in `ctx-frame-string` → stack
   slots.
4. Closure classification (local/global/hidden/slotted) to shrink captures in
   `ctx-update` effect callbacks.
5. Gate every commit: `make test` (53 suites), `--pgo` parity gate, 37-effect
   byte-parity vs Rust `--parity-dump`.

### A3. Non-negotiable process gates
Root-cause by inspection; dual-target verification on every GC/scheduling/
representation commit; one commit per concern with measured numbers; coordinate
before touching anything Bender-side shared with Stream B.

---

## Stream B — Bender Erlang-parity messaging (re-planned against full handoff)

### B0. Entry gate
- Confirm other agent idle/done; re-read HEAD (baseline now `a10c5664`, not the
  earlier `a7bcedcc`) and re-diff if it moved further; re-run
  `docs/internals/erlang-parity-handoff.md`'s verification discipline (§8) if so.
- Do NOT redo anything in B1 below — it is verified DONE three ways (unit
  discipline, dual-target, direct Erlang comparison). Treat as closed reference.

### B1. [CLOSED — reference only, do not redo] Messaging O(n²) fixes
Four real mechanisms fixed and dual-target verified:
1. `29c65339` env decoupling — shipped as harmless hardening, but **based on a
   wrong root-cause diagnosis**; did not fix the actual hang (corrected honestly
   in `6c6ea9c0`/`5750acd2`). Kept, not the fix.
2. `a7bcedcc` — THE dominant fix: `janet_sweep_fiber_owned` looped over the
   entire global fiber census per actor per minor (O(n²) by inspection). Routed
   `f->proc != NULL` fibers to the existing narrow `janet_sweep_proc_owned`.
3. `69e40dc9` → `a03f07bd` → `2e0e4a45` — `xfb` remembered set, 3 iterations.
   Iteration 2 (amortized rebuild) **degraded at N=100000 — do not resurrect**.
   Iteration 3 (true owner-bucketing at insert time, fixed 4096-slot hash table)
   is the committed, correct form.
4. `4d18a636` — universal narrow sweep for every fiber (fixed N=100000), after
   re-testing the historical "crashes boot image generation" warning 3/3 clean.

Measured: N=2000 2.05s→0.15s, N=5000 12.5s→0.34s, N=20000 (never finished)→4.0s,
N=100000 (never finished)→98s.

### B2. [CLOSED — reference only] Multi-thread scaling + direct Erlang comparison
- `bench/agents-multithread.janet` validated to 1,000,000 actors via 1000
  threads (72.8s, 55,157 msgs/s aggregate).
- Direct same-machine Erlang/OTP 29 comparison: Erlang beats Janet 9×–200×,
  confirmed architectural (BEAM's M:N scheduler across ~40 OS threads
  transparently), **not** a GC bug. This session's GC fixes were necessary for
  correctness at scale, not sufficient to close this gap — don't expect future
  GC work alone to close it.

### B3. [CLOSED — reference only] M4b re-scoped by measurement
Janet-level CPU-hog preemption already works and is ON by default (A/B measured:
quanta off → starved 1.278s; quanta on → preempted, light fiber done in 0.049s).
`janet_loop1_interrupt` (the missing hard C primitive) already exists and is
public. M4b is **not** a prerequisite for actor/MMO workloads. Only narrow
leftovers remain — see B8.

### B4. Finish Phase 2b (do this before Phase 3 — corrected order, see handoff §9.1
retraction of an earlier wrong recommendation)
1. **2b-iii generational/sweep policy** — storage landed (16 KiB pages, 8 size
   classes 32–512B) but the generational/sweep **policy** is still open. The
   messaging fixes in B1 all live in exactly this machinery
   (`janet_sweep_proc_owned`, `xfb` buckets, `janet_gc_mark_fiber_shallow`), so
   this is a good time to close it while the area is freshly understood.
2. **Per-proc GC independence** (the actual Phase 2b collector-swap goal, and
   the roadmap's own "NEXT after Phase 1" item). Not started.
3. **Residual O(n)-per-minor cost**: N=20000→N=100000 is 4.0s→98s (~4.5× per-
   call cost increase for 5× N — something still scales ~O(n) per minor).
   Strongest untested lead: `janet_sweep_drain_pending` (`gc.c:1702`) walks a
   **global** `sweep_pending[0..2]` list to find its tail before splicing;
   dying fibers prepend their whole heap list onto it. If deaths outpace
   drains, the tail-walk cost grows with N. A prior diagnostic-instrumentation
   attempt compiled but produced no output and was abandoned — debug that
   gating first, or measure `sweep_pending[k]` lengths via lldb directly at
   increasing N. Likely fix if confirmed: track the list's own tail pointer
   incrementally (O(1) append) instead of re-walking. Secondary candidates:
   `janet_slab_page_for`'s binary search (probably not dominant); an
   unconditional `getenv("JANET_ROOT_TRACE")` on every minor (cheap, but on the
   hot path, visible in `sample`).

### B5. Fix the `lanes=8` intermittent SIGTRAP crash (highest-value open bug —
gates Phase 3's M:N/resident-proc-migration item, since that builds on lanes)
- Repro: `JANET_NO_PROFILE=1 ./build/bender bench/agents-lanes.janet 5000 4 8`
  — crashes ~75–100% of attempts.
- ASAN-confirmed (use the working recipe below): heap-use-after-free at
  `janet_chanat_mark` (`ev.c:1325/1330`) — a 32-byte region allocated by one
  lane thread, freed by another, read by a third. `janet_chanat_mark` never
  calls `janet_chan_lock`, unlike every other access to the same queues.
- Two fix attempts already tried and REVERTED — do not repeat them blind:
  1. Lock held across the whole mark: fixed N=5000 but caused a NEW
     lock-ordering slowdown at N=20000 (39–56s user time, no crash). Reverted.
  2. Snapshot under lock, release, mark from copy: did NOT fix the crash
     (15/15 still crashed at N=5000); crash moved to the snapshot's own new
     allocation — meaning heap was already corrupted earlier. This undermines
     "missing lock" as the sole cause. Reverted.
- **Best resume lead**: every ASAN capture across both rounds shows the
  FREEING thread is always T0 (main thread), while allocating/reading threads
  vary. Investigate what T0 does that frees small objects (its own minor/major
  sweep believing it owns something exclusively, or lane-stop/teardown logic)
  rather than continuing to focus on the mark function itself.
- Working ASAN recipe (plain `-fsanitize=address` gives false positives on this
  codebase due to fiber stack switching):
  ```sh
  make clean
  JANET_NO_PROFILE=1 make CFLAGS="-O1 -g -fsanitize=address -fno-omit-frame-pointer -mllvm -asan-stack=0" LDFLAGS="-fsanitize=address" -j4
  ```
- Verification requirement: must pass at N=5000 **and** N=20000 **and** ideally
  N=50000+ — N=5000-only is demonstrably insufficient (attempt 1 passed it and
  still regressed at N=20000).
- Also fix `lanes=0` baseline-path hang (`bench/agents-lanes.janet 5000 4 0`
  hangs/times out) — not investigated at all yet, lower priority, same area.

### B6. Constant-factor spawn-cost gaps (mechanical, independent — can run in
parallel with B4/B5 by a second worker)
Identified against Erlang's ~1µs/spawn budget (external estimate, plausible but
unverified in this tree):
1. Two channels per proc (mailbox cap 16 + supervisor cap 8) allocated eagerly
   regardless of whether anyone monitors → lazy-allocate both.
2. Entry marshal-copy in `spawn-isolated` (Erlang passes `{M,F,A}`, no copy).
3. Env table + proto chain + `:proc-self` binding (partly addressed already by
   `29c65339`).
4. Per-spawn GC bookkeeping: census insert, handle rooting, owner-id assignment,
   nursery accounting → "registration-free liveness."
5. Scheduler enqueue + supervisor wiring.
Each is an independent, separately gated commit, measured against the spawn-rate
baseline.

### B7. Phase 3 (gated on B5 — do not start before the lanes crash is fixed;
B5.1/5.2/5.4 now fixed, all unblocked)
1. Named registry — register/lookup procs by name. Not started, no design doc.
2. Supervision-tree library — links/monitors primitives already exist (Phase
   2b); the tree library on top (restart strategies, child specs, restart
   intensity/period, supervisor hierarchies) does not. This is what a
   WhatsApp-style "crash, supervisor restarts it, nothing else notices" model
   depends on.

   **HARD DESIGN CONSTRAINT (user-specified, must not be lost before
   implementation starts): supervision trees are ALSO the AOT compiler's
   tree-shaking root set, not just a runtime reliability structure.**
   `tools/callgraph.janet:885`'s existing `reachable-from` whole-program
   reachability pass already does real dead-code pruning, but its `roots`
   (line 899-904) are every top-level `thunk` (every script body, necessarily
   conservative), and anything reaching a dynamic call site — which
   `proc/spawn` with a runtime-computed target is — gets marked `tainted`
   (line 916-929) and exempted from pruning entirely. Child specs in the
   supervision-tree library MUST be authored in a statically analyzable
   (literal/declarative) form — naming concrete functions/modules directly,
   not an arbitrary runtime-computed value — so the AOT build can walk the
   declared tree and feed every named child in as a precise static root (or
   as a static resolution for what would otherwise be a tainted dynamic
   `proc/spawn` site). Done right, this gives real actor-level tree-shaking:
   any actor, and everything only it calls, that isn't reachable from the
   declared supervision hierarchy gets pruned from the final image. Any
   supervision-tree design that constructs child specs dynamically (e.g. a
   runtime-built list of closures) defeats this and must be flagged as a
   real limitation, not silently accepted — same honesty discipline as every
   other phase in this project.
3. M:N scheduler core / resident-proc migration — make proc migration explicit
   in M4b; builds on the lanes machinery (hence gated on B5).

### B8. Remaining lower-priority items
- M4b leftovers (not prerequisites for actor workloads, per B3): async
  preemption of non-cooperating C/FFI code (the C primitive already exists —
  `janet_loop1_interrupt`; the work is a signal handler + policy, not new
  scheduler plumbing); running-task migration (suspended stack + VM-local
  waits semantics).
- Distribution design doc (cross-node pids) — design only, not built.
- Hot-code-loading design doc — design only, not built.
- Battle-hardening (fuzz/stress/soak campaigns) — not done, fold into every
  phase above as a standing gate.

### B9. Opportunistic, independent of all phases — pick up when convenient
1. `janet_profile_call` use-after-free (`vm.c:607`, `ASAN READ of size 8`) —
   actionable hypothesis already narrowed: `pa`/`pa->data` likely stale after a
   `realloc` inside the profiling path; needs re-fetch after growth. Found
   during single-threaded PGO profiling. Workaround while debugging other
   things: `JANET_NO_PROFILE=1`.
2. Teardown-order use-after-free (`gc.c:3131` → `gc.c:687` →
   `table.c:114/211` → `value.c:244`) during `thread/new` shutdown
   (`janet_try_init`/`janet_deinit`). Found under ASAN on `test/suite-proc.janet`.
   Not investigated.

### B10. Non-negotiable process gates (carried over, confirmed effective by the
other agent's track record — apply to every item above)
- Root-cause by code inspection or live debugger (lldb/ASAN), never guess-and-
  patch blind — their one blind-patch attempt (`29c65339`) shipped clean but
  didn't fix the bug; their two `lanes=8` attempts were educated but still both
  failed and were correctly reverted rather than kept "mostly working."
- Dual-target verification (interpreter + real AOT via `bender --aot`, which
  exercises `tools/mkimage.janet`) on every commit touching GC/scheduling.
- Full suite battery every time: proc(99) thread(30) ev(776) marsh(73)
  mailbox(76) pool(56), plus `JANET_GC_VERIFY`/`JANET_HEAP_VERIFY`/
  `JANET_PROC_VERIFY`, plus the C regression tests
  (`test-threaded-abstract-mark-claim.c`, `test-ev-selfpipe-interrupt.c`,
  `test-ev-selfpipe-saturation.c`, `test-gc-pcall.c`).
- Check every anomaly against a clean `git stash` baseline before accepting it
  as pre-existing vs. a real regression. Known pre-existing (not regressions):
  `test-gc-pcall.c` fails on clean baseline; `JANET_GC_VERIFY` + suite-ev abort;
  `JANET_PROC_VERIFY` + suite-proc "foreign fiber field" warning;
  `suite-lanes.janet` needs >30s; `suite-shared.janet` skips without
  `JANET_SHARED_STACKS=1`; `scale-proc-chain.janet 100000` doesn't finish in
  140s even on clean baseline.
- One commit per concern; measured before/after numbers in the commit message.
- Update `docs/internals/proc-lanes.md` AND
  `docs/internals/erlang-parity-handoff.md` in the same commit as any finding
  or fix — the handoff ledger is now the single source of truth and must stay
  current; do not let it drift stale the way the first revival-order draft did
  (§9.1's self-correction).

---

## Explicit non-goals for this plan right now

- No code has been written or modified in either repo this turn.
- Stream B's B1-B3 are closed and must not be redone; B4 is the actual next
  work, gated nowhere; B5 gates B7's M:N item specifically; B6 can run in
  parallel with B4/B5.
- Stream A's reconciliation (A0) is independent of Stream B entirely.

---

## Note on workspace collision (this file's path)

Saved at `docs/internals/blueant-combined-plan.md` instead of the default
`implementation_plan.md`, because this session shares a working directory with
the other agent, whose own plan file already occupies that default filename.
