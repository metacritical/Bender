/* Runnable contract for the opt-in fast-PUT commit hook. This is deliberately
 * not a whole native-PC census: alias callbacks and continuation GC exits
 * remain unresolved and cannot authorize a numeric-opportunity stop gate. */
#include <bender.h>
#include <assert.h>
#include <setjmp.h>

static jmp_buf after_commit;
static unsigned unwind_count;
static void commit_then_unwind(void *opaque) {
    ++*(unsigned *)opaque;
    ++unwind_count;
    longjmp(after_commit, 1);
}
#include <stdio.h>

#ifndef BENDER_NATIVE_COMMIT_PROBE
#error Build this fixture with BENDER_NATIVE_COMMIT_PROBE=1
#endif

static void committed(void *opaque) {
    unsigned *count = opaque;
    ++*count;
}

int main(void) {
    unsigned count = 0;
    Bender key = bender_wrap_integer(0);
    Bender value = bender_wrap_integer(17);
    bender_init();

    BenderArray *array = bender_array(1);
    bender_array_push(array, bender_wrap_nil());
    Bender a = bender_wrap_array(array);
    assert(!bender_native_array_put_fast_probe(a, bender_wrap_integer(1), value, committed, &count));
    assert(count == 0);
    assert(bender_native_array_put_fast_probe(a, key, value, committed, &count));
    assert(count == 1 && bender_equals(array->data[0], value));

    BenderBuffer *buffer = bender_buffer(1);
    bender_buffer_push_u8(buffer, 0);
    Bender b = bender_wrap_buffer(buffer);
    assert(!bender_native_buffer_put_fast_probe(b, key, bender_wrap_nil(), committed, &count));
    assert(count == 1);
    assert(bender_native_buffer_put_fast_probe(b, key, value, committed, &count));
    assert(count == 2 && buffer->data[0] == 17);

    BenderTable *table = bender_table(0);
    Bender t = bender_wrap_table(table);
    Bender tk = bender_wrap_integer(2);
    assert(!bender_native_table_put_fast_probe(t, tk, value, committed, &count));
    assert(count == 2);
    bender_table_put(table, tk, bender_wrap_integer(1));
    assert(bender_native_table_put_fast_probe(t, tk, value, committed, &count));
    assert(count == 3 && bender_equals(bender_table_get(table, tk), value));
    /* Successful no-op keys are still completed bytecode operations. */
    assert(bender_native_table_put_fast_probe(t, bender_wrap_nil(), value, committed, &count));
    assert(count == 4);
    /* Forced unwind from the commit hook demonstrates that the mutation
     * precedes a nonlocal exit (a barrier panic would run after the hook). */
    if (!setjmp(after_commit)) {
        bender_native_array_put_fast_probe(a, key, bender_wrap_integer(19),
                                    commit_then_unwind, &count);
        assert(0 && "commit hook did not unwind");
    }
    assert(unwind_count == 1 && count == 5);
    assert(bender_equals(array->data[0], bender_wrap_integer(19)));
    if (!setjmp(after_commit)) {
        bender_native_table_put_fast_probe(t, tk, bender_wrap_integer(23),
                                    commit_then_unwind, &count);
        assert(0 && "table commit hook did not unwind");
    }
    assert(unwind_count == 2 && count == 6);
    assert(bender_equals(bender_table_get(table, tk), bender_wrap_integer(23)));
    puts("native fast-PUT commit contract: passed");
    bender_deinit();
    return 0;
}
