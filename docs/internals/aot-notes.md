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
  enforced: an int kernel never sees a float temp); `div` needs a
  float return (C integer `/` truncates; int-div policy is TBD).
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
  from numeric purity.
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
dispatch through env with one conversion at the boundary. Build:

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
| int `div` | real division | rejected (use float) | fail-closed |
| non-numeric array elements | error | unchecked unwrap | documented, hardening later |

## 8. CLI reference

```
tools/callgraph.janet FILE... [--emit-native DIR]
                              [--emit-native-lib DIR]
                              [--externs FILE]
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
./build/janet tools/callgraph.janet w.janet --emit-native /tmp/gw
# native 1 fns ... /tmp/gw   (+ native-skip lines for thunk/main)
grep -E "int32_t add_i32" /tmp/gw/native.c
# int32_t add_i32(int32_t p0, int32_t p1) {
cc -O2 -w -fwrapv /tmp/gw/native.c -o /tmp/gw/native
/tmp/gw/native > /tmp/gw/actual.txt && cmp /tmp/gw/expected.txt /tmp/gw/actual.txt
```

Step 13's DCE is the reachability already in the pipeline
(Phase 4 step 2: reachability from entry thunks); Step 11's SoA story is N
`:array` params (positions, velocities, …) through the same
mechanism. `test/suite-callgraph.janet` (105 pins) locks all of
the above: exact C text, parity runs, alias canonicalization,
fail-closed reasons, policy modes, DCE absence.

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
