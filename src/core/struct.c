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

/* Begin creation of a struct */
BenderKV *bender_struct_begin(int32_t count) {
    /* Calculate capacity as power of 2 after 2 * count. */
    int32_t capacity = bender_tablen(2 * count);
    if (capacity < 0) capacity = bender_tablen(count + 1);

    size_t size = sizeof(BenderStructHead) + (size_t) capacity * sizeof(BenderKV);
    BenderStructHead *head = bender_gcalloc(BENDER_MEMORY_STRUCT, size);
    head->length = count;
    head->capacity = capacity;
    head->hash = 0;
    head->proto = NULL;

    BenderKV *st = (BenderKV *)(head->data);
    bender_memempty(st, capacity);
    return st;
}

/* Find an item in a struct without looking for prototypes. Should be similar to bender_dict_find, but
 * specialized to structs (slightly more compact). */
const BenderKV *bender_struct_find(const BenderKV *st, Bender key) {
    int32_t cap = bender_struct_capacity(st);
    int32_t index = bender_maphash(cap, bender_hash(key));
    int32_t i;
    for (i = index; i < cap; i++)
        if (bender_checktype(st[i].key, BENDER_NIL) || bender_key_equals(st[i].key, key))
            return st + i;
    for (i = 0; i < index; i++)
        if (bender_checktype(st[i].key, BENDER_NIL) || bender_key_equals(st[i].key, key))
            return st + i;
    return NULL;
}

/* Put a kv pair into a struct that has not yet been fully constructed.
 * Nil keys and values are ignored, extra keys are ignore, and duplicate keys are
 * ignored.
 *
 * Runs will be in sorted order, as the collisions resolver essentially
 * preforms an in-place insertion sort. This ensures the internal structure of the
 * hash map is independent of insertion order.
 */
void bender_struct_put_ext(BenderKV *st, Bender key, Bender value, int replace) {
    int32_t cap = bender_struct_capacity(st);
    int32_t hash = bender_hash(key);
    int32_t index = bender_maphash(cap, hash);
    int32_t i, j, dist;
    int32_t bounds[4] = {index, cap, 0, index};
    if (bender_checktype(key, BENDER_NIL) || bender_checktype(value, BENDER_NIL)) return;
    if (bender_checktype(key, BENDER_NUMBER) && isnan(bender_unwrap_number(key))) return;
    /* Avoid extra items */
    if (bender_struct_hash(st) == bender_struct_length(st)) return;
    for (dist = 0, j = 0; j < 4; j += 2)
        for (i = bounds[j]; i < bounds[j + 1]; i++, dist++) {
            int status;
            int32_t otherhash;
            int32_t otherindex, otherdist;
            BenderKV *kv = st + i;
            /* We found an empty slot, so just add key and value */
            if (bender_checktype(kv->key, BENDER_NIL)) {
                kv->key = key;
                kv->value = value;
                /* Update the temporary count */
                bender_struct_hash(st)++;
                return;
            }
            /* Robinhood hashing - check if colliding kv pair
             * is closer to their source than current. We use robinhood
             * hashing to ensure that equivalent structs that are constructed
             * with different order have the same internal layout, and therefor
             * will compare properly - i.e., {1 2 3 4} should equal {3 4 1 2}.
             * Collisions are resolved via an insertion sort insertion. */
            otherhash = bender_hash(kv->key);
            otherindex = bender_maphash(cap, otherhash);
            otherdist = (i + cap - otherindex) & (cap - 1);
            if (dist < otherdist)
                status = -1;
            else if (otherdist < dist)
                status = 1;
            else if (hash < otherhash)
                status = -1;
            else if (otherhash < hash)
                status = 1;
            else
                status = bender_compare(key, kv->key);
            /* If other is closer to their ideal slot */
            if (status == 1) {
                /* Swap current kv pair with pair in slot */
                BenderKV temp = *kv;
                kv->key = key;
                kv->value = value;
                key = temp.key;
                value = temp.value;
                /* Save dist and hash of new kv pair */
                dist = otherdist;
                hash = otherhash;
            } else if (status == 0) {
                if (replace) {
                    /* A key was added to the struct more than once - replace old value */
                    kv->value = value;
                }
                return;
            }
        }
}

void bender_struct_put(BenderKV *st, Bender key, Bender value) {
    bender_struct_put_ext(st, key, value, 1);
}

/* Finish building a struct */
const BenderKV *bender_struct_end(BenderKV *st) {
    if (bender_struct_hash(st) != bender_struct_length(st)) {
        /* Error building struct, probably duplicate values. We need to rebuild
         * the struct using only the values that went in. The second creation should always
         * succeed. */
        BenderKV *newst = bender_struct_begin(bender_struct_hash(st));
        for (int32_t i = 0; i < bender_struct_capacity(st); i++) {
            BenderKV *kv = st + i;
            if (!bender_checktype(kv->key, BENDER_NIL)) {
                bender_struct_put(newst, kv->key, kv->value);
            }
        }
        bender_struct_proto(newst) = bender_struct_proto(st);
        st = newst;
    }
    bender_struct_hash(st) = bender_kv_calchash(st, bender_struct_capacity(st));
    if (bender_struct_proto(st)) {
        bender_struct_hash(st) += 2654435761u * bender_struct_hash(bender_struct_proto(st));
    }
    return (const BenderKV *)st;
}

/* Get an item from a struct without looking into prototypes. */
Bender bender_struct_rawget(const BenderKV *st, Bender key) {
    const BenderKV *kv = bender_struct_find(st, key);
    return kv ? kv->value : bender_wrap_nil();
}

/* Get an item from a struct */
Bender bender_struct_get(const BenderKV *st, Bender key) {
    for (int i = BENDER_MAX_PROTO_DEPTH; st && i; --i, st = bender_struct_proto(st)) {
        const BenderKV *kv = bender_struct_find(st, key);
        if (NULL != kv && !bender_checktype(kv->key, BENDER_NIL)) {
            return kv->value;
        }
    }
    return bender_wrap_nil();
}

/* Get an item from a struct, and record which prototype the item came from. */
Bender bender_struct_get_ex(const BenderKV *st, Bender key, BenderStruct *which) {
    for (int i = BENDER_MAX_PROTO_DEPTH; st && i; --i, st = bender_struct_proto(st)) {
        const BenderKV *kv = bender_struct_find(st, key);
        if (NULL != kv && !bender_checktype(kv->key, BENDER_NIL)) {
            *which = st;
            return kv->value;
        }
    }
    return bender_wrap_nil();
}

/* Convert struct to table */
BenderTable *bender_struct_to_table(const BenderKV *st) {
    BenderTable *table = bender_table(bender_struct_capacity(st));
    int32_t i;
    for (i = 0; i < bender_struct_capacity(st); i++) {
        const BenderKV *kv = st + i;
        if (!bender_checktype(kv->key, BENDER_NIL)) {
            bender_table_put(table, kv->key, kv->value);
        }
    }
    return table;
}

/* C Functions */

BENDER_CORE_FN(cfun_struct_with_proto,
              "(struct/with-proto proto & ksvs)",
              "Create a struct using the `proto` argument as the struct's "
              "prototype. `ksvs` are as in the `struct` function.") {
    bender_arity(argc, 1, -1);
    BenderStruct proto = bender_optstruct(argv, argc, 0, NULL);
    if (!(argc & 1))
        bender_panic("expected odd number of arguments");
    BenderKV *st = bender_struct_begin(argc / 2);
    for (int32_t i = 1; i < argc; i += 2) {
        bender_struct_put(st, argv[i], argv[i + 1]);
    }
    bender_struct_proto(st) = proto;
    return bender_wrap_struct(bender_struct_end(st));
}

BENDER_CORE_FN(cfun_struct_getproto,
              "(struct/getproto st)",
              "Return the prototype of a struct, or nil if it doesn't have one.") {
    bender_fixarity(argc, 1);
    BenderStruct st = bender_getstruct(argv, 0);
    return bender_struct_proto(st)
           ? bender_wrap_struct(bender_struct_proto(st))
           : bender_wrap_nil();
}

BENDER_CORE_FN(cfun_struct_flatten,
              "(struct/proto-flatten st)",
              "Convert a struct with prototypes to a struct with no prototypes by merging "
              "all key value pairs from recursive prototypes into one new struct.") {
    bender_fixarity(argc, 1);
    BenderStruct st = bender_getstruct(argv, 0);

    /* get an upper bounds on the number of items in the final struct */
    int64_t pair_count = 0;
    BenderStruct cursor = st;
    while (cursor) {
        pair_count += bender_struct_length(cursor);
        cursor = bender_struct_proto(cursor);
    }

    if (pair_count > INT32_MAX) {
        bender_panic("struct too large");
    }

    BenderKV *accum = bender_struct_begin((int32_t) pair_count);
    cursor = st;
    while (cursor) {
        for (int32_t i = 0; i < bender_struct_capacity(cursor); i++) {
            const BenderKV *kv = cursor + i;
            if (!bender_checktype(kv->key, BENDER_NIL)) {
                bender_struct_put_ext(accum, kv->key, kv->value, 0);
            }
        }
        cursor = bender_struct_proto(cursor);
    }
    return bender_wrap_struct(bender_struct_end(accum));
}

BENDER_CORE_FN(cfun_struct_to_table,
              "(struct/to-table st &opt recursive)",
              "Convert a struct to a table. If recursive is true, also convert the "
              "table's prototypes into the new struct's prototypes as well.") {
    bender_arity(argc, 1, 2);
    BenderStruct st = bender_getstruct(argv, 0);
    int recursive = argc > 1 && bender_truthy(argv[1]);
    BenderTable *tab = NULL;
    BenderStruct cursor = st;
    BenderTable *tab_cursor = tab;
    do {
        if (tab) {
            tab_cursor->proto = bender_table(bender_struct_length(cursor));
            tab_cursor = tab_cursor->proto;
        } else {
            tab = bender_table(bender_struct_length(cursor));
            tab_cursor = tab;
        }
        /* TODO - implement as memcpy since struct memory should be compatible
         * with table memory */
        for (int32_t i = 0; i < bender_struct_capacity(cursor); i++) {
            const BenderKV *kv = cursor + i;
            if (!bender_checktype(kv->key, BENDER_NIL)) {
                bender_table_put(tab_cursor, kv->key, kv->value);
            }
        }
        cursor = bender_struct_proto(cursor);
    } while (recursive && cursor);
    return bender_wrap_table(tab);
}

BENDER_CORE_FN(cfun_struct_rawget,
              "(struct/rawget st key)",
              "Gets a value from a struct `st` without looking at the prototype struct. "
              "If `st` does not contain the key directly, the function will return "
              "nil without checking the prototype. Returns the value in the struct.") {
    bender_fixarity(argc, 2);
    BenderStruct st = bender_getstruct(argv, 0);
    return bender_struct_rawget(st, argv[1]);
}

/* Load the struct module */
void bender_lib_struct(BenderTable *env) {
    BenderRegExt struct_cfuns[] = {
        BENDER_CORE_REG("struct/with-proto", cfun_struct_with_proto),
        BENDER_CORE_REG("struct/getproto", cfun_struct_getproto),
        BENDER_CORE_REG("struct/proto-flatten", cfun_struct_flatten),
        BENDER_CORE_REG("struct/to-table", cfun_struct_to_table),
        BENDER_CORE_REG("struct/rawget", cfun_struct_rawget),
        BENDER_REG_END
    };
    bender_core_cfuns_ext(env, NULL, struct_cfuns);
}
