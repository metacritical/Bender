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

/* Implements a simple first fit register allocator for the compiler. */

#ifndef BENDER_REGALLOC_H
#define BENDER_REGALLOC_H

#include <stdint.h>

/* Placeholder for allocating temporary registers */
typedef enum {
    BENDERC_REGTEMP_0,
    BENDERC_REGTEMP_1,
    BENDERC_REGTEMP_2,
    BENDERC_REGTEMP_3,
    BENDERC_REGTEMP_4,
    BENDERC_REGTEMP_5,
    BENDERC_REGTEMP_6,
    BENDERC_REGTEMP_7
} BendercRegisterTemp;

typedef struct {
    uint32_t *chunks;
    int32_t count; /* number of chunks in chunks */
    int32_t capacity; /* amount allocated for chunks */
    int32_t max; /* The maximum allocated register so far */
    int32_t regtemps; /* Hold which temp. registers are allocated. */
} BendercRegisterAllocator;

void benderc_regalloc_init(BendercRegisterAllocator *ra);
void benderc_regalloc_deinit(BendercRegisterAllocator *ra);

int32_t benderc_regalloc_1(BendercRegisterAllocator *ra);
void benderc_regalloc_free(BendercRegisterAllocator *ra, int32_t reg);
int32_t benderc_regalloc_temp(BendercRegisterAllocator *ra, BendercRegisterTemp nth);
void benderc_regalloc_freetemp(BendercRegisterAllocator *ra, int32_t reg, BendercRegisterTemp nth);
void benderc_regalloc_clone(BendercRegisterAllocator *dest, BendercRegisterAllocator *src);
void benderc_regalloc_touch(BendercRegisterAllocator *ra, int32_t reg);
int benderc_regalloc_check(BendercRegisterAllocator *ra, int32_t reg);
int32_t benderc_regalloc_n(BendercRegisterAllocator *ra, int32_t n);
void benderc_regalloc_free_n(BendercRegisterAllocator *ra, int32_t loc, int32_t n);

#endif
