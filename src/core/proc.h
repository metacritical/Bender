/* Proc processes (Step P1+): shared struct for proc.c (the type +
 * API) and gc.c (per-proc majors, P3.4). */

#ifndef JANET_PROC_H
#define JANET_PROC_H

#ifndef JANET_AMALG
#include "features.h"
#include <janet.h>
#endif

typedef struct JanetProcess {
    JanetChannel *mbox; /* owned isolated mailbox (copy in/out) */    JanetChannel *sup;  /* plain completion channel (embeds fiber) */
    JanetFiber *fiber;  /* the process fiber (ev/go-scheduled) */
    uint32_t serial;    /* pid serial (never 0) */
    int done;           /* completion observed (quit/kill fast path) */
    /* P1.2: selective-recv stash (Janet-scanned) + monitor list
     * (flat [tag watcher] pairs, Janet-pumped via reapers) +
     * recorded exit reason (late monitors). All marked. */
    JanetArray *stash;
    JanetArray *monitors;
    Janet exit_reason;
    /* P1.4: trap-exit flag (loop processes consult it on :down). */
    int trap_exit;
    /* P1.4b: kill-armed flag (set by proc/kill, never cleared). */
    int killed;
    /* P3.2 strict isolation: entry was marshal-copied at spawn. */
    int isolated_heap;
} JanetProcess;

/* Abstract type (defined in proc.c, registered with the core). */
extern const JanetAbstractType janet_proc_type;

#endif
