# tools/infer.janet — shared type inference (Phase 4 steps 6-7).
# Analyzer model (bind_args_params / slot_take):
#   - at every call site, each pushed argument's type is bound
#     (monotonically narrowed) into the callee's parameter slot;
#   - annotation-seeded params/returns are contracts and are never
#     weakened (seeded widening);
#   - conflicting call sites widen to :number (polymorphic) — boxed;
#   - return types flow out of RETURN sites and through tail calls,
#     whole-program, until the fixpoint stabilizes.
# Lattice: :unknown (boxed top) > :number > {:long, :double}; leaves
# :bool/:nil. Everything conservative: unknown wins ties, inference
# only ever refines toward concrete numeric types.

# The full C primitive width set joins the numeric lattice; family
# normalization folds widths to int64/float64 for arithmetic joining —
# exact widths are FFI-boundary contracts (params/returns), computation
# runs at the kernel's declared return width.
(def numeric-types
  @{:long true :double true :number true
    :i32 true :i16 true :i8 true
    :u64 true :u32 true :u16 true :u8 true
    :float true})
(def width-family
  @{:i32 :long :i16 :long :i8 :long
    :u64 :long :u32 :long :u16 :long :u8 :long
    :float :double})

# int-family predicate (mirrors callgraph's int-hints table; :number
# is deliberately excluded — unknown int-or-float stays fail-closed)
(defn int-family? [t] (or (= t :long) (= :long (get width-family t))))

(defn type-join [a0 b0]
  # widths normalize to their family before joining
  (def a (get width-family a0 a0))
  (def b (get width-family b0 b0))
  (cond
    (= a b) a
    (= a :unknown) b
    (= b :unknown) a
    (and (get numeric-types a) (get numeric-types b))
    (if (or (= a :number) (= b :number))
      :number
      (if (or (= a :double) (= b :double)) :double :long))
    :unknown))

(defn arith-join [t1 t2]
  (if (and (get numeric-types t1) (get numeric-types t2))
    (type-join t1 t2)
    :unknown))

(defn bit-join [t1 t2]
  # int32-domain: both sides numeric gives :long. :number operands are
  # sound because emission inserts integrality + int32-range checks
  # that abort exactly where boxed errors (fail loud, like OOB).
  (if (and (get numeric-types t1) (get numeric-types t2))
    :long
    :unknown))

# intersection-style slot_take: refine cur toward new; disagreement on
# concrete widths widens to :number; non-numeric involvement gives up
(defn narrow [cur new]
  (cond
    # Unlike :unknown (no evidence yet), a conflicting call-site union
    # must remain boxed across later calls and fixpoint rounds.
    (or (= cur :any) (= new :any)) :any
    (= cur new) cur
    (= cur :unknown) new
    (= new :unknown) cur
    (= cur :number) (if (get numeric-types new) new :any)
    (= new :number) (if (get numeric-types cur) cur :any)
    (and (get numeric-types cur) (get numeric-types new)) :number
    :any))

(def arith-ops
  @{'add true 'sub true 'mul true 'divf true
    'mod true 'rem true})
# Real division: boxed (/) always produces a double, so div/divim
# result slots type :double (the emitter only ever emits them as
# float kernels -- int widths refuse). Typing them int-family let
# call-site widening narrow a callee's :number-seeded param to :long,
# and a fractional value then wrapped in an int64 kernel (caught by
# the parity gate as PGO PARITY FAILED on an unhinted numeric program).
(def div-ops
  @{'div true 'divim true})
(def arith-im-ops
  @{'addim true 'subim true 'mulim true})
# Bitwise ops: int32-domain (boxed errors outside int32, wraps inside).
# Join rule is int-family-only -- a :double/:number operand refuses
# (unknown), since C would silently truncate where Janet errors.
# Emission inserts int32-range + integrality checks (fail loud).
(def bit-ops
  @{'band true 'bor true 'bxor true 'sl true 'sr true 'sru true})
(def bit-im-ops
  @{'slim true 'srim true 'sruim true
    # unsigned-immediate bitwise: same int-family join rule as the
    # register forms above
    'bandim true 'borim true 'bxorim true})
# Pure core calls: math libm wrappers with exact C equivalents
# (JANET_DEFINE_MATHOP applies the libm fn to the unwrapped double).
# Callgraph records call sites by core name (ldc-loaded cfunctions);
# scan-def types them, emission lowers to libm. Arity mismatches stay
# :unknown (fail-closed). Args code-gen through (double) casts, exact
# for |v| <= 2^53 like all int kernels; beyond that the documented
# int-width caveat applies.
(def pure-calls
  @{"math/floor" {:c "floor" :params [:double] :returns :double}
    "math/ceil" {:c "ceil" :params [:double] :returns :double}
    "math/sqrt" {:c "sqrt" :params [:double] :returns :double}
    "math/abs" {:c "fabs" :params [:double] :returns :double}
    "math/pow" {:c "pow" :params [:double :double] :returns :double}})

# ---------------------------------------------------------------------------
# Phase 5 (type-generator plan): closed-shape LOCAL table detection.
# Decision rule, stated precisely before the code (same discipline as
# the lattice-widening comments below):
# A table constructed by `mktab` in function F is CLOSED-SHAPE (a
# candidate for native flat-struct flattening in callgraph.janet) iff
# ALL of the following hold:
#   1. CONSTRUCTION: at the mktab, the pending args are
#      [k1 v1 ... kn vn], n >= 1, every ki a KEYWORD CONSTANT (an
#      ldc-loaded keyword still live in its slot), no duplicate keys,
#      and every vi's inferred type exactly :long or :double (the
#      existing :kernel :pread/:pwrite field contract; :number /
#      :string / composites keep the table boxed).
#   2. SINGLE CONSTRUCTION: the mktab sits in the ENTRY basic block
#      (before any branch/terminator, so every later use is dominated
#      by it), and its dest slot is never written again by anything
#      except a shape-preserving movn/movf or a valid constant-key
#      `put`. ANY other write -- including a SECOND mktab on the same
#      slot, even a shape-identical one (two construction sites
#      reaching one use is deliberately out of this first pass's
#      scope) -- opens the slot STICKY: once open, never re-closed,
#      exactly like a widened lattice value.
#   3. USES: the shaped slot (and every slot it movn/movf's into,
#      which inherit the shape and the same rules) may be read ONLY
#      as (a) callee of an UNRESOLVED call/tcall with exactly one
#      pending keyword constant that is a shape key (pread), (b) the
#      dst of `put` whose key slot holds a shape-key keyword constant
#      and whose value slot's type is exactly the field type (:long
#      into a :double field also allowed -- exact for |v| <= 2^53 per
#      the documented int-width caveat; never double into :long), or
#      (c) the source of a movn/movf. ANY other read -- pushed as a
#      call argument (this is how escape to pairs/keys/values/generic
#      reflection and unknown callees materializes), returned,
#      arithmetized, compared, branched on, indexed, spread -- opens
#      the slot sticky.
# Consequence: a slot that survives the whole scan is provably only
# ever accessed via known-constant-key get/put inside its own function
# and never escapes it; ANY doubt keeps the table fully boxed. Never
# force a wrong layout. This is intentionally NARROWER than the plan's
# eventual whole-program shape tracking: single function, single
# construction site, entry block only. Structs ({...} -> mkstu) are
# not tracked in this first pass.
# ---------------------------------------------------------------------------


(def cmp-ops
  @{'eq true 'eqim true 'lt true 'ltim true 'gt true 'gtim true
    'lte true 'gte true
    'neq true 'neqim true 'ltu true 'gtu true 'cmp true 'next true})

# dst-slot position per op (mirrors callgraph's emit-writers, plus the
# loads/constructors it omits). Ops absent here write nothing.
(def shape-dst-ops
  @{'ldi 1 'ldc 1 'ldn 1 'ldt 1 'ldf 1 'len 1 'get 1 'put 1 'puti 1
    'call 1 'movn 1 'movf 2 'ldu 1
    'mkarr 1 'mktup 1 'mkbtp 1 'mkbuf 1 'mkstr 1 'mkstu 1 'mktab 1})

(defn shape-reads [op ins]
  # operand slots READ by op (dst excluded; push*/ret/cond-jumps read
  # the slots they name; mktab reads its pending -- covered by the
  # push-time escape rule). Returns an array (possibly empty).
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
    (or (get arith-ops op) (get bit-ops op) (get cmp-ops op)
        (get div-ops op)) @[(ins 2) (ins 3)]
    (or (get arith-im-ops op) (get bit-im-ops op)) @[(ins 2)]
    @[]))

(defn const-num-type [d cidx]
  # Preserve concrete nonnumeric evidence: :unknown means unseen, not
  # a boxed value that may be ignored while specializing a callee.
  (def consts (or (get d :constants) @[]))
  (if (>= cidx (length consts))
    :unknown
    (do (def c (consts cidx))
      (if (= :number (type c))
        (if (= c (math/trunc c)) :long :double)
        (type c)))))

(defn analyze [defs call-map &opt profile coremap]
  # defs: array of @{:d disasm :hint ...}; call-map: id -> {pc -> tid};
  # profile: @{name -> [{:params [...] :mins [...] :maxs [...] :calls n}]}
  # from a PGO dump; coremap: id -> {pc -> core cfunction name} for
  # pure-call shaping (may be nil). Rows may carry :from (:home/:worker
  # for Phase C provenance; absent means :home).
  # returns @{:pts pts :rts rts :nnp nnp :rounds rounds :pprov pprov}; nnp maps
  # id -> array of bool (PGO-unanimous observed min >= 0 per param).
  # pprov maps id -> per-slot provenance: nil (no rows / silent),
  # @{:seed t :from [...]}, or @{:conflict @{t [@[from...]]}}.
  (def n (length defs))
  (def pts @{})
  (def rts @[])
  (def nnp @{})
  (def pprov @{})
  (def pfixed @{})
  (def rfixed @{})
  # Phase 5 outputs (see the block comment at shape-dst-ops):
  #   tshapes id -> {slot -> shape {:fields :order :base}}
  #   taccs   id -> {pc -> {:kind :pread/:pwrite :slot :field :type}}
  #   topened id -> {slot -> true}  (sticky-opened, for reporting)
  (def tshapes @{})
  (def taccs @{})
  (def topened @{})
  (each id (range n)
    (def d ((get defs id) :d))
    (def h (get (get defs id) :hint))
    (def arity (or (get d :arity) 0))
    (def parr (array/new-filled arity :unknown))
    (def nnarr (array/new-filled arity false))
    (def hp (if h (get h :params) nil))
    (when hp
      (var i 0)
      (each p hp
        # Step 11: :array params are opaque-but-known (pointer, never
        # arithmetized); annotation-seeded either way (pfixed below).
        # :tuple (fixed positional struct-thing) and :pointer (native
        # memory, never arithmetized) are opaque the same way.
        (when (< i arity)
          (put parr i (if (or (get numeric-types p) (= p :array)
                              (= p :tuple) (= p :pointer)) p :unknown)))
        (++ i))
      (put pfixed id true))
    (put pts id parr)
    # PGO seeding: unhinted params take unanimous observations as a
    # starting point (never pfixed -- later conflicts widen soundly).
    # A slot seeds only if every matching row agrees on exactly one
    # type; unknown/empty slots stay unknown for the fixpoint.
    # Range seeding rides along for ALL fns (hinted or not): unanimous
    # observed min >= 0 marks the param nonneg (nnp), consumed by int
    # div/mod shaping. Like types, never pfixed -- any negative
    # observation withholds the mark.
    (when profile
      (def rows (get profile (string (or ((get defs id) :name) ""))))
      (def prov @[])
      (when rows
        (var i 0)
        (while (< i arity)
          (var agree nil)
          (var ok true)
          (var nonneg true)
          (def seen @{})
          (each row rows
            (def from (get row :from :home))
            (def row-froms (if (indexed? from) from [from]))
            (def type-from (get (get row :type-from @[]) i))
            (def ps (get row :params @[]))
            (def slot (if (< i (length ps)) (ps i) nil))
            (if (and (indexed? slot) (= 1 (length slot)) (keyword? (slot 0)))
              (do (def t (slot 0))
                (each origin (or (get type-from t) row-froms)
                  (array/push (or (get seen t) (do (def a @[]) (put seen t a) a)) origin))
                (if (nil? agree) (set agree t)
                  (when (not= agree t) (set ok false))))
              (do (set ok false)
                 (when (indexed? slot)
                   (each t slot
                     (when (keyword? t)
                       (each origin (or (get type-from t) row-froms)
                        (array/push (or (get seen t) (do (def a @[]) (put seen t a) a)) origin)))))))
            (def mns (get row :mins @[]))
            (def mn (if (< i (length mns)) (mns i) nil))
            (unless (and (number? mn) (>= mn 0)) (set nonneg false)))
          (when (and (nil? hp) ok (not (nil? agree)))
            (put parr i agree)
            # Phase C provenance: unanimous rows' origins (deduped, sorted).
            (def froms @[])
            (each [_ fs] (pairs seen) (each f fs (array/push froms f)))
            (array/push prov @{:seed agree :from (sorted (distinct froms))}))
          (when (and (nil? hp) (not ok) (> (length (keys seen)) 1))
            # Genuine type conflict (malformed slots stay silent): per-type origins.
            (def cmap @{})
            (each [t fs] (pairs seen)
              (put cmap t (sorted (distinct fs))))
            (array/push prov @{:conflict cmap}))
          (when (and (not ok) (<= (length (keys seen)) 1))
            (array/push prov nil))
          (when (and ok (nil? agree))
            (array/push prov nil))
          (when nonneg (put nnarr i true))
          (++ i)))
      (put pprov id prov))
    (put nnp id nnarr)
    (def hr (if h (get h :returns) nil))
    (array/push rts (if (or (get numeric-types hr) (= hr :array)) hr :unknown))
    (when (or (get numeric-types hr) (= hr :array)) (put rfixed id true)))
  (def box @{:changed true})
  (defn scan-def [id]
    # single forward pass; returns ret candidate; applies param binds
    (def info (get defs id))
    (def d (info :d))
    (def code (or (get d :bytecode) @[]))
    (def cmap (or (get call-map id) @{}))
    (def st @{})
    (def parr (get pts id))
    (var pi 0)
    (each t parr
      (put st pi t)
      (++ pi))
    (var pending @[])
    (var spread false)
    (var rtype :unknown)
    (var pc 0)
    # Phase 5 closed-shape state (see the block comment at
    # shape-dst-ops for the full decision rule):
    #   kconst     slot -> keyword constant from the last 'ldc writing
    #              it (nil once anything else writes the slot) -- this
    #              is how "known-constant-key" access is proven;
    #   pending-kw parallel to pending: per-arg keyword constant or nil;
    #   tshapes-f  slot -> shape {:fields {kw :long|:double}
    #              :order [kw...] :base <mktab slot>} for slots that
    #              currently hold a closed-shape table;
    #   opened     slot -> true, STICKY (a slot once opened is never
    #              re-closed -- lattice-widening discipline);
    #   tacc-f     pc -> {:kind :pread/:pwrite :slot :field :type} for
    #              every validated constant-key access to a shaped
    #              slot (emission's per-site decision table, mirroring
    #              how call-map drives call emission).
    (def kconst @{})
    (var pending-kw @[])
    (def tshapes-f @{})
    (def opened @{})
    (def tacc-f @{})
    # entry-block boundary: first terminator pc. Only an mktab strictly
    # before ANY branch/terminator is dominated-by-entry on every path
    # to a later use (rule 2); anything later never becomes a shape.
    (var entry-end (length code))
    (var epc 0)
    (each eins code
      (def eop (eins 0))
      (when (or (= eop 'jmp) (= eop 'jmpif) (= eop 'jmpno)
                (= eop 'jmpni) (= eop 'jmpnn) (= eop 'ret)
                (= eop 'retn) (= eop 'err) (= eop 'tcall))
        (set entry-end epc)
        (break))
      (++ epc))
    (defn shape-open [slot]
      # sticky: never re-closed once opened. The value records the pc
      # that opened it (diagnostic: table-open reports report it).
      (put tshapes-f slot nil)
      (put opened slot pc))
    (each ins code
      (def op (ins 0))
      # Phase 5 pre-pass: runs BEFORE the lattice cond so mktab/call
      # still see the pending list of the PREVIOUS instructions, and so
      # escapes can be detected no matter which lattice arm matches.
      (def shdst (get shape-dst-ops op))
      (cond
        (= op 'mktab)
        (do
          (def mkslot (ins 1))

          (if (or (get tshapes-f mkslot) (get opened mkslot)
                  (>= pc entry-end))
            # rule 2: a second construction site on this slot (even a
            # shape-identical one) or a construction outside the entry
            # block -- out of this pass's proven scope, open sticky
            (shape-open mkslot)
            (do
              (var mkshape nil)
              (when (and (not spread) (even? (length pending))
                         (>= (length pending) 2))
                (var ok true)
                (def kws @[])
                (def flds @{})
                (var i 0)
                (while (< i (length pending))
                  (def k (pending-kw i))
                  (def vt (pending (+ i 1)))
                  (unless (and (keyword? k) (nil? (get flds k))
                               (or (= vt :long) (= vt :double)))
                    (set ok false))
                  (when ok
                    (array/push kws k)
                    (put flds k vt))
                  (+= i 2))
                (when ok (set mkshape {:fields flds :order kws :base mkslot})))
              (when mkshape (put tshapes-f mkslot mkshape)))))
        (= op 'movn)
        (do (def src (ins 2)) (def dsts (ins 1))
          (cond
            (get tshapes-f src)
            (if (or (get opened dsts) (get tshapes-f dsts))
              (shape-open dsts)
              (put tshapes-f dsts (get tshapes-f src)))
            (get opened src) (shape-open dsts)
            nil))
        (= op 'movf)
        (do (def src (ins 1)) (def dsts (ins 2))
          (cond
            (get tshapes-f src)
            (if (or (get opened dsts) (get tshapes-f dsts))
              (shape-open dsts)
              (put tshapes-f dsts (get tshapes-f src)))
            (get opened src) (shape-open dsts)
            nil))
        (= op 'put)
        (do
          # rule 3b: constant-key pwrite into a shaped slot; anything
          # else (unknown key, non-shape key, wrong value type, or the
          # VALUE itself being a shaped table -> escape) opens it
          (when (get tshapes-f (ins 3)) (shape-open (ins 3)))
          (when (get tshapes-f (ins 1))
            (def pdst (ins 1))
            (def pk (get kconst (ins 2)))
            (def pvt (get st (ins 3) :unknown))
            (def pft (when pk (get (get (get tshapes-f pdst) :fields) pk)))
            (if (and pft (or (= pvt pft)
                             (and (= pft :double) (= pvt :long))))
              (put tacc-f pc {:kind :pwrite :slot pdst
                              :base ((get tshapes-f pdst) :base)
                              :field pk :type pft})
              (shape-open pdst))))
        (or (= op 'push) (= op 'push2) (= op 'push3) (= op 'pusha))
        # rule 3: pushing a shaped slot is an escape (call argument /
        # spread; generic reflection like pairs/keys reaches tables
        # exactly this way) -- open sticky
        (each rs (shape-reads op ins)
          (when (get tshapes-f rs) (shape-open rs)))
        (or (= op 'call) (= op 'tcall))
        (do
          (def callee (if (= op 'call) (ins 2) (ins 1)))
          (when (or (get tshapes-f callee) (get opened callee))
            (def ck (when (and (not spread) (= 1 (length pending)))
                      (pending-kw 0)))
            (def cft (when ck (get (get (get tshapes-f callee) :fields) ck)))
            (if (and (nil? (get cmap pc)) ck cft)
              (put tacc-f pc {:kind :pread :slot callee
                              :base ((get tshapes-f callee) :base)
                              :field ck :type cft})
              # rule 3a failed: unknown callee resolution, wrong arity,
              # non-constant or non-shape key -- escape, open sticky
              (shape-open callee))))
        # every other op: any read of a shaped slot is a use as a VALUE
        # (arith/cmp operand, condition, get base or index, len, ret)
        # -> open sticky (rule 3)
        (each rs (shape-reads op ins)
          (when (get tshapes-f rs) (shape-open rs))))
      # kconst maintenance: only an 'ldc of a keyword constant leaves
      # constant-key evidence in a slot; every other write clears it
      (when shdst
        (def wcv (and (= op 'ldc)
                      (get (or (get d :constants) @[]) (ins 2))))
        (put kconst (ins shdst) (if (keyword? wcv) wcv nil)))
      (cond
        (= op 'ldi) (put st (ins 1) :long)
        (= op 'ldc) (put st (ins 1) (const-num-type d (ins 2)))
        (= op 'ldn) (put st (ins 1) :nil)
        (or (= op 'ldt) (= op 'ldf)) (put st (ins 1) :bool)
        (= op 'len) (put st (ins 1) :long)
        (= op 'movn) (put st (ins 1) (get st (ins 2) :unknown))
        (= op 'movf) (put st (ins 2) (get st (ins 1) :unknown))
        (get arith-im-ops op)
        (put st (ins 1) (arith-join (get st (ins 2) :unknown) :long))
        (get div-ops op)
        (put st (ins 1) :double)
        (get arith-ops op)
        (put st (ins 1)
             (arith-join (get st (ins 2) :unknown) (get st (ins 3) :unknown)))
        (get bit-im-ops op)
        (put st (ins 1) (bit-join (get st (ins 2) :unknown) :long))
        (get bit-ops op)
        (put st (ins 1)
             (bit-join (get st (ins 2) :unknown) (get st (ins 3) :unknown)))
        (get cmp-ops op) (put st (ins 1) :bool)
        # literal constructors: arrays/tuples precise; the rest :unknown
        # (always sound -- kills stale slot types that otherwise leak
        # into call-site widening as phantom :longs). They also consume
        # the pending pushes (VM takes [stackstart, stacktop)); the
        # compiler never interleaves call args across them. Single
        # clause each: cond stops at the first match.
        (or (= op 'mkarr))
        (do (put st (ins 1) :array) (set pending @[]) (set pending-kw @[]))
        (or (= op 'mktup) (= op 'mkbtp))
        (do (put st (ins 1) :tuple) (set pending @[]) (set pending-kw @[]))
        (or (= op 'mkbuf) (= op 'mkstr) (= op 'mkstu) (= op 'mktab))
        (do (put st (ins 1) :unknown) (set pending @[]) (set pending-kw @[]))
        # ldu (load upvalue: same-unit fn refs, closed-over vars).
        # Opaque :unknown -- any concrete use refuses downstream. The
        # load itself is dead in emitted code (static calls go direct
        # by name); emission skips it explicitly.
        (= op 'ldu) (put st (ins 1) :unknown)
        # Step 11: array access. get needs an :array base and an
        # int-family or :number index (-> :number; :number indices
        # carry a runtime integrality guard at emission, fail-loud).
        # Anything else is :unknown (fail-closed: tables/strings stay
        # boxed). put/puti never change the container type (array-ness
        # is preserved by construction; element risk is runtime-gated
        # by unwrap, and unprovable stores refuse at emission).
        (= op 'get)
        (put st (ins 1)
             (if (and (= :array (get st (ins 2) :unknown))
                      (or (int-family? (get st (ins 3) :unknown))
                          (= :number (get st (ins 3) :unknown))))
               :number :unknown))
        (= op 'push)
        (do (array/push pending (get st (ins 1) :unknown))
          (array/push pending-kw (get kconst (ins 1))))
        (= op 'push2)
        (do (array/push pending (get st (ins 1) :unknown))
          (array/push pending-kw (get kconst (ins 1)))
          (array/push pending (get st (ins 2) :unknown))
          (array/push pending-kw (get kconst (ins 2))))
        (= op 'push3)
        (do (array/push pending (get st (ins 1) :unknown))
          (array/push pending-kw (get kconst (ins 1)))
          (array/push pending (get st (ins 2) :unknown))
          (array/push pending-kw (get kconst (ins 2)))
          (array/push pending (get st (ins 3) :unknown))
          (array/push pending-kw (get kconst (ins 3))))
        (= op 'pusha) (set spread true)
        (= op 'call)
        (do (def tid (get cmap pc))
          # Unresolved call (callee is an opaque struct/table/function
          # slot, or a spread call): the VM always writes dst, and the
          # result type is unknowable -- seed dst :unknown so the slot's
          # pre-call type (notably the :keyword constant of a call-shaped
          # struct access `ldc :k; push; call`) cannot leak through as a
          # phantom type. Matched rules below overwrite this default.
          (put st (ins 1) :unknown)
          (when (and tid (number? tid))
            # call-site widening: bind each pushed arg into the callee's
            # param slot (annotation-seeded params are never touched)
            (unless spread
              (def callee-pts (get pts tid))
              (def cf (get pfixed tid))
              (var i 0)
              (each a pending
                (when (< i (length callee-pts))
                  (unless cf
                    (def nn (narrow (callee-pts i) a))
                    (when (not= nn (callee-pts i))
                      (put callee-pts i nn)
                      (put box :changed true)))
                  (++ i))))
            (when (number? tid) (put st (ins 1) (rts tid))))
          # call-shaped array/tuple access: (a i) compiles to
          # push+tcall/call with the array as callee (tcall carries no
          # dst; call's dst is ins 1, callee ins 2). Same rule as get:
          # int-family or :number index into :array/:tuple -> :number
          # (:number indices carry a runtime integrality guard at
          # emission, fail-loud, like get/OOB).
          (when (and (nil? tid) (not spread) (= 1 (length pending)))
            (def callee-ty (get st (ins 2) :unknown))
            (when (and (or (= callee-ty :array) (= callee-ty :tuple))
                       (or (int-family? (pending 0)) (= :number (pending 0))))
              (put st (ins 1) :number)))
          # pure core calls (math/floor etc., resolved from ldc-loaded
          # cfunction constants): exact libm types when arity matches
          # and every pushed arg is numeric; else :unknown (fail-closed).
          (when (and (nil? tid) (not spread))
            (def pname (get (get coremap id) pc))
            (def pspec (and pname (get pure-calls pname)))
            (when (and pspec (= (length pending) (length (pspec :params))))
              (var allnum true)
              (each a pending
                (unless (get numeric-types a) (set allnum false)))
              (when allnum (put st (ins 1) (pspec :returns)))))
          (set pending @[])
          (set pending-kw @[])
          (set spread false))
        (= op 'tcall)
        (do (def tid (get cmap pc))
          (when (and tid (number? tid))
            (unless spread
              (def callee-pts (get pts tid))
              (def cf (get pfixed tid))
              (var i 0)
              (each a pending
                (when (< i (length callee-pts))
                  (unless cf
                    (def nn (narrow (callee-pts i) a))
                    (when (not= nn (callee-pts i))
                      (put callee-pts i nn)
                      (put box :changed true)))
                  (++ i))))
            (when (number? tid)
              (set rtype (type-join rtype (rts tid)))))
          # call-shaped access in tail position: (a i) as the return
          # value. tcall carries only the callee (ins 1); same rule
          # (int-family or :number index) as the call arm above.
          (when (and (nil? tid) (not spread) (= 1 (length pending)))
            (def callee-ty (get st (ins 1) :unknown))
            (when (and (or (= callee-ty :array) (= callee-ty :tuple))
                       (or (int-family? (pending 0)) (= :number (pending 0))))
              (set rtype (type-join rtype :number))))
          # pure core calls in tail position: same contract as above.
          (when (and (nil? tid) (not spread))
            (def pname (get (get coremap id) pc))
            (def pspec (and pname (get pure-calls pname)))
            (when (and pspec (= (length pending) (length (pspec :params))))
              (var allnum true)
              (each a pending
                (unless (get numeric-types a) (set allnum false)))
              (when allnum (set rtype (type-join rtype (pspec :returns))))))
          (set pending @[])
          (set pending-kw @[])
          (set spread false))
        (= op 'ret) (set rtype (type-join rtype (get st (ins 1) :unknown)))
        (= op 'retn) (set rtype (type-join rtype :nil))
        nil)
      # Phase 5: a validated constant-key table read (pread) gives the
      # call dest / tail return a CONCRETE type -- the field's -- which
      # flows into the ordinary lattice from here (this is what makes a
      # table-consuming function's return type provable at all).
      (def pta (when (or (= op 'call) (= op 'tcall)) (get tacc-f pc)))
      (when (and pta (= (pta :kind) :pread))
        (if (= op 'call)
          (put st (ins 1) (pta :type))
          (set rtype (type-join rtype (pta :type)))))
      (++ pc))
    # export per-scan shape state (recomputed every fixpoint round; the
    # final round's values -- computed at the stabilized pts -- are what
    # analyze returns, exactly like rtype itself)
    (put tshapes id tshapes-f)
    (put taccs id tacc-f)
    (put topened id opened)
    rtype)
  (var rounds 0)
  (while (and (get box :changed) (< rounds 8))
    (++ rounds)
    (put box :changed false)
    (each id (range n)
      (def cand (scan-def id))
      (if (get rfixed id)
        nil
        (do (def nt (type-join (rts id) cand))
          (when (not= nt (rts id))
            (put rts id nt)
            (put box :changed true))))))
  @{:pts pts :rts rts :nnp nnp :rounds rounds :pprov pprov
    :tshapes tshapes :taccs taccs :topened topened})
