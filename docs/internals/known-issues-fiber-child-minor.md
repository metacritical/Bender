# Known issue: child-fiber completion minor sweeps captured parent locals

## Status
FIXED (b4a28cb1). Reproduced deterministically on 14b66fb3 and on 2421de6e,
so pre-existing; not caused by the d220b6d0/14b66fb3 ev deadline fixes.

Root cause: janet_gc_barrier recorded a cross-owner edge only under the
CONTAINER's owner bucket, while janet_xfb_mark_owned looks up the SWEEPING
fiber's owner bucket. A parent-heap array holding values born on a child
fiber's heap was therefore invisible to that child's minor, which swept the
parent's array as its own garbage. Recording the edge under the value's owner
as well makes marking symmetric. Regression: test/suite-fiber-capture.janet
(fails 5/7 before, passes 7/7 after). suite-proc 98/99 -> 99/99.

## Minimal reproducer (no channels, no procs, no strings-from-IO)

    (defn run []
      (def got @[])
      (ev/go (fn []
               (for i 0 3
                 (array/push got (string i))
                 (printf "iter %d: %j" i (map string got)))))
      (ev/sleep 0.05)
      (printf "final: %j" (map string got)))
    (run)

Observed:

    iter 0: @["0"]
    iter 1: @["0" "1"]
    iter 2: @["0" "1" "2"]
    final: @["" "@" "\xD0"]        <-- corrupted

The array's CONTENTS are correct on every iteration while the child fiber
runs, and become garbage only after the fiber completes.

## What is NOT the cause (each ruled out by experiment)
- Not strings: integers (`(string i)` results) corrupt identically.
- Not channels: reproduces with no channel anywhere.
- Not `ev/give` marshal packing: reproduces with no channel at all.
- Not the ev deadline changes: identical on baseline 2421de6e.
- Not `repeat` vs `for`: both corrupt.
- Unrolled pushes with no loop body allocation are FINE (`@["pre" "pre"]`).
- A TOP-LEVEL captured array is FINE; only a FUNCTION-SCOPE local corrupts.
- Contents are correct inside the fiber, so this is a lifetime/sweep bug,
  not a delivery or marshal-copy bug.

## Narrowed mechanism
The corruption appears at child-fiber COMPLETION, in the per-fiber minor
(`janet_collect_fiber_minor`, gc.c:2621). Completion runs
`janet_collect_fiber_minor(task.fiber)` (ev.c, loop completion path). The
captured parent local `got` lives on the PARENT heap (different
`gc_owner`); the child's owned sweep must not reclaim it, and the child's
minor pins only its own world plus the xfb remember set
(`janet_mark_proc_shared` / `janet_xfb_mark_owned`, gc.c:2713,2931).

Prime suspect: a parent-heap container referenced by the child's closure is
not recorded in the child's cross-fiber remember set, so the child's
completion minor treats the parent's array (or the strings inside it) as
its own unreachable young block and drops them.

## How it surfaced
Chasing "thread-chan corrupts strings": the channel reproducer was a red
herring. The real trigger is a captured function-scope local mutated inside
an ev/go fiber that allocates in its loop.

## Next step
Trace `janet_collect_fiber_minor` at child completion with the reproducer
above under ASAN, and confirm whether the parent array or its young string
elements are being freed by the child's owned sweep. Do NOT patch the
channel pack/unpack path: it is not implicated.

## Related, separate issue
`suite-proc` test 14 fails 98/99. Tasklet and proc pids share the
`[:pid node serial chan]` shape but use independent serial counters, while
`tasklet/resolve` looks up `*tasks*` by serial alone, so a tasklet serial
can collide with a live proc serial and misroute mail. A strict identity
check (`(= (e :mbx) (t 3))`) fixes routing but HANGS suite-tasklet, because
a marshalled tasklet pid legitimately carries a COPY of the channel. A
correct fix needs a distinct namespace discriminator in the pid shape.
