# Hand-counted original-PC dispatch oracle, independent of ttfx and PGO.
(defn leaf [x] (+ x 1))
(defn probe [n]
  (var i 0)
  (var s 0)
  (while (< i n)
    (set s (leaf s))
    (++ i))
  s)
(defn branch-probe [n]
  (var i 0)
  (var s 0)
  (while (< i n)
    (if (= (band i 1) 0)
      (++ s)
      (set s (- s 1)))
    (++ i))
  s)
(assert (= 3 (probe 3)))
(assert (= 1 (branch-probe 3)))
# One definition has 301 original PCs, notably 256 beyond the PGO cap.
(def code (array/new-filled 300 '(noop)))
(array/push code '(retn))
(def long-probe (asm {:arity 0 :slotcount 1 :bytecode code}))
(long-probe)
# Reclaim temporary definitions, then churn new definitions to exercise
# recycled allocator addresses without changing earlier row identities.
(for i 0 200
  ((asm {:arity 0 :slotcount 1 :bytecode '[(ldi 0 1) (ret 0)]})))
(gcsetinterval 1)
(for i 0 200
  ((asm {:arity 0 :slotcount 1 :bytecode '[(ldi 0 2) (ret 0)]})))
(gcsetinterval 1)
(print "site-probe-ok")
