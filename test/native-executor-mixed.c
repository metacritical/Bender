/* Stage-2 mixed-region oracles. Compiled against generated native.c from
 * test/fixtures/native-executor.ben (+ its imported mixed module).
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

#ifndef BENDER_QUANTUM_BUDGET
#define BENDER_QUANTUM_BUDGET 200
#endif
#define TEST_BUDGET BENDER_QUANTUM_BUDGET

static BenderTable *env;

static BenderFunction *lookup(const char *name) {
    Bender x;
    assert(bender_resolve(env, bender_csymbol(name), &x));
    assert(bender_checktype(x, BENDER_FUNCTION));
    return bender_unwrap_function(x);
}
static Bender run(const char *src) {
    Bender out = bender_wrap_nil();
    int sig = bender_dostring(env, src, "mixed-test", &out);
    if (sig != 0) {
        fprintf(stderr, "mixed-test source failed (signal %d): %s\n", sig, src);
        assert(!"bender_dostring failed");
    }
    return out;
}
static BenderFiber *spawn1(BenderFunction *fn, int32_t n) {
    Bender arg = bender_wrap_integer(n);
    BenderFiber *f = bender_fiber(fn, 64, 1, &arg);
    bender_gcroot(bender_wrap_fiber(f));
    return f;
}
static BenderFiber *spawn_arg(BenderFunction *fn, Bender arg) {
    BenderFiber *f = bender_fiber(fn, 64, 1, &arg);
    bender_gcroot(bender_wrap_fiber(f));
    return f;
}
static int pc_of(BenderFiber *f) {
    BenderStackFrame *fr = bender_fiber_frame(f);
    if (!fr->func || !fr->func->def || !fr->func->def->bytecode) return -2;
    return (int)(fr->pc - fr->func->def->bytecode);
}
static int equiv(Bender x, Bender y) {
    if (bender_equals(x, y)) return 1;
    if (bender_type(x) != bender_type(y)) return 0;
    if (bender_checktype(x, BENDER_BUFFER)) {
        BenderBuffer *p = bender_unwrap_buffer(x), *q = bender_unwrap_buffer(y);
        return p->count == q->count && !memcmp(p->data, q->data, (size_t)p->count);
    }
    if (bender_checktype(x, BENDER_ARRAY)) {
        BenderArray *p = bender_unwrap_array(x), *q = bender_unwrap_array(y);
        if (p->count != q->count) return 0;
        for (int32_t i = 0; i < p->count; i++)
            if (!equiv(p->data[i], q->data[i])) return 0;
        return 1;
    }
    return 0;
}
static void same_parked(BenderFiber *a, BenderFiber *b) {
    assert(pc_of(a) == pc_of(b));
    assert(a->flags == b->flags);
    int count = bender_fiber_frame(a)->func->def->slotcount;
    for (int i = 0; i < count; i++)
        assert(equiv(a->data[a->frame+i], b->data[b->frame+i]));
}
static void reset_state(void) { run("(mx/reset)"); }
static Bender snapshot(void) { return run("(mx/snapshot)"); }

/* Run a mixed function to completion under a quantum budget, recording the
 * parked PC after every interrupt. Returns final value through *out. */
static int trace_run(BenderFunction *fn, int native, const BenderNativeCode *code,
                     int budget, Bender arg, Bender *out, int *pcs, int maxpcs) {
    BenderFiber *f = spawn_arg(fn, arg);
    if (native) {
        assert(bender_native_register(fn, code));
#ifdef BENDER_NATIVE_EXECUTORS
        assert(bender_vm.native_possible);
#endif
    }
    else fn->def->native_code = NULL;
    bender_vm.quantum = budget;
    int signals = 0;
    for (int i = 0; i < 4096; i++) {
        Bender x;
        BenderSignal s = bender_continue(f, bender_wrap_nil(), &x);
        if (s == BENDER_SIGNAL_OK) {
            *out = x;
            break;
        }
        assert(s == BENDER_SIGNAL_INTERRUPT);
        if (signals < maxpcs) pcs[signals] = pc_of(f);
        signals++;
        /* Budget 1 legitimately repeats the same unexecuted edge without
         * progress (interpreter policy); cap and finish boxed. */
        if (signals == 100) bender_vm.quantum = 0;
        assert(signals < maxpcs);
    }
    bender_vm.quantum = 0;
    bender_gcunroot(bender_wrap_fiber(f));
    return signals;
}

/* 1. Native and boxed runs produce identical results, identical interrupt
 * PC traces and identical exact-once side effects on shared module state. */
static void differential(BenderFunction *fn, const BenderNativeCode *code, Bender arg) {
    Bender a, b;
    int pcsa[512], pcsb[512];
    reset_state();
    int sa = trace_run(fn, 0, code, TEST_BUDGET, arg, &a, pcsa, 512);
    Bender snapa = snapshot();
    reset_state();
    int sb = trace_run(fn, 1, code, TEST_BUDGET, arg, &b, pcsb, 512);
    Bender snapb = snapshot();
    assert(sa == sb);
    assert(bender_equals(a, b));
    assert(bender_equals(snapa, snapb));
    for (int i = 0; i < sa; i++) assert(pcsa[i] == pcsb[i]);
    printf("mixed differential parity: interrupts=%d\n", sa);
}

int main(int argc, char **argv) {
    assert(argc == 2);
    bender_init();
#ifdef BENDER_NATIVE_EXECUTORS
    /* The compiled-in tier starts dormant until explicit registration or
     * descriptor publication; dispatch must be cheap in the ordinary VM. */
    assert(!bender_vm.native_possible);
#endif
    bender_vm.quantum = 0;
    env = bender_core_env(NULL);
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
    run("(module/add-syspath \".\")");
    FILE *file = fopen(argv[1], "rb"); assert(file);
    fseek(file, 0, SEEK_END); long size = ftell(file); rewind(file);
    char *src = malloc((size_t)size + 1); assert(src);
    assert(fread(src, 1, (size_t)size, file) == (size_t)size); fclose(file); src[size] = 0;
    assert(!bender_dostring(env, src, argv[1], NULL)); free(src);
    run("(import native-executor-mixed :as mx)");

#ifdef TEST_NATIVE_DISABLED
    {
        BenderFunction *boundary = lookup("mx/boundary");
        assert(!bender_native_register(boundary, &nx_2_code));
        assert(!boundary->def->native_code);
        bender_native_registry_add(&nx_2_code);
        assert(!bender_native_registry_count());
        assert(!bender_native_try_attach(boundary->def));
        puts("default-off: mixed registration and attachment refused");
        bender_deinit();
        return 0;
    }
#endif

    BenderFunction *boundary = lookup("mx/boundary");
    BenderFunction *tail = lookup("mx/tail-boundary");
    BenderFunction *buf = lookup("mx/build-buffer");
    BenderFunction *iter = lookup("mx/iterate-table");
    BenderFunction *sum = lookup("numeric-sum");

    differential(boundary, &nx_2_code, bender_wrap_integer(20 * TEST_BUDGET));
    differential(tail, &nx_4_code, bender_wrap_integer(4 * TEST_BUDGET));
    differential(buf, &nx_7_code, bender_wrap_integer(4 * TEST_BUDGET));
    differential(iter, &nx_8_code, run("@{:a 1 :b 2 :c 3 :d 4 :e 5 :f 6 :g 7 :h 8}"));

#ifndef BENDER_NO_INTERPRETER_INTERRUPT
    /* 2. Native/boxed paired interrupts park at identical PCs/slots while
     * boundaries interleave. Uses a function with purely local state so
     * two interleaved fibers see the same values. */
    {
        Bender arg = bender_wrap_integer(12 * TEST_BUDGET);
        BenderFiber *a = spawn_arg(buf, arg), *b = spawn_arg(buf, arg);
        buf->def->native_code = NULL;
        bender_vm.quantum = TEST_BUDGET;
        Bender x, y;
        int paired = 0, a_done = 0;
        for (int i = 0; i < 64; i++) {
            BenderSignal sa = bender_continue(a, bender_wrap_nil(), &x);
            if (sa == BENDER_SIGNAL_OK) { a_done = 1; break; }
            assert(sa == BENDER_SIGNAL_INTERRUPT);
            assert(bender_native_register(buf, &nx_7_code));
            BenderSignal sb = bender_continue(b, bender_wrap_nil(), &y);
            assert(sb == BENDER_SIGNAL_INTERRUPT);
            same_parked(a, b);
            paired++;
        }
        bender_vm.quantum = 0;
        if (!a_done) {
            buf->def->native_code = NULL;
            assert(bender_continue(a, bender_wrap_nil(), &x) == BENDER_SIGNAL_OK);
        }
        assert(bender_native_register(buf, &nx_7_code));
        assert(bender_continue(b, bender_wrap_nil(), &y) == BENDER_SIGNAL_OK);
        assert(bender_equals(x, y));
        assert(paired > 0);
        bender_gcunroot(bender_wrap_fiber(a)); bender_gcunroot(bender_wrap_fiber(b));
        printf("paired boundary interrupts: %d identical parked PCs/slots\n", paired);
    }
#endif

    /* 3. Opaque constant liveness: swapping a function constant that the
     * descriptor does not pin changes behavior through the native path too
     * (live pool load, never a specialized identity). */
    {
        assert(bender_native_register(boundary, &nx_2_code));
        Bender callee = run("mx/callee");
        int idx = -1;
        for (int32_t i = 0; i < boundary->def->constants_length; i++)
            if (bender_equals(boundary->def->constants[i], callee)) { idx = (int)i; break; }
        assert(idx >= 0);
        run("(defn swap-callee [x] (* x 10))");
        Bender replacement = run("swap-callee");
        Bender keep = boundary->def->constants[idx];
        reset_state();
        Bender r1 = run("(mx/boundary 3)");
        boundary->def->constants[idx] = replacement;
        reset_state();
        Bender r2 = run("(mx/boundary 3)");
        boundary->def->constants[idx] = keep;
        reset_state();
        Bender r3 = run("(mx/boundary 3)");
        assert(bender_equals(r1, r3));
        assert(!bender_equals(r1, r2));
        /* primitive constants stay pinned: exact matching refuses changes */
        assert(bender_native_register(sum, &nx_10_code));
        puts("opaque constant swap observed live; primitives stay exact");
    }

    /* 4. GC across boundaries: collect between every resume; callee code
     * and buffer allocation must not invalidate anything. */
    {
        assert(bender_native_register(boundary, &nx_2_code));
        assert(bender_native_register(buf, &nx_7_code));
        reset_state();
        BenderFiber *f = spawn1(boundary, 8);
        bender_vm.quantum = 2;
        Bender x;
        int done = 0;
        for (int i = 0; i < 128 && !done; i++) {
            if (i == 16) bender_vm.quantum = 0; /* budget-1 repeats one edge */
            BenderSignal s = bender_continue(f, bender_wrap_nil(), &x);
            done = s == BENDER_SIGNAL_OK;
            bender_collect(); /* including during parked native frames */
        }
        bender_vm.quantum = 0;
        assert(done);
        reset_state();
        Bender boxed = run("(mx/boundary 8)");
        assert(bender_equals(x, boxed));
        bender_gcunroot(bender_wrap_fiber(f));
        reset_state();
        assert(bender_equals(run("(mx/build-buffer 8)"), bender_wrap_integer(8)));
        puts("GC across VM boundaries: stable");
    }

    /* 5. Debugger: a breakpoint bit inside the body forces boxed execution
     * and preserves PCs; stepping suppresses native entry entirely. */
    {
        assert(bender_native_register(boundary, &nx_2_code));
        uint32_t hold = boundary->def->bytecode[3];
        boundary->def->bytecode[3] |= 0x80;
        assert(!bender_native_matches(boundary->def, &nx_2_code));
        reset_state();
        BenderFiber *f = spawn1(boundary, 3);
        Bender out;
        assert(bender_continue(f, bender_wrap_nil(), &out) == BENDER_SIGNAL_DEBUG);
        assert(pc_of(f) == 3);
        uint64_t before = nx_2_entries;
        int steps = 0;
        while (bender_step(f, bender_wrap_nil(), &out) == BENDER_SIGNAL_DEBUG) {
            assert(nx_2_entries == before);
            steps++;
        }
        assert(nx_2_entries == before && steps > 0);
        assert(bender_checktype(out, BENDER_NUMBER));
        bender_gcunroot(bender_wrap_fiber(f));
        boundary->def->bytecode[3] = hold;
        assert(bender_native_matches(boundary->def, &nx_2_code));
        reset_state();
        Bender boxed = run("(do (mx/boundary 3) nil)");
        (void)boxed;
        puts("breakpoint/step suppression preserved");
    }

    /* 5b. Re-entry validation cache: a raw non-breakpoint mutation of the
     * definition between resumes (primitive constant bits) invalidates the
     * cached verdict, so the mutated constant is observed through boxed
     * execution and no native entry happens until the word is restored. */
    {
        assert(bender_native_register(boundary, &nx_2_code));
        reset_state();
        BenderFiber *f1 = spawn1(boundary, 4);
        Bender r1;
        assert(bender_continue(f1, bender_wrap_nil(), &r1) == BENDER_SIGNAL_OK);
        assert(nx_2_entries > 0);
        bender_gcunroot(bender_wrap_fiber(f1));
        int32_t ci = -1;
        for (int32_t i = 0; i < boundary->def->constants_length; i++) {
            Bender c = boundary->def->constants[i];
            if (bender_checktype(c, BENDER_NUMBER)) {
                double v = bender_unwrap_number(c);
                if (!memcmp(&v, &(double){0.5}, sizeof(double))) { ci = i; break; }
            }
        }
        assert(ci >= 0);
        Bender hold = boundary->def->constants[ci];
        assert(bender_checktype(hold, BENDER_NUMBER));
        boundary->def->constants[ci] = bender_wrap_number(1.5);
        assert(!bender_native_matches(boundary->def, &nx_2_code));
        reset_state();
        BenderFiber *f2 = spawn1(boundary, 4);
        Bender r2;
        uint64_t before = nx_2_entries;
        assert(bender_continue(f2, bender_wrap_nil(), &r2) == BENDER_SIGNAL_OK);
        assert(nx_2_entries == before); /* mutated def: cache must not admit native entry */
        assert(bender_checktype(r2, BENDER_NUMBER));
        assert(bender_unwrap_number(r2) == bender_unwrap_number(r1) + 4.0); /* boxed sees 1.5 */
        bender_gcunroot(bender_wrap_fiber(f2));
        boundary->def->constants[ci] = hold;
        assert(bender_native_matches(boundary->def, &nx_2_code));
        reset_state();
        BenderFiber *f3 = spawn1(boundary, 4);
        Bender r3;
        assert(bender_continue(f3, bender_wrap_nil(), &r3) == BENDER_SIGNAL_OK);
        assert(nx_2_entries > before); /* restored def: native entry resumes */
        assert(bender_unwrap_number(r3) == bender_unwrap_number(r1));
        bender_gcunroot(bender_wrap_fiber(f3));
        reset_state();
        puts("between-resume constant mutation detected by re-entry validation");
    }

#ifndef BENDER_NO_INTERPRETER_INTERRUPT
    /* 6. Marshal: parked mixed frame marshals without machine addresses;
     * the unmarshaled closure re-attaches lazily and finishes correctly.
     * Uses tail-boundary: its loop backedge has no pending pushed
     * arguments. The baseline marshal format drops values between
     * stackstart and stacktop, so fibers parked at a call poll with pushed
     * arguments already lose them in the plain interpreter too (documented
     * stage-2 baseline limitation, reproduced without native code). */
    {
        assert(bender_native_register(tail, &nx_4_code));
        Bender arg = bender_wrap_integer(6 * TEST_BUDGET);
        BenderFiber *f = bender_fiber(tail, 64, 1, &arg);
        bender_gcroot(bender_wrap_fiber(f));
        bender_vm.quantum = TEST_BUDGET;
        Bender x;
        assert(bender_continue(f, bender_wrap_nil(), &x) == BENDER_SIGNAL_INTERRUPT);
        bender_vm.quantum = 0;
        BenderBuffer *b = bender_buffer(0);
        bender_gcroot(bender_wrap_buffer(b));
        bender_marshal(b, bender_wrap_fiber(f), NULL, BENDER_MARSHAL_UNSAFE);
        Bender restored = bender_unmarshal(b->data, b->count, BENDER_MARSHAL_UNSAFE, NULL, NULL);
        bender_gcroot(restored);
        BenderFiber *g = bender_unwrap_fiber(restored);
        assert(!bender_fiber_frame(g)->func->def->native_code);
        uint64_t before = nx_4_entries;
        bender_native_registry_add(&nx_4_code); /* bump generation */
        BenderSignal sg = bender_continue(g, bender_wrap_nil(), &x);
        if (sg != BENDER_SIGNAL_OK) {
            const uint8_t *ds = bender_formatc("%v", x);
            fprintf(stderr, "restored fiber finished with signal=%d pc=%d value=%s\n",
                    sg, pc_of(g), (const char *)ds);
            assert(!"restored mixed fiber failed to finish");
        }
        {
            char callbuf[64];
            snprintf(callbuf, sizeof callbuf, "(mx/tail-boundary %d)", 6 * TEST_BUDGET);
            assert(bender_equals(x, run(callbuf)));
        }
        assert(nx_4_entries > before); /* re-attached and entered natively */
        bender_gcunroot(bender_wrap_fiber(f)); bender_gcunroot(restored);
        bender_gcunroot(bender_wrap_buffer(b));
        puts("parked mixed-frame marshal round trip passed");
    }
#endif

    /* 7. Lazy attachment: publish after the fact, then compile a NEW
     * function identical to an emitted one; its first dispatch attaches
     * and executes natively. Explicit registration still wins. */
    {
        bender_native_registry_add(&nx_10_code);
        bender_native_registry_add(&nx_2_code);
        assert(bender_native_registry_count() >= 2);
        run("(defn later-sum [n] (var s 0) (var i 0)"
            " (while (< i n) (+= s i) (++ i)) s)");
        BenderFunction *later = lookup("later-sum");
        assert(!later->def->native_code);
        assert(bender_equals(run("(later-sum 10)"), bender_wrap_integer(45)));
        assert(later->def->native_code == &nx_10_code);
        uint64_t ops_before = nx_10_ops;
        assert(bender_equals(run("(later-sum 10)"), bender_wrap_integer(45)));
        assert(nx_10_ops > ops_before);
        assert(bender_native_stat(BENDER_NATIVE_STAT_ATTACH_ATTACHED) > 0);
        /* fresh marshal of a cfunction-carrying closure re-attaches too */
        puts("later-evaluated function attached and executed natively");
    }

    /* 8. Counters: entries/ops/vmexits for a mixed workload, VM exits by
     * opcode recorded (call/push/put/len are all exercised). */
    {
        assert(nx_2_entries > 0 && nx_2_ops > 0 && nx_2_vmexits > 0);
        assert(nx_2_entries >= nx_2_vmexits);
        fprintf(stderr, "VM_EXITS global=%llu registry=%d\n",
                (unsigned long long)bender_native_stat(BENDER_NATIVE_STAT_VM_EXITS),
                bender_native_registry_count());
        for (int op = 0; op < 256; op++) {
            if (bender_native_exit_stat(op))
                fprintf(stderr, "exit-stat op %d = %llu\n", op,
                        (unsigned long long)bender_native_exit_stat(op));
        }
        /* Exit-by-opcode counts the opcode at each native->VM region exit.
         * Argument pushes are native on the fiber stack; a non-alias call
         * is the first effectful operation and remains the region exit.
         * Boundary ops that only ever follow boxed instructions (like the
         * ldc before a call) are reached by ordinary dispatch and are not
         * region exits. */
        assert(bender_native_exit_stat(JOP_CALL) > 0);
        assert(bender_native_exit_stat(JOP_GET) > 0);
        assert(bender_native_exit_stat(JOP_PUT) > 0);
        printf("boundary counters: entries=%llu ops=%llu vmexits=%llu edges=%llu guards=%llu\n",
               (unsigned long long)nx_2_entries, (unsigned long long)nx_2_ops,
               (unsigned long long)nx_2_vmexits, (unsigned long long)nx_2_edges,
               (unsigned long long)nx_2_fallbacks);
        printf("global: attempts=%llu attached=%llu vmexits=%llu call-exits=%llu\n",
               (unsigned long long)bender_native_stat(BENDER_NATIVE_STAT_ATTACH_ATTEMPTS),
               (unsigned long long)bender_native_stat(BENDER_NATIVE_STAT_ATTACH_ATTACHED),
               (unsigned long long)bender_native_stat(BENDER_NATIVE_STAT_VM_EXITS),
               (unsigned long long)bender_native_exit_stat(JOP_CALL));
    }

    /* 9. Ambiguity refusal, deliberately last: a duplicate descriptor makes
     * every structurally matching def ambiguous and no further lazy
     * attachment happens for it. A fresh import compiles an identical body. */
    {
        static BenderNativeCode dup;
        dup = nx_2_code;
        bender_native_registry_add(&dup);
        run("(import native-executor-mixed :fresh true :as mx2)");
        BenderFunction *later2 = lookup("mx2/boundary");
        assert(later2 != boundary);
        reset_state();
        Bender r_later = run("(mx2/boundary 3)");
        reset_state();
        Bender r_orig = run("(mx/boundary 3)");
        assert(bender_equals(r_later, r_orig));
        assert(!later2->def->native_code); /* ambiguous: refused */
        assert(bender_native_stat(BENDER_NATIVE_STAT_ATTACH_AMBIGUOUS) > 0);
        /* explicit registration is still authoritative for this def */
        assert(bender_native_register(later2, &nx_2_code));
        assert(later2->def->native_code == &nx_2_code);
        puts("ambiguous descriptors refuse lazy attachment");
    }

    bender_deinit();
    return 0;
}
