# Proc processes, P1.1: first-class process objects (same-lane raw
# processes). Tasklets stay untouched. Each file runs in its own
# process.

(import ./helper :prefix "" :exit true)
(start-suite)

# 1. Spawn returns a proc; self binds inside, nil outside.
(assert (nil? (proc/self)) "self outside is nil")
(let [seen @[]
      p (proc/spawn (fn [] (array/push seen (proc/self))))]
  (assert (proc/pid? p) "spawn returns a proc")
  (assert (not (proc/pid? :nope)) "pid? rejects non-procs")
  (ev/sleep 0.05)
  (assert (= 1 (length seen)) "body ran")
  (assert (= p (get seen 0)) "self is the proc")
  (assert (not (proc/alive? p)) "completed body is not alive"))

# 2. Send/recv round-trip in order (single-value messages).
(let [got @[]
      p (proc/spawn (fn [] (repeat 3 (array/push got (proc/recv)))))]
  (ev/sleep 0.05)
  (proc/send p :a)
  (proc/send p :b)
  (proc/send p :c)
  (ev/sleep 0.1)
  (assert (deep= @[:a :b :c] got) "ordered delivery")
  (proc/kill p :done))

# 3. Isolation: payloads marshal-copy, never alias.
(let [got @[]
      p (proc/spawn (fn [] (array/push got (proc/recv))))]
  (ev/sleep 0.05)
  (def arr @[1 2 3])
  (proc/send p arr)
  (array/push arr 99)
  (ev/sleep 0.1)
  (assert (deep= @[1 2 3] (get got 0)) "copy, not alias")
  (assert (= 4 (length arr)) "sender side intact")
  (proc/kill p :done))

# 4. Recv suspends on empty, wakes on send.
(let [got @[]
      p (proc/spawn (fn [] (array/push got (proc/recv))))]
  (ev/sleep 0.05)
  (ev/go (fn [] (ev/sleep 0.05) (proc/send p :late)))
  (ev/sleep 0.2)
  (assert (deep= @[:late] got) "suspended recv wakes")
  (proc/kill p :done))

# 5. Kill flips alive; dead procs are a silent no-op.
(let [p (proc/spawn (fn [] (ev/sleep 30)))]
  (ev/sleep 0.05)
  (assert (proc/alive? p) "sleeper alive")
  (proc/kill p :killed)
  (ev/sleep 0.1)
  (assert (not (proc/alive? p)) "killed proc dead")
  (proc/kill p :again))

# 6. Procs refuse marshal (same-VM only, like pool handles).
(let [p (proc/spawn (fn [] :x))]
  (assert-error "proc refuses marshal" (marshal p))
  (proc/kill p :done))

# 7. Discipline: recv outside errors; stealing other mailboxes errors.
(assert-error "recv outside a proc" (proc/recv))
(let [a (proc/spawn (fn [] (ev/sleep 30)))
      b (proc/spawn (fn [] (proc/recv a)))]
  (ev/sleep 0.2)
  (assert (not (proc/alive? b)) "mailbox thief errors")
  (proc/kill a :done)
  (proc/kill b :done))

(import ../examples/proc :as procx)

# 8. P1.2 selective receive: out-of-order arrival, in-order match,
# non-matching stashed (C stash, Janet-scanned).
(let [results @[]
      c (proc/spawn (fn []
                      (array/push results (procx/select (fn [m] (= :a m))))
                      (array/push results (procx/select (fn [m] (= :b m))))
                      (array/push results (procx/select (fn [m] (= :c m))))))]
  (proc/send c :b)
  (proc/send c :c)
  (proc/send c :a)
  (ev/sleep 0.3)
  (assert (deep= @[:a :b :c] results) "selective order")
  (assert (= 0 (length (proc/stash c))) "stash drained")
  (proc/kill c :done))

# 9. Stash holds skipped messages across selects.
(let [results @[]
      c (proc/spawn (fn []
                      (array/push results (procx/select (fn [m] (= :b m))))
                      (array/push results (procx/select (fn [m] (= :a m))))))]
  (proc/send c :a)
  (proc/send c :b)
  (ev/sleep 0.3)
  (assert (deep= @[:b :a] results) "stashed message found later")
  (proc/kill c :done))

# 10. P1.2 monitors: crash observed with reason, clean exit :normal.
(let [results @[]
      crasher (proc/spawn (fn [] (ev/sleep 0.05) (error "bang")))
      cleaner (proc/spawn (fn [] :bye))
      watcher (proc/spawn (fn [] (repeat 2 (array/push results (proc/recv)))))]
  (procx/monitor crasher watcher :w)
  (procx/monitor cleaner watcher :c)
  (ev/sleep 1.0)
  (def downs (sorted
               (filter (fn [m] (and (tuple? m) (= :down (m 0)))) results)))
  (assert (= 2 (length downs)) "both downs")
  (assert (deep= [:down :c :normal] (downs 0)) "clean :normal")
  (assert (deep= [:down :w "bang"] (downs 1)) "crash reason")
  (proc/kill watcher :done))

# 11. Late monitors deliver from the retained completion event.
# The process completes long before anyone watches (no reaper yet,
# so done? is false); monitoring afterwards still delivers :normal.
(let [results @[]
      quick (proc/spawn (fn [] :bye))
      watcher (proc/spawn (fn [] (array/push results (proc/recv))))]
  (ev/sleep 0.5)
  (assert (not (proc/alive? quick)) "quick completed")
  (procx/monitor quick watcher :late)
  (ev/sleep 0.5)
  (assert (deep= @[[:down :late :normal]] results) "late :down")
  (assert (proc/done? quick) "exit recorded by reaper")
  (proc/kill watcher :done))

# 12. Links are symmetric fate with [:link tag] marks.
(let [results @[]
      a (proc/spawn (fn [] (ev/sleep 30)))
      b (proc/spawn (fn [] (array/push results (proc/recv))))]
  (procx/link a b :lab)
  (proc/kill a :boom)
  (ev/sleep 0.6)
  (assert (deep= @[[:down [:link :lab] :boom]] results) "link observed")
  (proc/kill b :done))

(import ../examples/tasklet :as tasklet)

# 13. P1.3 pid face: portable [:pid node serial mbox] tuple, same
# shape as tasklet pids (tasklet.janet untouched).
(let [p (proc/spawn (fn [] (proc/recv)))]
  (def pid (proc/pid p))
  (assert (tasklet/pid? pid) "tuple passes tasklet/pid?")
  (assert (= (pid 2) (proc/serial p)) "serial matches slot 2")
  (assert (= 0 (pid 1)) "node 0 (same VM)")
  (proc/kill p :done))

# 14. proc/send resolves tuples (own or foreign); tasklets deliver
# to proc tuples through the carried channel (conventions documented:
# proc bodies take single values, tasklet loops take triples).
(let [got @[]
      p (proc/spawn (fn [] (repeat 2 (array/push got (proc/recv)))))]
  (proc/send (proc/pid p) :via-tuple)
  (tasklet/send (proc/pid p) :note "hello")
  (ev/sleep 0.3)
  (assert (deep= @[:via-tuple [:note "hello" nil]] got) "both land")
  (proc/kill p :done))

# 15. Procs reach tasklets via registry tuples with triple payloads.
(let [seen @[]
      t (tasklet/spawn (fn [tag payload] (array/push seen [tag payload])))]
  (tasklet/register t :tmp-svc)
  (proc/send (tasklet/whereis :tmp-svc) [:ping 1 nil])
  (ev/sleep 0.3)
  (assert (= 1 (length seen)) "tasklet handler ran")
  (assert (= :ping ((get seen 0) 0)) "tag dispatched")
  (tasklet/unregister :tmp-svc)
  (tasklet/quit t))

# 16. monitor/kill need objects (C lists/fibers live there): tuples
# give a clear error, not a crash.
(let [p (proc/spawn (fn [] (ev/sleep 30)))]
  (assert-error "kill rejects tuples" (proc/kill (proc/pid p) :x))
  (assert-error "monitor rejects tuples"
    (proc/monitor (proc/pid p) p :x))
  (proc/kill p :done))

# 17. P1.4 loop processes: call round-trip over triples, copy
# semantics, handler errors re-raise without killing the loop.
(let [db (procx/spawn-loop
           (do (var store @{})
               (fn [tag payload]
                 (case tag
                   :put (put store (payload 0) (payload 1))
                   :get (get store payload)
                   :bad (error "loop-boom")))))]
  (procx/call db :put [:u "amy"])
  (assert (= "amy" (procx/call db :get :u)) "loop call")
  (assert-error-value "loop error surfaces" "loop-boom"
    (procx/call db :bad nil))
  (assert (= "amy" (procx/call db :get :u)) "loop survives")
  (procx/quit db)
  (procx/quit db))

# 18. P1.4 trap: untrapped loops die with linked peers, trapping
# loops observe and survive.
(let [seen @[]
      a (procx/spawn-loop (fn [tag payload] :a))
      b (procx/spawn-loop
          (fn [tag payload]
            (array/push seen [tag payload]) :b))]
  (procx/link a b :lab)
  (proc/trap-exit b true)
  (def killer (proc/spawn (fn [] (error "zap"))))
  (procx/link killer a :k)
  (ev/sleep 0.5)
  (assert (not (proc/alive? a)) "untrapped dies with peer")
  (assert (proc/alive? b) "trapper survives")
  (assert (deep= @[[:down [:lab "zap"]]] seen) "trapper observes")
  (proc/kill killer :done)
  (procx/quit b))

# 19. P1.4 registry: names keep pid identity; call by name works.
(let [s (procx/spawn-loop (fn [tag payload] :ok))]
  (procx/register s :svc)
  (def found (procx/whereis :svc))
  (assert (tasklet/pid? found) "registry pid shape")
  (assert (= :ok (procx/call found :ping nil)) "call by name")
  (procx/unregister :svc)
  (assert (nil? (procx/whereis :svc)) "unregister")
  (procx/quit s))

# 20. Call timeout fires on a wedged loop.
(let [hog (procx/spawn-loop (fn [tag payload] (ev/sleep 30) :never))]
  (assert-error "call times out" (procx/call hog :ping nil 0.1))
  (proc/kill hog :done))

# 21. P1.4b: kills of handler-wedged loops are not trapped by the
# loop's own protect (armed flag re-raises instead of replying).
(let [hog (procx/spawn-loop (fn [tag payload] (ev/sleep 30) :never))]
  (assert-error "wedged call times out" (procx/call hog :ping nil 0.1))
  (ev/sleep 0.2)
  (proc/kill hog :killed)
  (ev/sleep 0.3)
  (assert (not (proc/alive? hog)) "wedged loop dies")
  (assert (proc/killed? hog) "armed flag sticks"))

# 22. P2.1 lanes: spawn-on runs remotely; replies route back through
# the forward (thread-chans pass by reference through both marshals).
(let [lane (procx/lane-spawn)
      rch (ev/thread-chan 1)
      rpid (procx/spawn-on lane
             (fn [] (def m (proc/recv)) (ev/give (m 1) (m 0))))]
  (assert (= (lane :id) (rpid 1)) "remote node id")
  (proc/send rpid [42 rch])
  (assert (= 42 (ev/take rch)) "lane echo round-trip")
  (procx/lane-stop lane))

# 23. P2.1 loop-on-lane: call works through the forward; dead remote
# sends are a silent no-op; two lanes place independently.
(let [l1 (procx/lane-spawn)
      l2 (procx/lane-spawn)
      db (procx/spawn-loop-on l1 (fn [tag payload] [:echoed payload]))]
  (assert (deep= [:echoed 99] (procx/call db :ping 99)) "lane call")
  (assert (not= (l1 :id) (l2 :id)) "distinct lanes")
  (def other (procx/spawn-on l2 (fn [] (ev/sleep 30))))
  (assert (= (l2 :id) (other 1)) "placed on lane 2")
  (proc/send [:pid 999 12345 (l1 :ctl)] :ghost)
  (ev/sleep 0.1)
  (procx/lane-stop l1)
  (procx/lane-stop l2))

(end-suite)
