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

# same name in two files is ambiguous, not silently first-wins
(spit "/tmp/cg-amb1.janet" "(defn dup [] 1)\n")
(spit "/tmp/cg-amb2.janet" "(defn dup [] 2)\n(defn caller [] (dup))\n")
(assert (= 0 (os/execute ["/bin/sh" "-c"
  "./build/bender tools/callgraph.janet /tmp/cg-amb1.janet /tmp/cg-amb2.janet > /tmp/cg-amb-out.txt"] :p))
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
(assert (not (nil? (string/find "mod on int width"
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
(assert (not (nil? (string/find "divim on int width"
  (slurp "/tmp/cg-dii-out.txt")))) "int divim refused")

# lib path prints skip diagnostics (same contract as --emit-native):
# real builds use --emit-native-lib via bender --aot --native, and
# silent skips hide all signal (ttfx: 0 kernels, no reasons).
(spit "/tmp/cg-libskip.janet" (string
  "(defn k-ok {:hint {:params [:double] :returns :double}} [x] (+ x 1))\n"
  "(defn k-bad [t] (t :key))\n"
  "(defn main [] (print (k-ok 1) (k-bad @{:key 2})))\n"))
(assert (= 0 (os/execute ["/bin/sh" "-c"
  "rm -rf /tmp/cg-ls && ./build/bender tools/callgraph.janet /tmp/cg-libskip.janet --emit-native-lib /tmp/cg-ls > /tmp/cg-ls-out.txt 2>&1"] :p))
  "lib emit run")
(def lsout (slurp "/tmp/cg-ls-out.txt"))
(assert (not (nil? (string/find "native-skip" lsout))) "lib path prints skips")
(assert (not (nil? (string/find "k-bad" lsout))) "lib skip names the refused def")

# Step 10 (extern C calls): declared symbols emitted as direct calls,
# harness linked against a real C implementation, parity via fallback
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

(end-suite)
