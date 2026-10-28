# Post-Phase-4 execution handoff

## Purpose and scope

Deliver a trustworthy worked example and an evidence-led decision about the next AOT compiler improvement for a representative game operation. This is not a request to maximize actor count, add a JIT, promise C++/Rust parity, or implement an optimization regardless of evidence.

Prepared against repository HEAD `04808694` (Phase-4 fix `0c935aeb`). Recheck HEAD before execution. This document is a plan, not evidence that its future steps have passed. Phase 4 is complete; do not repeat its full investigation.

**Success has two valid forms:** (A) one justified optimization with correct output and reproducible end-to-end benefit, or (B) a defensible NO-GO decision with ranked evidence and the next measurement needed. A NO-GO is not permission to choose an unrelated optimization merely to produce code.

## Non-negotiable facts and constraints

- Public `proc/spawn` now has a Janet continuation wrapper. C `proc/spawn-prepare` tries nonblocking mode3 first; only refused writes produce a plan. Do not restore awaiting in the value-returning C path.
- `test/suite-proc-spawn-return.janet` already tests forced backpressure, ordinary/tail calls, full GC, cancellation, PID fields and actor replies. Reuse it; do not create a duplicate.
- Mutable-capture placement has not yet received a focused regression through that wrapper.
- Local actor-wave completion marking still has residual quadratic growth. That is an observed limit, not automatic authorization for a collector redesign.
- A successful `--native` build does not prove the relevant function executed native code.
- Historical NX_VMEXIT-tagged counts are NOT all native-to-VM boundaries. Never divide total native operations by that partial denominator and call the result all-boundary yield.
- The existing `test/suite-proc.janet` failure at line605 is baseline-qualified, not green. Compare the actual error and location on each pinned baseline; a changed failure is new evidence.
- Cancellation after enqueue can leave an actor whose PID the caller never receives. Do not claim rollback semantics.
- No JIT, blanket native actor continuation implementation, default PGO/LTO changes, low-yield heuristic, or new packages in this scope.
- Keep existing type-hint facilities in mind: verify their current semantics rather than proposing a new annotation syntax.

## Working agreement for the executor

1. Start from the preflight below. Read this document and the linked design/evidence files, not the entire repository.
2. Use one implementer per file/domain. Use a separate reviewer for semantic changes. Parallelize read-only independent questions; serialize builds, profiling and timed runs.
3. Delegate narrow questions with a deliverable and file ownership. Do not choose a model override; use the configured lower-cost default. At most two active delegates unless a genuine independent third concern appears.
4. Stop a delegate and receive its final ownership handoff before taking over its files. Queuing a message does not interrupt its current turn. Never have two writers racing on one test.
5. Every command record includes cwd, argv, environment overrides, exit status and stdout/stderr paths. Do not infer success from a final printed line.
6. Commit each verified concern immediately with a conventional message. Stage explicit paths, never unrelated untracked content. Never reset/clean the shared checkout.
7. At each gate update `STATUS.md`: completed gate, commit, commands, results, unresolved questions and exact next action. The next model should resume there without reconstructing the conversation.
8. If a command/flag below is marked discovery-required, inspect its actual parser/header and record the discovered invocation. Do not guess flags, invent APIs or report an unexecuted test as passed.

## Phase 0 — establish a reproducible starting point

### Commands

Run from the intended checkout and retain output:

```sh
pwd
git status --short
git rev-parse HEAD
git log -5 --oneline
command -v make python3 clang
clang --version
```

Confirm the checkout is `/Users/pankajdoharey/Development/Projects/janet`, not the Anton application implementation. Existing unrelated untracked paths include ML/, assorted planning documents, erl_crash.dump and examples/five-million.janet; preserve them.

Create a unique evidence directory outside the source tree, and record its absolute path. Record baseline SHA before edits. For any comparative instrumentation/compiler change create separate baseline and candidate worktrees from that SHA; keep builds, caches and profiles separate. Example with explicit fresh names:

```sh
BASE=$(git rev-parse HEAD)
git worktree add --detach /tmp/janet-next-baseline-$BASE "$BASE"
git worktree add -b next-game-kernel /tmp/janet-next-candidate-$BASE "$BASE"
```

Check destinations/branch availability first; if occupied, choose a fresh name, do not overwrite. Use the workdir parameter rather than relying on shell cwd persistence.

Known build command, used successfully in Phase4:

```sh
make -j4 NATIVE_EXECUTORS=1 build/bender build/libjanet.a
JANET_LANES=0 JANET_NO_PROFILE=1 ./build/bender test/suite-proc-spawn-return.janet
```

When invoking direct Xcode Clang, explicitly obtain and record `SDKROOT` using `xcrun --show-sdk-path`; do not generalize this requirement to non-macOS hosts. For AOT builds set `CPATH` to that worktree's `src/conf`. Record compiler, runtime/library/source hashes and effective executable resolution.

### Gate 0

Pass only when baseline identity, toolchain, clean tracked state or explicitly accounted diff, fresh artifact locations and working narrow spawn test are recorded. Missing tools or baseline failures get a precise blocker report. Do not proceed with an unidentifiable binary.

## Phase 1 — close mutable-capture placement coverage

### Task

Add a small focused test beside the existing proc suites, or extend the existing canonical regression if setup remains comprehensible. This is a test-first change; do not alter runtime semantics to manufacture a pass.

Inspect `src/core/proc.c` around `janet_proc_find_mutable_capture`, `janet_proc_spawn_routed`, and `proc/pid?`; inspect the public wrapper in `src/boot/boot.janet`. Verify the exact local-handle predicate before writing assertions: `proc/pid?` accepts more than one handle shape and alone cannot establish local placement.

### Required cases

- Start worker lanes explicitly in a fresh process.
- Create a lexical closure capturing an actual mutable array/table. Do not rely on a global lookup masquerading as a captured value.
- Spawn it through public `proc/spawn`, have it mutate that captured object, and signal completion. Assert the parent's original object changed. Pair this with the source-verified local-handle/placement check.
- Verify omitted/default and explicit capacity paths, plus a tail-call wrapper if it can be added without obscuring the test.
- Force GC between spawn and observed completion; verify the capture and reply remain live.
- Include a capture-free or immutable-only control that genuinely routes with lanes active. Assert its routed handle and an actual reply, not merely successful creation.
- Run the test with lanes active, and a separate no-worker configuration for local behavior where applicable. Do not silently substitute a no-lane test for the active-lane branch.
- All blocking runs have an outer timeout; retain failure output. Cleanly stop/join lanes on success. Do not treat sleeping for a guessed delay as synchronization.

### Validation and commit

Run the new test and canonical backpressure regression on the current runtime. For a test-only change, confirm it passes without changing runtime/compiler files. Review whether it genuinely reaches the mutable-capture branch. If it fails, reproduce and classify before any fix; a real new bug is a separate scoped commit with baseline-fail/candidate-pass evidence.

Suggested commit: `test(proc): cover mutable-capture locality through spawn wrapper`.

**Gate 1:** observable alias-preserving local placement with active lanes, a routed control, GC coverage, bounded successful execution, independent test review and committed files.

## Phase 2 — produce a verified hybrid worked example

### Locate existing work before adding files

Inspect `docs/internals/ORCHESTRATOR-PLAN.md`, `docs/internals/trinexraft-workload-tasks.md`, `docs/internals/phase4-native-actor-feasibility.md`, and existing examples matching hybrid/kernel/batch. Use targeted glob/grep or a read-only delegate; do not read all examples end to end. Historical native continuation design lives at `docs/internals/native-continuation-design.md`; its proposals are not proof that every feature shipped. Record which existing example is reused and which gap it leaves.

### Design contract

Reuse an existing hybrid example if one exists. Prefer a representative batch operation such as chunk-density computation, transforms, or a measured simulation kernel. Actors own scheduling/lifecycle and exchange one job/result per batch. The kernel performs substantial bounded computation per invocation. Avoid rendering/window/driver dependencies in the correctness fixture.

The example must be understandable in one read: input generation, coordinator/worker protocol, kernel, output check and shutdown. Do not introduce a framework. Use existing language/runtime APIs and supported numeric/type representations. If the current compiler cannot natively execute the intended kernel, document that failure and choose a supported illustration explicitly labelled as such; do not silently weaken the real workload gate later.

### Deliverables

1. A deterministic example with fixed input/seed and adjustable batch size/count.
2. An independent scalar/reference implementation or a preserved trusted output oracle. Do not compare two paths that call the same potentially wrong kernel.
3. Exact output comparison for integer/byte results; explicit tolerance and NaN/infinity rules if floats are necessary. Define those rules before measurement.
4. Documentation showing the coordination/native-compute boundary, data ownership and mutable-capture implications.
5. A reproducible script to build and verify the variants, with explicit logs and nonzero failure exits.

### Build matrix

Verify current CLI parsing before adapting these known Phase4 invocation patterns:

```sh
./build/bender --aot PATH_TO_EXAMPLE -o OUT_PLAIN
./build/bender --aot PATH_TO_EXAMPLE --native --no-auto-pgo -o OUT_NATIVE
./build/bender --aot PATH_TO_EXAMPLE --native --pgo TRAINING_ARGUMENT -o OUT_RELEASE
./build/bender --aot PATH_TO_EXAMPLE --native --release -o OUT_RELEASE
```

`TRAINING_ARGUMENT` is workload-specific, not a magic iteration count. Confirm how arguments are forwarded and profiles selected. Keep plain/no-auto-PGO/release caches distinct and preserve training input/hash. Do not reuse the actor-wave profile for an unrelated game kernel. Training runs and instrumented runs are never included in release timing samples.

### Prove execution, not just compilation

For the intended kernel map source/function identity to generated native function/region and runtime counters from the same build/run. Preserve generated-code or compiler-report evidence and demonstrate nonzero native entry/work on that kernel. Report VM fallback explicitly. Keep instrumentation-on correctness/coverage separate from instrumentation-off timing.

Measure a small and a useful batch size to expose overhead; do not optimize batch size using the held-out validation set. Report coordination cost and end-to-end cost separately if measured. No universal speed claim from this pedagogical example.

Suggested commit: `feat(examples): add verified batched native game kernel` followed by a documentation/evidence commit if logically separate.

**Gate 2:** all variants produce correct results, native execution of the named compute kernel is established, actor placement is stated only where proven, build/run instructions work, and the example is committed. A native refusal is a documented gate failure, not a native success.

## Phase 3 — validate census instrumentation before trusting it

### Scope

Recover the existing Task1/native-retirement implementation and tests from CURRENT source and committed plans. Do not reimplement instrumentation from stale memory. Establish whether it is already landed, incomplete, or confined to another worktree. Record commit ancestry and never merge an unknown patch wholesale.

### Verified current entry points (read-only inspection at `04808694`)

- `tools/loop-executor.janet:162`: `(defn emit [id d &opt cont-pcs])` generates native executors; compile-gated per-ID/per-PC counters and `NX_RETIRE` near170–174/205–207; pure-operation credits near605–611.
- `tools/callgraph.janet:5246`: `(defn emit-native-lib [outdir])` assembles generated native.c, invokes the executor generator near5654, and integrates the work-report template near5782–5789.
- `tools/work-profile-template.c:23`: `janet_work_report` emits schema-1 work telemetry, **not** a retirement census. Do not replace its semantics silently.
- `test/suite-native-executor.sh:118–143` covers budget1/2, portable, census, disabled and sanitized configurations; census uses `-DJANET_NATIVE_RETIRE_CENSUS`.
- `test/suite-forced-quantum.sh` creates private scratch, hashes generated C/executable and compares same-seed traces. Fixture success does not establish real-workload forced-interrupt parity.

Exact existing commands, inspected but **not executed while preparing this plan**:

```sh
make NATIVE_EXECUTORS=1 test-executors
make NATIVE_EXECUTORS=1 test-forced-quantum
```

The second is the narrower forced-quantum target; avoid rerunning it redundantly if the broader target already produced complete retained evidence. Inspect Makefile prerequisites and timeout/resource behavior first. Exact TrineXraft replay CLI is discovery-required; do not fabricate one.

**Current starting state and unfinished gate:** production TLS per-ID/per-original-PC counters exist, but retirement coverage is partial; the fixture-local hook is not the production census. Gate3's inclusive numeric-opportunity bound is currently unavailable. Complete explicit post-commit credits in `emit`, and unresolved/overflow reporting in a separate census reporter integrated through `emit-native-lib`. Preserve schema-1 telemetry.

Define the opportunity population conservatively as numeric producers **and transports**, divided by same-run retired native original-PC instructions. Never substitute NX_TICK, schema-1 native_ops, generated boundary exits, interpreter dispatches or CPU time. Unclassified CALL, branch, poll, PUT, fused or unknown paths require `coverage=incomplete` and `gate=unavailable`; an uninstrumented zero does not mean zero retirement. Relevant saturation must be sticky and make the bound unavailable unless a formally conservative treatment is proved. Documented exclusions may explain partial telemetry but may not authorize a numeric stop/go ratio.

### Counter contract table (mandatory)

For every proposed metric write: name, increment site, semantic event, scope (process/thread/function/PC), reset/aggregation behavior, saturation/overflow policy, and exclusions. Distinguish:

- attempted native entry vs accepted entry;
- attempted instruction vs successfully retired instruction;
- native work vs VM work;
- generated/tagged exits vs all native-to-VM boundaries;
- one region's operations vs all function operations;
- source identity vs unstable per-build function IDs.

A ratio is permitted only when numerator and denominator describe compatible events in the same run. Do not combine training PGO counts with release census counts. State partial coverage as partial.

### Correctness fixtures

Run existing exact-retirement/per-PC tests first. Require normal completion, branch/loop, rejected guard/fallback, call and return, error path, and forced tiny-quantum/resumption where supported. For a failed speculative native operation, establish which implementation actually retires it so it is not counted twice. Verify totals reconcile with per-ID/per-PC sums within documented exclusions. Check disabled instrumentation leaves semantics unchanged.

Compare candidate and baseline using identical inputs and byte-exact output where meaningful. A candidate built `--native --no-auto-pgo` proves correctness only for that mode; do not label it PGO-enabled evidence. If profiles or instrumentation change IDs, join by recorded source mapping, never numeric ID alone.

### Gate 3

Pass only with a documented counter contract, deterministic fixture results, independent semantic review, reproducible pinned artifacts and zero unexplained discrepancies. If unavailable or unsound, stop the performance decision and repair instrumentation in a separate concern. Do not infer heat from static complexity or from counters whose meaning is unknown.

## Phase 4 — choose and measure one representative game operation

### Select before optimizing

Read the current workload task document and native continuation/census plans. Select one bounded, deterministic operation with genuine game relevance and a correctness oracle. Prefer an existing headless fixture over a newly invented microbenchmark. If the real program requires GPU/window context, extract the same pure operation without altering its algorithm; document precisely what was excluded. Do not use startup/build time as a substitute for steady operation time.

Write a workload contract: source revision, input size/seed, expected result, operation boundary, actor/batch configuration, warmup, training set, held-out validation set and resource cap. Distinguish reference implementation from baseline implementation. Keep data and algorithm identical across variants.

### Measurements

1. One bounded correctness smoke per variant.
2. One instrumentation run for census/coverage (more only if determinism or variability requires it).
3. One bounded profiler sample only if census cannot attribute an important cost.
4. Instrumentation-off paired/rotated timing: minimum5 matched samples per variant after recorded warmup; extend to9 if close/noisy. Serialize trials. If dispersion still overlaps the alleged improvement, report inconclusive rather than running indefinitely.
5. Retain all samples, including failed/capped runs; exclude them from valid-latency statistics with explicit reasons, not silently.

Start conservatively: 10-second/1-GiB sampledRSS bounds for a smoke run where feasible. Choose and document a larger cap only if the selected workload inherently requires it; do not silently escalate to million-actor or unbounded runs. Enforce timeout around the actual child/process group you own; preserve actual exit status. A runner kill is not evidence of OS OOM. Sampled RSS is not an exact allocator peak.

Report full operation latency and relevant kernel latency, medians and spread, correctness, native coverage, fallback attribution and memory. Include build time separately, not inside a claimed steady-state speedup.

**Gate 4:** one reproducible workload with trusted counters, correct outputs, stable provenance and a ranked cost/opportunity table. If the workload is dominated by unrelated I/O/GC/coordination, say so; do not force it into an unboxing project.

## Phase 5 — evidence-led Task 2 decision

Produce a short `decision.md` with these rows:

| Candidate | Dynamic evidence | End-to-end cost share | Proposed change | Semantic risks | Verification | Decision |
|---|---|---|---|---|---|---|
| Unboxed locals / fewer boxing transitions | Measured per-ID/per-PC evidence | Measured or explicitly unknown | One bounded region/type path | GC roots, guards, joins, fallback, resume | Named fixtures | GO/NO-GO/INCONCLUSIVE |
| Dominant native refusal/fallback | Exact current reason and hot PCs | Measured or unknown | One supported extension | Error/side-effect/retirement parity | Named fixtures | GO/NO-GO/INCONCLUSIVE |
| Data layout / batching / runtime cost | Actual profile or ablation | Measured or unknown | Existing extension point | Ownership and workload parity | Named tests | GO/NO-GO/INCONCLUSIVE |

### Existing Task 2-specific constraints

Read `docs/internals/ORCHESTRATOR-PLAN.md:598–601` and `docs/internals/trinexraft-workload-tasks.md:259–285` (line numbers are snapshot hints, recheck current text). The former records only four actual mixed-slot definitions in the fresh refusal census and a STOP on generalized SSA. Do not turn this plan into authorization for generalized SSA.

Task2 additionally requires per-PC numeric type **and liveness** proof, and forced tiny-quantum materialization/resume parity on the actual hot workload, not just synthetic fixture success. Once the inclusive conservative opportunity bound is valid, a bound below approximately5% of dynamic retired native original-PC instructions is the existing NO-GO stop criterion. This opcode opportunity threshold is **not** the separate end-to-end performance acceptance threshold below. Missing bound or missing proofs means INCONCLUSIVE, not GO. If the gates clear, limit the first prototype to the existing narrow guarded counted-loop induction path with interrupt spill/reentry and IEEE edge-case checks.

Do not assume Task2 means unboxed locals is necessarily next. Before GO require: a hot measured opportunity, correctness specification, bounded implementation surface, independent reviewer agreement, and a way to test the claimed mechanism separately from incidental timing noise. Where possible estimate the upper bound from the removable fraction (Amdahl's law); do not invent that fraction from opcode counts alone.

Predeclare a practical acceptance threshold after baseline variance is known and before implementing. A reasonable default is a repeatable >=5% end-to-end median benefit with paired results clearly above noise, no correctness failures, and no material memory/startup/regression cost outside an explicitly accepted tradeoff. This is a project decision threshold, not a statistical confidence claim. A larger kernel-only gain with negligible operation benefit does not meet the end-to-end objective.

**NO-GO:** publish evidence and stop optimization work. **INCONCLUSIVE:** name exactly one missing measurement and its bounded cost; do not open a speculative redesign. **GO:** proceed to Phase6 for one target only.

## Phase 6 — conditional implementation and independent validation

Use the candidate worktree and retain an untouched baseline. Implement one measured target; keep instrumentation changes, semantic changes and evidence documentation in separate atomic concerns.

Before edits trace real callers/impact. For typed/unboxed changes specify entry guards, representation, joins, overflow/numeric semantics, error side effects, GC root visibility, native/VM handoff and resume state. Preserve exact retirement semantics; do not fix counters to make an optimization look better. Read existing typed/compiler facilities rather than adding redundant syntax.

Test smallest adversarial fixtures first, then existing native/forced-quantum/GC tests that reach the changed path, then the selected real workload. Run API/quality-delta checks; static map gaps for Janet are not test passes. A semantic reviewer must inspect the actual diff independently and identify untested assumptions.

Rebuild all measured artifacts from committed source, instrument off for timings, with matching toolchains/flags except the intentional change. Repeat the predeclared paired experiment on held-out inputs. Keep rejected outcomes. If the target fails correctness or benefit, do not land it as a performance improvement; retain an isolated experimental commit if useful and document rejection without altering unrelated work.

**Gate 6:** independent approval, required tests green or exact baseline-qualified unrelated failures, held-out correct output, demonstrated predeclared benefit, explicit limits, atomic commits and clean owned tracked changes. Only then update user-facing performance claims.

## Phase 7 — final handoff

Deliver: commit list; current status per gate; runnable example; one-page workload/counter/decision summary; raw evidence manifest with hashes; reproducible commands; baseline-qualified failures; accepted/rejected optimization result; remaining risks and the single best next task. Do not report pending gates as complete.

## Evidence layout

Use a unique versioned directory (do not overwrite Phase4 evidence):

```
bench/results/post-phase4-<baseline-short>/
  README.md                  # result and limits, not a transcript
  manifest.json              # source/toolchain/binary/input/profile hashes
  commands.jsonl             # cwd/argv/env/status/output references
  STATUS.md                  # exact resume point
  locality/                  # focused test evidence
  hybrid/                    # oracle, native mapping and build checks
  census/                    # contracts, fixtures, mapping, raw counts
  workload/                  # all timing/correctness samples
  decision.md                # GO / NO-GO / INCONCLUSIVE
  candidate/                 # only if GO
```

Commit small useful summaries/fixtures. Archive large raw logs only after byte-verifying every member and recording archive SHA256; avoid committing generated binaries unless repository policy requires them. Keep missing logs marked missing; never reconstruct claimed raw evidence from prose.

## Executor starting prompt

> Execute `docs/internals/post-phase4-execution-handoff.md` from Phase0. Treat it as a gated plan, not a requirement to produce an optimization. Phase4 is complete; preserve its findings and unrelated files. First confirm current HEAD and baseline, then implement and independently review the mutable-capture regression, then the verified hybrid example. Validate the existing per-ID/per-PC census before using it to select a representative game-kernel target. Stop at a documented NO-GO or concrete blocker; never silently change workload or counter semantics. Use narrow delegates with exclusive file ownership, one measured run at a time, bounded processes, exact output assertions and separate instrumentation/timing builds. Commit each verified concern atomically. Maintain STATUS.md with evidence paths and the exact next action after each gate. Do not restart Anton, add JIT support, enable PGO/LTO globally, or redesign GC as part of this scope.
