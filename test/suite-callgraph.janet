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

# Phase 4 step 1 (registration): tools/callgraph.janet extracts the
# FuncDef graph without running the program (single (do ...) unit,
# mkimage-style; macros pre-installed like loading does). Run it on
# fixtures and assert key lines. Each file runs in its own process, so
# no settings leak between suites.

(def fixture (string
  "(defn myfib [n] (if (< n 2) n (+ (myfib (- n 1)) (myfib (- n 2)))))\n"
  "(defn myodd [n] (if (= n 0) false true))\n"
  "(defn myeven [n] (if (= n 0) true (myodd (- n 1))))\n"
  "(defn mk-adder [n] (fn [x] (+ x n)))\n"
  "(defmacro my-or [a b] (tuple 'if a a b))\n"
  "(defn usem [x] (my-or x 10))\n"
  "(defn risky [code] (eval code))\n"
  "(defn main [] (print (myfib 10) (myeven 10) ((mk-adder 5) 1) (usem nil) (risky 1)))\n"))
(spit "/tmp/cg-suite-fixture.janet" fixture)
(assert (= 0 (os/execute ["/bin/sh" "-c"
  "./build/janet tools/callgraph.janet /tmp/cg-suite-fixture.janet > /tmp/cg-suite-out.txt"] :p))
  "callgraph runs")
(def text (slurp "/tmp/cg-suite-out.txt"))
(defn has [s] (not (nil? (string/find s text))))

# self recursion resolves to the same def
(assert (has "calls 1 1") "myfib self recursion")
# backward cross-def reference resolves (defs must precede use, as loading requires)
(assert (has "calls 3 2") "myeven -> myodd")
# closures nest via contains
(assert (has "contains 4 5") "mk-adder contains closure")
# macros expand away: no call edge to my-or anywhere
(assert (not (has "my-or")) "macro vanished")
# eval flags dynamic
(assert (has "dynamic 7 eval") "eval flagged dynamic")
# main's direct calls resolve; higher-order result call stays unknown
(assert (has "calls 8 1") "main -> myfib")
(assert (has "calls 8 3") "main -> myeven")
(assert (has "calls 8 6") "main -> usem")
(assert (has "calls 8 7") "main -> risky")
(assert (has "unknown 8 2") "higher-order stays unknown")
(assert (has "extern 8 print") "print extern")
(assert (has "stats 9 11 8 1 1") "stats line exact")

# fixpoint: reachability from thunks, caller sets, recursion, taint
(assert (has "reach 0 1 2 3 4 5 6 7 8") "all reachable")
(assert (has "callers 1 2 1 8") "myfib callers (self+main)")
(assert (has "callers 2 1 3") "myodd caller")
(assert (has "recursive 1") "myfib recursive")
(assert (not (has "recursive 3")) "myeven not recursive")
(assert (has "tainted 7") "risky tainted")
(assert (has "tainted 8") "main tainted via risky")
(assert (has "tainted 0") "thunk tainted via contains")
(assert (has "fix 9 1 3") "fix summary exact")

# same name in two files is ambiguous, not silently first-wins
(spit "/tmp/cg-amb1.janet" "(defn dup [] 1)\n")
(spit "/tmp/cg-amb2.janet" "(defn dup [] 2)\n(defn caller [] (dup))\n")
(assert (= 0 (os/execute ["/bin/sh" "-c"
  "./build/janet tools/callgraph.janet /tmp/cg-amb1.janet /tmp/cg-amb2.janet > /tmp/cg-amb-out.txt"] :p))
  "callgraph multi-file runs")
(assert (not (nil? (string/find "ambiguous 4 dup"
  (slurp "/tmp/cg-amb-out.txt")))) "duplicate def ambiguous")


# Step 3 (arity audit): called-as arity sets, mismatches, unknowns
(spit "/tmp/cg-arity.janet" (string
  "(defn f2 [a b] (+ a b))\n"
  "(defn opt3 [a b &opt c] (+ a b (if c c 0)))\n"
  "(defn varf [& xs] (length xs))\n"
  "(defn main [] (print (f2 1 2) (opt3 1 2) (opt3 1 2 3) (varf 1 2 3)))\n"))
(assert (= 0 (os/execute ["/bin/sh" "-c"
  "./build/janet tools/callgraph.janet /tmp/cg-arity.janet > /tmp/cg-arity-out.txt"] :p))
  "callgraph arity run")
(def atext (slurp "/tmp/cg-arity-out.txt"))
(defn ahas [s] (not (nil? (string/find s atext))))
(assert (ahas "called-as 1 2") "f2 called with 2")
(assert (ahas "called-as 2 2 3") "opt3 called with 2 and 3")
(assert (ahas "called-as 3 3") "varf called with 3")
(assert (ahas "arity 0 0") "no bad, no unknown")

# mismatch detector agrees with the compiler's own rejection
(spit "/tmp/cg-bad.janet" (string
  "(defn g3 [a b c] (+ a b c))\n"
  "(defn main [] (print (g3 1 2)))\n"))
(assert (= 0 (os/execute ["/bin/sh" "-c"
  "./build/janet tools/callgraph.janet /tmp/cg-bad.janet > /tmp/cg-bad-out.txt"] :p))
  "callgraph bad run")
(def btext (slurp "/tmp/cg-bad-out.txt"))
(assert (not (nil? (string/find "arity-bad 2 1 2" btext))) "too-few flagged")
(assert (not (nil? (string/find "arity 1 0" btext))) "bad count exact")

# Step 4 (type hints): Clojure-style optional annotations via the defn
# dict modifier; extraction, validation, and call-site linkage
(spit "/tmp/cg-hint.janet" (string
  "(defn add2 {:hint {:params [:double :double] :returns :double}} [x y] (+ x y))\n"
  "(defn half {:hint {:returns :double}} [x] (/ x 2))\n"
  "(defn main [] (print (add2 1.5 2.5) (half 4)))\n"))
(assert (= 0 (os/execute ["/bin/sh" "-c"
  "./build/janet tools/callgraph.janet /tmp/cg-hint.janet > /tmp/cg-hint-out.txt"] :p))
  "callgraph hint run")
(def htext (slurp "/tmp/cg-hint-out.txt"))
(assert (not (nil? (string/find "hint 1 double double -> double" htext))) "add2 hint line")
(assert (not (nil? (string/find "hint 2 - -> double" htext))) "half returns-only hint")
(assert (not (nil? (string/find "hints 2 0" htext))) "hint counts")
(assert (not (nil? (string/find "hint-call 1 2" htext))) "hinted callees linked")

# invalid hints are rejected, never silently accepted
(spit "/tmp/cg-hintbad.janet" (string
  "(defn f {:hint {:params [:double] :returns :bogus}} [x] x)\n"
  "(defn main [] (print (f 1)))\n"))
(assert (= 0 (os/execute ["/bin/sh" "-c"
  "./build/janet tools/callgraph.janet /tmp/cg-hintbad.janet > /tmp/cg-hintbad-out.txt"] :p))
  "callgraph hintbad run")
(def hbtext (slurp "/tmp/cg-hintbad-out.txt"))
(assert (not (nil? (string/find "hint-bad 1 bad-returns-hint" hbtext))) "bad returns flagged")
(assert (not (nil? (string/find "hints 1 1" hbtext))) "bad count in hints line")

(end-suite)
