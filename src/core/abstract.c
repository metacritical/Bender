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
#include "util.h"
#include "gc.h"
#include "state.h"
#endif

#ifdef BENDER_EV
#ifdef BENDER_WINDOWS
#include <windows.h>
#endif
#endif

/* Create new userdata */
void *bender_abstract_begin(const BenderAbstractType *atype, size_t size) {
    BenderAbstractHead *header = bender_gcalloc(BENDER_MEMORY_NONE,
        sizeof(BenderAbstractHead) + size);
    header->size = size;
    header->type = atype;
    return (void *) & (header->data);
}

void *bender_abstract_end(void *x) {
    bender_gc_settype((void *)(bender_abstract_head(x)), BENDER_MEMORY_ABSTRACT);
    return x;
}

void *bender_abstract(const BenderAbstractType *atype, size_t size) {
    return bender_abstract_end(bender_abstract_begin(atype, size));
}

#ifdef BENDER_EV

/*
 * Threaded abstracts
 *
 * The registry lock covers both VM lifetime transitions and posting to a
 * live home VM. Never inspect a home pointer from an abstract without first
 * finding its address AND generation in this registry under the lock.
 */
#ifdef BENDER_WINDOWS
static SRWLOCK abstract_home_lock = SRWLOCK_INIT;
static void abstract_home_take(void) { AcquireSRWLockExclusive(&abstract_home_lock); }
static void abstract_home_drop(void) { ReleaseSRWLockExclusive(&abstract_home_lock); }
#else
static pthread_mutex_t abstract_home_lock = PTHREAD_MUTEX_INITIALIZER;
static void abstract_home_take(void) { pthread_mutex_lock(&abstract_home_lock); }
static void abstract_home_drop(void) { pthread_mutex_unlock(&abstract_home_lock); }
#endif
typedef struct BenderAbstractHomeEntry {
    BenderVM *vm;
    uint64_t generation;
    struct BenderAbstractHomeEntry *next;
} BenderAbstractHomeEntry;
static BenderAbstractHomeEntry *abstract_homes;
static uint64_t abstract_home_next_generation;

void bender_abstract_home_register(void) {
    BenderAbstractHomeEntry *entry = malloc(sizeof(*entry));
    if (NULL == entry) BENDER_OUT_OF_MEMORY;
    abstract_home_take();
    bender_vm.abstract_home_generation = ++abstract_home_next_generation;
    entry->vm = &bender_vm;
    entry->generation = bender_vm.abstract_home_generation;
    entry->next = abstract_homes;
    abstract_homes = entry;
    abstract_home_drop();
}

void bender_abstract_home_unregister(void) {
    BenderAbstractHomeEntry **link;
    abstract_home_take();
    for (link = &abstract_homes; *link; link = &(*link)->next) {
        if ((*link)->vm == &bender_vm &&
                (*link)->generation == bender_vm.abstract_home_generation) {
            BenderAbstractHomeEntry *removed = *link;
            *link = removed->next;
            free(removed);
            break;
        }
    }
    abstract_home_drop();
}

void *bender_abstract_begin_threaded(const BenderAbstractType *atype, size_t size) {
    BenderAbstractHead *header = bender_malloc(sizeof(BenderAbstractHead) + size);
    if (NULL == header) {
        BENDER_OUT_OF_MEMORY;
    }
    bender_vm.next_collection += size + sizeof(BenderAbstractHead);
    header->gc.flags = BENDER_MEMORY_THREADED_ABSTRACT;
    header->gc.data.next = NULL; /* Clear memory for address sanitizers */
    header->gc.data.refcount = 1;
    header->size = size;
    header->type = atype;
    header->home_vm = (void *) &bender_vm;
    header->home_vm_generation = bender_vm.abstract_home_generation;
    header->teardown_started = 0;
    header->exported = 0;
    /* No complete home major has disproved a Bender-visible hold yet.
     * Partial collections must not authorize destruction of a newly
     * created abstract whose nested roots have not been traced. */
    header->home_gc_authorized = 1;
    void *abstract = (void *) & (header->data);
    bender_table_put(&bender_vm.threaded_abstracts, bender_wrap_abstract(abstract), bender_wrap_false());
    return abstract;
}

void *bender_abstract_end_threaded(void *x) {
    bender_gc_settype((void *)(bender_abstract_head(x)), BENDER_MEMORY_THREADED_ABSTRACT);
    return x;
}

void *bender_abstract_threaded(const BenderAbstractType *atype, size_t size) {
    return bender_abstract_end_threaded(bender_abstract_begin_threaded(atype, size));
}

/* Refcounting primitives and sync primitives */

#ifdef BENDER_WINDOWS

size_t bender_os_mutex_size(void) {
    return sizeof(CRITICAL_SECTION);
}

size_t bender_os_rwlock_size(void) {
    return sizeof(void *);
}

void bender_os_mutex_init(BenderOSMutex *mutex) {
    InitializeCriticalSection((CRITICAL_SECTION *) mutex);
}

void bender_os_mutex_deinit(BenderOSMutex *mutex) {
    DeleteCriticalSection((CRITICAL_SECTION *) mutex);
}

void bender_os_mutex_lock(BenderOSMutex *mutex) {
    EnterCriticalSection((CRITICAL_SECTION *) mutex);
}

void bender_os_mutex_unlock(BenderOSMutex *mutex) {
    /* error handling? May want to keep counter */
    LeaveCriticalSection((CRITICAL_SECTION *) mutex);
}

void bender_os_rwlock_init(BenderOSRWLock *rwlock) {
    InitializeSRWLock((PSRWLOCK) rwlock);
}

void bender_os_rwlock_deinit(BenderOSRWLock *rwlock) {
    /* no op? */
    (void) rwlock;
}

void bender_os_rwlock_rlock(BenderOSRWLock *rwlock) {
    AcquireSRWLockShared((PSRWLOCK) rwlock);
}

void bender_os_rwlock_wlock(BenderOSRWLock *rwlock) {
    AcquireSRWLockExclusive((PSRWLOCK) rwlock);
}

void bender_os_rwlock_runlock(BenderOSRWLock *rwlock) {
    ReleaseSRWLockShared((PSRWLOCK) rwlock);
}

void bender_os_rwlock_wunlock(BenderOSRWLock *rwlock) {
    ReleaseSRWLockExclusive((PSRWLOCK) rwlock);
}

#else

size_t bender_os_mutex_size(void) {
    return sizeof(pthread_mutex_t);
}

size_t bender_os_rwlock_size(void) {
    return sizeof(pthread_rwlock_t);
}

void bender_os_mutex_init(BenderOSMutex *mutex) {
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init((pthread_mutex_t *) mutex, &attr);
}

void bender_os_mutex_deinit(BenderOSMutex *mutex) {
    pthread_mutex_destroy((pthread_mutex_t *) mutex);
}

void bender_os_mutex_lock(BenderOSMutex *mutex) {
    pthread_mutex_lock((pthread_mutex_t *) mutex);
}

void bender_os_mutex_unlock(BenderOSMutex *mutex) {
    int ret = pthread_mutex_unlock((pthread_mutex_t *) mutex);
    if (ret) bender_panic("cannot release lock");
}

void bender_os_rwlock_init(BenderOSRWLock *rwlock) {
    pthread_rwlock_init((pthread_rwlock_t *) rwlock, NULL);
}

void bender_os_rwlock_deinit(BenderOSRWLock *rwlock) {
    pthread_rwlock_destroy((pthread_rwlock_t *) rwlock);
}

void bender_os_rwlock_rlock(BenderOSRWLock *rwlock) {
    pthread_rwlock_rdlock((pthread_rwlock_t *) rwlock);
}

void bender_os_rwlock_wlock(BenderOSRWLock *rwlock) {
    pthread_rwlock_wrlock((pthread_rwlock_t *) rwlock);
}

void bender_os_rwlock_runlock(BenderOSRWLock *rwlock) {
    pthread_rwlock_unlock((pthread_rwlock_t *) rwlock);
}

void bender_os_rwlock_wunlock(BenderOSRWLock *rwlock) {
    pthread_rwlock_unlock((pthread_rwlock_t *) rwlock);
}

#endif

int32_t bender_abstract_incref(void *abst) {
    return bender_atomic_inc(&bender_abstract_head(abst)->gc.data.refcount);
}

int32_t bender_abstract_decref(void *abst) {
    return bender_atomic_dec(&bender_abstract_head(abst)->gc.data.refcount);
}

#ifdef BENDER_EV
/* BUG 2: release a threaded abstract on its HOME VM's thread. The
 * last registry claim can be dropped by a foreign VM's sweep; the
 * home VM may still have in-flight operations touching the block
 * (serials bookkeeping, monitor lookups), so the free must not run
 * on the foreign thread. */
static void bender_abstract_release_cb(BenderEVGenericMessage msg) {
    bender_gc_release((BenderGCObject *) msg.argp);
}

/* Full teardown (finalizer + poison + registry tombstone + release),
 * executed on the abstract's HOME VM. In-flight operations on the
 * home VM run between loop iterations, never mid-fiber, so a fiber
 * hashing or comparing the abstract never sees the poison. */
void bender_abstract_home_teardown_cb(BenderEVGenericMessage msg) {
    void *abst = msg.argp;
    BenderAbstractHead *head = bender_abstract_head(abst);
    /* Single-shot: the sweep's deinit_block may have torn the
     * abstract down first (guard set there); skip cleanly. */
    if (1 != bender_atomic_inc(&head->teardown_started)) return;
    /* GC-visible hold deferral (BUG 2 density): a value can sit in a
     * same-VM channel queue / parked payload / table with no
     * registry claim backing it. Authorization starts conservative and
     * only a completed unvisited home major revokes it. Re-arm a single
     * registry watch claim: its value is false, so without another home
     * major mark the next sweep revokes authorization and drops the claim
     * rather than keeping the object alive indefinitely. */
    if (head->home_gc_authorized) {
        bender_atomic_inc(&head->gc.data.refcount);
        bender_table_put(&bender_vm.threaded_abstracts,
                bender_wrap_abstract(abst), bender_wrap_false());
        head->teardown_started = 0;
        return;
    }
    /* Lane-homed pinning (BUG 2 density): a lane's own abstracts are
     * referenced claim-less by circulating pid values (channel queue
     * items, parked payloads, serials). Freeing at refcount-0 dangles
     * them; lanes pin instead and bender_clear_memory drains the
     * quarantine at lane stop. Main-VM teardowns keep prompt
     * semantics (suite-ev2's weakref/count tests). */
    if (bender_vm.is_lane_vm || head->exported) {
        /* Lane-homed: circulating pid values. Exported (ever
         * marshaled cross-VM): a foreign lane may hold claim-less
         * values even when homed on main. Both pin until exit. */
        head->gc.data.next = bender_vm.home_quarantine;
        bender_vm.home_quarantine = head;
        return;
    }
    if (NULL != head->type && NULL != head->type->gc) {
        bender_assert(!head->type->gc(head->data, head->size),
                "finalizer failed");
    }
    head->type = NULL;
    /* No registry tombstone here: bender_table_put can allocate and
     * trigger a collection from inside the event loop's callback
     * (racing the channel GC's pending-queue walk). The observing
     * VM's purge tombstones its own entry after its decref, and the
     * home VM's purge purges the poisoned-type entry in place. */
    bender_gc_release((BenderGCObject *) head);
}
#endif

int32_t bender_abstract_decref_maybe_free(void *abst) {
    int32_t result = bender_abstract_decref(abst);
    if (0 == result) {
#ifdef BENDER_ASAN_UAF_TRACE
        {
        extern int backtrace(void **, int);
        extern void backtrace_symbols_fd(void *const *, int, int);
        BenderAbstractHead *ph = bender_abstract_head(abst);
            if (ph->gc.flags & BENDER_MEMORY_THREADED_ABSTRACT) {
                fprintf(stderr, "[xthread-free] abstract=%p size=%d\n",
                        abst, (int) ph->size);
                void *bt[16];
                int bn = backtrace(bt, 16);
                backtrace_symbols_fd(bt, bn, 2);
            }
        }
#endif
        BenderAbstractHead *head = bender_abstract_head(abst);
        /* ROUTING FIRST: every threaded teardown flows through the
         * home callback -- finalizer + poison + release + the
         * GC-visible-hold deferral live there under the single-shot
         * guard. (Running the finalizer inline here freed channel
         * queue buffers on the observing thread and the home VM's
         * routed callback re-ran the deinit on freed memory -- ASAN:
         * flag_chanpend_node/bender_chan_deinit_q UAF at density.)
         * Non-threaded abstracts are single-VM: teardown inline. */
        if (head->gc.flags & BENDER_MEMORY_THREADED_ABSTRACT) {
            /* Shutdown cannot wait for a foreign home to drain its event
             * pipe: the home may be busy collecting its own fibers (or
             * already stopped), and a full pipe blocks this VM's teardown
             * indefinitely. Nor may we finalize here: home values can
             * still point into this block. Leave this last claim allocated
             * until process exit instead. Normal, live-VM claims still
             * route home for prompt finalization. */
            if (bender_vm.gc_teardown_inline &&
                    (head->home_vm != (void *) &bender_vm ||
                     head->home_vm_generation != bender_vm.abstract_home_generation)) {
                return result;
            }
            if (bender_vm.gc_teardown_inline) {
                /* Home shutdown: the loop is gone. Inline teardown under
                 * the single-shot guard (skip the reachability
                 * deferral -- GC state is being destroyed anyway). */
                if (1 != bender_atomic_inc(&head->teardown_started)) {
                    return result;
                }
                if (NULL != head->type->gc) {
                    bender_assert(!head->type->gc(head->data, head->size),
                            "finalizer failed");
                }
                head->type = NULL;
                bender_gc_release((BenderGCObject *) head);
                return result;
            }
            /* A home may enter teardown (or leave and have its TLS VM
             * address reused) between this foreign sweep and the post.
             * Resolve identity under the lifetime lock; hold it through
             * the write so shutdown cannot close the pipe mid-post. A
             * stopped home cannot safely finalize foreign-held values:
             * retain this last block until process exit instead. */
            abstract_home_take();
            BenderAbstractHomeEntry *entry = abstract_homes;
            while (entry && (entry->vm != head->home_vm ||
                    entry->generation != head->home_vm_generation))
                entry = entry->next;
            if (NULL == entry) {
                abstract_home_drop();
                return result;
            }
            BenderVM *target = entry->vm;
            if (target->gc_teardown_inline) {
                abstract_home_drop();
                return result;
            }
            BenderEVGenericMessage msg;
            memset(&msg, 0, sizeof(msg));
            msg.argp = abst;
            bender_ev_post_event(target, bender_abstract_home_teardown_cb, msg);
            abstract_home_drop();
            return result;
        }
        if (head->type->gc) {
            bender_assert(!head->type->gc(head->data, head->size), "finalizer failed");
        }
        /* Poison the header before release: any registry entry or stale
         * heap reference that outlives the abstract (cross-thread frees,
         * scoped sweeps, teardown interleavings) must read type == NULL
         * instead of recycled memory, so the gc.c walks and markers can
         * identify the dead object instead of dereferencing freed data. */
        head->type = NULL;
        /* Free memory via the slab-aware release: threaded abstracts can
         * be freed outside sweep (and retagged post-alloc), so plain
         * bender_free would corrupt slab pages. */
        bender_gc_release((BenderGCObject *) head);
    }
    return result;
}

#endif
