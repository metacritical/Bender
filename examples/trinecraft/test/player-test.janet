# Headless player/collision tests. No window, no GPU.
# Run: ./build/bender examples/trinecraft/test/player-test.janet

(import ./../src/world :as world)
(import ./../src/player :as player)

(var failures 0)
(defn check [name ok &opt detail]
  (if ok
    (printf "  ok   %s" name)
    (do (++ failures)
        (printf "  FAIL %s%s" name (if detail (string " -- " detail) "")))))
(defn near? [a b &opt eps] (< (math/abs (- a b)) (or eps 0.02)))

(defn flat-world
  ``A world whose chunks are solid stone up to (and including) y = top,
  spanning chunk coords -1..1 so the player cannot walk off the edge.``
  [top]
  (def w (world/world-new))
  (for cz -1 2
    (for cx -1 2
      (def ch (world/chunk-new cx cz))
      (for y 0 (+ top 1)
        (for z 0 world/chunk-z
          (for x 0 world/chunk-x
            (world/chunk-set ch x y z world/block-stone))))
      (world/world-put-chunk w ch)))
  w)

#
# --- Box overlap --------------------------------------------------------
#
(print "box overlap:")
(def w (flat-world 10))   # solid through y=10, so y=11 is the first free layer

(check "standing on the surface does not overlap"
       (not (player/box-collides? w 8.5 11 8.5)))
(check "feet inside the floor overlaps"
       (player/box-collides? w 8.5 10 8.5))
(check "deep inside the ground overlaps"
       (player/box-collides? w 8.5 5 8.5))
(check "well above the ground is clear"
       (not (player/box-collides? w 8.5 20 8.5)))
# The box is 1.8 tall, so a head poking into a block one layer up must collide.
(def w2 (flat-world 10))
(world/world-set-block w2 8 12 8 world/block-stone)
(check "head inside a block overlaps" (player/box-collides? w2 8.5 11 8.5))
(check "below y=0 is solid so nobody leaves the world"
       (player/box-collides? w 8.5 -1 8.5))

#
# --- Gravity and landing ------------------------------------------------
#
(print "\ngravity and landing:")
(def p (player/player-new 8.5 25 8.5))
(put p :flying false)
(var ticks 0)
(while (and (< ticks 400) (not (p :on-ground)))
  (player/update-physics p w (/ 1 60) [0 0] false false 0)
  (++ ticks))
(check "a falling player lands" (p :on-ground) (string "after " ticks " ticks"))
(check "lands on top of the floor, not inside it"
       (near? (get (p :pos) 1) 11 0.05)
       (string "y = " (get (p :pos) 1)))
(check "does not fall through the world" (> (get (p :pos) 1) 0))

# Once grounded, the player must stay put rather than sinking frame by frame.
(def y-landed (get (p :pos) 1))
(for i 0 120 (player/update-physics p w (/ 1 60) [0 0] false false 0))
(check "resting on the ground is stable over 120 frames"
       (near? (get (p :pos) 1) y-landed 0.01)
       (string "drifted to " (get (p :pos) 1)))

#
# --- Jumping ------------------------------------------------------------
#
(print "\njumping:")
(def jp (player/player-new 8.5 11 8.5))
(put jp :flying false)
(player/update-physics jp w (/ 1 60) [0 0] false false 0)
(def base (get (jp :pos) 1))
(player/update-physics jp w (/ 1 60) [0 0] true false 0)
(var peak base)
(for i 0 120
  (player/update-physics jp w (/ 1 60) [0 0] false false 0)
  (when (> (get (jp :pos) 1) peak) (set peak (get (jp :pos) 1))))
(def jump-height (- peak base))
(check "a jump leaves the ground" (> jump-height 0.8)
       (string "height " jump-height))
(check "a jump clears one block" (> jump-height 1.0)
       (string "height " jump-height))
(check "a jump does not clear two blocks" (< jump-height 2.0)
       (string "height " jump-height))
(check "the player lands again" (near? (get (jp :pos) 1) base 0.05))

#
# --- Walls and sliding --------------------------------------------------
#
(print "\nwalls and sliding:")
# A wall two blocks tall at x=10, too high to step over.
(def ww (flat-world 10))
(for y 11 13
  (for z 0 16 (world/world-set-block ww 10 y z world/block-stone)))

(def wp (player/player-new 8.5 11 8.5))
(put wp :flying false)
(for i 0 120 (player/update-physics wp ww (/ 1 60) [1 0] false false 0))
(check "walking into a wall is stopped by it"
       (< (get (wp :pos) 0) 10)
       (string "x = " (get (wp :pos) 0)))
(check "the player ends up flush against the wall"
       (near? (get (wp :pos) 0) (- 10 player/half-width) 0.05)
       (string "x = " (get (wp :pos) 0)))
(check "the player is not inside the wall"
       (not (player/box-collides? ww (get (wp :pos) 0) (get (wp :pos) 1)
                                  (get (wp :pos) 2))))

# Pushing diagonally into the wall must still slide along Z.
(def sp (player/player-new 8.5 11 8.5))
(put sp :flying false)
(def z-start (get (sp :pos) 2))
(for i 0 60 (player/update-physics sp ww (/ 1 60) [1 1] false false 0))
(check "blocked on X, the player still slides along Z"
       (> (- (get (sp :pos) 2) z-start) 1.0)
       (string "moved " (- (get (sp :pos) 2) z-start)))

#
# --- Step up ------------------------------------------------------------
#
(print "\nstep up:")
(def st (flat-world 10))
# A raised plateau, not a one-block-wide lip: crossing a lip would drop the
# player straight back down the far side and prove nothing about stepping.
# Extends to the edge of the loaded chunks so 90 frames of walking cannot run
# off the far side and leave the player mid-fall.
(for x 10 32
  (for z 0 16 (world/world-set-block st x 11 z world/block-stone)))
(def stp (player/player-new 8.5 11 8.5))
(put stp :flying false)
(player/update-physics stp st (/ 1 60) [0 0] false false 0)
(for i 0 90 (player/update-physics stp st (/ 1 60) [1 0] false false 0))
(check "the player climbs onto a one-block rise without jumping"
       (> (get (stp :pos) 1) 11.5)
       (string "y = " (get (stp :pos) 1)))
(check "and stands on top of the plateau"
       (near? (get (stp :pos) 1) 12 0.05)
       (string "y = " (get (stp :pos) 1)))
(check "and ends up past the rise"
       (> (get (stp :pos) 0) 10)
       (string "x = " (get (stp :pos) 0)))
# A two-block wall must still require a jump, or step-up is too permissive.
(def st2 (flat-world 10))
(for x 10 16
  (for y 11 13
    (for z 0 16 (world/world-set-block st2 x y z world/block-stone))))
(def stp2 (player/player-new 8.5 11 8.5))
(put stp2 :flying false)
(player/update-physics stp2 st2 (/ 1 60) [0 0] false false 0)
(for i 0 90 (player/update-physics stp2 st2 (/ 1 60) [1 0] false false 0))
(check "a two-block wall is NOT auto-climbed"
       (< (get (stp2 :pos) 0) 10)
       (string "x = " (get (stp2 :pos) 0)))

#
# --- Tunnelling ---------------------------------------------------------
#
(print "\ntunnelling:")
# A single-block-thin wall, with a huge displacement in one frame. A naive
# implementation that tests only the destination would pass straight through.
(def tw (flat-world 10))
(for y 11 14
  (for z 0 16 (world/world-set-block tw 12 y z world/block-stone)))
(def tp (player/player-new 8.5 11 8.5))
(put tp :flying false)
(player/move-and-collide tp tw 40 0 0)
(check "a 40-block step cannot tunnel through a thin wall"
       (< (get (tp :pos) 0) 12)
       (string "x = " (get (tp :pos) 0)))
(check "and the player is not left inside geometry"
       (not (player/box-collides? tw (get (tp :pos) 0) (get (tp :pos) 1)
                                  (get (tp :pos) 2))))

#
# --- Ceilings -----------------------------------------------------------
#
(print "\nceilings:")
(def cw (flat-world 10))
(for z 0 16 (for x 0 16 (world/world-set-block cw x 14 z world/block-stone)))
(def cp (player/player-new 8.5 11 8.5))
(put cp :flying false)
(player/update-physics cp cw (/ 1 60) [0 0] true false 0)
(for i 0 60 (player/update-physics cp cw (/ 1 60) [0 0] false false 0))
(check "jumping into a ceiling does not push through it"
       (< (+ (get (cp :pos) 1) player/player-height) 14.05)
       (string "head at " (+ (get (cp :pos) 1) player/player-height)))

#
# --- Flying -------------------------------------------------------------
#
(print "\nflying:")
(def fp (player/player-new 8.5 20 8.5))
(put fp :flying true)
(def fy (get (fp :pos) 1))
(for i 0 30 (player/update-physics fp w (/ 1 60) [0 0] false false 0))
(check "a flying player does not fall" (near? (get (fp :pos) 1) fy 0.001))
(for i 0 30 (player/update-physics fp w (/ 1 60) [0 0] false false 1))
(check "a flying player ascends on input" (> (get (fp :pos) 1) fy))
(check "a flying player is still blocked by solid blocks"
       (do (def bp (player/player-new 8.5 11 8.5))
           (put bp :flying true)
           (for i 0 60 (player/update-physics bp ww (/ 1 60) [1 0] false false 0))
           (< (get (bp :pos) 0) 10)))

#
# --- Water --------------------------------------------------------------
#
(print "\nwater:")
(def wat (flat-world 10))
(for z 0 16 (for x 0 16 (world/world-set-block wat x 11 z world/block-water)))
(check "water does not block movement"
       (not (player/box-collides? wat 8.5 11 8.5)))

#
# --- Spawn helper -------------------------------------------------------
#
(print "\nspawn placement:")
(def dp (player/player-new 8.5 0 8.5))
(player/drop-to-ground dp w)
(check "drop-to-ground lands on the surface"
       (near? (get (dp :pos) 1) 11 0.001)
       (string "y = " (get (dp :pos) 1)))
(check "drop-to-ground leaves the player clear of geometry"
       (not (player/box-collides? w (get (dp :pos) 0) (get (dp :pos) 1)
                                  (get (dp :pos) 2))))

#
# --- Throughput ---------------------------------------------------------
#
(print "\nthroughput:")
(def bp2 (player/player-new 8.5 11 8.5))
(put bp2 :flying false)
(def t0 (os/clock))
(for i 0 10000 (player/update-physics bp2 w (/ 1 60) [1 1] false false 0))
(def t1 (os/clock))
(printf "  info update-physics: %.4f ms per tick"
        (* 1000 (/ (- t1 t0) 10000)))

(printf "\n%s  (%d failure%s)"
        (if (zero? failures) "ALL CHECKS PASSED" "CHECKS FAILED")
        failures (if (= 1 failures) "" "s"))
(os/exit (if (zero? failures) 0 1))
