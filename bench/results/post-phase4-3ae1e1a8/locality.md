# Phase1 mutable-capture locality regression

Landed `a6282fb1`, with final PID-recognition assertions `2fe32108`, test-only. Runtime/compiler unchanged from baseline3ae1e1a8.

`test/suite-proc-capture-locality.ben` uses factory parameters as real lexical captures of tables/arrays. It asserts local `:core/proc` handle identity and alias-visible mutation while worker lanes are active; a threaded-channel-only captured control produces a routed PID tuple and actual reply. `proc/pid?` is checked on both shapes but is not used as the placement discriminator.

Table/array actors give a ready signal, then park on a gate. Parent waits for ready before forcing GC and releasing the gate. The test also covers omitted/explicit mailbox capacity and a tail-call spawn wrapper. Two independent completion values are compared without assuming scheduler order. Zero-worker and active-worker arms are separate runs; BENDER_LANES=2 selects the active arm, which explicitly starts one worker. That is one worker tested, not a two-worker coverage claim. Successful runs stop/join their lanes; a failing assertion is bounded by the external timeout, not guaranteed in-test cleanup.

Final parent verification on the pinned baseline binary: both BENDER_LANES=0 and2, BENDER_NO_PROFILE=1, exit0 within10s; exact PASS stdout and phase stderr retained in locality-final-lanes*. Runtime hash remained71d75d31b8581249ba6da93278c80e0db84e309e70575fcf910f28d352340ba7. Independent source review approved the corrected handshake, unordered completions and placement predicates before commit.

## Rejected draft results retained

The first draft referred to a top-level global mutable object rather than a lexical capture, and therefore routed; this was a test construction error, not a runtime regression. A later draft ran collection before the actor started rather than while parked, and assumed completion FIFO. Review found both; final source corrects them. Draft logs and repeated smoke runs are retained for transparency, not treated as performance samples or separate coverage proof.

`locality-validation.tar.gz`:142 raw files/logs including failed drafts, command records, final parent results and paused-final checks of the four added PID assertions, every member byte-verified. SHA256 `33839e8f128bb2c0b47529fad5a8625965f4c6b466946fce7b631aedcc824227`.

Plain-channel-only captures across lanes remain outside this test's safe threaded-control contract. No claim that all abstract values are shareable across VMs follows from its placement predicate.
