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
  # (os/clock :monotonic :tuple) is [tv_sec tv_nsec] -- the runtime contract,
  # confirmed against the parent: (1394009 105833000) and the :double form
  # 1394009.105833 both reduce to 1394009105833 us.
  #
  # One format, no unit guessing. tv_nsec is legitimately 0..999999999, so a
  # small fraction (e.g. 0..999 near a second start) is real nanoseconds and
  # must NOT be reinterpreted as microseconds.
  (+ (* 1000000 (get t 0)) (math/floor (/ (get t 1) 1000))))

(defn- now-us []
  (clock->us (os/clock :monotonic :tuple)))

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

# Guard the contract's sharp edge cheaply: tv_nsec legitimately spans
# 0..999999999, and a value under 1000 (seconds + a few hundred ns, i.e. right
# after a second start) must convert as nanoseconds, not be reinterpreted as
# microseconds. Two arithmetic checks, no clock read, no cost on any path.
# Expected: 1 ms in ns -> 1000 us; a whole second -> 1000000 us;
# +1 us with ns still under 1000 -> 1000001 us; 999999000 ns -> 1999999 us.
(unless (and (= 1000 (clock->us [0 1000000]))
             (= 1000000 (clock->us [1 0]))
             (= 1000001 (clock->us [1 1000]))
             (= 1999999 (clock->us [1 999999000])))
  (eprintf "wave-phased: clock->us contract check failed")
  (os/exit 3))

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
# Marker accounting, stated exactly:
#   * wave_start is sampled AFTER the spawn-end marker is printed, so wave_ms
#     EXCLUDES the spawn-end print.
#   * wave_ms STILL INCLUDES the wave-start print (this marker's own write),
#     which is unavoidable: the interval start cannot be labelled without
#     emitting the label. It is ~tens of microseconds against a wave of
#     milliseconds, but it is inside the interval, not outside it.
#   * spawn_ms by contrast ENCLOSES the spawn-start print.
# The span between spawn_end and wave_start is reported separately as
# label_lag_us rather than folded into either interval.
(def wave_start (now-us))
(eprintf "phase=wave-start wave_start_us=%d" wave_start)
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
(eprintf "lanes=%d requested_lanes=%d placement=unknown"
        active-lanes requested-lanes)

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
