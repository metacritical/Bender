# Omarchy CLI: verified AOT/PGO release versus Rust

Measured 2026-10-18 on the local Apple M2 Pro. This is an end-to-end
**CLI dump-path** comparison, not GUI FPS, playback-only timing or a
terminal emulator benchmark. No application source was changed.

## Artifact and workload identity

- Compiler: original checkout `build/bender`, `1.43.0-dev-5180da8a`.
  Experimental native executors remain compiled off by default.
- New Janet application: unchanged ttfx Janet source, AOT `--native`, two
  PGO workloads using Omarchy decrypt seeds 42 and 7, bounded at 12 frames,
  then `--release`. Release explicitly replayed both workloads and passed
  `pgo parity ok`. No benchmark data from seed 99 was used for training.
- Full comparison fixture: ttfx `ruby-port/logo.txt` (81×10 Omarchy),
  SHA-256 `f98c966b1670d71ffc26620a6ff885a73ab355b370bff6082cea3215ee580a29`.
  Terminal 80×24; actual input-sized visible canvas 80×10 (column 81 clipped),
  southwest anchors, seeds 42 and 99, deterministic virtual-clock dumps.
- Rust: existing local `target/release/ttfx`, Oct 17 release build,
  SHA-256 `de6b68d51557638b50168fcf8d88d03c02db30ff5821c03b9d5d0a4e24f9e7d5`.
  `TTFX_FX=0` selects original engine; `TTFX_FX=force` selects fx.
  This binary contains optional local profiling instrumentation, not a
  newly rebuilt pristine upstream artifact. Timed comparisons did not
  enable its phase profiler.
- Previous Janet SHA-256:
  `7f9bfa0ecc874cf3e1c171d65673070583bb51c1861120ac39b8d4fc91ec3b30`.
- New verified Janet SHA-256:
  `2bc41d2c311c6e4db72cc3c81d33da2b4fdf6729e9b9368e68bb8c4ae3e7398f`.
  Installed at ttfx `janet-port/build/ttfx`; previous binary preserved at
  `janet-port/build/ttfx.before-compiler-5180da8a`.

Classic native names remain `floor-div`, `round-half-even`, `out-bounce`,
`k-next-word!`, `k-next-pack!`, `k-next-scale!`. **No new scene kernel or
native loop executor runs in this release.**

## Correctness and timing method

Full seed-42 dumps match the previous Janet binary for all 37 effects.
The five timed effects also match Rust original and fx byte-for-byte at
seed 42; seed-99 new/previous Janet and Rust fx dumps match too. Seed-42
frame counts are wipe 138, fireworks 1216, decrypt 895, beams 331, smoke 265.

Three rounds per effect/seed, rotating engine order. Wall timer surrounds
the entire subprocess: startup, effect build, stepping, frame formatting,
and dump writes to `/dev/null`. Input-file handle prepared before timing.
All outputs discarded equally. Rust subprocess times are only 7–23 ms,
so process-launch overhead is significant; ratios are not inner-loop ratios.
Existing Janet `--bench` excludes effect build; it was **not** used here.

| Seed | Effect | Previous Janet ms | AOT+PGO release ms | Rust original ms | Rust fx ms | Rust fx speed advantage |
|---:|---|---:|---:|---:|---:|---:|
| 42 | wipe | 50.65 | 51.18 | 10.06 | 8.21 | 6.2× |
| 42 | fireworks | 436.43 | 440.06 | 20.30 | 12.25 | 35.9× |
| 42 | decrypt | 352.19 | 352.97 | 22.63 | 9.37 | 37.7× |
| 42 | beams | 219.40 | 220.73 | 20.66 | 9.24 | 23.9× |
| 42 | smoke | 226.42 | 228.95 | 18.77 | 9.08 | 25.2× |
| 99 | wipe | 50.12 | 51.46 | 10.14 | 7.24 | 7.1× |
| 99 | fireworks | 432.88 | 437.30 | 20.30 | 11.53 | 37.9× |
| 99 | decrypt | 355.97 | 363.97 | 22.40 | 9.39 | 38.8× |
| 99 | beams | 221.99 | 225.45 | 20.00 | 8.80 | 25.6× |
| 99 | smoke | 228.35 | 231.86 | 19.09 | 8.86 | 26.2× |

**Result:** no improvement established; new Janet is 0–3% slower, within
the previously observed CLI noise band. General PGO/discovery/dispatch
infrastructure has not yet accelerated these application hot loops.
The requested 10% lead over Rust/Node is not met.

## Time attribution evidence and limits

The Rust original engine's optional `TTFX_PROFILE=1` reports the following
single-run phase totals on matching dumps. They are **separate instrumented
runs**, not the uninstrumented median wall times above:

| Effect | Seed | Motion ms | Animation ms | Formatting ms |
|---|---:|---:|---:|---:|
| decrypt | 42 | 2.8 | 4.8 | 1.9 |
| decrypt | 99 | 2.9 | 4.8 | 2.0 |
| fireworks | 42 | 3.5 | 2.5 | 2.6 |
| fireworks | 99 | 3.4 | 2.5 | 2.5 |

Motion/animation timers run per active character and add overhead. These
counters exclude writes, setup, pruning, orchestration and process costs;
wall time minus these counters must not be called “setup time.” No matching
Rust fx phase counters were observed. Rust fx's ordinary output can use a
renderer thread whereas the dump path is synchronous.

Janet's earlier logo sample captured VM dispatch, dictionary lookup, table
updates, buffer operations and GC, but lacked phase tagging. Its fresh
closure-call profiles show repeated scene predicate/animation and geometry
calls, **not CPU percentages**. The next compiler work must establish native
coverage of boxed scene fields and mixed-operation loops; reducing output
volume or adding effect-specific kernels would not address the requested
general-language optimization.

Raw comparison outputs, hashes, three-round timings and PGO logs are kept
as scratch artifacts (`final-cli-compare.YWrCWg` in the approved run directory).
This document records the reproducible conditions and conclusions independently
of that temporary directory.
