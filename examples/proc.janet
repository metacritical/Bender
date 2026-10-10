###
### Proc helpers (Step P1.2): selective receive, monitors, links over
### the core/proc primitives. Tasklets stay untouched; this module is
### the proc-side companion (same disciplines: at-most-once, stash
### order, single-shot reapers, recorded exits for late monitors).
###
###   (proc/select pred)            # first matching message, stashing rest
###   (proc/monitor watched w tag)  # [:down tag reason] to w on completion
###   (proc/link a b &opt tag)      # symmetric fate
###
### Caveats (same as tasklets): nil messages are invisible to select
### (nil = miss); raw bodies that protect everything can trap kills.
###

(defn select
  "Selective receive on the calling process's mailbox: return the
  first message satisfying `pred`, stashing the rest in arrival order
  in the C stash (visible via proc/stash). Must be called inside a
  process."
  [pred]
  (def me (proc/self))
  (when (nil? me) (error "proc/select outside a process"))
  (def st (proc/stash me))
  # Stash scan (Janet-side: wake values bypass C, so matching lives
  # here — see P1.2 notes).
  (var found nil)
  (var idx nil)
  (eachp [i m] st
    (when (and (nil? idx) (pred m))
      (set idx i)
      (set found m)))
  # NOTE: (break val) discards val in this Janet — park in a cell.
  (if (not (nil? idx))
    (do (array/remove st idx)
      found)
    (do (while (nil? found)
          (def m (proc/recv))
          (if (pred m)
            (set found m)
            (array/push st m)))
      found)))

###
### Monitors over supervisor completions (same shape as tasklets: a
### single-shot reaper per watched proc pumps its sup channel once and
### exits; the C monitor list holds [tag watcher] pairs; done/reason
### are recorded for late monitors).
###

(var *reapers* @{})

(defn- ensure-reaper [watched]
  (when (nil? (get *reapers* watched))
    (put *reapers* watched true)
    (def sup (proc/sup-of watched))
    (ev/go
      (fn []
        (def [sig fib _] (ev/take sup))
        (def reason (if (= sig :ok) :normal (fiber/last-value fib)))
        (proc/record-exit watched reason)
        (def mons (proc/monitors-of watched))
        (var i 0)
        (while (< i (length mons))
          (def mtag (get mons i))
          (def nch (get mons (+ i 1)))
          (proc/send nch [:down mtag reason])
          (+= i 2))
        (array/clear mons)))))

(defn monitor
  "Watch `watched`: when it completes, watcher proc `w` receives
  [:down tag reason]. Already-completed processes deliver immediately
  from the recorded exit. Returns `w`."
  [watched w tag]
  (if (proc/done? watched)
    (proc/send w [:down tag (proc/exit-reason watched)])
    (do (proc/monitor watched w tag)
      (ensure-reaper watched)))
  w)

(defn link
  "Symmetric link between two procs: when either completes, the other
  observes [:down [:link tag] reason] (trap enforcement lands with
  loop processes in P1.4; raw bodies pattern-match it themselves).
  Returns [a b]."
  [a b &opt tag]
  (default tag :link)
  (monitor a b [:link tag])
  (monitor b a [:link tag])
  [a b])

###
### Loop processes (P1.4): spawn-loop runs a handler over [tag
### payload reply] triples (same convention as tasklets, so the two
### sides interoperate). Reserved tags: :down (monitor/link reports)
### and :proc-quit (graceful stop). Reply channels must be thread
### channels (isolated-channel replies arrive as disconnected copies
### — the Step 1.4 lesson). Like everything else here: sends accept
### pids, control ops (call/quit/monitor/kill) need objects.
###

(defn- dispatch
  "Dispatch loop for spawn-loop handlers. Untraped linked-peer death
  re-raises (the loop dies with the reason); trapping loops observe
  (:down [tag reason]) and survive. Handler errors reply as
  (table :proc-error r) and never kill the loop."
  [me handler]
  (var running true)
  (while running
    (def [tag payload reply] (proc/recv me))
    (cond
      (= tag :proc-quit)
      (do (set running false)
        (when (and reply (= :core/channel (type reply)))
          (ev/give reply :proc-stopped)))
      (= tag :down)
      (do (def reason reply)
        (def linked (and (tuple? payload)
                         (= 2 (length payload))
                         (= :link (payload 0))))
        (def mtag (if linked (payload 1) payload))
        (when (and linked (not (proc/trap? me))
                   (not= reason :normal)
                   (not (nil? reason)))
          (error reason))
        (handler :down [mtag reason]))
      (let [[ok r] (protect (handler tag payload))]
        # P1.4b: armed kills re-raise (the loop's own protect must not
        # convert them into replies for handler-wedged fibers).
        (when (and (not ok) (proc/killed? me))
          (error r))
        (when (and reply (= :core/channel (type reply)))
          (ev/give reply (if ok r (table :proc-error r)))))))
  nil)

(defn spawn-loop
  "Spawn a loop process dispatching [tag payload] messages to
  `handler` (see loop). Returns the proc."
  [handler &opt capacity]
  (default capacity 16)
  (proc/spawn (fn [] (dispatch (proc/self) handler)) capacity))

(defn call
  "Synchronous request/reply with a loop process (or its pid tuple).
  Replies travel a thread channel (capability by reference; values
  still copy). With `timeout`, raises 'proc call timed out'. Handler
  errors re-raise."
  [p tag payload &opt timeout]
  (def rch (ev/thread-chan 1))
  (proc/send p [tag payload rch])
  (def raw
    (if (nil? timeout)
      (ev/take rch)
      (do (def tch (ev/chan 1))
        (ev/go (fn [] (ev/sleep timeout) (ev/give tch :proc-timed-out)))
        (def sel (ev/select rch tch))
        (if (= tch (sel 1))
          (error "proc call timed out")
          (sel 2)))))
  (if (and (table? raw) (get raw :proc-error))
    (error (raw :proc-error))
    raw))

(defn quit
  "Gracefully stop a loop process (proc object only — tuples cannot
  prove liveness, and an unread quit would hang). Dead fibers return
  at once (alive? reads the struct — no reaper needed, so double-quit
  is safe, unlike tasklet/quit's reaper race). Raw (proc/spawn) bodies
  that never proc/recv cannot quit. Returns `p`."
  [p]
  (when (not (proc/pid? p)) (error "proc/quit needs a proc object"))
  (if (not (proc/alive? p))
    p
    (do (def rch (ev/thread-chan 1))
      (proc/send p [:proc-quit nil rch])
      (ev/take rch)
      p)))

###
### Same-VM name registry (proc-local: pids keep identity — like the
### tasklet registry, separate namespace).
###

(var *proc-names* @{})

(defn register
  "Bind `name` to `p`'s pid tuple in the proc registry."
  [p name]
  (put *proc-names* name (if (proc/pid? p) (proc/pid p) p))
  p)

(defn whereis
  "Resolve `name` to a pid tuple (or nil). Plugs into proc/send."
  [name]
  (get *proc-names* name))

(defn unregister
  "Remove `name` from the proc registry."
  [name]
  (put *proc-names* name nil))

###
### M:N lanes (P2.1): persistent thread VMs running a control loop.
### Placement happens at spawn time (no live migration — 4.3
### boundary). Pids grow into their node slot: [:pid 0 serial mbox]
### local, [:pid lane serial ctl] remote. Sends to remote pids
### forward [:proc-fwd serial msg] over the lane control channel
### (C side); thread-chans inside forward by reference, so call
### replies route back. Fire-and-forget to a dead remote is a silent
### no-op (Erlang parity). monitor/kill/link stay same-lane objects.
###

(var *lane-next* 0)

(defn- lane-loop
  "Control loop running ON the lane thread: spawn locally, forward
  into local mailboxes (dropping for unknown/dead serials), stop on
  :proc-stop (then the thread is joinable)."
  [ctl procs]
  (var running true)
  (while running
    (def m (ev/take ctl))
    (def cmd (m 0))
    (cond
      (= cmd :proc-spawn)
      (do (def thunk (m 1))
        (def cap (m 2))
        (def reply (m 3))
        (def p (proc/spawn thunk cap))
        (put procs (proc/serial p) p)
        (ev/give reply [:proc-ok (proc/serial p)]))
      (= cmd :proc-fwd)
      (do (def p (get procs (m 1)))
        (when p
          (try (proc/send p (m 2)) ([_] nil))))
      (= cmd :proc-stop)
      # Teardown: kill everything hosted (parked fibers hold the
      # worker's janet_loop open — the thread can only exit once the
      # loop drains), then return so the thread becomes joinable.
      (do (eachk serial procs
            (try (proc/kill (get procs serial) :lane-stopped) ([_] nil)))
        (set running false)))
  nil))

(defn lane-spawn
  "Spawn a lane (persistent worker thread + loop). Returns {:id :ctl
  :thread}. Stop with lane-stop (required — unstopped lanes leak
  their thread, like pools)."
  []
  (def ctl (ev/thread-chan 64))
  (++ *lane-next*)
  (def id *lane-next*)
  (def h (thread/new (fn [c] (lane-loop c @{})) ctl))
  {:id id :ctl ctl :thread h})

(defn spawn-on
  "Spawn `thunk` (0-arg function) as a raw process on `lane`.
  Returns the remote pid [:pid lane serial ctl]."
  [lane thunk &opt cap]
  (default cap 16)
  (def reply (ev/thread-chan 1))
  (ev/give (lane :ctl) [:proc-spawn thunk cap reply])
  (def res (ev/take reply))
  [:pid (lane :id) (res 1) (lane :ctl)])

(defn spawn-loop-on
  "Spawn-loop on `lane`: like spawn-loop, but the loop runs there.
  Call it with procx/call (replies route back through the forward)."
  [lane handler &opt cap]
  (default cap 16)
  (spawn-on lane (fn [] (dispatch (proc/self) handler)) cap))

(defn lane-stop
  "Stop `lane`: poison-pill the control loop, then join the thread.
  Lane-hosted processes die with their VM. Returns `lane`."
  [lane]
  (ev/give (lane :ctl) [:proc-stop])
  (thread/join (lane :thread))
  lane)
