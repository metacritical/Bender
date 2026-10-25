# Trinecraft — chunk mesher.
#
# Turns a chunk's packed block ids into GPU-ready vertex data. This is the
# performance centre of the engine: it runs for every chunk that enters view
# and again whenever a block changes, so the inner loops are flat arithmetic
# over byte buffers with no closures, no intermediate tuples and no allocation.
#
# The strategy is face culling: a block face is emitted only when the
# neighbouring cell is see-through, so the interior of the terrain -- the
# overwhelming majority of blocks -- costs nothing. A solid 16x64x16 chunk
# emits zero faces rather than 98304.
#
# Scratch buffers live on the mesher and are reused across every chunk. Only
# the final right-sized copies handed to the GPU are freshly allocated, so
# meshing N chunks allocates O(emitted geometry), not O(N * worst case).

(import ./world :as world)

#
# --- Face table --------------------------------------------------------
#
# Face numbering is frozen and shared with the atlas module:
#   0 = +Z south, 1 = -Z north, 2 = +X east, 3 = -X west, 4 = +Y top, 5 = -Y bottom
#
# Corner winding is counter-clockwise viewed from OUTSIDE the block, which is
# what raylib's default backface culling expects. Corner order is
# bottom-left, bottom-right, top-right, top-left in face space, so the atlas
# UV rect maps onto it without a per-face special case.

(def face-count 6)

(def face-normals
  [[0 0 1] [0 0 -1] [1 0 0] [-1 0 0] [0 1 0] [0 -1 0]])

# Neighbour cell offset for each face, used for the culling test.
(def face-offsets
  [[0 0 1] [0 0 -1] [1 0 0] [-1 0 0] [0 1 0] [0 -1 0]])

(def face-corners
  [# +Z south
   [[0 0 1] [1 0 1] [1 1 1] [0 1 1]]
   # -Z north
   [[1 0 0] [0 0 0] [0 1 0] [1 1 0]]
   # +X east
   [[1 0 1] [1 0 0] [1 1 0] [1 1 1]]
   # -X west
   [[0 0 0] [0 0 1] [0 1 1] [0 1 0]]
   # +Y top
   [[0 1 1] [1 1 1] [1 1 0] [0 1 0]]
   # -Y bottom
   [[0 0 0] [1 0 0] [1 0 1] [0 0 1]]])

# Cheap directional shading baked into vertex colour: top faces full bright,
# sides dimmer, bottom darkest. Gives the world readable form with no lighting
# pass and no shader, which keeps the whole renderer to one draw call per chunk.
(def face-shade [0.76 0.76 0.62 0.62 1.0 0.45])

#
# --- Capacity ----------------------------------------------------------
#
# raylib's Mesh.indices is `unsigned short *`, so a single mesh addresses at
# most 65536 vertices = 16384 quads. Real terrain is nowhere near that (a
# chunk's visible surface is a few thousand faces), but a pathological
# checkerboard chunk would exceed it, so the mesher stops at the cap and
# reports truncation rather than silently emitting wrapped indices.
(def max-faces 16384)
(def max-verts (* max-faces 4))

(def bytes-per-float 4)
(def bytes-per-index 2)

(defn mesher-new
  ``Allocate a reusable mesher. The scratch buffers are sized for the
  worst case once, then reused for every chunk forever.``
  []
  @{:verts (buffer/new-filled (* max-verts 3 bytes-per-float) 0)
    :uvs (buffer/new-filled (* max-verts 2 bytes-per-float) 0)
    :norms (buffer/new-filled (* max-verts 3 bytes-per-float) 0)
    :colors (buffer/new-filled (* max-verts 4) 0)
    :idxs (buffer/new-filled (* max-faces 6 bytes-per-index) 0)
    :truncated false})

#
# --- Neighbour sampling -------------------------------------------------
#
# This is where the mesher's time goes, so it is written for the interpreter,
# not for elegance. A 16x64x16 chunk performs up to ~45k neighbour tests; the
# first version of this code called a function per test and cost 22 ms per
# chunk, which is more than a whole frame budget for a single chunk.
#
# Two changes removed that cost:
#   1. A neighbour inside the chunk is a *fixed index delta* away in the flat
#      block buffer -- +-1 on X, +-16 on Z, +-256 on Y -- so an interior test
#      is one array read with no coordinate maths and no call.
#   2. The border test and the transparency test are inlined at the use site
#      rather than being function calls.
#
# Only cells on a chunk face take the slow path into the world.

(def- air world/block-air)
(def- water world/block-water)
(def- stone world/block-stone)

(def- dz 16)    # +1 in Z
(def- dy 256)   # +1 in Y

(defn- seam-id
  ``Block id across a chunk boundary. A chunk that has not been generated yet
  reads as air, so a newly loaded chunk's edge faces are drawn now and culled
  when the neighbour arrives and both are re-meshed.``
  [world-ref wx wy wz]
  (if world-ref (world/world-get-block world-ref wx wy wz) air))

#
# --- The kernel ---------------------------------------------------------
#

(defn mesh-chunk
  ``Build vertex data for one chunk. `uv-fn` maps (block-id face) to a
  [u0 v0 u1 v1] atlas rect; pass nil to emit a unit rect per face.

  Returns nil when the chunk is empty or fully enclosed (nothing to draw),
  otherwise a table of right-sized buffers ready for rl/mesh-pack.``
  [m ch &opt world-ref uv-fn]
  (def blocks (get ch :blocks))
  (def verts (get m :verts))
  (def uvs (get m :uvs))
  (def norms (get m :norms))
  (def colors (get m :colors))
  (def idxs (get m :idxs))

  (var vcount 0)
  (var fcount 0)
  (var vo 0)   # byte cursor into verts/norms
  (var uo 0)   # byte cursor into uvs
  (var co 0)   # byte cursor into colors
  (var io 0)   # byte cursor into idxs
  (put m :truncated false)

  (def wx-base (* (get ch :cx) world/chunk-x))
  (def wz-base (* (get ch :cz) world/chunk-z))

  (for y 0 world/chunk-y
    (def ybase (* y dy))
    (def y-max (= y (- world/chunk-y 1)))
    (def y-min (= y 0))
    (for z 0 world/chunk-z
      (def zbase (+ ybase (* z dz)))
      (def z-max (= z (- world/chunk-z 1)))
      (def z-min (= z 0))
      (def wz (+ wz-base z))
      (for x 0 world/chunk-x
        (def i (+ zbase x))
        (def id (get blocks i))
        # Air contributes nothing; everything else may contribute up to 6 faces.
        (when (not= id air)
          (def wx (+ wx-base x))
          (def x-max (= x (- world/chunk-x 1)))
          (def x-min (= x 0))
          (for f 0 face-count
            # Interior neighbours are a fixed index delta; only a cell sitting
            # on the chunk's surface pays for a world lookup. Above the build
            # height is open sky, below y=0 is bedrock, so the world's
            # underside is never meshed.
            (def other
              (case f
                0 (if z-max (seam-id world-ref wx y (+ wz 1)) (get blocks (+ i dz)))
                1 (if z-min (seam-id world-ref wx y (- wz 1)) (get blocks (- i dz)))
                2 (if x-max (seam-id world-ref (+ wx 1) y wz) (get blocks (+ i 1)))
                3 (if x-min (seam-id world-ref (- wx 1) y wz) (get blocks (- i 1)))
                4 (if y-max air (get blocks (+ i dy)))
                5 (if y-min stone (get blocks (- i dy)))
                air))
            # A face is drawn when its neighbour is see-through. The id
            # comparison stops a body of water from meshing every internal
            # boundary between its own cells, while still letting its surface
            # and its contact with solids render.
            (when (and (or (= other air) (= other water)) (not= other id))
              (if (>= fcount max-faces)
                (put m :truncated true)
                (do
                  (def corners (get face-corners f))
                  (def normal (get face-normals f))
                  (def nx (get normal 0))
                  (def ny (get normal 1))
                  (def nz (get normal 2))
                  (def shade (get face-shade f))
                  (def lum (math/floor (* 255 shade)))
                  (def rect (if uv-fn (uv-fn id f) nil))
                  (def u0 (if rect (get rect 0) 0))
                  (def v0 (if rect (get rect 1) 0))
                  (def u1 (if rect (get rect 2) 1))
                  (def v1 (if rect (get rect 3) 1))

                  (var ci 0)
                  (while (< ci 4)
                    (def c (get corners ci))
                    # position, offset into chunk-local space
                    (ffi/write :float (* 1.0 (+ x (get c 0))) verts vo)
                    (ffi/write :float (* 1.0 (+ y (get c 1))) verts (+ vo 4))
                    (ffi/write :float (* 1.0 (+ z (get c 2))) verts (+ vo 8))
                    # normal
                    (ffi/write :float (* 1.0 nx) norms vo)
                    (ffi/write :float (* 1.0 ny) norms (+ vo 4))
                    (ffi/write :float (* 1.0 nz) norms (+ vo 8))
                    # uv: corners run bottom-left, bottom-right, top-right,
                    # top-left, and v0 is the tile's top edge
                    (ffi/write :float (* 1.0 (if (or (= ci 1) (= ci 2)) u1 u0)) uvs uo)
                    (ffi/write :float (* 1.0 (if (or (= ci 2) (= ci 3)) v0 v1)) uvs (+ uo 4))
                    # baked directional shade
                    (put colors co lum)
                    (put colors (+ co 1) lum)
                    (put colors (+ co 2) lum)
                    (put colors (+ co 3) 255)
                    (set vo (+ vo 12))
                    (set uo (+ uo 8))
                    (set co (+ co 4))
                    (++ ci))

                  # two triangles over the quad's four vertices
                  (ffi/write :uint16 vcount idxs io)
                  (ffi/write :uint16 (+ vcount 1) idxs (+ io 2))
                  (ffi/write :uint16 (+ vcount 2) idxs (+ io 4))
                  (ffi/write :uint16 vcount idxs (+ io 6))
                  (ffi/write :uint16 (+ vcount 2) idxs (+ io 8))
                  (ffi/write :uint16 (+ vcount 3) idxs (+ io 10))
                  (set io (+ io 12))
                  (set vcount (+ vcount 4))
                  (++ fcount)))))))))

  (if (zero? fcount)
    nil
    # Right-sized copies: the scratch stays huge and reusable, while what the
    # chunk retains is exactly the geometry it has.
    @{:vertex-count vcount
      :triangle-count (* fcount 2)
      :face-count fcount
      :truncated (get m :truncated)
      :verts (buffer/slice verts 0 vo)
      :uvs (buffer/slice uvs 0 uo)
      :norms (buffer/slice norms 0 vo)
      :colors (buffer/slice colors 0 co)
      :idxs (buffer/slice idxs 0 io)}))

(defn count-faces
  ``Count the faces a chunk would emit without building any geometry.
  Used by tests and benchmarks to separate culling cost from write cost.``
  [ch &opt world-ref]
  (def blocks (get ch :blocks))
  (def wx-base (* (get ch :cx) world/chunk-x))
  (def wz-base (* (get ch :cz) world/chunk-z))
  (var n 0)
  (for y 0 world/chunk-y
    (def ybase (* y dy))
    (def y-max (= y (- world/chunk-y 1)))
    (def y-min (= y 0))
    (for z 0 world/chunk-z
      (def zbase (+ ybase (* z dz)))
      (def z-max (= z (- world/chunk-z 1)))
      (def z-min (= z 0))
      (def wz (+ wz-base z))
      (for x 0 world/chunk-x
        (def i (+ zbase x))
        (def id (get blocks i))
        (when (not= id air)
          (def wx (+ wx-base x))
          (def x-max (= x (- world/chunk-x 1)))
          (def x-min (= x 0))
          (for f 0 face-count
            (def other
              (case f
                0 (if z-max (seam-id world-ref wx y (+ wz 1)) (get blocks (+ i dz)))
                1 (if z-min (seam-id world-ref wx y (- wz 1)) (get blocks (- i dz)))
                2 (if x-max (seam-id world-ref (+ wx 1) y wz) (get blocks (+ i 1)))
                3 (if x-min (seam-id world-ref (- wx 1) y wz) (get blocks (- i 1)))
                4 (if y-max air (get blocks (+ i dy)))
                5 (if y-min stone (get blocks (- i dy)))
                air))
            (when (and (or (= other air) (= other water)) (not= other id))
              (++ n)))))))
  n)
