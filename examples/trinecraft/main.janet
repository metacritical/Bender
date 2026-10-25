# Trinecraft — a voxel engine on bender + raylib.
#
# Run:  ./build/bender examples/trinecraft/main.janet
#       ./build/bender examples/trinecraft/main.janet --screenshot out.png
#
# Wires the pieces together: worldgen fills chunks with block ids, the mesher
# turns each chunk into one GPU mesh, and the frame loop issues a single
# DrawMesh per visible chunk. The engine never draws a block -- FFI call
# overhead forbids it -- so a 16x64x16 chunk of thousands of blocks costs
# exactly one call.

(import ./src/rl :as rl)
(import ./src/world :as world)
(import ./src/worldgen :as worldgen)
(import ./src/mesher :as mesher)
(import ./src/atlas :as atlas)

(def view-radius 4)          # chunks each way, so (2r+1)^2 chunks total
(def seed 4)   # land at the origin with coastline in view
(def move-speed 18.0)        # blocks per second
(def look-speed 0.0032)      # radians per pixel of mouse movement

#
# --- Argument handling --------------------------------------------------
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
# --- World construction -------------------------------------------------
#

(defn build-world []
  (def w (world/world-new))
  (put w :seed seed)
  (def t0 (os/clock))
  (for cz (- view-radius) (+ view-radius 1)
    (for cx (- view-radius) (+ view-radius 1)
      (worldgen/generate-chunk w cx cz seed)))
  (def t1 (os/clock))
  (printf "worldgen: %d chunks in %.0f ms"
          (world/world-chunk-count w) (* 1000 (- t1 t0)))
  w)

(defn build-meshes [w]
  ``Mesh every chunk and upload it. Chunks are meshed only after the whole
  world exists, so each one sees its real neighbours and no interior seam
  faces are emitted between adjacent chunks.``
  (def m (mesher/mesher-new))
  (def drawables @[])
  (var total-faces 0)
  (var mesh-ms 0)
  (var upload-ms 0)
  (each ch (values (get w :chunks))
    (def t0 (os/clock))
    (def geo (mesher/mesh-chunk m ch w atlas/face-uv))
    (def t1 (os/clock))
    (+= mesh-ms (- t1 t0))
    (when geo
      (+= total-faces (get geo :face-count))
      (def mesh (rl/mesh-pack (get geo :vertex-count)
                              (get geo :triangle-count)
                              (get geo :verts)
                              (get geo :uvs)
                              (get geo :norms)
                              (get geo :idxs)))
      (rl/mesh-upload mesh)
      (def t2 (os/clock))
      (+= upload-ms (- t2 t1))
      (if (rl/mesh-uploaded? mesh)
        (do
          (put ch :mesh mesh)
          (put ch :dirty false)
          (array/push drawables
                      @{:mesh mesh
                        :xform (rl/translation-matrix
                                 (* (get ch :cx) world/chunk-x) 0
                                 (* (get ch :cz) world/chunk-z))}))
        (eprintf "warning: chunk %d,%d failed to upload"
                 (get ch :cx) (get ch :cz)))))
  (printf "mesher:   %d faces across %d meshes in %.0f ms"
          total-faces (length drawables) (* 1000 mesh-ms))
  (printf "upload:   %.0f ms" (* 1000 upload-ms))
  drawables)

#
# --- Camera -------------------------------------------------------------
#
# A free-fly camera: yaw/pitch from the mouse, WASD along the view plane.
# Phase 6 replaces this with a grounded player that collides with the world.

(defn spawn-position []
  # Start clear of the terrain AND clear of the sea. Using the column height
  # alone drops the camera inside the water wherever the origin happens to sit
  # below sea level, which renders as a wall of blue.
  (def ground (max (worldgen/height-at seed 0 0) worldgen/sea-level))
  @[0.5 (+ ground 14.0) 0.5])

(defn look-direction [yaw pitch]
  (def cp (math/cos pitch))
  [(* cp (math/cos yaw)) (math/sin pitch) (* cp (math/sin yaw))])

(defn update-camera [state dt]
  (def d (rl/mouse-delta))
  (set (state :yaw) (+ (state :yaw) (* (get d 0) look-speed)))
  (set (state :pitch) (- (state :pitch) (* (get d 1) look-speed)))
  # Clamp pitch just short of vertical so the view never flips over.
  (def limit 1.55)
  (when (> (state :pitch) limit) (set (state :pitch) limit))
  (when (< (state :pitch) (- limit)) (set (state :pitch) (- limit)))

  (def fwd (look-direction (state :yaw) (state :pitch)))
  # Right vector is forward x up, flattened: no roll, so it stays horizontal.
  (def right [(- (math/sin (state :yaw))) 0 (math/cos (state :yaw))])
  (def speed (* move-speed dt (if (rl/key-down? rl/key-left-shift) 3.0 1.0)))
  (def pos (state :pos))
  (defn step [v s]
    (put pos 0 (+ (get pos 0) (* (get v 0) s)))
    (put pos 1 (+ (get pos 1) (* (get v 1) s)))
    (put pos 2 (+ (get pos 2) (* (get v 2) s))))
  (when (rl/key-down? rl/key-w) (step fwd speed))
  (when (rl/key-down? rl/key-s) (step fwd (- speed)))
  (when (rl/key-down? rl/key-d) (step right speed))
  (when (rl/key-down? rl/key-a) (step right (- speed)))
  (when (rl/key-down? rl/key-space) (put pos 1 (+ (get pos 1) speed)))
  (when (rl/key-down? rl/key-q) (put pos 1 (- (get pos 1) speed)))
  fwd)

(defn camera-of [state fwd]
  (def p (state :pos))
  (rl/camera3d [(get p 0) (get p 1) (get p 2)]
               [(+ (get p 0) (get fwd 0))
                (+ (get p 1) (get fwd 1))
                (+ (get p 2) (get fwd 2))]
               [0 1 0]
               70))

#
# --- Main ---------------------------------------------------------------
#

(defn main [&]
  (def w (build-world))

  (rl/set-config-flags (bor rl/flag-vsync-hint rl/flag-msaa-4x-hint))
  (rl/init-window 1280 800 "Trinecraft — bender + raylib")
  (rl/set-target-fps 60)

  # The atlas and every mesh need a live GL context, so they are created only
  # after the window exists.
  (def texture (atlas/load-atlas-texture))
  (def material (rl/load-material-default))
  (rl/material-set-texture material texture)

  (def drawables (build-meshes w))
  (when (empty? drawables)
    (eprint "error: no chunk produced geometry")
    (rl/close-window)
    (os/exit 1))

  (unless headless-frames (rl/disable-cursor))

  (def state @{:pos (spawn-position) :yaw 0.9 :pitch -0.45})
  (var frame 0)
  (var worst 0)

  (while (and (not (rl/window-should-close))
              (or (nil? headless-frames) (< frame headless-frames)))
    (def dt (rl/get-frame-time))
    (def fwd
      (if headless-frames
        # Deterministic orbit for screenshots, so the captured image does not
        # depend on where a mouse happened to be.
        (do (set (state :yaw) (+ 0.9 (* frame 0.01)))
            (look-direction (state :yaw) (state :pitch)))
        (update-camera state dt)))
    (def cam (camera-of state fwd))

    (rl/begin-drawing)
    (rl/clear-background rl/sky-blue)
    (rl/begin-mode-3d cam)
    # One DrawMesh per chunk: the whole visible world in a few hundred calls.
    (each d drawables
      (rl/mesh-draw (get d :mesh) material (get d :xform)))
    (rl/end-mode-3d)

    (def ms (* 1000 dt))
    (when (and (> frame 10) (> ms worst)) (set worst ms))
    (rl/draw-rectangle 8 8 320 86 [0 0 0 140])
    (rl/draw-text (string/format "%d FPS  %.1f ms (worst %.1f)"
                                 (rl/get-fps) ms worst)
                  16 16 20 rl/white)
    (rl/draw-text (string/format "%d chunks  %d draw calls"
                                 (world/world-chunk-count w) (length drawables))
                  16 40 18 rl/white)
    (rl/draw-text (string/format "x %.1f  y %.1f  z %.1f"
                                 (get (state :pos) 0) (get (state :pos) 1)
                                 (get (state :pos) 2))
                  16 62 18 rl/white)
    (rl/end-drawing)

    (when (and screenshot-path (= frame (- headless-frames 1)))
      (rl/take-screenshot screenshot-path))
    (++ frame))

  (printf "frames: %d, worst frame after warmup: %.1f ms" frame worst)
  (rl/close-window))
