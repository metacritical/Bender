# Phase 4: no-reclaim scoped minor — independent safety review

Status: **reviewed and approved for porting, NOT shipped.**
This document records a source-level safety argument for a candidate that is
still pending a production port and a focused C regression fixture. Nothing
here claims the change is already in `main`.

## Candidate

- Isolated worktree: `/tmp/janet-wave-candidate-35671ede`
- Change surface: `src/core/gc.c` only, in `janet_collect_fiber_minor`.
- Position: immediately **after** the existing fallbacks, **before**
  `JANET_GC_WORK_TICK`, `janet_gc_epoch_next()`, and the mark-phase sets.
- Form reviewed: an eligibility preflight over the fiber's three owned heap
  lists, followed on the no-eligible path by stamp clearing, pending drain,
  heap verify, nursery reset, and early return. The current candidate carries
  this as extracted helper(s); the extraction must stay allocation-free and
  preserve the adjacency and fallback ordering asserted below.
- Validation to date: **96/96** baseline/candidate trials pass across four
  build modes, two placements and three repetitions. Large local (~10x) win;
  negligible cross-lane change.

## The preflight predicate

The candidate scans `f->heap_lists[0..2]` for any object satisfying:

```c
!(obj->flags & JANET_MEM_DISABLED) &&
!janet_slab_obj_old(obj) &&
janet_slab_obj_owner(obj) == f->gc_owner &&
JANET_MEMORY_FIBER != janet_gc_type(obj)
```

This is deliberately **exactly** the reclaim condition of the narrow sweep,
not an approximation of it (`src/core/gc.c:964-966`).

## Why "no eligible object" implies the free set is empty

`janet_sweep_list` (`src/core/gc.c:940-1023`) has exactly four outcomes:

| outcome | line | effect |
|---|---|---|
| disabled | `956-957` | advance the list cursor; **no flag change** |
| marked survivor | `958-963` | clear `JANET_MEM_EPOCH_MASK` |
| reclaim | `964-1003` | unlink, `block_count--`, deinit or quarantine |
| unmarked floater | `1004-1019` | clear `JANET_MEM_EPOCH_MASK` |

Zero eligible objects makes the **reclaim branch unreachable**, so the free
set is empty *regardless of what marking would have produced*. That is the
whole basis for skipping marking: it is not a guess about reachability.

The candidate's stamp clearing is therefore **branch-identical**, not merely
analogous: clearing the epoch bits of every non-disabled listed object and
leaving disabled objects untouched reproduces branches 1, 2 and 4 exactly.
No unlink, no `block_count` change, no `janet_deinit_block`, no quarantine
push can occur.

## Hidden side effects: none on the executed path

- Branches 1/2/4 invoke no callback. `janet_deinit_block` and finalizers occur
  only in the reclaim branch (`985-1003`).
- `janet_sweep_drain_pending` (`1896-1922`) only splices
  `janet_vm.sweep_pending[k]` into the global lists; no reclamation, no flag
  writes.
- `janet_heap_verify` is diagnostic.

Disclosed *deferred* differences: skipping marking also skips three
mark-side producer effects — threaded-abstract registry claim
(`gc.c:304-332`), closure-env detach (`gc.c:429-435` → `fiber.c:886-900`),
and the fiber `JANET_ASYNC_EVENT_MARK` callback (`gc.c:699-701`). Each feeds a
sweep consumer; with the reclaim branch unreachable no consumer reads them
this pass, and the next real collection re-runs them. Detach/memory-timing is
deferred, not altered.

## Why skipping the epoch and visit cycle is safe

`janet_gc_epoch_next` (`gc.c:194-205`) does exactly two things: advance
`janet_gc_epoch` modulo 15, and `gc_visit_cycle++`. Both are mark bookkeeping —
the epoch is what `janet_gc_reachable` (`gc.c:190-192`) compares against a
stamp written by `janet_gc_mark`.

The invariant is a **pair**: every actual collector increments *and* marks
(e.g. `2782` then `2799`). The increment exists solely to invalidate the
previous pass's stamps before a new marking pass writes fresh ones.

A skipped pass performs no marking and reclaims nothing, so the pair is never
used. **Incrementing is deferred, not lost.** The next actual collection
advances normally and invalidates exactly as a non-skipped baseline would;
the epoch sequence *over real collections is unchanged*. Marking is the
producer, sweep is the consumer; when the consumer's decision set is provably
empty, the producer's output and its bookkeeping cannot change any later
verdict. This argument is strictly stronger than "nothing can be freed".

Independently, the fast path clears stamps on **every** non-disabled listed
object, including foreign-owner floaters — the same invalidation the normal
floater branch performs at `1004-1019`. Foreign stamps are therefore handled
identically to baseline; skipping no-op increments neither creates nor
repairs the pre-existing four-bit epoch-wrap behavior.

Scoped minors also never consume a stale visit verdict: they enter
`janet_sweep_proc_owned` (`1985-1992`), not the registry-decref pass in
`janet_sweep`.

## Ordering obligations for the port

1. Keep the existing fallbacks first: `gc_suspend` (`2766`), NULL/unowned
   (`2768-2773`), stuck remembered set (`2775-2779`).
2. Keep predicate and stamp clearing adjacent with **no allocation or
   callback between** them.
3. Return before `work_gc_fiber_minor`, `janet_gc_epoch_next()`, and any
   mark-phase set (`2781-2785`); do not increment an actual-collection
   counter.
4. Reset `f->gc_nursery_bytes` (mirroring `2898`) or `maybe_collect` refires.
5. Do not touch remembered sets, `gc_mark_phase`, or quarantined blocks.
6. Preserve the heap-kind assertions (`946-953`) if practical; dropping them
   loses a diagnostic, not a semantic.

## Proof scope and limits

- Reviewed by reading the source of the swept path and its callers. This is a
  source-level argument, not a formal verification.
- It does **not** establish that the pre-existing foreign-stamp epoch design
  is sound in general; only that omitting a no-mark/no-reclaim increment adds
  no new verdict or reclamation opportunity.
- It does **not** prove the remaining superlinear marking gone. Collections
  with eligible objects still traverse the graph; that cost is a separate
  measured problem (profiles put the dominant path through
  `janet_procat_mark` → `janet_mark_fiber` → `janet_drain_env_queue`).
- Shallow sampled call-stack depth does not bound total traversal: the
  environment drain (`gc.c:450-483`) is iterative and can defer unbounded work
  past the recursive proc-depth counter (`proc.c:984-991`).

## Pending gates before production

- Focused C fixture over a **nonempty but ineligible** owner list: real fiber,
  attributed malloc-large (old) object, temporary `f->proc` link valid only for
  the no-mark branch and cleared before any normal collection or teardown.
  Assert: list membership and `block_count` unchanged, non-disabled stamps
  cleared, disabled stamps preserved, nursery zeroed, visit cycle and epoch
  unchanged, mark/finalizer counters untouched; then a subsequent full
  collection behaves normally.
- Epoch-wrap stress beyond 15 actual collections with live weak entries.
- Pending-chain migration preserved.
- Suites: `suite-proc`, `suite-gc-minor`, `suite-gc-stress`, `suite-ev`,
  `suite-ev2`, `suite-process-heaps`, `suite-fiber-capture`.
- Remove the env-gated diagnostic probe from production code. Note the probe
  omits the fiber exclusion, so its "eligible" count is conservative and is
  not literally the reclaim count.
- Re-measure with diagnostics disabled; keep packaging runs reporting exact
  `result=N` and exit status.

## Baseline-qualified known failure

`test/suite-proc.janet` exits 1 on **both** the untouched pinned baseline and
the candidate, at the same strict-proc export assertion on **line 605**
("strict proc value escaping"). Baseline reproduction is retained under
`/tmp/janet-wave-evidence-35671ede/` evidence as `suite-proc-baseline.stderr`.

Consequently this suite must be reported as **incomplete/blocked at line 605**
for both arms — never as green — and the strict-proc cases after that point are
reachable only through a dedicated harness.

## Verdict

Approved as a safe optimization of scoped minors whose possible free set is
empty. Not approved as a fix for remaining marking cost, and not yet shipped.
