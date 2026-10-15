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
#include "fiber.h"
#include "proc.h"
#endif

#ifdef JANET_EV

#include <math.h>
#include <fcntl.h>
#ifdef JANET_WINDOWS
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
#ifdef JANET_EV_EPOLL
#include <sys/epoll.h>
#include <sys/timerfd.h>
#endif
#ifdef JANET_EV_KQUEUE
#include <sys/event.h>
#endif
#ifdef JANET_EV_POLL
#include <poll.h>
#endif
#endif

typedef struct JanetChanPendReg JanetChanPendReg;

typedef struct {
    JanetVM *thread;
    JanetFiber *fiber;
    uint32_t sched_id;
    enum {
        JANET_CP_MODE_READ,
        JANET_CP_MODE_WRITE,
        JANET_CP_MODE_CHOICE_READ,
        JANET_CP_MODE_CHOICE_WRITE,
        JANET_CP_MODE_CLOSE
    } mode;
    /* Back-pointer into the owning fiber's chan_pends registration
     * list. Unlink-on-free discipline: the node is claimed (and
     * freed) by whoever consumes the entry last — a channel pop, a
     * close revival, channel deinit, or fiber free. Fibers are only
     * ever dereferenced through an entry whose node proves them alive
     * (dead bit clear), replacing the unsound GC probe-on-read. */
    JanetChanPendReg *reg;
} JanetChannelPending;

/* Channel-pending registration (see JanetFiber.chan_pends). Malloc'd,
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
typedef struct JanetChanPendReg {
    struct JanetChanPendReg *next;
    JanetChannel *chan;
    uint8_t is_write;
    uint8_t in_queue;
    uint8_t dequeued;
    uint8_t dead;
    uint8_t chan_dead;
} JanetChanPendReg;

/* Drop finished registrations from the fiber's list (called on the
 * fiber's own thread at park time). Nodes are freed here exactly when
 * no channel-side reference can reach them anymore. */
static void janet_chanpends_prune(JanetFiber *f) {
    JanetChanPendReg **link = (JanetChanPendReg **) &f->chan_pends;
    while (NULL != *link) {
        JanetChanPendReg *reg = *link;
        /* chan_dead nodes are kept even once unqueued: a freed
         * channel's abandoned queue storage may still hold entries
         * that reference the node, and a push into that
         * freed-but-unreused channel (upstream's zombie discipline)
         * must find a valid node, never freed memory. They are
         * reclaimed by the zombie claim or, absent one, leak bounded
         * records on this broken-ownership path. */
        if (reg->dead || reg->dequeued || (!reg->in_queue && !reg->chan_dead)) {
            *link = reg->next;
            janet_free(reg);
        } else {
            link = &reg->next;
        }
    }
}

/* Register a new pending entry: node onto the fiber list, then the
 * caller fills pending.reg and pushes to the channel queue. */
static JanetChanPendReg *janet_chanpend_register(JanetFiber *f, JanetChannel *chan, int is_write) {
    janet_chanpends_prune(f);
    JanetChanPendReg *reg = janet_malloc(sizeof(JanetChanPendReg));
    reg->next = (JanetChanPendReg *) f->chan_pends;
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
static void janet_chanpends_unlink(JanetFiber *f, JanetChanPendReg *reg) {
    JanetChanPendReg **link = (JanetChanPendReg **) &f->chan_pends;
    while (NULL != *link) {
        if (*link == reg) {
            *link = reg->next;
            break;
        }
        link = &(*link)->next;
    }
    janet_free(reg);
}

/* Consume a same-VM pending entry popped from a plain/isolated
 * channel queue. The dead bit makes the fiber-liveness check sound
 * (no GC probing): a dead fiber is never dereferenced, its entry is
 * just reclaimed. Returns 1 when pending.fiber may be scheduled
 * (alive and sched-consistent), 0 when the entry was consumed as
 * stale. */
static int janet_chanpend_claim_plain(JanetChannelPending *p) {
    JanetChanPendReg *reg = p->reg;
    reg->in_queue = 0;
    if (reg->dead) {
        janet_free(reg);
        return 0;
    }
    int live = p->sched_id == p->fiber->sched_id;
    janet_chanpends_unlink(p->fiber, reg);
    return live;
}

/* Consume a threaded-channel entry popped under the channel lock:
 * only the node is touched (the fiber may live on another thread).
 * The owner thread reclaims the node at its next park or at free. */
static void janet_chanpend_claim_threaded(JanetChannelPending *p) {
    JanetChanPendReg *reg = p->reg;
    reg->in_queue = 0;
    reg->dequeued = 1;
}

/* Fiber free hook (called from janet_deinit_block's fiber case).
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
void janet_ev_chanpends_deinit(JanetFiber *f) {
    JanetChanPendReg *reg = (JanetChanPendReg *) f->chan_pends;
    while (NULL != reg) {
        JanetChanPendReg *next = reg->next;
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
            janet_free(reg);
        }
        reg = next;
    }
    f->chan_pends = NULL;
}

/* GC reverse edge (called from janet_mark_fiber): a fiber with
 * pending registrations pins its channels, mirroring the channel's
 * own pin on parked fibers (mark_fq). Without this, a scoped or
 * global sweep could free a channel out from under a parked fiber's
 * pending entry (observed: proc-scoped collection freeing an
 * attributed mailbox). Only entries still linked into a channel queue
 * own that reverse edge: claimed/dequeued nodes remain on the fiber's
 * list until its next park, but their channel can already be freed.
 * Dead-flagged nodes reference freed fibers (skip), and chan_dead
 * nodes reference freed channels (skip). */
void janet_ev_chanpends_mark(JanetFiber *f) {
    JanetChanPendReg *reg = (JanetChanPendReg *) f->chan_pends;
    while (NULL != reg) {
        if (reg->in_queue && !reg->chan_dead && !reg->dead) {
            janet_mark(janet_wrap_abstract(reg->chan));
        }
        reg = reg->next;
    }
}

struct JanetChannel {
    JanetQueue items;
    JanetQueue read_pending;
    JanetQueue write_pending;
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
    /* Step 4.1 home-lane pin: the creating/unmarshalling thread's VM.
     * Isolated channels are lock-free, so foreign-thread use would be
     * a silent race; push/pop refuse it loudly instead. Plain and
     * threaded channels leave this NULL (legacy behavior unchanged). */
    JanetVM *owner;
#ifdef JANET_WINDOWS
    CRITICAL_SECTION lock;
#else
    pthread_mutex_t lock;
#endif
};

typedef struct {
    JanetFiber *fiber;
    Janet value;
    JanetSignal sig;
    uint32_t expected_sched_id; /* If the fiber has been rescheduled this loop, don't run first scheduling. */
} JanetTask;

/* Wrap return value by pairing it with the callback used to handle it
 * in the main thread */
typedef struct {
    JanetEVGenericMessage msg;
    JanetThreadedCallback cb;
} JanetSelfPipeEvent;

/* Structure used to initialize threads in the thread pool
 * (same head structure as self pipe event)*/
typedef struct {
    JanetEVGenericMessage msg;
    JanetThreadedCallback cb;
    JanetThreadedSubroutine subr;
    JanetHandle write_pipe;
} JanetEVThreadInit;

/* Structure used to initialize threads that run timeouts */
typedef struct {
    double sec;
    JanetVM *vm;
    JanetFiber *fiber;
#ifdef JANET_WINDOWS
    HANDLE cancel_event;
#endif
} JanetThreadedTimeout;

#define JANET_MAX_Q_CAPACITY 0x7FFFFFF

static void janet_q_init(JanetQueue *q) {
    q->data = NULL;
    q->head = 0;
    q->tail = 0;
    q->capacity = 0;
}

static void janet_q_deinit(JanetQueue *q) {
    janet_free(q->data);
}

static int32_t janet_q_count(JanetQueue *q) {
    return (q->head > q->tail)
           ? (q->tail + q->capacity - q->head)
           : (q->tail - q->head);
}

static int janet_q_maybe_resize(JanetQueue *q, size_t itemsize) {
    int32_t count = janet_q_count(q);
    /* Resize if needed */
    if (count + 1 >= q->capacity) {
        if (count + 1 >= JANET_MAX_Q_CAPACITY) return 1;
        int32_t newcap = (count + 2) * 2;
        if (newcap > JANET_MAX_Q_CAPACITY) newcap = JANET_MAX_Q_CAPACITY;
        q->data = janet_realloc(q->data, itemsize * newcap);
        if (NULL == q->data) {
            JANET_OUT_OF_MEMORY;
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

static int janet_q_push(JanetQueue *q, void *item, size_t itemsize) {
    if (janet_q_maybe_resize(q, itemsize)) return 1;
    memcpy((char *) q->data + itemsize * q->tail, item, itemsize);
    q->tail = q->tail + 1 < q->capacity ? q->tail + 1 : 0;
    return 0;
}

static int janet_q_push_head(JanetQueue *q, void *item, size_t itemsize) {
    if (janet_q_maybe_resize(q, itemsize)) return 1;
    int32_t newhead = q->head - 1;
    if (newhead < 0) {
        newhead += q->capacity;
    }
    memcpy((char *) q->data + itemsize * newhead, item, itemsize);
    q->head = newhead;
    return 0;
}

static int janet_q_pop(JanetQueue *q, void *out, size_t itemsize) {
    if (q->head == q->tail) return 1;
    memcpy(out, (char *) q->data + itemsize * q->head, itemsize);
    q->head = q->head + 1 < q->capacity ? q->head + 1 : 0;
    return 0;
}

/* Get current timestamp (millisecond precision) */
static JanetTimestamp ts_now(void);

/* Get current timestamp + an interval (millisecond precision) */
static JanetTimestamp ts_delta(JanetTimestamp ts, double delta) {
    if (isinf(delta)) {
        return delta < 0 ? ts : INT64_MAX;
    }
    ts += (int64_t)round(delta * 1000);
    return ts;
}

/* Look at the next timeout value without removing it. */
static int peek_timeout(JanetTimeout *out) {
    if (janet_vm.tq_count == 0) return 0;
    *out = janet_vm.tq[0];
    return 1;
}

/* Remove the next timeout from the priority queue */
static void pop_timeout(size_t index) {
    if (janet_vm.tq_count <= index) return;
    janet_vm.tq[index] = janet_vm.tq[--janet_vm.tq_count];
    for (;;) {
        size_t left = (index << 1) + 1;
        size_t right = left + 1;
        size_t smallest = index;
        if (left < janet_vm.tq_count &&
                (janet_vm.tq[left].when < janet_vm.tq[smallest].when))
            smallest = left;
        if (right < janet_vm.tq_count &&
                (janet_vm.tq[right].when < janet_vm.tq[smallest].when))
            smallest = right;
        if (smallest == index) return;
        JanetTimeout temp = janet_vm.tq[index];
        janet_vm.tq[index] = janet_vm.tq[smallest];
        janet_vm.tq[smallest] = temp;
        index = smallest;
    }
}

/* Add a timeout to the timeout min heap */
static void add_timeout(JanetTimeout to) {
    size_t oldcount = janet_vm.tq_count;
    size_t newcount = oldcount + 1;
    if (newcount > janet_vm.tq_capacity) {
        size_t newcap = 2 * newcount;
        JanetTimeout *tq = janet_realloc(janet_vm.tq, newcap * sizeof(JanetTimeout));
        if (NULL == tq) {
            JANET_OUT_OF_MEMORY;
        }
        janet_vm.tq = tq;
        janet_vm.tq_capacity = newcap;
    }
    /* Append */
    janet_vm.tq_count = (int32_t) newcount;
    janet_vm.tq[oldcount] = to;
    /* Heapify */
    size_t index = oldcount;
    while (index > 0) {
        size_t parent = (index - 1) >> 1;
        if (janet_vm.tq[parent].when <= janet_vm.tq[index].when) break;
        /* Swap */
        JanetTimeout tmp = janet_vm.tq[index];
        janet_vm.tq[index] = janet_vm.tq[parent];
        janet_vm.tq[parent] = tmp;
        /* Next */
        index = parent;
    }
}

void janet_async_end(JanetFiber *fiber) {
    if (fiber->ev_callback) {
        if (fiber->ev_stream->read_fiber == fiber) {
            fiber->ev_stream->read_fiber = NULL;
        }
        if (fiber->ev_stream->write_fiber == fiber) {
            fiber->ev_stream->write_fiber = NULL;
        }
        fiber->ev_callback(fiber, JANET_ASYNC_EVENT_DEINIT);
        janet_gcunroot(janet_wrap_abstract(fiber->ev_stream));
        fiber->ev_callback = NULL;
        if (!(fiber->flags & JANET_FIBER_EV_FLAG_IN_FLIGHT)) {
            if (fiber->ev_state) {
                janet_free(fiber->ev_state);
                fiber->ev_state = NULL;
            }
            janet_ev_dec_refcount();
        }
    }
}

void janet_async_in_flight(JanetFiber *fiber) {
#ifdef JANET_WINDOWS
    fiber->flags |= JANET_FIBER_EV_FLAG_IN_FLIGHT;
#else
    (void) fiber;
#endif
}

void janet_async_start_fiber(JanetFiber *fiber, JanetStream *stream, JanetAsyncMode mode, JanetEVCallback callback, void *state) {
    janet_assert(!fiber->ev_callback, "double async on fiber");
    if (mode & JANET_ASYNC_LISTEN_READ) {
        stream->read_fiber = fiber;
    }
    if (mode & JANET_ASYNC_LISTEN_WRITE) {
        stream->write_fiber = fiber;
    }
    fiber->ev_callback = callback;
    fiber->ev_stream = stream;
    janet_ev_inc_refcount();
    janet_gcroot(janet_wrap_abstract(stream));
    fiber->ev_state = state;
    callback(fiber, JANET_ASYNC_EVENT_INIT);
}

void janet_async_start(JanetStream *stream, JanetAsyncMode mode, JanetEVCallback callback, void *state) {
    janet_async_start_fiber(janet_vm.root_fiber, stream, mode, callback, state);
    janet_await();
}

void janet_fiber_did_resume(JanetFiber *fiber) {
    janet_async_end(fiber);
}

static void janet_stream_checktoclose(JanetStream *stream) {
    if ((stream->flags & JANET_STREAM_TOCLOSE) && !stream->read_fiber && !stream->write_fiber) {
        janet_stream_close(stream);
    }
}

/* Forward declaration */
static void janet_register_stream(JanetStream *stream);
#ifndef JANET_WINDOWS
static void janet_unregister_stream(JanetStream *stream);
#endif

static const JanetMethod ev_default_stream_methods[] = {
    {"close", janet_cfun_stream_close},
    {"read", janet_cfun_stream_read},
    {"chunk", janet_cfun_stream_chunk},
    {"write", janet_cfun_stream_write},
    {NULL, NULL}
};

/* Create a stream*/
JanetStream *janet_stream_ext(JanetHandle handle, uint32_t flags, const JanetMethod *methods, size_t size) {
    janet_assert(size >= sizeof(JanetStream), "bad size");
    JanetStream *stream = janet_abstract(&janet_stream_type, size);
    stream->handle = handle;
    stream->flags = flags;
    stream->read_fiber = NULL;
    stream->write_fiber = NULL;
    if (methods == NULL) methods = ev_default_stream_methods;
    stream->methods = methods;
    stream->index = 0;
    janet_register_stream(stream);
    return stream;
}

JanetStream *janet_stream(JanetHandle handle, uint32_t flags, const JanetMethod *methods) {
    return janet_stream_ext(handle, flags, methods, sizeof(JanetStream));
}

static void janet_stream_close_impl(JanetStream *stream) {
    stream->flags |= JANET_STREAM_CLOSED;
    int canclose = !(stream->flags & JANET_STREAM_NOT_CLOSEABLE);
    /* If we are positive that the stream is the last instance of the underlying file description (such that "dup" was never called on the file description)
     * then skip unregister to save a syscall */
#ifdef JANET_WINDOWS
    if (stream->handle != INVALID_HANDLE_VALUE) {
#ifdef JANET_NET
        if (stream->flags & JANET_STREAM_SOCKET) {
            if (canclose) closesocket((SOCKET) stream->handle);
        } else
#endif
        {
            if (canclose) CloseHandle(stream->handle);
        }
        stream->handle = INVALID_HANDLE_VALUE;
    }
#else
    int canunregister = !(stream->flags & JANET_STREAM_UNREGISTERED);
    if (stream->handle != -1) {
        if (canunregister) janet_unregister_stream(stream);
        if (canclose) close(stream->handle);
        stream->handle = -1;
    }
#endif
}

void janet_stream_close(JanetStream *stream) {
    JanetFiber *rf = stream->read_fiber;
    JanetFiber *wf = stream->write_fiber;
    if (rf && rf->ev_callback) {
        rf->ev_callback(rf, JANET_ASYNC_EVENT_CLOSE);
        stream->read_fiber = NULL;
    }
    if (wf && wf->ev_callback) {
        wf->ev_callback(wf, JANET_ASYNC_EVENT_CLOSE);
        stream->write_fiber = NULL;
    }
    janet_stream_close_impl(stream);
}

/* Called to clean up a stream */
static int janet_stream_gc(void *p, size_t s) {
    (void) s;
    JanetStream *stream = (JanetStream *)p;
    janet_stream_close_impl(stream);
    return 0;
}

/* Mark a stream for GC */
static int janet_stream_mark(void *p, size_t s) {
    (void) s;
    JanetStream *stream = (JanetStream *) p;
    JanetFiber *rf = stream->read_fiber;
    JanetFiber *wf = stream->write_fiber;
    if (rf) {
        janet_mark(janet_wrap_fiber(rf));
    }
    if (wf) {
        janet_mark(janet_wrap_fiber(wf));
    }
    return 0;
}

static int janet_stream_getter(void *p, Janet key, Janet *out) {
    JanetStream *stream = (JanetStream *)p;
    if (!janet_checktype(key, JANET_KEYWORD)) return 0;
    const JanetMethod *stream_methods = stream->methods;
    return janet_getmethod(janet_unwrap_keyword(key), stream_methods, out);
}

static void janet_stream_marshal(void *p, JanetMarshalContext *ctx) {
    JanetStream *s = p;
    if (!(janet_marshal_flags(ctx) & JANET_MARSHAL_UNSAFE)) {
        janet_panic("can only marshal stream with unsafe flag");
    }
    s->flags &= ~JANET_STREAM_NODUPS; /* This stream now might be duplicated, invalidates some EV optimizations */
    janet_marshal_abstract(ctx, p);
    janet_marshal_int(ctx, (int32_t) s->flags);
    janet_marshal_ptr(ctx, s->methods);
#ifdef JANET_WINDOWS
    /* TODO - ref counting to avoid situation where a handle is closed or GCed
     * while in transit, and it's value gets reused. DuplicateHandle does not work
     * for network sockets, and in general for winsock it is better to not duplicate
     * unless there is a need to. */
    HANDLE duph = INVALID_HANDLE_VALUE;
    if (s->flags & JANET_STREAM_SOCKET) {
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
    janet_marshal_int64(ctx, (int64_t)(duph));
#else
    /* Marshal after dup because it is easier than maintaining our own ref counting. */
    int duph = dup(s->handle);
    if (duph < 0) janet_panicf("failed to duplicate stream handle: %V", janet_ev_lasterr());
    janet_marshal_int(ctx, (int32_t)(duph));
#endif
}

static void *janet_stream_unmarshal(JanetMarshalContext *ctx) {
    if (!(janet_unmarshal_flags(ctx) & JANET_MARSHAL_UNSAFE)) {
        janet_panic("can only unmarshal stream with unsafe flag");
    }
    JanetStream *p = janet_unmarshal_abstract(ctx, sizeof(JanetStream));
    /* Can't share listening state and such across threads */
    p->read_fiber = NULL;
    p->write_fiber = NULL;
    p->flags = (uint32_t) janet_unmarshal_int(ctx);
    p->methods =  janet_unmarshal_ptr(ctx);
#ifdef JANET_WINDOWS
    p->handle = (JanetHandle) janet_unmarshal_int64(ctx);
#else
    p->handle = (JanetHandle) janet_unmarshal_int(ctx);
#endif
#ifdef JANET_EV_POLL
    janet_register_stream(p);
#endif
    return p;
}

static Janet janet_stream_next(void *p, Janet key) {
    JanetStream *stream = (JanetStream *)p;
    return janet_nextmethod(stream->methods, key);
}

static void janet_stream_tostring(void *p, JanetBuffer *buffer) {
    JanetStream *stream = p;
    /* Let user print the file descriptor for debugging */
    janet_formatb(buffer, "[fd=%d]", stream->handle);
}

const JanetAbstractType janet_stream_type = {
    "core/stream",
    janet_stream_gc,
    janet_stream_mark,
    janet_stream_getter,
    NULL,
    janet_stream_marshal,
    janet_stream_unmarshal,
    janet_stream_tostring,
    NULL,
    NULL,
    janet_stream_next,
    JANET_ATEND_NEXT
};

/* Register a fiber to resume with value */
static void janet_schedule_general(JanetFiber *fiber, Janet value, JanetSignal sig, int soon) {
    if (fiber->gc.flags & JANET_FIBER_EV_GCFLAG_CANCELED) return;
    if (!(fiber->gc.flags & JANET_FIBER_EV_GCFLAG_ROOT)) {
        fiber->gc.flags |= JANET_FIBER_EV_GCFLAG_ROOT;
        Janet task_element = janet_wrap_fiber(fiber);
        janet_table_put(&janet_vm.active_tasks, task_element, janet_wrap_true());
    }
    JanetTask t = { fiber, value, sig, ++fiber->sched_id };
    if (sig == JANET_SIGNAL_ERROR) fiber->gc.flags |= JANET_FIBER_EV_GCFLAG_CANCELED;
    if (soon) {
        janet_assert(!janet_q_push_head(&janet_vm.spawn, &t, sizeof(t)), "schedule queue overflow");
    } else {
        janet_assert(!janet_q_push(&janet_vm.spawn, &t, sizeof(t)), "schedule queue overflow");
    }
}

void janet_schedule_signal(JanetFiber *fiber, Janet value, JanetSignal sig) {
    janet_schedule_general(fiber, value, sig, 0);
}

void janet_schedule_soon(JanetFiber *fiber, Janet value, JanetSignal sig) {
    janet_schedule_general(fiber, value, sig, 1);
}

void janet_cancel(JanetFiber *fiber, Janet value) {
    if (!(fiber->gc.flags & JANET_FIBER_EV_GCFLAG_ROOT)) {
        janet_panic("cannot cancel non-task fiber");
    }
    janet_schedule_signal(fiber, value, JANET_SIGNAL_ERROR);
}

void janet_schedule(JanetFiber *fiber, Janet value) {
    janet_schedule_signal(fiber, value, JANET_SIGNAL_OK);
}

/* Mark all pending tasks */
void janet_ev_mark(void) {

    /* Pending tasks */
    JanetTask *tasks = janet_vm.spawn.data;
    if (janet_vm.spawn.head <= janet_vm.spawn.tail) {
        for (int32_t i = janet_vm.spawn.head; i < janet_vm.spawn.tail; i++) {
            janet_mark(janet_wrap_fiber(tasks[i].fiber));
            janet_mark(tasks[i].value);
        }
    } else {
        for (int32_t i = janet_vm.spawn.head; i < janet_vm.spawn.capacity; i++) {
            janet_mark(janet_wrap_fiber(tasks[i].fiber));
            janet_mark(tasks[i].value);
        }
        for (int32_t i = 0; i < janet_vm.spawn.tail; i++) {
            janet_mark(janet_wrap_fiber(tasks[i].fiber));
            janet_mark(tasks[i].value);
        }
    }

    /* Pending timeouts */
    for (size_t i = 0; i < janet_vm.tq_count; i++) {
        janet_mark(janet_wrap_fiber(janet_vm.tq[i].fiber));
        if (janet_vm.tq[i].curr_fiber != NULL) {
            janet_mark(janet_wrap_fiber(janet_vm.tq[i].curr_fiber));
        }
    }
}

/* Push/pop bodies live here; proc.c shares them via ev.h (Step P1). */
int janet_channel_push(JanetChannel *channel, Janet x, int mode);
int janet_channel_pop(JanetChannel *channel, Janet *item, int is_choice);
/* Phase C: harvest helper defined with the pool code below (used by
 * both thread/new completions and pool worker exits). */
static int janet_profile_harvest_bytes(uint8_t **out_bytes, size_t *out_len);
/* NOTE(other-stream): forward declaration to unblock the build; the
 * owning change should place this with its own declarations. */
static int janet_profile_harvest_bytes(uint8_t **out_bytes, size_t *out_len);

static Janet make_supervisor_event(const char *name, JanetFiber *fiber, int threaded) {
    Janet tup[3];
    tup[0] = janet_ckeywordv(name);
    tup[1] = threaded ? fiber->last_value : janet_wrap_fiber(fiber) ;
    if (fiber->env != NULL) {
        tup[2] = janet_table_get(fiber->env, janet_ckeywordv("task-id"));
    } else {
        tup[2] = janet_wrap_nil();
    }
    return janet_wrap_tuple(janet_tuple_n(tup, 3));
}

/* Common init code */
void janet_ev_init_common(void) {
    janet_q_init(&janet_vm.spawn);
    janet_vm.tq = NULL;
    janet_vm.tq_count = 0;
    janet_vm.tq_capacity = 0;
    janet_table_init_raw(&janet_vm.threaded_abstracts, 0);
    janet_table_init_raw(&janet_vm.active_tasks, 0);
    janet_table_init_raw(&janet_vm.signal_handlers, 0);
    janet_rng_seed(&janet_vm.ev_rng, 0);
#ifndef JANET_WINDOWS
    pthread_attr_init(&janet_vm.new_thread_attr);
    pthread_attr_setdetachstate(&janet_vm.new_thread_attr, PTHREAD_CREATE_DETACHED);
#endif
}

#if JANET_ANDROID
static void janet_timeout_stop(int sig_num) {
    if (sig_num == SIGUSR1) {
        pthread_exit(NULL);
    }
}
#endif

static void handle_timeout_worker(JanetTimeout to, int cancel) {
    if (!to.has_worker) return;
#ifdef JANET_WINDOWS
    if (cancel && to.worker_event) {
        SetEvent(to.worker_event);
    }
    WaitForSingleObject(to.worker, INFINITE);
    CloseHandle(to.worker);
    if (to.worker_event) {
        CloseHandle(to.worker_event);
    }
#else
#ifdef JANET_ANDROID
    if (cancel) janet_assert(!pthread_kill(to.worker, SIGUSR1), "pthread_kill");
#else
    if (cancel) janet_assert(!pthread_cancel(to.worker), "pthread_cancel");
#endif
    void *res = NULL;
    janet_assert(!pthread_join(to.worker, &res), "pthread_join");
#endif
}

/* Common deinit code */
void janet_ev_deinit_common(void) {
    JanetTimeout to;
    while (peek_timeout(&to)) {
        handle_timeout_worker(to, 1);
        pop_timeout(0);
    }
    janet_q_deinit(&janet_vm.spawn);
    janet_free(janet_vm.tq);
    janet_table_deinit(&janet_vm.threaded_abstracts);
    janet_table_deinit(&janet_vm.active_tasks);
    janet_table_deinit(&janet_vm.signal_handlers);
#ifndef JANET_WINDOWS
    pthread_attr_destroy(&janet_vm.new_thread_attr);
#endif
}

/* Shorthand to yield to event loop */
void janet_await(void) {
    /* Store the fiber in a global table */
    janet_signalv(JANET_SIGNAL_EVENT, janet_wrap_nil());
}

/* Set timeout for the current root fiber */
void janet_addtimeout(double sec) {
    JanetFiber *fiber = janet_vm.root_fiber;
    JanetTimeout to;
    to.when = ts_delta(ts_now(), sec);
    to.fiber = fiber;
    to.curr_fiber = NULL;
    to.sched_id = fiber->sched_id;
    to.is_error = 1;
    to.has_worker = 0;
    add_timeout(to);
}

/* Set timeout for the current root fiber but resume with nil instead of raising an error */
void janet_addtimeout_nil(double sec) {
    JanetFiber *fiber = janet_vm.root_fiber;
    JanetTimeout to;
    to.when = ts_delta(ts_now(), sec);
    to.fiber = fiber;
    to.curr_fiber = NULL;
    to.sched_id = fiber->sched_id;
    to.is_error = 0;
    to.has_worker = 0;
    add_timeout(to);
}

static void janet_timeout_cb(JanetEVGenericMessage msg) {
    (void) msg;
    janet_interpreter_interrupt_handled(&janet_vm);
}

#ifdef JANET_WINDOWS
static DWORD WINAPI janet_timeout_body(LPVOID ptr) {
    JanetThreadedTimeout tto = *(JanetThreadedTimeout *)ptr;
    janet_free(ptr);
    JanetTimestamp wait_begin = ts_now();
    DWORD duration = (DWORD)round(tto.sec * 1000);
    DWORD res = WAIT_TIMEOUT;
    JanetTimestamp wait_end = ts_now();
    for (DWORD i = 1; res == WAIT_TIMEOUT && (wait_end - wait_begin) < duration; i++) {
        res = WaitForSingleObject(tto.cancel_event, (duration + i));
        wait_end = ts_now();
    }
    /* only send interrupt message if result is WAIT_TIMEOUT */
    if (res == WAIT_TIMEOUT) {
        janet_interpreter_interrupt(tto.vm);
        JanetEVGenericMessage msg = {0};
        janet_ev_post_event(tto.vm, janet_timeout_cb, msg);
    }
    return 0;
}
#else
static void *janet_timeout_body(void *ptr) {
#ifdef JANET_ANDROID
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0;
    action.sa_handler = &janet_timeout_stop;
    sigaction(SIGUSR1, &action, NULL);
#endif
    JanetThreadedTimeout tto = *(JanetThreadedTimeout *)ptr;
    janet_free(ptr);
    struct timespec ts;
    ts.tv_sec = (time_t) tto.sec;
    ts.tv_nsec = (tto.sec <= UINT32_MAX)
                 ? (long)((tto.sec - ((uint32_t)tto.sec)) * 1000000000)
                 : 0;
    nanosleep(&ts, &ts);
    janet_interpreter_interrupt(tto.vm);
    JanetEVGenericMessage msg = {0};
    janet_ev_post_event(tto.vm, janet_timeout_cb, msg);
    return NULL;
}
#endif


void janet_ev_inc_refcount(void) {
    janet_atomic_inc(&janet_vm.listener_count);
}

void janet_ev_dec_refcount(void) {
    janet_atomic_dec(&janet_vm.listener_count);
}

/* Channels */

#define JANET_MAX_CHANNEL_CAPACITY 0xFFFFFF

static inline int janet_chan_is_threaded(JanetChannel *chan) {
    return chan->is_threaded;
}

/* Step 1.4: values are marshal-copied through isolated channels the
 * same way as threaded ones (same transit ownership); only the lock
 * stays threaded-only (isolated channels are single-VM by contract). */
static inline int janet_chan_needs_pack(JanetChannel *chan) {
    return chan->is_threaded || chan->isolated;
}

static int janet_chan_pack(JanetChannel *chan, Janet *x) {
    if (!janet_chan_needs_pack(chan)) return 0;
    switch (janet_type(*x)) {
        default: {
            JanetBuffer *buf = janet_malloc(sizeof(JanetBuffer));
            if (NULL == buf) {
                JANET_OUT_OF_MEMORY;
            }
            janet_buffer_init(buf, 10);
            janet_marshal(buf, *x, NULL, JANET_MARSHAL_UNSAFE);
            *x = janet_wrap_buffer(buf);
            return 0;
        }
        case JANET_NIL:
        case JANET_NUMBER:
        case JANET_POINTER:
        case JANET_BOOLEAN:
        case JANET_CFUNCTION:
            return 0;
    }
}

static int janet_chan_unpack(JanetChannel *chan, Janet *x, int is_cleanup) {
    if (!janet_chan_needs_pack(chan)) return 0;
    switch (janet_type(*x)) {
        default:
            return 1;
        case JANET_BUFFER: {
            JanetBuffer *buf = janet_unwrap_buffer(*x);
            int flags = is_cleanup ? (JANET_MARSHAL_UNSAFE | JANET_MARSHAL_DECREF) : JANET_MARSHAL_UNSAFE;
            *x = janet_unmarshal(buf->data, buf->count, flags, NULL, NULL);
            janet_buffer_deinit(buf);
            janet_free(buf);
            return 0;
        }
        case JANET_NIL:
        case JANET_NUMBER:
        case JANET_POINTER:
        case JANET_BOOLEAN:
        case JANET_CFUNCTION:
            return 0;
    }
}

static void janet_chan_init(JanetChannel *chan, int32_t limit, int threaded, int isolated) {
    chan->limit = limit;
    chan->closed = 0;
    chan->is_threaded = threaded;
    chan->isolated = isolated;
    chan->proc_alive = 1;
    /* Step 4.1: isolated channels pin to the home lane (this thread's
     * VM — one VM per thread, so the address is the lane identity).
     * Unmarshal re-inits, so ownership follows the value. */
    chan->owner = isolated ? &janet_vm : NULL;
    janet_q_init(&chan->items);
    janet_q_init(&chan->read_pending);
    janet_q_init(&chan->write_pending);
    janet_os_mutex_init((JanetOSMutex *) &chan->lock);
}

/* (The old janet_chan_purge_fiber is gone: it was dead code — no
 * death path ever called it, which is how stale pendings survived to
 * crash push walks. The chan_pends registration makes reclamation
 * structural instead: fiber free marks its queued entries dead, and
 * every consumer path reclaims them. Root balance still mirrors
 * registration: only threaded pendings root, and every threaded
 * claim/unroot is preserved below.) */

void janet_channel_set_proc_alive(JanetChannel *chan, int alive) {
    chan->proc_alive = alive;
}

int janet_channel_proc_alive(JanetChannel *chan) {
    return chan->proc_alive;
}

static void janet_chan_lock(JanetChannel *chan) {
    if (!janet_chan_is_threaded(chan)) return;
    janet_os_mutex_lock((JanetOSMutex *) &chan->lock);
}

static void janet_chan_unlock(JanetChannel *chan) {
    if (!janet_chan_is_threaded(chan)) return;
    janet_os_mutex_unlock((JanetOSMutex *) &chan->lock);
}

/* Drain one pending queue at channel deinit: entries die with the
 * queue, nodes are released or left to their (living) fibers with
 * chan_dead set. Never dereferences fibers — only nodes. */
static void flag_chanpend_node(JanetChannelPending *p) {
    JanetChanPendReg *reg = p->reg;
    if (NULL != reg) {
        reg->in_queue = 0;
        reg->chan_dead = 1;
        /* Only dead-fiber nodes free here. A dequeued node whose
         * fiber still lives remains on that fiber's chan_pends list —
         * freeing it here would leave a dangling entry for the
         * fiber's prune/deinit to double-free. The fiber reclaims it
         * (dequeued ⇒ prune's free branch); a dead fiber's node has
         * no future walker, so it frees here. */
        if (reg->dead) janet_free(reg);
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
static void janet_chan_deinit_q(JanetQueue *q) {
    JanetChannelPending *pending = q->data;
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
        JanetChanPendReg *dead[64];
        size_t ndead = 0;
        int overflow = 0;
        for (int r = 0; r < nranges; r++) {
            for (int32_t i = ranges[r][0]; i < ranges[r][1]; i++) {
                JanetChanPendReg *reg = pending[i].reg;
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
        for (size_t k = 0; k < ndead; k++) janet_free(dead[k]);
        if (overflow) {
            /* Rare: more than 64 dead regs. Sweep again for the
             * remainder (slots were left intact for this pass). */
            for (int r = 0; r < nranges; r++) {
                for (int32_t i = ranges[r][0]; i < ranges[r][1]; i++) {
                    JanetChanPendReg *reg = pending[i].reg;
                    if (NULL != reg && reg->dead) {
                        janet_free(reg);
                        pending[i].reg = NULL;
                    }
                }
            }
        }
    }
    janet_q_deinit(q);
}

static void janet_chan_deinit(JanetChannel *chan) {
    /* LOCK the whole deinit: the pending-queue walk reads head/tail
     * bounds and reg nodes that foreign threads mutate concurrently
     * (claim pops, prune frees). Previously only pack channels took
     * the lock here, and the density teardown volume exposed the
     * unlocked walk as a use-after-free (ASAN: flag_chanpend_node
     * reading a node freed by the owning fiber's VM mid-walk). */
    janet_chan_lock(chan);
    if (janet_chan_needs_pack(chan)) {
        Janet item;
        janet_chan_deinit_q(&chan->read_pending);
        janet_chan_deinit_q(&chan->write_pending);
        while (!janet_q_pop(&chan->items, &item, sizeof(item))) {
            janet_chan_unpack(chan, &item, 1);
        }
        janet_q_deinit(&chan->items);
    } else {
        janet_chan_deinit_q(&chan->read_pending);
        janet_chan_deinit_q(&chan->write_pending);
        janet_q_deinit(&chan->items);
    }
    janet_chan_unlock(chan);
    janet_os_mutex_deinit((JanetOSMutex *) &chan->lock);
}

/*
 * Janet Channel abstract type
 */

static Janet janet_wrap_channel(JanetChannel *channel) {
    return janet_wrap_abstract(channel);
}

/* proc/kill on a recv-parked victim: remove the fiber's read pendings
 * from an isolated (same-VM, non-threaded) channel and error-resume it
 * so the kill lands immediately instead of at a next resume that may
 * never come. recv is own-mailbox only, so pendings found here belong
 * to the victim on this VM. Returns 1 when a pending was cancelled. */
int janet_channel_cancel_reader(JanetChannel *channel, JanetFiber *fiber, Janet reason) {
    if (NULL == channel || channel->is_threaded) return 0;
    JanetQueue *fq = &channel->read_pending;
    int32_t count = janet_q_count(fq);
    if (0 == count) return 0;
    JanetChannelPending *kept =
        janet_malloc(sizeof(JanetChannelPending) * (size_t) count);
    if (NULL == kept) return 0;
    int32_t nkept = 0;
    int cancelled = 0;
    int32_t i = fq->head;
    for (int32_t n = 0; n < count; n++) {
        JanetChannelPending p;
        memcpy(&p, (char *) fq->data + i * sizeof(JanetChannelPending),
               sizeof(p));
        i = i + 1 < fq->capacity ? i + 1 : 0;
        if (!cancelled && p.thread == &janet_vm && p.fiber == fiber) {
            /* Claim the node: unlinks the fiber's registration, frees
             * dead regs. Live entries schedule; stale ones drop. */
            if (janet_chanpend_claim_plain(&p)) {
                janet_schedule_signal(fiber, reason, JANET_SIGNAL_ERROR);
                cancelled = 1;
            }
            continue; /* consumed either way */
        }
        kept[nkept++] = p;
    }
    if (cancelled) {
        int32_t w = fq->head;
        for (int32_t k = 0; k < nkept; k++) {
            memcpy((char *) fq->data + w * sizeof(JanetChannelPending),
                   kept + k, sizeof(JanetChannelPending));
            w = w + 1 < fq->capacity ? w + 1 : 0;
        }
        fq->tail = w;
    }
    janet_free(kept);
    return cancelled;
}

/* Milestone 2c: current item depth of a channel (the :steal policy's
 * high-water check on lane control channels). */
JANET_API int32_t janet_channel_count(void *chan) {
    JanetChannel *channel = chan;
    return janet_q_count(&channel->items);
}

static int janet_chanat_gc(void *p, size_t s) {
    (void) s;
    JanetChannel *channel = p;
    janet_chan_deinit(channel);
    return 0;
}

static void janet_chanat_remove_vmref_one(JanetChannelPending *pending) {
    if (pending->thread == &janet_vm) {
        pending->thread = NULL;
        janet_gcunroot(janet_wrap_fiber(pending->fiber));
    }
}

static void janet_chanat_remove_vmref(JanetQueue *fq) {
    JanetChannelPending *pending = fq->data;
    if (fq->head <= fq->tail) {
        for (int32_t i = fq->head; i < fq->tail; i++) {
            janet_chanat_remove_vmref_one(pending + i);
        }
    } else {
        for (int32_t i = fq->head; i < fq->capacity; i++) {
            janet_chanat_remove_vmref_one(pending + i);
        }
        for (int32_t i = 0; i < fq->tail; i++) {
            janet_chanat_remove_vmref_one(pending + i);
        }
    }
}

static int janet_chanat_gcperthread(void *p, size_t s) {
    (void) s;
    JanetChannel *chan = p;
    janet_chan_lock(chan);
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
    janet_chanat_remove_vmref(&chan->read_pending);
    janet_chanat_remove_vmref(&chan->write_pending);
    janet_chan_unlock(chan);
    return 0;
}

static void janet_chanat_mark_fq(JanetQueue *fq) {
    JanetChannelPending *pending = fq->data;
    if (fq->head <= fq->tail) {
        for (int32_t i = fq->head; i < fq->tail; i++)
            if (!(pending[i].reg->dead))
                janet_mark(janet_wrap_fiber(pending[i].fiber));
    } else {
        for (int32_t i = fq->head; i < fq->capacity; i++)
            if (!(pending[i].reg->dead))
                janet_mark(janet_wrap_fiber(pending[i].fiber));
        for (int32_t i = 0; i < fq->tail; i++)
            if (!(pending[i].reg->dead))
                janet_mark(janet_wrap_fiber(pending[i].fiber));
    }
}

static int janet_chanat_mark(void *p, size_t s) {
    (void) s;
    JanetChannel *chan = p;
    janet_chanat_mark_fq(&chan->read_pending);
    janet_chanat_mark_fq(&chan->write_pending);
    JanetQueue *items = &chan->items;
    Janet *data = chan->items.data;
    if (items->head <= items->tail) {
        for (int32_t i = items->head; i < items->tail; i++)
            janet_mark(data[i]);
    } else {
        for (int32_t i = items->head; i < items->capacity; i++)
            janet_mark(data[i]);
        for (int32_t i = 0; i < items->tail; i++)
            janet_mark(data[i]);
    }
    return 0;
}

static Janet make_write_result(JanetChannel *channel) {
    Janet *tup = janet_tuple_begin(2);
    tup[0] = janet_ckeywordv("give");
    tup[1] = janet_wrap_channel(channel);
    return janet_wrap_tuple(janet_tuple_end(tup));
}

static Janet make_read_result(JanetChannel *channel, Janet x) {
    Janet *tup = janet_tuple_begin(3);
    tup[0] = janet_ckeywordv("take");
    tup[1] = janet_wrap_channel(channel);
    tup[2] = x;
    return janet_wrap_tuple(janet_tuple_end(tup));
}

static Janet make_close_result(JanetChannel *channel) {
    Janet *tup = janet_tuple_begin(2);
    tup[0] = janet_ckeywordv("close");
    tup[1] = janet_wrap_channel(channel);
    return janet_wrap_tuple(janet_tuple_end(tup));
}

/* Callback to use for scheduling a fiber from another thread. */
static void janet_thread_chan_cb(JanetEVGenericMessage msg) {
    uint32_t sched_id = (uint32_t) msg.argi;
    JanetFiber *fiber = msg.fiber;
    int mode = msg.tag;
    JanetChannel *channel = (JanetChannel *) msg.argp;
    Janet x = msg.argj;
    janet_chan_lock(channel);
    if (fiber->sched_id == sched_id) {
        if (mode == JANET_CP_MODE_CHOICE_READ) {
            janet_assert(!janet_chan_unpack(channel, &x, 0), "packing error");
            janet_schedule(fiber, make_read_result(channel, x));
        } else if (mode == JANET_CP_MODE_CHOICE_WRITE) {
            janet_schedule(fiber, make_write_result(channel));
        } else if (mode == JANET_CP_MODE_READ) {
            janet_assert(!janet_chan_unpack(channel, &x, 0), "packing error");
            janet_schedule(fiber, x);
        } else if (mode == JANET_CP_MODE_WRITE) {
            janet_schedule(fiber, janet_wrap_channel(channel));
        } else { /* (mode == JANET_CP_MODE_CLOSE) */
            janet_schedule(fiber, janet_wrap_nil());
        }
    } else if (mode != JANET_CP_MODE_CLOSE) {
        /* Fiber has already been canceled or resumed. */
        /* Resend event to another waiting thread, depending on mode */
        int is_read = (mode == JANET_CP_MODE_CHOICE_READ) || (mode == JANET_CP_MODE_READ);
        if (is_read) {
            JanetChannelPending reader;
            int sent = 0;
            while (!janet_q_pop(&channel->read_pending, &reader, sizeof(reader))) {
                janet_chanpend_claim_threaded(&reader);
                JanetVM *vm = reader.thread;
                if (!vm) continue;
                if (vm == &janet_vm) {
                    /* BUG 1 sibling: same-VM pending on a threaded
                     * channel. Posting through the self-pipe from the
                     * loop's own thread fills the pipe (~8000 writes
                     * at a 4000-value fan-in) and the loop then
                     * deadlocks inside its own wakeup write. Schedule
                     * inline instead -- we are the loop. */
                    uint32_t sid = (uint32_t) reader.sched_id;
                    if (reader.fiber->sched_id == sid) {
                        janet_gcunroot(janet_wrap_fiber(reader.fiber));
                        if (reader.mode == JANET_CP_MODE_CHOICE_READ) {
                            janet_assert(!janet_chan_unpack(channel, &x, 0),
                                    "packing error");
                            janet_schedule(reader.fiber,
                                    make_read_result(channel, x));
                        } else if (reader.mode == JANET_CP_MODE_READ) {
                            janet_assert(!janet_chan_unpack(channel, &x, 0),
                                    "packing error");
                            janet_schedule(reader.fiber, x);
                        } else {
                            janet_schedule(reader.fiber,
                                    make_write_result(channel));
                        }
                    }
                    sent = 1;
                    break;
                }
                JanetEVGenericMessage msg;
                msg.tag = reader.mode;
                msg.fiber = reader.fiber;
                msg.argi = (int32_t) reader.sched_id;
                msg.argp = channel;
                msg.argj = x;
                janet_ev_post_event(vm, janet_thread_chan_cb, msg);
                sent = 1;
                break;
            }
            if (!sent) {
                if (channel->closed || janet_q_push_head(&channel->items, &x, sizeof(Janet))) {
                    /* channel is closed or full. Otherwise, keep it. */
                    janet_chan_unpack(channel, &x, 1);
                }
            }
        } else {
            JanetChannelPending writer;
            while (!janet_q_pop(&channel->write_pending, &writer, sizeof(writer))) {
                janet_chanpend_claim_threaded(&writer);
                JanetVM *vm = writer.thread;
                if (!vm) continue;
                JanetEVGenericMessage msg;
                msg.tag = writer.mode;
                msg.fiber = writer.fiber;
                msg.argi = (int32_t) writer.sched_id;
                msg.argp = channel;
                msg.argj = janet_wrap_nil();
                janet_ev_post_event(vm, janet_thread_chan_cb, msg);
                break;
            }
        }
    }
    janet_chan_unlock(channel);
    janet_gcunroot(janet_wrap_fiber(fiber));
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
static void janet_chan_remove_stale(JanetQueue *fq) {
    int32_t count = janet_q_count(fq);
    if (count & (count - 1)) return;
    for (int32_t i = 0; i < count; i++) {
        JanetChannelPending pending;
        janet_q_pop(fq, &pending, sizeof(pending));
        if (pending.reg->dead) {
            /* Fiber gone: no root held (threaded roots keep fibers
             * alive), reclaim the node. */
            janet_free(pending.reg);
        } else if (pending.thread == &janet_vm && pending.sched_id != pending.fiber->sched_id) {
            janet_chanpend_claim_threaded(&pending);
            janet_gcunroot(janet_wrap_fiber(pending.fiber));
        } else {
            /* Live items must be inserted back in the same order */
            janet_q_push(fq, &pending, sizeof(pending));
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
extern void janet_abstract_home_teardown_cb(JanetEVGenericMessage msg);
static void janet_chan_inflight_release(JanetChannel *channel) {
    if (!janet_chan_is_threaded(channel)) return;
    if (0 == janet_abstract_decref((void *) channel)) {
        JanetAbstractHead *head = janet_abstract_head(channel);
        JanetEVGenericMessage msg;
        memset(&msg, 0, sizeof(msg));
        msg.argp = (void *) channel;
        janet_ev_post_event((JanetVM *) head->home_vm,
                janet_abstract_home_teardown_cb, msg);
    }
}
#define JANET_CHAN_INFLIGHT_ACQUIRE(chan) \
    do { if (janet_chan_is_threaded(chan)) \
        janet_abstract_incref((void *) (chan)); } while (0)
#define JANET_CHAN_INFLIGHT_DROP(chan) \
    do { janet_chan_inflight_release(chan); } while (0)

static int janet_channel_push_with_lock(JanetChannel *channel, Janet x, int mode) {
    JanetChannelPending reader;
    int is_empty;
    JANET_CHAN_INFLIGHT_ACQUIRE(channel);
    if (channel->closed) {
        janet_chan_unlock(channel);
        JANET_CHAN_INFLIGHT_DROP(channel);
        janet_panic("cannot write to closed channel");
    }
    /* P3.4: plain (non-packing) channels share by reference, so a
     * strict-heap value pushed here would leak invisibly to per-proc
     * majors. Refuse (packing channels marshal-copy, so they are
     * safe). Mode-2 supervisor pushes exempt: their events stay
     * reachable from the owning proc struct (marked + traversed —
     * no hole). Gated: legacy VMs skip for one branch; immediate and
     * immutable string-heap values exempt (no refs to leak). */
    if (2 != mode && janet_vm.heap_strict_live && !janet_chan_needs_pack(channel)) {
        JanetType xt = janet_type(x);
        if (xt != JANET_NIL && xt != JANET_BOOLEAN && xt != JANET_NUMBER &&
                xt != JANET_CFUNCTION && xt != JANET_POINTER &&
                xt != JANET_STRING && xt != JANET_SYMBOL && xt != JANET_KEYWORD) {
            void *xobj = NULL;
            switch (xt) {
                case JANET_ARRAY:
                    xobj = (void *) janet_unwrap_array(x);
                    break;
                case JANET_TUPLE:
                    xobj = (void *) janet_tuple_head(janet_unwrap_tuple(x));
                    break;
                case JANET_TABLE:
                    xobj = (void *) janet_unwrap_table(x);
                    break;
                case JANET_STRUCT:
                    xobj = (void *) janet_struct_head(janet_unwrap_struct(x));
                    break;
                case JANET_BUFFER:
                    xobj = (void *) janet_unwrap_buffer(x);
                    break;
                case JANET_FUNCTION:
                    xobj = (void *) janet_unwrap_function(x);
                    break;
                case JANET_ABSTRACT:
                    xobj = (void *) janet_abstract_head(janet_unwrap_abstract(x));
                    break;
                case JANET_FIBER:
                    xobj = (void *) janet_unwrap_fiber(x);
                    break;
                default:
                    break;
            }
            if (NULL != xobj &&
                    (janet_gc_header(xobj)->flags & JANET_MEM_ISOLATED)) {
                janet_chan_unlock(channel);
                janet_panic("strict proc value escaping through a "
                            "non-copying channel (use ev/ichan or "
                            "ev/thread-chan)");
            }
        }
    }
    if (janet_chan_pack(channel, &x)) {
        janet_chan_unlock(channel);
        janet_panicf("failed to pack value for channel: %v", x);
    }
    int is_threaded = janet_chan_is_threaded(channel);
    if (is_threaded) {
        /* don't dereference fiber from another thread */
        is_empty = janet_q_pop(&channel->read_pending, &reader, sizeof(reader));
        if (!is_empty) janet_chanpend_claim_threaded(&reader);
    } else {
        for (;;) {
            is_empty = janet_q_pop(&channel->read_pending, &reader, sizeof(reader));
            if (is_empty) break;
                    /* Registration makes this sound without GC probing: dead
             * fibers are reclaimed by node and never dereferenced;
             * stale entries (fiber rescheduled some other way) drop
             * here exactly as in the pre-P3.5c discipline. */
            if (janet_chanpend_claim_plain(&reader)) break;
        }
    }
    if (is_empty) {
        /* No pending reader */
        if (mode == 3 && janet_q_count(&channel->items) >= channel->limit) {
            /* Try-mode: report full without buffering (mode 2 keeps the
             * historical buffer-beyond-limit behavior for supervisor
             * pushes, which ignore the return value). Must unpack the
             * packed copy to avoid leaking the threaded marshal buffer. */
            janet_chan_unpack(channel, &x, 1);
            janet_chan_unlock(channel);
            JANET_CHAN_INFLIGHT_DROP(channel);
            return 1;
        }
        if (janet_q_push(&channel->items, &x, sizeof(Janet))) {
            janet_chan_unpack(channel, &x, 1);
            janet_chan_unlock(channel);
            janet_panicf("channel overflow: %v", x);
        } else if (janet_q_count(&channel->items) > channel->limit) {
            /* No root fiber, we are in completion on a root fiber. Don't block. */
            if (mode == 2) {
                janet_chan_unlock(channel);
                JANET_CHAN_INFLIGHT_DROP(channel);
                return 1;
            }
            /* Pushed successfully, but should block. */
            JanetChannelPending pending;
            pending.thread = &janet_vm;
            pending.fiber = janet_vm.root_fiber,
            pending.sched_id = janet_vm.root_fiber->sched_id,
            pending.mode = mode ? JANET_CP_MODE_CHOICE_WRITE : JANET_CP_MODE_WRITE;
            if (is_threaded) {
                janet_chan_remove_stale(&channel->write_pending);
            }
            pending.reg = janet_chanpend_register(pending.fiber, channel, 1);
            janet_q_push(&channel->write_pending, &pending, sizeof(pending));
            janet_chan_unlock(channel);
            if (is_threaded) {
                janet_gcroot(janet_wrap_fiber(pending.fiber));
            }
            JANET_CHAN_INFLIGHT_DROP(channel);
            return 1;
        }
    } else {
        /* Pending reader */
        if (is_threaded) {
            JanetVM *vm = reader.thread;
            if (vm == &janet_vm) {
                /* BUG 1 sibling: same-VM reader pending on a threaded
                 * channel (the density/recv park shape). Schedule
                 * inline -- a self-pipe write from the loop's own
                 * thread floods the pipe at fan-in scale and the loop
                 * deadlocks inside its own wakeup write. */
                uint32_t sid = (uint32_t) reader.sched_id;
                if (reader.fiber->sched_id == sid) {
                    janet_gcunroot(janet_wrap_fiber(reader.fiber));
                    if (reader.mode == JANET_CP_MODE_CHOICE_READ) {
                        janet_assert(!janet_chan_unpack(channel, &x, 0),
                                "bad channel packing");
                        janet_schedule(reader.fiber,
                                make_read_result(channel, x));
                    } else {
                        janet_assert(!janet_chan_unpack(channel, &x, 0),
                                "bad channel packing");
                        janet_schedule(reader.fiber, x);
                    }
                } else {
                    /* Stale reader (resumed via another clause, whose
                     * path already unrooted): keep the value for the
                     * next reader -- push-head, then release the
                     * packed buffer, like the pipe callback's stale
                     * path. */
                    if (channel->closed ||
                            janet_q_push_head(&channel->items, &x,
                                sizeof(Janet))) {
                        janet_chan_unpack(channel, &x, 1);
                    }
                }
            } else {
                JanetEVGenericMessage msg;
                msg.tag = reader.mode;
                msg.fiber = reader.fiber;
                msg.argi = (int32_t) reader.sched_id;
                msg.argp = channel;
                msg.argj = x;
                if (vm) {
                    janet_ev_post_event(vm, janet_thread_chan_cb, msg);
                } else {
                    /* If no vm to send to, we must clean up (unpack) the packed payload to avoid leak */
                    janet_chan_unpack(channel, &x, 1);
                }
            }
        } else {
            /* Same-VM handoff: decode transit values first (no-op for
             * plain channels, which never pack; the threaded path
             * decodes in its completion callback instead). */
            janet_assert(!janet_chan_unpack(channel, &x, 0), "bad channel packing");
            if (reader.mode == JANET_CP_MODE_CHOICE_READ) {
                janet_schedule(reader.fiber, make_read_result(channel, x));
            } else {
                janet_schedule(reader.fiber, x);
            }
        }
    }
    janet_chan_unlock(channel);
    JANET_CHAN_INFLIGHT_DROP(channel);
    return 0;
}

int janet_channel_push(JanetChannel *channel, Janet x, int mode) {
    /* Step 4.1 home-lane pin: isolated channels are lock-free, so a
     * foreign thread would race silently — refuse loudly instead
     * (use ev/thread-chan across threads). One predictable branch on
     * the non-isolated fast path. */
    if (channel->isolated && channel->owner != &janet_vm) {
        janet_panic("isolated channel used from a foreign thread "
                    "(pinned to its home lane; use ev/thread-chan "
                    "across threads)");
    }
    janet_chan_lock(channel);
    return janet_channel_push_with_lock(channel, x, mode);
}

/* Pop from a channel - returns 1 if item was obtained, 0 otherwise. The item
 * is returned by reference. If the pop would block, will add to the read_pending
 * queue in the channel. */
static int janet_channel_pop_with_lock(JanetChannel *channel, Janet *item, int is_choice) {
    JanetChannelPending writer;
    JANET_CHAN_INFLIGHT_ACQUIRE(channel);
    if (channel->closed) {
        janet_chan_unlock(channel);
        JANET_CHAN_INFLIGHT_DROP(channel);
        *item = janet_wrap_nil();
        return 1;
    }
    int is_threaded = janet_chan_is_threaded(channel);
    if (janet_q_pop(&channel->items, item, sizeof(Janet))) {
        /* Queue empty */
        if (is_choice == 2) {
            /* Try-mode: report empty without suspending (never leaves
             * the lock held — a leaked mutex would deadlock every
             * later operation on this channel). */
            janet_chan_unlock(channel);
            return 0; /* Skip pending read */
        }
        JanetChannelPending pending;
        pending.thread = &janet_vm;
        pending.fiber = janet_vm.root_fiber,
        pending.sched_id = janet_vm.root_fiber->sched_id;
        pending.mode = is_choice ? JANET_CP_MODE_CHOICE_READ : JANET_CP_MODE_READ;
        if (is_threaded) {
            janet_chan_remove_stale(&channel->read_pending);
        }
        pending.reg = janet_chanpend_register(pending.fiber, channel, 0);
        janet_q_push(&channel->read_pending, &pending, sizeof(pending));
        janet_chan_unlock(channel);
        if (is_threaded) {
            janet_gcroot(janet_wrap_fiber(pending.fiber));
        }
        JANET_CHAN_INFLIGHT_DROP(channel);
        return 0;
    }
    janet_assert(!janet_chan_unpack(channel, item, 0), "bad channel packing");
    if (!janet_q_pop(&channel->write_pending, &writer, sizeof(writer))) {
        /* Pending writer. Same staleness discipline as the read side:
         * dead fibers are reclaimed by node (never dereferenced);
         * same-VM stale entries drop; threaded entries address foreign
         * VMs and are root-pinned while queued. */
        if (!is_threaded && janet_chanpend_claim_plain(&writer)) {
            if (writer.mode == JANET_CP_MODE_CHOICE_WRITE) {
                janet_schedule(writer.fiber, make_write_result(channel));
            } else {
                janet_schedule(writer.fiber, janet_wrap_abstract(channel));
            }
        } else if (is_threaded) {
            janet_chanpend_claim_threaded(&writer);
            JanetVM *vm = writer.thread;
            if (vm == &janet_vm) {
                /* BUG 1 sibling: same-VM writer pending on a threaded
                 * channel -- schedule inline (see the push-side note). */
                uint32_t sid = (uint32_t) writer.sched_id;
                if (writer.fiber->sched_id == sid &&
                        janet_fiber_can_resume(writer.fiber)) {
                    janet_gcunroot(janet_wrap_fiber(writer.fiber));
                    if (writer.mode == JANET_CP_MODE_CHOICE_WRITE) {
                        janet_schedule(writer.fiber,
                                make_write_result(channel));
                    } else {
                        janet_schedule(writer.fiber,
                                janet_wrap_channel(channel));
                    }
                }
            } else {
                JanetEVGenericMessage msg;
                msg.tag = writer.mode;
                msg.fiber = writer.fiber;
                msg.argi = (int32_t) writer.sched_id;
                msg.argp = channel;
                msg.argj = janet_wrap_nil();
                if (vm) {
                    janet_ev_post_event(vm, janet_thread_chan_cb, msg);
                }
            }
        }
    }
    janet_chan_unlock(channel);
    JANET_CHAN_INFLIGHT_DROP(channel);
    return 1;
}

int janet_channel_pop(JanetChannel *channel, Janet *item, int is_choice) {
    /* Step 4.1 home-lane pin (see push). */
    if (channel->isolated && channel->owner != &janet_vm) {
        janet_panic("isolated channel used from a foreign thread "
                    "(pinned to its home lane; use ev/thread-chan "
                    "across threads)");
    }
    janet_chan_lock(channel);
    return janet_channel_pop_with_lock(channel, item, is_choice);
}

JanetChannel *janet_channel_unwrap(void *abstract) {
    return abstract;
}

JanetChannel *janet_getchannel(const Janet *argv, int32_t n) {
    return janet_channel_unwrap(janet_getabstract(argv, n, &janet_channel_type));
}

JanetChannel *janet_optchannel(const Janet *argv, int32_t argc, int32_t n, JanetChannel *dflt) {
    if (argc > n && !janet_checktype(argv[n], JANET_NIL)) {
        return janet_getchannel(argv, n);
    } else {
        return dflt;
    }
}

int janet_channel_give(JanetChannel *channel, Janet x) {
    return janet_channel_push(channel, x, 2);
}

int janet_channel_take(JanetChannel *channel, Janet *out) {
    return janet_channel_pop(channel, out, 2);
}

JanetChannel *janet_channel_make(uint32_t limit) {
    janet_assert(limit <= INT32_MAX, "bad limit");
    JanetChannel *channel = janet_abstract(&janet_channel_type, sizeof(JanetChannel));
    janet_chan_init(channel, (int32_t) limit, 0, 0);
    return channel;
}

JanetChannel *janet_channel_make_threaded(uint32_t limit) {
    janet_assert(limit <= INT32_MAX, "bad limit");
    JanetChannel *channel = janet_abstract_threaded(&janet_channel_type, sizeof(JanetChannel));
    janet_chan_init(channel, (int32_t) limit, 1, 0);
    return channel;
}

JanetChannel *janet_channel_make_isolated(uint32_t limit) {
    janet_assert(limit <= INT32_MAX, "bad limit");
    JanetChannel *channel = janet_abstract(&janet_channel_type, sizeof(JanetChannel));
    janet_chan_init(channel, (int32_t) limit, 0, 1);
    return channel;
}

/* Channel Methods */

JANET_CORE_FN(cfun_channel_push,
              "(ev/give chan val)",
              "Write `val` to a channel `chan`, suspending the current "
              "fiber if `chan` is full. Returns `chan` on success, nil "
              "otherwise.") {
    janet_fixarity(argc, 2);
    JanetChannel *channel = janet_getchannel(argv, 0);
    if (janet_vm.coerce_error) {
        janet_panic("cannot give to channel inside janet_call");
    }
    if (janet_channel_push(channel, argv[1], 0)) {
        janet_await();
    }
    return argv[0];
}

JANET_CORE_FN(cfun_channel_pop,
              "(ev/take chan)",
              "Read from a channel `chan`, suspending the current fiber if "
              "no value is available.") {
    janet_fixarity(argc, 1);
    JanetChannel *channel = janet_getchannel(argv, 0);
    Janet item;
    if (janet_vm.coerce_error) {
        janet_panic("cannot take from channel inside janet_call");
    }
    if (janet_channel_pop(channel, &item, 0)) {
        janet_schedule(janet_vm.root_fiber, item);
    }
    janet_await();
}

JANET_CORE_FN(cfun_channel_try_push,
              "(ev/try-give chan val)",
              "Non-blocking channel write. Returns `chan` if the value was "
              "accepted, nil if the channel is full (no suspension). "
              "Panics on a closed channel, like `ev/give`. Safe inside "
              "`janet_call` (never suspends).") {
    janet_fixarity(argc, 2);
    JanetChannel *channel = janet_getchannel(argv, 0);
    if (janet_channel_push(channel, argv[1], 3)) {
        return janet_wrap_nil();
    }
    return argv[0];
}

JANET_CORE_FN(cfun_channel_try_pop,
              "(ev/try-take chan)",
              "Non-blocking channel read. Returns the value if one is "
              "immediately available, nil if the channel is empty (no "
              "suspension). A closed channel yields nil, like `ev/take`. "
              "Safe inside `janet_call` (never suspends).") {
    janet_fixarity(argc, 1);
    JanetChannel *channel = janet_getchannel(argv, 0);
    Janet item = janet_wrap_nil();
    if (janet_channel_pop(channel, &item, 2)) {
        return item;
    }
    return janet_wrap_nil();
}

JANET_CORE_FN(cfun_channel_choice,
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
    janet_arity(argc, 1, -1);
    int32_t len;
    const Janet *data;

    if (janet_vm.coerce_error) {
        janet_panic("cannot select from channel inside janet_call");
    }

    /* Check channels for immediate reads and writes. Avoid awating to event loop
     * if possible, but should be very similar to async block of code. */
    for (int32_t i = 0; i < argc; i++) {
        if (janet_indexed_view(argv[i], &data, &len) && len == 2) {
            /* Write */
            JanetChannel *chan = janet_getchannel(data, 0);
            janet_chan_lock(chan);
            if (chan->closed) {
                janet_chan_unlock(chan);
                return make_close_result(chan);
            }
            if (janet_q_count(&chan->items) < chan->limit) {
                janet_channel_push_with_lock(chan, data[1], 1);
                return make_write_result(chan);
            }
            janet_chan_unlock(chan);
        } else {
            /* Read */
            JanetChannel *chan = janet_getchannel(argv, i);
            janet_chan_lock(chan);
            if (chan->closed) {
                janet_chan_unlock(chan);
                return make_close_result(chan);
            }
            if (chan->items.head != chan->items.tail) {
                Janet item;
                janet_channel_pop_with_lock(chan, &item, 1);
                return make_read_result(chan, item);
            }
            janet_chan_unlock(chan);
        }
    }

    /* We cannot assume threaded channels are unchanged here, as other threads can mutate them.
     * Namely, they may been closed or have items added or removed during this call. Simple channels
     * could be slightly simpler and skip the extra closed checks. */

    /* Wait for all readers or writers */
    for (int32_t i = 0; i < argc; i++) {
        if (janet_indexed_view(argv[i], &data, &len) && len == 2) {
            /* Write */
            JanetChannel *chan = janet_getchannel(data, 0);
            janet_chan_lock(chan);
            if (chan->closed) {
                janet_chan_unlock(chan);
                janet_schedule(janet_vm.root_fiber, make_close_result(chan));
                break;
            }
            if (!janet_channel_push_with_lock(chan, data[1], 1)) {
                janet_schedule(janet_vm.root_fiber, make_write_result(chan));
                break;
            }
        } else {
            /* Read */
            Janet item;
            JanetChannel *chan = janet_getchannel(argv, i);
            janet_chan_lock(chan);
            if (chan->closed) {
                janet_chan_unlock(chan);
                janet_schedule(janet_vm.root_fiber, make_close_result(chan));
                break;
            }
            if (janet_channel_pop_with_lock(chan, &item, 1)) {
                janet_schedule(janet_vm.root_fiber, make_read_result(chan, item));
                break;
            }
        }
    }

    janet_await();
}

JANET_CORE_FN(cfun_channel_full,
              "(ev/full chan)",
              "Returns true if a channel `chan` is full, false otherwise.") {
    janet_fixarity(argc, 1);
    JanetChannel *channel = janet_getchannel(argv, 0);
    janet_chan_lock(channel);
    Janet ret = janet_wrap_boolean(janet_q_count(&channel->items) >= channel->limit);
    janet_chan_unlock(channel);
    return ret;
}

JANET_CORE_FN(cfun_channel_capacity,
              "(ev/capacity chan)",
              "Get number of items a channel `chan` can store before "
              "blocking writers.") {
    janet_fixarity(argc, 1);
    JanetChannel *channel = janet_getchannel(argv, 0);
    janet_chan_lock(channel);
    Janet ret = janet_wrap_integer(channel->limit);
    janet_chan_unlock(channel);
    return ret;
}

JANET_CORE_FN(cfun_channel_count,
              "(ev/count chan)",
              "Get number of items waiting in a channel `chan`.") {
    janet_fixarity(argc, 1);
    JanetChannel *channel = janet_getchannel(argv, 0);
    janet_chan_lock(channel);
    Janet ret = janet_wrap_integer(janet_q_count(&channel->items));
    janet_chan_unlock(channel);
    return ret;
}

/* Fisher yates shuffle of arguments to get fairness */
static void fisher_yates_args(int32_t argc, Janet *argv) {
    for (int32_t i = argc; i > 1; i--) {
        int32_t swap_index = janet_rng_u32(&janet_vm.ev_rng) % i;
        Janet temp = argv[swap_index];
        argv[swap_index] = argv[i - 1];
        argv[i - 1] = temp;
    }
}

JANET_CORE_FN(cfun_channel_rchoice,
              "(ev/rselect & clauses)",
              "Similar to ev/select, but will try clauses in a random order for fairness.") {
    fisher_yates_args(argc, argv);
    return cfun_channel_choice(argc, argv);
}

JANET_CORE_FN(cfun_channel_new,
              "(ev/chan &opt capacity)",
              "Create a new channel. capacity is the number of values to queue before "
              "blocking writers, defaults to 0 if not provided. Returns a new channel.") {
    janet_arity(argc, 0, 1);
    int32_t limit = janet_optnat(argv, argc, 0, 0);
    JanetChannel *channel = janet_abstract(&janet_channel_type, sizeof(JanetChannel));
    janet_chan_init(channel, limit, 0, 0);
    return janet_wrap_abstract(channel);
}

JanetChannel *janet_channel_make_threaded_c(int32_t limit) {
    JanetChannel *tchan = janet_abstract_threaded(&janet_channel_type,
            sizeof(JanetChannel));
    janet_chan_init(tchan, limit, 1, 0);
    return tchan;
}

JANET_CORE_FN(cfun_channel_new_threaded,
              "(ev/thread-chan &opt limit)",
              "Create a threaded channel. A threaded channel is a channel that can be shared between threads and "
              "used to communicate between any number of operating system threads.") {
    janet_arity(argc, 0, 1);
    int32_t limit = janet_optnat(argv, argc, 0, 0);
    JanetChannel *tchan = janet_abstract_threaded(&janet_channel_type, sizeof(JanetChannel));
    janet_chan_init(tchan, limit, 1, 0);
    return janet_wrap_abstract(tchan);
}

JANET_CORE_FN(cfun_channel_new_isolated,
              "(ev/ichan &opt limit)",
              "Create an isolated channel. Values sent through it are "
              "marshal-copied like a threaded channel (no shared mutable "
              "references cross), but it takes no lock: single-VM use "
              "only, the tasklet mailbox transport. Cross-thread use is "
              "user error (use ev/thread-chan across threads).") {
    janet_arity(argc, 0, 1);
    int32_t limit = janet_optnat(argv, argc, 0, 0);
    JanetChannel *ichan = janet_abstract(&janet_channel_type, sizeof(JanetChannel));
    janet_chan_init(ichan, limit, 0, 1);
    return janet_wrap_abstract(ichan);
}

JANET_CORE_FN(cfun_channel_close,
              "(ev/chan-close chan)",
              "Close a channel `chan`. Once closed, causes all pending "
              "reads and writes to return nil. Returns `chan`.") {
    janet_fixarity(argc, 1);
    JanetChannel *channel = janet_getchannel(argv, 0);
    janet_chan_lock(channel);
    if (!channel->closed) {
        channel->closed = 1;
        JanetChannelPending writer;
        while (!janet_q_pop(&channel->write_pending, &writer, sizeof(writer))) {
            if (writer.thread != &janet_vm) {
                janet_chanpend_claim_threaded(&writer);
                JanetVM *vm = writer.thread;
                JanetEVGenericMessage msg;
                msg.fiber = writer.fiber;
                msg.argp = channel;
                msg.tag = JANET_CP_MODE_CLOSE;
                msg.argi = (int32_t) writer.sched_id;
                msg.argj = janet_wrap_nil();
                if (vm) {
                    janet_ev_post_event(vm, janet_thread_chan_cb, msg);
                }
            } else if (janet_chan_is_threaded(channel)) {
                /* Same-VM entry riding a threaded channel: root was
                 * held by this queue entry, release it now. */
                janet_chanpend_claim_threaded(&writer);
                if (janet_fiber_can_resume(writer.fiber) && writer.sched_id == writer.fiber->sched_id) {
                    if (writer.mode == JANET_CP_MODE_CHOICE_WRITE) {
                        janet_schedule(writer.fiber, make_close_result(channel));
                    } else {
                        janet_schedule(writer.fiber, janet_wrap_nil());
                    }
                }
                janet_gcunroot(janet_wrap_fiber(writer.fiber));
            } else {
                /* Plain/isolated: same-VM, unrooted entries. */
                int live = janet_chanpend_claim_plain(&writer);
                if (live && janet_fiber_can_resume(writer.fiber) && writer.sched_id == writer.fiber->sched_id) {
                    if (writer.mode == JANET_CP_MODE_CHOICE_WRITE) {
                        janet_schedule(writer.fiber, make_close_result(channel));
                    } else {
                        janet_schedule(writer.fiber, janet_wrap_nil());
                    }
                }
            }
        }
        JanetChannelPending reader;
        while (!janet_q_pop(&channel->read_pending, &reader, sizeof(reader))) {
            if (reader.thread != &janet_vm) {
                janet_chanpend_claim_threaded(&reader);
                JanetVM *vm = reader.thread;
                JanetEVGenericMessage msg;
                msg.fiber = reader.fiber;
                msg.argp = channel;
                msg.tag = JANET_CP_MODE_CLOSE;
                msg.argi = (int32_t) reader.sched_id;
                msg.argj = janet_wrap_nil();
                if (vm) {
                    janet_ev_post_event(vm, janet_thread_chan_cb, msg);
                }
            } else if (janet_chan_is_threaded(channel)) {
                /* Same-VM entry riding a threaded channel: root was
                 * held by this queue entry, release it now. */
                janet_chanpend_claim_threaded(&reader);
                if (janet_fiber_can_resume(reader.fiber) && reader.sched_id == reader.fiber->sched_id) {
                    if (reader.mode == JANET_CP_MODE_CHOICE_READ) {
                        janet_schedule(reader.fiber, make_close_result(channel));
                    } else {
                        janet_schedule(reader.fiber, janet_wrap_nil());
                    }
                }
                janet_gcunroot(janet_wrap_fiber(reader.fiber));
            } else {
                /* Plain/isolated: same-VM, unrooted entries. */
                int live = janet_chanpend_claim_plain(&reader);
                if (live && janet_fiber_can_resume(reader.fiber) && reader.sched_id == reader.fiber->sched_id) {
                    if (reader.mode == JANET_CP_MODE_CHOICE_READ) {
                        janet_schedule(reader.fiber, make_close_result(channel));
                    } else {
                        janet_schedule(reader.fiber, janet_wrap_nil());
                    }
                }
            }
        }
    }
    janet_chan_unlock(channel);
    return argv[0];
}

static const JanetMethod ev_chanat_methods[] = {
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

static int janet_chanat_get(void *p, Janet key, Janet *out) {
    (void) p;
    if (!janet_checktype(key, JANET_KEYWORD)) return 0;
    return janet_getmethod(janet_unwrap_keyword(key), ev_chanat_methods, out);
}

static Janet janet_chanat_next(void *p, Janet key) {
    (void) p;
    return janet_nextmethod(ev_chanat_methods, key);
}

static void janet_chanat_marshal(void *p, JanetMarshalContext *ctx) {
    JanetChannel *channel = (JanetChannel *)p;
    /* Step 1.4: flags byte (bit0 threaded, bit1 isolated). Old dumps
     * wrote 0/1, which read back identically (bit1 clear). */
    uint8_t flags = (uint8_t)
        ((channel->is_threaded ? 1 : 0) | (channel->isolated ? 2 : 0));
    janet_marshal_byte(ctx, flags);
    janet_marshal_abstract(ctx, channel);
    janet_marshal_byte(ctx, channel->closed);
    janet_marshal_int(ctx, channel->limit);
    int32_t count = janet_q_count(&channel->items);
    janet_marshal_int(ctx, count);
    JanetQueue *items = &channel->items;
    Janet *data = channel->items.data;
    if (items->head <= items->tail) {
        for (int32_t i = items->head; i < items->tail; i++)
            janet_marshal_janet(ctx, data[i]);
    } else {
        for (int32_t i = items->head; i < items->capacity; i++)
            janet_marshal_janet(ctx, data[i]);
        for (int32_t i = 0; i < items->tail; i++)
            janet_marshal_janet(ctx, data[i]);
    }
}

static void *janet_chanat_unmarshal(JanetMarshalContext *ctx) {
    uint8_t flags = janet_unmarshal_byte(ctx);
    /* Old dumps carry 0/1 (threaded bit only); bit1 clear preserves them. */
    int is_threaded = (flags & 1) != 0;
    int isolated = (flags & 2) != 0;
    JanetChannel *abst;
    if (is_threaded) {
        abst = janet_unmarshal_abstract_threaded(ctx, sizeof(JanetChannel));
    } else {
        abst = janet_unmarshal_abstract(ctx, sizeof(JanetChannel));
    }
    uint8_t is_closed = janet_unmarshal_byte(ctx);
    int32_t limit = janet_unmarshal_int(ctx);
    int32_t count = janet_unmarshal_int(ctx);
    if (count < 0) janet_panic("invalid negative channel count");
    if (count > limit) janet_panic("invalid channel count");
    janet_chan_init(abst, limit, is_threaded, isolated);
    abst->closed = !!is_closed;
    int needs_pack = (is_threaded || isolated);
    for (int32_t i = 0; i < count; i++) {
        Janet item = janet_unmarshal_janet(ctx);
        janet_assert(!janet_q_push(&abst->items, &item, sizeof(item)), "bad unmarshal channel");
        /* Packed queues hold malloc'd transit buffers (see pack), but
         * unmarshal rebuilds them as GC buffers — which unpack would
         * janet_free (invalid free). Move the bytes into malloc'd
         * ownership (encoding untouched); the GC copy becomes garbage.
         * Pack is bypassed deliberately: items are already transit. */
        if (needs_pack && janet_checktype(item, JANET_BUFFER)) {
            JanetBuffer *src = janet_unwrap_buffer(item);
            JanetBuffer *nb = janet_malloc(sizeof(JanetBuffer));
            if (NULL == nb) JANET_OUT_OF_MEMORY;
            janet_buffer_init(nb, 0);
            if (src->count > 0) {
                janet_buffer_push_bytes(nb, src->data, src->count);
            }
            JanetQueue *q = &abst->items;
            Janet *slots = q->data;
            int32_t tail = q->tail - 1;
            if (tail < 0) tail += q->capacity;
            slots[tail] = janet_wrap_buffer(nb);
        }
    }
    return abst;
}

const JanetAbstractType janet_channel_type = {
    "core/channel",
    janet_chanat_gc,
    janet_chanat_mark,
    janet_chanat_get,
    NULL, /* put */
    janet_chanat_marshal,
    janet_chanat_unmarshal,
    NULL, /* tostring */
    NULL, /* compare */
    NULL, /* hash */
    janet_chanat_next,
    NULL, /* call */
    NULL, /* length */
    NULL, /* bytes */
    janet_chanat_gcperthread
};

/* Main event loop */

void janet_loop1_impl(int has_timeout, JanetTimestamp timeout);

int janet_loop_done(void) {
    return !((janet_vm.spawn.head != janet_vm.spawn.tail) ||
             janet_vm.tq_count ||
             janet_atomic_load(&janet_vm.listener_count));
}

JanetFiber *janet_loop1(void) {
    /* Schedule expired timers */
    JanetTimeout to;
    JanetTimestamp now = ts_now();
    while (peek_timeout(&to) && to.when <= now) {
        pop_timeout(0);
        if (to.curr_fiber != NULL) {
            if (janet_fiber_can_resume(to.curr_fiber)) {
                janet_cancel(to.fiber, janet_cstringv("deadline expired"));
            }
        } else {
            /* This is a timeout (for a function call, not a whole fiber) */
            if (to.fiber->sched_id == to.sched_id) {
                if (to.is_error) {
                    janet_cancel(to.fiber, janet_cstringv("timeout"));
                } else {
                    janet_schedule(to.fiber, janet_wrap_nil());
                }
            }
        }
        handle_timeout_worker(to, 0);
    }

    /* Run scheduled fibers unless interrupts need to be handled. */
    while (janet_vm.spawn.head != janet_vm.spawn.tail) {
        /* Don't run until all interrupts have been marked as handled by calling janet_interpreter_interrupt_handled */
        if (janet_atomic_load_relaxed(&janet_vm.auto_suspend)) break;
        JanetTask task = {NULL, janet_wrap_nil(), JANET_SIGNAL_OK, 0};
        janet_q_pop(&janet_vm.spawn, &task, sizeof(task));
        if (task.fiber->gc.flags & JANET_FIBER_EV_GCFLAG_SUSPENDED) janet_ev_dec_refcount();
        task.fiber->gc.flags &= ~(JANET_FIBER_EV_GCFLAG_CANCELED | JANET_FIBER_EV_GCFLAG_SUSPENDED);
        if (task.expected_sched_id != task.fiber->sched_id) continue;
        Janet res;
        /* M4b-2 quantum driver: budget is granted per resume inside
         * janet_continue_no_check (covers loop-scheduled and direct
         * runs alike), so every run below starts full. Off = existing
         * behavior exact. */
        JanetSignal sig = janet_continue_signal(task.fiber, task.value, &res, task.sig);
        /* M4b quantum yield: the fiber WILL be re-queued below (same
         * signal, budget exhausted). It must bypass the completion
         * cleanup -- the minor frees the fiber's young nursery, and a
         * re-queued fiber would resume onto freed data (the density
         * UAF: status USER9, data poisoned, walked by later marks). */
        int quantum_yield = (sig == JANET_SIGNAL_INTERRUPT) &&
            janet_vm.quantum && !janet_atomic_load_relaxed(&janet_vm.auto_suspend);
        if (!janet_fiber_can_resume(task.fiber) && !quantum_yield) {
            janet_table_remove(&janet_vm.active_tasks, janet_wrap_fiber(task.fiber));
            /* P3.5c: a completed proc retires its mailbox (go quiet so
             * later sends drop silently, Erlang parity). Completed
             * fibers hold no self-pendings (recv is own-mailbox only),
             * so no purge is needed — only the flag. All objects are
             * listed-valid here (mid-mutator, pre-sweep). */
            if (NULL != task.fiber->proc && NULL != task.fiber->proc->mbox) {
                janet_channel_set_proc_alive(task.fiber->proc->mbox, 0);
            }
            /* Phase 1 density: one final own-heap minor at completion.
             * The global minor no longer walks fibers' lists (the 1M
             * quadratic), so a dead fiber's young garbage frees HERE —
             * O(own), at the completion boundary where the stack is
             * final and consistent. Parked-forever fibers are the only
             * deferred case (bounded by their real working set). */
            janet_collect_fiber_minor(task.fiber);
        }
        void *sv = task.fiber->supervisor_channel;
        int is_suspended = sig == JANET_SIGNAL_EVENT || sig == JANET_SIGNAL_YIELD || sig == JANET_SIGNAL_INTERRUPT;
        if (is_suspended) {
            task.fiber->gc.flags |= JANET_FIBER_EV_GCFLAG_SUSPENDED;
            janet_ev_inc_refcount();
        }
        if (sig == JANET_SIGNAL_INTERRUPT && janet_vm.quantum &&
                !janet_atomic_load_relaxed(&janet_vm.auto_suspend)) {
            /* Budget exhausted (not external quiesce — that leaves
             * auto_suspend set): re-queue at the back with the original
             * wake value/signal (transparent deferral — takes/sleeps
             * already materialized before any poll could fire) and BREAK
             * to event polling (not continue: a re-queued hog would
             * otherwise starve timers/channel-polling forever — the
             * spawn queue never drains). No supervisor event: nothing
             * completed. Resume value ignored via RESUME_NO_USEVAL. */
            janet_schedule_signal(task.fiber, task.value, task.sig);
            break;
        }
        if (NULL == sv) {
            if (!is_suspended) {
                janet_stacktrace_ext(task.fiber, res, "");
            }
        } else if (sig == JANET_SIGNAL_OK || (task.fiber->flags & (1 << sig))) {
            JanetChannel *chan = janet_channel_unwrap(sv);
            janet_channel_push(chan, make_supervisor_event(janet_signal_names[sig],
                    task.fiber, chan->is_threaded), 2);
        } else if (!is_suspended) {
            janet_stacktrace_ext(task.fiber, res, "");
        }
        if (sig == JANET_SIGNAL_INTERRUPT) {
            return task.fiber;
        }
    }

    /* Poll for events */
    if (janet_vm.tq_count || janet_atomic_load(&janet_vm.listener_count)) {
        JanetTimeout to;
        memset(&to, 0, sizeof(to));
        int has_timeout;
        /* Drop timeouts that are no longer needed */
        while ((has_timeout = peek_timeout(&to))) {
            if (to.curr_fiber != NULL) {
                if (!janet_fiber_can_resume(to.curr_fiber)) {
                    pop_timeout(0);
                    janet_table_remove(&janet_vm.active_tasks, janet_wrap_fiber(to.curr_fiber));
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
        if ((janet_vm.tq_count || janet_atomic_load(&janet_vm.listener_count)) &&
                !(janet_vm.quantum &&
                  !janet_atomic_load_relaxed(&janet_vm.auto_suspend) &&
                  janet_vm.spawn.head != janet_vm.spawn.tail)) {
            janet_loop1_impl(has_timeout, to.when);
        }
    }

    /* No fiber was interrupted */
    return NULL;
}

/* Same as janet_interpreter_interrupt, but will also
 * break out of the event loop if waiting for an event
 * (say, waiting for ev/sleep to finish). Does this by pushing
 * an empty event to the event loop. */
void janet_loop1_interrupt(JanetVM *vm) {
    janet_interpreter_interrupt(vm);
    JanetEVGenericMessage msg = {0};
    JanetCallback cb = NULL;
    janet_ev_post_event(vm, cb, msg);
}

void janet_loop(void) {
    while (!janet_loop_done()) {
        JanetFiber *interrupted_fiber = janet_loop1();
        if (NULL != interrupted_fiber) {
            janet_schedule(interrupted_fiber, janet_wrap_nil());
        }
    }
}

/*
 * Self-pipe handling code.
 */

#ifdef JANET_WINDOWS

/* On windows, use PostQueuedCompletionStatus instead for
 * custom events */

#else

static void janet_ev_setup_selfpipe(void) {
    if (janet_make_pipe(janet_vm.selfpipe, 1)) {
        JANET_EXIT("failed to initialize self pipe in event loop");
    }
}

/* Handle events from the self pipe inside the event loop */
static void janet_ev_handle_selfpipe(void) {
    JanetSelfPipeEvent response;
    int status;
recur:
    do {
        status = read(janet_vm.selfpipe[0], &response, sizeof(response));
    } while (status == -1 && errno == EINTR);
    if (status > 0) {
        if (NULL != response.cb) {
            response.cb(response.msg);
        }
        janet_ev_dec_refcount();
        goto recur;
    }
}

static void janet_ev_cleanup_selfpipe(void) {
    close(janet_vm.selfpipe[0]);
    close(janet_vm.selfpipe[1]);
}

#endif

#ifdef JANET_WINDOWS

static JanetTimestamp ts_now(void) {
    return (JanetTimestamp) GetTickCount64();
}

void janet_ev_init(void) {
    janet_ev_init_common();
    janet_vm.iocp = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
    if (NULL == janet_vm.iocp) janet_panic("could not create io completion port");
}

void janet_ev_deinit(void) {
    janet_ev_deinit_common();
    CloseHandle(janet_vm.iocp);
}

static void janet_register_stream(JanetStream *stream) {
    if (NULL == CreateIoCompletionPort(stream->handle, janet_vm.iocp, (ULONG_PTR) stream, 0)) {
        if (stream->flags & (JANET_STREAM_READABLE | JANET_STREAM_WRITABLE | JANET_STREAM_ACCEPTABLE)) {
            janet_panicf("failed to listen for events: %V", janet_ev_lasterr());
        }
        stream->flags |= JANET_STREAM_UNREGISTERED;
    }
}

void janet_loop1_impl(int has_timeout, JanetTimestamp to) {
    ULONG_PTR completionKey = 0;
    DWORD num_bytes_transferred = 0;
    LPOVERLAPPED overlapped = NULL;

    /* Calculate how long to wait before timeout */
    uint64_t waittime;
    if (has_timeout) {
        JanetTimestamp now = ts_now();
        if (now > to) {
            waittime = 0;
        } else {
            waittime = (uint64_t)(to - now);
        }
    } else {
        waittime = INFINITE;
    }
    BOOL result = GetQueuedCompletionStatus(janet_vm.iocp, &num_bytes_transferred, &completionKey, &overlapped, (DWORD) waittime);

    if (result || overlapped) {
        if (0 == completionKey) {
            /* Custom event */
            JanetSelfPipeEvent *response = (JanetSelfPipeEvent *)(overlapped);
            if (NULL != response->cb) {
                response->cb(response->msg);
            }
            janet_ev_dec_refcount();
            janet_free(response);
        } else {
            /* Normal event */
            JanetOverlapped *jo = (JanetOverlapped *) overlapped;
            JanetStream *stream = (JanetStream *) completionKey;
            JanetFiber *fiber = NULL;
            if (stream->read_fiber && stream->read_fiber->ev_state == jo) {
                fiber = stream->read_fiber;
            } else if (stream->write_fiber && stream->write_fiber->ev_state == jo) {
                fiber = stream->write_fiber;
            }
            if (fiber != NULL) {
                fiber->flags &= ~JANET_FIBER_EV_FLAG_IN_FLIGHT;
                jo->bytes_transfered = (ULONG_PTR) num_bytes_transferred;
                fiber->ev_callback(fiber, result ? JANET_ASYNC_EVENT_COMPLETE : JANET_ASYNC_EVENT_FAILED);
            } else {
                janet_free((void *) jo);
                janet_ev_dec_refcount();
            }
            janet_stream_checktoclose(stream);
        }
    }
}

void janet_stream_edge_triggered(JanetStream *stream) {
    (void) stream;
}

void janet_stream_level_triggered(JanetStream *stream) {
    (void) stream;
}

#elif defined(JANET_EV_EPOLL)

static JanetTimestamp ts_now(void) {
    struct timespec now;
    janet_assert(-1 != janet_gettime(&now, JANET_TIME_MONOTONIC), "failed to get time");
    uint64_t res = 1000 * now.tv_sec;
    res += now.tv_nsec / 1000000;
    return res;
}

/* Wait for the next event */
static void janet_register_stream_impl(JanetStream *stream, int mod, int edge_trigger) {
    struct epoll_event ev;
    ev.events = edge_trigger ? EPOLLET : 0;
    if (stream->flags & (JANET_STREAM_READABLE | JANET_STREAM_ACCEPTABLE)) ev.events |= EPOLLIN;
    if (stream->flags & JANET_STREAM_WRITABLE) ev.events |= EPOLLOUT;
    ev.data.ptr = stream;
    int status;
    do {
        status = epoll_ctl(janet_vm.epoll, mod ? EPOLL_CTL_MOD : EPOLL_CTL_ADD, stream->handle, &ev);
    } while (status == -1 && errno == EINTR);
    if (status == -1) {
        if (errno == EPERM) {
            /* Couldn't add to event loop, so assume that it completes
             * synchronously. */
            stream->flags |= JANET_STREAM_UNREGISTERED;
        } else {
            /* Unexpected error */
            janet_panicv(janet_ev_lasterr());
        }
    }
}

static void janet_register_stream(JanetStream *stream) {
    janet_register_stream_impl(stream, 0, 1);
}

void janet_stream_edge_triggered(JanetStream *stream) {
    janet_register_stream_impl(stream, 1, 1);
}

void janet_stream_level_triggered(JanetStream *stream) {
    janet_register_stream_impl(stream, 1, 0);
}

void janet_unregister_stream(JanetStream *stream) {
    if (stream->flags & JANET_STREAM_NODUPS) return;
    int status;
    do {
        status = epoll_ctl(janet_vm.epoll, EPOLL_CTL_DEL, stream->handle, NULL);
    } while (status == -1 && errno == EINTR);
    if (status == -1) {
        janet_panicv(janet_ev_lasterr());
    }
    stream->flags |= JANET_STREAM_UNREGISTERED;
}

#define JANET_EPOLL_MAX_EVENTS 64
void janet_loop1_impl(int has_timeout, JanetTimestamp timeout) {
    struct itimerspec its;
    if (janet_vm.timer_enabled || has_timeout) {
        memset(&its, 0, sizeof(its));
        if (has_timeout) {
            its.it_value.tv_sec = timeout / 1000;
            its.it_value.tv_nsec = (timeout % 1000) * 1000000;
        }
        timerfd_settime(janet_vm.timerfd, TFD_TIMER_ABSTIME, &its, NULL);
    }
    janet_vm.timer_enabled = has_timeout;

    /* Poll for events */
    struct epoll_event events[JANET_EPOLL_MAX_EVENTS];
    int ready;
    do {
        ready = epoll_wait(janet_vm.epoll, events, JANET_EPOLL_MAX_EVENTS, -1);
    } while (ready == -1 && errno == EINTR);
    if (ready == -1) {
        JANET_EXIT("failed to poll events");
    }

    /* Step state machines */
    for (int i = 0; i < ready; i++) {
        void *p = events[i].data.ptr;
        if (&janet_vm.timerfd == p) {
            /* Timer expired, ignore */;
        } else if (janet_vm.selfpipe == p) {
            /* Self-pipe handling */
            janet_ev_handle_selfpipe();
        } else {
            JanetStream *stream = p;
            int mask = events[i].events;
            int has_err = mask & EPOLLERR;
            int has_hup = mask & EPOLLHUP;
            JanetFiber *rf = stream->read_fiber;
            JanetFiber *wf = stream->write_fiber;
            if (rf) {
                if (rf->ev_callback && (mask & EPOLLIN)) {
                    rf->ev_callback(rf, JANET_ASYNC_EVENT_READ);
                }
                if (rf->ev_callback && has_err) {
                    rf->ev_callback(rf, JANET_ASYNC_EVENT_ERR);
                }
                if (rf->ev_callback && has_hup) {
                    rf->ev_callback(rf, JANET_ASYNC_EVENT_HUP);
                }
            }
            if (wf) {
                if (wf->ev_callback && (mask & EPOLLOUT)) {
                    wf->ev_callback(wf, JANET_ASYNC_EVENT_WRITE);
                }
                if (wf->ev_callback && has_err) {
                    wf->ev_callback(wf, JANET_ASYNC_EVENT_ERR);
                }
                if (wf->ev_callback && has_hup) {
                    wf->ev_callback(wf, JANET_ASYNC_EVENT_HUP);
                }
            }
            janet_stream_checktoclose(stream);
        }
    }
}

void janet_ev_init(void) {
    janet_ev_init_common();
    janet_ev_setup_selfpipe();
    janet_vm.epoll = epoll_create1(EPOLL_CLOEXEC);
    janet_vm.timerfd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
    janet_vm.timer_enabled = 0;
    if (janet_vm.epoll == -1 || janet_vm.timerfd == -1) goto error;
    struct epoll_event ev;
    ev.events = EPOLLIN | EPOLLET;
    ev.data.ptr = &janet_vm.timerfd;
    if (-1 == epoll_ctl(janet_vm.epoll, EPOLL_CTL_ADD, janet_vm.timerfd, &ev)) goto error;
    ev.events = EPOLLIN | EPOLLET;
    ev.data.ptr = janet_vm.selfpipe;
    if (-1 == epoll_ctl(janet_vm.epoll, EPOLL_CTL_ADD, janet_vm.selfpipe[0], &ev)) goto error;
    return;
error:
    JANET_EXIT("failed to initialize event loop");
}

void janet_ev_deinit(void) {
    janet_ev_deinit_common();
    close(janet_vm.epoll);
    close(janet_vm.timerfd);
    janet_ev_cleanup_selfpipe();
    janet_vm.epoll = 0;
}

/*
 * End epoll implementation
 */

#elif defined(JANET_EV_KQUEUE)
/* Definition from:
 *   https://github.com/wahern/cqueues/blob/master/src/lib/kpoll.c
 * NetBSD uses intptr_t while others use void * for .udata */
#define EV_SETx(ev, a, b, c, d, e, f) EV_SET((ev), (a), (b), (c), (d), (e), ((__typeof__((ev)->udata))(f)))
#define JANET_KQUEUE_MIN_INTERVAL 0

/* NOTE:
 * NetBSD and OpenBSD expect things are always intervals, and FreeBSD doesn't
 * like an ABSTIME in the past so just use intervals always. Introduces a
 * calculation to determine the minimum timeout per timeout requested of
 * kqueue. Also note that NetBSD doesn't accept timeout intervals less than 1
 * millisecond, so correct all intervals on that platform to be at least 1
 * millisecond.*/
JanetTimestamp to_interval(const JanetTimestamp ts) {
    return ts >= JANET_KQUEUE_MIN_INTERVAL ? ts : JANET_KQUEUE_MIN_INTERVAL;
}
#define JANET_KQUEUE_INTERVAL(timestamp) (to_interval((timestamp - ts_now())))

static JanetTimestamp ts_now(void) {
    struct timespec now;
    janet_assert(-1 != janet_gettime(&now, JANET_TIME_MONOTONIC), "failed to get time");
    uint64_t res = 1000 * now.tv_sec;
    res += now.tv_nsec / 1000000;
    return res;
}

/* NOTE: Assumes Janet's timestamp precision is in milliseconds. */
static void timestamp2timespec(struct timespec *t, JanetTimestamp ts) {
    t->tv_sec = ts == 0 ? 0 : ts / 1000;
    t->tv_nsec = ts == 0 ? 0 : (ts % 1000) * 1000000;
}

void janet_register_stream_impl(JanetStream *stream, int edge_trigger) {
    struct kevent kevs[2];
    int length = 0;
    int clear = edge_trigger ? EV_CLEAR : 0;
    if (stream->flags & (JANET_STREAM_READABLE | JANET_STREAM_ACCEPTABLE)) {
        EV_SETx(&kevs[length++], stream->handle, EVFILT_READ, EV_ADD | EV_ENABLE | clear, 0, 0, stream);
    }
    if (stream->flags & JANET_STREAM_WRITABLE) {
        EV_SETx(&kevs[length++], stream->handle, EVFILT_WRITE, EV_ADD | EV_ENABLE | clear, 0, 0, stream);
    }
    int status;
    do {
        status = kevent(janet_vm.kq, kevs, length, NULL, 0, NULL);
    } while (status == -1 && errno == EINTR);
    if (status == -1) {
        stream->flags |= JANET_STREAM_UNREGISTERED;
    }
}

void janet_register_stream(JanetStream *stream) {
    janet_register_stream_impl(stream, 1);
}

void janet_stream_edge_triggered(JanetStream *stream) {
    janet_register_stream_impl(stream, 1);
}

void janet_stream_level_triggered(JanetStream *stream) {
    /* On macos, we seem to need to delete any registered events before re-registering without
     * EV_CLEAR, otherwise the new event will still have EV_CLEAR set erroneously. This could be a
     * kernel bug, but unfortunately the specification is vague here, esp. in regards to where and when
     * EV_CLEAR is set automatically. */
    struct kevent kevs[2];
    int length = 0;
    if (stream->flags & (JANET_STREAM_READABLE | JANET_STREAM_ACCEPTABLE)) {
        EV_SETx(&kevs[length++], stream->handle, EVFILT_READ, EV_DELETE, 0, 0, stream);
    }
    if (stream->flags & JANET_STREAM_WRITABLE) {
        EV_SETx(&kevs[length++], stream->handle, EVFILT_WRITE, EV_DELETE, 0, 0, stream);
    }
    int status;
    do {
        status = kevent(janet_vm.kq, kevs, length, NULL, 0, NULL);
    } while (status == -1 && errno == EINTR);
    janet_register_stream_impl(stream, 0);
}

void janet_unregister_stream(JanetStream *stream) {
    if (stream->flags & JANET_STREAM_NODUPS) return;
    struct kevent kevs[2];
    int length = 0;
    if (stream->flags & (JANET_STREAM_READABLE | JANET_STREAM_ACCEPTABLE)) {
        EV_SETx(&kevs[length++], stream->handle, EVFILT_READ, EV_DELETE, 0, 0, stream);
    }
    if (stream->flags & JANET_STREAM_WRITABLE) {
        EV_SETx(&kevs[length++], stream->handle, EVFILT_WRITE, EV_DELETE, 0, 0, stream);
    }
    int status;
    do {
        status = kevent(janet_vm.kq, kevs, length, NULL, 0, NULL);
    } while (status == -1 && errno == EINTR);
    /* Status might be -1 on BSDs for subprocesses */
    stream->flags |= JANET_STREAM_UNREGISTERED;
}

#define JANET_KQUEUE_MAX_EVENTS 512

void janet_loop1_impl(int has_timeout, JanetTimestamp timeout) {
    /* Poll for events */
    /* NOTE:
     * We calculate the timeout interval per iteration. When the interval
     * drops to 0 or negative, we effect a timeout of 0. Effecting a timeout
     * of infinity will not work and could make other fibers with timeouts
     * miss their timeouts if we did so.
     * JANET_KQUEUE_INTERVAL insures we have a timeout of no less than 0. */
    int status;
    struct timespec ts;
    struct kevent events[JANET_KQUEUE_MAX_EVENTS];
    do {
        if (janet_vm.timer_enabled || has_timeout) {
            timestamp2timespec(&ts, JANET_KQUEUE_INTERVAL(timeout));
            status = kevent(janet_vm.kq, NULL, 0, events,
                            JANET_KQUEUE_MAX_EVENTS, &ts);
        } else {
            status = kevent(janet_vm.kq, NULL, 0, events,
                            JANET_KQUEUE_MAX_EVENTS, NULL);
        }
    } while (status == -1 && errno == EINTR);
    if (status == -1) {
        JANET_EXIT("failed to poll events");
    }

    /* Make sure timer is set accordingly. */
    janet_vm.timer_enabled = has_timeout;

    /* Step state machines */
    for (int i = 0; i < status; i++) {
        void *p = (void *) events[i].udata;
        if (janet_vm.selfpipe == p) {
            /* Self-pipe handling */
            janet_ev_handle_selfpipe();
        } else {
            JanetStream *stream = p;
            int filt = events[i].filter;
            int has_err = events[i].flags & EV_ERROR;
            int has_hup = events[i].flags & EV_EOF;
            for (int j = 0; j < 2; j++) {
                JanetFiber *f = j ? stream->read_fiber : stream->write_fiber;
                if (!f) continue;
                if (f->ev_callback && has_err) {
                    f->ev_callback(f, JANET_ASYNC_EVENT_ERR);
                }
                if (f->ev_callback && (filt == EVFILT_READ) && f == stream->read_fiber) {
                    f->ev_callback(f, JANET_ASYNC_EVENT_READ);
                }
                if (f->ev_callback && (filt == EVFILT_WRITE) && f == stream->write_fiber) {
                    f->ev_callback(f, JANET_ASYNC_EVENT_WRITE);
                }
                if (f->ev_callback && has_hup) {
                    f->ev_callback(f, JANET_ASYNC_EVENT_HUP);
                }
            }
            janet_stream_checktoclose(stream);
        }
    }
}

void janet_ev_init(void) {
    janet_ev_init_common();
    /* TODO - replace selfpipe with EVFILT_USER (or other events) */
    janet_ev_setup_selfpipe();
    janet_vm.kq = kqueue();
    janet_vm.timer_enabled = 0;
    if (janet_vm.kq == -1) goto error;
    struct kevent event;
    EV_SETx(&event, janet_vm.selfpipe[0], EVFILT_READ, EV_ADD | EV_ENABLE, 0, 0, janet_vm.selfpipe);
    int status;
    do {
        status = kevent(janet_vm.kq, &event, 1, NULL, 0, NULL);
    } while (status == -1 && errno != EINTR);
    if (status == -1) goto error;
    return;
error:
    JANET_EXIT("failed to initialize event loop");
}

void janet_ev_deinit(void) {
    janet_ev_deinit_common();
    close(janet_vm.kq);
    janet_ev_cleanup_selfpipe();
    janet_vm.kq = 0;
}

#elif defined(JANET_EV_POLL)

/* Simple poll implementation. Efficiency is not the goal here, although the poll implementation should be farily efficient
 * for low numbers of concurrent file descriptors. Rather, the code should be simple, portable, correct, and mirror the
 * epoll and kqueue code. */

static JanetTimestamp ts_now(void) {
    struct timespec now;
    janet_assert(-1 != janet_gettime(&now, JANET_TIME_MONOTONIC), "failed to get time");
    uint64_t res = 1000 * now.tv_sec;
    res += now.tv_nsec / 1000000;
    return res;
}

/* Wait for the next event */
void janet_register_stream(JanetStream *stream) {
    struct pollfd ev = {0};
    stream->index = (uint32_t) janet_vm.stream_count;
    size_t new_count = janet_vm.stream_count + 1;
    if (new_count > janet_vm.stream_capacity) {
        size_t new_cap = new_count * 2;
        janet_vm.fds = janet_realloc(janet_vm.fds, (1 + new_cap) * sizeof(struct pollfd));
        janet_vm.streams = janet_realloc(janet_vm.streams, new_cap * sizeof(JanetStream *));
        if (!janet_vm.fds || !janet_vm.streams) {
            JANET_OUT_OF_MEMORY;
        }
        janet_vm.stream_capacity = new_cap;
    }
    ev.fd = stream->handle;
    ev.events = POLLIN | POLLOUT;
    janet_vm.fds[janet_vm.stream_count + 1] = ev;
    janet_vm.streams[janet_vm.stream_count] = stream;
    janet_vm.stream_count = new_count;
}

void janet_unregister_stream(JanetStream *stream) {
    uint32_t i = stream->index;
    size_t j = janet_vm.stream_count - 1;
    JanetStream *last = janet_vm.streams[j];
    struct pollfd lastfd = janet_vm.fds[j + 1];
    janet_vm.fds[i + 1] = lastfd;
    janet_vm.streams[i] = last;
    last->index = stream->index;
    janet_vm.stream_count--;
    stream->flags |= JANET_STREAM_UNREGISTERED;
}

void janet_stream_edge_triggered(JanetStream *stream) {
    (void) stream;
}

void janet_stream_level_triggered(JanetStream *stream) {
    (void) stream;
}

void janet_loop1_impl(int has_timeout, JanetTimestamp timeout) {

    /* set event flags */
    for (size_t i = 0; i < janet_vm.stream_count; i++) {
        JanetStream *stream = janet_vm.streams[i];
        struct pollfd *pfd = janet_vm.fds + i + 1;
        pfd->events = 0;
        pfd->revents = 0;
        JanetFiber *rf = stream->read_fiber;
        JanetFiber *wf = stream->write_fiber;
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
            JanetTimestamp now = ts_now();
            to = now > timeout ? 0 : (int)(timeout - now);
        }
        ready = poll(janet_vm.fds, janet_vm.stream_count + 1, to);
    } while (ready == -1 && errno == EINTR);
    if (ready == -1) {
        JANET_EXIT("failed to poll events");
    }

    /* Undo negative hack */
    for (size_t i = 0; i < janet_vm.stream_count; i++) {
        struct pollfd *pfd = janet_vm.fds + i + 1;
        if (pfd->fd < 0) {
            pfd->fd = -pfd->fd;
        }
    }

    /* Check selfpipe */
    if (janet_vm.fds[0].revents & POLLIN) {
        janet_vm.fds[0].revents = 0;
        janet_ev_handle_selfpipe();
    }

    /* Step state machines */
    for (size_t i = 0; i < janet_vm.stream_count; i++) {
        struct pollfd *pfd = janet_vm.fds + i + 1;
        JanetStream *stream = janet_vm.streams[i];
        int mask = pfd->revents;
        if (!mask) continue;
        int has_err = mask & POLLERR;
        int has_hup = mask & POLLHUP;
        JanetFiber *rf = stream->read_fiber;
        JanetFiber *wf = stream->write_fiber;
        if (rf) {
            if (rf->ev_callback && (mask & POLLIN)) {
                rf->ev_callback(rf, JANET_ASYNC_EVENT_READ);
            } else if (rf->ev_callback && has_hup) {
                rf->ev_callback(rf, JANET_ASYNC_EVENT_HUP);
            } else if (rf->ev_callback && has_err) {
                rf->ev_callback(rf, JANET_ASYNC_EVENT_ERR);
            }
        }
        if (wf) {
            if (wf->ev_callback && (mask & POLLOUT)) {
                wf->ev_callback(wf, JANET_ASYNC_EVENT_WRITE);
            } else if (wf->ev_callback && has_hup) {
                wf->ev_callback(wf, JANET_ASYNC_EVENT_HUP);
            } else if (wf->ev_callback && has_err) {
                wf->ev_callback(wf, JANET_ASYNC_EVENT_ERR);
            }
        }
        janet_stream_checktoclose(stream);
    }
}

void janet_ev_init(void) {
    janet_ev_init_common();
    janet_vm.fds = NULL;
    janet_ev_setup_selfpipe();
    janet_vm.fds = janet_malloc(sizeof(struct pollfd));
    if (NULL == janet_vm.fds) {
        JANET_OUT_OF_MEMORY;
    }
    janet_vm.fds[0].fd = janet_vm.selfpipe[0];
    janet_vm.fds[0].events = POLLIN;
    janet_vm.fds[0].revents = 0;
    janet_vm.streams = NULL;
    janet_vm.stream_count = 0;
    janet_vm.stream_capacity = 0;
    return;
}

void janet_ev_deinit(void) {
    janet_ev_deinit_common();
    janet_ev_cleanup_selfpipe();
    janet_free(janet_vm.fds);
    janet_free(janet_vm.streams);
    janet_vm.fds = NULL;
    janet_vm.streams = NULL;
}

#endif

/*
 * End poll implementation
 */

/*
 * Generic Callback system. Post a function pointer + data to the event loop (from another
 * thread or even a signal handler). Allows posting events from another thread or signal handler.
 */
void janet_ev_post_event(JanetVM *vm, JanetCallback cb, JanetEVGenericMessage msg) {
    vm = vm ? vm : &janet_vm;
    janet_atomic_inc(&vm->listener_count);
#ifdef JANET_WINDOWS
    JanetHandle iocp = vm->iocp;
    JanetSelfPipeEvent *event = janet_malloc(sizeof(JanetSelfPipeEvent));
    if (NULL == event) {
        JANET_OUT_OF_MEMORY;
    }
    event->msg = msg;
    event->cb = cb;
    janet_assert(PostQueuedCompletionStatus(iocp,
                                            sizeof(JanetSelfPipeEvent),
                                            0,
                                            (LPOVERLAPPED) event),
                 "failed to post completion event");
#else
    JanetSelfPipeEvent event;
    memset(&event, 0, sizeof(event));
    event.msg = msg;
    event.cb = cb;
    int fd = vm->selfpipe[1];
    /* handle a bit of back pressure before giving up. */
    int tries = 20;
    while (tries > 0) {
        int status;
        do {
            status = write(fd, &event, sizeof(event));
        } while (status == -1 && errno == EINTR);
        if (status > 0) break;
        sleep(0);
        tries--;
    }
    janet_assert(tries > 0, "failed to write event to self-pipe");
#endif
}

/*
 * Threaded calls
 */

#ifdef JANET_WINDOWS
static DWORD WINAPI janet_thread_body(LPVOID ptr) {
    JanetEVThreadInit *init = (JanetEVThreadInit *)ptr;
    JanetEVGenericMessage msg = init->msg;
    JanetThreadedSubroutine subr = init->subr;
    JanetThreadedCallback cb = init->cb;
    JanetHandle iocp = init->write_pipe;
    /* Reuse memory from thread init for returning data */
    init->msg = subr(msg);
    init->cb = cb;
    janet_assert(PostQueuedCompletionStatus(iocp,
                                            sizeof(JanetSelfPipeEvent),
                                            0,
                                            (LPOVERLAPPED) init),
                 "failed to post completion event");
    return 0;
}
#else
static void *janet_thread_body(void *ptr) {
    JanetEVThreadInit *init = (JanetEVThreadInit *)ptr;
    JanetEVGenericMessage msg = init->msg;
    JanetThreadedSubroutine subr = init->subr;
    JanetThreadedCallback cb = init->cb;
    int fd = init->write_pipe;
    janet_free(init);
    JanetSelfPipeEvent response;
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

void janet_ev_threaded_call(JanetThreadedSubroutine fp, JanetEVGenericMessage arguments, JanetThreadedCallback cb) {
    JanetEVThreadInit *init = janet_malloc(sizeof(JanetEVThreadInit));
    if (NULL == init) {
        JANET_OUT_OF_MEMORY;
    }
    init->msg = arguments;
    init->subr = fp;
    init->cb = cb;

#ifdef JANET_WINDOWS
    init->write_pipe = janet_vm.iocp;
    HANDLE thread_handle = CreateThread(NULL, 0, janet_thread_body, init, 0, NULL);
    if (NULL == thread_handle) {
        janet_free(init);
        janet_panic("failed to create thread");
    }
    CloseHandle(thread_handle); /* detach from thread */
#else
    init->write_pipe = janet_vm.selfpipe[1];
    pthread_t waiter_thread;
    int err = pthread_create(&waiter_thread, &janet_vm.new_thread_attr, janet_thread_body, init);
    if (err) {
        janet_free(init);
        janet_panicf("%s", janet_strerror(err));
    }
#endif

    /* Increment ev refcount so we don't quit while waiting for a subprocess */
    janet_ev_inc_refcount();
}

/* Default callback for janet_ev_threaded_await. */
void janet_ev_default_threaded_callback(JanetEVGenericMessage return_value) {
    if (return_value.fiber == NULL) {
        /* Clean up */
        switch (return_value.tag) {
            default:
                break;
            case JANET_EV_TCTAG_STRINGF:
            case JANET_EV_TCTAG_ERR_STRINGF:
                janet_free(return_value.argp);
                break;
        }
        return;
    }
    if (janet_fiber_can_resume(return_value.fiber)) {
        switch (return_value.tag) {
            default:
                break;
            case JANET_EV_TCTAG_NIL:
                janet_schedule(return_value.fiber, janet_wrap_nil());
                break;
            case JANET_EV_TCTAG_INTEGER:
                janet_schedule(return_value.fiber, janet_wrap_integer(return_value.argi));
                break;
            case JANET_EV_TCTAG_STRING:
            case JANET_EV_TCTAG_STRINGF:
                janet_schedule(return_value.fiber, janet_cstringv((const char *) return_value.argp));
                break;
            case JANET_EV_TCTAG_KEYWORD:
                janet_schedule(return_value.fiber, janet_ckeywordv((const char *) return_value.argp));
                break;
            case JANET_EV_TCTAG_ERR_STRING:
            case JANET_EV_TCTAG_ERR_STRINGF:
                janet_cancel(return_value.fiber, janet_cstringv((const char *) return_value.argp));
                break;
            case JANET_EV_TCTAG_ERR_KEYWORD:
                janet_cancel(return_value.fiber, janet_ckeywordv((const char *) return_value.argp));
                break;
            case JANET_EV_TCTAG_BOOLEAN:
                janet_schedule(return_value.fiber, janet_wrap_boolean(return_value.argi));
                break;
        }
    }
    /* Clean up */
    switch (return_value.tag) {
        default:
            break;
        case JANET_EV_TCTAG_STRINGF:
        case JANET_EV_TCTAG_ERR_STRINGF:
            janet_free(return_value.argp);
            break;
    }
    janet_gcunroot(janet_wrap_fiber(return_value.fiber));
}

/* Convenience method for common case */
JANET_NO_RETURN
void janet_ev_threaded_await(JanetThreadedSubroutine fp, int tag, int argi, void *argp) {
    JanetEVGenericMessage arguments;
    memset(&arguments, 0, sizeof(arguments));
    arguments.tag = tag;
    arguments.argi = argi;
    arguments.argp = argp;
    arguments.fiber = janet_root_fiber();
    janet_gcroot(janet_wrap_fiber(arguments.fiber));
    janet_ev_threaded_call(fp, arguments, janet_ev_default_threaded_callback);
    janet_await();
}

/*
 * C API helpers for reading and writing from streams.
 * There is some networking code in here as well as generic
 * reading and writing primitives.
 */

void janet_stream_flags(JanetStream *stream, uint32_t flags) {
    if (stream->flags & JANET_STREAM_CLOSED) {
        janet_panic("stream is closed");
    }
    if ((stream->flags & flags) != flags) {
        const char *rmsg = "", *wmsg = "", *amsg = "", *dmsg = "", *smsg = "stream";
        if (flags & JANET_STREAM_READABLE) rmsg = "readable ";
        if (flags & JANET_STREAM_WRITABLE) wmsg = "writable ";
        if (flags & JANET_STREAM_ACCEPTABLE) amsg = "server ";
        if (flags & JANET_STREAM_UDPSERVER) dmsg = "datagram ";
        if (flags & JANET_STREAM_SOCKET) smsg = "socket";
        janet_panicf("bad stream, expected %s%s%s%s%s", rmsg, wmsg, amsg, dmsg, smsg);
    }
}

/* When there is an IO error, we need to be able to convert it to a Janet
 * string to raise a Janet error. */
#ifdef JANET_WINDOWS
#define JANET_EV_CHUNKSIZE 4096
Janet janet_ev_lasterr(void) {
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
    return janet_cstringv(msgbuf);
}
#else
Janet janet_ev_lasterr(void) {
    return janet_cstringv(janet_strerror(errno));
}
#endif

/* State machine for read/recv/recvfrom */

typedef enum {
    JANET_ASYNC_READMODE_READ,
    JANET_ASYNC_READMODE_RECV,
    JANET_ASYNC_READMODE_RECVFROM
} JanetReadMode;

typedef struct {
#ifdef JANET_WINDOWS
    JanetOverlapped overlapped;
    DWORD flags;
#ifdef JANET_NET
    WSABUF wbuf;
    struct sockaddr from;
    int fromlen;
#endif
    uint8_t chunk_buf[JANET_EV_CHUNKSIZE];
#else
    int flags;
#endif
    int32_t bytes_left;
    int32_t bytes_read;
    JanetBuffer *buf;
    int is_chunk;
    JanetReadMode mode;
} StateRead;

void ev_callback_read(JanetFiber *fiber, JanetAsyncEvent event) {
    JanetStream *stream = fiber->ev_stream;
    StateRead *state = (StateRead *) fiber->ev_state;
    switch (event) {
        default:
            break;
        case JANET_ASYNC_EVENT_MARK:
            janet_mark(janet_wrap_buffer(state->buf));
            break;
        case JANET_ASYNC_EVENT_CLOSE:
            janet_schedule(fiber, janet_wrap_nil());
            janet_async_end(fiber);
            break;
#ifdef JANET_WINDOWS
        case JANET_ASYNC_EVENT_FAILED:
        case JANET_ASYNC_EVENT_COMPLETE: {
            /* Called when read finished */
            uint32_t ev_bytes = (uint32_t) state->overlapped.bytes_transfered;
            state->bytes_read += ev_bytes;
            if (state->bytes_read == 0 && (state->mode != JANET_ASYNC_READMODE_RECVFROM)) {
                janet_schedule(fiber, janet_wrap_nil());
                janet_async_end(fiber);
                return;
            }

            janet_buffer_push_bytes(state->buf, state->chunk_buf, ev_bytes);
            state->bytes_left -= ev_bytes;

            if (state->bytes_left == 0 || !state->is_chunk || ev_bytes == 0) {
                Janet resume_val;
#ifdef JANET_NET
                if (state->mode == JANET_ASYNC_READMODE_RECVFROM) {
                    void *abst = janet_abstract(&janet_address_type, state->fromlen);
                    memcpy(abst, &state->from, state->fromlen);
                    resume_val = janet_wrap_abstract(abst);
                } else
#endif
                {
                    resume_val = janet_wrap_buffer(state->buf);
                }
                janet_schedule(fiber, resume_val);
                janet_async_end(fiber);
                return;
            }
        }

        /* fallthrough */
        case JANET_ASYNC_EVENT_INIT: {
            int32_t chunk_size = state->bytes_left > JANET_EV_CHUNKSIZE ? JANET_EV_CHUNKSIZE : state->bytes_left;
            memset(&(state->overlapped), 0, sizeof(JanetOverlapped));
            int status;
#ifdef JANET_NET
            if (state->mode == JANET_ASYNC_READMODE_RECVFROM) {
                state->wbuf.len = (ULONG) chunk_size;
                state->wbuf.buf = (char *) state->chunk_buf;
                state->fromlen = sizeof(state->from);
                status = WSARecvFrom((SOCKET) stream->handle, &state->wbuf, 1,
                                     NULL, &state->flags, &state->from, &state->fromlen, &state->overlapped.as.wsaoverlapped, NULL);
                if (status && (WSA_IO_PENDING != WSAGetLastError())) {
                    janet_cancel(fiber, janet_ev_lasterr());
                    janet_async_end(fiber);
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
                            janet_schedule(fiber, janet_wrap_buffer(state->buf));
                        } else {
                            janet_schedule(fiber, janet_wrap_nil());
                        }
                    } else {
                        janet_cancel(fiber, janet_ev_lasterr());
                    }
                    janet_async_end(fiber);
                    return;
                }
            }
            janet_async_in_flight(fiber);
        }
        break;
#else
        case JANET_ASYNC_EVENT_ERR: {
            if (state->bytes_read) {
                janet_schedule(fiber, janet_wrap_buffer(state->buf));
            } else {
                janet_schedule(fiber, janet_wrap_nil());
            }
            stream->read_fiber = NULL;
            janet_async_end(fiber);
            break;
        }

    read_more:
        case JANET_ASYNC_EVENT_HUP:
        case JANET_ASYNC_EVENT_INIT:
        case JANET_ASYNC_EVENT_READ: {
            JanetBuffer *buffer = state->buf;
            int32_t bytes_left = state->bytes_left;
            int32_t read_limit = state->is_chunk ? (bytes_left > 4096 ? 4096 : bytes_left) : bytes_left;
            janet_buffer_extra(buffer, read_limit);
            ssize_t nread;
#ifdef JANET_NET
            char saddr[256];
            socklen_t socklen = sizeof(saddr);
#endif
            do {
#ifdef JANET_NET
                if (state->mode == JANET_ASYNC_READMODE_RECVFROM) {
                    nread = recvfrom(stream->handle, buffer->data + buffer->count, read_limit, state->flags,
                                     (struct sockaddr *)&saddr, &socklen);
                } else if (state->mode == JANET_ASYNC_READMODE_RECV) {
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
                if (errno == EPIPE && (state->mode != JANET_ASYNC_READMODE_RECVFROM)) {
                    nread = 0;
                } else {
                    janet_cancel(fiber, janet_ev_lasterr());
                    janet_async_end(fiber);
                    break;
                }
            }

            /* Only allow 0-length packets in recv-from. In stream protocols, a zero length packet is EOS. */
            state->bytes_read += nread;
            if (state->bytes_read == 0 && (state->mode != JANET_ASYNC_READMODE_RECVFROM)) {
                janet_schedule(fiber, janet_wrap_nil());
                janet_async_end(fiber);
                break;
            }

            /* Increment buffer counts */
            buffer->count += nread;
            bytes_left -= nread;
            state->bytes_left = bytes_left;

            /* Resume if done */
            if (!state->is_chunk || bytes_left == 0 || nread == 0) {
                Janet resume_val;
#ifdef JANET_NET
                if (state->mode == JANET_ASYNC_READMODE_RECVFROM) {
                    void *abst = janet_abstract(&janet_address_type, socklen);
                    memcpy(abst, &saddr, socklen);
                    resume_val = janet_wrap_abstract(abst);
                } else
#endif
                {
                    resume_val = janet_wrap_buffer(buffer);
                }
                janet_schedule(fiber, resume_val);
                janet_async_end(fiber);
                break;
            }

            /* Read some more if possible */
            goto read_more;
        }
        break;
#endif
    }
}

static JANET_NO_RETURN void janet_ev_read_generic(JanetStream *stream, JanetBuffer *buf, int32_t nbytes, int is_chunked, JanetReadMode mode, int flags) {
    StateRead *state = janet_malloc(sizeof(StateRead));
    state->is_chunk = is_chunked;
    state->buf = buf;
    state->bytes_left = nbytes;
    state->bytes_read = 0;
    state->mode = mode;
#ifdef JANET_WINDOWS
    state->flags = (DWORD) flags;
#else
    state->flags = flags;
#endif
    janet_async_start(stream, JANET_ASYNC_LISTEN_READ, ev_callback_read, state);
}

JANET_NO_RETURN void janet_ev_read(JanetStream *stream, JanetBuffer *buf, int32_t nbytes) {
    janet_ev_read_generic(stream, buf, nbytes, 0, JANET_ASYNC_READMODE_READ, 0);
}
JANET_NO_RETURN void janet_ev_readchunk(JanetStream *stream, JanetBuffer *buf, int32_t nbytes) {
    janet_ev_read_generic(stream, buf, nbytes, 1, JANET_ASYNC_READMODE_READ, 0);
}
#ifdef JANET_NET
JANET_NO_RETURN void janet_ev_recv(JanetStream *stream, JanetBuffer *buf, int32_t nbytes, int flags) {
    janet_ev_read_generic(stream, buf, nbytes, 0, JANET_ASYNC_READMODE_RECV, flags);
}
JANET_NO_RETURN void janet_ev_recvchunk(JanetStream *stream, JanetBuffer *buf, int32_t nbytes, int flags) {
    janet_ev_read_generic(stream, buf, nbytes, 1, JANET_ASYNC_READMODE_RECV, flags);
}
JANET_NO_RETURN void janet_ev_recvfrom(JanetStream *stream, JanetBuffer *buf, int32_t nbytes, int flags) {
    janet_ev_read_generic(stream, buf, nbytes, 0, JANET_ASYNC_READMODE_RECVFROM, flags);
}
#endif

/*
 * State machine for write/send/send-to
 */

typedef enum {
    JANET_ASYNC_WRITEMODE_WRITE,
    JANET_ASYNC_WRITEMODE_SEND,
    JANET_ASYNC_WRITEMODE_SENDTO
} JanetWriteMode;

typedef struct {
#ifdef JANET_WINDOWS
    JanetOverlapped overlapped;
    DWORD flags;
#ifdef JANET_NET
    WSABUF wbuf;
#endif
#else
    int flags;
    int32_t start;
#endif
    union {
        JanetBuffer *buf;
        const uint8_t *str;
    } src;
    int is_buffer;
    JanetWriteMode mode;
    void *dest_abst;
} StateWrite;

void ev_callback_write(JanetFiber *fiber, JanetAsyncEvent event) {
    JanetStream *stream = fiber->ev_stream;
    StateWrite *state = (StateWrite *) fiber->ev_state;
    switch (event) {
        default:
            break;
        case JANET_ASYNC_EVENT_MARK: {
            janet_mark(state->is_buffer
                       ? janet_wrap_buffer(state->src.buf)
                       : janet_wrap_string(state->src.str));
            if (state->mode == JANET_ASYNC_WRITEMODE_SENDTO) {
                janet_mark(janet_wrap_abstract(state->dest_abst));
            }
            break;
        }
        case JANET_ASYNC_EVENT_CLOSE:
            janet_cancel(fiber, janet_cstringv("stream closed"));
            janet_async_end(fiber);
            break;
#ifdef JANET_WINDOWS
        case JANET_ASYNC_EVENT_FAILED:
        case JANET_ASYNC_EVENT_COMPLETE: {
            /* Called when write finished */
            uint32_t ev_bytes = (uint32_t) state->overlapped.bytes_transfered;
            if (ev_bytes == 0 && (state->mode != JANET_ASYNC_WRITEMODE_SENDTO)) {
                janet_cancel(fiber, janet_cstringv("disconnect"));
                janet_async_end(fiber);
                return;
            }

            janet_schedule(fiber, janet_wrap_nil());
            janet_async_end(fiber);
            return;
        }
        break;
        case JANET_ASYNC_EVENT_INIT: {
            /* Begin write */
            int32_t len;
            const uint8_t *bytes;
            if (state->is_buffer) {
                /* If buffer, convert to string. */
                /* TODO - be more efficient about this */
                JanetBuffer *buffer = state->src.buf;
                JanetString str = janet_string(buffer->data, buffer->count);
                bytes = str;
                len = buffer->count;
                state->is_buffer = 0;
                state->src.str = str;
            } else {
                bytes = state->src.str;
                len = janet_string_length(bytes);
            }
            memset(&(state->overlapped), 0, sizeof(JanetOverlapped));

            int status;
#ifdef JANET_NET
            if (state->mode == JANET_ASYNC_WRITEMODE_SENDTO) {
                SOCKET sock = (SOCKET) stream->handle;
                state->wbuf.buf = (char *) bytes;
                state->wbuf.len = len;
                const struct sockaddr *to = state->dest_abst;
                int tolen = (int) janet_abstract_size((void *) to);
                status = WSASendTo(sock, &state->wbuf, 1, NULL, state->flags, to, tolen, &state->overlapped.as.wsaoverlapped, NULL);
                if (status) {
                    if (WSA_IO_PENDING == WSAGetLastError()) {
                        janet_async_in_flight(fiber);
                    } else {
                        janet_cancel(fiber, janet_ev_lasterr());
                        janet_async_end(fiber);
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
                        janet_async_in_flight(fiber);
                    } else {
                        janet_cancel(fiber, janet_ev_lasterr());
                        janet_async_end(fiber);
                        return;
                    }
                }
            }
        }
        break;
#else
        case JANET_ASYNC_EVENT_ERR:
            janet_cancel(fiber, janet_cstringv("stream err"));
            janet_async_end(fiber);
            break;
        case JANET_ASYNC_EVENT_HUP:
            janet_cancel(fiber, janet_cstringv("stream hup"));
            janet_async_end(fiber);
            break;
        case JANET_ASYNC_EVENT_INIT:
        case JANET_ASYNC_EVENT_WRITE: {
            int32_t start, len;
            const uint8_t *bytes;
            start = state->start;
            if (state->is_buffer) {
                JanetBuffer *buffer = state->src.buf;
                bytes = buffer->data;
                len = buffer->count;
            } else {
                bytes = state->src.str;
                len = janet_string_length(bytes);
            }
            ssize_t nwrote = 0;
            if (start < len) {
                int32_t nbytes = len - start;
                void *dest_abst = state->dest_abst;
                do {
#ifdef JANET_NET
                    if (state->mode == JANET_ASYNC_WRITEMODE_SENDTO) {
                        nwrote = sendto(stream->handle, bytes + start, nbytes, state->flags,
                                        (struct sockaddr *) dest_abst, janet_abstract_size(dest_abst));
                    } else if (state->mode == JANET_ASYNC_WRITEMODE_SEND) {
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
                    janet_cancel(fiber, janet_ev_lasterr());
                    janet_async_end(fiber);
                    break;
                }

                /* Unless using datagrams, empty message is a disconnect */
                if (nwrote == 0 && !dest_abst) {
                    janet_cancel(fiber, janet_cstringv("disconnect"));
                    janet_async_end(fiber);
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
                janet_schedule(fiber, janet_wrap_nil());
                janet_async_end(fiber);
                break;
            }
            break;
        }
        break;
#endif
    }
}

static JANET_NO_RETURN void janet_ev_write_generic(JanetStream *stream, void *buf, void *dest_abst, JanetWriteMode mode, int is_buffer, int flags) {
    StateWrite *state = janet_malloc(sizeof(StateWrite));
    state->is_buffer = is_buffer;
    state->src.buf = buf;
    state->dest_abst = dest_abst;
    state->mode = mode;
#ifdef JANET_WINDOWS
    state->flags = (DWORD) flags;
#else
    state->flags = flags;
    state->start = 0;
#endif
    janet_async_start(stream, JANET_ASYNC_LISTEN_WRITE, ev_callback_write, state);
}

JANET_NO_RETURN void janet_ev_write_buffer(JanetStream *stream, JanetBuffer *buf) {
    janet_ev_write_generic(stream, buf, NULL, JANET_ASYNC_WRITEMODE_WRITE, 1, 0);
}

JANET_NO_RETURN void janet_ev_write_string(JanetStream *stream, JanetString str) {
    janet_ev_write_generic(stream, (void *) str, NULL, JANET_ASYNC_WRITEMODE_WRITE, 0, 0);
}

#ifdef JANET_NET
JANET_NO_RETURN void janet_ev_send_buffer(JanetStream *stream, JanetBuffer *buf, int flags) {
    janet_ev_write_generic(stream, buf, NULL, JANET_ASYNC_WRITEMODE_SEND, 1, flags);
}

JANET_NO_RETURN void janet_ev_send_string(JanetStream *stream, JanetString str, int flags) {
    janet_ev_write_generic(stream, (void *) str, NULL, JANET_ASYNC_WRITEMODE_SEND, 0, flags);
}

JANET_NO_RETURN void janet_ev_sendto_buffer(JanetStream *stream, JanetBuffer *buf, void *dest, int flags) {
    janet_ev_write_generic(stream, buf, dest, JANET_ASYNC_WRITEMODE_SENDTO, 1, flags);
}

JANET_NO_RETURN void janet_ev_sendto_string(JanetStream *stream, JanetString str, void *dest, int flags) {
    janet_ev_write_generic(stream, (void *) str, dest, JANET_ASYNC_WRITEMODE_SENDTO, 0, flags);
}
#endif

/* For a pipe ID */
#ifdef JANET_WINDOWS
static volatile long PipeSerialNumber;
#endif

/*
 * mode = 0: both sides non-blocking.
 * mode = 1: only read side non-blocking: write side sent to subprocess
 * mode = 2: only write side non-blocking: read side sent to subprocess
 * mode = 3: both sides blocking - for use in two subprocesses (making pipeline from external processes)
 */
int janet_make_pipe(JanetHandle handles[2], int mode) {
#ifdef JANET_WINDOWS
    /*
     * On windows, the built in CreatePipe function doesn't support overlapped IO
     * so we lift from the windows source code and modify for our own version.
     */
    JanetHandle shandle, chandle;
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
             "\\\\.\\Pipe\\JanetPipeFile.%08x.%08x",
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

JANET_CORE_FN(cfun_ev_go,
              "(ev/go fiber-or-fun &opt value supervisor)",
              "Put a fiber on the event loop to be resumed later. If a "
              "function is used, it is wrapped with `fiber/new` first. "
              "Returns a task fiber. Optionally pass a value to resume "
              "with, otherwise resumes with nil. An optional `core/channel` "
              "can be provided as a supervisor. When various events occur "
              "in the newly scheduled fiber, an event will be pushed to the "
              "supervisor. If not provided, the new fiber will inherit the "
              "current supervisor.") {
    janet_arity(argc, 1, 3);
    Janet value = argc >= 2 ? argv[1] : janet_wrap_nil();
    void *supervisor = janet_optabstract(argv, argc, 2, &janet_channel_type, janet_vm.root_fiber->supervisor_channel);
    JanetFiber *fiber;
    if (janet_checktype(argv[0], JANET_FUNCTION)) {
        /* Create a fiber for the user */
        JanetFunction *func = janet_unwrap_function(argv[0]);
        if (func->def->min_arity > 1) {
            janet_panicf("task function must accept 0 or 1 arguments");
        }
        fiber = janet_fiber(func, 64, func->def->min_arity, &value);
        janet_assert(fiber != NULL, "bad fiber-func");
        /* P3.6: ev tasks opt into the lane-shared stack when enabled
         * (fresh fibers only — user-supplied fibers keep theirs). */
        fiber->shared_stack = janet_vm.shared_stacks_on;
        fiber->flags |=
            JANET_FIBER_MASK_ERROR |
            JANET_FIBER_MASK_USER0 |
            JANET_FIBER_MASK_USER1 |
            JANET_FIBER_MASK_USER2 |
            JANET_FIBER_MASK_USER3 |
            JANET_FIBER_MASK_USER4;
        if (!janet_vm.fiber->env) {
            janet_vm.fiber->env = janet_table(0);
        }
        fiber->env = janet_table(0);
        fiber->env->proto = janet_vm.fiber->env;
    } else {
        fiber = janet_getfiber(argv, 0);
        if (janet_fiber_status(fiber) != JANET_STATUS_NEW) {
            janet_panic("can only schedule new fibers where (= (fiber/status f) :new)");
        }
    }
    fiber->supervisor_channel = supervisor;
    janet_schedule(fiber, value);
    return janet_wrap_fiber(fiber);
}

#define JANET_THREAD_SUPERVISOR_FLAG 0x100
/* Skip the worker VM's janet_deinit at thread end. Lane workers use
 * this: several VMs' deinits race on jointly-owned structures (the
 * observed teardown abort), and a lane worker's TLS state is bounded
 * by the lane count anyway. */
#define JANET_THREAD_NODEINIT_FLAG 0x8

/* For ev/thread - Run an interpreter in the new thread. */
JanetEVGenericMessage janet_go_thread_subr(JanetEVGenericMessage args) {
    JanetBuffer *buffer = (JanetBuffer *) args.argp;
    const uint8_t *nextbytes = buffer->data;
    const uint8_t *endbytes = nextbytes + buffer->count;
    uint32_t flags = args.tag;
    args.tag = 0;
    args.argp = NULL;
    janet_init();
    janet_vm.sandbox_flags = (uint32_t) args.argi;
    JanetTryState tstate;
    JanetSignal signal = janet_try(&tstate);
    if (!signal) {

        /* Set abstract registry */
        if (!(flags & 0x2)) {
            Janet aregv = janet_unmarshal(nextbytes, endbytes - nextbytes,
                                          JANET_MARSHAL_UNSAFE, NULL, &nextbytes);
            janet_assert(janet_checktype(aregv, JANET_TABLE), "expected table for abstract registry");
            janet_vm.abstract_registry = janet_unwrap_table(aregv);
            janet_gcroot(janet_wrap_table(janet_vm.abstract_registry));
        }

        /* Get supervisor */
        if (flags & JANET_THREAD_SUPERVISOR_FLAG) {
            Janet sup =
                janet_unmarshal(nextbytes, endbytes - nextbytes,
                                JANET_MARSHAL_UNSAFE, NULL, &nextbytes);
            /* Hack - use a global variable to avoid longjmp clobber */
            janet_vm.user = janet_unwrap_pointer(sup);
        }

        /* Set cfunction registry */
        if (!(flags & 0x4)) {
            uint32_t count1;
            memcpy(&count1, nextbytes, sizeof(count1));
            size_t count = (size_t) count1;
            /* Use division to avoid overflowing size_t */
            janet_assert(count <= (endbytes - nextbytes - sizeof(count1)) / sizeof(JanetCFunRegistry), "thread message invalid");
            janet_vm.registry_count = count;
            janet_vm.registry_cap = count;
            janet_vm.registry = janet_malloc(count * sizeof(JanetCFunRegistry));
            if (janet_vm.registry == NULL) {
                JANET_OUT_OF_MEMORY;
            }
            janet_vm.registry_dirty = 1;
            nextbytes += sizeof(uint32_t);
            memcpy(janet_vm.registry, nextbytes, count * sizeof(JanetCFunRegistry));
            nextbytes += count * sizeof(JanetCFunRegistry);
        }

        Janet fiberv = janet_unmarshal(nextbytes, endbytes - nextbytes,
                                       JANET_MARSHAL_UNSAFE, NULL, &nextbytes);
        Janet value = janet_unmarshal(nextbytes, endbytes - nextbytes,
                                      JANET_MARSHAL_UNSAFE, NULL, &nextbytes);
        JanetFiber *fiber;
        if (!janet_checktype(fiberv, JANET_FIBER)) {
            janet_assert(janet_checktype(fiberv, JANET_FUNCTION), "expected function or fiber");
            JanetFunction *func = janet_unwrap_function(fiberv);
            /* TODO - normal panics here do not seem to work correctly on Wine + Mingw. This needs to be investigated, and while it appears to be related to longjmp behavior
             * on the platform not working correctly, it is not obvious the issue is. That said, we probably should assert and hard-exit anyway if there is an issue there. */
            janet_assert(func->def->min_arity >= 0 && func->def->min_arity <= 1, "thread function must accept 0 or 1 arguments");
            fiber = janet_fiber(func, 64, func->def->min_arity, &value);
            janet_assert(fiber != NULL, "bad fiber in thread setup");
            fiber->flags |=
                JANET_FIBER_MASK_ERROR |
                JANET_FIBER_MASK_USER0 |
                JANET_FIBER_MASK_USER1 |
                JANET_FIBER_MASK_USER2 |
                JANET_FIBER_MASK_USER3 |
                JANET_FIBER_MASK_USER4;
        } else {
            fiber = janet_unwrap_fiber(fiberv);
        }
        if (flags & 0x8) {
            if (NULL == fiber->env) fiber->env = janet_table(0);
            janet_table_put(fiber->env, janet_ckeywordv("task-id"), value);
        }
        fiber->supervisor_channel = janet_vm.user;
        janet_schedule(fiber, value);
        janet_loop();
        args.tag = JANET_EV_TCTAG_NIL;
        janet_restore(&tstate);
    } else {
        janet_restore(&tstate);
        void *supervisor = janet_vm.user;
        if (NULL != supervisor) {
            /* Got a supervisor, write error there */
            Janet pair[] = {
                janet_ckeywordv("error"),
                tstate.payload
            };
            janet_channel_push((JanetChannel *)supervisor,
                               janet_wrap_tuple(janet_tuple_n(pair, 2)), 2);
        } else if (flags & 0x1) {
            /* No wait, just print to stderr */
            janet_eprintf("thread start failure: %v\n", tstate.payload);
        } else {
            /* Make ev/thread call from parent thread error */
            if (janet_checktype(tstate.payload, JANET_STRING)) {
                args.tag = JANET_EV_TCTAG_ERR_STRINGF;
                JanetString msg = janet_unwrap_string(tstate.payload);
                args.argp = janet_malloc(janet_string_length(msg) + 1);
                memcpy(args.argp, msg, janet_string_length(msg) + 1);
            } else {
                args.tag = JANET_EV_TCTAG_ERR_STRING;
                args.argp = "failed to start thread";
            }
        }
    }
    janet_buffer_deinit(buffer);
    janet_free(buffer);
    if (!(flags & JANET_THREAD_NODEINIT_FLAG)) {
        janet_deinit();
    }
    return args;
}

JANET_CORE_FN(cfun_ev_thread,
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
    janet_sandbox_assert(JANET_SANDBOX_THREADS);
    janet_arity(argc, 1, 4);
    Janet value = argc >= 2 ? argv[1] : janet_wrap_nil();
    if (janet_checktype(argv[0], JANET_FUNCTION)) {
        JanetFunction *func = janet_getfunction(argv, 0);
        if (func->def->arity < 0 || func->def->min_arity > 1) {
            janet_panic("function must take 0 or 1 arguments");
        }
    } else {
        janet_getfiber(argv, 0); /* arg check for fiber */
    }
    uint64_t flags = 0;
    if (argc >= 3) {
        flags = janet_getflags(argv, 2, "nact");
    }
    void *supervisor = janet_optabstract(argv, argc, 3, &janet_channel_type, janet_vm.root_fiber->supervisor_channel);
    if (NULL != supervisor) flags |= JANET_THREAD_SUPERVISOR_FLAG;

    /* Marshal arguments for the new thread. */
    JanetBuffer *buffer = janet_malloc(sizeof(JanetBuffer));
    if (NULL == buffer) {
        JANET_OUT_OF_MEMORY;
    }
    janet_buffer_init(buffer, 0);
    if (!(flags & 0x2)) {
        janet_marshal(buffer, janet_wrap_table(janet_vm.abstract_registry), NULL, JANET_MARSHAL_UNSAFE);
    }
    if (flags & JANET_THREAD_SUPERVISOR_FLAG) {
        janet_marshal(buffer, janet_wrap_abstract(supervisor), NULL, JANET_MARSHAL_UNSAFE);
    }
    if (!(flags & 0x4)) {
        janet_assert(janet_vm.registry_count <= INT32_MAX, "assert failed size check");
        uint32_t temp = (uint32_t) janet_vm.registry_count;
        janet_buffer_push_bytes(buffer, (uint8_t *) &temp, sizeof(temp));
        janet_buffer_push_bytes(buffer, (uint8_t *) janet_vm.registry, (int32_t) janet_vm.registry_count * sizeof(JanetCFunRegistry));
    }
    janet_marshal(buffer, argv[0], NULL, JANET_MARSHAL_UNSAFE);
    janet_marshal(buffer, value, NULL, JANET_MARSHAL_UNSAFE);
    if (flags & 0x1) {
        /* Return immediately */
        JanetEVGenericMessage arguments;
        memset(&arguments, 0, sizeof(arguments));
        arguments.tag = (uint32_t) flags;
        arguments.argi = (uint32_t) janet_vm.sandbox_flags;
        arguments.argp = buffer;
        arguments.fiber = NULL;
        janet_ev_threaded_call(janet_go_thread_subr, arguments, janet_ev_default_threaded_callback);
        return janet_wrap_nil();
    } else {
        janet_ev_threaded_await(janet_go_thread_subr, (uint32_t) flags, (uint32_t) janet_vm.sandbox_flags, buffer);
    }
}

/* Phase 3 slice: value-returning thread/new + thread/join.
 * Share-nothing: args marshalled in, result marshalled back.
 * Detached worker + pipe completion via janet_ev_post_event.
 * Handle state only mutated on owner thread; join is owner-checked. */

typedef struct {
    JanetVM *owner;
    int done;
    int joined;
    int is_error;
    int is_daemon;   /* daemon threads never pin process exit (no ev
                      * refcount); joining still works if they complete */
    Janet result;
    Janet profile_rows;   /* Phase C: harvested worker profile rows (or nil) */
    JanetFiber *waiter;
    /* sched_id of waiter when it suspended: every schedule bumps it, so
     * a waiter whose id moved on was already resumed elsewhere (timeout
     * or cancel) and must not be delivered to. Distinguishes a live
     * previous joiner from a concurrent one. */
    uint32_t waiter_sched;
    /* M2 monitors: flat [chan-wrap tag] pairs, chan of each pair is a
     * channel abstract wrap, tag is an arbitrary value. Both entries are
     * gcrooted on register and unrooted on delivery (or handle free).
     * Only touched on the owner thread. */
    Janet *monitors;
    int32_t n_monitors;
    int32_t cap_monitors;
} JanetThreadHandle;

typedef struct {
    JanetThreadHandle *handle;
    uint8_t *bytes;
    size_t len;
    int is_error;
    uint8_t *pbytes;   /* Phase C: harvested profile rows (or NULL) */
    size_t plen;
} JanetThreadCompletion;

typedef struct {
    JanetBuffer *input;
    JanetThreadHandle *handle;
    JanetVM *owner;
    uint32_t flags;
    uint32_t sandbox_flags;
    int profile_active;  /* Phase C: boot with the collector on */
} JanetThreadNewArgs;

static int janet_thread_handle_gc(void *p, size_t len) {
    (void) len;
    JanetThreadHandle *h = (JanetThreadHandle *) p;
    /* Drop undelivered monitors (completion already ran if done, so
     * this only fires when the handle is collected first). Tombstoned
     * slots hold nil and were unrooted by ev/demonitor. */
    if (h->monitors != NULL) {
        for (int32_t i = 0; i < h->n_monitors * 2; i++) {
            if (!janet_checktype(h->monitors[i], JANET_NIL)) {
                janet_gcunroot(h->monitors[i]);
            }
        }
        janet_free(h->monitors);
        h->monitors = NULL;
        h->n_monitors = 0;
        h->cap_monitors = 0;
    }
    return 0;
}

static int janet_thread_handle_mark(void *p, size_t len) {
    (void) len;
    JanetThreadHandle *h = (JanetThreadHandle *) p;
    janet_mark(h->result);
    janet_mark(h->profile_rows);
    if (h->waiter) {
        janet_mark(janet_wrap_fiber(h->waiter));
    }
    return 0;
}

const JanetAbstractType janet_thread_handle_type = {
    "core/thread-handle",
    janet_thread_handle_gc,
    janet_thread_handle_mark,
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
static Janet janet_thread_down_message(Janet tag, int is_error, Janet result) {
    Janet tup[3];
    tup[0] = janet_ckeywordv("down");
    tup[1] = tag;
    tup[2] = is_error ? result : janet_ckeywordv("normal");
    return janet_wrap_tuple(janet_tuple_n(tup, 3));
}

/* Best-effort delivery of one :down message (at-most-once: dropped
 * when the channel is closed or full). */
static void janet_thread_give_down(Janet chanv, Janet msg) {
    JanetChannel *chan = (JanetChannel *) janet_unwrap_abstract(chanv);
    if (chan->closed) return;
    janet_channel_give(chan, msg);
}

/* M2: deliver pending monitors after completion. Runs on the owner
 * event loop; each entry is unrooted and the list freed. Takes the
 * completion result explicitly: the waiter path below clears
 * h->result once a live joiner is resumed. */
static void janet_thread_deliver_monitors(JanetThreadHandle *h, int is_error, Janet result) {
    if (h->monitors == NULL) return;
    for (int32_t i = 0; i < h->n_monitors; i++) {
        Janet chanv = h->monitors[i * 2];
        Janet tag = h->monitors[i * 2 + 1];
        /* Tombstoned by ev/demonitor: already unrooted, skip. */
        if (janet_checktype(chanv, JANET_NIL)) continue;
        janet_thread_give_down(chanv,
            janet_thread_down_message(tag, is_error, result));
        janet_gcunroot(chanv);
        janet_gcunroot(tag);
    }
    janet_free(h->monitors);
    h->monitors = NULL;
    h->n_monitors = 0;
    h->cap_monitors = 0;
}

static void janet_thread_new_complete(JanetEVGenericMessage msg) {
    JanetThreadCompletion *c = (JanetThreadCompletion *) msg.argp;
    JanetThreadHandle *h = c->handle;
    Janet result = janet_wrap_nil();
    if (c->bytes != NULL && c->len > 0) {
        result = janet_unmarshal(c->bytes, c->len, JANET_MARSHAL_UNSAFE, NULL, NULL);
        janet_free(c->bytes);
    } else if (c->bytes != NULL) {
        janet_free(c->bytes);
    }
    /* Phase C: harvested worker profile rows (nil when it profiled
     * nothing). Stored on the handle for thread/profile. */
    if (c->pbytes != NULL && c->plen > 0) {
        h->profile_rows = janet_unmarshal(c->pbytes, c->plen, JANET_MARSHAL_UNSAFE, NULL, NULL);
        janet_free(c->pbytes);
    } else if (c->pbytes != NULL) {
        janet_free(c->pbytes);
    }
    int is_error = c->is_error;
    janet_free(c);
    h->result = result;
    h->is_error = is_error;
    h->done = 1;
    /* A suspended join roots (handle, waiter) as a pair; unroot exactly.
     * Deliver only if the waiter is still suspended in our await
     * (schedule bumps sched_id, so a moved id means timeout/cancel
     * already resumed it elsewhere — keep the result for a later join
     * instead of scheduling a rogue resume). */
    if (h->waiter != NULL) {
        JanetFiber *w = h->waiter;
        uint32_t ws = h->waiter_sched;
        h->waiter = NULL;
        janet_gcunroot(janet_wrap_fiber(w));
        janet_gcunroot(janet_wrap_abstract(h));
        if (w->sched_id == ws) {
            h->joined = 1;
            h->result = janet_wrap_nil();
            if (h->is_error) {
                janet_cancel(w, result);
            } else {
                janet_schedule(w, result);
            }
        }
    }
    /* M2: monitors fire after join delivery so the join path is
     * unaffected; :down messages are best-effort (at-most-once). */
    janet_thread_deliver_monitors(h, is_error, result);
    if (!h->is_daemon) janet_ev_dec_refcount();
    /* Release worker ref last; may free handle (with result) if owner
     * dropped it. Do not touch h after this. */
    janet_abstract_decref_maybe_free(h);
}

/* Build a completion holding a malloc'd copy of marshalled bytes. */
static JanetThreadCompletion *janet_thread_make_completion(
    JanetThreadHandle *handle, uint8_t *bytes, size_t len, int is_error) {
    JanetThreadCompletion *c = janet_malloc(sizeof(JanetThreadCompletion));
    if (NULL == c) {
        JANET_OUT_OF_MEMORY;
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
static void janet_thread_marshal_result_copy(Janet v, uint8_t **out_bytes, size_t *out_len) {
    JanetBuffer tmp;
    janet_buffer_init(&tmp, 0);
    janet_marshal(&tmp, v, NULL, JANET_MARSHAL_UNSAFE);
    uint8_t *copy = NULL;
    if (tmp.count > 0) {
        copy = janet_malloc((size_t) tmp.count);
        if (NULL == copy) {
            janet_buffer_deinit(&tmp);
            JANET_OUT_OF_MEMORY;
        }
        memcpy(copy, tmp.data, (size_t) tmp.count);
    }
    *out_bytes = copy;
    *out_len = (size_t) tmp.count;
    janet_buffer_deinit(&tmp);
}

static JanetThreadCompletion *janet_thread_new_run(JanetThreadNewArgs *targs) {
    JanetBuffer *buffer = targs->input;
    const uint8_t *nextbytes = buffer->data;
    const uint8_t *endbytes = nextbytes + buffer->count;
    uint32_t flags = targs->flags;
    JanetThreadHandle *handle = targs->handle;
    janet_init();
    janet_vm.sandbox_flags = targs->sandbox_flags;
    /* Phase C: inherit the collector when the owner was profiling. */
    if (targs->profile_active) janet_profile_start();
    JanetThreadCompletion *comp = NULL;
    JanetTryState tstate;
    JanetSignal signal = janet_try(&tstate);
    if (!signal) {
        /* Flag bits match the owner side (thread/new "acd"): :a skips
         * the abstract registry, :c skips the cfunction registry. */
        if (!(flags & 0x1)) {
            Janet aregv = janet_unmarshal(nextbytes, endbytes - nextbytes,
                JANET_MARSHAL_UNSAFE, NULL, &nextbytes);
            janet_assert(janet_checktype(aregv, JANET_TABLE), "expected table for abstract registry");
            janet_vm.abstract_registry = janet_unwrap_table(aregv);
            janet_gcroot(janet_wrap_table(janet_vm.abstract_registry));
        }
        if (!(flags & 0x2)) {
            uint32_t count1;
            memcpy(&count1, nextbytes, sizeof(count1));
            size_t count = (size_t) count1;
            janet_assert(count <= (size_t)(endbytes - nextbytes - sizeof(count1)) / sizeof(JanetCFunRegistry),
                "thread message invalid");
            janet_vm.registry_count = count;
            janet_vm.registry_cap = count;
            janet_vm.registry = janet_malloc(count * sizeof(JanetCFunRegistry));
            if (janet_vm.registry == NULL) {
                JANET_OUT_OF_MEMORY;
            }
            janet_vm.registry_dirty = 1;
            nextbytes += sizeof(uint32_t);
            memcpy(janet_vm.registry, nextbytes, count * sizeof(JanetCFunRegistry));
            nextbytes += count * sizeof(JanetCFunRegistry);
        }
        Janet fiberv = janet_unmarshal(nextbytes, endbytes - nextbytes,
            JANET_MARSHAL_UNSAFE, NULL, &nextbytes);
        Janet value = janet_unmarshal(nextbytes, endbytes - nextbytes,
            JANET_MARSHAL_UNSAFE, NULL, &nextbytes);
        /* Input consumed; free before running (malloc only, no VM). */
        janet_buffer_deinit(buffer);
        janet_free(buffer);
        janet_free(targs);
        targs = NULL;
        buffer = NULL;
        JanetFiber *fiber;
        if (!janet_checktype(fiberv, JANET_FIBER)) {
            janet_assert(janet_checktype(fiberv, JANET_FUNCTION), "expected function or fiber");
            JanetFunction *func = janet_unwrap_function(fiberv);
            janet_assert(func->def->min_arity >= 0 && func->def->min_arity <= 1,
                "thread function must accept 0 or 1 arguments");
            fiber = janet_fiber(func, 64, func->def->min_arity, &value);
            janet_assert(fiber != NULL, "bad fiber in thread setup");
            /* Phase C: primed fibers bypass JOP_CALL; record the root
             * invocation so worker rows carry calls/params. */
            janet_profile_task_call(func, &value, 1);
            fiber->flags |=
                JANET_FIBER_MASK_ERROR |
                JANET_FIBER_MASK_USER0 |
                JANET_FIBER_MASK_USER1 |
                JANET_FIBER_MASK_USER2 |
                JANET_FIBER_MASK_USER3 |
                JANET_FIBER_MASK_USER4;
        } else {
            fiber = janet_unwrap_fiber(fiberv);
        }
        fiber->supervisor_channel = NULL;
        janet_schedule(fiber, value);
        janet_loop();
        JanetFiberStatus status = janet_fiber_status(fiber);
        Janet result = fiber->last_value;
        int is_error = 0;
        if (status == JANET_STATUS_ERROR) {
            is_error = 1;
        } else if (status != JANET_STATUS_DEAD) {
            result = janet_cstringv("thread did not complete (yielded)");
            is_error = 1;
        }
        /* Root fiber (hence last_value) across marshalling allocations. */
        Janet fiber_j = janet_wrap_fiber(fiber);
        Janet result_j = result;
        janet_gcroot(fiber_j);
        janet_gcroot(result_j);
        uint8_t *bytes = NULL;
        size_t len = 0;
        janet_thread_marshal_result_copy(result, &bytes, &len);
        janet_gcunroot(fiber_j);
        janet_gcunroot(result_j);
        comp = janet_thread_make_completion(handle, bytes, len, is_error);
        /* Phase C: harvest this worker's profile table (best-effort;
         * empty when profiling was off). The VM is still alive here. */
        janet_profile_harvest_bytes(&comp->pbytes, &comp->plen);
        janet_restore(&tstate);
    } else {
        janet_restore(&tstate);
        /* Startup or marshal failure: report payload if marshallable,
         * else a static fallback. */
        Janet payload = tstate.payload;
        uint8_t *bytes = NULL;
        size_t len = 0;
        Janet payload_j = payload;
        janet_gcroot(payload_j);
        JanetTryState tstate2;
        JanetSignal sig2 = janet_try(&tstate2);
        if (!sig2) {
            janet_thread_marshal_result_copy(payload, &bytes, &len);
            janet_restore(&tstate2);
        } else {
            janet_restore(&tstate2);
            Janet fallback = janet_cstringv("thread failed (unmarshallable error)");
            Janet fallback_j = fallback;
            janet_gcroot(fallback_j);
            janet_thread_marshal_result_copy(fallback, &bytes, &len);
            janet_gcunroot(fallback_j);
        }
        janet_gcunroot(payload_j);
        if (buffer != NULL) {
            janet_buffer_deinit(buffer);
            janet_free(buffer);
        }
        if (targs != NULL) {
            janet_free(targs);
        }
        comp = janet_thread_make_completion(handle, bytes, len, 1);
    }
    janet_deinit();
    return comp;
}

#ifdef JANET_WINDOWS
static DWORD WINAPI janet_thread_new_worker(LPVOID ptr) {
    JanetThreadNewArgs *targs = (JanetThreadNewArgs *) ptr;
    JanetVM *owner = targs->owner;
    JanetThreadCompletion *comp = janet_thread_new_run(targs);
    JanetEVGenericMessage msg;
    memset(&msg, 0, sizeof(msg));
    msg.argp = comp;
    janet_ev_post_event(owner, janet_thread_new_complete, msg);
    return 0;
}
#else
static void *janet_thread_new_worker(void *ptr) {
    JanetThreadNewArgs *targs = (JanetThreadNewArgs *) ptr;
    JanetVM *owner = targs->owner;
    JanetThreadCompletion *comp = janet_thread_new_run(targs);
    JanetEVGenericMessage msg;
    memset(&msg, 0, sizeof(msg));
    msg.argp = comp;
    janet_ev_post_event(owner, janet_thread_new_complete, msg);
    return NULL;
}
#endif

JANET_CORE_FN(cfun_thread_new,
              "(thread/new main &opt value flags)",
              "Run `main` in a new operating system thread and return a handle. "
              "`main` is a function taking 0 or 1 arguments, or a fiber; `value` is passed as the argument. "
              "The thread runs an isolated VM (share-nothing: args marshalled in, result marshalled back). "
              "Use `(thread/join handle)` to wait for completion and get the return value (re-raises thread errors). "
              "Flags: `:a` don't copy abstract registry, `:c` don't copy cfunction registry. "
              "`:d` daemon: the thread never pins process exit (no ev refcount); "
              "joining still works if it completes. Long-lived service threads "
              "(pools) should pass `:d`, or the event loop waits for them forever.") {
    janet_sandbox_assert(JANET_SANDBOX_THREADS);
    janet_arity(argc, 1, 3);
    Janet value = argc >= 2 ? argv[1] : janet_wrap_nil();
    if (janet_checktype(argv[0], JANET_FUNCTION)) {
        JanetFunction *func = janet_getfunction(argv, 0);
        if (func->def->arity < 0 || func->def->min_arity > 1) {
            janet_panic("function must take 0 or 1 arguments");
        }
    } else {
        janet_getfiber(argv, 0);
    }
    uint64_t flags = 0;
    if (argc >= 3) {
        flags = janet_getflags(argv, 2, "acd");
    }
    JanetBuffer *buffer = janet_malloc(sizeof(JanetBuffer));
    if (NULL == buffer) {
        JANET_OUT_OF_MEMORY;
    }
    janet_buffer_init(buffer, 0);
    /* Flag bits follow janet_getflags order ("acd"): :a skips the
     * abstract registry, :c skips the cfunction registry, :d daemonizes
     * (no exit refcount — see below). */
    if (!(flags & 0x1)) {
        janet_marshal(buffer, janet_wrap_table(janet_vm.abstract_registry), NULL, JANET_MARSHAL_UNSAFE);
    }
    if (!(flags & 0x2)) {
        janet_assert(janet_vm.registry_count <= INT32_MAX, "assert failed size check");
        uint32_t temp = (uint32_t) janet_vm.registry_count;
        janet_buffer_push_bytes(buffer, (uint8_t *) &temp, sizeof(temp));
        janet_buffer_push_bytes(buffer, (uint8_t *) janet_vm.registry,
            (int32_t) janet_vm.registry_count * sizeof(JanetCFunRegistry));
    }
    janet_marshal(buffer, argv[0], NULL, JANET_MARSHAL_UNSAFE);
    janet_marshal(buffer, value, NULL, JANET_MARSHAL_UNSAFE);
    JanetThreadHandle *handle = janet_abstract_threaded(&janet_thread_handle_type, sizeof(JanetThreadHandle));
    handle->owner = &janet_vm;
    handle->done = 0;
    handle->joined = 0;
    handle->is_error = 0;
    handle->is_daemon = (flags & 0x4) ? 1 : 0;
    handle->result = janet_wrap_nil();
    handle->profile_rows = janet_wrap_nil();
    handle->waiter = NULL;
    handle->waiter_sched = 0;
    handle->monitors = NULL;
    handle->n_monitors = 0;
    handle->cap_monitors = 0;
    JanetThreadNewArgs *targs = janet_malloc(sizeof(JanetThreadNewArgs));
    if (NULL == targs) {
        janet_buffer_deinit(buffer);
        janet_free(buffer);
        janet_panic("failed to allocate thread args");
    }
    targs->input = buffer;
    targs->handle = handle;
    targs->owner = &janet_vm;
    targs->flags = (uint32_t) flags;
    targs->sandbox_flags = janet_vm.sandbox_flags;
    /* Phase C: the worker inherits the collector when profiling. */
    targs->profile_active = (janet_vm.profile_table != NULL);
    janet_abstract_incref(handle);
    /* Daemon threads never pin process exit (long-lived service
     * threads would hold the event loop forever); joining still
     * works if they complete (the completion path skips its dec). */
    if (!handle->is_daemon) {
        janet_ev_inc_refcount();
    }
#ifdef JANET_WINDOWS
    HANDLE wh = CreateThread(NULL, 0, janet_thread_new_worker, targs, 0, NULL);
    if (NULL == wh) {
        janet_abstract_decref_maybe_free(handle);
        if (!handle->is_daemon) janet_ev_dec_refcount();
        janet_buffer_deinit(buffer);
        janet_free(buffer);
        janet_free(targs);
        janet_panic("failed to create thread");
    }
    CloseHandle(wh);
#else
    pthread_t worker;
    int err = pthread_create(&worker, &janet_vm.new_thread_attr, janet_thread_new_worker, targs);
    if (err) {
        janet_abstract_decref_maybe_free(handle);
        if (!handle->is_daemon) janet_ev_dec_refcount();
        janet_buffer_deinit(buffer);
        janet_free(buffer);
        janet_free(targs);
        janet_panicf("%s", janet_strerror(err));
    }
#endif
    return janet_wrap_abstract(handle);
}

JANET_CORE_FN(cfun_thread_join,
              "(thread/join handle &opt timeout)",
              "Wait for the thread `handle` created by `thread/new` to complete and return its value. "
              "Re-raises the thread's error if it failed. Must be called from the creating thread, exactly once. "
              "With `timeout` (seconds), raise a timeout error instead of waiting forever; a later join can "
              "still collect the result. Timeouts require joining from an ev task.") {
    janet_arity(argc, 1, 2);
    JanetThreadHandle *handle = janet_getabstract(argv, 0, &janet_thread_handle_type);
    if (handle->owner != &janet_vm) {
        janet_panic("thread/join called from non-owner thread");
    }
    if (handle->joined) {
        janet_panic("thread already joined");
    }
    if (janet_vm.coerce_error) {
        janet_panic("cannot join inside janet_call");
    }
    double timeout = janet_optnumber(argv, argc, 1, INFINITY);
    if (handle->done) {
        /* Fast path: completion already ran. */
        handle->joined = 1;
        Janet result = handle->result;
        handle->result = janet_wrap_nil();
        if (handle->is_error) {
            janet_panicv(result);
        }
        return result;
    }
    JanetFiber *root = janet_root_fiber();
    if (handle->waiter != NULL) {
        /* A waiter whose sched_id moved on was already resumed elsewhere
         * (timeout or cancel): release its roots exactly and take over.
         * An unmoved waiter is a genuine concurrent join. */
        if (handle->waiter->sched_id == handle->waiter_sched) {
            janet_panic("thread already being joined");
        }
        janet_gcunroot(janet_wrap_fiber(handle->waiter));
        janet_gcunroot(janet_wrap_abstract(handle));
        handle->waiter = NULL;
    }
    if (timeout != INFINITY &&
            !(root->gc.flags & JANET_FIBER_EV_GCFLAG_ROOT)) {
        janet_panic("thread/join with timeout requires joining from an ev task");
    }
    handle->waiter = root;
    handle->waiter_sched = root->sched_id;
    /* Root across suspension; completion callback unroots
     * (mirrors janet_ev_threaded_await's fiber rooting). */
    janet_gcroot(janet_wrap_fiber(root));
    janet_gcroot(janet_wrap_abstract(handle));
    if (timeout != INFINITY) {
        janet_addtimeout(timeout);
    }
    janet_await();
    /* Unreachable when suspending; the completion schedules this fiber
     * with the thread's result (or cancels with its error). */
    return janet_wrap_nil();
}

JANET_CORE_FN(cfun_thread_alive,
              "(thread/alive? handle)",
              "Return whether the thread `handle` created by `thread/new` is "
              "still running. Once true it stays true until `thread/join`; "
              "after `thread/join` returns false. Must be called from the "
              "creating thread.") {
    janet_fixarity(argc, 1);
    JanetThreadHandle *handle = janet_getabstract(argv, 0, &janet_thread_handle_type);
    if (handle->owner != &janet_vm) {
        janet_panic("thread/alive? called from non-owner thread");
    }
    return janet_wrap_boolean(!handle->done);
}

JANET_CORE_FN(cfun_thread_profile,
              "(thread/profile handle)",
              "Return the worker's harvested profile rows (Phase C dump "
              "format) after the thread completed, or nil when it profiled "
              "nothing. Workers inherit the collector when the owner is "
              "profiling at thread/new time. Must be called from the "
              "creating thread.") {
    janet_fixarity(argc, 1);
    JanetThreadHandle *handle = janet_getabstract(argv, 0, &janet_thread_handle_type);
    if (handle->owner != &janet_vm) {
        janet_panic("thread/profile called from non-owner thread");
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
 * runs under a per-task janet_loop (so ev/sleep works inside tasks),
 * but a task that suspends forever wedges its worker.
 *
 * Suspension discipline: every blocking cfun below has at most ONE
 * trailing suspension (the proven take/join shape — code after
 * janet_await never runs). Capacity tokens are therefore acquired in a
 * separate step (pool/acquire, sequenced from Janet): the token take is
 * its suspension, the result wait is dispatch's.
 *
 * Lifetime: the pool abstract is gcrooted at spawn and unrooted at
 * stop, so the struct always outlives its workers (workers hold no
 * pool refs). pool/stop joins every worker, then frees the internals;
 * the type finalizer only handles the abandoned case (sets stopping +
 * broadcasts; the last exiting worker frees the contents). Abandoning
 * a pool without pool/stop leaks its threads until process exit —
 * documented, same class as leaked threads. */

typedef struct JanetPoolTask {
    uint8_t *bytes;          /* UNSAFE-marshalled [func args] tuple, malloc'd */
    size_t len;
    JanetVM *owner;          /* completion target */
    JanetFiber *waiter;      /* pool/call path (direct completion), else NULL */
    uint32_t waiter_sched;
    void *reply;             /* pool/submit path: incref'd threaded channel, else NULL */
    struct JanetPoolTask *next;
} JanetPoolTask;

/* Phase C: per-worker profile harvest. Workers with profiling active
 * serialize their tables at exit into UNSAFE-marshal buffers; the
 * owner drains them via pool/profile-harvest (merged Janet-side by
 * tools/profile-merge.janet). Malloc'd, pool-locked, freed on drain
 * or with the pool. */
typedef struct JanetProfileHarvest {
    uint8_t *bytes;
    size_t len;
    struct JanetProfileHarvest *next;
} JanetProfileHarvest;

typedef struct {
#ifdef JANET_WINDOWS
    CRITICAL_SECTION lock;
    CONDITION_VARIABLE nonempty;
#else
    pthread_mutex_t lock;
    pthread_cond_t nonempty;
#endif
    JanetPoolTask *head, *tail;
    int32_t count;
    int stopping;            /* drain, then exit (stop or finalizer) */
    int joined;              /* stop() completed: workers joined, contents freed */
    int contents_freed;
    int32_t n_workers;
    int32_t live_workers;
    JanetVM *owner;
    uint32_t flags;
    uint32_t sandbox_flags;
    uint8_t *prologue;       /* marshalled registries, immutable after spawn */
    size_t prologue_len;
    JanetChannel *tokens;    /* capacity tokens (pool-incref'd, never rooted) */
    JanetFiber *stop_waiter; /* pool/stop waiter (single), else NULL */
    uint32_t stop_sched;
    Janet self;              /* gcrooted wrapper: pins the struct until stop */
    int profile_active;      /* Phase C: workers boot with the collector on */
    JanetProfileHarvest *harvest_head, *harvest_tail;
#ifdef JANET_WINDOWS
    HANDLE *threads;
#else
    pthread_t *threads;
#endif
} JanetPool;

static void janet_pool_lock(JanetPool *pool) {
#ifdef JANET_WINDOWS
    EnterCriticalSection(&pool->lock);
#else
    pthread_mutex_lock(&pool->lock);
#endif
}

static void janet_pool_unlock(JanetPool *pool) {
#ifdef JANET_WINDOWS
    LeaveCriticalSection(&pool->lock);
#else
    pthread_mutex_unlock(&pool->lock);
#endif
}

static void janet_pool_broadcast(JanetPool *pool) {
#ifdef JANET_WINDOWS
    WakeAllConditionVariable(&pool->nonempty);
#else
    pthread_cond_broadcast(&pool->nonempty);
#endif
}

static void janet_pool_signal(JanetPool *pool) {
#ifdef JANET_WINDOWS
    WakeConditionVariable(&pool->nonempty);
#else
    pthread_cond_signal(&pool->nonempty);
#endif
}

/* Owner-loop callback: the last worker exited. Join (all already dead,
 * so immediate), free the contents, wake the pool/stop waiter. Runs on
 * the owner event loop — the only place teardown happens. */
static void janet_pool_all_exited(JanetEVGenericMessage msg);

static void janet_pool_sync_destroy(JanetPool *pool) {
#ifdef JANET_WINDOWS
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
static void janet_pool_free_contents(JanetPool *pool) {
    JanetPoolTask *t = pool->head;
    while (t != NULL) {
        JanetPoolTask *next = t->next;
        /* Abandon-path remnants: tasks the drain never ran. Their reply
         * channels keep them safe (worker incref pairs with this decref);
         * direct-completion waiters are gone with the owner flow. */
        if (t->reply != NULL) janet_abstract_decref_maybe_free(t->reply);
        if (t->bytes != NULL) janet_free(t->bytes);
        janet_free(t);
        t = next;
    }
    pool->head = pool->tail = NULL;
    if (pool->prologue != NULL) {
        janet_free(pool->prologue);
        pool->prologue = NULL;
    }
    if (pool->threads != NULL) {
        /* Thread handles are joined (stop) or abandoned with the leak
         * (documented): never closed here. Windows HANDLEs are closed
         * by pool/stop after joining. */
        janet_free(pool->threads);
        pool->threads = NULL;
    }
    /* Drop the pool's own token ref; the wrapper-owned ref dies with
     * the tokens wrapper. */
    if (pool->tokens != NULL) {
        janet_abstract_decref_maybe_free(pool->tokens);
        pool->tokens = NULL;
    }
    /* Phase C: unread harvest remnants (readers drain via
     * pool/profile-harvest; leftovers die with the pool). */
    {
        JanetProfileHarvest *h = pool->harvest_head;
        while (h != NULL) {
            JanetProfileHarvest *next = h->next;
            if (h->bytes != NULL) janet_free(h->bytes);
            janet_free(h);
            h = next;
        }
        pool->harvest_head = pool->harvest_tail = NULL;
    }
    pool->contents_freed = 1;
}

static int janet_pool_gc(void *p, size_t len) {
    (void) len;
    JanetPool *pool = (JanetPool *) p;
    /* The wrapper is unreachable: no Janet code can name this pool
     * anymore. Joined pools (and spawn-failure leftovers, already
     * joined on the panic path) have no threads: free without the
     * lock held — destroying a locked mutex is UB, and nothing else
     * can run here. The live case (abandonment) never unroots, so it
     * cannot reach here; if it ever does, only ask for a drain and
     * leave the documented leak rather than freeing under workers. */
    janet_pool_lock(pool);
    if (!pool->joined && pool->live_workers != 0) {
        pool->stopping = 1;
        janet_pool_broadcast(pool);
        janet_pool_unlock(pool);
        return 0;
    }
    int do_free = !pool->contents_freed;
    janet_pool_unlock(pool);
    if (do_free) {
        janet_pool_free_contents(pool);
        janet_pool_sync_destroy(pool);
    }
    return 0;
}

static int janet_pool_mark(void *p, size_t len) {
    (void) len;
    (void) p;
    /* Tasks reference no owner-VM heap objects (all payloads are
     * malloc'd marshal buffers); waiters/replies are rooted per-call. */
    return 0;
}

const JanetAbstractType janet_pool_type = {
    "core/pool",
    janet_pool_gc,
    janet_pool_mark,
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
static void janet_pool_give_reply(JanetChannel *chan, Janet msg) {
    if (chan->closed) return;
    janet_channel_push(chan, msg, 2);
}

typedef struct {
    JanetFiber *waiter;      /* direct-completion waiter, else NULL */
    uint32_t waiter_sched;
    JanetChannel *reply;     /* reply channel (borrowed ref), else NULL */
    uint8_t *bytes;
    size_t len;
    int is_error;
} JanetPoolCompletion;

static void janet_pool_complete(JanetEVGenericMessage msg) {
    JanetPoolCompletion *c = (JanetPoolCompletion *) msg.argp;
    Janet result = janet_wrap_nil();
    if (c->bytes != NULL && c->len > 0) {
        result = janet_unmarshal(c->bytes, c->len, JANET_MARSHAL_UNSAFE, NULL, NULL);
        janet_free(c->bytes);
    } else if (c->bytes != NULL) {
        janet_free(c->bytes);
    }
    /* Same exactly-once waiter discipline as thread/join: deliver only
     * if the waiter is still suspended in our await (a moved sched_id
     * means timeout/cancel already resumed it — drop the value). */
    if (c->waiter != NULL) {
        JanetFiber *w = c->waiter;
        uint32_t ws = c->waiter_sched;
        c->waiter = NULL;
        janet_gcunroot(janet_wrap_fiber(w));
        if (w->sched_id == ws) {
            if (c->is_error) {
                janet_cancel(w, result);
            } else {
                janet_schedule(w, result);
            }
        } else {
        }
    }
    if (c->reply != NULL) {
        /* Submit path: exactly one give into a limit-1 channel. The
         * task held a ref so the channel is alive here; drop ours. */
        janet_pool_give_reply(c->reply, c->is_error
            ? janet_wrap_tuple(janet_tuple_n((Janet[]) {
                janet_ckeywordv("pool-error"), result }, 2))
            : result);
        janet_abstract_decref_maybe_free(c->reply);
    }
    janet_free(c);
    janet_ev_dec_refcount();
}

/* Boot one worker VM from the shared prologue (registries), mirroring
 * janet_thread_new_run's registry section. Returns NULL with the task
 * fiber set up, or aborts on corrupt prologue (same fail-loud contract
 * as thread/new). */
typedef struct {
    JanetPool *pool;
    int index;
} JanetPoolWorkerArgs;

/* Phase C: harvest this VM's profile table (if active) into malloc'd
 * UNSAFE-marshal bytes of the dump row array. Returns 1 when harvested
 * (caller frees *out_bytes). Runs on the harvesting VM; the bytes cross
 * threads like any completion payload. */
static int janet_profile_harvest_bytes(uint8_t **out_bytes, size_t *out_len) {
    JanetArray *rows;
    Janet rows_j;
    JanetBuffer tmp;
    uint8_t *copy;
    if (NULL == janet_vm.profile_table) return 0;
    rows = janet_profile_rows();
    rows_j = janet_wrap_array(rows);
    janet_gcroot(rows_j);
    janet_buffer_init(&tmp, 0);
    janet_marshal(&tmp, rows_j, NULL, JANET_MARSHAL_UNSAFE);
    copy = NULL;
    if (tmp.count > 0) {
        /* Best-effort: teardown callers must never panic (a panic
         * with the pool lock held wedges stop), so OOM drops the
         * evidence instead of raising. */
        copy = janet_malloc((size_t) tmp.count);
        if (NULL != copy) {
            memcpy(copy, tmp.data, (size_t) tmp.count);
        } else {
            janet_buffer_deinit(&tmp);
            janet_gcunroot(rows_j);
            *out_bytes = NULL;
            *out_len = 0;
            return 0;
        }
    }
    *out_bytes = copy;
    *out_len = (size_t) tmp.count;
    janet_buffer_deinit(&tmp);
    janet_gcunroot(rows_j);
    return 1;
}

static void janet_pool_worker_boot(JanetPool *pool) {
    janet_init();
    janet_vm.sandbox_flags = pool->sandbox_flags;
    /* Phase C: inherit the collector when the owner was profiling at
     * spawn. Per-VM table; harvested at worker exit below. */
    if (pool->profile_active) janet_profile_start();
    const uint8_t *nextbytes = pool->prologue;
    const uint8_t *endbytes = nextbytes + pool->prologue_len;
    uint32_t flags = pool->flags;
    /* Same numbering as the owner side ("ac"): :a skips abstract, :c
     * skips cfunction (previously off by one on both sides). */
    if (!(flags & 0x1)) {
        Janet aregv = janet_unmarshal(nextbytes, endbytes - nextbytes,
            JANET_MARSHAL_UNSAFE, NULL, &nextbytes);
        janet_assert(janet_checktype(aregv, JANET_TABLE), "pool prologue: abstract registry");
        janet_vm.abstract_registry = janet_unwrap_table(aregv);
        janet_gcroot(janet_wrap_table(janet_vm.abstract_registry));
    }
    if (!(flags & 0x2)) {
        uint32_t count1;
        memcpy(&count1, nextbytes, sizeof(count1));
        size_t count = (size_t) count1;
        janet_assert(count <= (size_t)(endbytes - nextbytes - sizeof(count1)) / sizeof(JanetCFunRegistry),
            "pool prologue: registry bounds");
        janet_vm.registry_count = count;
        janet_vm.registry_cap = count;
        janet_vm.registry = janet_malloc(count * sizeof(JanetCFunRegistry));
        if (janet_vm.registry == NULL) {
            JANET_OUT_OF_MEMORY;
        }
        janet_vm.registry_dirty = 1;
        nextbytes += sizeof(uint32_t);
        memcpy(janet_vm.registry, nextbytes, count * sizeof(JanetCFunRegistry));
    }
}

/* Run one task payload ([func args] tuple) to completion in this worker
 * VM under a per-task janet_loop (so ev/sleep works inside tasks).
 * Returns the result value; sets *is_error on task failure. */
static Janet janet_pool_run_task(Janet taskv, int *is_error) {
    *is_error = 0;
    const Janet *pair = janet_unwrap_tuple(taskv);
    Janet funcv = pair[0];
    Janet argsv = pair[1];
    JanetFiber *fiber;
    if (janet_checktype(funcv, JANET_FIBER)) {
        /* Fibers resume with the args array as the single resume value
         * (same single-value shape as thread/new). */
        fiber = janet_unwrap_fiber(funcv);
        janet_schedule(fiber, argsv);
    } else {
        JanetFunction *func = janet_unwrap_function(funcv);
        const Janet *argdata = NULL;
        int32_t nargs = 0;
        if (janet_checktype(argsv, JANET_ARRAY)) {
            nargs = janet_unwrap_array(argsv)->count;
            argdata = janet_unwrap_array(argsv)->data;
        } else {
            nargs = janet_tuple_length(janet_unwrap_tuple(argsv));
            argdata = janet_unwrap_tuple(argsv);
        }
        fiber = janet_fiber(func, 64, nargs, argdata);
        janet_assert(fiber != NULL, "pool task: bad fiber");
        /* Phase C: primed fibers bypass JOP_CALL, so record the root
         * invocation explicitly (else rows show calls=0, no params). */
        janet_profile_task_call(func, argdata, nargs);
        janet_schedule(fiber, janet_wrap_nil());
    }
    fiber->supervisor_channel = NULL;
    janet_loop();
    JanetFiberStatus status = janet_fiber_status(fiber);
    if (status == JANET_STATUS_ERROR) {
        *is_error = 1;
        return fiber->last_value;
    }
    if (status != JANET_STATUS_DEAD) {
        *is_error = 1;
        return janet_cstringv("pool task did not complete (yielded)");
    }
    return fiber->last_value;
}

#ifdef JANET_WINDOWS
static DWORD WINAPI janet_pool_worker(LPVOID ptr) {
#else
static void *janet_pool_worker(void *ptr) {
#endif
    JanetPoolWorkerArgs *wargs = (JanetPoolWorkerArgs *) ptr;
    JanetPool *pool = wargs->pool;
    janet_free(wargs);
    janet_pool_worker_boot(pool);
    for (;;) {
        JanetPoolTask *task;
        janet_pool_lock(pool);
        while (pool->head == NULL && !pool->stopping) {
#ifdef JANET_WINDOWS
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
                if (janet_profile_harvest_bytes(&hbytes, &hlen)) {
                    JanetProfileHarvest *h = janet_malloc(sizeof(JanetProfileHarvest));
                    if (NULL == h) {
                        /* Best-effort (see above): drop, keep exiting. */
                        if (hbytes != NULL) janet_free(hbytes);
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
            janet_pool_unlock(pool);
            if (last) {
                JanetEVGenericMessage msg;
                memset(&msg, 0, sizeof(msg));
                msg.argp = pool;
                /* No refcount here (thread-local counters): pool/stop
                 * inc'd on the owner; the callback decs there. */
                janet_ev_post_event(pool->owner, janet_pool_all_exited, msg);
            }
            janet_deinit();
#ifdef JANET_WINDOWS
            return 0;
#else
            return NULL;
#endif
        }
        task = pool->head;
        pool->head = task->next;
        if (pool->head == NULL) pool->tail = NULL;
        pool->count--;
        janet_pool_unlock(pool);
        /* Run + complete. The capacity token is held for the task's
         * whole lifetime and handed back after completion posts: that
         * is what bounds memory (a wedged worker pins its capacity).
         * Total tokens are constant, so the give always fits. */
        Janet taskv = janet_unmarshal(task->bytes, task->len,
            JANET_MARSHAL_UNSAFE, NULL, NULL);
        janet_free(task->bytes);
        task->bytes = NULL;
        Janet taskv_j = taskv;
        janet_gcroot(taskv_j);
        int is_error = 0;
        JanetTryState tstate;
        Janet result = janet_wrap_nil();
        JanetSignal sig = janet_try(&tstate);
        if (!sig) {
            result = janet_pool_run_task(taskv, &is_error);
            janet_restore(&tstate);
        } else {
            janet_restore(&tstate);
            result = tstate.payload;
            is_error = 1;
        }
        Janet result_j = result;
        janet_gcroot(result_j);
        uint8_t *bytes = NULL;
        size_t len = 0;
        JanetTryState tstate2;
        JanetSignal sig2 = janet_try(&tstate2);
        if (!sig2) {
            janet_thread_marshal_result_copy(result, &bytes, &len);
            janet_restore(&tstate2);
        } else {
            janet_restore(&tstate2);
            Janet fallback = janet_cstringv("pool result failed to marshal");
            Janet fallback_j = fallback;
            janet_gcroot(fallback_j);
            janet_thread_marshal_result_copy(fallback, &bytes, &len);
            janet_gcunroot(fallback_j);
            is_error = 1;
        }
        janet_gcunroot(taskv_j);
        janet_gcunroot(result_j);
        JanetPoolCompletion *comp = janet_malloc(sizeof(JanetPoolCompletion));
        if (NULL == comp) {
            JANET_OUT_OF_MEMORY;
        }
        comp->waiter = task->waiter;
        comp->waiter_sched = task->waiter_sched;
        comp->reply = task->reply;
        comp->bytes = bytes;
        comp->len = len;
        comp->is_error = is_error;
        JanetVM *owner = task->owner;
        janet_free(task);
        JanetEVGenericMessage msg;
        memset(&msg, 0, sizeof(msg));
        msg.argp = comp;
        /* No refcount here: the matching inc ran on the owner at
         * dispatch (refcounts are thread-local; worker-inc/owner-dec
         * would strand the worker's counter and wedge its next loop). */
        janet_ev_post_event(owner, janet_pool_complete, msg);
        /* Hand the capacity token back now that the task is complete. */
        janet_channel_push(pool->tokens, janet_wrap_nil(), 2);
    }
}

static void janet_pool_all_exited(JanetEVGenericMessage msg) {
    JanetPool *pool = (JanetPool *) msg.argp;
    /* Join first (all workers already exited: immediate, loop unaffected).
     * Teardown of the internals happens in the type finalizer, not here:
     * the pool struct (and its lock) must stay mapped while any handle
     * Janet exists, so use-after-stop always fails loud via the flags
     * instead of touching freed memory. */
#ifdef JANET_WINDOWS
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
    janet_pool_lock(pool);
    pool->joined = 1;
    JanetFiber *w = pool->stop_waiter;
    uint32_t ws = pool->stop_sched;
    pool->stop_waiter = NULL;
    janet_pool_unlock(pool);
    /* Unpin the wrapper now that no thread can touch the pool. */
    janet_gcunroot(pool->self);
    pool->self = janet_wrap_nil();
    /* Wake the stopper unless it moved on (cancel); teardown stands. */
    if (w != NULL) {
        janet_gcunroot(janet_wrap_fiber(w));
        if (w->sched_id == ws) {
            janet_schedule(w, janet_wrap_nil());
        }
    }
    janet_ev_dec_refcount();
}

static JanetPool *janet_getpool(const Janet *argv, int32_t n) {
    JanetPool *pool = janet_getabstract(argv, n, &janet_pool_type);
    if (pool->owner != &janet_vm) {
        janet_panic("pool used from non-owner thread");
    }
    return pool;
}

JANET_CORE_FN(cfun_pool_spawn,
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
    janet_sandbox_assert(JANET_SANDBOX_THREADS);
    janet_arity(argc, 1, 3);
    int32_t nthreads = janet_getinteger(argv, 0);
    if (nthreads <= 0 || nthreads > 1024) {
        janet_panic("pool thread count must be between 1 and 1024");
    }
    int32_t capacity = 1024;
    if (argc >= 2 && !janet_checktype(argv[1], JANET_NIL)) {
        capacity = janet_getinteger(argv, 1);
    }
    if (capacity <= 0 || capacity > 0xFFFFFF) {
        janet_panic("pool capacity must be between 1 and 16777215");
    }
    uint64_t flags = 0;
    if (argc >= 3) {
        flags = janet_getflags(argv, 2, "ac");
    }
    JanetPool *pool = janet_abstract_threaded(&janet_pool_type, sizeof(JanetPool));
    memset(pool, 0, sizeof(JanetPool));
#ifdef JANET_WINDOWS
    InitializeCriticalSection(&pool->lock);
    InitializeConditionVariable(&pool->nonempty);
#else
    if (pthread_mutex_init(&pool->lock, NULL)) {
        janet_panic("pool mutex init failed");
    }
    if (pthread_cond_init(&pool->nonempty, NULL)) {
        pthread_mutex_destroy(&pool->lock);
        janet_panic("pool condvar init failed");
    }
#endif
    pool->owner = &janet_vm;
    pool->flags = (uint32_t) flags;
    pool->sandbox_flags = janet_vm.sandbox_flags;
    pool->n_workers = nthreads;
    pool->live_workers = nthreads;
    /* Phase C: workers inherit the collector when the owner is
     * profiling at spawn (harvested at worker exit, read via
     * pool/profile-harvest). */
    pool->profile_active = (janet_vm.profile_table != NULL);
    /* Prologue: registries marshalled once, shared read-only (same
     * layout thread/new builds per thread). Flag bits follow
     * janet_getflags order ("ac"): :a skips abstract, :c skips
     * cfunction (previously off by one — :a matched nothing, :c
     * skipped the wrong registry; existing pins were insensitive). */
    {
        JanetBuffer prologue;
        janet_buffer_init(&prologue, 0);
        if (!(flags & 0x1)) {
            janet_marshal(&prologue, janet_wrap_table(janet_vm.abstract_registry), NULL, JANET_MARSHAL_UNSAFE);
        }
        if (!(flags & 0x2)) {
            janet_assert(janet_vm.registry_count <= INT32_MAX, "assert failed size check");
            uint32_t temp = (uint32_t) janet_vm.registry_count;
            janet_buffer_push_bytes(&prologue, (uint8_t *) &temp, sizeof(temp));
            janet_buffer_push_bytes(&prologue, (uint8_t *) janet_vm.registry,
                (int32_t) janet_vm.registry_count * sizeof(JanetCFunRegistry));
        }
        if (prologue.count > 0) {
            pool->prologue = janet_malloc((size_t) prologue.count);
            if (NULL == pool->prologue) {
                janet_buffer_deinit(&prologue);
                janet_pool_sync_destroy(pool);
                JANET_OUT_OF_MEMORY;
            }
            memcpy(pool->prologue, prologue.data, (size_t) prologue.count);
            pool->prologue_len = (size_t) prologue.count;
        }
        janet_buffer_deinit(&prologue);
    }
    /* Capacity tokens: constant supply; submit takes, workers return. */
    pool->tokens = janet_channel_make_threaded((uint32_t) capacity);
    janet_abstract_incref(pool->tokens);
    for (int32_t i = 0; i < capacity; i++) {
        janet_channel_push(pool->tokens, janet_wrap_nil(), 2);
    }
    /* Pin the wrapper until stop: workers must never see a freed pool. */
    pool->self = janet_wrap_abstract(pool);
    janet_gcroot(pool->self);
#ifdef JANET_WINDOWS
    pool->threads = janet_malloc((size_t) nthreads * sizeof(HANDLE));
#else
    pool->threads = janet_malloc((size_t) nthreads * sizeof(pthread_t));
#endif
    if (NULL == pool->threads) {
        janet_pool_free_contents(pool);
        janet_pool_sync_destroy(pool);
        janet_gcunroot(pool->self);
        JANET_OUT_OF_MEMORY;
    }
    memset(pool->threads, 0, (size_t) nthreads * sizeof(pool->threads[0]));
    for (int32_t i = 0; i < nthreads; i++) {
        JanetPoolWorkerArgs *wargs = janet_malloc(sizeof(JanetPoolWorkerArgs));
        if (NULL == wargs) {
            pool->stopping = 1;
            janet_pool_broadcast(pool);
            goto pool_spawn_failed;
        }
        wargs->pool = pool;
        wargs->index = i;
#ifdef JANET_WINDOWS
        pool->threads[i] = CreateThread(NULL, 0, janet_pool_worker, wargs, 0, NULL);
        if (NULL == pool->threads[i]) goto pool_spawn_failed;
#else
        int err = pthread_create(&pool->threads[i], NULL, janet_pool_worker, wargs);
        if (err) goto pool_spawn_failed;
#endif
        continue;
pool_spawn_failed:
        if (wargs != NULL) janet_free(wargs);
        /* Reap the workers that did start (they exit on stopping with
         * an empty queue), then release everything and raise. */
        pool->stopping = 1;
        janet_pool_broadcast(pool);
        for (int32_t j = 0; j < i; j++) {
#ifdef JANET_WINDOWS
            WaitForSingleObject(pool->threads[j], INFINITE);
            CloseHandle(pool->threads[j]);
#else
            pthread_join(pool->threads[j], NULL);
#endif
        }
        janet_pool_free_contents(pool);
        janet_pool_sync_destroy(pool);
        janet_gcunroot(pool->self);
        janet_panic("pool failed to start workers");
    }
    return janet_wrap_abstract(pool);
}

JANET_CORE_FN(cfun_pool_acquire,
              "(pool/acquire p)",
              "Take one capacity token from pool `p`, blocking while the "
              "queue is full. Internal building block for pool/call and "
              "pool/submit: every acquire must be followed by exactly one "
              "pool/dispatch (the wrappers do this back-to-back with no "
              "suspension between, so the token cannot strand).") {
    janet_fixarity(argc, 1);
    JanetPool *pool = janet_getpool(argv, 0);
    if (janet_vm.coerce_error) {
        janet_panic("cannot acquire pool token inside janet_call");
    }
    /* Fail loud on dead pools before touching the token channel (its
     * memory is released at wrapper collection). */
    janet_pool_lock(pool);
    int dead = pool->stopping || pool->joined;
    janet_pool_unlock(pool);
    if (dead) {
        janet_panic("pool is stopping");
    }
    Janet item;
    if (janet_channel_pop(pool->tokens, &item, 0)) {
        janet_schedule(janet_vm.root_fiber, item);
    }
    janet_await();
}

JANET_CORE_FN(cfun_pool_dispatch,
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
    janet_arity(argc, 4, 5);
    JanetPool *pool = janet_getpool(argv, 0);
    if (janet_vm.coerce_error) {
        janet_panic("cannot dispatch pool task inside janet_call");
    }
    int is_call;
    if (janet_checktype(argv[3], JANET_KEYWORD)) {
        const uint8_t *kw = janet_unwrap_keyword(argv[3]);
        if (!strcmp((const char *) kw, "call")) {
            is_call = 1;
        } else if (!strcmp((const char *) kw, "submit")) {
            is_call = 0;
        } else {
            janet_panic("pool kind must be :call or :submit");
        }
    } else {
        janet_panic("pool kind must be :call or :submit");
    }
    double timeout = janet_optnumber(argv, argc, 4, INFINITY);
    if (!is_call && timeout != INFINITY) {
        janet_panic("pool timeout needs :call");
    }
    /* Bind the waiter BEFORE the task is enqueued: the late-binding
     * variant wrote task->waiter after the enqueue+signal, so a fast
     * worker could complete the task and build its completion while
     * waiter was still the malloc-zero NULL — the completion then
     * posted to nobody and the awaiter slept forever (the pool-call
     * hang: main in kevent, workers idle). The root is also gcrooted
     * here so a racing completion's unroot always pairs. */
    JanetFiber *call_waiter = NULL;
    uint32_t call_ws = 0;
    if (is_call) {
        call_waiter = janet_root_fiber();
        if (timeout != INFINITY &&
                !(call_waiter->gc.flags & JANET_FIBER_EV_GCFLAG_ROOT)) {
            janet_panic("pool/call with timeout requires an ev task");
        }
        call_ws = call_waiter->sched_id;
    }
    /* Validate the call shape on the owner (clear errors beat worker
     * asserts): functions take exactly nargs, fibers take the array. */
    int32_t nargs;
    if (janet_checktype(argv[2], JANET_ARRAY)) {
        nargs = janet_unwrap_array(argv[2])->count;
    } else if (janet_checktype(argv[2], JANET_TUPLE)) {
        nargs = janet_tuple_length(janet_unwrap_tuple(argv[2]));
    } else {
        janet_panic("pool args must be an array or tuple");
    }
    if (janet_checktype(argv[1], JANET_FUNCTION)) {
        JanetFunction *func = janet_unwrap_function(argv[1]);
        if (nargs < func->def->min_arity ||
                (func->def->max_arity >= 0 && nargs > func->def->max_arity)) {
            janet_panic("pool function arity mismatch");
        }
    } else if (!janet_checktype(argv[1], JANET_FIBER)) {
        janet_panic("pool func must be a function or fiber");
    }
    /* Marshal [func args] (UNSAFE: crosses into the worker VM). */
    Janet pair[2];
    pair[0] = argv[1];
    pair[1] = argv[2];
    JanetBuffer payload;
    janet_buffer_init(&payload, 0);
    janet_marshal(&payload, janet_wrap_tuple(janet_tuple_n(pair, 2)), NULL, JANET_MARSHAL_UNSAFE);
    uint8_t *bytes = NULL;
    size_t len = (size_t) payload.count;
    if (len > 0) {
        bytes = janet_malloc(len);
        if (NULL == bytes) {
            janet_buffer_deinit(&payload);
            JANET_OUT_OF_MEMORY;
        }
        memcpy(bytes, payload.data, len);
    }
    janet_buffer_deinit(&payload);
    JanetPoolTask *task = janet_malloc(sizeof(JanetPoolTask));
    if (NULL == task) {
        if (bytes != NULL) janet_free(bytes);
        JANET_OUT_OF_MEMORY;
    }
    task->bytes = bytes;
    task->len = len;
    task->owner = &janet_vm;
    task->waiter = call_waiter;
    task->waiter_sched = call_ws;
    if (NULL != call_waiter) janet_gcroot(janet_wrap_fiber(call_waiter));
    task->reply = NULL;
    task->next = NULL;
    Janet replyv = janet_wrap_nil();
    if (!is_call) {
        JanetChannel *reply = janet_channel_make_threaded(1);
        janet_abstract_incref(reply);
        task->reply = reply;
        replyv = janet_wrap_abstract(reply);
    }
    janet_pool_lock(pool);
    if (pool->stopping || pool->joined) {
        janet_pool_unlock(pool);
        /* Hand the token back: total supply is constant, so it fits. */
        janet_channel_push(pool->tokens, janet_wrap_nil(), 2);
        if (NULL != call_waiter) janet_gcunroot(janet_wrap_fiber(call_waiter));
        if (task->reply != NULL) janet_abstract_decref_maybe_free(task->reply);
        if (task->bytes != NULL) janet_free(task->bytes);
        janet_free(task);
        janet_panic("pool is stopping");
    }
    if (pool->tail != NULL) {
        pool->tail->next = task;
    } else {
        pool->head = task;
    }
    pool->tail = task;
    pool->count++;
    janet_pool_signal(pool);
    janet_pool_unlock(pool);
    /* Owner-side ref for the outstanding completion (paired with the
     * dec in janet_pool_complete). Thread-local counters: must inc
     * where the dec runs. */
    janet_ev_inc_refcount();
    if (!is_call) {
        return replyv;
    }
    /* :call: single trailing suspension (take/join shape). The waiter
     * fiber is rooted; the completion unroots it. The pool itself is
     * pinned from spawn until stop. */
    JanetFiber *root = call_waiter;
    if (timeout != INFINITY) {
        janet_addtimeout(timeout);
    }
    janet_await();
    /* Unreachable when suspending; the completion schedules this fiber
     * with the task result (or cancels with its error). */
    return janet_wrap_nil();
}

JANET_CORE_FN(cfun_pool_profile_harvest,
              "(pool/profile-harvest p)",
              "Drain this pool's harvested per-worker profiles (Phase C): "
              "returns an array with one dump-format row array per exited "
              "worker that profiled (empty when profiling was off or "
              "nothing exited yet). Workers inherit the collector when the "
              "owner is profiling at spawn; each exiting worker appends "
              "its table. Merge rows Janet-side (tools/profile-merge.janet) "
              "and feed callgraph --profile. Must be called from the owning "
              "thread. Unread harvests are freed with the pool.") {
    janet_fixarity(argc, 1);
    JanetPool *pool = janet_getpool(argv, 0);
    JanetArray *out = janet_array(0);
    Janet out_j = janet_wrap_array(out);
    janet_gcroot(out_j);
    janet_pool_lock(pool);
    JanetProfileHarvest *h = pool->harvest_head;
    pool->harvest_head = pool->harvest_tail = NULL;
    janet_pool_unlock(pool);
    while (h != NULL) {
        JanetProfileHarvest *next = h->next;
        if (h->bytes != NULL && h->len > 0) {
            Janet rows = janet_unmarshal(h->bytes, h->len, JANET_MARSHAL_UNSAFE, NULL, NULL);
            janet_array_push(out, rows);
            janet_free(h->bytes);
        } else if (h->bytes != NULL) {
            janet_free(h->bytes);
        }
        janet_free(h);
        h = next;
    }
    janet_gcunroot(out_j);
    return out_j;
}

JANET_CORE_FN(cfun_pool_stop,
              "(pool/stop p)",
              "Drain-stop pool `p`: queued tasks run first, then every "
              "worker exits; returns nil once all workers are joined. "
              "Suspends (the loop stays live during the drain). Further "
              "use panics. Must be called from the owning thread, exactly "
              "once. Pools must be stopped (like files must be closed).") {
    janet_fixarity(argc, 1);
    JanetPool *pool = janet_getpool(argv, 0);
    if (janet_vm.coerce_error) {
        janet_panic("cannot stop pool inside janet_call");
    }
    janet_pool_lock(pool);
    if (pool->joined || pool->stopping) {
        janet_pool_unlock(pool);
        janet_panic("pool already stopped");
    }
    pool->stopping = 1;
    if (pool->stop_waiter != NULL) {
        janet_pool_unlock(pool);
        janet_panic("pool already stopping");
    }
    JanetFiber *root = janet_root_fiber();
    pool->stop_waiter = root;
    pool->stop_sched = root->sched_id;
    janet_pool_broadcast(pool);
    janet_pool_unlock(pool);
    /* Rooted until the all-exited callback unroots (teardown stands
     * even if this waiter is cancelled first). Owner-side ref for the
     * all-exited event, paired with its dec there. */
    janet_gcroot(janet_wrap_fiber(root));
    janet_ev_inc_refcount();
    janet_await();
    /* Unreachable when suspending; the callback schedules nil here. */
    return janet_wrap_nil();
}

JANET_CORE_FN(cfun_thread_monitor,
              "(ev/monitor handle tag chan)",
              "Watch the thread `handle` created by `thread/new`: when the "
              "worker exits, send `[:down tag reason]` to `chan`, where "
              "`reason` is `:normal` on clean exit or the thread's error "
              "value on failure. Delivery is best-effort (at-most-once): "
              "the message is dropped if `chan` is closed or full. "
              "Monitoring an already-finished thread delivers immediately "
              "and returns nil. Must be called from the creating thread. "
              "Returns an integer monitor ref for `ev/demonitor`.") {
    janet_sandbox_assert(JANET_SANDBOX_THREADS);
    janet_fixarity(argc, 3);
    JanetThreadHandle *handle = janet_getabstract(argv, 0, &janet_thread_handle_type);
    if (handle->owner != &janet_vm) {
        janet_panic("ev/monitor called from non-owner thread");
    }
    Janet chanv = argv[2];
    janet_getchannel(argv, 2);
    Janet tag = argv[1];
    if (handle->done) {
        /* Late registration: completion already ran, deliver now.
         * No slot is stored, so there is no ref to return. */
        janet_thread_give_down(chanv,
            janet_thread_down_message(tag, handle->is_error, handle->result));
        return janet_wrap_nil();
    }
    if (handle->n_monitors >= handle->cap_monitors) {
        int32_t ncap = handle->cap_monitors ? handle->cap_monitors * 2 : 4;
        Janet *nmon = janet_realloc(handle->monitors, (size_t) ncap * 2 * sizeof(Janet));
        if (NULL == nmon) {
            JANET_OUT_OF_MEMORY;
        }
        handle->monitors = nmon;
        handle->cap_monitors = ncap;
    }
    janet_gcroot(chanv);
    janet_gcroot(tag);
    int32_t ref = handle->n_monitors;
    handle->monitors[ref * 2] = chanv;
    handle->monitors[ref * 2 + 1] = tag;
    handle->n_monitors++;
    return janet_wrap_integer(ref);
}

/* M3: process-global name registry. One store shared by every VM
 * in the process (Janet tables are per-VM and isolated, so this lives
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
} JanetNameRegistryEntry;

static JanetNameRegistryEntry *reg_entries = NULL;
static int32_t reg_count = 0;
static int32_t reg_cap = 0;
/* Local platform mutex (JanetOSMutex is opaque and cannot embed). */
#ifdef JANET_WINDOWS
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
#ifdef JANET_WINDOWS
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

JANET_CORE_FN(cfun_registry_register,
              "(ev/register name value)",
              "Bind keyword `name` to `value` in the process-global "
              "registry, visible from every thread. `value` is stored as "
              "a marshaled copy: lookups get an equal copy in the "
              "caller's own VM, never a cross-heap reference. Replacing "
              "an entry drops the old copy. Unmarshalable values (thread "
              "handles) raise. Returns `value`.") {
    janet_fixarity(argc, 2);
    const uint8_t *kws = janet_getkeyword(argv, 0);
    const char *name = (const char *) kws;
    /* Thread handles are owner-local capabilities: UNSAFE marshal
     * would pass them by pointer (silently leaking a ref and handing
     * out a handle every foreign use rejects). Refuse explicitly. */
    if (janet_checktype(argv[1], JANET_ABSTRACT) &&
            janet_abstract_type(janet_unwrap_abstract(argv[1])) == &janet_thread_handle_type) {
        janet_panic("ev/register cannot store thread handles (owner-local)");
    }
    JanetBuffer tmp;
    janet_buffer_init(&tmp, 0);
    janet_marshal(&tmp, argv[1], NULL, JANET_MARSHAL_UNSAFE);
    uint8_t *copy = janet_malloc(tmp.count ? (size_t) tmp.count : 1);
    if (NULL == copy) {
        janet_buffer_deinit(&tmp);
        JANET_OUT_OF_MEMORY;
    }
    if (tmp.count) memcpy(copy, tmp.data, (size_t) tmp.count);
    size_t len = (size_t) tmp.count;
    janet_buffer_deinit(&tmp);
    reg_ensure_init();
    reg_lock_take();
    int32_t at = reg_find(name);
    if (at >= 0) {
        janet_free(reg_entries[at].bytes);
        reg_entries[at].bytes = copy;
        reg_entries[at].len = len;
    } else {
        if (reg_count >= reg_cap) {
            int32_t ncap = reg_cap ? reg_cap * 2 : 8;
            JanetNameRegistryEntry *nent = janet_realloc(reg_entries,
                (size_t) ncap * sizeof(JanetNameRegistryEntry));
            if (NULL == nent) {
                reg_lock_drop();
                janet_free(copy);
                JANET_OUT_OF_MEMORY;
            }
            reg_entries = nent;
            reg_cap = ncap;
        }
        size_t nlen = strlen(name) + 1;
        char *kcopy = janet_malloc(nlen);
        if (NULL == kcopy) {
            reg_lock_drop();
            janet_free(copy);
            JANET_OUT_OF_MEMORY;
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

JANET_CORE_FN(cfun_registry_whereis,
              "(ev/whereis name)",
              "Look up keyword `name` in the process-global registry. "
              "Returns an unmarshaled copy of the registered value in "
              "the caller's VM, or nil when absent.") {
    janet_fixarity(argc, 1);
    const uint8_t *kws = janet_getkeyword(argv, 0);
    const char *name = (const char *) kws;
    reg_ensure_init();
    reg_lock_take();
    int32_t at = reg_find(name);
    uint8_t *copy = NULL;
    size_t len = 0;
    if (at >= 0 && reg_entries[at].len) {
        len = reg_entries[at].len;
        copy = janet_malloc(len);
        if (NULL == copy) {
            reg_lock_drop();
            JANET_OUT_OF_MEMORY;
        }
        memcpy(copy, reg_entries[at].bytes, len);
    }
    reg_lock_drop();
    if (NULL == copy) {
        return janet_wrap_nil();
    }
    Janet out = janet_unmarshal(copy, len, JANET_MARSHAL_UNSAFE, NULL, NULL);
    janet_free(copy);
    return out;
}

JANET_CORE_FN(cfun_registry_unregister,
              "(ev/unregister name)",
              "Remove keyword `name` from the process-global registry. "
              "Returns true when an entry was removed, false otherwise.") {
    janet_fixarity(argc, 1);
    const uint8_t *kws = janet_getkeyword(argv, 0);
    const char *name = (const char *) kws;
    reg_ensure_init();
    reg_lock_take();
    int32_t at = reg_find(name);
    int found = (at >= 0);
    if (found) {
        janet_free(reg_entries[at].name);
        janet_free(reg_entries[at].bytes);
        reg_entries[at] = reg_entries[reg_count - 1];
        reg_count--;
    }
    reg_lock_drop();
    return janet_wrap_boolean(found);
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
static const JanetAbstractType janet_shared_type;
#define SHARED_MAX_DEPTH 256

typedef struct {
    uint32_t len;
    uint8_t data[];
} JanetSharedBlob;

static void shared_push_u32(JanetBuffer *buf, uint32_t v) {
    uint8_t b[4];
    b[0] = (uint8_t) (v & 0xFF);
    b[1] = (uint8_t) ((v >> 8) & 0xFF);
    b[2] = (uint8_t) ((v >> 16) & 0xFF);
    b[3] = (uint8_t) ((v >> 24) & 0xFF);
    janet_buffer_push_bytes(buf, b, 4);
}

static int shared_is_blob(Janet x) {
    return janet_checktype(x, JANET_ABSTRACT) &&
        janet_abstract_type(janet_unwrap_abstract(x)) == &janet_shared_type;
}

/* Forward declarations (encoder/decoder recurse). */
static void shared_encode(JanetBuffer *buf, Janet x, int depth);

/* Deep shareability check: only immutable, heap-free leaves. */
static int shared_checkable(Janet x, int depth) {
    if (depth > SHARED_MAX_DEPTH) return 0;
    switch (janet_type(x)) {
        case JANET_NIL:
        case JANET_BOOLEAN:
        case JANET_NUMBER:
            return 1;
        case JANET_STRING:
        case JANET_SYMBOL:
        case JANET_KEYWORD:
            return 1;
        case JANET_TUPLE: {
            JanetTuple t = janet_unwrap_tuple(x);
            int32_t n = janet_tuple_length(t);
            for (int32_t i = 0; i < n; i++) {
                if (!shared_checkable(t[i], depth + 1)) return 0;
            }
            return 1;
        }
        case JANET_STRUCT: {
            /* Structs are open-addressed hash tables: iterate to
             * CAPACITY skipping nil keys (length counts live only).
             * Prototypes change lookup semantics and do not flatten,
             * so protos are rejected. */
            JanetStruct s = janet_unwrap_struct(x);
            if (NULL != janet_struct_proto(s)) return 0;
            int32_t cap = janet_struct_capacity(s);
            for (int32_t i = 0; i < cap; i++) {
                if (janet_checktype(s[i].key, JANET_NIL)) continue;
                if (!shared_checkable(s[i].key, depth + 1)) return 0;
                if (!shared_checkable(s[i].value, depth + 1)) return 0;
            }
            return 1;
        }
        case JANET_ABSTRACT:
            return shared_is_blob(x);
        default:
            return 0;
    }
}

static void shared_encode_str(JanetBuffer *buf, uint8_t tag, const uint8_t *s) {
    int32_t len = janet_string_length(s);
    janet_buffer_push_u8(buf, tag);
    shared_push_u32(buf, (uint32_t) len);
    janet_buffer_push_bytes(buf, s, len);
}

static void shared_encode(JanetBuffer *buf, Janet x, int depth) {
    if (depth > SHARED_MAX_DEPTH) janet_panic("shared value too deep");
    switch (janet_type(x)) {
        case JANET_NIL:
            janet_buffer_push_u8(buf, 0);
            break;
        case JANET_BOOLEAN:
            janet_buffer_push_u8(buf, janet_unwrap_boolean(x) ? 2 : 1);
            break;
        case JANET_NUMBER: {
            double d = janet_unwrap_number(x);
            janet_buffer_push_u8(buf, 3);
            janet_buffer_push_bytes(buf, (uint8_t *) &d, 8);
            break;
        }
        case JANET_STRING:
            shared_encode_str(buf, 4, janet_unwrap_string(x));
            break;
        case JANET_SYMBOL:
            shared_encode_str(buf, 6, janet_unwrap_symbol(x));
            break;
        case JANET_KEYWORD:
            shared_encode_str(buf, 5, janet_unwrap_keyword(x));
            break;
        case JANET_TUPLE: {
            JanetTuple t = janet_unwrap_tuple(x);
            int32_t n = janet_tuple_length(t);
            janet_buffer_push_u8(buf, 8);
            shared_push_u32(buf, (uint32_t) n);
            for (int32_t i = 0; i < n; i++) shared_encode(buf, t[i], depth + 1);
            break;
        }
        case JANET_STRUCT: {
            JanetStruct s = janet_unwrap_struct(x);
            int32_t n = janet_struct_length(s);
            int32_t cap = janet_struct_capacity(s);
            janet_buffer_push_u8(buf, 7);
            shared_push_u32(buf, (uint32_t) n);
            for (int32_t i = 0; i < cap; i++) {
                if (janet_checktype(s[i].key, JANET_NIL)) continue;
                shared_encode(buf, s[i].key, depth + 1);
                shared_encode(buf, s[i].value, depth + 1);
            }
            break;
        }
        case JANET_ABSTRACT: {
            /* Nested shared: inline expansion (deep copy — sharing is
             * by value, still immutable so semantics hold). */
            JanetSharedBlob *nb = janet_unwrap_abstract(x);
            janet_buffer_push_u8(buf, 9);
            shared_push_u32(buf, nb->len);
            janet_buffer_push_bytes(buf, nb->data, nb->len);
            break;
        }
        default:
            janet_panicf("cannot share value of type %s", janet_type_names[janet_type(x)]);
    }
}

/* Defensive cursor decoder: every read bounds-checked, truncation
 * panics instead of over-reading (marshal bytes cross threads). */
typedef struct {
    const uint8_t *p;
    const uint8_t *end;
} JanetSharedCursor;

static void shared_need(JanetSharedCursor *c, size_t n) {
    if ((size_t)(c->end - c->p) < n) janet_panic("truncated shared value");
}

static uint8_t shared_rd_u8(JanetSharedCursor *c) {
    shared_need(c, 1);
    return *(c->p)++;
}

static uint32_t shared_rd_u32(JanetSharedCursor *c) {
    shared_need(c, 4);
    uint32_t v = ((uint32_t) c->p[0]) | ((uint32_t) c->p[1] << 8) |
        ((uint32_t) c->p[2] << 16) | ((uint32_t) c->p[3] << 24);
    c->p += 4;
    return v;
}

static const uint8_t *shared_rd_bytes(JanetSharedCursor *c, size_t n) {
    shared_need(c, n);
    const uint8_t *p = c->p;
    c->p += n;
    return p;
}

static Janet shared_decode(JanetSharedCursor *c, int depth) {
    if (depth > SHARED_MAX_DEPTH) janet_panic("shared value too deep");
    uint8_t tag = shared_rd_u8(c);
    switch (tag) {
        case 0:
            return janet_wrap_nil();
        case 1:
            return janet_wrap_boolean(0);
        case 2:
            return janet_wrap_boolean(1);
        case 3: {
            double d = 0;
            memcpy(&d, shared_rd_bytes(c, 8), 8);
            return janet_wrap_number(d);
        }
        case 4: {
            uint32_t n = shared_rd_u32(c);
            const uint8_t *b = shared_rd_bytes(c, n);
            return janet_stringv(b, (int32_t) n);
        }
        case 5: {
            uint32_t n = shared_rd_u32(c);
            const uint8_t *b = shared_rd_bytes(c, n);
            return janet_keywordv(b, (int32_t) n);
        }
        case 6: {
            uint32_t n = shared_rd_u32(c);
            const uint8_t *b = shared_rd_bytes(c, n);
            return janet_symbolv(b, (int32_t) n);
        }
        case 7: {
            uint32_t n = shared_rd_u32(c);
            JanetTable *t = janet_table(0);
            for (uint32_t i = 0; i < n; i++) {
                Janet k = shared_decode(c, depth + 1);
                Janet v = shared_decode(c, depth + 1);
                janet_table_put(t, k, v);
            }
            return janet_wrap_struct(janet_table_to_struct(t));
        }
        case 8: {
            uint32_t n = shared_rd_u32(c);
            Janet *items = janet_malloc(sizeof(Janet) * (n ? n : 1));
            if (NULL == items) JANET_OUT_OF_MEMORY;
            uint32_t i = 0;
            for (; i < n; i++) items[i] = shared_decode(c, depth + 1);
            Janet ret = janet_wrap_tuple(janet_tuple_n(items, (int32_t) n));
            janet_free(items);
            return ret;
        }
        case 9: {
            /* Nested blob: rebuild fresh (deep-copy semantics). */
            uint32_t n = shared_rd_u32(c);
            const uint8_t *b = shared_rd_bytes(c, n);
            JanetSharedBlob *nb = janet_abstract_threaded(&janet_shared_type,
                sizeof(JanetSharedBlob) + n);
            nb->len = n;
            if (n) memcpy(nb->data, b, n);
            return janet_wrap_abstract(nb);
        }
        default:
            janet_panicf("invalid shared tag %d", tag);
            return janet_wrap_nil();
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

JANET_CORE_FN(cfun_thread_demonitor,
              "(ev/demonitor handle ref)",
              "Remove the monitor `ref` (returned by `ev/monitor`) from "
              "the thread `handle`, so no `:down` message is delivered for "
              "it. Unknown refs, already-removed monitors, and monitors on "
              "finished threads are silent no-ops. Must be called from the "
              "creating thread. Returns nil.") {
    janet_sandbox_assert(JANET_SANDBOX_THREADS);
    janet_fixarity(argc, 2);
    JanetThreadHandle *handle = janet_getabstract(argv, 0, &janet_thread_handle_type);
    if (handle->owner != &janet_vm) {
        janet_panic("ev/demonitor called from non-owner thread");
    }
    if (!janet_checkint(argv[1])) {
        janet_panic("ev/demonitor ref must be an integer");
    }
    int32_t ref = janet_unwrap_integer(argv[1]);
    /* Finished threads already delivered and freed the list. */
    if (handle->monitors == NULL) {
        return janet_wrap_nil();
    }
    if (ref < 0 || ref >= handle->n_monitors) {
        return janet_wrap_nil();
    }
    Janet chanv = handle->monitors[ref * 2];
    Janet tag = handle->monitors[ref * 2 + 1];
    if (janet_checktype(chanv, JANET_NIL)) {
        /* Already removed. */
        return janet_wrap_nil();
    }
    janet_gcunroot(chanv);
    janet_gcunroot(tag);
    handle->monitors[ref * 2] = janet_wrap_nil();
    handle->monitors[ref * 2 + 1] = janet_wrap_nil();
    return janet_wrap_nil();
}

/* Blob construction + type-level (un)marshal + accessors. */

static JanetSharedBlob *shared_blob_new(const uint8_t *data, uint32_t len) {
    JanetSharedBlob *b = janet_abstract_threaded(&janet_shared_type,
        sizeof(JanetSharedBlob) + (len ? len : 1));
    b->len = len;
    if (len) memcpy(b->data, data, len);
    return b;
}

static void janet_shared_marshal(void *p, JanetMarshalContext *ctx) {
    JanetSharedBlob *b = p;
    janet_marshal_int(ctx, (int32_t) b->len);
    if (b->len) janet_marshal_bytes(ctx, b->data, b->len);
}

static void *janet_shared_unmarshal(JanetMarshalContext *ctx) {
    int32_t len = janet_unmarshal_int(ctx);
    if (len < 0) janet_panic("invalid shared length");
    JanetSharedBlob *b = janet_abstract_threaded(&janet_shared_type,
        sizeof(JanetSharedBlob) + (len ? (uint32_t) len : 1));
    b->len = (uint32_t) len;
    if (len) {
        uint8_t *tmp = janet_malloc((size_t) len);
        if (NULL == tmp) JANET_OUT_OF_MEMORY;
        janet_unmarshal_bytes(ctx, tmp, (size_t) len);
        memcpy(b->data, tmp, (size_t) len);
        janet_free(tmp);
    }
    /* Register in the lookup table (alias/cycle tracking + the
     * framework's completion check); stays threaded either way. */
    janet_unmarshal_abstract_reuse(ctx, b);
    return b;
}

static int janet_shared_mark(void *p, size_t len) {
    /* Payload holds no GC pointers — nothing to mark. */
    (void) p;
    (void) len;
    return 0;
}

static void janet_shared_tostring(void *p, JanetBuffer *buffer) {
    (void) p;
    janet_buffer_push_cstring(buffer, "<shared>");
}

static const JanetAbstractType janet_shared_type = {
    "core/shared",
    NULL, /* gc (inline payload frees with the allocation) */
    janet_shared_mark,
    NULL, /* get (use shared/get) */
    NULL, /* put */
    janet_shared_marshal,
    janet_shared_unmarshal,
    janet_shared_tostring,
    NULL, /* compare */
    NULL, /* hash */
    NULL, /* next */
    NULL, /* call */
    NULL, /* length (use shared/length) */
    NULL, /* bytes */
    NULL  /* gcperthread (no nested refs to drop) */
};

static JanetSharedBlob *shared_get_blob(Janet x) {
    if (!shared_is_blob(x)) janet_panic("expected shared blob");
    return janet_unwrap_abstract(x);
}

JANET_CORE_FN(cfun_shared_new,
              "(shared/new x)",
              "Freeze the shareable value `x` into an immutable blob "
              "that is sound in every VM at once (lifetime is atomic "
              "refcounting, cross-thread movement is by pointer). "
              "Allowed leaves: nil, booleans, numbers, strings, symbols, "
              "keywords, tuples, structs, and nested shared blobs. "
              "Anything else (tables, arrays, buffers, functions, fibers, "
              "other abstracts) raises. Returns the blob.") {
    janet_fixarity(argc, 1);
    if (!shared_checkable(argv[0], 0)) {
        janet_panicf("cannot share value of type %s",
            janet_type_names[janet_type(argv[0])]);
    }
    JanetBuffer buf;
    janet_buffer_init(&buf, 0);
    shared_encode(&buf, argv[0], 0);
    JanetSharedBlob *b = shared_blob_new(buf.data, buf.count);
    janet_buffer_deinit(&buf);
    return janet_wrap_abstract(b);
}

JANET_CORE_FN(cfun_shared_shareable,
              "(shared/shareable? x)",
              "True when `x` can be frozen with shared/new (deep check).") {
    janet_fixarity(argc, 1);
    return janet_wrap_boolean(shared_checkable(argv[0], 0));
}

JANET_CORE_FN(cfun_shared_length,
              "(shared/length b)",
              "Element count of a struct/tuple blob, else nil.") {
    janet_fixarity(argc, 1);
    JanetSharedBlob *b = shared_get_blob(argv[0]);
    if (b->len < 1) return janet_wrap_nil();
    uint8_t tag = b->data[0];
    if (tag != 7 && tag != 8) return janet_wrap_nil();
    if (b->len < 5) return janet_wrap_nil();
    uint32_t n = ((uint32_t) b->data[1]) | ((uint32_t) b->data[2] << 8) |
        ((uint32_t) b->data[3] << 16) | ((uint32_t) b->data[4] << 24);
    return janet_wrap_integer((int32_t) n);
}

JANET_CORE_FN(cfun_shared_get,
              "(shared/get b k)",
              "Look up key `k` in a struct blob (or index a tuple blob) "
              "without decoding the whole value. Missing keys, "
              "out-of-range indices, and scalar blobs give nil. "
              "Unshareable key types also give nil.") {
    janet_fixarity(argc, 2);
    JanetSharedBlob *b = shared_get_blob(argv[0]);
    const uint8_t *end = b->data + b->len;
    if (b->len < 1) return janet_wrap_nil();
    uint8_t tag = b->data[0];
    if (tag == 8) {
        /* Tuple index. */
        if (!janet_checkint(argv[1])) return janet_wrap_nil();
        int32_t idx = janet_unwrap_integer(argv[1]);
        if (b->len < 5 || idx < 0) return janet_wrap_nil();
        uint32_t n = ((uint32_t) b->data[1]) | ((uint32_t) b->data[2] << 8) |
            ((uint32_t) b->data[3] << 16) | ((uint32_t) b->data[4] << 24);
        if ((uint32_t) idx >= n) return janet_wrap_nil();
        const uint8_t *p = b->data + 5;
        for (int32_t i = 0; i < idx; i++) {
            p = shared_skip_value(p, end);
            if (NULL == p) return janet_wrap_nil();
        }
        JanetSharedCursor c = {p, end};
        return shared_decode(&c, 0);
    }
    if (tag != 7) return janet_wrap_nil();
    if (!shared_checkable(argv[1], 0)) return janet_wrap_nil();
    JanetBuffer kb;
    janet_buffer_init(&kb, 0);
    shared_encode(&kb, argv[1], 0);
    const uint8_t *valp = NULL;
    int found = shared_struct_find(b->data + 1, end, kb.data, kb.count, &valp);
    janet_buffer_deinit(&kb);
    if (!found) return janet_wrap_nil();
    JanetSharedCursor c = {valp, end};
    return shared_decode(&c, 0);
}

JANET_CORE_FN(cfun_shared_materialize,
              "(shared/materialize b)",
              "Decode a whole blob back to ordinary caller-heap values "
              "(structs, tuples, re-interned keywords). The blob itself "
              "is unchanged.") {
    janet_fixarity(argc, 1);
    JanetSharedBlob *b = shared_get_blob(argv[0]);
    JanetSharedCursor c = {b->data, b->data + b->len};
    return shared_decode(&c, 0);
}

JANET_CORE_FN(cfun_ev_timeslice,
              "(ev/timeslice &opt on?)",
              "Enable (truthy) or disable (false/nil) preemptive "
              "round-robin quanta for the current VM, and return the new "
              "setting. Quanta are ON by default (Step 2 preemptive "
              "reductions; JANET_TIMESLICE=0 starts a process with them "
              "off): every scheduled task runs at most "
              "JANET_QUANTUM_BUDGET back-edge/call polls before it is "
              "transparently re-queued: compute hogs can no longer starve "
              "sleepers and takers, with results bit-exact. Quanta never "
              "land inside C calls or pcall regions (poll points are Janet "
              "ops only). Incompatible with external C quiesce while on. "
              "Per-VM; workers inherit the default independently.") {
    janet_arity(argc, 0, 1);
    if (argc >= 1) {
        janet_vm.quantum = janet_truthy(argv[0]) ? 1 : 0;
    }
    return janet_wrap_boolean(janet_vm.quantum);
}

JANET_CORE_FN(cfun_ev_interrupt,
              "(ev/interrupt fiber &opt payload)",
              "Request `fiber` (same VM) suspend with a yield of `payload` "
              "(default nil) at its next resume boundary: before running "
              "another instruction, never inside pcall, never in C. A "
              "racing wakeup value is dropped (interrupt wins); interrupts "
              "do not cross VMs (cleared on unmarshal). Returns `fiber`.") {
    janet_arity(argc, 1, 2);
    JanetFiber *fiber = janet_getfiber(argv, 0);
    fiber->interrupt_requested = 1;
    fiber->interrupt_value = (argc >= 2) ? argv[1] : janet_wrap_nil();
    return argv[0];
}

JANET_CORE_FN(cfun_ev_give_supervisor,
              "(ev/give-supervisor tag & payload)",
              "Send a message to the current supervisor channel if there is one. The message will be a "
              "tuple of all of the arguments combined into a single message, where the first element is tag. "
              "By convention, tag should be a keyword indicating the type of message. Returns nil.") {
    janet_arity(argc, 1, -1);
    void *chanv = janet_vm.root_fiber->supervisor_channel;
    if (NULL != chanv) {
        JanetChannel *chan = janet_channel_unwrap(chanv);
        if (janet_channel_push(chan, janet_wrap_tuple(janet_tuple_n(argv, argc)), 0)) {
            janet_await();
        }
    }
    return janet_wrap_nil();
}

JANET_NO_RETURN void janet_sleep_await(double sec) {
    JanetTimeout to;
    to.when = ts_delta(ts_now(), sec);
    to.fiber = janet_vm.root_fiber;
    to.is_error = 0;
    to.sched_id = to.fiber->sched_id;
    to.curr_fiber = NULL;
    to.has_worker = 0;
    add_timeout(to);
    janet_await();
}

JANET_CORE_FN(cfun_ev_sleep,
              "(ev/sleep sec)",
              "Suspend the current fiber for sec seconds without blocking the event loop.") {
    janet_fixarity(argc, 1);
    double sec = janet_getnumber(argv, 0);
    janet_sleep_await(sec);
}

JANET_CORE_FN(cfun_ev_deadline,
              "(ev/deadline sec &opt tocancel tocheck intr?)",
              "Schedules the event loop to try to cancel the `tocancel` task as with `ev/cancel`. "
              "After `sec` seconds, the event loop will attempt cancellation of `tocancel` if the "
              "`tocheck` fiber is resumable. `sec` is a number that can have a fractional part. "
              "`tocancel` defaults to `(fiber/root)`, but if specified, must be a task (root "
              "fiber). `tocheck` defaults to `(fiber/current)`, but if specified, must be a fiber. "
              "Returns `tocancel` immediately. If `interrupt?` is set to true, will create a "
              "background thread to try to interrupt the VM if the timeout expires.") {
    janet_arity(argc, 1, 4);
    double sec = janet_getnumber(argv, 0);
    sec = (sec < 0) ? 0 : sec;
    JanetFiber *tocancel = janet_optfiber(argv, argc, 1, janet_vm.root_fiber);
    JanetFiber *tocheck = janet_optfiber(argv, argc, 2, janet_vm.fiber);
    int use_interrupt = janet_optboolean(argv, argc, 3, 0);
    JanetTimeout to;
    to.when = ts_delta(ts_now(), sec);
    to.fiber = tocancel;
    to.curr_fiber = tocheck;
    to.is_error = 0;
    to.sched_id = to.fiber->sched_id;
    if (use_interrupt) {
#ifdef JANET_ANDROID
        janet_sandbox_assert(JANET_SANDBOX_SIGNAL);
#endif
        JanetThreadedTimeout *tto = janet_malloc(sizeof(JanetThreadedTimeout));
        if (NULL == tto) {
            JANET_OUT_OF_MEMORY;
        }
        tto->sec = sec;
        tto->vm = &janet_vm;
        tto->fiber = tocheck;
#ifdef JANET_WINDOWS
        HANDLE cancel_event = CreateEvent(NULL, TRUE, FALSE, NULL);
        if (NULL == cancel_event) {
            janet_free(tto);
            janet_panic("failed to create cancel event");
        }
        tto->cancel_event = cancel_event;
        HANDLE worker = CreateThread(NULL, 0, janet_timeout_body, tto, CREATE_SUSPENDED, NULL);
        if (NULL == worker) {
            janet_free(tto);
            janet_panic("failed to create thread");
        }
#else
        pthread_t worker;
        int err = pthread_create(&worker, NULL, janet_timeout_body, tto);
        if (err) {
            janet_free(tto);
            janet_panicf("%s", janet_strerror(err));
        }
#endif
        to.has_worker = 1;
        to.worker = worker;
#ifdef JANET_WINDOWS
        to.worker_event = cancel_event;
        ResumeThread(worker);
#endif
    } else {
        to.has_worker = 0;
    }
    add_timeout(to);
    return janet_wrap_fiber(tocancel);
}

JANET_CORE_FN(cfun_ev_cancel,
              "(ev/cancel fib err)",
              "Cancel a suspended task fiber `fib` in the event loop. "
              "Differs from `cancel` in that it returns the canceled "
              "fiber immediately.") {
    janet_fixarity(argc, 2);
    JanetFiber *fiber = janet_getfiber(argv, 0);
    Janet err = argv[1];
    janet_cancel(fiber, err);
    return argv[0];
}

JANET_CORE_FN(janet_cfun_stream_close,
              "(ev/close stream)",
              "Close a stream. This should be the same as calling (:close stream) for all streams.") {
    janet_fixarity(argc, 1);
    JanetStream *stream = janet_getabstract(argv, 0, &janet_stream_type);
    janet_stream_close(stream);
    return argv[0];
}

JANET_CORE_FN(janet_cfun_stream_read,
              "(ev/read stream n &opt buf timeout)",
              "Read at most `n` bytes into a buffer asynchronously from a "
              "stream. `n` can also be `:all` to read into the buffer until "
              "end of stream. Optionally provide a buffer, `buf`, to write "
              "into and `timeout` in seconds after which to cancel the "
              "operation and raise an error. Returns the buffer if the read "
              "was successful or nil if end-of-stream reached. Raises "
              "an error if there were problems with the IO operation.") {
    janet_arity(argc, 2, 4);
    JanetStream *stream = janet_getabstract(argv, 0, &janet_stream_type);
    janet_stream_flags(stream, JANET_STREAM_READABLE);
    JanetBuffer *buffer = janet_optbuffer(argv, argc, 2, 10);
    double to = janet_optnumber(argv, argc, 3, INFINITY);
    if (janet_keyeq(argv[1], "all")) {
        if (to != INFINITY) janet_addtimeout(to);
        janet_ev_readchunk(stream, buffer, INT32_MAX);
    } else {
        int32_t n = janet_getnat(argv, 1);
        if (to != INFINITY) janet_addtimeout(to);
        janet_ev_read(stream, buffer, n);
    }
}

JANET_CORE_FN(janet_cfun_stream_chunk,
              "(ev/chunk stream n &opt buf timeout)",
              "Same as `ev/read`, but does not return early if less than "
              "`n` bytes are available. If an end-of-stream is reached, "
              "returns early with the collected bytes.") {
    janet_arity(argc, 2, 4);
    JanetStream *stream = janet_getabstract(argv, 0, &janet_stream_type);
    janet_stream_flags(stream, JANET_STREAM_READABLE);
    int32_t n = janet_getnat(argv, 1);
    JanetBuffer *buffer = janet_optbuffer(argv, argc, 2, 10);
    double to = janet_optnumber(argv, argc, 3, INFINITY);
    if (to != INFINITY) janet_addtimeout(to);
    janet_ev_readchunk(stream, buffer, n);
}

JANET_CORE_FN(janet_cfun_stream_write,
              "(ev/write stream data &opt timeout)",
              "Write data to a stream, suspending the current fiber until the write "
              "completes. Takes an optional timeout in seconds, after which will return nil. "
              "Returns nil, or raises an error if the write failed.") {
    janet_arity(argc, 2, 3);
    JanetStream *stream = janet_getabstract(argv, 0, &janet_stream_type);
    janet_stream_flags(stream, JANET_STREAM_WRITABLE);
    double to = janet_optnumber(argv, argc, 2, INFINITY);
    if (janet_checktype(argv[1], JANET_BUFFER)) {
        if (to != INFINITY) janet_addtimeout(to);
        janet_ev_write_buffer(stream, janet_getbuffer(argv, 1));
    } else {
        JanetByteView bytes = janet_getbytes(argv, 1);
        if (to != INFINITY) janet_addtimeout(to);
        janet_ev_write_string(stream, bytes.bytes);
    }
}

static int mutexgc(void *p, size_t size) {
    (void) size;
    janet_os_mutex_deinit(p);
    return 0;
}

const JanetAbstractType janet_mutex_type = {
    "core/lock",
    mutexgc,
    JANET_ATEND_GC
};

JANET_CORE_FN(janet_cfun_mutex,
              "(ev/lock)",
              "Create a new lock to coordinate threads.") {
    janet_fixarity(argc, 0);
    (void) argv;
    void *mutex = janet_abstract_threaded(&janet_mutex_type, janet_os_mutex_size());
    janet_os_mutex_init(mutex);
    return janet_wrap_abstract(mutex);
}

JANET_CORE_FN(janet_cfun_mutex_acquire,
              "(ev/acquire-lock lock)",
              "Acquire a lock such that this operating system thread is the only thread with access to this resource."
              " This will block this entire thread until the lock becomes available, and will not yield to other fibers "
              "on this system thread.") {
    janet_fixarity(argc, 1);
    void *mutex = janet_getabstract(argv, 0, &janet_mutex_type);
    janet_os_mutex_lock(mutex);
    return argv[0];
}

JANET_CORE_FN(janet_cfun_mutex_release,
              "(ev/release-lock lock)",
              "Release a lock such that other threads may acquire it.") {
    janet_fixarity(argc, 1);
    void *mutex = janet_getabstract(argv, 0, &janet_mutex_type);
    janet_os_mutex_unlock(mutex);
    return argv[0];
}

static int rwlockgc(void *p, size_t size) {
    (void) size;
    janet_os_rwlock_deinit(p);
    return 0;
}

const JanetAbstractType janet_rwlock_type = {
    "core/rwlock",
    rwlockgc,
    JANET_ATEND_GC
};

JANET_CORE_FN(janet_cfun_rwlock,
              "(ev/rwlock)",
              "Create a new read-write lock to coordinate threads.") {
    janet_fixarity(argc, 0);
    (void) argv;
    void *rwlock = janet_abstract_threaded(&janet_rwlock_type, janet_os_rwlock_size());
    janet_os_rwlock_init(rwlock);
    return janet_wrap_abstract(rwlock);
}

JANET_CORE_FN(janet_cfun_rwlock_read_lock,
              "(ev/acquire-rlock rwlock)",
              "Acquire a read lock an a read-write lock.") {
    janet_fixarity(argc, 1);
    void *rwlock = janet_getabstract(argv, 0, &janet_rwlock_type);
    janet_os_rwlock_rlock(rwlock);
    return argv[0];
}

JANET_CORE_FN(janet_cfun_rwlock_write_lock,
              "(ev/acquire-wlock rwlock)",
              "Acquire a write lock on a read-write lock.") {
    janet_fixarity(argc, 1);
    void *rwlock = janet_getabstract(argv, 0, &janet_rwlock_type);
    janet_os_rwlock_wlock(rwlock);
    return argv[0];
}

JANET_CORE_FN(janet_cfun_rwlock_read_release,
              "(ev/release-rlock rwlock)",
              "Release a read lock on a read-write lock") {
    janet_fixarity(argc, 1);
    void *rwlock = janet_getabstract(argv, 0, &janet_rwlock_type);
    janet_os_rwlock_runlock(rwlock);
    return argv[0];
}

JANET_CORE_FN(janet_cfun_rwlock_write_release,
              "(ev/release-wlock rwlock)",
              "Release a write lock on a read-write lock") {
    janet_fixarity(argc, 1);
    void *rwlock = janet_getabstract(argv, 0, &janet_rwlock_type);
    janet_os_rwlock_wunlock(rwlock);
    return argv[0];
}

static JanetFile *get_file_for_stream(JanetStream *stream) {
    int32_t flags = 0;
    char fmt[4] = {0};
    int index = 0;
    if (stream->flags & JANET_STREAM_READABLE) {
        flags |= JANET_FILE_READ;
        janet_sandbox_assert(JANET_SANDBOX_FS_READ);
        fmt[index++] = 'r';
    }
    if (stream->flags & JANET_STREAM_WRITABLE) {
        flags |= JANET_FILE_WRITE;
        janet_sandbox_assert(JANET_SANDBOX_FS_WRITE);
        int currindex = index;
        fmt[index++] = (currindex == 0) ? 'w' : '+';
    }
    if (index == 0) return NULL;
    /* duplicate handle when converting stream to file */
    stream->flags &= ~JANET_STREAM_NODUPS;
#ifdef JANET_WINDOWS
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
    return janet_makejfile(f, flags);
}

JANET_CORE_FN(janet_cfun_to_file,
              "(ev/to-file)",
              "Create core/file copy of the stream. This value can be used "
              "when blocking IO behavior is needed.") {
    janet_fixarity(argc, 1);
    JanetStream *stream = janet_getabstract(argv, 0, &janet_stream_type);
    JanetFile *iof = get_file_for_stream(stream);
    if (iof == NULL) janet_panic("cannot make file from stream");
    return janet_wrap_abstract(iof);
}

JANET_CORE_FN(janet_cfun_ev_all_tasks,
              "(ev/all-tasks)",
              "Get an array of all active task fibers that are being used by the scheduler.") {
    janet_fixarity(argc, 0);
    (void) argv;
    JanetArray *array = janet_array(janet_vm.active_tasks.count);
    for (int32_t i = 0; i < janet_vm.active_tasks.capacity; i++) {
        if (!janet_checktype(janet_vm.active_tasks.data[i].key, JANET_NIL)) {
            janet_array_push(array, janet_vm.active_tasks.data[i].key);
        }
    }
    return janet_wrap_array(array);
}

void janet_lib_ev(JanetTable *env) {
    JanetRegExt ev_cfuns_ext[] = {
        JANET_CORE_REG("ev/give", cfun_channel_push),
        JANET_CORE_REG("ev/take", cfun_channel_pop),
        JANET_CORE_REG("ev/try-give", cfun_channel_try_push),
        JANET_CORE_REG("ev/try-take", cfun_channel_try_pop),
        JANET_CORE_REG("ev/full", cfun_channel_full),
        JANET_CORE_REG("ev/capacity", cfun_channel_capacity),
        JANET_CORE_REG("ev/count", cfun_channel_count),
        JANET_CORE_REG("ev/select", cfun_channel_choice),
        JANET_CORE_REG("ev/rselect", cfun_channel_rchoice),
        JANET_CORE_REG("ev/chan", cfun_channel_new),
        JANET_CORE_REG("ev/thread-chan", cfun_channel_new_threaded),
        JANET_CORE_REG("ev/ichan", cfun_channel_new_isolated),
        JANET_CORE_REG("ev/chan-close", cfun_channel_close),
        JANET_CORE_REG("ev/go", cfun_ev_go),
        JANET_CORE_REG("ev/thread", cfun_ev_thread),
        JANET_CORE_REG("thread/new", cfun_thread_new),
        JANET_CORE_REG("thread/join", cfun_thread_join),
        JANET_CORE_REG("thread/alive?", cfun_thread_alive),
        JANET_CORE_REG("thread/profile", cfun_thread_profile),
        JANET_CORE_REG("pool/spawn", cfun_pool_spawn),
        JANET_CORE_REG("pool/acquire", cfun_pool_acquire),
        JANET_CORE_REG("pool/dispatch", cfun_pool_dispatch),
        JANET_CORE_REG("pool/profile-harvest", cfun_pool_profile_harvest),
        JANET_CORE_REG("pool/stop", cfun_pool_stop),
        JANET_CORE_REG("ev/monitor", cfun_thread_monitor),
        JANET_CORE_REG("ev/demonitor", cfun_thread_demonitor),
        JANET_CORE_REG("ev/register", cfun_registry_register),
        JANET_CORE_REG("ev/whereis", cfun_registry_whereis),
        JANET_CORE_REG("ev/unregister", cfun_registry_unregister),
        JANET_CORE_REG("shared/new", cfun_shared_new),
        JANET_CORE_REG("shared/shareable?", cfun_shared_shareable),
        JANET_CORE_REG("shared/length", cfun_shared_length),
        JANET_CORE_REG("shared/get", cfun_shared_get),
        JANET_CORE_REG("shared/materialize", cfun_shared_materialize),
        JANET_CORE_REG("ev/give-supervisor", cfun_ev_give_supervisor),
        JANET_CORE_REG("ev/interrupt", cfun_ev_interrupt),
        JANET_CORE_REG("ev/timeslice", cfun_ev_timeslice),
        JANET_CORE_REG("ev/sleep", cfun_ev_sleep),
        JANET_CORE_REG("ev/deadline", cfun_ev_deadline),
        JANET_CORE_REG("ev/cancel", cfun_ev_cancel),
        JANET_CORE_REG("ev/close", janet_cfun_stream_close),
        JANET_CORE_REG("ev/read", janet_cfun_stream_read),
        JANET_CORE_REG("ev/chunk", janet_cfun_stream_chunk),
        JANET_CORE_REG("ev/write", janet_cfun_stream_write),
        JANET_CORE_REG("ev/lock", janet_cfun_mutex),
        JANET_CORE_REG("ev/acquire-lock", janet_cfun_mutex_acquire),
        JANET_CORE_REG("ev/release-lock", janet_cfun_mutex_release),
        JANET_CORE_REG("ev/rwlock", janet_cfun_rwlock),
        JANET_CORE_REG("ev/acquire-rlock", janet_cfun_rwlock_read_lock),
        JANET_CORE_REG("ev/acquire-wlock", janet_cfun_rwlock_write_lock),
        JANET_CORE_REG("ev/release-rlock", janet_cfun_rwlock_read_release),
        JANET_CORE_REG("ev/release-wlock", janet_cfun_rwlock_write_release),
        JANET_CORE_REG("ev/to-file", janet_cfun_to_file),
        JANET_CORE_REG("ev/all-tasks", janet_cfun_ev_all_tasks),
        JANET_REG_END
    };

    janet_core_cfuns_ext(env, NULL, ev_cfuns_ext);
    janet_register_abstract_type(&janet_stream_type);
    janet_register_abstract_type(&janet_channel_type);
    janet_register_abstract_type(&janet_mutex_type);
    janet_register_abstract_type(&janet_rwlock_type);
    janet_register_abstract_type(&janet_thread_handle_type);
    janet_register_abstract_type(&janet_pool_type);
    janet_register_abstract_type(&janet_shared_type);
}

#endif
