# Coordinate-derived terrain; only generate-chunk depends on world.
(import ./world :as world)

(def sea-level 28)
(def mask32 0xFFFFFFFF)


(defn- i32
  ``Reduce an arbitrary integer into the signed 32-bit range.

  bender's bit operations are 32-bit signed: they REJECT an operand outside
  that range (band 2147483648 raises) while blshift wraps its result. So every
  value entering the mixer must be reduced first, and the mixer itself may
  then overflow freely.``
  [n]
  (def m (% n 4294967296))
  (def p (if (neg? m) (+ m 4294967296) m))
  (if (>= p 2147483648) (- p 4294967296) p))

(def hash-modulus 2147483647)   # 2^31-1, prime
(def- inv-modulus (/ 1.0 2147483647.0))

(defn hash2
  ``Deterministic hash of a lattice point and seed, in [0, 2^31).

  Three Lehmer rounds modulo the Mersenne prime 2^31-1. Every product stays
  below 2^48, well inside the 2^53 a double represents exactly, so this is
  reproducible without any integer type.

  It replaces an xorshift version that looked reasonable and was badly broken:
  xorshift has almost no avalanche on small inputs, so hash2(seed,0,0) came
  out as exactly 270369*seed. Lattice points near the origin therefore all
  hashed to near-zero, which made the terrain around the origin permanently
  low -- always ocean -- and made the seed almost irrelevant, since scaling a
  near-zero value by the seed leaves it near zero. Uniformity over a large
  lattice looked perfect and hid all of it; only probing small coordinates
  directly exposed it.``
  [seed x z]
  (var n (% (+ 1
               (* (+ (i32 x) 10007) 48271)
               (* (+ (i32 z) 20011) 69621)
               (* (+ (i32 seed) 30011) 16807))
            hash-modulus))
  (when (neg? n) (set n (+ n hash-modulus)))
  (set n (% (* (+ n 1) 48271) hash-modulus))
  (set n (% (* (+ n 1) 69621) hash-modulus))
  (set n (% (* (+ n 1) 16807) hash-modulus))
  n)

(defn value-noise-2d [seed x z]
  # Cubic smoothstep interpolates four deterministic lattice values.
  (let [ix (math/floor x) iz (math/floor z)
        fx (- x ix) fz (- z iz)
        u (* fx fx (- 3 (* 2 fx)))
        v (* fz fz (- 3 (* 2 fz)))
        scale inv-modulus
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

(def min-height 4)
(def max-height 60)

(defn fbm-signed-2d
  ``fBm centred on zero, in [-1,1].

  Each octave is converted to [-1,1] BEFORE it is summed, so the total is
  symmetric about zero by construction. Deriving a signed value afterwards
  instead -- (fbm - measured-mean) / measured-spread -- requires hardcoded
  distribution constants, and if the divisor is even slightly small the result
  saturates against its clamp across most of the map. That failure is quiet
  and severe: it flattens the terrain AND erases the seed, because every
  sample pins to the same clamped extreme.``
  [seed x z octaves lacunarity gain]
  (var total 0.0)
  (var weight 0.0)
  (var amplitude 1.0)
  (var frequency 1.0)
  (for i 0 octaves
    (def n (value-noise-2d (+ seed (* i 1013)) (* x frequency) (* z frequency)))
    (set total (+ total (* amplitude (- (* 2 n) 1))))
    (set weight (+ weight amplitude))
    (set amplitude (* amplitude gain))
    (set frequency (* frequency lacunarity)))
  (if (> weight 0) (/ total weight) 0.0))

(defn- shape
  ``Push a signed [-1,1] value out toward its extremes.

  Summing octaves averages them, so by the central limit theorem the result
  clusters around the middle and terrain comes out as a shallow plain at one
  elevation. Raising the magnitude to a power below one widens the tails,
  turning a uniform mid-height sheet into decisive highlands and basins.``
  [t exponent]
  (def s (math/pow (math/abs t) exponent))
  (if (neg? t) (- s) s))

# Feature sizes are chosen against the visible world, not in the abstract: a
# continent scale far larger than the render distance puts the whole map
# inside a single noise cell, so every chunk sits at one elevation and the
# player sees an endless flat sheet.
(def continent-scale 40.0)
(def continent-amplitude 20)
(def hill-scale 14.0)
(def hill-amplitude 9)
(def base-height (+ sea-level 3))

(defn height-at [seed wx wz]
  # Two scales with different jobs: a low-frequency continent field decides
  # land against ocean, and a higher-frequency field puts hills on top of
  # whatever the continent field chose.
  (def continent
    (shape (fbm-signed-2d seed (/ wx continent-scale) (/ wz continent-scale)
                          3 2.0 0.5)
           0.75))
  (def hills
    (fbm-signed-2d (+ seed 7777) (/ wx hill-scale) (/ wz hill-scale)
                   4 2.0 0.5))
  (def h (math/floor (+ base-height
                        (* continent continent-amplitude)
                        (* hills hill-amplitude))))
  (if (< h min-height) min-height (if (> h max-height) max-height h)))

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
