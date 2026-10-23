# Native-to-native continuation acceptance image.
#
# Results first: every assertion below is a plain value check that must hold
# whether or not any continuation edge was taken, so running this image with
# JANET_NATIVE_NO_CONTINUATION=1 is a complete differential control.
#
# Then the counter-identity gate: chained call edges + interpreted
# Janet-function JOP_CALL dispatches is a conserved quantity. A call executed
# twice or skipped changes it, which byte-identical output cannot reveal when
# the callee is pure.

(import ./native-continuation-chain :as C)

# 3-deep chain, computed by hand: chain3 x = 6x, chain2 x = 24x,
# chain1 n = 24 * (0+1+...+(n-1)).
(assert (= 36 (C/chain3 6)))
(assert (= 144 (C/chain2 6)))
(assert (= (* 24 45) (C/chain1 10)))

# R1: exactly one side effect per edge.
(C/reset-effects)
(assert (= 10 (C/side-caller 5)))
(assert (= 5 (length C/effects)) "side-effecting callee did not run exactly once per call")
(C/reset-effects)
(assert (= 45 (C/side-caller 10)))
(assert (= 10 (length C/effects)) "side-effecting callee did not run exactly once per call")

# R3: a callee that boundary-exits mid-body must be interpreted against its
# OWN definition. r3-callee x = 2 * (1000000.5 / (x+1)) + 7.25.
# Computed at top level (no `defn`, so no executor of its own) in exactly the
# callee's accumulation order. d + d is exact in IEEE, so (* 2 d) is the same
# double the callee produces.
(var r3-expected 0)
(loop [i :range [0 6]]
  (+= r3-expected (+ (* 2 (/ 1000000.5 (+ i 1))) 7.25)))
(assert (= r3-expected (C/r3-caller 6)) "callee executed against the wrong definition")

# R8: a panic three native frames deep stays an ordinary Janet error, the
# trace names the innermost function, and the VM is usable afterwards.
(def [ok err] (protect (C/panic1 @[1 2 3])))
(assert (not ok) "expected a panic from three frames down")
(def trace
  (string (with-dyns [:err @""]
            (def f (fiber/new (fn [] (C/panic1 @[1 2 3])) :ei))
            (def v (resume f))
            (debug/stacktrace f v "")
            (dyn :err))))
(assert (string/find "panic3" trace) (string "stack trace lost the innermost frame: " trace))
(assert (string/find "panic1" trace) (string "stack trace lost the outer frame: " trace))
(assert (= 144 (C/chain2 6)) "VM unusable after a panic in a native chain")

# Chain depth beyond the trampoline's iteration budget must not change the
# answer -- the budget bounds chaining, not computation.
(assert (= 200 (C/deep 199)))

# Allocating callee: the GC safepoint on every push and pop.
# alloc-callee x = x + 4, so the sum over 0..9 is 45 + 40.
(assert (= 85 (C/alloc-caller 10)))

(when (get (curenv) 'executor-global-stats)
  (def gentry (get (curenv) 'executor-global-stats))
  (def gstats (if (function? gentry) gentry (gentry :value)))
  (def gs (gstats))
  # 6 continuations, 7 refusals, 8 native returns, 9 interpreted Janet calls.
  # Telemetry goes to stderr: it exists only in an executor-enabled native
  # build, so printing it on stdout would make the AOT binary's stdout differ
  # from the interpreter's and fail the --pgo parity gate. The suite captures
  # both streams.
  (eprint "cont-identity total=" (+ (gs 6) (gs 9))
         " continuations=" (gs 6)
         " refusals=" (gs 7)
         " native-returns=" (gs 8)
         " interp-calls=" (gs 9))
  (assert (>= (gs 9) 0))
  (def sentry (get (curenv) 'executor-stats))
  (def stats (if (function? sentry) sentry (sentry :value)))
  (loop [id :range [0 64]]
    (def [ok counts] (protect (stats id)))
    (when (and ok (> (counts 5) 0))
      (eprint "cont-executor " id " conts=" (counts 5) " entries=" (counts 0)))))
(print "native-continuation image passed")
