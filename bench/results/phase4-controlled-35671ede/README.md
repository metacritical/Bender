# Controlled Phase 4: initial baseline evidence

This is an initial diagnostic slice, NOT the completed script/AOT/native/PGO matrix or a Go comparison.

## Provenance

- Source revision: `35671edec04292e19a33b97cc3b18942e3e8c676`.
- Clean detached worktree: `/tmp/bender-wave-controlled-35671ede`.
- Build: `make build/bender -j4` with Apple Clang 21.0.0, arm64, macOS 26.6.2; complete compiler log in `build-baseline.log`.
- Built executable SHA256: `146ebc19d1c9ac6c702ef1985c7f90acab74989850d130589717200fcc578515`.
- Workload: unchanged pinned `bench/wave.ben`. N forwarding actors plus one sink; integers traverse real dependent mailbox handoffs.
- Runtime env explicitly sets `BENDER_LANES=0` or `2`, `BENDER_NO_PROFILE=1`, `BENDER_LANE_FASTPOST=0`, and removes `BENDER_PROFILE`. Other environment is inherited; this initial runner is not yet the hermetic matrix runner.
- Source workload prints integer-millisecond phase timings; the external runner checks `procs=N`, `result=N`, successful subprocess status, and records subprocess wall time. Source does not itself assert these results.
- Each trial is a separate subprocess. All samples retained, no selected minima. No overlapping timing runs.
- Initial machine snapshot: 16 GiB RAM, 12 logical cores; `memory_pressure -Q` reported 43% system-wide memory free. This is not a per-process allocation measurement or evidence that the machine was idle.

## Repeated wave-phase observations

Median [min, max] in milliseconds, three fresh subprocesses per arm; all 18 trials returned result=N and exited 0:

| Forwarding actors | Local (`BENDER_LANES=0`) | Two lanes (`BENDER_LANES=2`) |
|---:|---:|---:|
| 1,000 | 95 [95, 101] | 6 [5, 8] |
| 2,000 | 380 [377, 385] | 12 [11, 14] |
| 4,000 | 1,515 [1,511, 1,609] | 24 [22, 29] |

The local wave has roughly quadratic growth over this bounded range. The two-lane arm does not show that growth over these sizes. These are different placements, not equivalent implementations to rank universally. They show why the earlier claim that live-set/GC effects had been ruled out was too broad.

## Profile evidence

`local8000.sample.txt` retains the entire sample report, not just aggregated idle frames. A separate local N=8,000 run returned 8,000, exited 0 and reported spawn=8ms, wave=7,020ms. In the sampled interval, 4,148 of 4,151 main-thread stack samples were under `bender_collect_fiber_minor`, primarily recursive marking. This identifies completion-time collection as a dominant CPU cost for this local workload; it does not establish the safe fix or exclude other costs under other placements. Sampling overlaps most, but not the entire seven-second run.

`lanes2-10000.sample.txt` records a separate complete short process run (spawn=98ms, wave=141ms, result=10,000, exit=0). The report may have few useful samples because the process is short-lived.

A larger two-lane N=100,000 profiling attempt did not return within the runner's imposed wait (2s sample plus up to 30s). **The runner explicitly killed its owned child**, producing return code -9. This is NOT proof of an OS memory kill, a 600-second timeout, or a scaling law. `lanes2-100k-profile.json` records that status. No timing result is accepted for this failed trial.

## Pending

Explain the old 259ms/96ms artifact comparison with matched runtime builds; finish the four-mode matrix with phase markers; profile the cross-lane complete wave and trace dispatcher/encoding costs; independently review any GC fix for cross-owner reachability before changing collection behavior. Do not skip safety by imposing another traversal-depth cutoff.
