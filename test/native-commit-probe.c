/* Runnable contract for the opt-in fast-PUT commit hook. This is deliberately
 * not a whole native-PC census: alias callbacks and continuation GC exits
 * remain unresolved and cannot authorize a numeric-opportunity stop gate. */
#include <janet.h>
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

#ifndef JANET_NATIVE_COMMIT_PROBE
#error Build this fixture with JANET_NATIVE_COMMIT_PROBE=1
#endif

static void committed(void *opaque) {
    unsigned *count = opaque;
    ++*count;
}

int main(void) {
    unsigned count = 0;
    Janet key = janet_wrap_integer(0);
    Janet value = janet_wrap_integer(17);
    janet_init();

    JanetArray *array = janet_array(1);
    janet_array_push(array, janet_wrap_nil());
    Janet a = janet_wrap_array(array);
    assert(!janet_native_array_put_fast(a, janet_wrap_integer(1), value, committed, &count));
    assert(count == 0);
    assert(janet_native_array_put_fast(a, key, value, committed, &count));
    assert(count == 1 && janet_equals(array->data[0], value));

    JanetBuffer *buffer = janet_buffer(1);
    janet_buffer_push_u8(buffer, 0);
    Janet b = janet_wrap_buffer(buffer);
    assert(!janet_native_buffer_put_fast(b, key, janet_wrap_nil(), committed, &count));
    assert(count == 1);
    assert(janet_native_buffer_put_fast(b, key, value, committed, &count));
    assert(count == 2 && buffer->data[0] == 17);

    JanetTable *table = janet_table(0);
    Janet t = janet_wrap_table(table);
    Janet tk = janet_wrap_integer(2);
    assert(!janet_native_table_put_fast(t, tk, value, committed, &count));
    assert(count == 2);
    janet_table_put(table, tk, janet_wrap_integer(1));
    assert(janet_native_table_put_fast(t, tk, value, committed, &count));
    assert(count == 3 && janet_equals(janet_table_get(table, tk), value));
    /* Successful no-op keys are still completed bytecode operations. */
    assert(janet_native_table_put_fast(t, janet_wrap_nil(), value, committed, &count));
    assert(count == 4);
    /* Forced unwind from the commit hook demonstrates that the mutation
     * precedes a nonlocal exit (a barrier panic would run after the hook). */
    if (!setjmp(after_commit)) {
        janet_native_array_put_fast(a, key, janet_wrap_integer(19),
                                    commit_then_unwind, &count);
        assert(0 && "commit hook did not unwind");
    }
    assert(unwind_count == 1 && count == 5);
    assert(janet_equals(array->data[0], janet_wrap_integer(19)));
    if (!setjmp(after_commit)) {
        janet_native_table_put_fast(t, tk, janet_wrap_integer(23),
                                    commit_then_unwind, &count);
        assert(0 && "table commit hook did not unwind");
    }
    assert(unwind_count == 2 && count == 6);
    assert(janet_equals(janet_table_get(table, tk), janet_wrap_integer(23)));
    puts("native fast-PUT commit contract: passed");
    janet_deinit();
    return 0;
}
