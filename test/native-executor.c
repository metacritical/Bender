/* Compile against generated native.c from fixtures/native-executor.ben.
 * Compare VM signals, original PCs and all boxed live slots, not just sums. */
#include "state.h"
#include "fiber.h"
#include <assert.h>
#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
/* Generated numeric-sum callbacks exist only in this test compilation unit.
 * This flag is absent from release builds; only the numeric-sum fused region
 * invokes the hook, so these counters have deliberately partial coverage. */
#define BENDER_TEST_FUSED_SUM_RETIRE_COUNTS 1
#if BENDER_TEST_FUSED_SUM_RETIRE_COUNTS
static int retired[64], retired_count, retirement_enabled;
static uint64_t sum_retired[5]; /* native id 10, original PCs 2..6 */
static int sum_retired_overflow[5];
static void count_sum_retirement(int pc) {
    assert(pc >= 2 && pc <= 6);
    int index = pc - 2;
    if (sum_retired[index] == UINT64_MAX) sum_retired_overflow[index] = 1;
    else sum_retired[index]++;
}
#define BENDER_NATIVE_RETIRE_HOOK(pc) do { if (retirement_enabled) { \
    assert(retired_count < 64); retired[retired_count++] = (pc); \
    count_sum_retirement(pc); \
} } while (0)
#endif
#include NATIVE_EXECUTOR_C

static BenderFunction *lookup(BenderTable *env, const char *name) {
    Bender x;
    assert(bender_resolve(env, bender_csymbol(name), &x));
    assert(bender_checktype(x, BENDER_FUNCTION));
    return bender_unwrap_function(x);
}
static BenderFiber *spawn(BenderFunction *fn, int n) {
    Bender arg = bender_wrap_integer(n);
    BenderFiber *f = bender_fiber(fn, 64, 1, &arg);
    bender_gcroot(bender_wrap_fiber(f));
    return f;
}
static int pc_of(BenderFiber *f) {
    BenderStackFrame *fr = bender_fiber_frame(f);
    return (int)(fr->pc - fr->func->def->bytecode);
}
static void same_parked(BenderFiber *a, BenderFiber *b) {
    assert(pc_of(a) == pc_of(b));
    assert(a->flags == b->flags);
    int count = bender_fiber_frame(a)->func->def->slotcount;
    for (int i = 0; i < count; i++)
        assert(bender_equals(a->data[a->frame+i], b->data[b->frame+i]));
}
static void attach(BenderFunction *fn, const BenderNativeCode *code) {
    assert(bender_native_register(fn, code));
}
static BenderFunction *assembled(const BenderNativeCode *code) {
    BenderFuncDef *d = bender_funcdef_alloc();
    d->flags = code->flags; d->slotcount = code->slotcount; d->arity = code->arity;
    d->min_arity = code->min_arity; d->max_arity = code->max_arity;
    d->bytecode_length = code->length;
    d->bytecode = bender_malloc((size_t)code->length * sizeof(uint32_t));
    memcpy(d->bytecode, code->words, (size_t)code->length * sizeof(uint32_t));
    d->constants_length = code->constants_length;
    if (d->constants_length) {
        d->constants = bender_malloc((size_t)d->constants_length * sizeof(Bender));
        for (int32_t i = 0; i < d->constants_length; i++) {
            assert(code->constants[i].type == BENDER_NUMBER);
            d->constants[i] = bender_wrap_number(code->constants[i].number);
        }
    }
    BenderFunction *f = bender_thunk(d); bender_gcroot(bender_wrap_function(f));
    return f;
}
static void parity(BenderFunction *fn, const BenderNativeCode *code, int budget, int n, double answer) {
    BenderFiber *a = spawn(fn, n), *b = spawn(fn, n);
    Bender x, y;
    int done = 0, signals = 0, first_pc = -1;
    bender_vm.quantum = budget;
    for (int i = 0; i < 256; i++) {
        fn->def->native_code = NULL;
        Bender input = i ? bender_wrap_integer(999) : bender_wrap_nil();
        BenderSignal sa = bender_continue(a, input, &x);
        attach(fn, code);
        BenderSignal sb = bender_continue(b, input, &y);
        assert(sa == sb && bender_equals(x, y));
        if (sa == BENDER_SIGNAL_OK) { assert(bender_unwrap_number(x) == answer); done = 1; break; }
        assert(sa == BENDER_SIGNAL_INTERRUPT);
        same_parked(a, b);
        if (!signals) first_pc = pc_of(a);
        if (budget == 1) assert(pc_of(a) == first_pc);
        signals++;
        if (budget == 1 && i == 3) {
            /* HEAD retries the unexecuted edge forever at budget 1. */
            bender_vm.quantum = 0;
        }
    }
    assert(done && signals > 0);
    printf("trace budget=%d slots=%d interrupts=%d result=%.0f parity\n", budget, code->slotcount, signals, answer);
    bender_gcunroot(bender_wrap_fiber(a)); bender_gcunroot(bender_wrap_fiber(b));
    bender_vm.quantum = 0;
}
#ifdef BENDER_NATIVE_RETIRE_CENSUS
/* The generated per-definition census is driven directly, so this block also
 * documents its deliberate incompleteness: only general emitter branches call
 * NX_RETIRE, so the fused numeric-sum loop retires no PCs here at all. */
static void census_fixture(BenderFunction *sum) {
    Bender out;
    uint64_t before[8];
    memcpy(before, nx_10_retired, sizeof before);
    BenderFiber *f = spawn(sum, 2);
    bender_vm.quantum = 0;
    assert(bender_continue(f, bender_wrap_nil(), &out) == BENDER_SIGNAL_OK);
    assert(bender_unwrap_number(out) == 1);
    bender_gcunroot(bender_wrap_fiber(f));
    /* PC0 is the general `ldi`; PC1 is the specialization's fused entry jump
     * and retires nothing. Inside L2 the guarded test retires PCs 2/3 on every
     * completed test (twice, plus once for the final false test), the two
     * arithmetic PCs retire per true iteration, and PC6 only after the poll
     * passes. Everything else -- calls, branches and other fused regions --
     * stays uncounted, so no upper bound or gate verdict is available. */
    assert(nx_10_retired[0] == before[0] + 1);
    assert(nx_10_retired[1] == before[1]);
    assert(nx_10_retired[2] == before[2] + 3);
    assert(nx_10_retired[3] == before[3] + 3);
    assert(nx_10_retired[4] == before[4] + 2);
    assert(nx_10_retired[5] == before[5] + 2);
    assert(nx_10_retired[6] == before[6] + 2);
    assert(nx_10_retired[7] == before[7]);
    /* Saturation is sticky: a full PC counter reports overflow, never wraps. */
    nx_10_retired[0] = UINT64_MAX;
    nx_10_retired_overflow[0] = 0;
    f = spawn(sum, 0);
    assert(bender_continue(f, bender_wrap_nil(), &out) == BENDER_SIGNAL_OK);
    bender_gcunroot(bender_wrap_fiber(f));
    assert(nx_10_retired[0] == UINT64_MAX);
    assert(nx_10_retired_overflow[0] == 1);
    puts("native census id=10 coverage=partial gate=unavailable unresolved=calls,branches,other-fused");
}
#endif
static void retirement_fixture(BenderFunction *sum) {
    Bender out;
    retirement_enabled = 1;
    bender_vm.quantum = 0;
    for (int n = 0; n <= 2; n += 2) {
        BenderFiber *done = spawn(sum, n);
        retired_count = 0;
        assert(bender_continue(done, bender_wrap_nil(), &out) == BENDER_SIGNAL_OK);
        assert(bender_unwrap_number(out) == (n ? 1 : 0));
        int expected[13] = {2, 3, 4, 5, 6, 2, 3, 4, 5, 6, 2, 3};
        assert(retired_count == 5 * n + 2);
        for (int i = 0; i < retired_count; i++) assert(retired[i] == expected[i]);
        for (int pc = 2; pc <= 6; pc++)
            assert(sum_retired[pc - 2] == (uint64_t)(n + (pc <= 3 ? 1 + (n > 0) : 0)));
        bender_gcunroot(bender_wrap_fiber(done));
    }
    /* Out-of-range input rejects the fused region at unexecuted PC2.
     * Boxed execution may finish, but must not emit native retirements. */
    BenderFiber *guarded = spawn(sum, -1);
    uint64_t fallbacks_before = nx_10_fallbacks;
    retired_count = 0;
    assert(bender_continue(guarded, bender_wrap_nil(), &out) == BENDER_SIGNAL_OK);
    assert(bender_unwrap_number(out) == 0 && retired_count == 0);
    assert(nx_10_fallbacks == fallbacks_before + 1);
    for (int pc = 2; pc <= 6; pc++)
        assert(sum_retired[pc - 2] == (uint64_t)(2 + 2 * (pc <= 3)));
    bender_gcunroot(bender_wrap_fiber(guarded));
    BenderFiber *f;

#ifndef BENDER_NO_INTERPRETER_INTERRUPT
    f = spawn(sum, 4);
    retired_count = 0;
    bender_vm.quantum = 2;
    assert(bender_continue(f, bender_wrap_nil(), &out) == BENDER_SIGNAL_INTERRUPT);
    assert(pc_of(f) == 6 && retired_count == 5 * BENDER_QUANTUM_BUDGET - 1);
    for (int i = 0; i < retired_count; i++) {
        int expected[] = {2, 3, 4, 5, 6};
        assert(retired[i] == expected[i % 5]);
    }
    for (int pc = 2; pc <= 6; pc++) {
        uint64_t expected_count = 2 + 2 * (pc <= 3);
        for (int i = 0; i < retired_count; i++) expected_count += retired[i] == pc;
        assert(sum_retired[pc - 2] == expected_count);
    }
    /* Force the resumed operation through the VM: its pre-effect guard and
     * poll cannot report a retirement, nor can subsequent boxed execution. */
    sum->def->native_code = NULL;
    bender_vm.quantum = 0;
    assert(bender_continue(f, bender_wrap_nil(), &out) == BENDER_SIGNAL_OK);
    assert(bender_unwrap_number(out) == 6 && retired_count == 5 * BENDER_QUANTUM_BUDGET - 1);
    for (int pc = 2; pc <= 6; pc++) {
        uint64_t expected_count = 2 + 2 * (pc <= 3);
        for (int i = 0; i < retired_count; i++) expected_count += retired[i] == pc;
        assert(sum_retired[pc - 2] == expected_count);
    }
    attach(sum, &nx_10_code);
    bender_gcunroot(bender_wrap_fiber(f));
#endif
    bender_vm.quantum = 0;
    retirement_enabled = 0;
    /* Only the generated numeric-sum region is instrumented. The runtime
     * unresolved population is unknown; never infer a complete histogram or
     * a numeric performance gate from these observed retirements. */
    puts("fused numeric-sum id=10 original-pc=2..6 coverage=partial gate=unavailable INCOMPLETE unresolved=unknown");
    for (int pc = 2; pc <= 6; pc++)
        printf("fused id=10 pc=%d retired=%" PRIu64 " overflow=%d\n",
               pc, sum_retired[pc - 2], sum_retired_overflow[pc - 2]);
    uint64_t previous = sum_retired[0];
    sum_retired[0] = UINT64_MAX - 1;
    count_sum_retirement(2);
    assert(sum_retired[0] == UINT64_MAX && !sum_retired_overflow[0]);
    count_sum_retirement(2);
    assert(sum_retired[0] == UINT64_MAX && sum_retired_overflow[0]);
    count_sum_retirement(2);
    assert(sum_retired[0] == UINT64_MAX && sum_retired_overflow[0]);
    sum_retired[0] = previous;
}
static int overloaded;
static Bender add_once(int32_t argc, Bender *argv) {
    assert(argc == 2);
    overloaded++;
    double n = bender_getnumber(argv, 1);
    bender_collect(); /* Must not invalidate executor's former slot pointer. */
    return bender_wrap_number(100 + n);
}
static void fallback(BenderFunction *fn, int native, int bad) {
    BenderFiber *f = spawn(fn, 4);
    if (native) attach(fn, &nx_10_code); else fn->def->native_code = NULL;
    bender_vm.quantum = 2;
    Bender out;
    assert(bender_continue(f, bender_wrap_nil(), &out) == BENDER_SIGNAL_INTERRUPT);
    assert(pc_of(f) == 6);
    BenderTable *t = bender_table(1);
    bender_table_put(t, bender_ckeywordv("+"), bender_wrap_cfunction(add_once));
    f->data[f->frame+1] = bad ? bender_cstringv("wrong type") : bender_wrap_table(t);
    bender_vm.quantum = 0;
    overloaded = 0;
    BenderSignal sig = bender_continue(f, bender_wrap_nil(), &out);
    if (bad) { assert(sig == BENDER_SIGNAL_ERROR && overloaded == 0); assert(pc_of(f) == 4); }
    else { assert(sig == BENDER_SIGNAL_OK && overloaded == 1 && bender_unwrap_number(out) == (BENDER_QUANTUM_BUDGET == 1 ? 106 : 105)); }
    bender_gcunroot(bender_wrap_fiber(f));
}
int main(int argc, char **argv) {
    assert(argc == 2);
    bender_init();
    bender_vm.quantum = 0;
    BenderTable *env = bender_core_env(NULL);
    bender_gcroot(bender_wrap_table(env));
    /* The fixture imports ./native-executor-mixed: resolve from the
     * fixture's directory like the bender CLI does. */
    {
        char *dir = strdup(argv[1]);
        char *slash = strrchr(dir, '/');
        if (slash) { *slash = 0; assert(!chdir(dir)); }
        const char *base = slash ? slash + 1 : argv[1];
        argv[1] = (char *)base;
    }
    FILE *file = fopen(argv[1], "rb"); assert(file);
    fseek(file, 0, SEEK_END); long size = ftell(file); rewind(file);
    char *src = malloc((size_t)size + 1); assert(src);
    assert(fread(src, 1, (size_t)size, file) == (size_t)size); fclose(file); src[size] = 0;
    assert(!bender_dostring(env, src, argv[1], NULL)); free(src);
    BenderFunction *sum = lookup(env, "numeric-sum"), *nested = lookup(env, "numeric-nested");
    /* Predicate fast paths are keyed by the canonical core BenderFuncDef.
     * A different function must never match, even with the same arity. */
    {
        Bender nv = bender_wrap_nil();
        Bender pout = bender_wrap_nil();
        Bender pv = bender_wrap_function(lookup(env, "nil?"));
        bender_native_register_predicates(bender_core_env(NULL));
        assert(bender_native_core_predicate(pv, 1, &nv, &pout));
        assert(bender_checktype(pout, BENDER_BOOLEAN) && bender_unwrap_boolean(pout));
        pout = bender_wrap_nil();
        assert(!bender_native_core_predicate(bender_wrap_function(sum), 1, &nv, &pout));
    }
#ifdef TEST_NATIVE_DISABLED
    assert(!bender_native_register(sum, &nx_10_code));
    assert(!sum->def->native_code);
    puts("default-off registration refused; compiler oracles passed");
    bender_deinit(); return 0;
#endif
    attach(sum, &nx_10_code); attach(nested, &nx_11_code);
    retirement_fixture(sum);
#ifdef BENDER_NATIVE_RETIRE_CENSUS
    census_fixture(sum);
#endif
    /* A VM-only allocation at a later PC must not veto the native prefix. */
    BenderFunction *table = assembled(&nx_121_code);
    attach(table, &nx_121_code);
    Bender arg = bender_wrap_integer(3), result_table;
    BenderFiber *table_fiber = bender_fiber(table, 64, 1, &arg);
    bender_gcroot(bender_wrap_fiber(table_fiber));
    assert(bender_continue(table_fiber, bender_wrap_nil(), &result_table) == BENDER_SIGNAL_OK);
    assert(bender_checktype(result_table, BENDER_TABLE));
    assert(bender_equals(bender_table_get(bender_unwrap_table(result_table), bender_wrap_integer(1)),
                        bender_wrap_integer(0)));
    assert(nx_121_entries > 0 && nx_121_vmexits > 0);
    bender_gcunroot(bender_wrap_fiber(table_fiber));
    bender_gcunroot(bender_wrap_function(table));
    BenderFunction *bits = assembled(&nx_122_code);
    attach(bits, &nx_122_code);
    Bender bit_arg = bender_wrap_integer(4), result_bits;
    BenderFiber *bit_fiber = bender_fiber(bits, 64, 1, &bit_arg);
    bender_gcroot(bender_wrap_fiber(bit_fiber));
    assert(bender_continue(bit_fiber, bender_wrap_nil(), &result_bits) == BENDER_SIGNAL_OK);
    assert(bender_equals(result_bits, bender_wrap_integer(3)));
    assert(nx_122_entries > 0 && nx_122_edges >= 4 && nx_122_ops > 0);
    bender_gcunroot(bender_wrap_fiber(bit_fiber));
    bender_gcunroot(bender_wrap_function(bits));
    assert(!bender_native_register(lookup(env, "unsupported-loop"), &nx_10_code));
    assert(!lookup(env, "boxed-caller")->def->native_code);
    /* A boxed caller must actually reach the generated executor. */
    uint64_t before = nx_10_entries;
    assert(!bender_dostring(env, "(assert (= 45 (boxed-caller 10)))", "caller", NULL));
    assert(nx_10_entries > before && nx_10_edges >= 10);
#ifndef BENDER_NO_INTERPRETER_INTERRUPT
    assert(BENDER_QUANTUM_BUDGET == 1 || BENDER_QUANTUM_BUDGET == 2);
    parity(sum, &nx_10_code, BENDER_QUANTUM_BUDGET, 10, 45);
    parity(nested, &nx_11_code, BENDER_QUANTUM_BUDGET, 10, 870);
    BenderFunction *conditional = assembled(&nx_100_code);
    parity(conditional, &nx_100_code, BENDER_QUANTUM_BUDGET, 4, 6);
    bender_gcunroot(bender_wrap_function(conditional));
    BenderFunction *self = assembled(&nx_101_code); attach(self, &nx_101_code);
    BenderFiber *self_fiber = spawn(self, 0); Bender self_out;
    bender_vm.quantum = 1;
    for (int i = 0; i < 3; i++) {
        assert(bender_continue(self_fiber, bender_wrap_nil(), &self_out) == BENDER_SIGNAL_INTERRUPT);
        assert(pc_of(self_fiber) == 0);
    }
    assert(bender_continue_signal(self_fiber, bender_wrap_nil(), &self_out, BENDER_SIGNAL_ERROR) == BENDER_SIGNAL_ERROR);
    bender_gcunroot(bender_wrap_fiber(self_fiber)); bender_gcunroot(bender_wrap_function(self));
    fallback(sum, 0, 0); fallback(sum, 1, 0);
    fallback(sum, 0, 1); fallback(sum, 1, 1);
    assert(nx_10_fallbacks >= 2);
    /* Auto-suspend short circuits before spending any quantum. */
    BenderFiber *a = spawn(sum, 10), *b = spawn(sum, 10);
    Bender x, y;
    bender_vm.quantum = 2;
    bender_interpreter_interrupt(NULL);
    sum->def->native_code = NULL;
    assert(bender_continue(a, bender_wrap_nil(), &x) == BENDER_SIGNAL_INTERRUPT);
    assert(bender_vm.quantum_remaining == BENDER_QUANTUM_BUDGET);
    attach(sum, &nx_10_code);
    assert(bender_continue(b, bender_wrap_nil(), &y) == BENDER_SIGNAL_INTERRUPT);
    assert(bender_vm.quantum_remaining == BENDER_QUANTUM_BUDGET);
    same_parked(a, b);
    bender_interpreter_interrupt_handled(NULL);
    before = nx_10_entries;
    assert(bender_continue_signal(b, bender_wrap_integer(123), &y, BENDER_SIGNAL_ERROR) == BENDER_SIGNAL_ERROR);
    assert(bender_unwrap_number(y) == 123 && nx_10_entries == before);
    bender_vm.quantum = 0;
    assert(bender_continue(a, bender_wrap_nil(), &x) == BENDER_SIGNAL_OK && bender_unwrap_number(x) == 45);
    bender_gcunroot(bender_wrap_fiber(a)); bender_gcunroot(bender_wrap_fiber(b));
    /* Interleaved ordinary frames retain independent scanned slots. */
    a = spawn(sum, 8); b = spawn(sum, 12); bender_vm.quantum = 2;
    int da = 0, db = 0;
    for (int i = 0; i < 32 && !(da && db); i++) {
        if (!db) { BenderSignal s = bender_continue(b, bender_wrap_nil(), &y); db = s == BENDER_SIGNAL_OK; assert(db || s == BENDER_SIGNAL_INTERRUPT); }
        bender_collect();
        if (!da) { BenderSignal s = bender_continue(a, bender_wrap_nil(), &x); da = s == BENDER_SIGNAL_OK; assert(da || s == BENDER_SIGNAL_INTERRUPT); }
        if (BENDER_QUANTUM_BUDGET == 1) bender_vm.quantum = 0;
    }
    assert(da && db && bender_unwrap_number(x) == 28 && bender_unwrap_number(y) == 66);
    bender_gcunroot(bender_wrap_fiber(a)); bender_gcunroot(bender_wrap_fiber(b));
    /* Parked marshal drops machine addresses, retains original jump PC. */
    a = spawn(sum, 10);
    bender_vm.quantum = 1;
    assert(bender_continue(a, bender_wrap_nil(), &x) == BENDER_SIGNAL_INTERRUPT);
    BenderBuffer *buf = bender_buffer(0); bender_gcroot(bender_wrap_buffer(buf));
    bender_marshal(buf, bender_wrap_fiber(a), NULL, 0);
    Bender restored = bender_unmarshal(buf->data, buf->count, 0, NULL, NULL);
    bender_gcroot(restored); b = bender_unwrap_fiber(restored);
    assert(!bender_fiber_frame(b)->func->def->native_code); same_parked(a, b);
    before = nx_10_entries; bender_vm.quantum = 0;
    assert(bender_continue(b, bender_wrap_nil(), &y) == BENDER_SIGNAL_OK);
    assert(bender_unwrap_number(y) == 45 && nx_10_entries == before);
    bender_gcunroot(bender_wrap_fiber(a)); bender_gcunroot(restored); bender_gcunroot(bender_wrap_buffer(buf));
    puts("fallback once + GC, auto_suspend, cancellation, interleaving, parked marshal: passed");
#else
    Bender out; BenderFiber *f = spawn(nested, 10); bender_vm.quantum = 1;
    bender_interpreter_interrupt(NULL);
    assert(bender_continue(f, bender_wrap_nil(), &out) == BENDER_SIGNAL_OK);
    assert(bender_unwrap_number(out) == 870 && nx_11_edges > 0);
    bender_interpreter_interrupt_handled(NULL); bender_vm.quantum = 0;
    bender_gcunroot(bender_wrap_fiber(f));
#endif
    /* Any mutation anywhere refuses entry. Breakpoint and step keep PCs. */
    bender_vm.quantum = 0;
    attach(sum, &nx_10_code);
    uint32_t old = sum->def->bytecode[4]; sum->def->bytecode[4] |= 0x80;
    assert(!bender_native_matches(sum->def, &nx_10_code));
    BenderFiber *debug = spawn(sum, 3); Bender result;
    before = nx_10_entries;
    assert(bender_continue(debug, bender_wrap_nil(), &result) == BENDER_SIGNAL_DEBUG);
    assert(pc_of(debug) == 4 && nx_10_entries == before);
    assert(bender_step(debug, bender_wrap_nil(), &result) == BENDER_SIGNAL_DEBUG);
    assert(pc_of(debug) == 5 && nx_10_entries == before);
    sum->def->bytecode[4] = old;
    assert(bender_continue(debug, bender_wrap_nil(), &result) == BENDER_SIGNAL_OK);
    assert(bender_unwrap_number(result) == 3);
    bender_gcunroot(bender_wrap_fiber(debug));
    sum->def->bytecode[0] ^= 1u << 16;
    assert(!bender_native_matches(sum->def, &nx_10_code)); sum->def->bytecode[0] ^= 1u << 16;
    int flags = sum->def->flags; sum->def->flags ^= BENDER_FUNCDEF_FLAG_TAG;
    assert(!bender_native_matches(sum->def, &nx_10_code)); sum->def->flags = flags;
    assert(bender_native_matches(sum->def, &nx_10_code));
    BenderFunction *fraction = lookup(env, "numeric-fraction");
    attach(fraction, &nx_14_code);
    Bender constant = fraction->def->constants[0];
    fraction->def->constants[0] = bender_wrap_number(-0.0);
    assert(!bender_native_matches(fraction->def, &nx_14_code));
    fraction->def->constants[0] = bender_wrap_nil();
    assert(!bender_native_register(fraction, &nx_14_code));
    assert(!fraction->def->native_code);
    fraction->def->constants[0] = constant; attach(fraction, &nx_14_code);
    assert(!bender_dostring(env, "(assert (= 3 (numeric-fraction 10)))", "constants", NULL));
    assert(nx_14_entries > 0 && nx_14_edges == 10);
    BenderFunction *primitives = assembled(&nx_102_code);
    attach(primitives, &nx_102_code);
    BenderFiber *pf = spawn(primitives, 0);
    assert(bender_continue(pf, bender_wrap_nil(), &result) == BENDER_SIGNAL_OK);
    assert(bender_checktype(result, BENDER_NIL));
    assert(nx_102_entries == 1 && nx_102_edges == 2);
    bender_gcunroot(bender_wrap_fiber(pf)); bender_gcunroot(bender_wrap_function(primitives));
    const BenderNativeCode *ops[] = {&nx_110_code, &nx_111_code, &nx_112_code, &nx_113_code,
                                   &nx_114_code, &nx_115_code, &nx_116_code, &nx_117_code};
    double values[] = {0, -0.0, 1, -2, INFINITY, -INFINITY, NAN};
    for (size_t op = 0; op < sizeof(ops)/sizeof(*ops); op++) {
        BenderFunction *f = assembled(ops[op]);
        for (size_t i = 0; i < sizeof(values)/sizeof(*values); i++) {
            for (size_t j = 0; j < sizeof(values)/sizeof(*values); j++) {
                Bender args[2] = {bender_wrap_number(values[i]), bender_wrap_number(values[j])};
                BenderFiber *fa = bender_fiber(f, 64, 2, args); bender_gcroot(bender_wrap_fiber(fa));
                BenderFiber *fb = bender_fiber(f, 64, 2, args); bender_gcroot(bender_wrap_fiber(fb));
                Bender ra, rb; f->def->native_code = NULL;
                assert(bender_continue(fa, bender_wrap_nil(), &ra) == BENDER_SIGNAL_OK);
                attach(f, ops[op]);
                assert(bender_continue(fb, bender_wrap_nil(), &rb) == BENDER_SIGNAL_OK);
                if (bender_checktype(ra, BENDER_NUMBER)) {
                    double va = bender_unwrap_number(ra), vb = bender_unwrap_number(rb);
                    assert((isnan(va) && isnan(vb)) || !memcmp(&va, &vb, sizeof(double)));
                } else assert(bender_equals(ra, rb));
                bender_gcunroot(bender_wrap_fiber(fa)); bender_gcunroot(bender_wrap_fiber(fb));
            }
        }
        bender_gcunroot(bender_wrap_function(f));
    }
    /* Independent bit oracle for emitted constants, not runtime arguments. */
    const double expected_constants[] = {
        -0.0, 0.0, 0x1.fffffffffffffp1023, -0x1.fffffffffffffp1023,
        0x1p-1022, 0x1p-1074
    };
    assert(nx_120_code.constants_length == 6);
    for (int i = 0; i < 6; i++) {
        assert(nx_120_code.constants[i].type == BENDER_NUMBER);
        assert(!memcmp(&nx_120_code.constants[i].number,
                       &expected_constants[i], sizeof(double)));
    }
    BenderFunction *roundtrip = assembled(&nx_120_code);
    attach(roundtrip, &nx_120_code);
    BenderFiber *rf = spawn(roundtrip, 0);
    assert(bender_continue(rf, bender_wrap_nil(), &result) == BENDER_SIGNAL_OK);
    double actual_zero = bender_unwrap_number(result);
    assert(!memcmp(&actual_zero, &expected_constants[0], sizeof(double)));
    assert(nx_120_entries > 0);
    roundtrip->def->constants[0] = bender_wrap_number(0.0);
    assert(!bender_native_matches(roundtrip->def, &nx_120_code));
    bender_gcunroot(bender_wrap_fiber(rf));
    bender_gcunroot(bender_wrap_function(roundtrip));
    puts("emitted signed-zero/extreme finite/subnormal constant roundtrip passed");
    puts("primitive/conditional/self-edge oracles and 392 IEEE numeric pairs passed");
    printf("generated counters: sum entries=%llu backedges=%llu guards=%llu; nested entries=%llu backedges=%llu\n",
           (unsigned long long)nx_10_entries, (unsigned long long)nx_10_edges, (unsigned long long)nx_10_fallbacks,
           (unsigned long long)nx_11_entries, (unsigned long long)nx_11_edges);
    bender_deinit();
    return 0;
}
