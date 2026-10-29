# N processes, each doing real work, all results summed.
#
#   JANET_LANES=11 ./build/janet examples/five-million.janet [N]
#
# DESIGN. Every unit is a real process. The problem is getting N results back:
# posting N events over the self-pipe aborts around 1M ("failed to write event
# to self-pipe"; measured 800000 ok, 1000000 abort).
#
# So results are collected in BATCHES. Each batch has one SINK proc, spawned
# FIRST, which receives its members' results from within that batch's scope and
# reports a single number. Only nbatches numbers cross to the main VM, so the
# message count is bounded by the batch count rather than by N.
#
# `done` is top level on purpose: a channel created inside a function is a
# lexical capture, which pins procs to the current VM and serialises the run
# (measured 30ms vs 221ms).

(def N (or (scan-number (get (dyn :args) 1)) 5000000))
(def BATCH 500)
(def done (ev/thread-chan 0))

(defn work [x]
  (var s 0)
  (var i 0)
  (while (< i 100)
    (set s (+ s (* i x)))
    (set i (+ i 1)))
  s)

(def t0 (os/clock))
(def nbatches (max 1 (div (+ N BATCH -1) BATCH)))

# Each batch: one sink proc collecting BATCH results, reporting one number.
(var b 0)
(while (< b nbatches)
  (def lo (* b BATCH))
  (def hi (min N (+ lo BATCH)))
  (def cnt (- hi lo))
  ;; The sink is spawned first so its mailbox exists for the members.
  (def sink (proc/spawn (fn []
                          (var sum 0)
                          (var k 0)
                          (while (< k (proc/recv))
                            (set sum (+ sum (proc/recv)))
                            (set k (+ k 1)))
                          (ev/give done sum))
                        8))
  (proc/send sink cnt)
  ;; Now the batch's processes, each reporting into the sink's mailbox.
  (var i lo)
  (while (< i hi)
    (def x i)
    (proc/spawn (fn [] (proc/send sink (work x))) 8)
    (set i (+ i 1)))
  (set b (+ b 1)))

(def t-spawned (os/clock))

(var got 0)
(var checksum 0)
(while (< got nbatches)
  (set checksum (+ checksum (ev/take done)))
  (set got (+ got 1)))

(def t1 (os/clock))
(eprintf "procs=%d  batches=%d  spawn=%.1fs  total=%.1fs  checksum=%d  lanes=%d"
         N nbatches (- t-spawned t0) (- t1 t0) checksum (proc/lanes))
(when (> (proc/lanes) 0) (proc/lanes-stop))
