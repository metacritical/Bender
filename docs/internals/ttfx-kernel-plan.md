# Performance plan: from profiled gaps to Stalin-style kernels

> Historical diagnosis/proposal, reviewed against `5180da8a` on 2026-10-18.
> The measurements and “not started” status below are dated evidence, not new
> benchmark results. Application reshaping proposals are superseded by the current
> compiler-only scope: ttfx remains an unchanged integration fixture. Current work:
> [ttfx-driven-compiler-work.md](ttfx-driven-compiler-work.md).
> References below to “in tree”, `TODO-effects.md`, engine `src/`, and
> `bin/ttfx-cli.janet` refer to the external ttfx repository's `janet-port/`;
> Stalin `source/*.sc` paths refer to that external compiler checkout.

Date: 2026-10-14/15. Status: diagnosis complete, packages scoped, not started.
Machine for all numbers below: Apple M2 Pro, 12 cores, seed 42 unless noted.

Related work in tree: `TODO-effects.md` (37/37 ported, byte-identical vs
Rust ground truth), `src/` engine, `bin/ttfx-cli.janet --bench`.

--------------------------------------------------------------------------------
## 1. The instrumentation runs (Rust vs Janet, same methodology both sides)

Target picked for being the worst case: `rings` on `ruby-port/logo.txt`
(81x10 canvas), 1398 frames, seed 42.

### 1.1 Rust side (`/Users/pankajdoharey/Development/Projects/ML/ttfx`)

Added env-gated profiling to the legacy engine (zero cost when unset):
- `src/engine/ctx.rs`: `EngineCtx::tick` times `motion_move` vs
  `step_animation` separately (`prof_motion_ns` / `prof_anim_ns`);
  `frame()` times `get_formatted_output_string` (`prof_frame_ns`,
  `prof_frames`); `profile_report()` formats totals + per-frame means.
- `src/engine/effect.rs`: `dump_effect` prints the report to stderr when
  `TTFX_PROFILE` is set.
- Run: `TTFX_FX=0 TTFX_PROFILE=1 ./target/release/ttfx --parity-dump
  --seed 42 --input-file ruby-port/logo.txt rings`
  (`TTFX_FX=0` forces the legacy engine; the default fx engine is a
  different renderer and has no hooks.)

Result (1398 frames, 62ms total):

    profile frames=1398 motion_ms=45.1 (72.4%) anim_ms=11.5 (18.4%)
    render_ms=5.7 (9.1%) motion_us/frame=32.3 anim_us/frame=8.2
    render_us/frame=4.1

### 1.2 Janet side (`janet-port/src/scene.janet`, temporarily instrumented)

Same methodology: `ctx-update` accumulated per-tick motion vs animation
time into `(ctx :prof)` (via `os/clock`), `ctx-frame-string` accumulated
render time + frame count. Measured over the full 1398-frame run, then
**reverted** (`git diff` back to baseline, smoke-tested). Caveat: two
`os/clock` calls per char per phase inflates Janet's absolute numbers
somewhat; shares are still comparable.

Result (1398 frames, ~1933ms total):

    profile frames=1398 motion_ms=1408.3 (72.8%) anim_ms=229.9 (11.9%)
    render_ms=295.1 (15.3%) motion_us/frame=1007.4 anim_us/frame=164.4
    render_us/frame=211.1

(Earlier quick probes agreed in shape: 200 `next-frame` = 0.258s with
`frame-string` only 0.027s of it; steady-state 395-actives split gave
motion 5x animation. The full-run numbers above supersede them.)

### 1.3 The comparison (per-frame means, full run)

| Phase            | Rust            | Janet              | Ratio |
|------------------|-----------------|--------------------|-------|
| Motion stepping  | 32.3us (72%)    | 1007us (73%)       | ~31x  |
| Scene animation  | 8.2us (18%)     | 164us (12%)        | ~20x  |
| Frame render     | 4.1us (9%)      | 211us (15%)        | ~53x  |
| **Total**        | **44.6us**      | **1383us**         | **~31x** |

### 1.4 Where we were going wrong (findings)

1. **Same shape, uniform gap.** Both engines are motion-dominated at
   ~3/4 of frame time. The port is faithful even in *where* it burns
   time. The ~31x is interpreter overhead spread evenly, not one
   pathological algorithm. No single rewrite fixes it; it is a kernel
   program across all three phases.
2. **Render is the worst ratio (53x), not motion.** 211us vs 4us on
   formatting: buffer pushes, color-escape encoding (`color-string`),
   per-cell dispatch, space-run/gap logic in the interpreter. A
   formatting fast-path (or kernel) belongs alongside motion work.
3. **Motion is the biggest absolute prize (73% of time).** Per active
   char per tick: bezier/lerp math, easing eval, segment walking, all
   boxed. Rust: ~81ns/active. Janet: ~1.5us/active. Pure numeric f64
   work — ideal kernel material.
4. **Earlier misread, corrected:** first Janet probes used bulk passes
   over a fixed 395-active snapshot (different denominators from Rust's
   run-long averages), so the first table lacked Janet per-frame
   figures. Re-measured with identical accumulate-over-the-run
   methodology; the table above is apples-to-apples.
5. **Not the cause (ruled out):** build cost (51ms once), canvas size
   handling, gradient construction (amortized), RNG, GC pauses as a
   single spike — none dominate. Engine-step rate is the entire story.

--------------------------------------------------------------------------------
## 2. Why Stalin is fast (source: /Users/pankajdoharey/Development/Projects/stalin)

Stalin 0.11 (Siskind/Purdue), ~66k lines of Scheme: a batch-mode,
closed-world, whole-program optimizing Scheme-to-C compiler. No REPL,
no eval/load at runtime — and in exchange it sees everything.

### 2.1 Pipeline (driver: stalin.sc, orchestration: source/stalin5e.sc)

- `source/stalin1.sc` — IR data model (expression/result/type/type-set/
  variable/environment/call-site structs, globals).
- `source/stalin2.sc` — reader utils + macroexpander to core forms.
- `source/stalin3a.sc` — flow analysis (0CFA-style type inference),
  call graph, polyvariant cloning policy.
- `source/stalin3b.sc` — closure conversion, selective CPS, escape
  analysis, allocation choice (stack/region/heap), closed-world pruning.
- `source/stalin4d.sc` — ~100 per-primitive flow+codegen handlers
  (each with compatible?/propagate!/promote!/compile-call).
- `source/stalin4b.sc` — value motion (`move-*`, `widen`), allocation,
  arithmetic emitters.
- `source/stalin4c.sc` — whole-program C assembler + `compile` dispatch.
- `source/stalin4a.sc` — C AST constructors, arch params, representation
  promotion, copy propagation, dead-code sweep.
- `source/stalin5a/5b/5c/5d.sc` — R4RS macros, Scheme+C runtime preludes,
  dialect packs. Build via `make-single-file`, self-hosts
  (`gcc -O3 stalin.c -lgc`).

### 2.2 Types: two levels, fixpoint to monomorphism

- `u` = one type, `w` = type-set (red-black set of `u`s;
  `stalin1.sc:212-218`). Every expression and variable carries a `w`.
- Immediates are symbols (`fixnum flonum char …`); heap types are
  records (`pair` = 2-slot `structure`, vectors with element `w`,
  per-environment closures); recursion via slots; union = set
  membership (`can-be?/must-be?/monomorphic?`, `stalin1.sc:3285-3337`).
- Inference = abstract interpretation to fixpoint: `perform-flow-
  analysis!` drives `infer!` per expression kind + `propagate-call!`
  across call sites + per-primitive rules; all updates monotone
  (`assert-member!/assert-subset!`), worklist re-runs parents/callers
  (`stalin3a.sc:1436-2040`); backward narrowing from predicate tests;
  then `apply-closed-world-assumption!` merges equal-shape types
  (`stalin3b.sc:1649`). Result: most expressions hold exactly one
  concrete type, with no declarations.

### 2.3 Representation selection: the box→unboxed flattening

Decided per type-set (`c:type-set`, `stalin4a.sc:2175`), first match wins:
fictitious (no storage at all) → monomorphic (unboxed raw C:
`fixnum`→`long`, `flonum`→`double`, `type-switch` with `must-be?` emits
zero dispatch) → tag-only → squeezed (one real member + tag/range
tests) → squished (single word, alignment tags) → general (only then:
`struct {tag; union}`).
- The flattening is not a pass; it falls out of "narrow type ⇒ narrow
  layout, convert only at boundaries" (`widen-type` re-boxes once where
  a polymorphic consumer meets monomorphic producers).
- Pairs = heap structs whose **slots unbox independently**; closures
  keep only escaping free vars (linked parent structs, never whole
  frames); non-escaping/inlined procedures are fictitious (no object).
- Example (`benchmarks/tak.sc`): all-fixnum inference turns `(- x 1)`
  into raw `aN = (x - 1)` on C locals. The `{fixnum,flonum}` version
  gets one specialized tag-switch with both arms raw.

### 2.4 Codegen, closures, lifetime, GC

- One C function per surviving environment (`f<index>`); calls resolve
  to direct C calls (call graph known); single-call-site procedures
  inline entirely; self-calls become `goto`; tail calls via global
  parameter passing (no stack growth).
- Lifetime = `escapes?` over-approximation (`stalin3b.sc:127`); deepest
  safe environment wins; `stack`→`alloca`, region→bump-pointer frames
  freed on exit, `heap`→Boehm (vendored `gc6.8.tar.gz`, linked only if
  heap survives; incremental collection deliberately disabled).
- DCE falls out of use analysis (uncalled environments dropped, unused
  vars go fictitious, C-level unused-decl/label sweep).
- Ranked reasons it is fast: (1) monomorphic inference + unboxed
  selection (checks/dispatch vanish), (2) GC avoided on hot paths
  (stack/region), (3) closure elimination (inlining, local/global/
  hidden classification, zero-alloc calls).

--------------------------------------------------------------------------------
## 3. What we already do like Stalin (similar efforts)

| Stalin piece                | Our counterpart                                  |
|-----------------------------|--------------------------------------------------|
| Closed world (whole program)| `--aot` image is whole-program by construction   |
| Numeric kernels             | `--native` kernels (nbody 27x, spectral 64x)     |
| Profile-guided specialization| PGO collect → parity-gated kernelization        |
| Output must not change      | `--pgo` byte-identical oracle gate                |
| Whole-program visibility    | image build sees all sources (imports merged)     |

## 4. Where the gaps lie and how to fix them (ordered)

1. **Whole-program type fixpoint, no hints.** `infer.janet` walks
   functions; extend it across the image boundary to fixpoint
   (monotone type-set widening + worklist, Stalin `stalin3a.sc`
   pattern). Kills the `:hint` requirement for the common monomorphic
   case — Stalin proves hints are unnecessary when the whole program
   is visible, and `--aot` always sees it. First customer: motion loop
   (31x measured above).
2. **Representation selection at emission.** Generalize the kernel
   gate from "annotated loops" to "inferred-narrow expressions":
   monomorphic-numeric ⇒ raw `double`/int64 locals, convert
   (`widen`-equivalent) only at polymorphic boundaries. Targets in
   measured order: motion stepping, then frame formatting (53x worst
   ratio: buffer pushes + color escapes), then scene stepping.
3. **Escape analysis for hot paths.** Even a cheap local version (does
   this allocation outlive this function?) converts frame temps to
   stack slots and removes per-tick GC contacts — Stalin's #2 speedup;
   our profile shows GC-adjacent cost spread across all phases.
4. **Closure classification (local/global/hidden/slotted).** Our
   closures capture whole envs (drives spawn/marshal/sweep cost);
   local-vs-escaping distinction shrinks captures, messages, sweeps
   together.
5. **Keep the oracle green through all of it.** Representation changes
   are exactly what silently alters output; the `--pgo` parity gate
   plus the 37-effect byte-parity suite (`TODO-effects.md`) must pass
   per step. The 37-gate + Rust `--parity-dump` differential is the
   acceptance harness for every kernel.

Boundary: kernel emission lives with the compiler side; janet-port's
part is hot-loop reshaping onto numeric arrays + PGO workloads per
effect (motion-heavy: rings, matrix, swarm first).

--------------------------------------------------------------------------------
## 5. Acceptance criteria

- `rings` logo bench improves with zero byte-change vs Rust
  `--parity-dump` (same seed/inputs); then matrix, swarm, same gate.
- `make test` + 37-effect suite green per commit; PGO parity green.
- Long-term direction of travel: per-frame gap 31x → under 2x of Rust
  (`44.6us` vs `1383us` today), attacked in the order of §4.
