# Why proc spawn cost grows with the live process count

Measured against BEAM, which stays flat. This records the root cause, two
refuted hypotheses, and one fix attempt that **made things 5x worse** so it is
not tried again.

Baseline: `8b18ee81`, `bench/cross-runtime/wave.ben`, `BENDER_LANES=11`,
`BENDER_NO_PROFILE=1`.

## The measurement

Trampoline chain, per-unit cost: 11 us at N=20,000 rising to 75 us at
N=500,000, spawn-dominated (32,937 ms of a 37,423 ms total). Erlang stays at
1-2 us through 500,000.

Instrumented disposable build, spawn phase at N=200,000 (~3,600 ms):

| path | ms | share |
|---|---|---|
| scoped minor (`bender_collect_fiber_minor`) | 2,570 | **71%** |
| full sweep incl. per-fiber census | 32 | 0.9% |
| xfb linear loop (`gc.c:2643`) | 0.9 | 0.025% |
| global minor (`bender_collect_minor`) | 0 | **0%** |

Within the scoped minor: mark is ~87%, and `bender_procat_mark` is 60-69% of
the mark.

Control that isolates the cause: a **fan** of procs that complete is flat
(7.9 us/proc at 50,000, 7.6 us at 200,000), while the **chain**, which parks
every proc on `proc/recv` so all N stay live, grows to ~20 us/proc at 200,000.
So the cost tracks the LIVE set, not the number ever spawned.

## Refuted hypotheses

**xfb is not the cause.** `bender_mark_roots` does walk the cross-fiber remember
set unconditionally at `gc.c:2643-2644`, the set is genuinely append-only, and
the `:2638` comment claiming a prune "in 1.3" is stale — all confirmed. But the
loop fires 11 times total and costs 0.9 ms, i.e. 0.025% of the spawn phase.
Capping it is memory hygiene, not performance.

**The `block_count * BENDER_FIBER_MINOR_SCALE` amplifier is unreachable here.**
`bender_collect_minor` never fires on this workload (instrumented: 0 calls at
every N), because `maybe_collect`'s global-minor branch (`vm.c:305-319`) is an
`else if` behind the per-fiber branch, which always wins during a spawn burst.

**The C proc registry is not involved.** `bender_proc_registry_table`
(`proc.c:1769-1773`) is opt-in, created lazily, and `cfun_proc_spawn_impl` never
touches it.

**The slab directory is not involved.** `bender_slab_dir_lookup`
(`gc.c:1398-1407`) is linear-probe at load factor <= 1/2; no pathological chains.

## Root cause

Nothing is ever promoted out of the young set, so each scoped minor re-marks the
whole live proc set reachable from the lane dispatcher's frame. Instrumented
dispatcher-heap live blocks: 309,840 at N=50,000 to 1,272,135 at N=200,000
(~6 blocks per proc of `bender_procat_mark` retraversal), with `old=0` on every
sample.

The per-lane `serials` table (`src/boot/boot.ben:3678`, `:3693`) is a
contributing retainer: it is never removed from — the only read is `:3760` on
`:stop` — so a lane keeps every proc handle it ever spawned, and that table
lives in the dispatcher's frame, on the collector's root path.

This is the **third** instance of one recurring shape. The first two are fixed
in-tree with the same reasoning:

| site | shape | fix |
|---|---|---|
| `ev.c:796-806` | deep-marking queued fibers: O(N) per minor x N actors | `bender_ev_mark_shallow`, `ev.c:813` |
| `proc.c:1020-1037` | proc->fiber->env->proc chains: O(remaining) x N minors | depth cap 16, `proc.h:50` — measured 204x at n=32,000 on this wave |

## Fix attempt that FAILED — do not repeat

Pruning `serials` of finished procs (amortized `proc/alive?` sweep every 256
spawns) **regressed the chain 5x**: 200,000 went from 5,186 ms to 26,477 ms.

Reason: the sweep is O(live) per invocation, and in a chain nothing is dead, so
it pays a full scan and reclaims nothing — N/256 scans x O(N) of wasted work.
Reverted.

If `serials` is pruned at all it must be either event-driven on proc exit, or a
round-robin cursor scanning a FIXED number of entries per sweep so the work is
O(1) amortized per spawn. And it would only help workloads where procs die; it
cannot help the chain, where the live set is the cost.

## Second failed attempt: generational scoped minor — do not repeat

Attempted in an isolated worktree off `fa0c78dd`. Two changes, both reverted:

1. **Age pages in `bender_collect_fiber_minor`**, restricted to pages the fiber
   exclusively owns (`owner == gc_owner && !mixed`), with the same
   dirty-once-at-young->old promotion barrier the global path uses.
2. **Re-enable the table case** in the old+clean traversal-skip predicate
   (`gc.c:2238-2242`), where a `#if 0 /* TEMPORARY 2b-iii bisect */` had left
   tables always traversing. Table stores *are* fully barriered
   (`table.c:297-298, :376-377, :389-390, :441-442, :583`), so this looked safe.

Result: **58x WORSE.** The 20,000 chain went 268 ms -> 15,678 ms, and 200,000
failed outright. Isolating the halves, **the aging change alone** accounts for
it: 16,054 ms at 20,000 with the table skip reverted.

Why it fails: a scoped minor's sweep only covers the fiber's own young blocks,
and minors deliberately skip old sweeping. Promoting those pages to old means a
scoped minor can never reclaim them again, so garbage accumulates until a full
collect is forced. The existing comment in `bender_collect_process` —
"No aging, no global pressure reset, no weak dropping (minor rules)" — is a
deliberate and correct constraint, not an oversight.

Corollary: the table-skip change is a **no-op on its own**, because without
aging nothing ever reaches `BENDER_SLAB_OLD_AGE`. It should not be enabled
without a working promotion story, and the stale `#if 0` bisect should be
annotated rather than flipped.

## The actual fix, still not found

Make the scoped minor generational: promote surviving pages so a minor stops
re-marking the established live set (`gc.c:2318` gate plus page aging). This is
the high-value change and also the risky one — `gc.c:1820-1835` documents why
full collects must pre-clear every REACHABLE bit, because a scoped collect marks
objects on other fibers' lists and sweeps only its own, so a stale bit makes a
later mark early-return and skip a live object's dependents. Any promotion work
must preserve that invariant.

Target for comparison, verified in OTP `OTP-29.1.2-1539-g4ba5448abb`
(`/Users/pankajdoharey/Development/Projects/ML/otp`, read-only):

- `erl_gc.c` contains **zero** references to the process table, so collecting
  one process cannot walk another; a pid is an immediate packing a slot number
  (`erl_ptab.h:319-326`), not a traced heap pointer.
- A plain spawn reads exactly two global atomics (`erl_process.c:12572`,
  `:12669`), with no process list or registry walk.
- The process table never resizes or rehashes: allocated once at
  `erl_ptab.c:393-397`; the only `realloc` serves `processes/0` enumeration.

## Method notes

- `BENDER_NO_PROFILE=1` matters: interpreter runs collect diagnostics by default
  (visible as `profile-merge:` lines writing to
  `~/.cache/bender/pgo/interpreter/`). It is handled in `src/boot/boot.ben`, not
  `src/core/`, so grepping the C sources suggests wrongly that it is a no-op.
  Measured here at ~10% on the fan at 50,000; earlier work measured far larger
  effects and drew a false "quadratic" conclusion from profiling-on runs.
- Totals move with machine state: the same 200,000 chain measured 5,186 ms
  during the sweep and 4,182 ms after the revert, on a box carrying 12-15 GiB of
  swap. Treat sub-2x differences as unattributable without repetition.
