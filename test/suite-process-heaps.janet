# Step 1 (process heaps), slice 1.1: page ownership tagging.
# No behavior change: owners are recorded at page claim, never read
# for decisions yet. Each file runs in its own process.

(import ./helper :prefix "" :exit true)
(start-suite)

# 1. Shape + sums invariant (always deterministic).
(let [st (debug/slab-stats)]
  (assert (dictionary? st) "stats is a table")
  (assert (int? (st :pages)) "pages count")
  (assert (int? (st :unowned)) "unowned count")
  (assert (dictionary? (st :owned)) "owned table")
  (var owned-sum 0)
  (eachk id (st :owned) (+= owned-sum ((st :owned) id)))
  (assert (= (st :pages) (+ owned-sum (st :unowned))) "sums hold")
  (assert (> (st :pages) 0) "pages exist"))

# 2. Owner ids: nonzero, fresh per fiber, strictly increasing.
(def id-main (debug/fiber-owner))
(assert (and (int? id-main) (> id-main 0)) "main fiber has nonzero id")
(def f1 (fiber/new (fn [] (debug/fiber-owner))))
(def f2 (fiber/new (fn [] (debug/fiber-owner))))
(def id1 (resume f1))
(def id2 (resume f2))
(assert (and (int? id1) (> id1 id-main)) "fresh fiber id increases")
(assert (> id2 id1) "ids strictly increase")

# 3. Fresh claims attribute: allocate live arrays in a fresh fiber until
# its id appears in the owners table (fresh pages are unavoidable at
# volume; recycled boot slack is finite). Bounded; fails loudly if the
# mechanism breaks.
(def f3 (fiber/new (fn []
  (def box @[])
  (var found false)
  (var guard 0)
  (while (and (not found) (< guard 100))
    (++ guard)
    (repeat 200 (array/push box (array/new 4)))
    (def me (debug/fiber-owner))
    (def st (debug/slab-stats))
    (when (> (or ((st :owned) me) 0) 0) (set found true)))
  found)))
(assert (resume f3) "fresh claims attribute to the fiber")

# 4. Pages are monotonic (freed only at clear): workload never shrinks.
(let [before ((debug/slab-stats) :pages)
      f (fiber/new (fn [] (repeat 500 (array/new 4))))]
  (resume f)
  (gccollect)
  (assert (>= ((debug/slab-stats) :pages) before) "pages monotonic"))

(end-suite)
