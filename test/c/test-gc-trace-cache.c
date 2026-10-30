/* The strictness barrier is unconditional; only trace output is cached.
 * Reinitializing a VM must re-read the diagnostic environment. */
#include "state.h"
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>

int main(void) {
    assert(!unsetenv("BENDER_TRACE_STRICT"));
    assert(!bender_init());
    assert(!bender_vm.trace_strict);
    BenderTable *env = bender_core_env(NULL);
    bender_gcroot(bender_wrap_table(env));
    assert(!bender_dostring(env,
                          "(os/setenv \"BENDER_TRACE_STRICT\" \"1\")",
                          "gc-trace-test", NULL));
    assert(bender_vm.trace_strict);
    assert(!bender_dostring(env,
                          "(os/setenv \"BENDER_TRACE_STRICT\" nil)",
                          "gc-trace-test", NULL));
    assert(!bender_vm.trace_strict);
    bender_gcunroot(bender_wrap_table(env));
    assert(!setenv("BENDER_TRACE_STRICT", "1", 1));
    assert(!bender_vm.trace_strict); /* Foreign setenv: next init refreshes. */
    bender_deinit();

    assert(!bender_init());
    assert(bender_vm.trace_strict);
    assert(!unsetenv("BENDER_TRACE_STRICT"));
    assert(bender_vm.trace_strict);
    bender_deinit();

    assert(!bender_init());
    assert(!bender_vm.trace_strict);
    bender_deinit();
    puts("GC strict diagnostic cache: initial and reinitialized states passed");
    return 0;
}
