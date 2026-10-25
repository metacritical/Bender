# Trinecraft — a voxel engine on bender + raylib.
#
# Run:  ./build/bender examples/trinecraft/main.janet
#       ./build/bender examples/trinecraft/main.janet --screenshot out.png
#
# Controls: WASD move, mouse look, Space jump (or ascend when flying),
#           Shift sprint, Q descend when flying, F toggle fly/walk,
#           left click break, right click place, 1-4 select block, Esc quit.
#
# Wires the pieces together: worldgen fills chunks with block ids, the mesher
# turns each chunk into one GPU mesh, and the frame loop issues a single
# DrawMesh per visible chunk. The engine never draws a block -- FFI call
# overhead forbids it -- so a chunk of thousands of blocks costs one call.

(import ./src/rl :as rl)
(import ./src/world :as world)
(import ./src/worldgen :as worldgen)
(import ./src/mesher :as mesher)
(import ./src/atlas :as atlas)
(import ./src/player :as player)
(import ./src/raycast :as raycast)

(def view-radius 4)          # chunks each way, so (2r+1)^2 chunks total
(def seed 4)                 # land at the origin with coastline in view
(def reach 6.0)              # how far the player can break/place
(def look-speed 0.0032)      # radians per pixel of mouse movement

(def placeable
  [world/block-stone world/block-dirt world/block-wood world/block-leaves])

#
# --- Arguments ----------------------------------------------------------
#

(defn arg-value [flag]
  (var found nil)
  (def args (dyn :args))
  (for i 0 (length args)
    (when (and (= (get args i) flag) (< (+ i 1) (length args)))
      (set found (get args (+ i 1)))))
  found)

(def screenshot-path (arg-value "--screenshot"))
(def headless-frames (if screenshot-path 90 nil))

#
# --- World and meshes ---------------------------------------------------
#

(defn build-world []
  (def w (world/world-new))
  (put w :seed seed)
  (def t0 (os/clock))
  (for cz (- view-radius) (+ view-radius 1)
    (for cx (- view-radius) (+ view-radius 1)
      (worldgen/generate-chunk w cx cz seed)))
  (printf "worldgen: %d chunks in %.0f ms"
          (world/world-chunk-count w) (* 1000 (- (os/clock) t0)))
  w)

(defn upload-chunk
  ``Mesh one chunk and hand it to the GPU, replacing any mesh it already had.
  Returns the drawable, or nil when the chunk has no visible geometry.``
  [m w ch]
  (def geo (mesher/mesh-chunk m ch w atlas/face-uv))
  # Release the old GPU buffers before dropping the reference, or every edit
  # leaks a VAO and the driver eventually refuses to allocate.
  (when (ch :mesh)
    (rl/mesh-unload (ch :mesh))
    (put ch :mesh nil))
  (put ch :dirty false)
  (when geo
    (def mesh (rl/mesh-pack (get geo :vertex-count) (get geo :triangle-count)
                            (get geo :verts) (get geo :uvs)
                            (get geo :norms) (get geo :idxs)))
    (rl/mesh-upload mesh)
    (when (rl/mesh-uploaded? mesh)
      (put ch :mesh mesh)
      @{:mesh mesh
        :chunk ch
        :xform (rl/translation-matrix (* (get ch :cx) world/chunk-x) 0
                                      (* (get ch :cz) world/chunk-z))})))

(defn build-meshes [m w]
  ``Mesh every chunk after the whole world exists, so each sees its real
  neighbours and no interior seam faces are emitted between adjacent chunks.``
  (def drawables @{})
  (var verts 0)
  (def t0 (os/clock))
  (each ch (values (get w :chunks))
    (when-let [d (upload-chunk m w ch)]
      (+= verts (get (d :mesh) :vertex-count))
      (put drawables (world/chunk-key (ch :cx) (ch :cz)) d)))
  (printf "mesher:   %d meshes, %d vertices in %.0f ms"
          (length drawables) verts (* 1000 (- (os/clock) t0)))
  drawables)

(defn remesh-around
  ``Re-mesh the chunk containing a block, plus any neighbour that shares the
  seam it sits on. Editing a block on a chunk edge changes the neighbour's
  culling too, so re-meshing only the touched chunk leaves a visible hole or
  a stale face at the boundary.``
  [m w drawables wx wz]
  (def cx (world/world-to-chunk-coord wx))
  (def cz (world/world-to-chunk-coord wz))
  (def lx (world/world-to-local-coord wx))
  (def lz (world/world-to-local-coord wz))
  (def targets @[[cx cz]])
  (when (= lx 0) (array/push targets [(- cx 1) cz]))
  (when (= lx (- world/chunk-x 1)) (array/push targets [(+ cx 1) cz]))
  (when (= lz 0) (array/push targets [cx (- cz 1)]))
  (when (= lz (- world/chunk-z 1)) (array/push targets [cx (+ cz 1)]))
  (each t targets
    (when-let [ch (world/world-get-chunk w (get t 0) (get t 1))]
      (def key (world/chunk-key (get t 0) (get t 1)))
      (def d (upload-chunk m w ch))
      (if d (put drawables key d) (put drawables key nil)))))

#
# --- Input --------------------------------------------------------------
#

(defn apply-look [p]
  (def d (rl/mouse-delta))
  (put p :yaw (+ (p :yaw) (* (get d 0) look-speed)))
  (put p :pitch (- (p :pitch) (* (get d 1) look-speed)))
  # Clamp just short of vertical so the view never flips over.
  (def limit 1.55)
  (when (> (p :pitch) limit) (put p :pitch limit))
  (when (< (p :pitch) (- limit)) (put p :pitch (- limit))))

(defn movement-input [p]
  ``Desired horizontal direction in world space, from WASD relative to yaw.``
  (def yaw (p :yaw))
  (def fx (math/cos yaw))
  (def fz (math/sin yaw))
  # Right is forward rotated 90 degrees in the XZ plane.
  (def rx (- fz))
  (def rz fx)
  (var mx 0) (var mz 0)
  (when (rl/key-down? rl/key-w) (+= mx fx) (+= mz fz))
  (when (rl/key-down? rl/key-s) (-= mx fx) (-= mz fz))
  (when (rl/key-down? rl/key-d) (+= mx rx) (+= mz rz))
  (when (rl/key-down? rl/key-a) (-= mx rx) (-= mz rz))
  # Normalise so diagonals are not faster than the cardinals.
  (def len (math/sqrt (+ (* mx mx) (* mz mz))))
  (if (> len 0.0001) [(/ mx len) (/ mz len)] [0 0]))

#
# --- Main ---------------------------------------------------------------
#

(defn main [&]
  (def w (build-world))

  (rl/set-config-flags (bor rl/flag-vsync-hint rl/flag-msaa-4x-hint))
  (rl/init-window 1280 800 "Trinecraft — bender + raylib")
  (rl/set-target-fps 60)

  # The atlas and every mesh need a live GL context, so they come after the
  # window exists.
  (def texture (atlas/load-atlas-texture))
  (def material (rl/load-material-default))
  (rl/material-set-texture material texture)

  (def m (mesher/mesher-new))
  (def drawables (build-meshes m w))
  (when (empty? drawables)
    (eprint "error: no chunk produced geometry")
    (rl/close-window)
    (os/exit 1))

  (def p (player/player-new 0.5 40 0.5))
  (put p :yaw 0.9)
  (put p :pitch -0.35)
  (put p :flying false)
  (player/drop-to-ground p w)

  (unless headless-frames (rl/disable-cursor))

  (var frame 0)
  (var worst 0)
  (var selected 0)
  (var edits 0)

  (while (and (not (rl/window-should-close))
              (or (nil? headless-frames) (< frame headless-frames)))
    (def dt (min (rl/get-frame-time) 0.05))   # clamp so a stall cannot
                                              # teleport the player
    (if headless-frames
      # Deterministic sweep for screenshots, independent of mouse position.
      (do (put p :yaw (+ 0.9 (* frame 0.004)))
          (put p :pitch (+ -0.5 (* frame 0.004))))
      (apply-look p))

    (unless headless-frames
      (when (rl/key-pressed? rl/key-f)
        (put p :flying (not (p :flying))))
      (each [i key] (pairs [rl/key-one rl/key-two rl/key-three rl/key-four])
        (when (rl/key-pressed? key) (set selected i))))

    (def vertical
      (cond (rl/key-down? rl/key-space) 1
            (rl/key-down? rl/key-q) -1
            0))
    (player/update-physics p w dt
                           (if headless-frames [0 0] (movement-input p))
                           (rl/key-down? rl/key-space)
                           (rl/key-down? rl/key-left-shift)
                           vertical)

    # --- Targeting ---
    (def eye (player/eye-position p))
    (def dir (player/look-direction p))
    (def hit (raycast/raycast-first-solid w (get eye 0) (get eye 1) (get eye 2)
                                          (get dir 0) (get dir 1) (get dir 2)
                                          reach))
    # In headless mode drive the same break/place code paths on a schedule, so
    # a screenshot run actually exercises editing and incremental re-meshing
    # rather than just proving the renderer compiles.
    (when (and headless-frames hit)
      (when (and (>= frame 20) (< frame 40) (zero? (% frame 4)))
        (world/world-set-block w (hit :x) (hit :y) (hit :z) world/block-air)
        (remesh-around m w drawables (hit :x) (hit :z))
        (++ edits))
      (when (and (>= frame 50) (< frame 70) (zero? (% frame 4)) (hit :face))
        (world/world-set-block w (hit :px) (hit :py) (hit :pz)
                               world/block-wood)
        (remesh-around m w drawables (hit :px) (hit :pz))
        (++ edits)))

    (unless headless-frames
      (when (and hit (rl/mouse-pressed? rl/mouse-button-left))
        (world/world-set-block w (hit :x) (hit :y) (hit :z) world/block-air)
        (remesh-around m w drawables (hit :x) (hit :z))
        (++ edits))
      (when (and hit (hit :face) (rl/mouse-pressed? rl/mouse-button-right))
        # Refuse to place a block inside the player's own box.
        (def px (hit :px)) (def py (hit :py)) (def pz (hit :pz))
        (def saved (world/world-get-block w px py pz))
        (world/world-set-block w px py pz (get placeable selected))
        (if (player/box-collides? w (get (p :pos) 0) (get (p :pos) 1)
                                  (get (p :pos) 2))
          (world/world-set-block w px py pz saved)
          (do (remesh-around m w drawables px pz) (++ edits)))))

    # --- Draw ---
    (def cam (rl/camera3d eye
                          [(+ (get eye 0) (get dir 0))
                           (+ (get eye 1) (get dir 1))
                           (+ (get eye 2) (get dir 2))]
                          [0 1 0] 70))
    (rl/begin-drawing)
    (rl/clear-background rl/sky-blue)
    (rl/begin-mode-3d cam)
    (each d (values drawables)
      (when d (rl/mesh-draw (get d :mesh) material (get d :xform))))
    # Outline the targeted block. DrawCubeWires centres on the position, so
    # the centre of block (x,y,z) is (x+0.5, y+0.5, z+0.5).
    (when hit
      (rl/draw-cube-wires [(+ (hit :x) 0.5) (+ (hit :y) 0.5) (+ (hit :z) 0.5)]
                          1.002 1.002 1.002 rl/black))
    (rl/end-mode-3d)

    (def ms (* 1000 dt))
    (when (and (> frame 10) (> ms worst)) (set worst ms))
    (rl/draw-rectangle 8 8 360 108 [0 0 0 140])
    (rl/draw-text (string/format "%d FPS  %.1f ms (worst %.1f)"
                                 (rl/get-fps) ms worst)
                  16 16 20 rl/white)
    (rl/draw-text (string/format "%d chunks  %d draws  %d edits"
                                 (world/world-chunk-count w)
                                 (length drawables) edits)
                  16 40 18 rl/white)
    (rl/draw-text (string/format "x %.1f y %.1f z %.1f  %s"
                                 (get (p :pos) 0) (get (p :pos) 1)
                                 (get (p :pos) 2)
                                 (if (p :flying) "fly" "walk"))
                  16 62 18 rl/white)
    (rl/draw-text (string/format "holding: %p   [F] fly  [1-4] block"
                                 (world/block-name (get placeable selected)))
                  16 84 18 rl/white)
    # Crosshair.
    (def cx (/ (rl/screen-width) 2))
    (def cy (/ (rl/screen-height) 2))
    (rl/draw-rectangle (- cx 8) (- cy 1) 16 2 rl/white)
    (rl/draw-rectangle (- cx 1) (- cy 8) 2 16 rl/white)
    (rl/end-drawing)

    (when (and screenshot-path (= frame (- headless-frames 1)))
      (rl/take-screenshot screenshot-path))
    (++ frame))

  (printf "frames: %d, worst after warmup: %.1f ms, edits: %d"
          frame worst edits)
  (rl/close-window))
