/* The major budget must come from the LAST major's survivors, not from the
 * current block count. Otherwise retaining many small objects raises the bar
 * faster than pressure can climb (measured on the game: 12.4M blocks against a
 * ~12 GB limit, 263M polls, zero majors due).
 *
 * Behavioural only: this fixture never names the new state field, so it
 * compiles against trees whose state.h predates it.
 *
 * ORDERING IS THE POINT. Everything that can allocate -- compiling the probe,
 * registering it, running the survivor baseline -- happens BEFORE the garbage
 * is allocated and BEFORE the window is armed. A compile after the inflation
 * would collect, drop the count back, and silently invalidate the window.
 *
 * Discriminating window:
 *   survivors = block_count right after a full collect
 *   inflated  = survivors + real unrooted garbage allocated afterwards
 *   armed     = interval + survivors*1024 + 1   (fixed budget: DUE)
 *             < interval + inflated*1024        (current-count budget: NOT due)
 * The active fiber's nursery is pinned to 1 and the minor threshold to 1, so a
 * tree that checks the nursery gate first cannot reach the major branch.
 *
 * Build (fixture only; reuses an already-built archive):
 *   cc -O2 -DBENDER_NATIVE_EXECUTORS -Isrc/include -Isrc/conf -Isrc/core \
 *      test/c/test-gc-budget-current-blocks.c build/libbender.a \
 *      -lm -lpthread -ldl
 */
#include <bender.h>
#include "state.h"
#include <assert.h>
#include <stdio.h>

#define FULL_SCALE 1024
#define INFLATE 20000

static size_t arm_value;     /* pressure to install, chosen in main */
static int arm_calls;        /* proof the probe really ran */
static BenderFiber *armed_fiber;

/* Arm on the ACTIVE fiber; the VM's post-C-call check then runs on it. */
static Bender arm(int32_t argc, Bender *argv) {
    BenderFiber *f = bender_vm.fiber;
    bender_fixarity(argc, 1);
    assert(f != NULL);
    arm_calls++;
    armed_fiber = f;
    /* Nursery gate taken but never satisfied: a tree with the old check ORDER
     * cannot fall through to the major branch. */
    bender_vm.gc_minor_threshold = 1;
    f->gc_nursery_bytes = 1;
    bender_vm.next_collection = arm_value;
    return argv[0];
}

int main(void) {
    Bender probe_out, result;
    BenderFunction *probe_fn;
    BenderFiber *probe;
    size_t survivors, interval, current, survivor_bar, current_bar;

    assert(!bender_init());
    BenderTable *env = bender_core_env(NULL);
    bender_gcroot(bender_wrap_table(env));

    /* --- everything allocating happens BEFORE the count is inflated ------ */
    bender_def(env, "arm", bender_wrap_cfunction(arm), "arm the pressure window");
    /* The arm must NOT be in tail position: a tail call returns through
     * vm_return_no_restore and skips vm_checkgc, so the check would never run
     * after the window is armed. The trailing form forces a real JOP_CALL
     * return through vm_checkgc_pcnext. Measured: tail -> 11899905 (no
     * collect), this form -> 0. */
    assert(!bender_dostring(env, "(fn [] (arm 1) (+ 1 2))", "budget-probe",
                           &probe_out));
    assert(bender_checktype(probe_out, BENDER_FUNCTION));
    probe_fn = bender_unwrap_function(probe_out);
    probe = bender_fiber(probe_fn, 64, 0, NULL);
    assert(probe != NULL);
    /* The fiber and the function are separate roots: the fiber is not reachable
     * from the function, so the baseline collect below would free it. */
    bender_gcroot(probe_out);
    bender_gcroot(bender_wrap_fiber(probe));

    /* Survivor baseline and a stable interval from a real full major. */
    bender_collect();
    survivors = bender_vm.block_count;
    interval = bender_vm.gc_interval;

    /* --- inflate with real, unrooted, NON-INTERNED strings --------------- */
    /* A long constant prefix makes each string too long to intern, so every
     * allocation is a genuine new block rather than a deduplicated one. */
    for (int i = 0; i < INFLATE; i++) {
        char buf[48];
        int n = snprintf(buf, sizeof(buf),
                         "gc-budget-garbage-%08d-padding-padding", i);
        (void) bender_string((const uint8_t *) buf, (int32_t) n);
    }
    current = bender_vm.block_count;
    assert(current > survivors + 1000);

    survivor_bar = interval + survivors * FULL_SCALE;
    current_bar = interval + current * FULL_SCALE;
    arm_value = survivor_bar + 1;
    /* The window is real and points the right way. */
    assert(arm_value < current_bar);

    printf("survivors=%zu current=%zu interval=%zu\n", survivors, current, interval);
    printf("survivor_bar=%zu current_bar=%zu armed=%zu\n",
           survivor_bar, current_bar, arm_value);

    /* --- run the PRE-BUILT fiber: no compilation left to disturb the window */
    assert(BENDER_SIGNAL_OK == bender_continue(probe, bender_wrap_nil(), &result));
    assert(bender_equals(result, bender_wrap_integer(3)));

    /* The probe must have actually run, on the fiber the VM checked. */
    assert(arm_calls == 1);
    assert(armed_fiber == probe);

    /* A full collect must have run and reset the counter. A budget that still
     * reads the CURRENT count leaves it untouched and fails here. */
    assert(bender_vm.next_collection == 0);
    /* The garbage was reclaimed, so the count fell back near survivors. */
    assert(bender_vm.block_count < current);

    bender_gcunroot(bender_wrap_fiber(probe));
    bender_gcunroot(probe_out);
    bender_gcunroot(bender_wrap_table(env));
    bender_deinit();
    puts("gc-budget-current-blocks: budget tracks survivors, not current blocks");
    return 0;
}
