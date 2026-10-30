# General Bender optimization driven by ttfx

Updated 2026-10-21. **This is the authoritative compiler work/status record.**
The original repository is `/Users/pankajdoharey/Development/Projects/bender`.
All ongoing optimization design and status belong in this compiler repository.
The ttfx port is an unchanged integration fixture, not an optimization target.
Its existing `bench-engines.md` contains historical measurements; temporary
logs/binaries are supporting evidence only.

## 2026-10-21 checkpoint: committed safety foundation, not Rust parity

The next optimizer stages and their falsifiable gates are in
[`profile-guided-optimizer-plan.md`](profile-guided-optimizer-plan.md).
Two generic features are now committed: `ed422ca9` (direct primitive
array/tuple struct writers with generic fallback) and `0178fd1f`
(guarded native reads/iteration/scratch arrays and safe FFI-alias
safepoints, plus a cheap no-descriptor runtime dispatch gate). The
executor remains double opt-in; **do not enable it in the default
release based on emitted descriptor counts**. A prior leaf-FFI shortcut
was unsafe because it dropped GC roots, and has been removed.

`build/bender` was rebuilt with the default macro-free runtime and
reports `1.43.0-dev-0178fd1f`. The unchanged ttfx CLI was PGO-collected
twice and verified with `--release`: all 37 bounded frame dumps match
`rel2` at seed 42; five effects at five held-out seeds match 25/25.
Seven paired rounds on five effects gave geometric-mean release
throughput **1.006×** `rel2` (noise). The corrected opt-in executor was
**0.989×** its same-compiler macro-enabled no-executor control and
**0.927×** the macro-free `rel2` in a separate paired sitting. These are
correctness foundations, **not an achieved speedup**. Compiling executor
support but publishing no descriptors had cost ~6–7%; the new dormant
dispatch gate recovered **1.067×** on that macro-enabled control.

Windowed PGO collection initially exited 139 from a direct interpreter
launch after the hidden-window/font stage; the old `rel2` control also
crashed under the same idle-display conditions. A long-lived
`caffeinate -d -u` before collection restored the original real-GUI
workload: two scattered/windowed/uncapped collections and verified
`--release` now pass. No dummy `--list` workload or parity bypass was
used. The last complete Bender/Rust **37-effect** GUI comparison still
belongs to the earlier paired `rel2` record.

The sustained single-process **release** profile and first measured
runtime fix are recorded in the execution plan. Commit `5e3f2740`
caches the strict-heap *diagnostic logging flag*, not the isolation
check, across slab allocation and GC barriers. A four-second scattered
sample previously spent roughly 3% of main-thread stack tops in libc
environment lookup; the lookup disappears from the top list afterward.
Six paired long-running release samples improve **1.033×**, while five
whole-process CLI effects are neutral at **1.001×** geomean. The
default-off Bender and a new PGO-collected CLI release were rebuilt,
with 37/37 bounded frame dumps and 25/25 held-out effect/seed cases
byte-identical to `rel2`. GC-minor, GC-stress, proc (99/99), ev
(776/776), and the enabled executor/ASan matrix pass. This is an
incremental VM result; the GUI release PGO launch blocker above is
resolved, but the effectful-loop AOT compiler remains the next major
piece. With the same compiler, same real GUI PGO workload and paired
five-round windowed probes, the pre-GC and cached-flag release builds
are **1.005×** (noise). No broad GUI speedup is claimed.

## Goal and correctness contract

Optimize the general Bender compiler/VM, not effect-specific kernels. Unmodified
ttfx is the workload: use its `ruby-port/logo.txt` Omarchy artwork (81×10), not
hello or the historical repeated-fox input. Train with seeds 42/7/99/2026 and
reserve 13/314159 for held-out parity when all four train PGO.

Do not add custom kernels, benchmark-specific shortcuts, source reshapes, or
optimization tracking documents to `bender-port`. Existing handwritten application
kernels are historical inputs to the fixture, not templates to special-case in
Bender. Any new mechanism must be tested with generic Bender programs first and
must apply without changing the application's source.

The requested goal is at least 1.10× Rust and Node on every comparable benchmark.
**This goal is not achieved.** Match workload and semantics; preserve frame bytes,
GUI appearance/tick cadence, numeric behavior, GC, debugger behavior and exact VM
quantum/suspension ordering. Call counts, emission and successful registration
are not performance measurements. Use focused matched timing only after correctness.

Latest verified CLI release comparison:
[Omarchy AOT/PGO versus Rust](omarchy-cli-release-comparison.md). Full output
parity passes, but the rebuilt release remains within noise of previous Bender;
Rust fx is 6–39× faster on the measured whole-process dump workloads. Six leaf
kernels remain; the current compiler infrastructure has not nativeized the hot
mixed-operation scene loops. Do not describe these changes as a measured speedup.

## Compiler branch history (not yet merged into shared master)

Development began at `5fe56858` in isolated worktrees to preserve other agents'
dirty shared checkout. Branch `ttfx-native-array-call` is the integration branch;
`ttfx-multiseed-pgo` was integrated through a non-squash merge.

| Commit | General mechanism |
|---|---|
| `e5770fa2` | Proven native array-result composition into typed destination slots |
| `400cc465` | Multiple replayable PGO workloads, conservative merging and immutable cache generations |
| `d676d6e2` | Non-squash merge preserving both component histories |
| `19ef8e05` | Isolated native-array test roots |
| `5ac1c89b` | Joint argument-tag histograms and guarded scalar kernel variants |
| `6f1a74d0` | Analysis-only retention of hinted/profile-observed exported definitions |
| `3c9d8bc7` | Preserve home/worker provenance rows for one verified function identity |
| `19d7c19d` | Respect PGO cache locks; migrate automatic-PGO regression gates |
| `978400f1` | Default-off resumable VM-frame native numeric-loop executors |

Preserved checkpoint refs: `ttfx-compiler-verified-base`,
`ttfx-compiler-phase2-verified`, `ttfx-compiler-dce-verified`.
The completed commits were merged into the original repository's `master`
checkout as **`58268620`**, preserving component and merge history. The incoming
files did not overlap the shared checkout's 12 pre-existing dirty/untracked files;
SHA-256 checks confirmed all 12 were preserved byte-for-byte. The original checkout
was subsequently rebuilt: `build/bender` reports `1.43.0-dev-5180da8a` and
`build/libbender.a` was refreshed, with experimental executors default-off.
Array-result 16, joint-dispatch 28, hinted-export 78, multiseed 35, release 14,
and auto-PGO 98 assertions passed (269 total), plus hinted-callgraph checks.

## Implemented and validated

### Native result composition

Array-returning native calls can assign to proven, join-safe array slots even in
a double-returning caller. Direct-call argument proof occurs before a reused
destination register changes type. Tail return and mixed-slot checks remain.
This passes `BenderArray*`, **not unboxed array elements**. Regression: 16 checks.
Current ttfx still makes separate step/tail calls; no standalone speedup claim.

### Multi-workload PGO and guarded variants

- V3 manifests preserve exact argv/stdin, raw profiles and output oracles for
  each workload. Identical workloads accumulate; distinct ones remain separate.
- New collection and release replay all compatible prior workloads before
  installing. Copy-on-write generation files and atomic manifest publication
  preserve previous evidence on failure. Busy caches cannot authorize replay.
- V2 remains compatible; multi-workload UPX is explicitly unsupported.
- Joint full-arity tag vectors are bounded at eight; overflow/missing evidence
  remains open. Only identical vectors merge; no invented marginal cross-products.
- Loop-free scalar variants guard every argument, preserve boxed fallback and
  respect actual integer-conversion limits. No seed-number dispatch or inference
  that sampled array elements prove a numeric layout. Returns need static proof.
- Tests: multiseed 35, release 14, auto-PGO 98, joint-dispatch 28 assertions;
  bounded Omarchy training and held-out byte parity passed. The tested ttfx
  workload emitted no joint-dispatch candidate: no demonstrated ttfx speedup.

### Profile-observed export discovery

Analysis-only retention preserves exact nonprivate direct closure identities
selected by hints or positive-call source-qualified profile rows. Retained orphans
add no executable closure edge; ordinary image compilation stays unchanged.
Compiler scope identities are cleared at pop/finalization and GC roots are kept.
Repeated observations retain provenance; genuinely ambiguous definitions refuse.

Tests: compiler suite 78 plus qualified-import/refusal cases and prior suites.
Omarchy profiles now expose `ctx-update` and `ctx-frame-string`; classic native
emission still rejects their loops, and `ctx-char-active?` lacks table type proof.
Raw JDN path/line/name is not a content hash; normal release replay additionally
checks source compatibility. Bounded 37-effect Omarchy byte parity passed.

### Native executor stage 1

The integration branch's `docs/internals/native-loop-executors.md` documents
the ABI and test runner. Generated pure numeric loops keep original bytecode,
PCs and GC-scanned boxed slots. Runtime registration validates actual functions;
fallback executes the original instruction once. Backedges use the same polling
policy as the VM. Original returns, debugger stepping and marshal are preserved.

Generic tests demonstrate executed entries/backedges and identical budget1/2
interrupt traces, portable switch, default-off/interrupt-disabled configurations,
fallback/GC/cancellation/interleaving/marshal/debug checks and IEEE numeric cases.
Independent source review found no concrete blocker. Full shared-stack quantum
ASan aborts during re-priming even with executors disabled; this limitation has
not been suppressed. Stage 1 remains **off by default**.

Unmodified ttfx emitted **zero stage-1 executors**, retaining six classic kernels.
Bounded Omarchy decrypt/fireworks six-seed parity passed. No speedup claim.

## Current work: mixed-operation native regions (stage 2)

Stage 2 is implemented on branch `ttfx-mixed-regions`, still **off by
default** (same double opt-in: `-DBENDER_NATIVE_EXECUTORS` runtime build
plus `BENDER_NATIVE_EXECUTORS=1` at emission). Mixed-op loop bodies are
admitted with native arithmetic/control regions around explicit VM
boundaries (`call`, `tcall`, `push*`, `get`, `in`, `len`, `next`,
`put`, `mkarr`, `mkbuf`, `mktup`, the divide/modulo family) that exit
before effects at their original unexecuted PCs. Opaque non-primitive
constants load from the live pool into scanned slots; descriptors embed
no GC pointers and primitives stay exact-matched. Attachment to actual
runtime functions is lazy and structural: descriptors publish into a
process registry at `native_init` and each definition tries the unique
structurally matching descriptor once per registry generation at first
dispatch (imports, unmarshaled closures and later-evaluated functions
alike; ambiguous identity refuses). Whole-function entry validation is
gated by a useful-region bitmap (>= 3 native ops ahead; small defs
exempt) so boundaries do not pay validation per exit. Counters separate
entries/backedges/guard-failures from native ops and VM-boundary exits,
plus per-VM attach and exit-by-opcode statistics.

Verification: the six-configuration executor matrix (budgets 1/2,
portable dispatch, interrupts off, default-off, ASan+UBSan) now also
runs `test/native-executor-mixed.c` — differential native-vs-boxed
parity with exact-once side effects, paired interrupt parking, opaque
constant liveness, GC across boundaries, breakpoint/step suppression,
parked-marshal round trip with re-attachment, later-evaluated
attachment, counters and ambiguity refusal. Bounded unmodified Omarchy
decrypt+fireworks (40 frames, seeds 7, 13, 42, 99, 2026, 314159) are
byte-identical between no-executor and mixed-executor builds of the
same branch. Actual counter probes show native execution of
`ctx-frame-string`, `ctx-update`, `geom-bezier-length` on decrypt, plus
`geom-coord-on-bezier` and `ctx-path-step` on fireworks; the latter two
are honestly unhit on decrypt (no path motion). A pre-existing marshal
limitation was documented, not introduced: fibers parked at a call poll
lose values between stackstart and stacktop (reproduced with native
execution disabled). A few definitions refuse attachment as
structurally ambiguous and stay boxed. No timing claim.

## Rejected experiments (preserved, not shipped)

| Candidate | Correctness | Focused Omarchy result |
|---|---|---|
| Own-keyword table inline cache | Mutation/GC/cancellation/reinit tests passed | Decrypt 15–16% slower; fireworks 11–12% slower |
| Numeric two-op VM superinstructions | Differential/quantum/debug tests passed | Neutral/slightly slower, 0–1.6% |

Both were removed from source and archived with exact patches, tests and hashes.
The separate application render-texture experiment failed screenshot parity and
was removed. None is counted as a completed kernel port.

Two build-helper invocations accidentally wrote automatic profiles to the user's
default cache. Both were disclosed and no cleanup/overwrite was attempted there.
Further compiler builds and tests use explicit private cache/test environments.

## Final integration and preservation

Keep independent commits and merge parents, review combined diffs, and rerun
combined compiler tests plus unmodified Omarchy trained/held-out parity. Do not
reset dirty checkouts, force-push, squash away component history, blanket-stage
untracked files, or delete unknown work. Reconcile newer approved master commits
in the isolated integration branch, then coordinate a normal merge into clean master.

Shared-tree inventories initially recorded 11 Bender and 202 ttfx dirty/untracked
entries; a subsequent SHA-256 comparison found them unchanged. Review source,
tests/docs, generated artifacts, CI/ignore edits and ownership independently.
At the user's request, remaining original-checkout WIP was then organized into
feature commits rather than blanket-staged:

| Commit | Preserved work |
|---|---|
| `ebb83566` | Archive the unrelated partial recovery at `recovered/role-subagents-plan.partial.md`; restore the tracked Bender plan |
| `151bdc24` | Ignore Finder metadata without deleting it |
| `5ada1e33` | Historical combined compiler/messaging roadmap |
| `092269e1` | Historical DCE safety-gate/stripping plans |
| `6a3f59a2` | Type-generator design, with historical-scope corrections |
| `6f2da236` | Historical workload diagnosis, not an application-optimization mandate |
| `8bee3ee1` | Manual pool probes and byte-preserved archived proc snapshots |

The original partial plan cannot be fully reconstructed; its fragment and recovery
note remain preserved, not endorsed as current instructions. Both manual pool
probes exited 0 under a timeout. Historical plans include provenance/status warnings.
The original Bender worktree was clean after these commits; ttfx's separate WIP was
not included in this compiler cleanup. Branch/worktree references remain available.

## Guarded native-executor fast paths (2026-10-22, `efe625e5`)

Committed guarded, same-PC fast paths for primitive table get/put, in-range
array put, capacity-checked `buffer/push-string`, canonical core predicates,
and one-argument container-as-function reads (`(table key)`, `(struct key)`,
`(array i)`, tuples, strings, symbols, keywords, buffers). The container path
is exactly `bender_in`; composite keys and out-of-range indices remain VM
boundaries. Table/struct/string/tuple allocation opcodes are now admitted as
VM boundaries, so functions containing literals can still carry frame
executors.

Safety evidence: `test/suite-native-executor.sh` passes (budget, ASan/UBSan,
GC, breakpoint/step, parked marshal, preemption, differential parity); the
37-effect bounded CLI stream is 37/37 byte-identical. The release was built
from two fresh `decrypt --bench --seed 42` PGO collections (`home=1546`,
83 frame executors, 9 kernels) and `--release`.

Benchmarks are same-source release binaries with the container path on/off,
the same PGO cache/profile and the same 83 frame executors:

| Scope | Geomean | Median | Wins | Worst |
|---|---:|---:|---:|---|
| 37 effects, `--bench`, 5 rounds | **1.0979** | 1.0955 | 36/37 | `matrix` 1.000 |
| Five hot effects, no release PGO, 10 rounds | **1.13-1.16x** | - | 5/5 | - |

The comparison against the older `ttfx-cli-bender-gcR10` release (a
different PGO profile) was only 1.021 geomean across 37 effects; that number
is profile-confounded and is not used as this feature's gate. The clean
same-PGO on/off pair is the claim.

Remaining executor cost is now dominated by ~119k calls that still leave
native regions per sustained `scattered` run: `ctx-motion-move`,
`ctx-char-active?`, `ctx-seg-start`, `ease`, `ctx-add-frame`, and `color-fg`.
Next work is native-to-native call continuation/inlining and the GET/length/
divide boundaries that the container path exposed. No overall Rust parity
claim is made from this step.

## 2026-10-23 same-profile executor control and opt-in leaf executors

The earlier GUI container-vs-`gcR10` result was confounded: `gcR10` has
**zero** frame executors, while the container candidate had 85. To isolate
execution from code size and PGO, I rebuilt the GUI and CLI releases from
their exact existing PGO caches with `BENDER_NATIVE_EXECUTORS` unset. The
resulting controls have the same compiler, source, profile, and nine kernels,
but zero frame executors. Same-sitting paired runs:

| Pair | Metric | Geomean | Median | Paired wins |
|---|---|---:|---:|---:|
| CLI 83 executors vs 0, same PGO | `--bench` time ratio on/off (lower is better) | **0.9342** | 0.9348 | on faster 55/60 |
| GUI 85 executors vs 0, same PGO | windowed uncapped FPS ratio exec/noexec | **0.9901** | 0.9790 | noexec faster 16/25 |
| GUI 0 executors vs `gcR10` (different PGO) | FPS ratio noexec/gcR10 | **0.9710** | 0.9739 | `gcR10` faster 16/25 |

The CLI executor tier is a real **1.070x** same-profile speedup (6.6% lower
time); it stays enabled. The GUI tier is slightly negative even on the same
profile, and its 4.5 MB `native.c` / 2.8 MB binary versus 200 KB / 1.7 MB
for noexec makes the tradeoff worse. The noexec GUI recovers about two points
of the old 0.9538 candidate-vs-`gcR10` deficit, but is still 2.9% behind
`gcR10`; that residual is profile/source, not executor execution.

Profile-call coverage is thin at the definition level: 104,902 of 5,339,748
CLI rows (1.96%) and 72,132 of 12,136,582 GUI rows (0.59%) name a frame
executor. The hot CLI rows are mostly small helpers (`xor64`, `shr64`,
`shl64`, `cfunction?`, `nil?`, `ctx-char-active?`, `ctx-step-animation`),
and the GUI draw loop is dominated by the FFI alias `rl-draw-text-ex` plus
`EndDrawing`. Executors are selected for **all** structurally eligible
definitions; the final PGO collections cover only `decrypt` (CLI) and
`scattered` (GUI). Gating executor emission on those one-effect call counts
would remove executors that the six-effect CLI A/B shows are useful, so no
such gate was shipped.

### Leaf executor experiment (opt-in, not enabled in the final release)

I added a separate no-loop leaf tier behind
`BENDER_EXECUTOR_LEAF_MIN_CALLS` (default `math/inf`, i.e. off). It requires a
native run at entry and excludes FFI alias wrappers; a leaf wrapper around
void/struct-return aliases hung a GUI `highlight` probe, while the loop
executor's existing alias fast path already covers those calls. With a
threshold of 1000 it selected 16 CLI and 12 GUI leaf definitions.

Evidence: 37/37 bounded CLI frame streams identical; `test/suite-native-executor.sh`
passes with `BENDER_EXECUTOR_LEAF_MIN_CALLS=0` (budget 1/2, portable dispatch,
interrupts off, ASan+UBSan, GC across boundaries, breakpoint/step,
parked marshal, preemption); leaf executors `callee`/`reset`/`short-fn`
actually attach and execute. Paired leaf-vs-loop release runs gave 0.9846
time geomean across six effects (1.5% faster, 25/60 rows), but a 20-round
`decrypt` rerun gave 1.0002 (neutral). The GUI leaf A/B was inconclusive
(leaf/noexec 0.9604 versus leaf/loop 1.0201 across different sittings), so
the leaf tier remains opt-in and is **not** in the final release. A later
full 37-effect leaf-vs-loop release A/B (two rounds, complete bounded
dumps, startup included) gave a leaf/loop time geomean of **0.9978** (median
0.9910, leaf faster 25/37). That is not a material win, and `waves` was a
1.204 outlier; the leaf release was not promoted.

A more aggressive follow-up relaxed the entry rule to allow any native region
(not only one starting at PC 0) and admitted unused environment slots. It
selected 25 leaf definitions, including `ctx-char-active?`,
`ctx-step-animation`, `next-word`, and `next-word-boxed`, and passed 37/37
parity. Six-effect paired timing improved to 0.9787 geomean (median 0.9749,
32/60 rows), but `randomsequence` regressed to 1.1619 median (61.0 versus
52.5 ms); the new leaf wrappers around cfunction/native-kernel dispatch were
not robust across effects. That variant was rejected; its binary is retained
as `...-leaf-region-any-rel` for reproduction. The conservative 16-leaf tier
is the only one committed.

### Full-workload PGO experiment (rejected)

The release profiles above are one-effect (`decrypt` for CLI, `scattered` for
GUI). I tested whether a merged full-workload profile would improve type,
kernel, and layout evidence without changing the executor set.

For CLI I collected all 37 effects into one isolated cache
(`bender-cli-fullpgo-cache`), producing a release with the same 83 loop
executors and 9 kernels. It passes 37/37 bounded CLI parity. A same-sitting
six-effect paired A/B against the decrypt-only loop release gave a
fullpgo/loop time geomean of **1.0128** (median 1.0000, fullpgo faster
18/60), i.e. no win.

For GUI I collected the five target effects (`wipe`, `highlight`, `expand`,
`randomsequence`, `scattered`) into `bender-gui-fullpgo-cache` and built the
noexec release; its PGO gate passed and it has 0 executors / 9 kernels /
1.7 MB. A five-round paired A/B against the scattered-only noexec release
gave a fullpgo/noexec FPS geomean of **0.9859** (median 0.9659, fullpgo
faster 12/25), i.e. slower.

The merged profiles also produced expected type divergences across workloads
(for example `scene-new` and `filter` argument shapes) and did not change
executor count or code size. Both full-workload candidates were rejected and
retained as `bender-port/build/ttfx-cli-fullpgo` and
`bender-port/build/ttfx-gui-bender-fullpgo` for reproduction. The release
profiles remain one-effect; no full-workload PGO claim is made.

### All-37 effect survey against Rust

The five shared parity effects were only a slice of the workload. I measured
the current releases over all 37 effects against the Rust reference.

CLI protocol: 400-frame bounded dump of `ruby-port/logo.txt`, seed 42,
`COLUMNS=80`, `LINES=24`; the Rust `--parity-dump` and Bender
`--dump-frames` outputs were compared byte-for-byte, then three alternating
paired rounds timed the same complete dump to `/dev/null` (startup included;
Rust has no `--bench`). 36/37 effects are byte-identical; only `swarm`
differs. Bender/Rust time geomean is **23.242** (Bender speed **0.043x**);
on the 36 parity effects it is 23.491. Largest time ratios:

| Effect | Rust ms | Bender ms | Bender/Rust |
|---|---:|---:|---:|
| rings | 9.9 | 648.8 | 65.67 |
| binarypath | 7.8 | 490.7 | 62.97 |
| waves | 5.9 | 339.4 | 57.69 |
| vhstape | 8.0 | 416.3 | 52.03 |
| smoke | 5.1 | 229.6 | 44.97 |
| colorshift | 5.7 | 243.5 | 42.49 |
| thunderstorm | 6.3 | 263.6 | 41.77 |
| decrypt | 4.6 | 185.3 | 39.99 |

I also split the cached all-37 CLI profiles back into per-effect profiles
(37 records, mapped by collection order). The lag is not in the outer
effect frame functions, many of which are already loop executors. It is in
their callees and in build-time scene construction:

| Effect | Dominant profile rows | Effect-specific part | Native coverage |
|---|---|---|---|
| rings | `ctx-anim` 800k, `ctx-step-animation`/`ctx-char-active?` 512k, `ctx-motion-move` 504k, path/segment/lerp helpers 487k each | `rings-next-frame` 1.4k, `rings-set-disperse-path` 1.2k | only `ctx-path-step` and `rings-set-disperse-path` |
| binarypath | `nil?` 862k, `round-half-even` 517k, `empty?` 267k, step/active 248k, motion/path 230k | `binarypath.ben:82` 21.9k, `travel-complete?` 16.4k | `travel-complete?`; the 82 loop is VM |
| waves | active/step 190k each, `ease`/`scene-next-eased`/`round-half-even` 138k each, frame creation 74k | `waves.ben:71` 69.1k build-loop map, `while` at 70 3.2k | `waves-next-frame`; helpers VM |
| vhstape | `nil?` 924k, `xor64` 302k, `ctx-anim` 267k, RNG bit helpers 121-181k | `vhstape.ben:100` 85.4k, line 156 1.2k | no effect-specific executor |
| smoke | active/step 102k each, `color-clamp` 31k, frame creation 26k | `smoke.ben:73`/`:77` 12.8k each build-loop maps | `smoke-next-frame`; helpers VM |
| colorshift | active/step 209k each, frame creation 37k, `color-clamp` 10k | `colorshift.ben:103` 3.6k, line 73 1.2k | `colorshift-next-frame` absent from executor set; helpers VM |

Only five of the hot names (`ctx-path-step`, `waves-next-frame`,
`smoke-next-frame`, `travel-complete?`, `rings-set-disperse-path`) are in the
83 loop executors. The hot `ctx-char-active?`, `ctx-step-animation`,
`ctx-anim`, `ctx-motion-move`, `ease`, `nil?`, `empty?`, `index-of`, and the
64-bit RNG helpers remain VM calls. Several worst effects also spend heavily
in build-time loops (waves/smoke gradient-pair maps, binarypath collapse/wipe,
vhstape line completion), so per-frame executor tuning alone cannot close the
23x gap. Call counts are not time attribution; this is a target map, not a
cycle profile.

GUI protocol: 1458x400 windowed raylib, uncapped, `--show-fps`, seed 42,
same logo, 2 s probe; current noexec release versus Rust, two complete
rounds with per-run retries. Bender/Rust FPS geomean is **0.8458**, median
**0.8486**, with only `print` at or above Rust (1.010). The normally quoted
five-effect parity is therefore not representative of all 37: the weakest
effects are `rings` 0.648, `smoke` 0.719, `sweep` 0.721, `unstable` 0.748,
`binarypath` 0.749, `colorshift` 0.772, and `synthgrid` 0.789.

### All-37 GUI PGO experiment (rejected)

I collected all 37 GUI effects into one isolated PGO cache
(`bender-gui-all37pgo-cache`), then built the release from that cache. The
result has **0 executors**, **9 kernels**, and is **1.7 MB**; the compiler
reported `pgo parity ok` after verifying all 37 workloads. The merged
profile has 1,224 rows and the expected cross-workload type divergence
(`ctx-path-step` returns `[]` in some workloads and `[:tuple]` in others).

A two-round, 37-effect paired A/B against the current scattered-only noexec
release gave a PGO/base FPS geomean of **0.9992** (median **0.9930**; PGO
faster in 17/37 effects). The five target effects alone were 0.9938. The
outliers are not a broad win: `binarypath` +19.6% and `decrypt`/`crumble`
about +7%, but `fireworks` -10.0%, `synthgrid` -8.0%, and `unstable` -5.7%.
The candidate remains `bender-port/build/ttfx-gui-bender-all37pgo` for
reproduction; the release GUI stays the scattered-only noexec build.

The all-37 profiles identify where the remaining work is. CLI aggregate hot
rows are `nil?` 11.6M calls, `xor64` 10.0M, `round-half-even` 6.6M,
`cfunction?` 6.2M, `shl64` 6.0M, `ctx-step-animation` 5.6M,
`ctx-char-active?` 5.5M, `ctx-motion-move` 4.5M, and the other 64-bit RNG
helpers around 4M each. The GUI aggregate is dominated by the FFI alias
`rl-draw-text-ex` (44.8M) and a generated `boot.ben:4768` wrapper (92.3M);
the next Bender functions are much smaller (`nil?` 4.7M, `ctx-anim` 1.3M).
This is draw/FFI-call and VM-entry cost, not missing profile coverage. The
next compiler target remains native-to-native continuation/inlining for the
hot helpers and a lower-overhead batched FFI draw path; another merged PGO
profile is not the answer.

### Runtime sampling correction: the release is not the interpreter PGO profile

The PGO profiles above are collected by the interpreter. A 1 ms `sample`
of the current **AOT release** running `--bench` shows a different hot set;
the release already uses native kernels and executors, so interpreter rows
such as the RNG limb helpers are not the main cost.

| Effect | Samples | Dominant release stacks |
|---|---:|---|
| rings | 1,388 | `run_vm` 433; `bender_gc_barrier` 103; `bender_dict_find` 78; `bender_equals` 75; `ctx-fire-event` executor 68; `bender_in` 67; `bender_next_impl` 48; `bender_mark` 39; `bender_native_poll` 37; container-call fast 35; `native_structure_matches` 31; `ctx-path-step` 23; `ctx-frame-string` 16 |
| vhstape | 382 | `run_vm` 117; `bender_gc_barrier` 47; `bender_dict_find` 18; `bender_hash` 13; `bender_in` 13; `native_structure_matches` 12; `ctx-frame-string` 10; `ctx-path-step` 6; `ctx-fire-event` 6 |
| decrypt | 94 | `run_vm` 21; `ctx-frame-string` 9; `bender_dict_find` 8; buffer-push fast 7; `bender_core_string` 6; `bender_gc_barrier` 6 |

The hot release paths are therefore **table/event operations, GC write
barriers, VM excursions from already-native frame functions, and frame
string/color serialization**, not the boxed RNG helpers that dominate the
interpreter profile. `ctx-update`, `ctx-fire-event`, `ctx-path-step`,
`scene-reset`, `ctx-active-include`, and `ctx-frame-string` are already
executors; their cost is in their table/array accesses and their calls back
into the VM. This changes the next implementation order:

1. unbox scene/animation frame state from per-frame tables into packed
   arrays or typed records, eliminating the hot `put`/GC-barrier traffic;
2. stop executor calls to already-native state helpers from falling back to
   the VM (native-to-native continuation);
3. give the frame-string/color serializer a native vectorized path;
4. only then revisit interpreter-profile-only helpers such as the limb RNG.

A merged PGO profile cannot see this because collection runs before the
executor decides which calls stay native. Runtime sampling, not PGO call
counts, must gate the next change.

### Packed scene frames (accepted, first unboxing step)

The first runtime-sampling-driven change packs each scene frame from
`@{:sym :fg :duration :ticks}` into a four-element array. Scene frames are
written every tick by `ctx-step-animation` and cleared by `scene-reset`;
packing removes the per-frame hash-table `get`/`put` and table barrier from
that path. The scene API is unchanged, and ttfx effects do not access frame
fields directly. Parity is **37/37 byte-identical** to the previous CLI
release.

Built with the same decrypt PGO cache and 82 executors (the source change
removes one eligible executor), the three-round all-37 paired A/B against
the previous release gave a packed/base time geomean of **0.9744** (median
**0.9773**, faster in **29/37**). Gains are concentrated in `beams` 0.912,
`colorshift` 0.913, `waves` 0.918, `synthgrid` 0.932, `decrypt` 0.935,
`thunderstorm` 0.944, `vhstape` 0.956, and `smoke` 0.959. Remaining small
regressions are `matrix` 1.033, `overflow` 1.024, `bubbles` 1.018,
`slice` 1.017, `spotlights` 1.010, and `orbittingvolley` 1.007.

The candidate is `bender-port/build/ttfx-cli-packed-pgo`; the source change
is committed in the ttfx worktree as
`ttfx: pack scene frame state into arrays`. The previous release remains
`ttfx-cli-container-rel` until the next changes are combined and the final
release choice is made.

A follow-up changed `ctx :events` from a hash table keyed by character id
to a direct array. It also passed 37/37 parity, but the three-round all-37
A/B against the packed release was **1.0082** (median 1.0039, only 13/37
faster; `blackhole` 1.062 and `swarm` 1.061 regressed). That experiment was
reverted.

### Packed scene state (accepted)

Scene state (`:name :ease :sync :looping :all :queue :head :map :total
:estep`) was also a per-tick hash table. It is now a packed array with
index constants; effects do not access scene fields directly. Three-round
all-37 A/B against the packed-frame release: **0.9779** geomean, median
**0.9796**, faster in **32/37**. Largest gains: `waves` 0.917, `rings` 0.922,
`burn` 0.946, `decrypt` 0.947, `errorcorrect` 0.950, `unstable` 0.954,
`smoke` 0.957. Remaining regressions: `spotlights` 1.056, `scattered` 1.055,
`sweep` 1.007, `middleout` 1.004. Candidate:
`bender-port/build/ttfx-cli-packed-scene-pgo`; source commit
`ttfx: pack scene state into arrays`.

The combined packed-frame + packed-scene candidate is about **4.7% faster**
than the previous CLI release at the all-37 geomean level. This is still far
from Rust, so the next targets remain animation/path state and native
continuation.

### Packed motion-path state (accepted)

Motion paths (`waypoints`, `segments`, `total`, `step`, `max-steps`,
`hold`, origin, speed, ease) were also per-tick tables. They are now packed
arrays with small `ctx-path-*` accessors for the three effects that touched
path fields directly (`vhstape`, `unstable`, `orbittingvolley`). Three-round
all-37 A/B against the packed-scene release: **0.9893** geomean, median
**0.9895**, faster in **25/37**. Largest gains: `rings` 0.947,
`randomsequence` 0.952, `swarm` 0.960, `vhstape` 0.961, `laseretch` 0.970,
`rain` 0.972. Remaining regressions: `matrix` 1.025, `slice` 1.024,
`beams` 1.018, `unstable` 1.010. Candidate:
`bender-port/build/ttfx-cli-packed-path-pgo`; source commit
`ttfx: pack motion path state into arrays`.

The three accepted unboxing steps combined are about **5.8% faster** than
the original CLI release at the all-37 geomean level. This is not Rust
parity; the remaining budget is still dominated by VM excursions, table/GC
traffic in animation/event state, and frame serialization.

A frame-serializer experiment replaced the RGB-array color-cache key with
a packed integer RGB key. It passed 37/37 parity but regressed to **1.0285**
geomean (only 8/37 faster; `errorcorrect` 1.126, `sweep` 1.118,
`synthgrid` 1.087). The array-keyed cache was better and the change was
reverted.

### Final artifacts

- `bender-port/build/ttfx-cli-container-rel`: 83 loop executors, 9 kernels,
  2.8 MB, 37/37 bounded CLI parity against `ttfx-cli-rel2` (36/37 against
  Rust; `swarm` differs). This is the release CLI.
- `bender-port/build/ttfx-gui-bender-container-rel`: 0 frame executors,
  9 kernels, 1.7 MB. This is the release GUI; the 85-executor build is
  retained as `...-on-rel`, and the leaf candidate as `...-leaf-noffi-rel`.
- `bender-port/build/ttfx-cli-container-leaf-rel`: 99 executors (16 leaf),
  37/37 parity, retained only as an opt-in experiment.
- Rejected full-workload candidates: `bender-port/build/ttfx-cli-fullpgo`
  (83 executors, 37/37 parity against `ttfx-cli-rel2`, 1.0128 time ratio
  versus the one-effect loop release), `bender-port/build/ttfx-gui-bender-fullpgo`
  (five-effect profile, 0 executors, 0.9859 FPS ratio versus the one-effect
  noexec release), and `bender-port/build/ttfx-gui-bender-all37pgo`
  (all-37 profile, 0 executors, 9 kernels, 0.9992 FPS ratio versus the
  one-effect noexec release).

The measured next target is not another boundary fast path: it is an
environment-capable native function/leaf continuation for the hot
`ctx-*`/`ease`/RNG helpers. Full-workload PGO was tested and rejected above.
No overall Rust parity claim is made from this checkpoint.
