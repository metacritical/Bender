# Joint tag dispatch: only observed full vectors become guarded kernels.
(import ./helper :prefix "" :exit true)
(start-suite)
(def root (string (or (os/getenv "JANET_AOT_TEST_ROOT")
                      (os/getenv "XDG_CACHE_HOME")
                      (os/getenv "TMPDIR") "/tmp")
                  "/janet-joint-suite-" (os/getpid)))
(assert (nil? (os/stat root)) "unique scratch root")
(os/mkdir root)
(def cache (string root "/cache"))
(def env (merge @{} (os/environ) {"XDG_CACHE_HOME" cache "JANET_NO_PROFILE" "1"}))
(def src (string root "/app.janet"))
(def bin (string root "/app"))
(spit src (string
  "(defn joint [a b c] (* c 2.0))\n"
  "(defn one-slot [x y] (* y y))\n"
  "(var state 0)\n"
  "(defn stateful [a b c] (if (even? state) (* c 2.0) (+ c 3.0)))\n"
  "(defn many [a b c d e f g h i] (* i 2.0))\n"
  "(defn array-control [a b c] (if (array? a) (get a 0) nil))\n"
  "(defn main [& args]\n"
  " (def mode (if (empty? args) \"all\" (in args (dec (length args)))))\n"
  " (case mode\n"
  "  \"train\" (do (for i 0 20 (do (joint i \"s\" 3.5) (joint \"s\" i 4.5)))\n"
  "                 (one-slot 1 3) (one-slot :kw 4)\n"
  "                 (stateful 1 \"s\" 3.5) (stateful \"s\" 1 4.5)\n"
  "                 (many 1 \"s\" 1 1 1 1 1 1 2.0))\n"
  "  \"arity\" (apply joint [1 \"s\"])\n"
  "  \"all\" (do\n"
  "   (print (joint 3 \"s\" 3.5))\n"
  "   (print (joint \"s\" 4 4.5))\n"
  "   (print (joint 3 4 5.5))\n"
  "   (print (joint :kw 4 2.5))\n"
  "   (print (joint 3 \"s\" 0.25))\n"
  "   (print (joint \"s\" 4 2147483648.5))\n"
  "   (print (apply one-slot @[1 1.5]))\n"
  "   (print (apply one-slot @[:kw 1e20]))\n"
  "   (print (apply one-slot @[1 2147483648.0]))\n"
  "   (print (apply one-slot @[:kw -2147483649.0]))\n"
  "   (print (apply one-slot @[1 math/inf]))\n"
  "   (print (apply one-slot @[:kw math/nan]))\n"
  "   (set state 1) (print (stateful 1 \"s\" 3.5))\n"
  "   (print (stateful \"s\" 1 4.5))\n"
  "   (print (array-control @[\"changed\"] 0 1))\n"
  "   (print (many 1 \"s\" 1 1 1 1 1 1 2.0)))))\n"))
(defn capture [name & cmd]
  (def out (string root "/" name ".out"))
  (def err (string root "/" name ".err"))
  (def fo (file/open out :w)) (def fe (file/open err :w))
  (def status (os/execute cmd :ep (merge @{} env {:out fo :err fe})))
  (file/close fo) (file/close fe)
  [status (slurp out) (slurp err)])
(def train (capture "build" "./build/bender" "--aot" src "--native"
                    "--pgo" "train" "-o" bin))
(assert (= 0 (train 0)) (string "joint training build: " (train 2)))
(def c (slurp (string bin "-native/native.c")))
(assert (string/find "joint_x25joint_" c) "unique joint C candidates")
(assert (string/find "janet_type(argv[0]) == JANET_NUMBER && janet_type(argv[1]) == JANET_STRING && janet_type(argv[2]) == JANET_NUMBER" c)
        "full-vector guard before unbox")
(assert (string/find "janet_type(argv[0]) == JANET_STRING && janet_type(argv[1]) == JANET_NUMBER && janet_type(argv[2]) == JANET_NUMBER" c)
        "second full-vector guard")
(assert (string/find "isfinite(janet_unwrap_number(argv[1]))" c)
        "existing one-slot integer branch checks finite/range before unbox")
(assert (string/find "janet_unwrap_number(argv[1]) >= -2147483648" c)
        "signed int32 lower limit precedes janet_unwrap_integer")
(assert (string/find "janet_unwrap_number(argv[1]) <= 2147483647" c)
        "signed int32 upper limit precedes janet_unwrap_integer")
(def native-names (slurp (string bin "-native/native-names.txt")))
(assert (not (string/find "stateful" native-names))
        "hidden-state branch without static native proof stays boxed")
(assert (not (string/find "array-control" native-names))
        "untrained mutable array elements never seed a variant")
(def oracle (capture "oracle" "./build/bender" src "all"))
(def actual (capture "actual" bin "all"))
(assert (= 0 (oracle 0)) "boxed oracle")
(assert (= 0 (actual 0)) (string "native run: " (actual 2)))
(assert (deep= (oracle 1) (actual 1)) "held-out/mixed/state/array byte parity")
(assert (string/find "variant-0=2 variant-1=2" (actual 2)) "per-arm telemetry")
(assert (string/find "boxed-fallbacks=2" (actual 2)) "untrained signatures boxed")
(assert (string/find "name=one_slot calls=0 boxed-fallbacks=6" (actual 2))
        "fractional, int32 overflow, infinite and NaN inputs use boxed fallback")
(def plain-src (string root "/plain.janet"))
(spit plain-src (slurp src))
(assert (= 0 ((capture "plain-build" "./build/bender" "--aot" plain-src
                       "--native" "--no-auto-pgo" "-o" (string root "/plain")) 0))
        "profile-free control build")
(assert (not (string/find "joint" (slurp (string root "/plain-native/native-names.txt"))))
        "no histogram means no joint candidates")
(def wrong (capture "arity" bin "arity"))
(assert (not= 0 (wrong 0)) "incorrect arity cannot enter a variant")
(def dump-src (string root "/dump.janet"))
(spit dump-src (string
  "(debug/profile-start)\n"
  "(defn observed [a b c] (* c 2.0))\n"
  "(observed 1 \"s\" 2) (observed \"s\" 1 2)\n"
  "(defn over [x] x)\n"
  "(each x [1 \"s\" :k 'sym true nil @[1] (tuple 1) @{:k 1}] (over x))\n"
  "(defn wide [a b c d e f g h i] i)\n"
  "(wide 1 2 3 4 5 6 7 8 9)\n"
  "(debug/profile-dump \"" root "/dump.jdn\")\n"))
(assert (= 0 ((capture "dump" "./build/bender" dump-src) 0)) "VM dump")
(def rows (parse (slurp (string root "/dump.jdn"))))
(defn row [nm]
  (var found nil)
  (each r rows (when (= nm (get (r 0) 2)) (set found (r 1))))
  found)
(def sigs (get (row "observed") :signatures))
(assert (= 2 (length sigs)) "two joint vectors, no cross product")
(assert (= :open (get (row "wide") :signatures)) ">8 arity opens histogram")
(assert (= :open (get (row "over") :signatures)) ">8 distinct vectors opens histogram")
(def merged (string root "/merged.jdn"))
(assert (= 0 ((capture "merge" "./build/bender" "tools/profile-merge.janet"
                       "--accumulate" "--out" merged
                       (string "one=" root "/dump.jdn")
                       (string "two=" root "/dump.jdn")) 0))
        "accumulated joint histogram")
(def merged-rows (parse (slurp merged)))
(defn merged-row [nm]
  (var found nil)
  (each r merged-rows (when (= nm (get (r 0) 2)) (set found (r 1))))
  found)
(def doubled (get (merged-row "observed") :signatures))
(assert (= 2 (length doubled)) "accumulation preserves only exact vectors")
(assert (= 4 (+ (get (doubled 0) 1) (get (doubled 1) 1)))
        "matching vector counts add")
(assert (= :open (get (merged-row "wide") :signatures)) "open dominates merge")
(assert (= :open (get (merged-row "over") :signatures)) "overflow stays open on merge")
(end-suite)
