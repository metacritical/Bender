# Native-to-native continuation fixtures. Imported as a module (exactly like
# ttfx imports src/scene.janet) so intra-module references compile to
# constants and the definitions capture no environment -- the shape the
# executor tier requires.
#
# Every function here is loop-eligible (it has a back edge and only allowed
# opcodes), so each gets its own loop executor, and every call below is
# non-terminal: a terminal call is always owned by the VM.
#
# IMPORTANT, and the reason for the odd-looking `&opt` tails and the varied
# loop bounds: executor attachment is STRUCTURAL. Two definitions with the
# same flags/arity/slotcount/constant-shape/bytecode words are indistinguishable
# to janet_native_try_attach, which refuses an ambiguous match outright. A
# naive set of `(while (< i n) (+= s (callee i)) (++ i))` wrappers is
# byte-identical (the callee is an opaque constant), so NONE of them would
# attach and the fixture would silently prove nothing. Each definition below
# is therefore given a distinct shape. The `&opt` parameters do double duty:
# they make the arity signature unique AND they are the case the design
# singles out -- a non-vararg def with min_arity < max_arity, called with the
# optional arguments omitted, so janet_fiber_funcframe nil-fills them exactly
# as JOP_CALL would.

(def effects @"")

# --- 3-deep chain: chain1 -> chain2 -> chain3 ------------------------------

(defn chain3 [x]
  (var s 0)
  (var i 0)
  (while (< i 3)
    (+= s (* x 2))
    (++ i))
  s)

(defn chain2 [x &opt k]
  (var s 0)
  (var i 0)
  (while (< i 4)
    (+= s (chain3 x))
    (++ i))
  s)

(defn chain1 [n]
  (var s 0)
  (var i 0)
  (while (< i n)
    (+= s (chain2 i))
    (++ i))
  s)

# --- R1: the callee must run EXACTLY once per edge -------------------------
# A half-committed fast path followed by an ordinary JOP_CALL would append
# twice; a lost frame step would append zero times.

(defn side-callee [x &opt k]
  (var i 0)
  (while (< i 1)
    (buffer/push-string effects "x")
    (++ i))
  x)

(defn side-caller [n &opt k1 k2]
  (var s 0)
  (var i 0)
  (while (< i n)
    (+= s (side-callee i))
    (++ i))
  s)

# --- R3: a callee that exits to the VM in the MIDDLE of its own body -------
# `/` is a hard VM boundary for this tier, so r3-callee's executor stops
# inside r3-callee. If the interpreter's `func` register still named the
# CALLER at that point, the callee's bytecode would be executed against the
# caller's constant pool. The distinctive constants make that a wrong number
# rather than a crash.

(defn r3-callee [x &opt a b]
  (var s 0)
  (var i 0)
  (while (< i 2)
    (+= s (/ 1000000.5 (+ x 1)))
    (++ i))
  (+ s 7.25))

(defn r3-caller [n &opt a b c]
  (var s 0)
  (var i 0)
  (while (< i n)
    (+= s (r3-callee i))
    (++ i))
  s)

# --- R8: a panic three native frames deep ----------------------------------

(defn panic3 [a &opt q]
  (var s 0)
  (var i 0)
  (while (< i 5)
    (+= s 1)
    (++ i))
  (+ s (in a 999)))

(defn panic2 [a &opt q r]
  (var s 0)
  (var i 0)
  (while (< i 2)
    (+= s (panic3 a))
    (++ i))
  s)

(defn panic1 [a &opt q r t]
  (var s 0)
  (var i 0)
  (while (< i 3)
    (+= s (panic2 a))
    (++ i))
  s)

# --- Deep recursion: longer than the trampoline's chain budget -------------
# The budget bounds chained FRAMES, never the computation: the trampoline
# returns at an unexecuted PC in a real frame and the VM carries on, so the
# answer must be identical either way.

(defn deep [n &opt d]
  (var s 0)
  (var i 0)
  (while (< i 1)
    (+= s (if (< n 1) 0 (deep (- n 1))))
    (++ i))
  (+ s 1))

# --- An allocating callee, for the GC safepoint on every push and pop ------

(defn alloc-callee [x &opt a b c]
  (var i 0)
  (var q @[])
  (while (< i 4)
    (array/push q i)
    (++ i))
  (+ x (length q)))

(defn alloc-caller [n &opt a b c d]
  (var s 0)
  (var i 0)
  (while (< i n)
    (+= s (alloc-callee i))
    (++ i))
  s)

(defn reset-effects []
  (buffer/clear effects)
  (length effects))
