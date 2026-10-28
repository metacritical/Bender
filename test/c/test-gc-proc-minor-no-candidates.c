/* Standalone tests for the proc-scoped
 * minor candidate (empty / non-eligible / eligible / global fallback / epoch).
 *
 *   cc -O2 -g -DJANET_NATIVE_EXECUTORS -Isrc/include -Isrc/conf -Isrc/core \
 *      test/c/test-gc-proc-minor-no-candidates.c build/libjanet.a -lm -lpthread -ldl
 *
 * Determinism notes (why the assertions are shaped this way):
 *  - Eligibility needs a listed block on a slab page whose owner is the
 *    fiber AND whose age < 2 (gc.c:1420,2766). The process-wide freelist
 *    starts full of host-born (owner 0) slots, so the FIRST few allocations
 *    of any fiber land on recycled slots that are not owned by it. Only
 *    after exhausting them does a fresh page get claimed (owner = f).
 *    A2 therefore asserts only what holds either way, and A3 forces a
 *    fresh page by volume.
 *  - `old` cannot be fabricated without an aging hook, so the malloc-large
 *    branch of janet_slab_obj_old (gc.c:1421-1422: no slab page => old) is
 *    used instead: a string larger than the biggest slab class (512, see
 *    gc.c:1175) is malloc'd, filed on heap_lists[1], and always reads old.
 */
#include "state.h"
#include "gc.h"
#include "proc.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint64_t c_fiber(void) {
    return janet_native_stat(JANET_NATIVE_STAT_GC_FIBER_MINOR);
}
static uint64_t c_minor(void) {
    return janet_native_stat(JANET_NATIVE_STAT_GC_MINOR);
}

static int pending_empty(void) {
    return NULL == janet_vm.sweep_pending[0] &&
           NULL == janet_vm.sweep_pending[1] &&
           NULL == janet_vm.sweep_pending[2];
}

static size_t listed(JanetFiber *f) {
    size_t n = 0;
    for (int k = 0; k < 3; k++)
        for (JanetGCObject *cur = f->heap_lists[k]; cur; cur = cur->data.next) n++;
    return n;
}

typedef struct {
    JanetFiber *fiber;
    JanetProcess *proc;
} Fixture;

/* ---- Cross-owner callback probe ----------------------------------------
 * A custom abstract whose gcmark counts invocations. It starts reachable
 * ONLY through proc->exit_reason (proc is rooted, so the chain
 * proc -> exit_reason holds it), and it is deliberately allocated with no
 * attributing fiber, so it lands on the GLOBAL heap lists and never on the
 * fixture fiber's three lists. That keeps the fast path eligible while
 * proving what it must not do: run the traversal. */
static int probe_marks = 0;
static int probe_gc_calls = 0;
static int probe_gc(void *p, size_t l) {
    (void) p;
    (void) l;
    probe_gc_calls++;
    return 0;
}
static int probe_mark(void *p, size_t l) {
    (void) p;
    (void) l;
    probe_marks++;
    return 0;
}
static const JanetAbstractType probe_type = {
    "test/probe", probe_gc, probe_mark, JANET_ATEND_GCMARK
};

/* Allocate `type` on the GLOBAL heap lists (no attributing fiber). */
static Janet alloc_global(const JanetAbstractType *type, size_t size) {
    JanetFiber *saved_fiber = janet_vm.fiber;
    JanetFiber *saved_ctx = janet_vm.gc_alloc_ctx;
    janet_vm.fiber = NULL;
    janet_vm.gc_alloc_ctx = NULL;
    Janet x = janet_wrap_abstract(janet_abstract(type, size));
    janet_vm.gc_alloc_ctx = saved_ctx;
    janet_vm.fiber = saved_fiber;
    return x;
}

/* A REAL proc abstract: heap-allocated, zeroed, minimal live fields. */
static Fixture fixture(JanetFunction *fn) {
    Fixture fx;
    fx.fiber = janet_fiber(fn, 16, 0, NULL);
    assert(NULL != fx.fiber);
    janet_gcroot(janet_wrap_fiber(fx.fiber));
    fx.proc = janet_abstract(&janet_proc_type, sizeof(JanetProcess));
    memset(fx.proc, 0, sizeof(*fx.proc));
    fx.proc->exit_reason = janet_wrap_nil();
    fx.proc->fiber = fx.fiber;
    fx.proc->serial = 1;
    janet_gcroot(janet_wrap_abstract(fx.proc));
    fx.fiber->proc = fx.proc;
    assert(fx.fiber->gc_owner != 0);
    return fx;
}

/* Detach the fake proc before any global collection (real deinit). */
static void release(Fixture *fx) {
    fx->fiber->proc = NULL;
    janet_gcunroot(janet_wrap_abstract(fx->proc));
    janet_gcunroot(janet_wrap_fiber(fx->fiber));
}

/* Allocate `n` arrays attributed to `owner`. They are rooted, so the test
 * never silently leaks unreachable junk into a later collection. */
static void alloc_owned(JanetFiber *owner, int n) {
    JanetFiber *saved = janet_vm.gc_alloc_ctx;
    janet_vm.gc_alloc_ctx = owner;
    for (int i = 0; i < n; i++) janet_gcroot(janet_wrap_array(janet_array(3)));
    janet_vm.gc_alloc_ctx = saved;
}

int main(void) {
    setenv("JANET_GC_VERIFY", "1", 1);
    setenv("JANET_HEAP_VERIFY", "1", 1);
    janet_init();

    /* Root env and function for the whole run: fixtures only borrow them,
     * so a released fixture can never dangle them into a later collect. */
    JanetTable *env = janet_core_env(NULL);
    janet_gcroot(janet_wrap_table(env));
    Janet value;
    assert(0 == janet_dostring(env, "(fn [] nil)", "eligible-minor", &value));
    assert(janet_checktype(value, JANET_FUNCTION));
    janet_gcroot(value);
    JanetFunction *fn = janet_unwrap_function(value);

    /* ---- A1: nothing listed -> fast path, no collection work. ---- */
    {
        Fixture fx = fixture(fn);
        assert(0 == listed(fx.fiber));
        uint64_t base = c_fiber();
        fx.fiber->gc_nursery_bytes = 100;
        janet_collect_fiber_minor(fx.fiber);
        assert(c_fiber() == base);
        assert(fx.fiber->gc_nursery_bytes == 0);
        assert(pending_empty());
        release(&fx);
    }

    /* ---- A2: one small allocation is NOT guaranteed to be a candidate
     *          (it can sit on a recycled owner-0 slot). Assert only what
     *          holds either way. ---- */
    {
        Fixture fx = fixture(fn);
        alloc_owned(fx.fiber, 1);
        assert(listed(fx.fiber) > 0);
        uint64_t base = c_fiber();
        fx.fiber->gc_nursery_bytes = 100;
        janet_collect_fiber_minor(fx.fiber);
        uint64_t delta = c_fiber() - base;
        assert(delta == 0 || delta == 1); /* both legitimate */
        assert(fx.fiber->gc_nursery_bytes == 0);
        assert(pending_empty());
        release(&fx);
    }

    /* ---- A3: enough volume to exhaust recycled slots and claim a fresh
     *          OWNED page -> a real candidate exists, full path runs. ---- */
    {
        Fixture fx = fixture(fn);
        alloc_owned(fx.fiber, 4096);
        assert(listed(fx.fiber) > 0);
        uint64_t base = c_fiber();
        fx.fiber->gc_nursery_bytes = 100;
        janet_collect_fiber_minor(fx.fiber);
        assert(c_fiber() == base + 1);
        assert(fx.fiber->gc_nursery_bytes == 0);
        assert(pending_empty());
        release(&fx);
    }

    /* ---- C2: a string larger than the biggest slab class (512) is
     *          malloc'd, so janet_slab_obj_old() reports old and it is
     *          never a candidate -- deterministic, no aging hook. ---- */
    {
        Fixture fx = fixture(fn);
        char big[4096];
        memset(big, 'x', sizeof(big));
        JanetFiber *saved = janet_vm.gc_alloc_ctx;
        janet_vm.gc_alloc_ctx = fx.fiber;
        Janet s = janet_wrap_string(janet_string((const uint8_t *) big, (int32_t) sizeof(big)));
        janet_vm.gc_alloc_ctx = saved;
        janet_gcroot(s);
        assert(listed(fx.fiber) > 0);
        uint64_t base = c_fiber();
        fx.fiber->gc_nursery_bytes = 33;
        janet_collect_fiber_minor(fx.fiber);
        assert(c_fiber() == base); /* old => fast path */
        assert(fx.fiber->gc_nursery_bytes == 0);
        assert(pending_empty());
        release(&fx);
    }

    /* ---- A4: all listed blocks DISABLED -> never candidates. ---- */
    {
        Fixture fx = fixture(fn);
        alloc_owned(fx.fiber, 4);
        assert(listed(fx.fiber) > 0);
        for (int k = 0; k < 3; k++)
            for (JanetGCObject *cur = fx.fiber->heap_lists[k]; cur; cur = cur->data.next)
                cur->flags |= JANET_MEM_DISABLED;
        uint64_t base = c_fiber();
        fx.fiber->gc_nursery_bytes = 55;
        janet_collect_fiber_minor(fx.fiber);
        assert(c_fiber() == base);
        assert(fx.fiber->gc_nursery_bytes == 0);
        assert(pending_empty());
        for (int k = 0; k < 3; k++)
            for (JanetGCObject *cur = fx.fiber->heap_lists[k]; cur; cur = cur->data.next)
                cur->flags &= ~(JANET_MEM_DISABLED | JANET_MEM_EPOCH_MASK);
        release(&fx);
    }

    /* ---- B: suspension wins over both paths; accounting preserved. ---- */
    {
        Fixture fx = fixture(fn);
        int handle = janet_gclock();
        uint64_t base = c_fiber();
        fx.fiber->gc_nursery_bytes = 42;
        janet_collect_fiber_minor(fx.fiber);
        assert(fx.fiber->gc_nursery_bytes == 42);
        assert(c_fiber() == base);
        janet_gcunlock(handle);
        release(&fx);
    }

    /* ---- C: owner-0 fiber takes the GLOBAL minor fallback, which ticks
     *          work_gc_minor, not work_gc_fiber_minor. ---- */
    {
        Fixture fx = fixture(fn);
        uint32_t saved_owner = fx.fiber->gc_owner;
        fx.fiber->gc_owner = 0;
        uint64_t fib0 = c_fiber(), min0 = c_minor();
        fx.fiber->gc_nursery_bytes = 12;
        janet_collect_fiber_minor(fx.fiber);
        assert(fx.fiber->gc_nursery_bytes == 0);
        assert(c_fiber() == fib0);      /* scoped counter untouched */
        assert(c_minor() == min0 + 1);  /* global minor actually ran */
        fx.fiber->gc_owner = saved_owner;
        release(&fx);
    }

    /* ---- E: cross-owner reachability through proc->exit_reason.
     *
     * The probe is filed on the GLOBAL lists (no attributing fiber) and is
     * attached to proc->exit_reason. It is deliberately NOT rooted on the
     * test side: after this point the ONLY reference to it is
     * proc->exit_reason, so retention through the next real collection
     * proves the actual GC edge rather than a test-side root masking a
     * missed edge. Because proc is rooted, its exit_reason edge is the sole
     * claim under test.
     *
     * The 40 scoped minors must not traverse it (no candidates on the
     * fixture's own lists), and the next real collection must reach it
     * through proc and run its gcmark without finalizing it.
     * Counters are used for all post-collection verdicts: the abstract
     * pointer is never dereferenced once it may have been collected. ---- */
    {
        Fixture fx = fixture(fn);
        Janet probe = alloc_global(&probe_type, 64);
        fx.proc->exit_reason = probe; /* the ONLY reference to `probe` */
        assert(0 == listed(fx.fiber)); /* fast path is eligible */

        int marks_before = probe_marks;
        int gc_before = probe_gc_calls;
        for (int i = 0; i < 40; i++) janet_collect_fiber_minor(fx.fiber);
        assert(0 == listed(fx.fiber));
        assert(probe_marks == marks_before); /* fast path never traversed */
        assert(probe_gc_calls == gc_before); /* and never deinit'd it */

        /* A real full collection must reach it through proc->exit_reason. */
        janet_collect();
        assert(probe_marks > marks_before);
        assert(probe_gc_calls == gc_before); /* retained, not finalized */

        /* Still retained across a further scoped minor. */
        janet_collect_fiber_minor(fx.fiber);
        assert(probe_gc_calls == gc_before);
        fx.proc->exit_reason = janet_wrap_nil();
        release(&fx);
    }

    /* ---- D: repeated fast-path minors across the 4-bit epoch, then a real
     *          major; invariants must hold afterwards. ---- */
    {
        Fixture fx = fixture(fn);
        for (int i = 0; i < 40; i++) janet_collect_fiber_minor(fx.fiber);
        assert(pending_empty());
        janet_collect();
        /* A major's quarantine drain can free fibers after sweep_heaps has
         * already drained, so sweep_pending may legitimately be non-empty
         * here; the fast path must still clear it (it calls drain_pending). */
        janet_collect_fiber_minor(fx.fiber);
        assert(pending_empty());
        alloc_owned(fx.fiber, 4096);
        fx.fiber->gc_nursery_bytes = 8;
        janet_collect_fiber_minor(fx.fiber);
        assert(pending_empty());
        release(&fx);
    }

    janet_collect();
    janet_deinit();
    puts("eligible-minor: ok");
    return 0;
}
