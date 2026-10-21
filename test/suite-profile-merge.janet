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

# ---- inf-bearing profile loads (non-finite bounds serialize as tagged
# strings; the loader used to die on unknown symbol inf) ----
(spit "/tmp/pmg-inf.jdn"
  (string/format "%j"
    @[@[@["/tmp/pmg-wonly.janet" 1 "wkernel"]
        @{:calls 5 :params @[@[:double]] :mins @["-inf"] :maxs @["inf"]
          :elems @[] :rets @[:double] :from :worker}]]))
(def [cg-i-code cg-i-text]
  (run-out "./build/bender" "tools/callgraph.janet" "/tmp/pmg-wonly.janet"
           "--profile" "/tmp/pmg-inf.jdn"))
(assert (zero? cg-i-code) "callgraph loads inf-bearing profile")
(assert (string/find "profile-seed wkernel 0 double worker" cg-i-text)
        "inf-bearing evidence still seeds")

# ---- Run-over-run accumulation: union observations, retain type origins,
# expand ranges, and never seed a conflicting slot. ----
(spit "/tmp/pmg-acc-a.jdn" (string/format "%j"
  @[@[@["/tmp/pmg-fix.janet" 1 "wseed"]
      @{:calls 2 :params @[@[:number]] :mins @[2] :maxs @[9]
        :elems @[] :rets @[:number] :from :home :edges @[0 2 3 1]
         :receivers @[@[:array] @[:table]]}]]))
(spit "/tmp/pmg-acc-b.jdn" (string/format "%j"
  @[@[@["/tmp/pmg-fix.janet" 1 "wseed"]
      @{:calls 3 :params @[@[:string]] :mins @[-4] :maxs @["inf"]
        :elems @[] :rets @[:string] :from :worker :edges @[1 4 2 0]
         :receivers @[@[:tuple] @[:table]]}]]))
(assert (run-ok "./build/bender" "tools/profile-merge.janet" "--accumulate"
                "--out" "/tmp/pmg-acc-out.jdn"
                "cached=/tmp/pmg-acc-a.jdn" "run=/tmp/pmg-acc-b.jdn")
        "accumulation runs")
(def accumulated (parse (slurp "/tmp/pmg-acc-out.jdn")))
(def ai ((accumulated 0) 1))
(assert (= 1 (length accumulated)) "same identity coalesces")
(assert (= 5 (get ai :calls)) "counts add")
(assert (deep= @[1 6 5 1] (get ai :edges)) "per-PC successors add")
(assert (deep= @[@[:array :tuple] @[:table]] (get ai :receivers))
        "receiver types union per original PC")
(assert (deep= @[:number :string] ((get ai :params) 0)) "types union")
(assert (deep= @[-4] (get ai :mins)) "range min expands")
(assert (deep= @["inf"] (get ai :maxs)) "range max expands")
(assert (deep= @[:home :worker] (get ai :from)) "origins union")
(def [acc-code acc-log] (run-out "./build/bender" "tools/callgraph.janet"
                                 "/tmp/pmg-fix.janet" "--profile" "/tmp/pmg-acc-out.jdn"))
(assert (zero? acc-code) "callgraph accepts accumulated profile")
(assert (string/find "profile-conflict wseed 0 number:home string:worker" acc-log)
        "conflict reports per-type provenance")
(assert (nil? (string/find "profile-seed wseed" acc-log)) "conflict never seeds")
# Re-merge an already accumulated row: origin maps remain per type.
(assert (run-ok "./build/bender" "tools/profile-merge.janet" "--accumulate"
                "--out" "/tmp/pmg-acc-twice.jdn"
                "cached=/tmp/pmg-acc-out.jdn" "run=/tmp/pmg-acc-a.jdn")
        "reaccumulation runs")
(def twice (parse (slurp "/tmp/pmg-acc-twice.jdn")))
(assert (= 7 (get ((twice 0) 1) :calls)) "third-run calls add")
(assert (deep= @[1 8 8 2] (get ((twice 0) 1) :edges))
        "reaccumulation preserves PC counts")
(assert (deep= @[:home] (get ((get ((twice 0) 1) :type-from) 0) :number))
        "third-run provenance preserved")

# Missing historical slots stay unknown rather than manufacturing unanimity.
(spit "/tmp/pmg-missing.jdn" (string/format "%j"
  @[@[@["/tmp/pmg-fix.janet" 1 "wseed"]
      @{:calls 1 :params @[] :mins @[] :maxs @[]
        :elems @[] :rets @[] :from :home}]]))
(assert (run-ok "./build/bender" "tools/profile-merge.janet" "--accumulate"
                "--out" "/tmp/pmg-unknown.jdn"
                "cached=/tmp/pmg-missing.jdn" "home=/tmp/pmg-acc-a.jdn")
        "missing-slot merge runs")
(def missing-slot ((get ((parse (slurp "/tmp/pmg-unknown.jdn")) 0) 1) :params))
(assert (nil? (missing-slot 0)) "missing evidence remains unknown")
(assert (= :open (get (((parse (slurp "/tmp/pmg-unknown.jdn")) 0) 1) :edges))
        "missing edge history cannot imply unobserved successors")
(assert (= :open (get (((parse (slurp "/tmp/pmg-unknown.jdn")) 0) 1) :receivers))
        "missing receiver history cannot imply a monomorphic site")
(def [unknown-code unknown-log]
  (run-out "./build/bender" "tools/callgraph.janet" "/tmp/pmg-fix.janet"
           "--profile" "/tmp/pmg-unknown.jdn"))
(assert (zero? unknown-code) "callgraph accepts unknown slot")
(assert (nil? (string/find "profile-seed wseed" unknown-log))
        "unknown history never seeds")

# Cached profiles are data, never code: a hostile form must not execute.
(spit "/tmp/pmg-hostile.jdn"
  "(do (spit \"/tmp/pmg-profile-executed\" \"bad\") @[])\n")
(try (os/rm "/tmp/pmg-profile-executed") ([_] nil))
(def [evil-code evil-text]
  (run-out "./build/bender" "tools/callgraph.janet" "/tmp/pmg-wonly.janet"
           "--profile" "/tmp/pmg-hostile.jdn"))
(assert (not (zero? evil-code)) "executable profile form rejected")
(assert (nil? (os/stat "/tmp/pmg-profile-executed")) "profile never executes code")
(assert (string/find "invalid PGO profile" evil-text) "invalid profile diagnostic")
(spit "/tmp/pmg-trailing.jdn" "@[] (spit \"/tmp/pmg-profile-executed\" \"bad\")\n")
(def [trailing-code trailing-text]
  (run-out "./build/bender" "tools/callgraph.janet" "/tmp/pmg-wonly.janet"
           "--profile" "/tmp/pmg-trailing.jdn"))
(assert (not (zero? trailing-code)) "trailing form rejected")
(assert (nil? (os/stat "/tmp/pmg-profile-executed")) "trailing form never executes")
(spit "/tmp/pmg-bare.jdn" "[[\"/tmp/pmg-wonly.janet\" 1 \"wkernel\"] {:calls 1}]\n")
(def [bare-code bare-text]
  (run-out "./build/bender" "tools/callgraph.janet" "/tmp/pmg-wonly.janet"
           "--profile" "/tmp/pmg-bare.jdn"))
(assert (not (zero? bare-code)) "bare tuple profile rejected")
(assert (string/find "invalid PGO profile" bare-text) "bare tuple diagnostic")
(spit "/tmp/pmg-malformed.jdn" (string/format "%j"
  @[@[@["/tmp/pmg-wonly.janet" 1 "wkernel"]
      @{:calls 1 :params @[@[:double]] :mins @[0]
        :maxs @[1] :elems @[] :rets @[:double]
        :type-from @[@{:double "not-origins"}]}]]))
(def [malformed-code malformed-text]
  (run-out "./build/bender" "tools/callgraph.janet" "/tmp/pmg-wonly.janet"
           "--profile" "/tmp/pmg-malformed.jdn"))
(assert (not (zero? malformed-code)) "malformed provenance rejected")
(assert (string/find "invalid PGO profile" malformed-text)
        "malformed provenance diagnostic")

(end-suite)
