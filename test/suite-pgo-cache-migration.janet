# Regression: promoting a v2 automatic-PGO cache into the v3 manifest.
#
# The v3 format stores an independently replayable oracle per workload.
# v2 records have no oracle; the collection gate regenerates one from the
# recorded argv/stdin, but the promoted manifest used to keep the record
# oracle-less. The next build then refused the whole cache as "corrupt
# compatible v3", permanently stranding it.
#
# The fixed store attaches the regenerated oracle. A manifest already
# poisoned by the old writer is repaired by the next explicit --pgo run
# (the collection load admits oracle-less records; the automatic path,
# which replays oracles, stays strict).
(import ./helper :prefix "" :exit true)
(start-suite)
(def root "/tmp/janet-pgo-cache-migration")
(os/execute ["/bin/rm" "-rf" root] :p)
(os/mkdir root)
(def app (string root "/app.janet"))
(def out (string root "/cachetest"))
(def cache (string root "/xdg"))
(def pgo-dir (string cache "/janet/pgo"))
(os/mkdir cache)
(os/mkdir (string cache "/janet"))
(os/mkdir pgo-dir)
(def stdin-text (string root "/empty.stdin"))
(spit stdin-text "")

(spit app `(print "cache-migration-ok")
(print (string/join (map |(string $0) (range 1 6)) ","))
`)

# A real profile dump is a row array; make one with the collector rather
# than inventing a format the merge tool might reject.
(def legacy-profile (string root "/legacy.jdn"))
(def seed-env @{"PATH" (or (os/getenv "PATH") "/usr/bin:/bin")
                 "HOME" (or (os/getenv "HOME") "/tmp")
                 "XDG_CACHE_HOME" cache
                 "JANET_PROFILE" "1"
                 "JANET_PROFILE_OUT" legacy-profile})
(def seed-out (string root "/seed.out"))
(def seed-fh (file/open seed-out :w))
(put seed-env :out seed-fh)
(assert (= 0 (os/execute ["./build/bender" app "legacymode"] :e seed-env))
        "v2 profile collection run succeeds")
(file/close seed-fh)
(assert (os/stat legacy-profile)
        "collector wrote a profile for the legacy record")

# Seed a v2 cache keyed by the output basename ("cachetest").
(def key "cachetest")
(def base (string pgo-dir "/" key))
(spit (string base ".workload") "legacymode")
(spit (string base ".stdin") "")
(spit (string base ".jdn") (slurp legacy-profile))
(def sources @[[(os/realpath app) (slurp app)]])
(spit (string base ".meta")
      (string/format "%j" {:version 2
                           :args "legacymode"
                           :has-stdin false
                           :stdin-bytes nil
                           :sources sources}))

(def build-env (merge @{} (os/environ)
                      {"XDG_CACHE_HOME" cache
                       "JANET_NO_PROFILE" "1"}))
(defn build [tag & flags]
  (def log (string root "/" tag ".log"))
  (def log-fh (file/open log :w))
  (def env (merge @{} build-env {:out log-fh :err log-fh}))
  (def code (os/execute ["./build/bender" "--aot" app "--native" ;flags
                         "-o" out] :e env))
  (file/close log-fh)
  [code (string (slurp log))])

# First build promotes the v2 record into a v3 manifest.
(def [code1 log1] (build "pgo1" "--pgo" "legacymode"))
(assert (= 0 code1) (string "promoting build succeeds: " log1))
(def meta1 (parse (slurp (string base ".meta"))))
(assert (= 3 (get meta1 :version)) "promoted manifest is v3")
(assert (= 1 (length (get meta1 :workloads))) "one promoted workload")
(def rec1 ((get meta1 :workloads) 0))
(assert (and (string? (get rec1 :oracle))
             (= :file (os/stat (get rec1 :oracle) :mode)))
        "promoted record carries a persisted oracle")

# Reproduce the output of the old writer: a v3 manifest whose record has
# no oracle. A subsequent --pgo build must repair it, not refuse forever.
(def poisoned (parse (slurp (string base ".meta"))))
(def poisoned-records
  (map (fn [r]
         (def t @{})
         (eachp [k v] r (put t k v))
         (put t :oracle nil)
         t)
       (get poisoned :workloads)))
(def poisoned-meta @{})
(eachp [k v] poisoned (put poisoned-meta k v))
(put poisoned-meta :workloads poisoned-records)
(spit (string base ".meta") (string/format "%j" poisoned-meta))
(def [code2 log2] (build "pgo2" "--pgo" "legacymode"))
(assert (= 0 code2)
        (string "oracle-less v3 cache is repaired, not refused: " log2))
(assert (string/find "pgo parity ok" log2) "repaired build passed the gate")
(def meta2 (parse (slurp (string base ".meta"))))
(def rec2 ((get meta2 :workloads) 0))
(assert (and (string? (get rec2 :oracle))
             (= :file (os/stat (get rec2 :oracle) :mode)))
        "repair persisted the regenerated oracle")

# The automatic path is strict and must now verify the repaired record.
(def [code3 log3] (build "release" "--release"))
(assert (= 0 code3) (string "release build succeeds: " log3))
(assert (string/find "verifying 1 cached workload" log3)
        "release applies the repaired cache as a gated workload")
(assert (string/find "pgo parity ok" log3) "release replay passed parity")

# The native result still matches the interpreter byte for byte.
(def native-out (string root "/native.out"))
(def native-fh (file/open native-out :w))
(def run-env (merge @{} (os/environ) {"JANET_NO_PROFILE" "1"}))
(put run-env :out native-fh)
(assert (= 0 (os/execute [out "legacymode"] :e run-env))
        "native binary runs")
(file/close native-fh)
(assert (= (string (slurp seed-out)) (string (slurp native-out)))
        "native workload output matches the interpreter oracle")
(end-suite)
