# Shared C stacks (P3.6/S2): ev-task fibers share one lane mapping
# instead of private mappings. Run with JANET_SHARED_STACKS=1
# (without it these pin private-mode behavior where noted).
#
# Each file runs in its own process.

(import ./helper :prefix "" :exit true)
(start-suite)

(when (nil? (os/getenv "JANET_SHARED_STACKS"))
  (print "SKIP suite-shared.janet (needs JANET_SHARED_STACKS=1)")
  (os/exit 0))

# 1. Shared tasks run, suspend (sleep), and wake with exact values.
(let [got @[]]
  (ev/go (fn [] (array/push got :ran) (ev/sleep 0.05) (array/push got :woke)))
  (ev/sleep 0.2)
  (assert (deep= @[:ran :woke] got) "suspend/wake exact"))

# 2. Channel take/give across suspend works.
(let [c (ev/chan 1)
      got @[]]
  (ev/go (fn [] (array/push got (ev/take c))))
  (ev/sleep 0.05)
  (ev/give c :hello)
  (ev/sleep 0.1)
  (assert (deep= @[:hello] got) "take/give exact"))

# 3. Errors propagate out of shared tasks (supervisor sees them).
(let [sup (ev/chan 8)]
  (ev/go (fn [] (ev/sleep 0.01) (error "shared-boom")) nil sup)
  (def [sig _ _] (ev/take sup))
  (assert (= sig :error) "supervisor sees error"))

# 4. Quanta preempt shared hogs (sleeper beats infinite loop).
(ev/timeslice true)
(let [order @[]
      hog (ev/go (fn [] (var acc 0) (while true (+= acc 1))))]
  (ev/go (fn [] (ev/sleep 0.05) (array/push order :short)))
  (ev/sleep 0.4)
  (assert (= :short (get order 0)) "preemption under shared")
  (ev/cancel hog :done))
(ev/timeslice false)

# 5. Nested resume of completed children works (generators driven
# from shared tasks are fine); nested SUSPEND under a shared
# ancestor fails loud (abandoned C frames would be clobbered).
(let [gen (fiber/new (fn [] :gen-done))]
  (assert (= :gen-done ((fn [] (resume gen)))) "nested complete works"))
(let [hit @[]
      c (ev/chan 1)]
  (ev/go (fn []
           (try (resume (fiber/new (fn [] (ev/take c))))
                ([e] (array/push hit e)))))
  (ev/sleep 0.2)
  (assert (= 1 (length hit)) "nested suspend panics")
  (assert (string/has-prefix? "nested fiber suspended under a shared-stack"
                              (get hit 0))
          "panic names the hazard"))

# 6. Manual fibers stay private (flag only set by ev/go + proc spawn).
(let [f (fiber/new (fn [] :manual))]
  (assert (= :manual (resume f)) "manual resume exact"))

# 7. Proc tasks run shared (spawn/send/recv/quit round-trip).
(let [results @[]
      p (proc/spawn (fn []
                      (array/push results (proc/recv))
                      (array/push results (proc/recv))))]
  (ev/sleep 0.05)
  (proc/send p :one)
  (proc/send p :two)
  (ev/sleep 0.2)
  (assert (deep= @[:one :two] results) "proc round-trip shared")
  (proc/kill p :done))

# 8. Deep C recursion (marshal of 30k-nested arrays) attributes
# overflow to the running task (catchable via the D3 flag path);
# the lane survives. Same behavior private and shared.
(let [hit @[]]
  (var deep @[])
  (repeat 30000 (set deep @[deep]))
  (ev/go (fn []
           (def [ok _] (protect (marshal deep)))
           (array/push hit [ok (if ok :marshalled :overflowed)])))
  (ev/sleep 1.0)
  (assert (= 1 (length hit)) "overflow observed, lane alive")
  (assert (not ((get hit 0) 0)) "overflow is an error, not a value"))

(end-suite)
