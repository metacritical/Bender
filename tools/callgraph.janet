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
# --externs FILE: janet file defining an `externs` dictionary —
# {"janet-name" {:c "csymbol" :params [...] :returns t
#                :fallback (fn ...)}}
# The fallback pre-binds the name so the unit compiles and provides
# parity semantics; native emission calls the C symbol directly (the
# raylib story).
(var emit-native-dir nil)
(var emit-native-lib-dir nil)
(var externs-file nil)
# Step 13: int-overflow policy. wrap (default): kernels compute in
# exact int widths with two's-complement wrap (shipped/harness builds
# must pass -fwrapv: C signed overflow is otherwise UB). promote:
# int widths compute as double (oracle-identical everywhere, no
# overflow below 2^53... same as boxed — exact parity by
# construction, at some speed cost for int-heavy kernels).
(var int-policy :wrap)
(var filtered @[])
(var i 0)
(while (< i (length args))
  (def a (args i))
  (cond
    (and (= a "--emit-native") (< (+ i 1) (length args)))
    (do (set emit-native-dir (args (+ i 1))) (++ i))
    (and (= a "--emit-native-lib") (< (+ i 1) (length args)))
    (do (set emit-native-lib-dir (args (+ i 1))) (++ i))
    (and (= a "--externs") (< (+ i 1) (length args)))
    (do (set externs-file (args (+ i 1))) (++ i))
    (and (= a "--int-policy") (< (+ i 1) (length args)))
    (do (def pv (keyword (args (+ i 1))))
      (unless (or (= pv :wrap) (= pv :promote))
        (eprint "int-policy must be wrap or promote")
        (os/exit 4))
      (set int-policy pv)
      (++ i))
    (array/push filtered a))
  (++ i))
(set args filtered)
(var externs {})
(when externs-file
  # the externs file must define an `externs` dictionary binding
  (set externs (get (get (dofile externs-file) (quote externs)) :value))
  (unless (dictionary? externs)
    (eprint "externs file must define an `externs` dictionary")
    (os/exit 4)))
(var extern-call-map @{})  # id -> {pc -> extern janet name}
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
# :float is true float32 computation; :double stays float64.
(def valid-hints
  @{:long true :double true :number true :float true
    :i32 true :i16 true :i8 true
    :u64 true :u32 true :u16 true :u8 true
    :array true})
# Full C primitive width set (step 13/width): 64-bit is the default
# integer (:long); narrower widths are opt-in contracts honored at the
# FFI boundary; casts are automatic (C implicit conversion).
# Canonical: :long :i32 :i16 :i8 :u64 :u32 :u16 :u8 :double :float
# Aliases: :i64/:int64, :i32/:int32/:int, :i16/:int16, :i8/:int8,
# :u64/:uint64, :u32/:uint32, :u16/:uint16, :u8/:uint8,
# :f64/:double, :f32/:float.
(def hint-canonical
  @{:i64 :long :int64 :long
    :i32 :i32 :int32 :i32 :int :i32
    :i16 :i16 :int16 :i16
    :i8 :i8 :int8 :i8
    :u64 :u64 :uint64 :u64
    :u32 :u32 :uint32 :u32
    :u16 :u16 :uint16 :u16
    :u8 :u8 :uint8 :u8
    :f64 :double :f32 :float})
(def int-hints
  @{:long true :i32 true :i16 true :i8 true
    :u64 true :u32 true :u16 true :u8 true})
(def float-hints @{:double true :float true})
(def emit-c-type-map
  @{:long "int64_t" :i32 "int32_t" :i16 "int16_t" :i8 "int8_t"
    :u64 "uint64_t" :u32 "uint32_t" :u16 "uint16_t" :u8 "uint8_t"
    :double "double" :float "float" :number "double"
    :array "JanetArray*"})
# alias spellings fold to the canonical width keyword
(defn canon-hint [k] (get hint-canonical k k))
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
        # bare param-only hints: (defn f [x ^double y] ...) — no dict,
        # no ^ return hint, but the arglist carries ^ params
        (when (nil? h)
          (def arglist (if (or (table? m) (struct? m)) (form 3) (form 2)))
          (when (and (tuple? arglist)
                     (some (fn [a] (and (symbol? a) (= 94 (get (string a) 0))))
                           arglist))
            (def hm @{})
            (def ph @[])
            (var k 0)
            (while (< k (length arglist))
              (def a (arglist k))
              (if (and (symbol? a) (= 94 (get (string a) 0)))
                (do
                  (var pnm (string/slice (string a) 1))
                  (if (= 58 (get pnm 0)) (set pnm (string/slice pnm 1)))
                  (def pk (keyword pnm))
                  (array/push ph (canon-hint pk))
                  (++ k) (++ k))
                (do (array/push ph :number) (++ k))))
            (put hm :params (tuple ;ph))
            (set h hm)))
        (when (and (symbol? m) (= 94 (get (string m) 0)))
          # ^ returns-hint + optional ^ param hints in the arglist
          (def hm @{})
          (var rnm (string/slice (string m) 1))
          (if (= 58 (get rnm 0)) (set rnm (string/slice rnm 1)))
          (def rk (keyword rnm))
          (put hm :returns (if (= rk :float) :double rk))
          (def arglist (form 3))
          (def ph @[])
          (var k 0)
          (while (< k (length arglist))
            (def a (arglist k))
            (if (and (symbol? a) (= 94 (get (string a) 0)))
              (do
                # hinted param: the hint entry IS the param's entry
                (var pnm (string/slice (string a) 1))
                (if (= 58 (get pnm 0)) (set pnm (string/slice pnm 1)))
                (def pk (keyword pnm))
                (array/push ph (canon-hint pk))
                (++ k)
                (++ k))
              (do
                (array/push ph :number)
                (++ k))))
          (put hm :params (tuple ;ph))
          (set h hm))
        # canonicalize alias spellings (:int -> :i32, :f32 -> :float)
        # so every consumer sees one width vocabulary
        (when h
          (when (struct? h) (set h (table ;(kvs h))))
          (when (get h :params)
            (put h :params (tuple ;(map canon-hint (get h :params)))))
          (when (get h :returns)
            (put h :returns (canon-hint (get h :returns)))))
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

(defn resolve-name [caller-id tname pc]
  # shared tail: dynamic > declared extern > unique image def >
  # ambiguous > unknown. Returns the resolved target id, an extern
  # NAME string (recorded in extern-call-map), or nil.
  (cond
    (get dynamic-names tname)
    (do (array/push edges ["dynamic" caller-id tname]) nil)
    (get externs tname)
    (do (array/push edges ["extern-call" caller-id tname])
      (put (or (get extern-call-map caller-id)
               (do (def t2 @{}) (put extern-call-map caller-id t2) t2))
           pc tname)
      tname)
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
  # returns the resolved target id, an extern name string, or nil;
  # pushes exactly one edge line
  (++ callsites)
  (var target nil)
  # 1. named local via symbolmap range entry
  (def sname (slot-name d pc slot))
  (if sname
    (set target (resolve-name caller-id sname pc))
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
                  (= (ct 0) :fn) (set target (resolve-name caller-id (ct 1) pc))
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
                  (set target (resolve-name caller-id uname pc))
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
      (when (nil? tinfo) (break))
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

# Pre-execute top-level import/require/use forms into the unit env -- the
# same thing normal loading does (compile+exec each form in turn) before
# later forms compile. Single-thunk compilation otherwise never merges
# prefixed module bindings (WIPEMOD/GROUPS etc.), so multi-module programs
# fail with "unknown symbol". Modules load through the real loader
# (require: cached, resolves relatives recursively), then merge-module
# applies :as/:prefix/:only/:export exactly like import*. Runtime requires
# inside functions are out of scope (same as the one-thunk model).
# Keep in sync between tools/callgraph.janet and tools/mkimage.janet.
(defn preimport-unit-imports [env unitfile forms]
  (def prev-current-file (dyn :current-file))
  (setdyn :current-file unitfile)
  (each form forms
    (when (and (tuple? form) (>= (length form) 2))
      (def head (form 0))
      (when (and (symbol? head)
                 (or (= head (quote import))
                     (= head (quote use))
                     (= head (quote require))))
        (try
          (do (def path (string (form 1)))
            (def modenv (require path))
            (when (or (= head (quote import)) (= head (quote use)))
              (def kargs (table ;(tuple/slice form 2)))
              (def as (get kargs :as))
              (def prefix (get kargs :prefix))
              (def only (get kargs :only))
              (def ep (or (get kargs :export) (= head (quote use))))
              (def pfx (or (and as (string as "/")) prefix
                           (string (last (string/split "/" (string path))) "/")))
              (merge-module env modenv pfx ep only)))
          ([e] (eprint "aot: import pre-execution failed for " (form 1) ": " e)
               (os/exit 2))))))
  (setdyn :current-file prev-current-file))

(var core-names @{})
(each f args
  (def text (slurp f))
  (def forms (parse-all text))
  (def env (make-env))
  # mirror the janet CLI: the script's directory joins the module syspath
  # so relative imports (../src/...) resolve like a normal `janet f.janet`
  (def slash-idx (string/find "/" (string/reverse f)))
  (def dirname (if (nil? slash-idx) "." (string/slice f 0 (- (length f) slash-idx 1))))
  (module/add-syspath dirname)
  (preimport-unit-imports env f forms)
  # extern fallbacks pre-bind their names so the unit compiles and
  # provides the parity oracle
  (each name (keys externs)
    (def e (get externs name))
    (put env (symbol name) @{:value (get e :fallback)}))
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
(var extern-count 0)
(each e edges
  (when (= (e 0) "extern") (++ extern-count))
  (when (or (= (e 0) "ambiguous") (= (e 0) "unknown"))
    (++ unresolved)))
(print "stats " (length defs) " " callsites " " resolved " " extern-count " " unresolved)

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

# Step 7 (--emit-native) + Step 8 (branches): unboxed C emission. A def
# emits when its params and return are ALL concrete numeric — hinted
# (contracts, used directly) or inference-proven (widening) — and its
# body translates: straight-line arithmetic, push-arg direct calls to
# other emitted natives (two-round dependency pass), and BRANCHES: the
# jump ops recover a CFG, block-entry slot types come from iterative
# dataflow join, branch conditions must be :bool (janet truthiness
# makes 0 truthy — C truthiness on a numeric slot would diverge), and
# jumps emit labels + goto. `/` only for :double returns (janet / is
# real division; C / on int64 truncates).
(def emit-c-ops
  @{'ldi true 'ldc true 'add true 'sub true 'mul true 'div true
    'addim true 'subim true 'mulim true 'ret true
    'push true 'push2 true 'push3 true 'call true 'tcall true
    'jmp true 'jmpif true 'jmpno true
    'movn true 'movf true
    'lt true 'ltim true 'gt true 'gtim true 'lte true 'gte true
    'eq true 'eqim true 'neq true 'neqim true
    # callee-only loads: the C call targets the native symbol directly,
    # so ldu/lds feeding a call are no-ops
    'ldu true 'lds true})
(def emit-arith-c @{'add "+" 'sub "-" 'mul "*" 'div "/"})
(def emit-arith-im-c @{'addim "+" 'subim "-" 'mulim "*"})
(def emit-cmp-c @{'lt "<" 'ltim "<" 'gt ">" 'gtim ">"
                  'lte "<=" 'gte ">="
                  'eq "==" 'eqim "==" 'neq "!=" 'neqim "!="})
# dest slot operand position per writing op (params excluded from decls)
(def emit-writers
  @{'ldi 1 'ldc 1 'add 1 'sub 1 'mul 1 'div 1 'addim 1 'subim 1 'mulim 1
    'lt 1 'ltim 1 'gt 1 'gtim 1 'lte 1 'gte 1
    'eq 1 'eqim 1 'neq 1 'neqim 1 'call 1 'movn 1 'movf 2
    'len 1 'get 1 'put 1})
(def emit-cmp-im @{'ltim true 'gtim true 'eqim true 'neqim true})
(def emit-term-ops @{'ret true 'retn true 'err true})
(def emit-cond-jumps @{'jmpif true 'jmpno true})

(defn emit-c-type [t] (get emit-c-type-map t "double"))
# Step 13: kernel-side type mapping honors --int-policy. promote
# runs int widths as double (oracle-identical); extern C declarations
# keep exact types (foreign signatures are fixed).
(defn emit-kernel-type [t &opt fb]
  (default fb "double")
  (if (and (= int-policy :promote) (get int-hints t))
    "double"
    (get emit-c-type-map t fb)))
(defn emit-c-name [nm] (string/replace-all "-" "_" nm))
(defn emit-c-num [v] (string/format "%.17g" v))
(defn emit-slot [a arity] (if (< a arity) (string "p" a) (string "s" a)))

# Step 8: CFG recovery + block-entry slot dataflow + validation.
# Returns @{:ok true :leaders [...] :block-end {...} :states {pc -> st}}
# or @{:ok false :reason string}.
(defn flow-check [id d pt rt call-map rts]
  (def code (or (d :bytecode) @[]))
  (def n (length code))
  (def cmap (or (get call-map id) @{}))
  # leaders: entry, jump targets, post-terminator / post-jump
  (def leader? @{0 true})
  (var pc 0)
  (each ins code
    (def op (ins 0))
    (cond
      (= op 'jmp)
      (do (def t (+ pc (ins 1)))
        (when (and (>= t 0) (< t n)) (put leader? t true))
        (when (< (+ pc 1) n) (put leader? (+ pc 1) true)))
      (get emit-cond-jumps op)
      (do (def t (+ pc (ins 2)))
        (when (and (>= t 0) (< t n)) (put leader? t true))
        (when (< (+ pc 1) n) (put leader? (+ pc 1) true)))
      (get emit-term-ops op)
      (when (< (+ pc 1) n) (put leader? (+ pc 1) true))
      nil)
    (++ pc))
  (def leaders (sort (keys leader?)))
  (def block-end @{})
  (var bi 0)
  (while (< bi (length leaders))
    (put block-end (leaders bi)
         (if (< (+ bi 1) (length leaders)) (leaders (+ bi 1)) n))
    (++ bi))
  # dataflow: block-entry states, conservative join at merges
  (def st0 @{})
  (var pi 0)
  (each t pt (put st0 pi t) (++ pi))
  (def in-states @{0 st0})
  (def changed-box @{:v true})
  (defn state-copy [st]
    (def c2 @{})
    (eachk k st (put c2 k (get st k)))
    c2)
  (defn merge-to [tgt stx]
    (def cur (get in-states tgt))
    (if (nil? cur)
      (do (put in-states tgt (state-copy stx))
        (put changed-box :v true))
      (do (eachk k stx
        (def a (get cur k :unknown))
        (def b (get stx k :unknown))
        (def nn (if (or (= a :unknown) (= b :unknown))
                  :unknown
                  (infer/type-join a b)))
        (when (not= nn a)
          (put cur k nn)
          (put changed-box :v true))))))
  (var guard 0)
  (while (and (get changed-box :v) (< guard 20))
    (++ guard)
    (put changed-box :v false)
    (each start leaders
      (def inst (get in-states start))
      (when inst
        (def st (state-copy inst))
        (def endx (get block-end start))
        (var pc start)
        (while (< pc endx)
          (def ins (code pc))
          (def op (ins 0))
          (cond
            (= op 'ldi) (put st (ins 1) :long)
            (= op 'ldc) (put st (ins 1) (infer/const-num-type d (ins 2)))
            (= op 'ldn) (put st (ins 1) :nil)
            (or (= op 'ldt) (= op 'ldf)) (put st (ins 1) :bool)
            (= op 'len) (put st (ins 1) :long)
            (= op 'movn) (put st (ins 1) (get st (ins 2) :unknown))
            (= op 'movf) (put st (ins 2) (get st (ins 1) :unknown))
            (get infer/arith-im-ops op)
            (put st (ins 1) (infer/arith-join (get st (ins 2) :unknown) :long))
            (get infer/arith-ops op)
            (put st (ins 1)
                 (infer/arith-join (get st (ins 2) :unknown)
                                   (get st (ins 3) :unknown)))
            (get infer/cmp-ops op) (put st (ins 1) :bool)
            # Step 11: array access (mirrors infer/scan-def rules)
            (= op 'get)
            (put st (ins 1)
                 (if (and (= :array (get st (ins 2) :unknown))
                          (infer/int-family? (get st (ins 3) :unknown)))
                   :number :unknown))
            (= op 'put)
            (do (def pa (get st (ins 1) :unknown))
              (when (= pa :array)
                (unless (and (infer/int-family? (get st (ins 2) :unknown))
                             (get infer/numeric-types (get st (ins 3) :unknown)))
                  (put st (ins 1) :unknown))))
            (= op 'call)
            (do (def tid (get cmap pc))
              (when tid (put st (ins 1) (rts tid))))
            nil)
          (++ pc))
        (def lastins (code (- endx 1)))
        (def lop (lastins 0))
        (cond
          (get emit-term-ops lop) nil
          (= lop 'jmp) (merge-to (+ (- endx 1) (lastins 1)) st)
          (get emit-cond-jumps lop)
          (do (merge-to (+ (- endx 1) (lastins 2)) st)
            (merge-to endx st))
          (merge-to endx st)))))
  (when (get changed-box :v)
    (break @{:ok false :reason "dataflow did not converge"}))
  # per-tier slot purity: an int-width function must never see a
  # float-family value in a slot (the emitter types temps by the
  # return width; implicit conversion would silently truncate)
  (when (get int-hints rt)
    (var impure nil)
    (each start leaders
      (if impure
        nil
        (do (def st (get in-states start))
          (when st
            (eachp [k v] st
              # Step 11: :array slots are pointer temps, never confused
              # with numeric conversions — exempt from purity
              (when (and (not (get int-hints v)) (not= v :bool) (not= v :array) (>= k (length pt)) (nil? impure))
                (set impure (string "non-int slot " k " (" v ") in int fn"))))))))
  (if impure
    @{:ok false :reason impure}
    nil))
  (if (get changed-box :v)
    @{:ok false :reason "dataflow did not converge"}
    @{:ok true :leaders leaders :block-end block-end :states in-states}))

(defn emit-fn-c [id d rt pt call-map emitted-names rts]
  (def arity (length pt))
  (when (d :vararg) (break [nil "vararg"]))
  (unless (get infer/numeric-types rt) (break [nil "return not concrete"]))
  (def fl (flow-check id d pt rt call-map rts))
  (unless (fl :ok) (break [nil (fl :reason)]))
  (def leaders (fl :leaders))
  (def block-end (fl :block-end))
  (def states (fl :states))
  (def code (or (d :bytecode) @[]))
  (def n (length code))
  (def cmap (or (get call-map id) @{}))
  (def pdecls (seq [i :range [0 arity]]
                (string (emit-kernel-type (pt i)) " p" i)))
  # collect written slots (loop bodies reassign) -> declare once at top.
  # Step 11: declarations carry each temp's own type (array temps are
  # JanetArray*, not the return width). The sweep mirrors the trackers
  # below; unknown types fall back to ct (previous behavior exactly).
  (def written @{})
  (def wtypes @{})
  (each i (range arity) (put wtypes i (pt i)))
  (var wpc 0)
  (each ins code
    (def wop (ins 0))
    (cond
      (= wop 'ldi) (put wtypes (ins 1) :long)
      (= wop 'ldc) (put wtypes (ins 1) (infer/const-num-type d (ins 2)))
      (= wop 'ldn) (put wtypes (ins 1) :nil)
      (or (= wop 'ldt) (= wop 'ldf)) (put wtypes (ins 1) :bool)
      (= wop 'len) (put wtypes (ins 1) :long)
      (= wop 'movn) (put wtypes (ins 1) (get wtypes (ins 2) :unknown))
      (= wop 'movf) (put wtypes (ins 2) (get wtypes (ins 1) :unknown))
      (get infer/arith-im-ops wop)
      (put wtypes (ins 1) (infer/arith-join (get wtypes (ins 2) :unknown) :long))
      (get infer/arith-ops wop)
      (put wtypes (ins 1)
           (infer/arith-join (get wtypes (ins 2) :unknown)
                             (get wtypes (ins 3) :unknown)))
      (get infer/cmp-ops wop) (put wtypes (ins 1) :bool)
      (= wop 'get)
      (put wtypes (ins 1)
           (if (and (= :array (get wtypes (ins 2) :unknown))
                    (infer/int-family? (get wtypes (ins 3) :unknown)))
             :number :unknown))
      (= wop 'put)
      (do (def wpa (get wtypes (ins 1) :unknown))
        (when (= wpa :array)
          (unless (and (infer/int-family? (get wtypes (ins 2) :unknown))
                       (get infer/numeric-types (get wtypes (ins 3) :unknown)))
            (put wtypes (ins 1) :unknown))))
      (= wop 'call)
      (do (def wtid (get cmap wpc))
        (when wtid (put wtypes (ins 1) (rts wtid))))
      nil)
    (++ wpc)
    (def wpos (get emit-writers wop))
    (when wpos
      (def dst (ins wpos))
      (when (>= dst arity) (put written dst true))))
  (def ct (emit-kernel-type rt))
  (def lines @[])
  (var used-externs @[])
  (each slot (sort (keys written))
    (array/push lines (string "    " (emit-kernel-type (get wtypes slot :double) ct)
                             " s" slot ";")))
  (var bad nil)
  (each start leaders
    (def st (get states start))
    (if (nil? st)
      (set bad "unreachable block")
      (do
        (unless (= start 0) (array/push lines (string "L" start ":;")))
        (var pending @[])
        (var spread false)
        (var pc start)
        (def endx (get block-end start))
        (while (and (< pc endx) (nil? bad))
          (def ins (code pc))
          (def op (ins 0))
          # keep the slot-type state current (mirrors flow-check)
          (cond
            (= op 'ldi) (put st (ins 1) :long)
            (= op 'ldc) (put st (ins 1) (infer/const-num-type d (ins 2)))
            (= op 'ldn) (put st (ins 1) :nil)
            (or (= op 'ldt) (= op 'ldf)) (put st (ins 1) :bool)
            (= op 'len) (put st (ins 1) :long)
            (= op 'movn) (put st (ins 1) (get st (ins 2) :unknown))
            (= op 'movf) (put st (ins 2) (get st (ins 1) :unknown))
            (get infer/arith-im-ops op)
            (put st (ins 1) (infer/arith-join (get st (ins 2) :unknown) :long))
            (get infer/arith-ops op)
            (put st (ins 1)
                 (infer/arith-join (get st (ins 2) :unknown)
                                   (get st (ins 3) :unknown)))
            (get infer/cmp-ops op) (put st (ins 1) :bool)
            # Step 11: array access (mirrors infer/scan-def rules)
            (= op 'get)
            (put st (ins 1)
                 (if (and (= :array (get st (ins 2) :unknown))
                          (infer/int-family? (get st (ins 3) :unknown)))
                   :number :unknown))
            (= op 'put)
            (do (def pa (get st (ins 1) :unknown))
              (when (= pa :array)
                (unless (and (infer/int-family? (get st (ins 2) :unknown))
                             (get infer/numeric-types (get st (ins 3) :unknown)))
                  (put st (ins 1) :unknown))))
            (= op 'call)
            (do (def tid (get cmap pc))
              (when tid (put st (ins 1) (rts tid))))
            nil)
          (cond
            (= op 'ldi)
            (array/push lines (string "    s" (ins 1) " = " (ins 2) ";"))
            (= op 'ldc)
            (do (def cval ((or (d :constants) @[]) (ins 2)))
              (unless (and cval (= :number (type cval)))
                (set bad "ldc non-number")
                (break))
              (array/push lines (string "    s" (ins 1) " = " (emit-c-num cval) ";")))
            (= op 'ret)
            (do (def rvt (get st (ins 1) :unknown))
              # family check: int fn returns int-family values, float fn
              # returns any numeric (int promotes); C converts the width
              (def ok (cond
                        (get int-hints rt) (get int-hints rvt)
                        (get float-hints rt)
                        (or (get infer/numeric-types rvt) (= rvt :bool))
                        nil))
              (unless ok
                (set bad (string "ret slot is " rvt " not " rt))
                (break))
              (array/push lines (string "    return " (emit-slot (ins 1) arity) ";")))
            (= op 'push) (array/push pending (emit-slot (ins 1) arity))
            (= op 'push2)
            (do (array/push pending (emit-slot (ins 1) arity))
              (array/push pending (emit-slot (ins 2) arity)))
            (= op 'push3)
            (do (array/push pending (emit-slot (ins 1) arity))
              (array/push pending (emit-slot (ins 2) arity))
              (array/push pending (emit-slot (ins 3) arity)))
            (= op 'pusha) (set bad "spread args")
            (= op 'jmp)
            (array/push lines (string "    goto L" (+ pc (ins 1)) ";"))
            (get emit-cond-jumps op)
            (do (def cnd (get st (ins 1) :unknown))
              (unless (= cnd :bool)
                (set bad (string "branch condition is " cnd " not bool"))
                (break))
              (if (= op 'jmpif)
                (array/push lines (string "    if (" (emit-slot (ins 1) arity) ") goto L" (+ pc (ins 2)) ";"))
                (array/push lines (string "    if (!" (emit-slot (ins 1) arity) ") goto L" (+ pc (ins 2)) ";"))))
            (get emit-cmp-c op)
            (if (get emit-cmp-im op)
              (array/push lines
                          (string "    s" (ins 1) " = "
                                  (emit-slot (ins 2) arity) " " (emit-cmp-c op) " "
                                  (ins 3) ";"))
              (array/push lines
                          (string "    s" (ins 1) " = "
                                  (emit-slot (ins 2) arity) " " (emit-cmp-c op) " "
                                  (emit-slot (ins 3) arity) ";")))
            (= op 'div)
            # int-family: C integer division truncates (needs a
            # documented policy in step 13); float-family: real division
            (if (get float-hints rt)
              (do (set pending @[])
                (array/push lines
                            (string "    s" (ins 1) " = "
                                    (emit-slot (ins 2) arity) " / "
                                    (emit-slot (ins 3) arity) ";")))
              (do (set bad "div on int width (policy TBD)")
                (break)))
            (= op 'movn)
            (array/push lines (string "    s" (ins 1) " = "
                                      (emit-slot (ins 2) arity) ";"))
            (= op 'movf)
            (array/push lines (string "    s" (ins 2) " = "
                                      (emit-slot (ins 1) arity) ";"))
            # Step 11: array access. janet get-OOB returns nil, which has
            # no unboxed representation — the native kernel aborts
            # instead (documented opt-in divergence). In-bounds loops of
            # the `for i 0 (length a)` shape never trip it.
            (= op 'len)
            (do (unless (= :array (get st (ins 2) :unknown))
                  (set bad "len of non-array")
                  (break))
              (array/push lines (string "    s" (ins 1) " = "
                                        (emit-slot (ins 2) arity) "->count;")))
            (= op 'get)
            (do (unless (and (= :array (get st (ins 2) :unknown))
                             (infer/int-family? (get st (ins 3) :unknown)))
                  (set bad "get requires array and int index")
                  (break))
              (array/push lines
                          (string "    if ((int64_t)(" (emit-slot (ins 3) arity)
                                  ") < 0 || (int64_t)(" (emit-slot (ins 3) arity)
                                  ") >= " (emit-slot (ins 2) arity)
                                  "->count) { fprintf(stderr, \"array index out of bounds\\n\"); abort(); }"))
              (array/push lines
                          (string "    s" (ins 1) " = janet_unwrap_number("
                                  (emit-slot (ins 2) arity) "->data[(int64_t)("
                                  (emit-slot (ins 3) arity) ")]);")))
            (= op 'put)
            # (put arr idx val): dst IS the array slot (ins 1)
            (do (unless (and (= :array (get st (ins 1) :unknown))
                             (infer/int-family? (get st (ins 2) :unknown))
                             (get infer/numeric-types (get st (ins 3) :unknown)))
                  (set bad "put requires array, int index, numeric value")
                  (break))
              (array/push lines
                          (string "    if ((int64_t)(" (emit-slot (ins 2) arity)
                                  ") < 0 || (int64_t)(" (emit-slot (ins 2) arity)
                                  ") >= " (emit-slot (ins 1) arity)
                                  "->count) { fprintf(stderr, \"array index out of bounds\\n\"); abort(); }"))
              (array/push lines
                          (string "    " (emit-slot (ins 1) arity) "->data[(int64_t)("
                                  (emit-slot (ins 2) arity) ")] = janet_wrap_number("
                                  (emit-slot (ins 3) arity) ");")))
            (or (= op 'add) (= op 'sub) (= op 'mul))
            (do (set pending @[])
              (array/push lines
                          (string "    s" (ins 1) " = "
                                  (emit-slot (ins 2) arity) " " (emit-arith-c op) " "
                                  (emit-slot (ins 3) arity) ";")))
            (get emit-arith-im-c op)
            (do (set pending @[])
              (array/push lines
                          (string "    s" (ins 1) " = "
                                  (emit-slot (ins 2) arity) " " (emit-arith-im-c op) " "
                                  (ins 3) ";")))
            (or (= op 'call) (= op 'tcall))
            (do (def tid (get cmap pc))
              (if (string? tid)
                # declared extern: direct C symbol call
                (do (def ext (get externs tid))
                  (def extret (get ext :returns))
                  (def extc (get ext :c))
                  (if (and extret extc (= extret rt) (not spread)
                           (= (length pending) (length (get ext :params))))
                    (do (array/push used-externs tid)
                      (array/push lines
                                  (string "    "
                                          (if (= op 'tcall)
                                            (string "return ")
                                            (string ct " s" (ins 1) " = "))
                                          extc "(" (string/join pending ", ") ");"))
                      (set pending @[]))
                    (do (set bad "extern signature mismatch")
                      (break))))
                (do (def cname (if tid (get emitted-names tid)))
                  (def trt (if tid (rts tid) :unknown))
                  (if (and tid cname (= trt rt) (not spread) (not= op 'tcall))
                    (do (array/push lines
                                    (string "    s" (ins 1) " = "
                                            (emit-c-name cname) "(" (string/join pending ", ") ");"))
                      (set pending @[]))
                    (if (and tid cname (= trt rt) (not spread))
                      (do (array/push lines
                                      (string "    return " (emit-c-name cname)
                                              "(" (string/join pending ", ") ");"))
                        (set pending @[]))
                      (do (set bad "call to non-native or type mismatch")
                        (break)))))))
            nil)
          (++ pc)))))
  (if bad
    [nil bad]
    [{:code (string ct " " (emit-c-name (string (or (d :name) "?"))) "("
             (string/join pdecls ", ") ") {\n" (string/join lines "\n") "\n}")
      :externs used-externs}
     nil]))

(defn emit-native-all [outdir]
  (os/mkdir outdir)
  # candidates: fully concrete params + return, non-vararg; everything
  # else is reported WITH its unproven slots (add-a-hint diagnostics)
  (defn describe-types [pt2 rt2]
    (def qs (string/join (map (fn [t] (if (= t :unknown) "?" (string t))) pt2) " "))
    (string "params [" qs "] ret " (if (= rt2 :unknown) "?" (string rt2))))
  (def cand @{})
  (def skips @[])
  (each id (range (length defs))
    (def info (get defs id))
    (def d (info :d))
    (def pt (get pts id))
    (def rt (rts id))
    (var concrete (and (> (length pt) 0) (not (d :vararg))))
    # Step 11: :array params are opaque-but-emittable (pointer temps);
    # returns stay numeric-only
    (each t pt (unless (or (get infer/numeric-types t) (= t :array)) (set concrete false)))
    (unless (get infer/numeric-types rt) (set concrete false))
    # a def named like a declared extern is the compiled fallback
    # artifact — the real implementation lives in C
    (when (get externs (string (or (d :name) "?"))) (set concrete false))
    (if concrete
      (put cand id true)
      (array/push skips [id (string (or (d :name) "?"))
                          (string "types not proven: " (describe-types pt rt))])))
  # two-round dependency resolution: leaves first, then callers whose
  # targets are all emitted
  (def emitted-names @{})  # id -> janet name
  (def emitted @[])
  (def skip-reason @{})
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
        (def [res reason] (emit-fn-c id d (rts id) (get pts id) call-map emitted-names rts))
        (if res
          (do (def nm (string (d :name)))
            (put emitted-names id nm)
            (array/push emitted {:id id :name nm :code (res :code)
                                 :externs (res :externs)
                                 :pt (get pts id) :rt (rts id)})
            (set changed true))
          (put skip-reason id reason)))))
  (each id cand-ids
    (unless (get emitted-names id)
      (def d ((get defs id) :d))
      (array/push skips [id (string (or (d :name) "?"))
                          (or (get skip-reason id) "calls non-emitted native")])))
  (each [id nm r] skips (print "native-skip " id " " nm " " r))
  (when (empty? emitted)
    (eprint "emit-native: no emittable functions")
    (os/exit 1))
  (defn samples-for [pt2]
    # Step 11: kernels with array params sample ints from [0 1] — the
    # fixed variants have length >= 2 and indices must stay in-bounds
    # (OOB aborts by design, and nil has no oracle-comparable form).
    (def arrk (some (fn [t] (= t :array)) pt2))
    (def per (map (fn [t] (cond
                            (= t :array) @[0 1]
                            (get int-hints t) (if arrk @[0 1] @[-3 -1 0 1 3])
                            (get float-hints t) @[-2.5 0.5 1.5 2.5]
                            @[-2.5 0.5 1.5 2.5])) pt2))
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
  # bind extern fallbacks so eval'd defn forms can call them for parity
  (each name (keys externs)
    (def fb (get (get externs name) :fallback))
    # env entries are binding descriptors, not raw values
    (when fb (put (curenv) (symbol name) @{:value fb})))
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
  (buffer/push-string cout "#include <stdio.h>\n#include <inttypes.h>\n")
  # Step 11: array kernels need Janet layouts + wrap macros (header
  # only — no lib link) and abort() for the bounds guard.
  (var anyarr false)
  (each e emitted
    (each t (e :pt) (when (= t :array) (set anyarr true))))
  (when anyarr
    (buffer/push-string cout "#include <janet.h>\n#include <stdlib.h>\n"))
  (buffer/push-string cout "\n")
  # extern C declarations used by the emitted natives (raylib story)
  (var anyext false)
  (each e emitted
    (each en (e :externs)
      (def ext (get externs en))
      (buffer/push-string cout
        (string "extern " (emit-c-type (get ext :returns)) " "
                (get ext :c) "("
                (string/join (map emit-c-type (get ext :params)) ", ") ");\n"))
      (set anyext true)))
  (when anyext (buffer/push-string cout "\n"))
  (each c c-parts (buffer/push-string cout c) (buffer/push-string cout "\n\n"))
  (buffer/push-string cout "int main(void) {\n")
  # Step 11: fixed sample arrays, one builder pair per array param.
  # Variant 0 = [1.5 2.5 3.5], variant 1 = [4 5]; the oracle below
  # uses the same values.
  (when anyarr
    (each e emitted
      (each i (range (length (e :pt)))
        (when (= :array ((e :pt) i))
          (def an (string "arr_" (emit-c-name (e :name)) "_" i))
          (buffer/push-string cout
            (string "    Janet " an "_v0_elts[3];\n"
                    "    " an "_v0_elts[0] = janet_wrap_number(1.5);\n"
                    "    " an "_v0_elts[1] = janet_wrap_number(2.5);\n"
                    "    " an "_v0_elts[2] = janet_wrap_number(3.5);\n"
                    "    JanetArray " an "_v0 = {{0}, 3, 3, " an "_v0_elts};\n"
                    "    Janet " an "_v1_elts[2];\n"
                    "    " an "_v1_elts[0] = janet_wrap_number(4);\n"
                    "    " an "_v1_elts[1] = janet_wrap_number(5);\n"
                    "    JanetArray " an "_v1 = {{0}, 2, 2, " an "_v1_elts};\n"))))))
  (each c calls
    (def ept ((c :e) :pt))
    (def args-c (string/join
                  (seq [i :range [0 (length (c :args))]]
                    (if (= :array (ept i))
                      (string "&arr_" (emit-c-name ((c :e) :name))
                                      "_" i "_v" ((c :args) i))
                      (string ((c :args) i))))
                  ", "))
    (buffer/push-string cout
      (string "    printf(\"%.17g\\n\", (double) " (emit-c-name ((c :e) :name))
              "(" args-c "));\n")))
  (buffer/push-string cout "    return 0;\n}\n")
  (spit (string outdir "/native.c") cout)
  (def expected @[])
  (def arr-variants [@[1.5 2.5 3.5] @[4 5]])
  (each c calls
    (def ept ((c :e) :pt))
    (def coerced (seq [i :range [0 (length (c :args))]]
                   (if (= :array (ept i))
                     (get arr-variants ((c :args) i))
                     ((c :args) i))))
    (def v ((c :fn) ;coerced))
    (array/push expected (string/format "%.17g" v)))
  (spit (string outdir "/expected.txt") (string (string/join expected "\n") "\n"))
  (print "native " (length emitted) " fns " (length calls) " samples " outdir))

(defn wrapper-for [nm pt rt]
  (def cn (emit-c-name nm))
  # boundary casts: every int width unboxes/wraps as integer (C
  # implicit conversion narrows/widens to the kernel's exact width),
  # float widths as number, arrays pass through as pointers
  (def ps (string/join (seq [i :range [0 (length pt)]]
                     (def t (pt i))
                     (cond (get int-hints t)
                           (string "janet_unwrap_integer(argv[" i "])")
                           (= t :array)
                           (string "(JanetArray *)janet_unwrap_array(argv[" i "])")
                           (string "janet_unwrap_number(argv[" i "])")))
                  ", "))
  (def rv (if (and (get int-hints rt) (not= int-policy :promote))
              "janet_wrap_integer"
              "janet_wrap_number"))
  (string "static Janet " cn "_wrapper(int32_t argc, Janet *argv) {\n"
          "    (void) argc;\n"
          "    return " rv "(" cn "(" ps "));\n}\n"))

(when emit-native-dir (emit-native-all emit-native-dir))

(defn emit-native-lib [outdir]
  # Step 12: hybrid-binary flavor. Emits native.c with the kernels PLUS
  # janet-cfunction wrappers (unbox args -> kernel -> rebox result) and a
  # native_init(JanetTable*) that registers the wrappers into the runtime
  # env. The host app calls native_init(env) after janet_core_env and
  # before running/compiling any janet code — env-resolved call sites
  # (notably eval'd code) then dispatch straight to the natives.
  (os/mkdir outdir)
  # candidates + two-round dependency resolution (same discipline as
  # emit-native-all): leaves first, then callers of emitted natives
  (def cand @{})
  (each id (range (length defs))
    (def info (get defs id))
    (def d (info :d))
    (def pt (get pts id))
    (def rt (rts id))
    (var concrete (and (> (length pt) 0) (not (d :vararg))))
    # Step 11: :array params are opaque-but-emittable (pointer temps);
    # returns stay numeric-only
    (each t pt (unless (or (get infer/numeric-types t) (= t :array)) (set concrete false)))
    (unless (get infer/numeric-types rt) (set concrete false))
    (when concrete (put cand id true)))
  (def emitted-names @{})
  (def kernels @[])
  (var round 0)
  (var changed true)
  (while (and changed (< round 4))
    (++ round)
    (set changed false)
    (each id (filter (fn [k] (number? k)) (sort (keys cand)))
      (unless (get emitted-names id)
        (def info (get defs id))
        (def d (info :d))
        (def [res reason] (emit-fn-c id d (rts id) (get pts id) call-map emitted-names rts))
        (when res
          (def nm (string (d :name)))
          (put emitted-names id nm)
          (array/push kernels {:name nm :code (res :code)
                               :externs (res :externs)
                               :wrapper (wrapper-for nm (get pts id) (rts id))})
          (set changed true)))))
  (def cout @"")
  (buffer/push-string cout "#include <janet.h>\n#include <inttypes.h>\n\n")
  # extern C declarations used by the emitted kernels (raylib story)
  (each k kernels
    (each en (k :externs)
      (def ext (get externs en))
      (buffer/push-string cout
        (string "extern " (emit-c-type (get ext :returns)) " "
                (get ext :c) "("
                (string/join (map emit-c-type (get ext :params)) ", ") ");\n"))))
  (buffer/push-string cout "\n")
  (each k kernels
    (buffer/push-string cout (k :code))
    (buffer/push-string cout "\n")
    (buffer/push-string cout (k :wrapper))
    (buffer/push-string cout "\n"))
  (buffer/push-string cout "void native_init(JanetTable *env) {\n")
  (buffer/push-string cout "    static const JanetReg regs[] = {\n")
  (each k kernels
    (buffer/push-string cout
      (string "    { \"" (k :name) "\", " (emit-c-name (k :name)) "_wrapper, NULL },\n")))
  (buffer/push-string cout "    { NULL, NULL, NULL } };\n")
  (buffer/push-string cout "    janet_cfuns(env, \"native\", regs);\n")
  (buffer/push-string cout "    janet_cfuns(env, \"native\", regs);\n")  (buffer/push-string cout "    fprintf(stderr, \"native_init: registered %d wrappers\\n\", (int) (sizeof(regs)/sizeof(regs[0]) - 1));\n")
  (buffer/push-string cout "}\n")
  (spit (string outdir "/native.c") cout)
  (def nl @"")
  (each k kernels (buffer/push-string nl (k :name)) (buffer/push-string nl "\n"))
  (spit (string outdir "/native-names.txt") nl)
  (print "native-lib " (length kernels) " kernels " outdir))

(when emit-native-lib-dir (emit-native-lib emit-native-lib-dir))