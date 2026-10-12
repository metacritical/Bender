/* Internal event-loop channel API shared with proc.c (Step P1).
 * The push/pop wrappers own the suspension protocol (pending
 * registration + caller-side await); proc mailboxes reuse it so all
 * channel traffic keeps one funnel (Step 4.1 lane pin included). */

#ifndef JANET_EV_H
#define JANET_EV_H

#ifndef JANET_AMALG
#include "features.h"
#include <janet.h>
#endif

/* Blocking give (mode 0), try-give (mode 3), supervisor push (mode 2).
 * Nonzero = caller must janet_await (value pends); zero = done. */
int janet_channel_push(JanetChannel *channel, Janet x, int mode);
/* Blocking take (mode 0), choice variants, supervisor drain (mode 2).
 * Nonzero = item obtained now (caller schedules root fiber with it);
 * zero = caller must janet_await. */
int janet_channel_pop(JanetChannel *channel, Janet *item, int is_choice);
/* Channel-pending registration lifecycle (unlink-on-free): entries
 * still queued when a fiber is freed are flagged dead in their
 * registration node, so every consumer path reclaims them without
 * ever dereferencing the freed fiber (replaces the unsound GC
 * probe-on-read and the never-wired janet_channel_purge_fiber). */
void janet_ev_chanpends_deinit(JanetFiber *fiber);
/* GC mark hook: fibers pin the channels they are parked behind. */
void janet_ev_chanpends_mark(JanetFiber *fiber);
void janet_channel_set_proc_alive(JanetChannel *channel, int alive);
/* P-lanes: the ev/thread worker subroutine, reused by proc/lanes-start to
 * launch resident lane VMs (same share-nothing thread body). */
JanetEVGenericMessage janet_go_thread_subr(JanetEVGenericMessage args);
/* P-lanes: C-level threaded channel constructor (the struct is
 * private to ev.c). */
JanetChannel *janet_channel_make_threaded_c(int32_t limit);
int janet_channel_proc_alive(JanetChannel *channel);

#endif
