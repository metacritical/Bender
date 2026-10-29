# Game slowdown: corrected cause and matched evidence

Fix: `7a7d7623`, based on latest runtime `739994d4`. Frozen game: Trinecraft `6db06bcde71903ade5cd0d456629f834145e11d2`. This supersedes the causal claims in commits0b86d3ea/739994d4 and associated source comments: those experiments did not establish that page size or sorted directory lookup caused the reported profiling-related growth. The hash directory remains a separate optimization.

## No demonstrated regression after455d5c4f

Matched interpreter runs used the same exported tracked game source, no worker lanes, fresh cache per arm, explicit profiling mode, native-enabled clean runtime builds, serial execution, 6000 frames uncapped,90s wall/1GiB sampledRSS caps. Both455d5c4f and739994d4 exceeded1GiB while profiling (14.28s and12.26s respectively); both completed without profiling (6000frames,236800/239584KiB sampledRSS). These are runner-enforced stops, not OS OOM or user termination. Historical compiled/unprofiled success cannot establish profiled-interpreter success. No first-bad post455commit was found because the matched defect already exists at455.

## Demonstrated mechanism

`src/core/vm.c` maybe_collect had two independent problems:
1. The outer nursery >=base test could consume the branch even when the inner scaled nursery threshold declined collection; the else-if major check was then unreachable.
2. The major threshold included CURRENT block_count*1024. Uncollected small objects increased the budget faster than allocation pressure, continually moving the trigger away.

Disposable diagnostics on739994d4 recorded suspension0 throughout; at263million GC polls next_collection727004618bytes, block_count12399992, interval4194304. The major threshold was roughly12.7billion bytes and major-due remained0. This is a scheduling/budget defect, not proof of a profiler root leak.

`git show 53542114` identifies the scaled nested gate and current-count budget introduction on October11, before455d5c4f. This is the source introduction of the demonstrated mechanism, not a whole-game first-bad behavioral bisect.

Fix: check major pressure first and scale its budget using the post-major survivor count. Initialize that per-VM field, update after sweeping, clear at heap teardown. Keep page size/hash implementation, minor thresholds, interval API and suspension behavior unchanged. Independent review accepted source/lifecycle/rooting semantics.

## Discriminating evidence

| Build | Priority fixture | Survivor-budget fixture | Profiled game |
|---|---|---|---|
|739994d4|assert abort|assert abort|stopped >1GiB|
|priority-only patch|pass|assert abort|stopped >1GiB|
|full fix|pass|pass|6000frames,231344KiB peak,59.44s; no frame >100ms|

Full-fix profiling-off:6000frames,194256KiB,33.01s. Full-fix profiled soak:12000frames,230832KiB,109.06s,111.3averageFPS/8.99ms; worst63.4ms. Profiling still incurs instrumentation overhead; fixed memory growth does not mean profiling is free. Single trials are bounded behavioral verification, not a precise speedup estimate. Frame summaries, runner exit status and all rejected candidates are retained.

Narrow verification passed: gc-minor, gc-stress, process-heaps, fiber-capture, proc-spawn-return, proc-capture-locality, proc99/99, ev, slab-pages, no-candidate Cfixture, and full native test-executors matrix including forced quantum. Static test-gate reports broad GC blast radius and unmodelled callers; this is not exhaustive whole-repository certification.

Canonical regression: `make NATIVE_EXECUTORS=1 build/libjanet.a` then `sh test/suite-gc-major-pressure.sh`. RUNTIME_TREE selects an already-built historical runtime for expected failure checks. Fixtures allocate/compile before arming, invoke a real C callback on the running fiber and assert actual major pressure reset. Baseline and priority-only ablation use matching tree headers/libraries.

Raw evidence and reproducible runner scripts: `/tmp/janet-forward-455-evidence/`; baseline/fixed isolated worktrees: `/tmp/janet-forward-latest-739994d4`, `/tmp/janet-forward-survivor-739994d4`. Exported game source lives in the raw evidence directory. Earlier user-ended game trials remain user termination, not failed tests. Paused hybrid/census drafts are untouched.
