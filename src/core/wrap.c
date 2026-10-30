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
#include <math.h>
#include "util.h"
#include "state.h"
#endif

/* Macro fills */

BenderType(bender_type)(Bender x) {
    return bender_type(x);
}
int (bender_checktype)(Bender x, BenderType type) {
    return bender_checktype(x, type);
}
int (bender_checktypes)(Bender x, int typeflags) {
    return bender_checktypes(x, typeflags);
}
int (bender_truthy)(Bender x) {
    return bender_truthy(x);
}

BenderStruct(bender_unwrap_struct)(Bender x) {
    return bender_unwrap_struct(x);
}
BenderTuple(bender_unwrap_tuple)(Bender x) {
    return bender_unwrap_tuple(x);
}
BenderFiber *(bender_unwrap_fiber)(Bender x) {
    return bender_unwrap_fiber(x);
}
BenderArray *(bender_unwrap_array)(Bender x) {
    return bender_unwrap_array(x);
}
BenderTable *(bender_unwrap_table)(Bender x) {
    return bender_unwrap_table(x);
}
BenderBuffer *(bender_unwrap_buffer)(Bender x) {
    return bender_unwrap_buffer(x);
}
BenderString(bender_unwrap_string)(Bender x) {
    return bender_unwrap_string(x);
}
BenderSymbol(bender_unwrap_symbol)(Bender x) {
    return bender_unwrap_symbol(x);
}
BenderKeyword(bender_unwrap_keyword)(Bender x) {
    return bender_unwrap_keyword(x);
}
BenderAbstract(bender_unwrap_abstract)(Bender x) {
    return bender_unwrap_abstract(x);
}
void *(bender_unwrap_pointer)(Bender x) {
    return bender_unwrap_pointer(x);
}
BenderFunction *(bender_unwrap_function)(Bender x) {
    return bender_unwrap_function(x);
}
BenderCFunction(bender_unwrap_cfunction)(Bender x) {
    return bender_unwrap_cfunction(x);
}
int (bender_unwrap_boolean)(Bender x) {
    return bender_unwrap_boolean(x);
}
int32_t (bender_unwrap_integer)(Bender x) {
    return bender_unwrap_integer(x);
}

#if defined(BENDER_NANBOX_32) || defined(BENDER_NANBOX_64)
Bender(bender_wrap_nil)(void) {
    return bender_wrap_nil();
}
Bender(bender_wrap_true)(void) {
    return bender_wrap_true();
}
Bender(bender_wrap_false)(void) {
    return bender_wrap_false();
}
Bender(bender_wrap_boolean)(int x) {
    return bender_wrap_boolean(x);
}
Bender(bender_wrap_string)(BenderString x) {
    return bender_wrap_string(x);
}
Bender(bender_wrap_symbol)(BenderSymbol x) {
    return bender_wrap_symbol(x);
}
Bender(bender_wrap_keyword)(BenderKeyword x) {
    return bender_wrap_keyword(x);
}
Bender(bender_wrap_array)(BenderArray *x) {
    return bender_wrap_array(x);
}
Bender(bender_wrap_tuple)(BenderTuple x) {
    return bender_wrap_tuple(x);
}
Bender(bender_wrap_struct)(BenderStruct x) {
    return bender_wrap_struct(x);
}
Bender(bender_wrap_fiber)(BenderFiber *x) {
    return bender_wrap_fiber(x);
}
Bender(bender_wrap_buffer)(BenderBuffer *x) {
    return bender_wrap_buffer(x);
}
Bender(bender_wrap_function)(BenderFunction *x) {
    return bender_wrap_function(x);
}
Bender(bender_wrap_cfunction)(BenderCFunction x) {
    return bender_wrap_cfunction(x);
}
Bender(bender_wrap_table)(BenderTable *x) {
    return bender_wrap_table(x);
}
Bender(bender_wrap_abstract)(BenderAbstract x) {
    return bender_wrap_abstract(x);
}
Bender(bender_wrap_pointer)(void *x) {
    return bender_wrap_pointer(x);
}
Bender(bender_wrap_integer)(int32_t x) {
    return bender_wrap_integer(x);
}
#endif

#ifndef BENDER_NANBOX_32
double (bender_unwrap_number)(Bender x) {
    return bender_unwrap_number(x);
}
#endif

#ifdef BENDER_NANBOX_64
Bender(bender_wrap_number)(double x) {
    return bender_wrap_number(x);
}
#endif

/*****/

void *bender_memalloc_empty(int32_t count) {
    int32_t i;
    void *mem = array_allocate(sizeof(BenderKV), count);
    bender_vm.next_collection += (size_t) count * sizeof(BenderKV);
    if (NULL == mem) {
        BENDER_OUT_OF_MEMORY;
    }
    BenderKV *mmem = (BenderKV *)mem;
    for (i = 0; i < count; i++) {
        BenderKV *kv = mmem + i;
        kv->key = bender_wrap_nil();
        kv->value = bender_wrap_nil();
    }
    return mem;
}

void bender_memempty(BenderKV *mem, int32_t count) {
    int32_t i;
    for (i = 0; i < count; i++) {
        mem[i].key = bender_wrap_nil();
        mem[i].value = bender_wrap_nil();
    }
}

#ifdef BENDER_NANBOX_64

Bender bender_wrap_number_safe(double d) {
    Bender ret;
    ret.number = isnan(d) ? NAN : d;
    return ret;
}

void *bender_nanbox_to_pointer(Bender x) {
    x.i64 &= BENDER_NANBOX_PAYLOADBITS;
    x.u64 <<= BENDER_NANBOX_64_POINTER_SHIFT; /* Alignment, usually 0 */
    return x.pointer;
}

Bender bender_nanbox_from_pointer(void *p, uint64_t tagmask) {
    Bender ret;
    ret.pointer = p;
    /* Should be noop when pointer shift is 0 */
    /*
    bender_assert(!(ret.u64 & (uint64_t) ((1 << BENDER_NANBOX_64_POINTER_SHIFT) - 1)), "unaligned pointer wrap");
    */
    ret.u64 >>= BENDER_NANBOX_64_POINTER_SHIFT; /* Alignment, usually 0 */
    ret.u64 |= tagmask;
    return ret;
}

Bender bender_nanbox_from_cpointer(const void *p, uint64_t tagmask) {
    Bender ret;
    ret.pointer = (void *)p;
    /* Should be noop when pointer shift is 0 */
    /*
    bender_assert(!(ret.u64 & (uint64_t) ((1 << BENDER_NANBOX_64_POINTER_SHIFT) - 1)), "unaligned pointer wrap");
    */
    ret.u64 >>= BENDER_NANBOX_64_POINTER_SHIFT; /* Alignment, usually 0 */
    ret.u64 |= tagmask;
    return ret;
}

Bender bender_nanbox_from_double(double d) {
    Bender ret;
    ret.number = d;
    return ret;
}

Bender bender_nanbox_from_bits(uint64_t bits) {
    Bender ret;
    ret.u64 = bits;
    return ret;
}

#elif defined(BENDER_NANBOX_32)

Bender bender_wrap_number(double x) {
    Bender ret;
    ret.number = x;
    ret.tagged.type += BENDER_DOUBLE_OFFSET;
    return ret;
}

Bender bender_wrap_number_safe(double d) {
    double x = isnan(d) ? NAN : d;
    return bender_wrap_number(x);
}

Bender bender_nanbox32_from_tagi(uint32_t tag, int32_t integer) {
    Bender ret;
    ret.tagged.type = tag;
    ret.tagged.payload.integer = integer;
    return ret;
}

Bender bender_nanbox32_from_tagp(uint32_t tag, void *pointer) {
    Bender ret;
    ret.tagged.type = tag;
    ret.tagged.payload.pointer = pointer;
    return ret;
}

double bender_unwrap_number(Bender x) {
    x.tagged.type -= BENDER_DOUBLE_OFFSET;
    return x.number;
}

#else

Bender bender_wrap_number_safe(double d) {
    return bender_wrap_number(d);
}

Bender bender_wrap_nil(void) {
    Bender y;
    y.type = BENDER_NIL;
    y.as.u64 = 0;
    return y;
}

Bender bender_wrap_true(void) {
    Bender y;
    y.type = BENDER_BOOLEAN;
    y.as.u64 = 1;
    return y;
}

Bender bender_wrap_false(void) {
    Bender y;
    y.type = BENDER_BOOLEAN;
    y.as.u64 = 0;
    return y;
}

Bender bender_wrap_boolean(int x) {
    Bender y;
    y.type = BENDER_BOOLEAN;
    y.as.u64 = !!x;
    return y;
}

#define BENDER_WRAP_DEFINE(NAME, TYPE, DTYPE, UM)\
Bender bender_wrap_##NAME(TYPE x) {\
    Bender y;\
    y.type = DTYPE;\
    y.as.u64 = 0; /* zero other bits in case of 32 bit integer */ \
    y.as.UM = x;\
    return y;\
}

BENDER_WRAP_DEFINE(number, double, BENDER_NUMBER, number)
BENDER_WRAP_DEFINE(string, const uint8_t *, BENDER_STRING, cpointer)
BENDER_WRAP_DEFINE(symbol, const uint8_t *, BENDER_SYMBOL, cpointer)
BENDER_WRAP_DEFINE(keyword, const uint8_t *, BENDER_KEYWORD, cpointer)
BENDER_WRAP_DEFINE(array, BenderArray *, BENDER_ARRAY, pointer)
BENDER_WRAP_DEFINE(tuple, const Bender *, BENDER_TUPLE, cpointer)
BENDER_WRAP_DEFINE(struct, const BenderKV *, BENDER_STRUCT, cpointer)
BENDER_WRAP_DEFINE(fiber, BenderFiber *, BENDER_FIBER, pointer)
BENDER_WRAP_DEFINE(buffer, BenderBuffer *, BENDER_BUFFER, pointer)
BENDER_WRAP_DEFINE(function, BenderFunction *, BENDER_FUNCTION, pointer)
BENDER_WRAP_DEFINE(cfunction, BenderCFunction, BENDER_CFUNCTION, pointer)
BENDER_WRAP_DEFINE(table, BenderTable *, BENDER_TABLE, pointer)
BENDER_WRAP_DEFINE(abstract, void *, BENDER_ABSTRACT, pointer)
BENDER_WRAP_DEFINE(pointer, void *, BENDER_POINTER, pointer)

#undef BENDER_WRAP_DEFINE

#endif
