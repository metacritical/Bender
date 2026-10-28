# bench/cross-runtime/wave-phased.janet
#
# Phase-labelled sibling of wave.janet. The wave itself is IDENTICAL: the same
# N-process dependency chain, the same increment-and-forward actor body, the
# same channel capacities, the same push of 0 into the head and the same sink
# handoff. This file adds phase markers and millisecond-decimal timings on
# stderr; it does not reshape the graph, sample per actor, or count anything
# per node (per-actor sampling would add work to the very path being timed).
#
# Contract preserved from wave.janet:
#   * stdout stays deterministic and byte-identical in shape:
#       procs=N result=N
#     so AOT/PGO parity can compare stdout across builds.
#   * result is asserted equal to N before any output, exactly as before.
#   * timings go to stderr only.
#
# What this adds (stderr, in this order):
#   phase=spawn-start    spawn_start_us=<monotonic microseconds>
#   phase=spawn-end      spawn_end_us=..  spawn_ms=<%.3f>
#   phase=wave-start     wave_start_us=..
#   phase=wave-end       wave_end_us=..   wave_ms=<%.3f>
#   phase=shutdown-start shutdown_start_us=..
#   phase=shutdown-end   shutdown_end_us=.. shutdown_ms=<%.3f>
# Each timestamp key is distinct on purpose: a single reused `t_us` appears six
# times with six different values, which a key=value collector reports as a
# conflict. Unique keys keep the six phases independently addressable.
#   procs=N result=N spawn_ms=.. wave_ms=.. total_ms=..   (final summary)
#   lanes=<n> placement=<...>                             (see below)
#
# Lane placement evidence -- READ THIS BEFORE QUOTING IT:
#   The runtime exposes no per-actor lane/VM identity from script. proc/self-pid
#   returns the portable pid tuple [:pid lane serial ctl], but that tuple's
#   lane slot is hardcoded to the literal 1 in src/core/proc.c
#   (cfun_proc_self_pid), i.e. it is a shape marker, NOT the executing worker.
#   proc/lanes returns only how many worker VMs exist, and proc/lanes-stats
#   reports overflow-queue push/pull counts. None of these identify which lane
#   a given actor ran on.
#
#   Therefore this harness reports placement as "unknown" and never infers a
#   lane from the chain index or from the lane count. It records only what is
#   observable: the configured worker count and the pid tuple's lane field,
#   explicitly labelled as structural rather than runtime placement.
#
# Run:  JANET_LANES=11 ./build/bender bench/cross-runtime/wave-phased.janet <n>
#       JANET_LANES=0  ./build/bender bench/cross-runtime/wave-phased.janet <n>
# N is a required integer >= 1. There is no upper cap here beyond a 32-bit
# integer; memory is the real bound. Non-integer or non-positive N is rejected.

(defn- die [msg]
  (eprintf "wave-phased: %s" msg)
  (os/exit 2))

# --- monotonic clock, microsecond resolution --------------------------------
# os/clock :monotonic returns a double of seconds; :tuple returns
# [seconds nanoseconds]. The tuple form avoids losing microseconds when the
# machine has been up for a long time, so prefer it and fall back to the
# double if the tuple shape is ever unavailable.
(defn- clock->us [t]
  # Convert (os/clock :monotonic :tuple) to integer microseconds.
  #
  # The tuple is [tv_sec tv_nsec] -- verified against the parent runtime, where
  # (1394009 105833000) and the :double form 1394009.105833 agree:
  #   sec*1000000 + ns/1000 = 1394009105833  (== round(double*1000000))
  # So the whole-second term is scaled by 1000000, NOT by 1000. Scaling it by
  # 1000 mixes milliseconds with microseconds: inside one second the two terms
  # agree, but across a second boundary the result DROPS by ~999000 us and
  # every phase duration goes negative. Do not reintroduce a scale heuristic
  # here either -- inferring "ns vs us" from a small fraction misreads a
  # genuine sub-microsecond ns value near a boundary.
  (+ (* 1000000 (get t 0)) (math/floor (/ (get t 1) 1000))))

(defn- now-us []
  (def t (os/clock :monotonic :tuple))
  (if (and (tuple? t) (= 2 (length t)))
    (clock->us t)
    # Fallback: the :double form is seconds + fraction, so the same 1e6 scale.
    (math/floor (* 1000000 (os/clock :monotonic)))))

(defn- ms [a b] (/ (- b a) 1000.0))

# --- clock self-test (opt-in): real 1.1 s rollover regression ---------------
# Run as:  ./build/bender bench/cross-runtime/wave-phased.janet selftest
#
# Every synthetic control feeds hand-made [sec ns] tuples, so it checks the
# arithmetic but not the live clock. This mode is the only check that measures
# a REAL elapsed interval longer than one second and therefore crosses a real
# monotonic second boundary, which is exactly the condition the old
# sec*1000 bug needed to show up.
#
# It is not a performance measurement: it sleeps ~1.1 s once and asserts the
# interval landed in a sane window. The window is deliberately wide so a loaded
# or suspended machine does not produce a false failure; what it actually
# rejects is a wrong SCALE (an interval off by ~1000x) or a non-monotonic clock
# (negative or collapsed delta). Cost: ~1.1 s, only when explicitly requested.
(defn- selftest []
  (def target-us 1100000)
  (def started (now-us))
  (ev/sleep 1.1)
  (def ended (now-us))
  (def delta (- ended started))
  (def cross-sec-a (math/floor (/ started 1000000)))
  (def cross-sec-b (math/floor (/ ended 1000000)))
  (printf "selftest elapsed_us=%d elapsed_ms=%.3f crossed_second=%v"
          delta (ms started ended) (> cross-sec-b cross-sec-a))
  (def problems @[])
  (when (< delta 0)
    (array/push problems (string "non-monotonic: delta=" delta)))
  # Wide sanity band: a clock scaled by 1000 gives ~1100 us, not ~1.1e6 us.
  (when (< delta 900000)
    (array/push problems (string "elapsed too small (wrong scale?): " delta)))
  (when (> delta 5000000)
    (array/push problems (string "elapsed implausibly large: " delta)))
  (unless (> cross-sec-b cross-sec-a)
    (array/push problems "did not cross a second boundary; rollover untested"))
  (if (empty? problems)
    (do
      (print "selftest PASSED")
      0)
    (do
      (each problem problems (eprintf "selftest FAILED: %s" problem))
      1)))

(def raw-n (get (dyn :args) 1))

(when (= "selftest" raw-n)
  (os/exit (selftest)))

# --- N: required, integer, >= 1, no arbitrary cap ---------------------------
# Validated after the clock self-test dispatch so `selftest` is not read as N.
(when (nil? raw-n) (die "missing N argument"))

(def n (scan-number raw-n))
(unless (and (number? n) (= n (math/floor n)))
  (die (string "N must be an integer, got " raw-n)))
(when (< n 1) (die (string "N must be >= 1, got " raw-n)))
# Janet numbers are doubles; N above 2^31 is not representable as an int32
# chain index, so reject rather than silently truncate.
(when (> n 2147483647) (die (string "N exceeds int32 range: " raw-n)))

# --- phase: spawn -----------------------------------------------------------
(def done (ev/thread-chan 1))

(def spawn_start (now-us))
(eprintf "phase=spawn-start spawn_start_us=%d" spawn_start)

# Body below is byte-for-byte the wave.janet chain: sink first, then one
# forwarding actor per hop, each receiving and incrementing exactly once.
(def sink (proc/spawn (fn [] (ev/give done (proc/recv))) 8))
(var head sink)
(var i 0)
(while (< i n)
  (def downstream head)
  (set head (proc/spawn (fn [] (proc/send downstream (+ 1 (proc/recv)))) 8))
  (set i (+ i 1)))

(def spawn_end (now-us))
(def spawn_ms (ms spawn_start spawn_end))
(eprintf "phase=spawn-end spawn_end_us=%d spawn_ms=%.3f" spawn_end spawn_ms)

# --- phase: wave ------------------------------------------------------------
# wave_start is sampled AFTER the spawn-end marker is printed, so wave_ms
# excludes the marker writes -- but it also excludes the spawn-end print's own
# cost, and spawn_ms by contrast encloses the spawn-start print. The gap is
# therefore reported explicitly rather than hidden; label_lag_us is that gap.
# It is microseconds next to a wave of milliseconds, but it is not zero.
(def wave_start (now-us))
(eprintf "phase=wave-start wave_start_us=%d label_lag_us=%d"
         wave_start (- wave_start spawn_end))
(proc/send head 0)
(def result (ev/take done))
(def wave_end (now-us))
(def wave_ms (ms wave_start wave_end))
(eprintf "phase=wave-end wave_end_us=%d wave_ms=%.3f" wave_end wave_ms)

# Correctness gate before any summary line, exactly as wave.janet asserts.
(assert (= result n) "wave result must equal process count")

# --- lane placement evidence (observable facts only) -----------------------
# JANET_LANES is the knob the runtime itself reads; proc/lanes reports the
# worker VMs actually up. Neither says where an individual actor ran.
(def requested-lanes (or (scan-number (os/getenv "JANET_LANES")) 0))
(def active-lanes (proc/lanes))
(def self-proc (proc/self))
(def self-pid (if (nil? self-proc) nil (proc/self-pid)))
# The lane slot of the pid tuple is structural (hardcoded 1 in proc.c), so it
# is reported under a name that cannot be mistaken for runtime placement.
(def pid-shape-lane-field
  (if (and (tuple? self-pid) (>= (length self-pid) 3))
    (get self-pid 1)
    nil))

(eprintf "lanes=%d requested_lanes=%d placement=unknown"
        active-lanes requested-lanes)
(eprintf "placement_note=%s"
        (string "runtime exposes no per-actor lane identity; "
                "proc/self-pid lane field is structural, not the executing lane"))
(eprintf "self_in_proc=%s pid_shape_lane_field=%s"
        (if (nil? self-proc) "false" "true")
        (if (nil? pid-shape-lane-field) "nil"
            (string pid-shape-lane-field)))

# --- clock sanity gate ------------------------------------------------------
# Cheap regression guard for the tuple-scale bug: with seconds scaled as ms
# instead of us, a value taken before and after a second boundary DECREASES by
# ~999000. Two samples microseconds apart cannot legitimately go backward, so
# this fails loudly rather than emitting negative phase durations. Cost is two
# clock reads plus a comparison, outside all timed intervals.
(def clock_a (now-us))
(def clock_b (now-us))
(when (< clock_b clock_a)
  (eprintf "wave-phased: clock regression %d -> %d (tuple scale bug)"
           clock_a clock_b)
  (os/exit 3))

# --- phase: shutdown --------------------------------------------------------
(def shutdown_start (now-us))
(eprintf "phase=shutdown-start shutdown_start_us=%d" shutdown_start)
(when (> active-lanes 0) (proc/lanes-stop))
(def shutdown_end (now-us))
(def shutdown_ms (ms shutdown_start shutdown_end))
(eprintf "phase=shutdown-end shutdown_end_us=%d shutdown_ms=%.3f"
         shutdown_end shutdown_ms)

# --- deterministic stdout: identical shape to wave.janet --------------------
(printf "procs=%d result=%d" n result)

# --- final timing summary: same keys as wave.janet, 3-decimal precision -----
(eprintf "procs=%d result=%d spawn_ms=%.3f wave_ms=%.3f total_ms=%.3f"
        n result spawn_ms wave_ms (ms spawn_start wave_end))
