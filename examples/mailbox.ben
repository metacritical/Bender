###
### M1 of the messaging layer: worker-mailbox pattern on top of
### thread/new + cross-thread channels (see docs/internals/aot-plan.md).
###
### Share-nothing by construction: every message and reply marshals
### across the thread boundary, so handlers and payloads must be
### marshalable (closures over local state are fine -- they are copied).
###
### Surface:
###
###   (def mb (mailbox/spawn handler &opt capacity))
###   (mailbox/send  mb tag payload)               async, blocks at capacity
###   (mailbox/call  mb tag payload &opt timeout)  send + wait for the reply
###   (mailbox/quit  mb)                           graceful stop, joins worker
###
### Semantics: at-most-once per message (no acks); a dead worker makes
### sends block once the mailbox fills to capacity. Handlers that raise
### keep the worker alive; the caller gets a :mailbox-error reply.
###
### M2e pids: [:pid node serial chan] — VM-internal identity in the
### BEAM shape (never an OS pid). node is 0 on today's single-node
### cluster; serial comes from a per-VM counter; chan is the delivery
### address (channels marshal, so pids travel by value — verified).
### Equality is full-tuple equality; the chan element keeps distinct
### entities distinct even if (node, serial) ever collide across VMs.
### Resolution (pid -> worker handle) is node-local via *procs*;
### cross-node routing arrives with distribution.
###

(var *pid-serial* 0)
(var *procs* @{})  # serial -> {:pid :handle :chan}; same-VM only, never pruned

(defn- next-pid [chan]
  (def s *pid-serial*)
  (++ *pid-serial*)
  [:pid 0 s chan])

(defn pid?
  "True for [:pid node serial chan] tuples."
  [x]
  (and (tuple? x) (= 4 (length x)) (= :pid (x 0))))

(defn- resolve
  "Resolve a mailbox table or pid to {:pid :chan :handle-or-nil}.
  Unknown pids resolve the chan they carry with a nil handle."
  [m]
  (cond
    (and (table? m) (m :mbx))
    {:pid (m :pid) :chan (m :mbx) :handle (m :worker)}
    (pid? m)
    (do (def e (get *procs* (m 2)))
      (if e
        {:pid m :chan (e :chan) :handle (e :handle)}
        {:pid m :chan (m 3) :handle nil}))
    (error "not a pid or mailbox")))

(defn self
  "The calling worker's pid, or nil outside a mailbox worker."
  []
  (dyn :mailbox-self))

(defn spawn
  "Start a worker thread that dispatches [tag payload] messages to
  `handler`. Returns the mailbox handle (a table)."
  [handler &opt capacity]
  (default capacity 16)
  (def mbx (ev/thread-chan capacity))
  (def pid (next-pid mbx))
  (def mb @{:mbx mbx :worker nil :pid pid :trap-exit false})
  (def worker
    (thread/new
      (fn []
        (setdyn :mailbox-self pid)
        (var running true)
        # M2c: cooperative trap-exit. Default false (Erlang default).
        # Set via the :mailbox-trap control message (see trap-exit).
        (var trapping false)
        (while running
          (def [tag payload reply-ch] (ev/take mbx))
          (cond
            (= tag :mailbox-quit)
            (do
              (set running false)
              (when reply-ch (ev/give reply-ch :mailbox-stopped)))
            # Trap flag control: [:mailbox-trap bool nil]. FIFO with all
            # other traffic, so a :down queued before it keeps the old
            # flag value.
            (= tag :mailbox-trap)
            (set trapping (if payload true false))
            # M2 monitor/link traffic (see monitor/link below): the
            # :down tag is reserved. Monitor notifications are always
            # observed. Link legs carry a [:link tag] marker and obey
            # the trap flag: abnormal reasons propagate by killing this
            # worker unless trapping (or the reason is :normal/nil,
            # which never propagates); trapping workers observe
            # (tag=:down, payload=[tag reason]) and survive. The marker
            # is unwrapped before the handler sees it. No reply is sent.
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
              # reply only to real channels: a truthy non-channel in the
              # reply slot (e.g. a :down reason) is not a reply address.
              (when (and reply-ch (= :core/channel (type reply-ch)))
                (ev/give reply-ch (if ok r (table :mailbox-error r))))))))))
  (put mb :worker worker)
  (put *procs* (pid 2) {:pid pid :handle worker :chan mbx})
  mb)

(defn trap-exit
  "Set cooperative trap-exit on `mb` (M2c). A trapping worker observes
  linked/monitored abnormal exits as (:down [tag reason]) messages and
  survives; a non-trapping worker (default) exits with the peer's
  reason, cascading through the completion path. `:normal` (and nil)
  reasons never propagate. Accepts a mailbox or pid (the flag
  record on a pid argument is skipped — pids carry no mutable
  state). Returns its argument."
  [mb flag]
  (when (table? mb) (put mb :trap-exit (if flag true false)))
  (ev/give ((resolve mb) :chan) [:mailbox-trap (if flag true false) nil])
  mb)

(defn send
  "Fire-and-forget: enqueue [tag payload reply-ch]. Blocks if the
  mailbox is at capacity. Accepts a mailbox or pid. Returns its first
  argument for chaining."
  [mb tag payload &opt reply-ch]
  (ev/give ((resolve mb) :chan) [tag payload reply-ch])
  mb)

(defn call
  "Synchronous request/reply. Sends and waits for the worker's reply.
  With `timeout`, raises 'mailbox call timed out' if the reply does
  not arrive in time. Handler errors re-raise in the caller."
  [mb tag payload &opt timeout]
  (def rch (ev/thread-chan 1))
  (send mb tag payload rch)
  (def raw
    (if (nil? timeout)
      (ev/take rch)
      (let [tch (ev/chan 1)]
        (ev/go (fn [] (ev/sleep timeout) (ev/give tch :mailbox-timed-out)))
        # this build's ev/select yields [:take chan value] tuples
        (def sel (ev/select rch tch))
        (if (= tch (sel 1))
          (error "mailbox call timed out")
          (sel 2)))))
  (if (and (table? raw) (get raw :mailbox-error))
    (error (raw :mailbox-error))
    raw))

(defn quit
  "Ask the worker to stop, wait for the acknowledgement, and join the
  thread. Accepts a mailbox or a same-VM pid (resolved via *procs*).
  The mailbox must not be used afterwards."
  [mb]
  (def r (resolve mb))
  (def worker (r :handle))
  (when (nil? worker) (error "cannot quit unknown pid"))
  (def rch (ev/thread-chan 1))
  (send mb :mailbox-quit nil rch)
  (ev/take rch)
  (thread/join worker)
  mb)

###
### M2: monitors and links. A watched worker's exit is delivered to a
### watcher mailbox as monitor traffic: the watcher's handler observes
### (tag=:down, payload=[monitor-tag reason]), where reason is :normal
### on clean exit or the worker's error value on failure.
###

(defn monitor
  "Watch `watched`'s worker: when it exits, `watcher`'s mailbox
  receives the :down notification under `tag`. Both sides accept
  mailboxes or pids. A pid with no known same-VM worker delivers
  [:down tag :noproc] immediately. Live and finished-but-known
  workers go through ev/monitor (the late path delivers at once).
  Returns `watcher`."
  [watched watcher tag]
  (def w (resolve watcher))
  (def t (resolve watched))
  (def handle (t :handle))
  (if (nil? handle)
    (ev/give (w :chan) [:down tag :noproc])
    (ev/monitor handle tag (w :chan)))
  watcher)

(defn link
  "Symmetric link between two mailboxes: when either worker exits,
  the survivor's mailbox receives [:down tag reason]. Accepts
  mailboxes or (same-VM) pids. Link legs are marked [:link tag] so
  the loop can tell propagation traffic (trap-governed) from plain
  monitor notifications (always observed); the marker is unwrapped
  before handlers see it. Returns [a b]."
  [a b &opt tag]
  (default tag :link)
  (monitor a b [:link tag])
  (monitor b a [:link tag])
  [a b])

###
### M3: named registry sugar. ev/register/whereis/unregister are the
### runtime primitives (process-global, cross-thread); these bind a
### mailbox's pid under a name and resolve it back to a sendable pid.
###

(defn register
  "Bind `name` to `mb`'s pid in the process-global registry."
  [mb name]
  (ev/register name ((resolve mb) :pid))
  mb)

(defn whereis
  "Resolve `name` to a pid (or nil). The pid plugs straight into
  send/call/monitor/link."
  [name]
  (ev/whereis name))
