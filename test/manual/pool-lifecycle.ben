(import ../helper :prefix "" :exit true)
(import ../../examples/proc :as procx)
(let [pool (procx/pool-spawn 2)
      db (procx/pool-spawn-loop-on pool (fn [tag payload] [:got payload]))]
  (print (procx/call db :ping :hey))
  (procx/pool-stop pool))
(print "pool-only done")
