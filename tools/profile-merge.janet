# tools/profile-merge.janet — Phase C worker-profile merge.
#
# Combines a home profile dump (debug/profile-dump) with per-worker
# harvests (pool/profile-harvest rows, thread/profile rows) into one
# profile file for callgraph --profile. Matching stays by defn name in
# the consumer (same-name rows merge conservatively there), so this
# tool tags provenance and concatenates by default: no evidence is dropped
# or weakened here, and output order is deterministic (file order).
# --accumulate coalesces rows with the same full identity across runs,
# unioning type evidence and expanding ranges without erasing conflicts.
#
# Usage:
#   ./build/bender tools/profile-merge.janet --out merged.jdn \
#     home=home.jdn worker=pool1.jdn [worker=pool2.jdn ...]
#   ./build/bender tools/profile-merge.janet --accumulate --out cache.jdn \
#     cached=old.jdn run=new.jdn
# Bare paths default by position (first=:home, rest=:worker).
# Each input is a dump-format row array (@[[id info] ...]); rows keep
# their shape with :from added to info (:home/:worker). Prints a one-
# line summary to stderr (row counts per provenance).

(defn- usage []
  (eprint "usage: profile-merge.janet [--accumulate] --out OUT.jdn [home=]HOME.jdn [worker=...]")
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

# Normalize only known evidence; malformed input fails rather than silently
# turning a conflicting observation into a single-type seed.
(defn- bad [message]
  (eprint (string "profile-merge: " message))
  (os/exit 3))

(defn- types [slot]
  (unless (or (nil? slot) (indexed? slot)) (bad "invalid type slot"))
  (def out @[])
  (when slot
    (each t slot
      (unless (keyword? t) (bad "invalid observed type"))
      (unless (index-of out t) (array/push out t))))
  (sorted out))

(defn- origins [from]
  (def fs (if (indexed? from) from [from]))
  (each f fs (unless (keyword? f) (bad "invalid provenance")))
  (sorted (distinct fs)))

(defn- union-types [a b]
  (if (and (nil? a) (nil? b)) nil
    (types [;(types a) ;(types b)])))

(defn- bound [v]
  (cond (number? v) v
        (= v "inf") math/inf
        (= v "-inf") (- math/inf)
        (nil? v) nil
        (bad "invalid range bound")))

(defn- range-union [lo1 hi1 lo2 hi2]
  (def a (bound lo1))
  (def b (bound hi1))
  (def c (bound lo2))
  (def d (bound hi2))
  # A missing or NaN range is unknown, not permission to assert nonnegative.
  (if (or (nil? a) (nil? b) (nil? c) (nil? d)
          (not (<= a b)) (not (<= c d)))
    [nil nil]
    [(if (< a c) lo1 lo2) (if (> b d) hi1 hi2)]))

(defn- union-slots [a b kind]
  (def x (or a @[]))
  (def y (or b @[]))
  (unless (and (indexed? x) (indexed? y)) (bad (string "invalid " kind)))
  (def out @[])
  (for i 0 (max (length x) (length y))
    (def left (get x i))
    (def right (get y i))
    (array/push out
      (if (and (= kind :params) (or (nil? left) (nil? right)))
        nil # missing historical param evidence cannot become unanimous
        (union-types left right))))
  out)

# Joint vectors are never reconstructed from marginal :params. Missing
# historical evidence and overflow are both OPEN. Only exact vectors merge.
(defn- signatures [value]
  (when (or (nil? value) (= value :open)) (break :open))
  (unless (indexed? value) (bad "invalid signatures"))
  (def counts @{})
  (def vectors @{})
  (each entry value
    (unless (and (indexed? entry) (= 2 (length entry))
                 (indexed? (entry 0)) (<= (length (entry 0)) 8)
                 (number? (entry 1)) (> (entry 1) 0)
                 (= (entry 1) (math/trunc (entry 1))))
      (bad "invalid signature entry"))
    (each t (entry 0) (unless (keyword? t) (bad "invalid signature tag")))
    (def key (string/format "%j" (entry 0)))
    (put vectors key (entry 0))
    (put counts key (+ (entry 1) (get counts key 0))))
  (if (> (length (keys counts)) 8) :open
      (seq [key :in (sorted (keys counts))] @[(get vectors key) (get counts key)])))

(defn- merge-signatures [a b]
  (def x (signatures a)) (def y (signatures b))
  (if (or (= x :open) (= y :open)) :open
      (signatures [;x ;y])))

(defn- accumulate-info [a b]
  (def ca (get a :calls))
  (def cb (get b :calls))
  (unless (and (number? ca) (number? cb) (>= ca 0) (>= cb 0)
               (= ca (math/trunc ca)) (= cb (math/trunc cb)))
    (bad "invalid call count"))
  (def ps (union-slots (get a :params) (get b :params) :params))
  (def ea (union-slots (get a :elems) (get b :elems) :elems))
  (def ma (get a :mins @[])) (def xa (get a :maxs @[]))
  (def mb (get b :mins @[])) (def xb (get b :maxs @[]))
  (unless (and (indexed? ma) (indexed? xa) (indexed? mb) (indexed? xb))
    (bad "invalid ranges"))
  (def mn @[]) (def mx @[])
  (for i 0 (max (length ps) (length ma) (length mb))
    (def [lo hi] (range-union (get ma i) (get xa i)
                              (get mb i) (get xb i)))
    (array/push mn lo) (array/push mx hi))
  (def from (sorted (distinct [;(origins (get a :from :home))
                                ;(origins (get b :from :home))])))
  (def type-origins @[])
  (for i 0 (length ps)
    (def by-type @{})
    (each info [a b]
      (def source (get info :from :home))
      (def fs (origins source))
      (def previous (get (get info :type-from @[]) i))
      (when (and previous (not (table? previous))) (bad "invalid type provenance"))
      (each t (types (get (get info :params @[]) i))
        (put by-type t (sorted (distinct [;(or (get by-type t) @[])
                                          ;(origins (or (get previous t) fs))])))))
    (array/push type-origins by-type))
  @{:calls (+ ca cb) :params ps :elems ea :mins mn :maxs mx
    :rets (union-types (get a :rets) (get b :rets)) :from from
    :type-from type-origins
    :signatures (merge-signatures (get a :signatures) (get b :signatures))})

(defn main [& _args]
  # Script name arrives as args[0] (like tools/callgraph.janet, which
  # slices (dyn :args)); drop it so only real operands are parsed.
  (def argv (tuple/slice _args 1))
  (var out nil)
  (var inputs @[])
  (var seen-out false)
  (var first-bare true)
  (var accumulate false)
  (each a argv
    (cond
      seen-out (do (set out a) (set seen-out false))
      (= a "--out") (set seen-out true)
      (= a "--accumulate") (set accumulate true)
      (do (def eq (string/find "=" a))
        (if (nil? eq)
          (do (array/push inputs [(if first-bare :home :worker) a])
              (set first-bare false))
          (array/push inputs [(keyword (string/slice a 0 eq))
                              (string/slice a (+ eq 1))])))))
  (when (or (nil? out) (empty? inputs)) (usage))
  (def merged @[])
  (def counts @{})
  (def positions @{})
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
      (if accumulate
        (do (unless (and (indexed? id) (= 3 (length id)))
              (bad "invalid identity"))
            (def key (string/format "%j" id))
            (def pos (get positions key))
            (def tagged (merge info {:from (origins (get info :from prov))
                                     :signatures (signatures (get info :signatures))}))
            (unless (and (number? (get info :calls)) (>= (get info :calls) 0)
                         (= (get info :calls) (math/trunc (get info :calls))))
              (bad "invalid call count"))
            (if (nil? pos)
              (do (put positions key (length merged))
                  (def type-origins @[])
                  (each slot (get tagged :params @[])
                    (def per @{})
                    (def existing (get (get tagged :type-from @[]) (length type-origins)))
                    (when (and existing (not (table? existing))) (bad "invalid type provenance"))
                    (each t (types slot)
                      (put per t (origins (or (get existing t) (get tagged :from)))))
                    (array/push type-origins per))
                  (def mins (get tagged :mins @[]))
                   (def maxs (get tagged :maxs @[]))
                   (unless (and (indexed? mins) (indexed? maxs)) (bad "invalid ranges"))
                   (def normalized-mins @[])
                   (def normalized-maxs @[])
                   (for i 0 (max (length type-origins) (length mins) (length maxs))
                     (def [lo hi] (range-union (get mins i) (get maxs i)
                                                (get mins i) (get maxs i)))
                     (array/push normalized-mins lo)
                     (array/push normalized-maxs hi))
                   (array/push merged @[id (merge tagged
                     {:params (map types (get tagged :params @[]))
                      :elems (map types (get tagged :elems @[]))
                      :rets (types (get tagged :rets @[]))
                      :mins normalized-mins :maxs normalized-maxs
                      :type-from type-origins})]))
              (put merged pos @[id (accumulate-info ((merged pos) 1) tagged)])))
        (array/push merged @[(if (indexed? id) id @[]) (merge info {:from prov})]))
      (put counts prov (+ 1 (or (get counts prov) 0)))))
  (spit out (string/format "%j" merged))
  (eprint (string "profile-merge: " (length merged) " rows -> " out
                 " (" (string/join (map (fn [[k v]] (string k "=" v))
                                        (pairs counts)) " ") ")")))
