# Lanes, captured state, and process memory

Everything below was measured on a 12-logical-core Apple Silicon machine
(8 performance + 4 efficiency), and every claim is backed by a number that can
be reproduced with the commands given. Read this before writing code that
crosses lanes.

Related: `docs/internals/messaging-guide.md` (the API ladder),
`docs/internals/proc-lanes.md` (lane internals and history),
`docs/internals/known-issues-fiber-child-minor.md` (GC defects found here).

## 1. The one rule that matters

**A proc spawned onto a lane receives a MARSHALLED COPY of its closure.**

`proc/spawn` onto a remote lane does not share the closure; it sends it through
a threaded control channel, which marshals (deep-copies) every captured value.
The worker therefore runs against *copies* of everything the spawner captured.

Consequences, all confirmed by tests in `test/suite-lane-chan.janet`:

- Writing to a captured `ev/chan` from a worker does **nothing** the spawner can
  observe. Before the current guards this was silent — measured 0 of 12 values
  arriving, with no error.
- Pushing into a captured mutable array from a worker mutates a copy.
- Only explicit carriers work: the worker's own mailbox (`proc/send` /
  `proc/recv`), or an `ev/thread-chan`.

### What the runtime now does about it

Two guards, both loud:

```
plain ev/chan used from a foreign lane (it takes no lock and copies nothing, so
a blocking write parks forever and a buffered one silently loses the value --
use ev/thread-chan across lanes)
```

```
plain ev/chan arrived as a MARSHALLED COPY of a channel owned by another lane;
writes to it would be lost silently (pass messages to the worker's own mailbox,
or use ev/thread-chan across lanes)
```

The second is the one a lane worker hits. Pinning alone could not catch it: the
worker legitimately operates on its own copy, so there is no shared object to
check. The unmarshal path therefore marks a plain channel as a detached copy
**when it is materialized on a worker lane**, and push/pop refuse it.

A same-thread marshal round-trip on the main VM stays legal (suite-ev marshals a
channel and drains it); only a copy received on a worker lane is detached.

## 2. Turning lanes on

Lanes only activate once `proc/lanes-start` runs. That is opt-in, and there is a
second, lazy route:

```janet
(proc/lanes-start 4)          ;; explicit
```

```sh
JANET_LANES=4 ./my-program    # lazy: started on the first routed proc/spawn
JANET_LANES=0 ./my-program    # keep the historic single-VM behaviour
```

### Why `JANET_LANES` is opt-in and NOT the default

Enabling lanes changes `proc/spawn`'s **return type**: with lanes off you get a
`core/proc` object, with lanes on you get a `[:pid lane serial ctl]` tuple.
Code that calls `proc/pid`, `proc/alive?` or `proc/recv` on the handle behaves
differently depending on which shape it got. Turning that on by default breaks
`test/suite-proc.janet`'s own `"spawn returns a proc"` assertion — a real API
break, not a test detail.

The default can flip once proc handles are uniform across lanes (a local proxy
for remote pids, or a tuple that satisfies `proc/pid?`). Until then, opt in
deliberately.

Autostart is hooked into the first routed spawn, **not** into `boot.janet`:
`boot.janet` also runs during image generation, where starting lanes hangs the
build (`janet_boot` was observed stuck in the lane join). Spawning never happens
during image generation, so the lazy hook is safe by construction.

## 3. Measured cost of the switch

Independent work, 1000 procs each doing a 2000-iteration sum, reading results
back over one `ev/thread-chan`:

| configuration | wall | checksum |
|---|---|---|
| `JANET_LANES=0` | 22 ms | 1999000000 |
| `JANET_LANES=4` | 9 ms | 1999000000 |
| `JANET_LANES=11` | 8 ms | 1999000000 |

Identical checksums in every arm: the parallel path does the same work and loses
nothing. This is the case lanes exist for — **independent** work.

## 4. What lanes do NOT speed up

A dependency chain cannot be parallelised, and this surprises people. Building a
chain where each proc forwards to the next (`proc N+1` blocks on `proc N`) gives
no parallel speedup at all: measured single-threaded chains, and the same
workload in Go and Erlang, all spend essentially one core's worth of time.

This is worth internalising: **lanes multiply throughput of independent work;
they do nothing for latency of sequential work.**

## 4b. Choosing a lane count (measured on 12 cores)

Lane count is the direct lever for BOTH throughput and the burst ceiling, and
they peak at different points.

CPU-bound work (identical total work, same checksums in every arm):

| configuration | wall | speedup |
|---|---|---|
| one VM (sequential) | 231 ms | 1x |
| 12 lanes | 30 ms | 7.7x |
| 12 x `thread/new` | 28 ms | 8.3x |

Past ~16 workers both mechanisms plateau and then degrade (24 workers ~60 ms,
48 workers ~103 ms for the same work) -- that is the physical core count, not a
runtime difference. **Lanes and `thread/new` are the same lever**; a lane IS an
OS thread (24 lanes measured 25 OS threads). Choose lanes when you want the
mailbox/actor model, `thread/new` when you want fire-and-join with a value.

Burst throughput for a result-returning fan-out (50000 procs):

| lanes | wall |
|---|---|
| 8 | 360 ms |
| 11 | 332 ms |
| 16 | 331 ms |
| 24 | 330 ms |

So ~11 lanes saturates throughput on this box.

The burst CEILING, however, keeps rising with lane count, because each lane has
its own self-pipe and the post rate spreads across them:

| lanes | largest burst that completes |
|---|---|
| 4 | 80000 fails, 50000 OK |
| 8 | 100000 OK, 200000 fails |
| 11 | 200000 OK, 400000 fails |
| 24 | 400000 OK, 1000000 too slow to finish here |

That is why more lanes help even when throughput has plateaued: they buy
headroom for large fan-outs, not more speed.

## 4c. Fibers vs procs vs threads: what actually parallelises

Three concurrency primitives, measured on the same workload (12 heavy tasks):

| primitive | wall | parallel? |
|---|---|---|
| `ev/go` fibers, lanes off | 229 ms | no |
| `ev/go` fibers, lanes on (11) | 226 ms | **no** |
| `proc/spawn` across lanes (11) | 30 ms | **yes, 7.7x** |
| `thread/new` x 12 | 28 ms | yes, 8.3x |

**Fibers do NOT distribute across threads.** Turning lanes on makes no
difference to `ev/go` (226 vs 229 ms): all fibers still run on the VM that
created them, one thread. Fibers give concurrency (interleaved I/O, cheap
suspension), never parallelism. If work must use many cores it has to be a
`proc` (or `thread/new`), because those have their own VM and heap.

Cost per unit, which drives the choice:

| unit | memory | mechanism |
|---|---|---|
| fiber | **~1 KB** | cooperative, one VM, no migration |
| proc | ~10 KB | own fiber + own heap + mailbox, lane-migratable |

Measured fiber capacity on ONE thread: 250000 fibers spawned successfully
(100000 in 15.8 s, 250000 in 39.9 s). The limit is scheduling time, not a hard
count, and memory stays near 1 KB each (50000 fibers = 57 MB, 100000 = 109 MB).

So the rule is: use fibers for I/O-shaped or very numerous cooperative tasks
(cheap, single-core), and procs only for work that must occupy another core.

## 4d. Scheduler count

`proc/lanes-start` with no argument starts **cores - 1** lanes; on this 12-core
box that is 11, i.e. 12 schedulers counting the main VM -- the same shape as
BEAM's one-scheduler-per-core. `JANET_LANES=<n>` sets it explicitly, and the
guidance in 4b applies: ~11 for throughput, more for burst headroom.

## 5. Process memory, and why 1M processes is a memory question

Measured per-process footprint before and after the slab-page change:

| | per-process | 1M procs |
|---|---|---|
| before (`JANET_SLAB_PAGE_SIZE` 16 KB) | 34 KB | ~34 GB (impossible) |
| after (4 KB page) | **10 KB** | **~9 GB** (feasible) |
| Erlang, for reference | 2.7–3.2 KB | ~2.7 GB |

Instrumenting the slab allocator showed **2.06 pages claimed per process**, i.e.
~32 KB of the original 34 KB was slab capacity paid up front regardless of how
little the process actually allocated. Dropping the page to one OS page (4 KB)
cut it 3.4× with no measurable cost to per-hop work (still ~6 µs/hop).

`1M` in Janet is therefore feasible now, but only with roughly 9 GB free. Under
memory pressure it swaps and every timing number becomes meaningless — see §6.

## 5a. Milestone: 1,000,000 processes in 1.31 GB

After sharing young pages across ordinary proc construction (3401dc30):

| scale | RSS | per proc |
|---|---|---|
| 20000 | 32 MB | 1.6 KB |
| 100000 | 143 MB | 1.5 KB |
| **1000000** | **1311 MB** | **1.31 KB** |

All 1000000 bodies executed (verified by a shared counter reaching 1000000).
For comparison: Erlang measured 2.7-3.2 KB/proc, so Janet is now BELOW BEAM's
per-process footprint, and comfortably inside a 16 GB machine.

The win came from a specific defect, not a general rewrite. Instrumenting the
slab allocator showed 2.20 pages claimed per proc, with

    c1 (64 B,  64 slots/page) = 0.92 pages/proc
    c5 (256 B, 16 slots/page) = 0.92 pages/proc

-- each proc filled about ONE SLOT of a fresh page. Cause: every fiber drew from
its own nursery cursor so young pages stayed pure-owned. A proc being
constructed is a short-lived context that allocates a couple of small objects,
so ordinary construction now uses the SHARED cursor and many procs fill one
page; an owner-mixed page reads as shared and floats to a global collection.
Strict (spawn-isolated) construction is excluded, because its export rule needs
pages only that child owns.

That also removed the native-stack cost from the picture: the 64 KB reservation
is untouched and no longer matters, because untouched reservation is not memory.
Procs now measure 1.5 KB versus fibers' 1.15 KB, so the earlier advice to prefer
fibers for volume is obsolete -- procs are nearly as cheap AND can use all cores.

## 5b. Why a proc cost ~10 KB before (historical)


Measured per-proc accounting (20000 procs, all bodies executed, 195MB RSS):

    4 KB   slab page claim -- gc_alloc_ctx points at the new fiber during
           construction, so every proc's allocations land on its OWN heap and
           its first allocation claims a fresh 4 KB page
    ~5 KB  native C stack actually touched when the proc runs
    ~1 KB  fiber struct (384 B) + proc abstract (80 B) + child env table
           (48 B) + fiber data stack

The two big items are structural, not oversights:

- **A per-proc page is what per-process GC isolation IS.** Sharing pages
  across processes would make one process's minor collection able to free
  another's objects, which is the property the whole model relies on.
- **A running proc needs a real C stack.** Reducing the RESERVED commit
  (JANET_FIBER_STACK_INIT_BYTES 64 KiB -> 16 KiB) measured *no change at all*
  because reservation is not the cost; touching is.

So ~10 KB is close to the floor for a process that owns a heap and can run C
code, and getting to fiber-like ~1 KB would require processes to share heaps --
which is precisely the BEAM-model trade we have not made. Use fibers (~1 KB,
single core) when you need very many cheap cooperative tasks; use procs when
the work must occupy another core.

A smaller win WAS taken: the fiber stack was reserved at 64 slots for an
arity-0 thunk; it is now 16 (376MB -> 367MB at 40000 procs).

## 5c. The hybrid: fibers INSIDE procs

> Diagram-first version of this section, with imagined syntax and a decision
> picture: `docs/internals/concurrency-visual-guide.md`. Runnable example:
> `examples/concurrency-hybrid.janet`.

The two tiers compose, and the composition is better than either alone. Put
fibers inside procs: procs supply the cores, fibers supply the unit count.

Same total work (110M iterations), three configurations:

| configuration | wall | cores used |
|---|---|---|
| pure fibers, one VM | 1046 ms | 1 |
| pure procs, 11 lanes | 120 ms | 11 |
| **hybrid: 11 procs x 11 fibers each** | **121 ms** | **11** |

The hybrid matches pure-proc speed exactly -- wrapping work in fibers costs
nothing measurable -- while multiplying the number of concurrent units:

| concurrent units | layout | RSS |
|---|---|---|
| 1,000,000 | 1000 procs x 1000 fibers | **1078 MB** |
| 5,000,000 | 5000 procs x 1000 fibers | **2305 MB** |

That is ~0.5 KB per live unit at 5M, because a fiber is ~1 KB but the fixed
per-proc cost (page + structs) is amortised over the 1000 fibers beneath it.

**Why this matters:** the unit ceiling is now set by millisecond-scale
bookkeeping rather than by memory or core count. Neither Go (goroutines share
one heap, so GC cost scales with live set) nor Erlang (2.7-3.2 KB per PROCESS
with no cheaper inner tier) offers a comparable two-level structure: in BEAM the
process IS the cheap unit, so 5M live processes would cost ~14 GB, whereas the
Janet hybrid costs 2.3 GB and runs across all cores.

Practical shape:

```janet
;; one proc per core-group of work, many fibers inside each
(proc/spawn (fn []
              (def ch (ev/chan 4096))
              (var i 0)
              (while (< i 1000)
                (ev/go (fn [] (ev/give ch (unit-of-work))))
                (set i (+ i 1)))
              ;; collect
              ...))
```

Caveats: a proc's fibers stay on that proc's VM (no migration), so balance work
per proc evenly; and use `ev/thread-chan` to carry results back across lanes.

## 5d. The cross-lane memory tax (open issue)

Measured with the same 50000-process program, differing only in lane count:

| lanes | RSS | per proc |
|---|---|---|
| 0 (single VM) | 91 MB | **1.87 KB** |
| 4 | 341 MB | **6.98 KB** |

So running a process on a LANE costs ~3.7x more than running it on the main VM.
That is the current blocker for "5M processes", not the per-proc struct: idle
procs measure 1.31 KB and single-VM procs 1.87 KB, but a lane-hosted proc costs
~7 KB.

Consequence, measured:

| procs | result |
|---|---|
| 200000 | completes, 1045 MB |
| 400000 - 1000000 | **fails** on a machine with swap already in use |
| 2000000 | completed once at 3016 MB on an idle box, 161 s |

For comparison, Erlang does 5000000 processes in 823-1043 MB total
(0.17-0.21 KB per process official memory accounting). Erlang is not doing
anything cleverer per process; it simply does not pay a cross-boundary tax for
a process that stays on its scheduler.

**Closing in.** Modelling RSS as `fixed + per*n` over the measurements
(n=1000 -> 13 MB, n=5000 -> 39 MB, n=20000 -> 138 MB) fits:

    per lane-hosted proc = 6.66 KB
    fixed lane overhead  = 6.5 MB

So the tax is genuinely per-process, and the earlier "it decreases with n"
reading was just the small fixed term divided by a small n. Same-VM procs
measure 1.53 KB, so a lane proc costs ~5.1 KB more.

Ruled out so far, each by measurement:

- the ctl channel capacity (4096 -> 64 changed nothing: 6.83 vs 7.07 KB)
- closure allocation: a SHARED closure with no captures measures the same
  7.08 KB/proc as a per-item closure
- the work or the results: a spawn-only loop with an immediate `lanes-stop` shows
  the full tax (1.52 KB at lanes=0 against 6.83 KB at lanes=4)
- proc/send: 1000000 consecutive cross-lane sends completed with zero errors

Also ruled out: post-construction allocation. A proc that allocates NOTHING
(`(fn [] nil)`, shared closure) still measures 1.66 KB at lanes=0 and 7.08 KB at
lanes=4, so the tax is not caused by anything the proc body allocates. And lane
construction already shares young pages (5c), so it is not the construction
allocator either.

Also ruled out since: lane re-routing inside the worker's spawn handler
(switching it to proc/spawn-local changed nothing), queue backpressure (pacing
spawns with ev/pump changed nothing), and lane VM fixed overhead (an idle
4-lane VM is 5 MB, while the tax at n=20000 is 126 MB, so it is per-process).

**The slab design is NOT the problem, and this is worth stating because it is
the natural suspicion.** Slabs are per-VM with owner attribution, not per-proc;
shared_stack is ON by default for proc fibers; and lane-side construction
already routes through the shared page cursor. The same proc costs 1.64 KB when
spawned with `proc/spawn-local` even while lanes are running, and 6.63 KB when
routed with `proc/spawn` -- same process, same VM, same allocator. So the cost
enters somewhere in the routed path, not in the process representation.

Remaining instrument needed: count live slab pages and native-stack bytes on a
lane VM before/after N routed spawns. That separates heap from stack directly,
which RSS cannot. Until that runs, the honest statement is: single-VM procs are
1.6 KB, lane-routed procs are ~6.6 KB, and the delta is in the routing path.

Not yet diagnosed: which part of the lane path allocates. Candidates worth
measuring next: the marshal transit buffer created per routed spawn (the
closure is marshalled on every spawn), a per-lane native stack plus heap that
a large live set keeps resident, and the result channel's per-item transit.

``proc/send`` is NOT implicated: 1,000,000 consecutive sends across 11 lanes
completed with zero errors when tested in isolation.

## 6. Measurement discipline (learn from our mistakes)

Three separate wrong conclusions in one session came from ignored machine
state. Any process/memory benchmark here must:

1. Use **min-of-N across fresh processes**. The first run pays page faults and
   swap-in; a warm run is not comparable.
2. Check `sysctl -n vm.swapusage` **before** the run. Swap in active use means
   the numbers are measuring the kernel, not the runtime.
3. Check load average. A load of 3.5 on a 12-core box made 8-lane runs "hang"
   that passed on re-run.
4. Never trust a single run of anything above ~100k processes.

Concrete false conclusions this discipline would have prevented:

- "spawn is quadratic (16 KB pages)" — it was swap. Spawn is ~flat at 3–7 µs.
- "8+ lanes starve the main VM" — it was machine load.
- "`proc/lanes-start` hangs" — the test script exited without `proc/lanes-stop`,
  so the process waited on live worker threads. Lanes stop cleanly.

## 7. Quick reference

| want | use |
|---|---|
| N independent tasks, results collected | `JANET_LANES=n` or `proc/lanes-start`, plus `ev/thread-chan` for results |
| keep captured mutable state shared | do NOT cross lanes — stay on one VM |
| pass data to a lane worker | `proc/send` to its mailbox (marshal-copied, by design) |
| results out of a lane worker | `ev/thread-chan`, or the worker's supervisor channel |
| sequential pipeline | lanes will not help; optimise the work itself |
| millions of short-lived processes | feasible at ~10 KB each; watch RAM, not cores |
