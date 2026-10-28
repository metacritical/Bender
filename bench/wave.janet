# Trampoline wave: build a chain of N procs, send 0 into the head, each proc
# increments and forwards; the sink reports the final value.
#
#   JANET_LANES=11 ./build/janet /tmp/p4/wave.janet <N>
#
# `done` is top level: a channel created inside a function is a lexical capture
# and would pin every proc to the current VM, serialising the chain build.

(def N (or (scan-number (get (dyn :args) 1)) 100000))
(def done (ev/thread-chan 0))

(def t0 (os/clock))

(def sink (proc/spawn (fn [] (ev/give done (proc/recv))) 8))
(var next-proc sink)
(var i 0)
(while (< i N)
  (def down next-proc)
  (set next-proc (proc/spawn (fn [] (proc/send down (+ 1 (proc/recv)))) 8))
  (set i (+ i 1)))

(def t1 (os/clock))
(proc/send next-proc 0)
(def result (ev/take done))
(def t2 (os/clock))

(eprintf "procs=%d result=%d spawn=%.0fms wave=%.0fms total=%.0fms"
         N result (* 1000.0 (- t1 t0)) (* 1000.0 (- t2 t1)) (* 1000.0 (- t2 t0)))
(when (> (proc/lanes) 0) (proc/lanes-stop))
