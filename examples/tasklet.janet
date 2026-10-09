###
### Tasklets: bare fibers with owned isolated queues (Step 3.1-3.3).
### BEAM-shaped processes inside one VM: every task owns an ev/ichan
### mailbox (marshal-copied values, never shared mutable references),
### has a [:pid node serial chan] identity, and supports send/call/
### selective receive, links/monitors/trap-exit, and a same-VM name
### registry. Fibers stay pinned N:1 (no migration); supervision is
### cooperative (a wedged task never reads its :down), exactly like
### mailboxes. For cross-thread services, use mailbox.janet instead.
###
### Surface:
###
###   (def t (tasklet/spawn handler &opt capacity))
###   (tasklet/send t :tag payload)            # async
###   (tasklet/call t :tag payload &opt timeout)  # sync request/reply
###   (tasklet/recv pred &opt timeout)         # selective receive (in-task)
###   (tasklet/self)                           # calling task's pid
###   (tasklet/spawn-raw thunk)                # arbitrary body, self bound
###   (tasklet/quit t)                         # graceful stop
###   (tasklet/monitor watched watcher tag)    # :down on completion
###   (tasklet/link a b &opt tag)              # symmetric fate
###   (tasklet/trap-exit t bool)
###   (tasklet/register t name) / (tasklet/whereis name)
###
### Semantics: at-most-once per message (no acks); sends block at
### capacity; task errors re-raise in callers and never kill the loop;
### :normal/nil reasons never propagate; unknown pids resolve to their
### carried channel (best-effort, may be stale). Pids (which carry the
### isolated mailbox channel) must NOT travel as ichan payloads: plain
### channels marshal-copy, so the pid would point at a disconnected
### queue nobody reads — pass registry names instead, or use
### thread-chans (pointer passthrough) for pid-carrying traffic.
###

(var *tasklet-serial* 0)
(var *tasks* @{})   # serial -> task table; same-VM only, never pruned
(var *task-names* @{})

(defn- next-pid [chan]
  (def s *tasklet-serial*)
  (++ *tasklet-serial*)
  [:pid 0 s chan])

(defn pid?
  "True for [:pid node serial chan] tuples (shared shape with mailbox pids)."
  [x]
  (and (tuple? x) (= 4 (length x)) (= :pid (x 0))))

(defn- resolve
  "Resolve a tasklet table or pid to {:pid :chan :task-or-nil}."
  [t]
  (cond
    (and (table? t) (t :mbx))
    {:pid (t :pid) :chan (t :mbx) :task t}
    (pid? t)
    (do (def e (get *tasks* (t 2)))
      (if e
        {:pid t :chan (e :mbx) :task e}
        {:pid t :chan (t 3) :task nil}))
    (error "not a pid or tasklet")))

(defn self
  "The calling task's pid, or nil outside a tasklet."
  []
  (dyn :tasklet-self))

(defn- task-loop
  "Dispatch loop shared by spawned handlers (mirrors mailbox workers,
  over an isolated mailbox, on this VM's loop)."
  [mb handler pid]
  (var running true)
  (var trapping false)
  (while running
    (def [tag payload reply-ch] (ev/take (mb :mbx)))
    (cond
      (= tag :tasklet-quit)
      (do
        (set running false)
        (when (and reply-ch (= :core/channel (type reply-ch)))
          (ev/give reply-ch :tasklet-stopped)))
      (= tag :tasklet-trap)
      (set trapping (if payload true false))
      (= tag :down)
      (do
        (def reason reply-ch)
        (def linked (and (tuple? payload)
                         (= 2 (length payload))
                         (= :link (payload 0))))
        (def mtag (if linked (payload 1) payload))
        (when (and linked (not trapping)
                   (not= reason :normal)
                   (not (nil? reason)))
          (error reason))
        (handler :down [mtag reason]))
      (let [[ok r] (protect (handler tag payload))]
        (when (and reply-ch (= :core/channel (type reply-ch)))
          (ev/give reply-ch (if ok r (table :tasklet-error r)))))))
  nil)

(defn spawn
  "Start a tasklet: a fiber dispatching [tag payload] messages from an
  owned isolated mailbox to `handler`. Returns the tasklet handle (a
  table with :pid)."
  [handler &opt capacity]
  (default capacity 16)
  (def mbx (ev/ichan capacity))
  (def pid (next-pid mbx))
  # Plain (never packing) supervisor channel: completion events embed
  # the task fiber itself, which packing channels would try to marshal
  # (fibers with C frames refuse). Mailbox traffic stays isolated.
  (def sup (ev/chan 8))
  (def mb @{:mbx mbx :sup sup :fiber nil :pid pid
            :trap-exit false :stash @[] :monitors @[] :reaper nil})
  (def fib
    (ev/go (fn []
             (setdyn :tasklet-self pid)
             (task-loop mb handler pid))
           nil sup))
  (put mb :fiber fib)
  (put *tasks* (pid 2) mb)
  mb)

(defn spawn-raw
  "Run `thunk` (a function of no arguments) as a tasklet with self
  bound: no dispatch loop, use tasklet/recv inside. Returns the handle."
  [thunk &opt capacity]
  (default capacity 16)
  (def mbx (ev/ichan capacity))
  (def pid (next-pid mbx))
  # Plain (never packing) supervisor channel: completion events embed
  # the task fiber itself, which packing channels would try to marshal
  # (fibers with C frames refuse). Mailbox traffic stays isolated.
  (def sup (ev/chan 8))
  (def mb @{:mbx mbx :sup sup :fiber nil :pid pid
            :trap-exit false :stash @[] :monitors @[] :reaper nil})
  (put mb :fiber (ev/go (fn [] (setdyn :tasklet-self pid) (thunk)) nil sup))
  (put *tasks* (pid 2) mb)
  mb)

(defn trap-exit
  "Set cooperative trap-exit on `t` (tasklet M2c). Accepts a tasklet
  or pid (the flag record on a pid is skipped). Returns its argument."
  [t flag]
  (when (table? t) (put t :trap-exit (if flag true false)))
  (ev/give ((resolve t) :chan) [:tasklet-trap (if flag true false) nil])
  t)

(defn send
  "Fire-and-forget: enqueue [tag payload reply-ch]. Blocks if the
  mailbox is at capacity. Accepts a tasklet or pid."
  [t tag payload &opt reply-ch]
  (ev/give ((resolve t) :chan) [tag payload reply-ch])
  t)

(defn call
  "Synchronous request/reply over isolated channels. With `timeout`,
  raises 'tasklet call timed out'. Handler errors re-raise. Replies
  travel a thread channel (capability by reference; the reply VALUE
  still marshal-copies): an ichan reply would arrive as a disconnected
  copy, since only threaded abstracts pass by pointer."
  [t tag payload &opt timeout]
  (def rch (ev/thread-chan 1))
  (send t tag payload rch)
  (def raw
    (if (nil? timeout)
      (ev/take rch)
      (let [tch (ev/chan 1)]
        (ev/go (fn [] (ev/sleep timeout) (ev/give tch :tasklet-timed-out)))
        (def sel (ev/select rch tch))
        (if (= tch (sel 1))
          (error "tasklet call timed out")
          (sel 2)))))
  (if (and (table? raw) (get raw :tasklet-error))
    (error (raw :tasklet-error))
    raw))

(defn- stash-take
  "Remove and return the first stashed message matching pred, or nil."
  [stash pred]
  (var found nil)
  (var idx nil)
  (eachp [i m] stash
    (when (and (nil? idx) (pred m))
      (set idx i)
      (set found m)))
  (when (not (nil? idx))
    (array/remove stash idx))
  found)

(defn recv
  "Selective receive on the calling task's own mailbox (Step 3.2):
  return the first message satisfying `pred`, stashing the rest in
  arrival order for later receives. Messages are [tag payload] pairs
  as sent (replies arrive on their own channels, never here). With
  `timeout`, raise 'tasklet receive timed out'. Must be called inside
  a tasklet (spawn-raw bodies); errors outside."
  [pred &opt timeout]
  (def me (self))
  (when (nil? me) (error "tasklet/recv outside a tasklet"))
  (def t (resolve me))
  (def task (t :task))
  (when (nil? task) (error "tasklet/recv on unknown task"))
  (def stash (task :stash))
  (def mbx (task :mbx))
  (def hit (stash-take stash pred))
  (if (not (nil? hit))
    hit
    (do
  # NOTE: (break val) discards val in this Janet (loops yield nil),
  # so loops below park into a cell instead of breaking values out.
  (var found nil)
  (if (nil? timeout)
    (do (while (nil? found)
          (def m (ev/take mbx))
          (if (pred m)
            (set found m)
            (array/push stash m))))
    (do (def deadline (+ (os/clock) timeout))
        (while (nil? found)
          (def remaining (- deadline (os/clock)))
          (when (<= remaining 0)
            (error "tasklet receive timed out"))
          (def tch (ev/chan 1))
          (ev/go (fn [] (ev/sleep remaining) (ev/give tch :tasklet-timeout)))
          (def sel (ev/select mbx tch))
          (if (= tch (sel 1))
            (error "tasklet receive timed out")
            (do (def m (sel 2))
              (if (pred m)
                (set found m)
                (array/push stash m)))))))
  found)))

(defn quit
  "Ask the task to stop and wait for the acknowledgement. Accepts a
  tasklet or a same-VM pid. No join: fibers have no threads. Already-
  completed tasks return immediately. The tasklet must not be used
  afterwards. Raw (spawn-raw) tasks that never read their mailbox
  cannot quit (cooperative, like wedged workers)."
  [t]
  (def r (resolve t))
  (when (nil? (r :task)) (error "cannot quit unknown pid"))
  (if (get (r :task) :done)
    t
    (do (def rch (ev/thread-chan 1))
      (send t :tasklet-quit nil rch)
      (ev/take rch)
      t)))

###
### Monitors and links over supervisor completions. A watched task's
### exit delivers [:down tag reason] to the watcher, where reason is
### :normal on clean exit or the task's error value on failure. A
### single-shot reaper fiber per monitored task pumps its supervisor
### channel once and exits (no leak: exactly one event ever arrives).
###

(defn- ensure-reaper [t]
  (def task (t :task))
  (when (and task (nil? (task :reaper)))
    (def sup (task :sup))
    (put task :reaper true)
    (ev/go
      (fn []
        (def [sig fib _] (ev/take sup))
        (def reason (if (= sig :ok) :normal (fiber/last-value fib)))
        (put task :done true)
        (put task :exit-reason reason)
        (each [mtag nch] (task :monitors)
          (ev/give nch [:down mtag reason]))
        (put task :monitors @[])))))

(defn monitor
  "Watch `watched`: when it completes, `watcher` (tasklet or pid)
  receives [:down tag reason] on its mailbox. Unknown pids deliver
  [:down tag :noproc] immediately, as do already-completed tasks
  (read from the recorded exit reason). Returns `watcher`."
  [watched watcher tag]
  (def w (resolve watcher))
  (def t (resolve watched))
  (cond
    (nil? (t :task))
    (ev/give (w :chan) [:down tag :noproc])
    (get (t :task) :done)
    (ev/give (w :chan) [:down tag (get (t :task) :exit-reason)])
    true
    (do (ensure-reaper t)
      (array/push ((t :task) :monitors) [tag ((resolve watcher) :chan)])))
  watcher)

(defn link
  "Symmetric link between two tasklets (or same-VM pids): when either
  completes abnormally, the survivor observes it (trap-governed, like
  mailboxes). Returns [a b]."
  [a b &opt tag]
  (default tag :link)
  (monitor a b [:link tag])
  (monitor b a [:link tag])
  [a b])

###
### Same-VM name registry (tasklet-local: no marshal copies, so pids
### keep identity — ev/register would snapshot them instead).
###

(defn register
  "Bind `name` to `t`'s pid in the tasklet registry."
  [t name]
  (put *task-names* name ((resolve t) :pid))
  t)

(defn whereis
  "Resolve `name` to a pid (or nil). Plugs into send/call/monitor/link."
  [name]
  (get *task-names* name))

(defn unregister
  "Remove `name` from the tasklet registry."
  [name]
  (put *task-names* name nil))
