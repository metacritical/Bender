# Janet performance plan — compiler work, ttfx as the sample workload

`ttfx` (`janet-port/` in the ttfx repo) is the benchmark vehicle, not
the product. The product is this compiler: widening inference +
optional `{:hint}` contracts → C kernels, hybrid boxed/native runtime,
Erlang-style actors. Goal (2026-10-07): beat the original Rust ttfx on
throughput (pacing disabled; e.g. decrypt 5,506 frames in 11.7 ms),
proving the model that an easy dynamic language can squeeze the machine
through smart compilation. Workload-side plan (bench table, ports):
`janet-port/docs/perf-plan.md` in the ttfx repo.

## Standing gates (must stay green through all of this)

`make` · `make test` (43 suites) · `make fibertest` · AOT oracle same
×6 (`hello fib yield ev err thread`) · ttfx CLI `--native` binary
byte-identical to Ruby (decrypt + wipe, seed 42) · GUI renders
(framebuffer capture after `EndDrawing`).

## Shipped enablers (done)

- Generational slab GC + promotion barrier (`cde44a37`): dirty pages
  at young→old transition; `JANET_GC_VERIFY` blind spot closed.
- FFI AAPCS64 HFA fix: all-float struct args one FP register per
  element; HFA returns parsed element-wise. `DrawTextEx`/`MeasureTextEx`
  now render byte-identically to C — users bind libraries, write no C.
- Toolchain: multi-module AOT (`:current-file`-scoped import
  pre-execution, import-form stripping, script-dir syspath),
  `(dyn :args)` in the harness, extern-decl hoisting for `--native`,
  `-x/--ldflags/--externs` passthrough.
- Pending commit at time of writing: HFA fix, tool fixes,
  ffi-struct marshal/unmarshal (unblocks GUI AOT images).

## Profile-guided widening (design, 2026-10-07)

Idea: every development run profiles boxed objects and learns their
types; the next widening cycle reuses that information. Widening starts
from evidence, not just from `:unknown`.

### Collection (boxed tier, dev/interpreter runs)

The VM already tags every value — recording is a side table, not a
tax on semantics. Instrument, behind a flag/env (`JANET_PROFILE=T`):

- function entry: parameter type vector per defn id;
- call sites: argument types per caller-pc (validates candidates);
- hot stores: slot types in counted loops (optional, phase 2);
- returns: observed return-type lattice per defn id.

Emit a profile file per project (e.g. `.janet-profile`), keyed by
`content-hash(defnsource) → {params, returns, counts}`. Stale on code
change by construction (hash mismatch → entry ignored).

### Consumption (callgraph widening)

`infer/analyze` currently seeds everything `:unknown` and widens to a
fixpoint. With a profile, seed from evidence:

- unanimous observations (one type, N samples) seed the slot;
- conflicts widen immediately (no wasted rounds);
- thin evidence (few samples) seeds but stays provisional — first
  contradiction at a call site widens as today.

Soundness is free: a mispredicted profile only means that call site
stays boxed (the hybrid fallback). Profile data is *evidence*
(probabilistic, versioned); `{:hint}` contracts stay *promises* (fail
loud). Never conflate the two.

### Cycle

run (profile) → compile (widen from profile + hints) → bench →
profile again. The profile file is a build artifact, checked in per
project like a lockfile, refreshed in dev. Cold builds without a
profile behave exactly as today.

### Phases

1. Collector: param/return type table behind `JANET_PROFILE`,
   file format + hash invalidation. No compiler changes. SHIPPED
   2026-10-08 (debug/profile-start|stop|reset|dump, deinit auto-dump;
   378-row deterministic ttfx profile verified).
2. Seeding: callgraph reads the profile, seeds the fixpoint,
   reports `profile-seed` vs `profile-conflict` per defn.
3. Hot-store profiling + invalidation UX (stale-profile warnings).
4. Evaluate: does PGO move any ttfx bench row? Keep only what
   measures. Long-term interplay with Phase 3 value types below.

## Measured baseline (2026-10-07, M2 Pro, decrypt/logo.txt seed 42)

| input | impl | frames | wall | per frame |
|---|---|---|---|---|
| `hi` (2x1 canvas) | interp `--bench` | 545 | 4 ms | 7.3 us |
| `hi` | `--native` binary | 545 | 3 ms | 5.5 us |
| logo.txt 1525 B (256x256 canvas) | binary | 895 | ~710 ms | 0.79 ms |
| logo.txt, steady-state in-process | interp | 929 | ~740 ms | 0.80 ms |

Attribution (200 logo frames, in-process): `next-frame` engine
stepping 76 ms, `ctx-frame-string` ANSI building 75 ms,
`ctx-frame-cells` 12 ms. `sample` profile: 96% in `run_vm`
(interpreter dispatch), ~3% GC marking. A clean 50/50 split: engine
stepping needs numeric kernels (reshape scenes to arrays + hints);
string building needs allocation-light reuse (persistent output buffer
rewritten per frame, not fresh strings). Canvas is 256x256 for any
input (cost scales with canvas, not text) -- sparse/live-cell
iteration is the algorithmic win on top.

## Kernel shaping (per the bench table, not per effect)

`native-skip <fn> <reason>` + profiler dictate a small list of shared
numeric leaves (RNG stream, easing, gradient lerp, per-char math).
Reshape to numbers/SoA, optional `{:hint}`, re-bench, keep or revert.
Table/string engine code stays boxed by design.

## Memory-layout systems (design first, build on benchmark demand)

Unboxed composite `{:shape}` value types (fixed offsets, FFI structs
as precedent), escape analysis / frame-local allocation, inline caches
for the boxed tier. Build only when a bench row shows dispatch-or-GC
dominating after scalar kernels + SoA. See workload plan for detail.

## Resolved notes

- `/tmp/rl-demo/main.c`: hand-written all-native driver (Step 10/12
  harness artifact), not generator output.
- No per-draw-call shims: `ffi/defbind` covers every raylib signature
  since the HFA fix; externs+shim reserved for kernels calling C,
  itself pending profiling proof, preferably via generated bindings.
- macOS/GLFW: font textures uploaded while hidden never land — reload
  after the visible reopen. Framebuffer captures must be post-swap.
- `blshift` with negative counts is not a right shift (`brushift`).
