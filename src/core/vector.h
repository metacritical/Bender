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

#ifndef BENDER_VECTOR_H_defined
#define BENDER_VECTOR_H_defined

#ifndef BENDER_AMALG
#include "features.h"
#include <bender.h>
#endif

/*
 * vector code modified from
 * https://github.com/nothings/stb/blob/master/stretchy_buffer.h
*/

/* This is mainly used code such as the assembler or compiler, which
 * need vector like data structures that are only garbage collected in case
 * of an error, and normally rely on malloc/free. */

#define bender_v_free(v)         (((v) != NULL) ? (bender_sfree(bender_v__raw(v)), 0) : 0)
#define bender_v_push(v, x)      (bender_v__maybegrow(v, 1), (v)[bender_v__cnt(v)++] = (x))
#define bender_v_pop(v)          (bender_v_count(v) ? bender_v__cnt(v)-- : 0)
#define bender_v_count(v)        (((v) != NULL) ? bender_v__cnt(v) : 0)
#define bender_v_last(v)         ((v)[bender_v__cnt(v) - 1])
#define bender_v_empty(v)        (((v) != NULL) ? (bender_v__cnt(v) = 0) : 0)
#define bender_v_flatten(v)      (bender_v_flattenmem((v), sizeof(*(v))))

#define bender_v__raw(v) ((int32_t *)(v) - 2)
#define bender_v__cap(v) bender_v__raw(v)[0]
#define bender_v__cnt(v) bender_v__raw(v)[1]

#define bender_v__needgrow(v, n)  ((v) == NULL || bender_v__cnt(v) + (n) >= bender_v__cap(v))
#define bender_v__maybegrow(v, n) (bender_v__needgrow((v), (n)) ? bender_v__grow((v), (n)) : 0)
#define bender_v__grow(v, n)      ((v) = bender_v_grow((v), (n), sizeof(*(v))))

/* Actual functions defined in vector.c */
void *bender_v_grow(void *v, int32_t increment, int32_t itemsize);
void *bender_v_flattenmem(void *v, int32_t itemsize);

#endif
