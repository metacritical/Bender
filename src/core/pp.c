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
#include "util.h"
#include "state.h"
#include <math.h>
#endif

#include <string.h>
#include <ctype.h>
#include <inttypes.h>
#include <float.h>

/* Implements a pretty printer for Bender. The pretty printer
 * is simple and not that flexible, but fast. */

/* Temporary buffer size */
#define BUFSIZE 64

/* Preprocessor hacks */
#define STR_HELPER(x) #x
#define STR(x) STR_HELPER(x)

static void number_to_string_b(BenderBuffer *buffer, double x) {
    bender_buffer_ensure(buffer, buffer->count + BUFSIZE, 2);
    const char *fmt = (x == floor(x) &&
                       x <= BENDER_INTMAX_DOUBLE &&
                       x >= BENDER_INTMIN_DOUBLE) ? "%.0f" : ("%." STR(DBL_DIG) "g");
    int count;
    if (x == 0.0) {
        /* Prevent printing of '-0' */
        count = 1;
        buffer->data[buffer->count] = '0';
    } else {
        count = snprintf((char *) buffer->data + buffer->count, BUFSIZE, fmt, x);
    }
    buffer->count += count;
}

/* expects non positive x */
static int count_dig10(int32_t x) {
    int result = 1;
    for (;;) {
        if (x > -10) return result;
        if (x > -100) return result + 1;
        if (x > -1000) return result + 2;
        if (x > -10000) return result + 3;
        x /= 10000;
        result += 4;
    }
}

static int32_t integer_to_string_b(BenderBuffer *buffer, int32_t x) {
    bender_buffer_extra(buffer, BUFSIZE);
    uint8_t *buf = buffer->data + buffer->count;
    int32_t neg = 0;
    int32_t len = 0;
    if (x == 0) {
        buf[0] = '0';
        buffer->count++;
        return 1;
    }
    if (x > 0) {
        x = -x;
    } else {
        neg = 1;
        *buf++ = '-';
    }
    len = count_dig10(x);
    buf += len;
    while (x) {
        uint8_t digit = (uint8_t) - (x % 10);
        *(--buf) = '0' + digit;
        x /= 10;
    }
    buffer->count += len + neg;
    return len + neg;
}

#define HEX(i) (((uint8_t *) bender_base64)[(i)])

/* Returns a string description for a pointer. Truncates
 * title to 32 characters */
static void string_description_b(BenderBuffer *buffer, const char *title, void *pointer) {
    bender_buffer_ensure(buffer, buffer->count + BUFSIZE, 2);
    uint8_t *c = buffer->data + buffer->count;
    int32_t i;
    union {
        uint8_t bytes[sizeof(void *)];
        void *p;
    } pbuf;

    pbuf.p = pointer;
    *c++ = '<';
    /* Maximum of 32 bytes for abstract type name */
    for (i = 0; i < 32 && title[i]; ++i)
        *c++ = ((uint8_t *)title) [i];
    *c++ = ' ';
    *c++ = '0';
    *c++ = 'x';
#if defined(BENDER_NANBOX_64)
#define POINTSIZE 6
#else
#define POINTSIZE (sizeof(void *))
#endif
    for (i = POINTSIZE; i > 0; --i) {
        uint8_t byte = pbuf.bytes[i - 1];
        *c++ = HEX(byte >> 4);
        *c++ = HEX(byte & 0xF);
    }
    *c++ = '>';
    buffer->count = (int32_t)(c - buffer->data);
#undef POINTSIZE
}

static int bender_escape_string_impl(BenderBuffer *buffer, const uint8_t *str, int32_t len) {
    bender_buffer_push_u8(buffer, '"');
    int align = 1;
    for (int32_t i = 0; i < len; ++i) {
        uint8_t c = str[i];
        switch (c) {
            case '"':
                bender_buffer_push_bytes(buffer, (const uint8_t *)"\\\"", 2);
                align += 2;
                break;
            case '\n':
                bender_buffer_push_bytes(buffer, (const uint8_t *)"\\n", 2);
                align += 2;
                break;
            case '\r':
                bender_buffer_push_bytes(buffer, (const uint8_t *)"\\r", 2);
                align += 2;
                break;
            case '\0':
                bender_buffer_push_bytes(buffer, (const uint8_t *)"\\0", 2);
                align += 2;
                break;
            case '\f':
                bender_buffer_push_bytes(buffer, (const uint8_t *)"\\f", 2);
                align += 2;
                break;
            case '\v':
                bender_buffer_push_bytes(buffer, (const uint8_t *)"\\v", 2);
                align += 2;
                break;
            case '\a':
                bender_buffer_push_bytes(buffer, (const uint8_t *)"\\a", 2);
                align += 2;
                break;
            case '\b':
                bender_buffer_push_bytes(buffer, (const uint8_t *)"\\b", 2);
                align += 2;
                break;
            case 27:
                bender_buffer_push_bytes(buffer, (const uint8_t *)"\\e", 2);
                align += 2;
                break;
            case '\\':
                bender_buffer_push_bytes(buffer, (const uint8_t *)"\\\\", 2);
                align += 2;
                break;
            case '\t':
                bender_buffer_push_bytes(buffer, (const uint8_t *)"\\t", 2);
                align += 2;
                break;
            default:
                if (c < 32 || c > 126) {
                    uint8_t buf[4];
                    buf[0] = '\\';
                    buf[1] = 'x';
                    buf[2] = bender_base64[(c >> 4) & 0xF];
                    buf[3] = bender_base64[c & 0xF];
                    bender_buffer_push_bytes(buffer, buf, 4);
                    align += 4;
                } else {
                    bender_buffer_push_u8(buffer, c);
                    align++;
                }
                break;
        }
    }
    bender_buffer_push_u8(buffer, '"');
    return align + 1;
}

static void bender_escape_string_b(BenderBuffer *buffer, const uint8_t *str) {
    bender_escape_string_impl(buffer, str, bender_string_length(str));
}

static void bender_escape_buffer_b(BenderBuffer *buffer, BenderBuffer *bx) {
    if (bx == buffer) {
        /* Ensures buffer won't resize while escaping */
        bender_buffer_ensure(bx, bx->count + 5 * bx->count + 3, 1);
    }
    bender_buffer_push_u8(buffer, '@');
    bender_escape_string_impl(buffer, bx->data, bx->count);
}

void bender_to_string_b(BenderBuffer *buffer, Bender x) {
    switch (bender_type(x)) {
        case BENDER_NIL:
            bender_buffer_push_cstring(buffer, "");
            break;
        case BENDER_BOOLEAN:
            bender_buffer_push_cstring(buffer,
                                      bender_unwrap_boolean(x) ? "true" : "false");
            break;
        case BENDER_NUMBER:
            number_to_string_b(buffer, bender_unwrap_number(x));
            break;
        case BENDER_STRING:
        case BENDER_SYMBOL:
        case BENDER_KEYWORD:
            bender_buffer_push_bytes(buffer,
                                    bender_unwrap_string(x),
                                    bender_string_length(bender_unwrap_string(x)));
            break;
        case BENDER_BUFFER: {
            BenderBuffer *to = bender_unwrap_buffer(x);
            /* Prevent resizing buffer while appending */
            if (buffer == to) bender_buffer_extra(buffer, to->count);
            bender_buffer_push_bytes(buffer, to->data, to->count);
            break;
        }
        case BENDER_ABSTRACT: {
            BenderAbstract p = bender_unwrap_abstract(x);
            const BenderAbstractType *t = bender_abstract_type(p);
            if (t->tostring != NULL) {
                t->tostring(p, buffer);
            } else {
                string_description_b(buffer, t->name, p);
            }
        }
        return;
        case BENDER_CFUNCTION: {
            BenderCFunRegistry *reg = bender_registry_get(bender_unwrap_cfunction(x));
            if (NULL != reg) {
                bender_buffer_push_cstring(buffer, "<cfunction ");
                if (NULL != reg->name_prefix) {
                    bender_buffer_push_cstring(buffer, reg->name_prefix);
                    bender_buffer_push_u8(buffer, '/');
                }
                bender_buffer_push_cstring(buffer, reg->name);
                bender_buffer_push_u8(buffer, '>');
                break;
            }
            goto fallthrough;
        }
        case BENDER_FUNCTION: {
            BenderFunction *fun = bender_unwrap_function(x);
            BenderFuncDef *def = fun->def;
            if (def == NULL) {
                bender_buffer_push_cstring(buffer, "<incomplete function>");
                break;
            }
            if (def->name) {
                const uint8_t *n = def->name;
                bender_buffer_push_cstring(buffer, "<function ");
                bender_buffer_push_bytes(buffer, n, bender_string_length(n));
                bender_buffer_push_u8(buffer, '>');
                break;
            }
            goto fallthrough;
        }
    fallthrough:
        default:
            string_description_b(buffer, bender_type_names[bender_type(x)], bender_unwrap_pointer(x));
            break;
    }
}

/* See parse.c for full table */

/* Check if a symbol or keyword contains no symbol characters */
static int contains_bad_chars(const uint8_t *sym, int issym) {
    int32_t len = bender_string_length(sym);
    if (len && issym && sym[0] >= '0' && sym[0] <= '9') return 1;
    if (!bender_valid_utf8(sym, len)) return 1;
    for (int32_t i = 0; i < len; i++) {
        if (!bender_is_symbol_char(sym[i])) return 1;
    }
    return 0;
}

void bender_description_b(BenderBuffer *buffer, Bender x) {
    switch (bender_type(x)) {
        default:
            break;
        case BENDER_NIL:
            bender_buffer_push_cstring(buffer, "nil");
            return;
        case BENDER_KEYWORD:
            bender_buffer_push_u8(buffer, ':');
            break;
        case BENDER_STRING:
            bender_escape_string_b(buffer, bender_unwrap_string(x));
            return;
        case BENDER_BUFFER: {
            BenderBuffer *b = bender_unwrap_buffer(x);
            bender_escape_buffer_b(buffer, b);
            return;
        }
        case BENDER_ABSTRACT: {
            BenderAbstract p = bender_unwrap_abstract(x);
            const BenderAbstractType *t = bender_abstract_type(p);
            if (t->tostring != NULL) {
                bender_buffer_push_cstring(buffer, "<");
                bender_buffer_push_cstring(buffer, t->name);
                bender_buffer_push_cstring(buffer, " ");
                t->tostring(p, buffer);
                bender_buffer_push_cstring(buffer, ">");
            } else {
                string_description_b(buffer, t->name, p);
            }
            return;
        }
    }
    bender_to_string_b(buffer, x);
}

const uint8_t *bender_description(Bender x) {
    BenderBuffer b;
    bender_buffer_init(&b, 10);
    bender_description_b(&b, x);
    const uint8_t *ret = bender_string(b.data, b.count);
    bender_buffer_deinit(&b);
    return ret;
}

/* Convert any value to a bender string. Similar to description, but
 * strings, symbols, and buffers will return their content. */
const uint8_t *bender_to_string(Bender x) {
    switch (bender_type(x)) {
        default: {
            BenderBuffer b;
            bender_buffer_init(&b, 10);
            bender_to_string_b(&b, x);
            const uint8_t *ret = bender_string(b.data, b.count);
            bender_buffer_deinit(&b);
            return ret;
        }
        case BENDER_BUFFER:
            return bender_string(bender_unwrap_buffer(x)->data, bender_unwrap_buffer(x)->count);
        case BENDER_STRING:
        case BENDER_SYMBOL:
        case BENDER_KEYWORD:
            return bender_unwrap_string(x);
    }
}

/* Hold state for pretty printer. */
struct pretty {
    BenderBuffer *buffer;
    int depth;
    int width;
    int align;
    int leaf_align;
    int flags;
    int32_t bufstartlen;
    int32_t lookback_barrier;
    int32_t *keysort_buffer;
    int32_t keysort_capacity;
    int32_t keysort_start;
    BenderTable seen;
};

/* Print jdn format */
static int print_jdn_one(struct pretty *S, Bender x, int depth) {
    if (depth == 0) return 1;
    switch (bender_type(x)) {
        case BENDER_NIL:
        case BENDER_BOOLEAN:
        case BENDER_BUFFER:
        case BENDER_STRING:
            bender_description_b(S->buffer, x);
            break;
        case BENDER_NUMBER:
            bender_buffer_ensure(S->buffer, S->buffer->count + BUFSIZE, 2);
            double num = bender_unwrap_number(x);
            if (isnan(num)) return 1;
            if (isinf(num)) return 1;
            bender_buffer_dtostr(S->buffer, num);
            break;
        case BENDER_SYMBOL:
        case BENDER_KEYWORD:
            if (contains_bad_chars(bender_unwrap_keyword(x), bender_type(x) == BENDER_SYMBOL)) return 1;
            bender_description_b(S->buffer, x);
            break;
        case BENDER_TUPLE: {
            BenderTuple t = bender_unwrap_tuple(x);
            int isb = bender_tuple_flag(t) & BENDER_TUPLE_FLAG_BRACKETCTOR;
            bender_buffer_push_u8(S->buffer, isb ? '[' : '(');
            for (int32_t i = 0; i < bender_tuple_length(t); i++) {
                if (i) bender_buffer_push_u8(S->buffer, ' ');
                if (print_jdn_one(S, t[i], depth - 1)) return 1;
            }
            bender_buffer_push_u8(S->buffer, isb ? ']' : ')');
        }
        break;
        case BENDER_ARRAY: {
            bender_table_put(&S->seen, x, bender_wrap_true());
            BenderArray *a = bender_unwrap_array(x);
            bender_buffer_push_cstring(S->buffer, "@[");
            for (int32_t i = 0; i < a->count; i++) {
                if (i) bender_buffer_push_u8(S->buffer, ' ');
                if (print_jdn_one(S, a->data[i], depth - 1)) return 1;
            }
            bender_buffer_push_u8(S->buffer, ']');
        }
        break;
        case BENDER_TABLE: {
            bender_table_put(&S->seen, x, bender_wrap_true());
            BenderTable *tab = bender_unwrap_table(x);
            bender_buffer_push_cstring(S->buffer, "@{");
            int isFirst = 1;
            for (int32_t i = 0; i < tab->capacity; i++) {
                const BenderKV *kv = tab->data + i;
                if (bender_checktype(kv->key, BENDER_NIL)) continue;
                if (!isFirst) bender_buffer_push_u8(S->buffer, ' ');
                isFirst = 0;
                if (print_jdn_one(S, kv->key, depth - 1)) return 1;
                bender_buffer_push_u8(S->buffer, ' ');
                if (print_jdn_one(S, kv->value, depth - 1)) return 1;
            }
            bender_buffer_push_u8(S->buffer, '}');
        }
        break;
        case BENDER_STRUCT: {
            BenderStruct st = bender_unwrap_struct(x);
            bender_buffer_push_u8(S->buffer, '{');
            int isFirst = 1;
            for (int32_t i = 0; i < bender_struct_capacity(st); i++) {
                const BenderKV *kv = st + i;
                if (bender_checktype(kv->key, BENDER_NIL)) continue;
                if (!isFirst) bender_buffer_push_u8(S->buffer, ' ');
                isFirst = 0;
                if (print_jdn_one(S, kv->key, depth - 1)) return 1;
                bender_buffer_push_u8(S->buffer, ' ');
                if (print_jdn_one(S, kv->value, depth - 1)) return 1;
            }
            bender_buffer_push_u8(S->buffer, '}');
        }
        break;
        default:
            return 1;
    }
    return 0;
}

static void backtrack_newlines(const struct pretty *S) {
    if (S->flags & BENDER_PRETTY_ONELINE || S->buffer->count <= 0)
        return;
    switch (S->buffer->data[S->buffer->count - 1]) {
        case ')':
        case '}':
        case ']':
            break;
        default:
            return;
    }
    int32_t removed = 0;
    int32_t old_count = S->buffer->count;
    int32_t offset = old_count;
    int32_t b0 = S->lookback_barrier;
    int32_t columns = S->width;
    int32_t align = 0;
    while (--offset >= b0) {
        const char *s = (const char *)S->buffer->data + offset;
        if (*s == '\n') {
            if (align < S->leaf_align) {
                break;
            }
            columns += align;
            removed += align;
            align = 0;
        } else if (*s == ' ') {
            align++;
        } else {
            align = 0;
            /* Don't count color sequences: \x1B(0|3\d)m */
            if (S->flags & BENDER_PRETTY_COLOR && *s == 'm') {
                if (offset >= (3 + b0) && strncmp("\x1B[0m", s - 3, 4) == 0) {
                    offset -= 3;
                    columns++;
                } else if (offset >= (4 + b0) && strncmp("\x1B[3", s - 4, 3) == 0) {
                    offset -= 4;
                    columns++;
                }
            }
        }
        if (--columns <= 0) {
            return;
        }
    }
    offset++; /* Don't mess with the last newline we found */
    bender_assert(offset >= b0, "bad buffer index");
    S->buffer->count -= removed;
    for (int32_t i = offset; i < S->buffer->count; i++) {
        if (S->buffer->data[offset] == '\n') {
            S->buffer->data[i] = ' ';
            while (S->buffer->data[++offset] == ' ') {
                bender_assert(offset < old_count, "bad replacement of newline");
            }
        } else {
            S->buffer->data[i] = S->buffer->data[offset++];
        }
    }
}

static void print_newline(struct pretty *S, int align) {
    int i;
    if (S->flags & BENDER_PRETTY_ONELINE) {
        bender_buffer_push_u8(S->buffer, ' ');
        return;
    }
    backtrack_newlines(S);
    bender_buffer_push_u8(S->buffer, '\n');
    S->leaf_align = S->align = align;
    for (i = 0; i < S->align; i++) {
        bender_buffer_push_u8(S->buffer, ' ');
    }
}

/* Color coding for types */
static const char bender_cycle_color[] = "\x1B[36m";
static const char bender_class_color[] = "\x1B[34m";
static const char *bender_pretty_colors[] = {
    "\x1B[32m",
    "\x1B[36m",
    "\x1B[36m",
    "\x1B[36m",
    "\x1B[35m",
    "\x1B[34m",
    "\x1B[33m",
    "\x1B[36m",
    "\x1B[36m",
    "\x1B[36m",
    "\x1B[36m",
    "\x1B[35m",
    "\x1B[36m",
    "\x1B[36m",
    "\x1B[36m",
    "\x1B[36m"
};

#define BENDER_PRETTY_DICT_LIMIT 30
#define BENDER_PRETTY_DICT_KEYSORT_LIMIT 2000
#define BENDER_PRETTY_ARRAY_LIMIT 160

/* Helper for pretty printing */
static void bender_pretty_one(struct pretty *S, Bender x) {
    /* Add to seen */
    switch (bender_type(x)) {
        case BENDER_NIL:
        case BENDER_NUMBER:
        case BENDER_SYMBOL:
        case BENDER_BOOLEAN:
            break;
        default: {
            Bender seenid = bender_table_get(&S->seen, x);
            if (bender_checktype(seenid, BENDER_NUMBER)) {
                if (S->flags & BENDER_PRETTY_COLOR) {
                    bender_buffer_push_cstring(S->buffer, bender_cycle_color);
                }
                bender_buffer_push_cstring(S->buffer, "<cycle ");
                S->align += 8 + integer_to_string_b(S->buffer, bender_unwrap_integer(seenid));
                bender_buffer_push_u8(S->buffer, '>');
                if (S->flags & BENDER_PRETTY_COLOR) {
                    bender_buffer_push_cstring(S->buffer, "\x1B[0m");
                }
                return;
            } else {
                bender_table_put(&S->seen, x, bender_wrap_integer(S->seen.count));
                break;
            }
        }
    }

    switch (bender_type(x)) {
        default: {
            const char *color = bender_pretty_colors[bender_type(x)];
            if (color && (S->flags & BENDER_PRETTY_COLOR)) {
                bender_buffer_push_cstring(S->buffer, color);
            }
            if (bender_checktype(x, BENDER_BUFFER) && bender_unwrap_buffer(x) == S->buffer) {
                bender_buffer_ensure(S->buffer, S->buffer->count + S->bufstartlen * 4 + 3, 1);
                bender_buffer_push_u8(S->buffer, '@');
                /* Use start len to print to self better */
                S->align += 1 + bender_escape_string_impl(S->buffer, S->buffer->data, S->bufstartlen);
            } else {
                S->align -= S->buffer->count;
                bender_description_b(S->buffer, x);
                S->align += S->buffer->count;
            }
            if (color && (S->flags & BENDER_PRETTY_COLOR)) {
                bender_buffer_push_cstring(S->buffer, "\x1B[0m");
            }
            break;
        }
        case BENDER_ARRAY:
        case BENDER_TUPLE: {
            int32_t i = 0, len = 0;
            const Bender *arr = NULL;
            int isarray = bender_checktype(x, BENDER_ARRAY);
            bender_indexed_view(x, &arr, &len);
            int hasbrackets = !isarray && (bender_tuple_flag(arr) & BENDER_TUPLE_FLAG_BRACKETCTOR);
            const char *startstr = isarray ? "@[" : hasbrackets ? "[" : "(";
            const char endchar = isarray ? ']' : hasbrackets ? ']' : ')';
            bender_buffer_push_cstring(S->buffer, startstr);
            S->align += (int32_t) strlen(startstr);
            const int align = S->leaf_align = S->align;
            S->depth--;
            if (S->depth == 0) {
                bender_buffer_push_cstring(S->buffer, "...");
                S->align += 3;
            } else {
                if (len > BENDER_PRETTY_ARRAY_LIMIT && !(S->flags & BENDER_PRETTY_NOTRUNC)) {
                    for (i = 0; i < 3; i++) {
                        if (i) print_newline(S, align);
                        bender_pretty_one(S, arr[i]);
                    }
                    print_newline(S, align);
                    bender_buffer_push_cstring(S->buffer, "...");
                    S->align += 3;
                    for (i = len - 3; i < len; i++) {
                        print_newline(S, align);
                        bender_pretty_one(S, arr[i]);
                    }
                } else {
                    for (i = 0; i < len; i++) {
                        if (i) print_newline(S, align);
                        bender_pretty_one(S, arr[i]);
                    }
                }
            }
            S->depth++;
            bender_buffer_push_u8(S->buffer, endchar);
            S->align++;
            break;
        }
        case BENDER_STRUCT:
        case BENDER_TABLE: {
            int istable = bender_checktype(x, BENDER_TABLE);

            /* For object-like tables, print class name */
            if (istable) {
                BenderTable *t = bender_unwrap_table(x);
                BenderTable *proto = t->proto;
                S->align++;
                bender_buffer_push_cstring(S->buffer, "@");
                if (NULL != proto) {
                    Bender name = bender_table_get(proto, bender_ckeywordv("_name"));
                    const uint8_t *n;
                    int32_t len;
                    if (bender_bytes_view(name, &n, &len)) {
                        if (S->flags & BENDER_PRETTY_COLOR) {
                            bender_buffer_push_cstring(S->buffer, bender_class_color);
                        }
                        bender_buffer_push_bytes(S->buffer, n, len);
                        S->align += len;
                        if (S->flags & BENDER_PRETTY_COLOR) {
                            bender_buffer_push_cstring(S->buffer, "\x1B[0m");
                        }
                    }
                }
            } else {
                BenderStruct st = bender_unwrap_struct(x);
                BenderStruct proto = bender_struct_proto(st);
                if (NULL != proto) {
                    Bender name = bender_struct_get(proto, bender_ckeywordv("_name"));
                    const uint8_t *n;
                    int32_t len;
                    if (bender_bytes_view(name, &n, &len)) {
                        if (S->flags & BENDER_PRETTY_COLOR) {
                            bender_buffer_push_cstring(S->buffer, bender_class_color);
                        }
                        bender_buffer_push_bytes(S->buffer, n, len);
                        S->align += len;
                        if (S->flags & BENDER_PRETTY_COLOR) {
                            bender_buffer_push_cstring(S->buffer, "\x1B[0m");
                        }
                    }
                }
            }
            bender_buffer_push_u8(S->buffer, '{');
            const int align = S->leaf_align = ++S->align;

            S->depth--;
            if (S->depth == 0) {
                bender_buffer_push_cstring(S->buffer, "...");
                S->align += 3;
            } else {
                int32_t len = 0, cap = 0;
                const BenderKV *kvs = NULL;
                bender_dictionary_view(x, &kvs, &len, &cap);
                int32_t ks_start = S->keysort_start;
                int truncated = 0;

                /* Shortcut for huge dictionaries, don't bother sorting keys */
                if (len > BENDER_PRETTY_DICT_KEYSORT_LIMIT) {
                    if (!(S->flags & BENDER_PRETTY_NOTRUNC)) {
                        len = BENDER_PRETTY_DICT_LIMIT;
                        truncated = 1;
                    }
                    int32_t j = 0;
                    for (int32_t i = 0; i < len; i++) {
                        while (bender_checktype(kvs[j].key, BENDER_NIL)) j++;
                        if (i) print_newline(S, align);
                        bender_pretty_one(S, kvs[j].key);
                        bender_buffer_push_u8(S->buffer, ' ');
                        S->align++;
                        bender_pretty_one(S, kvs[j].value);
                        j++;
                    }
                    if (truncated) {
                        print_newline(S, align);
                        bender_buffer_push_cstring(S->buffer, "...");
                        S->align += 3;
                    }
                } else {
                    /* Sorted keys dictionaries */

                    /* Ensure buffer is large enough to sort keys. */
                    int64_t mincap = (int64_t) len + (int64_t) ks_start;
                    if (mincap > INT32_MAX) {
                        truncated = 1;
                        len = 0;
                        mincap = ks_start;
                    }

                    if (S->keysort_capacity < mincap) {
                        if (mincap >= INT32_MAX / 2) {
                            S->keysort_capacity = INT32_MAX;
                        } else {
                            S->keysort_capacity = (int32_t)(mincap * 2);
                        }
                        S->keysort_buffer = bender_srealloc(S->keysort_buffer, sizeof(int32_t) * S->keysort_capacity);
                        if (NULL == S->keysort_buffer) {
                            BENDER_OUT_OF_MEMORY;
                        }
                    }

                    bender_sorted_keys(kvs, cap, S->keysort_buffer == NULL ? NULL : S->keysort_buffer + ks_start);
                    S->keysort_start += len;
                    if (!(S->flags & BENDER_PRETTY_NOTRUNC) && (len > BENDER_PRETTY_DICT_LIMIT)) {
                        len = BENDER_PRETTY_DICT_LIMIT;
                        truncated = 1;
                    }

                    for (int32_t i = 0; i < len; i++) {
                        if (i) print_newline(S, align);
                        int32_t j = S->keysort_buffer[i + ks_start];
                        bender_pretty_one(S, kvs[j].key);
                        bender_buffer_push_u8(S->buffer, ' ');
                        S->align++;
                        bender_pretty_one(S, kvs[j].value);
                    }

                    if (truncated) {
                        print_newline(S, align);
                        bender_buffer_push_cstring(S->buffer, "...");
                        S->align += 3;
                    }

                }
                S->keysort_start = ks_start;
            }
            S->depth++;
            bender_buffer_push_u8(S->buffer, '}');
            S->align++;
            break;
        }
    }
    /* Remove from seen */
    bender_table_remove(&S->seen, x);
    return;
}

#define BENDER_COLUMNS 80

static BenderBuffer *bender_pretty_(BenderBuffer *buffer, int depth, int width,
                                  int flags, Bender x, int32_t startlen, int32_t lookback_barrier) {
    struct pretty S;
    if (NULL == buffer) {
        buffer = bender_buffer(0);
    }
    S.buffer = buffer;
    S.depth = depth;
    S.width = width;
    S.align = 0;
    S.flags = flags;
    S.bufstartlen = startlen;
    S.lookback_barrier = lookback_barrier;
    S.keysort_capacity = 0;
    S.keysort_buffer = NULL;
    S.keysort_start = 0;
    bender_table_init(&S.seen, 10);
    bender_pretty_one(&S, x);
    backtrack_newlines(&S);
    bender_table_deinit(&S.seen);
    return S.buffer;
}

/* Helper for printing a bender value in a pretty form. Not meant to be used
 * for serialization or anything like that. */
BenderBuffer *bender_pretty(BenderBuffer *buffer, int depth, int flags, Bender x) {
    return bender_pretty_(buffer, depth, BENDER_COLUMNS, flags,
                         x, buffer ? buffer->count : 0, buffer ? buffer->count : 0);
}

static BenderBuffer *bender_jdn_(BenderBuffer *buffer, int depth, Bender x, int32_t startlen, int32_t lookback_barrier) {
    struct pretty S;
    if (NULL == buffer) {
        buffer = bender_buffer(0);
    }
    S.buffer = buffer;
    S.depth = depth;
    S.align = 0;
    S.flags = 0;
    S.bufstartlen = startlen;
    S.lookback_barrier = lookback_barrier;
    S.keysort_capacity = 0;
    S.keysort_buffer = NULL;
    S.keysort_start = 0;
    bender_table_init(&S.seen, 10);
    int res = print_jdn_one(&S, x, depth);
    bender_table_deinit(&S.seen);
    if (res) {
        bender_panic("could not print to jdn format");
    }
    return S.buffer;
}

BenderBuffer *bender_jdn(BenderBuffer *buffer, int depth, Bender x) {
    return bender_jdn_(buffer, depth, x, buffer ? buffer->count : 0, buffer ? buffer->count : 0);
}

static const char *typestr(Bender x) {
    BenderType t = bender_type(x);
    return (t == BENDER_ABSTRACT)
           ? bender_abstract_type(bender_unwrap_abstract(x))->name
           : bender_type_names[t];
}

static void pushtypes(BenderBuffer *buffer, int types) {
    int first = 1;
    int i = 0;
    while (types) {
        if (1 & types) {
            if (first) {
                first = 0;
            } else {
                bender_buffer_push_cstring(buffer, (types == 1) ? " or " : ", ");
            }
            bender_buffer_push_cstring(buffer, bender_type_names[i]);
        }
        i++;
        types >>= 1;
    }
}

/*
 * code adapted from lua/lstrlib.c http://lua.org
 */

#define MAX_ITEM  256
#define FMT_FLAGS "-+ #0"
#define FMT_REPLACE_INTTYPES "diouxX"
#define MAX_FORMAT 32

struct FmtMapping {
    char c;
    const char *mapping;
};

/* Bender uses fixed width integer types for most things, so map
 * format specifiers to these fixed sizes */
static const struct FmtMapping format_mappings[] = {
    {'D', PRId64},
    {'I', PRIi64},
    {'d', PRId64},
    {'i', PRIi64},
    {'o', PRIo64},
    {'u', PRIu64},
    {'x', PRIx64},
    {'X', PRIX64},
};

static const char *get_fmt_mapping(char c) {
    for (size_t i = 0; i < (sizeof(format_mappings) / sizeof(struct FmtMapping)); i++) {
        if (format_mappings[i].c == c)
            return format_mappings[i].mapping;
    }
    bender_assert(0, "bad format mapping");
}

static const char *scanformat(
    const char *strfrmt,
    char *form,
    char width[3],
    char precision[3]) {
    const char *p = strfrmt;

    /* Parse strfrmt */
    memset(width, '\0', 3);
    memset(precision, '\0', 3);
    while (*p != '\0' && strchr(FMT_FLAGS, *p) != NULL)
        p++; /* skip flags */
    if ((size_t)(p - strfrmt) >= sizeof(FMT_FLAGS)) bender_panic("invalid format (repeated flags)");
    if (isdigit((int)(*p)))
        width[0] = *p++; /* skip width */
    if (isdigit((int)(*p)))
        width[1] = *p++; /* (2 digits at most) */
    if (*p == '.') {
        p++;
        if (isdigit((int)(*p)))
            precision[0] = *p++; /* skip precision */
        if (isdigit((int)(*p)))
            precision[1] = *p++; /* (2 digits at most) */
    }
    if (isdigit((int)(*p)))
        bender_panic("invalid format (width or precision too long)");

    /* Write to form - replace characters with fixed size stuff */
    *(form++) = '%';
    const char *p2 = strfrmt;
    while (p2 <= p) {
        char *loc = strchr(FMT_REPLACE_INTTYPES, *p2);
        if (loc != NULL && *loc != '\0') {
            const char *mapping = get_fmt_mapping(*p2++);
            size_t len = strlen(mapping);
            memcpy(form, mapping, len);
            form += len;
        } else {
            *(form++) = *(p2++);
        }
    }
    *form = '\0';

    return p;
}

void bender_formatbv(BenderBuffer *b, const char *format, va_list args) {
    const char *format_end = format + strlen(format);
    const char *c = format;
    int32_t startlen = b->count;
    while (c < format_end) {
        if (*c != '%') {
            bender_buffer_push_u8(b, (uint8_t) *c++);
        } else if (*++c == '%') {
            bender_buffer_push_u8(b, (uint8_t) *c++);
        } else {
            char form[MAX_FORMAT], item[MAX_ITEM];
            char width[3], precision[3];
            int nb = 0; /* number of bytes in added item */
            c = scanformat(c, form, width, precision);
            switch (*c++) {
                case 'c': {
                    int n = va_arg(args, int);
                    nb = snprintf(item, MAX_ITEM, form, n);
                    break;
                }
                case 'd':
                case 'i': {
                    int64_t n = (int64_t) va_arg(args, int32_t);
                    nb = snprintf(item, MAX_ITEM, form, n);
                    break;
                }
                case 'D':
                case 'I': {
                    int64_t n = va_arg(args, int64_t);
                    nb = snprintf(item, MAX_ITEM, form, n);
                    break;
                }
                case 'x':
                case 'X':
                case 'o':
                case 'u': {
                    uint64_t n = va_arg(args, uint64_t);
                    nb = snprintf(item, MAX_ITEM, form, n);
                    break;
                }
                case 'a':
                case 'A':
                case 'e':
                case 'E':
                case 'f':
                case 'g':
                case 'G': {
                    double d = va_arg(args, double);
                    nb = snprintf(item, MAX_ITEM, form, d);
                    break;
                }
                case 's':
                case 'S': {
                    const char *str = va_arg(args, const char *);
                    int32_t len = (c[-1] == 's')
                                  ? (int32_t) strlen(str)
                                  : bender_string_length((BenderString) str);
                    if (form[2] == '\0')
                        bender_buffer_push_bytes(b, (const uint8_t *) str, len);
                    else {
                        if (len != (int32_t) strlen((const char *) str))
                            bender_panic("string contains zeros");
                        if (!strchr(form, '.') && len >= 100) {
                            bender_panic("no precision and string is too long to be formatted");
                        } else {
                            nb = snprintf(item, MAX_ITEM, form, str);
                        }
                    }
                    break;
                }
                case 'V':
                    bender_to_string_b(b, va_arg(args, Bender));
                    break;
                case 'v':
                    bender_description_b(b, va_arg(args, Bender));
                    break;
                case 't':
                    bender_buffer_push_cstring(b, typestr(va_arg(args, Bender)));
                    break;
                case 'T': {
                    int types = va_arg(args, int);
                    pushtypes(b, types);
                    break;
                }
                case 'M':
                case 'm':
                case 'N':
                case 'n':
                case 'Q':
                case 'q':
                case 'P':
                case 'p': { /* bender pretty , precision = depth */
                    int depth = atoi(precision);
                    if (depth < 1) depth = BENDER_RECURSION_GUARD;
                    char d = c[-1];
                    int has_color = (d == 'P') || (d == 'Q') || (d == 'M') || (d == 'N');
                    int has_oneline = (d == 'Q') || (d == 'q') || (d == 'N') || (d == 'n');
                    int has_notrunc = (d == 'M') || (d == 'm') || (d == 'N') || (d == 'n');
                    int columns = atoi(width);
                    if (columns == 0) {
                        columns = BENDER_COLUMNS;
                    } else if (columns < 0) {
                        has_oneline = 1;
                    }
                    int flags = 0;
                    flags |= has_color ? BENDER_PRETTY_COLOR : 0;
                    flags |= has_oneline ? BENDER_PRETTY_ONELINE : 0;
                    flags |= has_notrunc ? BENDER_PRETTY_NOTRUNC : 0;
                    bender_pretty_(b, depth, columns, flags,
                                  va_arg(args, Bender), startlen, b->count);
                    break;
                }
                case 'j': {
                    int depth = atoi(precision);
                    if (depth < 1) {
                        depth = BENDER_RECURSION_GUARD;
                    }
                    bender_jdn_(b, depth, va_arg(args, Bender), startlen, b->count);
                    break;
                }
                default: {
                    /* also treat cases 'nLlh' */
                    bender_panicf("invalid conversion '%s' to 'format'",
                                 form);
                }
            }
            if (nb >= MAX_ITEM)
                bender_panic("format buffer overflow");
            if (nb > 0)
                bender_buffer_push_bytes(b, (uint8_t *) item, nb);
        }

    }
}

/* Helper function for formatting strings. Useful for generating error messages and the like.
 * Similar to printf, but specialized for operating with bender. */
const uint8_t *bender_formatc(const char *format, ...) {
    va_list args;
    const uint8_t *ret;
    BenderBuffer buffer;
    int32_t len = 0;

    /* Calculate length, init buffer and args */
    while (format[len]) len++;
    bender_buffer_init(&buffer, len);
    va_start(args, format);

    /* Run format */
    bender_formatbv(&buffer, format, args);

    /* Iterate length */
    va_end(args);

    ret = bender_string(buffer.data, buffer.count);
    bender_buffer_deinit(&buffer);
    return ret;
}

BenderBuffer *bender_formatb(BenderBuffer *buffer, const char *format, ...) {
    va_list args;
    va_start(args, format);
    bender_formatbv(buffer, format, args);
    va_end(args);
    return buffer;
}

/* Shared implementation between string/format and
 * buffer/format */
void bender_buffer_format(
    BenderBuffer *b,
    const char *strfrmt,
    int32_t argstart,
    int32_t argc,
    Bender *argv) {
    size_t sfl = strlen(strfrmt);
    const char *strfrmt_end = strfrmt + sfl;
    int32_t arg = argstart;
    int32_t startlen = b->count;
    while (strfrmt < strfrmt_end) {
        if (*strfrmt != '%')
            bender_buffer_push_u8(b, (uint8_t) * strfrmt++);
        else if (*++strfrmt == '%')
            bender_buffer_push_u8(b, (uint8_t) * strfrmt++); /* %% */
        else { /* format item */
            char form[MAX_FORMAT], item[MAX_ITEM];
            char width[3], precision[3];
            int nb = 0; /* number of bytes in added item */
#ifdef BENDER_PLAN9
            if (*strfrmt == 'r') {
                rerrstr(item, MAX_ITEM);
                nb = strlen(item);
            } else
#endif
                if (++arg >= argc)
                    bender_panic("not enough values for format");
            strfrmt = scanformat(strfrmt, form, width, precision);
            switch (*strfrmt++) {
#ifdef BENDER_PLAN9
                case 'r':
                    break;
#endif
                case 'c': {
                    nb = snprintf(item, MAX_ITEM, form, (int)
                                  bender_getinteger(argv, arg));
                    break;
                }
                case 'D':
                case 'I':
                case 'd':
                case 'i': {
                    int64_t n = bender_getinteger64(argv, arg);
                    nb = snprintf(item, MAX_ITEM, form, n);
                    break;
                }
                case 'x':
                case 'X':
                case 'o':
                case 'u': {
                    uint64_t n = bender_getuinteger64(argv, arg);
                    nb = snprintf(item, MAX_ITEM, form, n);
                    break;
                }
                case 'a':
                case 'A':
                case 'e':
                case 'E':
                case 'f':
                case 'g':
                case 'G': {
                    double d = bender_getnumber(argv, arg);
                    nb = snprintf(item, MAX_ITEM, form, d);
                    break;
                }
                case 's': {
                    const char *s = bender_getcbytes(argv, arg);
                    if (form[2] == '\0')
                        bender_buffer_push_cstring(b, s);
                    else {
                        nb = snprintf(item, MAX_ITEM, form, s);
                    }
                    break;
                }
                case 'V': {
                    bender_to_string_b(b, argv[arg]);
                    break;
                }
                case 'v': {
                    bender_description_b(b, argv[arg]);
                    break;
                }
                case 't':
                    bender_buffer_push_cstring(b, typestr(argv[arg]));
                    break;
                case 'M':
                case 'm':
                case 'N':
                case 'n':
                case 'Q':
                case 'q':
                case 'P':
                case 'p': { /* bender pretty , precision = depth */
                    int depth = atoi(precision);
                    if (depth < 1) depth = BENDER_RECURSION_GUARD;
                    char d = strfrmt[-1];
                    int has_color = (d == 'P') || (d == 'Q') || (d == 'M') || (d == 'N');
                    int has_oneline = (d == 'Q') || (d == 'q') || (d == 'N') || (d == 'n');
                    int has_notrunc = (d == 'M') || (d == 'm') || (d == 'N') || (d == 'n');
                    int columns = atoi(width);
                    if (columns == 0)
                        columns = BENDER_COLUMNS;
                    else if (columns < 0)
                        has_oneline = 1;
                    int flags = 0;
                    flags |= has_color ? BENDER_PRETTY_COLOR : 0;
                    flags |= has_oneline ? BENDER_PRETTY_ONELINE : 0;
                    flags |= has_notrunc ? BENDER_PRETTY_NOTRUNC : 0;
                    bender_pretty_(b, depth, columns, flags,
                                  argv[arg], startlen, b->count);
                    break;
                }
                case 'j': {
                    int depth = atoi(precision);
                    if (depth < 1)
                        depth = BENDER_RECURSION_GUARD;
                    bender_jdn_(b, depth, argv[arg], startlen, b->count);
                    break;
                }
                default: {
                    /* also treat cases 'nLlh' */
                    bender_panicf("invalid conversion '%s' to 'format'",
                                 form);
                }
            }
            if (nb >= MAX_ITEM)
                bender_panic("format buffer overflow");
            if (nb > 0)
                bender_buffer_push_bytes(b, (uint8_t *) item, nb);
        }
    }
}

#undef HEX
#undef BUFSIZE
