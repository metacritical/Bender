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
#
# Clojure-style struct destructuring: :keys / :or (specials.c).
# No :as whole-binding by design: {:as sym} keeps its legacy meaning
# (bind (get m :as)), which import* depends on — pinned below.

(import ./helper :prefix "" :exit true)
(start-suite)

# :keys vector: symbols bind from same-named keywords.
(let [{:keys [name age]} {:name "jan" :age 7 :extra 1}]
  (assert (= name "jan") "keys binds name")
  (assert (= age 7) "keys binds age"))

# :keys with keyword entries binds their symbol form.
(let [{:keys [:a]} {:a 1}]
  (assert (= a 1) "keys keyword entry"))

# :or defaults: missing and nil-valued keys fall back (literals only).
(let [{:keys [a b c] :or {b 20 c 30}} {:a 1 :c nil}]
  (assert (= a 1) "present value kept")
  (assert (= b 20) "missing key defaults")
  (assert (= c 30) "nil value defaults"))

# :or in fn params, both directions.
(assert (= 9 ((fn [{:keys [x] :or {x 9}}] x) {})) "param default on miss")
(assert (= 1 ((fn [{:keys [x] :or {x 9}}] x) {:x 1})) "param keeps present")

# Back-compat: non-sequential :keys value is a normal key binding.
(let [{:keys v} {:keys 42}]
  (assert (= v 42) "non-seq keys stays a lookup"))

# Legacy :as meaning pinned: binds (get m :as), NOT the whole map.
(let [{:as v} {:as 7 :other 1}]
  (assert (= v 7) ":as keeps legacy key meaning"))

# Nested :keys through a struct key.
(let [{:p {:keys [q]}} {:p {:q 8}}]
  (assert (= q 8) "nested keys"))

# def/var forms share the same path.
(def {:keys [dk] :or {dk 2}} {})
(assert (= dk 2) "def keys+or")
(var {:keys [vk]} {:vk 3})
(assert (= vk 3) "var keys")

(end-suite)
