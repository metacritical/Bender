# Residual marking and phase-labelled larger routed waves

## Provenance and controls

Post-fix clean sourcea69c0984; non-instrumented -O2 Clang baseline shell `/tmp/janet-wave-clang-sdk-a69c0984/baseline/bender` SHAdcd400a616b506f236efffb42b83eec02a4750acdc6980e8f8c2f2065f939a67. Uses the existing actor chain with phase-labelled driver `wave-phased-snapshot.janet` SHA50af8003a587b601f8fefd465920c2104c731c5ac3c0aedf3b48f83998950723. Snapshot is retained inside the archive; later harness edits cannot change these measurements. The initial author clock conversion bug was caught and fixed **before** this snapshot/run: seconds×1,000,000 plus nanoseconds/1,000. The snapshot uses exact known tuple units, no scale heuristic.

Explicit envJANET_LANES=2 (or0 for local profile),JANET_NO_PROFILE=1, JANET_PROFILE removed. Other environment inherited. Same real Nforwarding actors plus sink, dependent receive/increment/send, result=N assertion. Phase markers add small logging overhead; spawn-start and wave-start marker writes are enclosed in respective phases and label lag is reported. Spawn-end is enqueue/construction-request completion, not a global all-actors-parked barrier.

Performance and sampling runs are separate. Larger trials monitor the **owned PID** for at most10s and sampledRSS1GiB, then kill only that process on a bound. RSS sampling every~100ms is a sampledpeak, not exact maximum, per-live-actor cost or OS memory pressure measurement. No million-live test was attempted. Placement is **unknown per actor**: lane count is not a placement map, and the portable pid lane field is structural, not worker identity.

## Larger-wave observations (three repetitions each)

| Forward actors | Correct/exited0 | Wave median [min,max] ms | Max sampledRSS KiB |
|---:|---:|---:|---:|
|10000|3/3|81.402 [69.569,88.507]|80288|
|20000|3/3|151.056 [147.502,151.814]|156880|
|40000|3/3|307.475 [302.800,332.643]|304704|
|80000|0/3|**no accepted wave timing**|573152|

At80000 all three trials printed spawn-end and wave-start, then failed to complete before10s. The runner explicitly killed its child (returncode-9), not an OS OOM report. RSS remained below the1GiB guard. This is an observed **large routed-wave completion failure** and must not be hidden as a slow-but-successful data point. Timings at10k–40k do not justify extrapolating a million actors.

A separate6.3s diagnostic80k run retained a2s full process sample after startup. All three threads(main+two workers) were parked in kevent across1755samples each. This establishes quiescence during that interval, not why progress was lost. It does not prove a particular missing wake, queue corruption or GC root defect. The previous unmodified35671ede100k attempt also failed to complete under its bounded wait; the new collector fix is not thereby established as the cause.

## Residual local profile

A separate local N16000 correct/exited0 run reported spawn20.132ms, wave2396.977ms. Of1777main-thread samples,1753 descend through completion-time `janet_collect_fiber_minor`, mostly `janet_mark_fiber`, `janet_procat_mark` and environment queue draining. Thus the remaining eligible collections still dominate sampled local CPU even after no-candidate collections were removed. This matches residual roughlyquadratic growth; no further root edge may be skipped without a new proof.

A separate correct40k two-lane profile reported spawn327.569ms,wave295.591ms,shutdown21.197ms. Full process report includes active marshal/unmarshal/channel delivery/completion work and parked stacks across main+workers. Because the report aggregates both construction and wave, **do not interpret its counts as an exclusive wave phase breakdown**. Markers supply timing boundaries but per-sample phase attribution would require a phase-triggered sampler/trace. No Go comparison follows.

## Evidence and next action

`phase-evidence.tar.gz` contains42files: frozen source, all12larger trials with stdout/stderr, two successful full-process profiles, failure diagnostic and phase logs. Every member verified byte-for-byte against original; SHAe449d6cb708f83e0ef8fd6e3a7df25935d780c4c87fb7f6543f9fafc4bd0d3f9.

Do not declare complete large-wave scalability. Next diagnose the80k post-construction quiescence with exact scheduler/dispatcher/serial/registration counters and a bounded reproducer, preserving failed statuses and source. This is useful remaining work, not a blocker or a reason to replace dependent mailbox handoffs with a loop.
