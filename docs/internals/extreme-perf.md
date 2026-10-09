# Extreme performance measures — the full list

Everything we can do to close the gap to Rust, roughly ordered by
perf-per-risk. Checked items ship; unchecked items are staged behind
bench gates (bench/run.janet + ttfx byte-identical table). Goal is not
elegance — it is matching `rustc -O` assembly on hot loops while
keeping every Janet program working.

Baseline (M-series, interpreter): rng-double ~28ns/op, array-sum
~13ns/elem, table-get ~16ns/op, alloc-tuple ~64ns/op. Profile: 96%
`run_vm` dispatch, 3% GC. The remaining gap to Rust on numeric code is
~1000x on the stepping path (per-cell boxing + dispatch).

## Tier 0 — done, measured, keep

- [x] Computed-goto threading (`vm.c:run_vm`). Switch dispatch
  mispredicts; threading is strictly better on GCC/Clang.
- [x] `JANET_LIKELY/UNLIKELY` on ALU fast paths + GC polls (`vm.c`).
  ~1-3% on numeric loops, free (ttfx interp 547 → 637 fps).
  Next: annotate `fiber_push`, `table_get` probe loop, `symcache`
  hit path.
- [x] AOT cc flags: `-O3 -fwrapv -fno-math-errno
  -fno-trapping-math` (`boot.janet`). `-fwrapv` is a CORRECTNESS fix
  for `:wrap` int policy (signed overflow is UB otherwise); the rest
  is the safe fast-math subset (no reassociation, oracle-identical).
  Never `-ffast-math` (breaks parity on NaN/-0/div-by-zero).
- [x] Fail-closed native emission + oracle (`janet-diff.janet`,
  PGO parity gate in `bender --aot --pgo`). Every optimization below
  must keep byte-parity or refuse loudly (`native-skip`).
- [x] `divf`/`mod`/`rem` float-kernel emission (`callgraph.janet`).
  Exact boxed semantics: `floor(a/b)`, `b==0?a:a-b*floor(a/b)`,
  `fmod(a,b)`. Int widths refuse (C `%` truncates, Janet mods floor —
  diverge on negatives).
- [x] Reserved-C-name prefix (`k_` in `emit-c-name`). Defining
  `fmod` is UB: the compiler folds `fmod(-2.5,1.5)` with builtin
  semantics (-1) instead of calling the kernel (0.5). Any libm/libc
  name or keyword collides the same way.
- [x] `bench/run.janet` micro-harness: rng-double ~28ns/op,
  array-sum ~13ns/elem, table-get ~16ns/op, alloc-tuple ~64ns/op.
- [x] `divim` emission + lib-path skip diagnostics (the lib path
  printed nothing; real builds were undiagnosable).
- [x] Call-shaped `(a i)` access: inference + `->data[]` lowering
  (per-site snapshot: dest slots reuse index temps) +
  `:number`-as-`:double` returns (native-to-native into such fns
  still refuses — width unification is future work).
- [x] Division operand soundness: checks on operands, never rt
  (fixed a real `floor(int64/int64)` truncation miscompile on ttfx
  `floor_div`; stock `div` had the same hole).
- [x] PGO min/max collection + nonneg-seeded int `divf`/`mod`/`rem`
  with fail-loud domain guards (evidence, not proof).
- [x] Pure `math/floor|ceil|sqrt|abs|pow` → libm (+ `ldc`-cfunction
  skip); div-family gates on operands so array-building fns'
  numeric subcomputations emit.
- [x] Compiler `(string "a" "b")` literal folding (`compile.c`,
  pointer-compared against the core binding, shadowing-safe).
- [x] Counted `for i 0 (len a)` upper-guard elision (`< 0` kept as
  defense-in-depth; nearest-writer-before-use for reused temps).

## Tier 1 — VM dispatch (boxed code, benefits everything)

- [ ] **Inline caches on `JOP_GET` with constant keyword** (L3). One
  cached shape->offset per call site; hits skip hash+probe. 2-5x on
  boxed field access, zero source changes. Invalidate on table
  mutation (shape version counter in `table.c`). Start monomorphic
  (1 entry); go polymorphic (4) only if megamorphic sites show up.
- [ ] **Specialize `JOP_GET_INDEX`/`PUT_INDEX` for arrays.** Type-guard
  once per site instead of per access.
- [ ] **Superinstructions:** fuse `PUSH+CALL`, `GET+ADD`, `JMP_IF`
  pairs the profiler flags. Cuts dispatch count 10-20% on call-heavy
  code. Needs `bytecode.c:optimize` patterns + `vm.c` arms.
- [ ] **Threaded-codeforce `vm_checkgc` out of the fast path.** GC
  poll only on back-edges and calls, not every `MAKE_*`. Already
  partially true; audit each `vm_checkgc_pcnext` site.
- [ ] **Register pinning:** keep `stack`/`pc` in real machine registers
  (`register` keyword + `-O3` does most of it; verify with disasm of
  `run_vm`).

## Tier 2 — value representation + allocation

- [ ] **Bump-allocator nursery.** `janet_gcalloc` today does
  slab-or-malloc + 3-way list prepend + counters per object. Add a
  thread-local 256KB bump region: fast path = 3 instructions
  (pointer += size, compare, store). Fall back to slab on overflow.
  Kills most allocation overhead in `MAKE_*`/`fiber_push`.
- [ ] **Stack/frame-local allocation (escape analysis lite, L4).**
  Values proven dead-in-frame (frame strings, temp tuples) go to C
  locals or a reused frame buffer: zero GC pressure. Cheaper cousin
  ships first: hand-reuse a persistent buffer (already the documented
  idiom); build the compiler pass only if allocation still dominates
  after IC + bump nursery.
- [ ] **Unboxed array blocks (L1).** One malloc + header, raw
  `double`/`int64_t` elements, no per-element marking. Kernel
  `get`/`put` become one indexed load/store (`Vec<f64>` shape,
  discovered not declared). Element type from hint, widening, or PGO.
- [ ] **Shapes for records (L5).** Declared numeric composites lay out
  like C (`ffi/defstruct` machinery pointed inward). Field access
  compiles to `*(base+off)`. Numbers-only first (no tracing); heap
  fields need exact GC maps (the hard part — gates on measured need).
- [ ] **Small-int / small-float fast paths.** Today every int is a
  range-checked double. Int64-tagged fast path in `vm_binop` for
  `add/sub/mul` on integral doubles would cut conversion overhead
  where kernels refuse. Measure before building.

## Tier 3 — AOT codegen (native code, the Rust-parity tier)

- [ ] **Loop versioning + bounds-check elimination.** Emit a fast path
  guarded by one hoisted bounds/type check for `for i 0 (length a)`
  loops; fall back to guarded version on violation. Removes per-iter
  branches (the current OOB `abort()` guards are correct but cost).
- [ ] **`__restrict__` on kernel array pointers.** Lets LLVM prove
  no-aliasing across `get`/`put` (vectorization gate). Sound: kernels
  take distinct array params or disjoint ranges by contract.
- [ ] **Autovectorization pragmas + `-march=native` opt-in.**
  `#pragma clang loop vectorize(enable)` on counted loops; document
  that shipped binaries use baseline x86-64 (portable) while local
  builds may opt into native SIMD.
- [ ] **LTO + PGO for the C link.** `-flto` on kernel + lib link;
  `llvm-profdata` from workload runs. 5-15% on kernel-heavy binaries.
- [ ] **Emit LLVM IR directly for hot kernels** (long term).
  `cc -O3` cannot see through unbox/rebox wrappers; direct IR lets us
  control inlining, vectorization, and alias scopes. Compare
  `objdump -d` against `rustc -O` on the same loop until identical.
- [ ] **Widen the opcode subset further:** `ltu/gtu/cmp`,
  `div`/`divf`/`mod`/`rem` for ints under `--int-policy promote`
  (run ints as doubles — oracle-identical by construction),
  immediate-shift fusion, `len` on strings/buffers.

## Tier 4 — concurrency + runtime

- [ ] **Segmented / grow-on-fault fiber stacks.** 2MiB + 64K guard
  per fiber (`fiber.c`) = ~600MB virtual for 300 fibers. Grow from
  64KB on guard fault; or segmented stacks with hot-split guards.
- [ ] **Lock-free mailboxes (MPSC ring).** `pthread_mutex` per
  channel (`ev.c`) bounces cache lines; single-producer rings are
  ~5x lower latency. Keep mutex path for MPMC.
- [ ] **Work-stealing pool.** `examples/pool.janet` is static
  dispatch; steal-on-idle halves tail latency on uneven workloads.
- [ ] **Zero-copy numeric sharing by default.** `core/shared`
  flattening exists; extend to `:array of :double/:long` without
  copy (already one block after L1 unboxing).
- [ ] **Thread-local nursery + parallel minor GC.** Each OS thread
  bumps its own nursery; minor collections run concurrently with a
  safepoint at quantum boundaries. Major stays STW (rare by then).

## Tier 5 — strings, symbols, PEG, FFI

- [ ] **Tiered interning.** Hot keywords intern via a small
  thread-local cache; fall back to the global table (with a real
  lock, not hope) on miss. `symcache` linear `findmem` dominates
  `require`-heavy startup.
- [ ] **Rope / persistent-buffer strings for frame building.**
  Frame-string building is 50% of GUI frame time; a reused buffer
  (done) then a rope (deferred concat, flatten at boundary).
- [ ] **PEG fast path for literal prefixes.** `peg.c` interprets
  bytecode per byte; literal runs should `memmem`/`memchr`.
- [ ] **FFI call overhead.** Cache resolved signatures per call site
  (today re-validates); batch struct marshal.

## Tier 6 — nuclear options (only if Tiers 0-5 stall)

- [ ] **Tracing JIT for the boxed tier** (abandons "no JIT by
  default"; AOT-first, JIT as opt-in tier for long-running servers).
- [ ] **Ownership-lite annotations** (`{:owned}`, regions):
  deterministic free for kernels, GC fallback otherwise. Rust-lite
  without the borrow checker — contracts, not proofs.
- [ ] **Whole-program monomorphization** of hot call graphs
  (Rust's actual trick): clone kernels per observed type tuple.
  Binary size grows; dispatch dies.
- [ ] **io_uring / kqueue batching in `ev.c`** for the net bench.
- [ ] **Huge pages for nursery + slabs** (`madvise HUGEPAGE`).

## Method (non-negotiable)

1. Every item lands with a bench row (bench/run.janet or ttfx
   byte-identical table) or it does not land.
2. Soundness first: `native-skip` with a reason beats a fast wrong
   answer. The `fmod` name-collision bug (-1 vs 0.5) is the template
   for what review must catch: C reserves libm names; the oracle
   catches the rest.
3. One mechanism per commit, `feat(scope):` subject, tests in the
   same commit. Revert is always one `git revert`.

## Appendix — ttfx-grounded findings (2026-10-09)

Ran `janet-port` 46x190 decrypt, seed 42: 1819 frames, 637 fps
interpreter (was 547; branch hints +1.16x). AOT lib path emitted **0
kernels** with **zero diagnostics** (fixed: lib path now prints
`native-skip` lines like `--emit-native`). Passing all sources
explicitly: 148 skips, 1 kernel. PGO top rows: `nil?` 13M,
`color-string` 2M, `ctx-anim` 2M, `xor64` 1.7M, `shr64/shl64` 1M,
`next-word-boxed` 339K, `bitlen`/`randbelow` 187K. New items beyond
the tiers above, ranked for this workload:

1. **Call-shaped access recognition (shipped).** `(a i)` compiles
   to `(push i) (tcall a)`, `(t :k)` to `(ldc k) (push) (tcall t)` —
   the analyzer only understood `get`. Now: inference refines
   single-index calls into proven `:array`/`:tuple` slots like
   `get`, and emission lowers to `->data[]` (per-site snapshot:
   dest slots reuse index temps). Multi-arg calls refuse.
2. **Immediate-opcode symmetry (shipped: `divim`).** `(/ x 2)` folds
   to `divim`, which had no emission while `addim/subim/mulim` did —
   blocked `bitlen`. Rule: every `*im` the compiler can emit must
   have an inference + emission arm (audit: `eqim/neqim/ltim/gtim`,
   `slim/srim/sruim` done; `divim` done; none remain unhandled).
3. **Non-negative int range analysis (shipped: PGO evidence).**
   `floor-div` (`canvas.janet`, canvas dims) refused on int width.
   Now: collector min/max → unanimous `min >= 0` seeds nonneg for
   unrebound params → trunc `divf`/`mod`/`rem` with fail-loud
   domain guards. Static `≥0` tracking (`ldi 0`, `len`,
   monotonic counters) remains future.
4. **Pure-core-call intrinsics (shipped).** `(math/floor (/ x 256))`
   (`seed-limbs`), `math/sqrt`, `math/abs` (+ `ceil`, `pow`): recorded
   from ldc-loaded cfunction constants, typed from a shared table,
   lowered to libm with `(double)` casts. `ldc`-of-cfunction skips
   emission as a dead callee load.
5. **String-format fusion (partially shipped).** `color-string`
   2M calls + per-cell `buffer/push-string` is the other 50% of
   frame time. Shipped: `(string "a" "b")` literal folding in the
   compiler. Still open: dynamic fusion / buffer sinking (L4).
6. **Counted-loop versioning (Tier 3, shipped narrow).**
   `for i 0 (len a)` elides the `>= count` half (`< 0` kept);
   positional proof (init-0, single-+1, dominated lt-exit,
   same-array bound, access-before-increment, no count mutation).
   Multi-block loops, rebound counters/arrays still refuse.

Status after the batch: 1 → 4 ttfx kernels (`k_next_word_bang`,
`color_clamp`, `floor_div` with domain guard, `bitlen` via
divim-double + floor-intrinsic chaining). Still refused, next in
line: constructor emission (`mkarr` in `seed-limbs`), live-range
splitting for shared slots (`xor64` `s2 mixes array/long/number`),
dependency chains (`rotl64` waits on leaves).
