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

#ifndef BENDER_UTIL_H_defined
#define BENDER_UTIL_H_defined

#ifndef BENDER_AMALG
#include "features.h"
#include <bender.h>
#include "state.h"
#endif

#include <stdio.h>
#include <errno.h>
#include <stddef.h>
#include <stdbool.h>
#include <math.h>

#ifdef BENDER_EV
#ifndef BENDER_WINDOWS
#include <pthread.h>
#endif
#endif

#if !defined(BENDER_REDUCED_OS) || !defined(BENDER_SINGLE_THREADED)
#include <time.h>
#define BENDER_GETTIME
#endif

/* Handle runtime errors */
#ifndef BENDER_EXIT
#include <stdio.h>
#define BENDER_EXIT(m) do { \
    fprintf(stderr, "bender abort at %s:%d: %s\n",\
        __FILE__,\
        __LINE__,\
        (m));\
    abort();\
} while (0)
#endif

#define BENDER_MARSHAL_DECREF 0x40000

#define bender_assert(c, m) do { \
    if (!(c)) BENDER_EXIT((m)); \
} while (0)

/* Utils */
uint32_t bender_hash_mix(uint32_t input, uint32_t more);

#define bender_maphash(cap, hash) ((uint32_t)(hash) & (cap - 1))

/* bender_bitwise_same is true when x and y are the same type and bits,
 * guarded for the nanboxed implementation */
#if defined(BENDER_NANBOX_64) || defined(BENDER_NANBOX_32)
#define bender_bitwise_same(x, y) (bender_u64(x) == bender_u64(y))
#else
#define bender_bitwise_same(x, y) ((x).type == (y).type && bender_u64(x) == bender_u64(y))
#endif

/* Compare a stored key with a lookup key. Most types are equal only when bitwise identical, so
 * bender_equals only runs for zero (since 0.0 and -0.0 are equal), strings, tuples, structs and abstract
 * types. NaN is never stored as a key, so we skip it. */
static inline int bender_key_equals(Bender stored, Bender key) {
    if (bender_bitwise_same(stored, key)) return 1;
    switch (bender_type(key)) {
        case BENDER_NUMBER:
            return bender_unwrap_number(key) == 0 && bender_equals(stored, key);
        case BENDER_STRING:
        case BENDER_TUPLE:
        case BENDER_STRUCT:
        case BENDER_ABSTRACT:
            return bender_equals(stored, key);
        default:
            return 0;
    }
}

int bender_valid_utf8(const uint8_t *str, int32_t len);

int bender_is_symbol_char(uint8_t c);

extern const char bender_base64[65];

int32_t bender_array_calchash(const Bender *array, int32_t len);

int32_t bender_kv_calchash(const BenderKV *kvs, int32_t len);

int32_t bender_string_calchash(const uint8_t *str, int32_t len);

int32_t bender_tablen(int32_t n);

void safe_memcpy(void *dest, const void *src, size_t len);

void bender_buffer_push_types(BenderBuffer *buffer, int types);

const BenderKV *bender_dict_find(const BenderKV *buckets, int32_t cap, Bender key);

void bender_memempty(BenderKV *mem, int32_t count);

void *bender_memalloc_empty(int32_t count);

BenderTable *bender_get_core_table(const char *name);

void bender_def_addflags(BenderFuncDef *def);

void bender_buffer_dtostr(BenderBuffer *buffer, double x);

const char *bender_strerror(int e);

const void *bender_strbinsearch(
    const void *tab,
    size_t tabcount,
    size_t itemsize,
    const uint8_t *key);

void bender_buffer_format(
    BenderBuffer *b,
    const char *strfrmt,
    int32_t argstart,
    int32_t argc,
    Bender *argv);

Bender bender_next_impl(Bender ds, Bender key, int is_interpreter);

BenderBinding bender_binding_from_entry(Bender entry);

BenderByteView bender_text_substitution(
    Bender *subst,
    const uint8_t *bytes,
    uint32_t len,
    BenderArray *extra_args);

const BenderKV *bender_dict_find_keyword(
    const BenderKV *buckets,
    int32_t cap,
    const uint8_t *cstr,
    int32_t cstr_len);

Bender bender_table_get_keyword(BenderTable *t, const char *keyword);

/* P3.4 bookkeeping put (marshal memo, threaded registry): like
 * bender_table_put but generational-dirty only (no xfb record, no
 * export panic). */
void bender_table_put_raw(BenderTable *t, Bender key, Bender value);

/* Registry functions */
void bender_registry_put(
    BenderCFunction key,
    const char *name,
    const char *name_prefix,
    const char *source_file,
    int32_t source_line);
BenderCFunRegistry *bender_registry_get(BenderCFunction key);

/* Inside the bender core, defining globals is different
 * at bootstrap time and normal runtime */
#ifdef BENDER_BOOTSTRAP
#define BENDER_CORE_REG BENDER_REG
#define BENDER_CORE_FN BENDER_FN
#define BENDER_CORE_DEF BENDER_DEF
#define bender_core_def_sm bender_def_sm
#define bender_core_cfuns_ext bender_cfuns_ext
#else
#define BENDER_CORE_REG BENDER_REG_S
#define BENDER_CORE_FN BENDER_FN_S
#define BENDER_CORE_DEF(ENV, NAME, X, DOC) bender_core_def_sm(ENV, NAME, X, DOC, NULL, 0)
void bender_core_def_sm(BenderTable *env, const char *name, Bender x, const void *p, const void *sf, int32_t sl);
void bender_core_cfuns_ext(BenderTable *env, const char *regprefix, const BenderRegExt *cfuns);
#endif

/* Clock gettime */
#ifdef BENDER_GETTIME
enum BenderTimeSource {
    BENDER_TIME_REALTIME,
    BENDER_TIME_MONOTONIC,
    BENDER_TIME_CPUTIME
};
int bender_gettime(struct timespec *spec, enum BenderTimeSource source);
#endif

#ifdef BENDER_BOOTSTRAP
extern int64_t total_instruction_count;
extern int64_t total_optimize_fixpoint_loops;
extern int64_t total_funcdefs_optimized;
#endif

/* strdup */
#ifdef BENDER_WINDOWS
#define strdup(x) _strdup(x)
#endif

/* Safely allocate arrays */
void *array_allocate(size_t element_size, int32_t count);

/* Use LoadLibrary on windows or dlopen on posix to load dynamic libraries
 * with native code. */
#if defined(BENDER_NO_DYNAMIC_MODULES)
typedef int Clib;
#define load_clib(name) ((void) name, 0)
#define symbol_clib(lib, sym) ((void) lib, (void) sym, NULL)
const char *error_clib(void);
#define free_clib(c) ((void) (c), 0)
#elif defined(BENDER_WINDOWS)
#include <windows.h>
typedef HINSTANCE Clib;
void *symbol_clib(Clib clib, const char *sym);
void free_clib(Clib clib);
Clib load_clib(const char *name);
char *error_clib(void);
#else
#include <dlfcn.h>
typedef void *Clib;
#define load_clib(name) dlopen((name), RTLD_NOW)
#define free_clib(lib) dlclose((lib))
#define symbol_clib(lib, sym) dlsym((lib), (sym))
#define error_clib dlerror
#endif
char *get_processed_name(const char *name);

#ifdef BENDER_PLAN9
#define RETRY_EINTR(RC, CALL) (RC) = CALL;
#else
#define RETRY_EINTR(RC, CALL) do { (RC) = CALL; } while((RC) < 0 && errno == EINTR)
#endif

#ifdef BENDER_EV
#ifdef BENDER_WINDOWS
#include <winsock2.h>
#include <windows.h>
#include <io.h>
typedef struct {
    union {
        OVERLAPPED overlapped;
        WSAOVERLAPPED wsaoverlapped;
    } as;
    uint32_t bytes_transfered;
} BenderOverlapped;
#endif
#endif

/* Initialize builtin libraries */
void bender_lib_io(BenderTable *env);
void bender_lib_math(BenderTable *env);
void bender_lib_array(BenderTable *env);
void bender_lib_tuple(BenderTable *env);
void bender_lib_buffer(BenderTable *env);
void bender_lib_table(BenderTable *env);
void bender_lib_struct(BenderTable *env);
void bender_lib_fiber(BenderTable *env);
void bender_lib_os(BenderTable *env);
void bender_lib_string(BenderTable *env);
void bender_lib_marsh(BenderTable *env);
void bender_lib_parse(BenderTable *env);
#ifdef BENDER_ASSEMBLER
void bender_lib_asm(BenderTable *env);
#endif
void bender_lib_compile(BenderTable *env);
void bender_lib_debug(BenderTable *env);
#ifdef BENDER_PEG
void bender_lib_peg(BenderTable *env);
#endif
#ifdef BENDER_INT_TYPES
void bender_lib_inttypes(BenderTable *env);
#endif
#ifdef BENDER_NET
void bender_lib_net(BenderTable *env);
extern const BenderAbstractType bender_address_type;
#endif
#ifdef BENDER_EV
void bender_lib_ev(BenderTable *env);
void bender_lib_proc(BenderTable *env);
void bender_ev_mark(void);
/* Erlang-parity messaging fix: scoped-minor-only variant of
 * bender_ev_mark that shallow-marks queued fibers (reachable bit only,
 * no stack walk) instead of fully marking them. See gc.h's
 * bender_gc_mark_fiber_shallow and docs/internals/proc-lanes.md. */
void bender_ev_mark_shallow(void);
/* Drain ready I/O and expired timers once, without blocking or running tasks. */
void bender_loop1_poll_now(void);
void bender_async_start_fiber(BenderFiber *fiber, BenderStream *stream, BenderAsyncMode mode, BenderEVCallback callback, void *state);
int bender_make_pipe(BenderHandle handles[2], int mode);
#ifdef BENDER_FILEWATCH
void bender_lib_filewatch(BenderTable *env);
#endif
#endif
#ifdef BENDER_FFI
void bender_lib_ffi(BenderTable *env);
#endif

#endif
