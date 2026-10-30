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
#include "gc.h"
#ifdef BENDER_WINDOWS
#include <windows.h>
#else
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#endif
#endif

#ifdef BENDER_WINDOWS
#ifdef BENDER_DYNAMIC_MODULES
#include <psapi.h>
#ifdef BENDER_MSVC
#pragma comment (lib, "Psapi.lib")
#endif
#endif
#endif

#ifdef BENDER_APPLE
#include <AvailabilityMacros.h>
#endif

#include <inttypes.h>
#include <float.h>

/* Base 64 lookup table for digits */
const char bender_base64[65] =
    "0123456789"
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
    "abcdefghijklmnopqrstuvwxyz"
    "_=";

/* The BENDER value types in order. These types can be used as
 * mnemonics instead of a bit pattern for type checking */
const char *const bender_type_names[16] = {
    "number",
    "nil",
    "boolean",
    "fiber",
    "string",
    "symbol",
    "keyword",
    "array",
    "tuple",
    "table",
    "struct",
    "buffer",
    "function",
    "cfunction",
    "abstract",
    "pointer"
};

/* Docstring for signal lists these */
const char *const bender_signal_names[14] = {
    "ok",
    "error",
    "debug",
    "yield",
    "user0",
    "user1",
    "user2",
    "user3",
    "user4",
    "user5",
    "user6",
    "user7",
    "interrupt",
    "await"
};

/* Docstring for fiber/status lists these */
const char *const bender_status_names[16] = {
    "dead",
    "error",
    "debug",
    "pending",
    "user0",
    "user1",
    "user2",
    "user3",
    "user4",
    "user5",
    "user6",
    "user7",
    "interrupted",
    "suspended",
    "new",
    "alive"
};

uint32_t bender_hash_mix(uint32_t input, uint32_t more) {
    uint32_t mix1 = (more + 0x9e3779b9 + (input << 6) + (input >> 2));
    return input ^ (0x9e3779b9 + (mix1 << 6) + (mix1 >> 2));
}

#ifndef BENDER_PRF

int32_t bender_string_calchash(const uint8_t *str, int32_t len) {
    if (NULL == str || len == 0) return 5381;
    const uint8_t *end = str + len;
    uint32_t hash = 5381;
    while (str < end)
        hash = (hash << 5) + hash + *str++;
    hash = bender_hash_mix(hash, (uint32_t) len);
    return (int32_t) hash;
}

#else

/*
  Public domain siphash implementation sourced from:

  https://raw.githubusercontent.com/veorq/SipHash/master/halfsiphash.c

  We have made a few alterations, such as hardcoding the output size
  and then removing dead code.
*/
#define cROUNDS 2
#define dROUNDS 4

#define ROTL(x, b) (uint32_t)(((x) << (b)) | ((x) >> (32 - (b))))

#define U8TO32_LE(p)                                                           \
    (((uint32_t)((p)[0])) | ((uint32_t)((p)[1]) << 8) |                        \
     ((uint32_t)((p)[2]) << 16) | ((uint32_t)((p)[3]) << 24))

#define SIPROUND                                                               \
    do {                                                                       \
        v0 += v1;                                                              \
        v1 = ROTL(v1, 5);                                                      \
        v1 ^= v0;                                                              \
        v0 = ROTL(v0, 16);                                                     \
        v2 += v3;                                                              \
        v3 = ROTL(v3, 8);                                                      \
        v3 ^= v2;                                                              \
        v0 += v3;                                                              \
        v3 = ROTL(v3, 7);                                                      \
        v3 ^= v0;                                                              \
        v2 += v1;                                                              \
        v1 = ROTL(v1, 13);                                                     \
        v1 ^= v2;                                                              \
        v2 = ROTL(v2, 16);                                                     \
    } while (0)

static uint32_t halfsiphash(const uint8_t *in, const size_t inlen, const uint8_t *k) {

    uint32_t v0 = 0;
    uint32_t v1 = 0;
    uint32_t v2 = UINT32_C(0x6c796765);
    uint32_t v3 = UINT32_C(0x74656462);
    uint32_t k0 = U8TO32_LE(k);
    uint32_t k1 = U8TO32_LE(k + 4);
    uint32_t m;
    int i;
    const uint8_t *end = in + inlen - (inlen % sizeof(uint32_t));
    const int left = inlen & 3;
    uint32_t b = ((uint32_t)inlen) << 24;
    v3 ^= k1;
    v2 ^= k0;
    v1 ^= k1;
    v0 ^= k0;

    for (; in != end; in += 4) {
        m = U8TO32_LE(in);
        v3 ^= m;

        for (i = 0; i < cROUNDS; ++i)
            SIPROUND;

        v0 ^= m;
    }

    switch (left) {
        case 3:
            b |= ((uint32_t)in[2]) << 16;
        /* fallthrough */
        case 2:
            b |= ((uint32_t)in[1]) << 8;
        /* fallthrough */
        case 1:
            b |= ((uint32_t)in[0]);
            break;
        case 0:
            break;
    }

    v3 ^= b;

    for (i = 0; i < cROUNDS; ++i)
        SIPROUND;

    v0 ^= b;

    v2 ^= 0xff;

    for (i = 0; i < dROUNDS; ++i)
        SIPROUND;

    b = v1 ^ v3;
    return b;
}
/* end of siphash */

static uint8_t hash_key[BENDER_HASH_KEY_SIZE] = {0};

void bender_init_hash_key(uint8_t new_key[BENDER_HASH_KEY_SIZE]) {
    memcpy(hash_key, new_key, sizeof(hash_key));
}

/* Calculate hash for string */

int32_t bender_string_calchash(const uint8_t *str, int32_t len) {
    uint32_t hash;
    hash = halfsiphash(str, len, hash_key);
    return (int32_t)hash;
}

#endif

/* Computes hash of an array of values */
int32_t bender_array_calchash(const Bender *array, int32_t len) {
    const Bender *end = array + len;
    uint32_t hash = 33;
    while (array < end) {
        hash = bender_hash_mix(hash, bender_hash(*array++));
    }
    return (int32_t) hash;
}

/* Computes hash of an array of values */
int32_t bender_kv_calchash(const BenderKV *kvs, int32_t len) {
    const BenderKV *end = kvs + len;
    uint32_t hash = 33;
    while (kvs < end) {
        hash = bender_hash_mix(hash, bender_hash(kvs->key));
        hash = bender_hash_mix(hash, bender_hash(kvs->value));
        kvs++;
    }
    return (int32_t) hash;
}

/* Calculate next power of 2. May overflow. If n < 0,
 * will return 0. */
int32_t bender_tablen(int32_t n) {
    if (n < 0) return 0;
    n |= n >> 1;
    n |= n >> 2;
    n |= n >> 4;
    n |= n >> 8;
    n |= n >> 16;
    return n == INT32_MAX ? INT32_MAX : n + 1;
}

/* Avoid some undefined behavior that was common in the code base. */
void safe_memcpy(void *dest, const void *src, size_t len) {
    if (!len) return;
    memcpy(dest, src, len);
}

/* Helper to find a value in a Bender struct or table. Returns the bucket
 * containing the key, or the first empty bucket if there is no such key. */
const BenderKV *bender_dict_find(const BenderKV *buckets, int32_t cap, Bender key) {
    int32_t index = bender_maphash(cap, bender_hash(key));
    int32_t i;
    const BenderKV *first_bucket = NULL;
    /* Higher half */
    for (i = index; i < cap; i++) {
        const BenderKV *kv = buckets + i;
        if (bender_checktype(kv->key, BENDER_NIL)) {
            if (bender_checktype(kv->value, BENDER_NIL)) {
                return kv;
            } else if (NULL == first_bucket) {
                first_bucket = kv;
            }
        } else if (bender_key_equals(kv->key, key)) {
            return buckets + i;
        }
    }
    /* Lower half */
    for (i = 0; i < index; i++) {
        const BenderKV *kv = buckets + i;
        if (bender_checktype(kv->key, BENDER_NIL)) {
            if (bender_checktype(kv->value, BENDER_NIL)) {
                return kv;
            } else if (NULL == first_bucket) {
                first_bucket = kv;
            }
        } else if (bender_key_equals(kv->key, key)) {
            return buckets + i;
        }
    }
    return first_bucket;
}

/* Helper to find a keyword, symbol, or string in a Bender struct or table without allocating
 * memory or needing to find interned symbols */
const BenderKV *bender_dict_find_keyword(
    const BenderKV *buckets, int32_t cap,
    const uint8_t *cstr, int32_t cstr_len) {
    int32_t hash = bender_string_calchash(cstr, cstr_len);
    int32_t index = bender_maphash(cap, hash);
    int32_t i;
    const BenderKV *first_bucket = NULL;
    /* Higher half */
    for (i = index; i < cap; i++) {
        const BenderKV *kv = buckets + i;
        if (bender_checktype(kv->key, BENDER_NIL)) {
            if (bender_checktype(kv->value, BENDER_NIL)) {
                return kv;
            } else if (NULL == first_bucket) {
                first_bucket = kv;
            }
        } else if (bender_checktype(kv->key, BENDER_KEYWORD)) {
            /* Works for symbol and keyword, too */
            BenderString str = bender_unwrap_string(kv->key);
            int32_t len = bender_string_length(str);
            if (hash == bender_string_hash(str) && len == cstr_len && !memcmp(str, cstr, len)) {
                return buckets + i;
            }
        }
    }
    /* Lower half */
    for (i = 0; i < index; i++) {
        const BenderKV *kv = buckets + i;
        if (bender_checktype(kv->key, BENDER_NIL)) {
            if (bender_checktype(kv->value, BENDER_NIL)) {
                return kv;
            } else if (NULL == first_bucket) {
                first_bucket = kv;
            }
        } else if (bender_checktype(kv->key, BENDER_KEYWORD)) {
            /* Works for symbol and keyword, too */
            BenderString str = bender_unwrap_string(kv->key);
            int32_t len = bender_string_length(str);
            if (hash == bender_string_hash(str) && len == cstr_len && !memcmp(str, cstr, len)) {
                return buckets + i;
            }
        }
    }
    return first_bucket;
}

/* Get a value from a bender struct or table. */
Bender bender_dictionary_get(const BenderKV *data, int32_t cap, Bender key) {
    const BenderKV *kv = bender_dict_find(data, cap, key);
    if (kv && !bender_checktype(kv->key, BENDER_NIL)) {
        return kv->value;
    }
    return bender_wrap_nil();
}

/* Iterate through a struct or dictionary generically */
const BenderKV *bender_dictionary_next(const BenderKV *kvs, int32_t cap, const BenderKV *kv) {
    const BenderKV *end = kvs + cap;
    kv = (kv == NULL) ? kvs : kv + 1;
    while (kv < end) {
        if (!bender_checktype(kv->key, BENDER_NIL))
            return kv;
        kv++;
    }
    return NULL;
}

/* Compare a bender string with a cstring. More efficient than loading
 * c string as a bender string. */
int bender_cstrcmp(const uint8_t *str, const char *other) {
    int32_t len = bender_string_length(str);
    int32_t index;
    for (index = 0; index < len; index++) {
        uint8_t c = str[index];
        uint8_t k = ((const uint8_t *)other)[index];
        if (c < k) return -1;
        if (c > k) return 1;
        if (k == '\0') break;
    }
    return (other[index] == '\0') ? 0 : -1;
}

/* Do a binary search on a static array of structs. Each struct must
 * have a string as its first element, and the struct must be sorted
 * lexicographically by that element. */
const void *bender_strbinsearch(
    const void *tab,
    size_t tabcount,
    size_t itemsize,
    const uint8_t *key) {
    size_t low = 0;
    size_t hi = tabcount;
    const char *t = (const char *)tab;
    while (low < hi) {
        size_t mid = low + ((hi - low) / 2);
        const char **item = (const char **)(t + mid * itemsize);
        const char *name = *item;
        int comp = bender_cstrcmp(key, name);
        if (comp < 0) {
            hi = mid;
        } else if (comp > 0) {
            low = mid + 1;
        } else {
            return (const void *)item;
        }
    }
    return NULL;
}

/* Add sourcemapping and documentation to a binding table */
static void bender_add_meta(BenderTable *table, const char *doc, const char *source_file, int32_t source_line) {
    if (doc) {
        bender_table_put(table, bender_ckeywordv("doc"), bender_cstringv(doc));
    }
    if (source_file && source_line) {
        Bender triple[3];
        triple[0] = bender_cstringv(source_file);
        triple[1] = bender_wrap_integer(source_line);
        triple[2] = bender_wrap_integer(1);
        Bender value = bender_wrap_tuple(bender_tuple_n(triple, 3));
        bender_table_put(table, bender_ckeywordv("source-map"), value);
    }
}

/* Add a def to an environment */
void bender_def_sm(BenderTable *env, const char *name, Bender val, const char *doc, const char *source_file, int32_t source_line) {
    BenderTable *subt = bender_table(2);
    bender_table_put(subt, bender_ckeywordv("value"), val);
    bender_add_meta(subt, doc, source_file, source_line);
    bender_table_put(env, bender_csymbolv(name), bender_wrap_table(subt));
}
void bender_def(BenderTable *env, const char *name, Bender value, const char *doc) {
    bender_def_sm(env, name, value, doc, NULL, 0);
}

/* Add a var to the environment */
void bender_var_sm(BenderTable *env, const char *name, Bender val, const char *doc, const char *source_file, int32_t source_line) {
    BenderArray *array = bender_array(1);
    BenderTable *subt = bender_table(2);
    bender_array_push(array, val);
    bender_table_put(subt, bender_ckeywordv("ref"), bender_wrap_array(array));
    bender_add_meta(subt, doc, source_file, source_line);
    bender_table_put(env, bender_csymbolv(name), bender_wrap_table(subt));
}
void bender_var(BenderTable *env, const char *name, Bender val, const char *doc) {
    bender_var_sm(env, name, val, doc, NULL, 0);
}

/* Registry functions */

/* Put the registry in sorted order. */
static void bender_registry_sort(void) {
    for (size_t i = 1; i < bender_vm.registry_count; i++) {
        BenderCFunRegistry reg = bender_vm.registry[i];
        size_t j;
        for (j = i; j > 0; j--) {
            if ((void *)(bender_vm.registry[j - 1].cfun) < (void *)(reg.cfun)) break;
            bender_vm.registry[j] = bender_vm.registry[j - 1];
        }
        bender_vm.registry[j] = reg;
    }
    bender_vm.registry_dirty = 0;
}

void bender_registry_put(
    BenderCFunction key,
    const char *name,
    const char *name_prefix,
    const char *source_file,
    int32_t source_line) {
    if (bender_vm.registry_count == bender_vm.registry_cap) {
        size_t newcap = (bender_vm.registry_count + 1) * 2;
        /* Size it nicely with core by default */
        if (newcap < 512) {
            newcap = 512;
        }
        void *newmem = bender_realloc(bender_vm.registry, newcap * sizeof(BenderCFunRegistry));
        if (NULL == newmem) {
            BENDER_OUT_OF_MEMORY;
        }
        bender_vm.registry = newmem;
        bender_vm.registry_cap = newcap;
    }
    BenderCFunRegistry value = {
        key,
        name,
        name_prefix,
        source_file,
        source_line
    };
    bender_vm.registry[bender_vm.registry_count++] = value;
    bender_vm.registry_dirty = 1;
}

BenderCFunRegistry *bender_registry_get(BenderCFunction key) {
    if (bender_vm.registry_dirty) {
        bender_registry_sort();
    }
    for (size_t i = 0; i < bender_vm.registry_count; i++) {
        if (bender_vm.registry[i].cfun == key) {
            return bender_vm.registry + i;
        }
    }
    BenderCFunRegistry *lo = bender_vm.registry;
    BenderCFunRegistry *hi = lo + bender_vm.registry_count;
    while (lo < hi) {
        BenderCFunRegistry *mid = lo + (hi - lo) / 2;
        if (mid->cfun == key) {
            return mid;
        }
        if ((void *)(mid->cfun) > (void *)(key)) {
            hi = mid;
        } else {
            lo = mid + 1;
        }
    }
    return NULL;
}

typedef struct {
    char *buf;
    size_t plen;
} NameBuf;

static void namebuf_init(NameBuf *namebuf, const char *prefix) {
    size_t plen = strlen(prefix);
    namebuf->plen = plen;
    namebuf->buf = bender_smalloc(namebuf->plen + 256);
    if (NULL == namebuf->buf) {
        BENDER_OUT_OF_MEMORY;
    }
    memcpy(namebuf->buf, prefix, plen);
    namebuf->buf[plen] = '/';
}

static void namebuf_deinit(NameBuf *namebuf) {
    bender_sfree(namebuf->buf);
}

static char *namebuf_name(NameBuf *namebuf, const char *suffix) {
    size_t slen = strlen(suffix);
    namebuf->buf = bender_srealloc(namebuf->buf, namebuf->plen + 2 + slen);
    if (NULL == namebuf->buf) {
        BENDER_OUT_OF_MEMORY;
    }
    memcpy(namebuf->buf + namebuf->plen + 1, suffix, slen);
    namebuf->buf[namebuf->plen + 1 + slen] = '\0';
    return (char *)(namebuf->buf);
}

/* Add a little bit of safety when using nanboxing on arm. Instead of inserting run-time checks everywhere, we are
 * only doing it during registration which has much less cost (1 shift and mask). */
static void bender_check_pointer_align(void *p) {
    (void) p;
#if defined(BENDER_NANBOX_64) && BENDER_NANBOX_64_POINTER_SHIFT != 0
    union {
        void *p;
        uintptr_t u;
    } un;
    un.p = p;
    bender_assert(!(un.u & (uintptr_t)((1 << BENDER_NANBOX_64_POINTER_SHIFT) - 1)),
                 "unaligned pointer wrap - cfunction pointers and abstract types must be aligned with this nanboxing configuration.");
#endif
}

void bender_cfuns(BenderTable *env, const char *regprefix, const BenderReg *cfuns) {
    while (cfuns->name) {
        bender_check_pointer_align(cfuns->cfun);
        Bender fun = bender_wrap_cfunction(cfuns->cfun);
        if (env) bender_def(env, cfuns->name, fun, cfuns->documentation);
        bender_registry_put(cfuns->cfun, cfuns->name, regprefix, NULL, 0);
        cfuns++;
    }
}

void bender_cfuns_ext(BenderTable *env, const char *regprefix, const BenderRegExt *cfuns) {
    while (cfuns->name) {
        bender_check_pointer_align(cfuns->cfun);
        Bender fun = bender_wrap_cfunction(cfuns->cfun);
        if (env) bender_def_sm(env, cfuns->name, fun, cfuns->documentation, cfuns->source_file, cfuns->source_line);
        bender_registry_put(cfuns->cfun, cfuns->name, regprefix, cfuns->source_file, cfuns->source_line);
        cfuns++;
    }
}

void bender_cfuns_prefix(BenderTable *env, const char *regprefix, const BenderReg *cfuns) {
    NameBuf nb;
    if (env) namebuf_init(&nb, regprefix);
    while (cfuns->name) {
        bender_check_pointer_align(cfuns->cfun);
        Bender fun = bender_wrap_cfunction(cfuns->cfun);
        if (env) bender_def(env, namebuf_name(&nb, cfuns->name), fun, cfuns->documentation);
        bender_registry_put(cfuns->cfun, cfuns->name, regprefix, NULL, 0);
        cfuns++;
    }
    if (env) namebuf_deinit(&nb);
}

void bender_cfuns_ext_prefix(BenderTable *env, const char *regprefix, const BenderRegExt *cfuns) {
    NameBuf nb;
    if (env) namebuf_init(&nb, regprefix);
    while (cfuns->name) {
        bender_check_pointer_align(cfuns->cfun);
        Bender fun = bender_wrap_cfunction(cfuns->cfun);
        if (env) bender_def_sm(env, namebuf_name(&nb, cfuns->name), fun, cfuns->documentation, cfuns->source_file, cfuns->source_line);
        bender_registry_put(cfuns->cfun, cfuns->name, regprefix, cfuns->source_file, cfuns->source_line);
        cfuns++;
    }
    if (env) namebuf_deinit(&nb);
}

/* Register a value in the global registry */
void bender_register(const char *name, BenderCFunction cfun) {
    bender_registry_put(cfun, name, NULL, NULL, 0);
}

/* Abstract type introspection */

void bender_register_abstract_type(const BenderAbstractType *at) {
    bender_check_pointer_align((void *) at);
    Bender sym = bender_csymbolv(at->name);
    Bender check = bender_table_get(bender_vm.abstract_registry, sym);
    if (!bender_checktype(check, BENDER_NIL) && at != bender_unwrap_pointer(check)) {
        bender_panicf("cannot register abstract type %s, "
                     "a type with the same name exists", at->name);
    }
    bender_table_put(bender_vm.abstract_registry, sym, bender_wrap_pointer((void *) at));
}

const BenderAbstractType *bender_get_abstract_type(Bender key) {
    Bender wrapped = bender_table_get(bender_vm.abstract_registry, key);
    if (bender_checktype(wrapped, BENDER_NIL)) {
        return NULL;
    }
    return (BenderAbstractType *)(bender_unwrap_pointer(wrapped));
}

#ifndef BENDER_BOOTSTRAP
void bender_core_def_sm(BenderTable *env, const char *name, Bender x, const void *p, const void *sf, int32_t sl) {
    (void) sf;
    (void) sl;
    (void) p;
    Bender key = bender_csymbolv(name);
    bender_table_put(env, key, x);
    if (bender_checktype(x, BENDER_CFUNCTION)) {
        bender_registry_put(bender_unwrap_cfunction(x), name, NULL, NULL, 0);
    }
}

void bender_core_cfuns_ext(BenderTable *env, const char *regprefix, const BenderRegExt *cfuns) {
    (void) regprefix;
    while (cfuns->name) {
        bender_check_pointer_align(cfuns->cfun);
        Bender fun = bender_wrap_cfunction(cfuns->cfun);
        bender_table_put(env, bender_csymbolv(cfuns->name), fun);
        bender_registry_put(cfuns->cfun, cfuns->name, regprefix, cfuns->source_file, cfuns->source_line);
        cfuns++;
    }
}
#endif

BenderBinding bender_binding_from_entry(Bender entry) {
    BenderTable *entry_table;
    BenderBinding binding = {
        BENDER_BINDING_NONE,
        bender_wrap_nil(),
        BENDER_BINDING_DEP_NONE
    };

    /* Check environment for entry */
    if (!bender_checktype(entry, BENDER_TABLE))
        return binding;
    entry_table = bender_unwrap_table(entry);

    Bender deprecate = bender_table_get_keyword(entry_table, "deprecated");
    int macro = bender_truthy(bender_table_get_keyword(entry_table, "macro"));
    Bender value = bender_table_get_keyword(entry_table, "value");
    Bender ref = bender_table_get_keyword(entry_table, "ref");

    if (bender_checktype(deprecate, BENDER_KEYWORD)) {
        BenderKeyword depkw = bender_unwrap_keyword(deprecate);
        if (!bender_cstrcmp(depkw, "relaxed")) {
            binding.deprecation = BENDER_BINDING_DEP_RELAXED;
        } else if (!bender_cstrcmp(depkw, "normal")) {
            binding.deprecation = BENDER_BINDING_DEP_NORMAL;
        } else if (!bender_cstrcmp(depkw, "strict")) {
            binding.deprecation = BENDER_BINDING_DEP_STRICT;
        }
    } else if (!bender_checktype(deprecate, BENDER_NIL)) {
        binding.deprecation = BENDER_BINDING_DEP_NORMAL;
    }

    int ref_is_valid = bender_checktype(ref, BENDER_ARRAY);
    int redef = ref_is_valid && bender_truthy(bender_table_get_keyword(entry_table, "redef"));

    if (macro) {
        binding.value = redef ? ref : value;
        binding.type = redef ? BENDER_BINDING_DYNAMIC_MACRO : BENDER_BINDING_MACRO;
        return binding;
    }

    if (ref_is_valid) {
        binding.value = ref;
        binding.type = redef ? BENDER_BINDING_DYNAMIC_DEF : BENDER_BINDING_VAR;
    } else {
        binding.value = value;
        binding.type = BENDER_BINDING_DEF;
    }

    return binding;
}

/* If the value at the given address can be coerced to a byte view,
   return that byte view. If it can't, replace the value at the address
   with the result of bender_to_string, and return a byte view over that
   string. */
static BenderByteView memoize_byte_view(Bender *value) {
    BenderByteView result;
    if (!bender_bytes_view(*value, &result.bytes, &result.len)) {
        BenderString str = bender_to_string(*value);
        *value = bender_wrap_string(str);
        result.bytes = str;
        result.len = bender_string_length(str);
    }
    return result;
}

static BenderByteView to_byte_view(Bender value) {
    BenderByteView result;
    if (!bender_bytes_view(value, &result.bytes, &result.len)) {
        BenderString str = bender_to_string(value);
        result.bytes = str;
        result.len = bender_string_length(str);
    }
    return result;
}

BenderByteView bender_text_substitution(
    Bender *subst,
    const uint8_t *bytes,
    uint32_t len,
    BenderArray *extra_argv) {
    int32_t extra_argc = extra_argv == NULL ? 0 : extra_argv->count;
    BenderType type = bender_type(*subst);
    switch (type) {
        case BENDER_FUNCTION:
        case BENDER_CFUNCTION: {
            int32_t argc = 1 + extra_argc;
            Bender *argv = bender_tuple_begin(argc);
            argv[0] = bender_stringv(bytes, len);
            for (int32_t i = 0; i < extra_argc; i++) {
                argv[i + 1] = extra_argv->data[i];
            }
            bender_tuple_end(argv);
            if (type == BENDER_FUNCTION) {
                return to_byte_view(bender_call(bender_unwrap_function(*subst), argc, argv));
            } else {
                return to_byte_view(bender_unwrap_cfunction(*subst)(argc, argv));
            }
        }
        default:
            return memoize_byte_view(subst);
    }
}

BenderBinding bender_resolve_ext(BenderTable *env, const uint8_t *sym) {
    Bender entry = bender_table_get(env, bender_wrap_symbol(sym));
    return bender_binding_from_entry(entry);
}

BenderBindingType bender_resolve(BenderTable *env, const uint8_t *sym, Bender *out) {
    BenderBinding binding = bender_resolve_ext(env, sym);
    if (binding.type == BENDER_BINDING_DYNAMIC_DEF || binding.type == BENDER_BINDING_DYNAMIC_MACRO) {
        *out = bender_array_peek(bender_unwrap_array(binding.value));
    } else {
        *out = binding.value;
    }
    return binding.type;
}

/* Resolve a symbol in the core environment. */
Bender bender_resolve_core(const char *name) {
    BenderTable *env = bender_core_env(NULL);
    Bender out = bender_wrap_nil();
    bender_resolve(env, bender_csymbol(name), &out);
    return out;
}

/* Read both tuples and arrays as c pointers + int32_t length. Return 1 if the
 * view can be constructed, 0 if an invalid type. */
int bender_indexed_view(Bender seq, const Bender **data, int32_t *len) {
    if (bender_checktype(seq, BENDER_ARRAY)) {
        *data = bender_unwrap_array(seq)->data;
        *len = bender_unwrap_array(seq)->count;
        return 1;
    } else if (bender_checktype(seq, BENDER_TUPLE)) {
        *data = bender_unwrap_tuple(seq);
        *len = bender_tuple_length(bender_unwrap_tuple(seq));
        return 1;
    }
    return 0;
}

/* Read both strings and buffer as unsigned character array + int32_t len.
 * Returns 1 if the view can be constructed and 0 if the type is invalid. */
int bender_bytes_view(Bender str, const uint8_t **data, int32_t *len) {
    BenderType t = bender_type(str);
    if (t == BENDER_STRING || t == BENDER_SYMBOL || t == BENDER_KEYWORD) {
        *data = bender_unwrap_string(str);
        *len = bender_string_length(bender_unwrap_string(str));
        return 1;
    } else if (t == BENDER_BUFFER) {
        *data = bender_unwrap_buffer(str)->data;
        *len = bender_unwrap_buffer(str)->count;
        return 1;
    } else if (t == BENDER_ABSTRACT) {
        void *abst = bender_unwrap_abstract(str);
        const BenderAbstractType *atype = bender_abstract_type(abst);
        if (NULL == atype->bytes) {
            return 0;
        }
        BenderByteView view = atype->bytes(abst, bender_abstract_size(abst));
        *data = view.bytes;
        *len = view.len;
        return 1;
    }
    return 0;
}

/* Read both structs and tables as the entries of a hashtable with
 * identical structure. Returns 1 if the view can be constructed and
 * 0 if the type is invalid. */
int bender_dictionary_view(Bender tab, const BenderKV **data, int32_t *len, int32_t *cap) {
    if (bender_checktype(tab, BENDER_TABLE)) {
        *data = bender_unwrap_table(tab)->data;
        *cap = bender_unwrap_table(tab)->capacity;
        *len = bender_unwrap_table(tab)->count;
        return 1;
    } else if (bender_checktype(tab, BENDER_STRUCT)) {
        *data = bender_unwrap_struct(tab);
        *cap = bender_struct_capacity(bender_unwrap_struct(tab));
        *len = bender_struct_length(bender_unwrap_struct(tab));
        return 1;
    }
    return 0;
}

int bender_checkint(Bender x) {
    if (!bender_checktype(x, BENDER_NUMBER))
        return 0;
    double dval = bender_unwrap_number(x);
    return bender_checkintrange(dval);
}

int bender_checkuint(Bender x) {
    if (!bender_checktype(x, BENDER_NUMBER))
        return 0;
    double dval = bender_unwrap_number(x);
    return bender_checkuintrange(dval);
}

int bender_checkint64(Bender x) {
    if (!bender_checktype(x, BENDER_NUMBER))
        return 0;
    double dval = bender_unwrap_number(x);
    return bender_checkint64range(dval);
}

int bender_checkuint64(Bender x) {
    if (!bender_checktype(x, BENDER_NUMBER))
        return 0;
    double dval = bender_unwrap_number(x);
    return bender_checkuint64range(dval);
}

int bender_checkint16(Bender x) {
    if (!bender_checktype(x, BENDER_NUMBER))
        return 0;
    double dval = bender_unwrap_number(x);
    return bender_checkint16range(dval);
}

int bender_checkuint16(Bender x) {
    if (!bender_checktype(x, BENDER_NUMBER))
        return 0;
    double dval = bender_unwrap_number(x);
    return bender_checkuint16range(dval);
}

int bender_checkint8(Bender x) {
    if (!bender_checktype(x, BENDER_NUMBER))
        return 0;
    double dval = bender_unwrap_number(x);
    return bender_checkint8range(dval);
}

int bender_checkuint8(Bender x) {
    if (!bender_checktype(x, BENDER_NUMBER))
        return 0;
    double dval = bender_unwrap_number(x);
    return bender_checkuint8range(dval);
}

int bender_checksize(Bender x) {
    if (!bender_checktype(x, BENDER_NUMBER))
        return 0;
    double dval = bender_unwrap_number(x);
    if (dval != (double)((size_t) dval)) return 0;
#ifdef BENDER_PLAN9
    return dval <= SIZE_MAX;
#else
    if (SIZE_MAX > BENDER_INTMAX_INT64) {
        return dval <= BENDER_INTMAX_INT64;
    } else {
        return dval <= SIZE_MAX;
    }
#endif
}

BenderTable *bender_get_core_table(const char *name) {
    BenderTable *env = bender_core_env(NULL);
    Bender out = bender_wrap_nil();
    BenderBindingType bt = bender_resolve(env, bender_csymbol(name), &out);
    if (bt == BENDER_BINDING_NONE) return NULL;
    if (!bender_checktype(out, BENDER_TABLE)) return NULL;
    return bender_unwrap_table(out);
}

/* Sort keys of a dictionary type */
int32_t bender_sorted_keys(const BenderKV *dict, int32_t cap, int32_t *index_buffer) {

    /* First, put populated indices into index_buffer */
    int32_t next_index = 0;
    for (int32_t i = 0; i < cap; i++) {
        if (!bender_checktype(dict[i].key, BENDER_NIL)) {
            index_buffer[next_index++] = i;
        }
    }

    /* Next, sort those (simple insertion sort here for now) */
    for (int32_t i = 1; i < next_index; i++) {
        int32_t index_to_insert = index_buffer[i];
        Bender lhs = dict[index_to_insert].key;
        for (int32_t j = i - 1; j >= 0; j--) {
            index_buffer[j + 1] = index_buffer[j];
            Bender rhs = dict[index_buffer[j]].key;
            if (bender_compare(lhs, rhs) >= 0) {
                index_buffer[j + 1] = index_to_insert;
                break;
            } else if (j == 0) {
                index_buffer[0] = index_to_insert;
            }
        }
    }

    /* Return number of indices found */
    return next_index;

}

/* Clock shims for various platforms */
#ifdef BENDER_GETTIME
#ifdef BENDER_WINDOWS
#include <profileapi.h>
int bender_gettime(struct timespec *spec, enum BenderTimeSource source) {
    if (source == BENDER_TIME_REALTIME) {
        FILETIME ftime;
        GetSystemTimeAsFileTime(&ftime);
        int64_t wintime = (int64_t)(ftime.dwLowDateTime) | ((int64_t)(ftime.dwHighDateTime) << 32);
        /* Windows epoch is January 1, 1601 apparently */
        wintime -= 116444736000000000LL;
        spec->tv_sec  = wintime / 10000000LL;
        /* Resolution is 100 nanoseconds. */
        spec->tv_nsec = wintime % 10000000LL * 100;
    } else if (source == BENDER_TIME_MONOTONIC) {
        LARGE_INTEGER count;
        LARGE_INTEGER perf_freq;
        QueryPerformanceCounter(&count);
        QueryPerformanceFrequency(&perf_freq);
        spec->tv_sec = count.QuadPart / perf_freq.QuadPart;
        spec->tv_nsec = (long)((count.QuadPart % perf_freq.QuadPart) * 1000000000 / perf_freq.QuadPart);
    } else if (source == BENDER_TIME_CPUTIME) {
        FILETIME creationTime, exitTime, kernelTime, userTime;
        GetProcessTimes(GetCurrentProcess(), &creationTime, &exitTime, &kernelTime, &userTime);
        int64_t tmp = ((int64_t)userTime.dwHighDateTime << 32) + userTime.dwLowDateTime;
        spec->tv_sec = tmp / 10000000LL;
        spec->tv_nsec = tmp % 10000000LL * 100;
    }
    return 0;
}
/* clock_gettime() wasn't available on Mac until 10.12. */
#elif defined(BENDER_APPLE) && !defined(MAC_OS_X_VERSION_10_12)
#include <mach/clock.h>
#include <mach/mach.h>
int bender_gettime(struct timespec *spec, enum BenderTimeSource source) {
    if (source == BENDER_TIME_CPUTIME) {
        clock_t tmp = clock();
        spec->tv_sec = tmp / CLOCKS_PER_SEC;
        spec->tv_nsec = ((tmp - (spec->tv_sec * CLOCKS_PER_SEC)) * 1000000000) / CLOCKS_PER_SEC;
    } else {
        clock_serv_t cclock;
        mach_timespec_t mts;
        clock_id_t cid = CALENDAR_CLOCK;
        if (source == BENDER_TIME_REALTIME) {
            cid = CALENDAR_CLOCK;
        } else if (source == BENDER_TIME_MONOTONIC) {
            cid = SYSTEM_CLOCK;
        }
        host_get_clock_service(mach_host_self(), cid, &cclock);
        clock_get_time(cclock, &mts);
        mach_port_deallocate(mach_task_self(), cclock);
        spec->tv_sec = mts.tv_sec;
        spec->tv_nsec = mts.tv_nsec;
    }
    return 0;
}
#else
int bender_gettime(struct timespec *spec, enum BenderTimeSource source) {
    clockid_t cid = CLOCK_REALTIME;
    if (source == BENDER_TIME_REALTIME) {
        cid = CLOCK_REALTIME;
    } else if (source == BENDER_TIME_MONOTONIC) {
        cid = CLOCK_MONOTONIC;
    } else if (source == BENDER_TIME_CPUTIME) {
        cid = CLOCK_PROCESS_CPUTIME_ID;
    }
    return clock_gettime(cid, spec);
}
#endif
#endif

/* Better strerror (thread-safe if available) */
const char *bender_strerror(int e) {
#ifdef BENDER_WINDOWS
    /* Microsoft strerror seems sane here and is thread safe by default */
    return strerror(e);
#elif defined(__GLIBC__)
    /* See https://linux.die.net/man/3/strerror_r */
    return strerror_r(e, bender_vm.strerror_buf, sizeof(bender_vm.strerror_buf));
#else
    strerror_r(e, bender_vm.strerror_buf, sizeof(bender_vm.strerror_buf));
    return bender_vm.strerror_buf;
#endif
}

/* Setting C99 standard makes this not available, but it should
 * work/link properly if we detect a BSD */
#if defined(BENDER_BSD) || defined(MAC_OS_X_VERSION_10_7)
void arc4random_buf(void *buf, size_t nbytes);
#endif

int bender_cryptorand(uint8_t *out, size_t n) {
#ifndef BENDER_NO_CRYPTORAND
#ifdef BENDER_WINDOWS
    for (size_t i = 0; i < n; i += sizeof(unsigned int)) {
        unsigned int v;
        if (rand_s(&v))
            return -1;
        for (int32_t j = 0; (j < (int32_t) sizeof(unsigned int)) && (i + j < n); j++) {
            out[i + j] = v & 0xff;
            v = v >> 8;
        }
    }
    return 0;
#elif defined(BENDER_BSD) || defined(MAC_OS_X_VERSION_10_7)
    arc4random_buf(out, n);
    return 0;
#else
    /* We should be able to call getrandom on linux, but it doesn't seem
       to be uniformly supported on linux distros.
       On Mac, arc4random_buf wasn't available on until 10.7.
       In these cases, use this fallback path for now... */
    int rc;
    int randfd;
    RETRY_EINTR(randfd, open("/dev/urandom", O_RDONLY | O_CLOEXEC));
    if (randfd < 0)
        return -1;
    while (n > 0) {
        ssize_t nread;
        RETRY_EINTR(nread, read(randfd, out, n));
        if (nread <= 0) {
            RETRY_EINTR(rc, close(randfd));
            return -1;
        }
        out += nread;
        n -= nread;
    }
    RETRY_EINTR(rc, close(randfd));
    return 0;
#endif
#else
    (void) out;
    (void) n;
    return -1;
#endif
}

/* Dynamic library loading */

char *get_processed_name(const char *name) {
    if (name[0] == '.') return (char *) name;
    const char *c;
    for (c = name; *c; c++) {
        if (*c == '/') return (char *) name;
    }
    size_t l = (size_t)(c - name);
    char *ret = bender_malloc(l + 3);
    if (NULL == ret) {
        BENDER_OUT_OF_MEMORY;
    }
    ret[0] = '.';
    ret[1] = '/';
    memcpy(ret + 2, name, l + 1);
    return ret;
}

#if defined(BENDER_NO_DYNAMIC_MODULES)

const char *error_clib(void) {
    return "dynamic modules not supported";
}

#else
#if defined(BENDER_WINDOWS)

static char error_clib_buf[256];
char *error_clib(void) {
    FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                   NULL, GetLastError(), MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                   error_clib_buf, sizeof(error_clib_buf), NULL);
    error_clib_buf[strlen(error_clib_buf) - 1] = '\0';
    return error_clib_buf;
}

Clib load_clib(const char *name) {
    if (name == NULL) {
        return GetModuleHandle(NULL);
    } else {
        return LoadLibrary(name);
    }
}

void free_clib(HINSTANCE clib) {
    if (clib != GetModuleHandle(NULL)) {
        FreeLibrary(clib);
    }
}

void *symbol_clib(HINSTANCE clib, const char *sym) {
    if (clib != GetModuleHandle(NULL)) {
        return GetProcAddress(clib, sym);
    } else {
        /* Look up symbols from all loaded modules */
        HMODULE hMods[1024];
        DWORD needed = 0;
        if (EnumProcessModules(GetCurrentProcess(), hMods, sizeof(hMods), &needed)) {
            needed /= sizeof(HMODULE);
            for (DWORD i = 0; i < needed; i++) {
                void *address = GetProcAddress(hMods[i], sym);
                if (NULL != address) {
                    return address;
                }
            }
        } else {
            bender_panicf("ffi: %s", error_clib());
        }
        return NULL;
    }
}

#endif
#endif

/* Alloc function macro fills */
void *(bender_malloc)(size_t size) {
    return bender_malloc(size);
}

void (bender_free)(void *ptr) {
    bender_free(ptr);
}

void *(bender_calloc)(size_t nmemb, size_t size) {
    return bender_calloc(nmemb, size);
}

void *(bender_realloc)(void *ptr, size_t size) {
    return bender_realloc(ptr, size);
}

/* Avoid overflow and underflow, especially on 32-bit systems */
void *array_allocate(size_t element_size, int32_t count) {
    if (count < 0) return NULL;
    if ((size_t) count > (SIZE_MAX / element_size)) return NULL;
    return bender_malloc(element_size * count);
}
