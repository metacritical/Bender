# Isolated collector candidate: no reclaimable young objects

Status: **landed as `f82d0642` after independent source review and focused regression checks**. Base35671ede; isolated worktree `/tmp/janet-wave-candidate-35671ede`; measured patch `collector-candidate.patch`. Production differs only in explanatory comments and includes the separately committed null-process correctness fix.

## Hypothesis and scope

Completion minors repeatedly mark a large reachable process/closure graph. The existing proc sweep can reclaim only non-disabled, young, same-owner, non-fiber blocks on the process fiber's three heap lists. Nonempty lists need not contain any such block, because ordinary proc construction shares slab pages that become mixed-owner0.

The first attempted guard (all three lists empty) showed no material gain: local N1000/2000/4000 wave96/403/1617ms. It was not accepted as a useful remediation.

An isolated diagnostic on local N2000 counted2000 scoped collections,4000 listed blocks,166 blocks satisfying young/same-owner/non-disabled eligibility, and **zero empty lists**. This diagnostic is evidence about sweep eligibility, not a timing sample. It was removed from the measured candidate.

The second candidate scans the exact sweep-eligibility predicate before root marking. If no block can be reclaimed, it clears epoch stamps on non-disabled listed objects (the non-reclamation sweep behavior), drains pending lists and runs the existing heap verifier, resets nursery accounting and returns. Global fallback paths for owner0/stuck remembered sets run first and are unchanged. Any eligible block retains the original mark/sweep path. No root traversal depth is capped and no remembered-set entry is dropped.

Skipping an epoch increment and threaded-abstract visit-cycle changes still require explicit independent review. Passing the benchmark alone does not establish collector soundness.

## Matched before/after evidence

`evidence.tar.gz` retains315 files: every trial stdout/stderr, run provenance, distributions, build hashes, initial candidates, eligibility diagnostic and regression outputs. Every archive member was verified against the original file. SHA256 `3ee0bcfd48e973ea84636b0bff19e4be8f451fcd7cc9b1186a4e462b07766617`.

96 sequential trials: baseline/candidate × script/plain/native-noPGO/release × lanes0/2 × N1000/2000 ×3repetitions. All96 returned result=N and exited0. The runner uses explicit absolute executables; profile and diagnostic env removed. This batch groups arms rather than alternating every trial; a later rotated confirmation is required before performance acceptance.

Local **wave-phase** medians in ms:

| N | Mode | Baseline | Candidate |
|---:|---|---:|---:|
|1000|script|105|8|
|1000|plain AOT|278|23|
|1000|native-noPGO|289|22|
|1000|release|277|22|
|2000|script|407|32|
|2000|plain AOT|766|61|
|2000|native-noPGO|764|62|
|2000|release|757|61|

The candidate substantially reduces local wave time but **does not eliminate quadratic growth**. Remaining collections with eligible blocks still traverse the transitive graph. Two-lane timings remain roughly similar/noisy at these sizes; no cross-lane speedup or Go superiority is established. Some fresh-executable subprocess startup outliers around0.5s are retained in external wall measurements, not discarded.

## Regression results so far

- candidate suite-ev:776/776 pass;
- suite-ev2:13/13 pass;
- suite-fiber-capture:7/7 pass;
- suite-process-heaps:28/28 pass;
- suite-lanes:all assertions pass, including density milestones200k/400k/600k and stop/join after640k parked units;
- suite-proc:exit1 at test605 (strict-proc escape check). **Untouched baseline fails at the same test with the same error**. It is not reported green or excused as a candidate success.

These runs precede helper extraction but execute the same candidate eligibility/cleanup logic without the diagnostic env enabled. Clean helper-extracted candidate builds successfully and was used for the96trial matrix. Broader GC, pending/weak epoch and sanitizer coverage remain acceptance obligations.

## Landing decision and remaining limits

Independent review and focused real-abstract fixture completed; production committed as `f82d0642`. See `docs/internals/phase4-no-reclaim-minor-review.md`. The fixture covers empty, disabled, malloc-large old, eligible-young and repeated skipped/real collections, plus a callback probe reachable solely from the process exit reason across a later major. Production fixture and companion null-process regression pass.

Additional candidate checks: gc-minor12/12, gc-stress25/25, GC trace-cache C harness, forced-quantum replay and foreign-VM threaded-abstract ownership C harness all pass. Alternating baseline/candidate local confirmation gave N1000 median100→9ms, N2000 median416→33ms, all12 trials correct with exit0. This confirms the gain without resolving the remaining quadratic path, the residual script/AOT graph difference, or the baseline process-suite failure. No Go superiority claim follows.
