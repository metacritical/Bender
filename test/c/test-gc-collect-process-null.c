/* Regression for the NULL fallback in the internal process collector.
 * Build after make:
 * cc -O2 -o build/test-gc-collect-process-null \
 *   test/c/test-gc-collect-process-null.c -Isrc/include -Isrc/conf \
 *   build/libbender.a -lm -lpthread -ldl
 */
#include "bender.h"
#include <assert.h>
#include <stdio.h>

struct BenderProcess;
void bender_collect_process(struct BenderProcess *proc);

int main(void) {
    assert(!bender_init());
    BenderTable *env = bender_core_env(NULL);
    bender_gcroot(bender_wrap_table(env));
    for (int i = 0; i < 100; i++) {
        bender_collect_process(NULL);
    }
    assert(!bender_dostring(env, "(assert (= 42 (+ 40 2)))",
                          "collect-process-null", NULL));
    bender_gcunroot(bender_wrap_table(env));
    bender_deinit();
    puts("bender_collect_process(NULL): passed");
    return 0;
}
