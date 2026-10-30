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
#include "symcache.h"
#include "gc.h"
#include "util.h"
#endif

/* Create a new empty tuple of the given size. This will return memory
 * which should be filled with Benders. The memory will not be collected until
 * bender_tuple_end is called. */
Bender *bender_tuple_begin(int32_t length) {
    size_t size = sizeof(BenderTupleHead) + ((size_t) length * sizeof(Bender));
    BenderTupleHead *head = bender_gcalloc(BENDER_MEMORY_TUPLE, size);
    head->sm_line = -1;
    head->sm_column = -1;
    head->length = length;
    return (Bender *)(head->data);
}

/* Finish building a tuple */
const Bender *bender_tuple_end(Bender *tuple) {
    bender_tuple_hash(tuple) = bender_array_calchash(tuple, bender_tuple_length(tuple));
    return (const Bender *)tuple;
}

/* Build a tuple with n values */
const Bender *bender_tuple_n(const Bender *values, int32_t n) {
    Bender *t = bender_tuple_begin(n);
    safe_memcpy(t, values, sizeof(Bender) * n);
    return bender_tuple_end(t);
}

/* C Functions */

BENDER_CORE_FN(cfun_tuple_brackets,
              "(tuple/brackets & xs)",
              "Creates a new bracketed tuple containing the elements xs.") {
    const Bender *tup = bender_tuple_n(argv, argc);
    bender_tuple_flag(tup) |= BENDER_TUPLE_FLAG_BRACKETCTOR;
    return bender_wrap_tuple(tup);
}

BENDER_CORE_FN(cfun_tuple_slice,
              "(tuple/slice ind &opt start end)",
              "Returns a tuple based on a range of an indexed "
              "type `ind`. The range is specified by optional "
              "index arguments `start` and `end` defaulting to 0 "
              "and the length of `ind` respectively. When "
              "non-negative, `start` is inclusive and `end` is "
              "exclusive, i.e. [`start`, `end`), and their "
              "values can range from 0 through the length of "
              "`ind`. When negative, `start` is exclusive and "
              "`end` is inclusive, i.e. (`start`, `end`], and "
              "their values can range from the negative of one "
              "more than the length of `ind` through -1. If "
              "`start` or `end` is out of range, an error is "
              "raised.") {
    BenderView view = bender_getindexed(argv, 0);
    BenderRange range = bender_getslice(argc, argv);
    return bender_wrap_tuple(bender_tuple_n(view.items + range.start, range.end - range.start));
}

BENDER_CORE_FN(cfun_tuple_type,
              "(tuple/type tup)",
              "Checks how the tuple was constructed. Will return the keyword "
              ":brackets if the tuple was parsed with brackets, and :parens "
              "otherwise. The two types of tuples will behave the same most of "
              "the time, but will print differently and be treated differently by "
              "the compiler.") {
    bender_fixarity(argc, 1);
    const Bender *tup = bender_gettuple(argv, 0);
    if (bender_tuple_flag(tup) & BENDER_TUPLE_FLAG_BRACKETCTOR) {
        return bender_ckeywordv("brackets");
    } else {
        return bender_ckeywordv("parens");
    }
}

BENDER_CORE_FN(cfun_tuple_sourcemap,
              "(tuple/sourcemap tup)",
              "Returns the sourcemap metadata attached to a tuple `tup`, "
              "which is another tuple `[line column]`.") {
    bender_fixarity(argc, 1);
    const Bender *tup = bender_gettuple(argv, 0);
    Bender contents[2];
    contents[0] = bender_wrap_integer(bender_tuple_head(tup)->sm_line);
    contents[1] = bender_wrap_integer(bender_tuple_head(tup)->sm_column);
    return bender_wrap_tuple(bender_tuple_n(contents, 2));
}

BENDER_CORE_FN(cfun_tuple_setmap,
              "(tuple/setmap tup line col)",
              "Set the sourcemap metadata on a tuple, `tup`. `line` and "
              "`col` should be integers.") {
    bender_fixarity(argc, 3);
    const Bender *tup = bender_gettuple(argv, 0);
    bender_tuple_head(tup)->sm_line = bender_getinteger(argv, 1);
    bender_tuple_head(tup)->sm_column = bender_getinteger(argv, 2);
    return argv[0];
}

BENDER_CORE_FN(cfun_tuple_join,
              "(tuple/join & parts)",
              "Create a tuple by joining together other tuples and arrays.") {
    bender_arity(argc, 0, -1);
    int32_t total_len = 0;
    for (int32_t i = 0; i < argc; i++) {
        int32_t len = 0;
        const Bender *vals = NULL;
        if (!bender_indexed_view(argv[i], &vals, &len)) {
            bender_panicf("expected indexed type for argument %d, got %v", i, argv[i]);
        }
        if (INT32_MAX - total_len < len) {
            bender_panic("tuple too large");
        }
        total_len += len;
    }
    Bender *tup = bender_tuple_begin(total_len);
    Bender *tup_cursor = tup;
    for (int32_t i = 0; i < argc; i++) {
        int32_t len = 0;
        const Bender *vals = NULL;
        bender_indexed_view(argv[i], &vals, &len);
        memcpy(tup_cursor, vals, len * sizeof(Bender));
        tup_cursor += len;
    }
    return bender_wrap_tuple(bender_tuple_end(tup));
}

/* Load the tuple module */
void bender_lib_tuple(BenderTable *env) {
    BenderRegExt tuple_cfuns[] = {
        BENDER_CORE_REG("tuple/brackets", cfun_tuple_brackets),
        BENDER_CORE_REG("tuple/slice", cfun_tuple_slice),
        BENDER_CORE_REG("tuple/type", cfun_tuple_type),
        BENDER_CORE_REG("tuple/sourcemap", cfun_tuple_sourcemap),
        BENDER_CORE_REG("tuple/setmap", cfun_tuple_setmap),
        BENDER_CORE_REG("tuple/join", cfun_tuple_join),
        BENDER_REG_END
    };
    bender_core_cfuns_ext(env, NULL, tuple_cfuns);
}
