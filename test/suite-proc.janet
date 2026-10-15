# Proc processes, P1.1: first-class process objects (same-lane raw
# processes). Tasklets stay untouched. Each file runs in its own
# process.

(import ./helper :prefix "" :exit true)
(start-suite)

# 1. Spawn returns a proc; self binds inside, nil outside.
((fn []
(assert (nil? (proc/self)) "self outside is nil")
))
((fn []
(let [seen @[]
      p (proc/spawn (fn [] (array/push seen (proc/self))))]
  (assert (proc/pid? p) "spawn returns a proc")
  (assert (not (proc/pid? :nope)) "pid? rejects non-procs")
  (ev/sleep 0.05)
  (assert (= 1 (length seen)) "body ran")
  (assert (= p (get seen 0)) "self is the proc")
  (assert (not (proc/alive? p)) "completed body is not alive"))
))

# 2. Send/recv round-trip in order (single-value messages).
((fn []
(let [got @[]
      p (proc/spawn (fn [] (repeat 3 (array/push got (proc/recv)))))]
  (ev/sleep 0.05)
  (proc/send p :a)
  (proc/send p :b)
  (proc/send p :c)
  (ev/sleep 0.1)
  (assert (deep= @[:a :b :c] got) "ordered delivery")
  (proc/kill p :done))
))

# 3. Isolation: payloads marshal-copy, never alias.
((fn []
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
))

# 4. Recv suspends on empty, wakes on send.
((fn []
(let [got @[]
      p (proc/spawn (fn [] (array/push got (proc/recv))))]
  (ev/sleep 0.05)
  (ev/go (fn [] (ev/sleep 0.05) (proc/send p :late)))
  (ev/sleep 0.2)
  (assert (deep= @[:late] got) "suspended recv wakes")
  (proc/kill p :done))
))

# 5. Kill flips alive; dead procs are a silent no-op.
((fn []
(let [p (proc/spawn (fn [] (ev/sleep 30)))]
  (ev/sleep 0.05)
  (assert (proc/alive? p) "sleeper alive")
  (proc/kill p :killed)
  (ev/sleep 0.1)
  (assert (not (proc/alive? p)) "killed proc dead")
  (proc/kill p :again))
))

# 6. Procs refuse marshal (same-VM only, like pool handles).
((fn []
(let [p (proc/spawn (fn [] :x))]
  (assert-error "proc refuses marshal" (marshal p))
  (proc/kill p :done))
))

# 7. Discipline: recv outside errors; stealing other mailboxes errors.
((fn []
(assert-error "recv outside a proc" (proc/recv))
))
((fn []
(let [a (proc/spawn (fn [] (ev/sleep 30)))
      b (proc/spawn (fn [] (proc/recv a)))]
  (ev/sleep 0.2)
  (assert (not (proc/alive? b)) "mailbox thief errors")
  (proc/kill a :done)
  (proc/kill b :done))
))

(import ../examples/proc :as procx)

# 8. P1.2 selective receive: out-of-order arrival, in-order match,
# non-matching stashed (C stash, Janet-scanned).
((fn []
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
))

# 9. Stash holds skipped messages across selects.
((fn []
(let [results @[]
      c (proc/spawn (fn []
                      (array/push results (procx/select (fn [m] (= :b m))))
                      (array/push results (procx/select (fn [m] (= :a m))))))]
  (proc/send c :a)
  (proc/send c :b)
  (ev/sleep 0.3)
  (assert (deep= @[:b :a] results) "stashed message found later")
  (proc/kill c :done))
))

# 10. P1.2 monitors: crash observed with reason, clean exit :normal.
((fn []
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
))

# 11. Late monitors deliver from the retained completion event.
# The process completes long before anyone watches (no reaper yet,
# so done? is false); monitoring afterwards still delivers :normal.
((fn []
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
))

# 12. Links are symmetric fate with [:link tag] marks.
((fn []
(let [results @[]
      a (proc/spawn (fn [] (ev/sleep 30)))
      b (proc/spawn (fn [] (array/push results (proc/recv))))]
  (procx/link a b :lab)
  (proc/kill a :boom)
  (ev/sleep 0.6)
  (assert (deep= @[[:down [:link :lab] :boom]] results) "link observed")
  (proc/kill b :done))
))

(import ../examples/tasklet :as tasklet)

# 13. P1.3 pid face: portable [:pid node serial mbox] tuple, same
# shape as tasklet pids (tasklet.janet untouched).
((fn []
(let [p (proc/spawn (fn [] (proc/recv)))]
  (def pid (proc/pid p))
  (assert (tasklet/pid? pid) "tuple passes tasklet/pid?")
  (assert (= (pid 2) (proc/serial p)) "serial matches slot 2")
  (assert (= 0 (pid 1)) "node 0 (same VM)")
  (proc/kill p :done))
))

# 14. proc/send resolves tuples (own or foreign); tasklets deliver
# to proc tuples through the carried channel (conventions documented:
# proc bodies take single values, tasklet loops take triples).
((fn []
(let [got @[]
      p (proc/spawn (fn [] (repeat 2 (array/push got (proc/recv)))))]
  (proc/send (proc/pid p) :via-tuple)
  (tasklet/send (proc/pid p) :note "hello")
  (ev/sleep 0.3)
  (assert (deep= @[:via-tuple [:note "hello" nil]] got) "both land")
  (proc/kill p :done))
))

# 15. Procs reach tasklets via registry tuples with triple payloads.
((fn []
(let [seen @[]
      t (tasklet/spawn (fn [tag payload] (array/push seen [tag payload])))]
  (tasklet/register t :tmp-svc)
  (proc/send (tasklet/whereis :tmp-svc) [:ping 1 nil])
  (ev/sleep 0.3)
  (assert (= 1 (length seen)) "tasklet handler ran")
  (assert (= :ping ((get seen 0) 0)) "tag dispatched")
  (tasklet/unregister :tmp-svc)
  (tasklet/quit t))
))

# 16. monitor/kill need objects (C lists/fibers live there): tuples
# give a clear error, not a crash.
((fn []
(let [p (proc/spawn (fn [] (ev/sleep 30)))]
  (assert-error "kill rejects tuples" (proc/kill (proc/pid p) :x))
  (assert-error "monitor rejects tuples"
    (proc/monitor (proc/pid p) p :x))
  (proc/kill p :done))
))

# 17. P1.4 loop processes: call round-trip over triples, copy
# semantics, handler errors re-raise without killing the loop.
((fn []
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
))

# 18. P1.4 trap: untrapped loops die with linked peers, trapping
# loops observe and survive.
((fn []
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
))

# 19. P1.4 registry: names keep pid identity; call by name works.
((fn []
(let [s (procx/spawn-loop (fn [tag payload] :ok))]
  (procx/register s :svc)
  (def found (procx/whereis :svc))
  (assert (tasklet/pid? found) "registry pid shape")
  (assert (= :ok (procx/call found :ping nil)) "call by name")
  (procx/unregister :svc)
  (assert (nil? (procx/whereis :svc)) "unregister")
  (procx/quit s))
))

# 20. Call timeout fires on a wedged loop.
((fn []
(let [hog (procx/spawn-loop (fn [tag payload] (ev/sleep 30) :never))]
  (assert-error "call times out" (procx/call hog :ping nil 0.1))
  (proc/kill hog :done))
))

# 21. P1.4b: kills of handler-wedged loops are not trapped by the
# loop's own protect (armed flag re-raises instead of replying).
((fn []
(let [hog (procx/spawn-loop (fn [tag payload] (ev/sleep 30) :never))]
  (assert-error "wedged call times out" (procx/call hog :ping nil 0.1))
  (ev/sleep 0.2)
  (proc/kill hog :killed)
  (ev/sleep 0.3)
  (assert (not (proc/alive? hog)) "wedged loop dies")
  (assert (proc/killed? hog) "armed flag sticks"))
))

# 22. P2.1 lanes: spawn-on runs remotely; replies route back through
# the forward (thread-chans pass by reference through both marshals).
((fn []
(let [lane (procx/lane-spawn)
      rch (ev/thread-chan 1)
      rpid (procx/spawn-on lane
             (fn [] (def m (proc/recv)) (ev/give (m 1) (m 0))))]
  (assert (= (lane :id) (rpid 1)) "remote node id")
  (proc/send rpid [42 rch])
  (assert (= 42 (ev/take rch)) "lane echo round-trip")
  (procx/lane-stop lane))
))

# 23. P2.1 loop-on-lane: call works through the forward; dead remote
# sends are a silent no-op; two lanes place independently.
((fn []
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
))

# 24. P2.2 placement pool: round-robin spreads 6 procs over 3 lanes.
((fn []
(let [pool (procx/pool-spawn 3)
      pids @[]]
  (repeat 6 (array/push pids (procx/pool-spawn-on pool (fn [] (ev/sleep 30)))))
  (assert (deep= @[0 1 2 0 1 2]
                 (map (fn [p] (- (p 1) ((get pids 0) 1))) pids))
          "round-robin placement")
  (procx/pool-stop pool))
))

# 25. P2.2 remote call through the pool (exact reply + timeout bound).
((fn []
(let [pool (procx/pool-spawn 2)
      db (procx/pool-spawn-loop-on pool (fn [tag payload] [:got payload]))
      hog (procx/pool-spawn-loop-on pool
            (fn [tag payload] (ev/sleep 30) :never))]
  (assert (deep= [:got :hey] (procx/call db :ping :hey)) "pool call")
  (assert-error "remote call times out"
    (procx/call hog :ping nil 0.1))
  (procx/pool-stop pool))
))

# Regression: pool teardown interleaves home callbacks with major GC.
# A completed fiber retains a channel nested in its public final result
# even after unrelated pool channels lose their registry claims.
((fn []
(let [pool (procx/pool-spawn 2)
      holder (fiber/new (fn [] @{:answer @[(ev/thread-chan 1)]}))]
  (resume holder)
  (procx/pool-stop pool)
  (gccollect)
  (gccollect)
  (let [ch (((fiber/last-value holder) :answer) 0)]
    (ev/give ch :kept)
    (assert (= :kept (ev/take ch)) "post-pool final result pins nested channel"))
  (assert (= :dead (fiber/status holder)) "final-result fiber completed"))
))

# A stopped lane's VM must never receive a home-teardown post from a
# later main-VM sweep. Retain foreign-homed channels across lane stop,
# then drop them while collecting; repeat to exercise VM address reuse.
((fn []
  (repeat 8
    (let [lane (procx/lane-spawn)
          reply (ev/thread-chan 1)]
      (procx/spawn-on lane (fn [] (ev/give reply (ev/thread-chan 1))))
      (let [foreign (ev/take reply)]
        (assert foreign "lane-homed channel delivered")
        (procx/lane-stop lane))
      (gccollect)
      (gccollect))))
 )

# 26. P2.3 cross-lane monitors: remote crash/clean observed on an
# owner channel; unknown serials answer :noproc.
((fn []
(let [lane (procx/lane-spawn)
      ch (ev/thread-chan 4)
      rcrash (procx/spawn-on lane
               (fn [] (ev/sleep 0.05) (error "remote-boom")))
      rclean (procx/spawn-on lane (fn [] :remote-bye))]
  (procx/monitor-chan rcrash :rc ch)
  (procx/monitor-chan rclean :rl ch)
  (def d1 (ev/take ch))
  (def d2 (ev/take ch))
  (assert (or (deep= [:down :rc "remote-boom"] d1)
              (deep= [:down :rc "remote-boom"] d2))
          "remote crash observed")
  (assert (or (deep= [:down :rl :normal] d1)
              (deep= [:down :rl :normal] d2))
          "remote clean observed")
  (procx/monitor-chan [:pid 999 424242 (lane :ctl)] :ghost ch)
  (assert (deep= [:down :ghost :noproc] (ev/take ch)) "unknown :noproc")
  (procx/lane-stop lane))
))

# 27. P2.3 local channel watchers (same mechanism, no lane).
((fn []
(let [lw (ev/chan 2)
      lp (proc/spawn (fn [] (ev/sleep 0.02) (error "local-boom")))]
  (procx/monitor lp lw :lw)
  (assert (deep= [:down :lw "local-boom"] (ev/take lw)) "channel watcher"))
))

# 28. P3.2 strict spawn: entry captures arrive by value (mutating the
# original afterwards is invisible inside).
((fn []
(let [cfg @{:mode :fast :tags @[1 2]}
      rch (ev/thread-chan 1)
      p (proc/spawn-isolated
          (fn [] (proc/recv) (ev/give rch cfg)))]
  (assert (proc/isolated? p) "strict flag set")
  (ev/sleep 0.05)
  (put cfg :mode :slow)
  (array/push (cfg :tags) 99)
  (proc/send p :go)
  (assert (deep= @{:mode :fast :tags @[1 2]} (ev/take rch))
          "entry copied at spawn")
  (proc/kill p :done))
))

# 29. P3.2 strict spawn: plain procs are not flagged; C-frame fiber
# captures refuse (fresh fibers copy — 4.3 value semantics).
((fn []
(let [q (proc/spawn (fn [] nil))]
  (assert (not (proc/isolated? q)) "plain not flagged")
  (proc/kill q :done))
))
((fn []
(let [blocked (ev/go (fn [] (ev/take (ev/chan 1)) :never))]
  (ev/sleep 0.05)
  (assert-error "cframe capture refuses"
    (proc/spawn-isolated (fn [] blocked)))
  (ev/cancel blocked :done))
))

# 30. P3.3 export rule, P3.5c attribution: lexically captured tables
# disconnect into the child heap at spawn (copy semantics), so strict
# stores into them are intra-heap (no panic) and never touch the
# original. Genuinely shared stores (module env, never captured)
# still die with the escape panic (reason observed).
((fn []
(each mk ["table" "array" "function"]
  (let [g @{}
        ch (ev/thread-chan 2)
        body (case mk
               "table" (fn [] (put g :leak @{:x 1}) :escaped)
               "array" (fn [] (put g :leak @[1 2]) :escaped)
               "function" (fn [] (put g :leak (fn [] :inner)) :escaped))
        p (proc/spawn-isolated body)]
    (procx/monitor-chan p (keyword (string "m-" mk)) ch)
    (ev/sleep 0.3)
    (def d (ev/take ch))
    (assert (= :down (d 0)) (string mk " down arrives"))
    (assert (= :normal (d 2)) (string mk " capture disconnects: clean exit"))
    (assert (nil? (get g :leak)) (string mk " original untouched"))))
))
((fn []
(let [ch (ev/thread-chan 2)
      p (proc/spawn-isolated
          (fn [] (put (table/getproto (curenv)) :smuggle @{:x 1}) :done))]
  (procx/monitor-chan p :m-escape ch)
  (ev/sleep 0.3)
  (def d (ev/take ch))
  (assert (= :down (d 0)) "shared-store down arrives")
  (assert (string/has-prefix? "strict proc value escaping" (d 2))
          "shared-store escape reason")
  (put (table/getproto (curenv)) :smuggle nil)
  (proc/kill p :done))
))
# Captured fibers: fresh ones copy at spawn (main-owned post-copy,
# so their export never panics — value semantics); C-frame fibers
# refuse at capture (test 29). Procs refuse at capture outright:
((fn []
(let [p0 (proc/spawn (fn [] (ev/sleep 30)))]
  (assert-error "proc capture refuses"
    (proc/spawn-isolated (fn [] p0)))
  (proc/kill p0 :done))
))

# 31. P3.3 non-exports pass: locals/keywords/strings/nesting in strict
# bodies never panic; imports from shared are allowed.
((fn []
(let [rch (ev/thread-chan 1)
      p (proc/spawn-isolated
          (fn []
            (def t @{:a 1 :k :kw :s "str" :t [:lit] :n @{:d @[1]}})
            (proc/recv)
            (ev/give rch :local-ok)))]
  (ev/sleep 0.05)
  (proc/send p :go)
  (assert (= :local-ok (ev/take rch)) "strict locals fine")
  (proc/kill p :done))
))
((fn []
(let [g @{:tired 0}
      p (proc/spawn-isolated
          (fn [] (def t @{}) (put t :g (get g :seed :dflt)) t))]
  (put g :seed :sval)
  (ev/sleep 0.2)
  (proc/kill p :done))
))

# 32. P3.3 plain procs never trip the rule (flag off, bits clear):
# the same export completes :normal.
((fn []
(let [g @{}
      ch (ev/thread-chan 2)
      p (proc/spawn (fn [] (put g :leak @{:x 1}) :escaped))]
  (procx/monitor-chan p :m ch)
  (ev/sleep 0.3)
  (assert (deep= [:down :m :normal] (ev/take ch)) "plain export :normal"))
))

# 33. P3.4 per-proc major: nursery counter resets; live values in
# the target survive its own collection.
((fn []
(let [rch (ev/thread-chan 4)
      p (proc/spawn-isolated
          (fn []
            (def box @[])
            (repeat 100 (array/push box (array/new 4)))
            (ev/give rch (debug/fiber-nursery))
            (proc/recv)
            (ev/give rch (length box))
            (ev/give rch (debug/fiber-nursery))))]
  (ev/sleep 0.1)
  (def charged (ev/take rch))
  (assert (> charged 0) "nursery charged")
  (debug/collect-proc p)
  (proc/send p :go)
  (assert (= 100 (ev/take rch)) "live survives own major")
  (assert (= 0 (ev/take rch)) "counter reset")
  (proc/kill p :done))
))

# 34. P3.4 firewall: another proc's values survive my collection
# (foreign young floats until a global collection takes them).
# Bodies self-report (captured arrays would copy at spawn).
((fn []
(let [rch (ev/thread-chan 4)
      mkbox (fn []
              (def box @[])
              (repeat 100 (array/push box (array/new 4)))
              (proc/recv)
              (ev/give rch (length box)))
      a (proc/spawn-isolated mkbox)
      b (proc/spawn-isolated mkbox)]
  (ev/sleep 0.1)
  (debug/collect-proc a)
  (gccollect)
  (proc/send a :go)
  (proc/send b :go)
  (assert (= 100 (ev/take rch)) "owner box intact")
  (assert (= 100 (ev/take rch)) "foreign box intact")
  (proc/kill a :done)
  (proc/kill b :done))
))

# 35. P3.4 fallbacks (never unsound, just global): plain procs, dead
# procs, and self-collect degrade to a full minor without crashing.
((fn []
(let [p (proc/spawn (fn [] (proc/recv)))]
  (ev/sleep 0.05)
  (debug/collect-proc p)
  (proc/kill p :done))
))
((fn []
(let [d (proc/spawn-isolated (fn [] :bye))]
  (ev/sleep 0.1)
  (debug/collect-proc d))
))
# Self-collect canary compares by content, not identity: a thread-chan
# marshals (deep copy), so identity holds only via cache residency — and
# the pre-collect original is legitimately collectible once unreferenced
# (ev-sleep lets sibling forms overlap, so a major may strike between
# compile and take and re-interning then mints a duplicate). Strings
# compare by content, and the canary is built fresh after collecting, so
# this still proves exactly what the pin claims: the body survived
# self-collect and communicated.
((fn []
(let [rch (ev/thread-chan 1)
      s (proc/spawn-isolated
          (fn [] (debug/collect-proc (proc/self))
            (ev/give rch (string "sur" "vived"))))]
  (ev/sleep 0.2)
  (assert (= "survived" (ev/take rch)) "self-collect falls back")
  (proc/kill s :done))
))

# 36. P3.4 sharing workload (registry rendezvous — strict bodies
# cannot lexically capture peers, closures would copy/disconnect):
# message exchange + shared reads collect clean (also green under
# JANET_PROC_VERIFY=1, run manually as gate evidence).
((fn []
(let [db (procx/spawn-loop (fn [tag payload] payload))]
  (procx/register db :p34db)
  (def a (proc/spawn-isolated
           (fn []
             (def box @[])
             (repeat 200 (array/push box (array/new 4)))
             (proc/send (procx/whereis :p34db) [:store [1 2 3] nil])
             (def got (procx/select (fn [m] (= :ping m))))
             (array/push box got)
             (array/push box (length box))
             (proc/recv))))
  (def b (proc/spawn-isolated
           (fn []
             (def box @[])
             (repeat 200 (array/push box (array/new 4)))
             (proc/recv))))
  (ev/sleep 0.1)
  (proc/send (procx/whereis :p34db) :ping)
  (proc/send a :go)
  (proc/send b :go)
  (ev/sleep 0.2)
  (debug/collect-proc a)
  (debug/collect-proc b)
  (proc/kill a :done)
  (proc/kill b :done)
  (procx/quit db)
  (procx/unregister :p34db))
))

# 37. P3.4 discipline: capturing a proc in a strict entry refuses
# (pids travel by registry/message, never by closure capture).
((fn []
(let [q (proc/spawn (fn [] (ev/sleep 30)))]
  (assert-error "proc capture refuses" (proc/spawn-isolated (fn [] q)))
  (proc/kill q :done))
))

# 38. P3.5 auto-trigger: a strict fiber's nursery pressure routes to
# per-proc majors by itself (peak/drop shape mirrors the 1.3 pin).
# (This hung pre-P3.4-raw-puts: marshal memo puts tripped the export
# rule mid-give.)
((fn []
(let [rch (ev/thread-chan 4)
      p (proc/spawn-isolated
          (fn []
            (var peak 0)
            (var drops 0)
            (repeat 40000
              (array/new 4)
              (def n (debug/fiber-nursery))
              (when (> n peak) (set peak n))
              (when (and (> peak 200000) (< n peak)) (++ drops)))
            (ev/give rch [peak drops])))]
  (ev/sleep 2)
  (def res (ev/take rch))
  (assert (> (res 0) 200000) "pressure passed threshold")
  (assert (> (res 1) 0) "auto per-proc majors reset pressure")
  (proc/kill p :done))
))

# 39. Strict loops: isolated entry + dispatch + call/quit all work;
# threaded reply channels unpack clean on the strict side.
((fn []
(let [db (procx/spawn-loop-isolated
           (do (var store @{})
               (fn [tag payload]
                 (case tag
                   :put (put store (payload 0) (payload 1))
                   :get (get store payload)))))]
  (assert (proc/isolated? db) "loop is strict")
  (procx/call db :put [:u "amy"])
  (assert (= "amy" (procx/call db :get :u)) "strict call")
  (procx/quit db))
))

# 40. Bulk fan-in regression: many single-shot senders to one collector
# proc, sized just above the historical danger zone. Each sender's
# completion runs a fiber-scoped minor GC; the collector's wakeups sit
# in the spawn task queue at that moment, so a minor that skipped the
# queue's roots could sweep a pending wakeup and strand the collector
# (loop parked in poll forever). Completion + exact count = no lost
# wakeup.
((fn []
(let [n 4000
      result (ev/thread-chan 16)
      collector
      (proc/spawn
        (fn []
          (var acc 0) (var got 0)
          (while (< got n)
            (set acc (+ acc (proc/recv)))
            (++ got))
          (ev/give result acc))
        1024)]
  (var i 0)
  (while (< i n)
    (proc/spawn-local (fn [] (proc/send collector 1)))
    (++ i))
  (assert (= n (ev/take result)) "fan-in: all 4000 wakeups delivered"))
))

(end-suite)
