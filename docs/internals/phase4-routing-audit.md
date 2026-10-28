# Phase 4 routing audit: where a cross-lane hop's time actually goes

Status: source-verified against `a69c0984`. Every line reference below was
re-read at that commit. Claims are tagged so a later reader can tell evidence
from inference:

- **[measured]** - an artifact exists in the tree or in an archived record.
- **[reported]** - a figure from an earlier session with no retained artifact;
  treat as a claim, not a reading.
- **[static]** - read directly from the current source at the cited lines.
- **[open]** - unresolved; the evidence present does not decide it.

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

**[reported]** The phase-4 comparison table
(`docs/internals/phase4-comparison.md`) reported 6.9-7.4 us per cross-lane
round trip against 0.17 us same-VM, and a `sample` run that showed lane threads
parked in `kevent`. Those figures are **reported, not retained**: no script in
`bench/` produces the reported marshal or same-VM rows, and no output file
records them. `phase4-comparison.md` itself now heads that section
"Cross-lane hop cost: provisional, not root-caused", and the earlier "ROOT
CAUSE FOUND: cross-lane hops cost two poll cycles" heading was withdrawn.

**[open]** Neither the numbers nor the sample decompose the hop. A parked
thread observed at sample time says where the thread *waits*, not what its work
costs when it runs. The attribution to "poll wakeup latency" was inference from
a steady state, not a measurement of the components listed in section 1, and it
does not survive as an established cost model.

**[reported / provisional]** The follow-up fast-post attempt is recorded in
`docs/internals/fastpost-rejected.md` as an experiment whose source was
**reverted before commit and is not in the repository**; that document marks its
implementation detail and both measurement arms **transcript-only**. Two
specifics bear on how little this evidence can support:

- The pipe was **not** removed from the path. As described, the ring was drained
  first and the pipe was still written **as a nudge when the target had
  announced itself parked** — so a park-dependent self-pipe write remained in
  the design.
- The rejection is therefore not a measured refutation of the pipe's cost, and
  it is not an exclusive-cost proof of anything. The described implementation
  also had a reported reservation defect (two producers able to read the same
  ring head) that is a plausible independent cause of the reported hang, and
  the "ring never reported full" diagnostic was itself racy. A hang at larger n
  is equally consistent with either cause, and the report does not discriminate
  between them.

What the fast-post episode legitimately supports: an as-described lock-free
inbox did not show a latency win in that session, and its results are
unverifiable. It does not show the remaining cross-lane cost is unavoidable,
nor that any component of it has been attributed.

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

**[measured]** Local N=1000 wave: 99-101 ms when the workload is a script,
versus 279-282 ms for the same actor graph compiled as one top-level form, and
only 6-16 ms with lanes enabled. All arms produced correct results.

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
  (`src/boot/boot.janet:7055-7058`), i.e. the same shape as one `do` form.

### The shape is now measured, not hypothesized

**[measured]** A packaging control reproduced the gap by changing *only* the
top-level compilation shape: the same forwarding actors, sink, receive /
increment / send operations, completion assertion, and result output, with the
whole wave grouped inside one top-level `(do ...)` so it compiles as one thunk
instead of one thunk per form.

Committed artifacts (commit `dba6e617`) live under
`bench/results/phase4-controlled-35671ede/packaging/`:

- `packaging-control.jsonl` - 24 rotated normal/grouped trials, 3 repeats per
  cell, N=1000/2000, baseline and post-fix interpreters.
- `packaging-exact-control.jsonl` - 12 confirming grouped trials written with
  the **original integer-millisecond format**, so output-format differences
  cannot explain the numbers.
- `wave-single-do.janet`, `wave-single-do-exact.janet`, `README.md`.

Provenance for the two interpreters is archived alongside:
`../candidate/collector-candidate.patch` (the measured collector change) and
`../matrix/` (the corpus-wide build matrix). All 36 subprocesses exited 0 with
`result == N`.

Wave-only medians, ms:

| build | N | normal script | single-`do` script | plain-AOT reference |
|---|---|---|---|---|
| baseline | 1000 | 99 | 279 | 281 |
| baseline | 2000 | 406 | 772 | 777 |
| post-fix | 1000 | 8 | 22 | 23 |
| post-fix | 2000 | 33 | 64 | 61 |

Grouped figures are the 12-trial integer-format confirmation; the 24-trial
decimal batch agrees within a few ms (765.0 vs 772, 23.0 vs 22, 63.2 vs 64,
279.6 vs 279), so the format change only quantises, it does not explain the
gap. Normal-script figures are medians of the 24-trial batch.

The single-`do` arm lands on the plain-AOT reference numbers, and the grouped
cost is preserved on **both** builds - including after the collector fix, where
the absolute cost drops by roughly an order of magnitude. A packaging effect
that survives removing the dominant cost is not a link or build artifact.

**Supported conclusion:** grouping the source into one top-level thunk is
sufficient to reproduce the script/AOT local discrepancy *without* changing the
interpreter binary or linking a static AOT executable. Static-link or PGO
differences are therefore not required to explain this gap.

**[measured, bounded]** That is the whole claim. It does **not** identify the
exclusive object-level root path. The control cannot separate "more owners, so
more scoped collections" from "one large thunk keeps a larger reachable frame
set charged to a single nursery and trips the threshold differently" - both are
consequences of the same shape change, and apportioning the extra traversal
among specific references would need mark-edge instrumentation. Packaging is
also not claimed to be the only influence on every workload.

Residual limits of this evidence:

- **Lexical scope changes intentionally, and only in the driver.** The grouped
  source hoists `n`, `done`, `sink`, `head`, `i`, `t0`-`t2` inside the `do`
  block; the spawned process bodies and the `done` channel are unchanged. That
  is the point of the experiment - it changes driver compilation shape, not
  actor wiring - but the arms are not byte-identical sources, so this is a
  test of thunk shape, not an identical-object-graph control.
- **The AOT reference values are not simultaneous paired timings.** They come
  from earlier matched matrix/candidate batches, so "within noise" is an
  ordering-of-magnitude agreement, not a paired comparison.
- **The records carry no environment field**, so the lane setting per cell is
  not recoverable from the JSONL alone; the run used `JANET_LANES=0`.
- The baseline grouped decimal-format first trial (379 ms) is an outlier and is
  retained in the raw records rather than discarded.

**Operational warning:** these results do not license removing roots or
changing compilation semantics to improve a benchmark. The cost is a property
of reachable-frame shape, and the fix belongs in collection policy, not in
narrowing what user code may reference.

## 8. Discriminating packaging experiment (ran; results in section 7)

The design below was executed as the packaging control above. Kept because the
single-entry image arm (**E2**) has not been run: that one would separate
"many top-level thunks" from "image packaging" as a whole.

Change only packaging; keep every actor, mailbox, and message identical.

**E1 - single-form script.** Wrap the whole body in one `(do ...)`, producing
exactly one top-level form:

```
mkdir -p /tmp/pk && { printf '(do\n'; cat bench/cross-runtime/wave.janet; printf '\n)\n'; } > /tmp/pk/wave-do.janet
```

Run both at N=1000, same environment, several repeats:

```
JANET_LANES=0 JANET_NO_PROFILE=1 ./build/bender bench/cross-runtime/wave.janet 1000
JANET_LANES=0 JANET_NO_PROFILE=1 ./build/bender /tmp/pk/wave-do.janet 1000
```

**Observed:** the wrapped form reproduces the image-arm cost (table above), so
top-level compilation shape is sufficient for the gap. It is not evidence of an
exclusive root path (see the bounded conclusion in section 7).

**E2 - image with a single entry (not yet run).** Take the same wrapped body,
put it inside `(defn main [& args] (do ...))`, build with `-c`, and run the
image:

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
