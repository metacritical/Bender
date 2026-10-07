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
#include "symcache.h"
#include "gc.h"
#include "util.h"
#include "fiber.h"
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

/* Generational gate (Phase 2b-iii policy), defined with the slab code
 * below; used by the table/array markers. */
static int janet_gc_minor_skip(JanetGCObject *mem);
/* Object old for generational purposes (slab page age, or malloc-large);
 * defined with the slab code below. */
static int janet_slab_obj_old(void *ptr);
/* Reset generational slab state after a full sweep; defined below. */
static void janet_slab_major_reset(void);

/* Hint to the GC that we may need to collect */
void janet_gcpressure(size_t s) {
    janet_vm.next_collection += s;
}

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
#ifdef JANET_EV
    /* Check if abstract type is a threaded abstract type. If it is, marking means
     * updating the threaded_abstract table. */
    if ((janet_abstract_head(adata)->gc.flags & JANET_MEM_TYPEBITS) == JANET_MEMORY_THREADED_ABSTRACT) {
        janet_table_put(&janet_vm.threaded_abstracts, janet_wrap_abstract(adata), janet_wrap_true());
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
    janet_gc_mark(fiber);

    janet_mark(fiber->last_value);
    /* Values in flight across a context switch. The resume argument and
     * the last result live in the fiber struct (not in fiber->data), so
     * they must be marked explicitly or a collection between suspend and
     * resume could free them. */
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

    /* Mark values on the argument stack */
    janet_mark_many(fiber->data + fiber->stackstart,
                    fiber->stacktop - fiber->stackstart);

    i = fiber->frame;
    j = fiber->stackstart - JANET_FRAME_SIZE;
    while (i > 0) {
        frame = (JanetStackFrame *)(fiber->data + i - JANET_FRAME_SIZE);
        if (NULL != frame->func)
            janet_mark_function(frame->func);
        if (NULL != frame->env)
            janet_mark_funcenv(frame->env);
        /* Mark all values in the stack frame */
        janet_mark_many(fiber->data + i, j - i);
        j = i - JANET_FRAME_SIZE;
        i = frame->prevframe;
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
#endif

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
        case JANET_FIBER:
            return janet_gc_reachable(janet_unwrap_pointer(x));
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
 * to symbols); otherwise it must be neither -- misrouting is a bug. */
static void janet_sweep_list(JanetGCObject **head, int heap_kind, int is_minor) {
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
        if (current->flags & (JANET_MEM_REACHABLE | JANET_MEM_DISABLED)) {
            previous = current;
            current->flags &= ~JANET_MEM_REACHABLE;
        } else if (!is_minor || !janet_slab_obj_old(current)) {
            janet_vm.block_count--;
            janet_deinit_block(current);
            if (NULL != previous) {
                previous->data.next = next;
            } else {
                *head = next;
            }
            janet_gc_release(current);
        } else {
            previous = current;
        }
        current = next;
    }
}

void janet_sweep_weak_drop(void) {
    /* Weak-ref dropping runs at majors only. In minors the mark is
     * partial (old heap unscanned), so liveness verdicts would be wrong;
     * weak tables are instead traversed conservatively via the normal
     * mark exemption for weak types, and clearing waits for a major. */
    if (janet_vm.gc_minor_phase) return;
    JanetGCObject *current = janet_vm.weak_blocks;
    JanetGCObject *next;
    while (NULL != current) {
        next = current->data.next;
        if (current->flags & (JANET_MEM_REACHABLE | JANET_MEM_DISABLED)) {
            /* Check for dead references */
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
 * below also runs at majors only. */
static void janet_sweep_heaps(void) {
    /* Sweep weak heap to free blocks (minor-aware via helper). */
    janet_sweep_list((JanetGCObject **) &janet_vm.weak_blocks, 2, janet_vm.gc_minor_phase);

    /* Sweep both strong heaps with identical semantics (Phase 2b-ii):
     * the main heap, then the immutable-string heap. */
    janet_sweep_list((JanetGCObject **) &janet_vm.blocks, 0, janet_vm.gc_minor_phase);
    janet_sweep_list((JanetGCObject **) &janet_vm.string_blocks, 1, janet_vm.gc_minor_phase);
}

void janet_sweep(void) {
    janet_sweep_weak_drop();
    janet_sweep_heaps();

#ifdef JANET_EV
    /* Sweep threaded abstract types for references to decrement */
    JanetKV *items = janet_vm.threaded_abstracts.data;
    for (int32_t i = 0; i < janet_vm.threaded_abstracts.capacity; i++) {
        if (janet_checktype(items[i].key, JANET_ABSTRACT)) {

            /* If item was not visited during the mark phase, then this
             * abstract type isn't present in the heap and needs its refcount
             * decremented, and shouuld be removed from table. If the refcount is
             * then 0, the item will be collected. This ensures that only one interpreter
             * will clean up the threaded abstract. */

            /* If not visited... */
            if (!janet_truthy(items[i].value)) {
                void *abst = janet_unwrap_abstract(items[i].key);
                JanetAbstractHead *head = janet_abstract_head(abst);
                if (head->type->gcperthread) {
                    janet_assert(!head->type->gcperthread(head->data, head->size), "per-thread finalizer failed");
                }
                janet_abstract_decref_maybe_free(abst);

                /* Mark as tombstone in place */
                items[i].key = janet_wrap_nil();
                items[i].value = janet_wrap_false();
                janet_vm.threaded_abstracts.deleted++;
                janet_vm.threaded_abstracts.count--;
            }

            /* Reset for next sweep */
            items[i].value = janet_wrap_false();
        }
    }
#endif
    /* A full sweep leaves every survivor old: reset all page ages and
     * merge the old freelists back so old slots become reusable young
     * slots (conservative restart for the next minor cycle). */
    janet_slab_major_reset();
}

/* Allocate some memory that is tracked for garbage collection */
/* Phase 2b-iii slab allocator, generational (Spinel storage model).
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
#define JANET_SLAB_NCLASSES 8
#define JANET_SLAB_OLD_AGE 2

/* Bytes of young allocation between minor collections. */
#define JANET_MINOR_THRESHOLD (256 * 1024)

static const size_t janet_slab_classes[JANET_SLAB_NCLASSES] =
    {32, 64, 96, 128, 192, 256, 384, 512};

typedef struct JanetSlabPage {
    struct JanetSlabPage *next;
    uint32_t magic;
    int32_t class_index;
    int32_t age;
    int32_t dirty;
    int32_t bump;
} JanetSlabPage;

#define JANET_SLAB_USABLE (JANET_SLAB_PAGE_SIZE - sizeof(JanetSlabPage))
#define JANET_SLAB_SLOTS(p) ((void *)((char *)(p) + sizeof(JanetSlabPage)))

typedef struct {
    JanetSlabPage *pages[JANET_SLAB_NCLASSES];
    JanetSlabPage *cur[JANET_SLAB_NCLASSES];
    JanetGCObject *young_free[JANET_SLAB_NCLASSES];
    JanetGCObject *old_free[JANET_SLAB_NCLASSES];
    /* Sorted directory of live page bases for exact membership: the
     * mask+magic probe below must never read unmapped memory, and not
     * every candidate pointer lives on a slab page (malloc large path,
     * foreign memory). Pages are added rarely (one per 16 KiB of same
     * class) and freed en-masse, so a sorted array with binary search
     * is cheap and exact. Raw allocation (never triggers collections). */
    JanetSlabPage **dir;
    size_t dir_count;
    size_t dir_cap;
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

static void *janet_slab_alloc(int class_index) {
    JanetSlabState *ss = janet_slab_state();
    size_t slot = janet_slab_classes[class_index];
    /* Pop a young slot, skipping (to the old list) any slot whose page
     * has aged out since the slot was freed. Self-correcting: every pop
     * either uses or re-files the head, so it always terminates. */
    for (;;) {
        JanetGCObject *slot_obj = ss->young_free[class_index];
        if (NULL == slot_obj) break;
        ss->young_free[class_index] = slot_obj->data.next;
        JanetSlabPage *page = janet_slab_page_for(slot_obj);
        if (NULL != page && page->age < JANET_SLAB_OLD_AGE) {
            /* TEMPORARY 2b-iii bisect: zero recycled slots to test for
             * uninitialized-field reads. */
            memset(slot_obj, 0, slot);
            return slot_obj;
        }
        slot_obj->data.next = (JanetGCObject *) ss->old_free[class_index];
        ss->old_free[class_index] = slot_obj;
    }
    JanetSlabPage *page = ss->cur[class_index];
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
        if (!janet_slab_dir_insert(page)) {
            janet_slab_page_free(page);
            return NULL;
        }
        ss->pages[class_index] = page;
        ss->cur[class_index] = page;
    }
    void *out = (char *) JANET_SLAB_SLOTS(page) + page->bump;
    page->bump += (int32_t) slot;
    janet_vm.gc_young_bytes += slot;
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
                mem->data.next = (JanetGCObject *) ss->young_free[cls];
                ss->young_free[cls] = mem;
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
    }
    janet_free(ss->dir);
    ss->dir = NULL;
    ss->dir_count = 0;
    ss->dir_cap = 0;
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

/* Generational write barrier (Phase 2b-iii policy): record an old
 * container that newly references a heap value, so minor collections
 * (which skip clean old tables/arrays) still find the edge. Immediates
 * (nil/boolean/number/cfunction/pointer) never create edges; young and
 * large containers are always traversed, so only old slab pages are
 * marked dirty. Call after every store of a Janet value into a heap
 * table, array, or equivalent long-lived container. */
void janet_gc_barrier(void *obj, Janet value) {
    JanetType t = janet_type(value);
    if (t == JANET_NIL || t == JANET_BOOLEAN || t == JANET_NUMBER ||
            t == JANET_CFUNCTION || t == JANET_POINTER) {
        return;
    }
    JanetSlabPage *page = janet_slab_page_for(obj);
    if (NULL == page) return;
    if (page->age < JANET_SLAB_OLD_AGE) return;
    page->dirty = 1;
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

    /* Prepend block to heap list. Immutable strings (strings, symbols,
     * keywords -- keywords intern to symbols) live on their own heap so
     * collection policy can treat them independently (Phase 2b-ii). */
    janet_vm.next_collection += size;
    if (type == JANET_MEMORY_STRING || type == JANET_MEMORY_SYMBOL) {
        mem->data.next = janet_vm.string_blocks;
        janet_vm.string_blocks = mem;
    } else if (type < JANET_MEMORY_TABLE_WEAKK) {
        /* normal heap */
        mem->data.next = janet_vm.blocks;
        janet_vm.blocks = mem;
    } else {
        /* weak heap */
        mem->data.next = janet_vm.weak_blocks;
        janet_vm.weak_blocks = mem;
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
    if (m->flags & JANET_MEM_REACHABLE) return 1;
    fprintf(stderr, "gc-verify: young unmarked %p via %s\n", (void *) m, holder);
    return 0;
}

static int janet_gc_verify_minor(void) {
    int bad = 0;
    /* NOTE: janet_vm.blocks etc. are void* in the struct; cast through. */
    void *heads[3] = {janet_vm.blocks, janet_vm.string_blocks, janet_vm.weak_blocks};
    for (int h = 0; h < 3; h++) {
        for (JanetGCObject *m = (JanetGCObject *) heads[h]; NULL != m; m = m->data.next) {
            /* Only live holders matter: an unmarked holder's subgraph is
             * garbage as a whole (its young referents are correctly
             * unmarked too). Disabled objects are always kept+traversed,
             * so their edges count as live. */
            if (!(m->flags & (JANET_MEM_REACHABLE | JANET_MEM_DISABLED)) &&
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
    return bad;
}

/* Minor collection (Phase 2b-iii policy): mark from the same roots (old-
 * clean tables/arrays prune via the gate; old→young edges arrive via
 * the barrier-dirtied pages, which are traversed like everything else),
 * then sweep young dead blocks only. Weak-ref dropping waits for majors
 * (conservative: weak-held young objects float until then). Afterwards
 * young pages age (cap old) and dirty flags clear. */
void janet_collect_minor(void) {
    if (janet_vm.gc_suspend) return;
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
    janet_sweep_heaps();
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
    if (janet_vm.gc_suspend) return;
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
#ifdef JANET_EV
    JanetKV *items = janet_vm.threaded_abstracts.data;
    for (int32_t i = 0; i < janet_vm.threaded_abstracts.capacity; i++) {
        if (janet_checktype(items[i].key, JANET_ABSTRACT)) {
            void *abst = janet_unwrap_abstract(items[i].key);
            JanetAbstractHead *head = janet_abstract_head(abst);
            if (head->type->gcperthread) {
                janet_assert(!head->type->gcperthread(head->data, head->size), "per-thread finalizer failed");
            }
            janet_abstract_decref_maybe_free(abst);
        }
    }
#endif
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
