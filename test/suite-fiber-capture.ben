# Copyright (c) 2026 Calvin Rose & contributors
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

# A container captured from an enclosing function scope and mutated inside an
# ev/go fiber must survive that fiber's completion.
#
# The completion minor sweeps by owner id. The write barrier used to record a
# cross-owner edge only under the CONTAINER's owner, while the minority lookup
# is keyed by the SWEEPING fiber's owner, so a parent-heap array holding
# values born on the child's heap was swept as the child's own garbage. The
# contents then read correctly inside the fiber and turned to garbage at
# completion -- see docs/internals/known-issues-fiber-child-minor.md.

(import ./helper :prefix "" :exit true)
(start-suite)

# The original, minimal shape: function-local array, loop, allocating body.
(defn captured-alloc []
  (def got @[])
  (ev/go (fn [] (for i 0 3 (array/push got (string i)))))
  (ev/sleep 0.05)
  got)

(assert (deep= @["0" "1" "2"] (captured-alloc))
        "captured local survives child-fiber completion")

# Repeat it: the sweep is a completion event, so a second round must hold too.
(assert (deep= @["0" "1" "2"] (captured-alloc))
        "repeated captured local survives")

# Strings delivered through a packing channel into a captured array.
(defn captured-channel []
  (def got @[])
  (def c (ev/thread-chan 8))
  (ev/go (fn [] (repeat 3 (array/push got (ev/take c)))))
  (ev/sleep 0.01)
  (ev/give c "aa")
  (ev/give c "bb")
  (ev/give c "cc")
  (ev/sleep 0.15)
  got)

(assert (deep= @["aa" "bb" "cc"] (captured-channel))
        "packing-channel strings survive in a captured local")

# A captured table, not just an array.
(defn captured-table []
  (def t @{})
  (ev/go (fn [] (for i 0 4 (put t i (string i)))))
  (ev/sleep 0.05)
  t)
(assert (deep= @{0 "0" 1 "1" 2 "2" 3 "3"} (captured-table))
        "captured table survives child-fiber completion")

# Values computed on the child heap must remain readable by the parent.
(defn captured-nested []
  (def outer @[])
  (ev/go (fn []
           (for i 0 3
             (array/push outer @{:i i :s (string i)}))))
  (ev/sleep 0.05)
  outer)
(def nested (captured-nested))
(assert (= 3 (length nested)) "captured nested containers survive")
(assert (deep= @{:i 0 :s "0"} (nested 0)) "first nested entry intact")
(assert (deep= @{:i 2 :s "2"} (nested 2)) "last nested entry intact")

(end-suite)
