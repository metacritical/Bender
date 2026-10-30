(import ../helper :prefix "" :exit true)
(import ../../examples/proc :as procx)
(let [pool (procx/pool-spawn 3) pids @[]]
 (repeat 6 (array/push pids (procx/pool-spawn-on pool (fn [] (ev/sleep 30)))))
 (assert (deep= @[0 1 2 0 1 2] (map (fn [p] (- (p 1) ((get pids 0) 1))) pids)) "round robin")
 (procx/pool-stop pool))
(print "first pool stopped")
(let [pool (procx/pool-spawn 2)
      db (procx/pool-spawn-loop-on pool (fn [tag payload] [:got payload]))]
  (print (procx/call db :ping :hey))
  (procx/pool-stop pool))
(print "sequence done")
