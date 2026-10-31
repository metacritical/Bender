/*
* Copyright (c) 2026 Calvin Rose
*
* Permission is hereby granted, free of charge, to any person obtaining a copy
* of this software and associated documentation files (the "Software"), to
* deal in the Software without restriction, including without limitation the
* rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
* sell copies of the Software, and to permit persons to whom the Software is
* furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included in
* all copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
* AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
* FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
* IN THE SOFTWARE.
*/

#ifndef BENDER_AMALG
#include "features.h"
#include <bender.h>
#include "state.h"
#include <unistd.h>
#include <execinfo.h>
#include "symcache.h"
#include "gc.h"
#include "proc.h"
#include "util.h"
#include "fiber.h"
#include "ev.h"
#include "vector.h"
#include "compile.h"
#endif

#ifndef BENDER_NATIVE_TELEMETRY
#define BENDER_NATIVE_TELEMETRY 1
#endif
#if defined(BENDER_NATIVE_EXECUTORS) && BENDER_NATIVE_TELEMETRY
#define BENDER_GC_WORK_TICK(field) (++bender_vm.field)
#else
#define BENDER_GC_WORK_TICK(field) ((void)0)
#endif

/* Helpers for marking the various gc types */
static void bender_mark_funcenv(BenderFuncEnv *env);
static void bender_mark_funcdef(BenderFuncDef *def);
static void bender_mark_function(BenderFunction *func);
static void bender_mark_array(BenderArray *array);
static void bender_mark_table(BenderTable *table);
static void bender_mark_struct(const BenderKV *st);
static void bender_mark_tuple(const Bender *tuple);
static void bender_mark_buffer(BenderBuffer *buffer);
static void bender_mark_string(const uint8_t *str);
static void bender_mark_fiber(BenderFiber *fiber);
static void bender_mark_abstract(void *adata);

/* ------------------------------------------------------------------------
 * Iterative closure-environment marking.
 *
 * Marking used to recurse one C frame per object edge:
 *   bender_mark -> mark_function -> mark_funcenv -> mark_many -> bender_mark
 * so a deep chain of closures (a lazy sequence, a recursive builder, deeply
 * nested generated code) exhausted the C stack inside a collection and
 * crashed on the guard page instead of raising an error. Measured onset was
 * ~150k-180k links.
 *
 * Only the closure-environment edge needs to be iterative to remove that
 * unbounded recursion: every other container marker (tuple, array, table,
 * struct, buffer) recurses through a fixed number of frames and its children
 * are bounded by the container's own size, whereas an environment chain is
 * unbounded by construction. Enqueuing environments here makes mark depth
 * constant for the one edge that grows without limit.
 *
 * The queue is thread-local and saved/restored around each drain: marking can
 * re-enter (bender_env_maybe_detach allocates, which may itself collect), so an
 * inner operation must not observe or clobber the outer operation's pending
 * work. It grows with raw realloc for the same reason bender_gcroot does -- a
 * push must never be able to trigger the collection it is protecting against.
 * ---------------------------------------------------------------------- */
static BENDER_THREAD_LOCAL BenderFuncEnv **env_queue = NULL;
static BENDER_THREAD_LOCAL size_t env_queue_count = 0;
static BENDER_THREAD_LOCAL size_t env_queue_capacity = 0;
static BENDER_THREAD_LOCAL int env_queue_draining = 0;

static void bender_drain_env_queue(void);

static void bender_env_queue_push(BenderFuncEnv *env) {
    size_t newcount = env_queue_count + 1;
    if (newcount > env_queue_capacity) {
        size_t newcap = env_queue_capacity ? env_queue_capacity * 2 : 64;
        if (newcap < newcount) newcap = newcount;
        env_queue = bender_realloc(env_queue, sizeof(BenderFuncEnv *) * newcap);
        if (NULL == env_queue) BENDER_OUT_OF_MEMORY;
        env_queue_capacity = newcap;
    }
    env_queue[env_queue_count] = env;
    env_queue_count = newcount;
}

#ifdef BENDER_EV
/* Phase 2b-iii + Erlang-parity residual O(n) fix: a minor's mark phase
 * can flip a threaded abstract's registry "visited this cycle" marker
 * (gc.c first-visit gcmark dispatch), but bender_collect_minor calls
 * bender_sweep_heaps directly, never bender_sweep -- so with the original
 * boolean marker only a full bender_sweep's reset loop ever cleared it,
 * and a stale true marker would let the next cycle's mark skip re-running
 * gcmark on that abstract's internals (exactly the Phase 1
 * bender_mark_table crash chain pointed at via bender_thread_handle_mark).
 *
 * ORIGINAL FIX (O(n) per minor): a post-minor pass reset every abstract
 * entry back to false -- but it walked the ENTIRE registry table
 * (capacity-sized, ~= every channel/abstract ever registered) after
 * EVERY fiber minor. With minors ~= N actors and registry ~= N entries
 * that is O(N^2) overall, and it measured as 97% of all samples in a
 * `sample` profile of bench/agents.ben N=100000 (see
 * docs/internals/proc-lanes.md).
 *
 * CURRENT FORM (O(1) per collection): the marker is a per-collection
 * CYCLE STAMP (BenderVM.gc_visit_cycle, bumped in bender_gc_epoch_next
 * at the start of every collection). An entry is "visited this
 * collection" iff its registry value is a number equal to the current
 * cycle. Stamps from previous cycles are automatically stale -- no
 * reset pass at all. The marker never touches the refcount,
 * home_gc_authorized, or any teardown decision, which stay major-only
 * (a minor still cannot decide liveness/teardown).
 *
 * Valid registry values: nil (never registered), false (registered,
 * not visited -- written by abstract.c's registration and re-arm
 * paths, marsh.c's transfer, bender_sweep's post-major reset, and
 * tombstones), or a number (the cycle that last visited it; stale
 * numbers read as not-visited). uint32 wrap needs the same entry to
 * survive, unvisited, exactly 2^32 collections -- accounting noise
 * only, same class as gc_owner wrap. */
static int bender_threaded_visited(Bender marker) {
    return bender_checktype(marker, BENDER_NUMBER)
           && bender_unwrap_number(marker) == (double) bender_vm.gc_visit_cycle;
}
#endif

/* Local state that is only temporary for gc */
static BENDER_THREAD_LOCAL uint32_t depth = BENDER_RECURSION_GUARD;
#ifdef BENDER_GC_COUNT
static long g_n_minor = 0, g_n_major = 0, g_n_owned = 0, g_n_proc = 0;
#endif

/* Generational gate (Phase 2b-iii policy), defined with the slab code
 * below; used by the table/array markers. */
static int bender_gc_minor_skip(BenderGCObject *mem);
/* Object old for generational purposes (slab page age, or malloc-large);
 * defined with the slab code below. */
static int bender_slab_obj_old(void *ptr);
/* Step 1.3 owner for sweep filtering (0 = shared); defined below. */
static uint32_t bender_slab_obj_owner(void *ptr);
/* Reset generational slab state after a full sweep; defined below. */
static void bender_slab_major_reset(void);
/* Step 1.3 census removal at sweep-free; defined with the slab code. */
static void bender_census_remove(BenderFiber *f);
static void bender_sweep_heaps(int walk_fibers);
static void bender_sweep_fiber_owned(BenderFiber *f);
static void bender_sweep_proc_owned(BenderFiber *f);

/* Hint to the GC that we may need to collect */
void bender_gcpressure(size_t s) {
    bender_vm.next_collection += s;
}

static void bender_clear_all_reachable_bits(void);

/* Phase 1.1 epoch mark: gc.c-local 4-bit epoch in flags 11-14. Stale
 * bits self-invalidate PROVIDED no listed block carries a stamp out of
 * a sweep: the sweep zeroes every survivor AND every unmarked floater
 * (old/foreign/fiber blocks a minor or scoped sweep keeps), zeroes
 * blocks on entry to quarantine, and full collects pre-clear every
 * listed stamp before marking (bender_clear_all_reachable_bits) to
 * erase stamps left by scoped collections on lists they do not sweep.
 * With those, "nonzero stamp == marked this collection" holds for
 * every listed block at every sweep entry point, so the 15-value wrap
 * cannot resurrect a dead block as a false reachable verdict. */
static BENDER_THREAD_LOCAL int32_t bender_gc_epoch = 0;

#undef bender_gc_mark
#undef bender_gc_reachable
#define bender_gc_mark(m) (bender_gc_header(m)->flags = \
    ((bender_gc_header(m)->flags & ~BENDER_MEM_EPOCH_MASK) \
     | (bender_gc_epoch << BENDER_MEM_EPOCH_SHIFT)))
#define bender_gc_reachable(m) \
    (((bender_gc_header(m)->flags & BENDER_MEM_EPOCH_MASK) >> BENDER_MEM_EPOCH_SHIFT) \
     == bender_gc_epoch)

static void bender_gc_epoch_next(void) {
    bender_gc_epoch = (bender_gc_epoch % 15) + 1;
    /* Re-base the proc-chain depth for a fresh, non-nested collection.
     * bender_procat_mark balances its own ++/--, but a longjmp out of the
     * mark phase (bender_env_maybe_detach allocates, and an allocation
     * failure panics) would leave the counter high forever, after which
     * every later scoped minor would shallow-mark proc fibers immediately
     * and could free a still-live object. gc_mark_phase is still 0 here for
     * a top-level collection and already 1 for one re-entered from inside
     * marking, so a nested collection correctly leaves the outer walk's
     * depth alone. */
    if (!bender_vm.gc_mark_phase) bender_proc_mark_depth = 0;
    /* Threaded-abstract visit cycle (Erlang-parity residual O(n) fix):
     * this function runs exactly once at the start of EVERY collection
     * (fiber minor, proc minor, global minor, full major), so bumping
     * the visit cycle here guarantees each collection sees a fresh
     * cycle stamp. "Visited this collection" for a threaded abstract
     * registry entry is then value == gc_visit_cycle, which makes the
     * old post-minor whole-registry reset pass unnecessary. */
    bender_vm.gc_visit_cycle++;
}

/* Mark a value */
/* Mark a value */
void bender_mark(Bender x) {
    if (depth) {
        depth--;
        switch (bender_type(x)) {
            default:
                break;
            case BENDER_STRING:
            case BENDER_KEYWORD:
            case BENDER_SYMBOL:
                bender_mark_string(bender_unwrap_string(x));
                break;
            case BENDER_FUNCTION:
                bender_mark_function(bender_unwrap_function(x));
                break;
            case BENDER_ARRAY:
                bender_mark_array(bender_unwrap_array(x));
                break;
            case BENDER_TABLE:
                bender_mark_table(bender_unwrap_table(x));
                break;
            case BENDER_STRUCT:
                bender_mark_struct(bender_unwrap_struct(x));
                break;
            case BENDER_TUPLE:
                bender_mark_tuple(bender_unwrap_tuple(x));
                break;
            case BENDER_BUFFER:
                bender_mark_buffer(bender_unwrap_buffer(x));
                break;
            case BENDER_FIBER:
                bender_mark_fiber(bender_unwrap_fiber(x));
                break;
            case BENDER_ABSTRACT:
                bender_mark_abstract(bender_unwrap_abstract(x));
                break;
        }
        depth++;
    } else {
        bender_gcroot(x);
    }
}

static void bender_mark_string(const uint8_t *str) {
    bender_gc_mark(bender_string_head(str));
}

static void bender_mark_buffer(BenderBuffer *buffer) {
    bender_gc_mark(buffer);
}

static void bender_mark_abstract(void *adata) {
#ifdef BENDER_ASAN_UAF_TRACE
    if ((uintptr_t) adata < 4096) {
        char bbuf[128];
        int bl = snprintf(bbuf, sizeof bbuf,
                "[bogus-abstract] adata=%p ra=%p\n", adata,
                __builtin_return_address(0));
        write(2, bbuf, (size_t) bl);
        abort();
    }
    if (NULL == bender_abstract_head(adata)->type) {
        char bbuf[512];
        int bl = snprintf(bbuf, sizeof bbuf,
                "[mark-tombstoned] adata=%p"
                " ra0=%p ra1=%p ra2=%p ra3=%p ra4=%p ra5=%p ra6=%p\n", adata,
                __builtin_return_address(0),
                __builtin_return_address(1),
                __builtin_return_address(2),
                __builtin_return_address(3),
                __builtin_return_address(4),
                __builtin_return_address(5),
                __builtin_return_address(6));
        write(2, bbuf, (size_t) bl);
        abort();
    }
#endif
#ifdef BENDER_EV
    /* Check if abstract type is a threaded abstract type. If it is, marking means
     * updating the threaded_abstract table. */
    if ((bender_abstract_head(adata)->gc.flags & BENDER_MEM_TYPEBITS) == BENDER_MEMORY_THREADED_ABSTRACT) {
        /* A home major mark authorizes teardown deferral; only an
         * unvisited entry at the end of a complete home major revokes it.
         * Foreign, minor, and scoped traversals cannot change the grant.
         * The four-bit GC epoch is not a durable lifetime verdict. */
        if (bender_vm.gc_mark_phase && !bender_vm.gc_minor_phase &&
                bender_abstract_head(adata)->home_vm == (void *) &bender_vm)
            bender_abstract_head(adata)->home_gc_authorized = 1;
        /* Visit-flag semantics: the table's value is this cycle's
         * "already visited" marker -- a per-collection cycle stamp
         * (see bender_threaded_visited); first visit this cycle stamps
         * it and runs gcmark so the abstract's INTERNALS — channel
         * items, pending-entry values — are marked exactly like
         * non-threaded abstracts'. Without this, values buffered
         * inside a channel were collectable at any GC boundary,
         * surviving only while a sender-side reference happened to
         * keep them alive. */
        Bender kv = bender_wrap_abstract(adata);
        Bender registered = bender_table_get(&bender_vm.threaded_abstracts, kv);
        if (!bender_threaded_visited(registered)) {
            /* A foreign raw heap edge can reach this abstract without
             * unmarshaling it first. Its first registry insertion must own
             * a claim, just like a transferred channel-buffer reference.
             * An existing false (or stale-stamp) slot already owns one. */
            if (bender_checktype(registered, BENDER_NIL))
                bender_abstract_incref(adata);
            bender_table_put(&bender_vm.threaded_abstracts, kv,
                            bender_wrap_number((double) bender_vm.gc_visit_cycle));
            /* Stale heap references can outlive the abstract: freed heads
             * are poisoned (type == NULL), nothing left to mark, and
             * reading the freed header is unsound. */
            if (bender_abstract_head(adata)->type && bender_abstract_head(adata)->type->gcmark) {
                bender_abstract_head(adata)->type->gcmark(adata, bender_abstract_size(adata));
            }
        }
        return;
    }
#endif
    if (bender_gc_reachable(bender_abstract_head(adata)))
        return;
    bender_gc_mark(bender_abstract_head(adata));
    if (bender_abstract_head(adata)->type->gcmark) {
        bender_abstract_head(adata)->type->gcmark(adata, bender_abstract_size(adata));
    }
}

/* Mark a bunch of items in memory */
static void bender_mark_many(const Bender *values, int32_t n) {
    if (values == NULL)
        return;
    const Bender *end = values + n;
    while (values < end) {
        bender_mark(*values);
        values += 1;
    }
}

/* Mark a bunch of key values items in memory */
static void bender_mark_keys(const BenderKV *kvs, int32_t n) {
    const BenderKV *end = kvs + n;
    while (kvs < end) {
        bender_mark(kvs->key);
        kvs++;
    }
}

/* Mark a bunch of key values items in memory */
static void bender_mark_values(const BenderKV *kvs, int32_t n) {
    const BenderKV *end = kvs + n;
    while (kvs < end) {
        bender_mark(kvs->value);
        kvs++;
    }
}

/* Mark a bunch of key values items in memory */
static void bender_mark_kvs(const BenderKV *kvs, int32_t n) {
    const BenderKV *end = kvs + n;
    while (kvs < end) {
        bender_mark(kvs->key);
        bender_mark(kvs->value);
        kvs++;
    }
}

static void bender_mark_array(BenderArray *array) {
    if (bender_gc_reachable(array))
        return;
    if (bender_gc_minor_skip((BenderGCObject *) array))
        return;
    bender_gc_mark(array);
    if (bender_gc_type((BenderGCObject *) array) == BENDER_MEMORY_ARRAY) {
        bender_mark_many(array->data, array->count);
    }
}

static void bender_mark_table(BenderTable *table) {
recur: /* Manual tail recursion */
    if (bender_gc_reachable(table))
        return;
    if (bender_gc_minor_skip((BenderGCObject *) table))
        return;
    bender_gc_mark(table);
    enum BenderMemoryType memtype = bender_gc_type(table);
    if (memtype == BENDER_MEMORY_TABLE_WEAKK) {
        bender_mark_values(table->data, table->capacity);
    } else if (memtype == BENDER_MEMORY_TABLE_WEAKV) {
        bender_mark_keys(table->data, table->capacity);
    } else if (memtype == BENDER_MEMORY_TABLE) {
        bender_mark_kvs(table->data, table->capacity);
    }
    /* do nothing for BENDER_MEMORY_TABLE_WEAKKV */
    if (table->proto) {
        table = table->proto;
        goto recur;
    }
}

static void bender_mark_struct(const BenderKV *st) {
recur:
    if (bender_gc_reachable(bender_struct_head(st)))
        return;
    bender_gc_mark(bender_struct_head(st));
    bender_mark_kvs(st, bender_struct_capacity(st));
    st = bender_struct_proto(st);
    if (st) goto recur;
}

static void bender_mark_tuple(const Bender *tuple) {
    if (bender_gc_reachable(bender_tuple_head(tuple)))
        return;
    bender_gc_mark(bender_tuple_head(tuple));
    bender_mark_many(tuple, bender_tuple_length(tuple));
}

/* Helper to mark function environments */
static void bender_mark_funcenv(BenderFuncEnv *env) {
    if (bender_gc_reachable(env))
        return;
    bender_gc_mark(env);
    /* If closure env references a dead fiber, we can just copy out the stack frame we need so
     * we don't need to keep around the whole dead fiber. */
    bender_env_maybe_detach(env);
    if (env->offset > 0) {
        /* On stack */
        bender_mark_fiber(env->as.fiber);
    } else {
        /* Not on stack */
        bender_mark_many(env->as.values, env->length);
    }
}

/* Drain enqueued closure environments iteratively.
 *
 * The outermost caller owns the drain. Nested calls (a marker reached from
 * inside this loop) only enqueue, so recursion depth stays constant however
 * long the closure chain is. Marking can re-enter -- bender_env_maybe_detach
 * allocates, which may collect -- so the queue is saved and restored around
 * the drain: an inner operation must never consume or discard the outer
 * operation's pending work.
 *
 * env_queue grows with raw realloc, so pushing can never trigger a collection
 * (the same rule bender_gcroot follows). */
static void bender_drain_env_queue(void) {
    BenderFuncEnv **outer_queue;
    size_t outer_count;
    if (env_queue_draining) return;
    /* Take ownership of what is queued so far. */
    outer_queue = env_queue;
    outer_count = env_queue_count;
    /* Install a fresh private queue for this drain. */
    env_queue = NULL;
    env_queue_count = 0;
    env_queue_capacity = 0;
    env_queue_draining = 1;
    while (outer_count) {
        BenderFuncEnv *env = outer_queue[--outer_count];
        /* Any recursion inside this call enqueues onto the private queue. */
        bender_mark_funcenv(env);
        /* Absorb whatever the private queue accumulated. */
        if (env_queue_count) {
            size_t need = outer_count + env_queue_count;
            if (need > outer_count) {
                BenderFuncEnv **grown = bender_realloc(outer_queue, sizeof(BenderFuncEnv *) * need);
                if (NULL == grown) BENDER_OUT_OF_MEMORY;
                outer_queue = grown;
            }
            memcpy(outer_queue + outer_count, env_queue, sizeof(BenderFuncEnv *) * env_queue_count);
            outer_count += env_queue_count;
            env_queue_count = 0;
        }
    }
    /* Restore the caller's queue state. */
    bender_free(env_queue);
    env_queue = outer_queue;
    env_queue_count = outer_count;
    env_queue_capacity = outer_count;
    env_queue_draining = 0;
}

/* GC helper to mark a FuncDef */
static void bender_mark_funcdef(BenderFuncDef *def) {
    int32_t i;
    if (bender_gc_reachable(def))
        return;
    bender_gc_mark(def);
    bender_mark_many(def->constants, def->constants_length);
    for (i = 0; i < def->defs_length; ++i) {
        bender_mark_funcdef(def->defs[i]);
    }
    if (def->source)
        bender_mark_string(def->source);
    if (def->name)
        bender_mark_string(def->name);
    if (def->symbolmap) {
        for (int i = 0; i < def->symbolmap_length; i++) {
            bender_mark_string(def->symbolmap[i].symbol);
        }
    }

}

static void bender_mark_function(BenderFunction *func) {
    int32_t i;
    int32_t numenvs;
    if (bender_gc_reachable(func))
        return;
    bender_gc_mark(func);
    if (NULL != func->def) {
        /* this should always be true, except if function is only partially constructed */
        numenvs = func->def->environments_length;
        for (i = 0; i < numenvs; ++i) {
            bender_env_queue_push(func->envs[i]);
        }
        bender_mark_funcdef(func->def);
        bender_drain_env_queue();
    }
}

/* Phase 2a root discipline: mark everything a live compilation holds in
 * C structs invisible to the collector (scope consts/syms/defs, lint and
 * pin arrays, env, in-progress result). Nested compiles (macros calling
 * compile) chain via compiler_next, most recent first. Scopes form a
 * parent chain from the current (innermost) scope; popped scopes merge
 * their symbols upward and free their vectors, so walking parents covers
 * everything live. Raw symbol bytes are marked by wrapping (symbols are
 * the only string-likes stashed raw); integer/buffer state needs nothing.
 * Runs inside bender_collect, where the compiler stack only ever holds
 * live C-stack state. */
static void bender_mark_compiler(BenderCompiler *c) {
    if (c->env) bender_mark_table(c->env);
    if (c->lints) bender_mark_array(c->lints);
    if (c->result.funcdef) bender_mark_funcdef(c->result.funcdef);
    for (int32_t i = 0; i < bender_v_count(c->retained_defs); i++) {
        bender_mark(c->retained_defs[i].hint);
        if (c->retained_defs[i].parent) bender_mark_funcdef(c->retained_defs[i].parent);
        if (c->retained_defs[i].child) bender_mark_funcdef(c->retained_defs[i].child);
    }
    for (BenderScope *s = c->scope; s; s = s->parent) {
        if (s->consts) {
            bender_mark_many(s->consts, bender_v_count(s->consts));
        }
        if (s->syms) {
            int32_t n = bender_v_count(s->syms);
            for (int32_t i = 0; i < n; i++) {
                bender_mark(s->syms[i].slot.constant);
                if (s->syms[i].sym) {
                    bender_mark(bender_wrap_string(s->syms[i].sym));
                }
                if (s->syms[i].sym2 && s->syms[i].sym2 != s->syms[i].sym) {
                    bender_mark(bender_wrap_string(s->syms[i].sym2));
                }
            }
        }
        if (s->defs) {
            int32_t n = bender_v_count(s->defs);
            for (int32_t i = 0; i < n; i++) {
                if (s->defs[i]) bender_mark_funcdef(s->defs[i]);
            }
        }
    }
}

static void bender_mark_fiber(BenderFiber *fiber) {
    int32_t i, j;
    BenderStackFrame *frame;
recur:
    if (bender_gc_reachable(fiber))
        return;
#ifdef BENDER_ASAN_UAF_TRACE
    extern int __asan_address_is_poisoned(void const volatile *addr);
    extern void __sanitizer_print_stack_trace(void);
    if (__asan_address_is_poisoned(fiber)) {
        /* Crash-safe report: write(2) only -- bender_eprintf allocates,
         * and a GC inside the GC is how we got here. */
        uintptr_t fa = (uintptr_t) fiber;
        char buf[128];
        char *w = buf;
        const char *pfx = "[uaf-mark] swept fiber 0x";
        for (const char *q = pfx; *q; q++) *w++ = *q;
        for (int shift = 60; shift >= 0; shift -= 4) {
            int nib = (int)((fa >> shift) & 0xF);
            *w++ = nib < 10 ? '0' + nib : 'a' + nib - 10;
        }
        *w++ = '\n';
        size_t len = (size_t)(w - buf);
        int fd = 2;
        do { ssize_t r = write(fd, buf, len); if (r > 0) break; } while (1);
        __sanitizer_print_stack_trace();
    }
#endif
    bender_gc_mark(fiber);

    /* P3.5: proc fibers pin their proc (and procs pin their fibers
     * in procat_mark), so neither pointer dangles while the other
     * lives; the cycle collects together. */
    if (NULL != fiber->proc) {
        bender_mark(bender_wrap_abstract(fiber->proc));
    }
#ifdef BENDER_ASAN_UAF_TRACE
    {
        Bender vals[4] = {fiber->last_value, fiber->in_value,
                         fiber->out_payload, fiber->interrupt_value};
        const char *names[4] = {"last", "in", "out", "interrupt"};
        for (int k = 0; k < 4; k++) {
            if (bender_checktype(vals[k], BENDER_ABSTRACT)) {
                void *ad = bender_unwrap_abstract(vals[k]);
                if (NULL == bender_abstract_head(ad)->type)
                    fprintf(stderr, "[fiber-stale-field] fiber=%p field=%s adata=%p status=%d\n",
                            (void *) fiber, names[k], ad, (int) bender_fiber_status(fiber));
            }
        }
    }
#endif
    bender_mark(fiber->last_value);
    /* Values in flight across a context switch. The resume argument and
     * the last result live in the fiber struct (not in fiber->data), so
     * they must be marked explicitly or a collection between suspend and
     * resume could free them. The VM clears consumed input when parking
     * or completing, rather than hiding a potentially live root here. */
    bender_mark(fiber->in_value);
    bender_mark(fiber->out_payload);
#ifdef BENDER_EV
    bender_mark(fiber->interrupt_value);
#endif
    /* Phase 1.1: the resumer link keeps a suspended fiber's resume party
     * alive across collections while swapped out. */
    if (fiber->resume_fiber) {
        bender_mark(bender_wrap_fiber(fiber->resume_fiber));
    }

    /* BUG 2: a completed fiber's stack frames are stale. The
     * completion minor frees the young garbage they reference
     * (frame->func/env), while the fiber struct itself stays
     * reachable until its own sweep -- a later mark re-walking the
     * frames read freed memory (ASAN: bender_mark_function
     * use-after-free at density; the crash's flip side was
     * "bender_hash on a freed abstract"). Nothing resumes a fiber in
     * a terminal status, so only resumable fibers walk their
     * frames; last_value/out_payload/env above stay marked for
     * everyone. */
    {
        BenderFiberStatus fst = bender_fiber_status(fiber);
        int walk_frames = !(BENDER_STATUS_DEAD == fst ||
                            BENDER_STATUS_ERROR == fst);
        if (walk_frames) {
    /* Mark values on the argument stack */
    bender_mark_many(fiber->data + fiber->stackstart,
                    fiber->stacktop - fiber->stackstart);

#ifdef BENDER_ASAN_UAF_TRACE
    extern int __asan_address_is_poisoned(void const volatile *addr);
    extern int backtrace(void **, int);
    extern void backtrace_symbols_fd(void *const *, int, int);
    if (NULL != fiber->data && __asan_address_is_poisoned(fiber->data)) {
        void *bt[24];
        int n = backtrace(bt, 24);
        backtrace_symbols_fd(bt, n, 2);
    }
#endif
    i = fiber->frame;
    j = fiber->stackstart - BENDER_FRAME_SIZE;
    while (i > 0) {
        frame = (BenderStackFrame *)(fiber->data + i - BENDER_FRAME_SIZE);
        if (NULL != frame->func) {
#ifdef BENDER_ASAN_UAF_TRACE
            if (__asan_address_is_poisoned(frame->func)) {
                __sanitizer_print_stack_trace();
            }
#endif
            bender_mark_function(frame->func);
        }
        if (NULL != frame->env)
            bender_mark_funcenv(frame->env);
        /* Mark all values in the stack frame */
        bender_mark_many(fiber->data + i, j - i);
        j = i - BENDER_FRAME_SIZE;
        i = frame->prevframe;
    }
        }
    }

    if (fiber->env)
        bender_mark_table(fiber->env);

#ifdef BENDER_EV
    if (fiber->supervisor_channel) {
        bender_mark_abstract(fiber->supervisor_channel);
    }
    if (fiber->ev_stream) {
        bender_mark_abstract(fiber->ev_stream);
    }
    if (fiber->ev_callback) {
        fiber->ev_callback(fiber, BENDER_ASYNC_EVENT_MARK);
    }
    /* Pending-registration reverse edge: parked fibers pin their
     * channels (see bender_ev_chanpends_mark). */
    bender_ev_chanpends_mark(fiber);
#endif

    /* Supervisor channel: fibers hold their sup channel outside any
     * Bender-visible value once wired (proc spawn, ev/go and tasklet
     * wiring set it directly). Threaded abstracts are refcounted, and
     * bender_sweep's registry pass decrefs every unvisited entry each
     * full collect — an unmarked sup therefore dies under a parked or
     * running fiber, and freed channel memory is reused as Bender
     * values (the premature-free UAF family: clobbered closure defs,
     * registry slots, ASAN run_vm read of a freed 132-byte channel).
     * Marking it with the fiber pins it for every collector: visited
     * entries are never decref'd, and the pass reclaims it on the
     * first full collect after the fiber dies. */
    if (NULL != fiber->supervisor_channel)
        bender_mark(bender_wrap_abstract(fiber->supervisor_channel));

    /* Explicit tail recursion */
    if (fiber->child) {
        fiber = fiber->child;
        goto recur;
    }
}

/* Erlang-parity messaging fix: see gc.h's declaration for the full
 * rationale and docs/internals/proc-lanes.md for the lldb evidence.
 * Sets ONLY the reachable bit -- no frame walk, no stack/env/child
 * recursion. Correct specifically for a scoped (fiber/process) minor's
 * defensive need: protect a QUEUED fiber's OWN block in case it
 * happens to live on the heap being swept, without taking on the
 * unrelated job of proving that fiber's own deep reachability (that
 * is whichever collection actually owns and sweeps it). NOT a
 * substitute for bender_mark_fiber in any context that needs real
 * reachability (bender_mark_roots / global minor / major must keep
 * using the full deep mark). */
void bender_gc_mark_fiber_shallow(BenderFiber *fiber) {
    if (NULL == fiber || bender_gc_reachable(fiber)) return;
    bender_gc_mark(fiber);
}

/* Deinitialize a block of memory */
static void bender_deinit_block(BenderGCObject *mem) {
    switch (mem->flags & BENDER_MEM_TYPEBITS) {
        default:
        case BENDER_MEMORY_FUNCTION:
            break; /* Do nothing for non gc types */
        case BENDER_MEMORY_SYMBOL:
            bender_symbol_deinit(((BenderStringHead *) mem)->data);
            break;
        case BENDER_MEMORY_ARRAY:
        case BENDER_MEMORY_ARRAY_WEAK:
            bender_free(((BenderArray *) mem)->data);
            break;
        case BENDER_MEMORY_TABLE:
        case BENDER_MEMORY_TABLE_WEAKK:
        case BENDER_MEMORY_TABLE_WEAKV:
        case BENDER_MEMORY_TABLE_WEAKKV:
            bender_free(((BenderTable *) mem)->data);
            break;
        case BENDER_MEMORY_FIBER: {
            BenderFiber *f = (BenderFiber *)mem;
#ifdef BENDER_EV
            if (f->ev_state && !(f->flags & BENDER_FIBER_EV_FLAG_IN_FLIGHT)) {
                bender_ev_dec_refcount();
                bender_free(f->ev_state);
            } else if (f->gc.flags & BENDER_FIBER_EV_GCFLAG_SUSPENDED) {
                bender_ev_dec_refcount();
            }
            /* Channel-pending registrations (unlink-on-free): entries
             * still queued get flagged dead in their nodes — consumer
             * paths reclaim them lazily and never dereference this
             * fiber again — and unqueued nodes are freed now. */
            bender_ev_chanpends_deinit(f);
#endif
            /* P3.5b: park the dead fiber's heap lists for post-sweep
             * drain into the globals (see sweep_pending). Immediate
             * splicing here would race the in-progress sweep unlink
             * (orphaned chains, counted-but-unlisted). Chains prepend
             * per kind (each chain walked once at drain). Pointer
             * moves only: safe mid-sweep. */
            {
                int khi;
                for (khi = 0; khi < 3; khi++) {
                    BenderGCObject *fl = (BenderGCObject *) f->heap_lists[khi];
                    if (NULL != fl) {
                        BenderGCObject *tail = fl;
                        while (NULL != tail->data.next)
                            tail = tail->data.next;
                        tail->data.next = (BenderGCObject *) bender_vm.sweep_pending[khi];
                        bender_vm.sweep_pending[khi] = (void *) fl;
                        f->heap_lists[khi] = NULL;
                    }
                }
            }
            /* Step 1.3: leave the live-fiber census before release. */
            bender_census_remove(f);
#ifdef BENDER_ASAN_UAF_TRACE
            {
                extern int backtrace(void **, int);
                extern void backtrace_symbols_fd(void *const *, int, int);
                /* Is this fiber still in vm.roots while we destroy it?
                 * Yes => mark/sweep disagreement (a root entry that
                 * marking never honored). No => the root entry that
                 * later dangles was written after this free. */
                size_t ri;
                int rooted = 0;
                for (ri = 0; ri < bender_vm.root_count; ri++) {
                    Bender rv = bender_vm.roots[ri];
                    if (bender_checktype(rv, BENDER_FIBER) &&
                            bender_unwrap_fiber(rv) == f) {
                        rooted = 1;
                        break;
                    }
                }
                if (rooted) {
                    char bbuf[128];
                    int bl = snprintf(bbuf, sizeof bbuf,
                            "[free-rooted] fiber=%p data=%p rootidx=%zu\n",
                            (void *) f, (void *) f->data, ri);
                    write(2, bbuf, (size_t) bl);
                    void *bt[16];
                    int bn = backtrace(bt, 16);
                    backtrace_symbols_fd(bt, bn, 2);
                }
            }
#endif
            bender_free(f->data);
            bender_fiber_native_stack_free(f);
        }
        break;
        case BENDER_MEMORY_BUFFER:
            bender_buffer_deinit((BenderBuffer *) mem);
            break;
        case BENDER_MEMORY_ABSTRACT: {
            BenderAbstractHead *head = (BenderAbstractHead *)mem;
            /* Poisoned (type == NULL) means the abstract was already
             * finalized and released by the refcount path: skip both the
             * tombstone and the finalizers, storage drops below. */
            if (NULL == head->type) break;
            if (head->gc.flags & BENDER_MEMORY_THREADED_ABSTRACT) {
                /* D3: tombstone at death. Scoped sweeps free channels
                 * between full collects, leaving registry entries whose
                 * header is freed memory; the registry's raw-array walks
                 * (bender_sweep's unvisited pass, clear_memory's teardown)
                 * then read freed headers. bender_table_remove only nils
                 * slots in place (no rehash), so iteration stays valid. */
                bender_table_put(&bender_vm.threaded_abstracts,
                                bender_wrap_abstract((void *) head->data),
                                bender_wrap_nil());
            }
            if (head->gc.flags & BENDER_MEMORY_THREADED_ABSTRACT) {
                /* Single-shot teardown: the home-routed callback may
                 * have torn it down (or been posted and about to);
                 * only one path runs the hooks (BUG 2 double-deinit). */
                if (1 != bender_atomic_inc(&head->teardown_started)) break;
            }
            if (head->type->gcperthread) {
                bender_assert(!head->type->gcperthread(head->data, head->size), "per-thread finalizer failed");
            }
            if (head->type->gc) {
                bender_assert(!head->type->gc(head->data, head->size), "finalizer failed");
            }
        }
        break;
        case BENDER_MEMORY_FUNCENV: {
            BenderFuncEnv *env = (BenderFuncEnv *)mem;
            if (0 == env->offset)
                bender_free(env->as.values);
        }
        break;
        case BENDER_MEMORY_FUNCDEF: {
            BenderFuncDef *def = (BenderFuncDef *)mem;
            /* TODO - get this all with one alloc and one free */
            bender_free(def->defs);
            bender_free(def->environments);
            bender_free(def->constants);
            bender_free(def->bytecode);
            bender_free(def->sourcemap);
            bender_free(def->closure_bitset);
            bender_free(def->symbolmap);
        }
        break;
    }
}

/* Check that a value x has been visited in the mark phase */
static int bender_check_liveref(Bender x) {
    switch (bender_type(x)) {
        default:
            return 1;
        case BENDER_ARRAY:
        case BENDER_TABLE:
        case BENDER_FUNCTION:
        case BENDER_BUFFER:
        case BENDER_FIBER: {
            int r = bender_gc_reachable(bender_unwrap_pointer(x));
            if (getenv("BENDER_ROOT_TRACE"))
                fprintf(stderr, "[LIVEREF] fib=%p flags=%x ep=%d cur=%d live=%d\n",
                        bender_unwrap_pointer(x),
                        bender_gc_header(bender_unwrap_pointer(x))->flags,
                        (int) ((bender_gc_header(bender_unwrap_pointer(x))->flags
                                & BENDER_MEM_EPOCH_MASK) >> BENDER_MEM_EPOCH_SHIFT),
                        bender_gc_epoch, r);
            return r;
        }
        case BENDER_STRING:
        case BENDER_SYMBOL:
        case BENDER_KEYWORD:
            return bender_gc_reachable(bender_string_head(bender_unwrap_string(x)));
        case BENDER_ABSTRACT:
            return bender_gc_reachable(bender_abstract_head(bender_unwrap_abstract(x)));
        case BENDER_TUPLE:
            return bender_gc_reachable(bender_tuple_head(bender_unwrap_tuple(x)));
        case BENDER_STRUCT:
            return bender_gc_reachable(bender_struct_head(bender_unwrap_struct(x)));
    }
}

/* Iterate over all allocated memory, and free memory that is not
 * marked as reachable. Flip the gc color flag for next sweep. */

/* Sweep one strong-heap list (Phase 2b-ii shared; 2b-iii generational).
 * In a major collection, every unmarked block is freed. In a minor
 * collection, only young blocks are freed; old blocks are left alone
 * (marked ones get their bit cleared like usual, unmarked old blocks
 * simply wait for the next major). Young/old comes from the slab page
 * (malloc-backed large objects always count as old). When sweeping the
 * string heap, every block must be a string or symbol (keywords intern
 * to symbols); otherwise it must be neither -- misrouting is a bug.
 * Step 1.3: only_owner != 0 frees only that owner's young unmarked
 * (per-fiber minors); 0 keeps the shared behavior exactly.
 * P3.5c death reaping: a dying fiber's own lists are swept inline
 * (same filter) before its deinit parks survivors — O(own) instead
 * of floating to globals. Sound: every caller runs post-mark, so
 * unmarked-own is garbage; `clear_memory` bypasses the owned paths
 * (calls deinit directly, no fresh marks). */
static void bender_sweep_list(BenderGCObject **head, int heap_kind, int is_minor,
                             uint32_t only_owner) {
    BenderGCObject *previous = NULL;
    BenderGCObject *current = *head;
    while (NULL != current) {
        BenderGCObject *next = current->data.next;
        if (heap_kind == 0) {
            enum BenderMemoryType type = bender_gc_type(current);
            bender_assert(type != BENDER_MEMORY_STRING && type != BENDER_MEMORY_SYMBOL,
                         "string block on main heap");
        } else if (heap_kind == 1) {
            enum BenderMemoryType type = bender_gc_type(current);
            bender_assert(type == BENDER_MEMORY_STRING || type == BENDER_MEMORY_SYMBOL,
                         "non-string block on string heap");
        }
        /* heap_kind 2 (weak heap) carries no assert; weak types live there. */
        if (current->flags & BENDER_MEM_DISABLED) {
            previous = current;
        } else if (bender_gc_reachable(current)) {
            /* Zero the survivor's epoch: the field's only writer is the
             * stamp, so zeroed = unreachable in every future cycle —
             * the wrap never resurrects, no wipe needed. */
            current->flags &= ~BENDER_MEM_EPOCH_MASK;
            previous = current;
        } else if ((!is_minor || !bender_slab_obj_old(current)) &&
                (0 == only_owner || bender_slab_obj_owner(current) == only_owner) &&
                !(is_minor && BENDER_MEMORY_FIBER == bender_gc_type(current))) {
            bender_vm.block_count--;
            if (NULL != previous) {
                previous->data.next = next;
            } else {
                *head = next;
            }
            /* Phase 1.1 quarantine: defer deinit+release by one drain.
             * The block stays INTACT so the next weak-drop judges weak
             * entries against real (pre-free) headers — freeing
             * immediately lets a recycled slot alias a weak referent
             * and keep dead entries alive (suite-ev2 count = 100 under
             * the epoch mark). Unlinked now, so marking never reaches
             * quarantined blocks; deinit runs once, at drain.
             * EXCEPTION: the string heap frees immediately — strings
             * and symbols are interned by content, and a quarantined
             * string would be found (and later freed) by a new intern,
             * dangling the symbol (boot image generation does exactly
             * this, deterministically). */
            if (heap_kind == 1) {
                bender_deinit_block(current);
                bender_gc_release(current);
            } else {
                /* Zero the epoch stamp on the way into quarantine: the
                 * verdict "dead" must persist into the next weak-drop
                 * (minors quarantine without judging weak entries, so
                 * quarantined headers can outlive this cycle). A stale
                 * stamp left intact here could match the wrapped epoch
                 * 15 collections later and read as a false live verdict
                 * — keeping a weak entry whose key drains immediately
                 * after (dangling). Zeroed, the header reads not-
                 * reachable at every future epoch, which is exactly
                 * the block's settled fate. */
                current->flags &= ~BENDER_MEM_EPOCH_MASK;
                current->data.next = bender_vm.gc_quarantine[heap_kind];
                bender_vm.gc_quarantine[heap_kind] = current;
                bender_vm.gc_quarantine_count++;
            }
        } else {
            /* Floater: survives this sweep UNMARKED (old block in a
             * minor, fiber in a minor, foreign-owner block in a scoped
             * sweep). Zero the stale stamp so the 15-value epoch wrap
             * can never read it back as reachable: an unmarked fiber
             * that parked through a minor keeps the stamp of the last
             * cycle that marked it, and bender_gc_epoch revisits every
             * value every 15 collections — a matching wrap then keeps
             * the dead block out of the free set (false survivor) and,
             * worse, keeps its weak-table entries alive at the next
             * major's weak-drop (suite-ev2's "no leaked roots"). With
             * the stamp zeroed here, "nonzero epoch stamp" again means
             * exactly "marked this collection" for every listed block
             * at all times, as the Phase 1.1 design assumes. */
            current->flags &= ~BENDER_MEM_EPOCH_MASK;
            previous = current;
        }
        current = next;
    }
}

static void bender_sweep_weak_drop_list(BenderGCObject *current);

/* Phase 1.1: destroy the quarantined blocks. Their headers were intact
 * through the last weak-drop; after this they are gone (deinit'd and
 * released; the decref path poisons type — 3dbb2e0b — so any late raw
 * read is guarded). */
static void bender_quarantine_drain(void) {
    int kind;
    for (kind = 0; kind < 3; kind++) {
        BenderGCObject *cur = bender_vm.gc_quarantine[kind];
        bender_vm.gc_quarantine[kind] = NULL;
        while (NULL != cur) {
            BenderGCObject *next = cur->data.next;
            bender_deinit_block(cur);
            bender_gc_release(cur);
            cur = next;
        }
    }
    bender_vm.gc_quarantine_count = 0;
}

/* Cap-drain: minors quarantine without running the weak-drop, so the
 * quarantine grows between full collects. When it passes this many
 * blocks, drain early — the drained headers are deinit'd+poisoned
 * (3dbb2e0b), and the next weak-drop's guarded reads treat them as
 * dead, which is the correct verdict for quarantined blocks. */
#define BENDER_QUARANTINE_CAP 200000
static void bender_quarantine_maybe_drain(void) {
    if (bender_vm.gc_quarantine_count > BENDER_QUARANTINE_CAP) {
        /* Judge weak entries BEFORE releasing the quarantined blocks:
         * an early drain without a weak-drop lets recycled slots alias
         * dead referents (check_liveref reads the new occupant's
         * epoch — suite-ev2 count = 100). The epoch mark makes the
         * weak-drop valid at any point in any cycle. */
        bender_sweep_weak_drop();
        bender_quarantine_drain();
    }
}


void bender_sweep(void) {
    bender_sweep_weak_drop();
    bender_sweep_heaps(1);

#ifdef BENDER_EV
    /* Sweep threaded abstract types for references to decrement */
    BenderKV *items = bender_vm.threaded_abstracts.data;
    for (int32_t i = 0; i < bender_vm.threaded_abstracts.capacity; i++) {
        if (bender_checktype(items[i].key, BENDER_ABSTRACT)) {
            /* Belt-and-braces for the tombstone-at-death hook: an entry
             * whose abstract died without tombstoning this VM's registry
             * (cross-thread free) reads a poisoned (type == NULL) header.
             * Purge it in place instead of dereferencing freed memory. */
            if (NULL == bender_abstract_head(bender_unwrap_abstract(items[i].key))->type) {
                items[i].key = bender_wrap_nil();
                items[i].value = bender_wrap_false();
                bender_vm.threaded_abstracts.deleted++;
                bender_vm.threaded_abstracts.count--;
                continue;
            }

            /* If item was not visited during the mark phase, then this
             * abstract type isn't present in the heap and needs its refcount
             * decremented, and shouuld be removed from table. If the refcount is
             * then 0, the item will be collected. This ensures that only one interpreter
             * will clean up the threaded abstract. */

            /* If not visited (cycle-stamp compare, see
             * bender_threaded_visited; false and stale stamps read as
             * not-visited)... */
            if (!bender_threaded_visited(items[i].value)) {
                void *abst = bender_unwrap_abstract(items[i].key);
                BenderAbstractHead *head = bender_abstract_head(abst);
                /* This unvisited entry is the home VM's completed major
                 * verdict, not a minor/scoped or foreign VM traversal.
                 * Revoke before dropping the registry claim so a queued
                 * zero-ref callback cannot rearm an unreachable object. */
                if (head->home_vm == (void *) &bender_vm) {
                    head->home_gc_authorized = 0;
                }
                if (head->type->gcperthread) {
                    bender_assert(!head->type->gcperthread(head->data, head->size), "per-thread finalizer failed");
                }
                bender_abstract_decref_maybe_free(abst);

                /* Tombstone in place — skip if the decref path already
                 * tombstoned it (abstract.c's refcount free tombstones
                 * with its own bookkeeping; counting twice underflows
                 * count and corrupts later registry operations). */
                if (!bender_checktype(items[i].key, BENDER_NIL)) {
                    items[i].key = bender_wrap_nil();
                    items[i].value = bender_wrap_false();
                    bender_vm.threaded_abstracts.deleted++;
                    bender_vm.threaded_abstracts.count--;
                }
            }

            /* Reset for next sweep */
            items[i].value = bender_wrap_false();
        }
    }
#endif
    /* Quarantine drain: the weak-drop above has judged every entry
     * against intact headers; the quarantined blocks are now truly
     * unreferenced and can be destroyed. */
    bender_quarantine_drain();
    /* A full sweep leaves every survivor old: reset all page ages and
     * merge the old freelists back so old slots become reusable young
     * slots (conservative restart for the next minor cycle). */
    bender_slab_major_reset();
}

/* Allocate some memory that is tracked for garbage collection */
/* Phase 2b-iii slab allocator, generational (slab storage model).
 * Same lists, same deinit -- only the backing store changes from
 * one-malloc-per-object to carved pages with free lists. Objects larger
 * than the biggest class still use malloc (large path, always treated as
 * old). Freelist links overlay BenderGCObject.data.next: free slots are
 * not live objects. Pages are 16 KiB aligned so any slot address maps to
 * its page header with a mask; a live-page directory (binary searched)
 * plus a magic + bounds check keeps the mapping exact (never confused by
 * malloc'd blocks, never reads unmapped memory). All growth uses raw
 * allocation so slab paths never trigger the collections they serve.
 * Per-VM state (see BenderVM.slab_state).
 *
 * Generations are page-granular: a page's age counts survived minors
 * (0-1 young, 2+ old). This is sound with pointer-identity reasoning --
 * objects never move, and young allocations only ever land on young
 * pages, so a young page's occupants are all young (an old object on a
 * young page would have had to move there). The reverse is not true
 * (old pages freely hold young garbage until swept), which is why sweep
 * distinguishes per object, not per page. See bender_gc_barrier and
 * bender_collect_minor. */

/* Page size is a power of two so bender_slab_page_for can mask a pointer down
 * to its page base.
 *
 * 4 KB (one OS page): a fresh process costs ~4 KB of slab instead of ~16 KB,
 * which is what makes the per-process memory target (~10 KB) reachable.
 *
 * This is safe again because the page table is now a hash set rather than a
 * sorted array. Shrinking the page multiplies the number of pages for the
 * same live bytes; against a sorted array that deepened every lookup and
 * every claim, and a real game degraded to 24 FPS with 134 stalled frames.
 * With O(1) lookup the page size is purely a memory trade, so the small page
 * is the right default. Do not reintroduce an ordered structure here without
 * re-measuring that game. */
#define BENDER_SLAB_PAGE_SIZE 4096
#define BENDER_SLAB_PAGE_MASK (~(uintptr_t)0xFFF)
#define BENDER_SLAB_MAGIC 0x4A534C41u
#define BENDER_SLAB_OLD_AGE 2

/* Bytes of young allocation between minor collections. */
#define BENDER_MINOR_THRESHOLD (256 * 1024)

static const size_t bender_slab_classes[BENDER_SLAB_NCLASSES] =
    {32, 64, 96, 128, 192, 256, 384, 512};

/* Fiber nursery cursors pair with the class table above. */
_Static_assert(BENDER_SLAB_NCLASSES == 8,
    "nursery_cur literal in BenderFiber must match class count");

/* Erlang-parity messaging fix: one per-owner bucket in the xfb
 * owner-bucketing side-structure (see BenderSlabState.xfb_buckets).
 * Chained on hash collision (owner % BENDER_XFB_NBUCKETS); a
 * fixed-size table is adequate here (lookup is O(chain length), and
 * with BENDER_XFB_NBUCKETS this large, chains stay short even at
 * hundreds of thousands of live owners). */
#define BENDER_XFB_NBUCKETS 4096
struct BenderXfbBucketNode {
    uint32_t owner;
    Bender *data;
    size_t count;
    size_t cap;
    struct BenderXfbBucketNode *next;
};

typedef struct BenderSlabPage {
    struct BenderSlabPage *next;
    uint32_t magic;
    int32_t class_index;
    int32_t age;
    int32_t dirty;
    int32_t bump;
    /* Step 1 (process heaps): owner id of the claiming fiber (first-
     * claimer wins; recycled slots keep it). 0 means shared/host
     * (allocated with no running fiber). Numeric (never a pointer), so
     * fiber death can never dangle it. Approximate once bump pages are
     * shared — per-fiber cur pages (slice 1.2) make young pages
     * pure-owned. Read-only until then. */
    uint32_t owner;
    /* Step 1.2: set when a recycled slot is reused across owners. Mixed
     * pages read as shared (owner 0) everywhere: accounting lumps them
     * with shared, the barrier records their edges. Hot loops reusing
     * their own slots keep pages pure. */
    int32_t mixed;
    /* P3.3: set when the claiming fiber is strict (spawn-isolated).
     * Recycled slots inherit it with the page (keeps header bits and
     * page owners in agreement — sweep filters by owner, the barrier
     * by header bit). */
    int32_t isolated;
} BenderSlabPage;

#define BENDER_SLAB_USABLE (BENDER_SLAB_PAGE_SIZE - sizeof(BenderSlabPage))
#define BENDER_SLAB_SLOTS(p) ((void *)((char *)(p) + sizeof(BenderSlabPage)))

typedef struct {
    BenderSlabPage *pages[BENDER_SLAB_NCLASSES];
    BenderSlabPage *cur[BENDER_SLAB_NCLASSES];
    BenderGCObject *young_free[BENDER_SLAB_NCLASSES];
    BenderGCObject *old_free[BENDER_SLAB_NCLASSES];
    BenderGCObject *strict_free[BENDER_SLAB_NCLASSES];
    /* P3.3b segregated freelists: young slots on pure-strict pages
     * file here (never in young_free), so strict fibers reuse strict
     * slots (precise bits/owners) and legacy fibers never inherit
     * strictness through recycling. Non-strict fibers never pop this
     * list; strict fibers never pop young_free (they bump fresh pure
     * pages instead). Survives majors (merge only touches old_free);
     * dies at clear with the rest. */
    /* Sorted directory of live page bases for exact membership: the
     * mask+magic probe below must never read unmapped memory, and not
     * every candidate pointer lives on a slab page (malloc large path,
     * foreign memory). Pages are added rarely (one per 16 KiB of same
     * class) and freed en-masse, so a sorted array with binary search
     * is cheap and exact. Raw allocation (never triggers collections). */
    BenderSlabPage **dir;
    size_t dir_count;
    size_t dir_cap;
    /* Step 1.2 cross-fiber remembered set: containers whose stores
     * crossed owner ids (see bender_gc_barrier). Plain Bender values
     * (rooted via mark_roots); duplicates tolerated, pruned in 1.3.
     * Capped: past the cap recording stops (stuck=1) rather than
     * growing without bound — missing edges only matter once
     * per-fiber minors consume the set (1.3), so failing open here is
     * today's behavior exactly. */
    Bender *xfb;
    size_t xfb_count;
    size_t xfb_cap;
    int xfb_stuck;
    /* Erlang-parity messaging fix, final form (docs/internals/
     * proc-lanes.md): true owner-bucketing at INSERT time, replacing
     * an earlier sorted-index attempt that only amortized correctly
     * when scans were rare between rebuilds -- our workload scans
     * (one fiber minor) on almost every insert, so that approach's
     * unsorted-tail scan cost stayed quadratic in practice. This is
     * a PARALLEL, additive side-structure: the main xfb array above,
     * its insertion order, and bender_slab_owner_stats' rollback
     * truncation are completely untouched. Every bucket append
     * happens at the SAME single insert site as the main array (the
     * owner, own_o, is already computed there -- free to reuse, no
     * extra lookup). Confirmed safe to key by owner at insert time:
     * bender_slab_obj_owner only ever transitions owner=K -> mixed/0,
     * and a mixed-page object is already immune to any single-
     * owner's scoped sweep regardless of marking (traced
     * bender_sweep_list's own filter), so a stale bucket entry after
     * that transition is harmless -- same as any other "floats until
     * major" over-conservative case already standard in this file.
     * No rollback is implemented for buckets (bender_slab_owner_stats'
     * self-edges may leave a few harmless stray entries pointing at
     * real, live objects -- over-marking, never a premature free);
     * buckets ARE cleared at major-reset and VM clear, same as xfb
     * itself, for long-run hygiene. */
    struct BenderXfbBucketNode **xfb_buckets;
    /* Step 1.3 live-fiber census (all fibers with live memory,
     * reachable or not): per-fiber minors mark every census fiber so
     * no unreaped referrer can dangle (an unreached foreign-old fiber
     * holding target-young would otherwise). Appended at birth
     * (alloc + unmarshal), swap-removed at free, reset at clear.
     * Over-marking (dead fibers) only floats garbage until major. */
    BenderFiber **fibers;
    size_t fibers_count;
    size_t fibers_cap;
} BenderSlabState;

/* Erlang-parity messaging fix: free every chain node + its data array
 * in the owner-bucket table, then the table itself. Used at both VM
 * clear and major-reset (buckets carry no rollback semantics of
 * their own, see the BenderSlabState.xfb_buckets comment -- safe to
 * drop entirely at either point, same as xfb_count=0 is). */
static void bender_xfb_bucket_append(BenderSlabState *ss, uint32_t owner, Bender cobj);

static void bender_xfb_buckets_free(BenderSlabState *ss) {
    if (NULL == ss->xfb_buckets) return;
    for (size_t b = 0; b < BENDER_XFB_NBUCKETS; b++) {
        struct BenderXfbBucketNode *node = ss->xfb_buckets[b];
        while (NULL != node) {
            struct BenderXfbBucketNode *next = node->next;
            bender_free(node->data);
            bender_free(node);
            node = next;
        }
    }
    bender_free(ss->xfb_buckets);
    ss->xfb_buckets = NULL;
}

static BenderSlabState *bender_slab_state(void) {
    BenderSlabState *ss = (BenderSlabState *) bender_vm.slab_state;
    if (NULL == ss) {
        ss = bender_malloc(sizeof(BenderSlabState));
        if (NULL == ss) {
            BENDER_OUT_OF_MEMORY;
        }
        memset(ss, 0, sizeof(BenderSlabState));
        bender_vm.slab_state = ss;
    }
    return ss;
}

/* Size class for a total object size, or -1 for the malloc large path. */
static int bender_slab_class_for(size_t size) {
    for (int i = 0; i < BENDER_SLAB_NCLASSES; i++) {
        if (size <= bender_slab_classes[i]) return i;
    }
    return -1;
}

static void *bender_slab_page_alloc(void) {
#ifdef BENDER_WINDOWS
    return _aligned_malloc(BENDER_SLAB_PAGE_SIZE, BENDER_SLAB_PAGE_SIZE);
#else
    void *p = NULL;
    if (posix_memalign(&p, BENDER_SLAB_PAGE_SIZE, BENDER_SLAB_PAGE_SIZE) != 0) {
        return NULL;
    }
    return p;
#endif
}

static void bender_slab_page_free(void *page) {
#ifdef BENDER_WINDOWS
    _aligned_free(page);
#else
    bender_free(page);
#endif
}

/* Page owning ptr, or NULL (malloc-backed large object, a stack object,
 * or foreign memory -- all treated as old). Membership is established by
 * an open-addressed hash probe over the live-page table first, so the
 * header probe below only ever reads our own mapped pages; the magic +
 * bounds checks then keep the mapping exact.
 *
 * This table was a SORTED ARRAY with binary search until the 4 KB page
 * experiment exposed why that is the wrong shape. Page count scales with
 * heap size / page size, so a smaller page silently multiplied the cost
 * of EVERY object-leaf query -- owner, age, and the write barrier -- by
 * deepening the search, and every claim paid an O(n) insertion memmove.
 * A game holding a few hundred MB therefore ran progressively slower the
 * longer it played, which read as the GC "getting stuck".
 *
 * The table is now a grow-only open-addressed pointer set: O(1) expected
 * lookup and insert regardless of heap size, so page size is once again
 * purely a memory trade and not a speed one. Growth is amortized (capacity
 * doubles, and a rehash is a linear pass with no free/realloc per entry).
 * Pages are never removed individually -- they are released en-masse at
 * clear -- so no tombstones or deletion path are needed; an empty slot is
 * simply NULL. */
#define BENDER_SLAB_DIR_EMPTY ((BenderSlabPage *) NULL)
#define BENDER_SLAB_DIR_MINCAP 64u

/* Multiplicative mix: page bases are page-aligned, so the low bits are
 * constant and must be discarded before they dominate the bucket index. */
static size_t bender_slab_dir_hash(uintptr_t base) {
    uint64_t h = (uint64_t) (base >> 12);
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdULL;
    h ^= h >> 33;
    h *= 0xc4ceb9fe1a85ec53ULL;
    h ^= h >> 33;
    return (size_t) h;
}

static BenderSlabPage *bender_slab_dir_lookup(BenderSlabState *ss, uintptr_t base) {
    size_t mask = (size_t) ss->dir_cap - 1;
    size_t i = bender_slab_dir_hash(base) & mask;
    for (;;) {
        BenderSlabPage *slot = ss->dir[i];
        if (NULL == slot) return NULL;                 /* empty: absent */
        if ((uintptr_t) slot == base) return slot;
        i = (i + 1) & mask;                            /* linear probe */
    }
}

/* Rehash into ncap (a power of two >= dir_count). Returns 0 on OOM, in
 * which case the old table is untouched. Raw allocation only. */
static int bender_slab_dir_grow(BenderSlabState *ss, size_t ncap) {
    BenderSlabPage **ntab = bender_calloc(ncap, sizeof(BenderSlabPage *));
    size_t i;
    if (NULL == ntab) return 0;
    for (i = 0; i < ss->dir_cap; i++) {
        BenderSlabPage *page = ss->dir[i];
        if (NULL == page) continue;
        {
            size_t mask = ncap - 1;
            size_t j = bender_slab_dir_hash((uintptr_t) page) & mask;
            while (NULL != ntab[j]) j = (j + 1) & mask;
            ntab[j] = page;
        }
    }
    bender_free(ss->dir);
    ss->dir = ntab;
    ss->dir_cap = ncap;
    return 1;
}

/* Insert a page into the table. Returns 0 on OOM (caller must drop the
 * page). Raw allocation only. */
static int bender_slab_dir_insert(BenderSlabPage *page) {
    BenderSlabState *ss = (BenderSlabState *) bender_vm.slab_state;
    uintptr_t base = (uintptr_t) page;
    /* Keep load factor <= 1/2 so probe chains stay short. */
    if (0 == ss->dir_cap) {
        if (!bender_slab_dir_grow(ss, BENDER_SLAB_DIR_MINCAP)) return 0;
    } else if ((ss->dir_count + 1) * 2 > ss->dir_cap) {
        if (!bender_slab_dir_grow(ss, ss->dir_cap * 2)) return 0;
    }
    {
        size_t mask = (size_t) ss->dir_cap - 1;
        size_t i = bender_slab_dir_hash(base) & mask;
        while (NULL != ss->dir[i]) {
            /* Identical insert would only happen on a double-claim bug;
             * treat it as success so the caller keeps one entry. */
            if ((uintptr_t) ss->dir[i] == base) return 1;
            i = (i + 1) & mask;
        }
        ss->dir[i] = page;
    }
    ss->dir_count++;
    return 1;
}

static BenderSlabPage *bender_slab_page_for(void *ptr) {
    BenderSlabState *ss = (BenderSlabState *) bender_vm.slab_state;
    BenderSlabPage *page;
    uintptr_t base;
    if (NULL == ss || 0 == ss->dir_count) return NULL;
    base = (uintptr_t) ptr & BENDER_SLAB_PAGE_MASK;
    page = bender_slab_dir_lookup(ss, base);
    if (NULL == page) return NULL;
    if (page->magic != BENDER_SLAB_MAGIC) return NULL;
    if (page->class_index < 0 || page->class_index >= BENDER_SLAB_NCLASSES) return NULL;
    if (page->bump < 0 || (size_t) page->bump > (size_t) BENDER_SLAB_USABLE) return NULL;
    if ((char *) ptr < (char *) BENDER_SLAB_SLOTS(page)) return NULL;
    if ((char *) ptr >= (char *) base + BENDER_SLAB_PAGE_SIZE) return NULL;
    return page;
}

/* Object age for GC purposes: young pages hold only young objects (see
 * the header comment); anything else counts as old. */
static int bender_slab_obj_old(void *ptr) {
    BenderSlabPage *page = bender_slab_page_for(ptr);
    if (NULL == page) return 1;
    return page->age >= BENDER_SLAB_OLD_AGE;
}

/* Debug (BENDER_TRACE_STRICT): dump a copied entry's funcenvs and the
 * slab-page ownership of table upvalues — where did the child's
 * captures land? */
void bender_strict_debug_envs(BenderFunction *copy) {
    for (int ei = 0; ei < copy->def->environments_length; ei++) {
        BenderFuncEnv *fe = copy->envs[ei];
        bender_eprintf("[copy-env] env#%d len=%d offset=%d",
                ei, fe->length, fe->offset);
        if (fe->offset <= 0 && NULL != fe->as.values) {
            for (int vi = 0; vi < fe->length && vi < 12; vi++) {
                Bender v = fe->as.values[vi];
                BenderType vt = bender_type(v);
                const char *tn = bender_type_names[vt];
                if (vt == BENDER_TABLE) {
                    BenderSlabPage *tp = bender_slab_page_for(
                            bender_unwrap_table(v));
                    BenderTable *tt = bender_unwrap_table(v);
                    bender_eprintf(" [v%d=table(count=%d proto=%p "
                            "page(owner=%d isolated=%d))]", vi,
                            tt->count, (void *) tt->proto,
                            (NULL != tp) ? (int) tp->owner : -1,
                            (NULL != tp) ? tp->isolated : -1);
                } else {
                    bender_eprintf(" [v%d=%s]", vi, tn);
                }
            }
        }
        bender_eprintf("\n");
    }
}

/* Step 1.3: owning fiber id for sweep filtering (0 = shared: mixed
 * pages, unowned pages, and malloc-large objects all read shared and
 * are spared by per-fiber sweeps). */
static uint32_t bender_slab_obj_owner(void *ptr) {
    BenderSlabPage *page = bender_slab_page_for(ptr);
    if (NULL == page) return 0;
    if (page->mixed) return 0;
    return page->owner;
}

static void *bender_slab_alloc(int class_index) {
    BenderSlabState *ss = bender_slab_state();
    size_t slot = bender_slab_classes[class_index];
    /* Step 1.2 requester: the running fiber (possibly NULL on host)
     * pays nursery bytes and owns bump pages; recycled slots keep
     * their page and mark it mixed on cross-owner reuse. P3.5c: a
     * spawn-construction override redirects attribution to the child
     * being built (owner id, strict bit, nursery, filing). */
    BenderFiber *req = (NULL != bender_vm.gc_alloc_ctx)
        ? bender_vm.gc_alloc_ctx : bender_vm.fiber;
    uint32_t reqid = (NULL != req) ? req->gc_owner : 0;
    /* P3.3b: freelists segregate by strictness (see release). The
     * pop logic below is unchanged (young/old skip + mixed-marking
     * still apply within each list). */
    BenderGCObject **freep = (NULL != req && req->heap_strict) ?
        &ss->strict_free[class_index] : &ss->young_free[class_index];
    /* Pop a young slot, skipping (to the old list) any slot whose page
     * has aged out since the slot was freed. Self-correcting: every pop
     * either uses or re-files the head, so it always terminates. */
    for (;;) {
        BenderGCObject *slot_obj = *freep;
        if (NULL == slot_obj) break;
        *freep = slot_obj->data.next;
        BenderSlabPage *page = bender_slab_page_for(slot_obj);
        if (NULL != page && page->age < BENDER_SLAB_OLD_AGE) {
            if (0 != page->owner && page->owner != reqid) page->mixed = 1;
            if (NULL != req) req->gc_nursery_bytes += slot;
            if (bender_vm.trace_strict && NULL != req &&
                    req->heap_strict) {
                bender_eprintf("[slab-pop-strict] req_owner=%d "
                        "class=%d page(owner=%d mixed=%d isolated=%d)\n",
                        (int) reqid, class_index, (int) page->owner,
                        page->mixed, page->isolated);
            }
            /* TEMPORARY 2b-iii bisect: zero recycled slots to test for
             * uninitialized-field reads. */
            memset(slot_obj, 0, slot);
            return slot_obj;
        }
        slot_obj->data.next = (BenderGCObject *) ss->old_free[class_index];
        ss->old_free[class_index] = slot_obj;
    }
    /* Bump path.
     *
     * Historically every fiber drew from its OWN cur page so young pages
     * stayed pure-owned, which keeps a scoped sweep exact.

     * That costs a full page per fiber even when the fiber allocates one
     * small object: measured 2.20 pages/proc for a 5000-proc burst, with
     * class c1 (64 B, 64 slots/page) and c5 (256 B, 16 slots/page) each
     * claiming ~0.92 pages per proc -- i.e. pages ~1/64 full.
     *
     * A SHORT-LIVED allocation context (a proc being constructed, which is
     * exactly gc_alloc_ctx) does not need purity: it allocates a couple of
     * small objects and then either runs or dies. Route those through the
     * SHARED cur so a page is filled by many constructing procs; a page that
     * receives more than one owner is marked `mixed` on reuse, and mixed pages
     * read as owner 0 everywhere, which is sound because owner 0 means
     * "shared": they float until a global collection instead of a scoped one.
     * Steady-state fibers (allocating during real execution) keep their own
     * cur page exactly as before. */
    /* STRICT (spawn-isolated) construction must keep its private cursor: the
     * export/escape rule depends on the child's stores landing on pages only
     * it owns, and sharing them makes an intra-heap store look like a
     * cross-heap escape (measured: suite-proc's P3.3 export tests fail). So
     * the shared cursor applies ONLY to ordinary construction. */
    int constructing = (NULL != req && req == bender_vm.gc_alloc_ctx &&
                        !req->heap_strict);
    BenderSlabPage **curp = (NULL != req && !constructing) ?
        &req->nursery_cur[class_index] : &ss->cur[class_index];
    BenderSlabPage *page = *curp;
    if (NULL == page || page->age >= BENDER_SLAB_OLD_AGE ||
            (size_t) page->bump + slot > (size_t) BENDER_SLAB_USABLE) {
        page = bender_slab_page_alloc();
        if (NULL == page) {
            return NULL;
        }
        page->next = ss->pages[class_index];
        page->magic = BENDER_SLAB_MAGIC;
        page->class_index = (int32_t) class_index;
        page->age = 0;
        page->dirty = 0;
        page->bump = 0;
        /* Step 1: claiming fiber's owner id (0 when host-allocated).
         * Fresh pages start pure. */
        page->owner = reqid;
        page->mixed = 0;
        /* P3.3: claiming fiber's strict flag rides with the owner. */
        page->isolated = (NULL != req && req->heap_strict) ? 1 : 0;
        if (!bender_slab_dir_insert(page)) {
            bender_slab_page_free(page);
            return NULL;
        }
        ss->pages[class_index] = page;
        *curp = page;
    }
    void *out = (char *) BENDER_SLAB_SLOTS(page) + page->bump;
    page->bump += (int32_t) slot;
    if (NULL != req) req->gc_nursery_bytes += slot;
    /* P3.5c: strict-owned young is managed per-proc (nursery pressure
     * routes to per-proc collections; deaths float bounded by majors).
     * Counting it toward the global young threshold would fire
     * O(world) global minors on proc churn (the spawn scaler).
     * Spawn-construction attribution (override, never the running
     * fiber) is likewise per-proc managed. Shared/host bump still
     * counts. */
    if (NULL == req || (req == bender_vm.fiber && NULL == req->proc)) {
        /* Phase 1.1: every proc fiber's churn is per-fiber-scoped
         * (maybe_collect routes its nursery pressure to
         * collect_fiber_minor, which sweeps only its own lists);
         * counting it toward the global young threshold fires
         * O(world) global minors on proc churn — the spawn-scaler
         * grind (300k-string churn ran 90s; every global minor
         * sweeps every list). Strict procs were already excluded
         * (P3.5c); non-strict proc fibers join them. Shared/host and
         * plain-fiber bump still counts. */
        bender_vm.gc_young_bytes += slot;
    }
    return out;
}

/* Release a whole GC object: slab slot back to the free list matching
 * its page's current age, or plain free for large-path objects. All
 * whole-object frees route here. */
void bender_gc_release(BenderGCObject *mem) {
    BenderSlabState *ss = (BenderSlabState *) bender_vm.slab_state;
    if (NULL != ss) {
        BenderSlabPage *page = bender_slab_page_for(mem);
        if (NULL != page) {
            int cls = page->class_index;
            if (page->age < BENDER_SLAB_OLD_AGE) {
                /* P3.3b: pure-strict young slots segregate (see state). */
                if (!page->mixed && page->isolated && 0 != page->owner) {
                    mem->data.next = (BenderGCObject *) ss->strict_free[cls];
                    ss->strict_free[cls] = mem;
                } else {
                    mem->data.next = (BenderGCObject *) ss->young_free[cls];
                    ss->young_free[cls] = mem;
                }
            } else {
                mem->data.next = (BenderGCObject *) ss->old_free[cls];
                ss->old_free[cls] = mem;
            }
            return;
        }
    }
    bender_free(mem);
}


/* Free all slab pages (used at clear; freelists die with them). */
static void bender_slab_free_pages(void) {
    BenderSlabState *ss = (BenderSlabState *) bender_vm.slab_state;
    if (NULL == ss) return;
    for (int i = 0; i < BENDER_SLAB_NCLASSES; i++) {
        BenderSlabPage *page = ss->pages[i];
        while (NULL != page) {
            BenderSlabPage *next = page->next;
            bender_slab_page_free(page);
            page = next;
        }
        ss->pages[i] = NULL;
        ss->cur[i] = NULL;
        ss->young_free[i] = NULL;
        ss->old_free[i] = NULL;
        ss->strict_free[i] = NULL;
    }
    bender_free(ss->dir);
    ss->dir = NULL;
    ss->dir_count = 0;
    ss->dir_cap = 0;
    /* Step 1.2: remembered set dies with the pages. */
    bender_free(ss->xfb);
    ss->xfb = NULL;
    ss->xfb_count = 0;
    ss->xfb_cap = 0;
    ss->xfb_stuck = 0;
    bender_xfb_buckets_free(ss);
    /* Step 1.3: census resets with the pages (fibers are gone too). */
    bender_free(ss->fibers);
    ss->fibers = NULL;
    ss->fibers_count = 0;
    ss->fibers_cap = 0;
    bender_vm.gc_young_bytes = 0;
}

/* Merge old freelists back into young ones (used at major collections,
 * when all ages reset below). O(chains), no searching. */
static void bender_slab_merge_free(void) {
    BenderSlabState *ss = (BenderSlabState *) bender_vm.slab_state;
    if (NULL == ss) return;
    for (int i = 0; i < BENDER_SLAB_NCLASSES; i++) {
        if (NULL == ss->old_free[i]) continue;
        BenderGCObject *tail = ss->old_free[i];
        while (NULL != tail->data.next) {
            tail = tail->data.next;
        }
        tail->data.next = (BenderGCObject *) ss->young_free[i];
        ss->young_free[i] = ss->old_free[i];
        ss->old_free[i] = NULL;
    }
}

/* Reset generational state after a full sweep: all pages back to young
 * (conservative restart -- the next minors re-segregate), dirty flags
 * clear, freelists merged, nursery counter restarted. O(pages). */
static void bender_slab_major_reset(void) {
    BenderSlabState *ss = (BenderSlabState *) bender_vm.slab_state;
    if (NULL == ss) return;
    for (int i = 0; i < BENDER_SLAB_NCLASSES; i++) {
        for (BenderSlabPage *p = ss->pages[i]; NULL != p; p = p->next) {
            p->age = 0;
            p->dirty = 0;
        }
    }
    bender_slab_merge_free();
    bender_vm.gc_young_bytes = 0;
    /* P3.1 cross-fiber epoch: a major saw every edge, so the set
     * restarts empty (stale entries would pin dead containers as
     * roots forever; stuck clears too — capacity is per-epoch now).
     * Behavior-neutral today (marking is complete without xfb; the
     * census carries fiber minors). Load-bearing later: a per-proc
     * collection younger than the last major sees complete edges. */
    ss->xfb_count = 0;
    ss->xfb_stuck = 0;
    /* Buckets reset with the main set -- a major saw every edge, so
     * both restart empty (see bender_xfb_buckets_free's comment). */
    bender_xfb_buckets_free(ss);
}
void bender_sweep_weak_drop(void) {
    /* Weak-ref dropping runs at majors only. In minors the mark is
     * partial (old heap unscanned), so liveness verdicts would be wrong;
     * weak tables are instead traversed conservatively via the normal
     * mark exemption for weak types, and clearing waits for a major. */
    if (bender_vm.gc_minor_phase) return;
    bender_sweep_weak_drop_list(bender_vm.weak_blocks);
    /* P3.5b Slice A: weak tables now also live in per-fiber lists. */
    {
        BenderSlabState *ss = (BenderSlabState *) bender_vm.slab_state;
        if (NULL != ss) {
            size_t wi;
            for (wi = 0; wi < ss->fibers_count; wi++) {
                BenderFiber *f = ss->fibers[wi];
                bender_sweep_weak_drop_list((BenderGCObject *) f->heap_lists[2]);
            }
        }
    }
}

static void bender_sweep_weak_drop_list(BenderGCObject *current) {
    BenderGCObject *next;
    while (NULL != current) {
        next = current->data.next;
        if ((current->flags & BENDER_MEM_DISABLED) || bender_gc_reachable(current)) {
            /* Check for dead references. The epoch compare replaces the
             * old sticky REACHABLE gate — under the epoch mark no
             * sticky bit is ever set, and the old gate made the entire
             * weak-drop dead code (suite-ev2 count = 100). */
            enum BenderMemoryType type = bender_gc_type(current);
            if (type == BENDER_MEMORY_ARRAY_WEAK) {
                BenderArray *array = (BenderArray *) current;
                for (uint32_t i = 0; i < (uint32_t) array->count; i++) {
                    if (!bender_check_liveref(array->data[i])) {
                        array->data[i] = bender_wrap_nil();
                    }
                }
            } else {
                BenderTable *table = (BenderTable *) current;
                int check_values = (type == BENDER_MEMORY_TABLE_WEAKV) || (type == BENDER_MEMORY_TABLE_WEAKKV);
                int check_keys = (type == BENDER_MEMORY_TABLE_WEAKK) || (type == BENDER_MEMORY_TABLE_WEAKKV);
                BenderKV *end = table->data + table->capacity;
                BenderKV *kvs = table->data;
                while (kvs < end) {
                    int drop = 0;
                    if (check_keys && !bender_check_liveref(kvs->key)) drop = 1;
                    if (check_values && !bender_check_liveref(kvs->value)) drop = 1;
                    if (drop) {
                        /* Inlined from bender_table_remove without search */
                        table->count--;
                        table->deleted++;
                        kvs->key = bender_wrap_nil();
                        kvs->value = bender_wrap_false();
                    }
                    kvs++;
                }
            }
        }
        current = next;
    }
}

/* Sweep all three heaps' blocks (minor-aware). In a minor collection
 * only young dead blocks are freed; old blocks wait for a major.
 * Weak-ref dropping is separate (majors only); the ev threaded section
 * below also runs at majors only.
 * P3.5b Slice A: one heap kind across globals + every census fiber's
 * list (the filing rule guarantees fiber lists hold no fibers, so
 * freeing here never mutates the census mid-walk). Fiber lists sweep
 * before globals; fibers dying in the globals sweep park leftovers
 * for the post-sweep drain (marked survivors float one cycle —
 * sound, never dangling). Same free-set as the globals-only shape. */
/* Clear stale REACHABLE bits across every heap list. Upstream's
 * single-heap sweep guarantees "bits start clear at each cycle"
 * because every sweep clears every survivor. This fork's per-fiber
 * heap lists broke that: a scoped collect (collect_process) marks
 * objects living on OTHER fibers' lists (e.g. a proc abstract born
 * under the parent's attribution) and then sweeps only its own
 * lists, leaving those foreign bits set. Any later mark then
 * early-returns on the stale bit and skips the type's gcmark — so
 * dependents (the proc's mailbox) are never re-marked and the next
 * full sweep frees them while live code still holds the proc. Full
 * collects therefore pre-clear every bit before marking, restoring
 * the invariant explicitly. Cost: one O(heap) pass, same order as
 * the full collect itself. */
static void bender_clear_all_reachable_bits(void) {
    BenderSlabState *ss = (BenderSlabState *) bender_vm.slab_state;
    BenderGCObject *cur;
    size_t fi;
    int kind;
    if (NULL != ss) {
        for (fi = 0; fi < ss->fibers_count; fi++) {
            BenderFiber *f = ss->fibers[fi];
            for (kind = 0; kind < 3; kind++) {
                for (cur = f->heap_lists[kind]; NULL != cur; cur = cur->data.next)
                    cur->flags &= ~BENDER_MEM_EPOCH_MASK;
            }
        }
    }
    BenderGCObject *globals[3];
    globals[0] = bender_vm.blocks;
    globals[1] = (BenderGCObject *) bender_vm.string_blocks;
    globals[2] = bender_vm.weak_blocks;
    for (kind = 0; kind < 3; kind++) {
        for (cur = globals[kind]; NULL != cur; cur = cur->data.next)
            cur->flags &= ~BENDER_MEM_EPOCH_MASK;
    }
}

static void bender_sweep_kind(int kind, int is_minor, uint32_t only_owner,
                             int walk_fibers) {
    BenderGCObject **ghead;
    BenderSlabState *ss;
    size_t fi;
    if (kind == 1) {
        ghead = (BenderGCObject **) &bender_vm.string_blocks;
    } else if (kind == 0) {
        ghead = (BenderGCObject **) &bender_vm.blocks;
    } else {
        ghead = (BenderGCObject **) &bender_vm.weak_blocks;
    }
    /* Phase 1 density: the GLOBAL MINOR no longer walks every
     * registered fiber's lists. With 1M procs that loop is the
     * quadratic engine (measured: the second 100k spawns cost 3.2x
     * the first). Each fiber's own minor handles its own pressure;
     * fibers get one final minor at completion (ev.c), so a global
     * minor now sweeps only the globals. Parked-forever fibers keep
     * their young (bounded by their real working set). The FULL
     * collect (bender_sweep) still walks every fiber. */
    ss = (BenderSlabState *) bender_vm.slab_state;
    if (NULL != ss && walk_fibers) {
        for (fi = 0; fi < ss->fibers_count; fi++) {
            BenderFiber *f = ss->fibers[fi];
            bender_sweep_list((BenderGCObject **) &f->heap_lists[kind],
                             kind, is_minor, only_owner);
        }
    }
    bender_sweep_list(ghead, kind, is_minor, only_owner);
}

/* P3.5b Slice A heap verifier (BENDER_HEAP_VERIFY=1, dev only): every
 * listed object counted exactly once across globals + all fiber
 * lists, total == block_count; no cycles (bounded walk); fiber
 * objects only in globals (filing invariant). O(world); runs after
 * sweeps when enabled. */
/* Debug aid for the page table. A table bug is FAIL-SAFE, not corrupting:
 * an absent entry makes the page's objects look old, so they are retained
 * rather than freed-while-live. Correctness tests therefore cannot catch a
 * lost entry -- but the leak is real (live slots are never recycled) and
 * shows up as RSS ratcheting across churn. This audit is the direct check:
 * every page on every class list must be findable through the table, and
 * every occupied slot must resolve back to its own page. Only runs under
 * BENDER_HEAP_VERIFY, so it costs nothing in normal builds. */
static void bender_slab_dir_verify(const char *where) {
    BenderSlabState *ss = (BenderSlabState *) bender_vm.slab_state;
    size_t listed = 0, c;
    int cls;
    if (NULL == ss) return;
    for (cls = 0; cls < BENDER_SLAB_NCLASSES; cls++) {
        BenderSlabPage *page;
        for (page = ss->pages[cls]; NULL != page; page = page->next) {
            listed++;
            if (bender_slab_dir_lookup(ss, (uintptr_t) page) != page) {
                fprintf(stderr,
                        "slab-dir(%s): page %p class %d NOT findable\n",
                        where, (void *) page, cls);
                abort();
            }
        }
    }
    if (listed != ss->dir_count) {
        fprintf(stderr, "slab-dir(%s): %zu pages listed, %zu table entries\n",
                where, listed, ss->dir_count);
        abort();
    }
    for (c = 0; c < ss->dir_cap; c++) {
        BenderSlabPage *page = ss->dir[c];
        if (NULL == page) continue;
        if (bender_slab_dir_lookup(ss, (uintptr_t) page) != page) {
            fprintf(stderr, "slab-dir(%s): unreachable entry %p\n",
                    where, (void *) page);
            abort();
        }
    }
}

static void bender_heap_verify(const char *where) {
    static int verify = -1;
    size_t total = 0;
    size_t kindtot[3] = {0, 0, 0};
    int kind;
    if (-1 == verify) verify = (NULL != getenv("BENDER_HEAP_VERIFY")) ? 1 : 0;
    if (!verify) return;
    bender_slab_dir_verify(where);
    for (kind = 0; kind < 3; kind++) {
        BenderGCObject *ghead;
        BenderSlabState *ss = (BenderSlabState *) bender_vm.slab_state;
        size_t fi;
        if (kind == 1) ghead = (BenderGCObject *) bender_vm.string_blocks;
        else if (kind == 0) ghead = (BenderGCObject *) bender_vm.blocks;
        else ghead = (BenderGCObject *) bender_vm.weak_blocks;
        {
            size_t n = 0;
            BenderGCObject *cur;
            for (cur = ghead; NULL != cur; cur = cur->data.next) {
                if (++n > bender_vm.block_count + 1) {
                    fprintf(stderr, "heap-verify(%s): cycle in globals kind %d\n", where, kind);
                    abort();
                }
            }
            total += n;
            kindtot[kind] += n;
        }
        if (NULL != ss) {
            for (fi = 0; fi < ss->fibers_count; fi++) {
                BenderFiber *f = ss->fibers[fi];
                BenderGCObject *cur;
                size_t n = 0;
                for (cur = (BenderGCObject *) f->heap_lists[kind]; NULL != cur; cur = cur->data.next) {
                    if (++n > bender_vm.block_count + 1) {
                        fprintf(stderr, "heap-verify(%s): cycle in fiber %p kind %d\n",
                                where, (void *) f, kind);
                        abort();
                    }
                    if (bender_gc_type(cur) == BENDER_MEMORY_FIBER) {
                        fprintf(stderr, "heap-verify(%s): fiber object in per-fiber list\n", where);
                        abort();
                    }
                }
                total += n;
                kindtot[kind] += n;
            }
        }
    }
    if (total != bender_vm.block_count) {
        fprintf(stderr, "heap-verify(%s): listed %d != block_count %d (kinds %d %d %d)\n",
                where, (int) total, (int) bender_vm.block_count,
                (int) kindtot[0], (int) kindtot[1], (int) kindtot[2]);
        abort();
    }
}

/* P3.5b deferred drain: after all lists are walked, splice parked
 * death-migration chains into the globals (each chain walked once
 * for its tail). Parked chains hold marked survivors + old-unmarked
 * floated from dead fibers; both belong in the globals now. */
static void bender_sweep_drain_pending(void) {
    int k;
    for (k = 0; k < 3; k++) {
        BenderGCObject *fl = (BenderGCObject *) bender_vm.sweep_pending[k];
        if (NULL == fl) continue;
        bender_vm.sweep_pending[k] = NULL;
        {
            BenderGCObject *tail = fl;
            while (NULL != tail->data.next)
                tail = tail->data.next;
            if (k == 0) {
                tail->data.next = (BenderGCObject *) bender_vm.blocks;
                bender_vm.blocks = (void *) fl;
            } else if (k == 1) {
                tail->data.next = (BenderGCObject *) bender_vm.string_blocks;
                bender_vm.string_blocks = (void *) fl;
            } else {
                tail->data.next = (BenderGCObject *) bender_vm.weak_blocks;
                bender_vm.weak_blocks = (void *) fl;
            }
        }
    }
}

static void bender_sweep_heaps(int walk_fibers) {
bender_sweep_kind(2, bender_vm.gc_minor_phase, 0, walk_fibers);
    bender_sweep_kind(0, bender_vm.gc_minor_phase, 0, walk_fibers);
    bender_sweep_kind(1, bender_vm.gc_minor_phase, 0, walk_fibers);
    bender_sweep_drain_pending();
    bender_heap_verify("global");
}

/* Step 1.3: per-fiber sweep freeing only unmarked young blocks
 * owned by the target (P3.5b Slice B: the target's own three lists
 * only — O(own); globals + foreign lists untouched). Foreign young
 * unmarked blocks float until a global collection; foreign marked
 * blocks were already spared by the mark. All callers pass live
 * fibers (never freed mid-sweep here). */
static void bender_sweep_fiber_owned(BenderFiber *f) {
    /* Erlang-parity messaging fix, fourth mechanism (docs/internals/
     * proc-lanes.md): this wide path (walking the global fiber
     * census on every call -- see bender_sweep_kind's walk_fibers=1)
     * is now UNREACHABLE by design. The P3.5b-era "full narrowing
     * crashes boot image generation deterministically" warning was
     * re-tested directly against the exact historical scenario
     * (build/bender_boot's actual bootstrap image generation, not a
     * proxy) under full narrowing for every fiber, 3/3 runs, and
     * produced a byte-identical bender.c to the wide path every time.
     * All 60 test/suite-*.ben files, dual-target AOT across the
     * messaging-relevant ones, and suite-lanes' 640k-density join
     * (8.02s, matching the historical baseline exactly) also showed
     * zero behavioral difference. The real target workload
     * (bench/agents.ben at N=100000, which the wide path could not
     * even complete) finished cleanly under narrowing. Whatever
     * caused the historical crash was evidently fixed by other work
     * in the time since (this session's own marking-scope and
     * threaded-abstract fixes are plausible candidates, though not
     * confirmed which specific change). Kept as dead code (not
     * deleted) so a future regression can bisect to it quickly if
     * the historical failure mode ever resurfaces; see
     * bender_sweep_proc_owned below for the now-universal narrow
     * path every fiber actually takes. */
    uint32_t owner = f->gc_owner;
    (void) owner;
    bender_sweep_proc_owned(f);
    return;
#if 0
bender_sweep_kind(2, 1, owner, 1);
    bender_sweep_kind(0, 1, owner, 1);
    bender_sweep_kind(1, 1, owner, 1);
    bender_sweep_drain_pending();
    bender_heap_verify("owned");
#endif
}

/* P3.5b Slice B (strict only): O(own) — walk only the target's three
 * lists with the owner filter. Sound exactly where collect_process's
 * narrowed marking is sound (strict isolation: spawn-copy entry +
 * export panics + copy sends; foreign stacks hold no P-young, audited
 * by BENDER_PROC_VERIFY). Marking there is complete (P-roots + shared
 * C roots + remembered set), so unmarked-own is garbage; foreign
 * young in other lists floats until a global collection. Fresh pure
 * pages (spawn/churn steady state) free here; mixed/large slots read
 * owner 0 and float the same way. No fiber objects live here (filing
 * invariant), so the census never mutates mid-walk. */
static void bender_sweep_proc_owned(BenderFiber *f) {
    uint32_t owner = f->gc_owner;
    bender_sweep_list((BenderGCObject **) &f->heap_lists[2], 2, 1, owner);
    bender_sweep_list((BenderGCObject **) &f->heap_lists[0], 0, 1, owner);
    bender_sweep_list((BenderGCObject **) &f->heap_lists[1], 1, 1, owner);
    bender_sweep_drain_pending();
    bender_heap_verify("proc-owned");
}

static void bender_sweep_heaps_owned(BenderFiber *f) {
    bender_sweep_fiber_owned(f);
}


/* Step 1 introspection: per-owner slab page counts. Returns
 * @{ :pages total :unowned n :owned @{id count} } (pure numbers: fiber
 * death can never dangle them). Caller holds the result on the Bender
 * stack (rooted across the table builds below). */
BenderTable *bender_slab_owner_stats(void) {
    BenderSlabState *ss = bender_slab_state();
    /* P3.1: the observer must not perturb — table building below
     * stores interned keys (foreign-owned pages) and would record
     * cross-fiber edges for the introspection itself. The set is
     * append-only and order-free, so truncate to the entry count on
     * the way out (stuck flag untouched: cap pressure is real). */
    size_t xfb_entry = ss->xfb_count;
    BenderTable *out = bender_table(0);
    Bender out_j = bender_wrap_table(out);
    bender_gcroot(out_j);
    BenderTable *owned = bender_table(0);
    Bender owned_j = bender_wrap_table(owned);
    bender_gcroot(owned_j);
    int32_t total = 0;
    int32_t unowned = 0;
    int cls;
    for (cls = 0; cls < BENDER_SLAB_NCLASSES; cls++) {
        BenderSlabPage *p;
        for (p = ss->pages[cls]; NULL != p; p = p->next) {
            total++;
            if (0 == p->owner) {
                unowned++;
            } else {
                Bender k = bender_wrap_integer((int64_t) p->owner);
                Bender cur = bender_table_get(owned, k);
                int32_t n = bender_checktype(cur, BENDER_NIL) ? 0 : bender_unwrap_integer(cur);
                bender_table_put(owned, k, bender_wrap_integer(n + 1));
            }
        }
    }
    bender_table_put(out, bender_ckeywordv("pages"), bender_wrap_integer(total));
    bender_table_put(out, bender_ckeywordv("unowned"), bender_wrap_integer(unowned));
    bender_table_put(out, bender_ckeywordv("owned"), owned_j);
    /* Step 1.2: cross-fiber remembered edges pending (pruned in 1.3). */
    bender_table_put(out, bender_ckeywordv("remembered"),
                    bender_wrap_integer((int64_t) xfb_entry));
    bender_table_put(out, bender_ckeywordv("remember-stuck"),
                    bender_wrap_boolean(ss->xfb_stuck));
    /* P3.1 observer hygiene (see entry): drop this call's self-edges. */
    if (ss->xfb_count > xfb_entry) ss->xfb_count = xfb_entry;
    bender_gcunroot(owned_j);
    bender_gcunroot(out_j);
    return out;
}

/* Step 1.3 census helpers: add is cross-TU (fiber birth sites),
 * remove runs at sweep-free in this TU. */
void bender_census_add(BenderFiber *f) {
    BenderSlabState *ss = bender_slab_state();
    if (ss->fibers_count == ss->fibers_cap) {
        size_t ncap = ss->fibers_cap ? ss->fibers_cap * 2 : 16;
        BenderFiber **nf = bender_malloc(ncap * sizeof(BenderFiber *));
        if (NULL == nf) BENDER_OUT_OF_MEMORY;
        if (NULL != ss->fibers) {
            memcpy(nf, ss->fibers, ss->fibers_count * sizeof(BenderFiber *));
            bender_free(ss->fibers);
        }
        ss->fibers = nf;
        ss->fibers_cap = ncap;
    }
    /* Erlang-parity exit cost: record the fiber's own slot so removal
     * is O(1) (see census_idx in bender.h). */
    f->census_idx = ss->fibers_count;
    ss->fibers[ss->fibers_count++] = f;
}

/* O(1) swap-remove via the fiber's recorded index, with a fail-safe
 * linear-scan fallback. The scan this replaces was O(live fibers) per
 * fiber death, making mass exit of N processes O(N^2) on its own.
 *
 * The index is a HINT, never trusted as truth: it is validated against
 * the array before use, and ANY mismatch (stale index, fiber tracked in
 * a different slab state than the one being swept, never-added fiber)
 * falls back to the original exhaustive scan. Getting this wrong in the
 * unsafe direction would silently drop a LIVE fiber out of the GC root
 * set -- a use-after-free -- so the fallback is mandatory, not
 * defensive clutter. */
static void bender_census_remove(BenderFiber *f) {
    BenderSlabState *ss = (BenderSlabState *) bender_vm.slab_state;
    size_t i;
    if (NULL == ss) return;
    /* Fast path: the recorded slot must be in range AND actually hold
     * this fiber. Both checks are required -- a fiber censused on
     * another slab state can carry an index that is in-range here but
     * points at a different fiber. */
    if (f->census_idx < ss->fibers_count && ss->fibers[f->census_idx] == f) {
        size_t slot = f->census_idx;
        BenderFiber *moved = ss->fibers[--ss->fibers_count];
        ss->fibers[slot] = moved;
        /* The relocated fiber's own index must follow it, or its later
         * removal takes the slow path (correct, just slower). Skip when
         * f WAS the last element: moved == f, and f is leaving. */
        if (moved != f) moved->census_idx = slot;
        f->census_idx = BENDER_CENSUS_UNTRACKED;
        return;
    }
    for (i = 0; i < ss->fibers_count; i++) {
        if (ss->fibers[i] == f) {
            BenderFiber *moved = ss->fibers[--ss->fibers_count];
            ss->fibers[i] = moved;
            if (moved != f) moved->census_idx = i;
            f->census_idx = BENDER_CENSUS_UNTRACKED;
            return;
        }
    }
}

/* P3.4 bookkeeping touch (see gc.h): old containers go dirty, and
 * nothing else happens. Used by raw table puts for marshal identity
 * maps (never the value graph). */
void bender_gc_touch(void *obj) {
    BenderSlabPage *page = bender_slab_page_for(obj);
    if (NULL == page) return;
    if (page->age >= BENDER_SLAB_OLD_AGE) page->dirty = 1;
}

/* Minor-mode reachability gate (Phase 2b-iii policy): during a minor
 * collection, skip old, clean, precisely-tracked containers (their young
 * references are covered by the barrier). Everything else is traversed
 * exactly like a major: young objects, large malloc-backed objects
 * (always traversed, few), disabled objects, fibers, abstracts, weak
 * tables (cleared only at majors), and all non-table/array types.
 * Skipping is always safe; traversing is always safe; only skipping an
 * object with unbarriered young edges would be wrong, and every such
 * edge goes through bender_gc_barrier. */
static int bender_gc_minor_skip(BenderGCObject *mem) {
    if (!bender_vm.gc_minor_phase) return 0;
    /* While any compilation is active, traverse everything: compilation
     * interleaves symbol/env table traffic (macroexpansion resolves and
     * installs bindings, lint tables churn) with macro-fiber collections
     * in ways that defeat the clean/generation accounting the gate
     * relies on. Compiles are infrequent next to runtime stores, and
     * minors still skip all old sweeping, so this costs little. */
    if (bender_vm.compiler_stack) return 0;
    int32_t flags = mem->flags;
    if (flags & BENDER_MEM_DISABLED) return 0;
    int32_t type = flags & BENDER_MEM_TYPEBITS;
#if 0 /* TEMPORARY 2b-iii bisect: tables always traverse */
    if (type != BENDER_MEMORY_TABLE && type != BENDER_MEMORY_ARRAY) return 0;
#else
    if (type != BENDER_MEMORY_ARRAY) return 0;
#endif
    BenderSlabPage *page = bender_slab_page_for(mem);
    if (NULL == page) return 0;
    if (page->age < BENDER_SLAB_OLD_AGE) return 0;
    return !page->dirty;
}

/* Step 1.2 cap: past this many cross-fiber edges, recording stops
 * (stuck) instead of growing without bound. Pruning lands in 1.3. */
#define BENDER_XFB_MAX (1024 * 1024)

/* Generational write barrier (Phase 2b-iii policy): record an old
 * container that newly references a heap value, so minor collections
 * (which skip clean old tables/arrays) still find the edge. Immediates
 * (nil/boolean/number/cfunction/pointer) never create edges; young and
 * large containers are always traversed, so only old slab pages are
 * marked dirty. Call after every store of a Bender value into a heap
 * table, array, or equivalent long-lived container.
 *
 * Step 1.2 cross-fiber recording rides along: when container and value
 * live on pages owned by different fibers (mixed pages read as shared),
 * the container joins the cross-fiber remembered set for per-fiber
 * minors (1.3). Shared either side needs nothing (shared space is
 * always traversed). Hot path unchanged when same-owner. */
void bender_gc_barrier(void *obj, Bender value) {
    BENDER_GC_WORK_TICK(work_barrier_calls);
    BenderType t = bender_type(value);
    if (t == BENDER_NIL || t == BENDER_BOOLEAN || t == BENDER_NUMBER ||
            t == BENDER_CFUNCTION || t == BENDER_POINTER) {
        return;
    }
    /* P3.3 strict-export rule, header bits only (no page walks, so the
     * shared-container fast path stays fast): an isolated-heap value
     * must not land in a non-isolated container — the edge would be
     * invisible to per-proc majors. Imports pass silently; immutable
     * string-heap values (no outgoing refs, ever) are exempt;
     * malloc-large either side carries the bit via gcalloc, so no
     * NULL-page hole. Cross-strict sharing (both bits, different
     * owners) is checked on the slow path below, reusing its pages.
     * Gated behind heap_strict_live: legacy VMs (never strict) skip
     * the whole check for one predicted branch. */
    int iso_v = 0;
    int iso_o = 0;
    if (bender_vm.heap_strict_live &&
            t != BENDER_STRING && t != BENDER_SYMBOL && t != BENDER_KEYWORD) {
        iso_o = (bender_gc_header(obj)->flags & BENDER_MEM_ISOLATED) ? 1 : 0;
        /* Object starts (NOT unwrapped data pointers: tuples and
         * structs point past their heads — reading flags there sees
         * element bits as garbage and panics spuriously). */
        void *vobj = NULL;
        switch (t) {
            case BENDER_ARRAY:
                vobj = (void *) bender_unwrap_array(value);
                break;
            case BENDER_TUPLE:
                vobj = (void *) bender_tuple_head(bender_unwrap_tuple(value));
                break;
            case BENDER_TABLE:
                vobj = (void *) bender_unwrap_table(value);
                break;
            case BENDER_STRUCT:
                vobj = (void *) bender_struct_head(bender_unwrap_struct(value));
                break;
            case BENDER_BUFFER:
                vobj = (void *) bender_unwrap_buffer(value);
                break;
            case BENDER_FUNCTION:
                vobj = (void *) bender_unwrap_function(value);
                break;
            case BENDER_ABSTRACT:
                vobj = (void *) bender_abstract_head(bender_unwrap_abstract(value));
                break;
            case BENDER_FIBER:
                vobj = (void *) bender_unwrap_fiber(value);
                break;
            default:
                break;
        }
        if (NULL != vobj) {
            iso_v = (bender_gc_header(vobj)->flags & BENDER_MEM_ISOLATED) ? 1 : 0;
        }
        if (bender_vm.trace_strict) {
            BenderSlabPage *tp = bender_slab_page_for(obj);
            BenderFiber *treq = (NULL != bender_vm.gc_alloc_ctx)
                ? bender_vm.gc_alloc_ctx : bender_vm.fiber;
            bender_eprintf("[strict-put] obj=%p iso_o=%d "
                    "page(owner=%d mixed=%d isolated=%d) ",
                    obj, iso_o,
                    (NULL != tp) ? tp->owner : -1,
                    (NULL != tp) ? tp->mixed : -1,
                    (NULL != tp) ? tp->isolated : -1);
            if (t == BENDER_TABLE) {
                BenderTable *ct = (BenderTable *) obj;
                bender_eprintf("obj-table(count=%d proto=%p) ",
                        ct->count, (void *) ct->proto);
            }
            bender_eprintf("vobj=%p iso_v=%d req=%p req_strict=%d "
                    "req_owner=%d cur_fiber=%p",
                    vobj, iso_v, (void *) treq,
                    (NULL != treq) ? treq->heap_strict : -1,
                    (NULL != treq) ? (int) treq->gc_owner : -1,
                    (void *) bender_vm.fiber);
            fprintf(stderr, " ra0=%p\n", __builtin_return_address(0));
        }
        if (iso_v && !iso_o) {
            bender_panic("strict proc value escaping to a shared container "
                        "(copy it across: messages and spawn entries copy)");
        }
    }
    BenderSlabPage *page = bender_slab_page_for(obj);
    if (NULL == page) return;
    if (page->age >= BENDER_SLAB_OLD_AGE) page->dirty = 1;
    /* Cross-fiber edge below; same-owner (and shared) stores skip it. */
    uint32_t own_o = page->mixed ? 0 : page->owner;
    if (0 == own_o) return;
    void *vptr = NULL;
    Bender cobj = bender_wrap_nil();
    switch (bender_gc_type((BenderGCObject *) obj)) {
        case BENDER_MEMORY_ARRAY:
        case BENDER_MEMORY_ARRAY_WEAK:
            cobj = bender_wrap_array((BenderArray *) obj);
            break;
        case BENDER_MEMORY_TABLE:
        case BENDER_MEMORY_TABLE_WEAKK:
        case BENDER_MEMORY_TABLE_WEAKV:
        case BENDER_MEMORY_TABLE_WEAKKV:
            cobj = bender_wrap_table((BenderTable *) obj);
            break;
        default:
            return;
    }
    switch (t) {
        case BENDER_STRING:
        case BENDER_SYMBOL:
        case BENDER_KEYWORD:
            vptr = (void *) bender_unwrap_string(value);
            break;
        case BENDER_ARRAY:
            vptr = (void *) bender_unwrap_array(value);
            break;
        case BENDER_TUPLE:
            vptr = (void *) bender_unwrap_tuple(value);
            break;
        case BENDER_TABLE:
            vptr = (void *) bender_unwrap_table(value);
            break;
        case BENDER_STRUCT:
            vptr = (void *) bender_unwrap_struct(value);
            break;
        case BENDER_BUFFER:
            vptr = (void *) bender_unwrap_buffer(value);
            break;
        case BENDER_FUNCTION:
            vptr = (void *) bender_unwrap_function(value);
            break;
        case BENDER_ABSTRACT:
            vptr = (void *) bender_unwrap_abstract(value);
            break;
        case BENDER_FIBER:
            vptr = (void *) bender_unwrap_fiber(value);
            break;
        default:
            return;
    }
    BenderSlabPage *pv = bender_slab_page_for(vptr);
    if (NULL == pv) return;
    uint32_t own_v = pv->mixed ? 0 : pv->owner;
    if (0 == own_v || own_v == own_o) return;
    /* P3.3 cross-strict sharing: both bits set but different owners
     * (two strict heaps, or strict + claimed-strict page) — refuse
     * (message-passing keeps heaps disjoint). Reuses this path's
     * pages: zero extra walks. Unreachable when no strict fiber ever
     * ran (bits never set — iso_* stay 0 above). */
    if (iso_v && iso_o) {
        bender_panic("strict proc value shared across isolated heaps "
                    "(copy it across: messages and spawn entries copy)");
    }
    BenderSlabState *ss = bender_slab_state();
    if (ss->xfb_stuck) return;
    if (ss->xfb_count == ss->xfb_cap) {
        size_t ncap = ss->xfb_cap ? ss->xfb_cap * 2 : 64;
        if (ncap > BENDER_XFB_MAX) {
            ss->xfb_stuck = 1;
            return;
        }
        Bender *nxfb = bender_malloc(ncap * sizeof(Bender));
        if (NULL == nxfb) {
            ss->xfb_stuck = 1;
            return;
        }
        if (NULL != ss->xfb) {
            memcpy(nxfb, ss->xfb, ss->xfb_count * sizeof(Bender));
            bender_free(ss->xfb);
        }
        ss->xfb = nxfb;
        ss->xfb_cap = ncap;
    }
    ss->xfb[ss->xfb_count++] = cobj;
    /* Erlang-parity messaging fix: dual-write into the owner-bucket
     * side-structure at the SAME insert site (own_o already computed
     * above, no extra lookup). The main array above is unaffected;
     * this is purely additive. */
    bender_xfb_bucket_append(ss, own_o, cobj);
    /* Bucket keyed by the VALUE's owner as well.
     *
     * bender_xfb_mark_owned(ss, f->gc_owner) is called from a fiber's own
     * minor and looks up ITS owner's bucket, then marks those entries to
     * keep foreign young blocks alive. The append above keys on the
     * CONTAINER's owner (own_o), so it only protects the container. That
     * asymmetry loses the container itself whenever a parent-heap
     * container holds a value born on a child fiber's heap: the child's
     * minor looks up the child's bucket, finds nothing recorded for the
     * parent's array, and sweeps the parent's array (or its young
     * elements) as its own garbage.
     *
     * Observed: a captured function-scope array mutated inside an ev/go
     * fiber reads correctly on every iteration and turns to garbage at
     * completion (docs/internals/known-issues-fiber-child-minor.md).
     * Recording the edge under the value's owner too makes the marking
     * symmetric, so whichever of the two heaps runs a minor finds it. */
    if (own_v != own_o) {
        bender_xfb_bucket_append(ss, own_v, cobj);
    }
}

void *bender_gcalloc(enum BenderMemoryType type, size_t size) {
    BenderGCObject *mem;

    /* Make sure everything is inited */
    bender_assert(NULL != bender_vm.cache, "please initialize bender before use");
    /* Phase 2b-iii: slab pages for size-classed objects, malloc large
     * path otherwise. Either way the object joins a heap list below. */
    int slab_class = bender_slab_class_for(size);
    if (slab_class >= 0) {
        mem = bender_slab_alloc(slab_class);
    } else {
        mem = bender_malloc(size);
    }

    /* Check for bad malloc */
    if (NULL == mem) {
        BENDER_OUT_OF_MEMORY;
    }

    /* Requested bytes, not slab occupancy or external buffer storage. */
#if defined(BENDER_NATIVE_EXECUTORS) && BENDER_NATIVE_TELEMETRY
    bender_vm.work_allocations++;
    bender_vm.work_alloc_bytes += size;
#endif
    /* Configure block */
    mem->flags = type;
    /* P3.3 strict bit: slab objects follow their page's quarantine
     * (mixed pages read shared for owner AND bit — a non-strict
     * reuse of a strict slot must not inherit strictness, or
     * compile-time tables built on recycled slots would trip the
     * export rule); fresh pages carry the claimer's flag; malloc-
     * large objects (never recycled) take the allocating fiber's
     * flag (P3.5c: attribution override included). Single funnel:
     * every birth passes here. */
    if (slab_class >= 0) {
        BenderSlabPage *pg = bender_slab_page_for(mem);
        if (NULL != pg && !pg->mixed && pg->isolated) {
            mem->flags |= BENDER_MEM_ISOLATED;
        }
    } else {
        BenderFiber *breq = (NULL != bender_vm.gc_alloc_ctx)
            ? bender_vm.gc_alloc_ctx : bender_vm.fiber;
        if (NULL != breq && breq->heap_strict) {
            mem->flags |= BENDER_MEM_ISOLATED;
        }
    }

    /* Prepend block to heap list. Immutable strings (strings, symbols,
     * keywords -- keywords intern to symbols) live on their own heap so
     * collection policy can treat them independently (Phase 2b-ii).
     * P3.5b Slice A (owner-segregated filing): non-fiber births file
     * into the running fiber's per-heap lists (0 main, 1 strings,
     * 2 weak) instead of the globals, so per-fiber sweeps can walk
     * O(own) (Slice B). Fiber objects and host births (no running
     * fiber) stay global: fibers are scheduler roots (census), always
     * discoverable; this also keeps construction UAF-impossible (a
     * half-built fiber is never filed behind an uninitialized
     * heap_lists head — gcalloc never triggers a collection itself,
     * and no maybe_collect runs mid-construction). Same free-set as
     * before: every sweep path walks globals + all fiber lists. */
    bender_vm.next_collection += size;
    {
        int kind;
        BenderGCObject **head;
        if (type == BENDER_MEMORY_STRING || type == BENDER_MEMORY_SYMBOL) {
            kind = 1;
        } else if (type < BENDER_MEMORY_TABLE_WEAKK) {
            kind = 0;
        } else {
            kind = 2;
        }
        /* P3.5c: attribution override (strict spawn construction)
         * redirects births to the child being built (owner id, strict
         * bit, nursery, filing). Fiber objects always stay global. */
        BenderFiber *eff = (NULL != bender_vm.gc_alloc_ctx)
            ? bender_vm.gc_alloc_ctx : bender_vm.fiber;
        if (NULL != eff && type != BENDER_MEMORY_FIBER) {
            head = (BenderGCObject **) &eff->heap_lists[kind];
#ifdef BENDER_HEAP_FILE_CHECK
            {
                BenderSlabState *css = (BenderSlabState *) bender_vm.slab_state;
                int cfound = 0;
                size_t ci;
                if (NULL != css) {
                    for (ci = 0; ci < css->fibers_count; ci++) {
                        if (css->fibers[ci] == eff) { cfound = 1; break; }
                    }
                }
                if (!cfound) {
                    fprintf(stderr, "file-check: filing fiber %p not in census (%d) for type %d kind %d\n",
                            (void *) eff,
                            NULL != css ? (int) css->fibers_count : -1,
                            (int) type, kind);
                    abort();
                }
            }
#endif
        } else if (kind == 1) {
            head = (BenderGCObject **) &bender_vm.string_blocks;
        } else if (kind == 0) {
            head = (BenderGCObject **) &bender_vm.blocks;
        } else {
            head = (BenderGCObject **) &bender_vm.weak_blocks;
        }
        mem->data.next = *head;
        *head = mem;
    }
    bender_vm.block_count++;

    return (void *)mem;
}

static void free_one_scratch(BenderScratch *s) {
    if (NULL != s->finalize) {
        s->finalize((char *) s->mem);
    }
    bender_free(s);
}

/* Free all allocated scratch memory */
static void bender_free_all_scratch(void) {
    for (size_t i = 0; i < bender_vm.scratch_len; i++) {
        free_one_scratch(bender_vm.scratch_mem[i]);
    }
    bender_vm.scratch_len = 0;
}

static BenderScratch *bender_mem2scratch(void *mem) {
    BenderScratch *s = (BenderScratch *)mem;
    return s - 1;
}

/* Mark the full root set (Phase 2a explicit roots + 2b-i shadow roots).
 * Shared by major and minor collections; in minors the table/array gate
 * (bender_gc_minor_skip) prunes old-clean subtrees during traversal. */
static void bender_mark_roots(void) {
    uint32_t i;
    size_t orig_rootcount = bender_vm.root_count;
#ifdef BENDER_EV
    bender_ev_mark();
#endif
    if (bender_vm.top_dyns) {
        bender_mark(bender_wrap_table(bender_vm.top_dyns));
    }
    if (bender_vm.profile_table) {
        bender_mark(bender_wrap_table(bender_vm.profile_table));
    }
    /* B7.1 named proc registry: same class as the two above -- a C
     * container mutated without the bender write barrier. Marking it
     * keeps a REGISTERED proc alive (correct: a live registration must
     * keep its process reachable); a completed proc is dropped from
     * the table by the next proc/whereis or proc/register that touches
     * the name, which releases the reference naturally. No gcroot and
     * no extra marking code: an ordinary BenderTable's own marking
     * walks the values (bender_procat_mark covers the proc's children). */
    if (bender_vm.proc_registry) {
        bender_mark(bender_wrap_table(bender_vm.proc_registry));
    }
    if (bender_vm.root_fiber != NULL) { /* Can be NULL if bender_collect called outside of interpreter loop */
        bender_mark_fiber(bender_vm.root_fiber);
    }
    /* Mark values held in C structs by live compilations (Phase 2a). */
    for (BenderCompiler *comp = bender_vm.compiler_stack; comp; comp = comp->compiler_next) {
        bender_mark_compiler(comp);
    }
    for (i = 0; i < orig_rootcount; i++)
        bender_mark(bender_vm.roots[i]);
    /* Scoped shadow roots (Phase 2b-i): everything pushed is live. */
    for (i = 0; i < bender_vm.shadow_count; i++)
        bender_mark(bender_vm.shadow_roots[i]);
    /* Step 1.2 cross-fiber remembered set: recorded containers stay
     * live (pruned in 1.3). */
    {
        BenderSlabState *xss = (BenderSlabState *) bender_vm.slab_state;
        if (NULL != xss) {
            size_t xi;
            for (xi = 0; xi < xss->xfb_count; xi++)
                bender_mark(xss->xfb[xi]);
        }
    }
    while (orig_rootcount < bender_vm.root_count) {
        Bender x = bender_vm.roots[--bender_vm.root_count];
        bender_mark(x);
    }
}

/* Minor-mode edge verifier (Phase 2b-iii debugging, env-gated via
 * BENDER_GC_VERIFY=1): after minor marking, walk every heap object and
 * check that each young heap object reachable through a live edge is
 * marked. Reports the holder type and aborts on the first miss (a missed
 * barrier edge or a wrongly skipped container). Expensive: development
 * use only. */
static int bender_gc_verify_young_marked(const char *holder, Bender v) {
    BenderType t = bender_type(v);
    BenderGCObject *m = NULL;
    switch (t) {
        default:
            return 1;
        case BENDER_FIBER:
            m = (BenderGCObject *) bender_unwrap_fiber(v);
            break;
        case BENDER_STRING:
        case BENDER_SYMBOL:
        case BENDER_KEYWORD:
            m = (BenderGCObject *) bender_string_head(bender_unwrap_string(v));
            break;
        case BENDER_ARRAY:
            m = (BenderGCObject *) bender_unwrap_array(v);
            break;
        case BENDER_TABLE:
            m = (BenderGCObject *) bender_unwrap_table(v);
            break;
        case BENDER_STRUCT:
            m = (BenderGCObject *) bender_struct_head(bender_unwrap_struct(v));
            break;
        case BENDER_TUPLE:
            m = (BenderGCObject *) bender_tuple_head(bender_unwrap_tuple(v));
            break;
        case BENDER_BUFFER:
            m = (BenderGCObject *) bender_unwrap_buffer(v);
            break;
        case BENDER_FUNCTION:
            m = (BenderGCObject *) bender_unwrap_function(v);
            break;
        case BENDER_ABSTRACT:
            m = (BenderGCObject *) bender_abstract_head(bender_unwrap_abstract(v));
            break;
    }
    if (NULL == m) return 1;
    BenderSlabPage *page = bender_slab_page_for(m);
    if (NULL == page || page->age >= BENDER_SLAB_OLD_AGE) return 1;
    if (bender_gc_reachable(m)) return 1;
    fprintf(stderr, "gc-verify: young unmarked %p via %s\n", (void *) m, holder);
    return 0;
}

static int bender_gc_verify_minor(void) {
    int bad = 0;
    /* NOTE: bender_vm.blocks etc. are void* in the struct; cast through.
     * P3.5b Slice A: holders also live in per-fiber lists (pass ==
     * nfib covers the globals). */
    BenderSlabState *vss = (BenderSlabState *) bender_vm.slab_state;
    size_t nfib = (NULL != vss) ? vss->fibers_count : 0;
    for (size_t pass = 0; pass <= nfib; pass++) {
    void *heads[3];
    if (pass == nfib) {
        heads[0] = bender_vm.blocks;
        heads[1] = bender_vm.string_blocks;
        heads[2] = bender_vm.weak_blocks;
    } else {
        BenderFiber *vf = vss->fibers[pass];
        heads[0] = vf->heap_lists[0];
        heads[1] = vf->heap_lists[1];
        heads[2] = vf->heap_lists[2];
    }
    for (int h = 0; h < 3; h++) {
        for (BenderGCObject *m = (BenderGCObject *) heads[h]; NULL != m; m = m->data.next) {
            /* Only live holders matter: an unmarked holder's subgraph is
             * garbage as a whole (its young referents are correctly
             * unmarked too). Disabled objects are always kept+traversed,
             * so their edges count as live. */
            if (!(bender_gc_reachable(m) || (m->flags & BENDER_MEM_DISABLED)) &&
                    !bender_slab_obj_old(m)) continue;
            int32_t type = m->flags & BENDER_MEM_TYPEBITS;
            if (type == BENDER_MEMORY_TABLE || type == BENDER_MEMORY_TABLE_WEAKK ||
                    type == BENDER_MEMORY_TABLE_WEAKV || type == BENDER_MEMORY_TABLE_WEAKKV) {
                BenderTable *t = (BenderTable *) m;
                if (t->proto) {
                    if (!bender_gc_verify_young_marked("table-proto", bender_wrap_table(t->proto))) bad = 1;
                }
                BenderKV *end = t->data + t->capacity;
                for (BenderKV *kv = t->data; kv < end; kv++) {
                    if (!bender_gc_verify_young_marked("table-key", kv->key)) bad = 1;
                    if (!bender_gc_verify_young_marked("table-value", kv->value)) bad = 1;
                }
            } else if (type == BENDER_MEMORY_ARRAY || type == BENDER_MEMORY_ARRAY_WEAK) {
                BenderArray *a = (BenderArray *) m;
                for (int32_t i = 0; i < a->count; i++) {
                    if (!bender_gc_verify_young_marked("array-elem", a->data[i])) bad = 1;
                }
            } else if (type == BENDER_MEMORY_TUPLE) {
                BenderTupleHead *th = (BenderTupleHead *) m;
                for (int32_t i = 0; i < th->length; i++) {
                    if (!bender_gc_verify_young_marked("tuple-elem", th->data[i])) bad = 1;
                }
            } else if (type == BENDER_MEMORY_STRUCT) {
                BenderStructHead *sh = (BenderStructHead *) m;
                for (int32_t i = 0; i < sh->length; i++) {
                    if (!bender_gc_verify_young_marked("struct-key", sh->data[i].key)) bad = 1;
                    if (!bender_gc_verify_young_marked("struct-value", sh->data[i].value)) bad = 1;
                }
            } else if (type == BENDER_MEMORY_FIBER) {
                BenderFiber *f = (BenderFiber *) m;
                if (f->data) {
                    for (int32_t i = f->stackstart; i < f->stacktop; i++) {
                        if (!bender_gc_verify_young_marked("fiber-slot", f->data[i])) bad = 1;
                    }
                }
                if (f->env) {
                    if (!bender_gc_verify_young_marked("fiber-env", bender_wrap_table(f->env))) bad = 1;
                }
                if (f->child) {
                    if (!bender_gc_verify_young_marked("fiber-child", bender_wrap_fiber(f->child))) bad = 1;
                }
                if (!bender_gc_verify_young_marked("fiber-last", f->last_value)) bad = 1;
                if (!bender_gc_verify_young_marked("fiber-in", f->in_value)) bad = 1;
                if (!bender_gc_verify_young_marked("fiber-out", f->out_payload)) bad = 1;
            } else if (type == BENDER_MEMORY_FUNCTION) {
                /* envs are raw pointers, always followed by mark_function
                 * (functions are never gated); env objects themselves are
                 * walked independently below. Nothing to check here. */
            } else if (type == BENDER_MEMORY_FUNCDEF) {
                BenderFuncDef *def = (BenderFuncDef *) m;
                for (int32_t i = 0; i < def->constants_length; i++) {
                    if (!bender_gc_verify_young_marked("funcdef-const", def->constants[i])) bad = 1;
                }
            } else if (type == BENDER_MEMORY_FUNCENV) {
                BenderFuncEnv *env = (BenderFuncEnv *) m;
                if (env->offset == 0 && env->as.values) {
                    for (int32_t i = 0; i < env->length; i++) {
                        if (!bender_gc_verify_young_marked("funcenv-value", env->as.values[i])) bad = 1;
                    }
                }
            }
        }
    }
    }
    return bad;
}

/* Step 1.3 per-fiber minor: free only the target's young unmarked
 * blocks. Marking is complete (full roots + every census fiber, so no
 * unreaped referrer can dangle, plus the cross-fiber remembered set),
 * which makes this at least as sound as a full minor; precision comes
 * from the sweep filter (foreign young unmarked floats until a global
 * collection). No aging, no global pressure reset, no weak dropping
 * (minor rules, unchanged): promotion and global cadence stay owned by
 * full minors/majors. Only the target's nursery counter resets. A stuck
 * remembered set degrades to a full minor (missing edges would be
 * unsound to ignore). */

/* Erlang-parity messaging fix, final form: true owner-bucketing at
 * insert time. Two attempts preceded this (a) full linear scan with
 * an owner filter -- O(n) per minor, O(n^2) overall; (b) a lazily
 * rebuilt sorted-by-owner index -- amortizes correctly only when
 * scans are rare between rebuilds, but this workload scans on almost
 * every insert, so its unsorted-tail scan cost stayed quadratic in
 * practice (directly profiled and self-corrected, see proc-lanes.md).
 * This append is O(1) amortized (same doubling-array pattern as the
 * main xfb array itself) and needs no sort of any kind. */
static void bender_xfb_bucket_append(BenderSlabState *ss, uint32_t owner, Bender cobj) {
    if (NULL == ss->xfb_buckets) {
        ss->xfb_buckets = bender_malloc(BENDER_XFB_NBUCKETS * sizeof(struct BenderXfbBucketNode *));
        if (NULL == ss->xfb_buckets) return;
        memset(ss->xfb_buckets, 0, BENDER_XFB_NBUCKETS * sizeof(struct BenderXfbBucketNode *));
    }
    size_t slot = owner % BENDER_XFB_NBUCKETS;
    struct BenderXfbBucketNode *node = ss->xfb_buckets[slot];
    while (NULL != node && node->owner != owner) node = node->next;
    if (NULL == node) {
        node = bender_malloc(sizeof(struct BenderXfbBucketNode));
        if (NULL == node) return;
        node->owner = owner;
        node->data = NULL;
        node->count = 0;
        node->cap = 0;
        node->next = ss->xfb_buckets[slot];
        ss->xfb_buckets[slot] = node;
    }
    if (node->count == node->cap) {
        size_t ncap = node->cap ? node->cap * 2 : 8;
        Bender *ndata = bender_realloc(node->data, ncap * sizeof(Bender));
        if (NULL == ndata) return;
        node->data = ndata;
        node->cap = ncap;
    }
    node->data[node->count++] = cobj;
}

/* Erlang-parity messaging fix: mark exactly this owner's bucket --
 * O(1) hash lookup (chain length bounded by BENDER_XFB_NBUCKETS'
 * size relative to live owner count) + O(this owner's own entries).
 * No scan of any other owner's data at all; no sort, no rebuild. */
static void bender_xfb_mark_owned(BenderSlabState *ss, uint32_t owner) {
    if (NULL == ss->xfb_buckets) return;
    size_t slot = owner % BENDER_XFB_NBUCKETS;
    struct BenderXfbBucketNode *node = ss->xfb_buckets[slot];
    while (NULL != node && node->owner != owner) node = node->next;
    if (NULL == node) return;
    for (size_t i = 0; i < node->count; i++) {
        bender_mark(node->data[i]);
    }
}

/* Close a scoped collection's trigger accounting for fiber f: clear the
 * nursery and re-snapshot the live block count that this fiber's NEXT minor
 * trigger is scaled by. The two must always move together -- see the
 * gc_minor_blocks comment in bender.h for why the trigger is scaled by this
 * snapshot instead of a block_count read at trigger time. */
static void bender_fiber_minor_reset(BenderFiber *f) {
    if (NULL == f) return;
    f->gc_nursery_bytes = 0;
    f->gc_minor_blocks = bender_vm.block_count;
}

/* A proc minor can reclaim only young blocks belonging to its owner.
 * Mixed pages, old blocks and disabled objects survive regardless of marks. */
static int bender_proc_minor_has_candidates(BenderFiber *f) {
    for (int kind = 0; kind < 3; kind++) {
        for (BenderGCObject *cur = f->heap_lists[kind]; cur; cur = cur->data.next) {
            if (!(cur->flags & BENDER_MEM_DISABLED) &&
                    !bender_slab_obj_old(cur) &&
                    bender_slab_obj_owner(cur) == f->gc_owner &&
                    BENDER_MEMORY_FIBER != bender_gc_type(cur)) return 1;
        }
    }
    return 0;
}

/* With no reclaimable blocks, reproduce the sweep's maintenance only. */
static void bender_proc_minor_clear_stamps(BenderFiber *f) {
    for (int kind = 0; kind < 3; kind++) {
        for (BenderGCObject *cur = f->heap_lists[kind]; cur; cur = cur->data.next) {
            if (!(cur->flags & BENDER_MEM_DISABLED))
                cur->flags &= ~BENDER_MEM_EPOCH_MASK;
        }
    }
    bender_sweep_drain_pending();
    bender_heap_verify("proc-owned-no-young");
}

void bender_collect_fiber_minor(BenderFiber *f) {
    BenderSlabState *ss;
    if (bender_vm.gc_suspend) return;
    /* Fallbacks delegate to the global minor; count only actual scopes. */
    if (NULL == f || 0 == f->gc_owner) {
        /* D1 (plain-fiber form): the global minor never clears the
         * caller's gc_nursery_bytes. */
        bender_collect_minor();
        bender_fiber_minor_reset(f);
        return;
    }
    ss = (BenderSlabState *) bender_vm.slab_state;
    if (NULL != ss && ss->xfb_stuck) {
        bender_collect_minor();
        bender_fiber_minor_reset(f);
        return;
    }
    /* No free decision can depend on tracing when the scoped sweep has no
     * eligible objects. Leave epochs, visit cycles and incoming edges alone;
     * any later collection that can reclaim objects performs its full mark. */
    if (NULL != f->proc && !bender_proc_minor_has_candidates(f)) {
        bender_proc_minor_clear_stamps(f);
        bender_fiber_minor_reset(f);
        return;
    }
    BENDER_GC_WORK_TICK(work_gc_fiber_minor);
    bender_gc_epoch_next();
    depth = BENDER_RECURSION_GUARD;
    bender_vm.gc_mark_phase = 1;
    bender_vm.gc_minor_phase = 1;
    /* Phase 1 census removal: a fiber minor marks its OWN world plus
     * the cross-fiber remember set — NOT every registered fiber (the
     * old loop was O(N fibers x stack) per minor) and NOT the root
     * env (the 100k-spawn profile showed mark_roots as the quadratic
     * term: each child's minor walked the root env, which the
     * workload's own bookkeeping fills with N proc handles).
     * Soundness: only f's lists are swept, so foreign blocks survive
     * unmarked; foreign references INTO this heap — including any
     * root env entry — went through the write barrier and are
     * recorded in xfb, marked here; a stuck remember set already
     * degraded to a global minor above. Registered fibers prune
     * synchronously at deinit (census_remove), so no dangling
     * registry reads. */
    bender_mark_fiber(f);
    /* C-side VM roots: containers mutated by C code WITHOUT the bender
     * write barrier — the roots-free design's explicit pin set. The
     * profile table (PGO auto-collect runs it in every AOT binary)
     * holds function keys allocated young on whatever fiber was
     * running; unmarked, a child minor sweeps them and deinit's
     * profile_dump walks freed keys (the AOT teardown SEGV,
     * faulting in bender_profile_rows at 0x11). top_dyns is the same
     * class. Both are small; child minors are rare. */
    if (bender_vm.top_dyns) {
        bender_mark(bender_wrap_table(bender_vm.top_dyns));
    }
    if (bender_vm.profile_table) {
        bender_mark(bender_wrap_table(bender_vm.profile_table));
    }
    /* B7.1 named proc registry: third member of the same pin set. A
     * proc/register that ran on some fiber stored a proc handle into
     * this table without a write barrier, so a LATER actor's own minor
     * would otherwise sweep that handle (the registry entry then holds
     * a freed proc). Marked here exactly as top_dyns is. */
    if (bender_vm.proc_registry) {
        bender_mark(bender_wrap_table(bender_vm.proc_registry));
    }
    /* Event-loop C roots: the spawn task queue and the timeout queue
     * are naked C arrays that hold queued wakeups — task fibers and
     * resume values — allocated young on ANY fiber's heap (a channel
     * claim schedules the parked reader with a value born mid-push;
     * the loop schedules freshly spawned fibers before their parent
     * ever references them). A fiber-scoped minor that skips them can
     * sweep another fiber's queued wakeup: the drain then pops a
     * swept task, the expected_sched_id check drops it silently, and
     * the loop parks in poll forever with an empty spawn queue (the
     * bulk proc fan-in lost-wakeup). bender_ev_mark is the same root
     * set every full collect applies.
     *
     * CORRECTED (Erlang-parity messaging fix, see proc-lanes.md): the
     * "bounded by live queued work, so cheap here" claim above was
     * WRONG -- confirmed via live lldb backtraces. A queued fiber
     * (e.g. a driver script parked on ev/sleep mid spawn-burst) can
     * have an arbitrarily large live stack, and bender_ev_mark's full
     * bender_mark_fiber on every queued fiber walks that whole stack
     * on EVERY actor's own per-actor minor: O(N) per minor x N actors
     * = O(N^2), the exact cause of the bench/agents.ben wall. This
     * scoped collector only needs to keep a queued fiber's OWN block
     * from being swept if it happens to live on THIS heap, not to
     * prove its full reachability (that is whichever collection
     * actually owns and sweeps it) -- use the shallow variant. */
    bender_ev_mark_shallow();
    /* Erlang-parity messaging fix, part 3 (final form: true owner-
     * bucketing, see docs/internals/proc-lanes.md): bender_xfb_mark_owned
     * marks only this owner's bucket (O(1) hash lookup + O(this
     * owner's own entries)) instead of linearly scanning every xfb
     * entry on every single actor's own minor. Safety argument: an
     * entry's owner (fixed at insert time) can only ever degrade to
     * mixed/0, which makes it immune to any single-owner's scoped
     * sweep regardless of marking, so there is no premature-free
     * risk from bucketing by it. */
    bender_xfb_mark_owned(ss, f->gc_owner);
    bender_vm.gc_mark_phase = 0;
    /* Erlang-parity messaging fix (docs/internals/proc-lanes.md): the
     * real O(n^2) root cause, confirmed by direct code inspection (no
     * profiling ambiguity) -- bender_sweep_heaps_owned's wide path
     * (bender_sweep_fiber_owned -> bender_sweep_kind with walk_fibers=1)
     * loops over EVERY registered fiber in the global census on
     * EVERY call. bender_collect_fiber_minor is called once per actor
     * per minor, so a plain proc/spawn burst of N actors pays O(N)
     * census walk x N actors' own minors = O(N^2). This is the gap
     * P3.5c ("the linearizer", a5d9168e) left behind: it only
     * extended owner-attribution + narrow O(own) sweep
     * (bender_sweep_proc_owned, vm.c:122's heap_strict check) to
     * STRICT/isolated procs; plain (non-strict) procs were
     * deliberately left on the wide path ("no scale win claimed",
     * a739009e). A plain proc's fiber still has f->proc set (proc.c's
     * cfun_proc_spawn_impl sets it unconditionally), so route ANY
     * proc fiber -- strict or plain -- to the already-proven-safe
     * narrow sweep. Marking above is UNCHANGED (still the full
     * xfb/ev_mark/profile_table C-roots walk, still correct and
     * necessary for a plain proc's genuine cross-heap sharing); only
     * the SWEEP's redundant wide census walk is narrowed. Fibers with
     * no proc (raw tasklets, boot/image-generation's own fibers) keep
     * the wide path exactly as before -- that is the documented
     * "crashes boot image generation deterministically" risk
     * (a739009e), which this change does not touch. */
    if (NULL != f->proc) {
        bender_sweep_proc_owned(f);
    } else {
        bender_sweep_heaps_owned(f);
    }
    if (getenv("BENDER_ROOT_TRACE")) {
        static long cfmi = 0;
        if (++cfmi <= 12 || 0 == cfmi % 2000)
            fprintf(stderr, "[CFM2] n=%ld fiber=%p owner=%u nurs=%d xfb=%d\n",
                    cfmi, (void *) f, f->gc_owner,
                    (int) f->gc_nursery_bytes, (int) ss->xfb_count);
    }
#ifdef BENDER_GC_COUNT
    g_n_owned++;
#endif
    bender_vm.gc_minor_phase = 0;
    bender_fiber_minor_reset(f);
}

/* P3.4 selective C roots for per-proc majors: everything mark_roots
 * covers EXCEPT fibers and the spawn queue (foreign stacks are the
 * point of skipping). Mirrors the shared parts (small, stable). */
static void bender_mark_proc_shared(uint32_t owner_id) {
    uint32_t i;
    size_t orig_rootcount = bender_vm.root_count;
    if (bender_vm.top_dyns) {
        bender_mark(bender_wrap_table(bender_vm.top_dyns));
    }
    if (bender_vm.profile_table) {
        bender_mark(bender_wrap_table(bender_vm.profile_table));
    }
    /* B7.1 named proc registry (same pin set as top_dyns; see the
     * long note in bender_mark_roots). */
    if (bender_vm.proc_registry) {
        bender_mark(bender_wrap_table(bender_vm.proc_registry));
    }
    {
        BenderCompiler *comp;
        for (comp = bender_vm.compiler_stack; comp; comp = comp->compiler_next) {
            bender_mark_compiler(comp);
        }
    }
    for (i = 0; i < orig_rootcount; i++)
        bender_mark(bender_vm.roots[i]);
    for (i = 0; i < bender_vm.shadow_count; i++)
        bender_mark(bender_vm.shadow_roots[i]);
    /* Remembered cross-owner containers (holes route through here —
     * keep + traverse, never trust). Erlang-parity messaging fix:
     * same bender_xfb_mark_owned indexed lookup as the fiber minor
     * (same O(n^2) risk for a strict-proc spawn burst otherwise). */
    {
        BenderSlabState *xss = (BenderSlabState *) bender_vm.slab_state;
        if (NULL != xss) {
            bender_xfb_mark_owned(xss, owner_id);
        }
    }
}

/* P3.4 verify support: does Bender value x point at object o
 * (object-start-exact, same head discipline as the barrier)? */
static int bender_verify_points_at(Bender x, BenderGCObject *o) {
    void *p = NULL;
    switch (bender_type(x)) {
        case BENDER_STRING:
        case BENDER_SYMBOL:
        case BENDER_KEYWORD:
            return 0;
        case BENDER_ARRAY:
            p = (void *) bender_unwrap_array(x);
            break;
        case BENDER_TUPLE:
            p = (void *) bender_tuple_head(bender_unwrap_tuple(x));
            break;
        case BENDER_TABLE:
            p = (void *) bender_unwrap_table(x);
            break;
        case BENDER_STRUCT:
            p = (void *) bender_struct_head(bender_unwrap_struct(x));
            break;
        case BENDER_BUFFER:
            p = (void *) bender_unwrap_buffer(x);
            break;
        case BENDER_FUNCTION:
            p = (void *) bender_unwrap_function(x);
            break;
        case BENDER_ABSTRACT:
            p = (void *) bender_abstract_head(bender_unwrap_abstract(x));
            break;
        case BENDER_FIBER:
            p = (void *) bender_unwrap_fiber(x);
            break;
        default:
            return 0;
    }
    return p == (void *) o;
}

/* P3.4 verify (BENDER_PROC_VERIFY=1, dev only): audit that no swept
 * candidate is reachable from a foreign stack (struct fields, stack
 * slots, fiber env tables shallow). Candidates were P-owned young
 * unmarked — any hit is a would-be-unsound free: report + abort. */
#define BENDER_PROC_VERIFY_MAXCAND 4096
static void bender_proc_verify(BenderProcess *proc, BenderGCObject **cands, size_t ncand) {
    BenderSlabState *ss = (BenderSlabState *) bender_vm.slab_state;
    size_t ci, fi;
    if (NULL == ss) return;
    for (fi = 0; fi < ss->fibers_count; fi++) {
        BenderFiber *f = ss->fibers[fi];
        if (f == proc->fiber) continue;
        for (ci = 0; ci < ncand; ci++) {
            int32_t s;
            for (s = 0; s < f->stacktop; s++) {
                if (bender_verify_points_at(f->data[s], cands[ci])) {
                    fprintf(stderr, "proc-verify: foreign stack holds swept candidate %p\n",
                            (void *) cands[ci]);
                    abort();
                }
            }
            if (bender_verify_points_at(f->last_value, cands[ci]) ||
                    bender_verify_points_at(f->in_value, cands[ci]) ||
                    bender_verify_points_at(f->out_payload, cands[ci])) {
                fprintf(stderr, "proc-verify: foreign fiber field holds swept candidate %p\n",
                        (void *) cands[ci]);
                abort();
            }
            if (NULL != f->env) {
                int32_t k;
                for (k = 0; k < f->env->capacity; k++) {
                    BenderKV *kv = &f->env->data[k];
                    if (bender_verify_points_at(kv->key, cands[ci]) ||
                            bender_verify_points_at(kv->value, cands[ci])) {
                        fprintf(stderr, "proc-verify: foreign env holds swept candidate %p\n",
                                (void *) cands[ci]);
                        abort();
                    }
                }
            }
        }
    }
}

/* P3.4 per-proc major: free only the target strict proc's young
 * unmarked blocks (foreign young floats until a global collection).
 * Marking is P-roots (fiber + struct fields + env) plus shared C
 * roots plus the remembered set — foreign FIBER stacks are skipped,
 * which is sound exactly when no P-young is reachable from them:
 * spawn-copy entry (P3.2) + export panics (P3.3 table/array puts +
 * plain-channel gives) + copy sends close every programmatic path;
 * FFI/manual C smuggling stays documented-unsound and is what
 * BENDER_PROC_VERIFY=1 audits for. No aging, no global pressure
 * reset, no weak dropping (minor rules); only the fiber's nursery
 * counter resets. Non-strict, unowned, or dead fibers degrade to a
 * full minor (never unsound, just global). Running fibers collect
 * like fiber minors (instruction-boundary stacks are consistent).
 */
void bender_collect_process(BenderProcess *proc) {
    /* The NULL-proc fallback short-circuits before assigning f. */
    BenderFiber *f = NULL;
    if (bender_vm.gc_suspend) return;
    /* Stuck remember set: cross-fiber stores past BENDER_XFB_MAX go
     * unrecorded, so the narrow per-proc mark+sweep would free young
     * blocks still referenced from old foreign containers (the
     * ~900k-proc / ~225k-per-lane density crash: the next mark walk
     * hashed a reused slot). Degrade to the global minor exactly like
     * bender_collect_fiber_minor already does — a global minor marks
     * every registered fiber, so missing remembered edges are moot. */
    {
        BenderSlabState *dss = (BenderSlabState *) bender_vm.slab_state;
        if (NULL != dss && dss->xfb_stuck) {
            if (NULL != proc && NULL != (f = proc->fiber)) {
                bender_fiber_minor_reset(f);
            }
            bender_collect_minor();
            return;
        }
    }
    if (NULL == proc || NULL == (f = proc->fiber) || 0 == f->gc_owner ||
            !proc->isolated_heap || !bender_fiber_can_resume(f)) {
        /* Phase 1.1 D1: the global minor never clears the caller's
         * nursery counter, so the next VM instruction re-fires this
         * path — an infinite loop of O(world) minors. Clear it here.
         * (A fiber-scoped fallback that pins proc fields is WRONG
         * here: pinning a done proc's channels keeps chan_deinit from
         * running, and its parked readers then wait forever.) */
        bender_collect_minor();
        bender_fiber_minor_reset(f);
        return;
    }
    BENDER_GC_WORK_TICK(work_gc_process_minor);
    bender_gc_epoch_next();
    depth = BENDER_RECURSION_GUARD;
    bender_vm.gc_mark_phase = 1;
    bender_vm.gc_minor_phase = 1;
    bender_mark_proc_shared(f->gc_owner);
    bender_mark_fiber(f);
    if (NULL != f->env) bender_mark(bender_wrap_table(f->env));
    if (NULL != proc->mbox) bender_mark(bender_wrap_abstract(proc->mbox));
    if (NULL != proc->sup) bender_mark(bender_wrap_abstract(proc->sup));
    if (NULL != proc->stash) bender_mark(bender_wrap_array(proc->stash));
    if (NULL != proc->monitors) bender_mark(bender_wrap_array(proc->monitors));
    bender_mark(proc->exit_reason);
    bender_vm.gc_mark_phase = 0;
    if (NULL != getenv("BENDER_PROC_VERIFY")) {
        /* Gather P-owned young unmarked candidates (bounded sample).
         * P3.5b Slice A: candidates live in per-fiber lists too. */
        BenderGCObject *cands[BENDER_PROC_VERIFY_MAXCAND];
        size_t ncand = 0;
        BenderSlabState *pss = (BenderSlabState *) bender_vm.slab_state;
        size_t pnfi = (NULL != pss) ? pss->fibers_count : 0;
        size_t ppass;
        for (ppass = 0; ppass <= pnfi && ncand < BENDER_PROC_VERIFY_MAXCAND; ppass++) {
        BenderGCObject *lists[3];
        if (ppass == pnfi) {
            lists[0] = (BenderGCObject *) bender_vm.blocks;
            lists[1] = (BenderGCObject *) bender_vm.string_blocks;
            lists[2] = (BenderGCObject *) bender_vm.weak_blocks;
        } else {
            BenderFiber *pf = pss->fibers[ppass];
            lists[0] = (BenderGCObject *) pf->heap_lists[0];
            lists[1] = (BenderGCObject *) pf->heap_lists[1];
            lists[2] = (BenderGCObject *) pf->heap_lists[2];
        }
        int li;
        for (li = 0; li < 3; li++) {
            BenderGCObject *cur;
            for (cur = lists[li]; NULL != cur; cur = cur->data.next) {
                if (ncand == BENDER_PROC_VERIFY_MAXCAND) break;
                if (bender_gc_reachable(cur) || (cur->flags & BENDER_MEM_DISABLED)) continue;
                if (bender_slab_obj_old(cur)) continue;
                if (bender_slab_obj_owner(cur) != f->gc_owner) continue;
                cands[ncand++] = cur;
            }
        }
        }
        bender_proc_verify(proc, cands, ncand);
    }
    /* P3.5b Slice B: strict process collections sweep O(own) (narrow
     * marking already scoped them; see sweep_proc_owned). */
    bender_sweep_proc_owned(f);
#ifdef BENDER_GC_COUNT
    g_n_proc++;
#endif
    bender_vm.gc_minor_phase = 0;
    bender_fiber_minor_reset(f);
}

/* Minor collection (Phase 2b-iii policy): mark from the same roots (old-
 * clean tables/arrays prune via the gate; old→young edges arrive via
 * the barrier-dirtied pages, which are traversed like everything else),
 * then sweep young dead blocks only. Weak-ref dropping waits for majors
 * (conservative: weak-held young objects float until then). Afterwards
 * young pages age (cap old) and dirty flags clear. */
void bender_collect_minor(void) {
    if (bender_vm.gc_suspend) return;
    BENDER_GC_WORK_TICK(work_gc_minor);
#ifdef BENDER_GC_COUNT
    g_n_minor++;
#endif
    /* Same invariant as full collects: marking requires all REACHABLE
     * bits clear. Scoped process collects set bits on blocks living in
     * OTHER fibers' lists and sweep only their own, so foreign bits can
     * be stale-set here; a conservative minor that early-returns on a
     * stale old container orphans every young object reachable only
     * through it (proven: closures mid-suite swept while live). */
    bender_gc_epoch_next();
    depth = BENDER_RECURSION_GUARD;
    bender_vm.gc_mark_phase = 1;
    bender_vm.gc_minor_phase = 1;
    bender_mark_roots();
    bender_vm.gc_mark_phase = 0;
    if (getenv("BENDER_GC_VERIFY")) {
        if (bender_gc_verify_minor()) {
            fprintf(stderr, "gc-verify: minor mark incomplete, aborting\n");
            abort();
        }
    }
    bender_sweep_heaps(0);
    bender_vm.gc_minor_phase = 0;
    /* Age surviving young pages. Dirty flags deliberately persist until
     * the next major: a young referee needs several minors to age out,
     * so clearing per-minor would drop coverage while it is still young
     * (old container + young entry swept live). Stale-dirty pages cost
     * only extra traversal, never correctness. */
    BenderSlabState *ss = (BenderSlabState *) bender_vm.slab_state;
    if (NULL != ss) {
        for (int i = 0; i < BENDER_SLAB_NCLASSES; i++) {
            for (BenderSlabPage *p = ss->pages[i]; NULL != p; p = p->next) {
                if (p->age < BENDER_SLAB_OLD_AGE) {
                    p->age++;
                    /* Promotion barrier (Phase 2b-iii): edges stored while
                     * the page was young were never write-barriered (young
                     * pages are always traversed, so the barrier correctly
                     * early-outs). Dirty the page exactly once at the
                     * young->old transition so its first minor as an old
                     * page traverses those edges; stores made while old are
                     * covered by the regular bender_gc_barrier. */
                    if (p->age == BENDER_SLAB_OLD_AGE) p->dirty = 1;
                }
            }
        }
        bender_vm.gc_young_bytes = 0;
    }
}

/* Run garbage collection */
void bender_collect(void) {
#ifdef BENDER_GC_COUNT
    fprintf(stderr, "gc-count: minor=%ld major=%ld owned=%ld proc=%ld\n",
            g_n_minor, g_n_major, g_n_owned, g_n_proc);
    fflush(stderr);
#endif
    if (bender_vm.gc_suspend) return;
    BENDER_GC_WORK_TICK(work_gc_major);
#ifdef BENDER_GC_COUNT
    g_n_major++;
#endif
    bender_gc_epoch_next();
    depth = BENDER_RECURSION_GUARD;
    bender_vm.gc_mark_phase = 1;
    /* Pre-clear every listed block's epoch stamp before marking. This
     * restores the invariant bender_clear_all_reachable_bits was written
     * for (its comment says "full collects therefore pre-clear every
     * bit before marking") but which was never actually invoked.
     * Scoped collections (per-fiber/per-proc minors) mark foreign-listed
     * blocks through shared C roots (top_dyns, the proc registry, xfb,
     * the ev queues) and then sweep only their own lists, so those
     * foreign blocks carry stamps past the sweep that should have
     * cleared them. Left in place, a 15-collection epoch wrap can turn
     * such a stale stamp into a false bender_gc_reachable verdict:
     * bender_mark's early return then skips re-marking a LIVE block's
     * subtree and the sweep below frees its unmarked children while
     * reachable. O(listed blocks), the same order as the major. */
    bender_clear_all_reachable_bits();
    /* Try to prevent many major collections back to back.
     * A full collection will take O(bender_vm.block_count) time.
     * If we have a large heap, make sure our interval is not too
     * small so we won't make many collections over it. This is just a
     * heuristic for automatically changing the gc interval */
    if (bender_vm.block_count * 8 > bender_vm.gc_interval) {
        bender_vm.gc_interval = bender_vm.block_count * sizeof(BenderGCObject);
    }
    bender_mark_roots();
    bender_vm.gc_mark_phase = 0;
    bender_sweep();
    bender_vm.gc_major_live_blocks = bender_vm.block_count;
    bender_vm.next_collection = 0;
    /* NOTE (Phase 2a root discipline): scratch memory is deliberately NOT
     * reclaimed here. Scratch lifetimes belong to their C scopes (compile
     * buffers, vectors, table storage, ...), which routinely span Bender
     * allocations and therefore collections; freeing it per-collect
     * invalidates live scratch (use-after-free, then an "invalid
     * bender_sfree" abort). Panic-leaked scratch is still recovered at
     * shutdown in bender_clear_memory. */
}

/* Add a root value to the GC. This prevents the GC from removing a value
 * and all of its children. If gcroot is called on a value n times, unroot
 * must also be called n times to remove it as a gc root. */
void bender_gcroot(Bender root) {
#ifdef BENDER_ASAN_UAF_TRACE
    if (bender_checktype(root, BENDER_FIBER)) {
        BenderFiber *rf = bender_unwrap_fiber(root);
        extern int __asan_address_is_poisoned(void const volatile *addr);
        extern int backtrace(void **, int);
        extern void backtrace_symbols_fd(void *const *, int, int);
        int poisoned = __asan_address_is_poisoned(rf);
        int quarantined = 0;
        if (poisoned || quarantined) {
            char bbuf[128];
            int bl = snprintf(bbuf, sizeof bbuf,
                    "[gcroot-dead] fiber=%p poisoned=%d quarantined=%d "
                    "status=%d\n", (void *) rf, poisoned, quarantined,
                    (int) bender_fiber_status(rf));
            write(2, bbuf, (size_t) bl);
            void *bt[16];
            int bn = backtrace(bt, 16);
            backtrace_symbols_fd(bt, bn, 2);
        }
    }
#endif
    size_t newcount = bender_vm.root_count + 1;
    if (newcount > bender_vm.root_capacity) {
        size_t newcap = 2 * newcount;
        bender_vm.roots = bender_realloc(bender_vm.roots, sizeof(Bender) * newcap);
        if (NULL == bender_vm.roots) {
            BENDER_OUT_OF_MEMORY;
        }
        bender_vm.root_capacity = newcap;
    }
    bender_vm.roots[bender_vm.root_count] = root;
    bender_vm.root_count = newcount;
}

/* Identity equality for GC purposes */
static int bender_gc_idequals(Bender lhs, Bender rhs) {
    if (bender_type(lhs) != bender_type(rhs))
        return 0;
    switch (bender_type(lhs)) {
        case BENDER_BOOLEAN:
        case BENDER_NIL:
        case BENDER_NUMBER:
            /* These values don't really matter to the gc so returning 1 all the time is fine. */
            return 1;
        default:
            return bender_unwrap_pointer(lhs) == bender_unwrap_pointer(rhs);
    }
}

/* Scoped shadow roots for generated/C code (Phase 2b-i). Strictly LIFO:
 * snapshot with mark, discard with pop_to. Growth uses raw realloc so a
 * push itself can never trigger the collection it protects against. */
void bender_gcshadow_push(Bender x) {
    size_t newcount = bender_vm.shadow_count + 1;
    if (newcount > bender_vm.shadow_capacity) {
        size_t newcap = 2 * newcount;
        if (newcap < 8) newcap = 8;
        bender_vm.shadow_roots = bender_realloc(bender_vm.shadow_roots, sizeof(Bender) * newcap);
        if (NULL == bender_vm.shadow_roots) {
            BENDER_OUT_OF_MEMORY;
        }
        bender_vm.shadow_capacity = newcap;
    }
    bender_vm.shadow_roots[bender_vm.shadow_count] = x;
    bender_vm.shadow_count = newcount;
}

size_t bender_gcshadow_mark(void) {
    return bender_vm.shadow_count;
}

void bender_gcshadow_pop_to(size_t mark) {
    if (mark > bender_vm.shadow_count) {
        bender_panicf("gcshadow mark %d beyond stack top %d", (int) mark, (int) bender_vm.shadow_count);
    }
    bender_vm.shadow_count = mark;
}

/* Remove a root value from the GC. This allows the gc to potentially reclaim
 * a value and all its children. */
int bender_gcunroot(Bender root) {
    /* Search from top to bottom as access is most likely LIFO */
    for (Bender *v = bender_vm.roots + bender_vm.root_count; v > bender_vm.roots;) {
        v--;
        if (bender_gc_idequals(root, *v)) {
            *v = bender_vm.roots[--bender_vm.root_count];
            return 1;
        }
    }
    return 0;
}

/* Remove a root value from the GC. This sets the effective reference count to 0. */
int bender_gcunrootall(Bender root) {
    int ret = 0;
    /* Search from top to bottom as access is most likely LIFO */
    for (Bender *v = bender_vm.roots + bender_vm.root_count; v > bender_vm.roots;) {
        v--;
        if (bender_gc_idequals(root, *v)) {
            *v = bender_vm.roots[--bender_vm.root_count];
            ret = 1;
        }
    }
    return ret;
}

/* Free all allocated memory */
void bender_clear_memory(void) {
    /* Threaded teardowns run inline from here on: the loop is gone,
     * so routed posts would block forever on the dead self-pipe. */
    bender_vm.gc_teardown_inline = 1;
    /* Drain the lane-homed quarantine (BUG 2 density pinning): run
     * finalizers, poison, and free. These blocks are malloc'd and
     * unlinked from the GC heaps, so plain release is safe. */
    {
        BenderAbstractHead *hq = bender_vm.home_quarantine;
        bender_vm.home_quarantine = NULL;
        while (NULL != hq) {
            BenderAbstractHead *next = (BenderAbstractHead *) hq->gc.data.next;
            if (NULL != hq->type && NULL != hq->type->gc) {
                bender_assert(!hq->type->gc(hq->data, hq->size),
                        "finalizer failed");
            }
            hq->type = NULL;
            bender_free(hq);
            hq = next;
        }
    }
    /* Quarantined blocks are intact but dead: destroy them before the
     * registry teardown walk (their deinit tombstones purge their
     * entries, and freed threaded heads read poisoned under the
     * guards). */
    bender_quarantine_drain();
#ifdef BENDER_EV
    BenderKV *items = bender_vm.threaded_abstracts.data;
    for (int32_t i = 0; i < bender_vm.threaded_abstracts.capacity; i++) {
        if (bender_checktype(items[i].key, BENDER_ABSTRACT)) {
            void *abst = bender_unwrap_abstract(items[i].key);
            BenderAbstractHead *head = bender_abstract_head(abst);
            /* Stale entry over a freed (poisoned) abstract: the header
             * is gone (quarantine drain above, an earlier refcount
             * free, or a routed teardown callback). Nil the slot in
             * place so later table probes in this same teardown
             * (bender_deinit_block's tombstone put for another
             * abstract) never bender_equals against the dead key. */
            if (NULL == head->type) {
                items[i].key = bender_wrap_nil();
                items[i].value = bender_wrap_false();
                bender_vm.threaded_abstracts.deleted++;
                bender_vm.threaded_abstracts.count--;
                continue;
            }
            if (head->type->gcperthread) {
                bender_assert(!head->type->gcperthread(head->data, head->size), "per-thread finalizer failed");
            }
            bender_abstract_decref_maybe_free(abst);
            /* Tombstone in place (mirrors bender_sweep): the decref may
             * have freed the header (large-path heads hit real free()),
             * and a lingering key would be compared by any later
             * bender_table_put/remove in the blocks walk below --
             * use-after-free in bender_equals (value.c). Skip if the
             * decref path already tombstoned, so count bookkeeping is
             * not applied twice. */
            if (!bender_checktype(items[i].key, BENDER_NIL)) {
                items[i].key = bender_wrap_nil();
                items[i].value = bender_wrap_false();
                bender_vm.threaded_abstracts.deleted++;
                bender_vm.threaded_abstracts.count--;
            }
        }
    }
#endif
    /* P3.5b Slice A: drain per-fiber lists into the globals first
     * (fibers freed below then find empty lists, so the death
     * migration no-ops; no double-visit). */
    {
        BenderSlabState *ss = (BenderSlabState *) bender_vm.slab_state;
        if (NULL != ss) {
            size_t fi;
            for (fi = 0; fi < ss->fibers_count; fi++) {
                BenderFiber *f = ss->fibers[fi];
                int k;
                for (k = 0; k < 3; k++) {
                    BenderGCObject *fl = (BenderGCObject *) f->heap_lists[k];
                    if (NULL != fl) {
                        BenderGCObject *tail = fl;
                        while (NULL != tail->data.next)
                            tail = tail->data.next;
                        if (k == 0) {
                            tail->data.next = (BenderGCObject *) bender_vm.blocks;
                            bender_vm.blocks = (void *) fl;
                        } else if (k == 1) {
                            tail->data.next = (BenderGCObject *) bender_vm.string_blocks;
                            bender_vm.string_blocks = (void *) fl;
                        } else {
                            tail->data.next = (BenderGCObject *) bender_vm.weak_blocks;
                            bender_vm.weak_blocks = (void *) fl;
                        }
                        f->heap_lists[k] = NULL;
                    }
                }
            }
        }
    }
    BenderGCObject *current = bender_vm.blocks;
    while (NULL != current) {
        bender_deinit_block(current);
        BenderGCObject *next = current->data.next;
        bender_gc_release(current);
        current = next;
    }
    bender_vm.blocks = NULL;
    current = bender_vm.string_blocks;
    while (NULL != current) {
        bender_deinit_block(current);
        BenderGCObject *next = current->data.next;
        bender_gc_release(current);
        current = next;
    }
    bender_vm.string_blocks = NULL;
    /* With no live objects left, drop the slab pages themselves (frees
     * directory + freelists with them; fresh pages on next use). */
    bender_slab_free_pages();
    bender_vm.gc_major_live_blocks = 0;
    bender_free_all_scratch();
    bender_free(bender_vm.scratch_mem);
}

/* Primitives for suspending GC. */
int bender_gclock(void) {
    return bender_vm.gc_suspend++;
}
void bender_gcunlock(int handle) {
    bender_vm.gc_suspend = handle;
}

/* Scratch memory API
 * Scratch memory allocations do not need to be free (but optionally can be), and will be automatically cleaned
 * up in the next call to bender_collect. */

void *bender_smalloc(size_t size) {
    BenderScratch *s = bender_malloc(sizeof(BenderScratch) + size);
    if (NULL == s) {
        BENDER_OUT_OF_MEMORY;
    }
    s->finalize = NULL;
    if (bender_vm.scratch_len == bender_vm.scratch_cap) {
        size_t newcap = 2 * bender_vm.scratch_cap + 2;
        BenderScratch **newmem = (BenderScratch **) bender_realloc(bender_vm.scratch_mem, newcap * sizeof(BenderScratch));
        if (NULL == newmem) {
            BENDER_OUT_OF_MEMORY;
        }
        bender_vm.scratch_cap = newcap;
        bender_vm.scratch_mem = newmem;
    }
    bender_vm.scratch_mem[bender_vm.scratch_len++] = s;
    return (char *)(s->mem);
}

void *bender_scalloc(size_t nmemb, size_t size) {
    if (nmemb && size > SIZE_MAX / nmemb) {
        BENDER_OUT_OF_MEMORY;
    }
    size_t n = nmemb * size;
    void *p = bender_smalloc(n);
    memset(p, 0, n);
    return p;
}

void *bender_srealloc(void *mem, size_t size) {
    if (NULL == mem) return bender_smalloc(size);
    BenderScratch *s = bender_mem2scratch(mem);
    if (bender_vm.scratch_len) {
        for (size_t i = bender_vm.scratch_len - 1; ; i--) {
            if (bender_vm.scratch_mem[i] == s) {
                BenderScratch *news = bender_realloc(s, size + sizeof(BenderScratch));
                if (NULL == news) {
                    BENDER_OUT_OF_MEMORY;
                }
                bender_vm.scratch_mem[i] = news;
                return (char *)(news->mem);
            }
            if (i == 0) break;
        }
    }
    BENDER_EXIT("invalid bender_srealloc");
}

void bender_sfinalizer(void *mem, BenderScratchFinalizer finalizer) {
    BenderScratch *s = bender_mem2scratch(mem);
    s->finalize = finalizer;
}

void bender_sfree(void *mem) {
    if (NULL == mem) return;
    BenderScratch *s = bender_mem2scratch(mem);
    if (bender_vm.scratch_len) {
        for (size_t i = bender_vm.scratch_len - 1; ; i--) {
            if (bender_vm.scratch_mem[i] == s) {
                bender_vm.scratch_mem[i] = bender_vm.scratch_mem[--bender_vm.scratch_len];
                free_one_scratch(s);
                return;
            }
            if (i == 0) break;
        }
    }
    BENDER_EXIT("invalid bender_sfree");
}
