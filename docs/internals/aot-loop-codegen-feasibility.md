# AOT loop codegen: feasibility after native continuations

Read-only design scout. Evidence gathered in a throwaway worktree at
`d5a24e5d`, built `make NATIVE_EXECUTORS=1` with `JANET_NATIVE_EXECUTORS=1`.
Symbol names are primary; line numbers are approximate and were taken while
`src/core/vm.c`, `tools/loop-executor.janet`, `tools/callgraph.janet` and
`src/boot/boot.janet` were being concurrently edited.

## Verdict (three parts, in order of importance)

1. **The Tier A veto is still justified, and native continuations do not
   discharge its premise.** `janet_native_call_enter` / `janet_native_ret_pop` /
   `janet_native_trampoline` are a *call-edge* mechanism parameterised by
   `(JanetFiber *fiber, …, uint32_t *callpc, uint32_t **out_npc)`. A Tier A
   `emit-fn-c` kernel has no fiber, no PC and no frame slots — its emitted
   signature is literally `int64_t sum_to(int64_t p0)` with loop state in raw
   `int64_t` locals. There is no PC to suspend at and no GC-scanned location to
   materialise into. Continuations make *chains of frames* resumable; they do
   nothing for a loop that must suspend mid-iteration with raw C locals live.

2. **But the premise of the question — "the loop codegen exists and is simply
   unreachable from the deployable path" — is wrong.** The AOT path already
   emits safe looping native code. `--emit-native-lib`, which is the only thing
   `--aot` wires (`src/boot/boot.janet` ≈6456), emits **two** tiers into the same
   `native.c`: the Tier A kernels *and* the Tier B region executors from
   `tools/loop-executor.janet`. The Tier B executors already contain real C
   loops with in-loop `janet_native_poll`, wholesale deopt, and slot
   materialisation. Loop codegen is *not* unreachable from `--aot`; **unboxed
   raw-C-local** loop codegen is, and that one is unreachable for a correct
   reason.

3. **The census number is inflated by veto ordering.** 115 of the 169
   loop-vetoed refusals are zero-param, and zero-param functions are *already*
   refused by Tier A's arity gate. Lifting the loop veto admits none of them.
   The honest ceiling is ~54 functions, ~6.9% of 783 — comfortably inside the
   ±3% per-effect noise floor. This is not a step change.

The productive work is therefore **not** "lift the Tier A veto". It is "generalise
Tier B loop fusion", where every safety obligation is already discharged and
validated. The unboxed-Tier-A-loop question belongs in the typed/unboxed Phase-5
roadmap.

---

## 1. Current wiring: what each flag emits and links

| Flag | Entry symbol | Emits | Loop veto? | Reached by `--aot`? |
|---|---|---|---|---|
| `--emit-native DIR` | `emit-native-all` (`tools/callgraph.janet` ≈3860) | Tier A kernels + a `main()` oracle harness printing sampled results | **No** | **No** |
| `--emit-native-lib DIR` | `emit-native-lib` (`tools/callgraph.janet` ≈5202) | Tier A kernels + `*_wrapper` cfunctions + `native_init(JanetTable*)` **and** Tier B executors + `cfun_executor_register` | **Yes** | **Yes** |
| `--aot` | `src/boot/boot.janet` ≈6456 | shells out to `callgraph.janet … "--emit-native-lib" ndir`, then `mkimage.janet … --native <ndir>/native-names.txt` | inherits lib's | — |

Flag parsing: `emit-native-dir` / `emit-native-lib-dir` (`tools/callgraph.janet`
≈46-47, dispatched ≈70-73), invoked at ≈4636 and ≈5694 respectively. `--aot`
never passes `--emit-native`, so the oracle-harness flavour is a developer
diagnostic only.

### The real loop C that `--emit-native` produces

Input (`/tmp/loopexp/sumloop.janet`):

```janet
(defn sum-to [n]
  (var acc 0) (var i 0)
  (while (< i n) (set acc (+ acc i)) (set i (+ i 1)))
  acc)
```

`./build/bender tools/callgraph.janet … --emit-native /tmp/loopexp/nat` reported
`native 2 fns 10 samples` and emitted, verbatim:

```c
int64_t sum_to(int64_t p0) {
    int64_t s1;
    int64_t s2;
    int s3;
    s1 = 0;
    s2 = 0;
L2:;
    s3 = s2 < p0;
    if (!s3) goto L7;
L4:;
    s1 = s1 + s2;
    s2 = s2 + 1;
    goto L2;
L7:;
    return s1;
}
```

This is the whole argument in fifteen lines. The backward `goto L2` is a
genuine loop; the loop-carried state is `s1`/`s2`, raw `int64_t` automatics;
there is **no** `JanetFiber *`, **no** `uint32_t **pc`, **no**
`janet_native_poll`, and no path by which this function can be re-entered
part-way. `emit-fn-c` emits `goto L<pc>` with no sign test on the delta
(inside `emit-fn-c`: `(= op 'jmp) … "goto L" (+ pc (ins 1))`), and
`flow-check` (≈2108) computes block leaders from jump targets without rejecting
backward edges. The comment in `emit-fn-c`'s own declaration sweep — "collect
written slots (loop bodies reassign)" — shows loops were always an expected
input to this emitter.

The identical function on the deployable path:

```
$ ./build/bender tools/callgraph.janet /tmp/loopexp/sumloop.janet --emit-native-lib …
native-skip 1 sum-to loop requires resumable VM quantum polls
```

### What Tier B emits for the same function

`--emit-native-lib` on the same input produced executor `nx_1`, which is where
the interesting machinery lives:

```c
static JanetNativeStep nx_1(JanetFiber *f, JanetFuncDef *d, uint32_t **pc) {
Janet *s = f->data + f->frame;
…
switch (*pc - d->bytecode) {
case 0: goto L0;  case 1: goto L1;  case 2: goto L2;  …
default: return JANET_NATIVE_VM; }
L2:;
if (!janet_checktype(s[0], JANET_NUMBER) || !janet_checktype(s[1], JANET_NUMBER)
 || !janet_checktype(s[2], JANET_NUMBER)) { NX_FALLBACK(); NX_EXIT(2,JANET_NATIVE_VM); }
double nxn_ = janet_unwrap_number(s[0]), nxsum_ = janet_unwrap_number(s[1]),
       nxi_ = janet_unwrap_number(s[2]);
if (!(nxn_ >= 0.0 && nxn_ <= 1000000.0 && nxn_ == floor(nxn_)
   && nxi_ >= 0.0 && nxi_ <= nxn_ && nxi_ == floor(nxi_)
   && nxsum_ >= 0.0 && nxsum_ <= 500000000000.0 && nxsum_ == floor(nxsum_)))
  { NX_FALLBACK(); NX_EXIT(2,JANET_NATIVE_VM); }
while (nxi_ < nxn_) {
  nxsum_ += nxi_; nxi_ += 1.0;
  NX_TICK(); NX_TICK(); NX_TICK(); NX_EDGE();
  if (janet_native_poll(1)) {
    s[1] = janet_wrap_number(nxsum_); s[2] = janet_wrap_number(nxi_);
    s[3] = janet_wrap_true(); NX_EXIT(6,JANET_NATIVE_INTERRUPT);
  }
}
s[1] = janet_wrap_number(nxsum_); s[2] = janet_wrap_number(nxi_);
s[3] = janet_wrap_false(); NX_EXIT(7,JANET_NATIVE_VM);
```

That is an **unboxed** C loop, in the AOT-deployed artifact, today. It is safe
because every obligation in §3 is met by construction, and it is narrow because
the recognizer is a single hardcoded shape: `numeric-sum-pattern?`
(`tools/loop-executor.janet` ≈162, gating the whole block at ≈210).

The *general* (unfused) Tier B path handles every other loop at boxed-slot
speed, polling on each backward edge (`tools/loop-executor.janet` ≈549-561):

```janet
(when (<= delta 0)
  (out "NX_EDGE(); if (janet_native_poll(1)) NX_EXIT(" (string k) ",JANET_NATIVE_INTERRUPT);"))
(out "goto L" (string (+ k delta)) ";")
```

Note the predicate `(<= delta 0)` — the same backward-edge test as the Tier A
veto, used here to *insert a poll* rather than to refuse.

## 2. The veto: exact predicate and overridability

Sole site, in `emit-native-lib` (`tools/callgraph.janet` ≈5253-5262 as cited;
observed at ≈5257-5262 in the worktree):

```janet
(def loops (some (fn [ins]
                  (def op (ins 0))
                  (or (and (= op 'jmp) (<= (ins 1) 0))
                      (and (get emit-cond-jumps op) (<= (ins 2) 0))))
                (or (d :bytecode) @[])))
```

It is a purely syntactic scan of the function's own bytecode for any jump whose
signed delta is `<= 0`: operand 1 for unconditional `jmp`, operand 2 for any op
in `emit-cond-jumps`. `<= 0` rather than `< 0` deliberately includes
**self-edges** (delta 0), which the comment flags as able to "spin without
advancing pc".

Dispatch (≈5265-5275):

```janet
(cond
  (get info :name-duplicate) → skip "duplicate registration…"
  loops                      → skip "loop requires resumable VM quantum polls"
  concrete                   → (put cand id true)
  …)
```

**Nothing can override it.** There is no flag, no hint, no PGO path and no
environment variable that reaches it; it is not gated on `dce`, on
`profile-index`, or on `NATIVE_EXECUTORS`. The Phase-4 dispatch middle tier in
the `else` arm is unreachable for a looping function because `loops` short-
circuits first. The only way past it is a source edit.

**Ordering is load-bearing and under-appreciated.** `loops` is tested *before*
`concrete`, and `concrete` is initialised `(and (> (length pt) 0) (not (d :vararg)))`.
A zero-param looping function is therefore reported as loop-vetoed even
though the arity gate would refuse it anyway. Verified:

```
$ ./build/bender tools/callgraph.janet /tmp/loopexp/zp.janet --emit-native-lib …
native-skip 1 zero-param-loop loop requires resumable VM quantum polls
```

`zero-param-loop` has `params []`; lifting the loop veto reclassifies it to
"types not proven" and emits nothing. This is the basis of the deflation in §6.

## 3. Safety obligations for a looping native kernel

| Obligation | What it requires |
|---|---|
| **Interrupt/signal polling** | Every backward edge must execute the VM's own poll. `janet_native_poll` (`src/core/vm.c` ≈391) is exactly `vm_poll_condition(COND)` — `(COND) && (auto_suspend \|\| (quantum && --quantum_remaining <= 0))`. It *decrements* `quantum_remaining`, so a loop that skips it both fails to observe interrupts and fails to consume quantum, starving the scheduler. |
| **GC safepoints and barriers** | Loop-carried values that are heap pointers must live in GC-scanned locations (fiber slots `f->data + f->frame`) across any allocation or collection point, or be provably non-pointer. `maybe_collect()` is the VM's nursery check; `janet_native_call_enter` performs JOP_CALL's `vm_checkgc_next()` and the trampoline performs JOP_RETURN's `vm_checkgc_pcnext()`. |
| **Quantum/yield-resume contract** | For a fiber to yield from emitted loop code: (a) all live state is materialised into fiber slots; (b) `*pc` is left at an **unexecuted** bytecode instruction whose re-execution is idempotent; (c) the step value is `JANET_NATIVE_INTERRUPT`, so `vm_native()` sets `JANET_FIBER_RESUME_NO_USEVAL \| JANET_FIBER_RESUME_NO_SKIP` and does `vm_return(JANET_SIGNAL_INTERRUPT, …)`; (d) the code is re-enterable at that PC. Tier B satisfies (d) with the `switch (*pc - d->bytecode)` prologue plus the descriptor region mask consulted by `janet_native_region_allowed`. |
| **Deopt at a backward edge** | On any guard failure or quantum event, exit at a PC the interpreter can run. Tier B's `NX_EXIT(K, …)` sets `*pc = d->bytecode + K` and the fused path exits at PC 6 (the unexecuted backedge) or PC 2 (the unexecuted loop head). |
| **Liveness across a quantum boundary** | The fused loop's correctness argument: `nxn_`/`nxsum_`/`nxi_` are `double` and the entry guard proves them finite integral in range, so "the two local doubles never hold a heap pointer" (comment at ≈211-214). They may therefore be lost to a collection; only slots `s[1]`/`s[2]`/`s[3]` must be rewritten before the exit — including `s[3]`, the *comparison result* slot, so that re-entry at PC 6 sees the branch condition the VM would have computed. |
| **Stack depth / recursion** | A loop alone cannot deepen the C stack. A *calling* loop can: `janet_native_call_enter` pre-checks `fiber->stack_overflowed` and `fiber->stacktop > fiber->maxstack` and refuses rather than raising (the VM owns that error), `janet_native_trampoline` caps chaining at `JANET_NATIVE_CHAIN_BUDGET 64`, and the trampoline "holds NO state across an executor call that a longjmp would have to unwind". `tools/loop-executor.janet` ≈389 also emits an explicit in-executor `if (f->stack_overflowed) { NX_EXIT(k, JANET_NATIVE_VM); }`. |

Additional constraint already honoured by Tier B and **not** by Tier A for
loops: floating-point determinism. `native.c` opens with
`#ifdef __FAST_MATH__ #error …`, `#pragma STDC FP_CONTRACT OFF` and a GCC
`fp-contract=off` push, because byte-exact parity with the boxed double path is
a gate (§7).

## 4. What continuations already satisfy — and what they do not

**Satisfied, for the executor tier:**

- Call edges out of native code without a C recursion class.
  `janet_native_trampoline` runs executors "back to back across pushed and
  popped FIBER frames without ever recursing in C, so a chain of native calls
  still occupies exactly one C frame: no new stack-overflow class, no
  `janet_vm.stackn` pressure, **no pinned C frame across a yield**."
- All-or-nothing commit. `janet_native_call_enter` refuses before mutating any
  fiber state (callee identity, trace/profile hooks, `VARARG`/`NEEDSENV`/
  `STRUCTARG`/`NAMEDARGS`, `environments_length`, arity, descriptor present and
  epoch-verified, depth) and only then polls once and pushes.
- Quantum accounting on the call edge: `if (janet_native_poll(1)) return 2;`
  replaces JOP_CALL's own `vm_maybe_auto_suspend(1)`, and return `2` is
  propagated as `JANET_NATIVE_INTERRUPT` by the emitted
  `if (nxcont_ == 2) { NX_EXIT(k, JANET_NATIVE_INTERRUPT); }`.
- Breakpoint and profiler integrity: both `janet_native_call_enter` and
  `janet_native_ret_pop` refuse under `JANET_FIBER_BREAKPOINT` or a live
  `janet_vm.profile_table`, which is also what keeps auto-PGO evidence exact.
- Correct frame reload after chaining: `vm_native()` reloads `func` as well as
  `stack` and `pc`, because "the trampoline can leave us in a DIFFERENT frame".

**Not satisfied — the finding that matters:**

The trampoline keeping frames on the fiber **does not help a Tier A loop that
must suspend mid-iteration with raw C locals live.** Three independent reasons:

1. **No suspension point exists in the ABI.** The emitted kernel is
   `int64_t sum_to(int64_t p0)`. Suspension requires a `JanetFiber *` to
   materialise into and a `uint32_t **pc` to park. Neither is a parameter.
   Adding them changes the kernel ABI, the `*_wrapper` contract and the
   `native_init` registration shape — i.e. it is not a veto lift, it is a
   redesign into Tier B.
2. **No re-entry point exists.** Tier B resumes via
   `switch (*pc - d->bytecode) { case k: goto Lk; }` over a *bytecode* PC.
   A Tier A kernel's `L2:`/`L4:`/`L7:` labels are offsets into a C function
   with no persisted mapping and no entry dispatcher; the hybrid wrapper
   "replaces the bytecode definition entirely" (the veto's own preceding
   comment), so there is no bytecode left to re-enter at.
3. **Continuations are about the wrong boundary.** They make the *call* edge
   resumable by moving the callee frame to the fiber. A loop's problem is the
   *backward* edge inside one frame, where the live values are `s1`/`s2`
   automatics in a register allocator's assignment. Nothing in
   `janet_native_call_enter`'s contract touches that.

A fourth, weaker point in Tier A's favour, which should be stated because it is
the only honest route to a Tier A v1: a Tier A kernel is reached through
`clamp_wrapper`-style cfunctions, and a Janet cfunction is **atomic** — it
cannot yield mid-call under any circumstances today. So a Tier A loop does not
actually need to *resume*; it needs not to **monopolise the quantum or delay
interrupt observation**. That reframing is what makes bounded-trip-count the
only tractable Tier A subset (§5c). It does not make the veto wrong; it makes
the veto's stated *reason* ("resumable VM quantum polls") stricter than
strictly necessary for one narrow case.

## 5. A minimal safe v1 subset

| Candidate | Buys | Forbids | Compiler difficulty | Assessment |
|---|---|---|---|---|
| **(a) Loops with no calls and no allocation** | Removes the GC-liveness obligation entirely; raw locals provably hold no heap pointers. | Any `call`/`tcall`, any array/table/buffer/string construction, any `ldc` of a heap constant. | Low in Tier A (reuse the existing bytecode scan, add an opcode allowlist). | **Insufficient alone.** Still monopolises the quantum: an unbounded `while` with no calls is exactly the starvation case, and the wrapper cannot be interrupted. Must be combined with (c). |
| **(b) Wholesale deopt to the VM on any quantum event** | The textbook-correct answer; re-enter bytecode at the loop head with state materialised. | Nothing material. | **High in Tier A** (needs a fiber pointer, a slot map, a PC dispatcher — i.e. becomes Tier B). **Already implemented in Tier B.** | **Already done, in the wrong tier for the premise.** This is the Tier B fused path verbatim. Do not rebuild it in Tier A; generalise it in Tier B. |
| **(c) Counted loops with a statically bounded trip count that cannot exceed a quantum** | The only subset that is sound in Tier A as it stands. If max trips × ops-per-trip < the smallest quantum, the loop cannot delay a poll beyond what one bytecode instruction already may. | Any loop whose bound is not a compile-time constant, i.e. every `(while (< i n))` over a runtime `n` — including the `sum-to` example above. Realistically only literal-bounded `(for i 0 K)` with small `K`. | Medium: needs a trip-count analysis plus an ops-per-trip cost model plus a conservative tie to `janet_vm.quantum`, which is a *runtime* value the compiler cannot see. Pinning it to the minimum legal quantum is the only safe choice, which shrinks `K` hard. | **Sound but tiny.** A literal-bounded short loop is also exactly what the C compiler will unroll in a boxed wrapper anyway. Low value. |
| **(d) Loops over already-unboxed `table-flat`/scalar state** | Removes the reboxing cost at the loop boundary, which is where the Tier B fused path still pays (`janet_wrap_number` per exit). | Needs the typed/unboxed representation to exist and be proven whole-program. | High; depends on Phase 5. | **The right long-term answer, and it is a Phase-5 item, not a veto lift.** |

**Recommended v1: generalise Tier B fusion, leave the Tier A veto in place.**

Concretely: replace `numeric-sum-pattern?` (`tools/loop-executor.janet` ≈162)
with a parameterised recognizer for the shape it already proves — a counted
loop with a monotone integral induction variable, a loop-invariant numeric
bound, and a finite set of numeric accumulators updated by `add`/`sub`/`mul`
with a provable range guard — and reuse the existing entry guard,
materialisation and `NX_EXIT` machinery unchanged. Every obligation in §3 is
then discharged by code that already ships and already passes the parity gate.
The subset is (a)+(b) in combination, expressed in the tier that can actually
express (b).

## 6. Expected coverage gain — quantified and deflated

Census: **783** kernel refusals, **169** loop-vetoed, of which **115** are
zero-param.

The deflation, from §2 and the `zero-param-loop` experiment: `loops` is tested
*before* `concrete`, and `concrete` is false for every zero-param function.
Lifting the loop veto reclassifies all 115 to "types not proven" and admits
**none** of them.

```
loop-vetoed                       169   (21.6% of 783)
  − zero-param (double-vetoed)   −115
  = potentially admissible         54   ( 6.9% of 783)
```

The 54 must then clear every *other* Tier A gate they were never tested
against, because `loops` short-circuited: all params in
`infer/numeric-types ∪ abi-param-types`, no `:u64` anywhere, concrete return,
not `name-duplicate`, not an extern, not DCE-dead, and then `emit-fn-c` and
`flow-check` must actually succeed, and then two-round dependency resolution
must find every callee emitted. The measured pass rate for those gates on the
non-loop population is the right prior, and it is well under 1. Taking even a
generous 50% yields **~27 functions admitted, ~3.4% of refusals**.

Against a per-effect noise floor of ±3% and a geomean floor of ~0.1–0.3%:

- 27 newly-emitted kernels is a **sub-floor result** unless those 27 happen to
  sit on the hot path with high call counts. Refusal *count* is the wrong
  metric; the only number that would justify this work is the measured share of
  ttfx runtime spent inside those specific 54 functions, which should be read
  off the PGO profile before any code is written. If that share is below ~5%,
  the expected effect cannot clear ±3% even with a perfect kernel.
- Worse, the comparison is not against boxed bytecode. These functions are
  *already* executed by the Tier B general executor with per-backward-edge
  polling, and if they match the fused shape they are already unboxed in the
  inner loop. The marginal win of a Tier A kernel over an already-fused Tier B
  executor is the call-boundary boxing only — a constant per *call*, not per
  iteration, and therefore invisible for exactly the loop-heavy functions this
  change targets.

**Conclusion: another sub-floor result.** Generalising Tier B fusion (§5) has a
better profile because it improves the per-*iteration* cost of loops that are
already running, rather than the per-*call* cost of 27 functions.

## 7. Ordered implementation plan (for the recommended Tier B v1)

| # | Step | Risk |
|---|---|---|
| 1 | **Measure first.** From the committed PGO evidence, compute the ttfx runtime share of the 54 non-zero-param loop-vetoed functions and of all loops currently running through the *unfused* Tier B path. Publish both numbers. | None. **Gate: if neither share exceeds ~5%, stop here** and record the negative result. |
| 2 | Add a `native-skip` reason-ordering fix so `concrete` is tested before `loops`, making the census self-describing. Diagnostics only; emits nothing new. | Very low. Changes skip *strings*, which some tooling may parse. |
| 3 | Extract `numeric-sum-pattern?`'s proof obligations into an explicit record (induction variable, bound expression, accumulator set, range guard, materialisation slot list, deopt PCs) while keeping the single existing shape as the only match. Byte-identical output required. | Low. Refactor under an exact-output check. |
| 4 | Generalise the recognizer to N accumulators, still `add`/`sub` only, still requiring a loop-invariant integral bound and a conservative static range guard. Emit the guard from the record. | **Medium-high.** Range-guard derivation is where a miscompile lives (§8). Every bound must be provable from the guard, not assumed. |
| 5 | Admit `mul` and integral division with its existing guard shape. | High. Overflow and the int-div semantics are separate proofs; do not bundle. |
| 6 | *Deferred, low priority.* Tier A counted loops with literal bounds (§5c), only if step 1 showed call-boundary cost dominating. | Medium, low reward. |
| 7 | *Deferred to Phase 5.* Unboxed `table-flat`/scalar loop state (§5d). | — |

### Validation gates (every step, non-negotiable)

- `make test` **and** `make NATIVE_EXECUTORS=1 test` — both build modes. A plain
  `make` leaves the executor tier inert (define absent, registrations fail,
  stats read zeros), so a green plain-mode run proves nothing about this work
  and a green executor-mode run is the only meaningful signal.
- `make test-executors` → `test/suite-native-executor.sh` (`Makefile` ≈309-312).
- **ttfx 37/37 exact byte parity** via the committed gate at
  `ML/ttfx/bench/validate-bender.py` (referenced from
  `docs/internals/ttfx-pgo-runtime-feedback.md` ≈410, 420; the gate lives in the
  ML/ttfx tree, not in this repo — `bench/validate-bender.py` does not exist
  here). Byte parity, not approximate agreement: the `__FAST_MATH__` error and
  `FP_CONTRACT OFF` pragmas in the generated header exist precisely because
  this gate is exact.
- Executor telemetry must be read, not assumed: `nx_*_fallbacks` and
  `nx_*_vmexits` rising after a change means the new guard is refusing at
  runtime and the kernel is dead weight. A fused loop whose entry guard always
  fails is *worse* than no fusion — it pays the guard and then deopts.
- Counter identity: `janet_native_stat(JANET_NATIVE_STAT_CONTINUATIONS)` vs
  `…_REFUSALS` vs `…_INTERP_CALLS` must stay consistent, as the existing A/B
  reading depends on it (`docs/internals/native-continuation-design.md`,
  Implementation Notes).

## 8. Risk register

| # | Failure mode | Mechanism | Detection |
|---|---|---|---|
| R1 | **Incomplete materialisation on interrupt** | A generalised fusion forgets one live slot on the `JANET_NATIVE_INTERRUPT` path. The fused example writes `s[1]`, `s[2]` *and* `s[3]` — and `s[3]` is the easy one to miss, because it is the comparison result, not program state. Resume reads a stale branch condition. | Nondeterministic, load-dependent wrong answers that vanish without interrupts. Catch by deriving the materialisation set mechanically from the slot-liveness analysis (step 3) and by running the suite under a forced-tiny `quantum` so every loop interrupts. **Add that forced-interrupt mode as a gate.** |
| R2 | **Deopt PC is an already-executed instruction** | `NX_EXIT(K, …)` parks at `d->bytecode + K`. If `K` names an instruction whose effect was already applied in C, resume double-applies it. | Exact-parity failure, but only under interrupts — so R1's forced-interrupt mode is the detector. Invariant to assert: every `NX_EXIT` target is unexecuted on that path. |
| R3 | **Unsound range guard → silent numeric divergence** | The guard proves the bounds that make the unboxed `double` arithmetic exact (`nxsum_ <= 500000000000.0` keeps every add an exact integer). A generalised guard that is too loose lets a `double` accumulator lose precision where the boxed path would too — or would *not*. | ttfx 37/37 byte parity, plus the `--emit-native` oracle harness (`samples-for`), which prints `%.17g` for sampled inputs and is the cheapest differential test available. Extend sampling to boundary values of each derived bound. |
| R4 | **Quantum starvation / missed interrupt** | A fused loop that polls on fewer paths than the bytecode did, or a Tier A loop with no poll at all, stops decrementing `quantum_remaining`. Scheduler starves; `ev` deadlines slip; Ctrl-C stops working inside the loop. | Not caught by `make test`. Needs a positive test: a long loop plus a timer, asserting the timer fires. Add one. The `NX_EDGE()` counter is the telemetry proxy — zero edges on a loop that ran is the signature. |
| R5 | **GC collects a value the loop still holds** | A generalised recognizer admits an accumulator that can be a heap pointer (a buffer, an array) and keeps it only in a C local across an allocation. | Use-after-free; ASan/valgrind under the suite. Structural defence: the recognizer must *refuse* any accumulator whose proven type is not in `infer/numeric-types`, and that refusal must be a hard assert in the emitter, not a filter in the matcher. |
| R6 | **Fiber corrupted by a partial commit** | Does not apply to the loop itself, but a loop containing a call inherits it. `janet_native_call_enter` is all-or-nothing by design; a generalisation that commits argument slots before calling it would break that. | `make test-executors`; continuation counter identity (a refusal that nonetheless mutated state shows as a frame/PC mismatch). Keep the "refuse before the first byte changes" discipline explicit in review. |
| R7 | **Guard always fails: pure regression** | Fusion emitted, entry guard never passes at runtime, every entry pays the guard then deopts. | `nx_*_fallbacks` telemetry. Must be read per step; an unread counter is how this one ships. |
| R8 | **Epoch/revalidation skipped on re-entry** | A loop that re-enters without `janet_native_region_allowed` + `janet_native_revalidate` can run a stale descriptor against an edited definition (breakpoint write, republication). | The trampoline re-gates per frame; the `switch` prologue plus the region mask is the per-PC gate. Any new entry point must be added to the mask. Debugger tests in the suite; breakpoint-inside-loop is a case worth adding. |
| R9 | **FP contract reintroduced by the host build** | A host app compiling `native.c` with `-ffast-math` or FMA contraction silently breaks byte parity. | Already defended: `#error` on `__FAST_MATH__`, `FP_CONTRACT OFF`, GCC `fp-contract=off`. Keep these as the first lines emitted; they are load-bearing, not decoration. |

---

## Adversarial self-check

The conclusion most flattering to this investigation would be "continuations
discharged the veto; lift it and win 169 kernels". That is wrong on three
counts, each independently fatal: continuations address the call edge and not
the backward edge; Tier A kernels have no fiber, no PC and no re-entry point to
suspend into; and 115 of the 169 are double-vetoed so the headline number is
~54 before any other gate applies, which lands under the ±3% noise floor.

The conclusion second-most flattering would be "so loops are impossible in AOT
native code". That is also wrong, and in the more interesting direction: the
AOT artifact already contains an unboxed, polling, deopting C loop today, built
by `tools/loop-executor.janet` and reached through `--emit-native-lib`. The
limitation is that its recognizer matches exactly one hardcoded shape.

So: **the veto stands and should stay.** The reachable win is widening Tier B's
fusion recognizer, where all the safety obligations are already discharged by
shipping code. Unboxed raw-C-local loops belong in the typed/unboxed Phase-5
roadmap, and the entry requirement there is not "native continuations" but a
suspension ABI for kernels — a fiber pointer, a slot map and a PC dispatcher,
which is to say, becoming Tier B.

And before any of it: **step 1 of §7 is a stop gate.** Measure the runtime share
first. The most likely honest outcome of this whole line of work is a published
negative result.

### Experiment reproduction

```sh
git -C <repo> worktree add /tmp/janet-loopscout d5a24e5d
cd /tmp/janet-loopscout && make NATIVE_EXECUTORS=1 -j8     # binary is build/bender
export JANET_NATIVE_EXECUTORS=1
./build/bender tools/callgraph.janet /tmp/loopexp/sumloop.janet --emit-native     /tmp/loopexp/nat
./build/bender tools/callgraph.janet /tmp/loopexp/sumloop.janet --emit-native-lib /tmp/loopexp/lib
git -C <repo> worktree remove /tmp/janet-loopscout
```

`nat/native.c` holds the raw-local Tier A loop; `lib/native.c` holds the
`native-skip … loop requires resumable VM quantum polls` line and the `nx_1`
executor with the fused, polling, materialising loop.
