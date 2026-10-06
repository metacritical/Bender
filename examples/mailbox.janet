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

(defn spawn
  "Start a worker thread that dispatches [tag payload] messages to
  `handler`. Returns the mailbox handle (a table)."
  [handler &opt capacity]
  (default capacity 16)
  (def mbx (ev/thread-chan capacity))
  (def worker
    (thread/new
      (fn []
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
            # M2 monitor traffic (see monitor/link below): the :down tag
            # is reserved. Abnormal exits propagate by killing this
            # worker with the peer's reason unless trapping (or the
            # reason is :normal/nil, which never propagates); trapping
            # workers observe (tag=:down, payload=[monitor-tag reason])
            # and survive. No reply is ever sent.
            (= tag :down)
            (do
              (def reason reply-ch)
              (when (and (not trapping)
                         (not= reason :normal)
                         (not (nil? reason)))
                (error reason))
              (handler :down [payload reason]))
            (let [[ok r] (protect (handler tag payload))]
              # reply only to real channels: a truthy non-channel in the
              # reply slot (e.g. a :down reason) is not a reply address.
              (when (and reply-ch (= :core/channel (type reply-ch)))
                (ev/give reply-ch (if ok r (table :mailbox-error r))))))))))
  @{:mbx mbx :worker worker :trap-exit false})

(defn trap-exit
  "Set cooperative trap-exit on `mb` (M2c). A trapping worker observes
  linked/monitored abnormal exits as (:down [tag reason]) messages and
  survives; a non-trapping worker (default) exits with the peer's
  reason, cascading through the completion path. `:normal` (and nil)
  reasons never propagate. Returns the mailbox."
  [mb flag]
  (put mb :trap-exit (if flag true false))
  (ev/give (mb :mbx) [:mailbox-trap (if flag true false) nil])
  mb)

(defn send
  "Fire-and-forget: enqueue [tag payload reply-ch]. Blocks if the
  mailbox is at capacity. Returns the mailbox for chaining."
  [mb tag payload &opt reply-ch]
  (ev/give (mb :mbx) [tag payload reply-ch])
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
  thread. The mailbox must not be used afterwards."
  [mb]
  (def rch (ev/thread-chan 1))
  (send mb :mailbox-quit nil rch)
  (ev/take rch)
  (thread/join (mb :worker))
  mb)

###
### M2: monitors and links. A watched worker's exit is delivered to a
### watcher mailbox as monitor traffic: the watcher's handler observes
### (tag=:down, payload=[monitor-tag reason]), where reason is :normal
### on clean exit or the worker's error value on failure.
###

(defn monitor
  "Watch `watched`'s worker: when it exits, `watcher`'s mailbox
  receives the :down notification under `tag`. Returns `watcher`."
  [watched watcher tag]
  (ev/monitor (watched :worker) tag (watcher :mbx))
  watcher)

(defn link
  "Symmetric link between two mailboxes: when either worker exits,
  the survivor's mailbox receives [:down tag reason]. Returns [a b]."
  [a b &opt tag]
  (default tag :link)
  (monitor a b tag)
  (monitor b a tag)
  [a b])
