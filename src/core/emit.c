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
#include "emit.h"
#include "vector.h"
#include "regalloc.h"
#include "util.h"
#endif

/* Get a register */
int32_t benderc_allocfar(BenderCompiler *c) {
    int32_t reg = benderc_regalloc_1(&c->scope->ra);
    if (reg > 0xFFFF) {
        benderc_cerror(c, "ran out of internal registers");
    }
    return reg;
}

/* Get a register less than 256 for temporary use. */
int32_t benderc_allocnear(BenderCompiler *c, BendercRegisterTemp tag) {
    return benderc_regalloc_temp(&c->scope->ra, tag);
}

/* Emit a raw instruction with source mapping. */
void benderc_emit(BenderCompiler *c, uint32_t instr) {
    bender_v_push(c->buffer, instr);
    bender_v_push(c->mapbuffer, c->current_mapping);
}

/* Add a constant to the current scope. Return the index of the constant. */
static int32_t benderc_const(BenderCompiler *c, Bender x) {
    BenderScope *scope = c->scope;
    int32_t i, len;
    /* Get the topmost function scope */
    while (scope) {
        if (scope->flags & BENDER_SCOPE_FUNCTION)
            break;
        scope = scope->parent;
    }
    /* Check if already added */
    len = bender_v_count(scope->consts);
    for (i = 0; i < len; i++) {
        if (bender_equals(x, scope->consts[i]))
            return i;
    }
    /* Ensure not too many constants. */
    if (len >= 0xFFFF) {
        benderc_cerror(c, "too many constants");
        return 0;
    }
    bender_v_push(scope->consts, x);
    return len;
}

/* Load a constant into a local register */
static void benderc_loadconst(BenderCompiler *c, Bender k, int32_t reg) {
    switch (bender_type(k)) {
        case BENDER_NIL:
            benderc_emit(c, ((uint32_t) reg << 8) | JOP_LOAD_NIL);
            break;
        case BENDER_BOOLEAN:
            benderc_emit(c, ((uint32_t) reg << 8) |
                        (bender_unwrap_boolean(k) ? JOP_LOAD_TRUE : JOP_LOAD_FALSE));
            break;
        case BENDER_NUMBER: {
            double dval = bender_unwrap_number(k);
            if (dval < INT16_MIN || dval > INT16_MAX)
                goto do_constant;
            int32_t i = (int32_t) dval;
            if (dval != i)
                goto do_constant;
            uint32_t iu = (uint32_t)i;
            benderc_emit(c,
                        ((uint32_t) iu << 16) |
                        ((uint32_t) reg << 8) |
                        JOP_LOAD_INTEGER);
            break;
        }
        default:
        do_constant: {
                int32_t cindex = benderc_const(c, k);
                benderc_emit(c,
                            ((uint32_t) cindex << 16) |
                            ((uint32_t) reg << 8) |
                            JOP_LOAD_CONSTANT);
                break;
            }
    }
}

/* Move a slot to a near register */
static void benderc_movenear(BenderCompiler *c,
                            int32_t dest,
                            BenderSlot src) {
    bender_assert(dest <= 255, "dest slot index must be <= 255");
    if (src.flags & (BENDER_SLOT_CONSTANT | BENDER_SLOT_REF)) {
        benderc_loadconst(c, src.constant, dest);
        /* If we also are a reference, deref the one element array */
        if (src.flags & BENDER_SLOT_REF) {
            benderc_emit(c,
                        ((uint32_t) dest << 16) |
                        ((uint32_t) dest << 8) |
                        JOP_GET_INDEX);
        }
    } else if (src.envindex >= 0) {
        if (src.index > 0xFF) {
            benderc_cerror(c, "captured upvalue slot exceeds bytecode limit 255");
            return;
        }
        if (src.envindex > 0xFF) {
            benderc_cerror(c, "upvalue environment index exceeds bytecode limit 255");
            return;
        }
        benderc_emit(c,
                    ((uint32_t)(src.index) << 24) |
                    ((uint32_t)(src.envindex) << 16) |
                    ((uint32_t)(dest) << 8) |
                    JOP_LOAD_UPVALUE);
    } else if (src.index != dest) {
        bender_assert(src.index >= 0, "bad slot");
        benderc_emit(c,
                    ((uint32_t)(src.index) << 16) |
                    ((uint32_t)(dest) << 8) |
                    JOP_MOVE_NEAR);
    }
}

/* Move a near or far register to a Slot. */
static void benderc_moveback(BenderCompiler *c,
                            BenderSlot dest,
                            int32_t src) {
    if (dest.flags & BENDER_SLOT_REF) {
        int32_t refreg = benderc_regalloc_temp(&c->scope->ra, BENDERC_REGTEMP_5);
        benderc_loadconst(c, dest.constant, refreg);
        benderc_emit(c,
                    ((uint32_t) src << 16) |
                    ((uint32_t) refreg << 8) |
                    JOP_PUT_INDEX);
        benderc_regalloc_freetemp(&c->scope->ra, refreg, BENDERC_REGTEMP_5);
    } else if (dest.envindex >= 0) {
        if (dest.index > 0xFF) {
            benderc_cerror(c, "captured upvalue slot exceeds bytecode limit 255");
            return;
        }
        if (dest.envindex > 0xFF) {
            benderc_cerror(c, "upvalue environment index exceeds bytecode limit 255");
            return;
        }
        /* Convert src to near reg */
        if (src > 255) {
            int32_t newsrc = BENDERC_REGTEMP_5 + 0xF0;
            benderc_emit(c, JOP_MOVE_NEAR | ((uint32_t)(src) << 16) | ((uint32_t) newsrc << 8));
            src = newsrc;
        }
        benderc_emit(c,
                    ((uint32_t)(dest.index) << 24) |
                    ((uint32_t)(dest.envindex) << 16) |
                    ((uint32_t)(src) << 8) |
                    JOP_SET_UPVALUE);
    } else if (dest.index != src) {
        bender_assert(dest.index >= 0, "bad slot");
        /* Convert src to near reg */
        if (src > 255) {
            int32_t newsrc = BENDERC_REGTEMP_5 + 0xF0;
            benderc_emit(c, JOP_MOVE_NEAR | ((uint32_t)(src) << 16) | ((uint32_t)newsrc << 8));
            src = newsrc;
        }
        benderc_emit(c,
                    ((uint32_t)(dest.index) << 16) |
                    ((uint32_t)(src) << 8) |
                    JOP_MOVE_FAR);
    }
}

/* Call this to release a register after emitting the instruction. */
static void benderc_free_regnear(BenderCompiler *c, BenderSlot s, int32_t reg, BendercRegisterTemp tag) {
    if (reg != s.index ||
            s.envindex >= 0 ||
            s.flags & (BENDER_SLOT_CONSTANT | BENDER_SLOT_REF)) {
        /* We need to free the temporary slot */
        benderc_regalloc_freetemp(&c->scope->ra, reg, tag);
    }
}

/* Convert a slot to a two byte register */
static int32_t benderc_regfar(BenderCompiler *c, BenderSlot s, BendercRegisterTemp tag) {
    /* check if already near register */
    if (s.envindex < 0 && s.index >= 0) {
        return s.index;
    }
    int32_t reg;
    int32_t nearreg = benderc_regalloc_temp(&c->scope->ra, tag);
    benderc_movenear(c, nearreg, s);
    if (nearreg >= 0xF0) {
        reg = benderc_allocfar(c);
        benderc_emit(c, JOP_MOVE_FAR | ((uint32_t) nearreg << 8) | ((uint32_t) reg << 16));
        benderc_regalloc_freetemp(&c->scope->ra, nearreg, tag);
    } else {
        reg = nearreg;
        benderc_regalloc_freetemp(&c->scope->ra, nearreg, tag);
        benderc_regalloc_touch(&c->scope->ra, reg);
    }
    return reg;
}

/* Convert a slot to a temporary 1 byte register */
static int32_t benderc_regnear(BenderCompiler *c, BenderSlot s, BendercRegisterTemp tag) {
    /* check if already near register */
    if (s.envindex < 0 && s.index >= 0 && s.index <= 0xFF) {
        return s.index;
    }
    int32_t reg = benderc_regalloc_temp(&c->scope->ra, tag);
    benderc_movenear(c, reg, s);
    return reg;
}

/* Check if two slots are equal */
int benderc_sequal(BenderSlot lhs, BenderSlot rhs) {
    if ((lhs.flags & ~BENDER_SLOTTYPE_ANY) == (rhs.flags & ~BENDER_SLOTTYPE_ANY) &&
            lhs.index == rhs.index &&
            lhs.envindex == rhs.envindex) {
        if (lhs.flags & (BENDER_SLOT_REF | BENDER_SLOT_CONSTANT)) {
            return bender_equals(lhs.constant, rhs.constant);
        } else {
            return 1;
        }
    }
    return 0;
}

/* Move values from one slot to another. The destination must
 * be writeable (not a literal). */
void benderc_copy(
    BenderCompiler *c,
    BenderSlot dest,
    BenderSlot src) {
    if (dest.flags & BENDER_SLOT_CONSTANT) {
        benderc_cerror(c, "cannot write to constant");
        return;
    }
    if (benderc_sequal(dest, src)) return;
    /* If dest is a near register */
    if (dest.envindex < 0 && dest.index >= 0 && dest.index <= 0xFF) {
        benderc_movenear(c, dest.index, src);
        return;
    }
    /* If src is a near register */
    if (src.envindex < 0 && src.index >= 0 && src.index <= 0xFF) {
        benderc_moveback(c, dest, src.index);
        return;
    }
    /* Process: src -> near -> dest */
    int32_t nearreg = benderc_allocnear(c, BENDERC_REGTEMP_3);
    benderc_movenear(c, nearreg, src);
    benderc_moveback(c, dest, nearreg);
    /* Cleanup */
    benderc_regalloc_freetemp(&c->scope->ra, nearreg, BENDERC_REGTEMP_3);
}

/* Instruction templated emitters */

static int32_t emit1s(BenderCompiler *c, uint8_t op, BenderSlot s, int32_t rest, int wr) {
    int32_t reg = benderc_regnear(c, s, BENDERC_REGTEMP_0);
    int32_t label = bender_v_count(c->buffer);
    benderc_emit(c, op | ((uint32_t) reg << 8) | ((uint32_t) rest << 16));
    if (wr)
        benderc_moveback(c, s, reg);
    benderc_free_regnear(c, s, reg, BENDERC_REGTEMP_0);
    return label;
}

int32_t benderc_emit_s(BenderCompiler *c, uint8_t op, BenderSlot s, int wr) {
    int32_t reg = benderc_regfar(c, s, BENDERC_REGTEMP_0);
    int32_t label = bender_v_count(c->buffer);
    benderc_emit(c, op | ((uint32_t) reg << 8));
    if (wr)
        benderc_moveback(c, s, reg);
    benderc_free_regnear(c, s, reg, BENDERC_REGTEMP_0);
    return label;
}

int32_t benderc_emit_sl(BenderCompiler *c, uint8_t op, BenderSlot s, int32_t label) {
    int32_t current = bender_v_count(c->buffer) - 1;
    int32_t jump = label - current;
    if (jump < INT16_MIN || jump > INT16_MAX) {
        benderc_cerror(c, "jump is too far");
    }
    return emit1s(c, op, s, jump, 0);
}

int32_t benderc_emit_st(BenderCompiler *c, uint8_t op, BenderSlot s, int32_t tflags) {
    return emit1s(c, op, s, tflags, 0);
}

int32_t benderc_emit_si(BenderCompiler *c, uint8_t op, BenderSlot s, int16_t immediate, int wr) {
    return emit1s(c, op, s, immediate, wr);
}

int32_t benderc_emit_su(BenderCompiler *c, uint8_t op, BenderSlot s, uint16_t immediate, int wr) {
    return emit1s(c, op, s, (int32_t) immediate, wr);
}

static int32_t emit2s(BenderCompiler *c, uint8_t op, BenderSlot s1, BenderSlot s2, int32_t rest, int wr) {
    int32_t reg1 = benderc_regnear(c, s1, BENDERC_REGTEMP_0);
    int32_t reg2 = benderc_regnear(c, s2, BENDERC_REGTEMP_1);
    int32_t label = bender_v_count(c->buffer);
    benderc_emit(c, op | ((uint32_t) reg1 << 8) | ((uint32_t) reg2 << 16) | ((uint32_t)rest << 24));
    benderc_free_regnear(c, s2, reg2, BENDERC_REGTEMP_1);
    if (wr)
        benderc_moveback(c, s1, reg1);
    benderc_free_regnear(c, s1, reg1, BENDERC_REGTEMP_0);
    return label;
}

int32_t benderc_emit_ss(BenderCompiler *c, uint8_t op, BenderSlot s1, BenderSlot s2, int wr) {
    int32_t reg1 = benderc_regnear(c, s1, BENDERC_REGTEMP_0);
    int32_t reg2 = benderc_regfar(c, s2, BENDERC_REGTEMP_1);
    int32_t label = bender_v_count(c->buffer);
    benderc_emit(c, op | ((uint32_t) reg1 << 8) | ((uint32_t) reg2 << 16));
    benderc_free_regnear(c, s2, reg2, BENDERC_REGTEMP_1);
    if (wr)
        benderc_moveback(c, s1, reg1);
    benderc_free_regnear(c, s1, reg1, BENDERC_REGTEMP_0);
    return label;
}

int32_t benderc_emit_ssi(BenderCompiler *c, uint8_t op, BenderSlot s1, BenderSlot s2, int8_t immediate, int wr) {
    return emit2s(c, op, s1, s2, immediate, wr);
}

int32_t benderc_emit_ssu(BenderCompiler *c, uint8_t op, BenderSlot s1, BenderSlot s2, uint8_t immediate, int wr) {
    return emit2s(c, op, s1, s2, (int32_t) immediate, wr);
}

int32_t benderc_emit_sss(BenderCompiler *c, uint8_t op, BenderSlot s1, BenderSlot s2, BenderSlot s3, int wr) {
    int32_t reg1 = benderc_regnear(c, s1, BENDERC_REGTEMP_0);
    int32_t reg2 = benderc_regnear(c, s2, BENDERC_REGTEMP_1);
    int32_t reg3 = benderc_regnear(c, s3, BENDERC_REGTEMP_2);
    int32_t label = bender_v_count(c->buffer);
    benderc_emit(c, op | ((uint32_t) reg1 << 8) | ((uint32_t) reg2 << 16) | ((uint32_t) reg3 << 24));
    benderc_free_regnear(c, s2, reg2, BENDERC_REGTEMP_1);
    benderc_free_regnear(c, s3, reg3, BENDERC_REGTEMP_2);
    if (wr)
        benderc_moveback(c, s1, reg1);
    benderc_free_regnear(c, s1, reg1, BENDERC_REGTEMP_0);
    return label;
}
