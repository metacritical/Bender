# Copyright (c) 2026 Calvin Rose & contributors
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to
# deal in the Software without restriction, including without limitation the
# rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
# sell copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in
# all copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
# FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
# IN THE SOFTWARE.
#
# Auto-PGO loop: first binary runs collect into the standard cache;
# later plain builds widen from it automatically through the parity
# gate. XDG_CACHE_HOME is isolated per run; JANET_AUTO_PGO_MIN_CALLS=0
# keeps small programs deterministic (the 10k default needs slow ones).

(import ./helper :prefix "" :exit true)
(start-suite)

(def tmpcache "/tmp/janet-autopgo-test")

# run argv, capture stdout as a string
(defn capture [argv]
  (def p (os/spawn argv :p {:out :pipe}))
  (def out (:read (p :out) :all))
  (os/proc-wait p)
  (string out))
(os/execute ["/bin/rm" "-rf" tmpcache] :p)
(os/mkdir tmpcache)
# process env: inherited by every os/execute below (env prefixes do
# NOT survive && in sh, so prefixing commands is unreliable)
(os/setenv "XDG_CACHE_HOME" tmpcache)
(def mincalls "JANET_AUTO_PGO_MIN_CALLS=0 ")

# a program with a kernelizable hot fn
(spit "/tmp/apg-demo.janet" (string
  "(defn half {:hint {:params [:long :long] :returns :long}} [a b] (div a b))\n"
  "(var s 0)\n"
  "(for i 0 500 (set s (+ s (half (+ i 10) 3))))\n"
  "(print s)\n"))
(def expect-out (capture ["./build/bender" "/tmp/apg-demo.janet"]))

# 1. first run collects (no cache before)
(assert (= 0 (os/execute ["/bin/sh" "-c"
  "rm -rf /tmp/apg-bin* && ./build/bender --aot /tmp/apg-demo.janet --native -o /tmp/apg-bin >/dev/null 2>&1"] :p))
  "demo builds")
(assert (= expect-out (capture ["/tmp/apg-bin"]))
        "demo output correct")
(assert (= 0 (os/execute ["/bin/sh" "-c" (string mincalls "/tmp/apg-bin >/dev/null 2>&1")] :p))
  "demo runs")
(assert (not (nil? (os/stat (string tmpcache "/janet/pgo/apg-bin.jdn"))))
        "first run collects to cache")

# Parallel first runs publish exactly one base without truncating it.
(os/execute ["/bin/sh" "-c" (string "rm -f " tmpcache "/janet/pgo/apg-bin.jdn")] :p)
(assert (= 0 (os/execute ["/bin/sh" "-c"
  (string mincalls "/tmp/apg-bin >/dev/null 2>&1 & "
          mincalls "/tmp/apg-bin >/dev/null 2>&1 & wait")] :p))
  "parallel first runs complete")
(def concurrent-cache (string tmpcache "/janet/pgo/apg-bin.jdn"))
(assert (not (nil? (os/stat concurrent-cache))) "parallel runs publish base")
(assert (> (length (parse (slurp concurrent-cache))) 0)
        "parallel publication remains parseable")

# 2. second run does not recollect (profile exists)
(def m1 (get (os/stat (string tmpcache "/janet/pgo/apg-bin.jdn")) :modified))
(os/execute ["/bin/sh" "-c" "sleep 1"] :p)
(assert (= 0 (os/execute ["/bin/sh" "-c" (string mincalls "/tmp/apg-bin >/dev/null 2>&1")] :p))
  "demo runs again")
(def m2 (get (os/stat (string tmpcache "/janet/pgo/apg-bin.jdn")) :modified))
(assert (= m1 m2) "second run leaves cache alone")

# 3. JANET_NO_PROFILE disables even forced collection
(os/execute ["/bin/sh" "-c" (string "rm -f " tmpcache "/janet/pgo/apg-bin.jdn")] :p)
(assert (= 0 (os/execute ["/bin/sh" "-c"
  (string "JANET_NO_PROFILE=1 JANET_AUTO_PGO=1 " mincalls "/tmp/apg-bin >/dev/null 2>&1")] :p))
  "demo runs with opt-out")
(assert (nil? (os/stat (string tmpcache "/janet/pgo/apg-bin.jdn")))
        "opt-out writes nothing")

# 4. default threshold: trivial runs don't create cache files
(spit "/tmp/apg-tinysrc.janet" "(print (+ 1 2))\n")
(assert (= 0 (os/execute ["/bin/sh" "-c"
  "rm -rf /tmp/apg-tiny /tmp/apg-tiny-native /tmp/apg-tiny.c && ./build/bender --aot /tmp/apg-tinysrc.janet --native -o /tmp/apg-tiny >/dev/null 2>&1"] :p))
  "tiny builds")
(assert (= 0 (os/execute ["/bin/sh" "-c" "/tmp/apg-tiny >/dev/null 2>&1"] :p))
  "tiny runs")
(assert (nil? (os/stat (string tmpcache "/janet/pgo/apg-tiny.jdn")))
        "trivial run writes nothing by default")

# 5. explicit --pgo stores profile + workload; plain rebuild auto-applies
(assert (= 0 (os/execute ["/bin/sh" "-c" "rm -rf /tmp/apg-pgo* && ./build/bender --aot /tmp/apg-demo.janet --native --pgo \"\" -o /tmp/apg-pgo >/dev/null 2>&1"] :p))
  "pgo build succeeds")
(assert (not (nil? (os/stat (string tmpcache "/janet/pgo/apg-pgo.jdn"))))
        "pgo stores profile")
(assert (not (nil? (os/stat (string tmpcache "/janet/pgo/apg-pgo.workload"))))
        "pgo stores workload")
(assert (= 0 (os/execute ["/bin/sh" "-c" "./build/bender --aot /tmp/apg-demo.janet --native -o /tmp/apg-pgo >/tmp/apg-auto-out.txt 2>&1"] :p))
  "plain rebuild succeeds")
(def auto-log (string (slurp "/tmp/apg-auto-out.txt")))
(assert (not (nil? (string/find "applying cached PGO profile" auto-log)))
        "rebuild applies cache automatically")
(assert (not (nil? (string/find "parity ok" auto-log)))
        "gate passes on apply")
(assert (= expect-out (capture ["/tmp/apg-pgo"]))
        "optimized output correct")

# 6. Forced follow-up binary runs never replace the cached base. The next
# ordinary build folds sidecars into it before applying the parity gate.
(def cache-profile (string tmpcache "/janet/pgo/apg-pgo.jdn"))
(def before (string (slurp cache-profile)))
(assert (= 0 (os/execute ["/bin/sh" "-c"
  (string "JANET_AUTO_PGO=1 " mincalls "/tmp/apg-pgo >/dev/null 2>&1")] :p))
  "forced second run succeeds")
(assert (= before (string (slurp cache-profile))) "forced run preserves cached evidence")
(def sidecars (filter (fn [name] (string/has-prefix? "apg-pgo.jdn.run-" name))
                      (os/dir (string tmpcache "/janet/pgo"))))
(assert (= 1 (length sidecars)) "forced run creates sidecar")
(assert (= 0 (os/execute ["/bin/sh" "-c"
  "./build/bender --aot /tmp/apg-demo.janet --native -o /tmp/apg-pgo >/tmp/apg-merged-out.txt 2>&1"] :p))
  "plain rebuild merges sidecar")
(assert (not (= before (string (slurp cache-profile)))) "cache changes after merge")
(assert (empty? (filter (fn [name] (string/has-prefix? "apg-pgo.jdn.run-" name))
                        (os/dir (string tmpcache "/janet/pgo"))))
        "merged sidecar removed")
(def merged-log (string (slurp "/tmp/apg-merged-out.txt")))
(assert (string/find "parity ok" merged-log) "merged profile passes gate")
(assert (= expect-out (capture ["/tmp/apg-pgo"])) "merged binary output correct")
# Stale cache is never refreshed merely by merging a later sidecar.
(spit "/tmp/apg-stale-source.janet" "(print :old)\n")
(assert (= 0 (os/execute ["/bin/sh" "-c"
  "./build/bender --aot /tmp/apg-stale-source.janet --native -o /tmp/apg-stale >/dev/null 2>&1"] :p))
  "stale fixture builds")
(assert (= 0 (os/execute ["/bin/sh" "-c"
  (string mincalls "/tmp/apg-stale >/dev/null 2>&1")] :p))
  "old fixture profiles")
(def stale-cache (string tmpcache "/janet/pgo/apg-stale.jdn"))
(def stale-before (string (slurp stale-cache)))
(os/execute ["/bin/sh" "-c" "sleep 1"] :p)
(spit "/tmp/apg-stale-source.janet" "(print :new)\n")
(assert (= 0 (os/execute ["/bin/sh" "-c"
  (string "JANET_AUTO_PGO=1 " mincalls "/tmp/apg-stale >/dev/null 2>&1")] :p))
  "old binary creates forced sidecar")
(assert (= 0 (os/execute ["/bin/sh" "-c"
  "./build/bender --aot /tmp/apg-stale-source.janet --native -o /tmp/apg-stale >/tmp/apg-stale-log.txt 2>&1"] :p))
  "stale profile falls back without build failure")
(assert (= stale-before (string (slurp stale-cache))) "stale cache remains unchanged")
(assert (string/find "stale" (string (slurp "/tmp/apg-stale-log.txt")))
        "stale sidecar diagnostic")

# A held per-key transaction lock cannot authorize stale cached evidence.
(def lockdir (string tmpcache "/janet/pgo/apg-pgo.lock"))
(assert (os/mkdir lockdir) "test acquires cache lock")
(assert (= 0 (os/execute ["/bin/sh" "-c"
  "./build/bender --aot /tmp/apg-demo.janet --native -o /tmp/apg-pgo >/tmp/apg-busy-log.txt 2>&1"] :p))
  "busy cache falls back safely")
(assert (string/find "cache busy" (string (slurp "/tmp/apg-busy-log.txt")))
        "busy cache announces fallback")
(os/rmdir lockdir)
(assert (nil? (os/stat lockdir)) "test releases cache lock")

(end-suite)
