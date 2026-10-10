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

(end-suite)
