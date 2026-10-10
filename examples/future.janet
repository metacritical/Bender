### futures + promises over the Janet pool (examples/pool.janet).
###
### Futures: `(future & body)` runs the body on a shared worker pool and
### returns immediately; `deref` blocks for the value (unlike
### `thread/join`, any number of consumers may deref, and results are
### cached — errors re-raise on every deref). Cancellation is
### cooperative/best-effort: a cancelled future's `deref` raises, but an
### already-running worker runs to completion (no preemptive kill —
### same boundary as the rest of this runtime).
###
### Promises: `(promise)` + `(deliver p v)` (first delivery wins,
### later ones are silent no-ops) + blocking `deref` with optional
### timeout. Backed by a plain channel — same-VM only.

(import ./pool :as pool)

(var *default-pool* nil)

(defn- default-pool []
  # Daemon workers: the shared pool outlives any explicit stop, and
  # non-daemon threads would pin process exit on their refcount.
  (when (nil? *default-pool*)
    (set *default-pool* (pool/spawn (os/cpu-count 4) 1024 :d)))
  *default-pool*)

(defn future?
  "True for future handles."
  [x]
  (and (table? x) (= :future (get x :kind))))

(defn- settle-future-raw
  "Cache a pool reply: tagged [:future-value v] or a :pool-error
  table. Returns the value or raises the carried error."
  [fut raw]
  (def cache (fut :cache))
  (if (and (table? raw) (get raw :pool-error))
    (do (put cache :settled true)
      (put cache :ok false)
      (put cache :value (get raw :pool-error))
      (error (get raw :pool-error)))
    (do (put cache :settled true)
      (put cache :ok true)
      (put cache :value (raw 1))
      (raw 1))))

(defn- settle-promise-raw
  "Cache a promise delivery [:promise-value v]. Returns v."
  [prom raw]
  (put prom :delivered true)
  (raw 1))

(defn- cached-or-raise
  "Read back a settled future cache (value or re-raised error)."
  [fut]
  (def cache (fut :cache))
  (if (get cache :ok)
    (get cache :value)
    (error (get cache :value))))

(defn- await-chan
  "Take from `rch`, caching into the future/promise. With `timeout`
  (seconds), raises 'future timed out' instead of waiting forever.
  Pool replies are tagged [:future-value v] and promise deliveries
  [:promise-value v], so a nil take unambiguously means nothing has
  arrived (plain nils never sit in these channels). Mutual exclusion
  rides a cap-1 lock channel (C calls are quantum-atomic, so
  try-take acquisition cannot interleave): exactly one taker consumes;
  the rest wait on the shared cache with their own deadlines."
  [fut rch timeout]
  (def is-fut (future? fut))
  (def cache (if is-fut (fut :cache) (fut :pcache)))
  (def lock (fut :lock))
  (when (and is-fut (get fut :cancelled) (not (get cache :settled)))
    (error "future cancelled"))
  (if (get cache :settled)
    (if (and is-fut (not (get cache :ok)))
      (error (get cache :value))
      (get cache :value))
    (if (nil? (ev/try-take lock))
      (do (def deadline (if (nil? timeout) nil (+ (os/clock) timeout)))
        (while (not (get cache :settled))
          (when (and deadline (> (os/clock) deadline))
            (error "future timed out"))
          (ev/sleep 0.01))
        (if (and is-fut (not (get cache :ok)))
          (error (get cache :value))
          (get cache :value)))
      (do (def res (protect
                          (do (def raw
                                (if (nil? timeout)
                                  (ev/take rch)
                                  (let [tch (ev/chan 1)]
                                    (ev/go (fn [] (ev/sleep timeout) (ev/give tch :future-timed-out)))
                                    (def sel (ev/select rch tch))
                                    (if (= tch (sel 1))
                                      (error "future timed out")
                                      (sel 2)))))
                            (when (and is-fut (get fut :cancelled))
                              (if is-fut
                                (do (put (fut :cache) :settled true)
                                  (put (fut :cache) :ok false)
                                  (put (fut :cache) :value "future cancelled"))
                                nil)
                              (error "future cancelled"))
                            (if is-fut
                              (do (def cache (fut :cache))
                                (if (and (table? raw) (get raw :pool-error))
                                  (do (put cache :settled true)
                                    (put cache :ok false)
                                    (put cache :value (get raw :pool-error))
                                    [:err (get raw :pool-error)])
                                  (do (put cache :settled true)
                                    (put cache :ok true)
                                    (put cache :value (raw 1))
                                    [:ok (raw 1)])))
                              (do (put (fut :pcache) :settled true)
                                (put (fut :pcache) :value (raw 1))
                                [:ok (raw 1)])))))
        (ev/give lock :token)
        (def [rok rval] res)
        (if rok
          (if (= :ok (rval 0)) (rval 1) (error (rval 1)))
          (error rval))))))

(defn future*
  "Run zero-argument `thunk` on pool `p`, returning a future handle
  immediately (see `future`, `deref`)."
  [p thunk]
  (unless (function? thunk)
    (error "future* expects a function"))
  (def rch (pool/submit p (fn [] [:future-value (thunk)]) []))
  (def lock (ev/chan 1))
  (ev/give lock :token)
  @{:kind :future :rch rch :cache @{} :cancelled false :lock lock})

(defmacro future
  ``Run `body` on the default shared pool, returning a future handle
  immediately. `deref` blocks for the value (see `deref`). Non-core
  callees are captured by value (macro hygiene: the expansion must
  resolve anywhere).``
  [& body]
  (let [mk future* getpool default-pool]
    ~((fn [m* p*] (m* (p*) (fn [] ,;body))) ,mk ,getpool)))

(defn deref
  ``Block until `fut-or-prom` delivers. With `timeout` (seconds) raises
  'future timed out' instead of waiting forever; with `timeout` and
  `timeout-val`, returns `timeout-val` on expiry. Body errors re-raise
  here (and on every later deref — results cache).``
  [fut-or-prom &opt timeout timeout-val]
  (def is-fut (future? fut-or-prom))
  (def rch (if is-fut (fut-or-prom :rch) (fut-or-prom :chan)))
  (if (and (not (nil? timeout)) (not (nil? timeout-val)))
    (do (def [ok v] (protect (await-chan fut-or-prom rch timeout)))
      (if ok v timeout-val))
    (await-chan fut-or-prom rch timeout)))

(defn realized?
  "True once the value is available (non-blocking; ready values settle
  into the cache, so peeking never consumes anything observable).
  Peeks under the same lock as `deref` (never steals an in-flight
  reply from a suspended taker)."
  [fut-or-prom]
  (def is-fut (future? fut-or-prom))
  (if is-fut
    (do (def cache (fut-or-prom :cache))
      (if (get cache :settled)
        true
        (if (nil? (ev/try-take (fut-or-prom :lock)))
          false
          (do (def v (ev/try-take (fut-or-prom :rch)))
            (ev/give (fut-or-prom :lock) :token)
            (if (nil? v)
              false
              (do (protect (settle-future-raw fut-or-prom v))
                true))))))
    (= true (get fut-or-prom :delivered))))

(defn future-cancel
  "Best-effort cancel: later `deref`s raise 'future cancelled'. A
  worker already running runs to completion (its result is dropped).
  Returns the future."
  [fut]
  (put fut :cancelled true)
  fut)

(defn promise?
  "True for promise handles."
  [x]
  (and (table? x) (= :promise (get x :kind))))

(defn promise
  "A single-assignment box: `deliver` once, `deref` (blocking) many.
  Same-VM only (plain channel inside)."
  []
  (def lock (ev/chan 1))
  (ev/give lock :token)
  @{:kind :promise :chan (ev/chan 1) :delivered false :pcache @{} :lock lock})

(defn deliver
  "Deliver `v` to promise `p`. First delivery wins; later ones are
  silent no-ops returning nil. Returns `p` on the winning delivery.
  Values ride tagged internally so a delivered nil is distinct from
  an empty box."
  [p v]
  (if (get p :delivered)
    nil
    (do (put p :delivered true)
      (ev/give (p :chan) [:promise-value v])
      p)))
