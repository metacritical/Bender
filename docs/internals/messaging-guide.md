# Messaging layer: API guide with examples

Two styles over the share-nothing base (see `docs/internals/aot-plan.md`
for the roadmap, `docs/internals/shared-structs-design.md` for the M5
review). Everything here runs on `thread/new` OS threads with isolated
VMs unless noted; cross-thread values marshal (copy) by default.

Conventions used below: `mb` is a mailbox table, `p` a pool, `pid` a
`[:pid node serial chan]` tuple. All sends are at-most-once (no acks,
no retries); a dead peer's sends block at capacity rather than error
(monitors turn that into information).

## 0. The ladder (pick the cheapest tier that fits)

Bottom to top, each tier multiplexing the one below: OS processes
(`os/spawn`, §10) → pthreads (`thread/new`, §1) → lane VMs and the C
pool (§6) → procs (§11: Erlang-style, isolated heaps) → tasklets
(§6b: actors over fibers) → ev tasks/fibers (cooperative). Fibers stay
pinned (no migration); M:N exists only at job level over one shared
queue. Lane workers float by default; `proc/lanes-start` accepts
`{:affinity true}` to pin them to cores (best-effort, and usually the wrong
choice on Apple Silicon E/P cores — the OS scheduler is better informed).

## 1. One-shot tasks: `thread/new`, `thread/join`, `thread/alive?`

```bender
(def h (thread/new (fn [x] (* x x)) 21))  # value arg marshals in
(thread/alive? h)                          # true until joined
(thread/join h)                            # 441; re-raises thread errors
(thread/join h 5)                          # with timeout (ev tasks only)
```

Use when: a single computation with one result, fire-and-forget
background work you join once, or test fixtures. Do NOT use for
long-lived services (no mailbox, no restart) or high-rate batches
(spawn cost is a full VM boot — see pool).

## 2. Workers: `examples/mailbox.ben`

Long-lived worker thread + private queue + dispatch loop. Handlers
receive `[tag payload]` and return the reply value; handler errors
keep the worker alive and surface as errors in `call` callers.

```bender
(import ../examples/mailbox :as mailbox)

(def db (mailbox/spawn
          (do (var store @{})
              (fn [tag payload]
                (case tag
                  :put (put store (payload 0) (payload 1))
                  :get (get store payload))))))
(mailbox/send db :put [:user "amy"])      # async, blocks at capacity
(mailbox/call db :get :user)              # -> "amy" (sync request/reply)
(mailbox/call db :get :user 0.5)          # with timeout (seconds)
(mailbox/quit db)                         # graceful stop + join
```

Worker-local closure state (`store` above) lives in the worker's copy
— share-nothing, no locks. Payloads and handlers must marshal.

Use when: a stateful service, serialized access to a resource, or an
actor with an identity. Prefer over raw threads whenever the worker
lives longer than one call; prefer pool (below) for batches of
one-shot calls.

## 3. Supervision: monitors, links, trap-exit

`ev/monitor` is the runtime primitive; the mailbox layer adds
`:down` traffic handling, links, and trapping.

```bender
# raw: watch any thread handle, get [:down tag reason] on a channel
(def ch (ev/thread-chan 4))
(def ref (ev/monitor h :w ch))  # -> integer ref (nil if already dead)
(ev/demonitor h ref)            # silent no-op when stale/finished

# mailbox level: the watcher handler observes (:down [tag reason])
(def watcher (mailbox/spawn
               (do (var last nil)
                   (fn [tag payload]
                     (case tag
                       :down (set last payload)
                       :last last)))))
(mailbox/monitor watched watcher :w)   # watch by mailbox or pid
(mailbox/link a b :lab)                # symmetric; tag reserved as
                                       # [:link tag] on the wire

# trapping: observe crashes instead of dying with the peer
(mailbox/trap-exit watcher true)
```

Semantics to internalize (cooperative — see plan §Cooperative
supervision): monitors only ever *notify*; links *propagate* an
abnormal exit by killing a non-trapping worker with the peer's
reason (`:normal` and nil never propagate); trapping workers observe
`(:down [tag reason])` and survive. A dead worker's exit reason is
`:normal` on clean exit or the error value on failure; monitoring an
unknown pid gives `:noproc` immediately. Nothing here preempts: a
wedged worker (long computation, deadlocked send) never reads its
`:down`, so propagation needs live, mailbox-pumping workers.

Use when: crash notification (`monitor`), mutual-fate pairs
(`link`), and restart logic (`trap-exit` + spawn a replacement in
the `:down` handler — a one-level supervisor in ~10 lines).

## 4. Identity: pids and `self`

```bender
(def pid (mb :pid))            # [:pid 0 7 chan]
(mailbox/pid? pid)             # type check
(mailbox/self)                 # calling worker's pid (nil outside)
(mailbox/send pid :tag x)      # every op takes mailboxes or pids
(mailbox/call pid :tag x 5)
(mailbox/monitor pid watcher :t)
```

Pids travel by value (channels marshal; verified `deep=` across
threads). Equality is full-tuple equality. Resolution
(pid → worker handle) is node-local — cross-node routing arrives
with distribution, not here.

Use when: identity must be stored, compared, or passed detached
from the handle table (registries, link sets, reply-to addresses).

## 5. Names: the registry

```bender
(mailbox/register physics :physics)  # keyword -> pid, process-global
(mailbox/whereis :physics)            # -> pid or nil; plugs into
                                      # send/call/monitor/link directly
(ev/unregister :physics)
```

Values store as marshaled copies: lookups get an equal value in the
caller's own VM, never a cross-heap reference (thread handles are
refused — owner-local capabilities can't travel). Visible from
every thread, including eval'd mod code. Entries live until
removed/overwritten.

Use when: any layer must find a worker without threading a handle
through (game systems finding `:physics`, mods finding services).

## 6. Batches: C pool (`pool/spawn`) over N warm VMs

M one-shot calls over N long-lived worker VMs on a shared condvar
queue — no per-task spawn cost (the cheap-restart substrate). The C
runtime (M4c) is primary: one hop, direct completion events for
`pool/call` (no per-task reply channel). `examples/pool.ben` stays
as the reference implementation (same semantics, Bender-level dispatch
loop, per-task reply channels).

```bender
(def p (pool/spawn 4))
(pool/call p + [3 4])                    # 7
(pool/call p image-blur [pixels] 5)      # with timeout
(pool/map p price [[sku1] [sku2]])       # fan-out, results in order
(def ch (pool/submit p f [x])) (pool/collect ch)
(pool/stop p)                            # drain + join all workers
```

Task errors re-raise in the collector; workers survive. FIFO per
queue; no ordering across workers. `pool/submit` errors arrive as
`[:pool-error err]` tuples; `pool/collect` re-raises them (with
optional timeout). `pool/acquire` + `pool/dispatch` are the internal
building blocks (`call`/`submit`/`map` compose them — capacity tokens
are acquired first, then the task dispatches; never separate them).

Task discipline (hard rules — violations wedge or panic, never corrupt):
- Tasks run share-nothing: `func`/`args` marshal in, results marshal
  back. Fibers resume with the args array as one value.
- Inside tasks use ONLY threaded channels (`ev/thread-chan`) and
  marshalled values. Plain channels are single-VM: a cross-VM
  take/give schedules a foreign fiber on the wrong queue (hangs).
- Tasks may use `ev/sleep` (each runs under a per-task event loop),
  but a task that suspends forever wedges its worker (capacity it
  holds never returns — cooperative, like the rest of the layer).
- Do not nest `pool/call` on a saturated pool (same-pool recursion
  deadlocks at capacity — same as the reference impl).
- `pool/stop` is required (like closing a file); use-after-stop
  panics fail-loud (the reference impl instead blocks for a timeout).
  Abandoning a pool without stopping leaks its threads.

Use when: many calls (parallel maps, asset pipelines, physics
islands), or anywhere `thread/new` per item would dominate. Do NOT
use for stateful services (no per-task state — use mailbox) or
strictly ordered streams (use one mailbox).

## 6b. Tasklets: bare fibers with owned isolated queues

BEAM-shaped processes inside one VM (`examples/tasklet.ben`):
each task owns an `ev/ichan` mailbox (marshal-copied values, never
shared mutable references), has a `[:pid node serial chan]` identity
(shared shape with mailbox pids), and supports send/call/selective
receive, links/monitors/trap-exit, and a same-VM name registry.
Fibers stay pinned N:1; supervision is cooperative.

```bender
(import ../examples/tasklet :as tasklet)

(def db (tasklet/spawn
          (do (var store @{})
              (fn [tag payload]
                (case tag
                  :put (put store (payload 0) (payload 1))
                  :get (get store payload))))))
(tasklet/send db :put [:user "amy"])
(tasklet/call db :get :user)      # -> "amy" (copied)
# selective receive in a raw task (out-of-order arrival, in-order match)
(tasklet/spawn-raw
  (fn []
    (tasklet/recv (fn [m] (= :a (m 0))))))
(tasklet/quit db)
```

Rules that differ from mailboxes: payloads copy on every crossing
(mutations never alias); pids must not travel as ichan payloads (plain
channels marshal-copy, so the pid would point at a disconnected
queue — pass registry names instead); supervisor channels are plain
(completion events embed the task fiber, which packing would try to
marshal); quit on a completed task returns at once; raw tasks that
never read their mailbox cannot quit. `(break val)` discards val in
this Bender — loops park results in cells instead.

Kill and supervision (`tasklet/kill`, `supervisor-start`):

```bender
(tasklet/kill db :killed)   # untrappable at loop level (trap-exit
                             # never saves the victim); wedged hogs die
                             # at the next quantum boundary (<=200 polls);
                             # monitors observe [:down tag reason].
                             # Unknown/completed pids: silent no-op.
(def sup (tasklet/supervisor-start
           [{:id :db
             :start (fn [] (tasklet/spawn db-handler))
             :restart :transient}]))   # :permanent | :transient | :temporary
(tasklet/supervisor-which-children sup) # -> {id pid-or-nil}
(tasklet/supervisor-stop sup)           # graceful quit, then
                                        # :shutdown-kill stragglers
```

Restart intensity (default 3 per 5s) bounds crash loops — exceeding
it exits the supervisor with the reason. Stopping never resurrects
`:permanent` children. `which-children` returns pids, not handles
(handles embed live fibers, which cannot cross the reply channel).
Raw tasks that protect their whole body can trap a kill (loop tasks
cannot) — the one documented hole in "untrappable".

Lanes (Step 4): thread VMs + the shared job queue are the lanes
(`pool/*`, `examples/pool.ben`); tasklets stay pinned to their home
loop. Isolated channels enforce the pin (same-pointer foreign-thread
use panics; from Bender every crossing copies, so ownership visibly
follows the value). Suspended pure-Bender fibers migrate across
threads with exact state (`thread/new` a suspended fiber); fibers
suspended with C frames (any tasklet blocked in take/recv/sleep)
refuse marshal — running tasks relocate by respawn, never stack copy.

## 7. Broadcast constants: `shared`

Immutable values frozen once, readable in every VM with zero copies
after the first (refcounted off-heap blobs; reads copy out).

```bender
(def cfg (shared/new {:gravity 9.8 :tiles ["a" "b"]}))
(shared/shareable? {:a @[]})   # false — tables/arrays/buffers/
                               # functions/fibers rejected (fail-closed)
(shared/get cfg :gravity)      # 9.8 (navigates, no full decode)
(shared/length cfg)            # 2
(shared/materialize cfg)       # whole value in caller-heap form
```

Nested access composes with ordinary `get` on decoded pieces.
Structs iterate to capacity (never length); prototypes are dropped
(rejected); numbers compare bitwise; missing keys/indices give nil.

Use when: one value is read in N VMs (static game data, configs,
broadcast parameters) — the break-even vs marshal-copying, same as
Erlang refc binaries. Do NOT use for mutable or per-thread state,
huge values read once (copying is cheaper), or anything containing
handles/channels.

## 8. Cancellation: `ev/interrupt`

```bender
(ev/interrupt fiber :cancelled)  # next resume yields payload instead
                                 # of running; returns the fiber
```

Delivery happens at the resume funnel — before a single instruction
runs — so never mid-instruction, never inside `pcall`/`try`, never
in C. A racing wakeup value is dropped (interrupt wins). In-loop
tasks park like a bare yield (supervisor sees it); `interrupt` +
`ev/cancel` is the cooperative-kill pattern. Interrupts do not
cross VMs. Pending value is GC-rooted.

Use when: watchdog timers, canceling queued/sleeping work,
supervisor-driven shutdown. Do NOT expect it to stop running code
(see next section for that).

## 9. Fairness: `ev/timeslice` (Step 2: on by default)

```bender
(ev/timeslice)          # -> current setting (per-VM, default on)
(ev/timeslice false)    # fully cooperative (opt out)
(ev/timeslice true)     # back on
```

Every resume grants 200 back-edge/call polls (count only backward
jumps and calls — forward code is free); exhaustion suspends
transparently (resume continues exactly — verified bit-exact) and
the scheduler re-queues instead of unwinding. Compute hogs can no
longer starve sleepers and takers; C calls stay atomic (polls are
Bender ops only); try/pcall regions are transparent. Manual `resume`
loops are unaffected (exhaustion inside a resumed fiber retries
transparently). `BENDER_TIMESLICE=0` starts a process fully
cooperative. External C quiesce still unwinds (checked first).
~1% overhead measured.

Use `false` when: hand-rolled schedulers need run-to-completion
atomicity, or benchmarking wants dispatch without quanta. Do NOT use
to kill tasks (use interrupt+cancel), or expecting hard real-time
(quanta are poll-counted, not timed; straight-line megafunctions
aren't preemptible mid-way; native kernels don't poll).

## Decision cheat-sheet

| Need | Use |
|---|---|
| one result, join once | `thread/new` + `join` |
| stateful service / serialized resource | `mailbox/spawn` |
| many one-shot calls | `pool/spawn` + `call`/`map` |
| ordered event stream | one `mailbox` (FIFO per queue) |
| crash notification | `ev/monitor` / `mailbox/monitor` |
| mutual fate / cascade | `mailbox/link` (+ `trap-exit` to observe) |
| restart on crash | trapping supervisor + `spawn` replacement |
| find by name | registry (`register`/`whereis`) |
| pass identity around | pids (+ `self`) |
| broadcast constants | `shared/new` |
| cancel/wake a sleeper | `ev/interrupt` (+ `ev/cancel` to kill) |
| hogs starving the loop | `ev/timeslice` |
| non-blocking check (never suspends, safe in `bender_call`) | `ev/try-give` / `ev/try-take` (nil = full/empty; closed `give` panics, closed `take` gives nil) |
| isolated same-VM mailbox (copy, no lock) | `ev/ichan` — payloads marshal-copy like thread-chans; single-VM only |
| bare-fiber actor (send/call/recv/link/monitor) | `examples/tasklet.ben` (`spawn`, `spawn-raw`, `recv`, `link`, `monitor`, `register`) |
| Erlang-style proc (owned mailbox, pid, lanes) | `proc/spawn`, `proc/send`/`recv`, `proc/pid`, `proc/spawn-isolated` (§11) |
| one value, many blocking consumers | `future`/`deref` (examples/future.ben) |
| single-assignment box across tasks | `promise`/`deliver`/`deref` |
| heaviest isolation (crash-proof workers) | `os/spawn` + pipes (§10) |

## Worked example: supervised physics with named broadcast config

```bender
(import ../examples/mailbox :as mailbox)
(import ../examples/pool :as pool)

# Static level data, readable in every worker with no copies.
(def level (shared/new {:gravity 9.8 :walls [0 800]}))

# Physics worker, supervised: on abnormal exit, restart it.
(def sup (mailbox/spawn
           (do (var phys nil)
               (fn [tag payload]
                 (case tag
                   :down (set phys (mailbox/spawn @{})) # simplified restart
                   :get phys)))))
(mailbox/trap-exit sup true)
(def phys (mailbox/spawn (fn [t p] p)))
(mailbox/register phys :physics)
(mailbox/monitor phys sup :phys)
(mailbox/link phys sup :sup-link)

# Heavy lifting elsewhere: batch over a pool, addressed by name.
(def p (pool/spawn 4))
(pool/call p step-island [[1 2 3]])
(mailbox/quit phys)
(pool/stop p)
```

## Limits (not footnotes)

- At-most-once everywhere; no acks, no retries, no ordering across
  workers/threads (FIFO per queue only).
- Cooperative supervision only: no preemptive or untrappable kill;
  propagation needs live, mailbox-pumping workers.
- Pids resolve node-locally; no cross-node routing yet. `monitor` /
  `kill` / `link` need the live object (C state lives there) — pids
  are send addresses, not control capabilities. No `demonitor` for
  proc/tasklet monitors yet (thread `ev/monitor` has refs).
- Registry values are copies (snapshots); mutating the original
  after registering changes nothing already looked up. Proc/tasklet
  registries are same-VM; only `ev/register` is process-global.
- Shared blobs are immutable-by-construction; reads copy out;
  `nil` reasons never propagate; OOB array-kernel indices abort
  (see AOT notes, not this layer).
- Timeslice quanta are poll-counted (back-edges/calls), invisibly
  exact, but not timed; incompatible with external C quiesce while on.
- No restart supervisor for procs yet (tasklets have
  `supervisor-start`); proc supervision is reapers + loops today.
- Agents/STM are designed, unbuilt; atoms are same-VM only (no
  locking — cross-thread state goes through channels).

## 10. OS processes: `os/spawn` and pipes

Strongest isolation (a crashing worker cannot corrupt anyone), true
parallelism, highest spawn cost (fork+exec, ~ms) and per-message
serialization. The pattern for Erlang-style messaging over OS procs —
framed marshal over piped stdio with pid-shaped addresses and a
mailbox-holder task — is designed, unbuilt; today `os/spawn` gives
byte streams (`:out :pipe`), and pids carry a `node` slot in
anticipation of cross-node routing.

```bender
(def p (os/spawn ["./worker" "arg"] :p {:out :pipe}))
(def out (:read (p :out) :all))
(os/proc-wait p)
```

## 11. First-class procs: `core/proc` + lanes

**Read `docs/internals/lanes-and-captured-state.md` before crossing lanes.** A
proc spawned onto a lane receives a marshalled COPY of its closure, so captured
mutable state (arrays, plain `ev/chan`) is detached: writes from the worker are
invisible to the spawner. Lanes are opt-in via `proc/lanes-start` or
`BENDER_LANES=<n>`, and use `ev/thread-chan` (not `ev/chan`) to return results.

One GC object per process (mailbox fiber + supervisor channel +
serial). Same-lane basics, M:N lanes via `examples/proc-lane.ben`
(control thread-chans; placement pools), strict isolated heaps for
density + per-proc collection.

```bender
(def p (proc/spawn (fn [] (pp (proc/recv)))))
(proc/pid p)           # => (:pid 0 1 <core/channel ...>)
(proc/send p :hi)      # by object or pid tuple — same delivery
(proc/send pid :hi)    # tasklet <-> proc interop rides the channel
# remote: (:pid lane serial ctl), forwarded; dead remotes silent no-op
(proc/spawn-isolated (fn [] ...))  # marshal-copied entry, own heap
```

## 12. Futures and promises: `examples/future.ben`

`(future & body)` runs on the shared pool and returns immediately;
`deref` blocks (any number of consumers — the `thread/join` gap),
caches, re-raises errors, takes `timeout` / `timeout-val`;
`realized?` peeks without consuming; `future-cancel` is best-effort
(running workers finish). `(promise)` + `(deliver p v)` (first wins)
+ blocking `deref`. Handles pin the lane: same-VM typical.

```bender
(import ../examples/future :as fut)
(def f (fut/future (compute x)))
(fut/deref f 5 :too-slow)
```

## 13. Shared C stacks: `BENDER_SHARED_STACKS=1`

Opt-in lane-shared native stacks for ev tasks and procs (~1.5KB/task
measured, 10x down; context-switch cost identical). Raw C-API yield
and nested suspend under shared ancestors fail loud (generators stay
private); `defer`/`propagate` are completed-only and safe.
