# Cross-lane vs same-VM round trip, isolated from procs entirely.
(def N (or (scan-number (get (dyn :args) 1)) 2000))
(proc/lanes-start 4)

# same-VM channel
(def local (ev/chan 1))
(def t0 (os/clock))
(var i 0)
(while (< i N) (ev/give local i) (ev/take local) (set i (+ i 1)))
(def t-local (os/clock))

# cross-lane: a proc on some lane echoes back through a thread-chan
(def back (ev/thread-chan 64))
(def fwd (ev/thread-chan 64))
(def echo (proc/spawn (fn [] (var r true) (while r (def m (ev/take fwd)) (if (nil? m) (set r false) (ev/give back m)))) 8))
(def t1 (os/clock))
(var j 0)
(while (< j N) (ev/give fwd j) (ev/take back) (set j (+ j 1)))
(def t-lane (os/clock))

(eprintf "n=%d" N)
(eprintf "  same-VM chan : %.2f us/op" (/ (* 1000000.0 (- t-local t0)) N))
(eprintf "  cross-lane   : %.2f us/op" (/ (* 1000000.0 (- t-lane t1)) N))
(proc/lanes-stop)
