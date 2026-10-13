# Conway's Game of Life with one Janet proc per cell.
#
# Every cell owns its state and waits for one [state neighbor-count]
# message per generation. The coordinator owns the board, sends each
# cell its local snapshot, and collects one result from every cell.
#
# Run with:
#   ./build/bender examples/game-of-life-procs.janet
#
# Optional environment variables:
#   GOL_ROWS=12 GOL_COLS=24 GOL_STEPS=12
#   GOL_DELAY=0.08     (seconds between rendered generations)
#   GOL_QUIET=1        (suppress frames; useful as a smoke test)

(defn env-int [name fallback]
  (def raw (os/getenv name ""))
  (if (= "" raw) fallback (scan-number raw)))

(def rows (env-int "GOL_ROWS" 10))
(def cols (env-int "GOL_COLS" 20))
(def steps (env-int "GOL_STEPS" 12))
# Set GOL_LANES>1 only when the current proc/lanes build is desired;
# the default keeps this standalone example deterministic on every build.
(def lanes (env-int "GOL_LANES" 1))
(def frame-delay (scan-number (os/getenv "GOL_DELAY" "0.08")))
(def quiet (= "1" (os/getenv "GOL_QUIET" "")))
(def cell-count (* rows cols))

(assert (and (> rows 0) (> cols 0) (> steps 0))
        "GOL_ROWS, GOL_COLS, and GOL_STEPS must be positive")

# Keep the demo's process-per-cell behavior visible: these are real
# proc pids, and their mailboxes are the only way the coordinator
# sends work to a cell.
(proc/lanes-start lanes)

(def ready (ev/thread-chan cell-count))
(def results (ev/thread-chan cell-count))
(def pids (array cell-count))

(defn cell-proc [idx]
  (ev/give ready [idx (proc/self-pid)])
  (loop [generation :range [0 steps]]
    (def [alive neighbors] (proc/recv))
    (def next-alive (if alive
                      (or (= neighbors 2) (= neighbors 3))
                      (= neighbors 3)))
    (ev/give results [idx next-alive])))

(loop [idx :range [0 cell-count]]
  (proc/spawn (fn [] (cell-proc idx))))

# Wait until every independent cell proc has announced its pid.
(loop [i :range [0 cell-count]]
  (def [idx pid] (ev/take ready))
  (put pids idx pid))

(defn cell-index [r c]
  (+ (* r cols) c))

(defn live-at [board r c]
  (and (>= r 0) (< r rows) (>= c 0) (< c cols)
       (get board (cell-index r c))))

(defn neighbor-count [board r c]
  (var count 0)
  (loop [dr :range [-1 2]]
    (loop [dc :range [-1 2]]
      (unless (and (= dr 0) (= dc 0))
        (when (live-at board (+ r dr) (+ c dc))
          (++ count)))))
  count)

(defn render [board generation]
  (unless quiet
    (print "generation " generation)
    (loop [r :range [0 rows]]
      (def line @[])
      (loop [c :range [0 cols]]
        (array/push line (if (get board (cell-index r c)) "#" ".")))
      (print (string/join line "")))
    (print)))

# A centered blinker gives a compact, recognizable oscillator.
(var board (array cell-count false))
(def mid-r (math/floor (/ rows 2)))
(def mid-c (math/floor (/ cols 2)))
(put board (cell-index mid-r (- mid-c 1)) true)
(put board (cell-index mid-r mid-c) true)
(put board (cell-index mid-r (+ mid-c 1)) true)

(loop [generation :range [0 steps]]
  (render board generation)
  (var next-board (array cell-count false))
  (loop [r :range [0 rows]]
    (loop [c :range [0 cols]]
      (def idx (cell-index r c))
      (proc/send (get pids idx)
                 [(get board idx) (neighbor-count board r c)])))
  (loop [i :range [0 cell-count]]
    (def [idx alive] (ev/take results))
    (put next-board idx alive))
  (set board next-board)
  (when (and (> frame-delay 0) (< generation (- steps 1)))
    (ev/sleep frame-delay)))

(proc/lanes-stop)
(print "game-of-life: " rows "x" cols " cells=" cell-count
       " generations=" steps " complete")
