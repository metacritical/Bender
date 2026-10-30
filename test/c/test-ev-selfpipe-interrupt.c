/*
 * Regression for a NULL-callback interrupt event retaining a listener.
 *
 * Build after `make`:
 *   cc -Isrc/include -Isrc/conf test/c/test-ev-selfpipe-interrupt.c \
 *      build/libbender.a -lm -lpthread -ldl -o build/test-ev-selfpipe-interrupt
 * Run:
 *   ./build/test-ev-selfpipe-interrupt
 */

#include "bender.h"
#include <stdio.h>

#ifndef BENDER_WINDOWS
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
    if (bender_init()) {
        fprintf(stderr, "FAIL: bender_init\n");
        return 1;
    }

#ifndef BENDER_WINDOWS
    signal(SIGALRM, on_timeout);
    alarm(3);
#endif

    if (!bender_loop_done()) {
        fprintf(stderr, "FAIL: event loop not initially empty\n");
        return 1;
    }

    bender_loop1_interrupt(NULL);
    bender_interpreter_interrupt_handled(NULL);
    if (bender_loop_done()) {
        fprintf(stderr, "FAIL: posted interrupt was not counted\n");
        return 1;
    }

    /* One loop turn consumes the self-pipe event; a leaked listener would
     * keep the loop alive and cause the following turn to block forever. */
    (void) bender_loop1();
    if (!bender_loop_done()) {
        fprintf(stderr, "FAIL: interrupt listener remained after drain\n");
        return 1;
    }

#ifndef BENDER_WINDOWS
    alarm(0);
#endif
    bender_deinit();
    puts("PASS: interrupt event drains without retaining a listener");
    return 0;
}
