# tools/infer.janet — shared type inference (Phase 4 steps 6-7).
# Spinel-analyzer model (analyze_pass.c bind_args_params / slot_take):
#   - at every call site, each pushed argument's type is bound
#     (monotonically narrowed) into the callee's parameter slot;
#   - annotation-seeded params/returns are contracts and are never
#     weakened (Spinel's rbs_seeded);
#   - conflicting call sites widen to :number (Spinel's poly) — boxed;
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

# intersection-style slot_take: refine cur toward new; disagreement on
# concrete widths widens to :number; non-numeric involvement gives up
(defn narrow [cur new]
  (cond
    (= cur new) cur
    (= cur :unknown) new
    (= new :unknown) cur
    (= cur :number) (if (get numeric-types new) new :unknown)
    (= new :number) (if (get numeric-types cur) cur :unknown)
    (and (get numeric-types cur) (get numeric-types new)) :number
    :unknown))

(def arith-ops
  @{'add true 'sub true 'mul true 'div true 'divf true
    'mod true 'rem true})
(def arith-im-ops
  @{'addim true 'subim true 'mulim true})
(def cmp-ops
  @{'eq true 'eqim true 'lt true 'ltim true 'gt true 'gtim true
    'lte true 'gte true
    'neq true 'neqim true 'ltu true 'gtu true 'cmp true 'next true})

(defn const-num-type [d cidx]
  # :long/:double for numeric constants, :unknown otherwise
  (def consts (or (get d :constants) @[]))
  (if (>= cidx (length consts))
    :unknown
    (do (def c (consts cidx))
      (if (= :number (type c))
        (if (= c (math/trunc c)) :long :double)
        :unknown))))

(defn analyze [defs call-map]
  # defs: array of @{:d disasm :hint ...}; call-map: id -> {pc -> tid}
  # returns @{:pts pts :rts rts :rounds rounds}
  (def n (length defs))
  (def pts @{})
  (def rts @[])
  (def pfixed @{})
  (def rfixed @{})
  (each id (range n)
    (def d ((get defs id) :d))
    (def h (get (get defs id) :hint))
    (def arity (or (get d :arity) 0))
    (def parr (array/new-filled arity :unknown))
    (def hp (if h (get h :params) nil))
    (when hp
      (var i 0)
      (each p hp
        # Step 11: :array params are opaque-but-known (pointer, never
        # arithmetized); annotation-seeded either way (pfixed below)
        (when (< i arity)
          (put parr i (if (or (get numeric-types p) (= p :array)) p :unknown)))
        (++ i))
      (put pfixed id true))
    (put pts id parr)
    (def hr (if h (get h :returns) nil))
    (array/push rts (if (get numeric-types hr) hr :unknown))
    (when (get numeric-types hr) (put rfixed id true)))
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
    (each ins code
      (def op (ins 0))
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
        (get arith-ops op)
        (put st (ins 1)
             (arith-join (get st (ins 2) :unknown) (get st (ins 3) :unknown)))
        (get cmp-ops op) (put st (ins 1) :bool)
        # Step 11: array access. get needs an :array base and an
        # int-family index (-> :number); anything else is :unknown
        # (fail-closed: tables/strings stay boxed). put keeps :array
        # only with int index + numeric value, else :unknown.
        (= op 'get)
        (put st (ins 1)
             (if (and (= :array (get st (ins 2) :unknown))
                      (int-family? (get st (ins 3) :unknown)))
               :number :unknown))
        (= op 'put)
        (do (def pa (get st (ins 1) :unknown))
          (when (= pa :array)
            (unless (and (int-family? (get st (ins 2) :unknown))
                         (get numeric-types (get st (ins 3) :unknown)))
              (put st (ins 1) :unknown))))
        (= op 'push) (array/push pending (get st (ins 1) :unknown))
        (= op 'push2)
        (do (array/push pending (get st (ins 1) :unknown))
          (array/push pending (get st (ins 2) :unknown)))
        (= op 'push3)
        (do (array/push pending (get st (ins 1) :unknown))
          (array/push pending (get st (ins 2) :unknown))
          (array/push pending (get st (ins 3) :unknown)))
        (= op 'pusha) (set spread true)
        (= op 'call)
        (do (def tid (get cmap pc))
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
          (set pending @[])
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
          (set pending @[])
          (set spread false))
        (= op 'ret) (set rtype (type-join rtype (get st (ins 1) :unknown)))
        (= op 'retn) (set rtype (type-join rtype :nil))
        nil)
      (++ pc))
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
  @{:pts pts :rts rts :rounds rounds})
