# Why proc spawn cost grew with the live process count

Measured against BEAM. **Status: the N-dependence is found and fixed** in
`33f63f7a`; spawn scaling falls from `N^1.55` to `N^1.11` and 500,000 spawns
go from 18,394 ms to 4,562 ms. See "The fix" below. Everything above that
section is the investigation that led there, including four refuted
hypotheses and two attempts that measured 5x and 58x WORSE, all retained so
they are not retried.

Baseline: `8b18ee81`, `bench/cross-runtime/wave.ben`, `BENDER_LANES=11`,
`BENDER_NO_PROFILE=1`. Re-measured at `faff471f` with
`tools/wave-scaling.sh`.

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

> **Corrected by "The fix" below.** This section's diagnosis -- the whole live
> proc set is re-marked, and `serials` retains it -- is confirmed and is still
> the right description of *what* each expensive collection does. The
> conclusion drawn from it, that the answer is promotion out of the young set,
> is wrong: promotion measured 58x worse (next section but one). What actually
> made the cost quadratic is *how often* those collections ran, which was
> pinned by a 16 MiB clamp on the trigger. The fix is a frequency change and
> touches neither `serials` nor promotion.

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

## The fix (landed `33f63f7a`)

The cause is collection **frequency**, not collection content, and it is one
clamp in `maybe_collect` (`src/core/vm.c`).

The per-fiber minor threshold was already amplified by the live set --
`gc_minor_threshold + block_count * BENDER_FIBER_MINOR_SCALE` -- and then
clamped: `BENDER_FIBER_MINOR_CAP = 16 MiB`. A scoped minor costs Theta(live
blocks reachable from the collecting fiber), so bounding collection work per
unit of allocation requires the trigger to stay proportional to that live
set. The clamp froze the trigger while per-collection cost kept growing, so
the number of whole-live-set walks stayed proportional to total allocation and
each one cost Theta(N). The product is the Theta(N^2).

Measured with `tools/gc-spawn-probe.patch` (env-gated, never shipped):

| evidence | N=50,000 | N=200,000 |
|---|---:|---:|
| trigger level every dispatcher minor fired at | 16,777,216 B | 16,777,216 B |
| lane `block_count` at those minors | 430,147 | 1,172,572 |
| trigger the amplifier *would* have set | ~110 MB | ~300 MB |
| whole-live-set walks performed | 5 | **17** |
| fiber marks in those walks | 123,691 | 1,624,725 |
| largest single scoped minor | 41,905 fibers | **193,955 fibers** (0.97*N) |

Every dispatcher minor fired at *exactly* the clamp, while the unclamped
trigger was already 18x higher and rising -- so the clamp's truncation factor
itself grew with N. Scoped-minor mark work at N=200,000 fell 27.0M -> 4.79M.

### The trigger must be scaled by a SNAPSHOT, not a live count

Removing the clamp and scaling by `bender_vm.block_count` read at trigger time
**regressed `suite-process-heaps:108`** ("auto minor reset the counter"). A
fiber that retains what it allocates grows `block_count` at least as fast as
its own nursery grows -- the scale is 256 against a mean block of ~48 bytes --
so the inequality `nursery >= base + block_count * 256` becomes
*unsatisfiable* and the scoped collector silently goes unreachable. That is
the same defect class `b0b90b6c` fixed for the global minor, and this suite
asserts exactly that reachability.

The shipped form scales by `BenderFiber.gc_minor_blocks`, the live block count
snapshotted at the end of that fiber's last scoped collection
(`bender_fiber_minor_reset`). It is constant between two collections while the
nursery only grows, so the trigger is always crossed after a bounded amount of
the fiber's own allocation, and it still equals the live set, so the
amortisation argument holds.

Only *when* a collection runs changes -- never what is marked or swept. That
is why this carries none of the soundness risk of the two reverted attempts
above.

### Result

Spawn phase, min-of-3 per point, fresh process per run, swap checked:

| N | before | after | us/spawn before | us/spawn after | Erlang us/spawn |
|---|---:|---:|---:|---:|---:|
| 20,000 | 125 ms | 126 ms | 6.25 | 6.30 | -- |
| 50,000 | 374 ms | 311 ms | 7.48 | 6.22 | -- |
| 100,000 | 1,104 ms | 842 ms | 11.04 | 8.42 | -- |
| 200,000 | 3,622 ms | 1,401 ms | 18.11 | 7.00 | 1.02 |
| 500,000 | 18,394 ms | 4,562 ms | 36.79 | **9.12** | 1.31 |

- Scaling: **`N^1.55` -> `N^1.11`**; 4.03x at 500,000.
- Peak RSS at 500,000: 3,370 -> 3,453 MB (**+2.5%**). The headroom traded for
  the lower collection rate stays proportional to the live set, and the global
  major branch still bounds total footprint.
- The fixed arm is also *stable* where the old one was not: repeated 500,000
  runs gave 4,573 / 4,562 ms (+-0.2%) against 35,890 / 18,394 ms. Re-marking
  the whole live set repeatedly is swap-sensitive; not doing so is not.
- Erlang on the same machine (OTP 17.1.1): 204 ms at 200,000, 655 ms at
  500,000. The gap at 500,000 is **7x, down from 28x**. Not a parity claim.

## Refuted in this round (do not re-derive)

**The 16 MiB clamp is irrelevant -- WRONG, but for a real reason.** The first
probe read `gc_nursery_bytes` at the *last* scoped minor and found 192 bytes,
nowhere near the clamp, which looks like proof the clamp is not involved.
It is not: there are two distinct populations of scoped minor. Exactly one per
spawn is a **completion** minor from `ev.c:2514`, which is tiny (29-135 marks)
and fires at whatever the nursery happens to hold. A *few* are the lane
dispatcher's **threshold** minors, and those are the ones that fire at the
clamp and walk the whole live set. Averages and last-sample reads both hide
this; the per-collection identity dump (`[gc-probe-big]`) is what separated
them. Attribute by collection kind *and* by collecting fiber, never by a
total.

**The proc-capture depth cap leaks through the env drain -- true, and
measured NEUTRAL.** `bender_proc_mark_depth` is maintained only around the
recursive descent in `bender_procat_mark`, while closure environments are
marked later from the flat `bender_drain_env_queue` loop, where the counter
has already unwound to 0. So `BENDER_PROC_MARK_CHAIN_DEPTH` (`proc.h:50`)
genuinely does not bind across a deferred env -- the hole
`phase4-no-reclaim-minor-review.md` flagged. Carrying the depth with each
queued entry so the cap binds changed the largest fiber walk from 193,955 to
194,075 at N=200,000: **nothing**. Reverted.

The reason is worth keeping: the dispatcher's walk is **breadth, not depth**.
It goes dispatcher frame -> `serials` table -> N *sibling* proc abstracts, each
one hop from the table. A depth cap cannot bound a fan-out. Do not reach for
the depth cap for a breadth problem.

**`heap_strict` fibers bypassing the amplifier (`vm.c:321`) is not involved
here.** Instrumented `proc_minor=0` at every N on this workload: no proc on
the wave chain is strict, so that branch never runs.

**`top_dyns`, `profile_table` and the named proc registry are not on this
path.** Per-subpath marks measured exactly **0** for all three at every N, so
the lane `serials` table is not reached through the C-root pin set. Of a
scoped minor's six root subpaths, `bender_mark_fiber(f)` is 71-88% of the
marks, `bender_ev_mark_shallow` is exactly 3 marks per minor, and the xfb
bucket is the small remainder.

## What is still open

The residual is `N^1.11`, not linear, and 7x off BEAM. The remaining term is
the **marking constant**, i.e. hypothesis H3 in the original plan:
`bender_procat_mark` re-walks ~6 blocks per proc, so the surviving
whole-live-set walks (3 at N=200,000, down from 17) are still Theta(N) each.
Reducing the number of them further is a frequency change like this one;
reducing their cost means not putting N sibling proc handles on one fiber's
root path, which is the ownership question BEAM answers structurally (a pid is
an immediate packing a slot number, `erl_ptab.h:319-326`, not a traced heap
pointer).

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
- Reproduce with the committed harnesses rather than ad-hoc runs:
  `tools/wave-scaling.sh -b BIN -r 3 20000 50000 100000 200000` for the curve
  and peak RSS, `tools/gate-compare.sh BASELINE_BIN CANDIDATE_BIN` for the
  paired suites, and `patch -p0 < tools/gc-spawn-probe.patch` plus
  `make CFLAGS="-O2 -g -DBENDER_GC_PROBE"` and `BENDER_GC_PROBE=1` for the
  collector attribution. Report a CURVE: both reverted attempts and the
  mis-read of the clamp came from trusting one point.
- Attribute by collection KIND and by collecting FIBER. A single total, or a
  last-sample read of `gc_nursery_bytes`, averages the lane dispatcher's few
  whole-live-set walks together with the one-per-spawn completion minors and
  points at the wrong mechanism -- which is exactly what happened here first.
- The C fixtures `test/suite-gc-major-pressure.sh` and
  `test/suite-gc-trace-cache.sh` need `make NATIVE_EXECUTORS=1` (their
  telemetry asserts read `work_gc_*`) and a writable `BENDER_AOT_TEST_ROOT`.
  Without the former they fail on a no-op counter, which is a build mismatch
  and not a regression.
