# Phase 4 routing audit: where a cross-lane hop's time actually goes

Status: source-verified against `a69c0984`. Every line reference below was
re-read at that commit. Claims are tagged **[measured]**, **[static]**, or
**[open]** so a later reader can tell evidence from inference.

This document records the routing half of the phase-4 investigation. The GC
half lives in `phase4-no-reclaim-minor-review.md`.

## 1. The hop is a dispatcher relay, not a direct mailbox write

**[static]** A `proc/send` to a remote pid tuple does not touch the target
mailbox. `src/core/proc.c:1332-1351` builds a forwarding envelope:

```c
fwd[0] = janet_ckeywordv("proc-fwd");
fwd[1] = tup[2];
fwd[2] = argv[1];
```

and pushes it onto the pid's carried control channel. The receiving lane's
dispatcher resolves the serial and re-sends locally
(`src/boot/boot.janet:3709-3710`):

```janet
:proc-fwd (if-let [p (or (proc/lane-lookup serial) (get serials serial))]
            (proc/send p x))
```

So one logical hop costs: envelope tuple, channel push, dispatcher wake,
dispatcher resume, registry lookup, local mailbox push, receiver resume.

## 2. The envelope is serialized even for immediate payloads

**[static]** Threaded and isolated channels pack non-immediate values
(`src/core/ev.c:1038-1056`):

```c
janet_marshal(buf, *x, NULL, JANET_MARSHAL_UNSAFE);
```

A `[:proc-fwd serial number]` tuple is not an immediate type, so the tuple is
marshalled and later unmarshalled. The number inside does not exempt it.

## 3. Wakeups are conditional

**[static]** A parked foreign reader is woken by a posted event
(`src/core/ev.c:1776`):

```c
janet_ev_post_event(vm, janet_thread_chan_cb, msg);
```

with the callback unpacking and scheduling the specific fiber
(`src/core/ev.c:1457-1478`). Same-VM parked readers are scheduled inline
(`src/core/ev.c:1737-1763`), and a delivery that finds buffered items needs no
wakeup at all. Therefore "one wake per hop" is not a property of the design.

## 4. What the earlier "it is only kevent" claim did not establish

**[measured]** The phase-4 comparison table
(`docs/internals/phase4-comparison.md`) reported 6.9-7.4 us per cross-lane
round trip against 0.17 us same-VM, and a `sample` run showing lane threads
parked in `kevent`. Both observations are real.

**[open]** Neither observation decomposes the hop. A parked thread observed at
sample time says where the thread *waits*, not what its work costs when it
runs. The attribution to "poll wakeup latency" was inference from a steady
state, not a measurement of the components in section 1.

**[measured]** The follow-up fast-post experiment
(`docs/internals/fastpost-rejected.md`) removed the pipe from the path
entirely and measured 6.80 us baseline versus 7.82-10.90 us with the ring, then
lost wakeups under load. That result refutes the pipe as the dominant cost; it
does not show the remaining cost is unavoidable, because the experiment
changed only *where* an event is stored, not how many scheduler transitions a
hop requires.

## 5. Retention that outlives completion

**[static]** Dispatcher-side handles accumulate: spawned processes are stored
in a per-dispatcher table with no removal path
(`src/boot/boot.janet:3673-3678`), and `proc/self-pid` populates a rooted
lane-local registry, sometimes under two serials
(`src/core/proc.c:424-443`). Completion clears the scheduler's `active_tasks`
entry and retires the mailbox, then collects the fiber's own nursery
(`src/core/ev.c:2498-2514`); it does not clear those dispatcher references.
This matters for any live-set hypothesis, which therefore cannot be closed by
the flat `hops.janet` sweep alone: that benchmark measures one persistent echo
process, not N one-shot chain processes.

## 6. Benchmark shapes and what each can conclude

| benchmark | shape | limitation |
|---|---|---|
| `bench/wave.janet:14-25` | N+1 processes, chain | `t1` marks spawn-request completion, not "all receivers built and parked"; no barrier separates construction from wave |
| `bench/hops.janet:16-35` | 1 echo + caller, repeated | no filler-parking barrier; replies discarded; does not reproduce one-shot completion/retention |
| `bench/lane-rt.janet:6-18` | buffered local self-IO vs cross-thread echo | local arm has no partner; bypasses remote `proc/send` dispatcher forwarding entirely |

**[measured]** The cross-lane 100k run that ended rc -9 was terminated by its
parent after an imposed 30 s timeout; it is not evidence of an OS memory wall.
Its first two seconds of sample span construction and waiting, so it cannot be
read as a wave-phase profile.

## 7. Consequence for the packaging discrepancy

**[measured]** Local N=1000 wave: 101 ms when the workload is a script, versus
281 ms for the same source packaged as an image, and only 6-16 ms with lanes
enabled. All arms produced correct results.

**[static]** Those two packagings differ in top-level structure, not in actor
graph work:

- A script is evaluated form by form. `run-context`'s `eval1` compiles each
  top-level form separately, in its own fiber
  (`src/boot/boot.janet:3482-3516`); `dofile` drives it
  (`src/boot/boot.janet:4023`), and `cli-main` reaches it at
  `src/boot/boot.janet:6970`. A fresh fiber means a fresh `gc_owner`, and
  nursery accounting plus the scoped-collection trigger are per owner
  (`src/core/vm.c:305-318`, `src/core/gc.c:2790`).
- An image is one marshalled environment loaded and run as a single entry
  (`src/boot/boot.janet:7055-7058`).

So the two packagings differ in how many owners the same allocation stream is
charged to, and therefore where scoped collections fall. Note the wave's own
actor work happens inside spawned processes either way (they are attributed to
the child, `src/core/proc.c:1110`), so this difference is a *plausible* cause
of a several-fold gap, not an established one. Section 8 exists to decide it.

## 8. Discriminating packaging experiment (proposed, not yet run)

Change only packaging; keep every actor, mailbox, and message identical.

**E1 - single-form script.** Copy `bench/wave.janet` unchanged and wrap the
whole body in one `(do ...)`, producing exactly one top-level form:

```
mkdir -p /tmp/pk && { printf '(do\n'; cat bench/wave.janet; printf '\n)\n'; } > /tmp/pk/wave-do.janet
```

Run both at N=1000, min of 3, same environment:

```
JANET_LANES=0 JANET_NO_PROFILE=1 ./build/bender bench/wave.janet 1000
JANET_LANES=0 JANET_NO_PROFILE=1 ./build/bender /tmp/pk/wave-do.janet 1000
```

Prediction if per-form packaging is causal: the wrapped form moves materially
toward the image number. If both stay near 101 ms, per-form thunk count is not
the discriminator and section 7's explanation is wrong.

**E2 - image with a single entry.** Take the same wrapped body, put it inside
`(defn main [& args] (do ...))`, build with `-c`, and run the image:

```
./build/bender -c /tmp/pk/wave-main.janet /tmp/pk/wave-main.jimage
JANET_LANES=0 JANET_NO_PROFILE=1 ./build/bender -i /tmp/pk/wave-main.jimage 1000
```

This isolates "many top-level thunks" from "image packaging" while preserving
the actor graph. A `main` wrapper is required because the image path runs
`run-main` (`src/boot/boot.janet:7055-7058`).

Both arms must print `result` equal to N; a fast wrong answer is a failure.

## 9. Profiling seams for a trustworthy cross-lane profile

Phase markers, not just a stack sample: acknowledged "construction complete",
"all receivers parked", "wave started", "sink received", "cleanup". Without
them a 2-second sample of a 30-second run is unattributable.

Useful existing seams: the completion collector entry
(`src/core/ev.c:2514`), the scoped-candidate predicate
(`src/core/gc.c:2766`), the dispatcher relay
(`src/boot/boot.janet:3709-3710`), the channel pack point
(`src/core/ev.c:1047`), and the foreign wake (`src/core/ev.c:1776`).
