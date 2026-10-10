###
### Proc helpers (Step P1.2): selective receive, monitors, links over
### the core/proc primitives. Tasklets stay untouched; this module is
### the proc-side companion (same disciplines: at-most-once, stash
### order, single-shot reapers, recorded exits for late monitors).
###
###   (proc/select pred)            # first matching message, stashing rest
###   (proc/monitor watched w tag)  # [:down tag reason] to w on completion
###   (proc/link a b &opt tag)      # symmetric fate
###
### Caveats (same as tasklets): nil messages are invisible to select
### (nil = miss); raw bodies that protect everything can trap kills.
###

(defn select
  "Selective receive on the calling process's mailbox: return the
  first message satisfying `pred`, stashing the rest in arrival order
  in the C stash (visible via proc/stash). Must be called inside a
  process."
  [pred]
  (def me (proc/self))
  (when (nil? me) (error "proc/select outside a process"))
  (def st (proc/stash me))
  # Stash scan (Janet-side: wake values bypass C, so matching lives
  # here — see P1.2 notes).
  (var found nil)
  (var idx nil)
  (eachp [i m] st
    (when (and (nil? idx) (pred m))
      (set idx i)
      (set found m)))
  # NOTE: (break val) discards val in this Janet — park in a cell.
  (if (not (nil? idx))
    (do (array/remove st idx)
      found)
    (do (while (nil? found)
          (def m (proc/recv))
          (if (pred m)
            (set found m)
            (array/push st m)))
      found)))

###
### Monitors over supervisor completions (same shape as tasklets: a
### single-shot reaper per watched proc pumps its sup channel once and
### exits; the C monitor list holds [tag watcher] pairs; done/reason
### are recorded for late monitors).
###

(var *reapers* @{})

(defn- ensure-reaper [watched]
  (when (nil? (get *reapers* watched))
    (put *reapers* watched true)
    (def sup (proc/sup-of watched))
    (ev/go
      (fn []
        (def [sig fib _] (ev/take sup))
        (def reason (if (= sig :ok) :normal (fiber/last-value fib)))
        (proc/record-exit watched reason)
        (def mons (proc/monitors-of watched))
        (var i 0)
        (while (< i (length mons))
          (def mtag (get mons i))
          (def nch (get mons (+ i 1)))
          (proc/send nch [:down mtag reason])
          (+= i 2))
        (array/clear mons)))))

(defn monitor
  "Watch `watched`: when it completes, watcher proc `w` receives
  [:down tag reason]. Already-completed processes deliver immediately
  from the recorded exit. Returns `w`."
  [watched w tag]
  (if (proc/done? watched)
    (proc/send w [:down tag (proc/exit-reason watched)])
    (do (proc/monitor watched w tag)
      (ensure-reaper watched)))
  w)

(defn link
  "Symmetric link between two procs: when either completes, the other
  observes [:down [:link tag] reason] (trap enforcement lands with
  loop processes in P1.4; raw bodies pattern-match it themselves).
  Returns [a b]."
  [a b &opt tag]
  (default tag :link)
  (monitor a b [:link tag])
  (monitor b a [:link tag])
  [a b])
