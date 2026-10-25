(import ../src/world)
(import ../src/raycast)

(defn fixture []
  (let [w (world/world-new)]
    (each [cx cz] [[0 0] [-1 0] [0 -1] [-1 -1]]
      (world/world-put-chunk w (world/chunk-new cx cz)))
    w))

(defn place [w x y z id]
  (assert (world/world-set-block w x y z id)))

(defn check-hit [hit x y z face px py pz]
  (assert hit)
  (assert (= [x y z face px py pz]
             [(get hit :x) (get hit :y) (get hit :z) (get hit :face)
              (get hit :px) (get hit :py) (get hit :pz)])))

(defn axes []
  (let [w (fixture)]
    (each [x y z] [[4 20 4] [2 20 4] [3 22 4] [3 18 4]
                   [3 20 5] [3 20 2]]
      (place w x y z world/block-stone))
    (each [dx dy dz x y z face px py pz]
      [[1 0 0 4 20 4 3 3 20 4] [-1 0 0 2 20 4 2 3 20 4]
       [0 1 0 3 22 4 5 3 21 4] [0 -1 0 3 18 4 4 3 19 4]
       [0 0 1 3 20 5 1 3 20 4] [0 0 -1 3 20 2 0 3 20 3]]
      (check-hit (raycast/raycast-first-solid w 3.5 20.5 4.5 dx dy dz 8)
                 x y z face px py pz))
    (print "axis faces and zero components: passed")))

(defn misses-and-range []
  (let [w (fixture)]
    (assert (nil? (raycast/raycast w 1.5 10.5 1.5 0 1 0 8)))
    (place w 8 10 1 world/block-stone)
    (assert (nil? (raycast/raycast w 1.5 10.5 1.5 1 0 0 6.49)))
    (check-hit (raycast/raycast w 1.5 10.5 1.5 1 0 0 6.5)
               8 10 1 3 7 10 1)
    (assert (nil? (raycast/raycast w 1.5 10.5 1.5 0 0 0 100)))
    (print "misses, range and zero vector: passed")))

(defn inside-and-boundaries []
  (let [w (fixture)]
    (place w 0 12 0 world/block-stone)
    (let [hit (raycast/raycast w 0 12.5 0 1 0 0 0)]
      (check-hit hit 0 12 0 nil 0 12 0)
      (assert (= 0 (get hit :distance))))
    (place w 1 13 0 world/block-dirt)
    (check-hit (raycast/raycast w 1 12.5 0 0 1 0 2)
               1 13 0 5 1 12 0)
    (place w -1 11 0 world/block-stone)
    (check-hit (raycast/raycast w 0 11.5 0 -1 0 0 0)
               -1 11 0 2 0 11 0)
    (print "inside and integer boundaries: passed")))

(defn negatives-and-diagonals []
  (let [w (fixture)]
    (place w -3 9 -2 world/block-stone)
    (check-hit (raycast/raycast w -0.5 9.5 -1.5 -2 0 0 8)
               -3 9 -2 2 -2 9 -2)
    # A narrow corner crossing: the first touched cell is only visited
    # between two closely spaced plane crossings, regardless of sample stride.
    (place w 1 15 0 world/block-dirt)
    (check-hit (raycast/raycast w 0.01 15.5 0.01 1 0 0.99 4)
               1 15 0 3 0 15 0)
    (print "negative coordinates and diagonal corner: passed")))

(defn distances []
  (let [w (fixture)]
    (place w 5 8 3 world/block-stone)
    (var previous -1)
    (each limit [3.5 4 8 12]
      (let [hit (raycast/raycast w 1.25 8.5 3.5 7 0 0 limit)]
        (when hit
          (assert (>= (get hit :distance) previous))
          (set previous (get hit :distance)))))
    (assert (nil? (raycast/raycast w 1.25 8.5 3.5 1 0 0 3.74)))
    (let [hit (raycast/raycast w 1.25 8.5 3.5 1 0 0 3.75)]
      (assert (< (math/abs (- (get hit :distance) 3.75)) 0.00001)))
    (print "distance and normalization: passed")))

(defn predicates []
  (let [w (fixture)]
    (place w 2 7 2 world/block-water)
    (place w 3 7 2 world/block-stone)
    (check-hit (raycast/raycast w 1.5 7.5 2.5 1 0 0 4)
               3 7 2 3 2 7 2)
    (let [hit (raycast/raycast w 1.5 7.5 2.5 1 0 0 4
                               (fn [id] (not= id world/block-air)))]
      (check-hit hit 2 7 2 3 1 7 2)
      (assert (= world/block-water (get hit :block))))
    (print "water predicates: passed")))

(defn timing []
  (let [w (fixture)]
    (for x 0 16
      (for z 0 16
        (place w x 3 z world/block-stone)))
    (let [start (os/clock)]
      (for i 0 100000
        (raycast/raycast w 0.5 8.5 0.5 0 -1 0 12))
      (print "100000 raycasts: " (- (os/clock) start) " seconds"))))

(axes)
(misses-and-range)
(inside-and-boundaries)
(negatives-and-diagonals)
(distances)
(predicates)
(timing)
