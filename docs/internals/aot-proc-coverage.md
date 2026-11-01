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

**The real reason is ARITY, not types.** `tools/callgraph.ben` starts the
candidate flag at

    (var concrete (and (> (length pt) 0) (not (d :vararg))))

so a zero-arity function is refused before any type is examined. With the
diagnostic corrected (see below), `wave.ben` reports:

    native-skip 0 thunk  zero-arity: no ABI inputs ... : params [] ret nil
    native-skip 1        zero-arity: no ABI inputs ... : params [] ret ?
    native-skip 2 while  zero-arity: no ABI inputs ... : params [] ret nil
    native-skip 3        zero-arity: no ABI inputs ... : params [] ret ?

All four. Not one is a type failure. Confirmed directly: a probe with
`(defn with-param [x] ...)` and `(defn zero-arity [] ...)` emits exactly one
kernel, and the zero-arity function is refused even though its return type is
the fully proven `:long`.

And the precondition is *sound*, not an oversight. A kernel receives its
inputs through the C ABI. A zero-arity function has none to receive: its
inputs live in its captured environment, which that ABI cannot reach.

**This is structural for proc code.** `proc/spawn` takes a THUNK, so every
proc entry closure is zero-arity by construction. No proc body can ever be a
kernel candidate under this precondition, whatever the type signatures say.

### CORRECTION: library signatures are NOT the fix

An earlier version of this document concluded that the blocker was
`tools/infer.ben`'s missing standard-library signatures -- `pure-calls`
(`:152-165`) holds seven entries, six libm wrappers and two u64 casts, and is
a LOWERING table (`math/floor -> floor`) rather than a signature registry, so
every other cfun including the proc/ev/channel API infers `:unknown`. That
observation is true but it is NOT the operative cause: arity refuses these
functions first, and adding signatures would not have emitted a single extra
kernel for `wave.ben`.

Signatures remain mildly useful for functions that DO take parameters and call
cfuns, and that is now tracked as a low-priority item. The real question for
proc code is different, and open:

- can a proc entry thunk be made eligible at all -- e.g. by passing its
  captured environment as an explicit ABI parameter?
- or is the Tier-B loop-executor path (`tools/loop-executor.ben`), which
  compiles loop bodies rather than whole functions, the right tier for proc
  bodies instead of the kernel path?

Neither is answered yet. What IS settled is that this is an ABI-shape problem,
not a missing-metadata problem.

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

That is why the output above said `while types not proven: params [] ret nil`
when `:nil` is in fact an admissible return type -- the real cause was arity,
hidden by the message.

**FIXED.** Both emission paths (the kernel path and the lib path carried
identical copies of the gate) now thread a `why` string through a `refuse`
helper, first reason wins, and report it instead of the funnel message.
Reasons distinguished: zero-arity, vararg, provably dead, param type outside
the ABI set, `:u64` param, `:u64` return, return type not proven, and
extern-shadowing. Verified by the correction above -- the fix immediately
overturned this document's own prior conclusion, which is precisely what it
was for. Gates: suite-callgraph 256/256, suite-aot-multiseed 36/36,
suite-aot-quantum 5/5, suite-aot-release 14/14, suite-aot-size 13/13,
suite-dce-reachable 12/12, suite-dce-strip 61/61, suite-dce-harness 2/2,
suite-hinted-export 81/81, suite-autopgo 98/98, suite-native-dispatch 24/24,
suite-native-fiber 52/52, suite-native-joint-dispatch 28/28.
