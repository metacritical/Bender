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

#ifndef BENDER_GC_H
#define BENDER_GC_H

#ifndef BENDER_AMALG
#include "features.h"
#include <bender.h>
#endif

/* Slab size classes (ceiling byte sizes). Shared with the per-fiber
 * nursery cursor array in BenderFiber (sized by literal there). */
#define BENDER_SLAB_NCLASSES 8

/* The metadata header associated with an allocated block of memory */
#define bender_gc_header(mem) ((BenderGCObject *)(mem))

#define BENDER_MEM_TYPEBITS 0xFF
#define BENDER_MEM_REACHABLE 0x100
#define BENDER_MEM_DISABLED 0x200
/* P3.3 strict heaps: set when the owning fiber is strict (spawn-
 * isolated). Slab objects inherit their page's bit (claim-time, so
 * recycled slots agree with the page owner); malloc-large objects
 * take the allocating fiber's flag. Read by the barrier for the
 * export rule; never cleared except by recycle-zero + recompute. */
#define BENDER_MEM_ISOLATED 0x400

#define bender_gc_settype(m, t) ((bender_gc_header(m)->flags |= (0xFF & (t))))
#define bender_gc_type(m) (bender_gc_header(m)->flags & 0xFF)

#define BENDER_MEM_EPOCH_MASK 0xF800
#define BENDER_MEM_EPOCH_SHIFT 11

#define bender_gc_mark(m) (bender_gc_header(m)->flags |= BENDER_MEM_REACHABLE)
#define bender_gc_reachable(m) (bender_gc_header(m)->flags & BENDER_MEM_REACHABLE)

/* Memory types for the GC. Different from BenderType to include funcenv and funcdef. */
enum BenderMemoryType {
    BENDER_MEMORY_NONE,
    BENDER_MEMORY_STRING,
    BENDER_MEMORY_SYMBOL,
    BENDER_MEMORY_ARRAY,
    BENDER_MEMORY_TUPLE,
    BENDER_MEMORY_TABLE,
    BENDER_MEMORY_STRUCT,
    BENDER_MEMORY_FIBER,
    BENDER_MEMORY_BUFFER,
    BENDER_MEMORY_FUNCTION,
    BENDER_MEMORY_ABSTRACT,
    BENDER_MEMORY_FUNCENV,
    BENDER_MEMORY_FUNCDEF,
    BENDER_MEMORY_THREADED_ABSTRACT,
    BENDER_MEMORY_TABLE_WEAKK,
    BENDER_MEMORY_TABLE_WEAKV,
    BENDER_MEMORY_TABLE_WEAKKV,
    BENDER_MEMORY_ARRAY_WEAK
};

/* To allocate collectable memory, one must call bender_alloc, initialize the memory,
 * and then call when bender_enablegc when it is initialized and reachable by the gc (on the BENDER stack) */
void *bender_gcalloc(enum BenderMemoryType type, size_t size);

/* Minor (nursery) collection: mark from the full root set with old-clean
 * tables/arrays pruned via the gate, then sweep young dead blocks only.
 * Triggered by nursery pressure (see maybe_collect); explicit collects
 * stay major. */
void bender_collect_minor(void);

/* Step 1.3 per-fiber minor (see implementation for the soundness rule).
 * Falls back to a full minor for unowned fibers and stuck sets. */
void bender_collect_fiber_minor(BenderFiber *f);

/* P3.4 per-proc major (see implementation): frees only the target
 * strict proc's young unmarked (foreign young floats). Falls back to
 * a full minor for non-strict/dead/running fibers. */
struct BenderProcess;
void bender_collect_process(struct BenderProcess *proc);

/* Weak-ref dropping at majors (sweep helper, defined with the slab
 * code so it can walk per-fiber weak lists). */
void bender_sweep_weak_drop(void);

/* Generational write barrier (Phase 2b-iii policy): record an old
 * container that newly references a heap value, so minor collections
 * (which skip clean old tables/arrays) still find the edge. Immediates
 * and young containers need nothing; large/stack objects are always
 * traversed. Call after every store of a Bender value into a heap table,
 * array, or equivalent long-lived container. */
void bender_gc_barrier(void *obj, Bender value);

/* Release a GC object back to its backing store (Phase 2b-iii): either a
 * slab slot (returned to the class free list) or plain malloc (freed).
 * All whole-object frees must go through here; subsidiary buffers still
 * use bender_free directly. */
void bender_gc_release(BenderGCObject *mem);


/* P3.4 bookkeeping puts (marshal memo, threaded registry): mark an old
 * container dirty without barrier duties (no xfb record, no export
 * panic). Dirty is always safe (over-traversal); the skipped duties
 * would false-panic on transient identity maps holding strict refs. */
void bender_gc_touch(void *obj);

/* Step 1 introspection (process heaps): per-owner slab page counts as
 * @{ :pages total :unowned n :owned @{id count} }. */
BenderTable *bender_slab_owner_stats(void);

/* Step 1.3 live-fiber census for per-fiber minors (append at birth in
 * fiber_alloc/unmarshal, swap-remove at sweep-free). */
void bender_census_add(BenderFiber *f);

/* Debug (BENDER_TRACE_STRICT): dump a copied entry's funcenvs and the
 * slab-page ownership of table upvalues. */
void bender_strict_debug_envs(BenderFunction *copy);

/* Erlang-parity messaging fix: protect a QUEUED (not-yet-resumed) task
 * fiber's own GC block from a narrow, unrelated scoped collection's
 * sweep, WITHOUT deep-walking its stack. A scoped fiber/process minor
 * only sweeps its OWN heap; a foreign queued fiber's stack contents are
 * that fiber's own collection's responsibility, not this one's. Deep-
 * marking it (the full bender_mark_fiber recursion) was confirmed via
 * live lldb backtraces to make every actor's own per-actor minor walk
 * the ENTIRE stack of whatever fiber the event loop has queued (e.g. a
 * driver script parked on ev/sleep mid spawn-burst, with a large live
 * handle array on its stack) -- O(N) per minor x N actors = O(N^2). See
 * docs/internals/proc-lanes.md. Only sets the reachable bit; does not
 * recurse into the fiber's frames/stack/env. */
void bender_gc_mark_fiber_shallow(BenderFiber *fiber);

#endif
