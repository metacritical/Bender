# fannkuch-redux: pancake flips over all n! permutations (benchmarksgame).
# Mirrors the reference C (Joseph Piché, from Mazurov/Gouy Java):
# count-down rotation driver, permCount-parity checksum (as toggling
# sign — identical sequence). Hot loops live in hinted defns so Bender
# can kernelize them; allocation/printing stays boxed in main.
# Usage: bender fannkuch.janet N  (N=7 → "228" + "Pfannkuchen(7) = 16",
#   N=12 → "3968050" + "Pfannkuchen(12) = 65")

(defn count-flips {:hint {:params [:array :array] :returns :long}} [q perm]
  # n is always the full length in real calls (and must be: partial
  # copies duplicate elements, breaking the permutation invariant the
  # flip loop needs to terminate). Derived, not passed.
  (def n (length q))
  (for i 0 n (put q i (perm i)))
  (var flips 0)
  (var k (q 0))
  (while (not= k 0)
    (var i 0)
    (var j k)
    (while (< i j)
      (def t (q i))
      (put q i (q j))
      (put q j t)
      (++ i)
      (-- j))
    (++ flips)
    (set k (q 0)))
  flips)

(defn fannkuch-core
  {:hint {:params [:array :array :array :long :array] :returns :double}}
  [perm1 count q n out]
  (for i 0 n (put perm1 i i))
  (var maxflips 0)
  (var sign 1)
  (var checksum 0.0)
  (var r n)
  (var running true)
  (while running
    (while (not= r 1)
      (put count (- r 1) r)
      (-- r))
    (def flips (count-flips q perm1))
    (when (> flips maxflips) (set maxflips flips))
    (+= checksum (* sign flips))
    (var gen false)
    (while (and (not gen) running)
      (if (= r n)
        (do (put out 0 maxflips)
          (set gen true)
          (set running false))
        (do (def p0 (perm1 0))
          (var i 0)
          (while (< i r)
            (put perm1 i (perm1 (+ i 1)))
            (++ i))
          (put perm1 r p0)
          (put count r (- (count r) 1))
          (if (> (count r) 0)
            (set gen true)
            (++ r)))))
    (set sign (- sign)))
  checksum)

(defn main [&]
  (def n (scan-number (get (dyn :args) 1 "7")))
  (def perm1 (array/new-filled n 0))
  (def count (array/new-filled n 0))
  (def q (array/new-filled n 0))
  (def out (array/new-filled 1 0))
  (def checksum (fannkuch-core perm1 count q n out))
  (print checksum)
  (print (string "Pfannkuchen(" n ") = " (out 0))))

# top-level call runs in AOT images; os/exit beats the CLI auto-call
(os/exit (main))
