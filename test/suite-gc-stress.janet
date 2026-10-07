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

# Phase 2a GC root discipline (see docs/internals/native-fibers.md). Runs
# allocation churn across every value kind at a tiny collection interval so
# that any value reachable only through an unmarked path (VM roots, fiber
# buffers incl. C-frozen suspensions, marshal images, closures) gets swept
# while still live and fails loudly. Each suite file runs in its own
# process, so the lowered interval cannot leak into other suites.

(def old-interval (gcinterval))
(gcsetinterval 512)

# 1. String churn with live references held across collections.
(let [alive @[]]
  (for i 0 300
    (array/push alive (string "live-string-" i)))
  (gccollect)
  (assert (= 300 (length alive)) "kept strings")
  (assert (= "live-string-299" (last alive)) "string content"))

# 2. Table/struct/tuple/array/buffer churn.
(let [tbl @{}]
  (for i 0 200
    (put tbl (string "k" i) @{:i i :t (tuple i (+ i 1)) :s (string "v" i)}))
  (gccollect)
  (assert (= 200 (length tbl)) "table size")
  (assert (deep= @{:i 199 :t (tuple 199 200) :s "v199"} (tbl "k199")) "table content")
  (def arr (map (fn [i] @[i (string "e" i)]) (range 100)))
  (gccollect)
  (assert (= 100 (length arr)) "array size")
  (assert (deep= @[99 "e99"] (last arr)) "array content"))

# 3. Fiber churn: many suspended fibers across collections.
(let [fibers (map (fn [i] (fiber/new (fn [] (yield i) (+ i 1)))) (range 200))]
  (each fib fibers (resume fib))
  (gccollect)
  (def total (sum (map resume fibers)))
  (assert (= total (sum (map |(+ $ 1) (range 200)))) "fibers survive churn"))

# 4. C-frozen suspensions under churn: yields through string/replace-all
# with collections between every resume (swap-suspension register/field
# coverage: in_value, out_payload, resume_fiber link, parked registers).
(let [cf (fiber/new
           (fn []
             (string/replace-all "a"
               (fn [m]
                 (def payload @{:m m :s (string "churn-" m)})
                 (yield payload)
                 (string (payload :s)))
               "a-a-a")))]
  (assert (deep= @{:m "a" :s "churn-a"} (resume cf)) "churn yield 1")
  (gccollect)
  (assert (deep= @{:m "a" :s "churn-a"} (resume cf)) "churn yield 2")
  (gccollect)
  (assert (deep= @{:m "a" :s "churn-a"} (resume cf)) "churn yield 3")
  (gccollect)
  (assert (= "churn-a-churn-a-churn-a" (string (resume cf))) "churn completes"))

# 5. Closures with captured environments across collections.
(let [mk (fn [x] (fn [] (+ x 1)))]
  (def closures (map mk (range 100)))
  (gccollect)
  (assert (= 100 ((last closures))) "closure envs")
  (def total (sum (map (fn [c] (c)) closures)))
  (assert (= total (sum (map |(+ $ 1) (range 100)))) "all closures"))

# 6. Marshal round-trips of fibers and values under churn.
(let [mf (fiber/new (fn [] (yield 1) (+ 1 1)))]
  (assert (= 1 (resume mf)) "suspend")
  (gccollect)
  (def mf2 (unmarshal (marshal mf)))
  (gccollect)
  (assert (= 2 (resume mf2)) "resumed unmarshalled under churn")
  (def big @{:a (range 50) :s "data"})
  (assert (deep= big (unmarshal (marshal big))) "value round-trip"))

# 7. Peg/string callback churn (janet_call sites) with yields.
(let [pf (fiber/new
           (fn [] (peg/replace-all "a" (fn [m] (yield m) "X") "a-a")))]
  (assert (= "a" (resume pf)) "peg churn yield 1")
  (gccollect)
  (assert (= "a" (resume pf)) "peg churn yield 2")
  (gccollect)
  (assert (= "X-X" (string (resume pf))) "peg churn completes"))

# 7b. Large replacements forcing buffer growth across suspensions.
(let [big (string/repeat "Z" 5000)
      bf (fiber/new
           (fn [] (peg/replace-all "a" (fn [m] (yield m) big) "a-a-a-a")))]
  (resume bf) (resume bf) (resume bf) (resume bf)
  (gccollect)
  (def grown (string (resume bf)))
  (assert (= :dead (fiber/status bf)) "big replace dead")
  (assert (deep= grown (string big "-" big "-" big "-" big)) "big replace content"))

# 8. ev smoke under churn.
(when (dyn :ev)
  (def ch (ev/chan 0))
  (ev/go (fn [] (ev/sleep 0.01) (ev/give ch :ping)))
  (gccollect)
  (assert (= :ping (ev/take ch)) "ev rendezvous under churn"))

(gcsetinterval old-interval)
(end-suite)
