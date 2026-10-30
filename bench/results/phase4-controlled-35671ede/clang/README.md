# Independent Clang PGO / LTO assessment after the GC fix

## Scope

This is **C-compiler optimization of the shell plus Bender runtime**, not Bender-level `--native --pgo` specialization. Source pinned clean at `a69c09843714e1733660e5335b285dd383680d23`, after the no-reclaim scoped collector fix, in `/tmp/bender-wave-postfix-a69c0984`.

`bench/build-wave-clang-variants.py` built four matching shell/static-library pairs sequentially, cleaning between variants because Make does not track flag changes:

| Pair | CFLAGS | Link flags | Measured? |
|---|---|---|---|
| baseline | -O2 -g | -rdynamic | yes |
| instrumented | -O2 -g -fprofile-instr-generate | -rdynamic -fprofile-instr-generate | training only |
| profile-use | -O2 -g -fprofile-instr-use=<wave.profdata> | -rdynamic | yes |
| LTO-only | -O2 -g -flto | -rdynamic -flto | yes |

All pairs use NATIVE_EXECUTORS=1, same Apple Clang/llvm-profdata21, `/usr/bin/ar`, same source. LTO is NOT combined with PGO. Runtime build and training statuses all0; final source tree remains clean. SDKROOT was explicitly set for the successful run to `/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk`. An earlier attempt without SDKROOT failed at stdlib.h lookup and is not a successful build/timing. The build driver was subsequently fixed to select/record SDKROOT explicitly; the retained successful manifest predates that driver change, so this paragraph records the environment omitted from that manifest.

## Training

Exactly four sequential runs of the instrumented shell: original asserted wave, N1000 and2000, lanes0 and2. Each stdout exactly `procs=N result=N`; all statuses0. Nonempty per-process raw LLVM profiles were merged with the matching llvm-profdata. Only those four raw files feed the merged profile; training timings are never measured performance results. This is a **narrow wave training set**, not a representative game/runtime-wide profile.

## Measurement

Five repetitions per arm, rotating baseline/profile-use/LTO order across rounds, sequential processes. Original `bench/cross-runtime/wave.ben` (unchangedSHAe07d1f25...), N2000 and8000, lanes0 and2. Instrumentation/profile env removed for timed runs. **All60 trials exited0 and asserted result=N.** Full per-trial stdout/stderr, executable hashes, exact argv and timing phases retained. No Go comparison.

Wave-only milliseconds, median [min,max]:

| Placement | N | Baseline | Clang PGO use | LTO only |
|---|---:|---:|---:|---:|
|local|2000|33 [33,40]|24 [23,32]|33 [32,34]|
|local|8000|540 [538,546]|400 [395,423]|541 [536,545]|
|two lanes|2000|13 [13,17]|13 [11,22]|13 [13,16]|
|two lanes|8000|55 [53,58]|60 [48,65]|51 [47,57]|

Local PGO reduced wave time about26–27% in this sample (baseline/profile-use speed ratio~1.35–1.38). LTO alone did not help local waves. Cross-lane results are mixed/noisy; **there is no universal PGO benefit**. The local N8000 case was not a training size, but this is still the same benchmark family. Residual growth remains superlinear; PGO does not resolve the collector algorithm.

No default build flags were changed. A production recommendation requires broader workloads, independent repeats and safety/regression gates; this assessment establishes a measured local opportunity, not a guaranteed general win or native actor parity.

## Evidence

`build-and-trial-evidence.tar.gz`:33files, full compiler/training logs, manifest with artifact/archive hashes and tool versions, four raw profiles, merged profile and all60 trial records. Every member was verified byte-for-byte to the original. SHA256 `40aa8d4b8653be2d565bc6d05d18c8759d931e0e0f9626132f13c8fd3391c26e`.

Binary snapshots remain outside repository under `/tmp/bender-wave-clang-sdk-a69c0984/{baseline,instrumented,profile-use,lto}`; each bender/archive hash is in the manifest. No instrumented binary supplies a timing row.
