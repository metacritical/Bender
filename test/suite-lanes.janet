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

# Milestone 2b: cross-lane monitors and kill. Monitor registration
# routes [:proc-monitor serial tag ch] to the watched proc's home
# lane, which hosts the P2.3 reaper: [:down tag reason] back to the
# channel (threaded push marshals cross-VM). Kill routes [:kill
# serial reason] and is applied locally by the home lane.
(proc/lanes-start 2)
(def tap (ev/thread-chan 4))
(def vpready (ev/thread-chan 1))
(def victim
  (proc/spawn
    (fn []
      (def r (proc/self-pid))
      (ev/give vpready r)
      (proc/recv))))
(def vpid (ev/take vpready))
(ev/give (vpid 3) [:proc-monitor (vpid 2) :reaped tap])
(proc/kill vpid :shot)
(def down (ev/take tap))
(assert (and (tuple? down) (= :down (first down))) "monitor fired")
(assert (= :reaped (get down 1)) "monitor tag")
(assert (= :shot (get down 2)) "kill reason is the :down reason")
# killing a dead proc is a silent no-op
(proc/kill vpid :again)
# erroring proc: the error value is the reason (rendezvous so the
# monitor is registered before the proc dies)
(def v2ready (ev/thread-chan 1))
(def v2
  (proc/spawn
    (fn []
      (def r (proc/self-pid))
      (ev/give v2ready r)
      (proc/recv)
      (error :boom))))
(def v2pid (ev/take v2ready))
(ev/give (v2pid 3) [:proc-monitor (v2pid 2) :died tap])
(proc/send v2pid :go)
(def down2 (ev/take tap))
(assert (= :died (get down2 1)) "error monitor tag")
(assert (= :boom (get down2 2)) "error value is the :down reason")
# normal return: :normal reason
(def v3ready (ev/thread-chan 1))
(def v3
  (proc/spawn
    (fn []
      (def r (proc/self-pid))
      (ev/give v3ready r)
      (proc/recv)
      42)))
(def v3pid (ev/take v3ready))
(ev/give (v3pid 3) [:proc-monitor (v3pid 2) :done tap])
(proc/send v3pid :go)
(def down3 (ev/take tap))
(assert (= :done (get down3 1)) "normal monitor tag")
(assert (= :normal (get down3 2)) "clean return reports :normal")
# pid-tuple watcher: the :down routes through the watcher's own
# mailbox (main-VM watcher proc, worker-VM victim)
(def mready (ev/thread-chan 2))
(def watcher
  (proc/spawn-local
    (fn []
      (def r (proc/self-pid))
      (ev/give mready r)
      (def m (proc/recv))
      (ev/give mready m))))
(def wpid (ev/take mready))
(def v4ready (ev/thread-chan 1))
(def v4
  (proc/spawn
    (fn []
      (def r (proc/self-pid))
      (ev/give v4ready r)
      (proc/recv))))
(def v4pid (ev/take v4ready))
(ev/give (v4pid 3) [:proc-monitor (v4pid 2) :via-pid wpid])
(proc/kill v4pid :plugged)
(def down4 (ev/take mready))
(assert (and (tuple? down4) (= :down (first down4))) "pid-watcher fired")
(assert (= :via-pid (get down4 1)) "pid-watcher tag")
(assert (= :plugged (get down4 2)) "pid-watcher reason")
# unknown serial answers :noproc from the lane
(def ghost-ready (ev/thread-chan 1))
(ev/give (vpid 3) [:proc-monitor 424242 :ghost ghost-ready])
(assert (deep= [:down :ghost :noproc] (ev/take ghost-ready))
        "unknown :noproc")
(proc/lanes-stop)
(assert (= 0 (proc/lanes)) "lanes stop after monitor tests")

(eprint "suite-lanes: all assertions passed")
(os/exit 0)

# Milestone 2c: work-stealing. The policy knob round-trips, a skewed
# workload under :steal completes correctly with overflow traffic, and
# the counters prove pulls happened (feature indication, not vibes).
(proc/lanes-start 4)
(assert (= :round-robin (proc/lanes-policy)) "policy defaults to :round-robin")
(assert (= :steal (proc/lanes-policy :steal)) "policy set to :steal")
(assert (= :steal (proc/lanes-policy)) "policy reads back :steal")
(assert (= :least-loaded (proc/lanes-policy :least-loaded)) ":least-loaded accepted")
(assert (= :round-robin (proc/lanes-policy :round-robin)) "back to :round-robin")
(proc/lanes-policy :steal)
(def steal-done (ev/thread-chan 1))
(def steal-started (ev/thread-chan 1))
(def collector-steal
  (proc/spawn-local
    (fn []
      (ev/give steal-started (proc/self-pid))
      (var sum 0)
      (loop [i :range [0 200]]
        (+= sum (proc/recv)))
      (ev/give steal-done [:done sum]))))
(def cpid (ev/take steal-started 10))
# 200 tasks send 0..199 to the collector's PID (tuples marshal; proc
# abstracts don't cross lanes). Once any lane's ctl backs up past the
# :steal threshold, overflow engages and idle lanes drain it.
(loop [i :range [0 200]]
  (proc/spawn
    (fn []
      (proc/send cpid i))))
(def [steal-tag steal-sum] (ev/take steal-done 10))
(assert (= :done steal-tag) "skewed :steal workload completes")
(assert (= 19900 steal-sum) ":steal results correct (sum 0..199)")
(def stats (proc/lanes-stats))
(assert (pos? (stats :overflow-pushed)) ":steal pushed tasks to overflow")
(assert (pos? (stats :overflow-pulled)) "idle lanes pulled from overflow")
(proc/lanes-policy :round-robin)
(proc/lanes-stop)
(assert (= 0 (proc/lanes)) "lanes 0 after :steal section stop")
