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

#ifndef JANET_STATE_H_defined
#define JANET_STATE_H_defined

#ifndef JANET_AMALG
#include "features.h"
#include <janet.h>
#include <stdint.h>
#endif

#ifdef JANET_EV
#ifdef JANET_WINDOWS
#include <windows.h>
#else
#include <pthread.h>
#endif
#endif

typedef int64_t JanetTimestamp;

typedef struct JanetScratch {
    JanetScratchFinalizer finalize;
    long long mem[]; /* for proper alignment */
} JanetScratch;

typedef struct {
    JanetGCObject *self;
    JanetGCObject *other;
    int32_t index;
    int32_t index2;
} JanetTraversalNode;

typedef struct {
    int32_t capacity;
    int32_t head;
    int32_t tail;
    void *data;
} JanetQueue;

#ifdef JANET_EV
typedef struct {
    JanetTimestamp when;
    JanetFiber *fiber;
    JanetFiber *curr_fiber;
    uint32_t sched_id;
    int is_error;
    int has_worker;
#ifdef JANET_WINDOWS
    HANDLE worker;
    HANDLE worker_event;
#else
    pthread_t worker;
#endif
} JanetTimeout;
#endif

/* Registry table for C functions - contains metadata that can
 * be looked up by cfunction pointer. All strings here are pointing to
 * static memory not managed by Janet. */
typedef struct {
    JanetCFunction cfun;
    const char *name;
    const char *name_prefix;
    const char *source_file;
    int32_t source_line;
    /* int32_t min_arity; */
    /* int32_t max_arity; */
} JanetCFunRegistry;

struct JanetVM {
    /* Place for user data */
    void *user;

    /* Top level dynamic bindings */
    JanetTable *top_dyns;

    /* Cache the core environment */
    JanetTable *core_env;

    /* How many VM stacks have been entered */
    int stackn;

    /* If this flag is true, suspend on function calls and backwards jumps.
     * When this occurs, this flag will be reset to 0. */
    volatile JanetAtomicInt auto_suspend;

    /* M4b-2 quantum driver: nonzero enables preemptive round-robin.
     * Each scheduled task run gets QUANTUM_BUDGET back-edge/call polls;
     * exhaustion suspends with INTERRUPT (transparent: NO_USEVAL /
     * NO_SKIP make resume continue exactly). auto_suspend stays purely
     * for external quiesce — a nonzero auto_suspend at delivery means
     * quiesce (unwind), otherwise quantum (re-queue). Per-VM,
     * default 0 (existing behavior exact). */
    int32_t quantum;
    int32_t quantum_remaining;

    /* Native executor stage 2 counters (per VM/thread). Lazy structural
     * attachment to actual runtime functions and VM-boundary exits. */
    uint64_t native_attach_attempts;
    uint64_t native_attach_attached;
    uint64_t native_attach_ambiguous;
    uint64_t native_attach_nomatch;
    uint64_t native_vm_exits;
    uint64_t native_exit_opcodes[256];
#ifdef JANET_NATIVE_EXECUTORS
    /* A single predictable dispatch gate when no descriptor has been
     * published or explicitly attached in this VM. Even a runtime built
     * with executor support must not pay registry/shape checks in a
     * completely ordinary interpreter/AOT program. */
    int native_possible;
#endif
    /* Validation epoch for cached whole-definition native verdicts.
     * Bumped on every VM resume entry, debugger breakpoint write and
     * descriptor publication; a def's cached verdict is usable only at
     * the epoch it was proven at (see JanetFuncDef.native_verified). */
    uint64_t native_epoch;

    /* The current running fiber on the current thread.
     * Set and unset by functions in vm.c */
    JanetFiber *fiber;
    JanetFiber *root_fiber;

    /* Context saved in host code when it resumes a fiber and we switch onto
     * that fiber's native stack (only used when janet_vm.fiber is NULL --
     * resumer inside a fiber keeps the context in that fiber's ctx). */
    JanetFiberCtx base_ctx;
    /* Parked VM dynamic registers for host code, parallel to base_ctx: when
     * host code resumes a fiber, its live registers are saved here, and a
     * suspending fiber loads them back before switching out (Phase 1.1). */
    jmp_buf *base_signal_buf;
    Janet *base_return_reg;
    int32_t base_coerce_error;
    int32_t base_stackn;
    int32_t base_gc_suspend;

    /* The current pointer to the inner most jmp_buf. The current
     * return point for panics. */
    jmp_buf *signal_buf;
    Janet *return_reg;
    int coerce_error;

    /* The global registry for c functions. Used to store meta-data
     * along with otherwise bare c function pointers. */
    JanetCFunRegistry *registry;
    size_t registry_cap;
    size_t registry_count;
    int registry_dirty;

    /* Registry for abstract types that can be marshalled.
     * We need this to look up the constructors when unmarshalling. */
    JanetTable *abstract_registry;

    /* B7.1 named process registry: keyword name -> core/proc handle
     * (erlang:register/2, whereis/1, unregister/1). Lazily allocated --
     * NULL means "nothing named yet", behaviorally identical to an
     * empty table, and most programs never name a process (the same
     * discipline as B6's lazy mailbox and B3's lazy stash/monitors).
     * Owned by proc.c; it is a naked C pointer, so gc.c must mark it
     * as a C root at every root-set site that already marks top_dyns
     * and profile_table -- see janet_proc_registry() in proc.c. */
    JanetTable *proc_registry;

    /* Immutable value cache */
    const uint8_t **cache;
    uint32_t cache_capacity;
    uint32_t cache_count;
    uint32_t cache_deleted;
    uint8_t gensym_counter[8];

    /* PGO type-profile collector (Phase 1): when profile_table is
     * non-NULL, the VM records observed param/return types per function
     * (collector lives in vm.c). The table is marked as a root while
     * active. */
    JanetTable *profile_table;

    /* Garbage collection */
    void *blocks;
    void *weak_blocks;
    /* P3.5b deferred death-migration chains (see gc.c): parked here
     * by fiber deinit mid-sweep, drained into the globals after all
     * lists are walked. Immediate splicing would race the in-progress
     * unlink and orphan chains (counted-but-unlisted). Per-VM
     * (thread-safe); NULL outside sweeps. */
    void *sweep_pending[3];
    /* Phase 1.1 quarantine: blocks freed this cycle wait here INTACT
     * (no deinit, no release) until the next full collect's weak-drop
     * has judged weak-table entries against their pre-free mark state.
     * Freeing immediately lets a recycled slot alias a weak entry's
     * referent: the entry then reads the NEW occupant's mark state and
     * survives with a dead referent (suite-ev2 count-uncollected = 100
     * under the epoch mark). Per-VM; drained after each full collect's
     * registry pass, or early when over capacity (the poison+guards
     * from 3dbb2e0b make an early-drained, deinit'd header safe for
     * the weak-drop's guarded reads). */
    void *gc_quarantine[3];
    size_t gc_quarantine_count;
    /* Immutable-string heap (Phase 2b-ii): strings, symbols and keywords
     * live on their own block list so collection policy can treat them
     * independently of everything else. Sweep semantics are identical to
     * the main heap for now; see janet_sweep_list. */
    void *string_blocks;
    size_t gc_interval;
    size_t next_collection;
    size_t block_count;
    int gc_suspend;
    int gc_mark_phase;
    /* Generational collection (Phase 2b-iii policy): minor collections run
     * while gc_minor_phase is set (see janet_collect_minor); young_bytes
     * counts slab-carved bytes since the last minor, triggering one at
     * gc_minor_threshold. */
    int gc_minor_phase;
    size_t gc_young_bytes;
    size_t gc_minor_threshold;
    /* Threaded-abstract visit cycle (Erlang-parity residual O(n) fix,
     * see gc.c's janet_threaded_visited): the threaded_abstracts
     * registry's "visited this collection" marker is a cycle stamp,
     * not a boolean, so no post-minor reset pass over the registry is
     * needed (the old pass walked the whole table -- O(registry
     * capacity) ~= O(channels ever registered) -- after every single
     * fiber minor, the dominant residual O(n)-per-minor term at
     * bench/agents.janet N=100000, 97% of sample-profile samples).
     * Bumped once at the start of every collection, alongside
     * janet_gc_epoch_next. */
    uint32_t gc_visit_cycle;
    /* Step 1 (process heaps): monotonic owner-id source for fibers.
     * 0 is reserved (shared/unowned); wraps past 4B fibers (reissue
     * is accounting-only noise, never a dangling pointer). */
    uint32_t gc_owner_next;
    /* Step P1 (proc processes): monotonic pid-serial source. 0 is
     * reserved (never issued: wrap skips it, like gc_owner). */
    uint32_t proc_serial;
    /* P3.3 gate: set once a strict fiber exists in this VM (never
     * cleared — conservative). The barrier's export check runs only
     * past this flag, so legacy code pays one predicted branch. */
    int heap_strict_live;
    /* P3.6 shared lane stack: one native mapping per VM, lazily
     * created on the first shared resume (NULL until then, so
     * non-users map nothing). Same reserve/commit/grow shape as
     * private stacks; faults attribute to the running fiber. */
    void *shared_stack_base;
    void *shared_stack;
    size_t shared_stack_size;
    void *shared_stack_committed;
    /* P3.6 gate: ev/go + proc spawn opt fibers in when set (from
     * JANET_SHARED_STACKS in the environment, read once at init). */
    int shared_stacks_on;
    /* P3.5c allocation attribution: when non-NULL, slab/gcalloc births
     * attribute to this fiber (owner id, strict bit, nursery, filing)
     * instead of the running fiber. Set during strict/isolated spawn
     * construction so a process's memory is born owned (per-proc
     * pressure + narrow sweeps cover it; globals stay quiet). Cleared
     * in a janet_try/finally (panics longjmp past straight-line
     * clears, which would strand a dangling override). */
    struct JanetFiber *gc_alloc_ctx;

    /* GC roots */
    Janet *roots;
    size_t root_count;
    size_t root_capacity;
    /* Set during janet_clear_memory: threaded teardowns must run
     * inline (posting to the loop would block on a dead self-pipe). */
    int gc_teardown_inline;
    /* Identity in the process-wide threaded-abstract home-VM registry. */
    uint64_t abstract_home_generation;
    /* Lane-VM marker (proc.c lane thread body). Lane-homed threaded
     * abstracts are pinned until lane stop instead of refcount-freed:
     * pid values circulate in queues/messages with no claims, and a
     * mid-life free dangles them (BUG 2 density janet_equals/hash
     * class). Quarantine is a list of JanetAbstractHead linked via
     * gc.data.next. */
    int is_lane_vm;
    void *home_quarantine;

    /* Shadow root stack for generated/C code (Phase 2b-i). Strictly LIFO;
     * everything below shadow_count is marked on every collection. Backed
     * by raw memory (never triggers collections on growth). */
    Janet *shadow_roots;
    size_t shadow_count;
    size_t shadow_capacity;

    /* Slab allocator state (Phase 2b-iii): per-VM, gc.c-private.
     * NULL until the first slab-sized allocation. */
    void *slab_state;

    /* Active compilations for GC marking (Phase 2a root discipline).
     * Compilers stash values in C structs (scope consts/syms/defs) that
     * the collector cannot see; janet_collect walks this chain (most
     * recent first) via janet_mark_compiler. Pushed/popped by
     * janetc_init/janetc_deinit; entries point to C-stack state of live
     * compilations. */
    struct JanetCompiler *compiler_stack;

    /* Scratch memory */
    JanetScratch **scratch_mem;
    size_t scratch_cap;
    size_t scratch_len;

    /* Sandbox flags */
    uint32_t sandbox_flags;

    /* Random number generator */
    JanetRNG rng;

    /* Traversal pointers */
    JanetTraversalNode *traversal;
    JanetTraversalNode *traversal_top;
    JanetTraversalNode *traversal_base;

    /* Thread safe strerror error buffer - for janet_strerror */
#ifndef JANET_WINDOWS
    char strerror_buf[256];
#endif

    /* Event loop and scheduler globals */
#ifdef JANET_EV
    size_t tq_count;
    size_t tq_capacity;
    JanetQueue spawn;
    JanetTimeout *tq;
    JanetRNG ev_rng;
    volatile JanetAtomicInt listener_count; /* used in signal handler, must be volatile */
    JanetTable threaded_abstracts; /* All abstract types that can be shared between threads (used in this thread) */
    JanetTable active_tasks; /* All possibly live task fibers - used just for tracking */
    JanetTable signal_handlers;
#ifdef JANET_WINDOWS
    void **iocp;
    void *connect_ex; /* MSWsock extension if available */
    int connect_ex_loaded;
#elif defined(JANET_EV_EPOLL)
    pthread_attr_t new_thread_attr;
    JanetHandle selfpipe[2];
    int epoll;
    int timerfd;
    int timer_enabled;
#elif defined(JANET_EV_KQUEUE)
    pthread_attr_t new_thread_attr;
    JanetHandle selfpipe[2];
    int kq;
    int timer;
    int timer_enabled;
#else
    JanetStream **streams;
    size_t stream_count;
    size_t stream_capacity;
    pthread_attr_t new_thread_attr;
    JanetHandle selfpipe[2];
    struct pollfd *fds;
#endif
#endif

};

extern JANET_THREAD_LOCAL JanetVM janet_vm;

/* PGO type-profile collector (implemented in vm.c). */
void janet_profile_start(void);
/* VM-local AOT auto-PGO destination; never exported to child processes. */
JANET_API void janet_profile_auto_output(const char *path);
Janet janet_profile_stop(void);
void janet_profile_reset(void);
void janet_profile_dump(const char *path);
JanetArray *janet_profile_rows(void);
void janet_profile_task_call(JanetFunction *func, const Janet *argv, int32_t argc);

/* M4b-2 quantum driver: back-edge/call polls granted per task run.
 * One poll ≈ one loop iteration or call; 200 keeps scheduling
 * overhead near 1% while preempting hogs within microseconds. */
#ifndef JANET_QUANTUM_BUDGET
#define JANET_QUANTUM_BUDGET 200
#endif

#ifdef JANET_NET
void janet_net_init(void);
void janet_net_deinit(void);
#endif

#ifdef JANET_EV
void janet_abstract_home_register(void);
void janet_abstract_home_unregister(void);
void janet_ev_init(void);
void janet_ev_deinit(void);
#endif

#endif /* JANET_STATE_H_defined */
