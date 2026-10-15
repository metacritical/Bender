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
(assert (not (nil? (os/stat profile))) "normal-return auto-PGO dump exists")
(assert (> (length (parse (slurp profile))) 0) "normal-return dump contains calls")

# os/exit calls janet_deinit directly, bypassing generated main's return.
# It must still publish an independently collected profile without leaking
# its private output setting to the interpreter it spawned.
(def exit-source "/tmp/janet-aot-profile-env-exit-source.janet")
(def exit-binary "/tmp/janet-aot-profile-env-exit-binary")
(spit exit-source (string
  "(defn hot [x] (+ x 1))\n"
  "(for i 0 20 (hot i))\n"
  "(print :exit-parent-ok)\n"
  "(print (os/execute [\"./build/bender\" \"/tmp/janet-aot-profile-env-child.janet\"] :p))\n"
  "(os/exit 0)\n"))
(assert (= 0 (os/execute ["./build/bender" "--aot" exit-source "-o" exit-binary] :p))
        "os/exit fixture AOT builds")
(def exit-command (string "XDG_CACHE_HOME=" cache " JANET_AUTO_PGO_MIN_CALLS=0 "
                          exit-binary " > /tmp/janet-aot-profile-env-exit-output 2>&1"))
(assert (= 0 (os/execute ["/bin/sh" "-c" exit-command] :p)) "os/exit image and child run")
(def exit-output (slurp "/tmp/janet-aot-profile-env-exit-output"))
(assert (string/find "exit-parent-ok" exit-output) "os/exit parent ran")
(assert (string/find "child-ok" exit-output) "os/exit child interpreter ran")
(assert (string/find "\n0\n" exit-output) "os/exit child succeeded")
(assert (nil? (string/find "profiling not active" exit-output)) "os/exit child has no stray dump")
(def exit-profile (string cache "/janet/pgo/janet-aot-profile-env-exit-binary.jdn"))
(assert (not (nil? (os/stat exit-profile))) "os/exit auto-PGO dump exists")
(assert (> (length (parse (slurp exit-profile))) 0) "os/exit dump contains calls")
(end-suite)
