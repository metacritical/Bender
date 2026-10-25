# Procedural 4x4 block atlas. Image construction is CPU-only; upload needs GL.
(import ./rl :as rl)
(import ./world :as world)

(def atlas-cols 4)
(def atlas-rows 4)
(def tile-pixels 16)
(def atlas-width (* atlas-cols tile-pixels))
(def atlas-height (* atlas-rows tile-pixels))

# Tile 0 is reserved for air; grass occupies three separate slots.
(defn tile-for [block-id face]
  (case block-id
    1 1 # stone
    2 2 # dirt
    3 (if (= face 4) 3 (if (= face 5) 2 4))
    4 5 # sand
    5 6 # water
    6 7 # wood
    7 8 # leaves
    0))

# Tuple literals are created once, not in the mesher's face loop.
(def tile-rects
  (map (fn [i]
         (let [x (mod i atlas-cols) y (math/floor (/ i atlas-cols))]
           [(/ x atlas-cols) (/ y atlas-rows)
            (/ (+ x 1) atlas-cols) (/ (+ y 1) atlas-rows)]))
       (range (* atlas-cols atlas-rows))))

(def face-rects
  (map (fn [block]
         (map (fn [face] (get tile-rects (tile-for block face)))
              (range 6)))
       (range 8)))

(defn face-uv [block-id face]
  (get (get face-rects block-id) face))

(defn inset-uv [rect px]
  (let [du (/ px atlas-width) dv (/ px atlas-height)
        u0 (get rect 0) v0 (get rect 1)
        u1 (get rect 2) v1 (get rect 3)]
    [(min (+ u0 du) (/ (+ u0 u1) 2))
     (min (+ v0 dv) (/ (+ v0 v1) 2))
     (max (- u1 du) (/ (+ u0 u1) 2))
     (max (- v1 dv) (/ (+ v0 v1) 2))]))

(def image-offsets (map |($ :offset) ((ffi/struct-info rl/image-type) :fields)))

(defn speckle [x y seed]
  (mod (+ (* (+ x seed) 73856093) (* (+ y seed) 19349663)
          (* (+ x seed) (+ y seed) 83492791)) 17))

(defn paint-tile [image tile base dark light kind]
  (let [ox (* (mod tile atlas-cols) tile-pixels)
        oy (* (math/floor (/ tile atlas-cols)) tile-pixels)]
    (rl/image-draw-rectangle image ox oy tile-pixels tile-pixels base)
    (for y 0 tile-pixels
      (for x 0 tile-pixels
        (let [n (speckle x y tile)
              color (cond
                      (and (= kind :grass-side) (< y 4))
                        (if (< n 4) [51 140 47 255] [80 168 62 255])
                      (and (= kind :wood) (= (mod (+ x (math/floor (/ y 5))) 5) 0)) dark
                      (and (= kind :grass-side) (= y 4)) dark
                      (< n 2) dark
                      (> n 14) light)]
          (when color (rl/image-draw-rectangle image (+ ox x) (+ oy y) 1 1 color)))))))

(defn build-atlas-image []
  (let [raw (rl/gen-image-color atlas-width atlas-height [0 0 0 0])
        image (buffer/new-filled (ffi/size rl/image-type) 0)]
    # raylib's ImageDrawRectangle takes Image*, unlike the by-value upload APIs.
    (each i (range 5)
      (ffi/write (get [:ptr :int :int :int :int] i) (get raw i) image (get image-offsets i)))
    (paint-tile image 1 [126 128 130 255] [93 97 100 255] [158 160 157 255] :stone)
    (paint-tile image 2 [125 85 52 255] [94 60 38 255] [153 108 68 255] :dirt)
    (paint-tile image 3 [83 159 59 255] [49 123 43 255] [111 184 72 255] :grass)
    (paint-tile image 4 [125 85 52 255] [95 64 39 255] [157 111 71 255] :grass-side)
    (paint-tile image 5 [221 207 145 255] [191 174 114 255] [240 227 173 255] :sand)
    (paint-tile image 6 [55 116 190 255] [36 88 156 255] [85 151 211 255] :water)
    (paint-tile image 7 [136 94 51 255] [90 56 29 255] [163 119 67 255] :wood)
    (paint-tile image 8 [48 113 48 255] [30 79 36 255] [69 141 57 255] :leaves)
    image))

(defn load-atlas-texture []
  # This function alone requires an initialized raylib window/GL context.
  (let [image (build-atlas-image)
        value (ffi/read rl/image-type image)
        texture (rl/load-texture-from-image value)]
    (rl/set-texture-filter texture rl/texture-filter-point)
    (rl/unload-image value)
    texture))
