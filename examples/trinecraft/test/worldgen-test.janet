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
    (assert (and (>= h 20) (<= h 45)) "height outside designed band")
    (let [n (terrain/value-noise-2d 4 (/ x 39.0) (/ z 43.0))
          f (terrain/fbm-2d 4 (/ x 39.0) (/ z 43.0) 4 2.0 0.5)]
      (assert (and (>= n 0) (<= n 1)) "value noise outside unit interval")
      (assert (and (>= f 0) (<= f 1)) "fBm outside unit interval"))))
(print "height band and noise range: ok")

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
