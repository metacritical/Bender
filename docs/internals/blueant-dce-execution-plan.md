# B7.2 Part B: Safe Whole-Program DCE + Supervision-Tree Roots, and B7.3: M:N Scheduler Investigation

> Historical execution plan, reviewed against `5180da8a` on 2026-10-18.
> Pieces 1a/1b/1c landed as `edbeeb67`, `6cade6a0`, `a5c66600`; implementation
> imperatives and “no tree-shaking today” statements below are historical.
> Conservative end-to-end stripping remained a no-op; synthetic exclusions proved
> plumbing, not general pruning. Supervision-root Piece 2 remains a proposal.
> The GC-independence prerequisite in [b7.3-mn-scheduler-design-note.md](b7.3-mn-scheduler-design-note.md)
> supersedes the original “independent/unblocked” scheduling description.
> Old workspace/no-commit instructions below are archival, not current directives.

Full design rationale lives in `docs/internals/b7.2-part-b-design-note.md`
(this plan summarizes it for execution tracking). Two independent tracks,
safe to run in parallel on disjoint files.

## Track 1: B7.2 Part B — DCE (tools/callgraph.janet, tools/mkimage.janet)

Confirmed by direct code reading: `mkimage.janet` never consults
`callgraph.janet`'s `reachable`/`tainted` analysis. There is no
tree-shaking anywhere in this toolchain today — only native-kernel
candidate selection and diagnostics. Building real DCE is therefore a
new safety-critical system, not an extension of an existing one.

### Piece 1 (do first, build and prove in isolation): general whole-program DCE
1. A new, deliberately more paranoid reachability computation
   (`dce-reachable`), separate from the existing fail-soft
   `reachable-from`. Exhaustive disqualifying-pattern list (function
   stored in a container and invoked later; passed to a higher-order
   core function; resolved via `resolve`/`dyn`; exported/library-called;
   invoked only by the C host). ANY instance anywhere in the program ->
   strip nothing, program-wide. Unknown construct -> treated as
   disqualifying, never assumed safe.
2. An empirical cross-check harness: build with/without a candidate
   stripping set, run the FULL test corpus (every suite + ttfx) against
   both, assert byte-identical output. This is the actual safety net,
   not the analysis alone. The harness itself must be tested against a
   deliberately-wrong stripping set to confirm it correctly detects and
   fails on divergence.
3. V1 is allowed, and likely, to strip nothing for real programs —
   that's the correct conservative starting point, not a failure.
4. Opt-in build flag; hard build-time failure over silent wrong output
   whenever the analysis can't positively classify something.

Tests: harness self-test (catches a deliberately-wrong stripping set);
positive case (genuinely unused function, zero disqualifying patterns,
gets stripped, cross-check confirms identical behavior); negative case
(one disqualifying pattern anywhere -> whole build correctly strips
nothing).

### Piece 2 (only after Piece 1 lands and is verified): supervision-tree roots
Add the FIRST narrow exception to the container-taint rule: `sup/child`
calls with a bare top-level function reference at the `entry` argument
position (exactly `examples/supervisor.janet`'s own documented
contract) become precise roots instead of generic taint. Any
non-bare-reference `entry` contributes no root but must NOT fall back
to whole-program taint — only that one site stays generically taint-safe.

Tests: fixture with a supervision tree + an unreferenced function,
confirm the unreferenced one is actually absent from the built binary
(checked via nm/strings, not just a diagnostic line), cross-check
harness confirms identical behavior for everything kept. Negative:
non-bare `entry` expression contributes no root, doesn't trigger
whole-program fallback, function still ships correctly.

### Standing regression warning (carry into every future change here)
Part A found: `teardown-child`'s fixed 0.2s quit budget was too tight
for nested supervisors (whose own graceful shutdown recursively tears
down their own children before replying), causing an abrupt
`proc/kill` that orphaned grandchildren and hung the process on exit.
Fixed with a type-aware 3.0s budget for supervisor children. ANY future
change to supervisor teardown, nesting, or restart logic MUST include
an explicit 2-3-level-deep nesting test under a bounded timeout, the
same way `test/suite-supervisor.janet` test 5 now permanently guards
this. Do not let this class of bug recur.

## Track 2: B7.3 — M:N scheduler core / resident-proc migration (src/core/proc.c, src/core/ev.c)

Independent file surface from Track 1. Builds on the lanes machinery,
now unblocked since B5.1's crash fix (commit `1bb7aae5`). Not yet
scoped in detail — needs its own investigation pass (what does "make
proc migration explicit in M4b" concretely require, given the lanes
primitives that exist today) before any implementation, same
discipline as every other item.

## Sequencing
Track 1 Piece 1 and Track 2 run in parallel now (disjoint files).
Track 1 Piece 2 is strictly sequenced after Piece 1 lands, verified,
and committed. Every subagent dispatch gets the same workspace-hygiene
instructions (never touch implementation_plan.md, never commit,
confirm file-scope boundaries) and the same verification discipline
(full regression battery + dual-target + the specific new tests above)
established throughout this entire engagement.
