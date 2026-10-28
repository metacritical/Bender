# The shape of concurrent Janet: a visual guide

Code hides the structure. This page draws it first, then shows the code that
produces that structure. The imagined syntax is deliberately not real Janet --
it is a diagram you can read at a glance.

Everything measured here is from `docs/internals/lanes-and-captured-state.md`
(§4b, §5a, §5c). Numbers are from a 12-core Apple Silicon machine.

---

## 1. The three planes

```
                    ┌─────────────────────────────────────────────┐
   OS THREADS       │  main VM        lane 1      lane 2   ...     │  12 threads
   (schedulers)     │  (thread 0)     (thread 1)  (thread 2)       │  = 1 + 11 lanes
                    └──────┬──────────────┬───────────┬────────────┘
                           │              │           │
   PROCS           ┌───────┴──────┐  ┌────┴─────┐  ┌──┴───────┐
   (~1.3 KB each)  │  proc 0      │  │ proc 1   │  │ proc 2   │       own heap,
                   │  own heap    │  │ own heap │  │ own heap │       own mailbox
                   │  own mailbox │  │          │  │          │
                   └───┬────┬─────┘  └────┬─────┘  └──┬───┬───┘
                       │    │             │           │   │
   FIBERS        ┌─────┴┐ ┌─┴────┐   ┌────┴─┐   ┌─────┴┐ ┌┴─────┐
   (~1 KB each)  │ fib  │ │ fib  │   │ fib  │   │ fib  │ │ fib  │    cooperative,
                 │      │ │      │   │      │   │      │ │      │    stay on their VM
                 └──────┘ └──────┘   └──────┘   └──────┘ └──────┘
```

Read it bottom-up: **fibers** are the cheap units you want millions of, but
they cannot leave their VM. **Procs** are how you reach other cores; each owns a
heap and a mailbox. **Lanes** are the threads those procs are spread over.

The rule that makes it click: *fibers give you count, procs give you cores.*

---

## 2. What the work looks like when it runs

```
   1 unit of work                     many units, one core              many units, all cores
   ─────────────────                  ────────────────────              ─────────────────────
   ┌──┐                               ┌──┐┌──┐┌──┐┌──┐                  ┌──┐┌──┐┌──┐   lane 1
   │██│  done                         │██││██││██││██│  one           │██││██││██│
   └──┘                               └──┘└──┘└──┘└──┘  after         └──┘└──┘└──┘
                                                          another     ┌──┐┌──┐┌──┐   lane 2
                                       (interleaved,     │██││██││██│
                                        one thread)      └──┘└──┘└──┘
                                                                      ┌──┐┌──┐┌──┐   lane 3
                                                                      │██││██││██│
                                                                      └──┘└──┘└──┘
```

| | cores used | 110M iterations took |
|---|---|---|
| fibers, one VM | 1 | **1046 ms** |
| procs, 11 lanes | 11 | **120 ms** |
| procs **containing** fibers | 11 | **121 ms** |

The third row is the point: you get the full core count *and* the unit count.

---

## 3. Imagined syntax: a diagram you can read

This is not valid Janet. It is what the code *means*, drawn.

```
╔════════════════════════════════════════════════════════════════════════╗
║  SPAWN 11 PROCS                                                        ║
║  ┌──────────────┐ ┌──────────────┐        ┌──────────────┐             ║
║  │  PROC A      │ │  PROC B      │  ...   │  PROC K      │             ║
║  │              │ │              │        │              │             ║
║  │ 1000 FIBERS  │ │ 1000 FIBERS  │        │ 1000 FIBERS  │             ║
║  │ ┌──┬──┬──┐   │ │ ┌──┬──┬──┐   │        │ ┌──┬──┬──┐   │             ║
║  │ │  │  │  │…  │ │ │  │  │  │…  │        │ │  │  │  │…  │             ║
║  │ └──┴──┴──┘   │ │ └──┴──┴──┘   │        │ └──┴──┴──┘   │             ║
║  └──────┬───────┘ └──────┬───────┘        └──────┬───────┘             ║
║         │                │                       │                     ║
║         └────────────────┴───────────────────────┘                     ║
║                          │                                             ║
║                          ▼                                             ║
║             ╔════════════════════════════╗                             ║
║             ║  RESULTS CHANNEL           ║  ← units flow back here     ║
║             ║  (thread channel: crosses  ║                             ║
║             ║   every lane boundary)     ║                             ║
║             ╚════════════╤═══════════════╝                             ║
║                          ▼                                             ║
║                    SUM / COLLECT                                       ║
╚════════════════════════════════════════════════════════════════════════╝
```

Now the same shape with names that carry meaning:

```
SPAWN  = proc/spawn          "start a process on some core"
HOST   = the body inside a proc
DO     = ev/go               "start a fiber, right here, cheap"
UNIT   = the actual work
INBOX  = ev/chan             "collect locally, inside one proc"
OUTBOX = ev/thread-chan      "collect across procs"
```

```
for each PROC:
    OUTBOX ← (later, this proc's contribution)
    INBOX  ← INBOX.new
    repeat UNIT_COUNT:
        DO → INBOX          (a fiber, cheap, stays here)
    await INBOX × UNIT_COUNT
    OUTBOX ← sum
await OUTBOX × PROC_COUNT
```

The nesting is the whole idea: **`DO` inside a `SPAWN`.** Two levels, one
hierarchy. Neither Go nor BEAM offers this — in Go the inner unit shares one
global heap; in BEAM the process *is* the cheap unit, so there is no cheaper
thing to put inside it.

---

## 3a. Arrow syntax: the pseudocode, made typeable

The prose arrows above (`←`, `→`, `×`) are readable but awkward to type. Here is
a compact ASCII form that maps 1:1 onto the same idea, using only characters a
keyboard has:

```
   |>        put it INTO            x |> ch      = ev/give ch x
   <|        take it OUT OF         v <| ch      = ev/take ch
   **        spread across          f ** xs      = run f over xs
   <>        across the boundary    <>ch         = a thread-chan (crosses lanes)
   --        same VM only           --ch         = a plain ev/chan (never crosses)
   @         await all              @n           = collect n results
```

So the guide's loop becomes:

```
PER PROC:
    out <>              ; OUTBOX: crosses lanes
    in  --              ; INBOX: local only, correct by construction
    xs ** f |> in       ; spread f over the chunk INTO the inbox
    @len(xs) <| in      ; await that many
    sum |> out          ; send the total across

@PROCS <| out           ; await one result per proc
```

Read the two arrow families as a promise you can check against the code:

```
    |>   in        safe   -- the value stays on this VM
    |>   <>        safe   -- thread-chan marshals across
    |>   --        ERROR  -- plain channel cannot cross (loud, by design)
```

That last line is the whole reason the distinction is worth learning: a plain
`ev/chan` written from another lane raises instead of silently dropping. The
arrow notation makes the mistake visible before you run it -- `--` is obviously
"this side only" once you have seen it twice.

Honest status: this is a READING aid, not shipped syntax. An attempt to build a
`proc/fan-out` helper behind it measured 331ms where the equivalent hand-written
code measured 44ms on the same work, and I did not find the cause, so it was
reverted rather than shipped. The notation above describes code you write
yourself today.

## 3b. Two levels, two VISIBILITY rules (the part that bites)

The shape from §3 is correct, but where a proc RUNS decides what it can see.
Both cases below were verified:

```
    closure captures MUTABLE state?  ──── yes ───►  placed on THIS VM
              │                                     (shares the capture)
             no                                     loses a core,
              │                                     sees your variables
              ▼
        crosses to a lane                     globals are PER-VM:
        (its own heap)                        the lane has its own
```

Measured, `JANET_LANES=4`:

| spawn | placement | what the main VM sees |
|---|---|---|
| closes over a local array | **this VM** (object, not tuple) | `shared=100` — mutation visible |
| captures nothing (reads a global) | **a lane** (pid tuple) | `counter=0` — global is per-VM |

That second row is the trap. A proc on another lane gets its **own copy of the
global table**, so `(++ counter)` inside it updates a counter nobody reads. This
looks like "the work never ran" when it actually ran fine 990 times.

So, for a fan-out that must reach a lane, results have to come back explicitly:

```janet
(def out (ev/thread-chan 4))      ;; crosses lanes
(defn body []
  (def ch (ev/chan 4096))         ;; local to that VM: correct, never crosses
  ...
  (ev/give out acc))              ;; explicit return
(proc/spawn body 8)               ;; takes a lane (no mutable capture)
(def got (ev/take out))           ;; => 100
```

If you would rather keep direct access to your variables, capture one and the
proc stays home: same code, `shared=100`, one core instead of twelve. The choice
is per-spawn and visible in the returned handle (tuple = lane, object = this VM).

## 4. The real code, same shape

```janet
(def results (ev/thread-chan 65536))          # OUTBOX — crosses lanes

(defn do-work [id]                            # UNIT
  (var acc 0)
  (var i 0)
  (while (< i 1000) (set acc (+ acc (+ id i))) (set i (+ i 1)))
  acc)

(defn host [base count]                       # HOST — one core's worth
  (def inbox (ev/chan 4096))                  # INBOX — local to this proc
  (var i 0)
  (while (< i count)
    (def id (+ base i))
    (ev/go (fn [] (ev/give inbox (do-work id))))   # DO → INBOX
    (set i (+ i 1)))
  (var got 0)
  (while (< got count)                        # gather this proc's work
    (ev/give results (ev/take inbox))
    (set got (+ got 1))))

# SPAWN: one proc per slice of the unit range
(var started 0)
(while (< started units)
  (def take (min per-proc (- units started)))
  (proc/spawn (fn [] (host started take)) 8)
  (set started (+ started take)))

# COLLECT: exactly `units` values come back
(var sum 0)
(var k 0)
(while (< k units) (set sum (+ sum (ev/take results))) (set k (+ k 1)))
```

Verified behaviour of this exact script:

```
units=1210 procs=11 fibers_each<=110 lanes=11 wall=4ms checksum=1335840000
```

---

## 5. Choosing the shape (a decision picture)

```
How many units?                          Do they need > 1 core?
     │                                            │
     ├─ a few, heavy ───────────────────────────► PROC per unit
     │
     ├─ very many, light ────► need cores? ──no──► FIBERS in one VM
     │                            │
     │                           yes
     │                            ▼
     └───────────────────────► PROCS containing FIBERS
                                    (the hybrid)
```

| shape | memory per unit | cores | use when |
|---|---|---|---|
| 1 proc | ~1.3 KB | 1 | a few heavy jobs |
| 1 fiber | ~1 KB | 1 | I/O-ish, massive count |
| proc × fibers | **~0.5 KB** at 5M units | all | **default for heavy fan-out** |

The hybrid wins on *both* axes at once: 5,000,000 concurrent units fit in
2.3 GB and still use every core.

---

## 6. Where the data goes (and what cannot)

```
        ┌──────────── own heap ─────────────┐
PROC A  │  array  table  buffer   ← mutable │
        │  captured on spawn: COPIED, not   │
        │  shared  (a lane marshals the     │
        │  closure)                         │
        └───────────────┬───────────────────┘
                        │
              ev/thread-chan  (values cross)
              proc/send       (messages cross)
                        │
        ┌───────────────┴───────────────────┐
PROC B  │  its own tenancy of the same data │
        └───────────────────────────────────┘

   ev/chan  ── does NOT cross.  Loud error if you try.
```

If a spawn captures mutable state, Janet places that proc **on the current
VM** rather than silently giving it a detached copy — you keep sharing, you
lose a core, and `JANET_PROC_SHARE_TRACE=1` tells you it happened.

---

## 7. One-line summary

```
fibers = the units you count        procs = the cores you use
        └──── put the first inside the second ────┘
```
