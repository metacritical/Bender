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
