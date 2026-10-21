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
# Scope: VM collector contract only, including when hosted by an AOT binary.
# Build observed functions dynamically to prevent native replacement. Passing
# this suite under --aot --native is NOT native collector parity: native
# wrappers are cfunctions and do not produce these JanetFunction type/range
# rows. Their distinct telemetry counts wrapper calls and guard failures;
# test/suite-callgraph.janet covers its identity, guards, and release switch.
# Native type/range observation would need a separate identity/API contract,
# not fabricated VM rows or weakened assertions here.

(def prof-add (eval '(fn prof-add [a b] (+ a b))))
(def prof-mixed (eval '(fn prof-mixed [x] (if (> x 0) x "neg"))))

(debug/profile-start)
(prof-add 1 2)
(prof-add 1.5 2)
(prof-mixed 5)
(prof-mixed -1)
# unions in one slot
(def prof-u (eval '(fn prof-u [x] x)))
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
# AOT may profile only the single explicit thunk, without loader call rows.
(assert (> (length back) 0) "dump has rows")
(var saw-add false)
(each [id info] back
  (when (= (id 2) "prof-add")
    (set saw-add true)
    (assert (= 1 (get info :calls)) "dumped calls")))
(assert saw-add "prof-add in dump")

# Per-PC edge evidence is opt-in and keyed by the original bytecode index.
# Each pair is [fallthrough taken]; a loop must observe both successors
# somewhere, and resetting the collector must discard the previous counts.
(def prof-edges (eval '(fn prof-edges [n]
  (var sum 0)
  (var i 0)
  (while (< i n)
    (if (zero? (mod i 2)) (set sum (+ sum i)) (set sum (- sum i)))
    (++ i))
  sum)))
(debug/profile-start)
(assert (= -2 (prof-edges 4)) "profile edge fixture result")
(assert (= 0 (prof-edges 0)) "profile edge fixture empty loop")
(def edge-table (debug/profile-stop))
(var edge-entry nil)
(eachk key edge-table
  (def value (get edge-table key))
  (when (= prof-edges (get value :func)) (set edge-entry value)))
(assert edge-entry "profile edge entry exists")
(def edge-counts (get edge-entry :edges))
(assert (indexed? edge-counts) "small function has bounded edge slots")
(assert (zero? (mod (length edge-counts) 2)) "edge pairs match bytecode PCs")
(var saw-taken false)
(var saw-fallthrough false)
(for pc 0 (div (length edge-counts) 2)
  (when (> (edge-counts (+ (* pc 2) 1)) 0) (set saw-taken true))
  (when (> (edge-counts (* pc 2)) 0) (set saw-fallthrough true)))
(assert (and saw-taken saw-fallthrough) "both branch outcomes observed")
(debug/profile-start)
(prof-edges 0)
(debug/profile-dump "/tmp/suite-profile-edges.jdn")
(def edge-dump (parse (slurp "/tmp/suite-profile-edges.jdn")))
(var saw-edge-dump false)
(each [id info] edge-dump
  (when (= (id 2) "prof-edges")
    (set saw-edge-dump true)
    (assert (indexed? (get info :edges)) "edge counts survive dump")))
(assert saw-edge-dump "edge dump contains function")

# Collection reads sample receiver types at their original PC, before the
# operation runs. A single site seeing both an array and a tuple must retain
# the union; a different site in the same def observes a table separately.
(def prof-receivers (eval '(fn prof-receivers [value record]
  (+ (get value 0) (get record :key)))))
(debug/profile-start)
(prof-receivers @[1] @{:key 2})
(prof-receivers [3] @{:key 4})
(def receiver-table (debug/profile-stop))
(var receiver-entry nil)
(eachk key receiver-table
  (def value (get receiver-table key))
  (when (= prof-receivers (get value :func)) (set receiver-entry value)))
(assert receiver-entry "receiver profile entry exists")
(def receiver-sites (get receiver-entry :receivers))
(assert (= (length receiver-sites) (div (length (get receiver-entry :edges)) 2))
        "receiver sites align with original bytecode")
(var mixed-site false)
(var table-site false)
(each mask receiver-sites
  (when (and (not (zero? (band mask (blshift 1 7))))
             (not (zero? (band mask (blshift 1 8)))))
    (set mixed-site true))
  (when (not (zero? (band mask (blshift 1 9)))) (set table-site true)))
(assert (and mixed-site table-site) "receiver types stay per-site")
(debug/profile-start)
(prof-receivers @[5] @{:key 6})
(debug/profile-dump "/tmp/suite-profile-receivers.jdn")
(var receiver-dump nil)
(each [id info] (parse (slurp "/tmp/suite-profile-receivers.jdn"))
  (when (= (id 2) "prof-receivers") (set receiver-dump (get info :receivers))))
(assert (indexed? receiver-dump) "receiver evidence survives dump")
(assert (not (nil? (find |(and (indexed? $) (index-of :array $)) receiver-dump)))
        "dump decodes receiver type tags")

# reset clears: old entries must not survive, even though the loader's
# own evaluate/eval1 calls (one per top-level form) legitimately record
(debug/profile-reset)
(def ptab2 (debug/profile-stop))
(var f-calls nil)
(eachk k ptab2
  (def e (get ptab2 k))
  (when (= (get e :func) prof-add) (set f-calls (get e :calls))))
(assert (nil? f-calls) "reset clears old entries")

# range evidence: per-slot min/max for number params (PGO nonneg shaping)
(debug/profile-start)
(def prof-mm (eval '(fn prof-mm [a b] (+ a b))))
(prof-mm 3 4)
(prof-mm -1 10)
(prof-mm 5 0)
(def ptab3 (debug/profile-stop))
(var e-mm nil)
(eachk k ptab3
  (def e (get ptab3 k))
  (when (= (get e :func) prof-mm) (set e-mm e)))
(assert (not (nil? e-mm)) "mm observed")
(def mm-mins (get e-mm :mins))
(def mm-maxs (get e-mm :maxs))
(assert (= 8 (length mm-mins)) "8 min slots")
(assert (= -1 (mm-mins 0)) "slot 0 min")
(assert (= 5 (mm-maxs 0)) "slot 0 max")
(assert (= 0 (mm-mins 1)) "slot 1 min")
(assert (= 10 (mm-maxs 1)) "slot 1 max")
# dump carries the ranges (nil where unobserved)
(debug/profile-start)
(prof-mm 3 4)
(debug/profile-dump "/tmp/suite-profile-mm.jdn")
(def back2 (parse (slurp "/tmp/suite-profile-mm.jdn")))
(var saw-mm false)
(each [id info] back2
  (when (= (id 2) "prof-mm")
    (set saw-mm true)
    (assert (= 3 ((get info :mins) 0)) "dumped min")
    (assert (= 3 ((get info :maxs) 0)) "dumped max")
    (assert (= 4 ((get info :mins) 1)) "dumped min 1")
    (assert (= 4 ((get info :maxs) 1)) "dumped max 1")))
(assert saw-mm "prof-mm in dump")

# non-finite bounds round-trip: inf/-inf serialize as tagged strings
# (bare inf prints fine but parse reads it back as a symbol, which
# aborted every profile re-load with "unknown symbol inf"); NaN stays
# a nil sentinel. Loaders map the strings back to float infinities.
(debug/profile-start)
(def prof-inf (eval '(fn prof-inf [x] x)))
(prof-inf 1.5)
(prof-inf math/inf)
(prof-inf (- math/inf))
(prof-inf math/nan)
(debug/profile-dump "/tmp/suite-profile-inf.jdn")
(def back-inf (parse (slurp "/tmp/suite-profile-inf.jdn")))
(var saw-inf false)
(each [id info] back-inf
  (when (= (id 2) "prof-inf")
    (set saw-inf true)
    (assert (= "-inf" ((get info :mins) 0)) "-inf min tagged string")
    (assert (= "inf" ((get info :maxs) 0)) "inf max tagged string")))
(assert saw-inf "prof-inf in dump")
(debug/profile-stop)

# Phase C: worker harvest. Profiling off -> empty harvest, nil thread rows.
# (The range test above leaves the collector on; stop it first.)
(debug/profile-stop)
(let [p (pool/spawn 2)]
  (pool/call p + [1 2])
  (pool/stop p)
  (assert (deep= @[] (pool/profile-harvest p)) "harvest empty when off"))
(let [t (thread/new (fn [x] (+ x 1)) 1)]
  (thread/join t)
  (assert (nil? (thread/profile t)) "thread rows nil when off"))

# Phase C: profiling on -> workers inherit, harvest carries rows with
# identity + calls + param evidence (task roots recorded explicitly:
# primed fibers bypass JOP_CALL).
(debug/profile-start)
(def prof-wfn (eval '(fn prof-wfn [x] (+ x 1))))
(let [p (pool/spawn 2)]
  (assert (= 42 (pool/call p prof-wfn [41])) "profiled pool call")
  (assert (= 43 (pool/call p prof-wfn [42])) "profiled pool call 2")
  (pool/stop p)
  (def h (pool/profile-harvest p))
  (assert (= 2 (length h)) "one batch per worker")
  (var saw-wfn false)
  (var total-calls 0)
  (var gmin nil)
  (each batch h
    (assert (indexed? batch) "batch is rows array")
    (each [id info] batch
      (when (= (id 2) "prof-wfn")
        (set saw-wfn true)
        (+= total-calls (get info :calls))
        (assert (deep= @[:number] ((get info :params) 0)) "worker param evidence")
        # mins are per worker (calls may split across workers); the
        # merge tool unions them, so pin the global min here.
        (def m ((get info :mins) 0))
        (when (number? m)
          (set gmin (if (nil? gmin) m (min gmin m)))))))
  (assert saw-wfn "prof-wfn in harvest")
  (assert (= 2 total-calls) "worker calls counted")
  (assert (= 41 gmin) "worker min evidence"))
(debug/profile-stop)

# Phase C: thread harvest mirrors pool harvest.
(debug/profile-start)
(def prof-tfn (eval '(fn prof-tfn [x] (* x 2))))
(let [t (thread/new prof-tfn 21)]
  (assert (= 42 (thread/join t)) "profiled thread call")
  (def rows (thread/profile t))
  (assert (indexed? rows) "thread rows array")
  (var saw-tfn false)
  (each [id info] rows
    (when (= (id 2) "prof-tfn")
      (set saw-tfn true)
      (assert (= 1 (get info :calls)) "thread call counted")
      (assert (deep= @[:number] ((get info :params) 0)) "thread param evidence")))
  (assert saw-tfn "prof-tfn in thread rows"))
(debug/profile-stop)

# Phase C: harvest drains (second read is empty).
(debug/profile-start)
(let [p (pool/spawn 1)]
  (pool/call p prof-wfn [1])
  (pool/stop p)
  (assert (> (length (pool/profile-harvest p)) 0) "first drain")
  (assert (deep= @[] (pool/profile-harvest p)) "second drain empty"))
(debug/profile-stop)

(end-suite)
