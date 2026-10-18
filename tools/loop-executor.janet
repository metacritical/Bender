# Whole-function VM-frame executors. No dependency on scalar wrapper inference.
# Stage 2: mixed-operation bodies. Arithmetic/control runs natively in regions;
# explicit VM boundaries (calls, argument pushes, collection access/mutation,
# allocation, division) exit before effects at their original unexecuted PC so
# ordinary dispatch performs them exactly once. Opaque (non-primitive) runtime
# constants are loaded from the live pool; descriptors never embed GC pointers.
(def arithmetic {'add "+" 'sub "-" 'mul "*" 'addim "+" 'subim "-" 'mulim "*"})
(def comparisons {'lt "<" 'gt ">" 'lte "<=" 'gte ">=" 'eq "==" 'neq "!="
                  'ltim "<" 'gtim ">" 'eqim "==" 'neqim "!="})
(def immediate {'addim true 'subim true 'mulim true 'ltim true 'gtim true 'eqim true 'neqim true})
(def branches {'jmpif true 'jmpno true 'jmpni true 'jmpnn true})
# Bitwise family. Janet's bitwise ops are INT32-DOMAIN: operands must be
# integral and within int32 (integral doubles are accepted), results wrap
# int32, and shift counts mask to 5 bits. The emitted C below reproduces
# exactly that, mirroring the scalar-kernel helpers in callgraph.janet
# (k_band32/k_sl32/k_sr32/k_sru32) so both tiers agree bit-for-bit.
# Guarded by NX_INT32, which exits to the VM at the original PC whenever
# an operand is outside the domain -- so the boxed path still raises the
# same error in the same place rather than this tier inventing a result.
(def bitwise {'band "&" 'bor "|" 'bxor "^"})
# sru/sruim (brushift) are DELIBERATELY EXCLUDED: measured, their domain
# is uint32, not int32 -- (brushift -1 1) raises "value -1 out of range
# for 32-bit unsigned integers" while every op above accepts -1 happily.
# Admitting them under the int32 guard would let this tier compute a
# result where the boxed op raises. They need their own uint32 guard;
# until then they stay VM-only. ttfx uses brushift once, in gui.janet,
# never on a hot path, so the exclusion costs nothing measurable.
(def shifts {'sl true 'sr true 'slim true 'srim true})
(def shift-imm {'slim true 'srim true})
(def simple {'noop true 'ret true 'retn true 'ldn true 'ldt true 'ldf true
             'ldi true 'ldc true 'movn true 'movf true 'jmp true})
# Explicit VM boundaries: eligible in descriptors, never executed natively.
# The whole divide/modulo family stays in the VM alongside calls, argument
# pushes, collection access/mutation and allocation.
# `in` is handled natively below (see NX_IN): it is a PURE READ, so
# unlike `put` it cannot allocate, cannot need a GC write barrier, and
# cannot trigger a collection. `put` deliberately stays a boundary --
# JOP_PUT does vm_commit, sets RESUME_NO_USEVAL, may GROW the array
# (allocation), and ends with vm_checkgc, none of which an executor is
# permitted to do. `get` also stays: it returns nil for a missing key
# where `in` raises, and its nil-default path is compiled as a branch.
(def boundaries {'call true 'tcall true 'push true 'push2 true 'push3 true
                 'get true 'len true 'next true 'put true
                 'mkarr true 'mkbuf true 'mktup true
                 'div true 'divim true 'divf true 'mod true 'rem true})

(defn primitive-constant? [x]
  (or (nil? x) (boolean? x)
      (and (number? x) (= x x) (< (math/abs x) math/inf))))

(defn- allowed-op? [op]
  (or (get simple op) (get arithmetic op) (get comparisons op)
      (get branches op) (get boundaries op)
      (get bitwise op) (get shifts op) (= op 'bnot)
      (= op 'in) (= op 'inim)))

(defn eligible? [d]
  (and (d :bytecode-words) (not (d :vararg)) (not (d :structarg))
       (not (d :namedargs)) (empty? (or (d :environments) []))
       (empty? (or (d :defs) []))
       (every? (map |(allowed-op? ($0 0)) (d :bytecode)))
       (some |(or (and (= 'jmp ($0 0)) (<= ($0 1) 0))
                  (and (get branches ($0 0)) (<= ($0 2) 0))) (d :bytecode))))

# Native-op run length from each PC: instructions executed natively before the
# next boundary/return exit, following fall-through only. Returns count for PCs
# in a run. VM-boundary and ret/retn positions terminate with what precedes.
(defn- native-run-lengths [bytecode]
  (def n (length bytecode))
  (def runs (array/new-filled n 0))
  (var i (- n 1))
  (while (>= i 0)
    (def op ((bytecode i) 0))
    (set (runs i) (cond
                    (get boundaries op) 0
                    (or (= op 'ret) (= op 'retn)) 0
                    (= i (- n 1)) 1
                    (+ 1 (runs (+ i 1)))))
    (-- i))
  runs)

# Useful region starts: entry executes at least 3 native operations before the
# next VM boundary, so whole-definition validation cost is not paid per
# boundary or per single instruction.
(def useful-min 3)
# Small definitions are exempt from the useful-region filter: whole-function
# validation is already bounded by their instruction count.
(def small-def-max 16)

(defn- region-mask [bytecode]
  # Array of little-endian 64-bit words as zero-padded 16-digit hex strings,
  # one bit per instruction PC (bit set = native entry allowed).
  (def n (length bytecode))
  (def marked (if (<= n small-def-max)
               (seq [_ :range [0 n]] true)
               (map |(>= $ useful-min) (native-run-lengths bytecode))))
  (def nwords (if (zero? n) 0 (+ 1 (div (- n 1) 64))))
  (def words @[])
  (for w 0 nwords
    (def chars @[])
    (each nib (range 15 -1 -1)
      (var v 0)
      (loop [b :range [0 4]]
        (def i (+ (* w 64) (* nib 4) b))
        (when (and (< i n) (marked i)) (set v (+ v (blshift 1 b)))))
      (array/push chars (string/format "%x" v)))
    (array/push words (string "0x" (string/join chars))))
  words)

(defn emit [id d]
  (def b @"")
  (defn out [& xs] (buffer/push-string b ;xs) (buffer/push-string b "\n"))
  (def tag (string "nx_" id))
  (def mask (region-mask (d :bytecode)))
  (out "#ifdef __FAST_MATH__\n#error Native executors require strict floating-point semantics\n#endif\n#pragma STDC FP_CONTRACT OFF")
  (out "#if defined(__GNUC__) && !defined(__clang__)\n#pragma GCC push_options\n#pragma GCC optimize (\"fp-contract=off\")\n#endif")
  (out "static JANET_THREAD_LOCAL uint64_t " tag "_entries, " tag "_edges, " tag "_fallbacks, "
       tag "_ops, " tag "_vmexits;")
  (out "static const uint32_t " tag "_words[] = {"
       (string/join (map string (d :bytecode-words)) ",") "};")
  (out "static const JanetNativeConstant " tag "_constants[] = {")
  (each x (or (d :constants) [])
    (if (primitive-constant? x)
      (out "{" (case (type x) :number "JANET_NUMBER" :boolean "JANET_BOOLEAN" "JANET_NIL")
           "," (if (number? x) (string/format "%#.17g" x) (if x "1" "0")) "},")
      # Opaque runtime load: the live pool value is read by ldc at execution
      # time; the descriptor assumes nothing about its identity or contents.
      (out "{((JanetType)JANET_NATIVE_OPAQUE),0},")))
  (out "{JANET_NIL,0}};")
  (when (not (empty? mask))
    (out "static const uint64_t " tag "_mask[] = {"
         (string/join mask ",") "};"))
  (out "static JanetNativeStep " tag "(JanetFiber *f, JanetFuncDef *d, uint32_t **pc) {"
       "\nJanet *s = f->data + f->frame;\n" tag "_entries++;"
       "\n#define NX_EXIT(K,R) do { *pc = d->bytecode + (K); return (R); } while (0)"
       "\n#define NX_NUM(S,K) do { if (!janet_checktype(s[S], JANET_NUMBER)) { " tag
       "_fallbacks++; NX_EXIT(K,JANET_NATIVE_VM); } } while (0)"
       "\n#define NX_BOUNDARY(K) do { " tag "_vmexits++; NX_EXIT(K,JANET_NATIVE_VM); } while (0)"
       # int32-domain guard for the bitwise family: number, integral, and
       # representable in int32. Integral doubles pass (Janet accepts
       # them); anything else exits to the VM at the original PC so the
       # boxed op raises identically.
       "\n#define NX_INT32(S,K) do { if (!janet_checktype(s[S], JANET_NUMBER)) { " tag
       "_fallbacks++; NX_EXIT(K,JANET_NATIVE_VM); } { double nxd_ = janet_unwrap_number(s[S]); "
       "if (!(nxd_ >= -2147483648.0 && nxd_ <= 2147483647.0 && nxd_ == (double)(int32_t)nxd_)) { " tag
       "_fallbacks++; NX_EXIT(K,JANET_NATIVE_VM); } } } while (0)"
       "\nswitch (*pc - d->bytecode) {")
  (each k (range (length (d :bytecode))) (out "case " (string k) ": goto L" (string k) ";"))
  (out "default: return JANET_NATIVE_VM; }")
  (each k (range (length (d :bytecode)))
    (def ins ((d :bytecode) k))
    (def op (ins 0))
    (def a (get ins 1))
    (def c (get ins 2))
    (def z (get ins 3))
    (out "L" (string k) ":;")
    (cond
      # Native `in` / `inim`: ARRAY and TUPLE with an in-range
      # non-negative integer index are read directly; EVERYTHING else
      # (tables, structs, strings, buffers, negative or fractional or
      # out-of-range indices, non-indexable values) exits to the VM at
      # this exact PC so janet_in performs it and raises identically.
      # Pure read -- no allocation, no GC write barrier, no collection --
      # which is why `in` can be native here while `put` cannot. This is
      # the single most frequent boundary (6.6% of ttfx matrix
      # dispatches); removing it lengthens native regions, which is the
      # actual limit on this tier (a boundary every ~4.3 instructions,
      # each re-entry re-running the structural compare).
      (or (= op 'in) (= op 'inim))
      (do
        (out tag "_ops++;")
        (def keyexpr (if (= op 'inim)
                       (string "(double)(uint32_t)(" z ")")
                       (string "janet_unwrap_number(s[" z "])")))
        (out "{ Janet nxds_ = s[" (string c) "];")
        (unless (= op 'inim)
          (out "if (!janet_checktype(s[" (string z) "], JANET_NUMBER)) { NX_BOUNDARY(" (string k) "); }"))
        (out "double nxk_ = " keyexpr "; int32_t nxi_ = (int32_t) nxk_;")
        (out "if (nxk_ != (double) nxi_ || nxi_ < 0) { NX_BOUNDARY(" (string k) "); }")
        (out "if (janet_checktype(nxds_, JANET_ARRAY)) {")
        (out "  JanetArray *nxa_ = janet_unwrap_array(nxds_);")
        (out "  if (nxi_ >= nxa_->count) { NX_BOUNDARY(" (string k) "); }")
        (out "  s[" (string a) "] = nxa_->data[nxi_];")
        (out "} else if (janet_checktype(nxds_, JANET_TUPLE)) {")
        (out "  const Janet *nxt_ = janet_unwrap_tuple(nxds_);")
        (out "  if (nxi_ >= janet_tuple_length(nxt_)) { NX_BOUNDARY(" (string k) "); }")
        (out "  s[" (string a) "] = nxt_[nxi_];")
        (out "} else { NX_BOUNDARY(" (string k) "); } }"))

      (get boundaries op) (out "NX_BOUNDARY(" (string k) "); /* " (string op) ": VM executes once */")
      (or (get arithmetic op) (get comparisons op))
      (do
        (out tag "_ops++;")
        (out "NX_NUM(" (string c) "," (string k) ");")
        (unless (get immediate op) (out "NX_NUM(" (string z) "," (string k) ");"))
        (out "s[" (string a) "] = " (if (get arithmetic op) "janet_wrap_number" "janet_wrap_boolean")
             "(janet_unwrap_number(s[" (string c) "]) " (or (get arithmetic op) (get comparisons op))
             " " (if (get immediate op) (string "(double)(" z ")")
                    (string "janet_unwrap_number(s[" z "])") ) ");"))
      # Bitwise: int32-domain, wrapping, shift counts masked to 5 bits --
      # identical semantics to the scalar kernels' k_band32/k_sl32/
      # k_sr32/k_sru32 helpers, so the two tiers cannot disagree.
      (get bitwise op)
      (do
        (out tag "_ops++;")
        (out "NX_INT32(" (string c) "," (string k) ");")
        (out "NX_INT32(" (string z) "," (string k) ");")
        (out "s[" (string a) "] = janet_wrap_number((double)(int32_t)("
             "(int32_t)janet_unwrap_number(s[" (string c) "]) " (get bitwise op)
             " (int32_t)janet_unwrap_number(s[" (string z) "])));"))
      (= op 'bnot)
      (do
        (out tag "_ops++;")
        (out "NX_INT32(" (string c) "," (string k) ");")
        (out "s[" (string a) "] = janet_wrap_number((double)(int32_t)(~"
             "(int32_t)janet_unwrap_number(s[" (string c) "])));"))
      (get shifts op)
      (do
        (out tag "_ops++;")
        (out "NX_INT32(" (string c) "," (string k) ");")
        (unless (get shift-imm op) (out "NX_INT32(" (string z) "," (string k) ");"))
        # Shift count: immediate forms carry it in the instruction; the
        # register forms read it from a slot. Masked to 5 bits either way.
        (def cnt (if (get shift-imm op)
                   (string "(uint32_t)(" z ")")
                   (string "(uint32_t)(int32_t)janet_unwrap_number(s[" z "])")))
        (def av (string "(int32_t)janet_unwrap_number(s[" c "])"))
        (out "{ uint32_t nxn_ = " cnt " & 31u; int32_t nxa_ = " av "; int32_t nxr_;")
        (cond
          (or (= op 'sl) (= op 'slim))
          (out "nxr_ = (int32_t)((uint32_t)nxa_ << nxn_);")
          # arithmetic right shift: sign-extend, matching k_sr32 exactly
          (out "nxr_ = (int32_t)((uint32_t)nxa_ >> nxn_);"
               " if ((nxa_ < 0) && (nxn_ != 0)) { nxr_ |= (int32_t)(~0u << (32u - nxn_)); }"))
        (out "s[" (string a) "] = janet_wrap_number((double) nxr_); }"))
      (or (= op 'jmp) (get branches op))
      (do
        (def delta (if (= op 'jmp) a c))
        (out tag "_ops++;")
        (unless (= op 'jmp)
          (out "if (" (case op
                        'jmpif (string "janet_truthy(s[" a "])")
                        'jmpno (string "!janet_truthy(s[" a "])")
                        'jmpni (string "janet_checktype(s[" a "], JANET_NIL)")
                        (string "!janet_checktype(s[" a "], JANET_NIL)")) ") {"))
        (when (<= delta 0)
          (out tag "_edges++; if (janet_native_poll(1)) NX_EXIT(" (string k) ",JANET_NATIVE_INTERRUPT);"))
        (out "goto L" (string (+ k delta)) ";")
        (unless (= op 'jmp) (out "}")))
      (or (= op 'ret) (= op 'retn)) (out "NX_BOUNDARY(" (string k) ");")
      (= op 'movn) (do (out tag "_ops++;") (out "s[" (string a) "] = s[" (string c) "];"))
      (= op 'movf) (do (out tag "_ops++;") (out "s[" (string c) "] = s[" (string a) "];"))
      (= op 'ldc) (do (out tag "_ops++;") (out "s[" (string a) "] = d->constants[" (string c) "];"))
      (= op 'ldi) (do (out tag "_ops++;") (out "s[" (string a) "] = janet_wrap_integer(" (string c) ");"))
      (= op 'ldn) (do (out tag "_ops++;") (out "s[" (string a) "] = janet_wrap_nil();"))
      (= op 'ldt) (do (out tag "_ops++;") (out "s[" (string a) "] = janet_wrap_true();"))
      (= op 'ldf) (do (out tag "_ops++;") (out "s[" (string a) "] = janet_wrap_false();"))
      (= op 'noop) nil
      (out "NX_BOUNDARY(" (string k) "); /* unsupported, before effects */")))
  (out "#undef NX_NUM\n#undef NX_EXIT\n#undef NX_BOUNDARY\n}")
  (out "#if defined(__GNUC__) && !defined(__clang__)\n#pragma GCC pop_options\n#endif")
  (out "static const JanetNativeCode " tag "_code = {" tag "," tag "_words," tag "_constants,"
       (if (empty? mask) "NULL," (string tag "_mask,"))
       (string/join (map string [(length (d :bytecode)) (d :slotcount) (d :arity)
                                 (d :min-arity) (d :max-arity) (d :flags)
                                 (length (or (d :constants) []))]) ",") ","
       (string (length mask)) "};")
  (string b))
