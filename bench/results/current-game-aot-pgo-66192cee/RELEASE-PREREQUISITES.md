# Remaining native/PGO release prerequisites

## Round-two persistence check

`native-blocker-round2/` captures a fresh supported-driver build using the same frozen game and pinned runtime. Exit 1, 1.850 s process wall; stderr is byte-identical to the original failed build (7914 bytes), ending in captured upvalue slot 256 exceeding limit 255. All 228 game-source and 214 saved-world hashes were independently checked again after the attempt and remain unchanged. This check is not a benchmark or a PGO training run.

## Driver gates in pinned `src/boot/boot.janet`

- `--release` requires `--native` (line 6329).
- `--pgo-auto` requires `--native` (line 6333).
- `--pgo` requires `--native` (line 6339).
- Release requires existing verified profile evidence and excludes collection/control flags (lines 6330–6331 and release evidence handling).
- Explicit PGO collects the interpreter workload first (lines 6455–6473), then emits/compiles the native image. Therefore the compiler failure does **not** mean collection itself is impossible; it prevents completing that driver pipeline.
- The driver verifies candidate stdout byte-for-byte against the interpreter oracle before installation (native build/gate around lines 6767–6789 onward).

## Two independent obstacles

1. **Native image compilation:** six admitted native-name definitions push the single image thunk beyond the captured parent-slot encoding. Both the standard and DCE paths fail. A tested image/prelude-scoping fix would change the pinned runtime and must be a separate revision/experiment; do not silently patch it for this result.
2. **Normal-workload byte parity:** the frozen 6000-frame workload prints elapsed worldgen time, FPS, worst-frame timing and steady statistics to stdout. Re-running the same program cannot be expected to reproduce those bytes. Once compilation is repaired, the normal driver's full-stdout oracle still needs an agreed deterministic validation mode or separate scene/state oracle. Do not strip timing lines after the fact and claim the standard release gate passed.

External interpreter profiling with `JANET_PROFILE=1` and `JANET_PROFILE_OUT=<path>` is possible without native compilation. It would not create a verified release or remove either obstacle; no such run is counted among the requested three training runs here. The existing `--verify-pgo` mode uses a different bounded workload and is not silently substituted for the frozen 6000-frame saved-world contract.

## Continue only under an explicit new contract

Keep the completed supplemental interpreter/plain-AOT pair intact. For a native/PGO continuation, first agree how a separately fixed runtime is pinned and how deterministic scene/state parity will be validated while retaining all timing logs. Then build the unprofiled native control, run the three representative training sessions sequentially, compile and verify the release, and perform matched sequential measurements with reported native execution coverage. None of those pending stages is satisfied by the emitted-candidate manifests or the plain VM image.
