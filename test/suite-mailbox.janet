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

(end-suite)
