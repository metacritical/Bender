# Native-to-native continuation: design

Date: 2026-10-23
Status: design only (no code changed by this document)
Scope: `tools/loop-executor.ben`, `tools/callgraph.ben`, `src/core/vm.c`,
`src/core/bytecode.c`, `src/core/fiber.c`

This is a read-only study of the *current* code plus a concrete, staged
implementation design. Where the background note
(`docs/internals/ttfx-pgo-runtime-feedback.md`) describes the mechanism
loosely, or describes already-existing machinery as missing, the code is
quoted and the note is corrected in-line (see "Corrections to the background
doc").

---

## 1. What happens today at an executor-to-executor call edge

### 1.1 Shape of an executor

`tools/loop-executor.ben:181` emits one C function per eligible definition:

```c
static BenderNativeStep nx_<id>(BenderFiber *f, BenderFuncDef *d, uint32_t **pc)
```

with `Bender *s = f->data + f->frame;` — the executor operates **in place on
the already-pushed VM frame of its own definition**. It owns no frame, no
arguments, no return value. Entry is a computed `switch (*pc - d->bytecode)`
jumping to `L<pc>` (`loop-executor.ben:203-205`), so every bytecode PC is a
legal entry.

Exit is only ever one of two macros (`loop-executor.ben:194-196`):

```c
#define NX_EXIT(K,R) do { *pc = d->bytecode + (K); return (R); } while (0)
#define NX_BOUNDARY(K) do { NX_VMEXIT(); NX_EXIT(K,BENDER_NATIVE_VM); } while (0)
```

`BenderNativeStep` has exactly two values today (`src/include/bender.h:1283`):
`BENDER_NATIVE_VM` and `BENDER_NATIVE_INTERRUPT`. There is **no** step value
meaning "I changed the frame".

### 1.2 The `call` emitter

`loop-executor.ben:379-433` is the whole call story. In order:

1. Terminal call (call is the last instruction): unconditional
   `NX_BOUNDARY(k)` — "terminal call: VM owns it" (`:380-381`).
2. `f->stack_overflowed` → exit at `k` (`:385`).
3. `bender_native_core_predicate` (boot-predicate identity match) — computed
   inline, poll, `goto L(k+1)` (`:391-397`).
4. `bender_native_buffer_push_string_ready/_commit` (`:402-409`).
5. `bender_native_container_call_fast` — `(table key)`, `(array i)` as
   `bender_in` (`:416-422`).
6. `bender_native_alias_cfun` (`src/core/vm.c:389`) → direct FFI-alias
   cfunction call via `bender_native_alias_call` (`vm.c:406`), then
   `bender_native_alias_after_call()` (`vm.c:425`) and a VM exit at `k+1`
   (`:423-432`).
7. **Everything else**: `NX_EXIT(k, BENDER_NATIVE_VM)` (`:433`) — exit
   *before* the call, at the call PC.

A Bender-function callee that happens to have its own `native_code` falls into
case 7: `bender_native_alias_cfun` returns NULL for it (`vm.c:395` demands
`BENDER_FUNCDEF_FLAG_FFI_ALIAS`). So the hot `ctx-update → ctx-motion-move`
edge takes case 7 on every call.

Note also that `push`/`push2`/`push3` *are* native
(`loop-executor.ben:348-354`): they write straight to
`f->data[f->stacktop++]` after a capacity/`maxstack` check. At the moment of
the exit the arguments already sit between `f->stackstart` and `f->stacktop`
— exactly where `JOP_CALL` expects them.

### 1.3 Cost of the exit + re-entry, step by step

`vm_native()` (`src/core/vm.c:339-366`) is the only dispatcher into
executors; it runs once per `vm_next()` (`vm.c:101`, `:108`), i.e. before
every instruction, and once at resume entry (`vm.c:1872-1875`). On the exit
path:

1. The executor returns `BENDER_NATIVE_VM` with `*pc` = the call PC.
2. `vm.c:350-352`: `pc = npc; stack = fiber->data + fiber->frame;
   vm_commit();` (`vm_commit` is `bender_stack_frame(stack)->pc = pc`,
   `vm.c:274`).
3. `vm.c:354-355`: `bender_vm.native_vm_exits++;`
   `bender_vm.native_exit_opcodes[*pc & 0xFF]++;`
4. The interpreter dispatches `JOP_CALL` (`vm.c:2285-2351`):
   `vm_maybe_overflow()`, `vm_maybe_auto_suspend(1)` (a *second* poll — the
   executor already polls on back-edges and before its own fast-path calls),
   keyword-method check, type check, the 6-condition FFI-alias probe
   (`:2307-2312`, all failing for a plain Bender callee), trace-flag test,
   `vm_commit()`, `bender_fiber_funcframe(fiber, func)` (`fiber.c:765`),
   profile hook, `stack = fiber->data + fiber->frame`,
   `pc = func->def->bytecode`, `vm_checkgc_next()`.
5. `vm_next()` → `vm_native()` again, now with `func->def` = B:
   `bender_native_region_allowed(B_code, 0)` (`bytecode.c:2329`), then —
   whenever `bender_vm.native_epoch` has moved or the def's cache is cold —
   `bender_native_revalidate` (`bytecode.c:2060`), which runs
   `native_structure_matches` (`bytecode.c:1986`): a `memcmp` of *all*
   bytecode words, a per-word opcode-allowlist walk, and a constant compare;
   on the first (def, descriptor) pair also `bender_verify(d)`.
6. B's executor runs from PC 0. When B finishes, it does **not** execute its
   own `ret`: `loop-executor.ben:541` makes `ret`/`retn` an
   `NX_BOUNDARY`, so B exits to the VM at its return PC, the VM runs
   `JOP_RETURN` (`vm.c:1896-1907`) — `bender_fiber_popframe`, `vm_restore()`
   (which reloads `func` from the frame), `stack[A] = retval`,
   `vm_checkgc_pcnext()`.
7. Back in A at call PC + 1, `vm_native()` fires again: region check plus, if
   A's cached verdict was invalidated in the meantime (publication, resume,
   breakpoint write, or `bender_native_alias_after_call`'s epoch bump at
   `vm.c:427`), another whole-definition revalidation of A.

One hot call edge therefore costs: two `vm_native()` gate passes (potentially
two whole-definition `memcmp` + opcode-walk validations), one full
interpreted `JOP_CALL`, one interpreted `JOP_RETURN`, two executor-prologue
computed-goto re-entries, and one duplicated interrupt poll. That is exactly
the mix release sampling reports: `run_vm`, `bender_fiber_funcframe`,
`native_structure_matches`, alongside the executors.

What it does **not** cost: argument marshalling or state spill. Executors keep
every live value boxed in the GC-scanned VM frame at all times; the only raw
C doubles are inside the `numeric-sum` special case
(`loop-executor.ben:206-229`) and they are written back before any exit.
**There is no spill step to remove.**

### 1.4 Hot-edge grounding in the real ttfx port

The ttfx port is **not** inside this repository; it lives at
`/Users/pankajdoharey/Development/Projects/ML/ttfx/bender-port/`. The hot
functions are in `src/scene.ben` (not `canvas.ben`, which only holds the
state constructors `charanim-new`, `canvas.ben:44-50`, and
`fxchar-new`, `:40-41`).

Per-tick call tree, read from the source:

```
ctx-update                         scene.ben:628   (one call per tick)
├─ ctx-active-snapshot             :489  (:633)      1 edge/tick
├─ for each live id:
│  ├─ ctx-motion-move              :439  (:640, guarded by (ctx :path-live))
│  │  └─ ctx-path-step             :407  (:446)      unconditional once a path is active
│  │     ├─ E/ease                 easing (:413)     cross-module
│  │     └─ ctx-coord-on-line      :289  (:436)  or  geom-coord-on-bezier (:437)
│  │        └─ geom-lerp-point     :100  (:290)
│  └─ ctx-step-animation           :575  (:641)      1 edge/char/tick
│     ├─ ctx-step-synced           :542  (:582)      sync scenes only
│     ├─ scene-next-eased          :82   (:600)      eased scenes only
│     └─ ctx-fire-scene-complete   :533  (:599,:607) on completion only
└─ for each id in :active:
   └─ ctx-char-active?             :612  (:646)      1 edge/char/tick (compaction pass)
```

Three facts this grounding changes in the design:

1. **The edge count is three per live character per tick, not two.**
   `ctx-char-active?` (`scene.ben:612-620`) is called from `ctx-update`'s
   second, compaction loop (`:646`) and is a separate hot edge. The
   background doc's list (`ctx-motion-move`, `ctx-step-animation`,
   `ctx-path-step`) omits it.
2. **The motion edge is a 3-deep chain, not a single edge.**
   `ctx-update → ctx-motion-move → ctx-path-step → ctx-coord-on-line →
   geom-lerp-point`. A design that only removes *one* VM bounce per call
   captures a fraction of the cost; the flat trampoline (§2.2d) chains the
   whole depth inside one C frame, which is the main reason to prefer it over
   a single-edge fast path.
3. **The callees are small.** `ctx-char-active?` is five guards and a
   comparison; `ctx-step-animation`'s common (non-synced, non-eased) branch
   is inline frame bookkeeping that calls nothing; `ctx-motion-move` is
   mostly `put`s. For bodies this small the *fixed* per-edge VM-exit cost
   (§1.3) is a large fraction of the work done, which is exactly the regime
   where continuation pays.

A parallel survey measured **motion at ~72.8% of frame time and ~31x off
Rust** — the single dominant term, and precisely the
`ctx-motion-move`/`ctx-path-step` subtree above. That is independent
confirmation that continuation is the right next compiler feature and that
v1 should target these specific edges (§5) rather than being a general
facility first.

*Incidental observation while reading (not part of this design, no change
made):* `ctx-coord-on-line` (`scene.ben:289-292`) calls `geom-lerp-point`
and **discards the result**, then recomputes the same lerp inline. That is one
whole VM-exit call per path step per character computing a value nobody reads.
It is a free win available today, independent of this feature, and it should
be measured separately so it does not contaminate the continuation A/B.

---

## 2. Proposed mechanism

### 2.1 Why a static direct C call is not available

The tempting shape — `nx_A` calls `nx_B` directly — cannot be done soundly,
because **the compiler never knows which function will be in the callee
slot**. Executor identity in this toolchain is *structural and
runtime-resolved*:

- `callgraph.ben:5508-5518` emits descriptors keyed by a structural key
  (`flags/arity/min/max/slotcount/constants/bytecode-words`); exact
  duplicates are emitted once.
- `callgraph.ben:5654-5655` publishes them with
  `bender_native_registry_add(&nx_<id>_code)`.
- Attachment is lazy, in `bender_native_try_attach` (`bytecode.c:2376`): a
  shape hash (`native_shape_key`, `bytecode.c:2302`) then the full
  `bender_native_matches` exact compare. The comment is explicit: "Names and
  keys never decide attachment; `bender_native_matches` does." An *ambiguous*
  match refuses outright (`bytecode.c:2395-2400`).
- The executor reads the callee from `s[C]` at runtime. The static call graph
  (`call-map`, `callgraph.ben:1084-1100`; `["calls" caller-id callee-id]`
  at `:877`) is *analysis*, not a runtime guarantee: the slot can hold a
  rebound, shadowed or unmarshaled function.

The continuation must therefore be **runtime-resolved**: the call site checks
that the live callee's `BenderFuncDef` carries a verified `native_code`, then
dispatches through `def->native_code->execute`. A per-PC monomorphic inline
cache makes that nearly as cheap as a direct call (see v2).

### 2.2 Recommended mechanism: a flat native trampoline (no C recursion)

One new step value, two new runtime helpers, one loop.

**(a) New step kind** (`src/include/bender.h:1283`):

```c
typedef enum { BENDER_NATIVE_VM, BENDER_NATIVE_INTERRUPT, BENDER_NATIVE_FRAME } BenderNativeStep;
```

`BENDER_NATIVE_FRAME` means: *"I pushed a real fiber frame; `*pc` points into
the NEW current frame's definition; reload everything."*

**(b) Executor-side emitter change** (`loop-executor.ben:379-433`, inserted
after the alias fast path and before the final
`NX_EXIT(k, BENDER_NATIVE_VM)`):

```c
{ int32_t nxargc_ = f->stacktop - f->stackstart;
  uint32_t *nxnpc_ = NULL;
  int nxk_ = bender_native_call_enter(f, nxcallee_, nxargc_,
                                     d->bytecode + K, &nxnpc_);
  if (nxk_ == 1) { *pc = nxnpc_; return BENDER_NATIVE_FRAME; }
  if (nxk_ == 2) { NX_EXIT(K, BENDER_NATIVE_INTERRUPT); } }
/* nxk_ == 0: refused, nothing touched */
NX_EXIT(K, BENDER_NATIVE_VM);
```

`bender_native_call_enter` must be **all-or-nothing**: on refusal (`0`) it has
touched no fiber state whatsoever, so ordinary `JOP_CALL` is still correct
and performs the call exactly once. On `1` it has done exactly what
`JOP_CALL`'s function branch does, in the same order (§2.3).

**(c) VM-side adoption** (`vm.c:339-366`). `vm_native()` today keeps the
register `func` bound to the *caller* after an executor returns — safe only
because no executor can currently change frames. It gains a third arm:

```c
} else if (result == BENDER_NATIVE_FRAME) {
    vm_restore();          /* reloads stack, pc, func from bender_fiber_frame */
    pc = npc;              /* executor-chosen PC inside the new frame */
    maybe_collect();
}
```

`vm_restore()` (`vm.c:275-279`) already reloads `stack`, `pc` *and* `func`
from the frame — exactly the required transition. Because `pc` lands on B's
PC 0, the next `vm_next()`'s `vm_native()` enters B's executor if it has one;
correctness does not depend on whether B is native.

**(d) The actual continuation: a flat trampoline.** (c) alone removes
`JOP_CALL` dispatch but still bounces through `run_vm`'s loop head. The real
native-to-native chain is a loop *inside* a new vm.c function:

```c
/* Returns when the current frame is no longer natively continuable.
 * NEVER recurses: frames are pushed/popped on the FIBER, not on C. */
static BenderNativeStep bender_native_trampoline(BenderFiber *f, uint32_t **pc);
```

```
loop:
  d = bender_fiber_frame(f)->func->def
  if (!d->native_code
      || !bender_native_region_allowed(d->native_code, *pc - d->bytecode)
      || !bender_native_revalidate(d))                    return BENDER_NATIVE_VM;
  step = d->native_code->execute(f, d, pc);
  if (step == BENDER_NATIVE_FRAME) { maybe_collect(); goto loop; }      /* chain down */
  if (step == BENDER_NATIVE_VM
      && opcode_at(*pc) is JOP_RETURN/JOP_RETURN_NIL
      && bender_native_ret_pop(f, pc)) { maybe_collect(); goto loop; }  /* chain up */
  return step;
```

`vm_native()` then calls the trampoline instead of `execute` directly. Two
properties make this the right shape:

- **No new C frame per Bender frame.** A chain of ten native calls still has
  one C frame. No new stack-overflow class, no `bender_vm.stackn` pressure
  (`BENDER_RECURSION_GUARD`, `bender.h:296`), no new longjmp path. Contrast
  nesting `run_vm`, which is what `bender_call` does (`vm.c:2801`) and which
  would pin C frames and drag in the `bender_suspend_swap` chain
  (`vm.c:2810-2837`).
- **Every exit is still "unexecuted PC in a real frame".** The existing deopt
  contract is untouched: whatever the trampoline returns, `pc` and the frame
  chain are in a state `run_vm` already accepts.

**(e) Native return** (`bender_native_ret_pop`, new in vm.c) mirrors
`JOP_RETURN` (`vm.c:1896-1907`) exactly:

```c
Bender *s = f->data + f->frame;
Bender retval = s[D];                                /* D decoded from the ret word */
if (bender_stack_frame(s)->flags & BENDER_STACKFRAME_ENTRANCE) return 0; /* VM owns it */
if (NULL != bender_vm.profile_table) return 0;       /* profile_return must run in VM */
bender_fiber_popframe(f);
Bender *cs = f->data + f->frame;
uint32_t *cpc = bender_stack_frame(cs)->pc;          /* caller's committed call PC */
if ((*cpc & 0xFF) != JOP_CALL) return 0;            /* refuse anything unexpected */
cs[A_of(*cpc)] = retval;
bender_stack_frame(cs)->pc = cpc + 1;
*pc = cpc + 1;
return 1;
```

Refusing on `ENTRANCE` preserves `bender_call`'s contract (`vm.c:2791`,
`:1903`), and refusing under an active profiler preserves
`bender_profile_return` bookkeeping.

### 2.3 Arguments, locals, frames

- **Arguments**: nothing to map. `push*` already wrote them to
  `f->data[stackstart..stacktop)`; `bender_fiber_funcframe` (`fiber.c:765`)
  turns that region into B's frame slots in place (`nextframe =
  fiber->stackstart`, `fiber.c:770`) and nils the remaining slots
  (`fiber.c:786-789`: "Needed for gc correctness").
- **Locals**: stay boxed in the frame. No unboxed native ABI is introduced;
  that is the separate scalar/tuple-kernel work (background doc item 7).
- **Must a real `BenderFiber` frame still be pushed?** For v1: **yes,
  eagerly** — for reasons found in the code, not assumed:
  - GC marking of a fiber walks frames by `BENDER_FRAME_SIZE` arithmetic and
    `frame->prevframe` (`fiber.c:865`); values live only in C locals or in an
    unpushed window would not be marked.
  - Stack traces and error unwinding read `bender_stack_frame(...)->func/pc`;
    `bender_native_set_pc` (`vm.c:417`) exists precisely so executors keep
    panic locations exact. A lazily materialized frame would make a panic
    inside B report A's frame.
  - `bender_fiber_funcframe` *is* the arity gate (`fiber.c:776-778`, checked
    "before messing with state") and the vararg tuple builder
    (`fiber.c:801-819`).

  Lazy materialization is a v3 idea at most, and only for leaf callees that
  provably cannot panic, allocate, or be observed — a far smaller set than
  the hot edges in question.

---

## 3. Safety conditions

`bender_native_call_enter` must refuse (return 0, touching nothing) unless
*all* of the following hold. Each item names the code that forces it.

**Callee identity / dispatchability**

1. `bender_checktype(callee, BENDER_FUNCTION)`. Keywords must go to the VM
   (`JOP_CALL` resolves them with `resolve_method`, `vm.c:2292-2295`); so
   must cfunctions and non-functions (`vm.c:2352-2364`).
2. `def->native_code != NULL` **and** `bender_native_revalidate(def)` returns
   1 (`bytecode.c:2060`). Never trust `native_verified` directly; the epoch
   check inside `revalidate` must remain the authority.
3. `!(func->gc.flags & BENDER_FUNCFLAG_TRACE)` — otherwise `JOP_CALL` would
   run `vm_do_trace` (`vm.c:2326-2328`).
4. `NULL == bender_vm.profile_table` — `bender_profile_call`
   (`vm.c:2343-2347`) must observe every call; skipping it would corrupt
   auto-PGO evidence. A *profiling* build therefore never takes the
   continuation path (see §4 for the consequence).
5. `!(def->flags & (VARARG | NEEDSENV | STRUCTARG | NAMEDARGS))`, plus
   `def->min_arity <= argc && argc <= def->max_arity`. No vararg tuple is
   built (that path is guarded by the `VARARG` flag, `fiber.c:801`) and
   `bender_fiber_funcframe`'s arity gate (`fiber.c:776-778`) cannot fail.
   **Do not require `min_arity == max_arity`**: `&opt` parameters produce a
   non-vararg def with `min_arity < max_arity` (measured: `[a b &opt c]` →
   `arity 3, min 2, max 3, vararg false`), and the two hottest edges in ttfx
   are `&opt` functions (§5). Omitted optional arguments are nil-filled by
   `funcframe` (`fiber.c:786-789`), which is the same thing `JOP_CALL` does.
6. `def->environments_length == 0` — already implied by
   `native_structure_matches` (`bytecode.c:1995-1997`); assert anyway.
7. `bender_native_region_allowed(def->native_code, 0)` — the callee must be
   enterable at PC 0. `region-entry?` (`loop-executor.ben:114-118`) makes
   this true for selected definitions, but it must be *checked*.
8. No breakpoint bits. `native_structure_matches`' opcode allowlist already
   rejects them ("Includes every breakpoint bit", `bytecode.c:2047`), and
   `vm.c:1868-1875` bypasses executors entirely under
   `BENDER_FIBER_BREAKPOINT`; the trampoline must honour the same bypass.

**Type proof at the edge**

9. **v1 requires no type proof at all.** This is the important finding: B's
   executor is *itself* guarded. Arithmetic re-checks `NX_NUM`
   (`loop-executor.ben:195`), bitwise re-checks `NX_INT32` (`:201`), and
   `in`/`get`/`geti`/`next`/`put` all re-check the receiver and fall back at
   their own PC. A wrongly-typed argument therefore cannot miscompile — it
   makes B's executor exit to the VM at its own PC, which the trampoline
   handles as an ordinary `BENDER_NATIVE_VM` return. PGO type evidence is an
   **optimization selector** (which edges deserve the emitted fast path), not
   a correctness precondition. The type-proof requirement implied by the
   background doc belongs to the *kernel* tier in `callgraph.ben`, not to
   this tier.
   Any future unguarded/unboxed argument passing (v2.4 / v3) reverses this
   and then needs real proof: unanimous `:params` types plus `:mins`/`:maxs`
   ranges for *every* row of a source-verified bucket
   (`callgraph.ben:1716-1723`, validated at `:1731-1747`).

**GC / safepoints**

10. `maybe_collect()` (`vm.c:288-307`) must run after each frame push and
    each native return — exactly where `vm_checkgc_next()` /
    `vm_checkgc_pcnext()` run it in `JOP_CALL` / `JOP_RETURN`. Omitting it
    turns a long native chain into an unbounded nursery-growth window.
11. No GC write barrier is needed for the frame push itself: frame slots live
    in `fiber->data`, which is scanned, and `funcframe` nils unset slots. The
    `bender_gc_barrier` traffic visible in samples comes from table `put`
    (`bender_native_table_put_fast`), not from frames; this design does not
    change that.
12. Do **not** bump `bender_vm.native_epoch` on the continuation path (unlike
    `bender_native_alias_after_call`, `vm.c:425-428`): no arbitrary C callback
    ran, so cached verdicts remain valid. Bumping it per edge would
    reintroduce precisely the revalidation cost being removed.

**Interrupts / quantum**

13. Poll **once** per edge, inside `bender_native_call_enter`, *before*
    pushing the frame, via `bender_native_poll(1)` (`vm.c:375-382`, i.e.
    `vm_poll_condition`, `vm.c:373`: `auto_suspend` or
    `--quantum_remaining <= 0`). On a positive poll return 2 and let the
    executor `NX_EXIT(K, BENDER_NATIVE_INTERRUPT)` with the argument stack
    untouched, so the VM re-executes the call at PC K. This matches the
    existing discipline (`loop-executor.ben:393`, `:404`, `:418`, `:427`)
    and the "never poll twice" rule at `:425-426`: because `JOP_CALL`'s own
    `vm_maybe_auto_suspend(1)` (`vm.c:2287`) is skipped on this path, the
    edge must poll itself, exactly once.
14. `BENDER_NATIVE_INTERRUPT` propagation is already correct in `vm_native()`
    (`vm.c:356-359`): it sets `RESUME_NO_USEVAL | RESUME_NO_SKIP` and returns
    `BENDER_SIGNAL_INTERRUPT`. The trampoline returns the step unchanged.

**Depth**

15. Check `f->stack_overflowed` and `f->stacktop > f->maxstack` before each
    push, exactly as `JOP_CALL` does (`vm.c:2286`, `:2289`,
    `vm_maybe_overflow` at `vm.c:447-452`), and add a trampoline iteration
    budget (e.g. 64 chained frames) so a native recursion cannot monopolize
    the quantum; on exhaustion return `BENDER_NATIVE_VM` at the current
    unexecuted PC. `bender_vm.stackn` is *not* consumed — no C recursion
    occurs, which is a primary reason to prefer the flat trampoline.

**Deoptimization mid-call**

16. If B's executor hits any boundary (`put` growth, `mkarr`, `div`, a call
    it cannot handle, an unproven type) it does what it already does: exits
    at its own unexecuted PC with `BENDER_NATIVE_VM`. The trampoline returns,
    `vm_native()` adopts the frame via `vm_restore()`, and `run_vm` continues
    inside B. **Nothing needs rewinding** — the most valuable property of the
    existing executor contract, and the design is built to preserve it
    rather than to invent a deopt mechanism.
17. If B *panics*, the longjmp unwinds through the trampoline's single C
    frame. Because A's PC was committed with
    `bender_native_set_pc(f, d->bytecode + K)` before the push and B's frame
    carries its own `pc`, the stack trace and `BENDER_FIBER_DID_LONGJUMP`
    recovery (`vm.c:1843-1860`) see a perfectly ordinary two-frame chain.
    The trampoline must hold **no** state a longjmp would leak (no `gcroot`,
    no lock, no counter needing decrement) — a hard review rule.
18. If B yields/suspends it does so from `run_vm` after the trampoline has
    already returned (a yield is a signal op, never native), so no C frame is
    pinned. This is why the trampoline must never nest `run_vm`.

---

## 4. PGO integration

### 4.1 What already exists

- Per-executor thread-local counters `nx_<id>_entries`, `_edges`,
  `_fallbacks`, `_ops`, `_vmexits` (`loop-executor.ben:165-166`,
  `:183-192`), exposed as `executor-stats` / `executor-global-stats` /
  `executor-exit-stat` (`callgraph.ben:5533-5551`, registered at `:5641`).
- Global VM-exit accounting `bender_vm.native_vm_exits` and
  `bender_vm.native_exit_opcodes[opcode]` (`vm.c:354-355`), read via
  `bender_native_stat` / `bender_native_exit_stat` (`bytecode.c:2435`,
  `:2444-2446`).
- Attach census `native_attach_attempts/attached/ambiguous/nomatch`
  (`bytecode.c:2376-2410`).

So "per-definition VM-exit/native-entry counters" largely exist. What is
genuinely missing is **(callerDef, callPC) → callee attribution** and
**persistence into the gateable profile**. Also, `--release` forces
`BENDER_NATIVE_TELEMETRY=0` (background doc §1), so every `nx_*` counter
compiles out of release builds: feedback must come from the training build.

### 4.2 Correcting the profile-schema assumption

The profile's `:edges` field is **not** call edges. `vm.c:939-945` allocates
`bytecode_length * 2` integers — two *branch* counters per PC — and
`:receivers` (`vm.c:946-953`) is one type mask per PC;
`callgraph.ben:1663-1679` validates exactly that pairing
(`(= (length edges) (* 2 (length receivers)))`). Call structure comes from
static analysis instead (`call-map`, `callgraph.ben:1084-1100`). A new
field is required; do not overload `:edges`.

### 4.3 Proposed recording

Add `:calledges` to the profile entry table, allocated once per definition in
`bender_profile_entry` (`vm.c:843`, alongside `:edges`, under the same
`BENDER_PROFILE_EDGE_MAXPC` bound, `vm.c:752`) as an array of
`bytecode_length` entries, each

- `nil` — unobserved,
- `[calleeNameString totalCalls nativeCalls]` — monomorphic,
- `true` — open (more than one callee observed at this PC).

Increment it in `bender_profile_call` (`vm.c:2343`) using the already-cached
profile line (`bender_profile_hit`, `vm.c:833`) and the *caller's* committed
PC (`bender_stack_frame(...)->pc`, valid because `JOP_CALL` does `vm_commit()`
at `vm.c:2329` before the push). `nativeCalls` records whether the callee's
def had `native_code` at call time. Use `bender_table_put_raw` for the same
cross-heap reason documented at `vm.c:858-868`.

**Follow the shaping-evidence precedent, not the proof precedent.** The
`:elems` array (`vm.c:874-877`) is the model to copy: it is documented as
"Parallel element-type masks: for array params, the OR of the first elements'
types (heuristic sample…). **Evidence for SoA-kernel shaping, not proof.**"
`:calledges` is exactly that class — a per-PC heuristic that decides *where to
emit* a guard, never what the guard may skip. Give it the same framing in the
source comment, the same `BENDER_PROFILE_*` bound discipline, and the same
"consumers must still verify" contract, so no later reader mistakes an edge
count for a type or identity proof.

Validation mirrors `profile-rows`' existing strictness
(`callgraph.ben:1655-1690`): bounded length, integral non-negative counts,
`true`/`:open` tolerated, and a length disagreement with the definition is a
hard `profile-invalid`.

### 4.4 Selection in `callgraph.ben`

In the executor-selection block (`callgraph.ben:5490-5518`), after a
definition is accepted:

1. Build `continuation-pcs` for it: each PC whose `:calledges` entry is
   monomorphic with `totalCalls >= BENDER_EXECUTOR_EDGE_MIN_CALLS` (a new env
   knob with the same shape as `BENDER_EXECUTOR_LEAF_MIN_CALLS`,
   `:5481-5484`). Resolve the callee name through `name-ids`, require exactly
   one id (the same anti-ambiguity rule as the `name-duplicate` guard at
   `:5516` and `try_attach`'s ambiguity refusal), and require that callee to
   be in the selected `executors` set.
2. Cross-check the static `call-map` entry for that (id, PC). Disagreement
   means the slot is not statically monomorphic → do not emit the fast path.
3. Pass `continuation-pcs` to `loop-executor/emit` and emit the fast path
   **only at those PCs**. Keep the plain `NX_EXIT(K, BENDER_NATIVE_VM)`
   everywhere else, so generated-code size stays bounded and profile-free
   builds remain byte-identical to today — which the existing greps in
   `test/suite-native-executor.sh` depend on.
4. Print one `native-continuation <callerId> <pc> <calleeName> <calls>` line
   per selected edge and record the same lines in a manifest beside
   `native-executors.txt` (`callgraph.ben:5520-5522`) so test gates can
   assert on them.

Selection only chooses *where the guard is emitted*; the runtime still proves
the callee (§3), so a stale or wrong profile can only cost performance, never
correctness.

---

## 5. Scope for v1, and expected win

**v1 — minimal safe subset**

- Non-tail `call` only (`JOP_TAILCALL` stays a hard boundary,
  `loop-executor.ben:44`), and not the terminal instruction (`:380`).
- Callee: Bender function, `min_arity <= argc <= max_arity` (`&opt` allowed,
  §3.5), no vararg/structarg/namedargs/env, verified
  `native_code`, PC-0 region bit set, untraced, no active profiler.
- Eager `bender_fiber_funcframe` push — real frame, real arity gate, real
  stack trace.
- Flat trampoline, no C recursion, iteration budget 64.
- Native return only for non-`ENTRANCE` frames with no active profiler, and
  only when the caller's committed word is a `JOP_CALL`.
- One poll per edge; `maybe_collect()` per push and per pop; no epoch bump.
- No type proof required; no unboxing; no argument-representation change.

**v1 edge targets (exact, from §1.4).** These are the edges that must be
covered for v1 to be worth shipping; everything else can stay on the VM path
until the A/B justifies widening:

| Caller (scene.ben) | Call site | Callee | Frequency | Notes |
|---|---|---|---|---|
| `ctx-update` | `:640` | `ctx-motion-move` | per live char per tick | guarded by `(ctx :path-live)` |
| `ctx-motion-move` | `:446` | `ctx-path-step` | per moving char per tick | **2nd level of the chain** |
| `ctx-path-step` | `:436` | `ctx-coord-on-line` | per moving char per tick | 3rd level; bezier variant at `:437` |
| `ctx-update` | `:641` | `ctx-step-animation` | per live char per tick | small common branch |
| `ctx-update` | `:646` | `ctx-char-active?` | per active id per tick | missing from the background doc's list |

Eligibility against §3: all five are plain `defn`s with fixed positional
parameters. `ctx-motion-move` and `ctx-step-animation` are
`[ctx id &opt effect]`, i.e. **non-vararg defs with
`min_arity < max_arity`** (measured: `[a b &opt c]` → `arity 3, min 2, max 3,
vararg false`), and both call sites do pass the optional argument
(`:640`, `:641`). This is why §3.5 must be stated as
`min_arity <= argc <= max_arity` rather than `min_arity == max_arity`: the
stricter rule would refuse the two hottest edges in the whole program.
`ctx-path-step [ctx p a]`, `ctx-coord-on-line [ctx scol srow ecol erow t]`
and `ctx-char-active? [ctx id]` are strict fixed arity.

**Expected measurable win on the named edges.** Honest accounting, per
`ctx-update → ctx-motion-move` / `ctx-step-animation` / `ctx-path-step` call,
what is removed: one interpreted `JOP_CALL` dispatch (including the
6-condition alias probe), one interpreted `JOP_RETURN`, two `vm_native()`
gate passes, two executor-prologue computed-goto re-entries, one duplicated
interrupt poll, and — the part most visible in release samples — up to two
whole-definition `native_structure_matches` passes (full bytecode `memcmp`
plus per-word opcode walk) per edge whenever the epoch has moved.
`bender_fiber_funcframe` cost is **not** removed; it is still called.

**Revised estimate with the survey measurement.** The parallel survey puts
motion at **~72.8% of frame time and ~31x off Rust**. That changes the
arithmetic materially versus the first pass of this document, because the
targeted subtree is no longer a minor slice:

- The motion subtree is a **3-deep chain** (§1.4), so per moving character per
  tick v1 removes on the order of *three* `JOP_CALL`/`JOP_RETURN` pairs and
  *six* `vm_native()` gate passes, not one pair and two gates.
- Those callees are small (§1.4 item 3), so the removed fixed cost is a large
  fraction of their total cost rather than amortized over a long body.
- Amdahl: if motion is 72.8% of the budget and continuation removes a
  fraction *f* of motion's cost, end-to-end gain is `0.728 · f`. For the
  mechanism to deliver 10% end-to-end it must remove ~14% of motion's cost;
  for 20% it must remove ~27%.

Given the chain depth and the smallness of the callees, **removing 15-35% of
motion's cost is plausible, i.e. 10-25% end-to-end on motion-dominated
effects** (`binarypath`, `rings`, `waves`), and near-zero on effects dominated
by `ctx-frame-string` and table traffic. That is a step change compared to the
packing work (0.974-0.989 geomean each, background doc "What was already
optimized"), and it is consistent with the survey's conclusion that
continuation is the dominant remaining term.

Three honest bounds on that number:

1. `bender_fiber_funcframe` + `popframe` remain on every edge and are
   themselves visible in release samples. Only v2.2 (inline cache) and v3
   (lazy frames) attack those.
2. The 31x gap on motion is not all dispatch. Boxed doubles, boxed `[col
   row]` tuple returns from `ctx-path-step`/`ctx-coord-on-line`
   (`scene.ben:410`, `:291`), and the `E/ease` cross-module call carry cost
   that continuation does not touch. The scalar-pair-return ABI (background
   doc item 7) is the complementary feature and should be sequenced right
   after this one.
3. The ~22x whole-CLI gap will move much less than the motion number,
   because effects not dominated by motion are unaffected. Report both
   per-effect and geomean; do not quote the motion-effect figure as a CLI
   figure. Any claim beyond these ranges must come with the A/B.

**v2 extensions** (each independently gated and A/B'd)

1. **Tail calls**: `bender_fiber_funcframe_tail` (`fiber.c:905`) in the
   trampoline. It memmoves args down to the frame base, so the executor
   contract requires re-reading `s` afterwards. Enables native mutual
   recursion with no frame growth.
2. **Monomorphic per-PC inline cache**: a `BENDER_THREAD_LOCAL` (same reason
   as `bender_profile_cache`, `vm.c:771-776`) `{BenderFuncDef *def; const
   BenderNativeCode *code; uint64_t epoch;}` per emitted continuation PC, so
   a hit skips `revalidate` entirely. Invalidated by `bender_native_epoch`.
3. **Native `ret` for `ENTRANCE` frames**, via a distinguished step that
   `vm_native()` converts into `vm_return_no_restore(BENDER_SIGNAL_OK, ...)`
   (`vm.c:285`).
4. **Callee-side specialization**: when an edge is type-proven by PGO, emit a
   second entry point of B that skips its leading `NX_NUM` guards. This is
   the first point where type proof becomes a correctness requirement.
5. **Lazy frame materialization** for provably panic-free, allocation-free
   leaf callees. Highest risk, lowest expected value; last.

---

## 6. Risk register and parity gates

| # | Failure mode | Mechanism that prevents it | Gate |
|---|---|---|---|
| R1 | Call executed **twice** (fast path half-commits, VM then re-runs `JOP_CALL`) | `call_enter` is all-or-nothing; it touches nothing before every check passes | Fixture: callee with an observable side effect (buffer append) called from an executor; assert exactly one append |
| R2 | Call **skipped** (frame pushed but step value lost, VM re-dispatches at PC+1) | Only `BENDER_NATIVE_FRAME` may change frames; `vm_native()` must `vm_restore()` on it | Byte parity; plus the counter-identity check below |
| R3 | `func` register **stale** after a frame change | Today's `vm_native()` never reloads `func` (`vm.c:339-366`); forgetting `vm_restore()` executes B's bytecode against A's def → arbitrary miscompile | Unit test: 2-frame native chain that boundary-exits *inside* B; assert B's constants are read, not A's |
| R4 | Profiler/trace evidence silently dropped | §3.3, §3.4 refusals | `test/suite-profile.ben`, `test/suite-dev-profile.ben`, `test/suite-autopgo.ben` unchanged-green |
| R5 | Missed GC safepoint → nursery blowup under a long chain | `maybe_collect()` per push/pop + iteration budget | `test/suite-gc-stress.ben` plus an allocating-callee chain fixture |
| R6 | Interrupt/quantum starvation across a native chain | One poll per edge + budget | `test/suite-aot-quantum.ben` |
| R7 | Wrong return slot (A field decoded from the wrong word) | `ret_pop` decodes A from the caller's committed word and refuses unless it is `JOP_CALL` | Fixture with a `tcall` and with a non-call word at the caller PC; assert refusal |
| R8 | Fiber corruption on panic inside B | Eager frame push; no trampoline-local state across longjmp | `test/suite-error.ben` plus a fixture panicking 3 frames deep in a native chain, checking `debug/stacktrace` text |
| R9 | Debugger divergence | Honour the `BENDER_FIBER_BREAKPOINT` bypass (`vm.c:1868`) in the trampoline | `test/suite-debug.ben` |
| R10 | Stale descriptor executed after a def change | Never cache past `bender_native_epoch`; `revalidate` per trampoline iteration in v1 | `test/native-executor-marshal-probe.c`, unmarshal/attach suites |
| R11 | Generated-C regression in profile-free builds | Fast path emitted only at profile-selected PCs | `test/suite-native-executor.sh` (exact greps + `native-executors.txt` manifest), `test/suite-callgraph.ben` |
| R12 | Ambiguous callee identity attaches the wrong descriptor | Runtime proof is `bender_native_matches`; selection additionally requires a unique `name-ids` id | `test/suite-callgraph.ben` selection gate |

**Byte-parity gates — two tiers, and the distinction matters.**

*In this repository*: `test/suite-native-executor.sh` (emits a native lib
from `test/fixtures/native-executor.ben`, greps the emitted C for each fast
path, then builds and runs an AOT image), `test/native-executor-opcodes.ben`,
`test/suite-native-fiber.ben`, `test/suite-aot-release.ben`,
`test/suite-aot-quantum.ben`, `test/suite-autopgo.ben`.

*The 37-effect byte-parity harness is **not** in this checkout.* It lives in
the ttfx tree at
`/Users/pankajdoharey/Development/Projects/ML/ttfx/bender-port/`, which is a
separate repository — there is no `bender-port/` directory inside the bender
checkout, and the background doc's `bender-port/src/...` references must be
read as relative to that external tree. The in-repo template for the protocol
is
`test/manual/stage2-omarchy-parity.sh`, which points at
`/Users/pankajdoharey/Development/Projects/ML/ttfx/bender-port` and does
exactly the right thing: build the *same* source twice — reference
(`--no-auto-pgo`, no `BENDER_NATIVE_EXECUTORS`) and candidate
(`BENDER_NATIVE_EXECUTORS=1`) — then `cmp` `--dump-frames` output over
effects × seeds. The gate for this feature is that protocol extended to all
37 effects × ≥6 seeds × 400 frames with the continuation fast path on,
requiring **37/37 byte-identical output against the reference build**. The
known `swarm` divergence in the background doc is Bender-vs-*Rust*, not
Bender-vs-Bender; the Bender-vs-Bender gate must be 37/37 exact. Because the
fallback contract guarantees exactness, any single-byte divergence is a real
bug, never noise. Timed A/B is a separate later step on the same PGO record.

**Counter-identity gate.** With telemetry on, for the involved definitions:
`continuation edges taken + interpreted JOP_CALL count == profiled :calls
total`. This catches R1 and R2 directly, which byte parity can miss when the
callee is pure.

---

## 7. Implementation plan

Ordered; each step independently buildable and testable.

| Step | Files | Rough size | Content |
|---|---|---|---|
| 1 | `src/include/bender.h` (~1283, ~1300-1360) | ~15 lines | Add `BENDER_NATIVE_FRAME`; declare `bender_native_call_enter` (the trampoline stays static in vm.c) |
| 2 | `src/core/vm.c` (after `bender_native_alias_after_call`, ~428) | ~120 lines | `bender_native_call_enter` (all §3 refusals, poll, overflow checks, `funcframe`, `maybe_collect`) and `bender_native_ret_pop` |
| 3 | `src/core/vm.c` `vm_native()` (~339-366) | ~25 lines | Third arm (`vm_restore()` + `pc = npc` + `maybe_collect()`), breakpoint bypass preserved; new `bender_vm` counters `native_continuations`, `native_continuation_refusals` |
| 4 | `src/core/vm.c` | ~60 lines | `bender_native_trampoline` loop; route `vm_native()` through it |
| 5 | `src/core/bytecode.c` (~2420-2450) | ~10 lines | New `bender_native_stat` indexes for the two counters |
| 6 | `tools/loop-executor.ben` `call` emitter (~379-433) | ~25 lines | Emit the fast path, gated on a new `continuation-pcs` argument to `emit`; empty default keeps output byte-identical |
| 7 | `test/fixtures/*`, `test/suite-native-executor.sh` | ~80 lines | Executor→executor fixtures (side-effecting callee, panicking callee, boundary-in-callee callee, 3-deep chain, `tcall` caller word); greps for `bender_native_call_enter`; counter-identity assertion |
| 8 | `src/core/vm.c` `bender_profile_entry` / `bender_profile_call` (~843, ~2343) | ~50 lines | `:calledges` allocation and monomorphic recording |
| 9 | `tools/callgraph.ben` (`profile-rows` ~1655-1690, `profile-index` ~1716) | ~40 lines | Validate and index `:calledges` |
| 10 | `tools/callgraph.ben` (~5490-5522) | ~60 lines | Edge selection, static `call-map` cross-check, `native-continuation` manifest, `BENDER_EXECUTOR_EDGE_MIN_CALLS` |
| 11 | `test/suite-callgraph.ben`, `test/suite-autopgo.ben` | ~60 lines | Selection and profile-schema gates |
| 12 | ttfx tree (external) | — | 37-effect × ≥6-seed Bender-vs-Bender byte parity, then timed A/B on the same PGO record |
| 13 | `docs/internals/` | ~40 lines | Record the measured A/B — accept or reject with the same discipline as the packing table |

Steps 1-7 deliver a correct, byte-parity-gated mechanism with an always-on
guard at every non-terminal call PC (acceptable: ~6 loads/branches on
refusal). Steps 8-11 narrow emission to hot edges and make selection
profile-driven. Step 12 decides whether it ships.

---

## 8. What continuation changes for packed-state API design

A parallel survey recommends **no accessor/getter functions in packed state**,
on the grounds that every cross-function call is a VM exit today. That
recommendation is correct *today*, and the code says precisely why — but the
reasoning is sharper than "calls are slow", and continuation does change the
calculus.

**Why index access is already fast and getters are not.** The ttfx packed
state is a mutable array with named constant indices:
`motionpath-new` returns `@[name speed ease hold loop layer …]`
(`scene.ben:195`) with `(def path-step 9)` etc. (`:179`). So `(p path-step)`
is a **container-as-function call with one constant integer argument**, which
`loop-executor.ben:416-422` already executes natively inside an executor via
`bender_native_container_call_fast` — no frame, no VM exit, and `put p path-step
v` goes through `bender_native_array_put_fast` (`:333-341`). The same is true
for `(a :cur-col)` style table access via `bender_native_table_get_fast`
(`:294-298`), though table `put` only stays native for an
*already-present primitive key* (`:333-341`).

A getter `(defn path-step [p] (p path-step))` turns that zero-exit read into
case 7 of §1.2: a full VM exit, `JOP_CALL`, frame push, callee re-validation,
`JOP_RETURN`. The penalty is not "a function call" — it is **two `vm_native()`
validation gates and two interpreted dispatches for one array load**. Note the
port already contains such accessors (`ctx-path-waypoints`,
`ctx-path-set-speed`, `ctx-path-reset-hold`, `ctx-path-set-hold`,
`scene.ben:196-203`); they are correctly kept off the per-tick path.

**What v1 continuation changes.** It removes the interpreted `JOP_CALL`/
`JOP_RETURN` pair and the redundant validation gates, leaving
`bender_fiber_funcframe` + `popframe` + one executor entry per accessor call.
That makes accessors roughly comparable to a small cfunction call — far better
than today, but still strictly worse than the inline container access, which
costs ~zero. **So v1 does not make per-tick accessors free, and the survey's
recommendation stands for v1.** What it does buy is that accessors stop being
*catastrophic*: an accessor on a warm-but-not-hottest path becomes an
acceptable abstraction instead of a 50x cliff.

**What makes accessor-based APIs genuinely viable** is the combination of two
later items, and this is the honest sequencing answer:

1. **v2.2, the monomorphic per-PC inline cache** — removes
   `bender_native_revalidate` from the edge, leaving only the frame push/pop.
2. **v3 lazy frame materialization** (§2.3) — and an accessor is exactly the
   callee class where it is *actually* sound: a one-expression body that
   cannot panic (an in-range constant index on a known array), cannot
   allocate, captures nothing, and is never on a stack trace anyone reads.
   Accessors are the motivating use case for lazy frames, and conversely
   lazy frames are what make accessors cost-free.

With both, a packed-state accessor collapses to an inlined guarded load —
i.e. the same code the executor emits for `(p path-step)` today. **Design
recommendation:** keep packed state index-addressed with `def`-named constant
indices for now (it is already the fastest form available), but stop treating
accessor-based APIs as permanently disqualified — re-evaluate after v2.2
lands, and treat "accessor calls are free" as a v3 deliverable with its own
A/B rather than an assumption. Do not restructure the ttfx state model on the
promise of this feature until the measurement exists.

---

## Corrections to the background doc

1. **"Exits to `run_vm` at the call PC and re-enters"** — correct, but the
   call emitter already has four native fast paths (core predicates,
   `buffer/push-string`, container-as-function, FFI-alias cfunction;
   `loop-executor.ben:386-433`). A Bender callee with its own executor hits
   none of them. A *terminal* call is always owned by the VM (`:380-381`).
2. **"State spill"** — there is none. Executors keep every live value boxed
   in the GC-scanned VM frame continuously; the only raw doubles are in the
   `numeric-sum` special case and are written back before every exit. The
   real cost is dispatch + whole-definition revalidation + frame bookkeeping.
3. **"PGO should mark hot call edges"** — the profile has no call-edge field
   today. `:edges` is two *branch* counters per PC and `:receivers` a per-PC
   type mask (`vm.c:939-953`, validated at `callgraph.ben:1663-1679`). A
   new `:calledges` field is required (§4.3).
4. **"Add per-definition VM-exit/native-entry counters"** — they already
   exist: `nx_<id>_entries/_edges/_fallbacks/_ops/_vmexits`
   (`loop-executor.ben:165`) via `executor-stats`
   (`callgraph.ben:5533`), plus global `native_vm_exits` and per-opcode
   `native_exit_opcodes` (`vm.c:354-355`, `bytecode.c:2435-2446`). Missing is
   per-*edge* attribution and persistence into a gateable profile. Also
   `--release` sets `BENDER_NATIVE_TELEMETRY=0`, so these are
   training-build-only.
5. **"Chain native frames where types … are proven"** — type proof is *not*
   needed for v1 correctness. Callee executors are self-guarding and deopt at
   their own PC, so type evidence selects which edges get a fast path.
   Proof becomes mandatory only when arguments are passed unboxed or callee
   guards are elided (v2.4 / v3).
6. **A static native-to-native direct C call is not possible for the
   EXECUTOR tier** (the scope of this document): descriptor identity is
   structural and resolved at runtime by
   `bender_native_matches` / `bender_native_try_attach`
   (`bytecode.c:2050`, `:2376`), and the callee lives in a runtime slot. The
   continuation must be a runtime-resolved trampoline (§2.1).
   ERRATUM (see typed-path-capability-audit.md): the KERNEL tier (Tier A,
   `emit-fn-c`) makes exactly such static calls today — kernels are emitted
   non-`static` into one TU and call each other as literal C calls, inlined
   at -O3. Do not read this item as a blanket toolchain statement. The same
   audit found 1–8 lines of citation drift in this document; symbol names
   are authoritative, line numbers approximate.
7. **Executor selection is not purely structural today**: loop executors are
   (`loop-executor.ben:77`), but a second *leaf* (straight-line) tier is
   already PGO-gated by `BENDER_EXECUTOR_LEAF_MIN_CALLS`
   (`callgraph.ben:5481-5497`). Continuation selection should follow that
   precedent rather than invent a new mechanism.
8. **The 37-effect parity harness is not in this repository**, and neither is
   the ttfx port. `bender-port/` lives at
   `/Users/pankajdoharey/Development/Projects/ML/ttfx/bender-port/`, so the
   background doc's `bender-port/src/scene.ben` references are paths in that
   external tree. The in-repo analogue of the harness is
   `test/manual/stage2-omarchy-parity.sh` (2 effects × 6 seeds, absolute
   paths into the ttfx tree). Any "37/37 parity" claim for this feature must
   be produced there using that protocol.
9. **The hot-edge list is incomplete and understates the chain depth.**
   `ctx-char-active?` (`scene.ben:612`, called from `ctx-update:646`) is a
   third per-character edge the doc omits, and motion is a 3-deep chain
   (`ctx-update → ctx-motion-move → ctx-path-step → ctx-coord-on-line →
   geom-lerp-point`), not a flat set of calls from `ctx-update` (§1.4). Also,
   the hot functions are in `scene.ben`, not `canvas.ben`, which only
   holds the state constructors.
10. **"Chain native frames … where types are proven" plus "no accessors in
    packed state" interact.** The no-accessor rule is right today and right
    for v1, but for a mechanical reason worth recording: packed index access
    `(p path-step)` is *already* native inside an executor
    (`bender_native_container_call_fast`), so a getter does not replace a cheap
    call — it replaces a zero-exit inline load. Accessor-based APIs become
    viable only after v2.2 plus v3 (§8), and that should be measured, not
    assumed.

---

## Implementation Notes (v1 as built, 2026-10-23)

Code landed in `b5d87d06` (VM mechanism), `786b2c00` (executor emission),
`3db27d20` (tests). This section records where the build diverged from the
design above, and the measured outcome. **Where they disagree, the code and
these notes are authoritative.**

### What was built

Exactly §2's flat trampoline, §3's refusal list, and §5's v1 scope.
`BENDER_NATIVE_FRAME`, `bender_native_call_enter`, `bender_native_ret_pop`,
`bender_native_trampoline` (budget 64, no C recursion) and the third
`vm_native()` arm. Steps 8-11 of §7 (`:calledges` in the profile, profile-driven
`continuation-pcs` selection) were **not** built: emission is unconditional at
every non-terminal call PC, as §7 says steps 1-7 deliver.

### Divergences from the design text

1. **`vm_native()` reloads `func` unconditionally, not only on
   `BENDER_NATIVE_FRAME`.** §2.2c proposes a third arm doing `vm_restore()`.
   The trampoline can change frames and still return `BENDER_NATIVE_VM` (it
   chains up through a native return, or runs out of budget inside a chained
   callee), so a step-value-keyed restore would be wrong. The macro now always
   does `stack = fiber->data + fiber->frame; func = bender_stack_frame(stack)->func;`.
   This is inside the block that only runs when an executor actually
   dispatched, so it is per executor-exit, not per instruction. **R3 is closed
   by this line.**

2. **`bender_native_ret_pop` decides everything before the pop.** §2.2e's
   sketch pops the frame and then inspects the caller's word, which cannot
   refuse cleanly if the word turns out not to be a `JOP_CALL`. The built
   version reads the caller frame through `frame->prevframe` first and refuses
   with the fiber untouched. It also refuses `BENDER_STACKFRAME_TAILCALL`
   frames and `prevframe < BENDER_FRAME_SIZE` explicitly.

3. **The chain-up and chain-down paths share one re-gate.** After either
   transition the trampoline re-runs `region_allowed` + the epoch-cached
   `revalidate` for the new frame. `maybe_collect()` can run a finalizer, so
   re-proving after a push is not redundant.

4. **A native return does not require a continuation to have happened.**
   `ret_pop` fires whenever an executor stops at an unexecuted `ret`/`retn`
   and the caller's committed word is a `JOP_CALL` — including frames the
   interpreter pushed. On ttfx decrypt this is 45,719 edges even with zero
   chained calls, and 150,549 with the leaf tier on.

5. **New: `BENDER_NATIVE_NO_CONTINUATION`.** A runtime off-switch read once in
   `bender_init`. The design has no equivalent; it exists so one binary can run
   both halves of an A/B with a provably identical executor set, which turned
   out to matter (see the measurement).

6. **Telemetry.** Four `bender_vm` counters (`native_continuations`,
   `native_continuation_refusals`, `native_returns`, `native_interp_calls`) at
   `bender_native_stat` indexes 5..8 and `executor-global-stats` 6..9, plus a
   per-definition `nx_<id>_conts` at `executor-stats` index 5. The global
   `native_interp_calls` increment sits in `JOP_CALL`'s Bender-function branch;
   it is one increment next to `bender_fiber_funcframe` and is the only
   always-on cost this work adds to the interpreter.

### Environment finding that invalidated the first measurement round

`BENDER_NATIVE_EXECUTORS` is **not** set by this repository's `Makefile`.
A plain `make` produces a runtime in which the whole executor tier is inert:
`bender_native_register` refuses every descriptor, `bender_native_stat` returns
zero, and `test/suite-native-executor.sh` fails at its AOT-image assertions.
`docs/internals/native-loop-executors.md` states both opt-ins are required;
it is easy to satisfy only the environment-variable one. Every result below
used `make CFLAGS="-O2 -g -DBENDER_NATIVE_EXECUTORS"`. With that flag,
`make test` and `test/suite-native-executor.sh` are green at `HEAD` and after
this change.

### Fixture finding: structural ambiguity silently disables a chain fixture

Attachment is a structural match, and `bender_native_try_attach` refuses an
*ambiguous* one. A natural set of chain fixtures —
`(while (< i n) (+= s (callee i)) (++ i))` with different callees — compiles
to byte-identical definitions (the callee is an opaque constant), so none of
them attach and the fixture proves nothing while appearing to pass. The
committed fixtures therefore carry deliberately distinct shapes. Their `&opt`
tails also exercise §3.5's `min_arity < max_arity` case with the optional
arguments omitted.

### Validation

All gates below ran on `ruby-port/logo.txt`, `COLUMNS=80 LINES=24`,
`BENDER_NO_PROFILE=1`, 6 seeds (42, 1337, 7, 13, 99, 2026), 400 frames.

| Gate | Result |
|---|---|
| `make test` (executor build) | green |
| `test/suite-native-executor.sh` (incl. ASan/UBSan matrix) | green |
| In-repo chain fixture, continuation on vs off | identical values; identity 306 == 306; 88 edges vs 0 |
| ttfx 37-effect Bender-vs-Bender, loop tier only | **37/37 exact, 2208/2208 cases** |
| ttfx 37-effect Bender-vs-Bender, leaf tier (`--pgo`, 96 executors both sides) | **37/37 exact, 2208/2208 cases** |
| Counter identity, decrypt, loop tier | 653,838 == 653,838 (0 chained edges) |
| Counter identity, decrypt, leaf tier | **653,839 == 653,839** (90,041 chained edges on, 0 off) |

Executor binding reports were identical on both sides of every A/B pair
(81/81 then 96/96 bound, **no refusals on either side**).

### The result: no measurable speedup

Two independent timing legs, 5 paired alternating `--bench` rounds, medians:

*Cross-binary (candidate vs pre-change build, same compiler lineage, identical
96-executor sets):* **geomean candidate/baseline = 1.0100**. Motion-dominated
effects: `binarypath` 1.000, `rings` 1.021, `waves` 1.011 — i.e. at or just
below parity, not the predicted 10-25%.

*Same binary, continuation toggled at runtime (the cleaner comparison: byte-identical
code, identical executor set, only the `bender_vm.native_continuation` load differs):*

| effect | cont ON ms | cont OFF ms | ratio |
|---|---|---|---|
| binarypath | 901.8 | 898.6 | 1.004 |
| rings | 2478.6 | 2488.7 | 0.996 |
| waves | 372.0 | 374.8 | 0.992 |
| decrypt | 334.5 | 334.3 | 1.001 |
| swarm | 396.7 | 394.4 | 1.006 |
| matrix | 15610.7 | 15652.3 | 0.997 |
| fireworks | 432.4 | 431.3 | 1.002 |
| laseretch | 402.6 | 403.0 | 0.999 |

**geomean ON/OFF = 0.9996.** That is a null result inside noise, and it is the
figure to believe: the 1.0100 cross-binary number is build-to-build variation
(code layout), since the same-binary comparison on the same effects shows no
such gap.

**§5's estimate of 10-25% on motion-dominated effects is not supported by the
measurement and should be treated as withdrawn.** The honest reading is
§5's own bound 1: `bender_fiber_funcframe` + `bender_fiber_popframe` stay on
every edge, and the epoch-cached `revalidate` still runs per trampoline
iteration. What v1 removes — `JOP_CALL`/`JOP_RETURN` dispatch, the alias
probe, the duplicated poll, two executor prologue re-entries — is apparently
small next to what it keeps. Nothing here says the mechanism is wrong; it says
the mechanism alone is not where the time is.

### Why so few edges chain, measured

Refusal reasons on decrypt, loop tier only (throwaway instrumentation,
563,564 refusals):

| reason | count | share |
|---|---|---|
| callee is not a `BENDER_FUNCTION` (cfunction, keyword, container) | 447,151 | 79% |
| callee has no `native_code` | 114,956 | 20% |
| callee not region-allowed at PC 0 | 1,060 | 0.2% |
| argc outside `[min_arity, max_arity]` | 396 | — |
| captured environments | 1 | — |

So with only the loop tier, **nothing chains at all**: the hot callees are
small straight-line functions, and the straight-line (leaf) tier is gated by
`BENDER_EXECUTOR_LEAF_MIN_CALLS`, which is unset by default. Continuation has
nothing to chain into until that tier is enabled, which is a real dependency
the design does not call out. With `--pgo` plus `BENDER_EXECUTOR_LEAF_MIN_CALLS=500`
(96 executors, 15 leaf), 90,041 edges chain on decrypt — and the timing is
still flat.

### What this implies for the sequencing in §5 and §8

The next features should be chosen on the evidence that frame bookkeeping,
not dispatch, is the residual cost: v2.2 (the monomorphic per-PC inline cache,
which removes `revalidate` from the edge) and v3 (lazy frames, which removes
`funcframe`/`popframe`) attack what v1 left behind, whereas v2.1 (tail calls)
and v2.3 do not. §8's conclusion is unchanged and now has a measurement behind
it: v1 does **not** make per-tick accessors cheap, because the frame push and
pop it keeps are exactly their cost.

### Limitations

- No per-edge profile field (`:calledges`) and no profile-driven emission
  narrowing: the guard is emitted at every non-terminal call PC. Generated-C
  size grows accordingly, and profile-free builds are **not** byte-identical
  to pre-change output (R11's "no change without a profile" property was a
  property of steps 8-11, which are not built).
- The 37/37 parity and counter-identity runs used 6 seeds x 400 frames on one
  input; `--rust` was not run (this feature's gate is Bender-vs-Bender).
- Timing was measured on one machine, 5 rounds, medians, no CPU pinning.
- `bender_vm.native_interp_calls` is incremented unconditionally in `JOP_CALL`.
  It is one increment beside `bender_fiber_funcframe`; it was not separately
  A/B'd and is assumed below the noise floor shown above.

### Correction: the ~1% is guard cost, not noise (2026-10-23, later)

The validation workstream established the gate's noise floor twice
independently, using pairs whose generated `native.c` was **byte-identical**:
**+/-3% per effect, 0.1-0.3% geomean** (`ML/ttfx/bench/bender-37-gate.md`;
geomean 0.9992 over 37 effects and 1.0025 over 5). That floor does not license
calling this change's cross-binary geomean noise, because this change's two
sides do **not** share generated C -- the candidate emits a continuation
attempt at every non-terminal call PC. Re-measured on the same
executor-equal 96/96 pair, 7 paired alternating rounds:

| effect | candidate ms | baseline ms | ratio |
|---|---|---|---|
| beams | 298.0 | 294.0 | 1.014 |
| binarypath | 1341.3 | 1408.7 | 0.952 |
| decrypt | 323.9 | 316.6 | 1.023 |
| fireworks | 433.6 | 431.6 | 1.005 |
| laseretch | 419.8 | 411.5 | 1.020 |
| rings | 2561.3 | 2551.4 | 1.004 |
| swarm | 401.9 | 397.4 | 1.011 |
| waves | 383.0 | 373.9 | 1.024 |

**geomean 1.0064** (parity on this subset re-verified: 8/8 exact, 59/59 cases).
With the 37-effect 5-round run's 1.0100, two independent runs put the
cross-binary cost at **+0.6% to +1.0% geomean** -- above the 0.1-0.3% geomean
floor, though individual effects (`binarypath` 1.000 then 0.952) move well
inside the +/-3% per-effect floor and must not be read individually.

That gives a clean decomposition, because the two legs isolate different
things:

| leg | what differs | geomean | reads as |
|---|---|---|---|
| same binary, `BENDER_NATIVE_NO_CONTINUATION` toggled | only whether edges are *taken*; guard code present in both | 0.9996 | **chain benefit ~= 0** |
| candidate vs pre-change build, identical executor sets | guard emission + the `JOP_CALL` counter increment + the trampoline indirection, *and* edges taken | 1.0064 / 1.0100 | **always-on cost ~= +0.6-1.0%** |

So the earlier "build-to-build layout variation" reading was too generous and
is withdrawn: the honest statement is that **v1 as built is a small net
regression**, because it pays an unconditional guard at every non-terminal
call PC and the chaining it enables buys nothing measurable back. This is a
direct, quantified argument for design §7 steps 8-11 (`:calledges` plus
profile-narrowed emission at selected PCs only): narrowing emission removes
the measured cost term while keeping every edge that actually chains. v1
should not be enabled by default until that lands or the frame-bookkeeping
work (v2.2/v3) makes the chain itself pay.

### Reconciliation with the two reported registration failures

The workstream reports (a) `test/suite-native-executor.sh` red at `6cdb7857`
with four refusals and the `assert failure in (> (counts 0) 0)` at
`test/fixtures/native-executor.ben:50`, and (b) `188:int-string?` /
`190:to-i` refused "in every ttfx build", with an exact correspondence between
the refused ids and the only manifest entries whose `defn` lives in the AOT
entry file -- those being the only ones `tools/mkimage.ben` (~205-220)
re-emits with an eager `executor-register id fn`, while everything imported
from `src/` takes the lazy `bender_native_registry_add` + structural-attachment
path.

**The correspondence is real and the eager path is indeed the only one that
reports a refusal at all** (the lazy path is silent). Two pieces of evidence
from this work refine the cause:

1. **The red suite is explained by the missing build opt-in, not by the eager
   path being broken.** Reproduced identically on a pristine `66a5cf9a`
   worktree, and the four refused ids are 10, 11, 13 and 14 -- exactly
   `numeric-sum`, `numeric-nested`, `unsupported-loop` and `numeric-fraction`,
   the entry-file `defn`s of `test/fixtures/native-executor.ben`, while the
   imported `native-executor-mixed` ids (1, 2, 4, 7, 8) print nothing. That is
   the predicted signature of a runtime where `bender_native_register`
   *always* returns 0: without `BENDER_NATIVE_EXECUTORS` it is compiled to the
   refusing stub, so **every** registration fails and only the eager ones say
   so. Rebuilt with `make CFLAGS="-O2 -g -DBENDER_NATIVE_EXECUTORS"`, the same
   suite at the same commit is **green** and those four ids bind.

2. **`188:int-string?` and `190:to-i` do not refuse in builds from this
   lineage.** Both are present in the manifests of every ttfx binary built
   here (`ttfx-cli-cont-v1`, `ttfx-pgo-v1`) and both **bind**: the gate
   reports `available=81 REFUSED: none bound 81` and `available=96 REFUSED:
   none bound 96`, and running the binaries directly emits no refusal line.
   The gate doc's own table is consistent with that: `ttfx-cli-packed-path-pgo`
   (plain `--pgo`) shows `REFUSED: none`, while the refusals appear on
   `ttfx-cli-autopgo-v2`, built through `--pgo-auto` then `--release`. So the
   failure is not a property of those two functions, and not of entry-file
   eager registration in general -- it is specific to the
   `--pgo-auto`/`--release` pipeline, where something re-emits or transforms
   the entry file's forms after `tools/callgraph.ben` produced the
   descriptor. A `--native` vs `--release` pair off one source tree should
   bisect it in one step.

**Neither failure interacts with continuation.** `bender_native_call_enter`
never assumes a callee is attached: it requires `def->native_code != NULL`
*and* a current-epoch `bender_native_revalidate`, and refuses -- touching
nothing -- otherwise. A definition that lost its executor to either
registration bug simply never chains; it cannot mis-chain. Fixing the eager
path is left as a separate commit, as the workstream recommends.

---

## v2: profile-narrowed emission (`:calledges`), 2026-10-23

Code landed in `45bd0c66` (`:calledges` in the profile), `f6a15bbf`
(validation/index/merge), `e03ba5ed` (narrowed emission), `53baa402`
(tests). This is §7 steps 8-11, built in response to the measurement above.
**Where this section and the design text disagree, the code and this section
are authoritative.**

### What was built

- **`:calledges`** (`src/core/vm.c`): one slot per original bytecode PC of
  the caller -- `nil` unobserved, `[calleeName total native]` monomorphic,
  `:open` polymorphic or unnamed. Recorded in `JOP_CALL`'s Bender-function
  branch from a caller identity captured *before* `func` is rebound, under
  the same `BENDER_PROFILE_EDGE_MAXPC` bound as `:edges`/`:receivers`, and
  lookup-only (it never creates a profile entry). Framed exactly like
  `:elems`: shaping evidence that decides *where* a guard is emitted, never
  what a guard may skip.
- **Validation and merge** (`tools/callgraph.ben`, `tools/profile-merge.ben`):
  malformed slots are a hard `profile-invalid`; `--pgo-auto`'s merge sums
  agreeing slots and opens on any disagreement. Without the merge support the
  field would have been silently dropped by `accumulate-info`.
- **Narrowed emission** (`tools/loop-executor.ben`, `tools/callgraph.ben`):
  `loop-executor/emit` takes an optional `cont-pcs` set; with none it emits
  **no** continuation attempt anywhere, which is byte-for-byte the pre-v1
  shape. Selection requires a monomorphic slot at or above
  `BENDER_EXECUTOR_EDGE_MIN_CALLS`, a callee name resolving to exactly one
  `name-ids` id, that id in the selected executor set, and agreement from the
  static `call-map`.

### One divergence worth recording

Descriptors are deduplicated *structurally* and attach to every matching
definition, so `cont-pcs` cannot be a function of the emitted id. Selection
and emission are therefore two passes, and ids sharing a structural key emit
once with the **union** of their PC sets. Union is the conservative
direction: a guard that refuses touches nothing.

### Measured outcome: the cost term is gone

ttfx, one source tree, `--pgo` on the same decrypt workload,
`BENDER_EXECUTOR_LEAF_MIN_CALLS=500`, candidate additionally
`BENDER_EXECUTOR_EDGE_MIN_CALLS=500`. Baseline is bender `66a5cf9a`, the last
commit *before* continuation v1. Both binaries bind **96/96, REFUSED: none**,
with byte-identical `native-executors.txt`.

| leg | v1 as built | v2 narrowed |
|---|---|---|
| cross-binary vs pre-change, identical executor sets | 1.0064 (7 rounds) / 1.0100 (5 rounds) | **0.9881** (7 rounds, 436 cases) / **0.9834** (5 rounds) |
| same binary, `BENDER_NATIVE_NO_CONTINUATION` toggled | 0.9996 | **0.9924** (5 rounds, 8 effects) |

**The +0.6-1.0% always-on cost measured for v1 is removed.** Both independent
cross-binary runs now land *below* 1.0 rather than above it, and the
same-binary toggle moved from a flat 0.9996 to 0.9924 with 6 of 8 effects
under 1.0 -- a small but directionally consistent chain benefit that v1 could
not show because its guard cost cancelled it.

Two honest bounds on those numbers. First, 0.9834-0.9881 is *more* than
narrowing alone should buy: the candidate still carries the unconditional
`bender_vm.native_interp_calls++` in `JOP_CALL` and the trampoline
indirection, so part of the margin below 1.0 is build-to-build code layout,
the same effect that made v1's first 1.0100 reading look like noise. The
defensible claim is **"at or below parity"**, not "1.2% faster". Second, the
toggle leg's 0.9924 sits inside the +/-3% per-effect floor; only its
consistency across effects distinguishes it from noise.

### Narrowing keeps essentially every edge that chains

On decrypt, 400 frames, leaf tier on:

| | v1 (attempt at every non-terminal call PC) | v2 (5 emitted PCs) |
|---|---:|---:|
| emitted `bender_native_call_enter` sites | every non-terminal call PC | **5** |
| chained edges | 90,041 | **89,392** |
| runtime refusals | 563,564 (loop tier) | **2** |
| counter identity | 653,839 == 653,839 | **653,836 == 653,836** |

So 99.3% of the chaining survives five guards, and the refusal traffic --
the guard firing where it was never going to succeed -- collapses from
hundreds of thousands to two. That is the mechanism by which the cost term
disappears, and it confirms the design's premise that chaining edges are
concentrated at a handful of PCs.

The five selected PCs on this profile are `ctx-anim`, `scene-new`,
`ctx-scene` and `ctx-add-frame` edges -- **not** the motion chain of §5.
decrypt is not a motion-dominated workload, so the §5 edge table remains
unverified by this measurement; selecting it would need a profile collected
on `binarypath`/`rings`/`waves`.

### Gates

| Gate | Result |
|---|---|
| `make test`, `NATIVE_EXECUTORS=1` build | green |
| `make test-executors` | green |
| `test/suite-callgraph.ben` | 242/242 (6 new `:calledges` schema gates) |
| ttfx 37-effect Bender-vs-Bender, 96/96 both sides | **37/37 exact, 368/368 cases, 0 FAIL** |
| ttfx counter identity, decrypt, 400 frames | **653,836 == 653,836**, dumps byte-identical on/off |
| Profile-free build emits zero `bender_native_call_enter` | asserted in `test/suite-native-executor.sh` |

R11 is now closed as the design intended: a profile-free build is
byte-identical in shape to pre-v1 output, which v1 could not claim.

### Limitations

- The five-PC selection profile above covers one decrypt workload; its
  99.3% retained-chain observation applies to decrypt, **not all effects**.
  In the fixed 400-frame comparison recorded in `work-counters.md`, v2 lost
  exactly 654 decrypt, 1,966 rings, zero beams, and **30,846 binarypath**
  chains versus v1, with each loss replaced by an interpreted call. The
  missing binarypath edges came from `travel-complete?`, not one of decrypt's
  five selected callers. Binarypath-trained v2 selected that call and
  `ctx-coord-on-line`→`geom-lerp-point`, recovering live chains, but also
  selected 107 rather than 96 executors: this experiment is evidence for
  profile representativeness, not an isolated continuation speedup.
- `BENDER_EXECUTOR_EDGE_MIN_CALLS` is unset (or nonnumeric) by default, so a
  default build selects nothing and emits no continuation attempt at all.
  Pick a numeric threshold against profiles representative of the effects
  being shipped, then inspect `native-continuations.txt`, executor-set parity,
  chained calls and interpreter work *per effect*. At 500, a decrypt-only
  profile discarded 30,846 otherwise chainable binarypath calls. Lowering
  the threshold cannot recover an edge **absent from training evidence**;
  representative or conservatively merged multi-workload evidence is needed
  first, and merge may open conflicting edges. Reducing the threshold for
  observed edges can emit more guards (and spend more work on refused
  attempts), so recheck costs rather than choosing 500 universally. A pinned
  v2 `--pgo-auto` experiment merged 400-frame decrypt, binarypath and rings
  with an isolated output-keyed cache at threshold 500 (details and exact
  four-effect work vectors in `work-counters.md`). All three workloads passed
  separate replay/parity gates; 13 PCs were emitted, including the missing
  `travel-complete?` and motion `ctx-coord-on-line` edges. On binarypath,
  chains rose from decrypt-only v2's 45,385 to 216,823 (unnarrowed v1:
  76,231); rings rose 17,378→136,743 (v1: 19,344), and beams rose
  127,249→128,975 (v1: 127,249). Decrypt remained 259 short of v1
  (89,775 versus 90,034). Multi-profile selection did **not** keep near-zero
  refusals on every effect: 397 decrypt, 3 rings, 4,003 beams, 792 binarypath.
  It also selected 111 executors versus 96 in decrypt-trained v1/v2, so
  changed VM exits and native ops cannot isolate continuation benefit.
  A controlled `BENDER_NATIVE_NO_CONTINUATION` ON/OFF pair on binarypath *within*
  each pinned v2 image closes that attribution gap for each image separately:
  decrypt-trained (96 executors) saves 45,385 interpreted calls, 133,061
  dispatches and 65,764 VM exits with chaining ON; binarypath-trained (107)
  saves 216,823 calls, 452,699 dispatches and 367,232 exits. Each image has
  byte-identical ON/OFF output, exact conserved call edges, and invariant
  native operations and executor entries. The **difference between** those
  within-image savings is still conditional on different executor sets; it
  cannot assign the cross-image −5,282,115 dispatches entirely to selected
  edges. Same source and leaf threshold selected 96 vs 107 executors, so
  `BENDER_EXECUTOR_LEAF_MIN_CALLS` is workload-sensitive too. The binarypath
  profile selected `ctx-coord-on-line` PC 3→`geom-lerp-point`, **exactly the
  motion-chain edge predicted by §5**; decrypt training missed it. Full work
  vectors and limitations are in `work-counters.md`. The demonstrable choice
  is representative merged training for *coverage*, followed by per-effect
  guards/work and byte-parity checks; threshold 500 is not proven a universal
  shipping setting, and lower thresholds were not tested because all named
  missing edges were already emitted.
- Timing: one machine, no CPU pinning, medians of 5-7 paired alternating
  rounds. Per-effect ratios must not be read individually against the +/-3%
  floor.
- `bender_vm.native_interp_calls++` in `JOP_CALL` remains unconditional and
  was still not separately A/B'd.

---

## Correction: the `--pgo-auto`/`--release` descriptor refusal is not a descriptor bug

The reconciliation section above predicted that a `--native` vs `--release`
pair off one source tree would bisect the `188:int-string?` / `190:to-i`
refusals "in one step". It did, and the answer is that **no descriptor
transform exists**. Fixed in `591049a3`.

Bisected on one ttfx source tree with bender at `6cdb7857`, the exact commit
the gate doc blames:

| bender build at `6cdb7857` | ttfx build | refusals |
|---|---|---|
| plain `make` (no `-DBENDER_NATIVE_EXECUTORS`) | `--native --no-auto-pgo` | **2** (`id=188`, `id=190`) |
| plain `make` | `--pgo` then `--release` | **2** (same pair) |
| `make CFLAGS="-O2 -g -DBENDER_NATIVE_EXECUTORS"` | `--native --no-auto-pgo` | **0**, binds 81/81 |

The pipeline is irrelevant. Without the compile-time opt-in,
`bender_native_register` is a stub returning 0 and `bender_native_registry_add`
is a no-op, so **every** registration fails. `188:int-string?` and
`190:to-i` are the only manifest entries whose `defn` lives in the AOT entry
file, hence the only ones `tools/mkimage.ben` registers *eagerly* -- and
the eager path is the only one that prints anything. Two reports out of 81
silent failures read as two broken functions rather than a dead tier.

That is exactly the diagnosis the v1 notes reached for
`test/suite-native-executor.sh`; it generalizes to the ttfx binaries, and the
`--pgo-auto`/`--release` correlation in `ML/ttfx/bench/bender-37-gate.md` was
coincidental -- those two binaries were built against differently-configured
bender builds, not by different pipelines.

Verified at this HEAD with the opt-in: `--native`, `--pgo`, `--pgo-auto` and
`--release` ttfx builds each report **available=81, REFUSED: none**.

**The fix** makes the misleading line honest. `native_init` printed
"available N frame executors" straight from the build manifest even when the
runtime could not bind a single one. It now asks
`bender_native_registry_count()`, which is zero exactly when the tier is
absent, and otherwise says so explicitly:

```
native_init: 81 frame executors in this image, but this runtime was built
WITHOUT BENDER_NATIVE_EXECUTORS -- the executor tier is inert and every
registration will fail
```

The underlying ergonomic hazard -- that the tier needs both a compile-time
define and a runtime environment variable, and silently degrades to the
interpreter when only one is supplied -- is unchanged; `d5a24e5d`'s
`make NATIVE_EXECUTORS=1` and this message are the two mitigations now in
place.
