# Trinecraft — player movement and voxel collision.
#
# The player is an axis-aligned box that never rotates, so collision reduces
# to testing a box against the integer block grid. Movement is resolved one
# axis at a time: move along X, push out of anything hit, then Y, then Z.
#
# Resolving per axis is what makes sliding along a wall work. Moving on all
# three axes at once and then rejecting the whole step would stick the player
# to any surface they brush; resolving X independently lets the Z component of
# the same step survive.

(import ./world :as world)

# A player is 0.6 blocks wide and 1.8 tall, with the eye near the top. These
# are the vanilla proportions and they matter: a width under 1.0 lets the
# player fit through a one-block gap, and a height over 1.0 means they are
# always straddling at least two block layers.
(def player-width 0.6)
(def player-height 1.8)
(def eye-height 1.62)
(def half-width (/ player-width 2))

(def gravity 28.0)          # blocks/s^2
(def jump-speed 9.0)        # gives roughly a 1.25 block jump
(def terminal-velocity 60.0)
(def walk-speed 5.6)
(def sprint-multiplier 1.9)
(def fly-speed 18.0)
# Vanilla uses 0.6, which deliberately does NOT clear a full block: there you
# must jump to climb one, and auto-stepping only handles slabs and paths.
# Trinecraft's world is made exclusively of full blocks, so a 0.6 step would
# never fire at all and the player would have to jump at every single rise.
# 1.05 clears exactly one block and nothing taller.
(def step-height 1.05)

# Pushing the box exactly onto a block face leaves it touching, and the next
# frame's overlap test may or may not report contact depending on rounding.
# Backing off by a hair makes contact unambiguous.
(def skin 0.001)

(defn player-new [x y z]
  @{:pos @[x y z]
    :vel @[0 0 0]
    :yaw 0.0
    :pitch 0.0
    :on-ground false
    :flying true})

#
# --- Solidity -----------------------------------------------------------
#

(defn solid-at?
  ``Does the block at these world coords stop the player?

  Water is deliberately not solid, so the player swims rather than walks on
  it. Below y=0 is treated as solid so nobody falls out of the world, and
  above the build height is open air.``
  [w wx wy wz]
  (cond
    (< wy 0) true
    (>= wy world/chunk-y) false
    (world/block-solid? (world/world-get-block w wx wy wz))))

(defn box-collides?
  ``Does the player box centred on (x,z) with its feet at y overlap any solid
  block? The box spans [x-hw, x+hw] x [y, y+height] x [z-hw, z+hw], so the
  cells to test run from the floor of the low corner to the floor of the high
  corner inclusive.``
  [w x y z]
  (def x0 (math/floor (- x half-width)))
  (def x1 (math/floor (+ x half-width)))
  (def y0 (math/floor y))
  (def y1 (math/floor (+ y player-height)))
  (def z0 (math/floor (- z half-width)))
  (def z1 (math/floor (+ z half-width)))
  (var hit false)
  (var bx x0)
  (while (and (not hit) (<= bx x1))
    (var by y0)
    (while (and (not hit) (<= by y1))
      (var bz z0)
      (while (and (not hit) (<= bz z1))
        (when (solid-at? w bx by bz) (set hit true))
        (++ bz))
      (++ by))
    (++ bx))
  hit)

#
# --- Per-axis resolution ------------------------------------------------
#

(defn- axis-extent
  ``How far the player box reaches below and above its coordinate on an axis.

  X and Z are centred on the coordinate, but Y is NOT: the coordinate is the
  feet, with the head at +player-height. Getting this asymmetry wrong is what
  lets a jumping player punch through a ceiling.``
  [axis]
  (if (= axis 1)
    [0 player-height]
    [(- half-width) half-width]))

(defn- move-axis
  ``Attempt to move `delta` along one axis. Returns the resolved coordinate
  and whether the move was blocked.

  The step is subdivided so a fast-moving player cannot tunnel through a
  block: at terminal velocity a 16 ms frame covers nearly a whole block, and a
  single overlap test at the destination would miss a wall entirely. Each
  sub-step is at most half a block.

  On contact the box is snapped flush against the blocking face. The snap is
  computed from the box's LEADING EDGE, not from its coordinate: the wall is
  the integer plane the edge just crossed, so the coordinate that puts the
  edge back on that plane is wall - edge-offset. Deriving it from the centre
  instead happens to agree only when the box sits inside the same cell as its
  edge, and silently misplaces the player everywhere else.``
  [w pos axis delta]
  (if (zero? delta)
    [(get pos axis) false]
    (do
      (def [lo-off hi-off] (axis-extent axis))
      (def steps (max 1 (math/ceil (/ (math/abs delta) 0.5))))
      (def inc (/ delta steps))
      (var current (get pos axis))
      (var blocked false)
      (var i 0)
      (def probe @[(get pos 0) (get pos 1) (get pos 2)])
      (while (and (< i steps) (not blocked))
        (def next (+ current inc))
        (put probe axis next)
        (if (box-collides? w (get probe 0) (get probe 1) (get probe 2))
          (do
            (set blocked true)
            (set current
                 (if (pos? inc)
                   # Leading edge is the high side; it stops at the floor of
                   # the plane it crossed.
                   (- (math/floor (+ next hi-off)) hi-off skin)
                   # Leading edge is the low side; it stops at the top of the
                   # cell it entered.
                   (+ (math/floor (+ next lo-off)) 1 (- lo-off) skin))))
          (set current next))
        (++ i))
      [current blocked])))

(defn try-step-up
  ``A blocked horizontal move may still succeed if the obstruction is only a
  lip: lift the box by up to step-height, retry, and keep the result only if
  the raised position is clear. Without this the player catches on every
  one-block rise and has to jump constantly.``
  [w pos axis delta]
  (def raised @[(get pos 0) (get pos 1) (get pos 2)])
  (put raised 1 (+ (get pos 1) step-height))
  # No point climbing if the raised body is itself inside something.
  (if (box-collides? w (get raised 0) (get raised 1) (get raised 2))
    nil
    (do
      (def [moved blocked] (move-axis w raised axis delta))
      (if blocked
        nil
        (do
          (put raised axis moved)
          # Settle back down onto whatever is underneath.
          (def [dropped _] (move-axis w raised 1 (- step-height)))
          (put raised 1 dropped)
          raised)))))

#
# --- Integration --------------------------------------------------------
#

(defn move-and-collide
  ``Apply a displacement with collision, resolving X, then Y, then Z.
  Mutates and returns the player's :pos, and updates :on-ground.``
  [player w dx dy dz]
  (def pos (player :pos))

  # X
  (unless (zero? dx)
    (def [nx blocked] (move-axis w pos 0 dx))
    (if (and blocked (player :on-ground))
      (if-let [stepped (try-step-up w pos 0 dx)]
        (do (put pos 0 (get stepped 0)) (put pos 1 (get stepped 1)))
        (put pos 0 nx))
      (put pos 0 nx)))

  # Z
  (unless (zero? dz)
    (def [nz blocked] (move-axis w pos 2 dz))
    (if (and blocked (player :on-ground))
      (if-let [stepped (try-step-up w pos 2 dz)]
        (do (put pos 2 (get stepped 2)) (put pos 1 (get stepped 1)))
        (put pos 2 nz))
      (put pos 2 nz)))

  # Y last, so on-ground reflects the position actually settled into.
  (unless (zero? dy)
    (def [ny blocked] (move-axis w pos 1 dy))
    (put pos 1 ny)
    (if blocked
      (do
        (when (neg? dy) (put player :on-ground true))
        # Hitting a ceiling kills upward speed, otherwise the player hovers
        # under it for the rest of the jump.
        (put (player :vel) 1 0))
      (when (neg? dy) (put player :on-ground false))))
  pos)

(defn grounded?
  "Is there something solid immediately beneath the player's feet?"
  [player w]
  (def pos (player :pos))
  (box-collides? w (get pos 0) (- (get pos 1) (* 2 skin)) (get pos 2)))

(defn update-physics
  ``Advance the player by one frame. `move` is the desired horizontal
  direction in world space (need not be normalised), `jump` and `sprint` are
  booleans, `vertical` is the fly-mode up/down input.``
  [player w dt move jump sprint vertical]
  (def vel (player :vel))
  (def [mx mz] move)

  (if (player :flying)
    (do
      (def speed (* fly-speed dt (if sprint sprint-multiplier 1)))
      (put vel 1 0)
      (move-and-collide player w (* mx speed) (* vertical speed) (* mz speed))
      (put player :on-ground false))
    (do
      # Gravity integrates into velocity; velocity integrates into position.
      (put vel 1 (max (- terminal-velocity)
                      (- (get vel 1) (* gravity dt))))
      (put player :on-ground (grounded? player w))
      (when (and jump (player :on-ground))
        (put vel 1 jump-speed)
        (put player :on-ground false))
      (def speed (* walk-speed (if sprint sprint-multiplier 1)))
      (move-and-collide player w
                        (* mx speed dt)
                        (* (get vel 1) dt)
                        (* mz speed dt))))
  player)

(defn eye-position [player]
  (def p (player :pos))
  [(get p 0) (+ (get p 1) eye-height) (get p 2)])

(defn look-direction [player]
  (def cp (math/cos (player :pitch)))
  [(* cp (math/cos (player :yaw)))
   (math/sin (player :pitch))
   (* cp (math/sin (player :yaw)))])

(defn drop-to-ground
  ``Place the player on the first clear spot above the terrain at their x,z.
  Scans downward from the build ceiling so the player always starts standing
  on the surface instead of buried in it.``
  [player w]
  (def pos (player :pos))
  (def x (get pos 0))
  (def z (get pos 2))
  (var y (- world/chunk-y 3))
  (while (and (> y 1) (box-collides? w x y z))
    (-- y))
  (while (and (> y 1) (not (box-collides? w x (- y 1) z)))
    (-- y))
  (put pos 1 y)
  (put player :on-ground true)
  player)
