# Whole-function VM-frame executors. No dependency on scalar wrapper inference.
(def arithmetic {'add "+" 'sub "-" 'mul "*" 'addim "+" 'subim "-" 'mulim "*"})
(def comparisons {'lt "<" 'gt ">" 'lte "<=" 'gte ">=" 'eq "==" 'neq "!="
                  'ltim "<" 'gtim ">" 'eqim "==" 'neqim "!="})
(def immediate {'addim true 'subim true 'mulim true 'ltim true 'gtim true 'eqim true 'neqim true})
(def branches {'jmpif true 'jmpno true 'jmpni true 'jmpnn true})
(def simple {'noop true 'ret true 'retn true 'ldn true 'ldt true 'ldf true
             'ldi true 'ldc true 'movn true 'movf true 'jmp true})

(defn eligible? [d]
  (and (d :bytecode-words) (not (d :vararg)) (not (d :structarg))
       (not (d :namedargs)) (empty? (or (d :environments) []))
       (empty? (or (d :defs) []))
       (every? (map (fn [x] (or (nil? x) (boolean? x)
                           (and (number? x) (= x x) (< (math/abs x) math/inf))))
               (or (d :constants) [])))
       (every? (map |(or (get simple ($0 0)) (get arithmetic ($0 0))
                    (get comparisons ($0 0)) (get branches ($0 0))) (d :bytecode)))
       (some |(or (and (= 'jmp ($0 0)) (<= ($0 1) 0))
                  (and (get branches ($0 0)) (<= ($0 2) 0))) (d :bytecode))))

(defn emit [id d]
  (def b @"")
  (defn out [& xs] (buffer/push-string b ;xs) (buffer/push-string b "\n"))
  (def tag (string "nx_" id))
  (out "#ifdef __FAST_MATH__\n#error Native executors require strict floating-point semantics\n#endif\n#pragma STDC FP_CONTRACT OFF")
  (out "#if defined(__GNUC__) && !defined(__clang__)\n#pragma GCC push_options\n#pragma GCC optimize (\"fp-contract=off\")\n#endif")
  (out "static JANET_THREAD_LOCAL uint64_t " tag "_entries, " tag "_edges, " tag "_fallbacks;")
  (out "static const uint32_t " tag "_words[] = {"
       (string/join (map string (d :bytecode-words)) ",") "};")
  (out "static const JanetNativeConstant " tag "_constants[] = {")
  (each x (or (d :constants) [])
    (out "{" (case (type x) :number "JANET_NUMBER" :boolean "JANET_BOOLEAN" "JANET_NIL")
         "," (if (number? x) (string/format "%#.17g" x) (if x "1" "0")) "},"))
  (out "{JANET_NIL,0}};")
  (out "static JanetNativeStep " tag "(JanetFiber *f, JanetFuncDef *d, uint32_t **pc) {"
       "\nJanet *s = f->data + f->frame;\n" tag "_entries++;"
       "\n#define NX_EXIT(K,R) do { *pc = d->bytecode + (K); return (R); } while (0)"
       "\n#define NX_NUM(S,K) do { if (!janet_checktype(s[S], JANET_NUMBER)) { " tag
       "_fallbacks++; NX_EXIT(K,JANET_NATIVE_VM); } } while (0)"
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
      (or (get arithmetic op) (get comparisons op))
      (do
        (out "NX_NUM(" (string c) "," (string k) ");")
        (unless (get immediate op) (out "NX_NUM(" (string z) "," (string k) ");"))
        (out "s[" (string a) "] = " (if (get arithmetic op) "janet_wrap_number" "janet_wrap_boolean")
             "(janet_unwrap_number(s[" (string c) "]) " (or (get arithmetic op) (get comparisons op))
             " " (if (get immediate op) (string "(double)(" z ")")
                    (string "janet_unwrap_number(s[" z "])") ) ");"))
      (or (= op 'jmp) (get branches op))
      (do
        (def delta (if (= op 'jmp) a c))
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
      (or (= op 'ret) (= op 'retn)) (out "NX_EXIT(" (string k) ",JANET_NATIVE_VM);")
      (= op 'movn) (out "s[" (string a) "] = s[" (string c) "];")
      (= op 'movf) (out "s[" (string c) "] = s[" (string a) "];")
      (= op 'ldc) (out "s[" (string a) "] = d->constants[" (string c) "];")
      (= op 'ldi) (out "s[" (string a) "] = janet_wrap_integer(" (string c) ");")
      (= op 'ldn) (out "s[" (string a) "] = janet_wrap_nil();")
      (= op 'ldt) (out "s[" (string a) "] = janet_wrap_true();")
      (= op 'ldf) (out "s[" (string a) "] = janet_wrap_false();")
      (= op 'noop) nil
      (out "NX_EXIT(" (string k) ",JANET_NATIVE_VM); /* unsupported, before effects */")))
  (out "#undef NX_NUM\n#undef NX_EXIT\n}")
  (out "#if defined(__GNUC__) && !defined(__clang__)\n#pragma GCC pop_options\n#endif")
  (out "static const JanetNativeCode " tag "_code = {" tag "," tag "_words," tag "_constants,"
       (string/join (map string [(length (d :bytecode)) (d :slotcount) (d :arity)
                                (d :min-arity) (d :max-arity) (d :flags)
                                (length (or (d :constants) []))]) ",") "};")
  (string b))
