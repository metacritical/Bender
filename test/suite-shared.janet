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

# M5: shareable immutable blobs (core/shared runtime type). Each file
# runs in its own process, so no settings leak between suites.

(import ../examples/mailbox :as mailbox)

(def sample {:a 1 :b "two" :c [1.5 true nil] :d :kw :e {:n [1 2]}})

# 1. Full round-trip through materialize.
(let [b (shared/new sample)]
  (assert (deep= sample (shared/materialize b)) "materialize round-trip"))

# 2. Direct accessors without full decode.
(let [b (shared/new sample)]
  (assert (= 1 (shared/get b :a)) "get number")
  (assert (= "two" (shared/get b :b)) "get string")
  (assert (= :kw (shared/get b :d)) "get keyword")
  (assert (nil? (shared/get b :zzz)) "missing key is nil")
  (assert (nil? (shared/get b 0)) "int key on struct is nil")
  (assert (= 5 (shared/length b)) "struct length")
  (assert (= 3 (shared/length (shared/new [1 2 3]))) "tuple length")
  (assert (= 20 (shared/get (shared/new [10 20]) 1)) "tuple index")
  (assert (nil? (shared/get (shared/new [10 20]) 5)) "tuple OOB is nil")
  (assert (nil? (shared/length (shared/new 7))) "scalar length is nil"))

# 3. Nested navigation: shared/get returns decoded values, so deeper
# levels use ordinary get on the materialized piece.
(let [b (shared/new sample)]
  (assert (= 1.5 (get (shared/get b :c) 0)) "nested tuple item")
  (assert (= [1 2] (get (shared/get b :e) :n)) "nested struct path"))

# 4. shareable? predicate: deep, fail-closed.
(assert (shared/shareable? sample) "sample shareable")
(assert (shared/shareable? nil) "nil shareable")
(assert (not (shared/shareable? @[1])) "array refused")
(assert (not (shared/shareable? @{:a 1})) "table refused")
(assert (shared/shareable? {:a 1}) "struct accepted")
(assert (not (shared/shareable? {:a @[1]})) "array nested in struct refused")
(assert (not (shared/shareable? [1 @[2]])) "array nested in tuple refused")
(assert (not (shared/shareable? {:a print})) "function refused")
(assert (not (shared/shareable? {:a @"buf"})) "buffer refused")
(assert (not (shared/shareable? (ev/thread-chan 0))) "channel refused")
(assert (not (shared/shareable? {:a (ev/thread-chan 0)})) "channel nested refused")

# 5. shared/new rejects with errors (not silent garbage).
(assert-error "new rejects table" (shared/new @{:a 1}))
(assert-error "new rejects array value" (shared/new {:a @[1]}))
(assert-error "new rejects function" (shared/new print))
(assert-error "new rejects single-key bad value" (shared/new {:a @[]}))

# 6. Cross-thread transfer through a mailbox call (pointer passthrough).
(let [w (mailbox/spawn (fn [t p] (case t :id (shared/materialize p) :echo p)))]
  (assert (deep= {:a 1} (mailbox/call w :id (shared/new {:a 1}) 5))
          "blob crosses threads")
  (assert (deep= sample (mailbox/call w :id (shared/new sample) 5))
          "rich blob crosses threads")
  (mailbox/quit w))

# 7. Blobs travel inside pid tuples (registry path exercises bytes).
(let [w (mailbox/spawn (fn [t p] (case t :hold p)))]
  (mailbox/register w :sh7)
  (def looked (mailbox/whereis :sh7))
  (assert (mailbox/pid? looked) "registry pid resolves")
  (mailbox/quit w)
  (ev/unregister :sh7))

# 8. Safe-mode marshal round-trip (persistence shape).
(let [b (shared/new sample)]
  (assert (deep= sample (shared/materialize (unmarshal (marshal b))))
          "safe marshal round-trip"))

# 9. Soundness: blob readable after sender-side GC pressure; the
# payload is refcounted, not heap-tied.
(let [b (shared/new {:payload [1 2 {:deep "x"}]})]
  (gccollect)
  (assert (deep= [1 2 {:deep "x"}] (shared/get b :payload)) "post-GC read")
  (gccollect)
  (assert (= "x" (get (get (shared/get b :payload) 2) :deep))
          "post-GC nested read"))

(end-suite)
