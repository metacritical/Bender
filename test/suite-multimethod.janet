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
# Multimethods: defmulti/defmethod + introspection (boot.janet).

(import ./helper :prefix "" :exit true)
(start-suite)

(defmulti encounter (fn [x] (get x :type)))
(defmethod encounter :orc [x] (string "fight " (get x :name)))
(defmethod encounter :elf [x] (string "greet " (get x :name)))
(defmethod encounter :default [x] (string "ignore " (get x :name)))

(assert (= "fight grom" (encounter {:type :orc :name "grom"})) "orc dispatches")
(assert (= "greet luth" (encounter {:type :elf :name "luth"})) "elf dispatches")
(assert (= "ignore ??" (encounter {:type :rock :name "??"})) "default catches")

# get-method / remove-method / methods introspection.
(assert (function? (get-method encounter :orc)) "get-method finds")
(assert (function? (get-method encounter :nope)) "get-method falls back")
(assert (= 3 (length (methods encounter))) "three methods (incl default)")
(remove-method encounter :orc)
(assert (= "ignore grom" (encounter {:type :orc :name "grom"})) "removed falls back")
(try (do (remove-method encounter :default)
         (encounter {:type :rock :name "??"})
         (assert false "unreachable"))
     ([e] (assert (not (nil? (string/find "no method" e))) "missing method errors")))

# defmethod on a non-multimethod binding fails loud.
(def notmulti 42)
(try (eval '(defmethod notmulti :x [a] a))
     ([e] (assert (not (nil? (string/find "not a multimethod" e))) "unknown multi errors")))

# Custom default + non-keyword dispatch values.
(defmulti classify (fn [x] (type x)) {:default :thing})
(defmethod classify :number [x] (* x 2))
(defmethod classify :thing [x] :other)
(assert (= 10 (classify 5)) "type dispatch")
(assert (= :other (classify "s")) "custom default")

(end-suite)
