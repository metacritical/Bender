# Interpreter development runs collect diagnostic-only profiles by default.
(import ./helper :prefix "" :exit true)
(start-suite)

(def root "/tmp/janet-development-profile-suite")
(os/execute ["/bin/rm" "-rf" root] :p)
(os/mkdir root)
(def source (string root "/app.janet"))
(spit source "(defn hot [x] (+ x 1))\n(for i 0 40 (hot i))\n")
(def cache (string root "/cache"))
(def env (merge @{} (os/environ) {"XDG_CACHE_HOME" cache
                                    "JANET_PROFILE" "" "JANET_PROFILE_OUT" ""
                                    "JANET_NO_PROFILE" ""}))
(assert (= 0 (os/execute ["./build/bender" source] :e env))
        "ordinary interpreter run succeeds")
(def dir (string cache "/janet/pgo/interpreter"))
(def files (os/dir dir))
(assert (= 1 (length files)) "default interpreter run produces one diagnostic")
(def rows (parse (slurp (string dir "/" (files 0)))))
(assert (> (length rows) 0) "diagnostic includes observed calls")
(assert (nil? (os/stat (string cache "/janet/pgo/app.jdn")))
        "interpreter diagnostic never masquerades as gateable cache")
(assert (= 0 (os/execute ["./build/bender" source]
                         :e (merge @{} env {"JANET_NO_PROFILE" "1"})))
        "sensitive run succeeds with collection disabled")
(assert (= 1 (length (os/dir dir))) "runtime opt-out writes no diagnostic")
(def explicit (string root "/explicit.jdn"))
(assert (= 0 (os/execute ["./build/bender" source]
                         :e (merge @{} env {"JANET_PROFILE" "1"
                                            "JANET_PROFILE_OUT" explicit})))
        "explicit collector run succeeds")
(assert (os/stat explicit) "explicit collector path remains supported")
(assert (= 1 (length (os/dir dir))) "explicit run does not duplicate diagnostic")
(end-suite)
