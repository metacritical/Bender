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
             'ldi true 'ldc true 'movn true 'movf true 'jmp true 'geti true})
# Explicit VM boundaries: eligible in descriptors, never executed natively.
# The whole divide/modulo family stays in the VM alongside collection
# mutation and allocation. Calls, argument pushes, reusable scratch tuples,
# `next` iteration, `geti`, and primitive equality are handled by
# `native-control` / explicit emitters below; they fall back to their own PC
# when their fast path does not apply.
# `in` and guarded array/tuple `get` are pure reads: neither allocates,
# needs a GC write barrier, or triggers collection. `put` stays a boundary --
# JOP_PUT does vm_commit, sets RESUME_NO_USEVAL, may GROW the array
# (allocation), and ends with vm_checkgc, none of which an executor is
# permitted to do. Other `get` receivers exit at the original PC;
# invalid array/tuple keys return nil, whereas `in` raises.
(def boundaries {'tcall true
                 'len true 'put true
                 'mkarr true 'mkbuf true 'mktup true
                 'mkstr true 'mkstu true 'mktab true 'mkbtp true
                 'div true 'divim true 'divf true 'mod true 'rem true})
# Native control/data operations. These used to be hard VM boundaries; the
# stage-2 executor now executes them directly when it can prove the fast path
# (alias cfunction call, fiber-stack push, reusable scratch array, array/tuple
# iteration). Each op falls back to its original PC before any effect when the
# fast path does not apply, so the VM remains the semantic authority.
(def native-control {'call true 'push true 'push2 true 'push3 true
                     'mkscr true 'next true})

(defn primitive-constant? [x]
  (or (nil? x) (boolean? x)
      (and (number? x) (= x x) (< (math/abs x) math/inf))))

(defn- allowed-op? [op]
  (or (get simple op) (get arithmetic op) (get comparisons op)
      (get branches op) (get boundaries op) (get native-control op)
      (get bitwise op) (get shifts op) (= op 'bnot)
      (= op 'in) (= op 'inim) (= op 'get)))

(defn- base-eligible? [d]
  (and (d :bytecode-words) (not (d :vararg)) (not (d :structarg))
       (not (d :namedargs)) (empty? (or (d :environments) []))
       (empty? (or (d :defs) []))
       (every? (map |(allowed-op? ($0 0)) (d :bytecode)))))

(defn has-loop? [d]
  (some |(or (and (= 'jmp ($0 0)) (<= ($0 1) 0))
             (and (get branches ($0 0)) (<= ($0 2) 0))) (d :bytecode)))

(defn eligible? [d]
  (and (base-eligible? d) (has-loop? d)))

# A loop is not required to amortize a native entry when the whole leaf body
# is a short straight-line run: PGO can select these separately. They still
# use the same region mask, validation, and VM fallback at every boundary.
(defn straight-line-eligible? [d]
  (and (base-eligible? d) (not (has-loop? d))))

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
                    (or (get boundaries op) (= op 'call)) 0
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

# A leaf executor is useful only if its entry PC starts a native run (or the
# whole definition is small enough to mark every PC). Otherwise the VM would
# immediately fall back at PC 0 and the emitted code would be dead weight.
(defn region-entry? [d]
  (def bc (d :bytecode))
  (def n (length bc))
  (or (<= n small-def-max)
      (>= ((native-run-lengths bc) 0) useful-min)))

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

# Exact bytecode contract for a single-loop numeric reduction. The input
# guard limits execution to a small finite integer domain, so doubles retain
# exact integer values and no callback, allocation or GC can occur in-region.
# This is a name-independent pattern: any different bytecode uses the
# ordinary guarded executor, never a guessed source-level shape.
(defn- numeric-sum-pattern? [d]
  (and (= 1 (d :arity)) (= 4 (d :slotcount))
       (empty? (or (d :constants) []))
       (= 8 (length (d :bytecode)))
       (every? (map (fn [i expected]
                      (def ins ((d :bytecode) i))
                      (and (= (length ins) (length expected))
                           (every? (map = ins expected))))
                    (range 8)
                    '[(ldi 1 0) (ldi 2 0) (lt 3 2 0) (jmpno 3 4)
                      (add 1 1 2) (addim 2 2 1) (jmp -4) (ret 1)]))))

# `cont-pcs` is the profile-narrowed set of call PCs (a table/struct used as
# a set, or nil) at which a native-to-native continuation attempt is emitted.
# nil or an empty set emits NO attempt anywhere, which is byte-for-byte the
# pre-continuation shape: a plain NX_EXIT to the VM at the call PC. Narrowing
# only chooses WHERE the guard is emitted; an un-narrowed PC behaves exactly
# as it did before the feature existed, and a narrowed PC still proves the
# live callee at run time inside janet_native_call_enter.
(defn emit [id d &opt cont-pcs]
  (def b @"")
  (defn out [& xs] (buffer/push-string b ;xs) (buffer/push-string b "\n"))
  (def tag (string "nx_" id))
  (def mask (region-mask (d :bytecode)))
  (def numeric-sum? (numeric-sum-pattern? d))
  (out "#ifdef __FAST_MATH__\n#error Native executors require strict floating-point semantics\n#endif\n#pragma STDC FP_CONTRACT OFF")
  (out "#if defined(__GNUC__) && !defined(__clang__)\n#pragma GCC push_options\n#pragma GCC optimize (\"fp-contract=off\")\n#endif")
  (out "static JANET_THREAD_LOCAL uint64_t " tag "_entries, " tag "_edges, " tag "_fallbacks, "
       tag "_ops, " tag "_vmexits, " tag "_conts;")
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
       "\nJanet *s = f->data + f->frame;\n"
       "#ifndef JANET_NATIVE_TELEMETRY\n#define JANET_NATIVE_TELEMETRY 1\n#endif\n"
       "#if JANET_NATIVE_TELEMETRY\n"
       "#define NX_ENTRY() (" tag "_entries++)\n"
       "#define NX_EDGE() (" tag "_edges++)\n"
       "#define NX_TICK() (" tag "_ops++)\n"
       "#define NX_FALLBACK() (" tag "_fallbacks++)\n"
       "#define NX_VMEXIT() (" tag "_vmexits++)\n"
       # Per-definition attribution of chained call edges. The global
       # janet_native_stat counter is the one the counter-identity gate
       # uses; this one answers WHICH caller chained.
       "#define NX_CONT() (" tag "_conts++)\n"
       "#else\n#define NX_ENTRY() ((void)0)\n#define NX_EDGE() ((void)0)\n"
       "#define NX_TICK() ((void)0)\n#define NX_FALLBACK() ((void)0)\n"
       "#define NX_VMEXIT() ((void)0)\n#define NX_CONT() ((void)0)\n#endif\n"
       "NX_ENTRY();"
       "\n#define NX_EXIT(K,R) do { *pc = d->bytecode + (K); return (R); } while (0)"
       "\n#define NX_NUM(S,K) do { if (!janet_checktype(s[S], JANET_NUMBER)) { NX_FALLBACK(); NX_EXIT(K,JANET_NATIVE_VM); } } while (0)"
       "\n#define NX_BOUNDARY(K) do { NX_VMEXIT(); NX_EXIT(K,JANET_NATIVE_VM); } while (0)"
       # int32-domain guard for the bitwise family: number, integral, and
       # representable in int32. Integral doubles pass (Janet accepts
       # them); anything else exits to the VM at the original PC so the
       # boxed op raises identically.
       "\n#define NX_INT32(S,K) do { if (!janet_checktype(s[S], JANET_NUMBER)) { NX_FALLBACK(); NX_EXIT(K,JANET_NATIVE_VM); } { double nxd_ = janet_unwrap_number(s[S]); "
       "if (!(nxd_ >= -2147483648.0 && nxd_ <= 2147483647.0 && nxd_ == (double)(int32_t)nxd_)) { NX_FALLBACK(); NX_EXIT(K,JANET_NATIVE_VM); } } } while (0)"
       "\nswitch (*pc - d->bytecode) {")
  (each k (range (length (d :bytecode))) (out "case " (string k) ": goto L" (string k) ";"))
  (out "default: return JANET_NATIVE_VM; }")
  (when numeric-sum?
    # Enter only at the original loop head, after both initialization
    # instructions. n and the accumulator stay boxed and GC-scanned in the
    # VM frame; the two local doubles never hold a heap pointer. Deopt at
    # this unexecuted head on any failed guard. The bound makes all adds
    # exact integers; it also prevents int32 counter overflow.
    (out "L2:;")
    (out "if (!janet_checktype(s[0], JANET_NUMBER) || !janet_checktype(s[1], JANET_NUMBER) || !janet_checktype(s[2], JANET_NUMBER)) { NX_FALLBACK(); NX_EXIT(2,JANET_NATIVE_VM); }")
    (out "double nxn_ = janet_unwrap_number(s[0]), nxsum_ = janet_unwrap_number(s[1]), nxi_ = janet_unwrap_number(s[2]);")
    (out "if (!(nxn_ >= 0.0 && nxn_ <= 1000000.0 && nxn_ == floor(nxn_) && nxi_ >= 0.0 && nxi_ <= nxn_ && nxi_ == floor(nxi_) && nxsum_ >= 0.0 && nxsum_ <= 500000000000.0 && nxsum_ == floor(nxsum_))) { NX_FALLBACK(); NX_EXIT(2,JANET_NATIVE_VM); }")
    (out "while (nxi_ < nxn_) {")
    (out "  nxsum_ += nxi_; nxi_ += 1.0;")
    # The VM polls *after* the completed additions and *before* the jump.
    # On interruption, materialize the scanned VM slots and leave PC at
    # the unexecuted backedge. Without interruption, the doubles stay raw.
    (out "  NX_TICK(); NX_TICK(); NX_TICK(); NX_EDGE();")
    (out "  if (janet_native_poll(1)) {")
    (out "    s[1] = janet_wrap_number(nxsum_); s[2] = janet_wrap_number(nxi_);")
    (out "    s[3] = janet_wrap_true(); NX_EXIT(6,JANET_NATIVE_INTERRUPT);")
    (out "  }")
    (out "}")
    (out "s[1] = janet_wrap_number(nxsum_); s[2] = janet_wrap_number(nxi_);")
    (out "s[3] = janet_wrap_false(); NX_EXIT(7,JANET_NATIVE_VM);")
  )
  (each k (range (length (d :bytecode)))
    (def ins ((d :bytecode) k))
    (def op (ins 0))
    (def a (get ins 1))
    (def c (get ins 2))
    (def z (get ins 3))
    (unless (and numeric-sum? (= k 2)) (out "L" (string k) ":;"))
    (cond
      (and numeric-sum? (= k 1)) (do (out "NX_TICK();")
                                 (out "s[2] = janet_wrap_integer(0); goto L2;"))
      (and numeric-sum? (= k 2)) nil
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
        (out "NX_TICK();")
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

      # Guarded `get`: for arrays/tuples only, with janet_get's int32 key and
      # nil-on-invalid-index rule. Other receivers fall back before effects.
      (= op 'get)
      (do
        (out "NX_TICK();")
        (out "{ Janet nxds_ = s[" (string c) "]; Janet nxkey_ = s[" (string z) "];")
        (out "  if (janet_checktype(nxds_, JANET_ARRAY)) {")
        (out "    JanetArray *nxa_ = janet_unwrap_array(nxds_);")
        (out "    if (janet_checkint(nxkey_)) {")
        (out "      int32_t nxi_ = janet_unwrap_integer(nxkey_);")
        (out "      s[" (string a) "] = ((uint32_t)nxi_ < (uint32_t)nxa_->count) ? nxa_->data[nxi_] : janet_wrap_nil();")
        (out "    } else s[" (string a) "] = janet_wrap_nil();")
        (out "  } else if (janet_checktype(nxds_, JANET_TUPLE)) {")
        (out "    const Janet *nxt_ = janet_unwrap_tuple(nxds_);")
        (out "    if (janet_checkint(nxkey_)) {")
        (out "      int32_t nxi_ = janet_unwrap_integer(nxkey_);")
        (out "      s[" (string a) "] = ((uint32_t)nxi_ < (uint32_t)janet_tuple_length(nxt_)) ? nxt_[nxi_] : janet_wrap_nil();")
        (out "    } else s[" (string a) "] = janet_wrap_nil();")
        # Table reads with primitive keys are pure: janet_table_get walks
        # the table/prototype hashes without allocating or calling Janet.
        # Non-primitive keys (abstract hashing callbacks) stay in the VM.
        (out "  } else {")
        (out "    Janet nxout_;")
        (out "    if (janet_native_table_get_fast(nxds_, nxkey_, &nxout_)) s[" (string a) "] = nxout_;")
        (out "    else NX_BOUNDARY(" (string k) ");")
        (out "  } }"))

      # Native `geti`: immediate-index read with janet_getindex semantics.
      # Arrays, tuples, strings/symbols/keywords, and buffers are read
      # directly, including the out-of-range -> nil rule. Every other
      # type exits to the VM at this PC so table/struct/abstract getters
      # and panics happen exactly once.
      (= op 'geti)
      (do
        (out "NX_TICK();")
        (out "{ Janet nxds_ = s[" (string c) "];")
        (out "if (janet_checktype(nxds_, JANET_ARRAY)) {")
        (out "  JanetArray *nxa_ = janet_unwrap_array(nxds_);")
        (out "  s[" (string a) "] = ((uint32_t)(" (string z) ") < (uint32_t)nxa_->count) ? nxa_->data[" (string z) "] : janet_wrap_nil();")
        (out "} else if (janet_checktype(nxds_, JANET_TUPLE)) {")
        (out "  const Janet *nxt_ = janet_unwrap_tuple(nxds_);")
        (out "  s[" (string a) "] = ((uint32_t)(" (string z) ") < (uint32_t)janet_tuple_length(nxt_)) ? nxt_[" (string z) "] : janet_wrap_nil();")
        (out "} else if (janet_checktype(nxds_, JANET_STRING) ||")
        (out "           janet_checktype(nxds_, JANET_SYMBOL) ||")
        (out "           janet_checktype(nxds_, JANET_KEYWORD)) {")
        (out "  JanetString nxs_ = janet_unwrap_string(nxds_);")
        (out "  s[" (string a) "] = ((uint32_t)(" (string z) ") < (uint32_t)janet_string_length(nxs_)) ? janet_wrap_integer(nxs_[" (string z) "]) : janet_wrap_nil();")
        (out "} else if (janet_checktype(nxds_, JANET_BUFFER)) {")
        (out "  JanetBuffer *nxb_ = janet_unwrap_buffer(nxds_);")
        (out "  s[" (string a) "] = ((uint32_t)(" (string z) ") < (uint32_t)nxb_->count) ? janet_wrap_integer(nxb_->data[" (string z) "]) : janet_wrap_nil();")
        (out "} else { NX_BOUNDARY(" (string k) "); } }"))

      # Guarded `put`: an in-range array index performs exactly janet_put's
      # store plus generational barrier. An already-present primitive table
      # key performs exactly the existing-key branch of janet_table_put
      # (value store plus both barriers). Array growth, table insertion/
      # removal/rehash, buffers, abstract setters, non-integer array keys and
      # non-primitive table keys exit before any mutation. The PC is
      # committed before the barrier so a strict-heap panic still reports
      # this instruction.
      (= op 'put)
      (do
        (out "NX_TICK();")
        (out "{ Janet nxds_ = s[" (string a) "]; Janet nxkey_ = s[" (string c) "]; Janet nxval_ = s[" (string z) "];")
        (out "  janet_native_set_pc(f, d->bytecode + " (string k) ");")
        (out "  if (!janet_native_array_put_fast(nxds_, nxkey_, nxval_) &&")
        (out "      !janet_native_table_put_fast(nxds_, nxkey_, nxval_)) {")
        (out "    NX_BOUNDARY(" (string k) ");")
        (out "  } }"))

      # Native argument pushes. Values go directly to the fiber argument
      # stack, exactly where the bytecode would put them. Capacity and
      # maxstack are checked before every write; on failure we exit at this
      # push PC so the VM grows or reports overflow, with any earlier pushes
      # already visible.
      (or (= op 'push) (= op 'push2) (= op 'push3))
      (do
        (def n (case op 'push 1 'push2 2 3))
        (def vals (case op 'push [a] 'push2 [a c] [a c z]))
        (out "NX_TICK();")
        (out "if (f->stacktop + " (string n) " > f->maxstack || f->stacktop + " (string n) " > f->capacity) { NX_BOUNDARY(" (string k) "); }")
        (each v vals (out "f->data[f->stacktop++] = s[" (string v) "];")))

      # Native scratch-tuple construction. The compiler reserves a frame
      # slot that becomes a reusable array after the first VM execution. Only
      # a reusable array with enough capacity is handled here; the first
      # allocation (or a capacity growth) exits at this PC and the VM performs
      # it. No allocation, callback, or GC is reachable on the native path.
      (= op 'mkscr)
      (do
        (out "NX_TICK();")
        (out "{ Janet nxsc_ = s[" (string c) "];")
        (out "  if (!janet_checktype(nxsc_, JANET_ARRAY)) { NX_BOUNDARY(" (string k) "); }")
        (out "  JanetArray *nxa_ = janet_unwrap_array(nxsc_);")
        (out "  int32_t nxn_ = f->stacktop - f->stackstart;")
        (out "  if (nxn_ < 0 || nxa_->capacity < nxn_) { NX_BOUNDARY(" (string k) "); }")
        (out "  for (int32_t nxi_ = 0; nxi_ < nxn_; nxi_++) nxa_->data[nxi_] = f->data[f->stackstart + nxi_];")
        (out "  nxa_->count = nxn_;")
        (out "  s[" (string a) "] = nxsc_;")
        (out "  f->stacktop = f->stackstart; }"))

      # Native call to an installed FFI alias function. Arguments stay
      # GC-scanned in JOP_CALL's C frame. After storing the result and
      # checking GC, exit at the NEXT bytecode PC: callbacks/finalizers
      # can invalidate the executor's structural verdict. Every other
      # callee exits BEFORE the call with its argument stack untouched.
      (= op 'call)
      (if (= k (- (length (d :bytecode)) 1))
        (out "NX_BOUNDARY(" (string k) "); /* terminal call: VM owns it */")
        (do
          (out "NX_TICK();")
          (out "{ Janet nxcallee_ = s[" (string c) "];")
          (out "  if (f->stack_overflowed) { NX_EXIT(" (string k) ", JANET_NATIVE_VM); }")
          (out "  int32_t nxargc_ = f->stacktop - f->stackstart;")
          # Boot predicates (nil?, empty?, symbol?, ...) are tiny pure
          # Janet functions. Match their exact core JanetFuncDef, poll, and
          # compute the boxed boolean inline. Rebinding any predicate gives
          # a different def and falls through to the VM.
          (out "  { Janet nxpred_;")
          (out "    if (janet_native_core_predicate(nxcallee_, nxargc_, f->data + f->stackstart, &nxpred_)) {")
          (out "      if (janet_native_poll(1)) { NX_EXIT(" (string k) ", JANET_NATIVE_INTERRUPT); }")
          (out "      s[" (string a) "] = nxpred_;")
          (out "      f->stacktop = f->stackstart;")
          (out "      goto L" (string (+ k 1)) ";")
          (out "    } }")
          # Exact two-argument buffer/push-string call: capacity-checked
          # append with no allocation, collection, or signal. Poll before
          # the commit so an interrupt leaves the argument stack untouched
          # and the VM re-executes the call at this PC.
          (out "  { const uint8_t *nxbytes_ = NULL; int32_t nxlen_ = 0;")
          (out "    if (janet_native_buffer_push_string_ready(nxcallee_, nxargc_, f->data + f->stackstart, &nxbytes_, &nxlen_)) {")
          (out "      if (janet_native_poll(1)) { NX_EXIT(" (string k) ", JANET_NATIVE_INTERRUPT); }")
          (out "      Janet nxret_ = janet_native_buffer_push_string_commit(nxargc_, f->data + f->stackstart, nxbytes_, nxlen_);")
          (out "      s[" (string a) "] = nxret_;")
          (out "      f->stacktop = f->stackstart;")
          (out "      goto L" (string (+ k 1)) ";")
          (out "    } }")
          # Container-as-function calls: (table key), (struct key),
          # (array index), ... are exactly janet_in when argc == 1. The
          # helper only accepts primitive keys and in-range indices; all
          # other cases (including the VM's range errors and method hooks)
          # fall through. The read is side-effect-free, so a poll after the
          # lookup can still exit with the argument stack untouched.
          (out "  { Janet nxco_;")
          (out "    if (janet_native_container_call_fast(nxcallee_, nxargc_, f->data + f->stackstart, &nxco_)) {")
          (out "      if (janet_native_poll(1)) { NX_EXIT(" (string k) ", JANET_NATIVE_INTERRUPT); }")
          (out "      s[" (string a) "] = nxco_;")
          (out "      f->stacktop = f->stackstart;")
          (out "      goto L" (string (+ k 1)) ";")
          (out "    } }")
          (out "  JanetCFunction nxfn_ = janet_native_alias_cfun(nxcallee_, nxargc_);")
          (out "  if (nxfn_) {")
          (out "      /* Only the direct-wrapper path polls here. Ordinary")
          (out "         JOP_CALL polls after fallback; never poll twice. */")
          (out "      if (janet_native_poll(1)) { NX_EXIT(" (string k) ", JANET_NATIVE_INTERRUPT); }")
          (out "      Janet nxret_ = janet_native_alias_call(f, nxfn_, nxargc_, d->bytecode + " (string k) ");")
          (out "      s = f->data + f->frame;")
          (out "      s[" (string a) "] = nxret_;")
          (out "      janet_native_alias_after_call();")
          (out "      NX_VMEXIT(); NX_EXIT(" (string (+ k 1)) ", JANET_NATIVE_VM);")
          # Native-to-native continuation. The callee is read from a runtime
          # slot, so its identity is proven at run time, never statically:
          # janet_native_call_enter re-checks the whole safety list and is
          # all-or-nothing. On 1 it has done exactly what JOP_CALL's function
          # branch does (caller PC committed, janet_fiber_funcframe, GC
          # check) and we hand the NEW frame's PC 0 back with
          # JANET_NATIVE_FRAME; the trampoline then runs the callee's own
          # executor with no interpreter bounce. On 0 nothing was touched and
          # the ordinary VM exit below performs the call exactly once --
          # including its interrupt poll, which is why only the committed
          # path (1 or 2) is ever polled here.
          #
          # Emitted ONLY at profile-selected PCs. Everywhere else this is a
          # bare NX_EXIT to the VM, which is both the pre-continuation shape
          # and measurably cheaper: an always-on attempt at every
          # non-terminal call PC cost +0.6-1.0% geomean with no measurable
          # chaining benefit (see the Implementation Notes in
          # docs/internals/native-continuation-design.md).
          (out "  } else {")
          (if (and cont-pcs (get cont-pcs k))
            (do
              (out "      uint32_t *nxnpc_ = NULL;")
              (out "      int nxcont_ = janet_native_call_enter(f, nxcallee_, nxargc_,")
              (out "                                            d->bytecode + " (string k) ", &nxnpc_);")
              (out "      if (nxcont_ == 1) { NX_CONT(); *pc = nxnpc_; return JANET_NATIVE_FRAME; }")
              (out "      if (nxcont_ == 2) { NX_EXIT(" (string k) ", JANET_NATIVE_INTERRUPT); }")
              (out "      NX_EXIT(" (string k) ", JANET_NATIVE_VM);"))
            (out "      NX_EXIT(" (string k) ", JANET_NATIVE_VM);"))
          (out "  } }")))

      # Native `next` for arrays, tuples, strings, and buffers. This is the
      # iterator used by `each`; tables, structs, abstracts, fibers, and
      # non-integral keys exit to the VM's janet_next_impl at this PC.
      (= op 'next)
      (do
        (out "NX_TICK();")
        (out "{ Janet nds_ = s[" (string c) "]; Janet nkey_ = s[" (string z) "];")
        (out "  int32_t nxi_ = -1;")
        (out "  if (janet_checktype(nkey_, JANET_NIL)) nxi_ = 0;")
        (out "  else if (janet_checkint(nkey_)) {")
        (out "    int32_t nxn_ = janet_unwrap_integer(nkey_);")
        (out "    nxi_ = (nxn_ == INT32_MAX) ? -1 : nxn_ + 1;")
        (out "  }")
        (out "  else { NX_BOUNDARY(" (string k) "); }")
        (out "  int32_t nxl_ = -1;")
        (out "  if (janet_checktype(nds_, JANET_ARRAY)) nxl_ = janet_unwrap_array(nds_)->count;")
        (out "  else if (janet_checktype(nds_, JANET_TUPLE)) nxl_ = janet_tuple_length(janet_unwrap_tuple(nds_));")
        (out "  else if (janet_checktype(nds_, JANET_STRING)) nxl_ = janet_string_length(janet_unwrap_string(nds_));")
        (out "  else if (janet_checktype(nds_, JANET_BUFFER)) nxl_ = janet_unwrap_buffer(nds_)->count;")
        (out "  else { NX_BOUNDARY(" (string k) "); }")
        (out "  s[" (string a) "] = ((nxi_ >= 0 && nxi_ < nxl_) ? janet_wrap_integer(nxi_) : janet_wrap_nil()); }"))

      (get boundaries op) (out "NX_BOUNDARY(" (string k) "); /* " (string op) ": VM executes once */")

      # Equality on primitive non-numbers. The generic comparison path below
      # requires numbers; `=`/`not=` are also valid on strings, symbols,
      # keywords, nil, booleans, and function identities, and ttfx's draw loop
      # tests every cell symbol against " ". Keep composite values (tables,
      # structs, arrays, buffers, abstracts) on the VM path so recursive or
      # user equality cannot run inside a native region.
      (or (= op 'eq) (= op 'neq))
      (do
        (out "NX_TICK();")
        (out "if (janet_checktype(s[" (string c) "], JANET_NUMBER) && janet_checktype(s[" (string z) "], JANET_NUMBER)) {")
        (out "  s[" (string a) "] = janet_wrap_boolean(janet_unwrap_number(s[" (string c) "]) "
             (if (= op 'eq) "==" "!=") " janet_unwrap_number(s[" (string z) "]));")
        (out "} else {")
        (out "  JanetType nxta_ = janet_type(s[" (string c) "]);")
        (out "  JanetType nxtb_ = janet_type(s[" (string z) "]);")
        (out "  if (nxta_ == nxtb_ && (nxta_ == JANET_STRING || nxta_ == JANET_SYMBOL ||")
        (out "      nxta_ == JANET_KEYWORD || nxta_ == JANET_NIL || nxta_ == JANET_BOOLEAN ||")
        (out "      nxta_ == JANET_CFUNCTION || nxta_ == JANET_FUNCTION)) {")
        (out "    s[" (string a) "] = janet_wrap_boolean(janet_equals(s[" (string c) "], s[" (string z) "])"
             (if (= op 'eq) "" " ? 0 : 1") ");")
        (out "  } else { NX_BOUNDARY(" (string k) "); }")
        (out "}"))

      (or (get arithmetic op) (get comparisons op))
      (do
        (out "NX_TICK();")
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
        (out "NX_TICK();")
        (out "NX_INT32(" (string c) "," (string k) ");")
        (out "NX_INT32(" (string z) "," (string k) ");")
        (out "s[" (string a) "] = janet_wrap_number((double)(int32_t)("
             "(int32_t)janet_unwrap_number(s[" (string c) "]) " (get bitwise op)
             " (int32_t)janet_unwrap_number(s[" (string z) "])));"))
      (= op 'bnot)
      (do
        (out "NX_TICK();")
        (out "NX_INT32(" (string c) "," (string k) ");")
        (out "s[" (string a) "] = janet_wrap_number((double)(int32_t)(~"
             "(int32_t)janet_unwrap_number(s[" (string c) "])));"))
      (get shifts op)
      (do
        (out "NX_TICK();")
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
        (out "NX_TICK();")
        (unless (= op 'jmp)
          (out "if (" (case op
                        'jmpif (string "janet_truthy(s[" a "])")
                        'jmpno (string "!janet_truthy(s[" a "])")
                        'jmpni (string "janet_checktype(s[" a "], JANET_NIL)")
                        (string "!janet_checktype(s[" a "], JANET_NIL)")) ") {"))
        (when (<= delta 0)
          (out "NX_EDGE(); if (janet_native_poll(1)) NX_EXIT(" (string k) ",JANET_NATIVE_INTERRUPT);"))
        (out "goto L" (string (+ k delta)) ";")
        (unless (= op 'jmp) (out "}")))
      (or (= op 'ret) (= op 'retn)) (out "NX_BOUNDARY(" (string k) ");")
      (= op 'movn) (do (out "NX_TICK();") (out "s[" (string a) "] = s[" (string c) "];"))
      (= op 'movf) (do (out "NX_TICK();") (out "s[" (string c) "] = s[" (string a) "];"))
      (= op 'ldc) (do (out "NX_TICK();") (out "s[" (string a) "] = d->constants[" (string c) "];"))
      (= op 'ldi) (do (out "NX_TICK();") (out "s[" (string a) "] = janet_wrap_integer(" (string c) ");"))
      (= op 'ldn) (do (out "NX_TICK();") (out "s[" (string a) "] = janet_wrap_nil();"))
      (= op 'ldt) (do (out "NX_TICK();") (out "s[" (string a) "] = janet_wrap_true();"))
      (= op 'ldf) (do (out "NX_TICK();") (out "s[" (string a) "] = janet_wrap_false();"))
      (= op 'noop) nil
      (out "NX_BOUNDARY(" (string k) "); /* unsupported, before effects */")))
  (out "#undef NX_NUM\n#undef NX_EXIT\n#undef NX_BOUNDARY\n"
       "#undef NX_ENTRY\n#undef NX_EDGE\n#undef NX_TICK\n#undef NX_FALLBACK\n#undef NX_VMEXIT\n}")
  (out "#if defined(__GNUC__) && !defined(__clang__)\n#pragma GCC pop_options\n#endif")
  (out "static const JanetNativeCode " tag "_code = {" tag "," tag "_words," tag "_constants,"
       (if (empty? mask) "NULL," (string tag "_mask,"))
       (string/join (map string [(length (d :bytecode)) (d :slotcount) (d :arity)
                                 (d :min-arity) (d :max-arity) (d :flags)
                                 (length (or (d :constants) []))]) ",") ","
       (string (length mask)) "};")
  (string b))
