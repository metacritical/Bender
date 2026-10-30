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
#include "compile.h"
#include "emit.h"
#include "vector.h"
#endif

static int arity1or2(BenderFopts opts, BenderSlot *args) {
    (void) opts;
    int32_t arity = bender_v_count(args);
    return arity == 1 || arity == 2;
}
static int arity2or3(BenderFopts opts, BenderSlot *args) {
    (void) opts;
    int32_t arity = bender_v_count(args);
    return arity == 2 || arity == 3;
}
static int fixarity1(BenderFopts opts, BenderSlot *args) {
    (void) opts;
    return bender_v_count(args) == 1;
}
static int maxarity1(BenderFopts opts, BenderSlot *args) {
    (void) opts;
    return bender_v_count(args) <= 1;
}
static int minarity2(BenderFopts opts, BenderSlot *args) {
    (void) opts;
    return bender_v_count(args) >= 2;
}
static int fixarity2(BenderFopts opts, BenderSlot *args) {
    (void) opts;
    return bender_v_count(args) == 2;
}
static int fixarity3(BenderFopts opts, BenderSlot *args) {
    (void) opts;
    return bender_v_count(args) == 3;
}

/* Generic handling for $A = op $B */
static BenderSlot genericSS(BenderFopts opts, int op, BenderSlot s) {
    BenderSlot target = benderc_gettarget(opts);
    benderc_emit_ss(opts.compiler, op, target, s, 1);
    return target;
}

/* Generic handling for $A = $B op I */
static BenderSlot genericSSI(BenderFopts opts, int op, BenderSlot s, int32_t imm) {
    BenderSlot target = benderc_gettarget(opts);
    benderc_emit_ssi(opts.compiler, op, target, s, imm, 1);
    return target;
}

/* Emit an insruction that implements a form by itself. */
static BenderSlot opfunction(
    BenderFopts opts,
    BenderSlot *args,
    int op,
    Bender defaultArg2) {
    BenderCompiler *c = opts.compiler;
    int32_t len;
    len = bender_v_count(args);
    BenderSlot t;
    if (len == 1) {
        t = benderc_gettarget(opts);
        benderc_emit_sss(c, op, t, args[0], benderc_cslot(defaultArg2), 1);
        return t;
    } else {
        /* len == 2 */
        t = benderc_gettarget(opts);
        benderc_emit_sss(c, op, t, args[0], args[1], 1);
    }
    return t;
}

/* Check if a value can be coerced to an immediate value */
static int can_be_imm(Bender x, int8_t *out) {
    if (!bender_checkint(x)) return 0;
    int32_t integer = bender_unwrap_integer(x);
    if (integer > INT8_MAX || integer < INT8_MIN) return 0;
    *out = (int8_t) integer;
    return 1;
}

/* Check if a slot can be coerced to an immediate value */
static int can_slot_be_imm(BenderSlot s, int8_t *out) {
    if (!(s.flags & BENDER_SLOT_CONSTANT)) return 0;
    return can_be_imm(s.constant, out);
}

/* UNSIGNED immediate (0..255) for JINT_SSU-shaped opcodes. The C field of
 * a 32-bit instruction is bits 24-31; signed ops must reserve half that
 * range for negatives, but an index or a bitmask is never negative, so an
 * SSU opcode gets the full byte. A 16-bit immediate is NOT possible here:
 * the only 16-bit field (E, bits 16-31) overlaps the B slot, so any
 * instruction carrying two slot operands has exactly 8 bits left. */
static int can_be_uimm(Bender x, uint8_t *out) {
    if (!bender_checkint(x)) return 0;
    int32_t integer = bender_unwrap_integer(x);
    if (integer < 0 || integer > UINT8_MAX) return 0;
    *out = (uint8_t) integer;
    return 1;
}

static int can_slot_be_uimm(BenderSlot s, uint8_t *out) {
    if (!(s.flags & BENDER_SLOT_CONSTANT)) return 0;
    return can_be_uimm(s.constant, out);
}

/* Emit a series of instructions instead of a function call to a math op */
static BenderSlot opreduce(
    BenderFopts opts,
    BenderSlot *args,
    int op,
    int opim,
    Bender nullary,
    Bender unary) {
    BenderCompiler *c = opts.compiler;
    int32_t i, len;
    int8_t imm = 0;
    len = bender_v_count(args);
    BenderSlot t;
    if (len == 0) {
        return benderc_cslot(nullary);
    } else if (len == 1) {
        t = benderc_gettarget(opts);
        /* Special case subtract to be times -1 */
        if (op == JOP_SUBTRACT) {
            benderc_emit_ssi(c, JOP_MULTIPLY_IMMEDIATE, t, args[0], -1, 1);
        } else {
            benderc_emit_sss(c, op, t, benderc_cslot(unary), args[0], 1);
        }
        return t;
    }
    t = benderc_gettarget(opts);
    if (opim && can_slot_be_imm(args[1], &imm)) {
        benderc_emit_ssi(c, opim, t, args[0], imm, 1);
    } else {
        benderc_emit_sss(c, op, t, args[0], args[1], 1);
    }
    for (i = 2; i < len; i++) {
        if (opim && can_slot_be_imm(args[i], &imm)) {
            benderc_emit_ssi(c, opim, t, t, imm, 1);
        } else {
            benderc_emit_sss(c, op, t, t, args[i], 1);
        }
    }
    return t;
}

/* Function optimizers */

static BenderSlot do_propagate(BenderFopts opts, BenderSlot *args) {
    return opreduce(opts, args, JOP_PROPAGATE, 0, bender_wrap_nil(), bender_wrap_nil());
}
static BenderSlot do_error(BenderFopts opts, BenderSlot *args) {
    benderc_emit_s(opts.compiler, JOP_ERROR, args[0], 0);
    return benderc_cslot(bender_wrap_nil());
}
static BenderSlot do_debug(BenderFopts opts, BenderSlot *args) {
    (void)args;
    int32_t len = bender_v_count(args);
    BenderSlot t = benderc_gettarget(opts);
    benderc_emit_ssu(opts.compiler, JOP_SIGNAL, t,
                    (len == 1) ? args[0] : benderc_cslot(bender_wrap_nil()),
                    BENDER_SIGNAL_DEBUG,
                    1);
    return t;
}
static BenderSlot do_in(BenderFopts opts, BenderSlot *args) {
    /* Fuse a constant small-integer key into JOP_IN_IMMEDIATE. Measured:
     * load_integer -> in is the single hottest consecutive opcode pair in
     * ttfx matrix (5.55% of 4.6 BILLION dispatches), because every
     * (in arr <literal>) burned a whole dispatch plus a slot write just
     * to materialize the index. opreduce already picks the immediate form
     * whenever can_slot_be_imm succeeds, so this is purely supplying the
     * variant. Non-constant or out-of-int8-range keys fall back to
     * JOP_IN unchanged. */
    /* Unsigned gate: JOP_IN_IMMEDIATE is JINT_SSU, so the index range is
     * the full 0..255 rather than the signed 0..127 that opreduce's
     * can_slot_be_imm would allow. Negative and oversized constants fall
     * back to JOP_IN via opreduce below. */
    if (bender_v_count(args) == 2) {
        uint8_t uimm = 0;
        if (can_slot_be_uimm(args[1], &uimm)) {
            BenderCompiler *c = opts.compiler;
            BenderSlot t = benderc_gettarget(opts);
            benderc_emit_ssu(c, JOP_IN_IMMEDIATE, t, args[0], uimm, 1);
            return t;
        }
    }
    /* opim MUST stay 0 here: opreduce's gate is can_slot_be_imm, which is
     * SIGNED, so it would hand a negative constant to this unsigned
     * opcode. That is not merely imprecise, it MISCOMPILES -- (in a -1)
     * became (in a 255), silently reading a valid element instead of
     * raising. The unsigned gate above is the only path to
     * JOP_IN_IMMEDIATE; everything else takes plain JOP_IN. */
    return opreduce(opts, args, JOP_IN, 0,
                    bender_wrap_nil(), bender_wrap_nil());
}
static BenderSlot do_get(BenderFopts opts, BenderSlot *args) {
    if (bender_v_count(args) == 3) {
        BenderCompiler *c = opts.compiler;
        BenderSlot t = benderc_gettarget(opts);
        int target_is_default = benderc_sequal(t, args[2]);
        BenderSlot dflt_slot = args[2];
        if (target_is_default) {
            dflt_slot = benderc_farslot(c);
            benderc_copy(c, dflt_slot, t);
        }
        benderc_emit_sss(c, JOP_GET, t, args[0], args[1], 1);
        int32_t label = benderc_emit_si(c, JOP_JUMP_IF_NOT_NIL, t, 0, 0);
        benderc_copy(c, t, dflt_slot);
        if (target_is_default) benderc_freeslot(c, dflt_slot);
        int32_t current = bender_v_count(c->buffer);
        c->buffer[label] |= (uint32_t)(current - label) << 16;
        return t;
    } else {
        return opreduce(opts, args, JOP_GET, 0, bender_wrap_nil(), bender_wrap_nil());
    }
}
static BenderSlot do_next(BenderFopts opts, BenderSlot *args) {
    return opfunction(opts, args, JOP_NEXT, bender_wrap_nil());
}
static BenderSlot do_cmp(BenderFopts opts, BenderSlot *args) {
    return opreduce(opts, args, JOP_COMPARE, 0, bender_wrap_nil(), bender_wrap_nil());
}
static BenderSlot do_put(BenderFopts opts, BenderSlot *args) {
    /* JOP_PUT_INDEX is JINT_SSU -- its index field is UNSIGNED 0..255.
     * This used the SIGNED can_slot_be_imm + benderc_emit_ssi, which was
     * wrong in both directions:
     *   - MISCOMPILE: a literal negative index passed the signed gate and
     *     was read back as uint8, so (put arr -1 v) silently wrote index
     *     255 (growing the array to 256) instead of raising. A genuinely
     *     runtime -1 took JOP_PUT and raised correctly, so the constant
     *     and variable paths disagreed. Same root cause as the
     *     JOP_IN_IMMEDIATE bug fixed in a7b49516.
     *   - NARROW: indices 128..255 are representable but fell back to the
     *     slower JOP_PUT form for no reason.
     * Using the unsigned gate fixes both. */
    uint8_t inline_index = 0;
    if (can_slot_be_uimm(args[1], &inline_index)) {
        /* Use JOP_PUT_INDEX */
        if (opts.flags & BENDER_FOPTS_DROP) {
            benderc_emit_ssu(opts.compiler, JOP_PUT_INDEX, args[0], args[2], inline_index, 0);
            return benderc_cslot(bender_wrap_nil());
        } else {
            BenderSlot t = benderc_gettarget(opts);
            benderc_copy(opts.compiler, t, args[0]);
            benderc_emit_ssu(opts.compiler, JOP_PUT_INDEX, t, args[2], inline_index, 0);
            return t;
        }
    } else {
        /* Use JOP_PUT */
        if (opts.flags & BENDER_FOPTS_DROP) {
            benderc_emit_sss(opts.compiler, JOP_PUT, args[0], args[1], args[2], 0);
            return benderc_cslot(bender_wrap_nil());
        } else {
            BenderSlot t = benderc_gettarget(opts);
            benderc_copy(opts.compiler, t, args[0]);
            benderc_emit_sss(opts.compiler, JOP_PUT, t, args[1], args[2], 0);
            return t;
        }
    }
}
static BenderSlot do_length(BenderFopts opts, BenderSlot *args) {
    return genericSS(opts, JOP_LENGTH, args[0]);
}
static BenderSlot do_yield(BenderFopts opts, BenderSlot *args) {
    if (bender_v_count(args) == 0) {
        return genericSSI(opts, JOP_SIGNAL, benderc_cslot(bender_wrap_nil()), 3);
    } else {
        return genericSSI(opts, JOP_SIGNAL, args[0], 3);
    }
}
static BenderSlot do_resume(BenderFopts opts, BenderSlot *args) {
    return opfunction(opts, args, JOP_RESUME, bender_wrap_nil());
}
static BenderSlot do_cancel(BenderFopts opts, BenderSlot *args) {
    return opfunction(opts, args, JOP_CANCEL, bender_wrap_nil());
}
static BenderSlot do_apply(BenderFopts opts, BenderSlot *args) {
    /* Push phase */
    BenderCompiler *c = opts.compiler;
    int32_t i;
    for (i = 1; i < bender_v_count(args) - 3; i += 3)
        benderc_emit_sss(c, JOP_PUSH_3, args[i], args[i + 1], args[i + 2], 0);
    if (i == bender_v_count(args) - 3)
        benderc_emit_ss(c, JOP_PUSH_2, args[i], args[i + 1], 0);
    else if (i == bender_v_count(args) - 2)
        benderc_emit_s(c, JOP_PUSH, args[i], 0);
    /* Push array phase */
    benderc_emit_s(c, JOP_PUSH_ARRAY, bender_v_last(args), 0);
    /* Call phase */
    BenderSlot target;
    if (opts.flags & BENDER_FOPTS_TAIL) {
        benderc_emit_s(c, JOP_TAILCALL, args[0], 0);
        target = benderc_cslot(bender_wrap_nil());
        target.flags |= BENDER_SLOT_RETURNED;
    } else {
        target = benderc_gettarget(opts);
        benderc_emit_ss(c, JOP_CALL, target, args[0], 1);
    }
    return target;
}

/* Variadic operators specialization */

static BenderSlot do_add(BenderFopts opts, BenderSlot *args) {
    return opreduce(opts, args, JOP_ADD, JOP_ADD_IMMEDIATE, bender_wrap_integer(0), bender_wrap_integer(0));
}
static BenderSlot do_sub(BenderFopts opts, BenderSlot *args) {
    return opreduce(opts, args, JOP_SUBTRACT, JOP_SUBTRACT_IMMEDIATE, bender_wrap_integer(0), bender_wrap_integer(0));
}
static BenderSlot do_mul(BenderFopts opts, BenderSlot *args) {
    return opreduce(opts, args, JOP_MULTIPLY, JOP_MULTIPLY_IMMEDIATE, bender_wrap_integer(1), bender_wrap_integer(1));
}
static BenderSlot do_div(BenderFopts opts, BenderSlot *args) {
    return opreduce(opts, args, JOP_DIVIDE, JOP_DIVIDE_IMMEDIATE, bender_wrap_integer(1), bender_wrap_integer(1));
}
static BenderSlot do_divf(BenderFopts opts, BenderSlot *args) {
    return opreduce(opts, args, JOP_DIVIDE_FLOOR, 0, bender_wrap_integer(1), bender_wrap_integer(1));
}
static BenderSlot do_modulo(BenderFopts opts, BenderSlot *args) {
    return opreduce(opts, args, JOP_MODULO, 0, bender_wrap_integer(0), bender_wrap_integer(1));
}
static BenderSlot do_remainder(BenderFopts opts, BenderSlot *args) {
    return opreduce(opts, args, JOP_REMAINDER, 0, bender_wrap_integer(0), bender_wrap_integer(1));
}
static BenderSlot do_band(BenderFopts opts, BenderSlot *args) {
    /* Fuse a constant 0..255 mask into JOP_BAND_IMMEDIATE. Measured:
     * load_integer -> band is 3.38% of all dispatches in ttfx matrix,
     * because (band x 0xff) -- every limb mask -- spent a whole dispatch
     * materializing 255. UNSIGNED because 255 does not fit a signed int8,
     * which is precisely why band could not be fused alongside
     * JOP_IN_IMMEDIATE. opim stays 0 in the fallback for the same reason
     * as do_in: opreduce's gate is signed and would hand a negative mask
     * to an unsigned opcode, reading it back as 0..255 -- a silent wrong
     * answer. The unsigned gate below is the ONLY path to the opcode. */
    if (bender_v_count(args) == 2) {
        uint8_t uimm = 0;
        if (can_slot_be_uimm(args[1], &uimm)) {
            BenderCompiler *c = opts.compiler;
            BenderSlot t = benderc_gettarget(opts);
            benderc_emit_ssu(c, JOP_BAND_IMMEDIATE, t, args[0], uimm, 1);
            return t;
        }
    }
    return opreduce(opts, args, JOP_BAND, 0, bender_wrap_integer(-1), bender_wrap_integer(-1));
}
static BenderSlot do_bor(BenderFopts opts, BenderSlot *args) {
    /* Unsigned 0..255 constant operand fuses into JOP_BOR_IMMEDIATE;
     * see do_band for the full rationale (unsigned so masks above
     * 127 fit, and opim stays 0 so a negative constant can never
     * reach an unsigned opcode). */
    if (bender_v_count(args) == 2) {
        uint8_t uimm = 0;
        if (can_slot_be_uimm(args[1], &uimm)) {
            BenderCompiler *c = opts.compiler;
            BenderSlot t = benderc_gettarget(opts);
            benderc_emit_ssu(c, JOP_BOR_IMMEDIATE, t, args[0], uimm, 1);
            return t;
        }
    }
    return opreduce(opts, args, JOP_BOR, 0, bender_wrap_integer(0), bender_wrap_integer(0));
}
static BenderSlot do_bxor(BenderFopts opts, BenderSlot *args) {
    /* Unsigned 0..255 constant operand fuses into JOP_BXOR_IMMEDIATE;
     * see do_band for the full rationale (unsigned so masks above
     * 127 fit, and opim stays 0 so a negative constant can never
     * reach an unsigned opcode). */
    if (bender_v_count(args) == 2) {
        uint8_t uimm = 0;
        if (can_slot_be_uimm(args[1], &uimm)) {
            BenderCompiler *c = opts.compiler;
            BenderSlot t = benderc_gettarget(opts);
            benderc_emit_ssu(c, JOP_BXOR_IMMEDIATE, t, args[0], uimm, 1);
            return t;
        }
    }
    return opreduce(opts, args, JOP_BXOR, 0, bender_wrap_integer(0), bender_wrap_integer(0));
}
static BenderSlot do_lshift(BenderFopts opts, BenderSlot *args) {
    return opreduce(opts, args, JOP_SHIFT_LEFT, JOP_SHIFT_LEFT_IMMEDIATE, bender_wrap_integer(1), bender_wrap_integer(1));
}
static BenderSlot do_rshift(BenderFopts opts, BenderSlot *args) {
    return opreduce(opts, args, JOP_SHIFT_RIGHT, JOP_SHIFT_RIGHT_IMMEDIATE, bender_wrap_integer(1), bender_wrap_integer(1));
}
static BenderSlot do_rshiftu(BenderFopts opts, BenderSlot *args) {
    return opreduce(opts, args, JOP_SHIFT_RIGHT_UNSIGNED, JOP_SHIFT_RIGHT_UNSIGNED_IMMEDIATE, bender_wrap_integer(1), bender_wrap_integer(1));
}
static BenderSlot do_bnot(BenderFopts opts, BenderSlot *args) {
    return genericSS(opts, JOP_BNOT, args[0]);
}

/* Specialization for comparators */
static BenderSlot compreduce(
    BenderFopts opts,
    BenderSlot *args,
    int op,
    int opim,
    int invert) {
    BenderCompiler *c = opts.compiler;
    int32_t i, len;
    int8_t imm = 0;
    len = bender_v_count(args);
    int32_t *labels = NULL;
    BenderSlot t;
    if (len < 2) {
        return invert
               ? benderc_cslot(bender_wrap_false())
               : benderc_cslot(bender_wrap_true());
    }
    t = benderc_gettarget(opts);
    for (i = 1; i < len; i++) {
        if (opim && can_slot_be_imm(args[i], &imm)) {
            benderc_emit_ssi(c, opim, t, args[i - 1], imm, 1);
        } else {
            benderc_emit_sss(c, op, t, args[i - 1], args[i], 1);
        }
        if (i != (len - 1)) {
            int32_t label = benderc_emit_si(c, invert ? JOP_JUMP_IF : JOP_JUMP_IF_NOT, t, 0, 1);
            bender_v_push(labels, label);
        }
    }
    int32_t end = bender_v_count(c->buffer);
    for (i = 0; i < bender_v_count(labels); i++) {
        int32_t label = labels[i];
        c->buffer[label] |= ((uint32_t)(end - label) << 16);
    }
    bender_v_free(labels);
    return t;
}

static BenderSlot do_gt(BenderFopts opts, BenderSlot *args) {
    return compreduce(opts, args, JOP_GREATER_THAN, JOP_GREATER_THAN_IMMEDIATE, 0);
}
static BenderSlot do_lt(BenderFopts opts, BenderSlot *args) {
    return compreduce(opts, args, JOP_LESS_THAN, JOP_LESS_THAN_IMMEDIATE, 0);
}
static BenderSlot do_gte(BenderFopts opts, BenderSlot *args) {
    return compreduce(opts, args, JOP_GREATER_THAN_EQUAL, 0, 0);
}
static BenderSlot do_lte(BenderFopts opts, BenderSlot *args) {
    return compreduce(opts, args, JOP_LESS_THAN_EQUAL, 0, 0);
}
static BenderSlot do_eq(BenderFopts opts, BenderSlot *args) {
    return compreduce(opts, args, JOP_EQUALS, JOP_EQUALS_IMMEDIATE, 0);
}
static BenderSlot do_neq(BenderFopts opts, BenderSlot *args) {
    return compreduce(opts, args, JOP_NOT_EQUALS, JOP_NOT_EQUALS_IMMEDIATE, 1);
}

/* Arranged by tag */
static const BenderFunOptimizer optimizers[] = {
    {maxarity1, do_debug},
    {fixarity1, do_error},
    {minarity2, do_apply},
    {maxarity1, do_yield},
    {arity1or2, do_resume},
    {fixarity2, do_in},
    {fixarity3, do_put},
    {fixarity1, do_length},
    {NULL, do_add},
    {NULL, do_sub},
    {NULL, do_mul},
    {NULL, do_div},
    {NULL, do_band},
    {NULL, do_bor},
    {NULL, do_bxor},
    {NULL, do_lshift},
    {NULL, do_rshift},
    {NULL, do_rshiftu},
    {fixarity1, do_bnot},
    {NULL, do_gt},
    {NULL, do_lt},
    {NULL, do_gte},
    {NULL, do_lte},
    {NULL, do_eq},
    {NULL, do_neq},
    {fixarity2, do_propagate},
    {arity2or3, do_get},
    {arity1or2, do_next},
    {NULL, do_modulo},
    {NULL, do_remainder},
    {fixarity2, do_cmp},
    {fixarity2, do_cancel},
    {NULL, do_divf}
};

const BenderFunOptimizer *benderc_funopt(uint32_t flags) {
    uint32_t tag = flags & BENDER_FUNCDEF_FLAG_TAG;
    if (tag == 0)
        return NULL;
    uint32_t index = tag - 1;
    if (index >= (sizeof(optimizers) / sizeof(optimizers[0])))
        return NULL;
    return optimizers + index;
}

