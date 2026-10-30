/*
* Copyright (c) 2026 Calvin Rose
*
* Permission is hereby granted, free of charge, to any person obtaining a copy
* of this software and associated documentation files (the "Software"), to
* deal in the Software without restriction, including without limitation the
* rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
* sell copies of the Software, and to permit persons to whom the Software is
* furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included in
* all copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
* AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
* FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
* IN THE SOFTWARE.
*/

#ifndef BENDER_AMALG
#include "features.h"
#include <bender.h>
#include "gc.h"
#include "util.h"
#include "state.h"
#endif

#include <string.h>

static void bender_array_impl(BenderArray *array, int32_t capacity) {
    Bender *data = NULL;
    if (capacity > 0) {
        bender_vm.next_collection += capacity * sizeof(Bender);
        data = (Bender *) array_allocate(sizeof(Bender), capacity);
        if (NULL == data) {
            BENDER_OUT_OF_MEMORY;
        }
    }
    array->count = 0;
    array->capacity = capacity;
    array->data = data;
}

/* Creates a new array */
BenderArray *bender_array(int32_t capacity) {
    BenderArray *array = bender_gcalloc(BENDER_MEMORY_ARRAY, sizeof(BenderArray));
    bender_array_impl(array, capacity);
    return array;
}

/* Creates a new array with weak references */
BenderArray *bender_array_weak(int32_t capacity) {
    BenderArray *array = bender_gcalloc(BENDER_MEMORY_ARRAY_WEAK, sizeof(BenderArray));
    bender_array_impl(array, capacity);
    return array;
}

/* Creates a new array from n elements. */
BenderArray *bender_array_n(const Bender *elements, int32_t n) {
    BenderArray *array = bender_gcalloc(BENDER_MEMORY_ARRAY, sizeof(BenderArray));
    array->capacity = n;
    array->count = n;
    array->data = array_allocate(sizeof(Bender), (size_t) n);
    if (!array->data) {
        BENDER_OUT_OF_MEMORY;
    }
    safe_memcpy(array->data, elements, sizeof(Bender) * n);
    return array;
}

/* Ensure the array has enough capacity for elements */
void bender_array_ensure(BenderArray *array, int32_t capacity, int32_t growth) {
    Bender *newData;
    Bender *old = array->data;
    if (capacity <= array->capacity) return;
    int64_t new_capacity = ((int64_t) capacity) * growth;
    if (new_capacity > INT32_MAX) new_capacity = INT32_MAX;
    capacity = (int32_t) new_capacity;
    newData = bender_realloc(old, capacity * sizeof(Bender));
    if (NULL == newData) {
        BENDER_OUT_OF_MEMORY;
    }
    bender_vm.next_collection += (capacity - array->capacity) * sizeof(Bender);
    array->data = newData;
    array->capacity = capacity;
}

/* Set the count of an array. Extend with nil if needed. */
void bender_array_setcount(BenderArray *array, int32_t count) {
    if (count < 0)
        return;
    if (count > array->count) {
        int32_t i;
        bender_array_ensure(array, count, 1);
        for (i = array->count; i < count; i++) {
            array->data[i] = bender_wrap_nil();
        }
    }
    array->count = count;
}

/* Push a value to the top of the array */
void bender_array_push(BenderArray *array, Bender x) {
    if (array->count == INT32_MAX) {
        bender_panic("array overflow");
    }
    int32_t newcount = array->count + 1;
    bender_array_ensure(array, newcount, 2);
    array->data[array->count] = x;
    array->count = newcount;
    /* Generational barrier (Phase 2b-iii): old array, new edge. */
    bender_gc_barrier(array, x);
}

/* Pop a value from the top of the array */
Bender bender_array_pop(BenderArray *array) {
    if (array->count) {
        return array->data[--array->count];
    } else {
        return bender_wrap_nil();
    }
}

/* Look at the last value in the array */
Bender bender_array_peek(BenderArray *array) {
    if (array->count) {
        return array->data[array->count - 1];
    } else {
        return bender_wrap_nil();
    }
}

/* C Functions */

BENDER_CORE_FN(cfun_array_new,
              "(array/new capacity)",
              "Creates and returns a new empty array with a pre-allocated "
              "capacity. The same as `array` but can be more efficient if "
              "the maximum size is known.") {
    bender_fixarity(argc, 1);
    int32_t cap = bender_getinteger(argv, 0);
    BenderArray *array = bender_array(cap);
    return bender_wrap_array(array);
}

BENDER_CORE_FN(cfun_array_weak,
              "(array/weak capacity)",
              "Creates and returns a new empty array with a pre-allocated "
              "capacity and support for weak references. Similar to "
              "`array/new`.") {
    bender_fixarity(argc, 1);
    int32_t cap = bender_getinteger(argv, 0);
    BenderArray *array = bender_array_weak(cap);
    return bender_wrap_array(array);
}

BENDER_CORE_FN(cfun_array_new_filled,
              "(array/new-filled n &opt val)",
              "Creates and returns a new array of `n` values, all set to "
              "`val`, which defaults to nil.") {
    bender_arity(argc, 1, 2);
    int32_t count = bender_getnat(argv, 0);
    Bender x = (argc == 2) ? argv[1] : bender_wrap_nil();
    BenderArray *array = bender_array(count);
    for (int32_t i = 0; i < count; i++) {
        array->data[i] = x;
    }
    array->count = count;
    return bender_wrap_array(array);
}

BENDER_CORE_FN(cfun_array_fill,
              "(array/fill arr &opt val)",
              "Replace all values of an array `arr` with `val` "
              "(defaulting to nil) without changing the length of `arr`. "
              "Returns `arr`.") {
    bender_arity(argc, 1, 2);
    BenderArray *array = bender_getarray(argv, 0);
    Bender x = (argc == 2) ? argv[1] : bender_wrap_nil();
    for (int32_t i = 0; i < array->count; i++) {
        array->data[i] = x;
    }
    /* Generational barrier (Phase 2b-iii): old array, new edge. One call
     * covers the bulk fill (same value everywhere). */
    bender_gc_barrier(array, x);
    return argv[0];
}

BENDER_CORE_FN(cfun_array_pop,
              "(array/pop arr)",
              "Remove and return the last value of an array `arr`. If "
              "`arr` is empty, returns nil. Modifies `arr`.") {
    bender_fixarity(argc, 1);
    BenderArray *array = bender_getarray(argv, 0);
    return bender_array_pop(array);
}

BENDER_CORE_FN(cfun_array_peek,
              "(array/peek arr)",
              "Returns the last value of an array `arr`. Does not modify "
              "`arr`.") {
    bender_fixarity(argc, 1);
    BenderArray *array = bender_getarray(argv, 0);
    return bender_array_peek(array);
}

BENDER_CORE_FN(cfun_array_push,
              "(array/push arr & xs)",
              "Push all the values of `xs` to the end of an array `arr`. "
              "Returns `arr`.") {
    bender_arity(argc, 1, -1);
    BenderArray *array = bender_getarray(argv, 0);
    if (INT32_MAX - argc + 1 <= array->count) {
        bender_panic("array overflow");
    }
    int32_t newcount = array->count - 1 + argc;
    bender_array_ensure(array, newcount, 2);
    if (argc > 1) {
        /* Generational barrier: the memcpy bulk path skips the
         * per-element write barrier, so an old array bulk-pushing
         * young values would leave the old<-young edges unrecorded.
         * Record each edge before the copy (barrier is header-only,
         * value semantics unaffected). Same discipline array/insert
         * already applies to its argv chunk. */
        for (int32_t i = 1; i < argc; i++)
            bender_gc_barrier(array, argv[i]);
        memcpy(array->data + array->count, argv + 1, (size_t)(argc - 1) * sizeof(Bender));
    }
    array->count = newcount;
    return argv[0];
}

BENDER_CORE_FN(cfun_array_ensure,
              "(array/ensure arr capacity growth)",
              "Ensures that the memory backing the array `arr` is large "
              "enough for `capacity` items at the given rate `growth`. "
              "`capacity` and `growth` must be integers. If the backing "
              "capacity is already enough, then there is no change. "
              "Otherwise, the backing memory will be reallocated so that "
              "there is enough space. Returns `arr`.") {
    bender_fixarity(argc, 3);
    BenderArray *array = bender_getarray(argv, 0);
    int32_t newcount = bender_getinteger(argv, 1);
    int32_t growth = bender_getinteger(argv, 2);
    if (newcount < 1) bender_panic("expected positive integer");
    bender_array_ensure(array, newcount, growth);
    return argv[0];
}

BENDER_CORE_FN(cfun_array_slice,
              "(array/slice ind &opt start end)",
              "Same as `tuple/slice`, but returns an array.") {
    BenderView view = bender_getindexed(argv, 0);
    BenderRange range = bender_getslice(argc, argv);
    BenderArray *array = bender_array(range.end - range.start);
    if (array->data)
        memcpy(array->data, view.items + range.start, sizeof(Bender) * (range.end - range.start));
    array->count = range.end - range.start;
    return bender_wrap_array(array);
}

BENDER_CORE_FN(cfun_array_concat,
              "(array/concat arr & parts)",
              "Concatenates `parts`, a variable number of arrays and "
              "tuples, into the first argument `arr` which must be an "
              "array. If any of `parts` are arrays or tuples, their values "
              "are inserted into `arr`. Otherwise, each value in `parts` "
              "will be appended to `arr` in order. Returns `arr`.") {
    int32_t i;
    bender_arity(argc, 1, -1);
    BenderArray *array = bender_getarray(argv, 0);
    for (i = 1; i < argc; i++) {
        switch (bender_type(argv[i])) {
            default:
                bender_array_push(array, argv[i]);
                break;
            case BENDER_ARRAY:
            case BENDER_TUPLE: {
                int32_t j, len = 0;
                const Bender *vals = NULL;
                bender_indexed_view(argv[i], &vals, &len);
                if (array->data == vals) {
                    int32_t newcount = array->count + len;
                    bender_array_ensure(array, newcount, 2);
                    bender_indexed_view(argv[i], &vals, &len);
                }
                for (j = 0; j < len; j++)
                    bender_array_push(array, vals[j]);
            }
            break;
        }
    }
    return bender_wrap_array(array);
}

BENDER_CORE_FN(cfun_array_join,
              "(array/join arr & parts)",
              "Join `parts`, a variable number of arrays and tuples, into "
              "the first argument `arr` which must be an array. Returns "
              "`arr`.") {
    int32_t i;
    bender_arity(argc, 1, -1);
    BenderArray *array = bender_getarray(argv, 0);
    for (i = 1; i < argc; i++) {
        int32_t j, len = 0;
        const Bender *vals = NULL;
        if (!bender_indexed_view(argv[i], &vals, &len)) {
            bender_panicf("expected indexed type for argument %d, got %v", i, argv[i]);
        }
        if (array->data == vals) {
            int32_t newcount = array->count + len;
            bender_array_ensure(array, newcount, 2);
            bender_indexed_view(argv[i], &vals, &len);
        }
        for (j = 0; j < len; j++)
            bender_array_push(array, vals[j]);
    }
    return bender_wrap_array(array);
}

BENDER_CORE_FN(cfun_array_insert,
              "(array/insert arr at & xs)",
              "Insert all `xs` into array `arr` at index `at`. `at` can be "
              "an integer between 0 and the length of `arr`. If `at` is "
              "negative, will index backwards from the end of `arr`, "
              "inserting after the index such that inserting at -1 appends "
              "to `arr`. Returns `arr`.") {
    size_t chunksize, restsize;
    bender_arity(argc, 2, -1);
    BenderArray *array = bender_getarray(argv, 0);
    int32_t at = bender_getinteger(argv, 1);
    if (at < 0) {
        at = array->count + at + 1;
    }
    if (at < 0 || at > array->count)
        bender_panicf("insertion index %d out of range [0,%d]", at, array->count);
    chunksize = (argc - 2) * sizeof(Bender);
    restsize = (array->count - at) * sizeof(Bender);
    if (INT32_MAX - (argc - 2) < array->count) {
        bender_panic("array overflow");
    }
    bender_array_ensure(array, array->count + argc - 2, 2);
    if (restsize) {
        memmove(array->data + at + argc - 2,
                array->data + at,
                restsize);
    }
    safe_memcpy(array->data + at, argv + 2, chunksize);
    array->count += (argc - 2);
    /* Generational barrier (Phase 2b-iii): the memmoved prefix keeps its
     * old edges, but the argv values are new -- cover each of them. */
    for (int32_t i = 2; i < argc; i++) {
        bender_gc_barrier(array, argv[i]);
    }
    return argv[0];
}

BENDER_CORE_FN(cfun_array_remove,
              "(array/remove arr at &opt n)",
              "Remove up to `n` values starting at index `at` in array "
              "`arr`. `at` can index from the end of the array with a "
              "negative index, and `n` must be a non-negative integer. "
              "By default `n` is 1. Returns `arr`.") {
    bender_arity(argc, 2, 3);
    BenderArray *array = bender_getarray(argv, 0);
    int32_t at = bender_getinteger(argv, 1);
    int32_t n = 1;
    if (at < 0) {
        at = array->count + at;
    }
    if (at < 0 || at > array->count)
        bender_panicf("removal index %d out of range [0,%d]", at, array->count);
    if (argc == 3) {
        n = bender_getinteger(argv, 2);
        if (n < 0)
            bender_panicf("expected non-negative integer for argument n, got %v", argv[2]);
    }
    if (at + n > array->count) {
        n = array->count - at;
    }
    memmove(array->data + at,
            array->data + at + n,
            (array->count - at - n) * sizeof(Bender));
    array->count -= n;
    return argv[0];
}

BENDER_CORE_FN(cfun_array_trim,
              "(array/trim arr)",
              "Set the backing capacity of an array `arr` to its current "
              "length. Returns `arr`.") {
    bender_fixarity(argc, 1);
    BenderArray *array = bender_getarray(argv, 0);
    if (array->count) {
        if (array->count < array->capacity) {
            Bender *newData = bender_realloc(array->data, array->count * sizeof(Bender));
            if (NULL == newData) {
                BENDER_OUT_OF_MEMORY;
            }
            array->data = newData;
            array->capacity = array->count;
        }
    } else {
        array->capacity = 0;
        bender_free(array->data);
        array->data = NULL;
    }
    return argv[0];
}

BENDER_CORE_FN(cfun_array_clear,
              "(array/clear arr)",
              "Empties an array `arr`, setting its count to 0 but does not "
              "free the backing capacity. Returns `arr`.") {
    bender_fixarity(argc, 1);
    BenderArray *array = bender_getarray(argv, 0);
    array->count = 0;
    return argv[0];
}

/* Load the array module */
void bender_lib_array(BenderTable *env) {
    BenderRegExt array_cfuns[] = {
        BENDER_CORE_REG("array/new", cfun_array_new),
        BENDER_CORE_REG("array/weak", cfun_array_weak),
        BENDER_CORE_REG("array/new-filled", cfun_array_new_filled),
        BENDER_CORE_REG("array/fill", cfun_array_fill),
        BENDER_CORE_REG("array/pop", cfun_array_pop),
        BENDER_CORE_REG("array/peek", cfun_array_peek),
        BENDER_CORE_REG("array/push", cfun_array_push),
        BENDER_CORE_REG("array/ensure", cfun_array_ensure),
        BENDER_CORE_REG("array/slice", cfun_array_slice),
        BENDER_CORE_REG("array/concat", cfun_array_concat),
        BENDER_CORE_REG("array/insert", cfun_array_insert),
        BENDER_CORE_REG("array/remove", cfun_array_remove),
        BENDER_CORE_REG("array/trim", cfun_array_trim),
        BENDER_CORE_REG("array/clear", cfun_array_clear),
        BENDER_CORE_REG("array/join", cfun_array_join),
        BENDER_REG_END
    };
    bender_core_cfuns_ext(env, NULL, array_cfuns);
}
