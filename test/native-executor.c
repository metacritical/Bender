/* Compile against generated native.c from fixtures/native-executor.janet.
 * Compare VM signals, original PCs and all boxed live slots, not just sums. */
#include "state.h"
#include "fiber.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
/* Generated numeric-sum callbacks exist only in this test compilation unit. */
static int retired[64], retired_count, retirement_enabled;
#define JANET_NATIVE_RETIRE_HOOK(pc) do { if (retirement_enabled) { \
    assert(retired_count < 64); retired[retired_count++] = (pc); \
} } while (0)
#include NATIVE_EXECUTOR_C

static JanetFunction *lookup(JanetTable *env, const char *name) {
    Janet x;
    assert(janet_resolve(env, janet_csymbol(name), &x));
    assert(janet_checktype(x, JANET_FUNCTION));
    return janet_unwrap_function(x);
}
static JanetFiber *spawn(JanetFunction *fn, int n) {
    Janet arg = janet_wrap_integer(n);
    JanetFiber *f = janet_fiber(fn, 64, 1, &arg);
    janet_gcroot(janet_wrap_fiber(f));
    return f;
}
static int pc_of(JanetFiber *f) {
    JanetStackFrame *fr = janet_fiber_frame(f);
    return (int)(fr->pc - fr->func->def->bytecode);
}
static void same_parked(JanetFiber *a, JanetFiber *b) {
    assert(pc_of(a) == pc_of(b));
    assert(a->flags == b->flags);
    int count = janet_fiber_frame(a)->func->def->slotcount;
    for (int i = 0; i < count; i++)
        assert(janet_equals(a->data[a->frame+i], b->data[b->frame+i]));
}
static void attach(JanetFunction *fn, const JanetNativeCode *code) {
    assert(janet_native_register(fn, code));
}
static JanetFunction *assembled(const JanetNativeCode *code) {
    JanetFuncDef *d = janet_funcdef_alloc();
    d->flags = code->flags; d->slotcount = code->slotcount; d->arity = code->arity;
    d->min_arity = code->min_arity; d->max_arity = code->max_arity;
    d->bytecode_length = code->length;
    d->bytecode = janet_malloc((size_t)code->length * sizeof(uint32_t));
    memcpy(d->bytecode, code->words, (size_t)code->length * sizeof(uint32_t));
    d->constants_length = code->constants_length;
    if (d->constants_length) {
        d->constants = janet_malloc((size_t)d->constants_length * sizeof(Janet));
        for (int32_t i = 0; i < d->constants_length; i++) {
            assert(code->constants[i].type == JANET_NUMBER);
            d->constants[i] = janet_wrap_number(code->constants[i].number);
        }
    }
    JanetFunction *f = janet_thunk(d); janet_gcroot(janet_wrap_function(f));
    return f;
}
static void parity(JanetFunction *fn, const JanetNativeCode *code, int budget, int n, double answer) {
    JanetFiber *a = spawn(fn, n), *b = spawn(fn, n);
    Janet x, y;
    int done = 0, signals = 0, first_pc = -1;
    janet_vm.quantum = budget;
    for (int i = 0; i < 256; i++) {
        fn->def->native_code = NULL;
        Janet input = i ? janet_wrap_integer(999) : janet_wrap_nil();
        JanetSignal sa = janet_continue(a, input, &x);
        attach(fn, code);
        JanetSignal sb = janet_continue(b, input, &y);
        assert(sa == sb && janet_equals(x, y));
        if (sa == JANET_SIGNAL_OK) { assert(janet_unwrap_number(x) == answer); done = 1; break; }
        assert(sa == JANET_SIGNAL_INTERRUPT);
        same_parked(a, b);
        if (!signals) first_pc = pc_of(a);
        if (budget == 1) assert(pc_of(a) == first_pc);
        signals++;
        if (budget == 1 && i == 3) {
            /* HEAD retries the unexecuted edge forever at budget 1. */
            janet_vm.quantum = 0;
        }
    }
    assert(done && signals > 0);
    printf("trace budget=%d slots=%d interrupts=%d result=%.0f parity\n", budget, code->slotcount, signals, answer);
    janet_gcunroot(janet_wrap_fiber(a)); janet_gcunroot(janet_wrap_fiber(b));
    janet_vm.quantum = 0;
}
static void retirement_fixture(JanetFunction *sum) {
    Janet out;
    retirement_enabled = 1;
    janet_vm.quantum = 0;
    for (int n = 0; n <= 2; n += 2) {
        JanetFiber *done = spawn(sum, n);
        retired_count = 0;
        assert(janet_continue(done, janet_wrap_nil(), &out) == JANET_SIGNAL_OK);
        assert(janet_unwrap_number(out) == (n ? 1 : 0));
        assert(retired_count == 2 * n);
        for (int i = 0; i < retired_count; i++) assert(retired[i] == (i % 2 ? 5 : 4));
        janet_gcunroot(janet_wrap_fiber(done));
    }
    JanetFiber *f;

#ifndef JANET_NO_INTERPRETER_INTERRUPT
    f = spawn(sum, 4);
    retired_count = 0;
    janet_vm.quantum = 2;
    assert(janet_continue(f, janet_wrap_nil(), &out) == JANET_SIGNAL_INTERRUPT);
    assert(pc_of(f) == 6 && retired_count == 2 * JANET_QUANTUM_BUDGET);
    for (int i = 0; i < retired_count; i++) assert(retired[i] == (i % 2 ? 5 : 4));
    /* Force the resumed operation through the VM: its pre-effect guard and
     * poll cannot report a retirement, nor can subsequent boxed execution. */
    sum->def->native_code = NULL;
    janet_vm.quantum = 0;
    assert(janet_continue(f, janet_wrap_nil(), &out) == JANET_SIGNAL_OK);
    assert(janet_unwrap_number(out) == 6 && retired_count == 2 * JANET_QUANTUM_BUDGET);
    attach(sum, &nx_10_code);
    janet_gcunroot(janet_wrap_fiber(f));
#endif
    janet_vm.quantum = 0;
    retirement_enabled = 0;
}
static int overloaded;
static Janet add_once(int32_t argc, Janet *argv) {
    assert(argc == 2);
    overloaded++;
    double n = janet_getnumber(argv, 1);
    janet_collect(); /* Must not invalidate executor's former slot pointer. */
    return janet_wrap_number(100 + n);
}
static void fallback(JanetFunction *fn, int native, int bad) {
    JanetFiber *f = spawn(fn, 4);
    if (native) attach(fn, &nx_10_code); else fn->def->native_code = NULL;
    janet_vm.quantum = 2;
    Janet out;
    assert(janet_continue(f, janet_wrap_nil(), &out) == JANET_SIGNAL_INTERRUPT);
    assert(pc_of(f) == 6);
    JanetTable *t = janet_table(1);
    janet_table_put(t, janet_ckeywordv("+"), janet_wrap_cfunction(add_once));
    f->data[f->frame+1] = bad ? janet_cstringv("wrong type") : janet_wrap_table(t);
    janet_vm.quantum = 0;
    overloaded = 0;
    JanetSignal sig = janet_continue(f, janet_wrap_nil(), &out);
    if (bad) { assert(sig == JANET_SIGNAL_ERROR && overloaded == 0); assert(pc_of(f) == 4); }
    else { assert(sig == JANET_SIGNAL_OK && overloaded == 1 && janet_unwrap_number(out) == (JANET_QUANTUM_BUDGET == 1 ? 106 : 105)); }
    janet_gcunroot(janet_wrap_fiber(f));
}
int main(int argc, char **argv) {
    assert(argc == 2);
    janet_init();
    janet_vm.quantum = 0;
    JanetTable *env = janet_core_env(NULL);
    janet_gcroot(janet_wrap_table(env));
    /* The fixture imports ./native-executor-mixed: resolve from the
     * fixture's directory like the janet CLI does. */
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
    assert(!janet_dostring(env, src, argv[1], NULL)); free(src);
    JanetFunction *sum = lookup(env, "numeric-sum"), *nested = lookup(env, "numeric-nested");
    /* Predicate fast paths are keyed by the canonical core JanetFuncDef.
     * A different function must never match, even with the same arity. */
    {
        Janet nv = janet_wrap_nil();
        Janet pout = janet_wrap_nil();
        Janet pv = janet_wrap_function(lookup(env, "nil?"));
        janet_native_register_predicates(janet_core_env(NULL));
        assert(janet_native_core_predicate(pv, 1, &nv, &pout));
        assert(janet_checktype(pout, JANET_BOOLEAN) && janet_unwrap_boolean(pout));
        pout = janet_wrap_nil();
        assert(!janet_native_core_predicate(janet_wrap_function(sum), 1, &nv, &pout));
    }
#ifdef TEST_NATIVE_DISABLED
    assert(!janet_native_register(sum, &nx_10_code));
    assert(!sum->def->native_code);
    puts("default-off registration refused; compiler oracles passed");
    janet_deinit(); return 0;
#endif
    attach(sum, &nx_10_code); attach(nested, &nx_11_code);
    retirement_fixture(sum);
    /* A VM-only allocation at a later PC must not veto the native prefix. */
    JanetFunction *table = assembled(&nx_121_code);
    attach(table, &nx_121_code);
    Janet arg = janet_wrap_integer(3), result_table;
    JanetFiber *table_fiber = janet_fiber(table, 64, 1, &arg);
    janet_gcroot(janet_wrap_fiber(table_fiber));
    assert(janet_continue(table_fiber, janet_wrap_nil(), &result_table) == JANET_SIGNAL_OK);
    assert(janet_checktype(result_table, JANET_TABLE));
    assert(janet_equals(janet_table_get(janet_unwrap_table(result_table), janet_wrap_integer(1)),
                        janet_wrap_integer(0)));
    assert(nx_121_entries > 0 && nx_121_vmexits > 0);
    janet_gcunroot(janet_wrap_fiber(table_fiber));
    janet_gcunroot(janet_wrap_function(table));
    JanetFunction *bits = assembled(&nx_122_code);
    attach(bits, &nx_122_code);
    Janet bit_arg = janet_wrap_integer(4), result_bits;
    JanetFiber *bit_fiber = janet_fiber(bits, 64, 1, &bit_arg);
    janet_gcroot(janet_wrap_fiber(bit_fiber));
    assert(janet_continue(bit_fiber, janet_wrap_nil(), &result_bits) == JANET_SIGNAL_OK);
    assert(janet_equals(result_bits, janet_wrap_integer(3)));
    assert(nx_122_entries > 0 && nx_122_edges >= 4 && nx_122_ops > 0);
    janet_gcunroot(janet_wrap_fiber(bit_fiber));
    janet_gcunroot(janet_wrap_function(bits));
    assert(!janet_native_register(lookup(env, "unsupported-loop"), &nx_10_code));
    assert(!lookup(env, "boxed-caller")->def->native_code);
    /* A boxed caller must actually reach the generated executor. */
    uint64_t before = nx_10_entries;
    assert(!janet_dostring(env, "(assert (= 45 (boxed-caller 10)))", "caller", NULL));
    assert(nx_10_entries > before && nx_10_edges >= 10);
#ifndef JANET_NO_INTERPRETER_INTERRUPT
    assert(JANET_QUANTUM_BUDGET == 1 || JANET_QUANTUM_BUDGET == 2);
    parity(sum, &nx_10_code, JANET_QUANTUM_BUDGET, 10, 45);
    parity(nested, &nx_11_code, JANET_QUANTUM_BUDGET, 10, 870);
    JanetFunction *conditional = assembled(&nx_100_code);
    parity(conditional, &nx_100_code, JANET_QUANTUM_BUDGET, 4, 6);
    janet_gcunroot(janet_wrap_function(conditional));
    JanetFunction *self = assembled(&nx_101_code); attach(self, &nx_101_code);
    JanetFiber *self_fiber = spawn(self, 0); Janet self_out;
    janet_vm.quantum = 1;
    for (int i = 0; i < 3; i++) {
        assert(janet_continue(self_fiber, janet_wrap_nil(), &self_out) == JANET_SIGNAL_INTERRUPT);
        assert(pc_of(self_fiber) == 0);
    }
    assert(janet_continue_signal(self_fiber, janet_wrap_nil(), &self_out, JANET_SIGNAL_ERROR) == JANET_SIGNAL_ERROR);
    janet_gcunroot(janet_wrap_fiber(self_fiber)); janet_gcunroot(janet_wrap_function(self));
    fallback(sum, 0, 0); fallback(sum, 1, 0);
    fallback(sum, 0, 1); fallback(sum, 1, 1);
    assert(nx_10_fallbacks >= 2);
    /* Auto-suspend short circuits before spending any quantum. */
    JanetFiber *a = spawn(sum, 10), *b = spawn(sum, 10);
    Janet x, y;
    janet_vm.quantum = 2;
    janet_interpreter_interrupt(NULL);
    sum->def->native_code = NULL;
    assert(janet_continue(a, janet_wrap_nil(), &x) == JANET_SIGNAL_INTERRUPT);
    assert(janet_vm.quantum_remaining == JANET_QUANTUM_BUDGET);
    attach(sum, &nx_10_code);
    assert(janet_continue(b, janet_wrap_nil(), &y) == JANET_SIGNAL_INTERRUPT);
    assert(janet_vm.quantum_remaining == JANET_QUANTUM_BUDGET);
    same_parked(a, b);
    janet_interpreter_interrupt_handled(NULL);
    before = nx_10_entries;
    assert(janet_continue_signal(b, janet_wrap_integer(123), &y, JANET_SIGNAL_ERROR) == JANET_SIGNAL_ERROR);
    assert(janet_unwrap_number(y) == 123 && nx_10_entries == before);
    janet_vm.quantum = 0;
    assert(janet_continue(a, janet_wrap_nil(), &x) == JANET_SIGNAL_OK && janet_unwrap_number(x) == 45);
    janet_gcunroot(janet_wrap_fiber(a)); janet_gcunroot(janet_wrap_fiber(b));
    /* Interleaved ordinary frames retain independent scanned slots. */
    a = spawn(sum, 8); b = spawn(sum, 12); janet_vm.quantum = 2;
    int da = 0, db = 0;
    for (int i = 0; i < 32 && !(da && db); i++) {
        if (!db) { JanetSignal s = janet_continue(b, janet_wrap_nil(), &y); db = s == JANET_SIGNAL_OK; assert(db || s == JANET_SIGNAL_INTERRUPT); }
        janet_collect();
        if (!da) { JanetSignal s = janet_continue(a, janet_wrap_nil(), &x); da = s == JANET_SIGNAL_OK; assert(da || s == JANET_SIGNAL_INTERRUPT); }
        if (JANET_QUANTUM_BUDGET == 1) janet_vm.quantum = 0;
    }
    assert(da && db && janet_unwrap_number(x) == 28 && janet_unwrap_number(y) == 66);
    janet_gcunroot(janet_wrap_fiber(a)); janet_gcunroot(janet_wrap_fiber(b));
    /* Parked marshal drops machine addresses, retains original jump PC. */
    a = spawn(sum, 10);
    janet_vm.quantum = 1;
    assert(janet_continue(a, janet_wrap_nil(), &x) == JANET_SIGNAL_INTERRUPT);
    JanetBuffer *buf = janet_buffer(0); janet_gcroot(janet_wrap_buffer(buf));
    janet_marshal(buf, janet_wrap_fiber(a), NULL, 0);
    Janet restored = janet_unmarshal(buf->data, buf->count, 0, NULL, NULL);
    janet_gcroot(restored); b = janet_unwrap_fiber(restored);
    assert(!janet_fiber_frame(b)->func->def->native_code); same_parked(a, b);
    before = nx_10_entries; janet_vm.quantum = 0;
    assert(janet_continue(b, janet_wrap_nil(), &y) == JANET_SIGNAL_OK);
    assert(janet_unwrap_number(y) == 45 && nx_10_entries == before);
    janet_gcunroot(janet_wrap_fiber(a)); janet_gcunroot(restored); janet_gcunroot(janet_wrap_buffer(buf));
    puts("fallback once + GC, auto_suspend, cancellation, interleaving, parked marshal: passed");
#else
    Janet out; JanetFiber *f = spawn(nested, 10); janet_vm.quantum = 1;
    janet_interpreter_interrupt(NULL);
    assert(janet_continue(f, janet_wrap_nil(), &out) == JANET_SIGNAL_OK);
    assert(janet_unwrap_number(out) == 870 && nx_11_edges > 0);
    janet_interpreter_interrupt_handled(NULL); janet_vm.quantum = 0;
    janet_gcunroot(janet_wrap_fiber(f));
#endif
    /* Any mutation anywhere refuses entry. Breakpoint and step keep PCs. */
    janet_vm.quantum = 0;
    attach(sum, &nx_10_code);
    uint32_t old = sum->def->bytecode[4]; sum->def->bytecode[4] |= 0x80;
    assert(!janet_native_matches(sum->def, &nx_10_code));
    JanetFiber *debug = spawn(sum, 3); Janet result;
    before = nx_10_entries;
    assert(janet_continue(debug, janet_wrap_nil(), &result) == JANET_SIGNAL_DEBUG);
    assert(pc_of(debug) == 4 && nx_10_entries == before);
    assert(janet_step(debug, janet_wrap_nil(), &result) == JANET_SIGNAL_DEBUG);
    assert(pc_of(debug) == 5 && nx_10_entries == before);
    sum->def->bytecode[4] = old;
    assert(janet_continue(debug, janet_wrap_nil(), &result) == JANET_SIGNAL_OK);
    assert(janet_unwrap_number(result) == 3);
    janet_gcunroot(janet_wrap_fiber(debug));
    sum->def->bytecode[0] ^= 1u << 16;
    assert(!janet_native_matches(sum->def, &nx_10_code)); sum->def->bytecode[0] ^= 1u << 16;
    int flags = sum->def->flags; sum->def->flags ^= JANET_FUNCDEF_FLAG_TAG;
    assert(!janet_native_matches(sum->def, &nx_10_code)); sum->def->flags = flags;
    assert(janet_native_matches(sum->def, &nx_10_code));
    JanetFunction *fraction = lookup(env, "numeric-fraction");
    attach(fraction, &nx_14_code);
    Janet constant = fraction->def->constants[0];
    fraction->def->constants[0] = janet_wrap_number(-0.0);
    assert(!janet_native_matches(fraction->def, &nx_14_code));
    fraction->def->constants[0] = janet_wrap_nil();
    assert(!janet_native_register(fraction, &nx_14_code));
    assert(!fraction->def->native_code);
    fraction->def->constants[0] = constant; attach(fraction, &nx_14_code);
    assert(!janet_dostring(env, "(assert (= 3 (numeric-fraction 10)))", "constants", NULL));
    assert(nx_14_entries > 0 && nx_14_edges == 10);
    JanetFunction *primitives = assembled(&nx_102_code);
    attach(primitives, &nx_102_code);
    JanetFiber *pf = spawn(primitives, 0);
    assert(janet_continue(pf, janet_wrap_nil(), &result) == JANET_SIGNAL_OK);
    assert(janet_checktype(result, JANET_NIL));
    assert(nx_102_entries == 1 && nx_102_edges == 2);
    janet_gcunroot(janet_wrap_fiber(pf)); janet_gcunroot(janet_wrap_function(primitives));
    const JanetNativeCode *ops[] = {&nx_110_code, &nx_111_code, &nx_112_code, &nx_113_code,
                                   &nx_114_code, &nx_115_code, &nx_116_code, &nx_117_code};
    double values[] = {0, -0.0, 1, -2, INFINITY, -INFINITY, NAN};
    for (size_t op = 0; op < sizeof(ops)/sizeof(*ops); op++) {
        JanetFunction *f = assembled(ops[op]);
        for (size_t i = 0; i < sizeof(values)/sizeof(*values); i++) {
            for (size_t j = 0; j < sizeof(values)/sizeof(*values); j++) {
                Janet args[2] = {janet_wrap_number(values[i]), janet_wrap_number(values[j])};
                JanetFiber *fa = janet_fiber(f, 64, 2, args); janet_gcroot(janet_wrap_fiber(fa));
                JanetFiber *fb = janet_fiber(f, 64, 2, args); janet_gcroot(janet_wrap_fiber(fb));
                Janet ra, rb; f->def->native_code = NULL;
                assert(janet_continue(fa, janet_wrap_nil(), &ra) == JANET_SIGNAL_OK);
                attach(f, ops[op]);
                assert(janet_continue(fb, janet_wrap_nil(), &rb) == JANET_SIGNAL_OK);
                if (janet_checktype(ra, JANET_NUMBER)) {
                    double va = janet_unwrap_number(ra), vb = janet_unwrap_number(rb);
                    assert((isnan(va) && isnan(vb)) || !memcmp(&va, &vb, sizeof(double)));
                } else assert(janet_equals(ra, rb));
                janet_gcunroot(janet_wrap_fiber(fa)); janet_gcunroot(janet_wrap_fiber(fb));
            }
        }
        janet_gcunroot(janet_wrap_function(f));
    }
    /* Independent bit oracle for emitted constants, not runtime arguments. */
    const double expected_constants[] = {
        -0.0, 0.0, 0x1.fffffffffffffp1023, -0x1.fffffffffffffp1023,
        0x1p-1022, 0x1p-1074
    };
    assert(nx_120_code.constants_length == 6);
    for (int i = 0; i < 6; i++) {
        assert(nx_120_code.constants[i].type == JANET_NUMBER);
        assert(!memcmp(&nx_120_code.constants[i].number,
                       &expected_constants[i], sizeof(double)));
    }
    JanetFunction *roundtrip = assembled(&nx_120_code);
    attach(roundtrip, &nx_120_code);
    JanetFiber *rf = spawn(roundtrip, 0);
    assert(janet_continue(rf, janet_wrap_nil(), &result) == JANET_SIGNAL_OK);
    double actual_zero = janet_unwrap_number(result);
    assert(!memcmp(&actual_zero, &expected_constants[0], sizeof(double)));
    assert(nx_120_entries > 0);
    roundtrip->def->constants[0] = janet_wrap_number(0.0);
    assert(!janet_native_matches(roundtrip->def, &nx_120_code));
    janet_gcunroot(janet_wrap_fiber(rf));
    janet_gcunroot(janet_wrap_function(roundtrip));
    puts("emitted signed-zero/extreme finite/subnormal constant roundtrip passed");
    puts("primitive/conditional/self-edge oracles and 392 IEEE numeric pairs passed");
    printf("generated counters: sum entries=%llu backedges=%llu guards=%llu; nested entries=%llu backedges=%llu\n",
           (unsigned long long)nx_10_entries, (unsigned long long)nx_10_edges, (unsigned long long)nx_10_fallbacks,
           (unsigned long long)nx_11_entries, (unsigned long long)nx_11_edges);
    janet_deinit();
    return 0;
}
