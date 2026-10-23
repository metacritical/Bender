# ttfx AOT/PGO: current pipeline, measured gaps, and runtime-feedback proposal

Date: 2026-10-23

## Executive summary

The compiler has two PGO mechanisms:

1. **Explicit `--pgo` collection**: `bender --aot ... --native --pgo "<workload>"`
   runs a named, replayable workload, records a profile, verifies the
   compiled binary's output against the interpreter, and publishes a
   gateable PGO cache record.  A later `--release` build can use that record.
2. **Auto-PGO**: a non-release AOT binary profiles its first qualifying
   run into `<cache>/<key>.jdn`; further runs with `JANET_AUTO_PGO=1` write
   unique `.run-...jdn` sidecars.

The final release does **not** self-profile.  This is intentional: the
release binary is the benchmarking/product binary, and `--release`
explicitly adds `-DJANET_NO_PGO` and `-DJANET_NATIVE_TELEMETRY=0` in
`src/boot/boot.janet`.  The training/profiling binary is a separate build.

**Status 2026-10-23: this gap is now closed** (see "Implemented:
sidecar provenance and `--pgo-auto`" below).  Auto-PGO profiles carry a
companion workload record, and `bender --aot --native --pgo-auto` merges,
replays, gates, and publishes them as release evidence.  The description
below is kept because it explains what the pipeline looked like before,
and the failure messages still apply to profiles written by binaries that
predate the provenance record.

The actual integration gap was narrower than a missing profiler:
**auto-PGO sidecars were diagnostic and not replayable**, so they could
not be merged into a release build.  A rebuild from such an auto cache
prints

```
bender --aot: cached profile has no replayable workload; pass --pgo once
```

and `--release` fails with

```
bender --aot: --release needs compatible cached PGO evidence and a replayable workload; run --pgo first
```

The intended workflow is therefore:

```
training build (PGO enabled) -> 1-3 representative runs -> merge/gate
    -> release build with PGO instrumentation disabled
```

The release build consumes the merged profile at compile time only; the
release binary has no runtime PGO connection and never reads the cache.
The first two stages work.  The missing piece is making multiple training
runs carry enough workload provenance to be merged and gated for the release
build, without putting the profiler back into the release binary.

## Verified auto-PGO experiment

Commands used with the current packed-path source:

```sh
JANET_NATIVE_EXECUTORS=1 bender --aot bin/ttfx-cli.janet --native \
  -o build/ttfx-cli-autopgo
for i in 1 2 3; do
  XDG_CACHE_HOME=$T/autopgo-check-cache JANET_AUTO_PGO=1 \
    build/ttfx-cli-autopgo decrypt --bench --input-file logo.txt --seed 42
done
```

Observed cache contents after three runs:

```text
<xdg>/janet/pgo/ttfx-cli-autopgo.jdn
<xdg>/janet/pgo/ttfx-cli-autopgo.jdn.run-61523-0.jdn
<xdg>/janet/pgo/ttfx-cli-autopgo.jdn.run-61527-0.jdn
```

So the runtime collection is real: one base profile plus sidecars for runs
2 and 3.  But rebuilding with that cache produced **no workload gate**:

```text
bender --aot: cached profile has no replayable workload; pass --pgo once
native-lib 8 kernels build/ttfx-cli-autopgo-rebuild-native
```

and a release build refused:

```text
bender --aot: --release needs compatible cached PGO evidence and a replayable workload; run --pgo first
```

This is an integration gap, not a missing profiler.  The runtime sidecars
need work to become gateable release evidence.

## Why the CLI is still ~22x slower than Rust

Measured with the packed-path candidate:

- 400 bounded frames, `logo.txt`, seed 42, 80x24
- Rust `--parity-dump` vs Bender `--dump-frames`
- three alternating paired rounds
- startup, effect construction, frame generation, and output included
- 36/37 byte parity; only `swarm` differs — **verified 2026-10-23**, see
  "Stage 0" below
- Bender/Rust time geomean: **21.773** (Bender speed **0.046x**)
- previous release: 23.242, so the three packed-structure changes are a
  real but modest **~6.3%** improvement

Largest remaining ratios:

| Effect | Bender/Rust time |
|---|---:|
| rings | 61.7x |
| binarypath | 61.0x |
| vhstape | 48.2x |
| waves | 47.0x |
| smoke | 40.0x |
| colorshift | 38.0x |
| thunderstorm | 38.0x |

### Release sampling vs interpreter PGO

A 1 ms `sample` of the AOT release (`--bench`) showed:

- `run_vm` is still the largest single stack
- `janet_gc_barrier`, `janet_dict_find`, `janet_in`, `janet_equals`
- executor `ctx-update`
- executor `ctx-fire-event`
- executor `ctx-frame-string`
- `native_structure_matches`, `janet_fiber_funcframe`,
  `janet_native_poll`
- executor `ctx-path-step`

The interpreter profile overstates RNG limb helpers; the release already
uses native RNG kernels.  PGO call counts alone are therefore not a
release-time attribution oracle.

### Structural reasons for the gap

1. **Partial native coverage.**  The compiler emits C executors only for
   structurally eligible functions (83 loop executors in the original CLI
   release, 81 in the packed-path candidate).  Most Janet functions still
   run through `run_vm`.
2. **Executor-to-VM exits.**  Executors are not a whole-program C compiler.
   Calls, collection mutation, allocation, division, and unproven types
   fall back to the VM at the exact bytecode PC.  The hot `ctx-update`
   executor calls `ctx-motion-move` and `ctx-step-animation` through the
   VM.
3. **No native-to-native continuation.**  A call from one executor to
   another native function currently exits to `run_vm`; it does not chain
   native frames.  This is the biggest missing compiler feature.
4. **Boxed state.**  Janet numbers, tables, arrays, and tuples are boxed.
   The `table put/get` and GC-barrier traffic in scene/event/animation
   state is visible in release samples.  Packed scene frames, scene state,
   and path state removed part of this cost.
5. **Type evidence is not proof.**  Native kernels are emitted only where
   PGO/inference proves types.  Functions such as `ctx-motion-move`,
   `ctx-step-animation`, `ctx-char-active?`, `scene-next-eased`,
   `geom-lerp-point`, and `ctx-coord-on-line` were refused with messages
   such as:
   - `params [table ? table] ret nil`
   - `params [? ? ?] ret nil`
   - `params [?] ret tuple`
   - slot mixes `keyword/unknown` or `u64load/array`
6. **GC and allocation.**  `scene frames`, motion paths, and events were
   previously allocated as tables; per-tick writes incurred hash lookups
   and barriers.  Packing reduced but did not eliminate this class.
7. **PGO is offline and not continuous.**  The release profile is one
   explicit workload (decrypt) for CLI and scattered for GUI.  It does not
   include the 37-effect distribution, and a `--release` binary does not
   feed later runs back into the compiler.
8. **The generated C is guarded executor C, not Rust-style monomorphic
   structs.**  Rust owns typed `Vec`s/structs and direct call paths; the
   Janet AOT path still validates/guards and falls back.

## What was already optimized

Accepted structural changes (same decrypt PGO unless noted):

| Change | All-37 PGO A/B geomean | Median | Faster | Parity |
|---|---:|---:|---:|---|
| Packed scene frames | 0.9744 | 0.9773 | 29/37 | 37/37 |
| Packed scene state | 0.9779 | 0.9796 | 32/37 | 37/37 |
| Packed motion-path state | 0.9893 | 0.9895 | 25/37 | 37/37 |

Combined: about **5.8%** faster than the original CLI release.  The new
CLI-vs-Rust geomean is 21.773 vs 23.242.

Rejected experiments:

- event table -> array: 1.0082 (13/37 faster)
- packed RGB color-cache key: 1.0285 (8/37 faster)
- merged all-37 GUI PGO: 0.9992 FPS ratio; no win
- merged all-37 CLI PGO: 1.0128 time ratio; no win

## Proposed runtime-feedback integration

Goal: let ordinary runs (1–3) produce evidence that can be merged into a
verified release build without pretending arbitrary runs are replayable.

1. **Record workload provenance in auto-PGO sidecars.**  *(implemented)*  When the AOT
   launcher writes `<key>.jdn.run-*`, also write `<key>.run-*.workload`
   containing argv (and stdin bytes when applicable), plus source/image
   identity.  Do not record GUI/uncaptured non-replayable runs as
   gateable.
2. **Add a sidecar merge command.**  *(implemented as `--pgo-auto`)*  A new `--pgo-auto` build mode merges
   the base profile and sidecars, deduplicates by workload identity, and
   requires each record to reproduce its recorded output.  Sidecars that
   fail parity are quarantined, never used.
3. **Keep release instrumentation off.**  `--release` should remain
   `JANET_NO_PGO` for speed; the feedback loop is:
   profiling/non-release build -> 1–3 runs -> merge -> verified release.
   Optionally a `--pgo-release` wrapper automates the cycle.
4. **Profile multiple workloads, but gate each separately.**  A merged
   profile from all 37 effects slowed code in previous tests because
   types diverged.  The right unit is per-workload/group evidence, with
   conservative type intersection, not one giant union.
5. **Feed runtime call/exit counters back into executor selection.**
   PGO currently guides kernels/leaf selection; loop executor selection is
   structural.  Add per-definition VM-exit/native-entry counters and use
   them to choose hot loop executors and native continuations.
6. **Implement native-to-native continuation.**  This is the highest-value
   compiler feature for `ctx-update -> ctx-motion-move/ctx-step-animation/
   ctx-path-step` and for animation helpers.  PGO should mark hot call
   edges; the executor call path should chain native frames where types and
   GC/quantum semantics are proven.
7. **Extend scalar/tuple kernels.**  Geometry helpers return boxed tuples;
   scene easing returns a tuple.  Add an ABI for scalar pair returns or
   small unboxed structs, then fuse `geom-lerp-point`,
   `ctx-coord-on-line`, and `scene-next-eased` into path/step code.
8. **Pack animation state behind accessors.**  Effects access
   `:cur-col`, `:cur-row`, `:cur-sym`, `:cur-fg`, `:active-path`,
   `:active`, `:layer`, `:paths`, and `:scenes` directly.  Migrating those
   to accessors and packed arrays is the next large table/GC reduction.
9. **Native frame/color serializer.**  `ctx-frame-string` remains hot.
   A validated C serializer over packed cell records, with exact ANSI
   bytes, would remove per-cell Janet string/table work.

## Handoff: files and commands

Compiler:

- `tools/callgraph.janet`: profile parsing, kernel/executor selection,
  native C emission.
- `tools/loop-executor.janet`: executor bytecode eligibility and emitted
  C, including call fast paths and VM boundaries.
- `src/core/vm.c`: native registry, dispatch, auto-PGO collection.
- `src/core/bytecode.c`: core predicate fast paths.
- `src/boot/boot.janet`: `--pgo`, auto-PGO cache, `--release` flags and
  parity gate.
- `tools/mkimage.janet`: auto-PGO launcher/cache sidecars.

ttfx runtime:

- `janet-port/src/scene.janet`: packed frames/scenes/paths, frame
  serializer.
- `janet-port/src/effects/*.janet`: direct animation/path field users.
- `janet-port/build/ttfx-cli-packed-path-pgo`: current candidate.
- `janet-port/build/ttfx-cli-container-rel`: prior CLI release.

Reproduction:

```sh
# explicit PGO + verified release
JANET_NATIVE_EXECUTORS=1 XDG_CACHE_HOME=$CACHE bender --aot \
  bin/ttfx-cli.janet --native --pgo "decrypt --bench --input-file logo.txt --seed 42" \
  -o build/ttfx-cli-pgo
JANET_NATIVE_EXECUTORS=1 XDG_CACHE_HOME=$CACHE bender --aot \
  bin/ttfx-cli.janet --native --release -o build/ttfx-cli-pgo

# auto-PGO observation
XDG_CACHE_HOME=$CACHE JANET_AUTO_PGO=1 build/ttfx-cli-autopgo decrypt --bench ...

# all-37 CLI vs Rust -- bench-cli-packed-path-vs-rust.py was never committed
# and does not exist; the committed replacement lives in ML/ttfx
python3 bench/validate-bender.py --rust --timing
```

## Implemented: sidecar provenance and `--pgo-auto`

Proposal items 1 and 2 are implemented.

**Provenance** (`src/core/vm.c`, `src/core/io.c`, `tools/mkimage.janet`).
The AOT launcher passes its exact argv to the VM
(`janet_profile_auto_workload`).  Whenever the VM publishes an auto-PGO
profile it writes a companion `<profile>.workload` JDN record beside it,
using the same vocabulary as the explicit `--pgo` cache record
(`:args`, `:argv`, `:has-stdin`, `:stdin-bytes`) plus `:replayable`,
`:reason`, `:binary`, `:image-bytes`, `:stdout-hash`, `:stdout-bytes`.
It is a companion file rather than part of the `.jdn` because that payload
is a bare profile row array consumed by `tools/profile-merge.janet`.

Replayability is decided, never assumed:

| run shape | verdict |
|---|---|
| stdin never read | replayable, `:has-stdin false` |
| stdin read from a seekable regular file <= 1 MiB | replayable, bytes captured |
| stdin read from a tty or pipe, or an oversized file | `:replayable false :reason :stdin-not-replayable` |
| an argv element that cannot round-trip the space-joined `:args` string | `:replayable false :reason :unquotable-argv` |

`io.c` reports stdin consumption from the single `read_chunk` choke point
and hashes bytes reaching `stdout`; both are inert unless the launcher
armed auto-PGO.

**`--pgo-auto`** (`src/boot/boot.janet`).  Requires `--native` and a real
binary build; incompatible with `--pgo`, `--release`, `--no-auto-pgo`,
`-c/-S/-E`.  It loads `<key>.jdn` plus every sidecar, refuses with a
per-profile reason anything that lacks a workload record, is malformed, is
non-replayable, or is older than the sources, then groups the survivors by
workload identity (argv + stdin bytes).  Each workload keeps its own
merged evidence through the same `profile-merge --accumulate` machinery
`--pgo` uses -- per-workload evidence, not one anonymous union -- and is
replayed through the existing parity gate against an oracle regenerated
from the current sources.  The recorded `:stdout-hash` is provenance, not
authority.  Evidence that fails parity is quarantined
(`<profile>.quarantine-<pid>`, with its workload record) and no binary is
installed.  On success one gateable v3 record per workload is published
through the `--pgo` publication path, so a later `--release` succeeds.

`--release` is unchanged and still instrumentation-free
(`-DJANET_NO_PGO -DJANET_NATIVE_TELEMETRY=0`); a release binary writes no
profile.

Verified end to end against the real ttfx CLI at
`ML/ttfx/janet-port` (cache `XDG_CACHE_HOME=/tmp/autopgo2-cache`,
`logo.txt` = `../ruby-port/logo.txt`), using two workloads so the merge,
the dedupe and the per-workload gate are all exercised:

```text
# training build, 3 runs of "decrypt --bench ..." and 2 of "decrypt --dump-frames ..."
bender --aot: --pgo-auto merging 5 auto profiles over 2 workloads
bender --aot: --pgo-auto workload 0 runs=3 argv="decrypt --bench --input-file ../ruby-port/logo.txt --seed 42"
bender --aot: --pgo-auto workload 1 runs=2 argv="decrypt --dump-frames --max-frames 400 --input-file ../ruby-port/logo.txt --seed 42"
pgo-divergence: @["src/color.janet" 110 "color-fg"] workload 0 vs 1 params @[@[:array :tuple]] != @[@[:tuple]]
bender --aot: --pgo-auto published 2 gateable workloads
bender --aot: pgo parity ok, installed build/ttfx-cli-autopgo2
# --release from the same cache (same -o: the cache is keyed by output basename)
bender --aot: verifying 2 cached workloads
bender --aot: pgo parity ok, installed build/ttfx-cli-autopgo2
```

The `--dump-frames` workload makes the gate meaningful rather than
vacuous: 2,423,238 bytes of frame output, byte-identical between the
release binary and the interpreter.  (`--bench` writes its timing to
stderr and leaves stdout empty, so a `--bench`-only workload proves only
that the run reproduces an empty stdout.)

`bench/validate-languages.py` was run through a transient copy restricted
to installed runtimes (`lua`, `luajit` and `chez` are absent on this
machine) and extended with the two Bender binaries under test:
**150 byte-parity checks passed**, 5 effects x 3 inputs x 2 seeds, with
the `--pgo-auto` release binary byte-identical to Rust, to the current
`ttfx-cli-packed-path-pgo` candidate, and to the interpreter.

For one identical workload, the generated C from `--pgo-auto` evidence is
**byte-for-byte identical** to the generated C from an explicit `--pgo`
collection of that workload, and identical again to the C behind the
existing `ttfx-cli-packed-path-pgo` candidate (same 81 loop executors,
same 9 kernels, same 4,486,468-byte `native.c`).  Auto-PGO evidence is
therefore not weaker evidence; it is the same evidence collected at a
different time.

Direct `--dump-frames` A/B against that candidate, seed 42,
`../ruby-port/logo.txt`, 400 frames:

| effect | bytes | sha256 (first 24) | vs candidate | vs interpreter |
|---|---:|---|---|---|
| decrypt | 2,423,238 | `315c4acb2580e71f36bdb6ed` | identical | identical |
| rings | 2,693,696 | `6f53f72d3be9b59a1cf0ac05` | identical | identical |

`--bench` timing on decrypt is dominated by machine noise, and the stored
candidate binary is faster than *any* binary rebuilt today, including one
built from the untouched pre-change compiler (7 order-alternating rounds,
best-of):

| binary | min ms | median ms | min vs candidate |
|---|---:|---:|---:|
| `ttfx-cli-packed-path-pgo` (stored candidate) | 215 | 241 | 1.000 |
| pre-change janet `6cdb7857` + explicit `--pgo` | 240 | 277 | 1.116 |
| this work + `--pgo-auto` | 234 | 265 | 1.088 |

Since a build from the untouched tree is the *slowest* of the three while
the generated C is identical everywhere, the spread is link/layout and
machine state, not code quality, and `--pgo-auto` is not a regression
against an explicit `--pgo` build.

Known limitations:

- Stdin capture covers only a seekable regular file up to 1 MiB; piped or
  interactive stdin marks the run non-gateable rather than guessing.
- `:image-bytes`/`:stdout-hash` are recorded as provenance but are not yet
  gate criteria; freshness still uses source mtime plus the parity replay.
  A build-emitted image identity would make binary-to-source binding
  exact.
- A workload whose argv is parsed positionally cannot pass the gate at
  all, because an AOT binary's `main` receives argv without `argv[0]`
  while the interpreter prepends the script path.  This predates
  `--pgo-auto` and applies equally to `--pgo`; CLIs that scan flags (like
  ttfx) are unaffected.
- `--pgo-auto` refuses to promote a legacy v2 cache record; run `--pgo`
  once first.
- The PGO cache is keyed by the **output basename**, so the `--pgo-auto`
  and `--release` builds must use the same `-o` as the training build.  A
  different `-o` reports "no replayable workload" and is easy to mistake
  for an evidence problem.
- `bench-cli-packed-path-vs-rust.py`, the 37-effect A/B harness this
  document cited, was never committed anywhere.  It has been **replaced** by
  `ML/ttfx/bench/validate-bender.py` (see "Stage 0" below), which gates all
  37 effects and their 436 canonical option sets and reports parity and
  timing from one run.  A 6-effect `--bench` subset gave a geomean of 0.998
  for the `--pgo-auto` release against `ttfx-cli-packed-path-pgo`; the full
  37-effect table now gives 1.0205, but both numbers are confounded by the
  executor-binding inequality described below.

## Stage 0: the 37-effect gate exists again, and the swarm figure is confirmed

The committed replacement for the missing harness is
`ML/ttfx/bench/validate-bender.py` (`bench/bender-37-gate.md` for the
analysis, `bench/baselines/bender-37-2026-10-23.json` for the frozen run).
It enumerates all 37 effects *and* their 436 canonical option sets from
`tools/fx/cases/*.txt`, compares sha256 over complete bounded `--dump-frames`
output, reports a paired `--bench` timing table with geomean in the same run,
and reports Tier-B executor bindings per binary.

Two figures from this document are now settled empirically:

1. **"36/37, only swarm differs" is correct.** Bare invocations, seeds 42 and
   1337, 400 frames, `logo.txt`: Bender-vs-Bender 37/37 exact (74/74 cases);
   Bender-vs-Rust 36/37 with `swarm` the **sole** divergence
   (seed 42: janet 739,319 B `8327687e…` vs rust 757,416 B `0e69981a…`;
   seed 1337: janet 676,336 B `2b39cb85…` vs rust 672,992 B `11c0746a…`).
   Every record in the ttfx tree claiming 37/37 against Rust is wrong about
   swarm.  Under the canonical option sets Bender-vs-Rust drops to 24/37, but
   57 of those divergences are Janet CLI flags that do not exist (the CLI
   ignores unknown flags silently, so the two sides run different workloads);
   only 15 are genuine engine divergences.
2. **The all-37 A/B that validated the three packing stages is reproducible
   again**, so Phase 4 work can be gated.  The current candidate (built via
   `--pgo-auto` then `--release`) is **37/37 exact** against
   `ttfx-cli-packed-path-pgo` over 276 runnable cases.

Two blockers the gate surfaced, both documented in `bench/bender-37-gate.md`:
`bin/ttfx-cli.janet` `flag-values` panics on any option value shorter than
two characters, which blocks 159 of 436 canonical option sets (one-line fix,
verified in a scratch copy, not applied); and the
`docs/benchmarks/2026-09-21.csv` digests cannot cross-check the Janet port,
because that protocol needs `--virtual-clock`, which the Janet CLI does not
implement and silently ignores.

**Executor bindings are not automatically like-for-like.** The gate found the
candidate binding 79 of 81 loop executors while the baseline binds 81
(`int-string?` and `to-i` refused — now named, see the `id=` in
`tools/callgraph.janet`).  The same two refusals appear when ttfx is rebuilt
with the pre-change compiler at `6cdb7857`, so this is a property of
rebuilding today rather than of the auto-PGO work — but it means the current
timing geomean (1.0205 candidate/baseline) measures executor inequality as
much as anything else.  Equalizing those two is Phase 4's first task.

## Honest status

The CLI is still **~22x slower than Rust** in this end-to-end protocol.
Packing removed meaningful table/GC overhead, but the dominant remaining
budget is VM execution and non-continuable native calls.  Auto-PGO exists,
collects runtime evidence, and now feeds a verified release build through
`--pgo-auto`; that closes the plumbing gap but does not by itself move the
~22x figure, because the remaining budget is VM execution and
non-continuable native calls (proposal items 5-9).  No claim of Rust parity or full automatic self-optimization
is justified yet.
