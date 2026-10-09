# Phase C (PGO worker profiles): merge tool, provenance reporting,
# worker-seeded proof. Each file runs in its own process.

(import ./helper :prefix "" :exit true)
(start-suite)

# Subprocess capture: [exit-code stdout], after suite-ev's exec-slurp
# (gather reads while waiting, so big outputs cannot deadlock).
(defn- run-out [& args]
  (def proc (os/spawn args :p {:out :pipe :err :out}))
  (def [code data]
    (ev/gather
      (os/proc-wait proc)
      (ev/read (proc :out) :all)))
  [code (string (or data ""))])

(defn- run-ok [& args]
  (def [code _] (run-out ;args))
  (zero? code))

# ---- C2: merge tags provenance, concatenates deterministically ----
(spit "/tmp/pmg-home.jdn"
  (string/format "%j"
    @[@[@["h.janet" 1 "wfn"]
        @{:calls 3 :params @[@[:number]] :mins @[0] :maxs @[9]
          :elems @[] :rets @[:number]}]]))
(spit "/tmp/pmg-work.jdn"
  (string/format "%j"
    @[@[@["h.janet" 1 "wfn"]
        @{:calls 2 :params @[@[:number]] :mins @[10] :maxs @[20]
          :elems @[] :rets @[:number]}]
      @[@["h.janet" 5 "only-worker"]
        @{:calls 1 :params @[@[:string]] :mins @[nil] :maxs @[nil]
          :elems @[] :rets @[:string]}]]))
(assert (run-ok "./build/bender" "tools/profile-merge.janet"
                "--out" "/tmp/pmg-merged.jdn"
                "home=/tmp/pmg-home.jdn" "worker=/tmp/pmg-work.jdn")
        "merge runs")
(def merged (parse (slurp "/tmp/pmg-merged.jdn")))
(assert (= 3 (length merged)) "merge concatenates")
(assert (deep= @["h.janet" 1 "wfn"] ((merged 0) 0)) "order stable")
(assert (= :home (get ((merged 0) 1) :from)) "home tagged")
(assert (= :worker (get ((merged 1) 1) :from)) "worker tagged")
(assert (= :worker (get ((merged 2) 1) :from)) "worker-only tagged")
# home evidence survives the merge untouched (no weakening here).
(assert (= 3 (get ((merged 0) 1) :calls)) "home calls kept")
(assert (deep= @[0] (get ((merged 0) 1) :mins)) "home mins kept")
# bare paths: first=:home, rest=:worker.
(assert (run-ok "./build/bender" "tools/profile-merge.janet"
                "--out" "/tmp/pmg-bare.jdn"
                "/tmp/pmg-home.jdn" "/tmp/pmg-work.jdn")
        "merge bare paths")
(def bare (parse (slurp "/tmp/pmg-bare.jdn")))
(assert (= :home (get ((bare 0) 1) :from)) "bare first is home")
(assert (= :worker (get ((bare 1) 1) :from)) "bare rest is worker")
# bad inputs fail loud (nonzero exit), not silent.
(assert (not (run-ok "./build/bender" "tools/profile-merge.janet"
                     "--out" "/tmp/pmg-x.jdn"
                     "home=/tmp/does-not-exist.jdn"))
        "merge missing file fails")
(assert (not (run-ok "./build/bender" "tools/profile-merge.janet"))
        "merge usage fails")
# empty inputs merge to empty (identity).
(spit "/tmp/pmg-empty.jdn" (string/format "%j" @[]))
(assert (run-ok "./build/bender" "tools/profile-merge.janet"
                "--out" "/tmp/pmg-empty-out.jdn"
                "home=/tmp/pmg-empty.jdn" "worker=/tmp/pmg-empty.jdn")
        "merge empty runs")
(assert (deep= @[] (parse (slurp "/tmp/pmg-empty-out.jdn")))
        "empty merge is empty")

# ---- C3: provenance report (seed vs conflict, home vs worker) ----
(spit "/tmp/pmg-fix.janet"
  "(defn wseed [x] (+ x 1))\n(defn wconf [x] (+ x 1))\n(wseed 1)\n(wconf 1)\n")
(spit "/tmp/pmg-phome.jdn"
  (string/format "%j"
    @[@[@["/tmp/pmg-fix.janet" 1 "wseed"]
        @{:calls 2 :params @[@[:number]] :mins @[1] :maxs @[5]
          :elems @[] :rets @[:number]}]
      @[@["/tmp/pmg-fix.janet" 2 "wconf"]
        @{:calls 1 :params @[@[:number]] :mins @[1] :maxs @[1]
          :elems @[] :rets @[:number]}]]))
(spit "/tmp/pmg-pwork.jdn"
  (string/format "%j"
    @[@[@["/tmp/pmg-fix.janet" 1 "wseed"]
        @{:calls 3 :params @[@[:number]] :mins @[10] :maxs @[20]
          :elems @[] :rets @[:number]}]
      @[@["/tmp/pmg-fix.janet" 2 "wconf"]
        @{:calls 1 :params @[@[:string]] :mins @[nil] :maxs @[nil]
          :elems @[] :rets @[:string]}]]))
(assert (run-ok "./build/bender" "tools/profile-merge.janet"
                "--out" "/tmp/pmg-pm.jdn"
                "home=/tmp/pmg-phome.jdn" "worker=/tmp/pmg-pwork.jdn")
        "merge for provenance")
(def [cg-code cg-text]
  (run-out "./build/bender" "tools/callgraph.janet" "/tmp/pmg-fix.janet"
           "--profile" "/tmp/pmg-pm.jdn"))
(assert (zero? cg-code) "callgraph runs on merged profile")
(assert (string/find "profile-seed wseed 0 number home,worker" cg-text)
        "unanimous seed with both provenances")
(assert (string/find "profile-conflict wconf 0 number:home string:worker" cg-text)
        "conflict with per-type provenances")

# ---- C5: worker-only evidence seeds a kernel the home run never saw ----
(spit "/tmp/pmg-wonly.janet"
  "(defn wkernel [x] (+ x 1))\n(wkernel 1)\n")
# home profile: wkernel never observed (no rows at all).
(spit "/tmp/pmg-woh.jdn" (string/format "%j" @[]))
# worker profile: one double observation.
(spit "/tmp/pmg-wow.jdn"
  (string/format "%j"
    @[@[@["/tmp/pmg-wonly.janet" 1 "wkernel"]
        @{:calls 5 :params @[@[:double]] :mins @[0.5] :maxs @[2.5]
          :elems @[] :rets @[:double]}]]))
(assert (run-ok "./build/bender" "tools/profile-merge.janet"
                "--out" "/tmp/pmg-wo.jdn"
                "home=/tmp/pmg-woh.jdn" "worker=/tmp/pmg-wow.jdn")
        "merge worker-only")
# home-only: no seed line (nothing observed anywhere).
(def [cg-h-code cg-h-text]
  (run-out "./build/bender" "tools/callgraph.janet" "/tmp/pmg-wonly.janet"
           "--profile" "/tmp/pmg-woh.jdn"))
(assert (zero? cg-h-code) "callgraph runs on empty profile")
(assert (nil? (string/find "profile-seed wkernel" cg-h-text))
        "no seed without evidence")
# merged: worker-only evidence seeds.
(def [cg-m-code cg-m-text]
  (run-out "./build/bender" "tools/callgraph.janet" "/tmp/pmg-wonly.janet"
           "--profile" "/tmp/pmg-wo.jdn"))
(assert (zero? cg-m-code) "callgraph runs on worker-only profile")
(assert (string/find "profile-seed wkernel 0 double worker" cg-m-text)
        "worker-only evidence seeds")

(end-suite)
