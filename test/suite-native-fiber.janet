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

# Phase 1 native-stack fibers (see docs/internals/native-fibers.md). Each
# fiber runs its VM entries on its own C stack and suspends via a context
# switch; semantics must be identical to the old inline model. These tests
# stress the switch machinery: nesting, errors across stacks, GC of values
# in flight, marshal round-trips, and many live fibers at once.

# 1. Basic ping-pong across context switches
(defn gen [n]
  (for i 0 n (yield i))
  :done)
(let [f (fiber/new (fn [] (gen 5)))]
  (assert (= 0 (resume f)) "yield 0")
  (assert (= 1 (resume f)) "yield 1")
  (assert (= 2 (resume f)) "yield 2")
  (assert (= 3 (resume f)) "yield 3")
  (assert (= 4 (resume f)) "yield 4")
  (assert (= :done (resume f)) "return")
  (assert (= :dead (fiber/status f)) "dead"))

# 2. Nested resume: a fiber resuming another fiber (child path)
(defn inner [] (yield 10) 20)
(def g (fiber/new inner))
(defn outer []
  (def x (resume g))
  (assert (= x 10) "nested first")
  (yield x)
  (def y (resume g))
  (assert (= y 20) "nested second")
  (+ x y))
(def f (fiber/new outer))
(assert (= 10 (resume f)) "outer first")
(assert (= :pending (fiber/status g)) "child pending")
(assert (= 30 (resume f)) "outer second")

# 3. Error inside a fiber: status, payload, no re-resume
(def ef (fiber/new (fn [] (error "boom"))))
(def [ok _e] (protect (resume ef)))
(assert (not ok) "error propagates")
(assert (= :error (fiber/status ef)) "error status")
(def [ok2 _] (protect (resume ef)))
(assert (not ok2) "dead fiber rejects resume")

# 4. Error propagates through a nested resume (regression: this path
# used to crash the runtime during interpreter shutdown)
(defn child-err [] (error "x"))
(let [g2 (fiber/new child-err)
      f2 (fiber/new (fn [] (resume g2)))]
  (def [okn _en] (protect (resume f2)))
  (assert (not okn) "nested error propagates")
  (assert (= :error (fiber/status f2)) "parent error status")
  (assert (= :error (fiber/status g2)) "child error status"))

# 5. Deep recursion inside one fiber (native stack depth)
(defn countdown [n] (if (<= n 0) 0 (+ 1 (countdown (- n 1)))))
(let [rf (fiber/new (fn [] (countdown 5000)))]
  (assert (= 5000 (resume rf)) "deep recursion"))

# 6. Many live fibers at once (stack accounting, guard pages)
(let [fibers (map (fn [i] (fiber/new (fn [] (yield i) (+ i 1)))) (range 300))]
  (each fib fibers (resume fib))
  (def total (sum (map resume fibers)))
  (assert (= total (sum (map |(+ $ 1) (range 300)))) "300 fibers"))

# 7. GC stress across switches: values in flight must survive collections.
# Each resume passes a fresh table/string through in/out slots while the
# debug build forces collections via string churn.
(let [gf (fiber/new
           (fn []
             (var acc @[])
             (for i 0 200
               (def payload @{:i i :s (string "payload-" i)})
               (yield payload)
               (array/push acc payload))
             acc))]
  (var final nil)
  (for _ 0 200 (set final (resume gf)))
  (def acc (resume gf))
  (assert (= 200 (length acc)) "all payloads")
  (assert (deep= (last acc) final) "payload intact")
  (assert (= ((last acc) :i) 199) "payload content"))

# 8. Marshal round-trip of a suspended fiber, then resume it
# (exercises native-stack arming for unmarshalled fibers).
(let [mf (fiber/new (fn [] (yield 1) (+ 1 1)))]
  (assert (= 1 (resume mf)) "suspend")
  (def mf2 (unmarshal (marshal mf)))
  (assert (= 2 (resume mf2)) "resumed unmarshalled")
  (assert (= :dead (fiber/status mf2)) "dead after"))

# 9. Marshal round-trip of a fiber with a live child
(let [inner2 (fiber/new (fn [] (yield :v) :r))]
  (def outer2
    (fiber/new
      (fn []
        (def v (resume inner2))
        (yield v)
        (resume inner2))))
  (assert (= :v (resume outer2)) "outer suspend")
  (def outer3 (unmarshal (marshal outer2)))
  # the child fiber object travels with the marshal image; resume the copy
  (assert (= :r (resume outer3)) "outer resumed"))

# 10. ev smoke: scheduled fibers still interleave on their own stacks
(when (dyn :ev)
  (def ch (ev/chan 0))
  (ev/go (fn [] (ev/sleep 0.01) (ev/give ch :ping)))
  (assert (= :ping (ev/take ch)) "ev rendezvous"))

# 11. Phase 1.1 swap-suspension: a yield inside a Janet callback invoked
# from C (janet_call) freezes the C chain instead of coercing to an error.
# string/replace-all drives the callback from C with C locals live.
(let [rf (fiber/new
           (fn []
             (string/replace-all "a"
               (fn [m] (yield (string "got:" m)) "X")
               "a-a-b")))]
  (assert (= "got:a" (resume rf)) "yield 1 through C")
  (assert (= "got:a" (resume rf)) "yield 2 through C")
  (assert (= "X-X-b" (resume rf)) "C call completes after yields")
  (assert (= :dead (fiber/status rf)) "dead after"))

# 12. Resume values arrive as the yield result through C frames.
(let [vf (fiber/new
           (fn []
             (string/replace-all "a"
               (fn [m] (string "saw:" (yield m)))
               "a")))]
  (assert (= "a" (resume vf)) "yield value")
  (assert (= "saw:R" (resume vf "R")) "resume value through C"))

# 13. Errors inside a C-driven callback propagate uncoerced.
(let [efib (fiber/new
             (fn [] (string/replace "a" (fn [m] (error (string "inner-boom:" m))) "a")))]
  (def [ok13 e13] (protect (resume efib)))
  (assert (not ok13) "error propagates")
  (assert (string/find "inner-boom" (string e13)) "message intact, not coerced")
  (assert (= :error (fiber/status efib)) "error status"))

# 14. Cancel a fiber suspended inside C frames unwinds with the payload.
(let [cf (fiber/new
           (fn [] (string/replace-all "a" (fn [m] (yield m) m) "a-a")))]
  (assert (= "a" (resume cf)) "suspend inside C")
  (def [ok14 e14] (protect (cancel cf "stop")))
  (assert (not ok14) "cancel raises")
  (assert (= "stop" e14) "cancel payload")
  (assert (= :error (fiber/status cf)) "error status after cancel"))

# 15. Same suspension path through a second janet_call site (peg/replace).
(let [pf (fiber/new
           (fn [] (peg/replace-all ~(sequence "a") (fn [m] (yield m) "X") "a-a")))]
  (assert (= "a" (resume pf)) "peg yield 1 through C")
  (assert (= "a" (resume pf)) "peg yield 2 through C")
  (assert (= "X-X" (string (resume pf))) "peg replace completes"))

# 16. GC across a C-frozen suspension: values in flight survive collections.
(let [gf (fiber/new
           (fn []
             (string/replace-all "k"
               (fn [m]
                 (def payload @{:m m :s (string "payload-" m)})
                 (yield payload)
                 (string (payload :s) "!" (payload :m)))
               "k-k")))]
  (gccollect)
  (assert (deep= @{:m "k" :s "payload-k"} (resume gf)) "payload 1")
  (gccollect)
  (assert (deep= @{:m "k" :s "payload-k"} (resume gf)) "payload 2")
  (gccollect)
  (assert (= "payload-k!k-payload-k!k" (resume gf)) "completed through C"))

(end-suite)
