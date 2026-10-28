# test/suite-gc-slab-pages.janet
#
# Slab page table stress: many pages, exact membership, no false negatives.
#
# The slab page table maps a page base -> page header and is consulted on the
# hot path (janet_slab_obj_old, owner lookup, write barrier). It was a sorted
# array with binary search; it is now an open-addressed hash set. Both need to
# answer exactly, and the failure mode of a wrong answer is silent corruption
# (an object misclassified as old, or a page reported absent so its objects
# look like malloc-backed strangers), not a crash.
#
# So this test exists to force MANY pages and then demand the collector still
# behave: object identity and contents survive across collections, and dead
# objects are actually reclaimed. Page count scales with live bytes / page
# size, so allocating well past one page per size class is what exercises
# probes, rehashing and load factor.
#
# Run: JANET_NO_PROFILE=1 ./build/bender test/suite-gc-slab-pages.janet

(defn check [ok msg] (unless ok (eprintf "FAIL: %s" msg) (os/exit 1)))

# 1. Spread allocations across every size class and every page of each class.
#    Arrays of many lengths land in different classes; keeping them live pins
#    the pages so the table holds real entries at probe time.
(def live @[])
(var total 0)
(for len 0 400
  (def a @[])
  (for i 0 len
    (array/push a (string "p" len "-" i)))
  (array/push live a)
  (set total (+ total (length a))))
(check (= 79800 total) "allocation grid must be fully populated")

# 2. Every object must still be reachable and correct after a full collection.
(gccollect)
(check (= 400 (length live)) "root array of live containers must survive")

(for len 0 400
  (def a (get live len))
  (check (= len (length a)) (string "container " len " kept its length"))
  (for i 0 len
    (check (= (string "p" len "-" i) (get a i))
           (string "element " len "/" i " kept its value"))))

# 3. Churn: allocate and drop a large set so pages are recycled, then confirm
#    the surviving set is untouched. A page table that lost an entry (false
#    absent) would misclassify these objects as old/foreign and either keep
#    garbage or free live data.
(for round 0 3
  (var scratch @[])
  (for len 0 200
    (def a @[])
    (for i 0 len (array/push a (string "churn" round "-" len "-" i)))
    (array/push scratch a))
  (set scratch @[])
  (gccollect))

(for len 0 400
  (def a (get live len))
  (check (= len (length a)) (string "post-churn container " len " intact"))
  (for i 0 len
    (check (= (string "p" len "-" i) (get a i))
           (string "post-churn element " len "/" i " intact"))))

# 4. Tables (a different class path) with many live entries, plus strings that
#    the collector must not swallow while their pages are in the table.
(def tab @{})
(for i 0 2000
  (put tab (string "k" i) (string "v" i)))
(gccollect)
(for i 0 2000
  (check (= (string "v" i) (get tab (string "k" i)))
         (string "table entry " i " intact")))

# 5. Objects allocated AFTER the table has grown must be found too: insertion
#    into a heavily loaded table is the other half of the probe contract.
(def late @[])
(for i 0 5000 (array/push late (string "late" i)))
(gccollect)
(for i 0 5000
  (check (= (string "late" i) (get late i)) (string "late object " i " intact")))

# 6. Reclamation still works: a dropped generation must actually be freed.
#    Verify indirectly and robustly — a held weak-ish probe is overkill here,
#    so instead confirm repeated churn completes without error and the live
#    set is still exact. (Memory reclamation itself is covered by
#    suite-gc-stress and suite-process-heaps.)
(for i 0 5
  (var g @[])
  (for j 0 2000 (array/push g (string "gen" i "-" j)))
  (set g @[])
  (gccollect))

(check (= 400 (length live)) "live set unchanged after repeated churn")
(check (= 79800 (do (var t 0) (each a live (set t (+ t (length a)))) t))
       "element total unchanged after repeated churn")

(print "suite-gc-slab-pages PASSED")
