# Coordinate-derived terrain; only generate-chunk depends on world.
(import ./world :as world)

(def sea-level 28)
(def mask32 -1)

(defn hash2 [seed x z]
  # Janet's bit operations use signed 32-bit operands. Shift/xor mixing
  # stays in that domain; the final unsigned conversion preserves all bits.
  (var n (band (bxor seed (blshift x 11) (brshift x 7)
                     (blshift z 19) (brshift z 3)) mask32))
  (set n (band (bxor n (blshift n 13)) mask32))
  (set n (band (bxor n (brshift n 17)) mask32))
  (set n (band (bxor n (blshift n 5)) mask32))
  (if (< n 0) (+ n 4294967296) n))

(defn value-noise-2d [seed x z]
  # Cubic smoothstep interpolates four deterministic lattice values.
  (let [ix (math/floor x) iz (math/floor z)
        fx (- x ix) fz (- z iz)
        u (* fx fx (- 3 (* 2 fx)))
        v (* fz fz (- 3 (* 2 fz)))
        scale (/ 1.0 4294967296.0)
        a (* scale (hash2 seed ix iz))
        b (* scale (hash2 seed (+ ix 1) iz))
        c (* scale (hash2 seed ix (+ iz 1)))
        d (* scale (hash2 seed (+ ix 1) (+ iz 1)))]
    (+ (* (- 1 v) (+ a (* u (- b a))))
       (* v (+ c (* u (- d c)))))))

(defn fbm-2d [seed x z octaves lacunarity gain]
  (var total 0.0)
  (var weight 0.0)
  (var amplitude 1.0)
  (var frequency 1.0)
  (for i 0 octaves
    (set total (+ total (* amplitude (value-noise-2d (+ seed (* i 1013))
                                                      (* x frequency) (* z frequency)))))
    (set weight (+ weight amplitude))
    (set amplitude (* amplitude gain))
    (set frequency (* frequency lacunarity)))
  (if (> weight 0) (/ total weight) 0.0))

(defn height-at [seed wx wz]
  (+ 20 (math/floor (* 26 (fbm-2d seed (/ wx 48.0) (/ wz 48.0) 4 2.0 0.5)))))

(defn column-block [seed wx wy wz height]
  # Stone foundation, four-cell soil cap, sandy beaches, sea-level water.
  (cond
    (= wy 0) 1
    (< wy (- height 4)) 1
    (< wy height) (if (<= height (+ sea-level 1)) 4 2)
    (= wy height) (if (<= height (+ sea-level 1)) 4 3)
    (<= wy sea-level) 5
    0))

(defn generate-into [blocks cx cz seed]
  (for z 0 16
    (let [wz (+ (* cz 16) z)]
      (for x 0 16
        (let [wx (+ (* cx 16) x)
              height (height-at seed wx wz)
              beach (<= height 29)]
          (for y 0 64
            # Inline the column rules to keep the voxel hot loop flat.
            (put blocks (+ x (* 16 (+ z (* 16 y))))
                 (cond
                   (= y 0) 1
                   (< y (- height 4)) 1
                   (< y height) (if beach 4 2)
                   (= y height) (if beach 4 3)
                   (<= y sea-level) 5
                   0)))))))
  blocks)

(defn generate-chunk [w cx cz seed]
  (let [ch (world/chunk-new cx cz)]
    (generate-into (ch :blocks) cx cz seed)
    (world/world-put-chunk w ch)
    ch))
