/* The major budget must come from the LAST major's survivors, not from the
 * current block count. Otherwise retaining many small objects raises the bar
 * faster than pressure can climb (measured: 12.4M blocks -> ~12 GB limit).
 *
 * Behavioural only: this fixture never names the new state field, so it also
 * compiles against trees whose state.h predates it.
 *
 * Discriminating window:
 *   survivors = block_count right after a full collect
 *   inflated  = survivors + real unrooted garbage allocated afterwards
 *   armed     = interval + survivors*1024 + 1   (fixed budget: DUE)
 *             < interval + inflated*1024        (current-count budget: NOT due)
 * The active fiber's nursery is pinned to 1 and the minor threshold to 1, so a
 * tree that checks the nursery gate first can never reach the major branch.
 *
 * Build (fixture only; reuses an already-built archive):
 *   cc -O2 -DJANET_NATIVE_EXECUTORS -Isrc/include -Isrc/conf -Isrc/core \
 *      test/c/test-gc-budget-current-blocks.c build/libjanet.a \
 *      -lm -lpthread -ldl
 */
#include <janet.h>
#include "state.h"
#include <assert.h>
#include <stdio.h>

#define FULL_SCALE 1024
#define INFLATE 20000

static size_t arm_value;   /* pressure to install, chosen in main */

/* Arm on the ACTIVE fiber; the VM's post-C-call check then runs on it. */
static Janet arm(int32_t argc, Janet *argv) {
    JanetFiber *f = janet_vm.fiber;
    janet_fixarity(argc, 1);
    assert(f != NULL);
    /* Nursery gate taken but never satisfied: a tree with the old check ORDER
     * cannot fall through to the major branch. */
    janet_vm.gc_minor_threshold = 1;
    f->gc_nursery_bytes = 1;
    janet_vm.next_collection = arm_value;
    return argv[0];
}

int main(void) {
    Janet out;
    size_t survivors, interval, current, survivor_bar, current_bar;

    assert(!janet_init());
    JanetTable *env = janet_core_env(NULL);
    janet_gcroot(janet_wrap_table(env));
    janet_def(env, "arm", janet_wrap_cfunction(arm), "arm the pressure window");

    /* A full collect establishes the last-major survivor baseline and leaves a
     * stable interval. */
    janet_collect();
    survivors = janet_vm.block_count;
    interval = janet_vm.gc_interval;

    /* Real unrooted small garbage: block_count grows for real, and none of it
     * survives because nothing roots it. */
    for (int i = 0; i < INFLATE; i++) {
        char buf[24];
        snprintf(buf, sizeof(buf), "garbage-%d", i);
        (void) janet_string((const uint8_t *) buf, 8);
    }
    current = janet_vm.block_count;
    assert(current > survivors + 1000);

    survivor_bar = interval + survivors * FULL_SCALE;
    current_bar = interval + current * FULL_SCALE;
    arm_value = survivor_bar + 1;
    /* The window is real and points the right way. */
    assert(arm_value < current_bar);

    printf("survivors=%zu current=%zu interval=%zu\n", survivors, current, interval);
    printf("survivor_bar=%zu current_bar=%zu armed=%zu\n",
           survivor_bar, current_bar, arm_value);

    assert(!janet_dostring(env, "(arm 1)", "budget-arm", &out));

    /* A full collect must have run and reset the counter. A budget that still
     * reads the CURRENT count leaves it untouched and fails here. */
    assert(janet_vm.next_collection == 0);
    /* The garbage was reclaimed, so the count fell back near survivors. */
    assert(janet_vm.block_count < current);

    janet_gcunroot(janet_wrap_table(env));
    janet_deinit();
    puts("gc-budget-current-blocks: budget tracks survivors, not current blocks");
    return 0;
}
