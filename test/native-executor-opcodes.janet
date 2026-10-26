# Additional generic assembly oracles for taken/untaken conditional backedges,
# self edges, primitive loads and signed immediates. Separate from callgraph IDs.
(import ../tools/loop-executor :as executor)
(def output (file/open ((dyn :args) 1) :a))
(defn emit [id assembly]
  (def d (disasm (asm assembly)))
  (assert (executor/eligible? d))
  (file/write output (executor/emit id d)))
(emit 100 {:arity 1 :slotcount 6
           :bytecode '[(ldi 1 0) (ldi 2 0) (ldi 3 3)
                       (add 1 1 2) (subim 3 3 1) (mulim 4 2 -2)
                       (movn 5 4) (movf 5 4) (addim 2 2 1)
                       (lt 5 2 0) (jmpif 5 -7) (ret 1)]})
(emit 101 {:arity 1 :slotcount 1 :bytecode '[(jmp 0)]})
(emit 102 {:arity 1 :slotcount 3
           :bytecode '[(ldi 1 0) (ldn 2) (jmpnn 2 2) (ldt 2)
                       (jmpni 2 2) (ldf 2) (jmpno 2 2) (ret 0)
                       (addim 1 1 1) (ltim 2 1 3) (jmpif 2 -2) (retn)]})
(each [id op] (map tuple (range 110 118) '[lt lte gt gte eq neq sub mul])
  (emit id {:arity 2 :slotcount 3 :bytecode [(tuple op 2 0 1) '(ret 2) '(jmp 0)]}))
# These values must survive Janet parsing, emitter formatting and C compilation.
# The C oracle uses independent hexadecimal floating-point literals.
(emit 120 {:arity 1 :slotcount 1
           :constants [-0.0 0.0 1.7976931348623157e308 -1.7976931348623157e308
                       2.2250738585072014e-308 4.9406564584124654e-324]
           :bytecode '[(ldc 0 0) (ret 0) (jmp 0)]})
# A VM-only constructor at the tail may coexist with a useful native prefix.
# The VM, not generated C, owns its allocation and GC at the original PC.
(emit 121 {:arity 1 :slotcount 4
           :bytecode '[(ldi 1 0) (ldi 2 1) (push 2) (push 1)
                       (mktab 3) (ret 3) (jmp 0)]})
# Guarded bitwise operations belong to an attached loop; the backedge
# polls at its original PC and leaves live state in the scanned VM frame.
(emit 122 {:arity 1 :slotcount 5
           :bytecode '[(ldi 1 0) (ldi 2 0) (lt 3 1 0) (jmpno 3 4)
                       (bor 2 2 1) (addim 1 1 1) (jmp -4) (ret 2)]})
# Exact structural numeric loop emits one raw-double accumulator and counter
# across the backedge, while all other bytecode shapes remain generic.
(file/flush output)
(def sum-code (slurp ((dyn :args) 1)))
(assert (string/find "double nxn_ = janet_unwrap_number(s[0]), nxsum_" sum-code)
        "numeric sum has a raw-double loop region")
(assert (string/find "if (janet_native_poll(1)) {" sum-code)
        "unboxed backedge retains a resumable quantum poll")
(file/close output)
