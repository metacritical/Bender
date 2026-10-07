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

(import ./helper :prefix "" :exit true)
(start-suite)

# PGO phase 1: VM type-profile collector. stop returns the live table;
# dump writes it as Janet data (no truncation past 160 rows).

(defn prof-add [a b] (+ a b))
(defn prof-mixed [x] (if (> x 0) x "neg"))

(debug/profile-start)
(prof-add 1 2)
(prof-add 1.5 2)
(prof-mixed 5)
(prof-mixed -1)
# unions in one slot
(defn prof-u [x] x)
(prof-u 1)
(prof-u "s")
(prof-u :k)
(def ptab (debug/profile-stop))
(assert (dictionary? ptab) "profile-stop returns table")
(assert (nil? (debug/profile-stop)) "second stop gives nil")

(defn find-entry [pred]
  (var hit nil)
  (eachk k ptab
    (def e (get ptab k))
    (when (pred e) (set hit e)))
  hit)

(def e-add (find-entry (fn [e] (= (get e :func) prof-add))))
(assert (not (nil? e-add)) "add observed")
(assert (= 2 (get e-add :calls)) "add called twice")
# live table stores integer bitmasks over JanetType (:number is bit 0)
(def add-params (get e-add :params))
(assert (= 8 (length add-params)) "8 param slots")
(assert (= 1 (add-params 0)) "slot 0 numbers")
(assert (= 1 (add-params 1)) "slot 1 numbers")
(assert (= 1 (get e-add :rets)) "returns number")

(def e-mixed (find-entry (fn [e] (= (get e :func) prof-mixed))))
(assert (not (nil? e-mixed)) "mixed observed")
(assert (= 1 ((get e-mixed :params) 0)) "mixed param number")

# union entry: prof-u called with number, string, keyword
# :number bit 0, :string bit 4, :keyword bit 6 => 81
(def e-u (find-entry (fn [e] (= 81 ((get e :params) 0)))))
(assert (not (nil? e-u)) "union slot observed")

# dump round-trip: full table, no pretty truncation
(debug/profile-start)
(prof-add 10 20)
(debug/profile-dump "/tmp/suite-profile-out.jdn")
(def back (parse (slurp "/tmp/suite-profile-out.jdn")))
(assert (indexed? back) "dump parses")
(assert (> (length back) 3) "dump has rows")
(var saw-add false)
(each [id info] back
  (when (= (id 2) "prof-add")
    (set saw-add true)
    (assert (= 1 (get info :calls)) "dumped calls")))
(assert saw-add "prof-add in dump")

# reset clears: old entries must not survive, even though the loader's
# own evaluate/eval1 calls (one per top-level form) legitimately record
(debug/profile-reset)
(def ptab2 (debug/profile-stop))
(var f-calls nil)
(eachk k ptab2
  (def e (get ptab2 k))
  (when (= (get e :func) prof-add) (set f-calls (get e :calls))))
(assert (nil? f-calls) "reset clears old entries")

(end-suite)
