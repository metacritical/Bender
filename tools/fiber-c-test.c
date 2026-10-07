/* tools/fiber-c-test.c — Phase 1.1b proof (see docs/internals/native-fibers.md).
 *
 * Proves that real C call chains on a fiber's native stack survive
 * yield/resume via janet_fiber_yield: C locals intact across swaps,
 * resume values plumbed, rooted values surviving collections that run
 * while suspended, cancel unwinding through C frames, and clean marshal
 * refusal for C-frozen fibers.
 *
 * Build:  cc -O2 -Ibuild -Isrc/include tools/fiber-c-test.c \
 *             build/libjanet.a -lm -lpthread -o /tmp/fiber-c-test
 * (or: make fibertest). Exit 0 on success, 1 on any failure. */

#include <janet.h>

#include <assert.h>
#include <stdio.h>
#include <string.h>

static int failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("FAIL: %s\n", msg); failures++; } \
    else { printf("ok: %s\n", msg); } \
} while (0)

static int streq_cstring(Janet x, const char *s) {
    if (!janet_checktype(x, JANET_STRING)) return 0;
    JanetString jstr = janet_unwrap_string(x);
    int32_t len = janet_string_length(jstr);
    return len == (int32_t) strlen(s) && !memcmp(jstr, s, (size_t) len);
}

/* Direct yield from C with C locals live across two suspensions. */
static Janet c_yield1(int32_t argc, Janet *argv) {
    janet_arity(argc, 1, 1);
    Janet tag = argv[0];
    int canary = 0x5A5A5A5A;
    int counter = 0;
    char tagbuf[32];
    snprintf(tagbuf, sizeof(tagbuf), "tag-ok");
    (void) tag;
    Janet r1 = janet_fiber_yield(janet_cstringv("first"));
    counter++;
    if (canary != 0x5A5A5A5A) janet_panic("canary clobbered after yield 1");
    Janet r2 = janet_fiber_yield(r1);
    counter++;
    if (canary != 0x5A5A5A5A) janet_panic("canary clobbered after yield 2");
    if (counter != 2) janet_panic("counter wrong");
    Janet tup[3];
    tup[0] = janet_cstringv(tagbuf);
    tup[1] = r2;
    tup[2] = janet_wrap_integer(counter);
    return janet_wrap_tuple(janet_tuple_n(tup, 3));
}

/* Three nested plain-C frames, innermost yields. */
static Janet c_inner(void) {
    int canary = 0xA5A5;
    const char *name = "inner";
    Janet r = janet_fiber_yield(janet_cstringv("deep"));
    if (canary != 0xA5A5) janet_panic("inner canary clobbered");
    if (strcmp(name, "inner") != 0) janet_panic("inner string local clobbered");
    return r;
}

static Janet c_mid(Janet x) {
    int canary = 0x1234;
    (void) x;
    Janet r = c_inner();
    if (canary != 0x1234) janet_panic("mid canary clobbered");
    Janet tup[2];
    tup[0] = r;
    tup[1] = janet_cstringv("mid");
    return janet_wrap_tuple(janet_tuple_n(tup, 2));
}

static Janet c_outer(int32_t argc, Janet *argv) {
    janet_arity(argc, 0, 0);
    (void) argv;
    int canary = 0x7777;
    Janet r = c_mid(janet_wrap_nil());
    if (canary != 0x7777) janet_panic("outer canary clobbered");
    Janet tup[2];
    tup[0] = r;
    tup[1] = janet_cstringv("outer");
    return janet_wrap_tuple(janet_tuple_n(tup, 2));
}

/* A table held via the shadow root stack (Phase 2b-i) across a
 * suspension + collection. No manual gcroot: the push/pop_to scope keeps
 * it alive, mirroring what generated code will emit around calls. */
static Janet c_rooted(int32_t argc, Janet *argv) {
    janet_arity(argc, 0, 0);
    (void) argv;
    size_t mark = janet_gcshadow_mark();
    JanetTable *t = janet_table(0);
    janet_table_put(t, janet_ckeywordv("k"), janet_cstringv("v-rooted"));
    janet_gcshadow_push(janet_wrap_table(t));
    Janet r = janet_fiber_yield(janet_wrap_table(t));
    /* A collection ran while suspended (see driver). The shadow-pinned
     * table and the C local pointing at it must still be valid. */
    Janet check = janet_table_get(t, janet_ckeywordv("k"));
    int ok = streq_cstring(check, "v-rooted");
    janet_gcshadow_pop_to(mark);
    if (!ok) janet_panic("rooted table did not survive suspension + collect");
    Janet tup[2];
    tup[0] = check;
    tup[1] = r;
    return janet_wrap_tuple(janet_tuple_n(tup, 2));
}

/* Deep C call chain holding one value per level on the shadow stack.
 * Each level pushes its table, recurses, verifies on the way out, and
 * pops exactly its own push -- the scoped discipline generated code
 * will follow. No manual gcroot anywhere on this path. */
static Janet c_deep_rec(int level, int *sump) {
    size_t mark = janet_gcshadow_mark();
    JanetTable *t = janet_table(0);
    janet_table_put(t, janet_ckeywordv("level"), janet_wrap_integer(level));
    janet_gcshadow_push(janet_wrap_table(t));
    Janet r;
    if (level == 0) {
        r = janet_fiber_yield(janet_cstringv("bottom"));
    } else {
        r = c_deep_rec(level - 1, sump);
    }
    Janet lv = janet_table_get(t, janet_ckeywordv("level"));
    if (!janet_checktype(lv, JANET_NUMBER) || janet_unwrap_integer(lv) != level) {
        janet_panic("deep shadow table corrupted");
    }
    *sump += level;
    janet_gcshadow_pop_to(mark);
    return r;
}

static Janet c_deep(int32_t argc, Janet *argv) {
    janet_arity(argc, 1, 1);
    int depth = janet_unwrap_integer(argv[0]);
    int sum = 0;
    size_t entry = janet_gcshadow_mark();
    Janet r = c_deep_rec(depth, &sum);
    if (janet_gcshadow_mark() != entry) {
        janet_panic("shadow stack unbalanced across deep chain");
    }
    Janet tup[2];
    tup[0] = r;
    tup[1] = janet_wrap_integer(sum);
    return janet_wrap_tuple(janet_tuple_n(tup, 2));
}

static JanetFiber *run_fn(JanetTable *env, const char *name) {
    Janet v = janet_wrap_nil();
    if (janet_resolve(env, janet_csymbol(name), &v) == JANET_BINDING_NONE) {
        printf("FAIL: cannot resolve %s\n", name);
        failures++;
        return NULL;
    }
    JanetFunction *fn = janet_unwrap_function(v);
    JanetFiber *f = janet_fiber(fn, 64, 0, NULL);
    f->env = env;
    janet_gcroot(janet_wrap_fiber(f));
    return f;
}

static JanetFiber *run_fn1(JanetTable *env, const char *name, Janet arg) {
    Janet v = janet_wrap_nil();
    if (janet_resolve(env, janet_csymbol(name), &v) == JANET_BINDING_NONE) {
        printf("FAIL: cannot resolve %s\n", name);
        failures++;
        return NULL;
    }
    JanetFunction *fn = janet_unwrap_function(v);
    JanetFiber *f = janet_fiber(fn, 64, 1, &arg);
    f->env = env;
    janet_gcroot(janet_wrap_fiber(f));
    return f;
}

int main(int argc, char **argv) {
    (void) argc;
    (void) argv;
    janet_init();
    JanetTable *env = janet_core_env(NULL);
    janet_def(env, "c-yield1", janet_wrap_cfunction(c_yield1), "(test helper)");
    janet_def(env, "c-outer", janet_wrap_cfunction(c_outer), "(test helper)");
    janet_def(env, "c-rooted", janet_wrap_cfunction(c_rooted), "(test helper)");
    janet_def(env, "c-deep", janet_wrap_cfunction(c_deep), "(test helper)");

    Janet out = janet_wrap_nil();
    if (janet_dostring(env,
            "(defn w1 [] (c-yield1 :ignored))"
            "(defn w2 [] (c-outer))"
            "(defn w3 [] (c-rooted))"
            "(defn w4 [d] (c-deep d))",
            "fiber-c-test", &out)) {
        printf("FAIL: setup dostring failed\n");
        return 1;
    }

    /* 1. Direct C yield, two suspensions, C locals intact. */
    {
        JanetFiber *f = run_fn(env, "w1");
        Janet r = janet_wrap_nil();
        JanetSignal s = janet_continue(f, janet_wrap_nil(), &r);
        CHECK(s == JANET_SIGNAL_YIELD && streq_cstring(r, "first"), "c yield 1 suspends");
        janet_collect(); /* collection while C-frozen must be safe */
        s = janet_continue(f, janet_cstringv("R1"), &r);
        CHECK(s == JANET_SIGNAL_YIELD && streq_cstring(r, "R1"), "resume value echoes through C");
        s = janet_continue(f, janet_cstringv("R2"), &r);
        CHECK(s == JANET_SIGNAL_OK, "c function completes");
        CHECK(janet_checktype(r, JANET_TUPLE) &&
              streq_cstring(janet_unwrap_tuple(r)[0], "tag-ok") &&
              streq_cstring(janet_unwrap_tuple(r)[1], "R2") &&
              janet_unwrap_integer(janet_unwrap_tuple(r)[2]) == 2,
              "C locals intact across two swaps");
        CHECK(janet_fiber_status(f) == JANET_STATUS_DEAD, "fiber dead");
        janet_gcunroot(janet_wrap_fiber(f));
    }

    /* 2. Three nested C frames frozen across one suspension. */
    {
        JanetFiber *f = run_fn(env, "w2");
        Janet r = janet_wrap_nil();
        JanetSignal s = janet_continue(f, janet_wrap_nil(), &r);
        CHECK(s == JANET_SIGNAL_YIELD && streq_cstring(r, "deep"), "nested C chain suspends");
        s = janet_continue(f, janet_cstringv("RES"), &r);
        CHECK(s == JANET_SIGNAL_OK, "nested C chain completes");
        Janet outer = janet_unwrap_tuple(r)[0];
        CHECK(streq_cstring(janet_unwrap_tuple(outer)[0], "RES") &&
              streq_cstring(janet_unwrap_tuple(outer)[1], "mid") &&
              streq_cstring(janet_unwrap_tuple(r)[1], "outer"),
              "resume value flows up all C frames, canaries held");
        janet_gcunroot(janet_wrap_fiber(f));
    }

    /* 3. Rooted value in a C local survives a collection mid-suspension. */
    {
        JanetFiber *f = run_fn(env, "w3");
        Janet r = janet_wrap_nil();
        JanetSignal s = janet_continue(f, janet_wrap_nil(), &r);
        CHECK(s == JANET_SIGNAL_YIELD && janet_checktype(r, JANET_TABLE), "rooted table yielded");
        janet_collect();
        janet_collect();
        s = janet_continue(f, janet_cstringv("back"), &r);
        CHECK(s == JANET_SIGNAL_OK &&
              streq_cstring(janet_unwrap_tuple(r)[0], "v-rooted") &&
              streq_cstring(janet_unwrap_tuple(r)[1], "back"),
              "rooted C-local table intact after collects");
        janet_gcunroot(janet_wrap_fiber(f));
    }

    /* 4. Deep C chain on the shadow stack, no manual gcroot anywhere. */
    {
        JanetFiber *f = run_fn1(env, "w4", janet_wrap_integer(50));
        Janet r = janet_wrap_nil();
        JanetSignal s = janet_continue(f, janet_wrap_nil(), &r);
        CHECK(s == JANET_SIGNAL_YIELD && streq_cstring(r, "bottom"), "deep chain suspends");
        janet_collect();
        janet_collect();
        janet_collect();
        s = janet_continue(f, janet_cstringv("UP"), &r);
        CHECK(s == JANET_SIGNAL_OK, "deep chain completes");
        CHECK(janet_checktype(r, JANET_TUPLE) &&
              streq_cstring(janet_unwrap_tuple(r)[0], "UP") &&
              janet_unwrap_integer(janet_unwrap_tuple(r)[1]) == 1275,
              "all 51 shadow-pinned tables intact (0+..+50=1275)");
        CHECK(janet_gcshadow_mark() == 0, "shadow stack balanced at host");
        janet_gcunroot(janet_wrap_fiber(f));
    }

    /* 5. Cancel unwinds through frozen C frames with the payload. */
    {
        JanetFiber *f = run_fn(env, "w1");
        Janet r = janet_wrap_nil();
        JanetSignal s = janet_continue(f, janet_wrap_nil(), &r);
        CHECK(s == JANET_SIGNAL_YIELD, "suspended for cancel");
        s = janet_continue_signal(f, janet_cstringv("stop"), &r, JANET_SIGNAL_ERROR);
        CHECK(s == JANET_SIGNAL_ERROR && streq_cstring(r, "stop"), "cancel payload through C");
        CHECK(janet_fiber_status(f) == JANET_STATUS_ERROR, "fiber error after cancel");
        janet_gcunroot(janet_wrap_fiber(f));
    }

    /* 6. Marshal of a C-frozen fiber is a clean error, not a crash. */
    {
        JanetFiber *f = run_fn(env, "w1");
        Janet r = janet_wrap_nil();
        JanetSignal s = janet_continue(f, janet_wrap_nil(), &r);
        CHECK(s == JANET_SIGNAL_YIELD, "suspended for marshal");
        JanetTryState tstate;
        JanetSignal tsig = janet_try(&tstate);
        if (!tsig) {
            JanetBuffer *b = janet_buffer(10);
            janet_marshal(b, janet_wrap_fiber(f), NULL, 0);
            janet_restore(&tstate);
            CHECK(0, "marshal of C-frozen fiber must refuse");
        } else {
            const char *msg = (const char *) janet_unwrap_string(tstate.payload);
            janet_restore(&tstate);
            CHECK(strstr(msg, "c stackframe") != NULL, "marshal refuses C-frozen fiber cleanly");
        }
        /* Fiber still suspended and resumable afterwards. */
        s = janet_continue(f, janet_cstringv("R1"), &r);
        CHECK(s == JANET_SIGNAL_YIELD && streq_cstring(r, "R1"), "fiber usable after refused marshal");
        janet_gcunroot(janet_wrap_fiber(f));
    }

    /* 7. Yield with no current fiber panics instead of crashing. */
    {
        JanetTryState tstate;
        JanetSignal tsig = janet_try(&tstate);
        if (!tsig) {
            janet_fiber_yield(janet_wrap_nil());
            janet_restore(&tstate);
            CHECK(0, "yield with no fiber must panic");
        } else {
            const char *msg = (const char *) janet_unwrap_string(tstate.payload);
            janet_restore(&tstate);
            CHECK(msg != NULL && strstr(msg, "no current fiber") != NULL,
                  "yield with no fiber panics cleanly");
        }
    }

    janet_deinit();
    if (failures) {
        printf("fiber-c-test: %d FAILURES\n", failures);
        return 1;
    }
    printf("fiber-c-test: all passed\n");
    return 0;
}
