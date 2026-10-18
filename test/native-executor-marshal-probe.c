/* Check whether marshal of a fiber parked at a quantum interrupt inside a
 * call-heavy loop is lossy in the plain interpreter (no native code). */
#include "state.h"
#include "fiber.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include NATIVE_EXECUTOR_C

static JanetTable *env;
static Janet run(const char *src) {
    Janet out = janet_wrap_nil();
    int sig = janet_dostring(env, src, "probe", &out);
    if (sig) { fprintf(stderr, "probe failed: %s\n", src); assert(!"fail"); }
    return out;
}
int main(int argc, char **argv) {
    assert(argc == 2);
    janet_init();
    janet_vm.quantum = 0;
    env = janet_core_env(NULL);
    janet_gcroot(janet_wrap_table(env));
    char *dir = strdup(argv[1]);
    char *slash = strrchr(dir, '/');
    if (slash) { *slash = 0; assert(!chdir(dir)); argv[1] = slash + 1; }
    FILE *file = fopen(argv[1], "rb"); assert(file);
    fseek(file, 0, SEEK_END); long size = ftell(file); rewind(file);
    char *src = malloc((size_t)size + 1); assert(src);
    assert(fread(src, 1, (size_t)size, file) == (size_t)size); fclose(file); src[size] = 0;
    assert(!janet_dostring(env, src, argv[1], NULL)); free(src);
    run("(module/add-syspath \".\")(import ./native-executor-mixed :as mx)");
    JanetFunction *boundary = janet_unwrap_function(run("mx/boundary"));
    boundary->def->native_code = NULL; /* pure interpreter */
    Janet arg = janet_wrap_integer(JANET_QUANTUM_BUDGET == 1 ? 6 : 6 * JANET_QUANTUM_BUDGET);
    JanetFiber *f = janet_fiber(boundary, 64, 1, &arg);
    janet_gcroot(janet_wrap_fiber(f));
    janet_vm.quantum = JANET_QUANTUM_BUDGET;
    Janet x;
    JanetSignal s = janet_continue(f, janet_wrap_nil(), &x);
    janet_vm.quantum = 0;
    printf("first resume signal=%d pc=%ld\n", s,
           (long)(janet_fiber_frame(f)->pc - boundary->def->bytecode));
    if (s != JANET_SIGNAL_INTERRUPT) { puts("no interrupt; n/a"); return 0; }
    JanetBuffer *b = janet_buffer(0);
    janet_gcroot(janet_wrap_buffer(b));
    janet_marshal(b, janet_wrap_fiber(f), NULL, JANET_MARSHAL_UNSAFE);
    Janet restored = janet_unmarshal(b->data, b->count, JANET_MARSHAL_UNSAFE, NULL, NULL);
    janet_gcroot(restored);
    JanetSignal s2 = janet_continue(janet_unwrap_fiber(restored), janet_wrap_nil(), &x);
    printf("restored signal=%d value=", s2);
    const uint8_t *d = janet_formatc("%v", x);
    printf("%s\n", (const char *)d);
    return s2 == JANET_SIGNAL_OK ? 0 : 1;
}
