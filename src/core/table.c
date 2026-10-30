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
#include <math.h>
#endif

#define BENDER_TABLE_FLAG_STACK 0x10000

static void *bender_memalloc_empty_local(int32_t count) {
    int32_t i;
    void *mem = bender_smalloc((size_t) count * sizeof(BenderKV));
    BenderKV *mmem = (BenderKV *)mem;
    for (i = 0; i < count; i++) {
        BenderKV *kv = mmem + i;
        kv->key = bender_wrap_nil();
        kv->value = bender_wrap_nil();
    }
    return mem;
}

static BenderTable *bender_table_init_impl(BenderTable *table, int32_t capacity, int stackalloc) {
    BenderKV *data;
    capacity = bender_tablen(capacity);
    if (stackalloc) table->gc.flags = BENDER_TABLE_FLAG_STACK;
    if (capacity) {
        if (stackalloc) {
            data = bender_memalloc_empty_local(capacity);
        } else {
            data = (BenderKV *) bender_memalloc_empty(capacity);
            if (NULL == data) {
                BENDER_OUT_OF_MEMORY;
            }
        }
        table->data = data;
        table->capacity = capacity;
    } else {
        table->data = NULL;
        table->capacity = 0;
    }
    table->count = 0;
    table->deleted = 0;
    table->proto = NULL;
    return table;
}

/* Initialize a table (for use with scratch memory) */
BenderTable *bender_table_init(BenderTable *table, int32_t capacity) {
    return bender_table_init_impl(table, capacity, 1);
}

/* Initialize a table without using scratch memory */
BenderTable *bender_table_init_raw(BenderTable *table, int32_t capacity) {
    return bender_table_init_impl(table, capacity, 0);
}

/* Deinitialize a table */
void bender_table_deinit(BenderTable *table) {
    if (table->gc.flags & BENDER_TABLE_FLAG_STACK) {
        bender_sfree(table->data);
    } else {
        bender_free(table->data);
    }
}

/* Create a new table */

BenderTable *bender_table(int32_t capacity) {
    BenderTable *table = bender_gcalloc(BENDER_MEMORY_TABLE, sizeof(BenderTable));
    return bender_table_init_impl(table, capacity, 0);
}

BenderTable *bender_table_weakk(int32_t capacity) {
    BenderTable *table = bender_gcalloc(BENDER_MEMORY_TABLE_WEAKK, sizeof(BenderTable));
    return bender_table_init_impl(table, capacity, 0);
}

BenderTable *bender_table_weakv(int32_t capacity) {
    BenderTable *table = bender_gcalloc(BENDER_MEMORY_TABLE_WEAKV, sizeof(BenderTable));
    return bender_table_init_impl(table, capacity, 0);
}

BenderTable *bender_table_weakkv(int32_t capacity) {
    BenderTable *table = bender_gcalloc(BENDER_MEMORY_TABLE_WEAKKV, sizeof(BenderTable));
    return bender_table_init_impl(table, capacity, 0);
}

/* Find the bucket that contains the given key. Will also return
 * bucket where key should go if not in the table. */
BenderKV *bender_table_find(BenderTable *t, Bender key) {
    return (BenderKV *) bender_dict_find(t->data, t->capacity, key);
}

/* Resize the dictionary table. */
static void bender_table_rehash(BenderTable *t, int32_t size) {
    BenderKV *olddata = t->data;
    BenderKV *newdata;
    int islocal = t->gc.flags & BENDER_TABLE_FLAG_STACK;
    if (islocal) {
        newdata = (BenderKV *) bender_memalloc_empty_local(size);
    } else {
        newdata = (BenderKV *) bender_memalloc_empty(size);
        if (NULL == newdata) {
            BENDER_OUT_OF_MEMORY;
        }
    }
    int32_t oldcapacity = t->capacity;
    t->data = newdata;
    t->capacity = size;
    t->deleted = 0;
    for (int32_t i = 0; i < oldcapacity; i++) {
        BenderKV *kv = olddata + i;
        if (!bender_checktype(kv->key, BENDER_NIL)) {
            BenderKV *newkv = bender_table_find(t, kv->key);
            *newkv = *kv;
        }
    }
    if (islocal) {
        bender_sfree(olddata);
    } else {
        bender_free(olddata);
    }
}

/* Get a value out of the table */
Bender bender_table_get(BenderTable *t, Bender key) {
    for (int i = BENDER_MAX_PROTO_DEPTH; t && i; t = t->proto, --i) {
        BenderKV *bucket = bender_table_find(t, key);
        if (NULL != bucket && !bender_checktype(bucket->key, BENDER_NIL))
            return bucket->value;
    }
    return bender_wrap_nil();
}

/* Native-executor table read fast path. Restrict the key to primitive
 * values so bender_hash/bender_equals cannot invoke an abstract callback
 * or allocate while the executor's structural verdict is live. The table
 * itself may contain any keys: a primitive query never calls their
 * equality hook. */
BENDER_API int bender_native_table_get_fast(Bender ds, Bender key, Bender *out) {
    if (!bender_checktype(ds, BENDER_TABLE)) return 0;
    switch (bender_type(key)) {
        case BENDER_NUMBER:
        case BENDER_NIL:
        case BENDER_BOOLEAN:
        case BENDER_STRING:
        case BENDER_SYMBOL:
        case BENDER_KEYWORD:
            *out = bender_table_get(bender_unwrap_table(ds), key);
            return 1;
        default:
            return 0;
    }
}

static int bender_native_primitive_key_p(Bender key) {
    switch (bender_type(key)) {
        case BENDER_NUMBER:
        case BENDER_NIL:
        case BENDER_BOOLEAN:
        case BENDER_STRING:
        case BENDER_SYMBOL:
        case BENDER_KEYWORD:
            return 1;
        default:
            return 0;
    }
}

/* Non-function Bender values are callable with exactly one argument:
 * (table key), (struct key), (array index), (tuple index), and the byte
 * sequence types. The VM routes this through call_nonfn and
 * bender_method_invoke to bender_in. Executors recognize the cases that
 * cannot allocate, panic, or invoke a callback and return the exact
 * bender_in result inline. Out-of-range indices, composite keys, and all
 * other callee types stay with the VM so its errors and method hooks are
 * preserved. */
BENDER_API int bender_native_container_call_fast(Bender callee, int32_t argc,
        const Bender *argv, Bender *out) {
    if (argc != 1) return 0;
    switch (bender_type(callee)) {
        case BENDER_TABLE:
            if (!bender_native_primitive_key_p(argv[0])) return 0;
            *out = bender_table_get(bender_unwrap_table(callee), argv[0]);
            return 1;
        case BENDER_STRUCT:
            if (!bender_native_primitive_key_p(argv[0])) return 0;
            *out = bender_struct_get(bender_unwrap_struct(callee), argv[0]);
            return 1;
        case BENDER_ARRAY: {
            BenderArray *a = bender_unwrap_array(callee);
            int32_t i;
            if (!bender_checkint(argv[0])) return 0;
            i = bender_unwrap_integer(argv[0]);
            if (i < 0 || i >= a->count) return 0;
            *out = a->data[i];
            return 1;
        }
        case BENDER_TUPLE: {
            const Bender *t = bender_unwrap_tuple(callee);
            int32_t i, n = bender_tuple_length(t);
            if (!bender_checkint(argv[0])) return 0;
            i = bender_unwrap_integer(argv[0]);
            if (i < 0 || i >= n) return 0;
            *out = t[i];
            return 1;
        }
        case BENDER_STRING:
        case BENDER_SYMBOL:
        case BENDER_KEYWORD: {
            const uint8_t *str = bender_unwrap_string(callee);
            int32_t i, n = bender_string_length(str);
            if (!bender_checkint(argv[0])) return 0;
            i = bender_unwrap_integer(argv[0]);
            if (i < 0 || i >= n) return 0;
            *out = bender_wrap_integer(str[i]);
            return 1;
        }
        case BENDER_BUFFER: {
            BenderBuffer *b = bender_unwrap_buffer(callee);
            int32_t i;
            if (!bender_checkint(argv[0])) return 0;
            i = bender_unwrap_integer(argv[0]);
            if (i < 0 || i >= b->count) return 0;
            *out = bender_wrap_integer(b->data[i]);
            return 1;
        }
        default:
            return 0;
    }
}

/* Native-executor table write fast path for an already-present,
 * primitive key. bender_table_put's existing-key branch is just a value
 * store plus both generational barriers; insertion (which may rehash and
 * allocate) and nil removal stay in the VM. */
static int native_table_put_fast_impl(Bender ds, Bender key, Bender value
#if defined(BENDER_NATIVE_COMMIT_PROBE) && BENDER_NATIVE_COMMIT_PROBE
        , BenderNativeCommitHook commit, void *context
#endif
) {
    if (!bender_checktype(ds, BENDER_TABLE)) return 0;
    if (bender_checktype(key, BENDER_NIL)) { /* bender_table_put no-op */
#if defined(BENDER_NATIVE_COMMIT_PROBE) && BENDER_NATIVE_COMMIT_PROBE
        if (commit) commit(context);
#endif
        return 1;
    }
    if (bender_checktype(key, BENDER_NUMBER) && isnan(bender_unwrap_number(key))) {
#if defined(BENDER_NATIVE_COMMIT_PROBE) && BENDER_NATIVE_COMMIT_PROBE
        if (commit) commit(context);
#endif
        return 1;
    }
    if (bender_checktype(value, BENDER_NIL)) return 0;
    switch (bender_type(key)) {
        case BENDER_NUMBER:
        case BENDER_BOOLEAN:
        case BENDER_STRING:
        case BENDER_SYMBOL:
        case BENDER_KEYWORD:
            break;
        default:
            return 0;
    }
    {
        BenderTable *t = bender_unwrap_table(ds);
        BenderKV *bucket = bender_table_find(t, key);
        if (NULL == bucket || bender_checktype(bucket->key, BENDER_NIL)) return 0;
        bucket->value = value;
#if defined(BENDER_NATIVE_COMMIT_PROBE) && BENDER_NATIVE_COMMIT_PROBE
        if (commit) commit(context);
#endif
        bender_gc_barrier(t, key);
        bender_gc_barrier(t, value);
        return 1;
    }
}

BENDER_API int bender_native_table_put_fast(Bender ds, Bender key, Bender value) {
#if defined(BENDER_NATIVE_COMMIT_PROBE) && BENDER_NATIVE_COMMIT_PROBE
    return native_table_put_fast_impl(ds, key, value, NULL, NULL);
#else
    return native_table_put_fast_impl(ds, key, value);
#endif
}
#if defined(BENDER_NATIVE_COMMIT_PROBE) && BENDER_NATIVE_COMMIT_PROBE
BENDER_API int bender_native_table_put_fast_probe(Bender ds, Bender key, Bender value,
        BenderNativeCommitHook commit, void *context) {
    return native_table_put_fast_impl(ds, key, value, commit, context);
}
#endif

/* Used internally for compiler stuff */
Bender bender_table_get_keyword(BenderTable *t, const char *keyword) {
    int32_t keyword_len = (int32_t) strlen(keyword);
    for (int i = BENDER_MAX_PROTO_DEPTH; t && i; t = t->proto, --i) {
        BenderKV *bucket = (BenderKV *) bender_dict_find_keyword(t->data, t->capacity, (const uint8_t *) keyword, keyword_len);
        if (NULL != bucket && !bender_checktype(bucket->key, BENDER_NIL))
            return bucket->value;
    }
    return bender_wrap_nil();
}

/* Get a value out of the table, and record which prototype it was from. */
Bender bender_table_get_ex(BenderTable *t, Bender key, BenderTable **which) {
    for (int i = BENDER_MAX_PROTO_DEPTH; t && i; t = t->proto, --i) {
        BenderKV *bucket = bender_table_find(t, key);
        if (NULL != bucket && !bender_checktype(bucket->key, BENDER_NIL)) {
            *which = t;
            return bucket->value;
        }
    }
    return bender_wrap_nil();
}

/* Get a value out of the table. Don't check prototype tables. */
Bender bender_table_rawget(BenderTable *t, Bender key) {
    BenderKV *bucket = bender_table_find(t, key);
    if (NULL != bucket && !bender_checktype(bucket->key, BENDER_NIL))
        return bucket->value;
    else
        return bender_wrap_nil();
}

/* Remove an entry from the dictionary. Return the value that
 * was removed. */
Bender bender_table_remove(BenderTable *t, Bender key) {
    BenderKV *bucket = bender_table_find(t, key);
    if (NULL != bucket && !bender_checktype(bucket->key, BENDER_NIL)) {
        Bender ret = bucket->value;
        t->count--;
        t->deleted++;
        bucket->key = bender_wrap_nil();
        bucket->value = bender_wrap_false();
        return ret;
    } else {
        return bender_wrap_nil();
    }
}

/* Put a value into the object */
void bender_table_put(BenderTable *t, Bender key, Bender value) {
    if (bender_checktype(key, BENDER_NIL)) return;
    if (bender_checktype(key, BENDER_NUMBER) && isnan(bender_unwrap_number(key))) return;
    if (bender_checktype(value, BENDER_NIL)) {
        bender_table_remove(t, key);
    } else {
        BenderKV *bucket = bender_table_find(t, key);
        if (NULL != bucket && !bender_checktype(bucket->key, BENDER_NIL)) {
            bucket->value = value;
            /* Generational barrier (Phase 2b-iii): old table, new edge. */
            bender_gc_barrier(t, key);
            bender_gc_barrier(t, value);
        } else {
            if (NULL == bucket || 2 * (t->count + t->deleted + 1) > t->capacity) {
                bender_table_rehash(t, bender_tablen(2 * t->count + 2));
            }
            bucket = bender_table_find(t, key);
            if (bender_checktype(bucket->value, BENDER_BOOLEAN))
                --t->deleted;
            bucket->key = key;
            bucket->value = value;
            ++t->count;
            /* Generational barrier (Phase 2b-iii): old table, new edge. */
            bender_gc_barrier(t, key);
            bender_gc_barrier(t, value);
        }
    }
}

/* P3.4 bookkeeping put (marshal memo, threaded registry): identical
 * to bender_table_put except generational-dirty only (bender_gc_touch)
 * — no xfb record, no export panic. The containers are identity maps
 * (never the value graph); dirty keeps minors sound. Declared in
 * util.h for cross-TU use. */
void bender_table_put_raw(BenderTable *t, Bender key, Bender value) {
    if (bender_checktype(key, BENDER_NIL)) return;
    if (bender_checktype(key, BENDER_NUMBER) && isnan(bender_unwrap_number(key))) return;
    if (bender_checktype(value, BENDER_NIL)) {
        bender_table_remove(t, key);
    } else {
        BenderKV *bucket = bender_table_find(t, key);
        if (NULL != bucket && !bender_checktype(bucket->key, BENDER_NIL)) {
            bucket->value = value;
            bender_gc_touch(t);
        } else {
            if (NULL == bucket || 2 * (t->count + t->deleted + 1) > t->capacity) {
                bender_table_rehash(t, bender_tablen(2 * t->count + 2));
            }
            bucket = bender_table_find(t, key);
            if (bender_checktype(bucket->value, BENDER_BOOLEAN))
                --t->deleted;
            bucket->key = key;
            bucket->value = value;
            ++t->count;
            bender_gc_touch(t);
        }
    }
}

/* Used internally so don't check arguments
 * Put into a table, but if the key already exists do nothing. */
static void bender_table_put_no_overwrite(BenderTable *t, Bender key, Bender value) {
    BenderKV *bucket = bender_table_find(t, key);
    if (NULL != bucket && !bender_checktype(bucket->key, BENDER_NIL))
        return;
    if (NULL == bucket || 2 * (t->count + t->deleted + 1) > t->capacity) {
        bender_table_rehash(t, bender_tablen(2 * t->count + 2));
    }
    bucket = bender_table_find(t, key);
    if (bender_checktype(bucket->value, BENDER_BOOLEAN))
        --t->deleted;
    bucket->key = key;
    bucket->value = value;
    ++t->count;
    /* Generational barrier (Phase 2b-iii): old table, new edge. */
    bender_gc_barrier(t, key);
    bender_gc_barrier(t, value);
}

/* Clear a table */
void bender_table_clear(BenderTable *t) {
    int32_t capacity = t->capacity;
    BenderKV *data = t->data;
    bender_memempty(data, capacity);
    t->count = 0;
    t->deleted = 0;
}

/* Clone a table. */
BenderTable *bender_table_clone(BenderTable *table) {
    BenderTable *newTable = bender_gcalloc(BENDER_MEMORY_TABLE, sizeof(BenderTable));
    newTable->count = table->count;
    newTable->capacity = table->capacity;
    newTable->deleted = table->deleted;
    newTable->proto = table->proto;
    newTable->data = array_allocate(sizeof(BenderKV), newTable->capacity);
    if (NULL == newTable->data) {
        BENDER_OUT_OF_MEMORY;
    }
    memcpy(newTable->data, table->data, (size_t) table->capacity * sizeof(BenderKV));
    return newTable;
}

/* Merge a table or struct into a table */
static void bender_table_mergekv(BenderTable *table, const BenderKV *kvs, int32_t cap) {
    int32_t i;
    for (i = 0; i < cap; i++) {
        const BenderKV *kv = kvs + i;
        if (!bender_checktype(kv->key, BENDER_NIL)) {
            bender_table_put(table, kv->key, kv->value);
        }
    }
}

/* Merge a table into another table */
void bender_table_merge_table(BenderTable *table, BenderTable *other) {
    bender_table_mergekv(table, other->data, other->capacity);
}

/* Merge a struct into a table */
void bender_table_merge_struct(BenderTable *table, const BenderKV *other) {
    bender_table_mergekv(table, other, bender_struct_capacity(other));
}

/* Convert table to struct */
const BenderKV *bender_table_to_struct(BenderTable *t) {
    BenderKV *st = bender_struct_begin(t->count);
    BenderKV *kv = t->data;
    BenderKV *end = t->data + t->capacity;
    while (kv < end) {
        if (!bender_checktype(kv->key, BENDER_NIL))
            bender_struct_put(st, kv->key, kv->value);
        kv++;
    }
    return bender_struct_end(st);
}

BenderTable *bender_table_proto_flatten(BenderTable *t) {
    BenderTable *newTable = bender_table(0);
    while (t) {
        BenderKV *kv = t->data;
        BenderKV *end = t->data + t->capacity;
        while (kv < end) {
            if (!bender_checktype(kv->key, BENDER_NIL))
                bender_table_put_no_overwrite(newTable, kv->key, kv->value);
            kv++;
        }
        t = t->proto;
    }
    return newTable;
}

/* C Functions */

BENDER_CORE_FN(cfun_table_new,
              "(table/new capacity)",
              "Creates a new empty table with pre-allocated memory "
              "for `capacity` entries. This means that if one knows the number of "
              "entries going into a table on creation, extra memory allocation "
              "can be avoided. "
              "Returns the new table.") {
    bender_fixarity(argc, 1);
    int32_t cap = bender_getnat(argv, 0);
    return bender_wrap_table(bender_table(cap));
}

BENDER_CORE_FN(cfun_table_weak,
              "(table/weak capacity)",
              "Creates a new empty table with weak references to keys and values. Similar to `table/new`. "
              "Returns the new table.") {
    bender_fixarity(argc, 1);
    int32_t cap = bender_getnat(argv, 0);
    return bender_wrap_table(bender_table_weakkv(cap));
}

BENDER_CORE_FN(cfun_table_weak_keys,
              "(table/weak-keys capacity)",
              "Creates a new empty table with weak references to keys and normal references to values. Similar to `table/new`. "
              "Returns the new table.") {
    bender_fixarity(argc, 1);
    int32_t cap = bender_getnat(argv, 0);
    return bender_wrap_table(bender_table_weakk(cap));
}

BENDER_CORE_FN(cfun_table_weak_values,
              "(table/weak-values capacity)",
              "Creates a new empty table with normal references to keys and weak references to values. Similar to `table/new`. "
              "Returns the new table.") {
    bender_fixarity(argc, 1);
    int32_t cap = bender_getnat(argv, 0);
    return bender_wrap_table(bender_table_weakv(cap));
}

BENDER_CORE_FN(cfun_table_getproto,
              "(table/getproto tab)",
              "Get the prototype table of a table. Returns nil if the table "
              "has no prototype, otherwise returns the prototype.") {
    bender_fixarity(argc, 1);
    BenderTable *t = bender_gettable(argv, 0);
    return t->proto
           ? bender_wrap_table(t->proto)
           : bender_wrap_nil();
}

BENDER_CORE_FN(cfun_table_setproto,
              "(table/setproto tab proto)",
              "Set the prototype of a table. Returns the original table `tab`.") {
    bender_fixarity(argc, 2);
    BenderTable *table = bender_gettable(argv, 0);
    BenderTable *proto = NULL;
    if (!bender_checktype(argv[1], BENDER_NIL)) {
        proto = bender_gettable(argv, 1);
    }
    table->proto = proto;
    /* Generational barrier (Phase 2b-iii): a young proto on an old table
     * is a new edge minors would otherwise miss. */
    if (NULL != proto) {
        bender_gc_barrier(table, bender_wrap_table(proto));
    }
    return argv[0];
}

BENDER_CORE_FN(cfun_table_tostruct,
              "(table/to-struct tab &opt proto)",
              "Return a struct based on a table `tab`. If given, "
              "the optional argument `proto` specifies the new "
              "struct's prototype. Note that if `proto` is not "
              "specified, the new struct will not have a "
              "prototype.") {
    bender_arity(argc, 1, 2);
    BenderTable *t = bender_gettable(argv, 0);
    BenderStruct proto = bender_optstruct(argv, argc, 1, NULL);
    BenderStruct st = bender_table_to_struct(t);
    bender_struct_proto(st) = proto;
    return bender_wrap_struct(st);
}

BENDER_CORE_FN(cfun_table_rawget,
              "(table/rawget tab key)",
              "Gets a value from a table `tab` without looking at the prototype table. "
              "If `tab` does not contain the key directly, the function will return "
              "nil without checking the prototype. Returns the value in the table.") {
    bender_fixarity(argc, 2);
    BenderTable *table = bender_gettable(argv, 0);
    return bender_table_rawget(table, argv[1]);
}

BENDER_CORE_FN(cfun_table_clone,
              "(table/clone tab)",
              "Create a copy of a table. Updates to the new table will not change the old table, "
              "and vice versa.") {
    bender_fixarity(argc, 1);
    BenderTable *table = bender_gettable(argv, 0);
    return bender_wrap_table(bender_table_clone(table));
}

BENDER_CORE_FN(cfun_table_clear,
              "(table/clear tab)",
              "Remove all key-value pairs in a table and return the modified table `tab`.") {
    bender_fixarity(argc, 1);
    BenderTable *table = bender_gettable(argv, 0);
    bender_table_clear(table);
    return bender_wrap_table(table);
}

BENDER_CORE_FN(cfun_table_proto_flatten,
              "(table/proto-flatten tab)",
              "Create a new table that is the result of merging all prototypes into a new table.") {
    bender_fixarity(argc, 1);
    BenderTable *table = bender_gettable(argv, 0);
    return bender_wrap_table(bender_table_proto_flatten(table));
}

/* Load the table module */
void bender_lib_table(BenderTable *env) {
    BenderRegExt table_cfuns[] = {
        BENDER_CORE_REG("table/new", cfun_table_new),
        BENDER_CORE_REG("table/weak", cfun_table_weak),
        BENDER_CORE_REG("table/weak-keys", cfun_table_weak_keys),
        BENDER_CORE_REG("table/weak-values", cfun_table_weak_values),
        BENDER_CORE_REG("table/to-struct", cfun_table_tostruct),
        BENDER_CORE_REG("table/getproto", cfun_table_getproto),
        BENDER_CORE_REG("table/setproto", cfun_table_setproto),
        BENDER_CORE_REG("table/rawget", cfun_table_rawget),
        BENDER_CORE_REG("table/clone", cfun_table_clone),
        BENDER_CORE_REG("table/clear", cfun_table_clear),
        BENDER_CORE_REG("table/proto-flatten", cfun_table_proto_flatten),
        BENDER_REG_END
    };
    bender_core_cfuns_ext(env, NULL, table_cfuns);
}
