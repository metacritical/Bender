# Tasklets: bare fibers with owned isolated queues (Step 3.1-3.3).
# Each file runs in its own process.

(import ./helper :prefix "" :exit true)
(import ../examples/tasklet :as tasklet)
(start-suite)

# 1. Basic send/call over isolated mailboxes.
(let [db (tasklet/spawn
           (do (var store @{})
               (fn [tag payload]
                 (case tag
                   :put (put store (payload 0) (payload 1))
                   :get (get store payload)))))]
  (tasklet/send db :put [:user "amy"])
  (assert (= "amy" (tasklet/call db :get :user)) "call round-trip")
  (tasklet/quit db))

# 2. Isolation: mutations on either side never alias (marshal-copy).
(let [t (tasklet/spawn (fn [tag payload] payload))
      arr @[1 2 3]]
  (assert (deep= @[1 2 3] (tasklet/call t :echo arr)) "echo copies")
  (array/push arr 99)
  (assert (= 3 (length (tasklet/call t :echo @[1 2 3]))) "no aliasing")
  (tasklet/quit t))

# 3. Selective receive: out-of-order arrival, in-order matching,
# non-matching stashed (triples: [tag payload reply]).
(let [results @[]
      c (tasklet/spawn-raw
          (fn []
            (def got @[])
            (array/push got (tasklet/recv (fn [m] (= :a (m 0)))))
            (array/push got (tasklet/recv (fn [m] (= :b (m 0)))))
            (array/push got (tasklet/recv (fn [m] (= :c (m 0)))))
            (each m got (array/push results m))))]
  (tasklet/send c :b 2)
  (tasklet/send c :c 3)
  (tasklet/send c :a 1)
  (ev/sleep 0.3)
  (assert (deep= @[[:a 1 nil] [:b 2 nil] [:c 3 nil]] results)
          "selective order"))

# 4. Receive timeout fires; self outside is nil.
(assert (nil? (tasklet/self)) "self outside is nil")
(let [t (tasklet/spawn-raw (fn [] (tasklet/recv (fn [m] false) 0.05)))]
  (ev/sleep 0.2))
(assert-error "recv outside tasklet" (tasklet/recv (fn [m] true)))

# 5. Monitors: abnormal exit observed with reason, clean exit :normal.
(let [results @[]
      crasher (tasklet/spawn-raw (fn [] (ev/sleep 0.05) (error "bang")))
      cleaner (tasklet/spawn-raw (fn [] :bye))
      watcher (tasklet/spawn
                (fn [tag payload] (array/push results [tag payload])))]
  (tasklet/monitor crasher watcher :w)
  (tasklet/monitor cleaner watcher :c)
  (ev/sleep 0.4)
  (def downs (sorted
               (filter (fn [m] (and (tuple? m) (= :down (m 0)))) results)))
  (assert (= 2 (length downs)) "both downs")
  (assert (deep= [:down [:c :normal]] (downs 0)) "clean :normal")
  (assert (deep= [:down [:w "bang"]] (downs 1)) "crash reason")
  (tasklet/quit watcher))

# 6. Unknown pids deliver :noproc immediately.
(let [watcher (tasklet/spawn (fn [tag payload] payload))
      fake [:pid 0 999999 (ev/ichan 1)]]
  (assert (tasklet/pid? fake) "pid shape")
  (tasklet/monitor fake watcher :t)
  (ev/sleep 0.1)
  (tasklet/quit watcher))

# 7. Links + trap-exit: peer death propagates (untrapped dies),
# trapping observes and survives.
(let [killer (tasklet/spawn-raw (fn [] (error "boom")))
      a (tasklet/spawn (fn [tag payload] :a))]
  (tasklet/link killer a :k)
  (ev/sleep 0.3)
  # a died from propagation: quitting returns at once (done recorded).
  (tasklet/quit a))
(let [seen @[]
      c (tasklet/spawn
          (fn [tag payload] (array/push seen [tag payload]) :c))]
  (tasklet/trap-exit c true)
  (def killer2 (tasklet/spawn-raw (fn [] (error "zap"))))
  (tasklet/link killer2 c :k2)
  (ev/sleep 0.3)
  (assert (deep= @[[:down [:k2 "zap"]]] seen) "trapper observes")
  (tasklet/quit c))

# 8. Registry: same-VM names keep pid identity (no marshal copies).
(let [t (tasklet/spawn (fn [tag payload] :ok))]
  (tasklet/register t :svc)
  (def found (tasklet/whereis :svc))
  (assert (tasklet/pid? found) "registry pid")
  (assert (= :ok (tasklet/call found :ping nil)) "call by name")
  (tasklet/unregister :svc)
  (assert (nil? (tasklet/whereis :svc)) "unregister")
  (tasklet/quit t))

# 9. Task errors re-raise in callers; the loop survives.
(let [t (tasklet/spawn (fn [tag payload]
                         (if (= tag :bad) (error "task-boom") :fine)))]
  (assert-error-value "task error surfaces" "task-boom"
    (tasklet/call t :bad nil))
  (assert (= :fine (tasklet/call t :good nil)) "loop survives")
  (tasklet/quit t))

# 10. Step 4.1 home-lane pin: isolated mailboxes cross threads by
# copy (marshal) — the worker's copy is owned by the worker (usable
# locally, queued items cross with it) and disconnected from main's
# (no aliasing, no leakage back). Same-pointer foreign use refuses
# loudly in C (janet_channel_make_isolated contract); from Janet
# every crossing copies, so this is the observable face of the pin.
(let [c (ev/ichan 4)
      done (ev/thread-chan 1)]
  (ev/give c :main-msg)
  (thread/new (fn [chans]
                (def [mine wdone] chans)
                (assert (= :main-msg (ev/take mine)) "queued item crosses")
                (ev/give mine :worker-msg)
                (assert (= :worker-msg (ev/take mine)) "worker copy local")
                (ev/give wdone :worker-ok))
              [c done])
  (assert (= :worker-ok (ev/take done)) "worker ran")
  (assert (= :main-msg (ev/take c)) "main copy intact")
  (assert (nil? (ev/try-take c)) "no worker leakage into main"))

# 11. Step 5.1 kill: idle task dies, monitor observes the reason.
(let [results @[]
      t (tasklet/spawn (fn [tag payload] :ok))
      w (tasklet/spawn (fn [tag payload] (array/push results [tag payload])))]
  (tasklet/monitor t w :m)
  (tasklet/kill t :killed)
  (ev/sleep 0.2)
  (assert (deep= @[[:down [:m :killed]]] results) "kill observed")
  (assert (= :error (fiber/status (get t :fiber))) "victim errors")
  (tasklet/quit w))

# 12. Kill is untrappable at loop level: trap-exit does not save the
# victim (ERROR lands at the dispatch take, outside every protect).
(let [results @[]
      t (tasklet/spawn (fn [tag payload] :ok))
      w (tasklet/spawn (fn [tag payload] (array/push results [tag payload])))]
  (tasklet/trap-exit t true)
  (tasklet/monitor t w :m)
  (tasklet/kill t :killed)
  (ev/sleep 0.2)
  (assert (deep= @[[:down [:m :killed]]] results) "trapper still dies")
  (tasklet/quit w))

# 13. Kill lands on wedged hogs at the quantum boundary (<=200 polls,
# far inside the 0.3s sleep): status flips and monitors observe.
(let [results @[]
      hog (tasklet/spawn-raw
            (fn [] (var acc 0) (while true (+= acc 1))))
      w (tasklet/spawn (fn [tag payload] (array/push results [tag payload])))]
  (tasklet/monitor hog w :h)
  (ev/sleep 0.05)
  (tasklet/kill hog :killed)
  (ev/sleep 0.3)
  (assert (= :error (fiber/status (get hog :fiber))) "hog dies")
  (assert (deep= @[[:down [:h :killed]]] results) "hog kill observed")
  (tasklet/quit w))

# 14. Kill of unknown/completed pids is a silent no-op.
(let [t (tasklet/spawn (fn [tag payload] :ok))]
  (tasklet/quit t)
  (tasklet/kill t :whatever)
  (tasklet/kill [:pid 0 999999 (ev/ichan 1)] :whatever))

# 15. Step 5.2 one_for_one: transient crash restarts the child
# (same id, new pid), clean transient exits do not restart. The flaky
# child crashes its first two incarnations, then stays up (a tight
# crash loop would trip intensity before the assertions run — that is
# test 16's point).
(let [starts @[]]
  (var crashes-left 2)
  (def sup
    (tasklet/supervisor-start
      [{:id :flaky
        :start (fn []
                 (array/push starts :started)
                 (if (> crashes-left 0)
                   (do (-- crashes-left)
                     (tasklet/spawn-raw
                       (fn [] (ev/sleep 0.02) (error "flaky-boom"))))
                   (tasklet/spawn (fn [tag payload] :steady))))
        :restart :transient}
       {:id :clean
        :start (fn [] (tasklet/spawn-raw (fn [] :bye)))
        :restart :transient}]))
  (ev/sleep 0.4)
  (def kids (tasklet/supervisor-which-children sup))
  (assert (tasklet/pid? (kids :flaky)) "flaky re-pidded")
  (assert (nil? (kids :clean)) "clean transient not restarted")
  (assert (= 3 (length starts)) "two crashes + steady incarnation")
  (assert (= :steady (tasklet/call (kids :flaky) :ping nil))
          "restarted child serves")
  (tasklet/supervisor-stop sup))

# 16. Intensity bounds crash loops: 3 restarts per 5s, then the
# supervisor itself exits (observed dead, starts frozen).
(let [starts @[]
      sup (tasklet/supervisor-start
            [{:id :doomed
              :start (fn []
                       (array/push starts :started)
                       (tasklet/spawn-raw
                         (fn [] (ev/sleep 0.02) (error "boom"))))
              :restart :transient}])]
  (ev/sleep 0.6)
  (assert (= 4 (length starts)) "intensity: 1 + 3 restarts")
  (assert (= :error (fiber/status (get sup :fiber))) "sup gives up"))

# 17. Permanent steady child answers through its supervised pid;
# stop tears everything down (no resurrection, clean exit).
(let [sup (tasklet/supervisor-start
            [{:id :steady
              :start (fn [] (tasklet/spawn (fn [tag payload] :steady-ok)))}])]
  (ev/sleep 0.1)
  (def kids (tasklet/supervisor-which-children sup))
  (assert (tasklet/pid? (kids :steady)) "steady has pid")
  (assert (= :steady-ok (tasklet/call (kids :steady) :ping nil))
          "call through supervised pid")
  (tasklet/supervisor-stop sup))

(end-suite)
