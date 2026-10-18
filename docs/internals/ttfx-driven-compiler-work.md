# General Janet optimization driven by ttfx

Updated 2026-10-18. **This is the authoritative compiler work/status record.**
The original repository is `/Users/pankajdoharey/Development/Projects/janet`.
Application status belongs in the ttfx repository's
`janet-port/docs/optimization-status.md`; measured results belong in its
`bench-engines.md`. Temporary logs/binaries are supporting evidence only.

## Goal and correctness contract

Optimize the general Janet compiler/VM, not effect-specific kernels. Unmodified
ttfx is the workload: use its `ruby-port/logo.txt` Omarchy artwork (81×10), not
hello or the historical repeated-fox input. Train with seeds 42/7/99/2026 and
reserve 13/314159 for held-out parity when all four train PGO.

The requested goal is at least 1.10× Rust and Node on every comparable benchmark.
**This goal is not achieved.** Match workload and semantics; preserve frame bytes,
GUI appearance/tick cadence, numeric behavior, GC, debugger behavior and exact VM
quantum/suspension ordering. Call counts, emission and successful registration
are not performance measurements. Use focused matched timing only after correctness.

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
These commits are available in this repository's branch history but have **not**
been merged into this shared master checkout or installed as the user's compiler.

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

Implementation is in progress on the isolated integration branch; not validated:

1. Native arithmetic/control regions with calls, pushes, lookups, iteration,
   mutation, allocation and unsupported operations handed to the VM at original PCs.
2. Opaque constants loaded from the live runtime pool into scanned slots; no
   compile-process addresses or assumptions about mutable contents in descriptors.
3. Validated attachment to actual imported functions, distinguishing emitted,
   registered and executed regions. No bare-name or hash-only attachment.
4. Counters for actual native operations/regions and VM exits, not just entries.
   Whole-function entry validation may overwhelm short regions; do not weaken
   mutation/debug checks merely to produce a favorable timing result.

Real fixture blockers include CALL/TCALL, collection operations, pushes and
allocation in `ctx-update`, `ctx-frame-string`, `ctx-path-step` and Bezier work.
These names identify test coverage, **not compiler special cases**. Exact poll
semantics remain mandatory; budget1 currently repeats the same unexecuted backedge.

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
In particular, shared Janet `implementation_plan.md` reports another agent's
accidental partial overwrite: its owner must reconstruct/review it before merging.
Retain worktree/branch references until integration and preservation are verified.
