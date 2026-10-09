# tools/profile-merge.janet — Phase C worker-profile merge.
#
# Combines a home profile dump (debug/profile-dump) with per-worker
# harvests (pool/profile-harvest rows, thread/profile rows) into one
# profile file for callgraph --profile. Matching stays by defn name in
# the consumer (same-name rows merge conservatively there), so this
# tool only tags provenance and concatenates: no evidence is dropped
# or weakened here, and output order is deterministic (file order).
#
# Usage:
#   ./build/bender tools/profile-merge.janet --out merged.jdn \
#     home=home.jdn worker=pool1.jdn [worker=pool2.jdn ...]
# Bare paths default by position (first=:home, rest=:worker).
# Each input is a dump-format row array (@[[id info] ...]); rows keep
# their shape with :from added to info (:home/:worker). Prints a one-
# line summary to stderr (row counts per provenance).

(defn- usage []
  (eprint "usage: profile-merge.janet --out OUT.jdn [home=]HOME.jdn [worker=...]")
  (os/exit 2))

(defn- read-rows [path]
  (def text
    (try (slurp path)
      ([e] (eprint (string "profile-merge: cannot read " path ": " e))
              (os/exit 3))))
  (def rows
    (try (parse text)
      ([e] (eprint (string "profile-merge: cannot parse " path ": " e))
              (os/exit 3))))
  (unless (indexed? rows)
    (eprint (string "profile-merge: not a row array: " path))
    (os/exit 3))
  rows)

(defn main [& _args]
  # Script name arrives as args[0] (like tools/callgraph.janet, which
  # slices (dyn :args)); drop it so only real operands are parsed.
  (def argv (tuple/slice _args 1))
  (var out nil)
  (var inputs @[])
  (var seen-out false)
  (var first-bare true)
  (each a argv
    (cond
      seen-out (do (set out a) (set seen-out false))
      (= a "--out") (set seen-out true)
      (do (def eq (string/find "=" a))
        (if (nil? eq)
          (do (array/push inputs [(if first-bare :home :worker) a])
              (set first-bare false))
          (array/push inputs [(keyword (string/slice a 0 eq))
                              (string/slice a (+ eq 1))])))))
  (when (or (nil? out) (empty? inputs)) (usage))
  (def merged @[])
  (def counts @{})
  (each [prov path] inputs
    (each row (read-rows path)
      (unless (and (indexed? row) (= 2 (length row)))
        (eprint (string "profile-merge: bad row in " path))
        (os/exit 3))
      (def [id info] row)
      (unless (table? info)
        (eprint (string "profile-merge: bad info in " path))
        (os/exit 3))
      # Rows stay @[...] arrays (dump format): bare [...] reads as a
      # tuple, which eval would try to call.
      (array/push merged @[(if (indexed? id) id @[]) (merge info {:from prov})])
      (put counts prov (+ 1 (or (get counts prov) 0)))))
  (spit out (string/format "%j" merged))
  (eprint (string "profile-merge: " (length merged) " rows -> " out
                 " (" (string/join (map (fn [[k v]] (string k "=" v))
                                        (pairs counts)) " ") ")")))
