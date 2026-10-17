# B7.1: named process registry (erlang:register/2, whereis/1,
# unregister/1). `proc/register`/`proc/whereis`/`proc/unregister`
# are core primitives (src/core/proc.c); the actual "process
# completed" detection they rely on (proc/done?) is driven by the
# Janet-level reaper in examples/proc.janet's `monitor`, not by
# bare completion alone -- a watched proc only becomes proc/done?
# once something actually monitors it and the reaper runs. Every
# "after completion" case below therefore goes through
# `procx/monitor` + a short sleep, matching suite-proc.janet's own
# idiom for the same reason (see its test 11, "late monitors").
(import ./helper :prefix "" :exit true)
(import ../examples/proc :as procx)
(start-suite)

# 1. Register, whereis finds it, unregister, whereis nil.
((fn []
(def p (proc/spawn (fn [] (ev/sleep 1.0))))
(proc/register :svc-1 p)
(assert (= p (proc/whereis :svc-1)) "whereis finds the registered proc")
(proc/unregister :svc-1)
(assert (nil? (proc/whereis :svc-1)) "whereis nil after unregister")
(proc/kill p :done)
))

# 2. Double-register while the first proc is alive panics.
((fn []
(def p1 (proc/spawn (fn [] (ev/sleep 1.0))))
(def p2 (proc/spawn (fn [] (ev/sleep 1.0))))
(proc/register :svc-2 p1)
(assert-error "register refuses to overwrite a live registration"
              (proc/register :svc-2 p2))
(proc/kill p1 :done)
(proc/kill p2 :done)
))

# 3. First proc completes (via a real reaper run); re-registering the
# same name to a NEW proc succeeds without an explicit unregister.
((fn []
(def results @[])
(def p1 (proc/spawn (fn [] :bye)))
(def watcher (proc/spawn (fn [] (array/push results (proc/recv)))))
(proc/register :svc-3 p1)
(procx/monitor p1 watcher :tag3)
(ev/sleep 0.3)
(assert (proc/done? p1) "p1 completed (reaper ran)")
(def p2 (proc/spawn (fn [] (ev/sleep 1.0))))
(proc/register :svc-3 p2)
(assert (= p2 (proc/whereis :svc-3)) "re-register to a new proc succeeds")
(proc/kill watcher :done)
(proc/kill p2 :done)
))

# 4. whereis on a name whose registered proc has completed (no
# re-register) returns nil, not a stale handle -- the most important
# correctness property this feature has.
((fn []
(def results @[])
(def p (proc/spawn (fn [] :bye)))
(def watcher (proc/spawn (fn [] (array/push results (proc/recv)))))
(proc/register :svc-4 p)
(procx/monitor p watcher :tag4)
(ev/sleep 0.3)
(assert (nil? (proc/whereis :svc-4)) "whereis nil after completion, no stale handle")
(proc/kill watcher :done)
))

# 5. Light stress: ~50 register+complete+whereis-triggered-cleanup
# cycles; no crash, registry doesn't accumulate stale entries.
((fn []
(for i 0 50
  (def results @[])
  (def name (keyword (string "stress-" i)))
  (def p (proc/spawn (fn [] :bye)))
  (def watcher (proc/spawn (fn [] (array/push results (proc/recv)))))
  (proc/register name p)
  (procx/monitor p watcher name)
  (ev/sleep 0.02)
  (assert (nil? (proc/whereis name)) (string "stress cycle " i " cleaned up")))
(print "stress: 50 register/complete/whereis cycles clean")
))

(end-suite)
