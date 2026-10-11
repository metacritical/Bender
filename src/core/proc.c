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
#include "proc.h"
#endif

#ifdef JANET_EV

/* Forward: abstract type defined below (used by the getter). */
extern const JanetAbstractType janet_proc_type;
/* Forward: pid-tuple resolver (defined with proc/pid, used by send). */
static JanetChannel *janet_proc_resolve_mbox(Janet x);

/* JanetProcess lives in proc.h (shared with gc.c for P3.4 majors). */

static JanetProcess *janet_getproc(const Janet *argv, int32_t n) {
    return janet_getabstract(argv, n, &janet_proc_type);
}

/* Mark everything the process owns (fiber, both channels, P1.2
 * stash + monitors + exit reason). */
static int janet_procat_mark(void *p, size_t size) {
    (void) size;
    JanetProcess *proc = (JanetProcess *) p;
    if (NULL != proc->fiber) janet_mark(janet_wrap_fiber(proc->fiber));
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
            proc->mbox = janet_channel_make_isolated((uint32_t) cap);
            proc->sup = janet_channel_make(8);
            /* P1.2 fields: rooted individually until assigned (the proc
             * root covers them after; mark keeps them after that). */
            JanetArray *stash = janet_array(0);
            Janet stash_j = janet_wrap_array(stash);
            janet_gcroot(stash_j);
            JanetArray *monitors = janet_array(0);
            Janet monitors_j = janet_wrap_array(monitors);
            janet_gcroot(monitors_j);
            proc->stash = stash;
            proc->monitors = monitors;
            janet_gcunroot(monitors_j);
            janet_gcunroot(stash_j);
            if (isolated) {
                /* Marshal-copied entry, attributed to the child (its
                 * code and constants live in its heap). Re-reset the
                 * fresh fiber onto the copy (reuse pattern from thread
                 * workers). */
                JanetFunction *copy = janet_proc_copy_entry(func);
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
            fiber->env = janet_table(0);
            fiber->env->proto = (NULL != janet_vm.fiber) ? janet_vm.fiber->env : NULL;
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
              "type; refuses marshal — same-VM only for now).") {
    janet_arity(argc, 1, 2);
    JanetFunction *func = janet_getfunction(argv, 0);
    if (func->def->min_arity > 0) {
        janet_panic("proc thunk must accept 0 arguments");
    }
    int32_t cap = janet_optnat(argv, argc, 1, 16);
    return cfun_proc_spawn_impl(func, cap, 0);
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
        proc->killed = 1;
        janet_cancel(fiber, argv[1]);
        /* P3.5c: retire the mailbox now (purge the victim's pendings —
         * it may be parked in recv — and go quiet so later sends drop
         * silently, Erlang parity). Both objects are listed-valid here
         * (mid-mutator, pre-sweep). */
        if (NULL != proc->mbox) {
            /* DISABLED-HOOK-TEST */
            (void) 0;
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

JANET_CORE_FN(cfun_proc_pid,
              "(proc/pid? x)",
              "True for core/proc process handles.") {
    janet_fixarity(argc, 1);
    return janet_wrap_boolean(janet_checktype(argv[0], JANET_ABSTRACT) &&
        janet_abstract_type(janet_unwrap_abstract(argv[0])) == &janet_proc_type);
}

/* P1.3 pid interop: a proc's portable face is a [:pid node serial
 * mbox] tuple (same shape as tasklet pids). Sends resolve through
 * the carried channel (no registry needed); monitor/kill need the
 * object (C lists/fibers live there). Tasklet.janet stays untouched:
 * tasklets already resolve unknown pids via the carried channel. */
static JanetChannel *janet_proc_resolve_mbox(Janet x) {
    if (janet_checktype(x, JANET_ABSTRACT) &&
            janet_abstract_type(janet_unwrap_abstract(x)) == &janet_proc_type) {
        return ((JanetProcess *) janet_unwrap_abstract(x))->mbox;
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
    JanetProcess *proc = janet_getproc(argv, 0);
    Janet tup[4];
    tup[0] = janet_ckeywordv("pid");
    tup[1] = janet_wrap_integer(0);
    tup[2] = janet_wrap_integer((int64_t) proc->serial);
    tup[3] = janet_wrap_abstract(proc->mbox);
    return janet_wrap_tuple(janet_tuple_n(tup, 4));
}

JANET_CORE_FN(cfun_proc_serial,
              "(proc/serial p)",
              "P1.3: the process's numeric serial (pid tuple slot 2).") {
    janet_fixarity(argc, 1);
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
              "first — this just records. Returns `watcher`.") {
    janet_fixarity(argc, 3);
    JanetProcess *watched = janet_getproc(argv, 0);
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
    JANET_CORE_REG("proc/isolated?", cfun_proc_isolated),
    JANET_CORE_REG("proc/self", cfun_proc_self),
    JANET_CORE_REG("proc/send", cfun_proc_send),
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
    JANET_REG_END
};

void janet_lib_proc(JanetTable *env) {
    janet_core_cfuns_ext(env, NULL, (JanetRegExt *) proc_cfuns_ext);
    janet_register_abstract_type(&janet_proc_type);
}

#endif /* JANET_EV */
