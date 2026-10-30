# Erlang Parity — Complete Task Ledger & Session Handoff

**Purpose**: single-file, self-contained handoff of ALL Erlang-parity work.
Written to be revived in a fresh session with no prior context. Every claim
here is backed by a commit, a measurement, or an explicit "not verified" label.

**Repo**: `/Users/pankajdoharey/Development/Projects/bender`
**Session date**: 2026-10-16
**HEAD at handoff**: `9961fc12`
**Branch**: `master` (277+ commits ahead of `origin/master`)

---

## 0. TL;DR for whoever picks this up

- **Messaging/spawn scaling: FIXED.** Four real O(n²) mechanisms found and
  fixed. `bench/agents.ben` went from hanging at N=20000 to completing
  through N=1,000,000. All dual-target verified.
- **Erlang gap quantified honestly**: Erlang still beats us 9x–200x. Root
  cause is architectural (M:N scheduler), **not** GC — proven by same-machine
  measurement, see §3.
- **One real bug UNSOLVED and PARKED**: intermittent `lanes=8` crash (§5.1).
  Root-caused via ASAN, two fix attempts failed verification, both reverted.
- **M4b "true preemption" was re-scoped by measurement** (§4.2): Bender-level
  preemption already works and is ON by default. Only narrow C-code-preemption
  and task-migration remain. **Not** a prerequisite for actor workloads.
- **Recommended next item**: ⚠️ **finish Phase 2b first — it is NOT complete.**
  2b-iii's generational/sweep policy is still open, and "per-proc GC
  independence" (the collector-swap goal, and the roadmap's own "NEXT after
  Phase 1" item) has not been started. **Do not jump to Phase 3** — an
  earlier draft of this file wrongly recommended that; see §9.1 for the
  retraction and §9.3 for the corrected, dependency-respecting order.
  *(Update 2026-10-17: 2b-iii policy confirmed already landed — §9.2; the
  §5.5 residual O(n)-per-minor cost root-caused and fixed — see §5.5; the
  per-proc independence design note written — §9.2. Remaining Phase 2b
  follow-on: implement the design note.)*

---

## 1. FINISHED — Messaging throughput: four O(n²) mechanisms fixed

All four were verified with the full discipline described in §7.

### 1.1 `29c65339` — plain `proc/spawn` env decoupling
- **Status**: SHIPPED, verified. **But see the CORRECTION below.**
- **File**: `src/core/proc.c` (+59/-20)
- **What**: plain `proc/spawn`'s child env no longer inherits the spawner's
  full proto chain. Child gets only `:proc-self` (plus `:out`/`:err` when
  set); opt-in `(setdyn :proc-inherit-env true)` in the spawner restores the
  old behavior. Relevant symbols: `:proc-self`, `:proc-inherit-env`
  (`proc.c` ~901–971).
- **CORRECTION (`6c6ea9c0`, `5750acd2`)**: this was shipped believing it
  fixed the O(n²) hang. **It did not** — re-benchmarking showed N=20000 still
  hung identically. The env-proto-chain diagnosis was wrong. The change is
  still valid, harmless hardening (dual-target verified, zero regressions)
  and is kept, but it is **not** the fix for the scaling wall. Documented
  honestly rather than left as a false claim.

### 1.2 `a7bcedcc` — THE dominant fix: narrow sweep routing for proc fibers
- **Status**: SHIPPED, verified. This was the big one.
- **Files**: `src/core/gc.c` (+64), `src/core/gc.h` (+14),
  `src/core/ev.c` (+46), `src/core/util.h` (+5), `docs/internals/proc-lanes.md`
- **Root cause**: `bender_sweep_fiber_owned` (`gc.c` ~1740) called
  `bender_sweep_kind(..., walk_fibers=1)`, which loops over **every fiber in
  the global census** on every call. `bender_collect_fiber_minor` calls it once
  per actor per minor → a loop over N called N times → **O(n²) by direct code
  inspection**, no profiling ambiguity.
- **Historical context (found via `git log`)**: commit `a739009e` (P3.5b)
  explicitly predicted this ("spawn still superlinear… P3.5c is the
  linearizer. No scale win claimed"). `a5d9168e` (P3.5c) then shipped the
  linearizer **only for strict/isolated spawn**, via `vm.c`'s routing check
  `heap_strict && proc`. Plain `proc/spawn` fibers have `proc != NULL` but
  `heap_strict == 0`, so they fell into the wide path purely from that
  conjunction. A known, acknowledged, never-closed gap — not a new bug.
- **Fix**: route any `f->proc != NULL` fiber (strict or plain) to the
  already-existing, already-proven `bender_sweep_proc_owned` (narrow, O(own)).
- **Also in this commit**: `bender_gc_mark_fiber_shallow` (`gc.c:576`, declared
  `gc.h:148`) and `bender_ev_mark_shallow` (`ev.c:777`, declared `util.h:297`).
  A scoped fiber-minor only needs to protect a queued fiber's own block from
  being swept, not deep-walk its entire stack. `bender_mark_roots` (global
  minor/major) unchanged — still fully deep.

### 1.3 `69e40dc9` → `a03f07bd` → `2e0e4a45` — the `xfb` remembered set (3 iterations)
- **Status**: SHIPPED, verified. Took three attempts to land correctly.
- **File**: `src/core/gc.c`
- **Root cause**: `ss->xfb` is **one global, VM-wide array** of cross-fiber
  write-barrier edges, reset only at a full major (which this workload never
  triggers). Both scoped collectors scanned it **in full** on every call —
  O(minors) × O(xfb_count), both O(n). A pre-existing comment already
  recorded measuring "185k live xfb entries dominated every minor" at 100k
  agents.
- **Iteration 1 (`69e40dc9`)**: lazily-rebuilt sorted-by-owner index, rebuild
  on any growth. Helped small N.
- **Iteration 2 (`a03f07bd`)**: amortized-doubling rebuild threshold.
  **DO NOT RESURRECT THIS APPROACH** — profiled degrading at N=100000 because
  this workload interleaves a scan with nearly every insert, defeating the
  amortization; the unsorted-tail scan cost stayed quadratic.
- **Iteration 3 (`2e0e4a45`) — the committed form**: **true owner-bucketing at
  insert time.** Fixed 4096-slot chained hash table keyed by owner, populated
  at the same single insert site (owner already computed there).
  - Symbols: `BENDER_XFB_NBUCKETS` (`gc.c:1013`), `struct BenderXfbBucketNode`
    (`gc.c:1014`), `ss->xfb_buckets` (`gc.c:1107`),
    `bender_xfb_bucket_append` (`gc.c:1124`), `bender_xfb_buckets_free`
    (`gc.c:1126`), `bender_xfb_mark_owned`.
  - O(1) amortized insert, O(1) hash lookup + O(own entries) scan, no sort.
  - Verified via lldb: chain length ~25 (4096 slots / ~100k owners), per-owner
    entries ~2 — performing exactly as designed.
  - Safety argument: `page->owner` has exactly ONE write site (fresh page
    allocation) and is never reassigned; the only transition is
    `owner=K → mixed/0`, and a mixed-page object is already immune to any
    single-owner scoped sweep (traced `bender_sweep_list`'s own filter). So a
    stale cached owner can never cause a premature free.
  - The main `ss->xfb` array, its insertion order, and
    `bender_slab_owner_stats`' rollback truncation are **untouched** — buckets
    are a parallel additive dual-write, freed (not rolled back) at
    major-reset and VM clear.

### 1.4 `4d18a636` — universal narrow sweep (fixed N=100000)
- **Status**: SHIPPED, verified.
- **File**: `src/core/gc.c` (+38)
- **Root cause**: a `dofile`/run-context wrapper fiber executing the top-level
  send loop is **not a proc** (`f->proc == NULL`) and not `root_fiber`, so
  `a7bcedcc`'s proc-scoped fix didn't cover it. It still took the wide census
  path on every one of its own (frequent) minors.
- **Critical prerequisite checked, not assumed**: the P3.5b-era warning
  ("full narrowing crashes boot image generation deterministically") was
  **re-tested against the exact historical scenario** — `build/bender_boot`'s
  actual bootstrap image generation, 3/3 runs, producing **byte-identical**
  `bender.c` each time. Plus all 60 `test/suite-*.ben` files individually,
  and `suite-lanes`' 640k-density join (8.02s, matching historical baseline).
  Zero behavioral difference. Whatever caused the historical crash was fixed
  by other work in the interim.
- **Fix**: `bender_sweep_proc_owned` is now the universal default for **every**
  fiber. The wide path is kept as dead code behind `#if 0` in
  `bender_sweep_fiber_owned` (`gc.c` ~1740) **for bisection** if the historical
  failure mode ever resurfaces.

### 1.5 Measured outcome (`bench/agents.ben`, interpreter, `BENDER_NO_PROFILE=1`)

| N | before | after |
|---|---|---|
| 2,000 | 2.05s (3,901 msgs/s) | 0.15s (424,111 msgs/s) |
| 5,000 | 12.5s (3,187 msgs/s) | 0.34s (103,477 msgs/s) |
| 20,000 | **did not finish in 600s** | 4.0s (22,575 msgs/s) |
| 100,000 | did not finish | 98s (4,445 msgs/s) |
| 1,000,000 | did not finish | see §2 (via multi-thread topology) |

---

## 2. FINISHED — Multi-thread scaling validation

### 2.1 `79a0135c` — new benchmark: `bench/agents-multithread.ben`
- **Status**: SHIPPED, verified (interpreter + AOT).
- **File**: `bench/agents-multithread.ben` (new, 86 lines)
- **What**: spawns N independent `thread/new` workers (share-nothing isolated
  VMs, true multi-core), each running its own self-contained copy of the
  `bench/agents.ben` actor-burst kernel, then `thread/join` aggregates.
  Distinct from `bench/agents-lanes.ben`, which splits ONE workload across
  lane workers via `proc/spawn` routing.
- **Usage**: `bender bench/agents-multithread.ben [N] [T] [THREADS]`
  (N = actors **per thread**, default 100/4/20)

### 2.2 Escalation series (`72fb07a4`, `4e0662f7`, `8fdb873d`) — all PASS

| threads | procs/thread | total actors | wall | aggregate msgs/s | sys/user |
|---|---|---|---|---|---|
| 20 | 100 | 2,000 | 0.06s | 138,884 | ~1.5x |
| 50 | 100 | 5,000 | 0.10s | 209,237 | ~3x |
| 500 | 100 | 50,000 | 0.96s | 221,056 | ~5.6x |
| 500 | 1000 | 500,000 | 33.4s | 60,098 | ~12.6x |
| 1000 | 1000 | **1,000,000** | 72.8s | 55,157 | ~19.7x |

- All PASS — correct exact-once message accounting at every scale.
- Throughput stays flat 50→500 threads at fixed per-thread load, then drops
  when per-thread N grows 10x — i.e. **each thread's own VM still carries the
  per-VM scaling characteristics**; threads parallelize across VMs but don't
  fix what happens inside one.
- Rising sys/user ratio (1.5x→19.7x) = real OS thread/VM-creation contention
  as a separate, growing cost layer.
- Also verified: `bench/agents.ben` at N=50/100 and
  `test/scale-proc-chain.ben` at N=50/100 all PASS; `bench/concurrency.ben`
  all 9 micro-benches complete.

---

## 3. FINISHED — Direct same-machine Erlang comparison (`ac9b75ab`)

- **Files**: `bench/agents_erl.erl` (new, 72 lines), `docs/internals/proc-lanes.md`
- **Build/run**: `cd bench && erlc agents_erl.erl && erl -noshell -run agents_erl main N T -s init stop`
- **Erlang/OTP 29** (`/opt/homebrew/bin/erl`), same machine, same actor kernel shape.

| N | Bender (single VM) | Erlang (one node) | Erlang advantage |
|---|---|---|---|
| 2,000 | 14,573 msgs/s | 1,000,000 msgs/s | ~69x |
| 5,000 | 103,477 msgs/s | 952,380 msgs/s | ~9.2x |
| 20,000 | 22,575 msgs/s | 963,855 msgs/s | ~43x |
| 100,000 | 4,445 msgs/s (98s) | 896,860 msgs/s (0.79s) | **~200x** |
| 1,000,000 | needed 1000 manual threads, 72.8s | 12.4s, 377,287 msgs/s | — |

- **Root cause of the gap, confirmed by direct process inspection**: `erl`'s
  default invocation on this 12-core machine runs **40 OS threads** (verified
  `top -pid <beam.smp> -stats th`), matching `erlang:system_info/1`:
  `schedulers=12` + `dirty_cpu_schedulers=12` + `dirty_io_schedulers=10` +
  `thread_pool_size=1` (=35) + ~5 housekeeping. That's BEAM's **M:N
  scheduler** — millions of processes transparently mapped onto a small fixed
  preemptive thread pool, **zero application orchestration**.
- **Conclusion**: the gap is architectural (M:N scheduler + true preemption +
  30 years of per-process GC tuning), **not** GC bugs. This session's GC fixes
  were necessary to make the current cooperative one-VM-per-thread model
  functionally correct at scale; they were never going to close a 200x gap.

---

## 4. FINISHED — M4b investigation (re-scoped, not implemented)

### 4.1 `0d90e346` — re-verified already-shipped preemption machinery
- `test/suite-interrupt.ben` 15/15 PASS, `test/suite-quantum.ben` 16/16 PASS
  on current HEAD. The shipped `ev/interrupt` + `ev/timeslice` mechanisms
  (M4b-1, M4b-2 per `aot-plan.md`) are solid.

### 4.2 `9961fc12` — **M4b re-scoped by measurement** (important)
- **Finding 1**: the "missing" hard C primitive **already exists and is
  already public**. `bender_loop1_interrupt(BenderVM *vm)` (`ev.c` ~2461,
  declared `src/include/bender.h` ~1621) already cross-thread-interrupts a
  target VM by handle (`bender_interpreter_interrupt`, atomic inc on
  `vm->auto_suspend`, `state.c:63`) **and** breaks it out of a blocking
  event-loop wait via self-pipe post. `bender.h` explicitly documents it as
  callable from "library code, **a signal handler, or another thread**", with
  a documented usage pattern. The timeout path already uses it in production
  (`ev.c:966`).
- **Finding 2 (A/B measured, not assumed)**: Bender-level CPU-hog preemption
  **already works and is ON by default**. Probe: one 80M-iteration zero-yield
  CPU-bound fiber racing one light fiber (5× `ev/sleep 0.01`):

  | config | light-done | hog-done | verdict |
  |---|---|---|---|
  | `BENDER_TIMESLICE=0` (quanta OFF) | 1.278s | 1.224s | **STARVED** |
  | default (quanta ON) | **0.049s** | 1.276s | **PREEMPTED** |

  The off-case reproducing starvation exactly is what makes this conclusive.
- **What actually remains in M4b** (per `ev/timeslice`'s docstring, quanta
  "never land inside C calls or pcall regions — poll points are Bender ops
  only"):
  1. Async preemption of **non-cooperating C code** (FFI/blocking C calls).
     `bender_loop1_interrupt` is already the signal-safe primitive — the work
     is a signal handler + policy, **not** new core scheduler plumbing.
  2. **Running-task migration** (suspended stack + VM-local waits: channels,
     timeouts, supervisors — the semantics surface is the work).
- **Recommendation**: M4b is **NOT** a prerequisite for MMORPG/actor
  workloads. Quanta already prevent the motivating starvation scenario.

---

## 5. ⚠️ UNFINISHED / UNSOLVED — must be revived

### 5.1 ⚠️ **UNSOLVED: intermittent `lanes=8` SIGTRAP crash** (highest-value open bug)

**Status**: root-caused, two fix attempts FAILED verification, both REVERTED.
`src/core/ev.c` is confirmed back to its unmodified committed state.

- **Repro (fast)**: `BENDER_NO_PROFILE=1 ./build/bender bench/agents-lanes.ben 5000 4 8`
  — crashes ~75–100% of attempts, ~0.06–0.1s per non-crashing run. (N=20000
  crashes at the same rate but is much slower; use N=5000.)
- **Crash signature**: always `bender_chan_pack` → `bender_malloc` (`ev.c:1001`),
  reached via `cfun_proc_send` → `bender_channel_push` →
  `bender_channel_push_with_lock`. macOS malloc freelist-corruption detector
  (`EXC_BREAKPOINT` in `_xzm_xzone_malloc_freelist_outlined`), i.e. the
  classic signature of an **earlier** heap overrun discovered later.
- **ASAN-confirmed real bug** (2/2 identical reproductions):
  heap-use-after-free at `bender_chanat_mark` (`ev.c:1325/1330`, the channel
  abstract's `gcmark` callback) → `bender_mark` → `bender_mark_tuple`/
  `bender_mark_array`. A 32-byte region **allocated by one lane thread, freed
  by another, read by a third**.
- **Confirmed defect by code reading**: `bender_chanat_mark` **never calls
  `bender_chan_lock`**, while every other access to the same
  `read_pending`/`write_pending`/`items` queues does.
- **Fix attempt 1 — lock held across the whole mark**: 20/20 clean at N=5000,
  **but** N=20000 showed a NEW failure mode absent from baseline — severe
  workload-dependent slowdown (39–56s user time, not crashes), consistent
  with a lock-ordering risk (marking while locked can recurse into marking
  other threaded abstracts needing other locks). REVERTED.
- **Fix attempt 2 — snapshot under lock, release, then mark from the copy**:
  did **not** fix the crash (15/15 crashed at N=5000), and the crash moved to
  the snapshot code's **own new allocation**, meaning the heap was already
  corrupted before `bender_chanat_mark` ran in that instance. This undermines
  confidence that the missing lock is the sole (or real) cause. REVERTED.
- **Baseline re-confirmed**: unmodified code crashes 8/8 at N=20000, always
  within 0–1s, **never hangs** — so attempt 1's hang was genuinely new.
- 🔑 **BEST RESUME LEAD**: **every single ASAN capture across both
  investigation rounds shows the FREEING thread is always T0 (the main
  thread)**, while allocating and reading threads vary. Investigate what runs
  on T0 that frees small objects — its own minor/major collection sweeping
  something it believes it owns exclusively, or lane-stop/teardown logic —
  rather than continuing to focus on the mark function.
- **Verification requirement for any future fix**: must be verified at
  **N=5000 AND N=20000 AND ideally N=50000+**. N=5000-only verification is
  demonstrably insufficient (attempt 1 passed it and still regressed).
- **Full writeup**: `docs/internals/aot-plan.md`, commits `0d90e346`,
  `d5a1a421`, `14b426fb`, `f62eabf3`, `f5fd36a6`, `9ac90672`, `925fd117`.

### 5.2 ⚠️ UNSOLVED: `lanes=0` baseline-path hang
- `bench/agents-lanes.ben 5000 4 0` hangs/times out (exit 124), while
  `lanes=4`/`lanes=8` work. Pre-existing bug in that script's single-VM
  baseline path. **Not investigated at all.** Lower priority.

### 5.3 ⚠️ FILED NOT FIXED: `bender_profile_call` use-after-free
- ASAN: `READ of size 8` at `vm.c:607`, inside `bender_profile_call`
  (function starts `vm.c:576`), during ordinary **single-threaded** PGO
  auto-profiling — triggered just by running `tools/patch-header.ben`
  during a normal build with profiling left on.
- Freed region was 1072 bytes, freed by `realloc` on the SAME thread (T0)
  that later reads it. The loop at `vm.c:605-610` reads `argv[i]` (:607) and
  `pa->data[i]` (:608) — so the actionable hypothesis is that **`pa`
  (the profile param array obtained before the loop) is a stale pointer
  after something inside the profiling path `realloc`'d its backing store**,
  i.e. `pa`/`pa->data` needs re-fetching after any growth, or the growth
  needs to happen before `pa` is captured. Not confirmed — but the fix
  location is narrow and the read/write sites are only 5 lines apart.
- Found incidentally under ASAN. Unrelated to lanes. Not investigated further.
- **Workaround when debugging other things**: `BENDER_NO_PROFILE=1`.

### 5.4 ⚠️ FILED NOT FIXED: teardown-order use-after-free
- `bender_clear_memory` (`gc.c:3131`) → `bender_deinit_block` (`gc.c:687`) →
  `bender_table_put`/`bender_table_remove` (`table.c:114/211`) → `bender_equals`
  (`value.c:244`) reads freed memory, after
  `bender_abstract_decref_maybe_free` freed it earlier in the SAME
  `bender_clear_memory` call (`gc.c:3093`). During
  `bender_thread_new_run`'s `bender_try_init`/`bender_deinit` (thread shutdown).
- Found under ASAN on `test/suite-proc.ben`. Not investigated.

### 5.5 ✅ RESOLVED (2026-10-17 session): residual O(n)-per-minor cost
- N=20000 → N=100000 was 4.0s → 98s; per-minor cost ~0.2ms → ~0.9ms (a ~4.5x
  per-call increase for 5x N) → something still scaled ~O(n) per minor call.
  Minor count confirmed ≈ N (one per actor), nurseries correctly tiny.
- **Root cause FOUND and FIXED — it was NOT `bender_sweep_drain_pending`.**
  The handoff's sweep_pending tail-walk hypothesis is DISPROVED: the drain
  runs after every minor and empties the list, so its cost is amortized
  O(deaths since last drain), which stays tiny. (Found by code inspection
  before profiling; confirmed by not appearing in the profile at all.)
- **Actual root cause** (macOS `sample`, clean non-ASAN rebuild, N=100000
  mid-run: **97% of all samples** in `bender_collect_fiber_minor` at the
  `bender_reset_threaded_visited_minor()` call, gc.c:2582 in the pre-fix
  tree): that function walks the ENTIRE `bender_vm.threaded_abstracts`
  registry table — a capacity-sized loop over every channel/abstract ever
  registered (~2 per proc, so ≈O(N), and capacity never shrinks) — after
  EVERY fiber minor. Minors ≈ N → **O(N²) total**, exactly the measured
  ~O(n)-per-minor shape.
- **Fix**: replaced the registry's boolean "visited this cycle" marker with
  a per-collection **cycle stamp** (`BenderVM.gc_visit_cycle`, uint32,
  bumped inside `bender_gc_epoch_next` which already runs exactly once at
  the start of every collection). "Visited this collection" =
  `value == cycle` (helper `bender_threaded_visited`, gc.c). Deleted the
  whole-registry reset pass and its three call sites; updated the mark-site
  flip (gc.c ~200) and `bender_sweep`'s unvisited test (gc.c ~956).
  Writers of `false` (abstract.c registration/re-arm, marsh.c transfer,
  bender_sweep's post-major reset, tombstones) remain valid — false and
  stale stamps both read as "not visited"; the nil→incref single-claim
  semantics are preserved exactly.
- **Numbers** (`bench/agents.ben`, interpreter, `BENDER_NO_PROFILE=1`,
  clean rebuild, same machine): N=20000 ticks 4.49s → **1.43s** (3.1x);
  N=100000 ticks 113.4s → **26.5s** (4.3x). 20k→100k is now 18.5x wall
  for 5x N — better but still superlinear; a further residual term
  remains (candidates: allocation-path page-directory growth, spawn-phase
  costs; see §5.6), but the dominant per-minor O(n) term is gone.
- **Verification**: full battery per §8 — proc/thread/ev/marsh/mailbox/
  pool/gc-minor suites (proc 98/99 pre-existing flake "both land",
  reproduced 3/3 on stashed clean baseline; AOT proc is 99/99), AOT
  dual-target green, `BENDER_GC_VERIFY`/`BENDER_HEAP_VERIFY`/`BENDER_PROC_VERIFY`
  runs green, gcsetinterval 65536 stress on suite-boot/ev green, C
  regressions: threaded-abstract-mark-claim PASS, ev-selfpipe x2 PASS,
  gc-pcall fail = known pre-existing.
- Secondary candidates from the old §5.5 text (`bender_slab_page_for`
  binary search; per-minor `getenv` calls) are real but were noise-level
  in this profile; the reset pass was ~97% of the story.

### 5.6 ✅ INVESTIGATED (B6, uncommitted diff): constant-factor spawn-cost gaps
Identified against Erlang's ~1µs/spawn budget (external analysis, plausible
but unverified in this tree):
1. Two channels per proc (mailbox cap 16 + supervisor cap 8) allocated
   eagerly whether or not anyone monitors → lazy-allocate both.
   **B6 finding (code-verified + implemented, uncommitted)**: the mbox is
   now LAZY (NULL at birth, `bender_proc_ensure_mbox` materializes it on
   first send/try-send/recv/pid; capacity kept in a new
   `BenderProcess.mbox_cap`). Safe because every cross-file consumer already
   null-guards (ev.c retire at ~2371, gc.c mark at ~2797, proc.c kill) and
   the core/proc abstract never leaves its home VM (marshal refuses), so
   ensure-on-first-use has no cross-thread race; pid tuples carry the
   materialized channel pointer. The sup channel is **NOT lazy and cannot
   be without touching ev.c** (off-limits): ev.c ~2382/2401 pushes the
   completion event only when `fiber->supervisor_channel` is non-NULL, and
   suite-proc test 11 ("late monitors deliver from the retained completion
   event") depends on the event being buffered in the eager sup channel
   when nobody monitored during the proc's life. Lazy sup would silently
   drop late-registered reapers' events. Measured (isolated clean builds,
   alternating runs, quiet machine): spawn-only path (procs that never
   touch their mailbox) 3.22 → 3.03 µs/spawn median (~6%, ≈ the threaded
   channel's malloc+mutex+cond cost); `bench/agents.ben` spawn-rate
   unchanged (~230k/s both — every actor recv's immediately, so the alloc
   just moves to first recv; net zero there, real win only for
   never-messaged procs). Full battery dual-target identical (98/99 proc —
   test 14 "both land" fails on clean HEAD too, pre-existing — plus
   thread/ev/marsh/mailbox/pool all green, interpreter and AOT).
   ⚠️ Measurement hazard recorded: benchmarking in the shared tree while
   another agent rebuilds produces 5–40x phantom slowdowns from their
   ASAN-instrumented binary; A/B numbers must come from isolated copies.
2. Entry marshal-copy in `spawn-isolated` (Erlang passes `{M,F,A}`, no copy).
   **B6 finding: the copy IS the P3.2 isolation semantic** — captured
   values arrive by value; the P3.3 export-rule tests depend on it. Erlang
   can skip the copy because module code is globally shared and immutable;
   a Bender closure's funcdef/funcenv are parent-heap GC objects, so passing
   the entry by reference would hang the strict child's heap attribution
   off the parent's heap (exactly what per-proc sweeps and the strict bit
   exist to prevent). A capture-free-closure carve-out is conceivable but
   blocked by funcdef heap attribution. No change; design note only.
3. Env table + proto chain + `:proc-self` binding (partly addressed by
   `29c65339`). **B6 finding: already minimal.** 29c65339 narrowed the
   env to out/err copy-forward (+ opt-in full chain); what remains per
   spawn is one empty table + ≤3 puts, all load-bearing (`:out`/`:err` for
   printing, `:proc-self` for `proc/self`; `proc/recv` already uses the
   C back-pointer fast path). Nothing further safely gained.
4. Per-spawn GC bookkeeping: census insert, handle rooting, owner-id
   assignment, nursery accounting → "registration-free liveness".
   **B6 finding: all of these live in gc.c** (census insert at fiber
   birth, slab owner filing in `bender_gcalloc`/`bender_slab_alloc`) — not
   editable under B6's constraints. Read-only analysis: the census insert
   IS the fiber-liveness mechanism ("registration-free liveness" already
   exists in the sense that the census replaces explicit rooting for
   fibers); the `bender_gcroot(proc_j)`/`gcunroot` pair in proc.c covers
   the handle across the attributed-construction try block and cannot be
   dropped without reordering allocation so the abstract is born last —
   a gc.c-coupled refactor for ~two hash ops. Design note only.
5. Scheduler enqueue + supervisor wiring. **B6 finding: already minimal.**
   `bender_schedule` = flag check + one `active_tasks` table put (first
   schedule only) + one ring-buffer queue push (ev.c ~695); supervisor
   wiring is a single pointer store. Nothing avoidable or sensibly
   deferrable.

### 5.7 ⚠️ NOT STARTED: remaining M4b items
See §4.2 — async preemption of non-cooperating C code; running-task migration.
Both low priority, neither on the critical path.

### 5.8 ⚠️ NOT STARTED: larger epics
- **Per-proc GC independence** (full collector swap) ← **this is PHASE 2b
  work and is the correct next item** (roadmap's own "NEXT after Phase 1";
  see §9.2/§9.3). Phase 2b's 2b-iii generational/sweep **policy** is also
  still open on top of this.
- **Named registry + supervision trees (Phase 3)**. Genuinely missing; what
  a WhatsApp-style reliability model depends on ("a process crashes, its
  supervisor restarts it, nothing else notices"). Primitives (links/monitors)
  exist; no supervision-tree library, no named registry. ⚠️ **Do NOT start
  before Phase 2b is finished** — see §9.1's retraction. Full Phase 3 scope
  in §9.4.
- **Distribution design doc** (cross-node pids) — not built.
- **Hot code loading design doc** — not built.
- **Battle-hardening** (fuzz/stress/soak campaigns) — not done.

---

## 6. Documents index

| Document | Contains |
|---|---|
| `docs/internals/erlang-parity-handoff.md` | **this file** — complete task ledger |
| `docs/internals/proc-lanes.md` | messaging O(n²) investigation; all four fixes; corrections; multi-thread escalation tables; Erlang comparison; "HANDOFF: optimization tricks tried" section (what was tried / what did NOT help / untested candidates) |
| `docs/internals/aot-plan.md` | M4b epic scoping + re-scoping by measurement; the entire `lanes=8` crash investigation (6 rounds); **working ASAN recipe** (§7); both failed fix attempts; T0 resume lead |
| `implementation_plan.md` | standing plan — currently records lanes=8 as PARKED |

---

## 7. 🔧 Tooling recipes (hard-won — reuse these)

### Working ASAN recipe (CRITICAL — plain ASAN does NOT work on this codebase)
Plain `-fsanitize=address` produces **false-positive `stack-buffer-overflow`**
reports on ordinary, correct, single-threaded code (confirmed on
`test/suite-proc.ben`) — Bender's user-space fiber stack switching confuses
ASan's shadow-stack tracking. Adding `-mllvm -asan-stack=0` disables ONLY the
stack instrumentation, keeps heap/global instrumentation fully active, and
eliminates the false positive while still catching real heap bugs:

```sh
make clean
BENDER_NO_PROFILE=1 make \
  CFLAGS="-O1 -g -fsanitize=address -fno-omit-frame-pointer -mllvm -asan-stack=0" \
  LDFLAGS="-fsanitize=address" -j4
BENDER_NO_PROFILE=1 ./build/bender <script>
```
(`BENDER_NO_PROFILE=1` avoids conflating with §5.3's profiling UAF. `-O1`, not
`-O0`: readable traces without the slowdown.)

**This single discovery unblocks ASAN-based debugging for this codebase
generally.** Remember to `make clean && BENDER_NO_PROFILE=1 make -j4` afterward
to restore a normal build.

### Other tools used
- **Guard Malloc**: `DYLD_INSERT_LIBRARIES=/usr/lib/libgmalloc.dylib` — works
  without the fiber/ASan conflict, but far too slow per-allocation to reach
  the N that triggers the lanes bug.
- **lldb repeated-run capture** for intermittent crashes:
  ```sh
  cat > /tmp/l.txt <<'EOF'
  run bench/agents-lanes.ben 5000 4 8
  bt all
  quit
  EOF
  for i in 1 2 3; do timeout 15 lldb -b -s /tmp/l.txt ./build/bender 2>&1 | grep -A8 "stop reason"; done
  ```
- **Built-in instrumentation**: `BENDER_ROOT_TRACE=1` (prints `[CFM2]` per
  fiber-minor: count, fiber, owner, nursery, xfb size), `BENDER_GC_VERIFY=1`,
  `BENDER_HEAP_VERIFY=1`, `BENDER_PROC_VERIFY=1`, `BENDER_GC_COUNT` (compile-time).
- **`sample`** (macOS) for profiles — but note its inlined call graphs are
  ambiguous at `-O2`; lldb `bt` is authoritative. Several wrong turns this
  session came from over-reading `sample` output.

---

## 8. Verification discipline used (apply to any future fix here)

Every shipped commit in §1–§3 passed ALL of:
1. **Interpreter suites**: `proc`(99) `thread`(30) `ev`(776) `marsh`(73)
   `mailbox`(76) `pool`(56) — all green.
2. **AOT dual-target**: `./build/bender --aot test/suite-X.ben -o build/aot/suite-X-aot`
   then run the binary. Identical counts both targets. This also exercises
   `tools/mkimage.ben`'s image-generation path.
3. **Instrumented runs**: `BENDER_GC_VERIFY`, `BENDER_HEAP_VERIFY`,
   `BENDER_PROC_VERIFY`.
4. **Baseline comparison for every anomaly**: any flagged failure was checked
   against a clean `git stash` baseline before being accepted as pre-existing.
   Several were (and are genuinely pre-existing — see below).
5. **C regressions**: `test/c/test-threaded-abstract-mark-claim.c`,
   `test-ev-selfpipe-interrupt.c`, `test-ev-selfpipe-saturation.c`,
   `test-gc-pcall.c` — compiled fresh against `build/bender.o` and run.
6. **Re-benchmarked** `bench/agents.ben` at multiple N after each fix.

### Known pre-existing failures (NOT regressions — confirmed on clean baseline)
- `test/c/test-gc-pcall.c` — fails on clean baseline too.
- `BENDER_GC_VERIFY=1` + `test/suite-ev.ben` — "young unmarked … minor mark
  incomplete" aborts; byte-identical on clean pre-session baseline. Matches
  commit `a739009e`'s own note ("GC_VERIFY-over-boot abort is pre-existing").
- `BENDER_PROC_VERIFY=1` + `test/suite-proc.ben` — "foreign fiber field holds
  swept candidate"; reproduces identically on baseline.
- `test/suite-lanes.ben` needs >30s (passes fully at 120s: "640k-park stop
  in 8.02s").
- `test/suite-shared.ben` SKIPs without `BENDER_SHARED_STACKS=1`.
- `test/scale-proc-chain.ben 100000` does not finish in 140s — also on the
  clean pre-session baseline (historical "19-24s" claim is stale).
- Transient `"failed to write event to self-pipe"` aborts under heavy parallel
  system load — non-deterministic, not a regression.

---

## 9. Phase structure, and the CORRECTED revival order

### 9.1 ⚠️ CORRECTION to an earlier version of this file
An earlier draft of §9 recommended starting with **Phase 3** (named registry
+ supervision trees). **That was wrong and is retracted** — it skipped
unfinished **Phase 2b** work. Phase 2b is *not* complete, and the roadmap's
own gap table assigns "per-proc GC independence" to it explicitly:

> `Per-proc GC independence | Phase 2b collector swap | NEXT after Phase 1`
> — `docs/internals/proc-lanes.md` ~line 880

Do not jump phases. The corrected order is §9.3.

### 9.2 Where each phase actually stands (verified against the docs, not recalled)

| Phase | Scope | Status |
|---|---|---|
| **Phase 1** | Revival + completion on muse-spark-1.3 | **DONE** |
| **Phase 2b — collector swap** (`aot-plan.md` §"Phase 2b", line 184: *"NEXT; not started"* header is itself now stale — slices landed individually) | three ordered slices, see below | ⚠️ **INCOMPLETE** |
| ↳ 2b-i | scoped roots for generated/C code (`bender_gcshadow_push/mark/pop_to`) | **DONE** |
| ↳ 2b-ii | immutable-string heap split (`BenderVM.string_blocks`) | **DONE** |
| ↳ 2b-iii | slab/size-class allocator + generations | **DONE, including policy** — storage (16 KiB pages, 8 size classes 32–512B) AND the generational/sweep policy both landed: per-page age (young 0–1, old 2+), dirty flags, write barriers in table/array store primitives, minors skipping old-clean containers, `test/suite-gc-minor.ben` torture gate, `BENDER_GC_VERIFY` minor-edge verifier. The earlier "policy open" row here was doc drift vs aot-plan.md's "Policy done (this checkout)" — corrected 2026-10-17 after code inspection + gate runs (gc-minor 12/12, gcsetinterval 65536 stress on boot/ev, GC_VERIFY green) |
| ↳ "GC hardening" steps 1–5 | separate hardening pass (not the collector swap) | **DONE** |
| ↳ **per-proc GC independence** | the full collector swap goal | ⚠️ **DESIGN NOTE WRITTEN** — `docs/internals/per-proc-gc-independence.md` (scoped 5-slice plan reusing owner attribution, per-fiber heap_lists, xfb buckets, narrow sweeps; first milestone = per-owner slab arenas). Not implemented |
| **Phase 3** | named registry; supervision-tree library; M:N scheduler core / resident-proc migration | ⚠️ **NOT STARTED** (see §9.4) |
| Phase 3.5 / 5 candidates | copy-by-default messages; messaging-throughput campaign; proc migration | copy-by-default **DONE**; throughput campaign **DONE this session** (§1); migration open |
| M4b (milestone, not a numbered phase) | true preemption | re-scoped by measurement, §4.2 — mostly already shipped |

### 9.3 CORRECTED revival order (respects phase dependencies)

1. **Finish Phase 2b first.**
   a. **2b-iii policy work** — the generational/sweep policy left open after
      storage landed. Note this session's four messaging fixes (§1) all live
      in exactly this machinery (`bender_sweep_proc_owned`, the `xfb`
      buckets, `bender_gc_mark_fiber_shallow`), so the area is freshly
      understood — good time to close it.
   b. **Per-proc GC independence** (§5.8) — the actual Phase 2b collector-swap
      goal, and the gap table's own "NEXT after Phase 1" item.
   c. **Residual O(n)-per-minor cost** (§5.5) — belongs here too: it IS
      per-minor collector policy, with a specific untested candidate
      (`bender_sweep_drain_pending`'s global tail-walk, `gc.c:1702`) and a
      likely-simple fix (incremental tail pointer).
2. **`lanes=8` crash** (§5.1) — blocks trusting the lanes/M:N path, which
   Phase 3's M:N-scheduler-core item builds on. Fix before depending on lanes.
   Also `lanes=0` (§5.2) in the same area.
3. **Constant-factor spawn costs** (§5.6) — mechanical, independent, low risk;
   can be done in parallel with the above by a second worker.
4. **Then Phase 3**: named registry → supervision-tree library → resident-proc
   migration. See §9.4 for its scope.
5. Remaining: M4b leftovers (§5.7), distribution + hot-code-loading design
   docs, battle-hardening (§5.8).
6. Two filed-not-fixed ASAN UAFs (§5.3, §5.4) — independent of all phases,
   pick up opportunistically; §5.3 has a narrowed, actionable hypothesis.

### 9.4 Phase 3 scope (for when it is actually reached)

Per the gap table (`proc-lanes.md` ~879-882) and `aot-plan.md`:

- **Named registry** — register/lookup procs by name. Status: *recorded,
  not started*. No design doc yet.
- **Supervision trees** — *"primitives DONE (2b), tree library open"*. The
  links/monitors primitives already exist; what is missing is the
  supervision-tree library on top (restart strategies, child specs,
  restart intensity/period, supervisor hierarchies). This is what a
  WhatsApp-style reliability model actually depends on ("a process crashes,
  its supervisor restarts it, nothing else notices").
- **M:N scheduler core / resident-proc migration** — *"recorded, needs
  explicit task"*; make proc migration explicit in M4b. ⚠️ **Gated on
  §5.1** (the lanes crash) being fixed first, since it builds on the lanes
  machinery.
- Related Phase 3 goal referenced elsewhere: struct-sharing (the
  copy-by-default work in §1/3.5 was explicitly designed to "bridge Phase
  3's struct-sharing goal without the aliasing hazards").
