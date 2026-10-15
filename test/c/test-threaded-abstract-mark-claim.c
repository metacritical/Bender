/*
 * A raw heap edge can reach a threaded abstract without going through
 * unmarshal's registry transfer. Compile after `make`:
 *   cc -Isrc/include -Isrc/conf -Isrc/core \
 *      test/c/test-threaded-abstract-mark-claim.c build/libjanet.a \
 *      -lm -lpthread -ldl -o build/test-threaded-abstract-mark-claim
 */
#include "janet.h"
#include "state.h"
#include <stdio.h>

static const JanetAbstractType raw_type = {.name = "test/raw-threaded-abstract"};

static int check(int condition, const char *message) {
    if (condition) return 0;
    fprintf(stderr, "FAIL: %s\n", message);
    return 1;
}

int main(void) {
    if (janet_init()) return check(0, "janet_init");

    void *raw = janet_abstract_threaded(&raw_type, 0);
    Janet key = janet_wrap_abstract(raw);
    JanetAbstractHead *head = janet_abstract_head(raw);
    JanetArray *holder = janet_array(1);
    janet_array_push(holder, key);
    janet_gcroot(janet_wrap_array(holder));
    int fail = 0;

    /* Simulate a foreign raw reference: this VM has a heap edge but no
     * registry entry. The existing ref is held by the abstract's home VM. */
    janet_table_remove(&janet_vm.threaded_abstracts, key);
    fail |= check(janet_checktype(janet_table_get(&janet_vm.threaded_abstracts, key), JANET_NIL),
                  "foreign registry starts absent");
    fail |= check(head->gc.data.refcount == 1, "home owns initial claim");
    janet_collect();
    fail |= check(janet_checktype(janet_table_get(&janet_vm.threaded_abstracts, key), JANET_BOOLEAN),
                  "raw heap edge registers abstract on first mark");
    fail |= check(head->gc.data.refcount == 2,
                  "first registration acquires foreign claim");

    /* A completed sweep turns TRUE into FALSE. Visiting that existing
     * slot again must not double-acquire a registry claim. */
    janet_collect();
    fail |= check(head->gc.data.refcount == 2,
                  "mark of existing false slot does not double incref");
    fail |= check(janet_checktype(janet_table_get(&janet_vm.threaded_abstracts, key), JANET_BOOLEAN),
                  "live registry entry remains present");

    /* Drop the raw edge. A subsequent sweep must release its registry
     * claim and tombstone the slot, leaving only the simulated home claim. */
    holder->count = 0;
    janet_collect();
    fail |= check(janet_checktype(janet_table_get(&janet_vm.threaded_abstracts, key), JANET_NIL),
                  "unvisited registry slot is removed");
    fail |= check(head->gc.data.refcount == 1, "unvisited foreign claim released");
    janet_gcunroot(janet_wrap_array(holder));
    /* This synthetic setup removed the home registry entry without
     * releasing its original claim. janet_deinit cannot discover that
     * orphaned claim, so the block is intentionally retained until this
     * short-lived test process exits; the assertion above tests only the
     * newly acquired foreign claim's balanced release. */
    janet_deinit();
    if (!fail) puts("PASS: raw mark owns exactly one claim and purges its slot");
    return fail != 0;
}
