# TrineXraft Task 1: indexed buffer access stop-gate result

**Decision: NO-GO for this isolated Tier-B change.** A reversible implementation eliminated 2,162,688 native-to-VM exits in the existing TrineXraft benchmark, but five alternating, uninstrumented baseline/candidate runs observed only a **5.4% reduction in worldgen/chunk p50** and **0.6% in mesh-chunk p50**. Neither demonstrates the required double-digit loop-time opportunity on this noisy host. The implementation and its fixture changes were reverted; no buffer-access lowering is shipped. The TrineXraft fallback is **parallel meshing across lanes**, not lifting Tier A's settled loop veto or expanding into Tasks 2–3.

## Gate, method and baseline (before compiler edits)

Checked branch `ttfx-mixed-regions` at `2eb1d5b0`; the existing `build/bender` was built with `NATIVE_EXECUTORS=1`, its `janet_native_register` contains the real verifier, and a SHA256-identical safety copy is in ignored `tmp/bender-task1-known-good` (`d03f28353d3884e03c97088e2753439189b69b4af2995f070b66b96d2d0374e5`). Compiled `/Users/pankajdoharey/Development/Projects/ML/trinecraft/bench/kernels.janet` with `JANET_NATIVE_EXECUTORS=1 ... --aot ... --native`; ran both sides with `JANET_NO_PROFILE=1` **inline**. No TrineXraft source was modified.

| Measurement on this host | Interpreted | Executor-enabled native baseline |
|---|---:|---:|
| worldgen/chunk p50, single benchmark run | 5.200 ms | 6.084 ms |
| mesh-chunk p50, single benchmark run | 5.522 ms | 6.576 ms |
| raycast p50 | 11.294 µs | 11.968 µs |
| palette pack p50 | 1.841 ms | 2.207 ms |

The native baseline generated **0 `native-names.txt` lines, 12 `native-executors.txt` lines, 0 `native-continuations.txt` lines, 161 `native-skip` lines**, and 412 occurrences of `janet_native_poll` in `native.c`. The older task brief's 0/0/0 executor claim is stale for this revision. Positive liveness is established by *executed* per-definition counters: `generate-into` 2,207,658 entries / 54,645,174 native ops, and `fbm-signed-2d` 305,300 / 5,576,851. Merely printing `available 12` would not prove attachment. The benchmark's `mesh-chunk` descriptor was emitted but did not appear among active native-op rows.

The baseline `--work-profile` vector was 726,509,338 interpreted dispatches, 60,222,025 native ops, 2,500,740 VM exits and 2,230,404 generated native boundary exits. The worldgen descriptor alone recorded **2,162,820 boundary exits = 132 × (16,384 voxel stores + one other exit)**: the benchmark generates 120 timed chunks, 3 warmup chunks and 9 world chunks for meshing. The generated `nx_21` exits at its original buffer `put` PC 69 on each voxel and polls at the backedge PC 71. Thus buffer `put` represents approximately **2,162,688 / 2,230,404 = 97.0% of native boundary exits**, not 97% of dispatches or time.

Static bytecode audit: `generate-into`'s inner voxel loop, PCs 25–71, has **47 opcodes**, including **one `put` (2.13%)**, zero `get` and zero calls. That store executes exactly 16,384 times per generated chunk. The mesher's source-mapped loop regions span 382 static opcodes with 21 `get` and 5 `put` sites (26/382 = 6.81%); these paths are conditional, not 26 operations per iteration. The source includes block and neighbour reads, colour writes and FFI calls (`src/mesher.janet:143-229`). Neither source-site density nor exit density is a CPU-time share. The existing `--work-profile` has no per-opcode interpreted histogram and PGO records receiver types/branches, not per-`get`/`put` time; the requested dynamic collection share of **interpreted dispatches remains unknown**. An isolated `sample` trace was too coarse to attribute the inline `run_vm` cost of a single opcode; direct `janet_put` leaf samples omit dispatcher and re-entry cost. This uncertainty made the pre-edit gate provisional rather than a dishonest numeric time claim.

## Reversible treatment and measured result

The test-only implementation added a register-key buffer `get` with Janet's nil-on-invalid semantics; a `janet_native_buffer_put_fast` accepting only existing nonnegative integer indices and integer values; and a `put` useful-region run-length adjustment. The buffer data pointer was derived from the live object for **each** read/write; growth, invalid indices and values exited *before* mutation at the original PC, letting boxed `JOP_PUT` own reallocation, zero-fill and errors. No pointer survived a call, poll or VM exit. Generic loop state remained in fiber slots, with the existing backward-edge polling and original-PC interrupt handling; no new raw-C-local fused loop or slot-materialisation scheme was introduced. Existing `make test-executors` already exercises forced quantum budgets 1/2 (`test/suite-native-executor.sh:129-134`). A temporary fixture covered buffer/array read/write and guard cases, then was removed with the experiment.

| Same benchmark and seed | Baseline | Treatment | Difference |
|---|---:|---:|---:|
| Interpreted dispatches, one counter run | 726,509,338 | 724,346,843 | −2,162,495 (**−0.30%**) |
| Native ops | 60,222,025 | 60,222,029 | approximately invariant |
| Native entries | 2,512,958 | 350,274 | −2,162,684 |
| VM exits | 2,500,740 | 338,052 | −2,162,688 (**−86.5%**) |
| Generated boundary exits | 2,230,404 | 67,716 | −2,162,688 (**−97.0%**) |
| `generate-into` entries / exits | 2,207,658 / 2,162,820 | 44,970 / 132 | each −2,162,688 |

These counters measure *work*, not elapsed time; do not add native ops to VM dispatches as if they were equivalent. Counter runs of this timed benchmark are not perfectly repeatable: an identical-image repeat differed by 175 interpreted dispatches, six native entries/ops and ten interpreted calls, while boundary exits stayed exact. The changing `os/clock`/GC path and setup make that small residual a limit on exact vector equality; the 2.16-million-exit effect dwarfs it.

Five alternating baseline/candidate runs without `--work-profile`, each reporting its own 120-iteration p50 with `JANET_NO_PROFILE=1`:

| Run | worldgen baseline → treatment (ms) | mesh baseline → treatment (ms) |
|---|---:|---:|
| 1 | 5.732 → 5.368 | 6.247 → 6.244 |
| 2 | 5.796 → 5.340 | 6.394 → 6.226 |
| 3 | 5.694 → 5.422 | 6.378 → 6.294 |
| 4 | 5.760 → 5.456 | 6.314 → 6.278 |
| 5 | 5.693 → 5.472 | 6.262 → 6.284 |
| **Median of run p50s** | **5.732 → 5.422 (−5.41%)** | **6.314 → 6.278 (−0.57%)** |

Worldgen paired latency reductions ranged 3.88–7.87%; mesher −0.35–2.63%. The one-off unpaired comparison 6.084→5.416 ms suggested 11.0%, but failed the alternating-pair check and is **not** the conclusion. The host's prior identical-binary A/A comparisons have swung 0.71–1.15 per effect; this modest p50 movement is not a defensible speedup claim. Even granting its observed direction, it misses the explicit double-digit threshold. The mesher's dominant FFI writes and other boundaries remain untouched.

Validation of the reversible treatment: `make NATIVE_EXECUTORS=1` built; generated `nx_21` contained in-loop polls and `janet_native_buffer_put_fast`; emitted executor manifest had 12 lines; live per-definition counters above were nonzero. `make test-executors` passed its budget-1, budget-2, portable, disabled and sanitizer matrix, including 47 identical parked-PC/slot interrupt checks. `./run.sh test` passed all twelve TrineXraft suites. `suite-callgraph.janet` passed 256/256; `suite-aot-*.janet`, `suite-native-array-result.janet`, and `suite-dce-*.janet` passed when run **without** globally exporting `JANET_NO_PROFILE` (the profile-environment test requires profiling). The full executor-mode `make test` result is recorded separately in the run log; the plain-build-mode suite and isolated byte-identical output comparison were not needed to ship a feature, because the experiment was reverted. Do not treat those as passed checks. The non-double-digit outcome, rather than unproven correctness, is the reason to stop.

The complete experimental diff and raw measurements remain **gitignored**, not shipped: `tmp/task1-negative-experiment.patch`, `tmp/task1-native-work.json`, `tmp/task1-candidate-work.json`, and `tmp/task1-pair-*-{baseline,candidate}.out`. This note records the null so future work starts with the right gate rather than repeating it.
