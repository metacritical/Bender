# HANDOFF — epoch-mark ladder + ev2 root-protocol blocker (from the density session)

**From:** the Erlang-density goal session (the supervisor_channel / tombstone / decoupling commits)
**For:** whoever lands the parked-reader scaling fix (100k chain) or touches gc.c mark/sweep

## What I measured (chain = test/scale-proc-chain.ben)

| config | chain-100k | chain-10k |
|---|---|---|
| committed (78de4a75) | STALLS (probe timeout) | 1.2s / 119.7 µs-hop |
| scaled triggers only (fiber-minor + full + global-minor thresholds ∝ block_count) | 261s PASS | 1.2s |
| scaled triggers + **epoch mark** | **33s / 18.7 µs-hop** | **0.39s** |

The binding constraint is the O(world) bit pre-clear (`bender_clear_all_reachable_bits`)
in EVERY collector; threshold scaling alone cannot remove it. The epoch mark
(4-bit epoch, flags 11-14 = 0xF800, gc.c-local `bender_gc_epoch`, `epoch_next` at every
mark-phase start with a wholesale field wipe at wrap 15→1) removes all pre-clears and
was worth 8-9x on the chain. Full working diff reproducible from this session's transcript.

## Why it did not land: the ev2 root protocol

With the epoch landed, suite-ev2 fails: `count-uncollected` returns **100/100**
(expected <= 1). Instrumented evidence chain:

- `bender_gcroot`/`bender_gcunroot` of fibers: **319/319 balanced** — NOT a leaked-roots bug.
- `bender_mark_fiber`: the probe fibers are **never marked** (only ~10 boot-fiber marks).
- Sweep survivor dump: only 2 fiber survivors, flags `0x807` (epoch-stamped, current).
- The 100 dead fibers are **swept correctly** — but the weak table's
  `check_liveref` still reports them alive: their slots were **recycled and
  re-stamped** by the current cycle's marks, and `bender_gc_reachable` on the
  freed address reads the *new occupant's* epoch == current -> "alive".
  Freed-memory aliasing through `check_liveref` (gc.c weak-drop), not a
  root leak.

## The coordinated design to land both

1. Remove the same-VM `gcroot`/`gcunroot` pairs for parked fibers on THREADED
   channels (ev.c: 1421, 1503 park; 1275, 1299, 1869, 1907 wake; 1111 vmref).
   `bender_chanat_mark`'s in-place queue marking (a098223a) already protects
   same-VM parked fibers; the pairs are redundant and their visibility
   semantics are what the epoch breaks. Keep cross-VM refs rooted.
2. Make `check_liveref` alias-safe: weak-drop must consult liveness recorded
   BEFORE the sweep (a mark-time snapshot or generation tag), never a fresh
   read of possibly-recycled memory.
3. Re-land the epoch + scaled triggers. Strict-proc majors keep the RAW
   threshold (O(own) needs no amortization — suite-proc test 38 is the gate;
   my first cut scaled it and broke the pressure test).

## Probes (rebuild from this note; /tmp copies die)

- `BENDER_ROOT_TRACE=1` + fprintf traces in `bender_gcroot`/`bender_gcunroot`
  (BENDER_FIBER-filtered), `bender_mark_fiber` entry, sweep survivor path —
  the exact instrument set that produced the evidence chain above.
- ev2 probe: count-uncollected from suite-ev2.ben lines 118-133 in a
  standalone script; <= 1 is the pass gate for ANY mark-semantics change.

## Cross-session note

3dbb2e0b (your poison+guards) and my 854a67ac (tombstones+counter) coexist
green: suite-proc 5/5, all siblings, make test 52/52, chain-10k 0.55s at the
combined HEAD. The remaining hang face (parked fibers, lost wakeups —
bench/concurrency.ben x5) is the D2 family: my bisections show it appears
exactly when per-proc routing is widened (ex-fallback) or channel pinning
changes — see the todo tree's round-5/7 entries before re-attempting.
