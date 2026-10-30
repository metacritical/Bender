/* Proc processes (Step P1+): shared struct for proc.c (the type +
 * API) and gc.c (per-proc majors, P3.4). */

#ifndef BENDER_PROC_H
#define BENDER_PROC_H

#ifndef BENDER_AMALG
#include "features.h"
#include <bender.h>
#endif

typedef struct BenderProcess {
    /* B6 spawn-cost: the mailbox is LAZY — mbox is NULL at birth and
     * materialized on first real use (proc/send, proc/recv, proc/pid —
     * see bender_proc_ensure_mbox in proc.c). mbox_cap preserves the
     * spawn-time capacity for that allocation. Every consumer already
     * null-guards (ev.c retire, gc.c mark, proc.c kill), and the
     * core/proc abstract never leaves its home VM (marshal refuses),
     * so ensure-on-first-use has no cross-thread race. The sup channel
     * stays EAGER: ev.c's completion hook pushes the :down event only
     * when fiber->supervisor_channel is non-NULL, and suite-proc test
     * 11's late-monitor path depends on that event being buffered
     * even when nobody monitored during the proc's life. */
    BenderChannel *mbox;
    BenderChannel *sup;  /* plain completion channel (embeds fiber) */
    int32_t mbox_cap;   /* capacity for the lazy mbox allocation */
    BenderFiber *fiber;  /* the process fiber (ev/go-scheduled) */
    uint32_t serial;    /* pid serial (never 0) */
    int done;           /* completion observed (quit/kill fast path) */
    /* P1.2: selective-recv stash (Bender-scanned) + monitor list
     * (flat [tag watcher] pairs, Bender-pumped via reapers) +
     * recorded exit reason (late monitors). All marked. */
    BenderArray *stash;
    BenderArray *monitors;
    Bender exit_reason;
    /* P1.4: trap-exit flag (loop processes consult it on :down). */
    int trap_exit;
    /* P1.4b: kill-armed flag (set by proc/kill, never cleared). */
    int killed;
    /* P3.2 strict isolation: entry was marshal-copied at spawn. */
    int isolated_heap;
} BenderProcess;

/* Depth cap for transitive proc->fiber->env->proc marking inside a SCOPED
 * minor (see bender_procat_mark). A chain of proc captures would otherwise be
 * walked to its full length on every per-owner minor, which is O(N^2) across a
 * spawn burst. Global collections ignore this and mark transitively.
 * 16 comfortably covers genuine shared state between neighbouring procs while
 * making the per-minor walk independent of chain length. */
#define BENDER_PROC_MARK_CHAIN_DEPTH 16

extern BENDER_THREAD_LOCAL int bender_proc_mark_depth;

/* Abstract type (defined in proc.c, registered with the core). */
extern const BenderAbstractType bender_proc_type;

#endif
