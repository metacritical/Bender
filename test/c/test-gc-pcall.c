/*
 * Test that GC does not collect fibers during bender_pcall.
 *
 * Bug: bender_collect() marks bender_vm.root_fiber but not bender_vm.fiber.
 * When bender_pcall is called from a C function, the inner fiber becomes
 * bender_vm.fiber while root_fiber still points to the outer fiber. If GC
 * triggers inside the inner fiber's execution, the inner fiber is not in
 * any GC root set and can be collected — including its stack memory —
 * while it is actively running.
 *
 * Two tests:
 *   1. Single nesting: F1 -> C func -> bender_pcall -> F2
 *      F2 is not marked (it's bender_vm.fiber but not root_fiber)
 *   2. Deep nesting: F1 -> C func -> bender_pcall -> F2 -> C func -> bender_pcall -> F3
 *      F2 is not marked (saved only in a C stack local tstate.vm_fiber)
 *
 * Build (after building bender):
 *   cc -o build/test-gc-pcall test/test-gc-pcall.c \
 *      -Isrc/include -Isrc/conf build/libbender.a -lm -lpthread -ldl
 *
 * Run:
 *   ./build/test-gc-pcall
 */

#include "bender.h"
#include <stdio.h>

/* C function that calls a Bender callback via bender_pcall. */
static Bender cfun_call_via_pcall(int32_t argc, Bender *argv) {
    bender_fixarity(argc, 1);
    BenderFunction *fn = bender_getfunction(argv, 0);

    Bender result;
    BenderFiber *fiber = NULL;
    BenderSignal sig = bender_pcall(fn, 0, NULL, &result, &fiber);

    if (sig != BENDER_SIGNAL_OK) {
        bender_panicv(result);
    }

    return result;
}

static int run_test(BenderTable *env, const char *name, const char *source) {
    printf("  %s... ", name);
    fflush(stdout);
    Bender result;
    int status = bender_dostring(env, source, name, &result);
    if (status != 0) {
        printf("FAIL (crashed or errored)\n");
        return 1;
    }
    printf("PASS\n");
    return 0;
}

/* Test 1: Single nesting.
 * F1 -> cfun_call_via_pcall -> bender_pcall -> F2
 * F2 is bender_vm.fiber but not root_fiber, so GC can collect it.
 *
 * All allocations are done in Bender code so GC checks trigger in the
 * VM loop (bender_gcalloc does NOT call bender_collect — only the VM's
 * vm_checkgc_next does). */
static const char test_single[] =
    "(gcsetinterval 1024)\n"
    "(def cb\n"
    "  (do\n"
    "    (def captured @{:key \"value\" :nested @[1 2 3 4 5]})\n"
    "    (fn []\n"
    "      (var result nil)\n"
    "      (for i 0 500\n"
    "        (def t @{:i i :s (string \"iter-\" i) :arr @[i (+ i 1) (+ i 2)]})\n"
    "        (set result (get captured :key)))\n"
    "      result)))\n"
    "(for round 0 200\n"
    "  (def result (call-via-pcall cb))\n"
    "  (assert (= result \"value\")\n"
    "    (string \"round \" round \": expected 'value', got \" (describe result))))\n";

/* Test 2: Deep nesting.
 * F1 -> cfun_call_via_pcall -> bender_pcall -> F2 -> cfun_call_via_pcall -> bender_pcall -> F3
 * F2 is saved only in C stack local tstate.vm_fiber, invisible to GC.
 * F2's stack data can be freed if F2 is collected during F3's execution.
 *
 * The inner callback allocates in Bender code (not C) to ensure the
 * VM loop triggers GC checks during F3's execution. */
static const char test_deep[] =
    "(gcsetinterval 1024)\n"
    "(def inner-cb\n"
    "  (do\n"
    "    (def captured @{:key \"deep\" :nested @[10 20 30]})\n"
    "    (fn []\n"
    "      (var result nil)\n"
    "      (for i 0 500\n"
    "        (def t @{:i i :s (string \"iter-\" i) :arr @[i (+ i 1) (+ i 2)]})\n"
    "        (set result (get captured :key)))\n"
    "      result)))\n"
    "\n"
    "(def outer-cb\n"
    "  (do\n"
    "    (def state @{:count 0 :data @[\"a\" \"b\" \"c\" \"d\" \"e\"]})\n"
    "    (fn []\n"
    "      # This runs on F2. Calling call-via-pcall here creates F3.\n"
    "      # F2 becomes unreachable: it's not root_fiber (that's F1)\n"
    "      # and it's no longer bender_vm.fiber (that's now F3).\n"
    "      (def inner-result (call-via-pcall inner-cb))\n"
    "      # If F2 was collected during F3's execution, accessing\n"
    "      # state here reads freed memory.\n"
    "      (put state :count (+ (state :count) 1))\n"
    "      (string inner-result \"-\" (state :count)))))\n"
    "\n"
    "(for round 0 200\n"
    "  (def result (call-via-pcall outer-cb))\n"
    "  (def expected (string \"deep-\" (+ round 1)))\n"
    "  (assert (= result expected)\n"
    "    (string \"round \" round \": expected '\" expected \"', got '\" (describe result) \"'\")))\n";

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;
    int failures = 0;

    bender_init();

    BenderTable *env = bender_core_env(NULL);

    bender_def(env, "call-via-pcall",
              bender_wrap_cfunction(cfun_call_via_pcall),
              "Call a function via bender_pcall from C.");

    printf("Testing bender_pcall GC safety:\n");
    failures += run_test(env, "single-nesting", test_single);
    failures += run_test(env, "deep-nesting", test_deep);

    bender_deinit();

    if (failures > 0) {
        printf("\n%d test(s) FAILED\n", failures);
        return 1;
    }
    printf("\nAll tests passed.\n");
    return 0;
}
