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

# Step 1.2: per-fiber nursery accounting + cross-fiber barrier.
# 5. Nursery bytes charge allocations to the running fiber (bounds:
# array headers are fixed-size; data buffers are malloc, untracked).
(let [f (fiber/new (fn []
                     (def before (debug/fiber-nursery))
                     (def box @[])
                     (repeat 500 (array/push box (array/new 4)))
                     (- (debug/fiber-nursery) before)))]
  (def delta (resume f))
  (assert (and (> delta 0) (< delta (* 500 1024))) "nursery charges"))
# Host allocations (no running fiber... main script runs in a fiber,
# so check the counter moves there too) stay sane.
(assert (>= (debug/fiber-nursery) 0) "nursery readable")

# 6. Barrier recording: same-fiber stores never record (deterministic
# negative control — same owner or shared on either side skips); a
# cross-fiber store records its container.
(let [before ((debug/slab-stats) :remembered)
      f (fiber/new (fn []
                     (def box @[])
                     (def tab @{})
                     (repeat 200 (array/push box (array/new 4)))
                     (each v box (put tab (gensym) v))
                     tab))]
  (resume f)
  (assert (= before ((debug/slab-stats) :remembered)) "same-fiber silent"))
# Positive: fresh per-iteration tables in main + values spread across a
# big foreign box; loop until one cross-owned edge records (fresh
# pages unavoidable at volume; bound fails loudly if broken).
(let [fa (fiber/new (fn []
                      (def box @[])
                      (repeat 2000 (array/push box (array/new 4)))
                      box))
      abox (resume fa)
      vals (map (fn [i] (get abox (* i 199))) (range 10))
      lot @[]]
  (var grew false)
  (var guard 0)
  (while (and (not grew) (< guard 30))
    (++ guard)
    (def before ((debug/slab-stats) :remembered))
    (repeat 100
      (def t @{})
      (array/push lot t)
      (each v vals (put t (gensym) v)))
    (when (> ((debug/slab-stats) :remembered) before)
      (set grew true)))
  (assert grew "cross-fiber store recorded")
  (assert (> (length lot) 0) "setup live"))

(end-suite)
