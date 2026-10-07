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

# M4b-2: quantum driver (ev/timeslice). All order-based pins — no
# timing flakes. A busy-loop hog of 10M iterations (~0.35s) vs a
# 50ms sleeper separates preempted from starved deterministically.

(defn- hog-sum [n]
  (var s 0)
  (var i 0)
  (while (< i n) (+= s i) (++ i))
  s)

# 1. Baseline, quanta off: the hog starves the sleeper. Forced off
# (not assumed) so the suite also passes nested under quanta.
(let [order @[]]
  (ev/timeslice false)
  (ev/go (fn [] (hog-sum 10000000) (array/push order :hog)))
  (ev/go (fn [] (ev/sleep 0.05) (array/push order :short)))
  (ev/sleep 2)
  (assert (deep= @[:hog :short] order) "off: hog starves sleeper"))

# 2. Quanta on: the sleeper completes first (preemption proved).
(let [order @[]]
  (ev/timeslice true)
  (ev/go (fn [] (hog-sum 10000000) (array/push order :hog)))
  (ev/go (fn [] (ev/sleep 0.05) (array/push order :short)))
  (ev/sleep 3)
  (assert (deep= @[:short :hog] order) "on: sleeper beats hog")
  (ev/timeslice false))

# 3. Results bit-exact under quanta.
(let [done (ev/chan 1)]
  (ev/timeslice true)
  (ev/go (fn [] (ev/give done (hog-sum 1000000))))
  (assert (= 499999500000 (ev/take done)) "preempted sum exact")
  (ev/timeslice false))

# 4. Preemption inside try completes normally, nothing spurious caught.
(let [done (ev/chan 1)]
  (ev/timeslice true)
  (ev/go (fn [] (ev/give done (try (hog-sum 1000000) ([_e] :caught)))))
  (assert (= 499999500000 (ev/take done)) "try-transparent")
  (ev/timeslice false))

# 5. Call-site preemption: nested calls share quanta fairly and stay exact.
(defn- inner [n]
  (var s 0)
  (var i 0)
  (while (< i n) (+= s i) (++ i))
  s)
(defn- outer [n m]
  (var t 0)
  (var j 0)
  (while (< j m) (+= t (inner n)) (++ j))
  t)
(let [done (ev/chan 1)
      order @[]]
  (ev/timeslice true)
  (ev/go (fn [] (ev/give done (outer 20000 600)) (array/push order :hog)))
  (ev/go (fn [] (ev/sleep 0.05) (array/push order :short)))
  (ev/sleep 4)
  # outer = 600 * sum(0..19999) = 600 * 199990000 = 119994000000
  (assert (= 119994000000 (ev/take done)) "nested calls exact")
  (assert (= :short (get order 0)) "short beats nested hog")
  (ev/timeslice false))

# 6. Toggling off mid-run restores run-to-completion.
(let [order @[]]
  (ev/timeslice true)
  (ev/timeslice false)
  (assert (not (ev/timeslice)) "toggle reports off")
  (ev/go (fn [] (hog-sum 10000000) (array/push order :hog)))
  (ev/go (fn [] (ev/sleep 0.05) (array/push order :short)))
  (ev/sleep 2)
  (assert (deep= @[:hog :short] order) "off again: starvation returns")
  (assert (ev/timeslice true) "toggle reports on")
  (ev/timeslice false))

# 7. C-heavy tasks stay correct under quanta.
(let [done (ev/chan 1)]
  (ev/timeslice true)
  (ev/go (fn []
           (def b @"")
           (each _i (range 20000) (buffer/push-string b "ab"))
           (ev/give done (length b))))
  (assert (= 40000 (ev/take done)) "buffer task exact under quanta")
  (ev/timeslice false))

# 8. Quanta are per-VM: worker threads opt in independently.
(let [h (thread/new (fn [] (ev/timeslice)))]
  (assert (not (thread/join h)) "worker quantum defaults off"))

# 9. Macroexpansion under quanta: compile-time code runs to
# completion (transparent resume in macex — the bug that silently
# dropped suite-ev under quanta).
(let [done (ev/chan 1)]
  (ev/timeslice true)
  (ev/go (fn [] (ev/give done (eval '(+ 1 2)))))
  (assert (= 3 (ev/take done)) "eval under quanta")
  (ev/timeslice false))

(end-suite)
