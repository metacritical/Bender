###
### Supervision trees (B7.2 Part A): OTP-style one_for_one supervision
### over first-class core/proc processes. Built on proc/spawn plus the
### proc-side monitor in examples/proc.janet (single-shot reapers,
### recorded exits for late monitors) — no new C primitives.
###
###   (sup/child :worker-a worker-a :permanent)
###   (sup/supervisor :root 3 5 [(sup/child :a worker-a) ...])
###   (sup/start-tree spec)   -> live supervisor handle
###   (sup/which-children h)  -> {name {:pid :restart :alive}}
###   (sup/stop-tree h)       -> ordered teardown
###
### Restart strategies (Erlang names, same meanings):
###   :permanent restarts on every exit
###   :transient restarts only on an abnormal exit (a non-nil reason)
###   :temporary never restarts (a supervised one-shot side effect)
### A process that returns normally records a nil exit reason, so
### "abnormal" is exactly "reason is not nil" — same rule as tasklets.
### Nested `supervisor` specs give a recursive tree; a supervisor
### child has no :restart key and therefore defaults to :permanent,
### as in OTP.
###
### Crashing children are restarted at most `intensity` times within
### `period` seconds, over a sliding window. Exceeding that bound is
### the safety property: the supervisor stops respawning, terminates
### its remaining children, and exits abnormally carrying the
### offending child and reason — a crash loop escalates instead of
### spinning forever, and never leaks orphaned children.
###
### PART B CONTRACT (AOT tree-shaking root set). The `entry` argument
### of `child` must be a top-level, statically-referenced function
### VALUE at the call site — a bare symbol naming a `defn`, never a
### computed expression, a reassigned local, or an inline `(fn ...)`.
### tools/callgraph.janet recovers declared children from exactly that
### bytecode shape and treats them as explicit reachability roots.
### Anything else still runs correctly at runtime, but is reported as
### `sup-child ... ? -1` and contributes no root: fail-open, never a
### false root.
###
### Tags in :down payloads are child names, rebound on each respawn,
### so a stale monitor from a pre-respawn generation cannot misroute;
### unknown tags are ignored. Teardown sets the stopping flag first,
### so a :permanent child exiting during teardown is never
### resurrected.
###

(import ./proc :as procx)

(defn child
  "Child spec: supervise `entry` (a zero-argument function returning
  a process) under `name` with restart `strategy` — :permanent
  (default), :transient, or :temporary. See the Part B contract in
  this module's header for what `entry` must be to be statically
  visible to the AOT reachability pass."
  [name entry &opt strategy]
  (default strategy :permanent)
  @{:type :worker :name name :entry entry :restart strategy})

(defn supervisor
  "Supervisor spec: a one_for_one supervisor named `name` that may
  restart at most `intensity` (default 3) children per `period`
  (default 5) seconds, over `children` (a vector of child and/or
  nested supervisor specs)."
  [name &opt intensity period children]
  (default intensity 3)
  (default period 5)
  (default children @[])
  @{:type :supervisor :name name :intensity intensity :period period
    :children children})

# Forward declaration: start-tree and the supervisor body are mutually
# recursive (a supervisor child is itself a supervisor). Bound below.
(var start-tree nil)

(defn- entry-proc
  "Call a child `entry` and return the process it produced. The entry
  contract is 'zero arguments, returns a process'."
  [entry name]
  (when (not (function? entry))
    (error (string "supervisor child " name
                   " entry must be a function, got " (type entry))))
  (entry))

(defn- start-child
  "Start one child spec and return its handle {:proc :name :tree}.
  A :supervisor spec recurses; a :worker spec calls its entry."
  [spec]
  (case (spec :type)
    :supervisor (start-tree spec)
    (do
      (def p (entry-proc (spec :entry) (spec :name)))
      @{:proc (if (and (table? p) (proc/pid? (get p :proc))) (get p :proc) p)
        :name (spec :name)
        :tree spec})))

(defn- teardown-child
  "Stop one child: offer the graceful quit every loop process in this
  codebase answers, then kill bodies that never read their mailbox
  (raw proc/spawn bodies cannot quit — same rule as proc/quit).

  Found by direct tracing, not assumed: a NESTED SUPERVISOR child
  needs more than a worker's quit budget, because its own graceful
  :proc-quit handler recursively calls THIS SAME function for each of
  ITS children before it can reply -- a fixed worker-sized timeout
  here races that whole recursive teardown. If it loses the race, the
  nested supervisor is abruptly proc/kill'd before it tears down its
  own children, which never receive any quit/kill signal and are
  orphaned (observed directly: a supervision tree one level deep,
  stopped via stop-tree, left an infinite-loop worker proc alive and
  the whole script hanging on process exit). Give a supervisor child
  a budget generous enough to absorb its own recursive teardown
  (leaf workers still quit almost instantly, so this only slows down
  the case that actually needs it) while staying safely under
  stop-tree's own top-level 5s race, so a genuinely stuck nested tree
  still cannot hang the caller forever."
  [handle]
  (def p (get handle :proc))
  (def nested-sup? (= :supervisor (get (get handle :tree) :type)))
  (def budget (if nested-sup? 3.0 0.2))
  (when (and (proc/pid? p) (proc/alive? p))
    (def rch (ev/thread-chan 1))
    (proc/send p [:proc-quit nil rch])
    (def tch (ev/chan 1))
    (ev/go (fn [] (ev/sleep budget) (ev/give tch :timeout)))
    (when (= tch ((ev/select rch tch) 1))
      (proc/kill p :shutdown)))
  handle)

(defn- should-restart?
  "Restart policy. A normal exit records a nil reason, so :transient
  restarts exactly on an abnormal (non-nil) reason, and anything else
  — including :temporary — never restarts."
  [strategy reason]
  (def abnormal (not (nil? reason)))
  (case strategy
    :permanent true
    :transient abnormal
    false))

(defn- sup-body
  "The supervisor's process body: owns the children map, the restart
  window, and the stopping flag. Receives [:down name reason] from its
  monitors and [tag nil reply-chan] control messages. Teardown always
  runs once the loop ends — including when a restart bound is
  exceeded — so a failing supervisor never leaks its children."
  [tree]
  (fn []
    (def me (proc/self))
    (def intensity (get tree :intensity 3))
    (def period (get tree :period 5))
    (def children @{})
    (def restarts @[])
    (def stopping @[false])

    (defn launch
      "Start child `spec`, record it, and monitor it under its name."
      [spec]
      (def handle (start-child spec))
      (def rec @{:name (spec :name)
                 :spec spec
                 :handle handle
                 :restart (get spec :restart :permanent)
                 :generation 0})
      (put children (spec :name) rec)
      (procx/monitor (get handle :proc) me (spec :name))
      rec)

    (defn snapshot
      "The which-children view: pids and policy, never handles —
      handles embed live procs, which cannot cross a reply channel."
      []
      (def out @{})
      (eachk n children
        (def rec (get children n))
        (when (not (nil? rec))
          (def p (get (get rec :handle) :proc))
          (put out n @{:pid (proc/pid p)
                       :restart (get rec :restart)
                       :alive (proc/alive? p)})))
      out)

    (defn note-restart
      "Record a restart stamp, forget stamps outside the window, and
      report whether the bound still holds."
      []
      (def now (os/clock))
      (array/push restarts now)
      (def cutoff (- now period))
      (def fresh (filter (fn [ts] (> ts cutoff)) restarts))
      (array/clear restarts)
      (each ts fresh (array/push restarts ts))
      (<= (length fresh) intensity))

    (defn shutdown
      "Ordered teardown, deepest first: flag first, so nothing is
      resurrected while children exit."
      []
      (set (stopping 0) true)
      (def names (keys children))
      (each n names
        (def rec (get children n))
        (when (not (nil? rec))
          (teardown-child (get rec :handle))
          (put children n nil))))

    (defn handle-down
      "A child exited: apply the strategy, honouring the bound."
      [name reason]
      (def rec (get children name))
      (when (and (not (nil? rec)) (not (stopping 0)))
        (if (not (should-restart? (get rec :restart) reason))
          (put children name nil)
          (do
            (when (not (note-restart))
              (error (string "supervisor intensity exceeded for " name
                             " (" intensity " per " period " s): " reason)))
            (def generation (+ 1 (get rec :generation)))
            (def next (launch (get rec :spec)))
            (put next :generation generation)))))

    # Start every declared child before serving messages, so a child
    # that cannot start is a startup error, not a silent dead slot.
    (each spec (get tree :children @[])
      (launch spec))

    (var running true)
    (var failure nil)
    (while running
      (def msg (proc/recv me))
      (def tag (msg 0))
      (cond
        (= tag :down)
        (try
          (handle-down (msg 1) (msg 2))
          ([e _] (set failure e) (set running false)))

        (= tag :which-children)
        (ev/give (msg 2) (snapshot))

        # :stop-tree and :proc-quit are the same ordered teardown, so a
        # nested supervisor stops recursively when its parent stops.
        (or (= tag :stop-tree) (= tag :proc-quit))
        (do
          (shutdown)
          (ev/give (msg 2) :stopped)
          (set running false))

        # Unknown or stale tags are ignored (same rule as tasklets).
        nil))

    (shutdown)
    (when (not (nil? failure))
      (error failure))))

(defn start-tree*
  "Start supervision tree `spec` (a `supervisor` spec) and return its
  live handle {:proc :name :tree}. Every declared child starts before
  the handle is returned; a child that fails to start aborts startup."
  [spec]
  (when (not= :supervisor (get spec :type))
    (error (string "start-tree needs a supervisor spec, got "
                   (get spec :type))))
  (def p (proc/spawn (sup-body spec)))
  @{:proc p
    :name (get spec :name)
    :tree spec})

(set start-tree start-tree*)

(defn which-children
  "Ask live supervisor handle `sup` for its {name {:pid :restart
  :alive}} table (pids, not handles)."
  [sup]
  (def p (get sup :proc))
  (when (not (proc/alive? p))
    (error "which-children on a dead supervisor"))
  (def rch (ev/thread-chan 1))
  (proc/send p [:which-children nil rch])
  (ev/take rch))

(defn stop-tree
  "Stop supervisor handle `sup`: flag, then each child (gracefully,
  with a kill for stragglers), then the supervisor itself. Nested
  supervisors stop recursively. Safe on an already-dead tree.
  Returns `sup`."
  [sup]
  (def p (get sup :proc))
  (when (and (proc/pid? p) (proc/alive? p))
    (def rch (ev/thread-chan 1))
    (proc/send p [:stop-tree nil rch])
    (def tch (ev/chan 1))
    (ev/go (fn [] (ev/sleep 5) (ev/give tch :timeout)))
    (when (= tch ((ev/select rch tch) 1))
      (proc/kill p :shutdown)))
  sup)
