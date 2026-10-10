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

#endif
