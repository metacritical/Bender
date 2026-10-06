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

# ^ syntax (reader-level hints via defn): same metadata as the dict form
(spit "/tmp/cg-caret.janet" (string
  "(defn add3 ^long [a ^double b] (+ a b))\n"
  "(defn main [] (print (add3 1 2.5)))\n"))
(assert (= 0 (os/execute ["/bin/sh" "-c"
  "./build/janet tools/callgraph.janet /tmp/cg-caret.janet > /tmp/cg-caret-out.txt"] :p))
  "callgraph caret run")
(def ctext (slurp "/tmp/cg-caret-out.txt"))
(assert (not (nil? (string/find "hint 1 number double -> long" ctext))) "caret hints match dict form")
(assert (not (nil? (string/find "hints 1 0" ctext))) "caret hint counts clean")

# raw fn with ^ params is a compile error, not a silent arity change
(spit "/tmp/cg-guard.janet" "(def bad (fn [x ^long y] x))\n")
(assert (= 0 (os/execute ["/bin/sh" "-c"
  "./build/janet tools/callgraph.janet /tmp/cg-guard.janet > /tmp/cg-guard-out.txt"] :p))
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
  "./build/janet tools/callgraph.janet /tmp/cg-ret.janet > /tmp/cg-ret-out.txt"] :p))
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
  "./build/janet tools/callgraph.janet /tmp/cg-hybrid.janet > /tmp/cg-hybrid-out.txt"] :p))
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
  "rm -rf /tmp/cg-em && ./build/janet tools/callgraph.janet /tmp/cg-hybrid.janet --emit-native /tmp/cg-em"] :p))
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
  "rm -rf /tmp/cg-br && ./build/janet tools/callgraph.janet /tmp/cg-branch.janet --emit-native /tmp/cg-br"] :p))
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
  "rm -rf /tmp/cg-bc && ./build/janet tools/callgraph.janet /tmp/cg-badcond.janet --emit-native /tmp/cg-bc > /tmp/cg-bc-out.txt"])
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
  "rm -rf /tmp/cg-lp && ./build/janet tools/callgraph.janet /tmp/cg-loop.janet --emit-native /tmp/cg-lp"] :p))
  "loop emit run")
(assert (= 0 (os/execute ["/bin/sh" "-c"
  "cc -O2 /tmp/cg-lp/native.c -o /tmp/cg-lp/native"] :p)) "loop native compiles")
(assert (= 0 (os/execute ["/bin/sh" "-c"
  "/tmp/cg-lp/native > /tmp/cg-lp/actual.txt && cmp /tmp/cg-lp/expected.txt /tmp/cg-lp/actual.txt"] :p))
  "loop parity")
(def ltext (slurp "/tmp/cg-lp/native.c"))
(assert (not (nil? (string/find "goto L" ltext))) "loops emit back-edge goto")
(assert (not (nil? (string/find "s1 = s1 + s2;" ltext))) "loop-carried slots reassign")

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
  "rm -rf /tmp/cg-ex && ./build/janet tools/callgraph.janet /tmp/cg-ext.janet --externs /tmp/cg-ext-decls.janet --emit-native /tmp/cg-ex"] :p))
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
  "rm -rf /tmp/cg-w && ./build/janet tools/callgraph.janet /tmp/cg-width.janet --emit-native /tmp/cg-w"] :p))
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
  "rm -rf /tmp/cg-arr && ./build/janet tools/callgraph.janet /tmp/cg-arr.janet --emit-native /tmp/cg-arr"] :p))
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
  "rm -rf /tmp/cg-arrbad && ./build/janet tools/callgraph.janet /tmp/cg-arrbad.janet --emit-native /tmp/cg-arrbad >/tmp/cg-arrbad.log 2>&1"] :p)
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
  "rm -rf /tmp/cg-pr && ./build/janet tools/callgraph.janet /tmp/cg-promote.janet --int-policy promote --emit-native /tmp/cg-pr"] :p))
  "promote emit run")
(def prtext (slurp "/tmp/cg-pr/native.c"))
(assert (not (nil? (string/find "double padd(double p0, double p1)" prtext)))
        "promote maps int kernel to double")
(assert (= 0 (os/execute ["/bin/sh" "-c"
  "cc -O2 -w /tmp/cg-pr/native.c -o /tmp/cg-pr/native && /tmp/cg-pr/native > /tmp/cg-pr/actual.txt && cmp /tmp/cg-pr/expected.txt /tmp/cg-pr/actual.txt"] :p))
  "promote parity")
# wrap (default): -fwrapv gives defined two's-complement wrap
(assert (= 0 (os/execute ["/bin/sh" "-c"
  "rm -rf /tmp/cg-wr && ./build/janet tools/callgraph.janet /tmp/cg-promote.janet --emit-native /tmp/cg-wr && cc -O2 -w -fwrapv /tmp/cg-wr/native.c -o /tmp/cg-wr/native && /tmp/cg-wr/native > /tmp/cg-wr/actual.txt && cmp /tmp/cg-wr/expected.txt /tmp/cg-wr/actual.txt"] :p))
  "wrap parity under -fwrapv")
(def wrtext (slurp "/tmp/cg-wr/native.c"))
(assert (not (nil? (string/find "int64_t padd(int64_t p0, int64_t p1)" wrtext)))
        "wrap keeps int64 kernel")
# bad policy value is rejected
(assert (= 4 (os/execute ["/bin/sh" "-c"
  "./build/janet tools/callgraph.janet /tmp/cg-promote.janet --int-policy explode --emit-native /tmp/cg-bad >/dev/null 2>&1"] :p))
  "bad int-policy rejected")
# DCE: unreachable hinted defns never reach the output
(spit "/tmp/cg-dce.janet" (string
  "(defn used {:hint {:params [:long] :returns :long}} [x] (+ x 1))\n"
  "(defn dead {:hint {:params [:long] :returns :long}} [x] (* x 2))\n"
  "(defn main [] (print (used 41)))\n"))
(assert (= 0 (os/execute ["/bin/sh" "-c"
  "rm -rf /tmp/cg-dce && ./build/janet tools/callgraph.janet /tmp/cg-dce.janet --emit-native /tmp/cg-dce >/dev/null 2>&1"] :p))
  "dce emit run")
(def dcetext (slurp "/tmp/cg-dce/native.c"))
(assert (not (nil? (string/find "used(" dcetext))) "reachable kernel emitted")
(assert (nil? (string/find "dead" dcetext)) "unreachable kernel eliminated")

(end-suite)
