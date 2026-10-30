/* Major-pressure starvation fixture: a fiber nursery at/above the BASE
 * threshold but below the SCALED one takes the nursery branch of
 * `maybe_collect`, declines the inner test, and never reaches the major
 * branch (an `else if` of the gate just taken). Major pressure then grows
 * without bound. See src/core/vm.c maybe_collect.
 *
 * Trigger: a C function arms the straddle on the ACTIVE fiber and returns;
 * the VM's own post-C-call check (JOP_CALL vm_checkgc_pcnext) fires on it.
 * Observable: bender_collect() ends with next_collection = 0; no minor path
 * resets it. Major counter asserted when compiled with executor telemetry.
 *
 * Build: cc -O2 -DBENDER_NATIVE_EXECUTORS -Isrc/include -Isrc/conf -Isrc/core \
 *        test/c/test-gc-major-pressure.c build/libbender.a -lm -lpthread -ldl
 */
#include <bender.h>
#include "state.h"
#include <assert.h>
#include <stdio.h>

/* gc.c defines this default internally; the fixture must mirror it or the
 * counter arm silently compiles to nothing. */
#ifndef BENDER_NATIVE_TELEMETRY
#define BENDER_NATIVE_TELEMETRY 1
#endif

#define MINOR_SCALE 256
#define FULL_SCALE 1024

static size_t armed_limit, armed_scaled;
static int arm_calls;        /* proof the probe really ran */
static BenderFiber *armed_fiber;

static Bender arm(int32_t argc, Bender *argv) {
    BenderFiber *f = bender_vm.fiber;
    bender_fixarity(argc, 1);
    assert(f != NULL && bender_vm.block_count > 0);
    arm_calls++;
    armed_fiber = f;         /* the fiber the VM will check next */
    armed_scaled = bender_vm.gc_minor_threshold +
                   bender_vm.block_count * MINOR_SCALE;
    f->gc_nursery_bytes = bender_vm.gc_minor_threshold;   /* outer gate: yes */
    assert((size_t) f->gc_nursery_bytes < armed_scaled); /* inner gate: no  */
    bender_vm.gc_interval = bender_vm.block_count * sizeof(BenderGCObject);
    armed_limit = bender_vm.gc_interval + bender_vm.block_count * FULL_SCALE;
    bender_vm.next_collection = armed_limit;
    return argv[0];
}

int main(void) {
    Bender out;
    uint64_t before = 0;
    int counted = 0;

    assert(!bender_init());
    BenderTable *env = bender_core_env(NULL);
    bender_gcroot(bender_wrap_table(env));
    bender_def(env, "arm-pressure", bender_wrap_cfunction(arm), "arm");
    bender_vm.gc_minor_threshold = 256 * 1024;

#if defined(BENDER_NATIVE_EXECUTORS) && BENDER_NATIVE_TELEMETRY
    counted = 1;
    before = bender_vm.work_gc_major;
#endif

    assert(!bender_dostring(env, "(arm-pressure 1)", "gc-major", &out));

    /* The probe must have actually executed, on the fiber the VM checks. If
     * this is 0 the fixture proved nothing about maybe_collect. */
    assert(arm_calls == 1);
    assert(armed_fiber != NULL);

    printf("next_collection=%zu limit=%zu nursery=%zu scaled=%zu blocks=%zu\n",
           bender_vm.next_collection, armed_limit,
           (size_t) bender_vm.gc_minor_threshold, armed_scaled,
           (size_t) bender_vm.block_count);

    /* PRIMARY witness, build-independent: a major ran and zeroed the counter.
     * Under the starved ordering this stays at the limit and fails HERE. */
    assert(bender_vm.next_collection == 0);
    /* SECONDARY, mechanism only: exactly one major ran, and it was this one. */
    if (counted) {
        uint64_t advanced = bender_vm.work_gc_major - before;
        printf("majors_advanced=%llu\n", (unsigned long long) advanced);
        assert(advanced == 1);
    }
    assert(!bender_dostring(env, "(assert (= 500 (+ 200 300)))", "post", NULL));

    bender_gcunroot(bender_wrap_table(env));
    bender_deinit();
    puts("gc-major-pressure: passed");
    return 0;
}
