/* Proc processes (Step P1): first-class M:N-process objects for the
 * Erlang-like process epic. Tasklets stay untouched (convenience
 * layer over fibers); procs are the dedicated mechanism underneath:
 * one GC object per process (mailbox + fiber + supervisor channel +
 * serial), C API ready for lanes (P2) and heap attribution (P3).
 *
 * P1.1 scope: same-lane raw processes only (spawn/send/recv/self/
 * kill/alive?). Mailbox is an isolated channel (copy semantics, home
 * pin — Step 1.4/4.1 ride along free). Marshal refuses (same-VM only,
 * like pool handles; cross-lane proxies land in P2). No selective
 * receive yet (take-next only), no loop/handlers yet (raw bodies),
 * no registry yet. */

#ifndef BENDER_AMALG
#include "features.h"
#include <stdio.h>
#include <bender.h>
#include "util.h"
#include "gc.h"
#include "state.h"
#include "fiber.h"
#include "ev.h"
#include "ev.h"
#include "proc.h"
#endif

/* P-lanes platform includes: OUTSIDE the BENDER_AMALG guard — the
 * amalgam skips per-file includes but still needs these. */
#ifdef BENDER_APPLE
#include <mach/mach.h>
#include <mach/thread_act.h>
#include <mach/thread_policy.h>
#endif
#if defined(BENDER_LINUX) || defined(_GNU_SOURCE)
#include <pthread.h>
#include <sched.h>
#else
#include <pthread.h>
#endif

#ifdef BENDER_EV

/* Forward: abstract type defined below (used by the getter). */
extern const BenderAbstractType bender_proc_type;
/* Forward: pid-tuple resolver (defined with proc/pid, used by send). */
static BenderChannel *bender_proc_resolve_mbox(Bender x);
static int bender_proc_is_pid_tuple(Bender x);
static Bender cfun_proc_spawn_impl(BenderFunction *func, int32_t cap, int isolated);
static uint32_t bender_proc_next_serial(void);

/* ===== P-lanes: VM-per-core proc parallelism =====
 * N resident OS threads, each a full share-nothing VM (the ev/thread
 * body), each running a proc lane: a dispatcher that owns local procs
 * and services spawn/deliver requests marshaled from the main VM.
 * Per-proc heaps compose with per-VM collectors, so cores never
 * contend on a global GC — the BEAM shape. Lane 0 is the main VM;
 * workers are 1..N. Cross-lane handles are pid tuples
 * [:pid lane serial ctl] (resolve_mbox already speaks tuples; the
 * node field disambiguates lanes). */
static BENDER_THREAD_LOCAL BenderChannel **bender_lane_ctls;
static BENDER_THREAD_LOCAL BenderChannel *bender_lane0_ctl;
static BENDER_THREAD_LOCAL int32_t bender_lane_count;
static BENDER_THREAD_LOCAL int32_t bender_lane_rr;

/* Milestone 2c: work-stealing. The overflow queue is a global
 * threaded channel owned by the main VM (created at lanes-start,
 * rooted like lane0_ctl). Routed spawns under the :steal policy push
 * to it when the target ctl is at high water; dispatchers drain it
 * (up to K tasks) before parking on an empty ctl. Policy:
 * 0 = :round-robin (default), 1 = :least-loaded (reserved, behaves
 * as round-robin), 2 = :steal. Counters are the feature indication:
 * lanes-stats reports pushes/pulls so tests assert stealing actually
 * happened. */
static BENDER_THREAD_LOCAL BenderChannel *bender_lane_overflow;
static BENDER_THREAD_LOCAL int32_t bender_lane_policy;
static volatile BenderAtomicInt bender_lane_overflow_pushed;
static volatile BenderAtomicInt bender_lane_overflow_pulled;

/* Milestone 2a: pool-style join. Workers launch on JOINABLE pthreads,
 * each with a completion sync. lanes-stop asks every dispatcher to
 * exit, wakes locally-parked procs (best effort), waits a bounded
 * grace per worker, joins the ones that finished and detaches
 * stragglers. A joined worker drops its launch refcount, so a script
 * that stops its lanes can reach a clean loop exit (the AOT
 * exit-hang). Straggler workers keep the process alive exactly like
 * the milestone-1 detached workers did. */
typedef struct BenderLaneLaunch {
    pthread_t thread;
    pthread_mutex_t lock;
    pthread_cond_t cond;
    int exited;
    BenderBuffer *buf;
    uint32_t sandbox_flags;
    int32_t lane_id;
} BenderLaneLaunch;

static BENDER_THREAD_LOCAL BenderLaneLaunch **bender_lane_launches;

/* Grace per worker lane when stopping: procs parked in a single recv
 * wake and finish quickly; procs in long loops hit the grace and are
 * detached instead. */
#define BENDER_LANE_STOP_GRACE_SECONDS 2

static void *bender_lane_thread_body(void *p) {
    bender_vm.is_lane_vm = 1;
    BenderLaneLaunch *L = p;
    bender_vm.lane_id = L->lane_id;
    BenderEVGenericMessage args;
    memset(&args, 0, sizeof(args));
    args.tag = 0x1 /* :n — the caller never awaits us */
        | 0x8 /* BENDER_THREAD_NODEINIT_FLAG — see ev.c */;
    args.argi = L->sandbox_flags;
    args.argp = L->buf;
    args.fiber = NULL;
    bender_go_thread_subr(args);
    pthread_mutex_lock(&L->lock);
    L->exited = 1;
    pthread_cond_broadcast(&L->cond);
    pthread_mutex_unlock(&L->lock);
        bender_vm.is_lane_vm = 0;
        bender_vm.lane_id = 0;
return NULL;
}

/* P-lanes: with lanes active, route round-robin to a worker VM. The
 * entry marshals over a threaded channel; the handle is a pid tuple
 * [:pid lane serial ctl] (resolve_mbox speaks tuples, so proc/send
 * works unchanged). Local spawns (proc/spawn-local, lane-internal
 * spawns) call the impl directly. */
static int32_t bender_os_cpu_count(void);
static int32_t bender_lanes_start_impl(int32_t count, int affinity);

/* Lazily start the multi-core runtime the first time a proc is spawned.
 *
 * The lane machinery only activates once lanes exist, so without this a plain
 * proc/spawn loop silently uses one core forever while Go and Erlang start
 * their schedulers at boot. Autostart lives HERE rather than in boot.ben
 * because boot.ben also runs during image generation, where spawning lanes
 * hangs the build (observed: bender_boot stuck in the lane join).
 *
 * Opt out with BENDER_LANES=0; pin a count with BENDER_LANES=<n>. Each VM
 * autostarts once; a failed start leaves lanes at 0 rather than erroring, so a
 * sandboxed embedder that forbids threads keeps working single-threaded. */
static void bender_lanes_maybe_autostart(void) {
    static BENDER_THREAD_LOCAL int attempted = 0;
    if (attempted || bender_lane_count > 0) return;
    attempted = 1;
    /* OPT-IN for now, not default-on.
     *
     * Autostart is deliberately gated behind BENDER_LANES=<n> because turning it
     * on by default changes proc/spawn's RETURN TYPE from a core/proc object to
     * a [:pid lane serial ctl] tuple for every caller (measured: it breaks
     * suite-proc's "spawn returns a proc"). That is a real API break, and it
     * must not happen silently to code written against the single-VM shape.
     *
     * =0 keeps the historic single-VM behaviour, which is also what the test
     * suite assumes. Once proc handles are uniform across lanes (a local proc
     * proxy, or a pid tuple that satisfies proc/pid?), the default can flip. */
    const char *raw = getenv("BENDER_LANES");
    if (NULL == raw || 0 == strcmp(raw, "0")) return;
    int32_t count = -1;
    {
        long v = strtol(raw, NULL, 10);
        if (v > 0) count = (int32_t) v;
    }
    if (count < 0) {
        count = bender_os_cpu_count() - 1;
        if (count < 1) count = 1;
    }
    if (count > 64) count = 64;
    /* The worker dispatcher comes from boot; without it we cannot run lanes. */
    {
        Bender w = bender_wrap_nil();
        if (NULL != bender_vm.root_fiber && NULL != bender_vm.root_fiber->env) {
            w = bender_table_get(bender_vm.root_fiber->env,
                    bender_ckeywordv("proc-lane-worker"));
        }
        if (bender_checktype(w, BENDER_NIL) && NULL != bender_vm.top_dyns) {
            w = bender_table_get(bender_vm.top_dyns,
                    bender_ckeywordv("proc-lane-worker"));
        }
        if (!bender_checktype(w, BENDER_FUNCTION)) return;
    }
    (void) bender_lanes_start_impl(count, 0);
}

/* Does this captured value denote mutable cross-lane state?
 *
 * Remote spawn marshals the closure, so a captured array/table/buffer/fiber (or
 * a plain channel) becomes a detached copy in the worker: mutations the worker
 * makes are invisible to the spawner and vice versa. That is silent, which is
 * how it broke suite-proc's "body ran". Detect it so the spawn can say so
 * instead of quietly doing the wrong thing.
 *
 * Immutable values (nil/bool/number/string/symbol/keyword/tuple-of-these) are
 * fine to copy. Threaded channels and abstract handles are explicit carriers,
 * so they are allowed too. */
static int bender_proc_capture_is_mutable(Bender x) {
    switch (bender_type(x)) {
        case BENDER_ARRAY:
        case BENDER_TABLE:
        case BENDER_BUFFER:
        case BENDER_FIBER:
        case BENDER_STRUCT:
            return 1;
        default:
            return 0;
    }
}

/* Return the index of the first mutable capture in `func`'s environments, or
 * -1 when the closure captures only copy-safe values. */
static int32_t bender_proc_find_mutable_capture(BenderFunction *func) {
    if (NULL == func || NULL == func->def) return -1;
    int32_t numenvs = func->def->environments_length;
    int32_t slot = 0;
    for (int32_t e = 0; e < numenvs; e++) {
        BenderFuncEnv *env = func->envs[e];
        if (NULL == env) continue;
        int32_t len = env->length;
        for (int32_t i = 0; i < len; i++) {
            Bender v;
            if (env->offset > 0 && NULL != env->as.fiber) {
                v = env->as.fiber->data[env->offset + i];
            } else if (NULL != env->as.values) {
                v = env->as.values[i];
            } else {
                continue;
            }
            if (bender_proc_capture_is_mutable(v)) return slot;
            slot++;
        }
    }
    return -1;
}

/* Dependency-aware placement: a closure that captures a pid tuple is
 * usually spawning a child that will communicate with that pid. Keep the
 * child on the captured pid's lane, like OTP placing a spawned process on
 * the current scheduler. Round-robin remains for independent spawns. */
static int32_t bender_proc_find_pid_lane(BenderFunction *func) {
    if (NULL == func || NULL == func->def) return 0;
    int32_t numenvs = func->def->environments_length;
    for (int32_t e = 0; e < numenvs; e++) {
        BenderFuncEnv *env = func->envs[e];
        if (NULL == env) continue;
        int32_t len = env->length;
        for (int32_t i = 0; i < len; i++) {
            Bender v;
            if (env->offset > 0 && NULL != env->as.fiber) {
                v = env->as.fiber->data[env->offset + i];
            } else if (NULL != env->as.values) {
                v = env->as.values[i];
            } else {
                continue;
            }
            if (!bender_checktype(v, BENDER_TUPLE)) continue;
            const Bender *tup = bender_unwrap_tuple(v);
            if (4 != bender_tuple_length(tup)) continue;
            if (!bender_checktype(tup[0], BENDER_KEYWORD) ||
                    bender_unwrap_keyword(tup[0]) !=
                    bender_unwrap_keyword(bender_ckeywordv("pid"))) continue;
            if (!bender_checktype(tup[1], BENDER_NUMBER)) continue;
            int32_t lane = (int32_t) bender_unwrap_number(tup[1]);
            if (lane > 0 && lane <= 64) return lane;
        }
    }
    return 0;
}

/* ---- same-lane routed spawn batching ------------------------------------
 * One lane-control message per spawn was costing ~8.5 us/spawn. Split by what
 * each part is charged PER (tools/push-split-probe.patch on
 * bench/spawn-only-chain.ben, N=200,000):
 *
 *   bender_loop1_poll_now drain                 3.22  us/spawn  per message
 *   push remainder (lock, queue, pending read)  ~1.0  us/spawn  per message
 *   bender_ev_post_event pipe post              0.119 us/spawn  per message
 *   bender_chan_pack closure marshal            1.089 us/spawn  per CLOSURE
 *
 * ~4.3 us is per-MESSAGE and amortises with the batch factor; the marshal is
 * per-closure and does not. Target ~4.2-4.5 us/spawn, about 2x.
 *
 * NOT the rejected "drain less often" change, which cut the drain's frequency
 * while event production stayed at one per spawn and merely moved cost into
 * push. This cuts event PRODUCTION.
 *
 * THE MARSHAL IS EAGER, and that is load-bearing, not an optimisation. The
 * entry carries a pre-marshalled BUFFER, not a live function. A batch holding
 * bender_wrap_function(func) would defer the marshal to flush time, and the
 * marshal is what SNAPSHOTS captured values -- so every batched closure would
 * observe a later value of anything the spawner mutates afterwards. Measured:
 * five procs closing over the spawner's loop variable returned [1 4 9 16 25]
 * instead of [0 1 4 9 16]. A semantic bug, not a performance detail. Eager
 * marshalling also forfeits marsh.c's intra-message seen_defs dedup, so the
 * funcdef is copied per proc and footprint does not improve; that trade is
 * mandatory.
 *
 * THREE flush obligations, each for a different reason:
 *
 *  ORDERING -- the lane ctl is FIFO and the dispatcher resolves a delivery
 *  with (get serials serial) under an if-let with NO else branch, so a
 *  :deliver, :proc-fwd, :kill or :proc-monitor reaching the lane BEFORE the
 *  :spawn that creates its serial is dropped SILENTLY. Flushed from
 *  bender_channel_push: one choke point no future caller can bypass.
 *
 *  LIVENESS -- a program that spawns fewer than the batch limit and then waits
 *  has no size or ordering trigger, and its procs would never reach the lane.
 *  Flushed from bender_await, which is the single path every event wait takes
 *  and which a spawn burst never reaches. Loop-level predicates were tried and
 *  all failed: at the top of bender_loop1 and at its idle point they fire
 *  throughout a burst (the main VM's run queue is legitimately empty because
 *  the procs sit on a lane), collapsing the average batch to ~1 and making
 *  200,000 spawns 7,224 ms -- worse than no batching.
 *
 *  BACKPRESSURE -- a spawn into a FULL ctl must SUSPEND the calling fiber
 *  (test/suite-proc-spawn-return.ben installs a gated worker, fills the ctl
 *  and asserts :suspended). Only the :proc-spawn-write plan return can do
 *  that, so a full ctl bypasses batching entirely and takes the original
 *  single-spawn path verbatim.
 *
 * Also bypassed under :steal: batching leaves a ctl message queued, which
 * makes bender_channel_count(ctl) >= 1 true and activates that policy's
 * never-exercised overflow branch, which does not work (its mode-2 push hands
 * the pulling lane a corrupted task). Repairing :steal is separate work. */
/* OPT-IN, default OFF, via BENDER_SPAWN_BATCH=<n>. Batching is a measured
 * TRADE, not a free win, so the default must not impose it:
 *
 *   N=100,000   spawn 840 -> 249 ms (3.4x)   wave 146 -> 397 ms (2.7x WORSE)
 *               total 986 -> 647 ms (1.5x)   RSS 328 -> 362 MB (+10%)
 *   N=200,000   spawn 1448 -> 701 ms (2.1x)  wave 320 -> 715 ms (2.2x WORSE)
 *               total 1768 -> 1416 ms (1.25x) RSS 663 -> 728 MB (+10%)
 *
 * Matched alternating runs, min of 2 by total. Total improves, but the WAVE
 * regresses more than spawn gains in steady state, and a game loop spends its
 * life in the wave, not in spawn. So: enable it for mass-spawn workloads,
 * leave it off where message throughput dominates.
 *
 * The likely mechanism for the wave regression (consistent with the +10% RSS
 * but NOT directly measured): the eager marshal forfeits marsh.c's
 * intra-message seen_defs dedup, so every proc gets its own funcdef copy
 * instead of sharing one, and the wave then executes N distinct bytecode
 * copies rather than one hot one. If that is confirmed, sharing the funcdef
 * across a batch while keeping per-entry eager env snapshots would get both
 * wins -- that is the next step, not a bigger batch. */
#define BENDER_LANE_CTL_CAP 4096

static BENDER_THREAD_LOCAL int32_t bender_spawn_batch_max = -1;

static int32_t bender_spawn_batch_limit(void) {
    if (bender_spawn_batch_max < 0) {
        const char *e = getenv("BENDER_SPAWN_BATCH");
        int32_t v = (NULL != e) ? (int32_t) strtol(e, NULL, 10) : 1;
        if (v < 1) v = 1;
        if (v > 64) v = 64;
        bender_spawn_batch_max = v;
    }
    return bender_spawn_batch_max;
}

static BENDER_THREAD_LOCAL BenderChannel *bender_spawn_batch_ctl = NULL;
static BENDER_THREAD_LOCAL BenderArray *bender_spawn_batch = NULL;
static BENDER_THREAD_LOCAL int bender_spawn_batch_flushing = 0;

/* A TUPLE SNAPSHOT of the entries, never the live array: the array is cleared
 * as soon as the push is accepted, and a message still aliasing it would carry
 * zero entries, silently losing every spawn in the batch. */
static Bender bender_spawn_batch_message(void) {
    Bender entries = bender_wrap_tuple(
            bender_tuple_n(bender_spawn_batch->data, bender_spawn_batch->count));
    Bender m[4];
    m[0] = bender_ckeywordv("spawn-batch");
    m[1] = bender_wrap_number(0);
    m[2] = bender_wrap_number(0);
    m[3] = entries;
    return bender_wrap_tuple(bender_tuple_n(m, 4));
}

/* Push the pending batch. Never parks, never longjmps.
 *
 * MODE 3, not 2. Mode 3 is the try-push: on a full queue it reports without
 * buffering AND calls bender_chan_unpack, so a refused attempt leaves no
 * unreconciled marshal buffer. Mode 2 is choice-write (select semantics) whose
 * pushes ignore their return value; used as a try-push it left the lane's
 * unmarshalled copy pointing into a dead marshal buffer and the next
 * collection died in bender_mark_fiber on a garbage abstract type pointer.
 *
 * A batch cannot use the plan return (its other pids are already with the
 * program, so there is no frame to defer to) and bender_await is a longjmp, so
 * backpressure here is drain-and-retry. It terminates because the target lane
 * consumes ctl on its own thread. The message is gcrooted across the retry:
 * bender_loop1_poll_now can collect and it is otherwise reachable only from
 * this C local. */
static void bender_spawn_batch_push(void) {
    if (NULL == bender_spawn_batch || 0 == bender_spawn_batch->count) return;
    BenderChannel *ctl = bender_spawn_batch_ctl;
    if (NULL == ctl) { bender_spawn_batch->count = 0; return; }
    Bender msg = bender_spawn_batch_message();
    bender_gcroot(msg);
    bender_spawn_batch_flushing = 1;
    while (bender_channel_push(ctl, msg, 3)) {
        bender_loop1_poll_now();
    }
    bender_spawn_batch_flushing = 0;
    bender_gcunroot(msg);
    bender_spawn_batch->count = 0;
    bender_spawn_batch_ctl = NULL;
}

/* Flush a batch pending for this ctl, or for ANY ctl when chan is NULL (the
 * liveness call from bender_await). Cheap and non-reentrant. */
void bender_proc_spawn_batch_flush_for(void *chan) {
    if (bender_spawn_batch_flushing) return;
    if (NULL == bender_spawn_batch_ctl) return;
    if (NULL != chan && chan != (void *) bender_spawn_batch_ctl) return;
    bender_spawn_batch_push();
}

static Bender bender_proc_spawn_routed(BenderFunction *func, int32_t cap) {
    bender_lanes_maybe_autostart();
    if (0 == bender_lane_count) return cfun_proc_spawn_impl(func, cap, 0);
    /* Crossing a lane boundary marshals the closure, so a MUTABLE capture
     * would arrive as a detached copy: the worker's writes would be invisible
     * to the spawner. Rather than erroring, place such a proc on THIS VM.
     *
     * That is the semantically correct choice, not a compromise: capturing a
     * mutable value is how a caller says "this process shares state with me",
     * and only a same-VM spawn honours it. The proc still runs concurrently
     * with the spawner (same VM, cooperative scheduling), and every proc that
     * does NOT capture mutable state still distributes across lanes. So a
     * program mixing shared-state workers with independent workers uses all
     * cores and stays correct, with nothing to learn or opt into.
     *
     * BENDER_PROC_SHARE_TRACE=1 reports the decision for each spawn, since the
     * placement is otherwise invisible. */
    if (bender_proc_find_mutable_capture(func) >= 0) {
        if (NULL != getenv("BENDER_PROC_SHARE_TRACE")) {
            fprintf(stderr, "[proc-spawn] shared capture -> local placement "
                            "(a lane would marshal the closure and detach it)\n");
        }
        return cfun_proc_spawn_impl(func, cap, 0);
    }
    /* OTP placement: a process spawning from a scheduler/lane keeps the
     * child on that lane. No closure marshal, no control-channel round
     * trip. Cross-lane distribution remains an explicit policy/API. */
    if (bender_vm.is_lane_vm && bender_vm.lane_id > 0) {
        return cfun_proc_spawn_impl(func, cap, 0);
    }
    int32_t preferred = bender_proc_find_pid_lane(func);
    int32_t lane = (preferred > 0 && preferred <= bender_lane_count)
        ? (preferred - 1)
        : (bender_lane_rr % bender_lane_count);
    if (preferred <= 0) bender_lane_rr++;
    BenderChannel *ctl = bender_lane_ctls[lane];
    uint32_t serial = bender_proc_next_serial();
    /* Bulk fan-out flow control.
     *
     * Each cross-lane spawn posts an event to the target lane's self-pipe,
     * which holds only ~341 events (16 KiB / 48 bytes). A tight spawn loop
     * that never enters the event loop fills it and the poster aborts with
     * "failed to write event to self-pipe" -- measured at 30000 procs across 4
     * lanes. The poster cannot block waiting for the pipe (that inverts the
     * channel lock, observed as a live deadlock), so the CALLER drains: every
     * 4 routed spawns we sample ready I/O and deliver whatever has queued.
     * One non-blocking poll per 4 spawns is noise next to the marshal cost.
     *
     * This raises the measured ceiling rather than removing it: the pipe holds
     * ~341 events and macOS offers no way to enlarge it (no F_SETPIPE_SZ), so at
     * some process count the post rate still outruns the drain. Measured on a
     * 4-lane burst returning results: 50000 and 80000 complete, 100000 aborts;
     * with 2 lanes even 80000 aborts. Making this unbounded needs real
     * backpressure in the lane dispatcher, not a bigger drain frequency. */
    {
        static BENDER_THREAD_LOCAL uint32_t spawn_tick = 0;
        /* The drain interval tracks EVENT PRODUCTION, not spawn count. With
         * batching one message covers BENDER_SPAWN_BATCH_MAX spawns, so 16
         * reproduces the pipe-event rate that one-per-spawn had at 4. Under
         * :steal batching is bypassed, production stays at one per spawn, and
         * a wider interval starves the self-pipe -- a hang, not a slowdown
         * (suite-lane-steal.ben catches it). */
        uint32_t tick_mask = (bender_spawn_batch_limit() > 1 &&
                              2 != bender_lane_policy) ? 15u : 3u;
        if (0 == (++spawn_tick & tick_mask)) {
            bender_spawn_batch_push();
            bender_loop1_poll_now();
        }
    }
    Bender msg[4];
    msg[0] = bender_ckeywordv("spawn");
    msg[1] = bender_wrap_number((double) serial);
    msg[2] = bender_wrap_number(cap);
    msg[3] = bender_wrap_function(func);
    Bender spawn_msg = bender_wrap_tuple(bender_tuple_n(msg, 4));
    /* Milestone 2c: :steal policy offloads to the overflow queue when
     * the target ctl is at high water (3/4 capacity). The task runs
     * wherever a lane pulls it; the spawner's pid tuple keeps the
     * spawn-time lane (documented: sends via that tuple may hit
     * :noproc for pulled procs -- communicate via closures or the
     * proc's own self-pid). Try-mode never blocks the spawner. */
    if (2 == bender_lane_policy && NULL != bender_lane_overflow &&
            bender_channel_count(ctl) >= 1) {
        int pr = bender_channel_push(bender_lane_overflow, spawn_msg, 2);
        /* Try-mode convention: 0 = refused (queue full), nonzero =
         * enqueued. (Mode 0 is the opposite: 0 = delivered now,
         * nonzero = the caller must await the parked push.) */
        if (pr != 0) {
        bender_atomic_inc(&bender_lane_overflow_pushed);
        /* Wakeup: a dispatcher that just parked on an empty ctl will
         * not see the overflow push (pull happens before parking).
         * Nudge the next lane to drain the queue; it keeps pulling
         * until the overflow is empty. */
        if (bender_lane_count > 0) {
            BenderChannel *wake = bender_lane_ctls[
                    (bender_lane_rr + 1) % bender_lane_count];
            if (NULL != wake) {
                Bender wup[1];
                wup[0] = bender_ckeywordv("pull");
                (void) bender_channel_push(wake,
                        bender_wrap_tuple(bender_tuple_n(wup, 1)), 2);
            }
        }
        Bender tup[4];
        tup[0] = bender_ckeywordv("pid");
        tup[1] = bender_wrap_number((double) (lane + 1));
        tup[2] = bender_wrap_number((double) serial);
        tup[3] = bender_wrap_abstract(ctl);
        return bender_wrap_tuple(bender_tuple_n(tup, 4));
        }
        /* try-push refused (full): fall through to the ctl path */
    }
    /* Batched path, unless disabled or bypassed (see the header comment).
     * A limit of 1 means OFF: it must fall through to the original path
     * entirely, not merely flush every entry, because the batch path's eager
     * marshal is itself what costs the wave phase. */
    if (bender_spawn_batch_limit() > 1 && 2 != bender_lane_policy &&
            bender_channel_count(ctl) < BENDER_LANE_CTL_CAP) {
        if (NULL == bender_spawn_batch) {
            bender_spawn_batch = bender_array(bender_spawn_batch_limit());
            bender_gcroot(bender_wrap_array(bender_spawn_batch));
        }
        if (NULL != bender_spawn_batch_ctl && ctl != bender_spawn_batch_ctl) {
            bender_spawn_batch_push();
        }
        /* EAGER marshal: snapshot the closure HERE, where the spawn happens.
         * Deferring it to flush time would let the closure observe later
         * values of the spawner's mutable captures. */
        BenderBuffer *ebuf = bender_buffer(64);
        bender_marshal(ebuf, bender_wrap_function(func), NULL,
                       BENDER_MARSHAL_UNSAFE);
        Bender e[4];
        e[0] = bender_ckeywordv("spawn");
        e[1] = bender_wrap_number((double) serial);
        e[2] = bender_wrap_number(cap);
        e[3] = bender_wrap_buffer(ebuf);
        bender_spawn_batch_ctl = ctl;
        bender_array_push(bender_spawn_batch,
                          bender_wrap_tuple(bender_tuple_n(e, 4)));
        Bender btup[4];
        btup[0] = bender_ckeywordv("pid");
        btup[1] = bender_wrap_number((double) (lane + 1));
        btup[2] = bender_wrap_number((double) serial);
        btup[3] = bender_wrap_abstract(ctl);
        Bender bpid = bender_wrap_tuple(bender_tuple_n(btup, 4));
        if (bender_spawn_batch->count >= bender_spawn_batch_limit()) {
            bender_spawn_batch_push();
        }
        return bpid;
    }
    bender_spawn_batch_push();

    /* Try-mode never parks: preserve the immediate path when accepted. */
    int needs_write = bender_channel_push(ctl, spawn_msg, 3);
    Bender tup[4];
    tup[0] = bender_ckeywordv("pid");
    tup[1] = bender_wrap_number((double) (lane + 1));
    tup[2] = bender_wrap_number((double) serial);
    tup[3] = bender_wrap_abstract(ctl);
    Bender pid = bender_wrap_tuple(bender_tuple_n(tup, 4));
    if (!needs_write) return pid;
    /* A parked C write resumes with its channel, not at this C frame.
     * The boot wrapper retains the intended pid across its blocking give. */
    Bender plan[4];
    plan[0] = bender_ckeywordv("proc-spawn-write");
    plan[1] = pid;
    plan[2] = bender_wrap_abstract(ctl);
    plan[3] = spawn_msg;
    return bender_wrap_tuple(bender_tuple_n(plan, 4));
}

/* Worker-local: spawn-serial -> local proc abstract. */
/* Current proc->fiber->proc nesting depth during marking (see
 * bender_procat_mark and BENDER_PROC_MARK_CHAIN_DEPTH). */
BENDER_THREAD_LOCAL int bender_proc_mark_depth = 0;

static BENDER_THREAD_LOCAL BenderTable *bender_lane_local;

static int32_t bender_os_cpu_count(void) {
#ifdef _SC_NPROCESSORS_ONLN
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    if (n > 0) return (int32_t) n;
#endif
    return 4;
}

/* Best-effort core pin for the calling thread. Opt-in via
 * :affinity true; silently a no-op on unsupported platforms. */
static void bender_lane_pin_core(int32_t core) {
    if (core < 0) return;
#if defined(BENDER_LINUX) || (defined(_GNU_SOURCE) && defined(__GLIBC__))
    {
        cpu_set_t set;
        CPU_ZERO(&set);
        CPU_SET((int) core % (int) CPU_SETSIZE, &set);
        (void) pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
    }
#elif defined(BENDER_APPLE)
    {
        thread_affinity_policy_data_t policy;
        policy.affinity_tag = (integer_t) (core + 1);
        (void) thread_policy_set(mach_thread_self(),
                                 THREAD_AFFINITY_POLICY,
                                 (thread_policy_t) &policy, 1);
    }
#else
    (void) core;
#endif
}

BENDER_CORE_FN(cfun_proc_lane_pin,
              "(proc/lane-pin core)",
              "Best-effort pin the calling thread to `core`. Internal: "
              "called by lane workers at startup when :affinity is on. "
              "No-op on platforms without affinity support.") {
    bender_fixarity(argc, 1);
    int32_t core = bender_getinteger(argv, 0);
    bender_lane_pin_core(core);
    return bender_wrap_nil();
}

/* The resident lane dispatcher (a C function so it marshals across
 * the registry copy). Arg: [ctl lane-id]. Loops taking from the
 * lane's threaded control channel:
 *   [:spawn serial cap entry]  -> VM-local proc/spawn, register serial
 *   [:deliver serial payload]  -> proc/send into the local proc's mbox
 *   [:stop]                    -> return (thread exits)
 */
BENDER_CORE_FN(cfun_proc_self_pid,
              "(proc/self-pid)",
              "The calling process's portable pid tuple "
              "[:pid lane serial ctl] -- routable from any lane. For "
              "procs on the main VM this lazily registers the proc "
              "with lane 0's dispatcher on first call.") {
    bender_fixarity(argc, 0);
    (void) argv;
    BenderFiber *me = bender_vm.fiber;
    if (NULL == me || NULL == me->proc) {
        bender_panic("proc/self-pid outside a process");
    }
    BenderProcess *me_proc = me->proc;
    if (NULL != getenv("BENDER_TRACE_PID")) {
        bender_eprintf("[pid] env=%d lane_local=%p\n",
                (NULL != me_proc->fiber && NULL != me_proc->fiber->env) ? 1 : 0,
                (void *) bender_lane_local);
    }
    if (NULL != me_proc->fiber && NULL != me_proc->fiber->env) {
        Bender cached = bender_table_get(me_proc->fiber->env,
                bender_ckeywordv("proc-cross-pid"));
        if (!bender_checktype(cached, BENDER_NIL)) {
            if (NULL != getenv("BENDER_TRACE_PID")) {
                bender_eprintf("[pid] CACHE-HIT registering serial=%u\n",
                        me_proc->serial);
            }
            /* Milestone 2b: cached-pid procs (routed spawns) register
             * in this VM's lane registry too -- the home lane's
             * dispatcher resolves [:proc-monitor]/[:kill] ops through
             * proc/lane-lookup, which reads only this table. */
            if (NULL == bender_lane_local) {
                bender_lane_local = bender_table(0);
                bender_gcroot(bender_wrap_table(bender_lane_local));
            }
            if (NULL != getenv("BENDER_TRACE_PID"))
                bender_eprintf("[put] table=%p serial=%u\n",
                        (void *) bender_lane_local, me_proc->serial);
            bender_table_put(bender_lane_local,
                    bender_wrap_number((double) me_proc->serial),
                    bender_wrap_abstract(me_proc));
            /* ALSO under the DISPATCH serial: the routed-spawn op
             * allocates its own serial on the spawner's VM (the one
             * in the pid tuple / [:proc-monitor]/[:kill] ops), while
             * spawn_impl minted the proc's real serial here. */
            const Bender *dtup = bender_unwrap_tuple(cached);
            if (4 == bender_tuple_length(dtup) &&
                    bender_checktype(dtup[2], BENDER_NUMBER)) {
                bender_table_put(bender_lane_local, dtup[2],
                        bender_wrap_abstract(me_proc));
            }
            return cached;
        }
    }
    if (NULL == bender_lane0_ctl) {
        bender_panic("proc/self-pid requires lanes (proc/lanes-start)");
    }
    if (NULL == bender_lane_local) {
        bender_lane_local = bender_table(0);
        /* Allocated under the calling fiber's ctx: root it, or the
         * fiber's completion minor sweeps the registry out from
         * under later deliveries (observed as bender_hash on freed
         * memory at 500k procs across 4 lanes). */
        bender_gcroot(bender_wrap_table(bender_lane_local));
    }
    uint32_t serial = me_proc->serial;
    /* node must be NONZERO: proc/send forwards [:proc-fwd serial msg]
     * over the carried lane ctl for any nonzero node. Node 0 is the
     * legacy direct-mbox shape (same-VM tasklets). */
    Bender tup[4];
    tup[0] = bender_ckeywordv("pid");
    tup[1] = bender_wrap_number(1);
    tup[2] = bender_wrap_number((double) serial);
    tup[3] = bender_wrap_abstract(bender_lane0_ctl);
    Bender pid = bender_wrap_tuple(bender_tuple_n(tup, 4));
    bender_table_put(bender_lane_local, bender_wrap_number((double) serial),
            bender_wrap_abstract(me_proc));
    if (NULL != me_proc->fiber->env) {
        bender_table_put(me_proc->fiber->env, bender_ckeywordv("proc-cross-pid"), pid);
    }
    return pid;
}

BENDER_CORE_FN(cfun_proc_spawn_local,
              "(proc/spawn-local entry &opt cap)",
              "Spawn on the CALLING VM even when lanes are active "
              "(proc/spawn routes across lanes once started). Same "
              "semantics as proc/spawn otherwise.") {
    bender_arity(argc, 1, 2);
    BenderFunction *func = bender_getfunction(argv, 0);
    int32_t cap = bender_optnat(argv, argc, 1, 64);
    return cfun_proc_spawn_impl(func, cap, 0);
}

BENDER_CORE_FN(cfun_proc_lane_lookup,
              "(proc/lane-lookup serial)",
              "Internal: the main VM's C-side serial registry, consulted "
              "by lane 0's dispatcher as a delivery fallback for procs "
              "that registered from C (proc/self-pid's lazy path).") {
    bender_fixarity(argc, 1);
    uint32_t serial = (uint32_t) bender_getinteger(argv, 0);
    if (NULL == bender_lane_local) {
        if (NULL != getenv("BENDER_TRACE_PID"))
            bender_eprintf("[lookup] table NULL serial=%u\n", serial);
        return bender_wrap_nil();
    }
    Bender found = bender_table_get(bender_lane_local,
            bender_wrap_number((double) serial));
    if (NULL != getenv("BENDER_TRACE_PID"))
        bender_eprintf("[lookup] table=%p serial=%u found=%p\n",
                (void *) bender_lane_local, serial,
                bender_checktype(found, BENDER_NIL) ? NULL
                    : bender_unwrap_pointer(found));
    return found;
}

BENDER_CORE_FN(cfun_proc_lanes,
              "(proc/lanes)",
              "Return the number of active proc lanes (worker VMs), or 0 "
              "when running single-VM (the default). Lane 0 is the main "
              "VM; workers are 1..N.") {
    bender_fixarity(argc, 0);
    (void) argv;
    return bender_wrap_number(bender_lane_count);
}

BENDER_CORE_FN(cfun_proc_lanes_start,
              "(proc/lanes-start &opt count-or-opts)",
              "Start the multi-core proc runtime: `count-or-opts` is a "
              "worker count, or a table {:count n :affinity bool}. With "
              "no argument: N = cpu-count - 1 (min 1), round-robin "
              "placement, no affinity (OS scheduling is usually right). "
              "After this, proc/spawn distributes new processes "
              "round-robin across the main VM and N worker VMs; "
              "proc/send/recv work unchanged across lanes. Returns the "
              "worker count.") {
    bender_sandbox_assert(BENDER_SANDBOX_THREADS);
    bender_arity(argc, 0, 1);
    if (bender_lane_count > 0) {
        bender_panic("proc lanes already running");
    }
    int32_t count = -1;
    int affinity = 0;
    if (argc >= 1 && !bender_checktype(argv[0], BENDER_NIL)) {
        if (bender_checktype(argv[0], BENDER_NUMBER)) {
            count = bender_getinteger(argv, 0);
        } else if (bender_checktype(argv[0], BENDER_TABLE) ||
                   bender_checktype(argv[0], BENDER_STRUCT)) {
            Bender t = argv[0];
            Bender c = bender_get(t, bender_ckeywordv("count"));
            Bender a = bender_get(t, bender_ckeywordv("affinity"));
            if (bender_checktype(c, BENDER_NUMBER)) count = bender_unwrap_number(c);
            else if (!bender_checktype(c, BENDER_NIL))
                bender_panic(":count must be an integer");
            affinity = bender_truthy(a);
        } else {
            bender_panic("expected a worker count or an options table");
        }
    }
    if (count < 0) {
        count = bender_os_cpu_count() - 1;
        if (count < 1) count = 1;
    }
    if (count > 64) count = 64;
    return bender_wrap_number(bender_lanes_start_impl(count, affinity));
}

/* The actual lane startup, shared by proc/lanes-start and the lazy autostart.
 * `count`/`affinity` are already resolved by the caller. */
static int32_t bender_lanes_start_impl(int32_t count, int affinity) {
    /* Resolve the boot-level dispatcher via the root dyn table
     * (boot.ben stores it: (setdyn :proc-lane-worker ...)). The
     * worker is a Bender function — the thread body accepts a
     * function or fiber as its main. */
    BenderFunction *worker = NULL;
    {
        Bender w = bender_wrap_nil();
        /* boot.ben setdyn'd it on the root (boot) fiber's env. */
        if (NULL != bender_vm.root_fiber && NULL != bender_vm.root_fiber->env) {
            w = bender_table_get(bender_vm.root_fiber->env,
                    bender_ckeywordv("proc-lane-worker"));
        }
        if (bender_checktype(w, BENDER_NIL) && NULL != bender_vm.top_dyns) {
            w = bender_table_get(bender_vm.top_dyns,
                    bender_ckeywordv("proc-lane-worker"));
        }
        if (!bender_checktype(w, BENDER_FUNCTION))
            bender_panic("proc/lane-worker not registered (boot mismatch)");
        worker = (BenderFunction *) bender_unwrap_function(w);
    }
    bender_lane_ctls = bender_calloc(1, sizeof(BenderChannel *) * count);
    if (NULL == bender_lane_ctls) BENDER_OUT_OF_MEMORY;
    bender_lane_launches = bender_calloc(1, sizeof(BenderLaneLaunch *) * count);
    if (NULL == bender_lane_launches) {
        bender_free(bender_lane_ctls);
        bender_lane_ctls = NULL;
        BENDER_OUT_OF_MEMORY;
    }
    for (int32_t i = 0; i < count; i++) {
        /* Threaded control channel: marshals every boundary. */
        BenderChannel *ctl = bender_channel_make_threaded_c(4096);
        bender_gcroot(bender_wrap_abstract(ctl));
        bender_lane_ctls[i] = ctl;
        if (affinity) {
            /* Pin to core i (cores 0..N-1); main VM keeps the last
             * core unpinned for its own loop when count < cpu-count. */
            Bender args[2];
            args[0] = bender_wrap_number(i);
            bender_lane_pin_core(i); /* main-side pin of the WORKER is
            wrong from here: workers pin themselves at startup when
            told (handled in the launch tuple below). */
            (void) args;
        }
        {
            Bender tup[3];
            tup[0] = bender_wrap_abstract(ctl);
            tup[1] = bender_wrap_number(i + 1);
            tup[2] = bender_wrap_boolean(affinity);
            Bender launch = bender_wrap_tuple(bender_tuple_n(tup, 3));
            /* RAW-allocate the launch buffer: bender_buffer(256) would
             * gcalloc the struct into THIS VM's slab, and the worker
             * frees it with ITS slab directory absent — a raw free of
             * a slab-interior pointer (the teardown abort). Both the
             * struct and its data must be plain malloc here. */
            BenderBuffer *buf = bender_malloc(sizeof(BenderBuffer));
            if (NULL == buf) BENDER_OUT_OF_MEMORY;
            bender_buffer_init(buf, 256);
            /* Mirror ev/thread's buffer EXACTLY: the abstract
             * registry table, then the RAW cfunction-registry copy,
             * then the worker function, then the launch value. */
            bender_marshal(buf, bender_wrap_table(bender_vm.abstract_registry),
                          NULL, BENDER_MARSHAL_UNSAFE);
            uint32_t count = (uint32_t) bender_vm.registry_count;
            bender_buffer_push_bytes(buf, (uint8_t *) &count, sizeof(count));
            bender_buffer_push_bytes(buf,
                    (uint8_t *) bender_vm.registry,
                    (int32_t) (bender_vm.registry_count * sizeof(BenderCFunRegistry)));
            bender_marshal(buf, bender_wrap_function(worker), NULL,
                          BENDER_MARSHAL_UNSAFE);
            bender_marshal(buf, launch, NULL, BENDER_MARSHAL_UNSAFE);
            BenderEVGenericMessage args;
            memset(&args, 0, sizeof(args));
            args.tag = 0x1; /* :n — return immediately */
            args.argi = (uint32_t) bender_vm.sandbox_flags;
            args.argp = buf;
            args.fiber = NULL;
            /* Milestone 2a: joinable launch. The buffer ownership and
             * teardown inside bender_go_thread_subr are unchanged; we
             * only wrap the thread so lanes-stop can join it. */
            BenderLaneLaunch *L = bender_calloc(1, sizeof(BenderLaneLaunch));
            if (NULL == L) BENDER_OUT_OF_MEMORY;
            L->buf = buf;
            L->sandbox_flags = (uint32_t) bender_vm.sandbox_flags;
            L->lane_id = i + 1;
            pthread_mutex_init(&L->lock, NULL);
            pthread_cond_init(&L->cond, NULL);
            bender_lane_launches[i] = L;
            if (pthread_create(&L->thread, NULL, bender_lane_thread_body, L)) {
                bender_lane_launches[i] = NULL;
                pthread_mutex_destroy(&L->lock);
                pthread_cond_destroy(&L->cond);
                bender_free(L);
                bender_panic("failed to spawn worker lane thread");
            }
            /* Same contract as bender_ev_threaded_call: hold the loop
             * open while the worker lives. Dropped at join (or kept
             * for detached stragglers, preserving today's semantics). */
            bender_ev_inc_refcount();
        }
    }
    /* Milestone 2c: the global overflow queue for the :steal policy.
     * Main-homed and rooted like lane0_ctl; abandoned at stop. */
    bender_lane_overflow = bender_channel_make_threaded_c(4096);
    bender_gcroot(bender_wrap_abstract(bender_lane_overflow));
    bender_lane_overflow_pushed = 0;
    bender_lane_overflow_pulled = 0;
    /* Milestone 2c: hand each dispatcher its overflow queue via the
     * control channel (marshaling it inside the launch tuple crashes
     * the registry-dedup hash; a ctl message crosses the same way pid
     * tuples always have). */
    for (int32_t i = 0; i < count; i++) {
        BenderChannel *ctl = bender_lane_ctls[i];
        if (NULL == ctl) continue;
        Bender tup[2];
        tup[0] = bender_ckeywordv("overflow");
        tup[1] = bender_wrap_abstract(bender_lane_overflow);
        (void) bender_channel_push(ctl, bender_wrap_tuple(bender_tuple_n(tup, 2)), 0);
    }
    /* Lane 0: the main VM's inbound control channel, serviced by the
     * same dispatcher running as a task on the main loop. Symmetry:
     * every proc everywhere has a routable pid. */
    bender_lane0_ctl = bender_channel_make_threaded_c(4096);
    bender_gcroot(bender_wrap_abstract(bender_lane0_ctl));
    {
        Bender tup[3];
        tup[0] = bender_wrap_abstract(bender_lane0_ctl);
        tup[1] = bender_wrap_number(0);
        tup[2] = bender_wrap_boolean(0);
        Bender args[1];
        args[0] = bender_wrap_tuple(bender_tuple_n(tup, 3));
        BenderFiber *disp = bender_fiber(worker, 64, 1, args);
        bender_schedule(disp, bender_wrap_nil());
    }
    bender_lane_count = count;
    return count;
}

BENDER_CORE_FN(cfun_proc_lanes_policy,
              "(proc/lanes-policy &opt policy)",
              "Get or set the spawn placement policy: :round-robin "
              "(default -- predictable, optimal for uniform fan-out), "
              ":steal (spawns overflow to a global queue when the "
              "target lane's control channel is at high water; idle "
              "lanes pull from it before parking), or :least-loaded "
              "(reserved; currently behaves as round-robin). Returns "
              "the effective policy keyword.") {
    bender_arity(argc, 0, 1);
    if (argc == 1) {
        Bender kw = argv[0];
        if (!bender_checktype(kw, BENDER_KEYWORD))
            bender_panic("policy must be a keyword");
        const uint8_t *kn = bender_unwrap_keyword(kw);
        int32_t klen = bender_length(kw);
        if (5 == klen && 0 == memcmp(kn, "steal", 5)) {
            bender_lane_policy = 2;
            if (0 == bender_lane_count)
                bender_panic(":steal requires lanes (proc/lanes-start)");
        } else if (11 == klen && 0 == memcmp(kn, "round-robin", 11)) {
            bender_lane_policy = 0;
        } else if (12 == klen && 0 == memcmp(kn, "least-loaded", 12)) {
            bender_lane_policy = 1;
        } else {
            bender_panic("policy must be :round-robin, :least-loaded, or :steal");
        }
    }
    switch (bender_lane_policy) {
        default:
        case 0: return bender_ckeywordv("round-robin");
        case 1: return bender_ckeywordv("least-loaded");
        case 2: return bender_ckeywordv("steal");
    }
}

BENDER_CORE_FN(cfun_proc_lanes_stats,
              "(proc/lanes-stats)",
              "Work-stealing counters: a struct {:overflow-pushed n "
              ":overflow-pulled n}. Pushes count :steal-policy spawns "
              "offloaded to the overflow queue; pulls count tasks "
              "lanes took from it. Both reset at lanes-start.") {
    bender_fixarity(argc, 0);
    (void) argv;
    BenderKV *kv = bender_struct_begin(2);
    bender_struct_put(kv, bender_ckeywordv("overflow-pushed"),
            bender_wrap_number((double) bender_atomic_load(
                    &bender_lane_overflow_pushed)));
    bender_struct_put(kv, bender_ckeywordv("overflow-pulled"),
            bender_wrap_number((double) bender_atomic_load(
                    &bender_lane_overflow_pulled)));
    return bender_wrap_struct(kv);
}

BENDER_CORE_FN(cfun_proc_lane_pull,
              "(proc/lane-pull queue)",
              "Internal: try-pop one task from the given overflow "
              "queue (non-blocking). Returns the [:spawn serial cap "
              "entry] message or nil when empty. The dispatcher pulls "
              "up to K of these before parking on its control "
              "channel. The queue travels to lanes as a ctl message; "
              "the C-side TLS global is only valid on the main VM.") {
    bender_fixarity(argc, 1);
    BenderChannel *queue = bender_getchannel(argv, 0);
    Bender item;
    /* bender_channel_pop returns 1 when an item was obtained, 0 when
     * the queue is empty (try semantics). */
    if (!bender_channel_pop(queue, &item, 0)) {
        return bender_wrap_nil();
    }
    bender_atomic_inc(&bender_lane_overflow_pulled);
    return item;
}

BENDER_CORE_FN(cfun_proc_lanes_stop,
              "(proc/lanes-stop)",
              "Stop the proc lanes and join the worker threads. Delivers "
              ":stop to every lane dispatcher (workers wake their locally "
              "parked procs best-effort via proc/try-send first), waits "
              "up to a bounded grace per worker for a clean exit, joins "
              "the finished threads and detaches stragglers (procs in "
              "endless loops keep their worker alive in the background, "
              "as before). Joined workers drop their loop refcount, so a "
              "script can reach a clean exit afterwards. Returns nil.") {
    bender_fixarity(argc, 0);
    (void) argv;
    if (0 == bender_lane_count) return bender_wrap_nil();
    /* Main dispatcher (lane 0) exits; the main VM itself of course
     * keeps running. */
    if (NULL != bender_lane0_ctl) {
        Bender tup[1];
        tup[0] = bender_ckeywordv("stop");
        Bender msg = bender_wrap_tuple(bender_tuple_n(tup, 1));
        (void) bender_channel_push(bender_lane0_ctl, msg, 0);
    }
    /* Workers: ask each dispatcher to exit (try-mode: never block the
     * stop on a busy control channel). */
    if (NULL != bender_lane_ctls) {
        for (int32_t i = 0; i < bender_lane_count; i++) {
            BenderChannel *ctl = bender_lane_ctls[i];
            if (NULL == ctl) continue;
            Bender tup[1];
            tup[0] = bender_ckeywordv("stop");
            Bender msg = bender_wrap_tuple(bender_tuple_n(tup, 1));
            (void) bender_channel_push(ctl, msg, 3);
        }
    }
    /* Bounded join: wait each worker's grace, join the finished,
     * detach stragglers. A JOINED worker's ctl is unrooted here (the
     * worker is gone); a straggler keeps its ctl rooted so the main
     * VM's GC can never free a channel the straggler is using. */
    if (NULL != bender_lane_launches) {
        for (int32_t i = 0; i < bender_lane_count; i++) {
            BenderLaneLaunch *L = bender_lane_launches[i];
            if (NULL == L) continue;
            bender_lane_launches[i] = NULL;
            int done = 0;
#ifndef BENDER_WINDOWS
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec += BENDER_LANE_STOP_GRACE_SECONDS;
            pthread_mutex_lock(&L->lock);
            while (!L->exited) {
                int rc = pthread_cond_timedwait(&L->cond, &L->lock, &ts);
                if (ETIMEDOUT == rc) break;
            }
            done = L->exited;
            pthread_mutex_unlock(&L->lock);
            if (done) {
                pthread_join(L->thread, NULL);
                bender_ev_dec_refcount();
            } else {
                pthread_detach(L->thread);
            }
            pthread_mutex_destroy(&L->lock);
            pthread_cond_destroy(&L->cond);
            bender_free(L);
#else
            /* Windows: no graceful join yet — milestone-1 detached
             * semantics (documented). */
            done = 1;
            pthread_mutex_destroy(&L->lock);
            pthread_cond_destroy(&L->cond);
            bender_free(L);
#endif
            if (done && NULL != bender_lane_ctls && NULL != bender_lane_ctls[i]) {
                bender_gcunroot(bender_wrap_abstract(bender_lane_ctls[i]));
                bender_lane_ctls[i] = NULL;
            }
        }
        bender_free(bender_lane_launches);
        bender_lane_launches = NULL;
    }
    /* Free the worker-ctl array; surviving (straggler) entries keep
     * their roots and their channels alive. */
    if (NULL != bender_lane_ctls) {
        bender_free(bender_lane_ctls);
        bender_lane_ctls = NULL;
    }
    /* Lane 0's dispatcher will process its :stop on an upcoming loop
     * turn; until then the fiber's own stack keeps the channel alive
     * through GC, so dropping our registry root here is sound. */
    if (NULL != bender_lane0_ctl) {
        bender_gcunroot(bender_wrap_abstract(bender_lane0_ctl));
        bender_lane0_ctl = NULL;
    }
    if (NULL != bender_lane_local) {
        bender_gcunroot(bender_wrap_table(bender_lane_local));
    }
    bender_lane_local = NULL;
    bender_lane_count = 0;
    bender_lane_rr = 0;
    return bender_wrap_nil();
}

/* BenderProcess lives in proc.h (shared with gc.c for P3.4 majors). */

/* Resolve a handle argument to a LOCAL process object.
 *
 * A handle is either a core/proc object or a [:pid lane serial chan] tuple.
 * Object-form callers (monitor/kill/stash/...) need the C object, so a tuple
 * naming THIS lane (node 0) is resolved through the lane-local registry. That
 * is what makes handles uniform, and it is why proc/pid? accepts both shapes.
 * A tuple naming ANOTHER lane cannot be resolved here: those operations
 * manipulate C lists and fibers living in that VM, so it raises instead of
 * guessing. */
static BenderProcess *bender_getproc(const Bender *argv, int32_t n) {
    Bender x = argv[n];
    if (bender_checktype(x, BENDER_ABSTRACT) &&
            bender_abstract_type(bender_unwrap_abstract(x)) == &bender_proc_type) {
        return (BenderProcess *) bender_unwrap_abstract(x);
    }
    if (bender_proc_is_pid_tuple(x)) {
        const Bender *tup = bender_unwrap_tuple(x);
        int32_t node = (int32_t) bender_unwrap_number(tup[1]);
        if (0 != node) {
            bender_panicf("this operation needs a local process, but the pid "
                         "names lane %d (objects live in that VM; use "
                         "proc/send or proc/monitor to interact with it)",
                         (int) node);
        }
        if (NULL != bender_lane_local) {
            Bender hit = bender_table_get(bender_lane_local,
                    bender_wrap_number((double) bender_unwrap_number(tup[2])));
            if (bender_checktype(hit, BENDER_ABSTRACT) &&
                    bender_abstract_type(bender_unwrap_abstract(hit)) == &bender_proc_type) {
                return (BenderProcess *) bender_unwrap_abstract(hit);
            }
        }
        bender_panic("pid does not name a live local process");
    }
    bender_panicf("expected a proc or pid, got %v", x);
}

/* B6 spawn-cost: materialize the mailbox on first real use. Called
 * only on the proc's home VM (the core/proc abstract refuses marshal,
 * so it never crosses threads; pid tuples carry the channel pointer
 * itself and thus only exist after this has run). The assignment to
 * proc->mbox makes the channel reachable through the proc (which the
 * caller's argv/registry already roots) for any GC the subsequent
 * channel push may trigger. */
static BenderChannel *bender_proc_ensure_mbox(BenderProcess *proc) {
    if (NULL == proc->mbox) {
        proc->mbox = bender_channel_make_threaded_c(proc->mbox_cap);
    }
    return proc->mbox;
}

/* Mark everything the process owns (fiber, both channels, P1.2
 * stash + monitors + exit reason). */
static int bender_procat_mark(void *p, size_t size) {
    (void) size;
    BenderProcess *proc = (BenderProcess *) p;
#ifdef BENDER_ASAN_UAF_TRACE
    if (NULL != proc->mbox &&
            NULL == bender_abstract_head(proc->mbox)->type) {
        BenderAbstractHead *mh = bender_abstract_head(proc->mbox);
        char bbuf[560];
        int bl = snprintf(bbuf, sizeof bbuf,
                "[proc-mbox-tombstoned] proc=%p mbox=%p home=%p "
                "exported=%d guard=%d refcnt=%d lane=%d"
                " ra0=%p ra1=%p ra2=%p ra3=%p ra4=%p\n",
                (void *) proc, (void *) proc->mbox, mh->home_vm,
                mh->exported, mh->teardown_started,
                (int) mh->gc.data.refcount, bender_vm.is_lane_vm,
                __builtin_return_address(0),
                __builtin_return_address(1),
                __builtin_return_address(2),
                __builtin_return_address(3),
                __builtin_return_address(4));
        write(2, bbuf, (size_t) bl);
        abort();
    }
    if (NULL != proc->sup &&
            NULL == bender_abstract_head(proc->sup)->type) {
        char bbuf[160];
        int bl = snprintf(bbuf, sizeof bbuf,
                "[proc-sup-tombstoned] proc=%p sup=%p\n",
                (void *) proc, (void *) proc->sup);
        write(2, bbuf, (size_t) bl);
        abort();
    }
#endif
    if (NULL != proc->fiber) {
        /* Proc captures proc is a real GC edge, so a full transitive walk is
         * correct -- but only a GLOBAL collection needs it.
         *
         * A scoped minor (gc_minor_phase) sweeps one owner's heap and must
         * only stop blocks on THAT heap from being freed. Proving a foreign
         * fiber's full reachability is the job of the collection that owns and
         * sweeps it. Since every proc is its own fiber with its own gc_owner, a
         * chain of "each proc captures the next" made this line walk the entire
         * remaining chain on EVERY minor: O(remaining) per minor x N minors =
         * O(N^2). Measured on the trampoline wave, the identical computation
         * wired through channels ran 204x faster at n=32000 (60.0ms vs
         * 12260.8ms), isolating this edge as the cause.
         *
         * Marking the fiber shallowly instead is UNSOUND (suite-proc loses a
         * parked fiber's in-flight recv value). So keep the edge and bound its
         * depth: proc -> fiber -> closure env -> proc -> ... is walked transitively
         * for the first BENDER_PROC_MARK_CHAIN_DEPTH hops and shallow after that.
         * The blocks are still marked, so nothing reachable through a bounded
         * prefix is freed, and the quadratic collapses because depth -- not
         * chain length -- caps the walk. */

        if (!bender_vm.gc_minor_phase ||
                bender_proc_mark_depth < BENDER_PROC_MARK_CHAIN_DEPTH) {
            bender_proc_mark_depth++;
            bender_mark(bender_wrap_fiber(proc->fiber));
            bender_proc_mark_depth--;
        } else {
            bender_gc_mark_fiber_shallow(proc->fiber);
        }
    }
    /* Tombstoned (type == NULL) mbox/sup are gone forever: prune the
     * stale pointer instead of marking freed memory. A live fiber's
     * env can hold a proc value whose channels were torn down after
     * its claims dropped (the :proc-self self-reference outlives the
     * mailbox's registry life); marking through the dangling pointer
     * walked malloc-reused memory (BUG 2 density tail head). */
    if (NULL != proc->mbox &&
            NULL == bender_abstract_head(proc->mbox)->type) {
        proc->mbox = NULL;
    }
    if (NULL != proc->sup &&
            NULL == bender_abstract_head(proc->sup)->type) {
        proc->sup = NULL;
    }
    if (NULL != proc->mbox) bender_mark(bender_wrap_abstract(proc->mbox));
    if (NULL != proc->sup) bender_mark(bender_wrap_abstract(proc->sup));
    if (NULL != proc->stash) bender_mark(bender_wrap_array(proc->stash));
    if (NULL != proc->monitors) bender_mark(bender_wrap_array(proc->monitors));
    bender_mark(proc->exit_reason);
    return 0;
}

/* No separate teardown: fiber and channels are GC objects (marked
 * above, freed by their own finalizers). The struct rides the
 * abstract payload. */
/* Marshal: NULL/NULL (refuses both directions — same-VM only, like
 * pool handles; cross-lane proxies land in P2). */

const BenderAbstractType bender_proc_type = {
    "core/proc",
    NULL, /* gc: the fiber marks the proc (see bender_mark fiber case) */
    bender_procat_mark,
    NULL, /* get */
    NULL, /* put */
    NULL, /* marshal */
    NULL, /* unmarshal */
    NULL, /* tostring */
    NULL, /* compare */
    NULL, /* hash */
    NULL, /* next */
    NULL, /* call */
    NULL, /* length */
    NULL, /* bytes */
};

/* Issue a fresh pid serial (0 reserved, wrap skips it). */
static uint32_t bender_proc_next_serial(void) {
    uint32_t id = ++bender_vm.proc_serial;
    if (0 == id) id = ++bender_vm.proc_serial;
    return id;
}

/* P3.2: marshal-copy a spawn entry (same-VM, malloc transit buffer
 * like channel pack — proven shape). Closures copy by value
 * (bytecode travels); fibers suspended with C frames refuse (no
 * aliased stacks into the isolated heap). Caller roots the result
 * across subsequent allocations. */
static BenderFunction *bender_proc_copy_entry(BenderFunction *func) {
    BenderBuffer *buf = bender_malloc(sizeof(BenderBuffer));
    if (NULL == buf) {
        BENDER_OUT_OF_MEMORY;
    }
    bender_buffer_init(buf, 64);
    bender_marshal(buf, bender_wrap_function(func), NULL, BENDER_MARSHAL_UNSAFE);
    Bender copy = bender_unmarshal(buf->data, buf->count, BENDER_MARSHAL_UNSAFE, NULL, NULL);
    bender_buffer_deinit(buf);
    bender_free(buf);
    if (!bender_checktype(copy, BENDER_FUNCTION)) {
        bender_panic("proc entry did not survive copy");
    }
    return bender_unwrap_function(copy);
}

static Bender cfun_proc_spawn_impl(BenderFunction *func, int32_t cap, int isolated) {
    /* Fiber first (P3.5c): spawn-construction attributes to the child
     * via gc_alloc_ctx, so the process's memory is born owned (owner
     * id, strict bit, nursery, filing) and per-proc pressure/sweeps
     * cover it while globals stay quiet. */
    /* 16 slots instead of 64: a raw proc thunk is arity 0 with a tiny body in
     * the common case, and the fiber stack grows on demand anyway
     * (bender_fiber_pushn reallocates). 64 slots reserved 512 bytes per proc up
     * front for every process whether used or not. */
    BenderFiber *fiber = bender_fiber(func, 16, 0, NULL);

    bender_assert(NULL != fiber, "bad proc fiber");
    /* P3.3: strict fibers opt their page claims into the isolated bit
     * (barrier-checked exports) and arm the VM-wide gate. Never
     * inherited, never unmarshalled. Set before the override so every
     * attributed birth agrees. */
    fiber->heap_strict = isolated;
    if (isolated) bender_vm.heap_strict_live = 1;
    /* P3.6: lane-shared stacks when enabled (same suspend discipline
     * as ev tasks — proc bodies block in ev ops, never raw-yield). */
    fiber->shared_stack = bender_vm.shared_stacks_on;
    fiber->proc = NULL;
    /* The proc handle stays parent-born (spawner-facing): no strict
     * bit, so registry/monitor/discipline stores of it never trip
     * the export rule — same as before the override. */
    BenderProcess *proc = bender_abstract(&bender_proc_type, sizeof(BenderProcess));
    Bender proc_j = bender_wrap_abstract(proc);
    /* Root across the allocations below (fiber + tables can collect). */
    bender_gcroot(proc_j);
    proc->fiber = NULL;
    proc->serial = bender_proc_next_serial();
    proc->done = 0;
    proc->exit_reason = bender_wrap_nil();
    proc->trap_exit = 0;
    proc->killed = 0;
    proc->isolated_heap = isolated;
    /* Parent env exists for the proto chain (parent-born, before the
     * override engages). */
    if (NULL != bender_vm.fiber && !bender_vm.fiber->env) {
        bender_vm.fiber->env = bender_table(0);
    }
    /* Attributed construction under try/finally: panics longjmp past
     * straight-line clears, which would strand a dangling override
     * (later births filing into a dead fiber's lists). */
    bender_vm.gc_alloc_ctx = fiber;
    {
        BenderTryState tstate;
        BenderSignal sig = bender_try(&tstate);
        if (!sig) {
            /* Milestone 2b: the mbox is a THREADED channel — any proc
             * must be sendable (and monitorable) from any lane, and an
             * isolated mbox panics on foreign-thread push ("isolated
             * channel used from a foreign thread"), which silently ate
             * lane-N -> lane-0 deliveries. The sup channel stays
             * isolated: only the home-lane proxy takes it. */
            /* B6: the mbox is LAZY — most procs in a spawn burst are
             * never sent anything before they park or die, and a
             * threaded channel costs a malloc'd abstract plus mutex
             * and cond init. It materializes on first use
             * (bender_proc_ensure_mbox: send/try-send/recv/pid).
             * The sup channel stays EAGER: ev.c's completion hook
             * pushes the :down event only when the fiber carries a
             * supervisor channel, and the late-monitor semantic
             * (suite-proc test 11) needs that event buffered in the
             * channel even when nobody monitored during the life. */
            proc->mbox = NULL;
            proc->mbox_cap = (int32_t) cap;
            proc->sup = bender_channel_make(8);
            /* P1.2 fields, lazily allocated (density: most procs never
             * stash or monitor — spawn no longer pays 2 arrays + 2
             * root pairs per birth; every mark site null-checks, and
             * the accessors allocate on first use). */
            proc->stash = NULL;
            proc->monitors = NULL;
            if (isolated) {
                /* Marshal-copied entry, attributed to the child (its
                 * code and constants live in its heap). Re-reset the
                 * fresh fiber onto the copy (reuse pattern from thread
                 * workers). */
                BenderFunction *copy = bender_proc_copy_entry(func);
                if (bender_vm.trace_strict) {
                    bender_strict_debug_envs(copy);
                }
                Bender copy_j = bender_wrap_function(copy);
                bender_gcroot(copy_j);
                bender_fiber_reset(fiber, copy, 0, NULL);
                fiber->heap_strict = isolated;
                bender_gcunroot(copy_j);
            }
            /* Env chain (mirrors ev/go) plus :proc-self for proc/self.
             * Importing the parent-born handle into the strict env is
             * allowed (imports pass; exports panic). Masks set here —
             * after any re-reset, which clears flags. */
            fiber->flags |=
                BENDER_FIBER_MASK_ERROR |
                BENDER_FIBER_MASK_USER0 |
                BENDER_FIBER_MASK_USER1 |
                BENDER_FIBER_MASK_USER2 |
                BENDER_FIBER_MASK_USER3 |
                BENDER_FIBER_MASK_USER4;
            /* Erlang-parity fix (c), docs/internals/proc-lanes.md: do NOT
             * proto-chain the child's env to the spawner's FULL env. That
             * chain used to make every top-level `def` the spawner can see
             * -- including a flat array of every sibling proc handle, the
             * natural pattern for joining on a spawn burst -- reachable
             * from this child, which every one of ITS OWN per-actor minor
             * collections then walked: O(N) work x N actors = O(N^2),
             * confirmed via live `sample` profiling (proc-lanes.md's "root
             * cause found" section). Instead: copy forward only the small
             * fixed ambient-binding allowlist (*out* / *err*) BY VALUE (a
             * single bounded proto walk here, not a live chain kept open),
             * matching Erlang's explicit-arguments-only spawn semantic.
             * Legacy full-chain behavior is still available opt-in via
             * (setdyn :proc-inherit-env true) in the SPAWNER before
             * spawning, for code proven to depend on seeing the spawner's
             * other dynamic bindings or top-level defs. BENDER_PROC_ENV_TRACE
             * logs every narrow-env spawn as a migration aid (this is a
             * breaking change to what a spawned proc's (dyn :key) can see;
             * dyn's own nil-on-miss contract is deliberately NOT touched --
             * see proc-lanes.md's loud-error-vs-silent-nil answer). */
            fiber->env = bender_table(0);
            {
                BenderTable *spawner_env = bender_vm.fiber ? bender_vm.fiber->env : NULL;
                /* P3.3 export-rule carve-out: proc/spawn-isolated's strict
                 * heap discipline (bender_strict_debug_envs, the
                 * "shared-store, never captured" escape panic exercised by
                 * suite-proc's P3.3 test) depends on the full proto chain
                 * reaching the real shared parent env so a write through it
                 * is detectably a cross-heap escape, not a copy. Isolated
                 * procs are also not the demonstrated O(n^2) shape (they
                 * already pay marshal-copy per spawn); narrow only the
                 * plain (non-isolated) path here. */
                int inherit_full = isolated || (NULL != spawner_env &&
                    bender_truthy(bender_table_get(spawner_env, bender_ckeywordv("proc-inherit-env"))));
                if (inherit_full) {
                    fiber->env->proto = spawner_env;
                } else {
                    fiber->env->proto = NULL;
                    if (NULL != spawner_env) {
                        Bender v_out = bender_table_get(spawner_env, bender_ckeywordv("out"));
                        if (!bender_checktype(v_out, BENDER_NIL)) {
                            bender_table_put(fiber->env, bender_ckeywordv("out"), v_out);
                        }
                        Bender v_err = bender_table_get(spawner_env, bender_ckeywordv("err"));
                        if (!bender_checktype(v_err, BENDER_NIL)) {
                            bender_table_put(fiber->env, bender_ckeywordv("err"), v_err);
                        }
                    }
                    if (NULL != getenv("BENDER_PROC_ENV_TRACE")) {
                        fprintf(stderr, "[BENDER_PROC_ENV_TRACE] proc/spawn: narrow "
                                "env (out/err only, no proto-chain to spawner). "
                                "(dyn :key) for any other spawner binding now "
                                "returns nil where it used to resolve. Opt into "
                                "legacy full-chain inheritance with "
                                "(setdyn :proc-inherit-env true) before spawning "
                                "if this proc depends on it.\n");
                    }
                }
            }
            bender_table_put(fiber->env, bender_ckeywordv("proc-self"), proc_j);
            proc->fiber = fiber;
            /* P3.5: back-pointer for auto-trigger routing. */
            fiber->proc = proc;
            fiber->supervisor_channel = proc->sup;
            bender_vm.gc_alloc_ctx = NULL;
            bender_restore(&tstate);
        } else {
            bender_vm.gc_alloc_ctx = NULL;
            bender_restore(&tstate);
            bender_gcunroot(proc_j);
            bender_panicv(tstate.payload);
        }
    }
    bender_schedule(fiber, bender_wrap_nil());
    bender_gcunroot(proc_j);
    return proc_j;
}

BENDER_CORE_FN(cfun_proc_spawn,
              "(proc/spawn-prepare thunk &opt capacity)",
              "Internal primitive for proc/spawn; not a stable public API. "
              "Returns a local or accepted routed handle, or an internal "
              "[:proc-spawn-write pid ctl message] plan. The boot wrapper "
              "must complete that blocking write before returning pid.") {
    bender_arity(argc, 1, 2);
    BenderFunction *func = bender_getfunction(argv, 0);
    if (func->def->min_arity > 0) {
        bender_panic("proc thunk must accept 0 arguments");
    }
    int32_t cap = bender_optnat(argv, argc, 1, 16);
    return bender_proc_spawn_routed(func, cap);
}

BENDER_CORE_FN(cfun_proc_spawn_isolated,
              "(proc/spawn-isolated thunk &opt capacity)",
              "P3.2 strict spawn: like proc/spawn, but the entry is "
              "marshal-copied first — captured values arrive by value, "
              "never by alias (fresh fibers copy too; fibers suspended "
              "with C frames refuse with a clear error). First step "
              "toward per-proc majors: the process starts with no shared "
              "references of its own. Reads of shared state afterwards "
              "are still the body's discipline (barrier-checked in P3.3).") {
    bender_arity(argc, 1, 2);
    BenderFunction *func = bender_getfunction(argv, 0);
    if (func->def->min_arity > 0) {
        bender_panic("proc thunk must accept 0 arguments");
    }
    int32_t cap = bender_optnat(argv, argc, 1, 16);
    /* P3.5c: the entry copy happens inside the impl, attributed to
     * the child (its code lives in its heap). */
    return cfun_proc_spawn_impl(func, cap, 1);
}

BENDER_CORE_FN(cfun_proc_isolated,
              "(proc/isolated? p)",
              "P3.2: true for strict (spawn-copied) processes.") {
    bender_fixarity(argc, 1);
    BenderProcess *proc = bender_getproc(argv, 0);
    return bender_wrap_boolean(proc->isolated_heap);
}

BENDER_CORE_FN(cfun_proc_self,
              "(proc/self)",
              "The calling process, or nil outside a process.") {
    bender_fixarity(argc, 0);
    (void) argv;
    if (NULL == bender_vm.fiber || NULL == bender_vm.fiber->env) {
        return bender_wrap_nil();
    }
    Bender self = bender_table_get(bender_vm.fiber->env,
                                 bender_ckeywordv("proc-self"));
    if (bender_checktype(self, BENDER_ABSTRACT) &&
            bender_abstract_type(bender_unwrap_abstract(self)) == &bender_proc_type) {
        return self;
    }
    return bender_wrap_nil();
}

BENDER_CORE_FN(cfun_proc_send,
              "(proc/send p msg)",
              "Deliver `msg` to `p`'s mailbox (marshal-copied, like all "
              "isolated traffic). `p` is a proc or a [:pid node serial "
              "chan] pid tuple (proc or tasklet shape — sends resolve "
              "through the carried channel, so cross-kind delivery works "
              "if the sender honors the receiver's convention: proc "
              "bodies take single values, tasklet loops take "
              "[tag payload reply] triples). Node 0 sends give directly; "
              "remote nodes (P2.1) forward [:proc-fwd serial msg] over "
              "the carried lane-control channel (a thread channel — "
              "forgery with a plain channel shares by reference: user "
              "error, remote pids come from spawn-on). Blocks at "
              "capacity. Returns `p`.") {
    bender_fixarity(argc, 2);
    if (bender_vm.coerce_error) {
        bender_panic("cannot proc/send inside bender_call");
    }
    /* Remote pid? Forward through the lane control channel. */
    if (bender_checktype(argv[0], BENDER_TUPLE)) {
        const Bender *tup = bender_unwrap_tuple(argv[0]);
        if (4 == bender_tuple_length(tup) &&
                bender_checktype(tup[0], BENDER_KEYWORD) &&
                bender_unwrap_keyword(tup[0]) == bender_unwrap_keyword(bender_ckeywordv("pid")) &&
                bender_checktype(tup[1], BENDER_NUMBER) &&
                0 != (int64_t) bender_unwrap_number(tup[1])) {
            if (!bender_checktype(tup[2], BENDER_NUMBER)) {
                bender_panic("remote pid serial must be a number");
            }
            BenderChannel *ctl = (BenderChannel *) bender_unwrap_abstract(tup[3]);
            Bender fwd[3];
            fwd[0] = bender_ckeywordv("proc-fwd");
            fwd[1] = tup[2];
            fwd[2] = argv[1];
            Bender fwdmsg = bender_wrap_tuple(bender_tuple_n(fwd, 3));
            if (bender_channel_push(ctl, fwdmsg, 0)) {
                bender_await();
            }
            return argv[0];
        }
    }
    BenderChannel *mbox = bender_proc_resolve_mbox(argv[0]);
    if (bender_channel_push(mbox, argv[1], 0)) {
        bender_await();
    }
    return argv[0];
}

BENDER_CORE_FN(cfun_proc_try_send,
              "(proc/try-send p msg)",
              "Like `proc/send`, but never blocks: delivers when the "
              "mailbox has room (or a reader is parked) and reports "
              "`true`; returns `false` on a full mailbox. Remote pid "
              "tuples forward the same way, also without blocking.") {
    bender_fixarity(argc, 2);
    if (bender_vm.coerce_error) {
        bender_panic("cannot proc/send inside bender_call");
    }
    if (bender_checktype(argv[0], BENDER_TUPLE)) {
        const Bender *tup = bender_unwrap_tuple(argv[0]);
        if (4 == bender_tuple_length(tup) &&
                bender_checktype(tup[0], BENDER_KEYWORD) &&
                bender_unwrap_keyword(tup[0]) == bender_unwrap_keyword(bender_ckeywordv("pid")) &&
                bender_checktype(tup[1], BENDER_NUMBER) &&
                0 != (int64_t) bender_unwrap_number(tup[1])) {
            if (!bender_checktype(tup[2], BENDER_NUMBER)) {
                bender_panic("remote pid serial must be a number");
            }
            BenderChannel *ctl = (BenderChannel *) bender_unwrap_abstract(tup[3]);
            Bender fwd[3];
            fwd[0] = bender_ckeywordv("proc-fwd");
            fwd[1] = tup[2];
            fwd[2] = argv[1];
            Bender fwdmsg = bender_wrap_tuple(bender_tuple_n(fwd, 3));
            /* Try-mode push: 0 delivered, nonzero full. */
            return bender_wrap_boolean(0 == bender_channel_push(ctl, fwdmsg, 3));
        }
    }
    BenderChannel *mbox = bender_proc_resolve_mbox(argv[0]);
    return bender_wrap_boolean(0 == bender_channel_push(mbox, argv[1], 3));
}

BENDER_CORE_FN(cfun_proc_recv,
              "(proc/recv &opt p)",
              "Take the next message from the calling process's own "
              "mailbox (P1.1: take-next only; selective receive lands in "
              "P1.2). With `p`, it must be the calling process (stealing "
              "from other mailboxes is an error). Suspends when empty.") {
    bender_arity(argc, 0, 1);
    BenderFiber *me = bender_vm.fiber;
    if (NULL == me) {
        bender_panic("proc/recv outside a process");
    }
    /* Phase 1 fast path: the P3.5 back-pointer IS the self identity —
     * one deref instead of an env table get per message (the 100k-agent
     * profile showed the lookup as ~40% of the recv side). */
    BenderProcess *me_proc = me->proc;
    Bender self;
    if (NULL != me_proc) {
        self = bender_wrap_abstract(me_proc);
    } else {
        if (NULL == me->env) {
            bender_panic("proc/recv outside a process");
        }
        self = bender_table_get(me->env, bender_ckeywordv("proc-self"));
        if (!(bender_checktype(self, BENDER_ABSTRACT) &&
                bender_abstract_type(bender_unwrap_abstract(self)) == &bender_proc_type)) {
            bender_panic("proc/recv outside a process");
        }
    }
    if (argc >= 1) {
        /* Own pid tuple? Local node + serial must match (remote
         * serials repeat per lane — node disambiguates). */
        if (bender_checktype(argv[0], BENDER_TUPLE)) {
            const Bender *tup = bender_unwrap_tuple(argv[0]);
            BenderProcess *mine = (BenderProcess *) bender_unwrap_abstract(self);
            if (!(4 == bender_tuple_length(tup) &&
                    bender_checktype(tup[0], BENDER_KEYWORD) &&
                bender_unwrap_keyword(tup[0]) == bender_unwrap_keyword(bender_ckeywordv("pid")) &&
                    bender_checktype(tup[1], BENDER_NUMBER) &&
                    0 == (int64_t) bender_unwrap_number(tup[1]) &&
                    bender_checktype(tup[2], BENDER_NUMBER) &&
                    (uint32_t) bender_unwrap_number(tup[2]) == mine->serial)) {
                bender_panic("proc/recv reads only the caller's own mailbox");
            }
        } else {
            BenderProcess *want = bender_getproc(argv, 0);
            BenderProcess *mine = (BenderProcess *) bender_unwrap_abstract(self);
            if (want != mine) {
                bender_panic("proc/recv reads only the caller's own mailbox");
            }
        }
    }
    BenderProcess *proc = (BenderProcess *) bender_unwrap_abstract(self);
    if (bender_vm.coerce_error) {
        bender_panic("cannot proc/recv inside bender_call");
    }
    /* B6: first recv materializes the mailbox (an empty mailbox and
     * an unallocated one are behaviorally identical: no push site can
     * exist without the channel pointer, which only this, send, or
     * pid-of mint). */
    BenderChannel *mbox = bender_proc_ensure_mbox(proc);
    Bender item;
    if (bender_channel_pop(mbox, &item, 0)) {
        bender_schedule(bender_vm.root_fiber, item);
    }
    bender_await();
}

BENDER_CORE_FN(cfun_proc_kill,
              "(proc/kill p reason)",
              "Untrappable kill: cancel the process fiber with `reason` "
              "(lands at the next resume/quantum boundary; monitors "
              "observe via the supervisor completion in P1.2). Dead "
              "processes are a silent no-op. Returns `p`. "
              "Milestone 2b: a remote pid tuple forwards [:kill serial "
              "reason] to the home lane's dispatcher, which applies the "
              "kill locally.") {
    bender_fixarity(argc, 2);
    /* Milestone 2b: remote pid — forward to the home lane. */
    if (bender_checktype(argv[0], BENDER_TUPLE)) {
        const Bender *tup = bender_unwrap_tuple(argv[0]);
        if (4 == bender_tuple_length(tup) &&
                bender_checktype(tup[0], BENDER_KEYWORD) &&
                bender_unwrap_keyword(tup[0]) == bender_unwrap_keyword(bender_ckeywordv("pid")) &&
                bender_checktype(tup[1], BENDER_NUMBER) &&
                0 != (int64_t) bender_unwrap_number(tup[1])) {
            if (!bender_checktype(tup[2], BENDER_NUMBER)) {
                bender_panic("remote pid serial must be a number");
            }
            BenderChannel *ctl = (BenderChannel *) bender_unwrap_abstract(tup[3]);
            Bender k[3];
            k[0] = bender_ckeywordv("kill");
            k[1] = tup[2];      /* serial */
            k[2] = argv[1];     /* reason */
            bender_channel_push(ctl, bender_wrap_tuple(bender_tuple_n(k, 3)), 0);
            return argv[0];
        }
        bender_panic("proc/kill: bad pid tuple");
    }
    BenderProcess *proc = bender_getproc(argv, 0);
    BenderFiber *fiber = proc->fiber;
    if (NULL != fiber && bender_fiber_can_resume(fiber)) {
        proc->killed = 1;
        bender_cancel(fiber, argv[1]);
        /* P3.5c: retire the mailbox now (purge the victim's pendings —
         * it may be parked in recv — and go quiet so later sends drop
         * silently, Erlang parity). Both objects are listed-valid here
         * (mid-mutator, pre-sweep). */
        if (NULL != proc->mbox) {
            /* P3.5c hook, now real: if the victim is parked in recv on
             * its own mailbox, cancel the pending and land the kill
             * immediately (a parked fiber has no next resume). */
            bender_channel_cancel_reader(proc->mbox, fiber, argv[1]);
        }
    }
    return argv[0];
}

BENDER_CORE_FN(cfun_proc_alive,
              "(proc/alive? p)",
              "True while the process fiber can still resume. Accepts a local\n"
              "core/proc object or a node-0 pid tuple. A tuple naming ANOTHER\n"
              "lane cannot be inspected from here -- its fiber lives in that VM\n"
              "and there is no remote-query op -- so it reports true (it is not\n"
              "known-dead) and monitor/kill are the supported cross-lane verbs.") {
    bender_fixarity(argc, 1);
    if (bender_proc_is_pid_tuple(argv[0])) {
        const Bender *tup = bender_unwrap_tuple(argv[0]);
        int32_t node = (int32_t) bender_unwrap_number(tup[1]);
        if (0 == node && NULL != bender_lane_local) {
            Bender hit = bender_table_get(bender_lane_local,
                    bender_wrap_number((double) bender_unwrap_number(tup[2])));
            if (bender_checktype(hit, BENDER_ABSTRACT) &&
                    bender_abstract_type(bender_unwrap_abstract(hit)) == &bender_proc_type) {
                BenderProcess *proc = (BenderProcess *) bender_unwrap_abstract(hit);
                return bender_wrap_boolean(NULL != proc->fiber &&
                                          bender_fiber_can_resume(proc->fiber));
            }
            return bender_wrap_false();
        }
        /* Remote lane: not inspectable, and not known-dead. */
        return bender_wrap_true();
    }
    BenderProcess *proc = bender_getproc(argv, 0);
    return bender_wrap_boolean(NULL != proc->fiber &&
                              bender_fiber_can_resume(proc->fiber));
}

/* Is `x` a [:pid node serial chan] tuple? Shared so a remote handle can be
 * treated as a handle rather than as an unknown type. */
static int bender_proc_is_pid_tuple(Bender x) {
    if (!bender_checktype(x, BENDER_TUPLE)) return 0;
    const Bender *tup = bender_unwrap_tuple(x);
    return 4 == bender_tuple_length(tup) &&
           bender_checktype(tup[0], BENDER_KEYWORD) &&
           bender_unwrap_keyword(tup[0]) == bender_unwrap_keyword(bender_ckeywordv("pid")) &&
           bender_checktype(tup[1], BENDER_NUMBER) &&
           bender_checktype(tup[2], BENDER_NUMBER) &&
           bender_checktype(tup[3], BENDER_ABSTRACT);
}

BENDER_CORE_FN(cfun_proc_pid,
              "(proc/pid? x)",
              "True for a process handle of EITHER shape: a local core/proc\n"
              "object, or the [:pid lane serial chan] tuple that proc/spawn\n"
              "returns once lanes are active. Branch on this instead of the\n"
              "type; proc/pid, proc/serial and proc/alive? accept both.") {
    bender_fixarity(argc, 1);
    if (bender_checktype(argv[0], BENDER_ABSTRACT) &&
            bender_abstract_type(bender_unwrap_abstract(argv[0])) == &bender_proc_type) {
        return bender_wrap_true();
    }
    return bender_wrap_boolean(bender_proc_is_pid_tuple(argv[0]));
}

/* P1.3 pid interop: a proc's portable face is a [:pid node serial
 * mbox] tuple (same shape as tasklet pids). Sends resolve through
 * the carried channel (no registry needed); monitor/kill need the
 * object (C lists/fibers live there). Tasklet.ben stays untouched:
 * tasklets already resolve unknown pids via the carried channel. */
static BenderChannel *bender_proc_resolve_mbox(Bender x) {
    if (bender_checktype(x, BENDER_ABSTRACT) &&
            bender_abstract_type(bender_unwrap_abstract(x)) == &bender_proc_type) {
        /* B6: lazy mbox — home-VM access only (see ensure helper). */
        return bender_proc_ensure_mbox((BenderProcess *) bender_unwrap_abstract(x));
    }
    if (bender_checktype(x, BENDER_TUPLE)) {
        const Bender *tup = bender_unwrap_tuple(x);
        /* Keywords intern to unique pointers: pointer compare is exact. */
        if (4 == bender_tuple_length(tup) &&
                bender_checktype(tup[0], BENDER_KEYWORD) &&
                bender_unwrap_keyword(tup[0]) == bender_unwrap_keyword(bender_ckeywordv("pid")) &&
                bender_checktype(tup[3], BENDER_ABSTRACT)) {
            void *abst = bender_unwrap_abstract(tup[3]);
            /* Carried mailbox: isolated (proc pid) or any channel
             * (tasklet pid — caller owns the triple convention). */
            return (BenderChannel *) abst;
        }
    }
    bender_panic("expected a proc or [:pid node serial chan] pid");
}

BENDER_CORE_FN(cfun_proc_pid_of,
              "(proc/pid p)",
              "P1.3: the process's portable pid tuple [:pid node serial\n"
              "mbox] (same shape as tasklet pids — passes tasklet/pid?).\n"
              "Sends to it deliver via the carried mailbox; monitor/kill\n"
              "need the object.") {
    bender_fixarity(argc, 1);
    /* A pid tuple is already the portable face: return it unchanged so
     * proc/pid is idempotent across handle shapes. */
    if (bender_proc_is_pid_tuple(argv[0])) return argv[0];
    BenderProcess *proc = bender_getproc(argv, 0);
    Bender tup[4];
    tup[0] = bender_ckeywordv("pid");
    tup[1] = bender_wrap_integer(0);
    tup[2] = bender_wrap_integer((int64_t) proc->serial);
    /* B6: the carried mailbox is the pid tuple's routable face —
     * materialize it here (once) so sends through the tuple resolve. */
    tup[3] = bender_wrap_abstract(bender_proc_ensure_mbox(proc));
    return bender_wrap_tuple(bender_tuple_n(tup, 4));
}

BENDER_CORE_FN(cfun_proc_serial,
              "(proc/serial p)",
              "P1.3: the process's numeric serial (pid tuple slot 2).\n"
              "Accepts a local core/proc or a pid tuple.") {
    bender_fixarity(argc, 1);
    if (bender_proc_is_pid_tuple(argv[0])) return bender_unwrap_tuple(argv[0])[2];
    BenderProcess *proc = bender_getproc(argv, 0);
    return bender_wrap_integer((int64_t) proc->serial);
}

BENDER_CORE_FN(cfun_proc_stash,
              "(proc/stash p)",
              "P1.2: the process's selective-receive stash array (live "
              "object — scan/remove/push from Bender; the match loop in "
              "examples/proc.ben owns the discipline).") {
    bender_fixarity(argc, 1);
    BenderProcess *proc = bender_getproc(argv, 0);
    if (NULL == proc->stash) proc->stash = bender_array(0);
    return bender_wrap_array(proc->stash);
}

BENDER_CORE_FN(cfun_proc_sup_of,
              "(proc/sup-of p)",
              "P1.2: the process's completion (supervisor) channel. "
              "Reapers take completion events here; bodies never touch it.") {
    bender_fixarity(argc, 1);
    BenderProcess *proc = bender_getproc(argv, 0);
    return bender_wrap_abstract(proc->sup);
}

BENDER_CORE_FN(cfun_proc_monitor,
              "(proc/monitor watched watcher tag)",
              "P1.2: record that `watcher` wants [:down tag reason] when\n"
              "`watched` completes. `watcher` is a proc (same-lane) or,\n"
              "since P2.3, any channel (cross-lane monitor targets and\n"
              "same-lane channel taps — reapers give to channels, send\n"
              "to procs). Appends to the watched process's C monitor\n"
              "list. Late monitors (already done) must check proc/done?\n"
              "first — this just records. Returns `watcher`.\n\n"
              "Milestone 2b: when `watched` is a remote pid tuple, the\n"
              "monitor routes to the home lane's dispatcher as\n"
              "[:mon serial watcher tag]; a proxy proc on the home lane\n"
              "takes the watched proc's supervisor channel (same-VM\n"
              "only) and forwards [:down tag status value] to the\n"
              "watcher, whatever lane it lives on. Returns `watcher`.") {
    bender_fixarity(argc, 3);
    /* Milestone 2b: remote pid tuples are handled at the dispatcher
     * level (:proc-monitor op from procx/monitor-chan) -- registration
     * must happen on the watched proc's home lane, where the C
     * completion hook delivers. */
    BenderProcess *watched = bender_getproc(argv, 0);
    if (NULL == watched->monitors) watched->monitors = bender_array(0);
    bender_array_push(watched->monitors, argv[2]);
    bender_array_push(watched->monitors, argv[1]);
    return argv[1];
}

BENDER_CORE_FN(cfun_proc_monitors_of,
              "(proc/monitors-of p)",
              "P1.2: the flat [tag watcher] monitor array (live — reapers "
              "drain it, then clear it).") {
    bender_fixarity(argc, 1);
    BenderProcess *proc = bender_getproc(argv, 0);
    if (NULL == proc->monitors) proc->monitors = bender_array(0);
    return bender_wrap_array(proc->monitors);
}

BENDER_CORE_FN(cfun_proc_record_exit,
              "(proc/record-exit p reason)",
              "P1.2: mark the process done with `reason` (reapers call "
              "this once, right after taking the completion event).") {
    bender_fixarity(argc, 2);
    BenderProcess *proc = bender_getproc(argv, 0);
    proc->done = 1;
    proc->exit_reason = argv[1];
    return argv[0];
}

BENDER_CORE_FN(cfun_proc_done,
              "(proc/done? p)",
              "P1.2: true once the exit was recorded (late-monitor path).") {
    bender_fixarity(argc, 1);
    BenderProcess *proc = bender_getproc(argv, 0);
    return bender_wrap_boolean(proc->done);
}

/* B7.1 named process registry (erlang:register/2, whereis/1,
 * unregister/1). bender_vm.proc_registry is a lazily-allocated
 * keyword -> core/proc BenderTable, root-marked in gc.c alongside
 * top_dyns/profile_table and reset in vm.c's init/deinit (see those
 * sites for the full reasoning) -- no further GC plumbing needed
 * here; an ordinary BenderTable's own marking covers the registered
 * proc handles, so a live registration correctly keeps its process
 * reachable, and removing a stale entry (below) correctly releases
 * that reference.
 *
 * Known, bounded race: proc->done is set by the reaper path
 * (cfun_proc_record_exit, "right after taking the completion
 * event"), which can run strictly after the underlying fiber has
 * actually finished. There is no earlier authoritative signal
 * available without deeper scheduler plumbing. This means a name
 * can remain "in use" for a brief window after its process is
 * actually gone, but NEVER the other way around (whereis/register
 * never observes a name as free while its proc is still genuinely
 * alive) -- the lag leans in the safe direction, so proc->done is
 * used as-is rather than inventing an unverified workaround. */
static BenderTable *bender_proc_registry_table(void) {
    if (NULL == bender_vm.proc_registry) {
        bender_vm.proc_registry = bender_table(0);
    }
    return bender_vm.proc_registry;
}

BENDER_CORE_FN(cfun_proc_register,
              "(proc/register name proc)",
              "B7.1: register `proc` under keyword `name` for "
              "proc/whereis lookup. Panics if `name` is already "
              "registered to a process that is not proc/done? -- a "
              "live registration is never silently overwritten. If "
              "the previous holder of `name` has completed, the "
              "registration is replaced without requiring an "
              "explicit unregister first. Returns `proc`.") {
    bender_fixarity(argc, 2);
    if (!bender_checktype(argv[0], BENDER_KEYWORD)) {
        bender_panicf("proc/register name must be a keyword, got %v", argv[0]);
    }
    BenderProcess *proc = bender_getproc(argv, 1);
    BenderTable *reg = bender_proc_registry_table();
    Bender prev = bender_table_get(reg, argv[0]);
    if (bender_checktype(prev, BENDER_ABSTRACT) &&
            bender_abstract_type(bender_unwrap_abstract(prev)) == &bender_proc_type) {
        BenderProcess *prevproc = (BenderProcess *) bender_unwrap_abstract(prev);
        if (!prevproc->done) {
            bender_panicf("proc/register: name %v already registered to a live process",
                         argv[0]);
        }
    }
    bender_table_put(reg, argv[0], argv[1]);
    (void) proc;
    return argv[1];
}

BENDER_CORE_FN(cfun_proc_whereis,
              "(proc/whereis name)",
              "B7.1: look up the process registered under keyword "
              "`name`, or nil if unregistered. If the registered "
              "process has since completed, the stale entry is "
              "removed and nil is returned -- a caller never "
              "receives a handle to an already-finished process.") {
    bender_fixarity(argc, 1);
    if (!bender_checktype(argv[0], BENDER_KEYWORD)) {
        bender_panicf("proc/whereis name must be a keyword, got %v", argv[0]);
    }
    if (NULL == bender_vm.proc_registry) return bender_wrap_nil();
    Bender found = bender_table_get(bender_vm.proc_registry, argv[0]);
    if (bender_checktype(found, BENDER_NIL)) return bender_wrap_nil();
    BenderProcess *proc = (BenderProcess *) bender_unwrap_abstract(found);
    if (proc->done) {
        bender_table_remove(bender_vm.proc_registry, argv[0]);
        return bender_wrap_nil();
    }
    return found;
}

BENDER_CORE_FN(cfun_proc_unregister,
              "(proc/unregister name)",
              "B7.1: remove the registration under keyword `name`, "
              "if present. No-op, not an error, if nothing is "
              "registered under that name. Returns nil.") {
    bender_fixarity(argc, 1);
    if (!bender_checktype(argv[0], BENDER_KEYWORD)) {
        bender_panicf("proc/unregister name must be a keyword, got %v", argv[0]);
    }
    if (NULL != bender_vm.proc_registry) {
        bender_table_remove(bender_vm.proc_registry, argv[0]);
    }
    return bender_wrap_nil();
}

BENDER_CORE_FN(cfun_proc_exit_reason,
              "(proc/exit-reason p)",
              "P1.2: the recorded exit reason (:normal on clean exit).") {
    bender_fixarity(argc, 1);
    BenderProcess *proc = bender_getproc(argv, 0);
    return proc->exit_reason;
}

BENDER_CORE_FN(cfun_proc_trap_exit,
              "(proc/trap-exit p flag)",
              "P1.4: set the loop-process trap flag (read back with\n"
              "proc/trap?). Trapping loops observe linked peers' deaths\n"
              "as :down messages and survive; untrapped loops die with\n"
              "the reason. Kill (proc/kill) bypasses the flag. Returns `p`.") {
    bender_fixarity(argc, 2);
    BenderProcess *proc = bender_getproc(argv, 0);
    proc->trap_exit = bender_truthy(argv[1]) ? 1 : 0;
    return argv[0];
}

BENDER_CORE_FN(cfun_proc_trap,
              "(proc/trap? p)",
              "P1.4: the loop-process trap flag.") {
    bender_fixarity(argc, 1);
    BenderProcess *proc = bender_getproc(argv, 0);
    return bender_wrap_boolean(proc->trap_exit);
}

BENDER_CORE_FN(cfun_proc_killed,
              "(proc/killed? p)",
              "P1.4b: true once proc/kill armed this process (terminal; "
              "the dispatch loop re-raises handler errors while armed so "
              "kills of handler-wedged fibers cannot be trapped by the "
              "loop's own protect).") {
    bender_fixarity(argc, 1);
    BenderProcess *proc = bender_getproc(argv, 0);
    return bender_wrap_boolean(proc->killed);
}

BENDER_CORE_FN(cfun_proc_unmarshal_entry,
              "(proc/lane-unmarshal-entry buffer)",
              "INTERNAL lane-dispatcher protocol. Unmarshal one batched spawn "
              "entry's closure from BUFFER. The spawner marshals eagerly, at "
              "spawn time, because the marshal is what snapshots a closure's "
              "captured values -- deferring it to flush time would let the "
              "closure observe later values of anything the spawner mutates "
              "afterwards. The Bender-level `unmarshal` cannot be used here: "
              "it refuses threaded abstract pointers (a captured thread-chan "
              "is exactly that) because it has no unsafe flag, while the "
              "cross-lane channel path already marshals with "
              "BENDER_MARSHAL_UNSAFE for the same values. Not for general "
              "use: unmarshalling arbitrary bytecode is unsafe by "
              "construction, which is why the safe builtin refuses it.") {
    bender_fixarity(argc, 1);
    BenderBuffer *buf = bender_getbuffer(argv, 0);
    return bender_unmarshal(buf->data, buf->count, BENDER_MARSHAL_UNSAFE,
                            NULL, NULL);
}

static const BenderRegExt proc_cfuns_ext[] = {
    BENDER_CORE_REG("proc/lane-unmarshal-entry", cfun_proc_unmarshal_entry),
    BENDER_CORE_REG("proc/spawn-prepare", cfun_proc_spawn),
    BENDER_CORE_REG("proc/spawn-isolated", cfun_proc_spawn_isolated),
        BENDER_CORE_REG("proc/self-pid", cfun_proc_self_pid),
        BENDER_CORE_REG("proc/spawn-local", cfun_proc_spawn_local),
        BENDER_CORE_REG("proc/lane-lookup", cfun_proc_lane_lookup),
        BENDER_CORE_REG("proc/lanes", cfun_proc_lanes),
        BENDER_CORE_REG("proc/lanes-start", cfun_proc_lanes_start),
        BENDER_CORE_REG("proc/lanes-policy", cfun_proc_lanes_policy),
        BENDER_CORE_REG("proc/lanes-stats", cfun_proc_lanes_stats),
        BENDER_CORE_REG("proc/lane-pull", cfun_proc_lane_pull),
        BENDER_CORE_REG("proc/lanes-stop", cfun_proc_lanes_stop),
        BENDER_CORE_REG("proc/lane-pin", cfun_proc_lane_pin),
    BENDER_CORE_REG("proc/isolated?", cfun_proc_isolated),
    BENDER_CORE_REG("proc/self", cfun_proc_self),
    BENDER_CORE_REG("proc/send", cfun_proc_send),
    BENDER_CORE_REG("proc/try-send", cfun_proc_try_send),
    BENDER_CORE_REG("proc/recv", cfun_proc_recv),
    BENDER_CORE_REG("proc/kill", cfun_proc_kill),
    BENDER_CORE_REG("proc/alive?", cfun_proc_alive),
    BENDER_CORE_REG("proc/pid?", cfun_proc_pid),
    BENDER_CORE_REG("proc/pid", cfun_proc_pid_of),
    BENDER_CORE_REG("proc/serial", cfun_proc_serial),
    BENDER_CORE_REG("proc/stash", cfun_proc_stash),
    BENDER_CORE_REG("proc/sup-of", cfun_proc_sup_of),
    BENDER_CORE_REG("proc/monitor", cfun_proc_monitor),
    BENDER_CORE_REG("proc/monitors-of", cfun_proc_monitors_of),
    BENDER_CORE_REG("proc/record-exit", cfun_proc_record_exit),
    BENDER_CORE_REG("proc/done?", cfun_proc_done),
    BENDER_CORE_REG("proc/exit-reason", cfun_proc_exit_reason),
    BENDER_CORE_REG("proc/trap-exit", cfun_proc_trap_exit),
    BENDER_CORE_REG("proc/trap?", cfun_proc_trap),
    BENDER_CORE_REG("proc/killed?", cfun_proc_killed),
    BENDER_CORE_REG("proc/register", cfun_proc_register),
    BENDER_CORE_REG("proc/whereis", cfun_proc_whereis),
    BENDER_CORE_REG("proc/unregister", cfun_proc_unregister),
    BENDER_REG_END
};

void bender_lib_proc(BenderTable *env) {
    bender_core_cfuns_ext(env, NULL, (BenderRegExt *) proc_cfuns_ext);
    bender_register_abstract_type(&bender_proc_type);
}

#endif /* BENDER_EV */
