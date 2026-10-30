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
#include "gc.h"
#include "util.h"
#include "state.h"
#endif

#include <string.h>

/* Begin building a string */
uint8_t *bender_string_begin(int32_t length) {
    BenderStringHead *head = bender_gcalloc(BENDER_MEMORY_STRING, sizeof(BenderStringHead) + (size_t) length + 1);
    head->length = length;
    uint8_t *data = (uint8_t *)head->data;
    data[length] = 0;
    return data;
}

/* Finish building a string */
const uint8_t *bender_string_end(uint8_t *str) {
    bender_string_hash(str) = bender_string_calchash(str, bender_string_length(str));
    return str;
}

/* Load a buffer as a string */
const uint8_t *bender_string(const uint8_t *buf, int32_t len) {
    BenderStringHead *head = bender_gcalloc(BENDER_MEMORY_STRING, sizeof(BenderStringHead) + (size_t) len + 1);
    head->length = len;
    head->hash = bender_string_calchash(buf, len);
    uint8_t *data = (uint8_t *)head->data;
    safe_memcpy(data, buf, len);
    data[len] = 0;
    return data;
}

/* Compare two strings */
int bender_string_compare(const uint8_t *lhs, const uint8_t *rhs) {
    int32_t xlen = bender_string_length(lhs);
    int32_t ylen = bender_string_length(rhs);
    int32_t len = xlen > ylen ? ylen : xlen;
    int res = memcmp(lhs, rhs, len);
    if (res) return res > 0 ? 1 : -1;
    if (xlen == ylen) return 0;
    return xlen < ylen ? -1 : 1;
}

/* Compare a bender string with a piece of memory */
int bender_string_equalconst(const uint8_t *lhs, const uint8_t *rhs, int32_t rlen, int32_t rhash) {
    int32_t lhash = bender_string_hash(lhs);
    int32_t llen = bender_string_length(lhs);
    if (lhash != rhash || llen != rlen)
        return 0;
    if (lhs == rhs)
        return 1;
    return !memcmp(lhs, rhs, rlen);
}

/* Check if two strings are equal */
int bender_string_equal(const uint8_t *lhs, const uint8_t *rhs) {
    return bender_string_equalconst(lhs, rhs,
                                   bender_string_length(rhs), bender_string_hash(rhs));
}

/* Load a c string */
const uint8_t *bender_cstring(const char *str) {
    return bender_string((const uint8_t *)str, (int32_t)strlen(str));
}

/* Knuth Morris Pratt Algorithm */

struct kmp_state {
    int32_t i;
    int32_t j;
    int32_t textlen;
    int32_t patlen;
    int32_t *lookup;
    const uint8_t *text;
    const uint8_t *pat;
};

static void kmp_init(
    struct kmp_state *s,
    const uint8_t *text, int32_t textlen,
    const uint8_t *pat, int32_t patlen) {
    if (patlen == 0) {
        bender_panic("expected non-empty pattern");
    }
    int32_t *lookup = bender_calloc(patlen, sizeof(int32_t));
    if (!lookup) {
        BENDER_OUT_OF_MEMORY;
    }
    s->lookup = lookup;
    s->i = 0;
    s->j = 0;
    s->text = text;
    s->pat = pat;
    s->textlen = textlen;
    s->patlen = patlen;
    /* Init state machine */
    {
        int32_t i, j;
        for (i = 1, j = 0; i < patlen; i++) {
            while (j && pat[j] != pat[i]) j = lookup[j - 1];
            if (pat[j] == pat[i]) j++;
            lookup[i] = j;
        }
    }
}

static void kmp_deinit(struct kmp_state *state) {
    bender_free(state->lookup);
}

static void kmp_seti(struct kmp_state *state, int32_t i) {
    state->i = i;
    state->j = 0;
}

static int32_t kmp_next(struct kmp_state *state) {
    int32_t i = state->i;
    int32_t j = state->j;
    int32_t textlen = state->textlen;
    int32_t patlen = state->patlen;
    const uint8_t *text = state->text;
    const uint8_t *pat = state->pat;
    int32_t *lookup = state->lookup;
    while (i < textlen) {
        if (text[i] == pat[j]) {
            if (j == patlen - 1) {
                state->i = i + 1;
                state->j = lookup[j];
                return i - j;
            } else {
                i++;
                j++;
            }
        } else {
            if (j > 0) {
                j = lookup[j - 1];
            } else {
                i++;
            }
        }
    }
    return -1;
}

/* CFuns */

BENDER_CORE_FN(cfun_string_slice,
              "(string/slice bytes &opt start end)",
              "Returns a string based on a range of a byte "
              "sequence `bytes`. The range is specified by "
              "optional index arguments `start` and `end` "
              "defaulting to 0 and the length of `bytes` "
              "respectively. When non-negative, `start` is "
              "inclusive and `end` is exclusive, i.e. [`start`, "
              "`end`), and their values can range from 0 through "
              "the length of `bytes`. When negative, `start` is "
              "exclusive and `end` is inclusive, i.e. (`start`, "
              "`end`], and their values can range from the "
              "negative of one more than the length of `bytes` "
              "through -1. If `start` or `end` is out of range, "
              "an error is raised.") {
    BenderByteView view = bender_getbytes(argv, 0);
    BenderRange range = bender_getslice(argc, argv);
    return bender_stringv(view.bytes + range.start, range.end - range.start);
}

BENDER_CORE_FN(cfun_symbol_slice,
              "(symbol/slice bytes &opt start end)",
              "Same as `string/slice`, but returns a symbol.") {
    BenderByteView view = bender_getbytes(argv, 0);
    BenderRange range = bender_getslice(argc, argv);
    return bender_symbolv(view.bytes + range.start, range.end - range.start);
}

BENDER_CORE_FN(cfun_keyword_slice,
              "(keyword/slice bytes &opt start end)",
              "Same as `string/slice`, but returns a keyword.") {
    BenderByteView view = bender_getbytes(argv, 0);
    BenderRange range = bender_getslice(argc, argv);
    return bender_keywordv(view.bytes + range.start, range.end - range.start);
}

BENDER_CORE_FN(cfun_string_repeat,
              "(string/repeat bytes n)",
              "Returns a string that is `n` copies of `bytes` concatenated.") {
    bender_fixarity(argc, 2);
    BenderByteView view = bender_getbytes(argv, 0);
    int32_t rep = bender_getinteger(argv, 1);
    if (rep < 0) bender_panic("expected non-negative number of repetitions");
    if (rep == 0) return bender_cstringv("");
    int64_t mulres = (int64_t) rep * view.len;
    if (mulres > INT32_MAX) bender_panic("result string is too long");
    uint8_t *newbuf = bender_string_begin((int32_t) mulres);
    uint8_t *end = newbuf + mulres;
    for (uint8_t *p = newbuf; p < end; p += view.len) {
        safe_memcpy(p, view.bytes, view.len);
    }
    return bender_wrap_string(bender_string_end(newbuf));
}

BENDER_CORE_FN(cfun_string_bytes,
              "(string/bytes str)",
              "Returns a tuple of integers that are the byte values of the string.") {
    bender_fixarity(argc, 1);
    BenderByteView view = bender_getbytes(argv, 0);
    Bender *tup = bender_tuple_begin(view.len);
    int32_t i;
    for (i = 0; i < view.len; i++) {
        tup[i] = bender_wrap_integer((int32_t) view.bytes[i]);
    }
    return bender_wrap_tuple(bender_tuple_end(tup));
}

BENDER_CORE_FN(cfun_string_frombytes,
              "(string/from-bytes & byte-vals)",
              "Creates a string from integer parameters with byte values. All integers "
              "will be coerced to the range of 1 byte 0-255.") {
    int32_t i;
    uint8_t *buf = bender_string_begin(argc);
    for (i = 0; i < argc; i++) {
        int32_t c = bender_getinteger(argv, i);
        buf[i] = c & 0xFF;
    }
    return bender_wrap_string(bender_string_end(buf));
}

BENDER_CORE_FN(cfun_string_asciilower,
              "(string/ascii-lower str)",
              "Returns a new string where all bytes are replaced with the "
              "lowercase version of themselves in ASCII. Does only a very simple "
              "case check, meaning no unicode support.") {
    bender_fixarity(argc, 1);
    BenderByteView view = bender_getbytes(argv, 0);
    uint8_t *buf = bender_string_begin(view.len);
    for (int32_t i = 0; i < view.len; i++) {
        uint8_t c = view.bytes[i];
        if (c >= 65 && c <= 90) {
            buf[i] = c + 32;
        } else {
            buf[i] = c;
        }
    }
    return bender_wrap_string(bender_string_end(buf));
}

BENDER_CORE_FN(cfun_string_asciiupper,
              "(string/ascii-upper str)",
              "Returns a new string where all bytes are replaced with the "
              "uppercase version of themselves in ASCII. Does only a very simple "
              "case check, meaning no unicode support.") {
    bender_fixarity(argc, 1);
    BenderByteView view = bender_getbytes(argv, 0);
    uint8_t *buf = bender_string_begin(view.len);
    for (int32_t i = 0; i < view.len; i++) {
        uint8_t c = view.bytes[i];
        if (c >= 97 && c <= 122) {
            buf[i] = c - 32;
        } else {
            buf[i] = c;
        }
    }
    return bender_wrap_string(bender_string_end(buf));
}

BENDER_CORE_FN(cfun_string_reverse,
              "(string/reverse str)",
              "Returns a string that is the reversed version of `str`.") {
    bender_fixarity(argc, 1);
    BenderByteView view = bender_getbytes(argv, 0);
    uint8_t *buf = bender_string_begin(view.len);
    int32_t i, j;
    for (i = 0, j = view.len - 1; i < view.len; i++, j--) {
        buf[i] = view.bytes[j];
    }
    return bender_wrap_string(bender_string_end(buf));
}

static void findsetup(int32_t argc, Bender *argv, struct kmp_state *s, int32_t extra) {
    bender_arity(argc, 2, 3 + extra);
    BenderByteView pat = bender_getbytes(argv, 0);
    BenderByteView text = bender_getbytes(argv, 1);
    int32_t start = 0;
    if (argc >= 3) {
        start = bender_getinteger(argv, 2);
        if (start < 0) bender_panic("expected non-negative start index");
    }
    kmp_init(s, text.bytes, text.len, pat.bytes, pat.len);
    s->i = start;
}

BENDER_CORE_FN(cfun_string_find,
              "(string/find patt str &opt start)",
              "Searches for the first instance of pattern `patt` in string "
              "`str`. Returns the index of the first character in `patt` if "
              "found, otherwise returns nil. Search begins from `start` if "
              "provided.") {
    int32_t result;
    struct kmp_state state;
    findsetup(argc, argv, &state, 0);
    result = kmp_next(&state);
    kmp_deinit(&state);
    return result < 0
           ? bender_wrap_nil()
           : bender_wrap_integer(result);
}

BENDER_CORE_FN(cfun_string_hasprefix,
              "(string/has-prefix? pfx str)",
              "Tests whether `str` starts with `pfx`.") {
    bender_fixarity(argc, 2);
    BenderByteView prefix = bender_getbytes(argv, 0);
    BenderByteView str = bender_getbytes(argv, 1);
    return str.len < prefix.len
           ? bender_wrap_false()
           : bender_wrap_boolean(memcmp(prefix.bytes, str.bytes, prefix.len) == 0);
}

BENDER_CORE_FN(cfun_string_hassuffix,
              "(string/has-suffix? sfx str)",
              "Tests whether `str` ends with `sfx`.") {
    bender_fixarity(argc, 2);
    BenderByteView suffix = bender_getbytes(argv, 0);
    BenderByteView str = bender_getbytes(argv, 1);
    return str.len < suffix.len
           ? bender_wrap_false()
           : bender_wrap_boolean(memcmp(suffix.bytes,
                                       str.bytes + str.len - suffix.len,
                                       suffix.len) == 0);
}

BENDER_CORE_FN(cfun_string_findall,
              "(string/find-all patt str &opt start)",
              "Searches for all instances of pattern `patt` in string "
              "`str`. Returns an array of all indices of found patterns. "
              "Overlapping instances of the pattern are counted "
              "individually, meaning a byte in `str` may contribute to "
              "multiple found patterns. Search begins from `start` if "
              "provided.") {
    int32_t result;
    struct kmp_state state;
    findsetup(argc, argv, &state, 0);
    BenderArray *array = bender_array(0);
    while ((result = kmp_next(&state)) >= 0) {
        bender_array_push(array, bender_wrap_integer(result));
    }
    kmp_deinit(&state);
    return bender_wrap_array(array);
}

struct replace_state {
    struct kmp_state kmp;
    Bender subst;
};

static void replacesetup(int32_t argc, Bender *argv, struct replace_state *s) {
    bender_arity(argc, 3, 4);
    BenderByteView pat = bender_getbytes(argv, 0);
    Bender subst = argv[1];
    BenderByteView text = bender_getbytes(argv, 2);
    int32_t start = 0;
    if (argc == 4) {
        start = bender_getinteger(argv, 3);
        if (start < 0) bender_panic("expected non-negative start index");
    }
    kmp_init(&s->kmp, text.bytes, text.len, pat.bytes, pat.len);
    s->kmp.i = start;
    s->subst = subst;
}

BENDER_CORE_FN(cfun_string_replace,
              "(string/replace patt subst str)",
              "Replace the first occurrence of `patt` with `subst` in the string `str`. "
              "If `subst` is a function, it will be called with `patt` only if a match is found, "
              "and should return the actual replacement text to use. "
              "Will return the new string if `patt` is found, otherwise returns `str`.") {
    int32_t result;
    struct replace_state s;
    uint8_t *buf;
    replacesetup(argc, argv, &s);
    result = kmp_next(&s.kmp);
    if (result < 0) {
        kmp_deinit(&s.kmp);
        return bender_stringv(s.kmp.text, s.kmp.textlen);
    }
    BenderByteView subst = bender_text_substitution(&s.subst, s.kmp.text + result, s.kmp.patlen, NULL);
    buf = bender_string_begin(s.kmp.textlen - s.kmp.patlen + subst.len);
    safe_memcpy(buf, s.kmp.text, result);
    safe_memcpy(buf + result, subst.bytes, subst.len);
    safe_memcpy(buf + result + subst.len,
                s.kmp.text + result + s.kmp.patlen,
                s.kmp.textlen - result - s.kmp.patlen);
    kmp_deinit(&s.kmp);
    return bender_wrap_string(bender_string_end(buf));
}

BENDER_CORE_FN(cfun_string_replaceall,
              "(string/replace-all patt subst str)",
              "Replace all instances of `patt` with `subst` in the string `str`. Overlapping "
              "matches will not be counted, only the first match in such a span will be replaced. "
              "If `subst` is a function, it will be called with `patt` once for each match, "
              "and should return the actual replacement text to use. "
              "Will return the new string if `patt` is found, otherwise returns `str`.") {
    int32_t result;
    struct replace_state s;
    BenderBuffer b;
    int32_t lastindex = 0;
    replacesetup(argc, argv, &s);
    bender_buffer_init(&b, s.kmp.textlen);
    while ((result = kmp_next(&s.kmp)) >= 0) {
        BenderByteView subst = bender_text_substitution(&s.subst, s.kmp.text + result, s.kmp.patlen, NULL);
        bender_buffer_push_bytes(&b, s.kmp.text + lastindex, result - lastindex);
        bender_buffer_push_bytes(&b, subst.bytes, subst.len);
        lastindex = result + s.kmp.patlen;
        kmp_seti(&s.kmp, lastindex);
    }
    bender_buffer_push_bytes(&b, s.kmp.text + lastindex, s.kmp.textlen - lastindex);
    const uint8_t *ret = bender_string(b.data, b.count);
    bender_buffer_deinit(&b);
    kmp_deinit(&s.kmp);
    return bender_wrap_string(ret);
}

BENDER_CORE_FN(cfun_string_split,
              "(string/split sep str &opt start limit)",
              "Splits a string `str` with separator `sep` and returns an "
              "array of substrings. The substrings will not contain `sep`. "
              "If `sep` is not found, the returned array will have one "
              "element. Will start searching for `sep` at the index `start` "
              "(if provided), and return up to a maximum of `limit` results "
              "(if provided).") {
    int32_t result;
    BenderArray *array;
    struct kmp_state state;
    int32_t limit = -1, lastindex = 0;
    if (argc == 4) {
        limit = bender_getinteger(argv, 3);
    }
    findsetup(argc, argv, &state, 1);
    array = bender_array(0);
    while ((result = kmp_next(&state)) >= 0 && --limit) {
        const uint8_t *slice = bender_string(state.text + lastindex, result - lastindex);
        bender_array_push(array, bender_wrap_string(slice));
        lastindex = result + state.patlen;
        kmp_seti(&state, lastindex);
    }
    const uint8_t *slice = bender_string(state.text + lastindex, state.textlen - lastindex);
    bender_array_push(array, bender_wrap_string(slice));
    kmp_deinit(&state);
    return bender_wrap_array(array);
}

BENDER_CORE_FN(cfun_string_checkset,
              "(string/check-set set-bytes bytes)",
              "Checks that only bytes from `set-bytes` are contained in "
              "`bytes`. Returns true if all bytes in `bytes` appear in "
              "`set-bytes` or false if at least one byte in `bytes` does "
              "not appear in `set-bytes`. Both arguments are bytes types.") {
    uint32_t bitset[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    bender_fixarity(argc, 2);
    BenderByteView set = bender_getbytes(argv, 0);
    BenderByteView str = bender_getbytes(argv, 1);
    /* Populate set */
    for (int32_t i = 0; i < set.len; i++) {
        int index = set.bytes[i] >> 5;
        uint32_t mask = (uint32_t) 1 << (set.bytes[i] & 0x1F);
        bitset[index] |= mask;
    }
    /* Check set */
    for (int32_t i = 0; i < str.len; i++) {
        int index = str.bytes[i] >> 5;
        uint32_t mask = (uint32_t) 1 << (str.bytes[i] & 0x1F);
        if (!(bitset[index] & mask)) {
            return bender_wrap_false();
        }
    }
    return bender_wrap_true();
}

BENDER_CORE_FN(cfun_string_join,
              "(string/join parts &opt sep)",
              "Joins an array of strings into one string, optionally separated by "
              "a separator string `sep`.") {
    bender_arity(argc, 1, 2);
    BenderView parts = bender_getindexed(argv, 0);
    BenderByteView joiner;
    if (argc == 2) {
        joiner = bender_getbytes(argv, 1);
    } else {
        joiner.bytes = NULL;
        joiner.len = 0;
    }
    /* Check args */
    int32_t i;
    int64_t finallen = 0;
    for (i = 0; i < parts.len; i++) {
        const uint8_t *chunk;
        int32_t chunklen = 0;
        if (!bender_bytes_view(parts.items[i], &chunk, &chunklen)) {
            bender_panicf("item %d of parts is not a byte sequence, got %v", i, parts.items[i]);
        }
        if (i) finallen += joiner.len;
        finallen += chunklen;
        if (finallen > INT32_MAX)
            bender_panic("result string too long");
    }
    uint8_t *buf, *out;
    out = buf = bender_string_begin((int32_t) finallen);
    for (i = 0; i < parts.len; i++) {
        const uint8_t *chunk = NULL;
        int32_t chunklen = 0;
        if (i) {
            safe_memcpy(out, joiner.bytes, joiner.len);
            out += joiner.len;
        }
        bender_bytes_view(parts.items[i], &chunk, &chunklen);
        safe_memcpy(out, chunk, chunklen);
        out += chunklen;
    }
    return bender_wrap_string(bender_string_end(buf));
}

BENDER_CORE_FN(cfun_string_format,
              "(string/format fmt & args)",
              "Similar to C's `snprintf()`, but specialized for working with "
              "Bender values. Returns a new string.\n"
              "\n"
              "The following conversion specifiers are supported (upper "
              "case variants generate upper case output):\n"
              "\n"
              "* `c` - ASCII character\n"
              "* `d`, `i` - integer, formatted as a decimal number\n"
              "* `x`, `X` - integer, formatted as a hexadecimal number\n"
              "* `o` - integer, formatted as an octal number\n"
              "* `f`, `F` - floating point number, formatted as a decimal number\n"
              "* `e`, `E` - floating point number, formatted in scientific notation\n"
              "* `g`, `G` - floating point number, formatted in its shortest form\n"
              "* `a`, `A` - floating point number, formatted as a hexadecimal number\n"
              "* `s` - formatted as a string, precision indicates padding and maximum length\n"
              "* `t` - emit the type of the given value\n"
              "* `v` - format with `describe`\n"
              "* `V` - format with `string`\n"
              "* `j` - format to jdn (Bender data notation)\n"
              "\n"
              "The following conversion specifiers are used for "
              "\"pretty-printing\", where the upper case variants generate "
              "colored output. These specifiers can take a precision "
              "argument to specify the maximum nesting depth to print. "
              "The multiline specifiers can also take a width argument, "
              "which defaults to 80 columns.\n"
              "\n"
              "* `p`, `P` - pretty format, truncating if needed\n"
              "* `m`, `M` - pretty format without truncating\n"
              "* `q`, `Q` - pretty format on one line, truncating if needed\n"
              "* `n`, `N` - pretty format on one line without truncation") {
    bender_arity(argc, 1, -1);
    BenderBuffer *buffer = bender_buffer(0);
    const char *strfrmt = (const char *) bender_getstring(argv, 0);
    bender_buffer_format(buffer, strfrmt, 0, argc, argv);
    return bender_stringv(buffer->data, buffer->count);
}

static int trim_help_checkset(BenderByteView set, uint8_t x) {
    for (int32_t j = 0; j < set.len; j++)
        if (set.bytes[j] == x)
            return 1;
    return 0;
}

static int32_t trim_help_leftedge(BenderByteView str, BenderByteView set) {
    for (int32_t i = 0; i < str.len; i++)
        if (!trim_help_checkset(set, str.bytes[i]))
            return i;
    return str.len;
}

static int32_t trim_help_rightedge(BenderByteView str, BenderByteView set) {
    for (int32_t i = str.len - 1; i >= 0; i--)
        if (!trim_help_checkset(set, str.bytes[i]))
            return i + 1;
    return 0;
}

static void trim_help_args(int32_t argc, Bender *argv, BenderByteView *str, BenderByteView *set) {
    bender_arity(argc, 1, 2);
    *str = bender_getbytes(argv, 0);
    if (argc >= 2) {
        *set = bender_getbytes(argv, 1);
    } else {
        set->bytes = (const uint8_t *)(" \t\r\n\v\f");
        set->len = 6;
    }
}

BENDER_CORE_FN(cfun_string_trim,
              "(string/trim str &opt bytes)",
              "Trim leading and trailing whitespace from a byte sequence "
              "`str`. If argument `bytes` is provided, treat only "
              "characters in `bytes` as whitespace.") {
    BenderByteView str, set;
    trim_help_args(argc, argv, &str, &set);
    int32_t left_edge = trim_help_leftedge(str, set);
    int32_t right_edge = trim_help_rightedge(str, set);
    if (right_edge < left_edge)
        return bender_stringv(NULL, 0);
    return bender_stringv(str.bytes + left_edge, right_edge - left_edge);
}

BENDER_CORE_FN(cfun_string_triml,
              "(string/triml str &opt bytes)",
              "Trim leading whitespace from a byte sequence `str`. If "
              "argument `bytes` is provided, treat only characters in "
              "`bytes` as whitespace.") {
    BenderByteView str, set;
    trim_help_args(argc, argv, &str, &set);
    int32_t left_edge = trim_help_leftedge(str, set);
    return bender_stringv(str.bytes + left_edge, str.len - left_edge);
}

BENDER_CORE_FN(cfun_string_trimr,
              "(string/trimr str &opt bytes)",
              "Trim trailing whitespace from a byte sequence `str`. If "
              "argument `bytes` is provided, treat only characters in "
              "`bytes` as whitespace.") {
    BenderByteView str, set;
    trim_help_args(argc, argv, &str, &set);
    int32_t right_edge = trim_help_rightedge(str, set);
    return bender_stringv(str.bytes, right_edge);
}

/* Module entry point */
void bender_lib_string(BenderTable *env) {
    BenderRegExt string_cfuns[] = {
        BENDER_CORE_REG("string/slice", cfun_string_slice),
        BENDER_CORE_REG("keyword/slice", cfun_keyword_slice),
        BENDER_CORE_REG("symbol/slice", cfun_symbol_slice),
        BENDER_CORE_REG("string/repeat", cfun_string_repeat),
        BENDER_CORE_REG("string/bytes", cfun_string_bytes),
        BENDER_CORE_REG("string/from-bytes", cfun_string_frombytes),
        BENDER_CORE_REG("string/ascii-lower", cfun_string_asciilower),
        BENDER_CORE_REG("string/ascii-upper", cfun_string_asciiupper),
        BENDER_CORE_REG("string/reverse", cfun_string_reverse),
        BENDER_CORE_REG("string/find", cfun_string_find),
        BENDER_CORE_REG("string/find-all", cfun_string_findall),
        BENDER_CORE_REG("string/has-prefix?", cfun_string_hasprefix),
        BENDER_CORE_REG("string/has-suffix?", cfun_string_hassuffix),
        BENDER_CORE_REG("string/replace", cfun_string_replace),
        BENDER_CORE_REG("string/replace-all", cfun_string_replaceall),
        BENDER_CORE_REG("string/split", cfun_string_split),
        BENDER_CORE_REG("string/check-set", cfun_string_checkset),
        BENDER_CORE_REG("string/join", cfun_string_join),
        BENDER_CORE_REG("string/format", cfun_string_format),
        BENDER_CORE_REG("string/trim", cfun_string_trim),
        BENDER_CORE_REG("string/triml", cfun_string_triml),
        BENDER_CORE_REG("string/trimr", cfun_string_trimr),
        BENDER_REG_END
    };
    bender_core_cfuns_ext(env, NULL, string_cfuns);
}
