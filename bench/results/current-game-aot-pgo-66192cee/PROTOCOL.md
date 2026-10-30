# Controlled current-game interpreter / AOT / PGO protocol

## Immutable reference

- Runtime source: Bender commit `66192ceed5f2545e14546e12505eda4d31e6cca5`, tree `a9e757129652cea1715f0a4c4d1f512bf48c168c`.
- Runtime build source is isolated at `build/current-game-aot-pgo-66192cee/runtime`, extracted using `git archive 66192cee`; unrelated live checkout modifications are not included.
- Prior contract: `../current-game-6a4f008b-c07042a7/README.md`, runtime-source and saved-world manifests. Old JSON summaries are not byte-exact logs and do not substitute for new runs.
- Frozen workload must match the prior committed file manifests before any game run. Do not silently substitute the changing live Trinecraft worktree.
- World slot: `15f2268da8ec2a4105ef2deaf6ae0b19` (Terrain Diffusion 12345).
- Game arguments: `--frames 6000 --uncapped --world-slot 15f2268da8ec2a4105ef2deaf6ae0b19 --world-root <frozen-world-root>`.
- Benchmark runtime environment: scrub inherited `BENDER_*` / `BENDER_*`, set `BENDER_NO_PROFILE=1`, `BENDER_LANES=0`, matched isolated cache conditions. Training differs only where required to collect profiles, which must be disclosed.

## Acceptance gates

1. Capture complete stdout/stderr bytes directly to durable files for every build/training/benchmark command. Retain command, working directory, environment overrides, exit status, timing, memory, executable identity, stream lengths and SHA256 digests. Never overwrite a run's evidence.
2. Build interpreter support and plain AOT from the same runtime snapshot. A linked executable is not evidence of emitted native game kernels: retain generated C, build diagnostics, native kernel and fallback counts. If no game kernels emit, report that limitation explicitly.
3. Run three representative training sessions sequentially, requiring the frozen scene and all 6000 frames each time. Compile the release using the resulting profile via the supported mechanism, verifying actual profile consumption, not merely an output executable.
4. Run interpreter, plain AOT, then PGO release sequentially with identical game/world/render/frame inputs and matching runtime flags. No other benchmark arm may overlap. Do not terminate unrelated user processes.
5. Require exit zero, exactly 6000 frames, expected loaded saved-world scene (radius 16, loaded/wanted 64, no streaming in/out) and unchanged workload identities. Record drawn chunks and any scene deviation; reject an unmatched arm.
6. Keep process wall time and startup metrics distinct from gameplay frame statistics and steady-state FPS. Do not infer startup by subtracting rounded averages.
7. Treat the outcome as one matched comparison only, not an established speedup, worst-frame distribution improvement, GC-causality result, or proof of PGO benefit without verified native/profile coverage.

## Host caveat

Initial host inspection found substantial unrelated activity (including a live Trinecraft spawn-test process, OrbStack, browser rendering, Spotlight and iCloud). No unrelated process is to be interrupted. Retain host snapshots and disclose contention when interpreting results. Do not promise deterministic camera timing: the reference did not record a per-frame camera trace.

## Status

Preparation only. The isolated native-enabled runtime support build succeeded; this is not an AOT game artifact and does not establish any emitted game kernel. Game AOT compilation, PGO training/release and sequential benchmarking remain unstarted. No new performance result is claimed by this prospective protocol.

The saved-world manifest entries are relative to the slot directory `worlds-v1/15f2268da8ec2a4105ef2deaf6ae0b19`, not the world root. Its three subdirectories are snapshots of that same slot, not other slots. Workload verification must use this exact root mapping.
