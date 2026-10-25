# Local stand-in for the frozen world contract; never imports the sibling module.
(def chunk-x 16)
(def chunk-y 64)
(def chunk-z 16)
(def chunk-volume 16384)
(def block-air 0)
(def block-stone 1)
(def block-dirt 2)
(def block-grass 3)
(def block-sand 4)
(def block-water 5)
(def block-wood 6)
(def block-leaves 7)
(defn block-index [x y z] (+ x (* 16 (+ z (* 16 y)))))
(defn chunk-new [cx cz] {:cx cx :cz cz :blocks (buffer/new chunk-volume) :dirty false :mesh nil})
(defn chunk-set [ch x y z id] (put (ch :blocks) (block-index x y z) id))
(defn chunk-get [ch x y z] (get (ch :blocks) (block-index x y z)))
(defn world-new [] @{})
(defn world-put-chunk [w ch] (put w [(ch :cx) (ch :cz)] ch))
(defn world-get-chunk [w cx cz] (get w [cx cz]))

# This test keeps its own contract stub and does not import world.janet.
(import ../src/worldgen :as terrain)

(defn fresh [] (buffer/new chunk-volume))
(def a (fresh))
(def b (fresh))
(def c (fresh))
(terrain/generate-into a 0 0 1234)
(terrain/generate-into b 0 0 1234)
(terrain/generate-into c 0 0 5678)
(assert (= (string a) (string b)) "same seed/coords must produce identical bytes")
(assert (not= (string a) (string c)) "different seeds must change terrain")
(print "determinism: ok")

(each seam [-49 -17 -1 15 31 79]
  (each z [-33 0 17]
    (assert (<= (math/abs (- (terrain/height-at 1234 seam z)
                            (terrain/height-at 1234 (+ seam 1) z))) 3)
            "adjacent chunk seam heights diverged")))
(print "chunk seams: ok")

(for i 0 500
  (let [x (- (* i 37) 9200) z (- (* i 53) 13000)
        h (terrain/height-at -713 x z)]
    # Assert against the module's own declared band rather than a literal, so
    # retuning the terrain cannot leave this test silently checking a stale
    # design.
    (assert (and (>= h terrain/min-height) (<= h terrain/max-height))
            "height outside designed band")
    (let [n (terrain/value-noise-2d 4 (/ x 39.0) (/ z 43.0))
          f (terrain/fbm-2d 4 (/ x 39.0) (/ z 43.0) 4 2.0 0.5)]
      (assert (and (>= n 0) (<= n 1)) "value noise outside unit interval")
      (assert (and (>= f 0) (<= f 1)) "fBm outside unit interval"))))
(print "height band and noise range: ok")

# --- Hash avalanche ----------------------------------------------------
# Regression guard. An earlier xorshift hash had almost no avalanche on small
# inputs: hash2(seed,0,0) evaluated to exactly 270369*seed. Lattice points
# near the origin all hashed to near-zero, so terrain there was permanently
# low (always ocean) and the seed barely mattered. Uniformity over a large
# lattice looked perfect and hid it entirely, so these probes deliberately use
# SMALL coordinates and SMALL seeds.
(def small-hashes (map (fn [s] (terrain/hash2 s 0 0)) [1 2 3 4 5 6 7 8]))
(assert (= 8 (length (distinct small-hashes))) "hash2 collides on small seeds")
# Linear-in-seed output is the exact failure mode seen before: reject a
# constant first difference between consecutive seeds.
(def diffs (map (fn [i] (- (get small-hashes (+ i 1)) (get small-hashes i)))
                (range 0 7)))
(assert (> (length (distinct diffs)) 1) "hash2 is linear in the seed")
# A one-step coordinate change must scramble the result, not nudge it.
(each s [1 2 99]
  (def h0 (terrain/hash2 s 0 0))
  (def h1 (terrain/hash2 s 1 0))
  (def h2 (terrain/hash2 s 0 1))
  (assert (and (not= h0 h1) (not= h0 h2) (not= h1 h2))
          "hash2 does not avalanche across adjacent lattice points")
  (assert (> (math/abs (- h1 h0)) 1000)
          "adjacent lattice points hash to near-identical values"))
# Values near the origin must not be systematically small.
(var origin-sum 0)
(var origin-n 0)
(for z -4 5 (for x -4 5
  (+= origin-sum (/ (terrain/hash2 31337 x z) terrain/hash-modulus))
  (++ origin-n)))
(def origin-mean (/ origin-sum origin-n))
(assert (and (> origin-mean 0.3) (< origin-mean 0.7))
        "hash values near the origin are biased away from uniform")
(print "hash avalanche: ok")

# --- Seed sensitivity and relief ---------------------------------------
# The terrain must actually differ between seeds, and must not be a flat
# sheet. Both failed silently while the determinism test above still passed.
(defn- survey [seed]
  (var mn 9999) (var mx -9999) (var below 0) (var total 0)
  (for z -60 61 (for x -60 61
    (when (and (zero? (% x 5)) (zero? (% z 5)))
      (def h (terrain/height-at seed x z))
      (++ total)
      (when (< h terrain/sea-level) (++ below))
      (when (< h mn) (set mn h))
      (when (> h mx) (set mx h)))))
  {:min mn :max mx :relief (- mx mn) :ocean (/ below total)})

(def surveys (map survey [1 2 3 4 5 6]))
(each s surveys
  (assert (>= (s :relief) 15)
          (string "terrain is too flat: relief " (s :relief))))
(assert (> (length (distinct (map |($ :min) surveys))) 1)
        "every seed produces the same terrain")
(assert (> (length (distinct (map |($ :max) surveys))) 1)
        "every seed produces the same terrain")
# Across seeds the world should be neither all land nor all sea.
(def mean-ocean (/ (sum (map |($ :ocean) surveys)) (length surveys)))
(assert (and (> mean-ocean 0.1) (< mean-ocean 0.8))
        (string "land/sea balance is degenerate: " mean-ocean))
(printf "seed sensitivity and relief: ok (mean ocean %.0f%%, relief %j)"
        (* 100 mean-ocean) (map |($ :relief) surveys))

(def seen @{})
(for z 0 chunk-z
  (for x 0 chunk-x
    (var fluid false)
    (for y 0 chunk-y
      (let [id (get a (block-index x y z))
            solid (and (not= id block-air) (not= id block-water))]
        (put seen id true)
        (when (= y 0) (assert (= id block-stone) "missing stone foundation"))
        (when (and fluid solid) (error "solid block above water/air gap"))
        (when (or (= id block-air) (= id block-water)) (set fluid true))
        (when (= id block-water) (assert (<= y 28) "water above sea level"))))))
(print "column structure: ok")
(assert (>= (length (keys seen)) 3) "terrain needs at least three block types")
(assert (get seen block-stone) "terrain must contain stone")
(assert (get seen block-air) "terrain must contain air")
(print "non-degenerate terrain: ok")

(def start (os/clock))
(terrain/generate-into (fresh) 3 -2 1234)
(print "generate-into one chunk: " (* 1000 (- (os/clock) start)) " ms")
(print "worldgen tests: all passed")
