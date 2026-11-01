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
  500,000.

> **CORRECTION.** Those Erlang readings were taken while the box carried
> ~9.8 GiB of swap and are inflated, so the "7x" gap first recorded here was
> wrong. Re-measured alternating against the same binaries at ~3.5 GiB swap,
> Erlang spawns at **0.57-0.58 us/spawn** (114 ms at 200,000, 289 ms at
> 500,000) and is flat. The honest figures are therefore:
>
> | N | baseline | fixed | Erlang | gap before -> after |
> |---|---:|---:|---:|---|
> | 200,000 | 16.94 us | 7.03 us | 0.57 us | 29.7x -> **12.3x** |
> | 500,000 | 37.41 us | 8.56 us | 0.58 us | 64.5x -> **14.8x** |
>
> The 4.3x improvement stands; the multiple against BEAM does not. This is the
> method note at the bottom of this file applying to its own author: a
> cross-runtime ratio is only as good as the machine state of its slowest arm,
> so both arms must be re-run together.

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

## Where the remaining cost is -- measured, not assumed

After the fix, GC live-set tracing is no longer the dominant term, and the
remaining gap to BEAM is **the routed-spawn constant**. Two controls localise
it.

**Control 1 -- a fan with no pid capture is flat on BOTH arms.**
`bench/spawn-exit-scaling.ben 200000 20000` holds every proc live but captures
no downstream pid, so `9d5f7939` does not co-locate and spawns spread over all
11 lanes. Per-spawn cost is flat near 6 us on baseline *and* on the fix, and
still flat at N=500,000 (6.2-7.6 us across ten buckets). So the quadratic was
never "many live procs" as such -- it needed the whole live set on **one**
lane dispatcher's root path, which the chain's pid capture produces.

Relative to that flat floor at N=500,000:

| arm | us/spawn | vs flat floor |
|---|---:|---:|
| baseline chain | 36.79 | 5.6x |
| fixed chain | 9.12 | **1.4x** |
| flat fan floor | ~6.6 | 1.0x |
| Erlang (OTP 17.1.1, same box) | 1.31 | 0.2x |

**Control 2 -- the floor itself is routing, and local spawn is already at
BEAM parity.** Varying only the lane count on the fan at N=100,000:

| `BENDER_LANES` | us/spawn (min of 4 buckets) | delta |
|---|---:|---|
| 0 (local, no routing) | **1.09** | -- |
| 1 (routed, one lane) | 4.84 | **+3.75, routing** |
| 2 | 4.92 | +0.08 |
| 11 | 5.82 | +0.98 |

Going from no routing to one lane costs 3.75 us/spawn; going from one lane to
eleven costs 1.0 us more. So the constant is the **routed spawn path**, not
lane count and not contention.

Confirmed on the real chain (`wave.ben`, `BENDER_LANES=0`, correct `result=N`):

| N | local spawn | us/spawn | routed us/spawn |
|---|---:|---:|---:|
| 20,000 | 49 ms | 2.45 | 6.30 |
| 50,000 | 74 ms | 1.48 | 6.22 |
| 100,000 | 176 ms | 1.76 | 8.42 |
| 200,000 | 374 ms | **1.87** | 7.00 |

Local spawn is flat at ~1.5-1.9 us/spawn against Erlang's 1.02 us at the same
N -- **1.8x, not 18x.** Bender's process allocation is not the problem.

> Caveat: `BENDER_LANES=0` is a measurement isolation, NOT a usable
> configuration for this workload. With no lanes the wave phase at N=200,000
> takes 372,670 ms, because every hop of the dependency chain becomes a
> same-VM event-loop round trip. It isolates spawn cost and nothing else.

## The routed-spawn constant, attributed

`tools/spawn-routed-probe.patch` times the spawner thread's segments and sums
them over every routed spawn in one run. This is a within-run sum over 200,000
samples, not a cross-run wall-clock comparison, so it is robust on a loaded
box. Only the spawner is timed, which is the correct scope: `spawn_ms` is
main-thread wall time, so the target lane's unmarshal and fiber/proc creation
are concurrent and off this clock.

At N=200,000, `BENDER_LANES=11`, per routed spawn:

| component | us/spawn | share |
|---|---:|---:|
| `bender_loop1_poll_now` -- the every-4th self-pipe drain (`proc.c:327`) | **3.221** | **58%** |
| `bender_channel_push` -- contains the closure marshal and the pipe post | 2.189 | 40% |
| capture scans (`find_mutable_capture` + `find_pid_lane`, both walks) | 0.013 | 0.2% |
| spawn-msg + pid tuple allocation | 0.036 | 0.7% |
| total | 5.532 | |
| (timer floor, for subtraction) | 0.006 | |

Two results worth keeping.

**The drain, not the marshal, is the largest component.** `proc.c:317` asserts
"One non-blocking poll per 4 spawns is noise next to the marshal cost." That
is now measured and **wrong**: it is 1.5x the marshal and 58% of the constant.
Each actual poll costs ~12.9 us, since it only runs every 4th spawn.

**The drain is load-bearing, so its frequency is not a lever.** Sweeping
`BENDER_SPAWN_POLL_MASK` moves cost rather than removing it -- as the interval
grows the in-function total falls (7.13 -> 4.29 us at mask 3 -> 255) but
`push` *rises* (3.02 -> 4.03) and spawn wall time gets worse (1,938 ->
2,202 ms). Polling more often is not better either: at mask 1/3/7 the
same-mask spread (2,366 vs 2,997 ms) exceeded the between-mask spread, with
`wave_ms` swinging 3x independently, on a box at 9.8 GiB of swap. Per this
file's own method note, that is **not attributable**. Do not "tune" this
constant; the spawner is paying for delivery work that the architecture gives
it no other way to discharge, which is what `proc.c:323-324` already says
("needs real backpressure in the lane dispatcher, not a bigger drain
frequency").

## End-to-end trampoline wave, after both fixes

Matched alternating runs, min of 2, same box at ~3.5 GiB swap, `BENDER_LANES=11`,
`BENDER_NO_PROFILE=1`. `wave.ben` asserts `result == N`, so every one of the N
procs provably stayed live for the whole measurement.

| N | arm | spawn ms | wave ms | total ms | peak RSS |
|---|---|---:|---:|---:|---:|
| 200,000 | baseline `faff471f` | 3,388 | 538 | 3,926 | 1,377 MB |
| 200,000 | **fixed** | **1,406** | **256** | **1,662** | **655 MB** |
| 200,000 | Erlang OTP 17.1.1 | 114 | 185 | 299 | 694 MB |
| 500,000 | baseline `faff471f` | 18,704 | 2,138 | 20,842 | 2,798 MB |
| 500,000 | **fixed** | **4,282** | **662** | **4,944** | **1,526 MB** |
| 500,000 | Erlang OTP 17.1.1 | 289 | 431 | 720 | 1,457 MB |

- End to end: **4.2x faster and 1.8x lighter** at 500,000.
- **Memory is at parity with BEAM** on this workload: 5.6% lighter at 200,000,
  4.7% heavier at 500,000.
- The **wave** phase is within **1.4-1.5x** of BEAM, and improved 2.1-3.2x
  purely as a side effect of the collector work -- it was never the target.
- **Spawn remains the gap at 12-15x.** That is the routed-spawn constant
  attributed above (self-pipe drain 58%, closure marshal + pipe post 40%), not
  GC and not process creation: local spawn is 1.87 us/spawn flat.

## Processes are HEAVY: 94% of per-proc memory is slab-page slack

The swap pressure during the measurements above is not a measurement nuisance,
it is a second defect. A "lightweight process" runtime is making a memory
claim, and this one does not hold.

Marginal bytes per process, taken from the RSS *difference* between two
process counts so the runtime's fixed footprint cannot flatter it (peak RSS
via `/usr/bin/time -l`, `wave.ben`):

| runtime | RSS @200,000 | RSS @500,000 | marginal B/proc |
|---|---:|---:|---:|
| Erlang (OTP 17.1.1) | 694 MB | 1,183 MB | **1,709** |
| Bender | 1,428 MB | 3,453 MB | **7,078** |

**4.1x heavier per process.** Note Erlang's *average* per proc FALLS with N
(3,640 -> 2,481 bytes) as fixed cost amortises, while Bender's stays pinned
near 7,000 -- so Bender's cost is genuinely marginal, not boot overhead. At
1,000,000 procs that is ~7 GB against ~1.7 GB, which is the swap.

### It is not the structs

`BenderProcess` is small, the mailbox is already LAZY (`proc.h:13-23`), and the
eager supervisor channel is only ~200 bytes because `bender_chan_init` leaves
all three `BenderQueue`s empty. Replacing the process representation with
something lighter therefore buys approximately nothing; this was tested and
refuted. `tools/slab-page-probe.patch` says where the memory actually is:

| N | slab pages | pages/proc | from a fiber-PRIVATE cursor | slab MB | % of RSS |
|---|---:|---:|---:|---:|---:|
| 100,000 | 142,961 | 1.43 | **93.0%** | 558 | 78.4% |
| 200,000 | 287,722 | 1.44 | **92.8%** | 1,124 | 78.7% |

- Marginal: **1.45 slab pages per proc = 5,929 B/proc**, i.e. **84%** of the
  7,078 B marginal RSS.
- Real data per proc is ~5.9 blocks (the measured `block_count` per proc) at
  32-512 byte size classes, so ~378 bytes.
- **Page slack is therefore ~5,552 B/proc: 93.6% of per-proc memory is empty
  space inside 4 KiB pages.**

The mechanism is explicit in `bender_slab_alloc`: `BENDER_SLAB_PAGE_SIZE` is
4096 and every fiber carries `nursery_cur[BENDER_SLAB_NCLASSES]` -- a *private*
bump page per size class. Spawn *construction* is already routed through the
shared cursor, and that works (only 7% of claims are shared, but those are the
construction ones). Steady-state fibers "keep their own cur page exactly as
before", so a proc that allocates ~378 bytes and then parks forever holds
1.45 exclusive 4 KiB pages.

Even the cheapest possible proc pays it: measured at N=200,000, an `inert`
proc (`(fn [] nil)`) costs 5,041 B/proc and a `parked` proc
(`(fn [] (proc/recv))`) 5,856 B/proc (`bench/proc-footprint.ben`). So this is
a floor set by page granularity, not by what the body does.

The upside is large and specific: 378 bytes of data plus the fiber, proc
abstract, sup channel and a 16-slot stack is on the order of **1.1 KB**, which
would be *below* Erlang's 1,709 B marginal. The slack is recoverable, not
inherent.

### Merging BenderProcess and BenderFiber is NOT the memory lever

Asked whether the two were ever coupled into one structure to cut GC pressure
and pointer hopping. They were not. `proc.h:27` holds `BenderFiber *fiber` and
`bender.h:1118` holds `struct BenderProcess *proc` -- two separately allocated
GC objects joined by a mutual-pin cycle (`bender_mark_fiber` marks
`fiber->proc`, `bender_procat_mark` marks `proc->fiber`). `fa158856`
("fibers-inside-procs beats either tier alone") is about the two concurrency
TIERS composing, not about merging the structs.

Measured sizes, so the trade is arithmetic rather than opinion:

| allocation | head | payload | total |
|---|---:|---:|---:|
| fiber GC block | 16 | 392 | 408 |
| `fiber->data` stack (a separate `bender_realloc`, 16 slots) | 0 | 128 | 128 |
| proc abstract | 64 | 80 | 144 |
| sup channel abstract (eager) | 64 | 176 | 240 |
| | **144** | **776** | **920** |

That ~1 KB of structs matches `f54eab0b`'s accounting. Merging the fiber into
the proc saves **one 16-byte GCObject header**; folding the sup channel in too
saves **one 64-byte AbstractHead** -- note an AbstractHead (64 B) costs most of
a `BenderProcess` payload (80 B). So the ceiling on struct merging is ~80
B/proc, i.e. **1.1% of the measured 7,078 B per live proc**, against the
**78%** that is page slack.

What merging DOES buy is mark work and indirection, not bytes: one fewer GC
object among the ~5.9 blocks marked per proc is **~17% of per-proc mark
visits**, it removes the proc<->fiber cycle the collector must keep consistent,
and it removes a pointer hop on every `proc->fiber` access. Since
`bender_procat_mark` was 60-69% of scoped-minor mark time, that is real.

It is not free. A fiber must remain a first-class standalone GC object --
`ev/go` tasklets, generators, the root fiber and compiler fibers all have no
proc -- so merging means `BenderProcess` EMBEDS a `BenderFiber`, after which
every site that treats a `BenderFiber *` as its own GC object
(`bender_gc_mark`, `bender_gc_reachable`, `bender_slab_obj_owner`,
`bender_gc_type(obj) == BENDER_MEMORY_FIBER`, `heap_lists` filing,
`bender_census_remove`) must tolerate a fiber with no header of its own. Worth
doing for the mark-work win; it must not be sold as the footprint fix.

### FIXED: the hybrid nursery cursor (`35befbb2`)

The gate was wrong, not the design. `3401dc30` asked "is this fiber being
constructed"; the right question is "has this fiber earned a page". A fiber now
draws from a shared cursor until it has allocated `BENDER_SLAB_PAGE_SIZE` in
total (`BenderFiber.gc_total_bytes`, monotonic), then is promoted to its own
cursor permanently. A fiber that has not allocated a page's worth cannot fill a
private page, so many such procs pack into one.

The hybrid uses its OWN shared pool (`shy_cur[]`), stamped owner 0 / mixed. A
fiber whose objects straddle a shared page and its own page otherwise produces
spurious cross-owner edges in the append-only remembered set --
`suite-process-heaps:139` ("same-fiber silent") catches it. `cur[]` keeps the
construction stamping that A3 of
`test/c/test-gc-proc-minor-no-candidates.c` pins, and one cursor cannot carry
both. `shy_cur[]` nulls with `cur[]` in `bender_slab_free_pages` or a freed
page dangles.

**Gate 1, footprint** (`bench/proc-footprint.ben`, parked procs, marginal from
the RSS difference, same box back-to-back):

| | 100,000 | 300,000 | marginal B/proc |
|---|---:|---:|---:|
| before | 650 MB | 1,784 MB | 5,945 |
| after | 300 MB | 843 MB | **2,847** (2.09x lighter) |
| Erlang, same basis | | | 1,709 |

On `wave.ben` at 500,000: 1,523 MB against 3,453 MB, versus Erlang's 1,183 MB.

**Gate 2, the spawn curve** must not regress past `N^1.11`. It does not --
5.70 / 5.82 / 7.61 / 6.64 / 8.23 us/spawn at N = 20k/50k/100k/200k/500k,
against 6.75 / 6.52 / 8.88 / 7.78 / 9.12 before. Slightly faster in absolute
terms: packed pages have better locality than pages 1/64 full.

Both gates had to be measured together because they pull against each other --
sharing pages defers reclamation to global collections, which is precisely what
could have re-inflated the spawn curve. It did not.

### Why it was not a one-line fix

Private pages are load-bearing for the GC work above: a scoped minor sweeps
`f->heap_lists[]` filtered by `bender_slab_obj_owner(obj) == f->gc_owner`, and
a page that takes a second owner is marked `mixed`, reads as owner 0, and then
"float[s] until a global collection instead of a scoped one". Simply sharing
pages trades the memory win for the scaling win. The hybrid is the thing to
measure: use the shared cursor until a fiber's own footprint crosses a
threshold, then promote it to a private page, so small parked procs stay
compact while genuinely allocating procs keep page purity.

## What is still open

Three items, in value order.

1. **Batch routed spawns per target lane.** The routed path pays one
   self-pipe event *and* one closure marshal per spawn, and the drain that
   keeps the pipe from overflowing is itself the largest component. Both the
   58% drain and the 40% push are per-event costs, so coalescing consecutive
   same-lane spawns into one control message carrying K entries attacks both
   at once -- and the chain workload is the ideal case, because
   `find_pid_lane` already sends every spawn in a chain to the SAME lane.
   This is a lane-dispatcher protocol change (`proc.c` plus the dispatcher in
   `src/boot/boot.ben`), not a constant tweak. Measure against the local-spawn
   floor of 1.87 us/spawn, not against zero.

2. **Per-proc footprint: the inventory, and the real floor.** Asked whether
   ~300 B/proc is reachable. It is not, and the reason is worth stating
   precisely: `sizeof(BenderFiber)` is 392 bytes, so the fiber block alone is
   408 and no arrangement of the other objects gets a *runnable* process under
   ~400 B. For scale, BEAM's own floor is a 233-word initial heap = 1,864 B,
   so 300 B would be 6x better than BEAM, not 1.5x.

   What IS reachable comes from `tools/alloc-census-probe.patch`, which
   inventories gcalloc by type instead of ablating the workload (ablation
   fails: a proc that completes is reclaimed and reads ~1 B/proc). At
   N=200,000 on `bench/spawn-only-chain.ben`: **18 allocations and 1,654
   requested bytes per proc**, against 2,847 B measured marginal RSS -- the
   gap being slab size-class rounding, the malloc'd fiber stack and residual
   page slack.

   | type | per proc | B/proc | what it is |
   |---|---:|---:|---|
   | tuple | 6.0 | 384 | pid tuple, spawn message, cross-pid, ... |
   | fiber | 1.0 | 400 | irreducible without shrinking `BenderFiber` |
   | abstract | 2.0 | 376 | proc abstract (144) + supervisor channel (240) |
   | function | 3.0 | 128 | entry closure, dispatcher wrapper, marshal copies |
   | funcenv | 3.0 | 96 | their environments |
   | funcdef | 1.0 | 169 | **one compiled prototype PER PROC** |
   | table | 1.0 | 48 | the `setdyn :proc-cross-pid` dyns table |
   | string | 1.0 | 52 | |
   | **total** | **18.0** | **1,654** | |

   > **The mailbox is NOT in this census.** `bender_abstract_begin_threaded`
   > uses `bender_malloc`, not `bender_gcalloc`, because a threaded abstract
   > must outlive any one VM's heap so any lane can push to it. So the
   > materialized mailbox (~240 B) is invisible here, and the identified total
   > is really ~1,894 B of the 2,847 measured. Correcting an earlier version of
   > this table that attributed the two `none` rows to "sup channel +
   > mailbox": they are the proc abstract (64 B head + 80 B `BenderProcess` =
   > 144) and the supervisor channel (64 + 176 = 240), averaging 192 B against
   > the 188 B measured. Abstracts land in the `none` row because
   > `bender_abstract_begin` allocates as `BENDER_MEMORY_NONE` and only then
   > calls `bender_gc_settype(..., BENDER_MEMORY_ABSTRACT)`.

   What each allocation is for:

   | allocation | purpose |
   |---|---|
   | `BenderFiber` | the execution context: frames, pc, status, `gc_owner`, the eight `nursery_cur` page cursors, three `heap_lists`, env, and the in-flight resume values. What the scheduler runs and what a scoped minor sweeps. Exists WITHOUT a proc for `ev/go` tasklets, generators and the root fiber. |
   | `BenderProcess` | the actor layer over that fiber: `mbox`/`sup` channels, `mbox_cap`, `fiber`, pid `serial`, `done`, the selective-receive `stash`, the `monitors` list, `exit_reason`, `trap_exit`, `killed`, `isolated_heap`. 80 bytes of the 144. |
   | supervisor channel | carries the `:down` completion event, eagerly so a monitor registered AFTER the proc died still observes it (`suite-proc` test 11). The mailbox is already lazy; this is not. |
   | `BenderFuncDef` | the compiled PROTOTYPE -- bytecode, constants, arity, environment layout, source map, name. Immutable and identical for every proc spawned from the same closure, yet copied per proc. |
   | `BenderFunction` x3 | a closure = funcdef pointer + captured environment. Three: the user's entry closure, the dispatcher's wrapper `(fn [] (setdyn :proc-cross-pid ...) (entry))`, and the marshal copy. |
   | `BenderFuncEnv` x3 | the captured-variable environment for each of those, either a window into a live fiber's stack (`offset > 0`) or a detached values array. |
   | tuples x6 | the pid `[:pid lane serial ctl]`, the spawn control message `[:spawn serial cap entry]`, the `:proc-cross-pid` tuple, and marshal copies. |
   | `BenderTable` | the proc's own dynamic-binding table, created solely because the wrapper calls `(setdyn :proc-cross-pid ...)`. |
   | string | marshal of the funcdef's name/source info. |

   Ranked by removable bytes:

   - **Deduplicate the marshalled funcdef (169 B + part of the 128+96 B of
     function/funcenv copies).** A funcdef is bytecode, constants and source
     info, identical for every proc spawned from the same closure, yet the
     routed spawn copies one per proc. A per-lane cache keyed on the source
     funcdef would copy it once per lane instead of once per process. This is
     the largest single item and also cuts the 1.089 us/spawn marshal that
     batching provably cannot amortise, so it helps BOTH footprint and spawn
     latency -- the only candidate on this list that does.
   - **Lazy supervisor channel (188 B).** Eager only for the late-monitor
     semantic `suite-proc` test 11 pins; the mailbox is already lazy.
   - **The `:proc-cross-pid` dyns table plus its tuple (~100 B).** Set eagerly
     in the dispatcher's wrapper for every proc; computable on demand.
   - **Audit the 6 tuples/proc.** Unknown how many are necessary; the pid
     shape itself is one of them, which is candidate (c) in
     `spawn-parity-design.md` (make a pid an immediate).
   - **Fold fiber into proc (~80 B)** and shrink `BenderFiber` -- the only
     routes below ~400 B, and both are structural.

   Realistic target after the first three: **~900-1,100 B/proc**, i.e. 1.6-1.9x
   BETTER than BEAM rather than 1.67x worse. Gate both axes together as
   before, since sharing and laziness both defer reclamation.

3. **Previously item 2: close the last 1.67x.** The hybrid cursor took
   marginal cost from 5,945 to 2,847 B/proc (landed, `35befbb2`), so a live
   proc is now 1.67x Erlang's 1,709 B rather than 3.5x. What remains is the
   ~920 B of structs (fiber block 408, its separate 128 B stack realloc, proc
   abstract 144, eager sup channel 240) plus whatever slack survives at a
   4 KiB page granularity. The two candidates are making the supervisor
   channel lazy the way the mailbox already is -- it exists only for the
   late-monitor semantic that `suite-proc` test 11 pins -- and folding the
   fiber into the proc, which is worth ~80 B and ~17% of per-proc mark visits
   (see the merge analysis above). Measure both gates together again.

4. **A `1.4x` live-set residual on the co-located chain, i.e. `N^1.11`.** The
   surviving whole-live-set walks (3 at N=200,000, down from 17) are still
   Theta(N) each, so this is the **marking constant**, hypothesis H3:
   `bender_procat_mark` re-walks ~6 blocks per proc. Cutting the *number* of
   walks further is another frequency change like this one; cutting their
   *cost* means not putting N sibling proc handles on one fiber's root path,
   which is the ownership question BEAM answers structurally -- a pid is an
   immediate packing a slot number (`erl_ptab.h:319-326`), not a traced heap
   pointer. Note this residual is now worth less than item 1.

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
