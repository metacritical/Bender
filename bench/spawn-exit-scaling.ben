# Erlang-parity spawn/exit scaling benchmark.
#
# The goal this measures: Erlang spawns ~1M processes and exits them
# with FLAT per-process cost. If per-process cost grows with the
# number of already-live processes, total cost is quadratic and a
# million processes is unreachable regardless of constant factors.
#
# Reports per-bucket cost as the live set grows, so a rising
# per-process number is immediately visible as the O(N) term it is.
(def total (if-let [a ((dyn :args) 1)] (scan-number a) 40000))
(def bucket (if-let [a ((dyn :args) 2)] (scan-number a) 5000))

(def procs @[])
(print "=== SPAWN phase (live set growing) ===")
(var i 0)
(while (< i total)
  (def t0 (os/clock))
  (for j 0 bucket
    (array/push procs (proc/spawn (fn [] (proc/recv)))))
  (def t1 (os/clock))
  (print "  live=" (+ i bucket)
         " per-spawn-us=" (string/format "%.2f" (* 1000000 (/ (- t1 t0) bucket))))
  (+= i bucket))

(print "=== EXIT phase (live set shrinking) ===")
(var k 0)
(while (< k total)
  (def t0 (os/clock))
  (for j 0 bucket
    (def p (procs (+ k j)))
    (when (proc/alive? p) (proc/kill p :done)))
  (def t1 (os/clock))
  (print "  killed=" (+ k bucket)
         " per-exit-us=" (string/format "%.2f" (* 1000000 (/ (- t1 t0) bucket))))
  (+= k bucket))
(print "done")
(os/exit 0)
