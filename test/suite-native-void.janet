# Regression: functions whose whole-program return type resolves to
# :nil must compile to a void native kernel (not stay boxed, not
# crash), for both an explicit (ret nil-value) site and a bare
# (retn, JOP_RETURN_NIL) site, and must be correctly callable both as
# an entry point and as a native-to-native callee. See
# docs/internals/blueant-type-generator-plan.md Phase 1.
#
# Scope note: zero-arity functions are excluded here on purpose --
# emit-native-lib's candidate filter requires (> (length pt) 0)
# regardless of return type (a separate, pre-existing restriction,
# not part of this phase); the bare-retn path is still exercised
# below inside a >0-arity function.
(import ./helper :prefix "" :exit true)
(start-suite)
(def root "/tmp/janet-aot-native-void-suite")
(os/execute ["/bin/rm" "-rf" root] :p)
(os/mkdir root)
(def source (string root "/app.janet"))
(def binary (string root "/app"))
(spit source `
# Bare retn (JOP_RETURN_NIL) on one path, explicit trailing nil
# (regular 'ret of a :nil-typed value) on the other -- both must
# whole-program-resolve this fn's return type to :nil.
(defn- mixed-nil-returns [x]
  (when (> x 0) (break))
  nil)

# Numeric params, explicit trailing nil, pure arithmetic into an
# array slot the caller can observe (array mutation is a direct
# 'puti-style op, not an extern call, so it stays fully native).
(defn- numeric-params-explicit-nil [a x y]
  (put a 0 (+ x y))
  nil)

# Array param (array LITERAL at the call site, not a call to the
# 'array' constructor function -- the literal's bytecode op is what
# the static inference tracks as :array; a call to (array ...) is
# just an unresolved call returning :unknown and is a different,
# unrelated gap).
(defn- array-param-explicit-nil [a i v]
  (put a i (+ v 1))
  nil)

# Native-to-native callee check: a native kernel calling another
# native void kernel (not just the boxed interpreter calling it).
(defn- numeric-only-caller [a x y]
  (numeric-params-explicit-nil a x y)
  (+ x y))

(mixed-nil-returns 1)
(mixed-nil-returns -1)
(assert (= :nil (type (mixed-nil-returns 1))) "mixed retn/ret-nil fn returns nil")

(def a1 @[0 0 0])
(numeric-params-explicit-nil a1 3 4)
(assert (= 7 (a1 0)) "explicit-nil kernel's body executed (array slot mutated)")

(def a2 @[10 20 30])
(array-param-explicit-nil a2 0 41)
(assert (= 42 (a2 0)) "array-param void kernel mutated in place")
(assert (= 20 (a2 1)) "untouched slot unchanged")

(def a3 @[0])
(assert (= 11 (numeric-only-caller a3 5 6)) "native-to-native call into a void kernel")
(assert (= 11 (a3 0)) "callee's void-kernel side effect visible after native-to-native call")
`)
(def env (merge @{} (os/environ) {"JANET_NO_PROFILE" "1"}))
(assert (= 0 (os/execute ["./build/bender" "--aot" source "--native"
                         "--no-auto-pgo" "-o" binary] :e env))
        "void-kernel build succeeds")
(def names (string (slurp (string binary "-native/native-names.txt"))))
(assert (string/find "mixed-nil-returns" names)
        "bare-retn + explicit-ret-nil fn got a native wrapper")
(assert (string/find "numeric-params-explicit-nil" names)
        "explicit-nil numeric-param fn got a native wrapper")
(assert (string/find "array-param-explicit-nil" names)
        "explicit-nil array-param fn got a native wrapper")
(assert (string/find "numeric-only-caller" names)
        "caller of a void kernel got a native wrapper (native-to-native call proven compiled)")
# The spit'd source's own (assert ...) calls panic with a nonzero
# exit on any failure, so a clean exit proves every assertion above
# passed INSIDE the running native binary, not just that it started.
(assert (= 0 (os/execute [binary] :e env)) "native void-kernel binary exits clean")
# Interpreter parity: same source, same assertions, must also pass
# unmodified under the plain interpreter (same file, no --aot).
(assert (= 0 (os/execute ["./build/bender" source] :e env))
        "same source passes identically under the interpreter")
(end-suite)
