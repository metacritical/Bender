# The bytecode optimizer is OFF by default, and why

Investigation date: 2026-10-18. Prompted by asking whether the VM emits
unoptimized bytecode. It does — but not because the optimizer is weak.
It is because the optimizer is **disabled by default**, and it cannot
simply be enabled.

## 1. The default is 0

`to_optimization_level` (`src/core/compile.c:1331`) returns **0** when
`*optimize*` is not an integer, and `*optimize*`'s own docstring
(`src/boot/boot.janet:1573`) says *"Default is 0, and higher integer
levels will enable more optimization."*

Meanwhile `src/boot/boot.janet:10` does `(setdyn :optimize 1)` — so
**Janet's own core library is compiled at level 1 while all user code
compiles at level 0.**

Note the level is read from the *module's own env*
(`janet_table_get_keyword(c->env, "optimize")`), not from the caller's
dynamic bindings. `(setdyn :optimize 1)` before a `dofile` therefore
does nothing; the value must be in the env being compiled into, as a
RAW integer (`(put e :optimize 1)`), because a `@{:value 1}` metadata
wrapper fails `janet_checkint` and silently yields level 0. Both
mistakes cost real time during this investigation.

## 2. What level 0 leaves on the table

A representative hot loop, `(for i 0 n (set acc (+ acc (* (in a i) (in b i)))))`:

```
level 0 (13 instrs)            level 1 (12 instrs)
  lt    5, 3, 4                  lt    5, 3, 4
  jmpno 5, +8                    jmpno 5, +8
  movn  6, 3   <-- REDUNDANT     in    7, 0, 3
  in    7, 0, 6                  in    8, 1, 3
  in    8, 1, 6                  mul   9, 7, 8
```

`movn 6, 3` copies the loop counter into a second slot every iteration
and is pure overhead — the two `in`s can read slot 3 directly. This is
exactly the `move_near` **10.33%** in the opcode histogram (see
`5112fb01`).

Measured in-process, same workload, 200k iterations:

| level | loop instrs | time |
|------:|------------:|-----:|
| 0 | 12 | 0.245s |
| **1** | **11** | **0.231s (-5.7%)** |
| 2 | 11 | 0.232s |

**5.7% from removing one instruction from a loop body** — larger than
everything the immediate-fusion work in `6e17b064`/`6651d8d8`/`5f1bb86f`
achieved combined (~1.5% wall clock), for zero new code.

On real ttfx `src/scene.janet` (131 functions): level 1 gives 6061 ->
6016 static instructions and 548 -> 506 `movn`. Level 2 and 3 are
notably WORSE on static size (6844 instrs, 705 movn) and no faster than
level 1, so level 1 is the sweet spot.

## 3. Why it cannot just be switched on

Flipping the default to 1 was tried. Every suite passed —
suite-boot 1629/1629, marsh 73/73, asm 7/7, array 49/49, buffer 77/77,
table 15/15, string 69/69, capi 9/9, proc 99/99, ev 776/776,
aot-release 14/14, dce-strip 61/61, native-struct-shape 30/30 — **except
suite-callgraph, which failed "bitwise emit run"**, and the AOT kernel
emitter refused the kernel:

```
native-skip 1 k-xor8 get requires array and int index
```

Root cause, confirmed by disassembling
`(fn [r a b] (put r 0 (bxor (get a 0) (get b 0))) r)` with slot operands
(an earlier revision of this note blamed a backward scan for an adjacent
`ldi`; that was WRONG — the real cause is slot type joining):

```
level 0:  ldi 3,0 | get 4,1,3 | ldi 5,0 | get 3,2,5
level 1:  ldi 3,0 | get 4,1,3 |          get 3,2,3
```

Level 1 is CORRECT and better: it sees both indices are the constant 0,
drops the second `ldi`, and coalesces slots. But note instruction 2 at
level 1 — `get 3,2,3` — whose DESTINATION and INDEX are both slot 3.
Perfectly legal for the VM (operands are read before the destination is
written), but it means slot 3 now holds an integer index (from `ldi`) at
one program point and an array element (from `get`) at another.

The kernel emitter's gate is `infer/int-family?` on the index slot's
inferred type, and **inference in `tools/infer.janet` is SLOT-based, not
flow-sensitive**: one type per slot for the whole function. Two
differently-typed writes to a slot therefore JOIN to `:unknown`, the
int-family check fails, and the kernel is refused.

This is the SAME limitation already observed as
`native-skip ... slot s2 mixes array/long/number` on ttfx's `xor64` and
`or64` (3 refusals measured, see the skip-category audit). Level 1
simply makes slot reuse far more common, so the limitation goes from
affecting 3 functions to blocking kernels wholesale.

**So the optimizer is off because inference is slot-based while the
optimizer coalesces slots.** Enabling level 1 globally today would
silently lose native kernels — verified against ttfx.

The experiment was reverted (`src/core/compile.c` restored, 204/204 and
the ttfx kernel count confirmed back).

## 4. What would make this available

In rough order of cost:

1. **Make inference flow-sensitive.** Track a slot's type per program
   point instead of one joined type per function, so `get 3,2,3` reads
   slot 3 as int-family (its type at that instruction) and only then
   rebinds it to the element type. This is the real fix, unlocks ~5.7%
   on hot numeric code, and would ALSO fix the pre-existing
   `mixes array/long/number` refusals. It is a substantial change to
   `tools/infer.janet` — effectively per-instruction state or SSA — not
   a small patch.
2. **Per-unit opt-in.** Set `(put env :optimize 1)` for modules that
   contain no kernel candidates, leaving kernel-bearing modules at 0.
   Cheap, but splits the codebase into two compilation regimes and the
   win then misses exactly the hottest numeric files.
3. **Optimize after kernel selection.** Run callgraph's native
   selection against level-0 bytecode, then re-compile the boxed
   fallback bodies at level 1. More machinery, but both tiers get what
   they want.

Option 1 is the one worth doing: it is a bounded change to one backward
scan, it is testable against the existing `suite-callgraph` fixtures
(which already encode the shapes that must keep working), and it is the
only option where the optimizer's win lands on the numeric code that
needs it.

## 5. MEASURED on the real workload: worth ~0%

The 5.7% above is a BEST-CASE MICROBENCHMARK — one instruction removed
from the body of a 200,000-iteration loop. Before treating it as a real
win, it was tested end to end.

The level is read per MODULE (`janet_table_get_keyword(c->env,
"optimize")`), and an in-file `(setdyn :optimize 1)` does affect that
module's own compilation — verified: ttfx `src/scene.janet` goes
6061/548 -> 6014/504 (instructions / `movn`). Only
`src/rng-kernels.janet` carries `:hint` kernel candidates, so every
other ttfx module can take level 1 while that one stays at 0, which
sidesteps the slot-coalescing problem in §3 entirely.

Did that for `src/scene.janet` + `src/canvas.janet` (the two hottest
non-kernel modules) and measured:

- native kernels: still 4, unchanged
- byte-parity: matrix/decrypt/wipe/swarm frame dumps all identical
- wall clock, 4 alternating pairs: off mean 332.8 fps, on mean 332.5 --
  flat
- executed dispatches per frame (deterministic, histogram):
  **885,601 off vs 885,562 on — identical (-0.004%)**

(A first dispatch measurement suggested +1.88% WORSE, but the two runs
rendered different frame counts, 5309 vs 5477, so they were not
comparable; the repeat with matching 5302/5302 frames is the valid one.)

**Verdict: ~0% on the real workload.** The 44 removed instructions are
spread across 131 functions that matrix barely executes. The optimizer
genuinely works and genuinely helps tight numeric loops; ttfx's hot path
simply is not tight numeric loops — it is call-heavy code, which is the
same reason the frame-executor tier measured 0% three times (`17805b70`:
call + push are 13.15% of opcodes and can never be native).

So the recommendation in §4 stands as CORRECT BUT LOW VALUE for this
workload. Making inference flow-sensitive is still the right fix for the
pre-existing `mixes array/long/number` refusals, and would let level 1
be the global default — but it should not be justified by an expected
speedup on ttfx, because there isn't one. The ttfx experiment was
reverted.

## 6. Standing caution

Do not enable level 2 or 3. Measured larger static code (+13%
instructions on scene.janet) with no speed gain over level 1, on this
workload.
