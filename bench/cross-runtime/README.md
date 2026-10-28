# Cross-runtime concurrency benchmarks

Compares Janet against Erlang and Go on the same two questions. Every script
emits one machine-readable line, and `run.sh` turns them into one table.

```
./bench/cross-runtime/run.sh [N]        # default N=1000000
make bench                              # same, plus Janet's micro-benchmarks
make bench-janet                        # Janet only, no Erlang/Go needed
```

## 1. Trampoline wave (`wave.janet`, `wave.erl`, `wave.go`)

Build a chain of N processes. Process i receives a value, adds 1, forwards it to
process i+1. The last one reports the final value, which **must equal N**.

This is a **dependency chain**, not parallel work: every hop waits for the
previous one. It measures spawn cost plus per-hop message latency. **Cores
cannot speed it up**, so a low wave time is not "more parallel".

The runner defaults to 11 lanes; override with `WAVE_LANES`. Earlier small
single-VM runs showed quadratic scaling, while routed runs were faster. These
observations do not establish a GC cause or prove linear scaling to 1M.
The token is serial, but placement can still change scheduling overhead.

Janet emits deterministic `procs/result` on stdout and phase timings on stderr,
so PGO parity compares results rather than nondeterministic clock readings.
See `pgo-results.md` for the three-training-run release experiment and limits.

### Phase-labelled variant (`wave-phased.janet`)

Same chain, same actor body, same handoffs and the same deterministic
`procs=N result=N` on stdout as `wave.janet`; it additionally writes six
`phase=` markers to stderr (`spawn-start/end`, `wave-start/end`,
`shutdown-start/end`) with monotonic microsecond timestamps and timings at
`%.3f` ms instead of `%.0f`. Use it when the split between spawn, wave and
shutdown matters; `wave.janet` remains the parity reference.

N is a required integer `>= 1` with no upper cap beyond int32; invalid N exits 2
with nothing on stdout. Malformed input (`5x`, `10abc`, `1e`, `0x`, leading
space) is rejected the same way, because `scan-number` returns nil for all of
them; `1.5e2` is accepted as 150.

**Clock note.** Timestamps come from `(os/clock :monotonic :tuple)`, which
returns `[tv_sec tv_nsec]` (see the `:tuple` branch of `os_clock` in
`src/core/os.c`, which wraps `tv_sec`/`tv_nsec` directly). Microseconds are
`sec*1000000 + ns/1000`; scaling seconds as milliseconds instead makes the
value jump **backward** by ~999000 us at every second boundary and every phase
duration goes negative. That bug existed here and was fixed; `now-us` also
detects the field scale rather than assuming it. A cheap guard (`clock
regression`) exits 3 if two adjacent reads ever decrease.

**Inter-phase gap, disclosed not hidden.** `wave-start` is sampled after the
`spawn-end` marker is printed, so `wave_ms` excludes that marker's own write,
while `spawn_ms` encloses the `spawn-start` write. The difference is reported
as `label_lag_us` and is not folded into either interval; `total_ms` spans
`spawn_start`..`wave_end`, so `spawn_ms + wave_ms + label_lag_us/1000`
reconciles with it.

**Lane placement is not observable from script.** The runtime exposes no
per-actor lane identity. `proc/self-pid` returns `[:pid lane serial ctl]`, but
that lane field is hardcoded to the literal `1` in `src/core/proc.c`
(`cfun_proc_self_pid`) -- it is a shape marker, not the executing worker --
`proc/lanes` reports only how many worker VMs exist, and `proc/lanes-stats`
reports overflow-queue push/pull counts. So `wave-phased.janet` prints
`placement=unknown` and the configured worker count, and never infers a lane
from chain index or lane count. Do not read a placement claim into its output.
Adding real per-actor placement would need a new runtime metric; sampling per
actor inside the chain would add work to the timed path.

## 2. Live-process memory (`memory.janet`, `memory.erl`)

How much memory does one **resident** process cost?

This exists because an earlier comparison was invalid: both sides spawned
processes that **exited immediately**, so neither ever held N processes at once.
A process that sends its answer and dies is a queue entry, not a resident
process. Here every process blocks on `receive` until the whole set is up, and
only then is memory measured.

`memory.erl` also reports `base_mb`, the VM's own footprint before spawning, and
the table shows the **marginal** cost -- without subtracting the baseline, small
N is dominated by the runtime itself.

## Reading the numbers

- **Timings on a loaded machine are noise.** The script prints swap and load
  average first; if swap is in heavy use, the numbers are measuring the kernel.
- **A `WRONG` verdict means the chain dropped or duplicated a hop.** That is a
  correctness failure, not a performance one -- investigate before comparing
  times.
- Janet's `wave.janet` and `memory.janet` use `proc/spawn` / `proc/spawn-local`
  per the note in each file; the memory harness deliberately uses
  `spawn-local` so it measures process cost rather than the cross-lane routing
  path (which has a known ~3x tax, see
  `docs/internals/lanes-and-captured-state.md`).
