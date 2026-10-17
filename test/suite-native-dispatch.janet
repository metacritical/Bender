# Phase 4 (type-generator plan): switch/dispatch codegen for genuinely
# polymorphic ("flip") slots. See docs/internals/blueant-type-generator-plan.md
# Phase 4 and the dispatcher rationale in tools/callgraph.janet.
#
# A param slot whose PGO-observed type set is a CLOSED, small set of
# scalar atom types (>= 2 observed, each with a concrete C rep from
# Phases 1-3) gets a runtime dispatcher: one specialized kernel per
# observed type + a MANDATORY boxed fallback for anything else. The
# single most important property in this suite is the OUT-OF-SAMPLE
# FALLBACK: after training only on numbers and :keyword calls, a
# never-trained :string argument at runtime must take the default arm,
# fall back to the fully boxed interpreter definition, and produce the
# CORRECT answer -- never a crash, never wrong output.
(import ./helper :prefix "" :exit true)
(start-suite)

(def root "/tmp/janet-aot-native-dispatch-suite")
(os/execute ["/bin/rm" "-rf" root] :p)
(os/mkdir root)
(os/mkdir (string root "/cache"))
# Isolated cache for every subprocess so suite runs never touch (or
# read) the developer's real PGO cache. The --pgo build needs
# collection ON, so JANET_NO_PROFILE is deliberately NOT set here.
(def run-env (merge @{} (os/environ)
                    {"XDG_CACHE_HOME" (string root "/cache")}))

# One source, three run modes:
#   train -- only number/keyword flip args (the PGO workload)
#   oos   -- only the never-trained :string flip arg (the critical
#            out-of-sample fallback test)
#   all   -- every type, mixed native hits and fallbacks in one run
# bad-obs is deliberately trained with an :array flip arg (a type with
# no dispatch arm in this phase's scalar-atom scope) -- its observed
# set is NOT closed over dispatchable types, so it must stay boxed.
(def app-src (string
  "(defn flip-scale [x y] (* y y))\n"
  "(defn flip-cmp [x k] (if (= x k) 10.0 20.0))\n"
  "(defn bad-obs [x y] (* y y))\n"
  "(defn main [& args]\n"
  "  (def mode (if (empty? args) \"all\" (in args (dec (length args)))))\n"
  "  (var acc 0)\n"
  "  (case mode\n"
  "    \"train\"\n"
  "    (do (for i 0 100 (do (+= acc (flip-scale i 3)) (+= acc (flip-scale :kw 4))))\n"
  "        (+= acc (flip-cmp :big :big))\n"
  "        (+= acc (flip-cmp 5 :big))\n"
  "        (+= acc (bad-obs @[1] 2))\n"
  "        (+= acc (bad-obs :kw 2)))\n"
  "    \"oos\"\n"
  "    (do (+= acc (flip-scale \"str\" 3))\n"
  "        (+= acc (flip-cmp \"str\" :big)))\n"
  "    (do (+= acc (flip-scale 7 3))\n"
  "        (+= acc (flip-scale :kw 4))\n"
  "        (+= acc (flip-scale \"str\" 3))\n"
  "        (+= acc (flip-cmp :big :big))\n"
  "        (+= acc (flip-cmp 5 :big))\n"
  "        (+= acc (flip-cmp \"str\" :big))\n"
  "        (+= acc (bad-obs @[1] 2))\n"
  "        (+= acc (bad-obs :kw 2))))\n"
  "  (print \"acc=\" acc))\n"))
(def src (string root "/app.janet"))
(def bin (string root "/app"))
(spit src app-src)

(defn acc-of [path]
  # parse "acc=N" out of a captured stdout file
  (def s (slurp path))
  (def i (string/find "acc=" s))
  (when (nil? i) (error (string "no acc= in output: " s)))
  (parse (string/slice s (+ i 4))))

(defn run-capture [outfile & cmd]
  (def f (file/open outfile :w))
  (def code (os/execute cmd :ep (merge @{} run-env {:out f})))
  (file/close f)
  {:code code :acc (if (and (= 0 code) (string/find "acc=" (slurp outfile)))
                     (acc-of outfile) nil)})

# Train + gated build: --pgo "train" collects the profile, then builds
# through the normal parity gate. The binary main receives exactly
# argv[1..]; the interpreter main receives (script-path, argv...) --
# the source handles both by reading the LAST argument.
(def train (run-capture (string root "/train-build.out")
                        "./build/bender" "--aot" src "--native"
                        "--pgo" "train" "-o" bin))
(assert (= 0 (train :code)) "training --pgo build passes its parity gate")
(def i-train (run-capture (string root "/i-train.out") "./build/bender" src "train"))
(assert (= 0 (i-train :code)) "interpreter train mode exits clean")
(assert (not (nil? (os/stat bin))) "gated binary exists")

(def names (slurp (string bin "-native/native-names.txt")))
(def dispf (string bin "-native/native-dispatch.txt"))
(assert (string/find "flip-scale" names) "flip-scale compiled (dispatcher registered under its name)")
(assert (string/find "flip-cmp" names) "flip-cmp compiled (dispatcher registered under its name)")
(assert (not (nil? (os/stat dispf))) "native-dispatch.txt exists")
(assert (string/find "flip-scale" (slurp dispf)) "flip-scale is a dispatch function")
(assert (string/find "flip-cmp" (slurp dispf)) "flip-cmp is a dispatch function")
(assert (not (string/find "bad-obs" names))
        "non-dispatchable observed type (:array) correctly stays boxed")

(def native-c (slurp (string bin "-native/native.c")))
(assert (string/find "switch (janet_type(argv[0]))" native-c)
        "generated C switches on the flip slot's runtime type")
(assert (string/find "janet_call(janet_unwrap_function(fb)" native-c)
        "default arm falls back to the boxed definition via janet_call")

# Trained types: every call dispatches natively, answers exact.
(def b-train (run-capture (string root "/b-train.out") bin "train"))
(assert (= 0 (b-train :code)) "binary train-mode exits clean")
(assert (= (i-train :acc) (b-train :acc))
        "train mode: dispatched native calls give the interpreter's exact answer")

# THE critical test -- out-of-sample :string flip argument at runtime,
# a type never seen in training: must fall back to boxed and still be
# CORRECT (not a crash, not silent wrong output).
(def i-oos (run-capture (string root "/i-oos.out") "./build/bender" src "oos"))
(def b-oos (run-capture (string root "/b-oos.out") bin "oos"))
(assert (= 0 (i-oos :code)) "interpreter oos mode exits clean")
(assert (= 0 (b-oos :code))
        "OUT-OF-SAMPLE: binary with never-trained :string flip arg exits clean (no crash)")
(assert (= (i-oos :acc) (b-oos :acc))
        "OUT-OF-SAMPLE: boxed fallback produces the CORRECT answer")

# Mixed: native arms and fallback interleaved in one process.
(def i-all (run-capture (string root "/i-all.out") "./build/bender" src "all"))
(def b-all (run-capture (string root "/b-all.out") bin "all"))
(assert (= 0 (b-all :code)) "binary all-mode exits clean")
(assert (= (i-all :acc) (b-all :acc))
        "mixed native/fallback run gives the interpreter's exact answer")

# Control: the SAME source built with --no-auto-pgo (no profile at all)
# must not dispatch anything -- the tier only adds candidates on closed
# profile evidence, never invents them.
(def plain-src (string root "/plain.janet"))
(spit plain-src app-src)
(def plain (run-capture (string root "/plain.out")
                        "./build/bender" "--aot" plain-src "--native"
                        "--no-auto-pgo" "-o" (string root "/plain")))
(assert (= 0 (plain :code)) "no-profile control build succeeds")
(assert (not (string/find "flip-scale" (slurp (string root "/plain-native/native-names.txt"))))
        "without PGO evidence the flip functions stay boxed (no invented dispatch)")

# C-side value sampling (Phase 4B evidence stream): a helper script
# trains keywords and strings through profile-start/stop and dumps the
# profile; :vals must record the closed deduped value set per slot and
# mark a >8-distinct slot :open.
(def vals-src (string root "/vals.janet"))
(spit vals-src (string
  "(debug/profile-start)\n"
  "(defn vk [x] (if (keyword? x) 1 2))\n"
  "(each v [:a :b :c :a] (vk v))\n"
  "(defn vs [s] (length s))\n"
  "(each s [\"aa\" \"bb\" \"aa\"] (vs s))\n"
  "(defn vopen [k] (if (keyword? k) 1 2))\n"
  "(each k [:p :q :r :s :t :u :v :w :x :y :z] (vopen k))\n"
  "(debug/profile-dump \"" root "/vals-dump.jdn\")\n"))
(assert (= 0 (os/execute ["./build/bender" vals-src] :ep run-env))
        "value-sampling collection script runs clean")
(def dump (parse (slurp (string root "/vals-dump.jdn"))))
(defn vals-row [nm]
  (var out nil)
  (each row dump
    (when (= nm (get (row 0) 2)) (set out (row 1)))
    # last matching row wins, same as the tool-side merge
    )
  out)
(assert (deep= @[:a :b :c] (get (get (vals-row "vk") :vals) 0))
        "keyword slot records its closed deduped value set")
(assert (deep= @["aa" "bb"] (get (get (vals-row "vs") :vals) 0))
        "string slot records its closed deduped value set")
(assert (= :open (get (get (vals-row "vopen") :vals) 0))
        "over-cap slot (>8 distinct values) is marked :open, never a huge set")

(end-suite)
