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
static int pc(JanetFiber *f) {
    JanetStackFrame *fr = janet_fiber_frame(f);
    return (int)(fr->pc - fr->func->def->bytecode);
}
static void parked(JanetFiber *a, JanetFiber *b) {
    assert(pc(a) == pc(b) && a->flags == b->flags);
    assert(a->frame == b->frame && a->stacktop == b->stacktop);
    JanetFuncDef *da = janet_fiber_frame(a)->func->def;
    assert(da == janet_fiber_frame(b)->func->def);
    for (int i = 0; i < da->slotcount; i++) {
        Janet x = a->data[a->frame + i], y = b->data[b->frame + i];
        assert(janet_type(x) == janet_type(y));
        if (janet_checktype(x, JANET_NUMBER)) {
            double u = janet_unwrap_number(x), v = janet_unwrap_number(y);
            assert(!memcmp(&u, &v, sizeof u));
        } else assert(janet_equals(x, y));
    }
    for (int i = a->stackstart; i < a->stacktop; i++)
        assert(janet_equals(a->data[i], b->data[i]));
}
static uint64_t replay(JanetFunction *fn, const JanetNativeCode *code, int seed) {
    uint64_t trace = UINT64_C(1469598103934665603);
    int hits = 0;
    for (int pass = 0; pass < 12; pass++) {
        /* Rotate budget 1..6; each pass selects at most one eligible poll. */
        int ordinal = 1 + (seed + pass * 5) % 6;
        JanetFiber *a = spawn(fn, 16), *b = spawn(fn, 16);
        Janet x, y;
        fn->def->native_code = NULL;
        janet_vm.quantum = ordinal;
        JanetSignal sa = janet_continue(a, janet_wrap_nil(), &x);
        assert(janet_native_register(fn, code));
        JanetSignal sb = janet_continue(b, janet_wrap_nil(), &y);
        assert(sa == sb);
        assert(sa == JANET_SIGNAL_INTERRUPT);
        parked(a, b);
        hits++;
        trace = (trace ^ (uint64_t)(ordinal * 1024 + pc(a))) * UINT64_C(1099511628211);
        /* One-shot disarm ensures the unexecuted edge makes progress. */
        janet_vm.quantum = 0;
        fn->def->native_code = NULL;
        assert(janet_continue(a, janet_wrap_nil(), &x) == JANET_SIGNAL_OK);
        assert(janet_native_register(fn, code));
        assert(janet_continue(b, janet_wrap_nil(), &y) == JANET_SIGNAL_OK);
        assert(janet_equals(x, y));
        trace = (trace ^ (uint64_t)janet_unwrap_number(x)) * UINT64_C(1099511628211);
        janet_gcunroot(janet_wrap_fiber(a));
        janet_gcunroot(janet_wrap_fiber(b));
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
    janet_init();
    janet_vm.quantum = 0;
    JanetTable *env = janet_core_env(NULL);
    janet_gcroot(janet_wrap_table(env));
    char *dir = strdup(argv[1]);
    char *slash = strrchr(dir, '/');
    if (slash) { *slash = 0; assert(!chdir(dir)); }
    FILE *file = fopen(slash ? slash + 1 : argv[1], "rb"); assert(file);
    fseek(file, 0, SEEK_END); long size = ftell(file); rewind(file);
    char *src = malloc((size_t)size + 1); assert(src);
    assert(fread(src, 1, (size_t)size, file) == (size_t)size);
    fclose(file); src[size] = 0;
    assert(!janet_dostring(env, src, "native-executor.janet", NULL)); free(src);
    JanetFunction *sum = lookup(env, "numeric-sum");
    JanetFunction *nested = lookup(env, "numeric-nested");
    uint64_t before = nx_10_entries;
    replay(sum, &nx_10_code, seed);
    assert(nx_10_entries > before && nx_10_ops > 0 && nx_10_edges > 0);
    before = nx_11_entries;
    replay(nested, &nx_11_code, seed);
    assert(nx_11_entries > before && nx_11_ops > 0 && nx_11_edges > 0);
    janet_vm.quantum = 0;
    janet_gcunroot(janet_wrap_table(env));
    janet_deinit();
    return 0;
}
