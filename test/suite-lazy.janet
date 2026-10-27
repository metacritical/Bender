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

###
### Laziness
###
### The whole point of a lazy sequence is that constructing it does nothing.
### These assertions fail loudly if any combinator becomes eager.

(var evals 0)
(def mapped (lazy-map (fn [x] (++ evals) (* x x)) (lazy-iterate inc 0)))
(assert (= evals 0) "lazy-map over an infinite source evaluates nothing")

(def five (lazy-to-array (lazy-take 5 mapped)))
(assert (deep= @[0 1 4 9 16] five) "lazy-take 5 of an infinite map")
# Exactly n: the remaining count is tested BEFORE the cell is realized, so
# taking 5 never computes a 6th element.
(assert (= evals 5) "lazy-take realizes exactly n elements")

###
### Memoization
###

# Re-traversal must not recompute; a second pass stays at 5 evaluations.
(assert (deep= @[0 1 4 9 16] (lazy-to-array (lazy-take 5 mapped)))
        "re-traversal returns the same values")
(assert (= evals 5) "re-traversal computes nothing again")

# An impure mapper runs once per element no matter how often the sequence is
# walked.
(var calls 0)
(def counted (lazy-map (fn [x] (++ calls) x) (lazy-range 3)))
(lazy-to-array counted)
(lazy-to-array counted)
(assert (= calls 3) "an impure mapper runs once per element")

###
### Generators
###

(assert (deep= @[0 1 2 3 4] (lazy-to-array (lazy-range 5))) "lazy-range with one argument")
(assert (deep= @[2 3 4] (lazy-to-array (lazy-range 2 5))) "lazy-range with two arguments")
(assert (deep= @[0 2 4] (lazy-to-array (lazy-range 0 6 2))) "lazy-range with a step")
(assert (deep= @[] (lazy-to-array (lazy-range 0))) "lazy-range of nothing")
(assert (deep= @[:x :x :x] (lazy-to-array (lazy-take 3 (lazy-repeat :x))))
        "lazy-repeat is infinite")
(assert (deep= @[:y :y] (lazy-to-array (lazy-repeat :y 2))) "lazy-repeat with a count")
(assert (deep= @[0 1 2] (lazy-to-array (lazy-take 3 (lazy-iterate inc 0))))
        "lazy-iterate")
(assert (deep= @[1 2 3] (lazy-to-array (lazy-from-array [1 2 3])))
        "lazy-from-array")

###
### Combinators
###

(assert (deep= @[0 2 4 6] (lazy-to-array (lazy-filter even? (lazy-range 8))))
        "lazy-filter")
# Filtering an infinite source terminates while matches keep occurring.
(assert (deep= @[0 2 4 6] (lazy-to-array (lazy-take 4 (lazy-filter even? (lazy-iterate inc 0)))))
        "lazy-filter over an infinite source")
(assert (deep= @[3 4 5] (lazy-to-array (lazy-drop 3 (lazy-range 6)))) "lazy-drop")
(assert (deep= @[3 4 5] (lazy-to-array (lazy-take 3 (lazy-drop 3 (lazy-range 9)))))
        "lazy-take after lazy-drop")
(assert (deep= @[0 1 2 3 4] (lazy-to-array (lazy-take-while (fn [x] (< x 5)) (lazy-range 10))))
        "lazy-take-while")
(assert (deep= @[5 6 7 8 9] (lazy-to-array (lazy-drop-while (fn [x] (< x 5)) (lazy-range 10))))
        "lazy-drop-while")
(assert (deep= @[0 1 2 0 1] (lazy-to-array (lazy-cat (lazy-range 3) (lazy-range 2))))
        "lazy-cat")
(assert (deep= @[0 0 1 1 2 2]
               (lazy-to-array (lazy-mapcat (fn [x] (lazy-from-array [x x])) (lazy-range 3))))
        "lazy-mapcat")
(assert (deep= @[0 1] (lazy-to-array (lazy-cat (lazy-range 2)))) "lazy-cat of one")

###
### Head access and predicates
###

(assert (= 0 (lazy-first (lazy-range 3))) "lazy-first")
(assert (deep= @[1 2] (lazy-to-array (lazy-rest (lazy-range 3)))) "lazy-rest")
(assert (lazy-empty? (lazy-nil)) "lazy-nil is empty")
(assert (not (lazy-empty? (lazy-range 1))) "a non-empty sequence is not empty")
(assert (= 0 (length (lazy-to-array (lazy-nil)))) "realizing the empty sequence")
(assert (nil? (lazy-first (lazy-nil))) "lazy-first of empty is nil")

###
### Reduction and consumption
###

(assert (= 6 (lazy-reduce + 0 (lazy-range 4))) "lazy-reduce")
(def seen @[])
(lazy-each (fn [x] (array/push seen x)) (lazy-range 3))
(assert (deep= @[0 1 2] seen) "lazy-each")

# The fiber view makes ordinary `each` work, but it is one-shot: a second
# traversal of the SAME fiber yields nothing. The sequence itself remains
# re-traversable, so a fresh fiber is needed per pass.
(def seq4 (lazy-range 4))
(assert (deep= @[0 1 2 3] (do (def a @[]) (each v (lazy-to-fiber seq4) (array/push a v)) a))
        "each over a lazy-to-fiber view")
(assert (deep= @[0 1 2 3] (do (def a @[]) (each v (lazy-to-fiber seq4) (array/push a v)) a))
        "a fresh fiber view re-traverses the same sequence")
(def one-shot (lazy-to-fiber seq4))
(do (def a @[]) (each v one-shot (array/push a v)))
(assert (= 0 (length (do (def a @[]) (each v one-shot (array/push a v)) a)))
        "a fiber view is one-shot")

###
### Scale and resilience
###
### A long sequence must walk without growing the stack. Building one must not
### overflow the collector, which marks closure environments iteratively (see
### janet_drain_env_queue in gc.c); before that fix this crashed near 180k.

(assert (= 300000 (length (lazy-to-array (lazy-range 300000))))
        "walking a 300k lazy sequence")
(assert (= 300000 (lazy-reduce (fn [acc _] (+ acc 1)) 0 (lazy-range 300000)))
        "reducing a 300k lazy sequence")
(gccollect)
(assert (= 0 (lazy-first (lazy-range 5))) "a sequence survives a collection")

(end-suite)
