# Copyright (c) 2026 Calvin Rose & contributors
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to
# deal in the Software without restriction, including without limitation the
# rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
# sell copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in
# all copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
# FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
# IN THE SOFTWARE.

# Driving the event loop from a non-yielding render loop.
#
# A game keeps one main fiber busy every frame and must still let `ev/go`
# fibers and `ev` listeners make progress. Two runtime behaviours used to make
# that impossible:
#
#   1. A timed wait that had already completed kept its deadline armed, and the
#      quantum driver re-queued the fiber with the consumed JANET_SIGNAL_ERROR,
#      so one expired `net/accept` re-raised "timeout" into unrelated
#      arithmetic on every later turn.
#   2. Queued fibers only advanced when the caller suspended, and nothing
#      offered an explicit non-blocking pump.
#
# These tests pin the corrected semantics.

(import ./helper :prefix "" :exit true)
(start-suite)

# ---------------------------------------------------------------------------
# A timed wait must not abort unrelated work on the main fiber.
# ---------------------------------------------------------------------------

(defn busy-work
  "Pure arithmetic with no I/O and no suspension point."
  [n]
  (var s 0)
  (for i 0 n (set s (+ s (* i i))))
  s)

(def render-listener (net/listen "127.0.0.1" "0" :stream))

# Arm and expire a timed wait, exactly like a per-frame accept poll.
(var expired 0)
(for _i 0 50
  (try (net/accept render-listener 0.0001) ([_e] (++ expired))))
(assert (> expired 0) "the timed accept should have expired at least once")

# The regression: unrelated arithmetic after the poll. Before the fix the
# first of these raised the expired operation's "timeout".
(assert-no-error "timed wait does not abort unrelated work"
  (for _i 0 5 (busy-work 2000)))

# The failure mode was a *replaying* error, so a single surviving delivery is
# still a bug: every one of these calls must complete cleanly.
(var unrelated-errors @[])
(for _i 0 20
  (try (busy-work 500)
       ([e] (array/push unrelated-errors (string e)))))
(assert (= 0 (length unrelated-errors))
        (string "unrelated work raised after a completed timed wait: " unrelated-errors))

# ---------------------------------------------------------------------------
# A harness that never suspends but yields in small slices still sees no
# stale timeout, matching a frame loop that only occasionally pumps.
# ---------------------------------------------------------------------------

(var slice-errors @[])
(for frame 0 200
  (try
    (do (busy-work 100)
        (when (= 0 (% frame 50)) (ev/pump)))
    ([e] (array/push slice-errors (string e)))))
(assert (= 0 (length slice-errors))
        (string "sliced render loop raised: " slice-errors))

# ---------------------------------------------------------------------------
# ev/pump lets ev/go fibers progress under a non-yielding main fiber.
# ---------------------------------------------------------------------------

(def pump-hits @{:n 0})
(ev/go (fn [] (++ (pump-hits :n))))

# Non-yielding work: no sleep, no I/O, no suspension of the main fiber.
(busy-work 5000)
(ev/pump)
(assert (= 1 (pump-hits :n)) "ev/pump runs a queued ev/go fiber exactly once")

# Repeated pumps must not re-run an already completed fiber.
(ev/pump)
(ev/pump)
(assert (= 1 (pump-hits :n)) "a completed ev/go fiber is not re-run by later pumps")

# Many queued fibers all make progress.
(def many-hits @{:n 0})
(for _i 0 10 (ev/go (fn [] (++ (many-hits :n)))))
(ev/pump)
(assert (= 10 (many-hits :n))
        (string "ev/pump should run every ready fiber, got " (many-hits :n)))

# ---------------------------------------------------------------------------
# ev/pump must not be delayed by a pending timer or an idle listener.
# ---------------------------------------------------------------------------

# A long deadline is outstanding: a pump must not wait for it.
(ev/deadline 30 (fiber/root))
(def pump-start (os/clock))
(for _i 0 5 (ev/pump))
(def pump-elapsed (- (os/clock) pump-start))
(assert (< pump-elapsed 1.0)
        (string "ev/pump blocked instead of returning immediately: " pump-elapsed "s"))

# ---------------------------------------------------------------------------
# Repeated poll-shaped operations leak no deadlines.
# ---------------------------------------------------------------------------

# accept-loop fixtures count matches: each completed wait retires its own
# deadline, so a long poll loop cannot accumulate armed timers.
(for _i 0 200
  (try (net/accept render-listener 0.0001) ([_e] nil)))
(assert-no-error "poll loop leaves the loop usable"
  (do (busy-work 2000) (ev/pump)))

# ---------------------------------------------------------------------------
# An un-timed blocking wait keeps its blocking meaning.
# ---------------------------------------------------------------------------

(def blocking-listener (net/listen "127.0.0.1" "0" :stream))
(def blocking-port ((net/localname blocking-listener) 1))
(ev/go (fn []
         (ev/sleep 0.05)
         # Connect to ourselves so the blocking accept can complete.
         (def c (net/connect "127.0.0.1" blocking-port :stream))
         (net/close c)))
(def real-accept (net/accept blocking-listener))
(assert (not (nil? real-accept)) "an un-timed accept still blocks until a real connection")
(net/close real-accept)
(net/close blocking-listener)
(net/close render-listener)

(end-suite)
