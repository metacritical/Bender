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

#ifndef JANET_AMALG
#include "features.h"
#include <stdio.h>
#include <janet.h>
#include "util.h"
#include "gc.h"
#include "state.h"
#include "fiber.h"
#include "ev.h"
#include "ev.h"
#include "proc.h"
#endif

/* P-lanes platform includes: OUTSIDE the JANET_AMALG guard — the
 * amalgam skips per-file includes but still needs these. */
#ifdef JANET_APPLE
#include <mach/mach.h>
#include <mach/thread_act.h>
#include <mach/thread_policy.h>
#endif
#if defined(JANET_LINUX) || defined(_GNU_SOURCE)
#include <pthread.h>
#include <sched.h>
#else
#include <pthread.h>
#endif

#ifdef JANET_EV

/* Forward: abstract type defined below (used by the getter). */
extern const JanetAbstractType janet_proc_type;
/* Forward: pid-tuple resolver (defined with proc/pid, used by send). */
static JanetChannel *janet_proc_resolve_mbox(Janet x);
static int janet_proc_is_pid_tuple(Janet x);
static Janet cfun_proc_spawn_impl(JanetFunction *func, int32_t cap, int isolated);
static uint32_t janet_proc_next_serial(void);

/* ===== P-lanes: VM-per-core proc parallelism =====
 * N resident OS threads, each a full share-nothing VM (the ev/thread
 * body), each running a proc lane: a dispatcher that owns local procs
 * and services spawn/deliver requests marshaled from the main VM.
 * Per-proc heaps compose with per-VM collectors, so cores never
 * contend on a global GC — the BEAM shape. Lane 0 is the main VM;
 * workers are 1..N. Cross-lane handles are pid tuples
 * [:pid lane serial ctl] (resolve_mbox already speaks tuples; the
 * node field disambiguates lanes). */
static JANET_THREAD_LOCAL JanetChannel **janet_lane_ctls;
static JANET_THREAD_LOCAL JanetChannel *janet_lane0_ctl;
static JANET_THREAD_LOCAL int32_t janet_lane_count;
static JANET_THREAD_LOCAL int32_t janet_lane_rr;

/* Milestone 2c: work-stealing. The overflow queue is a global
 * threaded channel owned by the main VM (created at lanes-start,
 * rooted like lane0_ctl). Routed spawns under the :steal policy push
 * to it when the target ctl is at high water; dispatchers drain it
 * (up to K tasks) before parking on an empty ctl. Policy:
 * 0 = :round-robin (default), 1 = :least-loaded (reserved, behaves
 * as round-robin), 2 = :steal. Counters are the feature indication:
 * lanes-stats reports pushes/pulls so tests assert stealing actually
 * happened. */
static JANET_THREAD_LOCAL JanetChannel *janet_lane_overflow;
static JANET_THREAD_LOCAL int32_t janet_lane_policy;
static volatile JanetAtomicInt janet_lane_overflow_pushed;
static volatile JanetAtomicInt janet_lane_overflow_pulled;

/* Milestone 2a: pool-style join. Workers launch on JOINABLE pthreads,
 * each with a completion sync. lanes-stop asks every dispatcher to
 * exit, wakes locally-parked procs (best effort), waits a bounded
 * grace per worker, joins the ones that finished and detaches
 * stragglers. A joined worker drops its launch refcount, so a script
 * that stops its lanes can reach a clean loop exit (the AOT
 * exit-hang). Straggler workers keep the process alive exactly like
 * the milestone-1 detached workers did. */
typedef struct JanetLaneLaunch {
    pthread_t thread;
    pthread_mutex_t lock;
    pthread_cond_t cond;
    int exited;
    JanetBuffer *buf;
    uint32_t sandbox_flags;
} JanetLaneLaunch;

static JANET_THREAD_LOCAL JanetLaneLaunch **janet_lane_launches;

/* Grace per worker lane when stopping: procs parked in a single recv
 * wake and finish quickly; procs in long loops hit the grace and are
 * detached instead. */
#define JANET_LANE_STOP_GRACE_SECONDS 2

static void *janet_lane_thread_body(void *p) {
    janet_vm.is_lane_vm = 1;
    JanetLaneLaunch *L = p;
    JanetEVGenericMessage args;
    memset(&args, 0, sizeof(args));
    args.tag = 0x1 /* :n — the caller never awaits us */
        | 0x8 /* JANET_THREAD_NODEINIT_FLAG — see ev.c */;
    args.argi = L->sandbox_flags;
    args.argp = L->buf;
    args.fiber = NULL;
    janet_go_thread_subr(args);
    pthread_mutex_lock(&L->lock);
    L->exited = 1;
    pthread_cond_broadcast(&L->cond);
    pthread_mutex_unlock(&L->lock);
        janet_vm.is_lane_vm = 0;
return NULL;
}

/* P-lanes: with lanes active, route round-robin to a worker VM. The
 * entry marshals over a threaded channel; the handle is a pid tuple
 * [:pid lane serial ctl] (resolve_mbox speaks tuples, so proc/send
 * works unchanged). Local spawns (proc/spawn-local, lane-internal
 * spawns) call the impl directly. */
static int32_t janet_os_cpu_count(void);
static int32_t janet_lanes_start_impl(int32_t count, int affinity);

/* Lazily start the multi-core runtime the first time a proc is spawned.
 *
 * The lane machinery only activates once lanes exist, so without this a plain
 * proc/spawn loop silently uses one core forever while Go and Erlang start
 * their schedulers at boot. Autostart lives HERE rather than in boot.janet
 * because boot.janet also runs during image generation, where spawning lanes
 * hangs the build (observed: janet_boot stuck in the lane join).
 *
 * Opt out with JANET_LANES=0; pin a count with JANET_LANES=<n>. Each VM
 * autostarts once; a failed start leaves lanes at 0 rather than erroring, so a
 * sandboxed embedder that forbids threads keeps working single-threaded. */
static void janet_lanes_maybe_autostart(void) {
    static JANET_THREAD_LOCAL int attempted = 0;
    if (attempted || janet_lane_count > 0) return;
    attempted = 1;
    /* OPT-IN for now, not default-on.
     *
     * Autostart is deliberately gated behind JANET_LANES=<n> because turning it
     * on by default changes proc/spawn's RETURN TYPE from a core/proc object to
     * a [:pid lane serial ctl] tuple for every caller (measured: it breaks
     * suite-proc's "spawn returns a proc"). That is a real API break, and it
     * must not happen silently to code written against the single-VM shape.
     *
     * =0 keeps the historic single-VM behaviour, which is also what the test
     * suite assumes. Once proc handles are uniform across lanes (a local proc
     * proxy, or a pid tuple that satisfies proc/pid?), the default can flip. */
    const char *raw = getenv("JANET_LANES");
    if (NULL == raw || 0 == strcmp(raw, "0")) return;
    int32_t count = -1;
    {
        long v = strtol(raw, NULL, 10);
        if (v > 0) count = (int32_t) v;
    }
    if (count < 0) {
        count = janet_os_cpu_count() - 1;
        if (count < 1) count = 1;
    }
    if (count > 64) count = 64;
    /* The worker dispatcher comes from boot; without it we cannot run lanes. */
    {
        Janet w = janet_wrap_nil();
        if (NULL != janet_vm.root_fiber && NULL != janet_vm.root_fiber->env) {
            w = janet_table_get(janet_vm.root_fiber->env,
                    janet_ckeywordv("proc-lane-worker"));
        }
        if (janet_checktype(w, JANET_NIL) && NULL != janet_vm.top_dyns) {
            w = janet_table_get(janet_vm.top_dyns,
                    janet_ckeywordv("proc-lane-worker"));
        }
        if (!janet_checktype(w, JANET_FUNCTION)) return;
    }
    (void) janet_lanes_start_impl(count, 0);
}

static Janet janet_proc_spawn_routed(JanetFunction *func, int32_t cap) {
    janet_lanes_maybe_autostart();
    if (0 == janet_lane_count) return cfun_proc_spawn_impl(func, cap, 0);
    int32_t lane = (janet_lane_rr % janet_lane_count);
    janet_lane_rr++;
    JanetChannel *ctl = janet_lane_ctls[lane];
    uint32_t serial = janet_proc_next_serial();
    Janet msg[4];
    msg[0] = janet_ckeywordv("spawn");
    msg[1] = janet_wrap_number((double) serial);
    msg[2] = janet_wrap_number(cap);
    msg[3] = janet_wrap_function(func);
    Janet spawn_msg = janet_wrap_tuple(janet_tuple_n(msg, 4));
    /* Milestone 2c: :steal policy offloads to the overflow queue when
     * the target ctl is at high water (3/4 capacity). The task runs
     * wherever a lane pulls it; the spawner's pid tuple keeps the
     * spawn-time lane (documented: sends via that tuple may hit
     * :noproc for pulled procs -- communicate via closures or the
     * proc's own self-pid). Try-mode never blocks the spawner. */
    if (2 == janet_lane_policy && NULL != janet_lane_overflow &&
            janet_channel_count(ctl) >= 1) {
        int pr = janet_channel_push(janet_lane_overflow, spawn_msg, 2);
        /* Try-mode convention: 0 = refused (queue full), nonzero =
         * enqueued. (Mode 0 is the opposite: 0 = delivered now,
         * nonzero = the caller must await the parked push.) */
        if (pr != 0) {
        janet_atomic_inc(&janet_lane_overflow_pushed);
        /* Wakeup: a dispatcher that just parked on an empty ctl will
         * not see the overflow push (pull happens before parking).
         * Nudge the next lane to drain the queue; it keeps pulling
         * until the overflow is empty. */
        if (janet_lane_count > 0) {
            JanetChannel *wake = janet_lane_ctls[
                    (janet_lane_rr + 1) % janet_lane_count];
            if (NULL != wake) {
                Janet wup[1];
                wup[0] = janet_ckeywordv("pull");
                (void) janet_channel_push(wake,
                        janet_wrap_tuple(janet_tuple_n(wup, 1)), 2);
            }
        }
        Janet tup[4];
        tup[0] = janet_ckeywordv("pid");
        tup[1] = janet_wrap_number((double) (lane + 1));
        tup[2] = janet_wrap_number((double) serial);
        tup[3] = janet_wrap_abstract(ctl);
        return janet_wrap_tuple(janet_tuple_n(tup, 4));
        }
        /* try-push refused (full): fall through to the ctl path */
    }
    if (janet_channel_push(ctl, spawn_msg, 0)) {
        janet_await(); /* ctl full: resumed when the push lands */
    }
    Janet tup[4];
    tup[0] = janet_ckeywordv("pid");
    tup[1] = janet_wrap_number((double) (lane + 1));
    tup[2] = janet_wrap_number((double) serial);
    tup[3] = janet_wrap_abstract(ctl);
    return janet_wrap_tuple(janet_tuple_n(tup, 4));
}

/* Worker-local: spawn-serial -> local proc abstract. */
/* Current proc->fiber->proc nesting depth during marking (see
 * janet_procat_mark and JANET_PROC_MARK_CHAIN_DEPTH). */
JANET_THREAD_LOCAL int janet_proc_mark_depth = 0;

static JANET_THREAD_LOCAL JanetTable *janet_lane_local;

static int32_t janet_os_cpu_count(void) {
#ifdef _SC_NPROCESSORS_ONLN
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    if (n > 0) return (int32_t) n;
#endif
    return 4;
}

/* Best-effort core pin for the calling thread. Opt-in via
 * :affinity true; silently a no-op on unsupported platforms. */
static void janet_lane_pin_core(int32_t core) {
    if (core < 0) return;
#if defined(JANET_LINUX) || (defined(_GNU_SOURCE) && defined(__GLIBC__))
    {
        cpu_set_t set;
        CPU_ZERO(&set);
        CPU_SET((int) core % (int) CPU_SETSIZE, &set);
        (void) pthread_setaffinity_np(pthread_self(), sizeof(set), &set);
    }
#elif defined(JANET_APPLE)
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

JANET_CORE_FN(cfun_proc_lane_pin,
              "(proc/lane-pin core)",
              "Best-effort pin the calling thread to `core`. Internal: "
              "called by lane workers at startup when :affinity is on. "
              "No-op on platforms without affinity support.") {
    janet_fixarity(argc, 1);
    int32_t core = janet_getinteger(argv, 0);
    janet_lane_pin_core(core);
    return janet_wrap_nil();
}

/* The resident lane dispatcher (a C function so it marshals across
 * the registry copy). Arg: [ctl lane-id]. Loops taking from the
 * lane's threaded control channel:
 *   [:spawn serial cap entry]  -> VM-local proc/spawn, register serial
 *   [:deliver serial payload]  -> proc/send into the local proc's mbox
 *   [:stop]                    -> return (thread exits)
 */
JANET_CORE_FN(cfun_proc_self_pid,
              "(proc/self-pid)",
              "The calling process's portable pid tuple "
              "[:pid lane serial ctl] -- routable from any lane. For "
              "procs on the main VM this lazily registers the proc "
              "with lane 0's dispatcher on first call.") {
    janet_fixarity(argc, 0);
    (void) argv;
    JanetFiber *me = janet_vm.fiber;
    if (NULL == me || NULL == me->proc) {
        janet_panic("proc/self-pid outside a process");
    }
    JanetProcess *me_proc = me->proc;
    if (NULL != getenv("JANET_TRACE_PID")) {
        janet_eprintf("[pid] env=%d lane_local=%p\n",
                (NULL != me_proc->fiber && NULL != me_proc->fiber->env) ? 1 : 0,
                (void *) janet_lane_local);
    }
    if (NULL != me_proc->fiber && NULL != me_proc->fiber->env) {
        Janet cached = janet_table_get(me_proc->fiber->env,
                janet_ckeywordv("proc-cross-pid"));
        if (!janet_checktype(cached, JANET_NIL)) {
            if (NULL != getenv("JANET_TRACE_PID")) {
                janet_eprintf("[pid] CACHE-HIT registering serial=%u\n",
                        me_proc->serial);
            }
            /* Milestone 2b: cached-pid procs (routed spawns) register
             * in this VM's lane registry too -- the home lane's
             * dispatcher resolves [:proc-monitor]/[:kill] ops through
             * proc/lane-lookup, which reads only this table. */
            if (NULL == janet_lane_local) {
                janet_lane_local = janet_table(0);
                janet_gcroot(janet_wrap_table(janet_lane_local));
            }
            if (NULL != getenv("JANET_TRACE_PID"))
                janet_eprintf("[put] table=%p serial=%u\n",
                        (void *) janet_lane_local, me_proc->serial);
            janet_table_put(janet_lane_local,
                    janet_wrap_number((double) me_proc->serial),
                    janet_wrap_abstract(me_proc));
            /* ALSO under the DISPATCH serial: the routed-spawn op
             * allocates its own serial on the spawner's VM (the one
             * in the pid tuple / [:proc-monitor]/[:kill] ops), while
             * spawn_impl minted the proc's real serial here. */
            const Janet *dtup = janet_unwrap_tuple(cached);
            if (4 == janet_tuple_length(dtup) &&
                    janet_checktype(dtup[2], JANET_NUMBER)) {
                janet_table_put(janet_lane_local, dtup[2],
                        janet_wrap_abstract(me_proc));
            }
            return cached;
        }
    }
    if (NULL == janet_lane0_ctl) {
        janet_panic("proc/self-pid requires lanes (proc/lanes-start)");
    }
    if (NULL == janet_lane_local) {
        janet_lane_local = janet_table(0);
        /* Allocated under the calling fiber's ctx: root it, or the
         * fiber's completion minor sweeps the registry out from
         * under later deliveries (observed as janet_hash on freed
         * memory at 500k procs across 4 lanes). */
        janet_gcroot(janet_wrap_table(janet_lane_local));
    }
    uint32_t serial = me_proc->serial;
    /* node must be NONZERO: proc/send forwards [:proc-fwd serial msg]
     * over the carried lane ctl for any nonzero node. Node 0 is the
     * legacy direct-mbox shape (same-VM tasklets). */
    Janet tup[4];
    tup[0] = janet_ckeywordv("pid");
    tup[1] = janet_wrap_number(1);
    tup[2] = janet_wrap_number((double) serial);
    tup[3] = janet_wrap_abstract(janet_lane0_ctl);
    Janet pid = janet_wrap_tuple(janet_tuple_n(tup, 4));
    janet_table_put(janet_lane_local, janet_wrap_number((double) serial),
            janet_wrap_abstract(me_proc));
    if (NULL != me_proc->fiber->env) {
        janet_table_put(me_proc->fiber->env, janet_ckeywordv("proc-cross-pid"), pid);
    }
    return pid;
}

JANET_CORE_FN(cfun_proc_spawn_local,
              "(proc/spawn-local entry &opt cap)",
              "Spawn on the CALLING VM even when lanes are active "
              "(proc/spawn routes across lanes once started). Same "
              "semantics as proc/spawn otherwise.") {
    janet_arity(argc, 1, 2);
    JanetFunction *func = janet_getfunction(argv, 0);
    int32_t cap = janet_optnat(argv, argc, 1, 64);
    return cfun_proc_spawn_impl(func, cap, 0);
}

JANET_CORE_FN(cfun_proc_lane_lookup,
              "(proc/lane-lookup serial)",
              "Internal: the main VM's C-side serial registry, consulted "
              "by lane 0's dispatcher as a delivery fallback for procs "
              "that registered from C (proc/self-pid's lazy path).") {
    janet_fixarity(argc, 1);
    uint32_t serial = (uint32_t) janet_getinteger(argv, 0);
    if (NULL == janet_lane_local) {
        if (NULL != getenv("JANET_TRACE_PID"))
            janet_eprintf("[lookup] table NULL serial=%u\n", serial);
        return janet_wrap_nil();
    }
    Janet found = janet_table_get(janet_lane_local,
            janet_wrap_number((double) serial));
    if (NULL != getenv("JANET_TRACE_PID"))
        janet_eprintf("[lookup] table=%p serial=%u found=%p\n",
                (void *) janet_lane_local, serial,
                janet_checktype(found, JANET_NIL) ? NULL
                    : janet_unwrap_pointer(found));
    return found;
}

JANET_CORE_FN(cfun_proc_lanes,
              "(proc/lanes)",
              "Return the number of active proc lanes (worker VMs), or 0 "
              "when running single-VM (the default). Lane 0 is the main "
              "VM; workers are 1..N.") {
    janet_fixarity(argc, 0);
    (void) argv;
    return janet_wrap_number(janet_lane_count);
}

JANET_CORE_FN(cfun_proc_lanes_start,
              "(proc/lanes-start &opt count-or-opts)",
              "Start the multi-core proc runtime: `count-or-opts` is a "
              "worker count, or a table {:count n :affinity bool}. With "
              "no argument: N = cpu-count - 1 (min 1), round-robin "
              "placement, no affinity (OS scheduling is usually right). "
              "After this, proc/spawn distributes new processes "
              "round-robin across the main VM and N worker VMs; "
              "proc/send/recv work unchanged across lanes. Returns the "
              "worker count.") {
    janet_sandbox_assert(JANET_SANDBOX_THREADS);
    janet_arity(argc, 0, 1);
    if (janet_lane_count > 0) {
        janet_panic("proc lanes already running");
    }
    int32_t count = -1;
    int affinity = 0;
    if (argc >= 1 && !janet_checktype(argv[0], JANET_NIL)) {
        if (janet_checktype(argv[0], JANET_NUMBER)) {
            count = janet_getinteger(argv, 0);
        } else if (janet_checktype(argv[0], JANET_TABLE) ||
                   janet_checktype(argv[0], JANET_STRUCT)) {
            Janet t = argv[0];
            Janet c = janet_get(t, janet_ckeywordv("count"));
            Janet a = janet_get(t, janet_ckeywordv("affinity"));
            if (janet_checktype(c, JANET_NUMBER)) count = janet_unwrap_number(c);
            else if (!janet_checktype(c, JANET_NIL))
                janet_panic(":count must be an integer");
            affinity = janet_truthy(a);
        } else {
            janet_panic("expected a worker count or an options table");
        }
    }
    if (count < 0) {
        count = janet_os_cpu_count() - 1;
        if (count < 1) count = 1;
    }
    if (count > 64) count = 64;
    return janet_wrap_number(janet_lanes_start_impl(count, affinity));
}

/* The actual lane startup, shared by proc/lanes-start and the lazy autostart.
 * `count`/`affinity` are already resolved by the caller. */
static int32_t janet_lanes_start_impl(int32_t count, int affinity) {
    /* Resolve the boot-level dispatcher via the root dyn table
     * (boot.janet stores it: (setdyn :proc-lane-worker ...)). The
     * worker is a Janet function — the thread body accepts a
     * function or fiber as its main. */
    JanetFunction *worker = NULL;
    {
        Janet w = janet_wrap_nil();
        /* boot.janet setdyn'd it on the root (boot) fiber's env. */
        if (NULL != janet_vm.root_fiber && NULL != janet_vm.root_fiber->env) {
            w = janet_table_get(janet_vm.root_fiber->env,
                    janet_ckeywordv("proc-lane-worker"));
        }
        if (janet_checktype(w, JANET_NIL) && NULL != janet_vm.top_dyns) {
            w = janet_table_get(janet_vm.top_dyns,
                    janet_ckeywordv("proc-lane-worker"));
        }
        if (!janet_checktype(w, JANET_FUNCTION))
            janet_panic("proc/lane-worker not registered (boot mismatch)");
        worker = (JanetFunction *) janet_unwrap_function(w);
    }
    janet_lane_ctls = janet_calloc(1, sizeof(JanetChannel *) * count);
    if (NULL == janet_lane_ctls) JANET_OUT_OF_MEMORY;
    janet_lane_launches = janet_calloc(1, sizeof(JanetLaneLaunch *) * count);
    if (NULL == janet_lane_launches) {
        janet_free(janet_lane_ctls);
        janet_lane_ctls = NULL;
        JANET_OUT_OF_MEMORY;
    }
    for (int32_t i = 0; i < count; i++) {
        /* Threaded control channel: marshals every boundary. */
        JanetChannel *ctl = janet_channel_make_threaded_c(4096);
        janet_gcroot(janet_wrap_abstract(ctl));
        janet_lane_ctls[i] = ctl;
        if (affinity) {
            /* Pin to core i (cores 0..N-1); main VM keeps the last
             * core unpinned for its own loop when count < cpu-count. */
            Janet args[2];
            args[0] = janet_wrap_number(i);
            janet_lane_pin_core(i); /* main-side pin of the WORKER is
            wrong from here: workers pin themselves at startup when
            told (handled in the launch tuple below). */
            (void) args;
        }
        {
            Janet tup[3];
            tup[0] = janet_wrap_abstract(ctl);
            tup[1] = janet_wrap_number(i + 1);
            tup[2] = janet_wrap_boolean(affinity);
            Janet launch = janet_wrap_tuple(janet_tuple_n(tup, 3));
            /* RAW-allocate the launch buffer: janet_buffer(256) would
             * gcalloc the struct into THIS VM's slab, and the worker
             * frees it with ITS slab directory absent — a raw free of
             * a slab-interior pointer (the teardown abort). Both the
             * struct and its data must be plain malloc here. */
            JanetBuffer *buf = janet_malloc(sizeof(JanetBuffer));
            if (NULL == buf) JANET_OUT_OF_MEMORY;
            janet_buffer_init(buf, 256);
            /* Mirror ev/thread's buffer EXACTLY: the abstract
             * registry table, then the RAW cfunction-registry copy,
             * then the worker function, then the launch value. */
            janet_marshal(buf, janet_wrap_table(janet_vm.abstract_registry),
                          NULL, JANET_MARSHAL_UNSAFE);
            uint32_t count = (uint32_t) janet_vm.registry_count;
            janet_buffer_push_bytes(buf, (uint8_t *) &count, sizeof(count));
            janet_buffer_push_bytes(buf,
                    (uint8_t *) janet_vm.registry,
                    (int32_t) (janet_vm.registry_count * sizeof(JanetCFunRegistry)));
            janet_marshal(buf, janet_wrap_function(worker), NULL,
                          JANET_MARSHAL_UNSAFE);
            janet_marshal(buf, launch, NULL, JANET_MARSHAL_UNSAFE);
            JanetEVGenericMessage args;
            memset(&args, 0, sizeof(args));
            args.tag = 0x1; /* :n — return immediately */
            args.argi = (uint32_t) janet_vm.sandbox_flags;
            args.argp = buf;
            args.fiber = NULL;
            /* Milestone 2a: joinable launch. The buffer ownership and
             * teardown inside janet_go_thread_subr are unchanged; we
             * only wrap the thread so lanes-stop can join it. */
            JanetLaneLaunch *L = janet_calloc(1, sizeof(JanetLaneLaunch));
            if (NULL == L) JANET_OUT_OF_MEMORY;
            L->buf = buf;
            L->sandbox_flags = (uint32_t) janet_vm.sandbox_flags;
            pthread_mutex_init(&L->lock, NULL);
            pthread_cond_init(&L->cond, NULL);
            janet_lane_launches[i] = L;
            if (pthread_create(&L->thread, NULL, janet_lane_thread_body, L)) {
                janet_lane_launches[i] = NULL;
                pthread_mutex_destroy(&L->lock);
                pthread_cond_destroy(&L->cond);
                janet_free(L);
                janet_panic("failed to spawn worker lane thread");
            }
            /* Same contract as janet_ev_threaded_call: hold the loop
             * open while the worker lives. Dropped at join (or kept
             * for detached stragglers, preserving today's semantics). */
            janet_ev_inc_refcount();
        }
    }
    /* Milestone 2c: the global overflow queue for the :steal policy.
     * Main-homed and rooted like lane0_ctl; abandoned at stop. */
    janet_lane_overflow = janet_channel_make_threaded_c(4096);
    janet_gcroot(janet_wrap_abstract(janet_lane_overflow));
    janet_lane_overflow_pushed = 0;
    janet_lane_overflow_pulled = 0;
    /* Milestone 2c: hand each dispatcher its overflow queue via the
     * control channel (marshaling it inside the launch tuple crashes
     * the registry-dedup hash; a ctl message crosses the same way pid
     * tuples always have). */
    for (int32_t i = 0; i < count; i++) {
        JanetChannel *ctl = janet_lane_ctls[i];
        if (NULL == ctl) continue;
        Janet tup[2];
        tup[0] = janet_ckeywordv("overflow");
        tup[1] = janet_wrap_abstract(janet_lane_overflow);
        (void) janet_channel_push(ctl, janet_wrap_tuple(janet_tuple_n(tup, 2)), 0);
    }
    /* Lane 0: the main VM's inbound control channel, serviced by the
     * same dispatcher running as a task on the main loop. Symmetry:
     * every proc everywhere has a routable pid. */
    janet_lane0_ctl = janet_channel_make_threaded_c(4096);
    janet_gcroot(janet_wrap_abstract(janet_lane0_ctl));
    {
        Janet tup[3];
        tup[0] = janet_wrap_abstract(janet_lane0_ctl);
        tup[1] = janet_wrap_number(0);
        tup[2] = janet_wrap_boolean(0);
        Janet args[1];
        args[0] = janet_wrap_tuple(janet_tuple_n(tup, 3));
        JanetFiber *disp = janet_fiber(worker, 64, 1, args);
        janet_schedule(disp, janet_wrap_nil());
    }
    janet_lane_count = count;
    return count;
}

JANET_CORE_FN(cfun_proc_lanes_policy,
              "(proc/lanes-policy &opt policy)",
              "Get or set the spawn placement policy: :round-robin "
              "(default -- predictable, optimal for uniform fan-out), "
              ":steal (spawns overflow to a global queue when the "
              "target lane's control channel is at high water; idle "
              "lanes pull from it before parking), or :least-loaded "
              "(reserved; currently behaves as round-robin). Returns "
              "the effective policy keyword.") {
    janet_arity(argc, 0, 1);
    if (argc == 1) {
        Janet kw = argv[0];
        if (!janet_checktype(kw, JANET_KEYWORD))
            janet_panic("policy must be a keyword");
        const uint8_t *kn = janet_unwrap_keyword(kw);
        int32_t klen = janet_length(kw);
        if (5 == klen && 0 == memcmp(kn, "steal", 5)) {
            janet_lane_policy = 2;
            if (0 == janet_lane_count)
                janet_panic(":steal requires lanes (proc/lanes-start)");
        } else if (11 == klen && 0 == memcmp(kn, "round-robin", 11)) {
            janet_lane_policy = 0;
        } else if (12 == klen && 0 == memcmp(kn, "least-loaded", 12)) {
            janet_lane_policy = 1;
        } else {
            janet_panic("policy must be :round-robin, :least-loaded, or :steal");
        }
    }
    switch (janet_lane_policy) {
        default:
        case 0: return janet_ckeywordv("round-robin");
        case 1: return janet_ckeywordv("least-loaded");
        case 2: return janet_ckeywordv("steal");
    }
}

JANET_CORE_FN(cfun_proc_lanes_stats,
              "(proc/lanes-stats)",
              "Work-stealing counters: a struct {:overflow-pushed n "
              ":overflow-pulled n}. Pushes count :steal-policy spawns "
              "offloaded to the overflow queue; pulls count tasks "
              "lanes took from it. Both reset at lanes-start.") {
    janet_fixarity(argc, 0);
    (void) argv;
    JanetKV *kv = janet_struct_begin(2);
    janet_struct_put(kv, janet_ckeywordv("overflow-pushed"),
            janet_wrap_number((double) janet_atomic_load(
                    &janet_lane_overflow_pushed)));
    janet_struct_put(kv, janet_ckeywordv("overflow-pulled"),
            janet_wrap_number((double) janet_atomic_load(
                    &janet_lane_overflow_pulled)));
    return janet_wrap_struct(kv);
}

JANET_CORE_FN(cfun_proc_lane_pull,
              "(proc/lane-pull queue)",
              "Internal: try-pop one task from the given overflow "
              "queue (non-blocking). Returns the [:spawn serial cap "
              "entry] message or nil when empty. The dispatcher pulls "
              "up to K of these before parking on its control "
              "channel. The queue travels to lanes as a ctl message; "
              "the C-side TLS global is only valid on the main VM.") {
    janet_fixarity(argc, 1);
    JanetChannel *queue = janet_getchannel(argv, 0);
    Janet item;
    /* janet_channel_pop returns 1 when an item was obtained, 0 when
     * the queue is empty (try semantics). */
    if (!janet_channel_pop(queue, &item, 0)) {
        return janet_wrap_nil();
    }
    janet_atomic_inc(&janet_lane_overflow_pulled);
    return item;
}

JANET_CORE_FN(cfun_proc_lanes_stop,
              "(proc/lanes-stop)",
              "Stop the proc lanes and join the worker threads. Delivers "
              ":stop to every lane dispatcher (workers wake their locally "
              "parked procs best-effort via proc/try-send first), waits "
              "up to a bounded grace per worker for a clean exit, joins "
              "the finished threads and detaches stragglers (procs in "
              "endless loops keep their worker alive in the background, "
              "as before). Joined workers drop their loop refcount, so a "
              "script can reach a clean exit afterwards. Returns nil.") {
    janet_fixarity(argc, 0);
    (void) argv;
    if (0 == janet_lane_count) return janet_wrap_nil();
    /* Main dispatcher (lane 0) exits; the main VM itself of course
     * keeps running. */
    if (NULL != janet_lane0_ctl) {
        Janet tup[1];
        tup[0] = janet_ckeywordv("stop");
        Janet msg = janet_wrap_tuple(janet_tuple_n(tup, 1));
        (void) janet_channel_push(janet_lane0_ctl, msg, 0);
    }
    /* Workers: ask each dispatcher to exit (try-mode: never block the
     * stop on a busy control channel). */
    if (NULL != janet_lane_ctls) {
        for (int32_t i = 0; i < janet_lane_count; i++) {
            JanetChannel *ctl = janet_lane_ctls[i];
            if (NULL == ctl) continue;
            Janet tup[1];
            tup[0] = janet_ckeywordv("stop");
            Janet msg = janet_wrap_tuple(janet_tuple_n(tup, 1));
            (void) janet_channel_push(ctl, msg, 3);
        }
    }
    /* Bounded join: wait each worker's grace, join the finished,
     * detach stragglers. A JOINED worker's ctl is unrooted here (the
     * worker is gone); a straggler keeps its ctl rooted so the main
     * VM's GC can never free a channel the straggler is using. */
    if (NULL != janet_lane_launches) {
        for (int32_t i = 0; i < janet_lane_count; i++) {
            JanetLaneLaunch *L = janet_lane_launches[i];
            if (NULL == L) continue;
            janet_lane_launches[i] = NULL;
            int done = 0;
#ifndef JANET_WINDOWS
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec += JANET_LANE_STOP_GRACE_SECONDS;
            pthread_mutex_lock(&L->lock);
            while (!L->exited) {
                int rc = pthread_cond_timedwait(&L->cond, &L->lock, &ts);
                if (ETIMEDOUT == rc) break;
            }
            done = L->exited;
            pthread_mutex_unlock(&L->lock);
            if (done) {
                pthread_join(L->thread, NULL);
                janet_ev_dec_refcount();
            } else {
                pthread_detach(L->thread);
            }
            pthread_mutex_destroy(&L->lock);
            pthread_cond_destroy(&L->cond);
            janet_free(L);
#else
            /* Windows: no graceful join yet — milestone-1 detached
             * semantics (documented). */
            done = 1;
            pthread_mutex_destroy(&L->lock);
            pthread_cond_destroy(&L->cond);
            janet_free(L);
#endif
            if (done && NULL != janet_lane_ctls && NULL != janet_lane_ctls[i]) {
                janet_gcunroot(janet_wrap_abstract(janet_lane_ctls[i]));
                janet_lane_ctls[i] = NULL;
            }
        }
        janet_free(janet_lane_launches);
        janet_lane_launches = NULL;
    }
    /* Free the worker-ctl array; surviving (straggler) entries keep
     * their roots and their channels alive. */
    if (NULL != janet_lane_ctls) {
        janet_free(janet_lane_ctls);
        janet_lane_ctls = NULL;
    }
    /* Lane 0's dispatcher will process its :stop on an upcoming loop
     * turn; until then the fiber's own stack keeps the channel alive
     * through GC, so dropping our registry root here is sound. */
    if (NULL != janet_lane0_ctl) {
        janet_gcunroot(janet_wrap_abstract(janet_lane0_ctl));
        janet_lane0_ctl = NULL;
    }
    if (NULL != janet_lane_local) {
        janet_gcunroot(janet_wrap_table(janet_lane_local));
    }
    janet_lane_local = NULL;
    janet_lane_count = 0;
    janet_lane_rr = 0;
    return janet_wrap_nil();
}

/* JanetProcess lives in proc.h (shared with gc.c for P3.4 majors). */

/* Resolve a handle argument to a LOCAL process object.
 *
 * A handle is either a core/proc object or a [:pid lane serial chan] tuple.
 * Object-form callers (monitor/kill/stash/...) need the C object, so a tuple
 * naming THIS lane (node 0) is resolved through the lane-local registry. That
 * is what makes handles uniform, and it is why proc/pid? accepts both shapes.
 * A tuple naming ANOTHER lane cannot be resolved here: those operations
 * manipulate C lists and fibers living in that VM, so it raises instead of
 * guessing. */
static JanetProcess *janet_getproc(const Janet *argv, int32_t n) {
    Janet x = argv[n];
    if (janet_checktype(x, JANET_ABSTRACT) &&
            janet_abstract_type(janet_unwrap_abstract(x)) == &janet_proc_type) {
        return (JanetProcess *) janet_unwrap_abstract(x);
    }
    if (janet_proc_is_pid_tuple(x)) {
        const Janet *tup = janet_unwrap_tuple(x);
        int32_t node = (int32_t) janet_unwrap_number(tup[1]);
        if (0 != node) {
            janet_panicf("this operation needs a local process, but the pid "
                         "names lane %d (objects live in that VM; use "
                         "proc/send or proc/monitor to interact with it)",
                         (int) node);
        }
        if (NULL != janet_lane_local) {
            Janet hit = janet_table_get(janet_lane_local,
                    janet_wrap_number((double) janet_unwrap_number(tup[2])));
            if (janet_checktype(hit, JANET_ABSTRACT) &&
                    janet_abstract_type(janet_unwrap_abstract(hit)) == &janet_proc_type) {
                return (JanetProcess *) janet_unwrap_abstract(hit);
            }
        }
        janet_panic("pid does not name a live local process");
    }
    janet_panicf("expected a proc or pid, got %v", x);
}

/* B6 spawn-cost: materialize the mailbox on first real use. Called
 * only on the proc's home VM (the core/proc abstract refuses marshal,
 * so it never crosses threads; pid tuples carry the channel pointer
 * itself and thus only exist after this has run). The assignment to
 * proc->mbox makes the channel reachable through the proc (which the
 * caller's argv/registry already roots) for any GC the subsequent
 * channel push may trigger. */
static JanetChannel *janet_proc_ensure_mbox(JanetProcess *proc) {
    if (NULL == proc->mbox) {
        proc->mbox = janet_channel_make_threaded_c(proc->mbox_cap);
    }
    return proc->mbox;
}

/* Mark everything the process owns (fiber, both channels, P1.2
 * stash + monitors + exit reason). */
static int janet_procat_mark(void *p, size_t size) {
    (void) size;
    JanetProcess *proc = (JanetProcess *) p;
#ifdef JANET_ASAN_UAF_TRACE
    if (NULL != proc->mbox &&
            NULL == janet_abstract_head(proc->mbox)->type) {
        JanetAbstractHead *mh = janet_abstract_head(proc->mbox);
        char bbuf[560];
        int bl = snprintf(bbuf, sizeof bbuf,
                "[proc-mbox-tombstoned] proc=%p mbox=%p home=%p "
                "exported=%d guard=%d refcnt=%d lane=%d"
                " ra0=%p ra1=%p ra2=%p ra3=%p ra4=%p\n",
                (void *) proc, (void *) proc->mbox, mh->home_vm,
                mh->exported, mh->teardown_started,
                (int) mh->gc.data.refcount, janet_vm.is_lane_vm,
                __builtin_return_address(0),
                __builtin_return_address(1),
                __builtin_return_address(2),
                __builtin_return_address(3),
                __builtin_return_address(4));
        write(2, bbuf, (size_t) bl);
        abort();
    }
    if (NULL != proc->sup &&
            NULL == janet_abstract_head(proc->sup)->type) {
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
         * for the first JANET_PROC_MARK_CHAIN_DEPTH hops and shallow after that.
         * The blocks are still marked, so nothing reachable through a bounded
         * prefix is freed, and the quadratic collapses because depth -- not
         * chain length -- caps the walk. */

        if (!janet_vm.gc_minor_phase ||
                janet_proc_mark_depth < JANET_PROC_MARK_CHAIN_DEPTH) {
            janet_proc_mark_depth++;
            janet_mark(janet_wrap_fiber(proc->fiber));
            janet_proc_mark_depth--;
        } else {
            janet_gc_mark_fiber_shallow(proc->fiber);
        }
    }
    /* Tombstoned (type == NULL) mbox/sup are gone forever: prune the
     * stale pointer instead of marking freed memory. A live fiber's
     * env can hold a proc value whose channels were torn down after
     * its claims dropped (the :proc-self self-reference outlives the
     * mailbox's registry life); marking through the dangling pointer
     * walked malloc-reused memory (BUG 2 density tail head). */
    if (NULL != proc->mbox &&
            NULL == janet_abstract_head(proc->mbox)->type) {
        proc->mbox = NULL;
    }
    if (NULL != proc->sup &&
            NULL == janet_abstract_head(proc->sup)->type) {
        proc->sup = NULL;
    }
    if (NULL != proc->mbox) janet_mark(janet_wrap_abstract(proc->mbox));
    if (NULL != proc->sup) janet_mark(janet_wrap_abstract(proc->sup));
    if (NULL != proc->stash) janet_mark(janet_wrap_array(proc->stash));
    if (NULL != proc->monitors) janet_mark(janet_wrap_array(proc->monitors));
    janet_mark(proc->exit_reason);
    return 0;
}

/* No separate teardown: fiber and channels are GC objects (marked
 * above, freed by their own finalizers). The struct rides the
 * abstract payload. */
/* Marshal: NULL/NULL (refuses both directions — same-VM only, like
 * pool handles; cross-lane proxies land in P2). */

const JanetAbstractType janet_proc_type = {
    "core/proc",
    NULL, /* gc: the fiber marks the proc (see janet_mark fiber case) */
    janet_procat_mark,
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
static uint32_t janet_proc_next_serial(void) {
    uint32_t id = ++janet_vm.proc_serial;
    if (0 == id) id = ++janet_vm.proc_serial;
    return id;
}

/* P3.2: marshal-copy a spawn entry (same-VM, malloc transit buffer
 * like channel pack — proven shape). Closures copy by value
 * (bytecode travels); fibers suspended with C frames refuse (no
 * aliased stacks into the isolated heap). Caller roots the result
 * across subsequent allocations. */
static JanetFunction *janet_proc_copy_entry(JanetFunction *func) {
    JanetBuffer *buf = janet_malloc(sizeof(JanetBuffer));
    if (NULL == buf) {
        JANET_OUT_OF_MEMORY;
    }
    janet_buffer_init(buf, 64);
    janet_marshal(buf, janet_wrap_function(func), NULL, JANET_MARSHAL_UNSAFE);
    Janet copy = janet_unmarshal(buf->data, buf->count, JANET_MARSHAL_UNSAFE, NULL, NULL);
    janet_buffer_deinit(buf);
    janet_free(buf);
    if (!janet_checktype(copy, JANET_FUNCTION)) {
        janet_panic("proc entry did not survive copy");
    }
    return janet_unwrap_function(copy);
}

static Janet cfun_proc_spawn_impl(JanetFunction *func, int32_t cap, int isolated) {
    /* Fiber first (P3.5c): spawn-construction attributes to the child
     * via gc_alloc_ctx, so the process's memory is born owned (owner
     * id, strict bit, nursery, filing) and per-proc pressure/sweeps
     * cover it while globals stay quiet. */
    JanetFiber *fiber = janet_fiber(func, 64, 0, NULL);
    janet_assert(NULL != fiber, "bad proc fiber");
    /* P3.3: strict fibers opt their page claims into the isolated bit
     * (barrier-checked exports) and arm the VM-wide gate. Never
     * inherited, never unmarshalled. Set before the override so every
     * attributed birth agrees. */
    fiber->heap_strict = isolated;
    if (isolated) janet_vm.heap_strict_live = 1;
    /* P3.6: lane-shared stacks when enabled (same suspend discipline
     * as ev tasks — proc bodies block in ev ops, never raw-yield). */
    fiber->shared_stack = janet_vm.shared_stacks_on;
    fiber->proc = NULL;
    /* The proc handle stays parent-born (spawner-facing): no strict
     * bit, so registry/monitor/discipline stores of it never trip
     * the export rule — same as before the override. */
    JanetProcess *proc = janet_abstract(&janet_proc_type, sizeof(JanetProcess));
    Janet proc_j = janet_wrap_abstract(proc);
    /* Root across the allocations below (fiber + tables can collect). */
    janet_gcroot(proc_j);
    proc->fiber = NULL;
    proc->serial = janet_proc_next_serial();
    proc->done = 0;
    proc->exit_reason = janet_wrap_nil();
    proc->trap_exit = 0;
    proc->killed = 0;
    proc->isolated_heap = isolated;
    /* Parent env exists for the proto chain (parent-born, before the
     * override engages). */
    if (NULL != janet_vm.fiber && !janet_vm.fiber->env) {
        janet_vm.fiber->env = janet_table(0);
    }
    /* Attributed construction under try/finally: panics longjmp past
     * straight-line clears, which would strand a dangling override
     * (later births filing into a dead fiber's lists). */
    janet_vm.gc_alloc_ctx = fiber;
    {
        JanetTryState tstate;
        JanetSignal sig = janet_try(&tstate);
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
             * (janet_proc_ensure_mbox: send/try-send/recv/pid).
             * The sup channel stays EAGER: ev.c's completion hook
             * pushes the :down event only when the fiber carries a
             * supervisor channel, and the late-monitor semantic
             * (suite-proc test 11) needs that event buffered in the
             * channel even when nobody monitored during the life. */
            proc->mbox = NULL;
            proc->mbox_cap = (int32_t) cap;
            proc->sup = janet_channel_make(8);
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
                JanetFunction *copy = janet_proc_copy_entry(func);
                if (janet_vm.trace_strict) {
                    janet_strict_debug_envs(copy);
                }
                Janet copy_j = janet_wrap_function(copy);
                janet_gcroot(copy_j);
                janet_fiber_reset(fiber, copy, 0, NULL);
                fiber->heap_strict = isolated;
                janet_gcunroot(copy_j);
            }
            /* Env chain (mirrors ev/go) plus :proc-self for proc/self.
             * Importing the parent-born handle into the strict env is
             * allowed (imports pass; exports panic). Masks set here —
             * after any re-reset, which clears flags. */
            fiber->flags |=
                JANET_FIBER_MASK_ERROR |
                JANET_FIBER_MASK_USER0 |
                JANET_FIBER_MASK_USER1 |
                JANET_FIBER_MASK_USER2 |
                JANET_FIBER_MASK_USER3 |
                JANET_FIBER_MASK_USER4;
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
             * other dynamic bindings or top-level defs. JANET_PROC_ENV_TRACE
             * logs every narrow-env spawn as a migration aid (this is a
             * breaking change to what a spawned proc's (dyn :key) can see;
             * dyn's own nil-on-miss contract is deliberately NOT touched --
             * see proc-lanes.md's loud-error-vs-silent-nil answer). */
            fiber->env = janet_table(0);
            {
                JanetTable *spawner_env = janet_vm.fiber ? janet_vm.fiber->env : NULL;
                /* P3.3 export-rule carve-out: proc/spawn-isolated's strict
                 * heap discipline (janet_strict_debug_envs, the
                 * "shared-store, never captured" escape panic exercised by
                 * suite-proc's P3.3 test) depends on the full proto chain
                 * reaching the real shared parent env so a write through it
                 * is detectably a cross-heap escape, not a copy. Isolated
                 * procs are also not the demonstrated O(n^2) shape (they
                 * already pay marshal-copy per spawn); narrow only the
                 * plain (non-isolated) path here. */
                int inherit_full = isolated || (NULL != spawner_env &&
                    janet_truthy(janet_table_get(spawner_env, janet_ckeywordv("proc-inherit-env"))));
                if (inherit_full) {
                    fiber->env->proto = spawner_env;
                } else {
                    fiber->env->proto = NULL;
                    if (NULL != spawner_env) {
                        Janet v_out = janet_table_get(spawner_env, janet_ckeywordv("out"));
                        if (!janet_checktype(v_out, JANET_NIL)) {
                            janet_table_put(fiber->env, janet_ckeywordv("out"), v_out);
                        }
                        Janet v_err = janet_table_get(spawner_env, janet_ckeywordv("err"));
                        if (!janet_checktype(v_err, JANET_NIL)) {
                            janet_table_put(fiber->env, janet_ckeywordv("err"), v_err);
                        }
                    }
                    if (NULL != getenv("JANET_PROC_ENV_TRACE")) {
                        fprintf(stderr, "[JANET_PROC_ENV_TRACE] proc/spawn: narrow "
                                "env (out/err only, no proto-chain to spawner). "
                                "(dyn :key) for any other spawner binding now "
                                "returns nil where it used to resolve. Opt into "
                                "legacy full-chain inheritance with "
                                "(setdyn :proc-inherit-env true) before spawning "
                                "if this proc depends on it.\n");
                    }
                }
            }
            janet_table_put(fiber->env, janet_ckeywordv("proc-self"), proc_j);
            proc->fiber = fiber;
            /* P3.5: back-pointer for auto-trigger routing. */
            fiber->proc = proc;
            fiber->supervisor_channel = proc->sup;
            janet_vm.gc_alloc_ctx = NULL;
            janet_restore(&tstate);
        } else {
            janet_vm.gc_alloc_ctx = NULL;
            janet_restore(&tstate);
            janet_gcunroot(proc_j);
            janet_panicv(tstate.payload);
        }
    }
    janet_schedule(fiber, janet_wrap_nil());
    janet_gcunroot(proc_j);
    return proc_j;
}

JANET_CORE_FN(cfun_proc_spawn,
              "(proc/spawn thunk &opt capacity)",
              "Spawn a raw process: schedule 0-argument `thunk` as a "
              "fiber on this lane's event loop with an owned isolated "
              "mailbox (capacity `capacity`, default 16). The process "
              "reads its own mailbox with proc/recv and learns itself "
              "via proc/self. Returns the proc (a core/proc distinct "
              "type; refuses marshal — same-VM only). The historic O(n^2) "
              "hazard for keeping every handle in one flat array is GONE: the "
              "child env no longer proto-chains to the spawner (see "
              "docs/internals/proc-lanes.md) and a flat-array spawn burst now "
              "measures linear (2k/5k/10k procs in 212/234/274 ms). Keeping "
              "handles in an array to join on them is safe. With lanes active "
              "the handle is a [:pid lane serial ctl] tuple instead of an "
              "object -- see docs/internals/lanes-and-captured-state.md.") {
    janet_arity(argc, 1, 2);
    JanetFunction *func = janet_getfunction(argv, 0);
    if (func->def->min_arity > 0) {
        janet_panic("proc thunk must accept 0 arguments");
    }
    int32_t cap = janet_optnat(argv, argc, 1, 16);
    return janet_proc_spawn_routed(func, cap);
}

JANET_CORE_FN(cfun_proc_spawn_isolated,
              "(proc/spawn-isolated thunk &opt capacity)",
              "P3.2 strict spawn: like proc/spawn, but the entry is "
              "marshal-copied first — captured values arrive by value, "
              "never by alias (fresh fibers copy too; fibers suspended "
              "with C frames refuse with a clear error). First step "
              "toward per-proc majors: the process starts with no shared "
              "references of its own. Reads of shared state afterwards "
              "are still the body's discipline (barrier-checked in P3.3).") {
    janet_arity(argc, 1, 2);
    JanetFunction *func = janet_getfunction(argv, 0);
    if (func->def->min_arity > 0) {
        janet_panic("proc thunk must accept 0 arguments");
    }
    int32_t cap = janet_optnat(argv, argc, 1, 16);
    /* P3.5c: the entry copy happens inside the impl, attributed to
     * the child (its code lives in its heap). */
    return cfun_proc_spawn_impl(func, cap, 1);
}

JANET_CORE_FN(cfun_proc_isolated,
              "(proc/isolated? p)",
              "P3.2: true for strict (spawn-copied) processes.") {
    janet_fixarity(argc, 1);
    JanetProcess *proc = janet_getproc(argv, 0);
    return janet_wrap_boolean(proc->isolated_heap);
}

JANET_CORE_FN(cfun_proc_self,
              "(proc/self)",
              "The calling process, or nil outside a process.") {
    janet_fixarity(argc, 0);
    (void) argv;
    if (NULL == janet_vm.fiber || NULL == janet_vm.fiber->env) {
        return janet_wrap_nil();
    }
    Janet self = janet_table_get(janet_vm.fiber->env,
                                 janet_ckeywordv("proc-self"));
    if (janet_checktype(self, JANET_ABSTRACT) &&
            janet_abstract_type(janet_unwrap_abstract(self)) == &janet_proc_type) {
        return self;
    }
    return janet_wrap_nil();
}

JANET_CORE_FN(cfun_proc_send,
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
    janet_fixarity(argc, 2);
    if (janet_vm.coerce_error) {
        janet_panic("cannot proc/send inside janet_call");
    }
    /* Remote pid? Forward through the lane control channel. */
    if (janet_checktype(argv[0], JANET_TUPLE)) {
        const Janet *tup = janet_unwrap_tuple(argv[0]);
        if (4 == janet_tuple_length(tup) &&
                janet_checktype(tup[0], JANET_KEYWORD) &&
                janet_unwrap_keyword(tup[0]) == janet_unwrap_keyword(janet_ckeywordv("pid")) &&
                janet_checktype(tup[1], JANET_NUMBER) &&
                0 != (int64_t) janet_unwrap_number(tup[1])) {
            if (!janet_checktype(tup[2], JANET_NUMBER)) {
                janet_panic("remote pid serial must be a number");
            }
            JanetChannel *ctl = (JanetChannel *) janet_unwrap_abstract(tup[3]);
            Janet fwd[3];
            fwd[0] = janet_ckeywordv("proc-fwd");
            fwd[1] = tup[2];
            fwd[2] = argv[1];
            Janet fwdmsg = janet_wrap_tuple(janet_tuple_n(fwd, 3));
            if (janet_channel_push(ctl, fwdmsg, 0)) {
                janet_await();
            }
            return argv[0];
        }
    }
    JanetChannel *mbox = janet_proc_resolve_mbox(argv[0]);
    if (janet_channel_push(mbox, argv[1], 0)) {
        janet_await();
    }
    return argv[0];
}

JANET_CORE_FN(cfun_proc_try_send,
              "(proc/try-send p msg)",
              "Like `proc/send`, but never blocks: delivers when the "
              "mailbox has room (or a reader is parked) and reports "
              "`true`; returns `false` on a full mailbox. Remote pid "
              "tuples forward the same way, also without blocking.") {
    janet_fixarity(argc, 2);
    if (janet_vm.coerce_error) {
        janet_panic("cannot proc/send inside janet_call");
    }
    if (janet_checktype(argv[0], JANET_TUPLE)) {
        const Janet *tup = janet_unwrap_tuple(argv[0]);
        if (4 == janet_tuple_length(tup) &&
                janet_checktype(tup[0], JANET_KEYWORD) &&
                janet_unwrap_keyword(tup[0]) == janet_unwrap_keyword(janet_ckeywordv("pid")) &&
                janet_checktype(tup[1], JANET_NUMBER) &&
                0 != (int64_t) janet_unwrap_number(tup[1])) {
            if (!janet_checktype(tup[2], JANET_NUMBER)) {
                janet_panic("remote pid serial must be a number");
            }
            JanetChannel *ctl = (JanetChannel *) janet_unwrap_abstract(tup[3]);
            Janet fwd[3];
            fwd[0] = janet_ckeywordv("proc-fwd");
            fwd[1] = tup[2];
            fwd[2] = argv[1];
            Janet fwdmsg = janet_wrap_tuple(janet_tuple_n(fwd, 3));
            /* Try-mode push: 0 delivered, nonzero full. */
            return janet_wrap_boolean(0 == janet_channel_push(ctl, fwdmsg, 3));
        }
    }
    JanetChannel *mbox = janet_proc_resolve_mbox(argv[0]);
    return janet_wrap_boolean(0 == janet_channel_push(mbox, argv[1], 3));
}

JANET_CORE_FN(cfun_proc_recv,
              "(proc/recv &opt p)",
              "Take the next message from the calling process's own "
              "mailbox (P1.1: take-next only; selective receive lands in "
              "P1.2). With `p`, it must be the calling process (stealing "
              "from other mailboxes is an error). Suspends when empty.") {
    janet_arity(argc, 0, 1);
    JanetFiber *me = janet_vm.fiber;
    if (NULL == me) {
        janet_panic("proc/recv outside a process");
    }
    /* Phase 1 fast path: the P3.5 back-pointer IS the self identity —
     * one deref instead of an env table get per message (the 100k-agent
     * profile showed the lookup as ~40% of the recv side). */
    JanetProcess *me_proc = me->proc;
    Janet self;
    if (NULL != me_proc) {
        self = janet_wrap_abstract(me_proc);
    } else {
        if (NULL == me->env) {
            janet_panic("proc/recv outside a process");
        }
        self = janet_table_get(me->env, janet_ckeywordv("proc-self"));
        if (!(janet_checktype(self, JANET_ABSTRACT) &&
                janet_abstract_type(janet_unwrap_abstract(self)) == &janet_proc_type)) {
            janet_panic("proc/recv outside a process");
        }
    }
    if (argc >= 1) {
        /* Own pid tuple? Local node + serial must match (remote
         * serials repeat per lane — node disambiguates). */
        if (janet_checktype(argv[0], JANET_TUPLE)) {
            const Janet *tup = janet_unwrap_tuple(argv[0]);
            JanetProcess *mine = (JanetProcess *) janet_unwrap_abstract(self);
            if (!(4 == janet_tuple_length(tup) &&
                    janet_checktype(tup[0], JANET_KEYWORD) &&
                janet_unwrap_keyword(tup[0]) == janet_unwrap_keyword(janet_ckeywordv("pid")) &&
                    janet_checktype(tup[1], JANET_NUMBER) &&
                    0 == (int64_t) janet_unwrap_number(tup[1]) &&
                    janet_checktype(tup[2], JANET_NUMBER) &&
                    (uint32_t) janet_unwrap_number(tup[2]) == mine->serial)) {
                janet_panic("proc/recv reads only the caller's own mailbox");
            }
        } else {
            JanetProcess *want = janet_getproc(argv, 0);
            JanetProcess *mine = (JanetProcess *) janet_unwrap_abstract(self);
            if (want != mine) {
                janet_panic("proc/recv reads only the caller's own mailbox");
            }
        }
    }
    JanetProcess *proc = (JanetProcess *) janet_unwrap_abstract(self);
    if (janet_vm.coerce_error) {
        janet_panic("cannot proc/recv inside janet_call");
    }
    /* B6: first recv materializes the mailbox (an empty mailbox and
     * an unallocated one are behaviorally identical: no push site can
     * exist without the channel pointer, which only this, send, or
     * pid-of mint). */
    JanetChannel *mbox = janet_proc_ensure_mbox(proc);
    Janet item;
    if (janet_channel_pop(mbox, &item, 0)) {
        janet_schedule(janet_vm.root_fiber, item);
    }
    janet_await();
}

JANET_CORE_FN(cfun_proc_kill,
              "(proc/kill p reason)",
              "Untrappable kill: cancel the process fiber with `reason` "
              "(lands at the next resume/quantum boundary; monitors "
              "observe via the supervisor completion in P1.2). Dead "
              "processes are a silent no-op. Returns `p`. "
              "Milestone 2b: a remote pid tuple forwards [:kill serial "
              "reason] to the home lane's dispatcher, which applies the "
              "kill locally.") {
    janet_fixarity(argc, 2);
    /* Milestone 2b: remote pid — forward to the home lane. */
    if (janet_checktype(argv[0], JANET_TUPLE)) {
        const Janet *tup = janet_unwrap_tuple(argv[0]);
        if (4 == janet_tuple_length(tup) &&
                janet_checktype(tup[0], JANET_KEYWORD) &&
                janet_unwrap_keyword(tup[0]) == janet_unwrap_keyword(janet_ckeywordv("pid")) &&
                janet_checktype(tup[1], JANET_NUMBER) &&
                0 != (int64_t) janet_unwrap_number(tup[1])) {
            if (!janet_checktype(tup[2], JANET_NUMBER)) {
                janet_panic("remote pid serial must be a number");
            }
            JanetChannel *ctl = (JanetChannel *) janet_unwrap_abstract(tup[3]);
            Janet k[3];
            k[0] = janet_ckeywordv("kill");
            k[1] = tup[2];      /* serial */
            k[2] = argv[1];     /* reason */
            janet_channel_push(ctl, janet_wrap_tuple(janet_tuple_n(k, 3)), 0);
            return argv[0];
        }
        janet_panic("proc/kill: bad pid tuple");
    }
    JanetProcess *proc = janet_getproc(argv, 0);
    JanetFiber *fiber = proc->fiber;
    if (NULL != fiber && janet_fiber_can_resume(fiber)) {
        proc->killed = 1;
        janet_cancel(fiber, argv[1]);
        /* P3.5c: retire the mailbox now (purge the victim's pendings —
         * it may be parked in recv — and go quiet so later sends drop
         * silently, Erlang parity). Both objects are listed-valid here
         * (mid-mutator, pre-sweep). */
        if (NULL != proc->mbox) {
            /* P3.5c hook, now real: if the victim is parked in recv on
             * its own mailbox, cancel the pending and land the kill
             * immediately (a parked fiber has no next resume). */
            janet_channel_cancel_reader(proc->mbox, fiber, argv[1]);
        }
    }
    return argv[0];
}

JANET_CORE_FN(cfun_proc_alive,
              "(proc/alive? p)",
              "True while the process fiber can still resume.") {
    janet_fixarity(argc, 1);
    JanetProcess *proc = janet_getproc(argv, 0);
    return janet_wrap_boolean(NULL != proc->fiber &&
                              janet_fiber_can_resume(proc->fiber));
}

/* Is `x` a [:pid node serial chan] tuple? Shared so a remote handle can be
 * treated as a handle rather than as an unknown type. */
static int janet_proc_is_pid_tuple(Janet x) {
    if (!janet_checktype(x, JANET_TUPLE)) return 0;
    const Janet *tup = janet_unwrap_tuple(x);
    return 4 == janet_tuple_length(tup) &&
           janet_checktype(tup[0], JANET_KEYWORD) &&
           janet_unwrap_keyword(tup[0]) == janet_unwrap_keyword(janet_ckeywordv("pid")) &&
           janet_checktype(tup[1], JANET_NUMBER) &&
           janet_checktype(tup[2], JANET_NUMBER) &&
           janet_checktype(tup[3], JANET_ABSTRACT);
}

JANET_CORE_FN(cfun_proc_pid,
              "(proc/pid? x)",
              "True for a process handle of EITHER shape: a local core/proc\n"
              "object, or the [:pid lane serial chan] tuple that proc/spawn\n"
              "returns once lanes are active. Branch on this instead of the\n"
              "type; proc/pid, proc/serial and proc/alive? accept both.") {
    janet_fixarity(argc, 1);
    if (janet_checktype(argv[0], JANET_ABSTRACT) &&
            janet_abstract_type(janet_unwrap_abstract(argv[0])) == &janet_proc_type) {
        return janet_wrap_true();
    }
    return janet_wrap_boolean(janet_proc_is_pid_tuple(argv[0]));
}

/* P1.3 pid interop: a proc's portable face is a [:pid node serial
 * mbox] tuple (same shape as tasklet pids). Sends resolve through
 * the carried channel (no registry needed); monitor/kill need the
 * object (C lists/fibers live there). Tasklet.janet stays untouched:
 * tasklets already resolve unknown pids via the carried channel. */
static JanetChannel *janet_proc_resolve_mbox(Janet x) {
    if (janet_checktype(x, JANET_ABSTRACT) &&
            janet_abstract_type(janet_unwrap_abstract(x)) == &janet_proc_type) {
        /* B6: lazy mbox — home-VM access only (see ensure helper). */
        return janet_proc_ensure_mbox((JanetProcess *) janet_unwrap_abstract(x));
    }
    if (janet_checktype(x, JANET_TUPLE)) {
        const Janet *tup = janet_unwrap_tuple(x);
        /* Keywords intern to unique pointers: pointer compare is exact. */
        if (4 == janet_tuple_length(tup) &&
                janet_checktype(tup[0], JANET_KEYWORD) &&
                janet_unwrap_keyword(tup[0]) == janet_unwrap_keyword(janet_ckeywordv("pid")) &&
                janet_checktype(tup[3], JANET_ABSTRACT)) {
            void *abst = janet_unwrap_abstract(tup[3]);
            /* Carried mailbox: isolated (proc pid) or any channel
             * (tasklet pid — caller owns the triple convention). */
            return (JanetChannel *) abst;
        }
    }
    janet_panic("expected a proc or [:pid node serial chan] pid");
}

JANET_CORE_FN(cfun_proc_pid_of,
              "(proc/pid p)",
              "P1.3: the process's portable pid tuple [:pid node serial\n"
              "mbox] (same shape as tasklet pids — passes tasklet/pid?).\n"
              "Sends to it deliver via the carried mailbox; monitor/kill\n"
              "need the object.") {
    janet_fixarity(argc, 1);
    /* A pid tuple is already the portable face: return it unchanged so
     * proc/pid is idempotent across handle shapes. */
    if (janet_proc_is_pid_tuple(argv[0])) return argv[0];
    JanetProcess *proc = janet_getproc(argv, 0);
    Janet tup[4];
    tup[0] = janet_ckeywordv("pid");
    tup[1] = janet_wrap_integer(0);
    tup[2] = janet_wrap_integer((int64_t) proc->serial);
    /* B6: the carried mailbox is the pid tuple's routable face —
     * materialize it here (once) so sends through the tuple resolve. */
    tup[3] = janet_wrap_abstract(janet_proc_ensure_mbox(proc));
    return janet_wrap_tuple(janet_tuple_n(tup, 4));
}

JANET_CORE_FN(cfun_proc_serial,
              "(proc/serial p)",
              "P1.3: the process's numeric serial (pid tuple slot 2).\n"
              "Accepts a local core/proc or a pid tuple.") {
    janet_fixarity(argc, 1);
    if (janet_proc_is_pid_tuple(argv[0])) return janet_unwrap_tuple(argv[0])[2];
    JanetProcess *proc = janet_getproc(argv, 0);
    return janet_wrap_integer((int64_t) proc->serial);
}

JANET_CORE_FN(cfun_proc_stash,
              "(proc/stash p)",
              "P1.2: the process's selective-receive stash array (live "
              "object — scan/remove/push from Janet; the match loop in "
              "examples/proc.janet owns the discipline).") {
    janet_fixarity(argc, 1);
    JanetProcess *proc = janet_getproc(argv, 0);
    if (NULL == proc->stash) proc->stash = janet_array(0);
    return janet_wrap_array(proc->stash);
}

JANET_CORE_FN(cfun_proc_sup_of,
              "(proc/sup-of p)",
              "P1.2: the process's completion (supervisor) channel. "
              "Reapers take completion events here; bodies never touch it.") {
    janet_fixarity(argc, 1);
    JanetProcess *proc = janet_getproc(argv, 0);
    return janet_wrap_abstract(proc->sup);
}

JANET_CORE_FN(cfun_proc_monitor,
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
    janet_fixarity(argc, 3);
    /* Milestone 2b: remote pid tuples are handled at the dispatcher
     * level (:proc-monitor op from procx/monitor-chan) -- registration
     * must happen on the watched proc's home lane, where the C
     * completion hook delivers. */
    JanetProcess *watched = janet_getproc(argv, 0);
    if (NULL == watched->monitors) watched->monitors = janet_array(0);
    janet_array_push(watched->monitors, argv[2]);
    janet_array_push(watched->monitors, argv[1]);
    return argv[1];
}

JANET_CORE_FN(cfun_proc_monitors_of,
              "(proc/monitors-of p)",
              "P1.2: the flat [tag watcher] monitor array (live — reapers "
              "drain it, then clear it).") {
    janet_fixarity(argc, 1);
    JanetProcess *proc = janet_getproc(argv, 0);
    if (NULL == proc->monitors) proc->monitors = janet_array(0);
    return janet_wrap_array(proc->monitors);
}

JANET_CORE_FN(cfun_proc_record_exit,
              "(proc/record-exit p reason)",
              "P1.2: mark the process done with `reason` (reapers call "
              "this once, right after taking the completion event).") {
    janet_fixarity(argc, 2);
    JanetProcess *proc = janet_getproc(argv, 0);
    proc->done = 1;
    proc->exit_reason = argv[1];
    return argv[0];
}

JANET_CORE_FN(cfun_proc_done,
              "(proc/done? p)",
              "P1.2: true once the exit was recorded (late-monitor path).") {
    janet_fixarity(argc, 1);
    JanetProcess *proc = janet_getproc(argv, 0);
    return janet_wrap_boolean(proc->done);
}

/* B7.1 named process registry (erlang:register/2, whereis/1,
 * unregister/1). janet_vm.proc_registry is a lazily-allocated
 * keyword -> core/proc JanetTable, root-marked in gc.c alongside
 * top_dyns/profile_table and reset in vm.c's init/deinit (see those
 * sites for the full reasoning) -- no further GC plumbing needed
 * here; an ordinary JanetTable's own marking covers the registered
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
static JanetTable *janet_proc_registry_table(void) {
    if (NULL == janet_vm.proc_registry) {
        janet_vm.proc_registry = janet_table(0);
    }
    return janet_vm.proc_registry;
}

JANET_CORE_FN(cfun_proc_register,
              "(proc/register name proc)",
              "B7.1: register `proc` under keyword `name` for "
              "proc/whereis lookup. Panics if `name` is already "
              "registered to a process that is not proc/done? -- a "
              "live registration is never silently overwritten. If "
              "the previous holder of `name` has completed, the "
              "registration is replaced without requiring an "
              "explicit unregister first. Returns `proc`.") {
    janet_fixarity(argc, 2);
    if (!janet_checktype(argv[0], JANET_KEYWORD)) {
        janet_panicf("proc/register name must be a keyword, got %v", argv[0]);
    }
    JanetProcess *proc = janet_getproc(argv, 1);
    JanetTable *reg = janet_proc_registry_table();
    Janet prev = janet_table_get(reg, argv[0]);
    if (janet_checktype(prev, JANET_ABSTRACT) &&
            janet_abstract_type(janet_unwrap_abstract(prev)) == &janet_proc_type) {
        JanetProcess *prevproc = (JanetProcess *) janet_unwrap_abstract(prev);
        if (!prevproc->done) {
            janet_panicf("proc/register: name %v already registered to a live process",
                         argv[0]);
        }
    }
    janet_table_put(reg, argv[0], argv[1]);
    (void) proc;
    return argv[1];
}

JANET_CORE_FN(cfun_proc_whereis,
              "(proc/whereis name)",
              "B7.1: look up the process registered under keyword "
              "`name`, or nil if unregistered. If the registered "
              "process has since completed, the stale entry is "
              "removed and nil is returned -- a caller never "
              "receives a handle to an already-finished process.") {
    janet_fixarity(argc, 1);
    if (!janet_checktype(argv[0], JANET_KEYWORD)) {
        janet_panicf("proc/whereis name must be a keyword, got %v", argv[0]);
    }
    if (NULL == janet_vm.proc_registry) return janet_wrap_nil();
    Janet found = janet_table_get(janet_vm.proc_registry, argv[0]);
    if (janet_checktype(found, JANET_NIL)) return janet_wrap_nil();
    JanetProcess *proc = (JanetProcess *) janet_unwrap_abstract(found);
    if (proc->done) {
        janet_table_remove(janet_vm.proc_registry, argv[0]);
        return janet_wrap_nil();
    }
    return found;
}

JANET_CORE_FN(cfun_proc_unregister,
              "(proc/unregister name)",
              "B7.1: remove the registration under keyword `name`, "
              "if present. No-op, not an error, if nothing is "
              "registered under that name. Returns nil.") {
    janet_fixarity(argc, 1);
    if (!janet_checktype(argv[0], JANET_KEYWORD)) {
        janet_panicf("proc/unregister name must be a keyword, got %v", argv[0]);
    }
    if (NULL != janet_vm.proc_registry) {
        janet_table_remove(janet_vm.proc_registry, argv[0]);
    }
    return janet_wrap_nil();
}

JANET_CORE_FN(cfun_proc_exit_reason,
              "(proc/exit-reason p)",
              "P1.2: the recorded exit reason (:normal on clean exit).") {
    janet_fixarity(argc, 1);
    JanetProcess *proc = janet_getproc(argv, 0);
    return proc->exit_reason;
}

JANET_CORE_FN(cfun_proc_trap_exit,
              "(proc/trap-exit p flag)",
              "P1.4: set the loop-process trap flag (read back with\n"
              "proc/trap?). Trapping loops observe linked peers' deaths\n"
              "as :down messages and survive; untrapped loops die with\n"
              "the reason. Kill (proc/kill) bypasses the flag. Returns `p`.") {
    janet_fixarity(argc, 2);
    JanetProcess *proc = janet_getproc(argv, 0);
    proc->trap_exit = janet_truthy(argv[1]) ? 1 : 0;
    return argv[0];
}

JANET_CORE_FN(cfun_proc_trap,
              "(proc/trap? p)",
              "P1.4: the loop-process trap flag.") {
    janet_fixarity(argc, 1);
    JanetProcess *proc = janet_getproc(argv, 0);
    return janet_wrap_boolean(proc->trap_exit);
}

JANET_CORE_FN(cfun_proc_killed,
              "(proc/killed? p)",
              "P1.4b: true once proc/kill armed this process (terminal; "
              "the dispatch loop re-raises handler errors while armed so "
              "kills of handler-wedged fibers cannot be trapped by the "
              "loop's own protect).") {
    janet_fixarity(argc, 1);
    JanetProcess *proc = janet_getproc(argv, 0);
    return janet_wrap_boolean(proc->killed);
}

static const JanetRegExt proc_cfuns_ext[] = {
    JANET_CORE_REG("proc/spawn", cfun_proc_spawn),
    JANET_CORE_REG("proc/spawn-isolated", cfun_proc_spawn_isolated),
        JANET_CORE_REG("proc/self-pid", cfun_proc_self_pid),
        JANET_CORE_REG("proc/spawn-local", cfun_proc_spawn_local),
        JANET_CORE_REG("proc/lane-lookup", cfun_proc_lane_lookup),
        JANET_CORE_REG("proc/lanes", cfun_proc_lanes),
        JANET_CORE_REG("proc/lanes-start", cfun_proc_lanes_start),
        JANET_CORE_REG("proc/lanes-policy", cfun_proc_lanes_policy),
        JANET_CORE_REG("proc/lanes-stats", cfun_proc_lanes_stats),
        JANET_CORE_REG("proc/lane-pull", cfun_proc_lane_pull),
        JANET_CORE_REG("proc/lanes-stop", cfun_proc_lanes_stop),
        JANET_CORE_REG("proc/lane-pin", cfun_proc_lane_pin),
    JANET_CORE_REG("proc/isolated?", cfun_proc_isolated),
    JANET_CORE_REG("proc/self", cfun_proc_self),
    JANET_CORE_REG("proc/send", cfun_proc_send),
    JANET_CORE_REG("proc/try-send", cfun_proc_try_send),
    JANET_CORE_REG("proc/recv", cfun_proc_recv),
    JANET_CORE_REG("proc/kill", cfun_proc_kill),
    JANET_CORE_REG("proc/alive?", cfun_proc_alive),
    JANET_CORE_REG("proc/pid?", cfun_proc_pid),
    JANET_CORE_REG("proc/pid", cfun_proc_pid_of),
    JANET_CORE_REG("proc/serial", cfun_proc_serial),
    JANET_CORE_REG("proc/stash", cfun_proc_stash),
    JANET_CORE_REG("proc/sup-of", cfun_proc_sup_of),
    JANET_CORE_REG("proc/monitor", cfun_proc_monitor),
    JANET_CORE_REG("proc/monitors-of", cfun_proc_monitors_of),
    JANET_CORE_REG("proc/record-exit", cfun_proc_record_exit),
    JANET_CORE_REG("proc/done?", cfun_proc_done),
    JANET_CORE_REG("proc/exit-reason", cfun_proc_exit_reason),
    JANET_CORE_REG("proc/trap-exit", cfun_proc_trap_exit),
    JANET_CORE_REG("proc/trap?", cfun_proc_trap),
    JANET_CORE_REG("proc/killed?", cfun_proc_killed),
    JANET_CORE_REG("proc/register", cfun_proc_register),
    JANET_CORE_REG("proc/whereis", cfun_proc_whereis),
    JANET_CORE_REG("proc/unregister", cfun_proc_unregister),
    JANET_REG_END
};

void janet_lib_proc(JanetTable *env) {
    janet_core_cfuns_ext(env, NULL, (JanetRegExt *) proc_cfuns_ext);
    janet_register_abstract_type(&janet_proc_type);
}

#endif /* JANET_EV */
