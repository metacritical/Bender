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
#include "state.h"
#include "fiber.h"
#include "util.h"
#endif

#ifndef BENDER_SINGLE_THREADED
#ifndef BENDER_WINDOWS
#include <pthread.h>
#endif
#endif

#ifdef BENDER_WINDOWS
#include <windows.h>
#endif

#ifdef BENDER_USE_STDATOMIC
#include <stdatomic.h>
/* We don't need stdatomic on most compilers since we use compiler builtins for atomic operations.
 * Some (TCC), explicitly require using stdatomic.h and don't have any exposed builtins (that I know of).
 * For TCC and similar compilers, one would need -std=c11 or similar then to get access. */
#endif

BENDER_NO_RETURN static void bender_top_level_signal(const char *msg) {
#ifdef BENDER_TOP_LEVEL_SIGNAL
    BENDER_TOP_LEVEL_SIGNAL(msg);
#else
    fputs(msg, stdout);
    if (!(bender_vm.sandbox_flags & BENDER_SANDBOX_EXIT)) {
        /* Exit is not forbidden */
        exit(EXIT_FAILURE);
    }
    /* If not able to signal, then select good default behavior - for single threaded programs, we have no
     * choice but to exit. */
# ifdef BENDER_SINGLE_THREADED
    exit(EXIT_FAILURE);
# elif defined(BENDER_WINDOWS)
    ExitThread(-1);
# else
    /* Other threads will continue as usual */
    pthread_exit(NULL);
# endif
#endif
}

void bender_signalv(BenderSignal sig, Bender message) {
    if (bender_vm.return_reg != NULL) {
        /* Should match logic in bender_call for coercing everything not ok to an error (no awaits, yields, etc.) */
        if (bender_vm.coerce_error && sig != BENDER_SIGNAL_OK) {
#ifdef BENDER_EV
            if (NULL != bender_vm.root_fiber && sig == BENDER_SIGNAL_EVENT) {
                bender_vm.root_fiber->sched_id++;
            }
#endif
            if (sig != BENDER_SIGNAL_ERROR) {
                message = bender_wrap_string(bender_formatc("%v coerced from %s to error", message, bender_signal_names[sig]));
            }
            sig = BENDER_SIGNAL_ERROR;
        }
        *bender_vm.return_reg = message;
        if (NULL != bender_vm.fiber) {
            bender_vm.fiber->flags |= BENDER_FIBER_DID_LONGJUMP;
        }
#if defined(BENDER_BSD) || defined(BENDER_APPLE)
        _longjmp(*bender_vm.signal_buf, sig);
#else
        longjmp(*bender_vm.signal_buf, sig);
#endif
    } else {
        const char *str = (const char *)bender_formatc("bender top level signal - %v\n", message);
        bender_top_level_signal(str);
    }
}

void bender_panicv(Bender message) {
    bender_signalv(BENDER_SIGNAL_ERROR, message);
}

void bender_panicf(const char *format, ...) {
    va_list args;
    const uint8_t *ret;
    BenderBuffer buffer;
    int32_t len = 0;
    while (format[len]) len++;
    bender_buffer_init(&buffer, len);
    va_start(args, format);
    bender_formatbv(&buffer, format, args);
    va_end(args);
    ret = bender_string(buffer.data, buffer.count);
    bender_buffer_deinit(&buffer);
    bender_panics(ret);
}

void bender_panic(const char *message) {
    bender_panicv(bender_cstringv(message));
}

void bender_panics(const uint8_t *message) {
    bender_panicv(bender_wrap_string(message));
}

void bender_panic_type(Bender x, int32_t n, int expected) {
    bender_panicf("bad slot #%d, expected %T, got %v", n, expected, x);
}

void bender_panic_abstract(Bender x, int32_t n, const BenderAbstractType *at) {
    bender_panicf("bad slot #%d, expected %s, got %v", n, at->name, x);
}

void bender_fixarity(int32_t arity, int32_t fix) {
    if (arity != fix)
        bender_panicf("arity mismatch, expected %d, got %d", fix, arity);
}

void bender_arity(int32_t arity, int32_t min, int32_t max) {
    if (min >= 0 && arity < min)
        bender_panicf("arity mismatch, expected at least %d, got %d", min, arity);
    if (max >= 0 && arity > max)
        bender_panicf("arity mismatch, expected at most %d, got %d", max, arity);
}

#define DEFINE_GETTER(name, NAME, type) \
type bender_get##name(const Bender *argv, int32_t n) { \
    Bender x = argv[n]; \
    if (!bender_checktype(x, BENDER_##NAME)) { \
        bender_panic_type(x, n, BENDER_TFLAG_##NAME); \
    } \
    return bender_unwrap_##name(x); \
}

#define DEFINE_OPT(name, NAME, type) \
type bender_opt##name(const Bender *argv, int32_t argc, int32_t n, type dflt) { \
    if (n >= argc) return dflt; \
    if (bender_checktype(argv[n], BENDER_NIL)) return dflt; \
    return bender_get##name(argv, n); \
}

#define DEFINE_OPTLEN(name, NAME, type) \
type bender_opt##name(const Bender *argv, int32_t argc, int32_t n, int32_t dflt_len) { \
    if (n >= argc || bender_checktype(argv[n], BENDER_NIL)) {\
        return bender_##name(dflt_len); \
    }\
    return bender_get##name(argv, n); \
}

int bender_getmethod(const uint8_t *method, const BenderMethod *methods, Bender *out) {
    while (methods->name) {
        if (!bender_cstrcmp(method, methods->name)) {
            *out = bender_wrap_cfunction(methods->cfun);
            return 1;
        }
        methods++;
    }
    return 0;
}

Bender bender_nextmethod(const BenderMethod *methods, Bender key) {
    if (!bender_checktype(key, BENDER_NIL)) {
        while (methods->name) {
            if (bender_keyeq(key, methods->name)) {
                methods++;
                break;
            }
            methods++;
        }
    }
    if (methods->name) {
        return bender_ckeywordv(methods->name);
    } else {
        return bender_wrap_nil();
    }
}

DEFINE_GETTER(number, NUMBER, double)
DEFINE_GETTER(array, ARRAY, BenderArray *)
DEFINE_GETTER(tuple, TUPLE, const Bender *)
DEFINE_GETTER(table, TABLE, BenderTable *)
DEFINE_GETTER(struct, STRUCT, const BenderKV *)
DEFINE_GETTER(string, STRING, const uint8_t *)
DEFINE_GETTER(keyword, KEYWORD, const uint8_t *)
DEFINE_GETTER(symbol, SYMBOL, const uint8_t *)
DEFINE_GETTER(buffer, BUFFER, BenderBuffer *)
DEFINE_GETTER(fiber, FIBER, BenderFiber *)
DEFINE_GETTER(function, FUNCTION, BenderFunction *)
DEFINE_GETTER(cfunction, CFUNCTION, BenderCFunction)
DEFINE_GETTER(boolean, BOOLEAN, int)
DEFINE_GETTER(pointer, POINTER, void *)

DEFINE_OPT(number, NUMBER, double)
DEFINE_OPT(tuple, TUPLE, const Bender *)
DEFINE_OPT(struct, STRUCT, const BenderKV *)
DEFINE_OPT(string, STRING, const uint8_t *)
DEFINE_OPT(keyword, KEYWORD, const uint8_t *)
DEFINE_OPT(symbol, SYMBOL, const uint8_t *)
DEFINE_OPT(fiber, FIBER, BenderFiber *)
DEFINE_OPT(function, FUNCTION, BenderFunction *)
DEFINE_OPT(cfunction, CFUNCTION, BenderCFunction)
DEFINE_OPT(boolean, BOOLEAN, int)
DEFINE_OPT(pointer, POINTER, void *)

DEFINE_OPTLEN(buffer, BUFFER, BenderBuffer *)
DEFINE_OPTLEN(table, TABLE, BenderTable *)
DEFINE_OPTLEN(array, ARRAY, BenderArray *)

const char *bender_optcstring(const Bender *argv, int32_t argc, int32_t n, const char *dflt) {
    if (n >= argc || bender_checktype(argv[n], BENDER_NIL)) {
        return dflt;
    }
    return bender_getcstring(argv, n);
}

#undef DEFINE_GETTER
#undef DEFINE_OPT
#undef DEFINE_OPTLEN

const char *bender_getcstring(const Bender *argv, int32_t n) {
    if (!bender_checktype(argv[n], BENDER_STRING)) {
        bender_panic_type(argv[n], n, BENDER_TFLAG_STRING);
    }
    return bender_getcbytes(argv, n);
}

const char *bender_getcbytes(const Bender *argv, int32_t n) {
    /* Ensure buffer 0-padded */
    if (bender_checktype(argv[n], BENDER_BUFFER)) {
        BenderBuffer *b = bender_unwrap_buffer(argv[n]);
        if ((b->gc.flags & BENDER_BUFFER_FLAG_NO_REALLOC) && b->count == b->capacity) {
            /* Make a copy with bender_smalloc in the rare case we have a buffer that
             * cannot be realloced and pushing a 0 byte would panic. */
            char *new_string = bender_smalloc(b->count + 1);
            memcpy(new_string, b->data, b->count);
            new_string[b->count] = 0;
            if (strlen(new_string) != (size_t) b->count) goto badzeros;
            return new_string;
        } else {
            /* Ensure trailing 0 */
            bender_buffer_push_u8(b, 0);
            b->count--;
            if (strlen((char *)b->data) != (size_t) b->count) goto badzeros;
            return (const char *) b->data;
        }
    }
    BenderByteView view = bender_getbytes(argv, n);
    const char *cstr = (const char *)view.bytes;
    if (strlen(cstr) != (size_t) view.len) goto badzeros;
    return cstr;

badzeros:
    bender_panic("bytes contain embedded 0s");
}

const char *bender_optcbytes(const Bender *argv, int32_t argc, int32_t n, const char *dflt) {
    if (n >= argc || bender_checktype(argv[n], BENDER_NIL)) {
        return dflt;
    }
    return bender_getcbytes(argv, n);
}

int32_t bender_getnat(const Bender *argv, int32_t n) {
    Bender x = argv[n];
    if (!bender_checkint(x)) goto bad;
    int32_t ret = bender_unwrap_integer(x);
    if (ret < 0) goto bad;
    return ret;
bad:
    bender_panicf("bad slot #%d, expected non-negative 32 bit signed integer, got %v", n, x);
}

BenderAbstract bender_checkabstract(Bender x, const BenderAbstractType *at) {
    if (!bender_checktype(x, BENDER_ABSTRACT)) return NULL;
    BenderAbstract a = bender_unwrap_abstract(x);
    if (bender_abstract_type(a) != at) return NULL;
    return a;
}

static int bender_strlike_cmp(BenderType type, Bender x, const char *cstring) {
    if (bender_type(x) != type) return 0;
    return !bender_cstrcmp(bender_unwrap_string(x), cstring);
}

int bender_keyeq(Bender x, const char *cstring) {
    return bender_strlike_cmp(BENDER_KEYWORD, x, cstring);
}

int bender_streq(Bender x, const char *cstring) {
    return bender_strlike_cmp(BENDER_STRING, x, cstring);
}

int bender_symeq(Bender x, const char *cstring) {
    return bender_strlike_cmp(BENDER_SYMBOL, x, cstring);
}

int32_t bender_getinteger(const Bender *argv, int32_t n) {
    Bender x = argv[n];
    if (!bender_checkint(x)) {
        bender_panicf("bad slot #%d, expected 32 bit signed integer, got %v", n, x);
    }
    return bender_unwrap_integer(x);
}

uint32_t bender_getuinteger(const Bender *argv, int32_t n) {
    Bender x = argv[n];
    if (!bender_checkuint(x)) {
        bender_panicf("bad slot #%d, expected 32 bit unsigned integer, got %v", n, x);
    }
    return (uint32_t) bender_unwrap_number(x);
}

int16_t bender_getinteger16(const Bender *argv, int32_t n) {
    Bender x = argv[n];
    if (!bender_checkint16(x)) {
        bender_panicf("bad slot #%d, expected 16 bit signed integer, got %v", n, x);
    }
    return (int16_t) bender_unwrap_number(x);
}

uint16_t bender_getuinteger16(const Bender *argv, int32_t n) {
    Bender x = argv[n];
    if (!bender_checkuint16(x)) {
        bender_panicf("bad slot #%d, expected 16 bit unsigned integer, got %v", n, x);
    }
    return (uint16_t) bender_unwrap_number(x);
}

int8_t bender_getinteger8(const Bender *argv, int32_t n) {
    Bender x = argv[n];
    if (!bender_checkint8(x)) {
        bender_panicf("bad slot #%d, expected 8 bit signed integer, got %v", n, x);
    }
    return (int8_t) bender_unwrap_number(x);
}

uint8_t bender_getuinteger8(const Bender *argv, int32_t n) {
    Bender x = argv[n];
    if (!bender_checkuint8(x)) {
        bender_panicf("bad slot #%d, expected 8 bit unsigned integer, got %v", n, x);
    }
    return (uint8_t) bender_unwrap_number(x);
}

int64_t bender_getinteger64(const Bender *argv, int32_t n) {
#ifdef BENDER_INT_TYPES
    return bender_unwrap_s64(argv[n]);
#else
    Bender x = argv[n];
    if (!bender_checkint64(x)) {
        bender_panicf("bad slot #%d, expected 64 bit signed integer, got %v", n, x);
    }
    return (int64_t) bender_unwrap_number(x);
#endif
}

uint64_t bender_getuinteger64(const Bender *argv, int32_t n) {
#ifdef BENDER_INT_TYPES
    return bender_unwrap_u64(argv[n]);
#else
    Bender x = argv[n];
    if (!bender_checkuint64(x)) {
        bender_panicf("bad slot #%d, expected 64 bit unsigned integer, got %v", n, x);
    }
    return (uint64_t) bender_unwrap_number(x);
#endif
}

size_t bender_getsize(const Bender *argv, int32_t n) {
    Bender x = argv[n];
    if (!bender_checksize(x)) {
        bender_panicf("bad slot #%d, expected size, got %v", n, x);
    }
    return (size_t) bender_unwrap_number(x);
}

int32_t bender_gethalfrange(const Bender *argv, int32_t n, int32_t length, const char *which) {
    int32_t raw = bender_getinteger(argv, n);
    int32_t not_raw = raw;
    if (not_raw < 0) not_raw += length + 1;
    if (not_raw < 0 || not_raw > length)
        bender_panicf("%s index %d out of range [%d,%d]", which, (int64_t) raw, -(int64_t)length - 1, (int64_t) length);
    return not_raw;
}

int32_t bender_getstartrange(const Bender *argv, int32_t argc, int32_t n, int32_t length) {
    if (n >= argc || bender_checktype(argv[n], BENDER_NIL)) {
        return 0;
    }
    return bender_gethalfrange(argv, n, length, "start");
}

int32_t bender_getendrange(const Bender *argv, int32_t argc, int32_t n, int32_t length) {
    if (n >= argc || bender_checktype(argv[n], BENDER_NIL)) {
        return length;
    }
    return bender_gethalfrange(argv, n, length, "end");
}

int32_t bender_getargindex(const Bender *argv, int32_t n, int32_t length, const char *which) {
    int32_t raw = bender_getinteger(argv, n);
    int32_t not_raw = raw;
    if (not_raw < 0) not_raw += length;
    if (not_raw < 0 || not_raw > length)
        bender_panicf("%s index %d out of range [%d,%d)", which, (int64_t)raw, -(int64_t)length, (int64_t)length);
    return not_raw;
}

BenderView bender_getindexed(const Bender *argv, int32_t n) {
    Bender x = argv[n];
    BenderView view;
    if (!bender_indexed_view(x, &view.items, &view.len)) {
        bender_panic_type(x, n, BENDER_TFLAG_INDEXED);
    }
    return view;
}

BenderByteView bender_getbytes(const Bender *argv, int32_t n) {
    Bender x = argv[n];
    BenderByteView view;
    if (!bender_bytes_view(x, &view.bytes, &view.len)) {
        bender_panic_type(x, n, BENDER_TFLAG_BYTES);
    }
    return view;
}

BenderDictView bender_getdictionary(const Bender *argv, int32_t n) {
    Bender x = argv[n];
    BenderDictView view;
    if (!bender_dictionary_view(x, &view.kvs, &view.len, &view.cap)) {
        bender_panic_type(x, n, BENDER_TFLAG_DICTIONARY);
    }
    return view;
}

void *bender_getabstract(const Bender *argv, int32_t n, const BenderAbstractType *at) {
    Bender x = argv[n];
    if (!bender_checktype(x, BENDER_ABSTRACT)) {
        bender_panic_abstract(x, n, at);
    }
    void *abstractx = bender_unwrap_abstract(x);
    if (bender_abstract_type(abstractx) != at) {
        bender_panic_abstract(x, n, at);
    }
    return abstractx;
}

BenderRange bender_getslice(int32_t argc, const Bender *argv) {
    bender_arity(argc, 1, 3);
    BenderRange range;
    int32_t length = bender_length(argv[0]);
    range.start = bender_getstartrange(argv, argc, 1, length);
    range.end = bender_getendrange(argv, argc, 2, length);
    if (range.end < range.start)
        range.end = range.start;
    return range;
}

Bender bender_dyn(const char *name) {
    if (!bender_vm.fiber) {
        if (!bender_vm.top_dyns) return bender_wrap_nil();
        return bender_table_get(bender_vm.top_dyns, bender_ckeywordv(name));
    }
    if (bender_vm.fiber->env) {
        return bender_table_get_keyword(bender_vm.fiber->env, name);
    } else {
        return bender_wrap_nil();
    }
}

void bender_setdyn(const char *name, Bender value) {
    if (!bender_vm.fiber) {
        if (!bender_vm.top_dyns) bender_vm.top_dyns = bender_table(10);
        bender_table_put(bender_vm.top_dyns, bender_ckeywordv(name), value);
    } else {
        if (!bender_vm.fiber->env) {
            bender_vm.fiber->env = bender_table(1);
        }
        bender_table_put(bender_vm.fiber->env, bender_ckeywordv(name), value);
    }
}

/* Create a function that when called, returns X. Trivial in Bender, a pain in C. */
BenderFunction *bender_thunk_delay(Bender x) {
    static const uint32_t bytecode[] = {
        JOP_LOAD_CONSTANT,
        JOP_RETURN
    };
    BenderFuncDef *def = bender_funcdef_alloc();
    def->arity = 0;
    def->min_arity = 0;
    def->max_arity = INT32_MAX;
    def->flags = BENDER_FUNCDEF_FLAG_VARARG;
    def->slotcount = 1;
    def->bytecode = bender_malloc(sizeof(bytecode));
    def->bytecode_length = (int32_t)(sizeof(bytecode) / sizeof(uint32_t));
    def->constants = bender_malloc(sizeof(Bender));
    def->constants_length = 1;
    def->name = NULL;
    if (!def->bytecode || !def->constants) {
        BENDER_OUT_OF_MEMORY;
    }
    def->constants[0] = x;
    memcpy(def->bytecode, bytecode, sizeof(bytecode));
    bender_def_addflags(def);
    /* bender_verify(def); */
    return bender_thunk(def);
}

uint64_t bender_getflags(const Bender *argv, int32_t n, const char *flags) {
    uint64_t ret = 0;
    const uint8_t *keyw = bender_getkeyword(argv, n);
    int32_t klen = bender_string_length(keyw);
    int32_t flen = (int32_t) strlen(flags);
    if (flen > 64) {
        flen = 64;
    }
    for (int32_t j = 0; j < klen; j++) {
        for (int32_t i = 0; i < flen; i++) {
            if (((uint8_t) flags[i]) == keyw[j]) {
                ret |= 1ULL << i;
                goto found;
            }
        }
        bender_panicf("unexpected flag %c, expected one of \"%s\"", (char) keyw[j], flags);
    found:
        ;
    }
    return ret;
}

int32_t bender_optnat(const Bender *argv, int32_t argc, int32_t n, int32_t dflt) {
    if (argc <= n) return dflt;
    if (bender_checktype(argv[n], BENDER_NIL)) return dflt;
    return bender_getnat(argv, n);
}

int32_t bender_optinteger(const Bender *argv, int32_t argc, int32_t n, int32_t dflt) {
    if (argc <= n) return dflt;
    if (bender_checktype(argv[n], BENDER_NIL)) return dflt;
    return bender_getinteger(argv, n);
}

int64_t bender_optinteger64(const Bender *argv, int32_t argc, int32_t n, int64_t dflt) {
    if (argc <= n) return dflt;
    if (bender_checktype(argv[n], BENDER_NIL)) return dflt;
    return bender_getinteger64(argv, n);
}

size_t bender_optsize(const Bender *argv, int32_t argc, int32_t n, size_t dflt) {
    if (argc <= n) return dflt;
    if (bender_checktype(argv[n], BENDER_NIL)) return dflt;
    return bender_getsize(argv, n);
}

void *bender_optabstract(const Bender *argv, int32_t argc, int32_t n, const BenderAbstractType *at, void *dflt) {
    if (argc <= n) return dflt;
    if (bender_checktype(argv[n], BENDER_NIL)) return dflt;
    return bender_getabstract(argv, n, at);
}

uint32_t bender_optuinteger(const Bender *argv, int32_t argc, int32_t n, uint32_t dflt) {
    if (argc <= n) return dflt;
    if (bender_checktype(argv[n], BENDER_NIL)) return dflt;
    return bender_getuinteger(argv, n);
}

uint64_t bender_optuinteger64(const Bender *argv, int32_t argc, int32_t n, uint64_t dflt) {
    if (argc <= n) return dflt;
    if (bender_checktype(argv[n], BENDER_NIL)) return dflt;
    return bender_getuinteger64(argv, n);
}

/* Atomic refcounts */

BenderAtomicInt bender_atomic_inc(BenderAtomicInt volatile *x) {
#ifdef _MSC_VER
    return _InterlockedIncrement(x);
#elif defined(BENDER_USE_STDATOMIC)
    return atomic_fetch_add_explicit(x, 1, memory_order_relaxed) + 1;
#elif defined(BENDER_PLAN9)
    return aincl((void *)x, 1);
#else
    return __atomic_add_fetch(x, 1, __ATOMIC_RELAXED);
#endif
}

BenderAtomicInt bender_atomic_dec(BenderAtomicInt volatile *x) {
#ifdef _MSC_VER
    return _InterlockedDecrement(x);
#elif defined(BENDER_USE_STDATOMIC)
    return atomic_fetch_add_explicit(x, -1, memory_order_acq_rel) - 1;
#elif defined(BENDER_PLAN9)
    return aincl((void *)x, -1);
#else
    return __atomic_add_fetch(x, -1, __ATOMIC_ACQ_REL);
#endif
}

BenderAtomicInt bender_atomic_load(BenderAtomicInt volatile *x) {
#ifdef _MSC_VER
    return _InterlockedOr(x, 0);
#elif defined(BENDER_PLAN9)
    return agetl((void *)x);
#elif defined(BENDER_USE_STDATOMIC)
    return atomic_load_explicit(x, memory_order_acquire);
#else
    return __atomic_load_n(x, __ATOMIC_ACQUIRE);
#endif
}

BenderAtomicInt bender_atomic_load_relaxed(BenderAtomicInt volatile *x) {
#ifdef _MSC_VER
    return _InterlockedOr(x, 0);
#elif defined(BENDER_PLAN9)
    return agetl((void *)x);
#elif defined(BENDER_USE_STDATOMIC)
    return atomic_load_explicit(x, memory_order_relaxed);
#else
    return __atomic_load_n(x, __ATOMIC_RELAXED);
#endif
}

/* Some definitions for function-like macros */

BENDER_API BenderStructHead *(bender_struct_head)(BenderStruct st) {
    return bender_struct_head(st);
}

BENDER_API BenderAbstractHead *(bender_abstract_head)(const void *abstract) {
    return bender_abstract_head(abstract);
}

BENDER_API BenderStringHead *(bender_string_head)(BenderString s) {
    return bender_string_head(s);
}

BENDER_API BenderTupleHead *(bender_tuple_head)(BenderTuple tuple) {
    return bender_tuple_head(tuple);
}
