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
- 36/37 byte parity; only `swarm` differs
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

# all-37 CLI vs Rust
python3 bench-cli-packed-path-vs-rust.py
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

Verified end to end (no `janet-port` checkout exists in this repo, so an
equivalent in-repo CLI fixture with two argv workloads was used):

```text
# training build, then 3 runs of "decrypt --seed 42" and 2 of "bench --seed 17"
bender --aot: --pgo-auto merging 5 auto profiles over 2 workloads
bender --aot: --pgo-auto workload 0 runs=3 argv="decrypt --seed 42"
bender --aot: --pgo-auto workload 1 runs=2 argv="bench --seed 17"
bender --aot: cached PGO profiles for 2 workloads
bender --aot: --pgo-auto published 2 gateable workloads
bender --aot: pgo parity ok, installed tmp/e2e/app-autopgo
# then --release from the same cache
bender --aot: verifying 2 cached workloads
bender --aot: pgo parity ok, installed tmp/e2e/app-autopgo
```

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

## Honest status

The CLI is still **~22x slower than Rust** in this end-to-end protocol.
Packing removed meaningful table/GC overhead, but the dominant remaining
budget is VM execution and non-continuable native calls.  Auto-PGO exists,
collects runtime evidence, and now feeds a verified release build through
`--pgo-auto`; that closes the plumbing gap but does not by itself move the
~22x figure, because the remaining budget is VM execution and
non-continuable native calls (proposal items 5-9).  No claim of Rust parity or full automatic self-optimization
is justified yet.
