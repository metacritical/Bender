# Native emission coverage vs namin/inc

Comparison against `namin/inc` (Ghuloum, *An Incremental Approach to Compiler
Construction*), cloned at `/tmp/inc` and read from `src/compiler.scm`. inc is a
Scheme->x86 compiler in ~47 numbered test stages (`src/tests-1.1-req.scm`
through `tests-6.7-req.scm`) emitting **51 primitives** natively.

Our side: HEAD `0966547c`. Two independent tiers — the **kernel** tier
(`native-lib N kernels`, refuses any backward branch) and the **frame-executor**
tier (`native-executor <id> <name>`, compiles loops, off by default). Opcode
policy at `tools/loop-executor.ben:59-69`; contract enforced by
`test/native-executor-allowlist.py` (reports 60/64 emitter opcodes admitted).

## Where we match or exceed inc

| Capability | inc | ours |
|---|---|---|
| Integer add/sub/mul | `fx+ fx- fx*` | `add sub mul` + immediates |
| Increment/decrement | `fxadd1 fxsub1` | `addim subim` |
| Division/remainder | `fxquotient fxremainder` | `div divim divf mod rem` |
| Bitwise | `fxlogand fxlogor fxlognot` | `band bor bxor bnot` |
| Shifts | — | `sl sr slim srim` (signed; `sru/sruim` excluded) |
| Comparisons | `fx= fx< fx<= fx> fx>=` | `eq neq lt gt lte gte` + immediates |
| Zero test | `fxzero?` | `eqim` |
| Identity | `eq?` | `eq` |
| Conditional branch | if | `jmpif jmpno jmpni jmpnn` |
| Tail call | yes (step 4.x) | `tcall` |
| Container construction | `make-vector make-string` | `mkarr mkbuf mktup mkstr mkstu mktab mkbtp` |
| Indexed read | `vector-ref string-ref` | `in inim get geti` |
| Indexed write | `vector-set! string-set!` | `put` |
| Length | `vector-length string-length` | `len` |
| **Loops** | **no loop form — recursion + tail calls only** | **emitted directly (`jmp` back edge)** |

We cover more container kinds (arrays, buffers, tuples, tables, structs against
inc's vectors and strings), have division/modulo and shifts that inc's fixnum
steps lack, and emit real loops rather than relying on tail-recursion.

## Where inc is ahead — the actual gaps

Verified by compiling `/tmp/elig.ben`, four functions differing only in shape:

```
native-executor 1 plain       # leaf numeric loop          -> EMITTED
native-executor 4 arr         # indexed array get in loop  -> EMITTED
(no executor) capturing       # closes over an outer def   -> REFUSED
(no executor) varar           # vararg                     -> REFUSED
```

The gate is `base-eligible?` at `tools/loop-executor.ben:65-69`:

```janet
(and (d :bytecode-words) (not (d :vararg)) (not (d :structarg))
     (not (d :namedargs)) (empty? (or (d :environments) []))
     (empty? (or (d :defs) []))
     (every? (map |(allowed-op? ($0 0)) (d :bytecode))))
```

| Gap | inc | ours | Severity |
|---|---|---|---|
| **Closures / captured environment** | emitted (steps 4.1-4.3) | **refused** — `(empty? (d :environments))` | **highest.** Real game code captures state constantly; every such function falls back to the interpreter. |
| **Varargs / `apply`** | `apply` emitted | **refused** — `(not (d :vararg))` | medium |
| **Type predicates** | `fixnum? boolean? char? null? pair? vector? string? symbol? procedure?` all emitted inline | **not opcodes** — they are cfunctions, so a predicate is a `call`, not native test | medium; cheap to add as guards on proven types |
| Nested definitions | n/a | refused — `(empty? (d :defs))` | low |
| `foreign-call` | emitted | FFI alias wrapper only (stage 2, `native-loop-executors.md`) | low |

## Not real gaps (representation differences)

inc's `cons car cdr set-car! set-cdr!` and its char primitives
(`char->fixnum fixnum->char char=`) have no counterpart because this runtime has
no pair type and no distinct char type. inc's tagging scheme
(`fxshift 2`, `objshift 3`, `pairtag #x01`, `closuretag #x02`, ...,
`compiler.scm:3-24`) is its own immediate encoding; ours is the existing
Bender value representation, and emitted code keeps all original slots boxed and
GC-scanned (`native-loop-executors.md`, "ABI, ownership, and registration").

## Honest framing

inc has no GC, no preemption, no threads. Its native code can keep values in
registers across a whole function because nothing can interrupt it. Our emitter
must keep every slot boxed and GC-scanned, and the kernel tier refuses loops
outright because a kernel cannot perform the VM's resumable quantum polls. So
"equivalent" is the wrong target for the tagging and register discipline; the
right target is **equivalent operation coverage** under our safety constraints.

On that measure we are close on arithmetic, comparison, branching, containers
and tail calls, and clearly behind on **closures**, varargs and inline type
predicates.

## Recommended order

1. **Admit closures to the executor tier.** This is the single highest-value
   gap. Captured bindings already live in GC-scanned environment storage, so the
   question is whether the emitted code can read them under the existing rooting
   rules, not whether it is safe in principle. Measure the before/after on a
   capturing loop.
2. **Emit type predicates as native guards** where the PGO profile already
   proves the type; this also feeds the "types not proven" refusals seen on
   actor bodies.
3. **Varargs** last — the calling-convention work is larger and the payoff
   smaller than closures.

Not yet measured: what a closure-admitting executor is worth. The only speedup
figure we have is for leaf numeric loops (1.43-1.58x vs the interpreter, see
`bench/cross-runtime/aot-matrix-results.md`).
