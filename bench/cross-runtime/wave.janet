# bench/cross-runtime/wave.janet
#
# Trampoline wave: build a chain of N processes. Process i receives a value,
# adds 1, and forwards it to process i+1. The last process reports the final
# value, which MUST equal N.
#
# This is deliberately a DEPENDENCY CHAIN, not parallel work: every hop waits
# for the previous one. It measures spawn cost, per-hop message latency and
# scheduling overhead -- never parallel speedup. Lanes cannot help it and are
# not used.
#
# IMPORTANT: run this with lanes ON for realistic numbers. On a single VM the
# wave is QUADRATIC in chain length (measured 1000->95ms, 2000->377ms,
# 4000->1526ms: 2x length, 4x time), because every proc lives in one shared
# arena and each completion minor walks it. Spread across lanes each VM has its
# own arena and the cost becomes linear (4000: 1506ms -> 27ms, 56x faster).
#
# Run: JANET_LANES=11 ./build/bender bench/cross-runtime/wave.janet <n>
# Emits one machine-readable line, mirrored by wave.erl and wave.go.

(def n (scan-number (get (dyn :args) 1)))
(def done (ev/thread-chan 1))

# --- spawn: chain proc i -> proc i+1, sink at the end -----------------------
(def t0 (os/clock))
(def sink (proc/spawn (fn [] (ev/give done (proc/recv))) 8))
(var head sink)
(var i 0)
(while (< i n)
  (def downstream head)
  (set head (proc/spawn (fn [] (proc/send downstream (+ 1 (proc/recv)))) 8))
  (set i (+ i 1)))
(def t1 (os/clock))

# --- wave: push one value through the whole chain ---------------------------
(proc/send head 0)
(def result (ev/take done))
(def t2 (os/clock))

(printf "procs=%d result=%d spawn_ms=%.0f wave_ms=%.0f total_ms=%.0f"
        n result
        (* 1000.0 (- t1 t0))
        (* 1000.0 (- t2 t1))
        (* 1000.0 (- t2 t0)))

(when (> (proc/lanes) 0) (proc/lanes-stop))
