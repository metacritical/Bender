# AOT pipeline: implementation notes

Companion to the roadmap in `docs/internals/aot-plan.md` (which
records *what* was built and *why*). This document records *how it
works*: the hint surface, the width system, opcode coverage, externs,
hybrid binaries, and the parity methodology — with examples.

Pipeline shape: Janet source → parse → macroexpand/compile (macros
run as today) → disassemble → `tools/infer.janet` (hybrid
inference: hint contracts + call-site widening + fixpoint) →
`tools/callgraph.janet` (registration, reachability, flow-check,
C emission) → `cc` → native binary or hybrid library. Nothing is
trusted until the parity harness agrees byte-for-byte
(`expected.txt` vs `actual.txt` via the janet oracle).

## 1. Hint surface

Three spellings, one meaning (canonicalized at extraction):

```janet
(defn hyp ^double [a ^double b] (+ (* a a) (* b b)))
(defn hyp ^:double [a ^:double b] (+ (* a a) (* b b)))   # ^:keyword too
(defn hyp {:hint {:params [:double :double] :returns :double}} [a b]
  (+ (* a a) (* b b)))
```

- `^type` before the argvector hints the return; `^type` before a
  parameter hints that parameter (positional; unmarked parameters
  read as `:number`).
- Hints are direct contracts: inference never weakens them, and the
  emitter trusts them (a lying hint is a wrong kernel — fail loud in
  parity, not silent).
- No hint (or `:number`) keeps the function boxed.

Width vocabulary (canonical forms; 64-bit is the default integer):

| Hint | C type | Notes |
|---|---|---|
| `:long` | `int64_t` | default integer |
| `:i32` `:i16` `:i8` | `int32_t` … | opt-in narrow, exact widths emitted |
| `:u64` `:u32` `:u16` `:u8` | `uint64_t` … | unsigned family |
| `:double` | `double` | default float |
| `:float` | `float` | **true float32 arithmetic** (see divergences) |
| `:array` | `JanetArray*` | boxed-number arrays, Step 11 (below) |

Aliases fold at extraction (`:i64`/`:int64`→`:long`,
`:int`/`:int32`→`:i32`, `:f64`→`:double`, `:f32`→`:float`,
`:uint*` similarly). Inference joins widths through family
normalization (`:i32`→`:long`, `:float`→`:double`); mixed widths
widen conservatively to `:number`.

## 2. Int-overflow policy (`--int-policy wrap|promote`, default wrap)

- `wrap`: kernels compute in exact int widths; shipped and harness
  builds pass `-fwrapv` (defined two's-complement wrap — C signed
  overflow is otherwise UB).
- `promote`: int widths compute as `double` (oracle-identical
  everywhere; exact parity by construction), reboxed via
  `wrap_number`. Extern C declarations keep exact foreign signatures.
- Boxed janet truncates at 2^53 in both modes — native-vs-boxed
  divergence past 2^53 is by design, now explicit and flagged.

## 3. Opcode coverage (fail-closed)

A def emits only when params + return are concrete (hinted or
inference-proven), non-vararg, and every op in its body translates.
Anything else skips **with a reason** (`native-skip <id> <name>
<reason>` — the add-a-hint diagnostics):

- Arithmetic: `add/sub/mul/div` + immediates (int-family purity
  enforced: an int kernel never sees a float temp). Division-family
  soundness is checked on the OPERANDS, never just the return type
  (int64 operands truncate even inside a `:double` fn — the ttfx
  `floor_div` miscompile that motivated the rule): `div`/`divim` need
  float operands; `divf`/`mod`/`rem` need float operands, or
  PGO-nonneg int operands (below).
- `divf`/`mod`/`rem`/`divim` (2026-10-09): `divf` is `floor(a/b)`,
  `mod` is `b==0?a:a-b*floor(a/b)`, `rem` is `fmod(a,b)` — exact
  libm matches of boxed. Int widths refuse (C `%` truncates, Janet
  floors) unless PGO-unanimous `min >= 0` on unrebound params, in
  which case trunc == floor; the int path guards the whole nonneg
  domain fail-loud (`b <= 0 || a < 0` aborts — PGO evidence is not
  proof). Reserved C names (`fmod`, …) emit with a `k_` prefix:
  defining `fmod` is UB (the compiler folds calls with builtin
  semantics — observed `-1` instead of floor-mod `0.5`).
- PGO-nonneg ranges (2026-10-09): the collector tracks per-param
  min/max (`:mins`/`:maxs` in dumps); unanimous `min >= 0` seeds
  nonneg evidence for unrebound params. Never a contract — guards
  stay in the emitted code.
- Call-shaped access (2026-10-09): `(a i)` compiles to push+call
  with the array as callee, invisible to `get`-based rules. A
  single-index call into a proven `:array`/`:tuple` slot refines
  like `get` and lowers to `->data[]` with the same bounds abort
  (multi-arg calls refuse). Resolved once per site before the state
  update (dest slots routinely reuse index temps).
- Pure core calls (2026-10-09): `math/floor|ceil|sqrt|abs|pow`
  resolve from ldc-loaded cfunction constants and lower to libm
  with `(double)` casts (boxed bodies ARE these calls). Arity and
  numeric-args verified; `ldc`-of-cfunction skips emission (dead
  callee load, same discipline as `ldu`).
- Polymorphic `:number` returns code-gen as `:double` (boxed
  arithmetic IS double arithmetic; int-domain ops keep their own
  checks). Callers still see `:number`, so native-to-native calls
  into such fns refuse (width unification is future work).
- Counted loops (2026-10-09): `for i 0 (len arr)` loops elide the
  `>= count` half (init-0 + single-+1-increment + dominated
  `lt`-exit + same-array bound + access-before-increment + no count
  mutation, all positional). `< 0` stays as defense-in-depth.
- Branches: `if`/`goto` via CFG + dataflow join; conditions must be
  `:bool` (janet truthiness makes `0` truthy — C wouldn't).
- Loops: back edges + loop-carried slots (`lte`/`gte` supported).
- Comparisons: full `lt/gt/eq/neq` family (+ immediates).
- Moves: `movn`/`movf` (cross-tier copies).
- Calls: direct calls to other emitted natives (multi-round
  fixpoint); tail calls; declared externs (below).
- Arrays (`:array` params): `len` → `->count`; `get`/`put` →
  `->data[]` with an explicit bounds guard that aborts (janet
  OOB-`nil` has no unboxed form). Rules: base must be `:array`,
  index int-family, `put` value numeric, returns stay numeric.
  Kernels never retain pointers (caller-rooted for the call), so
  zero collector work. Pointer temps (`JanetArray*`) are exempt
  from numeric purity. Counted `for i 0 (len a)` loops elide the
  upper half (above).
- Dead code never reaches emission (reachability from entry thunks;
  uncalled defns are simply absent from the output).

## 4. Extern C calls (`--externs FILE`)

```janet
# decls.janet
(def externs
  {"game-clamp" {:c "game_clamp"
                 :params [:double :double :double]
                 :returns :double
                 :fallback (fn [x lo hi]
                             (if (< x lo) lo
                               (if (> x hi) hi x)))}})
```

- The declared C symbol is called directly from native code; the
  declaration (with `:c` name + exact types) is hoisted into the
  output header.
- `:fallback` pre-binds for compilation and serves as the parity
  oracle: fallback ≡ C implementation, byte-identical, verified by
  linking a real C file into the harness (the raylib story).
- `void` returns supported (statement calls, result discarded);
  `void*` params stay boxed/shim-side. Signature mismatches (arity,
  spread, return mismatch) fail closed with reasons.

## 5. Hybrid binaries (`--emit-native-lib`, `janet-aot.sh --native`)

`emit-native-lib` emits kernels + `janet-cfunction` wrappers
(unbox → kernel → rebox with per-width boundary casts) +
`native_init(JanetTable*)` registering them. `mkimage --native`
strips proven defns from the unit and prepends a prelude rebind, so
same-unit call sites compile against the wrappers (native stays
native-to-native internally) while boxed code and `eval`'d mod code
dispatch through env with one conversion at the boundary. The lib
path prints the same `native-skip` diagnostics as `--emit-native`
(real builds go through `--emit-native-lib`, and silent skips used
to hide all signal). Build:

```sh
tools/janet-aot.sh game.janet --native -o game   # needs a C compiler
```

The boxed tier (embedded VM) keeps running dynamic code; eval and
dynamic residue keep working — verified dispatching through native
kernels. Known boundary: truly dynamic aliasing of a kernel value
boxed-side keeps the boxed copy (correct, just interpreted).

## 6. Harness and parity methodology

`--emit-native DIR` writes `native.c` (kernels + a `main` that calls
each kernel over a sample grid) and `expected.txt` (same calls
through the janet oracle by `eval`ing the original defns):

- Scalar samples: ints from `[-3 -1 0 1 3]`, floats from
  `[-2.5 0.5 1.5 2.5]`; int kernels get integer samples only.
- Array kernels cycle two fixed variants (`[1.5 2.5 3.5]`,
  `[4 5]`); int params in array kernels sample `[0 1]` (variants
  have length ≥ 2; OOB aborts by design).
- Compile the harness with `cc -O2` (`-fwrapv` for int kernels,
  `-Isrc/include -Isrc/conf` + `build/janet.o` when array kernels
  pull `janet.h` layouts/macros), diff actual vs expected —
  byte-identical or it doesn't ship.

## 7. Divergences catalog (all opt-in, all documented)

| Case | Boxed | Native | Status |
|---|---|---|---|
| `:float` arithmetic | float64 | true float32 | opt-in contract |
| int past 2^53 | truncates | wraps (`wrap`) / exact (`promote`) | policy flag |
| array OOB `get`/`put` | `nil` | aborts | opt-in contract |
| int `div` | real division | float operands only; PGO-nonneg ints via guarded trunc | fail-closed + opt-in guard |
| non-numeric array elements | error | unchecked unwrap | documented, hardening later |
| PGO-nonneg domain exit | trunc==floor | aborts | opt-in contract (evidence, not proof) |

## 8. CLI reference

```
tools/callgraph.janet FILE... [--emit-native DIR]
                              [--emit-native-lib DIR]
                              [--externs FILE]
                              [--profile FILE]
                              [--include FILE]...
                              [--int-policy wrap|promote]
tools/janet-aot.sh app.janet [-o app] [-c] [-S] [-E args...] [--native]
```

## 9. Worked example

```janet
# w.janet
(defn add-i32 {:hint {:params [:i32 :i32] :returns :i32}} [a b] (+ a b))
(defn main [] (print (add-i32 2 3)))
```

```sh
./build/bender tools/callgraph.janet w.janet --emit-native /tmp/gw
# native 1 fns ... /tmp/gw   (+ native-skip lines for thunk/main)
grep -E "int32_t add_i32" /tmp/gw/native.c
# int32_t add_i32(int32_t p0, int32_t p1) {
cc -O2 -w -fwrapv /tmp/gw/native.c -o /tmp/gw/native
/tmp/gw/native > /tmp/gw/actual.txt && cmp /tmp/gw/expected.txt /tmp/gw/actual.txt
```

Step 13's DCE is the reachability already in the pipeline
(Phase 4 step 2: reachability from entry thunks); Step 11's SoA story is N
`:array` params (positions, velocities, …) through the same
mechanism. `test/suite-callgraph.janet` (171 pins as of 2026-10-09)
locks all of the above: exact C text, parity runs, alias
canonicalization, fail-closed reasons, policy modes, DCE absence.

## 10. FFI struct fixes that AOT GUI work required (2026-10-07)

- **AAPCS64 HFAs** (`src/core/ffi.c`): all-float struct args take one
  FP register per element (was: one packed register, x86-64 SysV
  behavior); HFA returns parsed element-wise from the FP return slots.
  Without this, any float-composite argument silently mis-marshalled
  (raylib `DrawTextEx`/`MeasureTextEx` drew nothing). Pins:
  `v2/v3/v4` add, HFA return, mixed int/string/HFA/float signature in
  `examples/ffi` (fail on unfixed core, pass on fixed); `DrawTextEx`
  renders byte-identically to pure C.
- **ffi-struct marshalling**: `core/ffi-struct` descriptors now
  marshal (field count/align/size/is-aligned + per-field
  prim/array-count/offset, nested recursion; layout restored verbatim,
  GC-safe reconstruction). Without this, any program using
  struct-typed `ffi/defbind` fails image compilation. Enables the GUI
  `--native` binary. Type registered for unmarshal lookup.
- **Multi-module AOT** (`tools/`): script-dir syspath, scoped
  `:current-file`, import pre-execution mirroring loader ordering,
  import-form stripping from images, `(dyn :args)` install in the
  harness, extern-decl hoisting for `--native`.

## 11. 2026-10-08/09 batch (ttfx-driven)

`bender --aot bin/ttfx-cli.janet --native` emitted 0 kernels with no
diagnostics; now 4 (`k_next_word_bang`, `color_clamp`, `floor_div`,
`bitlen`) with reasons for all 148 skips. Landed, in order:

1. AOT cc flags `-O3 -fwrapv -fno-math-errno
   -fno-trapping-math` (`boot.janet`; `-fwrapv` is a correctness
   fix for `:wrap`).
2. VM `JANET_LIKELY/UNLIKELY` on ALU fast paths + GC polls
   (`vm.c`; ttfx interp 547 → 637 fps).
3. `bench/run.janet` micro-harness (ALU/array/table-get/alloc).
4. `divf`/`mod`/`rem` float emission + `k_` reserved-name prefix
   (fixed a real miscompile: folded `fmod` gave `-1` vs `0.5`).
5. `divim` emission + lib-path skip diagnostics.
6. Call-shaped `(a i)` recognition (inference + `->data[]`
   lowering + per-site snapshot for slot reuse) and `:number`
   returns code-gen as `:double`.
7. Division operand soundness: checks on operands, not rt (fixed a
   real `floor(int64/int64)` truncation miscompile).
8. PGO min/max collection + nonneg-seeded int `divf`/`mod`/`rem`
   with fail-loud domain guards.
9. Pure `math/floor|ceil|sqrt|abs|pow` → libm (+ `ldc`-cfunction
   skip); div-family gates on operands, not rt (unlocks
   array-building fns' numeric subcomputations).
10. Compiler `(string "a" "b")` literal folding (`compile.c`,
    pointer-compared, shadowing-safe).
11. Counted `for i 0 (len a)` upper-guard elision (`< 0` kept).

Still refused on ttfx (next): constructor emission (`mkarr` in
`seed-limbs`), live-range splitting for shared slots (`xor64`
`s2 mixes array/long/number`), dependency chains (`rotl64`).

## 12. 2026-10-09 batch, part 2 (benchmarksgame-driven)

`bench/benchmarksgame/` ports n-body, spectral-norm, fannkuch-redux
against the published game (byte-exact outputs at N=1000/100/7).
Driving real programs through the pipeline exposed and fixed:

- **Join-aware declarations.** A reused slot must declare a type
  holding every write; last-wins truncated (`energy` declared a
  bool/double-shared temp `int`, zeroing `0.5*m`). Resolution:
  any float write → double; 64-bit sign mixes refuse; int mixes
  widen; singletons exact. Dead callee values (`ldc`-functions,
  `ldu`) don't poison the join; flow still gates live uses.
- **No put downgrades.** Stores preserve container type; element
  risk is runtime-gated by unwrap; unprovable stores refuse at
  emission with reasons.
- **Numeric indices** with fail-loud integrality guards
  (`:number` slots abort on fractions, as boxed errors).
- **Mixed division** via `(double)` casts with dest checks (boxed
  div is always real, even `(/ 1 2)` — the compiler folds integral
  `1.0` to `ldi`).
- **Cross-return native calls** with exact arg-conversion checks
  (`int->float` ok, `float->int` refuses); tail calls keep exact
  match.
- **Oracle globals**: the harness binds top-level self-evaluating
  `def`s before sampling; eval failures skip per-fn (fail-closed).
- **Per-sample fixtures**: in-place kernels mutated shared fixtures
  across samples — a pancake loop over contaminated duplicates
  cycled forever. Both sides now reset per sample (aliasing
  preserved within a sample; tuples immutable).
- **Const-global propagation** remains open: kernels can't read
  globals (`NB`, `SOLAR-MASS`) — benchmarks use literals (matching
  C `#define`), real fix is value-tracking `ldu` (specified).

Measured (M2 Pro, this repo's `build/bender`):

| program | interp | native | ref C | ref Rust |
|---|---|---|---|---|
| n-body 50M | — (est. ~150s) | **6.0s** exact | 2.10s | 2.19s |
| spectral-norm 5500 | 64.4s | **1.85s** exact (35×) | — | — |
| fannkuch-redux 12 | — (est. ~10min) | **162s** exact | 2.14s | 3.81s |

n-body sits at Go/Java-naot level (6.4s/6.0s), 2.9× off C, with the
whole hot loop (`advance`, `energy`) kernelized. fannkuch needs
whole-loop native (rotation stays boxed + per-permutation wrapper
overhead) — see next steps in `extreme-perf.md`.

## 13. Auto-PGO: on-demand collection, automatic recompilation

Manual `--pgo` (collect with workload, widen, gate, install) works
but needs expert plumbing per build. The automatic loop closes it with no
manual intervention after a one-time workload:

- **Runs collect on demand.** AOT binaries (generated `main`) collect a
  type profile into the standard cache when no profile exists yet;
  `JANET_AUTO_PGO=1` forces additional runs into unique sidecars rather
  than replacing cached evidence. Never when `JANET_NO_PROFILE` is set;
  an unwritable cache silently runs clean. Collection costs ~10%
  (16-line direct-mapped entry cache, exact type masks; min/max and
  element evidence sampled 1-in-8 after a 16-call warmup).
- **Trivial runs don't clobber.** Auto dumps additionally require a
  minimum call count (`JANET_AUTO_PGO_MIN_CALLS`, default 10000), so
  `--help` runs never overwrite a useful profile. Explicit dumps
  always write.
- **Cache layout and accumulation.** `$XDG_CACHE_HOME/janet/pgo/<key>.{jdn,workload,stdin}`,
  key = sanitized binary basename (same algorithm in C and Janet).
  Forced binary runs claim `<key>.jdn.run-<pid>-<attempt>.pending`
  and publish `.jdn` sidecars only after the dump finishes.
  Binary sidecars have no replayable workload identity, so remain diagnostic:
  they are **not** merged into a gateable cache. Successful explicit `--pgo`
  creates a versioned workload record and immutable stdin snapshot. Repeated
  explicit runs with identical arguments and stdin bytes accumulate calls,
  union types, and widen ranges; changed workloads start fresh. Legacy pairs
  without that record are ineligible for auto-application. The cache transaction
  is serialized; a busy cache falls back to unoptimized rather than reading
  partially published evidence.
- **Plain builds auto-apply.** `bender --aot app --native -o app`
  (no flags) uses a fresh profile (newer than all sources) +
  recorded workload and captured stdin automatically: fresh oracle run, seeded build,
  same parity gate. Gate failure falls back to unoptimized with a
  warning (never breaks the build); `--pgo` still dies on mismatch.
  `--no-auto-pgo` opts out; missing/stale pieces print what to do.
- **Size builds.** `bender --aot app.janet -Oz -o app` and
  `bender --aot app.janet --native -Oz -o app` request compiler size
  optimization, function/data sections and platform dead-section removal,
  then strip debug symbols from a staged binary before installation. `-Oz`
  requires a binary build (not `-c`, `-S`, or `-E`); unsupported compiler
  flags or targets fail instead of silently producing a normal build.
  On macOS arm64, a 1,258,312-byte plain fixture became 1,028,464 bytes
  (fixture-specific, not a general compression guarantee). This flag does
  not itself disable runtime profile collection or replace PGO parity.
- **Locked-down targets.** `-DJANET_NO_PGO` (iOS: no JIT/dynamic
  codegen) compiles the binary-side mechanism out entirely; explicit
  profiling APIs keep working.
