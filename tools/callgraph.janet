# tools/callgraph.janet — Phase 4 steps 1+2 (registration + fixpoint).
# Extract the whole-program FuncDef graph from Janet source WITHOUT running
# it: parse -> compile (macros run at compile time, as today) -> disasm ->
# walk. Prints node/edge lines, a stats summary, and a fixpoint block
# (reachability from entry thunks, caller sets, recursion, dynamic taint).
# Read-only; no src/ changes. Usage:
# ./build/janet tools/callgraph.janet FILE [FILE...]
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

(var args (tuple/slice (dyn :args) 1))
# --emit-native DIR: also emit unboxed C for defs whose param/return
# types are concrete (hinted contracts OR inference-proven)
(var emit-native-dir nil)
(var filtered @[])
(var i 0)
(while (< i (length args))
  (def a (args i))
  (if (and (= a "--emit-native") (< (+ i 1) (length args)))
    (do (set emit-native-dir (args (+ i 1))) (++ i))
    (array/push filtered a))
  (++ i))
(set args filtered)
(when (empty? args)
  (eprint "usage: callgraph.janet FILE [FILE...] [--emit-native DIR]")
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
  (array/push defs @{:d d :name name :hint nil})
  (def bucket (get name-ids name))
  (if bucket
    (array/push bucket id)
    (put name-ids name @[id]))
  # children pre-order, right after the parent: ids read top-down and
  # each subtree occupies one contiguous range
  (each child (or (d :defs) @[])
    (register-def child))
  id)

# Step 4 (type hints): optional Clojure-style annotations. Surface:
# (defn f {:hint {:params [:double :long] :returns :long}} [x y] ...)
# — the dict modifier lands in the env metadata table and survives into
# the thunk constants; params maps positionally onto the arglist.
# :long and :double are unboxed; absent (or :number) stays boxed.
# Validation only here — consumption (unboxed codegen) is the next phase.
(def valid-hints @{:long true :double true :number true})
(defn hint-of [id]
  (def info (get defs id))
  (when (nil? info) (break nil))
  (info :hint))
(var all-defn-forms @{})
(defn extract-hints []
  (var nhints 0)
  (each f args
    (def text (slurp f))
    (def forms (parse-all text))
    (each form forms
      (when (and (tuple? form) (>= (length form) 3)
                 (symbol? (form 0))
                 (or (= (string (form 0)) "defn") (= (string (form 0)) "defn-")))
        # keep every defn form so the native emitter can eval one for
        # expected values (hinted or not)
        (put all-defn-forms (string (form 1)) form)
        # find defn forms carrying hints and record the hint under every
        # name-ids entry registered for that name. Two surfaces:
        # (defn f {:hint {...}} ...)            dict modifier
        # (defn f ^long [x ^double y] ...)      ^ syntax (defn strips it
        # into the same metadata; we parse it directly here)
        (var h nil)
        (def m (form 2))
        (when (and (or (table? m) (struct? m)) (get m :hint))
          (set h (get m :hint)))
        (when (and (symbol? m) (= 94 (get (string m) 0)))
          # ^ returns-hint + optional ^ param hints in the arglist
          (def hm @{})
          (put hm :returns (keyword (string/slice (string m) 1)))
          (def arglist (form 3))
          (def ph @[])
          (var k 0)
          (while (< k (length arglist))
            (def a (arglist k))
            (if (and (symbol? a) (= 94 (get (string a) 0)))
              (do
                # hinted param: the hint entry IS the param's entry
                (array/push ph (keyword (string/slice (string a) 1)))
                (++ k)
                (++ k))
              (do
                (array/push ph :number)
                (++ k))))
          (put hm :params (tuple ;ph))
          (set h hm))
        (when h
          (def nm (string (form 1)))
          (def ids (get name-ids nm))
          (when ids
            (each id ids
              (def info (get defs id))
              (put info :hint h))
            (++ nhints))))))
  nhints)

(defn check-hints []
  # validate each hint against its def: param count fits arity, hint
  # keywords are known; emit hint lines and hint-bad problems
  (var nbad 0)
  (each id (range (length defs))
    (def h (hint-of id))
    (when h
      (def d (get (get defs id) :d))
      (def params (get h :params))
      (def returns (get h :returns))
      (var ok true)
      (when params
        (unless (indexed? params)
          (set ok false)
          (array/push edges ["hint-bad" id "params-not-indexed"]))
        (when (indexed? params)
          (def tmax (or (get d :max-arity) 2147483647))
          (when (> (length params) tmax)
            (set ok false)
            (array/push edges ["hint-bad" id "too-many-params"]))
          (each p params
            (unless (and (keyword? p) (get valid-hints p))
              (set ok false)
              (array/push edges ["hint-bad" id "bad-param-hint"]))))
        (when (and (indexed? params) (< (length params) (get d :min-arity 0)))
          # hinted params must cover at least the required prefix
          (set ok false)
          (array/push edges ["hint-bad" id "too-few-params"])))
      (when (and returns (or (not (keyword? returns))
                             (not (get valid-hints returns))))
        (set ok false)
        (array/push edges ["hint-bad" id "bad-returns-hint"]))
      (when ok
        (def pl (if params (string/join (map string params) " ") ""))
        (print "hint " id " "
               (if (empty? pl) "-" pl)
               " -> " (if returns (string returns) "-")))
      (unless ok (++ nbad))))
  nbad)

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
  # shared tail: dynamic > unique image def > ambiguous > extern.
  # returns the resolved target id or nil.
  (cond
    (get dynamic-names tname)
    (do (array/push edges ["dynamic" caller-id tname]) nil)
    (do (def ids (get name-ids tname))
      (cond
        (nil? ids)
        (do (array/push edges ["extern" caller-id tname]) nil)
        (= 1 (length ids))
        (do (array/push edges ["calls" caller-id (ids 0)])
          (++ resolved)
          (ids 0))
        (do (array/push edges ["ambiguous" caller-id tname]) nil)))))

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
  # returns the resolved target id or nil; pushes exactly one edge line
  (++ callsites)
  (var target nil)
  # 1. named local via symbolmap range entry
  (def sname (slot-name d pc slot))
  (if sname
    (set target (resolve-name caller-id sname))
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
                  (= (ct 0) :fn) (set target (resolve-name caller-id (ct 1)))
                  (= (ct 0) :core) (array/push edges ["extern" caller-id (ct 1)])
                  (= (ct 0) :key) (array/push edges ["extern" caller-id (ct 1)])
                  (array/push edges ["unknown" caller-id slot])))
              (= op 'clo)
              (do (def cid (child-at caller-id (ins 2)))
                (if cid
                  (do (array/push edges ["calls" caller-id cid])
                    (++ resolved)
                    (set target cid))
                  (array/push edges ["unknown" caller-id slot])))
              (= op 'lds)
              (do (array/push edges ["calls" caller-id caller-id])
                (++ resolved)
                (set target caller-id))
              (= op 'ldu)
              (do (def uname (if (>= (length ins) 4) (upvalue-name d (ins 3)) nil))
                (if uname
                  (set target (resolve-name caller-id uname))
                  (array/push edges ["unknown" caller-id slot])))
              (array/push edges ["unknown" caller-id slot]))
            (set done true)))
        (-- j))
      (when (not done)
        (array/push edges ["unknown" caller-id slot]))))
  target)

# Step 3 (call-site arity audit): count pushed args with a strict
# straight-line pattern only — consecutive push/push2/push3 plus at most
# one callee-slot load. Loads between pushes are arg temps (slots are
# reused), so any load op is fine; anything else that is not a push or
# a load means the args are computed/spread — `arity-unknown`, never a
# false mismatch. Returns the count or nil.
(var called-as @{})   # target id -> table used as int set
(var hinted-targets @{})  # hinted callee ids actually called somewhere
(var arity-bad 0)
(var arity-unknown 0)
(def load-ops
  @{'ldc true 'clo true 'lds true 'ldu true 'ldi true 'ldn true
    'ldt true 'ldf true 'get true 'in true 'movn true 'movf true
    'geti true 'add true 'sub true 'mul true 'div true 'addim true
    'subim true 'mulim true 'len true 'equ true 'lt true 'gt true
    'ltu true 'gtu true 'equim true 'ltim true 'gtim true})
(defn call-arity [d pc slot]
  (def code (or (d :bytecode) @[]))
  (var n 0)
  (var j (- pc 1))
  (var scanned 0)
  (var ok true)
  (while (and (>= j 0) (< scanned 60) (= ok true))
    (++ scanned)
    (def ins (code j))
    (def op (ins 0))
    (cond
      (= op 'push) (+= n 1)
      (= op 'push2) (+= n 2)
      (= op 'push3) (+= n 3)
      # a previous call/tcall/return/jump ends this site's arg block
      # cleanly (ops before it are a different branch or call)
      (get flow-stop-ops op) (set ok false)
      (= op 'call) (set ok false)
      (= op 'pusha) (set ok nil)
      (get load-ops op) nil   # arg temps / callee load: harmless
      (set ok nil))
    (-- j))
  # running off the start (j < 0) or hitting a previous call/tcall is a
  # clean boundary; anything else unknown means the pattern was broken
  (if (or (< j 0) (= ok false)) n nil))

(defn audit-site [caller-id d pc slot tid]
  (def n (call-arity d pc slot))
  (if (nil? n)
    (do (++ arity-unknown)
      (array/push edges ["arity-unknown" caller-id]))
    (when tid
      (def tinfo (get defs tid))
      (when (nil? tinfo)
        (eprint "DBG tid=" tid " ndefs=" (length defs))
        (break))
      (def tdef (tinfo :d))
      (def tmin (or (tdef :min-arity) 0))
      (def tmax (or (tdef :max-arity) 2147483647))
      (def seen (or (get called-as tid) @{}))
      (put seen n true)
      (put called-as tid seen)
      (when (or (< n tmin) (> n tmax))
        (++ arity-bad)
        (array/push edges ["arity-bad" caller-id tid n]))
      # hinted callee: record the call for unboxing decisions
      (when (tinfo :hint)
        (put hinted-targets tid true)))))

(var call-map @{})  # id -> table {pc -> resolved target id} for inference

(defn walk-def-at [id core-names]
  # contains edges to direct children (found via contiguous ranges),
  # then this def's own call sites (each audited for arity)
  (def info (defs id))
  (def d (info :d))
  (var cid (+ id 1))
  (each child (or (d :defs) @[])
    (array/push edges ["contains" id cid])
    (+= cid (subtree-size cid)))
  (def cmap @{})
  (put call-map id cmap)
  (def code (or (d :bytecode) @[]))
  (var pc 0)
  (each ins code
    (def op (ins 0))
    (cond
      (= op 'call)
      (do (def tid (record-call id d pc (ins 2) core-names))
        (put cmap pc tid)
        (audit-site id d pc (ins 2) tid))
      (= op 'tcall)
      (do (def tid (record-call id d pc (ins 1) core-names))
        (put cmap pc tid)
        (audit-site id d pc (ins 1) tid))
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

# Step 4: extract type hints BEFORE the walk so audit-site sees them.
(var nhints (extract-hints))

# Pass 2: walk every def exactly once (ids are dense 0..n-1).
(var i 0)
(while (< i (length defs))
  (walk-def-at i core-names)
  (++ i))

# validate hints (after the walk: hint-bad lines join the edge stream)
(var nbad-hints (check-hints))

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
    (= (e 0) "arity-bad") (print "arity-bad " (e 1) " " (e 2) " " (e 3))
    (= (e 0) "arity-unknown") (print "arity-unknown " (e 1))
    (= (e 0) "hint-bad") (print "hint-bad " (e 1) " " (e 2))
    (print "unknown-edge " e)))

(var unresolved 0)
(var externs 0)
(each e edges
  (when (= (e 0) "extern") (++ externs))
  (when (or (= (e 0) "ambiguous") (= (e 0) "unknown"))
    (++ unresolved)))
(print "stats " (length defs) " " callsites " " resolved " " externs " " unresolved)

# Pass 3 (fixpoint): reachability from entry thunks, caller sets,
# recursion, dynamic taint. Roots are defs named "thunk" — every
# top-level unit may run. Follows calls + contains; predecessors come
# from both (a container is a caller of its children for taint).
(var succ @{})
(var pred @{})
(var caller-sets @{})
(var dynsite @{})
(each id (range (length defs))
  (put succ id @[])
  (put pred id @[])
  (put caller-sets id @[]))
(each e edges
  (cond
    (= (e 0) "calls")
    (do (array/push (get succ (e 1)) (e 2))
      (array/push (get pred (e 2)) (e 1))
      (array/push (get caller-sets (e 2)) (e 1)))
    (= (e 0) "contains")
    (do (array/push (get succ (e 1)) (e 2))
      (array/push (get pred (e 2)) (e 1)))
    (= (e 0) "dynamic")
    (put dynsite (e 1) true)
    nil))
(defn reachable-from [roots]
  (def seen @{})
  (def work @[])
  (each r roots
    (when (nil? (get seen r))
      (put seen r true)
      (array/push work r)))
  (while (not (empty? work))
    (def id (array/pop work))
    (each t (get succ id)
      (when (nil? (get seen t))
        (put seen t true)
        (array/push work t))))
  seen)
(var roots @[])
(var ri 0)
(each info defs
  (when (= (info :name) "thunk")
    (array/push roots ri))
  (++ ri))
(def reachable (reachable-from roots))
# recursive: id is re-reached through at least one edge, i.e. some
# successor reaches back (self-edge counts)
(var recursive @{})
(each id (range (length defs))
  (var found false)
  (each t (get succ id)
    (when (get (reachable-from @[t]) id)
      (set found true)
      (break)))
  (when found (put recursive id true)))
# tainted: has a dynamic site or transitively reaches one (fixpoint
# backwards from dynamic defs over calls + contains predecessors)
(var tainted @{})
(def twork @[])
(each id (range (length defs))
  (when (get dynsite id)
    (put tainted id true)
    (array/push twork id)))
(while (not (empty? twork))
  (def id (array/pop twork))
  (each p (get pred id)
    (when (nil? (get tainted p))
      (put tainted p true)
      (array/push twork p))))
# print: reach set, per-def callers, recursive, tainted, summary
(def reach-list (sorted (keys reachable)))
(print "reach" (if (empty? reach-list) "" (string " " (string/join (map string reach-list) " "))))
(each id (range (length defs))
  (def cs (sorted (distinct (get caller-sets id))))
  (when (not (empty? cs))
    (print "callers " id " " (length cs) " " (string/join (map string cs) " "))))
(each id (range (length defs))
  (when (get recursive id)
    (print "recursive " id)))
(each id (range (length defs))
  (when (get tainted id)
    (print "tainted " id)))
(print "fix " (length reach-list) " " (length recursive) " " (length tainted))
(each tid (sort (keys called-as))
  (def counts (sorted (keys (get called-as tid))))
  (print "called-as " tid " " (string/join (map string counts) " ")))
(print "arity " arity-bad " " arity-unknown)
(print "hints " nhints " " nbad-hints)
# hinted defs with at least one resolvable call site: unboxing candidates
(var hc @[])
(each id (range (length defs))
  (when (and (get (get defs id) :hint) (get hinted-targets id))
    (array/push hc id)))
(print "hint-call" (if (empty? hc) "" (string " " (string/join (map string hc) " "))))

(import ./infer)

# Step 6+ (hybrid inference): hints are direct contracts (used as-is for
# unboxing, never weakened); call-site widening + fixpoint prove the
# unhinted rest. Concrete => unboxed, unproven => boxed.
(def res (infer/analyze defs call-map))
(def rts (res :rts))
(def pts (res :pts))
(each id (range (length defs))
  (def t (rts id))
  (when (get infer/numeric-types t)
    (print "ret " id " " t)))
(each id (range (length defs))
  (def parr (get pts id))
  (var all-concrete (> (length parr) 0))
  (each t parr
    (unless (get infer/numeric-types t) (set all-concrete false)))
  (when all-concrete
    (print "ptypes " id " " (string/join (map string parr) " "))))
(def cnts @{:long 0 :double 0 :number 0})
(each id (range (length defs))
  (def t (rts id))
  (when (get cnts t) (put cnts t (+ 1 (get cnts t)))))
(print "rets " (get cnts :long) " " (get cnts :double) " " (get cnts :number) " rounds " (res :rounds))

# Step 7 (--emit-native): unboxed C emission. A def emits when its
# params and return are ALL concrete numeric — from hints (contracts,
# used directly) or from inference (widening + fixpoint proven) — and
# its body is in the supported subset. Straight-line arithmetic plus
# direct calls to other emitted natives (two-round dependency pass);
# `/` only for :double returns (janet / is real division; C / on
# int64 truncates).
(def emit-c-ops
  @{'ldi true 'ldc true 'add true 'sub true 'mul true 'div true
    'addim true 'subim true 'mulim true 'ret true
    'push true 'push2 true 'push3 true 'call true 'tcall true
    # callee-only loads: the C call targets the native symbol directly,
    # so ldu/lds feeding a call are no-ops (the value never crosses)
    'ldu true 'lds true})
(def emit-arith-c @{'add "+" 'sub "-" 'mul "*" 'div "/"})
(def emit-arith-im-c @{'addim "+" 'subim "-" 'mulim "*"})

(defn emit-c-type [t] (if (= t :long) "int64_t" "double"))
(defn emit-c-name [nm] (string/replace-all "-" "_" nm))
(defn emit-c-num [v] (string/format "%.17g" v))
(defn emit-slot [a arity] (if (< a arity) (string "p" a) (string "s" a)))

(defn emit-fn-c [id d rt pt call-map emitted-names]
  (def arity (length pt))
  (when (d :vararg) (break [nil "vararg"]))
  (unless (get infer/numeric-types rt) (break [nil "return not concrete"]))
  (def pdecls (seq [i :range [0 arity]]
                (string (emit-c-type (pt i)) " p" i)))
  (def ct (emit-c-type rt))
  (def lines @[])
  (var bad nil)
  (var pending @[])
  (var spread false)
  (def cmap (or (get call-map id) @{}))
  (var pc 0)
  (each ins (or (d :bytecode) @[])
    (def op (ins 0))
    (if (get emit-c-ops op)
      (cond
        (= op 'ldi)
        (array/push lines (string "    const " ct " s" (ins 1) " = " (ins 2) ";"))
        (= op 'ldc)
        (do (def cval ((or (d :constants) @[]) (ins 2)))
          (unless (and cval (= :number (type cval)))
            (set bad "ldc non-number")
            (break))
          (array/push lines (string "    const " ct " s" (ins 1) " = " (emit-c-num cval) ";")))
        (= op 'ret)
        (array/push lines (string "    return s" (ins 1) ";"))
        (= op 'push) (array/push pending (emit-slot (ins 1) arity))
        (= op 'push2)
        (do (array/push pending (emit-slot (ins 1) arity))
          (array/push pending (emit-slot (ins 2) arity)))
        (= op 'push3)
        (do (array/push pending (emit-slot (ins 1) arity))
          (array/push pending (emit-slot (ins 2) arity))
          (array/push pending (emit-slot (ins 3) arity)))
        (= op 'pusha) (do (set bad "spread args") (break))
        (= op 'div)
        (if (= rt :double)
          (do (set pending @[])
            (array/push lines
                        (string "    const double s" (ins 1) " = "
                                (emit-slot (ins 2) arity) " / "
                                (emit-slot (ins 3) arity) ";")))
          (do (set bad "div on long")
            (break)))
        (or (= op 'add) (= op 'sub) (= op 'mul))
        (do (set pending @[])
          (array/push lines
                      (string "    const " ct " s" (ins 1) " = "
                              (emit-slot (ins 2) arity) " " (emit-arith-c op) " "
                              (emit-slot (ins 3) arity) ";")))
        (get emit-arith-im-c op)
        (do (set pending @[])
          (array/push lines
                      (string "    const " ct " s" (ins 1) " = "
                              (emit-slot (ins 2) arity) " " (emit-arith-im-c op) " "
                              (ins 3) ";")))
        (or (= op 'call) (= op 'tcall))
        (do (def tid (get cmap pc))
          (def cname (if tid (get emitted-names tid)))
          (unless (and tid cname (not spread))
            (set bad "call to non-native")
            (break))
          (def callx (string (emit-c-name cname) "(" (string/join pending ", ") ")"))
          (if (= op 'call)
            (array/push lines (string "    const " ct " s" (ins 1) " = " callx ";"))
            (array/push lines (string "    return " callx ";")))
          (set pending @[])))
      (do (set bad (string "op " op))
        (break)))
    (++ pc))
  (if bad
    [nil bad]
    [(string "static " ct " " (emit-c-name (string (or (d :name) "?"))) "("
             (string/join pdecls ", ") ") {\n" (string/join lines "\n") "\n}")
     nil]))

(defn emit-native-all [outdir]
  (os/mkdir outdir)
  # candidates: fully concrete params + return, non-vararg
  (def cand @{})
  (each id (range (length defs))
    (def info (get defs id))
    (def d (info :d))
    (def pt (get pts id))
    (def rt (rts id))
    (var concrete (and (> (length pt) 0) (not (d :vararg))))
    (each t pt (unless (get infer/numeric-types t) (set concrete false)))
    (unless (get infer/numeric-types rt) (set concrete false))
    (when concrete (put cand id true)))
  # two-round dependency resolution: leaves first, then callers whose
  # targets are all emitted
  (def emitted-names @{})  # id -> janet name
  (def emitted @[])
  (def cand-ids (filter (fn [k] (number? k)) (sort (keys cand))))
  (var round 0)
  (var changed true)
  (while (and changed (< round 4))
    (++ round)
    (set changed false)
    (each id cand-ids
      (unless (get emitted-names id)
        (def info (get defs id))
        (def d (info :d))
        (def [code reason] (emit-fn-c id d (rts id) (get pts id) call-map emitted-names))
        (if code
          (do (def nm (string (d :name)))
            (put emitted-names id nm)
            (array/push emitted {:id id :name nm :code code :pt (get pts id) :rt (rts id)})
            (set changed true))
          (put cand (string id "-r" reason) reason)))))
  # report skips: candidates that never made it
  (def skips @[])
  (each id cand-ids
    (unless (get emitted-names id)
      (array/push skips [id (string (get ((get defs id) :d) :name)) "unsupported or calls non-native"])))
  (each [id nm r] skips (print "native-skip " id " " nm " " r))
  (when (empty? emitted)
    (eprint "emit-native: no emittable functions")
    (os/exit 1))
  (defn samples-for [pt]
    (def per (map (fn [t] (if (= t :long) @[0 1 3] @[0.5 1.5 2.5])) pt))
    (var combos @[@[]])
    (each opts per
      (def nxt @[])
      (each c combos
        (each v opts
          (def cc (array/slice c))
          (array/push cc v)
          (array/push nxt cc)))
      (set combos nxt))
    combos)
  (def c-parts @[])
  (def calls @[])
  (each e emitted
    (array/push c-parts (e :code))
    (def form (get all-defn-forms (e :name)))
    (unless form
      (eprint "emit-native: no form for " (e :name))
      (os/exit 1))
    (def fnval (eval form))
    (each combo (samples-for (e :pt))
      (array/push calls {:e e :args (tuple ;combo) :fn fnval})))
  (def cout @"")
  (buffer/push-string cout "#include <stdio.h>\n#include <inttypes.h>\n\n")
  (each c c-parts (buffer/push-string cout c) (buffer/push-string cout "\n\n"))
  (buffer/push-string cout "int main(void) {\n")
  (each c calls
    (def args-c (string/join (map string (c :args)) ", "))
    (buffer/push-string cout
      (string "    printf(\"%.17g\\n\", (double) " (emit-c-name ((c :e) :name))
              "(" args-c "));\n")))
  (buffer/push-string cout "    return 0;\n}\n")
  (spit (string outdir "/native.c") cout)
  (def expected @[])
  (each c calls
    (def v ((c :fn) ;(c :args)))
    (array/push expected (string/format "%.17g" v)))
  (spit (string outdir "/expected.txt") (string (string/join expected "\n") "\n"))
  (print "native " (length emitted) " fns " (length calls) " samples " outdir))

(when emit-native-dir (emit-native-all emit-native-dir))