# B7.2 Part B, Piece 1: the DCE cross-check harness
# (tools/dce-crosscheck.janet) -- the actual safety net for future
# whole-program dead-code elimination, not the (not-yet-built)
# analysis itself. See docs/internals/b7.2-part-b-design-note.md.
#
# This suite proves the harness can tell a correct stripping decision
# from a wrong one -- the one property that matters before trusting
# it for anything. A harness that can't catch an injected wrong
# answer is worse than useless (it would rubber-stamp a real
# miscompile), so this is tested as a first-class property, not an
# afterthought.
#
# Found and fixed while writing this suite: the harness's own output
# comparison used `=` on two `slurp` results. `slurp` returns a
# mutable buffer, and Janet's `=` compares buffers by IDENTITY, not
# content -- so the harness reported FAIL on two byte-identical
# outputs until fixed to compare `(string a) (string b)` instead. The
# harness's self-test below is exactly what caught this.
(import ./helper :prefix "" :exit true)
(start-suite)

(def root "/tmp/janet-dce-harness-suite")
(os/execute ["/bin/rm" "-rf" root] :p)
(os/mkdir root)
(def env (merge @{} (os/environ) {"JANET_NO_PROFILE" "1"}))

(defn run-harness [fixture-name fixture-source workdir-suffix strip-names]
  (def src (string root "/" fixture-name ".janet"))
  (spit src fixture-source)
  (def workdir (string root "/" workdir-suffix))
  (def result
    (os/execute ["./build/bender" "tools/dce-crosscheck.janet" src workdir
                ;strip-names] :e env))
  result)

# --- Correct case: stripping a genuinely unused function (zero
# remaining textual references anywhere) must PASS -- removing dead
# code cannot change behavior.
(def correct-exit
  (run-harness "fixture1" `
(defn used-fn [x] (* x 2))
(defn unused-fn [x] (* x 99))
(print (used-fn 21))
` "correct" ["unused-fn"]))
(assert (= 0 correct-exit) "harness PASSes when stripping a genuinely unused function")

# --- Wrong case (the one that matters): stripping a function that IS
# still referenced must be caught, not silently accepted. The
# harness's build step itself fails (unknown symbol) -- a real,
# detected failure, not a false PASS. This is the single property
# that makes the harness worth having: it must never rubber-stamp a
# broken stripping decision.
(def wrong-exit
  (run-harness "fixture2" `
(defn used-fn [x] (* x 2))
(print (used-fn 21))
` "wrong" ["used-fn"]))
(assert (not= 0 wrong-exit)
        "harness correctly FAILS (nonzero exit) when stripping a still-used function")

(end-suite)
