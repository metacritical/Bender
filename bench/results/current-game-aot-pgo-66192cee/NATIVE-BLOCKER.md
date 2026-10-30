# Native game build refusal on pinned runtime

## Observed result

Both unprofiled hybrid-native builds fail with exit 1 before producing a linked executable:

```text
cannot capture parent local save-on-exit!:
captured upvalue slot 256 exceeds bytecode limit 255
```

- Standard control: `native-unprofiled-build/` (`--native --no-auto-pgo`).
- Supported dead-code-elimination probe: `native-unprofiled-dce-build/` (same flags plus `--dce`).
- Each directory preserves complete stdout/stderr, capture metadata, generated native C and name/executor/FFI-alias manifests.
- Source/runtime identities: frozen manifests in `workload-verification.json`, pinned runtime in `runtime-build-pinned-v3/output-identities.json`.
- Both failures occur on unchanged frozen game source and the same pinned `66192cee` runtime. No workaround source edits were made.

The emitter reports **6 scalar kernels and 78 executor candidates**, with 0 continuations, 77 FFI-alias install forms and 1660 native-skip diagnostics. These are emitted candidates, **not measured runtime coverage**: neither hybrid image compiled, linked, or ran. Plain AOT separately succeeds, but it is a boxed-bytecode VM image with **0 emitted game-native kernels**, not a substitute native result.

## Mechanism and scope

The compiler rejects a captured parent-local slot above 255 (`src/core/compile.c:435-440` in the pinned runtime source). The encoding is one byte; silently relaxing the check would be incorrect. The native image path adds prelude/registration forms before compiling the program as one thunk (`tools/mkimage.ben`, including native-name prepends at lines 336-345, executor registration and FFI-alias forms). The plain-image path does not add the native prelude and compiles the same game successfully.

An independent read-only audit confirmed the same failure in both logs and found no supported driver flag or environment variable that changes this capture limit. `--dce` does not avoid it on this workload. Fixing image/prelude scoping in the runtime or restructuring game locals would change the controlled sources; neither is part of this comparison.

### Causal precision and manual-manifest alternative

Do not infer that the 77 FFI aliases cause the overflow simply because that diagnostic immediately precedes the error. A separate child reported scratch experiments showing the six native-name `def` prepends are sufficient: trimming one entry from the generated `native-names.txt` allowed a manually staged image/link and a small `--verify-pgo` test, even with all aliases retained. Those scratch experiments were outside the delegated read-only scope, are not controlled 6000-frame evidence, and are not included as a benchmark result. Their report corrects the earlier alias-specific causal hypothesis; the retained standard-driver failure itself remains valid.

The supported AOT driver has no kernel-roster trim flag. Manually editing generated manifests changes admitted native coverage and bypasses the normal driver/release path; it was **not** accepted as a matched release for this experiment. The finding is therefore scoped to the unchanged supported driver, not a claim that every possible hand-assembled native executable is impossible.

## Consequence for requested PGO work

Three representative profile runs and a verified final native/PGO release remain **pending behind this compiler defect**. External interpreter profiling is technically possible, but was not run merely to produce profiles for a native artifact that cannot currently be built. This is not a finding of zero PGO benefit: there is **no PGO measurement**.

A supplemental interpreter/plain-AOT comparison may be collected under the same saved world, render settings and 6000-frame workload, with complete raw logs and scene validation. It must be labelled as that pair only; it cannot answer the native or PGO performance question.
