/*
 * A full event self-pipe must fail promptly rather than block VM shutdown.
 * Compile after `make build/libjanet.a`:
 *   cc -Isrc/include -Isrc/conf -Isrc/core \
 *      test/c/test-ev-selfpipe-saturation.c build/libjanet.a \
 *      -lm -lpthread -ldl -o /tmp/janet-test-selfpipe-saturation
 * This probes the existing bounded-failure policy, not successful delivery:
 * exhaustion currently asserts. Keep it in a child so the test runner lives.
 */
#include "janet.h"
#include "state.h"
#include <stdio.h>

#if !defined(JANET_WINDOWS)
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

static void unused_callback(JanetEVGenericMessage msg) { (void) msg; }

int main(void) {
    pid_t child = fork();
    if (child < 0) { perror("fork"); return 1; }
    if (child == 0) {
        /* An alarm distinguishes bounded failure from a blocking write. */
        alarm(3);
        if (janet_init()) _exit(2);
        int fd = janet_vm.selfpipe[1];
        if (!(fcntl(fd, F_GETFL) & O_NONBLOCK)) _exit(3);
        char bytes[256] = {0};
        for (;;) {
            ssize_t n = write(fd, bytes, sizeof(bytes));
            if (n > 0) continue;
            if (n < 0 && errno == EINTR) continue;
            if (n < 0 && errno == EAGAIN) break;
            _exit(4);
        }
        JanetEVGenericMessage msg = {0};
        janet_ev_post_event(&janet_vm, unused_callback, msg);
        _exit(5); /* Full-pipe posting must never silently report success. */
    }
    int status = 0;
    if (waitpid(child, &status, 0) != child) { perror("waitpid"); return 1; }
    if (!WIFSIGNALED(status) || WTERMSIG(status) != SIGABRT) {
        fprintf(stderr, "FAIL: full-pipe post terminated with status %d (expected bounded SIGABRT)\n", status);
        return 1;
    }
    puts("PASS: full nonblocking self-pipe fails promptly rather than hanging");
    return 0;
}
#else
int main(void) { puts("SKIP: POSIX self-pipe saturation test"); return 0; }
#endif
