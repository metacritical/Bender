# Controlled AOT/PGO matrix

Runtime built ONCE (`make NATIVE_EXECUTORS=1`, stamp `enabled`, 9 executor
symbols present in `build/bender`) and used both as the script interpreter and
as the compiler for every AOT arm. HEAD `b0b90b6c`.

This supersedes `pgo-results.md`, whose "release is 2.7x slower" figure came
from comparing a pre-existing `build/bender` against a release linked to a
freshly built static runtime — build configuration was confounded with the AOT
path. **That 2.7x regression does not reproduce under a controlled comparison.**

## Numeric workload (`/tmp/num.ben 400`, float mandelbrot loop)

All four arms printed `checksum=837095`. 3 repetitions, arm order alternated
per repetition, swap 5.6 GiB in use.

| arm | build | median | min | max | kernels |
|---|---|---|---|---|---|
| A | script interpreter | 50 ms | 50 | 60 | n/a |
| B | plain AOT image | 47 ms | 47 | 243 | — |
| C | native, `--no-auto-pgo` | 47 ms | 47 | 211 | **0** |
| D | `--release` after 3x `--pgo '400'` | 47 ms | 47 | 47 | **0** |

Spread is ~6% and the maxima (243/211 ms) are first-run page-in artifacts, so
**all four arms are equal within this box's noise**. PGO was trained at the same
`400` the measurement used, so this is not a profile-mismatch result.

## Why: loops are never emitted as kernels

Every arm reported `native-lib 0 kernels` with the same refusal:

```
native-skip 0 thunk      loop requires resumable VM quantum polls
native-skip 1 mandel-iter loop requires resumable VM quantum polls
```

The gate is `tools/callgraph.ben:5302-5317`. A definition is marked as looping
if **any** instruction is a backward branch:

```janet
(def loops (some (fn [ins]
                  (def op (ins 0))
                  (or (and (= op 'jmp) (<= (ins 1) 0))
                      (and (get emit-cond-jumps op) (<= (ins 2) 0))))
                (or (d :bytecode) @[])))
```

and a looping definition is unconditionally skipped at the kernel tier, because
a native kernel has no way to perform the VM's resumable quantum polls that keep
a loop preemptible. So **any function containing a loop is excluded from native
emission** — which is most hot code in a game.

Verified not to be a toggle: emission still refused with
`BENDER_NATIVE_EXECUTORS=1` and `JANET_NATIVE_EXECUTORS=1` set during the build,
and after three `--pgo` training runs plus a verified `--release`.

No frame-executor markers appeared in any build log for this workload, so the
separate loop-executor tier documented in `native-loop-executors.md` (which has
produced 76 frame executors / 9 kernels on another workload) did not engage
here. Why it did not is **not yet established** and is the next question.

## Actor workload refuses for a different reason

`bench/cross-runtime/wave.ben` also emits 0 kernels, but its census differs:

```
native-skip 0 thunk types not proven: params [] ret nil
native-skip 1       types not proven: params [] ret ?
native-skip 2 while types not proven: params [] ret nil
native-skip 3       types not proven: params [] ret ?
```

So there are **two independent blockers**: unproven types for actor bodies, and
the loop/quantum gate for numeric code. A controlled four-arm matrix for the
wave has not been run yet; the only wave AOT numbers we have are the discredited
confounded ones.

## What this matrix proves, and what it does not

**Proves.** With zero kernels emitted, AOT changes only packaging and startup,
and on this workload that is worth ~6% at best — inside noise. The earlier
"release 2.7x slower" claim is **not reproducible** controlled. `pgo parity ok`
appeared on every training run and every release build; it proves stdout
equality, never performance.

**Does not prove.** Nothing about AOT's potential once kernels do emit — that
case has not been measured, because no arm here emitted any. It also says
nothing about larger or longer-running workloads where startup amortizes
differently, and nothing about the wave under controlled conditions.

## Next experiments, in order

1. Find what makes the frame-executor tier engage, since it demonstrably has on
   another workload. That tier, not the kernel tier, is the one that could
   compile loops.
2. Write a kernel-eligible workload deliberately: a leaf function with no
   backward branch and proven numeric types, to measure what an emitted kernel
   is actually worth. Until that number exists, AOT upside is unquantified.
3. Decide whether the quantum-poll requirement can be satisfied inside emitted
   loop code (a poll at the back edge) or needs the native resumable
   continuation design in `native-continuation-design.md`.
