/* The strictness barrier is unconditional; only trace output is cached.
 * Reinitializing a VM must re-read the diagnostic environment. */
#include "state.h"
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>

int main(void) {
    assert(!unsetenv("JANET_TRACE_STRICT"));
    assert(!janet_init());
    assert(!janet_vm.trace_strict);
    JanetTable *env = janet_core_env(NULL);
    janet_gcroot(janet_wrap_table(env));
    assert(!janet_dostring(env,
                          "(os/setenv \"JANET_TRACE_STRICT\" \"1\")",
                          "gc-trace-test", NULL));
    assert(janet_vm.trace_strict);
    assert(!janet_dostring(env,
                          "(os/setenv \"JANET_TRACE_STRICT\" nil)",
                          "gc-trace-test", NULL));
    assert(!janet_vm.trace_strict);
    janet_gcunroot(janet_wrap_table(env));
    assert(!setenv("JANET_TRACE_STRICT", "1", 1));
    assert(!janet_vm.trace_strict); /* Foreign setenv: next init refreshes. */
    janet_deinit();

    assert(!janet_init());
    assert(janet_vm.trace_strict);
    assert(!unsetenv("JANET_TRACE_STRICT"));
    assert(janet_vm.trace_strict);
    janet_deinit();

    assert(!janet_init());
    assert(!janet_vm.trace_strict);
    janet_deinit();
    puts("GC strict diagnostic cache: initial and reinitialized states passed");
    return 0;
}
