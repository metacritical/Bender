# Copyright (c) 2026 Calvin Rose
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

(import ./helper :prefix "" :exit true)
(start-suite)

# M1: worker-mailbox pattern (examples/mailbox.janet) on top of the
# phase 3 thread primitives. Each file runs in its own process, so no
# settings leak between suites.

(import ../examples/mailbox :as mailbox)

# 1. Basic call round-trips across the thread boundary.
(let [mb (mailbox/spawn (fn [tag payload] [tag payload]))]
  (assert (deep= [:add 3] (mailbox/call mb :add 3)) "call round-trip")
  (assert (deep= [:tbl @{:k "v"}] (mailbox/call mb :tbl @{:k "v"})) "table payload marshals")
  (mailbox/quit mb))

# 2. Worker-local closure state persists across calls (share-nothing:
# the state lives in the worker's copy of the closure).
(let [mb (mailbox/spawn
           (do
             (var n 0)
             (fn [tag payload]
               (case tag
                 :inc (set n (+ n payload))
                 :get n)))) ]
  (mailbox/send mb :inc 5)
  (mailbox/send mb :inc 7)
  (assert (= 12 (mailbox/call mb :get nil 5)) "worker closure state")
  (mailbox/quit mb))

# 3. Handler errors re-raise in the caller, worker stays alive.
(let [mb (mailbox/spawn (fn [_tag _payload] (error "boom")))]
  (assert-error-value "handler error surfaces" "boom" (mailbox/call mb :x nil 5))
  (assert-error-value "worker still alive" "boom" (mailbox/call mb :x nil 5))
  (mailbox/quit mb))

# 4. call timeout fires when the handler is slow.
(let [mb (mailbox/spawn (fn [_tag _payload] (ev/sleep 1) :slow))]
  (assert-error "call timeout" (mailbox/call mb :x nil 0.05))
  (mailbox/quit mb))

# 5. Concurrent senders: messages serialize through the worker.
(let [mb (mailbox/spawn
           (do
             (var n 0)
             (fn [tag payload]
               (case tag
                 :add (set n (+ n payload))
                 :get n))))]
  (def done (ev/chan 2))
  (each _ (range 2)
    (ev/go (fn []
             (each i (range 50) (mailbox/send mb :add i))
             (ev/give done :done))))
  # wait for both producers to finish enqueueing, then read the total
  (ev/take done)
  (ev/take done)
  (assert (= 2450 (mailbox/call mb :get nil 5)) "concurrent senders")
  (mailbox/quit mb))

# 6. Backpressure: capacity 1 + slow handler still delivers everything
# (sends block, they do not drop).
(let [mb (mailbox/spawn
           (do
             (var n 0)
             (fn [tag _payload]
               (ev/sleep 0.01)
               (case tag
                 :tick (set n (+ n 1))
                 :count n)))
           1)]
  (each i (range 5) (mailbox/send mb :tick i))
  (assert (= 5 (mailbox/call mb :count nil 5)) "backpressure capacity 1")
  (mailbox/quit mb))

# 7. Lifecycle: alive? transitions and quit acknowledgement.
(let [mb (mailbox/spawn (fn [_tag payload] payload))]
  (assert (thread/alive? (mb :worker)) "worker alive after spawn")
  (mailbox/quit mb)
  (assert (not (thread/alive? (mb :worker))) "worker joined after quit"))

# 8. A dead worker makes calls time out (at-most-once; sends block at
# capacity -- no drop, no error, until M2 monitors).
(let [mb (mailbox/spawn (fn [_tag payload] payload))]
  (mailbox/quit mb)
  (assert-error "call to dead worker" (mailbox/call mb :x nil 0.05)))

# M2: monitors and links (ev/monitor runtime + mailbox sugar).

# 9. Raw monitor: normal exit delivers [:down tag :normal]; join still
# collects the value afterwards.
(let [ch (ev/thread-chan 4)
      h (thread/new (fn [] (ev/sleep 0.02) :fine))]
  (ev/monitor h :t9 ch)
  (assert (deep= [:down :t9 :normal] (ev/take ch)) "monitor normal exit")
  (assert (= :fine (thread/join h)) "join after monitor"))

# 10. Error exit delivers the error value as the reason, and join
# still re-raises.
(let [ch (ev/thread-chan 4)
      h (thread/new (fn [] (error "went-bad")))]
  (ev/monitor h :t10 ch)
  (assert (deep= [:down :t10 "went-bad"] (ev/take ch)) "monitor error exit")
  (assert-error-value "join re-raises" "went-bad" (thread/join h)))

# 11. Multiple monitors on one handle each get their own message.
(let [c1 (ev/thread-chan 4)
      c2 (ev/thread-chan 4)
      h (thread/new (fn [] :multi))]
  (ev/monitor h :m1 c1)
  (ev/monitor h :m2 c2)
  (assert (deep= [:down :m1 :normal] (ev/take c1)) "first monitor fires")
  (assert (deep= [:down :m2 :normal] (ev/take c2)) "second monitor fires")
  (thread/join h))

# 12. Late monitor on a finished thread delivers immediately.
(let [ch (ev/thread-chan 4)
      h (thread/new (fn [] 7))]
  (thread/join h)
  (ev/monitor h :t12 ch)
  (assert (deep= [:down :t12 :normal] (ev/take ch)) "late monitor"))

# 13. Mailbox-level monitor: the watcher handler observes
# (:down [monitor-tag reason]).
(let [watched (mailbox/spawn (fn [_t p] p))
      watcher (mailbox/spawn
                (do (var seen nil)
                    (fn [tag payload]
                      (case tag
                        :down (set seen payload)
                        :last seen))))]
  (mailbox/monitor watched watcher :w13)
  (mailbox/quit watched)
  (assert (deep= [:w13 :normal] (mailbox/call watcher :last nil 5))
          "watcher sees peer :normal exit")
  (mailbox/quit watcher))

# 14. Link chain: B dies, A is notified; A keeps working.
(let [a (mailbox/spawn
          (do (var seen nil)
              (fn [tag payload]
                (case tag
                  :down (set seen payload)
                  :last seen
                  :ping :pong))))
      b (mailbox/spawn (fn [_t p] p))]
  (mailbox/link a b :l14)
  (mailbox/quit b)
  (assert (deep= [:l14 :normal] (mailbox/call a :last nil 5)) "link notifies")
  (assert (= :pong (mailbox/call a :ping nil 5)) "survivor keeps working")
  (mailbox/quit a))

# M2b: monitor refs + ev/demonitor.

# 15. ev/monitor returns an integer ref; demonitor-then-exit delivers
# nothing (channel stays empty after join).
(let [ch (ev/thread-chan 4)
      h (thread/new (fn [] (ev/sleep 0.02) :gone))]
  (def ref (ev/monitor h :t15 ch))
  (assert (int? ref) "monitor returns integer ref")
  (ev/demonitor h ref)
  (assert (= :gone (thread/join h)) "join still works")
  (assert (= 0 (ev/count ch)) "demonitored monitor is silent"))

# 16. Double demonitor and unknown refs are silent no-ops.
(let [ch (ev/thread-chan 4)
      h (thread/new (fn [] :twice))]
  (def ref (ev/monitor h :t16 ch))
  (ev/demonitor h ref)
  (ev/demonitor h ref)
  (ev/demonitor h 99)
  (ev/demonitor h -1)
  (thread/join h)
  (assert (= 0 (ev/count ch)) "double/unknown demonitor silent"))

# 17. Demonitor after completion is a silent no-op; the already-fired
# message is still there.
(let [ch (ev/thread-chan 4)
      h (thread/new (fn [] :early))]
  (def ref (ev/monitor h :t17 ch))
  (thread/join h)
  (ev/demonitor h ref)
  (assert (deep= [:down :t17 :normal] (ev/take ch)) "post-join demonitor keeps message"))

# 18. Survivors still fire when a sibling monitor is removed.
(let [c1 (ev/thread-chan 4)
      c2 (ev/thread-chan 4)
      h (thread/new (fn [] (ev/sleep 0.02) :both))]
  (def r1 (ev/monitor h :gone c1))
  (def r2 (ev/monitor h :kept c2))
  (assert (not= r1 r2) "refs are distinct slots")
  (ev/demonitor h r1)
  (thread/join h)
  (assert (= 0 (ev/count c1)) "removed monitor silent")
  (assert (deep= [:down :kept :normal] (ev/take c2)) "survivor fires"))

# 19. Late registration (already-finished thread) delivers immediately
# and returns nil — there is no live slot to demonitor.
(let [ch (ev/thread-chan 4)
      h (thread/new (fn [] 9))]
  (thread/join h)
  (assert (nil? (ev/monitor h :t19 ch)) "late monitor returns nil")
  (assert (deep= [:down :t19 :normal] (ev/take ch)) "late monitor delivers"))

# M2c: cooperative trap-exit.

# Helper: wait (bounded) for a mailbox worker to exit. Cascades are
# async — a fixed sleep is flaky under load, polling is not.
(defn- await-exit [mb timeout]
  (var t 0)
  (while (and (thread/alive? (mb :worker)) (< t timeout))
    (ev/sleep 0.02)
    (set t (+ t 0.02)))
  (not (thread/alive? (mb :worker))))

# 20. A trapping worker observes an abnormal peer exit as
# (:down [tag reason]) and survives.
(let [w (mailbox/spawn
          (do (var seen nil)
              (fn [tag payload]
                (case tag
                  :down (set seen payload)
                  :last seen
                  :ping :pong))))]
  (mailbox/trap-exit w true)
  (def seed (thread/new (fn [] (ev/sleep 0.02) (error "seed-boom"))))
  (ev/monitor seed :s (w :mbx))
  (assert-error-value "seed dies" "seed-boom" (thread/join seed))
  (assert (deep= [:s "seed-boom"] (mailbox/call w :last nil 5))
          "trapper observes abnormal exit")
  (assert (= :pong (mailbox/call w :ping nil 5)) "trapper survives")
  (mailbox/quit w))

# 21. A non-trapping linked worker exits with the peer's reason.
(let [a (mailbox/spawn (fn [_t p] p))
      b (mailbox/spawn (fn [_t p] p))
      watch (ev/thread-chan 4)]
  (mailbox/link a b :l21)
  (ev/monitor (a :worker) :wa watch)
  # seed simulates link traffic: a linked peer's death arrives marked.
  (def seed (thread/new (fn [] (ev/sleep 0.02) (error "pair-boom"))))
  (ev/monitor seed [:link :s] (b :mbx))
  (protect (thread/join seed))
  (assert (await-exit b 5) "linked worker dies")
  (assert (await-exit a 5) "death propagates to peer")
  (assert (deep= [:down :wa "pair-boom"] (ev/take watch))
          "propagated reason preserved"))

# 22. Cascade A->B->C preserves the seed reason end to end.
(let [a (mailbox/spawn (fn [_t p] p))
      b (mailbox/spawn (fn [_t p] p))
      c (mailbox/spawn (fn [_t p] p))
      watch (ev/thread-chan 4)]
  (mailbox/link a b :lab)
  (mailbox/link b c :lbc)
  (ev/monitor (a :worker) :wa watch)
  # seed simulates link traffic (see test 21).
  (def seed (thread/new (fn [] (ev/sleep 0.02) (error "deep"))))
  (ev/monitor seed [:link :s] (c :mbx))
  (protect (thread/join seed))
  (assert (await-exit c 5) "seed peer dies")
  (assert (await-exit b 5) "middle dies")
  (assert (await-exit a 5) "cascade reaches A")
  (assert (deep= [:down :wa "deep"] (ev/take watch)) "cascade reason intact"))

# 23. :normal exits never propagate: quitting one linked peer leaves
# the other alive and observing.
(let [a (mailbox/spawn
          (do (var seen nil)
              (fn [tag payload]
                (case tag
                  :down (set seen payload)
                  :last seen
                  :ping :pong))))
      b (mailbox/spawn (fn [_t p] p))]
  (mailbox/link a b :l23)
  (mailbox/quit b)
  (assert (thread/alive? (a :worker)) "peer survives :normal exit")
  (assert (deep= [:l23 :normal] (mailbox/call a :last nil 5))
          "normal exit observed, not propagated")
  (assert (= :pong (mailbox/call a :ping nil 5)) "peer keeps working")
  (mailbox/quit a))

# M2d: link composition (no new mechanism — pins on monitor+trap).

# 24. Linking with an already-dead peer: the late path delivers an
# immediate :down to the survivor, which stays alive and working.
(let [a (mailbox/spawn
          (do (var seen nil)
              (fn [tag payload]
                (case tag
                  :down (set seen payload)
                  :last seen
                  :ping :pong))))
      b (mailbox/spawn (fn [_t p] p))]
  (mailbox/quit b)
  (mailbox/link a b :l24)
  (assert (deep= [:l24 :normal] (mailbox/call a :last nil 5))
          "dead-peer link notifies immediately")
  (assert (thread/alive? (a :worker)) "survivor alive")
  (assert (= :pong (mailbox/call a :ping nil 5)) "survivor works")
  (mailbox/quit a))

# 25. Double link installs two independent monitors: quitting the peer
# notifies twice, consistently.
(let [a (mailbox/spawn
          (do (var n 0)
              (var seen nil)
              (fn [tag payload]
                (case tag
                  :down (do (set seen payload) (set n (+ n 1)))
                  :last seen
                  :count n))))
      b (mailbox/spawn (fn [_t p] p))]
  (mailbox/link a b :l25)
  (mailbox/link a b :l25)
  (mailbox/quit b)
  (assert (= 2 (mailbox/call a :count nil 5)) "both link legs fire")
  (assert (deep= [:l25 :normal] (mailbox/call a :last nil 5))
          "link tags consistent")
  (mailbox/quit a))

# M2e: pids.

# 26. Pid shape, predicate, and distinct serials.
(let [a (mailbox/spawn (fn [_t p] p))
      b (mailbox/spawn (fn [_t p] p))]
  (def pa (a :pid))
  (def pb (b :pid))
  (assert (mailbox/pid? pa) "pid predicate")
  (assert (not (mailbox/pid? a)) "mailbox is not a pid")
  (assert (not (mailbox/pid? [:pid 0 1])) "short tuple is not a pid")
  (assert (= :pid (pa 0)) "pid head")
  (assert (= 0 (pa 1)) "single-node id")
  (assert (not= (pa 2) (pb 2)) "serials distinct")
  (assert (not (deep= pa pb)) "pids distinct")
  (mailbox/quit a)
  (mailbox/quit b))

# 27. self() inside a worker is its pid; outside workers it is nil.
(let [w (mailbox/spawn (fn [t p] (case t :who (mailbox/self) :echo p)))]
  (assert (deep= (w :pid) (mailbox/call w :who nil 5)) "self is stable pid")
  (assert (nil? (mailbox/self)) "self outside workers is nil")
  (mailbox/quit w))

# 28. Pids round-trip through marshal by value (channel identity kept).
(let [w (mailbox/spawn (fn [t p] (case t :who (mailbox/self) :echo p)))]
  (def pid (w :pid))
  (assert (deep= pid (mailbox/call w :echo pid 5)) "pid marshal round-trip")
  (mailbox/quit w))

# 29. send/call accept pids.
(let [w (mailbox/spawn (fn [t p] (case t :who (mailbox/self) :echo p)))]
  (def pid (w :pid))
  (mailbox/send pid :echo :ping)
  (assert (deep= pid (mailbox/call pid :who nil 5)) "call by pid")
  (mailbox/quit pid))

# 30. Monitor by pid: live worker watched through its pid.
(let [watched (mailbox/spawn (fn [_t p] p))
      watcher (mailbox/spawn
                (do (var seen nil)
                    (fn [tag payload]
                      (case tag
                        :down (set seen payload)
                        :last seen))))]
  (mailbox/monitor (watched :pid) watcher :wp30)
  (mailbox/quit watched)
  (assert (deep= [:wp30 :normal] (mailbox/call watcher :last nil 5))
          "monitor by pid")
  (mailbox/quit watcher))

# 31. Unknown pid monitors deliver :noproc immediately, and the
# watcher — not trapping — survives: monitor notifications never
# propagate, only link legs do.
(let [watcher (mailbox/spawn
                (do (var seen nil)
                    (fn [tag payload]
                      (case tag
                        :down (set seen payload)
                        :last seen
                        :ping :pong))))]
  (mailbox/monitor [:pid 0 9999 (ev/thread-chan 1)] watcher :ghost)
  (assert (deep= [:ghost :noproc] (mailbox/call watcher :last nil 5))
          "unknown pid gives :noproc")
  (assert (= :pong (mailbox/call watcher :ping nil 5))
          "watcher survives :noproc")
  (mailbox/quit watcher))

# 32. Plain monitor notifications of abnormal exits are observed, not
# propagated — even by non-trapping workers.
(let [watcher (mailbox/spawn
                (do (var seen nil)
                    (fn [tag payload]
                      (case tag
                        :down (set seen payload)
                        :last seen
                        :ping :pong))))]
  (def seed (thread/new (fn [] (ev/sleep 0.02) (error "plain-boom"))))
  (ev/monitor seed :m32 (watcher :mbx))
  (protect (thread/join seed))
  (assert (deep= [:m32 "plain-boom"] (mailbox/call watcher :last nil 5))
          "abnormal monitor notification observed")
  (assert (= :pong (mailbox/call watcher :ping nil 5))
          "non-trapper survives monitor traffic")
  (mailbox/quit watcher))

# M3: named registry (ev/register/whereis/unregister + sugar).

# 33. Pid round-trips through the registry by value.
(let [w (mailbox/spawn (fn [_t p] p))]
  (mailbox/register w :m33)
  (assert (deep= (w :pid) (mailbox/whereis :m33)) "registry pid round-trip")
  (assert (nil? (mailbox/whereis :m33-missing)) "missing name is nil")
  (mailbox/quit w)
  (ev/unregister :m33))

# 34. Overwrite wins; unregister removes (true) and is false when absent.
(ev/register :m34 :first)
(ev/register :m34 :second)
(assert (= :second (ev/whereis :m34)) "overwrite wins")
(assert (ev/unregister :m34) "unregister present")
(assert (nil? (ev/whereis :m34)) "removed name is nil")
(assert (not (ev/unregister :m34)) "unregister absent is false")

# 35. Cross-thread visibility both directions.
(let [sig (ev/thread-chan 1)
      h (thread/new (fn [c] (ev/register :m35-w :wval) (ev/give c :done)) sig)]
  (ev/take sig)
  (thread/join h)
  (assert (= :wval (ev/whereis :m35-w)) "worker registration visible")
  (ev/unregister :m35-w))
(ev/register :m35-m :mval)
(let [sig (ev/thread-chan 1)
      h (thread/new (fn [c] (ev/give c (ev/whereis :m35-m))) sig)]
  (assert (= :mval (ev/take sig)) "worker sees main registration")
  (thread/join h)
  (ev/unregister :m35-m))

# 36. Unmarshalable values raise without wedging the registry.
(let [h (thread/new (fn [] :x))]
  (assert-error "handle refused" (ev/register :m36 h))
  (thread/join h))
(assert (= :ok (do (ev/register :m36 :ok) (ev/whereis :m36)))
        "registry usable after refused value")
(ev/unregister :m36)

# 37. Named send: address a worker by name, call through the looked-up pid.
(let [phys (mailbox/spawn (fn [t p] (case t :step (+ p 1) :echo p)))]
  (mailbox/register phys :m37-phys)
  (def target (mailbox/whereis :m37-phys))
  (assert (= 43 (mailbox/call target :step 42 5)) "call via registry pid")
  (mailbox/quit phys)
  (ev/unregister :m37-phys))

(end-suite)
