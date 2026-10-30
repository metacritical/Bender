# Why 71% of functions have unproven types, and a plan for unboxing

Investigation date: 2026-10-18, against ttfx `bin/ttfx-cli.ben`
(`native-lib 6 kernels`, 779 refused functions).

## 1. The measured distribution

From a real `--aot --native` build of ttfx:

| reason | count | share |
|---|------:|------:|
| loop requires resumable VM quantum polls | 228 | 29% |
| types not proven | 548 | 71% |
| **total refused** | **779** | |

Within the 548 unproven, by return type:

| return | count | note |
|--------|------:|------|
| `?` | 322 | return also unprovable |
| `nil` | 86 | **return already supported** (Phase 1) |
| `tuple` | 76 | **supported** |
| `bool` | 34 | **supported** |
| `function` | 9 | not representable |
| `double`/`long`/`string`/`array` | 21 | **supported** |

**226 of 548 already have a fully supported return type.** Their only
blocker is parameters.

## 2. Root cause: tables have no kernel representation

Verified three ways:

1. `emit-c-type-map` (`tools/callgraph.ben`) has **no `:table` or
   `:struct` entry**. The map covers numeric widths, `:array`,
   `:tuple`, `:pointer`, `:bool`, `:nil`, `:string`, `:keyword`,
   `:symbol`. Tables are absent.
2. `tools/infer.ben` **never mentions `:table`** (grep: zero hits).
   The lattice cannot even name the type.
3. The native gate accepts exactly:
   `numeric | :array | :tuple | :pointer | :string | :keyword | :symbol`.

Meanwhile ttfx is written in a **record-passing style**: nearly every
engine function takes a table as its first argument. A survey of
unproven functions shows first params named `ctx` (effect context),
`cv` (canvas), `st` (effect state), `rng`, `bt`, `fx` — e.g.

```bender
(defn canvas-char-count [cv] (length (cv :kept)))
(defn rng-random [rng] ...)
(defn backtracker-step [bt ctx] ...)
```

So the 71% is overwhelmingly **"takes a table record"**. This is not a
subtle inference weakness; the type simply does not exist in the
compiler's vocabulary.

## 3. A second, independent structural gap: zero-arg functions

The gate opens with:

```bender
(var concrete (and (> (length pt) 0) (not (d :vararg))))
```

`(> (length pt) 0)` **categorically excludes every zero-argument
function**, regardless of provability. 133 refusals are zero-param, of
which **44 have fully proven returns** (37 `nil`, 5 `string`,
2 `tuple`). These are refused by a structural precondition, not by any
analysis failure.

Caveat before acting on this: a zero-arg function has no parameters to
unbox, so the only available win is dispatch overhead — which the
executor-tier experiment (below) measured at ~0%. Low expected value;
cheap to try.

## 4. What the loop barrier is actually worth: nothing (measured)

The 228 loop refusals exist because preemption requires suspending at
loop backedges (`vm.c:1554`, `vm_maybe_auto_suspend(DS <= 0)` — the
poll fires only on backward jumps). A C loop cannot be suspended
mid-iteration, so the kernelizer refuses.

The new executor tier (`978400f1`) solves this by keeping `pc` and
slots as real VM state and polling via the exported
`bender_native_poll()`. Enabled it (`-DBENDER_NATIVE_EXECUTORS` +
`BENDER_NATIVE_EXECUTORS=1`) and measured, 3 alternating pairs per
effect:

| effect | baseline | executors | delta |
|--------|---------:|----------:|------:|
| decrypt | 2958/2962/2935 | 2993/2968/2989 | +1.0% |
| smoke | 1291/1293/1304 | 1272/1277/1320 | −0.3% |
| wipe | 3454/3485/3437 | 3460/3463/3513 | +0.2% |

**80 executors emitted, zero gain**, at +850 KB binary
(1.6 MB → 2.48 MB). Two reasons: (a) 65 of 80 warn *"executor target X
has no retained top-level defn; relying on lazy structural attachment
at runtime"* — the same dangling-kernel class as the Oct-8 xoshiro bug;
(b) by design the tier keeps **all slots boxed and GC-scanned**, so it
removes dispatch but not boxing.

**Conclusion: dispatch is not the bottleneck. Boxing is.** This is the
single most useful negative result available, and it should stop anyone
from pursuing dispatch-level work expecting a win.

## 5. Why unboxing tables is the real lever

A table field read is `bender_table_get` → `bender_dict_find`: hash the
key, probe the array, unwrap the result. Phase 5 (`cbdfffe6`) already
proved the alternative for *locally constructed* closed-shape tables:
flatten to a C struct and use the existing `:pread`/`:pwrite`
direct-memory path — field access becomes a fixed offset load.

The gap is that Phase 5 stops at function boundaries. It handles a
table built and consumed inside one function; it cannot handle a table
that **arrives as a parameter**, which is exactly ttfx's style.

## 6. Plan

Strictly sequenced; each step gated on the previous one measuring a
real win. Do not batch these.

### Step 0 (prerequisite): make the value measurable
Build a sustained single-process profiling fixture for the CLI engine
(the `--bench` path is ~300 ms, too short to sample; attempts to
profile it caught dyld startup). Without this, every later step is
unfalsifiable. **Do not skip — the executor tier shipped 80 functions
of machinery for 0% precisely because value was assumed, not measured.**

Deliverable: a profile of CLI decrypt/smoke attributing time across
`bender_dict_find` (table access), boxing/allocation, dispatch, and GC.
This single measurement decides whether Steps 1-3 are worth anything.

### Step 1: `:table` as an opaque handle (small, low risk, low ceiling)
Add `:table "BenderTable*"` to `emit-c-type-map`, accept `:table` in the
gate, unbox via `bender_unwrap_table`. Field access still calls
`bender_table_get` from C — **no hashing is eliminated**. Expected win:
small, possibly zero (same shape as the executor result).

Value: unblocks the ~226 functions whose returns are already proven, so
they stop being refused outright, and makes Step 2 incremental rather
than all-or-nothing. Gate: if Step 0's profile shows `bender_dict_find`
is not material, **stop here and reconsider the whole direction**.

### Step 2: closed-shape table *parameters* (the actual win)
Extend Phase 5's closed-shape analysis interprocedurally: for a
function taking a table param, examine **every** call site; if all
reaching construction sites agree on an identical closed key set and
per-key types, emit the kernel against a flat C struct and use
`:pread`/`:pwrite` offsets instead of hash lookups.

Hard parts, in order of difficulty:
- **Boundary cost.** If the table must be flattened on each call, the
  conversion replaces the saving. The shape has to *propagate*: a table
  created flat (Phase 5) and passed flat needs no conversion. Without
  propagation this step is pointless.
- **Whole-program agreement.** Any single disagreeing call site must
  force the boxed path for that function — the same monotone-widening
  discipline as Phases 2/3/5.
- **Escape.** A table reaching `pairs`/`keys`/`merge`/marshal must stay
  boxed. Reuse Piece 1b's `dce-reachable` disqualifying-pattern
  machinery, which already detects container escape.

### Step 3: app-side reshape (the already-identified fallback)
`bench-engines.md` calls this "the scene/ctx reshape is still the next
lever" — move hot loops onto numeric arrays, which are *already*
supported. Requires no compiler change and is proven to work, but is
invasive app surgery and must preserve byte-parity on 37 effects.

Prefer Step 2 if the analysis proves tractable, since it fixes the
problem for every program rather than one app. Step 3 is the pragmatic
fallback if Step 2's propagation requirement proves intractable.

## 7. Honest expectation setting

The CLI gap is **33-159× behind Rust** (`bench-engines.md` §2: decrypt
206,500 vs 1,290 fps). Six kernels, full PGO, and 80 executors have not
moved it. Unboxing table access is the largest single identified lever,
but nobody should expect it to close two orders of magnitude on its
own — Bender values are NaN-boxed throughout, allocation is pervasive,
and the GC is global. Unboxing is necessary, not sufficient.

Related constraint worth stating plainly: **the million-process goal
and the CLI-speed goal pull in opposite directions.** Preemption
requires suspendable-at-backedge loops (hence boxed, VM-resident
state); speed requires unboxed values in registers and real C loops.
The current architecture can have either per function, not both. Any
plan that claims both without naming which guarantee it relaxes is
wrong.
