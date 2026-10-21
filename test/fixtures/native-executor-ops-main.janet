(import ./native-executor-ops :as O)

# Pin the compiler shape as well as the result. The runtime association is
# structural, so a silent opcode change must fail this test, not pass boxed.
(def index-ops (map |($0 0) ((disasm O/indexed-loop) :bytecode)))
(def scratch-ops (map |($0 0) ((disasm O/scratch-loop) :bytecode)))
(assert (index-of 'geti index-ops) "geti was not compiled")
(assert (index-of 'next index-ops) "next was not compiled")
(assert (index-of 'eq index-ops) "string equality was not compiled")
(assert (index-of 'mkscr scratch-ops) "scratch tuple was not compiled")

# Array and tuple reads, out-of-range nil, primitive string equality and
# collection iteration. Tables force a VM fallback at geti.
(assert (= 7 (O/indexed-loop @[[1 " "] [2 "x"] [3 " "]])))
(assert (= 7 (O/indexed-loop @[(tuple 1 " ") (tuple 2 "x") (tuple 3 " ")])))
(assert (= 7 (O/indexed-loop @[@{0 1 1 " "} @{0 2 1 :x} @{0 3 1 " "}])))

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
