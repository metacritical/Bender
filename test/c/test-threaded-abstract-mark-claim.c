/*
 * A raw heap edge in a foreign VM can reach a threaded abstract without
 * going through unmarshal's registry transfer. Compile against a current
 * build of this checkout (state.h's JanetVM layout must match the archive):
 *   cc -Isrc/include -Isrc/conf -Isrc/core \
 *      test/c/test-threaded-abstract-mark-claim.c build/libjanet.a \
 *      -lm -lpthread -ldl -o /tmp/janet-test-threaded-claim
 */
#include "janet.h"
#include "state.h"
#include <pthread.h>
#include <stdio.h>

static const JanetAbstractType raw_type = {.name = "test/raw-threaded-abstract"};

typedef struct {
    void *raw;
    int failed;
} ClaimTest;

static int check(int condition, const char *message) {
    if (condition) return 0;
    fprintf(stderr, "FAIL: %s\n", message);
    return 1;
}

static void *foreign_vm(void *arg) {
    ClaimTest *test = arg;
    void *raw = test->raw;
    Janet key = janet_wrap_abstract(raw);
    JanetAbstractHead *head = janet_abstract_head(raw);
    int fail = 0;

    if (janet_init()) {
        test->failed = check(0, "foreign janet_init");
        return NULL;
    }

    /* The worker has not unmarshaled or otherwise registered this object.
     * Its only edge is this rooted heap array, populated with the raw key. */
    JanetArray *holder = janet_array(1);
    janet_array_push(holder, key);
    janet_gcroot(janet_wrap_array(holder));
    fail |= check(janet_checktype(janet_table_get(&janet_vm.threaded_abstracts, key), JANET_NIL),
                  "foreign registry starts absent");
    fail |= check(head->gc.data.refcount == 1, "home owns initial claim");
    janet_collect();
    fail |= check(janet_checktype(janet_table_get(&janet_vm.threaded_abstracts, key), JANET_BOOLEAN),
                  "raw heap edge registers abstract on first mark");
    fail |= check(head->gc.data.refcount == 2,
                  "first registration acquires foreign claim");

    /* Sweep resets the live slot to false; marking it a second time must
     * visit the existing claim rather than acquire another one. */
    janet_collect();
    fail |= check(head->gc.data.refcount == 2,
                  "mark of existing false slot does not double incref");
    fail |= check(janet_checktype(janet_table_get(&janet_vm.threaded_abstracts, key), JANET_BOOLEAN),
                  "live foreign registry entry remains present");

    /* Removing the only worker heap edge lets its next sweep release the
     * foreign claim; the home VM remains alive and rooted throughout. */
    holder->count = 0;
    janet_collect();
    fail |= check(janet_checktype(janet_table_get(&janet_vm.threaded_abstracts, key), JANET_NIL),
                  "unvisited foreign registry slot is removed");
    fail |= check(head->gc.data.refcount == 1, "unvisited foreign claim released");
    janet_gcunroot(janet_wrap_array(holder));
    janet_deinit();
    test->failed = fail;
    return NULL;
}

int main(void) {
    pthread_t thread;
    int fail = 0;
    if (janet_init()) return check(0, "home janet_init");

    void *raw = janet_abstract_threaded(&raw_type, 0);
    Janet key = janet_wrap_abstract(raw);
    ClaimTest test = {raw, 0};
    /* This is the real home registry claim, kept live until worker exit. */
    janet_gcroot(key);
    if (pthread_create(&thread, NULL, foreign_vm, &test)) {
        fail = check(0, "pthread_create");
    } else {
        if (pthread_join(thread, NULL)) fail = check(0, "pthread_join");
        fail |= test.failed;
    }
    janet_gcunroot(key);
    janet_deinit();
    if (!fail) puts("PASS: foreign raw mark owns exactly one claim and purges its slot");
    return fail != 0;
}
