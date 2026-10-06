# Copyright (c) 2026 Calvin Rose
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to
# deal in the Software without restriction, including without limitation the
# rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
# sell copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in
# all copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
# FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
# IN THE SOFTWARE.

(import ./helper :prefix "" :exit true)
(start-suite)

# Phase 3 slice: value-returning thread/new + thread/join
# (see docs/internals/aot-plan.md). Threads run isolated VMs;
# arguments go in and results come back via marshal.
# Each file runs in its own process, so no settings leak between suites.

# 1. Basic value round-trips (0-arg and 1-arg functions).
(assert (= 42 (thread/join (thread/new (fn [] 42)))) "int result")
(assert (= 43 (thread/join (thread/new (fn [x] (+ x 1)) 42))) "arg result")
(assert (= "hi bob" (thread/join (thread/new (fn [x] (string "hi " x)) "bob"))) "string result")
(assert (= :done (thread/join (thread/new (fn [] :done)))) "keyword result")
(assert (= true (thread/join (thread/new (fn [] true)))) "bool result")
(assert (= nil (thread/join (thread/new (fn [] nil)))) "nil result")
(assert (deep= @[1 "two" :three] (thread/join (thread/new (fn [] @[1 "two" :three])))) "array result")
(assert (deep= @{:v 7 :sq 49} (thread/join (thread/new (fn [x] @{:v x :sq (* x x)}) 7))) "table result")

# 2. Threads run concurrently and all results come back in join order.
(let [hs (map (fn [i] (thread/new (fn [x] (* x 2)) i)) (range 8))]
  (assert (deep= @[0 2 4 6 8 10 12 14] (map thread/join hs)) "parallel batch"))

# 3. Event loop works inside the worker thread.
(assert (= :slept (thread/join (thread/new (fn [] (ev/sleep 0.05) :slept)))) "ev in thread")

# 4. Thread errors re-raise in the joiner with the original value.
(assert-error-value "join re-raises" "kablam"
  (thread/join (thread/new (fn [] (error "kablam")))))

# 5. Single-join discipline.
(let [h (thread/new (fn [] 1))]
  (thread/join h)
  (assert-error "double join" (thread/join h)))

# 6. Argument validation.
(assert-error "non-function refused" (thread/new 42))

# 6b. Fiber arguments round-trip through marshal into the worker.
(let [f (fiber/new (fn [x] (string "hi " x)))]
  (assert (= "hi bob" (thread/join (thread/new f "bob"))) "fiber arg"))

# 7. thread/alive? transitions: a fresh handle is always alive (completion
# can only be processed once this fiber suspends), and a joined handle is not.
(let [h (thread/new (fn [] (ev/sleep 0.05) :slow))]
  (assert (thread/alive? h) "fresh handle alive")
  (assert (= :slow (thread/join h)) "slow join")
  (assert (not (thread/alive? h)) "joined handle not alive"))

# 8. Joining from inside an ev task works and keeps the loop responsive.
(let [ch (ev/chan 0)]
  (ev/go (fn []
    (def h (thread/new (fn [x] (+ x 100)) 23))
    (ev/give ch (thread/join h))))
  (assert (= 123 (ev/take ch)) "join in ev task"))

# 9. Join with a generous timeout succeeds without firing.
(assert (= 7 (thread/join (thread/new (fn [] 7)) 5)) "timeout success")

# 10. Join timeout fires inside ev tasks, and a later join still collects.
(let [ch (ev/chan 0)
      h (thread/new (fn [] (ev/sleep 0.2) :slow))]
  (ev/go (fn []
    (def err (try (thread/join h 0.05) ([e] e)))
    (ev/give ch err)))
  (assert (= "timeout" (ev/take ch)) "timeout fires")
  (assert (= :slow (thread/join h)) "rejoin after timeout"))

# 11. Top-level join with timeout fires too (the main fiber is a task
# fiber once it has suspended through channel ops, so flag it first).
(let [ch (ev/chan 1)] (ev/give ch :x) (ev/take ch))
(let [h (thread/new (fn [] (ev/sleep 0.3) :slow))]
  (assert-error-value "top-level timeout" "timeout" (thread/join h 0.05))
  (assert (= :slow (thread/join h)) "top-level rejoin"))

# 12. Fibers nested inside args and results round-trip through marshal.
(let [f (fiber/new (fn [x] (* x 3)))]
  (assert (= 42 (thread/join (thread/new (fn [t] (resume (t :fib) 14)) @{:fib f}))) "fiber nested in arg"))
(let [h (thread/new (fn [_] (fiber/new (fn [x] (string "w=" x)))) 7)]
  (assert (= "w=7" (resume (thread/join h) 7)) "fiber nested in result"))

(end-suite)
