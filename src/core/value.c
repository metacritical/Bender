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
#include "util.h"
#include "state.h"
#include "gc.h"
#include "fiber.h"
#include <bender.h>
#endif

#include <math.h>

static void push_traversal_node(void *lhs, void *rhs, int32_t index2) {
    BenderTraversalNode node;
    node.self = (BenderGCObject *) lhs;
    node.other = (BenderGCObject *) rhs;
    node.index = 0;
    node.index2 = index2;
    int is_new = bender_vm.traversal_base == NULL;
    if (is_new || (bender_vm.traversal + 1 >= bender_vm.traversal_top)) {
        size_t oldsize = is_new ? 0 : (bender_vm.traversal - bender_vm.traversal_base);
        size_t newsize = 2 * oldsize + 1;
        if (newsize < 128) {
            newsize = 128;
        }
        BenderTraversalNode *tn = bender_realloc(bender_vm.traversal_base, newsize * sizeof(BenderTraversalNode));
        if (tn == NULL) {
            BENDER_OUT_OF_MEMORY;
        }
        bender_vm.traversal_base = tn;
        bender_vm.traversal_top = bender_vm.traversal_base + newsize;
        bender_vm.traversal = bender_vm.traversal_base + oldsize;
    }
    *(++bender_vm.traversal) = node;
}

/*
 * Used for travsersing structs and tuples without recursion
 * Returns:
 * 0 - next node found
 * 1 - early stop - lhs < rhs
 * 2 - no next node found
 * 3 - early stop - lhs > rhs
 */
static int traversal_next(Bender *x, Bender *y) {
    BenderTraversalNode *t = bender_vm.traversal;
    while (t && t > bender_vm.traversal_base) {
        BenderGCObject *self = t->self;
        BenderTupleHead *tself = (BenderTupleHead *)self;
        BenderStructHead *sself = (BenderStructHead *)self;
        BenderGCObject *other = t->other;
        BenderTupleHead *tother = (BenderTupleHead *)other;
        BenderStructHead *sother = (BenderStructHead *)other;
        if ((self->flags & BENDER_MEM_TYPEBITS) == BENDER_MEMORY_TUPLE) {
            /* Node is a tuple at index t->index */
            if (t->index < tself->length && t->index < tother->length) {
                int32_t index = t->index++;
                *x = tself->data[index];
                *y = tother->data[index];
                bender_vm.traversal = t;
                return 0;
            }
            if (t->index2 && tself->length != tother->length) {
                return tself->length > tother->length ? 3 : 1;
            }
        } else {
            /* Node is a struct at index t->index: if t->index2 is true, we should return the values. */
            if (t->index2) {
                t->index2 = 0;
                int32_t index = t->index++;
                *x = sself->data[index].value;
                *y = sother->data[index].value;
                bender_vm.traversal = t;
                return 0;
            }
            for (int32_t i = t->index; i < sself->capacity; i++) {
                t->index2 = 1;
                *x = sself->data[t->index].key;
                *y = sother->data[t->index].key;
                bender_vm.traversal = t;
                return 0;
            }
            /* Traverse prototype */
            BenderStruct sproto = sself->proto;
            BenderStruct oproto = sother->proto;
            if (sproto && !oproto) return 3;
            if (!sproto && oproto) return 1;
            if (oproto && sproto) {
                *x = bender_wrap_struct(sproto);
                *y = bender_wrap_struct(oproto);
                bender_vm.traversal = t - 1;
                return 0;
            }
        }
        t--;
    }
    bender_vm.traversal = t;
    return 2;
}

/*
 * Define a number of functions that can be used internally on ANY Bender.
 */

Bender bender_next(Bender ds, Bender key) {
    return bender_next_impl(ds, key, 0);
}

Bender bender_next_impl(Bender ds, Bender key, int is_interpreter) {
    BenderType t = bender_type(ds);
    switch (t) {
        default:
            bender_panicf("expected iterable type, got %v", ds);
        case BENDER_TABLE:
        case BENDER_STRUCT: {
            const BenderKV *start;
            int32_t cap;
            if (t == BENDER_TABLE) {
                BenderTable *tab = bender_unwrap_table(ds);
                cap = tab->capacity;
                start = tab->data;
            } else {
                BenderStruct st = bender_unwrap_struct(ds);
                cap = bender_struct_capacity(st);
                start = st;
            }
            const BenderKV *end = start + cap;
            const BenderKV *kv = bender_checktype(key, BENDER_NIL)
                                ? start
                                : bender_dict_find(start, cap, key) + 1;
            while (kv < end) {
                if (!bender_checktype(kv->key, BENDER_NIL)) return kv->key;
                kv++;
            }
            break;
        }
        case BENDER_STRING:
        case BENDER_KEYWORD:
        case BENDER_SYMBOL:
        case BENDER_BUFFER:
        case BENDER_ARRAY:
        case BENDER_TUPLE: {
            int32_t i;
            if (bender_checktype(key, BENDER_NIL)) {
                i = 0;
            } else if (bender_checkint(key)) {
                i = bender_unwrap_integer(key) + 1;
            } else {
                break;
            }
            int32_t len;
            if (t == BENDER_BUFFER) {
                len = bender_unwrap_buffer(ds)->count;
            } else if (t == BENDER_ARRAY) {
                len = bender_unwrap_array(ds)->count;
            } else if (t == BENDER_TUPLE) {
                len = bender_tuple_length(bender_unwrap_tuple(ds));
            } else {
                len = bender_string_length(bender_unwrap_string(ds));
            }
            if (i < len && i >= 0) {
                return bender_wrap_integer(i);
            }
            break;
        }
        case BENDER_ABSTRACT: {
            BenderAbstract abst = bender_unwrap_abstract(ds);
            const BenderAbstractType *at = bender_abstract_type(abst);
            if (NULL == at->next) break;
            return at->next(abst, key);
        }
        case BENDER_FIBER: {
            BenderFiber *child = bender_unwrap_fiber(ds);
            Bender retreg;
            BenderFiberStatus status = bender_fiber_status(child);
            if (status == BENDER_STATUS_ALIVE ||
                    status == BENDER_STATUS_DEAD ||
                    status == BENDER_STATUS_ERROR ||
                    status == BENDER_STATUS_USER0 ||
                    status == BENDER_STATUS_USER1 ||
                    status == BENDER_STATUS_USER2 ||
                    status == BENDER_STATUS_USER3 ||
                    status == BENDER_STATUS_USER4) {
                return bender_wrap_nil();
            }
            bender_vm.fiber->child = child;
            BenderSignal sig;
            /* M4b-2: quantum-INTERRUPT is transparent here (iteration
             * must complete) — resume again. Quiesce still unwinds. */
            do {
                sig = bender_continue(child, bender_wrap_nil(), &retreg);
            } while (sig == BENDER_SIGNAL_INTERRUPT && bender_vm.quantum &&
                    !bender_atomic_load_relaxed(&bender_vm.auto_suspend));
            if (sig != BENDER_SIGNAL_OK && !(child->flags & (1 << sig))) {
                if (is_interpreter) {
                    bender_signalv(sig, retreg);
                } else {
                    bender_vm.fiber->child = NULL;
                    bender_panicv(retreg);
                }
            }
            bender_vm.fiber->child = NULL;
            if (sig == BENDER_SIGNAL_OK ||
                    sig == BENDER_SIGNAL_ERROR ||
                    sig == BENDER_SIGNAL_USER0 ||
                    sig == BENDER_SIGNAL_USER1 ||
                    sig == BENDER_SIGNAL_USER2 ||
                    sig == BENDER_SIGNAL_USER3 ||
                    sig == BENDER_SIGNAL_USER4) {
                /* Fiber cannot be resumed, so discard last value. */
                return bender_wrap_nil();
            } else {
                return bender_wrap_integer(0);
            }
        }
    }
    return bender_wrap_nil();
}

/* Compare two abstract values */
static int bender_compare_abstract(BenderAbstract xx, BenderAbstract yy) {
    if (xx == yy) return 0;
    const BenderAbstractType *xt = bender_abstract_type(xx);
    const BenderAbstractType *yt = bender_abstract_type(yy);
    if (xt != yt) {
        return xt > yt ? 1 : -1;
    }
    if (xt->compare == NULL) {
        return xx > yy ? 1 : -1;
    }
    return xt->compare(xx, yy);
}

/* Compare two values of the same type that are not tuples or structs */
static int bender_equals_leaf(Bender x, Bender y) {
    switch (bender_type(x)) {
        case BENDER_NUMBER:
            return bender_unwrap_number(x) == bender_unwrap_number(y);
        case BENDER_STRING:
            return bender_string_equal(bender_unwrap_string(x), bender_unwrap_string(y));
        case BENDER_ABSTRACT:
            return !bender_compare_abstract(bender_unwrap_abstract(x), bender_unwrap_abstract(y));
        default:
            return bender_bitwise_same(x, y);
    }
}

static int bender_equals_containers(Bender x, Bender y) {
    bender_vm.traversal = bender_vm.traversal_base;
    do {
        if (bender_type(x) != bender_type(y)) return 0;
        switch (bender_type(x)) {
            default:
                if (!bender_equals_leaf(x, y)) return 0;
                break;
            case BENDER_TUPLE: {
                const Bender *t1 = bender_unwrap_tuple(x);
                const Bender *t2 = bender_unwrap_tuple(y);
                if (t1 == t2) break;
                if (BENDER_TUPLE_FLAG_BRACKETCTOR & (bender_tuple_flag(t1) ^ bender_tuple_flag(t2))) return 0;
                if (bender_tuple_hash(t1) != bender_tuple_hash(t2)) return 0;
                if (bender_tuple_length(t1) != bender_tuple_length(t2)) return 0;
                push_traversal_node(bender_tuple_head(t1), bender_tuple_head(t2), 0);
                break;
            }
            case BENDER_STRUCT: {
                const BenderKV *s1 = bender_unwrap_struct(x);
                const BenderKV *s2 = bender_unwrap_struct(y);
                if (s1 == s2) break;
                if (bender_struct_hash(s1) != bender_struct_hash(s2)) return 0;
                if (bender_struct_length(s1) != bender_struct_length(s2)) return 0;
                if (bender_struct_proto(s1) && !bender_struct_proto(s2)) return 0;
                if (!bender_struct_proto(s1) && bender_struct_proto(s2)) return 0;
                push_traversal_node(bender_struct_head(s1), bender_struct_head(s2), 0);
                break;
            }
        }
    } while (!traversal_next(&x, &y));
    return 1;
}

int bender_equals(Bender x, Bender y) {
    if (bender_type(x) != bender_type(y)) return 0;
    if (bender_checktypes(x, BENDER_TFLAG_TUPLE | BENDER_TFLAG_STRUCT)) return bender_equals_containers(x, y);
    return bender_equals_leaf(x, y);
}

static uint64_t murmur64(uint64_t h) {
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdUL;
    h ^= h >> 33;
    h *= 0xc4ceb9fe1a85ec53UL;
    h ^= h >> 33;
    return h;
}

/* Computes a hash value for a function */
int32_t bender_hash(Bender x) {
    int32_t hash = 0;
    switch (bender_type(x)) {
        case BENDER_NIL:
            hash = 0;
            break;
        case BENDER_BOOLEAN:
            hash = bender_unwrap_boolean(x);
            break;
        case BENDER_STRING:
        case BENDER_SYMBOL:
        case BENDER_KEYWORD:
            hash = bender_string_hash(bender_unwrap_string(x));
            break;
        case BENDER_TUPLE:
            hash = bender_tuple_hash(bender_unwrap_tuple(x));
            uint32_t inc = (bender_tuple_flag(bender_unwrap_tuple(x)) & BENDER_TUPLE_FLAG_BRACKETCTOR) ? 1 : 0;
            hash = (int32_t)((uint32_t)hash + inc); /* avoid overflow undefined behavior */
            break;
        case BENDER_STRUCT:
            hash = bender_struct_hash(bender_unwrap_struct(x));
            break;
        case BENDER_NUMBER: {
            union {
                double d;
                uint64_t u;
            } as;
            as.d = bender_unwrap_number(x);
            as.d += 0.0; /* normalize negative 0 */
            as.u = murmur64(as.u);
            uint32_t hi = (uint32_t)(as.u >> 32);
            hash = (int32_t)hi;
            break;
        }
        case BENDER_ABSTRACT: {
            BenderAbstract xx = bender_unwrap_abstract(x);
            const BenderAbstractType *at = bender_abstract_type(xx);
            if (at->hash != NULL) {
                hash = at->hash(xx, bender_abstract_size(xx));
                break;
            }
        }
        /* fallthrough */
        default:
            if (sizeof(double) == sizeof(void *)) {
                /* Assuming 8 byte pointer (8 byte aligned) */
                uint64_t i = murmur64(bender_u64(x));
                hash = (int32_t)(i >> 32);
            } else {
                /* Assuming 4 byte pointer (or smaller) */
                uintptr_t diff = (uintptr_t) bender_unwrap_pointer(x);
                uint32_t hilo = (uint32_t) diff * 2654435769u;
                hash = (int32_t)((hilo << 16) | (hilo >> 16));
            }
            break;
    }
    return hash;
}

/* Compares x to y. If they are equal returns 0. If x is less, returns -1.
 * If y is less, returns 1. All types are comparable
 * and should have strict ordering, excepts NaNs. */
int bender_compare(Bender x, Bender y) {
    bender_vm.traversal = bender_vm.traversal_base;
    int status;
    do {
        BenderType tx = bender_type(x);
        BenderType ty = bender_type(y);
        if (tx != ty) return tx < ty ? -1 : 1;
        switch (tx) {
            case BENDER_NIL:
                break;
            case BENDER_BOOLEAN: {
                int diff = bender_unwrap_boolean(x) - bender_unwrap_boolean(y);
                if (diff) return diff;
                break;
            }
            case BENDER_NUMBER: {
                double xx = bender_unwrap_number(x);
                double yy = bender_unwrap_number(y);
                if (xx == yy) {
                    break;
                } else {
                    return (xx < yy) ? -1 : 1;
                }
            }
            case BENDER_STRING:
            case BENDER_SYMBOL:
            case BENDER_KEYWORD: {
                int diff = bender_string_compare(bender_unwrap_string(x), bender_unwrap_string(y));
                if (diff) return diff;
                break;
            }
            case BENDER_ABSTRACT: {
                int diff = bender_compare_abstract(bender_unwrap_abstract(x), bender_unwrap_abstract(y));
                if (diff) return diff;
                break;
            }
            default: {
                if (bender_unwrap_pointer(x) == bender_unwrap_pointer(y)) {
                    break;
                } else {
                    return bender_unwrap_pointer(x) > bender_unwrap_pointer(y) ? 1 : -1;
                }
            }
            case BENDER_TUPLE: {
                const Bender *lhs = bender_unwrap_tuple(x);
                const Bender *rhs = bender_unwrap_tuple(y);
                if (BENDER_TUPLE_FLAG_BRACKETCTOR & (bender_tuple_flag(lhs) ^ bender_tuple_flag(rhs))) {
                    return (bender_tuple_flag(lhs) & BENDER_TUPLE_FLAG_BRACKETCTOR) ? 1 : -1;
                }
                push_traversal_node(bender_tuple_head(lhs), bender_tuple_head(rhs), 1);
                break;
            }
            case BENDER_STRUCT: {
                const BenderKV *lhs = bender_unwrap_struct(x);
                const BenderKV *rhs = bender_unwrap_struct(y);
                int32_t llen = bender_struct_capacity(lhs);
                int32_t rlen = bender_struct_capacity(rhs);
                int32_t lhash = bender_struct_hash(lhs);
                int32_t rhash = bender_struct_hash(rhs);
                if (llen < rlen) return -1;
                if (llen > rlen) return 1;
                if (lhash < rhash) return -1;
                if (lhash > rhash) return 1;
                push_traversal_node(bender_struct_head(lhs), bender_struct_head(rhs), 0);
                break;
            }
        }
    } while (!(status = traversal_next(&x, &y)));
    return status - 2;
}

static int32_t getter_checkint(BenderType type, Bender key, int32_t max) {
    if (!bender_checkint(key)) goto bad;
    int32_t ret = bender_unwrap_integer(key);
    if (ret < 0) goto bad;
    if (ret >= max) goto bad;
    return ret;
bad:
    bender_panicf("expected integer key for %s in range [0, %d), got %v", bender_type_names[type], max, key);
}

/* Gets a value and returns. Can panic. */
Bender bender_in(Bender ds, Bender key) {
    Bender value;
    BenderType type = bender_type(ds);
    switch (type) {
        default:
            bender_panicf("expected %T, got %v", BENDER_TFLAG_LENGTHABLE, ds);
            break;
        case BENDER_STRUCT:
            value = bender_struct_get(bender_unwrap_struct(ds), key);
            break;
        case BENDER_TABLE:
            value = bender_table_get(bender_unwrap_table(ds), key);
            break;
        case BENDER_ARRAY: {
            BenderArray *array = bender_unwrap_array(ds);
            int32_t index = getter_checkint(type, key, array->count);
            value = array->data[index];
            break;
        }
        case BENDER_TUPLE: {
            const Bender *tuple = bender_unwrap_tuple(ds);
            int32_t len = bender_tuple_length(tuple);
            value = tuple[getter_checkint(type, key, len)];
            break;
        }
        case BENDER_BUFFER: {
            BenderBuffer *buffer = bender_unwrap_buffer(ds);
            int32_t index = getter_checkint(type, key, buffer->count);
            value = bender_wrap_integer(buffer->data[index]);
            break;
        }
        case BENDER_STRING:
        case BENDER_SYMBOL:
        case BENDER_KEYWORD: {
            const uint8_t *str = bender_unwrap_string(ds);
            int32_t index = getter_checkint(type, key, bender_string_length(str));
            value = bender_wrap_integer(str[index]);
            break;
        }
        case BENDER_ABSTRACT: {
            BenderAbstractType *type = (BenderAbstractType *)bender_abstract_type(bender_unwrap_abstract(ds));
            if (type->get) {
                if (!(type->get)(bender_unwrap_abstract(ds), key, &value))
                    bender_panicf("key %v not found in %v ", key, ds);
            } else {
                bender_panicf("no getter for %v", ds);
            }
            break;
        }
        case BENDER_FIBER: {
            /* Bit of a hack to allow iterating over fibers. */
            if (bender_equals(key, bender_wrap_integer(0))) {
                return bender_unwrap_fiber(ds)->last_value;
            } else {
                bender_panicf("expected key 0, got %v", key);
            }
        }
    }
    return value;
}

Bender bender_get(Bender ds, Bender key) {
    BenderType t = bender_type(ds);
    switch (t) {
        default:
            return bender_wrap_nil();
        case BENDER_STRING:
        case BENDER_SYMBOL:
        case BENDER_KEYWORD: {
            if (!bender_checkint(key)) return bender_wrap_nil();
            int32_t index = bender_unwrap_integer(key);
            if (index < 0) return bender_wrap_nil();
            const uint8_t *str = bender_unwrap_string(ds);
            if (index >= bender_string_length(str)) return bender_wrap_nil();
            return bender_wrap_integer(str[index]);
        }
        case BENDER_ABSTRACT: {
            Bender value;
            void *abst = bender_unwrap_abstract(ds);
            BenderAbstractType *type = (BenderAbstractType *)bender_abstract_type(abst);
            if (!type->get) return bender_wrap_nil();
            if ((type->get)(abst, key, &value))
                return value;
            return bender_wrap_nil();
        }
        case BENDER_ARRAY:
        case BENDER_TUPLE:
        case BENDER_BUFFER: {
            if (!bender_checkint(key)) return bender_wrap_nil();
            int32_t index = bender_unwrap_integer(key);
            if (index < 0) return bender_wrap_nil();
            if (t == BENDER_ARRAY) {
                BenderArray *a = bender_unwrap_array(ds);
                if (index >= a->count) return bender_wrap_nil();
                return a->data[index];
            } else if (t == BENDER_BUFFER) {
                BenderBuffer *b = bender_unwrap_buffer(ds);
                if (index >= b->count) return bender_wrap_nil();
                return bender_wrap_integer(b->data[index]);
            } else {
                const Bender *t = bender_unwrap_tuple(ds);
                if (index >= bender_tuple_length(t)) return bender_wrap_nil();
                return t[index];
            }
        }
        case BENDER_TABLE: {
            return bender_table_get(bender_unwrap_table(ds), key);
        }
        case BENDER_STRUCT: {
            const BenderKV *st = bender_unwrap_struct(ds);
            return bender_struct_get(st, key);
        }
        case BENDER_FIBER: {
            /* Bit of a hack to allow iterating over fibers. */
            if (bender_equals(key, bender_wrap_integer(0))) {
                return bender_unwrap_fiber(ds)->last_value;
            } else {
                return bender_wrap_nil();
            }
        }
    }
}

Bender bender_getindex(Bender ds, int32_t index) {
    Bender value;
    if (index < 0) bender_panic("expected non-negative index");
    switch (bender_type(ds)) {
        default:
            bender_panicf("expected %T, got %v", BENDER_TFLAG_LENGTHABLE, ds);
            break;
        case BENDER_STRING:
        case BENDER_SYMBOL:
        case BENDER_KEYWORD:
            if (index >= bender_string_length(bender_unwrap_string(ds))) {
                value = bender_wrap_nil();
            } else {
                value = bender_wrap_integer(bender_unwrap_string(ds)[index]);
            }
            break;
        case BENDER_ARRAY:
            if (index >= bender_unwrap_array(ds)->count) {
                value = bender_wrap_nil();
            } else {
                value = bender_unwrap_array(ds)->data[index];
            }
            break;
        case BENDER_BUFFER:
            if (index >= bender_unwrap_buffer(ds)->count) {
                value = bender_wrap_nil();
            } else {
                value = bender_wrap_integer(bender_unwrap_buffer(ds)->data[index]);
            }
            break;
        case BENDER_TUPLE:
            if (index >= bender_tuple_length(bender_unwrap_tuple(ds))) {
                value = bender_wrap_nil();
            } else {
                value = bender_unwrap_tuple(ds)[index];
            }
            break;
        case BENDER_TABLE:
            value = bender_table_get(bender_unwrap_table(ds), bender_wrap_integer(index));
            break;
        case BENDER_STRUCT:
            value = bender_struct_get(bender_unwrap_struct(ds), bender_wrap_integer(index));
            break;
        case BENDER_ABSTRACT: {
            BenderAbstractType *type = (BenderAbstractType *)bender_abstract_type(bender_unwrap_abstract(ds));
            if (type->get) {
                if (!(type->get)(bender_unwrap_abstract(ds), bender_wrap_integer(index), &value))
                    value = bender_wrap_nil();
            } else {
                bender_panicf("no getter for %v", ds);
            }
            break;
        }
        case BENDER_FIBER: {
            if (index == 0) {
                value = bender_unwrap_fiber(ds)->last_value;
            } else {
                value = bender_wrap_nil();
            }
            break;
        }
    }
    return value;
}

int32_t bender_length(Bender x) {
    switch (bender_type(x)) {
        default:
            bender_panicf("expected %T, got %v", BENDER_TFLAG_LENGTHABLE, x);
        case BENDER_STRING:
        case BENDER_SYMBOL:
        case BENDER_KEYWORD:
            return bender_string_length(bender_unwrap_string(x));
        case BENDER_ARRAY:
            return bender_unwrap_array(x)->count;
        case BENDER_BUFFER:
            return bender_unwrap_buffer(x)->count;
        case BENDER_TUPLE:
            return bender_tuple_length(bender_unwrap_tuple(x));
        case BENDER_STRUCT:
            return bender_struct_length(bender_unwrap_struct(x));
        case BENDER_TABLE:
            return bender_unwrap_table(x)->count;
        case BENDER_ABSTRACT: {
            void *abst = bender_unwrap_abstract(x);
            const BenderAbstractType *type = bender_abstract_type(abst);
            if (type->length != NULL) {
                size_t len = type->length(abst, bender_abstract_size(abst));
                if (len > INT32_MAX) {
                    bender_panicf("invalid integer length %u", len);
                }
                return (int32_t)(len);
            }
            Bender argv[1] = { x };
            Bender len = bender_mcall("length", 1, argv);
            if (!bender_checkint(len))
                bender_panicf("invalid integer length %v", len);
            return bender_unwrap_integer(len);
        }
    }
}

Bender bender_lengthv(Bender x) {
    switch (bender_type(x)) {
        default:
            bender_panicf("expected %T, got %v", BENDER_TFLAG_LENGTHABLE, x);
        case BENDER_STRING:
        case BENDER_SYMBOL:
        case BENDER_KEYWORD:
            return bender_wrap_integer(bender_string_length(bender_unwrap_string(x)));
        case BENDER_ARRAY:
            return bender_wrap_integer(bender_unwrap_array(x)->count);
        case BENDER_BUFFER:
            return bender_wrap_integer(bender_unwrap_buffer(x)->count);
        case BENDER_TUPLE:
            return bender_wrap_integer(bender_tuple_length(bender_unwrap_tuple(x)));
        case BENDER_STRUCT:
            return bender_wrap_integer(bender_struct_length(bender_unwrap_struct(x)));
        case BENDER_TABLE:
            return bender_wrap_integer(bender_unwrap_table(x)->count);
        case BENDER_ABSTRACT: {
            void *abst = bender_unwrap_abstract(x);
            const BenderAbstractType *type = bender_abstract_type(abst);
            if (type->length != NULL) {
                size_t len = type->length(abst, bender_abstract_size(abst));
                /* If len is always less then double, we can never overflow */
#ifdef BENDER_32
                return bender_wrap_number(len);
#else
                if (len < (size_t) BENDER_INTMAX_INT64) {
                    return bender_wrap_number((double) len);
                } else {
                    bender_panicf("integer length %u too large", len);
                }
#endif
            }
            Bender argv[1] = { x };
            return bender_mcall("length", 1, argv);
        }
    }
}

void bender_putindex(Bender ds, int32_t index, Bender value) {
    switch (bender_type(ds)) {
        default:
            bender_panicf("expected %T, got %v",
                         BENDER_TFLAG_ARRAY | BENDER_TFLAG_BUFFER | BENDER_TFLAG_TABLE, ds);
        case BENDER_ARRAY: {
            BenderArray *array = bender_unwrap_array(ds);
            if (index >= array->count) {
                bender_array_ensure(array, index + 1, 2);
                for (int32_t i = array->count; i < index + 1; i++) {
                    array->data[i] = bender_wrap_nil();
                }
                array->count = index + 1;
            }
            array->data[index] = value;
            /* Generational barrier (Phase 2b-iii): old array, new edge. */
            bender_gc_barrier(array, value);
            break;
        }
        case BENDER_BUFFER: {
            BenderBuffer *buffer = bender_unwrap_buffer(ds);
            if (!bender_checkint(value))
                bender_panicf("can only put integers in buffers, got %v", value);
            if (index >= buffer->count) {
                bender_buffer_ensure(buffer, index + 1, 2);
                memset(buffer->data + buffer->count, 0, index + 1 - buffer->count);
                buffer->count = index + 1;
            }
            buffer->data[index] = (uint8_t)(bender_unwrap_integer(value) & 0xFF);
            break;
        }
        case BENDER_TABLE: {
            BenderTable *table = bender_unwrap_table(ds);
            bender_table_put(table, bender_wrap_integer(index), value);
            break;
        }
        case BENDER_ABSTRACT: {
            BenderAbstractType *type = (BenderAbstractType *)bender_abstract_type(bender_unwrap_abstract(ds));
            if (type->put) {
                (type->put)(bender_unwrap_abstract(ds), bender_wrap_integer(index), value);
            } else {
                bender_panicf("no setter for %v ", ds);
            }
            break;
        }
    }
}

void bender_put(Bender ds, Bender key, Bender value) {
    BenderType type = bender_type(ds);
    switch (type) {
        default:
            bender_panicf("expected %T, got %v",
                         BENDER_TFLAG_ARRAY | BENDER_TFLAG_BUFFER | BENDER_TFLAG_TABLE, ds);
        case BENDER_ARRAY: {
            BenderArray *array = bender_unwrap_array(ds);
            int32_t index = getter_checkint(type, key, INT32_MAX - 1);
            if (index >= array->count) {
                bender_array_ensure(array, index + 1, 2);
                for (int32_t i = array->count; i < index + 1; i++) {
                    array->data[i] = bender_wrap_nil();
                }
                array->count = index + 1;
            }
            array->data[index] = value;
            /* Generational barrier (Phase 2b-iii): old array, new edge. */
            bender_gc_barrier(array, value);
            break;
        }
        case BENDER_BUFFER: {
            BenderBuffer *buffer = bender_unwrap_buffer(ds);
            int32_t index = getter_checkint(type, key, INT32_MAX - 1);
            if (!bender_checkint(value))
                bender_panicf("can only put integers in buffers, got %v", value);
            if (index >= buffer->count) {
                bender_buffer_ensure(buffer, index + 1, 2);
                memset(buffer->data + buffer->count, 0, index + 1 - buffer->count);
                buffer->count = index + 1;
            }
            buffer->data[index] = (uint8_t)(bender_unwrap_integer(value) & 0xFF);
            break;
        }
        case BENDER_TABLE:
            bender_table_put(bender_unwrap_table(ds), key, value);
            break;
        case BENDER_ABSTRACT: {
            BenderAbstractType *type = (BenderAbstractType *)bender_abstract_type(bender_unwrap_abstract(ds));
            if (type->put) {
                (type->put)(bender_unwrap_abstract(ds), key, value);
            } else {
                bender_panicf("no setter for %v ", ds);
            }
            break;
        }
    }
}

/* Native-executor buffer write fast path. The in-range integer subset of
 * bender_put cannot grow or reallocate its backing bytes. Obtain data from the
 * current BenderBuffer on every access; never retain it across a call, poll, or
 * a VM fallback. Refuse BEFORE mutation for invalid keys/values or growth, so
 * the original JOP_PUT produces precisely its own error or zero-fill. */
static int native_buffer_put_fast_impl(Bender ds, Bender key, Bender value
#if defined(BENDER_NATIVE_COMMIT_PROBE) && BENDER_NATIVE_COMMIT_PROBE
        , BenderNativeCommitHook commit, void *context
#endif
) {
    BenderBuffer *buffer;
    int32_t index;
    if (!bender_checktype(ds, BENDER_BUFFER) || !bender_checkint(key) ||
            !bender_checkint(value)) return 0;
    index = bender_unwrap_integer(key);
    if (index < 0) return 0;
    buffer = bender_unwrap_buffer(ds);
    if (index >= buffer->count) return 0;
    buffer->data[index] = (uint8_t)(bender_unwrap_integer(value) & 0xFF);
#if defined(BENDER_NATIVE_COMMIT_PROBE) && BENDER_NATIVE_COMMIT_PROBE
    if (commit) commit(context);
#endif
    return 1;
}

BENDER_API int bender_native_buffer_put_fast(Bender ds, Bender key, Bender value) {
#if defined(BENDER_NATIVE_COMMIT_PROBE) && BENDER_NATIVE_COMMIT_PROBE
    return native_buffer_put_fast_impl(ds, key, value, NULL, NULL);
#else
    return native_buffer_put_fast_impl(ds, key, value);
#endif
}
#if defined(BENDER_NATIVE_COMMIT_PROBE) && BENDER_NATIVE_COMMIT_PROBE
BENDER_API int bender_native_buffer_put_fast_probe(Bender ds, Bender key, Bender value,
        BenderNativeCommitHook commit, void *context) {
    return native_buffer_put_fast_impl(ds, key, value, commit, context);
}
#endif

/* Native-executor array write fast path. Only an in-range non-negative
 * integer index is handled: bender_put's store plus generational barrier.
 * Growth (which allocates and fills intermediates), other receivers and
 * abstract setters stay in the VM at the original PC. */
static int native_array_put_fast_impl(Bender ds, Bender key, Bender value
#if defined(BENDER_NATIVE_COMMIT_PROBE) && BENDER_NATIVE_COMMIT_PROBE
        , BenderNativeCommitHook commit, void *context
#endif
) {
    BenderArray *array;
    int32_t index;
    if (!bender_checktype(ds, BENDER_ARRAY)) return 0;
    if (!bender_checkint(key)) return 0;
    index = bender_unwrap_integer(key);
    if (index < 0) return 0;
    array = bender_unwrap_array(ds);
    if (index >= array->count) return 0;
    array->data[index] = value;
#if defined(BENDER_NATIVE_COMMIT_PROBE) && BENDER_NATIVE_COMMIT_PROBE
    if (commit) commit(context);
#endif
    bender_gc_barrier(array, value);
    return 1;
}

BENDER_API int bender_native_array_put_fast(Bender ds, Bender key, Bender value) {
#if defined(BENDER_NATIVE_COMMIT_PROBE) && BENDER_NATIVE_COMMIT_PROBE
    return native_array_put_fast_impl(ds, key, value, NULL, NULL);
#else
    return native_array_put_fast_impl(ds, key, value);
#endif
}
#if defined(BENDER_NATIVE_COMMIT_PROBE) && BENDER_NATIVE_COMMIT_PROBE
BENDER_API int bender_native_array_put_fast_probe(Bender ds, Bender key, Bender value,
        BenderNativeCommitHook commit, void *context) {
    return native_array_put_fast_impl(ds, key, value, commit, context);
}
#endif
