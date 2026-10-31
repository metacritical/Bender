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

## CORRECTION: loops ARE emitted — by the frame-executor tier

The section below is accurate only about the **kernel** tier, and the matrix
above is wrong to conclude AOT cannot help. Re-measured with the frame-executor
tier requested at emission:

```
BENDER_NATIVE_EXECUTORS=1 ./build/bender --aot num.ben --native --no-auto-pgo -o e400
  native-skip 0 thunk       loop requires resumable VM quantum polls   <- kernel tier
  native-skip 1 mandel-iter loop requires resumable VM quantum polls   <- kernel tier
  native-executor 1 mandel-iter                                        <- EMITTED
  native-lib 0 kernels
```

and at run time: `native_init: available 1 frame executors`.

So the loop compiles. The kernel-tier refusal is real but is not the whole
picture, and "native-lib 0 kernels" does not mean nothing was emitted — the two
tiers are counted separately. I read only the kernel count and drew the wrong
conclusion.

Measured, 3-5 repetitions per cell, order alternated, all checksums agreeing:

| n | script | native (no exec) | native + executor |
|---|---|---|---|
| 400 | 51 ms | 48 ms (1.07x) | **33 ms (1.58x)** |
| 1000 | 144 ms | 127 ms (1.14x) | **98 ms (1.47x)** |
| 2000 | 483 ms | 411 ms (1.17x) | **339 ms (1.43x)** |

Packaging alone is worth ~1.1x; the loop executor roughly doubles that to
**~1.45x**, and the ratio holds as compute grows, so it is not a startup
artifact.

### Why it did not engage in the matrix above

The tier is **off by default and needs two opt-ins** (documented in
`native-loop-executors.md:1-8`): the runtime built with
`-DBENDER_NATIVE_EXECUTORS` (this build has it — stamp `enabled`, 9 symbols)
**and** `BENDER_NATIVE_EXECUTORS=1` set in the environment when running the
`--aot --native` driver. The matrix arms B/C/D set neither, so they measured the
interpreter's own loop path and the ~1.1x packaging win only.

That makes the default the actual problem, not the codegen: a user who runs
`--aot --native --release` with no environment variable gets no loop
compilation and no indication that a tier was available.

## Why the KERNEL tier refuses loops

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
