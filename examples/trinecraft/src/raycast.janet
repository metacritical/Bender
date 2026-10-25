# Amanatides-Woo traversal in world-space distance units. Ties step one axis
# at a time (X, then Y, then Z), so every touched voxel is considered.
(import ./world)

(defn raycast [w ox oy oz dx dy dz max-distance &opt solid-pred]
  (let [length (math/sqrt (+ (* dx dx) (* dy dy) (* dz dz)))
        pred (or solid-pred world/block-solid?)]
    (when (and (> length 0) (>= max-distance 0))
      (var x (math/floor ox))
      (var y (math/floor oy))
      (var z (math/floor oz))
      (var block (world/world-get-block w x y z))
      (if (pred block)
        @{:x x :y y :z z :block block :face nil
          :px x :py y :pz z :distance 0}
        (let [ux (/ dx length) uy (/ dy length) uz (/ dz length)
              sx (if (> dx 0) 1 (if (< dx 0) -1 0))
              sy (if (> dy 0) 1 (if (< dy 0) -1 0))
              sz (if (> dz 0) 1 (if (< dz 0) -1 0))]
          # nil means infinity; explicitly skip stationary axes without division.
          (var tx (if (= sx 0) nil (/ (- (if (> sx 0) (+ x 1) x) ox) ux)))
          (var ty (if (= sy 0) nil (/ (- (if (> sy 0) (+ y 1) y) oy) uy)))
          (var tz (if (= sz 0) nil (/ (- (if (> sz 0) (+ z 1) z) oz) uz)))
          (let [dtx (if (= sx 0) nil (/ 1 (math/abs ux)))
                dty (if (= sy 0) nil (/ 1 (math/abs uy)))
                dtz (if (= sz 0) nil (/ 1 (math/abs uz)))]
            (var distance 0)
            (var face nil)
            (var px x)
            (var py y)
            (var pz z)
            (var hit nil)
            (var done false)
            (while (not done)
              (set px x)
              (set py y)
              (set pz z)
              (cond
                (and tx (or (nil? ty) (<= tx ty)) (or (nil? tz) (<= tx tz)))
                (do (set distance tx) (set tx (+ tx dtx)) (set x (+ x sx))
                    (set face (if (> sx 0) 3 2)))
                (and ty (or (nil? tz) (<= ty tz)))
                (do (set distance ty) (set ty (+ ty dty)) (set y (+ y sy))
                    (set face (if (> sy 0) 5 4)))
                tz
                (do (set distance tz) (set tz (+ tz dtz)) (set z (+ z sz))
                    (set face (if (> sz 0) 1 0)))
                (set done true))
              (when (and (not done) (> distance max-distance))
                (set done true))
              (when (not done)
                (set block (world/world-get-block w x y z))
                (when (pred block)
                  (set hit @{:x x :y y :z z :block block :face face
                             :px px :py py :pz pz :distance distance})
                  (set done true))))
            hit))))))

(defn raycast-first-solid [w ox oy oz dx dy dz max-distance]
  (raycast w ox oy oz dx dy dz max-distance))
