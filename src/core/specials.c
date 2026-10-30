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
#include "util.h"
#include "vector.h"
#include "emit.h"
#endif

static int bad_16bit_jump(int32_t lab1, int32_t lab2) {
    if (lab2 - lab1 > INT16_MAX) return 1;
    if (lab2 - lab1 < INT16_MIN) return 1;
    return 0;
}

static void check_16bit_jump(BenderCompiler *c, int32_t lab1, int32_t lab2) {
    if (bad_16bit_jump(lab1, lab2)) {
        benderc_cerror(c, "bad 16-bit jump, too large");
    }
}

static int bad_24bit_jump(int32_t lab1, int32_t lab2) {
    if (lab2 - lab1 > 0xFFFFFF) return 1;
    if (lab2 - lab1 < -0x1000000) return 1;
    return 0;
}

static void check_24bit_jump(BenderCompiler *c, int32_t lab1, int32_t lab2) {
    if (bad_24bit_jump(lab1, lab2)) {
        benderc_cerror(c, "bad 24-bit jump, too large");
    }
}

static BenderSlot benderc_quote(BenderFopts opts, int32_t argn, const Bender *argv) {
    if (argn != 1) {
        benderc_cerror(opts.compiler, "expected 1 argument to quote");
        return benderc_cslot(bender_wrap_nil());
    }
    return benderc_cslot(argv[0]);
}

static BenderSlot benderc_splice(BenderFopts opts, int32_t argn, const Bender *argv) {
    BenderSlot ret;
    if (!(opts.flags & BENDER_FOPTS_ACCEPT_SPLICE)) {
        benderc_cerror(opts.compiler, "splice can only be used in function parameters and data constructors, it has no effect here");
        return benderc_cslot(bender_wrap_nil());
    }
    if (argn != 1) {
        benderc_cerror(opts.compiler, "expected 1 argument to splice");
        return benderc_cslot(bender_wrap_nil());
    }
    ret = benderc_value(opts, argv[0]);
    ret.flags |= BENDER_SLOT_SPLICED;
    return ret;
}

static BenderSlot qq_slots(BenderFopts opts, BenderSlot *slots, int makeop) {
    BenderSlot target = benderc_gettarget(opts);
    benderc_pushslots(opts.compiler, slots);
    benderc_freeslots(opts.compiler, slots);
    benderc_emit_s(opts.compiler, makeop, target, 1);
    return target;
}

static BenderSlot quasiquote(BenderFopts opts, Bender x, int depth, int level) {
    if (depth == 0) {
        benderc_cerror(opts.compiler, "quasiquote too deeply nested");
        return benderc_cslot(bender_wrap_nil());
    }
    BenderSlot *slots = NULL;
    BenderFopts subopts = opts;
    subopts.flags &= ~BENDER_FOPTS_HINT;
    switch (bender_type(x)) {
        default:
            return benderc_cslot(x);
        case BENDER_TUPLE: {
            int32_t i, len;
            const Bender *tup = bender_unwrap_tuple(x);
            len = bender_tuple_length(tup);
            if (len > 1 && bender_checktype(tup[0], BENDER_SYMBOL)) {
                const uint8_t *head = bender_unwrap_symbol(tup[0]);
                if (!bender_cstrcmp(head, "unquote")) {
                    if (level == 0) {
                        BenderFopts subopts = benderc_fopts_default(opts.compiler);
                        subopts.flags |= BENDER_FOPTS_ACCEPT_SPLICE;
                        return benderc_value(subopts, tup[1]);
                    } else {
                        level--;
                    }
                } else if (!bender_cstrcmp(head, "quasiquote")) {
                    level++;
                }
            }
            for (i = 0; i < len; i++)
                bender_v_push(slots, quasiquote(subopts, tup[i], depth - 1, level));
            return qq_slots(opts, slots, (bender_tuple_flag(tup) & BENDER_TUPLE_FLAG_BRACKETCTOR)
                            ? JOP_MAKE_BRACKET_TUPLE
                            : JOP_MAKE_TUPLE);
        }
        case BENDER_ARRAY: {
            int32_t i;
            BenderArray *array = bender_unwrap_array(x);
            for (i = 0; i < array->count; i++)
                bender_v_push(slots, quasiquote(subopts, array->data[i], depth - 1, level));
            return qq_slots(opts, slots, JOP_MAKE_ARRAY);
        }
        case BENDER_TABLE:
        case BENDER_STRUCT: {
            const BenderKV *kv = NULL, *kvs = NULL;
            int32_t len, cap = 0;
            bender_dictionary_view(x, &kvs, &len, &cap);
            while ((kv = bender_dictionary_next(kvs, cap, kv))) {
                BenderSlot key = quasiquote(subopts, kv->key, depth - 1, level);
                BenderSlot value =  quasiquote(subopts, kv->value, depth - 1, level);
                key.flags &= ~BENDER_SLOT_SPLICED;
                value.flags &= ~BENDER_SLOT_SPLICED;
                bender_v_push(slots, key);
                bender_v_push(slots, value);
            }
            return qq_slots(opts, slots,
                            bender_checktype(x, BENDER_TABLE) ? JOP_MAKE_TABLE : JOP_MAKE_STRUCT);
        }
    }
}

static BenderSlot benderc_quasiquote(BenderFopts opts, int32_t argn, const Bender *argv) {
    if (argn != 1) {
        benderc_cerror(opts.compiler, "expected 1 argument to quasiquote");
        return benderc_cslot(bender_wrap_nil());
    }
    return quasiquote(opts, argv[0], opts.compiler->recursion_guard, 0);
}

static BenderSlot benderc_unquote(BenderFopts opts, int32_t argn, const Bender *argv) {
    (void) argn;
    (void) argv;
    benderc_cerror(opts.compiler, "cannot use unquote here");
    return benderc_cslot(bender_wrap_nil());
}

/* Perform destructuring. Be careful to
 * keep the order registers are freed.
 * Returns if the slot 'right' can be freed. */
/* Byte-compare two string-like keys (symbol/keyword/string), so `:or`
 * defaults keyed by symbol `a` match keyword lookups for `:a`. */
static int destructure_key_bytes_equal(Bender a, Bender b) {
    const uint8_t *ab = NULL, *bb = NULL;
    int32_t alen = 0, blen = 0;
    switch (bender_type(a)) {
        case BENDER_SYMBOL:
            ab = bender_unwrap_symbol(a);
            break;
        case BENDER_KEYWORD:
            ab = bender_unwrap_keyword(a);
            break;
        case BENDER_STRING:
            ab = bender_unwrap_string(a);
            break;
        default:
            return 0;
    }
    switch (bender_type(b)) {
        case BENDER_SYMBOL:
            bb = bender_unwrap_symbol(b);
            break;
        case BENDER_KEYWORD:
            bb = bender_unwrap_keyword(b);
            break;
        case BENDER_STRING:
            bb = bender_unwrap_string(b);
            break;
        default:
            return 0;
    }
    alen = bender_string_length(ab);
    blen = bender_string_length(bb);
    if (alen != blen) return 0;
    return !memcmp(ab, bb, (size_t) alen);
}
static int destructure(BenderCompiler *c,
                       Bender left,
                       BenderSlot right,
                       int (*leaf)(BenderCompiler *c,
                                   const uint8_t *sym,
                                   BenderSlot s,
                                   BenderTable *attr),
                       BenderTable *attr) {
    if (c->recursion_guard <= 0) {
        benderc_error(c, bender_cstring("C stack recursed too deeply"));
        return 1;
    }
    switch (bender_type(left)) {
        default:
            benderc_error(c, bender_formatc("unexpected type in destructuring, got %v", left));
            return 1;
        case BENDER_SYMBOL:
            /* Leaf, assign right to left */
            return leaf(c, bender_unwrap_symbol(left), right, attr);
        case BENDER_TUPLE:
        case BENDER_ARRAY: {
            int32_t len = 0;
            const Bender *values = NULL;
            bender_indexed_view(left, &values, &len);
            for (int32_t i = 0; i < len; i++) {
                BenderSlot nextright = benderc_farslot(c);
                Bender subval = values[i];

                if (bender_checktype(subval, BENDER_SYMBOL) && !bender_cstrcmp(bender_unwrap_symbol(subval), "&")) {
                    if (i + 1 >= len) {
                        benderc_cerror(c, "expected symbol following '& in destructuring pattern");
                        return 1;
                    }

                    if (i + 2 < len) {
                        int32_t num_extra = len - i - 1;
                        Bender *extra = bender_tuple_begin(num_extra);
                        bender_tuple_flag(extra) |= BENDER_TUPLE_FLAG_BRACKETCTOR;

                        for (int32_t j = 0; j < num_extra; ++j) {
                            extra[j] = values[j + i + 1];
                        }

                        benderc_error(c, bender_formatc("expected a single symbol follow '& in destructuring pattern, found %q", bender_wrap_tuple(bender_tuple_end(extra))));
                        return 1;
                    }

                    if (!bender_checktype(values[i + 1], BENDER_SYMBOL)) {
                        benderc_error(c, bender_formatc("expected symbol following '& in destructuring pattern, found %q", values[i + 1]));
                        return 1;
                    }

                    BenderSlot argi = benderc_farslot(c);
                    BenderSlot arg  = benderc_farslot(c);
                    BenderSlot len  = benderc_farslot(c);

                    benderc_emit_si(c, JOP_LOAD_INTEGER, argi, i, 0);
                    benderc_emit_ss(c, JOP_LENGTH, len, right, 0);

                    /* loop condition - reuse arg slot for the condition result */
                    int32_t label_loop_start = benderc_emit_sss(c, JOP_LESS_THAN, arg, argi, len, 0);
                    int32_t label_loop_cond_jump = benderc_emit_si(c, JOP_JUMP_IF_NOT, arg, 0, 0);

                    /* loop body */
                    benderc_emit_sss(c, JOP_GET, arg, right, argi, 0);
                    benderc_emit_s(c, JOP_PUSH, arg, 0);
                    benderc_emit_ssi(c, JOP_ADD_IMMEDIATE, argi, argi, 1, 0);

                    /* loop - jump back to the start of the loop */
                    int32_t label_loop_loop = bender_v_count(c->buffer);
                    benderc_emit(c, JOP_JUMP);
                    int32_t label_loop_exit = bender_v_count(c->buffer);

                    /* avoid shifting negative numbers */
                    check_16bit_jump(c, label_loop_cond_jump, label_loop_exit);
                    check_24bit_jump(c, label_loop_start, label_loop_loop);
                    c->buffer[label_loop_cond_jump] |= (uint32_t)(label_loop_exit - label_loop_cond_jump) << 16;
                    c->buffer[label_loop_loop] |= (uint32_t)(label_loop_start - label_loop_loop) << 8;

                    benderc_freeslot(c, argi);
                    benderc_freeslot(c, arg);
                    benderc_freeslot(c, len);

                    benderc_emit_s(c, JOP_MAKE_TUPLE, nextright, 1);

                    leaf(c, bender_unwrap_symbol(values[i + 1]), nextright, attr);
                    benderc_freeslot(c, nextright);
                    break;
                }

                if (i < 0x100) {
                    benderc_emit_ssu(c, JOP_GET_INDEX, nextright, right, (uint8_t) i, 1);
                } else {
                    BenderSlot k = benderc_cslot(bender_wrap_integer(i));
                    benderc_emit_sss(c, JOP_IN, nextright, right, k, 1);
                }
                c->recursion_guard--;
                if (destructure(c, subval, nextright, leaf, attr))
                    benderc_freeslot(c, nextright);
                c->recursion_guard++;
            }
        }
        return 1;
        case BENDER_TABLE:
        case BENDER_STRUCT: {
            const BenderKV *kvs = NULL;
            int32_t cap = 0, len = 0;
            bender_dictionary_view(left, &kvs, &len, &cap);
            /* Clojure-style `:or {key default}`: first pass collects
             * the defaults dict (literal values only — patterns are
             * data, so defaults cannot be computed). A non-dict `:or`
             * value falls through to a normal key binding. */
            const BenderKV *orkvs = NULL;
            int32_t orlen = 0, orcap = 0;
            {
                int32_t oi;
                for (oi = 0; oi < cap; oi++) {
                    if (bender_checktype(kvs[oi].key, BENDER_KEYWORD)) {
                    }
                    if (bender_checktype(kvs[oi].key, BENDER_KEYWORD) &&
                            !bender_cstrcmp(bender_unwrap_keyword(kvs[oi].key), "or") &&
                            (bender_checktype(kvs[oi].value, BENDER_TABLE) ||
                             bender_checktype(kvs[oi].value, BENDER_STRUCT))) {
                        {
                            Bender vv = kvs[oi].value;
                        }
                        bender_dictionary_view(kvs[oi].value, &orkvs, &orlen, &orcap);
                        break;
                    }
                }
            }
            for (int32_t i = 0; i < cap; i++) {
                if (bender_checktype(kvs[i].key, BENDER_NIL)) continue;
                /* The `:or` defaults entry was consumed in the first
                 * pass; never bind it as a key. */
                if (NULL != orkvs &&
                        bender_checktype(kvs[i].key, BENDER_KEYWORD) &&
                        !bender_cstrcmp(bender_unwrap_keyword(kvs[i].key), "or"))
                    continue;
                /* `:keys [a b]` shorthand: symbol S binds S from keyword
                 * :S; keyword :K binds its symbol form from :K. A
                 * non-sequential `:keys` value is a normal binding. */
                if (bender_checktype(kvs[i].key, BENDER_KEYWORD) &&
                        !bender_cstrcmp(bender_unwrap_keyword(kvs[i].key), "keys") &&
                        (bender_checktype(kvs[i].value, BENDER_TUPLE) ||
                         bender_checktype(kvs[i].value, BENDER_ARRAY))) {
                    const Bender *names = NULL;
                    int32_t nlen = 0;
                    int32_t ni;
                    bender_indexed_view(kvs[i].value, &names, &nlen);
                    for (ni = 0; ni < nlen; ni++) {
                        const uint8_t *nb = NULL;
                        const uint8_t *kb = NULL;
                        Bender key;
                        if (bender_checktype(names[ni], BENDER_SYMBOL)) {
                            nb = bender_unwrap_symbol(names[ni]);
                            kb = nb;
                        } else if (bender_checktype(names[ni], BENDER_KEYWORD)) {
                            kb = bender_unwrap_keyword(names[ni]);
                            nb = kb;
                        } else {
                            benderc_error(c, bender_formatc("expected symbol or keyword in :keys vector, got %v", names[ni]));
                            return 1;
                        }
                        key = bender_keywordv(kb, bender_string_length(kb));
                        {
                            Bender bindpat = bender_wrap_symbol(bender_symbol(nb, bender_string_length(nb)));
                            BenderSlot nextright = benderc_farslot(c);
                            BenderSlot k = benderc_value(benderc_fopts_default(c), key);
                            Bender dflt = bender_wrap_nil();
                            int has_dflt = 0;
                            int32_t oj;
                            for (oj = 0; oj < orcap; oj++) {
                                if (bender_checktype(orkvs[oj].key, BENDER_NIL)) continue;
                                if (destructure_key_bytes_equal(orkvs[oj].key, key)) {
                                    dflt = orkvs[oj].value;
                                    has_dflt = 1;
                                    break;
                                }
                            }
                            benderc_emit_sss(c, JOP_IN, nextright, right, k, 1);
                            if (has_dflt) {
                                int32_t lab = benderc_emit_si(c, JOP_JUMP_IF_NOT_NIL, nextright, 0, 0);
                                BenderSlot d = benderc_value(benderc_fopts_default(c), dflt);
                                benderc_copy(c, nextright, d);
                                {
                                    int32_t cur = bender_v_count(c->buffer);
                                    check_16bit_jump(c, lab, cur);
                                    c->buffer[lab] |= (uint32_t)(cur - lab) << 16;
                                }
                            }
                            c->recursion_guard--;
                            if (destructure(c, bindpat, nextright, leaf, attr))
                                benderc_freeslot(c, nextright);
                            c->recursion_guard++;
                        }
                    }
                    continue;
                }
                /* NOTE: no `:as` whole-binding: `{:as sym}` keeps its
                 * legacy meaning (bind `(get m :as)`), which the
                 * `import*` calling convention depends on
                 * (`(def {:as as :prefix prefix ...} kargs)`). A
                 * Clojure-style whole-binding would silently redefine
                 * every such use — caught by suite-array via broken
                 * imports. */
                {
                    BenderSlot nextright = benderc_farslot(c);
                    BenderSlot k = benderc_value(benderc_fopts_default(c), kvs[i].key);
                    Bender dflt = bender_wrap_nil();
                    int has_dflt = 0;
                    int32_t oj;
                    for (oj = 0; oj < orcap; oj++) {
                        if (bender_checktype(orkvs[oj].key, BENDER_NIL)) continue;
                        if (destructure_key_bytes_equal(orkvs[oj].key, kvs[i].key)) {
                            dflt = orkvs[oj].value;
                            has_dflt = 1;
                            break;
                        }
                    }
                    benderc_emit_sss(c, JOP_IN, nextright, right, k, 1);
                    if (has_dflt) {
                        int32_t lab = benderc_emit_si(c, JOP_JUMP_IF_NOT_NIL, nextright, 0, 0);
                        BenderSlot d = benderc_value(benderc_fopts_default(c), dflt);
                        benderc_copy(c, nextright, d);
                        {
                            int32_t cur = bender_v_count(c->buffer);
                            check_16bit_jump(c, lab, cur);
                            c->buffer[lab] |= (uint32_t)(cur - lab) << 16;
                        }
                    }
                    c->recursion_guard--;
                    if (destructure(c, kvs[i].value, nextright, leaf, attr))
                        benderc_freeslot(c, nextright);
                    c->recursion_guard++;
                }
            }
        }
        return 1;
    }
}

/* Create a source map for definitions. */
static const Bender *benderc_make_sourcemap(BenderCompiler *c) {
    Bender *tup = bender_tuple_begin(3);
    tup[0] = c->source ? bender_wrap_string(c->source) : bender_wrap_nil();
    tup[1] = bender_wrap_integer(c->current_mapping.line);
    tup[2] = bender_wrap_integer(c->current_mapping.column);
    return bender_tuple_end(tup);
}

static BenderSlot benderc_varset(BenderFopts opts, int32_t argn, const Bender *argv) {
    if (argn != 2) {
        benderc_cerror(opts.compiler, "expected 2 arguments to set");
        return benderc_cslot(bender_wrap_nil());
    }
    BenderFopts subopts = benderc_fopts_default(opts.compiler);
    if (bender_checktype(argv[0], BENDER_SYMBOL)) {
        /* Normal var - (set a 1) */
        const uint8_t *sym = bender_unwrap_symbol(argv[0]);
        BenderSlot dest = benderc_resolve(opts.compiler, sym);
        if (!(dest.flags & BENDER_SLOT_MUTABLE)) {
            benderc_cerror(opts.compiler, "cannot set constant");
            return benderc_cslot(bender_wrap_nil());
        }
        subopts.flags = BENDER_FOPTS_HINT;
        subopts.hint = dest;
        BenderSlot ret = benderc_value(subopts, argv[1]);
        benderc_copy(opts.compiler, dest, ret);
        return ret;
    } else if (bender_checktype(argv[0], BENDER_TUPLE)) {
        /* Set a field (setf behavior) - (set (tab :key) 2) */
        const Bender *tup = bender_unwrap_tuple(argv[0]);
        /* Tuple must have 2 elements */
        if (bender_tuple_length(tup) != 2) {
            benderc_cerror(opts.compiler, "expected 2 element tuple for l-value to set");
            return benderc_cslot(bender_wrap_nil());
        }
        BenderSlot ds = benderc_value(subopts, tup[0]);
        BenderSlot key = benderc_value(subopts, tup[1]);
        /* Can't be tail position because we will emit a PUT instruction afterwards */
        /* Also can't drop either */
        opts.flags &= ~(BENDER_FOPTS_TAIL | BENDER_FOPTS_DROP);
        BenderSlot rvalue = benderc_value(opts, argv[1]);
        /* Emit the PUT instruction */
        benderc_emit_sss(opts.compiler, JOP_PUT, ds, key, rvalue, 0);
        return rvalue;
    } else {
        /* Error */
        benderc_cerror(opts.compiler, "expected symbol or tuple for l-value to set");
        return benderc_cslot(bender_wrap_nil());
    }
}

/* Add attributes to a global def or var table */
static BenderTable *handleattr(BenderCompiler *c, const char *kind, int32_t argn, const Bender *argv) {
    int32_t i;
    if (argn < 2) {
        benderc_error(c, bender_formatc("expected at least 2 arguments to %s", kind));
        return NULL;
    }
    BenderTable *tab = bender_table(2);
    /* Phase 2a root discipline: this table is held only in C locals across
     * dohead_destructure below, which compiles the bound value (arbitrary
     * code, allocations, collections, and -- since Phase 1.1 -- even
     * suspensions). Pin it for the rest of the compilation. */
    benderc_pin(c, bender_wrap_table(tab));
    const char *binding_name = bender_type(argv[0]) == BENDER_SYMBOL
                               ? ((const char *)bender_unwrap_symbol(argv[0]))
                               : "<multiple bindings>";
    for (i = 1; i < argn - 1; i++) {
        Bender attr = argv[i];
        switch (bender_type(attr)) {
            case BENDER_TUPLE:
                if (argn > 10) {
                    for (int32_t q = 0; q < argn; q++) {
                        BenderString dstr = bender_description(argv[q]);
                        bender_eprintf("  argv[%d] type=%d len=%d: ",
                                      (int) q, (int) bender_type(argv[q]),
                                      (int) bender_string_length(dstr));
                        for (int32_t w = 0; w < bender_string_length(dstr) && w < 70; w++) {
                            bender_eprintf("%c", (char) dstr[w]);
                        }
                        bender_eprintf("\n");
                    }
                }
                benderc_cerror(c, "unexpected form - did you intend to use defn?");
                break;
            default:
                benderc_error(c, bender_formatc("cannot add metadata %v to binding %s", attr, binding_name));
                break;
            case BENDER_KEYWORD:
                bender_table_put(tab, attr, bender_wrap_true());
                break;
            case BENDER_STRING:
                bender_table_put(tab, bender_ckeywordv("doc"), attr);
                break;
            case BENDER_STRUCT:
                bender_table_merge_struct(tab, bender_unwrap_struct(attr));
                break;
        }
    }
    return tab;
}

typedef struct SlotHeadPair {
    Bender lhs;
    BenderSlot rhs;
} SlotHeadPair;

SlotHeadPair *dohead_destructure(BenderCompiler *c, SlotHeadPair *into, BenderFopts opts, Bender lhs, Bender rhs) {
    if (c->recursion_guard <= 0) {
        benderc_error(c, bender_cstring("C stack recursed too deeply"));
        return NULL;
    }
    /* Detect if we can do an optimization to avoid some allocations */
    int can_destructure_lhs = bender_checktype(lhs, BENDER_TUPLE)
                              || bender_checktype(lhs, BENDER_ARRAY);
    int rhs_is_indexed = bender_checktype(rhs, BENDER_ARRAY)
                         || (bender_checktype(rhs, BENDER_TUPLE) && (bender_tuple_flag(bender_unwrap_tuple(rhs)) & BENDER_TUPLE_FLAG_BRACKETCTOR));
    uint32_t has_drop = opts.flags & BENDER_FOPTS_DROP;

    BenderFopts subopts = benderc_fopts_default(c);
    subopts.flags = opts.flags & ~(BENDER_FOPTS_TAIL | BENDER_FOPTS_DROP);

    if (has_drop && can_destructure_lhs && rhs_is_indexed) {
        /* Code is of the form (def [a b] [1 2]), avoid the allocation of two tuples */
        BenderView view_lhs = {0};
        BenderView view_rhs = {0};
        bender_indexed_view(lhs, &view_lhs.items, &view_lhs.len);
        bender_indexed_view(rhs, &view_rhs.items, &view_rhs.len);
        int found_amp = 0;
        int found_splice = 0;
        /* Check for (def [x y z] [(splice [1 2 3]) 4 5 6]), bail out of optimization */
        for (int32_t i = 0; i < view_rhs.len; i++) {
            if (!bender_checktype(view_rhs.items[i], BENDER_TUPLE)) {
                continue;
            }
            BenderTuple tup = bender_unwrap_tuple(view_rhs.items[i]);
            if (bender_tuple_length(tup) == 0) {
                continue;
            }
            if (bender_symeq(tup[0], "splice")) {
                found_splice = 1;
                /* Good error will be generated later. */
                break;
            }
        }
        /* Check for (def [x & more] [1 2 3]), bail out of optimization */
        for (int32_t i = 0; i < view_lhs.len; i++) {
            if (bender_symeq(view_lhs.items[i], "&")) {
                found_amp = 1;
                /* Good error will be generated later. */
                break;
            }
        }
        if (!found_amp && !found_splice) {
            for (int32_t i = 0; i < view_lhs.len; i++) {
                Bender sub_rhs = view_rhs.len <= i ? bender_wrap_nil() : view_rhs.items[i];
                c->recursion_guard--;
                into = dohead_destructure(c, into, subopts, view_lhs.items[i], sub_rhs);
                c->recursion_guard++;
            }
            return into;
        }
    }

    /* No optimization, do the simple way */
    subopts.hint = opts.hint;
    BenderSlot ret = benderc_value(subopts, rhs);
    SlotHeadPair shp = {lhs, ret};
    bender_v_push(into, shp);
    return into;
}

/* Def or var a symbol in a local scope */
static int namelocal(BenderCompiler *c, const uint8_t *head, int32_t flags, BenderSlot ret, uint32_t def_flags) {
    int isUnnamedRegister = !(ret.flags & BENDER_SLOT_NAMED) &&
                            ret.index > 0 &&
                            ret.envindex >= 0;
    /* optimization for `(def x my-def)` - don't emit a movn/movf instruction, we can just alias my-def */
    /* TODO - implement optimization for `(def x my-var)` correctly as well w/ de-aliasing */
    int canAlias = !(flags & BENDER_SLOT_MUTABLE) &&
                   !(ret.flags & BENDER_SLOT_MUTABLE) &&
                   (ret.flags & BENDER_SLOT_NAMED) &&
                   (ret.index >= 0) &&
                   (ret.envindex == -1);
    if (canAlias) {
        ret.flags &= ~BENDER_SLOT_MUTABLE;
        isUnnamedRegister = 1; /* don't free slot after use - is an alias for another slot */
    } else if (!isUnnamedRegister) {
        /* Slot is not able to be named */
        BenderSlot localslot = benderc_farslot(c);
        benderc_copy(c, localslot, ret);
        ret = localslot;
    }
    ret.flags |= flags;
    if (c->scope->flags & BENDER_SCOPE_TOP) {
        def_flags |= BENDER_DEFFLAG_NO_UNUSED;
    }
    benderc_nameslot(c, head, ret, def_flags);
    return !isUnnamedRegister;
}

static int varleaf(
    BenderCompiler *c,
    const uint8_t *sym,
    BenderSlot s,
    BenderTable *reftab) {
    if (c->scope->flags & BENDER_SCOPE_TOP) {
        /* Global var, generate var */
        BenderSlot refslot;
        BenderTable *entry = bender_table_clone(reftab);

        int is_redef = c->is_redef;

        BenderArray *ref;
        BenderBinding old_binding;
        if (is_redef && (old_binding = bender_resolve_ext(c->env, sym),
                         old_binding.type == BENDER_BINDING_VAR)) {
            ref = bender_unwrap_array(old_binding.value);
        } else {
            ref = bender_array(1);
            bender_array_push(ref, bender_wrap_nil());
        }

        bender_table_put(entry, bender_ckeywordv("ref"), bender_wrap_array(ref));
        bender_table_put(entry, bender_ckeywordv("source-map"),
                        bender_wrap_tuple(benderc_make_sourcemap(c)));
        bender_table_put(c->env, bender_wrap_symbol(sym), bender_wrap_table(entry));
        refslot = benderc_cslot(bender_wrap_array(ref));
        benderc_emit_ssu(c, JOP_PUT_INDEX, refslot, s, 0, 0);
        return 1;
    } else {
        int no_unused = reftab && reftab->count && bender_truthy(bender_table_get_keyword(reftab, "unused"));
        int no_shadow = reftab && reftab->count && bender_truthy(bender_table_get_keyword(reftab, "shadow"));
        uint32_t def_flags = 0;
        if (no_unused) def_flags |= BENDER_DEFFLAG_NO_UNUSED;
        if (no_shadow) def_flags |= BENDER_DEFFLAG_NO_SHADOWCHECK;
        return namelocal(c, sym, BENDER_SLOT_MUTABLE, s, def_flags);
    }
}

static void check_metadata_lint(BenderCompiler *c, BenderTable *attr_table) {
    if (!(c->scope->flags & BENDER_SCOPE_TOP) && attr_table && attr_table->count) {
        /* A macro is a normal lint, other metadata is a strict lint */
        if (bender_truthy(bender_table_get_keyword(attr_table, "macro"))) {
            benderc_lintf(c, BENDER_C_LINT_NORMAL, "macro tag is ignored in inner scopes");
        }
    }
}

static BenderSlot benderc_var(BenderFopts opts, int32_t argn, const Bender *argv) {
    BenderCompiler *c = opts.compiler;
    BenderTable *attr_table = handleattr(c, "var", argn, argv);
    if (c->result.status == BENDER_COMPILE_ERROR) {
        return benderc_cslot(bender_wrap_nil());
    }
    check_metadata_lint(c, attr_table);
    SlotHeadPair *into = NULL;
    into = dohead_destructure(c, into, opts, argv[0], argv[argn - 1]);
    if (c->result.status == BENDER_COMPILE_ERROR) {
        bender_v_free(into);
        return benderc_cslot(bender_wrap_nil());
    }
    BenderSlot ret = benderc_cslot(bender_wrap_nil());
    for (int32_t i = 0; i < bender_v_count(into); i++) {
        destructure(c, into[i].lhs, into[i].rhs, varleaf, attr_table);
        ret = into[i].rhs;
    }
    bender_v_free(into);
    return ret;
}

static int defleaf(
    BenderCompiler *c,
    const uint8_t *sym,
    BenderSlot s,
    BenderTable *tab) {
    BenderTable *entry = NULL;
    int is_redef = 0;
    if (c->scope->flags & BENDER_SCOPE_TOP) {
        entry = bender_table_clone(tab);
        bender_table_put(entry, bender_ckeywordv("source-map"),
                        bender_wrap_tuple(benderc_make_sourcemap(c)));

        is_redef = c->is_redef;
        if (is_redef) bender_table_put(entry, bender_ckeywordv("redef"), bender_wrap_true());

        if (is_redef) {
            BenderBinding binding = bender_resolve_ext(c->env, sym);
            BenderArray *ref;
            if (binding.type == BENDER_BINDING_DYNAMIC_DEF || binding.type == BENDER_BINDING_DYNAMIC_MACRO) {
                ref = bender_unwrap_array(binding.value);
            } else {
                ref = bender_array(1);
                bender_array_push(ref, bender_wrap_nil());
            }
            bender_table_put(entry, bender_ckeywordv("ref"), bender_wrap_array(ref));
            BenderSlot refslot = benderc_cslot(bender_wrap_array(ref));
            benderc_emit_ssu(c, JOP_PUT_INDEX, refslot, s, 0, 0);
        } else {
            BenderSlot valsym = benderc_cslot(bender_ckeywordv("value"));
            BenderSlot tabslot = benderc_cslot(bender_wrap_table(entry));
            benderc_emit_sss(c, JOP_PUT, tabslot, valsym, s, 0);
        }
    }
    int no_unused = tab && tab->count && bender_truthy(bender_table_get_keyword(tab, "unused"));
    int no_shadow = is_redef || (tab && tab->count && bender_truthy(bender_table_get_keyword(tab, "shadow")));
    uint32_t def_flags = 0;
    if (no_unused) def_flags |= BENDER_DEFFLAG_NO_UNUSED;
    if (no_shadow) def_flags |= BENDER_DEFFLAG_NO_SHADOWCHECK;
    int result = namelocal(c, sym, 0, s, def_flags);
    if (entry) {
        /* Add env entry to env AFTER namelocal to avoid the shadowcheck false positive */
        bender_table_put(c->env, bender_wrap_symbol(sym), bender_wrap_table(entry));
    }
    return result;
}

static BenderSlot benderc_def(BenderFopts opts, int32_t argn, const Bender *argv) {
    BenderCompiler *c = opts.compiler;
    BenderTable *attr_table = handleattr(c, "def", argn, argv);
    if (c->result.status == BENDER_COMPILE_ERROR) {
        return benderc_cslot(bender_wrap_nil());
    }
    check_metadata_lint(c, attr_table);
    /* The expanded binding metadata is authoritative. Only a direct named
     * function literal has a provable one-to-one child identity. */
    Bender hint = bender_table_get_keyword(attr_table, "hint");
    int hinted = bender_checktype(hint, BENDER_TABLE) || bender_checktype(hint, BENDER_STRUCT);
    Bender selections = bender_table_get_keyword(c->env, "analysis-retain-profiled-defs");
    Bender selected_line = bender_wrap_nil();
    if (c->retain_hinted_exports_for_analysis && bender_checktype(argv[0], BENDER_SYMBOL) &&
        bender_checktype(selections, BENDER_TABLE))
        selected_line = bender_table_get(bender_unwrap_table(selections), argv[0]);
    int profiled = bender_checkint(selected_line);
    /* Private hints are checked contracts too; profile selection remains
     * export-only, since private names are not stable module bindings. */
    int retain = c->retain_hinted_exports_for_analysis &&
                 (hinted || (profiled && !bender_truthy(bender_table_get_keyword(attr_table, "private"))));
    BenderScope *fn_scope = c->scope;
    while (fn_scope && !(fn_scope->flags & BENDER_SCOPE_FUNCTION)) fn_scope = fn_scope->parent;
    int32_t defs_before = fn_scope ? bender_v_count(fn_scope->defs) : 0;
    int32_t code_before = bender_v_count(c->buffer);
    opts.flags &= ~BENDER_FOPTS_HINT;
    SlotHeadPair *into = NULL;
    into = dohead_destructure(c, into, opts, argv[0], argv[argn - 1]);
    if (c->result.status == BENDER_COMPILE_ERROR) {
        bender_v_free(into);
        return benderc_cslot(bender_wrap_nil());
    }
    /* ffi/defbind-alias adds {:ffi-alias true} metadata. Mark the direct
     * named function literal's FuncDef so calls compiled later can
     * recognize it without depending on any runtime registration. */
    if (bender_truthy(bender_table_get_keyword(attr_table, "ffi-alias")) &&
            fn_scope && bender_checktype(argv[0], BENDER_SYMBOL) &&
            bender_v_count(into) == 1 &&
            bender_v_count(fn_scope->defs) == defs_before + 1) {
        Bender rhs = argv[argn - 1];
        if (bender_checktype(rhs, BENDER_TUPLE)) {
            BenderTuple form = bender_unwrap_tuple(rhs);
            if (bender_tuple_length(form) >= 3 && bender_symeq(form[0], "fn") &&
                    bender_checktype(form[1], BENDER_SYMBOL) &&
                    bender_equals(form[1], argv[0])) {
                fn_scope->defs[defs_before]->flags |= BENDER_FUNCDEF_FLAG_FFI_ALIAS_DECL;
            }
        }
    }
    if (retain) {
        int matched = 0;
        BenderFuncDef *child = NULL;
        Bender rhs = argv[argn - 1];
        if (bender_checktype(argv[0], BENDER_SYMBOL) && bender_checktype(rhs, BENDER_TUPLE)) {
            BenderTuple form = bender_unwrap_tuple(rhs);
            if (bender_tuple_length(form) >= 3 && bender_symeq(form[0], "fn") &&
                bender_checktype(form[1], BENDER_SYMBOL) &&
                bender_equals(form[1], argv[0]) &&
                bender_v_count(into) == 1 && fn_scope &&
                bender_v_count(fn_scope->defs) == defs_before + 1) {
                child = fn_scope->defs[defs_before];
                if (!child->name || !bender_equals(bender_wrap_string(child->name),
                                                   bender_wrap_string(bender_unwrap_symbol(argv[0]))))
                    child = NULL;
                for (int32_t pc = code_before; pc < bender_v_count(c->buffer); pc++) {
                    uint32_t op = c->buffer[pc];
                    if ((op & 0x7F) == JOP_CLOSURE && (op >> 16) == (uint32_t) defs_before &&
                        ((op >> 8) & 0xFF) == (uint32_t) into[0].rhs.index)
                        matched++;
                }
            }
        }
        if (matched == 1 && child && profiled &&
            (!child->sourcemap || child->bytecode_length == 0 ||
             child->sourcemap[0].line != bender_unwrap_integer(selected_line))) {
            profiled = 0;
            if (!hinted)
                bender_eprintf("analysis retention refused for profiled def %v: source line does not match child identity\n", argv[0]);
        }
        if (matched == 1 && child && (hinted || profiled)) {
            BenderRetainedDef entry = {fn_scope, NULL, child, hinted ? hint : bender_wrap_nil(), profiled};
            bender_v_push(c->retained_defs, entry);
        } else if (matched != 1 || !child) {
            bender_eprintf("analysis retention refused for %s def %v: cannot prove direct closure identity\n",
                          hinted ? "hinted" : "profiled", argv[0]);
        }
    }
    BenderSlot ret = benderc_cslot(bender_wrap_nil());
    for (int32_t i = 0; i < bender_v_count(into); i++) {
        destructure(c, into[i].lhs, into[i].rhs, defleaf, attr_table);
        ret = into[i].rhs;
    }
    bender_v_free(into);
    return ret;
}

/* Check if a form matches the pattern (= nil _) or (not= nil _) */
static int benderc_check_nil_form(Bender x, Bender *capture, uint32_t fun_tag) {
    if (!bender_checktype(x, BENDER_TUPLE)) return 0;
    BenderTuple tup = bender_unwrap_tuple(x);
    if (3 != bender_tuple_length(tup)) return 0;
    Bender op1 = tup[0];
    if (!bender_checktype(op1, BENDER_FUNCTION)) return 0;
    BenderFunction *fun = bender_unwrap_function(op1);
    uint32_t tag = fun->def->flags & BENDER_FUNCDEF_FLAG_TAG;
    if (tag != fun_tag) return 0;
    if (bender_checktype(tup[1], BENDER_NIL)) {
        *capture = tup[2];
        return 1;
    } else if (bender_checktype(tup[2], BENDER_NIL)) {
        *capture = tup[1];
        return 1;
    }
    return 0;
}

/*
 * :condition
 * ...
 * jump-if-not condition :right
 * :left
 * ...
 * jump done (only if not tail)
 * :right
 * ...
 * :done
 */
static BenderSlot benderc_if(BenderFopts opts, int32_t argn, const Bender *argv) {
    BenderCompiler *c = opts.compiler;
    int32_t labelr, labeljr, labeld, labeljd;
    BenderFopts condopts, bodyopts;
    BenderSlot cond, left, right, target;
    Bender truebody, falsebody;
    BenderScope condscope, tempscope;
    const int tail = opts.flags & BENDER_FOPTS_TAIL;
    const int drop = opts.flags & BENDER_FOPTS_DROP;
    uint8_t ifnjmp = JOP_JUMP_IF_NOT;

    if (argn < 2 || argn > 3) {
        benderc_cerror(c, "expected 2 or 3 arguments to if");
        return benderc_cslot(bender_wrap_nil());
    }

    /* Get the bodies of the if expression */
    truebody = argv[1];
    falsebody = argn > 2 ? argv[2] : bender_wrap_nil();

    /* Get options */
    condopts = benderc_fopts_default(c);
    bodyopts = opts;
    bodyopts.flags &= ~BENDER_FOPTS_ACCEPT_SPLICE;

    /* Set target for compilation */
    target = (drop || tail)
             ? benderc_cslot(bender_wrap_nil())
             : benderc_gettarget(opts);

    /* Compile condition */
    benderc_scope(&condscope, c, 0, "if");

    Bender condform = argv[0];
    if (benderc_check_nil_form(condform, &condform, BENDER_FUN_EQ)) {
        ifnjmp = JOP_JUMP_IF_NOT_NIL;
    } else if (benderc_check_nil_form(condform, &condform, BENDER_FUN_NEQ)) {
        ifnjmp = JOP_JUMP_IF_NIL;
    }

    cond = benderc_value(condopts, condform);

    /* Check constant condition. */
    /* TODO: Use type info for more short circuits */
    if (cond.flags & BENDER_SLOT_CONSTANT) {
        int swap_condition = 0;
        if (ifnjmp == JOP_JUMP_IF_NOT && !bender_truthy(cond.constant)) swap_condition = 1;
        if (ifnjmp == JOP_JUMP_IF_NIL && bender_checktype(cond.constant, BENDER_NIL)) swap_condition = 1;
        if (ifnjmp == JOP_JUMP_IF_NOT_NIL && !bender_checktype(cond.constant, BENDER_NIL)) swap_condition = 1;
        if (swap_condition) {
            /* Swap the true and false bodies */
            Bender temp = falsebody;
            falsebody = truebody;
            truebody = temp;
        }
        benderc_scope(&tempscope, c, 0, "if-true");
        right = benderc_value(bodyopts, truebody);
        if (!drop && !tail) benderc_copy(c, target, right);
        benderc_popscope(c);
        if (!bender_checktype(falsebody, BENDER_NIL)) {
            benderc_throwaway(bodyopts, falsebody);
        }
        benderc_popscope(c);
        return target;
    }

    /* Compile jump to right */
    labeljr = benderc_emit_si(c, ifnjmp, cond, 0, 0);

    /* Condition left body */
    benderc_scope(&tempscope, c, 0, "if-true");
    left = benderc_value(bodyopts, truebody);
    if (!drop && !tail) benderc_copy(c, target, left);
    benderc_popscope(c);

    /* Compile jump to done */
    labeljd = bender_v_count(c->buffer);
    if (!tail && !(drop && bender_checktype(falsebody, BENDER_NIL))) benderc_emit(c, JOP_JUMP);

    /* Compile right body */
    labelr = bender_v_count(c->buffer);
    benderc_scope(&tempscope, c, 0, "if-false");
    right = benderc_value(bodyopts, falsebody);
    if (!drop && !tail) benderc_copy(c, target, right);
    benderc_popscope(c);

    /* Pop main scope */
    benderc_popscope(c);

    /* Write jumps - only add jump lengths if jump actually emitted */
    labeld = bender_v_count(c->buffer);
    if (labeljr < labeld) {
        check_16bit_jump(c, labeljr, labelr);
        c->buffer[labeljr] |= (uint32_t)(labelr - labeljr) << 16;
        if (!tail && labeljd < labeld) {
            check_24bit_jump(c, labeljd, labeld);
            c->buffer[labeljd] |= (uint32_t)(labeld - labeljd) << 8;
        }
    }

    if (tail) target.flags |= BENDER_SLOT_RETURNED;
    return target;
}

/* Compile a do form. Do forms execute their body sequentially and
 * evaluate to the last expression in the body. */
static BenderSlot benderc_do(BenderFopts opts, int32_t argn, const Bender *argv) {
    int32_t i;
    BenderSlot ret = benderc_cslot(bender_wrap_nil());
    BenderCompiler *c = opts.compiler;
    BenderFopts subopts = benderc_fopts_default(c);
    BenderScope tempscope;
    benderc_scope(&tempscope, c, 0, "do");
    for (i = 0; i < argn; i++) {
        if (i != argn - 1) {
            subopts.flags = BENDER_FOPTS_DROP;
        } else {
            subopts = opts;
            subopts.flags &= ~BENDER_FOPTS_ACCEPT_SPLICE;
        }
        ret = benderc_value(subopts, argv[i]);
        if (i != argn - 1) {
            benderc_freeslot(c, ret);
        }
    }
    benderc_popscope_keepslot(c, ret);
    return ret;
}

/* Compile an upscope form. Upscope forms execute their body sequentially and
 * evaluate to the last expression in the body, but without lexical scope. */
static BenderSlot benderc_upscope(BenderFopts opts, int32_t argn, const Bender *argv) {
    int32_t i;
    BenderSlot ret = benderc_cslot(bender_wrap_nil());
    BenderCompiler *c = opts.compiler;
    BenderFopts subopts = benderc_fopts_default(c);
    for (i = 0; i < argn; i++) {
        if (i != argn - 1) {
            subopts.flags = BENDER_FOPTS_DROP;
        } else {
            subopts = opts;
            subopts.flags &= ~BENDER_FOPTS_ACCEPT_SPLICE;
        }
        ret = benderc_value(subopts, argv[i]);
        if (i != argn - 1) {
            benderc_freeslot(c, ret);
        }
    }
    return ret;
}

/* Add a funcdef to the top most function scope */
static int32_t benderc_addfuncdef(BenderCompiler *c, BenderFuncDef *def) {
    BenderScope *scope = c->scope;
    while (scope) {
        if (scope->flags & BENDER_SCOPE_FUNCTION)
            break;
        scope = scope->parent;
    }
    bender_assert(scope, "could not add funcdef");
    bender_v_push(scope->defs, def);
    return bender_v_count(scope->defs) - 1;
}

/*
 * break
 *
 * jump :end or retn if in function
 */
static BenderSlot benderc_break(BenderFopts opts, int32_t argn, const Bender *argv) {
    BenderCompiler *c = opts.compiler;
    BenderScope *scope = c->scope;
    if (argn > 1) {
        benderc_cerror(c, "expected at most 1 argument");
        return benderc_cslot(bender_wrap_nil());
    }

    /* Find scope to break from */
    while (scope) {
        if (scope->flags & (BENDER_SCOPE_FUNCTION | BENDER_SCOPE_WHILE))
            break;
        scope = scope->parent;
    }
    if (NULL == scope) {
        benderc_cerror(c, "break must occur in while loop or closure");
        return benderc_cslot(bender_wrap_nil());
    }

    /* Emit code to break from that scope */
    BenderFopts subopts = benderc_fopts_default(c);
    if (scope->flags & BENDER_SCOPE_FUNCTION) {
        if (!(scope->flags & BENDER_SCOPE_WHILE) && argn) {
            /* Closure body with return argument */
            subopts.flags |= BENDER_FOPTS_TAIL;
            benderc_value(subopts, argv[0]);
            return benderc_cslot(bender_wrap_nil());
        } else {
            /* while loop IIFE or no argument */
            if (argn) {
                subopts.flags |= BENDER_FOPTS_DROP;
                benderc_value(subopts, argv[0]);
            }
            benderc_emit(c, JOP_RETURN_NIL);
            return benderc_cslot(bender_wrap_nil());
        }
    } else {
        if (argn) {
            subopts.flags |= BENDER_FOPTS_DROP;
            benderc_value(subopts, argv[0]);
        }
        /* Tag the instruction so the while special can turn it into a proper jump */
        benderc_emit(c, 0x80 | JOP_JUMP);
        return benderc_cslot(bender_wrap_nil());
    }
}

/*
 * :whiletop
 * ...
 * :condition
 * jump-if-not cond :done
 * ...
 * jump :whiletop
 * :done
 */
static BenderSlot benderc_while(BenderFopts opts, int32_t argn, const Bender *argv) {
    BenderCompiler *c = opts.compiler;
    BenderSlot cond;
    BenderFopts subopts = benderc_fopts_default(c);
    BenderScope tempscope;
    int32_t labelwt, labeld, labeljt, labelc, i;
    int infinite = 0;
    int is_nil_form = 0;
    int is_notnil_form = 0;
    uint8_t ifjmp = JOP_JUMP_IF;
    uint8_t ifnjmp = JOP_JUMP_IF_NOT;

    if (argn < 1) {
        benderc_cerror(c, "expected at least 1 argument to while");
        return benderc_cslot(bender_wrap_nil());
    }

    labelwt = bender_v_count(c->buffer);

    benderc_scope(&tempscope, c, BENDER_SCOPE_WHILE, "while");

    /* Check for `(= nil _)` or `(not= nil _)` in condition, and if so, use the
     * jmpnl or jmpnn instructions. This let's us implement `(each ...)`
     * more efficiently. */
    Bender condform = argv[0];
    if (benderc_check_nil_form(condform, &condform, BENDER_FUN_EQ)) {
        is_nil_form = 1;
        ifjmp = JOP_JUMP_IF_NIL;
        ifnjmp = JOP_JUMP_IF_NOT_NIL;
    }
    if (benderc_check_nil_form(condform, &condform, BENDER_FUN_NEQ)) {
        is_notnil_form = 1;
        ifjmp = JOP_JUMP_IF_NOT_NIL;
        ifnjmp = JOP_JUMP_IF_NIL;
    }

    /* Compile condition */
    cond = benderc_value(subopts, condform);

    /* Check for constant condition */
    if (cond.flags & BENDER_SLOT_CONSTANT) {
        /* Loop never executes */
        int never_executes = is_nil_form
                             ? !bender_checktype(cond.constant, BENDER_NIL)
                             : is_notnil_form
                             ? bender_checktype(cond.constant, BENDER_NIL)
                             : !bender_truthy(cond.constant);
        if (never_executes) {
            benderc_popscope(c);
            return benderc_cslot(bender_wrap_nil());
        }
        /* Infinite loop */
        infinite = 1;
    }

    /* Infinite loop does not need to check condition */
    labelc = infinite
             ? 0
             : benderc_emit_si(c, ifnjmp, cond, 0, 0);

    /* Compile body */
    for (i = 1; i < argn; i++) {
        subopts.flags = BENDER_FOPTS_DROP;
        benderc_freeslot(c, benderc_value(subopts, argv[i]));
    }

    /* Check if closure created in while scope. If so,
     * recompile in a function scope. */
    if (tempscope.flags & BENDER_SCOPE_CLOSURE) {
        subopts = benderc_fopts_default(c);
        tempscope.flags |= BENDER_SCOPE_UNUSED;
        benderc_popscope(c);
        if (c->buffer) bender_v__cnt(c->buffer) = labelwt;
        if (c->mapbuffer) bender_v__cnt(c->mapbuffer) = labelwt;

        benderc_scope(&tempscope, c, BENDER_SCOPE_FUNCTION, "while-iife");

        /* Recompile in the function scope */
        cond = benderc_value(subopts, condform);
        if (!(cond.flags & BENDER_SLOT_CONSTANT)) {
            /* If not an infinite loop, return nil when condition false */
            benderc_emit_si(c, ifjmp, cond, 2, 0);
            benderc_emit(c, JOP_RETURN_NIL);
        }
        for (i = 1; i < argn; i++) {
            subopts.flags = BENDER_FOPTS_DROP;
            benderc_freeslot(c, benderc_value(subopts, argv[i]));
        }
        /* But now add tail recursion */
        int32_t tempself = benderc_regalloc_temp(&tempscope.ra, BENDERC_REGTEMP_0);
        benderc_emit(c, JOP_LOAD_SELF | ((uint32_t) tempself << 8));
        benderc_emit(c, JOP_TAILCALL | ((uint32_t) tempself << 8));
        benderc_regalloc_freetemp(&c->scope->ra, tempself, BENDERC_REGTEMP_0);
        /* Compile function */
        BenderFuncDef *def = benderc_pop_funcdef(c, bender_cstring("while"));
        bender_def_addflags(def);
        int32_t defindex = benderc_addfuncdef(c, def);
        /* Do basic optimization */
        if (!(c->scope->flags & BENDER_SCOPE_UNUSED)) {
            benderc_optimize_funcdef(c, def);
        }
        /* And then load the closure and call it. */
        int32_t cloreg = benderc_regalloc_temp(&c->scope->ra, BENDERC_REGTEMP_0);
        benderc_emit(c, JOP_CLOSURE | ((uint32_t) cloreg << 8) | ((uint32_t) defindex << 16));
        benderc_emit(c, JOP_CALL | ((uint32_t) cloreg << 8) | ((uint32_t) cloreg << 16));
        benderc_regalloc_freetemp(&c->scope->ra, cloreg, BENDERC_REGTEMP_0);
        c->scope->flags |= BENDER_SCOPE_CLOSURE;
        return benderc_cslot(bender_wrap_nil());
    }

    /* Compile jump to :whiletop */
    labeljt = bender_v_count(c->buffer);
    benderc_emit(c, JOP_JUMP);

    /* Calculate jumps */
    labeld = bender_v_count(c->buffer);
    if (!infinite) {
        check_16bit_jump(c, labelc, labeld);
        c->buffer[labelc] |= (uint32_t)(labeld - labelc) << 16;
    }

    check_24bit_jump(c, labeljt, labelwt);
    c->buffer[labeljt] |= (uint32_t)(labelwt - labeljt) << 8;

    /* Calculate breaks */
    for (int32_t i = labelwt; i < labeld; i++) {
        if (c->buffer[i] == (0x80 | JOP_JUMP)) {
            check_24bit_jump(c, i, labeld);
            c->buffer[i] = JOP_JUMP | ((uint32_t)(labeld - i) << 8);
        }
    }

    /* Pop scope and return nil slot */
    benderc_popscope(c);

    return benderc_cslot(bender_wrap_nil());
}

static BenderSlot benderc_fn(BenderFopts opts, int32_t argn, const Bender *argv) {
    BenderCompiler *c = opts.compiler;
    BenderFuncDef *def;
    BenderSlot ret;
    Bender head;
    BenderScope fnscope;
    int32_t paramcount, argi, parami, arity, min_arity = 0, max_arity, defindex, i;
    BenderFopts subopts = benderc_fopts_default(c);
    const Bender *params;
    const char *errmsg = NULL;

    /* Function flags */
    int vararg = 0;
    int structarg = 0;
    int allow_extra = 0;
    int selfref = 0;
    int hasname = 0;
    int seenamp = 0;
    int seenopt = 0;
    int namedargs = 0;

    /* Begin function */
    c->scope->flags |= BENDER_SCOPE_CLOSURE;
    benderc_scope(&fnscope, c, BENDER_SCOPE_FUNCTION, "function");

    if (argn == 0) {
        errmsg = "expected at least 1 argument to function literal";
        goto error;
    }

    /* Read function parameters */
    parami = 0;
    head = argv[0];
    if (bender_checktype(head, BENDER_SYMBOL)) {
        selfref = 1;
        hasname = 1;
        parami = 1;
    } else if (bender_checktype(head, BENDER_KEYWORD)) {
        hasname = 1;
        parami = 1;
    }
    if (parami >= argn || !bender_checktype(argv[parami], BENDER_TUPLE)) {
        errmsg = "expected function parameters";
        goto error;
    }

    /* Keep track of destructured parameters */
    BenderSlot *destructed_params = NULL;
    BenderSlot *named_params = NULL;
    BenderTable *named_table = NULL;
    BenderSlot named_slot;

    /* Compile function parameters */
    params = bender_unwrap_tuple(argv[parami]);
    paramcount = bender_tuple_length(params);
    arity = paramcount;
    for (i = 0; i < paramcount; i++) {
        Bender param = params[i];
        if (namedargs) {
            arity--;
            if (!bender_checktype(param, BENDER_SYMBOL)) {
                errmsg = "only named arguments can follow &named";
                goto error;
            }
            Bender key = bender_wrap_keyword(bender_unwrap_symbol(param));
            bender_table_put(named_table, key, param);
            bender_v_push(named_params, benderc_farslot(c));
        } else if (bender_checktype(param, BENDER_SYMBOL)) {
            /* Check for varargs and unfixed arity */
            const uint8_t *sym = bender_unwrap_symbol(param);
            if (sym[0] == '^') {
                /* Type hints belong on defn (^type before a parameter),
                 * which strips them before the fn special ever sees the
                 * arglist. A raw fn with ^ params would otherwise bind
                 * the hint as a parameter and silently change arity. */
                errmsg = "parameter names cannot start with ^ (type hints belong on defn)";
                goto error;
            }
            if (sym[0] == '&') {
                if (!bender_cstrcmp(sym, "&")) {
                    if (seenamp) {
                        errmsg = "& in unexpected location";
                        goto error;
                    } else if (i == paramcount - 1) {
                        allow_extra = 1;
                        arity--;
                    } else if (i == paramcount - 2) {
                        vararg = 1;
                        arity -= 2;
                    } else {
                        errmsg = "& in unexpected location";
                        goto error;
                    }
                    seenamp = 1;
                } else if (!bender_cstrcmp(sym, "&opt")) {
                    if (seenopt) {
                        errmsg = "only one &opt allowed";
                        goto error;
                    } else if (i == paramcount - 1) {
                        errmsg = "&opt cannot be last item in parameter list";
                        goto error;
                    }
                    min_arity = i;
                    arity--;
                    seenopt = 1;
                } else if (!bender_cstrcmp(sym, "&keys")) {
                    if (seenamp) {
                        errmsg = "&keys in unexpected location";
                        goto error;
                    } else if (i == paramcount - 2) {
                        vararg = 1;
                        structarg = 1;
                        arity -= 2;
                    } else {
                        errmsg = "&keys in unexpected location";
                        goto error;
                    }
                    seenamp = 1;
                } else if (!bender_cstrcmp(sym, "&named")) {
                    if (seenamp) {
                        errmsg = "&named in unexpected location";
                        goto error;
                    }
                    vararg = 1;
                    structarg = 1;
                    arity--;
                    seenamp = 1;
                    namedargs = 1;
                    named_table = bender_table(10);
                    named_slot = benderc_farslot(c);
                } else {
                    benderc_nameslot(c, sym, benderc_farslot(c), 0);
                }
            } else {
                benderc_nameslot(c, sym, benderc_farslot(c), 0);
            }
        } else {
            bender_v_push(destructed_params, benderc_farslot(c));
        }
    }

    /* Compile named arguments */
    if (namedargs) {
        Bender param = bender_wrap_table(named_table);
        destructure(c, param, named_slot, defleaf, NULL);
        benderc_freeslot(c, named_slot);
        bender_v_free(named_params);
    }

    /* Compile destructed params */
    int32_t j = 0;
    for (i = 0; i < paramcount; i++) {
        Bender param = params[i];
        if (!bender_checktype(param, BENDER_SYMBOL)) {
            bender_assert(bender_v_count(destructed_params) > j, "out of bounds");
            BenderSlot reg = destructed_params[j++];
            destructure(c, param, reg, defleaf, NULL);
            benderc_freeslot(c, reg);
        }
    }
    bender_v_free(destructed_params);

    max_arity = (vararg || allow_extra) ? INT32_MAX : arity;
    if (!seenopt) min_arity = arity;

    /* Check for self ref (also avoid if arguments shadow own name) */
    if (selfref) {
        /* Check if the parameters shadow the function name. If so, don't
         * emit JOP_LOAD_SELF and add a binding since that most users
         * seem to expect that function parameters take precedence over the
         * function name */
        const uint8_t *sym = bender_unwrap_symbol(head);
        int32_t len = bender_v_count(c->scope->syms);
        int found = 0;
        for (int32_t i = 0; i < len; i++) {
            if (c->scope->syms[i].sym == sym) {
                found = 1;
            }
        }
        if (!found) {
            BenderSlot slot = benderc_farslot(c);
            slot.flags = BENDER_SLOT_NAMED | BENDER_FUNCTION;
            benderc_emit_s(c, JOP_LOAD_SELF, slot, 1);
            /* We should figure out a better way to avoid `(def x 1) (def x :shadow (fn x [...] ...))` triggering a
             * shadow lint for the last x */
            benderc_nameslot(c, sym, slot, BENDER_DEFFLAG_NO_UNUSED | BENDER_DEFFLAG_NO_SHADOWCHECK);
        }
    }

    /* Compile function body */
    if (parami + 1 == argn) {
        benderc_emit(c, JOP_RETURN_NIL);
    } else {
        for (argi = parami + 1; argi < argn; argi++) {
            subopts.flags = (argi == (argn - 1)) ? BENDER_FOPTS_TAIL : BENDER_FOPTS_DROP;
            benderc_value(subopts, argv[argi]);
            if (c->result.status == BENDER_COMPILE_ERROR)
                goto error2;
        }
    }

    /* Build function */
    BenderString name = hasname ? bender_unwrap_symbol(head) : NULL;
    def = benderc_pop_funcdef(c, name);
    def->arity = arity;
    def->min_arity = min_arity;
    def->max_arity = max_arity;
    if (named_table != NULL) {
        def->named_args_count = named_table->count;
    }
    if (vararg) def->flags |= BENDER_FUNCDEF_FLAG_VARARG;
    if (structarg) def->flags |= BENDER_FUNCDEF_FLAG_STRUCTARG;
    if (namedargs) def->flags |= BENDER_FUNCDEF_FLAG_NAMEDARGS;

    bender_def_addflags(def);
    defindex = benderc_addfuncdef(c, def);

    /* Ensure enough slots for vararg function. */
    if (arity + vararg > def->slotcount) def->slotcount = arity + vararg;

    /* Do basic optimization */
    if (!(fnscope.flags & BENDER_SCOPE_UNUSED)) {
        benderc_optimize_funcdef(c, def);
    }

    /* Instantiate closure */
    ret = benderc_gettarget(opts);
    benderc_emit_su(c, JOP_CLOSURE, ret, defindex, 1);
    return ret;

error:
    benderc_cerror(c, errmsg);
error2:
    benderc_popscope(c);
    return benderc_cslot(bender_wrap_nil());
}

/* Keep in lexicographic order */
static const BenderSpecial benderc_specials[] = {
    {"break", benderc_break},
    {"def", benderc_def},
    {"do", benderc_do},
    {"fn", benderc_fn},
    {"if", benderc_if},
    {"quasiquote", benderc_quasiquote},
    {"quote", benderc_quote},
    {"set", benderc_varset},
    {"splice", benderc_splice},
    {"unquote", benderc_unquote},
    {"upscope", benderc_upscope},
    {"var", benderc_var},
    {"while", benderc_while}
};

/* Find a special */
const BenderSpecial *benderc_special(const uint8_t *name) {
    return bender_strbinsearch(
               &benderc_specials,
               sizeof(benderc_specials) / sizeof(BenderSpecial),
               sizeof(BenderSpecial),
               name);
}
