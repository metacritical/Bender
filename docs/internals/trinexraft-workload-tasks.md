# Compiler tasks from the TrineXraft workload

Evidence-driven requests from a real, profiled, interactive workload:
**TrineXraft** (`/Users/pankajdoharey/Development/Projects/ML/trinecraft`), a
voxel game in Janet rendering through raylib over the built-in FFI.

Its Phase 10 compiled the game with `bender --aot --native` and measured it
against the interpreter. **Result: 1.3–2.9%.** This document explains why, and
what would actually move it.

> Read `aot-loop-codegen-feasibility.md` first. It already answers the obvious
> question ("lift the Tier A loop veto") with a well-argued *no*, and this
> document agrees with it. The request below is the one that document itself
> recommends — widening Tier B's fusion recognizer — now with a concrete
> workload showing exactly which boundary blocks it.

---

## Getting started (read this before touching anything)

### Build

```sh
cd /Users/pankajdoharey/Development/Projects/janet
make                     # -> build/bender   (JANET_TARGET, Makefile:33)
```

**Keep a known-good `build/bender` somewhere safe before you start.** It is
the interpreter TrineXraft runs on; a regression there breaks the game
immediately, and you will want a binary to bisect against.

### Repo conventions and one trap

- The canonical plan is `docs/internals/ORCHESTRATOR-PLAN.md`. Design notes
  live in `docs/internals/`, not `docs/`.
- **`implementation_plan.md` at the repo root is a decoy.** It is the default
  target of the `plan_write` tool and has been clobbered repeatedly; its own
  contents say so. If you record a plan, pass an explicit path under the
  gitignored `tmp/`. If you find it modified,
  `git checkout -- implementation_plan.md` restores it losslessly.

### Prerequisites for the acceptance benchmark

The benchmark lives in a sibling repo and needs raylib on the system:

- `/Users/pankajdoharey/Development/Projects/ML/trinecraft` present
- raylib 6.2 (`brew install raylib`) — the engine resolves it at runtime
  through the FFI

`bench/kernels.janet` is **headless**: it imports the atlas, which imports the
raylib binding module, but the FFI context is lazy and no window is opened. It
runs fine over SSH or in CI.

### Establish your own baseline first

Do not trust the numbers below as absolutes — they are from an M2 Pro. Before
changing anything, reproduce them on your machine and record the result. Every
later claim should be relative to *your* baseline.

### Your main feedback loop

The build prints its lowering decisions. This is the fastest signal that a
change is working:

```sh
bender --aot bench/kernels.janet --native -o build/b 2>&1 | grep -E "native-skip|native-lib"
wc -l build/b-native/native-names.txt build/b-native/native-executors.txt
grep -c janet_native_poll build/b-native/native.c
```

Today those are 0, 0 and 0, with 161 `native-skip` lines. Any of them becoming
non-zero is progress worth measuring.

---

## The measurement

```sh
cd /Users/pankajdoharey/Development/Projects/ML/trinecraft
JANET_NO_PROFILE=1 bender bench/kernels.janet          # interpreted
bender --aot bench/kernels.janet --native -o build/b   # native
JANET_NO_PROFILE=1 ./build/b
```

`JANET_NO_PROFILE=1` on **both** sides is mandatory: default PGO collection
inflates interpreted timings 50–75%, which manufactures a fake speedup.

| kernel (p50, M2 Pro) | interpreted | native | gain |
|---|---|---|---|
| worldgen/chunk | 5.240 ms | 5.173 ms | 1.3% |
| mesh-chunk | 5.530 ms | 5.372 ms | 2.9% |
| raycast | 11.18 µs | 10.80 µs | 3.4% |

### What the build emitted: nothing, in either tier

```
native-names.txt        0 lines   (Tier A scalar kernels)
native-executors.txt    0 lines   (Tier B region executors)
native-continuations.txt 0 lines
janet_native_poll       0 occurrences in native.c
161 native-skip lines
```

So this is **not** the Tier A loop-veto story. Tier B — the tier that already
emits real C loops with in-loop polling, deopt and slot materialisation, and
which `--aot` does reach — matched **zero regions** in a workload consisting
almost entirely of tight numeric loops over byte buffers.

---

## Task 1 — Admit buffer/array element access into Tier B regions

### Why zero regions matched

`tools/loop-executor.janet:1-5` defines the boundaries:

> *"Arithmetic/control runs natively in regions; explicit VM boundaries
> (calls, argument pushes, **collection access/mutation**, allocation,
> division) exit before effects at their original unexecuted PC."*

TrineXraft's hot loops are made of precisely those boundary operations. The
mesher's inner loop (`src/mesher.janet`) is, per cell:

```janet
(def id (get blocks i))                      ; collection access
...
(def other (get blocks (+ i dz)))            ; collection access
...
(put colors co lum)                          ; collection mutation
(ffi/write :float x verts vo)                ; call
```

Per chunk that is ~16k cells and ~45k neighbour tests, every one of them a
buffer `get`. Worldgen is the same shape: `(put blocks idx id)` 16,384 times.
There is essentially no arithmetic *between* boundaries to fuse, so the
recognizer finds no region worth emitting — correctly, by its current rules.

**This is the gap worth closing.** Buffer and array indexed access is the
single most common operation in numeric Janet, and it is the one thing the
native region cannot do.

### What to build

Admit indexed element access on **buffers and arrays with integer indices**
into the native region, rather than treating it as an unconditional VM
boundary:

- `(get buf i)` / `(buf i)` and `(put buf i v)` where the collection is a
  known buffer/array and the index is a proven integer.
- Guard exactly as the bitwise family already does with `NX_INT32`: bounds
  and type checks that **exit to the VM at the original PC** when the guard
  fails, so the boxed path raises the same error in the same place and this
  tier never invents a result.
- Buffers can be reallocated; any admitted access must be safe against that,
  or must re-derive the data pointer after any operation that could move it.

The existing machinery is the model: `NX_INT32` shows the guard-and-exit
pattern, and the deopt/materialisation obligations are already discharged for
regions generally.

### Acceptance

1. `native-executors.txt` becomes non-empty for `bench/kernels.janet`, and
   `native.c` contains real loops with `janet_native_poll`.
2. Measurable improvement on `mesh-chunk` and `worldgen/chunk` against the
   5.372 / 5.173 ms native baseline, with `JANET_NO_PROFILE=1`.
3. Differential correctness: same seed, byte-identical output interpreted vs
   native. TrineXraft has suites for this — `./run.sh test` runs twelve,
   including `worldgen-test` (determinism, seam continuity) and `mesher-test`
   (face counts, buffer sizes, index ranges).
4. Guard failures still raise the original error at the original PC.
5. Run under the forced-tiny-quantum interrupt mode the feasibility doc
   recommends (R1), so every loop interrupts and materialisation is proven.
6. Existing suites pass: `test/suite-aot-*.janet`,
   `test/suite-native-array-result.janet`, `test/suite-dce-*.janet`.

### Honest expectations

The feasibility doc's §7 gate applies: **measure the runtime share first and
publish it.** If admitting buffer access does not move a double-digit share of
this workload's loop time, say so and stop. A negative result here is
genuinely useful to TrineXraft — it would redirect that project to
parallelism across lanes instead, which is its current fallback plan.

---

## Task 1 — RESULT (measured 2026-10-26, bender `02b32d24`)

Buffer admission landed and **works**. Measured on the TrineXraft benchmark,
p50 of three runs each:

| kernel | interpreted | native, no tier | native + executors | vs interpreted |
|---|---|---|---|---|
| **mesh-chunk** | 5.47-5.55 | 5.38-5.46 | **4.28-4.38** | **21% faster** |
| worldgen/chunk | 5.14-5.20 | 5.19-5.22 | 5.39-5.65 | **4-8% slower** |
| palette pack | 1.81 | — | 2.22 | **23% slower** |
| raycast | 11.4 us | — | 11.6 us | flat |

The tail matters more than the median for a game, and it moved further:
mesh-chunk p90 8.85 -> 4.37 ms, max 28.4 -> **4.43 ms** (6.4x better worst
case). The game emits 32 executors; spawn meshing 169 -> 140 ms; all twelve
TrineXraft suites pass.

### Observed native-vs-boxed differences; earlier causal account retracted

The original native-vs-interpreted run reported slower `worldgen/chunk` and
`palette pack`, but its attribution to Task 1 buffer admission or call-dense,
low-yield fusion was **wrong**. A directly paired, source-comparable Task 1
control (`da207b6a` pre vs `02b32d24` post, equal 12-ID manifests) already
recorded worldgen **post/pre 0.96563** (~3.44% faster) and mesh **0.84830**
in `ORCHESTRATOR-PLAN.md:485-498`. Native-vs-interpreted and buffer-admission
post/pre are different comparisons; neither alone identifies a cause for
remaining native-vs-interpreted differences. The 25.266
`generate-into` native ops per generated boundary exit belonged to the
**pre-buffer-admission** build. On the current build, it records 54,645,175
ops and 132 **NX_VMEXIT-tagged generated exits** (~413,979 ops per tagged
event); bare CALL fallbacks are not counted, so this is **not** all-exit yield.
A two-registration-site ablation that removes only its Tier-B region makes
worldgen **8.76% slower** (11 alternating
pairs; median decline/native p50 ratio 1.08762, range 1.07415–1.15847).
The region helps worldgen; declining it is not a fix.

`palette/pack` has **no emitted Tier-B region**. Its native-vs-boxed slower
observation reproduced on these arms (9 paired runs, median ratio 1.11446,
range 0.99909–1.25562; 8/9 pairs slower), but cannot result from its *own*
region guard. No common cause with worldgen has been established: image or
initialization, FFI, GC/data state, neighbouring benchmark work, code layout,
and clock contention remain possible. Ratios from independently paired trials
must not be combined into a derived overhead. The original suggestions to
decline low-yield regions or inline leaves as a remedy for this supposed
fusion regression are withdrawn. See `ORCHESTRATOR-PLAN.md` for the calibrated
12+81 region distribution, counter evidence, ablation and gate provenance.

### A trap that cost an afternoon

The first Phase 10 verdict ("AOT gives 1.3-2.9%, nothing lowers") was taken
against a plain `make` build, where the tier is compiled out. `Makefile:64-71`
does warn about this, but the failure is silent at the call site — the build
succeeds, emits `native-executors.txt` with zero lines, and reports a
plausible small gain.

Worth making louder: if `--aot --native` is invoked and
`JANET_NATIVE_EXECUTORS` is unset, or the binary lacks the compile-time
define, **say so on stderr**. A one-line warning would have saved the wrong
conclusion, and anyone benchmarking bender from the outside will hit this.

Also note `make NATIVE_EXECUTORS=1` alone is not enough after a plain build —
make does not notice the CFLAGS change, so `make clean` is required first.

---

## Task 2 — Unboxed numeric locals inside emitted code

A Janet value is NaN-boxed into 8 bytes (`janet.h:324`, `JANET_NANBOX_64`).
Inside emitted native code there is no reason to carry that representation for
provably numeric locals, induction variables and accumulators.

Tier A already does this — `aot-loop-codegen-feasibility.md` notes kernels
emit as `int64_t sum_to(int64_t p0)` with loop state in raw locals — so the
question is how far the same treatment can extend into Tier B regions, where
slots must remain materialisable on interrupt.

**Note the tension explicitly:** raw C locals are exactly what makes Tier A
unable to suspend mid-loop. Any unboxing inside a Tier B region must keep
every live slot materialisable at every poll point, or it reintroduces the
Tier A problem inside the tier that was supposed to avoid it.

### Constraints

- Janet numbers are IEEE doubles, exact to 2^53. Do not narrow to int32/int64
  where a double could legitimately exceed that, or you introduce truncating
  miscompiles. The existing standard — *"boxed Janet arithmetic IS double
  arithmetic — exact, never a truncating miscompile"* — must hold.
- NaN, infinities and negative zero must behave identically to the VM.

### Acceptance

Differential tests covering values above 2^53, NaN, ±infinity, negative zero
and integer/float boundaries, plus the forced-interrupt mode from Task 1.

---

## Task 3 — Widen bitwise operators from 32-bit to 53-bit (ergonomics, contained)

### The problem

```janet
(band 0x12345678 0xFFFFFFFF)  # ERR: rhs must be valid 32-bit signed integer
(band 0x80000000 255)         # ERR: value out of range for 32-bit signed integers
```

The mask itself is illegal, which is the sharp edge — `0xFFFFFFFF` is the
natural thing to write. In TrineXraft this silently forced a redesign: the
worldgen hash began as xorshift-and-mask, raised on its own constant, and
became Lehmer rounds modulo 2^31-1.

### Where

`src/core/vm.c:730-772`. Width and range check are macro parameters:

```c
#define vm_bitop(op)  _vm_bitop(op, int32_t,  janet_checkintrange,  "32-bit signed integers")
#define vm_bitopu(op) _vm_bitop(op, uint32_t, janet_checkuintrange, "32-bit unsigned integers")
```

Inside `_vm_bitop` (`:755-770`) the **right** operand is hardcoded `int32_t`
regardless of `type1`, so masks and shift counts need widening too.

### Both tiers must agree

This is not a VM-only change. `tools/loop-executor.janet:12-27` documents that
Janet's bitwise ops are INT32-DOMAIN and that the emitted C reproduces that
exactly, *"mirroring the scalar-kernel helpers in callgraph.janet
(k_band32/k_sl32/k_sr32/k_sru32) so both tiers agree bit-for-bit."*

Widening the VM without widening `NX_INT32`, `k_band32` and friends would make
the tiers disagree — native computing a result where the VM raises, or vice
versa. **All three must move together.**

Also preserve the deliberate `brushift`/`sru` exclusion documented at
`:21-27`: its domain is uint32, not int32, so it needs its own guard.

### The hard limit

**Do not attempt true 64-bit semantics on plain numbers.** Values are
NaN-boxed doubles; integers above 2^53 cannot round-trip. That is why
`int/u64` / `int/s64` exist as boxed types, and they already work:

```janet
(band (int/u64 0xFF00) (int/u64 0x0FF0))  ; => 3840
(blshift (int/u64 1) 40)                  ; => 1099511627776
```

Measured, and the reason the boxed path is not a general answer:

| approach | cost |
|---|---|
| int32 bit ops | 36.7 ns/op |
| plain arithmetic (`*`, `%`, `math/floor`) | 35.6 ns/op |
| boxed `int/u64` bit ops | **332.1 ns/op (9×)** |

Note the middle row: **arithmetic already matches bit-op speed and reaches
2^53.** So this is an ergonomics fix, not a performance one. Scope it that way.

### Acceptance

1. Operands up to 2^53 work for `band`/`bor`/`bxor`/`blshift`/`brshift`;
   error messages stop claiming "32-bit".
2. In-range 32-bit behaviour unchanged, including `blshift` wrapping and the
   arithmetic/logical split.
3. Both native tiers updated and still bit-for-bit identical to the VM.
4. No slowdown on the int32 fast path (re-measure 36.7 ns/op).

---

## Explicitly out of scope

**Widening Janet values to 128 bits** for unboxed 64-bit integers. Values are
8 bytes today; doubling that doubles memory traffic for every array, table and
buffer. For a cache-bound workload like voxel meshing that is plausibly a net
slowdown, and the blast radius covers equality, hashing, printing, marshalling
and the whole C API.

**Lifting the Tier A loop veto.** `aot-loop-codegen-feasibility.md` already
established the ceiling at ~54 functions / ~6.9%, inside the ±3% noise floor,
and the veto's premise still holds. This document does not reopen it.

## Order

Task 1 first — it is the only one with step-change potential for this
workload. Task 2 composes with it. Task 3 is independent and small.
