# tools/callgraph.janet — Phase 4 step 1 (registration).
# Extract the whole-program FuncDef graph from Janet source WITHOUT running
# it: parse -> compile (macros run at compile time, as today) -> disasm ->
# walk. Prints node/edge lines plus a stats summary. Read-only; no src/
# changes. Usage: ./build/janet tools/callgraph.janet FILE [FILE...]
#
# Edge kinds:
#   def <id> <name> arity <a> slots <s> instrs <n>   every discovered FuncDef
#   contains <parent> <child>                  JOP_CLOSURE nesting
#   calls <caller> <callee>                     direct call, unique target
#   extern <caller> <name>                      call to unknown/global name
#   ambiguous <caller> <name>                   >1 same-named target
#   unknown <caller> <slot>                     callee slot has no symbol
#   uncompiled <file> <-1> <error>             whole unit failed (no defs)
#   dynamic <caller> <name>                     may defeat inference
#                                              (eval/compile/require/...)
#   stats <defs> <callsites> <resolved> <extern> <unresolved>
#
# Resolution is deliberately shallow (this slice): a call-site callee slot
# resolves through the enclosing def's symbolmap range entry, or by
# scanning back to the nearest writer (ldc of a function/cfunction constant,
# clo of a nested def, lds self-reference, ldu by upvalue index).
# No dataflow fixpoint yet — that is the next Phase 4 slice. Anything
# fancier prints as extern/ambiguous/unknown.
#
# Single-unit semantics (verified against upstream, not a tool shortcut):
# the file compiles as one (do ...) exactly like the real loader treats
# sequential forms for references — a forward reference to a name defined
# later fails the whole unit (same "unknown symbol" the loader reports),
# recorded as one honest `uncompiled` line. Top-level defns the unit never
# references are dropped by the compiler's dead-store elimination, so the
# graph is precisely the statically-live code; anything reachable only
# dynamically (eval) needs the interpreter fallback flagged by `dynamic`.

(def args (tuple/slice (dyn :args) 1))
(when (empty? args)
  (eprint "usage: callgraph.janet FILE [FILE...]")
  (os/exit 4))

# Inference-defeating globals (Phase 4 refusals): dynamic code loading and
# friends. A call resolving to one of these names is flagged, never an edge.
(def dynamic-names
  @{"eval" true "compile" true "dobytes" true "dostring" true
    "require" true "use" true "import" true "ffi" true})

(var next-id 0)
(var defs @[])        # id -> {:d disasm-struct :name string}
(var name-ids @{})    # name -> array of ids
(var edges @[])       # tuples in discovery order
(var callsites 0)
(var resolved 0)

(defn register-def [d]
  (def id next-id)
  (++ next-id)
  (def name (string (or (d :name) (string "<anon-" id ">"))))
  (array/push defs {:d d :name name})
  (def bucket (get name-ids name))
  (if bucket
    (array/push bucket id)
    (put name-ids name @[id]))
  # children pre-order, right after the parent: ids read top-down and
  # each subtree occupies one contiguous range
  (each child (or (d :defs) @[])
    (register-def child))
  id)

(defn slot-name [d pc slot]
  # symbolmap range entries are [start end slot name]; pc is the
  # instruction index. (Upvalue entries [:upvalue _ slot name] have no
  # pc range and lose under slot reuse, so the backward scan resolves
  # ldu writes by upvalue index instead — never here.)
  (var found nil)
  (each e (or (d :symbolmap) @[])
    (when (and (= 4 (length e)) (number? (e 0))
               (= (e 2) slot) (<= (e 0) pc) (<= pc (e 1)))
      (set found (string (e 3)))
      (break)))
  found)

(defn upvalue-name [d upidx]
  # upvalue entries are [:upvalue _ upidx name]; ldu carries the upidx
  # in its third operand
  (var found nil)
  (each e (or (d :symbolmap) @[])
    (when (and (= 4 (length e)) (= :upvalue (e 0)) (= (e 2) upidx))
      (set found (string (e 3)))
      (break)))
  found)

# Ops without a destination slot (everything else writes (ins 1)).
(def no-dest-ops
  @{'push true 'push2 true 'push3 true 'pusha true
    'jmp true 'jmpif true 'jmpni true 'jmpnn true 'jmpno true
    'ret true 'retn true 'err true 'tcall true 'res true})
# Ops that end straight-line flow for the backward scan.
(def flow-stop-ops
  @{'jmp true 'jmpif true 'jmpni true 'jmpnn true 'jmpno true
    'ret true 'retn true 'err true 'tcall true 'res true})

(defn resolve-name [caller-id tname]
  # shared tail: dynamic > unique image def > ambiguous > extern
  (cond
    (get dynamic-names tname) (array/push edges ["dynamic" caller-id tname])
    (do (def ids (get name-ids tname))
      (cond
        (nil? ids) (array/push edges ["extern" caller-id tname])
        (= 1 (length ids)) (do (array/push edges ["calls" caller-id (ids 0)])
                              (++ resolved))
        (array/push edges ["ambiguous" caller-id tname])))))

(defn const-target [d cidx core-names]
  # classify a constant for call resolution. Returns a tuple
  # [kind payload]: [:fn name] [:core name] [:key name] [:no].
  (def consts (or (d :constants) @[]))
  (if (>= cidx (length consts))
    [:no nil]
    (do (def c (consts cidx))
      (def t (type c))
      (cond
        (= t :function) [:fn (string (or ((disasm c) :name) "?"))]
        (= t :cfunction) (do (def n (get core-names c))
                           (if n [:core n] [:no nil]))
        (= t :keyword) [:key (string c)]
        (= t :symbol) [:key (string c)]
        [:no nil]))))

(defn subtree-size [id]
  # ids are pre-order contiguous: 1 + sum of children's sizes
  (var n 1)
  (var cid (+ id 1))
  (each child (or (((defs id) :d) :defs) @[])
    (def s (subtree-size cid))
    (+= n s)
    (+= cid s))
  n)

(defn child-at [caller-id defidx]
  # id of the defidx-th direct child (pre-order contiguous layout)
  (def kids (or (((defs caller-id) :d) :defs) @[]))
  (if (>= defidx (length kids))
    nil
    (do (var cid (+ caller-id 1))
      (var k 0)
      (while (< k defidx)
        (+= cid (subtree-size cid))
        (++ k))
      cid)))

(defn record-call [caller-id d pc slot core-names]
  (++ callsites)
  # 1. named local via symbolmap range entry
  (def sname (slot-name d pc slot))
  (if sname
    (resolve-name caller-id sname)
    # 2. scan back to the nearest writer of the slot
    (do (def code (or (d :bytecode) @[]))
      (var j (- pc 1))
      (var scanned 0)
      (var done false)
      (while (and (>= j 0) (< scanned 40) (not done))
        (++ scanned)
        (def ins (code j))
        (def op (ins 0))
        (if (get flow-stop-ops op)
          (do (array/push edges ["unknown" caller-id slot])
            (set done true))
          (when (and (>= (length ins) 2) (= (ins 1) slot)
                     (not (get no-dest-ops op)))
            # nearest writer found
            (cond
              (= op 'ldc)
              (do (def ct (const-target d (ins 2) core-names))
                (cond
                  (= (ct 0) :fn) (resolve-name caller-id (ct 1))
                  (= (ct 0) :core) (array/push edges ["extern" caller-id (ct 1)])
                  (= (ct 0) :key) (array/push edges ["extern" caller-id (ct 1)])
                  (array/push edges ["unknown" caller-id slot])))
              (= op 'clo)
              (do (def cid (child-at caller-id (ins 2)))
                (if cid
                  (do (array/push edges ["calls" caller-id cid]) (++ resolved))
                  (array/push edges ["unknown" caller-id slot])))
              (= op 'lds)
              (do (array/push edges ["calls" caller-id caller-id]) (++ resolved))
              (= op 'ldu)
              (do (def uname (if (>= (length ins) 4) (upvalue-name d (ins 3)) nil))
                (if uname
                  (resolve-name caller-id uname)
                  (array/push edges ["unknown" caller-id slot])))
              (array/push edges ["unknown" caller-id slot]))
            (set done true)))
        (-- j))
      (when (not done)
        (array/push edges ["unknown" caller-id slot])))))

(defn walk-def-at [id core-names]
  # contains edges to direct children (found via contiguous ranges),
  # then this def's own call sites
  (def info (defs id))
  (def d (info :d))
  (var cid (+ id 1))
  (each child (or (d :defs) @[])
    (array/push edges ["contains" id cid])
    (+= cid (subtree-size cid)))
  (def code (or (d :bytecode) @[]))
  (var pc 0)
  (each ins code
    (def op (ins 0))
    (cond
      (= op 'call) (record-call id d pc (ins 2) core-names)
      (= op 'tcall) (record-call id d pc (ins 1) core-names)
      nil)
    (++ pc)))

# Pass 1 (registration): compile the whole file as ONE (do ...) thunk,
# mkimage-style, then register every def up front. Macros execute at
# compile time inside the unit, as they do in normal compilation — but a
# macro DEFINED in-unit is only usable if installed first, so defmacro
# thunks really execute up front (exactly what loading does; macro
# definitions don't run user code). Nothing else executes. A unit that
# fails (e.g. it needs ran siblings, like boot's setup stanzas, or has
# cross-form forward references, which real loading rejects too)
# records one `uncompiled` line.
(var core-names @{})
(each f args
  (def text (slurp f))
  (def forms (parse-all text))
  (def env (make-env))
  (each name (all-bindings env)
    (def entry (get env name))
    (def v (if (table? entry) (get entry :value) entry))
    (when (or (= :function (type v)) (= :cfunction (type v)))
      (put core-names v (string name))))
  (each form forms
    (when (and (tuple? form) (>= (length form) 1)
               (symbol? (form 0)) (= (string (form 0)) "defmacro"))
      (def mt (try (compile form env f) ([_] nil)))
      (when (= :function (type mt))
        (try (mt) ([e] (eprint "callgraph: macro error in " f ": " e) (os/exit 2))))))
  (def program (tuple/slice @['do ;forms] 0))
  (def res
    (try (compile program env f)
      ([e] [:thrown e])))
  (def thunk (if (and (indexed? res) (= :thrown (res 0))) nil res))
  (cond
    (nil? thunk)
    (array/push edges ["uncompiled" f -1 (string (res 1))])
    (not= :function (type thunk))
    (array/push edges ["uncompiled" f -1 (string (get thunk :error))])
    (register-def (disasm thunk))))

# Pass 2: walk every def exactly once (ids are dense 0..n-1).
(var i 0)
(while (< i (length defs))
  (walk-def-at i core-names)
  (++ i))

# Print nodes.
(var j 0)
(each info defs
  (def d (info :d))
  (print "def " j " " (info :name)
         " arity " (or (d :arity) "?")
         " slots " (or (d :slotcount) "?")
         " instrs " (length (or (d :bytecode) @[])))
  (++ j))

# Print edges.
(each e edges
  (cond
    (= (e 0) "calls") (print "calls " (e 1) " " (e 2))
    (= (e 0) "contains") (print "contains " (e 1) " " (e 2))
    (= (e 0) "extern") (print "extern " (e 1) " " (e 2))
    (= (e 0) "ambiguous") (print "ambiguous " (e 1) " " (e 2))
    (= (e 0) "unknown") (print "unknown " (e 1) " " (e 2))
    (= (e 0) "uncompiled") (print "uncompiled " (e 1) " " (e 2) " " (e 3))
    (= (e 0) "dynamic") (print "dynamic " (e 1) " " (e 2))
    (print "unknown-edge " e)))

(var unresolved 0)
(var externs 0)
(each e edges
  (when (= (e 0) "extern") (++ externs))
  (when (or (= (e 0) "ambiguous") (= (e 0) "unknown"))
    (++ unresolved)))
(print "stats " (length defs) " " callsites " " resolved " " externs " " unresolved)
