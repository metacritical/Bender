# Report observed differences between independent workloads. Element masks
# are sampled hints, never proof of an array's contents or shape.
(defn- mask [tags] (sorted (distinct (or tags @[]))))
(defn- normalized [info field]
  (def value (get info field))
  (if (= field :rets) (mask value)
      (map mask (or value @[]))))
(defn main [& args]
  (def paths (tuple/slice args 1))
  (when (< (length paths) 2) (break nil))
  (def rows (map (fn [path] (parse (slurp path))) paths))
  (def by-id @{})
  (for workload 0 (length rows)
    (each [id info] (rows workload)
      (def key (string/format "%j" id))
      (def slots (or (get by-id key) @[]))
      (while (< (length slots) workload) (array/push slots nil))
      (array/push slots info)
      (put by-id key slots)))
  (var shown 0)
  (each [id slots] (pairs by-id)
    (for i 0 (length slots)
      (def a (slots i))
      (when a
        (for j (+ i 1) (length slots)
          (def b (slots j))
          (when b
            (each field [:params :rets :elems]
              (def x (normalized a field)) (def y (normalized b field))
              (unless (deep= x y)
                (when (< shown 12)
                  (eprint "pgo-divergence: " id " workload " i " vs " j
                          " " field " " (string/format "%j" x)
                          " != " (string/format "%j" y)
                          (if (= field :elems) " (sampled only)" "")))
                (++ shown))))))))
  (when (> shown 12)
    (eprint "pgo-divergence: " (- shown 12) " more differences")))
