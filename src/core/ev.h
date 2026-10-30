/* Internal event-loop channel API shared with proc.c (Step P1).
 * The push/pop wrappers own the suspension protocol (pending
 * registration + caller-side await); proc mailboxes reuse it so all
 * channel traffic keeps one funnel (Step 4.1 lane pin included). */

#ifndef BENDER_EV_H
#define BENDER_EV_H

#ifndef BENDER_AMALG
#include "features.h"
#include <bender.h>
#endif

/* Blocking give (mode 0), try-give (mode 3), supervisor push (mode 2).
 * Nonzero = caller must bender_await (value pends); zero = done. */
int bender_channel_push(BenderChannel *channel, Bender x, int mode);
/* proc/kill support: cancel a recv-parked reader with an error. */
int bender_channel_cancel_reader(BenderChannel *channel, BenderFiber *fiber, Bender reason);
/* Blocking take (mode 0), choice variants, supervisor drain (mode 2).
 * Nonzero = item obtained now (caller schedules root fiber with it);
 * zero = caller must bender_await. */
int bender_channel_pop(BenderChannel *channel, Bender *item, int is_choice);
/* Channel-pending registration lifecycle (unlink-on-free): entries
 * still queued when a fiber is freed are flagged dead in their
 * registration node, so every consumer path reclaims them without
 * ever dereferencing the freed fiber (replaces the unsound GC
 * probe-on-read and the never-wired bender_channel_purge_fiber). */
void bender_ev_chanpends_deinit(BenderFiber *fiber);
/* GC mark hook: fibers pin the channels they are parked behind. */
void bender_ev_chanpends_mark(BenderFiber *fiber);
void bender_channel_set_proc_alive(BenderChannel *channel, int alive);
/* P-lanes: the ev/thread worker subroutine, reused by proc/lanes-start to
 * launch resident lane VMs (same share-nothing thread body). */
BenderEVGenericMessage bender_go_thread_subr(BenderEVGenericMessage args);
/* P-lanes: C-level threaded channel constructor (the struct is
 * private to ev.c). */
BenderChannel *bender_channel_make_threaded_c(int32_t limit);
int bender_channel_proc_alive(BenderChannel *channel);

#endif
