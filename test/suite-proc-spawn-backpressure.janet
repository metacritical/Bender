# test/suite-proc-spawn-backpressure.janet
# Full lane control channel: proc/spawn must return a PID, never the write
# channel. Run in a fresh process with JANET_LANES=0.
#
# Forcing the parked branch deterministically (each fact measured on this build):
#   * setdyn at top level writes the CURRENT fiber env, but lanes-start reads
#     root_fiber->env first, so the worker override must go into the root env.
#   * try-give refusal is not proof of parking (the dispatcher keeps draining),
#     so the fill loop is driven by ev/count against ev/capacity.
(def ready (ev/thread-chan 1))
(def gate (ev/thread-chan 1))
(def returned (ev/thread-chan 2))
(def real-worker (dyn :proc-lane-worker))

(defn gated-worker [launch]
  (def [ctl lane-id] launch)
  # Only gates real workers; main lane 0 delegates immediately.
  (when (> lane-id 0)
    (ev/give ready [ctl lane-id])
    (ev/take gate))
  (real-worker launch))

(def root-env (fiber/getenv (fiber/root)))
(def old-worker (get root-env :proc-lane-worker))
(put root-env :proc-lane-worker gated-worker)
(assert (= 1 (proc/lanes-start 1)))
(put root-env :proc-lane-worker old-worker)

(def [ctl lane-id] (ev/take ready))
(assert (= lane-id 1))

# Startup already queued an overflow descriptor, so fill to capacity, not by
# a fixed count.
(def capacity (ev/capacity ctl))
(while (< (ev/count ctl) capacity) (ev/give ctl [:test-noop]))
(assert (= (ev/count ctl) capacity))

(defn spawn-tail [] (proc/spawn (fn [] (ev/give (proc/recv) :ok)) 8))

(def ordinary
  (ev/go (fn []
    (def pid (proc/spawn (fn [] (ev/give (proc/recv) :ok)) 8))
    (ev/give returned [:ordinary pid]))))
(def tail (ev/go (fn [] (ev/give returned [:tail (spawn-tail)]))))

(ev/sleep 0)
(assert (= :suspended (fiber/status ordinary)))
(assert (= :suspended (fiber/status tail)))
(gccollect)

# Cancellation must propagate an error, never become a successful PID return.
(def canceled (ev/thread-chan 1))
(def cancel-target
  (ev/go (fn []
    (try
      (do (spawn-tail) (ev/give canceled :unexpected-success))
      ([err] (ev/give canceled err))))))
(ev/sleep 0)
(assert (= :suspended (fiber/status cancel-target)))
(ev/cancel cancel-target :spawn-canceled)
(assert (= :spawn-canceled (ev/take canceled)))
(gccollect)

# Release and check the resumed values.
(ev/give gate true)
(def first (ev/take returned))
(def second (ev/take returned))
(defn check-pid [pid]
  (assert (and (tuple? pid) (= 4 (length pid)) (= :pid (pid 0)))
          "resumed proc/spawn must return PID, not the write channel")
  (assert (= lane-id (pid 1)))
  (assert (number? (pid 2)))
  (assert (= ctl (pid 3))))
(check-pid (first 1))
(check-pid (second 1))
(assert (not= ((first 1) 2) ((second 1) 2)))

# Both resumed procs must be reachable and answer.
(def replies (ev/thread-chan 2))
(proc/send (first 1) replies)
(proc/send (second 1) replies)
(assert (= :ok (ev/take replies)))
(assert (= :ok (ev/take replies)))

# Immediate (never parked) path must produce the same shape.
(def immediate (spawn-tail))
(check-pid immediate)
(proc/send immediate replies)
(assert (= :ok (ev/take replies)))

(proc/lanes-stop)
(print "suite-proc-spawn-backpressure PASSED")
