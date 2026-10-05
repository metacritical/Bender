# Janet AOT plan (Spinel-style) — spike → 1.1 → 2 → threads/AOT

## Handoff — read this first

This document plus `docs/internals/native-fibers.md` is the whole plan;
each phase records its goal, mechanics (with file:line-level pointers),
what was proven, and what is explicitly left open. A new session should
start by verifying the checkout, then pick up at the first unchecked
phase below (currently Phase 2b).

- This repo: the `janet` checkout (`janet-lang/janet` plus local work).
- Spinel reference: `/Users/pankajdoharey/Development/Projects/spinel_test`
  (`matz/spinel`; the installed `spinel` binary matches it). Relevant
  first reads there: `README.md` (pipeline + limitations), `src/main.c`
  (single-binary driver shape), `docs/` (FFI, limitations, threads).
- Work status: phases 0–2a are implemented but UNCOMMITTED in the working
  tree (13 modified files, new files under `tools/`, `docs/internals/`,
  `test/`). Commit before starting new work so the next change has a
  clean base and `git diff` stays reviewable.

## Verify the checkout (must all pass before new work)

```sh
make                    # default -O2 build -> build/janet
make test               # full suite: 37/37 (incl. suite-native-fiber 47/47,
                        # suite-gc-stress 21/21, suite-gc-minor 12/12,
                        # suite-thread 23/23)
make fibertest          # C call-chain suspension proof: 21 checks green
mkdir -p /tmp/aot-tests
printf '(print "hi from janet")\n' > /tmp/aot-tests/hello.janet
printf '(defn fib [n] (if (< n 2) n (+ (fib (- n 1)) (fib (- n 2)))))\n(print (fib 20))\n' > /tmp/aot-tests/fib.janet
printf '(def f (fiber/new (fn [] (for i 0 3 (yield i)) :done)))\n(while (def v (resume f)) (print v) (when (= v :done) (break)))\n' > /tmp/aot-tests/yield.janet
printf '(def ch (ev/chan 0))\n(ev/go (fn [] (ev/sleep 0.01) (ev/give ch :ping)))\n(print (ev/take ch))\n' > /tmp/aot-tests/ev.janet
printf '(error "boom")\n' > /tmp/aot-tests/err.janet
printf '(def hs (map (fn [i] (thread/new (fn [x] (* x x)) i)) (range 5)))\n(pp (map thread/join hs))\n(print (thread/join (thread/new (fn [x] (string "n=" x)) 7)))\n' > /tmp/aot-tests/thread.janet
for t in hello fib yield ev err thread; do
  ./build/janet tools/janet-diff.janet /tmp/aot-tests/$t.janet
done                    # AOT oracle: same × 6
make clean && make CFLAGS="-O0 -g" && make test   # same gates, -O0 build
```

## New files this project adds (all untracked until committed)

- `tools/mkimage.janet`, `tools/janet-aot.sh`, `tools/janet-diff.janet`
  (Phase 0 AOT spike: image compiler, driver, oracle).
- `tools/fiber-c-test.c` (Phase 1.1b proof program; `make fibertest`).
- `test/suite-native-fiber.janet` (47 tests: native-stack + swap-suspension).
- `test/suite-gc-stress.janet` (21 tests: GC root gate).
- `docs/internals/aot-plan.md` (this file), `docs/internals/native-fibers.md`
  (runtime direction + per-phase mechanics/pitfalls).

Status: Phase 1 (native-stack substrate) DONE on this checkout, uncommitted.
`build/janet test/suite-native-fiber.janet` → 28/28. Full suite is the gate
before each phase below. See `docs/internals/native-fibers.md` for Phase 1 detail.

Goal: a `janet-aot` single binary that compiles Janet source to a standalone
native executable: parse → macroexpand → analyze → emit one `.c` → `cc` →
binary, with the interpreter embedded as fallback for dynamic code.
Clone of the `spinel` driver shape (`spinel_test/src/main.c`):
parse+analyze+codegen+cc in one process, no helper chain at compile time.

## Janet pipeline today (what we reuse)

- `src/core/parse.c` (`JanetParser`) → `src/core/compile.c:janet_compile`
  → `specials.c` + `cfuns.c` builtin inlining → `emit.c/emit.h`
  (`janetc_emit_*`) + `regalloc.c` → `bytecode.c:janet_bytecode_optimize`
  → `JanetFuncDef{bytecode,consts,defs,slotcount,...}` (`janet.h`)
  → `run.c:janet_dobytes` → `vm.c:run_vm` on `fiber->data` frames.
- Values: NaN-boxed `Janet` (`janet.h`), 16 `JanetType`s; GC objects headed
  by `JanetGCObject`. `boot/boot.janet` baked into the binary via
  `build/c/janet.c` (Makefile bootstrap); `tools/amalg.janet` precedent for
  shipping source as C.
- Fibers now each own a 2 MiB + 64 KiB-guard native stack with
  `janet_fiber_ctx_{prime,swap}` (`fiber.c`, `janet.h`), trampoline in
  `vm.c:janet_fiber_trampoline`. Frames/values still live in `fiber->data`.

## Phase 0 — AOT skeleton spike (first; proves the pipe, no perf)

Build `tools/janet-aot` (C, linked against `build/libjanet.a` at first):
`janet-aot app.janet [-o app] [-c] [-S] [-E args...]`.

1. Parse with `parse.c`, macroexpand with the *host* `janet` semantics
   (macros run in a fiber at compile time, as today — `compile.c`).
2. Take the resulting top-level `JanetFuncDef`s as IR (do NOT redesign the
   frontend yet; `compile.h:JanetCompiler/Scope/Slot`, `emit.h` stay private,
   spike reads finished `FuncDef`s via `marshal.c`-style walk).
3. Emit one `.c`: static `JanetFuncDef` initializers for consts/bytecode +
   `main()` that `janet_init`s, `janet_gcroot`s the constants, loads the
   image and calls into it. All ops stay boxed (`Janet`); `JOP_CALL/RESUME/
   YIELD/CLOSURE` lower to existing `janet_call/continue` entry points.
4. `cc -O2 -ffunction-sections -fdata-sections -Wl,--gc-sections` (macOS:
   `-Wl,-dead_strip`) against a `libjanet_rt.a` split (header-inline hot
   paths vs archived `src/core/*.c`, mirroring `lib/spinel_rt.h` vs
   `lib/sp_*.c`). Static roots only; no VM-stack scanning for constants.
5. Refusals (compile error with file:line list, Spinel-style): `eval`,
   `compile`, `dobytes/dostring` on runtime strings, runtime `require` with
   computed paths, `dyn *redef*` redefinition, live `marshal` of code,
   `ffi`/`dlopen` natives, `ev` threading beyond smoke. `--defer-fallback`
   keeps building and routes refused forms to the embedded interpreter.
6. Oracle: `tools/janet-diff` — run `janet app.janet` vs `janet-aot -E`
   (fold addresses/paths/times), labels `same/output-diff/exception-diff/
   compile-error/crash`, exit 0/1/2/3 like `spinel diff`.

Accept: `print "hi"`, `fib(20)`, file IO, one fiber yield/resume program,
one `ev/chan` smoke program, and one `thread/new` + `thread/join` program
produce `same` through the oracle; `-c/-S`
emit inspectable C; binary starts with no `janet` on PATH.

## Phase 1.1 — frames on the native stack

Goal: VM frames live in C frames on the fiber's stack; `fiber->data`
becomes values-only, then is removed. Compiled functions get real C locals
and can yield with live C state (hard requirement for unboxed codegen).
Keep semantics identical; interpreter rides the same substrate.

- Move `JanetStackFrame` chain out of `fiber->data` into the trampoline's
  C frames (`vm.c`, `fiber.c/h`, `state.h:JanetVM`).
- Every `janet_try` jmp_buf already lives on the fiber stack (Phase 1);
  extend the invariant to all return/signal registers.
- Update `janet_mark_fiber` (`gc.c`), fiber marshal (`marsh.c`), debugger
  (`debug.c`) for the new layout.
- Regression: full suite + `suite-native-fiber` (deep recursion, nested
  resume/error, GC-in-flight, marshal round-trip, 300 live fibers).

### 1.1a swap-suspension substrate (DONE — prerequisite, this checkout)

Before frames can move, suspension must preserve the C stack at all.
`janet_call` coerced any nested non-OK signal to `"coerced ... to error"`;
now non-ERROR signals swap-freeze the C chain (`janet_suspend_swap`,
register exchange per party, resume re-enters the nested VM) while ERROR
keeps legacy longjmp unwinding. Full detail in
`docs/internals/native-fibers.md` Phase 1.1. Remaining 1.1 work is the
frame migration itself on top of this substrate.

### 1.1b C-yield proof (DONE — rescoped, this checkout)

Decision: the interpreter's `fiber->data` frames do NOT move — the flat
`run_vm` loop stays as the fallback interpreter and Phase 4 adds a
codegen path beside it. What AOT actually needs (now proven) is C call
chains surviving yield: `janet_fiber_yield(v)` suspends from any C on
the fiber stack, `tools/fiber-c-test.c` proves canaries/checksums/value
plumbing across 1–3 C frames, mid-suspension collects, cancel-unwind,
and clean marshal refusal (`make fibertest`, 17 checks green).
`test/suite-native-fiber.janet` 11–16 lock the Janet-visible paths.

## Phase 2 — explicit GC roots + collector discipline

Goal: Spinel-style precise, non-moving mark/sweep with explicit roots so
generated C never depends on scanning `janet_vm`/VM stacks.

- Introduce `JANET_GC_ROOT`-style root registration; per-fiber native-stack
  ranges + generated-code roots join the root set; `fiber->data` buffers
  (or their Phase-1.1 successors) walked as explicit roots.
- Retire the `janet_vm.fiber` traversal (`janet_gcmark`); VM-local state
  reached only through roots. Immutable-string heap with marker bits
  (Spinel model); per-worker root arrays land with Phase 3.
- Regression: full suite under `make test` + `make valtest` (leak/over-mark
  check), marshal + `weak` table tests explicitly.

### Phase 2a — root discipline + stress (DONE, this checkout)

Proved every live value reachable before swapping anything: new
`test/suite-gc-stress.janet` gate (21 tests), six latent root bugs fixed
(peg replace set, print-fn buffer, scratch lifetime, macroexpansion pins,
def/var attr tables, compiler-scope marking via a new `compiler_stack`
walk, panic-safe marshal/unmarshal suspension), plus a half-built-function
fiber invariant. Full detail in `docs/internals/native-fibers.md`
Phase 2a. Remaining: the 2b collector swap itself (slab/size-classes,
string heap, per-worker roots), then threads.

### Phase 2b — collector swap (NEXT; not started)

Do NOT replace the collector at once. The current collector lives in
`src/core/gc.c`: singly-linked `janet_vm.blocks` of GC objects,
mark from ev / `top_dyns` / `root_fiber`+child / explicit `roots[]` /
`compiler_stack`, sweep in `janet_sweep`, collection scheduling via
`next_collection`/`gc_interval` counters, `gc_suspend` nesting counter,
weak tables, scratch. Keep all entry points and their signatures
(`janet_gcalloc`, `janet_collect`, `janet_gcroot/unroot`, `janet_mark`,
`janet_table_*` etc.) stable so the rest of the runtime never notices;
swap the implementation underneath in three ordered slices:

1. **2b-i — scoped roots for generated/C code** (DONE, this checkout).
   New public API (`src/include/janet.h`, `src/core/gc.c`): a strictly
   LIFO shadow root stack (`JanetVM.shadow_roots`, raw backing so pushes
   never trigger collections) marked on every collection —
   `janet_gcshadow_push(x)` / `janet_gcshadow_mark()` /
   `janet_gcshadow_pop_to(mark)` (over-pop panics). `tools/fiber-c-test.c`
   migrated off manual `gcroot` onto it, plus a new 50-deep C call chain
   holding one table per level across mid-suspension collects with no
   manual rooting anywhere, verified intact on unwind with a balanced
   stack at the host (`make fibertest`, now 21 checks green). Full suite
   green on `-O2` and `-O0 -g`, oracle still `same` × 5.
2. **2b-ii — immutable-string heap split** (DONE, this checkout).
   Strings, symbols and keywords (keywords intern to symbols — there is
   no separate keyword block type) now live on their own
   `JanetVM.string_blocks` list, routed by type in `janet_gcalloc`;
   everything else (incl. mutable buffers) stays on the main heap.
   Both strong heaps sweep with identical semantics through a shared
   `janet_sweep_list` helper, which asserts the split every sweep
   (string heap: only string/symbol; main heap: neither) — misrouting
   aborts immediately instead of corrupting silently. `block_count`
   stays a single total so scheduling is unchanged; `clear_memory` and
   init handle both lists; the debugger's funcdef heap scan needs no
   change (funcdefs never move). Deliberately no sweep-skipping policy
   yet: skipping the string heap when "nothing new" is unsound (old
   live strings can die without new allocations), so that waits for
   generations in 2b-iii. Full suite green on `-O2` and `-O0 -g`,
   stress + fibertest + oracle green, targeted suites at 64 KiB green,
   string microbench healthy (~7 ms / 20k interning ops). No valgrind
   on this host (macOS) — `make valtest` gate pending a Linux runner.
3. **2b-iii — slab/size-class allocator + generations.** Replace the
   block list with size-classed slabs and bitmap generation/mark state
   behind the same `gcalloc` interface; sweep beside the program.
   Accept: suite + stress + `make bench` within noise of (eventually
   faster than) the current collector, `make valtest` clean.

   **Storage done (this checkout), policy open.** GC object storage moved
   from one-malloc-per-object to 16 KiB pages carved into 8 size classes
   (32–512 bytes; larger objects keep the malloc large path), behind the
   unchanged `janet_gcalloc` signature. Freelist links overlay
   `JanetGCObject.data.next`; release finds a slot's class through a
   small sorted page directory (pages are added rarely), so no per-object
   tags, no layout changes, no stolen flag bits. All whole-object frees
   (sweep all three heaps, `clear_memory`, threaded-abstract refcount
   release) route through one `janet_gc_release`; subsidiary buffers
   still use plain free. Slab state is per-VM (`JanetVM.slab_state`),
   freed at clear/deinit. Full suite green on `-O2` and `-O0 -g`,
   stress + fibertest + oracle green, boot/peg/ev/marsh clean at
   64 KiB stress, string microbench at parity (~6.5 ms vs ~7.0 ms per
   20k interning ops). Notably, the two extreme-pressure behavioral
   failures from Phase 2a no longer reproduce at any interval tried
   (likely layout shift, not a targeted fix — the macro-path audit
   stays open).

   **Policy done (this checkout).** Page-granular generations on top:
   per-page age (young 0–1, old 2+) and dirty flags on the slab headers;
   a write barrier in the table/array store primitives (`table_put`,
   `put_no_overwrite`, `array_push`, indexed `put`/`putindex`, `fill`,
   `insert`, `setproto`) that dirties old pages on new heap edges;
   minors that mark from full roots while skipping old-clean tables and
   arrays, then sweep young dead blocks only (old blocks wait); majors
   unchanged plus age reset and freelist merge. Only tables/arrays are
   gated — fibers, abstracts, functions, tuples, structs, weak tables
   and everything else traverse exactly like majors (their young refs
   can't hide behind an unbarriered edge, proven by construction for
   immutables and by exhaustive store audit for the rest). Fibers stay
   fully scanned (no barrier in `run_vm`). New `test/suite-gc-minor.janet`
   torture gate (old→young webs through every barrier site under churn)
   plus a `JANET_GC_VERIFY=1` post-mark edge verifier that aborts on any
   young unmarked reachable object. Full suite green on `-O2` and `-O0 -g`
   (36 files), stress + fibertest + oracle green, targeted suites clean
   at 64 KiB stress. Rule learned the hard way and now enforced: never
   skip traversal while any compilation is active (compile-time table
   traffic defeats generation accounting — safe by construction, since
   traversing more is never wrong).

Gates for all of 2b: `make test`, `test/suite-gc-stress.janet`,
`make fibertest`, the AOT oracle, plus targeted stress runs
(`gcsetinterval 65536` over `suite-boot`/`suite-peg`/`suite-ev`/
`suite-marsh` must stay green; extreme `2048` currently has two known
behavioral failures — see native-fibers.md — fix or carry explicitly).

## Phase 3 — M:N threads (after GC)

Replace `ev.c` cooperation with an M:N scheduler (`sp_sched.c` analogue):
green threads as fibers + scheduling records, work-stealing runqueues,
preemption at interpreter-loop poll points (`SIGURG`-style), scheduler-aware
parking for blocking IO (`net.c`, `ev.c`, `os.c`). `Thread.new/join/kill`
API on top. Single-threaded semantics unchanged until this phase.

### Phase 3 slice — value-returning thread/new + thread/join (DONE, this checkout)

`ev/thread` suspends until completion but always returns nil. New
`thread/new` (function or fiber) + `thread/join` + `thread/alive?` in
`src/core/ev.c`: threaded-abstract handle (`JanetThreadHandle`,
owner-checked, single-join), detached worker bootstrapping a
thread-local VM exactly like `janet_go_thread_subr` (marshal registries
+ fn/fiber + arg in, marshal result out), completion via
`janet_ev_post_event` to the owner (message-owned result bytes,
unmarshalled on the owner; waiter scheduled with the value, cancelled
with the error — the joining cfun never resumes after await, like
channel ops). `thread/join` takes an optional timeout (seconds):
expiry cancels the waiter with a timeout error (same `sched_id`
exactly-once discipline as the scheduler: concurrent joiners panic,
a resumed-then-rejoining fiber reclaims instead of double-delivering,
and a later join still collects); finite timeouts need a task fiber.
`thread/alive?` polls completion without blocking (fresh
handles read alive deterministically: completion can only be processed
once the spawner suspends). Roots follow the `ev/thread`
pattern (join gcroots, callback unroots). New `test/suite-thread.janet`
(23 tests: value round-trips incl. fiber args, 8-way parallel,
ev-in-thread, error re-raise, double-join, non-function refusal,
alive? transitions, join-in-ev-task, timeout fire/rejoin/top-level)
runs in `make test` (now 37/37). Gates: full suite +
fibertest + stress/minor/ev + oracle `same` × 6 on `-O2` and `-O0 -g`,
plus a full ASan run (37/37, fibertest, stress/minor/ev green);
thread suite 25+/25 stable plus 64 KiB `gcsetinterval` runs over
suite-boot/peg/ev/marsh green.

Fixed along the way (both pre-existing, both crashers):
- Slab `janet_slab_page_for` (`src/core/gc.c`) masked any pointer to
  16 KiB and dereferenced it: malloc-large-path and foreign pointers
  could hit unmapped memory (ASan abort in `janet_gc_barrier` via
  `table_put`). Now a per-VM sorted live-page directory gates the
  probe (binary search; insert on page alloc, cleared with the pages).
- `unmarshal_one_fiber` (`src/core/marsh.c`) never initialized
  `native_stack{,_base,_size}`, trusting zeroed memory that dirty slab
  bump slots and malloc reuse don't provide; workers then primed a wild
  stack (`ev/thread` with a fiber arg segfaulted, flakily without ASan,
  ~100% under it after churn). Fields are now set explicitly like
  `fiber_alloc`; fiber args are accepted again.

## Phase 4 — full AOT (inference + unboxed codegen)

Only after 1.1+2: whole-program type analysis over the `FuncDef` graph
(Spinel `analyze*.c` analogue: registration → call-site widening →
fixpoint on returns/slots → feature/DCE flags → per-node type cache shared
with codegen in one process), then:

- Boxed `Janet` for polymorphic slots; unboxed C (`double`, `int64`) for
  inferred numeric loops/structs; value-type promotion for small immutable
  records; int-overflow mode flag (`raise` default / `wrap` / `promote`
  to bignum discipline, cf. Spinel `--int-overflow`).
- DCE via section-GC + reachability from `main`; `--emit-types` diagnostics.
- `eval` and inference-defeating code permanently fall back to the embedded
  interpreter in the same runtime (Phase 4 in `native-fibers.md`).

## Internals changes required (recap)

| Area | Spike | 1.1 | 2 | 4 |
|---|---|---|---|---|
| `compile.h/Scope/emit.h` → public IR / NodeTable analogue | read-only use | — | — | own + publish |
| `vm.c/fiber.c/h/state.h` frames→C stack | — | yes | — | benefits |
| `gc.c/h` explicit roots → new collector | static roots only | marking update | yes | benefits |
| `marsh.c` fiber/FuncDef image | static init instead | image update | root-aware | static DCE |
| `ffi.c/ev.c/net.c` | refuse/defer | — | parking hooks | direct externs or keep refusing |
| `Makefile`/`tools/` driver+oracle | `janet-aot`, `janet-diff` | test gates | `libjanet_rt.a` split | inference flags |

## Gates

Each phase: `make` (default `-O2` and `-O0 -g`), `make test` (full suite
incl. `suite-native-fiber`), `make valtest` for GC phases. No phase may
change program semantics — oracle `same` is the bar until Phase 4
optimizations, which must still be `same`.

## Spike results (done, this checkout — tools/ only, no src/ changes)

- `tools/mkimage.janet`: whole file compiles as ONE `(do ...)` thunk
  (correct cross-form refs, zero build-time execution), marshals with a
  rev-lookup registry, emits `.c` with image bytes + registry names.
- `tools/janet-aot.sh`: driver (`-o/-c/-S/-E`, spinel CLI shape).
- `tools/janet-diff.janet`: oracle (`same/output-diff/exception-diff/
  compile-error`, exits 0/1/2/4; folds `0xHEX`, tmp paths, ` (tail call)`).
- Findings: env entries are binding descriptors — registry must map
  unwrapped `:value`s and walk the `make-env` proto chain (util.c
  `janet_binding_from_entry`); runtime resolves via `janet_resolve_core`.
  `:args[0]` is the script path. `string/replace-all` takes strings only —
  use `peg/replace-all` for pattern folding. AOT `main` mirrors
  `run.c janet_dobytes` (continue, then `janet_loop()`).
- Acceptance: hello/fib(20)=6765/fiber-yield/ev-chan-rendezvous/error-exit-1
  all `same` (34/34 repo suites incl. `suite-native-fiber` still green).
  One folded cosmetic divergence: single-thunk images render one fewer
  ` (tail call)` frame note than per-form dobytes on error paths
  (identical bytecode; runtime frame-reuse artifact — Phase 1.1 closes it).
