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
