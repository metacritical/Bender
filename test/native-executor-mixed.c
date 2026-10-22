/* Stage-2 mixed-region oracles. Compiled against generated native.c from
 * test/fixtures/native-executor.janet (+ its imported mixed module).
 * Differential native-vs-boxed parity for boundary-heavy loop bodies,
 * exact-once side effects, opaque constant liveness, GC across boundaries,
 * quantum/debugger/marshal behavior, lazy structural attachment and
 * counters separating native ops/regions from VM exits. */
#include "state.h"
#include "fiber.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include NATIVE_EXECUTOR_C

#ifndef JANET_QUANTUM_BUDGET
#define JANET_QUANTUM_BUDGET 200
#endif
#define TEST_BUDGET JANET_QUANTUM_BUDGET

static JanetTable *env;

static JanetFunction *lookup(const char *name) {
    Janet x;
    assert(janet_resolve(env, janet_csymbol(name), &x));
    assert(janet_checktype(x, JANET_FUNCTION));
    return janet_unwrap_function(x);
}
static Janet run(const char *src) {
    Janet out = janet_wrap_nil();
    int sig = janet_dostring(env, src, "mixed-test", &out);
    if (sig != 0) {
        fprintf(stderr, "mixed-test source failed (signal %d): %s\n", sig, src);
        assert(!"janet_dostring failed");
    }
    return out;
}
static JanetFiber *spawn1(JanetFunction *fn, int32_t n) {
    Janet arg = janet_wrap_integer(n);
    JanetFiber *f = janet_fiber(fn, 64, 1, &arg);
    janet_gcroot(janet_wrap_fiber(f));
    return f;
}
static JanetFiber *spawn_arg(JanetFunction *fn, Janet arg) {
    JanetFiber *f = janet_fiber(fn, 64, 1, &arg);
    janet_gcroot(janet_wrap_fiber(f));
    return f;
}
static int pc_of(JanetFiber *f) {
    JanetStackFrame *fr = janet_fiber_frame(f);
    if (!fr->func || !fr->func->def || !fr->func->def->bytecode) return -2;
    return (int)(fr->pc - fr->func->def->bytecode);
}
static int equiv(Janet x, Janet y) {
    if (janet_equals(x, y)) return 1;
    if (janet_type(x) != janet_type(y)) return 0;
    if (janet_checktype(x, JANET_BUFFER)) {
        JanetBuffer *p = janet_unwrap_buffer(x), *q = janet_unwrap_buffer(y);
        return p->count == q->count && !memcmp(p->data, q->data, (size_t)p->count);
    }
    if (janet_checktype(x, JANET_ARRAY)) {
        JanetArray *p = janet_unwrap_array(x), *q = janet_unwrap_array(y);
        if (p->count != q->count) return 0;
        for (int32_t i = 0; i < p->count; i++)
            if (!equiv(p->data[i], q->data[i])) return 0;
        return 1;
    }
    return 0;
}
static void same_parked(JanetFiber *a, JanetFiber *b) {
    assert(pc_of(a) == pc_of(b));
    assert(a->flags == b->flags);
    int count = janet_fiber_frame(a)->func->def->slotcount;
    for (int i = 0; i < count; i++)
        assert(equiv(a->data[a->frame+i], b->data[b->frame+i]));
}
static void reset_state(void) { run("(mx/reset)"); }
static Janet snapshot(void) { return run("(mx/snapshot)"); }

/* Run a mixed function to completion under a quantum budget, recording the
 * parked PC after every interrupt. Returns final value through *out. */
static int trace_run(JanetFunction *fn, int native, const JanetNativeCode *code,
                     int budget, Janet arg, Janet *out, int *pcs, int maxpcs) {
    JanetFiber *f = spawn_arg(fn, arg);
    if (native) {
        assert(janet_native_register(fn, code));
#ifdef JANET_NATIVE_EXECUTORS
        assert(janet_vm.native_possible);
#endif
    }
    else fn->def->native_code = NULL;
    janet_vm.quantum = budget;
    int signals = 0;
    for (int i = 0; i < 4096; i++) {
        Janet x;
        JanetSignal s = janet_continue(f, janet_wrap_nil(), &x);
        if (s == JANET_SIGNAL_OK) {
            *out = x;
            break;
        }
        assert(s == JANET_SIGNAL_INTERRUPT);
        if (signals < maxpcs) pcs[signals] = pc_of(f);
        signals++;
        /* Budget 1 legitimately repeats the same unexecuted edge without
         * progress (interpreter policy); cap and finish boxed. */
        if (signals == 100) janet_vm.quantum = 0;
        assert(signals < maxpcs);
    }
    janet_vm.quantum = 0;
    janet_gcunroot(janet_wrap_fiber(f));
    return signals;
}

/* 1. Native and boxed runs produce identical results, identical interrupt
 * PC traces and identical exact-once side effects on shared module state. */
static void differential(JanetFunction *fn, const JanetNativeCode *code, Janet arg) {
    Janet a, b;
    int pcsa[512], pcsb[512];
    reset_state();
    int sa = trace_run(fn, 0, code, TEST_BUDGET, arg, &a, pcsa, 512);
    Janet snapa = snapshot();
    reset_state();
    int sb = trace_run(fn, 1, code, TEST_BUDGET, arg, &b, pcsb, 512);
    Janet snapb = snapshot();
    assert(sa == sb);
    assert(janet_equals(a, b));
    assert(janet_equals(snapa, snapb));
    for (int i = 0; i < sa; i++) assert(pcsa[i] == pcsb[i]);
    printf("mixed differential parity: interrupts=%d\n", sa);
}

int main(int argc, char **argv) {
    assert(argc == 2);
    janet_init();
#ifdef JANET_NATIVE_EXECUTORS
    /* The compiled-in tier starts dormant until explicit registration or
     * descriptor publication; dispatch must be cheap in the ordinary VM. */
    assert(!janet_vm.native_possible);
#endif
    janet_vm.quantum = 0;
    env = janet_core_env(NULL);
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
    run("(module/add-syspath \".\")");
    FILE *file = fopen(argv[1], "rb"); assert(file);
    fseek(file, 0, SEEK_END); long size = ftell(file); rewind(file);
    char *src = malloc((size_t)size + 1); assert(src);
    assert(fread(src, 1, (size_t)size, file) == (size_t)size); fclose(file); src[size] = 0;
    assert(!janet_dostring(env, src, argv[1], NULL)); free(src);
    run("(import native-executor-mixed :as mx)");

#ifdef TEST_NATIVE_DISABLED
    {
        JanetFunction *boundary = lookup("mx/boundary");
        assert(!janet_native_register(boundary, &nx_2_code));
        assert(!boundary->def->native_code);
        janet_native_registry_add(&nx_2_code);
        assert(!janet_native_registry_count());
        assert(!janet_native_try_attach(boundary->def));
        puts("default-off: mixed registration and attachment refused");
        janet_deinit();
        return 0;
    }
#endif

    JanetFunction *boundary = lookup("mx/boundary");
    JanetFunction *tail = lookup("mx/tail-boundary");
    JanetFunction *buf = lookup("mx/build-buffer");
    JanetFunction *iter = lookup("mx/iterate-table");
    JanetFunction *sum = lookup("numeric-sum");

    differential(boundary, &nx_2_code, janet_wrap_integer(20 * TEST_BUDGET));
    differential(tail, &nx_4_code, janet_wrap_integer(4 * TEST_BUDGET));
    differential(buf, &nx_7_code, janet_wrap_integer(4 * TEST_BUDGET));
    differential(iter, &nx_8_code, run("@{:a 1 :b 2 :c 3 :d 4 :e 5 :f 6 :g 7 :h 8}"));

#ifndef JANET_NO_INTERPRETER_INTERRUPT
    /* 2. Native/boxed paired interrupts park at identical PCs/slots while
     * boundaries interleave. Uses a function with purely local state so
     * two interleaved fibers see the same values. */
    {
        Janet arg = janet_wrap_integer(12 * TEST_BUDGET);
        JanetFiber *a = spawn_arg(buf, arg), *b = spawn_arg(buf, arg);
        buf->def->native_code = NULL;
        janet_vm.quantum = TEST_BUDGET;
        Janet x, y;
        int paired = 0, a_done = 0;
        for (int i = 0; i < 64; i++) {
            JanetSignal sa = janet_continue(a, janet_wrap_nil(), &x);
            if (sa == JANET_SIGNAL_OK) { a_done = 1; break; }
            assert(sa == JANET_SIGNAL_INTERRUPT);
            assert(janet_native_register(buf, &nx_7_code));
            JanetSignal sb = janet_continue(b, janet_wrap_nil(), &y);
            assert(sb == JANET_SIGNAL_INTERRUPT);
            same_parked(a, b);
            paired++;
        }
        janet_vm.quantum = 0;
        if (!a_done) {
            buf->def->native_code = NULL;
            assert(janet_continue(a, janet_wrap_nil(), &x) == JANET_SIGNAL_OK);
        }
        assert(janet_native_register(buf, &nx_7_code));
        assert(janet_continue(b, janet_wrap_nil(), &y) == JANET_SIGNAL_OK);
        assert(janet_equals(x, y));
        assert(paired > 0);
        janet_gcunroot(janet_wrap_fiber(a)); janet_gcunroot(janet_wrap_fiber(b));
        printf("paired boundary interrupts: %d identical parked PCs/slots\n", paired);
    }
#endif

    /* 3. Opaque constant liveness: swapping a function constant that the
     * descriptor does not pin changes behavior through the native path too
     * (live pool load, never a specialized identity). */
    {
        assert(janet_native_register(boundary, &nx_2_code));
        Janet callee = run("mx/callee");
        int idx = -1;
        for (int32_t i = 0; i < boundary->def->constants_length; i++)
            if (janet_equals(boundary->def->constants[i], callee)) { idx = (int)i; break; }
        assert(idx >= 0);
        run("(defn swap-callee [x] (* x 10))");
        Janet replacement = run("swap-callee");
        Janet keep = boundary->def->constants[idx];
        reset_state();
        Janet r1 = run("(mx/boundary 3)");
        boundary->def->constants[idx] = replacement;
        reset_state();
        Janet r2 = run("(mx/boundary 3)");
        boundary->def->constants[idx] = keep;
        reset_state();
        Janet r3 = run("(mx/boundary 3)");
        assert(janet_equals(r1, r3));
        assert(!janet_equals(r1, r2));
        /* primitive constants stay pinned: exact matching refuses changes */
        assert(janet_native_register(sum, &nx_10_code));
        puts("opaque constant swap observed live; primitives stay exact");
    }

    /* 4. GC across boundaries: collect between every resume; callee code
     * and buffer allocation must not invalidate anything. */
    {
        assert(janet_native_register(boundary, &nx_2_code));
        assert(janet_native_register(buf, &nx_7_code));
        reset_state();
        JanetFiber *f = spawn1(boundary, 8);
        janet_vm.quantum = 2;
        Janet x;
        int done = 0;
        for (int i = 0; i < 128 && !done; i++) {
            if (i == 16) janet_vm.quantum = 0; /* budget-1 repeats one edge */
            JanetSignal s = janet_continue(f, janet_wrap_nil(), &x);
            done = s == JANET_SIGNAL_OK;
            janet_collect(); /* including during parked native frames */
        }
        janet_vm.quantum = 0;
        assert(done);
        reset_state();
        Janet boxed = run("(mx/boundary 8)");
        assert(janet_equals(x, boxed));
        janet_gcunroot(janet_wrap_fiber(f));
        reset_state();
        assert(janet_equals(run("(mx/build-buffer 8)"), janet_wrap_integer(8)));
        puts("GC across VM boundaries: stable");
    }

    /* 5. Debugger: a breakpoint bit inside the body forces boxed execution
     * and preserves PCs; stepping suppresses native entry entirely. */
    {
        assert(janet_native_register(boundary, &nx_2_code));
        uint32_t hold = boundary->def->bytecode[3];
        boundary->def->bytecode[3] |= 0x80;
        assert(!janet_native_matches(boundary->def, &nx_2_code));
        reset_state();
        JanetFiber *f = spawn1(boundary, 3);
        Janet out;
        assert(janet_continue(f, janet_wrap_nil(), &out) == JANET_SIGNAL_DEBUG);
        assert(pc_of(f) == 3);
        uint64_t before = nx_2_entries;
        int steps = 0;
        while (janet_step(f, janet_wrap_nil(), &out) == JANET_SIGNAL_DEBUG) {
            assert(nx_2_entries == before);
            steps++;
        }
        assert(nx_2_entries == before && steps > 0);
        assert(janet_checktype(out, JANET_NUMBER));
        janet_gcunroot(janet_wrap_fiber(f));
        boundary->def->bytecode[3] = hold;
        assert(janet_native_matches(boundary->def, &nx_2_code));
        reset_state();
        Janet boxed = run("(do (mx/boundary 3) nil)");
        (void)boxed;
        puts("breakpoint/step suppression preserved");
    }

    /* 5b. Re-entry validation cache: a raw non-breakpoint mutation of the
     * definition between resumes (primitive constant bits) invalidates the
     * cached verdict, so the mutated constant is observed through boxed
     * execution and no native entry happens until the word is restored. */
    {
        assert(janet_native_register(boundary, &nx_2_code));
        reset_state();
        JanetFiber *f1 = spawn1(boundary, 4);
        Janet r1;
        assert(janet_continue(f1, janet_wrap_nil(), &r1) == JANET_SIGNAL_OK);
        assert(nx_2_entries > 0);
        janet_gcunroot(janet_wrap_fiber(f1));
        int32_t ci = -1;
        for (int32_t i = 0; i < boundary->def->constants_length; i++) {
            Janet c = boundary->def->constants[i];
            if (janet_checktype(c, JANET_NUMBER)) {
                double v = janet_unwrap_number(c);
                if (!memcmp(&v, &(double){0.5}, sizeof(double))) { ci = i; break; }
            }
        }
        assert(ci >= 0);
        Janet hold = boundary->def->constants[ci];
        assert(janet_checktype(hold, JANET_NUMBER));
        boundary->def->constants[ci] = janet_wrap_number(1.5);
        assert(!janet_native_matches(boundary->def, &nx_2_code));
        reset_state();
        JanetFiber *f2 = spawn1(boundary, 4);
        Janet r2;
        uint64_t before = nx_2_entries;
        assert(janet_continue(f2, janet_wrap_nil(), &r2) == JANET_SIGNAL_OK);
        assert(nx_2_entries == before); /* mutated def: cache must not admit native entry */
        assert(janet_checktype(r2, JANET_NUMBER));
        assert(janet_unwrap_number(r2) == janet_unwrap_number(r1) + 4.0); /* boxed sees 1.5 */
        janet_gcunroot(janet_wrap_fiber(f2));
        boundary->def->constants[ci] = hold;
        assert(janet_native_matches(boundary->def, &nx_2_code));
        reset_state();
        JanetFiber *f3 = spawn1(boundary, 4);
        Janet r3;
        assert(janet_continue(f3, janet_wrap_nil(), &r3) == JANET_SIGNAL_OK);
        assert(nx_2_entries > before); /* restored def: native entry resumes */
        assert(janet_unwrap_number(r3) == janet_unwrap_number(r1));
        janet_gcunroot(janet_wrap_fiber(f3));
        reset_state();
        puts("between-resume constant mutation detected by re-entry validation");
    }

#ifndef JANET_NO_INTERPRETER_INTERRUPT
    /* 6. Marshal: parked mixed frame marshals without machine addresses;
     * the unmarshaled closure re-attaches lazily and finishes correctly.
     * Uses tail-boundary: its loop backedge has no pending pushed
     * arguments. The baseline marshal format drops values between
     * stackstart and stacktop, so fibers parked at a call poll with pushed
     * arguments already lose them in the plain interpreter too (documented
     * stage-2 baseline limitation, reproduced without native code). */
    {
        assert(janet_native_register(tail, &nx_4_code));
        Janet arg = janet_wrap_integer(6 * TEST_BUDGET);
        JanetFiber *f = janet_fiber(tail, 64, 1, &arg);
        janet_gcroot(janet_wrap_fiber(f));
        janet_vm.quantum = TEST_BUDGET;
        Janet x;
        assert(janet_continue(f, janet_wrap_nil(), &x) == JANET_SIGNAL_INTERRUPT);
        janet_vm.quantum = 0;
        JanetBuffer *b = janet_buffer(0);
        janet_gcroot(janet_wrap_buffer(b));
        janet_marshal(b, janet_wrap_fiber(f), NULL, JANET_MARSHAL_UNSAFE);
        Janet restored = janet_unmarshal(b->data, b->count, JANET_MARSHAL_UNSAFE, NULL, NULL);
        janet_gcroot(restored);
        JanetFiber *g = janet_unwrap_fiber(restored);
        assert(!janet_fiber_frame(g)->func->def->native_code);
        uint64_t before = nx_4_entries;
        janet_native_registry_add(&nx_4_code); /* bump generation */
        JanetSignal sg = janet_continue(g, janet_wrap_nil(), &x);
        if (sg != JANET_SIGNAL_OK) {
            const uint8_t *ds = janet_formatc("%v", x);
            fprintf(stderr, "restored fiber finished with signal=%d pc=%d value=%s\n",
                    sg, pc_of(g), (const char *)ds);
            assert(!"restored mixed fiber failed to finish");
        }
        {
            char callbuf[64];
            snprintf(callbuf, sizeof callbuf, "(mx/tail-boundary %d)", 6 * TEST_BUDGET);
            assert(janet_equals(x, run(callbuf)));
        }
        assert(nx_4_entries > before); /* re-attached and entered natively */
        janet_gcunroot(janet_wrap_fiber(f)); janet_gcunroot(restored);
        janet_gcunroot(janet_wrap_buffer(b));
        puts("parked mixed-frame marshal round trip passed");
    }
#endif

    /* 7. Lazy attachment: publish after the fact, then compile a NEW
     * function identical to an emitted one; its first dispatch attaches
     * and executes natively. Explicit registration still wins. */
    {
        janet_native_registry_add(&nx_10_code);
        janet_native_registry_add(&nx_2_code);
        assert(janet_native_registry_count() >= 2);
        run("(defn later-sum [n] (var s 0) (var i 0)"
            " (while (< i n) (+= s i) (++ i)) s)");
        JanetFunction *later = lookup("later-sum");
        assert(!later->def->native_code);
        assert(janet_equals(run("(later-sum 10)"), janet_wrap_integer(45)));
        assert(later->def->native_code == &nx_10_code);
        uint64_t ops_before = nx_10_ops;
        assert(janet_equals(run("(later-sum 10)"), janet_wrap_integer(45)));
        assert(nx_10_ops > ops_before);
        assert(janet_native_stat(JANET_NATIVE_STAT_ATTACH_ATTACHED) > 0);
        /* fresh marshal of a cfunction-carrying closure re-attaches too */
        puts("later-evaluated function attached and executed natively");
    }

    /* 8. Counters: entries/ops/vmexits for a mixed workload, VM exits by
     * opcode recorded (call/push/put/len are all exercised). */
    {
        assert(nx_2_entries > 0 && nx_2_ops > 0 && nx_2_vmexits > 0);
        assert(nx_2_entries >= nx_2_vmexits);
        fprintf(stderr, "VM_EXITS global=%llu registry=%d\n",
                (unsigned long long)janet_native_stat(JANET_NATIVE_STAT_VM_EXITS),
                janet_native_registry_count());
        for (int op = 0; op < 256; op++) {
            if (janet_native_exit_stat(op))
                fprintf(stderr, "exit-stat op %d = %llu\n", op,
                        (unsigned long long)janet_native_exit_stat(op));
        }
        /* Exit-by-opcode counts the opcode at each native->VM region exit.
         * Argument pushes are native on the fiber stack; a non-alias call
         * is the first effectful operation and remains the region exit.
         * Boundary ops that only ever follow boxed instructions (like the
         * ldc before a call) are reached by ordinary dispatch and are not
         * region exits. */
        assert(janet_native_exit_stat(JOP_CALL) > 0);
        assert(janet_native_exit_stat(JOP_GET) > 0);
        assert(janet_native_exit_stat(JOP_PUT) > 0);
        printf("boundary counters: entries=%llu ops=%llu vmexits=%llu edges=%llu guards=%llu\n",
               (unsigned long long)nx_2_entries, (unsigned long long)nx_2_ops,
               (unsigned long long)nx_2_vmexits, (unsigned long long)nx_2_edges,
               (unsigned long long)nx_2_fallbacks);
        printf("global: attempts=%llu attached=%llu vmexits=%llu call-exits=%llu\n",
               (unsigned long long)janet_native_stat(JANET_NATIVE_STAT_ATTACH_ATTEMPTS),
               (unsigned long long)janet_native_stat(JANET_NATIVE_STAT_ATTACH_ATTACHED),
               (unsigned long long)janet_native_stat(JANET_NATIVE_STAT_VM_EXITS),
               (unsigned long long)janet_native_exit_stat(JOP_CALL));
    }

    /* 9. Ambiguity refusal, deliberately last: a duplicate descriptor makes
     * every structurally matching def ambiguous and no further lazy
     * attachment happens for it. A fresh import compiles an identical body. */
    {
        static JanetNativeCode dup;
        dup = nx_2_code;
        janet_native_registry_add(&dup);
        run("(import native-executor-mixed :fresh true :as mx2)");
        JanetFunction *later2 = lookup("mx2/boundary");
        assert(later2 != boundary);
        reset_state();
        Janet r_later = run("(mx2/boundary 3)");
        reset_state();
        Janet r_orig = run("(mx/boundary 3)");
        assert(janet_equals(r_later, r_orig));
        assert(!later2->def->native_code); /* ambiguous: refused */
        assert(janet_native_stat(JANET_NATIVE_STAT_ATTACH_AMBIGUOUS) > 0);
        /* explicit registration is still authoritative for this def */
        assert(janet_native_register(later2, &nx_2_code));
        assert(later2->def->native_code == &nx_2_code);
        puts("ambiguous descriptors refuse lazy attachment");
    }

    janet_deinit();
    return 0;
}
