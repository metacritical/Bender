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
#include "util.h"
#include "gc.h"
#include "state.h"
#endif

#ifdef JANET_EV
#ifdef JANET_WINDOWS
#include <windows.h>
#endif
#endif

/* Create new userdata */
void *janet_abstract_begin(const JanetAbstractType *atype, size_t size) {
    JanetAbstractHead *header = janet_gcalloc(JANET_MEMORY_NONE,
        sizeof(JanetAbstractHead) + size);
    header->size = size;
    header->type = atype;
    return (void *) & (header->data);
}

void *janet_abstract_end(void *x) {
    janet_gc_settype((void *)(janet_abstract_head(x)), JANET_MEMORY_ABSTRACT);
    return x;
}

void *janet_abstract(const JanetAbstractType *atype, size_t size) {
    return janet_abstract_end(janet_abstract_begin(atype, size));
}

#ifdef JANET_EV

/*
 * Threaded abstracts
 */

void *janet_abstract_begin_threaded(const JanetAbstractType *atype, size_t size) {
    JanetAbstractHead *header = janet_malloc(sizeof(JanetAbstractHead) + size);
    if (NULL == header) {
        JANET_OUT_OF_MEMORY;
    }
    janet_vm.next_collection += size + sizeof(JanetAbstractHead);
    header->gc.flags = JANET_MEMORY_THREADED_ABSTRACT;
    header->gc.data.next = NULL; /* Clear memory for address sanitizers */
    header->gc.data.refcount = 1;
    header->size = size;
    header->type = atype;
    header->home_vm = (void *) &janet_vm;
    header->teardown_started = 0;
    header->exported = 0;
    /* No complete home major has disproved a Janet-visible hold yet.
     * Partial collections must not authorize destruction of a newly
     * created abstract whose nested roots have not been traced. */
    header->home_gc_authorized = 1;
    void *abstract = (void *) & (header->data);
    janet_table_put(&janet_vm.threaded_abstracts, janet_wrap_abstract(abstract), janet_wrap_false());
    return abstract;
}

void *janet_abstract_end_threaded(void *x) {
    janet_gc_settype((void *)(janet_abstract_head(x)), JANET_MEMORY_THREADED_ABSTRACT);
    return x;
}

void *janet_abstract_threaded(const JanetAbstractType *atype, size_t size) {
    return janet_abstract_end_threaded(janet_abstract_begin_threaded(atype, size));
}

/* Refcounting primitives and sync primitives */

#ifdef JANET_WINDOWS

size_t janet_os_mutex_size(void) {
    return sizeof(CRITICAL_SECTION);
}

size_t janet_os_rwlock_size(void) {
    return sizeof(void *);
}

void janet_os_mutex_init(JanetOSMutex *mutex) {
    InitializeCriticalSection((CRITICAL_SECTION *) mutex);
}

void janet_os_mutex_deinit(JanetOSMutex *mutex) {
    DeleteCriticalSection((CRITICAL_SECTION *) mutex);
}

void janet_os_mutex_lock(JanetOSMutex *mutex) {
    EnterCriticalSection((CRITICAL_SECTION *) mutex);
}

void janet_os_mutex_unlock(JanetOSMutex *mutex) {
    /* error handling? May want to keep counter */
    LeaveCriticalSection((CRITICAL_SECTION *) mutex);
}

void janet_os_rwlock_init(JanetOSRWLock *rwlock) {
    InitializeSRWLock((PSRWLOCK) rwlock);
}

void janet_os_rwlock_deinit(JanetOSRWLock *rwlock) {
    /* no op? */
    (void) rwlock;
}

void janet_os_rwlock_rlock(JanetOSRWLock *rwlock) {
    AcquireSRWLockShared((PSRWLOCK) rwlock);
}

void janet_os_rwlock_wlock(JanetOSRWLock *rwlock) {
    AcquireSRWLockExclusive((PSRWLOCK) rwlock);
}

void janet_os_rwlock_runlock(JanetOSRWLock *rwlock) {
    ReleaseSRWLockShared((PSRWLOCK) rwlock);
}

void janet_os_rwlock_wunlock(JanetOSRWLock *rwlock) {
    ReleaseSRWLockExclusive((PSRWLOCK) rwlock);
}

#else

size_t janet_os_mutex_size(void) {
    return sizeof(pthread_mutex_t);
}

size_t janet_os_rwlock_size(void) {
    return sizeof(pthread_rwlock_t);
}

void janet_os_mutex_init(JanetOSMutex *mutex) {
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init((pthread_mutex_t *) mutex, &attr);
}

void janet_os_mutex_deinit(JanetOSMutex *mutex) {
    pthread_mutex_destroy((pthread_mutex_t *) mutex);
}

void janet_os_mutex_lock(JanetOSMutex *mutex) {
    pthread_mutex_lock((pthread_mutex_t *) mutex);
}

void janet_os_mutex_unlock(JanetOSMutex *mutex) {
    int ret = pthread_mutex_unlock((pthread_mutex_t *) mutex);
    if (ret) janet_panic("cannot release lock");
}

void janet_os_rwlock_init(JanetOSRWLock *rwlock) {
    pthread_rwlock_init((pthread_rwlock_t *) rwlock, NULL);
}

void janet_os_rwlock_deinit(JanetOSRWLock *rwlock) {
    pthread_rwlock_destroy((pthread_rwlock_t *) rwlock);
}

void janet_os_rwlock_rlock(JanetOSRWLock *rwlock) {
    pthread_rwlock_rdlock((pthread_rwlock_t *) rwlock);
}

void janet_os_rwlock_wlock(JanetOSRWLock *rwlock) {
    pthread_rwlock_wrlock((pthread_rwlock_t *) rwlock);
}

void janet_os_rwlock_runlock(JanetOSRWLock *rwlock) {
    pthread_rwlock_unlock((pthread_rwlock_t *) rwlock);
}

void janet_os_rwlock_wunlock(JanetOSRWLock *rwlock) {
    pthread_rwlock_unlock((pthread_rwlock_t *) rwlock);
}

#endif

int32_t janet_abstract_incref(void *abst) {
    return janet_atomic_inc(&janet_abstract_head(abst)->gc.data.refcount);
}

int32_t janet_abstract_decref(void *abst) {
    return janet_atomic_dec(&janet_abstract_head(abst)->gc.data.refcount);
}

#ifdef JANET_EV
/* BUG 2: release a threaded abstract on its HOME VM's thread. The
 * last registry claim can be dropped by a foreign VM's sweep; the
 * home VM may still have in-flight operations touching the block
 * (serials bookkeeping, monitor lookups), so the free must not run
 * on the foreign thread. */
static void janet_abstract_release_cb(JanetEVGenericMessage msg) {
    janet_gc_release((JanetGCObject *) msg.argp);
}

/* Full teardown (finalizer + poison + registry tombstone + release),
 * executed on the abstract's HOME VM. In-flight operations on the
 * home VM run between loop iterations, never mid-fiber, so a fiber
 * hashing or comparing the abstract never sees the poison. */
void janet_abstract_home_teardown_cb(JanetEVGenericMessage msg) {
    void *abst = msg.argp;
    JanetAbstractHead *head = janet_abstract_head(abst);
    /* Single-shot: the sweep's deinit_block may have torn the
     * abstract down first (guard set there); skip cleanly. */
    if (1 != janet_atomic_inc(&head->teardown_started)) return;
    /* GC-visible hold deferral (BUG 2 density): a value can sit in a
     * same-VM channel queue / parked payload / table with no
     * registry claim backing it. Authorization starts conservative and
     * only a completed unvisited home major revokes it. Re-arm a single
     * registry watch claim: its value is false, so without another home
     * major mark the next sweep revokes authorization and drops the claim
     * rather than keeping the object alive indefinitely. */
    if (head->home_gc_authorized) {
        janet_atomic_inc(&head->gc.data.refcount);
        janet_table_put(&janet_vm.threaded_abstracts,
                janet_wrap_abstract(abst), janet_wrap_false());
        head->teardown_started = 0;
        return;
    }
    /* Lane-homed pinning (BUG 2 density): a lane's own abstracts are
     * referenced claim-less by circulating pid values (channel queue
     * items, parked payloads, serials). Freeing at refcount-0 dangles
     * them; lanes pin instead and janet_clear_memory drains the
     * quarantine at lane stop. Main-VM teardowns keep prompt
     * semantics (suite-ev2's weakref/count tests). */
    if (janet_vm.is_lane_vm || head->exported) {
        /* Lane-homed: circulating pid values. Exported (ever
         * marshaled cross-VM): a foreign lane may hold claim-less
         * values even when homed on main. Both pin until exit. */
        head->gc.data.next = janet_vm.home_quarantine;
        janet_vm.home_quarantine = head;
        return;
    }
    if (NULL != head->type && NULL != head->type->gc) {
        janet_assert(!head->type->gc(head->data, head->size),
                "finalizer failed");
    }
    head->type = NULL;
    /* No registry tombstone here: janet_table_put can allocate and
     * trigger a collection from inside the event loop's callback
     * (racing the channel GC's pending-queue walk). The observing
     * VM's purge tombstones its own entry after its decref, and the
     * home VM's purge purges the poisoned-type entry in place. */
    janet_gc_release((JanetGCObject *) head);
}
#endif

int32_t janet_abstract_decref_maybe_free(void *abst) {
    int32_t result = janet_abstract_decref(abst);
    if (0 == result) {
#ifdef JANET_ASAN_UAF_TRACE
        {
        extern int backtrace(void **, int);
        extern void backtrace_symbols_fd(void *const *, int, int);
        JanetAbstractHead *ph = janet_abstract_head(abst);
            if (ph->gc.flags & JANET_MEMORY_THREADED_ABSTRACT) {
                fprintf(stderr, "[xthread-free] abstract=%p size=%d\n",
                        abst, (int) ph->size);
                void *bt[16];
                int bn = backtrace(bt, 16);
                backtrace_symbols_fd(bt, bn, 2);
            }
        }
#endif
        JanetAbstractHead *head = janet_abstract_head(abst);
        /* ROUTING FIRST: every threaded teardown flows through the
         * home callback -- finalizer + poison + release + the
         * GC-visible-hold deferral live there under the single-shot
         * guard. (Running the finalizer inline here freed channel
         * queue buffers on the observing thread and the home VM's
         * routed callback re-ran the deinit on freed memory -- ASAN:
         * flag_chanpend_node/janet_chan_deinit_q UAF at density.)
         * Non-threaded abstracts are single-VM: teardown inline. */
        if (head->gc.flags & JANET_MEMORY_THREADED_ABSTRACT) {
            /* A foreign VM may be shutting down while the abstract's
             * home loop is still alive. Its registry claim must be routed
             * home, not finalized on this worker with surviving home
             * Janet values pointing into the malloc-backed block. */
            if (janet_vm.gc_teardown_inline &&
                    (NULL == head->home_vm || head->home_vm == (void *) &janet_vm)) {
                /* Home shutdown: the loop is gone. Inline teardown under
                 * the single-shot guard (skip the reachability
                 * deferral -- GC state is being destroyed anyway). */
                if (1 != janet_atomic_inc(&head->teardown_started)) {
                    return result;
                }
                if (NULL != head->type->gc) {
                    janet_assert(!head->type->gc(head->data, head->size),
                            "finalizer failed");
                }
                head->type = NULL;
                janet_gc_release((JanetGCObject *) head);
                return result;
            }
            JanetVM *target = (NULL != head->home_vm)
                ? (JanetVM *) head->home_vm : &janet_vm;
            JanetEVGenericMessage msg;
            memset(&msg, 0, sizeof(msg));
            msg.argp = abst;
            janet_ev_post_event(target, janet_abstract_home_teardown_cb, msg);
            return result;
        }
        if (head->type->gc) {
            janet_assert(!head->type->gc(head->data, head->size), "finalizer failed");
        }
        /* Poison the header before release: any registry entry or stale
         * heap reference that outlives the abstract (cross-thread frees,
         * scoped sweeps, teardown interleavings) must read type == NULL
         * instead of recycled memory, so the gc.c walks and markers can
         * identify the dead object instead of dereferencing freed data. */
        head->type = NULL;
        /* Free memory via the slab-aware release: threaded abstracts can
         * be freed outside sweep (and retagged post-alloc), so plain
         * janet_free would corrupt slab pages. */
        janet_gc_release((JanetGCObject *) head);
    }
    return result;
}

#endif
