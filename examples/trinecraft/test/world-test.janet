(import ../src/world)

(defn check-index []
  (def seen @{})
  (for y 0 world/chunk-y
    (for z 0 world/chunk-z
      (for x 0 world/chunk-x
        (let [i (world/block-index x y z)]
          (assert (and (<= 0 i) (< i world/chunk-volume)))
          (assert (nil? (get seen i)))
          (put seen i true)
          (assert (= x (mod i 16)))
          (assert (= z (mod (math/floor (/ i 16)) 16)))
          (assert (= y (math/floor (/ i 256))))))))
  (assert (= world/chunk-volume (length seen)))
  (print "block indexing: passed"))

(defn check-blocks-and-bounds []
  (assert (= 16 world/chunk-x))
  (assert (= 64 world/chunk-y))
  (assert (= 16 world/chunk-z))
  (assert (= 16384 world/chunk-volume))
  (each [id name] [[0 :air] [1 :stone] [2 :dirt] [3 :grass]
                   [4 :sand] [5 :water] [6 :wood] [7 :leaves]]
    (assert (= name (world/block-name id))))
  (assert (world/block-transparent? world/block-air))
  (assert (world/block-transparent? world/block-water))
  (assert (not (world/block-solid? world/block-air)))
  (assert (not (world/block-solid? world/block-water)))
  (for id 1 8
    (when (not= id world/block-water)
      (assert (world/block-solid? id))
      (assert (not (world/block-transparent? id)))))
  (assert (world/in-chunk-bounds? 15 63 15))
  (each coords [[-1 0 0] [16 0 0] [0 -1 0] [0 64 0]
                [0 0 -1] [0 0 16]]
    (assert (not (apply world/in-chunk-bounds? coords))))
  (print "blocks and bounds: passed"))

(defn check-chunk []
  (def ch (world/chunk-new 2 -3))
  (assert (= 2 (get ch :cx)))
  (assert (= -3 (get ch :cz)))
  (assert (= world/chunk-volume (length (get ch :blocks))))
  (assert (get ch :dirty))
  (assert (nil? (get ch :mesh)))
  (assert (world/chunk-empty? ch))
  (put ch :dirty false)
  (world/chunk-set ch 3 4 5 world/block-stone)
  (assert (get ch :dirty))
  (assert (= world/block-stone (world/chunk-get ch 3 4 5)))
  (assert (not (world/chunk-empty? ch)))
  (put ch :dirty false)
  (each coords [[-1 4 5] [16 4 5] [3 -1 5] [3 64 5]
                [3 4 -1] [3 4 16]]
    (assert (= world/block-air (apply world/chunk-get (array ch ;coords))))
    (apply world/chunk-set (array ch ;coords world/block-dirt)))
  (assert (not (get ch :dirty)))
  (assert (= world/block-stone (world/chunk-get ch 3 4 5)))
  (world/chunk-fill ch world/block-sand)
  (assert (get ch :dirty))
  (assert (= world/block-sand (world/chunk-get ch 15 63 15)))
  (assert (not (world/chunk-empty? ch)))
  (world/chunk-fill ch world/block-air)
  (assert (world/chunk-empty? ch))
  (print "chunk storage: passed"))

(defn check-coordinates []
  (each [position cx local] [[-17 -2 15] [-16 -1 0] [-1 -1 15]
                             [0 0 0] [15 0 15] [16 1 0] [31 1 15]]
    (assert (= cx (world/world-to-chunk-coord position)))
    (assert (= local (world/world-to-local-coord position))))
  (print "world coordinates: passed"))

(defn check-world []
  (def w (world/world-new))
  (assert (= 0 (get w :seed)))
  (assert (= 0 (world/world-chunk-count w)))
  (each [cx cz] [[-2 0] [-1 -1] [-1 0] [0 -1] [0 0] [1 0] [0 1]]
    (def ch (world/chunk-new cx cz))
    (assert (= ch (world/world-put-chunk w ch)))
    (assert (= ch (world/world-get-chunk w cx cz))))
  (assert (= 7 (world/world-chunk-count w)))
  (each [x y z id] [[-17 3 1 1] [-16 4 2 2] [-1 5 3 3]
                     [0 6 4 4] [15 7 5 5] [16 8 6 6]]
    (assert (world/world-set-block w x y z id))
    (assert (= id (world/world-get-block w x y z))))
  (assert (= world/block-air (world/world-get-block w 32 0 0)))
  (assert (= world/block-air (world/world-get-block w 0 -1 0)))
  (assert (= world/block-air (world/world-get-block w 0 64 0)))
  (assert (not (world/world-set-block w 32 0 0 1)))
  (assert (not (world/world-set-block w 0 64 0 1)))
  (def center (world/world-get-chunk w 0 0))
  (def west (world/world-get-chunk w -1 0))
  (def east (world/world-get-chunk w 1 0))
  (def north (world/world-get-chunk w 0 -1))
  (def south (world/world-get-chunk w 0 1))
  (each neighbour [west east north south] (put neighbour :dirty false))
  (world/world-set-block w 0 2 8 1)
  (assert (get west :dirty))
  (each neighbour [west east north south] (put neighbour :dirty false))
  (world/world-set-block w 15 2 8 1)
  (assert (get east :dirty))
  (each neighbour [west east north south] (put neighbour :dirty false))
  (world/world-set-block w 8 2 0 1)
  (assert (get north :dirty))
  (each neighbour [west east north south] (put neighbour :dirty false))
  (world/world-set-block w 8 2 15 1)
  (assert (get south :dirty))
  (each neighbour [west east north south] (put neighbour :dirty false))
  (world/world-set-block w 0 2 0 1)
  (assert (and (get west :dirty) (get north :dirty)))
  (assert (not (get east :dirty)))
  (assert (not (get south :dirty)))
  (world/world-remove-chunk w -2 0)
  (assert (nil? (world/world-get-chunk w -2 0)))
  (assert (= 6 (world/world-chunk-count w)))
  (print "world access and seams: passed"))

(check-index)
(check-blocks-and-bounds)
(check-chunk)
(check-coordinates)
(check-world)
(print "world tests: all passed")
