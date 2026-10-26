(import ./native-executor-ops :as O)

# Pin the compiler shape as well as the result. The runtime association is
# structural, so a silent opcode change must fail this test, not pass boxed.
(def index-ops (map |($0 0) ((disasm O/indexed-loop) :bytecode)))
(def scratch-ops (map |($0 0) ((disasm O/scratch-loop) :bytecode)))
(def get-ops (map |($0 0) ((disasm O/get-loop) :bytecode)))
(def buffer-ops (map |($0 0) ((disasm O/buffer-loop) :bytecode)))
(assert (index-of 'geti index-ops) "geti was not compiled")
(assert (index-of 'get get-ops) "register-key get was not compiled")
(assert (index-of 'put buffer-ops) "buffer put was not compiled")
(assert (index-of 'get buffer-ops) "buffer get was not compiled")
(assert (index-of 'next index-ops) "next was not compiled")
(assert (index-of 'eq index-ops) "string equality was not compiled")
(assert (index-of 'mkscr scratch-ops) "scratch tuple was not compiled")

# Array and tuple reads, out-of-range nil, primitive string equality and
# collection iteration. Tables force a VM fallback at geti.
(assert (= 7 (O/indexed-loop @[[1 " "] [2 "x"] [3 " "]])))
(assert (= 7 (O/indexed-loop @[(tuple 1 " ") (tuple 2 "x") (tuple 3 " ")])))
(assert (= 7 (O/indexed-loop @[@{0 1 1 " "} @{0 2 1 :x} @{0 3 1 " "}])))

# A dynamic int key, including out-of-range -> nil; arrays and tuples
# execute the guarded read, tables require exact-PC VM fallback. A non-int
# key must return nil, not convert to zero or accidentally call a getter.
(def get-array @[@[1 2] [3 4] @[-5]])
(def get-tuple @[[1 2] (tuple 3 4) [-5]])
(def get-table @[@{0 1 1 2} @{0 3 1 4} @{0 -5}])
(assert (= 20 (O/get-loop get-array 12)))
(assert (= 20 (O/get-loop get-tuple 12)))
(assert (= 20 (O/get-loop get-table 12)))
(assert (= nil (get @[99] :non-index)))
(assert (= nil (get (tuple 99) :non-index)))

# Identical buffer and array loops exercise the new register-key read and
# existing-index write fast paths; 257..258 wrap to byte 1..2 on buffers.
(def bytes (buffer/new-filled 8 0))
(assert (= 36 (O/buffer-loop bytes 8)))
(assert (= 1 (get bytes 0)))
(assert (= 8 (get bytes 7)))
(assert (= nil (get bytes -1)))
(assert (= nil (get bytes :bad)))
(assert (= nil (get bytes 99)))
(assert (= 2084 (O/buffer-loop (array/new-filled 8 0) 8)))

# The rejected cases are deliberately executed after a native loop prefix:
# errors must originate in JOP_PUT, leave prior bytes alone, and growth must
# still be performed by the VM (including zero-filled intermediate bytes).
(def bad-index (try (O/buffer-guard-loop bytes -1 9) ([e] (string e))))
(def bad-type (try (O/buffer-guard-loop bytes 0 :bad) ([e] (string e))))
(assert (string/find "expected integer key for buffer" bad-index) bad-index)
(assert (string/find "can only put integers in buffers" bad-type) bad-type)
(assert (= 1 (get bytes 0)))
(O/buffer-guard-loop bytes 10 260)
(assert (= 11 (length bytes)))
(assert (= 0 (get bytes 8)))
(assert (= 4 (get bytes 10)))

# Force frequent GC checks in the alias-call path. The temporary tuple
# argument is a per-site scratch array; the struct result is GC allocated.
(gcsetinterval 1)
(def total (O/scratch-loop 300))
(assert (= total 135150))
(assert (= 150 (O/tuple-alias-loop 30)))
(print "ops-parity " total)

# The new alias/read boundaries also run from event-loop fibers. Native
# backedges and calls must keep the ordinary VM's suspend/resume discipline.
(ev/timeslice true)
(def results (ev/chan 2))
(ev/go (fn [] (ev/give results (O/scratch-loop 300))))
(ev/go (fn [] (ev/give results (O/indexed-loop @[[1 " "] [2 "x"] [3 " "]]))))
(def answers (sorted [(ev/take results) (ev/take results)]))
(assert (= (answers 0) 7))
(assert (= (answers 1) 135150))
(ev/timeslice false)

(when (os/getenv "NATIVE_OPS_STATS")
  (def stats ((get (curenv) 'executor-global-stats) :value))
  (def gs (stats))
  (assert (>= (gs 1) 2) "indexed and scratch loops did not attach")
  (assert (> (gs 4) 0) "no guarded native/VM boundaries ran")
  (eprint (string/format "native-ops-global %j" gs)))
