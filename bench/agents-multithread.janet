# bench/agents-multithread.janet — multi-OS-thread agents throughput.
#
# Spawns THREADS independent OS threads (thread/new, share-nothing
# isolated VMs), each running its OWN self-contained copy of the
# bench/agents.janet actor-burst workload (N local procs, T ticks each)
# fully in parallel on real cores -- not a single VM splitting one
# workload across lanes (see bench/agents-lanes.janet for that shape).
# Reports per-thread and aggregate (sum of msgs / max wall time)
# throughput, so single-VM numbers can be compared against real
# multi-core scaling.
#
# Usage: bender bench/agents-multithread.janet [N] [T] [THREADS]
#   N       = actor count PER THREAD (default 100)
#   T       = ticks per actor (default 4)
#   THREADS = worker OS thread count (default 20)

(def args (dyn :args))
(def n (if (> (length args) 1) (parse (get args 1)) 100))
(def ticks (if (> (length args) 2) (parse (get args 2)) 4))
(def nthreads (if (> (length args) 3) (parse (get args 3)) 20))

# Each worker thread runs this function in its OWN isolated VM.
# value = [n ticks] (marshalled in). Returns a result table
# (marshalled back) with this thread's own timings.
(defn worker [value]
  (def [wn wticks] value)
  (def actor
    (fn []
      (var acc 0)
      (var remaining wticks)
      (while (pos? remaining)
        (def msg (proc/recv))
        (set acc (+ acc (* 3 (if (number? msg) msg 1))))
        (set acc (mod acc 1000003))
        (-- remaining))
      acc))
  (var spawned 0)
  (def procs (array/new wn))
  (def t0 (os/clock))
  (while (< spawned wn)
    (put procs spawned (proc/spawn actor 16))
    (++ spawned))
  (def spawn-time (- (os/clock) t0))
  (ev/sleep 0.05)
  (def t1 (os/clock))
  (var sent 0)
  (var round 0)
  (while (< round wticks)
    (var i 0)
    (while (< i wn)
      (proc/send (get procs i) 1)
      (++ i))
    (++ round)
    (set sent (+ sent wn)))
  (def tick-time (- (os/clock) t1))
  {:n wn :ticks wticks :spawn-time spawn-time :sent sent :tick-time tick-time})

(eprint (string "agents-multithread: threads=" nthreads " n-per-thread=" n
                " ticks-per-actor=" ticks " (total actors=" (* nthreads n) ")"))

(def t-start (os/clock))
(var handles @[])
(var i 0)
(while (< i nthreads)
  (array/push handles (thread/new worker [n ticks]))
  (++ i))

(var results @[])
(each h handles
  (array/push results (thread/join h)))
(def wall-time (- (os/clock) t-start))

(var total-sent 0)
(var max-tick-time 0)
(var max-spawn-time 0)
(each r results
  (set total-sent (+ total-sent (r :sent)))
  (set max-tick-time (max max-tick-time (r :tick-time)))
  (set max-spawn-time (max max-spawn-time (r :spawn-time))))

(eprint (string "per-thread: spawn<=" max-spawn-time "s tick<=" max-tick-time "s"))
(eprint (string "aggregate: " total-sent " msgs across " nthreads
                " threads in " wall-time "s wall ("
                (math/floor (/ total-sent wall-time)) " msgs/s aggregate, "
                (math/floor (/ total-sent (* nthreads max-tick-time))) " msgs/s per-thread avg)"))
(eprint "PASS")
