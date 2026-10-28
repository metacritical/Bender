# Routed spawn backpressure corrupts its public return value

Status: demonstrated defect; **fix in progress**, not landed in this evidence commit.

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

## Planned correction

Keep backpressure, routing and actor semantics. A private C prepare stage returns either a direct local/already-accepted-steal handle or a rooted plan containing ctl, spawn message and intended PID. A public Janet wrapper performs the existing blocking write and returns the saved PID from its Janet frame. This avoids a fiber-wide wake-value override, new channel cross-thread protocol, busy waiting, or dropping spawn requests. Implementation, deterministic full-control-channel tests, independent rooting/API review and bounded larger-wave reruns remain required.

## Evidence

`diagnostics.tar.gz`:34files with raw statuses/stdout/stderr and corrected diagnostic source, verified byte-for-byte to originals. SHA256 `486abc46fb1161a42fcb68dd2cbcf211f6a29875feaf8316817f712c6f62626f`.

Runtime: `a69c0984` non-instrumented Clang baseline shell SHA`dcd400a616b506f236efffb42b83eec02a4750acdc6980e8f8c2f2065f939a67`. Main source bug also exists at35671ede before the GC fix; the GC optimization is not required to explain this defect. Original successful 80k result on higher lane counts is not evidence that lower-lane backpressure is correct.
