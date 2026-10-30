# Wave PGO/AOT experiment

Source baseline: d120917d plus the wave output correction in this commit.
Bounded workload: N=1000, BENDER_LANES=0. This is not the requested full 1M
comparison; earlier 1M Bender runs did not produce completion evidence.

## Reproduce

From repository root (requires clang and the normal build prerequisites):

```sh
make -j4 all
mkdir -p build/wave-pgo-evidence
export XDG_CACHE_HOME="$PWD/build/wave-pgo-evidence/cache"
export BENDER_LANES=0
for i in 1 2 3; do
  ./build/bender --aot bench/cross-runtime/wave.ben --native \
    --pgo '1000' -o build/wave-pgo-evidence/wave \
    > "build/wave-pgo-evidence/train-$i.log" 2>&1 || exit 1
done
./build/bender --aot bench/cross-runtime/wave.ben --native --release \
  -o build/wave-pgo-evidence/wave \
  > build/wave-pgo-evidence/release.log 2>&1 || exit 1
for i in 1 2 3; do
  BENDER_NO_PROFILE=1 ./build/bender bench/cross-runtime/wave.ben 1000 \
    > "build/wave-pgo-evidence/baseline-$i.out" \
    2> "build/wave-pgo-evidence/baseline-$i.time" || exit 1
  BENDER_NO_PROFILE=1 ./build/wave-pgo-evidence/wave 1000 \
    > "build/wave-pgo-evidence/release-$i.out" \
    2> "build/wave-pgo-evidence/release-$i.time" || exit 1
  cmp "build/wave-pgo-evidence/baseline-$i.out" \
      "build/wave-pgo-evidence/release-$i.out" || exit 1
done
```

An initial attempted build collected a profile but failed linking because
`build/libbender.a` was missing. `make all` resolved that prerequisite. All three
subsequent training builds and the final release exited 0 and reported
`pgo parity ok`. Each training profile had 31 rows; later training runs merged
cached evidence. Release verified one cached workload. All four compilations
reported **native-lib 0 kernels**: this experiment does not demonstrate native
execution of the actor bodies. This is Bender type-profile PGO, not clang PGO.

## Results (milliseconds, sequential paired runs)

| run | script spawn | script wave | script total | release spawn | release wave | release total |
|---|---:|---:|---:|---:|---:|---:|
| 1 | 1 | 103 | 104 | 1 | 264 | 265 |
| 2 | 1 | 95 | 96 | 1 | 259 | 259 |
| 3 | 1 | 93 | 94 | 1 | 257 | 258 |

Every stdout record was `procs=1000 result=1000`, byte-equal between paired
runs. Median total: script 96 ms, release 259 ms (~2.70x slower). Timings are
rounded independently, so phase sums can differ from total by 1 ms.

This compares script execution with the final linked AOT image, not a controlled
PGO-on/off pair of identical runtime builds. The pre-existing bender and the
newly built static runtime may differ in configuration. Zero emitted native
kernels and the runtime's mailbox/scheduler/GC work limit what this compilation
can improve. Do not attribute the slowdown to PGO alone or claim a GC root cause
without profiling. Earlier Erlang/Go runs did return N, but their old times are
not paired with this experiment and should not be used for a new speedup ratio.

## Evidence caveats still open

- A low RSS reading from `pgrep ... | head -1` may identify a timeout/shell
  wrapper; it does not prove the actual VM was swapped out.
- The prior 5M Bender memory run printed no successful count marker. Its peak
  RSS alone does not prove 5M live processes or superiority over BEAM.
- The earlier ready-channel script discarded readiness messages without
  counting them, then waited for N more: that harness can hang independently
  of scheduler behavior. Likewise observing zero bodies before a spawned
  producer yields is not proof that a lane event loop never pumps.
- Current cross-runtime memory rows still mix Bender peak RSS with Erlang
  marginal internal accounting. They are not apples-to-apples live heap costs.
- Full 1M completion, matched memory accounting, and runtime profiling remain
  outstanding. No heavy 1M or 5M retries were made in this experiment.
