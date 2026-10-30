# Routed spawn return values across control-channel backpressure

Landed in `0c935aeb`.

## Defect and direct evidence

The routed spawn primitive used a blocking mode-0 control-channel push followed by `bender_await()` and only then constructed its PID. Await raises an event signal and longjmps out of the C function; it does not resume that C frame. The channel writer is later scheduled with the channel value. Consequently the public spawn returned `core/channel` on the parked path instead of a PID.

The per-spawn diagnostic at `bench/results/phase4-controlled-35671ede/spawn-return/` caught wrong returns at index15606(one lane) and65504(two lanes). The earlier all-threads-parked80k waves were therefore not sufficient evidence for a missed-wakeup diagnosis: a malformed intermediate chain handle is a concrete alternate cause. One-lane final-head corruption also produced an immediate invalid-PID error.

## Correction

- `src/core/proc.c` registers `proc/spawn-prepare` as an explicitly internal primitive, not a stable public API.
- Local placement and accepted steal paths still return their existing handles. The normal routed path first tries mode3, which never parks and returns0 on acceptance,1 when full.
- Accepted writes return the PID immediately. Full-channel writes return an internal `[:proc-spawn-write pid ctl message]` plan without awaiting.
- Public `proc/spawn` is a Bender wrapper in `src/boot/boot.ben`. It holds the PID and plan in its Bender frame, performs existing blocking `ev/give`, discards the channel-valued write result and returns the saved PID.

No new wake-value override, channel cross-thread protocol, busy wait, polling loop or message drop was introduced. The public call still waits for backpressure before returning. Mutable captures retain local placement; local/isolated primitives remain unchanged. The helper remains resolvable through normal C registry/image machinery; there is no new private-registration mechanism.

## Rooting and continuation review

`bender_tuple_n` and the slab allocator do not invoke collection; collection is polled at VM boundaries. Bare C locals remain valid through plan construction. After return, the plan and saved PID are Bender frame values. A parked nonterminal frame is scanned by `bender_mark_fiber`, including across full GC. Cancellation follows normal Bender-frame unwinding rather than C cleanup. No extra manually rooted state can leak.

The immediate try path matters: sending every routed spawn through `ev/give` would add steady-state plan allocations and reject nonblocking use inside `bender_call` earlier than the old path. Only the full-channel case now uses the blocking wrapper.

Independent reviews of the isolated implementation accepted these contracts and the internal-helper visibility boundary. Epoch/collector behavior was not changed by this fix.

## Deterministic regression

`test/suite-proc-spawn-return.ben` installs a gated worker via the **root fiber's environment** (startup reads that, not the current form-evaluation fiber's setdyn). It gates only worker lane1; the lane0 dispatcher still runs normally. Ready/gate/result channels are threaded. The test fills ctl to its reported capacity, accounting for the startup overflow descriptor already queued, so worker scheduling speed cannot avoid backpressure.

It then verifies:
- ordinary-call and tail-call spawns both park on the full channel;
- a full collection while parked retains their plans/PIDs;
- cancellation propagates its error rather than producing a successful PID;
- after releasing the gate, both returned PID tags, lane/control fields and distinct serials are correct;
- both spawned actors actually receive a message and reply via a threaded channel;
- an immediate spawn also returns a usable PID.

The old runtime fails the PID assertion; the corrected production runtime passes. The test is a correctness fixture, not a timing benchmark. Cancel-after-enqueue may still leave an actor whose PID the caller never learns; this is pre-existing buffering/cancellation semantics, not a rollback guarantee.

## Verification and limits

Production per-spawn80k checks pass for lanes1 and2, with result80000 and exit0. Regression suites ev776, fiber-capture7, process-heaps28, gc-minor12, gc-stress25 and lane suite including640k parked stop/join pass. `suite-proc` still exits1 at its existing strict-proc escape test605, matching baseline; **not reported green**.

This fixes the demonstrated wrong return. It does not guarantee absence of every channel race, infinite-scale capacity, per-actor placement observability, or native actor execution. Resource-bounded repeated larger-wave results are retained separately; no million-live attempt or Go superiority claim is made.
