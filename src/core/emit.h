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

#ifndef BENDER_EMIT_H
#define BENDER_EMIT_H

#ifndef BENDER_AMALG
#include "compile.h"
#endif

void benderc_emit(BenderCompiler *c, uint32_t instr);

int32_t benderc_allocfar(BenderCompiler *c);
int32_t benderc_allocnear(BenderCompiler *c, BendercRegisterTemp);

int32_t benderc_emit_s(BenderCompiler *c, uint8_t op, BenderSlot s, int wr);
int32_t benderc_emit_sl(BenderCompiler *c, uint8_t op, BenderSlot s, int32_t label);
int32_t benderc_emit_st(BenderCompiler *c, uint8_t op, BenderSlot s, int32_t tflags);
int32_t benderc_emit_si(BenderCompiler *c, uint8_t op, BenderSlot s, int16_t immediate, int wr);
int32_t benderc_emit_su(BenderCompiler *c, uint8_t op, BenderSlot s, uint16_t immediate, int wr);
int32_t benderc_emit_ss(BenderCompiler *c, uint8_t op, BenderSlot s1, BenderSlot s2, int wr);
int32_t benderc_emit_ssi(BenderCompiler *c, uint8_t op, BenderSlot s1, BenderSlot s2, int8_t immediate, int wr);
int32_t benderc_emit_ssu(BenderCompiler *c, uint8_t op, BenderSlot s1, BenderSlot s2, uint8_t immediate, int wr);
int32_t benderc_emit_sss(BenderCompiler *c, uint8_t op, BenderSlot s1, BenderSlot s2, BenderSlot s3, int wr);

/* Check if two slots are equivalent */
int benderc_sequal(BenderSlot x, BenderSlot y);

/* Move value from one slot to another. Cannot copy to constant slots. */
void benderc_copy(BenderCompiler *c, BenderSlot dest, BenderSlot src);

#endif
