/* tools/fiber-c-test.c — Phase 1.1b proof (see docs/internals/native-fibers.md).
 *
 * Proves that real C call chains on a fiber's native stack survive
 * yield/resume via bender_fiber_yield: C locals intact across swaps,
 * resume values plumbed, rooted values surviving collections that run
 * while suspended, cancel unwinding through C frames, and clean marshal
 * refusal for C-frozen fibers.
 *
 * Build:  cc -O2 -Ibuild -Isrc/include tools/fiber-c-test.c \
 *             build/libbender.a -lm -lpthread -o /tmp/fiber-c-test
 * (or: make fibertest). Exit 0 on success, 1 on any failure. */

#include <bender.h>
/* state.h for bender_vm (committed-window reads in the D3 probes).
 * Needs -Isrc/core (see the fibertest Makefile line). */
#include "state.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>
#ifndef BENDER_WINDOWS
#include <unistd.h>
#include <sys/wait.h>
#endif
#ifdef BENDER_WINDOWS
#include <malloc.h>
#else
#include <alloca.h>
#endif

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); failures++; } \
    else { printf("ok: %s\n", msg); } \
} while (0)

static int streq_cstring(Bender x, const char *s) {
    if (!bender_checktype(x, BENDER_STRING)) return 0;
    BenderString jstr = bender_unwrap_string(x);
    int32_t len = bender_string_length(jstr);
    return len == (int32_t) strlen(s) && !memcmp(jstr, s, (size_t) len);
}

/* Direct yield from C with C locals live across two suspensions. */
static Bender c_yield1(int32_t argc, Bender *argv) {
    bender_arity(argc, 1, 1);
    Bender tag = argv[0];
    int canary = 0x5A5A5A5A;
    int counter = 0;
    char tagbuf[32];
    snprintf(tagbuf, sizeof(tagbuf), "tag-ok");
    (void) tag;
    Bender r1 = bender_fiber_yield(bender_cstringv("first"));
    counter++;
    if (canary != 0x5A5A5A5A) bender_panic("canary clobbered after yield 1");
    Bender r2 = bender_fiber_yield(r1);
    counter++;
    if (canary != 0x5A5A5A5A) bender_panic("canary clobbered after yield 2");
    if (counter != 2) bender_panic("counter wrong");
    Bender tup[3];
    tup[0] = bender_cstringv(tagbuf);
    tup[1] = r2;
    tup[2] = bender_wrap_integer(counter);
    return bender_wrap_tuple(bender_tuple_n(tup, 3));
}

/* Three nested plain-C frames, innermost yields. */
static Bender c_inner(void) {
    int canary = 0xA5A5;
    const char *name = "inner";
    Bender r = bender_fiber_yield(bender_cstringv("deep"));
    if (canary != 0xA5A5) bender_panic("inner canary clobbered");
    if (strcmp(name, "inner") != 0) bender_panic("inner string local clobbered");
    return r;
}

static Bender c_mid(Bender x) {
    int canary = 0x1234;
    (void) x;
    Bender r = c_inner();
    if (canary != 0x1234) bender_panic("mid canary clobbered");
    Bender tup[2];
    tup[0] = r;
    tup[1] = bender_cstringv("mid");
    return bender_wrap_tuple(bender_tuple_n(tup, 2));
}

static Bender c_outer(int32_t argc, Bender *argv) {
    bender_arity(argc, 0, 0);
    (void) argv;
    int canary = 0x7777;
    Bender r = c_mid(bender_wrap_nil());
    if (canary != 0x7777) bender_panic("outer canary clobbered");
    Bender tup[2];
    tup[0] = r;
    tup[1] = bender_cstringv("outer");
    return bender_wrap_tuple(bender_tuple_n(tup, 2));
}

/* A table held via the shadow root stack (Phase 2b-i) across a
 * suspension + collection. No manual gcroot: the push/pop_to scope keeps
 * it alive, mirroring what generated code will emit around calls. */
static Bender c_rooted(int32_t argc, Bender *argv) {
    bender_arity(argc, 0, 0);
    (void) argv;
    size_t mark = bender_gcshadow_mark();
    BenderTable *t = bender_table(0);
    bender_table_put(t, bender_ckeywordv("k"), bender_cstringv("v-rooted"));
    bender_gcshadow_push(bender_wrap_table(t));
    Bender r = bender_fiber_yield(bender_wrap_table(t));
    /* A collection ran while suspended (see driver). The shadow-pinned
     * table and the C local pointing at it must still be valid. */
    Bender check = bender_table_get(t, bender_ckeywordv("k"));
    int ok = streq_cstring(check, "v-rooted");
    bender_gcshadow_pop_to(mark);
    if (!ok) bender_panic("rooted table did not survive suspension + collect");
    Bender tup[2];
    tup[0] = check;
    tup[1] = r;
    return bender_wrap_tuple(bender_tuple_n(tup, 2));
}

/* Deep C call chain holding one value per level on the shadow stack.
 * Each level pushes its table, recurses, verifies on the way out, and
 * pops exactly its own push -- the scoped discipline generated code
 * will follow. No manual gcroot anywhere on this path. */
static Bender c_deep_rec(int level, int *sump) {
    size_t mark = bender_gcshadow_mark();
    BenderTable *t = bender_table(0);
    bender_table_put(t, bender_ckeywordv("level"), bender_wrap_integer(level));
    bender_gcshadow_push(bender_wrap_table(t));
    Bender r;
    if (level == 0) {
        r = bender_fiber_yield(bender_cstringv("bottom"));
    } else {
        r = c_deep_rec(level - 1, sump);
    }
    Bender lv = bender_table_get(t, bender_ckeywordv("level"));
    if (!bender_checktype(lv, BENDER_NUMBER) || bender_unwrap_integer(lv) != level) {
        bender_panic("deep shadow table corrupted");
    }
    *sump += level;
    bender_gcshadow_pop_to(mark);
    return r;
}

static Bender c_deep(int32_t argc, Bender *argv) {
    bender_arity(argc, 1, 1);
    int depth = bender_unwrap_integer(argv[0]);
    int sum = 0;
    size_t entry = bender_gcshadow_mark();
    Bender r = c_deep_rec(depth, &sum);
    if (bender_gcshadow_mark() != entry) {
        bender_panic("shadow stack unbalanced across deep chain");
    }
    Bender tup[2];
    tup[0] = r;
    tup[1] = bender_wrap_integer(sum);
    return bender_wrap_tuple(bender_tuple_n(tup, 2));
}

static BenderFiber *run_fn(BenderTable *env, const char *name) {
    Bender v = bender_wrap_nil();
    if (bender_resolve(env, bender_csymbol(name), &v) == BENDER_BINDING_NONE) {
        printf("FAIL: cannot resolve %s\n", name);
        failures++;
        return NULL;
    }
    BenderFunction *fn = bender_unwrap_function(v);
    BenderFiber *f = bender_fiber(fn, 64, 0, NULL);
    f->env = env;
    bender_gcroot(bender_wrap_fiber(f));
    return f;
}

static BenderFiber *run_fn1(BenderTable *env, const char *name, Bender arg) {
    Bender v = bender_wrap_nil();
    if (bender_resolve(env, bender_csymbol(name), &v) == BENDER_BINDING_NONE) {
        printf("FAIL: cannot resolve %s\n", name);
        failures++;
        return NULL;
    }
    BenderFunction *fn = bender_unwrap_function(v);
    BenderFiber *f = bender_fiber(fn, 64, 1, &arg);
    f->env = env;
    bender_gcroot(bender_wrap_fiber(f));
    return f;
}

/* Phase D: grow-on-fault. Fat-frame C recursion to a calibrated depth:
 * depth 1500 x ~512B ≈ 750KB forces several growth faults on a 64KB
 * initial commit; the result proves transparent resume across growths.
 * alloca (not a volatile array: optimizers shrink those unpredictably)
 * pins the frame size; post-call work defeats tail calls. Over-cap
 * callers die identified (test 11, child process). */
static Bender c_grow_rec(int level, volatile char *touchp) {
    volatile char *pad = (volatile char *) alloca(448);
    pad[0] = (char) level;
    {
        int i;
        for (i = 1; i < 448; i++) pad[i] = (char) (pad[i - 1] + i);
    }
    *touchp += pad[0] + pad[447];
    Bender r;
    if (level == 0) {
        r = bender_wrap_integer(*touchp);
    } else {
        r = c_grow_rec(level - 1, touchp);
    }
    *touchp += pad[0];
    return r;
}

static Bender c_grow(int32_t argc, Bender *argv) {
    bender_arity(argc, 1, 1);
    int depth = bender_unwrap_integer(argv[0]);
    volatile char touch = 0;
    return c_grow_rec(depth, &touch);
}

/* Committed window below the mapping top, or 0 where untracked (flat). */
static size_t fiber_committed_window(BenderFiber *f) {
    if (NULL == f->native_stack_base || 0 == f->native_stack_size ||
            NULL == f->native_stack_committed)
        return 0;
    return (size_t) ((char *) f->native_stack_base + f->native_stack_size -
                     (char *) f->native_stack_committed);
}

/* Phase D3: self-calibrating red-zone touch. Recurse (alloca frames)
 * until the committed window reaches the usable start — i.e. a red
 * fault was handled and the overflow flag is armed — then unwind
 * without descending further. Returns [levels entered?]. Bounded by
 * maxlevel (never reached when growth works; keeps a broken handler
 * from hanging the suite). */
static Bender c_touch_red_rec(int level, int maxlevel, volatile char *touchp, int *entered) {
    volatile char *pad = (volatile char *) alloca(448);
    pad[0] = (char) level;
    {
        int i;
        for (i = 1; i < 448; i++) pad[i] = (char) (pad[i - 1] + i);
    }
    *touchp += pad[0] + pad[447];
    {
        BenderFiber *f = bender_vm.fiber;
        if (NULL != f && NULL != f->native_stack &&
                NULL != f->native_stack_committed &&
                f->native_stack_committed < f->native_stack) {
            *entered = 1;
            return bender_wrap_integer(level);
        }
    }
    Bender r;
    if (level >= maxlevel) {
        r = bender_wrap_integer(-1);
    } else {
        r = c_touch_red_rec(level + 1, maxlevel, touchp, entered);
    }
    *touchp += pad[0];
    return r;
}

static Bender c_touch_red(int32_t argc, Bender *argv) {
    (void) argc;
    (void) argv;
    volatile char touch = 0;
    int entered = 0;
    Bender r = c_touch_red_rec(0, 100000, &touch, &entered);
    Bender tup[2];
    tup[0] = bender_wrap_integer(entered);
    tup[1] = r;
    return bender_wrap_tuple(bender_tuple_n(tup, 2));
}

static Bender c_noop(int32_t argc, Bender *argv) {
    (void) argc;
    (void) argv;
    return bender_wrap_nil();
}

int main(int argc, char **argv) {
    (void) argc;
    (void) argv;
    bender_init();
    BenderTable *env = bender_core_env(NULL);
    bender_def(env, "c-yield1", bender_wrap_cfunction(c_yield1), "(test helper)");
    bender_def(env, "c-outer", bender_wrap_cfunction(c_outer), "(test helper)");
    bender_def(env, "c-rooted", bender_wrap_cfunction(c_rooted), "(test helper)");
    bender_def(env, "c-deep", bender_wrap_cfunction(c_deep), "(test helper)");
    bender_def(env, "c-grow", bender_wrap_cfunction(c_grow), "(test helper)");
    bender_def(env, "c-touch-red", bender_wrap_cfunction(c_touch_red), "(test helper)");
    bender_def(env, "c-noop", bender_wrap_cfunction(c_noop), "(test helper)");

    Bender out = bender_wrap_nil();
    if (bender_dostring(env,
            "(defn w1 [] (c-yield1 :ignored))"
            "(defn w2 [] (c-outer))"
            "(defn w3 [] (c-rooted))"
            "(defn w4 [d] (c-deep d))"
            "(defn w5 [d] (c-grow d))"
            "(defn w6 [] (c-touch-red) (c-noop))",
            "fiber-c-test", &out)) {
        printf("FAIL: setup dostring failed\n");
        return 1;
    }

    /* 1. Direct C yield, two suspensions, C locals intact. */
    {
        BenderFiber *f = run_fn(env, "w1");
        Bender r = bender_wrap_nil();
        BenderSignal s = bender_continue(f, bender_wrap_nil(), &r);
        CHECK(s == BENDER_SIGNAL_YIELD && streq_cstring(r, "first"), "c yield 1 suspends");
        bender_collect(); /* collection while C-frozen must be safe */
        s = bender_continue(f, bender_cstringv("R1"), &r);
        CHECK(s == BENDER_SIGNAL_YIELD && streq_cstring(r, "R1"), "resume value echoes through C");
        s = bender_continue(f, bender_cstringv("R2"), &r);
        CHECK(s == BENDER_SIGNAL_OK, "c function completes");
        CHECK(bender_checktype(r, BENDER_TUPLE) &&
              streq_cstring(bender_unwrap_tuple(r)[0], "tag-ok") &&
              streq_cstring(bender_unwrap_tuple(r)[1], "R2") &&
              bender_unwrap_integer(bender_unwrap_tuple(r)[2]) == 2,
              "C locals intact across two swaps");
        CHECK(bender_fiber_status(f) == BENDER_STATUS_DEAD, "fiber dead");
        bender_gcunroot(bender_wrap_fiber(f));
    }

    /* 2. Three nested C frames frozen across one suspension. */
    {
        BenderFiber *f = run_fn(env, "w2");
        Bender r = bender_wrap_nil();
        BenderSignal s = bender_continue(f, bender_wrap_nil(), &r);
        CHECK(s == BENDER_SIGNAL_YIELD && streq_cstring(r, "deep"), "nested C chain suspends");
        s = bender_continue(f, bender_cstringv("RES"), &r);
        CHECK(s == BENDER_SIGNAL_OK, "nested C chain completes");
        Bender outer = bender_unwrap_tuple(r)[0];
        CHECK(streq_cstring(bender_unwrap_tuple(outer)[0], "RES") &&
              streq_cstring(bender_unwrap_tuple(outer)[1], "mid") &&
              streq_cstring(bender_unwrap_tuple(r)[1], "outer"),
              "resume value flows up all C frames, canaries held");
        bender_gcunroot(bender_wrap_fiber(f));
    }

    /* 3. Rooted value in a C local survives a collection mid-suspension. */
    {
        BenderFiber *f = run_fn(env, "w3");
        Bender r = bender_wrap_nil();
        BenderSignal s = bender_continue(f, bender_wrap_nil(), &r);
        CHECK(s == BENDER_SIGNAL_YIELD && bender_checktype(r, BENDER_TABLE), "rooted table yielded");
        bender_collect();
        bender_collect();
        s = bender_continue(f, bender_cstringv("back"), &r);
        CHECK(s == BENDER_SIGNAL_OK &&
              streq_cstring(bender_unwrap_tuple(r)[0], "v-rooted") &&
              streq_cstring(bender_unwrap_tuple(r)[1], "back"),
              "rooted C-local table intact after collects");
        bender_gcunroot(bender_wrap_fiber(f));
    }

    /* 4. Deep C chain on the shadow stack, no manual gcroot anywhere. */
    {
        BenderFiber *f = run_fn1(env, "w4", bender_wrap_integer(50));
        Bender r = bender_wrap_nil();
        BenderSignal s = bender_continue(f, bender_wrap_nil(), &r);
        CHECK(s == BENDER_SIGNAL_YIELD && streq_cstring(r, "bottom"), "deep chain suspends");
        bender_collect();
        bender_collect();
        bender_collect();
        s = bender_continue(f, bender_cstringv("UP"), &r);
        CHECK(s == BENDER_SIGNAL_OK, "deep chain completes");
        CHECK(bender_checktype(r, BENDER_TUPLE) &&
              streq_cstring(bender_unwrap_tuple(r)[0], "UP") &&
              bender_unwrap_integer(bender_unwrap_tuple(r)[1]) == 1275,
              "all 51 shadow-pinned tables intact (0+..+50=1275)");
        CHECK(bender_gcshadow_mark() == 0, "shadow stack balanced at host");
        bender_gcunroot(bender_wrap_fiber(f));
    }

    /* 5. Cancel unwinds through frozen C frames with the payload. */
    {
        BenderFiber *f = run_fn(env, "w1");
        Bender r = bender_wrap_nil();
        BenderSignal s = bender_continue(f, bender_wrap_nil(), &r);
        CHECK(s == BENDER_SIGNAL_YIELD, "suspended for cancel");
        s = bender_continue_signal(f, bender_cstringv("stop"), &r, BENDER_SIGNAL_ERROR);
        CHECK(s == BENDER_SIGNAL_ERROR && streq_cstring(r, "stop"), "cancel payload through C");
        CHECK(bender_fiber_status(f) == BENDER_STATUS_ERROR, "fiber error after cancel");
        bender_gcunroot(bender_wrap_fiber(f));
    }

    /* 6. Marshal of a C-frozen fiber is a clean error, not a crash. */
    {
        BenderFiber *f = run_fn(env, "w1");
        Bender r = bender_wrap_nil();
        BenderSignal s = bender_continue(f, bender_wrap_nil(), &r);
        CHECK(s == BENDER_SIGNAL_YIELD, "suspended for marshal");
        BenderTryState tstate;
        BenderSignal tsig = bender_try(&tstate);
        if (!tsig) {
            BenderBuffer *b = bender_buffer(10);
            bender_marshal(b, bender_wrap_fiber(f), NULL, 0);
            bender_restore(&tstate);
            CHECK(0, "marshal of C-frozen fiber must refuse");
        } else {
            const char *msg = (const char *) bender_unwrap_string(tstate.payload);
            bender_restore(&tstate);
            CHECK(strstr(msg, "c stackframe") != NULL, "marshal refuses C-frozen fiber cleanly");
        }
        /* Fiber still suspended and resumable afterwards. */
        s = bender_continue(f, bender_cstringv("R1"), &r);
        CHECK(s == BENDER_SIGNAL_YIELD && streq_cstring(r, "R1"), "fiber usable after refused marshal");
        bender_gcunroot(bender_wrap_fiber(f));
    }

    /* 8-11. Phase D grow-on-fault (POSIX grown stacks; flat builds
     * only check completion, never the window). */
    {
        BenderFiber *f = run_fn1(env, "w5", bender_wrap_integer(0));
        Bender r = bender_wrap_nil();
        BenderSignal s = bender_continue(f, bender_wrap_nil(), &r);
        CHECK(s == BENDER_SIGNAL_OK && bender_checktype(r, BENDER_NUMBER),
              "shallow C recursion completes");
#ifndef BENDER_WINDOWS
        CHECK(fiber_committed_window(f) == 64 * 1024,
              "idle fiber commits exactly the initial window");
#else
        CHECK(fiber_committed_window(f) == 0, "flat build tracks no window");
#endif
        bender_gcunroot(bender_wrap_fiber(f));
    }
    {
        BenderFiber *f = run_fn1(env, "w5", bender_wrap_integer(1500));
        Bender r = bender_wrap_nil();
        BenderSignal s = bender_continue(f, bender_wrap_nil(), &r);
        CHECK(s == BENDER_SIGNAL_OK && bender_checktype(r, BENDER_NUMBER),
              "750KB C recursion completes across growths");
#ifndef BENDER_WINDOWS
        {
            size_t w = fiber_committed_window(f);
            CHECK(w >= 512 * 1024 && w <= 2 * 1024 * 1024,
                  "committed window grew with use (and never past cap)");
        }
#endif
        CHECK(bender_fiber_status(f) == BENDER_STATUS_DEAD, "grown fiber dead");
        bender_gcunroot(bender_wrap_fiber(f));
    }
    {
        /* D5 accounting: 1000 idle fibers commit ~64MB total, not ~2GB.
         * Portable (no OS counters): the tracked window is the proof. */
        size_t total = 0;
        int i;
        for (i = 0; i < 1000; i++) {
            BenderFiber *f = run_fn1(env, "w5", bender_wrap_integer(0));
            total += fiber_committed_window(f);
            bender_gcunroot(bender_wrap_fiber(f));
        }
        bender_collect();
#ifndef BENDER_WINDOWS
        CHECK(total <= 1000 * 128 * 1024, "1k idle fibers commit small");
        printf("info: 1000 idle fibers commit %lu bytes total\n",
               (unsigned long) total);
#else
        (void) total;
        printf("info: flat build skips commit accounting\n");
#endif
    }
#ifndef BENDER_WINDOWS
    /* 11. Over-cap C recursion dies identified (child: fatal by design).
     * D3 catchable errors are the follow-up; slice 1 must at least name
     * the killer instead of corrupting into a mystery fault. */
    {
        int pipefd[2];
        CHECK(pipe(pipefd) == 0, "overflow pipe ready");
        pid_t pid = fork();
        if (pid == 0) {
            dup2(pipefd[1], STDERR_FILENO);
            close(pipefd[0]);
            close(pipefd[1]);
            {
                BenderFiber *f = run_fn1(env, "w5", bender_wrap_integer(20000));
                Bender r = bender_wrap_nil();
                (void) bender_continue(f, bender_wrap_nil(), &r);
            }
            _exit(0);
        }
        close(pipefd[1]);
        {
            char buf[256];
            size_t off = 0;
            ssize_t n;
            while ((n = read(pipefd[0], buf + off, sizeof(buf) - off - 1)) > 0) {
                off += (size_t) n;
                if (off >= sizeof(buf) - 1) break;
            }
            buf[off] = '\0';
            close(pipefd[0]);
            int status = 0;
            waitpid(pid, &status, 0);
            CHECK(WIFSIGNALED(status), "over-cap recursion dies by signal");
            CHECK(strstr(buf, "fiber native stack overflow") != NULL,
                  "over-cap death names the stack");
        }
    }
#endif

    /* 12-14. Phase D3 catchable overflow: a red-zone touch arms the
     * flag; the next call surfaces it. Panics inside run_vm are caught
     * by the trampoline and delivered as ERROR returns (not host
     * longjmps), so assert on the return, not on bender_try. */
    {
        BenderFiber *f = run_fn(env, "w6");
        Bender r = bender_wrap_nil();
        BenderSignal s = bender_continue(f, bender_wrap_nil(), &r);
        CHECK(s == BENDER_SIGNAL_ERROR, "red touch raises on the next call");
        CHECK(bender_checktype(r, BENDER_STRING) &&
              strstr((const char *) bender_unwrap_string(r), "fiber native stack overflow") != NULL,
              "overflow raises catchably with its message");
        CHECK(bender_fiber_status(f) == BENDER_STATUS_ERROR, "overflowed fiber errors");
        bender_gcunroot(bender_wrap_fiber(f));
    }
    {
        /* Repeatability: fresh fibers catch again (red space remains). */
        int i;
        for (i = 0; i < 2; i++) {
            BenderFiber *f = run_fn(env, "w6");
            Bender r = bender_wrap_nil();
            BenderSignal s = bender_continue(f, bender_wrap_nil(), &r);
            CHECK(s == BENDER_SIGNAL_ERROR, "repeat overflow must raise");
            CHECK(bender_checktype(r, BENDER_STRING) &&
                  strstr((const char *) bender_unwrap_string(r), "fiber native stack overflow") != NULL,
                  "repeat overflow catchable");
            bender_gcunroot(bender_wrap_fiber(f));
        }
    }
    {
        /* Reset reclaim: an errored fiber reset for reuse leaves the
         * red zone (committed back out) with the flag clear. */
        Bender v = bender_wrap_nil();
        BenderFiber *f = NULL;
        if (bender_resolve(env, bender_csymbol("w6"), &v) == BENDER_BINDING_NONE) {
            CHECK(0, "cannot resolve w6 for reset test");
        } else {
            f = run_fn(env, "w6");
            Bender r = bender_wrap_nil();
            (void) bender_continue(f, bender_wrap_nil(), &r);
        }
        if (NULL != f) {
            CHECK(bender_fiber_status(f) == BENDER_STATUS_ERROR, "reset subject errored");
            f = bender_fiber_reset(f, bender_unwrap_function(v), 0, NULL);
            CHECK(!f->stack_overflowed, "reset clears the flag");
            CHECK(f->native_stack_committed == NULL ||
                    f->native_stack_committed >= f->native_stack,
                  "reset reclaims the red zone");
            bender_gcunroot(bender_wrap_fiber(f));
        }
    }

    /* 7. Yield with no current fiber panics instead of crashing. */
    {
        BenderTryState tstate;
        BenderSignal tsig = bender_try(&tstate);
        if (!tsig) {
            bender_fiber_yield(bender_wrap_nil());
            bender_restore(&tstate);
            CHECK(0, "yield with no fiber must panic");
        } else {
            const char *msg = (const char *) bender_unwrap_string(tstate.payload);
            bender_restore(&tstate);
            CHECK(msg != NULL && strstr(msg, "no current fiber") != NULL,
                  "yield with no fiber panics cleanly");
        }
    }

    bender_deinit();
    if (failures) {
        printf("fiber-c-test: %d FAILURES\n", failures);
        return 1;
    }
    printf("fiber-c-test: all passed\n");
    return 0;
}
