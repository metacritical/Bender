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

# M4a: pooled warm-VM executors (examples/pool.janet). Each file runs
# in its own process, so no settings leak between suites.

(import ../examples/pool :as pool)

# 1. Basic calls with arg round-trips across the pool.
(let [p (pool/spawn 2)]
  (assert (= 7 (pool/call p + [3 4])) "call add")
  (assert (= 49 (pool/call p (fn [x] (* x x)) [7])) "call closure")
  (assert (deep= @{:k 7} (pool/call p (fn [x] @{:k x}) [7])) "table arg")
  (pool/stop p))

# 2. Task errors re-raise in the caller; the pool survives.
(let [p (pool/spawn 2)]
  (assert-error-value "task error surfaces" "kaboom"
    (pool/call p (fn [_] (error "kaboom")) [nil]))
  (assert (= 8 (pool/call p + [3 5])) "pool survives errors")
  (pool/stop p))

# 3. Call timeout fires on slow tasks; the task itself still runs.
(let [p (pool/spawn 1)]
  (assert-error "call timeout" (pool/call p (fn [_] (ev/sleep 1) :slow) [nil] 0.05))
  (pool/stop p))

# 4. More tasks than workers: everything runs, all correct.
(let [p (pool/spawn 3)]
  (assert (deep= (map (fn [i] (* i i)) (range 20))
                 (pool/map p (fn [x] (* x x)) (map (fn [i] [i]) (range 20))))
          "20 tasks over 3 workers")
  (pool/stop p))

# 5. pool/map preserves order and takes per-call arg arrays.
(let [p (pool/spawn 2)]
  (assert (deep= @[3 7 11] (pool/map p + [[1 2] [3 4] [5 6]])) "map order")
  (pool/stop p))

# 6. Parallelism: 8 x 50ms sleeps on 4 workers finish well under the
# 400ms serial cost (generous bound for loaded machines).
(let [p (pool/spawn 4)
      t0 (os/clock)]
  (pool/map p (fn [_] (ev/sleep 0.05) :ok) (map (fn [_] [nil]) (range 8)))
  (def dt (- (os/clock) t0))
  (assert (< dt 0.3) (string "parallel speedup, took " dt))
  (pool/stop p))

# 7. submit/collect split: raw reply channels compose.
(let [p (pool/spawn 2)
      c1 (pool/submit p + [1 2])
      c2 (pool/submit p + [10 20])]
  (assert (= 3 (ev/take c1)) "submit channel 1")
  (assert (= 30 (ev/take c2)) "submit channel 2")
  (pool/stop p))

# 8. Lifecycle: stop joins every worker; calls after stop time out
# (queue has no takers — at-most-once, same as dead mailboxes).
(let [p (pool/spawn 2)]
  (assert (= 2 (pool/call p + [1 1])) "pre-stop call")
  (pool/stop p)
  (var any-alive false)
  (each w (p :workers) (when (thread/alive? w) (set any-alive true)))
  (assert (not any-alive) "all workers joined")
  (assert-error "call after stop" (pool/call p + [1 1] 0.05)))

# 9. Bad spawn args refused.
(assert-error "zero threads" (pool/spawn 0))
(assert-error "non-integer threads" (pool/spawn 2.5))

(end-suite)
