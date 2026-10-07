###
### M4a of the messaging layer: pooled warm-VM task executors
### (see docs/internals/aot-plan.md). The first staged slice toward M4:
### M one-shot tasks multiplexed over N long-lived worker VMs, with
### none of thread/new's per-task spawn cost — the cheap-restart
### substrate supervision at scale needs. Pure janet, zero runtime
### code: one shared thread-chan task queue + N mailbox-style loops.
###
### Contrast with M1 mailboxes: a mailbox is ONE worker with a private
### queue and a fixed handler; a pool is N workers on a SHARED queue
### running caller-supplied functions. Contrast with thread/new: no
### spawn per task, and errors come back as values on a reply channel
### instead of killing a join.
###
### Surface:
###
###   (def p (pool/spawn nthreads &opt capacity))
###   (pool/call   p func args &opt timeout)  submit + wait for result
###   (pool/submit p func args)               submit, returns reply chan
###   (pool/map    p func arglist)            fan-out, results in order
###   (pool/stop   p)                         drain-stop + join workers
###
### Semantics: at-most-once per task (same as mailboxes); task errors
### re-raise in the collector and never kill a worker; functions and
### args marshal into the worker VM (closures over local state work —
### they are copied). FIFO per queue; no ordering across workers.
###
### Note: pool/map shadows core map inside this module, so internal
### uses go through core-map (captured before the shadowing defn).
###

(def- core-map map)

(defn- worker-loop
  [q]
  (var running true)
  (while running
    (def [tag func args reply-ch] (ev/take q))
    (if (= tag :pool-stop)
      (set running false)
      (let [[ok r] (protect (apply func args))]
        (when (and reply-ch (= :core/channel (type reply-ch)))
          (ev/give reply-ch (if ok r (table :pool-error r))))))))

(defn spawn
  "Start a pool of `nthreads` worker VMs sharing one task queue.
  Returns the pool handle (a table)."
  [nthreads &opt capacity]
  (default capacity 1024)
  (unless (and (int? nthreads) (pos? nthreads))
    (error "pool thread count must be a positive integer"))
  (def q (ev/thread-chan capacity))
  (def workers (core-map (fn [_] (thread/new worker-loop q)) (range nthreads)))
  @{:queue q :workers workers})

(defn submit
  "Enqueue [func args]; returns a fresh reply channel carrying the
  result (or a :pool-error table on failure)."
  [p func args]
  (def rch (ev/thread-chan 1))
  (ev/give (p :queue) [:pool-call func args rch])
  rch)

(defn- collect
  [rch timeout]
  (def raw
    (if (nil? timeout)
      (ev/take rch)
      (let [tch (ev/chan 1)]
        (ev/go (fn [] (ev/sleep timeout) (ev/give tch :pool-timed-out)))
        # this build's ev/select yields [:take chan value] tuples
        (def sel (ev/select rch tch))
        (if (= tch (sel 1))
          (error "pool call timed out")
          (sel 2)))))
  (if (and (table? raw) (get raw :pool-error))
    (error (raw :pool-error))
    raw))

(defn call
  "Submit [func args] and wait for the result. With `timeout`,
  raises 'pool call timed out' instead of waiting forever. Task
  errors re-raise in the caller; the worker survives."
  [p func args &opt timeout]
  (collect (submit p func args) timeout))

(defn map
  "Fan `func` over `arglist` (each element is the arg ARRAY for one
  call) and collect results in order."
  [p func arglist &opt timeout]
  (def chans (core-map (fn [args] (submit p func args)) arglist))
  (core-map (fn [ch] (collect ch timeout)) chans))

(defn stop
  "Drain-stop: one :pool-stop pill per worker, then join all workers.
  Tasks already queued run before the pills (FIFO). The pool must not
  be used afterwards."
  [p]
  (each _ (p :workers)
    (ev/give (p :queue) [:pool-stop nil nil nil]))
  (each w (p :workers) (thread/join w))
  p)
