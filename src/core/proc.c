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
#include <janet.h>
#include "util.h"
#include "gc.h"
#include "state.h"
#include "fiber.h"
#include "ev.h"
#endif

#ifdef JANET_EV

/* Forward: abstract type defined below (used by the getter). */
extern const JanetAbstractType janet_proc_type;

typedef struct JanetProcess {
    JanetChannel *mbox; /* owned isolated mailbox (copy in/out) */
    JanetChannel *sup;  /* plain completion channel (embeds fiber) */
    JanetFiber *fiber;  /* the process fiber (ev/go-scheduled) */
    uint32_t serial;    /* pid serial (never 0) */
    int done;           /* completion observed (quit/kill fast path) */
} JanetProcess;

static JanetProcess *janet_getproc(const Janet *argv, int32_t n) {
    return janet_getabstract(argv, n, &janet_proc_type);
}

/* Mark everything the process owns (fiber, both channels). */
static int janet_procat_mark(void *p, size_t size) {
    (void) size;
    JanetProcess *proc = (JanetProcess *) p;
    if (NULL != proc->fiber) janet_mark(janet_wrap_fiber(proc->fiber));
    if (NULL != proc->mbox) janet_mark(janet_wrap_abstract(proc->mbox));
    if (NULL != proc->sup) janet_mark(janet_wrap_abstract(proc->sup));
    return 0;
}

/* No separate teardown: fiber and channels are GC objects (marked
 * above, freed by their own finalizers). The struct rides the
 * abstract payload. */
/* Marshal: NULL/NULL (refuses both directions — same-VM only, like
 * pool handles; cross-lane proxies land in P2). */

const JanetAbstractType janet_proc_type = {
    "core/proc",
    NULL, /* gc */
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

JANET_CORE_FN(cfun_proc_spawn,
              "(proc/spawn thunk &opt capacity)",
              "Spawn a raw process: schedule 0-argument `thunk` as a "
              "fiber on this lane's event loop with an owned isolated "
              "mailbox (capacity `capacity`, default 16). The process "
              "reads its own mailbox with proc/recv and learns itself "
              "via proc/self. Returns the proc (a core/proc distinct "
              "type; refuses marshal — same-VM only for now).") {
    janet_arity(argc, 1, 2);
    JanetFunction *func = janet_getfunction(argv, 0);
    if (func->def->min_arity > 0) {
        janet_panic("proc thunk must accept 0 arguments");
    }
    int32_t cap = janet_optnat(argv, argc, 1, 16);
    JanetProcess *proc = janet_abstract(&janet_proc_type, sizeof(JanetProcess));
    Janet proc_j = janet_wrap_abstract(proc);
    /* Root across the allocations below (fiber + tables can collect). */
    janet_gcroot(proc_j);
    proc->mbox = janet_channel_make_isolated((uint32_t) cap);
    proc->sup = janet_channel_make(8);
    proc->fiber = NULL;
    proc->serial = janet_proc_next_serial();
    proc->done = 0;
    JanetFiber *fiber = janet_fiber(func, 64, 0, NULL);
    janet_assert(NULL != fiber, "bad proc fiber");
    fiber->flags |=
        JANET_FIBER_MASK_ERROR |
        JANET_FIBER_MASK_USER0 |
        JANET_FIBER_MASK_USER1 |
        JANET_FIBER_MASK_USER2 |
        JANET_FIBER_MASK_USER3 |
        JANET_FIBER_MASK_USER4;
    /* Env chain (mirrors ev/go) plus :proc-self for proc/self. */
    if (!janet_vm.fiber->env) {
        janet_vm.fiber->env = janet_table(0);
    }
    fiber->env = janet_table(0);
    fiber->env->proto = janet_vm.fiber->env;
    janet_table_put(fiber->env, janet_ckeywordv("proc-self"), proc_j);
    proc->fiber = fiber;
    fiber->supervisor_channel = proc->sup;
    janet_schedule(fiber, janet_wrap_nil());
    janet_gcunroot(proc_j);
    return proc_j;
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
              "Deliver `msg` to process `p`'s mailbox (marshal-copied, "
              "like all isolated traffic). Blocks at capacity. Returns `p`.") {
    janet_fixarity(argc, 2);
    JanetProcess *proc = janet_getproc(argv, 0);
    if (janet_vm.coerce_error) {
        janet_panic("cannot proc/send inside janet_call");
    }
    if (janet_channel_push(proc->mbox, argv[1], 0)) {
        janet_await();
    }
    return argv[0];
}

JANET_CORE_FN(cfun_proc_recv,
              "(proc/recv &opt p)",
              "Take the next message from the calling process's own "
              "mailbox (P1.1: take-next only; selective receive lands in "
              "P1.2). With `p`, it must be the calling process (stealing "
              "from other mailboxes is an error). Suspends when empty.") {
    janet_arity(argc, 0, 1);
    JanetFiber *me = janet_vm.fiber;
    if (NULL == me || NULL == me->env) {
        janet_panic("proc/recv outside a process");
    }
    Janet self = janet_table_get(me->env, janet_ckeywordv("proc-self"));
    if (!(janet_checktype(self, JANET_ABSTRACT) &&
            janet_abstract_type(janet_unwrap_abstract(self)) == &janet_proc_type)) {
        janet_panic("proc/recv outside a process");
    }
    if (argc >= 1) {
        JanetProcess *want = janet_getproc(argv, 0);
        JanetProcess *mine = (JanetProcess *) janet_unwrap_abstract(self);
        if (want != mine) {
            janet_panic("proc/recv reads only the caller's own mailbox");
        }
    }
    JanetProcess *proc = (JanetProcess *) janet_unwrap_abstract(self);
    if (janet_vm.coerce_error) {
        janet_panic("cannot proc/recv inside janet_call");
    }
    Janet item;
    if (janet_channel_pop(proc->mbox, &item, 0)) {
        janet_schedule(janet_vm.root_fiber, item);
    }
    janet_await();
}

JANET_CORE_FN(cfun_proc_kill,
              "(proc/kill p reason)",
              "Untrappable kill: cancel the process fiber with `reason` "
              "(lands at the next resume/quantum boundary; monitors "
              "observe via the supervisor completion in P1.2). Dead "
              "processes are a silent no-op. Returns `p`.") {
    janet_fixarity(argc, 2);
    JanetProcess *proc = janet_getproc(argv, 0);
    JanetFiber *fiber = proc->fiber;
    if (NULL != fiber && janet_fiber_can_resume(fiber)) {
        janet_cancel(fiber, argv[1]);
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

JANET_CORE_FN(cfun_proc_pid,
              "(proc/pid? x)",
              "True for core/proc process handles.") {
    janet_fixarity(argc, 1);
    return janet_wrap_boolean(janet_checktype(argv[0], JANET_ABSTRACT) &&
        janet_abstract_type(janet_unwrap_abstract(argv[0])) == &janet_proc_type);
}

static const JanetRegExt proc_cfuns_ext[] = {
    JANET_CORE_REG("proc/spawn", cfun_proc_spawn),
    JANET_CORE_REG("proc/self", cfun_proc_self),
    JANET_CORE_REG("proc/send", cfun_proc_send),
    JANET_CORE_REG("proc/recv", cfun_proc_recv),
    JANET_CORE_REG("proc/kill", cfun_proc_kill),
    JANET_CORE_REG("proc/alive?", cfun_proc_alive),
    JANET_CORE_REG("proc/pid?", cfun_proc_pid),
    JANET_REG_END
};

void janet_lib_proc(JanetTable *env) {
    janet_core_cfuns_ext(env, NULL, (JanetRegExt *) proc_cfuns_ext);
    janet_register_abstract_type(&janet_proc_type);
}

#endif /* JANET_EV */
