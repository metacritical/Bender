# ttfx lessons → Bender mechanisms

Every optimization that worked on the ttfx port maps to a general
compiler/VM feature. This note is the bridge: each lesson, the
universal mechanism, where it lands, and the gate. Goal: after
compilation, Janet programs approach Rust throughput without giving
up dynamism. Measured anchor points (46x190 decrypt, M2 Pro): stock
174 fps -> port shaping 547 fps (3.15x) -> Rust 470k fps. The
remaining ~1000x is per-cell boxing + dispatch on the stepping path.
Port-level shaping cannot fix that for all programs; the mechanisms
below can.

## L1. Typed unboxed arrays (from: SoA grid, 1.37x)

Lesson: replacing a hash table with dense parallel arrays was the
single biggest structural win per line of code. Today Janet makes you
hand-roll parallel `@[]` columns, and kernels see them as opaque
`:array` (boxed slots).

Mechanism: **typed array constructors** in the VM —

    (ta/new :double n)        # n doubles, one GC-managed block
    (ta/get a i) (ta/put a i v)
    (ta/length a)

- Layout: one malloc block + header, elements raw `double`/`int64_t`.
  GC: one object, numeric fields need no marking (same rule as
  Level 3; strings/pointers inside typed arrays are a later, traced
  extension).
- Kernel side: `:array`-like param that lowers to a raw pointer —
  `get`/`put` become one indexed load/store (today they pay boxed
  slot round-trips). This is the Rust `Vec<f64>` shape, exposed
  dynamically.
- Files: `src/core/array.c` (or new `ta.c`), `tools/callgraph.janet`
  (param type `:ta-double` etc.), wrapper unboxing.
- Gate: ship when a bench loop shows kernels bound by slot round-trip
  rather than arithmetic (already true for the SoA port loops).

## L2. Import-aware kernel analysis (from: "PGO added nothing")

Lesson: the kernelizer only saw the entry file; engine modules
compiled into the thunk were invisible, so PGO had no candidates.

Mechanism: the preimport pass already knows every file it loaded.
Thread those paths into analysis + registration automatically
(`janet-aot.sh --include-imports`, or always-on with an opt-out).
Then PGO seeding + widening apply to *all* program code, not just the
entry file.

- Files: `tools/callgraph.janet` (register per-file defs from
  preimport record), `tools/mkimage.janet` (strip native names in
  imported files too), `tools/janet-aot.sh` (flag).
- Gate: ttfx rebuild emits easing/anim kernels without source
  reshaping.

## L3. Inline caches on keyword access (from: per-cell field reads)

Lesson: `(a :cur-row)` per cell per frame is hash+probe+tag-check;
at 8,740 cells x 60 fields that dominates boxed code.

Mechanism: monomorphic inline caches in `vm.c` — `get` with a
constant keyword caches the last table-shape -> slot offset at the
call site; subsequent hits skip the hash. 2-5x on boxed field access,
benefits ALL dynamic code with zero source changes. Precedent: every
mainstream VM does this for monomorphic property access.

- Files: `src/core/vm.c` (JOP_GET path), `src/core/table.c`
  (shape/chain hints), gc invalidation on table mutation.
- Gate: profile shows keyword `get` dispatch as top row after L1/L2.

## L4. Frame-local allocation (escape analysis lite) (from: buffer reuse, 2.3x)

Lesson: per-frame immortal strings were the single biggest garbage
source; one reused buffer fixed it. The general mechanism is proving
"this value dies in this frame" and stack/reuse-allocating it.

Mechanism, staged:
  a. Library-level now: encourage `@""` accumulation + `(string buf)`
     at the boundary (what ttfx does). Document as the idiom.
  b. Compiler: escape analysis for strings/buffers built and consumed
     in one function -> stack temp or caller-provided reusable buffer
     (calling convention: hidden out-param). Level 4 of
     memory-layout.md; build only when allocation still dominates a
     bench after L1-L3.

## L5. Level 3 shapes (from: chars-as-boxed-tables)

Unchanged from memory-layout.md, now with measured demand: hot records
(Engine chars) become declared numeric composites via the
`ffi/defstruct`/`:kernel` machinery; numeric-only fields need no GC
tracing. Sequenced AFTER L1 (typed arrays give 80% of the win for 20%
of the work — L3 adds field-name ergonomics and per-record layout).

## Order and gates

1. L2 (import-aware analysis) — smallest effort, unlocks everything
   else for real programs; no VM changes.
2. L1 (typed arrays) — the structural Rust-gap closer.
3. L3 (inline caches) — VM change, biggest boxed-code multiplier.
4. L5 (shapes), L4b (EA) — only if a post-L1-L3 bench still demands.

Success metric: the ttfx bench table (byte-identical) climbing toward
Rust numbers from *unmodified* engine sources — every mechanism here
is judged by that bench, not by its own elegance.
