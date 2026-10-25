# Trinecraft chunk and world block storage.

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

(defn block-solid? [id] (not (or (= id block-air) (= id block-water))))
(defn block-transparent? [id] (or (= id block-air) (= id block-water)))
(defn block-name [id]
  (get [:air :stone :dirt :grass :sand :water :wood :leaves] id))

# X varies fastest, then Z, then Y; one arithmetic expression, no allocation.
(defn block-index [x y z] (+ x (* 16 (+ z (* 16 y)))))
(defn in-chunk-bounds? [x y z]
  (and (<= 0 x) (< x chunk-x)
       (<= 0 y) (< y chunk-y)
       (<= 0 z) (< z chunk-z)))

(defn chunk-new [cx cz]
  @{:cx cx :cz cz :blocks (buffer/new-filled chunk-volume 0)
    :dirty true :mesh nil})
(defn chunk-get [ch x y z]
  (if (in-chunk-bounds? x y z)
    (get (get ch :blocks) (block-index x y z))
    block-air))
(defn chunk-set [ch x y z id]
  (when (in-chunk-bounds? x y z)
    (put (get ch :blocks) (block-index x y z) id)
    (put ch :dirty true)))
(defn chunk-fill [ch id]
  (buffer/fill (get ch :blocks) id)
  (put ch :dirty true))
(defn chunk-empty? [ch]
  (var empty true)
  (for i 0 chunk-volume
    (when (not= (get (get ch :blocks) i) block-air)
      (set empty false)))
  empty)

(defn world-new [] @{:chunks @{} :seed 0})
# Bender compares tuple keys by value, including independently constructed tuples.
(defn chunk-key [cx cz] [cx cz])
(defn world-get-chunk [w cx cz]
  (get (get w :chunks) (chunk-key cx cz)))
(defn world-put-chunk [w ch]
  (put (get w :chunks) (chunk-key (get ch :cx) (get ch :cz)) ch)
  ch)
(defn world-remove-chunk [w cx cz]
  (put (get w :chunks) (chunk-key cx cz) nil))
(defn world-chunk-count [w]
  (length (get w :chunks)))

# Floor division rather than truncation keeps negative world positions in their
# preceding chunk; subtracting its origin yields a nonnegative local coordinate.
(defn world-to-chunk-coord [wx] (math/floor (/ wx chunk-x)))
(defn world-to-local-coord [wx]
  (- wx (* chunk-x (world-to-chunk-coord wx))))
(defn world-get-block [w wx wy wz]
  (if (and (<= 0 wy) (< wy chunk-y))
    (let [ch (world-get-chunk w (world-to-chunk-coord wx) (world-to-chunk-coord wz))]
      (if ch
        (chunk-get ch (world-to-local-coord wx) wy (world-to-local-coord wz))
        block-air))
    block-air))
(defn world-set-block [w wx wy wz id]
  (let [cx (world-to-chunk-coord wx)
        cz (world-to-chunk-coord wz)
        ch (world-get-chunk w cx cz)]
    (if (and ch (<= 0 wy) (< wy chunk-y))
      (let [x (world-to-local-coord wx)
            z (world-to-local-coord wz)]
        (chunk-set ch x wy z id)
        (when (= x 0)
          (let [other (world-get-chunk w (- cx 1) cz)]
            (when other (put other :dirty true))))
        (when (= x 15)
          (let [other (world-get-chunk w (+ cx 1) cz)]
            (when other (put other :dirty true))))
        (when (= z 0)
          (let [other (world-get-chunk w cx (- cz 1))]
            (when other (put other :dirty true))))
        (when (= z 15)
          (let [other (world-get-chunk w cx (+ cz 1))]
            (when other (put other :dirty true))))
        true)
      false)))
