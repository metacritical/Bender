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

# same name in two files is ambiguous, not silently first-wins
(spit "/tmp/cg-amb1.janet" "(defn dup [] 1)\n")
(spit "/tmp/cg-amb2.janet" "(defn dup [] 2)\n(defn caller [] (dup))\n")
(assert (= 0 (os/execute ["/bin/sh" "-c"
  "./build/janet tools/callgraph.janet /tmp/cg-amb1.janet /tmp/cg-amb2.janet > /tmp/cg-amb-out.txt"] :p))
  "callgraph multi-file runs")
(assert (not (nil? (string/find "ambiguous 4 dup"
  (slurp "/tmp/cg-amb-out.txt")))) "duplicate def ambiguous")

(end-suite)
