/* Test-only one-shot poll selector: the seeded budget selects a dynamic
 * eligible poll ordinal, then the edge is allowed to execute on resume.
 * Never run blanket budget 1 indefinitely: it retries the same edge. */
#include "state.h"
#include "fiber.h"
#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
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
static int pc(BenderFiber *f) {
    BenderStackFrame *fr = bender_fiber_frame(f);
    return (int)(fr->pc - fr->func->def->bytecode);
}
static void parked(BenderFiber *a, BenderFiber *b) {
    assert(pc(a) == pc(b) && a->flags == b->flags);
    assert(a->frame == b->frame && a->stacktop == b->stacktop);
    BenderFuncDef *da = bender_fiber_frame(a)->func->def;
    assert(da == bender_fiber_frame(b)->func->def);
    for (int i = 0; i < da->slotcount; i++) {
        Bender x = a->data[a->frame + i], y = b->data[b->frame + i];
        assert(bender_type(x) == bender_type(y));
        if (bender_checktype(x, BENDER_NUMBER)) {
            double u = bender_unwrap_number(x), v = bender_unwrap_number(y);
            assert(!memcmp(&u, &v, sizeof u));
        } else assert(bender_equals(x, y));
    }
    for (int i = a->stackstart; i < a->stacktop; i++)
        assert(bender_equals(a->data[i], b->data[i]));
}
static uint64_t replay(BenderFunction *fn, const BenderNativeCode *code, int seed) {
    uint64_t trace = UINT64_C(1469598103934665603);
    int hits = 0;
    for (int pass = 0; pass < 12; pass++) {
        /* Rotate budget 1..6; each pass selects at most one eligible poll. */
        int ordinal = 1 + (seed + pass * 5) % 6;
        BenderFiber *a = spawn(fn, 16), *b = spawn(fn, 16);
        Bender x, y;
        fn->def->native_code = NULL;
        bender_vm.quantum = ordinal;
        BenderSignal sa = bender_continue(a, bender_wrap_nil(), &x);
        assert(bender_native_register(fn, code));
        BenderSignal sb = bender_continue(b, bender_wrap_nil(), &y);
        assert(sa == sb);
        assert(sa == BENDER_SIGNAL_INTERRUPT);
        parked(a, b);
        hits++;
        trace = (trace ^ (uint64_t)(ordinal * 1024 + pc(a))) * UINT64_C(1099511628211);
        /* One-shot disarm ensures the unexecuted edge makes progress. */
        bender_vm.quantum = 0;
        fn->def->native_code = NULL;
        assert(bender_continue(a, bender_wrap_nil(), &x) == BENDER_SIGNAL_OK);
        assert(bender_native_register(fn, code));
        assert(bender_continue(b, bender_wrap_nil(), &y) == BENDER_SIGNAL_OK);
        assert(bender_equals(x, y));
        trace = (trace ^ (uint64_t)bender_unwrap_number(x)) * UINT64_C(1099511628211);
        bender_gcunroot(bender_wrap_fiber(a));
        bender_gcunroot(bender_wrap_fiber(b));
    }
    assert(hits == 12);
    printf("forced definition slots=%d seed=%d hits=%d trace=%016" PRIx64 "\n",
           code->slotcount, seed, hits, trace);
    return trace;
}
int main(int argc, char **argv) {
    assert(argc == 3);
    int seed = atoi(argv[2]);
    assert(seed >= 0 && seed < 1000000);
    bender_init();
    bender_vm.quantum = 0;
    BenderTable *env = bender_core_env(NULL);
    bender_gcroot(bender_wrap_table(env));
    char *dir = strdup(argv[1]);
    char *slash = strrchr(dir, '/');
    if (slash) { *slash = 0; assert(!chdir(dir)); }
    FILE *file = fopen(slash ? slash + 1 : argv[1], "rb"); assert(file);
    fseek(file, 0, SEEK_END); long size = ftell(file); rewind(file);
    char *src = malloc((size_t)size + 1); assert(src);
    assert(fread(src, 1, (size_t)size, file) == (size_t)size);
    fclose(file); src[size] = 0;
    assert(!bender_dostring(env, src, "native-executor.ben", NULL)); free(src);
    BenderFunction *sum = lookup(env, "numeric-sum");
    BenderFunction *nested = lookup(env, "numeric-nested");
    uint64_t before = nx_10_entries;
    replay(sum, &nx_10_code, seed);
    assert(nx_10_entries > before && nx_10_ops > 0 && nx_10_edges > 0);
    before = nx_11_entries;
    replay(nested, &nx_11_code, seed);
    assert(nx_11_entries > before && nx_11_ops > 0 && nx_11_edges > 0);
    bender_vm.quantum = 0;
    bender_gcunroot(bender_wrap_table(env));
    bender_deinit();
    return 0;
}
