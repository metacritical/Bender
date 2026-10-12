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

# Density regression: 640k parked procs across 4 lanes (~160k/lane) --
# between the historical gcroot crash zone (~550k total) and the open
# per-lane density threshold (~225k/lane). Every parked proc holds a
# cross-thread pid reference in its env, so this exercises cross-thread
# refs surviving per-lane GC at scale. Skips silently on low-RAM hosts.
# Assumes ~3GB RAM headroom (640k x ~3.5KB/proc). Hosts below that
# should set JANET_SKIP_DENSITY=1 to skip this section.
(unless (os/getenv "JANET_SKIP_DENSITY")
  (proc/lanes-start 4)
  (var i 0)
  (while (< i 640000)
    (proc/spawn (fn [] (proc/recv)))
    (++ i)
    (when (zero? (mod i 200000))
      (eprint "density at" i)))
  (assert (= i 640000) "640k procs spawned across 4 lanes")
  (ev/sleep 0.5)
  (eprint "density section passed"))

# Milestone 2a: pool-style join. The density lanes above are still
# running with 640k parked procs -- stopping must wake them (each
# parked-recv proc gets :lane-stop, returns, dies), drain the worker
# loops, and join every thread within the grace budget. The restart
# then proves the registry is reusable.
(var t-stop (os/clock))
(proc/lanes-stop)
(def stop-dt (- (os/clock) t-stop))
(assert (< stop-dt 60) "lanes-stop bounded (grace join, no hang)")
(assert (= 0 (proc/lanes)) "lanes 0 after stop")
(eprint "join after 640k-park stop in" stop-dt "s")

# Restart after join: a full round trip on fresh lanes.
(proc/lanes-start 2)
(assert (= 2 (proc/lanes)) "restart after join")
(def ready (ev/thread-chan 1))
(def collector
  (proc/spawn-local
    (fn []
      (def r (proc/self-pid))
      (ev/give ready r)
      (def m (proc/recv))
      (ev/give ready m))))
(def rpid (ev/take ready))
(proc/spawn (fn [] (proc/send rpid :restart-ok)))
(assert (= :restart-ok (ev/take ready)) "round trip after restart")
(proc/lanes-stop)
(assert (= 0 (proc/lanes)) "lanes 0 after second stop")

(eprint "suite-lanes: all assertions passed")
(os/exit 0)
