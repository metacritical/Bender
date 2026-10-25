# Headless contract tests for the procedural Trinecraft atlas.
(import ../src/atlas :as atlas)
(import ../src/rl :as rl)

(defn near [a b] (< (math/abs (- a b)) 0.000001))

(for block 1 8
  (for face 0 6
    (let [tile (atlas/tile-for block face)]
      (assert (and (<= 0 tile) (< tile 16))))))
(print "tile indices: pass")

(let [top (atlas/tile-for 3 4)
      side (atlas/tile-for 3 0)
      bottom (atlas/tile-for 3 5)]
  (assert (and (not= top side) (not= side bottom) (not= top bottom)))
  (for face 0 4 (assert (= side (atlas/tile-for 3 face)))))
(print "grass faces: pass")

(for block 1 8
  (for face 0 6
    (let [rect (atlas/face-uv block face)]
      (assert (= 4 (length rect)))
      (each n rect (assert (and (number? n) (<= 0 n) (<= n 1))))
      (assert (< (get rect 0) (get rect 2)))
      (assert (< (get rect 1) (get rect 3))))))
(print "UV bounds: pass")

(let [tiles @{}]
  (for block 1 8
    (for face 0 6
      (let [tile (atlas/tile-for block face)
            rect (atlas/face-uv block face)]
        (if (has-key? tiles tile)
          (assert (= rect (get tiles tile)))
          (put tiles tile rect)))))
  (each [a ra] (pairs tiles)
    (each [b rb] (pairs tiles)
      (when (not= a b) (assert (not= ra rb))))))
(print "tile-to-UV mapping: pass")

(for block 1 8
  (for face 0 6
    (let [tile (atlas/tile-for block face)
          rect (atlas/face-uv block face)
          col (mod tile atlas/atlas-cols)
          row (math/floor (/ tile atlas/atlas-cols))]
      (assert (near (- (get rect 2) (get rect 0)) (/ 1 atlas/atlas-cols)))
      (assert (near (- (get rect 3) (get rect 1)) (/ 1 atlas/atlas-rows)))
      (assert (near (get rect 0) (/ col atlas/atlas-cols)))
      (assert (near (get rect 1) (/ row atlas/atlas-rows))))))
(print "atlas grid alignment: pass")

(let [rect (atlas/face-uv 1 0)
      shrunk (atlas/inset-uv rect 1)
      extreme (atlas/inset-uv rect 100)]
  (assert (near (- (get shrunk 0) (get rect 0)) (/ 1 64)))
  (assert (near (- (get shrunk 1) (get rect 1)) (/ 1 64)))
  (assert (near (- (get rect 2) (get shrunk 2)) (/ 1 64)))
  (assert (near (- (get rect 3) (get shrunk 3)) (/ 1 64)))
  (assert (<= (get extreme 0) (get extreme 2)))
  (assert (<= (get extreme 1) (get extreme 3))))
(print "UV inset: pass")

# Raylib image generation and drawing are CPU-side; no window is opened.
(let [image (atlas/build-atlas-image)
      offsets (map |($ :offset) ((ffi/struct-info rl/image-type) :fields))]
  (assert (= 64 (ffi/read :int image (get offsets 1))))
  (assert (= 64 (ffi/read :int image (get offsets 2))))
  (rl/unload-image (ffi/read rl/image-type image)))
(print "headless image dimensions: pass")

(let [started (os/clock)]
  (for i 0 200000 (atlas/face-uv (+ 1 (mod i 7)) (mod i 6)))
  (print "face-uv 200000 calls: " (- (os/clock) started) " s"))
(print "atlas tests: pass")
