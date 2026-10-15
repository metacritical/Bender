/*
 * Regression for a NULL-callback interrupt event retaining a listener.
 *
 * Build after `make`:
 *   cc -Isrc/include -Isrc/conf test/c/test-ev-selfpipe-interrupt.c \
 *      build/libjanet.a -lm -lpthread -ldl -o build/test-ev-selfpipe-interrupt
 * Run:
 *   ./build/test-ev-selfpipe-interrupt
 */

#include "janet.h"
#include <stdio.h>

#ifndef JANET_WINDOWS
#include <signal.h>
#include <unistd.h>

static void on_timeout(int signal_number) {
    (void) signal_number;
    static const char message[] = "FAIL: interrupt event did not drain\n";
    (void) write(STDERR_FILENO, message, sizeof(message) - 1);
    _exit(124);
}
#endif

int main(void) {
    if (janet_init()) {
        fprintf(stderr, "FAIL: janet_init\n");
        return 1;
    }

#ifndef JANET_WINDOWS
    signal(SIGALRM, on_timeout);
    alarm(3);
#endif

    if (!janet_loop_done()) {
        fprintf(stderr, "FAIL: event loop not initially empty\n");
        return 1;
    }

    janet_loop1_interrupt(NULL);
    janet_interpreter_interrupt_handled(NULL);
    if (janet_loop_done()) {
        fprintf(stderr, "FAIL: posted interrupt was not counted\n");
        return 1;
    }

    /* One loop turn consumes the self-pipe event; a leaked listener would
     * keep the loop alive and cause the following turn to block forever. */
    (void) janet_loop1();
    if (!janet_loop_done()) {
        fprintf(stderr, "FAIL: interrupt listener remained after drain\n");
        return 1;
    }

#ifndef JANET_WINDOWS
    alarm(0);
#endif
    janet_deinit();
    puts("PASS: interrupt event drains without retaining a listener");
    return 0;
}
