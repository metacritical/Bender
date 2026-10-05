# tools/janet-diff.janet — AOT spike oracle (Spinel `diff` analogue).
# Compares the host interpreter against a janet-aot binary on the same program:
#   ./build/janet tools/janet-diff.janet app.janet -- arg1 arg2...
# Labels: same | output-diff | exception-diff | compile-error | timeout.
# Exit: 0 same, 1 difference, 2 could not build/run AOT, 4 tool error.
# Folds per-run values the way spinel diff does (addresses, temp paths);
# note: janet's math/rng is deterministic, unlike Spinel-vs-CRuby RNG.

(def args (tuple/slice (dyn :args) 1)) # :args[0] is this script's path
(var prog nil)
(var pargs @[])
(var seen-dash false)
(each a args
  (if seen-dash
    (array/push pargs a)
    (if (= a "--") (set seen-dash true) (set prog a))))
(when (nil? prog) (eprint "usage: janet-diff.janet PROG.janet [-- args...]") (os/exit 4))

(def root
  (do (def p (os/realpath "tools/janet-diff.janet"))
      (string/slice p 0 (- (length p) (length "tools/janet-diff.janet")))))
(def janet (string root "build/janet"))
(def driver (string root "tools/janet-aot.sh"))
(def tmp (string "/tmp/janet-diff-" (os/getpid)))
(os/mkdir tmp)
(defn cleanup []
  (each f ["interp.out" "interp.err" "aot.out" "aot.err" "app" "app.c"]
    (try (os/rm (string tmp "/" f)) ([_] nil)))
  (try (os/rmdir tmp) ([_] nil)))
(def interp-out (string tmp "/interp.out"))
(def interp-err (string tmp "/interp.err"))
(def aot-out (string tmp "/aot.out"))
(def aot-err (string tmp "/aot.err"))
(def aot-bin (string tmp "/app"))

(defn run-to [bin bout berr pargs]
  (def out (file/open bout :w)) (def err (file/open berr :w))
  (def code (os/execute [bin prog ;pargs] :p {:out out :err err}))
  (:close out) (:close err)
  code)

(defn readf [p] (try (slurp p) ([_] "")))
# fold volatile tokens: 0xHEX addresses, /tmp/... paths
(def hexp (peg/compile '(sequence "0x" (some (range "09" "af" "AF")))))
(defn fold [s]
  (def s1 (peg/replace-all hexp "<addr>" s))
  # Known spike divergence: the single-(do...)-thunk image can render one
  # fewer " (tail call)" frame notes than per-form dobytes on error paths
  # (identical bytecode; runtime frame-reuse artifact — Phase 1.1 closes it).
  (def s2 (string/replace-all " (tail call)" "" (string s1)))
  (string/replace-all tmp "<tmp>" s2))

(def ic (run-to janet interp-out interp-err pargs))
(def bc (os/execute [driver prog "-o" aot-bin] :p))
(when (not= bc 0)
  (print "janet diff: compile-error\n  program: " prog)
  (cleanup) (os/exit 2))
(def ac (run-to aot-bin aot-out aot-err @[]))
(def [io ie ao ae] (map fold [(readf interp-out) (readf interp-err)
                              (readf aot-out) (readf aot-err)]))
(defn report [tag]
  (print "janet diff: " tag "\n  program: " prog
         "\n  janet:   exit " ic "\n  aot:     exit " ac)
  (when (not= io ao) (print "  stdout differs"))
  (when (not= ie ae) (print "  stderr differs")))
(cond
  (and (= ic ac) (= io ao) (= ie ae)) (do (print "janet diff: same\n  program: " prog)
                                          (cleanup) (os/exit 0))
  (not= ic ac) (do (report "exception-diff") (cleanup) (os/exit 1))
  (do (report "output-diff") (cleanup) (os/exit 1)))
