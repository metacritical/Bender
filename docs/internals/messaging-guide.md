# Messaging layer: API guide with examples

Two styles over the share-nothing base (see `docs/internals/aot-plan.md`
for the roadmap, `docs/internals/shared-structs-design.md` for the M5
review). Everything here runs on `thread/new` OS threads with isolated
VMs unless noted; cross-thread values marshal (copy) by default.

Conventions used below: `mb` is a mailbox table, `p` a pool, `pid` a
`[:pid node serial chan]` tuple. All sends are at-most-once (no acks,
no retries); a dead peer's sends block at capacity rather than error
(monitors turn that into information).

## 1. One-shot tasks: `thread/new`, `thread/join`, `thread/alive?`

```janet
(def h (thread/new (fn [x] (* x x)) 21))  # value arg marshals in
(thread/alive? h)                          # true until joined
(thread/join h)                            # 441; re-raises thread errors
(thread/join h 5)                          # with timeout (ev tasks only)
```

Use when: a single computation with one result, fire-and-forget
background work you join once, or test fixtures. Do NOT use for
long-lived services (no mailbox, no restart) or high-rate batches
(spawn cost is a full VM boot — see pool).

## 2. Workers: `examples/mailbox.janet`

Long-lived worker thread + private queue + dispatch loop. Handlers
receive `[tag payload]` and return the reply value; handler errors
keep the worker alive and surface as errors in `call` callers.

```janet
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

```janet
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

```janet
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

```janet
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

## 6. Batches: `examples/pool.janet`

M one-shot calls over N warm worker VMs on a shared queue — no
per-task spawn cost (the cheap-restart substrate).

```janet
(import ../examples/pool :as pool)

(def p (pool/spawn 4))
(pool/call p + [3 4])                    # 7
(pool/call p image-blur [pixels] 5)      # with timeout
(pool/map p price [[sku1] [sku2]])       # fan-out, results in order
(def ch (pool/submit p f [x])) (ev/take ch)
(pool/stop p)                            # drain + join all workers
```

Task errors re-raise in the collector; workers survive. FIFO per
queue; no ordering across workers.

Use when: many calls (parallel maps, asset pipelines, physics
islands), or anywhere `thread/new` per item would dominate. Do NOT
use for stateful services (no per-task state — use mailbox) or
strictly ordered streams (use one mailbox).

## 7. Broadcast constants: `shared`

Immutable values frozen once, readable in every VM with zero copies
after the first (refcounted off-heap blobs; reads copy out).

```janet
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

```janet
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

## 9. Fairness: `ev/timeslice`

```janet
(ev/timeslice true)   # preemptive round-robin on (default off)
(ev/timeslice)        # -> current setting (per-VM)
```

Every resume grants 200 back-edge/call polls; exhaustion suspends
transparently (resume continues exactly — verified bit-exact) and
the scheduler re-queues instead of unwinding. Compute hogs can no
longer starve sleepers and takers; C calls stay atomic (polls are
Janet ops only); try/pcall regions are transparent. External C
quiesce still unwinds (checked first). Zero behavior change when
off (one predictable branch); ~1% overhead measured when on.

Use when: mixing compute-heavy tasks with latency-sensitive ones
in one loop (frame tasks + background baking). Do NOT use to kill
tasks (use interrupt+cancel), across threads (opt each VM in), or
expecting hard real-time (quanta are poll-counted, not timed;
straight-line megafunctions aren't preemptible mid-way).

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

## Worked example: supervised physics with named broadcast config

```janet
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
- Pids resolve node-locally; no cross-node routing yet.
- Registry values are copies (snapshots); mutating the original
  after registering changes nothing already looked up.
- Shared blobs are immutable-by-construction; reads copy out;
  `nil` reasons never propagate; OOB array-kernel indices abort
  (see AOT notes, not this layer).
- Timeslice quanta are poll-counted (back-edges/calls), invisibly
  exact, but not timed; incompatible with external C quiesce while on.
