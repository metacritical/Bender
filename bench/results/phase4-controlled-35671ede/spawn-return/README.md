# Routed spawn backpressure corrupts its public return value

Status: **fixed in `0c935aeb`**, independently reviewed, deterministic regression verified and bounded larger-wave reruns complete. Original failure evidence remains unchanged.

## Runtime observation

Using the post-fix pinned -O2 baseline shell from the Clang control set, existing frozen phase-labelled chain and explicit time/RSS bounds:
- lanes2 N50000 and60000 complete correctly;
- lanes2 N65535/65536/70000 reach spawn-end/wave-start but fail to return within5s; runner kills owned child (-9), RSS below1GiB;
- N80000 lanes1 sometimes raises invalid proc/PID at the main send; another repeat stalls;
- N80000 lanes3 and4 complete correctly (~1.4s total). This lane-count differential does not prove a per-lane integer limit or GC cause.

An additional diagnostic validates **every `proc/spawn` return immediately**, before storing it as the next chain head. It preserves all forwarding actors and handoffs; it adds construction-time checks, so its timings are NOT performance observations.

Observed on one run each:

```
lanes=1: bad_spawn index=15606 type=:core/channel   exit3
lanes=2: bad_spawn index=65504 type=:core/channel   exit3
```

This is direct wrong-return evidence, not an inference from parked threads. Early diagnostic attempts tried `%j` on the bad value and themselves failed with `could not print to jdn format`; those failed diagnostics are retained separately (`spawncheck.jsonl`), not conflated with the corrected type-only diagnostic (`spawncheck-safe.jsonl`). The retained source is the corrected version.

## Source mechanism

At pinned source `src/core/proc.c:326-334`, `janet_proc_spawn_routed` pushes its spawn descriptor to a control channel. If the channel is full it calls `janet_await()` **before constructing the PID tuple**. `janet_await` signals the event loop (`src/core/ev.c:927-931`); `janet_signalv` marks DID_LONGJUMP and longjmps (`src/core/capi.c:70-92`). The VM later pops the C frame and uses the incoming value as the call result, including tailcall handling (`src/core/vm.c:2163-2183`). It does NOT return to the C fall-through PID construction.

Channel write completion normally schedules the blocked writer with the channel (`src/core/ev.c:1473-1474`, same-VM path1892-1898). Therefore a parked routed spawn returns `core/channel`, exactly matching the diagnostic. If an intermediate head is wrong, later chain work is routed to an invalid target or channel while the sink remains waiting; if the final head is wrong the initial main send can raise. This establishes a concrete defect without claiming it is the sole possible stall cause.

## Landed correction

Keep backpressure, routing and actor semantics. An explicitly internal C prepare stage returns a direct local/accepted routed/accepted-steal handle, or a plan containing ctl, spawn message and intended PID only when try-mode3 refuses the full control channel. The public Janet wrapper performs the existing blocking write and returns the saved PID from its rooted Janet frame. No fiber-wide wake-value override, new channel cross-thread protocol, busy waiting or dropped spawn request. See `docs/internals/proc-spawn-backpressure.md` for independent rooting/API review and deterministic test details.

The deterministic test fails on the old runtime and passes on production: full gated worker ctl, ordinary and tail calls, full GC while parked, cancellation, distinct valid PID/control/serial fields, actual actor replies and immediate acceptance. Main suites ev776/capture7/heaps28/gc-minor12/gc-stress25 and lanes640k parked stop/join pass. The process suite still fails at the identical baseline test605; not green.

## Bounded reruns after the fix

All12 phase-labelled trials returned result=N and exit0 within10s/1GiB sampledRSS bounds:

| Lanes | N | Correct | Wave median ms | Maximum sampledRSS KiB |
|---:|---:|---:|---:|---:|
|1|80000|3/3|263.218|605120|
|2|65536|3/3|572.416|509904|
|2|80000|3/3|698.252|608736|
|2|100000|3/3|873.575|738304|

These are correctness/resource-bounded completion checks, **not matched before/after speedup claims**: the old80k arm stalled and has no accepted latency. Independent per-spawn checks also validated every returned PID in80k waves on lanes1 and2. Plain AOT, native-enabled/noPGO and trained-release AOT artifacts each rebuilt successfully and returned correct N1000 on local/two-lane arms. No claim of million-live scalability, per-actor placement, solved residual quadratic GC, or Go superiority.

`fixed-verification.tar.gz` retains52files with build logs, all12bounded trials stdout/stderr/hash/status, independent per-spawn checks, regression logs and AOT verification. Every archive member verified byte-for-byte. SHA256 `c211954e4a0e1f38e1045e0bd05dd4d14daa0867fecc1a05a7eab7f187926ac5`.

## Evidence

`diagnostics.tar.gz`:34files with raw statuses/stdout/stderr and corrected diagnostic source, verified byte-for-byte to originals. SHA256 `486abc46fb1161a42fcb68dd2cbcf211f6a29875feaf8316817f712c6f62626f`.

Runtime: `a69c0984` non-instrumented Clang baseline shell SHA`dcd400a616b506f236efffb42b83eec02a4750acdc6980e8f8c2f2065f939a67`. Main source bug also exists at35671ede before the GC fix; the GC optimization is not required to explain this defect. Original successful 80k result on higher lane counts is not evidence that lower-lane backpressure is correct.
