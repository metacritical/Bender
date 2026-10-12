# proc/lanes: VM-per-Core Process Parallelism

Implementation notes for the `proc/lanes` feature (landed through milestone 1,
`fc32f7c5`) and the work-stealing design for milestone 2. Audience: maintainers
extending the scheduler, debugging lane behavior, or building the next slices.

## Why lanes

Phase 1's density work made a single VM hold millions of processes with per-proc
heaps and per-owner collection. Lanes extend that horizontally: N resident OS
threads, each running a **full share-nothing VM** (its own heap lists, its own
collector, its own event loop). The properties that fall out:

* **No global GC contention.** Per-proc heaps compose with per-VM collectors —
  one core's collection never pauses another core's procs. The BEAM shape.
* **Density scales per core.** ~3.5 KB/proc held per VM; N lanes ≈ N× capacity.
* **The proc API does not change.** `proc/spawn`, `proc/send`, `proc/recv` work
  identically before and after `lanes-start`; routing is transparent.

## API

```janet
(proc/lanes)                 # active worker count; 0 = single-VM mode (default)
(proc/lanes-start)           # auto: cpu-count - 1 (min 1), round-robin, no affinity
(proc/lanes-start 4)         # explicit worker count
(proc/lanes-start {:count 4
                   :affinity true})   # full form (struct or table accepted)
(proc/lanes-stop)            # release lane 0; see lifecycle below
(proc/spawn-local thunk)     # bypass routing: spawn on the calling VM
(proc/self-pid)              # portable pid tuple [:pid node serial ctl]
```

After `lanes-start`, plain `proc/spawn` routes new processes round-robin across
the worker VMs. `proc/send`, `proc/recv`, and all mailbox semantics are
unchanged from the caller's point of view. Affinity pinning is best-effort:
`pthread_setaffinity_np` on Linux/glibc, `thread_policy_set` on macOS, silent
no-op elsewhere (opt-in — OS scheduling is usually right).

Lane numbering: **0 is the main VM**, workers are 1..N. A fresh process is
single-VM; lanes are opt-in and process-lifetime.

## Architecture

Three layers:

1. **Substrate (pre-existing).** `ev/thread`'s body gives OS threads each
   running a fresh VM: `janet_init()`, unmarshal a function + value, run the
   event loop until that fiber exits. Lanes reuse `janet_go_thread_subr`
   verbatim (exported from ev.c) via `janet_ev_threaded_call`.
2. **Lane registry (C, proc.c).** Thread-local state on the main VM:
   `janet_lane_ctls[N]` (one threaded control channel per worker),
   `janet_lane0_ctl` (the main VM's inbound channel), the round-robin counter,
   and a C-side serial registry. All fields are `JANET_THREAD_LOCAL`.
3. **Residency (boot.janet).** Each lane runs `proc/lane-worker` — a resident
   dispatcher looping on its control channel:
   * `[:spawn serial cap entry]` → VM-local `proc/spawn`, register serial→proc
   * `[:deliver serial payload]` → lookup serial, `proc/send` into local mbox
   * `[:proc-fwd serial msg]` → same as deliver (proc/send's remote convention)
   * `[:stop]` → exit the dispatcher (lane 0 only, milestone 1)

### Launch protocol

`lanes-start` mirrors `ev/thread`'s marshaled-buffer format exactly — the
subr's unmarshaler is order-sensitive:

1. the abstract registry table (so threaded channels resolve by reference),
2. the RAW cfunction registry copy (`uint32` count + bytes — not marshaled),
3. the dispatcher function (`proc/lane-worker`, resolved from the boot fiber
   env where boot.janet `setdyn`'d it),
4. the launch value `[ctl lane-id affinity]`.

The dispatcher takes ONE argument (the subr enforces arity 0-1) and
destructures the tuple.

### Cross-lane process identity

A `proc` abstract refuses marshal, so cross-lane handles are **pid tuples**
`[:pid node serial ctl]`:

* Worker spawns store the cross-pid into the child's env (`:proc-cross-pid`)
  so `proc/self-pid` returns a routable handle from anywhere.
* Main-VM procs synthesize it lazily in `proc/self-pid` (node 1 = "a lane
  ctl", the lane-0 channel) and register in the C-side registry — deliveries
  to main-VM procs consult `(proc/lane-lookup serial)` as a fallback.
* `proc/send` on a nonzero-node tuple pushes `[:proc-fwd serial msg]` over
  the carried channel (the pre-existing remote-node convention). The receiving
  lane's dispatcher resolves the serial and gives to the local mbox.
* The tuple's channel marshals by reference (threaded abstracts), so pids
  travel inside closures and messages freely.

Serial space: one counter per spawner VM. Routed spawns allocate on the main
VM; lane-internal spawns allocate on the lane's own counter. The dispatcher's
registry keys on the SPAWNER's serial, so no collision arises.

### Routing and the TLS trap

Routing lives in `janet_proc_spawn_routed` (called only from `proc/spawn`);
`proc/spawn-local` and lane-internal spawns call the impl directly. The single
most important implementation detail: **the amalgam's file statics are
process-global.** Without `JANET_THREAD_LOCAL`, worker VMs saw the main VM's
`lanes_count` and re-routed every "local" spawn back to a lane — kids
ping-ponged between lanes forever. Every lane-state static must be TLS.

## Lifecycle (milestone 1)

* `lanes-stop` pushes `[:stop]` to **lane 0 only**, unblocking the main loop.
  Worker threads stay parked on their ctls (detached, share-nothing) and are
  reaped at process exit. Suites should end with `(os/exit 0)`.
* Deliberately NOT done yet: pushing `:stop` to workers races their
  `janet_deinit()` against the main VM's teardown (libmalloc abort observed).
  Graceful worker shutdown needs a pool-style join — milestone 2.

## Work-stealing (milestone 2 design)

Milestone 1 places spawns **round-robin** — optimal for uniform fan-out, but
blind to skew: a lane that draws long-running procs falls behind while others
idle. The design below is recorded so the next slice can land without
re-derivation.

### Why not steal procs

Procs are anchored: their fibers run on their home VM's loop, their heaps are
owner-scoped, their mailboxes are home-pinned isolated channels. **Migration is
out** (it would drag a heap across VMs). Work-stealing therefore steals *task
submissions*, not resident procs — the Erlang-friendly shape.

### Design: idle-lane pull from a global overflow queue

1. **Overflow queue.** When a routed spawn would land on a lane whose ctl is
   at a high-water mark (say 3/4 capacity), push instead to a global MPSC
   overflow deque (a threaded channel is adequate for milestone 2a; a lock-free
   deque later).
2. **Idle pull.** A dispatcher whose ctl is EMPTY (its take would park) first
   drains up to K tasks from the overflow queue before parking. This converts
   spare capacity into throughput without any migration: the *entry closure*
   executes wherever capacity exists.
3. **Locality tiebreak.** Steals prefer tasks whose pid handles reference the
   stealing lane's existing procs (cheap check: compare carried ctls), keeping
   hot-message traffic same-VM.
4. **Placement knob.** `{:policy :round-robin | :least-loaded | :steal}` —
   `:least-loaded` tracks per-lane depth counters (fetch-add on push, on take);
   `:steal` adds the overflow-pull. Round-robin remains the default for
   predictability.
5. **Batching.** Pulls take K=4..8 tasks at once to amortize channel traffic,
   half given back if the stealer's own queue floods first.

### Expected behavior

* Uniform fan-out (the agents benchmark): round-robin ≈ steal. No regression.
* Skewed workloads: steal recovers most of the idle-core loss. The 1-vs-N
  agents demo with a deliberately slowed actor population is the gate.
* Comms-heavy skew: locality tiebreak keeps same-VM message paths at
  in-VM cost (786k msgs/s measured AOT, 692k interpreted at 100k actors);
  stolen tasks pay marshal only for cross-lane sends they actually make.

### Milestone 2 remainder (in order)

1. Pool-style join: workers take a join token; `lanes-stop` (or a new
   `proc/lanes-join`) delivers `:stop` to all lanes and joins the pthreads —
   requires the subr path to expose thread handles or a completion handshake
   per lane.
2. Cross-lane monitors/kill/exit-reasons: `:down` messages marshal over lane
   ctls keyed by the watched pid tuple; kill forwards a `:kill` op that the
   home lane applies locally (signal into the local fiber).
3. Work-stealing per the design above.
4. The 1M-processes-across-N-lanes demo + scaling measurements.

## Messaging speed (measured, `bench/lanes-comms.janet`)

| Shape | Throughput | Per-send |
|---|---|---|
| same-lane ping-pong (2 procs, 1 VM) | ~940-980k msgs/s | ~1.05 us |
| cross-lane ping-pong (2 lanes, marshaled hops) | ~156-234k msgs/s | ~4.4-6.4 us |
| 20k fan-in across 4 lanes -> main-VM collector | ~101k msgs/s end-to-end | incl. spawns (0.197 s) |

The cross-lane tax (~4-6x) is the marshal-copy + thread handoff + dispatcher
hop; it is the price of share-nothing and matches the design target of
keeping hot-message traffic same-VM.

**Known bug (open, pre-existing, NOT lanes-specific):** bulk fan-in of
>=~10-20k single-shot senders to ONE collector proc loses a wakeup on some
runs — the collector stops receiving mid-stream (reproduces with all senders
on the SAME VM via proc/spawn-local, with collector mbox caps 64 and 4096;
100-10000 senders complete reliably, 20000 is flaky). Same-VM, so the lane
wake path is exonerated; suspicion falls on the isolated-mbox pending
registration/reclaim under bulk completion minors. Record: reproduce with
`/tmp/fo_cap.janet` shape (20k spawn-local senders, one collector).

## Evidence

* `test/suite-lanes.janet`: 2-lane and 4-lane round trips (kids compute i²
  across lanes, forward to a lane-0 collector, main verifies the sum), auto
  count, affinity option, spawn-local, single-VM unaffected.
* Full battery at `fc32f7c5`: make test 52/52, suite-proc 5/5, all nine
  sibling suites green.
* Single-lane density baselines (apply per lane): chain-100k 2.36 s /
  23.6 µs per hop; 10k spawn+park 3.0 µs/proc; ~3.5 KB/proc resident.
