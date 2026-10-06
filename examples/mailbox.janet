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
        (while running
          (def [tag payload reply-ch] (ev/take mbx))
          (if (= tag :mailbox-quit)
            (do
              (set running false)
              (when reply-ch (ev/give reply-ch :mailbox-stopped)))
            (let [[ok r] (protect (handler tag payload))]
              (when reply-ch
                (ev/give reply-ch (if ok r (table :mailbox-error r))))))))))
  @{:mbx mbx :worker worker})

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
