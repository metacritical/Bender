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
#include "fiber.h"
#include "proc.h"
#endif

#ifdef BENDER_EV

#include <math.h>
#include <fcntl.h>
#ifdef BENDER_WINDOWS
#include <winsock2.h>
#include <windows.h>
#include <io.h>
#else
#include <pthread.h>
#include <limits.h>
#include <errno.h>
#include <unistd.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <netdb.h>
#include <sys/socket.h>
#include <sys/wait.h>
#ifdef BENDER_EV_EPOLL
#include <sys/epoll.h>
#include <sys/timerfd.h>
#endif
#ifdef BENDER_EV_KQUEUE
#include <sys/event.h>
#endif
#ifdef BENDER_EV_POLL
#include <poll.h>
#endif
#endif

typedef struct BenderChanPendReg BenderChanPendReg;

typedef struct {
    BenderVM *thread;
    BenderFiber *fiber;
    uint32_t sched_id;
    enum {
        BENDER_CP_MODE_READ,
        BENDER_CP_MODE_WRITE,
        BENDER_CP_MODE_CHOICE_READ,
        BENDER_CP_MODE_CHOICE_WRITE,
        BENDER_CP_MODE_CLOSE
    } mode;
    /* Back-pointer into the owning fiber's chan_pends registration
     * list. Unlink-on-free discipline: the node is claimed (and
     * freed) by whoever consumes the entry last — a channel pop, a
     * close revival, channel deinit, or fiber free. Fibers are only
     * ever dereferenced through an entry whose node proves them alive
     * (dead bit clear), replacing the unsound GC probe-on-read. */
    BenderChanPendReg *reg;
} BenderChannelPending;

/* Channel-pending registration (see BenderFiber.chan_pends). Malloc'd,
 * never a GC object. Bit semantics:
 *   in_queue  — the entry is linked into the channel queue right now
 *   dequeued  — a threaded-channel pop claimed the entry under lock;
 *               the owner thread prunes the node at next park/deinit
 *   dead      — the owning fiber was freed; the entry must never
 *               schedule (or dereference) its fiber again
 *   chan_dead — the owning channel was freed; queue storage is gone
 * Plain/isolated channels are same-thread by contract (their home
 * lane), so their node mutations need no lock. Threaded channels
 * mutate `dequeued` and `dead` under the channel lock. */
typedef struct BenderChanPendReg {
    struct BenderChanPendReg *next;
    BenderChannel *chan;
    uint8_t is_write;
    uint8_t in_queue;
    uint8_t dequeued;
    uint8_t dead;
    uint8_t chan_dead;
} BenderChanPendReg;

/* Drop finished registrations from the fiber's list (called on the
 * fiber's own thread at park time). Nodes are freed here exactly when
 * no channel-side reference can reach them anymore. */
static void bender_chanpends_prune(BenderFiber *f) {
    BenderChanPendReg **link = (BenderChanPendReg **) &f->chan_pends;
    while (NULL != *link) {
        BenderChanPendReg *reg = *link;
        /* chan_dead nodes are kept even once unqueued: a freed
         * channel's abandoned queue storage may still hold entries
         * that reference the node, and a push into that
         * freed-but-unreused channel (upstream's zombie discipline)
         * must find a valid node, never freed memory. They are
         * reclaimed by the zombie claim or, absent one, leak bounded
         * records on this broken-ownership path. */
        if (reg->dead || reg->dequeued || (!reg->in_queue && !reg->chan_dead)) {
            *link = reg->next;
            bender_free(reg);
        } else {
            link = &reg->next;
        }
    }
}

/* Register a new pending entry: node onto the fiber list, then the
 * caller fills pending.reg and pushes to the channel queue. */
static BenderChanPendReg *bender_chanpend_register(BenderFiber *f, BenderChannel *chan, int is_write) {
    bender_chanpends_prune(f);
    BenderChanPendReg *reg = bender_malloc(sizeof(BenderChanPendReg));
    reg->next = (BenderChanPendReg *) f->chan_pends;
    reg->chan = chan;
    reg->is_write = (uint8_t) is_write;
    reg->in_queue = 1;
    reg->dequeued = 0;
    reg->dead = 0;
    reg->chan_dead = 0;
    f->chan_pends = reg;
    return reg;
}

/* Remove a node from the fiber's list and free it. The fiber must be
 * alive and on this thread (same-VM claim path only). */
static void bender_chanpends_unlink(BenderFiber *f, BenderChanPendReg *reg) {
    BenderChanPendReg **link = (BenderChanPendReg **) &f->chan_pends;
    while (NULL != *link) {
        if (*link == reg) {
            *link = reg->next;
            break;
        }
        link = &(*link)->next;
    }
    bender_free(reg);
}

/* Consume a same-VM pending entry popped from a plain/isolated
 * channel queue. The dead bit makes the fiber-liveness check sound
 * (no GC probing): a dead fiber is never dereferenced, its entry is
 * just reclaimed. Returns 1 when pending.fiber may be scheduled
 * (alive and sched-consistent), 0 when the entry was consumed as
 * stale. */
static int bender_chanpend_claim_plain(BenderChannelPending *p) {
    BenderChanPendReg *reg = p->reg;
    reg->in_queue = 0;
    if (reg->dead) {
        bender_free(reg);
        return 0;
    }
    int live = p->sched_id == p->fiber->sched_id;
    bender_chanpends_unlink(p->fiber, reg);
    return live;
}

/* Consume a threaded-channel entry popped under the channel lock:
 * only the node is touched (the fiber may live on another thread).
 * The owner thread reclaims the node at its next park or at free. */
static void bender_chanpend_claim_threaded(BenderChannelPending *p) {
    BenderChanPendReg *reg = p->reg;
    reg->in_queue = 0;
    reg->dequeued = 1;
}

/* Fiber free hook (called from bender_deinit_block's fiber case).
 * Entries still queued stay in the queues with `dead` set — channels
 * reclaim them lazily and never dereference the fiber again. No
 * channel locks are taken here: this runs inside the GC sweep, and a
 * thread mid-channel-op can be the one collecting (allocation under
 * the chan lock would self-deadlock). Plain/isolated mutations are
 * same-thread by the home-lane contract; a queued *threaded* entry
 * roots its fiber, so a queued threaded node is unreachable from
 * fiber free. Nodes whose channel already died are kept (dead
 * flagged) because abandoned queue storage may still reference them;
 * only dequeued nodes free here. */
void bender_ev_chanpends_deinit(BenderFiber *f) {
    BenderChanPendReg *reg = (BenderChanPendReg *) f->chan_pends;
    while (NULL != reg) {
        BenderChanPendReg *next = reg->next;
        if (!reg->chan_dead && !reg->dequeued) {
            /* Queued on a living channel: flag dead, channel reclaims.
             * (Dequeuing is impossible for a freed fiber.) */
            reg->dead = 1;
        } else if (reg->chan_dead) {
            /* Channel freed before this fiber: abandoned queue
             * storage may still reference the node — keep it (dead
             * flagged) so zombie claims stay sound; unclaimed nodes
             * leak bounded records. */
            reg->dead = 1;
        } else {
            bender_free(reg);
        }
        reg = next;
    }
    f->chan_pends = NULL;
}

/* GC reverse edge (called from bender_mark_fiber): a fiber with
 * pending registrations pins its channels, mirroring the channel's
 * own pin on parked fibers (mark_fq). Without this, a scoped or
 * global sweep could free a channel out from under a parked fiber's
 * pending entry (observed: proc-scoped collection freeing an
 * attributed mailbox). Only entries still linked into a channel queue
 * own that reverse edge: claimed/dequeued nodes remain on the fiber's
 * list until its next park, but their channel can already be freed.
 * Dead-flagged nodes reference freed fibers (skip), and chan_dead
 * nodes reference freed channels (skip). */
void bender_ev_chanpends_mark(BenderFiber *f) {
    BenderChanPendReg *reg = (BenderChanPendReg *) f->chan_pends;
    while (NULL != reg) {
        if (reg->in_queue && !reg->chan_dead && !reg->dead) {
            bender_mark(bender_wrap_abstract(reg->chan));
        }
        reg = reg->next;
    }
}

struct BenderChannel {
    BenderQueue items;
    BenderQueue read_pending;
    BenderQueue write_pending;
    int32_t limit;
    int closed;
    int is_threaded;
    /* P3.5c: proc mailboxes go quiet on proc death (sends drop as a
     * silent no-op, Erlang parity). Set at creation; cleared when the
     * owning proc's fiber dies (kill/completion/sweep-free). Plain
     * and threaded channels ignore it (always 1). Read lock-free on
     * the send path (isolated mailboxes are home-thread only, and
     * the bit moves true->false exactly once). */
    int proc_alive;
    /* Step 1.4: isolated channels marshal-copy values like threaded
     * ones (same transit ownership) but take no lock: single-VM by
     * contract (tasklet mailboxes). Cross-thread use is user error. */
    int isolated;
    /* True when this channel arrived by UNMARSHAL, i.e. it is a private
     * copy of a channel that really lives in another lane. A plain (unlocked,
     * uncopied) channel in that state is a trap: writes land in the copy and
     * the original owner never observes them, with no error. Set from the
     * unmarshal path only; cleared on fresh construction. */
    int detached_copy;
    /* Step 4.1 home-lane pin: the creating/unmarshalling thread's VM.
     * Isolated channels are lock-free, so foreign-thread use would be
     * a silent race; push/pop refuse it loudly instead. Plain and
     * threaded channels leave this NULL (legacy behavior unchanged). */
    BenderVM *owner;
#ifdef BENDER_WINDOWS
    CRITICAL_SECTION lock;
#else
    pthread_mutex_t lock;
#endif
};

typedef struct {
    BenderFiber *fiber;
    Bender value;
    BenderSignal sig;
    uint32_t expected_sched_id; /* If the fiber has been rescheduled this loop, don't run first scheduling. */
} BenderTask;

/* Wrap return value by pairing it with the callback used to handle it
 * in the main thread */
typedef struct {
    BenderEVGenericMessage msg;
    BenderThreadedCallback cb;
} BenderSelfPipeEvent;

/* Structure used to initialize threads in the thread pool
 * (same head structure as self pipe event)*/
typedef struct {
    BenderEVGenericMessage msg;
    BenderThreadedCallback cb;
    BenderThreadedSubroutine subr;
    BenderHandle write_pipe;
} BenderEVThreadInit;

/* Structure used to initialize threads that run timeouts */
typedef struct {
    double sec;
    BenderVM *vm;
    BenderFiber *fiber;
#ifdef BENDER_WINDOWS
    HANDLE cancel_event;
#endif
} BenderThreadedTimeout;

#define BENDER_MAX_Q_CAPACITY 0x7FFFFFF

static void bender_q_init(BenderQueue *q) {
    q->data = NULL;
    q->head = 0;
    q->tail = 0;
    q->capacity = 0;
}

static void bender_q_deinit(BenderQueue *q) {
    bender_free(q->data);
}

static int32_t bender_q_count(BenderQueue *q) {
    return (q->head > q->tail)
           ? (q->tail + q->capacity - q->head)
           : (q->tail - q->head);
}

static int bender_q_maybe_resize(BenderQueue *q, size_t itemsize) {
    int32_t count = bender_q_count(q);
    /* Resize if needed */
    if (count + 1 >= q->capacity) {
        if (count + 1 >= BENDER_MAX_Q_CAPACITY) return 1;
        int32_t newcap = (count + 2) * 2;
        if (newcap > BENDER_MAX_Q_CAPACITY) newcap = BENDER_MAX_Q_CAPACITY;
        q->data = bender_realloc(q->data, itemsize * newcap);
        if (NULL == q->data) {
            BENDER_OUT_OF_MEMORY;
        }
        if (q->head > q->tail) {
            /* Two segments, fix 2nd seg. */
            int32_t newhead = q->head + (newcap - q->capacity);
            size_t seg1 = (size_t)(q->capacity - q->head);
            if (seg1 > 0) {
                memmove((char *) q->data + (newhead * itemsize),
                        (char *) q->data + (q->head * itemsize),
                        seg1 * itemsize);
            }
            q->head = newhead;
        }
        q->capacity = newcap;
    }
    return 0;
}

static int bender_q_push(BenderQueue *q, void *item, size_t itemsize) {
    if (bender_q_maybe_resize(q, itemsize)) return 1;
    memcpy((char *) q->data + itemsize * q->tail, item, itemsize);
    q->tail = q->tail + 1 < q->capacity ? q->tail + 1 : 0;
    return 0;
}

static int bender_q_push_head(BenderQueue *q, void *item, size_t itemsize) {
    if (bender_q_maybe_resize(q, itemsize)) return 1;
    int32_t newhead = q->head - 1;
    if (newhead < 0) {
        newhead += q->capacity;
    }
    memcpy((char *) q->data + itemsize * newhead, item, itemsize);
    q->head = newhead;
    return 0;
}

static int bender_q_pop(BenderQueue *q, void *out, size_t itemsize) {
    if (q->head == q->tail) return 1;
    memcpy(out, (char *) q->data + itemsize * q->head, itemsize);
    q->head = q->head + 1 < q->capacity ? q->head + 1 : 0;
    return 0;
}

/* Get current timestamp (millisecond precision) */
static BenderTimestamp ts_now(void);

/* Get current timestamp + an interval (millisecond precision) */
static BenderTimestamp ts_delta(BenderTimestamp ts, double delta) {
    if (isinf(delta)) {
        return delta < 0 ? ts : INT64_MAX;
    }
    ts += (int64_t)round(delta * 1000);
    return ts;
}

/* Look at the next timeout value without removing it. */
static int peek_timeout(BenderTimeout *out) {
    if (bender_vm.tq_count == 0) return 0;
    *out = bender_vm.tq[0];
    return 1;
}

/* Remove the next timeout from the priority queue */
static void pop_timeout(size_t index) {
    if (bender_vm.tq_count <= index) return;
    bender_vm.tq[index] = bender_vm.tq[--bender_vm.tq_count];
    for (;;) {
        size_t left = (index << 1) + 1;
        size_t right = left + 1;
        size_t smallest = index;
        if (left < bender_vm.tq_count &&
                (bender_vm.tq[left].when < bender_vm.tq[smallest].when))
            smallest = left;
        if (right < bender_vm.tq_count &&
                (bender_vm.tq[right].when < bender_vm.tq[smallest].when))
            smallest = right;
        if (smallest == index) return;
        BenderTimeout temp = bender_vm.tq[index];
        bender_vm.tq[index] = bender_vm.tq[smallest];
        bender_vm.tq[smallest] = temp;
        index = smallest;
    }
}

/* Add a timeout to the timeout min heap */
static void add_timeout(BenderTimeout to) {
    size_t oldcount = bender_vm.tq_count;
    size_t newcount = oldcount + 1;
    if (newcount > bender_vm.tq_capacity) {
        size_t newcap = 2 * newcount;
        BenderTimeout *tq = bender_realloc(bender_vm.tq, newcap * sizeof(BenderTimeout));
        if (NULL == tq) {
            BENDER_OUT_OF_MEMORY;
        }
        bender_vm.tq = tq;
        bender_vm.tq_capacity = newcap;
    }
    /* Append */
    bender_vm.tq_count = (int32_t) newcount;
    bender_vm.tq[oldcount] = to;
    /* Heapify */
    size_t index = oldcount;
    while (index > 0) {
        size_t parent = (index - 1) >> 1;
        if (bender_vm.tq[parent].when <= bender_vm.tq[index].when) break;
        /* Swap */
        BenderTimeout tmp = bender_vm.tq[index];
        bender_vm.tq[index] = bender_vm.tq[parent];
        bender_vm.tq[parent] = tmp;
        /* Next */
        index = parent;
    }
}

void bender_async_end(BenderFiber *fiber) {
    if (fiber->ev_callback) {
        if (fiber->ev_stream->read_fiber == fiber) {
            fiber->ev_stream->read_fiber = NULL;
        }
        if (fiber->ev_stream->write_fiber == fiber) {
            fiber->ev_stream->write_fiber = NULL;
        }
        fiber->ev_callback(fiber, BENDER_ASYNC_EVENT_DEINIT);
        bender_gcunroot(bender_wrap_abstract(fiber->ev_stream));
        fiber->ev_callback = NULL;
        if (!(fiber->flags & BENDER_FIBER_EV_FLAG_IN_FLIGHT)) {
            if (fiber->ev_state) {
                bender_free(fiber->ev_state);
                fiber->ev_state = NULL;
            }
            bender_ev_dec_refcount();
        }
    }
}

void bender_async_in_flight(BenderFiber *fiber) {
#ifdef BENDER_WINDOWS
    fiber->flags |= BENDER_FIBER_EV_FLAG_IN_FLIGHT;
#else
    (void) fiber;
#endif
}

void bender_async_start_fiber(BenderFiber *fiber, BenderStream *stream, BenderAsyncMode mode, BenderEVCallback callback, void *state) {
    bender_assert(!fiber->ev_callback, "double async on fiber");
    if (mode & BENDER_ASYNC_LISTEN_READ) {
        stream->read_fiber = fiber;
    }
    if (mode & BENDER_ASYNC_LISTEN_WRITE) {
        stream->write_fiber = fiber;
    }
    fiber->ev_callback = callback;
    fiber->ev_stream = stream;
    bender_ev_inc_refcount();
    bender_gcroot(bender_wrap_abstract(stream));
    fiber->ev_state = state;
    callback(fiber, BENDER_ASYNC_EVENT_INIT);
}

void bender_async_start(BenderStream *stream, BenderAsyncMode mode, BenderEVCallback callback, void *state) {
    bender_async_start_fiber(bender_vm.root_fiber, stream, mode, callback, state);
    bender_await();
}

static void handle_timeout_worker(BenderTimeout to, int cancel);

/* Remove every operation-scoped deadline owned by `fiber`.
 *
 * A timed wait arms a deadline, and the wait usually completes long before it.
 * sched_id alone cannot retire that entry: it only blocks *delivery* while the
 * ids differ, so a later schedule that lands on the captured id fires a
 * deadline for an operation that already finished. That is how a completed
 * `net/accept` used to abort unrelated arithmetic thousands of instructions
 * later, and why the same expiry could be delivered more than once.
 *
 * Resuming is the authoritative "that operation is over" event, so retiring
 * here keeps the queue bounded by genuinely pending waits. `curr_fiber` entries
 * are ev/deadline's explicit user-armed cancellations, not operation waits, and
 * are left alone. The heap holds one entry per concurrently waiting fiber, so
 * this scan is tiny and paid only on resume. */
static void bender_drop_deadlines_for(BenderFiber *fiber) {
    size_t i = 0;
    while (i < bender_vm.tq_count) {
        BenderTimeout t = bender_vm.tq[i];
        if (t.curr_fiber == NULL && t.fiber == fiber) {
            pop_timeout(i);
            handle_timeout_worker(t, 1);
            continue;
        }
        i++;
    }
}

void bender_fiber_did_resume(BenderFiber *fiber) {
    bender_async_end(fiber);
    bender_drop_deadlines_for(fiber);
}

static void bender_stream_checktoclose(BenderStream *stream) {
    if ((stream->flags & BENDER_STREAM_TOCLOSE) && !stream->read_fiber && !stream->write_fiber) {
        bender_stream_close(stream);
    }
}

/* Forward declaration */
static void bender_register_stream(BenderStream *stream);
#ifndef BENDER_WINDOWS
static void bender_unregister_stream(BenderStream *stream);
#endif

static const BenderMethod ev_default_stream_methods[] = {
    {"close", bender_cfun_stream_close},
    {"read", bender_cfun_stream_read},
    {"chunk", bender_cfun_stream_chunk},
    {"write", bender_cfun_stream_write},
    {NULL, NULL}
};

/* Create a stream*/
BenderStream *bender_stream_ext(BenderHandle handle, uint32_t flags, const BenderMethod *methods, size_t size) {
    bender_assert(size >= sizeof(BenderStream), "bad size");
    BenderStream *stream = bender_abstract(&bender_stream_type, size);
    stream->handle = handle;
    stream->flags = flags;
    stream->read_fiber = NULL;
    stream->write_fiber = NULL;
    if (methods == NULL) methods = ev_default_stream_methods;
    stream->methods = methods;
    stream->index = 0;
    bender_register_stream(stream);
    return stream;
}

BenderStream *bender_stream(BenderHandle handle, uint32_t flags, const BenderMethod *methods) {
    return bender_stream_ext(handle, flags, methods, sizeof(BenderStream));
}

static void bender_stream_close_impl(BenderStream *stream) {
    stream->flags |= BENDER_STREAM_CLOSED;
    int canclose = !(stream->flags & BENDER_STREAM_NOT_CLOSEABLE);
    /* If we are positive that the stream is the last instance of the underlying file description (such that "dup" was never called on the file description)
     * then skip unregister to save a syscall */
#ifdef BENDER_WINDOWS
    if (stream->handle != INVALID_HANDLE_VALUE) {
#ifdef BENDER_NET
        if (stream->flags & BENDER_STREAM_SOCKET) {
            if (canclose) closesocket((SOCKET) stream->handle);
        } else
#endif
        {
            if (canclose) CloseHandle(stream->handle);
        }
        stream->handle = INVALID_HANDLE_VALUE;
    }
#else
    int canunregister = !(stream->flags & BENDER_STREAM_UNREGISTERED);
    if (stream->handle != -1) {
        if (canunregister) bender_unregister_stream(stream);
        if (canclose) close(stream->handle);
        stream->handle = -1;
    }
#endif
}

void bender_stream_close(BenderStream *stream) {
    BenderFiber *rf = stream->read_fiber;
    BenderFiber *wf = stream->write_fiber;
    if (rf && rf->ev_callback) {
        rf->ev_callback(rf, BENDER_ASYNC_EVENT_CLOSE);
        stream->read_fiber = NULL;
    }
    if (wf && wf->ev_callback) {
        wf->ev_callback(wf, BENDER_ASYNC_EVENT_CLOSE);
        stream->write_fiber = NULL;
    }
    bender_stream_close_impl(stream);
}

/* Called to clean up a stream */
static int bender_stream_gc(void *p, size_t s) {
    (void) s;
    BenderStream *stream = (BenderStream *)p;
    bender_stream_close_impl(stream);
    return 0;
}

/* Mark a stream for GC */
static int bender_stream_mark(void *p, size_t s) {
    (void) s;
    BenderStream *stream = (BenderStream *) p;
    BenderFiber *rf = stream->read_fiber;
    BenderFiber *wf = stream->write_fiber;
    if (rf) {
        bender_mark(bender_wrap_fiber(rf));
    }
    if (wf) {
        bender_mark(bender_wrap_fiber(wf));
    }
    return 0;
}

static int bender_stream_getter(void *p, Bender key, Bender *out) {
    BenderStream *stream = (BenderStream *)p;
    if (!bender_checktype(key, BENDER_KEYWORD)) return 0;
    const BenderMethod *stream_methods = stream->methods;
    return bender_getmethod(bender_unwrap_keyword(key), stream_methods, out);
}

static void bender_stream_marshal(void *p, BenderMarshalContext *ctx) {
    BenderStream *s = p;
    if (!(bender_marshal_flags(ctx) & BENDER_MARSHAL_UNSAFE)) {
        bender_panic("can only marshal stream with unsafe flag");
    }
    s->flags &= ~BENDER_STREAM_NODUPS; /* This stream now might be duplicated, invalidates some EV optimizations */
    bender_marshal_abstract(ctx, p);
    bender_marshal_int(ctx, (int32_t) s->flags);
    bender_marshal_ptr(ctx, s->methods);
#ifdef BENDER_WINDOWS
    /* TODO - ref counting to avoid situation where a handle is closed or GCed
     * while in transit, and it's value gets reused. DuplicateHandle does not work
     * for network sockets, and in general for winsock it is better to not duplicate
     * unless there is a need to. */
    HANDLE duph = INVALID_HANDLE_VALUE;
    if (s->flags & BENDER_STREAM_SOCKET) {
        duph = s->handle;
    } else {
        DuplicateHandle(
            GetCurrentProcess(),
            s->handle,
            GetCurrentProcess(),
            &duph,
            0,
            FALSE,
            DUPLICATE_SAME_ACCESS);
    }
    bender_marshal_int64(ctx, (int64_t)(duph));
#else
    /* Marshal after dup because it is easier than maintaining our own ref counting. */
    int duph = dup(s->handle);
    if (duph < 0) bender_panicf("failed to duplicate stream handle: %V", bender_ev_lasterr());
    bender_marshal_int(ctx, (int32_t)(duph));
#endif
}

static void *bender_stream_unmarshal(BenderMarshalContext *ctx) {
    if (!(bender_unmarshal_flags(ctx) & BENDER_MARSHAL_UNSAFE)) {
        bender_panic("can only unmarshal stream with unsafe flag");
    }
    BenderStream *p = bender_unmarshal_abstract(ctx, sizeof(BenderStream));
    /* Can't share listening state and such across threads */
    p->read_fiber = NULL;
    p->write_fiber = NULL;
    p->flags = (uint32_t) bender_unmarshal_int(ctx);
    p->methods =  bender_unmarshal_ptr(ctx);
#ifdef BENDER_WINDOWS
    p->handle = (BenderHandle) bender_unmarshal_int64(ctx);
#else
    p->handle = (BenderHandle) bender_unmarshal_int(ctx);
#endif
#ifdef BENDER_EV_POLL
    bender_register_stream(p);
#endif
    return p;
}

static Bender bender_stream_next(void *p, Bender key) {
    BenderStream *stream = (BenderStream *)p;
    return bender_nextmethod(stream->methods, key);
}

static void bender_stream_tostring(void *p, BenderBuffer *buffer) {
    BenderStream *stream = p;
    /* Let user print the file descriptor for debugging */
    bender_formatb(buffer, "[fd=%d]", stream->handle);
}

const BenderAbstractType bender_stream_type = {
    "core/stream",
    bender_stream_gc,
    bender_stream_mark,
    bender_stream_getter,
    NULL,
    bender_stream_marshal,
    bender_stream_unmarshal,
    bender_stream_tostring,
    NULL,
    NULL,
    bender_stream_next,
    BENDER_ATEND_NEXT
};

/* Register a fiber to resume with value */
static void bender_schedule_general(BenderFiber *fiber, Bender value, BenderSignal sig, int soon) {
    if (fiber->gc.flags & BENDER_FIBER_EV_GCFLAG_CANCELED) return;
    if (!(fiber->gc.flags & BENDER_FIBER_EV_GCFLAG_ROOT)) {
        fiber->gc.flags |= BENDER_FIBER_EV_GCFLAG_ROOT;
        Bender task_element = bender_wrap_fiber(fiber);
        bender_table_put(&bender_vm.active_tasks, task_element, bender_wrap_true());
    }
    BenderTask t = { fiber, value, sig, ++fiber->sched_id };
    if (sig == BENDER_SIGNAL_ERROR) fiber->gc.flags |= BENDER_FIBER_EV_GCFLAG_CANCELED;
    if (soon) {
        bender_assert(!bender_q_push_head(&bender_vm.spawn, &t, sizeof(t)), "schedule queue overflow");
    } else {
        bender_assert(!bender_q_push(&bender_vm.spawn, &t, sizeof(t)), "schedule queue overflow");
    }
}

void bender_schedule_signal(BenderFiber *fiber, Bender value, BenderSignal sig) {
    bender_schedule_general(fiber, value, sig, 0);
}

void bender_schedule_soon(BenderFiber *fiber, Bender value, BenderSignal sig) {
    bender_schedule_general(fiber, value, sig, 1);
}

void bender_cancel(BenderFiber *fiber, Bender value) {
    if (!(fiber->gc.flags & BENDER_FIBER_EV_GCFLAG_ROOT)) {
        bender_panic("cannot cancel non-task fiber");
    }
    bender_schedule_signal(fiber, value, BENDER_SIGNAL_ERROR);
}

void bender_schedule(BenderFiber *fiber, Bender value) {
    bender_schedule_signal(fiber, value, BENDER_SIGNAL_OK);
}

/* Mark all pending tasks */
void bender_ev_mark(void) {

    /* Pending tasks */
    BenderTask *tasks = bender_vm.spawn.data;
    if (bender_vm.spawn.head <= bender_vm.spawn.tail) {
        for (int32_t i = bender_vm.spawn.head; i < bender_vm.spawn.tail; i++) {
            bender_mark(bender_wrap_fiber(tasks[i].fiber));
            bender_mark(tasks[i].value);
        }
    } else {
        for (int32_t i = bender_vm.spawn.head; i < bender_vm.spawn.capacity; i++) {
            bender_mark(bender_wrap_fiber(tasks[i].fiber));
            bender_mark(tasks[i].value);
        }
        for (int32_t i = 0; i < bender_vm.spawn.tail; i++) {
            bender_mark(bender_wrap_fiber(tasks[i].fiber));
            bender_mark(tasks[i].value);
        }
    }

    /* Pending timeouts */
    for (size_t i = 0; i < bender_vm.tq_count; i++) {
        bender_mark(bender_wrap_fiber(bender_vm.tq[i].fiber));
        if (bender_vm.tq[i].curr_fiber != NULL) {
            bender_mark(bender_wrap_fiber(bender_vm.tq[i].curr_fiber));
        }
    }
}

/* Erlang-parity messaging fix (docs/internals/proc-lanes.md): a scoped
 * fiber/process minor's narrow defensive need is only to keep a QUEUED
 * fiber's OWN block from being swept if it happens to live on the
 * heap being collected -- NOT to prove that fiber's full reachability
 * (that is whichever collection actually owns and sweeps it). The
 * original bender_ev_mark deep-marks every queued fiber (bender_mark ->
 * bender_mark_fiber, full frame walk), which — confirmed via live lldb
 * backtraces — made every actor's own per-actor minor walk the ENTIRE
 * stack of whatever fiber the event loop has queued (e.g. a driver
 * script parked on ev/sleep mid spawn-burst, with a large live handle
 * array on its stack): O(N) per minor x N actors = O(N^2). This
 * variant shallow-marks every queued FIBER (reachable bit only, no
 * frame walk) while still fully marking every queued VALUE (the
 * wakeup payload the original comment's "lost-wakeup" concern is
 * actually about) exactly as before. Use ONLY from a scoped minor
 * (bender_collect_fiber_minor); bender_mark_roots (global minor/major)
 * must keep calling the full bender_ev_mark. */
void bender_ev_mark_shallow(void) {

    /* Pending tasks */
    BenderTask *tasks = bender_vm.spawn.data;
    if (bender_vm.spawn.head <= bender_vm.spawn.tail) {
        for (int32_t i = bender_vm.spawn.head; i < bender_vm.spawn.tail; i++) {
            bender_gc_mark_fiber_shallow(tasks[i].fiber);
            bender_mark(tasks[i].value);
        }
    } else {
        for (int32_t i = bender_vm.spawn.head; i < bender_vm.spawn.capacity; i++) {
            bender_gc_mark_fiber_shallow(tasks[i].fiber);
            bender_mark(tasks[i].value);
        }
        for (int32_t i = 0; i < bender_vm.spawn.tail; i++) {
            bender_gc_mark_fiber_shallow(tasks[i].fiber);
            bender_mark(tasks[i].value);
        }
    }

    /* Pending timeouts */
    for (size_t i = 0; i < bender_vm.tq_count; i++) {
        bender_gc_mark_fiber_shallow(bender_vm.tq[i].fiber);
        if (bender_vm.tq[i].curr_fiber != NULL) {
            bender_gc_mark_fiber_shallow(bender_vm.tq[i].curr_fiber);
        }
    }
}

/* Push/pop bodies live here; proc.c shares them via ev.h (Step P1). */
int bender_channel_push(BenderChannel *channel, Bender x, int mode);
/* proc.c: flush a routed-spawn batch pending for this ctl, or any when NULL.
 * Called from the push path (ORDERING) and from bender_await (LIVENESS). */
void bender_proc_spawn_batch_flush_for(void *chan);
int bender_channel_pop(BenderChannel *channel, Bender *item, int is_choice);
/* Phase C: harvest helper defined with the pool code below (used by
 * both thread/new completions and pool worker exits). */
static int bender_profile_harvest_bytes(uint8_t **out_bytes, size_t *out_len);
/* NOTE(other-stream): forward declaration to unblock the build; the
 * owning change should place this with its own declarations. */
static int bender_profile_harvest_bytes(uint8_t **out_bytes, size_t *out_len);

static Bender make_supervisor_event(const char *name, BenderFiber *fiber, int threaded) {
    Bender tup[3];
    tup[0] = bender_ckeywordv(name);
    tup[1] = threaded ? fiber->last_value : bender_wrap_fiber(fiber) ;
    if (fiber->env != NULL) {
        tup[2] = bender_table_get(fiber->env, bender_ckeywordv("task-id"));
    } else {
        tup[2] = bender_wrap_nil();
    }
    return bender_wrap_tuple(bender_tuple_n(tup, 3));
}

/* Common init code */
void bender_ev_init_common(void) {
    bender_q_init(&bender_vm.spawn);
    bender_vm.tq = NULL;
    bender_vm.tq_count = 0;
    bender_vm.tq_capacity = 0;
    bender_table_init_raw(&bender_vm.threaded_abstracts, 0);
    bender_table_init_raw(&bender_vm.active_tasks, 0);
    bender_table_init_raw(&bender_vm.signal_handlers, 0);
    bender_rng_seed(&bender_vm.ev_rng, 0);
#ifndef BENDER_WINDOWS
    pthread_attr_init(&bender_vm.new_thread_attr);
    pthread_attr_setdetachstate(&bender_vm.new_thread_attr, PTHREAD_CREATE_DETACHED);
#endif
}

#if BENDER_ANDROID
static void bender_timeout_stop(int sig_num) {
    if (sig_num == SIGUSR1) {
        pthread_exit(NULL);
    }
}
#endif

static void handle_timeout_worker(BenderTimeout to, int cancel) {
    if (!to.has_worker) return;
#ifdef BENDER_WINDOWS
    if (cancel && to.worker_event) {
        SetEvent(to.worker_event);
    }
    WaitForSingleObject(to.worker, INFINITE);
    CloseHandle(to.worker);
    if (to.worker_event) {
        CloseHandle(to.worker_event);
    }
#else
#ifdef BENDER_ANDROID
    if (cancel) bender_assert(!pthread_kill(to.worker, SIGUSR1), "pthread_kill");
#else
    if (cancel) bender_assert(!pthread_cancel(to.worker), "pthread_cancel");
#endif
    void *res = NULL;
    bender_assert(!pthread_join(to.worker, &res), "pthread_join");
#endif
}

/* Common deinit code */
void bender_ev_deinit_common(void) {
    BenderTimeout to;
    while (peek_timeout(&to)) {
        handle_timeout_worker(to, 1);
        pop_timeout(0);
    }
    bender_q_deinit(&bender_vm.spawn);
    bender_free(bender_vm.tq);
    bender_table_deinit(&bender_vm.threaded_abstracts);
    bender_table_deinit(&bender_vm.active_tasks);
    bender_table_deinit(&bender_vm.signal_handlers);
#ifndef BENDER_WINDOWS
    pthread_attr_destroy(&bender_vm.new_thread_attr);
#endif
}

/* Shorthand to yield to event loop */
void bender_await(void) {
    /* LIVENESS for batched routed spawns: the single path every event wait
     * takes, so exactly "this fiber is about to park", and a spawn burst never
     * reaches it. Before the signal, since bender_signalv longjmps. */
    bender_proc_spawn_batch_flush_for(NULL);
    /* Store the fiber in a global table */
    bender_signalv(BENDER_SIGNAL_EVENT, bender_wrap_nil());
}

/* Set timeout for the operation that is about to suspend.
 *
 * The task identity is root_fiber: only it carries the scheduler flag that
 * bender_cancel requires, and a deadline to a non-task child fiber can never be
 * delivered. What must be operation-scoped is the lifetime of the entry, not
 * this pointer -- see bender_drop_deadlines_for. */
void bender_addtimeout(double sec) {
    BenderFiber *fiber = bender_vm.root_fiber;
    BenderTimeout to;
    to.when = ts_delta(ts_now(), sec);
    to.fiber = fiber;
    to.curr_fiber = NULL;
    to.sched_id = fiber->sched_id;
    to.is_error = 1;
    to.has_worker = 0;
    add_timeout(to);
}

/* Set timeout for the current root fiber but resume with nil instead of raising an error */
void bender_addtimeout_nil(double sec) {
    BenderFiber *fiber = bender_vm.root_fiber;
    BenderTimeout to;
    to.when = ts_delta(ts_now(), sec);
    to.fiber = fiber;
    to.curr_fiber = NULL;
    to.sched_id = fiber->sched_id;
    to.is_error = 0;
    to.has_worker = 0;
    add_timeout(to);
}

static void bender_timeout_cb(BenderEVGenericMessage msg) {
    (void) msg;
    bender_interpreter_interrupt_handled(&bender_vm);
}

#ifdef BENDER_WINDOWS
static DWORD WINAPI bender_timeout_body(LPVOID ptr) {
    BenderThreadedTimeout tto = *(BenderThreadedTimeout *)ptr;
    bender_free(ptr);
    BenderTimestamp wait_begin = ts_now();
    DWORD duration = (DWORD)round(tto.sec * 1000);
    DWORD res = WAIT_TIMEOUT;
    BenderTimestamp wait_end = ts_now();
    for (DWORD i = 1; res == WAIT_TIMEOUT && (wait_end - wait_begin) < duration; i++) {
        res = WaitForSingleObject(tto.cancel_event, (duration + i));
        wait_end = ts_now();
    }
    /* only send interrupt message if result is WAIT_TIMEOUT */
    if (res == WAIT_TIMEOUT) {
        bender_interpreter_interrupt(tto.vm);
        BenderEVGenericMessage msg = {0};
        bender_ev_post_event(tto.vm, bender_timeout_cb, msg);
    }
    return 0;
}
#else
static void *bender_timeout_body(void *ptr) {
#ifdef BENDER_ANDROID
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0;
    action.sa_handler = &bender_timeout_stop;
    sigaction(SIGUSR1, &action, NULL);
#endif
    BenderThreadedTimeout tto = *(BenderThreadedTimeout *)ptr;
    bender_free(ptr);
    struct timespec ts;
    ts.tv_sec = (time_t) tto.sec;
    ts.tv_nsec = (tto.sec <= UINT32_MAX)
                 ? (long)((tto.sec - ((uint32_t)tto.sec)) * 1000000000)
                 : 0;
    nanosleep(&ts, &ts);
    bender_interpreter_interrupt(tto.vm);
    BenderEVGenericMessage msg = {0};
    bender_ev_post_event(tto.vm, bender_timeout_cb, msg);
    return NULL;
}
#endif


void bender_ev_inc_refcount(void) {
    bender_atomic_inc(&bender_vm.listener_count);
}

void bender_ev_dec_refcount(void) {
    bender_atomic_dec(&bender_vm.listener_count);
}

/* Channels */

#define BENDER_MAX_CHANNEL_CAPACITY 0xFFFFFF

static inline int bender_chan_is_threaded(BenderChannel *chan) {
    return chan->is_threaded;
}

/* Step 1.4: values are marshal-copied through isolated channels the
 * same way as threaded ones (same transit ownership); only the lock
 * stays threaded-only (isolated channels are single-VM by contract). */
static inline int bender_chan_needs_pack(BenderChannel *chan) {
    return chan->is_threaded || chan->isolated;
}

static int bender_chan_pack(BenderChannel *chan, Bender *x) {
    if (!bender_chan_needs_pack(chan)) return 0;
    switch (bender_type(*x)) {
        default: {
            BenderBuffer *buf = bender_malloc(sizeof(BenderBuffer));
            if (NULL == buf) {
                BENDER_OUT_OF_MEMORY;
            }
            bender_buffer_init(buf, 10);
            bender_marshal(buf, *x, NULL, BENDER_MARSHAL_UNSAFE);
            *x = bender_wrap_buffer(buf);
            return 0;
        }
        case BENDER_NIL:
        case BENDER_NUMBER:
        case BENDER_POINTER:
        case BENDER_BOOLEAN:
        case BENDER_CFUNCTION:
            return 0;
    }
}

static int bender_chan_unpack(BenderChannel *chan, Bender *x, int is_cleanup) {
    if (!bender_chan_needs_pack(chan)) return 0;
    switch (bender_type(*x)) {
        default:
            return 1;
        case BENDER_BUFFER: {
            BenderBuffer *buf = bender_unwrap_buffer(*x);
            int flags = is_cleanup ? (BENDER_MARSHAL_UNSAFE | BENDER_MARSHAL_DECREF) : BENDER_MARSHAL_UNSAFE;
            *x = bender_unmarshal(buf->data, buf->count, flags, NULL, NULL);
            bender_buffer_deinit(buf);
            bender_free(buf);
            return 0;
        }
        case BENDER_NIL:
        case BENDER_NUMBER:
        case BENDER_POINTER:
        case BENDER_BOOLEAN:
        case BENDER_CFUNCTION:
            return 0;
    }
}

static void bender_chan_init(BenderChannel *chan, int32_t limit, int threaded, int isolated) {
    chan->limit = limit;
    chan->closed = 0;
    chan->is_threaded = threaded;
    chan->isolated = isolated;
    chan->proc_alive = 1;
    /* Step 4.1: isolated channels pin to the home lane (this thread's
     * VM — one VM per thread, so the address is the lane identity).
     * Unmarshal re-inits, so ownership follows the value. */
    /* Home-lane pin. Isolated channels were already pinned (lock-free
     * single-VM contract). Plain channels get the same pin for a
     * different and much worse reason: a plain channel has NO lock and
     * NO marshal copy, so a worker lane pushing into a main-VM channel
     * corrupts the queue header of a VM it does not own -- and because
     * the value is never copied the reader can never see it. Observed
     * as a silent hang (no error, no data) when one proc spawned on a
     * lane pushed to a plain ev/chan. Pinning turns that into the same
     * loud "use ev/thread-chan" panic isolated channels already give.
     * Threaded channels stay unpinned: they take the lock and copy. */
    chan->detached_copy = 0;
    chan->owner = (isolated || (!threaded)) ? &bender_vm : NULL;
    bender_q_init(&chan->items);
    bender_q_init(&chan->read_pending);
    bender_q_init(&chan->write_pending);
    bender_os_mutex_init((BenderOSMutex *) &chan->lock);
}

/* (The old bender_chan_purge_fiber is gone: it was dead code — no
 * death path ever called it, which is how stale pendings survived to
 * crash push walks. The chan_pends registration makes reclamation
 * structural instead: fiber free marks its queued entries dead, and
 * every consumer path reclaims them. Root balance still mirrors
 * registration: only threaded pendings root, and every threaded
 * claim/unroot is preserved below.) */

void bender_channel_set_proc_alive(BenderChannel *chan, int alive) {
    chan->proc_alive = alive;
}

int bender_channel_proc_alive(BenderChannel *chan) {
    return chan->proc_alive;
}

static void bender_chan_lock(BenderChannel *chan) {
    if (!bender_chan_is_threaded(chan)) return;
    bender_os_mutex_lock((BenderOSMutex *) &chan->lock);
}

static void bender_chan_unlock(BenderChannel *chan) {
    if (!bender_chan_is_threaded(chan)) return;
    bender_os_mutex_unlock((BenderOSMutex *) &chan->lock);
}

/* Drain one pending queue at channel deinit: entries die with the
 * queue, nodes are released or left to their (living) fibers with
 * chan_dead set. Never dereferences fibers — only nodes. */
static void flag_chanpend_node(BenderChannelPending *p) {
    BenderChanPendReg *reg = p->reg;
    if (NULL != reg) {
        reg->in_queue = 0;
        reg->chan_dead = 1;
        /* Only dead-fiber nodes free here. A dequeued node whose
         * fiber still lives remains on that fiber's chan_pends list —
         * freeing it here would leave a dangling entry for the
         * fiber's prune/deinit to double-free. The fiber reclaims it
         * (dequeued ⇒ prune's free branch); a dead fiber's node has
         * no future walker, so it frees here. */
        if (reg->dead) bender_free(reg);
    }
}

/* Flag one pending queue's nodes at channel deinit. The queue array
 * itself is abandoned intact (upstream discipline). Two-phase: flag
 * every reachable reg first (chan_dead=1; dead-fiber slots are nulled
 * and collected), then free the collected dead regs. Single-pass
 * freed a reg and then revisited a duplicate entry sharing it
 * (ASAN: flag_chanpend_node UAF at density), and frees must stay
 * ordered after all flagging so no walker sees a half-state. Living
 * fibers' regs are never freed here -- in_queue drops to 0 and their
 * own prune reclaims them. Never dereferences fibers -- only nodes. */
static void bender_chan_deinit_q(BenderQueue *q) {
    BenderChannelPending *pending = q->data;
    if (NULL != pending) {
        int32_t ranges[2][2];
        int nranges = 0;
        if (q->head <= q->tail) {
            ranges[nranges][0] = q->head;
            ranges[nranges++][1] = q->tail;
        } else {
            ranges[nranges][0] = q->head;
            ranges[nranges++][1] = q->capacity;
            ranges[nranges][0] = 0;
            ranges[nranges++][1] = q->tail;
        }
        /* Pass 1: flag, collect dead regs, null their slots. */
        BenderChanPendReg *dead[64];
        size_t ndead = 0;
        int overflow = 0;
        for (int r = 0; r < nranges; r++) {
            for (int32_t i = ranges[r][0]; i < ranges[r][1]; i++) {
                BenderChanPendReg *reg = pending[i].reg;
                if (NULL == reg) continue;
                reg->chan_dead = 1;
                if (reg->dead) {
                    if (ndead < 64) {
                        dead[ndead++] = reg;
                        pending[i].reg = NULL;
                    } else {
                        overflow = 1;
                    }
                } else {
                    reg->in_queue = 0;
                }
            }
        }
        /* Pass 2: free the collected dead regs (no future walker:
         * dead fibers never prune). */
        for (size_t k = 0; k < ndead; k++) bender_free(dead[k]);
        if (overflow) {
            /* Rare: more than 64 dead regs. Sweep again for the
             * remainder (slots were left intact for this pass). */
            for (int r = 0; r < nranges; r++) {
                for (int32_t i = ranges[r][0]; i < ranges[r][1]; i++) {
                    BenderChanPendReg *reg = pending[i].reg;
                    if (NULL != reg && reg->dead) {
                        bender_free(reg);
                        pending[i].reg = NULL;
                    }
                }
            }
        }
    }
    bender_q_deinit(q);
}

static void bender_chan_deinit(BenderChannel *chan) {
    /* LOCK the whole deinit: the pending-queue walk reads head/tail
     * bounds and reg nodes that foreign threads mutate concurrently
     * (claim pops, prune frees). Previously only pack channels took
     * the lock here, and the density teardown volume exposed the
     * unlocked walk as a use-after-free (ASAN: flag_chanpend_node
     * reading a node freed by the owning fiber's VM mid-walk). */
    bender_chan_lock(chan);
    if (bender_chan_needs_pack(chan)) {
        Bender item;
        bender_chan_deinit_q(&chan->read_pending);
        bender_chan_deinit_q(&chan->write_pending);
        while (!bender_q_pop(&chan->items, &item, sizeof(item))) {
            bender_chan_unpack(chan, &item, 1);
        }
        bender_q_deinit(&chan->items);
    } else {
        bender_chan_deinit_q(&chan->read_pending);
        bender_chan_deinit_q(&chan->write_pending);
        bender_q_deinit(&chan->items);
    }
    bender_chan_unlock(chan);
    bender_os_mutex_deinit((BenderOSMutex *) &chan->lock);
}

/*
 * Bender Channel abstract type
 */

static Bender bender_wrap_channel(BenderChannel *channel) {
    return bender_wrap_abstract(channel);
}

/* proc/kill on a recv-parked victim: remove the fiber's read pendings
 * from an isolated (same-VM, non-threaded) channel and error-resume it
 * so the kill lands immediately instead of at a next resume that may
 * never come. recv is own-mailbox only, so pendings found here belong
 * to the victim on this VM. Returns 1 when a pending was cancelled. */
int bender_channel_cancel_reader(BenderChannel *channel, BenderFiber *fiber, Bender reason) {
    if (NULL == channel || channel->is_threaded) return 0;
    BenderQueue *fq = &channel->read_pending;
    int32_t count = bender_q_count(fq);
    if (0 == count) return 0;
    BenderChannelPending *kept =
        bender_malloc(sizeof(BenderChannelPending) * (size_t) count);
    if (NULL == kept) return 0;
    int32_t nkept = 0;
    int cancelled = 0;
    int32_t i = fq->head;
    for (int32_t n = 0; n < count; n++) {
        BenderChannelPending p;
        memcpy(&p, (char *) fq->data + i * sizeof(BenderChannelPending),
               sizeof(p));
        i = i + 1 < fq->capacity ? i + 1 : 0;
        if (!cancelled && p.thread == &bender_vm && p.fiber == fiber) {
            /* Claim the node: unlinks the fiber's registration, frees
             * dead regs. Live entries schedule; stale ones drop. */
            if (bender_chanpend_claim_plain(&p)) {
                bender_schedule_signal(fiber, reason, BENDER_SIGNAL_ERROR);
                cancelled = 1;
            }
            continue; /* consumed either way */
        }
        kept[nkept++] = p;
    }
    if (cancelled) {
        int32_t w = fq->head;
        for (int32_t k = 0; k < nkept; k++) {
            memcpy((char *) fq->data + w * sizeof(BenderChannelPending),
                   kept + k, sizeof(BenderChannelPending));
            w = w + 1 < fq->capacity ? w + 1 : 0;
        }
        fq->tail = w;
    }
    bender_free(kept);
    return cancelled;
}

/* Milestone 2c: current item depth of a channel (the :steal policy's
 * high-water check on lane control channels). */
BENDER_API int32_t bender_channel_count(void *chan) {
    BenderChannel *channel = chan;
    return bender_q_count(&channel->items);
}

static int bender_chanat_gc(void *p, size_t s) {
    (void) s;
    BenderChannel *channel = p;
    bender_chan_deinit(channel);
    return 0;
}

static void bender_chanat_remove_vmref_one(BenderChannelPending *pending) {
    if (pending->thread == &bender_vm) {
        pending->thread = NULL;
        bender_gcunroot(bender_wrap_fiber(pending->fiber));
    }
}

static void bender_chanat_remove_vmref(BenderQueue *fq) {
    BenderChannelPending *pending = fq->data;
    if (fq->head <= fq->tail) {
        for (int32_t i = fq->head; i < fq->tail; i++) {
            bender_chanat_remove_vmref_one(pending + i);
        }
    } else {
        for (int32_t i = fq->head; i < fq->capacity; i++) {
            bender_chanat_remove_vmref_one(pending + i);
        }
        for (int32_t i = 0; i < fq->tail; i++) {
            bender_chanat_remove_vmref_one(pending + i);
        }
    }
}

static int bender_chanat_gcperthread(void *p, size_t s) {
    (void) s;
    BenderChannel *chan = p;
    bender_chan_lock(chan);
    /* Make sure that the internals of the channel no longer reference
     * _this_ thread. Replace those references with NULL. Entries are
     * deliberately NOT dropped here: this finalizer also fires when a
     * plain/isolated channel is freed while its parked fibers are
     * still alive (owner-heap retirement), and a later push must
     * still schedule them through the NULL-thread entry (the original
     * upstream discipline). Node lifecycle is untouched by this
     * callback: chan_deinit runs right after it, flags remaining
     * nodes chan_dead, and each fiber's prune/deinit reclaims the
     * node without ever dereferencing the channel again. */
    bender_chanat_remove_vmref(&chan->read_pending);
    bender_chanat_remove_vmref(&chan->write_pending);
    bender_chan_unlock(chan);
    return 0;
}

static void bender_chanat_mark_fq(BenderQueue *fq) {
    BenderChannelPending *pending = fq->data;
/* A stale entry (its fiber was resumed some other way, e.g. by another
 * clause of ev/select, so sched_id no longer matches) must not pin the
 * fiber: nothing may ever visit this channel again, so the pin would be
 * permanent (observed: 100/100 leaked fibers). Only skip the mark here —
 * never claim/free the node from inside GC (the queue entry stays valid;
 * a later pop or bender_chan_remove_stale reclaims it under the channel's
 * normal discipline). Reading ->sched_id of a same-VM fiber is safe:
 * plain-channel entries only ever reference this VM's fibers, and only
 * this VM's scheduler mutates sched_id. */
#define MARK_ONE(idx) do { \
        BenderChannelPending *p = &pending[idx]; \
        if (!(p->reg->dead)) { \
            if (!(p->thread == &bender_vm && \
                    p->fiber->sched_id != (uint32_t) p->sched_id)) { \
                bender_mark(bender_wrap_fiber(p->fiber)); \
            } \
        } \
    } while (0)
    if (fq->head <= fq->tail) {
        for (int32_t i = fq->head; i < fq->tail; i++)
            MARK_ONE(i);
    } else {
        for (int32_t i = fq->head; i < fq->capacity; i++)
            MARK_ONE(i);
        for (int32_t i = 0; i < fq->tail; i++)
            MARK_ONE(i);
    }
#undef MARK_ONE
}

static int bender_chanat_mark(void *p, size_t s) {
    (void) s;
    BenderChannel *chan = p;
    if (bender_chan_is_threaded(chan)) {
        /* Threaded-channel internals must not be marked from an
         * arbitrary VM's GC. Every mutation of items/read_pending/
         * write_pending (push growth realloc, pop+unpack freeing the
         * transit buffer, pending claims, teardown deinit) happens
         * under the channel lock on SOME OTHER thread, so an
         * unlocked mark iterates freed memory (ASAN-confirmed
         * heap-use-after-free at the loops below: the items.data
         * array freed by a concurrent push's realloc, and item
         * values freed by a concurrent pop's unpack). Taking the
         * lock here instead is not an option: marking while holding
         * a channel's lock recurses into marking other threaded
         * abstracts reachable from the marked values and deadlocks/
         * livelocks under contention (verified by a reverted fix
         * attempt). Marking is also unnecessary here:
         *  - items only ever hold immediates (no mark needed) or
         *    transit buffers from bender_chan_pack -- raw bender_malloc
         *    memory that is NOT a GC object on any VM's heap; its
         *    lifetime is queue ownership (freed only by the popper
         *    or deinit, under the lock), so no collector ever needs
         *    to keep it alive;
         *  - every fiber parked on a threaded channel (both the
         *    push path and the pop path) is bender_gcroot'd by its
         *    OWN VM while queued (see the bender_gcroot calls beside
         *    both pending enqueues), so the pending fibers are kept
         *    alive by their enqueuing VMs, not by this mark.
         * Plain/isolated channels are single-VM by contract: their
         * pendings are NOT rooted and their raw item values ARE
         * heap-shared, so they keep the full mark below. */
        return 0;
    }
    bender_chanat_mark_fq(&chan->read_pending);
    bender_chanat_mark_fq(&chan->write_pending);
    BenderQueue *items = &chan->items;
    Bender *data = chan->items.data;
    if (items->head <= items->tail) {
        for (int32_t i = items->head; i < items->tail; i++)
            bender_mark(data[i]);
    } else {
        for (int32_t i = items->head; i < items->capacity; i++)
            bender_mark(data[i]);
        for (int32_t i = 0; i < items->tail; i++)
            bender_mark(data[i]);
    }
    return 0;
}

static Bender make_write_result(BenderChannel *channel) {
    Bender *tup = bender_tuple_begin(2);
    tup[0] = bender_ckeywordv("give");
    tup[1] = bender_wrap_channel(channel);
    return bender_wrap_tuple(bender_tuple_end(tup));
}

static Bender make_read_result(BenderChannel *channel, Bender x) {
    Bender *tup = bender_tuple_begin(3);
    tup[0] = bender_ckeywordv("take");
    tup[1] = bender_wrap_channel(channel);
    tup[2] = x;
    return bender_wrap_tuple(bender_tuple_end(tup));
}

static Bender make_close_result(BenderChannel *channel) {
    Bender *tup = bender_tuple_begin(2);
    tup[0] = bender_ckeywordv("close");
    tup[1] = bender_wrap_channel(channel);
    return bender_wrap_tuple(bender_tuple_end(tup));
}

/* Callback to use for scheduling a fiber from another thread. */
static void bender_thread_chan_cb(BenderEVGenericMessage msg) {
    uint32_t sched_id = (uint32_t) msg.argi;
    BenderFiber *fiber = msg.fiber;
    int mode = msg.tag;
    BenderChannel *channel = (BenderChannel *) msg.argp;
    Bender x = msg.argj;
    bender_chan_lock(channel);
    if (fiber->sched_id == sched_id) {
        if (mode == BENDER_CP_MODE_CHOICE_READ) {
            bender_assert(!bender_chan_unpack(channel, &x, 0), "packing error");
            bender_schedule(fiber, make_read_result(channel, x));
        } else if (mode == BENDER_CP_MODE_CHOICE_WRITE) {
            bender_schedule(fiber, make_write_result(channel));
        } else if (mode == BENDER_CP_MODE_READ) {
            bender_assert(!bender_chan_unpack(channel, &x, 0), "packing error");
            bender_schedule(fiber, x);
        } else if (mode == BENDER_CP_MODE_WRITE) {
            bender_schedule(fiber, bender_wrap_channel(channel));
        } else { /* (mode == BENDER_CP_MODE_CLOSE) */
            bender_schedule(fiber, bender_wrap_nil());
        }
    } else if (mode != BENDER_CP_MODE_CLOSE) {
        /* Fiber has already been canceled or resumed. */
        /* Resend event to another waiting thread, depending on mode */
        int is_read = (mode == BENDER_CP_MODE_CHOICE_READ) || (mode == BENDER_CP_MODE_READ);
        if (is_read) {
            BenderChannelPending reader;
            int sent = 0;
            while (!bender_q_pop(&channel->read_pending, &reader, sizeof(reader))) {
                bender_chanpend_claim_threaded(&reader);
                BenderVM *vm = reader.thread;
                if (!vm) continue;
                if (vm == &bender_vm) {
                    /* BUG 1 sibling: same-VM pending on a threaded
                     * channel. Posting through the self-pipe from the
                     * loop's own thread fills the pipe (~8000 writes
                     * at a 4000-value fan-in) and the loop then
                     * deadlocks inside its own wakeup write. Schedule
                     * inline instead -- we are the loop. */
                    uint32_t sid = (uint32_t) reader.sched_id;
                    if (reader.fiber->sched_id == sid) {
                        bender_gcunroot(bender_wrap_fiber(reader.fiber));
                        if (reader.mode == BENDER_CP_MODE_CHOICE_READ) {
                            bender_assert(!bender_chan_unpack(channel, &x, 0),
                                    "packing error");
                            bender_schedule(reader.fiber,
                                    make_read_result(channel, x));
                        } else if (reader.mode == BENDER_CP_MODE_READ) {
                            bender_assert(!bender_chan_unpack(channel, &x, 0),
                                    "packing error");
                            bender_schedule(reader.fiber, x);
                        } else {
                            bender_schedule(reader.fiber,
                                    make_write_result(channel));
                        }
                    }
                    sent = 1;
                    break;
                }
                BenderEVGenericMessage msg;
                msg.tag = reader.mode;
                msg.fiber = reader.fiber;
                msg.argi = (int32_t) reader.sched_id;
                msg.argp = channel;
                msg.argj = x;
                bender_ev_post_event(vm, bender_thread_chan_cb, msg);
                sent = 1;
                break;
            }
            if (!sent) {
                if (channel->closed || bender_q_push_head(&channel->items, &x, sizeof(Bender))) {
                    /* channel is closed or full. Otherwise, keep it. */
                    bender_chan_unpack(channel, &x, 1);
                }
            }
        } else {
            BenderChannelPending writer;
            while (!bender_q_pop(&channel->write_pending, &writer, sizeof(writer))) {
                bender_chanpend_claim_threaded(&writer);
                BenderVM *vm = writer.thread;
                if (!vm) continue;
                BenderEVGenericMessage msg;
                msg.tag = writer.mode;
                msg.fiber = writer.fiber;
                msg.argi = (int32_t) writer.sched_id;
                msg.argp = channel;
                msg.argj = bender_wrap_nil();
                bender_ev_post_event(vm, bender_thread_chan_cb, msg);
                break;
            }
        }
    }
    bender_chan_unlock(channel);
    bender_gcunroot(bender_wrap_fiber(fiber));
}

/* Remove pending entries of this thread whose fiber has since been resumed some
 * other way (e.g. by another clause of ev/select), releasing the gc root taken
 * when each was queued. Entries still in the queue have nothing posted for them,
 * so each root is released exactly once. Fibers of other threads are never
 * dereferenced. Must hold the channel lock. Only scans when the queue length is
 * a power of two, which keeps this amortized O(1) per call while bounding stale
 * entries to about the number of live ones. Dead-node entries (fiber freed —
 * only possible post-gcperthread, when the root is already gone) are dropped
 * without touching the fiber. */
static void bender_chan_remove_stale(BenderQueue *fq) {
    int32_t count = bender_q_count(fq);
    if (count & (count - 1)) return;
    for (int32_t i = 0; i < count; i++) {
        BenderChannelPending pending;
        bender_q_pop(fq, &pending, sizeof(pending));
        if (pending.reg->dead) {
            /* Fiber gone: no root held (threaded roots keep fibers
             * alive), reclaim the node. */
            bender_free(pending.reg);
        } else if (pending.thread == &bender_vm && pending.sched_id != pending.fiber->sched_id) {
            bender_chanpend_claim_threaded(&pending);
            bender_gcunroot(bender_wrap_fiber(pending.fiber));
        } else {
            /* Live items must be inserted back in the same order */
            bender_q_push(fq, &pending, sizeof(pending));
        }
    }
}

/* Push a value to a channel, and return 1 if channel should block, zero otherwise.
 * If the push would block, will add to the write_pending queue in the channel.
 * Handles both threaded and unthreaded channels. */
/* BUG 2 density: in-flight channel operations on foreign VMs held no
 * refcount, so a home teardown at refcount-0 could destroy the
 * channel struct (and deinit its mutex) while a foreign push/pop was
 * entering. Each op now takes a claim at entry and drops it before
 * its synchronous return; parked-waiter safety continues to come
 * from the pending-node discipline (deinit flags nodes, never
 * dereferences fibers). Panic paths longjmp past the drop -- a
 * leaked claim only over-retains. The zero-drop teardown is deferred
 * to the loop: we may still hold the channel lock here, and freeing
 * under it would hand the unlock a freed mutex. */
extern void bender_abstract_home_teardown_cb(BenderEVGenericMessage msg);
static void bender_chan_inflight_release(BenderChannel *channel) {
    if (!bender_chan_is_threaded(channel)) return;
    if (0 == bender_abstract_decref((void *) channel)) {
        BenderAbstractHead *head = bender_abstract_head(channel);
        BenderEVGenericMessage msg;
        memset(&msg, 0, sizeof(msg));
        msg.argp = (void *) channel;
        bender_ev_post_event((BenderVM *) head->home_vm,
                bender_abstract_home_teardown_cb, msg);
    }
}
#define BENDER_CHAN_INFLIGHT_ACQUIRE(chan) \
    do { if (bender_chan_is_threaded(chan)) \
        bender_abstract_incref((void *) (chan)); } while (0)
#define BENDER_CHAN_INFLIGHT_DROP(chan) \
    do { bender_chan_inflight_release(chan); } while (0)

static int bender_channel_push_with_lock(BenderChannel *channel, Bender x, int mode) {
    BenderChannelPending reader;
    int is_empty;
    BENDER_CHAN_INFLIGHT_ACQUIRE(channel);
    if (channel->closed) {
        bender_chan_unlock(channel);
        BENDER_CHAN_INFLIGHT_DROP(channel);
        bender_panic("cannot write to closed channel");
    }
    /* P3.4: plain (non-packing) channels share by reference, so a
     * strict-heap value pushed here would leak invisibly to per-proc
     * majors. Refuse (packing channels marshal-copy, so they are
     * safe). Mode-2 supervisor pushes exempt: their events stay
     * reachable from the owning proc struct (marked + traversed —
     * no hole). Gated: legacy VMs skip for one branch; immediate and
     * immutable string-heap values exempt (no refs to leak). */
    if (2 != mode && bender_vm.heap_strict_live && !bender_chan_needs_pack(channel)) {
        BenderType xt = bender_type(x);
        if (xt != BENDER_NIL && xt != BENDER_BOOLEAN && xt != BENDER_NUMBER &&
                xt != BENDER_CFUNCTION && xt != BENDER_POINTER &&
                xt != BENDER_STRING && xt != BENDER_SYMBOL && xt != BENDER_KEYWORD) {
            void *xobj = NULL;
            switch (xt) {
                case BENDER_ARRAY:
                    xobj = (void *) bender_unwrap_array(x);
                    break;
                case BENDER_TUPLE:
                    xobj = (void *) bender_tuple_head(bender_unwrap_tuple(x));
                    break;
                case BENDER_TABLE:
                    xobj = (void *) bender_unwrap_table(x);
                    break;
                case BENDER_STRUCT:
                    xobj = (void *) bender_struct_head(bender_unwrap_struct(x));
                    break;
                case BENDER_BUFFER:
                    xobj = (void *) bender_unwrap_buffer(x);
                    break;
                case BENDER_FUNCTION:
                    xobj = (void *) bender_unwrap_function(x);
                    break;
                case BENDER_ABSTRACT:
                    xobj = (void *) bender_abstract_head(bender_unwrap_abstract(x));
                    break;
                case BENDER_FIBER:
                    xobj = (void *) bender_unwrap_fiber(x);
                    break;
                default:
                    break;
            }
            if (NULL != xobj &&
                    (bender_gc_header(xobj)->flags & BENDER_MEM_ISOLATED)) {
                bender_chan_unlock(channel);
                bender_panic("strict proc value escaping through a "
                            "non-copying channel (use ev/ichan or "
                            "ev/thread-chan)");
            }
        }
    }
    if (bender_chan_pack(channel, &x)) {
        bender_chan_unlock(channel);
        bender_panicf("failed to pack value for channel: %v", x);
    }
    int is_threaded = bender_chan_is_threaded(channel);
    if (is_threaded) {
        /* don't dereference fiber from another thread */
        is_empty = bender_q_pop(&channel->read_pending, &reader, sizeof(reader));
        if (!is_empty) bender_chanpend_claim_threaded(&reader);
    } else {
        for (;;) {
            is_empty = bender_q_pop(&channel->read_pending, &reader, sizeof(reader));
            if (is_empty) break;
                    /* Registration makes this sound without GC probing: dead
             * fibers are reclaimed by node and never dereferenced;
             * stale entries (fiber rescheduled some other way) drop
             * here exactly as in the pre-P3.5c discipline. */
            if (bender_chanpend_claim_plain(&reader)) break;
        }
    }
    if (is_empty) {
        /* No pending reader */
        if (mode == 3 && bender_q_count(&channel->items) >= channel->limit) {
            /* Try-mode: report full without buffering (mode 2 keeps the
             * historical buffer-beyond-limit behavior for supervisor
             * pushes, which ignore the return value). Must unpack the
             * packed copy to avoid leaking the threaded marshal buffer. */
            bender_chan_unpack(channel, &x, 1);
            bender_chan_unlock(channel);
            BENDER_CHAN_INFLIGHT_DROP(channel);
            return 1;
        }
        if (bender_q_push(&channel->items, &x, sizeof(Bender))) {
            bender_chan_unpack(channel, &x, 1);
            bender_chan_unlock(channel);
            bender_panicf("channel overflow: %v", x);
        } else if (bender_q_count(&channel->items) > channel->limit) {
            /* No root fiber, we are in completion on a root fiber. Don't block. */
            if (mode == 2) {
                bender_chan_unlock(channel);
                BENDER_CHAN_INFLIGHT_DROP(channel);
                return 1;
            }
            /* Pushed successfully, but should block. */
            BenderChannelPending pending;
            pending.thread = &bender_vm;
            pending.fiber = bender_vm.root_fiber,
            pending.sched_id = bender_vm.root_fiber->sched_id,
            pending.mode = mode ? BENDER_CP_MODE_CHOICE_WRITE : BENDER_CP_MODE_WRITE;
            if (is_threaded) {
                bender_chan_remove_stale(&channel->write_pending);
            }
            pending.reg = bender_chanpend_register(pending.fiber, channel, 1);
            bender_q_push(&channel->write_pending, &pending, sizeof(pending));
            bender_chan_unlock(channel);
            if (is_threaded) {
                bender_gcroot(bender_wrap_fiber(pending.fiber));
            }
            BENDER_CHAN_INFLIGHT_DROP(channel);
            return 1;
        }
    } else {
        /* Pending reader */
        if (is_threaded) {
            BenderVM *vm = reader.thread;
            if (vm == &bender_vm) {
                /* BUG 1 sibling: same-VM reader pending on a threaded
                 * channel (the density/recv park shape). Schedule
                 * inline -- a self-pipe write from the loop's own
                 * thread floods the pipe at fan-in scale and the loop
                 * deadlocks inside its own wakeup write. */
                uint32_t sid = (uint32_t) reader.sched_id;
                if (reader.fiber->sched_id == sid) {
                    bender_gcunroot(bender_wrap_fiber(reader.fiber));
                    if (reader.mode == BENDER_CP_MODE_CHOICE_READ) {
                        bender_assert(!bender_chan_unpack(channel, &x, 0),
                                "bad channel packing");
                        bender_schedule(reader.fiber,
                                make_read_result(channel, x));
                    } else {
                        bender_assert(!bender_chan_unpack(channel, &x, 0),
                                "bad channel packing");
                        bender_schedule(reader.fiber, x);
                    }
                } else {
                    /* Stale reader (resumed via another clause, whose
                     * path already unrooted): keep the value for the
                     * next reader -- push-head, then release the
                     * packed buffer, like the pipe callback's stale
                     * path. */
                    if (channel->closed ||
                            bender_q_push_head(&channel->items, &x,
                                sizeof(Bender))) {
                        bender_chan_unpack(channel, &x, 1);
                    }
                }
            } else {
                BenderEVGenericMessage msg;
                msg.tag = reader.mode;
                msg.fiber = reader.fiber;
                msg.argi = (int32_t) reader.sched_id;
                msg.argp = channel;
                msg.argj = x;
                if (vm) {
                    bender_ev_post_event(vm, bender_thread_chan_cb, msg);
                } else {
                    /* If no vm to send to, we must clean up (unpack) the packed payload to avoid leak */
                    bender_chan_unpack(channel, &x, 1);
                }
            }
        } else {
            /* Same-VM handoff: decode transit values first (no-op for
             * plain channels, which never pack; the threaded path
             * decodes in its completion callback instead). */
            bender_assert(!bender_chan_unpack(channel, &x, 0), "bad channel packing");
            if (reader.mode == BENDER_CP_MODE_CHOICE_READ) {
                bender_schedule(reader.fiber, make_read_result(channel, x));
            } else {
                bender_schedule(reader.fiber, x);
            }
        }
    }
    bender_chan_unlock(channel);
    BENDER_CHAN_INFLIGHT_DROP(channel);
    return 0;
}

/* A channel is home-lane pinned when it takes no lock and copies nothing
 * (plain and isolated). Touching one from another lane is corruption, not
 * merely a lost value, and for a zero-capacity channel it parks a fiber into
 * a pending queue whose owning VM is not polling this thread -- an unkillable
 * silent hang. One predicate so push, try-push and pop agree. */
static int bender_chan_foreign_lane(BenderChannel *channel) {
    /* A detached copy of a plain channel is the marshalled echo of a channel
     * that lives in another lane: using it would move values into a queue its
     * real owner never reads, silently. Treat that exactly like a foreign
     * lane, because that is what it is. */
    if (channel->detached_copy && !channel->is_threaded) return 1;
    return NULL != channel->owner && channel->owner != &bender_vm;
}

static BENDER_NO_RETURN void bender_chan_foreign_lane_panic(BenderChannel *channel) {
    bender_panic(channel->isolated
                ? "isolated channel used from a foreign lane (pinned to its home lane; use ev/thread-chan across lanes)"
                : (channel->detached_copy
                   ? "plain ev/chan arrived as a MARSHALLED COPY of a channel owned by another lane; writes to it would be lost silently (pass messages to the worker's own mailbox, or use ev/thread-chan across lanes)"
                   : "plain ev/chan used from a foreign lane (it takes no lock and copies nothing, so a blocking write parks forever and a buffered one silently loses the value -- use ev/thread-chan across lanes)"));
}

int bender_channel_push(BenderChannel *channel, Bender x, int mode) {
    /* ORDERING: nothing may overtake a pending spawn batch to the same lane. */
    bender_proc_spawn_batch_flush_for((void *) channel);
    /* Step 4.1 home-lane pin: isolated channels are lock-free, so a
     * foreign thread would race silently — refuse loudly instead
     * (use ev/thread-chan across threads). One predictable branch on
     * the non-isolated fast path. */
    if (bender_chan_foreign_lane(channel)) bender_chan_foreign_lane_panic(channel);
    bender_chan_lock(channel);
    return bender_channel_push_with_lock(channel, x, mode);
}

/* Pop from a channel - returns 1 if item was obtained, 0 otherwise. The item
 * is returned by reference. If the pop would block, will add to the read_pending
 * queue in the channel. */
static int bender_channel_pop_with_lock(BenderChannel *channel, Bender *item, int is_choice) {
    BenderChannelPending writer;
    BENDER_CHAN_INFLIGHT_ACQUIRE(channel);
    if (channel->closed) {
        bender_chan_unlock(channel);
        BENDER_CHAN_INFLIGHT_DROP(channel);
        *item = bender_wrap_nil();
        return 1;
    }
    int is_threaded = bender_chan_is_threaded(channel);
    if (bender_q_pop(&channel->items, item, sizeof(Bender))) {
        /* Queue empty */
        if (is_choice == 2) {
            /* Try-mode: report empty without suspending (never leaves
             * the lock held — a leaked mutex would deadlock every
             * later operation on this channel). */
            bender_chan_unlock(channel);
            return 0; /* Skip pending read */
        }
        BenderChannelPending pending;
        pending.thread = &bender_vm;
        pending.fiber = bender_vm.root_fiber,
        pending.sched_id = bender_vm.root_fiber->sched_id;
        pending.mode = is_choice ? BENDER_CP_MODE_CHOICE_READ : BENDER_CP_MODE_READ;
        if (is_threaded) {
            bender_chan_remove_stale(&channel->read_pending);
        }
        pending.reg = bender_chanpend_register(pending.fiber, channel, 0);
        bender_q_push(&channel->read_pending, &pending, sizeof(pending));
        bender_chan_unlock(channel);
        if (is_threaded) {
            bender_gcroot(bender_wrap_fiber(pending.fiber));
        }
        BENDER_CHAN_INFLIGHT_DROP(channel);
        return 0;
    }
    bender_assert(!bender_chan_unpack(channel, item, 0), "bad channel packing");
    if (!bender_q_pop(&channel->write_pending, &writer, sizeof(writer))) {
        /* Pending writer. Same staleness discipline as the read side:
         * dead fibers are reclaimed by node (never dereferenced);
         * same-VM stale entries drop; threaded entries address foreign
         * VMs and are root-pinned while queued. */
        if (!is_threaded && bender_chanpend_claim_plain(&writer)) {
            if (writer.mode == BENDER_CP_MODE_CHOICE_WRITE) {
                bender_schedule(writer.fiber, make_write_result(channel));
            } else {
                bender_schedule(writer.fiber, bender_wrap_abstract(channel));
            }
        } else if (is_threaded) {
            bender_chanpend_claim_threaded(&writer);
            BenderVM *vm = writer.thread;
            if (vm == &bender_vm) {
                /* BUG 1 sibling: same-VM writer pending on a threaded
                 * channel -- schedule inline (see the push-side note). */
                uint32_t sid = (uint32_t) writer.sched_id;
                if (writer.fiber->sched_id == sid &&
                        bender_fiber_can_resume(writer.fiber)) {
                    bender_gcunroot(bender_wrap_fiber(writer.fiber));
                    if (writer.mode == BENDER_CP_MODE_CHOICE_WRITE) {
                        bender_schedule(writer.fiber,
                                make_write_result(channel));
                    } else {
                        bender_schedule(writer.fiber,
                                bender_wrap_channel(channel));
                    }
                }
            } else {
                BenderEVGenericMessage msg;
                msg.tag = writer.mode;
                msg.fiber = writer.fiber;
                msg.argi = (int32_t) writer.sched_id;
                msg.argp = channel;
                msg.argj = bender_wrap_nil();
                if (vm) {
                    bender_ev_post_event(vm, bender_thread_chan_cb, msg);
                }
            }
        }
    }
    bender_chan_unlock(channel);
    BENDER_CHAN_INFLIGHT_DROP(channel);
    return 1;
}

int bender_channel_pop(BenderChannel *channel, Bender *item, int is_choice) {
    /* Step 4.1 home-lane pin (see push). */
    if (bender_chan_foreign_lane(channel)) bender_chan_foreign_lane_panic(channel);
    bender_chan_lock(channel);
    return bender_channel_pop_with_lock(channel, item, is_choice);
}

BenderChannel *bender_channel_unwrap(void *abstract) {
    return abstract;
}

BenderChannel *bender_getchannel(const Bender *argv, int32_t n) {
    return bender_channel_unwrap(bender_getabstract(argv, n, &bender_channel_type));
}

BenderChannel *bender_optchannel(const Bender *argv, int32_t argc, int32_t n, BenderChannel *dflt) {
    if (argc > n && !bender_checktype(argv[n], BENDER_NIL)) {
        return bender_getchannel(argv, n);
    } else {
        return dflt;
    }
}

int bender_channel_give(BenderChannel *channel, Bender x) {
    return bender_channel_push(channel, x, 2);
}

int bender_channel_take(BenderChannel *channel, Bender *out) {
    return bender_channel_pop(channel, out, 2);
}

BenderChannel *bender_channel_make(uint32_t limit) {
    bender_assert(limit <= INT32_MAX, "bad limit");
    BenderChannel *channel = bender_abstract(&bender_channel_type, sizeof(BenderChannel));
    bender_chan_init(channel, (int32_t) limit, 0, 0);
    return channel;
}

BenderChannel *bender_channel_make_threaded(uint32_t limit) {
    bender_assert(limit <= INT32_MAX, "bad limit");
    BenderChannel *channel = bender_abstract_threaded(&bender_channel_type, sizeof(BenderChannel));
    bender_chan_init(channel, (int32_t) limit, 1, 0);
    return channel;
}

BenderChannel *bender_channel_make_isolated(uint32_t limit) {
    bender_assert(limit <= INT32_MAX, "bad limit");
    BenderChannel *channel = bender_abstract(&bender_channel_type, sizeof(BenderChannel));
    bender_chan_init(channel, (int32_t) limit, 0, 1);
    return channel;
}

/* Channel Methods */

BENDER_CORE_FN(cfun_channel_push,
              "(ev/give chan val)",
              "Write `val` to a channel `chan`, suspending the current "
              "fiber if `chan` is full. Returns `chan` on success, nil "
              "otherwise.") {
    bender_fixarity(argc, 2);
    BenderChannel *channel = bender_getchannel(argv, 0);
    if (bender_vm.coerce_error) {
        bender_panic("cannot give to channel inside bender_call");
    }
    if (bender_channel_push(channel, argv[1], 0)) {
        bender_await();
    }
    return argv[0];
}

BENDER_CORE_FN(cfun_channel_pop,
              "(ev/take chan)",
              "Read from a channel `chan`, suspending the current fiber if "
              "no value is available.") {
    bender_fixarity(argc, 1);
    BenderChannel *channel = bender_getchannel(argv, 0);
    Bender item;
    if (bender_vm.coerce_error) {
        bender_panic("cannot take from channel inside bender_call");
    }
    if (bender_channel_pop(channel, &item, 0)) {
        bender_schedule(bender_vm.root_fiber, item);
    }
    bender_await();
}

BENDER_CORE_FN(cfun_channel_try_push,
              "(ev/try-give chan val)",
              "Non-blocking channel write. Returns `chan` if the value was "
              "accepted, nil if the channel is full (no suspension). "
              "Panics on a closed channel, like `ev/give`. Safe inside "
              "`bender_call` (never suspends).") {
    bender_fixarity(argc, 2);
    BenderChannel *channel = bender_getchannel(argv, 0);
    if (bender_channel_push(channel, argv[1], 3)) {
        return bender_wrap_nil();
    }
    return argv[0];
}

BENDER_CORE_FN(cfun_channel_try_pop,
              "(ev/try-take chan)",
              "Non-blocking channel read. Returns the value if one is "
              "immediately available, nil if the channel is empty (no "
              "suspension). A closed channel yields nil, like `ev/take`. "
              "Safe inside `bender_call` (never suspends).") {
    bender_fixarity(argc, 1);
    BenderChannel *channel = bender_getchannel(argv, 0);
    Bender item = bender_wrap_nil();
    if (bender_channel_pop(channel, &item, 2)) {
        return item;
    }
    return bender_wrap_nil();
}

BENDER_CORE_FN(cfun_channel_choice,
              "(ev/select & clauses)",
              "Block until the first of several channel operations occur. Returns a "
              "tuple of the form [:give chan], [:take chan x], or [:close chan], "
              "where a :give tuple is the result of a write and a :take tuple is the "
              "result of a read. Each clause must be either a channel (for a channel "
              "take operation) or a tuple [channel x] (for a channel give operation). "
              "Operations are tried in order such that earlier clauses take "
              "precedence over later clauses. Both give and take operations can "
              "return a [:close chan] tuple, which indicates that the specified "
              "channel was closed while waiting, or that the channel was already "
              "closed.") {
    bender_arity(argc, 1, -1);
    int32_t len;
    const Bender *data;

    if (bender_vm.coerce_error) {
        bender_panic("cannot select from channel inside bender_call");
    }

    /* Check channels for immediate reads and writes. Avoid awating to event loop
     * if possible, but should be very similar to async block of code. */
    for (int32_t i = 0; i < argc; i++) {
        if (bender_indexed_view(argv[i], &data, &len) && len == 2) {
            /* Write */
            BenderChannel *chan = bender_getchannel(data, 0);
            bender_chan_lock(chan);
            if (chan->closed) {
                bender_chan_unlock(chan);
                return make_close_result(chan);
            }
            if (bender_q_count(&chan->items) < chan->limit) {
                bender_channel_push_with_lock(chan, data[1], 1);
                return make_write_result(chan);
            }
            bender_chan_unlock(chan);
        } else {
            /* Read */
            BenderChannel *chan = bender_getchannel(argv, i);
            bender_chan_lock(chan);
            if (chan->closed) {
                bender_chan_unlock(chan);
                return make_close_result(chan);
            }
            if (chan->items.head != chan->items.tail) {
                Bender item;
                bender_channel_pop_with_lock(chan, &item, 1);
                return make_read_result(chan, item);
            }
            bender_chan_unlock(chan);
        }
    }

    /* We cannot assume threaded channels are unchanged here, as other threads can mutate them.
     * Namely, they may been closed or have items added or removed during this call. Simple channels
     * could be slightly simpler and skip the extra closed checks. */

    /* Wait for all readers or writers */
    for (int32_t i = 0; i < argc; i++) {
        if (bender_indexed_view(argv[i], &data, &len) && len == 2) {
            /* Write */
            BenderChannel *chan = bender_getchannel(data, 0);
            bender_chan_lock(chan);
            if (chan->closed) {
                bender_chan_unlock(chan);
                bender_schedule(bender_vm.root_fiber, make_close_result(chan));
                break;
            }
            if (!bender_channel_push_with_lock(chan, data[1], 1)) {
                bender_schedule(bender_vm.root_fiber, make_write_result(chan));
                break;
            }
        } else {
            /* Read */
            Bender item;
            BenderChannel *chan = bender_getchannel(argv, i);
            bender_chan_lock(chan);
            if (chan->closed) {
                bender_chan_unlock(chan);
                bender_schedule(bender_vm.root_fiber, make_close_result(chan));
                break;
            }
            if (bender_channel_pop_with_lock(chan, &item, 1)) {
                bender_schedule(bender_vm.root_fiber, make_read_result(chan, item));
                break;
            }
        }
    }

    bender_await();
}

BENDER_CORE_FN(cfun_channel_full,
              "(ev/full chan)",
              "Returns true if a channel `chan` is full, false otherwise.") {
    bender_fixarity(argc, 1);
    BenderChannel *channel = bender_getchannel(argv, 0);
    bender_chan_lock(channel);
    Bender ret = bender_wrap_boolean(bender_q_count(&channel->items) >= channel->limit);
    bender_chan_unlock(channel);
    return ret;
}

BENDER_CORE_FN(cfun_channel_capacity,
              "(ev/capacity chan)",
              "Get number of items a channel `chan` can store before "
              "blocking writers.") {
    bender_fixarity(argc, 1);
    BenderChannel *channel = bender_getchannel(argv, 0);
    bender_chan_lock(channel);
    Bender ret = bender_wrap_integer(channel->limit);
    bender_chan_unlock(channel);
    return ret;
}

BENDER_CORE_FN(cfun_channel_count,
              "(ev/count chan)",
              "Get number of items waiting in a channel `chan`.") {
    bender_fixarity(argc, 1);
    BenderChannel *channel = bender_getchannel(argv, 0);
    bender_chan_lock(channel);
    Bender ret = bender_wrap_integer(bender_q_count(&channel->items));
    bender_chan_unlock(channel);
    return ret;
}

/* Fisher yates shuffle of arguments to get fairness */
static void fisher_yates_args(int32_t argc, Bender *argv) {
    for (int32_t i = argc; i > 1; i--) {
        int32_t swap_index = bender_rng_u32(&bender_vm.ev_rng) % i;
        Bender temp = argv[swap_index];
        argv[swap_index] = argv[i - 1];
        argv[i - 1] = temp;
    }
}

BENDER_CORE_FN(cfun_channel_rchoice,
              "(ev/rselect & clauses)",
              "Similar to ev/select, but will try clauses in a random order for fairness.") {
    fisher_yates_args(argc, argv);
    return cfun_channel_choice(argc, argv);
}

BENDER_CORE_FN(cfun_channel_new,
              "(ev/chan &opt capacity)",
              "Create a new channel. capacity is the number of values to queue before "
              "blocking writers, defaults to 0 if not provided. Returns a new channel.") {
    bender_arity(argc, 0, 1);
    int32_t limit = bender_optnat(argv, argc, 0, 0);
    BenderChannel *channel = bender_abstract(&bender_channel_type, sizeof(BenderChannel));
    bender_chan_init(channel, limit, 0, 0);
    return bender_wrap_abstract(channel);
}

BenderChannel *bender_channel_make_threaded_c(int32_t limit) {
    BenderChannel *tchan = bender_abstract_threaded(&bender_channel_type,
            sizeof(BenderChannel));
    bender_chan_init(tchan, limit, 1, 0);
    return tchan;
}

BENDER_CORE_FN(cfun_channel_new_threaded,
              "(ev/thread-chan &opt limit)",
              "Create a threaded channel. A threaded channel is a channel that can be shared between threads and "
              "used to communicate between any number of operating system threads.") {
    bender_arity(argc, 0, 1);
    int32_t limit = bender_optnat(argv, argc, 0, 0);
    BenderChannel *tchan = bender_abstract_threaded(&bender_channel_type, sizeof(BenderChannel));
    bender_chan_init(tchan, limit, 1, 0);
    return bender_wrap_abstract(tchan);
}

BENDER_CORE_FN(cfun_channel_new_isolated,
              "(ev/ichan &opt limit)",
              "Create an isolated channel. Values sent through it are "
              "marshal-copied like a threaded channel (no shared mutable "
              "references cross), but it takes no lock: single-VM use "
              "only, the tasklet mailbox transport. Cross-thread use is "
              "user error (use ev/thread-chan across threads).") {
    bender_arity(argc, 0, 1);
    int32_t limit = bender_optnat(argv, argc, 0, 0);
    BenderChannel *ichan = bender_abstract(&bender_channel_type, sizeof(BenderChannel));
    bender_chan_init(ichan, limit, 0, 1);
    return bender_wrap_abstract(ichan);
}

BENDER_CORE_FN(cfun_channel_close,
              "(ev/chan-close chan)",
              "Close a channel `chan`. Once closed, causes all pending "
              "reads and writes to return nil. Returns `chan`.") {
    bender_fixarity(argc, 1);
    BenderChannel *channel = bender_getchannel(argv, 0);
    bender_chan_lock(channel);
    if (!channel->closed) {
        channel->closed = 1;
        BenderChannelPending writer;
        while (!bender_q_pop(&channel->write_pending, &writer, sizeof(writer))) {
            if (writer.thread != &bender_vm) {
                bender_chanpend_claim_threaded(&writer);
                BenderVM *vm = writer.thread;
                BenderEVGenericMessage msg;
                msg.fiber = writer.fiber;
                msg.argp = channel;
                msg.tag = BENDER_CP_MODE_CLOSE;
                msg.argi = (int32_t) writer.sched_id;
                msg.argj = bender_wrap_nil();
                if (vm) {
                    bender_ev_post_event(vm, bender_thread_chan_cb, msg);
                }
            } else if (bender_chan_is_threaded(channel)) {
                /* Same-VM entry riding a threaded channel: root was
                 * held by this queue entry, release it now. */
                bender_chanpend_claim_threaded(&writer);
                if (bender_fiber_can_resume(writer.fiber) && writer.sched_id == writer.fiber->sched_id) {
                    if (writer.mode == BENDER_CP_MODE_CHOICE_WRITE) {
                        bender_schedule(writer.fiber, make_close_result(channel));
                    } else {
                        bender_schedule(writer.fiber, bender_wrap_nil());
                    }
                }
                bender_gcunroot(bender_wrap_fiber(writer.fiber));
            } else {
                /* Plain/isolated: same-VM, unrooted entries. */
                int live = bender_chanpend_claim_plain(&writer);
                if (live && bender_fiber_can_resume(writer.fiber) && writer.sched_id == writer.fiber->sched_id) {
                    if (writer.mode == BENDER_CP_MODE_CHOICE_WRITE) {
                        bender_schedule(writer.fiber, make_close_result(channel));
                    } else {
                        bender_schedule(writer.fiber, bender_wrap_nil());
                    }
                }
            }
        }
        BenderChannelPending reader;
        while (!bender_q_pop(&channel->read_pending, &reader, sizeof(reader))) {
            if (reader.thread != &bender_vm) {
                bender_chanpend_claim_threaded(&reader);
                BenderVM *vm = reader.thread;
                BenderEVGenericMessage msg;
                msg.fiber = reader.fiber;
                msg.argp = channel;
                msg.tag = BENDER_CP_MODE_CLOSE;
                msg.argi = (int32_t) reader.sched_id;
                msg.argj = bender_wrap_nil();
                if (vm) {
                    bender_ev_post_event(vm, bender_thread_chan_cb, msg);
                }
            } else if (bender_chan_is_threaded(channel)) {
                /* Same-VM entry riding a threaded channel: root was
                 * held by this queue entry, release it now. */
                bender_chanpend_claim_threaded(&reader);
                if (bender_fiber_can_resume(reader.fiber) && reader.sched_id == reader.fiber->sched_id) {
                    if (reader.mode == BENDER_CP_MODE_CHOICE_READ) {
                        bender_schedule(reader.fiber, make_close_result(channel));
                    } else {
                        bender_schedule(reader.fiber, bender_wrap_nil());
                    }
                }
                bender_gcunroot(bender_wrap_fiber(reader.fiber));
            } else {
                /* Plain/isolated: same-VM, unrooted entries. */
                int live = bender_chanpend_claim_plain(&reader);
                if (live && bender_fiber_can_resume(reader.fiber) && reader.sched_id == reader.fiber->sched_id) {
                    if (reader.mode == BENDER_CP_MODE_CHOICE_READ) {
                        bender_schedule(reader.fiber, make_close_result(channel));
                    } else {
                        bender_schedule(reader.fiber, bender_wrap_nil());
                    }
                }
            }
        }
    }
    bender_chan_unlock(channel);
    return argv[0];
}

static const BenderMethod ev_chanat_methods[] = {
    {"select", cfun_channel_choice},
    {"rselect", cfun_channel_rchoice},
    {"count", cfun_channel_count},
    {"take", cfun_channel_pop},
    {"give", cfun_channel_push},
    {"try-take", cfun_channel_try_pop},
    {"try-give", cfun_channel_try_push},
    {"capacity", cfun_channel_capacity},
    {"full", cfun_channel_full},
    {"close", cfun_channel_close},
    {NULL, NULL}
};

static int bender_chanat_get(void *p, Bender key, Bender *out) {
    (void) p;
    if (!bender_checktype(key, BENDER_KEYWORD)) return 0;
    return bender_getmethod(bender_unwrap_keyword(key), ev_chanat_methods, out);
}

static Bender bender_chanat_next(void *p, Bender key) {
    (void) p;
    return bender_nextmethod(ev_chanat_methods, key);
}

static void bender_chanat_marshal(void *p, BenderMarshalContext *ctx) {
    BenderChannel *channel = (BenderChannel *)p;
    /* Step 1.4: flags byte (bit0 threaded, bit1 isolated). Old dumps
     * wrote 0/1, which read back identically (bit1 clear). */
    uint8_t flags = (uint8_t)
        ((channel->is_threaded ? 1 : 0) | (channel->isolated ? 2 : 0));
    bender_marshal_byte(ctx, flags);
    bender_marshal_abstract(ctx, channel);
    bender_marshal_byte(ctx, channel->closed);
    bender_marshal_int(ctx, channel->limit);
    int32_t count = bender_q_count(&channel->items);
    bender_marshal_int(ctx, count);
    BenderQueue *items = &channel->items;
    Bender *data = channel->items.data;
    if (items->head <= items->tail) {
        for (int32_t i = items->head; i < items->tail; i++)
            bender_marshal_bender(ctx, data[i]);
    } else {
        for (int32_t i = items->head; i < items->capacity; i++)
            bender_marshal_bender(ctx, data[i]);
        for (int32_t i = 0; i < items->tail; i++)
            bender_marshal_bender(ctx, data[i]);
    }
}

static void *bender_chanat_unmarshal(BenderMarshalContext *ctx) {
    uint8_t flags = bender_unmarshal_byte(ctx);
    /* Old dumps carry 0/1 (threaded bit only); bit1 clear preserves them. */
    int is_threaded = (flags & 1) != 0;
    int isolated = (flags & 2) != 0;
    BenderChannel *abst;
    if (is_threaded) {
        abst = bender_unmarshal_abstract_threaded(ctx, sizeof(BenderChannel));
    } else {
        abst = bender_unmarshal_abstract(ctx, sizeof(BenderChannel));
    }
    uint8_t is_closed = bender_unmarshal_byte(ctx);
    int32_t limit = bender_unmarshal_int(ctx);
    int32_t count = bender_unmarshal_int(ctx);
    if (count < 0) bender_panic("invalid negative channel count");
    if (count > limit) bender_panic("invalid channel count");
    bender_chan_init(abst, limit, is_threaded, isolated);
    /* Detached only when this unmarshal happens ON A WORKER LANE. Every
     * proc/spawn onto a lane sends the closure through a threaded control
     * channel, so the worker receives marshal copies of captured values. A
     * plain (unlocked, uncopied) channel copy in that position is a trap:
     * writes land in the copy and the real owner never sees them.
     *
     * A same-thread marshal round-trip on the MAIN VM is a supported pattern
     * (suite-ev marshals a channel and drains it), so it must stay legal --
     * hence the is_lane_vm test rather than flagging every unmarshal. */
    abst->detached_copy = bender_vm.is_lane_vm ? 1 : 0;
    abst->closed = !!is_closed;
    int needs_pack = (is_threaded || isolated);
    for (int32_t i = 0; i < count; i++) {
        Bender item = bender_unmarshal_bender(ctx);
        bender_assert(!bender_q_push(&abst->items, &item, sizeof(item)), "bad unmarshal channel");
        /* Packed queues hold malloc'd transit buffers (see pack), but
         * unmarshal rebuilds them as GC buffers — which unpack would
         * bender_free (invalid free). Move the bytes into malloc'd
         * ownership (encoding untouched); the GC copy becomes garbage.
         * Pack is bypassed deliberately: items are already transit. */
        if (needs_pack && bender_checktype(item, BENDER_BUFFER)) {
            BenderBuffer *src = bender_unwrap_buffer(item);
            BenderBuffer *nb = bender_malloc(sizeof(BenderBuffer));
            if (NULL == nb) BENDER_OUT_OF_MEMORY;
            bender_buffer_init(nb, 0);
            if (src->count > 0) {
                bender_buffer_push_bytes(nb, src->data, src->count);
            }
            BenderQueue *q = &abst->items;
            Bender *slots = q->data;
            int32_t tail = q->tail - 1;
            if (tail < 0) tail += q->capacity;
            slots[tail] = bender_wrap_buffer(nb);
        }
    }
    return abst;
}

const BenderAbstractType bender_channel_type = {
    "core/channel",
    bender_chanat_gc,
    bender_chanat_mark,
    bender_chanat_get,
    NULL, /* put */
    bender_chanat_marshal,
    bender_chanat_unmarshal,
    NULL, /* tostring */
    NULL, /* compare */
    NULL, /* hash */
    bender_chanat_next,
    NULL, /* call */
    NULL, /* length */
    NULL, /* bytes */
    bender_chanat_gcperthread
};

/* Main event loop */

void bender_loop1_impl(int has_timeout, BenderTimestamp timeout);

int bender_loop_done(void) {
    return !((bender_vm.spawn.head != bender_vm.spawn.tail) ||
             bender_vm.tq_count ||
             bender_atomic_load(&bender_vm.listener_count));
}

/* Service every deadline that has come due. A deadline either cancels the
 * waiting task fiber (operation timeouts, ev/deadline) or wakes it with nil.
 * Shared by the blocking loop and by ev/pump's non-blocking sample so both
 * deliver identical semantics. */
static void bender_expire_timeouts(void) {
    BenderTimeout to;
    BenderTimestamp now = ts_now();
    while (peek_timeout(&to) && to.when <= now) {
        pop_timeout(0);
        if (to.curr_fiber != NULL) {
            if (bender_fiber_can_resume(to.curr_fiber)) {
                bender_cancel(to.fiber, bender_cstringv("deadline expired"));
            }
        } else if (to.fiber->sched_id == to.sched_id) {
            /* A timeout for a suspended operation, not a whole fiber. */
            if (to.is_error) {
                bender_cancel(to.fiber, bender_cstringv("timeout"));
            } else {
                bender_schedule(to.fiber, bender_wrap_nil());
            }
        }
        handle_timeout_worker(to, 0);
    }
}

/* Sample expired timers and ready I/O without blocking or running tasks.
 * Used by ev/pump, which must never re-enter the task scheduler from inside a
 * live fiber, and by bulk cross-lane callers (proc/spawn) that must drain the
 * self-pipe while they work. */
void bender_loop1_poll_now(void);

BenderFiber *bender_loop1(void) {
    bender_expire_timeouts();

    /* Run scheduled fibers unless interrupts need to be handled. */
    while (bender_vm.spawn.head != bender_vm.spawn.tail) {
        /* Don't run until all interrupts have been marked as handled by calling bender_interpreter_interrupt_handled */
        if (bender_atomic_load_relaxed(&bender_vm.auto_suspend)) break;
        BenderTask task = {NULL, bender_wrap_nil(), BENDER_SIGNAL_OK, 0};
        bender_q_pop(&bender_vm.spawn, &task, sizeof(task));
        if (task.fiber->gc.flags & BENDER_FIBER_EV_GCFLAG_SUSPENDED) bender_ev_dec_refcount();
        task.fiber->gc.flags &= ~(BENDER_FIBER_EV_GCFLAG_CANCELED | BENDER_FIBER_EV_GCFLAG_SUSPENDED);
        if (task.expected_sched_id != task.fiber->sched_id) continue;
        Bender res;
        /* M4b-2 quantum driver: budget is granted per resume inside
         * bender_continue_no_check (covers loop-scheduled and direct
         * runs alike), so every run below starts full. Off = existing
         * behavior exact. */
        BenderSignal sig = bender_continue_signal(task.fiber, task.value, &res, task.sig);
        /* M4b quantum yield: the fiber WILL be re-queued below (same
         * signal, budget exhausted). It must bypass the completion
         * cleanup -- the minor frees the fiber's young nursery, and a
         * re-queued fiber would resume onto freed data (the density
         * UAF: status USER9, data poisoned, walked by later marks). */
        int quantum_yield = (sig == BENDER_SIGNAL_INTERRUPT) &&
            bender_vm.quantum && !bender_atomic_load_relaxed(&bender_vm.auto_suspend);
        if (!bender_fiber_can_resume(task.fiber) && !quantum_yield) {
            bender_table_remove(&bender_vm.active_tasks, bender_wrap_fiber(task.fiber));
            /* P3.5c: a completed proc retires its mailbox (go quiet so
             * later sends drop silently, Erlang parity). Completed
             * fibers hold no self-pendings (recv is own-mailbox only),
             * so no purge is needed — only the flag. All objects are
             * listed-valid here (mid-mutator, pre-sweep). */
            if (NULL != task.fiber->proc && NULL != task.fiber->proc->mbox) {
                bender_channel_set_proc_alive(task.fiber->proc->mbox, 0);
            }
            /* Phase 1 density: one final own-heap minor at completion.
             * The global minor no longer walks fibers' lists (the 1M
             * quadratic), so a dead fiber's young garbage frees HERE —
             * O(own), at the completion boundary where the stack is
             * final and consistent. Parked-forever fibers are the only
             * deferred case (bounded by their real working set). */
            bender_collect_fiber_minor(task.fiber);
        }
        void *sv = task.fiber->supervisor_channel;
        int is_suspended = sig == BENDER_SIGNAL_EVENT || sig == BENDER_SIGNAL_YIELD || sig == BENDER_SIGNAL_INTERRUPT;
        if (is_suspended) {
            task.fiber->gc.flags |= BENDER_FIBER_EV_GCFLAG_SUSPENDED;
            bender_ev_inc_refcount();
        }
        if (sig == BENDER_SIGNAL_INTERRUPT && bender_vm.quantum &&
                !bender_atomic_load_relaxed(&bender_vm.auto_suspend)) {
            /* Budget exhausted (not external quiesce — that leaves
             * auto_suspend set): re-queue at the back with the original
             * wake value/signal (transparent deferral — takes/sleeps
             * already materialized before any poll could fire) and BREAK
             * to event polling (not continue: a re-queued hog would
             * otherwise starve timers/channel-polling forever — the
             * spawn queue never drains). No supervisor event: nothing
             * completed. Resume value ignored via RESUME_NO_USEVAL. */
            /* Re-queue transparently. The wake signal has ALREADY been
             * consumed by the resume above: re-arming it would replay a
             * BENDER_SIGNAL_ERROR into the fiber's next resume, and once
             * bender_continue_signal has smuggled that error into the status
             * bits it is re-raised on every later turn. Under quanta that
             * turned one expired deadline into a permanently replaying
             * "timeout" that surfaced in unrelated arithmetic (and, for
             * BENDER_SIGNAL_OK, could overwrite a real wake value). Resume
             * with a plain OK and no value. */
            bender_schedule_signal(task.fiber, bender_wrap_nil(), BENDER_SIGNAL_OK);
            break;
        }
        if (NULL == sv) {
            if (!is_suspended) {
                bender_stacktrace_ext(task.fiber, res, "");
            }
        } else if (sig == BENDER_SIGNAL_OK || (task.fiber->flags & (1 << sig))) {
            BenderChannel *chan = bender_channel_unwrap(sv);
            bender_channel_push(chan, make_supervisor_event(bender_signal_names[sig],
                    task.fiber, chan->is_threaded), 2);
        } else if (!is_suspended) {
            bender_stacktrace_ext(task.fiber, res, "");
        }
        if (sig == BENDER_SIGNAL_INTERRUPT) {
            return task.fiber;
        }
    }

    /* Poll for events. A non-blocking turn still samples ready I/O, but with
     * a zero deadline so the OS can never delay the caller. */
    if (bender_vm.tq_count || bender_atomic_load(&bender_vm.listener_count)) {
        BenderTimeout to;
        memset(&to, 0, sizeof(to));
        int has_timeout;
        /* Drop timeouts that are no longer needed */
        while ((has_timeout = peek_timeout(&to))) {
            if (to.curr_fiber != NULL) {
                if (!bender_fiber_can_resume(to.curr_fiber)) {
                    pop_timeout(0);
                    bender_table_remove(&bender_vm.active_tasks, bender_wrap_fiber(to.curr_fiber));
                    handle_timeout_worker(to, 1);
                    continue;
                }
            } else if (to.fiber->sched_id != to.sched_id) {
                pop_timeout(0);
                handle_timeout_worker(to, 1);
                continue;
            }
            break;
        }
        /* Run polling implementation only if pending timeouts or pending events.
         * M4b-2: under quanta with runnable tasks queued (and no external
         * quiesce pending), never block here — a re-queued quantum would
         * otherwise stall till the next deadline despite ready work.
         * Expired timers above were already serviced without blocking,
         * and the loop re-enters spawn immediately. Quantum off, or
         * external quiesce active: identical to before. */
        if ((bender_vm.tq_count || bender_atomic_load(&bender_vm.listener_count)) &&
                !(bender_vm.quantum &&
                  !bender_atomic_load_relaxed(&bender_vm.auto_suspend) &&
                  bender_vm.spawn.head != bender_vm.spawn.tail)) {
            bender_loop1_impl(has_timeout, to.when);
        }
    }

    /* No fiber was interrupted */
    return NULL;
}

/* Expire due deadlines and sample ready I/O once, with a zero deadline so the
 * OS can never delay the caller. Task fibers are deliberately NOT run: resuming
 * them swaps bender_vm.fiber and the C stack, which is only safe from the top of
 * the scheduler loop. */
void bender_loop1_poll_now(void) {
    bender_expire_timeouts();
    /* Always poll with a zero deadline. The previous gate
     * (tq_count || listener_count) skipped the poller whenever no timer or
     * listener existed -- but a PENDING SELF-PIPE EVENT is exactly such a
     * case: cross-lane posts incref listener_count at post time and decref at
     * drain, so the count can sit at zero while events still wait, and the
     * pipe then fills and the poster aborts. A zero-deadline poll is one
     * syscall and cannot sleep, so calling it unconditionally is safe. */
    bender_loop1_impl(1, ts_now());
}

/* Same as bender_interpreter_interrupt, but will also
 * break out of the event loop if waiting for an event
 * (say, waiting for ev/sleep to finish). Does this by pushing
 * an empty event to the event loop. */
void bender_loop1_interrupt(BenderVM *vm) {
    bender_interpreter_interrupt(vm);
    BenderEVGenericMessage msg = {0};
    BenderCallback cb = NULL;
    bender_ev_post_event(vm, cb, msg);
}

void bender_loop(void) {
    while (!bender_loop_done()) {
        BenderFiber *interrupted_fiber = bender_loop1();
        if (NULL != interrupted_fiber) {
            bender_schedule(interrupted_fiber, bender_wrap_nil());
        }
    }
}

/*
 * Self-pipe handling code.
 */

#ifdef BENDER_WINDOWS

/* On windows, use PostQueuedCompletionStatus instead for
 * custom events */

#else

static void bender_ev_setup_selfpipe(void) {
    /* Both ends must be nonblocking: cross-VM posts hold the home-lifetime
     * lock until the write finishes, and a full pipe must not pin shutdown. */
    if (bender_make_pipe(bender_vm.selfpipe, 0)) {
        BENDER_EXIT("failed to initialize self pipe in event loop");
    }
#if defined(__linux__) && defined(F_SETPIPE_SZ)
    /* Phase 2b step 4 (backpressure, not a correctness fix): the default
     * pipe capacity (typically 64KiB) divided by sizeof(BenderSelfPipeEvent)
     * bounds how many concurrent posts (signal handlers, threaded-call
     * completions, pool/proc exits) can be in flight before
     * bender_ev_post_event's bounded retry loop aborts via "failed to
     * write event to self-pipe". That abort is the deliberate, signal-
     * safe fallback (test-ev-selfpipe-saturation asserts it is bounded,
     * not that it never happens) and must stay exactly as-is: a signal
     * handler cannot block, lock, or allocate, so no real backpressure
     * is possible from that context. Growing the buffer only widens the
     * margin before that fallback is needed; it changes no control flow
     * and is a best-effort Linux-only call (unprivileged processes are
     * typically capped around 1MiB by pipe-max-size, so failure here is
     * expected and harmless -- fall back to the kernel default). */
    fcntl(bender_vm.selfpipe[1], F_SETPIPE_SZ, 1 << 20);
#endif
}

/* Handle events from the self pipe inside the event loop */
static void bender_ev_handle_selfpipe(void) {
    BenderSelfPipeEvent response;
    int status;
recur:
    do {
        status = read(bender_vm.selfpipe[0], &response, sizeof(response));
    } while (status == -1 && errno == EINTR);
    if (status > 0) {
        if (NULL != response.cb) {
            response.cb(response.msg);
        }
        bender_ev_dec_refcount();
        goto recur;
    }
}

static void bender_ev_cleanup_selfpipe(void) {
    close(bender_vm.selfpipe[0]);
    close(bender_vm.selfpipe[1]);
}

#endif

#ifdef BENDER_WINDOWS

static BenderTimestamp ts_now(void) {
    return (BenderTimestamp) GetTickCount64();
}

void bender_ev_init(void) {
    bender_ev_init_common();
    bender_vm.iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
    if (NULL == bender_vm.iocp) bender_panic("could not create io completion port");
}

void bender_ev_deinit(void) {
    bender_ev_deinit_common();
    CloseHandle(bender_vm.iocp);
}

static void bender_register_stream(BenderStream *stream) {
    if (NULL == CreateIoCompletionPort(stream->handle, bender_vm.iocp, (ULONG_PTR) stream, 0)) {
        if (stream->flags & (BENDER_STREAM_READABLE | BENDER_STREAM_WRITABLE | BENDER_STREAM_ACCEPTABLE)) {
            bender_panicf("failed to listen for events: %V", bender_ev_lasterr());
        }
        stream->flags |= BENDER_STREAM_UNREGISTERED;
    }
}

void bender_loop1_impl(int has_timeout, BenderTimestamp to) {
    ULONG_PTR completionKey = 0;
    DWORD num_bytes_transferred = 0;
    LPOVERLAPPED overlapped = NULL;

    /* Calculate how long to wait before timeout */
    uint64_t waittime;
    if (has_timeout) {
        BenderTimestamp now = ts_now();
        if (now > to) {
            waittime = 0;
        } else {
            waittime = (uint64_t)(to - now);
        }
    } else {
        waittime = INFINITE;
    }
    BOOL result = GetQueuedCompletionStatus(bender_vm.iocp, &num_bytes_transferred, &completionKey, &overlapped, (DWORD) waittime);

    if (result || overlapped) {
        if (0 == completionKey) {
            /* Custom event */
            BenderSelfPipeEvent *response = (BenderSelfPipeEvent *)(overlapped);
            if (NULL != response->cb) {
                response->cb(response->msg);
            }
            bender_ev_dec_refcount();
            bender_free(response);
        } else {
            /* Normal event */
            BenderOverlapped *jo = (BenderOverlapped *) overlapped;
            BenderStream *stream = (BenderStream *) completionKey;
            BenderFiber *fiber = NULL;
            if (stream->read_fiber && stream->read_fiber->ev_state == jo) {
                fiber = stream->read_fiber;
            } else if (stream->write_fiber && stream->write_fiber->ev_state == jo) {
                fiber = stream->write_fiber;
            }
            if (fiber != NULL) {
                fiber->flags &= ~BENDER_FIBER_EV_FLAG_IN_FLIGHT;
                jo->bytes_transfered = (ULONG_PTR) num_bytes_transferred;
                fiber->ev_callback(fiber, result ? BENDER_ASYNC_EVENT_COMPLETE : BENDER_ASYNC_EVENT_FAILED);
            } else {
                bender_free((void *) jo);
                bender_ev_dec_refcount();
            }
            bender_stream_checktoclose(stream);
        }
    }
}

void bender_stream_edge_triggered(BenderStream *stream) {
    (void) stream;
}

void bender_stream_level_triggered(BenderStream *stream) {
    (void) stream;
}

#elif defined(BENDER_EV_EPOLL)

static BenderTimestamp ts_now(void) {
    struct timespec now;
    bender_assert(-1 != bender_gettime(&now, BENDER_TIME_MONOTONIC), "failed to get time");
    uint64_t res = 1000 * now.tv_sec;
    res += now.tv_nsec / 1000000;
    return res;
}

/* Wait for the next event */
static void bender_register_stream_impl(BenderStream *stream, int mod, int edge_trigger) {
    struct epoll_event ev;
    ev.events = edge_trigger ? EPOLLET : 0;
    if (stream->flags & (BENDER_STREAM_READABLE | BENDER_STREAM_ACCEPTABLE)) ev.events |= EPOLLIN;
    if (stream->flags & BENDER_STREAM_WRITABLE) ev.events |= EPOLLOUT;
    ev.data.ptr = stream;
    int status;
    do {
        status = epoll_ctl(bender_vm.epoll, mod ? EPOLL_CTL_MOD : EPOLL_CTL_ADD, stream->handle, &ev);
    } while (status == -1 && errno == EINTR);
    if (status == -1) {
        if (errno == EPERM) {
            /* Couldn't add to event loop, so assume that it completes
             * synchronously. */
            stream->flags |= BENDER_STREAM_UNREGISTERED;
        } else {
            /* Unexpected error */
            bender_panicv(bender_ev_lasterr());
        }
    }
}

static void bender_register_stream(BenderStream *stream) {
    bender_register_stream_impl(stream, 0, 1);
}

void bender_stream_edge_triggered(BenderStream *stream) {
    bender_register_stream_impl(stream, 1, 1);
}

void bender_stream_level_triggered(BenderStream *stream) {
    bender_register_stream_impl(stream, 1, 0);
}

void bender_unregister_stream(BenderStream *stream) {
    if (stream->flags & BENDER_STREAM_NODUPS) return;
    int status;
    do {
        status = epoll_ctl(bender_vm.epoll, EPOLL_CTL_DEL, stream->handle, NULL);
    } while (status == -1 && errno == EINTR);
    if (status == -1) {
        bender_panicv(bender_ev_lasterr());
    }
    stream->flags |= BENDER_STREAM_UNREGISTERED;
}

#define BENDER_EPOLL_MAX_EVENTS 64
void bender_loop1_impl(int has_timeout, BenderTimestamp timeout) {
    struct itimerspec its;
    if (bender_vm.timer_enabled || has_timeout) {
        memset(&its, 0, sizeof(its));
        if (has_timeout) {
            its.it_value.tv_sec = timeout / 1000;
            its.it_value.tv_nsec = (timeout % 1000) * 1000000;
        }
        timerfd_settime(bender_vm.timerfd, TFD_TIMER_ABSTIME, &its, NULL);
    }
    bender_vm.timer_enabled = has_timeout;

    /* Poll for events */
    struct epoll_event events[BENDER_EPOLL_MAX_EVENTS];
    int ready;
    do {
        ready = epoll_wait(bender_vm.epoll, events, BENDER_EPOLL_MAX_EVENTS, -1);
    } while (ready == -1 && errno == EINTR);
    if (ready == -1) {
        BENDER_EXIT("failed to poll events");
    }

    /* Step state machines */
    for (int i = 0; i < ready; i++) {
        void *p = events[i].data.ptr;
        if (&bender_vm.timerfd == p) {
            /* Timer expired, ignore */;
        } else if (bender_vm.selfpipe == p) {
            /* Self-pipe handling */
            bender_ev_handle_selfpipe();
        } else {
            BenderStream *stream = p;
            int mask = events[i].events;
            int has_err = mask & EPOLLERR;
            int has_hup = mask & EPOLLHUP;
            BenderFiber *rf = stream->read_fiber;
            BenderFiber *wf = stream->write_fiber;
            if (rf) {
                if (rf->ev_callback && (mask & EPOLLIN)) {
                    rf->ev_callback(rf, BENDER_ASYNC_EVENT_READ);
                }
                if (rf->ev_callback && has_err) {
                    rf->ev_callback(rf, BENDER_ASYNC_EVENT_ERR);
                }
                if (rf->ev_callback && has_hup) {
                    rf->ev_callback(rf, BENDER_ASYNC_EVENT_HUP);
                }
            }
            if (wf) {
                if (wf->ev_callback && (mask & EPOLLOUT)) {
                    wf->ev_callback(wf, BENDER_ASYNC_EVENT_WRITE);
                }
                if (wf->ev_callback && has_err) {
                    wf->ev_callback(wf, BENDER_ASYNC_EVENT_ERR);
                }
                if (wf->ev_callback && has_hup) {
                    wf->ev_callback(wf, BENDER_ASYNC_EVENT_HUP);
                }
            }
            bender_stream_checktoclose(stream);
        }
    }
}

void bender_ev_init(void) {
    bender_ev_init_common();
    bender_ev_setup_selfpipe();
    bender_vm.epoll = epoll_create1(EPOLL_CLOEXEC);
    bender_vm.timerfd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
    bender_vm.timer_enabled = 0;
    if (bender_vm.epoll == -1 || bender_vm.timerfd == -1) goto error;
    struct epoll_event ev;
    ev.events = EPOLLIN | EPOLLET;
    ev.data.ptr = &bender_vm.timerfd;
    if (-1 == epoll_ctl(bender_vm.epoll, EPOLL_CTL_ADD, bender_vm.timerfd, &ev)) goto error;
    ev.events = EPOLLIN | EPOLLET;
    ev.data.ptr = bender_vm.selfpipe;
    if (-1 == epoll_ctl(bender_vm.epoll, EPOLL_CTL_ADD, bender_vm.selfpipe[0], &ev)) goto error;
    return;
error:
    BENDER_EXIT("failed to initialize event loop");
}

void bender_ev_deinit(void) {
    bender_ev_deinit_common();
    close(bender_vm.epoll);
    close(bender_vm.timerfd);
    bender_ev_cleanup_selfpipe();
    bender_vm.epoll = 0;
}

/*
 * End epoll implementation
 */

#elif defined(BENDER_EV_KQUEUE)
/* Definition from:
 *   https://github.com/wahern/cqueues/blob/master/src/lib/kpoll.c
 * NetBSD uses intptr_t while others use void * for .udata */
#define EV_SETx(ev, a, b, c, d, e, f) EV_SET((ev), (a), (b), (c), (d), (e), ((__typeof__((ev)->udata))(f)))
#define BENDER_KQUEUE_MIN_INTERVAL 0

/* NOTE:
 * NetBSD and OpenBSD expect things are always intervals, and FreeBSD doesn't
 * like an ABSTIME in the past so just use intervals always. Introduces a
 * calculation to determine the minimum timeout per timeout requested of
 * kqueue. Also note that NetBSD doesn't accept timeout intervals less than 1
 * millisecond, so correct all intervals on that platform to be at least 1
 * millisecond.*/
BenderTimestamp to_interval(const BenderTimestamp ts) {
    return ts >= BENDER_KQUEUE_MIN_INTERVAL ? ts : BENDER_KQUEUE_MIN_INTERVAL;
}
#define BENDER_KQUEUE_INTERVAL(timestamp) (to_interval((timestamp - ts_now())))

static BenderTimestamp ts_now(void) {
    struct timespec now;
    bender_assert(-1 != bender_gettime(&now, BENDER_TIME_MONOTONIC), "failed to get time");
    uint64_t res = 1000 * now.tv_sec;
    res += now.tv_nsec / 1000000;
    return res;
}

/* NOTE: Assumes Bender's timestamp precision is in milliseconds. */
static void timestamp2timespec(struct timespec *t, BenderTimestamp ts) {
    t->tv_sec = ts == 0 ? 0 : ts / 1000;
    t->tv_nsec = ts == 0 ? 0 : (ts % 1000) * 1000000;
}

void bender_register_stream_impl(BenderStream *stream, int edge_trigger) {
    struct kevent kevs[2];
    int length = 0;
    int clear = edge_trigger ? EV_CLEAR : 0;
    if (stream->flags & (BENDER_STREAM_READABLE | BENDER_STREAM_ACCEPTABLE)) {
        EV_SETx(&kevs[length++], stream->handle, EVFILT_READ, EV_ADD | EV_ENABLE | clear, 0, 0, stream);
    }
    if (stream->flags & BENDER_STREAM_WRITABLE) {
        EV_SETx(&kevs[length++], stream->handle, EVFILT_WRITE, EV_ADD | EV_ENABLE | clear, 0, 0, stream);
    }
    int status;
    do {
        status = kevent(bender_vm.kq, kevs, length, NULL, 0, NULL);
    } while (status == -1 && errno == EINTR);
    if (status == -1) {
        stream->flags |= BENDER_STREAM_UNREGISTERED;
    }
}

void bender_register_stream(BenderStream *stream) {
    bender_register_stream_impl(stream, 1);
}

void bender_stream_edge_triggered(BenderStream *stream) {
    bender_register_stream_impl(stream, 1);
}

void bender_stream_level_triggered(BenderStream *stream) {
    /* On macos, we seem to need to delete any registered events before re-registering without
     * EV_CLEAR, otherwise the new event will still have EV_CLEAR set erroneously. This could be a
     * kernel bug, but unfortunately the specification is vague here, esp. in regards to where and when
     * EV_CLEAR is set automatically. */
    struct kevent kevs[2];
    int length = 0;
    if (stream->flags & (BENDER_STREAM_READABLE | BENDER_STREAM_ACCEPTABLE)) {
        EV_SETx(&kevs[length++], stream->handle, EVFILT_READ, EV_DELETE, 0, 0, stream);
    }
    if (stream->flags & BENDER_STREAM_WRITABLE) {
        EV_SETx(&kevs[length++], stream->handle, EVFILT_WRITE, EV_DELETE, 0, 0, stream);
    }
    int status;
    do {
        status = kevent(bender_vm.kq, kevs, length, NULL, 0, NULL);
    } while (status == -1 && errno == EINTR);
    bender_register_stream_impl(stream, 0);
}

void bender_unregister_stream(BenderStream *stream) {
    if (stream->flags & BENDER_STREAM_NODUPS) return;
    struct kevent kevs[2];
    int length = 0;
    if (stream->flags & (BENDER_STREAM_READABLE | BENDER_STREAM_ACCEPTABLE)) {
        EV_SETx(&kevs[length++], stream->handle, EVFILT_READ, EV_DELETE, 0, 0, stream);
    }
    if (stream->flags & BENDER_STREAM_WRITABLE) {
        EV_SETx(&kevs[length++], stream->handle, EVFILT_WRITE, EV_DELETE, 0, 0, stream);
    }
    int status;
    do {
        status = kevent(bender_vm.kq, kevs, length, NULL, 0, NULL);
    } while (status == -1 && errno == EINTR);
    /* Status might be -1 on BSDs for subprocesses */
    stream->flags |= BENDER_STREAM_UNREGISTERED;
}

#define BENDER_KQUEUE_MAX_EVENTS 512

void bender_loop1_impl(int has_timeout, BenderTimestamp timeout) {
    /* Poll for events */
    /* NOTE:
     * We calculate the timeout interval per iteration. When the interval
     * drops to 0 or negative, we effect a timeout of 0. Effecting a timeout
     * of infinity will not work and could make other fibers with timeouts
     * miss their timeouts if we did so.
     * BENDER_KQUEUE_INTERVAL insures we have a timeout of no less than 0. */
    int status;
    struct timespec ts;
    struct kevent events[BENDER_KQUEUE_MAX_EVENTS];
    do {
        if (bender_vm.timer_enabled || has_timeout) {
            timestamp2timespec(&ts, BENDER_KQUEUE_INTERVAL(timeout));
            status = kevent(bender_vm.kq, NULL, 0, events,
                            BENDER_KQUEUE_MAX_EVENTS, &ts);
        } else {
            status = kevent(bender_vm.kq, NULL, 0, events,
                            BENDER_KQUEUE_MAX_EVENTS, NULL);
        }
    } while (status == -1 && errno == EINTR);
    if (status == -1) {
        BENDER_EXIT("failed to poll events");
    }

    /* Make sure timer is set accordingly. */
    bender_vm.timer_enabled = has_timeout;

    /* Step state machines */
    for (int i = 0; i < status; i++) {
        void *p = (void *) events[i].udata;
        if (bender_vm.selfpipe == p) {
            /* Self-pipe handling */
            bender_ev_handle_selfpipe();
        } else {
            BenderStream *stream = p;
            int filt = events[i].filter;
            int has_err = events[i].flags & EV_ERROR;
            int has_hup = events[i].flags & EV_EOF;
            for (int j = 0; j < 2; j++) {
                BenderFiber *f = j ? stream->read_fiber : stream->write_fiber;
                if (!f) continue;
                if (f->ev_callback && has_err) {
                    f->ev_callback(f, BENDER_ASYNC_EVENT_ERR);
                }
                if (f->ev_callback && (filt == EVFILT_READ) && f == stream->read_fiber) {
                    f->ev_callback(f, BENDER_ASYNC_EVENT_READ);
                }
                if (f->ev_callback && (filt == EVFILT_WRITE) && f == stream->write_fiber) {
                    f->ev_callback(f, BENDER_ASYNC_EVENT_WRITE);
                }
                if (f->ev_callback && has_hup) {
                    f->ev_callback(f, BENDER_ASYNC_EVENT_HUP);
                }
            }
            bender_stream_checktoclose(stream);
        }
    }
}

void bender_ev_init(void) {
    bender_ev_init_common();
    /* TODO - replace selfpipe with EVFILT_USER (or other events) */
    bender_ev_setup_selfpipe();
    bender_vm.kq = kqueue();
    bender_vm.timer_enabled = 0;
    if (bender_vm.kq == -1) goto error;
    struct kevent event;
    EV_SETx(&event, bender_vm.selfpipe[0], EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, bender_vm.selfpipe);
    int status;
    do {
        status = kevent(bender_vm.kq, &event, 1, NULL, 0, NULL);
    } while (status == -1 && errno != EINTR);
    if (status == -1) goto error;
    return;
error:
    BENDER_EXIT("failed to initialize event loop");
}

void bender_ev_deinit(void) {
    bender_ev_deinit_common();
    close(bender_vm.kq);
    bender_ev_cleanup_selfpipe();
    bender_vm.kq = 0;
}

#elif defined(BENDER_EV_POLL)

/* Simple poll implementation. Efficiency is not the goal here, although the poll implementation should be farily efficient
 * for low numbers of concurrent file descriptors. Rather, the code should be simple, portable, correct, and mirror the
 * epoll and kqueue code. */

static BenderTimestamp ts_now(void) {
    struct timespec now;
    bender_assert(-1 != bender_gettime(&now, BENDER_TIME_MONOTONIC), "failed to get time");
    uint64_t res = 1000 * now.tv_sec;
    res += now.tv_nsec / 1000000;
    return res;
}

/* Wait for the next event */
void bender_register_stream(BenderStream *stream) {
    struct pollfd ev = {0};
    stream->index = (uint32_t) bender_vm.stream_count;
    size_t new_count = bender_vm.stream_count + 1;
    if (new_count > bender_vm.stream_capacity) {
        size_t new_cap = new_count * 2;
        bender_vm.fds = bender_realloc(bender_vm.fds, (1 + new_cap) * sizeof(struct pollfd));
        bender_vm.streams = bender_realloc(bender_vm.streams, new_cap * sizeof(BenderStream *));
        if (!bender_vm.fds || !bender_vm.streams) {
            BENDER_OUT_OF_MEMORY;
        }
        bender_vm.stream_capacity = new_cap;
    }
    ev.fd = stream->handle;
    ev.events = POLLIN | POLLOUT;
    bender_vm.fds[bender_vm.stream_count + 1] = ev;
    bender_vm.streams[bender_vm.stream_count] = stream;
    bender_vm.stream_count = new_count;
}

void bender_unregister_stream(BenderStream *stream) {
    uint32_t i = stream->index;
    size_t j = bender_vm.stream_count - 1;
    BenderStream *last = bender_vm.streams[j];
    struct pollfd lastfd = bender_vm.fds[j + 1];
    bender_vm.fds[i + 1] = lastfd;
    bender_vm.streams[i] = last;
    last->index = stream->index;
    bender_vm.stream_count--;
    stream->flags |= BENDER_STREAM_UNREGISTERED;
}

void bender_stream_edge_triggered(BenderStream *stream) {
    (void) stream;
}

void bender_stream_level_triggered(BenderStream *stream) {
    (void) stream;
}

void bender_loop1_impl(int has_timeout, BenderTimestamp timeout) {

    /* set event flags */
    for (size_t i = 0; i < bender_vm.stream_count; i++) {
        BenderStream *stream = bender_vm.streams[i];
        struct pollfd *pfd = bender_vm.fds + i + 1;
        pfd->events = 0;
        pfd->revents = 0;
        BenderFiber *rf = stream->read_fiber;
        BenderFiber *wf = stream->write_fiber;
        if (rf && rf->ev_callback) pfd->events |= POLLIN;
        if (wf && wf->ev_callback) pfd->events |= POLLOUT;
        /* Hack to ignore a file descriptor - make file descriptor negative if we want to ignore */
        if (!pfd->events) {
            pfd->fd = -pfd->fd;
        }
    }

    /* Poll for events */
    int ready;
    do {
        int to = -1;
        if (has_timeout) {
            BenderTimestamp now = ts_now();
            to = now > timeout ? 0 : (int)(timeout - now);
        }
        ready = poll(bender_vm.fds, bender_vm.stream_count + 1, to);
    } while (ready == -1 && errno == EINTR);
    if (ready == -1) {
        BENDER_EXIT("failed to poll events");
    }

    /* Undo negative hack */
    for (size_t i = 0; i < bender_vm.stream_count; i++) {
        struct pollfd *pfd = bender_vm.fds + i + 1;
        if (pfd->fd < 0) {
            pfd->fd = -pfd->fd;
        }
    }

    /* Check selfpipe */
    if (bender_vm.fds[0].revents & POLLIN) {
        bender_vm.fds[0].revents = 0;
        bender_ev_handle_selfpipe();
    }

    /* Step state machines */
    for (size_t i = 0; i < bender_vm.stream_count; i++) {
        struct pollfd *pfd = bender_vm.fds + i + 1;
        BenderStream *stream = bender_vm.streams[i];
        int mask = pfd->revents;
        if (!mask) continue;
        int has_err = mask & POLLERR;
        int has_hup = mask & POLLHUP;
        BenderFiber *rf = stream->read_fiber;
        BenderFiber *wf = stream->write_fiber;
        if (rf) {
            if (rf->ev_callback && (mask & POLLIN)) {
                rf->ev_callback(rf, BENDER_ASYNC_EVENT_READ);
            } else if (rf->ev_callback && has_hup) {
                rf->ev_callback(rf, BENDER_ASYNC_EVENT_HUP);
            } else if (rf->ev_callback && has_err) {
                rf->ev_callback(rf, BENDER_ASYNC_EVENT_ERR);
            }
        }
        if (wf) {
            if (wf->ev_callback && (mask & POLLOUT)) {
                wf->ev_callback(wf, BENDER_ASYNC_EVENT_WRITE);
            } else if (wf->ev_callback && has_hup) {
                wf->ev_callback(wf, BENDER_ASYNC_EVENT_HUP);
            } else if (wf->ev_callback && has_err) {
                wf->ev_callback(wf, BENDER_ASYNC_EVENT_ERR);
            }
        }
        bender_stream_checktoclose(stream);
    }
}

void bender_ev_init(void) {
    bender_ev_init_common();
    bender_vm.fds = NULL;
    bender_ev_setup_selfpipe();
    bender_vm.fds = bender_malloc(sizeof(struct pollfd));
    if (NULL == bender_vm.fds) {
        BENDER_OUT_OF_MEMORY;
    }
    bender_vm.fds[0].fd = bender_vm.selfpipe[0];
    bender_vm.fds[0].events = POLLIN;
    bender_vm.fds[0].revents = 0;
    bender_vm.streams = NULL;
    bender_vm.stream_count = 0;
    bender_vm.stream_capacity = 0;
    return;
}

void bender_ev_deinit(void) {
    bender_ev_deinit_common();
    bender_ev_cleanup_selfpipe();
    bender_free(bender_vm.fds);
    bender_free(bender_vm.streams);
    bender_vm.fds = NULL;
    bender_vm.streams = NULL;
}

#endif

/*
 * End poll implementation
 */

/*
 * Generic Callback system. Post a function pointer + data to the event loop (from another
 * thread or even a signal handler). Allows posting events from another thread or signal handler.
 */
void bender_ev_post_event(BenderVM *vm, BenderCallback cb, BenderEVGenericMessage msg) {
    vm = vm ? vm : &bender_vm;
    bender_atomic_inc(&vm->listener_count);
#ifdef BENDER_WINDOWS
    BenderHandle iocp = vm->iocp;
    BenderSelfPipeEvent *event = bender_malloc(sizeof(BenderSelfPipeEvent));
    if (NULL == event) {
        BENDER_OUT_OF_MEMORY;
    }
    event->msg = msg;
    event->cb = cb;
    bender_assert(PostQueuedCompletionStatus(iocp,
                                            sizeof(BenderSelfPipeEvent),
                                            0,
                                            (LPOVERLAPPED) event),
                 "failed to post completion event");
#else
    BenderSelfPipeEvent event;
    memset(&event, 0, sizeof(event));
    event.msg = msg;
    event.cb = cb;
    int fd = vm->selfpipe[1];
    /* Writes are nonblocking. A full pipe means the TARGET VM has not drained
     * yet, which is a scheduling fact rather than an error: the poster simply
     * has to wait for it to run. The retry budget therefore has to outlast a
     * real scheduler interval, not a single yield.
     *
     * A proc-spawn burst crosses the default pipe capacity (16 KiB / 48 bytes
     * per event = ~341 events) almost immediately: measured, 29000 procs across
     * 4 lanes succeeds and 30000 aborts with "failed to write event to
     * self-pipe" -- a hard crash from ordinary, correct code. `sleep(0)` only
     * yields microseconds, so the old 20-try budget could never cover drain
     * time on a loaded machine.
     *
     * The budget stays bounded and still ends in the deliberate signal-safe
     * abort, because a signal handler genuinely cannot block; ordinary thread
     * posters now simply get long enough to be serviced. */
    int tries = 2000;
    int retried_from_zero = 0;
    while (tries > 0) {
        int status;
        do {
            status = write(fd, &event, sizeof(event));
        } while (status == -1 && errno == EINTR);
        if (status > 0) break;
        /* Back off progressively: yield first (cheap, usually enough), then
         * start sleeping so a draining VM can actually make progress. */
        if (tries > 1900) {
            sleep(0);
        } else {
            struct timespec ts = {0, 200000};  /* 200us */
            nanosleep(&ts, NULL);
        }
        tries--;
    }
    /* NOTE: do NOT block-and-wait here. bender_ev_post_event is called while
     * holding a channel lock (see the threaded-channel delivery paths), so a
     * poster that waits for the pipe to drain can hold the lock that the
     * draining VM needs to make progress -- a lock-order inversion, observed
     * live as one thread in pthread_mutex_lock while another sat in this loop.
     * The budget above is generous enough for ordinary scheduling skew, and the
     * bounded abort stays as the signal-safe fallback. Bulk cross-lane fan-in
     * needs flow control at the CALLER (proc/spawn and ev/give should pump the
     * loop periodically), not a blocking wait inside the poster. */
    if (0 == tries && !retried_from_zero) {
        /* One last chance: a full pipe means the TARGET VM needs to run. On a
         * lane thread the target is usually a different VM, so sleeping here
         * is correct and cannot invert anything (we hold no channel lock at
         * THIS point in the retry, unlike the delivery paths that post under
         * the lock -- those are guarded by the caller's own pacing). Keep it
         * strictly bounded so a genuinely dead target still aborts. */
        retried_from_zero = 1;
        for (int slow = 0; slow < 20000; slow++) {
            int st;
            do {
                st = write(fd, &event, sizeof(event));
            } while (st == -1 && errno == EINTR);
            if (st > 0) { tries = 1; break; }
            struct timespec ts = {0, 200000};   /* 200us */
            nanosleep(&ts, NULL);
        }
    }
    bender_assert(tries > 0, "failed to write event to self-pipe");
#endif
}

/*
 * Threaded calls
 */

#ifdef BENDER_WINDOWS
static DWORD WINAPI bender_thread_body(LPVOID ptr) {
    BenderEVThreadInit *init = (BenderEVThreadInit *)ptr;
    BenderEVGenericMessage msg = init->msg;
    BenderThreadedSubroutine subr = init->subr;
    BenderThreadedCallback cb = init->cb;
    BenderHandle iocp = init->write_pipe;
    /* Reuse memory from thread init for returning data */
    init->msg = subr(msg);
    init->cb = cb;
    bender_assert(PostQueuedCompletionStatus(iocp,
                                            sizeof(BenderSelfPipeEvent),
                                            0,
                                            (LPOVERLAPPED) init),
                 "failed to post completion event");
    return 0;
}
#else
static void *bender_thread_body(void *ptr) {
    BenderEVThreadInit *init = (BenderEVThreadInit *)ptr;
    BenderEVGenericMessage msg = init->msg;
    BenderThreadedSubroutine subr = init->subr;
    BenderThreadedCallback cb = init->cb;
    int fd = init->write_pipe;
    bender_free(init);
    BenderSelfPipeEvent response;
    memset(&response, 0, sizeof(response));
    response.msg = subr(msg);
    response.cb = cb;
    /* handle a bit of back pressure before giving up. */
    int tries = 4;
    while (tries > 0) {
        int status;
        do {
            status = write(fd, &response, sizeof(response));
        } while (status == -1 && errno == EINTR);
        if (status > 0) break;
        sleep(1);
        tries--;
    }
    return NULL;
}
#endif

void bender_ev_threaded_call(BenderThreadedSubroutine fp, BenderEVGenericMessage arguments, BenderThreadedCallback cb) {
    BenderEVThreadInit *init = bender_malloc(sizeof(BenderEVThreadInit));
    if (NULL == init) {
        BENDER_OUT_OF_MEMORY;
    }
    init->msg = arguments;
    init->subr = fp;
    init->cb = cb;

#ifdef BENDER_WINDOWS
    init->write_pipe = bender_vm.iocp;
    HANDLE thread_handle = CreateThread(NULL, 0, bender_thread_body, init, 0, NULL);
    if (NULL == thread_handle) {
        bender_free(init);
        bender_panic("failed to create thread");
    }
    CloseHandle(thread_handle); /* detach from thread */
#else
    init->write_pipe = bender_vm.selfpipe[1];
    pthread_t waiter_thread;
    int err = pthread_create(&waiter_thread, &bender_vm.new_thread_attr, bender_thread_body, init);
    if (err) {
        bender_free(init);
        bender_panicf("%s", bender_strerror(err));
    }
#endif

    /* Increment ev refcount so we don't quit while waiting for a subprocess */
    bender_ev_inc_refcount();
}

/* Default callback for bender_ev_threaded_await. */
void bender_ev_default_threaded_callback(BenderEVGenericMessage return_value) {
    if (return_value.fiber == NULL) {
        /* Clean up */
        switch (return_value.tag) {
            default:
                break;
            case BENDER_EV_TCTAG_STRINGF:
            case BENDER_EV_TCTAG_ERR_STRINGF:
                bender_free(return_value.argp);
                break;
        }
        return;
    }
    if (bender_fiber_can_resume(return_value.fiber)) {
        switch (return_value.tag) {
            default:
                break;
            case BENDER_EV_TCTAG_NIL:
                bender_schedule(return_value.fiber, bender_wrap_nil());
                break;
            case BENDER_EV_TCTAG_INTEGER:
                bender_schedule(return_value.fiber, bender_wrap_integer(return_value.argi));
                break;
            case BENDER_EV_TCTAG_STRING:
            case BENDER_EV_TCTAG_STRINGF:
                bender_schedule(return_value.fiber, bender_cstringv((const char *) return_value.argp));
                break;
            case BENDER_EV_TCTAG_KEYWORD:
                bender_schedule(return_value.fiber, bender_ckeywordv((const char *) return_value.argp));
                break;
            case BENDER_EV_TCTAG_ERR_STRING:
            case BENDER_EV_TCTAG_ERR_STRINGF:
                bender_cancel(return_value.fiber, bender_cstringv((const char *) return_value.argp));
                break;
            case BENDER_EV_TCTAG_ERR_KEYWORD:
                bender_cancel(return_value.fiber, bender_ckeywordv((const char *) return_value.argp));
                break;
            case BENDER_EV_TCTAG_BOOLEAN:
                bender_schedule(return_value.fiber, bender_wrap_boolean(return_value.argi));
                break;
        }
    }
    /* Clean up */
    switch (return_value.tag) {
        default:
            break;
        case BENDER_EV_TCTAG_STRINGF:
        case BENDER_EV_TCTAG_ERR_STRINGF:
            bender_free(return_value.argp);
            break;
    }
    bender_gcunroot(bender_wrap_fiber(return_value.fiber));
}

/* Convenience method for common case */
BENDER_NO_RETURN
void bender_ev_threaded_await(BenderThreadedSubroutine fp, int tag, int argi, void *argp) {
    BenderEVGenericMessage arguments;
    memset(&arguments, 0, sizeof(arguments));
    arguments.tag = tag;
    arguments.argi = argi;
    arguments.argp = argp;
    arguments.fiber = bender_root_fiber();
    bender_gcroot(bender_wrap_fiber(arguments.fiber));
    bender_ev_threaded_call(fp, arguments, bender_ev_default_threaded_callback);
    bender_await();
}

/*
 * C API helpers for reading and writing from streams.
 * There is some networking code in here as well as generic
 * reading and writing primitives.
 */

void bender_stream_flags(BenderStream *stream, uint32_t flags) {
    if (stream->flags & BENDER_STREAM_CLOSED) {
        bender_panic("stream is closed");
    }
    if ((stream->flags & flags) != flags) {
        const char *rmsg = "", *wmsg = "", *amsg = "", *dmsg = "", *smsg = "stream";
        if (flags & BENDER_STREAM_READABLE) rmsg = "readable ";
        if (flags & BENDER_STREAM_WRITABLE) wmsg = "writable ";
        if (flags & BENDER_STREAM_ACCEPTABLE) amsg = "server ";
        if (flags & BENDER_STREAM_UDPSERVER) dmsg = "datagram ";
        if (flags & BENDER_STREAM_SOCKET) smsg = "socket";
        bender_panicf("bad stream, expected %s%s%s%s%s", rmsg, wmsg, amsg, dmsg, smsg);
    }
}

/* When there is an IO error, we need to be able to convert it to a Bender
 * string to raise a Bender error. */
#ifdef BENDER_WINDOWS
#define BENDER_EV_CHUNKSIZE 4096
Bender bender_ev_lasterr(void) {
    int code = GetLastError();
    char msgbuf[256];
    msgbuf[0] = '\0';
    FormatMessage(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                  NULL,
                  code,
                  MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                  msgbuf,
                  sizeof(msgbuf),
                  NULL);
    if (!*msgbuf) snprintf(msgbuf, sizeof(msgbuf), "%d", code);
    char *c = msgbuf;
    while (*c) {
        if (*c == '\n' || *c == '\r') {
            *c = '\0';
            break;
        }
        c++;
    }
    return bender_cstringv(msgbuf);
}
#else
Bender bender_ev_lasterr(void) {
    return bender_cstringv(bender_strerror(errno));
}
#endif

/* State machine for read/recv/recvfrom */

typedef enum {
    BENDER_ASYNC_READMODE_READ,
    BENDER_ASYNC_READMODE_RECV,
    BENDER_ASYNC_READMODE_RECVFROM
} BenderReadMode;

typedef struct {
#ifdef BENDER_WINDOWS
    BenderOverlapped overlapped;
    DWORD flags;
#ifdef BENDER_NET
    WSABUF wbuf;
    struct sockaddr from;
    int fromlen;
#endif
    uint8_t chunk_buf[BENDER_EV_CHUNKSIZE];
#else
    int flags;
#endif
    int32_t bytes_left;
    int32_t bytes_read;
    BenderBuffer *buf;
    int is_chunk;
    BenderReadMode mode;
} StateRead;

void ev_callback_read(BenderFiber *fiber, BenderAsyncEvent event) {
    BenderStream *stream = fiber->ev_stream;
    StateRead *state = (StateRead *) fiber->ev_state;
    switch (event) {
        default:
            break;
        case BENDER_ASYNC_EVENT_MARK:
            bender_mark(bender_wrap_buffer(state->buf));
            break;
        case BENDER_ASYNC_EVENT_CLOSE:
            bender_schedule(fiber, bender_wrap_nil());
            bender_async_end(fiber);
            break;
#ifdef BENDER_WINDOWS
        case BENDER_ASYNC_EVENT_FAILED:
        case BENDER_ASYNC_EVENT_COMPLETE: {
            /* Called when read finished */
            uint32_t ev_bytes = (uint32_t) state->overlapped.bytes_transfered;
            state->bytes_read += ev_bytes;
            if (state->bytes_read == 0 && (state->mode != BENDER_ASYNC_READMODE_RECVFROM)) {
                bender_schedule(fiber, bender_wrap_nil());
                bender_async_end(fiber);
                return;
            }

            bender_buffer_push_bytes(state->buf, state->chunk_buf, ev_bytes);
            state->bytes_left -= ev_bytes;

            if (state->bytes_left == 0 || !state->is_chunk || ev_bytes == 0) {
                Bender resume_val;
#ifdef BENDER_NET
                if (state->mode == BENDER_ASYNC_READMODE_RECVFROM) {
                    void *abst = bender_abstract(&bender_address_type, state->fromlen);
                    memcpy(abst, &state->from, state->fromlen);
                    resume_val = bender_wrap_abstract(abst);
                } else
#endif
                {
                    resume_val = bender_wrap_buffer(state->buf);
                }
                bender_schedule(fiber, resume_val);
                bender_async_end(fiber);
                return;
            }
        }

        /* fallthrough */
        case BENDER_ASYNC_EVENT_INIT: {
            int32_t chunk_size = state->bytes_left > BENDER_EV_CHUNKSIZE ? BENDER_EV_CHUNKSIZE : state->bytes_left;
            memset(&(state->overlapped), 0, sizeof(BenderOverlapped));
            int status;
#ifdef BENDER_NET
            if (state->mode == BENDER_ASYNC_READMODE_RECVFROM) {
                state->wbuf.len = (ULONG) chunk_size;
                state->wbuf.buf = (char *) state->chunk_buf;
                state->fromlen = sizeof(state->from);
                status = WSARecvFrom((SOCKET) stream->handle, &state->wbuf, 1,
                                     NULL, &state->flags, &state->from, &state->fromlen, &state->overlapped.as.wsaoverlapped, NULL);
                if (status && (WSA_IO_PENDING != WSAGetLastError())) {
                    bender_cancel(fiber, bender_ev_lasterr());
                    bender_async_end(fiber);
                    return;
                }
            } else
#endif
            {
                /* Some handles (not all) read from the offset in lpOverlapped
                 * if its not set before calling `ReadFile` these streams will always read from offset 0 */
                state->overlapped.as.overlapped.Offset = (DWORD) state->bytes_read;

                status = ReadFile(stream->handle, state->chunk_buf, chunk_size, NULL, &state->overlapped.as.overlapped);
                if (!status && (ERROR_IO_PENDING != GetLastError())) {
                    if (GetLastError() == ERROR_BROKEN_PIPE) {
                        if (state->bytes_read) {
                            bender_schedule(fiber, bender_wrap_buffer(state->buf));
                        } else {
                            bender_schedule(fiber, bender_wrap_nil());
                        }
                    } else {
                        bender_cancel(fiber, bender_ev_lasterr());
                    }
                    bender_async_end(fiber);
                    return;
                }
            }
            bender_async_in_flight(fiber);
        }
        break;
#else
        case BENDER_ASYNC_EVENT_ERR: {
            if (state->bytes_read) {
                bender_schedule(fiber, bender_wrap_buffer(state->buf));
            } else {
                bender_schedule(fiber, bender_wrap_nil());
            }
            stream->read_fiber = NULL;
            bender_async_end(fiber);
            break;
        }

    read_more:
        case BENDER_ASYNC_EVENT_HUP:
        case BENDER_ASYNC_EVENT_INIT:
        case BENDER_ASYNC_EVENT_READ: {
            BenderBuffer *buffer = state->buf;
            int32_t bytes_left = state->bytes_left;
            int32_t read_limit = state->is_chunk ? (bytes_left > 4096 ? 4096 : bytes_left) : bytes_left;
            bender_buffer_extra(buffer, read_limit);
            ssize_t nread;
#ifdef BENDER_NET
            char saddr[256];
            socklen_t socklen = sizeof(saddr);
#endif
            do {
#ifdef BENDER_NET
                if (state->mode == BENDER_ASYNC_READMODE_RECVFROM) {
                    nread = recvfrom(stream->handle, buffer->data + buffer->count, read_limit, state->flags,
                                     (struct sockaddr *)&saddr, &socklen);
                } else if (state->mode == BENDER_ASYNC_READMODE_RECV) {
                    nread = recv(stream->handle, buffer->data + buffer->count, read_limit, state->flags);
                } else
#endif
                {
                    nread = read(stream->handle, buffer->data + buffer->count, read_limit);
                }
            } while (nread == -1 && errno == EINTR);

            /* Check for errors - special case errors that can just be waited on to fix */
            if (nread == -1) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    break;
                }
                /* In stream protocols, a pipe error is end of stream */
                if (errno == EPIPE && (state->mode != BENDER_ASYNC_READMODE_RECVFROM)) {
                    nread = 0;
                } else {
                    bender_cancel(fiber, bender_ev_lasterr());
                    bender_async_end(fiber);
                    break;
                }
            }

            /* Only allow 0-length packets in recv-from. In stream protocols, a zero length packet is EOS. */
            state->bytes_read += nread;
            if (state->bytes_read == 0 && (state->mode != BENDER_ASYNC_READMODE_RECVFROM)) {
                bender_schedule(fiber, bender_wrap_nil());
                bender_async_end(fiber);
                break;
            }

            /* Increment buffer counts */
            buffer->count += nread;
            bytes_left -= nread;
            state->bytes_left = bytes_left;

            /* Resume if done */
            if (!state->is_chunk || bytes_left == 0 || nread == 0) {
                Bender resume_val;
#ifdef BENDER_NET
                if (state->mode == BENDER_ASYNC_READMODE_RECVFROM) {
                    void *abst = bender_abstract(&bender_address_type, socklen);
                    memcpy(abst, &saddr, socklen);
                    resume_val = bender_wrap_abstract(abst);
                } else
#endif
                {
                    resume_val = bender_wrap_buffer(buffer);
                }
                bender_schedule(fiber, resume_val);
                bender_async_end(fiber);
                break;
            }

            /* Read some more if possible */
            goto read_more;
        }
        break;
#endif
    }
}

static BENDER_NO_RETURN void bender_ev_read_generic(BenderStream *stream, BenderBuffer *buf, int32_t nbytes, int is_chunked, BenderReadMode mode, int flags) {
    StateRead *state = bender_malloc(sizeof(StateRead));
    state->is_chunk = is_chunked;
    state->buf = buf;
    state->bytes_left = nbytes;
    state->bytes_read = 0;
    state->mode = mode;
#ifdef BENDER_WINDOWS
    state->flags = (DWORD) flags;
#else
    state->flags = flags;
#endif
    bender_async_start(stream, BENDER_ASYNC_LISTEN_READ, ev_callback_read, state);
}

BENDER_NO_RETURN void bender_ev_read(BenderStream *stream, BenderBuffer *buf, int32_t nbytes) {
    bender_ev_read_generic(stream, buf, nbytes, 0, BENDER_ASYNC_READMODE_READ, 0);
}
BENDER_NO_RETURN void bender_ev_readchunk(BenderStream *stream, BenderBuffer *buf, int32_t nbytes) {
    bender_ev_read_generic(stream, buf, nbytes, 1, BENDER_ASYNC_READMODE_READ, 0);
}
#ifdef BENDER_NET
BENDER_NO_RETURN void bender_ev_recv(BenderStream *stream, BenderBuffer *buf, int32_t nbytes, int flags) {
    bender_ev_read_generic(stream, buf, nbytes, 0, BENDER_ASYNC_READMODE_RECV, flags);
}
BENDER_NO_RETURN void bender_ev_recvchunk(BenderStream *stream, BenderBuffer *buf, int32_t nbytes, int flags) {
    bender_ev_read_generic(stream, buf, nbytes, 1, BENDER_ASYNC_READMODE_RECV, flags);
}
BENDER_NO_RETURN void bender_ev_recvfrom(BenderStream *stream, BenderBuffer *buf, int32_t nbytes, int flags) {
    bender_ev_read_generic(stream, buf, nbytes, 0, BENDER_ASYNC_READMODE_RECVFROM, flags);
}
#endif

/*
 * State machine for write/send/send-to
 */

typedef enum {
    BENDER_ASYNC_WRITEMODE_WRITE,
    BENDER_ASYNC_WRITEMODE_SEND,
    BENDER_ASYNC_WRITEMODE_SENDTO
} BenderWriteMode;

typedef struct {
#ifdef BENDER_WINDOWS
    BenderOverlapped overlapped;
    DWORD flags;
#ifdef BENDER_NET
    WSABUF wbuf;
#endif
#else
    int flags;
    int32_t start;
#endif
    union {
        BenderBuffer *buf;
        const uint8_t *str;
    } src;
    int is_buffer;
    BenderWriteMode mode;
    void *dest_abst;
} StateWrite;

void ev_callback_write(BenderFiber *fiber, BenderAsyncEvent event) {
    BenderStream *stream = fiber->ev_stream;
    StateWrite *state = (StateWrite *) fiber->ev_state;
    switch (event) {
        default:
            break;
        case BENDER_ASYNC_EVENT_MARK: {
            bender_mark(state->is_buffer
                       ? bender_wrap_buffer(state->src.buf)
                       : bender_wrap_string(state->src.str));
            if (state->mode == BENDER_ASYNC_WRITEMODE_SENDTO) {
                bender_mark(bender_wrap_abstract(state->dest_abst));
            }
            break;
        }
        case BENDER_ASYNC_EVENT_CLOSE:
            bender_cancel(fiber, bender_cstringv("stream closed"));
            bender_async_end(fiber);
            break;
#ifdef BENDER_WINDOWS
        case BENDER_ASYNC_EVENT_FAILED:
        case BENDER_ASYNC_EVENT_COMPLETE: {
            /* Called when write finished */
            uint32_t ev_bytes = (uint32_t) state->overlapped.bytes_transfered;
            if (ev_bytes == 0 && (state->mode != BENDER_ASYNC_WRITEMODE_SENDTO)) {
                bender_cancel(fiber, bender_cstringv("disconnect"));
                bender_async_end(fiber);
                return;
            }

            bender_schedule(fiber, bender_wrap_nil());
            bender_async_end(fiber);
            return;
        }
        break;
        case BENDER_ASYNC_EVENT_INIT: {
            /* Begin write */
            int32_t len;
            const uint8_t *bytes;
            if (state->is_buffer) {
                /* If buffer, convert to string. */
                /* TODO - be more efficient about this */
                BenderBuffer *buffer = state->src.buf;
                BenderString str = bender_string(buffer->data, buffer->count);
                bytes = str;
                len = buffer->count;
                state->is_buffer = 0;
                state->src.str = str;
            } else {
                bytes = state->src.str;
                len = bender_string_length(bytes);
            }
            memset(&(state->overlapped), 0, sizeof(BenderOverlapped));

            int status;
#ifdef BENDER_NET
            if (state->mode == BENDER_ASYNC_WRITEMODE_SENDTO) {
                SOCKET sock = (SOCKET) stream->handle;
                state->wbuf.buf = (char *) bytes;
                state->wbuf.len = len;
                const struct sockaddr *to = state->dest_abst;
                int tolen = (int) bender_abstract_size((void *) to);
                status = WSASendTo(sock, &state->wbuf, 1, NULL, state->flags, to, tolen, &state->overlapped.as.wsaoverlapped, NULL);
                if (status) {
                    if (WSA_IO_PENDING == WSAGetLastError()) {
                        bender_async_in_flight(fiber);
                    } else {
                        bender_cancel(fiber, bender_ev_lasterr());
                        bender_async_end(fiber);
                        return;
                    }
                }
            } else
#endif
            {
                /*
                 * File handles in IOCP need to specify this if they are writing to the
                 * ends of files, like how this is used here.
                 * If the underlying resource doesn't support seeking
                 * byte offsets, they will be ignored
                 * but this otherwise writes to the end of the file in question
                 * Right now, os/open streams aren't seekable, so this works.
                 * for more details see the lpOverlapped parameter in
                 * https://docs.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-writefile
                 */
                state->overlapped.as.overlapped.Offset = (DWORD) 0xFFFFFFFF;
                state->overlapped.as.overlapped.OffsetHigh = (DWORD) 0xFFFFFFFF;
                status = WriteFile(stream->handle, bytes, len, NULL, &state->overlapped.as.overlapped);
                if (!status) {
                    if (ERROR_IO_PENDING == GetLastError()) {
                        bender_async_in_flight(fiber);
                    } else {
                        bender_cancel(fiber, bender_ev_lasterr());
                        bender_async_end(fiber);
                        return;
                    }
                }
            }
        }
        break;
#else
        case BENDER_ASYNC_EVENT_ERR:
            bender_cancel(fiber, bender_cstringv("stream err"));
            bender_async_end(fiber);
            break;
        case BENDER_ASYNC_EVENT_HUP:
            bender_cancel(fiber, bender_cstringv("stream hup"));
            bender_async_end(fiber);
            break;
        case BENDER_ASYNC_EVENT_INIT:
        case BENDER_ASYNC_EVENT_WRITE: {
            int32_t start, len;
            const uint8_t *bytes;
            start = state->start;
            if (state->is_buffer) {
                BenderBuffer *buffer = state->src.buf;
                bytes = buffer->data;
                len = buffer->count;
            } else {
                bytes = state->src.str;
                len = bender_string_length(bytes);
            }
            ssize_t nwrote = 0;
            if (start < len) {
                int32_t nbytes = len - start;
                void *dest_abst = state->dest_abst;
                do {
#ifdef BENDER_NET
                    if (state->mode == BENDER_ASYNC_WRITEMODE_SENDTO) {
                        nwrote = sendto(stream->handle, bytes + start, nbytes, state->flags,
                                        (struct sockaddr *) dest_abst, bender_abstract_size(dest_abst));
                    } else if (state->mode == BENDER_ASYNC_WRITEMODE_SEND) {
                        nwrote = send(stream->handle, bytes + start, nbytes, state->flags);
                    } else
#endif
                    {
                        nwrote = write(stream->handle, bytes + start, nbytes);
                    }
                } while (nwrote == -1 && errno == EINTR);

                /* Handle write errors */
                if (nwrote == -1) {
                    if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                    bender_cancel(fiber, bender_ev_lasterr());
                    bender_async_end(fiber);
                    break;
                }

                /* Unless using datagrams, empty message is a disconnect */
                if (nwrote == 0 && !dest_abst) {
                    bender_cancel(fiber, bender_cstringv("disconnect"));
                    bender_async_end(fiber);
                    break;
                }

                if (nwrote > 0) {
                    start += nwrote;
                } else {
                    start = len;
                }
            }
            state->start = start;
            if (start >= len) {
                bender_schedule(fiber, bender_wrap_nil());
                bender_async_end(fiber);
                break;
            }
            break;
        }
        break;
#endif
    }
}

static BENDER_NO_RETURN void bender_ev_write_generic(BenderStream *stream, void *buf, void *dest_abst, BenderWriteMode mode, int is_buffer, int flags) {
    StateWrite *state = bender_malloc(sizeof(StateWrite));
    state->is_buffer = is_buffer;
    state->src.buf = buf;
    state->dest_abst = dest_abst;
    state->mode = mode;
#ifdef BENDER_WINDOWS
    state->flags = (DWORD) flags;
#else
    state->flags = flags;
    state->start = 0;
#endif
    bender_async_start(stream, BENDER_ASYNC_LISTEN_WRITE, ev_callback_write, state);
}

BENDER_NO_RETURN void bender_ev_write_buffer(BenderStream *stream, BenderBuffer *buf) {
    bender_ev_write_generic(stream, buf, NULL, BENDER_ASYNC_WRITEMODE_WRITE, 1, 0);
}

BENDER_NO_RETURN void bender_ev_write_string(BenderStream *stream, BenderString str) {
    bender_ev_write_generic(stream, (void *) str, NULL, BENDER_ASYNC_WRITEMODE_WRITE, 0, 0);
}

#ifdef BENDER_NET
BENDER_NO_RETURN void bender_ev_send_buffer(BenderStream *stream, BenderBuffer *buf, int flags) {
    bender_ev_write_generic(stream, buf, NULL, BENDER_ASYNC_WRITEMODE_SEND, 1, flags);
}

BENDER_NO_RETURN void bender_ev_send_string(BenderStream *stream, BenderString str, int flags) {
    bender_ev_write_generic(stream, (void *) str, NULL, BENDER_ASYNC_WRITEMODE_SEND, 0, flags);
}

BENDER_NO_RETURN void bender_ev_sendto_buffer(BenderStream *stream, BenderBuffer *buf, void *dest, int flags) {
    bender_ev_write_generic(stream, buf, dest, BENDER_ASYNC_WRITEMODE_SENDTO, 1, flags);
}

BENDER_NO_RETURN void bender_ev_sendto_string(BenderStream *stream, BenderString str, void *dest, int flags) {
    bender_ev_write_generic(stream, (void *) str, dest, BENDER_ASYNC_WRITEMODE_SENDTO, 0, flags);
}
#endif

/* For a pipe ID */
#ifdef BENDER_WINDOWS
static volatile long PipeSerialNumber;
#endif

/*
 * mode = 0: both sides non-blocking.
 * mode = 1: only read side non-blocking: write side sent to subprocess
 * mode = 2: only write side non-blocking: read side sent to subprocess
 * mode = 3: both sides blocking - for use in two subprocesses (making pipeline from external processes)
 */
int bender_make_pipe(BenderHandle handles[2], int mode) {
#ifdef BENDER_WINDOWS
    /*
     * On windows, the built in CreatePipe function doesn't support overlapped IO
     * so we lift from the windows source code and modify for our own version.
     */
    BenderHandle shandle, chandle;
    CHAR PipeNameBuffer[MAX_PATH];
    SECURITY_ATTRIBUTES saAttr;
    memset(&saAttr, 0, sizeof(saAttr));
    saAttr.nLength = sizeof(saAttr);
    saAttr.bInheritHandle = TRUE;
    if (mode == 3) {
        /* No overlapped IO involved, just call CreatePipe */
        if (!CreatePipe(handles, handles + 1, &saAttr, 0)) return -1;
        return 0;
    }
    snprintf(PipeNameBuffer,
             sizeof(PipeNameBuffer),
             "\\\\.\\Pipe\\BenderPipeFile.%08x.%08x",
             (unsigned int) GetCurrentProcessId(),
             (unsigned int) InterlockedIncrement(&PipeSerialNumber));

    /* server handle goes to subprocess */
    shandle = CreateNamedPipeA(
                  PipeNameBuffer,
                  (mode == 2 ? PIPE_ACCESS_INBOUND : PIPE_ACCESS_OUTBOUND) | FILE_FLAG_OVERLAPPED,
                  PIPE_TYPE_BYTE | PIPE_WAIT,
                  255,           /* Max number of pipes for duplication. */
                  4096,          /* Out buffer size */
                  4096,          /* In buffer size */
                  120 * 1000,    /* Timeout in ms */
                  &saAttr);
    if (shandle == INVALID_HANDLE_VALUE) {
        return -1;
    }

    /* we keep client handle */
    chandle = CreateFileA(
                  PipeNameBuffer,
                  (mode == 2 ? GENERIC_WRITE : GENERIC_READ),
                  0,
                  &saAttr,
                  OPEN_EXISTING,
                  FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED,
                  NULL);

    if (chandle == INVALID_HANDLE_VALUE) {
        CloseHandle(shandle);
        return -1;
    }
    if (mode == 2) {
        handles[0] = shandle;
        handles[1] = chandle;
    } else {
        handles[0] = chandle;
        handles[1] = shandle;
    }
    return 0;
#else
    if (pipe(handles)) return -1;
    if (mode != 2 && fcntl(handles[0], F_SETFD, FD_CLOEXEC)) goto error;
    if (mode != 1 && fcntl(handles[1], F_SETFD, FD_CLOEXEC)) goto error;
    if (mode != 2 && mode != 3 && fcntl(handles[0], F_SETFL, O_NONBLOCK)) goto error;
    if (mode != 1 && mode != 3 && fcntl(handles[1], F_SETFL, O_NONBLOCK)) goto error;
    return 0;
error:
    close(handles[0]);
    close(handles[1]);
    return -1;
#endif
}

/* C functions */

BENDER_CORE_FN(cfun_ev_go,
              "(ev/go fiber-or-fun &opt value supervisor)",
              "Put a fiber on the event loop to be resumed later. If a "
              "function is used, it is wrapped with `fiber/new` first. "
              "Returns a task fiber. Optionally pass a value to resume "
              "with, otherwise resumes with nil. An optional `core/channel` "
              "can be provided as a supervisor. When various events occur "
              "in the newly scheduled fiber, an event will be pushed to the "
              "supervisor. If not provided, the new fiber will inherit the "
              "current supervisor.\n\n"
              "The fiber does NOT start immediately. Queued fibers make "
              "progress only when the event loop runs, which happens when the "
              "current fiber suspends or when the loop is pumped explicitly. "
              "A main fiber that loops without suspending therefore starves "
              "every `ev/go` fiber. Call `(ev/pump)` once per iteration to "
              "drive the queue from a non-yielding loop such as a render "
              "frame, or `(ev/sleep 0)` to also yield the current fiber.") {
    bender_arity(argc, 1, 3);
    Bender value = argc >= 2 ? argv[1] : bender_wrap_nil();
    void *supervisor = bender_optabstract(argv, argc, 2, &bender_channel_type, bender_vm.root_fiber->supervisor_channel);
    BenderFiber *fiber;
    if (bender_checktype(argv[0], BENDER_FUNCTION)) {
        /* Create a fiber for the user */
        BenderFunction *func = bender_unwrap_function(argv[0]);
        if (func->def->min_arity > 1) {
            bender_panicf("task function must accept 0 or 1 arguments");
        }
        fiber = bender_fiber(func, 64, func->def->min_arity, &value);
        bender_assert(fiber != NULL, "bad fiber-func");
        /* P3.6: ev tasks opt into the lane-shared stack when enabled
         * (fresh fibers only — user-supplied fibers keep theirs). */
        fiber->shared_stack = bender_vm.shared_stacks_on;
        fiber->flags |=
            BENDER_FIBER_MASK_ERROR |
            BENDER_FIBER_MASK_USER0 |
            BENDER_FIBER_MASK_USER1 |
            BENDER_FIBER_MASK_USER2 |
            BENDER_FIBER_MASK_USER3 |
            BENDER_FIBER_MASK_USER4;
        if (!bender_vm.fiber->env) {
            bender_vm.fiber->env = bender_table(0);
        }
        fiber->env = bender_table(0);
        fiber->env->proto = bender_vm.fiber->env;
    } else {
        fiber = bender_getfiber(argv, 0);
        if (bender_fiber_status(fiber) != BENDER_STATUS_NEW) {
            bender_panic("can only schedule new fibers where (= (fiber/status f) :new)");
        }
    }
    fiber->supervisor_channel = supervisor;
    bender_schedule(fiber, value);
    return bender_wrap_fiber(fiber);
}

#define BENDER_THREAD_SUPERVISOR_FLAG 0x100
/* Skip the worker VM's bender_deinit at thread end. Lane workers use
 * this: several VMs' deinits race on jointly-owned structures (the
 * observed teardown abort), and a lane worker's TLS state is bounded
 * by the lane count anyway. */
#define BENDER_THREAD_NODEINIT_FLAG 0x8

/* For ev/thread - Run an interpreter in the new thread. */
BenderEVGenericMessage bender_go_thread_subr(BenderEVGenericMessage args) {
    BenderBuffer *buffer = (BenderBuffer *) args.argp;
    const uint8_t *nextbytes = buffer->data;
    const uint8_t *endbytes = nextbytes + buffer->count;
    uint32_t flags = args.tag;
    args.tag = 0;
    args.argp = NULL;
    bender_init();
    bender_vm.sandbox_flags = (uint32_t) args.argi;
    BenderTryState tstate;
    BenderSignal signal = bender_try(&tstate);
    if (!signal) {

        /* Set abstract registry */
        if (!(flags & 0x2)) {
            Bender aregv = bender_unmarshal(nextbytes, endbytes - nextbytes,
                                          BENDER_MARSHAL_UNSAFE, NULL, &nextbytes);
            bender_assert(bender_checktype(aregv, BENDER_TABLE), "expected table for abstract registry");
            bender_vm.abstract_registry = bender_unwrap_table(aregv);
            bender_gcroot(bender_wrap_table(bender_vm.abstract_registry));
        }

        /* Get supervisor */
        if (flags & BENDER_THREAD_SUPERVISOR_FLAG) {
            Bender sup =
                bender_unmarshal(nextbytes, endbytes - nextbytes,
                                BENDER_MARSHAL_UNSAFE, NULL, &nextbytes);
            /* Hack - use a global variable to avoid longjmp clobber */
            bender_vm.user = bender_unwrap_pointer(sup);
        }

        /* Set cfunction registry */
        if (!(flags & 0x4)) {
            uint32_t count1;
            memcpy(&count1, nextbytes, sizeof(count1));
            size_t count = (size_t) count1;
            /* Use division to avoid overflowing size_t */
            bender_assert(count <= (endbytes - nextbytes - sizeof(count1)) / sizeof(BenderCFunRegistry), "thread message invalid");
            bender_vm.registry_count = count;
            bender_vm.registry_cap = count;
            bender_vm.registry = bender_malloc(count * sizeof(BenderCFunRegistry));
            if (bender_vm.registry == NULL) {
                BENDER_OUT_OF_MEMORY;
            }
            bender_vm.registry_dirty = 1;
            nextbytes += sizeof(uint32_t);
            memcpy(bender_vm.registry, nextbytes, count * sizeof(BenderCFunRegistry));
            nextbytes += count * sizeof(BenderCFunRegistry);
        }

        Bender fiberv = bender_unmarshal(nextbytes, endbytes - nextbytes,
                                       BENDER_MARSHAL_UNSAFE, NULL, &nextbytes);
        Bender value = bender_unmarshal(nextbytes, endbytes - nextbytes,
                                      BENDER_MARSHAL_UNSAFE, NULL, &nextbytes);
        BenderFiber *fiber;
        if (!bender_checktype(fiberv, BENDER_FIBER)) {
            bender_assert(bender_checktype(fiberv, BENDER_FUNCTION), "expected function or fiber");
            BenderFunction *func = bender_unwrap_function(fiberv);
            /* TODO - normal panics here do not seem to work correctly on Wine + Mingw. This needs to be investigated, and while it appears to be related to longjmp behavior
             * on the platform not working correctly, it is not obvious the issue is. That said, we probably should assert and hard-exit anyway if there is an issue there. */
            bender_assert(func->def->min_arity >= 0 && func->def->min_arity <= 1, "thread function must accept 0 or 1 arguments");
            fiber = bender_fiber(func, 64, func->def->min_arity, &value);
            bender_assert(fiber != NULL, "bad fiber in thread setup");
            fiber->flags |=
                BENDER_FIBER_MASK_ERROR |
                BENDER_FIBER_MASK_USER0 |
                BENDER_FIBER_MASK_USER1 |
                BENDER_FIBER_MASK_USER2 |
                BENDER_FIBER_MASK_USER3 |
                BENDER_FIBER_MASK_USER4;
        } else {
            fiber = bender_unwrap_fiber(fiberv);
        }
        if (flags & 0x8) {
            if (NULL == fiber->env) fiber->env = bender_table(0);
            bender_table_put(fiber->env, bender_ckeywordv("task-id"), value);
        }
        fiber->supervisor_channel = bender_vm.user;
        bender_schedule(fiber, value);
        bender_loop();
        args.tag = BENDER_EV_TCTAG_NIL;
        bender_restore(&tstate);
    } else {
        bender_restore(&tstate);
        void *supervisor = bender_vm.user;
        if (NULL != supervisor) {
            /* Got a supervisor, write error there */
            Bender pair[] = {
                bender_ckeywordv("error"),
                tstate.payload
            };
            bender_channel_push((BenderChannel *)supervisor,
                               bender_wrap_tuple(bender_tuple_n(pair, 2)), 2);
        } else if (flags & 0x1) {
            /* No wait, just print to stderr */
            bender_eprintf("thread start failure: %v\n", tstate.payload);
        } else {
            /* Make ev/thread call from parent thread error */
            if (bender_checktype(tstate.payload, BENDER_STRING)) {
                args.tag = BENDER_EV_TCTAG_ERR_STRINGF;
                BenderString msg = bender_unwrap_string(tstate.payload);
                args.argp = bender_malloc(bender_string_length(msg) + 1);
                memcpy(args.argp, msg, bender_string_length(msg) + 1);
            } else {
                args.tag = BENDER_EV_TCTAG_ERR_STRING;
                args.argp = "failed to start thread";
            }
        }
    }
    bender_buffer_deinit(buffer);
    bender_free(buffer);
    if (!(flags & BENDER_THREAD_NODEINIT_FLAG)) {
        bender_deinit();
    }
    return args;
}

BENDER_CORE_FN(cfun_ev_thread,
              "(ev/thread main &opt value flags supervisor)",
              "Run `main` in a new operating system thread, optionally passing `value` "
              "to resume with. The parameter `main` can either be a fiber, or a function that accepts "
              "0 or 1 arguments. "
              "Unlike `ev/go`, this function will suspend the current fiber until the thread is complete. "
              "If you want to run the thread without waiting for a result, pass the `:n` flag to return nil immediately. "
              "Otherwise, returns nil. Available flags:\n\n"
              "* `:n` - return immediately\n"
              "* `:t` - set the task-id of the new thread to value. The task-id is passed in messages to the supervisor channel.\n"
              "* `:a` - don't copy abstract registry to new thread (performance optimization)\n"
              "* `:c` - don't copy cfunction registry to new thread (performance optimization)") {
    bender_sandbox_assert(BENDER_SANDBOX_THREADS);
    bender_arity(argc, 1, 4);
    Bender value = argc >= 2 ? argv[1] : bender_wrap_nil();
    if (bender_checktype(argv[0], BENDER_FUNCTION)) {
        BenderFunction *func = bender_getfunction(argv, 0);
        if (func->def->arity < 0 || func->def->min_arity > 1) {
            bender_panic("function must take 0 or 1 arguments");
        }
    } else {
        bender_getfiber(argv, 0); /* arg check for fiber */
    }
    uint64_t flags = 0;
    if (argc >= 3) {
        flags = bender_getflags(argv, 2, "nact");
    }
    void *supervisor = bender_optabstract(argv, argc, 3, &bender_channel_type, bender_vm.root_fiber->supervisor_channel);
    if (NULL != supervisor) flags |= BENDER_THREAD_SUPERVISOR_FLAG;

    /* Marshal arguments for the new thread. */
    BenderBuffer *buffer = bender_malloc(sizeof(BenderBuffer));
    if (NULL == buffer) {
        BENDER_OUT_OF_MEMORY;
    }
    bender_buffer_init(buffer, 0);
    if (!(flags & 0x2)) {
        bender_marshal(buffer, bender_wrap_table(bender_vm.abstract_registry), NULL, BENDER_MARSHAL_UNSAFE);
    }
    if (flags & BENDER_THREAD_SUPERVISOR_FLAG) {
        bender_marshal(buffer, bender_wrap_abstract(supervisor), NULL, BENDER_MARSHAL_UNSAFE);
    }
    if (!(flags & 0x4)) {
        bender_assert(bender_vm.registry_count <= INT32_MAX, "assert failed size check");
        uint32_t temp = (uint32_t) bender_vm.registry_count;
        bender_buffer_push_bytes(buffer, (uint8_t *) &temp, sizeof(temp));
        bender_buffer_push_bytes(buffer, (uint8_t *) bender_vm.registry, (int32_t) bender_vm.registry_count * sizeof(BenderCFunRegistry));
    }
    bender_marshal(buffer, argv[0], NULL, BENDER_MARSHAL_UNSAFE);
    bender_marshal(buffer, value, NULL, BENDER_MARSHAL_UNSAFE);
    if (flags & 0x1) {
        /* Return immediately */
        BenderEVGenericMessage arguments;
        memset(&arguments, 0, sizeof(arguments));
        arguments.tag = (uint32_t) flags;
        arguments.argi = (uint32_t) bender_vm.sandbox_flags;
        arguments.argp = buffer;
        arguments.fiber = NULL;
        bender_ev_threaded_call(bender_go_thread_subr, arguments, bender_ev_default_threaded_callback);
        return bender_wrap_nil();
    } else {
        bender_ev_threaded_await(bender_go_thread_subr, (uint32_t) flags, (uint32_t) bender_vm.sandbox_flags, buffer);
    }
}

/* Phase 3 slice: value-returning thread/new + thread/join.
 * Share-nothing: args marshalled in, result marshalled back.
 * Detached worker + pipe completion via bender_ev_post_event.
 * Handle state only mutated on owner thread; join is owner-checked. */

typedef struct {
    BenderVM *owner;
    int done;
    int joined;
    int is_error;
    int is_daemon;   /* daemon threads never pin process exit (no ev
                      * refcount); joining still works if they complete */
    Bender result;
    Bender profile_rows;   /* Phase C: harvested worker profile rows (or nil) */
    BenderFiber *waiter;
    /* sched_id of waiter when it suspended: every schedule bumps it, so
     * a waiter whose id moved on was already resumed elsewhere (timeout
     * or cancel) and must not be delivered to. Distinguishes a live
     * previous joiner from a concurrent one. */
    uint32_t waiter_sched;
    /* M2 monitors: flat [chan-wrap tag] pairs, chan of each pair is a
     * channel abstract wrap, tag is an arbitrary value. Both entries are
     * gcrooted on register and unrooted on delivery (or handle free).
     * Only touched on the owner thread. */
    Bender *monitors;
    int32_t n_monitors;
    int32_t cap_monitors;
} BenderThreadHandle;

typedef struct {
    BenderThreadHandle *handle;
    uint8_t *bytes;
    size_t len;
    int is_error;
    uint8_t *pbytes;   /* Phase C: harvested profile rows (or NULL) */
    size_t plen;
} BenderThreadCompletion;

typedef struct {
    BenderBuffer *input;
    BenderThreadHandle *handle;
    BenderVM *owner;
    uint32_t flags;
    uint32_t sandbox_flags;
    int profile_active;  /* Phase C: boot with the collector on */
} BenderThreadNewArgs;

static int bender_thread_handle_gc(void *p, size_t len) {
    (void) len;
    BenderThreadHandle *h = (BenderThreadHandle *) p;
    /* Drop undelivered monitors (completion already ran if done, so
     * this only fires when the handle is collected first). Tombstoned
     * slots hold nil and were unrooted by ev/demonitor. */
    if (h->monitors != NULL) {
        for (int32_t i = 0; i < h->n_monitors * 2; i++) {
            if (!bender_checktype(h->monitors[i], BENDER_NIL)) {
                bender_gcunroot(h->monitors[i]);
            }
        }
        bender_free(h->monitors);
        h->monitors = NULL;
        h->n_monitors = 0;
        h->cap_monitors = 0;
    }
    return 0;
}

static int bender_thread_handle_mark(void *p, size_t len) {
    (void) len;
    BenderThreadHandle *h = (BenderThreadHandle *) p;
    bender_mark(h->result);
    bender_mark(h->profile_rows);
    if (h->waiter) {
        bender_mark(bender_wrap_fiber(h->waiter));
    }
    return 0;
}

const BenderAbstractType bender_thread_handle_type = {
    "core/thread-handle",
    bender_thread_handle_gc,
    bender_thread_handle_mark,
    NULL, /* get */
    NULL, /* put */
    NULL, /* marshal (refuse: handles stay on owner thread) */
    NULL, /* unmarshal */
    NULL, /* tostring */
    NULL, /* compare */
    NULL, /* hash */
    NULL, /* next */
    NULL, /* call */
    NULL, /* length */
    NULL, /* bytes */
    NULL  /* gcperthread */
};

/* M2: build one [:down tag reason] message. reason is :normal on
 * clean exit, the (marshaled-back) error value on failure. */
static Bender bender_thread_down_message(Bender tag, int is_error, Bender result) {
    Bender tup[3];
    tup[0] = bender_ckeywordv("down");
    tup[1] = tag;
    tup[2] = is_error ? result : bender_ckeywordv("normal");
    return bender_wrap_tuple(bender_tuple_n(tup, 3));
}

/* Best-effort delivery of one :down message (at-most-once: dropped
 * when the channel is closed or full). */
static void bender_thread_give_down(Bender chanv, Bender msg) {
    BenderChannel *chan = (BenderChannel *) bender_unwrap_abstract(chanv);
    if (chan->closed) return;
    bender_channel_give(chan, msg);
}

/* M2: deliver pending monitors after completion. Runs on the owner
 * event loop; each entry is unrooted and the list freed. Takes the
 * completion result explicitly: the waiter path below clears
 * h->result once a live joiner is resumed. */
static void bender_thread_deliver_monitors(BenderThreadHandle *h, int is_error, Bender result) {
    if (h->monitors == NULL) return;
    for (int32_t i = 0; i < h->n_monitors; i++) {
        Bender chanv = h->monitors[i * 2];
        Bender tag = h->monitors[i * 2 + 1];
        /* Tombstoned by ev/demonitor: already unrooted, skip. */
        if (bender_checktype(chanv, BENDER_NIL)) continue;
        bender_thread_give_down(chanv,
            bender_thread_down_message(tag, is_error, result));
        bender_gcunroot(chanv);
        bender_gcunroot(tag);
    }
    bender_free(h->monitors);
    h->monitors = NULL;
    h->n_monitors = 0;
    h->cap_monitors = 0;
}

static void bender_thread_new_complete(BenderEVGenericMessage msg) {
    BenderThreadCompletion *c = (BenderThreadCompletion *) msg.argp;
    BenderThreadHandle *h = c->handle;
    Bender result = bender_wrap_nil();
    int result_rooted = 0;
    if (c->bytes != NULL && c->len > 0) {
        result = bender_unmarshal(c->bytes, c->len, BENDER_MARSHAL_UNSAFE, NULL, NULL);
        /* result is freshly unmarshaled and lives only as a C local until
         * the barrier-protected store onto h below; root it explicitly for
         * this window rather than relying on h's own mark state to cover a
         * value it does not point to yet (closes the Phase 2b-i fiber-
         * rooting-chronology gap with the existing explicit-root API).
         * Skip immediates (nil included) to avoid needless roots-array
         * churn, matching bender_gc_barrier's own immediate-skip policy;
         * result_rooted records exactly what was rooted so the unroot
         * below stays symmetric regardless of what result later is. */
        if (!bender_checktype(result, BENDER_NIL)) {
            bender_gcroot(result);
            result_rooted = 1;
        }
        bender_free(c->bytes);
    } else if (c->bytes != NULL) {
        bender_free(c->bytes);
    }
    /* Phase C: harvested worker profile rows (nil when it profiled
     * nothing). Stored on the handle for thread/profile. */
    if (c->pbytes != NULL && c->plen > 0) {
        h->profile_rows = bender_unmarshal(c->pbytes, c->plen, BENDER_MARSHAL_UNSAFE, NULL, NULL);
        bender_gc_barrier(h, h->profile_rows);
        bender_free(c->pbytes);
    } else if (c->pbytes != NULL) {
        bender_free(c->pbytes);
    }
    int is_error = c->is_error;
    bender_free(c);
    h->result = result;
    bender_gc_barrier(h, h->result);
    if (result_rooted) {
        bender_gcunroot(result);
    }
    h->is_error = is_error;
    h->done = 1;
    /* A suspended join roots (handle, waiter) as a pair; unroot exactly.
     * Deliver only if the waiter is still suspended in our await
     * (schedule bumps sched_id, so a moved id means timeout/cancel
     * already resumed it elsewhere — keep the result for a later join
     * instead of scheduling a rogue resume). */
    if (h->waiter != NULL) {
        BenderFiber *w = h->waiter;
        uint32_t ws = h->waiter_sched;
        h->waiter = NULL;
        bender_gcunroot(bender_wrap_fiber(w));
        bender_gcunroot(bender_wrap_abstract(h));
        if (w->sched_id == ws) {
            h->joined = 1;
            h->result = bender_wrap_nil();
            if (h->is_error) {
                bender_cancel(w, result);
            } else {
                bender_schedule(w, result);
            }
        }
    }
    /* M2: monitors fire after join delivery so the join path is
     * unaffected; :down messages are best-effort (at-most-once). */
    bender_thread_deliver_monitors(h, is_error, result);
    if (!h->is_daemon) bender_ev_dec_refcount();
    /* Release worker ref last; may free handle (with result) if owner
     * dropped it. Do not touch h after this. */
    bender_abstract_decref_maybe_free(h);
}

/* Build a completion holding a malloc'd copy of marshalled bytes. */
static BenderThreadCompletion *bender_thread_make_completion(
    BenderThreadHandle *handle, uint8_t *bytes, size_t len, int is_error) {
    BenderThreadCompletion *c = bender_malloc(sizeof(BenderThreadCompletion));
    if (NULL == c) {
        BENDER_OUT_OF_MEMORY;
    }
    c->handle = handle;
    c->bytes = bytes;
    c->len = len;
    c->is_error = is_error;
    c->pbytes = NULL;
    c->plen = 0;
    return c;
}

/* Marshal v (worker VM) into a malloc'd byte copy for transfer. */
static void bender_thread_marshal_result_copy(Bender v, uint8_t **out_bytes, size_t *out_len) {
    BenderBuffer tmp;
    bender_buffer_init(&tmp, 0);
    bender_marshal(&tmp, v, NULL, BENDER_MARSHAL_UNSAFE);
    uint8_t *copy = NULL;
    if (tmp.count > 0) {
        copy = bender_malloc((size_t) tmp.count);
        if (NULL == copy) {
            bender_buffer_deinit(&tmp);
            BENDER_OUT_OF_MEMORY;
        }
        memcpy(copy, tmp.data, (size_t) tmp.count);
    }
    *out_bytes = copy;
    *out_len = (size_t) tmp.count;
    bender_buffer_deinit(&tmp);
}

static BenderThreadCompletion *bender_thread_new_run(BenderThreadNewArgs *targs) {
    BenderBuffer *buffer = targs->input;
    const uint8_t *nextbytes = buffer->data;
    const uint8_t *endbytes = nextbytes + buffer->count;
    uint32_t flags = targs->flags;
    BenderThreadHandle *handle = targs->handle;
    bender_init();
    bender_vm.sandbox_flags = targs->sandbox_flags;
    /* Phase C: inherit the collector when the owner was profiling. */
    if (targs->profile_active) bender_profile_start();
    BenderThreadCompletion *comp = NULL;
    BenderTryState tstate;
    BenderSignal signal = bender_try(&tstate);
    if (!signal) {
        /* Flag bits match the owner side (thread/new "acd"): :a skips
         * the abstract registry, :c skips the cfunction registry. */
        if (!(flags & 0x1)) {
            Bender aregv = bender_unmarshal(nextbytes, endbytes - nextbytes,
                BENDER_MARSHAL_UNSAFE, NULL, &nextbytes);
            bender_assert(bender_checktype(aregv, BENDER_TABLE), "expected table for abstract registry");
            bender_vm.abstract_registry = bender_unwrap_table(aregv);
            bender_gcroot(bender_wrap_table(bender_vm.abstract_registry));
        }
        if (!(flags & 0x2)) {
            uint32_t count1;
            memcpy(&count1, nextbytes, sizeof(count1));
            size_t count = (size_t) count1;
            bender_assert(count <= (size_t)(endbytes - nextbytes - sizeof(count1)) / sizeof(BenderCFunRegistry),
                "thread message invalid");
            bender_vm.registry_count = count;
            bender_vm.registry_cap = count;
            bender_vm.registry = bender_malloc(count * sizeof(BenderCFunRegistry));
            if (bender_vm.registry == NULL) {
                BENDER_OUT_OF_MEMORY;
            }
            bender_vm.registry_dirty = 1;
            nextbytes += sizeof(uint32_t);
            memcpy(bender_vm.registry, nextbytes, count * sizeof(BenderCFunRegistry));
            nextbytes += count * sizeof(BenderCFunRegistry);
        }
        Bender fiberv = bender_unmarshal(nextbytes, endbytes - nextbytes,
            BENDER_MARSHAL_UNSAFE, NULL, &nextbytes);
        Bender value = bender_unmarshal(nextbytes, endbytes - nextbytes,
            BENDER_MARSHAL_UNSAFE, NULL, &nextbytes);
        /* Input consumed; free before running (malloc only, no VM). */
        bender_buffer_deinit(buffer);
        bender_free(buffer);
        bender_free(targs);
        targs = NULL;
        buffer = NULL;
        BenderFiber *fiber;
        if (!bender_checktype(fiberv, BENDER_FIBER)) {
            bender_assert(bender_checktype(fiberv, BENDER_FUNCTION), "expected function or fiber");
            BenderFunction *func = bender_unwrap_function(fiberv);
            bender_assert(func->def->min_arity >= 0 && func->def->min_arity <= 1,
                "thread function must accept 0 or 1 arguments");
            fiber = bender_fiber(func, 64, func->def->min_arity, &value);
            bender_assert(fiber != NULL, "bad fiber in thread setup");
            /* Phase C: primed fibers bypass JOP_CALL; record the root
             * invocation so worker rows carry calls/params. */
            bender_profile_task_call(func, &value, 1);
            fiber->flags |=
                BENDER_FIBER_MASK_ERROR |
                BENDER_FIBER_MASK_USER0 |
                BENDER_FIBER_MASK_USER1 |
                BENDER_FIBER_MASK_USER2 |
                BENDER_FIBER_MASK_USER3 |
                BENDER_FIBER_MASK_USER4;
        } else {
            fiber = bender_unwrap_fiber(fiberv);
        }
        fiber->supervisor_channel = NULL;
        bender_schedule(fiber, value);
        bender_loop();
        BenderFiberStatus status = bender_fiber_status(fiber);
        Bender result = fiber->last_value;
        int is_error = 0;
        if (status == BENDER_STATUS_ERROR) {
            is_error = 1;
        } else if (status != BENDER_STATUS_DEAD) {
            result = bender_cstringv("thread did not complete (yielded)");
            is_error = 1;
        }
        /* Root fiber (hence last_value) across marshalling allocations. */
        Bender fiber_j = bender_wrap_fiber(fiber);
        Bender result_j = result;
        bender_gcroot(fiber_j);
        bender_gcroot(result_j);
        uint8_t *bytes = NULL;
        size_t len = 0;
        bender_thread_marshal_result_copy(result, &bytes, &len);
        bender_gcunroot(fiber_j);
        bender_gcunroot(result_j);
        comp = bender_thread_make_completion(handle, bytes, len, is_error);
        /* Phase C: harvest this worker's profile table (best-effort;
         * empty when profiling was off). The VM is still alive here. */
        bender_profile_harvest_bytes(&comp->pbytes, &comp->plen);
        bender_restore(&tstate);
    } else {
        bender_restore(&tstate);
        /* Startup or marshal failure: report payload if marshallable,
         * else a static fallback. */
        Bender payload = tstate.payload;
        uint8_t *bytes = NULL;
        size_t len = 0;
        Bender payload_j = payload;
        bender_gcroot(payload_j);
        BenderTryState tstate2;
        BenderSignal sig2 = bender_try(&tstate2);
        if (!sig2) {
            bender_thread_marshal_result_copy(payload, &bytes, &len);
            bender_restore(&tstate2);
        } else {
            bender_restore(&tstate2);
            Bender fallback = bender_cstringv("thread failed (unmarshallable error)");
            Bender fallback_j = fallback;
            bender_gcroot(fallback_j);
            bender_thread_marshal_result_copy(fallback, &bytes, &len);
            bender_gcunroot(fallback_j);
        }
        bender_gcunroot(payload_j);
        if (buffer != NULL) {
            bender_buffer_deinit(buffer);
            bender_free(buffer);
        }
        if (targs != NULL) {
            bender_free(targs);
        }
        comp = bender_thread_make_completion(handle, bytes, len, 1);
    }
    bender_deinit();
    return comp;
}

#ifdef BENDER_WINDOWS
static DWORD WINAPI bender_thread_new_worker(LPVOID ptr) {
    BenderThreadNewArgs *targs = (BenderThreadNewArgs *) ptr;
    BenderVM *owner = targs->owner;
    BenderThreadCompletion *comp = bender_thread_new_run(targs);
    BenderEVGenericMessage msg;
    memset(&msg, 0, sizeof(msg));
    msg.argp = comp;
    bender_ev_post_event(owner, bender_thread_new_complete, msg);
    return 0;
}
#else
static void *bender_thread_new_worker(void *ptr) {
    BenderThreadNewArgs *targs = (BenderThreadNewArgs *) ptr;
    BenderVM *owner = targs->owner;
    BenderThreadCompletion *comp = bender_thread_new_run(targs);
    BenderEVGenericMessage msg;
    memset(&msg, 0, sizeof(msg));
    msg.argp = comp;
    bender_ev_post_event(owner, bender_thread_new_complete, msg);
    return NULL;
}
#endif

BENDER_CORE_FN(cfun_thread_new,
              "(thread/new main &opt value flags)",
              "Run `main` in a new operating system thread and return a handle. "
              "`main` is a function taking 0 or 1 arguments, or a fiber; `value` is passed as the argument. "
              "The thread runs an isolated VM (share-nothing: args marshalled in, result marshalled back). "
              "Use `(thread/join handle)` to wait for completion and get the return value (re-raises thread errors). "
              "Flags: `:a` don't copy abstract registry, `:c` don't copy cfunction registry. "
              "`:d` daemon: the thread never pins process exit (no ev refcount); "
              "joining still works if it completes. Long-lived service threads "
              "(pools) should pass `:d`, or the event loop waits for them forever.") {
    bender_sandbox_assert(BENDER_SANDBOX_THREADS);
    bender_arity(argc, 1, 3);
    Bender value = argc >= 2 ? argv[1] : bender_wrap_nil();
    if (bender_checktype(argv[0], BENDER_FUNCTION)) {
        BenderFunction *func = bender_getfunction(argv, 0);
        if (func->def->arity < 0 || func->def->min_arity > 1) {
            bender_panic("function must take 0 or 1 arguments");
        }
    } else {
        bender_getfiber(argv, 0);
    }
    uint64_t flags = 0;
    if (argc >= 3) {
        flags = bender_getflags(argv, 2, "acd");
    }
    BenderBuffer *buffer = bender_malloc(sizeof(BenderBuffer));
    if (NULL == buffer) {
        BENDER_OUT_OF_MEMORY;
    }
    bender_buffer_init(buffer, 0);
    /* Flag bits follow bender_getflags order ("acd"): :a skips the
     * abstract registry, :c skips the cfunction registry, :d daemonizes
     * (no exit refcount — see below). */
    if (!(flags & 0x1)) {
        bender_marshal(buffer, bender_wrap_table(bender_vm.abstract_registry), NULL, BENDER_MARSHAL_UNSAFE);
    }
    if (!(flags & 0x2)) {
        bender_assert(bender_vm.registry_count <= INT32_MAX, "assert failed size check");
        uint32_t temp = (uint32_t) bender_vm.registry_count;
        bender_buffer_push_bytes(buffer, (uint8_t *) &temp, sizeof(temp));
        bender_buffer_push_bytes(buffer, (uint8_t *) bender_vm.registry,
            (int32_t) bender_vm.registry_count * sizeof(BenderCFunRegistry));
    }
    bender_marshal(buffer, argv[0], NULL, BENDER_MARSHAL_UNSAFE);
    bender_marshal(buffer, value, NULL, BENDER_MARSHAL_UNSAFE);
    BenderThreadHandle *handle = bender_abstract_threaded(&bender_thread_handle_type, sizeof(BenderThreadHandle));
    handle->owner = &bender_vm;
    handle->done = 0;
    handle->joined = 0;
    handle->is_error = 0;
    handle->is_daemon = (flags & 0x4) ? 1 : 0;
    handle->result = bender_wrap_nil();
    handle->profile_rows = bender_wrap_nil();
    handle->waiter = NULL;
    handle->waiter_sched = 0;
    handle->monitors = NULL;
    handle->n_monitors = 0;
    handle->cap_monitors = 0;
    BenderThreadNewArgs *targs = bender_malloc(sizeof(BenderThreadNewArgs));
    if (NULL == targs) {
        bender_buffer_deinit(buffer);
        bender_free(buffer);
        bender_panic("failed to allocate thread args");
    }
    targs->input = buffer;
    targs->handle = handle;
    targs->owner = &bender_vm;
    targs->flags = (uint32_t) flags;
    targs->sandbox_flags = bender_vm.sandbox_flags;
    /* Phase C: the worker inherits the collector when profiling. */
    targs->profile_active = (bender_vm.profile_table != NULL);
    bender_abstract_incref(handle);
    /* Daemon threads never pin process exit (long-lived service
     * threads would hold the event loop forever); joining still
     * works if they complete (the completion path skips its dec). */
    if (!handle->is_daemon) {
        bender_ev_inc_refcount();
    }
#ifdef BENDER_WINDOWS
    HANDLE wh = CreateThread(NULL, 0, bender_thread_new_worker, targs, 0, NULL);
    if (NULL == wh) {
        bender_abstract_decref_maybe_free(handle);
        if (!handle->is_daemon) bender_ev_dec_refcount();
        bender_buffer_deinit(buffer);
        bender_free(buffer);
        bender_free(targs);
        bender_panic("failed to create thread");
    }
    CloseHandle(wh);
#else
    pthread_t worker;
    int err = pthread_create(&worker, &bender_vm.new_thread_attr, bender_thread_new_worker, targs);
    if (err) {
        bender_abstract_decref_maybe_free(handle);
        if (!handle->is_daemon) bender_ev_dec_refcount();
        bender_buffer_deinit(buffer);
        bender_free(buffer);
        bender_free(targs);
        bender_panicf("%s", bender_strerror(err));
    }
#endif
    return bender_wrap_abstract(handle);
}

BENDER_CORE_FN(cfun_thread_join,
              "(thread/join handle &opt timeout)",
              "Wait for the thread `handle` created by `thread/new` to complete and return its value. "
              "Re-raises the thread's error if it failed. Must be called from the creating thread, exactly once. "
              "With `timeout` (seconds), raise a timeout error instead of waiting forever; a later join can "
              "still collect the result. Timeouts require joining from an ev task.") {
    bender_arity(argc, 1, 2);
    BenderThreadHandle *handle = bender_getabstract(argv, 0, &bender_thread_handle_type);
    if (handle->owner != &bender_vm) {
        bender_panic("thread/join called from non-owner thread");
    }
    if (handle->joined) {
        bender_panic("thread already joined");
    }
    if (bender_vm.coerce_error) {
        bender_panic("cannot join inside bender_call");
    }
    double timeout = bender_optnumber(argv, argc, 1, INFINITY);
    if (handle->done) {
        /* Fast path: completion already ran. */
        handle->joined = 1;
        Bender result = handle->result;
        handle->result = bender_wrap_nil();
        if (handle->is_error) {
            bender_panicv(result);
        }
        return result;
    }
    BenderFiber *root = bender_root_fiber();
    if (handle->waiter != NULL) {
        /* A waiter whose sched_id moved on was already resumed elsewhere
         * (timeout or cancel): release its roots exactly and take over.
         * An unmoved waiter is a genuine concurrent join. */
        if (handle->waiter->sched_id == handle->waiter_sched) {
            bender_panic("thread already being joined");
        }
        bender_gcunroot(bender_wrap_fiber(handle->waiter));
        bender_gcunroot(bender_wrap_abstract(handle));
        handle->waiter = NULL;
    }
    if (timeout != INFINITY &&
            !(root->gc.flags & BENDER_FIBER_EV_GCFLAG_ROOT)) {
        bender_panic("thread/join with timeout requires joining from an ev task");
    }
    handle->waiter = root;
    handle->waiter_sched = root->sched_id;
    /* Root across suspension; completion callback unroots
     * (mirrors bender_ev_threaded_await's fiber rooting). */
    bender_gcroot(bender_wrap_fiber(root));
    bender_gcroot(bender_wrap_abstract(handle));
    if (timeout != INFINITY) {
        bender_addtimeout(timeout);
    }
    bender_await();
    /* Unreachable when suspending; the completion schedules this fiber
     * with the thread's result (or cancels with its error). */
    return bender_wrap_nil();
}

BENDER_CORE_FN(cfun_thread_alive,
              "(thread/alive? handle)",
              "Return whether the thread `handle` created by `thread/new` is "
              "still running. Once true it stays true until `thread/join`; "
              "after `thread/join` returns false. Must be called from the "
              "creating thread.") {
    bender_fixarity(argc, 1);
    BenderThreadHandle *handle = bender_getabstract(argv, 0, &bender_thread_handle_type);
    if (handle->owner != &bender_vm) {
        bender_panic("thread/alive? called from non-owner thread");
    }
    return bender_wrap_boolean(!handle->done);
}

BENDER_CORE_FN(cfun_thread_profile,
              "(thread/profile handle)",
              "Return the worker's harvested profile rows (Phase C dump "
              "format) after the thread completed, or nil when it profiled "
              "nothing. Workers inherit the collector when the owner is "
              "profiling at thread/new time. Must be called from the "
              "creating thread.") {
    bender_fixarity(argc, 1);
    BenderThreadHandle *handle = bender_getabstract(argv, 0, &bender_thread_handle_type);
    if (handle->owner != &bender_vm) {
        bender_panic("thread/profile called from non-owner thread");
    }
    return handle->profile_rows;
}

/* M4c: pooled warm-VM task executors (C runtime). M one-shot tasks run
 * over N long-lived worker VMs sharing one condvar queue: no per-task
 * spawn cost (cf. thread/new, which boots a VM per call), one hop, and
 * direct completion events for pool/call (no per-task reply channel).
 * Share-nothing: [func args] go in and results come back via UNSAFE
 * marshal copies. Fibers stay pinned: workers are 1:1 pthreads with
 * isolated VMs (registries marshalled once at spawn, exactly like
 * thread/new); only tasks move. This is M:N at job level — never
 * migrating fibers. Cooperative like the rest of the layer: each task
 * runs under a per-task bender_loop (so ev/sleep works inside tasks),
 * but a task that suspends forever wedges its worker.
 *
 * Suspension discipline: every blocking cfun below has at most ONE
 * trailing suspension (the proven take/join shape — code after
 * bender_await never runs). Capacity tokens are therefore acquired in a
 * separate step (pool/acquire, sequenced from Bender): the token take is
 * its suspension, the result wait is dispatch's.
 *
 * Lifetime: the pool abstract is gcrooted at spawn and unrooted at
 * stop, so the struct always outlives its workers (workers hold no
 * pool refs). pool/stop joins every worker, then frees the internals;
 * the type finalizer only handles the abandoned case (sets stopping +
 * broadcasts; the last exiting worker frees the contents). Abandoning
 * a pool without pool/stop leaks its threads until process exit —
 * documented, same class as leaked threads. */

typedef struct BenderPoolTask {
    uint8_t *bytes;          /* UNSAFE-marshalled [func args] tuple, malloc'd */
    size_t len;
    BenderVM *owner;          /* completion target */
    BenderFiber *waiter;      /* pool/call path (direct completion), else NULL */
    uint32_t waiter_sched;
    void *reply;             /* pool/submit path: incref'd threaded channel, else NULL */
    struct BenderPoolTask *next;
} BenderPoolTask;

/* Phase C: per-worker profile harvest. Workers with profiling active
 * serialize their tables at exit into UNSAFE-marshal buffers; the
 * owner drains them via pool/profile-harvest (merged Bender-side by
 * tools/profile-merge.ben). Malloc'd, pool-locked, freed on drain
 * or with the pool. */
typedef struct BenderProfileHarvest {
    uint8_t *bytes;
    size_t len;
    struct BenderProfileHarvest *next;
} BenderProfileHarvest;

typedef struct {
#ifdef BENDER_WINDOWS
    CRITICAL_SECTION lock;
    CONDITION_VARIABLE nonempty;
#else
    pthread_mutex_t lock;
    pthread_cond_t nonempty;
#endif
    BenderPoolTask *head, *tail;
    int32_t count;
    int stopping;            /* drain, then exit (stop or finalizer) */
    int joined;              /* stop() completed: workers joined, contents freed */
    int contents_freed;
    int32_t n_workers;
    int32_t live_workers;
    BenderVM *owner;
    uint32_t flags;
    uint32_t sandbox_flags;
    uint8_t *prologue;       /* marshalled registries, immutable after spawn */
    size_t prologue_len;
    BenderChannel *tokens;    /* capacity tokens (pool-incref'd, never rooted) */
    BenderFiber *stop_waiter; /* pool/stop waiter (single), else NULL */
    uint32_t stop_sched;
    Bender self;              /* gcrooted wrapper: pins the struct until stop */
    int profile_active;      /* Phase C: workers boot with the collector on */
    BenderProfileHarvest *harvest_head, *harvest_tail;
#ifdef BENDER_WINDOWS
    HANDLE *threads;
#else
    pthread_t *threads;
#endif
} BenderPool;

static void bender_pool_lock(BenderPool *pool) {
#ifdef BENDER_WINDOWS
    EnterCriticalSection(&pool->lock);
#else
    pthread_mutex_lock(&pool->lock);
#endif
}

static void bender_pool_unlock(BenderPool *pool) {
#ifdef BENDER_WINDOWS
    LeaveCriticalSection(&pool->lock);
#else
    pthread_mutex_unlock(&pool->lock);
#endif
}

static void bender_pool_broadcast(BenderPool *pool) {
#ifdef BENDER_WINDOWS
    WakeAllConditionVariable(&pool->nonempty);
#else
    pthread_cond_broadcast(&pool->nonempty);
#endif
}

static void bender_pool_signal(BenderPool *pool) {
#ifdef BENDER_WINDOWS
    WakeConditionVariable(&pool->nonempty);
#else
    pthread_cond_signal(&pool->nonempty);
#endif
}

/* Owner-loop callback: the last worker exited. Join (all already dead,
 * so immediate), free the contents, wake the pool/stop waiter. Runs on
 * the owner event loop — the only place teardown happens. */
static void bender_pool_all_exited(BenderEVGenericMessage msg);

static void bender_pool_sync_destroy(BenderPool *pool) {
#ifdef BENDER_WINDOWS
    /* CONDITION_VARIABLE needs no destroy; CRITICAL_SECTION does. */
    DeleteCriticalSection(&pool->lock);
#else
    pthread_cond_destroy(&pool->nonempty);
    pthread_mutex_destroy(&pool->lock);
#endif
}

/* Free queue/tasks/prologue/threads (not the abstract itself). Runs
 * exactly once: by pool/stop (workers joined) or by the last exiting
 * worker (abandoned path). Callers must exclude each other via the
 * pool lock + contents_freed flag, except stop which is exclusive by
 * construction (owner-only, single stop). */
static void bender_pool_free_contents(BenderPool *pool) {
    BenderPoolTask *t = pool->head;
    while (t != NULL) {
        BenderPoolTask *next = t->next;
        /* Abandon-path remnants: tasks the drain never ran. Their reply
         * channels keep them safe (worker incref pairs with this decref);
         * direct-completion waiters are gone with the owner flow. */
        if (t->reply != NULL) bender_abstract_decref_maybe_free(t->reply);
        if (t->bytes != NULL) bender_free(t->bytes);
        bender_free(t);
        t = next;
    }
    pool->head = pool->tail = NULL;
    if (pool->prologue != NULL) {
        bender_free(pool->prologue);
        pool->prologue = NULL;
    }
    if (pool->threads != NULL) {
        /* Thread handles are joined (stop) or abandoned with the leak
         * (documented): never closed here. Windows HANDLEs are closed
         * by pool/stop after joining. */
        bender_free(pool->threads);
        pool->threads = NULL;
    }
    /* Drop the pool's own token ref; the wrapper-owned ref dies with
     * the tokens wrapper. */
    if (pool->tokens != NULL) {
        bender_abstract_decref_maybe_free(pool->tokens);
        pool->tokens = NULL;
    }
    /* Phase C: unread harvest remnants (readers drain via
     * pool/profile-harvest; leftovers die with the pool). */
    {
        BenderProfileHarvest *h = pool->harvest_head;
        while (h != NULL) {
            BenderProfileHarvest *next = h->next;
            if (h->bytes != NULL) bender_free(h->bytes);
            bender_free(h);
            h = next;
        }
        pool->harvest_head = pool->harvest_tail = NULL;
    }
    pool->contents_freed = 1;
}

static int bender_pool_gc(void *p, size_t len) {
    (void) len;
    BenderPool *pool = (BenderPool *) p;
    /* The wrapper is unreachable: no Bender code can name this pool
     * anymore. Joined pools (and spawn-failure leftovers, already
     * joined on the panic path) have no threads: free without the
     * lock held — destroying a locked mutex is UB, and nothing else
     * can run here. The live case (abandonment) never unroots, so it
     * cannot reach here; if it ever does, only ask for a drain and
     * leave the documented leak rather than freeing under workers. */
    bender_pool_lock(pool);
    if (!pool->joined && pool->live_workers != 0) {
        pool->stopping = 1;
        bender_pool_broadcast(pool);
        bender_pool_unlock(pool);
        return 0;
    }
    int do_free = !pool->contents_freed;
    bender_pool_unlock(pool);
    if (do_free) {
        bender_pool_free_contents(pool);
        bender_pool_sync_destroy(pool);
    }
    return 0;
}

static int bender_pool_mark(void *p, size_t len) {
    (void) len;
    (void) p;
    /* Tasks reference no owner-VM heap objects (all payloads are
     * malloc'd marshal buffers); waiters/replies are rooted per-call. */
    return 0;
}

const BenderAbstractType bender_pool_type = {
    "core/pool",
    bender_pool_gc,
    bender_pool_mark,
    NULL, /* get */
    NULL, /* put */
    NULL, /* marshal (refuse: pools stay on the owner thread) */
    NULL, /* unmarshal */
    NULL, /* tostring */
    NULL, /* compare */
    NULL, /* hash */
    NULL, /* next */
    NULL, /* call */
    NULL, /* length */
    NULL, /* bytes */
    NULL  /* gcperthread */
};

/* Best-effort reply give (at-most-once: dropped when closed). The reply
 * channel has limit 1 and takes exactly one give, so mode 2 only fires
 * on close — which we pre-check to avoid panicking on the owner loop. */
static void bender_pool_give_reply(BenderChannel *chan, Bender msg) {
    if (chan->closed) return;
    bender_channel_push(chan, msg, 2);
}

typedef struct {
    BenderFiber *waiter;      /* direct-completion waiter, else NULL */
    uint32_t waiter_sched;
    BenderChannel *reply;     /* reply channel (borrowed ref), else NULL */
    uint8_t *bytes;
    size_t len;
    int is_error;
} BenderPoolCompletion;

static void bender_pool_complete(BenderEVGenericMessage msg) {
    BenderPoolCompletion *c = (BenderPoolCompletion *) msg.argp;
    Bender result = bender_wrap_nil();
    if (c->bytes != NULL && c->len > 0) {
        result = bender_unmarshal(c->bytes, c->len, BENDER_MARSHAL_UNSAFE, NULL, NULL);
        bender_free(c->bytes);
    } else if (c->bytes != NULL) {
        bender_free(c->bytes);
    }
    /* Same exactly-once waiter discipline as thread/join: deliver only
     * if the waiter is still suspended in our await (a moved sched_id
     * means timeout/cancel already resumed it — drop the value). */
    if (c->waiter != NULL) {
        BenderFiber *w = c->waiter;
        uint32_t ws = c->waiter_sched;
        c->waiter = NULL;
        bender_gcunroot(bender_wrap_fiber(w));
        if (w->sched_id == ws) {
            if (c->is_error) {
                bender_cancel(w, result);
            } else {
                bender_schedule(w, result);
            }
        } else {
        }
    }
    if (c->reply != NULL) {
        /* Submit path: exactly one give into a limit-1 channel. The
         * task held a ref so the channel is alive here; drop ours. */
        bender_pool_give_reply(c->reply, c->is_error
            ? bender_wrap_tuple(bender_tuple_n((Bender[]) {
                bender_ckeywordv("pool-error"), result }, 2))
            : result);
        bender_abstract_decref_maybe_free(c->reply);
    }
    bender_free(c);
    bender_ev_dec_refcount();
}

/* Boot one worker VM from the shared prologue (registries), mirroring
 * bender_thread_new_run's registry section. Returns NULL with the task
 * fiber set up, or aborts on corrupt prologue (same fail-loud contract
 * as thread/new). */
typedef struct {
    BenderPool *pool;
    int index;
} BenderPoolWorkerArgs;

/* Phase C: harvest this VM's profile table (if active) into malloc'd
 * UNSAFE-marshal bytes of the dump row array. Returns 1 when harvested
 * (caller frees *out_bytes). Runs on the harvesting VM; the bytes cross
 * threads like any completion payload. */
static int bender_profile_harvest_bytes(uint8_t **out_bytes, size_t *out_len) {
    BenderArray *rows;
    Bender rows_j;
    BenderBuffer tmp;
    uint8_t *copy;
    if (NULL == bender_vm.profile_table) return 0;
    rows = bender_profile_rows();
    rows_j = bender_wrap_array(rows);
    bender_gcroot(rows_j);
    bender_buffer_init(&tmp, 0);
    bender_marshal(&tmp, rows_j, NULL, BENDER_MARSHAL_UNSAFE);
    copy = NULL;
    if (tmp.count > 0) {
        /* Best-effort: teardown callers must never panic (a panic
         * with the pool lock held wedges stop), so OOM drops the
         * evidence instead of raising. */
        copy = bender_malloc((size_t) tmp.count);
        if (NULL != copy) {
            memcpy(copy, tmp.data, (size_t) tmp.count);
        } else {
            bender_buffer_deinit(&tmp);
            bender_gcunroot(rows_j);
            *out_bytes = NULL;
            *out_len = 0;
            return 0;
        }
    }
    *out_bytes = copy;
    *out_len = (size_t) tmp.count;
    bender_buffer_deinit(&tmp);
    bender_gcunroot(rows_j);
    return 1;
}

static void bender_pool_worker_boot(BenderPool *pool) {
    bender_init();
    bender_vm.sandbox_flags = pool->sandbox_flags;
    /* Phase C: inherit the collector when the owner was profiling at
     * spawn. Per-VM table; harvested at worker exit below. */
    if (pool->profile_active) bender_profile_start();
    const uint8_t *nextbytes = pool->prologue;
    const uint8_t *endbytes = nextbytes + pool->prologue_len;
    uint32_t flags = pool->flags;
    /* Same numbering as the owner side ("ac"): :a skips abstract, :c
     * skips cfunction (previously off by one on both sides). */
    if (!(flags & 0x1)) {
        Bender aregv = bender_unmarshal(nextbytes, endbytes - nextbytes,
            BENDER_MARSHAL_UNSAFE, NULL, &nextbytes);
        bender_assert(bender_checktype(aregv, BENDER_TABLE), "pool prologue: abstract registry");
        bender_vm.abstract_registry = bender_unwrap_table(aregv);
        bender_gcroot(bender_wrap_table(bender_vm.abstract_registry));
    }
    if (!(flags & 0x2)) {
        uint32_t count1;
        memcpy(&count1, nextbytes, sizeof(count1));
        size_t count = (size_t) count1;
        bender_assert(count <= (size_t)(endbytes - nextbytes - sizeof(count1)) / sizeof(BenderCFunRegistry),
            "pool prologue: registry bounds");
        bender_vm.registry_count = count;
        bender_vm.registry_cap = count;
        bender_vm.registry = bender_malloc(count * sizeof(BenderCFunRegistry));
        if (bender_vm.registry == NULL) {
            BENDER_OUT_OF_MEMORY;
        }
        bender_vm.registry_dirty = 1;
        nextbytes += sizeof(uint32_t);
        memcpy(bender_vm.registry, nextbytes, count * sizeof(BenderCFunRegistry));
    }
}

/* Run one task payload ([func args] tuple) to completion in this worker
 * VM under a per-task bender_loop (so ev/sleep works inside tasks).
 * Returns the result value; sets *is_error on task failure. */
static Bender bender_pool_run_task(Bender taskv, int *is_error) {
    *is_error = 0;
    const Bender *pair = bender_unwrap_tuple(taskv);
    Bender funcv = pair[0];
    Bender argsv = pair[1];
    BenderFiber *fiber;
    if (bender_checktype(funcv, BENDER_FIBER)) {
        /* Fibers resume with the args array as the single resume value
         * (same single-value shape as thread/new). */
        fiber = bender_unwrap_fiber(funcv);
        bender_schedule(fiber, argsv);
    } else {
        BenderFunction *func = bender_unwrap_function(funcv);
        const Bender *argdata = NULL;
        int32_t nargs = 0;
        if (bender_checktype(argsv, BENDER_ARRAY)) {
            nargs = bender_unwrap_array(argsv)->count;
            argdata = bender_unwrap_array(argsv)->data;
        } else {
            nargs = bender_tuple_length(bender_unwrap_tuple(argsv));
            argdata = bender_unwrap_tuple(argsv);
        }
        fiber = bender_fiber(func, 64, nargs, argdata);
        bender_assert(fiber != NULL, "pool task: bad fiber");
        /* Phase C: primed fibers bypass JOP_CALL, so record the root
         * invocation explicitly (else rows show calls=0, no params). */
        bender_profile_task_call(func, argdata, nargs);
        bender_schedule(fiber, bender_wrap_nil());
    }
    fiber->supervisor_channel = NULL;
    bender_loop();
    BenderFiberStatus status = bender_fiber_status(fiber);
    if (status == BENDER_STATUS_ERROR) {
        *is_error = 1;
        return fiber->last_value;
    }
    if (status != BENDER_STATUS_DEAD) {
        *is_error = 1;
        return bender_cstringv("pool task did not complete (yielded)");
    }
    return fiber->last_value;
}

#ifdef BENDER_WINDOWS
static DWORD WINAPI bender_pool_worker(LPVOID ptr) {
#else
static void *bender_pool_worker(void *ptr) {
#endif
    BenderPoolWorkerArgs *wargs = (BenderPoolWorkerArgs *) ptr;
    BenderPool *pool = wargs->pool;
    bender_free(wargs);
    bender_pool_worker_boot(pool);
    for (;;) {
        BenderPoolTask *task;
        bender_pool_lock(pool);
        while (pool->head == NULL && !pool->stopping) {
#ifdef BENDER_WINDOWS
            SleepConditionVariableCS(&pool->nonempty, &pool->lock, INFINITE);
#else
            pthread_cond_wait(&pool->nonempty, &pool->lock);
#endif
        }
        /* Drain-then-exit: take remaining tasks while stopping, but
         * leave once the queue is empty. */
        if (pool->head == NULL) {
            /* stopping && empty: exit. Teardown belongs to the
             * all-exited callback (pool/stop joins there); the last
             * exiting worker only reports. The abandoned path leaks
             * by design (documented) since the pool wrapper stays
             * gcrooted until stop. */
            /* Phase C: harvest this worker's profile table first (the
             * VM is still alive; the bytes wait in the pool). The pool
             * lock is already held here: link directly, no re-lock. */
            {
                uint8_t *hbytes = NULL;
                size_t hlen = 0;
                if (bender_profile_harvest_bytes(&hbytes, &hlen)) {
                    BenderProfileHarvest *h = bender_malloc(sizeof(BenderProfileHarvest));
                    if (NULL == h) {
                        /* Best-effort (see above): drop, keep exiting. */
                        if (hbytes != NULL) bender_free(hbytes);
                    } else {
                        h->bytes = hbytes;
                        h->len = hlen;
                        h->next = NULL;
                        if (pool->harvest_tail != NULL) {
                            pool->harvest_tail->next = h;
                        } else {
                            pool->harvest_head = h;
                        }
                        pool->harvest_tail = h;
                    }
                }
            }
            pool->live_workers--;
            int last = (pool->live_workers == 0);
            bender_pool_unlock(pool);
            if (last) {
                BenderEVGenericMessage msg;
                memset(&msg, 0, sizeof(msg));
                msg.argp = pool;
                /* No refcount here (thread-local counters): pool/stop
                 * inc'd on the owner; the callback decs there. */
                bender_ev_post_event(pool->owner, bender_pool_all_exited, msg);
            }
            bender_deinit();
#ifdef BENDER_WINDOWS
            return 0;
#else
            return NULL;
#endif
        }
        task = pool->head;
        pool->head = task->next;
        if (pool->head == NULL) pool->tail = NULL;
        pool->count--;
        bender_pool_unlock(pool);
        /* Run + complete. The capacity token is held for the task's
         * whole lifetime and handed back after completion posts: that
         * is what bounds memory (a wedged worker pins its capacity).
         * Total tokens are constant, so the give always fits. */
        Bender taskv = bender_unmarshal(task->bytes, task->len,
            BENDER_MARSHAL_UNSAFE, NULL, NULL);
        bender_free(task->bytes);
        task->bytes = NULL;
        Bender taskv_j = taskv;
        bender_gcroot(taskv_j);
        int is_error = 0;
        BenderTryState tstate;
        Bender result = bender_wrap_nil();
        BenderSignal sig = bender_try(&tstate);
        if (!sig) {
            result = bender_pool_run_task(taskv, &is_error);
            bender_restore(&tstate);
        } else {
            bender_restore(&tstate);
            result = tstate.payload;
            is_error = 1;
        }
        Bender result_j = result;
        bender_gcroot(result_j);
        uint8_t *bytes = NULL;
        size_t len = 0;
        BenderTryState tstate2;
        BenderSignal sig2 = bender_try(&tstate2);
        if (!sig2) {
            bender_thread_marshal_result_copy(result, &bytes, &len);
            bender_restore(&tstate2);
        } else {
            bender_restore(&tstate2);
            Bender fallback = bender_cstringv("pool result failed to marshal");
            Bender fallback_j = fallback;
            bender_gcroot(fallback_j);
            bender_thread_marshal_result_copy(fallback, &bytes, &len);
            bender_gcunroot(fallback_j);
            is_error = 1;
        }
        bender_gcunroot(taskv_j);
        bender_gcunroot(result_j);
        BenderPoolCompletion *comp = bender_malloc(sizeof(BenderPoolCompletion));
        if (NULL == comp) {
            BENDER_OUT_OF_MEMORY;
        }
        comp->waiter = task->waiter;
        comp->waiter_sched = task->waiter_sched;
        comp->reply = task->reply;
        comp->bytes = bytes;
        comp->len = len;
        comp->is_error = is_error;
        BenderVM *owner = task->owner;
        bender_free(task);
        BenderEVGenericMessage msg;
        memset(&msg, 0, sizeof(msg));
        msg.argp = comp;
        /* No refcount here: the matching inc ran on the owner at
         * dispatch (refcounts are thread-local; worker-inc/owner-dec
         * would strand the worker's counter and wedge its next loop). */
        bender_ev_post_event(owner, bender_pool_complete, msg);
        /* Hand the capacity token back now that the task is complete. */
        bender_channel_push(pool->tokens, bender_wrap_nil(), 2);
    }
}

static void bender_pool_all_exited(BenderEVGenericMessage msg) {
    BenderPool *pool = (BenderPool *) msg.argp;
    /* Join first (all workers already exited: immediate, loop unaffected).
     * Teardown of the internals happens in the type finalizer, not here:
     * the pool struct (and its lock) must stay mapped while any handle
     * Bender exists, so use-after-stop always fails loud via the flags
     * instead of touching freed memory. */
#ifdef BENDER_WINDOWS
    for (int32_t i = 0; i < pool->n_workers; i++) {
        if (pool->threads[i] != NULL) {
            WaitForSingleObject(pool->threads[i], INFINITE);
            CloseHandle(pool->threads[i]);
            pool->threads[i] = NULL;
        }
    }
#else
    for (int32_t i = 0; i < pool->n_workers; i++) {
        pthread_join(pool->threads[i], NULL);
    }
#endif
    bender_pool_lock(pool);
    pool->joined = 1;
    BenderFiber *w = pool->stop_waiter;
    uint32_t ws = pool->stop_sched;
    pool->stop_waiter = NULL;
    bender_pool_unlock(pool);
    /* Unpin the wrapper now that no thread can touch the pool. */
    bender_gcunroot(pool->self);
    pool->self = bender_wrap_nil();
    /* Wake the stopper unless it moved on (cancel); teardown stands. */
    if (w != NULL) {
        bender_gcunroot(bender_wrap_fiber(w));
        if (w->sched_id == ws) {
            bender_schedule(w, bender_wrap_nil());
        }
    }
    bender_ev_dec_refcount();
}

static BenderPool *bender_getpool(const Bender *argv, int32_t n) {
    BenderPool *pool = bender_getabstract(argv, n, &bender_pool_type);
    if (pool->owner != &bender_vm) {
        bender_panic("pool used from non-owner thread");
    }
    return pool;
}

BENDER_CORE_FN(cfun_pool_spawn,
              "(pool/spawn nthreads &opt capacity flags)",
              "Start a pool of `nthreads` worker VMs sharing one task queue "
              "and return a pool handle. `capacity` (default 1024) bounds "
              "queued tasks; submitters block when full. Workers run an "
              "isolated VM each (share-nothing, registries copied once at "
              "spawn like thread/new). Flags `:a`/`:c` skip the abstract / "
              "cfunction registry copies. Use pool/call, pool/submit, "
              "pool/map to run tasks and pool/stop to drain and shut down "
              "(required: an unstopped pool leaks its threads). Must be "
              "called from the owning thread.") {
    bender_sandbox_assert(BENDER_SANDBOX_THREADS);
    bender_arity(argc, 1, 3);
    int32_t nthreads = bender_getinteger(argv, 0);
    if (nthreads <= 0 || nthreads > 1024) {
        bender_panic("pool thread count must be between 1 and 1024");
    }
    int32_t capacity = 1024;
    if (argc >= 2 && !bender_checktype(argv[1], BENDER_NIL)) {
        capacity = bender_getinteger(argv, 1);
    }
    if (capacity <= 0 || capacity > 0xFFFFFF) {
        bender_panic("pool capacity must be between 1 and 16777215");
    }
    uint64_t flags = 0;
    if (argc >= 3) {
        flags = bender_getflags(argv, 2, "ac");
    }
    BenderPool *pool = bender_abstract_threaded(&bender_pool_type, sizeof(BenderPool));
    memset(pool, 0, sizeof(BenderPool));
#ifdef BENDER_WINDOWS
    InitializeCriticalSection(&pool->lock);
    InitializeConditionVariable(&pool->nonempty);
#else
    if (pthread_mutex_init(&pool->lock, NULL)) {
        bender_panic("pool mutex init failed");
    }
    if (pthread_cond_init(&pool->nonempty, NULL)) {
        pthread_mutex_destroy(&pool->lock);
        bender_panic("pool condvar init failed");
    }
#endif
    pool->owner = &bender_vm;
    pool->flags = (uint32_t) flags;
    pool->sandbox_flags = bender_vm.sandbox_flags;
    pool->n_workers = nthreads;
    pool->live_workers = nthreads;
    /* Phase C: workers inherit the collector when the owner is
     * profiling at spawn (harvested at worker exit, read via
     * pool/profile-harvest). */
    pool->profile_active = (bender_vm.profile_table != NULL);
    /* Prologue: registries marshalled once, shared read-only (same
     * layout thread/new builds per thread). Flag bits follow
     * bender_getflags order ("ac"): :a skips abstract, :c skips
     * cfunction (previously off by one — :a matched nothing, :c
     * skipped the wrong registry; existing pins were insensitive). */
    {
        BenderBuffer prologue;
        bender_buffer_init(&prologue, 0);
        if (!(flags & 0x1)) {
            bender_marshal(&prologue, bender_wrap_table(bender_vm.abstract_registry), NULL, BENDER_MARSHAL_UNSAFE);
        }
        if (!(flags & 0x2)) {
            bender_assert(bender_vm.registry_count <= INT32_MAX, "assert failed size check");
            uint32_t temp = (uint32_t) bender_vm.registry_count;
            bender_buffer_push_bytes(&prologue, (uint8_t *) &temp, sizeof(temp));
            bender_buffer_push_bytes(&prologue, (uint8_t *) bender_vm.registry,
                (int32_t) bender_vm.registry_count * sizeof(BenderCFunRegistry));
        }
        if (prologue.count > 0) {
            pool->prologue = bender_malloc((size_t) prologue.count);
            if (NULL == pool->prologue) {
                bender_buffer_deinit(&prologue);
                bender_pool_sync_destroy(pool);
                BENDER_OUT_OF_MEMORY;
            }
            memcpy(pool->prologue, prologue.data, (size_t) prologue.count);
            pool->prologue_len = (size_t) prologue.count;
        }
        bender_buffer_deinit(&prologue);
    }
    /* Capacity tokens: constant supply; submit takes, workers return. */
    pool->tokens = bender_channel_make_threaded((uint32_t) capacity);
    bender_abstract_incref(pool->tokens);
    for (int32_t i = 0; i < capacity; i++) {
        bender_channel_push(pool->tokens, bender_wrap_nil(), 2);
    }
    /* Pin the wrapper until stop: workers must never see a freed pool. */
    pool->self = bender_wrap_abstract(pool);
    bender_gcroot(pool->self);
#ifdef BENDER_WINDOWS
    pool->threads = bender_malloc((size_t) nthreads * sizeof(HANDLE));
#else
    pool->threads = bender_malloc((size_t) nthreads * sizeof(pthread_t));
#endif
    if (NULL == pool->threads) {
        bender_pool_free_contents(pool);
        bender_pool_sync_destroy(pool);
        bender_gcunroot(pool->self);
        BENDER_OUT_OF_MEMORY;
    }
    memset(pool->threads, 0, (size_t) nthreads * sizeof(pool->threads[0]));
    for (int32_t i = 0; i < nthreads; i++) {
        BenderPoolWorkerArgs *wargs = bender_malloc(sizeof(BenderPoolWorkerArgs));
        if (NULL == wargs) {
            pool->stopping = 1;
            bender_pool_broadcast(pool);
            goto pool_spawn_failed;
        }
        wargs->pool = pool;
        wargs->index = i;
#ifdef BENDER_WINDOWS
        pool->threads[i] = CreateThread(NULL, 0, bender_pool_worker, wargs, 0, NULL);
        if (NULL == pool->threads[i]) goto pool_spawn_failed;
#else
        int err = pthread_create(&pool->threads[i], NULL, bender_pool_worker, wargs);
        if (err) goto pool_spawn_failed;
#endif
        continue;
pool_spawn_failed:
        if (wargs != NULL) bender_free(wargs);
        /* Reap the workers that did start (they exit on stopping with
         * an empty queue), then release everything and raise. */
        pool->stopping = 1;
        bender_pool_broadcast(pool);
        for (int32_t j = 0; j < i; j++) {
#ifdef BENDER_WINDOWS
            WaitForSingleObject(pool->threads[j], INFINITE);
            CloseHandle(pool->threads[j]);
#else
            pthread_join(pool->threads[j], NULL);
#endif
        }
        bender_pool_free_contents(pool);
        bender_pool_sync_destroy(pool);
        bender_gcunroot(pool->self);
        bender_panic("pool failed to start workers");
    }
    return bender_wrap_abstract(pool);
}

BENDER_CORE_FN(cfun_pool_acquire,
              "(pool/acquire p)",
              "Take one capacity token from pool `p`, blocking while the "
              "queue is full. Internal building block for pool/call and "
              "pool/submit: every acquire must be followed by exactly one "
              "pool/dispatch (the wrappers do this back-to-back with no "
              "suspension between, so the token cannot strand).") {
    bender_fixarity(argc, 1);
    BenderPool *pool = bender_getpool(argv, 0);
    if (bender_vm.coerce_error) {
        bender_panic("cannot acquire pool token inside bender_call");
    }
    /* Fail loud on dead pools before touching the token channel (its
     * memory is released at wrapper collection). */
    bender_pool_lock(pool);
    int dead = pool->stopping || pool->joined;
    bender_pool_unlock(pool);
    if (dead) {
        bender_panic("pool is stopping");
    }
    Bender item;
    if (bender_channel_pop(pool->tokens, &item, 0)) {
        bender_schedule(bender_vm.root_fiber, item);
    }
    bender_await();
}

BENDER_CORE_FN(cfun_pool_dispatch,
              "(pool/dispatch p func args kind &opt timeout)",
              "Enqueue `[func args]` on pool `p` (caller must hold a token "
              "from pool/acquire) and, for `:call`, wait for the result. "
              "`func` takes N arguments (fibers resume with the args array "
              "as one value); `args` is an array or tuple. `:submit` "
              "returns a 1-slot reply channel instead of waiting; task "
              "errors arrive as `[:pool-error err]` and pool/call re-raises "
              "them in the caller. With `timeout` (seconds, `:call` only), "
              "raise a timeout error instead of waiting; the late result "
              "is dropped. Panics if the pool is stopping (the token is "
              "handed back first).") {
    bender_arity(argc, 4, 5);
    BenderPool *pool = bender_getpool(argv, 0);
    if (bender_vm.coerce_error) {
        bender_panic("cannot dispatch pool task inside bender_call");
    }
    int is_call;
    if (bender_checktype(argv[3], BENDER_KEYWORD)) {
        const uint8_t *kw = bender_unwrap_keyword(argv[3]);
        if (!strcmp((const char *) kw, "call")) {
            is_call = 1;
        } else if (!strcmp((const char *) kw, "submit")) {
            is_call = 0;
        } else {
            bender_panic("pool kind must be :call or :submit");
        }
    } else {
        bender_panic("pool kind must be :call or :submit");
    }
    double timeout = bender_optnumber(argv, argc, 4, INFINITY);
    if (!is_call && timeout != INFINITY) {
        bender_panic("pool timeout needs :call");
    }
    /* Bind the waiter BEFORE the task is enqueued: the late-binding
     * variant wrote task->waiter after the enqueue+signal, so a fast
     * worker could complete the task and build its completion while
     * waiter was still the malloc-zero NULL — the completion then
     * posted to nobody and the awaiter slept forever (the pool-call
     * hang: main in kevent, workers idle). The root is also gcrooted
     * here so a racing completion's unroot always pairs. */
    BenderFiber *call_waiter = NULL;
    uint32_t call_ws = 0;
    if (is_call) {
        call_waiter = bender_root_fiber();
        if (timeout != INFINITY &&
                !(call_waiter->gc.flags & BENDER_FIBER_EV_GCFLAG_ROOT)) {
            bender_panic("pool/call with timeout requires an ev task");
        }
        call_ws = call_waiter->sched_id;
    }
    /* Validate the call shape on the owner (clear errors beat worker
     * asserts): functions take exactly nargs, fibers take the array. */
    int32_t nargs;
    if (bender_checktype(argv[2], BENDER_ARRAY)) {
        nargs = bender_unwrap_array(argv[2])->count;
    } else if (bender_checktype(argv[2], BENDER_TUPLE)) {
        nargs = bender_tuple_length(bender_unwrap_tuple(argv[2]));
    } else {
        bender_panic("pool args must be an array or tuple");
    }
    if (bender_checktype(argv[1], BENDER_FUNCTION)) {
        BenderFunction *func = bender_unwrap_function(argv[1]);
        if (nargs < func->def->min_arity ||
                (func->def->max_arity >= 0 && nargs > func->def->max_arity)) {
            bender_panic("pool function arity mismatch");
        }
    } else if (!bender_checktype(argv[1], BENDER_FIBER)) {
        bender_panic("pool func must be a function or fiber");
    }
    /* Marshal [func args] (UNSAFE: crosses into the worker VM). */
    Bender pair[2];
    pair[0] = argv[1];
    pair[1] = argv[2];
    BenderBuffer payload;
    bender_buffer_init(&payload, 0);
    bender_marshal(&payload, bender_wrap_tuple(bender_tuple_n(pair, 2)), NULL, BENDER_MARSHAL_UNSAFE);
    uint8_t *bytes = NULL;
    size_t len = (size_t) payload.count;
    if (len > 0) {
        bytes = bender_malloc(len);
        if (NULL == bytes) {
            bender_buffer_deinit(&payload);
            BENDER_OUT_OF_MEMORY;
        }
        memcpy(bytes, payload.data, len);
    }
    bender_buffer_deinit(&payload);
    BenderPoolTask *task = bender_malloc(sizeof(BenderPoolTask));
    if (NULL == task) {
        if (bytes != NULL) bender_free(bytes);
        BENDER_OUT_OF_MEMORY;
    }
    task->bytes = bytes;
    task->len = len;
    task->owner = &bender_vm;
    task->waiter = call_waiter;
    task->waiter_sched = call_ws;
    if (NULL != call_waiter) bender_gcroot(bender_wrap_fiber(call_waiter));
    task->reply = NULL;
    task->next = NULL;
    Bender replyv = bender_wrap_nil();
    if (!is_call) {
        BenderChannel *reply = bender_channel_make_threaded(1);
        bender_abstract_incref(reply);
        task->reply = reply;
        replyv = bender_wrap_abstract(reply);
    }
    bender_pool_lock(pool);
    if (pool->stopping || pool->joined) {
        bender_pool_unlock(pool);
        /* Hand the token back: total supply is constant, so it fits. */
        bender_channel_push(pool->tokens, bender_wrap_nil(), 2);
        if (NULL != call_waiter) bender_gcunroot(bender_wrap_fiber(call_waiter));
        if (task->reply != NULL) bender_abstract_decref_maybe_free(task->reply);
        if (task->bytes != NULL) bender_free(task->bytes);
        bender_free(task);
        bender_panic("pool is stopping");
    }
    if (pool->tail != NULL) {
        pool->tail->next = task;
    } else {
        pool->head = task;
    }
    pool->tail = task;
    pool->count++;
    bender_pool_signal(pool);
    bender_pool_unlock(pool);
    /* Owner-side ref for the outstanding completion (paired with the
     * dec in bender_pool_complete). Thread-local counters: must inc
     * where the dec runs. */
    bender_ev_inc_refcount();
    if (!is_call) {
        return replyv;
    }
    /* :call: single trailing suspension (take/join shape). The waiter
     * fiber is rooted; the completion unroots it. The pool itself is
     * pinned from spawn until stop. */
    BenderFiber *root = call_waiter;
    if (timeout != INFINITY) {
        bender_addtimeout(timeout);
    }
    bender_await();
    /* Unreachable when suspending; the completion schedules this fiber
     * with the task result (or cancels with its error). */
    return bender_wrap_nil();
}

BENDER_CORE_FN(cfun_pool_profile_harvest,
              "(pool/profile-harvest p)",
              "Drain this pool's harvested per-worker profiles (Phase C): "
              "returns an array with one dump-format row array per exited "
              "worker that profiled (empty when profiling was off or "
              "nothing exited yet). Workers inherit the collector when the "
              "owner is profiling at spawn; each exiting worker appends "
              "its table. Merge rows Bender-side (tools/profile-merge.ben) "
              "and feed callgraph --profile. Must be called from the owning "
              "thread. Unread harvests are freed with the pool.") {
    bender_fixarity(argc, 1);
    BenderPool *pool = bender_getpool(argv, 0);
    BenderArray *out = bender_array(0);
    Bender out_j = bender_wrap_array(out);
    bender_gcroot(out_j);
    bender_pool_lock(pool);
    BenderProfileHarvest *h = pool->harvest_head;
    pool->harvest_head = pool->harvest_tail = NULL;
    bender_pool_unlock(pool);
    while (h != NULL) {
        BenderProfileHarvest *next = h->next;
        if (h->bytes != NULL && h->len > 0) {
            Bender rows = bender_unmarshal(h->bytes, h->len, BENDER_MARSHAL_UNSAFE, NULL, NULL);
            bender_array_push(out, rows);
            bender_free(h->bytes);
        } else if (h->bytes != NULL) {
            bender_free(h->bytes);
        }
        bender_free(h);
        h = next;
    }
    bender_gcunroot(out_j);
    return out_j;
}

BENDER_CORE_FN(cfun_pool_stop,
              "(pool/stop p)",
              "Drain-stop pool `p`: queued tasks run first, then every "
              "worker exits; returns nil once all workers are joined. "
              "Suspends (the loop stays live during the drain). Further "
              "use panics. Must be called from the owning thread, exactly "
              "once. Pools must be stopped (like files must be closed).") {
    bender_fixarity(argc, 1);
    BenderPool *pool = bender_getpool(argv, 0);
    if (bender_vm.coerce_error) {
        bender_panic("cannot stop pool inside bender_call");
    }
    bender_pool_lock(pool);
    if (pool->joined || pool->stopping) {
        bender_pool_unlock(pool);
        bender_panic("pool already stopped");
    }
    pool->stopping = 1;
    if (pool->stop_waiter != NULL) {
        bender_pool_unlock(pool);
        bender_panic("pool already stopping");
    }
    BenderFiber *root = bender_root_fiber();
    pool->stop_waiter = root;
    pool->stop_sched = root->sched_id;
    bender_pool_broadcast(pool);
    bender_pool_unlock(pool);
    /* Rooted until the all-exited callback unroots (teardown stands
     * even if this waiter is cancelled first). Owner-side ref for the
     * all-exited event, paired with its dec there. */
    bender_gcroot(bender_wrap_fiber(root));
    bender_ev_inc_refcount();
    bender_await();
    /* Unreachable when suspending; the callback schedules nil here. */
    return bender_wrap_nil();
}

BENDER_CORE_FN(cfun_thread_monitor,
              "(ev/monitor handle tag chan)",
              "Watch the thread `handle` created by `thread/new`: when the "
              "worker exits, send `[:down tag reason]` to `chan`, where "
              "`reason` is `:normal` on clean exit or the thread's error "
              "value on failure. Delivery is best-effort (at-most-once): "
              "the message is dropped if `chan` is closed or full. "
              "Monitoring an already-finished thread delivers immediately "
              "and returns nil. Must be called from the creating thread. "
              "Returns an integer monitor ref for `ev/demonitor`.") {
    bender_sandbox_assert(BENDER_SANDBOX_THREADS);
    bender_fixarity(argc, 3);
    BenderThreadHandle *handle = bender_getabstract(argv, 0, &bender_thread_handle_type);
    if (handle->owner != &bender_vm) {
        bender_panic("ev/monitor called from non-owner thread");
    }
    Bender chanv = argv[2];
    bender_getchannel(argv, 2);
    Bender tag = argv[1];
    if (handle->done) {
        /* Late registration: completion already ran, deliver now.
         * No slot is stored, so there is no ref to return. */
        bender_thread_give_down(chanv,
            bender_thread_down_message(tag, handle->is_error, handle->result));
        return bender_wrap_nil();
    }
    if (handle->n_monitors >= handle->cap_monitors) {
        int32_t ncap = handle->cap_monitors ? handle->cap_monitors * 2 : 4;
        Bender *nmon = bender_realloc(handle->monitors, (size_t) ncap * 2 * sizeof(Bender));
        if (NULL == nmon) {
            BENDER_OUT_OF_MEMORY;
        }
        handle->monitors = nmon;
        handle->cap_monitors = ncap;
    }
    bender_gcroot(chanv);
    bender_gcroot(tag);
    int32_t ref = handle->n_monitors;
    handle->monitors[ref * 2] = chanv;
    handle->monitors[ref * 2 + 1] = tag;
    handle->n_monitors++;
    return bender_wrap_integer(ref);
}

/* M3: process-global name registry. One store shared by every VM
 * in the process (Bender tables are per-VM and isolated, so this lives
 * in C). Values are kept as marshaled byte copies: no cross-heap
 * references, no GC coupling — lookups copy out and unmarshal in the
 * caller's VM. Marshal runs BEFORE the lock is taken, so a refusal
 * (e.g. thread handles) raises without wedging the mutex. UNSAFE mode
 * passes threaded channels by pointer (shared, incref'd) — the M2e
 * pid shape travels intact. Known minor cost: one channel ref per
 * registration is never released; irrelevant at registry scale. */
typedef struct {
    char *name;
    uint8_t *bytes;
    size_t len;
} BenderNameRegistryEntry;

static BenderNameRegistryEntry *reg_entries = NULL;
static int32_t reg_count = 0;
static int32_t reg_cap = 0;
/* Local platform mutex (BenderOSMutex is opaque and cannot embed). */
#ifdef BENDER_WINDOWS
static CRITICAL_SECTION reg_lock;
static void reg_lock_init(void) { InitializeCriticalSection(&reg_lock); }
static void reg_lock_take(void) { EnterCriticalSection(&reg_lock); }
static void reg_lock_drop(void) { LeaveCriticalSection(&reg_lock); }
#else
static pthread_mutex_t reg_lock;
static void reg_lock_init(void) { pthread_mutex_init(&reg_lock, NULL); }
static void reg_lock_take(void) { pthread_mutex_lock(&reg_lock); }
static void reg_lock_drop(void) { pthread_mutex_unlock(&reg_lock); }
#endif
#ifdef BENDER_WINDOWS
static INIT_ONCE reg_once = INIT_ONCE_STATIC_INIT;
static BOOL CALLBACK reg_do_init(PINIT_ONCE once, PVOID arg, PVOID *ctx) {
    (void) once;
    (void) arg;
    (void) ctx;
    reg_lock_init();
    return TRUE;
}
static void reg_ensure_init(void) {
    InitOnceExecuteOnce(&reg_once, reg_do_init, NULL, NULL);
}
#else
static pthread_once_t reg_once = PTHREAD_ONCE_INIT;
static void reg_do_init(void) {
    reg_lock_init();
}
static void reg_ensure_init(void) {
    pthread_once(&reg_once, reg_do_init);
}
#endif

/* Caller holds reg_lock. Returns index or -1. */
static int32_t reg_find(const char *name) {
    for (int32_t i = 0; i < reg_count; i++) {
        if (!strcmp(reg_entries[i].name, name)) return i;
    }
    return -1;
}

BENDER_CORE_FN(cfun_registry_register,
              "(ev/register name value)",
              "Bind keyword `name` to `value` in the process-global "
              "registry, visible from every thread. `value` is stored as "
              "a marshaled copy: lookups get an equal copy in the "
              "caller's own VM, never a cross-heap reference. Replacing "
              "an entry drops the old copy. Unmarshalable values (thread "
              "handles) raise. Returns `value`.") {
    bender_fixarity(argc, 2);
    const uint8_t *kws = bender_getkeyword(argv, 0);
    const char *name = (const char *) kws;
    /* Thread handles are owner-local capabilities: UNSAFE marshal
     * would pass them by pointer (silently leaking a ref and handing
     * out a handle every foreign use rejects). Refuse explicitly. */
    if (bender_checktype(argv[1], BENDER_ABSTRACT) &&
            bender_abstract_type(bender_unwrap_abstract(argv[1])) == &bender_thread_handle_type) {
        bender_panic("ev/register cannot store thread handles (owner-local)");
    }
    BenderBuffer tmp;
    bender_buffer_init(&tmp, 0);
    bender_marshal(&tmp, argv[1], NULL, BENDER_MARSHAL_UNSAFE);
    uint8_t *copy = bender_malloc(tmp.count ? (size_t) tmp.count : 1);
    if (NULL == copy) {
        bender_buffer_deinit(&tmp);
        BENDER_OUT_OF_MEMORY;
    }
    if (tmp.count) memcpy(copy, tmp.data, (size_t) tmp.count);
    size_t len = (size_t) tmp.count;
    bender_buffer_deinit(&tmp);
    reg_ensure_init();
    reg_lock_take();
    int32_t at = reg_find(name);
    if (at >= 0) {
        bender_free(reg_entries[at].bytes);
        reg_entries[at].bytes = copy;
        reg_entries[at].len = len;
    } else {
        if (reg_count >= reg_cap) {
            int32_t ncap = reg_cap ? reg_cap * 2 : 8;
            BenderNameRegistryEntry *nent = bender_realloc(reg_entries,
                (size_t) ncap * sizeof(BenderNameRegistryEntry));
            if (NULL == nent) {
                reg_lock_drop();
                bender_free(copy);
                BENDER_OUT_OF_MEMORY;
            }
            reg_entries = nent;
            reg_cap = ncap;
        }
        size_t nlen = strlen(name) + 1;
        char *kcopy = bender_malloc(nlen);
        if (NULL == kcopy) {
            reg_lock_drop();
            bender_free(copy);
            BENDER_OUT_OF_MEMORY;
        }
        memcpy(kcopy, name, nlen);
        reg_entries[reg_count].name = kcopy;
        reg_entries[reg_count].bytes = copy;
        reg_entries[reg_count].len = len;
        reg_count++;
    }
    reg_lock_drop();
    return argv[1];
}

BENDER_CORE_FN(cfun_registry_whereis,
              "(ev/whereis name)",
              "Look up keyword `name` in the process-global registry. "
              "Returns an unmarshaled copy of the registered value in "
              "the caller's VM, or nil when absent.") {
    bender_fixarity(argc, 1);
    const uint8_t *kws = bender_getkeyword(argv, 0);
    const char *name = (const char *) kws;
    reg_ensure_init();
    reg_lock_take();
    int32_t at = reg_find(name);
    uint8_t *copy = NULL;
    size_t len = 0;
    if (at >= 0 && reg_entries[at].len) {
        len = reg_entries[at].len;
        copy = bender_malloc(len);
        if (NULL == copy) {
            reg_lock_drop();
            BENDER_OUT_OF_MEMORY;
        }
        memcpy(copy, reg_entries[at].bytes, len);
    }
    reg_lock_drop();
    if (NULL == copy) {
        return bender_wrap_nil();
    }
    Bender out = bender_unmarshal(copy, len, BENDER_MARSHAL_UNSAFE, NULL, NULL);
    bender_free(copy);
    return out;
}

BENDER_CORE_FN(cfun_registry_unregister,
              "(ev/unregister name)",
              "Remove keyword `name` from the process-global registry. "
              "Returns true when an entry was removed, false otherwise.") {
    bender_fixarity(argc, 1);
    const uint8_t *kws = bender_getkeyword(argv, 0);
    const char *name = (const char *) kws;
    reg_ensure_init();
    reg_lock_take();
    int32_t at = reg_find(name);
    int found = (at >= 0);
    if (found) {
        bender_free(reg_entries[at].name);
        bender_free(reg_entries[at].bytes);
        reg_entries[at] = reg_entries[reg_count - 1];
        reg_count--;
    }
    reg_lock_drop();
    return bender_wrap_boolean(found);
}

/* M5: shareable immutable blobs (see
 * docs/internals/shared-structs-design.md). A threaded abstract whose
 * payload is a self-contained flattened encoding — never GC pointers
 * — so it is sound in every VM at once: lifetime is the abstract's
 * atomic refcount, cross-thread movement rides the existing UNSAFE
 * threaded passthrough, and reads copy out. Layout is little-endian
 * fixed-width throughout (same-machine sharing; cross-machine stays
 * marshal-based). Tags: 0 nil, 1 false, 2 true, 3 f64, 4 string,
 * 5 keyword, 6 symbol, 7 struct, 8 tuple, 9 nested-shared. */
static const BenderAbstractType bender_shared_type;
#define SHARED_MAX_DEPTH 256

typedef struct {
    uint32_t len;
    uint8_t data[];
} BenderSharedBlob;

static void shared_push_u32(BenderBuffer *buf, uint32_t v) {
    uint8_t b[4];
    b[0] = (uint8_t) (v & 0xFF);
    b[1] = (uint8_t) ((v >> 8) & 0xFF);
    b[2] = (uint8_t) ((v >> 16) & 0xFF);
    b[3] = (uint8_t) ((v >> 24) & 0xFF);
    bender_buffer_push_bytes(buf, b, 4);
}

static int shared_is_blob(Bender x) {
    return bender_checktype(x, BENDER_ABSTRACT) &&
        bender_abstract_type(bender_unwrap_abstract(x)) == &bender_shared_type;
}

/* Forward declarations (encoder/decoder recurse). */
static void shared_encode(BenderBuffer *buf, Bender x, int depth);

/* Deep shareability check: only immutable, heap-free leaves. */
static int shared_checkable(Bender x, int depth) {
    if (depth > SHARED_MAX_DEPTH) return 0;
    switch (bender_type(x)) {
        case BENDER_NIL:
        case BENDER_BOOLEAN:
        case BENDER_NUMBER:
            return 1;
        case BENDER_STRING:
        case BENDER_SYMBOL:
        case BENDER_KEYWORD:
            return 1;
        case BENDER_TUPLE: {
            BenderTuple t = bender_unwrap_tuple(x);
            int32_t n = bender_tuple_length(t);
            for (int32_t i = 0; i < n; i++) {
                if (!shared_checkable(t[i], depth + 1)) return 0;
            }
            return 1;
        }
        case BENDER_STRUCT: {
            /* Structs are open-addressed hash tables: iterate to
             * CAPACITY skipping nil keys (length counts live only).
             * Prototypes change lookup semantics and do not flatten,
             * so protos are rejected. */
            BenderStruct s = bender_unwrap_struct(x);
            if (NULL != bender_struct_proto(s)) return 0;
            int32_t cap = bender_struct_capacity(s);
            for (int32_t i = 0; i < cap; i++) {
                if (bender_checktype(s[i].key, BENDER_NIL)) continue;
                if (!shared_checkable(s[i].key, depth + 1)) return 0;
                if (!shared_checkable(s[i].value, depth + 1)) return 0;
            }
            return 1;
        }
        case BENDER_ABSTRACT:
            return shared_is_blob(x);
        default:
            return 0;
    }
}

static void shared_encode_str(BenderBuffer *buf, uint8_t tag, const uint8_t *s) {
    int32_t len = bender_string_length(s);
    bender_buffer_push_u8(buf, tag);
    shared_push_u32(buf, (uint32_t) len);
    bender_buffer_push_bytes(buf, s, len);
}

static void shared_encode(BenderBuffer *buf, Bender x, int depth) {
    if (depth > SHARED_MAX_DEPTH) bender_panic("shared value too deep");
    switch (bender_type(x)) {
        case BENDER_NIL:
            bender_buffer_push_u8(buf, 0);
            break;
        case BENDER_BOOLEAN:
            bender_buffer_push_u8(buf, bender_unwrap_boolean(x) ? 2 : 1);
            break;
        case BENDER_NUMBER: {
            double d = bender_unwrap_number(x);
            bender_buffer_push_u8(buf, 3);
            bender_buffer_push_bytes(buf, (uint8_t *) &d, 8);
            break;
        }
        case BENDER_STRING:
            shared_encode_str(buf, 4, bender_unwrap_string(x));
            break;
        case BENDER_SYMBOL:
            shared_encode_str(buf, 6, bender_unwrap_symbol(x));
            break;
        case BENDER_KEYWORD:
            shared_encode_str(buf, 5, bender_unwrap_keyword(x));
            break;
        case BENDER_TUPLE: {
            BenderTuple t = bender_unwrap_tuple(x);
            int32_t n = bender_tuple_length(t);
            bender_buffer_push_u8(buf, 8);
            shared_push_u32(buf, (uint32_t) n);
            for (int32_t i = 0; i < n; i++) shared_encode(buf, t[i], depth + 1);
            break;
        }
        case BENDER_STRUCT: {
            BenderStruct s = bender_unwrap_struct(x);
            int32_t n = bender_struct_length(s);
            int32_t cap = bender_struct_capacity(s);
            bender_buffer_push_u8(buf, 7);
            shared_push_u32(buf, (uint32_t) n);
            for (int32_t i = 0; i < cap; i++) {
                if (bender_checktype(s[i].key, BENDER_NIL)) continue;
                shared_encode(buf, s[i].key, depth + 1);
                shared_encode(buf, s[i].value, depth + 1);
            }
            break;
        }
        case BENDER_ABSTRACT: {
            /* Nested shared: inline expansion (deep copy — sharing is
             * by value, still immutable so semantics hold). */
            BenderSharedBlob *nb = bender_unwrap_abstract(x);
            bender_buffer_push_u8(buf, 9);
            shared_push_u32(buf, nb->len);
            bender_buffer_push_bytes(buf, nb->data, nb->len);
            break;
        }
        default:
            bender_panicf("cannot share value of type %s", bender_type_names[bender_type(x)]);
    }
}

/* Defensive cursor decoder: every read bounds-checked, truncation
 * panics instead of over-reading (marshal bytes cross threads). */
typedef struct {
    const uint8_t *p;
    const uint8_t *end;
} BenderSharedCursor;

static void shared_need(BenderSharedCursor *c, size_t n) {
    if ((size_t)(c->end - c->p) < n) bender_panic("truncated shared value");
}

static uint8_t shared_rd_u8(BenderSharedCursor *c) {
    shared_need(c, 1);
    return *(c->p)++;
}

static uint32_t shared_rd_u32(BenderSharedCursor *c) {
    shared_need(c, 4);
    uint32_t v = ((uint32_t) c->p[0]) | ((uint32_t) c->p[1] << 8) |
        ((uint32_t) c->p[2] << 16) | ((uint32_t) c->p[3] << 24);
    c->p += 4;
    return v;
}

static const uint8_t *shared_rd_bytes(BenderSharedCursor *c, size_t n) {
    shared_need(c, n);
    const uint8_t *p = c->p;
    c->p += n;
    return p;
}

static Bender shared_decode(BenderSharedCursor *c, int depth) {
    if (depth > SHARED_MAX_DEPTH) bender_panic("shared value too deep");
    uint8_t tag = shared_rd_u8(c);
    switch (tag) {
        case 0:
            return bender_wrap_nil();
        case 1:
            return bender_wrap_boolean(0);
        case 2:
            return bender_wrap_boolean(1);
        case 3: {
            double d = 0;
            memcpy(&d, shared_rd_bytes(c, 8), 8);
            return bender_wrap_number(d);
        }
        case 4: {
            uint32_t n = shared_rd_u32(c);
            const uint8_t *b = shared_rd_bytes(c, n);
            return bender_stringv(b, (int32_t) n);
        }
        case 5: {
            uint32_t n = shared_rd_u32(c);
            const uint8_t *b = shared_rd_bytes(c, n);
            return bender_keywordv(b, (int32_t) n);
        }
        case 6: {
            uint32_t n = shared_rd_u32(c);
            const uint8_t *b = shared_rd_bytes(c, n);
            return bender_symbolv(b, (int32_t) n);
        }
        case 7: {
            uint32_t n = shared_rd_u32(c);
            BenderTable *t = bender_table(0);
            for (uint32_t i = 0; i < n; i++) {
                Bender k = shared_decode(c, depth + 1);
                Bender v = shared_decode(c, depth + 1);
                bender_table_put(t, k, v);
            }
            return bender_wrap_struct(bender_table_to_struct(t));
        }
        case 8: {
            uint32_t n = shared_rd_u32(c);
            Bender *items = bender_malloc(sizeof(Bender) * (n ? n : 1));
            if (NULL == items) BENDER_OUT_OF_MEMORY;
            uint32_t i = 0;
            for (; i < n; i++) items[i] = shared_decode(c, depth + 1);
            Bender ret = bender_wrap_tuple(bender_tuple_n(items, (int32_t) n));
            bender_free(items);
            return ret;
        }
        case 9: {
            /* Nested blob: rebuild fresh (deep-copy semantics). */
            uint32_t n = shared_rd_u32(c);
            const uint8_t *b = shared_rd_bytes(c, n);
            BenderSharedBlob *nb = bender_abstract_threaded(&bender_shared_type,
                sizeof(BenderSharedBlob) + n);
            nb->len = n;
            if (n) memcpy(nb->data, b, n);
            return bender_wrap_abstract(nb);
        }
        default:
            bender_panicf("invalid shared tag %d", tag);
            return bender_wrap_nil();
    }
}

/* Navigation without full decode: skip any encoded value (NULL
 * on truncation), then compare canonical key bytes directly. */
static const uint8_t *shared_skip_value(const uint8_t *p, const uint8_t *end) {
    if (p >= end) return NULL;
    uint8_t t = *p++;
    uint32_t n = 0;
    switch (t) {
        case 0:
        case 1:
        case 2:
            return p;
        case 3:
            return ((size_t)(end - p) >= 8) ? p + 8 : NULL;
        case 4:
        case 5:
        case 6:
        case 9:
            if ((size_t)(end - p) < 4) return NULL;
            n = ((uint32_t) p[0]) | ((uint32_t) p[1] << 8) |
                ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24);
            p += 4;
            return ((size_t)(end - p) >= n) ? p + n : NULL;
        case 7: {
            if ((size_t)(end - p) < 4) return NULL;
            n = ((uint32_t) p[0]) | ((uint32_t) p[1] << 8) |
                ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24);
            p += 4;
            for (uint32_t i = 0; i < n * 2; i++) {
                p = shared_skip_value(p, end);
                if (NULL == p) return NULL;
            }
            return p;
        }
        case 8: {
            if ((size_t)(end - p) < 4) return NULL;
            n = ((uint32_t) p[0]) | ((uint32_t) p[1] << 8) |
                ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24);
            p += 4;
            for (uint32_t i = 0; i < n; i++) {
                p = shared_skip_value(p, end);
                if (NULL == p) return NULL;
            }
            return p;
        }
        default:
            return NULL;
    }
}

/* Find key in an encoded struct (base at u32 count). On hit sets
 * *valp to the value start and returns 1. */
static int shared_struct_find(const uint8_t *base, const uint8_t *end,
        const uint8_t *skey, uint32_t skeylen, const uint8_t **valp) {
    if ((size_t)(end - base) < 4) return 0;
    uint32_t n = ((uint32_t) base[0]) | ((uint32_t) base[1] << 8) |
        ((uint32_t) base[2] << 16) | ((uint32_t) base[3] << 24);
    const uint8_t *p = base + 4;
    for (uint32_t i = 0; i < n; i++) {
        const uint8_t *kend = shared_skip_value(p, end);
        if (NULL == kend) return 0;
        if ((uint32_t)(kend - p) == skeylen && !memcmp(p, skey, skeylen)) {
            *valp = kend;
            return 1;
        }
        p = shared_skip_value(kend, end);
        if (NULL == p) return 0;
    }
    return 0;
}

BENDER_CORE_FN(cfun_thread_demonitor,
              "(ev/demonitor handle ref)",
              "Remove the monitor `ref` (returned by `ev/monitor`) from "
              "the thread `handle`, so no `:down` message is delivered for "
              "it. Unknown refs, already-removed monitors, and monitors on "
              "finished threads are silent no-ops. Must be called from the "
              "creating thread. Returns nil.") {
    bender_sandbox_assert(BENDER_SANDBOX_THREADS);
    bender_fixarity(argc, 2);
    BenderThreadHandle *handle = bender_getabstract(argv, 0, &bender_thread_handle_type);
    if (handle->owner != &bender_vm) {
        bender_panic("ev/demonitor called from non-owner thread");
    }
    if (!bender_checkint(argv[1])) {
        bender_panic("ev/demonitor ref must be an integer");
    }
    int32_t ref = bender_unwrap_integer(argv[1]);
    /* Finished threads already delivered and freed the list. */
    if (handle->monitors == NULL) {
        return bender_wrap_nil();
    }
    if (ref < 0 || ref >= handle->n_monitors) {
        return bender_wrap_nil();
    }
    Bender chanv = handle->monitors[ref * 2];
    Bender tag = handle->monitors[ref * 2 + 1];
    if (bender_checktype(chanv, BENDER_NIL)) {
        /* Already removed. */
        return bender_wrap_nil();
    }
    bender_gcunroot(chanv);
    bender_gcunroot(tag);
    handle->monitors[ref * 2] = bender_wrap_nil();
    handle->monitors[ref * 2 + 1] = bender_wrap_nil();
    return bender_wrap_nil();
}

/* Blob construction + type-level (un)marshal + accessors. */

static BenderSharedBlob *shared_blob_new(const uint8_t *data, uint32_t len) {
    BenderSharedBlob *b = bender_abstract_threaded(&bender_shared_type,
        sizeof(BenderSharedBlob) + (len ? len : 1));
    b->len = len;
    if (len) memcpy(b->data, data, len);
    return b;
}

static void bender_shared_marshal(void *p, BenderMarshalContext *ctx) {
    BenderSharedBlob *b = p;
    bender_marshal_int(ctx, (int32_t) b->len);
    if (b->len) bender_marshal_bytes(ctx, b->data, b->len);
}

static void *bender_shared_unmarshal(BenderMarshalContext *ctx) {
    int32_t len = bender_unmarshal_int(ctx);
    if (len < 0) bender_panic("invalid shared length");
    BenderSharedBlob *b = bender_abstract_threaded(&bender_shared_type,
        sizeof(BenderSharedBlob) + (len ? (uint32_t) len : 1));
    b->len = (uint32_t) len;
    if (len) {
        uint8_t *tmp = bender_malloc((size_t) len);
        if (NULL == tmp) BENDER_OUT_OF_MEMORY;
        bender_unmarshal_bytes(ctx, tmp, (size_t) len);
        memcpy(b->data, tmp, (size_t) len);
        bender_free(tmp);
    }
    /* Register in the lookup table (alias/cycle tracking + the
     * framework's completion check); stays threaded either way. */
    bender_unmarshal_abstract_reuse(ctx, b);
    return b;
}

static int bender_shared_mark(void *p, size_t len) {
    /* Payload holds no GC pointers — nothing to mark. */
    (void) p;
    (void) len;
    return 0;
}

static void bender_shared_tostring(void *p, BenderBuffer *buffer) {
    (void) p;
    bender_buffer_push_cstring(buffer, "<shared>");
}

static const BenderAbstractType bender_shared_type = {
    "core/shared",
    NULL, /* gc (inline payload frees with the allocation) */
    bender_shared_mark,
    NULL, /* get (use shared/get) */
    NULL, /* put */
    bender_shared_marshal,
    bender_shared_unmarshal,
    bender_shared_tostring,
    NULL, /* compare */
    NULL, /* hash */
    NULL, /* next */
    NULL, /* call */
    NULL, /* length (use shared/length) */
    NULL, /* bytes */
    NULL  /* gcperthread (no nested refs to drop) */
};

static BenderSharedBlob *shared_get_blob(Bender x) {
    if (!shared_is_blob(x)) bender_panic("expected shared blob");
    return bender_unwrap_abstract(x);
}

BENDER_CORE_FN(cfun_shared_new,
              "(shared/new x)",
              "Freeze the shareable value `x` into an immutable blob "
              "that is sound in every VM at once (lifetime is atomic "
              "refcounting, cross-thread movement is by pointer). "
              "Allowed leaves: nil, booleans, numbers, strings, symbols, "
              "keywords, tuples, structs, and nested shared blobs. "
              "Anything else (tables, arrays, buffers, functions, fibers, "
              "other abstracts) raises. Returns the blob.") {
    bender_fixarity(argc, 1);
    if (!shared_checkable(argv[0], 0)) {
        bender_panicf("cannot share value of type %s",
            bender_type_names[bender_type(argv[0])]);
    }
    BenderBuffer buf;
    bender_buffer_init(&buf, 0);
    shared_encode(&buf, argv[0], 0);
    BenderSharedBlob *b = shared_blob_new(buf.data, buf.count);
    bender_buffer_deinit(&buf);
    return bender_wrap_abstract(b);
}

BENDER_CORE_FN(cfun_shared_shareable,
              "(shared/shareable? x)",
              "True when `x` can be frozen with shared/new (deep check).") {
    bender_fixarity(argc, 1);
    return bender_wrap_boolean(shared_checkable(argv[0], 0));
}

BENDER_CORE_FN(cfun_shared_length,
              "(shared/length b)",
              "Element count of a struct/tuple blob, else nil.") {
    bender_fixarity(argc, 1);
    BenderSharedBlob *b = shared_get_blob(argv[0]);
    if (b->len < 1) return bender_wrap_nil();
    uint8_t tag = b->data[0];
    if (tag != 7 && tag != 8) return bender_wrap_nil();
    if (b->len < 5) return bender_wrap_nil();
    uint32_t n = ((uint32_t) b->data[1]) | ((uint32_t) b->data[2] << 8) |
        ((uint32_t) b->data[3] << 16) | ((uint32_t) b->data[4] << 24);
    return bender_wrap_integer((int32_t) n);
}

BENDER_CORE_FN(cfun_shared_get,
              "(shared/get b k)",
              "Look up key `k` in a struct blob (or index a tuple blob) "
              "without decoding the whole value. Missing keys, "
              "out-of-range indices, and scalar blobs give nil. "
              "Unshareable key types also give nil.") {
    bender_fixarity(argc, 2);
    BenderSharedBlob *b = shared_get_blob(argv[0]);
    const uint8_t *end = b->data + b->len;
    if (b->len < 1) return bender_wrap_nil();
    uint8_t tag = b->data[0];
    if (tag == 8) {
        /* Tuple index. */
        if (!bender_checkint(argv[1])) return bender_wrap_nil();
        int32_t idx = bender_unwrap_integer(argv[1]);
        if (b->len < 5 || idx < 0) return bender_wrap_nil();
        uint32_t n = ((uint32_t) b->data[1]) | ((uint32_t) b->data[2] << 8) |
            ((uint32_t) b->data[3] << 16) | ((uint32_t) b->data[4] << 24);
        if ((uint32_t) idx >= n) return bender_wrap_nil();
        const uint8_t *p = b->data + 5;
        for (int32_t i = 0; i < idx; i++) {
            p = shared_skip_value(p, end);
            if (NULL == p) return bender_wrap_nil();
        }
        BenderSharedCursor c = {p, end};
        return shared_decode(&c, 0);
    }
    if (tag != 7) return bender_wrap_nil();
    if (!shared_checkable(argv[1], 0)) return bender_wrap_nil();
    BenderBuffer kb;
    bender_buffer_init(&kb, 0);
    shared_encode(&kb, argv[1], 0);
    const uint8_t *valp = NULL;
    int found = shared_struct_find(b->data + 1, end, kb.data, kb.count, &valp);
    bender_buffer_deinit(&kb);
    if (!found) return bender_wrap_nil();
    BenderSharedCursor c = {valp, end};
    return shared_decode(&c, 0);
}

BENDER_CORE_FN(cfun_shared_materialize,
              "(shared/materialize b)",
              "Decode a whole blob back to ordinary caller-heap values "
              "(structs, tuples, re-interned keywords). The blob itself "
              "is unchanged.") {
    bender_fixarity(argc, 1);
    BenderSharedBlob *b = shared_get_blob(argv[0]);
    BenderSharedCursor c = {b->data, b->data + b->len};
    return shared_decode(&c, 0);
}

BENDER_CORE_FN(cfun_ev_timeslice,
              "(ev/timeslice &opt on?)",
              "Enable (truthy) or disable (false/nil) preemptive "
              "round-robin quanta for the current VM, and return the new "
              "setting. Quanta are ON by default (Step 2 preemptive "
              "reductions; BENDER_TIMESLICE=0 starts a process with them "
              "off): every scheduled task runs at most "
              "BENDER_QUANTUM_BUDGET back-edge/call polls before it is "
              "transparently re-queued: compute hogs can no longer starve "
              "sleepers and takers, with results bit-exact. Quanta never "
              "land inside C calls or pcall regions (poll points are Bender "
              "ops only). Incompatible with external C quiesce while on. "
              "Per-VM; workers inherit the default independently.") {
    bender_arity(argc, 0, 1);
    if (argc >= 1) {
        bender_vm.quantum = bender_truthy(argv[0]) ? 1 : 0;
    }
    return bender_wrap_boolean(bender_vm.quantum);
}

BENDER_CORE_FN(cfun_ev_interrupt,
              "(ev/interrupt fiber &opt payload)",
              "Request `fiber` (same VM) suspend with a yield of `payload` "
              "(default nil) at its next resume boundary: before running "
              "another instruction, never inside pcall, never in C. A "
              "racing wakeup value is dropped (interrupt wins); interrupts "
              "do not cross VMs (cleared on unmarshal). Returns `fiber`.") {
    bender_arity(argc, 1, 2);
    BenderFiber *fiber = bender_getfiber(argv, 0);
    fiber->interrupt_requested = 1;
    fiber->interrupt_value = (argc >= 2) ? argv[1] : bender_wrap_nil();
    return argv[0];
}

BENDER_CORE_FN(cfun_ev_give_supervisor,
              "(ev/give-supervisor tag & payload)",
              "Send a message to the current supervisor channel if there is one. The message will be a "
              "tuple of all of the arguments combined into a single message, where the first element is tag. "
              "By convention, tag should be a keyword indicating the type of message. Returns nil.") {
    bender_arity(argc, 1, -1);
    void *chanv = bender_vm.root_fiber->supervisor_channel;
    if (NULL != chanv) {
        BenderChannel *chan = bender_channel_unwrap(chanv);
        if (bender_channel_push(chan, bender_wrap_tuple(bender_tuple_n(argv, argc)), 0)) {
            bender_await();
        }
    }
    return bender_wrap_nil();
}

BENDER_NO_RETURN void bender_sleep_await(double sec) {
    BenderTimeout to;
    to.when = ts_delta(ts_now(), sec);
    to.fiber = bender_vm.root_fiber;
    to.is_error = 0;
    to.sched_id = to.fiber->sched_id;
    to.curr_fiber = NULL;
    to.has_worker = 0;
    add_timeout(to);
    bender_await();
}

BENDER_CORE_FN(cfun_ev_pump,
              "(ev/pump)",
              "Run one non-blocking turn of the event loop and return. Any ready "
              "task fiber runs, and ready I/O is sampled with a zero deadline, "
              "but the call never waits: it cannot be delayed by a pending "
              "timer or an idle stream. Call it once per frame from a render "
              "loop to let `ev/go` fibers and `ev` listeners make progress "
              "without suspending the caller. Returns nil. `ev/sleep 0` is the "
              "suspending alternative; it additionally yields the current "
              "fiber.") {
    bender_fixarity(argc, 0);
    (void) argv;
    /* Run one non-blocking scheduler turn.
     *
     * This must not re-enter bender_loop1 inside the calling frame: the loop
     * resumes task fibers, which swaps bender_vm.fiber and the C stack out from
     * under the caller (observed as a bus error). So pump never runs a task
     * directly. It first samples expired timers and ready I/O with a
     * zero-deadline poll that cannot block, then suspends with a zero-duration
     * sleep. Once parked, the ordinary driver resumes queued task fibers and
     * the timer wakes us immediately, so a pump costs one scheduler round trip
     * and stays in the microseconds. */
    bender_loop1_poll_now();
    bender_sleep_await(0.0);
}

BENDER_CORE_FN(cfun_ev_sleep,
              "(ev/sleep sec)",
              "Suspend the current fiber for sec seconds without blocking the event loop.") {
    bender_fixarity(argc, 1);
    double sec = bender_getnumber(argv, 0);
    bender_sleep_await(sec);
}

BENDER_CORE_FN(cfun_ev_deadline,
              "(ev/deadline sec &opt tocancel tocheck intr?)",
              "Schedules the event loop to try to cancel the `tocancel` task as with `ev/cancel`. "
              "After `sec` seconds, the event loop will attempt cancellation of `tocancel` if the "
              "`tocheck` fiber is resumable. `sec` is a number that can have a fractional part. "
              "`tocancel` defaults to `(fiber/root)`, but if specified, must be a task (root "
              "fiber). `tocheck` defaults to `(fiber/current)`, but if specified, must be a fiber. "
              "Returns `tocancel` immediately. If `interrupt?` is set to true, will create a "
              "background thread to try to interrupt the VM if the timeout expires.") {
    bender_arity(argc, 1, 4);
    double sec = bender_getnumber(argv, 0);
    sec = (sec < 0) ? 0 : sec;
    BenderFiber *tocancel = bender_optfiber(argv, argc, 1, bender_vm.root_fiber);
    BenderFiber *tocheck = bender_optfiber(argv, argc, 2, bender_vm.fiber);
    int use_interrupt = bender_optboolean(argv, argc, 3, 0);
    BenderTimeout to;
    to.when = ts_delta(ts_now(), sec);
    to.fiber = tocancel;
    to.curr_fiber = tocheck;
    to.is_error = 0;
    to.sched_id = to.fiber->sched_id;
    if (use_interrupt) {
#ifdef BENDER_ANDROID
        bender_sandbox_assert(BENDER_SANDBOX_SIGNAL);
#endif
        BenderThreadedTimeout *tto = bender_malloc(sizeof(BenderThreadedTimeout));
        if (NULL == tto) {
            BENDER_OUT_OF_MEMORY;
        }
        tto->sec = sec;
        tto->vm = &bender_vm;
        tto->fiber = tocheck;
#ifdef BENDER_WINDOWS
        HANDLE cancel_event = CreateEvent(NULL, TRUE, FALSE, NULL);
        if (NULL == cancel_event) {
            bender_free(tto);
            bender_panic("failed to create cancel event");
        }
        tto->cancel_event = cancel_event;
        HANDLE worker = CreateThread(NULL, 0, bender_timeout_body, tto, CREATE_SUSPENDED, NULL);
        if (NULL == worker) {
            bender_free(tto);
            bender_panic("failed to create thread");
        }
#else
        pthread_t worker;
        int err = pthread_create(&worker, NULL, bender_timeout_body, tto);
        if (err) {
            bender_free(tto);
            bender_panicf("%s", bender_strerror(err));
        }
#endif
        to.has_worker = 1;
        to.worker = worker;
#ifdef BENDER_WINDOWS
        to.worker_event = cancel_event;
        ResumeThread(worker);
#endif
    } else {
        to.has_worker = 0;
    }
    add_timeout(to);
    return bender_wrap_fiber(tocancel);
}

BENDER_CORE_FN(cfun_ev_cancel,
              "(ev/cancel fib err)",
              "Cancel a suspended task fiber `fib` in the event loop. "
              "Differs from `cancel` in that it returns the canceled "
              "fiber immediately.") {
    bender_fixarity(argc, 2);
    BenderFiber *fiber = bender_getfiber(argv, 0);
    Bender err = argv[1];
    bender_cancel(fiber, err);
    return argv[0];
}

BENDER_CORE_FN(bender_cfun_stream_close,
              "(ev/close stream)",
              "Close a stream. This should be the same as calling (:close stream) for all streams.") {
    bender_fixarity(argc, 1);
    BenderStream *stream = bender_getabstract(argv, 0, &bender_stream_type);
    bender_stream_close(stream);
    return argv[0];
}

BENDER_CORE_FN(bender_cfun_stream_read,
              "(ev/read stream n &opt buf timeout)",
              "Read at most `n` bytes into a buffer asynchronously from a "
              "stream. `n` can also be `:all` to read into the buffer until "
              "end of stream. Optionally provide a buffer, `buf`, to write "
              "into and `timeout` in seconds after which to cancel the "
              "operation and raise an error. The timeout applies to this read "
              "alone: expiry resumes only the waiting fiber, and a completed "
              "read retires its own deadline, so repeated polling never "
              "accumulates timers that fire into unrelated work. Returns the "
              "buffer if the read "
              "was successful or nil if end-of-stream reached. Raises "
              "an error if there were problems with the IO operation.") {
    bender_arity(argc, 2, 4);
    BenderStream *stream = bender_getabstract(argv, 0, &bender_stream_type);
    bender_stream_flags(stream, BENDER_STREAM_READABLE);
    BenderBuffer *buffer = bender_optbuffer(argv, argc, 2, 10);
    double to = bender_optnumber(argv, argc, 3, INFINITY);
    if (bender_keyeq(argv[1], "all")) {
        if (to != INFINITY) bender_addtimeout(to);
        bender_ev_readchunk(stream, buffer, INT32_MAX);
    } else {
        int32_t n = bender_getnat(argv, 1);
        if (to != INFINITY) bender_addtimeout(to);
        bender_ev_read(stream, buffer, n);
    }
}

BENDER_CORE_FN(bender_cfun_stream_chunk,
              "(ev/chunk stream n &opt buf timeout)",
              "Same as `ev/read`, but does not return early if less than "
              "`n` bytes are available. If an end-of-stream is reached, "
              "returns early with the collected bytes.") {
    bender_arity(argc, 2, 4);
    BenderStream *stream = bender_getabstract(argv, 0, &bender_stream_type);
    bender_stream_flags(stream, BENDER_STREAM_READABLE);
    int32_t n = bender_getnat(argv, 1);
    BenderBuffer *buffer = bender_optbuffer(argv, argc, 2, 10);
    double to = bender_optnumber(argv, argc, 3, INFINITY);
    if (to != INFINITY) bender_addtimeout(to);
    bender_ev_readchunk(stream, buffer, n);
}

BENDER_CORE_FN(bender_cfun_stream_write,
              "(ev/write stream data &opt timeout)",
              "Write data to a stream, suspending the current fiber until the write "
              "completes. Takes an optional timeout in seconds, after which will return nil. "
              "Returns nil, or raises an error if the write failed.") {
    bender_arity(argc, 2, 3);
    BenderStream *stream = bender_getabstract(argv, 0, &bender_stream_type);
    bender_stream_flags(stream, BENDER_STREAM_WRITABLE);
    double to = bender_optnumber(argv, argc, 2, INFINITY);
    if (bender_checktype(argv[1], BENDER_BUFFER)) {
        if (to != INFINITY) bender_addtimeout(to);
        bender_ev_write_buffer(stream, bender_getbuffer(argv, 1));
    } else {
        BenderByteView bytes = bender_getbytes(argv, 1);
        if (to != INFINITY) bender_addtimeout(to);
        bender_ev_write_string(stream, bytes.bytes);
    }
}

static int mutexgc(void *p, size_t size) {
    (void) size;
    bender_os_mutex_deinit(p);
    return 0;
}

const BenderAbstractType bender_mutex_type = {
    "core/lock",
    mutexgc,
    BENDER_ATEND_GC
};

BENDER_CORE_FN(bender_cfun_mutex,
              "(ev/lock)",
              "Create a new lock to coordinate threads.") {
    bender_fixarity(argc, 0);
    (void) argv;
    void *mutex = bender_abstract_threaded(&bender_mutex_type, bender_os_mutex_size());
    bender_os_mutex_init(mutex);
    return bender_wrap_abstract(mutex);
}

BENDER_CORE_FN(bender_cfun_mutex_acquire,
              "(ev/acquire-lock lock)",
              "Acquire a lock such that this operating system thread is the only thread with access to this resource."
              " This will block this entire thread until the lock becomes available, and will not yield to other fibers "
              "on this system thread.") {
    bender_fixarity(argc, 1);
    void *mutex = bender_getabstract(argv, 0, &bender_mutex_type);
    bender_os_mutex_lock(mutex);
    return argv[0];
}

BENDER_CORE_FN(bender_cfun_mutex_release,
              "(ev/release-lock lock)",
              "Release a lock such that other threads may acquire it.") {
    bender_fixarity(argc, 1);
    void *mutex = bender_getabstract(argv, 0, &bender_mutex_type);
    bender_os_mutex_unlock(mutex);
    return argv[0];
}

static int rwlockgc(void *p, size_t size) {
    (void) size;
    bender_os_rwlock_deinit(p);
    return 0;
}

const BenderAbstractType bender_rwlock_type = {
    "core/rwlock",
    rwlockgc,
    BENDER_ATEND_GC
};

BENDER_CORE_FN(bender_cfun_rwlock,
              "(ev/rwlock)",
              "Create a new read-write lock to coordinate threads.") {
    bender_fixarity(argc, 0);
    (void) argv;
    void *rwlock = bender_abstract_threaded(&bender_rwlock_type, bender_os_rwlock_size());
    bender_os_rwlock_init(rwlock);
    return bender_wrap_abstract(rwlock);
}

BENDER_CORE_FN(bender_cfun_rwlock_read_lock,
              "(ev/acquire-rlock rwlock)",
              "Acquire a read lock an a read-write lock.") {
    bender_fixarity(argc, 1);
    void *rwlock = bender_getabstract(argv, 0, &bender_rwlock_type);
    bender_os_rwlock_rlock(rwlock);
    return argv[0];
}

BENDER_CORE_FN(bender_cfun_rwlock_write_lock,
              "(ev/acquire-wlock rwlock)",
              "Acquire a write lock on a read-write lock.") {
    bender_fixarity(argc, 1);
    void *rwlock = bender_getabstract(argv, 0, &bender_rwlock_type);
    bender_os_rwlock_wlock(rwlock);
    return argv[0];
}

BENDER_CORE_FN(bender_cfun_rwlock_read_release,
              "(ev/release-rlock rwlock)",
              "Release a read lock on a read-write lock") {
    bender_fixarity(argc, 1);
    void *rwlock = bender_getabstract(argv, 0, &bender_rwlock_type);
    bender_os_rwlock_runlock(rwlock);
    return argv[0];
}

BENDER_CORE_FN(bender_cfun_rwlock_write_release,
              "(ev/release-wlock rwlock)",
              "Release a write lock on a read-write lock") {
    bender_fixarity(argc, 1);
    void *rwlock = bender_getabstract(argv, 0, &bender_rwlock_type);
    bender_os_rwlock_wunlock(rwlock);
    return argv[0];
}

static BenderFile *get_file_for_stream(BenderStream *stream) {
    int32_t flags = 0;
    char fmt[4] = {0};
    int index = 0;
    if (stream->flags & BENDER_STREAM_READABLE) {
        flags |= BENDER_FILE_READ;
        bender_sandbox_assert(BENDER_SANDBOX_FS_READ);
        fmt[index++] = 'r';
    }
    if (stream->flags & BENDER_STREAM_WRITABLE) {
        flags |= BENDER_FILE_WRITE;
        bender_sandbox_assert(BENDER_SANDBOX_FS_WRITE);
        int currindex = index;
        fmt[index++] = (currindex == 0) ? 'w' : '+';
    }
    if (index == 0) return NULL;
    /* duplicate handle when converting stream to file */
    stream->flags &= ~BENDER_STREAM_NODUPS;
#ifdef BENDER_WINDOWS
    int htype = 0;
    if (fmt[0] == 'r' && fmt[1] == '+') {
        htype = _O_RDWR;
    } else if (fmt[0] == 'r') {
        htype = _O_RDONLY;
    } else if (fmt[0] == 'w') {
        htype = _O_WRONLY;
    }
    int fd = _open_osfhandle((intptr_t) stream->handle, htype);
    if (fd < 0) return NULL;
    int fd_dup = _dup(fd);
    if (fd_dup < 0) return NULL;
    FILE *f = _fdopen(fd_dup, fmt);
    if (NULL == f) {
        _close(fd_dup);
        return NULL;
    }
#else
    int fd_dup = dup(stream->handle);
    if (fd_dup < 0) return NULL;
    FILE *f = fdopen(fd_dup, fmt);
    if (NULL == f) {
        close(fd_dup);
        return NULL;
    }
#endif
    return bender_makejfile(f, flags);
}

BENDER_CORE_FN(bender_cfun_to_file,
              "(ev/to-file)",
              "Create core/file copy of the stream. This value can be used "
              "when blocking IO behavior is needed.") {
    bender_fixarity(argc, 1);
    BenderStream *stream = bender_getabstract(argv, 0, &bender_stream_type);
    BenderFile *iof = get_file_for_stream(stream);
    if (iof == NULL) bender_panic("cannot make file from stream");
    return bender_wrap_abstract(iof);
}

BENDER_CORE_FN(bender_cfun_ev_all_tasks,
              "(ev/all-tasks)",
              "Get an array of all active task fibers that are being used by the scheduler.") {
    bender_fixarity(argc, 0);
    (void) argv;
    BenderArray *array = bender_array(bender_vm.active_tasks.count);
    for (int32_t i = 0; i < bender_vm.active_tasks.capacity; i++) {
        if (!bender_checktype(bender_vm.active_tasks.data[i].key, BENDER_NIL)) {
            bender_array_push(array, bender_vm.active_tasks.data[i].key);
        }
    }
    return bender_wrap_array(array);
}

void bender_lib_ev(BenderTable *env) {
    BenderRegExt ev_cfuns_ext[] = {
        BENDER_CORE_REG("ev/give", cfun_channel_push),
        BENDER_CORE_REG("ev/take", cfun_channel_pop),
        BENDER_CORE_REG("ev/try-give", cfun_channel_try_push),
        BENDER_CORE_REG("ev/try-take", cfun_channel_try_pop),
        BENDER_CORE_REG("ev/full", cfun_channel_full),
        BENDER_CORE_REG("ev/capacity", cfun_channel_capacity),
        BENDER_CORE_REG("ev/count", cfun_channel_count),
        BENDER_CORE_REG("ev/select", cfun_channel_choice),
        BENDER_CORE_REG("ev/rselect", cfun_channel_rchoice),
        BENDER_CORE_REG("ev/chan", cfun_channel_new),
        BENDER_CORE_REG("ev/thread-chan", cfun_channel_new_threaded),
        BENDER_CORE_REG("ev/ichan", cfun_channel_new_isolated),
        BENDER_CORE_REG("ev/chan-close", cfun_channel_close),
        BENDER_CORE_REG("ev/go", cfun_ev_go),
        BENDER_CORE_REG("ev/thread", cfun_ev_thread),
        BENDER_CORE_REG("thread/new", cfun_thread_new),
        BENDER_CORE_REG("thread/join", cfun_thread_join),
        BENDER_CORE_REG("thread/alive?", cfun_thread_alive),
        BENDER_CORE_REG("thread/profile", cfun_thread_profile),
        BENDER_CORE_REG("pool/spawn", cfun_pool_spawn),
        BENDER_CORE_REG("pool/acquire", cfun_pool_acquire),
        BENDER_CORE_REG("pool/dispatch", cfun_pool_dispatch),
        BENDER_CORE_REG("pool/profile-harvest", cfun_pool_profile_harvest),
        BENDER_CORE_REG("pool/stop", cfun_pool_stop),
        BENDER_CORE_REG("ev/monitor", cfun_thread_monitor),
        BENDER_CORE_REG("ev/demonitor", cfun_thread_demonitor),
        BENDER_CORE_REG("ev/register", cfun_registry_register),
        BENDER_CORE_REG("ev/whereis", cfun_registry_whereis),
        BENDER_CORE_REG("ev/unregister", cfun_registry_unregister),
        BENDER_CORE_REG("shared/new", cfun_shared_new),
        BENDER_CORE_REG("shared/shareable?", cfun_shared_shareable),
        BENDER_CORE_REG("shared/length", cfun_shared_length),
        BENDER_CORE_REG("shared/get", cfun_shared_get),
        BENDER_CORE_REG("shared/materialize", cfun_shared_materialize),
        BENDER_CORE_REG("ev/give-supervisor", cfun_ev_give_supervisor),
        BENDER_CORE_REG("ev/interrupt", cfun_ev_interrupt),
        BENDER_CORE_REG("ev/timeslice", cfun_ev_timeslice),
        BENDER_CORE_REG("ev/pump", cfun_ev_pump),
        BENDER_CORE_REG("ev/sleep", cfun_ev_sleep),
        BENDER_CORE_REG("ev/deadline", cfun_ev_deadline),
        BENDER_CORE_REG("ev/cancel", cfun_ev_cancel),
        BENDER_CORE_REG("ev/close", bender_cfun_stream_close),
        BENDER_CORE_REG("ev/read", bender_cfun_stream_read),
        BENDER_CORE_REG("ev/chunk", bender_cfun_stream_chunk),
        BENDER_CORE_REG("ev/write", bender_cfun_stream_write),
        BENDER_CORE_REG("ev/lock", bender_cfun_mutex),
        BENDER_CORE_REG("ev/acquire-lock", bender_cfun_mutex_acquire),
        BENDER_CORE_REG("ev/release-lock", bender_cfun_mutex_release),
        BENDER_CORE_REG("ev/rwlock", bender_cfun_rwlock),
        BENDER_CORE_REG("ev/acquire-rlock", bender_cfun_rwlock_read_lock),
        BENDER_CORE_REG("ev/acquire-wlock", bender_cfun_rwlock_write_lock),
        BENDER_CORE_REG("ev/release-rlock", bender_cfun_rwlock_read_release),
        BENDER_CORE_REG("ev/release-wlock", bender_cfun_rwlock_write_release),
        BENDER_CORE_REG("ev/to-file", bender_cfun_to_file),
        BENDER_CORE_REG("ev/all-tasks", bender_cfun_ev_all_tasks),
        BENDER_REG_END
    };

    bender_core_cfuns_ext(env, NULL, ev_cfuns_ext);
    bender_register_abstract_type(&bender_stream_type);
    bender_register_abstract_type(&bender_channel_type);
    bender_register_abstract_type(&bender_mutex_type);
    bender_register_abstract_type(&bender_rwlock_type);
    bender_register_abstract_type(&bender_thread_handle_type);
    bender_register_abstract_type(&bender_pool_type);
    bender_register_abstract_type(&bender_shared_type);
}

#endif
