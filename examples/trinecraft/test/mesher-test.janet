# Headless mesher tests: correctness of face culling, buffer sizing and seam
# handling, plus a throughput benchmark. No window or GPU required.
# Run: ./build/bender examples/trinecraft/test/mesher-test.janet

(import ./../src/world :as world)
(import ./../src/worldgen :as worldgen)
(import ./../src/mesher :as mesher)

(var failures 0)
(defn check [name ok &opt detail]
  (if ok
    (printf "  ok   %s" name)
    (do (++ failures)
        (printf "  FAIL %s%s" name (if detail (string " -- " detail) "")))))

(defn read-f32 [buf i] (ffi/read :float buf (* i 4)))
(defn read-u16 [buf i] (ffi/read :uint16 buf (* i 2)))

(def m (mesher/mesher-new))

#
# --- Culling correctness ------------------------------------------------
#
(print "face culling:")

# Empty chunk: nothing at all.
(def empty-ch (world/chunk-new 0 0))
(check "empty chunk emits no mesh" (nil? (mesher/mesh-chunk m empty-ch)))
(check "empty chunk counts zero faces" (zero? (mesher/count-faces empty-ch)))

# A single isolated block has all 6 faces exposed.
(def one-ch (world/chunk-new 0 0))
(world/chunk-set one-ch 5 5 5 world/block-stone)
(def one (mesher/mesh-chunk m one-ch))
(check "lone block emits 6 faces" (= 6 (get one :face-count))
       (string "got " (get one :face-count)))
(check "lone block emits 24 vertices" (= 24 (get one :vertex-count)))
(check "lone block emits 12 triangles" (= 12 (get one :triangle-count)))

# Two adjacent blocks hide the shared pair of faces: 12 - 2 = 10.
(def two-ch (world/chunk-new 0 0))
(world/chunk-set two-ch 5 5 5 world/block-stone)
(world/chunk-set two-ch 6 5 5 world/block-stone)
(def two (mesher/mesh-chunk m two-ch))
(check "adjacent pair hides the shared faces" (= 10 (get two :face-count))
       (string "got " (get two :face-count)))

# A fully solid chunk is sealed except for its outer shell. With no world
# given, seams read as air, so every border face is exposed:
#   2*(16*64) east/west + 2*(16*64) north/south + 2*(16*16) top/bottom
# but the bottom sits at y=0, whose neighbour is treated as stone (bedrock),
# so the underside is culled.
(def solid-ch (world/chunk-new 0 0))
(world/chunk-fill solid-ch world/block-stone)
(def expected-shell (+ (* 4 16 64) (* 1 16 16)))
(def solid-faces (mesher/count-faces solid-ch))
(check "solid chunk meshes only its shell, underside culled"
       (= expected-shell solid-faces)
       (string "want " expected-shell " got " solid-faces))

# Interior is genuinely free: a solid chunk must not emit ~98k faces.
(check "solid chunk emits far fewer faces than it has block faces"
       (< solid-faces (* 0.1 6 world/chunk-volume)))

#
# --- Water rules --------------------------------------------------------
#
(print "\nwater and transparency:")

# Water against water emits nothing; water's top against air emits one face.
(def wch (world/chunk-new 0 0))
(world/chunk-set wch 3 3 3 world/block-water)
(world/chunk-set wch 3 4 3 world/block-water)
(def wfaces (mesher/count-faces wch))
(check "stacked water does not mesh its internal boundary" (= 10 wfaces)
       (string "got " wfaces))

# A solid block submerged in water still draws, because water is transparent.
(def sch (world/chunk-new 0 0))
(world/chunk-set sch 3 3 3 world/block-water)
(world/chunk-set sch 3 4 3 world/block-stone)
(def sfaces (mesher/count-faces sch))
# The stone's underside IS drawn, because water is see-through. The water's
# top face against the stone is NOT, because the stone hides it. So the pair
# emits 6 (stone) + 5 (water) = 11, not 12.
(check "solid against water meshes the solid's face only" (= 11 sfaces)
       (string "got " sfaces))

#
# --- Buffer shape -------------------------------------------------------
#
(print "\nbuffer sizing and contents:")
(def vc (get one :vertex-count))
(check "verts buffer is 3 floats per vertex"
       (= (length (get one :verts)) (* vc 3 4)))
(check "norms buffer is 3 floats per vertex"
       (= (length (get one :norms)) (* vc 3 4)))
(check "uvs buffer is 2 floats per vertex"
       (= (length (get one :uvs)) (* vc 2 4)))
(check "colors buffer is 4 bytes per vertex"
       (= (length (get one :colors)) (* vc 4)))
(check "idxs buffer is 6 uint16 per face"
       (= (length (get one :idxs)) (* (get one :face-count) 6 2)))

# Every index must address a real vertex.
(var idx-ok true)
(def nidx (/ (length (get one :idxs)) 2))
(for i 0 nidx
  (def v (read-u16 (get one :idxs) i))
  (when (or (< v 0) (>= v vc)) (set idx-ok false)))
(check "every index is within the vertex range" idx-ok)

# Positions must stay inside the chunk's local box.
(var pos-ok true)
(for i 0 (* vc 3)
  (def v (read-f32 (get one :verts) i))
  (when (or (< v -0.001) (> v 65.0)) (set pos-ok false)))
(check "all positions lie inside the chunk box" pos-ok)

# Normals are unit axis vectors.
(var norm-ok true)
(for v 0 vc
  (def nx (read-f32 (get one :norms) (* v 3)))
  (def ny (read-f32 (get one :norms) (+ (* v 3) 1)))
  (def nz (read-f32 (get one :norms) (+ (* v 3) 2)))
  (def len (+ (* nx nx) (* ny ny) (* nz nz)))
  (when (> (math/abs (- len 1.0)) 0.001) (set norm-ok false)))
(check "all normals are unit length" norm-ok)

# The six faces of a lone block must have six distinct normals.
(def seen @{})
(for f 0 6
  (def v (* f 4))
  (put seen [(read-f32 (get one :norms) (* v 3))
             (read-f32 (get one :norms) (+ (* v 3) 1))
             (read-f32 (get one :norms) (+ (* v 3) 2))] true))
(check "lone block has 6 distinct face normals" (= 6 (length seen))
       (string "got " (length seen)))

#
# --- UV plumbing --------------------------------------------------------
#
(print "\nuv mapping:")
(def uv-ch (world/chunk-new 0 0))
(world/chunk-set uv-ch 1 1 1 world/block-grass)
# Feed a stub atlas that gives each face a distinct quarter rect.
(defn stub-uv [id face]
  (def col (% face 2))
  (def row (math/floor (/ face 2)))
  [(* col 0.5) (* row 0.25) (+ (* col 0.5) 0.5) (+ (* row 0.25) 0.25)])
(def uvm (mesher/mesh-chunk m uv-ch nil stub-uv))
(var uv-in-range true)
(for i 0 (* (get uvm :vertex-count) 2)
  (def v (read-f32 (get uvm :uvs) i))
  (when (or (< v -0.001) (> v 1.001)) (set uv-in-range false)))
(check "uv-fn output stays within 0..1" uv-in-range)
# With no uv-fn the mesher emits a unit rect, so corners must hit 0 and 1.
(var saw-zero false)
(var saw-one false)
(for i 0 (* (get one :vertex-count) 2)
  (def v (read-f32 (get one :uvs) i))
  (when (< (math/abs v) 0.001) (set saw-zero true))
  (when (< (math/abs (- v 1)) 0.001) (set saw-one true)))
(check "default uvs span the unit rect" (and saw-zero saw-one))

#
# --- Cross-chunk seams --------------------------------------------------
#
(print "\ncross-chunk seams:")
(def w (world/world-new))
(def a (world/chunk-new 0 0))
(def b (world/chunk-new 1 0))
(world/chunk-fill a world/block-stone)
(world/chunk-fill b world/block-stone)
(world/world-put-chunk w a)
(world/world-put-chunk w b)
(def lone-shell (mesher/count-faces a))
(def joined-shell (mesher/count-faces a w))
# With chunk 1 present to the east, chunk 0's entire east wall is hidden.
(check "neighbouring chunk culls the shared wall"
       (= joined-shell (- lone-shell (* 16 64)))
       (string "lone " lone-shell " joined " joined-shell))
(check "seam culling strictly reduces face count" (< joined-shell lone-shell))

# Without the world, the same chunk keeps its wall.
(check "absent neighbour leaves the wall exposed"
       (> (mesher/count-faces a) (mesher/count-faces a w)))

#
# --- Realistic terrain + throughput ------------------------------------
#
(print "\ngenerated terrain:")
(def gw (world/world-new))
(def seed 1337)
(for cz 0 3
  (for cx 0 3
    (worldgen/generate-chunk gw cx cz seed)))
(def centre (world/world-get-chunk gw 1 1))
(def tm (mesher/mesh-chunk m centre gw))
(check "generated chunk produces geometry" (not (nil? tm)))
(check "generated chunk was not truncated" (not (get tm :truncated)))
(check "generated chunk stays under the uint16 vertex cap"
       (< (get tm :vertex-count) 65536)
       (string "got " (get tm :vertex-count)))
(printf "  info terrain chunk: %d faces, %d verts, %d KB of vertex data"
        (get tm :face-count) (get tm :vertex-count)
        (math/floor (/ (+ (length (get tm :verts)) (length (get tm :uvs))
                          (length (get tm :norms)) (length (get tm :colors))
                          (length (get tm :idxs))) 1024)))

# Culling must actually pay off on real terrain.
(def naive (* 6 (do (var n 0)
                    (for i 0 world/chunk-volume
                      (when (not= 0 (get (get centre :blocks) i)) (++ n)))
                    n)))
(printf "  info culling: %d faces emitted vs %d unculled (%.1f%% kept)"
        (get tm :face-count) naive
        (* 100 (/ (get tm :face-count) (max naive 1))))
(check "culling discards most faces on real terrain"
       (< (get tm :face-count) (* 0.25 naive)))

(print "\nthroughput:")
(def reps 20)
(def t0 (os/clock))
(for i 0 reps (mesher/mesh-chunk m centre gw))
(def t1 (os/clock))
(def per-chunk (* 1000 (/ (- t1 t0) reps)))
(printf "  info mesh-chunk: %.2f ms per chunk (%d reps)" per-chunk reps)
(printf "  info budget: %.1f chunks per 16.6 ms frame" (/ 16.6 per-chunk))

(printf "\n%s  (%d failure%s)"
        (if (zero? failures) "ALL CHECKS PASSED" "CHECKS FAILED")
        failures (if (= 1 failures) "" "s"))
(os/exit (if (zero? failures) 0 1))
