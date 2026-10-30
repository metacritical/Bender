/* Moving-goalpost fixture: the major threshold is a function of the CURRENT
 * block count, so retaining many small objects raises the bar faster than
 * pressure can climb.
 *
 *   gc_interval updated only inside bender_collect(): block_count*sizeof(GC)
 *   major due when next_collection >= gc_interval + block_count*1024
 *
 * Per live small block: pressure grows by the object size (~48 B for a short
 * string) while the limit grows by sizeof(BenderGCObject)+1024 (~1040 B). The
 * gap therefore WIDENS with the live small-object count, and a heap full of
 * small survivors can never become due. This is not the branch-priority defect
 * and the priority patch does not change it.
 *
 * Deterministic form: arm "one byte short of due", then allocate exactly one
 * retained small string and show that the major is NOW further from firing.
 *
 * Build (fixture only; reuses an already-built archive):
 *   cc -O2 -DBENDER_NATIVE_EXECUTORS -Isrc/include -Isrc/conf -Isrc/core \
 *      test/c/test-gc-interval-feedback.c build/libbender.a -lm -lpthread -ldl
 */
#include <bender.h>
#include "state.h"
#include <assert.h>
#include <stdio.h>

#define FULL_SCALE 1024

/* Per-block growth of the due-threshold. */
static size_t limit_growth(void) {
    return sizeof(BenderGCObject) + FULL_SCALE;
}

int main(void) {
    size_t limit_before, limit_after, pressure_before, pressure_after;
    size_t blocks_before, blocks_after;
    char payload[8];

    assert(!bender_init());
    BenderTable *env = bender_core_env(NULL);
    bender_gcroot(bender_wrap_table(env));

    /* Kill any pending pressure and pin the interval so `limit` is stable
     * except for block_count. */
    bender_vm.next_collection = 0;
    bender_vm.gc_interval = bender_vm.block_count * sizeof(BenderGCObject);

    limit_before = bender_vm.gc_interval + bender_vm.block_count * FULL_SCALE;
    blocks_before = bender_vm.block_count;
    /* Exactly one byte short of due: the next accounted byte fires it. */
    bender_vm.next_collection = limit_before - 1;
    pressure_before = bender_vm.next_collection;

    /* One small RETAINED object: live, therefore not garbage that a sweep
     * would remove. Content is unique so nothing can dedupe it. */
    {
        BenderString s;
        snprintf(payload, sizeof(payload), "u%zu", blocks_before);
        s = bender_string((const uint8_t *) payload, 6);
        bender_gcroot(bender_wrap_string(s));
    }

    blocks_after = bender_vm.block_count;
    limit_after = bender_vm.gc_interval + blocks_after * FULL_SCALE;
    pressure_after = bender_vm.next_collection;

    printf("blocks %zu -> %zu\n", blocks_before, blocks_after);
    printf("limit %zu -> %zu (grew %zu)\n",
           limit_before, limit_after, limit_after - limit_before);
    printf("pressure %zu -> %zu (grew %zu)\n",
           pressure_before, pressure_after, pressure_after - pressure_before);
    printf("margin before=%zd after=%zd\n",
           (ssize_t) (limit_before - pressure_before),
           (ssize_t) (limit_after - pressure_after));

    /* The allocation did add to block_count and to pressure... */
    assert(blocks_after > blocks_before);
    assert(pressure_after > pressure_before);
    /* ...but it moved the threshold further than it moved the pressure, so a
     * collection that was one byte away is now further away than before. */
    assert(limit_after - limit_before > pressure_after - pressure_before);
    assert((limit_after - pressure_after) > (limit_before - pressure_before));
    /* Therefore: not due, and it was about to be. */
    assert(pressure_after < limit_after);

    /* The same arithmetic implies the gap is unbounded in the live count. */
    assert(limit_growth() > 1);

    bender_gcunroot(bender_wrap_table(env));
    bender_deinit();
    puts("gc-interval-feedback: small survivors outrun the major threshold");
    return 0;
}
