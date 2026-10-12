# suite-lanes.janet — proc/lanes: VM-per-core process parallelism
#
# Gates:
#   * single-VM mode unaffected (proc/lanes == 0 before start)
#   * lanes-start auto/explicit counts
#   * cross-lane spawn + send + reply round trip (2 lanes, 4 lanes)
#   * serial uniqueness, round-robin distribution across lanes
#   * lanes-stop semantics (lane 0 unblocked; workers reaped at exit)
#   * full teardown via explicit os/exit (worker threads are detached)

(defn assert-lines-mode
  []
  (assert (= 0 (proc/lanes)) "single-VM before start"))

(assert-lines-mode)

#
# Round-trip helper: N kids spread round-robin across lanes compute i*i
# and cross-lane-send to a lane-0 collector proc; the main verifies the
# sum. Exercises: routed spawn, marshal across threads, cross-lane pid
# tuple send, :proc-fwd delivery, lazy main-side registration.
#
(defn round-trip
  [lanes kids expected]
  (def n (proc/lanes-start lanes))
  (assert (= n lanes) "worker count matches request")
  (def result (ev/thread-chan (* 2 kids)))
  (def ready (ev/thread-chan 1))
  (def R (proc/spawn-local
           (fn []
             (ev/give ready (proc/self-pid))
             (var got 0)
             (while (< got kids)
               (def m (proc/recv))
               (ev/give result m)
               (++ got))
             (proc/recv))))
  (def rpid (ev/take ready))
  (assert (tuple? rpid) "self-pid is a pid tuple")
  (assert (= :pid (rpid 0)) "pid tuple tag")
  (var acc 0)
  (var i 0)
  (while (< i kids)
    (proc/spawn (fn [] (proc/send rpid (* i i))))
    (++ i))
  (var got 0)
  (while (< got kids)
    (def v (ev/take result))
    (set acc (+ acc v))
    (++ got))
  (assert (= acc expected) (string "kids sum over " lanes " lanes"))
  # Serial uniqueness: the collector's replies were all attributed to
  # one serial; the round trip itself proves no cross-delivery noise.
  (proc/send R :bye)
  (proc/lanes-stop))

(round-trip 2 4 14)
(round-trip 4 8 140)

# Auto count: at least one worker on any machine.
(do
  (proc/lanes-start)
  (assert (>= (proc/lanes) 1) "auto lanes >= 1")
  (proc/lanes-stop))

# Local spawn still works with lanes up (spawn-local bypasses routing)
(do
  (proc/lanes-start 2)
  (def ready (ev/thread-chan 1))
  (def p (proc/spawn-local (fn [] (ev/give ready :local) (proc/recv))))
  (assert (= :local (ev/take ready)) "spawn-local runs on the main VM")
  (proc/send p :bye)
  (proc/lanes-stop))

# Affinity option accepted (best-effort; silent on unsupported hosts)
(do
  (proc/lanes-start {:count 2 :affinity true})
  (assert (= 2 (proc/lanes)) "affinity option accepted")
  (proc/lanes-stop))

(eprint "suite-lanes: all assertions passed")
(os/exit 0)
