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

# --- N: required, integer, >= 1, no arbitrary cap ---------------------------
(def raw-n (get (dyn :args) 1))
(when (nil? raw-n) (die "missing N argument"))

(def n (scan-number raw-n))
(unless (and (number? n) (= n (math/floor n)))
  (die (string "N must be an integer, got " raw-n)))
(when (< n 1) (die (string "N must be >= 1, got " raw-n)))
# Janet numbers are doubles; N above 2^31 is not representable as an int32
# chain index, so reject rather than silently truncate.
(when (> n 2147483647) (die (string "N exceeds int32 range: " raw-n)))

# --- monotonic clock, microsecond resolution --------------------------------
# os/clock :monotonic returns a double of seconds; :tuple returns
# [seconds nanoseconds]. The tuple form avoids losing microseconds when the
# machine has been up for a long time, so prefer it and fall back to the
# double if the tuple shape is ever unavailable.
(defn- now-us []
  (def t (os/clock :monotonic :tuple))
  (if (and (tuple? t) (= 2 (length t)))
    (+ (* 1000 (get t 0)) (math/floor (/ (get t 1) 1000)))
    (math/floor (* 1000000 (os/clock :monotonic)))))

(defn- ms [a b] (/ (- b a) 1000.0))

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
(eprintf "phase=wave-start wave_start_us=%d" spawn_end)
(proc/send head 0)
(def result (ev/take done))
(def wave_end (now-us))
(def wave_ms (ms spawn_end wave_end))
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
        n result spawn_ms wave_ms (+ spawn_ms wave_ms))
