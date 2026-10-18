# tools/callgraph.janet — Phase 4 steps 1+2 (registration + fixpoint).
# Extract the whole-program FuncDef graph from Janet source WITHOUT running
# it: parse -> compile (macros run at compile time, as today) -> disasm ->
# walk. Prints node/edge lines, a stats summary, and a fixpoint block
# (reachability from entry thunks, caller sets, recursion, dynamic taint).
# Read-only; no src/ changes. Usage:
# ./build/bender tools/callgraph.janet FILE [FILE...]
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
# B7.2 Part B, Piece 1c: --dce opts in to WRITING the dce-reachable
# verdict out as dce-excluded.txt (next to native-names.txt) for
# mkimage.janet to consume. Without it the gate at the end of this
# file still runs and still prints its diagnostic verdict (Piece 1b
# behavior, byte-identical), but nothing is written and no build can
# strip anything. The flag only enables EMISSION; it never relaxes
# the gate.
(var dce-emit false)
(var externs-file nil)
(var profile-file nil)
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
    (= a "--dce")
    (set dce-emit true)
    (and (= a "--externs") (< (+ i 1) (length args)))
    (do (set externs-file (args (+ i 1))) (++ i))
    (and (= a "--profile") (< (+ i 1) (length args)))
    (do (set profile-file (args (+ i 1))) (++ i))
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
(var core-call-map @{})   # id -> {pc -> core cfunction name} (pure-call shaping)
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
# Sound real-division needs C-double operands: C int-division
# truncates toward zero where boxed divides real (floor_div(-7,2) is
# -4 boxed but -3 in C on int64). :number temps declare as double, so
# they are fine; int-family or bool operands must refuse. Applies to
# div/divf/mod/rem (and divim's single operand below).
(defn float-operand-ok [st slot]
  (def t (get st slot :unknown))
  (or (get float-hints t) (= t :number)))
# PGO-nonneg int operand: unanimous observed min >= 0 on an unrebound
# param slot that is int-family now. Rebinding kills the evidence
# (pwritten); monotonic-counter reasoning is future work.
(defn nonneg-operand? [nn-vec pw slot st]
  (and (< slot (length nn-vec))
       (get nn-vec slot false)
       (nil? (get pw slot))
       (get int-hints (get st slot :unknown))))
(def emit-c-type-map
  @{:long "int64_t" :i32 "int32_t" :i16 "int16_t" :i8 "int8_t"
    :u64 "uint64_t" :u32 "uint32_t" :u16 "uint16_t" :u8 "uint8_t"
    :double "double" :float "float" :number "double"
    :array "JanetArray*" :tuple "JanetTuple" :pointer "void*"
    :bool "int"
    # Phase 1 (type-generator plan): a function proven to ALWAYS
    # return nil (every 'ret/'retn site) is a legitimate void kernel --
    # no boxed value is ever produced, so there is nothing to unbox.
    :nil "void"
    # Phase 2: JanetString is already `typedef const uint8_t *` with a
    # length-prefixed JanetStringHead immediately before the pointer
    # (janet.h:497,585) -- already a flat C value, no transformation
    # needed, only this type-map entry plus the kernel ops below.
    :string "JanetString"
    # Phase 3: JanetKeyword and JanetSymbol are also already
    # `typedef const uint8_t *` (janet.h:586-587) -- same flat C value
    # shape as :string, and additionally INTERNED (src/core/symcache.c,
    # `#define janet_keyword janet_symbol`), so equal keywords/symbols
    # are pointer-identical: raw C `==`/`!=` (the existing 'eq/'neq
    # path) is already correct and NO content-equality kernel is
    # needed, unlike Phase 2's non-interned strings.
    :keyword "JanetKeyword"
    :symbol "JanetSymbol"})

# Phase 2/3: atom (string/keyword/symbol) literal constants used
# inside a kernel body (e.g. a `case`/`=` dispatch against "linear" /
# :a / 'foo) can never be emitted as a bare C string literal cast to
# the Janet pointer type -- each needs a real JanetStringHead
# (strings) or real interning (keywords/symbols), which only the
# runtime constructors `janet_string()` / `janet_keyword()` /
# `janet_symbol()` perform. Each distinct literal gets ONE static
# slot, materialized once in native_init (before any wrapper can be
# called), and every 'ldc of that same literal across every kernel in
# this build references that slot -- deduplicated by [tag bytes], so
# identical bytes appearing as BOTH a string and a keyword (or
# symbol) stay two distinct Janet values in two distinct slots. Reset
# per build (module-global state across one compile run, same pattern
# as kernel-intrinsics/unit-import-files elsewhere in this file).
(var atom-const-ids @{})
(var atom-const-order @[])
(defn atom-const? [ty]
  (or (= ty :string) (= ty :keyword) (= ty :symbol)))
(defn atom-const-name [tag bytes]
  (def key [tag bytes])
  (or (get atom-const-ids key)
      (do (def nm (string (case tag
                            :string "k_str_"
                            :keyword "k_kw_"
                            "k_sym_")
                          (length atom-const-order)))
        (put atom-const-ids key nm)
        (array/push atom-const-order key)
        nm)))
(defn- cmp-kind-class [t]
  # value-rep vs pointer-rep operand classes for eq/neq emission
  (cond (or (get int-hints t) (get float-hints t) (= t :number) (= t :bool)) :val
        (or (= t :string) (= t :keyword) (= t :symbol) (= t :array)
            (= t :tuple) (= t :pointer)) :ptr
        :other))
(defn- mixed-class-cmp? [a b]
  # both operands concretely classed and in DIFFERENT classes: a value
  # rep can never equal a pointer rep (janet_equals checks type first),
  # so the comparison result is a compile-time constant. This is what
  # makes a flip slot comparable inside a number-branch kernel whose
  # other operand is a keyword param (and vice versa).
  (and (not= :other (cmp-kind-class a)) (not= :other (cmp-kind-class b))
       (not= (cmp-kind-class a) (cmp-kind-class b))))

(def atom-const-ctors
  @{:string "janet_string" :keyword "janet_keyword" :symbol "janet_symbol"})
(defn emit-c-string-literal [bytes]
  # C string literal body only -- caller supplies the surrounding
  # quotes. Escapes everything outside printable, non-quote, non-
  # backslash ASCII as \xHH so arbitrary (including multi-byte UTF-8
  # and non-UTF-8) Janet string bytes survive verbatim; \xHH is always
  # followed by a literal "" "" string-literal break so a subsequent
  # hex digit in the source bytes can never extend the escape (C's
  # \x consumes as many hex digits as follow it).
  (string/join
    (map (fn [b]
           (cond
             (= b 34) "\\\""
             (= b 92) "\\\\"
             (and (>= b 32) (< b 127)) (string/from-bytes b)
             (string "\\x" (string/format "%02x" b) "\"\"")))
         bytes)
    ""))
# alias spellings fold to the canonical width keyword
(defn canon-hint [k] (get hint-canonical k k))
(defn hint-of [id]
  (def info (get defs id))
  (when (nil? info) (break nil))
  (info :hint))
(var all-defn-forms @{})
# Path 2 struct intrinsics: callee name -> [:pread|:pwrite ctype-kw offset].
# Populated from defn :kernel metadata; calls to these defs emit direct
# native-memory access instead of calls (the callee itself is never
# emitted -- its boxed body serves the interpreter).
(var kernel-intrinsics @{})
# file -> macroexpanded top-level forms (macro-generating-defns, notably
# ffi/defstruct, must be visible to registration and the source scans)
(var unit-forms @{})
(var unit-import-files @[])
(var analyzed-files @{})
(var unit-envs @{})
(defn extract-intrinsics []
  # Path 2 struct intrinsics, second pass over the same unit files:
  # (defn f {:hint {...} :kernel {:pread [:double OFF]}} ...) marks
  # calls to f as direct native-memory access instead of calls (the
  # callee itself is never emitted -- its boxed body serves the
  # interpreter). Validated here; invalid specs are ignored (the
  # normal call path then refuses loudly -- fail closed).
  (var n 0)
  (each f (array/concat @[] args unit-import-files)
    (each form (or (get unit-forms f) @[])
      (when (and (tuple? form) (>= (length form) 3)
                 (symbol? (form 0))
                 (or (= (string (form 0)) "defn") (= (string (form 0)) "defn-")))
        (def m (form 2))
        (when (and (or (table? m) (struct? m)) (get m :kernel))
          (def kspec (get m :kernel))
          (def kk (cond (get kspec :pread)
                        [:pread (get kspec :pread)]
                        (get kspec :pwrite)
                        [:pwrite (get kspec :pwrite)]))
          (when kk
            (def kv (get kk 1))
            (when (and (indexed? kv) (= 2 (length kv))
                       (or (= (get kv 0) :double)
                           (= (get kv 0) :long)
                           (= (get kv 0) :float)
                           (= (get kv 0) :i32))
                       (int? (get kv 1)) (>= (get kv 1) 0))
              (put kernel-intrinsics (string (form 1))
                   [(get kk 0) (get kv 0) (get kv 1)])
              (++ n)))))))
  n)

(defn extract-hints []
  (var nhints 0)
  (each f (array/concat @[] args unit-import-files)
    (each form (or (get unit-forms f) @[])
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
            (++ nhints)))
)))
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
                  (= (ct 0) :core)
                  (do (array/push edges ["extern" caller-id (ct 1)])
                    # record core-cfunction call sites for pure-call
                    # shaping (math/floor etc.): name keyed by pc, like
                    # extern-call-map but without --externs declarations.
                    (put (or (get core-call-map caller-id)
                             (do (def t2 @{}) (put core-call-map caller-id t2) t2))
                         pc (ct 1)))
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
    'geti true 'add true 'sub true 'mul true 'div true 'divf true
    'mod true 'rem true 'divim true 'addim true
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
            # L2: remember resolved source files so analysis covers
            # engine modules, not just the entry (kernel visibility).
            (try
              (do (def found (module/find path))
                (when (and (indexed? found)
                           (string? (found 0))
                           (string/has-suffix? ".janet" (found 0))
                           (nil? (index-of (found 0) unit-import-files)))
                  (array/push unit-import-files (found 0))))
              ([_] nil))
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

(defn analyze-unit [f]
  (when (get analyzed-files f) (break))
  (put analyzed-files f true)
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
  # expand top-level macro calls (notably ffi/defstruct) so
  # macro-generated defns register like hand-written ones. defn/defmacro
  # heads are NEVER expanded: macex would dissolve the defn shape the
  # source scans pattern-match. Unexpandable forms stay as they were
  # (yesterday's behavior, fail-open to no worse).
  (def noexpand-heads @{"defn" true "defn-" true "defmacro" true})
  # A macro call expanding to a (do ...) splices: defns nested in a
  # do-block never register as top-level defs (closures inline), so the
  # block's body joins the stream as siblings. User-written do-blocks
  # (head already do) stay intact.
  (def expanded @[])
  (each form forms
    (def res
      (if (and (tuple? form) (>= (length form) 1) (symbol? (form 0))
               (not (get noexpand-heads (string (form 0))))
               (not= (form 0) 'do))
        (try (with-env env (macex1 form)) ([_] form))
        form))
    (if (and (not= res form) (tuple? res) (>= (length res) 1)
             (= (get res 0) 'do))
      (each x (tuple/slice res 1) (array/push expanded x))
      (array/push expanded res)))
  (put unit-forms f expanded)
  (put unit-envs f env)
  (def program (tuple/slice @['do ;expanded] 0))
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

# entry files first, then L2: drain transitively discovered import
# source files (engine modules become analyzable: registration,
# hints, PGO, emission).
(each f args (analyze-unit f))
(var import-queue-idx 0)
(while (< import-queue-idx (length unit-import-files))
  (def qf (unit-import-files import-queue-idx))
  (++ import-queue-idx)
  (analyze-unit qf))

# Step 4: extract type hints BEFORE the walk so audit-site sees them.
(var nhints (extract-hints))
(var nintrinsics (extract-intrinsics))

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

# PGO phase 2: profile-seeded widening. --profile FILE loads a
# debug/profile-dump output; unanimous observations seed unhinted
# params (never pfixed -- conflicts widen soundly as usual). Matching
# is by defn name; same-name rows from different sources merge
# conservatively (a slot seeds only if unanimous across all of them).
# Never evaluate a profile: it is cache/user-controlled Janet data. Reject
# trailing forms and malformed evidence before it can participate in seeding.
(defn- profile-invalid [reason]
  (error (string "invalid PGO profile: " reason)))

(defn- profile-sequence [value field]
  (unless (indexed? value) (profile-invalid (string field " must be indexed")))
  value)

(defn- profile-types [value field]
  (when value
    (each kind (profile-sequence value field)
      (unless (keyword? kind) (profile-invalid (string field " has non-keyword type")))))
  value)

(defn- profile-origins [value field]
  (def origins (if (indexed? value) value [value]))
  (each origin origins
    (unless (keyword? origin) (profile-invalid (string field " has non-keyword origin"))))
  value)

(defn- profile-bound [value]
  (unless (or (nil? value)
              (and (number? value) (= value value))
              (= value "inf") (= value "-inf"))
    (profile-invalid "invalid range bound"))
  value)

(defn- profile-rows [path]
  (def text (slurp path))
  (unless text (profile-invalid "cannot read profile"))
  (def forms (parse-all text))
  (unless (= 1 (length forms)) (profile-invalid "expected exactly one data form"))
  (def rows (forms 0))
  (profile-sequence rows "rows")
  (each row rows
    (unless (and (indexed? row) (= 2 (length row)))
      (profile-invalid "row must be [identity info]"))
    (def id (row 0))
    (def info (row 1))
    (unless (and (indexed? id) (= 3 (length id))
                 (or (nil? (id 0)) (string? (id 0)))
                 (number? (id 1)) (= (id 1) (math/trunc (id 1)))
                 (or (nil? (id 2)) (string? (id 2)))
                 (table? info))
      (profile-invalid "invalid row identity or info"))
    (def calls (get info :calls))
    (unless (and (number? calls) (>= calls 0) (= calls (math/trunc calls)))
      (profile-invalid "invalid call count"))
    (each field [:params :elems]
      (def slots (profile-sequence (get info field @[]) field))
      (each slot slots (profile-types slot field)))
    (profile-types (get info :rets @[]) :rets)
    (each field [:mins :maxs]
      (each value (profile-sequence (get info field @[]) field)
        (profile-bound value)))
    (profile-origins (get info :from :home) :from)
    (def by-slot (profile-sequence (get info :type-from @[]) :type-from))
    (each mapping by-slot
      (unless (table? mapping) (profile-invalid "invalid type provenance slot"))
      (each [kind origins] (pairs mapping)
        (unless (keyword? kind) (profile-invalid "invalid provenance type"))
        (profile-origins origins :type-from))))
  rows)

(var profile-index nil)
(when profile-file
  (def rows (profile-rows profile-file))
  # Non-finite range bounds arrive as tagged strings ("inf"/"-inf"):
  # the C dumper cannot emit bare inf (parse reads it back as a symbol
  # and aborted every profile re-load). Map them to float infinities;
  # NaN slots were already serialized as nil sentinels.
  (defn prof-bound [x]
    (cond
      (string? x) (case x "inf" math/inf "-inf" (- math/inf) nil)
      (number? x) x
      nil))
  (set profile-index @{})
  (each row rows
    (def id (row 0))
    (def info (row 1))
    (def nm (if (>= (length id) 3) (string (id 2)) nil))
    (when nm
      (def bucket (or (get profile-index nm) @[]))
      (array/push bucket {:params (get info :params @[])
                          :mins (map prof-bound (get info :mins @[]))
                          :maxs (map prof-bound (get info :maxs @[]))
                          :calls (get info :calls 0)
                          :from (get info :from :home)
                          :type-from (get info :type-from @[])})
      (put profile-index nm bucket)))
  (print "profile " (length rows) " rows"))

# Step 6+ (hybrid inference): hints are direct contracts (used as-is for
# unboxing, never weakened); call-site widening + fixpoint prove the
# unhinted rest. Concrete => unboxed, unproven => boxed.
(def res (infer/analyze defs call-map profile-index core-call-map))
(def rts (res :rts))
(def nnp (res :nnp))
(def pts (res :pts))
# Phase 5 (type-generator plan): closed-shape local tables. infer/analyze
# detects, per function, which mktab-constructed table slots have a
# CLOSED shape (fixed literal keyword keys, :long/:double fields, single
# entry-block construction, constant-key-only get/put, zero escapes --
# full decision rule in tools/infer.janet's block comment). tshapes maps
# id -> {slot -> shape}; taccs maps id -> {pc -> validated pread/pwrite};
# topened reports sticky-opened slots (every refusal is a correct boxed
# fallback, never a silent miscompile).
(def tshapes (or (res :tshapes) @{}))
(def taccs (or (res :taccs) @{}))
(def topened (or (res :topened) @{}))
(each id (range (length defs))
  (def nm5 (string (or ((get defs id) :name) "?")))
  (eachp [slot shp] (or (get tshapes id) @{})
    (print "table-flat " id " " nm5 " slot " slot " "
           (string/join (map (fn [k] (string k ":" ((shp :fields) k)))
                             (shp :order)) ",")))
  (eachp [slot opc] (or (get topened id) @{})
    (print "table-open " id " " nm5 " slot " slot " at pc " opc)))
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
# Phase 5 flat-struct emission helpers. A shaped table slot is declared
# as a LOCAL C struct (valid C, defined once per kernel body, deduped by
# name): struct nsh<id>_<base-slot> { <ctype> f0; ... }; fields are
# positional in construction order; access reuses the same direct-member
# memory-access discipline as the :kernel pread/pwrite intrinsics (this
# is the auto-discovered sibling of the hand-declared ffi/defstruct
# path -- same field contract :long/:double, same fail-closed guards).
(defn nshape-name [id shp] (string "nsh" id "_" (shp :base)))
(defn nshape-var [shp] (string "nshv" (shp :base)))
(defn nshape-ctype [t] (if (= t :long) "int64_t" "double"))

(defn- operand-slots-of [op ins]
  # slots READ by op (dst excluded), mirroring infer/shape-reads.
  # MUST cover every reader family: a missed read makes the forward
  # scan in dead-key-ldc? run off the end and wrongly declare a LIVE
  # keyword load dead (a real miscompile, caught by probe).
  (cond
    (= op 'len) @[(ins 2)]
    (= op 'get) @[(ins 2) (ins 3)]
    (= op 'put) @[(ins 2) (ins 3)]
    (= op 'push) @[(ins 1)]
    (= op 'push2) @[(ins 1) (ins 2)]
    (= op 'push3) @[(ins 1) (ins 2) (ins 3)]
    (= op 'pusha) @[(ins 1)]
    (or (= op 'jmpif) (= op 'jmpno) (= op 'jmpni) (= op 'jmpnn)
        (= op 'ret)) @[(ins 1)]
    (= op 'call) @[(ins 2)]
    (= op 'tcall) @[(ins 1)]
    (= op 'movn) @[(ins 2)]
    (= op 'movf) @[(ins 1)]
    (or (get infer/arith-ops op) (get infer/bit-ops op)
        (get infer/cmp-ops op) (get infer/div-ops op)) @[(ins 2) (ins 3)]
    (or (get infer/arith-im-ops op) (get infer/bit-im-ops op)) @[(ins 2)]
    # pure writers / control transfer read nothing
    (or (= op 'ldi) (= op 'ldc) (= op 'ldn) (= op 'ldt) (= op 'ldf)
        (= op 'ldu) (= op 'lds) (= op 'jmp)
        (= op 'mkarr) (= op 'mktup) (= op 'mkbtp) (= op 'mkbuf)
        (= op 'mkstr) (= op 'mkstu) (= op 'mktab)) @[]
    # puti reads its value operand; any other unknown op is assumed to
    # read every common operand slot (fail-closed; tuples vary in
    # length so use safe gets)
    (= op 'puti) @[(get ins 2)]
    (filter |(not (nil? $0)) @[(get ins 1) (get ins 2) (get ins 3)])))

(defn- push-slots-of [op ins]
  (case op 'push @[(ins 1)] 'push2 @[(ins 1) (ins 2)] 'push3 @[(ins 1) (ins 2) (ins 3)] nil))

(defn dead-key-ldc? [id d pc slot kw]
  # Phase 5: a keyword-constant 'ldc feeding ONLY a closed-shape key
  # position (an mktab construction key, a pread call's key argument,
  # or a pwrite put's key slot) is DEAD in emitted code -- field
  # indices resolve statically -- so both its C assignment and its
  # declaration-type evidence are skipped. This is what keeps the
  # Phase 2/3 slot-mixing guard from refusing every table literal:
  # the compiler routes :kw keys through temp slots it later reuses
  # for numerics. Fail-closed forward scan from pc+1:
  #   - an instruction that neither reads nor writes slot: continue;
  #   - a WRITE of slot with no read of it first: dead live range ->
  #     skippable (the loaded key is never consumed as a value);
  #   - a push collects args; at the run's consumer, slot must sit at
  #     a dead-key position (mktab: even index AND a shape exists;
  #     call/tcall with a validated pread: index 0) -- else not dead;
  #   - any other READ of slot: not dead.
  # The direct put-key form ((ldc S kw) then (put T S V)) is dead iff
  # the put at pc+1 is a validated pwrite and S is its key slot.
  (def code (or (get d :bytecode) @[]))
  (def n (length code))
  # the put-key form ((ldc S kw) ... (put T S V), value ldc between)
  # is handled inside the main loop below.
  (var q (+ pc 1))
  (var args @[])
  (var res nil)
  (while (< q n)
    (def ins (code q))
    (def op (ins 0))
    (def rd (operand-slots-of op ins))
    (def psl (push-slots-of op ins))
    (cond
      # put-key form: slot read as a validated pwrite's key -> dead
      (and (= op 'put) (= slot (ins 2)))
      (do
        (def ta (get (get taccs id) q))
        (set res (and ta (= :pwrite (ta :kind))))
        (break))
      psl
      # extend the pending arg run
      (array/push args ;psl)
      (and rd (find |(= $ slot) rd))
      # a live read outside a dead-key position
      (do (set res false) (break))
      # consumer of the push run (only real consumers decide; other
      # non-push instructions between pushes keep the run open)
      (and (> (length args) 0) (nil? psl)
           (or (= op 'mktab) (= op 'call) (= op 'tcall)))
      (do
        (def pos (find-index |(= $ slot) args))
        (set res
             (cond
               (nil? pos)
               # slot was not pushed by this run at all -- its load is
               # dead IF this consumer did not read it either (rd was
               # checked above); keep scanning conservatively
               nil
               # NOTE: args collected here are only the SUFFIX of the
               # consumer's arg list (pushes before our ldc are not
               # visible to a forward scan), so index parity is not
               # decidable -- classify by the SHAPE instead: the key-
               # word constant is dead iff it is one of the shape's
               # keys (a keyword can only sit at a key position here;
               # keyword VALUES are excluded by the shape rule itself)
               (= op 'mktab)
               (do (def mshp (get (get tshapes id) (ins 1)))
                 (and mshp (get (mshp :fields) kw)))
               (or (= op 'call) (= op 'tcall))
               (do (def ta (get (get taccs id) q))
                 (and ta (= :pread (ta :kind)) (= 0 pos)))
               false))
        (when (not= res nil) (break))
        # unresolved (slot not in this run): keep scanning
        (set args @[]))
      # a write of slot with no read: dead live range
      (and (get infer/shape-dst-ops op)
           (= slot (ins (get infer/shape-dst-ops op))))
      (do (set res true) (break))
      nil)
    (++ q))
  (if (not= res nil) res
    # ran off the end with the load never consumed as a live value
    true))

(def cnts @{:long 0 :double 0 :number 0})
(each id (range (length defs))
  (def t (rts id))
  (when (get cnts t) (put cnts t (+ 1 (get cnts t)))))
(print "rets " (get cnts :long) " " (get cnts :double) " " (get cnts :number) " rounds " (res :rounds))

# Phase C: profile provenance report. pprov (from infer) maps def id ->
# per-slot seed/conflict records with home/worker origins; prints are
# sorted (ids and slots iterate in order, origins pre-sorted).
(when profile-index
  (def pprov (or (res :pprov) @{}))
  (each id (range (length defs))
    (def nm (string (or ((get defs id) :name) "")))
    (def slots (get pprov id))
    (when slots
      (var si 0)
      (each s slots
        (when (table? s)
          (if (def seed (get s :seed))
            (print "profile-seed " nm " " si " " seed " "
                   (string/join (map string (get s :from [])) ","))
            (when (def cmap (get s :conflict))
              (def parts @[])
              (each [t fs] (pairs cmap)
                (array/push parts (string t ":" (string/join (map string fs) ","))))
              (print "profile-conflict " nm " " si " "
                     (string/join (sort parts) " ")))))
        (++ si)))))

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
    'divf true 'mod true 'rem true 'divim true
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
# Bitwise kernels call helpers (int32-domain, wraparound, masked counts;
# see k_*32 definitions hoisted into each prelude).
(def emit-bit-c @{'band "k_band32" 'bor "k_bor32" 'bxor "k_bxor32"
                   'sl "k_sl32" 'sr "k_sr32" 'sru "k_sru32"})
(def emit-bit-im-c @{'slim "k_sl32" 'srim "k_sr32" 'sruim "k_sru32"})
(def emit-cmp-c @{'lt "<" 'ltim "<" 'gt ">" 'gtim ">"
                  'lte "<=" 'gte ">="
                  'eq "==" 'eqim "==" 'neq "!=" 'neqim "!="})
# dest slot operand position per writing op (params excluded from decls)
(def emit-writers
  @{'ldi 1 'ldc 1 'add 1 'sub 1 'mul 1 'div 1 'divf 1 'mod 1 'rem 1
    'divim 1
    'addim 1 'subim 1 'mulim 1
    'band 1 'bor 1 'bxor 1 'sl 1 'sr 1 'sru 1 'slim 1 'srim 1 'sruim 1
    'lt 1 'ltim 1 'gt 1 'gtim 1 'lte 1 'gte 1
    'eq 1 'eqim 1 'neq 1 'neqim 1 'call 1 'movn 1 'movf 2
    'len 1 'get 1 'put 1 'puti 1})
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
(def emit-c-reserved
  # Defining these as external C symbols is UB (C11 7.31 reserved +
  # keywords): the compiler folds calls like fmod(-2.5, 1.5) with builtin
  # semantics instead of calling our kernel (observed: -1 instead of
  # 0.5). Colliding Janet names get a k_ prefix; everything else keeps
  # its stable spelling.
  @{"fmod" true "modf" true "floor" true "ceil" true "round" true
    "trunc" true "fdiv" true "div" true "rem" true "frem" true
    "sin" true "cos" true "tan" true "exp" true "log" true "pow" true
    "sqrt" true "fabs" true "fma" true "fmax" true "fmin" true
    "abs" true "labs" true "main" true "malloc" true "free" true
    "abort" true "exit" true "printf" true "auto" true "break" true
    "case" true "char" true "const" true "continue" true "default" true
    "do" true "double" true "else" true "enum" true "extern" true
    "float" true "for" true "goto" true "if" true "int" true "long" true
    "return" true "short" true "signed" true "sizeof" true "static" true
    "struct" true "switch" true "typedef" true "union" true "unsigned" true
    "void" true "while" true})
(defn emit-c-name [nm]
  # Janet name characters with no C equivalent fold deterministically
  # (! mutation convention, ? predicate convention, / module paths,
  # + * < > = arithmetic/comparison spellings). Character-mapped, not
  # PEG-replaced: ? and ! are PEG-special in string/replace-all. Any
  # other non-identifier byte hex-escapes (_xHH), so symbols like
  # widen+2 or *mul* cannot leak raw C into the kernel source.
  (def base (string/replace-all "-" "_" nm))
  (def folded (string/join
                (map (fn [c] (cond (= c 33) "_bang"
                                  (= c 63) "_q"
                                  (= c 47) "_"
                                  (= c 43) "_plus"
                                  (= c 42) "_star"
                                  (= c 60) "_lt"
                                  (= c 62) "_gt"
                                  (= c 61) "_eq"
                                  (or (and (>= c 48) (<= c 57))
                                      (and (>= c 65) (<= c 90))
                                      (and (>= c 97) (<= c 122))
                                      (= c 95)) (string/from-bytes c)
                                  (string/format "_x%02X" c)))
                     (string/bytes base)) ""))
  (if (get emit-c-reserved folded) (string "k_" folded) folded))
(defn emit-c-num [v] (string/format "%.17g" v))
(defn emit-slot [a arity] (if (< a arity) (string "p" a) (string "s" a)))

# Step 14: call-shaped array/tuple access. (a i) compiles to
# push+tcall/call with the array as callee (tcall carries only the
# callee; call's dst is ins 1, callee ins 2); (t :k) is likewise a
# call. Returns [callee-slot idx-slot needs-check?] when code[pc] is a
# single-index access into a proven :array/:tuple slot, else nil. The
# single push must immediately precede the call (modulo one
# callee-slot load); anything else is fail-closed. needs-check? is
# true for :number indices (int-family needs no check): emission adds
# a fail-loud integrality guard. Mirrors infer/scan-def's
# pending-based rule; emission lowers to ->data[] exactly like get
# (bounds abort).
(defn call-shape-idx [d pc st]
  (def code (or (d :bytecode) @[]))
  (def n (length code))
  (when (or (< pc 0) (>= pc n)) (break nil))
  (def ins (code pc))
  (def op (ins 0))
  (var callee nil)
  (cond (= op 'call) (set callee (ins 2))
        (= op 'tcall) (set callee (ins 1))
        (break nil))
  (def callee-ty (get st callee :unknown))
  (unless (or (= callee-ty :array) (= callee-ty :tuple)) (break nil))
  (var j (- pc 1))
  # at most one callee-slot load between the push and the call
  (when (and (>= j 0) (get load-ops ((code j) 0))) (-- j))
  (when (< j 0) (break nil))
  (def jn (code j))
  (unless (= (jn 0) 'push) (break nil))
  (def idx (jn 1))
  # a second adjacent push means 2+ args (not our shape)
  (when (>= (- j 1) 0)
    (def pj ((code (- j 1)) 0))
    (when (or (= pj 'push) (= pj 'push2) (= pj 'push3) (= pj 'pusha))
      (break nil)))
  (def idx-ty (get st idx :unknown))
  (unless (or (infer/int-family? idx-ty) (= idx-ty :number)) (break nil))
  [callee idx (= idx-ty :number)])

# Pure core-call dest type: recorded core-cfunction call sites (ldc
# constants) typed from infer/pure-calls. Arity is NOT checked here —
# emission re-verifies strictly and refuses; intermediate optimism
# never escapes a refused fn.
(defn pure-dest-type [id pc]
  (def pn (get (get core-call-map id) pc))
  (def ps (and pn (get infer/pure-calls pn)))
  (and ps (ps :returns)))

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
            (get infer/div-ops op)
            (put st (ins 1) :double)
            (get infer/arith-ops op)
            (put st (ins 1)
                 (infer/arith-join (get st (ins 2) :unknown)
                                   (get st (ins 3) :unknown)))
            (get infer/bit-im-ops op)
            (put st (ins 1) (infer/bit-join (get st (ins 2) :unknown) :long))
            (get infer/bit-ops op)
            (put st (ins 1)
                 (infer/bit-join (get st (ins 2) :unknown)
                                 (get st (ins 3) :unknown)))
            (get infer/cmp-ops op) (put st (ins 1) :bool)
            (or (= op 'mkarr))
            (do (put st (ins 1) :array))
            (or (= op 'mktup) (= op 'mkbtp))
            (do (put st (ins 1) :tuple))
            (or (= op 'mkbuf) (= op 'mkstr) (= op 'mkstu) (= op 'mktab))
            (do (put st (ins 1) :unknown))

            (= op 'ldu) (put st (ins 1) :unknown)
            # Step 11: array access (mirrors infer/scan-def rules).
            # :number indices accepted (runtime guard at emission);
            # put/puti preserve the container type (see infer).
            (= op 'get)
            (put st (ins 1)
                 (if (and (or (= :array (get st (ins 2) :unknown))
                              (= :tuple (get st (ins 2) :unknown)))
                          (or (infer/int-family? (get st (ins 3) :unknown))
                              (= :number (get st (ins 3) :unknown))))
                   :number :unknown))
            (= op 'call)
            (do (def fta (get (get taccs id) pc))
              (if (and fta (= (fta :kind) :pread))
                # Phase 5: validated constant-key table read -> field type
                (put st (ins 1) (fta :type))
                (do (def tid (get cmap pc))
                  (if tid
                    (put st (ins 1) (rts tid))
                    (do (def pdt (pure-dest-type id pc))
                      (if pdt
                        (put st (ins 1) pdt)
                        (do (def sh (call-shape-idx d pc st))
                          (when sh (put st (ins 1) :number)))))))))
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

# int32-domain checked operand for bitwise emission. Boxed Janet
# errors outside int32 (integral doubles accepted, results wrap int32);
# the checks abort exactly where boxed errors (fail loud, like OOB).
# st: flow slot types; pt: hinted param types; slot: operand index.
(defn bit-operand-type [st pt arity slot]
  (if (< slot arity) (get pt slot :unknown) (get st slot :unknown)))
(defn emit-bit-check [expr t]
  (if (get int-hints t)
    (string "    if (" expr " < -2147483647LL - 1 || " expr
            " > 2147483647LL) { fprintf(stderr, \"int32 domain error\\n\"); abort(); }")
    (string "    if (" expr " != floor(" expr ") || " expr
            " < -2147483648.0 || " expr
            " > 2147483647.0) { fprintf(stderr, \"int32 domain error\\n\"); abort(); }")))

# Fail-loud guard for PGO-nonneg int division: the evidence is a
# workload observation, not a proof, so out-of-domain inputs (zero
# denominator, either operand negative) abort instead of silently
# miscomputing. Same doctrine as the OOB bounds abort.
(defn emit-intdiv-guard [sa sb arity]
  (string "    if (" (emit-slot sb arity) " <= 0 || "
          (emit-slot sa arity) " < 0) { fprintf(stderr, "
          "\"int div out of nonneg domain\\n\"); abort(); }"))

# Real-division operand expr: boxed div ALWAYS computes in doubles
# (even `(/ 1 2)` — the compiler folds integral `1.0` to `ldi`, but
# boxed still divides real). Int-family slots cast through (double),
# exact for |v| <= 2^53 like all int kernels; other numerics pass
# through. Returns nil for non-numeric (bool/nil/composite) → refuse.
(defn div-operand-expr [st slot arity]
  (def t (get st slot :unknown))
  (cond (or (get float-hints t) (= t :number)) (emit-slot slot arity)
        (get int-hints t) (string "(double)(" (emit-slot slot arity) ")")
        nil))

# Real-division dest must declare double: assigning a fractional
# double to an int64 temp would truncate where boxed keeps 3.5.
(defn div-dest-ok? [wtypes dst]
  (def t (get wtypes dst :double))
  (or (get float-hints t) (= t :number)))

(defn call-arg-slots [d pc]
  # mirror of call-arity, collecting pushed arg slots in order; nil
  # unless the walk ends at a clean boundary (fail-closed). Push
  # operands are authoritative -- interleaved loads only compute the
  # temps the pushes reference.
  (def code (or (d :bytecode) @[]))
  (def slots @[])
  (var j (- pc 1))
  (var scanned 0)
  (var ok true)
  (while (and (>= j 0) (< scanned 60) (= ok true))
    (++ scanned)
    (def ins (code j))
    (def op (ins 0))
    (cond
      (= op 'push) (array/insert slots 0 (ins 1))
      (= op 'push2) (do (array/insert slots 0 (ins 2))
                      (array/insert slots 0 (ins 1)))
      (= op 'push3) (do (array/insert slots 0 (ins 3))
                      (array/insert slots 0 (ins 2))
                      (array/insert slots 0 (ins 1)))
      (get flow-stop-ops op) (set ok false)
      (= op 'call) (set ok false)
      (= op 'pusha) (set ok nil)
      (get load-ops op) nil
      (set ok nil))
    (-- j))
  (if (or (< j 0) (= ok false)) slots nil))

# Strict pure-call resolution for emission: recorded core name +
# arity + numeric args (slots via the fail-closed backward walk).
# Returns [spec slots]; nil refuses. Defined after call-arg-slots.
(defn pure-call-shape [id pc st d]
  (def nm (get (get core-call-map id) pc))
  (when (nil? nm) (break nil))
  (def spec (get infer/pure-calls nm))
  (when (nil? spec) (break nil))
  (def slots (call-arg-slots d pc))
  (when (or (nil? slots) (not= (length slots) (length (spec :params))))
    (break nil))
  (each s slots
    (unless (get infer/numeric-types (get st s :unknown))
      (break nil)))
  [spec slots])

# Native-to-native call argument check: each passed arg must convert
# exactly into the callee param (same type, or int->float which is
# exact for |v| <= 2^53 like all int kernels). float->int refuses
# (truncation diverges from boxed). In particular, an array argument
# must be proven before bypassing the wrapper's type guard.
(defn call-args-compatible? [tid st d pc]
  (def cpt (get pts tid))
  (when (nil? cpt) (break false))
  (def slots (call-arg-slots d pc))
  (when (nil? slots)
    (break (not (some (fn [t] (= t :array)) cpt))))
  (when (not= (length slots) (length cpt)) (break false))
  (var i 0)
  (var ok true)
  (while (and ok (< i (length slots)))
    (def at (get st (slots i) :unknown))
    (def pt (cpt i))
    (unless (or (= at pt)
                (and (get infer/numeric-types at)
                     (or (= pt :double) (= pt :float) (= pt :number))))
      (set ok false))
    (++ i))
  ok)

# OOB guard emission with counted-loop upper elision (Step 16):
# elide? drops the `>= count` half (proven by loop discipline) and
# keeps `< 0` as defense-in-depth.
(defn emit-oob-check [lines idx-e bound-e msg elide?]
  (array/push lines
    (if elide?
      (string "    if (" idx-e " < 0) { fprintf(stderr, \"" msg "\\n\"); abort(); }")
      (string "    if (" idx-e " < 0 || " idx-e
              " >= " bound-e ") { fprintf(stderr, \"" msg "\\n\"); abort(); }"))))

# Integrality guard for :number indices (Step 17): int-family slots
# need no check; :number slots abort fail-loud on fractions (boxed
# indexing with a non-integer errors). idx-d is the double expr.
(defn emit-index-check [lines idx-d needs-check?]
  (when needs-check?
    (array/push lines
      (string "    if (" idx-d " != floor(" idx-d ")) { fprintf(stderr, \"index not an integer\\n\"); abort(); }"))))

# Writers of a slot in a pc range: array of [pc ins] via emit-writer
# positions. No early exits (safe inside any context).
(defn writes-of [code slot lo hi]
  (def out @[])
  (var q (max lo 0))
  (while (<= q hi)
    (def qn (code q))
    (def qp (get emit-writers (qn 0)))
    (when (and qp (= (qn qp) slot)) (array/push out [q qn]))
    (++ q))
  out)

# Step 16: counted-loop upper-guard elision for the `for i 0 (len arr)`
# idiom: the per-access `idx >= count` abort is redundant when the
# loop-exit check already proves it. Elides ONLY the upper half; the
# `< 0` check stays as defense-in-depth. Fail-closed: any deviation
# (opaque calls in loop, extra jumps, rebound array/counter, const
# index) keeps both checks. Recognized shape, from `for` codegen:
#   ldi IDX 0 ... len L ARR ... movn B L ... HEADER: lt C IDX B,
#   jmpno C EXIT ... accesses ... addim IDX IDX 1, jmp HEADER.
# Soundness: init-0 + single-+1-increment + dominated exit check +
# same-array bound + access-before-increment + no count mutation.
# Calls that lower to loads (call-shaped access, pure libm) are not
# opaque: anything else that could grow the array refuses.
(defn loop-elide-upper? [d pcA arr idxslot id st]
  (def code (or (d :bytecode) @[]))
  (def n (length code))
  # 1. innermost enclosing back-edge loop [T, JE] around pcA
  (def backs @[])
  (var j 0)
  (while (< j n)
    (def jn (code j))
    (when (and (= (jn 0) 'jmp) (< (+ j (jn 1)) j)
               (<= (+ j (jn 1)) pcA) (<= pcA j))
      (array/push backs [(+ j (jn 1)) j]))
    (++ j))
  (when (empty? backs) (break false))
  (var best (backs 0))
  (each b backs
    (when (< (- (b 1) (b 0)) (- (best 1) (best 0))) (set best b)))
  (def T (best 0))
  (def JE (best 1))
  # 2. exactly two jumps in (T, JE]: exit cond + back-edge
  (var njumps 0)
  (var q (+ T 1))
  (while (<= q JE)
    (def qo ((code q) 0))
    (when (or (= qo 'jmp) (= qo 'jmpif) (= qo 'jmpno)) (++ njumps))
    (++ q))
  (unless (= njumps 2) (break false))
  # 3. no opaque ops in [T, JE]: calls that lower to loads
  # (call-shaped access, pure libm) cannot mutate the count; any other
  # call/tcall/pusha/res aborts the elision (a callee could grow ARR).
  # The access at pcA itself is skipped (its st is post-state-update;
  # the sh-now snapshot already proved it lowers).
  (var nopaque 0)
  (var q2 T)
  (while (<= q2 JE)
    (when (not= q2 pcA)
      (def qo ((code q2) 0))
      (cond (or (= qo 'pusha) (= qo 'res)
                (= qo 'ret) (= qo 'retn) (= qo 'err))
            (++ nopaque)
            (or (= qo 'call) (= qo 'tcall))
            (unless (or (call-shape-idx d q2 st)
                        (pure-call-shape id q2 st d))
              (++ nopaque))))
    (++ q2))
  (unless (= nopaque 0) (break false))
  # 4. exit is jmpno (for-shape), target outside the loop
  (var exit-pc nil)
  (var q3 (+ T 1))
  (while (<= q3 JE)
    (when (and (not= q3 JE) (or (= ((code q3) 0) 'jmpif) (= ((code q3) 0) 'jmpno)))
      (set exit-pc q3))
    (++ q3))
  (when (nil? exit-pc) (break false))
  (def en (code exit-pc))
  (unless (= (en 0) 'jmpno) (break false))
  (def C (en 1))
  (def Ctarget (+ exit-pc (en 2)))
  (when (and (> Ctarget T) (<= Ctarget JE)) (break false))
  # 5. C defined in [T, pcA] by a single `lt C IDX2 B`
  (def cw (writes-of code C T pcA))
  (unless (= 1 (length cw)) (break false))
  (def lt-pc ((cw 0) 0))
  (def cins ((cw 0) 1))
  (unless (and (= (cins 0) 'lt) (= (cins 1) C)) (break false))
  (def IDX2 (cins 2))
  (def B (cins 3))
  # 6. counter discipline: exactly two writers, `ldi 0` before T and
  # `addim IDX2 IDX2 1` inside (T, JE]
  (def iw (writes-of code IDX2 0 (- n 1)))
  (unless (= 2 (length iw)) (break false))
  (var init-pc nil)
  (var incr-pc nil)
  (each w iw
    (def wpc (w 0))
    (def wins (w 1))
    (cond (and (= (wins 0) 'ldi) (= (wins 2) 0) (< wpc T))
          (set init-pc wpc)
          (and (= (wins 0) 'addim) (= (wins 1) IDX2) (= (wins 2) IDX2)
               (= (wins 3) 1) (> wpc T) (<= wpc JE))
          (set incr-pc wpc)))
  (when (or (nil? init-pc) (nil? incr-pc)) (break false))
  # 7. access index is the counter or its single movn-copy in [T, pcA]
  (def idx-ok
    (or (= idxslot IDX2)
        (do (def xw (writes-of code idxslot T pcA))
          (and (= 1 (length xw))
               (let [xi ((xw 0) 1)]
                 (and (= (xi 0) 'movn) (= (xi 1) idxslot) (= (xi 2) IDX2)))))))
  (unless idx-ok (break false))
  # 8. access sees the pre-increment value
  (unless (< pcA incr-pc) (break false))
  # 9. bound provenance, nearest-writer-before-use (slots are
  # reused across the prologue: the `for` macro reuses the len-temp as
  # the cond-temp, so global-single-writer would refuse the idiom).
  # B's nearest writer at-or-before the lt is `movn B L` (or direct
  # `len B ARR`) before T, from L's nearest writer at-or-before it
  # which is a single `len L ARR` of the SAME array before T.
  (defn writes-before [slot pc]
    (def out @[])
    (var q 0)
    (while (<= q pc)
      (def qn (code q))
      (def qp (get emit-writers (qn 0)))
      (when (and qp (= (qn qp) slot)) (array/push out [q qn]))
      (++ q))
    out)
  (def bw (writes-before B lt-pc))
  (unless (= 1 (length bw)) (break false))
  (def bins ((bw 0) 1))
  (def bpc ((bw 0) 0))
  (unless (< bpc T) (break false))
  (var len-pc nil)
  (cond (= (bins 0) 'len)
        (do (unless (= (bins 2) arr) (break false))
          (set len-pc bpc))
        (= (bins 0) 'movn)
        (do (def L (bins 2))
          (def lw (writes-before L bpc))
          (unless (= 1 (length lw)) (break false))
          (def lins ((lw 0) 1))
          (unless (and (= (lins 0) 'len) (= (lins 2) arr) (< ((lw 0) 0) T))
            (break false))
          (set len-pc ((lw 0) 0)))
        (break false))
  # 10. the array itself is stable: every rebinding write precedes
  # the len that bounds it, none inside the loop. `put`/`puti` mutate
  # in place (identity and count preserved) and never disqualify.
  (def aw (writes-of code arr 0 (- n 1)))
  (var astable true)
  (each w aw
    (def wpc (w 0))
    (def wop ((w 1) 0))
    (unless (or (= wop 'put) (= wop 'puti))
      (when (or (and (>= wpc T) (<= wpc JE)) (>= wpc len-pc))
        (set astable false))))
  (unless astable (break false))
  true)

(defn intrinsic-ctype [kty]
  # C type of the native field; loads convert to the dest slot width
  (cond (= kty :long) "int64_t" (= kty :float) "float"
        (= kty :i32) "int32_t" "double"))

(defn intrinsic-boxed [kty]
  # the Janet type boxed ffi/read returns for the storage width
  # (only :long survives as an integer -- s64 aside, everything
  # numeric comes back double)
  (if (= kty :long) :long :double))

(defn emit-intrinsic-call [kn tid d code pc st arity pending spread op dest]
  # kn: [kind ty off] from :kernel metadata. Direct native-memory
  # access for Path 2 struct getters/setters; [lines] or [nil reason].
  # Fail-closed: callee contract, arg count/types, and value types must
  # all agree, else the normal call path refuses as non-native.
  (def kk (kn 0))
  (def kty (kn 1))
  (def koff (kn 2))
  (def cty (intrinsic-ctype kty))
  (def kargc (if (= kk :pread) 1 2))
  (if (not= op 'call)
    (break [nil "intrinsic tail call unsupported"]))
  (if spread
    (break [nil "intrinsic spread call"]))
  (if (not= (length pending) kargc)
    (break [nil "intrinsic arity mismatch"]))
  # callee contract agrees with the spec (getter returns ty; setter
  # takes [pointer ty]-exactly -- implicit conversions stay boxed)
  (def cpt (get pts tid))
  (def crt (rts tid))
  # contract is the BOXED type (what ffi/read/poke observe), not the
  # storage width: :float/:i32 fields read back double
  (def bty (intrinsic-boxed kty))
  (if (= kk :pread)
    (when (not= crt bty) (break [nil "intrinsic returns mismatch"]))
    (do (when (or (not= (length cpt) 2) (not= (cpt 0) :pointer)
                  (not= (cpt 1) bty))
          (break [nil "intrinsic setter contract mismatch"]))
        (when (not= crt bty) (break [nil "intrinsic returns mismatch"]))))
  (def slots (call-arg-slots d pc))
  (if (or (nil? slots) (not= (length slots) kargc))
    (break [nil "intrinsic arg slots unclear"]))
  (if (not= :pointer (get st (slots 0) :unknown))
    (break [nil "intrinsic base must be :pointer"]))
  (def pexpr (pending 0))
  (def mem (string "(*(" cty "*)((char*)(" pexpr ") + " koff "))"))
  (if (= kk :pread)
    # load converts to the dest slot width (int-policy promote runs
    # int widths as double -- the cast keeps the decl honest)
    (do (def fvt (get st dest :unknown))
      (def dct (emit-kernel-type fvt))
      (break [[(string "    s" dest " = (" dct ")(" mem ");")]]))
    # store the value (cast to field width), dest is the stored value
    (do (def vslot (slots 1))
      (def vty (get st vslot :unknown))
      # value must BE the contract type exactly (no implicit
      # conversion at the boundary -- stays boxed otherwise)
      (unless (= vty bty) (break [nil "intrinsic value type mismatch"]))
      (def vok (if (= kty :long) (get int-hints vty)
                   (get infer/numeric-types vty)))
      (unless vok (break [nil "intrinsic value type mismatch"]))
      (def vexpr (pending 1))
      (break [[(string "    " mem " = (" cty ")(" vexpr ");")
               (string "    s" dest " = " vexpr ";")]]))))

(defn emit-fn-c [id d rt pt call-map emitted-names rts]
  (def arity (length pt))
  (when (d :vararg) (break [nil "vararg"]))
  # Phase 1: :nil is concrete too -- every 'ret/'retn site in a function
  # whose whole-program return type resolved to :nil genuinely returns
  # nil (type-join widens away from :nil the instant any other return
  # value appears, so rt=:nil here is sound, not a stale default).
  (unless (or (get infer/numeric-types rt) (= rt :array) (= rt :nil))
    (break [nil "return not concrete"]))
  # Step 14b: polymorphic :number returns code-gen as :double. Sound:
  # boxed arithmetic IS double arithmetic, int-domain ops carry their
  # own runtime checks (bit ops) or refuse (int div), and every double
  # op below matches boxed bit-for-bit. Callers still see the declared
  # :number in rts, so native-to-native calls into such fns refuse
  # fail-closed (width unification is future work); roots emit standalone.
  (def rt (if (= rt :number) :double rt))
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
  (def wsets @{})
  # param slots rebound in the body (nonneg evidence is entry-value
  # only, so any rebind kills it for int-div shaping below)
  (def pwritten @{})
  (defn wput [slot ty]
    (put wtypes slot ty)
    (def st (or (get wsets slot) @{}))
    (put st ty true)
    (put wsets slot st))
  (each i (range arity)
    (put wtypes i (pt i))
    (def st0 (or (get wsets i) @{}))
    (put st0 (pt i) true)
    (put wsets i st0))
  (var wpc 0)
  (each ins code
    (def wop (ins 0))
    (cond
      (= wop 'ldi) (wput (ins 1) :long)
      (= wop 'ldc)
      # ldc-loaded functions are dead callee values (calls resolve by
      # position, never by slot type), so they must not poison the
      # declaration join: a slot reused as a numeric temp afterwards
      # would otherwise mix function/number and refuse (nbody s21).
      # Flow/state walks still record :unknown (conservative: any live
      # non-call use refuses downstream).
      # Phase 5: a keyword constant consumed ONLY as a closed-shape
      # table key is dead in emitted code (indices resolve statically);
      # skipping its wput keeps the slot's numeric reuse declarable.
      (do (def wcc ((or (d :constants) @[]) (ins 2)))
        (unless (or (= :cfunction (type wcc)) (= :function (type wcc))
                    (and (= :keyword (type wcc))
                         (dead-key-ldc? id d wpc (ins 1) wcc)))
          (wput (ins 1) (infer/const-num-type d (ins 2)))))
      (= wop 'ldn) (wput (ins 1) :nil)
      (or (= wop 'ldt) (= wop 'ldf)) (wput (ins 1) :bool)
      (= wop 'len) (wput (ins 1) :long)
      # Phase 5: a move out of a shaped table slot is dead in emitted
      # code (the flat struct lives in its own variable); contribute no
      # declaration evidence to the dest slot
      (= wop 'movn)
      (unless (get (get tshapes id) (ins 2))
        (wput (ins 1) (get wtypes (ins 2) :unknown)))
      (= wop 'movf)
      (unless (get (get tshapes id) (ins 1))
        (wput (ins 2) (get wtypes (ins 1) :unknown)))
      (get infer/arith-im-ops wop)
      (wput (ins 1) (infer/arith-join (get wtypes (ins 2) :unknown) :long))
      (get infer/arith-ops wop)
      (wput (ins 1)
           (infer/arith-join (get wtypes (ins 2) :unknown)
                             (get wtypes (ins 3) :unknown)))
      (get infer/bit-im-ops wop)
      (wput (ins 1) (infer/bit-join (get wtypes (ins 2) :unknown) :long))
      (get infer/bit-ops wop)
      (wput (ins 1)
           (infer/bit-join (get wtypes (ins 2) :unknown)
                           (get wtypes (ins 3) :unknown)))
      (get infer/cmp-ops wop) (wput (ins 1) :bool)
      (or (= wop 'mkarr)) (wput (ins 1) :array)
      (or (= wop 'mktup) (= wop 'mkbtp)) (wput (ins 1) :tuple)
      # Phase 5: a shaped mktab contributes NO declaration evidence
      # (the flat struct lives in its own variable; the table slot is
      # typically reused for construction operands); an unshaped one
      # keeps the historical :unknown (refuses downstream).
      (or (= wop 'mkbuf) (= wop 'mkstr) (= wop 'mkstu))
      (wput (ins 1) :unknown)
      (= wop 'mktab)
      (unless (get (get tshapes id) (ins 1)) (wput (ins 1) :unknown))
      (= wop 'ldu)
      # Like ldc-loaded functions above: upvalue loads are overwhelmingly
      # dead callee values (extern fallbacks, closed-over fns resolve by
      # name/position, never by slot type), so they must not poison the
      # declaration join. Flow/state walks still record :unknown, so any
      # live non-call use refuses downstream fail-closed.
      nil
      (= wop 'get)
      (wput (ins 1)
           (if (and (or (= :array (get wtypes (ins 2) :unknown))
                        (= :tuple (get wtypes (ins 2) :unknown)))
                    (or (infer/int-family? (get wtypes (ins 3) :unknown))
                        (= :number (get wtypes (ins 3) :unknown))))
             :number :unknown))
      (= wop 'call)
      (do (def wta (get (get taccs id) wpc))
        (if (and wta (= (wta :kind) :pread))
          # Phase 5: a validated constant-key table read declares the
          # field's exact type
          (wput (ins 1) (wta :type))
          (do (def wtid (get cmap wpc))
            (if wtid
              (wput (ins 1) (rts wtid))
              (do (def wpdt (pure-dest-type id wpc))
                (if wpdt
                  (wput (ins 1) wpdt)
                  # call-shaped access declares a numeric dst (wtypes stands in
                  # for st in this linear sweep; same rule as flow-check)
                  (do (def wsh (call-shape-idx d wpc wtypes))
                    (when wsh (wput (ins 1) :number)))))))))
      nil)
    (++ wpc)
    (def wpos (get emit-writers wop))
    (when wpos
      (def dst (ins wpos))
      (if (>= dst arity)
        (put written dst true)
        (put pwritten dst true))))
  (def ct (emit-kernel-type rt))
  # Join-aware declarations: a reused slot must DECLARE a type that
  # holds EVERYTHING ever written to it. Last-wins miscompiled
  # silently here (nbody energy: a bool/int temp shared with double
  # writes declared `int`, truncating 0.5*m to 0). Resolution, in order:
  # unanalyzable (:unknown mixes, 64-bit sign mixes, composite mixes)
  # refuses loudly; any float-family write -> double (bools and ints
  # convert exactly into it; >2^53 ints keep the documented int-width
  # caveat); int mixes widen soundly (:long, else :u64, else int64_t
  # for narrower mixes); singletons keep exact current behavior.
  (defn decl-kind [kinds]
    # nil = keep the legacy wtypes fallback (dead/odd singletons only)
    (cond (= 0 (length kinds)) nil
          (and (= 1 (length kinds)) (= (kinds 0) :unknown)) nil
          (and (> (length kinds) 1)
               (some (fn [t] (= t :unknown)) kinds))
          :bad-unknown
          (some (fn [t] (or (= t :double) (= t :float) (= t :number))) kinds)
          :double
          (and (some (fn [t] (= t :long)) kinds)
               (some (fn [t] (= t :u64)) kinds))
          :bad-sign
          (some (fn [t] (= t :long)) kinds) :long
          (some (fn [t] (= t :u64)) kinds) :u64
          (some (fn [t] (get int-hints t)) kinds) :long
          (= 1 (length kinds)) (kinds 0)
          :bool))
  (var slotbad nil)
  (def decl-types @{})
  (each slot (sort (keys written))
    (when (and (nil? slotbad) (>= slot arity))
      (def kinds (keys (or (get wsets slot) @{})))
      (def dk (decl-kind kinds))
      (cond (= dk :bad-unknown)
            (set slotbad (string "slot s" slot " mixes unanalyzable "
                                 (string/join (map string kinds) "/")))
            (= dk :bad-sign)
            (set slotbad (string "slot s" slot " mixes signed/unsigned 64-bit"))
            (and (> (length kinds) 1)
                 (some (fn [t] (or (= t :array) (= t :tuple) (= t :struct)
                                   (= t :nil) (= t :string)
                                   (= t :keyword) (= t :symbol))) kinds))
            # Phase 2: a slot reused by the register allocator for both
            # a string value and anything else (e.g. a comparison's
            # bool result reusing the same temp a later branch no
            # longer needs as a string) must refuse, not silently fall
            # through decl-kind's unconditional :bool default -- that
            # would declare the slot `int` and then try to assign a
            # JanetString pointer into it (a real miscompile, caught
            # only by the C compiler's own type error, not by us).
            (set slotbad (string "slot s" slot " mixes "
                                 (string/join (map string kinds) "/")))
            dk
            (put decl-types slot dk))))
  (def lines @[])
  (var used-externs @[])
  (var used-externs @[])
  (each slot (sort (keys written))
    # Phase 1: a slot whose only writes are call destinations for a
    # :nil-returning (void) callee needs no C declaration at all --
    # `void sN;` is an invalid incomplete-type declaration, and the
    # call-emission branch above never assigns or reads such a slot
    # (bare statement call, no assignment).
    (def dt (get decl-types slot (get wtypes slot :double)))
    (unless (= dt :nil)
      (array/push lines (string "    " (emit-kernel-type dt ct)
                               " s" slot ";"))))
  (var bad nil)
  (when slotbad (set bad slotbad))
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
          # call-shaped access is resolved ONCE per site, before the
          # state update below runs: the dest slot often reuses the
          # index temp ((ldi 2 0) (push 2) (call 2 0)), so testing
          # after the update would see the :number result instead of
          # the :long index and refuse. Both the state rule and the
          # emission arm below share this snapshot.
          (def sh-now
            (when (and (or (= op 'call) (= op 'tcall))
                       (nil? (get cmap pc)))
              (call-shape-idx d pc st)))
          # Phase 5: snapshot of a VALIDATED constant-key table read at
          # this pc (from infer's taccs -- same pattern as cmap): loaded
          # before the state update below because the dest slot often
          # reuses the key-constant temp once its live range ends.
          (def ta-now
            (when (or (= op 'call) (= op 'tcall))
              (def ta (get (get taccs id) pc))
              (when (and ta (= (ta :kind) :pread)) ta)))
          # Phase 5: the mktab arm below needs the pending VALUE
          # expressions, but the state-update cond's literal-constructor
          # arm resets pending before the emission cond runs -- snapshot
          # the arg list first (same pattern as ta-now).
          (def mk-now
            (when (= op 'mktab) (array ;pending)))
          # Check direct-call args against their PRE-call types. The VM
          # may reuse an argument register for the call's destination;
          # after the state update, that register has the return type.
          (def args-now
            (when (and (or (= op 'call) (= op 'tcall))
                       (number? (get cmap pc)))
              (call-args-compatible? (get cmap pc) st d pc)))
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
            (get infer/div-ops op)
            (put st (ins 1) :double)
            (get infer/arith-ops op)
            (put st (ins 1)
                 (infer/arith-join (get st (ins 2) :unknown)
                                   (get st (ins 3) :unknown)))
            (get infer/bit-im-ops op)
            (put st (ins 1) (infer/bit-join (get st (ins 2) :unknown) :long))
            (get infer/bit-ops op)
            (put st (ins 1)
                 (infer/bit-join (get st (ins 2) :unknown)
                                 (get st (ins 3) :unknown)))
            (get infer/cmp-ops op) (put st (ins 1) :bool)
            (or (= op 'mkarr))
            (do (put st (ins 1) :array) (set pending @[]))
            (or (= op 'mktup) (= op 'mkbtp))
            (do (put st (ins 1) :tuple) (set pending @[]))
            (or (= op 'mkbuf) (= op 'mkstr) (= op 'mkstu) (= op 'mktab))
            (do (put st (ins 1) :unknown) (set pending @[]))

            (= op 'ldu) (put st (ins 1) :unknown)
            # Step 11: array access (mirrors infer/scan-def rules).
            # :number indices accepted (runtime guard at emission);
            # put/puti preserve the container type (see infer).
            (= op 'get)
            (put st (ins 1)
                 (if (and (or (= :array (get st (ins 2) :unknown))
                              (= :tuple (get st (ins 2) :unknown)))
                          (or (infer/int-family? (get st (ins 3) :unknown))
                              (= :number (get st (ins 3) :unknown))))
                   :number :unknown))
            (= op 'call)
            (if ta-now
              (put st (ins 1) (ta-now :type))
              (do (def tid (get cmap pc))
                (if tid
                  (put st (ins 1) (rts tid))
                  (do (def pdt (pure-dest-type id pc))
                    (if pdt
                      (put st (ins 1) pdt)
                      (when sh-now (put st (ins 1) :number)))))))
            nil)
          (cond
            (= op 'ldi)
            (array/push lines (string "    s" (ins 1) " = " (ins 2) ";"))
            (= op 'ldc)
            (do (def cval ((or (d :constants) @[]) (ins 2)))
              (cond
                (= :cfunction (type cval))
                # dead callee load (math/floor etc.): the call arms
                # resolve by core name; any live use of the loaded value
                # carries :unknown through flow, refusing at the use site
                # (same discipline as the ldu skip below).
                nil
                # Phase 5 carve-out: a keyword constant that feeds only
                # a closed-shape table key never emits at all -- field
                # indices resolve statically, and its slot may be
                # declared for a reused numeric temp instead.
                (and (= :keyword (type cval))
                     (dead-key-ldc? id d pc (ins 1) cval))
                nil
                # Phase 2/3: a string/keyword/symbol constant references
                # its deduplicated static slot (materialized once in
                # native_init -- see atom-const-name/emit-c-string-literal).
                (atom-const? (type cval))
                (array/push lines (string "    s" (ins 1) " = "
                                          (atom-const-name (type cval) cval) ";"))
                (do (unless (and cval (= :number (type cval)))
                      (set bad "ldc non-number")
                      (break))
                  (array/push lines (string "    s" (ins 1) " = " (emit-c-num cval) ";")))))
            (= op 'ret)
            (do (def rvt (get st (ins 1) :unknown))
              # family check: int fn returns int-family values, float fn
              # returns any numeric (int promotes); array fn returns the
              # array (in place); C converts the width; void fn's slot
              # must itself have resolved to :nil (sound per the type-join
              # widening argument at this function's native-eligibility gate)
              (def ok (cond
                        (= rt :array) (= rvt :array)
                        (= rt :nil) (= rvt :nil)
                        (get int-hints rt) (get int-hints rvt)
                        (get float-hints rt)
                        (or (get infer/numeric-types rvt) (= rvt :bool))
                        nil))
              (unless ok
                (set bad (string "ret slot is " rvt " not " rt))
                (break))
              (array/push lines (if (= rt :nil) "    return;"
                                   (string "    return " (emit-slot (ins 1) arity) ";"))))
            # retn (JOP_RETURN_NIL): a bare nil return with no value slot
            # at all -- only sound for a function whose whole-program
            # return type already resolved to :nil (same argument as 'ret
            # above); any other rt here would mean type-join should have
            # already widened away from :nil, so refuse defensively.
            (= op 'retn)
            (if (= rt :nil)
              (array/push lines "    return;")
              (do (set bad (string "retn in non-nil-returning fn (rt=" rt ")"))
                (break)))
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
            (cond
              # Phase 2: string (in)equality is a BYTE-CONTENT compare,
              # not a pointer compare -- raw C `==`/`!=` on two
              # JanetString values would compare the pointers, which is
              # wrong whenever two distinct string objects hold equal
              # bytes (the overwhelmingly common case: no one strings
              # are interned). Only (eq/neq) have two real operand
              # slots to inspect this way; (eqim/neqim) compare against
              # a small-int immediate and never apply to strings (a
              # string can't be a bytecode immediate), so they keep the
              # numeric path unconditionally.
              (and (not (get emit-cmp-im op))
                   (or (= op 'eq) (= op 'neq))
                   (= :string (get st (ins 2) :unknown))
                   (= :string (get st (ins 3) :unknown)))
              (array/push lines
                          (string "    s" (ins 1) " = "
                                  (if (= op 'neq) "!" "")
                                  "janet_string_equal(" (emit-slot (ins 2) arity)
                                  ", " (emit-slot (ins 3) arity) ");"))
              # Phase 4: cross-class comparison (value rep vs pointer
              # rep, both concrete): never equal by Janet semantics
              # (janet_equals checks type first), so the result is a
              # compile-time constant. Raw C `==` here is invalid C
              # (double vs const uint8_t*) -- previously surfaced only
              # as a late C-compiler error, now handled soundly.
              (and (not (get emit-cmp-im op))
                   (or (= op 'eq) (= op 'neq))
                   (mixed-class-cmp? (get st (ins 2) :unknown)
                                     (get st (ins 3) :unknown)))
              (array/push lines
                          (string "    s" (ins 1) " = "
                                  (if (= op 'eq) "0" "1") ";"))
              (get emit-cmp-im op)
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
            # documented policy in step 13); float-family: real division.
            # Operands cast through (double) as needed (boxed div is
            # always real); the dest must declare double.
            (do (def d1 (div-operand-expr st (ins 2) arity))
              (def d2 (div-operand-expr st (ins 3) arity))
              (if (and d1 d2 (div-dest-ok? wtypes (ins 1)))
                (do (set pending @[])
                  (array/push lines
                              (string "    s" (ins 1) " = " d1 " / " d2 ";")))
                (do (set bad "div needs float-compatible operands and dest (policy TBD)")
                  (break))))
            # divim (x / const): same policy as div, denominator needs
            # no cast (int consts promote in double division). Int path
            # needs nonneg src (PGO) and a nonzero const (a const zero
            # denominator would trap, so it refuses instead of guarding).
            (= op 'divim)
            (do (def d1 (div-operand-expr st (ins 2) arity))
              (cond
                (and d1 (div-dest-ok? wtypes (ins 1)))
                (do (set pending @[])
                  (array/push lines
                              (string "    s" (ins 1) " = " d1 " / "
                                      (ins 3) ";")))
                (and (not= (ins 3) 0)
                     (nonneg-operand? (get nnp id) pwritten (ins 2) st))
                (do (set pending @[])
                  (array/push lines
                              (string "    if (" (emit-slot (ins 2) arity)
                                      " < 0) { fprintf(stderr, \"int div out of nonneg domain\\n\"); abort(); }"))
                  (array/push lines
                              (string "    s" (ins 1) " = "
                                      (emit-slot (ins 2) arity) " / "
                                      (ins 3) ";")))
                (do (set bad "divim needs float or nonneg-int operands (policy TBD)")
                  (break))))
            # divf/mod/rem: float-family only (C % truncates toward zero
            # but Janet mod is floor-based — they diverge on negatives;
            # doubles via libm match boxed exactly, same op order).
            # Operands checked like div: int64 operands would truncate.
            # PGO-nonneg int path below: trunc == floor when both
            # operands are >= 0. PGO evidence is not proof, so the int
            # path guards BOTH zero (trap otherwise) and negativity
            # (silent miscompile otherwise) fail-loud, like OOB.
            (= op 'divf)
            (do (def d1 (div-operand-expr st (ins 2) arity))
              (def d2 (div-operand-expr st (ins 3) arity))
              (cond
                (and d1 d2 (div-dest-ok? wtypes (ins 1)))
                (do (set pending @[])
                  (array/push lines
                              (string "    s" (ins 1) " = floor(" d1 " / " d2 ");")))
                (and (nonneg-operand? (get nnp id) pwritten (ins 2) st)
                     (nonneg-operand? (get nnp id) pwritten (ins 3) st))
                (do (set pending @[])
                  (array/push lines (emit-intdiv-guard (ins 2) (ins 3) arity))
                  (array/push lines
                              (string "    s" (ins 1) " = "
                                      (emit-slot (ins 2) arity) " / "
                                      (emit-slot (ins 3) arity) ";")))
                (do (set bad "divf needs float or nonneg-int operands (policy TBD)")
                  (break))))
            (= op 'rem)
            (do (def d1 (div-operand-expr st (ins 2) arity))
              (def d2 (div-operand-expr st (ins 3) arity))
              (cond
                (and d1 d2 (div-dest-ok? wtypes (ins 1)))
                (do (set pending @[])
                  (array/push lines
                              (string "    s" (ins 1) " = fmod(" d1 ", " d2 ");")))
                (and (nonneg-operand? (get nnp id) pwritten (ins 2) st)
                     (nonneg-operand? (get nnp id) pwritten (ins 3) st))
                (do (set pending @[])
                  (array/push lines (emit-intdiv-guard (ins 2) (ins 3) arity))
                  (array/push lines
                              (string "    s" (ins 1) " = "
                                      (emit-slot (ins 2) arity) " % "
                                      (emit-slot (ins 3) arity) ";")))
                (do (set bad "rem needs float or nonneg-int operands (policy TBD)")
                  (break))))
            (= op 'mod)
            (do (def d1 (div-operand-expr st (ins 2) arity))
              (def d2 (div-operand-expr st (ins 3) arity))
              (cond
                (and d1 d2 (div-dest-ok? wtypes (ins 1)))
                (do (set pending @[])
                  (array/push lines
                              (string "    s" (ins 1) " = (" d2 ") == 0 ? ("
                                      d1 ") : ((" d1 ") - (" d2 ") * floor(("
                                      d1 ") / (" d2 ")));")))
                (and (nonneg-operand? (get nnp id) pwritten (ins 2) st)
                     (nonneg-operand? (get nnp id) pwritten (ins 3) st))
                (do (set pending @[])
                  (array/push lines (emit-intdiv-guard (ins 2) (ins 3) arity))
                  (array/push lines
                              (string "    s" (ins 1) " = "
                                      (emit-slot (ins 2) arity) " % "
                                      (emit-slot (ins 3) arity) ";")))
                (do (set bad "mod needs float or nonneg-int operands (policy TBD)")
                  (break))))
            # Phase 5: moving a closed-shape table slot (the compiler's
            # callee-position move) is dead in emitted code -- the flat
            # struct lives in its own variable and access sites refer
            # to it directly.
            (= op 'movn)
            (if (get (get tshapes id) (ins 2))
              nil
              (array/push lines (string "    s" (ins 1) " = "
                                        (emit-slot (ins 2) arity) ";")))
            (= op 'movf)
            (if (get (get tshapes id) (ins 1))
              nil
              (array/push lines (string "    s" (ins 2) " = "
                                        (emit-slot (ins 1) arity) ";")))
            # Phase 5: a proven closed-shape mktab lowers to a LOCAL C
            # struct (declared and zero-initialized right here -- valid
            # C, and the single-construction rule guarantees this site
            # executes at most once per call) plus field stores from
            # the pending VALUE expressions (odd positions; even
            # positions are the keyword keys, resolved statically --
            # their ldc loads were skipped as dead keys). The struct
            # lives in its own variable, NOT the table slot: the slot
            # was almost certainly reused by the register allocator
            # for the construction operands. No shape -> refuse (the
            # table stays boxed in the interpreter fallback, never a
            # forced layout).
            (= op 'mktab)
            (do (def mshp (get (get tshapes id) (ins 1)))
              (if mshp
                (do
                  (def snm (nshape-name id mshp))
                  (def svn (nshape-var mshp))
                  (array/push lines
                              (string "    struct " snm " { "
                                      (string/join
                                        (seq [i :range [0 (length (mshp :order))]]
                                          (string (nshape-ctype ((mshp :fields) ((mshp :order) i)))
                                                  " f" i)) "; ") "; };"))
                  (array/push lines
                              (string "    struct " snm " " svn " = {0};"))
                  (var mi 0)
                  (each mk (mshp :order)
                    (array/push lines
                                (string "    " svn ".f" mi " = ("
                                        (nshape-ctype ((mshp :fields) mk)) ")("
                                        (mk-now (+ 1 (* 2 mi))) ");"))
                    (++ mi))
                  (set pending @[]))
                (do (set bad "unhandled op mktab")
                  (break))))
            # Step 11: array access. janet get-OOB returns nil, which has
            # no unboxed representation — the native kernel aborts
            # instead (documented opt-in divergence). In-bounds loops of
            # the `for i 0 (length a)` shape never trip it.
            (= op 'len)
            (do (def lbase (get st (ins 2) :unknown))
              (unless (or (= lbase :array) (= lbase :tuple))
                (set bad "len of non-array")
                (break))
              (array/push lines (string "    s" (ins 1) " = "
                                        (if (= lbase :tuple)
                                          (string "janet_tuple_head(" (emit-slot (ins 2) arity) ")->length")
                                          (string (emit-slot (ins 2) arity) "->count")) ";")))
            (= op 'get)
            (do
              (def gbase (get st (ins 2) :unknown))
              (def gtup (= gbase :tuple))
              (unless (and (or (= gbase :array) gtup)
                           (infer/int-family? (get st (ins 3) :unknown)))
                (set bad (if gtup "get requires tuple and int index"
                             "get requires array and int index"))
                (break))
              # tuples index the inline data directly (JanetTuple IS
              # const Janet*); length lives in the head. Arrays use
              # ->data/->count. Bounds abort either way (fail loud).
              (def garr (emit-slot (ins 2) arity))
              (def gidxd (emit-slot (ins 3) arity))
              (def gidx (string "(int64_t)(" gidxd ")"))
              (def gbound (if gtup (string "janet_tuple_head(" garr ")->length")
                              (string garr "->count")))
              (def gdata (if gtup (string garr "[" gidx "]")
                             (string garr "->data[" gidx "]")))
              (def oobmsg (if gtup "tuple index out of bounds"
                              "array index out of bounds"))
              (emit-index-check lines gidxd (= :number (get st (ins 3) :unknown)))
              (emit-oob-check lines gidx gbound oobmsg
                              (loop-elide-upper? d pc (ins 2) (ins 3) id st))
              (array/push lines
                          (string "    s" (ins 1) " = janet_unwrap_number(" gdata ");")))
            (= op 'put)
            # Phase 5 first: a VALIDATED constant-key table write (infer
            # taccs) lowers to a direct member store into the flat
            # struct -- same pwrite discipline as the :kernel intrinsics
            # (:long into a :double field casts exactly for |v| <= 2^53,
            # the documented int-width caveat; double into :long was
            # already refused by the analysis).
            (do (def pwta (get (get taccs id) pc))
              (if (and pwta (= (pwta :kind) :pwrite))
                (do (def pwshp (get (get tshapes id) (pwta :base)))
                  (var pwidx -1)
                  (var pwi 0)
                  (each pwk (pwshp :order)
                    (when (= pwk (pwta :field)) (set pwidx pwi))
                    (++ pwi))
                  (array/push lines
                              (string "    " (nshape-var pwshp) ".f" pwidx " = ("
                                      (nshape-ctype (pwta :type)) ")("
                                      (emit-slot (ins 3) arity) ");")))
              # (put arr idx val): dst IS the array slot (ins 1).
              # :number indices carry the runtime integrality guard.
              (do (unless (and (= :array (get st (ins 1) :unknown))
                             (or (infer/int-family? (get st (ins 2) :unknown))
                                 (= :number (get st (ins 2) :unknown)))
                             (get infer/numeric-types (get st (ins 3) :unknown)))
                  (set bad "put requires array, int index, numeric value")
                  (break))
              (def pidx-e (string "(int64_t)(" (emit-slot (ins 2) arity) ")"))
              (emit-index-check lines (emit-slot (ins 2) arity)
                                (= :number (get st (ins 2) :unknown)))
              (emit-oob-check lines pidx-e
                              (string (emit-slot (ins 1) arity) "->count")
                              "array index out of bounds"
                              (loop-elide-upper? d pc (ins 1) (ins 2) id st))
              (array/push lines
                          (string "    " (emit-slot (ins 1) arity) "->data[(int64_t)("
                                  (emit-slot (ins 2) arity) ")] = janet_wrap_number("
                                  (emit-slot (ins 3) arity) ");")))))
            (= op 'puti)
            # puti: constant-index put (dst=ins1, value=ins2, index=ins3).
            # Bounds abort like put (fail loud); the constant is int32
            # by construction (compiler range).
            (do (unless (and (= :array (get st (ins 1) :unknown))
                             (get infer/numeric-types (get st (ins 2) :unknown)))
                  (set bad "puti requires array, numeric value")
                  (break))
              (array/push lines
                          (string "    if (" (ins 3) " < 0 || " (ins 3)
                                  " >= " (emit-slot (ins 1) arity)
                                  "->count) { fprintf(stderr, \"array index out of bounds\\n\"); abort(); }"))
              (array/push lines
                          (string "    " (emit-slot (ins 1) arity) "->data[" (ins 3)
                                  "] = janet_wrap_number("
                                  (emit-slot (ins 2) arity) ");")))
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
            (get emit-bit-c op)
            (do (set pending @[])
              (def bt2 (bit-operand-type st pt arity (ins 2)))
              (def bt3 (bit-operand-type st pt arity (ins 3)))
              (array/push lines (emit-bit-check (emit-slot (ins 2) arity) bt2))
              (array/push lines (emit-bit-check (emit-slot (ins 3) arity) bt3))
              (array/push lines
                          (string "    s" (ins 1) " = "
                                  (get emit-bit-c op) "((int32_t)"
                                  (emit-slot (ins 2) arity) ", (int32_t)"
                                  (emit-slot (ins 3) arity) ");")))
            (get emit-bit-im-c op)
            (do (set pending @[])
              (def bt2 (bit-operand-type st pt arity (ins 2)))
              (array/push lines (emit-bit-check (emit-slot (ins 2) arity) bt2))
              (array/push lines
                          (string "    s" (ins 1) " = "
                                  (get emit-bit-im-c op) "((int32_t)"
                                  (emit-slot (ins 2) arity) ", (int32_t)"
                                  (ins 3) ");")))
            # Phase 5: a VALIDATED constant-key table read (infer
            # taccs) lowers to a direct member load out of the flat
            # struct (pread) -- checked before pure/core calls because
            # the callee slot's lattice type is :unknown (a table), so
            # no other call arm can claim it.
            (or ta-now)
            (do (def tshp5 (get (get tshapes id) (ta-now :base)))
              (var tidx5 -1)
              (var ti5 0)
              (each tk5 (tshp5 :order)
                (when (= tk5 (ta-now :field)) (set tidx5 ti5))
                (++ ti5))
              (def tmem5 (string (nshape-var tshp5) ".f" tidx5))
              (if (= op 'tcall)
                (array/push lines (string "    return " tmem5 ";"))
                (array/push lines (string "    s" (ins 1) " = " tmem5 ";")))
              (set pending @[]))
            # call-shaped array/tuple access (see call-shape-idx):
            # single-index (a i) into :array/:tuple lowers exactly like
            # get (bounds abort, fail loud). Checked before the general
            # call arm; true calls (resolved tid) fall through. Uses the
            # sh-now snapshot (see above), never re-queries st.
            (or sh-now)
            (do (def sh2 sh-now)
              (def sharr (sh2 0))
              (def shidx (sh2 1))
              (def shtup (= (get st sharr :unknown) :tuple))
              (def sharr-e (emit-slot sharr arity))
              (def shidx-e (string "(int64_t)(" (emit-slot shidx arity) ")"))
              (def shbound (if shtup (string "janet_tuple_head(" sharr-e ")->length")
                                   (string sharr-e "->count")))
              (def shdata (if shtup (string sharr-e "[" shidx-e "]")
                                     (string sharr-e "->data[" shidx-e "]")))
              (def shoob (if shtup "tuple index out of bounds"
                                    "array index out of bounds"))
              (emit-index-check lines (emit-slot shidx arity) (sh2 2))
              (emit-oob-check lines shidx-e shbound shoob
                              (loop-elide-upper? d pc sharr shidx id st))
              (if (= op 'tcall)
                (array/push lines (string "    return janet_unwrap_number(" shdata ");"))
                (array/push lines (string "    s" (ins 1) " = janet_unwrap_number(" shdata ");")))
              (set pending @[]))
            # pure core calls (math/floor etc.): strict name + arity +
            # numeric-args check, lowered to libm with (double) casts.
            # Checked after the shape arm (mutually exclusive in practice:
            # shape needs an array callee, pure a recorded cfunction).
            (or (and (or (= op 'call) (= op 'tcall))
                     (nil? (get cmap pc))
                     (not sh-now)
                     (not ta-now)
                     (pure-call-shape id pc st d)))
            (do (def pres (pure-call-shape id pc st d))
              (def pspec (pres 0))
              (def pslots (pres 1))
              (def pargs (seq [s :in pslots]
                           (string "(double)(" (emit-slot s arity) ")")))
              (def pcall (string (pspec :c) "(" (string/join pargs ", ") ")"))
              (if (= op 'tcall)
                (array/push lines (string "    return " pcall ";"))
                (array/push lines (string "    s" (ins 1) " = " pcall ";")))
              (set pending @[]))
            (or (= op 'call) (= op 'tcall))
            (do (def tid (get cmap pc))
              # Path 2 struct intrinsic: a call to a def carrying
              # :kernel metadata emits direct native-memory access
              # instead of a call (the callee is never emitted; its
              # boxed body serves the interpreter). Anything unclear
              # falls through to the normal path (refuses as usual).
              (def kn (when (number? tid)
                        (def tinfo (get defs tid))
                        (when tinfo (get kernel-intrinsics (get tinfo :name)))))
              (if kn
                (do (def kres (emit-intrinsic-call kn tid d code pc st arity
                                                  pending spread op (ins 1)))
                  (if (kres 0)
                    (do (each l (kres 0) (array/push lines l))
                      (set pending @[]))
                    (do (set bad (kres 1)) (break))))
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
                  # Numeric results compose as before. An exact array
                  # result also composes when the destination is proven
                  # array on this path AND every write to that C slot is
                  # array-typed (the declaration join refuses mixed slots).
                  # This does not unbox array elements.
                  # Args must convert exactly (int->float ok, float->int
                  # refuses). Tail calls keep exact match (returned as
                  # our own value; conversion could truncate).
                  (def ret-ok
                    (or (and (not= trt :array) (= trt rt))
                        (get infer/numeric-types trt)
                        (and (= trt :array)
                             (= (get st (ins 1) :unknown) :array)
                             (= (get decl-types (ins 1) :unknown) :array))))
                  # Phase 1: a :nil-returning callee produces no C value
                  # at all -- `sN = voidfn(args);` is invalid C, so a
                  # void callee can NEVER compose into an assignment
                  # regardless of ret-ok's numeric test above. Call it
                  # as a bare statement instead; the destination slot
                  # (ins 1) is never subsequently read for a real value
                  # (the whole-program lattice already fail-closes any
                  # attempt to use a :nil result as something concrete).
                  (if (and tid cname (= trt :nil) (not spread) (not= op 'tcall)
                            args-now)
                    (do (array/push lines
                                    (string "    " (emit-c-name cname)
                                            "(" (string/join pending ", ") ");"))
                      (set pending @[]))
                  (if (and tid cname ret-ok (not spread) (not= op 'tcall)
                            args-now)
                    (do (array/push lines
                                    (string "    s" (ins 1) " = "
                                            (emit-c-name cname) "(" (string/join pending ", ") ");"))
                      (set pending @[]))
                    (if (and tid cname (= trt rt) (not spread)
                              args-now)
                      (do (array/push lines
                                      (string "    return " (emit-c-name cname)
                                              "(" (string/join pending ", ") ");"))
                        (set pending @[]))
                      (do (set bad "call to non-native or type mismatch")
                        (break)))))))))
            # ldu (load upvalue) is dead in emitted code: static calls
            # resolve by name, and any live use of the loaded value
            # carries :unknown through flow, refusing at the use site.
            # Skipping here is sound; the else below stays fail-closed.
            (= op 'ldu) nil
            # fail closed: an unhandled opcode previously vanished
            # silently (puti proved it), producing wrong C. Refuse with
            # the op name instead.
            (do (set bad (string "unhandled op " op))
              (break)))
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
    # :array returns are the (in-place) array itself
    (each t pt (unless (or (get infer/numeric-types t) (= t :array) (= t :tuple) (= t :pointer) (= t :string) (= t :keyword) (= t :symbol)) (set concrete false)))
    # Phase 1: :nil is concrete -- a whole-program-proven-always-nil
    # return is a legitimate void kernel, not an unproven boxed value.
    (unless (or (get infer/numeric-types rt) (= rt :array) (= rt :nil)) (set concrete false))
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
    # Tuple params sample the same way (fixed length-8 fixtures below).
    (def arrk (some (fn [t] (= t :array)) pt2))
    # :pointer fixtures are two malloc'd 8-double blocks ([0..7],
    # [7..0]); kernels must stay within 64 bytes (documented contract)
    (def per (map (fn [t] (cond
                            (= t :array) @[0 1]
                            (= t :tuple) @[0 1]
                            (= t :pointer) @[0 1]
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
  # bind intrinsic accessors (macro-installed into unit envs, invisible
  # to the analyzer env) so eval'd kernel forms resolve them for parity
  (each f args
    (def uenv (get unit-envs f))
    (when uenv
      (each kn (keys kernel-intrinsics)
        (def entry (get uenv (symbol kn)))
        (when entry (put (curenv) (symbol kn) entry)))))
  # bind top-level self-evaluating globals (numbers, strings,
  # keywords, booleans, nil, quotes) so eval'd kernel forms resolve
  # constants like NB. Calls and complex initializers stay out (their
  # side effects belong to load time, not the harness); fns needing
  # them fail closed at the eval below with a reason.
  (defn self-eval-value? [v]
    (or (string? v) (number? v) (keyword? v) (boolean? v) (nil? v)
        (and (tuple? v) (= 2 (length v)) (= (v 0) 'quote))))
  (each f args
    (each form (or (get unit-forms f) @[])
      (when (and (tuple? form) (>= (length form) 3) (= (form 0) 'def)
                 (symbol? (form 1)) (self-eval-value? (form 2)))
        (try (eval form) ([_] nil)))))
  (var eval-failed @[])
  (each e emitted
    (def form (get all-defn-forms (e :name)))
    (unless form
      (eprint "emit-native: no form for " (e :name))
      (os/exit 1))
    (def fnval (try (eval form) ([err] nil)))
    (if (nil? fnval)
      (array/push eval-failed e)
      (do (array/push c-parts (e :code))
        (each combo (samples-for (e :pt))
          (array/push calls {:e e :args (tuple ;combo) :fn fnval})))))
  (each e eval-failed
    (print "native-skip " (e :id) " " (e :name) " oracle eval failed"))
  (when (empty? calls)
    (eprint "emit-native: no samplable functions")
    (os/exit 1))
  (def cout @"")
  (buffer/push-string cout "#include <stdio.h>\n#include <inttypes.h>\n#include <math.h>\n#include <stdlib.h>\n#include <string.h>\n")
  (buffer/push-string cout "/* int32-domain bitwise helpers: boxed Janet errors outside int32\n * (integral doubles accepted); results wrap int32; shift counts mask\n * to 5 bits. Callers range/integrality-check operands (fail loud). */\nstatic int32_t k_band32(int32_t a, int32_t b) { return a & b; }\nstatic int32_t k_bor32(int32_t a, int32_t b) { return a | b; }\nstatic int32_t k_bxor32(int32_t a, int32_t b) { return a ^ b; }\nstatic int32_t k_sl32(int32_t a, int32_t b) { return (int32_t)((uint32_t)a << ((uint32_t)b & 31u)); }\nstatic int32_t k_sr32(int32_t a, int32_t b) { uint32_t n = (uint32_t)b & 31u; int32_t r = (int32_t)((uint32_t)a >> n); if ((a < 0) && (n != 0)) { r |= (int32_t)(~0u << (32u - n)); } return r; }\nstatic int32_t k_sru32(int32_t a, int32_t b) { return (int32_t)((uint32_t)a >> ((uint32_t)b & 31u)); }\n\n")
  # Step 11: array kernels need Janet layouts + wrap macros (header
  # only — no lib link) and abort() for the bounds guard. Tuple
  # kernels need the same (JanetTupleHead for fixture construction).
  (var anyarr false)
  (each e emitted
    (each t (e :pt) (when (or (= t :array) (= t :tuple)) (set anyarr true))))
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
  # Step 11: fixed sample arrays, ONE pair shared by every array param
  # (aliasing matches the eval side, where each variant is a single
  # shared object -- required for in-place kernels, whose mutations
  # must propagate identically on both sides).
  # Variant 0 = [0..7], variant 1 = [7..0]; the oracle below uses the
  # same values. Elements stay integral (bitwise kernels abort on
  # fractions by design, and every kernel runs every variant); length
  # 8 covers fixed-size kernels (u64 limbs) and keeps element values
  # valid as indices into either variant.
  # Tuple fixtures mirror the arrays (same values, immutable): a
  # head+data store per variant, JanetTuple aliasing its data (head
  # layout matches JanetTupleHead member-for-member by construction).
  (var anytup false)
  (each e emitted
    (each t (e :pt) (when (= t :tuple) (set anytup true))))
  (when anytup
    (buffer/push-string cout
      (string "    struct { JanetGCObject gc; int32_t length; int32_t hash;"
              " int32_t sm_line; int32_t sm_column; Janet tup_data[8]; } tup_v0_store, tup_v1_store;\n"
              "    memset(&tup_v0_store, 0, sizeof(tup_v0_store));\n"
              "    memset(&tup_v1_store, 0, sizeof(tup_v1_store));\n"
              "    tup_v0_store.length = 8;\n"
              "    tup_v1_store.length = 8;\n"
              "    for (int _q = 0; _q < 8; _q++) tup_v0_store.tup_data[_q] = janet_wrap_number(_q);\n"
              "    for (int _q = 0; _q < 8; _q++) tup_v1_store.tup_data[_q] = janet_wrap_number(7 - _q);\n"
              "    JanetTuple tup_v0 = tup_v0_store.tup_data;\n"
              "    JanetTuple tup_v1 = tup_v1_store.tup_data;\n")))
  (var anyptr false)
  (each e emitted
    (each t (e :pt) (when (= t :pointer) (set anyptr true))))
  (when anyptr
    (buffer/push-string cout
      (string "    double *ptr_v0 = (double *)malloc(8 * sizeof(double));\n"
              "    double *ptr_v1 = (double *)malloc(8 * sizeof(double));\n"
              "    for (int _q = 0; _q < 8; _q++) ptr_v0[_q] = (double)_q;\n"
              "    for (int _q = 0; _q < 8; _q++) ptr_v1[_q] = (double)(7 - _q);\n")))
  (when anyarr
    (buffer/push-string cout
      (string "    Janet arr_v0_elts[8];\n"
              "    for (int _q = 0; _q < 8; _q++) arr_v0_elts[_q] = janet_wrap_number(_q);\n"
              "    JanetArray arr_v0 = {{0}, 8, 8, arr_v0_elts};\n"
              "    Janet arr_v1_elts[8];\n"
              "    for (int _q = 0; _q < 8; _q++) arr_v1_elts[_q] = janet_wrap_number(7 - _q);\n"
              "    JanetArray arr_v1 = {{0}, 8, 8, arr_v1_elts};\n")))
  (each c calls
    (def ept ((c :e) :pt))
    (def args-c (string/join
                  (seq [i :range [0 (length (c :args))]]
                    (cond (= :array (ept i))
                          (string "&arr_v" ((c :args) i))
                          (= :tuple (ept i))
                          (string "tup_v" ((c :args) i))
                          (= :pointer (ept i))
                          (string "ptr_v" ((c :args) i))
                          (string ((c :args) i))))
                  ", "))
    # per-sample fixture reset: in-place kernels mutate the shared
    # variants, and later samples must observe pristine inputs
    # (a pancake loop on contaminated duplicates can even cycle
    # forever). Tuples are immutable and need no reset.
    (when anyarr
      (buffer/push-string cout
        "    { for (int _q = 0; _q < 8; _q++) { arr_v0_elts[_q] = janet_wrap_number(_q); arr_v1_elts[_q] = janet_wrap_number(7 - _q); } }\n"))
    (when anyptr
      (buffer/push-string cout
        "    { for (int _q = 0; _q < 8; _q++) { ptr_v0[_q] = (double)_q; ptr_v1[_q] = (double)(7 - _q); } }\n"))
    (buffer/push-string cout
      (if (= :array ((c :e) :rt))
        (string "    { JanetArray *_r = " (emit-c-name ((c :e) :name))
                "(" args-c "); printf(\"[\"); "
                "for (int32_t _k = 0; _k < _r->count; _k++) "
                "{ if (_k) printf(\" \"); "
                "printf(\"%.17g\", janet_unwrap_number(_r->data[_k])); } "
                "printf(\"]\\n\"); }\n")
        (string "    printf(\"%.17g\\n\", (double) " (emit-c-name ((c :e) :name))
                "(" args-c "));\n"))))
  (when anyptr
    (buffer/push-string cout "    free(ptr_v0);\n    free(ptr_v1);\n"))
  (buffer/push-string cout "    return 0;\n}\n")
  (spit (string outdir "/native.c") cout)
  (def expected @[])
  (def arr-variants [@[0 1 2 3 4 5 6 7] @[7 6 5 4 3 2 1 0]])
  (def tup-variants [(tuple 0 1 2 3 4 5 6 7) (tuple 7 6 5 4 3 2 1 0)])
  # pointer variants mirror the C fixtures exactly (8 doubles each).
  # Arrays AND pointers are re-created fresh per sample (shared within
  # a sample by variant index, so aliasing is preserved): in-place
  # kernels mutate their inputs, and reusing one shared object across
  # samples contaminates later samples — a data-dependent loop over
  # contaminated duplicates can even cycle forever (fannkuch found it).
  # Tuples are immutable and stay shared. The C side resets its
  # fixtures before every call (see above), so both sides agree.
  (defn- mkptr [flip]
    (def p (unsafe/malloc 64))
    (for i 0 8 (unsafe/poke :double p (if flip (- 7 i) i) (* i 8)))
    p)
  (def ptr-garbage @[])
  (defn- fresh-ptr [flip]
    (def p (mkptr flip))
    (array/push ptr-garbage p)
    p)
  (each c calls
    (def ept ((c :e) :pt))
    (def fresh @{})
    (def coerced (seq [i :range [0 (length (c :args))]]
                   (do (def ai ((c :args) i))
                     (cond (= :array (ept i))
                           # put returns the table: re-fetch the array
                           (or (get fresh ai)
                               (do (put fresh ai (array/slice (get arr-variants ai)))
                                 (get fresh ai)))
                           (= :tuple (ept i))
                           (get tup-variants ai)
                           (= :pointer (ept i))
                           (or (get fresh (tuple :p ai))
                               (do (put fresh (tuple :p ai) (fresh-ptr (= ai 1)))
                                 (get fresh (tuple :p ai))))
                           ai))))
    (def v ((c :fn) ;coerced))
    # array returns print canonically on both sides: [%.17g ...]
    # (numeric elements assumed -- the harness only feeds those)
    (array/push expected
      (if (= :array ((c :e) :rt))
        (string "[" (string/join (map (fn [x] (string/format "%.17g" x)) v) " ") "]")
        (string/format "%.17g" v))))
  (each p ptr-garbage (unsafe/free p))
  (spit (string outdir "/expected.txt") (string (string/join expected "\n") "\n"))
  (print "native " (length emitted) " fns " (length calls) " samples " outdir))

(defn wrapper-for [nm pt rt telemetry-id]
  (def cn (emit-c-name nm))
  (def tid (string/format "%08X" telemetry-id))
  (def type-guards
    (string/join
      (seq [i :range [0 (length pt)]]
        (def t (pt i))
        (def expected (cond
          (or (get int-hints t) (get float-hints t) (= t :number)) "JANET_NUMBER"
          (= t :array) "JANET_ARRAY"
          (= t :tuple) "JANET_TUPLE"
          (= t :pointer) "JANET_POINTER"
          (= t :string) "JANET_STRING"
          (= t :keyword) "JANET_KEYWORD"
          (= t :symbol) "JANET_SYMBOL"
          nil))
        (when expected
          (string "    if (janet_type(argv[" i "]) != " expected ") { " cn "_TELEMETRY_TYPE_GUARD(); janet_panicf(\"native " cn " argument " i " has wrong Janet type\"); }\n")))
      ""))
  # boundary casts: every int width unboxes/wraps as integer (C
  # implicit conversion narrows/widens to the kernel's exact width),
  # float widths as number, arrays/tuples/pointers pass through
  (def ps (string/join (seq [i :range [0 (length pt)]]
                     (def t (pt i))
                     (cond (get int-hints t)
                           (string "janet_unwrap_integer(argv[" i "])")
                           (= t :array)
                           (string "(JanetArray *)janet_unwrap_array(argv[" i "])")
                           (= t :tuple)
                           (string "janet_unwrap_tuple(argv[" i "])")
                           (= t :pointer)
                           (string "janet_unwrap_pointer(argv[" i "])")
                           (= t :string)
                           (string "janet_unwrap_string(argv[" i "])")
                           (= t :keyword)
                           (string "janet_unwrap_keyword(argv[" i "])")
                           (= t :symbol)
                           (string "janet_unwrap_symbol(argv[" i "])")
                           (string "janet_unwrap_number(argv[" i "])")))
                  ", "))
  # Phase 1: a void kernel produces no C value at all -- rv stays nil as
  # a sentinel; the final-line emission below special-cases it to call
  # the kernel as a bare statement, then separately return nil.
  (def rv (cond (= rt :nil) nil
                (= rt :array) "janet_wrap_array"
                (and (get int-hints rt) (not= int-policy :promote))
                "janet_wrap_integer"
                "janet_wrap_number"))
  (string "#ifndef JANET_NATIVE_TELEMETRY\n#define JANET_NATIVE_TELEMETRY 1\n#endif\n"
          "#if JANET_NATIVE_TELEMETRY\n"
          "static JANET_THREAD_LOCAL unsigned long long " cn "_telemetry_calls;\n"
          "static JANET_THREAD_LOCAL unsigned long long " cn "_telemetry_arity_failures;\n"
          "static JANET_THREAD_LOCAL unsigned long long " cn "_telemetry_type_failures;\n"
          "static void " cn "_telemetry_report(void) {\n"
          "  if (" cn "_telemetry_calls || " cn "_telemetry_arity_failures || " cn "_telemetry_type_failures)\n"
          "    fprintf(stderr, \"native-telemetry id=" tid " name=" cn " calls=%llu arity-guard-failures=%llu type-guard-failures=%llu\\n\", " cn "_telemetry_calls, " cn "_telemetry_arity_failures, " cn "_telemetry_type_failures);\n}\n"
          "#define " cn "_TELEMETRY_HIT() (++" cn "_telemetry_calls)\n"
          "#define " cn "_TELEMETRY_ARITY_GUARD() (++" cn "_telemetry_arity_failures)\n"
          "#define " cn "_TELEMETRY_TYPE_GUARD() (++" cn "_telemetry_type_failures)\n"
          "#else\n#define " cn "_TELEMETRY_HIT() ((void)0)\n#define " cn "_TELEMETRY_ARITY_GUARD() ((void)0)\n#define " cn "_TELEMETRY_TYPE_GUARD() ((void)0)\n"
          "#define " cn "_telemetry_report() ((void)0)\n#endif\n"
          "static Janet " cn "_wrapper(int32_t argc, Janet *argv) {\n"
          "    if (argc != " (length pt) ") { " cn "_TELEMETRY_ARITY_GUARD(); janet_panicf(\"native " cn
          " called with %d arguments, expected " (length pt) "\", argc); }\n"
          type-guards
          "    " cn "_TELEMETRY_HIT();\n"
          (if (nil? rv)
            (string "    " cn "(" ps ");\n    return janet_wrap_nil();\n}\n")
            (string "    return " rv "(" cn "(" ps "));\n}\n"))
          "#undef " cn "_TELEMETRY_HIT\n#undef " cn "_TELEMETRY_ARITY_GUARD\n#undef " cn "_TELEMETRY_TYPE_GUARD\n"))

(when emit-native-dir (emit-native-all emit-native-dir))

# ---------------------------------------------------------------------------
# Phase 4 (type-generator plan): switch/dispatch codegen for genuinely
# polymorphic ("flip") slots. Today a slot is either proven ONE concrete
# type (native-eligible) or the whole function stays boxed. This is the
# middle tier: a slot whose PGO-observed type mask is a CLOSED, small set
# of scalar atom types (>= 2 distinct observed types, every one with a
# concrete C representation from Phases 1-3) gets a runtime dispatcher:
# switch (janet_type(argv[slot])) with one specialized kernel per observed
# type and a MANDATORY, reachable, correct `default` arm that falls back
# to the fully boxed interpreter definition via janet_call. A value of a
# never-trained type at runtime takes that default arm and must produce
# the correct boxed answer -- never a crash, never silent wrong output.
# That fallback is the single most important safety property of this
# phase and is tested first-class in test/suite-native-dispatch.janet.
#
# Deliberately narrow scope (be conservative, only widen):
#   - exactly ONE polymorphic param slot (multiple flip slots multiply
#     dispatch complexity; not attempted),
#   - observed types limited to :number/:string/:keyword/:symbol (the
#     scalar atom shapes wrapper-for can already guard+unbox),
#   - :number branches always emit as :double (boxed Janet arithmetic
#     IS double arithmetic -- exact, never a truncating miscompile),
#   - uniform proven return type (numeric/:array/:nil), non-vararg,
#     loop-free (loop bodies still need resumable VM quanta),
#   - the function is NOT statically provable (this tier only ADDS
#     candidates that today stay boxed; never changes existing ones),
#   - every per-branch emit-fn-c must succeed, else the whole function
#     stays boxed (no partial dispatch).
# ---------------------------------------------------------------------------

(def dispatch-obs-types @{:number true :string true :keyword true :symbol true})
(defn- dispatch-branch-suffix [t]
  (case t :number "num" :string "str" :keyword "kw" "sym"))
(defn- dispatch-branch-type [t]
  # observed :number -> :double kernel param (see rationale above)
  (if (= t :number) :double t))

(defn- native-unwrap-expr [t i]
  # One argument's unbox expression (mirrors wrapper-for's ps logic;
  # factored out so the dispatcher can build per-branch arg lists).
  (cond (get int-hints t)
        (string "janet_unwrap_integer(argv[" i "])")
        (= t :array)
        (string "(JanetArray *)janet_unwrap_array(argv[" i "])")
        (= t :tuple)
        (string "janet_unwrap_tuple(argv[" i "])")
        (= t :pointer)
        (string "janet_unwrap_pointer(argv[" i "])")
        (= t :string)
        (string "janet_unwrap_string(argv[" i "])")
        (= t :keyword)
        (string "janet_unwrap_keyword(argv[" i "])")
        (= t :symbol)
        (string "janet_unwrap_symbol(argv[" i "])")
        (string "janet_unwrap_number(argv[" i "])")))

(defn- dispatch-janet-type-tag [t]
  (case t :number "JANET_NUMBER" :string "JANET_STRING"
    :keyword "JANET_KEYWORD" "JANET_SYMBOL"))

(defn dispatch-wrapper-for [nm slot branches pt rt telemetry-id]
  # branches: [{:obs :pt :cname}] ordered; each arm unboxes ALL args per
  # that branch's pt (the flip slot per its branch type, the rest per the
  # statically-proven pt) and calls that branch's specialized kernel.
  (def cn (emit-c-name nm))
  (def tid (string/format "%08X" telemetry-id))
  (def rv (cond (= rt :nil) nil
                (= rt :array) "janet_wrap_array"
                (and (get int-hints rt) (not= int-policy :promote))
                "janet_wrap_integer"
                "janet_wrap_number"))
  (def arms
    (string/join
      (seq [b :in branches]
        (def call-args
          (string/join (seq [i :range [0 (length (b :pt))]]
                        (native-unwrap-expr ((b :pt) i) i)) ", "))
        (string
          "    case " (dispatch-janet-type-tag (b :obs)) ": {\n"
          "        " cn "_TELEMETRY_HIT();\n"
          (if (nil? rv)
            (string "        " (b :cname) "(" call-args ");\n"
                    "        return janet_wrap_nil();\n")
            (string "        return " rv "(" (b :cname) "(" call-args "));\n"))
          "    }\n"))
      ""))
  (string "#ifndef JANET_NATIVE_TELEMETRY\n#define JANET_NATIVE_TELEMETRY 1\n#endif\n"
          "#if JANET_NATIVE_TELEMETRY\n"
          "static JANET_THREAD_LOCAL unsigned long long " cn "_telemetry_calls;\n"
          "static JANET_THREAD_LOCAL unsigned long long " cn "_telemetry_fallbacks;\n"
          "static void " cn "_telemetry_report(void) {\n"
          "  if (" cn "_telemetry_calls || " cn "_telemetry_fallbacks)\n"
          "    fprintf(stderr, \"native-telemetry id=" tid " name=" cn " calls=%llu boxed-fallbacks=%llu\\n\", " cn "_telemetry_calls, " cn "_telemetry_fallbacks);\n}\n"
          "#define " cn "_TELEMETRY_HIT() (++" cn "_telemetry_calls)\n"
          "#define " cn "_TELEMETRY_FALLBACK() (++" cn "_telemetry_fallbacks)\n"
          "#else\n#define " cn "_TELEMETRY_HIT() ((void)0)\n#define " cn "_TELEMETRY_FALLBACK() ((void)0)\n#define " cn "_telemetry_report() ((void)0)\n#endif\n"
          "static Janet " cn "_wrapper(int32_t argc, Janet *argv) {\n"
          "    if (argc != " (length pt) ") { " cn "_TELEMETRY_FALLBACK(); janet_panicf(\"native " cn
          " called with %d arguments, expected " (length pt) "\", argc); }\n"
          "    switch (janet_type(argv[" slot "])) {\n"
          arms
          "    default: break;\n"
          "    }\n"
          "    /* Phase 4: out-of-sample type at runtime -- fall back to the\n"
          "     * fully boxed interpreter definition (kept in the image under\n"
          "     <name>%boxed and registered from the mkimage prelude via\n"
          "     native/dispatch-set-boxed). Correctness here is the safety\n"
          "     property this whole tier rests on. */\n"
          "    " cn "_TELEMETRY_FALLBACK();\n"
          "    Janet fb = (NULL != n_disp_boxed)\n"
          "        ? janet_table_get(n_disp_boxed, janet_ckeywordv(\"" nm "\"))\n"
          "        : janet_wrap_nil();\n"
          "    if (!janet_checktype(fb, JANET_FUNCTION))\n"
          "        janet_panicf(\"native " cn ": untrained argument type and no boxed fallback registered\");\n"
          "    return janet_call(janet_unwrap_function(fb), argc, argv);\n"
          "}\n"
          "#undef " cn "_TELEMETRY_HIT\n#undef " cn "_TELEMETRY_FALLBACK\n"))

(defn emit-native-lib [outdir]
  # Step 12: hybrid-binary flavor. Emits native.c with the kernels PLUS
  # janet-cfunction wrappers (unbox args -> kernel -> rebox result) and a
  # native_init(JanetTable*) that registers the wrappers into the runtime
  # env. The host app calls native_init(env) after janet_core_env and
  # before running/compiling any janet code — env-resolved call sites
  # (notably eval'd code) then dispatch straight to the natives.
  (os/mkdir outdir)
  # candidates + two-round dependency resolution (same discipline as
  # emit-native-all): leaves first, then callers of emitted natives.
  # Skips print as native-skip lines (same contract as emit-native-all)
  # so --aot --native builds are diagnosable without re-running the
  # explicit-file --emit-native path (ttfx lesson: the lib path is the
  # one real builds use, and silent skips hide all signal).
  (defn describe-types-lib [pt2 rt2]
    (def qs (string/join (map (fn [t] (if (= t :unknown) "?" (string t))) pt2) " "))
    (string "params [" qs "] ret " (if (= rt2 :unknown) "?" (string rt2))))
  (def cand @{})
  # Phase 4: id -> {:slot j :types [observed keyword types]} for the
  # dispatch middle tier (filled by the classification loop below).
  (def disp-cand @{})
  (def lib-skips @[])
  (each id (range (length defs))
    (def info (get defs id))
    (def d (info :d))
    (def pt (get pts id))
    (def rt (rts id))
    (var concrete (and (> (length pt) 0) (not (d :vararg))))
    # Step 11: :array params are opaque-but-emittable (pointer temps);
    # :array returns are the (in-place) array itself
    (each t pt (unless (or (get infer/numeric-types t) (= t :array) (= t :tuple) (= t :pointer) (= t :string) (= t :keyword) (= t :symbol)) (set concrete false)))
    # Phase 1: :nil is concrete -- a whole-program-proven-always-nil
    # return is a legitimate void kernel, not an unproven boxed value.
    (unless (or (get infer/numeric-types rt) (= rt :array) (= rt :nil)) (set concrete false))
    # Hybrid wrappers replace the bytecode definition entirely. A C loop
    # cannot yield a VM quantum and later resume its native stack/locals.
    # Keep loops boxed until native continuations exist; callers also stay
    # boxed through the normal non-emitted-callee dependency check below.
    # Include self-edges, which can otherwise spin without advancing pc.
    (def loops (some (fn [ins]
                      (def op (ins 0))
                      (or (and (= op 'jmp) (<= (ins 1) 0))
                          (and (get emit-cond-jumps op) (<= (ins 2) 0))))
                    (or (d :bytecode) @[])))
    (if loops
      (array/push lib-skips [id (string (or (d :name) "?"))
                             "loop requires resumable VM quantum polls"])
      (if concrete
        (put cand id true)
        (do
          # Phase 4 middle tier: a function whose params are all
          # concrete EXCEPT exactly one slot, whose PGO-observed type
          # set is a closed, small set of scalar atom types, becomes a
          # dispatch candidate (specialized kernel per observed type +
          # mandatory boxed fallback). Everything else keeps today's
          # skip. This tier only ADDS candidates a plain build keeps
          # boxed today; statically-proven functions are unaffected.
          (var disp nil)
          (when (and (> (length pt) 0)
                     (or (get infer/numeric-types rt) (= rt :array) (= rt :nil))
                     profile-index)
            (def nm4 (string (or (d :name) "")))
            (def rows4 (get profile-index nm4))
            (when rows4
              (var unk -1)
              (var pok true)
              (var i4 0)
              (each t pt
                # the widened conflict value is :any (infer's top for
                # unprovable params); :unknown appears for never-seen
                # slots. Both mean "statically unproven" here.
                (cond (or (= t :unknown) (= t :any))
                      (if (= unk -1) (set unk i4) (set pok false))
                      (or (get infer/numeric-types t) (= t :array) (= t :tuple)
                          (= t :pointer) (= t :string) (= t :keyword) (= t :symbol))
                      nil
                      (set pok false))
                (++ i4))
              (when (and pok (>= unk 0))
                (def types4 @{})
                (var cok true)
                (each row rows4
                  (def ps4 (row :params))
                  (def slot4 (if (< unk (length ps4)) (ps4 unk) nil))
                  (if (indexed? slot4)
                    (each t slot4
                      (if (get dispatch-obs-types t)
                        (put types4 t true)
                        (set cok false)))
                    (set cok false)))
                (def n4 (length (keys types4)))
                (when (and cok (>= n4 2) (<= n4 4))
                  (set disp {:slot unk :types (sorted (keys types4))})))))
          (if disp
            (put disp-cand id disp)
            (array/push lib-skips [id (string (or (d :name) "?"))
                                   (string "types not proven: " (describe-types-lib pt rt))]))))))
  (def emitted-names @{})
  (def kernels @[])
  (def lib-skip-reason @{})
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
        (if res
          (do
            (def nm (string (d :name)))
            (put emitted-names id nm)
            (array/push kernels {:name nm :code (res :code)
                                 :externs (res :externs)
                                 :wrapper (wrapper-for nm (get pts id) (rts id) id)})
            (set changed true))
          (put lib-skip-reason id reason)))))
  (each id (filter (fn [k] (number? k)) (sort (keys cand)))
    (unless (get emitted-names id)
      (def d ((get defs id) :d))
      (array/push lib-skips [id (string (or (d :name) "?"))
                             (or (get lib-skip-reason id) "calls non-emitted native")])))
  # Phase 4 emission pass: runs AFTER the dependency rounds so branch
  # kernels can call already-emitted natives. Branch kernels are NOT
  # registered in emitted-names (other kernels refuse to call a
  # dispatcher natively -- callers stay boxed, conservative), and the
  # dispatcher itself replaces the function's wrapper under its plain
  # name. All-or-nothing per function: one failed branch => the whole
  # function stays boxed.
  (def disp-names @[])
  (each id (sort (keys disp-cand))
    (def info (get defs id))
    (def d (info :d))
    (def disp (get disp-cand id))
    (def nm (string (d :name)))
    (def pt (get pts id))
    (def rt (rts id))
    (def slot (disp :slot))
    (var branches @[])
    (var fail-reason nil)
    (each obs (disp :types)
      (def bt (dispatch-branch-type obs))
      (def bname (string nm "%" (dispatch-branch-suffix obs)))
      (def bd (merge d {:name bname}))
      (def bpt (map |(if (= $0 slot) bt $1) (range (length pt)) pt))
      (def [res reason] (emit-fn-c id bd rt bpt call-map emitted-names rts))
      (if res
        (array/push branches {:obs obs :pt bpt :cname (emit-c-name bname)
                              :code (res :code) :externs (res :externs)})
        (do (set fail-reason (string "dispatch branch " obs ": " reason))
            (break))))
    (if fail-reason
      (array/push lib-skips [id nm fail-reason])
      (do
        (array/push disp-names nm)
        (array/push kernels
                    {:name nm
                     :code (string/join (map |($0 :code) branches) "\n")
                     :externs (distinct (flatten (map |($0 :externs) branches)))
                     :wrapper (dispatch-wrapper-for nm slot branches pt rt id)
                     :dispatch true})
        (print "native-dispatch " id " " nm " slot " slot " "
               (string/join (map string (disp :types)) " ")))))
  (each [id nm r] lib-skips (print "native-skip " id " " nm " " r))
  (def cout @"")
  (buffer/push-string cout "#include <janet.h>\n#include <inttypes.h>\n#include <math.h>\n#include <stdlib.h>\n\n")
  (buffer/push-string cout "/* int32-domain bitwise helpers: boxed Janet errors outside int32\n * (integral doubles accepted); results wrap int32; shift counts mask\n * to 5 bits. Callers range/integrality-check operands (fail loud). */\nstatic int32_t k_band32(int32_t a, int32_t b) { return a & b; }\nstatic int32_t k_bor32(int32_t a, int32_t b) { return a | b; }\nstatic int32_t k_bxor32(int32_t a, int32_t b) { return a ^ b; }\nstatic int32_t k_sl32(int32_t a, int32_t b) { return (int32_t)((uint32_t)a << ((uint32_t)b & 31u)); }\nstatic int32_t k_sr32(int32_t a, int32_t b) { uint32_t n = (uint32_t)b & 31u; int32_t r = (int32_t)((uint32_t)a >> n); if ((a < 0) && (n != 0)) { r |= (int32_t)(~0u << (32u - n)); } return r; }\nstatic int32_t k_sru32(int32_t a, int32_t b) { return (int32_t)((uint32_t)a >> ((uint32_t)b & 31u)); }\n\n")
  # Phase 2/3: one static slot per distinct string/keyword/symbol
  # literal referenced by any kernel in this build, declared here and
  # materialized once in native_init below (before any wrapper is
  # reachable) -- see atom-const-name/emit-c-string-literal.
  (each key atom-const-order
    (buffer/push-string cout (string "static " (emit-c-type (key 0)) " "
                                      (get atom-const-ids key) ";\n")))
  (when (> (length atom-const-order) 0) (buffer/push-string cout "\n"))
  # Phase 4: boxed-fallback registry for dispatch wrappers. The AOT
  # image keeps the original (boxed) defn of every dispatch function
  # under <name>%boxed (mkimage rename, see native-dispatch.txt); the
  # image prelude registers it here right after the defn runs, before
  # any call site can reach the dispatcher. GC-rooted once, on first
  # registration.
  (when (not (empty? (filter |(get $0 :dispatch) kernels)))
    (buffer/push-string cout
      "static JanetTable *n_disp_boxed = NULL;\n"
      "static Janet cfun_disp_set_boxed(int32_t argc, Janet *argv) {\n"
      "    janet_fixarity(argc, 2);\n"
      "    if (NULL == n_disp_boxed) {\n"
      "        n_disp_boxed = janet_table(4);\n"
      "        janet_gcroot(janet_wrap_table(n_disp_boxed));\n"
      "    }\n"
      "    janet_table_put(n_disp_boxed, argv[0], argv[1]);\n"
      "    return janet_wrap_nil();\n"
      "}\n\n"))
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
  (buffer/push-string cout "#if JANET_NATIVE_TELEMETRY\nstatic void native_telemetry_report_all(void) {\n")
  (each k kernels (buffer/push-string cout (string "    " (emit-c-name (k :name)) "_telemetry_report();\n")))
  (buffer/push-string cout "}\n#endif\n\n")
  (buffer/push-string cout "void native_init(JanetTable *env) {\n")
  (buffer/push-string cout "#if JANET_NATIVE_TELEMETRY\n    atexit(native_telemetry_report_all);\n#endif\n")
  (each key atom-const-order
    (buffer/push-string cout
      (string "    " (get atom-const-ids key) " = "
              (get atom-const-ctors (key 0))
              "((const uint8_t*)\"" (emit-c-string-literal (key 1))
              "\", " (length (key 1)) ");\n")))
  (buffer/push-string cout "    static const JanetReg regs[] = {\n")
  (each k kernels
    (buffer/push-string cout
      (string "    { \"" (k :name) "\", " (emit-c-name (k :name)) "_wrapper, NULL },\n")))
  (when (not (empty? (filter |(get $0 :dispatch) kernels)))
    (buffer/push-string cout "    { \"dispatch-set-boxed\", cfun_disp_set_boxed, NULL },\n"))
  (buffer/push-string cout "    { NULL, NULL, NULL } };\n")
  (buffer/push-string cout "    janet_cfuns(env, \"native\", regs);\n")
  (buffer/push-string cout "    if (sizeof(regs)/sizeof(regs[0]) > 1)\n        fprintf(stderr, \"native_init: registered %d wrappers\\n\", (int) (sizeof(regs)/sizeof(regs[0]) - 1));\n")
  (buffer/push-string cout "}\n")
  (spit (string outdir "/native.c") cout)
  (def nl @"")
  (each k kernels (buffer/push-string nl (k :name)) (buffer/push-string nl "\n"))
  (spit (string outdir "/native-names.txt") nl)
  # Phase 4: dispatch function names -- mkimage keeps (rather than
  # strips) these defns, renamed to <name>%boxed, and registers them
  # into n_disp_boxed from the image prelude.
  (when (not (empty? disp-names))
    (def dnl @"")
    (each n2 disp-names (buffer/push-string dnl n2) (buffer/push-string dnl "\n"))
    (spit (string outdir "/native-dispatch.txt") dnl))
  (print "native-lib " (length kernels) " kernels " outdir))

(when emit-native-lib-dir (emit-native-lib emit-native-lib-dir))
# ---------------------------------------------------------------------------
# B7.2 Part B, Piece 1b: dce-reachable -- whole-program DCE safety gate.
# (docs/internals/b7.2-part-b-design-note.md, "Piece 1"). A NEW,
# deliberately paranoid classification layered ON TOP of the existing
# fixpoint machinery above -- `reachable-from` itself is never modified
# and never used as a safety argument: it only COMPUTES the reachable id
# set once this gate has positively cleared the whole program.
#
# The gate walks every def's bytecode linearly looking for ANY instance
# of four disqualifying patterns. Any one anywhere -> :unsafe, strip
# nothing. Only a fully clean program earns a :safe verdict plus the
# actual reachable set. V1 is allowed to strip nothing; the default for
# "not sure" is always "taint everything".
#
# Patterns:
#   1. a function value stored into a container: a function-value slot
#      used as a `put`/`puti` value, pending-pushed when a container
#      constructor (mkarr/mktup/mkbtp/mkbuf/mkstr/mkstu/mktab) consumes
#      it, or left pending across any other non-consuming, non-call op
#      (conservative: an unknown use, including `ret`).
#   2. a function value pending-pushed as an ARGUMENT at a call/tcall
#      site (the callee slot is excluded: it resolves statically via
#      record-call). Maximally conservative: any function-typed value
#      flowing into a call as an argument disqualifies -- refining to a
#      smaller safe list is future work (design-note Piece 2).
#   3. any def flagged by the EXISTING `tainted` fixpoint (any `dynsite`
#      reachable backwards over calls+contains). tainted was built
#      fail-soft for a different consumer and provably does NOT catch
#      patterns 1/2 (a fn stored via mktab produces no `dynamic` edge),
#      so it is an ADDITIONAL input, never a substitute.
#   4. any bytecode op not on the whitelist of ops this file's
#      emission/inference walks confidently handle. Default
#      disqualifying (same fail-closed doctrine as emission's
#      "unhandled op" refusal).
#
# Function-value producers tracked: `ldc` of a function/cfunction
# constant, `lds` (self reference), `clo` (closure creation). `ldu`
# (upvalue load) is NOT tracked as a producer: upvalues hold captured
# values of any type and no static type evidence exists at this level,
# so guessing would both over- and under-approximate -- documented
# limitation, flagged here rather than silently ignored.
# ---------------------------------------------------------------------------

(var dce-recognized-ops
  @{'ldi true 'ldn true 'ldt true 'ldf true 'ldc true 'ldu true 'lds true
    'clo true 'movn true 'movf true
    'add true 'sub true 'mul true 'div true 'divf true 'divim true
    'mod true 'rem true 'addim true 'subim true 'mulim true
    'band true 'bor true 'bxor true 'sl true 'sr true 'sru true
    'slim true 'srim true 'sruim true
    'lt true 'ltim true 'gt true 'gtim true 'lte true 'gte true
    'eq true 'eqim true 'neq true 'neqim true 'ltu true 'gtu true
    'cmp true 'next true 'in true 'len true 'get true 'put true 'puti true
    'jmp true 'jmpif true 'jmpno true 'jmpni true 'jmpnn true
    'call true 'tcall true 'ret true 'retn true 'err true 'res true
    'push true 'push2 true 'push3 true 'pusha true
    'mkarr true 'mktup true 'mkbtp true 'mkbuf true 'mkstr true 'mkstu true
    'mktab true})

# ops that OVERWRITE their dst slot (kills any previous function-ness).
# Mirrors emit-writers plus the loads/constructors/call it omits.
(var dce-writer-ops
  @{'ldi true 'ldn true 'ldt true 'ldf true 'ldc true 'ldu true 'lds true
    'clo true 'movn true 'movf true
    'add true 'sub true 'mul true 'div true 'divf true 'divim true
    'mod true 'rem true 'addim true 'subim true 'mulim true
    'band true 'bor true 'bxor true 'sl true 'sr true 'sru true
    'slim true 'srim true 'sruim true
    'lt true 'ltim true 'gt true 'gtim true 'lte true 'gte true
    'eq true 'eqim true 'neq true 'neqim true 'ltu true 'gtu true
    'cmp true 'next true 'in true 'len true 'get true
    'call true 'tcall true
    'mkarr true 'mktup true 'mkbtp true 'mkbuf true 'mkstr true 'mkstu true
    'mktab true})

(defn dce-scan-def [id]
  # returns nil, or [pattern pc description]
  (def info (defs id))
  (def d (info :d))
  (def code (or (d :bytecode) @[]))
  (def consts (or (d :constants) @[]))
  (var fnslots @{})   # slot -> true when it holds a function value
  # pending snapshots FN-NESS AT PUSH TIME (not live slot lookups):
  # the callee slot at a call may alias an arg slot, and slots get
  # reused/rewritten between the push and the call -- the property that
  # matters is what the pushed VALUE was when it was pushed.
  (var pending @[])
  (var bad nil)
  (defn pending-fn? []
    (var found false)
    (each s pending (when s (set found true)))
    found)
  (var pc 0)
  (while (and (< pc (length code)) (nil? bad))
    (def ins (code pc))
    (def op (ins 0))
    (cond
      # Pattern 4: unrecognized op anywhere -> whole program unsafe.
      (nil? (get dce-recognized-ops op))
      (set bad [4 pc (string "unrecognized op " op)])

      # Producers. ldc KILLS its dst first: slots are reused, and a
      # stale fn-ness from an earlier producer on the same slot must
      # not survive a re-load of a non-function constant into it.
      # (clo/lds always produce a function, so setting true is
      # already kill+set for them.)
      (= op 'ldc)
      (do (put fnslots (ins 1) nil)
        (def c (consts (ins 2)))
        (when (or (= :function (type c)) (= :cfunction (type c)))
          (put fnslots (ins 1) true)))

      (= op 'clo) (put fnslots (ins 1) true)
      (= op 'lds) (put fnslots (ins 1) true)

      # movn: dst <- src; movf: src <- dst (copies propagate fn-ness)
      (= op 'movn)
      (when (get fnslots (ins 2)) (put fnslots (ins 1) true))
      (= op 'movf)
      (when (get fnslots (ins 1)) (put fnslots (ins 2) true))

      # Pattern 1, direct stores: fn value put into a container.
      (and (= op 'put) (get fnslots (ins 3)))
      (set bad [1 pc (string "function value in slot s" (ins 3)
                             " stored into container via put")])
      (and (= op 'puti) (get fnslots (ins 2)))
      (set bad [1 pc (string "function value in slot s" (ins 2)
                             " stored into container via puti")])

      # Pending pushes: classified by their next consumer. The pending
      # entry is the slot's fn-ness SNAPSHOT at push time.
      (or (= op 'push) (= op 'push2) (= op 'push3))
      (do
        (array/push pending (get fnslots (ins 1)))
        (when (or (= op 'push2) (= op 'push3))
          (array/push pending (get fnslots (ins 2))))
        (when (= op 'push3) (array/push pending (get fnslots (ins 3)))))
      (= op 'pusha)
      (do (array/push pending (get fnslots (ins 1)))
        # spread push: unknown arity/uses, conservatively treat any fn
        # here as immediately pattern-1
        (when (get fnslots (ins 1))
          (set bad [1 pc (string "function value in slot s" (ins 1)
                                 " spread-pushed (pusha)")])))

      # Call sites: pattern 2 when a fn value is a pending ARGUMENT
      # (callee slot is not a push and never reaches `pending`).
      (or (= op 'call) (= op 'tcall))
      (do (when (pending-fn?)
          (set bad [2 pc "function value pending as call argument"]))
        (set pending @[]))

      # Container constructors consuming pending: pattern 1.
      (or (= op 'mkarr) (= op 'mktup) (= op 'mkbtp) (= op 'mkbuf)
          (= op 'mkstr) (= op 'mkstu) (= op 'mktab))
      (do (when (pending-fn?)
          (set bad [1 pc (string "function value pending into " op
                                 " (stored in container)")]))
        (set pending @[]))

      # Any other op with a fn value still pending across it: a
      # non-consuming, non-call use -- conservative pattern 1 (covers
      # `ret` of a fn value: the value escapes the def).
      (when (pending-fn?)
        (set bad [1 pc (string "function value pending across non-consuming op "
                               op)])))

    # Kill: any op that overwrites a dst slot destroys fn-ness there.
    # (movn/movf are handled as copies in the dispatch above; clo/lds/
    # ldc are the fn-value PRODUCERS -- their own clause already decided
    # their dst's fn-ness, and a same-iteration kill would wipe it.)
    (when (and (nil? bad) (get dce-writer-ops op)
               (not (= op 'movn)) (not (= op 'movf))
               (not (= op 'clo)) (not (= op 'lds)) (not (= op 'ldc)))
      (put fnslots (ins 1) nil))
    (++ pc))
  bad)

(var dce-verdict :safe)
(var dce-bad-def -1)
(var dce-bad-desc "")
(var dce-bad-pc -1)
(var dce-bad-pattern 0)
(when (dyn :dce-debug)
  (each i (range (length defs))
    (print "dcebc " i " " (string/format "%q" (or (((defs i) :d) :bytecode) @[])))))
# Pattern 4 + 1 + 2: scan every def's bytecode.
(var dce-i 0)
(while (and (= :safe dce-verdict) (< dce-i (length defs)))
  (def v (dce-scan-def dce-i))
  (when v
    (set dce-verdict :unsafe)
    (set dce-bad-def dce-i)
    (set dce-bad-pattern (v 0))
    (set dce-bad-pc (v 1))
    (set dce-bad-desc (v 2)))
  (++ dce-i))
# Pattern 3: any dynsite anywhere (the existing taint fixpoint is
# non-empty iff some def reaches a dynamic site) -> strip nothing.
(when (and (= :safe dce-verdict) (not (empty? dynsite)))
  (set dce-verdict :unsafe)
  (set dce-bad-pattern 3)
  (set dce-bad-def (first (sort (keys dynsite))))
  (set dce-bad-pc -1)
  (set dce-bad-desc "dynamic call site (dynsite, existing taint fixpoint)"))
(if (= :unsafe dce-verdict)
  (print "dce unsafe pattern " dce-bad-pattern
         " def " dce-bad-def " (" ((defs dce-bad-def) :name) ")"
         " pc " dce-bad-pc ": " dce-bad-desc
         " -- stripping nothing")
  (do
    # Whole program cleared: the existing reachability computation is
    # trustworthy for computing the concrete reachable set.
    (def dce-reach (reachable-from roots))
    (def dce-excluded @[])
    (var dce-ri 0)
    (each info defs
      (when (nil? (get dce-reach dce-ri))
        (array/push dce-excluded (string dce-ri ":" (info :name))))
      (++ dce-ri))
    (print "dce safe reachable "
           (if (empty? dce-reach) "-" (string/join (map string (sorted (keys dce-reach))) " "))
           " excluded "
           (if (empty? dce-excluded) "-" (string/join dce-excluded " ")))
    # --- B7.2 Part B, Piece 1c: emit the verdict for mkimage.janet ---
    # Written ONLY on a :safe verdict and ONLY under --dce. On :unsafe
    # the write is skipped entirely, so an absent file means exactly the
    # same thing as "--dce was never passed": mkimage strips nothing.
    # That equivalence is the design note's "V1 is allowed to strip
    # nothing" principle made mechanical rather than documented.
    #
    # mkimage.janet matches top-level `defn`/`defn-` forms BY NAME, so
    # only names that are unambiguously safe to match that way are
    # emitted. Three filters, all fail-closed:
    #   - "thunk" is never emitted (it is a unit entry, not a defn).
    #   - a generated "<anon-N>" name has no source-level defn form.
    #   - a name owned by MORE than one def id is dropped unless EVERY
    #     id carrying that name is excluded; otherwise a name match in
    #     mkimage would delete a reachable homonym. `name-ids` (built
    #     at registration) is the authority for that ownership check.
    (when (and dce-emit emit-native-lib-dir)
      (def dce-excluded-ids @{})
      (var dce-xi 0)
      (each info defs
        (when (nil? (get dce-reach dce-xi)) (put dce-excluded-ids dce-xi true))
        (++ dce-xi))
      (def dce-names @[])
      (each nm (sorted (keys name-ids))
        (def ids (get name-ids nm))
        (when (and (not= nm "thunk")
                   (not (string/has-prefix? "<anon-" nm))
                   (all (fn [one] (get dce-excluded-ids one)) ids))
          (array/push dce-names nm)))
      (def dce-buf @"")
      (each nm dce-names
        (buffer/push-string dce-buf nm)
        (buffer/push-string dce-buf "\n"))
      (spit (string emit-native-lib-dir "/dce-excluded.txt") (string dce-buf))
      (print "dce emit " (length dce-names) " excluded names "
             emit-native-lib-dir "/dce-excluded.txt"))))
