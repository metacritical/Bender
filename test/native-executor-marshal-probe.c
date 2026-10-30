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

static BenderTable *env;
static Bender run(const char *src) {
    Bender out = bender_wrap_nil();
    int sig = bender_dostring(env, src, "probe", &out);
    if (sig) { fprintf(stderr, "probe failed: %s\n", src); assert(!"fail"); }
    return out;
}
int main(int argc, char **argv) {
    assert(argc == 2);
    bender_init();
    bender_vm.quantum = 0;
    env = bender_core_env(NULL);
    bender_gcroot(bender_wrap_table(env));
    char *dir = strdup(argv[1]);
    char *slash = strrchr(dir, '/');
    if (slash) { *slash = 0; assert(!chdir(dir)); argv[1] = slash + 1; }
    FILE *file = fopen(argv[1], "rb"); assert(file);
    fseek(file, 0, SEEK_END); long size = ftell(file); rewind(file);
    char *src = malloc((size_t)size + 1); assert(src);
    assert(fread(src, 1, (size_t)size, file) == (size_t)size); fclose(file); src[size] = 0;
    assert(!bender_dostring(env, src, argv[1], NULL)); free(src);
    run("(module/add-syspath \".\")(import ./native-executor-mixed :as mx)");
    BenderFunction *boundary = bender_unwrap_function(run("mx/boundary"));
    boundary->def->native_code = NULL; /* pure interpreter */
    Bender arg = bender_wrap_integer(BENDER_QUANTUM_BUDGET == 1 ? 6 : 6 * BENDER_QUANTUM_BUDGET);
    BenderFiber *f = bender_fiber(boundary, 64, 1, &arg);
    bender_gcroot(bender_wrap_fiber(f));
    bender_vm.quantum = BENDER_QUANTUM_BUDGET;
    Bender x;
    BenderSignal s = bender_continue(f, bender_wrap_nil(), &x);
    bender_vm.quantum = 0;
    printf("first resume signal=%d pc=%ld\n", s,
           (long)(bender_fiber_frame(f)->pc - boundary->def->bytecode));
    if (s != BENDER_SIGNAL_INTERRUPT) { puts("no interrupt; n/a"); return 0; }
    BenderBuffer *b = bender_buffer(0);
    bender_gcroot(bender_wrap_buffer(b));
    bender_marshal(b, bender_wrap_fiber(f), NULL, BENDER_MARSHAL_UNSAFE);
    Bender restored = bender_unmarshal(b->data, b->count, BENDER_MARSHAL_UNSAFE, NULL, NULL);
    bender_gcroot(restored);
    BenderSignal s2 = bender_continue(bender_unwrap_fiber(restored), bender_wrap_nil(), &x);
    printf("restored signal=%d value=", s2);
    const uint8_t *d = bender_formatc("%v", x);
    printf("%s\n", (const char *)d);
    return s2 == BENDER_SIGNAL_OK ? 0 : 1;
}
