# Current frozen game: AOT/PGO attempt and supplemental matched pair

## Requested native/PGO comparison is unfinished

The supported `66192cee` hybrid-native driver fails image compilation on the frozen game, both normally and with `--dce`:

```text
cannot capture parent local save-on-exit!: captured upvalue slot 256 exceeds bytecode limit 255
```

It emitted **6 scalar kernels and 78 executor candidates**, but produced **no native executable**. Emission is not executed native coverage. Three representative PGO training runs, the verified final release, and native/PGO measurements remain pending. There is **no PGO performance result**, not a finding of zero PGO benefit. See `NATIVE-BLOCKER.md` for retained diagnostics and the supported-driver scope. No game or pinned runtime source was changed to force a result.

## One supplemental interpreter/plain-AOT comparison

Both arms completed **6000 frames**, exit 0, sequentially (interpreter first), using identical frozen game/world bytes and render flags. This is **plain AOT embedding boxed bytecode**, not a native-kernel or PGO arm.

| Arm | Average FPS | Average frame ms | Steady FPS | Worst frame ms | >33.3ms spikes | Peak RSS MiB* | Process wall s* |
|---|---:|---:|---:|---:|---:|---:|---:|
| Interpreter | 173.5 | 5.76 | 173.3 | 190.9 | 29 | 207.95 | 37.411 |
| Plain AOT VM image | 165.8 | 6.03 | 166.6 | 341.6 | 19 | 136.05 | 39.487 |

\* Memory comes from `wait4` accounting of the `/usr/bin/time` wrapper; the separate `resource.time.txt` files retain the platform report. It is not the prior comparison's sampled child-RSS metric. Process wall includes wrapper, loading, setup, game loop and teardown. The 1 GiB threshold was checked after exit, **not enforced mid-run**; the 180-second timeout was enforced.

These are observations from **one successful matched pair**, not statistical throughput/worst-frame claims or proof of a GC cause. Plain AOT averaged lower FPS here while recording fewer >33.3ms spikes and a larger worst outlier; these mixed metrics do not establish a general advantage. The worst frame had 11% CPU busy for interpreter and 1% for plain AOT, so wall-frame outliers are not evidence of a particular runtime cause.

## Startup versus gameplay

- Cached spawn construction: 25 chunks, 72 ms interpreter / 70 ms plain AOT. This is only a startup component, not all startup.
- Steady-state threshold reached at frame 11 in each arm; each steady statistic covers 5958 frames.
- Gameplay summary excludes the first 10 frames. Do not infer exact startup by subtracting rounded frame averages from process wall.
- No complete frame-time distribution or GC timing breakdown was collected.

## Matching and retention

- Runtime source: `66192ceed5f2545e14546e12505eda4d31e6cca5`; authoritative pinned runtime identity is in `SOURCE-PROVENANCE.md` and `runtime-build-pinned-v3/output-identities.json`.
- Frozen game: **228/228 source hashes**, saved world **214/214 hashes**, checked before and after each successful arm.
- World slot: `15f2268da8ec2a4105ef2deaf6ae0b19`, Terrain Diffusion 12345. Saved-world manifest paths are relative to that slot directory.
- Scene in each arm: radius 16, loaded 64, drawn 64, wanted 64, streamed in/out 0/0; frustum culling enabled; edits 0.
- Same `--frames 6000 --uncapped --world-slot ... --world-root ...` arguments. The frozen bounded test mode rotates yaw/pitch deterministically by frame index. No fixed-camera flag was added because it would change the reference workload.
- Inherited `BENDER_*`/`BENDER_*` keys scrubbed; `BENDER_NO_PROFILE=1`, `BENDER_LANES=0`; distinct initially empty per-arm XDG cache roots.
- Complete stdout/stderr bytes, byte counts, SHA256 digests, commands, binary hashes, exits, timestamps, resource reports and pre/post workload validation are committed in `interp-6000/` and `plain-aot-6000/`. Unlike the earlier interpreter comparison, these raw logs do not depend on a temporary directory.
- Both arms recorded the same capture utility SHA256 `ac1b5ff70cebe9c839db151074bd994e458d00c66ac3a9ff3fa3a2c84bc8bbe2`, matching committed `95ea9846`. Subsequent tooling edits are not the implementation used for this pair.
- Frozen game/world archive: `frozen-game-source-66192cee.tar.gz`; source archive and recovery/roundtrip verification are also committed. Build outputs remain under ignored `build/current-game-aot-pgo-66192cee/`; generated C/manifests and output hashes are retained in the build evidence directories.

## Host limitations

The host was not idle. Per-arm one-minute load averages were approximately 8.9–9.7 on a 12-logical-CPU machine; unrelated browser, VM, indexing and a live game were present during preparation/execution. No unrelated process was stopped. Arms did not overlap each other, but host interference remains a limitation. Repeat sequential runs with retained logs under quieter controlled conditions before making stronger claims.

## Reproduction

Restore the archived frozen workload to the path in `workload-verification.json` (or select it with `--game-dir`), rebuild from the committed runtime archive using the exact captured build argv, then build the plain image with the command in `plain-aot-build/capture.json`. Run `run-matched.py` once per arm, sequentially, with new evidence and cache paths. Existing evidence directories and populated caches are refused. The exact measured argv is preserved in each arm's `capture.json`; use `--dry-run` to validate before execution.

Native/PGO continuation first requires a separately tested compiler/image-scoping fix or an explicitly agreed new experiment. Do not silently trim generated kernel manifests or change the frozen game and label that as the originally requested release.
