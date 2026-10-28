# Mutable-capture locality through the public proc/spawn wrapper.
#
# Placement rule (src/core/proc.c:195-257): a closure capturing a MUTABLE
# value (array/table/buffer/fiber/struct) spawns on the CALLING VM, because a
# lane crossing marshal-copies the closure and the actor's writes would be
# invisible. A closure capturing only abstracts/immediates may route.
#
# Two source facts this depends on:
#  * (type x) returns the abstract TYPE NAME (corelib.c:606-611); the local
#    proc type is "core/proc" (proc.c:1030), a routed handle is :tuple.
#    proc/pid? is true for BOTH shapes (proc.c:1554-1566), so it is not
#    used as a placement predicate.
#  * A top-level `def` is a module-level binding resolved by GLOBAL LOOKUP,
#    not a lexical capture - a closure reading one captures nothing mutable
#    and ROUTES (verified on the baseline: def -> tuple node=1, factory
#    parameter -> core/proc node=0). Cases 1-2 and 4-5 build their body with
#    a factory whose PARAMETERS are the captured values; case 3's `cap-body`
#    is a factory too. Where a `let` binding is read directly by an inline
#    `fn` (the no-worker arm) the value is still a lexical capture, but the
#    factory form is used wherever a mutable capture is the point of the
#    case, so capture is explicit rather than compiler-inferred.
#
#    Local placement is forced by the MUTABLE CAPTURE RULE, not by the plain
#    channel in `done`. A plain ev/chan in a local actor's capture set is
#    safe only because the actor is local; putting a plain channel in a
#    ROUTED closure would be the documented home-lane hazard and is out of
#    scope here. The one routed control (case 5) uses a threaded channel.
#    Placement is judged from (type h) alone. The pid tuple's node field is
#    used only in the routed? check, and node 0 == "this VM" is asserted
#    solely in this test's root/main-fiber context, never for arbitrary
#    child fibers.
#
# Run both arms under an outer timeout:
#   JANET_LANES=0 JANET_NO_PROFILE=1 ./build/bender test/suite-proc-capture-locality.janet
#   JANET_LANES=2 JANET_NO_PROFILE=1 ./build/bender test/suite-proc-capture-locality.janet

(defn local? [h] (= :core/proc (type h)))
(defn routed? [h] (and (= :tuple (type h)) (= :pid (h 0)) (>= (h 1) 1)))

(var started-here false)
(def want-lanes (not= "0" (or (os/getenv "JANET_LANES") "")))
(when (and want-lanes (zero? (proc/lanes)))
  (proc/lanes-start 1)
  (set started-here true))
(def lanes (proc/lanes))
(eprintf "phase=lanes=%d" lanes)
(when want-lanes (assert (>= lanes 1) "active-lane arm needs a worker lane"))

# Factories: parameters are lexical upvalues, so the capture is guaranteed.
(defn table-body [box ready gate done]
  (fn [] (ev/give ready true)           # announce: actor is running
         (ev/take gate)                 # then park until the parent releases
         (put box :mutated true)
         (put box :count (inc (get box :count)))
         (ev/give done (get box :count))))
(defn array-body [box ready gate done]
  (fn [] (ev/give ready true)
         (ev/take gate)
         (array/push box :from-actor)
         (ev/give done (length box))))
(defn cap-body [box done]
  (fn [i] (fn [] (put box :n (+ (get box :n) i)) (ev/give done i))))
(defn tail-body [box done]
  (fn [] (array/push box :tail) (ev/give done (length box))))
(defn chan-body [reply] (fn [] (ev/give reply :routed)))

# 1. table capture, GC while the actor is parked before it mutates.
(let [box @{:count 0} ready (ev/chan 1) gate (ev/thread-chan 1) done (ev/chan 1)]
  (def p (proc/spawn (table-body box ready gate done) 8))
  (eprintf "phase=table type=%v" (type p))
  (assert (local? p) "table capture must spawn locally")
  (assert (proc/pid? p) "a local handle must satisfy proc/pid?")
  (assert (not (routed? p)) "table capture must not route")
  # Handshake: the actor has RUN and signalled; it now parks in ev/take gate.
  (assert (= true (ev/take ready)) "actor must signal readiness before gate")
  # Only now, with the actor suspended, force collections.
  (gccollect) (gccollect)
  (ev/give gate true)
  (assert (= 1 (ev/take done)) "actor must report count 1")
  (assert (= true (get box :mutated)) "parent must see the actor's write")
  (assert (= 1 (get box :count)) "capture must be the same object"))

# 2. array capture with the same handshake.
(let [box @[] ready (ev/chan 1) gate (ev/thread-chan 1) done (ev/chan 1)]
  (def p (proc/spawn (array-body box ready gate done) 8))
  (assert (local? p) "array capture must spawn locally")
  (assert (proc/pid? p) "a local handle must satisfy proc/pid?")
  (assert (= true (ev/take ready)) "array actor must signal readiness")
  (gccollect) (gccollect)
  (ev/give gate true)
  (assert (= 1 (ev/take done)) "actor must report one element")
  (assert (= :from-actor (get box 0)) "parent must see the pushed element"))

# 3. omitted vs explicit capacity, one shared mutable capture.
(let [box @{:n 0} done (ev/chan 2) mk (cap-body box done)]
  (def p1 (proc/spawn (mk 1)))
  (assert (local? p1) "default capacity must keep local placement")
  (assert (proc/pid? p1) "a local handle must satisfy proc/pid?")
  (def p2 (proc/spawn (mk 2) 32))
  (assert (local? p2) "explicit capacity must keep local placement")
  (assert (proc/pid? p2) "a local handle must satisfy proc/pid?")
  # Completion ORDER is unspecified between two local actors: assert set
  # membership, never "first is 1, second is 2".
  (def a (ev/take done))
  (def b (ev/take done))
  (assert (and (number? a) (number? b)) "both variants must report a number")
  (assert (or (and (= 1 a) (= 2 b)) (and (= 2 a) (= 1 b)))
          "both capacity variants must report exactly 1 and 2, in any order")
  (assert (= 3 (get box :n)) "both variants must mutate the shared capture"))

# 4. tail-call wrapper preserves placement.
(defn spawn-tail [f cap] (proc/spawn f cap))
(let [box @[] done (ev/chan 1)]
  (def p (spawn-tail (tail-body box done) 8))
  (assert (local? p) "tail-call wrapper must preserve local placement")
  (assert (proc/pid? p) "a local handle must satisfy proc/pid?")
  (assert (= 1 (ev/take done)) "tail-spawned actor must run")
  (assert (= 1 (length box)) "tail actor must mutate the capture"))

# 5. abstract-only control (a channel, not a mutable value) must route.
(when want-lanes
  (let [reply (ev/thread-chan 1)]
    (def p (proc/spawn (chan-body reply) 8))
    (eprintf "phase=abstract-only type=%v" (type p))
    (assert (routed? p) "abstract-only capture must route when lanes are active")
    (assert (proc/pid? p) "a routed handle must satisfy proc/pid?")
    (assert (= :routed (ev/take reply)) "routed actor must actually reply")))

# 6. no-worker arm: everything is local.
(when (zero? lanes)
  (let [reply (ev/chan 1)]
    (def p (proc/spawn (chan-body reply) 8))
    (assert (local? p) "with no worker lanes every handle is local")
    (assert (proc/pid? p) "a local handle must satisfy proc/pid?")
    (assert (= :routed (ev/take reply)) "no-worker actor must still run")))

# Cleanup runs on the success path. On assertion failure the process exits
# nonzero and the outer timeout kills it; lanes need no explicit teardown in
# that case because the whole process is going away.
(when (and started-here (> (proc/lanes) 0)) (proc/lanes-stop))
(eprintf "phase=done lanes=%d" (proc/lanes))
(print "suite-proc-capture-locality PASSED")
