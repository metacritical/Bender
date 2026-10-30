# Current loaded game: baseline vs tip

Both arms completed 600 and then 6000 gameplay frames, exit 0, without hitting
90-second wall or 1-GiB sampled-RSS caps. Sequential runs, baseline first.

| 6000-frame arm | average FPS | frame ms | steady FPS | worst ms | >33ms spikes | sampled peak RSS KiB | wall seconds |
|---|---:|---:|---:|---:|---:|---:|---:|
| 6a4f008b | 164.8 | 6.07 | 165.0 | 178.2 | 72 | 506416 | 38.954 |
| c07042a7 | 176.1 | 5.68 | 176.1 | 44.6 | 21 | 237904 | 36.360 |

The tip was ~6.9% higher average FPS and used ~53% less sampled peak RSS in
this pair. These are observations, not statistically established speedups or
attribution to a particular GC change. Worst frames had only 2%/9% CPU busy,
so OS/render waits can materially affect wall-frame outliers.

600-frame smoke: baseline157.3FPS/506000KiB/worst81.8ms;
tip182.5FPS/229776KiB/worst24.5ms. All four arms validated exact frame counts.

## Matched inputs

- Frozen CURRENT Trinecraft worktree from
  `/Users/pankajdoharey/Development/Projects/ML/trinecraft`, not the older
  archived game. Its live HEAD advanced during preparation; captured metadata
  says2c68438, main/src matched at freeze. File hashes, not that label, identify
  this dirty-source snapshot: `runtime-source-manifest.json` SHA256
  `8d3fc0c3fee811ad5b14f8e4141e5cd4522687dc9ce885b43b754acd37b7ef48`.
- Saved world `15f2268da8ec2a4105ef2deaf6ae0b19`, named Terrain Diffusion12345;
  copied into the frozen workload. `saved-world-manifest.json` pins214 files.
  Original and copied world were byte-equal after runs. No new model generation.
- Same scene radius16, loaded64, wanted64, streamed0 in/out; drawn53 at end of
  both smoke runs and64 at end of both long runs. Camera/OS timing can still
  vary; no deterministic per-frame camera trace was recorded.
- Both fresh isolated source trees built `make clean && make NATIVE_EXECUTORS=1 -j4`.
  Logs preserve compiler flags. These are script/interpreter runs, NOT AOT/PGO
  native game artifacts. Enabled build support alone does not establish native
  actor/game kernel execution.
- Profiling OFF (`JANET_NO_PROFILE=1`), lanes0, uncapped rendering, fresh per-arm
  cache at smoke. Long runs reuse their respective smoke cache directories;
  profiling remains disabled. Other JANET_/BENDER_ env keys scrubbed.
- Baseline binary SHA2568355f162165c0957acd312ea92153e78c98a4d5e7bcf4bf04e4f71c2b965a8d1;
  tip4ef0e7dea4cbe648c07e5d1f321167c83644277f89f2c130dad82eaa95690e9a.

## Startup versus frames

Cached spawn-chunk construction: baseline72ms, tip68ms (25 spawn chunks).
This is not all startup: total process wall includes loading, mesh/upload,
window setup and teardown, whereas frame statistics are the game loop.
Full frame-time distributions/percentiles and GC timings were not captured.
Heavy system swap/background activity existed; these runs do not reproduce or
refute the old1234FPS workload, and do not establish nonblocking GC.

## Reproduce

The exact executed scripts are `run-loaded.py` (600 frames) and
`run-loaded-long.py` (6000). They use preserved isolated paths under `/tmp`;
restore the frozen game and saved-world bytes matching both manifests first.
Then run sequentially:

```
python3 bench/results/current-game-6a4f008b-c07042a7/run-loaded.py
python3 bench/results/current-game-6a4f008b-c07042a7/run-loaded-long.py
```

The scripts sample the actual child PID, kill only its own process group on a
cap breach, and require exit0 plus exact frame count. JSON records preserve
commands, identities, frame summaries, exit status and resource outcomes.
Raw stdout/stderr files disappeared from the shared temporary evidence directory
after results were captured; the JSON summaries are retained, but are not full
raw logs. The runner and manifest are saved here; the ~54MiB game plus saved
world remain in `/tmp/janet-current-game-ab-evidence/game-source` and are NOT
embedded in this commit. `FREEZE-NOTE.md` predates addition of the saved world;
its exclusion statement is superseded by this README and saved-world manifest.

No game/runtime source edits, checkout switches, GC changes, or cherry-picks
were made in the main working trees for this comparison.
