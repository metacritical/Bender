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

# M4b-1: cooperative per-fiber interrupt (ev/interrupt). Delivery
# happens at the resume boundary, never mid-instruction. Each file
# runs in its own process, so no settings leak between suites.

# 1. Interrupt a new fiber: first resume yields the payload, the
# fiber never ran; second resume runs the body.
(let [f (fiber/new (fn [] :ran))]
  (ev/interrupt f :stop)
  (assert (= :stop (resume f)) "pending interrupt yields payload")
  (assert (= :ran (resume f)) "fiber runs after interrupt consumed"))

# 2. Default payload is nil.
(let [f (fiber/new (fn [] :ran))]
  (ev/interrupt f)
  (assert (nil? (resume f)) "default payload nil")
  (assert (= :ran (resume f)) "runs after nil interrupt"))

# 3. Interrupt wins over the continuation: a fiber suspended at yield
# observes the interrupt payload instead of continuing, then proceeds.
(let [f (fiber/new (fn [] (yield :first) :second))]
  (assert (= :first (resume f)) "suspends at yield")
  (ev/interrupt f :sig)
  (assert (= :sig (resume f)) "interrupt preempts continuation")
  (assert (= :second (resume f)) "continues after interrupt"))

# 4. Interrupts are never swallowed by pcall: delivery happens before
# a single instruction runs, outside any try region.
(let [f (fiber/new (fn [] (try (do (yield :in-try) :after) ([_] :caught))))]
  (assert (= :in-try (resume f)) "suspends inside try")
  (ev/interrupt f :sig)
  (assert (= :sig (resume f)) "interrupt not caught by try")
  (assert (= :after (resume f)) "try body continues"))

# 5. A newer request replaces a pending one.
(let [f (fiber/new (fn [] :ran))]
  (ev/interrupt f :old)
  (ev/interrupt f :new)
  (assert (= :new (resume f)) "latest interrupt wins"))

# 6. ev-loop integration: interrupting a task blocked on ev/take
# hijacks the rendezvous — the in-flight value is dropped (racing
# wakeup loses, documented) and the task parks exactly like a bare
# yield (no re-arm outstanding). The supervisor sees the yield.
# Pattern: interrupt + cancel = cooperative kill.
(let [ch (ev/chan 1)
      sup (ev/chan 10)
      done @[]
      t (ev/go (fn []
                 (array/push done (ev/take ch))
                 (array/push done (ev/take ch))) nil sup)]
  (ev/sleep 0.02)
  (ev/interrupt t :cancelled)
  (ev/give ch :v1)
  (ev/give ch :v2)
  (ev/sleep 0.05)
  (assert (= 1 (ev/count ch)) "v1 dropped, v2 still buffered")
  (assert (= 0 (length done)) "task parked, takes not completed")
  (assert (= :yield ((ev/take sup) 0)) "supervisor sees hijack yield")
  (ev/cancel t nil)
  (ev/sleep 0.02))

# 7. Supervisor observes an interrupt-yield like any yield. Order
# matters: the take would block until the sleep expires, so cancel
# first to force the resume that delivers the interrupt.
(let [sup (ev/chan 10)
      t (ev/go (fn [] (ev/sleep 100) :never) nil sup)]
  (ev/sleep 0.02)
  (ev/interrupt t :watchdog)
  (ev/cancel t :drop)
  (def [status] (ev/take sup))
  (assert (= :yield status) "supervisor sees yield on interrupt")
  (ev/cancel t nil))

(end-suite)
