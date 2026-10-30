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
#include "gc.h"
#endif

#ifdef BENDER_FFI

#ifdef _MSC_VER
#define alloca _alloca
#elif defined(BENDER_LINUX)
#include <alloca.h>
#elif !defined(alloca)
/* Last ditch effort to get alloca - works for gcc and clang */
#define alloca __builtin_alloca
#endif

#ifndef BENDER_WINDOWS
#include <dlfcn.h>
#endif

/* FFI jit includes */
#ifdef BENDER_FFI_JIT
#ifndef BENDER_WINDOWS
#include <sys/mman.h>
#endif
#endif

#define BENDER_FFI_MAX_RECUR 64

/* Compiler, OS, and arch detection. Used
 * to enable a set of calling conventions. The
 * :none calling convention is always enabled. */
#if defined(BENDER_WINDOWS) && (defined(__x86_64__) || defined(_M_X64))
#define BENDER_FFI_WIN64_ENABLED
#endif
#if (defined(__x86_64__) || defined(_M_X64)) && !defined(BENDER_WINDOWS)
#define BENDER_FFI_SYSV64_ENABLED
#endif
#if (defined(__aarch64__) || defined(_M_ARM64)) && !defined(BENDER_WINDOWS)
#define BENDER_FFI_AAPCS64_ENABLED
#endif

typedef struct BenderFFIType BenderFFIType;
typedef struct BenderFFIStruct BenderFFIStruct;

typedef enum {
    BENDER_FFI_TYPE_VOID,
    BENDER_FFI_TYPE_BOOL,
    BENDER_FFI_TYPE_PTR,
    BENDER_FFI_TYPE_STRING,
    BENDER_FFI_TYPE_FLOAT,
    BENDER_FFI_TYPE_DOUBLE,
    BENDER_FFI_TYPE_INT8,
    BENDER_FFI_TYPE_UINT8,
    BENDER_FFI_TYPE_INT16,
    BENDER_FFI_TYPE_UINT16,
    BENDER_FFI_TYPE_INT32,
    BENDER_FFI_TYPE_UINT32,
    BENDER_FFI_TYPE_INT64,
    BENDER_FFI_TYPE_UINT64,
    BENDER_FFI_TYPE_STRUCT
} BenderFFIPrimType;

/* Custom alignof since alignof not in c99 standard */
#define ALIGNOF(type) offsetof(struct { char c; type member; }, member)

typedef struct {
    size_t size;
    size_t align;
} BenderFFIPrimInfo;

static const BenderFFIPrimInfo bender_ffi_type_info[] = {
    {0, 0}, /* BENDER_FFI_TYPE_VOID */
    {sizeof(char), ALIGNOF(char)}, /* BENDER_FFI_TYPE_BOOL */
    {sizeof(void *), ALIGNOF(void *)}, /* BENDER_FFI_TYPE_PTR */
    {sizeof(char *), ALIGNOF(char *)}, /* BENDER_FFI_TYPE_STRING */
    {sizeof(float), ALIGNOF(float)}, /* BENDER_FFI_TYPE_FLOAT */
    {sizeof(double), ALIGNOF(double)}, /* BENDER_FFI_TYPE_DOUBLE */
    {sizeof(int8_t), ALIGNOF(int8_t)}, /* BENDER_FFI_TYPE_INT8 */
    {sizeof(uint8_t), ALIGNOF(uint8_t)}, /* BENDER_FFI_TYPE_UINT8 */
    {sizeof(int16_t), ALIGNOF(int16_t)}, /* BENDER_FFI_TYPE_INT16 */
    {sizeof(uint16_t), ALIGNOF(uint16_t)}, /* BENDER_FFI_TYPE_UINT16 */
    {sizeof(int32_t), ALIGNOF(int32_t)}, /* BENDER_FFI_TYPE_INT32 */
    {sizeof(uint32_t), ALIGNOF(uint32_t)}, /* BENDER_FFI_TYPE_UINT32 */
    {sizeof(int64_t), ALIGNOF(int64_t)}, /* BENDER_FFI_TYPE_INT64 */
    {sizeof(uint64_t), ALIGNOF(uint64_t)}, /* BENDER_FFI_TYPE_UINT64 */
    {0, ALIGNOF(uint64_t)} /* BENDER_FFI_TYPE_STRUCT */
};

struct BenderFFIType {
    BenderFFIStruct *st;
    BenderFFIPrimType prim;
    int32_t array_count;
};

typedef struct {
    BenderFFIType type;
    size_t offset;
} BenderFFIStructMember;

/* Also used to store array types */
struct BenderFFIStruct {
    uint32_t size;
    uint32_t align;
    uint32_t field_count;
    uint32_t is_aligned;
    BenderFFIStructMember fields[];
};

/* Specifies how the registers are classified. This is used
 * to determine if a certain argument should be passed in a register,
 * on the stack, special floating pointer register, etc. */
typedef enum {
    BENDER_SYSV64_INTEGER,
    BENDER_SYSV64_SSE,
    BENDER_SYSV64_SSEUP,
    BENDER_SYSV64_PAIR_INTINT,
    BENDER_SYSV64_PAIR_INTSSE,
    BENDER_SYSV64_PAIR_SSEINT,
    BENDER_SYSV64_PAIR_SSESSE,
    BENDER_SYSV64_NO_CLASS,
    BENDER_SYSV64_MEMORY,
    BENDER_WIN64_REGISTER,
    BENDER_WIN64_STACK,
    BENDER_WIN64_REGISTER_REF,
    BENDER_WIN64_STACK_REF,
    BENDER_AAPCS64_GENERAL,
    BENDER_AAPCS64_SSE,
    BENDER_AAPCS64_GENERAL_REF,
    BENDER_AAPCS64_STACK,
    BENDER_AAPCS64_STACK_REF,
    BENDER_AAPCS64_NONE
} BenderFFIWordSpec;

/* Describe how each Bender argument is interpreted in terms of machine words
 * that will be mapped to registers/stack. */
typedef struct {
    BenderFFIType type;
    BenderFFIWordSpec spec;
    uint32_t offset; /* point to the exact register / stack offset depending on spec. */
    uint32_t offset2; /* for reference passing apis (windows), use to allocate reference */
} BenderFFIMapping;

typedef enum {
    BENDER_FFI_CC_NONE,
    BENDER_FFI_CC_SYSV_64,
    BENDER_FFI_CC_WIN_64,
    BENDER_FFI_CC_AAPCS64
} BenderFFICallingConvention;

#ifdef BENDER_FFI_WIN64_ENABLED
#define BENDER_FFI_CC_DEFAULT BENDER_FFI_CC_WIN_64
#elif defined(BENDER_FFI_SYSV64_ENABLED)
#define BENDER_FFI_CC_DEFAULT BENDER_FFI_CC_SYSV_64
#elif defined(BENDER_FFI_AAPCS64_ENABLED)
#define BENDER_FFI_CC_DEFAULT BENDER_FFI_CC_AAPCS64
#else
#define BENDER_FFI_CC_DEFAULT BENDER_FFI_CC_NONE
#endif

#define BENDER_FFI_MAX_ARGS 32

typedef struct {
    uint32_t frame_size;
    uint32_t arg_count;
    uint32_t word_count;
    uint32_t variant;
    uint32_t stack_count;
    BenderFFICallingConvention cc;
    BenderFFIMapping ret;
    BenderFFIMapping args[BENDER_FFI_MAX_ARGS];
} BenderFFISignature;

int signature_mark(void *p, size_t s) {
    (void) s;
    BenderFFISignature *sig = p;
    for (uint32_t i = 0; i < sig->arg_count; i++) {
        BenderFFIType t = sig->args[i].type;
        if (t.prim == BENDER_FFI_TYPE_STRUCT) {
            bender_mark(bender_wrap_abstract(t.st));
        }
    }
    return 0;
}

static const BenderAbstractType bender_signature_type = {
    "core/ffi-signature",
    NULL,
    signature_mark,
    BENDER_ATEND_GCMARK
};

int struct_mark(void *p, size_t s) {
    (void) s;
    BenderFFIStruct *st = p;
    for (uint32_t i = 0; i < st->field_count; i++) {
        BenderFFIType t = st->fields[i].type;
        if (t.prim == BENDER_FFI_TYPE_STRUCT) {
            bender_mark(bender_wrap_abstract(t.st));
        }
    }
    return 0;
}

typedef struct {
    void *function_pointer;
    size_t size;
} BenderFFIJittedFn;

static void bender_ffi_struct_marshal(void *p, BenderMarshalContext *ctx);
static void *bender_ffi_struct_unmarshal(BenderMarshalContext *ctx);

static const BenderAbstractType bender_struct_type = {
    "core/ffi-struct",
    NULL,
    struct_mark,
    NULL, /* get */
    NULL, /* put */
    bender_ffi_struct_marshal,
    bender_ffi_struct_unmarshal,
    BENDER_ATEND_TOSTRING
};

static int bender_ffijit_gc(void *p, size_t s) {
    (void) s;
    BenderFFIJittedFn *fn = p;
    if (fn->function_pointer == NULL) return 0;
#ifdef BENDER_FFI_JIT
#ifdef BENDER_WINDOWS
    VirtualFree(fn->function_pointer, fn->size, MEM_RELEASE);
#else
    munmap(fn->function_pointer, fn->size);
#endif
#endif
    return 0;
}

static BenderByteView bender_ffijit_getbytes(void *p, size_t s) {
    (void) s;
    BenderFFIJittedFn *fn = p;
    BenderByteView bytes;
    bytes.bytes = fn->function_pointer;
    bytes.len = (int32_t) fn->size;
    return bytes;
}

static size_t bender_ffijit_length(void *p, size_t s) {
    (void) s;
    BenderFFIJittedFn *fn = p;
    return fn->size;
}

const BenderAbstractType bender_type_ffijit = {
    .name = "unsafe/jitfn",
    .gc = bender_ffijit_gc,
    .bytes = bender_ffijit_getbytes,
    .length = bender_ffijit_length
};

typedef struct {
    Clib clib;
    int closed;
    int is_self;
} BenderAbstractNative;

static const BenderAbstractType bender_native_type = {
    "core/ffi-native",
    BENDER_ATEND_NAME
};

static BenderFFIType prim_type(BenderFFIPrimType pt) {
    BenderFFIType t;
    t.prim = pt;
    t.st = NULL;
    t.array_count = -1;
    return t;
}

static size_t type_size(BenderFFIType t) {
    size_t count = t.array_count < 0 ? 1 : (size_t) t.array_count;
    if (t.prim == BENDER_FFI_TYPE_STRUCT) {
        return t.st->size * count;
    } else {
        return bender_ffi_type_info[t.prim].size * count;
    }
}

static size_t type_align(BenderFFIType t) {
    if (t.prim == BENDER_FFI_TYPE_STRUCT) {
        return t.st->align;
    } else {
        return bender_ffi_type_info[t.prim].align;
    }
}

static BenderFFICallingConvention decode_ffi_cc(const uint8_t *name) {
    if (!bender_cstrcmp(name, "none")) return BENDER_FFI_CC_NONE;
#ifdef BENDER_FFI_WIN64_ENABLED
    if (!bender_cstrcmp(name, "win64")) return BENDER_FFI_CC_WIN_64;
#endif
#ifdef BENDER_FFI_SYSV64_ENABLED
    if (!bender_cstrcmp(name, "sysv64")) return BENDER_FFI_CC_SYSV_64;
#endif
#ifdef BENDER_FFI_AAPCS64_ENABLED
    if (!bender_cstrcmp(name, "aapcs64")) return BENDER_FFI_CC_AAPCS64;
#endif
    if (!bender_cstrcmp(name, "default")) return BENDER_FFI_CC_DEFAULT;
    bender_panicf("unknown calling convention %s", name);
}

static BenderFFIPrimType decode_ffi_prim(const uint8_t *name) {
    if (!bender_cstrcmp(name, "void")) return BENDER_FFI_TYPE_VOID;
    if (!bender_cstrcmp(name, "bool")) return BENDER_FFI_TYPE_BOOL;
    if (!bender_cstrcmp(name, "ptr")) return BENDER_FFI_TYPE_PTR;
    if (!bender_cstrcmp(name, "pointer")) return BENDER_FFI_TYPE_PTR;
    if (!bender_cstrcmp(name, "string")) return BENDER_FFI_TYPE_STRING;
    if (!bender_cstrcmp(name, "float")) return BENDER_FFI_TYPE_FLOAT;
    if (!bender_cstrcmp(name, "double")) return BENDER_FFI_TYPE_DOUBLE;
    if (!bender_cstrcmp(name, "int8")) return BENDER_FFI_TYPE_INT8;
    if (!bender_cstrcmp(name, "uint8")) return BENDER_FFI_TYPE_UINT8;
    if (!bender_cstrcmp(name, "int16")) return BENDER_FFI_TYPE_INT16;
    if (!bender_cstrcmp(name, "uint16")) return BENDER_FFI_TYPE_UINT16;
    if (!bender_cstrcmp(name, "int32")) return BENDER_FFI_TYPE_INT32;
    if (!bender_cstrcmp(name, "uint32")) return BENDER_FFI_TYPE_UINT32;
    if (!bender_cstrcmp(name, "int64")) return BENDER_FFI_TYPE_INT64;
    if (!bender_cstrcmp(name, "uint64")) return BENDER_FFI_TYPE_UINT64;
#ifdef BENDER_64
    if (!bender_cstrcmp(name, "size")) return BENDER_FFI_TYPE_UINT64;
    if (!bender_cstrcmp(name, "ssize")) return BENDER_FFI_TYPE_INT64;
#else
    if (!bender_cstrcmp(name, "size")) return BENDER_FFI_TYPE_UINT32;
    if (!bender_cstrcmp(name, "ssize")) return BENDER_FFI_TYPE_INT32;
#endif
    /* aliases */
    if (!bender_cstrcmp(name, "r32")) return BENDER_FFI_TYPE_FLOAT;
    if (!bender_cstrcmp(name, "r64")) return BENDER_FFI_TYPE_DOUBLE;
    if (!bender_cstrcmp(name, "s8")) return BENDER_FFI_TYPE_INT8;
    if (!bender_cstrcmp(name, "u8")) return BENDER_FFI_TYPE_UINT8;
    if (!bender_cstrcmp(name, "s16")) return BENDER_FFI_TYPE_INT16;
    if (!bender_cstrcmp(name, "u16")) return BENDER_FFI_TYPE_UINT16;
    if (!bender_cstrcmp(name, "s32")) return BENDER_FFI_TYPE_INT32;
    if (!bender_cstrcmp(name, "u32")) return BENDER_FFI_TYPE_UINT32;
    if (!bender_cstrcmp(name, "s64")) return BENDER_FFI_TYPE_INT64;
    if (!bender_cstrcmp(name, "u64")) return BENDER_FFI_TYPE_UINT64;
    if (!bender_cstrcmp(name, "char")) return BENDER_FFI_TYPE_INT8;
    if (!bender_cstrcmp(name, "short")) return BENDER_FFI_TYPE_INT16;
    if (!bender_cstrcmp(name, "int")) return BENDER_FFI_TYPE_INT32;
    if (!bender_cstrcmp(name, "long")) return BENDER_FFI_TYPE_INT64;
    if (!bender_cstrcmp(name, "byte")) return BENDER_FFI_TYPE_UINT8;
    if (!bender_cstrcmp(name, "uchar")) return BENDER_FFI_TYPE_UINT8;
    if (!bender_cstrcmp(name, "ushort")) return BENDER_FFI_TYPE_UINT16;
    if (!bender_cstrcmp(name, "uint")) return BENDER_FFI_TYPE_UINT32;
    if (!bender_cstrcmp(name, "ulong")) return BENDER_FFI_TYPE_UINT64;
    bender_panicf("unknown machine type %s", name);
}

/* A common callback function signature. To avoid runtime code generation, which is prohibited
 * on many platforms, often buggy (see libffi), and generally complicated, instead provide
 * a single (or small set of commonly used function signatures). All callbacks should
 * eventually call this. */
void bender_ffi_trampoline(void *ctx, void *userdata) {
    if (NULL == userdata) {
        /* Userdata not set. */
        bender_eprintf("no userdata found for bender callback");
        return;
    }
    Bender context = bender_wrap_pointer(ctx);
    BenderFunction *fun = userdata;
    bender_call(fun, 1, &context);
}

/* Marshal support for ffi-struct abstracts. AOT images bake struct type
 * constants (ffi/defbind signatures close over them); without this, any
 * program using struct-typed FFI bindings fails image compilation.
 * Encoding per struct: field count, align, size, is_aligned, then per
 * field (prim, array_count, offset), recursing into nested struct
 * descriptors. Layout values are restored verbatim (exact under :pack).
 * GC safety: every fresh shell gets all prim fields initialized before
 * any further allocation, so struct_mark never follows a garbage
 * pointer even if a collection runs mid-reconstruction. */
static void bender_ffi_struct_marshal_one(BenderMarshalContext *ctx, BenderFFIStruct *st) {
    bender_marshal_int(ctx, (int32_t) st->field_count);
    bender_marshal_int(ctx, (int32_t) st->align);
    bender_marshal_int(ctx, (int32_t) st->size);
    bender_marshal_int(ctx, (int32_t) st->is_aligned);
    for (uint32_t i = 0; i < st->field_count; i++) {
        BenderFFIType t = st->fields[i].type;
        bender_marshal_int(ctx, (int32_t) t.prim);
        bender_marshal_int(ctx, t.array_count);
        bender_marshal_int(ctx, (int32_t) st->fields[i].offset);
        if (t.prim == BENDER_FFI_TYPE_STRUCT) {
            bender_ffi_struct_marshal_one(ctx, t.st);
        }
    }
}

static void bender_ffi_struct_marshal(void *p, BenderMarshalContext *ctx) {
    bender_marshal_abstract(ctx, p);
    bender_ffi_struct_marshal_one(ctx, p);
}

static BenderFFIStruct *bender_ffi_struct_unmarshal_one(BenderMarshalContext *ctx, int is_top) {
    uint32_t nfields = (uint32_t) bender_unmarshal_int(ctx);
    uint32_t align = (uint32_t) bender_unmarshal_int(ctx);
    uint32_t size = (uint32_t) bender_unmarshal_int(ctx);
    uint32_t is_aligned = (uint32_t) bender_unmarshal_int(ctx);
    size_t memsize = sizeof(BenderFFIStruct) + nfields * sizeof(BenderFFIStructMember);
    BenderFFIStruct *st;
    Bender root = bender_wrap_nil();
    if (is_top) {
        /* Consumes the context's single abstract slot and registers
         * identity for the (acyclic) descriptor graph. */
        st = bender_unmarshal_abstract(ctx, memsize);
    } else {
        /* Nested descriptors bypass the single-use slot (equality, not
         * identity, is what matters for immutable descriptors); root
         * explicitly until attached to the parent. */
        st = bender_abstract(&bender_struct_type, memsize);
        root = bender_wrap_abstract(st);
        bender_gcroot(root);
    }
    for (uint32_t i = 0; i < nfields; i++) {
        st->fields[i].type.prim = BENDER_FFI_TYPE_VOID;
        st->fields[i].type.array_count = -1;
        st->fields[i].type.st = NULL;
        st->fields[i].offset = 0;
    }
    st->field_count = nfields;
    st->align = align;
    st->size = size;
    st->is_aligned = is_aligned;
    for (uint32_t i = 0; i < nfields; i++) {
        BenderFFIType t;
        t.prim = (BenderFFIPrimType) bender_unmarshal_int(ctx);
        t.array_count = bender_unmarshal_int(ctx);
        st->fields[i].offset = (size_t) bender_unmarshal_int(ctx);
        if (t.prim == BENDER_FFI_TYPE_STRUCT) {
            t.st = bender_ffi_struct_unmarshal_one(ctx, 0);
        } else {
            t.st = NULL;
        }
        st->fields[i].type = t;
    }
    if (!is_top) bender_gcunroot(root);
    return st;
}

static void *bender_ffi_struct_unmarshal(BenderMarshalContext *ctx) {
    return bender_ffi_struct_unmarshal_one(ctx, 1);
}

static BenderFFIType decode_ffi_type(Bender x);

static BenderFFIStruct *build_struct_type(int32_t argc, const Bender *argv) {
    /* Use :pack to indicate a single packed struct member and :pack-all
     * to pack the remaining members */
    int32_t member_count = argc;
    int all_packed = 0;
    for (int32_t i = 0; i < argc; i++) {
        if (bender_keyeq(argv[i], "pack")) {
            member_count--;
        } else if (bender_keyeq(argv[i], "pack-all")) {
            member_count--;
            all_packed = 1;
        }
    }

    BenderFFIStruct *st = bender_abstract(&bender_struct_type,
                                        sizeof(BenderFFIStruct) + argc * sizeof(BenderFFIStructMember));
    st->field_count = 0;
    st->size = 0;
    st->align = 1;
    if (argc == 0) {
        bender_panic("invalid empty struct");
    }
    uint32_t is_aligned = 1;
    int32_t i = 0;
    for (int32_t j = 0; j < argc; j++) {
        int pack_one = 0;
        if (bender_keyeq(argv[j], "pack") || bender_keyeq(argv[j], "pack-all")) {
            pack_one = 1;
            j++;
            if (j == argc) break;
        }
        st->fields[i].type = decode_ffi_type(argv[j]);
        size_t el_size = type_size(st->fields[i].type);
        size_t el_align = type_align(st->fields[i].type);
        if (el_align <= 0) bender_panicf("bad field type %V", argv[j]);
        if (all_packed || pack_one) {
            if (st->size % el_align != 0) is_aligned = 0;
            st->fields[i].offset = st->size;
            st->size += (uint32_t) el_size;
        } else {
            if (el_align > st->align) st->align = (uint32_t) el_align;
            st->fields[i].offset = (uint32_t)(((st->size + el_align - 1) / el_align) * el_align);
            st->size = (uint32_t)(el_size + st->fields[i].offset);
        }
        i++;
    }
    st->is_aligned = is_aligned;
    st->size += (st->align - 1);
    st->size /= st->align;
    st->size *= st->align;
    st->field_count = member_count;
    return st;
}

static BenderFFIType decode_ffi_type(Bender x) {
    if (bender_checktype(x, BENDER_KEYWORD)) {
        return prim_type(decode_ffi_prim(bender_unwrap_keyword(x)));
    }
    BenderFFIType ret;
    ret.array_count = -1;
    ret.prim = BENDER_FFI_TYPE_STRUCT;
    if (bender_checkabstract(x, &bender_struct_type)) {
        ret.st = bender_unwrap_abstract(x);
        return ret;
    }
    int32_t len;
    const Bender *els;
    if (bender_indexed_view(x, &els, &len)) {
        if (bender_checktype(x, BENDER_ARRAY)) {
            if (len != 2 && len != 1) bender_panicf("array type must be of form @[type count], got %v", x);
            ret = decode_ffi_type(els[0]);
            int32_t array_count = len == 1 ? 0 : bender_getnat(els, 1);
            ret.array_count = array_count;
        } else {
            ret.st = build_struct_type(len, els);
        }
        return ret;
    } else {
        bender_panicf("bad native type %v", x);
    }
}

BENDER_CORE_FN(cfun_ffi_struct, "(ffi/struct & ffi-types)",
              "Create a struct type definition from `ffi-types` that can be "
              "used to pass structs into native functions.") {
    bender_arity(argc, 1, -1);
    return bender_wrap_abstract(build_struct_type(argc, argv));
}

BENDER_CORE_FN(cfun_ffi_size,
              "(ffi/size ffi-type)",
              "Get the size of an FFI type `ffi-type` in bytes.") {
    bender_fixarity(argc, 1);
    size_t size = type_size(decode_ffi_type(argv[0]));
    return bender_wrap_number((double) size);
}

BENDER_CORE_FN(cfun_ffi_align,
              "(ffi/align ffi-type)",
              "Get the align of an FFI type `ffi-type` in bytes.") {
    bender_fixarity(argc, 1);
    size_t size = type_align(decode_ffi_type(argv[0]));
    return bender_wrap_number((double) size);
}

/* Canonical keyword spelling of an ffi prim. The mapping is many-to-one
 * (int/char/s16/... all decode to one prim), so the name is reconstructed
 * from the prim, never echoed back from user input. */
static const char *bender_ffi_prim_name(BenderFFIPrimType prim) {
    switch (prim) {
        default:
        case BENDER_FFI_TYPE_VOID: return "void";
        case BENDER_FFI_TYPE_BOOL: return "bool";
        case BENDER_FFI_TYPE_PTR: return "ptr";
        case BENDER_FFI_TYPE_STRING: return "string";
        case BENDER_FFI_TYPE_FLOAT: return "float";
        case BENDER_FFI_TYPE_DOUBLE: return "double";
        case BENDER_FFI_TYPE_INT8: return "int8";
        case BENDER_FFI_TYPE_UINT8: return "uint8";
        case BENDER_FFI_TYPE_INT16: return "int16";
        case BENDER_FFI_TYPE_UINT16: return "uint16";
        case BENDER_FFI_TYPE_INT32: return "int32";
        case BENDER_FFI_TYPE_UINT32: return "uint32";
        case BENDER_FFI_TYPE_INT64: return "int64";
        case BENDER_FFI_TYPE_UINT64: return "uint64";
        case BENDER_FFI_TYPE_STRUCT: return "struct";
    }
}

/* Structural description of one BenderFFIType as a plain Bender table.
 * `:struct` carries a nested struct abstract; `:prim` is the canonical
 * keyword spelling of the primitive (or nil for a struct). */
static Bender bender_ffi_type_desc(BenderFFIType t) {
    BenderTable *out = bender_table(3);
    bender_table_put(out, bender_ckeywordv("array-count"), bender_wrap_integer(t.array_count));
    if (t.prim == BENDER_FFI_TYPE_STRUCT) {
        bender_table_put(out, bender_ckeywordv("prim"), bender_wrap_nil());
        bender_table_put(out, bender_ckeywordv("struct"), bender_wrap_abstract(t.st));
    } else {
        bender_table_put(out, bender_ckeywordv("prim"), bender_ckeywordv(bender_ffi_prim_name(t.prim)));
        bender_table_put(out, bender_ckeywordv("struct"), bender_wrap_nil());
    }
    return bender_wrap_table(out);
}

BENDER_CORE_FN(cfun_ffi_struct_info,
              "(ffi/struct-info ffi-type)",
              "Describe a struct type created by `ffi/struct`: returns a table\n"
              "with `:size`, `:align`, `:is-aligned`, and `:fields` -- an array of\n"
              "tables with `:offset`, `:array-count`, `:prim` (canonical keyword, or\n"
              "nil for a nested struct), and `:struct` (nested struct abstract).\n"
              "Returns nil for any type that is not a struct type. This is a pure\n"
              "read of the layout the FFI marshaller itself uses.") {
    bender_fixarity(argc, 1);
    Bender x = argv[0];
    if (!bender_checkabstract(x, &bender_struct_type)) return bender_wrap_nil();
    BenderFFIStruct *st = bender_unwrap_abstract(x);
    BenderTable *out = bender_table(5);
    bender_table_put(out, bender_ckeywordv("size"), bender_wrap_integer((int32_t) st->size));
    bender_table_put(out, bender_ckeywordv("align"), bender_wrap_integer((int32_t) st->align));
    bender_table_put(out, bender_ckeywordv("is-aligned"), bender_wrap_boolean(st->is_aligned ? 1 : 0));
    BenderArray *fields = bender_array(st->field_count);
    for (uint32_t i = 0; i < st->field_count; i++) {
        BenderTable *f = bender_table(4);
        bender_table_put(f, bender_ckeywordv("offset"), bender_wrap_integer((int32_t) st->fields[i].offset));
        bender_table_put(f, bender_ckeywordv("type"), bender_ffi_type_desc(st->fields[i].type));
        bender_array_push(fields, bender_wrap_table(f));
    }
    bender_table_put(out, bender_ckeywordv("fields"), bender_wrap_array(fields));
    return bender_wrap_table(out);
}

static void *bender_ffi_getpointer(const Bender *argv, int32_t n) {
    switch (bender_type(argv[n])) {
        default:
            bender_panicf("bad slot #%d, expected ffi pointer convertible type, got %v", n, argv[n]);
        case BENDER_POINTER:
        case BENDER_STRING:
        case BENDER_KEYWORD:
        case BENDER_SYMBOL:
        case BENDER_CFUNCTION:
            return bender_unwrap_pointer(argv[n]);
        case BENDER_ABSTRACT:
            return (void *) bender_getbytes(argv, n).bytes;
        case BENDER_BUFFER:
            return bender_unwrap_buffer(argv[n])->data;
        case BENDER_FUNCTION:
            /* Users may pass in a function. Any function passed is almost certainly
             * being used as a callback, so we add it to the root set. */
            bender_gcroot(argv[n]);
            return bender_unwrap_pointer(argv[n]);
        case BENDER_NIL:
            return NULL;
    }
}

/* Coerce one boxed value to a raw pointer with exactly the rules the FFI
 * argument marshaller uses (bender_ffi_getpointer). Generated native alias
 * wrappers call this so :ptr arguments keep boxed-path semantics (and
 * boxed-path panics). */
BENDER_API void *bender_ffi_pointer_value(Bender x) {
    return bender_ffi_getpointer(&x, 0);
}

/* Native FFI alias tier support (see tools/callgraph.ben). An
 * ffi/defbind-alias binding is an ordinary defn whose body calls a
 * macro-expansion-created memoized lookup thunk: for a lazy ffi/context
 * that thunk runs `(ffi/lookup <library> <C symbol>)` on first use, which
 * keeps the library dlopen lazy. The thunk is a constant closure whose
 * captured environment is detached and rebuilt by the image marshaler,
 * so calling it later (from a generated wrapper, after the frame it was
 * written against is gone) is not sound: a stale captured slot surfaces
 * as a garbage function or value. Resolve the pointer exactly once, here,
 * while installing, and fail closed at every step. */

#define BENDER_FFI_ALIAS_OK 0
#define BENDER_FFI_ALIAS_BAD_SHAPE 1
#define BENDER_FFI_ALIAS_BAD_POINTER 2

/* Bytecode word layout shared with emit.c / vm.c. */
#define BENDER_ALIAS_INSTR(op, a, b, c) \
    ((uint32_t) (op) | ((uint32_t) (a) << 8) | ((uint32_t) (b) << 16) | ((uint32_t) (c) << 24))
#define BENDER_ALIAS_OP(w) ((int32_t) ((w) & 0xFF))
#define BENDER_ALIAS_A(w) ((int32_t) (((w) >> 8) & 0xFF))
#define BENDER_ALIAS_B(w) ((int32_t) (((w) >> 16) & 0xFF))
#define BENDER_ALIAS_C(w) ((int32_t) ((w) >> 24))
#define BENDER_ALIAS_E(w) ((int32_t) ((w) >> 16))

/* Rewrite an ffi/defbind-alias definition in place so that every call
 * site, including call sites whose constant pool already baked the boxed
 * function value (module-internal calls -- the common case), ends up
 * tail-calling a generated cfunction wrapper instead of running the
 * boxed body.
 *
 * The shape is re-verified here rather than trusted from the manifest:
 * the manifest is keyed by source name, and installing a wrapper on a
 * different function of the same name would call a C function with
 * unconverted arguments. Refusal is always safe: the boxed body runs.
 *
 * The lookup thunk is called under bender_pcall, so a thunk that errors
 * is a refusal instead of a panic at image init, and the resolved
 * pointer is checked with dladdr against `symbol`, so only the symbol
 * the generated wrapper was emitted for can ever be installed.
 *
 * Rewritten body (arity n; slot n is a scratch slot):
 *   push/push2/push3 0 1 .. n-1   ; the alias's own arguments
 *   ldc n 0                       ; the wrapper cfunction
 *   tcall n
 * This is byte-for-byte the shape the compiler emits for
 * `(g a b ...)` in tail position, so argument passing, error propagation
 * and fiber stack growth are the VM's ordinary paths.
 *
 * Returns BENDER_FFI_ALIAS_OK after rewriting and stores the validated
 * pointer in *out_ptr. Any other return leaves the definition untouched. */
BENDER_API int bender_ffi_alias_install(BenderFunction *f, int32_t thunk_index, Bender wrapper,
                                      const char *symbol, void **out_ptr) {
    if (NULL == f) return BENDER_FFI_ALIAS_BAD_SHAPE;
    BenderFuncDef *d = f->def;
    if (NULL == d) return BENDER_FFI_ALIAS_BAD_SHAPE;
    if (d->flags & BENDER_FUNCDEF_FLAG_VARARG) return BENDER_FFI_ALIAS_BAD_SHAPE;
    if (d->named_args_count != 0) return BENDER_FFI_ALIAS_BAD_SHAPE;
    if (!bender_checktype(wrapper, BENDER_CFUNCTION)) return BENDER_FFI_ALIAS_BAD_SHAPE;
    int32_t arity = d->arity;
    if (arity < 0 || arity > 200) return BENDER_FFI_ALIAS_BAD_SHAPE;
    if (d->slotcount < arity + 1) return BENDER_FFI_ALIAS_BAD_SHAPE;
    if ((thunk_index < 0) || (thunk_index >= d->constants_length)) return BENDER_FFI_ALIAS_BAD_SHAPE;
    uint32_t *bc = d->bytecode;
    int32_t len = d->bytecode_length;
    if (NULL == bc || len < 6) return BENDER_FFI_ALIAS_BAD_SHAPE;
    Bender *consts = d->constants;
    /* ldc <slot> <const>; call <dst> <slot> -- twice (pointer, signature) */
    if (BENDER_ALIAS_OP(bc[0]) != JOP_LOAD_CONSTANT) return BENDER_FFI_ALIAS_BAD_SHAPE;
    if (BENDER_ALIAS_OP(bc[1]) != JOP_CALL) return BENDER_FFI_ALIAS_BAD_SHAPE;
    if (BENDER_ALIAS_OP(bc[2]) != JOP_LOAD_CONSTANT) return BENDER_FFI_ALIAS_BAD_SHAPE;
    if (BENDER_ALIAS_OP(bc[3]) != JOP_CALL) return BENDER_FFI_ALIAS_BAD_SHAPE;
    int32_t ptr_slot = BENDER_ALIAS_A(bc[0]);
    int32_t ptr_const = BENDER_ALIAS_E(bc[0]);
    int32_t ptr_dst = BENDER_ALIAS_A(bc[1]);
    int32_t ptr_callee = BENDER_ALIAS_E(bc[1]);
    int32_t sig_slot = BENDER_ALIAS_A(bc[2]);
    int32_t sig_const = BENDER_ALIAS_E(bc[2]);
    int32_t sig_dst = BENDER_ALIAS_A(bc[3]);
    int32_t sig_callee = BENDER_ALIAS_E(bc[3]);
    if (ptr_callee != ptr_slot || sig_callee != sig_slot) return BENDER_FFI_ALIAS_BAD_SHAPE;
    if ((ptr_const < 0) || (ptr_const >= d->constants_length)) return BENDER_FFI_ALIAS_BAD_SHAPE;
    if ((sig_const < 0) || (sig_const >= d->constants_length)) return BENDER_FFI_ALIAS_BAD_SHAPE;
    if (!bender_checktype(consts[ptr_const], BENDER_FUNCTION)) return BENDER_FFI_ALIAS_BAD_SHAPE;
    if (!bender_checktype(consts[sig_const], BENDER_FUNCTION)) return BENDER_FFI_ALIAS_BAD_SHAPE;
    if (thunk_index != ptr_const) return BENDER_FFI_ALIAS_BAD_SHAPE;
    /* pushes: exactly the pointer, the signature, then the arguments */
    int32_t i = 4;
    int32_t pushed = 0;
    int32_t want[258];
    want[pushed++] = ptr_dst;
    want[pushed++] = sig_dst;
    for (int32_t k = 0; k < arity; k++) want[pushed++] = k;
    int32_t seen = 0;
    while ((i < len) && (BENDER_ALIAS_OP(bc[i]) != JOP_LOAD_CONSTANT)) {
        int32_t op = BENDER_ALIAS_OP(bc[i]);
        int32_t cnt = (op == JOP_PUSH) ? 1 : (op == JOP_PUSH_2) ? 2 : (op == JOP_PUSH_3) ? 3 : 0;
        if (cnt == 0) return BENDER_FFI_ALIAS_BAD_SHAPE;
        int32_t vals[3];
        vals[0] = BENDER_ALIAS_A(bc[i]);
        vals[1] = (op == JOP_PUSH_3) ? BENDER_ALIAS_B(bc[i]) : BENDER_ALIAS_E(bc[i]);
        vals[2] = BENDER_ALIAS_C(bc[i]);
        for (int32_t k = 0; k < cnt; k++) {
            if (seen >= pushed || want[seen] != vals[k]) return BENDER_FFI_ALIAS_BAD_SHAPE;
            seen++;
        }
        i++;
    }
    if (seen != pushed) return BENDER_FFI_ALIAS_BAD_SHAPE;
    /* ldc <slot> <cfunction>; tcall <slot> */
    if (i + 1 != len - 1) return BENDER_FFI_ALIAS_BAD_SHAPE;
    if (BENDER_ALIAS_OP(bc[i]) != JOP_LOAD_CONSTANT) return BENDER_FFI_ALIAS_BAD_SHAPE;
    if (BENDER_ALIAS_OP(bc[i + 1]) != JOP_TAILCALL) return BENDER_FFI_ALIAS_BAD_SHAPE;
    int32_t cfun_slot = BENDER_ALIAS_A(bc[i]);
    int32_t cfun_const = BENDER_ALIAS_E(bc[i]);
    if (BENDER_ALIAS_A(bc[i + 1]) != cfun_slot) return BENDER_FFI_ALIAS_BAD_SHAPE;
    if ((cfun_const < 0) || (cfun_const >= d->constants_length)) return BENDER_FFI_ALIAS_BAD_SHAPE;
    if (!bender_checktype(consts[cfun_const], BENDER_CFUNCTION)) return BENDER_FFI_ALIAS_BAD_SHAPE;
    /* rewrite (never grows: push groups + 2 <= the boxed body's length) */
    int32_t nops = ((arity + 2) / 3) + 2;
    if (nops > len) return BENDER_FFI_ALIAS_BAD_SHAPE;
    /* Resolve the pointer before rewriting so a refusal leaves the boxed
     * body exactly as it was. bender_pcall turns a thunk error (a stale
     * captured slot can make the thunk raise) into a refusal. */
    Bender thunkv = consts[thunk_index];
    if (!bender_checktype(thunkv, BENDER_FUNCTION)) return BENDER_FFI_ALIAS_BAD_POINTER;
    Bender resolved;
    BenderFiber *resume = NULL;
    if (BENDER_SIGNAL_OK != bender_pcall(bender_unwrap_function(thunkv), 0, NULL, &resolved, &resume))
        return BENDER_FFI_ALIAS_BAD_POINTER;
    if (!bender_checktype(resolved, BENDER_POINTER)) return BENDER_FFI_ALIAS_BAD_POINTER;
    void *ptr = bender_unwrap_pointer(resolved);
    if (NULL == ptr || NULL == symbol) return BENDER_FFI_ALIAS_BAD_POINTER;
#ifndef BENDER_WINDOWS
    /* A stale captured slot can yield a pointer-typed value; dladdr
     * proves the address really is an exported definition of `symbol`. */
    Dl_info info;
    if (0 == dladdr(ptr, &info) || NULL == info.dli_sname ||
            0 != strcmp(info.dli_sname, symbol)) return BENDER_FFI_ALIAS_BAD_POINTER;
#endif
    /* Any compiled native code or validation cache for this def describes
     * the boxed body, which no longer exists. Drop both; the lazy-attach
     * generation is left alone so the next call does not re-match (the
     * shim cannot match a descriptor anyway). */
    d->native_code = NULL;
    d->native_verified = NULL;
    d->native_verified_epoch = 0;
    int32_t w = 0;
    int32_t off = 0;
    while (off < arity) {
        int32_t n = arity - off;
        if (n > 3) n = 3;
        if (n == 3) {
            bc[w++] = BENDER_ALIAS_INSTR(JOP_PUSH_3, off, off + 1, off + 2);
        } else if (n == 2) {
            bc[w++] = BENDER_ALIAS_INSTR(JOP_PUSH_2, off, off + 1, 0);
        } else {
            bc[w++] = BENDER_ALIAS_INSTR(JOP_PUSH, off, 0, 0);
        }
        off += n;
    }
    bc[w++] = BENDER_ALIAS_INSTR(JOP_LOAD_CONSTANT, arity, 0, 0);
    bc[w++] = BENDER_ALIAS_INSTR(JOP_TAILCALL, arity, 0, 0);
    d->bytecode_length = w;
    /* The shim uses one scratch slot above the argument slots, and
     * slotcount bounds every slot the verifier (and the frame) may touch.
     * Compiled defbind-alias bodies already reserve temporaries, so this
     * is a no-op for the shape this function accepts; it keeps the shim
     * in bounds if a def with slotcount == arity ever reaches here. */
    if (d->slotcount < arity + 1) d->slotcount = arity + 1;
    d->constants[0] = wrapper;
    d->constants_length = 1;
    d->defs_length = 0;
    d->environments_length = 0;
    /* Runtime-only marker for the JOP_CALL fast path: the body above is
     * exactly `push args; ldc wrapper; tcall`, so the VM may invoke the
     * wrapper cfunction directly instead of entering this frame. */
    d->flags |= BENDER_FUNCDEF_FLAG_FFI_ALIAS;
    *out_ptr = ptr;
    return BENDER_FFI_ALIAS_OK;
}

static void *bender_ffi_get_callable_pointer(const Bender *argv, int32_t n) {
    switch (bender_type(argv[n])) {
        default:
            break;
        case BENDER_POINTER:
            return bender_unwrap_pointer(argv[n]);
        case BENDER_ABSTRACT:
            if (!bender_checkabstract(argv[n], &bender_type_ffijit)) break;
            return ((BenderFFIJittedFn *)bender_unwrap_abstract(argv[n]))->function_pointer;
    }
    bender_panicf("bad slot #%d, expected ffi callable pointer type, got %v", n, argv[n]);
}

/* Write a value given by some Bender values and an FFI type as it would appear in memory.
 * The alignment and space available is assumed to already be sufficient */
static void bender_ffi_write_one(void *to, const Bender *argv, int32_t n, BenderFFIType type, int recur) {
    if (recur == 0) bender_panic("recursion too deep");
    if (type.array_count >= 0) {
        BenderFFIType el_type = type;
        el_type.array_count = -1;
        size_t el_size = type_size(el_type);
        BenderView els = bender_getindexed(argv, n);
        if (els.len != type.array_count) {
            bender_panicf("bad array length, expected %d, got %d", type.array_count, els.len);
        }
        char *cursor = to;
        for (int32_t i = 0; i < els.len; i++) {
            bender_ffi_write_one(cursor, els.items, i, el_type, recur - 1);
            cursor += el_size;
        }
        return;
    }
    switch (type.prim) {
        case BENDER_FFI_TYPE_VOID:
            if (!bender_checktype(argv[n], BENDER_NIL)) {
                bender_panicf("expected nil, got %v", argv[n]);
            }
            break;
        case BENDER_FFI_TYPE_STRUCT: {
            BenderView els = bender_getindexed(argv, n);
            BenderFFIStruct *st = type.st;
            if ((uint32_t) els.len != st->field_count) {
                bender_panicf("wrong number of fields in struct, expected %d, got %d",
                             (int32_t) st->field_count, els.len);
            }
            for (int32_t i = 0; i < els.len; i++) {
                BenderFFIType tp = st->fields[i].type;
                bender_ffi_write_one((char *) to + st->fields[i].offset, els.items, i, tp, recur - 1);
            }
        }
        break;
        case BENDER_FFI_TYPE_DOUBLE:
            ((double *)(to))[0] = bender_getnumber(argv, n);
            break;
        case BENDER_FFI_TYPE_FLOAT:
            ((float *)(to))[0] = (float) bender_getnumber(argv, n);
            break;
        case BENDER_FFI_TYPE_PTR:
            ((void **)(to))[0] = bender_ffi_getpointer(argv, n);
            break;
        case BENDER_FFI_TYPE_STRING:
            ((const char **)(to))[0] = bender_getcstring(argv, n);
            break;
        case BENDER_FFI_TYPE_BOOL:
            ((bool *)(to))[0] = bender_getboolean(argv, n);
            break;
        case BENDER_FFI_TYPE_INT8:
            ((int8_t *)(to))[0] = bender_getinteger(argv, n);
            break;
        case BENDER_FFI_TYPE_INT16:
            ((int16_t *)(to))[0] = bender_getinteger(argv, n);
            break;
        case BENDER_FFI_TYPE_INT32:
            ((int32_t *)(to))[0] = bender_getinteger(argv, n);
            break;
        case BENDER_FFI_TYPE_INT64:
            ((int64_t *)(to))[0] = bender_getinteger64(argv, n);
            break;
        case BENDER_FFI_TYPE_UINT8:
            ((uint8_t *)(to))[0] = (uint8_t) bender_getuinteger64(argv, n);
            break;
        case BENDER_FFI_TYPE_UINT16:
            ((uint16_t *)(to))[0] = (uint16_t) bender_getuinteger64(argv, n);
            break;
        case BENDER_FFI_TYPE_UINT32:
            ((uint32_t *)(to))[0] = (uint32_t) bender_getuinteger64(argv, n);
            break;
        case BENDER_FFI_TYPE_UINT64:
            ((uint64_t *)(to))[0] = bender_getuinteger64(argv, n);
            break;
    }
}

/* Read a value from memory and construct a Bender data structure that can be passed back into
 * the interpreter. This should be the inverse to bender_ffi_write_one. It is assumed that the
 * size of the data is correct. */
static Bender bender_ffi_read_one(const uint8_t *from, BenderFFIType type, int recur) {
    if (recur == 0) bender_panic("recursion too deep");
    if (type.array_count >= 0) {
        BenderFFIType el_type = type;
        el_type.array_count = -1;
        size_t el_size = type_size(el_type);
        BenderArray *array = bender_array(type.array_count);
        for (int32_t i = 0; i < type.array_count; i++) {
            bender_array_push(array, bender_ffi_read_one(from, el_type, recur - 1));
            from += el_size;
        }
        return bender_wrap_array(array);
    }
    switch (type.prim) {
        default:
        case BENDER_FFI_TYPE_VOID:
            return bender_wrap_nil();
        case BENDER_FFI_TYPE_STRUCT: {
            BenderFFIStruct *st = type.st;
            Bender *tup = bender_tuple_begin(st->field_count);
            for (uint32_t i = 0; i < st->field_count; i++) {
                BenderFFIType tp = st->fields[i].type;
                tup[i] = bender_ffi_read_one(from + st->fields[i].offset, tp, recur - 1);
            }
            return bender_wrap_tuple(bender_tuple_end(tup));
        }
        case BENDER_FFI_TYPE_DOUBLE:
            return bender_wrap_number(((double *)(from))[0]);
        case BENDER_FFI_TYPE_FLOAT:
            return bender_wrap_number(((float *)(from))[0]);
        case BENDER_FFI_TYPE_PTR: {
            void *ptr = ((void **)(from))[0];
            return (NULL == ptr) ? bender_wrap_nil() : bender_wrap_pointer(ptr);
        }
        case BENDER_FFI_TYPE_STRING:
            return bender_cstringv(((char **)(from))[0]);
        case BENDER_FFI_TYPE_BOOL:
            return bender_wrap_boolean(((bool *)(from))[0]);
        case BENDER_FFI_TYPE_INT8:
            return bender_wrap_number(((int8_t *)(from))[0]);
        case BENDER_FFI_TYPE_INT16:
            return bender_wrap_number(((int16_t *)(from))[0]);
        case BENDER_FFI_TYPE_INT32:
            return bender_wrap_number(((int32_t *)(from))[0]);
        case BENDER_FFI_TYPE_UINT8:
            return bender_wrap_number(((uint8_t *)(from))[0]);
        case BENDER_FFI_TYPE_UINT16:
            return bender_wrap_number(((uint16_t *)(from))[0]);
        case BENDER_FFI_TYPE_UINT32:
            return bender_wrap_number(((uint32_t *)(from))[0]);
#ifdef BENDER_INT_TYPES
        case BENDER_FFI_TYPE_INT64:
            return bender_wrap_s64(((int64_t *)(from))[0]);
        case BENDER_FFI_TYPE_UINT64:
            return bender_wrap_u64(((uint64_t *)(from))[0]);
#else
        case BENDER_FFI_TYPE_INT64:
            return bender_wrap_number(((int64_t *)(from))[0]);
        case BENDER_FFI_TYPE_UINT64:
            return bender_wrap_number(((uint64_t *)(from))[0]);
#endif
    }
}

static BenderFFIMapping void_mapping(void) {
    BenderFFIMapping m;
    m.type = prim_type(BENDER_FFI_TYPE_VOID);
    m.spec = BENDER_SYSV64_NO_CLASS;
    m.offset = 0;
    return m;
}

#ifdef BENDER_FFI_SYSV64_ENABLED
/* AMD64 ABI Draft 0.99.7 – November 17, 2014 – 15:08
 * See section 3.2.3 Parameter Passing */
static BenderFFIWordSpec sysv64_classify_ext(BenderFFIType type, size_t shift) {
    switch (type.prim) {
        case BENDER_FFI_TYPE_PTR:
        case BENDER_FFI_TYPE_STRING:
        case BENDER_FFI_TYPE_BOOL:
        case BENDER_FFI_TYPE_INT8:
        case BENDER_FFI_TYPE_INT16:
        case BENDER_FFI_TYPE_INT32:
        case BENDER_FFI_TYPE_INT64:
        case BENDER_FFI_TYPE_UINT8:
        case BENDER_FFI_TYPE_UINT16:
        case BENDER_FFI_TYPE_UINT32:
        case BENDER_FFI_TYPE_UINT64:
            return BENDER_SYSV64_INTEGER;
        case BENDER_FFI_TYPE_DOUBLE:
        case BENDER_FFI_TYPE_FLOAT:
            return BENDER_SYSV64_SSE;
        case BENDER_FFI_TYPE_STRUCT: {
            BenderFFIStruct *st = type.st;
            if (st->size > 16) return BENDER_SYSV64_MEMORY;
            if (!st->is_aligned) return BENDER_SYSV64_MEMORY;
            BenderFFIWordSpec clazz = BENDER_SYSV64_NO_CLASS;
            if (st->size > 8 && st->size <= 16) {
                /* map to pair classification */
                int has_int_lo = 0;
                int has_int_hi = 0;
                for (uint32_t i = 0; i < st->field_count; i++) {
                    BenderFFIWordSpec next_class = sysv64_classify_ext(st->fields[i].type, shift + st->fields[i].offset);
                    switch (next_class) {
                        default:
                            break;
                        case BENDER_SYSV64_INTEGER:
                            if (shift + st->fields[i].offset + type_size(st->fields[i].type) <= 8) {
                                has_int_lo = 1;
                            } else {
                                has_int_hi = 2;
                            }
                            break;
                        case BENDER_SYSV64_PAIR_INTINT:
                            has_int_lo = 1;
                            has_int_hi = 2;
                            break;
                        case BENDER_SYSV64_PAIR_INTSSE:
                            has_int_lo = 1;
                            break;
                        case BENDER_SYSV64_PAIR_SSEINT:
                            has_int_hi = 2;
                            break;
                            break;
                    }
                }
                switch (has_int_hi + has_int_lo) {
                    case 0:
                        clazz = BENDER_SYSV64_PAIR_SSESSE;
                        break;
                    case 1:
                        clazz = BENDER_SYSV64_PAIR_INTSSE;
                        break;
                    case 2:
                        clazz = BENDER_SYSV64_PAIR_SSEINT;
                        break;
                    case 3:
                        clazz = BENDER_SYSV64_PAIR_INTINT;
                        break;
                }
            } else {
                /* Normal struct classification */
                for (uint32_t i = 0; i < st->field_count; i++) {
                    BenderFFIWordSpec next_class = sysv64_classify_ext(st->fields[i].type, shift + st->fields[i].offset);
                    if (next_class != clazz) {
                        if (clazz == BENDER_SYSV64_NO_CLASS) {
                            clazz = next_class;
                        } else if (clazz == BENDER_SYSV64_MEMORY || next_class == BENDER_SYSV64_MEMORY) {
                            clazz = BENDER_SYSV64_MEMORY;
                        } else if (clazz == BENDER_SYSV64_INTEGER || next_class == BENDER_SYSV64_INTEGER) {
                            clazz = BENDER_SYSV64_INTEGER;
                        } else {
                            clazz = BENDER_SYSV64_SSE;
                        }
                    }
                }
            }
            return clazz;
        }
        case BENDER_FFI_TYPE_VOID:
            return BENDER_SYSV64_NO_CLASS;
        default:
            bender_panic("nyi");
            return BENDER_SYSV64_NO_CLASS;
    }
}
static BenderFFIWordSpec sysv64_classify(BenderFFIType type) {
    return sysv64_classify_ext(type, 0);
}
#endif

#ifdef BENDER_FFI_AAPCS64_ENABLED
/* Procedure Call Standard for the Arm® 64-bit Architecture (AArch64) 2023Q3 – October 6, 2023
 * See section 6.8.2 Parameter passing rules.
 * https://github.com/ARM-software/abi-aa/releases/download/2023Q3/aapcs64.pdf
 *
 * Additional documentation needed for Apple platforms.
 * https://developer.apple.com/documentation/xcode/writing-arm64-code-for-apple-platforms */

#define BENDER_FFI_AAPCS64_FORCE_STACK_ALIGN(ptr, alignment) (ptr = ((ptr) + ((alignment) - 1)) & ~((alignment) - 1))
#if !defined(BENDER_APPLE)
#define BENDER_FFI_AAPCS64_STACK_ALIGN(ptr, alignment) ((void) alignment, BENDER_FFI_AAPCS64_FORCE_STACK_ALIGN(ptr, 8))
#else
#define BENDER_FFI_AAPCS64_STACK_ALIGN(ptr, alignment) BENDER_FFI_AAPCS64_FORCE_STACK_ALIGN(ptr, alignment)
#endif

typedef struct {
    uint64_t a;
    uint64_t b;
} Aapcs64Variant1ReturnGeneral;

typedef struct {
    double a;
    double b;
    double c;
    double d;
} Aapcs64Variant2ReturnSse;

/* Workaround for passing a return value pointer through x8.
 * Limits struct returns to 128 bytes. */
typedef struct {
    uint64_t a;
    uint64_t b;
    uint64_t c;
    uint64_t d;
    uint64_t e;
    uint64_t f;
    uint64_t g;
    uint64_t h;
    uint64_t i;
    uint64_t j;
    uint64_t k;
    uint64_t l;
    uint64_t m;
    uint64_t n;
    uint64_t o;
    uint64_t p;
} Aapcs64Variant3ReturnPointer;

static BenderFFIWordSpec aapcs64_classify(BenderFFIType type) {
    switch (type.prim) {
        case BENDER_FFI_TYPE_PTR:
        case BENDER_FFI_TYPE_STRING:
        case BENDER_FFI_TYPE_BOOL:
        case BENDER_FFI_TYPE_INT8:
        case BENDER_FFI_TYPE_INT16:
        case BENDER_FFI_TYPE_INT32:
        case BENDER_FFI_TYPE_INT64:
        case BENDER_FFI_TYPE_UINT8:
        case BENDER_FFI_TYPE_UINT16:
        case BENDER_FFI_TYPE_UINT32:
        case BENDER_FFI_TYPE_UINT64:
            return BENDER_AAPCS64_GENERAL;
        case BENDER_FFI_TYPE_DOUBLE:
        case BENDER_FFI_TYPE_FLOAT:
            return BENDER_AAPCS64_SSE;
        case BENDER_FFI_TYPE_STRUCT: {
            BenderFFIStruct *st = type.st;
            if (st->field_count <= 4 && aapcs64_classify(st->fields[0].type) == BENDER_AAPCS64_SSE) {
                bool is_hfa = true;
                for (uint32_t i = 1; i < st->field_count; i++) {
                    if (st->fields[0].type.prim != st->fields[i].type.prim) {
                        is_hfa = false;
                        break;
                    }
                }
                if (is_hfa) {
                    return BENDER_AAPCS64_SSE;
                }
            }

            if (type_size(type) > 16) {
                return BENDER_AAPCS64_GENERAL_REF;
            }

            return BENDER_AAPCS64_GENERAL;
        }
        case BENDER_FFI_TYPE_VOID:
            return BENDER_AAPCS64_NONE;
        default:
            bender_panic("nyi");
            return BENDER_AAPCS64_NONE;
    }
}
#endif

BENDER_CORE_FN(cfun_ffi_signature,
              "(ffi/signature calling-convention ret-type & arg-types)",
              "Create a function signature object that can be used to make calls "
              "with raw function pointers.") {
    bender_arity(argc, 2, -1);
    uint32_t frame_size = 0;
    uint32_t variant = 0;
    uint32_t arg_count = argc - 2;
    uint32_t stack_count = 0;
    BenderFFICallingConvention cc = decode_ffi_cc(bender_getkeyword(argv, 0));
    BenderFFIType ret_type = decode_ffi_type(argv[1]);
    BenderFFIMapping ret = {
        ret_type,
        BENDER_SYSV64_NO_CLASS,
        0,
        0
    };
    BenderFFIMapping mappings[BENDER_FFI_MAX_ARGS];
    for (int i = 0; i < BENDER_FFI_MAX_ARGS; i++) mappings[i] = void_mapping();
    switch (cc) {
        default:
        case BENDER_FFI_CC_NONE: {
            /* Even if unsupported, we can check that the signature is valid
             * and error at runtime */
            for (uint32_t i = 0; i < arg_count; i++) {
                decode_ffi_type(argv[i + 2]);
            }
        }
        break;

#ifdef BENDER_FFI_WIN64_ENABLED
        case BENDER_FFI_CC_WIN_64: {
            size_t ret_size = type_size(ret.type);
            uint32_t ref_stack_count = 0;
            ret.spec = BENDER_WIN64_REGISTER;
            uint32_t next_register = 0;
            if (ret_size != 0 && ret_size != 1 && ret_size != 2 && ret_size != 4 && ret_size != 8) {
                ret.spec = BENDER_WIN64_REGISTER_REF;
                next_register++;
            } else if (ret.type.prim == BENDER_FFI_TYPE_FLOAT ||
                       ret.type.prim == BENDER_FFI_TYPE_DOUBLE) {
                variant += 16;
            }
            for (uint32_t i = 0; i < arg_count; i++) {
                mappings[i].type = decode_ffi_type(argv[i + 2]);
                size_t el_size = type_size(mappings[i].type);
                int is_register_sized = (el_size == 1 || el_size == 2 || el_size == 4 || el_size == 8);
                if (next_register < 4) {
                    mappings[i].offset = next_register;
                    if (is_register_sized) {
                        mappings[i].spec = BENDER_WIN64_REGISTER;
                        if (mappings[i].type.prim == BENDER_FFI_TYPE_FLOAT ||
                                mappings[i].type.prim == BENDER_FFI_TYPE_DOUBLE) {
                            variant += 1 << (3 - next_register);
                        }
                    } else {
                        mappings[i].spec = BENDER_WIN64_REGISTER_REF;
                        mappings[i].offset2 = ref_stack_count;
                        ref_stack_count += (uint32_t)((el_size + 15) / 16);
                    }
                    next_register++;
                } else {
                    if (is_register_sized) {
                        mappings[i].spec = BENDER_WIN64_STACK;
                        mappings[i].offset = stack_count;
                        stack_count++;
                    } else {
                        mappings[i].spec = BENDER_WIN64_STACK_REF;
                        mappings[i].offset = stack_count;
                        stack_count++;
                        mappings[i].offset2 = ref_stack_count;
                        ref_stack_count += (uint32_t)((el_size + 15) / 16);
                    }
                }
            }

            /* Add reference items */
            stack_count += 2 * ref_stack_count;
            if (stack_count & 0x1) {
                stack_count++;
            }

            /* Invert stack
             * Offsets are in units of 8-bytes */
            for (uint32_t i = 0; i < arg_count; i++) {
                if (mappings[i].spec == BENDER_WIN64_STACK_REF || mappings[i].spec == BENDER_WIN64_REGISTER_REF) {
                    /* Align size to 16 bytes */
                    size_t size = (type_size(mappings[i].type) + 15) & ~0xFUL;
                    mappings[i].offset2 = (uint32_t)(stack_count - mappings[i].offset2 - (size / 8));
                }
            }

        }
        break;
#endif

#ifdef BENDER_FFI_SYSV64_ENABLED
        case BENDER_FFI_CC_SYSV_64: {
            BenderFFIWordSpec ret_spec = sysv64_classify(ret.type);
            ret.spec = ret_spec;
            if (ret_spec == BENDER_SYSV64_SSE) variant = 1;
            if (ret_spec == BENDER_SYSV64_PAIR_INTSSE) variant = 2;
            if (ret_spec == BENDER_SYSV64_PAIR_SSEINT) variant = 3;
            /* Spill register overflow to memory */
            uint32_t next_register = 0;
            uint32_t next_fp_register = 0;
            const uint32_t max_regs = 6;
            const uint32_t max_fp_regs = 8;
            if (ret_spec == BENDER_SYSV64_MEMORY) {
                /* First integer reg is pointer. */
                next_register = 1;
            }
            for (uint32_t i = 0; i < arg_count; i++) {
                mappings[i].type = decode_ffi_type(argv[i + 2]);
                mappings[i].offset = 0;
                mappings[i].spec = sysv64_classify(mappings[i].type);
                if (mappings[i].spec == BENDER_SYSV64_NO_CLASS) {
                    bender_panic("unexpected void parameter");
                }
                size_t el_size = (type_size(mappings[i].type) + 7) / 8;
                switch (mappings[i].spec) {
                    default:
                        bender_panicf("nyi: %d", mappings[i].spec);
                    case BENDER_SYSV64_INTEGER: {
                        if (next_register < max_regs) {
                            mappings[i].offset = next_register++;
                        } else {
                            mappings[i].spec = BENDER_SYSV64_MEMORY;
                            mappings[i].offset = stack_count;
                            stack_count += el_size;
                        }
                    }
                    break;
                    case BENDER_SYSV64_SSE: {
                        if (next_fp_register < max_fp_regs) {
                            mappings[i].offset = next_fp_register++;
                        } else {
                            mappings[i].spec = BENDER_SYSV64_MEMORY;
                            mappings[i].offset = stack_count;
                            stack_count += el_size;
                        }
                    }
                    break;
                    case BENDER_SYSV64_MEMORY: {
                        mappings[i].offset = stack_count;
                        stack_count += el_size;
                    }
                    break;
                    case BENDER_SYSV64_PAIR_INTINT: {
                        if (next_register + 1 < max_regs) {
                            mappings[i].offset = next_register++;
                            mappings[i].offset2 = next_register++;
                        } else {
                            mappings[i].spec = BENDER_SYSV64_MEMORY;
                            mappings[i].offset = stack_count;
                            stack_count += el_size;
                        }
                    }
                    break;
                    case BENDER_SYSV64_PAIR_INTSSE: {
                        if (next_register < max_regs && next_fp_register < max_fp_regs) {
                            mappings[i].offset = next_register++;
                            mappings[i].offset2 = next_fp_register++;
                        } else {
                            mappings[i].spec = BENDER_SYSV64_MEMORY;
                            mappings[i].offset = stack_count;
                            stack_count += el_size;
                        }
                    }
                    break;
                    case BENDER_SYSV64_PAIR_SSEINT: {
                        if (next_register < max_regs && next_fp_register < max_fp_regs) {
                            mappings[i].offset = next_fp_register++;
                            mappings[i].offset2 = next_register++;
                        } else {
                            mappings[i].spec = BENDER_SYSV64_MEMORY;
                            mappings[i].offset = stack_count;
                            stack_count += el_size;
                        }
                    }
                    break;
                    case BENDER_SYSV64_PAIR_SSESSE: {
                        if (next_fp_register < max_fp_regs) {
                            mappings[i].offset = next_fp_register++;
                            mappings[i].offset2 = next_fp_register++;
                        } else {
                            mappings[i].spec = BENDER_SYSV64_MEMORY;
                            mappings[i].offset = stack_count;
                            stack_count += el_size;
                        }
                    }
                    break;
                }
            }
        }
        break;
#endif

#ifdef BENDER_FFI_AAPCS64_ENABLED
        case BENDER_FFI_CC_AAPCS64: {
            uint32_t next_general_reg = 0;
            uint32_t next_fp_reg = 0;
            uint32_t stack_offset = 0;
            uint32_t ref_stack_offset = 0;

            BenderFFIWordSpec ret_spec = aapcs64_classify(ret_type);
            ret.spec = ret_spec;
            if (ret_spec == BENDER_AAPCS64_SSE) {
                variant = 1;
            } else if (ret_spec == BENDER_AAPCS64_GENERAL_REF) {
                if (type_size(ret_type) > sizeof(Aapcs64Variant3ReturnPointer)) {
                    bender_panic("return value bigger than supported");
                }
                variant = 2;
            } else {
                variant = 0;
            }

            for (uint32_t i = 0; i < arg_count; i++) {
                mappings[i].type = decode_ffi_type(argv[i + 2]);
                mappings[i].spec = aapcs64_classify(mappings[i].type);
                size_t arg_size = type_size(mappings[i].type);

                switch (mappings[i].spec) {
                    case BENDER_AAPCS64_GENERAL: {
                        bool arg_is_struct = mappings[i].type.prim == BENDER_FFI_TYPE_STRUCT;
                        uint32_t needed_registers = (arg_size + 7) / 8;
                        if (next_general_reg + needed_registers <= 8) {
                            mappings[i].offset = next_general_reg;
                            next_general_reg += needed_registers;
                        } else {
                            size_t arg_align = arg_is_struct ? 8 : type_align(mappings[i].type);
                            mappings[i].spec = BENDER_AAPCS64_STACK;
                            mappings[i].offset = BENDER_FFI_AAPCS64_STACK_ALIGN(stack_offset, arg_align);
#if !defined(BENDER_APPLE)
                            stack_offset += arg_size > 8 ? arg_size : 8;
#else
                            stack_offset += arg_size;
#endif
                            next_general_reg = 8;
                        }
                        break;
                    }
                    case BENDER_AAPCS64_GENERAL_REF:
                        if (next_general_reg < 8) {
                            mappings[i].offset = next_general_reg++;
                        } else {
                            mappings[i].spec = BENDER_AAPCS64_STACK_REF;
                            mappings[i].offset = BENDER_FFI_AAPCS64_STACK_ALIGN(stack_offset, 8);
                            stack_offset += 8;
                        }
                        mappings[i].offset2 = BENDER_FFI_AAPCS64_FORCE_STACK_ALIGN(ref_stack_offset, 8);
                        ref_stack_offset += arg_size;
                        break;
                    case BENDER_AAPCS64_SSE: {
                        /* AAPCS64: an HFA (homogeneous float aggregate) is
                         * passed one element per FP register -- a Vector2
                         * takes v[n] and v[n+1], not one packed register
                         * like x86-64 SysV. Scalars stay one-per. */
                        uint32_t needed_registers = 1;
                        if (mappings[i].type.prim == BENDER_FFI_TYPE_STRUCT) {
                            needed_registers = mappings[i].type.st->field_count;
                        }
                        if (next_fp_reg + needed_registers <= 8) {
                            mappings[i].offset = next_fp_reg;
                            next_fp_reg += needed_registers;
                        } else {
                            mappings[i].spec = BENDER_AAPCS64_STACK;
                            mappings[i].offset = BENDER_FFI_AAPCS64_STACK_ALIGN(stack_offset, 8);
#if !defined(BENDER_APPLE)
                            stack_offset += 8;
#else
                            stack_offset += arg_size;
#endif
                        }
                        break;
                    }
                    default:
                        bender_panic("nyi");
                }
            }

            stack_offset = (stack_offset + 15) & ~0xFUL;
            ref_stack_offset = (ref_stack_offset + 15) & ~0xFUL;
            stack_count = stack_offset + ref_stack_offset;

            for (uint32_t i = 0; i < arg_count; i++) {
                if (mappings[i].spec == BENDER_AAPCS64_GENERAL_REF || mappings[i].spec == BENDER_AAPCS64_STACK_REF) {
                    mappings[i].offset2 = stack_offset + mappings[i].offset2;
                }
            }
        }
        break;
#endif
    }

    /* Create signature abstract value */
    BenderFFISignature *abst = bender_abstract(&bender_signature_type, sizeof(BenderFFISignature));
    abst->frame_size = frame_size;
    abst->cc = cc;
    abst->ret = ret;
    abst->arg_count = arg_count;
    abst->variant = variant;
    abst->stack_count = stack_count;
    memcpy(abst->args, mappings, sizeof(BenderFFIMapping) * BENDER_FFI_MAX_ARGS);
    return bender_wrap_abstract(abst);
}

#ifdef BENDER_FFI_SYSV64_ENABLED

static void bender_ffi_sysv64_standard_callback(void *ctx, void *userdata) {
    bender_ffi_trampoline(ctx, userdata);
}

/* Functions that set all argument registers. Two variants - one to read rax and rdx returns, another
 * to read xmm0 and xmm1 returns. */
typedef struct {
    uint64_t x;
    uint64_t y;
} sysv64_int_return;
typedef struct {
    double x;
    double y;
} sysv64_sse_return;
typedef struct {
    uint64_t x;
    double y;
} sysv64_intsse_return;
typedef struct {
    double y;
    uint64_t x;
} sysv64_sseint_return;
typedef sysv64_int_return bender_sysv64_variant_1(uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f,
        double r1, double r2, double r3, double r4, double r5, double r6, double r7, double r8);
typedef sysv64_sse_return bender_sysv64_variant_2(uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f,
        double r1, double r2, double r3, double r4, double r5, double r6, double r7, double r8);
typedef sysv64_intsse_return bender_sysv64_variant_3(uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f,
        double r1, double r2, double r3, double r4, double r5, double r6, double r7, double r8);
typedef sysv64_sseint_return bender_sysv64_variant_4(uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f,
        double r1, double r2, double r3, double r4, double r5, double r6, double r7, double r8);

static Bender bender_ffi_sysv64(BenderFFISignature *signature, void *function_pointer, const Bender *argv) {
    union {
        sysv64_int_return int_return;
        sysv64_sse_return sse_return;
        sysv64_sseint_return sseint_return;
        sysv64_intsse_return intsse_return;
    } retu;
    uint64_t pair[2];
    uint64_t regs[6];
    double fp_regs[8];
    BenderFFIWordSpec ret_spec = signature->ret.spec;
    void *ret_mem = &retu.int_return;
    if (ret_spec == BENDER_SYSV64_MEMORY) {
        ret_mem = alloca(type_size(signature->ret.type));
        regs[0] = (uint64_t) ret_mem;
    }
    uint64_t *stack = alloca(sizeof(uint64_t) * signature->stack_count);
    for (uint32_t i = 0; i < signature->arg_count; i++) {
        uint64_t *to;
        int32_t n = i + 2;
        BenderFFIMapping arg = signature->args[i];
        switch (arg.spec) {
            default:
                bender_panic("nyi");
            case BENDER_SYSV64_INTEGER:
                to = regs + arg.offset;
                break;
            case BENDER_SYSV64_SSE:
                to = (uint64_t *)(fp_regs + arg.offset);
                break;
            case BENDER_SYSV64_MEMORY:
                to = stack + arg.offset;
                break;
            case BENDER_SYSV64_PAIR_INTINT:
                bender_ffi_write_one(pair, argv, n, arg.type, BENDER_FFI_MAX_RECUR);
                regs[arg.offset] = pair[0];
                regs[arg.offset2] = pair[1];
                continue;
            case BENDER_SYSV64_PAIR_INTSSE:
                bender_ffi_write_one(pair, argv, n, arg.type, BENDER_FFI_MAX_RECUR);
                regs[arg.offset] = pair[0];
                ((uint64_t *) fp_regs)[arg.offset2] = pair[1];
                continue;
            case BENDER_SYSV64_PAIR_SSEINT:
                bender_ffi_write_one(pair, argv, n, arg.type, BENDER_FFI_MAX_RECUR);
                ((uint64_t *) fp_regs)[arg.offset] = pair[0];
                regs[arg.offset2] = pair[1];
                continue;
            case BENDER_SYSV64_PAIR_SSESSE:
                bender_ffi_write_one(pair, argv, n, arg.type, BENDER_FFI_MAX_RECUR);
                ((uint64_t *) fp_regs)[arg.offset] = pair[0];
                ((uint64_t *) fp_regs)[arg.offset2] = pair[1];
                continue;
        }
        bender_ffi_write_one(to, argv, n, arg.type, BENDER_FFI_MAX_RECUR);
    }

    switch (signature->variant) {
        case 0:
            retu.int_return = ((bender_sysv64_variant_1 *)(function_pointer))(
                                  regs[0], regs[1], regs[2], regs[3], regs[4], regs[5],
                                  fp_regs[0], fp_regs[1], fp_regs[2], fp_regs[3],
                                  fp_regs[4], fp_regs[5], fp_regs[6], fp_regs[7]);
            break;
        case 1:
            retu.sse_return = ((bender_sysv64_variant_2 *)(function_pointer))(
                                  regs[0], regs[1], regs[2], regs[3], regs[4], regs[5],
                                  fp_regs[0], fp_regs[1], fp_regs[2], fp_regs[3],
                                  fp_regs[4], fp_regs[5], fp_regs[6], fp_regs[7]);
            break;
        case 2:
            retu.intsse_return = ((bender_sysv64_variant_3 *)(function_pointer))(
                                     regs[0], regs[1], regs[2], regs[3], regs[4], regs[5],
                                     fp_regs[0], fp_regs[1], fp_regs[2], fp_regs[3],
                                     fp_regs[4], fp_regs[5], fp_regs[6], fp_regs[7]);
            break;
        case 3:
            retu.sseint_return = ((bender_sysv64_variant_4 *)(function_pointer))(
                                     regs[0], regs[1], regs[2], regs[3], regs[4], regs[5],
                                     fp_regs[0], fp_regs[1], fp_regs[2], fp_regs[3],
                                     fp_regs[4], fp_regs[5], fp_regs[6], fp_regs[7]);
            break;
    }

    return bender_ffi_read_one(ret_mem, signature->ret.type, BENDER_FFI_MAX_RECUR);
}

#endif

#ifdef BENDER_FFI_WIN64_ENABLED

static void bender_ffi_win64_standard_callback(void *ctx, void *userdata) {
    bender_ffi_trampoline(ctx, userdata);
}

/* Variants that allow setting all required registers for 64 bit windows calling convention.
 * win64 calling convention has up to 4 arguments on registers, and one register for returns.
 * Each register can either be an integer or floating point register, resulting in
 * 2^5 = 32 variants. Unlike sysv, there are no function signatures that will fill
 * all of the possible registers which is why we have so many variants. If you were using
 * assembly, you could manually fill all of the registers and only have a single variant.
 * And msvc does not support inline assembly on 64 bit targets, so yeah, we have this hackery. */
typedef uint64_t (win64_variant_i_iiii)(uint64_t, uint64_t, uint64_t, uint64_t);
typedef uint64_t (win64_variant_i_iiif)(uint64_t, uint64_t, uint64_t, double);
typedef uint64_t (win64_variant_i_iifi)(uint64_t, uint64_t, double, uint64_t);
typedef uint64_t (win64_variant_i_iiff)(uint64_t, uint64_t, double, double);
typedef uint64_t (win64_variant_i_ifii)(uint64_t, double, uint64_t, uint64_t);
typedef uint64_t (win64_variant_i_ifif)(uint64_t, double, uint64_t, double);
typedef uint64_t (win64_variant_i_iffi)(uint64_t, double, double, uint64_t);
typedef uint64_t (win64_variant_i_ifff)(uint64_t, double, double, double);
typedef uint64_t (win64_variant_i_fiii)(double, uint64_t, uint64_t, uint64_t);
typedef uint64_t (win64_variant_i_fiif)(double, uint64_t, uint64_t, double);
typedef uint64_t (win64_variant_i_fifi)(double, uint64_t, double, uint64_t);
typedef uint64_t (win64_variant_i_fiff)(double, uint64_t, double, double);
typedef uint64_t (win64_variant_i_ffii)(double, double, uint64_t, uint64_t);
typedef uint64_t (win64_variant_i_ffif)(double, double, uint64_t, double);
typedef uint64_t (win64_variant_i_fffi)(double, double, double, uint64_t);
typedef uint64_t (win64_variant_i_ffff)(double, double, double, double);
typedef double (win64_variant_f_iiii)(uint64_t, uint64_t, uint64_t, uint64_t);
typedef double (win64_variant_f_iiif)(uint64_t, uint64_t, uint64_t, double);
typedef double (win64_variant_f_iifi)(uint64_t, uint64_t, double, uint64_t);
typedef double (win64_variant_f_iiff)(uint64_t, uint64_t, double, double);
typedef double (win64_variant_f_ifii)(uint64_t, double, uint64_t, uint64_t);
typedef double (win64_variant_f_ifif)(uint64_t, double, uint64_t, double);
typedef double (win64_variant_f_iffi)(uint64_t, double, double, uint64_t);
typedef double (win64_variant_f_ifff)(uint64_t, double, double, double);
typedef double (win64_variant_f_fiii)(double, uint64_t, uint64_t, uint64_t);
typedef double (win64_variant_f_fiif)(double, uint64_t, uint64_t, double);
typedef double (win64_variant_f_fifi)(double, uint64_t, double, uint64_t);
typedef double (win64_variant_f_fiff)(double, uint64_t, double, double);
typedef double (win64_variant_f_ffii)(double, double, uint64_t, uint64_t);
typedef double (win64_variant_f_ffif)(double, double, uint64_t, double);
typedef double (win64_variant_f_fffi)(double, double, double, uint64_t);
typedef double (win64_variant_f_ffff)(double, double, double, double);

/* MSVC stack frame runtime error checking (/RTCs) prepends alloca() allocations with an _RTC_ALLOCA_NODE
 * header; misalligning stack-based FFI arguments and causing the memmove() (by stack_shift) to corrupt
 * the _RTC_ALLOCA_NODE header.
 *
 * We turn off the RTC-instrumented alloca() and adding of _RTC_CheckStackVars to function prologue just
 * for bender_ffi_win64() */
#ifdef __MSVC_RUNTIME_CHECKS
#pragma runtime_checks( "s", off )
#endif
static Bender bender_ffi_win64(BenderFFISignature *signature, void *function_pointer, const Bender *argv) {
    union {
        uint64_t integer;
        double real;
    } regs[4];
    union {
        uint64_t integer;
        double real;
    } ret_reg;
    BenderFFIWordSpec ret_spec = signature->ret.spec;
    void *ret_mem = &ret_reg.integer;
    if (ret_spec == BENDER_WIN64_REGISTER_REF) {
        ret_mem = alloca(type_size(signature->ret.type));
        regs[0].integer = (uint64_t) ret_mem;
    }
    size_t stack_size = signature->stack_count * 8;
    size_t stack_shift = 2;
    uint64_t *stack = alloca(stack_size);
    for (uint32_t i = 0; i < signature->arg_count; i++) {
        int32_t n = i + 2;
        BenderFFIMapping arg = signature->args[i];
        if (arg.spec == BENDER_WIN64_STACK) {
            bender_ffi_write_one(stack + arg.offset, argv, n, arg.type, BENDER_FFI_MAX_RECUR);
        } else if (arg.spec == BENDER_WIN64_STACK_REF) {
            uint8_t *ptr = (uint8_t *)(stack + arg.offset2);
            bender_ffi_write_one(ptr, argv, n, arg.type, BENDER_FFI_MAX_RECUR);
            stack[arg.offset] = (uint64_t)(ptr - stack_shift * sizeof(uint64_t));
        } else if (arg.spec == BENDER_WIN64_REGISTER_REF) {
            uint8_t *ptr = (uint8_t *)(stack + arg.offset2);
            bender_ffi_write_one(ptr, argv, n, arg.type, BENDER_FFI_MAX_RECUR);
            regs[arg.offset].integer = (uint64_t)(ptr - stack_shift * sizeof(uint64_t));
        } else {
            bender_ffi_write_one((uint8_t *) &regs[arg.offset].integer, argv, n, arg.type, BENDER_FFI_MAX_RECUR);
        }
    }

    /* hack to get proper stack placement and avoid clobbering from logic above - shift stack down, otherwise we have issues.
     * Technically, this writes into 16 bytes of unallocated stack memory */
#ifdef BENDER_MINGW
#pragma GCC diagnostic ignored "-Wstringop-overflow"
#endif
    if (stack_size) memmove(stack - stack_shift, stack, stack_size);
#ifdef BENDER_MINGW
#pragma GCC diagnostic pop
#endif

    switch (signature->variant) {
        default:
            bender_panicf("unknown variant %d", signature->variant);
        case 0:
            ret_reg.integer = ((win64_variant_i_iiii *) function_pointer)(regs[0].integer, regs[1].integer, regs[2].integer, regs[3].integer);
            break;
        case 1:
            ret_reg.integer = ((win64_variant_i_iiif *) function_pointer)(regs[0].integer, regs[1].integer, regs[2].integer, regs[3].real);
            break;
        case 2:
            ret_reg.integer = ((win64_variant_i_iifi *) function_pointer)(regs[0].integer, regs[1].integer, regs[2].real, regs[3].integer);
            break;
        case 3:
            ret_reg.integer = ((win64_variant_i_iiff *) function_pointer)(regs[0].integer, regs[1].integer, regs[2].real, regs[3].real);
            break;
        case 4:
            ret_reg.integer = ((win64_variant_i_ifii *) function_pointer)(regs[0].integer, regs[1].real, regs[2].integer, regs[3].integer);
            break;
        case 5:
            ret_reg.integer = ((win64_variant_i_ifif *) function_pointer)(regs[0].integer, regs[1].real, regs[2].integer, regs[3].real);
            break;
        case 6:
            ret_reg.integer = ((win64_variant_i_iffi *) function_pointer)(regs[0].integer, regs[1].real, regs[2].real, regs[3].integer);
            break;
        case 7:
            ret_reg.integer = ((win64_variant_i_ifff *) function_pointer)(regs[0].integer, regs[1].real, regs[2].real, regs[3].real);
            break;
        case 8:
            ret_reg.integer = ((win64_variant_i_fiii *) function_pointer)(regs[0].real, regs[1].integer, regs[2].integer, regs[3].integer);
            break;
        case 9:
            ret_reg.integer = ((win64_variant_i_fiif *) function_pointer)(regs[0].real, regs[1].integer, regs[2].integer, regs[3].real);
            break;
        case 10:
            ret_reg.integer = ((win64_variant_i_fifi *) function_pointer)(regs[0].real, regs[1].integer, regs[2].real, regs[3].integer);
            break;
        case 11:
            ret_reg.integer = ((win64_variant_i_fiff *) function_pointer)(regs[0].real, regs[1].integer, regs[2].real, regs[3].real);
            break;
        case 12:
            ret_reg.integer = ((win64_variant_i_ffii *) function_pointer)(regs[0].real, regs[1].real, regs[2].integer, regs[3].integer);
            break;
        case 13:
            ret_reg.integer = ((win64_variant_i_ffif *) function_pointer)(regs[0].real, regs[1].real, regs[2].integer, regs[3].real);
            break;
        case 14:
            ret_reg.integer = ((win64_variant_i_fffi *) function_pointer)(regs[0].real, regs[1].real, regs[2].real, regs[3].integer);
            break;
        case 15:
            ret_reg.integer = ((win64_variant_i_ffff *) function_pointer)(regs[0].real, regs[1].real, regs[2].real, regs[3].real);
            break;
        case 16:
            ret_reg.real = ((win64_variant_f_iiii *) function_pointer)(regs[0].integer, regs[1].integer, regs[2].integer, regs[3].integer);
            break;
        case 17:
            ret_reg.real = ((win64_variant_f_iiif *) function_pointer)(regs[0].integer, regs[1].integer, regs[2].integer, regs[3].real);
            break;
        case 18:
            ret_reg.real = ((win64_variant_f_iifi *) function_pointer)(regs[0].integer, regs[1].integer, regs[2].real, regs[3].integer);
            break;
        case 19:
            ret_reg.real = ((win64_variant_f_iiff *) function_pointer)(regs[0].integer, regs[1].integer, regs[2].real, regs[3].real);
            break;
        case 20:
            ret_reg.real = ((win64_variant_f_ifii *) function_pointer)(regs[0].integer, regs[1].real, regs[2].integer, regs[3].integer);
            break;
        case 21:
            ret_reg.real = ((win64_variant_f_ifif *) function_pointer)(regs[0].integer, regs[1].real, regs[2].integer, regs[3].real);
            break;
        case 22:
            ret_reg.real = ((win64_variant_f_iffi *) function_pointer)(regs[0].integer, regs[1].real, regs[2].real, regs[3].integer);
            break;
        case 23:
            ret_reg.real = ((win64_variant_f_ifff *) function_pointer)(regs[0].integer, regs[1].real, regs[2].real, regs[3].real);
            break;
        case 24:
            ret_reg.real = ((win64_variant_f_fiii *) function_pointer)(regs[0].real, regs[1].integer, regs[2].integer, regs[3].integer);
            break;
        case 25:
            ret_reg.real = ((win64_variant_f_fiif *) function_pointer)(regs[0].real, regs[1].integer, regs[2].integer, regs[3].real);
            break;
        case 26:
            ret_reg.real = ((win64_variant_f_fifi *) function_pointer)(regs[0].real, regs[1].integer, regs[2].real, regs[3].integer);
            break;
        case 27:
            ret_reg.real = ((win64_variant_f_fiff *) function_pointer)(regs[0].real, regs[1].integer, regs[2].real, regs[3].real);
            break;
        case 28:
            ret_reg.real = ((win64_variant_f_ffii *) function_pointer)(regs[0].real, regs[1].real, regs[2].integer, regs[3].integer);
            break;
        case 29:
            ret_reg.real = ((win64_variant_f_ffif *) function_pointer)(regs[0].real, regs[1].real, regs[2].integer, regs[3].real);
            break;
        case 30:
            ret_reg.real = ((win64_variant_f_fffi *) function_pointer)(regs[0].real, regs[1].real, regs[2].real, regs[3].integer);
            break;
        case 31:
            ret_reg.real = ((win64_variant_f_ffff *) function_pointer)(regs[0].real, regs[1].real, regs[2].real, regs[3].real);
            break;
    }

    return bender_ffi_read_one(ret_mem, signature->ret.type, BENDER_FFI_MAX_RECUR);
}
#ifdef __MSVC_RUNTIME_CHECKS
// Restore stack frame runtime error checking (/RTCs) if it was enabled.
#pragma runtime_checks ( "s", restore )
#endif

#endif

#ifdef BENDER_FFI_AAPCS64_ENABLED

static void bender_ffi_aapcs64_standard_callback(void *ctx, void *userdata) {
    bender_ffi_trampoline(ctx, userdata);
}

typedef Aapcs64Variant1ReturnGeneral bender_aapcs64_variant_1(uint64_t x0, uint64_t x1, uint64_t x2, uint64_t x3, uint64_t x4, uint64_t x5, uint64_t x6, uint64_t x7,
        double v0, double v1, double v2, double v3, double v4, double v5, double v6, double v7);
typedef Aapcs64Variant2ReturnSse bender_aapcs64_variant_2(uint64_t x0, uint64_t x1, uint64_t x2, uint64_t x3, uint64_t x4, uint64_t x5, uint64_t x6, uint64_t x7,
        double v0, double v1, double v2, double v3, double v4, double v5, double v6, double v7);
typedef Aapcs64Variant3ReturnPointer bender_aapcs64_variant_3(uint64_t x0, uint64_t x1, uint64_t x2, uint64_t x3, uint64_t x4, uint64_t x5, uint64_t x6, uint64_t x7,
        double v0, double v1, double v2, double v3, double v4, double v5, double v6, double v7);


static Bender bender_ffi_aapcs64(BenderFFISignature *signature, void *function_pointer, const Bender *argv) {
    union {
        Aapcs64Variant1ReturnGeneral general_return;
        Aapcs64Variant2ReturnSse sse_return;
        Aapcs64Variant3ReturnPointer pointer_return;
    } retu;
    uint64_t regs[8];
    double fp_regs[8];
    void *ret_mem = &retu.general_return;

    /* Apple's stack values do not need to be 8-byte aligned,
     * thus all stack offsets refer to actual byte positions. */
    uint8_t *stack = alloca(signature->stack_count);
#if defined(BENDER_APPLE)
    /* Values must be zero-extended by the caller instead of the callee. */
    memset(stack, 0, signature->stack_count);
#endif
    for (uint32_t i = 0; i < signature->arg_count; i++) {
        int32_t n = i + 2;
        BenderFFIMapping arg = signature->args[i];
        void *to = NULL;

        switch (arg.spec) {
            case BENDER_AAPCS64_GENERAL:
                to = regs + arg.offset;
                break;
            case BENDER_AAPCS64_GENERAL_REF:
                to = stack + arg.offset2;
                regs[arg.offset] = (uint64_t) to;
                break;
            case BENDER_AAPCS64_SSE:
                if (arg.type.prim == BENDER_FFI_TYPE_STRUCT) {
                    /* HFA: one float element per FP register. */
                    BenderView els = bender_getindexed(argv, n);
                    BenderFFIStruct *st = arg.type.st;
                    if ((uint32_t) els.len != st->field_count) {
                        bender_panicf("wrong number of fields in struct, expected %d, got %d",
                                     (int32_t) st->field_count, els.len);
                    }
                    for (uint32_t k = 0; k < st->field_count; k++) {
                        bender_ffi_write_one(fp_regs + arg.offset + k, els.items, k,
                                            st->fields[k].type, BENDER_FFI_MAX_RECUR);
                    }
                } else {
                    to = fp_regs + arg.offset;
                }
                break;
            case BENDER_AAPCS64_STACK:
                to = stack + arg.offset;
                break;
            case BENDER_AAPCS64_STACK_REF:
                to = stack + arg.offset2;
                uint64_t *ptr = (uint64_t *) stack + arg.offset;
                *ptr = (uint64_t) to;
                break;
            default:
                bender_panic("nyi");
        }

        if (to) {
            bender_ffi_write_one(to, argv, n, arg.type, BENDER_FFI_MAX_RECUR);
        }
    }

    switch (signature->variant) {
        case 0:
            retu.general_return = ((bender_aapcs64_variant_1 *)(function_pointer))(
                                      regs[0], regs[1], regs[2], regs[3],
                                      regs[4], regs[5], regs[6], regs[7],
                                      fp_regs[0], fp_regs[1], fp_regs[2], fp_regs[3],
                                      fp_regs[4], fp_regs[5], fp_regs[6], fp_regs[7]);
            break;
        case 1:
            retu.sse_return = ((bender_aapcs64_variant_2 *)(function_pointer))(
                                  regs[0], regs[1], regs[2], regs[3],
                                  regs[4], regs[5], regs[6], regs[7],
                                  fp_regs[0], fp_regs[1], fp_regs[2], fp_regs[3],
                                  fp_regs[4], fp_regs[5], fp_regs[6], fp_regs[7]);
            break;
        case 2: {
            retu.pointer_return = ((bender_aapcs64_variant_3 *)(function_pointer))(
                                      regs[0], regs[1], regs[2], regs[3],
                                      regs[4], regs[5], regs[6], regs[7],
                                      fp_regs[0], fp_regs[1], fp_regs[2], fp_regs[3],
                                      fp_regs[4], fp_regs[5], fp_regs[6], fp_regs[7]);
        }
    }

    /* AAPCS64 HFA returns come back one element per FP register
     * (x in v0, y in v1, ...), which the variant-1 union captures as
     * sse_return.a..d. Parse element-wise instead of a C-layout read;
     * float elements sit in the low half of each slot. */
    if (signature->variant == 1 &&
            signature->ret.type.prim == BENDER_FFI_TYPE_STRUCT) {
        BenderFFIStruct *st = signature->ret.type.st;
        Bender *tup = bender_tuple_begin(st->field_count);
        float *fslots = (float *) &retu.sse_return;
        /* each double slot is 2 floats; element k of a float HFA lives in
         * the low half of slot k */
        for (uint32_t k = 0; k < st->field_count; k++) {
            BenderFFIType ft = st->fields[k].type;
            if (ft.prim == BENDER_FFI_TYPE_FLOAT) {
                tup[k] = bender_wrap_number((double) fslots[2 * k]);
            } else if (ft.prim == BENDER_FFI_TYPE_DOUBLE) {
                double *dslots = (double *) &retu.sse_return;
                tup[k] = bender_wrap_number(dslots[k]);
            } else {
                bender_panic("unsupported mixed HFA return");
            }
        }
        return bender_wrap_tuple(bender_tuple_end(tup));
    }
    return bender_ffi_read_one(ret_mem, signature->ret.type, BENDER_FFI_MAX_RECUR);
}

#endif

/* Allocate executable memory chunks in sizes of a page. Ideally we would keep
 * an allocator around so that multiple JIT allocations would point to the same
 * region but it isn't really worth it. */
#define FFI_PAGE_MASK 0xFFF

BENDER_CORE_FN(cfun_ffi_jitfn,
              "(unsafe/jitfn bytes)",
              "Create an abstract type that can be used as the pointer argument to `ffi/call`. The content "
              "of `bytes` is architecture specific machine code that will be copied into executable memory.") {
    bender_sandbox_assert(BENDER_SANDBOX_FFI_JIT);
    bender_fixarity(argc, 1);
    BenderByteView bytes = bender_getbytes(argv, 0);

    /* Quick hack to align to page boundary, we should query OS. FIXME */
    size_t alloc_size = ((size_t) bytes.len + FFI_PAGE_MASK) & ~FFI_PAGE_MASK;

#ifdef BENDER_FFI_JIT
#ifdef BENDER_EV
    BenderFFIJittedFn *fn = bender_abstract_threaded(&bender_type_ffijit, sizeof(BenderFFIJittedFn));
#else
    BenderFFIJittedFn *fn = bender_abstract(&bender_type_ffijit, sizeof(BenderFFIJittedFn));
#endif
    fn->function_pointer = NULL;
    fn->size = 0;
#ifdef BENDER_WINDOWS
    void *ptr = VirtualAlloc(NULL, alloc_size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!ptr) {
        bender_panic("failed to allocate writable memory");
    }
#else
#ifdef MAP_ANONYMOUS
    void *ptr = mmap(0, alloc_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
#elif defined(MAP_ANON)
    /* macos doesn't have MAP_ANONYMOUS */
    void *ptr = mmap(0, alloc_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
#else
    /* -std=c99 gets in the way */
    /* #define MAP_ANONYMOUS 0x20 should work, though. */
    void *ptr = mmap(0, alloc_size, PROT_READ | PROT_WRITE, MAP_PRIVATE, -1, 0);
#endif
    if (ptr == MAP_FAILED) {
        bender_panic("failed to memory map writable memory");
    }
#endif
    memcpy(ptr, bytes.bytes, bytes.len);
#ifdef BENDER_WINDOWS
    DWORD old = 0;
    if (!VirtualProtect(ptr, alloc_size, PAGE_EXECUTE_READ, &old)) {
        bender_panic("failed to make mapped memory executable");
    }
#else
    if (mprotect(ptr, alloc_size, PROT_READ | PROT_EXEC) == -1) {
        bender_panic("failed to make mapped memory executable");
    }
#endif
    fn->size = alloc_size;
    fn->function_pointer = ptr;
    return bender_wrap_abstract(fn);
#else
    bender_panic("unsafe/jitfn not available on this platform");
#endif
}

BENDER_CORE_FN(cfun_ffi_call,
              "(ffi/call ptr signature & args)",
              "Call a raw pointer `ptr` as a function pointer. The function "
              "signature specifies how Bender values in `args` are converted "
              "to native machine types.") {
    bender_sandbox_assert(BENDER_SANDBOX_FFI_USE);
    bender_arity(argc, 2, -1);
    void *function_pointer = bender_ffi_get_callable_pointer(argv, 0);
    BenderFFISignature *signature = bender_getabstract(argv, 1, &bender_signature_type);
    bender_fixarity(argc - 2, signature->arg_count);
    switch (signature->cc) {
        default:
        case BENDER_FFI_CC_NONE:
            (void) function_pointer;
            bender_panic("calling convention not supported");
#ifdef BENDER_FFI_WIN64_ENABLED
        case BENDER_FFI_CC_WIN_64:
            return bender_ffi_win64(signature, function_pointer, argv);
#endif
#ifdef BENDER_FFI_SYSV64_ENABLED
        case BENDER_FFI_CC_SYSV_64:
            return bender_ffi_sysv64(signature, function_pointer, argv);
#endif
#ifdef BENDER_FFI_AAPCS64_ENABLED
        case BENDER_FFI_CC_AAPCS64:
            return bender_ffi_aapcs64(signature, function_pointer, argv);
#endif
    }
}

BENDER_CORE_FN(cfun_ffi_buffer_write,
              "(ffi/write ffi-type data &opt buf index)",
              "Append a native type, `ffi-type`, to a buffer as it would "
              "appear in memory. This can be used to pass pointers to "
              "structs in the ffi, or send C/C++/native structs over "
              "the network or to files. Returns a modified buffer, `buf`, "
              "or a new buffer if not supplied.") {
    bender_sandbox_assert(BENDER_SANDBOX_FFI_USE);
    bender_arity(argc, 2, 4);
    BenderFFIType type = decode_ffi_type(argv[0]);
    uint32_t el_size = (uint32_t) type_size(type);
    BenderBuffer *buffer = bender_optbuffer(argv, argc, 2, el_size);
    int32_t index = bender_optnat(argv, argc, 3, buffer->count);
    int32_t old_count = buffer->count;
    if (index > old_count) bender_panic("index out of bounds");
    buffer->count = index;
    bender_buffer_extra(buffer, el_size);
    buffer->count = old_count;
    memset(buffer->data + index, 0, el_size);
    bender_ffi_write_one(buffer->data + index, argv, 1, type, BENDER_FFI_MAX_RECUR);
    index += el_size;
    if (buffer->count < index) buffer->count = index;
    return bender_wrap_buffer(buffer);
}

BENDER_CORE_FN(cfun_ffi_buffer_read,
              "(ffi/read ffi-type bytes &opt offset)",
              "Parse a native struct out of a buffer and convert it to normal Bender data structures. "
              "This function is the inverse of `ffi/write`. `bytes` can also be a raw pointer, although "
              "this is unsafe.") {
    bender_sandbox_assert(BENDER_SANDBOX_FFI_USE);
    bender_arity(argc, 2, 3);
    BenderFFIType type = decode_ffi_type(argv[0]);
    size_t offset = (size_t) bender_optnat(argv, argc, 2, 0);
    if (bender_checktype(argv[1], BENDER_POINTER)) {
        uint8_t *ptr = bender_unwrap_pointer(argv[1]);
        return bender_ffi_read_one(ptr + offset, type, BENDER_FFI_MAX_RECUR);
    } else {
        size_t el_size = type_size(type);
        BenderByteView bytes = bender_getbytes(argv, 1);
        if ((size_t) bytes.len < offset + el_size) bender_panic("read out of range");
        return bender_ffi_read_one(bytes.bytes + offset, type, BENDER_FFI_MAX_RECUR);
    }
}

BENDER_CORE_FN(cfun_ffi_get_callback_trampoline,
              "(unsafe/trampoline cc)",
              "Get a native function pointer that can be used as a callback "
              "and passed to C libraries. This callback trampoline has the "
              "signature `void trampoline(void *ctx, void *userdata)` in "
              "the given calling convention. This is the only function "
              "signature supported. It is up to the programmer to ensure "
              "that the `userdata` argument contains a bender function that "
              "will be called with one argument, `ctx` which is an opaque "
              "pointer. This pointer can be further inspected with "
              "`ffi/read`.") {
    bender_arity(argc, 0, 1);
    BenderFFICallingConvention cc = BENDER_FFI_CC_DEFAULT;
    if (argc >= 1) cc = decode_ffi_cc(bender_getkeyword(argv, 0));
    switch (cc) {
        default:
        case BENDER_FFI_CC_NONE:
            bender_panic("calling convention not supported");
#ifdef BENDER_FFI_WIN64_ENABLED
        case BENDER_FFI_CC_WIN_64:
            return bender_wrap_pointer(bender_ffi_win64_standard_callback);
#endif
#ifdef BENDER_FFI_SYSV64_ENABLED
        case BENDER_FFI_CC_SYSV_64:
            return bender_wrap_pointer(bender_ffi_sysv64_standard_callback);
#endif
#ifdef BENDER_FFI_AAPCS64_ENABLED
        case BENDER_FFI_CC_AAPCS64:
            return bender_wrap_pointer(bender_ffi_aapcs64_standard_callback);
#endif
    }
}

BENDER_CORE_FN(bender_core_raw_native,
              "(ffi/native &opt path)",
              "Load a shared object or dll from the given path, and do not extract"
              " or run any code from it. This is different than `native`, which will "
              "run initialization code to get a module table. If `path` is nil, opens the current running binary. "
              "Returns a `core/native`.") {
    bender_sandbox_assert(BENDER_SANDBOX_FFI_DEFINE);
    bender_arity(argc, 0, 1);
    const char *path = bender_optcstring(argv, argc, 0, NULL);
    Clib lib = load_clib(path);
    if (!lib) bender_panic(error_clib());
    BenderAbstractNative *anative = bender_abstract(&bender_native_type, sizeof(BenderAbstractNative));
    anative->clib = lib;
    anative->closed = 0;
    anative->is_self = path == NULL;
    return bender_wrap_abstract(anative);
}

BENDER_CORE_FN(bender_core_native_lookup,
              "(ffi/lookup ffi-native sym-name)",
              "Lookup a symbol `sym-name` from a native object "
              "`ffi-native`. All symbol lookups return a raw pointer if "
              "the symbol is found, else nil.") {
    bender_sandbox_assert(BENDER_SANDBOX_FFI_DEFINE);
    bender_fixarity(argc, 2);
    BenderAbstractNative *anative = bender_getabstract(argv, 0, &bender_native_type);
    const char *sym = bender_getcstring(argv, 1);
    if (anative->closed) bender_panic("native object already closed");
    void *value = symbol_clib(anative->clib, sym);
    if (NULL == value) return bender_wrap_nil();
    return bender_wrap_pointer(value);
}

BENDER_CORE_FN(bender_core_native_close,
              "(ffi/close ffi-native)",
              "Free a native object `ffi-native`. Dereferencing pointers to "
              "symbols in the object has undefined behavior after freeing.") {
    bender_sandbox_assert(BENDER_SANDBOX_FFI_DEFINE);
    bender_fixarity(argc, 1);
    BenderAbstractNative *anative = bender_getabstract(argv, 0, &bender_native_type);
    if (anative->closed) bender_panic("native object already closed");
    if (anative->is_self) bender_panic("cannot close self");
    anative->closed = 1;
    free_clib(anative->clib);
    return bender_wrap_nil();
}

BENDER_CORE_FN(cfun_ffi_malloc,
              "(unsafe/malloc size)",
              "Allocates memory directly using the bender memory allocator. Memory allocated in this way must be freed manually! Returns a raw pointer, or nil if size = 0.") {
    bender_sandbox_assert(BENDER_SANDBOX_FFI_USE);
    bender_fixarity(argc, 1);
    size_t size = bender_getsize(argv, 0);
    if (size == 0) return bender_wrap_nil();
    return bender_wrap_pointer(bender_malloc(size));
}

BENDER_CORE_FN(cfun_ffi_free,
              "(unsafe/free ptr)",
              "Free memory reachable from `ptr` allocated with "
              "`unsafe/malloc`. Returns nil.") {
    bender_sandbox_assert(BENDER_SANDBOX_FFI_USE);
    bender_fixarity(argc, 1);
    if (bender_checktype(argv[0], BENDER_NIL)) return bender_wrap_nil();
    void *pointer = bender_getpointer(argv, 0);
    bender_free(pointer);
    return bender_wrap_nil();
}

/* High-risk native memory primitives (PGO companion work): pointer
 * arithmetic, typed peek/poke through raw pointers, calloc/realloc, and
 * explicit executable-page control for hand-rolled JITs. These are
 * deliberately low-level -- wrong use corrupts memory with no safety
 * net. Documented in docs/internals/native-memory.md. */

BENDER_CORE_FN(cfun_ffi_ptr_add,
              "(unsafe/ptr-add ptr nbytes)",
              "Pointer arithmetic: return a new pointer `nbytes` (signed, "
              "may be negative) past `ptr`. No bounds checking is possible; "
              "out-of-range results are instant undefined behavior. "
              "Returns a pointer.") {
    bender_sandbox_assert(BENDER_SANDBOX_FFI_USE);
    bender_arity(argc, 2, 2);
    uint8_t *pointer = bender_getpointer(argv, 0);
    int64_t off = bender_getinteger64(argv, 1);
    return bender_wrap_pointer(pointer + off);
}

BENDER_CORE_FN(cfun_ffi_ptr_sub,
              "(unsafe/ptr-sub ptr nbytes)",
              "Pointer arithmetic: return a new pointer `nbytes` (signed) "
              "before `ptr`. Same caveats as `unsafe/ptr-add`. Returns a pointer.") {
    bender_sandbox_assert(BENDER_SANDBOX_FFI_USE);
    bender_arity(argc, 2, 2);
    uint8_t *pointer = bender_getpointer(argv, 0);
    int64_t off = bender_getinteger64(argv, 1);
    return bender_wrap_pointer(pointer - off);
}

BENDER_CORE_FN(cfun_ffi_ptr_eq,
              "(unsafe/ptr-eq a b)",
              "Pointer identity: true when `a` and `b` (pointers or nil) "
              "are the same address. Nil compares equal to nil only.") {
    bender_sandbox_assert(BENDER_SANDBOX_FFI_USE);
    bender_arity(argc, 2, 2);
    void *a = bender_checktype(argv[0], BENDER_NIL) ? NULL : bender_getpointer(argv, 0);
    void *b = bender_checktype(argv[1], BENDER_NIL) ? NULL : bender_getpointer(argv, 1);
    return bender_wrap_boolean(a == b);
}

BENDER_CORE_FN(cfun_ffi_poke,
              "(unsafe/poke ffi-type ptr value &opt offset)",
              "Write `value` through raw pointer `ptr` (+ byte `offset`) "
              "as `ffi-type`. The inverse of `ffi/read` on pointers: no "
              "bounds checking, misaligned or wild writes corrupt memory. "
              "Returns nil.") {
    bender_sandbox_assert(BENDER_SANDBOX_FFI_USE);
    bender_arity(argc, 3, 4);
    BenderFFIType type = decode_ffi_type(argv[0]);
    uint8_t *ptr = bender_getpointer(argv, 1);
    size_t offset = (size_t) bender_optnat(argv, argc, 3, 0);
    bender_ffi_write_one((char *) ptr + offset, argv, 2, type, BENDER_FFI_MAX_RECUR);
    return bender_wrap_nil();
}

BENDER_CORE_FN(cfun_ffi_calloc,
              "(unsafe/calloc nitems size)",
              "Allocate `nitems` * `size` zeroed bytes (C calloc). Must be "
              "released with `unsafe/free`. Returns a raw pointer.") {
    bender_sandbox_assert(BENDER_SANDBOX_FFI_USE);
    bender_arity(argc, 2, 2);
    size_t nitems = bender_getsize(argv, 0);
    size_t size = bender_getsize(argv, 1);
    void *p = bender_calloc(nitems, size);
    if (NULL == p && nitems != 0 && size != 0) BENDER_OUT_OF_MEMORY;
    return bender_wrap_pointer(p);
}

BENDER_CORE_FN(cfun_ffi_realloc,
              "(unsafe/realloc ptr size)",
              "Resize a `unsafe/malloc` (or `unsafe/calloc`, `unsafe/realloc`) "
              "block to `size` bytes, preserving contents. A nil `ptr` "
              "allocates fresh; the old pointer is invalid afterwards. "
              "Release with `unsafe/free`. Returns the (possibly moved) pointer.") {
    bender_sandbox_assert(BENDER_SANDBOX_FFI_USE);
    bender_arity(argc, 2, 2);
    void *pointer = bender_checktype(argv[0], BENDER_NIL)
        ? NULL : bender_getpointer(argv, 0);
    size_t size = bender_getsize(argv, 1);
    void *p = bender_realloc(pointer, size);
    if (NULL == p && size != 0) BENDER_OUT_OF_MEMORY;
    return bender_wrap_pointer(p);
}

BENDER_CORE_FN(cfun_ffi_exec_alloc,
              "(unsafe/exec-alloc size)",
              "Map at least `size` bytes of read/write (not yet executable) "
              "memory for hand-rolled JITs. Write machine code into it "
              "(e.g. via `unsafe/poke` on a `unsafe/pointer-buffer`), then flip it "
              "executable with `unsafe/exec-protect` and call it through "
              "`ffi/call` or `unsafe/pointer-cfunction`. There is no free: "
              "mappings live until process exit. Returns a raw pointer.") {
    bender_sandbox_assert(BENDER_SANDBOX_FFI_JIT);
    bender_fixarity(argc, 1);
    size_t size = bender_getsize(argv, 0);
    size_t alloc_size = (size + FFI_PAGE_MASK) & ~FFI_PAGE_MASK;
    if (alloc_size == 0) alloc_size = FFI_PAGE_MASK + 1;
#ifdef BENDER_WINDOWS
    void *ptr = VirtualAlloc(NULL, alloc_size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (NULL == ptr) bender_panic("failed to allocate writable memory");
#else
#ifdef MAP_ANONYMOUS
    void *ptr = mmap(0, alloc_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
#elif defined(MAP_ANON)
    void *ptr = mmap(0, alloc_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
#else
    void *ptr = mmap(0, alloc_size, PROT_READ | PROT_WRITE, MAP_PRIVATE, -1, 0);
#endif
    if (ptr == MAP_FAILED) bender_panic("failed to memory map writable memory");
#endif
    return bender_wrap_pointer(ptr);
}

BENDER_CORE_FN(cfun_ffi_exec_protect,
              "(unsafe/exec-protect ptr size)",
              "Flip a `unsafe/exec-alloc` mapping (rounded up to whole pages "
              "like the allocator) to read/execute. Call only after all "
              "writes are done; writing afterwards faults. Returns nil.") {
    bender_sandbox_assert(BENDER_SANDBOX_FFI_JIT);
    bender_arity(argc, 2, 2);
    void *ptr = bender_getpointer(argv, 0);
    size_t size = bender_getsize(argv, 1);
    size_t alloc_size = (size + FFI_PAGE_MASK) & ~FFI_PAGE_MASK;
    if (alloc_size == 0) alloc_size = FFI_PAGE_MASK + 1;
#ifdef BENDER_WINDOWS
    {
        DWORD old = 0;
        if (!VirtualProtect(ptr, alloc_size, PAGE_EXECUTE_READ, &old))
            bender_panic("failed to make mapped memory executable");
    }
#else
    if (mprotect(ptr, alloc_size, PROT_READ | PROT_EXEC) == -1)
        bender_panic("failed to make mapped memory executable");
#endif
    return bender_wrap_nil();
}

BENDER_CORE_FN(cfun_ffi_pointer_buffer,
              "(unsafe/pointer-buffer ptr capacity &opt n offset)",
              "Create a buffer from a pointer `ptr`. The underlying memory "
              "of the buffer will not be reallocated or freed by the "
              "garbage collector, allowing unmanaged, mutable memory to be "
              "manipulated with buffer functions. Attempts to resize or "
              "extend the buffer beyond its initial capacity will raise an "
              "error. As with many FFI functions, this is memory unsafe and "
              "can potentially allow out of bounds memory access. Returns a "
              "new buffer.") {
    bender_sandbox_assert(BENDER_SANDBOX_FFI_USE);
    bender_arity(argc, 2, 4);
    void *pointer = bender_getpointer(argv, 0);
    int32_t capacity = bender_getnat(argv, 1);
    int32_t count = bender_optnat(argv, argc, 2, 0);
    int64_t offset = bender_optinteger64(argv, argc, 3, 0);
    uint8_t *offset_pointer = ((uint8_t *) pointer) + offset;
    return bender_wrap_buffer(bender_pointer_buffer_unsafe(offset_pointer, capacity, count));
}

BENDER_CORE_FN(cfun_ffi_pointer_cfunction,
              "(unsafe/pointer-cfunction ptr &opt name source line)",
              "Create a C Function from a raw pointer `ptr`. Optionally "
              "give the cfunction a name and source location for stack "
              "traces and debugging.") {
    bender_sandbox_assert(BENDER_SANDBOX_FFI_USE);
    bender_arity(argc, 1, 4);
    void *pointer = bender_getpointer(argv, 0);
    const char *name = bender_optcstring(argv, argc, 1, NULL);
    const char *source = bender_optcstring(argv, argc, 2, NULL);
    int32_t line = bender_optinteger(argv, argc, 3, -1);
    if ((name != NULL) || (source != NULL) || (line != -1)) {
        bender_registry_put((BenderCFunction) pointer, name, NULL, source, line);
    }
    return bender_wrap_cfunction((BenderCFunction) pointer);
}

BENDER_CORE_FN(cfun_ffi_supported_calling_conventions,
              "(ffi/calling-conventions)",
              "Get an array of all supported calling conventions on the "
              "current architecture. Some architectures may have some FFI "
              "functionality (`unsafe/malloc`, `unsafe/free`, `unsafe/read`, "
              "`ffi/write`, etc.) but not support any calling conventions. "
              "This function can be used to get all supported calling "
              "conventions that can be used on this architecture. All "
              "architectures support the `:none` calling convention which "
              "is a placeholder that cannot be used at runtime.") {
    bender_fixarity(argc, 0);
    (void) argv;
    BenderArray *array = bender_array(4);
#ifdef BENDER_FFI_WIN64_ENABLED
    bender_array_push(array, bender_ckeywordv("win64"));
#endif
#ifdef BENDER_FFI_SYSV64_ENABLED
    bender_array_push(array, bender_ckeywordv("sysv64"));
#endif
#ifdef BENDER_FFI_AAPCS64_ENABLED
    bender_array_push(array, bender_ckeywordv("aapcs64"));
#endif
    bender_array_push(array, bender_ckeywordv("none"));
    return bender_wrap_array(array);
}

void bender_lib_ffi(BenderTable *env) {
    BenderRegExt ffi_cfuns[] = {
        BENDER_CORE_REG("ffi/native", bender_core_raw_native),
        BENDER_CORE_REG("ffi/lookup", bender_core_native_lookup),
        BENDER_CORE_REG("ffi/close", bender_core_native_close),
        BENDER_CORE_REG("ffi/signature", cfun_ffi_signature),
        BENDER_CORE_REG("ffi/call", cfun_ffi_call),
        BENDER_CORE_REG("ffi/struct", cfun_ffi_struct),
        BENDER_CORE_REG("ffi/write", cfun_ffi_buffer_write),
        BENDER_CORE_REG("ffi/read", cfun_ffi_buffer_read),
        BENDER_CORE_REG("ffi/size", cfun_ffi_size),
        BENDER_CORE_REG("ffi/align", cfun_ffi_align),
        BENDER_CORE_REG("ffi/struct-info", cfun_ffi_struct_info),
        BENDER_CORE_REG("unsafe/trampoline", cfun_ffi_get_callback_trampoline),
        BENDER_CORE_REG("unsafe/jitfn", cfun_ffi_jitfn),
        BENDER_CORE_REG("unsafe/malloc", cfun_ffi_malloc),
        BENDER_CORE_REG("unsafe/free", cfun_ffi_free),
        BENDER_CORE_REG("unsafe/calloc", cfun_ffi_calloc),
        BENDER_CORE_REG("unsafe/realloc", cfun_ffi_realloc),
        BENDER_CORE_REG("unsafe/ptr-add", cfun_ffi_ptr_add),
        BENDER_CORE_REG("unsafe/ptr-sub", cfun_ffi_ptr_sub),
        BENDER_CORE_REG("unsafe/ptr-eq", cfun_ffi_ptr_eq),
        BENDER_CORE_REG("unsafe/poke", cfun_ffi_poke),
        BENDER_CORE_REG("unsafe/exec-alloc", cfun_ffi_exec_alloc),
        BENDER_CORE_REG("unsafe/exec-protect", cfun_ffi_exec_protect),
        BENDER_CORE_REG("unsafe/pointer-buffer", cfun_ffi_pointer_buffer),
        BENDER_CORE_REG("unsafe/pointer-cfunction", cfun_ffi_pointer_cfunction),
        BENDER_CORE_REG("ffi/calling-conventions", cfun_ffi_supported_calling_conventions),
        BENDER_REG_END
    };
    bender_core_cfuns_ext(env, NULL, ffi_cfuns);
    /* Registered so ffi-struct descriptors resolve by name when
     * unmarshalling (AOT images, message passing). */
    bender_register_abstract_type(&bender_struct_type);
}

#endif
