# n-body: Jovian planets, symplectic integrator, dt=0.01 (benchmarksgame).
# Usage: bender nbody.janet N  (N=1000 correctness, N=50000000 perf)
# SoA layout, arrays threaded as params so Bender can kernelize advance.

(def PI 3.141592653589793)
(def SOLAR-MASS (* 4.0 PI PI))
(def DAYS-PER-YEAR 365.24)
(def NB 5) # kept for readers; hot fns use the literal (C #define style)
(def DT 0.01)

# [x y z vx vy vz mass], velocities scaled by days-per-year exactly
# like the reference C (same operation order → identical doubles).
(def init-data
  @[@[0.0 0.0 0.0
      0.0 0.0 0.0
      SOLAR-MASS]
    @[4.8414314424647209 -1.16032004402742839 -0.103622044471123109
      (* 1.66007664274403694e-3 DAYS-PER-YEAR)
      (* 7.69901118419740425e-3 DAYS-PER-YEAR)
      (* -6.90460016972063023e-5 DAYS-PER-YEAR)
      (* 9.54791938424326609e-4 SOLAR-MASS)]
    @[8.34336671824457987 4.12479856412430479 -0.403523417114321381
      (* -2.76742510726862411e-3 DAYS-PER-YEAR)
      (* 4.99852801234917238e-3 DAYS-PER-YEAR)
      (* 2.30417297573763929e-5 DAYS-PER-YEAR)
      (* 2.85885980666130812e-4 SOLAR-MASS)]
    @[12.894369562139131 -15.1111514016986312 -0.223307578892655734
      (* 2.96460137564761618e-3 DAYS-PER-YEAR)
      (* 2.37847173959480950e-3 DAYS-PER-YEAR)
      (* -2.96589568540237556e-5 DAYS-PER-YEAR)
      (* 4.36624404335156298e-5 SOLAR-MASS)]
    @[15.3796971148509165 -25.9193146099879641 0.179258772950371181
      (* 2.68067772490389322e-3 DAYS-PER-YEAR)
      (* 1.62824170038242295e-3 DAYS-PER-YEAR)
      (* -9.51592254519715870e-5 DAYS-PER-YEAR)
      (* 5.15138902046611451e-5 SOLAR-MASS)]])

(defn make-vec [k]
  (def a (array/new-filled NB 0.0))
  (for i 0 NB (put a i ((init-data i) k)))
  a)

(def xs (make-vec 0))
(def ys (make-vec 1))
(def zs (make-vec 2))
(def vxs (make-vec 3))
(def vys (make-vec 4))
(def vzs (make-vec 5))
(def ms (make-vec 6))

(defn offset-momentum {:hint {:params [:array :array :array :array] :returns :array}}
  [vxs vys vzs ms]
  (var px 0.0) (var py 0.0) (var pz 0.0)
  (for i 0 5
    (+= px (* (vxs i) (ms i)))
    (+= py (* (vys i) (ms i)))
    (+= pz (* (vzs i) (ms i))))
  (put vxs 0 (- (/ px SOLAR-MASS)))
  (put vys 0 (- (/ py SOLAR-MASS)))
  (put vzs 0 (- (/ pz SOLAR-MASS)))
  vxs)

(defn advance
  {:hint {:params [:double :array :array :array :array :array :array :array]
          :returns :array}}
  [dt xs ys zs vxs vys vzs ms]
  (for i 0 5
    (for j (+ i 1) 5
      (def dx (- (xs i) (xs j)))
      (def dy (- (ys i) (ys j)))
      (def dz (- (zs i) (zs j)))
      (def dsq (+ (* dx dx) (* dy dy) (* dz dz)))
      (def mag (/ dt (* dsq (math/sqrt dsq))))
      (put vxs i (- (vxs i) (* dx (ms j) mag)))
      (put vys i (- (vys i) (* dy (ms j) mag)))
      (put vzs i (- (vzs i) (* dz (ms j) mag)))
      (put vxs j (+ (vxs j) (* dx (ms i) mag)))
      (put vys j (+ (vys j) (* dy (ms i) mag)))
      (put vzs j (+ (vzs j) (* dz (ms i) mag)))))
  (for i 0 5
    (put xs i (+ (xs i) (* dt (vxs i))))
    (put ys i (+ (ys i) (* dt (vys i))))
    (put zs i (+ (zs i) (* dt (vzs i)))))
  xs)

(defn energy
  {:hint {:params [:array :array :array :array :array :array :array]
          :returns :double}}
  [xs ys zs vxs vys vzs ms]
  (var e 0.0)
  (for i 0 5
    (+= e (* 0.5 (ms i)
              (+ (* (vxs i) (vxs i))
                 (* (vys i) (vys i))
                 (* (vzs i) (vzs i)))))
    (for j (+ i 1) 5
      (def dx (- (xs i) (xs j)))
      (def dy (- (ys i) (ys j)))
      (def dz (- (zs i) (zs j)))
      (-= e (/ (* (ms i) (ms j)) (math/sqrt (+ (* dx dx) (* dy dy) (* dz dz)))))))
  e)

(defn main [&]
  (def n (scan-number (get (dyn :args) 1 "1000")))
  (offset-momentum vxs vys vzs ms)
  (print (string/format "%.9f" (energy xs ys zs vxs vys vzs ms)))
  (for step 0 n (advance DT xs ys zs vxs vys vzs ms))
  (print (string/format "%.9f" (energy xs ys zs vxs vys vzs ms))))

# top-level call runs in AOT images; os/exit beats the CLI auto-call
(os/exit (main))
