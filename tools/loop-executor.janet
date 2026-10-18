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
(def simple {'noop true 'ret true 'retn true 'ldn true 'ldt true 'ldf true
             'ldi true 'ldc true 'movn true 'movf true 'jmp true})
# Explicit VM boundaries: eligible in descriptors, never executed natively.
# The whole divide/modulo family stays in the VM alongside calls, argument
# pushes, collection access/mutation and allocation.
(def boundaries {'call true 'tcall true 'push true 'push2 true 'push3 true
                 'get true 'in true 'len true 'next true 'put true
                 'mkarr true 'mkbuf true 'mktup true
                 'div true 'divim true 'divf true 'mod true 'rem true})

(defn primitive-constant? [x]
  (or (nil? x) (boolean? x)
      (and (number? x) (= x x) (< (math/abs x) math/inf))))

(defn- allowed-op? [op]
  (or (get simple op) (get arithmetic op) (get comparisons op)
      (get branches op) (get boundaries op)))

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
