# General Janet optimization driven by ttfx

Updated 2026-10-21. **This is the authoritative compiler work/status record.**
The original repository is `/Users/pankajdoharey/Development/Projects/janet`.
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

Optimize the general Janet compiler/VM, not effect-specific kernels. Unmodified
ttfx is the workload: use its `ruby-port/logo.txt` Omarchy artwork (81×10), not
hello or the historical repeated-fox input. Train with seeds 42/7/99/2026 and
reserve 13/314159 for held-out parity when all four train PGO.

Do not add custom kernels, benchmark-specific shortcuts, source reshapes, or
optimization tracking documents to `janet-port`. Existing handwritten application
kernels are historical inputs to the fixture, not templates to special-case in
Bender. Any new mechanism must be tested with generic Janet programs first and
must apply without changing the application's source.

The requested goal is at least 1.10× Rust and Node on every comparable benchmark.
**This goal is not achieved.** Match workload and semantics; preserve frame bytes,
GUI appearance/tick cadence, numeric behavior, GC, debugger behavior and exact VM
quantum/suspension ordering. Call counts, emission and successful registration
are not performance measurements. Use focused matched timing only after correctness.

Latest verified CLI release comparison:
[Omarchy AOT/PGO versus Rust](omarchy-cli-release-comparison.md). Full output
parity passes, but the rebuilt release remains within noise of previous Janet;
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
`build/libjanet.a` was refreshed, with experimental executors default-off.
Array-result 16, joint-dispatch 28, hinted-export 78, multiseed 35, release 14,
and auto-PGO 98 assertions passed (269 total), plus hinted-callgraph checks.

## Implemented and validated

### Native result composition

Array-returning native calls can assign to proven, join-safe array slots even in
a double-returning caller. Direct-call argument proof occurs before a reused
destination register changes type. Tail return and mixed-slot checks remain.
This passes `JanetArray*`, **not unboxed array elements**. Regression: 16 checks.
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
default** (same double opt-in: `-DJANET_NATIVE_EXECUTORS` runtime build
plus `JANET_NATIVE_EXECUTORS=1` at emission). Mixed-op loop bodies are
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

Shared-tree inventories initially recorded 11 Janet and 202 ttfx dirty/untracked
entries; a subsequent SHA-256 comparison found them unchanged. Review source,
tests/docs, generated artifacts, CI/ignore edits and ownership independently.
At the user's request, remaining original-checkout WIP was then organized into
feature commits rather than blanket-staged:

| Commit | Preserved work |
|---|---|
| `ebb83566` | Archive the unrelated partial recovery at `recovered/role-subagents-plan.partial.md`; restore the tracked Janet plan |
| `151bdc24` | Ignore Finder metadata without deleting it |
| `5ada1e33` | Historical combined compiler/messaging roadmap |
| `092269e1` | Historical DCE safety-gate/stripping plans |
| `6a3f59a2` | Type-generator design, with historical-scope corrections |
| `6f2da236` | Historical workload diagnosis, not an application-optimization mandate |
| `8bee3ee1` | Manual pool probes and byte-preserved archived proc snapshots |

The original partial plan cannot be fully reconstructed; its fragment and recovery
note remain preserved, not endorsed as current instructions. Both manual pool
probes exited 0 under a timeout. Historical plans include provenance/status warnings.
The original Janet worktree was clean after these commits; ttfx's separate WIP was
not included in this compiler cleanup. Branch/worktree references remain available.
