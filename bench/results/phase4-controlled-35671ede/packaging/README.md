# Packaging control: reproduce the AOT gap inside one interpreter

## Hypothesis

Script execution compiles/evaluates individual top-level forms; `mkimage` compiles the image as one `(do ...)` thunk. This changes frame/capture lifetimes even when the forwarding actor body is identical. The matched matrix reproduced a local script/AOT gap already in plain AOT, and both profiles were dominated by completion-time GC marking.

## Discriminating experiment

Run the original `bench/cross-runtime/wave.janet` and a source with the same forms grouped inside one top-level `do`, using the **same interpreter executable**. N forwarding actors and the sink, receive/increment/send operations, completion assertion, and result output remain. `do` changes lexical compilation scope by design; this is a test of top-level compilation shape, not an identical object-graph control.

All36 subprocesses exited0 with result=N:
- `packaging-control.jsonl`:24 rotated normal/grouped trials, three repetitions, N1000/2000, baseline and post-fix binaries. Grouped source uses three-decimal phase timing; all raw values retained.
- `packaging-exact-control.jsonl`:12 confirming grouped-source trials with the **original integer-ms format**, removing output-format/constant differences as an explanation.

Baseline interpreter: `/tmp/janet-wave-controlled-35671ede/build/bender`, SHA07e766fb28ab15eb78d563bbdc2e7c9a9b2adf86e187df0d6b705c7d6648151c. Post-fix interpreter: clean-helper candidate at `/tmp/janet-wave-candidate-35671ede/build/bender`, measured collector patch archived in `../candidate/collector-candidate.patch`; same source base/runtime flags with collector change. No new compiler-profile flags.

## Results

Wave-only medians ms (grouped uses exact-format confirmation):

| Runtime | N | Normal script | Single-do script | Plain AOT reference |
|---|---:|---:|---:|---:|
| baseline |1000|99|279|281|
| baseline |2000|406|772|777|
| post-fix |1000|8|22|23|
| post-fix |2000|33|64|61|

Normal values are medians of the24trial batch. AOT reference values are from earlier matched matrix/candidate batches, not simultaneous paired AOT timings. Every range and outlier remains in the raw records; the baseline grouped decimal-format first trial379ms is not discarded.

## Supported conclusion

**Grouping source into one top-level thunk is sufficient to reproduce the script/AOT local discrepancy without changing the interpreter binary or linking a static AOT executable.** Static link or PGO differences are not required for this effect. The plain-AOT profile identifies the extra cost as GC traversal during the wave, not startup alone.

This does NOT prove the exclusive object-level root path, nor that packaging is the only influence on all artifacts/workloads. Compiling as one thunk changes scope, captures and reachable-frame shape. Further mark-edge instrumentation would be needed to apportion the additional traversal among specific references. Do not remove roots or alter compilation semantics merely to improve this benchmark.

The runtime no-reclaim optimization reduces both cases substantially but leaves their graph-shape difference and residual quadratic growth.
