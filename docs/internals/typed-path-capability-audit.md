# Typed-path capability audit

Date: 2026-10-23
Status: audit only — **no tracked file in either repo was modified or committed.**
Scope: `tools/callgraph.janet`, `tools/infer.janet`, `tools/loop-executor.janet`,
`src/boot/boot.janet`, `src/core/{vm,gc,ffi}.c`, `src/include/janet.h`, and the
ttfx port at `/Users/pankajdoharey/Development/Projects/ML/ttfx/janet-port/`.

Every claim below is backed by `file:line` **and** a reproducible experiment.
Experiments live in `/tmp/q1x`, `/tmp/pilot`, `/tmp/ttfxb`, `/tmp/q3*`.
Toolchain used: `/Users/pankajdoharey/Development/Projects/janet/build/bender`.

## Verdicts at a glance

| # | Capability | Verdict |
|---|---|---|
| Q1 | Type hints on functions | **EXISTS** — closed vocabulary, boundary-**checked**, fail-closed |
| Q2 | Unboxed flat aggregates (single struct) | **EXISTS** — two independent routes, both emitting direct loads |
| Q2b | Flat struct **arrays** (stride) | **ABSENT** — no `ptr-add` intrinsic |
| Q2c | `unsafe/*` pointer ops | **PARTIAL** — 14 cfuns exist; lowered **only** via `:kernel` metadata (and there is no `unsafe/peek`) |
| Q3a | Arena / region allocation | **ABSENT** |
| Q3b | Whole-module TU + direct cross-calls | **EXISTS** (kernel tier), unused by ttfx |
| Q4 | "VM near LuaJIT-class speed" | **NOT SUPPORTED** by committed evidence |

---

## Q1 — Type hints

### (a) Syntax and parsing

A hint is a Clojure-style metadata dict on `defn`, documented at
`tools/callgraph.janet:198-204`:

```janet
(defn f {:hint {:params [:double :long] :returns :long}} [x y] ...)
```

Parsing/extraction: `extract-hints` at `tools/callgraph.janet:663-773`
(dict-modifier branch at `:683-684`). A **bare `^type` param form** is also
accepted — `(defn f [x ^double y] ...)` — handled at `:685-739`; `^` return
hints at `:709-726`. Validation is `check-hints` at `:775-809`, which emits
`hint` lines and `hint-bad` problems. Aliases fold through `canon-hint`
(`:379`, table at `:222-231`). Extraction runs *before* the walk (`:1384-1385`)
so hints are visible to inference.

The working template cited by the owner is confirmed:
`janet-port/src/rng-kernels.janet:13`.

### (b) Type vocabulary — closed, no user types

`valid-hints` at `tools/callgraph.janet:205-214`:

- integers: `:long :i32 :i16 :i8 :u64 :u32 :u16 :u8`
- floats: `:double :float` (`:float` is true float32); `:number` stays boxed-width
- aggregates/opaque: `:array :tuple :pointer :string :keyword :symbol :bool
  :table :struct :buffer :fiber :function :cfunction :abstract`
- aliases (`:222-231`): `:i64/:int64→:long`, `:int/:int32→:i32`, `:f64→:double`,
  `:f32→:float`, etc.

C mapping: `emit-c-type-map` at `:252-294`. `:value` (`:289-294`) is a
representation leaf — **never a user hint**.

**User-defined types such as `:ctx` are rejected.** Experiment
(`/tmp/q1x/voc.janet`):

```
hint-bad 1 bad-param-hint      # {:params [:ctx] ...}
hint-bad 2 bad-returns-hint    # {:returns :ctx}
hints 4 2
```

Narrow widths work (`hint 3 i32 u8 float -> i16`), and a `:params`-only hint with
no `:returns` is legal (`hint 4 double double double -> -`).

### (c) Semantics — CHECKED at the boundary, TRUSTED inside, fail-closed

This is the question that determines the Phase 5 design, so it is answered in
three separate dimensions. **None of them admits undefined behavior.**

**1. Caller argument types → CHECKED, panic (not deopt).**
`wrapper-for` (`tools/callgraph.janet:4168-4284`) emits a per-argument
`janet_type(argv[i]) != EXPECTED` guard that calls `janet_panicf`
(`:4194-4195`), plus an arity guard (`:4275-4276`). The comment at
`:210-211` states it exactly: *"A hint is a contract; the emitted wrapper guards
it and panics loudly."*

Proven (`/tmp/q1x/guard.janet`) — passing a keyword to a `:double` param:

```
ok: 8
error: native blend argument 1 has wrong Janet type
  in native/blend
native-telemetry id=00000001 name=blend calls=1 arity-guard-failures=0 type-guard-failures=1
exit 1
```

So a violated hint is a **loud runtime error**, never silent UB, and **never a
deopt back to the VM**.

**2. Function body → TRUSTED, emitted as unguarded C.**
Inside the kernel the hinted C types are used directly (`double blend(double p0,
…)`), with no per-operation type checks.

**3. Hint vs. inferred body type → CROSS-CHECKED; contradiction refuses emission.**
A hint does not silently override the body. From `/tmp/q1x/voc.janet`:

```
native-skip 3 c2 ret slot is double not i16
```

The `:i16` return hint was *accepted as a hint* yet the function was **refused
native emission** because the body's inferred type disagreed. Fail-closed again.

**Relationship to PGO (the genuine EVIDENCE channel).**
Hints are contracts, *not* evidence: `tools/callgraph.janet:1750-1752` —
*"hints are direct contracts (used as-is for unboxing, never weakened);
call-site widening + fixpoint prove the unhinted rest."* Measured: the hinted
build converged in `rounds 1` vs `rounds 3` unhinted.

The **deopt-with-fallback** behavior the owner may be thinking of belongs to the
separate PGO dispatch tier, not to hints: `dispatch-wrapper-for`
(`:4723-4783`) emits a *mandatory boxed fallback* and only panics when none is
registered (`:4778-4779`); tier rationale at `:5277-5283`.

> **Summary for Phase 5:** hints are **CHECKED** at the ABI boundary
> (fail-closed panic), **TRUSTED** within the body, and **cross-checked**
> against inference. They are *not* trusted-with-UB, and *not* deopt-checked.
> PGO is the only evidence/deopt channel.

### (d) Scope — Tier-A kernels only

Hints feed Tier-A scalar-kernel candidacy (`pts`/`rts` consumed by the gate at
`tools/callgraph.janet:5225-5275`). `tools/loop-executor.janet` — the Tier-B
whole-function VM-frame executors — contains **zero** occurrences of `hint`
(verified by grep over all 560 lines). Its header (`:1-6`) confirms it is
bytecode/VM-frame driven: *"No dependency on scalar wrapper inference."*

**Hints have no effect whatsoever on Tier-B.**

### (e) Limits hints do NOT lift — confirmed

Both owner-cited limits reproduce, and hints do not bypass either.

- **Constructor poison.** `tools/callgraph.janet:2201-2202` sets `mkbuf`/`mkstr`/
  `mkstu`/`mktab` destinations to `:unknown`. This is a *flow-slot* type; hints
  only set params/returns, so they cannot reach it.
- **Backward-jump veto.** `tools/callgraph.janet:5258-5262` flags any `jmp` with
  a non-positive offset or any conditional jump with offset ≤ 0, including
  self-edges; `:5271-5273` converts that into the skip
  `"loop requires resumable VM quantum polls"`. Unconditional — no hint input.

Experiment (`/tmp/q1x/lim.janet`) — three *validly hinted* functions
(`hints 3 0`, i.e. all accepted, none bad):

```
native-skip 1 hloop loop requires resumable VM quantum polls   # backward jump
native-skip 2 hbuf  call to non-native or type mismatch        # buffer/new
native-skip 3 hstr  len of non-array                           # mkstr poison
native-lib 0 kernels
```

A further structural limit worth recording: `tools/callgraph.janet:5230`
(`(> (length pt) 0)`) means **zero-argument and vararg functions can never be
Tier-A kernels**, hinted or not.

### Q1 EXPERIMENT — generated-C diff and timings

Generated C is retained next to the output as `<out>-native/native.c`
(dump flags: `--emit-native` / `--emit-native-lib`, `tools/callgraph.janet:70-73`).

**Experiment 1 — function inference already proves (`/tmp/q1x/{unhinted,hinted}.janet`).**
A loop-free numeric chain called with literal doubles. Unhinted, inference
*already* proved it (`ptypes double double double long`, `native-lib 1 kernels`).
The hint changed only the width of the one param inference had widened to `long`:

```diff
-double blend(double p0, double p1, double p2, int64_t p3) {
+double blend(double p0, double p1, double p2, double p3) {
-    …, janet_unwrap_integer(argv[3])));
+    …, janet_unwrap_number(argv[3])));
```

Note the **type guard is byte-identical** (`JANET_NUMBER`) either way — the hint
changes *representation*, not the safety level.

Timings (20M calls, 3 runs): unhinted `2.05 / 2.14 / 1.81 s`; hinted
`2.06 / 1.75 / 1.88 s`. **No measurable difference** — as expected when
inference already succeeded. Both printed `189687561.688273`.

**Experiment 2 — where hints actually pay: inference cannot prove
(`/tmp/q1x/dyn-{u,h}.janet`).** Identical function, but reached through a table
(`(def tbl @{:blend blend})`, then `(get tbl :blend)`) — the exact
dynamic-dispatch shape `callgraph.janet:5235` calls out for ttfx's easing tables.

| | unhinted | hinted |
|---|---|---|
| build | `native-skip 1 blend types not proven: params [? ? ? ?]` | `ptypes double double double double` |
| kernels | **`native-lib 0 kernels`** | **`native-lib 1 kernels`** |
| `native.c` | 31 lines (no kernel) | 93 lines |
| time (3 runs) | 2.26 / 1.93 / 1.94 s | 1.20 / 1.31 / 1.23 s |

**→ ~1.56× faster; the hint *unlocks* native emission that inference refuses.**
Both printed `173437493.625`.

**That is the real semantics of a hint: it is an enabling contract, not an
optimizer directive.**

---

## Q2 — Unboxed aggregates and pointer ops

**Verdict: PARTIAL.** Two independent mechanisms exist. Each can do what the
other cannot, and *neither alone* expresses "a persistent array of flat entity
structs stepped in a native loop."

### Mechanism 1 — `table-flat`: closed-shape local tables → real C struct

Decision rule stated precisely at `tools/infer.janet:167-207`; plumbing and the
`table-flat` / `table-open` diagnostics at `tools/callgraph.janet:1757-1775`.
Field access rides the same `:kernel {:pread/:pwrite [ctype offset]}` contract
handled by `extract-intrinsics` (`tools/callgraph.janet:402-433`,
`kernel-intrinsics` at `:385-389`).

**This genuinely compiles to direct loads.** `/tmp/pilot`-style source:

```janet
(defn step {:hint {:params [:double :double] :returns :double}} [x v]
  (def st @{:px 0.5 :vx 0.5})
  (put st :px x) (put st :vx v)
  (+ (* (st :px) 2.0) (* (st :vx) 3.0)))
```

Build: `table-flat 1 step slot 2 px:double,vx:double` → `native-lib 1 kernels`.
Emitted C (`ofc/app-native/native.c`) — a stack struct, direct member traffic,
zero boxing, zero `janet_*` calls, zero allocation:

```c
double step(double p0, double p1) {
    struct nsh1_2 { double f0; double f1; };
    struct nsh1_2 nshv2 = {0};
    nshv2.f0 = (double)(s3);
    nshv2.f1 = (double)(s2);
    nshv2.f0 = (double)(p0);
    nshv2.f1 = (double)(p1);
    s4 = nshv2.f0;  s5 = s4 * 2;
    s4 = nshv2.f1;  s6 = s4 * 3;
    s4 = s5 + s6;   return s4;
}
```

**Preconditions (ablated empirically):**

| variation | result |
|---|---|
| init literal `0.0` | **opens** — `0.0` compiles to `ldi`, typing the field `:long`; a `:double` `put` into a `:long` field is refused |
| init literal `0.5` | `table-flat` ✅ |
| read via `(get st :px)` | **opens** (`get` is an escape) |
| read via `(in st :px)` | **opens** |
| read via callee form `(st :px)` | ✅ — the only accepted read form (rule 3(a), "pread") |
| read in tail position | ✅ (`tcall` accepted) |

**Hard limits:**

- **No cross-function lifetime.** Returning it (`table-open … → 0 kernels`) or
  passing it to another kernel (`table-open … → 0 kernels`) both open the shape,
  exactly as `tools/infer.janet:200-204` mandates. Scope is explicit at
  `:205-207`: single function, single construction site, entry block only.
- **`:long` + `:double` mixing works** (`struct nsh1_2 { int64_t f0; double f1; }`),
  but asymmetrically per rule 3(b): `:long`→`:double` field ✅,
  `:double`→`:long` field opens ❌.
- **`mkstu` (`{...}` struct literals) unsupported** — `(def st {:px x :vx v})`
  yields no `table-flat` line at all and `slot s4 mixes unanalyzable
  keyword/unknown`. Only `mktab` (`@{...}`) is dispatched (`infer.janet:468`);
  the comment at `:207` is accurate.
- **No field-count cap** — 10 fields emit fine (`k0:double,…,k9:double`).
  Validator `infer.janet:481-497` checks only
  even/≥2/keyword-const/no-dup/`:long`-or-`:double`.
- **Single construction site, entry block only.** A second `mktab` on the slot —
  *even a shape-identical one* — opens it sticky (`infer.janet:474-480`).
- `movn`/`movf` **propagate** the shape and its rules (`infer.janet:499+`), which
  surfaces as two `table-flat` lines for one logical table.
- Width mapping (`nshape-ctype`, `callgraph.janet:1784`): `:long`→`int64_t`,
  otherwise `double`.

### Mechanism 2 — `ffi/defstruct` + `:kernel` intrinsics → direct pointer loads

**This is the stronger of the two mechanisms and it is a fork extension.**

`ffi/defstruct` (`src/boot/boot.janet:4792-4866`, **not upstream**) generates a
whole kit from one declaration: `Name/descriptor` (`:4824`), `Name/size`
(`:4825`), `Name/offsets` (`:4826`), `Name/new` (`:4829`), `Name/malloc`
(`:4833-4837`, `unsafe/calloc` + per-field `unsafe/poke`), `Name/free` (`:4839`),
plus per-field getters (`:4855-4857`) and setters (`:4862-4864`).

The crucial part is the `:kernel` metadata it attaches (`:4848-4851`, `:4858-4861`):

```janet
{:hint {:params [:pointer]    :returns ht} :kernel {:pread  [kt off]}}
{:hint {:params [:pointer ht] :returns ht} :kernel {:pwrite [kt off]}}
```

The emitter consumes that through `kernel-intrinsics` (`tools/callgraph.janet:389`,
populated by `extract-intrinsics` `:402-434`, dispatched at `:3755-3765`) and
lowers it in `emit-intrinsic-call` (`:2598-2650`). The money line is `:2634`:

```janet
(def mem (string "(*(" cty "*)((char*)(" pexpr ") + " koff "))"))
```

**Verified end to end.** A 5-field `{double×4, int}` animation struct stepped in
a hinted kernel emitted *every* field access as a direct load/store, including
the `int`:

```c
double step(void* p0, double p1) {
    s3 = (double)((*(double*)((char*)(p0) + 0)));
    s2 = (double)((*(double*)((char*)(p0) + 16)));
    s4 = s2 * p1;  s2 = s3 + s4;
    (*(double*)((char*)(p0) + 0)) = (double)(s2);
    s4 = (double)((*(int32_t*)((char*)(p0) + 32)));
    s5 = s4 + 1;
    (*(int32_t*)((char*)(p0) + 32)) = (int32_t)(s5);
    …
}
```

Running the real AOT binary: `native-lib 1 kernels`, `calls=5`,
`arity-guard-failures=0 type-guard-failures=0`, result bit-identical to the
interpreter. **Values do not re-box at field boundaries, and kernel→kernel
pointer calls stay native** (`s2 = inner(p0);` beside a direct load).

**FFI is on by default and live inside AOT images.** `src/include/janet.h:198-204`
makes `JANET_NO_FFI` an opt-*out*, and the fork **removed upstream's x86-64 arch
guard** (commit `398833eb`, "Enable FFI module unconditionally"), which is why
this works on arm64. `Makefile:165` always compiles `src/core/ffi.c`;
`meson_options.txt:24-25` default both FFI options to `true`. Confirmed by running
`ffi/size`, `ffi/align`, `unsafe/malloc`, `unsafe/poke`, `ffi/read`, `ffi/struct`
inside a built AOT image.

**Symbol inventory** (`src/core/ffi.c:2440-2465`): `ffi/native :2440`,
`ffi/lookup :2441`, `ffi/close :2442`, `ffi/signature :2443`, `ffi/call :2444`,
`ffi/struct :2445`, `ffi/write :2446`, `ffi/read :2447`, `ffi/size :2448`,
`ffi/align :2449`, `ffi/struct-info :2450`, `ffi/calling-conventions :2465`; and
14 `unsafe/*` cfuns at `:2451-2464` — `trampoline, jitfn, malloc, free, calloc,
realloc, ptr-add, ptr-sub, ptr-eq, poke, exec-alloc, exec-protect,
pointer-buffer, pointer-cfunction`.

> **Naming correction:** there is **no `unsafe/peek`.** The read side is
> `ffi/read` (`:2447`); only the write side is `unsafe/poke` (`:2460`).

**Limits (each verified by experiment):**

- **No pointer arithmetic in the emitter.** `unsafe/ptr-add` is a plain cfunction
  with no entry in any emitter table (zero hits for `ptr-add` in
  `tools/callgraph.janet`). A stride loop over an array of structs is refused
  outright: `native-skip 5 loopsum call to non-native or type mismatch`.
  **This is the single blocking gap for an animation system over N entities.**
- **Only `:double`/`:float`/`:int` fields are kernel-mapped** (gate at
  `src/boot/boot.janet:4783-4789`). `:long`, pointers and nested structs are
  boxed-only and refuse fail-closed.
- **Hand-written `ffi/read`/`unsafe/poke` are NOT lowered and poison the
  function** — the lowering is strictly whitelist-by-`:kernel`-metadata. A bare
  `(ffi/read :double p 0)` yields `native-skip … slot s2 mixes keyword/u64ok`:
  the `:double` keyword argument poisons the slot and disqualifies the function.
- **A getter in tail position refuses** (guard at `tools/callgraph.janet:2609`);
  worked around by `(+ 0 (M/frame p))`, but it is a live foot-gun.

### The two mechanisms are exactly dual — by design

This duality is explicit in the source. The emission helpers at
`tools/callgraph.janet:1776-1784` describe `table-flat` as *"the auto-discovered
sibling of the hand-declared `ffi/defstruct` path -- same field contract
`:long`/`:double`, same fail-closed guards"* (`:1781-1783`).

| | `ffi/defstruct` | `table-flat` |
|---|---|---|
| direct-load field access | **YES** | **YES** |
| allocation | **heap** (`unsafe/calloc`) | **stack** (`struct nsh1_2 nshv2 = {0};`) |
| lifetime | **persists across calls** | **function-local only** |
| crosses fn boundaries | **YES** (`s2 = inner(p0);`) | **NO** (return/arg → sticky open) |
| `:long` fields unboxed | **NO** (boxed-only) | **YES** (`int64_t`) |
| array / stride | **NO** (no `ptr-add`) | N/A (no addressing at all) |
| loop inside the kernel | OK | OK standalone; **refused** by `--aot --native` |
| tail-position read | **refused** (`:2609`) | **OK** (`tcall` accepted) |
| declared by | hand (`:kernel` metadata) | auto-inferred |

Note the tail-position row inverts between the two — a getter in tail position
refuses under `ffi/defstruct` but is fine under `table-flat`.

So: **a flat animation struct whose field access compiles to a direct load —
YES, today, by two independent routes.** `ffi/defstruct` is the vehicle for
*persistent* per-entity state; `table-flat` is the zero-ceremony, zero-heap route
for a *per-call* working set, and is the only one that unboxes `:long` or
supports heterogeneous `:long`+`:double` layout. What remains genuinely
impossible is a **persistent array of such structs strided in a native loop**,
blocked solely by the missing `ptr-add` intrinsic.

**The three shapes actually achievable today:**

1. **One** persistent entity via `ffi/defstruct` + a per-call native `step`
   (proven: 5 native calls, 0 guard failures, bit-identical).
2. Scalars-in → `table-flat` working struct → scalar-out per call, with the
   entity loop staying in the interpreter (proven: `native-lib 1 kernels`).
3. A full native C loop over a `table-flat` struct — but **only** via the
   standalone `--emit-native` + manual `cc` tier (20/20 `PARITY_OK`), *not* the
   deployable `--aot --native` tier.

### Two defects found in passing

**Defect A — emitted C relies on strict-aliasing UB, and the production build
does not suppress it.** The generated `(*(int32_t*)((char*)p + 32))` type-puns
bytes written as `double`. A mixed `{double…, int}` kernel **diverged from the
interpreter under optimization**:

```
cc -O2 …                       → -4 3 6 9     (wrong)
cc -O1 …                       → -4 3 6 9     (wrong)
cc -O0 …                       → -4 2 4 6     (correct)
cc -O2 -fno-strict-aliasing …  → -4 2 4 6     (correct)
interpreter ground truth       → -4 2 4 6
```

The real AOT path (`src/boot/boot.janet:6472-6476`, plain-image `:6127`) compiles
at `-O3` and **never passes `-fno-strict-aliasing`**. In genuine use the memory
comes from `unsafe/calloc` (no declared type) and fields occupy distinct offsets,
so the real `--native` build did not miscompile — but nothing in the emitter
*enforces* that invariant. One flag fixes it.

**Defect B — the mixed-struct parity oracle is unsound.** The pointer fixtures
are `double[8]` on both sides (C at `tools/callgraph.janet:4098-4104`, Janet
`mkptr` at `:4131-4134`). Any struct with a non-`:double` field is therefore
"validated" by reinterpreting double bytes as `int32` — both UB-sensitive per
Defect A and semantically meaningless. `test/suite-callgraph.janet:883-903`
exercises only an all-`:double` `Point`, so the shipped suite never covers the
mixed-type path that `docs/internals/native-memory.md:143-148` advertises.

*Doc accuracy:* `native-memory.md:122-155` is otherwise accurate line by line
(the `Point/size => 24`, `offsets => {:x 0 :y 8 :n 16}` example matches
`test/suite-ffi.janet:60-61`). Its "**8× measured on a stepping loop**" claim
(`:146`) is **unbacked** — no committed benchmark supports that number.

---

## Q3 — Arena allocation and whole-module emission

### (a) Arena / region allocation — **ABSENT**

- `janet_smalloc` / `janet_srealloc` / `janet_sfree` are declared at
  `src/include/janet.h:2360-2364` and are **upstream-unchanged** —
  malloc-per-call plus an O(n) tracking array (`src/core/gc.c:3315-3381`).
- **No Janet-level binding** for any arena/scratch/region/bump/slab allocator
  (`JANET_REG` sweep finds none). User code gets plain libc via `unsafe/malloc`.
- **Emitted code uses no allocator at all**: zero `smalloc`/`sfree`/`malloc`
  occurrences in `tools/callgraph.janet`, in `tools/loop-executor.janet`, or in
  the real 2601-line ttfx `native.c`. Allocation deliberately *exits* the native
  region to the VM (`tools/loop-executor.janet:4,142,356-360`).
- The only bump cursor is the GC's private slab nursery
  (`src/core/gc.c:1358-1420`), not addressable as a region.
- `docs/internals/phase2b-gc-design.md:70-75` lists bump regions as an
  explicitly **deferred** option — the doc is accurate.

### (b) Whole-module emission — **ONE TU, non-static kernels, direct cross-calls EXIST**

- **One TU per build**: `tools/callgraph.janet:5668` writes a single `native.c`.
  The real ttfx artifact is one 72k-line TU with 237 functions.
- **Kernel bodies are non-`static`** and emitted without prototypes, topologically
  ordered by a 4-round fixpoint (`tools/callgraph.janet:5361-5375`). Confirmed in
  the real ttfx `native.c`: all 8 kernels declared as e.g.
  `double round_half_even(double p0) {` — no `static` (and `T` symbols under `nm`).
- **Registration** is `JanetReg` + `janet_cfuns(env, "native", regs)`; only the
  boxed `*_wrapper` is registered, so the raw kernel stays a plain C symbol:

```c
void native_init(JanetTable *env) {
    static const JanetReg regs[] = {
    { "step", step_wrapper, NULL },
    { NULL, NULL, NULL } };
    janet_cfuns(env, "native", regs);
```

- **Direct C cross-calls already work.** `/tmp/q1x/k2k.janet` (two hinted
  kernels, one calling the other) emits a literal C call — no registry, no boxing:

```c
double outer(double p0, double p1) {
    s3 = inner(p0);
    s2 = inner(p1);
    s4 = p0 * p1;
    …
}
```

  Emission shapes are at `tools/callgraph.janet:3814-3830` (void statement,
  value assignment, tail call). Verified at the machine level on a 3-deep chain
  `hyp2`→`addsq`→`sq`: `nm -gU` reports `T _sq`, `T _addsq`, `T _hyp2`
  (**external linkage**, confirming non-`static`), and `-O3` collapsed the whole
  chain into **3 ARM64 instructions** (`mul`/`mul`/`add`).
- **Ordering is by 4-round fixpoint, not prototypes** (`:5361-5375`). Because no
  forward declarations are emitted, a kernel can only call one appearing
  *earlier* in the file; the fixpoint guarantees callee-before-caller
  (verified with deliberately out-of-order sources). Survivors are reported as
  `"calls non-emitted native"` (`:5378-5381`).
- **Recursion is refused.** A self-recursive `fib` fails at the load-self opcode:
  `native-skip 1 fib unhandled op lds`. **Recursive kernels do not exist**, so
  the no-prototype design is never stressed by a back edge.
- **Dispatch-branch kernels are deliberately not composable**: they are excluded
  from `emitted-names` (`:5382-5386`), so other kernels refuse to call a
  dispatcher natively and stay boxed — conservative by design.
- **Three tiers with different linkage.** (1) Kernels: external linkage,
  `JanetReg` + `janet_cfuns(env, "native", regs)`. (2) Loop/leaf executors
  `nx_<id>`: **`static`**, registered structurally via
  `janet_native_registry_add` (`:5651`) and matched by a shape key, not a name;
  double opt-in (`JANET_NATIVE_EXECUTORS=1` at emission **and**
  `-DJANET_NATIVE_EXECUTORS` at build). (3) FFI alias thunks:
  `static void *ffi_alias_ptr_<id>` (`:5086`).
  **Zero executor→kernel calls exist in the real artifact.**
- **ttfx exercises none of it**: all 8 kernels are non-static in one TU but make
  **zero** cross-calls, because their callers (`randbelow`, `rng-random`) are
  themselves not kernels.

**Corrections to existing docs** (all first-hand verified):

- `docs/internals/aot-plan.md:996` claims kernels emit as `static int64_t|double
  f(...)` — **wrong**. Bodies carry no storage class (`callgraph.janet:3855`) and
  link as `T`. Only wrappers, executors, helpers and atom slots are `static`.
- `janet-port/src/rng-kernels.janet:8-10` claims kernel-to-kernel calls "refuse
  `:array` returns" — **over-broad/stale**. The quoted error string is real
  (`callgraph.janet:3840`), but per `ret-ok` clause 3 (`:3796-3802`) an
  `:array`-returning kernel **does** compose when the destination slot is proven
  `:array`. Verified both ways: `s3 = mk(p0, p1);` with a `JanetArray*` return
  succeeds, while adding a `nil` branch to the same slot yields
  `native-skip … slot s2 mixes nil/array`. What actually refuses is a **mixed
  destination slot**. (The file's *conclusion* is still right for ttfx — those
  kernels are called back-to-back from boxed code — but its stated rule is not.)
- `docs/internals/native-continuation-design.md:847-851` states *"a static
  native-to-native direct C call is not possible in this toolchain"* —
  **true for the executor tier only, and misleading as a blanket statement.**
  Its §2.1 scope is `nx_A`→`nx_B` (`:193`), which genuinely is impossible
  (structural runtime resolution). The **kernel tier does make static direct C
  calls**, as proven above. A reader taking `:847` literally concludes wrongly.
- `docs/internals/aot-notes.md:257-260` ("now 4 kernels … `bitlen`") is a dated
  changelog and is **stale**: the current ttfx artifact has 9 kernels and no
  `bitlen`. By contrast `aot-notes.md:114` and `:262-264` are exactly right.
- **Systemic line drift** in `native-continuation-design.md`: 3 of 3 checked
  `callgraph.janet` citations were off by 1–8 lines (e.g. the structural key is
  at `:5500-5510`, not `:5508-5518`; registry publish at `:5650-5651`, not
  `:5654-5655`). Symbols are correct; re-resolve line numbers rather than trust them.

Note array *elements* still re-box (`janet_unwrap_number(s3->data[…])`).

### Compile flags

`src/boot/boot.janet:6472-6478` (and `:6127`):

```
cc -O3 -fwrapv -fno-math-errno -fno-trapping-math -w -DJANET_AOT_NATIVE_INIT \
   -I <root>/build -I <root>/src/include  app.c native.c libjanet.a [ldflags] -lm -lpthread -o out
```

- `-O3` default; `-Oz` with the `-Oz` flag (`:6060`). `CC` is overridable (`:6057`).
  There is no `--opt` flag and no `CFLAGS` hook on the AOT path.
- `-fwrapv` is a **correctness** requirement for the `:wrap` int policy
  (rationale at `:6122-6126`); `-fno-math-errno -fno-trapping-math` is a
  deliberate oracle-identical fast-math subset, explicitly *not* `-ffast-math`.
- **No LTO, no `-march`/`-mcpu`** — both still unchecked `- [ ]` in
  `docs/internals/extreme-perf.md:113,117`.
- **Optimization asymmetry worth knowing:** `native.c` compiles at `-O3`, but it
  links against `libjanet.a` built at **`-O2`** (`Makefile:56`,
  `CFLAGS?=-O2 -g`), with **no LTO between them**. The runtime the kernels call
  into is therefore a less-optimized TU.
- `--pgo` / `--pgo-auto` are *Janet-level* profiling, **not** compiler PGO — they
  seed kernel/executor selection via `--profile <file>` (`:6159`, `:6456`); no
  `-fprofile-generate`/`-fprofile-use` is ever passed.
- `--ldflags "-flto -mcpu=apple-m1"` is an undocumented but **verified working**
  escape hatch — the words land in the single `cc` invocation before `-o`
  (`:6478`), so they reach the compiler, not just the linker.
- Intermediates are **retained by default**; `-c` keeps them and prints paths
  (`aot-clean` at `:6566-6569` only removes them on the non-`-c` path).

### Bonus finding: the loop veto is a *tier* restriction, not a codegen gap

The code generator can already compile loops. `/tmp/q1x/lp.janet`, one hinted
function with a `while`:

- deployable `--aot --native` path → `native-skip 1 sumloop loop requires
  resumable VM quantum polls`, `native-lib 0 kernels`
- standalone `--emit-native` path → `native 1 fns 4 samples`, and the emitted C
  contains the real loop (`L2:; … if (!s3) goto L9; … goto L2; L9:;`)

`src/boot/boot.janet:6456` shows `--aot` wires only `--emit-native-lib`. So
loop-capable emission exists and is sample-verified, but is **not reachable from
the deployable path** — which is precisely what the native-continuation work
(`callgraph.janet:5253-5256`) is meant to unlock.

---

## Q4 — "VM tests near LuaJIT-class speeds"

**Verdict: NOT SUPPORTED by committed evidence.** (Archaeology only; no
benchmarks were run.)

*Repo-topology correction:* `ML/ttfx/janet-port/` is **not** a separate repo —
`git rev-parse --show-toplevel` resolves it to `/Users/pankajdoharey/Development/Projects/ML/ttfx`.
There are two repos: `janet` and `ttfx` (which contains `janet-port/`).
`janet-port/docs/` is empty and `janet-port/bench/` holds only `hot-stack.janet`.

The scout-cited file is `/Users/pankajdoharey/Development/Projects/ML/ttfx/bench-engines.md`
(tracked, 576 lines, at the ttfx repo *root* — not under any `docs/`). Two
corrections: the "Node at 31/37" figure is a **port-coverage count**, not a speed
number (`:89-92`: Node reports `unknown effect` for 6 effects), and **LuaJIT
appears zero times in that file**. Its CLI table (`:182-188`) actually shows
janet-AOT **4–12× behind** Node/Bun and 32–166× behind Rust.

The only committed LuaJIT comparison is `ttfx/bench/languages.md` (commit
`dfe223b`), which cleanly separates "Bender VM" (interpreter) from
"Bender release" (AOT):

| metric | Bender VM (interpreter) | LuaJIT | ratio |
|---|---|---|---|
| CLI geomean/Rust, logo (`:86-94`) | **0.032×** (ranked 7/7, last) | 0.151× | **0.21×** |
| CLI geomean/Rust, paragraph (`:128-130`) | **0.041×** | 0.192× | 0.214× |
| raylib GUI FPS/Rust (`:180-188`) | 0.756× | 0.990× | 0.76× |

The interpreter is **~4.7× slower than LuaJIT** on CLI throughput, is ranked
**7th of 7 engines (last)**, and **loses to plain Lua 5.5 by roughly half**
(0.032 vs 0.067 on logo; 0.041 vs 0.108 on paragraph) — i.e. it does not lead
even the non-JIT interpreters. `languages.md:191-194` states this outright:
*"Among non-JIT bytecode interpreters, Lua 5.5 is first and Bender VM second."*
This is the fork's *own*
reproducible evidence: committed `validate-languages.py` / `compare-languages.py`,
raw 245-sample JSON, pinned versions, a 210/210 byte-parity gate.

Two numbers that could be misread as supporting the claim:

- `janet/docs/internals/profile-guided-optimizer-plan.md:12,202` cites
  **0.47–0.48× LuaJIT** — that is the **AOT+PGO native release**, not the interpreter.
- The flattering **0.865× Rust** is **GUI render FPS**, which has a shared OpenGL
  floor; the docs themselves forbid mixing it with throughput.

The fork's own `README.md:94` still lists *"As fast as or faster than Lua"* as an
**open** checkbox, and `:494-498` describes the interpreter as *"about the same
speed as other interpreted languages without a JIT… a bit faster than Python"* —
consistent with `janet-port/README.md:41` (547 fps vs Python TTE ~543 fps).

Committed caveat text is extensive and honest throughout (±10–30% GUI noise,
startup included, *"not proof of intrinsic language/runtime speed"*).

---

## Implications for the typed-effects pilot

### How much headroom is actually there

The real ttfx CLI build (`bender --aot bin/ttfx-cli.janet --native`) emits
**8 kernels** and refuses **783** definitions. Breakdown of the refusals:

| reason | count | hint-fixable? |
|---|---|---|
| `loop requires resumable VM quantum polls` | 169 | **No** — needs native continuations |
| `types not proven`, zero-param (`params []`) | 115 | **No** — `callgraph.janet:5230` requires ≥1 param |
| `types not proven`, ≥1 unproven param | **338** | **Yes — this is the hint-addressable population** |
| duplicate registration / mixed slots | rest | No |

So **~43% of all refusals (338/783) are the population hints directly address** —
and that is an *upper bound*, because a hinted function must still survive
body-op lowering.

Adoption today is essentially zero: **one** ttfx file uses `:hint`
(`src/rng-kernels.janet`, 4 hints) and **no** file uses `:kernel`. 4 of the 8
emitted kernels come from that single file; the other 4 were inference-proven.
Across ~30 effect files, **no effect code is natively compiled at all.**

### Validated pilot recipe (new files, no edits)

I validated the recommended approach end-to-end on a **real ttfx function**,
`radial-fraction` (`janet-port/src/color.janet:191-203`), which the real build
refuses with `types not proven: params [? ? ? ? ? ?] ret double`.

Critically, **adding hints alone was not enough**:

```
typed  → native-skip 1 radial-fraction slot s16 mixes unanalyzable double/unknown/u64ok
```

Diagnosis: the type lattice is **per-slot, not SSA**, and Janet's register
allocator reuses slot 16 for an integer literal, a `math/sqrt` *cfunction
constant* (`:unknown`), and a double. `math/sqrt`/`math/pow` lower fine on their
own (`sqrt((double)(p0))`, verified in isolation — 4/4 kernels); the collision is
caused by **named-local pressure**, not by the math calls.

Reducing the number of named `def`s (same arithmetic, same result) fixed it:

| variant | build | time (3 runs) |
|---|---|---|
| `base` — verbatim ttfx source | `native-lib 0 kernels` | 1.27 / 1.20 / 1.18 s |
| `typed4` — hints + fewer named locals | `native-lib 1 kernels` | **0.34 / 0.33 / 0.36 s** |

**≈3.5× faster, bit-identical output** (`2734599.96095399` both). Files:
`/tmp/pilot/{base,typed4}.janet`.

**Cheapest pilot shape — strictly additive, no edits to originals:**

1. For each hot effect, add a sibling file (e.g. `src/effects/rain-typed.janet`)
   that re-defines only the hot leaf functions with `:hint`, and have the typed
   variant require the original for everything else.
2. Target leaves matching the *hint-addressable* profile: **≥1 param**,
   **loop-free**, **pure numeric**. Skip loops and zero-arg functions entirely —
   they are structurally immune.
3. Expect to **also restructure the body**: minimize named locals, use
   non-integral literals (`0.5`, not `0.0`) for flat-table field initializers,
   and read flat-table fields with the callee form `(st :px)` — never
   `(get …)`/`(in …)`.
4. Gate every variant on the build diagnostics, which are the real contract:
   `native-lib N kernels`, `table-flat`, `native-skip <reason>`, `hint-bad`.
5. Benchmark typed-vs-untyped by building each entry point twice and diffing
   output for bit-parity first, then timing.
6. Retain and inspect the generated C via `<out>-native/native.c`
   (or `--emit-native-lib`) — it is the ground truth for whether anything unboxed.

Because hinting is fail-closed and cross-checked, this pilot **cannot silently
miscompile**: a wrong hint either refuses to emit or panics loudly at the boundary.

### Missing pieces, ranked by leverage

1. **Native continuations / resumable VM quantum for loops** — unblocks the single
   largest bucket (169 refusals) and every per-frame effect loop. The loop
   *codegen already exists and is parity-tested* in the `--emit-native` tier; what
   is missing is making it reachable from the deployable `--emit-native-lib` path
   (`callgraph.janet:5253-5262`). Highest leverage by a wide margin.
2. **SSA / per-pc slot typing instead of per-slot** — the `mixes unanalyzable …`
   family is an artifact of register reuse, not of genuinely untyped code. It
   forced a source rewrite even in my successful 3.5× pilot, and it silently caps
   how much of the 338-refusal population hints can actually convert.
3. **Pointer arithmetic (`ptr-add`) as a `:kernel` intrinsic** — the *single*
   remaining blocker for a persistent array of flat entity structs, i.e. real
   animation-state packing. Everything else on that path already works:
   `ffi/defstruct` persists across calls and already emits direct loads, so this
   is a narrowly-scoped addition (one constant-stride intrinsic) with
   disproportionate payoff. Promoted above cross-function `table-flat` lifetime
   for exactly that reason.
4. **Cross-function lifetime for flat aggregates** (pass/return a `table-flat`
   struct) — today it can only ever be a per-call working set. Lower priority
   than #3 because `ffi/defstruct` already covers persistent state.
4c. **Extend Phase 5 to `mkstu` and to whole-program shape tracking** — the
   decision-rule comment at `tools/infer.janet:205-207` names this as the
   intended next step, and it is the same lever as #4: whole-program tracking is
   precisely what would let a flat struct cross a function boundary.
4b. **Safety fixes that are nearly free**: add `-fno-strict-aliasing` at both
   `aot-run-cc` sites (`src/boot/boot.janet:6127`, `:6472`) to close the Defect-A
   UB exposure, and give the parity harness a `char`-backed or per-struct-typed
   pointer fixture (`tools/callgraph.janet:4098-4134`) plus a mixed-type case in
   `test/suite-callgraph.janet:883` to close Defect B. These are correctness,
   not performance, and should precede any wider rollout of struct kernels.
5. **Zero-arg/vararg kernel support** (`callgraph.janet:5230`) — 115 refusals, and
   cheap relative to items 1–4.
6. **An arena / per-frame region allocator** exposed to both user code and emitted
   kernels — currently fully absent; needed before native loops can allocate
   without exiting to the VM.
7. **`-flto` / `-march` plumbing as first-class flags** — today only reachable via
   the undocumented `--ldflags` escape hatch; both still unchecked in
   `extreme-perf.md:113,117`. Also consider `-fno-strict-aliasing` given the
   punning exposure noted in Q2.
8. **Honest benchmark hygiene** — retire or annotate the "near-LuaJIT" framing;
   the fork's own `bench/languages.md` contradicts it for the interpreter, while
   the AOT+PGO path (0.47–0.48× LuaJIT) is the defensible claim.

---

## Reproduction index

| Experiment | Path | Shows |
|---|---|---|
| hinted vs unhinted (inference already proves) | `/tmp/q1x/{unhinted,hinted}.janet` | C diff is param width only; no timing delta |
| hint unlocks dynamic dispatch | `/tmp/q1x/dyn-{u,h}.janet` | 0→1 kernels, 1.56× |
| boundary guard panics | `/tmp/q1x/guard.janet` | fail-closed, no deopt |
| hints do not lift Tier-A limits | `/tmp/q1x/lim.janet` | 3 valid hints, 0 kernels |
| vocabulary / user types | `/tmp/q1x/voc.janet` | `:ctx` rejected; body/hint cross-check |
| flat struct direct loads | `/tmp/q1x/fc.janet`, ablations `ab-*.janet` | real C struct + preconditions |
| kernel→kernel direct call | `/tmp/q1x/k2k.janet`, `k2ka.janet` | `inner(p0)`, `mk(p0,p1)` |
| loop tier split | `/tmp/q1x/lp.janet`, `/tmp/q1x/en/native.c` | veto is tier-level, not codegen |
| real-function pilot | `/tmp/pilot/{base,typed4}.janet` | 3.5×, bit-identical |
| real ttfx build | `/tmp/ttfxb/ttfx-native/native.c` | 8 kernels, 783 skips, one TU |

## Limits of this audit

- Q4 is archaeology only; no benchmark was executed and LuaJIT was not run.
- Timings are wall-clock `/usr/bin/time -p`, 3 runs, on one unpinned machine with
  no CPU isolation; the 1.56× and 3.5× figures are robust to that noise but the
  "no measurable difference" in Experiment 1 is a null result, not a proof of
  exact equality.
- The seven background docs received a targeted citation sweep (every
  `file:line` I reproduce above was checked first-hand), not an exhaustive audit.
- The 338 "hint-addressable" count is an upper bound on candidates, **not** a
  prediction of 338 new kernels; body-op lowering and slot-typing will reject a
  substantial share, as the `radial-fraction` pilot demonstrates.
