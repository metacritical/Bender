# Regression: :string param support in the native kernel emitter.
# See docs/internals/blueant-type-generator-plan.md Phase 2.
#
# JanetString is already `typedef const uint8_t *` with a length-
# prefixed JanetStringHead immediately before the pointer -- no
# representation transformation, only a C type-map entry + a content
# (not pointer) equality kernel (janet_string_equal), since Janet
# strings are not interned (two distinct string objects can hold
# equal bytes; raw `==` would be wrong).
#
# Known, honestly-scoped boundary (not fixed by this phase): a
# literal string constant used in a comparison needs its own `ldc`
# temp slot, and Janet's register allocator commonly reuses that same
# slot afterward for a differently-typed value once the literal's
# live range ends (e.g. the branch-merge result of
# `(if (= name "linear") 1.0 0.0)`). One Janet slot -> one C
# declaration cannot represent two different live types in the same
# slot, so this phase REFUSES that shape (a real, tested safety
# property -- see the negative case below) rather than emitting a
# broken declaration. Closing it needs per-live-range slot splitting,
# a bigger follow-up, not attempted here. Param-to-param string
# comparison (no literal constant, hence no extra temp slot) is
# unaffected and is this phase's positive, proven capability.
(import ./helper :prefix "" :exit true)
(start-suite)
(def root "/tmp/janet-aot-native-string-suite")
(os/execute ["/bin/rm" "-rf" root] :p)
(os/mkdir root)
(def env (merge @{} (os/environ) {"JANET_NO_PROFILE" "1"}))

(defn build-and-run [name source]
  (def src (string root "/" name ".janet"))
  (def bin (string root "/" name))
  (spit src source)
  (def build-ok (= 0 (os/execute ["./build/bender" "--aot" src "--native"
                                  "--no-auto-pgo" "-o" bin] :e env)))
  (def run-ok (and build-ok (= 0 (os/execute [bin] :e env))))
  (def interp-ok (= 0 (os/execute ["./build/bender" src] :e env)))
  {:build build-ok :run run-ok :interp interp-ok
   :names (if build-ok (string (slurp (string bin "-native/native-names.txt"))) "")})

# --- Positive: param-to-param string equality, multi-byte/UTF-8
# content, drives a numeric branch merge, no literal constant temp
# slot involved (no slot-reuse risk) -- must compile native.
(def r1 (build-and-run "param-eq" `
(defn- eq-count [a s1 s2 s3 s4]
  (put a 0 (+ (if (= s1 s2) 1 0) (if (= s3 s4) 1 0)))
  nil)
(def a @[0])
(eq-count a "hello" "hello" "héllo-日本語" "different")
(assert (= 1 (a 0)) "exactly one of the two pairs matched")
(eq-count a "日本語テスト" "日本語テスト" "x" "x")
(assert (= 2 (a 0)) "both multi-byte UTF-8 pairs matched")
(eq-count a "a" "b" "c" "d")
(assert (= 0 (a 0)) "neither pair matched")
`))
(assert (r1 :build) "param-to-param string-eq fn build succeeds")
(assert (string/find "eq-count" (r1 :names))
        "param-to-param string-eq fn got a native wrapper (the positive case)")
(assert (r1 :run) "native binary's own assertions pass (byte-correct string equality)")
(assert (r1 :interp) "same source passes identically under the interpreter")

# --- Negative (safety, as important as the positive case): a slot
# reused for a string literal temp AND a differently-typed later
# value must be REFUSED, never silently miscompiled into an invalid
# or incorrect C declaration. Correctness here is "falls back to
# boxed and still produces the right answer", not "goes native".
(def r2 (build-and-run "literal-reuse" `
(defn- flag-for [name]
  (if (= name "linear") 1.0 0.0))
(assert (= 1.0 (flag-for "linear")) "literal-compare branch, true case")
(assert (= 0.0 (flag-for "other")) "literal-compare branch, false case")
`))
(assert (r2 :build) "literal-constant-comparison fn build still succeeds (falls back to boxed, not a hard error)")
(assert (not (string/find "flag-for" (r2 :names)))
        "slot-reuse-with-literal case correctly stays boxed (not silently miscompiled)")
(assert (r2 :run) "boxed fallback still produces correct output for the literal-compare case")
(assert (r2 :interp) "same source passes identically under the interpreter")

(end-suite)
