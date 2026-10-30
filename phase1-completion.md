# Phase 1 Completion: bugs, milestone 2, dual-target tests

## Objective
Finish the proc/lanes phase end-to-end: two open bugs, the AOT lanes
measurement, milestone 2 features, beautiful feature-indication tests for
both interpreter and AOT builds, battery-green throughout, feature commits
per landed slice.

## Workstreams (execution order)

### 1. BUG: bulk fan-in lost wakeup (single-VM, pre-existing)
- Repro: 20k spawn-local senders -> 1 collector; flaky at 20000, clean <= 10000,
  both mbox caps 64/4096; stalls mid-stream (observed after collector got 5000).
- FIRST: re-read FULL stderr of a stalled run (a dispatcher error/stacktrace
  may have been missed by greps).
- Suspects: lane0_ctl blocked-writer wake (threaded path, same-VM writer),
  collector mbox blocked-writer (isolated path), pending-node reclaim under
  bulk completion minors, dispatcher fiber death.
- Fix + regression test (suite-proc or suite-lanes fan-in section).

### 2. BUG: per-lane density threshold
- 4-lane 1M (~225k/lane) crashes above ~900k total: bender_hash on freed
  abstract in worker run_vm. 8-lane (125k/lane) clean.
- Audit cross-thread refs that survive GC: pid tuples in child envs, C-side
  registries, marshaled channel refs; find the unrooted/dangling one.
- Repro script + fix + regression (scaled suite with skip-if-low-RAM).

### 3. AOT lanes measurement
- Build AOT binary with lanes code; rerun 1M-across-N + comms matrix;
  record numbers in docs/internals/proc-lanes.md + aot-notes.
- Expected: same capacity, faster spawn/ticks.

### 4. Milestone 2 features (each: implement -> test -> feature commit)
- 4a. Pool-style join: workers take a join token; lanes-stop (or lanes-join)
  delivers :stop to ALL lanes and joins pthreads cleanly (fix the deinit race
  documented in proc-lanes.md).
- 4b. Cross-lane monitors/kill/exit-reasons: :down over lane ctls keyed by
  pid tuple; kill forwards :kill op applied locally by the home lane.
- 4c. Work-stealing per the recorded design: overflow queue + idle-lane pull,
  :policy knob, locality tiebreak, batched steals.
- 4d. 1-vs-N-lane agents demo benchmark + numbers.

### 5. Tests (throughout, feature-indication style)
- Extend suite-lanes.ben per feature (join, monitors, kill, steal policy,
  fan-in regression, density regression skip-if-low-RAM).
- Every suite runs under BOTH bender (interpreter) and the AOT binary;
  make test battery green; feature commits per slice.

## Gates
- make test 52/52 + suite-proc 5/5 + nine siblings + suite-lanes (extended)
  under interpreter AND AOT.
- Bugs fixed with regression tests.
- Doc updated (messaging matrix, AOT numbers, milestone-2 semantics).
- One feature commit per landed slice, conventional messages.