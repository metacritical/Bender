# An AOT launcher's private auto-PGO output must not contaminate child
# interpreters, while its own run still writes a useful cached profile.
(import ./helper :prefix "" :exit true)
(start-suite)

(def cache "/tmp/janet-aot-profile-env-cache")
(def source "/tmp/janet-aot-profile-env-source.janet")
(def binary "/tmp/janet-aot-profile-env-binary")
(def child "/tmp/janet-aot-profile-env-child.janet")
(os/execute ["/bin/rm" "-rf" cache] :p)
(os/mkdir cache)
(spit child "(print :child-ok)\n")
(spit source (string
  "(defn hot [x] (+ x 1))\n"
  "(for i 0 20 (hot i))\n"
  "(print :parent-ok)\n"
  "(print (os/execute [\"./build/bender\" \"/tmp/janet-aot-profile-env-child.janet\"] :p))\n"))
(assert (= 0 (os/execute ["./build/bender" "--aot" source "-o" binary] :p))
        "fixture AOT builds")
# Set the cache before launching the image, not within it: the launcher
# selects its profile destination before executing the embedded program.
(def command (string "XDG_CACHE_HOME=" cache " JANET_AUTO_PGO_MIN_CALLS=0 "
                     binary " > /tmp/janet-aot-profile-env-output 2>&1"))
(assert (= 0 (os/execute ["/bin/sh" "-c" command] :p)) "image and child run")
(def output (slurp "/tmp/janet-aot-profile-env-output"))
(assert (string/find "parent-ok" output) "parent ran")
(assert (string/find "child-ok" output) "child interpreter ran")
(assert (string/find "\n0\n" output) "child succeeded without inherited profile")
(assert (nil? (string/find "profiling not active" output)) "child has no stray dump")
(def profile (string cache "/janet/pgo/janet-aot-profile-env-binary.jdn"))
(assert (not (nil? (os/stat profile))) "parent auto-PGO dump exists")
(assert (> (length (parse (slurp profile))) 0) "parent dump contains calls")
(end-suite)
