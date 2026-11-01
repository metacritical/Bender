# AOT on proc code: zero kernels, and why

Measured on `bench/cross-runtime/wave.ben` at `60cfe06b`. Two separate
conclusions: AOT does not help the spawn path at all, and the native tier
cannot compile proc-using code at all. The second is the one that matters.

## AOT does not help spawn

Standalone binaries built with `bender --aot /tmp/waveaot.ben -o wave-aot` and
with `--native --no-auto-pgo`, min of 2, `BENDER_LANES=11`:

| N | interpreter us/spawn | AOT us/spawn |
|---|---:|---:|
| 100,000 | 8.04 | 8.23 |
| 200,000 | 7.11 | 7.11 |
| 500,000 | 8.76 | 8.69 |

Dead even, which is the expected result: the routed-spawn cost is C runtime --
closure marshal 1.089 us, self-pipe drain 3.22 us, channel lock/queue/post --
not bytecode dispatch, so compiling the Bender-level loop changes nothing. A
standalone binary changes startup, not the spawn path. Do not expect AOT to
move any number whose profile points into `src/core/`.

## The native tier emits ZERO kernels for this program

    native-skip 2 while types not proven: params [] ret nil
    native-skip 3  types not proven: params [] ret ?
    native-lib 0 kernels

Kernels are GENERATED, not drawn from a fixed library: `tools/callgraph.ben`
emits one C kernel per eligible program function ("one site, one kernel") and
`tools/loop-executor.ben` emits Tier-B loop executors per descriptor, both
driven by the program's own call graph at AOT time. So "add more kernels" is
not the lever.

The gate is not the problem either. `abi-return-types`
(`tools/callgraph.ben:309-313`) admits `:array :nil :bool :value :tuple
:string :keyword :symbol :table :struct :buffer :fiber :function :cfunction
:abstract :pointer` -- boxed values are perfectly emittable.

**The problem is that the inferencer has no knowledge of the standard
library.** `tools/infer.ben`'s `pure-calls` (`:152-165`) holds SEVEN entries:
six libm wrappers and two u64 casts. It is not a signature registry at all --
it is a LOWERING table, mapping a call to an exact C equivalent
(`math/floor -> floor`). Every other cfun, including the whole proc, ev and
channel API, infers `:unknown`, i.e. `?`.

So `wave.ben`'s spawn closure calls `proc/recv` and `proc/send`, its return
type is `?`, and it is refused. For a project describing itself as an AOT game
compiler this is the significant finding: any game loop that uses procs gets no
native compilation whatsoever.

### The fix is library signatures, not kernels

What is missing is a declaration WEAKER than `pure-calls`: not "lower this call
to a C function" but merely "this cfun returns a boxed `:value`" (or `:tuple`,
`:nil`, ...). That alone satisfies `abi-return-types` and makes callers
provable, with no lowering and no new codegen. It is additive, and it is the
cheapest route to native coverage for concurrent code.

Scope note before anyone starts: a signature asserts a CONTRACT. Declaring a
return type that the C implementation can violate (e.g. a cfun that returns nil
on one path and a tuple on another, declared as `:tuple`) would let the emitter
generate code on a false premise. Each declaration needs checking against the
C source, which is why this is a bounded audit rather than a bulk edit.

## A misattributed diagnostic (worth fixing first)

In `tools/callgraph.ben:3947-3954` the `cond` reports `"types not proven"` for
ANY `concrete = false`, including causes that are not type-related at all:
`dce-dead-ids` membership, a `:u64` parameter or return, a name shadowing a
declared extern, and duplicate site registration.

That is why the output above says `while types not proven: params [] ret nil`
when `:nil` is in fact an admissible return type -- that skip had some other
cause, hidden by the message. Anyone auditing native coverage is pointed at
types regardless of the real reason. Carry the actual reason into the skip
record before using these messages to drive work.
