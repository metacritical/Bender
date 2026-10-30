# Evidence-local matched-run driver (`run-matched.py`)

## Objective
A new, independent driver under `bench/results/current-game-aot-pgo-66192cee/`
that runs ONE arm of the frozen workload against an already-built executable and
writes byte-exact, self-contained evidence. Reusable for both TRAIN (PGO profile
collection) and BENCH arms. The parent runs arms sequentially; this driver never
runs more than one child.

## Hard constraints
- **No game runs by me.** I implement and test the driver only; narrow tests use
  `/bin/sleep`, `/bin/sh`, and static fixtures — never the game.
- **Do not modify** the game, the runtime, or `tools/benchmark_capture.py`.
- Import `tools/benchmark_capture.py` as a module (no copy, no fork).
- Evidence and manifests must be repo-local: no dependency on `/tmp`.

## Design
1. **Import** `benchmark_capture` via `sys.path` insertion of `tools/`, recording
   the utility's sha256 in the evidence so the capture implementation is pinned.
2. **Pre-flight validation** of the frozen tree against the COMMITTED manifests
   (`runtime-source-manifest.json` 228, `saved-world-manifest.json` 214). Block
   before spawning if either mismatches. Re-validate AFTER the run to prove the
   arm wrote nothing into the frozen tree.
3. **Environment**: drop every `JANET_*`/`BENDER_*`, then set
   `JANET_LANES=0` and `XDG_CACHE_HOME=<cache-root>`.
   - BENCH: `JANET_NO_PROFILE=1`.
   - TRAIN: `JANET_PROFILE=1` + `JANET_PROFILE_OUT=<evidence>/profile.jdn`,
     and **no** `JANET_NO_PROFILE` (verified: the dump is suppressed otherwise).
4. **Invocation**: `<executable> [<script>] --frames 6000 --uncapped
   --world-slot 15f2268da8ec2a4105ef2deaf6ae0b19 --world-root <frozen>/worlds-v1`,
   cwd = frozen game dir, `--resource-tool time`.
5. **Caps**: 180 s wall via the capture `timeout`. The utility exposes **no live
   RSS cap** — only post-hoc `ru_maxrss`. Record the 1 GiB threshold as a
   VERDICT on the observed peak and say plainly that it is not enforced mid-run.
6. **Parse** the game's own lines separately: `frames:` (exact 6000), `scene:`
   (radius 16, loaded 64, drawn 64, wanted 64, streamed 0/0), `steady:`,
   `worst frame:`, plus `worldgen:` startup. Keep startup distinct from frames.
7. **Host-load snapshot** before and after (load average, bundle names only, no
   secrets). Capture already records stream hashes and rusage.
8. **CLI**: `--label --executable --script --train --evidence-dir --cache-root`.

## Gates (driver exits non-zero when unmet)
- exit 0, exactly 6000 frames, scene identity exactly as above.
- name the failing field rather than reporting a bare failure.

## Testing (narrow, no game)
- Parser fixtures built from the four committed prior JSON summaries
  (`baseline-loaded`, `tip-loaded`, and both `-long`) — real text, and each
  fixture asserts the exact expected verdict.
- Negative fixtures: short frame count, wrong radius, streaming in/out.
- One `CaptureRequest` integration test with `/bin/sleep`.

## Risks
- `tools/benchmark_capture.py` is currently dirty in the working tree (another
  agent fixed `start_new_session` mid-session). The driver imports it, so the
  utility's sha is recorded per run; if that file changes, evidence says so.
- TRAIN profiles only exist for the interpreter/AOT binary that supports
  profiling; a binary built without profile support writes no dump and the
  driver must say so rather than claim a profile.