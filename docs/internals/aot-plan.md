# Janet AOT plan — spike → 1.1 → 2 → threads/AOT

## Handoff — read this first

This document plus `docs/internals/native-fibers.md` is the whole plan;
each phase records its goal, mechanics (with file:line-level pointers),
what was proven, and what is explicitly left open. A new session should
start by verifying the checkout, then pick up at the first unchecked
phase below (currently Phase 2b).

- This repo: the `janet` checkout (`janet-lang/janet` plus local work).
- Reference driver shape: single-binary driver (parse+analyze+codegen+cc
  in one process, no helper chain at compile time). Relevant first reads
  in any AOT: pipeline + limitations, driver shape, FFI, threads.
- Work status: phases 0–2a are implemented but UNCOMMITTED in the working
  tree (13 modified files, new files under `tools/`, `docs/internals/`,
  `test/`). Commit before starting new work so the next change has a
  clean base and `git diff` stays reviewable.

## Verify the checkout (must all pass before new work)

```sh
make                    # default -O2 build -> build/janet
make test               # full suite: 38/38 (incl. suite-native-fiber 47/47,
                        # suite-gc-stress 21/21, suite-gc-minor 12/12,
                        # suite-thread 25/25, suite-callgraph 66/66)
make fibertest          # C call-chain suspension proof: 21 checks green
mkdir -p /tmp/aot-tests
printf '(print "hi from janet")\n' > /tmp/aot-tests/hello.janet
printf '(defn fib [n] (if (< n 2) n (+ (fib (- n 1)) (fib (- n 2)))))\n(print (fib 20))\n' > /tmp/aot-tests/fib.janet
printf '(def f (fiber/new (fn [] (for i 0 3 (yield i)) :done)))\n(while (def v (resume f)) (print v) (when (= v :done) (break)))\n' > /tmp/aot-tests/yield.janet
printf '(def ch (ev/chan 0))\n(ev/go (fn [] (ev/sleep 0.01) (ev/give ch :ping)))\n(print (ev/take ch))\n' > /tmp/aot-tests/ev.janet
printf '(error "boom")\n' > /tmp/aot-tests/err.janet
printf '(def hs (map (fn [i] (thread/new (fn [x] (* x x)) i)) (range 5)))\n(pp (map thread/join hs))\n(print (thread/join (thread/new (fn [x] (string "n=" x)) 7)))\n' > /tmp/aot-tests/thread.janet
for t in hello fib yield ev err thread; do
  ./build/janet tools/janet-diff.janet /tmp/aot-tests/$t.janet
done                    # AOT oracle: same × 6
make clean && make CFLAGS="-O0 -g" && make test   # same gates, -O0 build
```

## New files this project adds (all untracked until committed)

- `tools/mkimage.janet`, `tools/janet-aot.sh`, `tools/janet-diff.janet`
  (Phase 0 AOT spike: image compiler, driver, oracle).
- `tools/fiber-c-test.c` (Phase 1.1b proof program; `make fibertest`).
- `test/suite-native-fiber.janet` (47 tests: native-stack + swap-suspension).
- `test/suite-gc-stress.janet` (21 tests: GC root gate).
- `docs/internals/aot-plan.md` (this file), `docs/internals/native-fibers.md`
  (runtime direction + per-phase mechanics/pitfalls).

Status: Phase 1 (native-stack substrate) DONE on this checkout, uncommitted.
`build/janet test/suite-native-fiber.janet` → 28/28. Full suite is the gate
before each phase below. See `docs/internals/native-fibers.md` for Phase 1 detail.

Goal: a `janet-aot` single binary that compiles Janet source to a standalone
native executable: parse → macroexpand → analyze → emit one `.c` → `cc` →
binary, with the interpreter embedded as fallback for dynamic code.
Single-binary driver shape:
parse+analyze+codegen+cc in one process, no helper chain at compile time.

## Janet pipeline today (what we reuse)

- `src/core/parse.c` (`JanetParser`) → `src/core/compile.c:janet_compile`
  → `specials.c` + `cfuns.c` builtin inlining → `emit.c/emit.h`
  (`janetc_emit_*`) + `regalloc.c` → `bytecode.c:janet_bytecode_optimize`
  → `JanetFuncDef{bytecode,consts,defs,slotcount,...}` (`janet.h`)
  → `run.c:janet_dobytes` → `vm.c:run_vm` on `fiber->data` frames.
- Values: NaN-boxed `Janet` (`janet.h`), 16 `JanetType`s; GC objects headed
  by `JanetGCObject`. `boot/boot.janet` baked into the binary via
  `build/c/janet.c` (Makefile bootstrap); `tools/amalg.janet` precedent for
  shipping source as C.
- Fibers now each own a 2 MiB + 64 KiB-guard native stack with
  `janet_fiber_ctx_{prime,swap}` (`fiber.c`, `janet.h`), trampoline in
  `vm.c:janet_fiber_trampoline`. Frames/values still live in `fiber->data`.

## Phase 0 — AOT skeleton spike (first; proves the pipe, no perf)

Build `tools/janet-aot` (C, linked against `build/libjanet.a` at first):
`janet-aot app.janet [-o app] [-c] [-S] [-E args...]`.

1. Parse with `parse.c`, macroexpand with the *host* `janet` semantics
   (macros run in a fiber at compile time, as today — `compile.c`).
2. Take the resulting top-level `JanetFuncDef`s as IR (do NOT redesign the
   frontend yet; `compile.h:JanetCompiler/Scope/Slot`, `emit.h` stay private,
   spike reads finished `FuncDef`s via `marshal.c`-style walk).
3. Emit one `.c`: static `JanetFuncDef` initializers for consts/bytecode +
   `main()` that `janet_init`s, `janet_gcroot`s the constants, loads the
   image and calls into it. All ops stay boxed (`Janet`); `JOP_CALL/RESUME/
   YIELD/CLOSURE` lower to existing `janet_call/continue` entry points.
4. `cc -O2 -ffunction-sections -fdata-sections -Wl,--gc-sections` (macOS:
   `-Wl,-dead_strip`) against a `libjanet_rt.a` split (header-inline hot
   paths vs archived `src/core/*.c`, mirroring `lib/rt.h` vs
   `lib/rt_*.c`). Static roots only; no VM-stack scanning for constants.
5. Refusals (compile error with file:line list): `eval`,
   `compile`, `dobytes/dostring` on runtime strings, runtime `require` with
   computed paths, `dyn *redef*` redefinition, live `marshal` of code,
   `ffi`/`dlopen` natives, `ev` threading beyond smoke. `--defer-fallback`
   keeps building and routes refused forms to the embedded interpreter.
6. Oracle: `tools/janet-diff` — run `janet app.janet` vs `janet-aot -E`
   (fold addresses/paths/times), labels `same/output-diff/exception-diff/
   compile-error/crash`, exit 0/1/2/3 like a native/boxed diff oracle.

Accept: `print "hi"`, `fib(20)`, file IO, one fiber yield/resume program,
one `ev/chan` smoke program, and one `thread/new` + `thread/join` program
produce `same` through the oracle; `-c/-S`
emit inspectable C; binary starts with no `janet` on PATH.

## Phase 1.1 — frames on the native stack

Goal: VM frames live in C frames on the fiber's stack; `fiber->data`
becomes values-only, then is removed. Compiled functions get real C locals
and can yield with live C state (hard requirement for unboxed codegen).
Keep semantics identical; interpreter rides the same substrate.

- Move `JanetStackFrame` chain out of `fiber->data` into the trampoline's
  C frames (`vm.c`, `fiber.c/h`, `state.h:JanetVM`).
- Every `janet_try` jmp_buf already lives on the fiber stack (Phase 1);
  extend the invariant to all return/signal registers.
- Update `janet_mark_fiber` (`gc.c`), fiber marshal (`marsh.c`), debugger
  (`debug.c`) for the new layout.
- Regression: full suite + `suite-native-fiber` (deep recursion, nested
  resume/error, GC-in-flight, marshal round-trip, 300 live fibers).

### 1.1a swap-suspension substrate (DONE — prerequisite, this checkout)

Before frames can move, suspension must preserve the C stack at all.
`janet_call` coerced any nested non-OK signal to `"coerced ... to error"`;
now non-ERROR signals swap-freeze the C chain (`janet_suspend_swap`,
register exchange per party, resume re-enters the nested VM) while ERROR
keeps legacy longjmp unwinding. Full detail in
`docs/internals/native-fibers.md` Phase 1.1. Remaining 1.1 work is the
frame migration itself on top of this substrate.

### 1.1b C-yield proof (DONE — rescoped, this checkout)

Decision: the interpreter's `fiber->data` frames do NOT move — the flat
`run_vm` loop stays as the fallback interpreter and Phase 4 adds a
codegen path beside it. What AOT actually needs (now proven) is C call
chains surviving yield: `janet_fiber_yield(v)` suspends from any C on
the fiber stack, `tools/fiber-c-test.c` proves canaries/checksums/value
plumbing across 1–3 C frames, mid-suspension collects, cancel-unwind,
and clean marshal refusal (`make fibertest`, 17 checks green).
`test/suite-native-fiber.janet` 11–16 lock the Janet-visible paths.

## Phase 2 — explicit GC roots + collector discipline

Goal: precise, non-moving mark/sweep with explicit roots so
generated C never depends on scanning `janet_vm`/VM stacks.

- Introduce `JANET_GC_ROOT`-style root registration; per-fiber native-stack
  ranges + generated-code roots join the root set; `fiber->data` buffers
  (or their Phase-1.1 successors) walked as explicit roots.
- Retire the `janet_vm.fiber` traversal (`janet_gcmark`); VM-local state
  reached only through roots. Immutable-string heap with marker bits;
  per-worker root arrays land with Phase 3.
- Regression: full suite under `make test` + `make valtest` (leak/over-mark
  check), marshal + `weak` table tests explicitly.

### Phase 2a — root discipline + stress (DONE, this checkout)

Proved every live value reachable before swapping anything: new
`test/suite-gc-stress.janet` gate (21 tests), six latent root bugs fixed
(peg replace set, print-fn buffer, scratch lifetime, macroexpansion pins,
def/var attr tables, compiler-scope marking via a new `compiler_stack`
walk, panic-safe marshal/unmarshal suspension), plus a half-built-function
fiber invariant. Full detail in `docs/internals/native-fibers.md`
Phase 2a. Remaining: the 2b collector swap itself (slab/size-classes,
string heap, per-worker roots), then threads.

### Phase 2b — collector swap (NEXT; not started)

Do NOT replace the collector at once. The current collector lives in
`src/core/gc.c`: singly-linked `janet_vm.blocks` of GC objects,
mark from ev / `top_dyns` / `root_fiber`+child / explicit `roots[]` /
`compiler_stack`, sweep in `janet_sweep`, collection scheduling via
`next_collection`/`gc_interval` counters, `gc_suspend` nesting counter,
weak tables, scratch. Keep all entry points and their signatures
(`janet_gcalloc`, `janet_collect`, `janet_gcroot/unroot`, `janet_mark`,
`janet_table_*` etc.) stable so the rest of the runtime never notices;
swap the implementation underneath in three ordered slices:

1. **2b-i — scoped roots for generated/C code** (DONE, this checkout).
   New public API (`src/include/janet.h`, `src/core/gc.c`): a strictly
   LIFO shadow root stack (`JanetVM.shadow_roots`, raw backing so pushes
   never trigger collections) marked on every collection —
   `janet_gcshadow_push(x)` / `janet_gcshadow_mark()` /
   `janet_gcshadow_pop_to(mark)` (over-pop panics). `tools/fiber-c-test.c`
   migrated off manual `gcroot` onto it, plus a new 50-deep C call chain
   holding one table per level across mid-suspension collects with no
   manual rooting anywhere, verified intact on unwind with a balanced
   stack at the host (`make fibertest`, now 21 checks green). Full suite
   green on `-O2` and `-O0 -g`, oracle still `same` × 5.
2. **2b-ii — immutable-string heap split** (DONE, this checkout).
   Strings, symbols and keywords (keywords intern to symbols — there is
   no separate keyword block type) now live on their own
   `JanetVM.string_blocks` list, routed by type in `janet_gcalloc`;
   everything else (incl. mutable buffers) stays on the main heap.
   Both strong heaps sweep with identical semantics through a shared
   `janet_sweep_list` helper, which asserts the split every sweep
   (string heap: only string/symbol; main heap: neither) — misrouting
   aborts immediately instead of corrupting silently. `block_count`
   stays a single total so scheduling is unchanged; `clear_memory` and
   init handle both lists; the debugger's funcdef heap scan needs no
   change (funcdefs never move). Deliberately no sweep-skipping policy
   yet: skipping the string heap when "nothing new" is unsound (old
   live strings can die without new allocations), so that waits for
   generations in 2b-iii. Full suite green on `-O2` and `-O0 -g`,
   stress + fibertest + oracle green, targeted suites at 64 KiB green,
   string microbench healthy (~7 ms / 20k interning ops). No valgrind
   on this host (macOS) — `make valtest` gate pending a Linux runner.
3. **2b-iii — slab/size-class allocator + generations.** Replace the
   block list with size-classed slabs and bitmap generation/mark state
   behind the same `gcalloc` interface; sweep beside the program.
   Accept: suite + stress + `make bench` within noise of (eventually
   faster than) the current collector, `make valtest` clean.

   **Storage done (this checkout), policy open.** GC object storage moved
   from one-malloc-per-object to 16 KiB pages carved into 8 size classes
   (32–512 bytes; larger objects keep the malloc large path), behind the
   unchanged `janet_gcalloc` signature. Freelist links overlay
   `JanetGCObject.data.next`; release finds a slot's class through a
   small sorted page directory (pages are added rarely), so no per-object
   tags, no layout changes, no stolen flag bits. All whole-object frees
   (sweep all three heaps, `clear_memory`, threaded-abstract refcount
   release) route through one `janet_gc_release`; subsidiary buffers
   still use plain free. Slab state is per-VM (`JanetVM.slab_state`),
   freed at clear/deinit. Full suite green on `-O2` and `-O0 -g`,
   stress + fibertest + oracle green, boot/peg/ev/marsh clean at
   64 KiB stress, string microbench at parity (~6.5 ms vs ~7.0 ms per
    20k interning ops). Notably, the two extreme-pressure behavioral
    failures from Phase 2a no longer reproduce at any interval tried
    (down to `512` on suite-boot) — the macro-path audit is now closed
    (see Phase 2a in native-fibers.md).

   **Policy done (this checkout).** Page-granular generations on top:
   per-page age (young 0–1, old 2+) and dirty flags on the slab headers;
   a write barrier in the table/array store primitives (`table_put`,
   `put_no_overwrite`, `array_push`, indexed `put`/`putindex`, `fill`,
   `insert`, `setproto`) that dirties old pages on new heap edges;
   minors that mark from full roots while skipping old-clean tables and
   arrays, then sweep young dead blocks only (old blocks wait); majors
   unchanged plus age reset and freelist merge. Only tables/arrays are
   gated — fibers, abstracts, functions, tuples, structs, weak tables
   and everything else traverse exactly like majors (their young refs
   can't hide behind an unbarriered edge, proven by construction for
   immutables and by exhaustive store audit for the rest). Fibers stay
   fully scanned (no barrier in `run_vm`). New `test/suite-gc-minor.janet`
   torture gate (old→young webs through every barrier site under churn)
   plus a `JANET_GC_VERIFY=1` post-mark edge verifier that aborts on any
   young unmarked reachable object. Full suite green on `-O2` and `-O0 -g`
   (36 files), stress + fibertest + oracle green, targeted suites clean
   at 64 KiB stress. Rule learned the hard way and now enforced: never
   skip traversal while any compilation is active (compile-time table
   traffic defeats generation accounting — safe by construction, since
   traversing more is never wrong).

Gates for all of 2b: `make test`, `test/suite-gc-stress.janet`,
`make fibertest`, the AOT oracle, plus targeted stress runs
(`gcsetinterval 65536` over `suite-boot`/`suite-peg`/`suite-ev`/
`suite-marsh` must stay green; extreme `2048` — and `512` for
suite-boot, `1024` for suite-thread — also green now, closing the
Phase 2a macro-path audit).

## Phase 3 — threads (slices)

Replace `ev.c` cooperation with an M:N scheduler (`sp_sched.c` analogue):
green threads as fibers + scheduling records, work-stealing runqueues,
preemption at interpreter-loop poll points (`SIGURG`-style), scheduler-aware
parking for blocking IO (`net.c`, `ev.c`, `os.c`). Single-threaded
semantics unchanged until this phase.

Done slices (this checkout): native-stack fiber substrate (phases 1/1.1),
1:1 OS threads — `thread/new` / `thread/join` / `thread/alive?` — running
isolated thread-local VMs, share-nothing, values marshaled across (the
Ractor one-shot model), plus threaded channels (`ev/thread-chan`) for
streaming.

### Phase 3 — messaging layer roadmap (NEW tasks)

Two messaging styles over the share-nothing base, both first-class:

- **Ractor-style (one-shot)** — DONE as the base: spawn with marshaled
  args, join for the marshaled result. Remaining refinements:
  - *Shareable immutable values*: janet structs are immutable — pass them
    across without deep copy (needs cross-heap GC ownership: transfer or
    refcount). Design doc before implementing.
- **Erlang-style (addresses + mailboxes)** — the addressable-process
  layer. The address primitive is the channel; workers are long-lived
  threads with a receive loop. Tasks:
  - *Worker-mailbox pattern library* (janet level): `spawn-worker`/
    `send`/`receive`/`:quit` over threaded channels — ~30 lines, makes
    the pattern first-class; plus a user-managed name registry table.
  - *Links & monitors* (runtime): "notify me when worker X died" —
    needs thread-exit events plumbed through the completion path and a
    monitor table in the spawner env. Enables supervision trees.
  - *Named registry* (runtime): VM-level table mapping keywords to
    thread handles + mailboxes, `register`/`whereis`/`unregister` —
    games address the physics worker by name, not by passing handles
    through every layer.

Order: pattern library first (pure janet, immediately useful), then
monitors (small runtime feature), then the M:N scheduler core, then
struct sharing (cross-heap GC).

### Phase 3 messaging — implementation plan (detail for the tasks above)

**Node/pid model (BEAM-kind, recorded 2026-10-06).** Erlang pids are
VM-internal (`<node.serial.id>`), never OS pids: a BEAM node is one OS
process multiplexing green processes N:M onto scheduler threads. Ours
is the same shape with two tiers:
- One janet OS process = one **node**. `self()`-style identity is a
  `[:pid node serial addr]` tuple: `node` is the node id (0 today,
  nonzero when distribution lands), `serial` comes from a per-root-VM
  counter, `addr` is the delivery address.
- **Today** every addressable entity is a cross-VM worker, so `addr`
  is its `thread-chan` (channels marshal — verified — so pids travel
  by value, unlike thread handles which refuse marshal and stay
  owner-local). `thread/new` workers are effectively separate
  single-process *nodes* (share-nothing + marshal = distributed-Erlang
  semantics already); the M4 scheduler adds the within-a-node tier,
  where `addr` resolves to a task queue instead.
- Bare ev-task fibers own no channel, so fiber pids wait on M4.
  Mailbox/worker pids need nothing new: counter + channel + `self()`.
- `monitor`, `link`, `send (!)` and the M3 registry all resolve
  **pids**, never handles/channels. Reason values: `:normal` |
  error value | `:noproc` (target already dead).

**Cooperative supervision (recorded 2026-10-06).** Full OTP signal
semantics fight this architecture (no interpreter safepoints, no
shared heap, marshal-everything), so supervision is *cooperative*:
it works between live workers that pump their mailboxes, which covers
the games workload (a crashed physics worker gets restarted instead of
killing the game). The boundary is semantics, not footnotes:
- GUARANTEED: `:down` notification to monitors on worker exit;
  error values travel as reasons; trapping workers observe exits as
  messages and can restart peers via `thread/new`.
- NOT PROVIDED (documented, behind M4): preemptive or untrappable
  kill — a wedged worker (long computation, deadlocked send) never
  reads its `:down` and never dies; cross-thread signal ordering is
  best-effort, never Erlang-strict; restart cost is a full OS-thread +
  VM boot, so supervision is for coarse workers, not Erlang-scale
  process counts (cheap restart comes with M4 task queues).
- Design, in order:
  - *M2b refs/demonitor*: `ev/monitor` returns an integer slot ref;
    `ev/demonitor handle ref` tombstones it (post-completion
    demonitor is a clean no-op). ~20 lines of C, no architectural
    tension.
  - *M2c trap-exit*: per-mailbox flag (`mailbox/trap-exit mb bool`).
    Worker loop on `:down` with non-`:normal` reason: trapping ->
    handler observes `(:down [tag reason])` as today; not trapping
    -> worker exits with that reason (`error`), which rides the
    existing completion path and cascades. ~10 lines of Janet, zero
    new runtime primitives. Supervisors are trapping workers.
  - *M2d link as composition*: `mailbox/link` stays two monitors +
    propagate-on-down — identical semantics to a C primitive under
    best-effort delivery, so no primitive is needed. Setup needs no
    atomicity: back-to-back `ev/monitor` calls on one owner cannot
    interleave harmfully (a worker exiting mid-setup yields an
    immediate `:down` — still correct ordering).
  - *M2e pids*: `[:pid node serial chan]` + per-root-VM counter +
    `mailbox/self`; pid-resolving `ev/monitor`/`link`/`send`;
    `:noproc` immediate-down for dead pids. Bare-fiber pids wait on
    M4 (fibers own no channel yet).

**M1 — worker-mailbox pattern library** (pure janet, `ev/mailbox.janet`
or shipped as an example). Surface:

```janet
(def mb (mailbox/spawn handler-fn &opt capacity))  # -> mailbox address
(mailbox/send mb tag payload)                       # async, fire-and-forget
(mailbox/call  mb tag payload &opt timeout)         # send + wait for reply
(mailbox/quit  mb)                                  # graceful stop
```

- `mailbox/spawn` = `(ev/thread-chan cap)` + `(thread/new loop :n)`; the
  loop is `while running: [tag payload reply-ch] = ev/take mb; dispatch`.
- `send` = `ev/give mb [tag payload reply-ch]`; `call` attaches a
  fresh reply channel and `ev/take`s it with a deadline.
- Semantics: at-most-once per message (no acks); a dead worker makes
  sends block at capacity — monitors (M2) turn that into an error.
- Tests: spawn/quit lifecycle, backpressure at capacity, call/timeout,
  concurrent senders (2+ producer threads), marshal of payloads.
- Files: `examples/mailbox.janet` + pins in suite-ev or a new
  `test/suite-mailbox.janet`.

**M2 — links & monitors** (runtime, ev.c only — there is no
thread.c; all thread primitives live in ev.c). Status: SHIPPED
handle-based (`ev/monitor handle tag chan`, commit M2); pid-shaped
surface below is the follow-up that unlocks supervisors. Surface:

```janet
(ev/monitor pid tag)        # when pid exits, send [:down tag reason]
                            # to the monitor's own mailbox (pid-implied,
                            # no channel argument)
(ev/link pid-a pid-b)       # symmetric: one dies -> other gets :down
(mailbox/self)              # -> calling worker's pid
(pid ! msg)                 # send to a pid (mailbox-level sugar)
```

- Mechanics: the thread-completion path (the completion posting that
  thread/join consumes) already reaches the spawner — extend it: each
  thread carries a monitor list (mailbox + tag pairs); on exit,
  deliver `[:down handle reason]` messages. Reason: `:normal` |
  `:error` (marshaled error value).
- Data: a monitor table in the thread handle (registered mailboxes are
  channel handles — marshal-safe).
- Tests: normal exit delivers `:normal`; error exit delivers the
  error; monitor survives worker crash; link chain (A<->B, B dies, A
  notified). Gate: full `make test` + threaded-channel stress.
- Pid follow-up (unlocks supervisors, TODO in order — each lands
  with suite pins + full gates before the next starts):
  - TODO M2b refs/demonitor: slot-ref return + `ev/demonitor`;
    pins: demonitor-then-exit delivers nothing, double-demonitor and
    post-completion demonitor are silent no-ops, surviving monitors
    still fire.
  - TODO M2c trap-exit: `mailbox/trap-exit` flag + loop propagation;
    pins: trapping worker observes `:down` and survives; non-trapping
    linked worker exits with the peer's reason (cascade A->B->C);
    `:normal` exits never propagate.
  - TODO M2d link composition: `mailbox/link` on M2b+M2c; pins: link
    chain B-dies-A-notified with reason preserved; mid-setup exit
    still orders correctly.
  - TODO M2e pids: pid value + counter + `mailbox/self`,
    pid-resolving monitor/link/send, `:noproc` immediate-down; pins:
    pid round-trips through marshal, self() inside workers, monitor
    by pid, dead-pid monitor gives `:noproc`. Bare-fiber pids
    deferred to M4. Registry (M3) maps names to pids.
- Known-open (pre-existing, reproduced on the clean tree 2026-10-06,
  and re-verified with quantum off 2026-10-07): ASan stack-scope
  reports (`stack-use-after-scope`, plus `stack-buffer-under/overflow`
  twins) around 24-byte memcpys on fiber stacks — single-frame
  traces, no janet frames (custom stacks defeat unwinding). Minimal
  repro (no threads, no monitors involved):
  `(def c (ev/thread-chan 1)) (ev/go (fn [] (ev/sleep 0.01)
  (ev/give c :x))) (ev/take c)`. Plain channels and sleep-free tasks
  are clean. All M2/M4b paths verified clean under
  `ASAN_OPTIONS=halt_on_error=0` — the only signatures seen are this
  pre-existing family, including with quantum off (where the new
  branches provably cannot execute: every one is predicated on a
  nonzero quantum flag).

**M3 — named registry** (runtime, ev.c — no new file needed).
DONE. Surface as spec'd (`ev/register`/`ev/whereis`/`ev/unregister`,
keywords to pids), plus `mailbox/register`/`mailbox/whereis` sugar.
Design as implemented:
- Process-global C table (name -> marshaled byte copy) under a
  platform once-init mutex. Janet tables are per-VM/isolated, so the
  store lives in C; values copy out and unmarshal in the caller's VM
  — never a cross-heap reference, no GC coupling, VMs may die freely.
- Marshal runs BEFORE the lock (a refusal raises without wedging
  the mutex). UNSAFE mode passes threaded channels by pointer, so
  M2e pids travel intact (verified: pid round-trips `deep=`).
- Thread handles are refused explicitly (UNSAFE would pass them by
  pointer, leaking a ref and handing out an owner-locked capability).
  Registry entries live until removed/overwritten; one channel ref
  per registration is never released (documented, registry-scale).
- Tests: pid round-trip, overwrite, unregister semantics,
  cross-thread visibility both directions, refusal + continued use,
  named send/call through looked-up pids. ASan stress clean
  (40 entries, overwrite churn, concurrent + cross-thread registrants).

**M4 — M:N scheduler core** (runtime, sp_sched.c analogue).
M4a SHIPPED: pooled warm-VM executors (`examples/pool.janet` —
M one-shot tasks over N long-lived VMs on a shared queue, zero
runtime code) deliver the cheap-restart half of M4's promise
today. REMAINS (M4b, multi-session epic): the preemptive core —
work-stealing runqueues, SIGURG preemption at poll points,
scheduler-aware parking.
M4b-i FINDING (recorded 2026-10-06, investigated in-tree): the
poll-point half of preemption ALREADY EXISTS — `vm_maybe_auto_suspend`
fires on backward jumps + calls (vm.c), suspending with the real
`JANET_SIGNAL_INTERRUPT`, requested via `janet_interpreter_interrupt`
(atomic, state.c), and the ev loop unwinds on it (ev.c: suspends the
task, returns the fiber, and refuses to schedule further tasks while
`auto_suspend` is nonzero). But it is a QUIESCE primitive by design
(pause-the-world until `interrupt_handled`), not a quantum: the
flag is VM-wide (no per-fiber targeting), setting it stops ALL
further scheduling until cleared, and there is no Janet-level
requester (self-request is pointless — a fiber that can run code
can just yield; cross-thread request has no VM handle to target).
Turning it into quanta means per-fiber pending state + the loop
re-queueing interrupted tasks instead of unwinding + a request
API + chaos proving — i.e. the scheduler redesign itself, which is
the epic, not a slice. No dead-code API was added on top.
M4b-1 SHIPPED 2026-10-06: per-fiber interrupt-pending state
(`ev/interrupt fiber &opt payload`) delivered as a forced YIELD at
the resume funnel (`janet_continue_no_check` head — before a single
instruction, never inside pcall/try, never in C). Semantics pinned:
interrupt wins over the continuation and over a racing wakeup (the
in-flight take value is dropped); in-loop tasks park exactly like
a bare yield (supervisor sees it; interrupt + cancel = cooperative
kill); interrupts do not cross VMs (cleared on unmarshal); pending
value GC-marked.
M4b-2 SHIPPED 2026-10-07: quantum driver (`ev/timeslice`) — per-VM
flag; every resume grants JANET_QUANTUM_BUDGET (200) back-edge/call
polls via the existing `vm_maybe_auto_suspend` points (zero new
hot-path cost: one branch that already existed, one decrement);
exhaustion suspends transparently (NO_USEVAL/NO_SKIP resume exact)
and the scheduler re-queues instead of unwinding (skipping the
supervisor event + the blocking poll while runnable work is queued;
external quiesce still unwinds via the auto_suspend check).
Synchronous resume sites loop transparently (dostring, macex ×2,
fiber-`next` iteration); pcall/nested/step propagate for fairness
and debugger semantics. Pinned: order-based starvation reversal,
bit-exact results, try-transparency, nested-call fairness, toggle,
per-VM scoping, eval-under-quanta; full suites + fibertest green,
all scheduler suites re-run under quanta (chaos gate), ASan shows
only the known pre-existing fiber-stack family. REMAINS:
work-stealing queues, task migration, SIGURG injection. As the
Phase 3 text above: work-stealing runqueues, SIGURG preemption at
poll points, scheduler-aware parking. Prereq: everything above is
schedulable on 1:N; M:N changes WHERE fibers run, not how they
communicate — the messaging layer is deliberately scheduler-agnostic.
EPIC SCOPING (recorded 2026-10-07, verified against this tree):
work-stealing and task migration largely hold BY CONSTRUCTION here
and need no implementation — the pool multiplexes over ONE shared
queue (balanced by definition; nothing private to steal from), and
pending work migrates trivially (any thread takes from the shared
queue). Mailbox workers are pinned by design (state locality).
What remains genuinely missing is ASYNC preemption: waking threads
parked in blocking syscalls and interrupting non-cooperating C code
(SIGURG + signal-safe handler discipline across ev.c/net.c/os.c),
plus migration of RUNNING tasks (suspended stacks + VM-local waits:
channels, timeouts, supervisors — the semantics surface is the work).
No current workload needs either (quantum covers Janet-level hogs;
cross-thread wakeups already post via self-pipe); both stay tracked
until a workload demands them.
M4 additionally unlocks what cooperation cannot: Erlang-exact signal
semantics (preemptive/untrappable kill via interpreter safepoints),
strict cross-task ordering, cheap task restart (fresh task in a warm
VM — supervision at scale), and bare-fiber pids (every scheduled task
owns a queue).

**M5 — shareable immutable structs.** IMPLEMENTED 2026-10-06
(`core/shared` in ev.c + 34 pins in test/suite-shared.janet):
review verdict followed exactly — threaded abstract with inline
flattened payload (no nested refs, so no finalizers needed),
`shareable?` deep predicate (fail-closed; structs iterated to
capacity skipping nil keys, protos rejected), navigation without
full decode, safe-mode marshal/unmarshal via payload bytes.
Review doc carries per-proof outcomes, including the real trap
found in testing (struct capacity iteration).

**Dependency chain:** M1 (pure janet) -> M2 (runtime) -> M2b/c/d/e
(cooperative supervision: refs, trapping, link, pids — each gated
separately) -> M3 (registry, names to pids) -> M4 (scheduler core +
Erlang-exact signals + fiber pids); M5 independent of M2-M4, gated on
the GC design review. Each task: feature branch, suite pins, full
gates (`make test`, `make fibertest`, oracle x6), feature commit.

### Phase 3 slice — value-returning thread/new + thread/join (DONE, this checkout)

`ev/thread` suspends until completion but always returns nil. New
`thread/new` (function or fiber) + `thread/join` + `thread/alive?` in
`src/core/ev.c`: threaded-abstract handle (`JanetThreadHandle`,
owner-checked, single-join), detached worker bootstrapping a
thread-local VM exactly like `janet_go_thread_subr` (marshal registries
+ fn/fiber + arg in, marshal result out), completion via
`janet_ev_post_event` to the owner (message-owned result bytes,
unmarshalled on the owner; waiter scheduled with the value, cancelled
with the error — the joining cfun never resumes after await, like
channel ops). `thread/join` takes an optional timeout (seconds):
expiry cancels the waiter with a timeout error (same `sched_id`
exactly-once discipline as the scheduler: concurrent joiners panic,
a resumed-then-rejoining fiber reclaims instead of double-delivering,
and a later join still collects); finite timeouts need a task fiber.
`thread/alive?` polls completion without blocking (fresh
handles read alive deterministically: completion can only be processed
once the spawner suspends). Roots follow the `ev/thread`
pattern (join gcroots, callback unroots). New `test/suite-thread.janet`
(25 tests: value round-trips incl. fiber args, nested fibers, 8-way parallel,
ev-in-thread, error re-raise, double-join, non-function refusal,
alive? transitions, join-in-ev-task, timeout fire/rejoin/top-level)
runs in `make test` (now 37/37). Gates: full suite +
fibertest + stress/minor/ev + oracle `same` × 6 on `-O2` and `-O0 -g`,
plus a full ASan run (37/37, fibertest, stress/minor/ev green);
thread suite 25+/25 stable plus 64 KiB `gcsetinterval` runs over
suite-boot/peg/ev/marsh green.

Fixed along the way (both pre-existing, both crashers):
- Slab `janet_slab_page_for` (`src/core/gc.c`) masked any pointer to
  16 KiB and dereferenced it: malloc-large-path and foreign pointers
  could hit unmapped memory (ASan abort in `janet_gc_barrier` via
  `table_put`). Now a per-VM sorted live-page directory gates the
  probe (binary search; insert on page alloc, cleared with the pages).
- `unmarshal_one_fiber` (`src/core/marsh.c`) never initialized
  `native_stack{,_base,_size}`, trusting zeroed memory that dirty slab
  bump slots and malloc reuse don't provide; workers then primed a wild
  stack (`ev/thread` with a fiber arg segfaulted, flakily without ASan,
  ~100% under it after churn). Fields are now set explicitly like
  `fiber_alloc`; fiber args are accepted again.

## Phase 4 — full AOT (inference + unboxed codegen)

Only after 1.1+2: whole-program type analysis over the `FuncDef` graph
(analysis pipeline: registration → call-site widening →
fixpoint on returns/slots → feature/DCE flags → per-node type cache shared
with codegen in one process), then:

### Phase 4 step 1 — registration (DONE, this checkout)

`tools/callgraph.janet` extracts the FuncDef graph without running the
program: parse → one `(do ...)` unit compile (mkimage-style; macros
pre-installed like loading does) → `disasm` walk. Emits `def` nodes
(arity/slots/instrs), `contains` (closure nesting), `calls` (unique
direct target), `extern`/`ambiguous`/`unknown`, `dynamic` (eval/compile/
require/… — the inference-defeating set), and uncompiled-unit lines,
plus a stats summary. Resolution is shallow on purpose: symbolmap
ranges, backward scan to the nearest slot writer (`ldc` constants with
core-identity for cfunctions, `clo` children, `lds` self, `ldu` by
upvalue index). `test/suite-callgraph.janet` (46 tests) runs in
`make test`. Semantics verified against upstream (not tool shortcuts):
forward references fail the unit exactly like real loading; statically
unreferenced top-level defns are dropped by the compiler's dead-store
elimination, so the graph is precisely the statically-live code (dynamic
escape needs the interpreter fallback flagged by `dynamic`).

### Phase 4 step 2 — reachability + flags fixpoint (DONE, this checkout)

Same tool, appended `reach`/`callers`/`recursive`/`tainted`/`fix` lines:
reachability from entry thunks over calls+contains, per-def caller
sets, self-reachability through ≥1 edge, and backwards dynamic taint
(a container taints its holder — the DCE root/keep rule in embryo).
Exactly-once delivery style, all deterministic and order-stable.

### Phase 4 step 3 — call-site arity audit (DONE, this checkout)

`record-call` now returns the resolved target; each site is audited:
a backward scan counts `push`/`push2`/`push3` with load-ops (arg temps,
slots are reused) and clean boundaries (prior `call`/`tcall` or any
flow stop); anything else (`pusha`, computed spreads) is
`arity-unknown`, never a false mismatch. Emits `called-as <id> <n...>`
sets, `arity-bad <caller> <callee> <n>` when a counted call violates
the callee's min/max arity, and an `arity <bad> <unknown>` summary —
mismatches agree with the compiler's own runtime rejections.

### Phase 4 step 4 — type hints (DONE, this checkout)

Clojure-style optional annotations, zero parser changes (the `^`
caret symbol is NOT usable in arglists — it parses as a distinct
parameter slot and silently changes arity). Surface:
`(defn f {:hint {:params [:double :long] :returns :long}} [x y] ...)`
— the dict modifier lands in the env metadata and survives into the
thunk constants; params maps positionally. `:long`/`:double` unbox;
absent or `:number` stays boxed; unannotated code keeps the inference
path. Tool-side validation only so far (extraction, `hint` lines,
`hint-bad` rejections for unknown keywords/arity mismatches,
`hint-call` linking hinted callees with resolvable sites).

### Phase 4 step 6 — hybrid inference: returns fixpoint + call-site
widening (DONE, this checkout)

`tools/infer.janet` (shared by analysis and emission): at every call site, each
pushed argument's type narrows monotonically into the callee's param
slot (intersection; conflicting widths widen to `:number` = boxed
poly; annotation-seeded slots are contracts and never touched.
Returns flow from RETURN sites and tail
calls; whole-program fixpoint (≤8 rounds). Lattice
`:unknown > :number > {long, double}` + bool/nil leaves; intra-fn
forward slot scan seeded from param types. THE HYBRID: hints are
used directly — a `:double` hint commits to unboxed double with no
inference needed; unhinted code is proven by widening (e.g.
`(defn twice [x] (inc-l (inc-l x)))` infers param+return `long`
from one hinted callee). Emits `ret <id> <t>` and `ptypes <id>
<t...>` lines plus `rets` summary. Suite pins include the unhinted
propagation chain.

### Phase 4 step 7 — unboxed C emission (DONE, this checkout)

`callgraph.janet FILE --emit-native DIR`: any def whose params and
return are all concrete (hinted contract or inference-proven) and
whose body is in the straight-line subset (ldi/ldc-number/
add/sub/mul/div + immediates, push args, ret, direct calls to other
emitted natives via a two-round dependency pass; ldu/lds callee
loads are no-ops since the C call targets the native symbol)
emits as `static int64_t|double f(...)` with janet `/` only for
`:double` (C `/` truncates int64). Harness runs fixed samples;
expected.txt comes from the janet interpreter calling the same
functions — the suite `cmp`s them: byte-identical parity, including
unhinted functions and native-to-native calls. This is the
end-to-end proof of the hybrid: hinted contracts and pure inference
both land in the same unboxed C. Next: fold emission into the
janet-aot driver (image entry calling natives), widen the supported
subset (comparisons/branches), then the full pipeline.

## Phase 4 steps 8–13 — closing the native-tier gaps (roadmap)

Goal: grow the transcriber until real game-loop code (the target
workload: janet + raylib, C compiler on every platform, AOT-legal
where JITs are not) proves out, and ship it inside `janet-aot`'s
hybrid binary. Every step is gated by the existing parity harness
(native results `cmp`-equal to the interpreter) — no translation is
trusted until it agrees byte-for-byte.

- **Step 8 — branches (CFG).** Recover basic blocks from the jump
  ops (`jmp`, `jmpif/jmpno/jmpni/jmpnn` — operand is a signed offset
  from the jump's pc), emit labels + `goto`/`if`-`goto`. Condition
  slots must be `:bool` (from cmp ops): janet truthiness (0 is
  truthy!) forbids C truthiness on numeric slots — refuse otherwise.
  Slot types at block merges via iterative dataflow join (monotone,
  terminates). `jmpni`/`jmpnn` (nil tests) refused initially.
- **Step 9 — loops (DONE, this checkout).** Back edges were already
  gotos via the Step 8 CFG; the real work: `lte`/`gte` added to the
  cmp sets (janet `<=`/`>=` compile to dedicated ops), and temps
  changed from const-per-write to declare-once/assign-many (loop
  bodies reassign their variable slots — `(mul 1 1 2)` on slot r).
  Loop-carried types converge in the existing dataflow worklist.
  Parity: sum-to/fact byte-identical incl. back-edge gotos.
- **Step 10 — external C calls (DONE, this checkout).** `--externs
  FILE` loads declared signatures (janet name → C symbol, param/return
  types, plus a janet `:fallback` fn). The fallback pre-binds the name
  so the unit compiles and provides the parity oracle; the emitted
  native calls the C symbol directly with an `extern` declaration
  hoisted into the harness. Verified with a real C implementation
  linked into the harness — fallback ≡ C impl, byte-identical. This
  is the raylib story proven end to end.
- **Step 11 — numeric array state.** SoA state for game entities.
  DESIGN (recorded 2026-10-06, spike shipped): NOT a new core
  primitive and NOT buffer views — boxed `JanetArray*` params with
  unboxed indexed access. Rationale: arrays already exist, marshal,
  and root like any value; kernels only need read/write/accumulate,
  and SoA is just N array params. Surface: `:array` param hint
  (opaque pointer, never arithmetized); opcodes `len` (array->:long),
  `get` (array x int->:number), `put` (array x int x numeric->:array,
  in place). Rules enforced at flow-check: len/get base must be
  :array-typed, get/put index must be int-family, put value numeric,
  returns stay numeric; anything else skips with a reason.
  - GC: arrays are rooted by the caller for the call duration;
    kernels never retain pointers (no escaping: no globals, no
    pointer returns) — safe with zero collector work.
  - Bounds: janet `get` OOB returns nil, which has no unboxed
    representation — native emits an explicit check that aborts on
    violation (documented opt-in divergence, same class as :float).
    Canonical `for i 0 (length a)` loops are in-bounds by
    construction; proving len-guard dominance to elide the check is
    future work.
  - Elements are unchecked `janet_unwrap_number` (garbage for
    non-numbers — documented; hardening with checknumber later).
  - Harness: fixed sample arrays (one float, one int) built with
    wrap macros; harness cc adds `-Isrc/include -Isrc/conf` and links
    `build/janet.o` (nanbox helpers are real functions, not macros —
    pure bit ops, no VM init needed); expected values by eval with
    the same arrays. Array kernels sample int params from [0 1]
    (variants have length >= 2; OOB aborts by design).
  - Wrapper: `(JanetArray *)janet_unwrap_array(argv[i])` pass-through.
  - Future: typed buffers (`:buf-f64` views over buffer bytes),
    bounds-check elision, `put` chaining returns.
- **Step 12 — hybrid binary integration (DONE, this checkout).**
  `emit-native-lib` emits kernels + janet-cfunction wrappers (unbox →
  kernel → rebox) + `native_init(JanetTable*)` registering them.
  `mkimage --native names.txt` strips proven defns from the unit and
  prepends a prelude rebind: `(def name (:value (get (curenv)
  (quote native/name))))` — so same-unit call sites compile against
  the wrappers (native internals stay native-to-native), while boxed
  code and eval'd mod code dispatch through env with one conversion
  at the boundary. mkimage emits the native_init hook under
  JANET_AOT_NATIVE_INIT (called post-core-env, pre-unmarshal).
  `janet-aot.sh --native` builds the full hybrid. Verified: the
  hybrid binary runs boxed and eval'd paths through native kernels
  (env-registered wrappers). Known boundary: same-unit call sites
  that the compiler resolves to embedded boxed constants before the
  prelude exists are covered by the prelude rebind because stripping
  removes the boxed def; truly dynamic aliasing of a kernel value
  boxed-side keeps the boxed copy (correct, just interpreted).
  Residue (eval, dynamic require) keeps working — eval'd mod code
  demonstrably dispatches to native kernels.
- **Step 13 — int-overflow policy + DCE (DONE).** `--int-policy
  wrap|promote` (default wrap): wrap computes in exact int widths and
  shipped/harness builds pass `-fwrapv` (defined two's-complement
  wrap; C signed overflow is otherwise UB); promote maps int widths
  to double in kernel signatures/temps (oracle-identical everywhere,
  exact parity by construction) and reboxes via wrap_number. Extern C
  declarations keep exact types (foreign signatures are fixed).
  Boxed janet truncates at 2^53 in both modes — native-vs-boxed
  divergence past 2^53 is by design, now explicit. DCE is the
  reachability already in the pipeline (unreachable defns never reach
  emission — pinned: uncalled hinted fn absent from native.c).
- **Width hints — DONE.** Full C primitive set on the hint surface:
  :long/:i32/:i16/:i8/:u64/:u32/:u16/:u8/:double/:float plus aliases
  (:i64/:int64, :i32/:int32/:int, :i16/:int16, :i8/:int8, :u64/:uint64,
  :u32/:uint32, :u16/:uint16, :u8/:uint8, :f64/:double, :f32/:float),
  canonicalized at extraction (`^:keyword` and `^float` caret
  spellings too). 64-bit is the default integer; narrower widths are
  opt-in FFI-boundary contracts — computation runs at the kernel's
  declared return width, casts are automatic at call boundaries
  (harness samples, lib wrappers). :float is true float32 arithmetic;
  janet's oracle computes in doubles, so :float parity is documented
  divergence by opt-in. Infer lattice: widths join via family
  normalization (:i32 etc -> :long, :float -> :double); mixed widths
  widen conservatively. Purity/ret/div checks family-aware; int-width
  kernels get integer harness samples.

Order serves the games workload: branches and loops unblock every
real loop body; external C calls unblock raylib; arrays unblock
entity state; integration makes it a product; policy/DCE are polish.

### Phase 4 step 5 — `^` hint syntax (DONE, this checkout)

Clojure-ergonomic surface, no parser changes: the reader already
tokenizes `^type` as an ordinary symbol, so `defn` itself now
interprets it — `^type` before the argvector is the return hint,
`^type` before a parameter hints that parameter — strips both, and
emits the identical `{:hint ...}` metadata as the dict form. Written
in boot-janet using only core C functions (defn sits at line 12,
before `and`/`or`/`cond`/`when`/`each`/`++` exist) and committed with
exact paren accounting. The fn special (specials.c) now REJECTS
parameter symbols starting with `^` so a raw `(fn [x ^long y])`
fails loudly instead of silently binding the hint as a parameter and
changing arity (the original corruption that made `^` unusable).
Tool extract-hints understands both surfaces; suite pins cover the
dict form, the `^` form (identical `hint` lines), and the guard.
Bootstrap lessons recorded: def-attribute metadata must be a struct
(a table is rejected by handleattr), and a single missing paren in
boot.janet manifests as bizarre downstream compile errors (the
swallowed-forms `argn=21`) — paren balance in boot edits must be
verified mechanically. Steps 6–7 above supersede this next-step note:
the returns fixpoint, call-site widening, and unboxed C emission are
DONE (hybrid: hints direct, inference fills the rest).

- Boxed `Janet` for polymorphic slots; unboxed C (`double`, `int64`) for
  inferred numeric loops/structs; value-type promotion for small immutable
  records; int-overflow mode flag (`raise` default / `wrap` / `promote`
  to bignum discipline).
- DCE via section-GC + reachability from `main`; `--emit-types` diagnostics.
- `eval` and inference-defeating code permanently fall back to the embedded
  interpreter in the same runtime (Phase 4 in `native-fibers.md`).

## Internals changes required (recap)

| Area | Spike | 1.1 | 2 | 4 |
|---|---|---|---|---|
| `compile.h/Scope/emit.h` → public IR / NodeTable analogue | read-only use | — | — | own + publish |
| `vm.c/fiber.c/h/state.h` frames→C stack | — | yes | — | benefits |
| `gc.c/h` explicit roots → new collector | static roots only | marking update | yes | benefits |
| `marsh.c` fiber/FuncDef image | static init instead | image update | root-aware | static DCE |
| `ffi.c/ev.c/net.c` | refuse/defer | — | parking hooks | direct externs or keep refusing |
| `Makefile`/`tools/` driver+oracle | `janet-aot`, `janet-diff` | test gates | `libjanet_rt.a` split | inference flags |

## Gates

Each phase: `make` (default `-O2` and `-O0 -g`), `make test` (full suite
incl. `suite-native-fiber`), `make valtest` for GC phases. No phase may
change program semantics — oracle `same` is the bar until Phase 4
optimizations, which must still be `same`.

## Spike results (done, this checkout — tools/ only, no src/ changes)

- `tools/mkimage.janet`: whole file compiles as ONE `(do ...)` thunk
  (correct cross-form refs, zero build-time execution), marshals with a
  rev-lookup registry, emits `.c` with image bytes + registry names.
- `tools/janet-aot.sh`: driver (`-o/-c/-S/-E`, standard CLI shape).
- `tools/janet-diff.janet`: oracle (`same/output-diff/exception-diff/
  compile-error`, exits 0/1/2/4; folds `0xHEX`, tmp paths, ` (tail call)`).
- Findings: env entries are binding descriptors — registry must map
  unwrapped `:value`s and walk the `make-env` proto chain (util.c
  `janet_binding_from_entry`); runtime resolves via `janet_resolve_core`.
  `:args[0]` is the script path. `string/replace-all` takes strings only —
  use `peg/replace-all` for pattern folding. AOT `main` mirrors
  `run.c janet_dobytes` (continue, then `janet_loop()`).
- Acceptance: hello/fib(20)=6765/fiber-yield/ev-chan-rendezvous/error-exit-1
  all `same` (34/34 repo suites incl. `suite-native-fiber` still green).
  One folded cosmetic divergence: single-thunk images render one fewer
  ` (tail call)` frame note than per-form dobytes on error paths
  (identical bytecode; runtime frame-reuse artifact — Phase 1.1 closes it).

## Post-M5 addendum — generational GC promotion barrier (fixed 2026-10-07)

The M4b-1 layout shift exposed a latent minor-GC liveness bug in the
ttfx workload (uncapped `--dump-frames` decrypt crashed at ~frame 461
with garbage doubles read from swept slab slots; ASan: heap-use-after-free
reading a freed array data buffer via a still-referenced JanetArray).

Root cause: edges stored into a container while its slab page was YOUNG
are never write-barriered (correctly — young pages are always traversed).
When the page AGED to old, the minor-skip gate treated it as old+clean
and pruned it, losing those unbarriered edges; young objects reachable
only through them were swept while still referenced.

Fix (gc.c): promotion barrier — the aging loop sets `dirty = 1` exactly
once at the young→old transition, so the page's first minor as an old
page traverses its pre-existing edges; stores made while old are covered
by the regular `janet_gc_barrier`. Also closed the matching
`JANET_GC_VERIFY` blind spot: old unmarked holders are not garbage
(they float until the next major), so the verifier now walks their
edges too instead of filtering on `JANET_MEM_REACHABLE`.

Isolation evidence: minor→major forcing passed 3/3; never-skip-arrays
passed 3/3; mark-neuter and interrupt-neuter still failed 3/3 (M4b-1's
fields/checks exonerated — layout only shifted timing). Post-fix:
uncapped CLI 5/5, ttfx decrypt+wipe byte-identical to live Ruby,
43/43 suites, fibertest green, AOT oracle same ×6.
