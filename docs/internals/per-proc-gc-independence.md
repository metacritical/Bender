# Per-Proc GC Independence — Scoped Design Note

**Status**: design only, not implemented. Stream B item B4 (Phase 2b's
collector-swap end goal per `erlang-parity-handoff.md` §5.8/§9.3(b)).
Written against the tree as of 2026-10-17 (post
`bender_threaded_visited` cycle-stamp fix; see proc-lanes.md).

## 1. What "per-proc GC independence" means here

Today every fiber/proc allocates from **one VM-global slab state**
(`BenderVM.slab_state`): shared page directory, shared size-class
freelists, shared young-byte counter, shared `xfb` remembered set, and a
shared `threaded_abstracts` registry. Collections are *scoped sweeps*
over a shared heap (`bender_sweep_proc_owned`: walk only the target
fiber's three `heap_lists` with an owner filter), not independent
heaps. "Independence" = each strict/isolated proc gets a **heap that a
private (or heap-local) collector can collect without walking, locking,
or reading any other proc's state** — the Erlang property that makes
per-process GC cost proportional to the process's own live set, always.

This is the full Phase 2b collector swap; the three landed slices
(2b-i scoped roots, 2b-ii string-heap split, 2b-iii slab storage +
generational policy) were explicitly the runway for it.

## 2. What already exists and can be reused as-is

- **Owner attribution**: every slab page has a single `page->owner`
  (write-once at fresh-page allocation; only transition is
  `K → 0/mixed`). `gc_owner_next` is the monotonic id source.
- **Per-fiber block lists**: every fiber already carries
  `heap_lists[3]`; the filing invariant (fiber objects only in
  globals) keeps census walks stable.
- **Narrow sweeps**: `bender_sweep_proc_owned` is already O(own lists).
- **Remembered set**: `xfb` owner-bucketed hash (4096 slots) gives
  O(1) insert and O(own-entries) scan — exactly the shape a shared→
  private edge log needs.
- **Scoped roots**: `bender_gcshadow_push/mark/pop_to` (2b-i) and
  `bender_ev_mark_shallow` already let a scoped collector protect C-side
  roots without deep walks.
- **Generational policy**: page age + dirty flags + write barriers in
  table/array store primitives (2b-iii) already skip old-clean
  containers in minors.

## 3. The actual swap, in ordered slices

1. **Per-owner slab arenas.** Split `BenderSlabState`'s page directory
   and freelists into a small shared arena (globals, root fiber,
   non-strict fibers) plus one `BenderSlabArena` per *live strict owner*
   (keyed by `gc_owner`, hash table of arenas, arena freed when the
   owner's fiber deinits). `bender_gcalloc` routes by
   `fiber->gc_owner` (attribution already computed there). Large
   objects stay on the malloc path, unchanged. This is mechanical but
   touches every allocation fast path — the riskiest slice for
   regressions, gate it hardest.
2. **Per-arena young accounting + collection triggers.** Move
   `gc_young_bytes`/threshold accounting into the arena so a proc's
   minor fires on *its own* pressure (today's nursery counters already
   approximate this per fiber; the global `gc_minor_threshold` stays
   for the shared arena only).
3. **Private minors over the arena.** `bender_collect_fiber_minor`
   degenerates to: mark arena roots (fiber + proc fields + shadow
   stack slice + ev-shallow), mark the owner's xfb bucket, sweep the
   arena's three lists with NO owner filter (the arena is pure-own by
   construction; mixed pages with owner 0 already degrade correctly).
   The cycle-stamp visited marker (this session's fix) already removes
   the last global-registry walk from this path.
4. **Shared↔private edges.** Keep the single global `xfb` insert site;
   with arenas, an edge into a private arena page dirties/buckets by
   target arena, and the target's minor marks the source bucket. The
   existing safety argument (owner only degrades to mixed/0) carries
   over unchanged.
5. **Majors/teardown.** Full collects iterate arenas; arena free at
   fiber deinit must drain any pending `sweep_pending` chains the same
   way `bender_deinit_block` parks them today. `threaded_abstracts`
   stays VM-global (channels are cross-proc by nature); only its
   *visit* cost was per-minor and is already O(1).

## 4. What a swap does NOT fix (honesty section)

- **Cross-proc channels** are inherently shared; values in transit
  live on sender/allocator arenas but are reachable from the receiver.
  The remembered-set cost is irreducible without copy-on-send
  semantics (already landed for strict procs).
- **The scheduler gap**: the 200x Erlang delta is M:N scheduling, not
  GC (handoff §3). Independence bounds per-proc GC cost, it does not
  add parallelism.
- **The `lanes=8` UAF (§5.1)** is in the same subsystem; any arena
  split multiplies its blast radius. Gate item 1 above on that bug's
  fix or on a single-threaded-only build flag.

## 5. Suggested first implementation milestone

Slice 1 alone (arenas, shared collection logic otherwise unchanged)
measured on `bench/agents.ben` N=20000/100000 plus the full battery
is the right first PR-sized unit. Everything after it is policy
refinement inside already-proven sweep/mark code.
