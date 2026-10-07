# Native-stack fibers, M:N threads, and the Spinel-style collector

This document records the direction for bringing Janet's runtime to the model
Spinel uses for its own `Fiber` / `Thread` / GC, so that Janet programs can
eventually be compiled ahead of time (Spinel-style) without the interpreter
underneath.

The end state looks like this:

```
Ruby-like "JanetThread" ── green thread ── runqueues/monitor/SIGURG preemption
        │
        ▼
Fiber ──── owns a native C stack + context record (one-shot per entry)
        │
        ▼
janet VM runs its fiber on that native stack
        │  values / frames stay in the fiber's growable buffer (Phase 1: kept;
        │  Phase 2: frames live in the VM state, values in the buffer)
        ▼
Spinel-style collector: precise, non-moving, explicit roots,
object heap + immutable-string heap with marker bytes
```

## Why the runtime must change

Janet's existing `JanetFiber` is stackless: since Janet is an interpreter, a
fiber needs to suspend its *VM*, not its C call chain, so suspension parks by
longjmp out through `vm_return` and re-enters `run_vm` at a committed PC. That
works because a resumed fiber never has live C frames from its previous run.

But for AOT-compiled code, locals and VM frames will live in C frames on the
fiber's own stack. A fiber then needs its own C stack so that:

- yields/resumes can freeze a C stack instead of unwinding it,
- a compiled function's local variables have a stack to occupy,
- the GC can treat the VM buffer and C roots independently (Phase 2).

The interpreter is embedded inside the same fibers for `eval`/dynamic fallback,
so this substrate change must preserve full interpreter semantics.

## Phase 1 — native-stack substrate (DONE)

Goal: each `JanetFiber` runs its VM entry on its own allocated native C stack.
Suspension remains a logical unwind (VM frames still live in `fiber->data`,
longjmp-based signal dispatch unchanged), so *semantics are identical* to
today; only the physical stack used by that fiber's C calls changes.

Status: implemented on the `janet` checkout; full suite passes
(4318/4318 incl. a new `test/suite-native-fiber.janet`, 28 tests), on both
`-O0 -g` and default `-O2` builds.

Pieces (all in `src/`):

1. `janet_fiber_ctx_*` (`src/core/fiber.c`, decls in `src/include/janet.h`) —
   portable context switch, ported from `spinel/lib/sp_fiber_ctx.h`: fast
   register swap on x86_64/aarch64, `swapcontext()` fallback elsewhere.
2. `JanetFiber` gains: `native_stack`, `native_stack_base`,
   `native_stack_size` (2 MiB usable + 64 KiB `PROT_NONE` guard via mmap),
   `ctx` (resume point), `resume_ctx` (switch-back target), `in_value`,
   `out_payload`, `out_signal`.
3. `JanetVM` gains `base_ctx`: landing slot for continues from host code.
4. `continue` moves its tail `run_vm` call into the fiber's own trampoline
   (`janet_fiber_trampoline` in `src/core/vm.c`); every `janet_try` jmp_buf
   now lives on the fiber's native stack, so no longjmp crosses stacks.
5. `janet_mark_fiber` (`src/core/gc.c`) marks the new `in_value` /
   `out_payload` slots (plus `interrupt_value` since M4b-1);
   unmarshalled fibers get stacks via
   `janet_fiber_ensure_native_stack` (`src/core/marsh.c`).
6. VM frames/memory model (`fiber->data`, `JanetStackFrame`, fiber marshal)
   kept as-is.

Pitfalls found while porting (all fixed, with regression tests):

- Double `janet_try_init` in the trampoline leaked `vm.stackn` (+1 per
  resume → "C stack recursed too deeply" at boot). `janet_try` already
  inits; call it once.
- `janet_deinit` runs *inside* a fiber (`os/exit` from a script), so
  `clear_memory` must not `munmap` the executing fiber's stack —
  `janet_fiber_native_stack_free` skips `janet_vm.fiber` (leaks one mapping
  at shutdown, safe).
- Unmarshalled fibers bypass `fiber_alloc`; resume would swap into a
  garbage SP. Fixed via `janet_fiber_ensure_native_stack`.

(Phase 1.1a swap-suspension + 1.1b C-yield proof, both done — see below;
Phase 2a root discipline + stress verification, done — see below;
Phase 2b collector swap, Phase 3 threads — later.)

## Phase 1.1 — swap-suspension through live C frames (DONE)

Goal: a suspension (yield, user signal, debug, ...) that reaches C code
on the fiber's stack freezes the whole C chain with a context switch
instead of coercing to `"coerced from yield to error"`. Resume continues
the frozen chain transparently. Previously, `janet_call` (cfunction
calling back into yielding Janet code: `string/replace` with fn,
`peg/replace`, `io` writers, `ffi` callbacks) turned any non-OK signal
into an error; now only ERROR unwinds (longjmp, unchanged), everything
else suspends.

Status: implemented on the `janet` checkout; full suite passes
(34/34 suites, incl. `test/suite-native-fiber.janet` 47/47 with 6 new
swap-suspension tests 11–16), on both `-O2` (default) and `-O0 -g`
builds. AOT spike oracle (`tools/janet-diff.janet`) still `same` × 5.

Mechanics (all in `src/`):

1. Register exchange (`src/core/vm.c`): the VM dynamic registers always
   describe the running party. `janet_continue_no_check` parks the
   caller into its fiber (or `janet_vm.base_*` for host code) and loads
   the target's parked registers; `janet_suspend_swap` and the
   trampoline publish path do the inverse. Parked per fiber:
   `signal_buf`, `return_reg`, `coerce_error`, `stackn`, `gc_suspend`
   (`JanetFiber.saved_*` in `src/include/janet.h`, host slots in
   `src/core/state.h`). The GC lock counter and recursion depth ride
   along, so each context keeps its own balanced counts.
2. `janet_call` (`src/core/vm.c`): a nested `run_vm` returning a
   non-OK, non-ERROR signal suspends via `janet_suspend_swap`; each
   resume re-enters the nested VM with the resume value (flags cleared
   for trampoline-identical delivery), so the yielding function runs to
   completion and C callers observe an ordinary call. A smuggled signal
   on resume (cancel) is consumed like `run_vm` entry: ERROR unwinds
   from inside the nested computation, anything else re-propagates
   outward. Fallible work (`janet_gcroot`) in the resume path stays
   above the exchange, where the caller's signal chain is live.
3. Fresh/unmarshalled fibers park zeroes (`fiber_reset` in
   `src/core/fiber.c`, `src/core/marsh.c`); the trampoline's `janet_try`
   establishes their chain on first entry as before. `suspend_mode`
   records the resume path (0 = re-enter `run_vm` from the trampoline,
   1 = swap into the frozen C stack); the trampoline resets it on every
   publish. `resume_fiber` (marked in `src/core/gc.c`) identifies the
   resumer for the exchange.
4. Errors keep exact legacy semantics (longjmp to the nearest try, frame
   and try-state handling untouched); `(signal :yield)` via longjmp
   still coerces (freezing needs the return path — documented).

Pitfalls found (all fixed, with regression tests):

- Resuming must continue the nested VM, not inject the resume value as
  the call result — otherwise the yielding function's remaining opcodes
  never run (first yield worked, completion was wrong).
- `string/replace` replaces the first match only (`replace-all` for
  all): "X-a" is correct parity, not corruption.
- `peg/replace-all` returns a buffer; test assertions must `string` it.

## Phase 1.1b — C-callable yield + call-chain proof (DONE)

The interpreter's `fiber->data` frames stay where they are (the flat
`run_vm` loop is load-bearing; Phase 4 adds a codegen path beside it
rather than converting it). What 1.1b proves instead is the property
AOT needs: arbitrary C call chains on the fiber stack survive
yield/resume with locals intact.

Status: implemented; `make fibertest` green (17 checks) plus full suite
34/34 on `-O2` and the C test binary itself also green under `-O0`.

Pieces:

1. `janet_fiber_yield(v)` (`src/core/vm.c`, decl in
   `src/include/janet.h`) — suspend the current fiber from any C code
   on its stack (cfunctions, nested `janet_call` frames, future AOT
   frames). Returns the resume value. A smuggled signal on resume
   (cancel) is consumed like `run_vm` entry: ERROR unwinds with a
   longjmp through the C frames, anything else re-suspends outward.
   Panics with no current fiber. Values in C locals follow the usual C
   API contract across suspension: root them or keep them in VM slots —
   the collector cannot see the frozen C stack (explicit motivation
   for Phase 2's root discipline / conservative stack scan).
2. `tools/fiber-c-test.c` — embedder proof program (also the AOT host
   shape): direct C yield ×2 with canary/checksum/string locals,
   3-deep nested C frames frozen and resumed with value plumbing,
   rooted table surviving two mid-suspension collects, cancel payload
   through C frames, clean marshal refusal for C-frozen fibers (still
   resumable after), no-fiber panic. Wired as `make fibertest`.
3. `test/suite-native-fiber.janet` 11–16 cover the same paths from
   Janet (`string/replace`, `peg/replace` callbacks).

## Phase 2 — GC swap (option B+C)

- Port the Spinel collector's discipline: precise mark/sweep, explicit
  `JANET_GC_ROOT`-style roots (include a string marker-bit heap), per-worker
  root arrays.
- The collector's root walks become: fibers' `data` buffers as today (until
  Phase 1.1), each fiber's native-stack ranges as root objects, plus explicit
  roots from the generated code.
- The current `janet_gcmark`-from-`janet_vm` traversal goes away; the VM-local
  state is reached through explicit roots instead of scanning `janet_vm.fiber`.

Storage half done (see aot-plan.md 2b-i/2b-ii/2b-iii): scoped shadow roots,
immutable-string heap split, size-class slab pages behind `gcalloc` —
same lists/mark/sweep/deinit semantics throughout. Policy half done:
page-granular generations (age + dirty per slab page), write barrier in
the table/array store primitives, minors that skip old-clean tables and
arrays, never skipping while a compilation is active.

### Phase 2a — root discipline + stress verification (DONE)

Before swapping the collector, prove every live value is reachable
explicitly. Shipped on this checkout; full suite green on `-O2` and
`-O0 -g` (35/35 incl. new suites), AOT oracle still `same` × 5:

1. `test/suite-gc-stress.janet` (21 tests): allocation churn across every
   value kind at a tiny collection interval, incl. C-frozen suspensions,
   marshal round-trips, closures, ev. Runs in the normal `make test`.
2. Bugs found by stress and fixed (all pre-existing latent windows, most
   widened by swap-suspension letting collections run while C-frozen):
   - `peg/replace*` working set (`ret`, captures, scratch, tags, peg,
     extra args) unrooted across `janet_call` — pinned in
     `peg_cfun_init`/`peg_cfun_deinit` (`src/core/peg.c`).
   - `print`-to-function buffer unrooted across `janet_call`
     (`src/core/io.c`).
   - Scratch freed live by every collection (`janet_free_all_scratch`
     in `janet_collect`): scratch lifetimes belong to their C scopes, so
     collections no longer reclaim it (still reclaimed at shutdown).
   - Macroexpansion results in unrooted C locals swept mid-compile
     (later siblings compiled to nils): pinned via `c->pins`
     (`src/core/compile.[ch]`), macro fibers rooted in `macroexpand1`.
   - `def`/`var` attribute tables unrooted across value compilation
     (crashed in `defleaf` table_put): pinned in `handleattr`
     (`src/core/specials.c`).
   - Compiler scope state (consts/syms/nested defs in scratch vectors)
     unrooted across nested macro-fiber collections (compiled wrong code
     nondeterministically): the collector now walks the VM compiler
     stack (`JanetVM.compiler_stack`, pushed/popped in
     `janetc_init`/`janetc_deinit`) marking env, lints, result def,
     scope consts/symbols/nested defs (`janet_mark_compiler` in
     `src/core/gc.c`).
   - `marshal`/`unmarshal` state (buffers, lookup vectors with raw
     def/env pointers) unrooted: collections suspended for the whole
     operation with panic-safe re-raise (`src/core/marsh.c`).
3. New invariant: starting a fiber on a half-built function aborts with
   a stacktrace (`janet_fiber_reset`, `src/core/fiber.c`) instead of
   crashing later in `run_vm`.
4. `make fibertest` (17 C checks) and the AOT oracle cover the
   suspension paths; `tools/janet-aot.sh` unchanged.

Closed: the extreme-pressure (`gcsetinterval 2048`) `suite-boot`
behavioral asserts (`match 9`, `issue 463`) no longer reproduce —
verified green at `2048` across suite-boot/peg/ev/marsh/thread/
gc-stress/native-fiber, plus suite-boot at `512` and suite-thread at
`1024`. Never explained by a targeted fix (likely the 2b-iii rooting
discipline plus layout shifts); if it ever recurs, suspect an unrooted
value in macro/quasiquote paths first.

## Phase 3 — threads

Port `janet_ev`/`ev.c` replaced by an M:N scheduler (`sp_sched.c` analogue):
green threads as fibers + scheduling record, work-stealing run queues,
`SIGURG` preemption at interpreter loop poll points, scheduler-aware I/O
parking for blocking ops, `Thread.new`/`join`/`kill`/`[]` API.

### Phase 3 slice — thread/new + thread/join (DONE)

Mechanics (`src/core/ev.c`): `JanetThreadHandle` threaded abstract
(owner VM, done/joined/is_error, result Janet, waiter fiber; mark covers
result + waiter; no per-thread state so gcperthread is NULL) plus,
since M2, a gcrooted monitor list (`[chan tag]` pairs, tombstonable
via refs since M2b, delivered best-effort on the completion path,
freed/unrooted on delivery or handle free); worker args
(`JanetThreadNewArgs`: marshalled input, handle, owner, flags — mirrors
`cfun_ev_thread` layout minus supervisor); worker runs `janet_init`,
unmarshals registries + function + arg, `janet_fiber` (error/user masks),
`janet_schedule` + `janet_loop`, captures `fiber->last_value` + status
(DEAD ok / ERROR err / else "did not complete" err), marshals to a
malloc'd copy (fiber + result rooted across the copy), posts
`JanetThreadCompletion` to the owner, `janet_deinit`s. Completion runs on
the owner: unmarshals, stores, unroots join's roots, schedules the waiter
with the value (or cancels with the error), drops the ev refcount and the
worker's handle ref. `thread/join` fast path (done) consumes inline;
slow path claims (`joined=1`), records waiter, roots, `janet_await`s —
like channel ops, the cfun never resumes after await, the result arrives
as the call's value. `thread/alive?` (owner-checked, `!done`) polls
without blocking. `(thread/join h timeout)` arms `janet_addtimeout`:
expiry cancels the waiter (timeout error, rejoinable later); delivery
and reclaim key off `waiter_sched` (every schedule bumps `sched_id`),
so a resumed-then-returning fiber can never receive a rogue second
resume. Finite timeouts need a task fiber (top-level mains cannot be
cancelled — same upstream limit as `ev/deadline`). `test/suite-thread.janet`
(25 tests, incl. nested fibers) locks the behavior in `make test`. Panics: non-owner
join/alive?, double/concurrent join, join inside `janet_call`.

Pitfalls found (all fixed): `janet_try` around `janet_await` catches the
EVENT suspension itself (sig 13, empty payload) — no try, follow the
channel pattern; worker must gcroot fiber + result across result
marshalling; completion must read `is_error` before freeing its message;
`=` on arrays is identity — tests use `deep=`; `unmarshal_one_fiber`
must init `native_stack{,_base,_size}` explicitly (dirty slots, not
zeroes — wild-stack prime); `janet_slab_page_for` must gate the
mask+magic probe behind live-page membership (wild read on malloc-large
and foreign pointers — per-VM sorted directory now).

Closed (was: live-fiber arguments race): the crash was uninitialized
`native_stack{,_base,_size}` on unmarshalled fibers meeting dirty slots,
fixed by explicit init like `fiber_alloc`. Top-level fiber args are
covered by `test/suite-thread.janet`, and fibers nested inside args and
results round-trip too (probed both directions, 20/20 stable).

## Phase 4 — AOT compiler

Spinel-style: parse via `src/core/parse.c`, macroexpand as today, then a
whole-program type analysis and C codegen (`value`/`Janet` for boxed values,
unboxed C for inferred numeric/struct cases). `eval` and code that defeats
inference falls back to the interpreter inside the same runtime.

### Phase 4 step 1 — registration (DONE)

`tools/callgraph.janet` (gated by `test/suite-callgraph.janet`, 66 tests):
whole-file `(do ...)` compile, `disasm` walk, `def`/`contains`/`calls`/
`extern`/`ambiguous`/`unknown`/`dynamic` edges plus stats, with a
reachability + caller/recursive/taint fixpoint on top. Shallow
resolution only (symbolmap ranges + nearest-writer scan); the fixpoint
comes next. Notable semantics, both verified against upstream: a unit
with cross-form forward references fails exactly like real loading
(one honest `uncompiled` line), and statically-unreferenced top-level
defns are dropped by dead-store elimination — the graph is the
statically-live code by construction.

## Non-goals of Phase 1

- No change to the buffer/frame representation (`fiber->data`).
- No thread scheduler yet (single-threaded VM semantics unchanged).
- No collector replacement yet (still the list-of-blocks sweep).
- Semantics of signals, `JANET_FIBER_MASK_*`, debug stack traces and ev
  fiber scheduling stay identical.

## Addendum — where the later work lives (2026-10-07)

This document stays scoped to Phases 0–4 step 1 mechanics. Everything
after it is recorded in its own notes file:

- Messaging layer API + examples + decision guide (mailbox, pool,
  monitors/links/trap-exit, pids, registry, shared blobs, interrupt,
  timeslice): `docs/internals/messaging-guide.md`.
- AOT pipeline reference (hint surface, widths, arrays, int policy,
  externs, hybrid binaries, harness/parity, divergences, CLI):
  `docs/internals/aot-notes.md`.
- Shareable-values GC review + per-proof outcomes:
  `docs/internals/shared-structs-design.md`.
- Roadmap status for every step (M1–M5, Phase 4 steps 1–13, M4b
  slices): `docs/internals/aot-plan.md`.
- Runtime additions since this doc: `ev/monitor` + `ev/demonitor`
  (handle monitor lists), `ev/register`/`whereis`/`unregister`
  (process-global marshaled-copy registry), `core/shared` blobs,
  `ev/interrupt` (per-fiber resume-boundary delivery),
  `ev/timeslice` (quantum driver: per-resume poll budgets +
  scheduler re-queue); fiber fields `interrupt_requested` /
  `interrupt_value`, VM fields `quantum` / `quantum_remaining`.
