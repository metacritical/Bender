# proc/lanes: VM-per-Core Process Parallelism

## Goal
Spread millions of lightweight processes across N OS threads (one full VM each),
with the proc API unchanged, zero-config defaults, and optional control (count,
affinity, policy). This is the fork's BEAM-style multi-core story: per-proc heaps
compose with per-VM GCs, so cores never contend on a global collector.

## Architecture (3 layers)
1. **Substrate (exists)**: `ev/thread`/`thread/new` give OS threads each running a
   fresh, share-nothing VM (registries copied, own GC). `pool/spawn` proves the
   pattern and stays for task-parallel work.
2. **Lane registry (new, C)**: `janet_vm.lanes` — array of {threaded ctl channel,
   thread handle, lane-id}. Lane 0 = the main VM. A global atomic round-robin
   counter. API:
   - `(proc/lanes)` → count (0 = single-VM mode)
   - `(proc/lanes-start)` → auto: N = cpu-count - 1 (min 1), round-robin, no affinity
   - `(proc/lanes-start 4)` → explicit count
   - `(proc/lanes-start {:count 4 :affinity true :policy :round-robin})` → full form
   - `(proc/lanes-stop)` → best-effort stop (deliver :stop; detached threads die at exit)
3. **Residency (new)**: each lane runs `proc/lane-main` (boot.janet) — a resident
   dispatcher looping on its ctl channel:
   - `[:spawn serial cap entry]` → VM-local `proc/spawn`, register serial→proc
   - `[:deliver serial payload]` → look up serial, `proc/send` into the local mbox
   - `[:stop]` → break
   Cross-lane proc handles are **pid tuples** `[:pid lane-id serial ctl-chan]` —
   `janet_proc_resolve_mbox` already handles the tuple form, threaded channels
   marshal, and the pid node field disambiguates lanes (the design anticipated this).

## Routing
- `proc/spawn` with lanes active: pick lane = atomic fetch-add % N + 1; allocate a
  global serial; marshal-send `[:spawn serial cap entry]` to that lane's ctl;
  return the pid tuple `[:pid lane serial ctl]` as the handle.
- `proc/send` to a pid tuple with node ≠ 0: push `[:deliver serial payload]` onto
  the tuple's ctl (threaded channel → marshals).
- `proc/recv`, local sends, local monitors: unchanged (VM-local machinery).
- Out of milestone 1: cross-lane monitors/kill/exit-reasons, work-stealing,
  proc migration. Recorded on the tree.

## Core affinity
Best-effort C helper: Linux `pthread_setaffinity_np`, macOS `thread_policy_set`,
opt-in via `:affinity true` (default off — OS scheduling is usually right).
Sane fallback: ignore silently on unsupported platforms.

## Gates
- New `test/suite-lanes.janet`: spawn/send/recv across 2 and 4 lanes; serial
  uniqueness; round-robin distribution; stop; single-VM mode unaffected.
- Demo: agents-style benchmark partitioned across lanes vs 1 lane — expect
  ~linear scaling of the compute side, marshal tax on cross-lane sends.
- Full battery stays green (52/52 + suite-proc 5/5 + nine siblings).

## Sequencing
C registry + API → boot.janet lane-main → spawn routing → send routing →
affinity helper → tests → demo → commit. Work-stealing/migration/monitors are
follow-ups on the tree.