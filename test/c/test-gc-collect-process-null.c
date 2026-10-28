/* Regression for the NULL fallback in the internal process collector.
 * Build after make:
 * cc -O2 -o build/test-gc-collect-process-null \
 *   test/c/test-gc-collect-process-null.c -Isrc/include -Isrc/conf \
 *   build/libjanet.a -lm -lpthread -ldl
 */
#include "janet.h"
#include <assert.h>
#include <stdio.h>

struct JanetProcess;
void janet_collect_process(struct JanetProcess *proc);

int main(void) {
    assert(!janet_init());
    JanetTable *env = janet_core_env(NULL);
    janet_gcroot(janet_wrap_table(env));
    for (int i = 0; i < 100; i++) {
        janet_collect_process(NULL);
    }
    assert(!janet_dostring(env, "(assert (= 42 (+ 40 2)))",
                          "collect-process-null", NULL));
    janet_gcunroot(janet_wrap_table(env));
    janet_deinit();
    puts("janet_collect_process(NULL): passed");
    return 0;
}
