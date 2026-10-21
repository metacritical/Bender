# Profile-guided AOT optimizer: execution plan

Updated 2026-10-21. This is a concrete continuation of
[`ttfx-driven-compiler-work.md`](ttfx-driven-compiler-work.md), not a promise
that PGO by itself makes boxed bytecode run at native speed.

## Goal and current evidence

The long-run target is to meet or exceed Rust on **comparable, parity-gated**
workloads. The five-effect release GUI comparison is Bender/Rust **0.865×**;
the same-scene CLI frame-dump throughput is about **0.072×** Rust and
**0.48×** LuaJIT. CLI includes construction, formatting and startup; GUI
includes a shared raylib/OpenGL presentation floor. These are different
optimization targets and must not be combined into one ratio.

Single-frame to full-animation release CLI probes were, respectively:
`wipe` 16→49 ms, `randomsequence` 14→68 ms, `scattered` 18→96 ms.
The latter additional time remains in effect stepping and frame-string
construction, not just stdout writing. In the generated release, nine
scalar helpers are native; `ctx-frame-string` and `ctx-update` remain
mixed-operation VM loops. The first expanded frame-executor prototype
generated 76 descriptors but lost 5.2% CLI throughput in a paired A/B;
more importantly its leaf-alias shortcut was GC-unsafe. Its timings are
historical, **not a gate for the corrected code**. See
[`native-loop-executors.md`](native-loop-executors.md).

The corrected default-off dispatch gate recovered **6.7%** of the
macro-enabled runtime's CLI throughput when no descriptors were present.
The safe executor candidate itself was **0.989×** its same-compiler
no-executor release across five paired effects and **0.927×** the prior
macro-free `rel2` in a separate paired sitting. It is a correctness
foundation that remains opt-in, not an achieved optimization win.
The default macro-free runtime and CLI were rebuilt from the feature
commits: two isolated PGO collections, verified `--release`, 37/37
bounded frame streams at seed 42 and 25/25 held-out effect/seed cases
match `rel2`; seven paired CLI rounds gave **1.006×** `rel2` (noise).
Windowed GUI PGO collection initially crashed after the hidden-window
stage when launched directly on an idle display, also with an old
`rel2` control. A long-lived `caffeinate -d -u` before collection
allowed the **real** scattered GUI workload to collect twice and
verify `--release`. Do not substitute a non-GUI profile or benchmark a
non-release candidate as the GUI result.

## Invariants for every stage

- No ttfx-specific lowering or change to ttfx source. Scratch experiments
  live outside both repositories; code-generation changes must be generic.
- Exact full-frame bytes on all 37 effects, multiple seeds with held-out
  seeds; comparable GUI appearance, dimension, tick cadence and draw count.
- Original GC roots, write barriers, exceptions, debugger PCs, marshal and
  quantum/interruption order. A guard must deopt **before** unexecuted
  effects, or **after** a completed effect at the following original PC.
- No JIT/downloaded code on iOS: profile-guided versions are generated
  before packaging and linked into the app with bundled bytecode.
- Benchmark the verified `--release` binary after **two isolated PGO
  collections**, not an instrumentation/telemetry build. Interleave
  same-sitting pairs, retain all raw samples, and reject noisy wins.

## Work units, in dependency order

### 0. Attribute the release workload (measurement prerequisite)

Create a sustained single-process AOT/PGO probe in the approved scratch
root, reusing the unmodified scene modules and input. Attribute sampled
time and allocations to VM dispatch, `janet_dict_find`/container access,
core calls, string-buffer construction, GC, and native code. Report
dynamic executor entries/backedges/**effectful exits**, not just emitted
function counts. Measure startup/construction independently from playback.
Do not choose a data-layout rewrite until its expected share is measured.

**First measured slice (scattered, 2026-10-21):** A scratch driver outside
both repos imports the unchanged effect modules, runs 100 full animations
in one process, and is itself PGO-collected twice and verified `--release`.
Its 4-second main-thread sample had 3,380 stack tops: `run_vm` 1,383
(~41%), `janet_gc_barrier` 283 (~8%), `janet_dict_find` 185 (~5%),
`janet_in` 169 (~5%), and libc environment lookup 96 (~3%). Samples
are an attribution guide, not independent addends or a VM-only timing.
The lookup came from repeated diagnostic `getenv("JANET_TRACE_STRICT")`
checks on slab reuse and strict write barriers, not enforcement of
strict-heap isolation.

The first generic VM fix (`5e3f2740`) caches that diagnostic flag at
`janet_init` and refreshes it on `os/setenv`; C reinitialization tests
pin both modes. GC/proc/event and the enabled executor/ASan matrix pass.
Re-sampling removes environment lookup from the hot-stack list. In six
same-sitting paired sustained-scattered release runs it is **1.033×**
faster, with every pair positive. Wipe was noisy/neutral and
randomsequence about 1.01×; a seven-round five-effect **whole-process**
release CLI sweep was **1.001×** (noise). Full frame parity is 37/37 at
seed 42 and 25/25 held-out effect/seed cases. This is a small measured
workload win, **not** Rust-class whole-program throughput. Profile other
effects before ranking table layout against dispatch/GC work.
With the same compiler and same real-GUI PGO workload on both sides,
five paired windowed rounds yielded **1.005×** cached/pre-cache GUI
FPS on the five effects (noise). A separate comparison against the
older `rel2` GUI artifact was confounded by other compiler/PGO changes;
it cannot be attributed to this diagnostic-flag cache.

### 1. Sound effectful safepoints (first feature, default-off)

Finish the guarded native `geti`, `next`, primitive equality, fiber-stack
push and scratch-array reuse. For an installed FFI alias, use the same
GC-scanned C frame as `JOP_CALL`, store its return in the live frame
before the GC check, then exit at the next original PC and invalidate the
structural-validation epoch. Non-alias calls fall back at the unexecuted
call PC without double polling. Allowlist every executed opcode in the
runtime verifier. Test with a genuine struct-by-value alias that allocates
a tuple return under GC pressure, both boxed and executor images, the
budget-1/2 native matrix, ASan/UBSan and event-loop fibers. **Passing is a
correctness milestone, not a speed claim**. Keep double opt-in until a
positive release A/B survives the broader sweep.

### 2. Collect useful feedback for an optimizing IR

Extend existing source-qualified PGO evidence with bounded per-PC receiver
types/shapes, observed call targets, branch/edge counts and allocation
sites. Merge only compatible workloads; guard every observed type at
runtime. Preserve the existing source/replay parity gate and test held-out
inputs to prevent profile-dependent answers. Feedback must enable a
specialization decision, not merely count how often a wrapper ran.

**First feedback slice (`74c687fb`, 2026-10-21):** With VM profiling active,
small definitions (at most 256 bytecode words) collect bounded per-original-PC
fallthrough/taken successor counts for the five jump opcodes. Unconditional
jumps count only the taken successor. Backedge counts increment **after** the
original interrupt poll, so an interrupted edge is not reported as executed.
There is no collection or allocation inside the branch hook; the profiler
entry owns a GC-scanned counter array, and profiling-off builds retain only a
predictable inactive check. Dump and accumulating merge preserve compatible
counter arrays; absent or incompatible history becomes `:open`, not invented
zero-count evidence. The callgraph loader validates the optional field but
**does not consume it to specialize code**. No shape, receiver, call-target or
allocation-site feedback exists yet, and no runtime performance claim follows
from this diagnostic slice.

Narrow checks: rebuilt interpreter profile 57/57, merge 55/55, native joint
dispatch 28/28, source-qualified callgraph and GC trace pass. Representative
CLI value/math/array/table/I/O/interrupt suites pass 273/273. The native
executor suite still stops at its AOT image's `native executor registration
refused` and zero entry count, before the budget-1/2 and sanitizer lanes; the
image links the ordinary macro-free `build/libjanet.a` unless an executor-enabled
library is built, so this is **not** a passing executor safety gate or a
verified regression attribution. A standalone `bench/run.janet` native image
runs and preserves numeric results, but its single noisy microbench timings
are not a paired release speed measurement. The CLI frame-stream, held-out
seed and paired same-compiler GUI/CLI release gates remain pending for this
slice. Executor stays opt-in.

**Measured feedback continuation (`1acdc279`, 2026-10-21):** An unchanged
`572c8bcf` compiler and the branch-only compiler each built two independent
CLI PGO runs (`wipe` and `scattered`, seed 42, bounded frame streams) and
verified `--release` images; 37/37 bounded effect streams and 25/25 held-out
seed/effect streams matched byte-for-byte. Seven interleaved full-animation
CLI rounds were noisy and did not establish a win. Ten paired short CLI rounds
had per-effect median control/candidate throughput ratios
`1.0124/0.9940/1.0054/0.9971/0.9982` (wipe/highlight/expand/randomsequence/
scattered), geometric mean **1.0014×**, effectively neutral. Five paired real
GUI windowed probes after two GUI PGO runs and verified releases had
candidate/control effect-median geometric mean **0.9915×**; individual scenes
varied in sign. These are within sitting-to-sitting display and startup noise,
not Rust parity progress. The first ordinary executor suite failure was a
macro-free AOT library, not a demonstrated VM failure; an independently
compiled executor-enabled amalgamated harness passed budget-1 and budget-2
ASan/UBSan gates (with the existing alternate-stack ASan warning).

The next slice instruments **per-PC receiver type masks** for VM `in`,
`in-immediate`, `get` and `get-index` on definitions up to 256 instructions.
Types are recorded before the potentially effectful read; the hook allocates
nothing, changes no PC, holds no unscanned pointer, and remains inactive with
profiling off. Dump decodes type tags, cumulative merge unions compatible PC
vectors and opens missing history, and the callgraph loader validates but does
not yet use the evidence. Focused profile 62/62, merge 57/57, native joint
28/28, source-qualified callgraph, auto-PGO/cache migration, GC tracing, and
receiver-heavy CLI suites pass. Two fresh CLI PGO runs, verified `--release`,
37/37 bounded effect streams and 25/25 held-out seed/effect cases match the
unchanged control. Seven interleaved **full-animation** CLI rounds gave
per-effect median geometric mean control/candidate **1.0026×**; five real GUI
windowed paired rounds after two GUI PGO runs and verified releases gave
candidate/control **0.9952×**, with sign-changing per-effect noise. This is
feedback coverage, **not a speedup**. The separately compiled executor-enabled
budget-2 ASan/UBSan/quantum harness passes with the known alternate-stack
warning. Native record unboxing, effectful IR, roots, deopt snapshots and
startup specialization remain unimplemented; the executor stays opt-in.

### 2026-10-21 compiler-to-Rust CLI gap diagnosis

The seven-round, five-effect **complete bounded ANSI frame stream** sitting
(seed 42, `ruby-port/logo.txt`, output discarded, whole process included)
passed byte-for-byte parity across current verified-release Bender, Rust,
Lua 5.5, LuaJIT and precompiled Chez. Raw records:
`/tmp/janet-receiver-cli-language-fps.json` (ephemeral local artifact).
Median effective frames/s were:

| Effect | Frames | Bender | Rust | Lua 5.5 | LuaJIT | Chez compiled |
|---|---:|---:|---:|---:|---:|---:|
| wipe | 138 | 2,656 | 28,695 | 2,449 | 5,260 | 2,156 |
| highlight | 129 | 2,464 | 28,175 | 2,075 | 4,811 | 1,833 |
| expand | 117 | 1,337 | 21,713 | 1,580 | 3,333 | 1,674 |
| randomsequence | 261 | 3,662 | 53,732 | 3,075 | 7,966 | 3,477 |
| scattered | 181 | 1,806 | 31,833 | 1,810 | 3,758 | 2,195 |

Bender's across-effect geomean throughput is **0.072× Rust, 0.470× LuaJIT,
1.053× Lua 5.5 and 1.028× compiled Chez**. Those are workload-specific
whole-process ratios, not isolated compiler efficiency or GUI FPS. The
release callgraph emits **nine scalar wrappers and zero frame executors**;
`ctx-update` (def 47) and `ctx-frame-string` (def 58) explicitly skip with
`loop requires resumable VM quantum polls`, and neither name appears in
`native.c`. Its 813 logged skips include 169 loop-quantum refusals, 436
unproven-type refusals and 180 duplicate registrations (not independent
functions). PGO seeds both hot functions' parameters but cannot turn the
313-instruction `ctx-frame-string` into native code; its 43 calls, six writes,
six lengths and three allocations cross exactly the effects that require
roots, guards, polling and precise fallback. The <=256-PC receiver collector
cannot even cover this 313-instruction function yet. Do not infer that
activating the opt-in frame executor solves it: its prior corrected paired
candidate did not win and exits around effectful operations.

A controlled one-frame versus full-frame dump at the same input previously
measured Bender **16.0→49.4 ms** (`wipe`), **14.3→67.9 ms**
(`randomsequence`), **17.9→96.0 ms** (`scattered`); Rust measured **4.1→4.4,
3.8→4.3, 4.1→5.4 ms** respectively (see native-loop-executors.md).
A new seven-repeat, output-discarded sitting measured Bender `--bench`
whole process **50.02/68.75/95.68 ms** for those effects versus full dumps
**51.05/70.59/96.68 ms**. Thus stdout writing is not the main difference;
startup/effect construction and VM work both matter. One-frame repeats in
that sitting were **17.73/13.80/18.50 ms**, but the separate quick ablation
sitting was noisy (one wipe sample at 45 ms); these estimates are not a
subtractive causal profile. A sustained unrelated `nbody` VM sample showed
container access (`janet_in`), `janet_put` and GC barriers on its hot stack;
it does not quantify ttfx scene costs. A direct ttfx `sample` request found
no stacks because the ~100 ms process ended before sampling began. Avoid
inventing a main-thread ttfx percentage from those incomplete samples.

Ranked safe next steps: (1) obtain **actual ttfx hot-stack and per-function
counts** by replaying many full effects in a single process or adding a
nonsemantic sampling harness, separating startup from playback; (2) grow a
bounded, source/bytecode-qualified receiver/edge feedback representation
that covers the 313-PC renderer without profile-dependent behavior;
(3) implement one guarded, effect-aware loop region with original-PC deopt
snapshots, GC-scanned boxed roots, exact VM call/backedge polls, validated
array/table reads and post-effect revalidation; (4) only then propagate
closed record shapes and scalar-replace proven nonescaping temporaries,
and independently address image startup. Fail closed on aliasing, callback,
mutation or type changes; never replay an already committed write/call.
Require executor-enabled sanitizer/preemption, all-effect and held-out
frame parity, and two-PGO paired verified-release CLI+GUI A/B before any
performance claim. None of these steps is yet proven to reach Rust parity.

**Bounded verified executor increment (`c748bb45`):** The opt-in
whole-function frame executor now executes the VM `get` opcode directly for
array/tuple receivers with integral int32 keys, including invalid/out-of-range
keys returning nil. Any other receiver exits **before** the original PC, so
table/struct/abstract getters and their callbacks remain VM-owned. The live
fiber slot holds each receiver and key (GC-scanned); this fast path allocates
nothing, and existing backedge/call VM polls and structural revalidation remain
unchanged. It is not a scalar-replaced record or an effectful-call region.
A generic register-key loop fixture asserts bytecode `get` was emitted, runs
array, tuple and table receivers (20 result across each), and matches a boxed
image while an executor-enabled archive reports four attached functions and
1,299 VM boundary exits. Exact-PC mixed differential at quantum budget 1
passed four interrupted workloads plus parked slot/PC comparisons. Budget-2
ASan/UBSan executor and mixed-effects harnesses passed, with the known macOS
alternate-stack ASan caveat. The default `test/suite-native-executor.sh`
**still fails at its image step** when it links the ordinary macro-free
`build/libjanet.a`; an isolated executor-enabled archive and image passes.
This is an opt-in semantic slice, not a ttfx speed claim. The 313-PC ttfx
renderer still fails executor eligibility on other operations; independent
verified-release parity/performance gates remain pending for any larger
lowering.

### 3. Lower effectful loops as a single native region

Build an SSA-like representation with typed values, effects, liveness and
per-PC deopt snapshots. Lower common core calls, table/array reads and
bounded writes with type/shape/arity guards. Calls that may allocate need
scanned roots, GC checks and revalidation; unknown callbacks deopt. Poll
at exactly the VM's backedges and calls. Never rerun a completed write or
call on fallback. First gate on tiny generic loop fixtures, then measure
the unchanged `ctx-frame-string` and `ctx-update` paths. A function that
still exits once per cell has not met this milestone.

### 4. Remove boxed record and temporary traffic

Only after Step 0 measures it material: propagate proven closed table
shapes across function boundaries (see [`unboxing-plan.md`](unboxing-plan.md)),
scalar-replace nonescaping tuples/records and specialize append buffers.
Unknown shape, reflection, marshal or escape must stay boxed. Conversion
on every call can cost more than the saved hash lookup, so require a
measured reduction in total allocations and time, not just faster loads.

### 5. Lower startup cost separately

Minimize image initialization, deserialization and module setup without
special-casing effect selection. Even zero-time playback cannot make a
15–18 ms first frame match Rust's 4–5 ms whole process. Gate startup
with the same full binary, not a microbenchmark that omits module setup.

## Decision gates

For CLI: first catch the LuaJIT geomean on shared parity-gated effects,
then move toward Rust; for GUI: **0.865 → 0.92 → 0.97 → ≥1.0** against
paired Rust. No feature is advertised as faster from emission counts,
counter hits, or a non-release run. A failed A/B is documented and the
optimization stays opt-in; correctness features may still be committed
behind the existing runtime and emission gates.
