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
#include "state.h"
#include "vector.h"
#include "gc.h"
#include "fiber.h"
#include "util.h"
#endif

typedef struct {
    BenderBuffer *buf;
    BenderTable seen;
    BenderTable *rreg;
    BenderFuncEnv **seen_envs;
    BenderFuncDef **seen_defs;
    int32_t nextid;
    int maybe_cycles;
} MarshalState;

/* Lead bytes in marshaling protocol */
enum {
    LB_REAL = 200,
    LB_NIL, /* 201 */
    LB_FALSE, /* 202 */
    LB_TRUE,  /* 203 */
    LB_FIBER, /* 204 */
    LB_INTEGER, /* 205 */
    LB_STRING, /* 206 */
    LB_SYMBOL, /* 207 */
    LB_KEYWORD, /* 208 */
    LB_ARRAY, /* 209 */
    LB_TUPLE, /* 210 */
    LB_TABLE, /* 211 */
    LB_TABLE_PROTO, /* 212 */
    LB_STRUCT, /* 213 */
    LB_BUFFER, /* 214 */
    LB_FUNCTION, /* 215 */
    LB_REGISTRY, /* 216 */
    LB_ABSTRACT, /* 217 */
    LB_REFERENCE, /* 218 */
    LB_FUNCENV_REF, /* 219 */
    LB_FUNCDEF_REF, /* 220 */
    LB_UNSAFE_CFUNCTION, /* 221 */
    LB_UNSAFE_POINTER, /* 222 */
    LB_STRUCT_PROTO, /* 223 */
#ifdef BENDER_EV
    LB_THREADED_ABSTRACT, /* 224 */
    LB_POINTER_BUFFER, /* 225 */
#endif
    LB_TABLE_WEAKK, /* 226 */
    LB_TABLE_WEAKV, /* 227 */
    LB_TABLE_WEAKKV, /* 228 */
    LB_TABLE_WEAKK_PROTO, /* 229 */
    LB_TABLE_WEAKV_PROTO, /* 230 */
    LB_TABLE_WEAKKV_PROTO, /* 231 */
    LB_ARRAY_WEAK, /* 232 */
} LeadBytes;

/* Helper to look inside an entry in an environment */
static Bender entry_getval(Bender env_entry) {
    if (bender_checktype(env_entry, BENDER_TABLE)) {
        BenderTable *entry = bender_unwrap_table(env_entry);
        Bender checkval = bender_table_get(entry, bender_ckeywordv("value"));
        if (bender_checktype(checkval, BENDER_NIL)) {
            checkval = bender_table_get(entry, bender_ckeywordv("ref"));
        }
        return checkval;
    } else if (bender_checktype(env_entry, BENDER_STRUCT)) {
        const BenderKV *entry = bender_unwrap_struct(env_entry);
        Bender checkval = bender_struct_get(entry, bender_ckeywordv("value"));
        if (bender_checktype(checkval, BENDER_NIL)) {
            checkval = bender_struct_get(entry, bender_ckeywordv("ref"));
        }
        return checkval;
    } else {
        return bender_wrap_nil();
    }
}

/* Merge values from an environment into an existing lookup table. */
void bender_env_lookup_into(BenderTable *renv, BenderTable *env, const char *prefix, int recurse) {
    while (env) {
        for (int32_t i = 0; i < env->capacity; i++) {
            if (bender_checktype(env->data[i].key, BENDER_SYMBOL)) {
                if (prefix) {
                    int32_t prelen = (int32_t) strlen(prefix);
                    const uint8_t *oldsym = bender_unwrap_symbol(env->data[i].key);
                    int32_t oldlen = bender_string_length(oldsym);
                    uint8_t *symbuf = bender_smalloc(prelen + oldlen);
                    safe_memcpy(symbuf, prefix, prelen);
                    safe_memcpy(symbuf + prelen, oldsym, oldlen);
                    Bender s = bender_symbolv(symbuf, prelen + oldlen);
                    bender_sfree(symbuf);
                    bender_table_put(renv, s, entry_getval(env->data[i].value));
                } else {
                    bender_table_put(renv,
                                    env->data[i].key,
                                    entry_getval(env->data[i].value));
                }
            }
        }
        env = recurse ? env->proto : NULL;
    }
}

/* Make a forward lookup table from an environment (for unmarshaling) */
BenderTable *bender_env_lookup(BenderTable *env) {
    BenderTable *renv = bender_table(env->count);
    bender_env_lookup_into(renv, env, NULL, 1);
    return renv;
}

/* Marshal an integer onto the buffer */
static void pushint(MarshalState *st, int32_t x) {
    if (x >= 0 && x < 128) {
        bender_buffer_push_u8(st->buf, x);
    } else if (x <= 8191 && x >= -8192) {
        uint8_t intbuf[2];
        intbuf[0] = ((x >> 8) & 0x3F) | 0x80;
        intbuf[1] = x & 0xFF;
        bender_buffer_push_bytes(st->buf, intbuf, 2);
    } else {
        uint8_t intbuf[5];
        intbuf[0] = LB_INTEGER;
        intbuf[1] = (x >> 24) & 0xFF;
        intbuf[2] = (x >> 16) & 0xFF;
        intbuf[3] = (x >> 8) & 0xFF;
        intbuf[4] = x & 0xFF;
        bender_buffer_push_bytes(st->buf, intbuf, 5);
    }
}

static void pushbyte(MarshalState *st, uint8_t b) {
    bender_buffer_push_u8(st->buf, b);
}

static void pushbytes(MarshalState *st, const uint8_t *bytes, int32_t len) {
    bender_buffer_push_bytes(st->buf, bytes, len);
}

static void pushpointer(MarshalState *st, const void *ptr) {
    bender_buffer_push_bytes(st->buf, (const uint8_t *) &ptr, sizeof(ptr));
}

/* Marshal a size_t onto the buffer */
static void push64(MarshalState *st, uint64_t x) {
    if (x <= 0xF0) {
        /* Single byte */
        pushbyte(st, (uint8_t) x);
    } else {
        /* Multibyte, little endian */
        uint8_t bytes[9];
        int nbytes = 0;
        while (x) {
            bytes[++nbytes] = x & 0xFF;
            x >>= 8;
        }
        bytes[0] = 0xF0 + nbytes;
        pushbytes(st, bytes, nbytes + 1);
    }
}

/* Forward declaration to enable mutual recursion. */
static void marshal_one(MarshalState *st, Bender x, int flags);
static void marshal_one_fiber(MarshalState *st, BenderFiber *fiber, int flags);
static void marshal_one_def(MarshalState *st, BenderFuncDef *def, int flags);
static void marshal_one_env(MarshalState *st, BenderFuncEnv *env, int flags);

/* Prevent stack overflows */
#define MARSH_STACKCHECK if ((flags & 0xFFFF) > BENDER_RECURSION_GUARD) bender_panic("stack overflow")

/* Quick check if a fiber cannot be marshalled. This is will
 * have no false positives, but may have false negatives. */
static int fiber_cannot_be_marshalled(BenderFiber *fiber) {
    if (bender_fiber_status(fiber) == BENDER_STATUS_ALIVE) return 1;
    int32_t i = fiber->frame;
    while (i > 0) {
        BenderStackFrame *frame = (BenderStackFrame *)(fiber->data + i - BENDER_FRAME_SIZE);
        if (!frame->func) return 1; /* has cfunction on stack */
        i = frame->prevframe;
    }
    return 0;
}

/* Marshal a function env */
static void marshal_one_env(MarshalState *st, BenderFuncEnv *env, int flags) {
    MARSH_STACKCHECK;
    for (int32_t i = 0; i < bender_v_count(st->seen_envs); i++) {
        if (st->seen_envs[i] == env) {
            pushbyte(st, LB_FUNCENV_REF);
            pushint(st, i);
            return;
        }
    }
    bender_env_valid(env);
    bender_v_push(st->seen_envs, env);

    /* Special case for early detachment */
    if (env->offset > 0 && fiber_cannot_be_marshalled(env->as.fiber)) {
        pushint(st, 0);
        pushint(st, env->length);
        Bender *values = env->as.fiber->data + env->offset;
        uint32_t *bitset = bender_stack_frame(values)->func->def->closure_bitset;
        for (int32_t i = 0; i < env->length; i++) {
            if (1 & (bitset[i >> 5] >> (i & 0x1F))) {
                marshal_one(st, values[i], flags + 1);
            } else {
                pushbyte(st, LB_NIL);
            }
        }
    } else {
        bender_env_maybe_detach(env);
        pushint(st, env->offset);
        pushint(st, env->length);
        if (env->offset > 0) {
            /* On stack variant */
            marshal_one(st, bender_wrap_fiber(env->as.fiber), flags + 1);
        } else {
            /* Off stack variant */
            for (int32_t i = 0; i < env->length; i++)
                marshal_one(st, env->as.values[i], flags + 1);
        }
    }
}

/* Marshal a sequence of u32s */
static void bender_marshal_u32s(MarshalState *st, const uint32_t *u32s, int32_t n) {
    for (int32_t i = 0; i < n; i++) {
        pushbyte(st, u32s[i] & 0xFF);
        pushbyte(st, (u32s[i] >> 8) & 0xFF);
        pushbyte(st, (u32s[i] >> 16) & 0xFF);
        pushbyte(st, (u32s[i] >> 24) & 0xFF);
    }
}

/* Marshal a function def */
static void marshal_one_def(MarshalState *st, BenderFuncDef *def, int flags) {
    MARSH_STACKCHECK;
    for (int32_t i = 0; i < bender_v_count(st->seen_defs); i++) {
        if (st->seen_defs[i] == def) {
            pushbyte(st, LB_FUNCDEF_REF);
            pushint(st, i);
            return;
        }
    }
    /* Add to lookup */
    bender_v_push(st->seen_defs, def);

    pushint(st, def->flags);
    pushint(st, def->slotcount);
    pushint(st, def->arity);
    pushint(st, def->min_arity);
    pushint(st, def->max_arity);
    pushint(st, def->constants_length);
    pushint(st, def->bytecode_length);
    if (def->flags & BENDER_FUNCDEF_FLAG_NAMEDARGS)
        pushint(st, def->named_args_count);
    if (def->flags & BENDER_FUNCDEF_FLAG_HASENVS)
        pushint(st, def->environments_length);
    if (def->flags & BENDER_FUNCDEF_FLAG_HASDEFS)
        pushint(st, def->defs_length);
    if (def->flags & BENDER_FUNCDEF_FLAG_HASSYMBOLMAP)
        pushint(st, def->symbolmap_length);
    if (def->flags & BENDER_FUNCDEF_FLAG_HASNAME)
        marshal_one(st, bender_wrap_string(def->name), flags);
    if (def->flags & BENDER_FUNCDEF_FLAG_HASSOURCE)
        marshal_one(st, bender_wrap_string(def->source), flags);

    /* marshal constants */
    for (int32_t i = 0; i < def->constants_length; i++)
        marshal_one(st, def->constants[i], flags + 1);

    /* Marshal symbol map, if needed */
    for (int32_t i = 0; i < def->symbolmap_length; i++) {
        pushint(st, (int32_t) def->symbolmap[i].birth_pc);
        pushint(st, (int32_t) def->symbolmap[i].death_pc);
        pushint(st, (int32_t) def->symbolmap[i].slot_index);
        marshal_one(st, bender_wrap_symbol(def->symbolmap[i].symbol), flags + 1);
    }

    /* marshal the bytecode */
    bender_marshal_u32s(st, def->bytecode, def->bytecode_length);

    /* marshal the environments if needed */
    for (int32_t i = 0; i < def->environments_length; i++)
        pushint(st, def->environments[i]);

    /* marshal the sub funcdefs if needed */
    for (int32_t i = 0; i < def->defs_length; i++)
        marshal_one_def(st, def->defs[i], flags + 1);

    /* marshal source maps if needed */
    if (def->flags & BENDER_FUNCDEF_FLAG_HASSOURCEMAP) {
        int32_t current = 0;
        for (int32_t i = 0; i < def->bytecode_length; i++) {
            BenderSourceMapping map = def->sourcemap[i];
            pushint(st, map.line - current);
            pushint(st, map.column);
            current = map.line;
        }
    }

    /* Marshal closure bitset, if needed */
    if (def->flags & BENDER_FUNCDEF_FLAG_HASCLOBITSET) {
        bender_marshal_u32s(st, def->closure_bitset, ((def->slotcount - 1) >> 5) + 1);
    }
}

#define BENDER_FIBER_FLAG_HASCHILD (1 << 29)
#define BENDER_FIBER_FLAG_HASENV   (1 << 30)
#define BENDER_STACKFRAME_HASENV   (INT32_MIN)

/* Marshal a fiber */
static void marshal_one_fiber(MarshalState *st, BenderFiber *fiber, int flags) {
    MARSH_STACKCHECK;
    int32_t fflags = fiber->flags;
    if (fiber->child) fflags |= BENDER_FIBER_FLAG_HASCHILD;
    if (fiber->env) fflags |= BENDER_FIBER_FLAG_HASENV;
    if (bender_fiber_status(fiber) == BENDER_STATUS_ALIVE)
        bender_panic("cannot marshal alive fiber");
    pushint(st, fflags);
    pushint(st, fiber->frame);
    pushint(st, fiber->stackstart);
    pushint(st, fiber->stacktop);
    pushint(st, fiber->maxstack);
    /* Do frames */
    int32_t i = fiber->frame;
    int32_t j = fiber->stackstart - BENDER_FRAME_SIZE;
    while (i > 0) {
        BenderStackFrame *frame = (BenderStackFrame *)(fiber->data + i - BENDER_FRAME_SIZE);
        if (frame->env) frame->flags |= BENDER_STACKFRAME_HASENV;
        if (!frame->func) bender_panicf("cannot marshal fiber with c stackframe (%v)", bender_wrap_cfunction((BenderCFunction) frame->pc));
        pushint(st, frame->flags);
        pushint(st, frame->prevframe);
        int32_t pcdiff = (int32_t)(frame->pc - frame->func->def->bytecode);
        pushint(st, pcdiff);
        marshal_one(st, bender_wrap_function(frame->func), flags + 1);
        if (frame->env) marshal_one_env(st, frame->env, flags + 1);
        /* Marshal all values in the stack frame */
        for (int32_t k = i; k < j; k++)
            marshal_one(st, fiber->data[k], flags + 1);
        j = i - BENDER_FRAME_SIZE;
        i = frame->prevframe;
    }
    if (fiber->env) {
        marshal_one(st, bender_wrap_table(fiber->env), flags + 1);
    }
    if (fiber->child)
        marshal_one(st, bender_wrap_fiber(fiber->child), flags + 1);
    marshal_one(st, fiber->last_value, flags + 1);
}

void bender_marshal_size(BenderMarshalContext *ctx, size_t value) {
    bender_marshal_int64(ctx, (int64_t) value);
}

void bender_marshal_int64(BenderMarshalContext *ctx, int64_t value) {
    MarshalState *st = (MarshalState *)(ctx->m_state);
    push64(st, (uint64_t) value);
}

void bender_marshal_int(BenderMarshalContext *ctx, int32_t value) {
    MarshalState *st = (MarshalState *)(ctx->m_state);
    pushint(st, value);
}

/* Only use in unsafe - don't marshal pointers otherwise */
void bender_marshal_ptr(BenderMarshalContext *ctx, const void *ptr) {
    if (!(ctx->flags & BENDER_MARSHAL_UNSAFE)) {
        bender_panic("can only marshal pointers in unsafe mode");
    }
    MarshalState *st = (MarshalState *)(ctx->m_state);
    pushpointer(st, ptr);
}

void bender_marshal_byte(BenderMarshalContext *ctx, uint8_t value) {
    MarshalState *st = (MarshalState *)(ctx->m_state);
    pushbyte(st, value);
}

void bender_marshal_bytes(BenderMarshalContext *ctx, const uint8_t *bytes, size_t len) {
    MarshalState *st = (MarshalState *)(ctx->m_state);
    if (len > INT32_MAX) bender_panic("size_t too large to fit in buffer");
    pushbytes(st, bytes, (int32_t) len);
}

void bender_marshal_bender(BenderMarshalContext *ctx, Bender x) {
    MarshalState *st = (MarshalState *)(ctx->m_state);
    marshal_one(st, x, ctx->flags + 1);
}

#ifdef BENDER_MARSHAL_DEBUG
#define MARK_SEEN() \
    do { if (st->maybe_cycles) { \
        Bender _check = bender_table_get(&st->seen, x); \
        if (!bender_checktype(_check, BENDER_NIL)) bender_eprintf("double MARK_SEEN on %v\n", x); \
        bender_eprintf("made reference %d (%t) to %v\n", st->nextid, x, x); \
        bender_table_put_raw(&st->seen, x, bender_wrap_integer(st->nextid++)); \
    } } while (0)
#else
#define MARK_SEEN() \
    do { if (st->maybe_cycles) { \
        bender_table_put_raw(&st->seen, x, bender_wrap_integer(st->nextid++)); \
    } } while (0)
#endif

void bender_marshal_abstract(BenderMarshalContext *ctx, void *abstract) {
    MarshalState *st = (MarshalState *)(ctx->m_state);
    Bender x = bender_wrap_abstract(abstract);
    MARK_SEEN();
}

int bender_marshal_flags(BenderMarshalContext *ctx) {
    return ctx->flags;
}

static void marshal_one_abstract(MarshalState *st, Bender x, int flags) {
    void *abstract = bender_unwrap_abstract(x);
#ifdef BENDER_EV
    /* Threaded abstract types get passed through as pointers in the unsafe mode */
    if ((flags & BENDER_MARSHAL_UNSAFE) &&
            (BENDER_MEMORY_THREADED_ABSTRACT == (bender_abstract_head(abstract)->gc.flags & BENDER_MEM_TYPEBITS))) {

        /* Increment refcount before sending message. This prevents a "death in transit" problem
         * where a message is garbage collected while in transit between two threads - i.e., the sending threads
         * loses the reference and runs a garbage collection before the receiving thread gets the message. */
        bender_abstract_incref(abstract);
        bender_abstract_head(abstract)->exported = 1;
        pushbyte(st, LB_THREADED_ABSTRACT);
        pushbytes(st, (uint8_t *) &abstract, sizeof(abstract));
        MARK_SEEN();
        return;
    }
#endif
    const BenderAbstractType *at = bender_abstract_type(abstract);
    if (at->marshal) {
        pushbyte(st, LB_ABSTRACT);
        marshal_one(st, bender_csymbolv(at->name), flags + 1);
        BenderMarshalContext context = {st, NULL, flags + 1, NULL, at};
        at->marshal(abstract, &context);
    } else {
        bender_panicf("cannot marshal %p", x);
    }
}

/* The main body of the marshaling function. Is the main
 * entry point for the mutually recursive functions. */
static void marshal_one(MarshalState *st, Bender x, int flags) {
    MARSH_STACKCHECK;
    BenderType type = bender_type(x);

    /* Check simple primitives (non reference types, no benefit from memoization) */
    switch (type) {
        default:
            break;
        case BENDER_NIL:
            pushbyte(st, LB_NIL);
            return;
        case BENDER_BOOLEAN:
            pushbyte(st, bender_unwrap_boolean(x) ? LB_TRUE : LB_FALSE);
            return;
        case BENDER_NUMBER: {
            double xval = bender_unwrap_number(x);
            if (bender_checkintrange(xval)) {
                pushint(st, (int32_t) xval);
                return;
            }
            break;
        }
    }

    /* Check reference and registry value */
    {
        Bender check;
        if (st->maybe_cycles) {
            check = bender_table_get(&st->seen, x);
            if (bender_checkint(check)) {
                pushbyte(st, LB_REFERENCE);
                pushint(st, bender_unwrap_integer(check));
                return;
            }
        }
        if (st->rreg) {
            check = bender_table_get(st->rreg, x);
            if (bender_checktype(check, BENDER_SYMBOL)) {
                MARK_SEEN();
                const uint8_t *regname = bender_unwrap_symbol(check);
                pushbyte(st, LB_REGISTRY);
                pushint(st, bender_string_length(regname));
                pushbytes(st, regname, bender_string_length(regname));
                return;
            }
        }
    }

    /* Reference types */
    switch (type) {
        case BENDER_NUMBER: {
            union {
                double d;
                uint8_t bytes[8];
            } u;
            u.d = bender_unwrap_number(x);
#ifdef BENDER_BIG_ENDIAN
            /* Swap byte order */
            uint8_t temp;
            temp = u.bytes[7];
            u.bytes[7] = u.bytes[0];
            u.bytes[0] = temp;
            temp = u.bytes[6];
            u.bytes[6] = u.bytes[1];
            u.bytes[1] = temp;
            temp = u.bytes[5];
            u.bytes[5] = u.bytes[2];
            u.bytes[2] = temp;
            temp = u.bytes[4];
            u.bytes[4] = u.bytes[3];
            u.bytes[3] = temp;
#endif
            pushbyte(st, LB_REAL);
            pushbytes(st, u.bytes, 8);
            MARK_SEEN();
            return;
        }
        case BENDER_STRING:
        case BENDER_SYMBOL:
        case BENDER_KEYWORD: {
            const uint8_t *str = bender_unwrap_string(x);
            int32_t length = bender_string_length(str);
            /* Record reference */
            MARK_SEEN();
            uint8_t lb = (type == BENDER_STRING) ? LB_STRING :
                         (type == BENDER_SYMBOL) ? LB_SYMBOL :
                         LB_KEYWORD;
            pushbyte(st, lb);
            pushint(st, length);
            pushbytes(st, str, length);
            return;
        }
        case BENDER_BUFFER: {
            BenderBuffer *buffer = bender_unwrap_buffer(x);
            /* Record reference */
            MARK_SEEN();
#ifdef BENDER_EV
            if ((flags & BENDER_MARSHAL_UNSAFE) &&
                    (buffer->gc.flags & BENDER_BUFFER_FLAG_NO_REALLOC)) {
                pushbyte(st, LB_POINTER_BUFFER);
                pushint(st, buffer->count);
                pushint(st, buffer->capacity);
                pushpointer(st, buffer->data);
                return;
            }
#endif
            pushbyte(st, LB_BUFFER);
            pushint(st, buffer->count);
            pushbytes(st, buffer->data, buffer->count);
            return;
        }
        case BENDER_ARRAY: {
            int32_t i;
            BenderArray *a = bender_unwrap_array(x);
            MARK_SEEN();
            enum BenderMemoryType memtype = bender_gc_type(a);
            pushbyte(st, memtype == BENDER_MEMORY_ARRAY_WEAK ? LB_ARRAY_WEAK : LB_ARRAY);
            pushint(st, a->count);
            for (i = 0; i < a->count; i++)
                marshal_one(st, a->data[i], flags + 1);
            return;
        }
        case BENDER_TUPLE: {
            int32_t i, count, flag;
            const Bender *tup = bender_unwrap_tuple(x);
            count = bender_tuple_length(tup);
            flag = bender_tuple_flag(tup) >> 16;
            pushbyte(st, LB_TUPLE);
            pushint(st, count);
            pushint(st, flag);
            /* Tuple source maps ride the payload: images marshal parsed
             * constants, and dropping sm_line/sm_column here is what left
             * every image constant reading [-1 -1] at runtime. */
            pushint(st, bender_tuple_head(tup)->sm_line);
            pushint(st, bender_tuple_head(tup)->sm_column);
            for (i = 0; i < count; i++)
                marshal_one(st, tup[i], flags + 1);
            /* Mark as seen AFTER marshaling */
            MARK_SEEN();
            return;
        }
        case BENDER_TABLE: {
            BenderTable *t = bender_unwrap_table(x);
            MARK_SEEN();
            enum BenderMemoryType memtype = bender_gc_type(t);
            if (memtype == BENDER_MEMORY_TABLE_WEAKK) {
                pushbyte(st, t->proto ? LB_TABLE_WEAKK_PROTO : LB_TABLE_WEAKK);
            } else if (memtype == BENDER_MEMORY_TABLE_WEAKV) {
                pushbyte(st, t->proto ? LB_TABLE_WEAKV_PROTO : LB_TABLE_WEAKV);
            } else if (memtype == BENDER_MEMORY_TABLE_WEAKKV) {
                pushbyte(st, t->proto ? LB_TABLE_WEAKKV_PROTO : LB_TABLE_WEAKKV);
            } else {
                pushbyte(st, t->proto ? LB_TABLE_PROTO : LB_TABLE);
            }
            pushint(st, t->count);
            if (t->proto)
                marshal_one(st, bender_wrap_table(t->proto), flags + 1);
            for (int32_t i = 0; i < t->capacity; i++) {
                if (bender_checktype(t->data[i].key, BENDER_NIL))
                    continue;
                marshal_one(st, t->data[i].key, flags + 1);
                marshal_one(st, t->data[i].value, flags + 1);
            }
            return;
        }
        case BENDER_STRUCT: {
            int32_t count;
            const BenderKV *struct_ = bender_unwrap_struct(x);
            count = bender_struct_length(struct_);
            pushbyte(st, bender_struct_proto(struct_) ? LB_STRUCT_PROTO : LB_STRUCT);
            pushint(st, count);
            if (bender_struct_proto(struct_))
                marshal_one(st, bender_wrap_struct(bender_struct_proto(struct_)), flags + 1);
            for (int32_t i = 0; i < bender_struct_capacity(struct_); i++) {
                if (bender_checktype(struct_[i].key, BENDER_NIL))
                    continue;
                marshal_one(st, struct_[i].key, flags + 1);
                marshal_one(st, struct_[i].value, flags + 1);
            }
            /* Mark as seen AFTER marshaling */
            MARK_SEEN();
            return;
        }
        case BENDER_ABSTRACT: {
            marshal_one_abstract(st, x, flags);
            return;
        }
        case BENDER_FUNCTION: {
            pushbyte(st, LB_FUNCTION);
            BenderFunction *func = bender_unwrap_function(x);
            pushint(st, func->def->environments_length);
            /* Mark seen before reading def */
            MARK_SEEN();
            marshal_one_def(st, func->def, flags);
            for (int32_t i = 0; i < func->def->environments_length; i++)
                marshal_one_env(st, func->envs[i], flags + 1);
            return;
        }
        case BENDER_FIBER: {
            MARK_SEEN();
            pushbyte(st, LB_FIBER);
            marshal_one_fiber(st, bender_unwrap_fiber(x), flags + 1);
            return;
        }
        case BENDER_CFUNCTION: {
            if (!(flags & BENDER_MARSHAL_UNSAFE)) goto no_registry;
            MARK_SEEN();
            pushbyte(st, LB_UNSAFE_CFUNCTION);
            BenderCFunction cfn = bender_unwrap_cfunction(x);
            pushbytes(st, (uint8_t *) &cfn, sizeof(BenderCFunction));
            return;
        }
        case BENDER_POINTER: {
            if (!(flags & BENDER_MARSHAL_UNSAFE)) goto no_registry;
            MARK_SEEN();
            pushbyte(st, LB_UNSAFE_POINTER);
            pushpointer(st, bender_unwrap_pointer(x));
            return;
        }
    no_registry:
        default: {
            bender_panicf("no registry value and cannot marshal %p", x);
        }
    }
#undef MARK_SEEN
}

void bender_marshal(
    BenderBuffer *buf,
    Bender x,
    BenderTable *rreg,
    int flags) {
    MarshalState st;
    st.buf = buf;
    st.nextid = 0;
    st.seen_defs = NULL;
    st.seen_envs = NULL;
    st.rreg = rreg;
    st.maybe_cycles = !(flags & BENDER_MARSHAL_NO_CYCLES);
    /* Phase 2a root discipline: suspend collections for the whole
     * operation. The marshal state (output buffer, seen tables, id
     * vectors) lives in C/struct scratch the collector cannot see; a
     * collection mid-operation could sweep the in-progress image or the
     * input's shared structure. The lock is released on all paths; a
     * panic inside re-raises after unlocking (same signal as without).
     *
     * Ordering: bender_restore(&tstate) re-applies the try-time
     * gc_suspend (captured AFTER the lock), so the unlock must run
     * AFTER the final restore — placing it before re-leaked +1 per
     * call, permanently suspending collection in AOT runs (one fiber
     * for the whole script, so the leak was live at gccollect; the
     * interpreter wiped it at each form's bender_continue exit). */
    int lock = bender_gclock();
    BenderTryState tstate;
    BenderSignal sig = bender_try(&tstate);
    if (!sig) {
        bender_table_init(&st.seen, 0);
        marshal_one(&st, x, flags);
        bender_table_deinit(&st.seen);
    }
    bender_v_free(st.seen_envs);
    bender_v_free(st.seen_defs);
    if (sig) {
        bender_restore(&tstate);
        bender_gcunlock(lock);
        if (sig == BENDER_SIGNAL_ERROR) {
            bender_panicv(tstate.payload);
        } else {
            bender_signalv(sig, tstate.payload);
        }
    }
    bender_restore(&tstate);
    bender_gcunlock(lock);
}

typedef struct {
    jmp_buf err;
    Bender *lookup;
    BenderTable *reg;
    BenderFuncEnv **lookup_envs;
    BenderFuncDef **lookup_defs;
    const uint8_t *start;
    const uint8_t *end;
} UnmarshalState;

#define MARSH_EOS(st, data) do { \
    if ((data) >= (st)->end) bender_panic("unexpected end of source");\
} while (0)

/* Helper to read a 32 bit integer from an unmarshal state */
static int32_t readint(UnmarshalState *st, const uint8_t **atdata) {
    const uint8_t *data = *atdata;
    int32_t ret;
    MARSH_EOS(st, data);
    if (*data < 128) {
        ret = *data++;
    } else if (*data < 192) {
        MARSH_EOS(st, data + 1);
        uint32_t uret = ((data[0] & 0x3F) << 8) + data[1];
        /* Sign extend 18 MSBs */
        uret |= (uret >> 13) ? 0xFFFFC000 : 0;
        ret = (int32_t)uret;
        data += 2;
    } else if (*data == LB_INTEGER) {
        MARSH_EOS(st, data + 4);
        uint32_t ui = ((uint32_t)(data[1]) << 24) |
                      ((uint32_t)(data[2]) << 16) |
                      ((uint32_t)(data[3]) << 8) |
                      (uint32_t)(data[4]);
        ret = (int32_t)ui;
        data += 5;
    } else {
        bender_panicf("expected integer, got byte %x at index %d",
                     *data,
                     data - st->start);
        ret = 0;
    }
    *atdata = data;
    return ret;
}

/* Helper to read a natural number (int >= 0). */
static int32_t readnat(UnmarshalState *st, const uint8_t **atdata) {
    int32_t ret = readint(st, atdata);
    if (ret < 0) {
        bender_panicf("expected integer >= 0, got %d", ret);
    }
    return ret;
}

/* Helper to read a size_t (up to 8 bytes unsigned). */
static uint64_t read64(UnmarshalState *st, const uint8_t **atdata) {
    uint64_t ret;
    const uint8_t *data = *atdata;
    MARSH_EOS(st, data);
    if (*data <= 0xF0) {
        /* Single byte */
        ret = *data;
        *atdata = data + 1;
    } else {
        /* Multibyte, little endian */
        int nbytes = *data - 0xF0;
        ret = 0;
        if (nbytes > 8) bender_panic("invalid 64 bit integer");
        MARSH_EOS(st, data + nbytes);
        for (int i = nbytes; i > 0; i--)
            ret = (ret << 8) + data[i];
        *atdata = data + nbytes + 1;
    }
    return ret;
}

#ifdef BENDER_MARSHAL_DEBUG
static void dump_reference_table(UnmarshalState *st) {
    for (int32_t i = 0; i < bender_v_count(st->lookup); i++) {
        bender_eprintf("  reference %d (%t) = %v\n", i, st->lookup[i], st->lookup[i]);
    }
}
#endif

/* Assert a bender type */
static void bender_asserttype(Bender x, BenderType t, UnmarshalState *st) {
    if (!bender_checktype(x, t)) {
#ifdef BENDER_MARSHAL_DEBUG
        dump_reference_table(st);
#else
        (void) st;
#endif
        bender_panicf("expected type %T, got %v", 1 << t, x);
    }
}

/* Forward declarations for mutual recursion */
static const uint8_t *unmarshal_one(
    UnmarshalState *st,
    const uint8_t *data,
    Bender *out,
    int flags);
static const uint8_t *unmarshal_one_env(
    UnmarshalState *st,
    const uint8_t *data,
    BenderFuncEnv **out,
    int flags);
static const uint8_t *unmarshal_one_def(
    UnmarshalState *st,
    const uint8_t *data,
    BenderFuncDef **out,
    int flags);
static const uint8_t *unmarshal_one_fiber(
    UnmarshalState *st,
    const uint8_t *data,
    BenderFiber **out,
    int flags);

/* Unmarshal a funcenv */
static const uint8_t *unmarshal_one_env(
    UnmarshalState *st,
    const uint8_t *data,
    BenderFuncEnv **out,
    int flags) {
    MARSH_EOS(st, data);
    if (*data == LB_FUNCENV_REF) {
        data++;
        int32_t index = readint(st, &data);
        if (index < 0 || index >= bender_v_count(st->lookup_envs))
            bender_panicf("invalid funcenv reference %d", index);
        *out = st->lookup_envs[index];
    } else {
        BenderFuncEnv *env = bender_gcalloc(BENDER_MEMORY_FUNCENV, sizeof(BenderFuncEnv));
        env->length = 0;
        env->offset = 0;
        env->as.values = NULL;
        bender_v_push(st->lookup_envs, env);
        int32_t offset = readnat(st, &data);
        int32_t length = readnat(st, &data);
        if (offset > 0) {
            Bender fiberv;
            /* On stack variant */
            data = unmarshal_one(st, data, &fiberv, flags);
            bender_asserttype(fiberv, BENDER_FIBER, st);
            env->as.fiber = bender_unwrap_fiber(fiberv);
            /* Negative offset indicates untrusted input */
            env->offset = -offset;
        } else {
            /* Off stack variant */
            if (length == 0) {
                bender_panic("invalid funcenv length");
            }
            env->as.values = array_allocate(sizeof(Bender), length);
            if (!env->as.values) {
                BENDER_OUT_OF_MEMORY;
            }
            env->offset = 0;
            for (int32_t i = 0; i < length; i++)
                data = unmarshal_one(st, data, env->as.values + i, flags);
        }
        env->length = length;
        *out = env;
    }
    return data;
}

/* Unmarshal a series of u32s */
static const uint8_t *bender_unmarshal_u32s(UnmarshalState *st, const uint8_t *data, uint32_t *into, int32_t n) {
    for (int32_t i = 0; i < n; i++) {
        MARSH_EOS(st, data + 3);
        into[i] =
            (uint32_t)(data[0]) |
            ((uint32_t)(data[1]) << 8) |
            ((uint32_t)(data[2]) << 16) |
            ((uint32_t)(data[3]) << 24);
        data += 4;
    }
    return data;
}

/* Unmarshal a funcdef */
static const uint8_t *unmarshal_one_def(
    UnmarshalState *st,
    const uint8_t *data,
    BenderFuncDef **out,
    int flags) {
    MARSH_EOS(st, data);
    if (*data == LB_FUNCDEF_REF) {
        data++;
        int32_t index = readint(st, &data);
        if (index < 0 || index >= bender_v_count(st->lookup_defs))
            bender_panicf("invalid funcdef reference %d", index);
        *out = st->lookup_defs[index];
    } else {
        /* Initialize with values that will not break garbage collection
         * if unmarshalling fails. */
        BenderFuncDef *def = bender_gcalloc(BENDER_MEMORY_FUNCDEF, sizeof(BenderFuncDef));
        def->environments_length = 0;
        def->defs_length = 0;
        def->constants_length = 0;
        def->bytecode_length = 0;
        def->name = NULL;
        def->source = NULL;
        def->closure_bitset = NULL;
        def->defs = NULL;
        def->environments = NULL;
        def->constants = NULL;
        def->bytecode = NULL;
        def->native_code = NULL;
        def->native_attach_gen = 0;
        def->native_verified = NULL;
        def->native_verified_epoch = 0;
        def->sourcemap = NULL;
        def->symbolmap = NULL;
        def->symbolmap_length = 0;
        def->named_args_count = 0;
        bender_v_push(st->lookup_defs, def);

        /* Set default lengths to zero */
        int32_t bytecode_length = 0;
        int32_t constants_length = 0;
        int32_t environments_length = 0;
        int32_t defs_length = 0;
        int32_t symbolmap_length = 0;

        /* Read flags and other fixed values */
        def->flags = readint(st, &data);
        def->slotcount = readnat(st, &data);
        def->arity = readnat(st, &data);
        def->min_arity = readnat(st, &data);
        def->max_arity = readnat(st, &data);

        /* Read some lengths */
        constants_length = readnat(st, &data);
        bytecode_length = readnat(st, &data);
        if (def->flags & BENDER_FUNCDEF_FLAG_NAMEDARGS)
            def->named_args_count = readnat(st, &data);
        if (def->flags & BENDER_FUNCDEF_FLAG_HASENVS)
            environments_length = readnat(st, &data);
        if (def->flags & BENDER_FUNCDEF_FLAG_HASDEFS)
            defs_length = readnat(st, &data);
        if (def->flags & BENDER_FUNCDEF_FLAG_HASSYMBOLMAP)
            symbolmap_length = readnat(st, &data);

        /* Check name and source (optional) */
        if (def->flags & BENDER_FUNCDEF_FLAG_HASNAME) {
            Bender x;
            data = unmarshal_one(st, data, &x, flags + 1);
            bender_asserttype(x, BENDER_STRING, st);
            def->name = bender_unwrap_string(x);
        }
        if (def->flags & BENDER_FUNCDEF_FLAG_HASSOURCE) {
            Bender x;
            data = unmarshal_one(st, data, &x, flags + 1);
            bender_asserttype(x, BENDER_STRING, st);
            def->source = bender_unwrap_string(x);
        }

        /* Unmarshal constants */
        if (constants_length) {
            def->constants = array_allocate(sizeof(Bender), constants_length);
            if (!def->constants) {
                BENDER_OUT_OF_MEMORY;
            }
            for (int32_t i = 0; i < constants_length; i++)
                data = unmarshal_one(st, data, def->constants + i, flags + 1);
        } else {
            def->constants = NULL;
        }
        def->constants_length = constants_length;

        /* Unmarshal symbol map, if needed */
        if (def->flags & BENDER_FUNCDEF_FLAG_HASSYMBOLMAP) {
            def->symbolmap = array_allocate(sizeof(BenderSymbolMap), symbolmap_length);
            if (def->symbolmap == NULL) {
                BENDER_OUT_OF_MEMORY;
            }
            for (int32_t i = 0; i < symbolmap_length; i++) {
                def->symbolmap[i].birth_pc = (uint32_t) readint(st, &data);
                def->symbolmap[i].death_pc = (uint32_t) readint(st, &data);
                def->symbolmap[i].slot_index = (uint32_t) readint(st, &data);
                Bender value;
                data = unmarshal_one(st, data, &value, flags + 1);
                if (!bender_checktype(value, BENDER_SYMBOL)) {
                    bender_panicf("corrupted symbolmap when unmarshalling debug info, got %v", value);
                }
                def->symbolmap[i].symbol = bender_unwrap_symbol(value);
            }
            def->symbolmap_length = (uint32_t) symbolmap_length;
        }

        /* Unmarshal bytecode */
        def->bytecode = array_allocate(sizeof(uint32_t), bytecode_length);
        if (!def->bytecode) {
            BENDER_OUT_OF_MEMORY;
        }
        data = bender_unmarshal_u32s(st, data, def->bytecode, bytecode_length);
        def->bytecode_length = bytecode_length;

        /* Unmarshal environments */
        if (def->flags & BENDER_FUNCDEF_FLAG_HASENVS) {
            def->environments = bender_calloc(1, sizeof(int32_t) * (size_t) environments_length);
            if (!def->environments) {
                BENDER_OUT_OF_MEMORY;
            }
            for (int32_t i = 0; i < environments_length; i++) {
                def->environments[i] = readint(st, &data);
            }
        } else {
            def->environments = NULL;
        }
        def->environments_length = environments_length;

        /* Unmarshal sub funcdefs */
        if (def->flags & BENDER_FUNCDEF_FLAG_HASDEFS) {
            def->defs = bender_calloc(1, sizeof(BenderFuncDef *) * (size_t) defs_length);
            if (!def->defs) {
                BENDER_OUT_OF_MEMORY;
            }
            for (int32_t i = 0; i < defs_length; i++) {
                data = unmarshal_one_def(st, data, def->defs + i, flags + 1);
            }
        } else {
            def->defs = NULL;
        }
        def->defs_length = defs_length;

        /* Unmarshal source maps if needed */
        if (def->flags & BENDER_FUNCDEF_FLAG_HASSOURCEMAP) {
            int32_t current = 0;
            def->sourcemap = array_allocate(sizeof(BenderSourceMapping), bytecode_length);
            if (!def->sourcemap) {
                BENDER_OUT_OF_MEMORY;
            }
            for (int32_t i = 0; i < bytecode_length; i++) {
                current += readint(st, &data);
                def->sourcemap[i].line = current;
                def->sourcemap[i].column = readint(st, &data);
            }
        } else {
            def->sourcemap = NULL;
        }

        /* Unmarshal closure bitset if needed */
        if (def->flags & BENDER_FUNCDEF_FLAG_HASCLOBITSET) {
            int32_t n = ((def->slotcount - 1) >> 5) + 1;
            def->closure_bitset = array_allocate(sizeof(uint32_t), n);
            if (NULL == def->closure_bitset) {
                BENDER_OUT_OF_MEMORY;
            }
            data = bender_unmarshal_u32s(st, data, def->closure_bitset, n);
        }

        /* Validate */
        int status;
        if ((status = bender_verify(def))) {
            bender_panicf("funcdef has invalid bytecode: %d", status);
        }

        /* Set def */
        *out = def;
    }
    return data;
}

/* Unmarshal a fiber */
static const uint8_t *unmarshal_one_fiber(
    UnmarshalState *st,
    const uint8_t *data,
    BenderFiber **out,
    int flags) {

    /* Initialize a new fiber with gc friendly defaults. Unmarshalled
     * fibers get a fresh owner id (Step 1): the origin VM's ids are
     * meaningless here, and reuse would misattribute. */
    BenderFiber *fiber = bender_gcalloc(BENDER_MEMORY_FIBER, sizeof(BenderFiber));
    bender_fiber_init_owner(fiber);
    bender_census_add(fiber);
    {
        /* Step 1.2: nursery starts empty (see fiber_alloc). */
        int nci;
        for (nci = 0; nci < BENDER_SLAB_NCLASSES; nci++)
            fiber->nursery_cur[nci] = NULL;
        fiber->gc_nursery_bytes = 0;
        /* bender_gcalloc does NOT zero, so every field the collector reads
         * must be set here. gc_minor_blocks scales this fiber's scoped-minor
         * trigger (see maybe_collect): left uninitialized it makes the
         * threshold garbage, so a fiber either never runs a scoped minor or
         * runs one per allocation, nondeterministically per process. */
        fiber->gc_minor_blocks = 0;
    }
    {
        /* P3.5b: heap lists start empty (see fiber_alloc). Unmarshalled
         * fibers must not inherit garbage heads (gcalloc doesn't zero
         * fresh bump slots) — births would file into wild memory. */
        int hli;
        for (hli = 0; hli < 3; hli++)
            fiber->heap_lists[hli] = NULL;
    }
    fiber->proc = NULL;
    fiber->heap_strict = 0;
    /* Unmarshalled fibers have no channel pendings here (a suspended
     * fiber cannot be mid-park across a marshal). */
    fiber->chan_pends = NULL;
    /* P3.6: unmarshalled fibers always take private stacks (sharedness
     * is a lane-local scheduling property, never marshalled). */
    fiber->shared_stack = 0;
    fiber->flags = 0;
    fiber->frame = 0;
    fiber->stackstart = 0;
    fiber->stacktop = 0;
    fiber->capacity = 0;
    fiber->maxstack = 0;
    fiber->data = NULL;
    fiber->child = NULL;
    fiber->env = NULL;
    fiber->last_value = bender_wrap_nil();
    /* Native-stack fields must be initialized explicitly (like fiber_alloc):
     * gcalloc slots are not guaranteed zero (dirty slab bump slots and
     * malloc reuse), and ensure_native_stack below treats a non-NULL base
     * as a live mapping. Uninitialized garbage here primes a wild stack. */
    fiber->native_stack = NULL;
    fiber->native_stack_base = NULL;
    fiber->native_stack_size = 0;
    fiber->native_stack_committed = NULL;
    fiber->stack_overflowed = 0;
#ifdef BENDER_EV
    fiber->sched_id = 0;
    fiber->supervisor_channel = NULL;
    /* Interrupts do not cross VMs. */
    fiber->interrupt_requested = 0;
    fiber->interrupt_value = bender_wrap_nil();
    fiber->ev_state = NULL;
    fiber->ev_callback = NULL;
    fiber->ev_stream = NULL;
#endif

    /* Push fiber to seen stack */
    bender_v_push(st->lookup, bender_wrap_fiber(fiber));

    /* Read ints */
    int32_t fiber_flags = readint(st, &data);
    int32_t frame = readnat(st, &data);
    int32_t fiber_stackstart = readnat(st, &data);
    int32_t fiber_stacktop = readnat(st, &data);
    int32_t fiber_maxstack = readnat(st, &data);
    BenderTable *fiber_env = NULL;

    /* Check for bad flags and ints */
    if ((int32_t)(frame + BENDER_FRAME_SIZE) > fiber_stackstart ||
            fiber_stackstart > fiber_stacktop ||
            fiber_stacktop > fiber_maxstack) {
        bender_panic("fiber has incorrect stack setup");
    }

    /* Allocate stack memory */
    if (fiber_stacktop < INT32_MAX - 10) {
        fiber->capacity = fiber_stacktop + 10;
    } else {
        /* Extra capacity is usually nice to avoid immediately reallocing on pushed arguments, but not needed */
        fiber->capacity = INT32_MAX;
    }
    fiber->data = array_allocate(sizeof(Bender), fiber->capacity);
    if (!fiber->data) {
        BENDER_OUT_OF_MEMORY;
    }
    for (int32_t i = 0; i < fiber->capacity; i++) {
        fiber->data[i] = bender_wrap_nil();
    }

    /* get frames */
    int32_t stack = frame;
    int32_t stacktop = fiber_stackstart - BENDER_FRAME_SIZE;
    while (stack > 0) {
        BenderFunction *func = NULL;
        BenderFuncDef *def = NULL;
        BenderFuncEnv *env = NULL;
        int32_t frameflags = readint(st, &data);
        int32_t prevframe = readnat(st, &data);
        int32_t pcdiff = readnat(st, &data);

        /* Get frame items */
        Bender *framestack = fiber->data + stack;
        BenderStackFrame *framep = bender_stack_frame(framestack);

        /* Get function */
        Bender funcv;
        data = unmarshal_one(st, data, &funcv, flags + 1);
        bender_asserttype(funcv, BENDER_FUNCTION, st);
        func = bender_unwrap_function(funcv);
        def = func->def;

        /* Check env */
        if (frameflags & BENDER_STACKFRAME_HASENV) {
            frameflags &= ~BENDER_STACKFRAME_HASENV;
            data = unmarshal_one_env(st, data, &env, flags + 1);
        }

        /* Error checking */
        int32_t expected_framesize = def->slotcount;
        if (expected_framesize != stacktop - stack) {
            bender_panic("fiber stackframe size mismatch");
        }
        if (pcdiff >= def->bytecode_length) {
            bender_panic("fiber stackframe has invalid pc");
        }
        if ((int32_t)(prevframe + BENDER_FRAME_SIZE) > stack) {
            bender_panic("fiber stackframe does not align with previous frame");
        }

        /* Get stack items */
        for (int32_t i = stack; i < stacktop; i++)
            data = unmarshal_one(st, data, fiber->data + i, flags + 1);

        /* Set frame */
        framep->env = env;
        framep->pc = def->bytecode + pcdiff;
        framep->prevframe = prevframe;
        framep->flags = frameflags;
        framep->func = func;

        /* Goto previous frame */
        stacktop = stack - BENDER_FRAME_SIZE;
        stack = prevframe;
    }
    if (stack < 0) {
        bender_panic("fiber has too many stackframes");
    }

    /* Check for fiber env */
    if (fiber_flags & BENDER_FIBER_FLAG_HASENV) {
        Bender envv;
        fiber_flags &= ~BENDER_FIBER_FLAG_HASENV;
        data = unmarshal_one(st, data, &envv, flags + 1);
        bender_asserttype(envv, BENDER_TABLE, st);
        fiber_env = bender_unwrap_table(envv);
    }

    /* Check for child fiber */
    if (fiber_flags & BENDER_FIBER_FLAG_HASCHILD) {
        Bender fiberv;
        fiber_flags &= ~BENDER_FIBER_FLAG_HASCHILD;
        data = unmarshal_one(st, data, &fiberv, flags + 1);
        bender_asserttype(fiberv, BENDER_FIBER, st);
        fiber->child = bender_unwrap_fiber(fiberv);
    }

    /* Get the fiber last value */
    data = unmarshal_one(st, data, &fiber->last_value, flags + 1);

    /* We have valid fiber, finally construct remaining fields. */
    fiber->frame = frame;
    fiber->flags = fiber_flags;
    fiber->stackstart = fiber_stackstart;
    fiber->stacktop = fiber_stacktop;
    fiber->maxstack = fiber_maxstack;
    fiber->env = fiber_env;

    int status = bender_fiber_status(fiber);
    if (status < 0 || status > BENDER_STATUS_ALIVE) {
        bender_panic("invalid fiber status");
    }

    /* Native-stack substrate (see docs/internals/native-fibers.md):
     * unmarshalled fibers bypass fiber_alloc, so arm their switch context
     * here and initialize the cross-switch value slots explicitly
     * (gcalloc zeroes are not valid nils). */
    fiber->in_value = bender_wrap_nil();
    fiber->out_payload = bender_wrap_nil();
    fiber->out_signal = BENDER_SIGNAL_OK;
    fiber->resume_ctx = NULL;
    fiber->suspend_mode = 0;
    fiber->resume_fiber = NULL;
    fiber->saved_signal_buf = NULL;
    fiber->saved_return_reg = NULL;
    fiber->saved_coerce_error = 0;
    fiber->saved_stackn = 0;
    fiber->saved_gc_suspend = 0;
    bender_fiber_ensure_native_stack(fiber);

    /* Return data */
    *out = fiber;
    return data;
}

void bender_unmarshal_ensure(BenderMarshalContext *ctx, size_t size) {
    UnmarshalState *st = (UnmarshalState *)(ctx->u_state);
    MARSH_EOS(st, ctx->data + size);
}

int32_t bender_unmarshal_int(BenderMarshalContext *ctx) {
    UnmarshalState *st = (UnmarshalState *)(ctx->u_state);
    return readint(st, &(ctx->data));
}

size_t bender_unmarshal_size(BenderMarshalContext *ctx) {
    return (size_t) bender_unmarshal_int64(ctx);
}

int64_t bender_unmarshal_int64(BenderMarshalContext *ctx) {
    UnmarshalState *st = (UnmarshalState *)(ctx->u_state);
    return read64(st, &(ctx->data));
}

void *bender_unmarshal_ptr(BenderMarshalContext *ctx) {
    if (!(ctx->flags & BENDER_MARSHAL_UNSAFE)) {
        bender_panic("can only unmarshal pointers in unsafe mode");
    }
    UnmarshalState *st = (UnmarshalState *)(ctx->u_state);
    void *ptr;
    MARSH_EOS(st, ctx->data + sizeof(void *) - 1);
    memcpy((char *) &ptr, ctx->data, sizeof(void *));
    ctx->data += sizeof(void *);
    return ptr;
}

uint8_t bender_unmarshal_byte(BenderMarshalContext *ctx) {
    UnmarshalState *st = (UnmarshalState *)(ctx->u_state);
    MARSH_EOS(st, ctx->data);
    return *(ctx->data++);
}

void bender_unmarshal_bytes(BenderMarshalContext *ctx, uint8_t *dest, size_t len) {
    UnmarshalState *st = (UnmarshalState *)(ctx->u_state);
    MARSH_EOS(st, ctx->data + len - 1);
    safe_memcpy(dest, ctx->data, len);
    ctx->data += len;
}

Bender bender_unmarshal_bender(BenderMarshalContext *ctx) {
    Bender ret;
    UnmarshalState *st = (UnmarshalState *)(ctx->u_state);
    ctx->data = unmarshal_one(st, ctx->data, &ret, ctx->flags);
    return ret;
}

void bender_unmarshal_abstract_reuse(BenderMarshalContext *ctx, void *p) {
    UnmarshalState *st = (UnmarshalState *)(ctx->u_state);
    if (ctx->at == NULL) {
        bender_panicf("bender_unmarshal_abstract called more than once");
    }
    bender_v_push(st->lookup, bender_wrap_abstract(p));
    ctx->at = NULL;
}

void *bender_unmarshal_abstract(BenderMarshalContext *ctx, size_t size) {
    void *p = bender_abstract(ctx->at, size);
    bender_unmarshal_abstract_reuse(ctx, p);
    return p;
}

/* Phase E: the BENDER_THREADS gate was stale — threaded allocation
 * works wherever BENDER_EV does (every threaded type in this tree
 * allocates through it, including on builds that never define
 * BENDER_THREADS). Safe-mode unmarshal of a type with marshal fns is a
 * deep copy (fresh threaded alloc; items copied, pendings dropped by
 * the type's own unmarshal); UNSAFE pointer passthrough still travels
 * by reference (LB_THREADED_ABSTRACT). Types without marshal fns
 * (thread handles, pools, locks, jitfns) keep refusing everywhere. */
void *bender_unmarshal_abstract_threaded(BenderMarshalContext *ctx, size_t size) {
    void *p = bender_abstract_threaded(ctx->at, size);
    bender_unmarshal_abstract_reuse(ctx, p);
    return p;
}

int bender_unmarshal_flags(BenderMarshalContext *ctx) {
    return ctx->flags;
}

static const uint8_t *unmarshal_one_abstract(UnmarshalState *st, const uint8_t *data, Bender *out, int flags) {
    Bender key;
    data = unmarshal_one(st, data, &key, flags + 1);
    const BenderAbstractType *at = bender_get_abstract_type(key);
    if (at == NULL) bender_panic("unknown abstract type");
    if (at->unmarshal) {
        BenderMarshalContext context = {NULL, st, flags, data, at};
        void *abst = at->unmarshal(&context);
        bender_assert(abst != NULL, "null pointer abstract");
        *out = bender_wrap_abstract(abst);
        if (context.at != NULL) {
            bender_panic("bender_unmarshal_abstract not called");
        }
        return context.data;
    }
    bender_panic("invalid abstract type - no unmarshal function pointer");
}

static const uint8_t *unmarshal_one(
    UnmarshalState *st,
    const uint8_t *data,
    Bender *out,
    int flags) {
    uint8_t lead;
    MARSH_STACKCHECK;
    MARSH_EOS(st, data);
    lead = data[0];
    if (lead < LB_REAL) {
        *out = bender_wrap_integer(readint(st, &data));
        return data;
    }
    switch (lead) {
        case LB_NIL:
            *out = bender_wrap_nil();
            return data + 1;
        case LB_FALSE:
            *out = bender_wrap_false();
            return data + 1;
        case LB_TRUE:
            *out = bender_wrap_true();
            return data + 1;
        case LB_INTEGER:
            /* Long integer */
            MARSH_EOS(st, data + 4);
            uint32_t ui = ((uint32_t)(data[4])) |
                          ((uint32_t)(data[3]) << 8) |
                          ((uint32_t)(data[2]) << 16) |
                          ((uint32_t)(data[1]) << 24);
            int32_t si = (int32_t)ui;
            *out = bender_wrap_integer(si);
            return data + 5;
        case LB_REAL:
            /* Real */
        {
            union {
                double d;
                uint8_t bytes[8];
            } u;
            MARSH_EOS(st, data + 8);
#ifdef BENDER_BIG_ENDIAN
            u.bytes[0] = data[8];
            u.bytes[1] = data[7];
            u.bytes[2] = data[6];
            u.bytes[3] = data[5];
            u.bytes[4] = data[4];
            u.bytes[5] = data[3];
            u.bytes[6] = data[2];
            u.bytes[7] = data[1];
#else
            memcpy(&u.bytes, data + 1, sizeof(double));
#endif
            *out = bender_wrap_number_safe(u.d);
            bender_v_push(st->lookup, *out);
            return data + 9;
        }
        case LB_STRING:
        case LB_SYMBOL:
        case LB_BUFFER:
        case LB_KEYWORD:
        case LB_REGISTRY: {
            data++;
            int32_t len = readnat(st, &data);
            MARSH_EOS(st, data - 1 + len);
            if (lead == LB_STRING) {
                const uint8_t *str = bender_string(data, len);
                *out = bender_wrap_string(str);
            } else if (lead == LB_SYMBOL) {
                const uint8_t *str = bender_symbol(data, len);
                *out = bender_wrap_symbol(str);
            } else if (lead == LB_KEYWORD) {
                const uint8_t *str = bender_keyword(data, len);
                *out = bender_wrap_keyword(str);
            } else if (lead == LB_REGISTRY) {
                if (st->reg) {
                    Bender regkey = bender_symbolv(data, len);
                    *out = bender_table_get(st->reg, regkey);
                } else {
                    *out = bender_wrap_nil();
                }
            } else { /* (lead == LB_BUFFER) */
                BenderBuffer *buffer = bender_buffer(len);
                buffer->count = len;
                safe_memcpy(buffer->data, data, len);
                *out = bender_wrap_buffer(buffer);
            }
            bender_v_push(st->lookup, *out);
            return data + len;
        }
        case LB_FIBER: {
            BenderFiber *fiber;
            data = unmarshal_one_fiber(st, data + 1, &fiber, flags + 1);
            *out = bender_wrap_fiber(fiber);
            return data;
        }
        case LB_FUNCTION: {
            BenderFunction *func;
            BenderFuncDef *def;
            data++;
            int32_t len = readnat(st, &data);
            if (len > 255) {
                bender_panicf("invalid function - too many environments (%d)", len);
            }
            func = bender_gcalloc(BENDER_MEMORY_FUNCTION, sizeof(BenderFunction) +
                                 len * sizeof(BenderFuncEnv));
            func->def = NULL;
            for (int32_t i = 0; i < len; i++) {
                func->envs[i] = NULL;
            }
            *out = bender_wrap_function(func);
            bender_v_push(st->lookup, *out);
            data = unmarshal_one_def(st, data, &def, flags + 1);
            func->def = def;
            for (int32_t i = 0; i < len; i++) {
                data = unmarshal_one_env(st, data, &(func->envs[i]), flags + 1);
            }
            return data;
        }
        case LB_ABSTRACT: {
            data++;
            return unmarshal_one_abstract(st, data, out, flags);
        }
        case LB_REFERENCE:
        case LB_ARRAY:
        case LB_ARRAY_WEAK:
        case LB_TUPLE:
        case LB_STRUCT:
        case LB_STRUCT_PROTO:
        case LB_TABLE:
        case LB_TABLE_PROTO:
        case LB_TABLE_WEAKK:
        case LB_TABLE_WEAKV:
        case LB_TABLE_WEAKKV:
        case LB_TABLE_WEAKK_PROTO:
        case LB_TABLE_WEAKV_PROTO:
        case LB_TABLE_WEAKKV_PROTO:
            /* Things that open with integers */
        {
            data++;
            int32_t len = readnat(st, &data);
            /* DOS check */
            if (lead != LB_REFERENCE) {
                MARSH_EOS(st, data - 1 + len);
            }
            if (lead == LB_ARRAY || lead == LB_ARRAY_WEAK) {
                /* Array */
                BenderArray *array = (lead == LB_ARRAY_WEAK) ? bender_array_weak(len) : bender_array(len);
                array->count = len;
                *out = bender_wrap_array(array);
                bender_v_push(st->lookup, *out);
                for (int32_t i = 0; i < len; i++) {
                    data = unmarshal_one(st, data, array->data + i, flags + 1);
                }
            } else if (lead == LB_TUPLE) {
                /* Tuple */
                Bender *tup = bender_tuple_begin(len);
                int32_t flag = readint(st, &data);
                bender_tuple_flag(tup) |= (int32_t)(((uint32_t) flag) << 16);  /* Avoid left shift of negative value */
                bender_tuple_head(tup)->sm_line = readint(st, &data);
                bender_tuple_head(tup)->sm_column = readint(st, &data);
                for (int32_t i = 0; i < len; i++) {
                    data = unmarshal_one(st, data, tup + i, flags + 1);
                }
                *out = bender_wrap_tuple(bender_tuple_end(tup));
                bender_v_push(st->lookup, *out);
            } else if (lead == LB_STRUCT || lead == LB_STRUCT_PROTO) {
                /* Struct */
                BenderKV *struct_ = bender_struct_begin(len);
                if (lead == LB_STRUCT_PROTO) {
                    Bender proto;
                    data = unmarshal_one(st, data, &proto, flags + 1);
                    bender_asserttype(proto, BENDER_STRUCT, st);
                    bender_struct_proto(struct_) = bender_unwrap_struct(proto);
                }
                for (int32_t i = 0; i < len; i++) {
                    Bender key, value;
                    data = unmarshal_one(st, data, &key, flags + 1);
                    data = unmarshal_one(st, data, &value, flags + 1);
                    bender_struct_put(struct_, key, value);
                }
                *out = bender_wrap_struct(bender_struct_end(struct_));
                bender_v_push(st->lookup, *out);
            } else if (lead == LB_REFERENCE) {
                if (len >= bender_v_count(st->lookup))
                    bender_panicf("invalid reference %d", len);
                *out = st->lookup[len];
            } else {
                /* Table */
                BenderTable *t;
                if (lead == LB_TABLE_WEAKK_PROTO || lead == LB_TABLE_WEAKK) {
                    t = bender_table_weakk(len);
                } else if (lead == LB_TABLE_WEAKV_PROTO || lead == LB_TABLE_WEAKV) {
                    t = bender_table_weakv(len);
                } else if (lead == LB_TABLE_WEAKKV_PROTO || lead == LB_TABLE_WEAKKV) {
                    t = bender_table_weakkv(len);
                } else {
                    t = bender_table(len);
                }
                *out = bender_wrap_table(t);
                bender_v_push(st->lookup, *out);
                if (lead == LB_TABLE_PROTO || lead == LB_TABLE_WEAKK_PROTO || lead == LB_TABLE_WEAKV_PROTO || lead == LB_TABLE_WEAKKV_PROTO) {
                    Bender proto;
                    data = unmarshal_one(st, data, &proto, flags + 1);
                    bender_asserttype(proto, BENDER_TABLE, st);
                    t->proto = bender_unwrap_table(proto);
                }
                for (int32_t i = 0; i < len; i++) {
                    Bender key, value;
                    data = unmarshal_one(st, data, &key, flags + 1);
                    data = unmarshal_one(st, data, &value, flags + 1);
                    bender_table_put(t, key, value);
                }
            }
            return data;
        }
        case LB_UNSAFE_POINTER: {
            MARSH_EOS(st, data + sizeof(void *));
            data++;
            if (!(flags & BENDER_MARSHAL_UNSAFE)) {
                bender_panicf("unsafe flag not given, "
                             "will not unmarshal raw pointer at index %d",
                             (int)(data - st->start));
            }
            union {
                void *ptr;
                uint8_t bytes[sizeof(void *)];
            } u;
            memcpy(u.bytes, data, sizeof(void *));
            data += sizeof(void *);
            *out = bender_wrap_pointer(u.ptr);
            bender_v_push(st->lookup, *out);
            return data;
        }
#ifdef BENDER_EV
        case LB_POINTER_BUFFER: {
            data++;
            int32_t count = readnat(st, &data);
            int32_t capacity = readnat(st, &data);
            MARSH_EOS(st, data + sizeof(void *));
            union {
                void *ptr;
                uint8_t bytes[sizeof(void *)];
            } u;
            if (!(flags & BENDER_MARSHAL_UNSAFE)) {
                bender_panicf("unsafe flag not given, "
                             "will not unmarshal raw pointer at index %d",
                             (int)(data - st->start));
            }
            memcpy(u.bytes, data, sizeof(void *));
            data += sizeof(void *);
            BenderBuffer *buffer = bender_pointer_buffer_unsafe(u.ptr, capacity, count);
            *out = bender_wrap_buffer(buffer);
            bender_v_push(st->lookup, *out);
            return data;
        }
#endif
        case LB_UNSAFE_CFUNCTION: {
            MARSH_EOS(st, data + sizeof(BenderCFunction));
            data++;
            if (!(flags & BENDER_MARSHAL_UNSAFE)) {
                bender_panicf("unsafe flag not given, "
                             "will not unmarshal function pointer at index %d",
                             (int)(data - st->start));
            }
            union {
                BenderCFunction ptr;
                uint8_t bytes[sizeof(BenderCFunction)];
            } u;
            memcpy(u.bytes, data, sizeof(BenderCFunction));
            data += sizeof(BenderCFunction);
            *out = bender_wrap_cfunction(u.ptr);
            bender_v_push(st->lookup, *out);
            return data;
        }
#ifdef BENDER_EV
        case LB_THREADED_ABSTRACT: {
            MARSH_EOS(st, data + sizeof(void *));
            data++;
            if (!(flags & BENDER_MARSHAL_UNSAFE)) {
                bender_panicf("unsafe flag not given, "
                             "will not unmarshal threaded abstract pointer at index %d",
                             (int)(data - st->start));
            }
            union {
                void *ptr;
                uint8_t bytes[sizeof(void *)];
            } u;
            memcpy(u.bytes, data, sizeof(void *));
            data += sizeof(void *);

            if (flags & BENDER_MARSHAL_DECREF) {
                /* Decrement immediately and don't bother putting into heap */
                bender_abstract_decref(u.ptr);
                *out = bender_wrap_nil();
            } else {
                *out = bender_wrap_abstract(u.ptr);
                Bender check = bender_table_get(&bender_vm.threaded_abstracts, *out);
                if (bender_checktype(check, BENDER_NIL)) {
                    /* Transfers reference from threaded channel buffer to current heap */
                    bender_table_put_raw(&bender_vm.threaded_abstracts, *out, bender_wrap_false());
                } else {
                    /* Heap reference already accounted for, remove threaded channel reference. */
                    bender_abstract_decref(u.ptr);
                }
            }

            bender_v_push(st->lookup, *out);
            return data;
        }
#endif
        default: {
            bender_panicf("unknown byte %x at index %d",
                         *data,
                         (int)(data - st->start));
            return NULL;
        }
    }
}

Bender bender_unmarshal(
    const uint8_t *bytes,
    size_t len,
    int flags,
    BenderTable *reg,
    const uint8_t **next) {
    UnmarshalState st;
    st.start = bytes;
    st.end = bytes + len;
    st.lookup_defs = NULL;
    st.lookup_envs = NULL;
    st.lookup = NULL;
    st.reg = reg;
    Bender out = bender_wrap_nil();
    /* Phase 2a root discipline: suspend collections for the whole
     * operation. The lookup vectors hold raw FuncDef/FuncEnv pointers and
     * half-built values that cannot be rooted individually; a collection
     * mid-operation could sweep them (rebuilt objects then dangle). The
     * lock is released on all paths; a panic inside re-raises after
     * unlocking (same signal as without). */
    int ulock = bender_gclock();
    BenderTryState ustate;
    BenderSignal usig = bender_try(&ustate);
    const uint8_t *nextbytes = NULL;
    if (!usig) {
        nextbytes = unmarshal_one(&st, bytes, &out, flags);
    }
    bender_v_free(st.lookup_defs);
    bender_v_free(st.lookup_envs);
    bender_v_free(st.lookup);
    /* Same ordering discipline as bender_marshal: unlock AFTER the
     * final restore (the restore re-applies the try-time gc_suspend,
     * which was captured after the lock), and before the re-raise on
     * the signal path. Unlock-then-restore leaked +1 per call. */
    if (usig) {
        bender_restore(&ustate);
        bender_gcunlock(ulock);
        if (usig == BENDER_SIGNAL_ERROR) {
            bender_panicv(ustate.payload);
        } else {
            bender_signalv(usig, ustate.payload);
        }
    }
    bender_restore(&ustate);
    bender_gcunlock(ulock);
    if (next) *next = nextbytes;
    return out;
}

/* C functions */

BENDER_CORE_FN(cfun_env_lookup,
              "(env-lookup env)",
              "Creates a forward lookup table for unmarshalling from an "
              "environment `env`. To create a reverse lookup table, use "
              "`invert` to swap keys and values in the returned table.") {
    bender_fixarity(argc, 1);
    BenderTable *env = bender_gettable(argv, 0);
    return bender_wrap_table(bender_env_lookup(env));
}

BENDER_CORE_FN(cfun_marshal,
              "(marshal x &opt rev-lookup buf no-cycles)",
              "Marshal a value, `x`, into a buffer and return the buffer. "
              "The buffer can then later be unmarshalled to reconstruct the "
              "initial value. Optionally, pass in a table, `rev-lookup`, to "
              "specify aliased values to not marshal. Then a forward lookup "
              "table can be used to recover the original value when "
              "unmarshalling. If optional argument `buf` is provided, "
              "marshal into and return that instead. The optional "
              "`no-cycles` parameter will make `unmarshal` faster if it is "
              "known that there are no cycles; will error if there are "
              "cycles.") {
    bender_arity(argc, 1, 4);
    BenderBuffer *buffer;
    BenderTable *rreg = NULL;
    uint32_t flags = 0;
    if (argc > 1) {
        rreg = bender_gettable(argv, 1);
    }
    if (argc > 2) {
        buffer = bender_getbuffer(argv, 2);
    } else {
        buffer = bender_buffer(10);
    }
    if (argc > 3 && bender_truthy(argv[3])) {
        flags |= BENDER_MARSHAL_NO_CYCLES;
    }
    bender_marshal(buffer, argv[0], rreg, flags);
    return bender_wrap_buffer(buffer);
}

BENDER_CORE_FN(cfun_unmarshal,
              "(unmarshal buf &opt lookup)",
              "Unmarshal a value from a buffer, `buf`. Optionally provide "
              "a table, `lookup`, to allow for aliases to be resolved. "
              "Returns the value unmarshalled from `buf`.") {
    bender_sandbox_assert(BENDER_SANDBOX_UNMARSHAL);
    bender_arity(argc, 1, 2);
    BenderByteView view = bender_getbytes(argv, 0);
    BenderTable *reg = NULL;
    if (argc > 1) {
        reg = bender_gettable(argv, 1);
    }
    return bender_unmarshal(view.bytes, (size_t) view.len, 0, reg, NULL);
}

/* Module entry point */
void bender_lib_marsh(BenderTable *env) {
    BenderRegExt marsh_cfuns[] = {
        BENDER_CORE_REG("marshal", cfun_marshal),
        BENDER_CORE_REG("unmarshal", cfun_unmarshal),
        BENDER_CORE_REG("env-lookup", cfun_env_lookup),
        BENDER_REG_END
    };
    bender_core_cfuns_ext(env, NULL, marsh_cfuns);
}
