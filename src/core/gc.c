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

#ifndef JANET_AMALG
#include "features.h"
#include <janet.h>
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

/* Helpers for marking the various gc types */
static void janet_mark_funcenv(JanetFuncEnv *env);
static void janet_mark_funcdef(JanetFuncDef *def);
static void janet_mark_function(JanetFunction *func);
static void janet_mark_array(JanetArray *array);
static void janet_mark_table(JanetTable *table);
static void janet_mark_struct(const JanetKV *st);
static void janet_mark_tuple(const Janet *tuple);
static void janet_mark_buffer(JanetBuffer *buffer);
static void janet_mark_string(const uint8_t *str);
static void janet_mark_fiber(JanetFiber *fiber);
static void janet_mark_abstract(void *adata);

/* Local state that is only temporary for gc */
static JANET_THREAD_LOCAL uint32_t depth = JANET_RECURSION_GUARD;
#ifdef JANET_GC_COUNT
static long g_n_minor = 0, g_n_major = 0, g_n_owned = 0, g_n_proc = 0;
#endif

/* Generational gate (Phase 2b-iii policy), defined with the slab code
 * below; used by the table/array markers. */
static int janet_gc_minor_skip(JanetGCObject *mem);
/* Object old for generational purposes (slab page age, or malloc-large);
 * defined with the slab code below. */
static int janet_slab_obj_old(void *ptr);
/* Step 1.3 owner for sweep filtering (0 = shared); defined below. */
static uint32_t janet_slab_obj_owner(void *ptr);
/* Reset generational slab state after a full sweep; defined below. */
static void janet_slab_major_reset(void);
/* Step 1.3 census removal at sweep-free; defined with the slab code. */
static void janet_census_remove(JanetFiber *f);
static void janet_sweep_heaps(int walk_fibers);
static void janet_sweep_fiber_owned(JanetFiber *f);

/* Hint to the GC that we may need to collect */
void janet_gcpressure(size_t s) {
    janet_vm.next_collection += s;
}

static void janet_clear_all_reachable_bits(void);

/* Phase 1.1 epoch mark: gc.c-local 4-bit epoch in flags 11-14. Stale
 * bits self-invalidate; the sweep zeroes survivors, so the wrap can
 * only cause one-cycle-late frees — no wipe, no wrap-clear. */
static JANET_THREAD_LOCAL int32_t janet_gc_epoch = 0;

#undef janet_gc_mark
#undef janet_gc_reachable
#define janet_gc_mark(m) (janet_gc_header(m)->flags = \
    ((janet_gc_header(m)->flags & ~JANET_MEM_EPOCH_MASK) \
     | (janet_gc_epoch << JANET_MEM_EPOCH_SHIFT)))
#define janet_gc_reachable(m) \
    (((janet_gc_header(m)->flags & JANET_MEM_EPOCH_MASK) >> JANET_MEM_EPOCH_SHIFT) \
     == janet_gc_epoch)

static void janet_gc_epoch_next(void) {
    janet_gc_epoch = (janet_gc_epoch % 15) + 1;
}

/* Mark a value */
/* Mark a value */
void janet_mark(Janet x) {
    if (depth) {
        depth--;
        switch (janet_type(x)) {
            default:
                break;
            case JANET_STRING:
            case JANET_KEYWORD:
            case JANET_SYMBOL:
                janet_mark_string(janet_unwrap_string(x));
                break;
            case JANET_FUNCTION:
                janet_mark_function(janet_unwrap_function(x));
                break;
            case JANET_ARRAY:
                janet_mark_array(janet_unwrap_array(x));
                break;
            case JANET_TABLE:
                janet_mark_table(janet_unwrap_table(x));
                break;
            case JANET_STRUCT:
                janet_mark_struct(janet_unwrap_struct(x));
                break;
            case JANET_TUPLE:
                janet_mark_tuple(janet_unwrap_tuple(x));
                break;
            case JANET_BUFFER:
                janet_mark_buffer(janet_unwrap_buffer(x));
                break;
            case JANET_FIBER:
                janet_mark_fiber(janet_unwrap_fiber(x));
                break;
            case JANET_ABSTRACT:
                janet_mark_abstract(janet_unwrap_abstract(x));
                break;
        }
        depth++;
    } else {
        janet_gcroot(x);
    }
}

static void janet_mark_string(const uint8_t *str) {
    janet_gc_mark(janet_string_head(str));
}

static void janet_mark_buffer(JanetBuffer *buffer) {
    janet_gc_mark(buffer);
}

static void janet_mark_abstract(void *adata) {
#ifdef JANET_ASAN_UAF_TRACE
    if ((uintptr_t) adata < 4096) {
        char bbuf[128];
        int bl = snprintf(bbuf, sizeof bbuf,
                "[bogus-abstract] adata=%p ra=%p\n", adata,
                __builtin_return_address(0));
        write(2, bbuf, (size_t) bl);
        abort();
    }
    if (NULL == janet_abstract_head(adata)->type) {
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
#ifdef JANET_EV
    /* Check if abstract type is a threaded abstract type. If it is, marking means
     * updating the threaded_abstract table. */
    if ((janet_abstract_head(adata)->gc.flags & JANET_MEM_TYPEBITS) == JANET_MEMORY_THREADED_ABSTRACT) {
        /* A home major mark authorizes teardown deferral; only an
         * unvisited entry at the end of a complete home major revokes it.
         * Foreign, minor, and scoped traversals cannot change the grant.
         * The four-bit GC epoch is not a durable lifetime verdict. */
        if (janet_vm.gc_mark_phase && !janet_vm.gc_minor_phase &&
                janet_abstract_head(adata)->home_vm == (void *) &janet_vm)
            janet_abstract_head(adata)->home_gc_authorized = 1;
        /* Visit-flag semantics: the table's value is this cycle's
         * "already visited" marker (the sweep resets every entry to
         * false). First visit this cycle: flip it and run gcmark so
         * the abstract's INTERNALS — channel items, pending-entry
         * values — are marked exactly like non-threaded abstracts'.
         * Without this, values buffered inside a channel were
         * collectable at any GC boundary, surviving only while a
         * sender-side reference happened to keep them alive. */
        Janet kv = janet_wrap_abstract(adata);
        if (!janet_truthy(janet_table_get(&janet_vm.threaded_abstracts, kv))) {
            janet_table_put(&janet_vm.threaded_abstracts, kv, janet_wrap_true());
            /* Stale heap references can outlive the abstract: freed heads
             * are poisoned (type == NULL), nothing left to mark, and
             * reading the freed header is unsound. */
            if (janet_abstract_head(adata)->type && janet_abstract_head(adata)->type->gcmark) {
                janet_abstract_head(adata)->type->gcmark(adata, janet_abstract_size(adata));
            }
        }
        return;
    }
#endif
    if (janet_gc_reachable(janet_abstract_head(adata)))
        return;
    janet_gc_mark(janet_abstract_head(adata));
    if (janet_abstract_head(adata)->type->gcmark) {
        janet_abstract_head(adata)->type->gcmark(adata, janet_abstract_size(adata));
    }
}

/* Mark a bunch of items in memory */
static void janet_mark_many(const Janet *values, int32_t n) {
    if (values == NULL)
        return;
    const Janet *end = values + n;
    while (values < end) {
        janet_mark(*values);
        values += 1;
    }
}

/* Mark a bunch of key values items in memory */
static void janet_mark_keys(const JanetKV *kvs, int32_t n) {
    const JanetKV *end = kvs + n;
    while (kvs < end) {
        janet_mark(kvs->key);
        kvs++;
    }
}

/* Mark a bunch of key values items in memory */
static void janet_mark_values(const JanetKV *kvs, int32_t n) {
    const JanetKV *end = kvs + n;
    while (kvs < end) {
        janet_mark(kvs->value);
        kvs++;
    }
}

/* Mark a bunch of key values items in memory */
static void janet_mark_kvs(const JanetKV *kvs, int32_t n) {
    const JanetKV *end = kvs + n;
    while (kvs < end) {
        janet_mark(kvs->key);
        janet_mark(kvs->value);
        kvs++;
    }
}

static void janet_mark_array(JanetArray *array) {
    if (janet_gc_reachable(array))
        return;
    if (janet_gc_minor_skip((JanetGCObject *) array))
        return;
    janet_gc_mark(array);
    if (janet_gc_type((JanetGCObject *) array) == JANET_MEMORY_ARRAY) {
        janet_mark_many(array->data, array->count);
    }
}

static void janet_mark_table(JanetTable *table) {
recur: /* Manual tail recursion */
    if (janet_gc_reachable(table))
        return;
    if (janet_gc_minor_skip((JanetGCObject *) table))
        return;
    janet_gc_mark(table);
    enum JanetMemoryType memtype = janet_gc_type(table);
    if (memtype == JANET_MEMORY_TABLE_WEAKK) {
        janet_mark_values(table->data, table->capacity);
    } else if (memtype == JANET_MEMORY_TABLE_WEAKV) {
        janet_mark_keys(table->data, table->capacity);
    } else if (memtype == JANET_MEMORY_TABLE) {
        janet_mark_kvs(table->data, table->capacity);
    }
    /* do nothing for JANET_MEMORY_TABLE_WEAKKV */
    if (table->proto) {
        table = table->proto;
        goto recur;
    }
}

static void janet_mark_struct(const JanetKV *st) {
recur:
    if (janet_gc_reachable(janet_struct_head(st)))
        return;
    janet_gc_mark(janet_struct_head(st));
    janet_mark_kvs(st, janet_struct_capacity(st));
    st = janet_struct_proto(st);
    if (st) goto recur;
}

static void janet_mark_tuple(const Janet *tuple) {
    if (janet_gc_reachable(janet_tuple_head(tuple)))
        return;
    janet_gc_mark(janet_tuple_head(tuple));
    janet_mark_many(tuple, janet_tuple_length(tuple));
}

/* Helper to mark function environments */
static void janet_mark_funcenv(JanetFuncEnv *env) {
    if (janet_gc_reachable(env))
        return;
    janet_gc_mark(env);
    /* If closure env references a dead fiber, we can just copy out the stack frame we need so
     * we don't need to keep around the whole dead fiber. */
    janet_env_maybe_detach(env);
    if (env->offset > 0) {
        /* On stack */
        janet_mark_fiber(env->as.fiber);
    } else {
        /* Not on stack */
        janet_mark_many(env->as.values, env->length);
    }
}

/* GC helper to mark a FuncDef */
static void janet_mark_funcdef(JanetFuncDef *def) {
    int32_t i;
    if (janet_gc_reachable(def))
        return;
    janet_gc_mark(def);
    janet_mark_many(def->constants, def->constants_length);
    for (i = 0; i < def->defs_length; ++i) {
        janet_mark_funcdef(def->defs[i]);
    }
    if (def->source)
        janet_mark_string(def->source);
    if (def->name)
        janet_mark_string(def->name);
    if (def->symbolmap) {
        for (int i = 0; i < def->symbolmap_length; i++) {
            janet_mark_string(def->symbolmap[i].symbol);
        }
    }

}

static void janet_mark_function(JanetFunction *func) {
    int32_t i;
    int32_t numenvs;
    if (janet_gc_reachable(func))
        return;
    janet_gc_mark(func);
    if (NULL != func->def) {
        /* this should always be true, except if function is only partially constructed */
        numenvs = func->def->environments_length;
        for (i = 0; i < numenvs; ++i) {
            janet_mark_funcenv(func->envs[i]);
        }
        janet_mark_funcdef(func->def);
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
 * Runs inside janet_collect, where the compiler stack only ever holds
 * live C-stack state. */
static void janet_mark_compiler(JanetCompiler *c) {
    if (c->env) janet_mark_table(c->env);
    if (c->lints) janet_mark_array(c->lints);
    if (c->result.funcdef) janet_mark_funcdef(c->result.funcdef);
    for (JanetScope *s = c->scope; s; s = s->parent) {
        if (s->consts) {
            janet_mark_many(s->consts, janet_v_count(s->consts));
        }
        if (s->syms) {
            int32_t n = janet_v_count(s->syms);
            for (int32_t i = 0; i < n; i++) {
                janet_mark(s->syms[i].slot.constant);
                if (s->syms[i].sym) {
                    janet_mark(janet_wrap_string(s->syms[i].sym));
                }
                if (s->syms[i].sym2 && s->syms[i].sym2 != s->syms[i].sym) {
                    janet_mark(janet_wrap_string(s->syms[i].sym2));
                }
            }
        }
        if (s->defs) {
            int32_t n = janet_v_count(s->defs);
            for (int32_t i = 0; i < n; i++) {
                if (s->defs[i]) janet_mark_funcdef(s->defs[i]);
            }
        }
    }
}

static void janet_mark_fiber(JanetFiber *fiber) {
    int32_t i, j;
    JanetStackFrame *frame;
recur:
    if (janet_gc_reachable(fiber))
        return;
#ifdef JANET_ASAN_UAF_TRACE
    extern int __asan_address_is_poisoned(void const volatile *addr);
    extern void __sanitizer_print_stack_trace(void);
    if (__asan_address_is_poisoned(fiber)) {
        /* Crash-safe report: write(2) only -- janet_eprintf allocates,
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
    janet_gc_mark(fiber);

    /* P3.5: proc fibers pin their proc (and procs pin their fibers
     * in procat_mark), so neither pointer dangles while the other
     * lives; the cycle collects together. */
    if (NULL != fiber->proc) {
        janet_mark(janet_wrap_abstract(fiber->proc));
    }
#ifdef JANET_ASAN_UAF_TRACE
    {
        Janet vals[4] = {fiber->last_value, fiber->in_value,
                         fiber->out_payload, fiber->interrupt_value};
        const char *names[4] = {"last", "in", "out", "interrupt"};
        for (int k = 0; k < 4; k++) {
            if (janet_checktype(vals[k], JANET_ABSTRACT)) {
                void *ad = janet_unwrap_abstract(vals[k]);
                if (NULL == janet_abstract_head(ad)->type)
                    fprintf(stderr, "[fiber-stale-field] fiber=%p field=%s adata=%p status=%d\n",
                            (void *) fiber, names[k], ad, (int) janet_fiber_status(fiber));
            }
        }
    }
#endif
    janet_mark(fiber->last_value);
    /* Values in flight across a context switch. The resume argument and
     * the last result live in the fiber struct (not in fiber->data), so
     * they must be marked explicitly or a collection between suspend and
     * resume could free them. The VM clears consumed input when parking
     * or completing, rather than hiding a potentially live root here. */
    janet_mark(fiber->in_value);
    janet_mark(fiber->out_payload);
#ifdef JANET_EV
    janet_mark(fiber->interrupt_value);
#endif
    /* Phase 1.1: the resumer link keeps a suspended fiber's resume party
     * alive across collections while swapped out. */
    if (fiber->resume_fiber) {
        janet_mark(janet_wrap_fiber(fiber->resume_fiber));
    }

    /* BUG 2: a completed fiber's stack frames are stale. The
     * completion minor frees the young garbage they reference
     * (frame->func/env), while the fiber struct itself stays
     * reachable until its own sweep -- a later mark re-walking the
     * frames read freed memory (ASAN: janet_mark_function
     * use-after-free at density; the crash's flip side was
     * "janet_hash on a freed abstract"). Nothing resumes a fiber in
     * a terminal status, so only resumable fibers walk their
     * frames; last_value/out_payload/env above stay marked for
     * everyone. */
    {
        JanetFiberStatus fst = janet_fiber_status(fiber);
        int walk_frames = !(JANET_STATUS_DEAD == fst ||
                            JANET_STATUS_ERROR == fst);
        if (walk_frames) {
    /* Mark values on the argument stack */
    janet_mark_many(fiber->data + fiber->stackstart,
                    fiber->stacktop - fiber->stackstart);

#ifdef JANET_ASAN_UAF_TRACE
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
    j = fiber->stackstart - JANET_FRAME_SIZE;
    while (i > 0) {
        frame = (JanetStackFrame *)(fiber->data + i - JANET_FRAME_SIZE);
        if (NULL != frame->func) {
#ifdef JANET_ASAN_UAF_TRACE
            if (__asan_address_is_poisoned(frame->func)) {
                __sanitizer_print_stack_trace();
            }
#endif
            janet_mark_function(frame->func);
        }
        if (NULL != frame->env)
            janet_mark_funcenv(frame->env);
        /* Mark all values in the stack frame */
        janet_mark_many(fiber->data + i, j - i);
        j = i - JANET_FRAME_SIZE;
        i = frame->prevframe;
    }
        }
    }

    if (fiber->env)
        janet_mark_table(fiber->env);

#ifdef JANET_EV
    if (fiber->supervisor_channel) {
        janet_mark_abstract(fiber->supervisor_channel);
    }
    if (fiber->ev_stream) {
        janet_mark_abstract(fiber->ev_stream);
    }
    if (fiber->ev_callback) {
        fiber->ev_callback(fiber, JANET_ASYNC_EVENT_MARK);
    }
    /* Pending-registration reverse edge: parked fibers pin their
     * channels (see janet_ev_chanpends_mark). */
    janet_ev_chanpends_mark(fiber);
#endif

    /* Supervisor channel: fibers hold their sup channel outside any
     * Janet-visible value once wired (proc spawn, ev/go and tasklet
     * wiring set it directly). Threaded abstracts are refcounted, and
     * janet_sweep's registry pass decrefs every unvisited entry each
     * full collect — an unmarked sup therefore dies under a parked or
     * running fiber, and freed channel memory is reused as Janet
     * values (the premature-free UAF family: clobbered closure defs,
     * registry slots, ASAN run_vm read of a freed 132-byte channel).
     * Marking it with the fiber pins it for every collector: visited
     * entries are never decref'd, and the pass reclaims it on the
     * first full collect after the fiber dies. */
    if (NULL != fiber->supervisor_channel)
        janet_mark(janet_wrap_abstract(fiber->supervisor_channel));

    /* Explicit tail recursion */
    if (fiber->child) {
        fiber = fiber->child;
        goto recur;
    }
}

/* Deinitialize a block of memory */
static void janet_deinit_block(JanetGCObject *mem) {
    switch (mem->flags & JANET_MEM_TYPEBITS) {
        default:
        case JANET_MEMORY_FUNCTION:
            break; /* Do nothing for non gc types */
        case JANET_MEMORY_SYMBOL:
            janet_symbol_deinit(((JanetStringHead *) mem)->data);
            break;
        case JANET_MEMORY_ARRAY:
        case JANET_MEMORY_ARRAY_WEAK:
            janet_free(((JanetArray *) mem)->data);
            break;
        case JANET_MEMORY_TABLE:
        case JANET_MEMORY_TABLE_WEAKK:
        case JANET_MEMORY_TABLE_WEAKV:
        case JANET_MEMORY_TABLE_WEAKKV:
            janet_free(((JanetTable *) mem)->data);
            break;
        case JANET_MEMORY_FIBER: {
            JanetFiber *f = (JanetFiber *)mem;
#ifdef JANET_EV
            if (f->ev_state && !(f->flags & JANET_FIBER_EV_FLAG_IN_FLIGHT)) {
                janet_ev_dec_refcount();
                janet_free(f->ev_state);
            } else if (f->gc.flags & JANET_FIBER_EV_GCFLAG_SUSPENDED) {
                janet_ev_dec_refcount();
            }
            /* Channel-pending registrations (unlink-on-free): entries
             * still queued get flagged dead in their nodes — consumer
             * paths reclaim them lazily and never dereference this
             * fiber again — and unqueued nodes are freed now. */
            janet_ev_chanpends_deinit(f);
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
                    JanetGCObject *fl = (JanetGCObject *) f->heap_lists[khi];
                    if (NULL != fl) {
                        JanetGCObject *tail = fl;
                        while (NULL != tail->data.next)
                            tail = tail->data.next;
                        tail->data.next = (JanetGCObject *) janet_vm.sweep_pending[khi];
                        janet_vm.sweep_pending[khi] = (void *) fl;
                        f->heap_lists[khi] = NULL;
                    }
                }
            }
            /* Step 1.3: leave the live-fiber census before release. */
            janet_census_remove(f);
#ifdef JANET_ASAN_UAF_TRACE
            {
                extern int backtrace(void **, int);
                extern void backtrace_symbols_fd(void *const *, int, int);
                /* Is this fiber still in vm.roots while we destroy it?
                 * Yes => mark/sweep disagreement (a root entry that
                 * marking never honored). No => the root entry that
                 * later dangles was written after this free. */
                size_t ri;
                int rooted = 0;
                for (ri = 0; ri < janet_vm.root_count; ri++) {
                    Janet rv = janet_vm.roots[ri];
                    if (janet_checktype(rv, JANET_FIBER) &&
                            janet_unwrap_fiber(rv) == f) {
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
            janet_free(f->data);
            janet_fiber_native_stack_free(f);
        }
        break;
        case JANET_MEMORY_BUFFER:
            janet_buffer_deinit((JanetBuffer *) mem);
            break;
        case JANET_MEMORY_ABSTRACT: {
            JanetAbstractHead *head = (JanetAbstractHead *)mem;
            /* Poisoned (type == NULL) means the abstract was already
             * finalized and released by the refcount path: skip both the
             * tombstone and the finalizers, storage drops below. */
            if (NULL == head->type) break;
            if (head->gc.flags & JANET_MEMORY_THREADED_ABSTRACT) {
                /* D3: tombstone at death. Scoped sweeps free channels
                 * between full collects, leaving registry entries whose
                 * header is freed memory; the registry's raw-array walks
                 * (janet_sweep's unvisited pass, clear_memory's teardown)
                 * then read freed headers. janet_table_remove only nils
                 * slots in place (no rehash), so iteration stays valid. */
                janet_table_put(&janet_vm.threaded_abstracts,
                                janet_wrap_abstract((void *) head->data),
                                janet_wrap_nil());
            }
            if (head->gc.flags & JANET_MEMORY_THREADED_ABSTRACT) {
                /* Single-shot teardown: the home-routed callback may
                 * have torn it down (or been posted and about to);
                 * only one path runs the hooks (BUG 2 double-deinit). */
                if (1 != janet_atomic_inc(&head->teardown_started)) break;
            }
            if (head->type->gcperthread) {
                janet_assert(!head->type->gcperthread(head->data, head->size), "per-thread finalizer failed");
            }
            if (head->type->gc) {
                janet_assert(!head->type->gc(head->data, head->size), "finalizer failed");
            }
        }
        break;
        case JANET_MEMORY_FUNCENV: {
            JanetFuncEnv *env = (JanetFuncEnv *)mem;
            if (0 == env->offset)
                janet_free(env->as.values);
        }
        break;
        case JANET_MEMORY_FUNCDEF: {
            JanetFuncDef *def = (JanetFuncDef *)mem;
            /* TODO - get this all with one alloc and one free */
            janet_free(def->defs);
            janet_free(def->environments);
            janet_free(def->constants);
            janet_free(def->bytecode);
            janet_free(def->sourcemap);
            janet_free(def->closure_bitset);
            janet_free(def->symbolmap);
        }
        break;
    }
}

/* Check that a value x has been visited in the mark phase */
static int janet_check_liveref(Janet x) {
    switch (janet_type(x)) {
        default:
            return 1;
        case JANET_ARRAY:
        case JANET_TABLE:
        case JANET_FUNCTION:
        case JANET_BUFFER:
        case JANET_FIBER: {
            int r = janet_gc_reachable(janet_unwrap_pointer(x));
            if (getenv("JANET_ROOT_TRACE"))
                fprintf(stderr, "[LIVEREF] fib=%p flags=%x ep=%d cur=%d live=%d\n",
                        janet_unwrap_pointer(x),
                        janet_gc_header(janet_unwrap_pointer(x))->flags,
                        (int) ((janet_gc_header(janet_unwrap_pointer(x))->flags
                                & JANET_MEM_EPOCH_MASK) >> JANET_MEM_EPOCH_SHIFT),
                        janet_gc_epoch, r);
            return r;
        }
        case JANET_STRING:
        case JANET_SYMBOL:
        case JANET_KEYWORD:
            return janet_gc_reachable(janet_string_head(janet_unwrap_string(x)));
        case JANET_ABSTRACT:
            return janet_gc_reachable(janet_abstract_head(janet_unwrap_abstract(x)));
        case JANET_TUPLE:
            return janet_gc_reachable(janet_tuple_head(janet_unwrap_tuple(x)));
        case JANET_STRUCT:
            return janet_gc_reachable(janet_struct_head(janet_unwrap_struct(x)));
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
static void janet_sweep_list(JanetGCObject **head, int heap_kind, int is_minor,
                             uint32_t only_owner) {
    JanetGCObject *previous = NULL;
    JanetGCObject *current = *head;
    while (NULL != current) {
        JanetGCObject *next = current->data.next;
        if (heap_kind == 0) {
            enum JanetMemoryType type = janet_gc_type(current);
            janet_assert(type != JANET_MEMORY_STRING && type != JANET_MEMORY_SYMBOL,
                         "string block on main heap");
        } else if (heap_kind == 1) {
            enum JanetMemoryType type = janet_gc_type(current);
            janet_assert(type == JANET_MEMORY_STRING || type == JANET_MEMORY_SYMBOL,
                         "non-string block on string heap");
        }
        /* heap_kind 2 (weak heap) carries no assert; weak types live there. */
        if (current->flags & JANET_MEM_DISABLED) {
            previous = current;
        } else if (janet_gc_reachable(current)) {
            /* Zero the survivor's epoch: the field's only writer is the
             * stamp, so zeroed = unreachable in every future cycle —
             * the wrap never resurrects, no wipe needed. */
            current->flags &= ~JANET_MEM_EPOCH_MASK;
            previous = current;
        } else if ((!is_minor || !janet_slab_obj_old(current)) &&
                (0 == only_owner || janet_slab_obj_owner(current) == only_owner) &&
                !(is_minor && JANET_MEMORY_FIBER == janet_gc_type(current))) {
            janet_vm.block_count--;
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
                janet_deinit_block(current);
                janet_gc_release(current);
            } else {
                current->data.next = janet_vm.gc_quarantine[heap_kind];
                janet_vm.gc_quarantine[heap_kind] = current;
                janet_vm.gc_quarantine_count++;
            }
        } else {
            previous = current;
        }
        current = next;
    }
}

static void janet_sweep_weak_drop_list(JanetGCObject *current);

/* Phase 1.1: destroy the quarantined blocks. Their headers were intact
 * through the last weak-drop; after this they are gone (deinit'd and
 * released; the decref path poisons type — 3dbb2e0b — so any late raw
 * read is guarded). */
static void janet_quarantine_drain(void) {
    int kind;
    for (kind = 0; kind < 3; kind++) {
        JanetGCObject *cur = janet_vm.gc_quarantine[kind];
        janet_vm.gc_quarantine[kind] = NULL;
        while (NULL != cur) {
            JanetGCObject *next = cur->data.next;
            janet_deinit_block(cur);
            janet_gc_release(cur);
            cur = next;
        }
    }
    janet_vm.gc_quarantine_count = 0;
}

/* Cap-drain: minors quarantine without running the weak-drop, so the
 * quarantine grows between full collects. When it passes this many
 * blocks, drain early — the drained headers are deinit'd+poisoned
 * (3dbb2e0b), and the next weak-drop's guarded reads treat them as
 * dead, which is the correct verdict for quarantined blocks. */
#define JANET_QUARANTINE_CAP 200000
static void janet_quarantine_maybe_drain(void) {
    if (janet_vm.gc_quarantine_count > JANET_QUARANTINE_CAP) {
        /* Judge weak entries BEFORE releasing the quarantined blocks:
         * an early drain without a weak-drop lets recycled slots alias
         * dead referents (check_liveref reads the new occupant's
         * epoch — suite-ev2 count = 100). The epoch mark makes the
         * weak-drop valid at any point in any cycle. */
        janet_sweep_weak_drop();
        janet_quarantine_drain();
    }
}

void janet_sweep(void) {
    janet_sweep_weak_drop();
    janet_sweep_heaps(1);

#ifdef JANET_EV
    /* Sweep threaded abstract types for references to decrement */
    JanetKV *items = janet_vm.threaded_abstracts.data;
    for (int32_t i = 0; i < janet_vm.threaded_abstracts.capacity; i++) {
        if (janet_checktype(items[i].key, JANET_ABSTRACT)) {
            /* Belt-and-braces for the tombstone-at-death hook: an entry
             * whose abstract died without tombstoning this VM's registry
             * (cross-thread free) reads a poisoned (type == NULL) header.
             * Purge it in place instead of dereferencing freed memory. */
            if (NULL == janet_abstract_head(janet_unwrap_abstract(items[i].key))->type) {
                items[i].key = janet_wrap_nil();
                items[i].value = janet_wrap_false();
                janet_vm.threaded_abstracts.deleted++;
                janet_vm.threaded_abstracts.count--;
                continue;
            }

            /* If item was not visited during the mark phase, then this
             * abstract type isn't present in the heap and needs its refcount
             * decremented, and shouuld be removed from table. If the refcount is
             * then 0, the item will be collected. This ensures that only one interpreter
             * will clean up the threaded abstract. */

            /* If not visited... */
            if (!janet_truthy(items[i].value)) {
                void *abst = janet_unwrap_abstract(items[i].key);
                JanetAbstractHead *head = janet_abstract_head(abst);
                /* This unvisited entry is the home VM's completed major
                 * verdict, not a minor/scoped or foreign VM traversal.
                 * Revoke before dropping the registry claim so a queued
                 * zero-ref callback cannot rearm an unreachable object. */
                if (head->home_vm == (void *) &janet_vm) {
                    head->home_gc_authorized = 0;
                }
                if (head->type->gcperthread) {
                    janet_assert(!head->type->gcperthread(head->data, head->size), "per-thread finalizer failed");
                }
                janet_abstract_decref_maybe_free(abst);

                /* Tombstone in place — skip if the decref path already
                 * tombstoned it (abstract.c's refcount free tombstones
                 * with its own bookkeeping; counting twice underflows
                 * count and corrupts later registry operations). */
                if (!janet_checktype(items[i].key, JANET_NIL)) {
                    items[i].key = janet_wrap_nil();
                    items[i].value = janet_wrap_false();
                    janet_vm.threaded_abstracts.deleted++;
                    janet_vm.threaded_abstracts.count--;
                }
            }

            /* Reset for next sweep */
            items[i].value = janet_wrap_false();
        }
    }
#endif
    /* Quarantine drain: the weak-drop above has judged every entry
     * against intact headers; the quarantined blocks are now truly
     * unreferenced and can be destroyed. */
    janet_quarantine_drain();
    /* A full sweep leaves every survivor old: reset all page ages and
     * merge the old freelists back so old slots become reusable young
     * slots (conservative restart for the next minor cycle). */
    janet_slab_major_reset();
}

/* Allocate some memory that is tracked for garbage collection */
/* Phase 2b-iii slab allocator, generational (slab storage model).
 * Same lists, same deinit -- only the backing store changes from
 * one-malloc-per-object to carved pages with free lists. Objects larger
 * than the biggest class still use malloc (large path, always treated as
 * old). Freelist links overlay JanetGCObject.data.next: free slots are
 * not live objects. Pages are 16 KiB aligned so any slot address maps to
 * its page header with a mask; a live-page directory (binary searched)
 * plus a magic + bounds check keeps the mapping exact (never confused by
 * malloc'd blocks, never reads unmapped memory). All growth uses raw
 * allocation so slab paths never trigger the collections they serve.
 * Per-VM state (see JanetVM.slab_state).
 *
 * Generations are page-granular: a page's age counts survived minors
 * (0-1 young, 2+ old). This is sound with pointer-identity reasoning --
 * objects never move, and young allocations only ever land on young
 * pages, so a young page's occupants are all young (an old object on a
 * young page would have had to move there). The reverse is not true
 * (old pages freely hold young garbage until swept), which is why sweep
 * distinguishes per object, not per page. See janet_gc_barrier and
 * janet_collect_minor. */

#define JANET_SLAB_PAGE_SIZE 16384
#define JANET_SLAB_PAGE_MASK (~(uintptr_t)0x3FFF)
#define JANET_SLAB_MAGIC 0x4A534C41u
#define JANET_SLAB_OLD_AGE 2

/* Bytes of young allocation between minor collections. */
#define JANET_MINOR_THRESHOLD (256 * 1024)

static const size_t janet_slab_classes[JANET_SLAB_NCLASSES] =
    {32, 64, 96, 128, 192, 256, 384, 512};

/* Fiber nursery cursors pair with the class table above. */
_Static_assert(JANET_SLAB_NCLASSES == 8,
    "nursery_cur literal in JanetFiber must match class count");

typedef struct JanetSlabPage {
    struct JanetSlabPage *next;
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
} JanetSlabPage;

#define JANET_SLAB_USABLE (JANET_SLAB_PAGE_SIZE - sizeof(JanetSlabPage))
#define JANET_SLAB_SLOTS(p) ((void *)((char *)(p) + sizeof(JanetSlabPage)))

typedef struct {
    JanetSlabPage *pages[JANET_SLAB_NCLASSES];
    JanetSlabPage *cur[JANET_SLAB_NCLASSES];
    JanetGCObject *young_free[JANET_SLAB_NCLASSES];
    JanetGCObject *old_free[JANET_SLAB_NCLASSES];
    JanetGCObject *strict_free[JANET_SLAB_NCLASSES];
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
    JanetSlabPage **dir;
    size_t dir_count;
    size_t dir_cap;
    /* Step 1.2 cross-fiber remembered set: containers whose stores
     * crossed owner ids (see janet_gc_barrier). Plain Janet values
     * (rooted via mark_roots); duplicates tolerated, pruned in 1.3.
     * Capped: past the cap recording stops (stuck=1) rather than
     * growing without bound — missing edges only matter once
     * per-fiber minors consume the set (1.3), so failing open here is
     * today's behavior exactly. */
    Janet *xfb;
    size_t xfb_count;
    size_t xfb_cap;
    int xfb_stuck;
    /* Step 1.3 live-fiber census (all fibers with live memory,
     * reachable or not): per-fiber minors mark every census fiber so
     * no unreaped referrer can dangle (an unreached foreign-old fiber
     * holding target-young would otherwise). Appended at birth
     * (alloc + unmarshal), swap-removed at free, reset at clear.
     * Over-marking (dead fibers) only floats garbage until major. */
    JanetFiber **fibers;
    size_t fibers_count;
    size_t fibers_cap;
} JanetSlabState;

static JanetSlabState *janet_slab_state(void) {
    JanetSlabState *ss = (JanetSlabState *) janet_vm.slab_state;
    if (NULL == ss) {
        ss = janet_malloc(sizeof(JanetSlabState));
        if (NULL == ss) {
            JANET_OUT_OF_MEMORY;
        }
        memset(ss, 0, sizeof(JanetSlabState));
        janet_vm.slab_state = ss;
    }
    return ss;
}

/* Size class for a total object size, or -1 for the malloc large path. */
static int janet_slab_class_for(size_t size) {
    for (int i = 0; i < JANET_SLAB_NCLASSES; i++) {
        if (size <= janet_slab_classes[i]) return i;
    }
    return -1;
}

static void *janet_slab_page_alloc(void) {
#ifdef JANET_WINDOWS
    return _aligned_malloc(JANET_SLAB_PAGE_SIZE, JANET_SLAB_PAGE_SIZE);
#else
    void *p = NULL;
    if (posix_memalign(&p, JANET_SLAB_PAGE_SIZE, JANET_SLAB_PAGE_SIZE) != 0) {
        return NULL;
    }
    return p;
#endif
}

static void janet_slab_page_free(void *page) {
#ifdef JANET_WINDOWS
    _aligned_free(page);
#else
    janet_free(page);
#endif
}

/* Page owning ptr, or NULL (malloc-backed large object, a stack object,
 * or foreign memory -- all treated as old). Membership is established by
 * binary search over the live-page directory first, so the header probe
 * below only ever reads our own mapped pages; the magic + bounds checks
 * then keep the mapping exact. */
static JanetSlabPage *janet_slab_page_for(void *ptr) {
    JanetSlabState *ss = (JanetSlabState *) janet_vm.slab_state;
    if (NULL == ss || 0 == ss->dir_count) return NULL;
    uintptr_t base = (uintptr_t) ptr & JANET_SLAB_PAGE_MASK;
    size_t lo = 0, hi = ss->dir_count;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        uintptr_t midbase = (uintptr_t) ss->dir[mid];
        if (midbase < base) {
            lo = mid + 1;
        } else if (midbase > base) {
            hi = mid;
        } else {
            JanetSlabPage *page = (JanetSlabPage *) base;
            if (page->magic != JANET_SLAB_MAGIC) return NULL;
            if (page->class_index < 0 || page->class_index >= JANET_SLAB_NCLASSES) return NULL;
            if (page->bump < 0 || (size_t) page->bump > (size_t) JANET_SLAB_USABLE) return NULL;
            if ((char *) ptr < (char *) JANET_SLAB_SLOTS(page)) return NULL;
            if ((char *) ptr >= (char *) base + JANET_SLAB_PAGE_SIZE) return NULL;
            return page;
        }
    }
    return NULL;
}

/* Insert a page into the sorted directory. Returns 0 on OOM (caller
 * must drop the page). Raw allocation only. */
static int janet_slab_dir_insert(JanetSlabPage *page) {
    JanetSlabState *ss = (JanetSlabState *) janet_vm.slab_state;
    size_t lo = 0, hi = ss->dir_count;
    uintptr_t base = (uintptr_t) page;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if ((uintptr_t) ss->dir[mid] < base) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    if (ss->dir_count == ss->dir_cap) {
        size_t ncap = ss->dir_cap ? ss->dir_cap * 2 : 16;
        JanetSlabPage **ndir = janet_malloc(ncap * sizeof(JanetSlabPage *));
        if (NULL == ndir) return 0;
        memcpy(ndir, ss->dir, lo * sizeof(JanetSlabPage *));
        memcpy(ndir + lo + 1, ss->dir + lo, (ss->dir_count - lo) * sizeof(JanetSlabPage *));
        janet_free(ss->dir);
        ss->dir = ndir;
        ss->dir_cap = ncap;
    } else {
        memmove(ss->dir + lo + 1, ss->dir + lo, (ss->dir_count - lo) * sizeof(JanetSlabPage *));
    }
    ss->dir[lo] = page;
    ss->dir_count++;
    return 1;
}

/* Object age for GC purposes: young pages hold only young objects (see
 * the header comment); anything else counts as old. */
static int janet_slab_obj_old(void *ptr) {
    JanetSlabPage *page = janet_slab_page_for(ptr);
    if (NULL == page) return 1;
    return page->age >= JANET_SLAB_OLD_AGE;
}

/* Debug (JANET_TRACE_STRICT): dump a copied entry's funcenvs and the
 * slab-page ownership of table upvalues — where did the child's
 * captures land? */
void janet_strict_debug_envs(JanetFunction *copy) {
    for (int ei = 0; ei < copy->def->environments_length; ei++) {
        JanetFuncEnv *fe = copy->envs[ei];
        janet_eprintf("[copy-env] env#%d len=%d offset=%d",
                ei, fe->length, fe->offset);
        if (fe->offset <= 0 && NULL != fe->as.values) {
            for (int vi = 0; vi < fe->length && vi < 12; vi++) {
                Janet v = fe->as.values[vi];
                JanetType vt = janet_type(v);
                const char *tn = janet_type_names[vt];
                if (vt == JANET_TABLE) {
                    JanetSlabPage *tp = janet_slab_page_for(
                            janet_unwrap_table(v));
                    JanetTable *tt = janet_unwrap_table(v);
                    janet_eprintf(" [v%d=table(count=%d proto=%p "
                            "page(owner=%d isolated=%d))]", vi,
                            tt->count, (void *) tt->proto,
                            (NULL != tp) ? (int) tp->owner : -1,
                            (NULL != tp) ? tp->isolated : -1);
                } else {
                    janet_eprintf(" [v%d=%s]", vi, tn);
                }
            }
        }
        janet_eprintf("\n");
    }
}

/* Step 1.3: owning fiber id for sweep filtering (0 = shared: mixed
 * pages, unowned pages, and malloc-large objects all read shared and
 * are spared by per-fiber sweeps). */
static uint32_t janet_slab_obj_owner(void *ptr) {
    JanetSlabPage *page = janet_slab_page_for(ptr);
    if (NULL == page) return 0;
    if (page->mixed) return 0;
    return page->owner;
}

static void *janet_slab_alloc(int class_index) {
    JanetSlabState *ss = janet_slab_state();
    size_t slot = janet_slab_classes[class_index];
    /* Step 1.2 requester: the running fiber (possibly NULL on host)
     * pays nursery bytes and owns bump pages; recycled slots keep
     * their page and mark it mixed on cross-owner reuse. P3.5c: a
     * spawn-construction override redirects attribution to the child
     * being built (owner id, strict bit, nursery, filing). */
    JanetFiber *req = (NULL != janet_vm.gc_alloc_ctx)
        ? janet_vm.gc_alloc_ctx : janet_vm.fiber;
    uint32_t reqid = (NULL != req) ? req->gc_owner : 0;
    /* P3.3b: freelists segregate by strictness (see release). The
     * pop logic below is unchanged (young/old skip + mixed-marking
     * still apply within each list). */
    JanetGCObject **freep = (NULL != req && req->heap_strict) ?
        &ss->strict_free[class_index] : &ss->young_free[class_index];
    /* Pop a young slot, skipping (to the old list) any slot whose page
     * has aged out since the slot was freed. Self-correcting: every pop
     * either uses or re-files the head, so it always terminates. */
    for (;;) {
        JanetGCObject *slot_obj = *freep;
        if (NULL == slot_obj) break;
        *freep = slot_obj->data.next;
        JanetSlabPage *page = janet_slab_page_for(slot_obj);
        if (NULL != page && page->age < JANET_SLAB_OLD_AGE) {
            if (0 != page->owner && page->owner != reqid) page->mixed = 1;
            if (NULL != req) req->gc_nursery_bytes += slot;
            if (NULL != getenv("JANET_TRACE_STRICT") && NULL != req &&
                    req->heap_strict) {
                janet_eprintf("[slab-pop-strict] req_owner=%d "
                        "class=%d page(owner=%d mixed=%d isolated=%d)\n",
                        (int) reqid, class_index, (int) page->owner,
                        page->mixed, page->isolated);
            }
            /* TEMPORARY 2b-iii bisect: zero recycled slots to test for
             * uninitialized-field reads. */
            memset(slot_obj, 0, slot);
            return slot_obj;
        }
        slot_obj->data.next = (JanetGCObject *) ss->old_free[class_index];
        ss->old_free[class_index] = slot_obj;
    }
    /* Bump path: the requester's own cur page when in a fiber (young
     * pages stay pure-owned), else the shared cur (host behavior
     * unchanged from before Step 1.2). */
    JanetSlabPage **curp = (NULL != req) ?
        &req->nursery_cur[class_index] : &ss->cur[class_index];
    JanetSlabPage *page = *curp;
    if (NULL == page || page->age >= JANET_SLAB_OLD_AGE ||
            (size_t) page->bump + slot > (size_t) JANET_SLAB_USABLE) {
        page = janet_slab_page_alloc();
        if (NULL == page) {
            return NULL;
        }
        page->next = ss->pages[class_index];
        page->magic = JANET_SLAB_MAGIC;
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
        if (!janet_slab_dir_insert(page)) {
            janet_slab_page_free(page);
            return NULL;
        }
        ss->pages[class_index] = page;
        *curp = page;
    }
    void *out = (char *) JANET_SLAB_SLOTS(page) + page->bump;
    page->bump += (int32_t) slot;
    if (NULL != req) req->gc_nursery_bytes += slot;
    /* P3.5c: strict-owned young is managed per-proc (nursery pressure
     * routes to per-proc collections; deaths float bounded by majors).
     * Counting it toward the global young threshold would fire
     * O(world) global minors on proc churn (the spawn scaler).
     * Spawn-construction attribution (override, never the running
     * fiber) is likewise per-proc managed. Shared/host bump still
     * counts. */
    if (NULL == req || (req == janet_vm.fiber && NULL == req->proc)) {
        /* Phase 1.1: every proc fiber's churn is per-fiber-scoped
         * (maybe_collect routes its nursery pressure to
         * collect_fiber_minor, which sweeps only its own lists);
         * counting it toward the global young threshold fires
         * O(world) global minors on proc churn — the spawn-scaler
         * grind (300k-string churn ran 90s; every global minor
         * sweeps every list). Strict procs were already excluded
         * (P3.5c); non-strict proc fibers join them. Shared/host and
         * plain-fiber bump still counts. */
        janet_vm.gc_young_bytes += slot;
    }
    return out;
}

/* Release a whole GC object: slab slot back to the free list matching
 * its page's current age, or plain free for large-path objects. All
 * whole-object frees route here. */
void janet_gc_release(JanetGCObject *mem) {
    JanetSlabState *ss = (JanetSlabState *) janet_vm.slab_state;
    if (NULL != ss) {
        JanetSlabPage *page = janet_slab_page_for(mem);
        if (NULL != page) {
            int cls = page->class_index;
            if (page->age < JANET_SLAB_OLD_AGE) {
                /* P3.3b: pure-strict young slots segregate (see state). */
                if (!page->mixed && page->isolated && 0 != page->owner) {
                    mem->data.next = (JanetGCObject *) ss->strict_free[cls];
                    ss->strict_free[cls] = mem;
                } else {
                    mem->data.next = (JanetGCObject *) ss->young_free[cls];
                    ss->young_free[cls] = mem;
                }
            } else {
                mem->data.next = (JanetGCObject *) ss->old_free[cls];
                ss->old_free[cls] = mem;
            }
            return;
        }
    }
    janet_free(mem);
}


/* Free all slab pages (used at clear; freelists die with them). */
static void janet_slab_free_pages(void) {
    JanetSlabState *ss = (JanetSlabState *) janet_vm.slab_state;
    if (NULL == ss) return;
    for (int i = 0; i < JANET_SLAB_NCLASSES; i++) {
        JanetSlabPage *page = ss->pages[i];
        while (NULL != page) {
            JanetSlabPage *next = page->next;
            janet_slab_page_free(page);
            page = next;
        }
        ss->pages[i] = NULL;
        ss->cur[i] = NULL;
        ss->young_free[i] = NULL;
        ss->old_free[i] = NULL;
        ss->strict_free[i] = NULL;
    }
    janet_free(ss->dir);
    ss->dir = NULL;
    ss->dir_count = 0;
    ss->dir_cap = 0;
    /* Step 1.2: remembered set dies with the pages. */
    janet_free(ss->xfb);
    ss->xfb = NULL;
    ss->xfb_count = 0;
    ss->xfb_cap = 0;
    ss->xfb_stuck = 0;
    /* Step 1.3: census resets with the pages (fibers are gone too). */
    janet_free(ss->fibers);
    ss->fibers = NULL;
    ss->fibers_count = 0;
    ss->fibers_cap = 0;
    janet_vm.gc_young_bytes = 0;
}

/* Merge old freelists back into young ones (used at major collections,
 * when all ages reset below). O(chains), no searching. */
static void janet_slab_merge_free(void) {
    JanetSlabState *ss = (JanetSlabState *) janet_vm.slab_state;
    if (NULL == ss) return;
    for (int i = 0; i < JANET_SLAB_NCLASSES; i++) {
        if (NULL == ss->old_free[i]) continue;
        JanetGCObject *tail = ss->old_free[i];
        while (NULL != tail->data.next) {
            tail = tail->data.next;
        }
        tail->data.next = (JanetGCObject *) ss->young_free[i];
        ss->young_free[i] = ss->old_free[i];
        ss->old_free[i] = NULL;
    }
}

/* Reset generational state after a full sweep: all pages back to young
 * (conservative restart -- the next minors re-segregate), dirty flags
 * clear, freelists merged, nursery counter restarted. O(pages). */
static void janet_slab_major_reset(void) {
    JanetSlabState *ss = (JanetSlabState *) janet_vm.slab_state;
    if (NULL == ss) return;
    for (int i = 0; i < JANET_SLAB_NCLASSES; i++) {
        for (JanetSlabPage *p = ss->pages[i]; NULL != p; p = p->next) {
            p->age = 0;
            p->dirty = 0;
        }
    }
    janet_slab_merge_free();
    janet_vm.gc_young_bytes = 0;
    /* P3.1 cross-fiber epoch: a major saw every edge, so the set
     * restarts empty (stale entries would pin dead containers as
     * roots forever; stuck clears too — capacity is per-epoch now).
     * Behavior-neutral today (marking is complete without xfb; the
     * census carries fiber minors). Load-bearing later: a per-proc
     * collection younger than the last major sees complete edges. */
    ss->xfb_count = 0;
    ss->xfb_stuck = 0;
}
void janet_sweep_weak_drop(void) {
    /* Weak-ref dropping runs at majors only. In minors the mark is
     * partial (old heap unscanned), so liveness verdicts would be wrong;
     * weak tables are instead traversed conservatively via the normal
     * mark exemption for weak types, and clearing waits for a major. */
    if (janet_vm.gc_minor_phase) return;
    janet_sweep_weak_drop_list(janet_vm.weak_blocks);
    /* P3.5b Slice A: weak tables now also live in per-fiber lists. */
    {
        JanetSlabState *ss = (JanetSlabState *) janet_vm.slab_state;
        if (NULL != ss) {
            size_t wi;
            for (wi = 0; wi < ss->fibers_count; wi++) {
                JanetFiber *f = ss->fibers[wi];
                janet_sweep_weak_drop_list((JanetGCObject *) f->heap_lists[2]);
            }
        }
    }
}

static void janet_sweep_weak_drop_list(JanetGCObject *current) {
    JanetGCObject *next;
    while (NULL != current) {
        next = current->data.next;
        if ((current->flags & JANET_MEM_DISABLED) || janet_gc_reachable(current)) {
            /* Check for dead references. The epoch compare replaces the
             * old sticky REACHABLE gate — under the epoch mark no
             * sticky bit is ever set, and the old gate made the entire
             * weak-drop dead code (suite-ev2 count = 100). */
            enum JanetMemoryType type = janet_gc_type(current);
            if (type == JANET_MEMORY_ARRAY_WEAK) {
                JanetArray *array = (JanetArray *) current;
                for (uint32_t i = 0; i < (uint32_t) array->count; i++) {
                    if (!janet_check_liveref(array->data[i])) {
                        array->data[i] = janet_wrap_nil();
                    }
                }
            } else {
                JanetTable *table = (JanetTable *) current;
                int check_values = (type == JANET_MEMORY_TABLE_WEAKV) || (type == JANET_MEMORY_TABLE_WEAKKV);
                int check_keys = (type == JANET_MEMORY_TABLE_WEAKK) || (type == JANET_MEMORY_TABLE_WEAKKV);
                JanetKV *end = table->data + table->capacity;
                JanetKV *kvs = table->data;
                while (kvs < end) {
                    int drop = 0;
                    if (check_keys && !janet_check_liveref(kvs->key)) drop = 1;
                    if (check_values && !janet_check_liveref(kvs->value)) drop = 1;
                    if (drop) {
                        /* Inlined from janet_table_remove without search */
                        table->count--;
                        table->deleted++;
                        kvs->key = janet_wrap_nil();
                        kvs->value = janet_wrap_false();
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
static void janet_clear_all_reachable_bits(void) {
    JanetSlabState *ss = (JanetSlabState *) janet_vm.slab_state;
    JanetGCObject *cur;
    size_t fi;
    int kind;
    if (NULL != ss) {
        for (fi = 0; fi < ss->fibers_count; fi++) {
            JanetFiber *f = ss->fibers[fi];
            for (kind = 0; kind < 3; kind++) {
                for (cur = f->heap_lists[kind]; NULL != cur; cur = cur->data.next)
                    cur->flags &= ~JANET_MEM_EPOCH_MASK;
            }
        }
    }
    JanetGCObject *globals[3];
    globals[0] = janet_vm.blocks;
    globals[1] = (JanetGCObject *) janet_vm.string_blocks;
    globals[2] = janet_vm.weak_blocks;
    for (kind = 0; kind < 3; kind++) {
        for (cur = globals[kind]; NULL != cur; cur = cur->data.next)
            cur->flags &= ~JANET_MEM_EPOCH_MASK;
    }
}

static void janet_sweep_kind(int kind, int is_minor, uint32_t only_owner,
                             int walk_fibers) {
    JanetGCObject **ghead;
    JanetSlabState *ss;
    size_t fi;
    if (kind == 1) {
        ghead = (JanetGCObject **) &janet_vm.string_blocks;
    } else if (kind == 0) {
        ghead = (JanetGCObject **) &janet_vm.blocks;
    } else {
        ghead = (JanetGCObject **) &janet_vm.weak_blocks;
    }
    /* Phase 1 density: the GLOBAL MINOR no longer walks every
     * registered fiber's lists. With 1M procs that loop is the
     * quadratic engine (measured: the second 100k spawns cost 3.2x
     * the first). Each fiber's own minor handles its own pressure;
     * fibers get one final minor at completion (ev.c), so a global
     * minor now sweeps only the globals. Parked-forever fibers keep
     * their young (bounded by their real working set). The FULL
     * collect (janet_sweep) still walks every fiber. */
    ss = (JanetSlabState *) janet_vm.slab_state;
    if (NULL != ss && walk_fibers) {
        for (fi = 0; fi < ss->fibers_count; fi++) {
            JanetFiber *f = ss->fibers[fi];
            janet_sweep_list((JanetGCObject **) &f->heap_lists[kind],
                             kind, is_minor, only_owner);
        }
    }
    janet_sweep_list(ghead, kind, is_minor, only_owner);
}

/* P3.5b Slice A heap verifier (JANET_HEAP_VERIFY=1, dev only): every
 * listed object counted exactly once across globals + all fiber
 * lists, total == block_count; no cycles (bounded walk); fiber
 * objects only in globals (filing invariant). O(world); runs after
 * sweeps when enabled. */
static void janet_heap_verify(const char *where) {
    static int verify = -1;
    size_t total = 0;
    size_t kindtot[3] = {0, 0, 0};
    int kind;
    if (-1 == verify) verify = (NULL != getenv("JANET_HEAP_VERIFY")) ? 1 : 0;
    if (!verify) return;
    for (kind = 0; kind < 3; kind++) {
        JanetGCObject *ghead;
        JanetSlabState *ss = (JanetSlabState *) janet_vm.slab_state;
        size_t fi;
        if (kind == 1) ghead = (JanetGCObject *) janet_vm.string_blocks;
        else if (kind == 0) ghead = (JanetGCObject *) janet_vm.blocks;
        else ghead = (JanetGCObject *) janet_vm.weak_blocks;
        {
            size_t n = 0;
            JanetGCObject *cur;
            for (cur = ghead; NULL != cur; cur = cur->data.next) {
                if (++n > janet_vm.block_count + 1) {
                    fprintf(stderr, "heap-verify(%s): cycle in globals kind %d\n", where, kind);
                    abort();
                }
            }
            total += n;
            kindtot[kind] += n;
        }
        if (NULL != ss) {
            for (fi = 0; fi < ss->fibers_count; fi++) {
                JanetFiber *f = ss->fibers[fi];
                JanetGCObject *cur;
                size_t n = 0;
                for (cur = (JanetGCObject *) f->heap_lists[kind]; NULL != cur; cur = cur->data.next) {
                    if (++n > janet_vm.block_count + 1) {
                        fprintf(stderr, "heap-verify(%s): cycle in fiber %p kind %d\n",
                                where, (void *) f, kind);
                        abort();
                    }
                    if (janet_gc_type(cur) == JANET_MEMORY_FIBER) {
                        fprintf(stderr, "heap-verify(%s): fiber object in per-fiber list\n", where);
                        abort();
                    }
                }
                total += n;
                kindtot[kind] += n;
            }
        }
    }
    if (total != janet_vm.block_count) {
        fprintf(stderr, "heap-verify(%s): listed %d != block_count %d (kinds %d %d %d)\n",
                where, (int) total, (int) janet_vm.block_count,
                (int) kindtot[0], (int) kindtot[1], (int) kindtot[2]);
        abort();
    }
}

/* P3.5b deferred drain: after all lists are walked, splice parked
 * death-migration chains into the globals (each chain walked once
 * for its tail). Parked chains hold marked survivors + old-unmarked
 * floated from dead fibers; both belong in the globals now. */
static void janet_sweep_drain_pending(void) {
    int k;
    for (k = 0; k < 3; k++) {
        JanetGCObject *fl = (JanetGCObject *) janet_vm.sweep_pending[k];
        if (NULL == fl) continue;
        janet_vm.sweep_pending[k] = NULL;
        {
            JanetGCObject *tail = fl;
            while (NULL != tail->data.next)
                tail = tail->data.next;
            if (k == 0) {
                tail->data.next = (JanetGCObject *) janet_vm.blocks;
                janet_vm.blocks = (void *) fl;
            } else if (k == 1) {
                tail->data.next = (JanetGCObject *) janet_vm.string_blocks;
                janet_vm.string_blocks = (void *) fl;
            } else {
                tail->data.next = (JanetGCObject *) janet_vm.weak_blocks;
                janet_vm.weak_blocks = (void *) fl;
            }
        }
    }
}

static void janet_sweep_heaps(int walk_fibers) {
janet_sweep_kind(2, janet_vm.gc_minor_phase, 0, walk_fibers);
    janet_sweep_kind(0, janet_vm.gc_minor_phase, 0, walk_fibers);
    janet_sweep_kind(1, janet_vm.gc_minor_phase, 0, walk_fibers);
    janet_sweep_drain_pending();
    janet_heap_verify("global");
}

/* Step 1.3: per-fiber sweep freeing only unmarked young blocks
 * owned by the target (P3.5b Slice B: the target's own three lists
 * only — O(own); globals + foreign lists untouched). Foreign young
 * unmarked blocks float until a global collection; foreign marked
 * blocks were already spared by the mark. All callers pass live
 * fibers (never freed mid-sweep here). */
static void janet_sweep_fiber_owned(JanetFiber *f) {
    /* P3.5b: fiber minors keep the Slice A shape (owner filter across
     * all lists — today's behavior exactly). Full narrowing is
     * deferred: walking only own lists here crashes boot image
     * generation deterministically (garbage argv in partition-slice;
     * heap-history-dependent reads masked by prompt freeing — under
     * investigation, see aot-plan). Plain fibers need census marking
     * O(N) regardless, so narrowing their sweep buys no complexity
     * win. Strict process collections (the O(n) spawn target) use
     * the narrow sibling below. */
    uint32_t owner = f->gc_owner;
janet_sweep_kind(2, 1, owner, 1);
    janet_sweep_kind(0, 1, owner, 1);
    janet_sweep_kind(1, 1, owner, 1);
    janet_sweep_drain_pending();
    janet_heap_verify("owned");
}

/* P3.5b Slice B (strict only): O(own) — walk only the target's three
 * lists with the owner filter. Sound exactly where collect_process's
 * narrowed marking is sound (strict isolation: spawn-copy entry +
 * export panics + copy sends; foreign stacks hold no P-young, audited
 * by JANET_PROC_VERIFY). Marking there is complete (P-roots + shared
 * C roots + remembered set), so unmarked-own is garbage; foreign
 * young in other lists floats until a global collection. Fresh pure
 * pages (spawn/churn steady state) free here; mixed/large slots read
 * owner 0 and float the same way. No fiber objects live here (filing
 * invariant), so the census never mutates mid-walk. */
static void janet_sweep_proc_owned(JanetFiber *f) {
    uint32_t owner = f->gc_owner;
    janet_sweep_list((JanetGCObject **) &f->heap_lists[2], 2, 1, owner);
    janet_sweep_list((JanetGCObject **) &f->heap_lists[0], 0, 1, owner);
    janet_sweep_list((JanetGCObject **) &f->heap_lists[1], 1, 1, owner);
    janet_sweep_drain_pending();
    janet_heap_verify("proc-owned");
}

static void janet_sweep_heaps_owned(JanetFiber *f) {
    janet_sweep_fiber_owned(f);
}


/* Step 1 introspection: per-owner slab page counts. Returns
 * @{ :pages total :unowned n :owned @{id count} } (pure numbers: fiber
 * death can never dangle them). Caller holds the result on the Janet
 * stack (rooted across the table builds below). */
JanetTable *janet_slab_owner_stats(void) {
    JanetSlabState *ss = janet_slab_state();
    /* P3.1: the observer must not perturb — table building below
     * stores interned keys (foreign-owned pages) and would record
     * cross-fiber edges for the introspection itself. The set is
     * append-only and order-free, so truncate to the entry count on
     * the way out (stuck flag untouched: cap pressure is real). */
    size_t xfb_entry = ss->xfb_count;
    JanetTable *out = janet_table(0);
    Janet out_j = janet_wrap_table(out);
    janet_gcroot(out_j);
    JanetTable *owned = janet_table(0);
    Janet owned_j = janet_wrap_table(owned);
    janet_gcroot(owned_j);
    int32_t total = 0;
    int32_t unowned = 0;
    int cls;
    for (cls = 0; cls < JANET_SLAB_NCLASSES; cls++) {
        JanetSlabPage *p;
        for (p = ss->pages[cls]; NULL != p; p = p->next) {
            total++;
            if (0 == p->owner) {
                unowned++;
            } else {
                Janet k = janet_wrap_integer((int64_t) p->owner);
                Janet cur = janet_table_get(owned, k);
                int32_t n = janet_checktype(cur, JANET_NIL) ? 0 : janet_unwrap_integer(cur);
                janet_table_put(owned, k, janet_wrap_integer(n + 1));
            }
        }
    }
    janet_table_put(out, janet_ckeywordv("pages"), janet_wrap_integer(total));
    janet_table_put(out, janet_ckeywordv("unowned"), janet_wrap_integer(unowned));
    janet_table_put(out, janet_ckeywordv("owned"), owned_j);
    /* Step 1.2: cross-fiber remembered edges pending (pruned in 1.3). */
    janet_table_put(out, janet_ckeywordv("remembered"),
                    janet_wrap_integer((int64_t) xfb_entry));
    janet_table_put(out, janet_ckeywordv("remember-stuck"),
                    janet_wrap_boolean(ss->xfb_stuck));
    /* P3.1 observer hygiene (see entry): drop this call's self-edges. */
    if (ss->xfb_count > xfb_entry) ss->xfb_count = xfb_entry;
    janet_gcunroot(owned_j);
    janet_gcunroot(out_j);
    return out;
}

/* Step 1.3 census helpers: add is cross-TU (fiber birth sites),
 * remove runs at sweep-free in this TU. */
void janet_census_add(JanetFiber *f) {
    JanetSlabState *ss = janet_slab_state();
    if (ss->fibers_count == ss->fibers_cap) {
        size_t ncap = ss->fibers_cap ? ss->fibers_cap * 2 : 16;
        JanetFiber **nf = janet_malloc(ncap * sizeof(JanetFiber *));
        if (NULL == nf) JANET_OUT_OF_MEMORY;
        if (NULL != ss->fibers) {
            memcpy(nf, ss->fibers, ss->fibers_count * sizeof(JanetFiber *));
            janet_free(ss->fibers);
        }
        ss->fibers = nf;
        ss->fibers_cap = ncap;
    }
    ss->fibers[ss->fibers_count++] = f;
}

static void janet_census_remove(JanetFiber *f) {
    JanetSlabState *ss = (JanetSlabState *) janet_vm.slab_state;
    size_t i;
    if (NULL == ss) return;
    for (i = 0; i < ss->fibers_count; i++) {
        if (ss->fibers[i] == f) {
            ss->fibers[i] = ss->fibers[--ss->fibers_count];
            return;
        }
    }
}

/* P3.4 bookkeeping touch (see gc.h): old containers go dirty, and
 * nothing else happens. Used by raw table puts for marshal identity
 * maps (never the value graph). */
void janet_gc_touch(void *obj) {
    JanetSlabPage *page = janet_slab_page_for(obj);
    if (NULL == page) return;
    if (page->age >= JANET_SLAB_OLD_AGE) page->dirty = 1;
}

/* Minor-mode reachability gate (Phase 2b-iii policy): during a minor
 * collection, skip old, clean, precisely-tracked containers (their young
 * references are covered by the barrier). Everything else is traversed
 * exactly like a major: young objects, large malloc-backed objects
 * (always traversed, few), disabled objects, fibers, abstracts, weak
 * tables (cleared only at majors), and all non-table/array types.
 * Skipping is always safe; traversing is always safe; only skipping an
 * object with unbarriered young edges would be wrong, and every such
 * edge goes through janet_gc_barrier. */
static int janet_gc_minor_skip(JanetGCObject *mem) {
    if (!janet_vm.gc_minor_phase) return 0;
    /* While any compilation is active, traverse everything: compilation
     * interleaves symbol/env table traffic (macroexpansion resolves and
     * installs bindings, lint tables churn) with macro-fiber collections
     * in ways that defeat the clean/generation accounting the gate
     * relies on. Compiles are infrequent next to runtime stores, and
     * minors still skip all old sweeping, so this costs little. */
    if (janet_vm.compiler_stack) return 0;
    int32_t flags = mem->flags;
    if (flags & JANET_MEM_DISABLED) return 0;
    int32_t type = flags & JANET_MEM_TYPEBITS;
#if 0 /* TEMPORARY 2b-iii bisect: tables always traverse */
    if (type != JANET_MEMORY_TABLE && type != JANET_MEMORY_ARRAY) return 0;
#else
    if (type != JANET_MEMORY_ARRAY) return 0;
#endif
    JanetSlabPage *page = janet_slab_page_for(mem);
    if (NULL == page) return 0;
    if (page->age < JANET_SLAB_OLD_AGE) return 0;
    return !page->dirty;
}

/* Step 1.2 cap: past this many cross-fiber edges, recording stops
 * (stuck) instead of growing without bound. Pruning lands in 1.3. */
#define JANET_XFB_MAX (1024 * 1024)

/* Generational write barrier (Phase 2b-iii policy): record an old
 * container that newly references a heap value, so minor collections
 * (which skip clean old tables/arrays) still find the edge. Immediates
 * (nil/boolean/number/cfunction/pointer) never create edges; young and
 * large containers are always traversed, so only old slab pages are
 * marked dirty. Call after every store of a Janet value into a heap
 * table, array, or equivalent long-lived container.
 *
 * Step 1.2 cross-fiber recording rides along: when container and value
 * live on pages owned by different fibers (mixed pages read as shared),
 * the container joins the cross-fiber remembered set for per-fiber
 * minors (1.3). Shared either side needs nothing (shared space is
 * always traversed). Hot path unchanged when same-owner. */
void janet_gc_barrier(void *obj, Janet value) {
    JanetType t = janet_type(value);
    if (t == JANET_NIL || t == JANET_BOOLEAN || t == JANET_NUMBER ||
            t == JANET_CFUNCTION || t == JANET_POINTER) {
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
    if (janet_vm.heap_strict_live &&
            t != JANET_STRING && t != JANET_SYMBOL && t != JANET_KEYWORD) {
        iso_o = (janet_gc_header(obj)->flags & JANET_MEM_ISOLATED) ? 1 : 0;
        /* Object starts (NOT unwrapped data pointers: tuples and
         * structs point past their heads — reading flags there sees
         * element bits as garbage and panics spuriously). */
        void *vobj = NULL;
        switch (t) {
            case JANET_ARRAY:
                vobj = (void *) janet_unwrap_array(value);
                break;
            case JANET_TUPLE:
                vobj = (void *) janet_tuple_head(janet_unwrap_tuple(value));
                break;
            case JANET_TABLE:
                vobj = (void *) janet_unwrap_table(value);
                break;
            case JANET_STRUCT:
                vobj = (void *) janet_struct_head(janet_unwrap_struct(value));
                break;
            case JANET_BUFFER:
                vobj = (void *) janet_unwrap_buffer(value);
                break;
            case JANET_FUNCTION:
                vobj = (void *) janet_unwrap_function(value);
                break;
            case JANET_ABSTRACT:
                vobj = (void *) janet_abstract_head(janet_unwrap_abstract(value));
                break;
            case JANET_FIBER:
                vobj = (void *) janet_unwrap_fiber(value);
                break;
            default:
                break;
        }
        if (NULL != vobj) {
            iso_v = (janet_gc_header(vobj)->flags & JANET_MEM_ISOLATED) ? 1 : 0;
        }
        if (NULL != getenv("JANET_TRACE_STRICT")) {
            JanetSlabPage *tp = janet_slab_page_for(obj);
            JanetFiber *treq = (NULL != janet_vm.gc_alloc_ctx)
                ? janet_vm.gc_alloc_ctx : janet_vm.fiber;
            janet_eprintf("[strict-put] obj=%p iso_o=%d "
                    "page(owner=%d mixed=%d isolated=%d) ",
                    obj, iso_o,
                    (NULL != tp) ? tp->owner : -1,
                    (NULL != tp) ? tp->mixed : -1,
                    (NULL != tp) ? tp->isolated : -1);
            if (t == JANET_TABLE) {
                JanetTable *ct = (JanetTable *) obj;
                janet_eprintf("obj-table(count=%d proto=%p) ",
                        ct->count, (void *) ct->proto);
            }
            janet_eprintf("vobj=%p iso_v=%d req=%p req_strict=%d "
                    "req_owner=%d cur_fiber=%p",
                    vobj, iso_v, (void *) treq,
                    (NULL != treq) ? treq->heap_strict : -1,
                    (NULL != treq) ? (int) treq->gc_owner : -1,
                    (void *) janet_vm.fiber);
            fprintf(stderr, " ra0=%p\n", __builtin_return_address(0));
        }
        if (iso_v && !iso_o) {
            janet_panic("strict proc value escaping to a shared container "
                        "(copy it across: messages and spawn entries copy)");
        }
    }
    JanetSlabPage *page = janet_slab_page_for(obj);
    if (NULL == page) return;
    if (page->age >= JANET_SLAB_OLD_AGE) page->dirty = 1;
    /* Cross-fiber edge below; same-owner (and shared) stores skip it. */
    uint32_t own_o = page->mixed ? 0 : page->owner;
    if (0 == own_o) return;
    void *vptr = NULL;
    Janet cobj = janet_wrap_nil();
    switch (janet_gc_type((JanetGCObject *) obj)) {
        case JANET_MEMORY_ARRAY:
        case JANET_MEMORY_ARRAY_WEAK:
            cobj = janet_wrap_array((JanetArray *) obj);
            break;
        case JANET_MEMORY_TABLE:
        case JANET_MEMORY_TABLE_WEAKK:
        case JANET_MEMORY_TABLE_WEAKV:
        case JANET_MEMORY_TABLE_WEAKKV:
            cobj = janet_wrap_table((JanetTable *) obj);
            break;
        default:
            return;
    }
    switch (t) {
        case JANET_STRING:
        case JANET_SYMBOL:
        case JANET_KEYWORD:
            vptr = (void *) janet_unwrap_string(value);
            break;
        case JANET_ARRAY:
            vptr = (void *) janet_unwrap_array(value);
            break;
        case JANET_TUPLE:
            vptr = (void *) janet_unwrap_tuple(value);
            break;
        case JANET_TABLE:
            vptr = (void *) janet_unwrap_table(value);
            break;
        case JANET_STRUCT:
            vptr = (void *) janet_unwrap_struct(value);
            break;
        case JANET_BUFFER:
            vptr = (void *) janet_unwrap_buffer(value);
            break;
        case JANET_FUNCTION:
            vptr = (void *) janet_unwrap_function(value);
            break;
        case JANET_ABSTRACT:
            vptr = (void *) janet_unwrap_abstract(value);
            break;
        case JANET_FIBER:
            vptr = (void *) janet_unwrap_fiber(value);
            break;
        default:
            return;
    }
    JanetSlabPage *pv = janet_slab_page_for(vptr);
    if (NULL == pv) return;
    uint32_t own_v = pv->mixed ? 0 : pv->owner;
    if (0 == own_v || own_v == own_o) return;
    /* P3.3 cross-strict sharing: both bits set but different owners
     * (two strict heaps, or strict + claimed-strict page) — refuse
     * (message-passing keeps heaps disjoint). Reuses this path's
     * pages: zero extra walks. Unreachable when no strict fiber ever
     * ran (bits never set — iso_* stay 0 above). */
    if (iso_v && iso_o) {
        janet_panic("strict proc value shared across isolated heaps "
                    "(copy it across: messages and spawn entries copy)");
    }
    JanetSlabState *ss = janet_slab_state();
    if (ss->xfb_stuck) return;
    if (ss->xfb_count == ss->xfb_cap) {
        size_t ncap = ss->xfb_cap ? ss->xfb_cap * 2 : 64;
        if (ncap > JANET_XFB_MAX) {
            ss->xfb_stuck = 1;
            return;
        }
        Janet *nxfb = janet_malloc(ncap * sizeof(Janet));
        if (NULL == nxfb) {
            ss->xfb_stuck = 1;
            return;
        }
        if (NULL != ss->xfb) {
            memcpy(nxfb, ss->xfb, ss->xfb_count * sizeof(Janet));
            janet_free(ss->xfb);
        }
        ss->xfb = nxfb;
        ss->xfb_cap = ncap;
    }
    ss->xfb[ss->xfb_count++] = cobj;
}

void *janet_gcalloc(enum JanetMemoryType type, size_t size) {
    JanetGCObject *mem;

    /* Make sure everything is inited */
    janet_assert(NULL != janet_vm.cache, "please initialize janet before use");
    /* Phase 2b-iii: slab pages for size-classed objects, malloc large
     * path otherwise. Either way the object joins a heap list below. */
    int slab_class = janet_slab_class_for(size);
    if (slab_class >= 0) {
        mem = janet_slab_alloc(slab_class);
    } else {
        mem = janet_malloc(size);
    }

    /* Check for bad malloc */
    if (NULL == mem) {
        JANET_OUT_OF_MEMORY;
    }

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
        JanetSlabPage *pg = janet_slab_page_for(mem);
        if (NULL != pg && !pg->mixed && pg->isolated) {
            mem->flags |= JANET_MEM_ISOLATED;
        }
    } else {
        JanetFiber *breq = (NULL != janet_vm.gc_alloc_ctx)
            ? janet_vm.gc_alloc_ctx : janet_vm.fiber;
        if (NULL != breq && breq->heap_strict) {
            mem->flags |= JANET_MEM_ISOLATED;
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
    janet_vm.next_collection += size;
    {
        int kind;
        JanetGCObject **head;
        if (type == JANET_MEMORY_STRING || type == JANET_MEMORY_SYMBOL) {
            kind = 1;
        } else if (type < JANET_MEMORY_TABLE_WEAKK) {
            kind = 0;
        } else {
            kind = 2;
        }
        /* P3.5c: attribution override (strict spawn construction)
         * redirects births to the child being built (owner id, strict
         * bit, nursery, filing). Fiber objects always stay global. */
        JanetFiber *eff = (NULL != janet_vm.gc_alloc_ctx)
            ? janet_vm.gc_alloc_ctx : janet_vm.fiber;
        if (NULL != eff && type != JANET_MEMORY_FIBER) {
            head = (JanetGCObject **) &eff->heap_lists[kind];
#ifdef JANET_HEAP_FILE_CHECK
            {
                JanetSlabState *css = (JanetSlabState *) janet_vm.slab_state;
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
            head = (JanetGCObject **) &janet_vm.string_blocks;
        } else if (kind == 0) {
            head = (JanetGCObject **) &janet_vm.blocks;
        } else {
            head = (JanetGCObject **) &janet_vm.weak_blocks;
        }
        mem->data.next = *head;
        *head = mem;
    }
    janet_vm.block_count++;

    return (void *)mem;
}

static void free_one_scratch(JanetScratch *s) {
    if (NULL != s->finalize) {
        s->finalize((char *) s->mem);
    }
    janet_free(s);
}

/* Free all allocated scratch memory */
static void janet_free_all_scratch(void) {
    for (size_t i = 0; i < janet_vm.scratch_len; i++) {
        free_one_scratch(janet_vm.scratch_mem[i]);
    }
    janet_vm.scratch_len = 0;
}

static JanetScratch *janet_mem2scratch(void *mem) {
    JanetScratch *s = (JanetScratch *)mem;
    return s - 1;
}

/* Mark the full root set (Phase 2a explicit roots + 2b-i shadow roots).
 * Shared by major and minor collections; in minors the table/array gate
 * (janet_gc_minor_skip) prunes old-clean subtrees during traversal. */
static void janet_mark_roots(void) {
    uint32_t i;
    size_t orig_rootcount = janet_vm.root_count;
#ifdef JANET_EV
    janet_ev_mark();
#endif
    if (janet_vm.top_dyns) {
        janet_mark(janet_wrap_table(janet_vm.top_dyns));
    }
    if (janet_vm.profile_table) {
        janet_mark(janet_wrap_table(janet_vm.profile_table));
    }
    if (janet_vm.root_fiber != NULL) { /* Can be NULL if janet_collect called outside of interpreter loop */
        janet_mark_fiber(janet_vm.root_fiber);
    }
    /* Mark values held in C structs by live compilations (Phase 2a). */
    for (JanetCompiler *comp = janet_vm.compiler_stack; comp; comp = comp->compiler_next) {
        janet_mark_compiler(comp);
    }
    for (i = 0; i < orig_rootcount; i++)
        janet_mark(janet_vm.roots[i]);
    /* Scoped shadow roots (Phase 2b-i): everything pushed is live. */
    for (i = 0; i < janet_vm.shadow_count; i++)
        janet_mark(janet_vm.shadow_roots[i]);
    /* Step 1.2 cross-fiber remembered set: recorded containers stay
     * live (pruned in 1.3). */
    {
        JanetSlabState *xss = (JanetSlabState *) janet_vm.slab_state;
        if (NULL != xss) {
            size_t xi;
            for (xi = 0; xi < xss->xfb_count; xi++)
                janet_mark(xss->xfb[xi]);
        }
    }
    while (orig_rootcount < janet_vm.root_count) {
        Janet x = janet_vm.roots[--janet_vm.root_count];
        janet_mark(x);
    }
}

/* Minor-mode edge verifier (Phase 2b-iii debugging, env-gated via
 * JANET_GC_VERIFY=1): after minor marking, walk every heap object and
 * check that each young heap object reachable through a live edge is
 * marked. Reports the holder type and aborts on the first miss (a missed
 * barrier edge or a wrongly skipped container). Expensive: development
 * use only. */
static int janet_gc_verify_young_marked(const char *holder, Janet v) {
    JanetType t = janet_type(v);
    JanetGCObject *m = NULL;
    switch (t) {
        default:
            return 1;
        case JANET_FIBER:
            m = (JanetGCObject *) janet_unwrap_fiber(v);
            break;
        case JANET_STRING:
        case JANET_SYMBOL:
        case JANET_KEYWORD:
            m = (JanetGCObject *) janet_string_head(janet_unwrap_string(v));
            break;
        case JANET_ARRAY:
            m = (JanetGCObject *) janet_unwrap_array(v);
            break;
        case JANET_TABLE:
            m = (JanetGCObject *) janet_unwrap_table(v);
            break;
        case JANET_STRUCT:
            m = (JanetGCObject *) janet_struct_head(janet_unwrap_struct(v));
            break;
        case JANET_TUPLE:
            m = (JanetGCObject *) janet_tuple_head(janet_unwrap_tuple(v));
            break;
        case JANET_BUFFER:
            m = (JanetGCObject *) janet_unwrap_buffer(v);
            break;
        case JANET_FUNCTION:
            m = (JanetGCObject *) janet_unwrap_function(v);
            break;
        case JANET_ABSTRACT:
            m = (JanetGCObject *) janet_abstract_head(janet_unwrap_abstract(v));
            break;
    }
    if (NULL == m) return 1;
    JanetSlabPage *page = janet_slab_page_for(m);
    if (NULL == page || page->age >= JANET_SLAB_OLD_AGE) return 1;
    if (janet_gc_reachable(m)) return 1;
    fprintf(stderr, "gc-verify: young unmarked %p via %s\n", (void *) m, holder);
    return 0;
}

static int janet_gc_verify_minor(void) {
    int bad = 0;
    /* NOTE: janet_vm.blocks etc. are void* in the struct; cast through.
     * P3.5b Slice A: holders also live in per-fiber lists (pass ==
     * nfib covers the globals). */
    JanetSlabState *vss = (JanetSlabState *) janet_vm.slab_state;
    size_t nfib = (NULL != vss) ? vss->fibers_count : 0;
    for (size_t pass = 0; pass <= nfib; pass++) {
    void *heads[3];
    if (pass == nfib) {
        heads[0] = janet_vm.blocks;
        heads[1] = janet_vm.string_blocks;
        heads[2] = janet_vm.weak_blocks;
    } else {
        JanetFiber *vf = vss->fibers[pass];
        heads[0] = vf->heap_lists[0];
        heads[1] = vf->heap_lists[1];
        heads[2] = vf->heap_lists[2];
    }
    for (int h = 0; h < 3; h++) {
        for (JanetGCObject *m = (JanetGCObject *) heads[h]; NULL != m; m = m->data.next) {
            /* Only live holders matter: an unmarked holder's subgraph is
             * garbage as a whole (its young referents are correctly
             * unmarked too). Disabled objects are always kept+traversed,
             * so their edges count as live. */
            if (!(janet_gc_reachable(m) || (m->flags & JANET_MEM_DISABLED)) &&
                    !janet_slab_obj_old(m)) continue;
            int32_t type = m->flags & JANET_MEM_TYPEBITS;
            if (type == JANET_MEMORY_TABLE || type == JANET_MEMORY_TABLE_WEAKK ||
                    type == JANET_MEMORY_TABLE_WEAKV || type == JANET_MEMORY_TABLE_WEAKKV) {
                JanetTable *t = (JanetTable *) m;
                if (t->proto) {
                    if (!janet_gc_verify_young_marked("table-proto", janet_wrap_table(t->proto))) bad = 1;
                }
                JanetKV *end = t->data + t->capacity;
                for (JanetKV *kv = t->data; kv < end; kv++) {
                    if (!janet_gc_verify_young_marked("table-key", kv->key)) bad = 1;
                    if (!janet_gc_verify_young_marked("table-value", kv->value)) bad = 1;
                }
            } else if (type == JANET_MEMORY_ARRAY || type == JANET_MEMORY_ARRAY_WEAK) {
                JanetArray *a = (JanetArray *) m;
                for (int32_t i = 0; i < a->count; i++) {
                    if (!janet_gc_verify_young_marked("array-elem", a->data[i])) bad = 1;
                }
            } else if (type == JANET_MEMORY_TUPLE) {
                JanetTupleHead *th = (JanetTupleHead *) m;
                for (int32_t i = 0; i < th->length; i++) {
                    if (!janet_gc_verify_young_marked("tuple-elem", th->data[i])) bad = 1;
                }
            } else if (type == JANET_MEMORY_STRUCT) {
                JanetStructHead *sh = (JanetStructHead *) m;
                for (int32_t i = 0; i < sh->length; i++) {
                    if (!janet_gc_verify_young_marked("struct-key", sh->data[i].key)) bad = 1;
                    if (!janet_gc_verify_young_marked("struct-value", sh->data[i].value)) bad = 1;
                }
            } else if (type == JANET_MEMORY_FIBER) {
                JanetFiber *f = (JanetFiber *) m;
                if (f->data) {
                    for (int32_t i = f->stackstart; i < f->stacktop; i++) {
                        if (!janet_gc_verify_young_marked("fiber-slot", f->data[i])) bad = 1;
                    }
                }
                if (f->env) {
                    if (!janet_gc_verify_young_marked("fiber-env", janet_wrap_table(f->env))) bad = 1;
                }
                if (f->child) {
                    if (!janet_gc_verify_young_marked("fiber-child", janet_wrap_fiber(f->child))) bad = 1;
                }
                if (!janet_gc_verify_young_marked("fiber-last", f->last_value)) bad = 1;
                if (!janet_gc_verify_young_marked("fiber-in", f->in_value)) bad = 1;
                if (!janet_gc_verify_young_marked("fiber-out", f->out_payload)) bad = 1;
            } else if (type == JANET_MEMORY_FUNCTION) {
                /* envs are raw pointers, always followed by mark_function
                 * (functions are never gated); env objects themselves are
                 * walked independently below. Nothing to check here. */
            } else if (type == JANET_MEMORY_FUNCDEF) {
                JanetFuncDef *def = (JanetFuncDef *) m;
                for (int32_t i = 0; i < def->constants_length; i++) {
                    if (!janet_gc_verify_young_marked("funcdef-const", def->constants[i])) bad = 1;
                }
            } else if (type == JANET_MEMORY_FUNCENV) {
                JanetFuncEnv *env = (JanetFuncEnv *) m;
                if (env->offset == 0 && env->as.values) {
                    for (int32_t i = 0; i < env->length; i++) {
                        if (!janet_gc_verify_young_marked("funcenv-value", env->as.values[i])) bad = 1;
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

/* Phase 1 density: remembered-set ownership filter. An xfb entry is
 * only relevant to a fiber minor if its target block lives on the
 * minor-ing fiber's OWN lists — foreign targets are kept alive by
 * their owners and cost a pointer compare to skip, versus a full
 * mark chain (proc -> fiber -> channels) per entry otherwise. The
 * 100k-agent profile: 185k live xfb entries dominated every minor.
 * Returns 1 when the entry must be marked. Non-GC values and
 * malloc-large blocks (no owning page) mark conservatively. */
static int janet_xfb_relevant(Janet x, uint32_t owner) {
    void *p;
    switch (janet_type(x)) {
        default:
            return 1;
        case JANET_ABSTRACT:
            p = janet_unwrap_abstract(x);
            break;
        case JANET_ARRAY:
            p = janet_unwrap_array(x);
            break;
        case JANET_TABLE:
            p = janet_unwrap_table(x);
            break;
        case JANET_FIBER:
            p = janet_unwrap_fiber(x);
            break;
    }
    if (NULL == p) return 1;
    JanetSlabPage *page = janet_slab_page_for(p);
    if (NULL == page) return 1;
    return janet_slab_obj_owner((JanetGCObject *) p) == owner;
}

void janet_collect_fiber_minor(JanetFiber *f) {
    JanetSlabState *ss;
    size_t i;
    if (janet_vm.gc_suspend) return;
    if (NULL == f || 0 == f->gc_owner) {
        /* D1 (plain-fiber form): the global minor never clears the
         * caller's gc_nursery_bytes. */
        janet_collect_minor();
        if (NULL != f) f->gc_nursery_bytes = 0;
        return;
    }
    ss = (JanetSlabState *) janet_vm.slab_state;
    if (NULL != ss && ss->xfb_stuck) {
        janet_collect_minor();
        f->gc_nursery_bytes = 0;
        return;
    }
    janet_gc_epoch_next();
    depth = JANET_RECURSION_GUARD;
    janet_vm.gc_mark_phase = 1;
    janet_vm.gc_minor_phase = 1;
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
    janet_mark_fiber(f);
    /* C-side VM roots: containers mutated by C code WITHOUT the janet
     * write barrier — the roots-free design's explicit pin set. The
     * profile table (PGO auto-collect runs it in every AOT binary)
     * holds function keys allocated young on whatever fiber was
     * running; unmarked, a child minor sweeps them and deinit's
     * profile_dump walks freed keys (the AOT teardown SEGV,
     * faulting in janet_profile_rows at 0x11). top_dyns is the same
     * class. Both are small; child minors are rare. */
    if (janet_vm.top_dyns) {
        janet_mark(janet_wrap_table(janet_vm.top_dyns));
    }
    if (janet_vm.profile_table) {
        janet_mark(janet_wrap_table(janet_vm.profile_table));
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
     * bulk proc fan-in lost-wakeup). janet_ev_mark is the same root
     * set every full collect applies; it is bounded by live queued
     * work, so it is cheap here. */
    janet_ev_mark();
    {
        size_t xi;
        for (xi = 0; xi < ss->xfb_count; xi++)
            if (janet_xfb_relevant(ss->xfb[xi], f->gc_owner))
                janet_mark(ss->xfb[xi]);
    }
    janet_vm.gc_mark_phase = 0;
    janet_sweep_heaps_owned(f);
    if (getenv("JANET_ROOT_TRACE")) {
        static long cfmi = 0;
        if (++cfmi <= 12 || 0 == cfmi % 2000)
            fprintf(stderr, "[CFM2] n=%ld fiber=%p owner=%u nurs=%d xfb=%d\n",
                    cfmi, (void *) f, f->gc_owner,
                    (int) f->gc_nursery_bytes, (int) ss->xfb_count);
    }
#ifdef JANET_GC_COUNT
    g_n_owned++;
#endif
    janet_vm.gc_minor_phase = 0;
    f->gc_nursery_bytes = 0;
}

/* P3.4 selective C roots for per-proc majors: everything mark_roots
 * covers EXCEPT fibers and the spawn queue (foreign stacks are the
 * point of skipping). Mirrors the shared parts (small, stable). */
static void janet_mark_proc_shared(uint32_t owner_id) {
    uint32_t i;
    size_t orig_rootcount = janet_vm.root_count;
    if (janet_vm.top_dyns) {
        janet_mark(janet_wrap_table(janet_vm.top_dyns));
    }
    if (janet_vm.profile_table) {
        janet_mark(janet_wrap_table(janet_vm.profile_table));
    }
    {
        JanetCompiler *comp;
        for (comp = janet_vm.compiler_stack; comp; comp = comp->compiler_next) {
            janet_mark_compiler(comp);
        }
    }
    for (i = 0; i < orig_rootcount; i++)
        janet_mark(janet_vm.roots[i]);
    for (i = 0; i < janet_vm.shadow_count; i++)
        janet_mark(janet_vm.shadow_roots[i]);
    /* Remembered cross-owner containers (holes route through here —
     * keep + traverse, never trust). */
    {
        JanetSlabState *xss = (JanetSlabState *) janet_vm.slab_state;
        if (NULL != xss) {
            size_t xi;
            /* Ownership filter: same rationale as the fiber minor. */
            for (xi = 0; xi < xss->xfb_count; xi++)
                if (janet_xfb_relevant(xss->xfb[xi], owner_id))
                    janet_mark(xss->xfb[xi]);
        }
    }
}

/* P3.4 verify support: does Janet value x point at object o
 * (object-start-exact, same head discipline as the barrier)? */
static int janet_verify_points_at(Janet x, JanetGCObject *o) {
    void *p = NULL;
    switch (janet_type(x)) {
        case JANET_STRING:
        case JANET_SYMBOL:
        case JANET_KEYWORD:
            return 0;
        case JANET_ARRAY:
            p = (void *) janet_unwrap_array(x);
            break;
        case JANET_TUPLE:
            p = (void *) janet_tuple_head(janet_unwrap_tuple(x));
            break;
        case JANET_TABLE:
            p = (void *) janet_unwrap_table(x);
            break;
        case JANET_STRUCT:
            p = (void *) janet_struct_head(janet_unwrap_struct(x));
            break;
        case JANET_BUFFER:
            p = (void *) janet_unwrap_buffer(x);
            break;
        case JANET_FUNCTION:
            p = (void *) janet_unwrap_function(x);
            break;
        case JANET_ABSTRACT:
            p = (void *) janet_abstract_head(janet_unwrap_abstract(x));
            break;
        case JANET_FIBER:
            p = (void *) janet_unwrap_fiber(x);
            break;
        default:
            return 0;
    }
    return p == (void *) o;
}

/* P3.4 verify (JANET_PROC_VERIFY=1, dev only): audit that no swept
 * candidate is reachable from a foreign stack (struct fields, stack
 * slots, fiber env tables shallow). Candidates were P-owned young
 * unmarked — any hit is a would-be-unsound free: report + abort. */
#define JANET_PROC_VERIFY_MAXCAND 4096
static void janet_proc_verify(JanetProcess *proc, JanetGCObject **cands, size_t ncand) {
    JanetSlabState *ss = (JanetSlabState *) janet_vm.slab_state;
    size_t ci, fi;
    if (NULL == ss) return;
    for (fi = 0; fi < ss->fibers_count; fi++) {
        JanetFiber *f = ss->fibers[fi];
        if (f == proc->fiber) continue;
        for (ci = 0; ci < ncand; ci++) {
            int32_t s;
            for (s = 0; s < f->stacktop; s++) {
                if (janet_verify_points_at(f->data[s], cands[ci])) {
                    fprintf(stderr, "proc-verify: foreign stack holds swept candidate %p\n",
                            (void *) cands[ci]);
                    abort();
                }
            }
            if (janet_verify_points_at(f->last_value, cands[ci]) ||
                    janet_verify_points_at(f->in_value, cands[ci]) ||
                    janet_verify_points_at(f->out_payload, cands[ci])) {
                fprintf(stderr, "proc-verify: foreign fiber field holds swept candidate %p\n",
                        (void *) cands[ci]);
                abort();
            }
            if (NULL != f->env) {
                int32_t k;
                for (k = 0; k < f->env->capacity; k++) {
                    JanetKV *kv = &f->env->data[k];
                    if (janet_verify_points_at(kv->key, cands[ci]) ||
                            janet_verify_points_at(kv->value, cands[ci])) {
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
 * JANET_PROC_VERIFY=1 audits for. No aging, no global pressure
 * reset, no weak dropping (minor rules); only the fiber's nursery
 * counter resets. Non-strict, unowned, or dead fibers degrade to a
 * full minor (never unsound, just global). Running fibers collect
 * like fiber minors (instruction-boundary stacks are consistent).
 */
void janet_collect_process(JanetProcess *proc) {
    JanetFiber *f;
    if (janet_vm.gc_suspend) return;
    /* Stuck remember set: cross-fiber stores past JANET_XFB_MAX go
     * unrecorded, so the narrow per-proc mark+sweep would free young
     * blocks still referenced from old foreign containers (the
     * ~900k-proc / ~225k-per-lane density crash: the next mark walk
     * hashed a reused slot). Degrade to the global minor exactly like
     * janet_collect_fiber_minor already does — a global minor marks
     * every registered fiber, so missing remembered edges are moot. */
    {
        JanetSlabState *dss = (JanetSlabState *) janet_vm.slab_state;
        if (NULL != dss && dss->xfb_stuck) {
            if (NULL != proc && NULL != (f = proc->fiber)) {
                f->gc_nursery_bytes = 0;
            }
            janet_collect_minor();
            return;
        }
    }
    if (NULL == proc || NULL == (f = proc->fiber) || 0 == f->gc_owner ||
            !proc->isolated_heap || !janet_fiber_can_resume(f)) {
        /* Phase 1.1 D1: the global minor never clears the caller's
         * nursery counter, so the next VM instruction re-fires this
         * path — an infinite loop of O(world) minors. Clear it here.
         * (A fiber-scoped fallback that pins proc fields is WRONG
         * here: pinning a done proc's channels keeps chan_deinit from
         * running, and its parked readers then wait forever.) */
        janet_collect_minor();
        if (NULL != f) f->gc_nursery_bytes = 0;
        return;
    }
    janet_gc_epoch_next();
    depth = JANET_RECURSION_GUARD;
    janet_vm.gc_mark_phase = 1;
    janet_vm.gc_minor_phase = 1;
    janet_mark_proc_shared(f->gc_owner);
    janet_mark_fiber(f);
    if (NULL != f->env) janet_mark(janet_wrap_table(f->env));
    if (NULL != proc->mbox) janet_mark(janet_wrap_abstract(proc->mbox));
    if (NULL != proc->sup) janet_mark(janet_wrap_abstract(proc->sup));
    if (NULL != proc->stash) janet_mark(janet_wrap_array(proc->stash));
    if (NULL != proc->monitors) janet_mark(janet_wrap_array(proc->monitors));
    janet_mark(proc->exit_reason);
    janet_vm.gc_mark_phase = 0;
    if (NULL != getenv("JANET_PROC_VERIFY")) {
        /* Gather P-owned young unmarked candidates (bounded sample).
         * P3.5b Slice A: candidates live in per-fiber lists too. */
        JanetGCObject *cands[JANET_PROC_VERIFY_MAXCAND];
        size_t ncand = 0;
        JanetSlabState *pss = (JanetSlabState *) janet_vm.slab_state;
        size_t pnfi = (NULL != pss) ? pss->fibers_count : 0;
        size_t ppass;
        for (ppass = 0; ppass <= pnfi && ncand < JANET_PROC_VERIFY_MAXCAND; ppass++) {
        JanetGCObject *lists[3];
        if (ppass == pnfi) {
            lists[0] = (JanetGCObject *) janet_vm.blocks;
            lists[1] = (JanetGCObject *) janet_vm.string_blocks;
            lists[2] = (JanetGCObject *) janet_vm.weak_blocks;
        } else {
            JanetFiber *pf = pss->fibers[ppass];
            lists[0] = (JanetGCObject *) pf->heap_lists[0];
            lists[1] = (JanetGCObject *) pf->heap_lists[1];
            lists[2] = (JanetGCObject *) pf->heap_lists[2];
        }
        int li;
        for (li = 0; li < 3; li++) {
            JanetGCObject *cur;
            for (cur = lists[li]; NULL != cur; cur = cur->data.next) {
                if (ncand == JANET_PROC_VERIFY_MAXCAND) break;
                if (janet_gc_reachable(cur) || (cur->flags & JANET_MEM_DISABLED)) continue;
                if (janet_slab_obj_old(cur)) continue;
                if (janet_slab_obj_owner(cur) != f->gc_owner) continue;
                cands[ncand++] = cur;
            }
        }
        }
        janet_proc_verify(proc, cands, ncand);
    }
    /* P3.5b Slice B: strict process collections sweep O(own) (narrow
     * marking already scoped them; see sweep_proc_owned). */
    janet_sweep_proc_owned(f);
#ifdef JANET_GC_COUNT
    g_n_proc++;
#endif
    janet_vm.gc_minor_phase = 0;
    f->gc_nursery_bytes = 0;
}

/* Minor collection (Phase 2b-iii policy): mark from the same roots (old-
 * clean tables/arrays prune via the gate; old→young edges arrive via
 * the barrier-dirtied pages, which are traversed like everything else),
 * then sweep young dead blocks only. Weak-ref dropping waits for majors
 * (conservative: weak-held young objects float until then). Afterwards
 * young pages age (cap old) and dirty flags clear. */
void janet_collect_minor(void) {
    if (janet_vm.gc_suspend) return;
#ifdef JANET_GC_COUNT
    g_n_minor++;
#endif
    /* Same invariant as full collects: marking requires all REACHABLE
     * bits clear. Scoped process collects set bits on blocks living in
     * OTHER fibers' lists and sweep only their own, so foreign bits can
     * be stale-set here; a conservative minor that early-returns on a
     * stale old container orphans every young object reachable only
     * through it (proven: closures mid-suite swept while live). */
    janet_gc_epoch_next();
    depth = JANET_RECURSION_GUARD;
    janet_vm.gc_mark_phase = 1;
    janet_vm.gc_minor_phase = 1;
    janet_mark_roots();
    janet_vm.gc_mark_phase = 0;
    if (getenv("JANET_GC_VERIFY")) {
        if (janet_gc_verify_minor()) {
            fprintf(stderr, "gc-verify: minor mark incomplete, aborting\n");
            abort();
        }
    }
    janet_sweep_heaps(0);
    janet_vm.gc_minor_phase = 0;
    /* Age surviving young pages. Dirty flags deliberately persist until
     * the next major: a young referee needs several minors to age out,
     * so clearing per-minor would drop coverage while it is still young
     * (old container + young entry swept live). Stale-dirty pages cost
     * only extra traversal, never correctness. */
    JanetSlabState *ss = (JanetSlabState *) janet_vm.slab_state;
    if (NULL != ss) {
        for (int i = 0; i < JANET_SLAB_NCLASSES; i++) {
            for (JanetSlabPage *p = ss->pages[i]; NULL != p; p = p->next) {
                if (p->age < JANET_SLAB_OLD_AGE) {
                    p->age++;
                    /* Promotion barrier (Phase 2b-iii): edges stored while
                     * the page was young were never write-barriered (young
                     * pages are always traversed, so the barrier correctly
                     * early-outs). Dirty the page exactly once at the
                     * young->old transition so its first minor as an old
                     * page traverses those edges; stores made while old are
                     * covered by the regular janet_gc_barrier. */
                    if (p->age == JANET_SLAB_OLD_AGE) p->dirty = 1;
                }
            }
        }
        janet_vm.gc_young_bytes = 0;
    }
}

/* Run garbage collection */
void janet_collect(void) {
#ifdef JANET_GC_COUNT
    fprintf(stderr, "gc-count: minor=%ld major=%ld owned=%ld proc=%ld\n",
            g_n_minor, g_n_major, g_n_owned, g_n_proc);
    fflush(stderr);
#endif
    if (janet_vm.gc_suspend) return;
#ifdef JANET_GC_COUNT
    g_n_major++;
#endif
    janet_gc_epoch_next();
    depth = JANET_RECURSION_GUARD;
    janet_vm.gc_mark_phase = 1;
    /* Try to prevent many major collections back to back.
     * A full collection will take O(janet_vm.block_count) time.
     * If we have a large heap, make sure our interval is not too
     * small so we won't make many collections over it. This is just a
     * heuristic for automatically changing the gc interval */
    if (janet_vm.block_count * 8 > janet_vm.gc_interval) {
        janet_vm.gc_interval = janet_vm.block_count * sizeof(JanetGCObject);
    }
    janet_mark_roots();
    janet_vm.gc_mark_phase = 0;
    janet_sweep();
    janet_vm.next_collection = 0;
    /* NOTE (Phase 2a root discipline): scratch memory is deliberately NOT
     * reclaimed here. Scratch lifetimes belong to their C scopes (compile
     * buffers, vectors, table storage, ...), which routinely span Janet
     * allocations and therefore collections; freeing it per-collect
     * invalidates live scratch (use-after-free, then an "invalid
     * janet_sfree" abort). Panic-leaked scratch is still recovered at
     * shutdown in janet_clear_memory. */
}

/* Add a root value to the GC. This prevents the GC from removing a value
 * and all of its children. If gcroot is called on a value n times, unroot
 * must also be called n times to remove it as a gc root. */
void janet_gcroot(Janet root) {
#ifdef JANET_ASAN_UAF_TRACE
    if (janet_checktype(root, JANET_FIBER)) {
        JanetFiber *rf = janet_unwrap_fiber(root);
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
                    (int) janet_fiber_status(rf));
            write(2, bbuf, (size_t) bl);
            void *bt[16];
            int bn = backtrace(bt, 16);
            backtrace_symbols_fd(bt, bn, 2);
        }
    }
#endif
    size_t newcount = janet_vm.root_count + 1;
    if (newcount > janet_vm.root_capacity) {
        size_t newcap = 2 * newcount;
        janet_vm.roots = janet_realloc(janet_vm.roots, sizeof(Janet) * newcap);
        if (NULL == janet_vm.roots) {
            JANET_OUT_OF_MEMORY;
        }
        janet_vm.root_capacity = newcap;
    }
    janet_vm.roots[janet_vm.root_count] = root;
    janet_vm.root_count = newcount;
}

/* Identity equality for GC purposes */
static int janet_gc_idequals(Janet lhs, Janet rhs) {
    if (janet_type(lhs) != janet_type(rhs))
        return 0;
    switch (janet_type(lhs)) {
        case JANET_BOOLEAN:
        case JANET_NIL:
        case JANET_NUMBER:
            /* These values don't really matter to the gc so returning 1 all the time is fine. */
            return 1;
        default:
            return janet_unwrap_pointer(lhs) == janet_unwrap_pointer(rhs);
    }
}

/* Scoped shadow roots for generated/C code (Phase 2b-i). Strictly LIFO:
 * snapshot with mark, discard with pop_to. Growth uses raw realloc so a
 * push itself can never trigger the collection it protects against. */
void janet_gcshadow_push(Janet x) {
    size_t newcount = janet_vm.shadow_count + 1;
    if (newcount > janet_vm.shadow_capacity) {
        size_t newcap = 2 * newcount;
        if (newcap < 8) newcap = 8;
        janet_vm.shadow_roots = janet_realloc(janet_vm.shadow_roots, sizeof(Janet) * newcap);
        if (NULL == janet_vm.shadow_roots) {
            JANET_OUT_OF_MEMORY;
        }
        janet_vm.shadow_capacity = newcap;
    }
    janet_vm.shadow_roots[janet_vm.shadow_count] = x;
    janet_vm.shadow_count = newcount;
}

size_t janet_gcshadow_mark(void) {
    return janet_vm.shadow_count;
}

void janet_gcshadow_pop_to(size_t mark) {
    if (mark > janet_vm.shadow_count) {
        janet_panicf("gcshadow mark %d beyond stack top %d", (int) mark, (int) janet_vm.shadow_count);
    }
    janet_vm.shadow_count = mark;
}

/* Remove a root value from the GC. This allows the gc to potentially reclaim
 * a value and all its children. */
int janet_gcunroot(Janet root) {
    /* Search from top to bottom as access is most likely LIFO */
    for (Janet *v = janet_vm.roots + janet_vm.root_count; v > janet_vm.roots;) {
        v--;
        if (janet_gc_idequals(root, *v)) {
            *v = janet_vm.roots[--janet_vm.root_count];
            return 1;
        }
    }
    return 0;
}

/* Remove a root value from the GC. This sets the effective reference count to 0. */
int janet_gcunrootall(Janet root) {
    int ret = 0;
    /* Search from top to bottom as access is most likely LIFO */
    for (Janet *v = janet_vm.roots + janet_vm.root_count; v > janet_vm.roots;) {
        v--;
        if (janet_gc_idequals(root, *v)) {
            *v = janet_vm.roots[--janet_vm.root_count];
            ret = 1;
        }
    }
    return ret;
}

/* Free all allocated memory */
void janet_clear_memory(void) {
    /* Threaded teardowns run inline from here on: the loop is gone,
     * so routed posts would block forever on the dead self-pipe. */
    janet_vm.gc_teardown_inline = 1;
    /* Drain the lane-homed quarantine (BUG 2 density pinning): run
     * finalizers, poison, and free. These blocks are malloc'd and
     * unlinked from the GC heaps, so plain release is safe. */
    {
        JanetAbstractHead *hq = janet_vm.home_quarantine;
        janet_vm.home_quarantine = NULL;
        while (NULL != hq) {
            JanetAbstractHead *next = (JanetAbstractHead *) hq->gc.data.next;
            if (NULL != hq->type && NULL != hq->type->gc) {
                janet_assert(!hq->type->gc(hq->data, hq->size),
                        "finalizer failed");
            }
            hq->type = NULL;
            janet_free(hq);
            hq = next;
        }
    }
    /* Quarantined blocks are intact but dead: destroy them before the
     * registry teardown walk (their deinit tombstones purge their
     * entries, and freed threaded heads read poisoned under the
     * guards). */
    janet_quarantine_drain();
#ifdef JANET_EV
    JanetKV *items = janet_vm.threaded_abstracts.data;
    for (int32_t i = 0; i < janet_vm.threaded_abstracts.capacity; i++) {
        if (janet_checktype(items[i].key, JANET_ABSTRACT)) {
            void *abst = janet_unwrap_abstract(items[i].key);
            JanetAbstractHead *head = janet_abstract_head(abst);
            /* Stale entry over a freed (poisoned) abstract: skip. */
            if (NULL == head->type) continue;
            if (head->type->gcperthread) {
                janet_assert(!head->type->gcperthread(head->data, head->size), "per-thread finalizer failed");
            }
            janet_abstract_decref_maybe_free(abst);
        }
    }
#endif
    /* P3.5b Slice A: drain per-fiber lists into the globals first
     * (fibers freed below then find empty lists, so the death
     * migration no-ops; no double-visit). */
    {
        JanetSlabState *ss = (JanetSlabState *) janet_vm.slab_state;
        if (NULL != ss) {
            size_t fi;
            for (fi = 0; fi < ss->fibers_count; fi++) {
                JanetFiber *f = ss->fibers[fi];
                int k;
                for (k = 0; k < 3; k++) {
                    JanetGCObject *fl = (JanetGCObject *) f->heap_lists[k];
                    if (NULL != fl) {
                        JanetGCObject *tail = fl;
                        while (NULL != tail->data.next)
                            tail = tail->data.next;
                        if (k == 0) {
                            tail->data.next = (JanetGCObject *) janet_vm.blocks;
                            janet_vm.blocks = (void *) fl;
                        } else if (k == 1) {
                            tail->data.next = (JanetGCObject *) janet_vm.string_blocks;
                            janet_vm.string_blocks = (void *) fl;
                        } else {
                            tail->data.next = (JanetGCObject *) janet_vm.weak_blocks;
                            janet_vm.weak_blocks = (void *) fl;
                        }
                        f->heap_lists[k] = NULL;
                    }
                }
            }
        }
    }
    JanetGCObject *current = janet_vm.blocks;
    while (NULL != current) {
        janet_deinit_block(current);
        JanetGCObject *next = current->data.next;
        janet_gc_release(current);
        current = next;
    }
    janet_vm.blocks = NULL;
    current = janet_vm.string_blocks;
    while (NULL != current) {
        janet_deinit_block(current);
        JanetGCObject *next = current->data.next;
        janet_gc_release(current);
        current = next;
    }
    janet_vm.string_blocks = NULL;
    /* With no live objects left, drop the slab pages themselves (frees
     * directory + freelists with them; fresh pages on next use). */
    janet_slab_free_pages();
    janet_free_all_scratch();
    janet_free(janet_vm.scratch_mem);
}

/* Primitives for suspending GC. */
int janet_gclock(void) {
    return janet_vm.gc_suspend++;
}
void janet_gcunlock(int handle) {
    janet_vm.gc_suspend = handle;
}

/* Scratch memory API
 * Scratch memory allocations do not need to be free (but optionally can be), and will be automatically cleaned
 * up in the next call to janet_collect. */

void *janet_smalloc(size_t size) {
    JanetScratch *s = janet_malloc(sizeof(JanetScratch) + size);
    if (NULL == s) {
        JANET_OUT_OF_MEMORY;
    }
    s->finalize = NULL;
    if (janet_vm.scratch_len == janet_vm.scratch_cap) {
        size_t newcap = 2 * janet_vm.scratch_cap + 2;
        JanetScratch **newmem = (JanetScratch **) janet_realloc(janet_vm.scratch_mem, newcap * sizeof(JanetScratch));
        if (NULL == newmem) {
            JANET_OUT_OF_MEMORY;
        }
        janet_vm.scratch_cap = newcap;
        janet_vm.scratch_mem = newmem;
    }
    janet_vm.scratch_mem[janet_vm.scratch_len++] = s;
    return (char *)(s->mem);
}

void *janet_scalloc(size_t nmemb, size_t size) {
    if (nmemb && size > SIZE_MAX / nmemb) {
        JANET_OUT_OF_MEMORY;
    }
    size_t n = nmemb * size;
    void *p = janet_smalloc(n);
    memset(p, 0, n);
    return p;
}

void *janet_srealloc(void *mem, size_t size) {
    if (NULL == mem) return janet_smalloc(size);
    JanetScratch *s = janet_mem2scratch(mem);
    if (janet_vm.scratch_len) {
        for (size_t i = janet_vm.scratch_len - 1; ; i--) {
            if (janet_vm.scratch_mem[i] == s) {
                JanetScratch *news = janet_realloc(s, size + sizeof(JanetScratch));
                if (NULL == news) {
                    JANET_OUT_OF_MEMORY;
                }
                janet_vm.scratch_mem[i] = news;
                return (char *)(news->mem);
            }
            if (i == 0) break;
        }
    }
    JANET_EXIT("invalid janet_srealloc");
}

void janet_sfinalizer(void *mem, JanetScratchFinalizer finalizer) {
    JanetScratch *s = janet_mem2scratch(mem);
    s->finalize = finalizer;
}

void janet_sfree(void *mem) {
    if (NULL == mem) return;
    JanetScratch *s = janet_mem2scratch(mem);
    if (janet_vm.scratch_len) {
        for (size_t i = janet_vm.scratch_len - 1; ; i--) {
            if (janet_vm.scratch_mem[i] == s) {
                janet_vm.scratch_mem[i] = janet_vm.scratch_mem[--janet_vm.scratch_len];
                free_one_scratch(s);
                return;
            }
            if (i == 0) break;
        }
    }
    JANET_EXIT("invalid janet_sfree");
}
