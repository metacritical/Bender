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

## 7. Resolution (2026-10-19): emitter fixed, default flipped to 1

The §3 blocker is fixed and the default is now 1.

The real defect was in the EMITTER, not in infer: emit-fn-c updates
its slot-type state BEFORE the emission gate runs, and level 1 makes
`get` reuse its index temp as the destination (`get 3 2 3`), so the
gate read the get's own :number RESULT where the :long index had been
and refused. Fix (efa04d58): a pre-update snapshot of the get's
operand types (get-now), the same pattern as the existing sh-now /
ta-now snapshots for call-shaped access, which were added for exactly
this dest-reuses-operand aliasing. The gate now also accepts :number
indices with the existing Step 17 integrality guard, completing the
state rule (put and infer already accepted them; the guard's :number
arm was dead).

With that, `to_optimization_level`'s absent-value default is 1
(matching boot.janet:10's own core compilation), the *optimize*
docstring is updated, and suite-unknown's symbolmap fixture pins its
level explicitly (level 1 legitimately constant-folds dead defs and
prunes their symbolmap entries — a coherent, smaller map — so that
fixture must not depend on the process default).

Verification, all with default 1:

- suite-callgraph **206/206** (204 prior + 2 new fixture tests that
  pin level 1 in-file via `(setdyn :optimize 1)` and assert the
  aliased get shape still emits and passes native parity; they fail
  with "k-xor8o get requires array and int index" without the fix).
- Full battery: boot 1629/1629, marsh 73/73, asm 7/7, array 49/49,
  buffer 77/77, table 15/15, string 69/69, capi 9/9, ev 776/776,
  pool 56/56, aot-release 14/14, dce-strip 61/61,
  native-struct-shape 30/30, native-executor matrix pass, and a full
  sweep of every `test/suite*.janet` green except:
  - suite-proc 98/99 ("both land") — PRE-EXISTING at HEAD f9780afd
    with level 0, with and without the uncommitted gc.c edit; not
    caused by the optimizer.
  - suite-aot-profile-env fails ONLY under `JANET_NO_PROFILE=1`
    (it asserts an auto-PGO dump exists; 16/16 without the variable).
- ttfx, identical PGO protocol both sides (two decrypt workloads,
  seeds 42 and 7, bounded 12 frames, then `--release`, private
  caches): native-names IDENTICAL at level 1 and level 0 — 4 kernels
  each (`plain-length`, `k-next-word!`, `k-next-pack!`,
  `k-next-scale!`). **No kernel loss.** (The older verified binary
  shows 6 because the app tree has since changed: `floor-div` now
  refuses on a duplicate definition in `src/canvas.janet` — the same
  refusal at level 0 — and `round-half-even`/`out-bounce` classify
  ambiguous — also identical at level 0.)
- Byte parity: all 37 effects, FULL dumps, seed 42,
  `ruby-port/logo.txt`: level-1 build == level-0 build == previous
  verified binary, byte-identical (e.g. decrypt dump SHA-256
  `47fe71b8b53ed5c2643c66cc5aaa99d4342e925ec3b2c0106ff6f89d7c93266d`
  at both levels).
- Paired 3-round `--bench` (both full PGO+release builds, alternating
  run order): beams +1.21%, decrypt +1.13%, fireworks +0.55%,
  matrix −1.36%, smoke −1.47%, vhstape +0.46%, wipe −0.02%. All
  within round-to-round spread except matrix, which over 8 paired
  rounds is reproducibly ~1.1% SLOWER. Verdict: **end-to-end ~0%**,
  exactly as §5 measured — the optimizer demonstrably applies
  (scene.janet static 5876→5809 instructions, 543→485 `movn`) but
  ttfx's hot path is call-heavy, not `movn`-bound.

So the default flip stands on: consistency with the core's own
compilation level, the free static-size win, and the now-removed
aliasing trap — NOT on a ttfx speedup, because there is none (and
matrix gives ~1% back, reproducibly). §6's caution against levels 2
and 3 is unchanged.

---

# Appendix: the ttfx GUI path (2026-10-18)

Profiled the GUI because the CLI engine gap is 33-159x while the GUI
collapses to ~3.5x. Three findings, two of them negative, all measured.

## A. The GUI is raylib-draw bound — confirmed, with a number

The GUI PGO profile's top TWO entries were not ttfx code: two anonymous
`boot.janet:4768` thunks at 1,238,657 calls each, **40.6% of all calls**.
That is `ffi/defbind-alias`'s lazy branch re-forcing a memoized pointer
and signature on every FFI call:

```
lazy  -> ldc call ldc call push3 ldc tcall
eager -> ldc      ldc      push3 ldc tcall
```

ttfx set `:lazy true` but only `bin/ttfx-gui.janet` imports `gui.janet`,
and that entry requires raylib anyway, so lazy bought nothing. Dropping
it (ttfx commit `4b8edc3`) gave, same workload:

| mode | total calls | draws | lazy thunks | calls/draw |
|------|------------:|------:|------------:|-----------:|
| lazy | 5,962,634 | 1,161,656 | 2,400,610 | 5.13 |
| eager | 3,494,034 | 1,096,642 | **0** | **3.19** |

37.9% fewer Janet calls per draw, and **fps UNCHANGED** (eager mean
1653.6 vs lazy 1671.6 across 4 probes, inside a +/-10% band).

**Removing 2.4 MILLION Janet calls per 3-second probe moved nothing.**
That is the cleanest possible proof the GUI is not Janet-bound. Kept
anyway: it removes pointless work and unmasks the profile, where those
thunks were crowding out real application work.

## B. Draw-call batching: 2-3.6x fewer draws, and 2x SLOWER

ttfx issues one `DrawTextEx` per visible cell. Measured the batching
opportunity (maximal runs of horizontally adjacent same-colour cells):

| effect | cells | runs | reduction |
|--------|------:|-----:|----------:|
| wipe | 401 | 111 | 3.61x |
| beams | 382 | 192 | 1.99x |
| smoke | 439 | 230 | 1.91x |
| decrypt | 186 | 147 | 1.27x |

Largest exactly on the block-heavy effects where this port loses worst.
Implemented it, and it was **~2x slower**:

| effect | per-cell | batched | |
|--------|---------:|--------:|---|
| wipe | 1054.6 | 489.2 | -54% |
| beams | 995.6 | 396.7 | -60% |
| smoke | 812.6 | 390.5 | -52% |
| decrypt | 1556.2 | 765.1 | -51% |
| fireworks | 2039.4 | 1705.5 | -16% |

Cause: `cells` arrives in character-creation order (it is built by
walking `(ctx :chars)`, whose entries move independently), so batching
requires a per-frame sort. That sort — a Janet comparator closure over
~400 tuples, plus building the intermediate array and per-run buffers —
costs far more than the saved FFI draw calls.

**The correct lesson, which contradicts the naive reading of finding A:**
raylib-bound means *removing* Janet work does not help. It does NOT mean
Janet work is free. Adding Janet work hurts immediately and heavily.

Reverted. Any real version of this must produce cells in row-major order
at the SOURCE (in `ctx-frame-cells`, or by maintaining a row-indexed
grid) so no per-frame sort is needed — not by sorting in the draw path.

### Two correctness traps found while doing it (keep these)

1. **Glyph advance is fractional.** `cell-w` is `floor(advance)`: measured
   21.6 vs cell-w 21. A multi-char `DrawTextEx` therefore drifts 0.6px
   per character — over half a cell after 20 chars. Passing
   `spacing = cell-w - advance` fixes it exactly.
2. **Draw order is load-bearing.** `(ctx :chars)` can hold several
   characters on the SAME (row,col) — overlapping particles — and the
   per-cell path draws them in creation order, so the last created wins.
   Any reorder changes which is visible. Colour-only batching, then
   same-symbol batching, then a stable sort with a creation-order
   tiebreaker ALL still rendered decrypt and smoke differently from the
   per-cell path (wipe and beams matched throughout, having no overlaps),
   so this was not fully solved even with the tiebreaker.

### The pixel gate that caught it

`--probe-shot FILE` (relative path; raylib prepends CWD) is deterministic
across runs at a fixed `--probe-seconds` — verified by hashing two
identical runs. That makes screenshot hashing a usable automated gate for
GUI rendering changes, which the test suite otherwise lacks entirely
(`--dump-frames` tests the CLI ANSI path, so GUI regressions are
invisible to it). Use it for any future GUI draw change.

## C. Remaining GUI lever

The js port draws block cells as 1-3 filled rects instead of glyphs, and
`bench-engines.md` already names a `--blocks=rects` mode as unimplemented.
That is still the plausible win for block-heavy effects, and unlike
batching it needs no sort — a rect draw is per-cell, same order, just a
cheaper call. Untested here.

## D. Why GUI draw-call batching cannot work: the GUI is PER-CELL bound

Retested batching properly — release binary, 3 PGO passes per side, as
the interpreter result was rightly distrusted. Rewrote the
implementation first to remove every objection to the original attempt:

- zero per-frame allocation (persistent grid, reused buffer and
  touched-list in the gui table) instead of ~700 allocations/frame
- NO per-frame sort: a dense row-major grid gives ordered traversal free
- overlap order preserved by last-write-wins into the grid

Both sides: 3x `--pgo` then `--release`, "pgo parity ok", 5 kernels,
1,695,424-byte binaries.

| effect | baseline | batched | delta |
|--------|---------:|--------:|------:|
| wipe | 861.7 | 853.9 | -0.9% |
| beams | 912.7 | 790.1 | -13.4% |
| smoke | 778.2 | 713.0 | -8.4% |
| decrypt | 1540.6 | 1104.3 | -28.3% |
| fireworks | 2274.0 | 2132.5 | -6.2% |

Still slower everywhere, though much less than the interpreter's ~-50%
— so PGO did recover most of the overhead, just not enough.

**The reason, and it closes the question.** Multiply cells drawn per
frame by baseline fps:

| effect | cells | fps | cells x fps |
|--------|------:|----:|------------:|
| beams | 382 | 912.7 | 348,651 |
| wipe | 401 | 861.7 | 345,542 |
| smoke | 439 | 778.2 | 341,630 |

**Constant at 345,274 +/- 2.0%** across three effects with different cell
counts AND different batching ratios (1.91x-3.61x). That is the
signature of being bound by PER-CELL work, not by call count. Frame time
is proportional to the number of glyphs rasterized, and batching draws
exactly the same glyphs in fewer calls — so there is nothing to save,
and the bookkeeping is pure loss.

decrypt sits lower (286,552) despite drawing text rather than solid
blocks, consistent with varied glyphs costing more per cell than one
repeated block glyph (atlas locality), and it is hurt worst by batching
(-28.3%) because it has both the lowest occupancy (186 of 810 grid
slots) and the shortest runs (1.27x) — so it pays the full fixed cost of
scanning the grid for almost no batching benefit.

This also explains finding A: removing 2.4M FFI lazy-thunk calls changed
nothing because the FFI boundary is not the cost either. Per-cell GPU
fill and glyph sampling are.

### Consequence: the only real GUI lever is cheaper or fewer CELLS

Reducing draw CALLS is proven worthless. What remains:

1. **`--blocks=rects`** (named as unimplemented in bench-engines.md, and
   what the js port does): draw block cells as 1-3 filled rectangles
   instead of anti-aliased textured glyphs. This attacks per-cell cost
   directly — a solid rect needs no atlas sample — and needs no sort, no
   grid, and no reordering. This is the one worth trying.
2. Draw fewer cells (skip occluded//unchanged cells; a dirty-rect scheme).

### Also recorded: a correctness result and a gate limitation

Batching was pixel-IDENTICAL on wipe but genuinely DIFFERENT on decrypt,
even with last-write-wins ordering — overlapping anti-aliased glyphs
blend when drawn individually, and keeping only the last occupant loses
that blend.

And the screenshot gate is only valid for SOME effects: baseline hashes
for smoke and beams changed between two identical baseline runs
(5aefa0a6 -> 8f71c2d1, 25b3a428 -> bdbb16bd) while decrypt and wipe were
stable across runs. So `--probe-shot` is a usable gate for decrypt and
wipe only; smoke/beams animation state is not reproducible at a fixed
probe duration.

## E. `--blocks=rects` also measured NULL -- the GUI is PIXEL-FILL bound

Implemented the one remaining GUI lever from section D: bind
`DrawRectangle` and draw block-element glyphs as filled rectangles
instead of font glyphs, behind an opt-in `--blocks=rects` flag. This is
what the js port does, and `bench-engines.md` named it as unimplemented.

First measured the glyph mix to confirm the opportunity was real (new
`TTFX_SYM_PROBE` instrumentation, one frame of each effect):

| effect | full block `U+2588` | other blocks | rect-replaceable |
|--------|--------------------:|-------------|-----------------:|
| wipe | 337 of 401 (84%) | half-blocks 64 | **100%** |
| beams | 208 of 382 (54%) | eighths/halves 111 | ~84% |
| smoke | 279 of 464 (60%) | halves 54 (shades 131 cannot) | ~72% |

Shade blocks (U+2591-2593) are deliberately excluded: they are DITHERED
patterns, not solid, so a flat rect would render them wrongly.

Release binary, 3 PGO passes, `--show-fps --uncapped`, 3s probe. First
round looked promising -- wipe +5.0%, beams +2.3%. Repeats did not hold:

| effect | glyphs (4 runs, mean) | rects (4 runs, mean) | verdict |
|--------|----------------------:|---------------------:|---------|
| wipe | 1038.3 | 1069.3 | +3.0%, ranges overlap |
| beams | 977.9 | 939.4 | **-3.9%, SIGN FLIPPED** |

No reliable win. The first round was noise, and beams reversed direction
between rounds.

**The reason, obvious in hindsight and the final piece of the GUI
picture:** `U+2588` FILLS THE ENTIRE CELL with solid colour. A rect over
that same cell fills exactly the same pixels in the same colour. The GPU
does identical fill work either way; the only thing saved is one texture
atlas sample, which is negligible against the fill. wipe is 84%
full-blocks, so there was essentially nothing to save -- and effects with
few blocks (fireworks -4.0%) pay a per-cell table lookup for nothing.

### The GUI is now fully characterised, by elimination

| hypothesis | test | result |
|------------|------|--------|
| Janet-call bound | removed 2.4M FFI lazy thunks (-37.9% calls/draw) | **0%** |
| draw-call bound | run batching (1.9-3.6x fewer calls) | **slower** |
| atlas-sampling bound | rects instead of glyphs | **0%** |
| **per-cell pixel-fill bound** | cells x fps constant at 345,274 +/- 2.0% | **confirmed** |

Frame time is proportional to cells rasterized, and every strategy
tested rasterizes the same pixels. There is no Janet-side or
draw-strategy win available in this GUI. Any real improvement must
reduce the NUMBER of cells filled (dirty-rect / occlusion culling, i.e.
skip cells unchanged since the last frame) or change resolution.

Reverted; ttfx tree clean. The `TTFX_SYM_PROBE` approach is worth
re-adding temporarily if anyone needs a glyph-mix census again.
