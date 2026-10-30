# Retirement census pre-implementation audit

Read-only domain audit against source baseline3ae1e1a8; subsequent main commits c1a91aed/5d51f276 add only baseline evidence. No compiler/runtime edits or census tests executed for this audit.

## Existing counter contract

`tools/loop-executor.ben:170–174,205–207` emits default-off thread-local uint64 counters per generated executor ID/original PC. Per-PC counts saturate at UINT64_MAX; the sticky overflow flag is set on the next attempted credit after the count has reached that maximum, not on the increment that first reaches it. There is currently no reset API, cross-thread aggregation, standalone census reporter or durable source-to-ID mapping.

Current credits: fused numeric-sum PCs2–6 (near228–253), post-store MOVN/MOVF/LDC/LDI/LDN/LDT/LDF (near605–611). Fixture-local retirement hooks are separate and do not prove production coverage. NX_TICK can precede rejected guards; it is not retirement. `tools/work-profile-template.c:23–88` is schema-1 work telemetry, not this retirement census.

## Unclassified inventory

| Paths | Source hint | Problem requiring proof |
|---|---|---|
|IN/INIM|277–296|Successful vs rejected guards|
|GET/GETI|300–356|Return, miss, error and fallback ownership|
|PUT|367–376|Effect followed by unwind/panic|
|Pushes and scratch construction|383–407|Exact commit point and failure|
|CALL alternatives/continuations|414–496|Status0/1/2 and caller/callee retirement ownership|
|NEXT|501–518|Branch/iteration and failure semantics|
|EQ/NEQ/arithmetic|528–553|Successful result vs speculative failure|
|Bitwise/shifts|557–589|Result commit and guard failure|
|Branches/backedge polls|590–603|Taken/untaken, interruption and resume ownership|
|NOOP|612|Explicit successful retirement credit|
|VM-owned/unknown boundaries|520,604,613|Do not count a precommitted PC as successful native retirement|

Fused PC7 remains VM-owned. Line numbers are snapshot navigation hints, not stable APIs. Unknown or uninstrumented zeros are not observed zero retirement.

## Gate result

`coverage=incomplete`, `gate=unavailable`. No numeric opportunity percentage is justified. Task2 additionally needs conservative numeric producers+transports over same-run retired original-PC operations, per-PC type and liveness proof and actual workload forced-interrupt parity. Existing synthetic forced-quantum checks do not establish the latter.

## Bounded next instrumentation concern

A separate compile-gated reporter can expose existing partial counts, sticky overflow, source mappings and an explicit unresolved inventory through `tools/callgraph.ben` native-library assembly (emit-native-lib near5246, generator invocation5654, report integration5782–5789). Keep schema1 unchanged and report incomplete/unavailable, rather than implementing guessed credits across every path. Follow-up post-commit credits must be independently proven per operation family and atomically reviewed.

Tests required for any future completeness claim: successful/rejected guards, both branch outcomes, poll/resume, CALL statuses0/1/2, PUT mutation/panic, sticky saturation, default-off absence and reporter output. `make NATIVE_EXECUTORS=1 test-executors` is the existing broad suite; it does not by itself prove real workload interruption parity.
