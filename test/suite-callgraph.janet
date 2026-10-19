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
  "./build/bender tools/callgraph.janet /tmp/cg-suite-fixture.janet > /tmp/cg-suite-out.txt"] :p))
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

# Same name in two files must never be silently first-wins. This used to
# assert "ambiguous 4 dup": with two defs named `dup` the resolver gave up.
# Per-unit source scoping (6f1a74d0, "Local names can resolve only within
# their own unit") made this PRECISE rather than merely safe -- `caller`
# lives in cg-amb2, and Janet's lexical scoping means it calls cg-amb2's
# OWN dup (def 3), never cg-amb1's (def 1). Asserting the precise edge is
# strictly stronger than asserting ambiguity: a first-wins bug would
# resolve to def 1 and fail the second assertion below.
(spit "/tmp/cg-amb1.janet" "(defn dup [] 1)\n")
(spit "/tmp/cg-amb2.janet" "(defn dup [] 2)\n(defn caller [] (dup))\n")
(assert (= 0 (os/execute ["/bin/sh" "-c"
  "./build/bender tools/callgraph.janet /tmp/cg-amb1.janet /tmp/cg-amb2.janet > /tmp/cg-amb-out.txt"] :p))
  "callgraph multi-file runs")
(def amb-text (slurp "/tmp/cg-amb-out.txt"))
(assert (not (nil? (string/find "calls 4 3" amb-text)))
  "duplicate def resolves to its OWN unit's def")
(assert (nil? (string/find "calls 4 1" amb-text))
  "duplicate def never resolves across units (no first-wins)")

# A cross-unit UNQUALIFIED name is not evidence of a call target: that unit
# simply does not compile, and the tool says so honestly instead of
# attributing the call to the same-named def in another file.
(spit "/tmp/cg-amb3.janet" "(defn dup [] 1)\n")
(spit "/tmp/cg-amb4.janet" "(defn caller2 [] (dup))\n")
(assert (= 0 (os/execute ["/bin/sh" "-c"
  "./build/bender tools/callgraph.janet /tmp/cg-amb3.janet /tmp/cg-amb4.janet > /tmp/cg-amb2-out.txt"] :p))
  "callgraph cross-unit runs")
(assert (not (nil? (string/find "uncompiled /tmp/cg-amb4.janet"
  (slurp "/tmp/cg-amb2-out.txt"))))
  "cross-unit unqualified name reported uncompiled, never attributed")


# Step 3 (arity audit): called-as arity sets, mismatches, unknowns
(spit "/tmp/cg-arity.janet" (string
  "(defn f2 [a b] (+ a b))\n"
  "(defn opt3 [a b &opt c] (+ a b (if c c 0)))\n"
  "(defn varf [& xs] (length xs))\n"
  "(defn main [] (print (f2 1 2) (opt3 1 2) (opt3 1 2 3) (varf 1 2 3)))\n"))
(assert (= 0 (os/execute ["/bin/sh" "-c"
  "./build/bender tools/callgraph.janet /tmp/cg-arity.janet > /tmp/cg-arity-out.txt"] :p))
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
  "./build/bender tools/callgraph.janet /tmp/cg-bad.janet > /tmp/cg-bad-out.txt"] :p))
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
  "./build/bender tools/callgraph.janet /tmp/cg-hint.janet > /tmp/cg-hint-out.txt"] :p))
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
  "./build/bender tools/callgraph.janet /tmp/cg-hintbad.janet > /tmp/cg-hintbad-out.txt"] :p))
  "callgraph hintbad run")
(def hbtext (slurp "/tmp/cg-hintbad-out.txt"))
(assert (not (nil? (string/find "hint-bad 1 bad-returns-hint" hbtext))) "bad returns flagged")
(assert (not (nil? (string/find "hints 1 1" hbtext))) "bad count in hints line")

# ^ syntax (reader-level hints via defn): same metadata as the dict form
(spit "/tmp/cg-caret.janet" (string
  "(defn add3 ^long [a ^double b] (+ a b))\n"
  "(defn main [] (print (add3 1 2.5)))\n"))
(assert (= 0 (os/execute ["/bin/sh" "-c"
  "./build/bender tools/callgraph.janet /tmp/cg-caret.janet > /tmp/cg-caret-out.txt"] :p))
  "callgraph caret run")
(def ctext (slurp "/tmp/cg-caret-out.txt"))
(assert (not (nil? (string/find "hint 1 number double -> long" ctext))) "caret hints match dict form")
(assert (not (nil? (string/find "hints 1 0" ctext))) "caret hint counts clean")

# raw fn with ^ params is a compile error, not a silent arity change
(spit "/tmp/cg-guard.janet" "(def bad (fn [x ^long y] x))\n")
(assert (= 0 (os/execute ["/bin/sh" "-c"
  "./build/bender tools/callgraph.janet /tmp/cg-guard.janet > /tmp/cg-guard-out.txt"] :p))
  "callgraph guard run")
(def gtext (slurp "/tmp/cg-guard-out.txt"))
(assert (not (nil? (string/find "uncompiled" gtext))) "raw fn ^ param refused")
(assert (not (nil? (string/find "cannot start with ^" gtext))) "guard message present")

# Step 6 (returns fixpoint): hints seed, inference propagates through
# calls and arithmetic; hinted contracts are never weakened
(spit "/tmp/cg-ret.janet" (string
  "(defn inc-l {:hint {:params [:long] :returns :long}} [x] (+ x 1))\n"
  "(defn wrap2 [x] (inc-l (inc-l x)))\n"
  "(defn mul-d ^double [x ^double y] (* x y))\n"
  "(defn use-d [x] (+ (mul-d x 1.5) 0.5))\n"
  "(defn poly [x] (if (> x 0) x nil))\n"
  "(defn main [] (print (inc-l 1) (wrap2 2) (mul-d 1 2.5) (use-d 3)))\n"))
(assert (= 0 (os/execute ["/bin/sh" "-c"
  "./build/bender tools/callgraph.janet /tmp/cg-ret.janet > /tmp/cg-ret-out.txt"] :p))
  "callgraph ret run")
(def rtext (slurp "/tmp/cg-ret-out.txt"))
(assert (not (nil? (string/find "ret 1 long" rtext))) "hinted long")
(assert (not (nil? (string/find "ret 2 long" rtext))) "propagated long via calls")
(assert (not (nil? (string/find "ret 3 double" rtext))) "hinted double held")
(assert (not (nil? (string/find "ret 4 double" rtext))) "propagated double via call result")
(assert (not (nil? (string/find "rets 2 2 0" rtext))) "rets summary")
# poly joins long with nil -> unknown -> no ret line for it (defs 0,5,6)
(assert (not (nil? (string/find "def 5 main" rtext))) "main is def 5")
(assert (nil? (string/find "ret 6 " rtext)) "no ret for main")
(assert (nil? (string/find "ret 5 " rtext)) "no ret for thunk")

# Hybrid inference: unhinted functions proven by call-site widening
(spit "/tmp/cg-hybrid.janet" (string
  "(defn inc-l {:hint {:params [:long] :returns :long}} [x] (+ x 1))\n"
  "(defn twice [x] (inc-l (inc-l x)))\n"
  "(defn quad [x] (twice (twice x)))\n"
  "(defn scale ^double [a ^double b] (+ (* a a) (* b b)))\n"
  "(defn main [] (print (quad 1) (scale 3 4) (twice 5)))\n"))
(assert (= 0 (os/execute ["/bin/sh" "-c"
  "./build/bender tools/callgraph.janet /tmp/cg-hybrid.janet > /tmp/cg-hybrid-out.txt"] :p))
  "callgraph hybrid run")
(def htext (slurp "/tmp/cg-hybrid-out.txt"))
(assert (not (nil? (string/find "ret 2 long" htext))) "unhinted twice inferred long")
(assert (not (nil? (string/find "ret 3 long" htext))) "chained unhinted quad inferred")
(assert (not (nil? (string/find "ptypes 2 long" htext))) "twice param widened from call site")
(assert (not (nil? (string/find "ptypes 3 long" htext))) "quad param widened")
(assert (not (nil? (string/find "ptypes 4 number double" htext))) "scale params from hint")

# Step 7 parity: hinted AND unhinted-inferred fns emit to unboxed C;
# native results must byte-match the janet interpreter on samples
(assert (= 0 (os/execute ["/bin/sh" "-c"
  "rm -rf /tmp/cg-em && ./build/bender tools/callgraph.janet /tmp/cg-hybrid.janet --emit-native /tmp/cg-em"] :p))
  "emit-native run")
(assert (= 0 (os/execute ["/bin/sh" "-c" "cc -O2 /tmp/cg-em/native.c -o /tmp/cg-em/native"] :p))
  "native.c compiles")
(assert (= 0 (os/execute ["/bin/sh" "-c"
  "/tmp/cg-em/native > /tmp/cg-em/actual.txt && cmp /tmp/cg-em/expected.txt /tmp/cg-em/actual.txt"] :p))
  "native/interpreter parity")
(def ntext (slurp "/tmp/cg-em/native.c"))
(assert (not (nil? (string/find "int64_t twice(int64_t p0)" ntext))) "unhinted fn emitted")
(assert (not (nil? (string/find "inc_l(p0)" ntext))) "native-to-native calls")

# Step 8 (branches): CFG + if/goto emission, parity through merges
(spit "/tmp/cg-branch.janet" (string
  "(defn iabs {:hint {:params [:long] :returns :long}} [x] (if (< x 0) (- 0 x) x))\n"
  "(defn maxi [a b] (if (> a b) a b))\n"
  "(defn clamp {:hint {:params [:long :long :long] :returns :long}} [x lo hi]\n"
  "  (if (< x lo) lo (if (> x hi) hi x)))\n"
  "(defn main [] (print (iabs -5) (maxi 2 7) (clamp 15 0 10)))\n"))
(assert (= 0 (os/execute ["/bin/sh" "-c"
  "rm -rf /tmp/cg-br && ./build/bender tools/callgraph.janet /tmp/cg-branch.janet --emit-native /tmp/cg-br"] :p))
  "branch emit run")
(assert (= 0 (os/execute ["/bin/sh" "-c"
  "cc -O2 -w /tmp/cg-br/native.c -o /tmp/cg-br/native"] :p)) "branch native compiles")
(assert (= 0 (os/execute ["/bin/sh" "-c"
  "/tmp/cg-br/native > /tmp/cg-br/actual.txt && cmp /tmp/cg-br/expected.txt /tmp/cg-br/actual.txt"] :p))
  "branch parity")
(def btext (slurp "/tmp/cg-br/native.c"))
(assert (not (nil? (string/find "goto L" btext))) "branches emit goto")
# unhinted maxi proven through merge dataflow
(assert (not (nil? (string/find "int64_t maxi(int64_t p0, int64_t p1)" btext)))
        "unhinted branched fn emitted")

# refusal: branch condition must be a bool slot (janet 0 is truthy)
(spit "/tmp/cg-badcond.janet" (string
  "(defn f {:hint {:params [:long] :returns :long}} [x] (if x 1 2))\n"
  "(defn main [] (print (f 1)))\n"))
(os/execute ["/bin/sh" "-c"
  "rm -rf /tmp/cg-bc && ./build/bender tools/callgraph.janet /tmp/cg-badcond.janet --emit-native /tmp/cg-bc > /tmp/cg-bc-out.txt"])
# the tool legitimately exits nonzero (nothing emittable); the report is the contract
(assert (not (nil? (string/find "branch condition is long not bool"
  (slurp "/tmp/cg-bc-out.txt")))) "truthy-on-number refused")

# Step 9 (loops): while loops, loop-carried slots, back edges
(spit "/tmp/cg-loop.janet" (string
  "(defn sum-to {:hint {:params [:long] :returns :long}} [n]\n"
  "  (var acc 0) (var i 0)\n"
  "  (while (< i n) (set acc (+ acc i)) (set i (+ i 1)))\n"
  "  acc)\n"
  "(defn fact {:hint {:params [:long] :returns :long}} [n]\n"
  "  (var r 1) (var k 2)\n"
  "  (while (<= k n) (set r (* r k)) (set k (+ k 1)))\n"
  "  r)\n"
  "(defn main [] (print (sum-to 10) (fact 5)))\n"))
(assert (= 0 (os/execute ["/bin/sh" "-c"
  "rm -rf /tmp/cg-lp && ./build/bender tools/callgraph.janet /tmp/cg-loop.janet --emit-native /tmp/cg-lp"] :p))
  "loop emit run")
(assert (= 0 (os/execute ["/bin/sh" "-c"
  "cc -O2 /tmp/cg-lp/native.c -o /tmp/cg-lp/native"] :p)) "loop native compiles")
(assert (= 0 (os/execute ["/bin/sh" "-c"
  "/tmp/cg-lp/native > /tmp/cg-lp/actual.txt && cmp /tmp/cg-lp/expected.txt /tmp/cg-lp/actual.txt"] :p))
  "loop parity")
(def ltext (slurp "/tmp/cg-lp/native.c"))
(assert (not (nil? (string/find "goto L" ltext))) "loops emit back-edge goto")
(assert (not (nil? (string/find "s1 = s1 + s2;" ltext))) "loop-carried slots reassign")

# divf/mod/rem (float-family): libm emission, parity incl. negatives + div-by-zero
(spit "/tmp/cg-fmod.janet" (string
  "(defn fdiv {:hint {:params [:double :double] :returns :double}} [a b] (div a b))\n"
  "(defn fmod {:hint {:params [:double :double] :returns :double}} [a b] (mod a b))\n"
  "(defn frem {:hint {:params [:double :double] :returns :double}} [a b] (% a b))\n"
  "(defn main [] (print (fdiv 7 2) \" \" (fmod 7 2) \" \" (frem 7 2) \" \" (fmod -7 3) \" \" (frem -7 3) \" \" (fmod 5 0)))\n"))
(assert (= 0 (os/execute ["/bin/sh" "-c"
  "rm -rf /tmp/cg-fm && ./build/bender tools/callgraph.janet /tmp/cg-fmod.janet --emit-native /tmp/cg-fm"] :p))
  "fmod emit run")
(assert (= 0 (os/execute ["/bin/sh" "-c"
  "cc -O2 /tmp/cg-fm/native.c -o /tmp/cg-fm/native"] :p)) "fmod native compiles")
(assert (= 0 (os/execute ["/bin/sh" "-c"
  "/tmp/cg-fm/native > /tmp/cg-fm/actual.txt && cmp /tmp/cg-fm/expected.txt /tmp/cg-fm/actual.txt"] :p))
  "fmod parity (floor-mod vs fmod, div-by-zero)")
(def fmtext (slurp "/tmp/cg-fm/native.c"))
(assert (not (nil? (string/find "floor(" fmtext))) "divf/mod emit floor")
(assert (not (nil? (string/find "fmod(" fmtext))) "rem emits fmod")
# reserved C identifiers get a k_ prefix (defining fmod is UB: the C
# compiler folds fmod(-2.5, 1.5) with builtin semantics, observed -1
# instead of floor-mod 0.5)
(assert (not (nil? (string/find "double k_fmod(double p0, double p1)" fmtext)))
        "reserved name fmod prefixed")

# refusal: divf/mod/rem on int widths (C trunc vs floor-mod divergence)
(spit "/tmp/cg-imod.janet" (string
  "(defn imod {:hint {:params [:long :long] :returns :long}} [a b] (mod a b))\n"
  "(defn main [] (print (imod 7 2)))\n"))
(os/execute ["/bin/sh" "-c"
  "rm -rf /tmp/cg-im && ./build/bender tools/callgraph.janet /tmp/cg-imod.janet --emit-native /tmp/cg-im > /tmp/cg-im-out.txt"])
(assert (not (nil? (string/find "mod needs float or nonneg-int operands"
  (slurp "/tmp/cg-im-out.txt")))) "int mod refused")

# divim (x / const): float kernels divide exactly like boxed; int refuses
(spit "/tmp/cg-divim.janet" (string
  "(defn halve {:hint {:params [:double] :returns :double}} [x] (/ x 2))\n"
  "(defn main [] (print (halve 7)))\n"))
(assert (= 0 (os/execute ["/bin/sh" "-c"
  "rm -rf /tmp/cg-di && ./build/bender tools/callgraph.janet /tmp/cg-divim.janet --emit-native /tmp/cg-di"] :p))
  "divim emit run")
(assert (= 0 (os/execute ["/bin/sh" "-c"
  "cc -O2 /tmp/cg-di/native.c -o /tmp/cg-di/native"] :p)) "divim native compiles")
(assert (= 0 (os/execute ["/bin/sh" "-c"
  "/tmp/cg-di/native > /tmp/cg-di/actual.txt && cmp /tmp/cg-di/expected.txt /tmp/cg-di/actual.txt"] :p))
  "divim parity")
(def ditext (slurp "/tmp/cg-di/native.c"))
(assert (not (nil? (string/find "p0 / 2" ditext))) "divim emits immediate divide")
(spit "/tmp/cg-divimi.janet" (string
  "(defn halve-i {:hint {:params [:long] :returns :long}} [x] (/ x 2))\n"
  "(defn main [] (print (halve-i 7)))\n"))
(os/execute ["/bin/sh" "-c"
  "rm -rf /tmp/cg-dii && ./build/bender tools/callgraph.janet /tmp/cg-divimi.janet --emit-native /tmp/cg-dii > /tmp/cg-dii-out.txt"])
# Refused upstream of the operand check: boxed (/) is real division, so
# the result slot types :double and a :long kernel refuses at the ret
# check ("div/divim need float operands" per the divergence table).
(assert (not (nil? (string/find "ret slot is double not long"
  (slurp "/tmp/cg-dii-out.txt")))) "int divim refused")

# regression (ttfx floor-div miscompile): int64 OPERANDS in a :double
# fn must refuse — C int-div truncates before floor (-3) where boxed
# floor-divides real (-4). Checking rt alone is not enough. (Source
# `div` is opcode `divf`.)
(spit "/tmp/cg-divmix.janet" (string
  "(defn fdiv-mix {:hint {:params [:long :long] :returns :double}} [a b] (div a b))\n"
  "(defn main [] (print (fdiv-mix -7 2)))\n"))
(os/execute ["/bin/sh" "-c"
  "rm -rf /tmp/cg-dm && ./build/bender tools/callgraph.janet /tmp/cg-divmix.janet --emit-native /tmp/cg-dm > /tmp/cg-dm-out.txt 2>&1"])
(assert (not (nil? (string/find "divf needs float or nonneg-int operands"
  (slurp "/tmp/cg-dm-out.txt")))) "int operands in double fn refused")

# mixed int/double division: the int64 operand casts through (double)
# (boxed div is always real), dest declares double. Even 0 denominators
# match (double inf, no trap).
(spit "/tmp/cg-divcast.janet" (string
  "(defn fdivm {:hint {:params [:long :double] :returns :double}} [a b] (div a b))\n"
  "(defn main [] (print (fdivm 7 2)))\n"))
(assert (= 0 (os/execute ["/bin/sh" "-c"
  "rm -rf /tmp/cg-dc && ./build/bender tools/callgraph.janet /tmp/cg-divcast.janet --emit-native /tmp/cg-dc"] :p))
  "mixed-div emit run")
(assert (= 0 (os/execute ["/bin/sh" "-c"
  "cc -O2 /tmp/cg-dc/native.c -o /tmp/cg-dc/native"] :p)) "mixed-div compiles")
(assert (= 0 (os/execute ["/bin/sh" "-c"
  "/tmp/cg-dc/native > /tmp/cg-dc/actual.txt && cmp /tmp/cg-dc/expected.txt /tmp/cg-dc/actual.txt"] :p))
  "mixed-div parity (incl. div-by-zero)")
(assert (not (nil? (string/find "(double)" (slurp "/tmp/cg-dc/native.c"))))
        "int operand casts through double")

# PGO-nonneg int path: unanimous min >= 0 on unrebound :long params
# unlocks trunc-division (== floor there) with a fail-loud zero guard.
# NOTE: the --emit-native sample grid feeds 0 denominators, which the
# guard aborts by design — so parity goes through the end-to-end PGO
# gate (profile, build, stdout-compare, install), with a nonzero workload.
(spit "/tmp/cg-nneg.janet" (string
  "(defn half {:hint {:params [:long :long] :returns :long}} [a b] (div a b))\n"
  "(print (half 7 2) \" \" (half 9 4))\n"))
(assert (= 0 (os/execute ["/bin/sh" "-c"
  (string "rm -rf /tmp/cg-nn-bin* && ./build/bender --aot /tmp/cg-nneg.janet --native"
          " --pgo \"\" -o /tmp/cg-nn-bin")] :p))
  "nonneg PGO end-to-end parity")
# the gate installs the binary only on byte-parity; double-check output
(assert (= "3 2" (string/trim (slurp "/tmp/cg-nn-bin-native/expected.out")))
        "oracle output sane")
(def nntext (slurp "/tmp/cg-nn-bin-native/native.c"))
(assert (not (nil? (string/find "int div out of nonneg domain" nntext))) "zero guard emitted")
(assert (not (nil? (string/find " / " nntext))) "trunc division emitted")
# negative evidence withholds: re-profile with a negative denominator
(spit "/tmp/cg-nneg2.janet" (string
  "(defn half {:hint {:params [:long :long] :returns :long}} [a b] (div a b))\n"
  "(defn main [&] (half 7 -2))\n"))
(os/execute ["/bin/sh" "-c"
  (string "rm -rf /tmp/cg-nn2 && JANET_PROFILE=1 JANET_PROFILE_OUT=/tmp/cg-nn2-prof.jdn"
          " ./build/bender /tmp/cg-nneg2.janet >/dev/null 2>&1"
          " && ./build/bender tools/callgraph.janet /tmp/cg-nneg2.janet"
          " --profile /tmp/cg-nn2-prof.jdn --emit-native /tmp/cg-nn2 > /tmp/cg-nn2-out.txt 2>&1")])
(assert (not (nil? (string/find "nonneg-int operands" (slurp "/tmp/cg-nn2-out.txt"))))
        "negative evidence refuses")

# Step 14 (call-shaped access): (a i) compiles to push+call with the
# array as callee; the kernelizer lowers it exactly like get.
# Same for tail-position (tcall) and tuples.
(spit "/tmp/cg-shape.janet" (string
  "(defn asum-c {:hint {:params [:array] :returns :double}} [a]\n"
  "  (var s 0.0)\n"
  "  (for i 0 (length a) (+= s (a i)))\n"
  "  s)\n"
  "(defn tfirst {:hint {:params [:tuple] :returns :double}} [t] (t 0))\n"
  "(defn main [] (print (asum-c @[1.5 2.5]) (tfirst (tuple 4 5))))\n"))
(assert (= 0 (os/execute ["/bin/sh" "-c"
  "rm -rf /tmp/cg-sh && ./build/bender tools/callgraph.janet /tmp/cg-shape.janet --emit-native /tmp/cg-sh"] :p))
  "shape emit run")
(assert (= 0 (os/execute ["/bin/sh" "-c"
  "cc -O2 -w -Isrc/include -Isrc/conf /tmp/cg-sh/native.c build/janet.o -lm -lpthread -ldl -o /tmp/cg-sh/native"] :p))
  "shape native compiles+links")
(assert (= 0 (os/execute ["/bin/sh" "-c"
  "/tmp/cg-sh/native > /tmp/cg-sh/actual.txt && cmp /tmp/cg-sh/expected.txt /tmp/cg-sh/actual.txt"] :p))
  "shape parity (call + tcall + tuple)")
(def shtext (slurp "/tmp/cg-sh/native.c"))
(assert (not (nil? (string/find "janet_unwrap_number" shtext))) "shape lowers to unwrap")
# multi-arg call to an array value stays a refused call (not a get)
(spit "/tmp/cg-shapebad.janet" (string
  "(defn bad2 {:hint {:params [:array :long] :returns :double}} [a i] (a i 99))\n"
  "(defn main [] (print (bad2 @[1] 0)))\n"))
(os/execute ["/bin/sh" "-c"
  "rm -rf /tmp/cg-shb && ./build/bender tools/callgraph.janet /tmp/cg-shapebad.janet --emit-native /tmp/cg-shb > /tmp/cg-shb-out.txt 2>&1"])
(assert (not (nil? (string/find "native-skip" (slurp "/tmp/cg-shb-out.txt"))))
        "multi-arg array call refused")

# Step 16 (counted-loop upper elision): `for i 0 (len a)` drops the
# `>= count` half (loop discipline proves it), keeps `< 0`.
(os/execute ["/bin/sh" "-c"
  "rm -rf /tmp/cg-el && ./build/bender tools/callgraph.janet /tmp/cg-shape.janet --emit-native /tmp/cg-el"] :p)
# asum-c's counted loop elides to `< 0` only; tfirst (no loop) keeps
# the full guard, so assert presence of the elided form, not absence
# of the full one (covered by the opaque-loop test below).
(def eltext (slurp "/tmp/cg-el/native.c"))
(assert (not (nil? (string/find " < 0) {" eltext)))
        "upper guard elided, lower kept")
# negative: a true call inside the loop (opaque: the callee could in
# principle grow the array) keeps both halves, while the loop still
# emits via native-to-native calls
(spit "/tmp/cg-noelide.janet" (string
  "(defn inc1 {:hint {:params [:double] :returns :double}} [x] (+ x 1))\n"
  "(defn nsum {:hint {:params [:array] :returns :double}} [a]\n"
  "  (var s 0.0)\n"
  "  (for i 0 (length a) (+= s (inc1 (a i))))\n"
  "  s)\n"
  "(defn main [] (print (nsum @[1 2])))\n"))
(assert (= 0 (os/execute ["/bin/sh" "-c"
  "rm -rf /tmp/cg-ne && ./build/bender tools/callgraph.janet /tmp/cg-noelide.janet --emit-native /tmp/cg-ne"] :p))
  "opaque-loop emit run")
(assert (not (nil? (string/find "< 0 ||" (slurp "/tmp/cg-ne/native.c"))))
        "opaque loop keeps full guard")

# lib path prints skip diagnostics (same contract as --emit-native):
# real builds use --emit-native-lib via bender --aot --native, and
# silent skips hide all signal (ttfx: 0 kernels, no reasons).
(spit "/tmp/cg-libskip.janet" (string
  "(defn k-ok {:hint {:params [:double] :returns :double}} [x] (+ x 1))\n"
  "(defn k-bad [t] (t :key))\n"
  "(defn call-ok {:hint {:params [:double] :returns :double}} [x] (k-ok x))\n"
  "(defn main [] (print (call-ok 3) (k-bad @{:key 2})))\n"
  "(try (k-ok \"bad\") ([e] nil))\n"))
(assert (= 0 (os/execute ["/bin/sh" "-c"
  "rm -rf /tmp/cg-ls && ./build/bender tools/callgraph.janet /tmp/cg-libskip.janet --emit-native-lib /tmp/cg-ls > /tmp/cg-ls-out.txt 2>&1"] :p))
  "lib emit run")
(def telemetry-c (slurp "/tmp/cg-ls/native.c"))
(assert (string/find "JANET_NATIVE_TELEMETRY" telemetry-c)
        "wrapper telemetry has release compile switch")
(assert (string/find "_telemetry_report" telemetry-c)
        "wrapper telemetry snapshots at exit")
(assert (string/find "type-guard-failures" telemetry-c)
        "wrapper telemetry distinguishes type guard failures")
(assert (string/find "janet_type(argv[0]) != JANET_NUMBER" telemetry-c)
        "wrapper validates actual Janet number type")
(assert (string/find "arity-guard-failures" telemetry-c)
        "wrapper telemetry distinguishes arity failures")
(assert (string/find "janet_type(argv[0]) != JANET_NUMBER" telemetry-c)
        "native double boundary validates Janet number type")
# All numeric width hints must use Janet-number guards, not callable case tuples.
(spit "/tmp/cg-guard-width.janet" (string
  "(defn half-f32 {:hint {:params [:float] :returns :float}} [x] (* x 0.5))\n"
  "(defn half-i32 {:hint {:params [:i32] :returns :i32}} [x] (+ x 1))\n"
  "(defn main [] (print (half-f32 4) (half-i32 4)))\n"))
(assert (= 0 (os/execute ["/bin/sh" "-c"
  "rm -rf /tmp/cg-guard-width && ./build/bender tools/callgraph.janet /tmp/cg-guard-width.janet --emit-native-lib /tmp/cg-guard-width >/tmp/cg-guard-width.log 2>&1"] :p))
  "float and integer native wrappers emit")
(def guard-width-c (slurp "/tmp/cg-guard-width/native.c"))
(assert (string/find "half_f32 argument 0 has wrong Janet type" guard-width-c)
        "float32 wrapper emits number guard")
(assert (string/find "half_i32 argument 0 has wrong Janet type" guard-width-c)
        "integer wrapper emits number guard")
(assert (string/find "native-telemetry id=00000001 name=k_ok" telemetry-c)
        "wrapper telemetry reports stable numeric kernel identity")
(assert (string/find "atexit(native_telemetry_report_all)" telemetry-c)
        "telemetry snapshots through atexit")
# A build with wrappers announces them; a build with NONE must stay silent,
# so native_init never pollutes the stderr of a subprocess that happens to
# emit no kernels. This used to assert the C-level runtime guard
# `if (sizeof(regs)/sizeof(regs[0]) > 1)`. 978400f1 moved that decision
# compile-time into the emitter -- the fprintf is only written when
# `kernels` is non-empty -- which is strictly better (a zero-kernel build
# emits no statement at all, rather than a runtime check that is always
# false). Assert the BEHAVIOR, not the retired mechanism: this build has
# wrappers so it reports them, and the zero-kernel build below emits
# nothing.
(assert (string/find "native_init: registered" telemetry-c)
        "a build WITH wrappers announces them")
(spit "/tmp/cg-zerokernel.janet"
  "(defn opaque [x] (string x))\n(defn main [] (print (opaque 1)))\n")
(assert (= 0 (os/execute ["/bin/sh" "-c"
  "rm -rf /tmp/cg-zerokernel && ./build/bender tools/callgraph.janet /tmp/cg-zerokernel.janet --emit-native-lib /tmp/cg-zerokernel >/dev/null 2>&1"] :p))
  "zero-kernel native lib emits")
(assert (nil? (string/find "native_init: registered"
                           (slurp "/tmp/cg-zerokernel/native.c")))
        "zero-wrapper native init does not pollute subprocess output")
(assert (= 0 (os/execute ["cc" "-O2" "-w" "-fwrapv"
  "-DJANET_NATIVE_TELEMETRY=0" "-Isrc/include" "-Isrc/conf"
  "/tmp/cg-ls/native.c" "build/libjanet.a" "-lm" "-lpthread"
  "-o" "/tmp/cg-ls/native-release"] :p))
  "release macro compiles telemetry out")
(assert (= 0 (os/execute ["/tmp/cg-ls/native-release"] :e
  {:out (file/open "/tmp/cg-ls-release.out" :w)
   :err (file/open "/tmp/cg-ls-release.err" :w)}))
  "macro-disabled wrapper runs")
(assert (= 0 (os/execute ["cc" "-O2" "-w" "-fwrapv"
  "-Isrc/include" "-Isrc/conf" "/tmp/cg-ls/native.c"
  "build/libjanet.a" "-lm" "-lpthread" "-o" "/tmp/cg-ls/native-dev"] :p))
  "development macro default compiles")
(assert (string/find "_TELEMETRY_HIT() (++k_ok_telemetry_calls)" telemetry-c)
        "wrapper collects calls without hot-path I/O")
(assert (= 0 (os/execute ["cc" "-fsyntax-only" "-Isrc/include" "-Isrc/conf"
  "-DJANET_NATIVE_TELEMETRY=0" "/tmp/cg-ls/native.c"] :p))
  "release macro compiles telemetry out")
(def lsout (slurp "/tmp/cg-ls-out.txt"))
(assert (not (nil? (string/find "native-skip" lsout))) "lib path prints skips")
(assert (not (nil? (string/find "k-bad" lsout))) "lib skip names the refused def")

# Step 15 (pure core calls): math/floor etc. resolve from ldc-loaded
# cfunction constants and lower to libm (exact: boxed bodies ARE these
# calls). Chained use (floor of a division) keeps temps numeric.
(spit "/tmp/cg-pure.janet" (string
  "(defn fl {:hint {:params [:double] :returns :double}} [x] (math/floor x))\n"
  "(defn hy {:hint {:params [:double :double] :returns :double}} [a b] (math/sqrt (+ (* a a) (* b b))))\n"
  "(defn blogd {:hint {:params [:double] :returns :double}} [v] (do (var n 0.0) (var x v)"
  " (while (> x 0) (set n (+ n 1)) (set x (math/floor (/ x 2)))) n))\n"
  "(defn main [] (print (fl 2.7) (hy 3 4) (blogd 7)))\n"))
(assert (= 0 (os/execute ["/bin/sh" "-c"
  "rm -rf /tmp/cg-pu && ./build/bender tools/callgraph.janet /tmp/cg-pure.janet --emit-native /tmp/cg-pu"] :p))
  "pure emit run")
(assert (= 0 (os/execute ["/bin/sh" "-c"
  "cc -O2 /tmp/cg-pu/native.c -lm -o /tmp/cg-pu/native"] :p)) "pure native compiles+links libm")
(assert (= 0 (os/execute ["/bin/sh" "-c"
  "/tmp/cg-pu/native > /tmp/cg-pu/actual.txt && cmp /tmp/cg-pu/expected.txt /tmp/cg-pu/actual.txt"] :p))
  "pure parity (floor/sqrt/chained floor-div)")
(def putext (slurp "/tmp/cg-pu/native.c"))
(assert (not (nil? (string/find "floor((double)" putext))) "floor lowers to libm")
(assert (not (nil? (string/find "sqrt((double)" putext))) "sqrt lowers to libm")
# arity mismatch stays boxed (pow takes 2)
(spit "/tmp/cg-purebad.janet" (string
  "(defn badp {:hint {:params [:double] :returns :double}} [x] (math/pow x))\n"
  "(defn main [] (print (badp 2)))\n"))
(os/execute ["/bin/sh" "-c"
  "rm -rf /tmp/cg-pub && ./build/bender tools/callgraph.janet /tmp/cg-purebad.janet --emit-native /tmp/cg-pub > /tmp/cg-pub-out.txt 2>&1"])
(assert (not (nil? (string/find "native-skip" (slurp "/tmp/cg-pub-out.txt"))))
        "intrinsic arity mismatch refused")

# Cross-return native calls: a :double callee composes into an
# :array caller (temp carries the callee type); args convert exactly.
(spit "/tmp/cg-compo.janet" (string
  "(defn dbl {:hint {:params [:double] :returns :double}} [x] (+ x 1.0))\n"
  "(defn usen {:hint {:params [:array] :returns :double}} [a]\n"
  "  (var s 0.0)\n"
  "  (for i 0 (length a) (+= s (dbl (a i))))\n"
  "  s)\n"
  "(defn main [] (print (usen @[1 2])))\n"))
(assert (= 0 (os/execute ["/bin/sh" "-c"
  "rm -rf /tmp/cg-co && ./build/bender tools/callgraph.janet /tmp/cg-compo.janet --emit-native /tmp/cg-co"] :p))
  "composition emit run")
(assert (= 0 (os/execute ["/bin/sh" "-c"
  "cc -O2 -w -Isrc/include -Isrc/conf /tmp/cg-co/native.c build/janet.o -lm -lpthread -ldl -o /tmp/cg-co/native"] :p))
  "composition compiles+links")
(assert (= 0 (os/execute ["/bin/sh" "-c"
  "/tmp/cg-co/native > /tmp/cg-co/actual.txt && cmp /tmp/cg-co/expected.txt /tmp/cg-co/actual.txt"] :p))
  "composition parity")
(assert (not (nil? (string/find "dbl(" (slurp "/tmp/cg-co/native.c"))))
        "callee inlined by direct call")

# Regression (nbody energy miscompile): bool/int temps shared with
# double writes must declare double, not int (truncated 0.5*m to 0).
# Exercises the join-aware declarations end to end with parity.
(assert (= 0 (os/execute ["/bin/sh" "-c"
  "rm -rf /tmp/cg-nb && ./build/bender tools/callgraph.janet bench/benchmarksgame/nbody.janet --emit-native /tmp/cg-nb"] :p))
  "nbody emit run")
(assert (= 0 (os/execute ["/bin/sh" "-c"
  "cc -O2 -w -Isrc/include -Isrc/conf /tmp/cg-nb/native.c build/janet.o -lm -lpthread -ldl -o /tmp/cg-nb/native"] :p))
  "nbody compiles+links")
(assert (= 0 (os/execute ["/bin/sh" "-c"
  "/tmp/cg-nb/native > /tmp/cg-nb/actual.txt && cmp /tmp/cg-nb/expected.txt /tmp/cg-nb/actual.txt"] :p))
  "nbody parity (640 samples incl. real magnitudes)")

# Step 10 (extern C calls): declared symbols emitted as direct calls,# harness linked against a real C implementation, parity via fallback
(spit "/tmp/cg-ext.janet" (string
  "(defn game-clamp [x lo hi] (if (< x lo) lo (if (> x hi) hi x)))\n"
  "(defn physics {:hint {:params [:double :double :double :double] :returns :double}}\n"
  "  [pos vel lo hi] (game-clamp (+ pos (* vel 0.016)) lo hi))\n"
  "(defn main [] (print (physics 0 100 0 10)))\n"))
(spit "/tmp/cg-ext-decls.janet" (string
  "(def externs {\"game-clamp\" {:c \"game_clamp\"\n"
  "               :params [:double :double :double]\n"
  "               :returns :double\n"
  "               :fallback (fn [x lo hi] (if (< x lo) lo (if (> x hi) hi x)))}})\n"))
(spit "/tmp/cg-ext-impl.c" (string
  "double game_clamp(double x, double lo, double hi) {\n"
  "  if (x < lo) return lo;\n"
  "  if (x > hi) return hi;\n"
  "  return x; }\n"))
(assert (= 0 (os/execute ["/bin/sh" "-c"
  "rm -rf /tmp/cg-ex && ./build/bender tools/callgraph.janet /tmp/cg-ext.janet --externs /tmp/cg-ext-decls.janet --emit-native /tmp/cg-ex"] :p))
  "extern emit run")
(assert (= 0 (os/execute ["/bin/sh" "-c"
  "cc -O2 /tmp/cg-ex/native.c /tmp/cg-ext-impl.c -o /tmp/cg-ex/native"] :p))
  "extern native compiles+links real C impl")
(assert (= 0 (os/execute ["/bin/sh" "-c"
  "/tmp/cg-ex/native > /tmp/cg-ex/actual.txt && cmp /tmp/cg-ex/expected.txt /tmp/cg-ex/actual.txt"] :p))
  "extern parity")
(def etext (slurp "/tmp/cg-ex/native.c"))
(assert (not (nil? (string/find "extern double game_clamp(double, double, double);" etext)))
        "extern decl hoisted")

# ---- width hints: full C primitive set ----
(spit "/tmp/cg-width.janet" (string
  "(defn add-i32 {:hint {:params [:i32 :i32] :returns :i32}} [a b] (+ a b))\n"
  "(defn mulf {:hint {:params [:float :float] :returns :float}} [a b] (* a b))\n"
  "(defn mix {:hint {:params [:int :double] :returns :double}} [n x] (+ x n))\n"
  "(defn main [] (print (add-i32 2 3) \" \" (mulf 1.5 2.5) \" \" (mix 7 0.5)))\n"))
(assert (= 0 (os/execute ["/bin/sh" "-c"
  "rm -rf /tmp/cg-w && ./build/bender tools/callgraph.janet /tmp/cg-width.janet --emit-native /tmp/cg-w"] :p))
  "width emit run")
(assert (= 0 (os/execute ["/bin/sh" "-c"
  "cc -w -O2 /tmp/cg-w/native.c -o /tmp/cg-w/native && /tmp/cg-w/native > /tmp/cg-w/actual.txt && cmp /tmp/cg-w/expected.txt /tmp/cg-w/actual.txt"] :p))
  "width parity (i32/float/mixed)")
(def wtext (slurp "/tmp/cg-w/native.c"))
(assert (not (nil? (string/find "int32_t add_i32(int32_t p0, int32_t p1)" wtext)))
        "i32 kernel exact width")
(assert (not (nil? (string/find "float mulf(float p0, float p1)" wtext)))
        "float kernel true float32")
(assert (not (nil? (string/find "double mix(int32_t p0, double p1)" wtext)))
        "mixed widths + :int alias canonicalized")
# int-width kernels get integer samples in the harness (no float
# literals coerced at the call boundary)
(assert (nil? (string/find "add_i32(-2.5" wtext)) "int-width harness samples integral")

# ---- Step 11: numeric array state (SoA spike) ----
(spit "/tmp/cg-arr.janet" (string
  "(defn asum {:hint {:params [:array] :returns :double}} [a]\n"
  "  (var s 0.0)\n"
  "  (for i 0 (length a)\n"
  "    (+= s (get a i)))\n"
  "  s)\n"
  "(defn ascale {:hint {:params [:array :double] :returns :double}} [a k]\n"
  "  (for i 0 (length a)\n"
  "    (put a i (* (get a i) k)))\n"
  "  (get a 0))\n"
  "(defn main [] (print (asum @[1.5 2.5]) (ascale @[1 2 3] 10)))\n"))
(assert (= 0 (os/execute ["/bin/sh" "-c"
  "rm -rf /tmp/cg-arr && ./build/bender tools/callgraph.janet /tmp/cg-arr.janet --emit-native /tmp/cg-arr"] :p))
  "array emit run")
(assert (= 0 (os/execute ["/bin/sh" "-c"
  (string "cc -O2 -w -Isrc/include -Isrc/conf /tmp/cg-arr/native.c build/janet.o"
          " -lm -lpthread -ldl -o /tmp/cg-arr/native"
          " && /tmp/cg-arr/native > /tmp/cg-arr/actual.txt"
          " && cmp /tmp/cg-arr/expected.txt /tmp/cg-arr/actual.txt")] :p))
  "array parity (sum over len/get loop + in-place scale via put)")
(def atext (slurp "/tmp/cg-arr/native.c"))
(assert (not (nil? (string/find "JanetArray* p0" atext))) "array param exact type")
(assert (not (nil? (string/find "->count" atext))) "len lowers to count")
(assert (not (nil? (string/find "->data[" atext))) "get/put lower to data[]")
(assert (not (nil? (string/find "array index out of bounds" atext)))
        "OOB guard emitted")
(assert (not (nil? (string/find "#include <janet.h>" atext)))
        "janet.h included for array kernels")
# fail-closed rules: float index and len-of-number skip with reasons
(spit "/tmp/cg-arrbad.janet" (string
  "(defn gbad {:hint {:params [:array :double] :returns :double}} [a i] (get a i))\n"
  "(defn lns {:hint {:params [:double] :returns :double}} [x] (length x))\n"
  "# reachable (else dropped pre-analysis) but never run: emit fails first\n"
  "(defn main [] (print (gbad @[1] 0.5) (lns 3)))\n"))
(os/execute ["/bin/sh" "-c"
  "rm -rf /tmp/cg-arrbad && ./build/bender tools/callgraph.janet /tmp/cg-arrbad.janet --emit-native /tmp/cg-arrbad >/tmp/cg-arrbad.log 2>&1"] :p)
(def badout (slurp "/tmp/cg-arrbad.log"))
(assert (not (nil? (string/find "get requires array and int index" badout)))
        "float-index get skips with reason")
(assert (not (nil? (string/find "len of non-array" badout)))
        "len-of-number skips with reason")

# ---- Step 13: int-overflow policy + DCE ----
# promote: int widths compute as double (oracle-identical)
(spit "/tmp/cg-promote.janet" (string
  "(defn padd {:hint {:params [:long :long] :returns :long}} [a b] (+ a b))\n"
  "(defn main [] (print (padd 40 2)))\n"))
(assert (= 0 (os/execute ["/bin/sh" "-c"
  "rm -rf /tmp/cg-pr && ./build/bender tools/callgraph.janet /tmp/cg-promote.janet --int-policy promote --emit-native /tmp/cg-pr"] :p))
  "promote emit run")
(def prtext (slurp "/tmp/cg-pr/native.c"))
(assert (not (nil? (string/find "double padd(double p0, double p1)" prtext)))
        "promote maps int kernel to double")
(assert (= 0 (os/execute ["/bin/sh" "-c"
  "cc -O2 -w /tmp/cg-pr/native.c -o /tmp/cg-pr/native && /tmp/cg-pr/native > /tmp/cg-pr/actual.txt && cmp /tmp/cg-pr/expected.txt /tmp/cg-pr/actual.txt"] :p))
  "promote parity")
# wrap (default): -fwrapv gives defined two's-complement wrap
(assert (= 0 (os/execute ["/bin/sh" "-c"
  "rm -rf /tmp/cg-wr && ./build/bender tools/callgraph.janet /tmp/cg-promote.janet --emit-native /tmp/cg-wr && cc -O2 -w -fwrapv /tmp/cg-wr/native.c -o /tmp/cg-wr/native && /tmp/cg-wr/native > /tmp/cg-wr/actual.txt && cmp /tmp/cg-wr/expected.txt /tmp/cg-wr/actual.txt"] :p))
  "wrap parity under -fwrapv")
(def wrtext (slurp "/tmp/cg-wr/native.c"))
(assert (not (nil? (string/find "int64_t padd(int64_t p0, int64_t p1)" wrtext)))
        "wrap keeps int64 kernel")
# bad policy value is rejected
(assert (= 4 (os/execute ["/bin/sh" "-c"
  "./build/bender tools/callgraph.janet /tmp/cg-promote.janet --int-policy explode --emit-native /tmp/cg-bad >/dev/null 2>&1"] :p))
  "bad int-policy rejected")
# ---- Step 14: bitwise ops + array returns + puti ----
# int32-domain checked emission (fail loud like OOB); constant-index
# puts lower (puti was silently dropped before); array kernels return
# the in-place array.
(spit "/tmp/cg-bit.janet" (string
  "(defn k-xor8 {:hint {:params [:array :array :array] :returns :array}} [r a b]\n"
  "  (put r 0 (bxor (get a 0) (get b 0)))\n"
  "  (put r 1 (bxor (get a 1) (get b 1)))\n"
  "  (put r 2 (bxor (get a 2) (get b 2)))\n"
  "  (put r 3 (bxor (get a 3) (get b 3)))\n"
  "  (put r 4 (bxor (get a 4) (get b 4)))\n"
  "  (put r 5 (bxor (get a 5) (get b 5)))\n"
  "  (put r 6 (bxor (get a 6) (get b 6)))\n"
  "  (put r 7 (bxor (get a 7) (get b 7)))\n"
  "  r)\n"
  "(defn main [] (print (k-xor8 @[0 0 0 0 0 0 0 0] @[1 2 3 4 5 6 7 8] @[8 7 6 5 4 3 2 1])))\n"))
(assert (= 0 (os/execute ["/bin/sh" "-c"
  "rm -rf /tmp/cg-bit && ./build/bender tools/callgraph.janet /tmp/cg-bit.janet --emit-native /tmp/cg-bit"] :p))
  "bitwise emit run")
(def bittext (slurp "/tmp/cg-bit/native.c"))
(assert (not (nil? (string/find "k_bxor32" bittext))) "bxor lowers to helper")
(assert (not (nil? (string/find "int32 domain error" bittext))) "int32 guard emitted")
(assert (not (nil? (string/find "JanetArray* k_xor8" bittext))) "array in/out signature")
(assert (= 0 (os/execute ["/bin/sh" "-c"
  "cc -O2 -w -fwrapv -Isrc/include -Isrc/conf /tmp/cg-bit/native.c build/janet.o -lm -lpthread -ldl -o /tmp/cg-bit/native && /tmp/cg-bit/native > /tmp/cg-bit/actual.txt && cmp /tmp/cg-bit/expected.txt /tmp/cg-bit/actual.txt"] :p))
  "bitwise+puti+array-return parity")
# Optimizer level 1 (slot coalescing) must not cost kernels: the same
# k-xor8 shape, compiled with an in-file (setdyn :optimize 1) pin, makes
# the register allocator fuse the index temp into the get DEST slot
# (`get 3 2 3` -- dest == index). The emitter's state update writes the
# RESULT type into that slot before the emission gate reads it, so the
# gate must prove the index from the pre-update snapshot (get-now), not
# from st. This fixture pins level 1 regardless of the process default,
# so it keeps guarding the aliasing path even at default 0.
(spit "/tmp/cg-opt1.janet" (string
  "(setdyn :optimize 1)\n"
  "(defn k-xor8o {:hint {:params [:array :array :array] :returns :array}} [r a b]\n"
  "  (put r 0 (bxor (get a 0) (get b 0)))\n"
  "  (put r 1 (bxor (get a 1) (get b 1)))\n"
  "  (put r 2 (bxor (get a 2) (get b 2)))\n"
  "  (put r 3 (bxor (get a 3) (get b 3)))\n"
  "  (put r 4 (bxor (get a 4) (get b 4)))\n"
  "  (put r 5 (bxor (get a 5) (get b 5)))\n"
  "  (put r 6 (bxor (get a 6) (get b 6)))\n"
  "  (put r 7 (bxor (get a 7) (get b 7)))\n"
  "  r)\n"
  "(defn main [] (print (k-xor8o @[0 0 0 0 0 0 0 0] @[1 2 3 4 5 6 7 8] @[8 7 6 5 4 3 2 1])))\n"))
(assert (= 0 (os/execute ["/bin/sh" "-c"
  "rm -rf /tmp/cg-opt1 && ./build/bender tools/callgraph.janet /tmp/cg-opt1.janet --emit-native /tmp/cg-opt1"] :p))
  "level1 aliased-get emit run")
(assert (= 0 (os/execute ["/bin/sh" "-c"
  "cc -O2 -w -fwrapv -Isrc/include -Isrc/conf /tmp/cg-opt1/native.c build/janet.o -lm -lpthread -ldl -o /tmp/cg-opt1/native && /tmp/cg-opt1/native > /tmp/cg-opt1/actual.txt && cmp /tmp/cg-opt1/expected.txt /tmp/cg-opt1/actual.txt"] :p))
  "level1 aliased-get parity")
# DCE: unreachable hinted defns never reach the output
(spit "/tmp/cg-dce.janet" (string
  "(defn used {:hint {:params [:long] :returns :long}} [x] (+ x 1))\n"
  "(defn dead {:hint {:params [:long] :returns :long}} [x] (* x 2))\n"
  "(defn main [] (print (used 41)))\n"))
(assert (= 0 (os/execute ["/bin/sh" "-c"
  "rm -rf /tmp/cg-dce && ./build/bender tools/callgraph.janet /tmp/cg-dce.janet --emit-native /tmp/cg-dce >/dev/null 2>&1"] :p))
  "dce emit run")
# ---- Branches + shifts: CFG emission with bitwise ops ----
# k-shl64! exercises if/goto + sl/sr + puti + array in/out at parity.
# Regression cover for the :bool -> int declaration mapping (bool
# temps in array-returning functions previously fell back to the
# return width and miscompiled).
(spit "/tmp/cg-shl.janet" (string
  "(defn k-shl2 {:hint {:params [:array :array :long] :returns :array}} [r a k]\n"
  "  (def ws (brshift k 3))\n"
  "  (def bs (band k 7))\n"
  "  (def j0 (- 0 ws))\n"
  "  (if (>= j0 0)\n"
  "    (do (var v0 (blshift (get a j0) bs))\n"
  "      (if (> bs 0) (set v0 (bor v0 (brshift (get a 0) 1))) nil)\n"
  "      (put r 0 (band v0 255)))\n"
  "    (put r 0 0))\n"
  "  (def j1 (- 1 ws))\n"
  "  (if (>= j1 0)\n"
  "    (do (var v1 (blshift (get a j1) bs))\n"
  "      (put r 1 (band v1 255)))\n"
  "    (put r 1 0))\n"
  "  r)\n"
  "(defn main [&] (print (k-shl2 @[0 0] @[1 2 3 4 5 6 7 8] 17)))\n"))
(assert (= 0 (os/execute ["/bin/sh" "-c"
  "rm -rf /tmp/cg-shl && ./build/bender tools/callgraph.janet /tmp/cg-shl.janet --emit-native /tmp/cg-shl"] :p))
  "branch shift emit run")
(assert (= 0 (os/execute ["/bin/sh" "-c"
  "cc -O2 -w -fwrapv -Isrc/include -Isrc/conf /tmp/cg-shl/native.c build/janet.o -lm -lpthread -ldl -o /tmp/cg-shl/native && /tmp/cg-shl/native > /tmp/cg-shl/actual.txt && cmp /tmp/cg-shl/expected.txt /tmp/cg-shl/actual.txt"] :p))
  "branch shift parity")
# ---- PGO + widening: unhinted array kernel infers via call sites ----
# (array literals type :array; constructors consume pending pushes so
# literal elements never leak into widening as phantom :longs)
(spit "/tmp/cg-wide.janet" (string
  "(defn k-xor8u [r a b]\n"
  "  (put r 0 (bxor (get a 0) (get b 0)))\n"
  "  (put r 1 (bxor (get a 1) (get b 1)))\n"
  "  (put r 2 (bxor (get a 2) (get b 2)))\n"
  "  (put r 3 (bxor (get a 3) (get b 3)))\n"
  "  (put r 4 (bxor (get a 4) (get b 4)))\n"
  "  (put r 5 (bxor (get a 5) (get b 5)))\n"
  "  (put r 6 (bxor (get a 6) (get b 6)))\n"
  "  (put r 7 (bxor (get a 7) (get b 7)))\n"
  "  r)\n"
  "(defn main [&] (print (k-xor8u @[0 0 0 0 0 0 0 0] @[0 1 2 3 4 5 6 7] @[7 6 5 4 3 2 1 0])))\n"))
(assert (= 0 (os/execute ["/bin/sh" "-c"
  "rm -rf /tmp/cg-wide && ./build/bender tools/callgraph.janet /tmp/cg-wide.janet --emit-native /tmp/cg-wide"] :p))
  "unhinted widening emit run")
(assert (= 0 (os/execute ["/bin/sh" "-c"
  "cc -O2 -w -fwrapv -Isrc/include -Isrc/conf /tmp/cg-wide/native.c build/janet.o -lm -lpthread -ldl -o /tmp/cg-wide/native && /tmp/cg-wide/native > /tmp/cg-wide/actual.txt && cmp /tmp/cg-wide/expected.txt /tmp/cg-wide/actual.txt"] :p))
  "unhinted widening parity")
# ---- PGO phase 2: profile-seeded params emit without hints ----
(assert (= 0 (os/execute ["/bin/sh" "-c"
  "JANET_PROFILE=1 JANET_PROFILE_OUT=/tmp/cg-wide-prof.out ./build/bender /tmp/cg-wide.janet >/dev/null 2>&1"] :p))
  "profile run")
(assert (= 0 (os/execute ["/bin/sh" "-c"
  "rm -rf /tmp/cg-widep && ./build/bender tools/callgraph.janet /tmp/cg-wide.janet --profile /tmp/cg-wide-prof.out --emit-native /tmp/cg-widep && cc -O2 -w -fwrapv -Isrc/include -Isrc/conf /tmp/cg-widep/native.c build/janet.o -lm -lpthread -ldl -o /tmp/cg-widep/native && /tmp/cg-widep/native > /tmp/cg-widep/actual.txt && cmp /tmp/cg-widep/expected.txt /tmp/cg-widep/actual.txt"] :p))
  "profile-seeded parity")
(def dcetext (slurp "/tmp/cg-dce/native.c"))
(assert (not (nil? (string/find "used(" dcetext))) "reachable kernel emitted")
(assert (nil? (string/find "dead" dcetext)) "unreachable kernel eliminated")

# ---- Path 1: tuples (fixed positional struct-things) in kernels ----
(spit "/tmp/cg-tuple.janet" (string
  "(defn k-dot {:hint {:params [:tuple :tuple] :returns :double}} [a b]\n"
  "  (var acc 0)\n"
  "  (var i 0)\n"
  "  (def n (length a))\n"
  "  (while (< i n)\n"
  "    (set acc (+ acc (* (get a i) (get b i))))\n"
  "    (set i (+ i 1)))\n"
  "  acc)\n"
  "(defn k-first {:hint {:params [:tuple] :returns :double}} [t]\n"
  "  (get t 0))\n"
  "(defn main [&] (print (k-dot (tuple 1 2 3) (tuple 4 5 6))) (print (k-first (tuple 7 8))))\n"))
(assert (= 0 (os/execute ["/bin/sh" "-c"
  "rm -rf /tmp/cg-tup && ./build/bender tools/callgraph.janet /tmp/cg-tuple.janet --emit-native /tmp/cg-tup"] :p))
  "tuple emit run")
(assert (= 0 (os/execute ["/bin/sh" "-c"
  "cc -O2 -w -fwrapv -Isrc/include -Isrc/conf /tmp/cg-tup/native.c build/janet.o -lm -lpthread -ldl -o /tmp/cg-tup/native && /tmp/cg-tup/native > /tmp/cg-tup/actual.txt && cmp /tmp/cg-tup/expected.txt /tmp/cg-tup/actual.txt"] :p))
  "tuple parity")
(def tuptext (slurp "/tmp/cg-tup/native.c"))
(assert (not (nil? (string/find "double k_dot(JanetTuple p0, JanetTuple p1)" tuptext))) "tuple params emit")
(assert (not (nil? (string/find "janet_tuple_head" tuptext))) "tuple length via head")
# tuples are immutable: put refuses loudly, never miscompiles
(spit "/tmp/cg-tupbad.janet" (string
  "(defn k-tput {:hint {:params [:tuple :long] :returns :double}} [t v]\n"
  "  (put t 0 v)\n"
  "  1.0)\n"
  "(defn main [&] (print (k-tput (tuple 1 2) 9)))\n"))
(assert (= 1 (os/execute ["/bin/sh" "-c"
  "./build/bender tools/callgraph.janet /tmp/cg-tupbad.janet --emit-native /tmp/cg-tupbad > /tmp/cg-tupbad-out.txt 2>&1"] :p))
  "tuple put run (nothing emittable)")
(def tbtext (slurp "/tmp/cg-tupbad-out.txt"))
(assert (not (nil? (string/find "puti requires array" tbtext))) "tuple put refused")

# ---- Path 2: ffi/defstruct + :kernel intrinsics (native C structs) ----
(spit "/tmp/cg-struct.janet" (string
  "(ffi/defstruct Point x :double y :double)\n"
  "(defn k-move {:hint {:params [:pointer :double :double] :returns :double}} [p dx dy]\n"
  "  (Point/set-x p (+ (Point/x p) dx))\n"
  "  (Point/set-y p (+ (Point/y p) dy))\n"
  "  (+ (Point/x p) (Point/y p)))\n"
  "(defn main [&]\n"
  "  (def p (Point/malloc 1 2))\n"
  "  (print (k-move p 10 20))\n"
  "  (Point/free p))\n"))
(assert (= 0 (os/execute ["/bin/sh" "-c"
  "rm -rf /tmp/cg-st && ./build/bender tools/callgraph.janet /tmp/cg-struct.janet --emit-native /tmp/cg-st"] :p))
  "struct emit run")
(assert (= 0 (os/execute ["/bin/sh" "-c"
  "cc -O2 -w -fwrapv -Isrc/include -Isrc/conf /tmp/cg-st/native.c build/janet.o -lm -lpthread -ldl -o /tmp/cg-st/native && /tmp/cg-st/native > /tmp/cg-st/actual.txt && cmp /tmp/cg-st/expected.txt /tmp/cg-st/actual.txt"] :p))
  "struct parity")
(def sttext (slurp "/tmp/cg-st/native.c"))
(assert (not (nil? (string/find "double k_move(void* p0, double p1, double p2)" sttext))) "pointer params emit")
(assert (not (nil? (string/find "(*(double*)((char*)(p0) + 0))" sttext))) "field access is direct")

# ---- L0: --pgo auto-PGO loop (collect -> seed -> parity-gate -> install) ----
# validation: --pgo without --native refuses
(assert (= 2 (os/execute ["/bin/sh" "-c"
  "./build/bender --aot /tmp/cg-hybrid.janet --pgo '' -o /tmp/cg-pgo-no 2>/tmp/cg-pgo-no.log"] :p))
  "pgo without native exits 2")
(def nolog (slurp "/tmp/cg-pgo-no.log"))
(assert (not (nil? (string/find "needs --native" nolog))) "pgo without native says why")
# validation: --pgo with -c refuses (no binary, nothing to gate on)
(assert (= 2 (os/execute ["/bin/sh" "-c"
  "./build/bender --aot /tmp/cg-hybrid.janet --native --pgo '' -c -o /tmp/cg-pgo-c 2>/tmp/cg-pgo-c.log"] :p))
  "pgo with -c exits 2")
# full pipeline on a tiny deterministic program (empty workload args
# also proves --pgo '' still triggers the loop)
(spit "/tmp/cg-pgo.janet" (string
  "(defn k-double {:hint {:params [:long] :returns :long}} [x] (+ x x))\n"
  "(defn main [&] (print (k-double 21)) 0)\n"
  "# AOT images run the thunk only: entry must invoke main itself, and\n"
  "# (os/exit (main)) also shields the interpreter run from run-main's\n"
  "# second call, so both sides print exactly once.\n"
  "(os/exit (main))\n"))
(assert (= 0 (os/execute ["/bin/sh" "-c"
  "rm -f /tmp/cg-pgo-app && ./build/bender --aot /tmp/cg-pgo.janet --native --pgo '' -o /tmp/cg-pgo-app > /tmp/cg-pgo.log 2>&1"] :p))
  "pgo pipeline exits 0")
(def pgolog (slurp "/tmp/cg-pgo.log"))
(assert (not (nil? (string/find "pgo parity ok" pgolog))) "pgo parity gate passed")
(assert (= 0 (os/execute ["/bin/sh" "-c" "/tmp/cg-pgo-app > /tmp/cg-pgo-app.out"] :p))
  "pgo-installed binary runs")
(assert (= "42\n" (string (slurp "/tmp/cg-pgo-app.out"))) "pgo-installed binary output correct")
# the gate bites: os/clock always differs between the two runs
(spit "/tmp/cg-pgofail.janet" (string
  "(defn main [&] (print (os/clock)) 0)\n"
  "(os/exit (main))\n"))
(assert (= 1 (os/execute ["/bin/sh" "-c"
  "rm -f /tmp/cg-pgofail-app && ./build/bender --aot /tmp/cg-pgofail.janet --native --pgo '' -o /tmp/cg-pgofail-app > /tmp/cg-pgofail.log 2>&1"] :p))
  "pgo parity failure exits 1")
(def pgofaillog (slurp "/tmp/cg-pgofail.log"))
(assert (not (nil? (string/find "PGO PARITY FAILED" pgofaillog))) "pgo failure says why")
(assert (nil? (os/stat "/tmp/cg-pgofail-app")) "broken binary never installs")


# ---- `in` / `inim` in kernels ----
# ttfx indexes limb arrays with (in a i) rather than call syntax, and a
# constant index compiles to JOP_IN_IMMEDIATE. The kernel emitter knew
# only `get`, so such functions failed with "unhandled op in"/"unhandled
# op inim" and silently lost kernels. Both must emit like `get`, which is
# semantically the closer match: the emitted code ABORTS on a bad index,
# matching `in` raising, where `get` would return nil (no unboxed form).
(spit "/tmp/cg-in.janet" (string
  "(defn k-sum3 {:hint {:params [:array] :returns :double}} [a]\n"
  "  (+ (in a 0) (in a 1) (in a 2)))\n"          # constant -> inim
  "(defn k-sumv {:hint {:params [:array :long] :returns :double}} [a i]\n"
  "  (+ (in a i) (in a 0)))\n"                    # variable -> in, plus inim
  "(defn main [&] (print (k-sum3 @[1 2 3]) \" \" (k-sumv @[4 5 6] 1)))\n"))
(assert (= 0 (os/execute ["/bin/sh" "-c"
  "rm -rf /tmp/cg-in && ./build/bender tools/callgraph.janet /tmp/cg-in.janet --emit-native /tmp/cg-in"] :p))
  "in/inim kernel emit run")
(def intext (slurp "/tmp/cg-in/native.c"))
(assert (string/find "k_sum3" intext) "inim kernel emitted")
(assert (string/find "k_sumv" intext) "in kernel emitted")
(assert (= 0 (os/execute ["/bin/sh" "-c"
  "cc -O2 -w -fwrapv -Isrc/include -Isrc/conf /tmp/cg-in/native.c build/janet.o -lm -lpthread -ldl -o /tmp/cg-in/native && /tmp/cg-in/native > /tmp/cg-in/actual.txt && cmp /tmp/cg-in/expected.txt /tmp/cg-in/actual.txt"] :p))
  "in/inim kernel parity")


# ---- Stream F: u64 kernel arithmetic domain ----
# The limb-emulation blocker was the int32 bitwise domain. This adds a
# u64 domain internal to kernels: enter via (int/u64 n), leave via
# (int/to-number v); band/bor/bxor on u64s, shifts masked & 63, wrapping
# +. The boxed oracle is the int/u64 abstract (exact, verified). Guards
# abort exactly where boxed errors (negative/fractional cast input,
# to-number above 2^53). u64 never crosses the kernel ABI.
(spit "/tmp/cg-u64.janet" (string
  "(defn k-u64 {:hint {:params [:array :array] :returns :number}} [a b]\n"
  "  (def x (bor (blshift (int/u64 (in a 0)) 56) (int/u64 (in a 1))))\n"
  "  (def y (int/u64 (in b 0)))\n"
  "  (def z (bxor (+ x y) (brshift x 41)))\n"
  "  (int/to-number (band z (int/u64 255))))\n"
  "(defn main [] (print (k-u64 @[255 1] @[7])))\n"))
(assert (= 0 (os/execute ["/bin/sh" "-c"
  "rm -rf /tmp/cg-u64 && ./build/bender tools/callgraph.janet /tmp/cg-u64.janet --emit-native /tmp/cg-u64"] :p))
  "u64 kernel emit run")
(def u64text (slurp "/tmp/cg-u64/native.c"))
(assert (string/find "uint64_t" u64text) "u64 kernel declares uint64_t")
(assert (string/find "u64 cast domain error" u64text) "u64 cast guard emitted")
(assert (string/find "u64 to-number out of range" u64text) "u64 to-number guard emitted")
(assert (= 0 (os/execute ["/bin/sh" "-c"
  "cc -O2 -w -fwrapv -Isrc/include -Isrc/conf /tmp/cg-u64/native.c build/janet.o -lm -lpthread -ldl -o /tmp/cg-u64/native && /tmp/cg-u64/native > /tmp/cg-u64/actual.txt && cmp /tmp/cg-u64/expected.txt /tmp/cg-u64/actual.txt"] :p))
  "u64 kernel parity vs boxed oracle")
# negative guards: u64 must NOT be hintable as a param or return
(spit "/tmp/cg-u64bad.janet" (string
  "(defn k-bad {:hint {:params [:u64] :returns :number}} [a] a)\n"))
(assert (= 0 (os/execute ["/bin/sh" "-c"
  "rm -rf /tmp/cg-u64bad && ./build/bender tools/callgraph.janet /tmp/cg-u64bad.janet --emit-native /tmp/cg-u64bad 2>&1 | grep -q 'native-skip 1'"] :p))
  "u64 param hint refused (internal domain only)")


# ---- Stream F step 2: u64 xoshiro step (limb-emulation replacement) ----
# The real target of the u64 domain: a 64-bit RNG step over native
# uint64_t instead of 8 emulated limbs. Four intermediate failures had
# to be fixed for this to emit, each now regression-covered:
#   (a) :u64tonum evidence -- an (int/to-number v) result is exact in
#       both representations (the emission guards 2^53), so it must not
#       force a u64-reusing slot to refuse;
#   (b) decl-kind -- u64 writes WIN the declaration over :u64tonum
#       (resolving to double emitted `s24 | s25` with a double, a C
#       type error, and a double declared for a u64-written slot);
#   (c) :number admitted as in-domain FLOW evidence alongside real u64
#       writes (it is a flow type, not a concrete kind);
#   (d) store-ops in the backward arg walk -- (put r 0 (int/to-number
#       ...)) between pushes made the walk fail-closed on puti and
#       refuse a call whose arguments were perfectly provable.
(spit "/tmp/cg-xor64.janet" (string
  "(defn k-x64 {:hint {:params [:array :array] :returns :array}} [r s]\n"
  "  (def w0 (bor (int/u64 (in s 0)) (blshift (int/u64 (in s 1)) 8)\n"
  "              (blshift (int/u64 (in s 2)) 16) (blshift (int/u64 (in s 3)) 24)\n"
  "              (blshift (int/u64 (in s 4)) 32) (blshift (int/u64 (in s 5)) 40)\n"
  "              (blshift (int/u64 (in s 6)) 48) (blshift (int/u64 (in s 7)) 56)))\n"
  "  (def rot (bor (blshift w0 23) (brshift w0 41)))\n"
  "  (def res (+ rot w0))\n"
  "  (put r 0 (int/to-number (band res (int/u64 255))))\n"
  "  (put r 1 (int/to-number (band (brshift res 8) (int/u64 255))))\n"
  "  (put r 2 (int/to-number (band (brshift res 16) (int/u64 255))))\n"
  "  (put r 3 (int/to-number (band (brshift res 24) (int/u64 255))))\n"
  "  (put r 4 (int/to-number (band (brshift res 32) (int/u64 255))))\n"
  "  (put r 5 (int/to-number (band (brshift res 40) (int/u64 255))))\n"
  "  (put r 6 (int/to-number (band (brshift res 48) (int/u64 255))))\n"
  "  (put r 7 (int/to-number (band (brshift res 56) (int/u64 255))))\n"
  "  r)\n"
  "(defn main [] (print (k-x64 @[0 0 0 0 0 0 0 0] @[9 8 7 6 5 4 3 2])))\n"))
(assert (= 0 (os/execute ["/bin/sh" "-c"
  "rm -rf /tmp/cg-xor64 && ./build/bender tools/callgraph.janet /tmp/cg-xor64.janet --emit-native /tmp/cg-xor64"] :p))
  "u64 xoshiro-shaped kernel emits")
(def x64c (slurp "/tmp/cg-xor64/native.c"))
# (a)/(b)/(c): the rotl-shaped slot and the to-number results compile as
# uint64_t / double with no cross-type bitwise expression
(assert (string/find "uint64_t" x64c) "u64 kernel declares uint64_t")
# every bitwise operand must be a uint64_t slot (the u64 domain), never
# a double -- a double in a bitwise expression is a C type error, which
# the compile assertion below would catch, but assert it structurally
# too so the failure names the real cause.
# collect "double sNN;" declarations by plain scanning (the slot number
# follows "double s" and ends at ';')
(defn x64-double-slot [line]
  (def at (string/find "double s" line))
  (when at
    (var i (+ at 8))
    (var num "")
    (def digs "0123456789")
    (while (and (< i (length line)) (string/find (string (line i)) digs))
      (set num (string num (line i)))
      (++ i))
    (when (> (length num) 0) (string "s" num))))
(var x64-double-slots @[])
(each line (string/split "\n" x64c)
  (def ds (x64-double-slot line))
  (when ds (array/push x64-double-slots ds)))
(var x64-bad 0)
(each line (string/split "\n" x64c)
  (each ds x64-double-slots
    (when (or (string/find (string " " ds " | ") line)
              (string/find (string " " ds " & ") line)
              (string/find (string " " ds " ^ ") line)
              (string/find (string "| " ds ";") line)
              (string/find (string "& " ds ";") line)
              (string/find (string "^ " ds ";") line))
      (++ x64-bad))))
(assert (= 0 x64-bad) "no double slot used as a bitwise operand")
# (d): emission succeeded at all, which requires the store-skipping walk
(assert (= 0 (os/execute ["/bin/sh" "-c"
  "cc -O2 -w -fwrapv -Isrc/include -Isrc/conf /tmp/cg-xor64/native.c build/janet.o -lm -lpthread -ldl -o /tmp/cg-xor64/native"] :p))
  "u64 xoshiro-shaped kernel compiles")
# and the emitted code agrees with the boxed oracle on its samples
(assert (= 0 (os/execute ["/bin/sh" "-c"
  "cd /tmp/cg-xor64 && ./native > actual.txt && cmp expected.txt actual.txt"] :p))
  "u64 xoshiro-shaped kernel parity vs boxed oracle")

(end-suite)
