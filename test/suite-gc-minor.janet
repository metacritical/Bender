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

# Phase 2b-iii generational torture (see docs/internals/aot-plan.md). Builds
# an OLD generation, then links young objects into it through every
# barrier-covered store path (table put/putindex, array push/indexed/fill/
# insert) plus upvalue and fiber paths, forcing minor collections between
# every step. A missed barrier edge shows as corrupted or missing values.
# Each file runs in its own process, so no settings leak between suites.

(defn churn [n]
  # Allocate roughly n*10 KB of young garbage to force many minor
  # collections (one fires per 256 KB of nursery allocation; aging an
  # object old takes 2+ survived minors).
  (var acc 0)
  (for i 0 n
    (def t @[])
    (for j 0 200 (array/push t (string "churn-" i "-" j)))
    (set acc (+ acc (length t))))
  acc)

# 1. Old tables holding young values (put path).
(let [oldtab @{}]
  (for i 0 50 (put oldtab (string "oldkey-" i) i))
  (churn 60) # age the table old (survives several minors)
  (for i 0 50 (put oldtab (string "youngkey-" i) (string "youngval-" i)))
  (churn 60) # minors must find young values via the barrier
  (var ok true)
  (for i 0 50
    (unless (= (string "youngval-" i) (oldtab (string "youngkey-" i)))
      (set ok false)))
  (assert ok "old table keeps young values"))

# 2. Old arrays holding young values (push/indexed paths).
(let [oldarr @[]]
  (for i 0 50 (array/push oldarr i))
  (churn 60)
  (for i 0 25 (array/push oldarr (string "pushed-" i)))
  (for i 0 10 (put oldarr i (string "indexed-" i)))
  (churn 60)
  (assert (= 75 (length oldarr)) "array length")
  (assert (= "indexed-5" (get oldarr 5)) "indexed write survived")
  (assert (= "pushed-24" (last oldarr)) "pushed value survived"))

# 2b. array/fill on an old array.
(let [fa @[1 2 3]]
  (churn 60)
  (array/fill fa (string "filled"))
  (churn 60)
  (assert (deep= @["filled" "filled" "filled"] fa) "fill survived"))

# 3. array/insert of young values into an old array.
(let [ia @[1 2 3]]
  (churn 60)
  (array/insert ia 1 (string "ins-a") (string "ins-b"))
  (churn 60)
  (assert (deep= @[1 "ins-a" "ins-b" 2 3] ia) "insert survived"))

# 4. Closures over old bindings, mutated via set (upvalue/env paths).
(let [holder @{}]
  (put holder :mk
    (fn [x]
      (var cell x)
      (fn [op v]
        (if (= op :get) cell (set cell v)))))
  (churn 60) # age the holder and its closures old
  (def getset ((holder :mk) (string "v0")))
  (churn 20)
  (getset :set (string "v1"))
  (churn 60) # minors must keep the young string via env/upvalue paths
  (assert (= "v1" (getset :get nil)) "upvalue round-trip"))

# 5. Fibers suspended with old frames (fiber slots are roots).
(let [ff (fiber/new
           (fn []
             (def acc @[])
             (for i 0 30 (array/push acc (string "f-" i)) (yield i))
             acc))]
  (for _ 0 10 (resume ff))
  (churn 60)
  (for _ 0 10 (resume ff))
  (churn 60)
  (for _ 0 10 (resume ff))
  (def tail (resume ff))
  (assert (= :dead (fiber/status ff)) "fiber done")
  (assert (= 30 (length tail)) "fiber survivors"))

# 6. Marshal round-trip of young graphs under minor pressure.
(let [mg @{:a (range 20) :s "payload"}]
  (churn 40)
  (assert (deep= mg (unmarshal (marshal mg))) "marshal round-trip"))

# 7. Weak tables don't crash minors (clearing waits for majors).
(let [wt (table/weak-values 10)]
  (put wt :k (string "weakval"))
  (churn 60)
  (gccollect) # major: weak clearing happens here, must be clean
  (assert true "weak tables survive minors"))

# 8. Combined pressure at low interval (minors + majors interleaved).
(gcsetinterval 2048)
(let [ct @{}]
  (for i 0 100 (put ct i (string "c-" i)))
  (churn 30)
  (var ok true)
  (for i 0 100
    (unless (= (string "c-" i) (get ct i)) (set ok false)))
  (assert ok "combined pressure"))
(gcsetinterval 4194304)

(end-suite)
