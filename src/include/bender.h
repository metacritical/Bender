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

#include "benderconf.h"

#ifndef BENDER_H_defined
#define BENDER_H_defined

#ifdef __cplusplus
extern "C" {
#endif

/* Variable length arrays are ok */
#ifdef _MSC_VER
#pragma warning( push )
#pragma warning( disable : 4200 )
#endif

/***** START SECTION CONFIG *****/

#ifndef BENDER_VERSION
#define BENDER_VERSION "latest"
#endif

#ifndef BENDER_BUILD
#define BENDER_BUILD "local"
#endif

/*
 * Detect OS and endianness.
 * From webkit source. There is likely some extreneous
 * detection for unsupported platforms
 */

/* Check for any flavor of BSD (except apple) */
#if defined(__FreeBSD__) || defined(__DragonFly__) || \
    defined(__NetBSD__) || defined(__OpenBSD__)
#define BENDER_BSD 1
#endif

/* Check for macOS or OS X */
#if defined(__APPLE__) && defined(__MACH__)
#define BENDER_APPLE 1
#endif

/* Check for Linux */
#ifdef __linux__
#define BENDER_LINUX 1
#endif

/* Check for GNU Hurd */
#ifdef __gnu_hurd__
#define BENDER_GNU_HURD 1
#endif

/* Check for Android */
#ifdef __ANDROID__
#define BENDER_ANDROID 1
#endif

/* Check for Cygwin */
#if defined(__CYGWIN__)
#define BENDER_CYGWIN 1
#endif

/* Check for Illumos */
#if defined(__illumos__)
#define BENDER_ILLUMOS 1
#endif

/* Check Unix */
#if defined(_AIX) \
    || defined(__APPLE__) /* Darwin */ \
    || defined(__FreeBSD__) || defined(__DragonFly__) \
    || defined(__FreeBSD_kernel__) \
    || defined(__GNU__) /* GNU/Hurd */ \
    || defined(__HAIKU__) \
    || defined(__linux__) \
    || defined(__NetBSD__) \
    || defined(__OpenBSD__) \
    || defined(__QNXNTO__) \
    || defined(sun) || defined(__sun) /* Solaris */ \
    || defined(unix) || defined(__unix) || defined(__unix__)
#define BENDER_POSIX 1
#elif defined(__EMSCRIPTEN__)
#define BENDER_WEB 1
#elif defined(WIN32) || defined(_WIN32)
#define BENDER_WINDOWS 1
#endif

/* Check if compiling with MSVC - else assume a GCC-like compiler by default */
#ifdef _MSC_VER
#define BENDER_MSVC
#endif

/* Check Mingw 32-bit and 64-bit */
#ifdef __MINGW32__
#define BENDER_MINGW
#endif

/* Check 64-bit vs 32-bit */
#if ((defined(__x86_64__) || defined(_M_X64)) \
     && (defined(BENDER_POSIX) || defined(BENDER_WINDOWS))) \
    || (defined(_WIN64)) /* Windows 64 bit */ \
    || (defined(__ia64__) && defined(__LP64__)) /* Itanium in LP64 mode */ \
    || defined(__alpha__) /* DEC Alpha */ \
    || (defined(__sparc__) && defined(__arch64__) || defined (__sparcv9)) /* BE */ \
    || defined(__s390x__) /* S390 64-bit (BE) */ \
    || (defined(__ppc64__) || defined(__PPC64__)) \
    || defined(PLAN9_arm64) || defined(PLAN9_amd64) \
    || defined(__aarch64__) /* ARM 64-bit */ \
    || (defined(__riscv) && (__riscv_xlen == 64)) /* RISC-V 64-bit */ \
    || defined(__loongarch64) /* LoongArch64 64-bit */
#define BENDER_64 1
#else
#define BENDER_32 1
#endif

/* Check big endian */
#if defined(__LITTLE_ENDIAN__) || \
    (defined(__BYTE_ORDER__) && (__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__))
/* If we know the target is LE, always use that - e.g. ppc64 little endian
 * defines the __LITTLE_ENDIAN__ macro in the ABI spec, so we can rely
 * on that and if that's not defined, fall back to big endian assumption
 */
#define BENDER_LITTLE_ENDIAN 1
#elif defined(__MIPSEB__) /* MIPS 32-bit */ \
    || defined(__ppc__) || defined(__PPC__) /* CPU(PPC) - PowerPC 32-bit */ \
    || defined(__powerpc__) || defined(__powerpc) || defined(__POWERPC__) \
    || defined(_M_PPC) || defined(__PPC) \
    || defined(__ppc64__) || defined(__PPC64__) /* PowerPC 64-bit */ \
    || defined(__sparc)   /* Sparc 32bit */  \
    || defined(__sparc__) /* Sparc 64-bit */ \
    || defined(__s390x__) /* S390 64-bit */ \
    || defined(__s390__)  /* S390 32-bit */ \
    || defined(__ARMEB__) /* ARM big endian */ \
    || defined(__AARCH64EB__) /* ARM64 big endian */ \
    || ((defined(__CC_ARM) || defined(__ARMCC__)) /* ARM RealView compiler */ \
        && defined(__BIG_ENDIAN))
#define BENDER_BIG_ENDIAN 1
#else
#define BENDER_LITTLE_ENDIAN 1
#endif

/* Limits for converting doubles to 64 bit integers */
#define BENDER_INTMAX_DOUBLE 9007199254740992.0
#define BENDER_INTMIN_DOUBLE (-9007199254740992.0)
#define BENDER_INTMAX_INT64 9007199254740992
#define BENDER_INTMIN_INT64 (-9007199254740992)

/* Check emscripten */
#ifdef __EMSCRIPTEN__
#define BENDER_NO_DYNAMIC_MODULES
#define BENDER_NO_PROCESSES
#endif

/* Check sun */
#if defined(__sun) && !defined(BENDER_ILLUMOS)
#define BENDER_NO_UTC_MKTIME
#endif

/* Define how global bender state is declared */
/* Also enable the thread library only if not single-threaded */
#ifdef BENDER_SINGLE_THREADED
#define BENDER_THREAD_LOCAL
#elif !(defined(BENDER_THREAD_LOCAL)) && defined(__GNUC__)
#define BENDER_THREAD_LOCAL __thread
#elif !(defined(BENDER_THREAD_LOCAL)) && defined(_MSC_BUILD)
#define BENDER_THREAD_LOCAL __declspec(thread)
#elif !(defined(BENDER_THREAD_LOCAL))
#define BENDER_THREAD_LOCAL
#endif

/* Enable or disable dynamic module loading. Enabled by default. */
#ifndef BENDER_NO_DYNAMIC_MODULES
#define BENDER_DYNAMIC_MODULES
#endif

/* Enable or disable the FFI library. Currently, FFI only enabled on
 * x86-64 operating systems. */
#ifndef BENDER_NO_FFI
#if !defined(__EMSCRIPTEN__)
#define BENDER_FFI
#endif
#endif

/* If FFI is enabled and FFI-JIT is not disabled... */
#ifdef BENDER_FFI
#ifndef BENDER_NO_FFI_JIT
#define BENDER_FFI_JIT
#endif
#endif

/* Enable or disable the assembler. Enabled by default. */
#ifndef BENDER_NO_ASSEMBLER
#define BENDER_ASSEMBLER
#endif

/* Enable or disable the peg module */
#ifndef BENDER_NO_PEG
#define BENDER_PEG
#endif

/* Enable or disable event loop */
#if !defined(BENDER_NO_EV) && !defined(__EMSCRIPTEN__)
#define BENDER_EV
#endif

/* Enable or disable the filewatch/ module */
#if !defined(BENDER_NO_FILEWATCH)
#define BENDER_FILEWATCH
#endif

/* Enable or disable networking */
#if defined(BENDER_EV) && !defined(BENDER_NO_NET) && !defined(__EMSCRIPTEN__)
#define BENDER_NET
#endif

/* Enable or disable large int types (for now 64 bit, maybe 128 / 256 bit integer types) */
#ifndef BENDER_NO_INT_TYPES
#define BENDER_INT_TYPES
#endif

/* Enable or disable epoll on Linux */
#if defined(BENDER_LINUX) && !defined(BENDER_EV_NO_EPOLL)
#define BENDER_EV_EPOLL
#endif

/* Enable or disable kqueue on BSD */
#if defined(BENDER_BSD) && !defined(BENDER_EV_NO_KQUEUE)
#define BENDER_EV_KQUEUE
#endif

/* Enable or disable kqueue on Apple */
#if defined(BENDER_APPLE) && !defined(BENDER_EV_NO_KQUEUE)
#define BENDER_EV_KQUEUE
#endif

/* Use poll as last resort */
#if !defined(BENDER_WINDOWS) && !defined(BENDER_EV_EPOLL) && !defined(BENDER_EV_KQUEUE)
#define BENDER_EV_POLL
#endif

/* How to export symbols */
#ifndef BENDER_EXPORT
#ifdef BENDER_WINDOWS
#define BENDER_EXPORT __declspec(dllexport)
#else
#define BENDER_EXPORT __attribute__((visibility ("default")))
#endif
#endif

/* How declare API functions */
#ifndef BENDER_API
#ifdef BENDER_WINDOWS
#ifdef BENDER_DLL_IMPORT
#define BENDER_API __declspec(dllimport)
#else
#define BENDER_API __declspec(dllexport)
#endif
#else
#define BENDER_API __attribute__((visibility ("default")))
#endif
#endif

/* Tell compiler some functions don't return */
#ifndef BENDER_NO_RETURN
#ifdef BENDER_WINDOWS
#define BENDER_NO_RETURN __declspec(noreturn)
#else
#define BENDER_NO_RETURN __attribute__((noreturn))
#endif
#endif

/* Prevent some recursive functions from recursing too deeply
 * and crashing (the parser). Instead, error out. */
#define BENDER_RECURSION_GUARD 1024

/* Maximum depth to follow table prototypes before giving up and returning nil. */
#define BENDER_MAX_PROTO_DEPTH 200

/* Prevent macros to expand too deeply and error out. */
#define BENDER_MAX_MACRO_EXPAND 200

/* Define default max stack size for stacks before raising a stack overflow error.
 * This can also be set on a per fiber basis. */
#ifndef BENDER_STACK_MAX
#define BENDER_STACK_MAX 0x7fffffff
#endif

/* Use nanboxed values - uses 8 bytes per value instead of 12 or 16.
 * To turn of nanboxing, for debugging purposes or for certain
 * architectures (Nanboxing only tested on x86 and x64), comment out
 * the BENDER_NANBOX define.*/

#ifndef BENDER_NO_NANBOX
#ifdef BENDER_32
#define BENDER_NANBOX_32
#elif defined(__x86_64__) || defined(_WIN64) || defined(__riscv) || defined(__aarch64__) || defined(_M_ARM64)
/* We will only enable nanboxing by default on 64 bit systems
 * for x64, risc-v, and arm64. This is mainly because the approach is tied to the
 * implicit 47 bit address space. Many arches allow/require this, but not all,
 * and it requires cooperation from the OS. ARM should also work in many configurations by taking advantage
 * of pointer alignment to allow for 48 or 49 bits of address space. */
#define BENDER_NANBOX_64

/* Allow 64-bit nanboxing to assume aligned pointers to get back some extra bits for representation.
 * This is needed to use nanboxing on systems with larger than 47-bit address spaces, such as many
 * aarch64 systems. */
#ifndef BENDER_NANBOX_64_POINTER_SHIFT
#if (defined(_M_ARM64) || defined(__aarch64__)) && !defined(BENDER_APPLE)
/* All pointers, including function pointers, should be 4-byte aligned on aarch64 by default.
 * The exception is aarch64 macos, as it uses the same 47-bit userland address-space as on amd64. */
#define BENDER_NANBOX_64_POINTER_SHIFT 2
#endif
#endif
#endif
#endif

/* Allow for custom pointer alignment as well */
#if defined(BENDER_NANBOX_64) && !defined(BENDER_NANBOX_64_POINTER_SHIFT)
#define BENDER_NANBOX_64_POINTER_SHIFT 0
#endif

/* Runtime config constants */
#ifdef BENDER_NO_NANBOX
#define BENDER_NANBOX_BIT 0x0
#else
#define BENDER_NANBOX_BIT 0x1
#endif

#ifdef BENDER_SINGLE_THREADED
#define BENDER_SINGLE_THREADED_BIT 0x2
#else
#define BENDER_SINGLE_THREADED_BIT 0
#endif

#ifdef BENDER_NANBOX_64_POINTER_SHIFT
#define BENDER_NANBOX_POINTER_SHIFT_BITS (BENDER_NANBOX_64_POINTER_SHIFT ? (0x4 << BENDER_NANBOX_64_POINTER_SHIFT) : 0)
#else
#define BENDER_NANBOX_POINTER_SHIFT_BITS 0
#endif

#define BENDER_CURRENT_CONFIG_BITS \
    (BENDER_SINGLE_THREADED_BIT | \
     BENDER_NANBOX_BIT | \
     BENDER_NANBOX_POINTER_SHIFT_BITS)

/* Represents the settings used to compile Bender, as well as the version */
typedef struct {
    unsigned major;
    unsigned minor;
    unsigned patch;
    unsigned bits;
} BenderBuildConfig;

/* Get config of current compilation unit. */
#ifdef __cplusplus
/* C++11 syntax */
#define bender_config_current() (BenderBuildConfig { \
    BENDER_VERSION_MAJOR, \
    BENDER_VERSION_MINOR, \
    BENDER_VERSION_PATCH, \
    BENDER_CURRENT_CONFIG_BITS })
#else
/* C99 syntax */
#define bender_config_current() ((BenderBuildConfig){ \
    BENDER_VERSION_MAJOR, \
    BENDER_VERSION_MINOR, \
    BENDER_VERSION_PATCH, \
    BENDER_CURRENT_CONFIG_BITS })
#endif

/* Some extra includes if EV is enabled */
#ifdef BENDER_EV
typedef struct BenderOSMutex BenderOSMutex;
typedef struct BenderOSRWLock BenderOSRWLock;
typedef struct BenderChannel BenderChannel;
#endif

/***** END SECTION CONFIG *****/

/***** START SECTION TYPES *****/

#ifdef BENDER_WINDOWS
/* Must be defined before including stdlib.h */
#define _CRT_RAND_S
#endif

#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdarg.h>
#include <setjmp.h>
#include <stddef.h>
#include <stdio.h>

/* What to do when out of memory */
#ifndef BENDER_OUT_OF_MEMORY
#define BENDER_OUT_OF_MEMORY do { fprintf(stderr, "%s:%d - bender out of memory\n", __FILE__, __LINE__); exit(1); } while (0)
#endif

#ifdef BENDER_BSD
int _setjmp(jmp_buf);
BENDER_NO_RETURN void _longjmp(jmp_buf, int);
#endif

/* Names of all of the types */
BENDER_API extern const char *const bender_type_names[16];
BENDER_API extern const char *const bender_signal_names[14];
BENDER_API extern const char *const bender_status_names[16];

/* For various IO routines, we want to use an int on posix and HANDLE on windows */
#ifdef BENDER_WINDOWS
typedef void *BenderHandle;
#define BENDER_HANDLE_NONE NULL
#else
typedef int BenderHandle;
#define BENDER_HANDLE_NONE (-1)
#endif

/* Fiber signals */
typedef enum {
    BENDER_SIGNAL_OK,
    BENDER_SIGNAL_ERROR,
    BENDER_SIGNAL_DEBUG,
    BENDER_SIGNAL_YIELD,
    BENDER_SIGNAL_USER0,
    BENDER_SIGNAL_USER1,
    BENDER_SIGNAL_USER2,
    BENDER_SIGNAL_USER3,
    BENDER_SIGNAL_USER4,
    BENDER_SIGNAL_USER5,
    BENDER_SIGNAL_USER6,
    BENDER_SIGNAL_USER7,
    BENDER_SIGNAL_USER8,
    BENDER_SIGNAL_USER9,
    BENDER_SIGNAL_INTERRUPT = BENDER_SIGNAL_USER8,
    BENDER_SIGNAL_EVENT = BENDER_SIGNAL_USER9,
} BenderSignal;

/* Fiber statuses - mostly corresponds to signals. */
typedef enum {
    BENDER_STATUS_DEAD,
    BENDER_STATUS_ERROR,
    BENDER_STATUS_DEBUG,
    BENDER_STATUS_PENDING,
    BENDER_STATUS_USER0,
    BENDER_STATUS_USER1,
    BENDER_STATUS_USER2,
    BENDER_STATUS_USER3,
    BENDER_STATUS_USER4,
    BENDER_STATUS_USER5,
    BENDER_STATUS_USER6,
    BENDER_STATUS_USER7,
    BENDER_STATUS_USER8,
    BENDER_STATUS_USER9,
    BENDER_STATUS_NEW,
    BENDER_STATUS_ALIVE
} BenderFiberStatus;

/* For encapsulating all thread-local Bender state (except natives) */
typedef struct BenderVM BenderVM;

/* Use type punning for GC objects */
typedef struct BenderGCObject BenderGCObject;

/* All of the primary Bender GCed types */
typedef struct BenderFunction BenderFunction;
typedef struct BenderArray BenderArray;
typedef struct BenderBuffer BenderBuffer;
typedef struct BenderTable BenderTable;
typedef struct BenderFiber BenderFiber;

/* Prefixed Bender types */
typedef struct BenderTupleHead BenderTupleHead;
typedef struct BenderStructHead BenderStructHead;
typedef struct BenderStringHead BenderStringHead;
typedef struct BenderAbstractHead BenderAbstractHead;

/* Other structs */
typedef struct BenderFuncDef BenderFuncDef;
typedef struct BenderFuncEnv BenderFuncEnv;
typedef struct BenderKV BenderKV;
typedef struct BenderStackFrame BenderStackFrame;
typedef struct BenderAbstractType BenderAbstractType;
typedef struct BenderReg BenderReg;
typedef struct BenderRegExt BenderRegExt;
typedef struct BenderMethod BenderMethod;
typedef struct BenderSourceMapping BenderSourceMapping;
typedef struct BenderSymbolMap BenderSymbolMap;
typedef struct BenderView BenderView;
typedef struct BenderByteView BenderByteView;
typedef struct BenderDictView BenderDictView;
typedef struct BenderRange BenderRange;
typedef struct BenderRNG BenderRNG;

/* Basic types for all Bender Values */
typedef enum BenderType {
    BENDER_NUMBER,
    BENDER_NIL,
    BENDER_BOOLEAN,
    BENDER_FIBER,
    BENDER_STRING,
    BENDER_SYMBOL,
    BENDER_KEYWORD,
    BENDER_ARRAY,
    BENDER_TUPLE,
    BENDER_TABLE,
    BENDER_STRUCT,
    BENDER_BUFFER,
    BENDER_FUNCTION,
    BENDER_CFUNCTION,
    BENDER_ABSTRACT,
    BENDER_POINTER
} BenderType;

/* Recursive type (Bender) */
#ifdef BENDER_NANBOX_64
typedef union Bender Bender;
union Bender {
    uint64_t u64;
    int64_t i64;
    double number;
    void *pointer;
};
#elif defined(BENDER_NANBOX_32)
typedef union Bender Bender;
union Bender {
    struct {
#ifdef BENDER_BIG_ENDIAN
        uint32_t type;
        union {
            int32_t integer;
            void *pointer;
        } payload;
#else
        union {
            int32_t integer;
            void *pointer;
        } payload;
        uint32_t type;
#endif
    } tagged;
    double number;
    uint64_t u64;
};
#else
typedef struct Bender Bender;
struct Bender {
    union {
        uint64_t u64;
        double number;
        int32_t integer;
        void *pointer;
        const void *cpointer;
    } as;
    BenderType type;
};
#endif

/* C functions */
typedef Bender(*BenderCFunction)(int32_t argc, Bender *argv);

/* String and other aliased pointer types */
typedef const uint8_t *BenderString;
typedef const uint8_t *BenderSymbol;
typedef const uint8_t *BenderKeyword;
typedef const Bender *BenderTuple;
typedef const BenderKV *BenderStruct;
typedef void *BenderAbstract;

#define BENDER_COUNT_TYPES (BENDER_POINTER + 1)

/* Type flags */
#define BENDER_TFLAG_NIL (1 << BENDER_NIL)
#define BENDER_TFLAG_BOOLEAN (1 << BENDER_BOOLEAN)
#define BENDER_TFLAG_FIBER (1 << BENDER_FIBER)
#define BENDER_TFLAG_NUMBER (1 << BENDER_NUMBER)
#define BENDER_TFLAG_STRING (1 << BENDER_STRING)
#define BENDER_TFLAG_SYMBOL (1 << BENDER_SYMBOL)
#define BENDER_TFLAG_KEYWORD (1 << BENDER_KEYWORD)
#define BENDER_TFLAG_ARRAY (1 << BENDER_ARRAY)
#define BENDER_TFLAG_TUPLE (1 << BENDER_TUPLE)
#define BENDER_TFLAG_TABLE (1 << BENDER_TABLE)
#define BENDER_TFLAG_STRUCT (1 << BENDER_STRUCT)
#define BENDER_TFLAG_BUFFER (1 << BENDER_BUFFER)
#define BENDER_TFLAG_FUNCTION (1 << BENDER_FUNCTION)
#define BENDER_TFLAG_CFUNCTION (1 << BENDER_CFUNCTION)
#define BENDER_TFLAG_ABSTRACT (1 << BENDER_ABSTRACT)
#define BENDER_TFLAG_POINTER (1 << BENDER_POINTER)

#define BENDER_TFLAG_BYTES (BENDER_TFLAG_STRING | BENDER_TFLAG_SYMBOL | BENDER_TFLAG_BUFFER | BENDER_TFLAG_KEYWORD)
#define BENDER_TFLAG_INDEXED (BENDER_TFLAG_ARRAY | BENDER_TFLAG_TUPLE)
#define BENDER_TFLAG_DICTIONARY (BENDER_TFLAG_TABLE | BENDER_TFLAG_STRUCT)
#define BENDER_TFLAG_LENGTHABLE (BENDER_TFLAG_BYTES | BENDER_TFLAG_INDEXED | BENDER_TFLAG_DICTIONARY)
#define BENDER_TFLAG_CALLABLE (BENDER_TFLAG_FUNCTION | BENDER_TFLAG_CFUNCTION | \
        BENDER_TFLAG_LENGTHABLE | BENDER_TFLAG_ABSTRACT)

/* Event Loop Types */
#ifdef BENDER_EV

#define BENDER_STREAM_CLOSED 0x1
#define BENDER_STREAM_SOCKET 0x2
#define BENDER_STREAM_UNREGISTERED 0x4
#define BENDER_STREAM_READABLE 0x200
#define BENDER_STREAM_WRITABLE 0x400
#define BENDER_STREAM_ACCEPTABLE 0x800
#define BENDER_STREAM_UDPSERVER 0x1000
#define BENDER_STREAM_NOT_CLOSEABLE 0x2000
#define BENDER_STREAM_TOCLOSE 0x10000
#define BENDER_STREAM_NODUPS 0x20000

typedef enum {
    BENDER_ASYNC_EVENT_INIT = 0,
    BENDER_ASYNC_EVENT_MARK = 1,
    BENDER_ASYNC_EVENT_DEINIT = 2,
    BENDER_ASYNC_EVENT_CLOSE = 3,
    BENDER_ASYNC_EVENT_ERR = 4,
    BENDER_ASYNC_EVENT_HUP = 5,
    BENDER_ASYNC_EVENT_READ = 6,
    BENDER_ASYNC_EVENT_WRITE = 7,
    BENDER_ASYNC_EVENT_COMPLETE = 8, /* Used on windows for IOCP */
    BENDER_ASYNC_EVENT_FAILED = 9 /* Used on windows for IOCP */
} BenderAsyncEvent;

typedef enum {
    BENDER_ASYNC_LISTEN_READ = 1,
    BENDER_ASYNC_LISTEN_WRITE,
    BENDER_ASYNC_LISTEN_BOTH
} BenderAsyncMode;

typedef struct BenderStream BenderStream;

/* Wrapper around file descriptors and HANDLEs that can be polled. */
struct BenderStream {
    BenderHandle handle;
    uint32_t flags;
    uint32_t index;
    BenderFiber *read_fiber;
    BenderFiber *write_fiber;
    const void *methods; /* Methods for this stream */
};

typedef void (*BenderEVCallback)(BenderFiber *fiber, BenderAsyncEvent event);

/* Start listening for events from a stream on the current root fiber. After
 * calling this, users should call bender_await() before returning from the
 * current C Function. This also will call bender_await.
 * mode is which events to listen for, and callback is the function pointer to
 * call when ever an event is sent from the event loop. state is an optional (can be NULL)
 * pointer to data allocated with bender_malloc. This pointer will be passed to callback as
 * fiber->ev_state. It will also be freed for you by the runtime when the event loop determines
 * it can no longer be referenced. On windows, the contents of state MUST contained an OVERLAPPED struct at the 0 offset. */

BENDER_API void bender_async_start_fiber(BenderFiber *fiber, BenderStream *stream, BenderAsyncMode mode, BenderEVCallback callback, void *state);
BENDER_API BENDER_NO_RETURN void bender_async_start(BenderStream *stream, BenderAsyncMode mode, BenderEVCallback callback, void *state);

/* Do not send any more events to the given callback. Call this after scheduling fiber to be resume
 * or canceled. */
BENDER_API void bender_async_end(BenderFiber *fiber);

/* Needed for windows to mark a fiber as waiting for an IOCP completion event. Noop on other platforms. */
BENDER_API void bender_async_in_flight(BenderFiber *fiber);

/* On some platforms, it is important to be able to control if a stream is edge-trigger or level triggered.
 * For example, a server that is accepting connections might want to be level triggered or edge-triggered
 * depending on expected service. */
BENDER_API void bender_stream_edge_triggered(BenderStream *stream);
BENDER_API void bender_stream_level_triggered(BenderStream *stream);

#endif

/* Bender uses atomic integers in several places for synchronization between threads and
 * signals. Define them here */
#ifdef BENDER_WINDOWS
typedef long BenderAtomicInt;
#elif defined(BENDER_PLAN9)
typedef long BenderAtomicInt;
#else
typedef int32_t BenderAtomicInt;
#endif
BENDER_API BenderAtomicInt bender_atomic_inc(BenderAtomicInt volatile *x);
BENDER_API BenderAtomicInt bender_atomic_dec(BenderAtomicInt volatile *x);
BENDER_API BenderAtomicInt bender_atomic_load(BenderAtomicInt volatile *x);
BENDER_API BenderAtomicInt bender_atomic_load_relaxed(BenderAtomicInt volatile *x);

/* We provide three possible implementations of Benders. The preferred
 * nanboxing approach, for 32 or 64 bits, and the standard C version. Code in the rest of the
 * application must interact through exposed interface. */

/* Required interface for Bender */
/* wrap and unwrap for all types */
/* Get type quickly */
/* Check against type quickly */
/* Small footprint */
/* 32 bit integer support */

/* bender_type(x)
 * bender_checktype(x, t)
 * bender_wrap_##TYPE(x)
 * bender_unwrap_##TYPE(x)
 * bender_truthy(x)
 * bender_memclear(p, n) - clear memory for hash tables to nils
 * bender_u64(x) - get 64 bits of payload for hashing
 */

/***** START SECTION NON-C API *****/

/* Some bender types use offset tricks to make operations easier in C. For
 * external bindings, we should prefer using the Head structs directly, and
 * use the host language to add sugar around the manipulation of the Bender types. */

BENDER_API BenderStructHead *bender_struct_head(BenderStruct st);
BENDER_API BenderAbstractHead *bender_abstract_head(const void *abstract);
BENDER_API BenderStringHead *bender_string_head(BenderString s);
BENDER_API BenderTupleHead *bender_tuple_head(BenderTuple tuple);

/* Some language bindings won't have access to the macro versions. */

BENDER_API BenderType bender_type(Bender x);
BENDER_API int bender_checktype(Bender x, BenderType type);
BENDER_API int bender_checktypes(Bender x, int typeflags);
BENDER_API int bender_truthy(Bender x);

BENDER_API BenderStruct bender_unwrap_struct(Bender x);
BENDER_API BenderTuple bender_unwrap_tuple(Bender x);
BENDER_API BenderFiber *bender_unwrap_fiber(Bender x);
BENDER_API BenderArray *bender_unwrap_array(Bender x);
BENDER_API BenderTable *bender_unwrap_table(Bender x);
BENDER_API BenderBuffer *bender_unwrap_buffer(Bender x);
BENDER_API BenderString bender_unwrap_string(Bender x);
BENDER_API BenderSymbol bender_unwrap_symbol(Bender x);
BENDER_API BenderKeyword bender_unwrap_keyword(Bender x);
BENDER_API BenderAbstract bender_unwrap_abstract(Bender x);
BENDER_API void *bender_unwrap_pointer(Bender x);
BENDER_API BenderFunction *bender_unwrap_function(Bender x);
BENDER_API BenderCFunction bender_unwrap_cfunction(Bender x);
BENDER_API int bender_unwrap_boolean(Bender x);
BENDER_API double bender_unwrap_number(Bender x);
BENDER_API int32_t bender_unwrap_integer(Bender x);

BENDER_API Bender bender_wrap_nil(void);
BENDER_API Bender bender_wrap_number(double x);
BENDER_API Bender bender_wrap_true(void);
BENDER_API Bender bender_wrap_false(void);
BENDER_API Bender bender_wrap_boolean(int x);
BENDER_API Bender bender_wrap_string(BenderString x);
BENDER_API Bender bender_wrap_symbol(BenderSymbol x);
BENDER_API Bender bender_wrap_keyword(BenderKeyword x);
BENDER_API Bender bender_wrap_array(BenderArray *x);
BENDER_API Bender bender_wrap_tuple(BenderTuple x);
BENDER_API Bender bender_wrap_struct(BenderStruct x);
BENDER_API Bender bender_wrap_fiber(BenderFiber *x);
BENDER_API Bender bender_wrap_buffer(BenderBuffer *x);
BENDER_API Bender bender_wrap_function(BenderFunction *x);
BENDER_API Bender bender_wrap_cfunction(BenderCFunction x);
BENDER_API Bender bender_wrap_table(BenderTable *x);
BENDER_API Bender bender_wrap_abstract(BenderAbstract x);
BENDER_API Bender bender_wrap_pointer(void *x);
BENDER_API Bender bender_wrap_integer(int32_t x);

/***** END SECTION NON-C API *****/

#ifdef BENDER_NANBOX_64

#include <math.h>

#define bender_u64(x) ((x).u64)

#define BENDER_NANBOX_TAGBITS     0xFFFF800000000000llu
#define BENDER_NANBOX_PAYLOADBITS 0x00007FFFFFFFFFFFllu
#define bender_nanbox_lowtag(type) ((uint64_t)(type) | 0x1FFF0)
#define bender_nanbox_tag(type) (bender_nanbox_lowtag(type) << 47)
#define bender_type(x) \
    (isnan((x).number) \
        ? (BenderType) (((x).u64 >> 47) & 0xF) \
        : BENDER_NUMBER)

#define bender_nanbox_checkauxtype(x, type) \
    (((x).u64 & BENDER_NANBOX_TAGBITS) == bender_nanbox_tag((type)))

#define bender_nanbox_isnumber(x) \
    (!isnan((x).number) || ((((x).u64 >> 47) & 0xF) == BENDER_NUMBER))

#define bender_checktype(x, t) \
    (((t) == BENDER_NUMBER) \
        ? bender_nanbox_isnumber(x) \
        : bender_nanbox_checkauxtype((x), (t)))

/* Use BENDER_API so that modules will use a local version of these functions if possible */
BENDER_API void *bender_nanbox_to_pointer(Bender x);
BENDER_API Bender bender_nanbox_from_pointer(void *p, uint64_t tagmask);
BENDER_API Bender bender_nanbox_from_cpointer(const void *p, uint64_t tagmask);
BENDER_API Bender bender_nanbox_from_double(double d);
BENDER_API Bender bender_nanbox_from_bits(uint64_t bits);

#define bender_truthy(x) \
    (!bender_checktype((x), BENDER_NIL) && \
     (!bender_checktype((x), BENDER_BOOLEAN) || ((x).u64 & 0x1)))

#define bender_nanbox_from_payload(t, p) \
    bender_nanbox_from_bits(bender_nanbox_tag(t) | (p))

#define bender_nanbox_wrap_(p, t) \
    bender_nanbox_from_pointer((p), bender_nanbox_tag(t))

#define bender_nanbox_wrap_c(p, t) \
    bender_nanbox_from_cpointer((p), bender_nanbox_tag(t))

/* Wrap the simple types */
#define bender_wrap_nil() bender_nanbox_from_payload(BENDER_NIL, 1)
#define bender_wrap_true() bender_nanbox_from_payload(BENDER_BOOLEAN, 1)
#define bender_wrap_false() bender_nanbox_from_payload(BENDER_BOOLEAN, 0)
#define bender_wrap_boolean(b) bender_nanbox_from_payload(BENDER_BOOLEAN, !!(b))
#define bender_wrap_number(r) bender_nanbox_from_double(r)

/* Unwrap the simple types */
#define bender_unwrap_boolean(x) ((x).u64 & 0x1)
#define bender_unwrap_number(x) ((x).number)

/* Wrap the pointer types */
#define bender_wrap_struct(s) bender_nanbox_wrap_c((s), BENDER_STRUCT)
#define bender_wrap_tuple(s) bender_nanbox_wrap_c((s), BENDER_TUPLE)
#define bender_wrap_fiber(s) bender_nanbox_wrap_((s), BENDER_FIBER)
#define bender_wrap_array(s) bender_nanbox_wrap_((s), BENDER_ARRAY)
#define bender_wrap_table(s) bender_nanbox_wrap_((s), BENDER_TABLE)
#define bender_wrap_buffer(s) bender_nanbox_wrap_((s), BENDER_BUFFER)
#define bender_wrap_string(s) bender_nanbox_wrap_c((s), BENDER_STRING)
#define bender_wrap_symbol(s) bender_nanbox_wrap_c((s), BENDER_SYMBOL)
#define bender_wrap_keyword(s) bender_nanbox_wrap_c((s), BENDER_KEYWORD)
#define bender_wrap_abstract(s) bender_nanbox_wrap_((s), BENDER_ABSTRACT)
#define bender_wrap_function(s) bender_nanbox_wrap_((s), BENDER_FUNCTION)
#define bender_wrap_cfunction(s) bender_nanbox_wrap_((s), BENDER_CFUNCTION)
#define bender_wrap_pointer(s) bender_nanbox_wrap_((s), BENDER_POINTER)

/* Unwrap the pointer types */
#define bender_unwrap_struct(x) ((BenderStruct)bender_nanbox_to_pointer(x))
#define bender_unwrap_tuple(x) ((BenderTuple)bender_nanbox_to_pointer(x))
#define bender_unwrap_fiber(x) ((BenderFiber *)bender_nanbox_to_pointer(x))
#define bender_unwrap_array(x) ((BenderArray *)bender_nanbox_to_pointer(x))
#define bender_unwrap_table(x) ((BenderTable *)bender_nanbox_to_pointer(x))
#define bender_unwrap_buffer(x) ((BenderBuffer *)bender_nanbox_to_pointer(x))
#define bender_unwrap_string(x) ((BenderString)bender_nanbox_to_pointer(x))
#define bender_unwrap_symbol(x) ((BenderSymbol)bender_nanbox_to_pointer(x))
#define bender_unwrap_keyword(x) ((const uint8_t *)bender_nanbox_to_pointer(x))
#define bender_unwrap_abstract(x) (bender_nanbox_to_pointer(x))
#define bender_unwrap_pointer(x) (bender_nanbox_to_pointer(x))
#define bender_unwrap_function(x) ((BenderFunction *)bender_nanbox_to_pointer(x))
#define bender_unwrap_cfunction(x) ((BenderCFunction)bender_nanbox_to_pointer(x))

#elif defined(BENDER_NANBOX_32)

#define BENDER_DOUBLE_OFFSET 0xFFFF

#define bender_u64(x) ((x).u64)
#define bender_type(x) (((x).tagged.type < BENDER_DOUBLE_OFFSET) ? (BenderType)((x).tagged.type) : BENDER_NUMBER)
#define bender_checktype(x, t) ((t) == BENDER_NUMBER \
        ? (x).tagged.type >= BENDER_DOUBLE_OFFSET \
        : (x).tagged.type == (t))
#define bender_truthy(x) \
    ((x).tagged.type != BENDER_NIL && ((x).tagged.type != BENDER_BOOLEAN || ((x).tagged.payload.integer & 0x1)))

BENDER_API Bender bender_nanbox32_from_tagi(uint32_t tag, int32_t integer);
BENDER_API Bender bender_nanbox32_from_tagp(uint32_t tag, void *pointer);

#define bender_wrap_nil() bender_nanbox32_from_tagi(BENDER_NIL, 0)
#define bender_wrap_true() bender_nanbox32_from_tagi(BENDER_BOOLEAN, 1)
#define bender_wrap_false() bender_nanbox32_from_tagi(BENDER_BOOLEAN, 0)
#define bender_wrap_boolean(b) bender_nanbox32_from_tagi(BENDER_BOOLEAN, !!(b))

/* Wrap the pointer types */
#define bender_wrap_struct(s) bender_nanbox32_from_tagp(BENDER_STRUCT, (void *)(s))
#define bender_wrap_tuple(s) bender_nanbox32_from_tagp(BENDER_TUPLE, (void *)(s))
#define bender_wrap_fiber(s) bender_nanbox32_from_tagp(BENDER_FIBER, (void *)(s))
#define bender_wrap_array(s) bender_nanbox32_from_tagp(BENDER_ARRAY, (void *)(s))
#define bender_wrap_table(s) bender_nanbox32_from_tagp(BENDER_TABLE, (void *)(s))
#define bender_wrap_buffer(s) bender_nanbox32_from_tagp(BENDER_BUFFER, (void *)(s))
#define bender_wrap_string(s) bender_nanbox32_from_tagp(BENDER_STRING, (void *)(s))
#define bender_wrap_symbol(s) bender_nanbox32_from_tagp(BENDER_SYMBOL, (void *)(s))
#define bender_wrap_keyword(s) bender_nanbox32_from_tagp(BENDER_KEYWORD, (void *)(s))
#define bender_wrap_abstract(s) bender_nanbox32_from_tagp(BENDER_ABSTRACT, (void *)(s))
#define bender_wrap_function(s) bender_nanbox32_from_tagp(BENDER_FUNCTION, (void *)(s))
#define bender_wrap_cfunction(s) bender_nanbox32_from_tagp(BENDER_CFUNCTION, (void *)(s))
#define bender_wrap_pointer(s) bender_nanbox32_from_tagp(BENDER_POINTER, (void *)(s))

#define bender_unwrap_struct(x) ((BenderStruct)(x).tagged.payload.pointer)
#define bender_unwrap_tuple(x) ((BenderTuple)(x).tagged.payload.pointer)
#define bender_unwrap_fiber(x) ((BenderFiber *)(x).tagged.payload.pointer)
#define bender_unwrap_array(x) ((BenderArray *)(x).tagged.payload.pointer)
#define bender_unwrap_table(x) ((BenderTable *)(x).tagged.payload.pointer)
#define bender_unwrap_buffer(x) ((BenderBuffer *)(x).tagged.payload.pointer)
#define bender_unwrap_string(x) ((BenderString)(x).tagged.payload.pointer)
#define bender_unwrap_symbol(x) ((BenderSymbol)(x).tagged.payload.pointer)
#define bender_unwrap_keyword(x) ((BenderKeyword)(x).tagged.payload.pointer)
#define bender_unwrap_abstract(x) ((x).tagged.payload.pointer)
#define bender_unwrap_pointer(x) ((x).tagged.payload.pointer)
#define bender_unwrap_function(x) ((BenderFunction *)(x).tagged.payload.pointer)
#define bender_unwrap_cfunction(x) ((BenderCFunction)(x).tagged.payload.pointer)
#define bender_unwrap_boolean(x) ((x).tagged.payload.integer)

#else

#define bender_u64(x) ((x).as.u64)
#define bender_type(x) ((x).type)
#define bender_checktype(x, t) ((x).type == (t))
#define bender_truthy(x) \
    ((x).type != BENDER_NIL && ((x).type != BENDER_BOOLEAN || ((x).as.u64 & 0x1)))

#define bender_unwrap_struct(x) ((BenderStruct)(x).as.pointer)
#define bender_unwrap_tuple(x) ((BenderTuple)(x).as.pointer)
#define bender_unwrap_fiber(x) ((BenderFiber *)(x).as.pointer)
#define bender_unwrap_array(x) ((BenderArray *)(x).as.pointer)
#define bender_unwrap_table(x) ((BenderTable *)(x).as.pointer)
#define bender_unwrap_buffer(x) ((BenderBuffer *)(x).as.pointer)
#define bender_unwrap_string(x) ((BenderString)(x).as.pointer)
#define bender_unwrap_symbol(x) ((BenderSymbol)(x).as.pointer)
#define bender_unwrap_keyword(x) ((BenderKeyword)(x).as.pointer)
#define bender_unwrap_abstract(x) ((x).as.pointer)
#define bender_unwrap_pointer(x) ((x).as.pointer)
#define bender_unwrap_function(x) ((BenderFunction *)(x).as.pointer)
#define bender_unwrap_cfunction(x) ((BenderCFunction)(x).as.pointer)
#define bender_unwrap_boolean(x) ((x).as.u64 & 0x1)
#define bender_unwrap_number(x) ((x).as.number)

/* End of tagged union implementation */
#endif

BENDER_API int bender_checkint8(Bender x);
BENDER_API int bender_checkuint8(Bender x);
BENDER_API int bender_checkint16(Bender x);
BENDER_API int bender_checkuint16(Bender x);
BENDER_API int bender_checkint(Bender x);
BENDER_API int bender_checkuint(Bender x);
BENDER_API int bender_checkint64(Bender x);
BENDER_API int bender_checkuint64(Bender x);
BENDER_API int bender_checksize(Bender x);
BENDER_API BenderAbstract bender_checkabstract(Bender x, const BenderAbstractType *at);
#define bender_checkint8range(x) ((x) >= INT8_MIN && (x) <= INT8_MAX && (x) == (int8_t)(x))
#define bender_checkuint8range(x) ((x) >= 0 && (x) <= UINT8_MAX && (x) == (uint8_t)(x))
#define bender_checkint16range(x) ((x) >= INT16_MIN && (x) <= INT16_MAX && (x) == (int16_t)(x))
#define bender_checkuint16range(x) ((x) >= 0 && (x) <= UINT16_MAX && (x) == (uint16_t)(x))
#define bender_checkintrange(x) ((x) >= INT32_MIN && (x) <= INT32_MAX && (x) == (int32_t)(x))
#define bender_checkuintrange(x) ((x) >= 0 && (x) <= UINT32_MAX && (x) == (uint32_t)(x))
#define bender_checkint64range(x) ((x) >= BENDER_INTMIN_DOUBLE && (x) <= BENDER_INTMAX_DOUBLE && (x) == (int64_t)(x))
#define bender_checkuint64range(x) ((x) >= 0 && (x) <= BENDER_INTMAX_DOUBLE && (x) == (uint64_t)(x))
#define bender_unwrap_integer(x) ((int32_t) bender_unwrap_number(x))
#define bender_wrap_integer(x) bender_wrap_number((int32_t)(x))

#define bender_checktypes(x, tps) ((1 << bender_type(x)) & (tps))

/* GC Object type pun. The lower 16 bits of flags are reserved for the garbage collector,
 * but the upper 16 can be used per type for custom flags. This fork's collector files
 * blocks on per-fiber heap lists plus global lists (strings, weak, large), with a
 * nursery of slab pages per owner: full collects walk everything, minors sweep only
 * young blocks, and scoped process collects sweep a single proc's own lists. */
struct BenderGCObject {
    int32_t flags;
    union {
        BenderGCObject *next;
        volatile BenderAtomicInt refcount; /* For threaded abstract types */
    } data;
};

/* A lightweight green thread in bender. Does not correspond to
 * operating system threads. */

/* Native-stack switch context for a fiber. On
 * x86_64/aarch64 the context is just the saved stack pointer (the callee-
 * saved registers live on the coroutine's own stack, see bender_fiber_ctx_swap
 * in src/core/fiber.c); elsewhere it is a POSIX ucontext_t. One slot is
 * enough because a fiber is suspended either from inside its VM loop or
 * from an in-flight nested resume -- the slot always holds the state to
 * re-enter, and is overwritten on every out-running switch. */
#if defined(__x86_64__) || defined(__aarch64__)
typedef struct BenderFiberCtx { void *sp; } BenderFiberCtx;
#else
#include <ucontext.h>
typedef struct BenderFiberCtx { ucontext_t uc; } BenderFiberCtx;
#endif

/* Prime a context so the first bender_fiber_ctx_swap into it begins
 * executing entry(); on x86_64/aarch64 it paints the first save frame
 * on the stack, on other platforms it runs getcontext/makecontext. */
BENDER_API void bender_fiber_ctx_prime(BenderFiberCtx *ctx, void *base,
                                     size_t size, void (*entry)(void));
/* Save *from's current position and switch to *to. */
BENDER_API void bender_fiber_ctx_swap(BenderFiberCtx *from, BenderFiberCtx *to);

/* Forward: slab pages (gc.c) for per-fiber nursery cursors below.
 * Array sized by literal (must match BENDER_SLAB_NCLASSES in gc.h;
 * enforced by static assert there). */
struct BenderSlabPage;

struct BenderFiber {
    BenderGCObject gc; /* GC Object stuff */
    int32_t flags; /* More flags */
    int32_t frame; /* Index of the stack frame */
    int32_t stackstart; /* Beginning of next args */
    int32_t stacktop; /* Top of stack. Where values are pushed and popped from. */
    int32_t capacity; /* How big is the stack memory */
    int32_t maxstack; /* Arbitrary defined limit for stack overflow */
    BenderTable *env; /* Dynamic bindings table (usually current environment). */
    Bender *data; /* Dynamically resized stack memory */
    BenderFiber *child; /* Keep linked list of fibers for restarting pending fibers */
    Bender last_value; /* Last returned value from a fiber */
    /* Native-stack state for fibers: this fiber's VM entries run
     * on native_stack, suspending is a context switch out via ctx, and
     * resume_ctx is where the switch returns control to. in_value,
     * out_payload, and out_signal carry values across the switch boundary. */
    BenderFiberCtx ctx; /* This fiber's context; holds its resume point */
    BenderFiberCtx *resume_ctx; /* The fiber's resumer, to switch back to */
    Bender in_value; /* Argument for the next resume */
    Bender out_payload; /* Value or error from the last resume */
    BenderSignal out_signal; /* Signal from the last resume */
    /* Phase 1.1 swap-suspension (see docs/internals/native-fibers.md): when
     * a fiber suspends with live C frames on its native stack (a yield or
     * user signal passing through bender_call), the VM dynamic registers
     * are parked in saved_* and the resumer's registers loaded, so no
     * Bender state points into the frozen C stack while the resumer runs.
     * suspend_mode selects the resume path: 0 re-enters run_vm from the
     * trampoline (fiber returned to it), 1 swaps back into the frozen C
     * stack. resume_fiber is the resumer (NULL for host code). */
    int32_t suspend_mode;
    BenderFiber *resume_fiber;
    jmp_buf *saved_signal_buf;
    Bender *saved_return_reg;
    int32_t saved_coerce_error;
    int32_t saved_stackn;
    int32_t saved_gc_suspend;
    /* Total mapped size (guard pages + usable region). The usable region is
     * [native_stack, native_stack_base + native_stack_size). */
    void *native_stack; /* Usable region of the native stack (post-guard) */
    void *native_stack_base; /* Mapping base (native_stack - guard) */
    size_t native_stack_size; /* Total mapped bytes */
    /* Phase D: lowest committed address of the usable window (grows down
     * on guard faults; NULL where growth is unsupported). The guard red
     * zone below usable is never committed. */
    void *native_stack_committed;
    /* Phase D3: set by the fault handler on red-zone entry (emergency
     * space committed alongside); converted to a catchable error at the
     * next call-path poll, which clears it. Surviving suspension. */
    int stack_overflowed;
#ifdef BENDER_EV
    /* These fields are only relevant for fibers that are used as "root fibers" -
     * that is, fibers that are scheduled on the event loop and behave much like threads
     * in a multi-tasking system. It would be possible to move these fields to a new
     * type, say "BenderTask", that as separate from fibers to save a bit of space. */
    uint32_t sched_id; /* Increment everytime fiber is scheduled by event loop */
    BenderEVCallback ev_callback; /* Call this before starting scheduled fibers */
    BenderStream *ev_stream; /* which stream we are waiting on */
    void *ev_state; /* Extra data for ev callback state. On windows, first element must be OVERLAPPED. */
    void *supervisor_channel; /* Channel to push self to when complete */
    /* M4b-1 cooperative interrupt: set by ev/interrupt, delivered as a
     * forced YIELD at the next resume boundary in bender_continue_no_check
     * — before a single instruction runs, so delivery can never land
     * mid-instruction, inside pcall, or in C. A racing wakeup value is
     * dropped (interrupt wins); interrupts do not cross VMs. */
    int interrupt_requested;
    Bender interrupt_value;
#endif
    /* Step 1 (process heaps): owner id for slab page attribution (0 =
     * shared/unowned). Assigned at fiber creation from the VM counter;
     * kept across resets (same logical fiber), fresh on unmarshal. */
    uint32_t gc_owner;
    /* Step 1.2: per-fiber nursery bump pages (one per slab class) for
     * pure-owned young allocation, plus lifetime slab bytes charged to
     * this fiber (pressure accounting for per-fiber minors). NULL/0 at
     * birth; kept across resets (pages stay valid). */
    struct BenderSlabPage *nursery_cur[8];
    size_t gc_nursery_bytes;
    /* P3.3 strict heaps: set for spawn-isolated proc fibers (never
     * inherited, never unmarshalled — re-establish by construction).
     * Slab pages claimed while running one record it (page bit). */
    int heap_strict;
    /* P3.5 auto-trigger: owning proc (set by proc spawn for proc
     * fibers, NULL otherwise). maybe_collect routes strict pressure
     * to per-proc majors through this. */
    struct BenderProcess *proc;
    /* P3.5b owner-segregated sweep: blocks born on this fiber file by
     * heap kind here (0 main, 1 strings, 2 weak) instead of the
     * global lists, so per-fiber/per-proc sweeps walk O(own) instead
     * of O(world). Global collections walk these plus globals (dead
     * fibers migrate theirs at free). NULL at birth. */
    void *heap_lists[3];
    /* Channel-pending registrations (BenderChanPendReg list, ev.c).
     * One node per pending queue entry this fiber is parked behind,
     * claimed by the channel when the entry is consumed and freed at
     * fiber free (bender_ev_chanpends_deinit) — the unlink-on-free
     * discipline that replaced probe-on-read for stale entries.
     * Opaque here: nodes belong to ev.c. */
    void *chan_pends;
    /* P3.6 shared C stacks: ev-task fibers opt in (BENDER_SHARED_STACKS
     * + ev/go or proc spawn). Shared fibers hold no private native
     * mapping (base stays NULL); each resume re-primes their context
     * on the lane-shared mapping (see BenderVM). Suspend paths abandon
     * C frames by discipline, so nothing stack-bound survives a
     * switch — except raw yield and nested resume, which panic
     * (fail loud, generators stay private). */
    int shared_stack;
    /* Erlang-parity exit cost: this fiber's own index in the slab
     * state's census array (ss->fibers), making bender_census_remove an
     * O(1) swap-remove instead of an O(live-fibers) linear scan --
     * mass exit of N processes paid O(N^2) purely for that scan.
     * BENDER_CENSUS_UNTRACKED when not in any census. Treated strictly
     * as a HINT, never as truth: census_remove validates
     * ss->fibers[census_idx] == f and falls back to the full linear
     * scan on any mismatch, so a stale index can only ever cost time,
     * never drop a live fiber out of the GC root set. */
    size_t census_idx;
};
#define BENDER_CENSUS_UNTRACKED ((size_t) -1)

/* Mark if a stack frame is a tail call for debugging */
#define BENDER_STACKFRAME_TAILCALL 1

/* Mark if a stack frame is an entrance frame */
#define BENDER_STACKFRAME_ENTRANCE 2

/* A stack frame on the fiber. Is stored along with the stack values. */
struct BenderStackFrame {
    BenderFunction *func;
    uint32_t *pc;
    BenderFuncEnv *env;
    int32_t prevframe;
    int32_t flags;
};

/* Number of Benders a frame takes up in the stack
 * Should be constant across architectures */
#define BENDER_FRAME_SIZE 4

/* A dynamic array type. */
struct BenderArray {
    BenderGCObject gc;
    int32_t count;
    int32_t capacity;
    Bender *data;
};

/* A byte buffer type. Used as a mutable string or string builder. */
struct BenderBuffer {
    BenderGCObject gc;
    int32_t count;
    int32_t capacity;
    uint8_t *data;
};

/* A mutable associative data type. Backed by a hashtable. */
struct BenderTable {
    BenderGCObject gc;
    int32_t count;
    int32_t capacity;
    int32_t deleted;
    BenderKV *data;
    BenderTable *proto;
};

/* A key value pair in a struct or table */
struct BenderKV {
    Bender key;
    Bender value;
};

/* Prefix for a tuple */
struct BenderTupleHead {
    BenderGCObject gc;
    int32_t length;
    int32_t hash;
    int32_t sm_line;
    int32_t sm_column;
    const Bender data[];
};

/* Prefix for a struct */
struct BenderStructHead {
    BenderGCObject gc;
    int32_t length;
    int32_t hash;
    int32_t capacity;
    const BenderKV *proto;
    const BenderKV data[];
};

/* Prefix for a string */
struct BenderStringHead {
    BenderGCObject gc;
    int32_t length;
    int32_t hash;
    const uint8_t data[];
};

/* Prefix for an abstract value */
struct BenderAbstractHead {
    BenderGCObject gc;
    const BenderAbstractType *type;
    size_t size;
    /* VM that created this threaded abstract. The refcount may drop to
     * zero on ANY thread (whichever purges its registry claim last);
     * the memory release must run on the home VM's thread so the home
     * VM's in-flight operations never see the block freed underneath
     * them (BUG 2 density: worker T22 read a proc abstract freed by
     * main T0's registry purge). */
    void *home_vm;
    /* Distinguishes a stopped VM from a later VM reusing its address. */
    uint64_t home_vm_generation;
    /* Single-shot teardown guard (BUG 2 density double-deinit): the
     * abstract sweep path (bender_deinit_block) and the home-routed
     * teardown both want to run the gc hook; exactly one wins. */
    int teardown_started;
    /* Set when the abstract was ever marshaled to another VM (a
     * foreign lane may hold claim-less values); exported abstracts
     * pin on their home VM until exit instead of prompt-freeing. */
    int exported;
    /* Conservative at creation; a home major's marked visit maintains
     * authorization and only its completed unvisited registry sweep
     * revokes it. Minor and foreign VM traversals cannot change it. */
    int home_gc_authorized;
    long long data[]; /* Use long long to ensure most general alignment */
};

/* Some function definition flags */
#define BENDER_FUNCDEF_FLAG_VARARG 0x10000
#define BENDER_FUNCDEF_FLAG_NEEDSENV 0x20000
#define BENDER_FUNCDEF_FLAG_HASSYMBOLMAP 0x40000
#define BENDER_FUNCDEF_FLAG_HASNAME 0x80000
#define BENDER_FUNCDEF_FLAG_HASSOURCE 0x100000
#define BENDER_FUNCDEF_FLAG_HASDEFS 0x200000
#define BENDER_FUNCDEF_FLAG_HASENVS 0x400000
#define BENDER_FUNCDEF_FLAG_HASSOURCEMAP 0x800000
#define BENDER_FUNCDEF_FLAG_STRUCTARG 0x1000000
#define BENDER_FUNCDEF_FLAG_HASCLOBITSET 0x2000000
#define BENDER_FUNCDEF_FLAG_NAMEDARGS 0x4000000
/* Runtime-only: this def's body is the ffi alias shim installed by
 * bender_ffi_alias_install (push args; ldc wrapper; tcall). Set only at
 * runtime by the installer; the compiler never emits it. Lets the VM
 * call the wrapper cfunction directly without entering the shim frame. */
#define BENDER_FUNCDEF_FLAG_FFI_ALIAS 0x8000000
/* Compile-time-only marker set by ffi/defbind-alias. The native alias
 * install flag above is set at image runtime; this one is present from
 * the moment the definition is compiled, so call sites compiled later
 * can recognize alias functions and reuse scratch arrays for tuple
 * literal arguments. */
#define BENDER_FUNCDEF_FLAG_FFI_ALIAS_DECL 0x10000000
#define BENDER_FUNCDEF_FLAG_TAG 0xFFFF

/* Source mapping structure for a bytecode instruction */
struct BenderSourceMapping {
    int32_t line;
    int32_t column;
};

/* Symbol to slot mapping & lifetime structure. */
struct BenderSymbolMap {
    uint32_t birth_pc;
    uint32_t death_pc;
    uint32_t slot_index;
    const uint8_t *symbol;
};

/* Linked, executable-lifetime metadata; never contains GC references or gets
 * serialized. All bytecode PCs are legal entries. Enabled only in a runtime
 * built with BENDER_NATIVE_EXECUTORS. */
/* BENDER_NATIVE_FRAME: the executor pushed a real fiber frame for a Bender
 * callee (exactly what JOP_CALL's function branch does) and `*pc` points at
 * PC 0 of the NEW current frame's definition. Every consumer must reload
 * stack, pc AND func from the frame (vm_restore) before continuing. */
typedef enum { BENDER_NATIVE_VM, BENDER_NATIVE_INTERRUPT, BENDER_NATIVE_FRAME } BenderNativeStep;
typedef BenderNativeStep (*BenderNativeExecutor)(BenderFiber *, BenderFuncDef *, uint32_t **);
/* Stage 2: opaque entries describe a live pool load only. They assume
 * nothing about the boxed value at that index beyond it being a valid
 * indexed constant; the runtime pool is read at execution time. */
#define BENDER_NATIVE_OPAQUE ((BenderType)100)
typedef struct {
    BenderType type;
    double number; /* numeric value or boolean 0/1; no boxed pointers */
} BenderNativeConstant;
typedef struct BenderNativeCode {
    BenderNativeExecutor execute;
    const uint32_t *words;
    const BenderNativeConstant *constants;
    const uint64_t *region_mask; /* bit per instruction PC: native entry allowed */
    int32_t length, slotcount, arity, min_arity, max_arity, flags, constants_length;
    int32_t region_mask_words; /* == (length + 63) / 64 when length > 0 */
} BenderNativeCode;
BENDER_API int bender_native_register(BenderFunction *, const BenderNativeCode *);
BENDER_API int bender_native_matches(BenderFuncDef *, const BenderNativeCode *);
BENDER_API int bender_native_poll(int condition);
BENDER_API BenderCFunction bender_native_alias_cfun(Bender callee, int32_t argc);
BENDER_API Bender bender_native_alias_call(BenderFiber *fiber, BenderCFunction cfun,
                                        int32_t argc, uint32_t *pc);
BENDER_API void bender_native_alias_after_call(void);
/* Native-to-native continuation (v1, flat trampoline).
 *
 * bender_native_call_enter is ALL-OR-NOTHING. It returns:
 *   0 - refused; NOTHING on the fiber was touched, so the executor must take
 *       its ordinary VM exit at the call PC and JOP_CALL performs the call
 *       exactly once;
 *   1 - it did precisely what JOP_CALL's function branch does (commit the
 *       caller PC, bender_fiber_funcframe, GC check) and *out_npc is PC 0 of
 *       the callee definition; the executor must return BENDER_NATIVE_FRAME;
 *   2 - an interrupt is due; nothing was touched and the executor must return
 *       BENDER_NATIVE_INTERRUPT at the call PC so the VM re-executes it.
 * The single interrupt poll happens only after every refusal check passed,
 * so a refused edge is polled exactly once -- by JOP_CALL, as today.
 *
 * bender_native_ret_pop mirrors JOP_RETURN/JOP_RETURN_NIL: it returns 1 after
 * popping the frame and storing the result in the caller's result slot
 * (decoded from the caller's committed JOP_CALL word), with *pc advanced to
 * the instruction after the call; 0 without touching anything. */
BENDER_API int bender_native_call_enter(BenderFiber *fiber, Bender callee, int32_t argc,
                                      uint32_t *callpc, uint32_t **out_npc);
BENDER_API int bender_native_ret_pop(BenderFiber *fiber, uint32_t **pc);
/* Fast, no-allocation paths for native executors. Each returns 1 after
 * performing exactly the boxed operation's mutation/read, or 0 without
 * touching the target so the executor can fall back at the original PC. */
BENDER_API void bender_native_set_pc(BenderFiber *fiber, uint32_t *pc);
/* A census-only, nonallocating commit hook. It fires after an irreversible
 * fast-PUT effect and before any later barrier can unwind. A refused helper
 * never calls it. The ordinary ABI and hot path are unchanged when disabled. */
#if defined(BENDER_NATIVE_COMMIT_PROBE) && BENDER_NATIVE_COMMIT_PROBE
typedef void (*BenderNativeCommitHook)(void *context);
BENDER_API int bender_native_buffer_put_fast_probe(Bender ds, Bender key, Bender value,
        BenderNativeCommitHook commit, void *context);
BENDER_API int bender_native_array_put_fast_probe(Bender ds, Bender key, Bender value,
        BenderNativeCommitHook commit, void *context);
BENDER_API int bender_native_table_put_fast_probe(Bender ds, Bender key, Bender value,
        BenderNativeCommitHook commit, void *context);
#endif
BENDER_API int bender_native_buffer_put_fast(Bender ds, Bender key, Bender value);
BENDER_API int bender_native_array_put_fast(Bender ds, Bender key, Bender value);
BENDER_API int bender_native_table_get_fast(Bender ds, Bender key, Bender *out);
BENDER_API int bender_native_container_call_fast(Bender callee, int32_t argc,
        const Bender *argv, Bender *out);
BENDER_API int bender_native_table_put_fast(Bender ds, Bender key, Bender value);
BENDER_API int bender_native_buffer_push_string_ready(Bender callee, int32_t argc,
        const Bender *argv, const uint8_t **bytes, int32_t *len);
BENDER_API Bender bender_native_buffer_push_string_commit(int32_t argc,
        const Bender *argv, const uint8_t *bytes, int32_t len);
BENDER_API void bender_native_register_predicates(BenderTable *env);
BENDER_API int bender_native_core_predicate(Bender callee, int32_t argc,
        const Bender *argv, Bender *out);
/* Stage 2: VM-boundary regions. region_allowed gates whole-definition
 * validation to useful native starts (>= 3 native operations ahead).
 * registry_add publishes a linked descriptor; try_attach performs the
 * unique-candidate structural match against a definition (ambiguous
 * identity refuses). Both are no-ops without BENDER_NATIVE_EXECUTORS. */
BENDER_API int bender_native_region_allowed(const BenderNativeCode *n, uint32_t pc);
BENDER_API void bender_native_registry_add(const BenderNativeCode *n);
BENDER_API int bender_native_registry_count(void);
/* Native FFI alias tier support. `bender_ffi_pointer_value` coerces one
 * boxed value to a raw pointer with the FFI argument marshaller's rules.
 * `bender_ffi_alias_install` resolves the definition's lookup thunk once
 * (under bender_pcall), validates the resulting pointer with dladdr
 * against `symbol`, and only then replaces the definition's body with a
 * tail call to the generated cfunction wrapper. Returns 0 on success and
 * stores the pointer in *out_ptr; any refusal code leaves the boxed body
 * in place and touches nothing. Callers are generated by
 * tools/callgraph.ben from the statically proven alias shape. */
BENDER_API void *bender_ffi_pointer_value(Bender x);
BENDER_API int bender_ffi_alias_install(BenderFunction *f, int32_t thunk_index, Bender wrapper,
                                      const char *symbol, void **out_ptr);
BENDER_API uint32_t bender_native_registry_generation(void);
BENDER_API int bender_native_try_attach(BenderFuncDef *d);
/* VM region re-entry validation for d->native_code: exact structural
 * compare, with bender_verify amortized to the first validation of the
 * (def, descriptor) pair (deterministic over the compared fields) and
 * the verdict cached for the current validation epoch. */
BENDER_API int bender_native_revalidate(BenderFuncDef *d);
/* Invalidate cached whole-definition validation verdicts (bumps the
 * per-VM epoch). Called on VM resume entry, debugger breakpoint writes,
 * and descriptor publication, so any def change between resumes forces
 * a fresh exact bender_native_matches at the next native region entry. */
BENDER_API void bender_native_invalidate(void);
BENDER_API uint64_t bender_native_stat(int which);
BENDER_API uint64_t bender_native_exit_stat(int opcode);
/* bender_native_stat indexes */
#define BENDER_NATIVE_STAT_ATTACH_ATTEMPTS 0
#define BENDER_NATIVE_STAT_ATTACH_ATTACHED 1
#define BENDER_NATIVE_STAT_ATTACH_AMBIGUOUS 2
#define BENDER_NATIVE_STAT_ATTACH_NOMATCH 3
#define BENDER_NATIVE_STAT_VM_EXITS 4
#define BENDER_NATIVE_STAT_CONTINUATIONS 5
#define BENDER_NATIVE_STAT_CONTINUATION_REFUSALS 6
#define BENDER_NATIVE_STAT_NATIVE_RETURNS 7
#define BENDER_NATIVE_STAT_INTERP_CALLS 8
#define BENDER_NATIVE_STAT_INTERP_DISPATCHES 9
#define BENDER_NATIVE_STAT_ALLOCATIONS 10
#define BENDER_NATIVE_STAT_ALLOC_BYTES 11
#define BENDER_NATIVE_STAT_BARRIER_CALLS 12
#define BENDER_NATIVE_STAT_GC_MAJOR 13
#define BENDER_NATIVE_STAT_GC_MINOR 14
#define BENDER_NATIVE_STAT_GC_FIBER_MINOR 15
#define BENDER_NATIVE_STAT_GC_PROCESS_MINOR 16

/* A function definition. Contains information needed to instantiate closures. */
struct BenderFuncDef {
    BenderGCObject gc;
#ifdef BENDER_TTFX_SITE_PROFILE
    /* Monotone per-thread definition identity for diagnostic counts. Distinct
     * allocations remain distinct even if GC reuses the same address. */
    uint64_t site_profile_id;
#endif
    int32_t *environments; /* Which environments to capture from parent. */
    Bender *constants;
    BenderFuncDef **defs;
    uint32_t *bytecode;
    const BenderNativeCode *native_code;
    uint32_t native_attach_gen; /* lazy attach already tried at this registry generation */
    /* Region re-entry validation cache: `native_verified` is the descriptor
     * whose exact structural match was last proven for this def, at the
     * `native_verified_epoch` validation epoch. The cache only skips
     * re-running the proof while nothing can have changed the def;
     * bender_native_matches remains the sole authority and is re-run after
     * every invalidation (resume, breakpoint write, publication). */
    const BenderNativeCode *native_verified;
    uint64_t native_verified_epoch;
    uint32_t *closure_bitset; /* Bit set indicating which slots can be referenced by closures. */

    /* Various debug information */
    BenderSourceMapping *sourcemap;
    BenderString source;
    BenderString name;
    BenderSymbolMap *symbolmap;

    int32_t flags;
    int32_t slotcount; /* The amount of stack space required for the function */
    int32_t arity; /* Not including varargs */
    int32_t min_arity; /* Including varargs */
    int32_t max_arity; /* Including varargs */
    int32_t constants_length;
    int32_t bytecode_length;
    int32_t environments_length;
    int32_t defs_length;
    int32_t symbolmap_length;
    int32_t named_args_count;
};

/* A function environment */
struct BenderFuncEnv {
    BenderGCObject gc;
    union {
        BenderFiber *fiber;
        Bender *values;
    } as;
    int32_t length; /* Size of environment */
    int32_t offset; /* Stack offset when values still on stack. If offset is <= 0, then
        environment is no longer on the stack. */
};

#define BENDER_FUNCFLAG_TRACE (1 << 16)

/* A function */
struct BenderFunction {
    BenderGCObject gc;
    BenderFuncDef *def;
    BenderFuncEnv *envs[];
};

/* Use to read Bender data structures into memory from source code */
typedef struct BenderParseState BenderParseState;
typedef struct BenderParser BenderParser;

typedef int (*Consumer)(BenderParser *p, BenderParseState *state, uint8_t c);

struct BenderParseState {
    int32_t counter;
    int32_t argn;
    int flags;
    size_t line;
    size_t column;
    Consumer consumer;
};

enum BenderParserStatus {
    BENDER_PARSE_ROOT,
    BENDER_PARSE_ERROR,
    BENDER_PARSE_PENDING,
    BENDER_PARSE_DEAD
};

/* A bender parser */
struct BenderParser {
    Bender *args;
    const char *error;
    BenderParseState *states;
    uint8_t *buf;
    size_t argcount;
    size_t argcap;
    size_t statecount;
    size_t statecap;
    size_t bufcount;
    size_t bufcap;
    size_t line;
    size_t column;
    size_t pending;
    int lookback;
    int flag;
};

/* A context for marshaling and unmarshaling abstract types */
struct BenderMarshalContext {
    void *m_state;
    void *u_state;
    int flags;
    const uint8_t *data;
    const BenderAbstractType *at;
};
typedef struct BenderMarshalContext BenderMarshalContext;

/* Defines an abstract type. Use a const pointer to one of these structures
 * when creating abstract types. The memory for this pointer should not be free
 * until after bender_deinit is called. Usually, this means declaring BenderAbstractType's
 * as const data at file scope, and creating instances with bender_abstract(&MyType, sizeof(MyTypeStruct)); */
struct BenderAbstractType {
    const char *name;
    int (*gc)(void *data, size_t len);
    int (*gcmark)(void *data, size_t len);
    int (*get)(void *data, Bender key, Bender *out);
    void (*put)(void *data, Bender key, Bender value);
    void (*marshal)(void *p, BenderMarshalContext *ctx);
    void *(*unmarshal)(BenderMarshalContext *ctx);
    void (*tostring)(void *p, BenderBuffer *buffer);
    int (*compare)(void *lhs, void *rhs);
    int32_t (*hash)(void *p, size_t len);
    Bender(*next)(void *p, Bender key);
    Bender(*call)(void *p, int32_t argc, Bender *argv);
    size_t (*length)(void *p, size_t len);
    BenderByteView(*bytes)(void *p, size_t len);
    int (*gcperthread)(void *data, size_t len);
};

/* Some macros to let us add extra types to BenderAbstract types without
 * needing to changing native modules that declare them as static const
 * structures. If more fields are added, these macros are modified to include
 * default values (usually NULL). This silences missing field warnings. */
#define BENDER_ATEND_NAME        NULL,BENDER_ATEND_GC
#define BENDER_ATEND_GC          NULL,BENDER_ATEND_GCMARK
#define BENDER_ATEND_GCMARK      NULL,BENDER_ATEND_GET
#define BENDER_ATEND_GET         NULL,BENDER_ATEND_PUT
#define BENDER_ATEND_PUT         NULL,BENDER_ATEND_MARSHAL
#define BENDER_ATEND_MARSHAL     NULL,BENDER_ATEND_UNMARSHAL
#define BENDER_ATEND_UNMARSHAL   NULL,BENDER_ATEND_TOSTRING
#define BENDER_ATEND_TOSTRING    NULL,BENDER_ATEND_COMPARE
#define BENDER_ATEND_COMPARE     NULL,BENDER_ATEND_HASH
#define BENDER_ATEND_HASH        NULL,BENDER_ATEND_NEXT
#define BENDER_ATEND_NEXT        NULL,BENDER_ATEND_CALL
#define BENDER_ATEND_CALL        NULL,BENDER_ATEND_LENGTH
#define BENDER_ATEND_LENGTH      NULL,BENDER_ATEND_BYTES
#define BENDER_ATEND_BYTES       NULL,BENDER_ATEND_GCPERTHREAD
#define BENDER_ATEND_GCPERTHREAD

struct BenderReg {
    const char *name;
    BenderCFunction cfun;
    const char *documentation;
};

struct BenderRegExt {
    const char *name;
    BenderCFunction cfun;
    const char *documentation;
    const char *source_file;
    int32_t source_line;
};

struct BenderMethod {
    const char *name;
    BenderCFunction cfun;
};

struct BenderView {
    const Bender *items;
    int32_t len;
};

struct BenderByteView {
    const uint8_t *bytes;
    int32_t len;
};

struct BenderDictView {
    const BenderKV *kvs;
    int32_t len;
    int32_t cap;
};

struct BenderRange {
    int32_t start;
    int32_t end;
};

struct BenderRNG {
    uint32_t a, b, c, d;
    uint32_t counter;
};

typedef struct BenderFile BenderFile;
struct BenderFile {
    FILE *file;
    int32_t flags;
    size_t vbufsize;
};

/* For bender_try and bender_restore */
typedef struct {
    /* old state */
    int32_t stackn;
    int gc_handle;
    BenderFiber *vm_fiber;
    jmp_buf *vm_jmp_buf;
    Bender *vm_return_reg;
    /* new state */
    jmp_buf buf;
    Bender payload;
    int coerce_error;
} BenderTryState;

/***** END SECTION TYPES *****/

/***** START SECTION OPCODES *****/

/* Bytecode op argument types */
enum BenderOpArgType {
    BENDER_OAT_SLOT,
    BENDER_OAT_ENVIRONMENT,
    BENDER_OAT_CONSTANT,
    BENDER_OAT_INTEGER,
    BENDER_OAT_TYPE,
    BENDER_OAT_SIMPLETYPE,
    BENDER_OAT_LABEL,
    BENDER_OAT_FUNCDEF
};

/* Various types of instructions */
enum BenderInstructionType {
    JINT_0, /* No args */
    JINT_S, /* Slot(3) */
    JINT_L, /* Label(3) */
    JINT_SS, /* Slot(1), Slot(2) */
    JINT_SL, /* Slot(1), Label(2) */
    JINT_ST, /* Slot(1), Slot(2) */
    JINT_SI, /* Slot(1), Immediate(2) */
    JINT_SD, /* Slot(1), Closure(2) */
    JINT_SU, /* Slot(1), Unsigned Immediate(2) */
    JINT_SSS, /* Slot(1), Slot(1), Slot(1) */
    JINT_SSI, /* Slot(1), Slot(1), Immediate(1) */
    JINT_SSU, /* Slot(1), Slot(1), Unsigned Immediate(1) */
    JINT_SES, /* Slot(1), Environment(1), Far Slot(1) */
    JINT_SC /* Slot(1), Constant(2) */
};

/* All opcodes for the bytecode interpreter. */
enum BenderOpCode {
    JOP_NOOP,
    JOP_ERROR,
    JOP_TYPECHECK,
    JOP_RETURN,
    JOP_RETURN_NIL,
    JOP_ADD_IMMEDIATE,
    JOP_ADD,
    JOP_SUBTRACT_IMMEDIATE,
    JOP_SUBTRACT,
    JOP_MULTIPLY_IMMEDIATE,
    JOP_MULTIPLY,
    JOP_DIVIDE_IMMEDIATE,
    JOP_DIVIDE,
    JOP_DIVIDE_FLOOR,
    JOP_MODULO,
    JOP_REMAINDER,
    JOP_BAND,
    JOP_BOR,
    JOP_BXOR,
    JOP_BNOT,
    JOP_SHIFT_LEFT,
    JOP_SHIFT_LEFT_IMMEDIATE,
    JOP_SHIFT_RIGHT,
    JOP_SHIFT_RIGHT_IMMEDIATE,
    JOP_SHIFT_RIGHT_UNSIGNED,
    JOP_SHIFT_RIGHT_UNSIGNED_IMMEDIATE,
    JOP_MOVE_FAR,
    JOP_MOVE_NEAR,
    JOP_JUMP,
    JOP_JUMP_IF,
    JOP_JUMP_IF_NOT,
    JOP_JUMP_IF_NIL,
    JOP_JUMP_IF_NOT_NIL,
    JOP_GREATER_THAN,
    JOP_GREATER_THAN_IMMEDIATE,
    JOP_LESS_THAN,
    JOP_LESS_THAN_IMMEDIATE,
    JOP_EQUALS,
    JOP_EQUALS_IMMEDIATE,
    JOP_COMPARE,
    JOP_LOAD_NIL,
    JOP_LOAD_TRUE,
    JOP_LOAD_FALSE,
    JOP_LOAD_INTEGER,
    JOP_LOAD_CONSTANT,
    JOP_LOAD_UPVALUE,
    JOP_LOAD_SELF,
    JOP_SET_UPVALUE,
    JOP_CLOSURE,
    JOP_PUSH,
    JOP_PUSH_2,
    JOP_PUSH_3,
    JOP_PUSH_ARRAY,
    JOP_CALL,
    JOP_TAILCALL,
    JOP_RESUME,
    JOP_SIGNAL,
    JOP_PROPAGATE,
    JOP_IN,
    JOP_GET,
    JOP_PUT,
    JOP_GET_INDEX,
    JOP_PUT_INDEX,
    JOP_LENGTH,
    JOP_MAKE_ARRAY,
    JOP_MAKE_BUFFER,
    JOP_MAKE_STRING,
    JOP_MAKE_STRUCT,
    JOP_MAKE_TABLE,
    JOP_MAKE_TUPLE,
    JOP_MAKE_BRACKET_TUPLE,
    JOP_GREATER_THAN_EQUAL,
    JOP_LESS_THAN_EQUAL,
    JOP_NEXT,
    JOP_NOT_EQUALS,
    JOP_NOT_EQUALS_IMMEDIATE,
    JOP_CANCEL,
    /* (in ds <const small int>) fused: the key is an UNSIGNED immediate
     * instead of a preceding load_integer into a slot. The handler calls
     * bender_in with the wrapped immediate, so semantics are IDENTICAL to
     * JOP_IN -- deliberately NOT reusing JOP_GET_INDEX, whose
     * bender_getindex returns nil where bender_in RAISES for an
     * out-of-range string/symbol/keyword index (verified by test).
     * Appended at the end of the enum so every existing opcode number,
     * and therefore marshalled bytecode, is unchanged. */
    JOP_IN_IMMEDIATE,
    /* (band x <const 0..255>) fused: the mask is an UNSIGNED immediate
     * instead of a preceding load_integer. Unsigned because the hot mask
     * is 0xff = 255, which does NOT fit a signed int8 -- that is exactly
     * why band could not be fused when JOP_IN_IMMEDIATE landed. Negative
     * masks fall back to JOP_BAND. Appended at the end so existing
     * opcode numbers and marshalled bytecode are unchanged. */
    JOP_BAND_IMMEDIATE,
    /* (bor x <const 0..255>) and (bxor x <const 0..255>): same
     * unsigned-immediate rationale as JOP_BAND_IMMEDIATE above. */
    JOP_BOR_IMMEDIATE,
    JOP_BXOR_IMMEDIATE,
    /* make-scratch-tuple: A = destination (near), E = persistent scratch
     * slot (far); the element count is implicit in the push stack, exactly
     * like JOP_MAKE_TUPLE. The scratch slot holds a mutable array reused
     * for every execution of this call site in the current frame. Emitted
     * only for tuple literals passed directly to an FFI alias, which reads
     * the array synchronously and never retains it. Appended at the end so
     * existing opcode numbers and marshalled bytecode are unchanged. */
    JOP_MAKE_SCRATCH_TUPLE,
    JOP_INSTRUCTION_COUNT
};

/* Info about all instructions */
extern const enum BenderInstructionType bender_instructions[JOP_INSTRUCTION_COUNT];

/***** END SECTION OPCODES *****/

/***** START SECTION MAIN *****/

#ifdef BENDER_EV

extern BENDER_API const BenderAbstractType bender_stream_type;
extern BENDER_API const BenderAbstractType bender_channel_type;

/* Run the event loop */
BENDER_API void bender_loop(void);

/* Run the event loop, but allow for user scheduled interrupts triggered
 * by bender_loop1_interrupt being called in library code, a signal handler, or
 * another thread.
 *
 * Example:
 *
 * while (!bender_loop_done()) {
 *   // One turn of the event loop
 *   BenderFiber *interrupted_fiber = bender_loop1();
 *   // interrupted_fiber may be NULL
 *   // do some work here periodically...
 *   if (NULL != interrupted_fiber) {
 *     if (cancel_interrupted_fiber) {
 *       bender_cancel(interrupted_fiber, bender_cstringv("fiber was interrupted for [reason]"));
 *     } else {
 *       bender_schedule(interrupted_fiber, bender_wrap_nil());
 *     }
 *     bender_interpreter_interrupt_handled(NULL);
 *   }
 * }
 *
 */
BENDER_API int bender_loop_done(void);
BENDER_API BenderFiber *bender_loop1(void);
BENDER_API void bender_loop1_interrupt(BenderVM *vm);

/* Wrapper around streams */
BENDER_API BenderStream *bender_stream(BenderHandle handle, uint32_t flags, const BenderMethod *methods);
BENDER_API BenderStream *bender_stream_ext(BenderHandle handle, uint32_t flags, const BenderMethod *methods, size_t size); /* Allow for type punning streams */
BENDER_API void bender_stream_close(BenderStream *stream);
BENDER_API Bender bender_cfun_stream_close(int32_t argc, Bender *argv);
BENDER_API Bender bender_cfun_stream_read(int32_t argc, Bender *argv);
BENDER_API Bender bender_cfun_stream_chunk(int32_t argc, Bender *argv);
BENDER_API Bender bender_cfun_stream_write(int32_t argc, Bender *argv);
BENDER_API void bender_stream_flags(BenderStream *stream, uint32_t flags);

/* Queue a fiber to run on the event loop */
BENDER_API void bender_schedule(BenderFiber *fiber, Bender value);
BENDER_API void bender_cancel(BenderFiber *fiber, Bender value);
BENDER_API void bender_schedule_signal(BenderFiber *fiber, Bender value, BenderSignal sig);
BENDER_API void bender_schedule_soon(BenderFiber *fiber, Bender value, BenderSignal sig);

/* Shorthand for yielding to event loop in C */
BENDER_NO_RETURN BENDER_API void bender_await(void);
BENDER_NO_RETURN BENDER_API void bender_sleep_await(double sec);

/* Arm an operation-scoped deadline for the wait the current fiber is about to
 * suspend on. If no other event schedules that fiber first, the loop resumes it
 * after `sec` seconds: bender_addtimeout raises an error, bender_addtimeout_nil
 * resumes with nil.
 *
 * Scope: the deadline belongs to THIS operation. Expiry cancels the waiting
 * task fiber (never an arbitrary C stack), and resuming that fiber retires its
 * pending deadlines, so a completed wait leaves nothing behind to fire into
 * later, unrelated work. A timeout therefore cannot interrupt the root fiber
 * mid-computation, and repeated poll-shaped calls cannot accumulate live
 * timers. Arm it immediately before suspending - the deadline is not tied to
 * the operation object, only to the fiber that suspends.
 *
 * The task identity is bender_vm.root_fiber because only a task fiber carries
 * the scheduler flag bender_cancel requires. */
BENDER_API void bender_addtimeout(double sec);
BENDER_API void bender_addtimeout_nil(double sec);
BENDER_API void bender_ev_inc_refcount(void);
BENDER_API void bender_ev_dec_refcount(void);

/* Thread aware abstract types and helpers */
BENDER_API void *bender_abstract_begin_threaded(const BenderAbstractType *atype, size_t size);
BENDER_API void *bender_abstract_end_threaded(void *x);
BENDER_API void *bender_abstract_threaded(const BenderAbstractType *atype, size_t size);

/* Allow reference counting on threaded abstract types. This is useful when external code , either
 * in the current OS thread or in a different OS thread, takes a pointer to this abstract type. The programmer
 * should tncrement the reference count when taking the pointer, and then decrement and possibly cleanup and free
 * if the reference count is 0. */
BENDER_API int32_t bender_abstract_incref(void *abst);
BENDER_API int32_t bender_abstract_decref(void *abst);

/* If this returns 0, *abst will be deinitialized and freed. Useful shorthand if there is no other cleanup for
 * this abstract type before calling `bender_free` on it's backing memory. */
BENDER_API int32_t bender_abstract_decref_maybe_free(void *abst);

/* Expose channel utilities */
BENDER_API BenderChannel *bender_channel_make(uint32_t limit);
BENDER_API BenderChannel *bender_channel_make_threaded(uint32_t limit);
BENDER_API BenderChannel *bender_channel_make_isolated(uint32_t limit);
BENDER_API BenderChannel *bender_getchannel(const Bender *argv, int32_t n);
BENDER_API BenderChannel *bender_optchannel(const Bender *argv, int32_t argc, int32_t n, BenderChannel *dflt);
BENDER_API int bender_channel_give(BenderChannel *channel, Bender x);
BENDER_API int bender_channel_take(BenderChannel *channel, Bender *out);

/* Expose some OS sync primitives - mutexes and reader-writer locks */
BENDER_API size_t bender_os_mutex_size(void);
BENDER_API size_t bender_os_rwlock_size(void);
BENDER_API void bender_os_mutex_init(BenderOSMutex *mutex);
BENDER_API void bender_os_mutex_deinit(BenderOSMutex *mutex);
BENDER_API void bender_os_mutex_lock(BenderOSMutex *mutex);
BENDER_API void bender_os_mutex_unlock(BenderOSMutex *mutex);
BENDER_API void bender_os_rwlock_init(BenderOSRWLock *rwlock);
BENDER_API void bender_os_rwlock_deinit(BenderOSRWLock *rwlock);
BENDER_API void bender_os_rwlock_rlock(BenderOSRWLock *rwlock);
BENDER_API void bender_os_rwlock_wlock(BenderOSRWLock *rwlock);
BENDER_API void bender_os_rwlock_runlock(BenderOSRWLock *rwlock);
BENDER_API void bender_os_rwlock_wunlock(BenderOSRWLock *rwlock);

/* Get last error from an IO operation */
BENDER_API Bender bender_ev_lasterr(void);

/* Async service for calling a function or syscall in a background thread. This is not
 * as efficient in the slightest as using Streams but can be used for arbitrary blocking
 * functions and syscalls. */

/* Used to pass data between the main thread and worker threads for simple tasks.
 * We could just use a pointer but this prevents malloc/free in the common case
 * of only a handful of arguments. */
typedef struct {
    int tag;
    int argi;
    void *argp;
    Bender argj;
    BenderFiber *fiber;
} BenderEVGenericMessage;

/* How to resume or cancel after a threaded call. Not exhaustive of the possible
 * ways one might want to resume after returning from a threaded call, but should
 * cover most of the common cases. For something more complicated, such as resuming
 * with an abstract type or a struct, one should use bender_ev_threaded_call instead
 * of bender_ev_threaded_await with a custom callback. */

#define BENDER_EV_TCTAG_NIL 0          /* resume with nil */
#define BENDER_EV_TCTAG_INTEGER 1      /* resume with bender_wrap_integer(argi) */
#define BENDER_EV_TCTAG_STRING 2       /* resume with bender_cstringv((const char *) argp) */
#define BENDER_EV_TCTAG_STRINGF 3      /* resume with bender_cstringv((const char *) argp), then call free on argp. */
#define BENDER_EV_TCTAG_KEYWORD 4      /* resume with bender_ckeywordv((const char *) argp) */
#define BENDER_EV_TCTAG_ERR_STRING 5   /* cancel with bender_cstringv((const char *) argp) */
#define BENDER_EV_TCTAG_ERR_STRINGF 6  /* cancel with bender_cstringv((const char *) argp), then call free on argp. */
#define BENDER_EV_TCTAG_ERR_KEYWORD 7  /* cancel with bender_ckeywordv((const char *) argp) */
#define BENDER_EV_TCTAG_BOOLEAN 8      /* resume with bender_wrap_boolean(argi) */

/* Function pointer that is run in the thread pool */
typedef BenderEVGenericMessage(*BenderThreadedSubroutine)(BenderEVGenericMessage arguments);

/* Handler for events posted to the event loop */
typedef void (*BenderCallback)(BenderEVGenericMessage return_value);

/* Handler that is run in the main thread with the result of the BenderAsyncSubroutine (same as BenderCallback) */
typedef void (*BenderThreadedCallback)(BenderEVGenericMessage return_value);

/* API calls for quickly offloading some work in C to a new thread or thread pool. */
BENDER_API void bender_ev_threaded_call(BenderThreadedSubroutine fp, BenderEVGenericMessage arguments, BenderThreadedCallback cb);
BENDER_NO_RETURN BENDER_API void bender_ev_threaded_await(BenderThreadedSubroutine fp, int tag, int argi, void *argp);

/* Post callback + userdata to an event loop. Takes the vm parameter to allow posting from other
 * threads or signal handlers. Use NULL to post to the current thread. */
BENDER_API void bender_ev_post_event(BenderVM *vm, BenderCallback cb, BenderEVGenericMessage msg);
BENDER_API int32_t bender_channel_count(void *chan);

/* Callback used by bender_ev_threaded_await */
BENDER_API void bender_ev_default_threaded_callback(BenderEVGenericMessage return_value);

/* Read async from a stream. These function yield to the event-loop with bender_await(), and so do not return.
 * When the fiber is resumed, the fiber will simply continue to the next Bender abstract machine instruction. */
BENDER_NO_RETURN BENDER_API void bender_ev_read(BenderStream *stream, BenderBuffer *buf, int32_t nbytes);
BENDER_NO_RETURN BENDER_API void bender_ev_readchunk(BenderStream *stream, BenderBuffer *buf, int32_t nbytes);
#ifdef BENDER_NET
BENDER_NO_RETURN BENDER_API void bender_ev_recv(BenderStream *stream, BenderBuffer *buf, int32_t nbytes, int flags);
BENDER_NO_RETURN BENDER_API void bender_ev_recvchunk(BenderStream *stream, BenderBuffer *buf, int32_t nbytes, int flags);
BENDER_NO_RETURN BENDER_API void bender_ev_recvfrom(BenderStream *stream, BenderBuffer *buf, int32_t nbytes, int flags);
#endif

/* Write async to a stream. These function yield to the event-loop with bender_await(), and so do not return.
 * When the fiber is resumed, the fiber will simply continue to the next Bender abstract machine instruction. */
BENDER_NO_RETURN BENDER_API void bender_ev_write_buffer(BenderStream *stream, BenderBuffer *buf);
BENDER_NO_RETURN BENDER_API void bender_ev_write_string(BenderStream *stream, BenderString str);
#ifdef BENDER_NET
BENDER_NO_RETURN BENDER_API void bender_ev_send_buffer(BenderStream *stream, BenderBuffer *buf, int flags);
BENDER_NO_RETURN BENDER_API void bender_ev_send_string(BenderStream *stream, BenderString str, int flags);
BENDER_NO_RETURN BENDER_API void bender_ev_sendto_buffer(BenderStream *stream, BenderBuffer *buf, void *dest, int flags);
BENDER_NO_RETURN BENDER_API void bender_ev_sendto_string(BenderStream *stream, BenderString str, void *dest, int flags);
#endif

#endif

/* Parsing.
 *
 * E.g.
 *
 * BenderParser parser;
 * bender_parser_init(&parser);
 * for (int i = 0; i < source_code_length + 1; i++) {
 *   if (i >= source_code_length) {
 *     bender_parser_eof(&parser);
 *   } else {
 *     bender_parser_consume(&parser, source_code[i]);
 *   }
 *   while (bender_parser_has_more(&parser)) {
 *      Bender x = bender_parser_produce(&parser);
 *      bender_printf("got value: %v\n", x);
 *   }
 *   switch (bender_parser_status(&parser)) {
 *      case BENDER_PARSE_PENDING: break;
 *      case BENDER_PARSE_ERROR: bender_eprintf("error: %s\n", bender_parser_error(&parser)); break;
 *      case BENDER_PARSE_ROOT: break;
 *      case BENDER_PARSE_DEAD: break;
 *   }
 * }
 * bender_parser_deinit(&parser);
 *
 * */
extern BENDER_API const BenderAbstractType bender_parser_type;

/* Construct/destruct a parser. Parsers can be allocated on the stack or the heap. */
BENDER_API void bender_parser_init(BenderParser *parser);
BENDER_API void bender_parser_deinit(BenderParser *parser);

/* Feed bytes into the parser. Check the parser state after every byte to handle errors. */
BENDER_API void bender_parser_consume(BenderParser *parser, uint8_t c);

/* Check the current status of the parser */
BENDER_API enum BenderParserStatus bender_parser_status(BenderParser *parser);

/* Produce a value from the parser. Call this when bender_parser_has_more(&parser) is non-zero. */
BENDER_API Bender bender_parser_produce(BenderParser *parser);

/* Produce a value from the parser, wrapped in a tuple. The tuple is used to carry the source mapping information of the
 * top level form, such as a line number or symbol. */
BENDER_API Bender bender_parser_produce_wrapped(BenderParser *parser);

/* When there is an error while parsing (bender_parser_status(&parser) == BENDER_PARSE_ERROR), get a nice error string.
 * Calling this will also flush the parser. */
BENDER_API const char *bender_parser_error(BenderParser *parser);

/* If there is a parsing error, flush the parser to set the state back to empty.
 * This allows for better error recover and less confusing error messages on bad syntax deep inside nested data structures. */
BENDER_API void bender_parser_flush(BenderParser *parser);

/* Indicate that there is no more source code */
BENDER_API void bender_parser_eof(BenderParser *parser);

/* If non-zero, the parser has values ready to be produced. */
BENDER_API int bender_parser_has_more(BenderParser *parser);

/* Assembly */
#ifdef BENDER_ASSEMBLER
#define BENDER_ASSEMBLE_FLAG_OPTIMIZE 1
typedef struct BenderAssembleResult BenderAssembleResult;
enum BenderAssembleStatus {
    BENDER_ASSEMBLE_OK,
    BENDER_ASSEMBLE_ERROR
};
struct BenderAssembleResult {
    BenderFuncDef *funcdef;
    BenderString error;
    enum BenderAssembleStatus status;
};
BENDER_API BenderAssembleResult bender_asm(Bender source, int flags);
BENDER_API Bender bender_disasm(BenderFuncDef *def);
BENDER_API Bender bender_asm_decode_instruction(uint32_t instr);
#endif

/* Compilation */
typedef struct BenderCompileResult BenderCompileResult;
enum BenderCompileStatus {
    BENDER_COMPILE_OK,
    BENDER_COMPILE_ERROR
};
struct BenderCompileResult {
    BenderFuncDef *funcdef;
    BenderString error;
    BenderFiber *macrofiber;
    BenderSourceMapping error_mapping;
    enum BenderCompileStatus status;
};
BENDER_API BenderCompileResult bender_compile(Bender source, BenderTable *env, BenderString where);
BENDER_API BenderCompileResult bender_compile_lint(
    Bender source,
    BenderTable *env,
    BenderString where,
    BenderArray *lints);

/* Get the default environment for bender */
BENDER_API BenderTable *bender_core_env(BenderTable *replacements);
BENDER_API BenderTable *bender_core_lookup_table(BenderTable *replacements);

/* Execute strings.
 *
 * These functions wrap parsing, compilation, and evalutation into convenient functions.
 * */
#define BENDER_DO_ERROR_RUNTIME 0x01
#define BENDER_DO_ERROR_COMPILE 0x02
#define BENDER_DO_ERROR_PARSE 0x04
BENDER_API int bender_dobytes(BenderTable *env, const uint8_t *bytes, int32_t len, const char *sourcePath, Bender *out);
BENDER_API int bender_dostring(BenderTable *env, const char *str, const char *sourcePath, Bender *out);

/* Run the entrypoint of a wrapped program */
BENDER_API int bender_loop_fiber(BenderFiber *fiber);

/* Number scanning */
BENDER_API int bender_scan_number(const uint8_t *str, int32_t len, double *out);
BENDER_API int bender_scan_number_base(const uint8_t *str, int32_t len, int32_t base, double *out);
BENDER_API int bender_scan_int64(const uint8_t *str, int32_t len, int64_t *out);
BENDER_API int bender_scan_uint64(const uint8_t *str, int32_t len, uint64_t *out);
#ifdef BENDER_INT_TYPES
BENDER_API int bender_scan_numeric(const uint8_t *str, int32_t len, Bender *out);
#endif

/* Debugging */
BENDER_API void bender_debug_break(BenderFuncDef *def, int32_t pc);
BENDER_API void bender_debug_unbreak(BenderFuncDef *def, int32_t pc);
BENDER_API void bender_debug_find(
    BenderFuncDef **def_out, int32_t *pc_out,
    BenderString source, int32_t line, int32_t column);

/* RNG */
extern BENDER_API const BenderAbstractType bender_rng_type;
BENDER_API BenderRNG *bender_default_rng(void);
BENDER_API void bender_rng_seed(BenderRNG *rng, uint32_t seed);
BENDER_API void bender_rng_longseed(BenderRNG *rng, const uint8_t *bytes, int32_t len);
BENDER_API uint32_t bender_rng_u32(BenderRNG *rng);
BENDER_API double bender_rng_double(BenderRNG *rng);

/* Array functions */
BENDER_API BenderArray *bender_array(int32_t capacity);
BENDER_API BenderArray *bender_array_weak(int32_t capacity);
BENDER_API BenderArray *bender_array_n(const Bender *elements, int32_t n);
BENDER_API void bender_array_ensure(BenderArray *array, int32_t capacity, int32_t growth);
BENDER_API void bender_array_setcount(BenderArray *array, int32_t count);
BENDER_API void bender_array_push(BenderArray *array, Bender x);
BENDER_API Bender bender_array_pop(BenderArray *array);
BENDER_API Bender bender_array_peek(BenderArray *array);

/* Buffer functions */
#define BENDER_BUFFER_FLAG_NO_REALLOC 0x10000
BENDER_API BenderBuffer *bender_buffer(int32_t capacity);
BENDER_API BenderBuffer *bender_buffer_init(BenderBuffer *buffer, int32_t capacity);
BENDER_API BenderBuffer *bender_pointer_buffer_unsafe(void *memory, int32_t capacity, int32_t count);
BENDER_API void bender_buffer_deinit(BenderBuffer *buffer);
BENDER_API void bender_buffer_ensure(BenderBuffer *buffer, int32_t capacity, int32_t growth);
BENDER_API void bender_buffer_setcount(BenderBuffer *buffer, int32_t count);
BENDER_API void bender_buffer_extra(BenderBuffer *buffer, int32_t n);
BENDER_API void bender_buffer_push_bytes(BenderBuffer *buffer, const uint8_t *string, int32_t len);
BENDER_API void bender_buffer_push_string(BenderBuffer *buffer, BenderString string);
BENDER_API void bender_buffer_push_cstring(BenderBuffer *buffer, const char *cstring);
BENDER_API void bender_buffer_push_u8(BenderBuffer *buffer, uint8_t x);
BENDER_API void bender_buffer_push_u16(BenderBuffer *buffer, uint16_t x);
BENDER_API void bender_buffer_push_u32(BenderBuffer *buffer, uint32_t x);
BENDER_API void bender_buffer_push_u64(BenderBuffer *buffer, uint64_t x);

/* Tuple */

#define BENDER_TUPLE_FLAG_BRACKETCTOR 0x10000

#define bender_tuple_head(t) ((BenderTupleHead *)((char *)t - offsetof(BenderTupleHead, data)))
#define bender_tuple_from_head(gcobject) ((BenderTuple)((char *)gcobject + offsetof(BenderTupleHead, data)))
#define bender_tuple_length(t) (bender_tuple_head(t)->length)
#define bender_tuple_hash(t) (bender_tuple_head(t)->hash)
#define bender_tuple_sm_line(t) (bender_tuple_head(t)->sm_line)
#define bender_tuple_sm_column(t) (bender_tuple_head(t)->sm_column)
#define bender_tuple_flag(t) (bender_tuple_head(t)->gc.flags)
BENDER_API Bender *bender_tuple_begin(int32_t length);
BENDER_API BenderTuple bender_tuple_end(Bender *tuple);
BENDER_API BenderTuple bender_tuple_n(const Bender *values, int32_t n);

/* String/Symbol functions */
#define bender_string_head(s) ((BenderStringHead *)((char *)s - offsetof(BenderStringHead, data)))
#define bender_string_length(s) (bender_string_head(s)->length)
#define bender_string_hash(s) (bender_string_head(s)->hash)
BENDER_API uint8_t *bender_string_begin(int32_t length);
BENDER_API BenderString bender_string_end(uint8_t *str);
BENDER_API BenderString bender_string(const uint8_t *buf, int32_t len);
BENDER_API BenderString bender_cstring(const char *cstring);
BENDER_API int bender_string_compare(BenderString lhs, BenderString rhs);
BENDER_API int bender_string_equal(BenderString lhs, BenderString rhs);
BENDER_API int bender_string_equalconst(BenderString lhs, const uint8_t *rhs, int32_t rlen, int32_t rhash);
BENDER_API BenderString bender_description(Bender x);
BENDER_API BenderString bender_to_string(Bender x);
BENDER_API void bender_to_string_b(BenderBuffer *buffer, Bender x);
BENDER_API void bender_description_b(BenderBuffer *buffer, Bender x);
#define bender_cstringv(cstr) bender_wrap_string(bender_cstring(cstr))
#define bender_stringv(str, len) bender_wrap_string(bender_string((str), (len)))
BENDER_API BenderString bender_formatc(const char *format, ...);
BENDER_API BenderBuffer *bender_formatb(BenderBuffer *bufp, const char *format, ...);
BENDER_API void bender_formatbv(BenderBuffer *bufp, const char *format, va_list args);

/* Symbol functions */
BENDER_API BenderSymbol bender_symbol(const uint8_t *str, int32_t len);
BENDER_API BenderSymbol bender_csymbol(const char *str);
BENDER_API BenderSymbol bender_symbol_gen(void);
#define bender_symbolv(str, len) bender_wrap_symbol(bender_symbol((str), (len)))
#define bender_csymbolv(cstr) bender_wrap_symbol(bender_csymbol(cstr))

/* Keyword functions */
#define bender_keyword bender_symbol
#define bender_ckeyword bender_csymbol
#define bender_keywordv(str, len) bender_wrap_keyword(bender_keyword((str), (len)))
#define bender_ckeywordv(cstr) bender_wrap_keyword(bender_ckeyword(cstr))

/* Structs */
#define bender_struct_head(t) ((BenderStructHead *)((char *)t - offsetof(BenderStructHead, data)))
#define bender_struct_from_head(t) ((BenderStruct)((char *)gcobject + offsetof(BenderStructHead, data)))
#define bender_struct_length(t) (bender_struct_head(t)->length)
#define bender_struct_capacity(t) (bender_struct_head(t)->capacity)
#define bender_struct_hash(t) (bender_struct_head(t)->hash)
#define bender_struct_proto(t) (bender_struct_head(t)->proto)
BENDER_API BenderKV *bender_struct_begin(int32_t count);
BENDER_API void bender_struct_put(BenderKV *st, Bender key, Bender value);
BENDER_API BenderStruct bender_struct_end(BenderKV *st);
BENDER_API Bender bender_struct_get(BenderStruct st, Bender key);
BENDER_API Bender bender_struct_rawget(BenderStruct st, Bender key);
BENDER_API Bender bender_struct_get_ex(BenderStruct st, Bender key, BenderStruct *which);
BENDER_API BenderTable *bender_struct_to_table(BenderStruct st);
BENDER_API const BenderKV *bender_struct_find(BenderStruct st, Bender key);

/* Table functions */
BENDER_API BenderTable *bender_table(int32_t capacity);
BENDER_API BenderTable *bender_table_init(BenderTable *table, int32_t capacity);
BENDER_API BenderTable *bender_table_init_raw(BenderTable *table, int32_t capacity);
BENDER_API void bender_table_deinit(BenderTable *table);
BENDER_API Bender bender_table_get(BenderTable *t, Bender key);
BENDER_API Bender bender_table_get_ex(BenderTable *t, Bender key, BenderTable **which);
BENDER_API Bender bender_table_rawget(BenderTable *t, Bender key);
BENDER_API Bender bender_table_remove(BenderTable *t, Bender key);
BENDER_API void bender_table_put(BenderTable *t, Bender key, Bender value);
BENDER_API BenderStruct bender_table_to_struct(BenderTable *t);
BENDER_API void bender_table_merge_table(BenderTable *table, BenderTable *other);
BENDER_API void bender_table_merge_struct(BenderTable *table, BenderStruct other);
BENDER_API BenderKV *bender_table_find(BenderTable *t, Bender key);
BENDER_API BenderTable *bender_table_clone(BenderTable *table);
BENDER_API void bender_table_clear(BenderTable *table);
BENDER_API BenderTable *bender_table_weakk(int32_t capacity);
BENDER_API BenderTable *bender_table_weakv(int32_t capacity);
BENDER_API BenderTable *bender_table_weakkv(int32_t capacity);

/* Fiber */
BENDER_API BenderFiber *bender_fiber(BenderFunction *callee, int32_t capacity, int32_t argc, const Bender *argv);
BENDER_API BenderFiber *bender_fiber_reset(BenderFiber *fiber, BenderFunction *callee, int32_t argc, const Bender *argv);
BENDER_API BenderFiberStatus bender_fiber_status(BenderFiber *fiber);
BENDER_API int bender_fiber_can_resume(BenderFiber *fiber);
BENDER_API BenderFiber *bender_current_fiber(void);
BENDER_API BenderFiber *bender_root_fiber(void);

/* Treat similar types through uniform interfaces for iteration */
BENDER_API int bender_indexed_view(Bender seq, const Bender **data, int32_t *len);
BENDER_API int bender_bytes_view(Bender str, const uint8_t **data, int32_t *len);
BENDER_API int bender_dictionary_view(Bender tab, const BenderKV **data, int32_t *len, int32_t *cap);
BENDER_API Bender bender_dictionary_get(const BenderKV *data, int32_t cap, Bender key);
BENDER_API const BenderKV *bender_dictionary_next(const BenderKV *kvs, int32_t cap, const BenderKV *kv);

/* Abstract */
#define bender_abstract_head(u) ((BenderAbstractHead *)((char *)u - offsetof(BenderAbstractHead, data)))
#define bender_abstract_from_head(gcobject) ((BenderAbstract)((char *)gcobject + offsetof(BenderAbstractHead, data)))
#define bender_abstract_type(u) (bender_abstract_head(u)->type)
#define bender_abstract_size(u) (bender_abstract_head(u)->size)
BENDER_API void *bender_abstract_begin(const BenderAbstractType *type, size_t size);
BENDER_API BenderAbstract bender_abstract_end(void *abstractTemplate);
BENDER_API BenderAbstract bender_abstract(const BenderAbstractType *type, size_t size); /* begin and end in one call */

/* Native */
typedef void (*BenderModule)(BenderTable *);
typedef BenderBuildConfig(*BenderModconf)(void);
BENDER_API BenderModule bender_native(const char *name, BenderString *error);

/* Marshaling */
#define BENDER_MARSHAL_UNSAFE 0x20000
#define BENDER_MARSHAL_NO_CYCLES 0x40000

BENDER_API void bender_marshal(
    BenderBuffer *buf,
    Bender x,
    BenderTable *rreg,
    int flags);
BENDER_API Bender bender_unmarshal(
    const uint8_t *bytes,
    size_t len,
    int flags,
    BenderTable *reg,
    const uint8_t **next);
BENDER_API BenderTable *bender_env_lookup(BenderTable *env);
BENDER_API void bender_env_lookup_into(BenderTable *renv, BenderTable *env, const char *prefix, int recurse);

/* GC */

/* The main interface to garbage collection. Call this to do a full mark and sweep cleanup. */
BENDER_API void bender_collect(void);

/* Add "roots" to the garbage collector to prevent the runtime from freeing objects.
 * This is only needed if code outside of Bender keeps references to Bender values */
BENDER_API void bender_gcroot(Bender root);
BENDER_API int bender_gcunroot(Bender root);

/* Scoped GC roots for generated/C code (Phase 2b-i). Values pushed on the
 * shadow stack are marked on every collection until popped. Unlike
 * bender_gcroot (a persistent set with search removal), the shadow stack is
 * strictly LIFO: snapshot the top with bender_gcshadow_mark, push values
 * held in C frames, and discard with bender_gcshadow_pop_to when the frame
 * exits. A panicking exit leaks the pushed slots (bounded, exceptional --
 * same convention as bender_gcroot). Pushing never triggers a collection. */
BENDER_API void bender_gcshadow_push(Bender x);
BENDER_API size_t bender_gcshadow_mark(void);
BENDER_API void bender_gcshadow_pop_to(size_t mark);

/* Allow disabling garbage collection temporarily or for certain sections of code.
 * this is a very cheap operation. */
BENDER_API int bender_gclock(void);
BENDER_API void bender_gcunlock(int handle);

/* The mark and sweep components of the mark and sweep collector. Prefer using bender_collect directly. */
BENDER_API void bender_mark(Bender x);
BENDER_API void bender_sweep(void);

/* Clear all gced memory and call all destructors. Used as part of the standard cleanup routune, most programmers will not need this. */
BENDER_API void bender_clear_memory(void);

/* Remove all GC roots. Used as part of the standard cleanup routine, most programmers will not need this. */
BENDER_API int bender_gcunrootall(Bender root);

/* Hint to the collector that memory of size s was just allocated to help it better understand when to free memory. */
BENDER_API void bender_gcpressure(size_t s);

/* Functions */
BENDER_API BenderFuncDef *bender_funcdef_alloc(void);
BENDER_API BenderFunction *bender_thunk(BenderFuncDef *def);

/* Get a function that when called with no args, will return x. */
BENDER_API BenderFunction *bender_thunk_delay(Bender x);

/* Do some simple verfification on constructed bytecode to disallow any trivial incorrect bytecode. */
BENDER_API int bender_verify(BenderFuncDef *def);

/* Pretty printing */
#define BENDER_PRETTY_COLOR 1
#define BENDER_PRETTY_ONELINE 2
#define BENDER_PRETTY_NOTRUNC 4
BENDER_API BenderBuffer *bender_pretty(BenderBuffer *buffer, int depth, int flags, Bender x);

/* Misc */
#ifdef BENDER_PRF
#define BENDER_HASH_KEY_SIZE 16
BENDER_API void bender_init_hash_key(uint8_t key[BENDER_HASH_KEY_SIZE]);
#endif
BENDER_API void bender_try_init(BenderTryState *state);
#if defined(BENDER_BSD) || defined(BENDER_APPLE)
#define bender_try(state) (bender_try_init(state), (BenderSignal) _setjmp((state)->buf))
#else
#define bender_try(state) (bender_try_init(state), (BenderSignal) setjmp((state)->buf))
#endif
BENDER_API void bender_restore(BenderTryState *state);
BENDER_API int bender_equals(Bender x, Bender y);
BENDER_API int32_t bender_hash(Bender x);
BENDER_API int bender_compare(Bender x, Bender y);
BENDER_API int bender_cstrcmp(BenderString str, const char *other);
BENDER_API Bender bender_in(Bender ds, Bender key);
BENDER_API Bender bender_get(Bender ds, Bender key);
BENDER_API Bender bender_next(Bender ds, Bender key);
BENDER_API Bender bender_getindex(Bender ds, int32_t index);
BENDER_API int32_t bender_length(Bender x);
BENDER_API Bender bender_lengthv(Bender x);
BENDER_API void bender_put(Bender ds, Bender key, Bender value);
BENDER_API void bender_putindex(Bender ds, int32_t index, Bender value);
#define bender_flag_at(F, I) ((F) & ((1ULL) << (I)))
BENDER_API Bender bender_wrap_number_safe(double x);
BENDER_API int bender_keyeq(Bender x, const char *cstring);
BENDER_API int bender_streq(Bender x, const char *cstring);
BENDER_API int bender_symeq(Bender x, const char *cstring);
BENDER_API int32_t bender_sorted_keys(const BenderKV *dict, int32_t cap, int32_t *index_buffer);

/* VM functions */
BENDER_API int bender_init(void);
BENDER_API void bender_deinit(void);
BENDER_API BenderVM *bender_vm_alloc(void);
BENDER_API BenderVM *bender_local_vm(void);
BENDER_API void bender_vm_free(BenderVM *vm);
BENDER_API void bender_vm_save(BenderVM *into);
BENDER_API void bender_vm_load(BenderVM *from);
BENDER_API void bender_interpreter_interrupt(BenderVM *vm);
BENDER_API void bender_interpreter_interrupt_handled(BenderVM *vm); /* Call this after running interrupt handler */
BENDER_API BenderSignal bender_continue(BenderFiber *fiber, Bender in, Bender *out);
BENDER_API BenderSignal bender_continue_signal(BenderFiber *fiber, Bender in, Bender *out, BenderSignal sig);
/* Suspend the current fiber with a yield signal carrying v, freezing any
 * live C frames on the fiber's native stack (Phase 1.1 swap-suspension).
 * Returns the resume value. A signal smuggled in on resume (e.g. cancel)
 * raises like a yield point inside run_vm would: ERROR unwinds with a
 * longjmp, anything else re-suspends outward. Panics if there is no
 * current fiber. Values held in C locals across the suspension follow the
 * usual C API contract: root them (bender_gcroot) or keep them in VM
 * stack slots, as the collector cannot see the frozen C stack. */
BENDER_API Bender bender_fiber_yield(Bender v);
BENDER_API BenderSignal bender_pcall(BenderFunction *fun, int32_t argn, const Bender *argv, Bender *out, BenderFiber **f);
BENDER_API BenderSignal bender_step(BenderFiber *fiber, Bender in, Bender *out);
BENDER_API Bender bender_call(BenderFunction *fun, int32_t argc, const Bender *argv);
BENDER_API Bender bender_mcall(const char *name, int32_t argc, Bender *argv);
BENDER_API void bender_stacktrace(BenderFiber *fiber, Bender err);
BENDER_API void bender_stacktrace_ext(BenderFiber *fiber, Bender err, const char *prefix);

/* Sandboxing API */
#define BENDER_SANDBOX_SANDBOX 1
#define BENDER_SANDBOX_SUBPROCESS 2
#define BENDER_SANDBOX_NET_CONNECT 4
#define BENDER_SANDBOX_NET_LISTEN 8
#define BENDER_SANDBOX_FFI_DEFINE 16
#define BENDER_SANDBOX_FS_WRITE 32
#define BENDER_SANDBOX_FS_READ 64
#define BENDER_SANDBOX_HRTIME 128
#define BENDER_SANDBOX_ENV 256
#define BENDER_SANDBOX_DYNAMIC_MODULES 512
#define BENDER_SANDBOX_FS_TEMP 1024
#define BENDER_SANDBOX_FFI_USE 2048
#define BENDER_SANDBOX_FFI_JIT 4096
#define BENDER_SANDBOX_SIGNAL 8192
#define BENDER_SANDBOX_CHROOT 16384
#define BENDER_SANDBOX_FFI (BENDER_SANDBOX_FFI_DEFINE | BENDER_SANDBOX_FFI_USE | BENDER_SANDBOX_FFI_JIT)
#define BENDER_SANDBOX_FS (BENDER_SANDBOX_FS_WRITE | BENDER_SANDBOX_FS_READ | BENDER_SANDBOX_FS_TEMP)
#define BENDER_SANDBOX_NET (BENDER_SANDBOX_NET_CONNECT | BENDER_SANDBOX_NET_LISTEN)
#define BENDER_SANDBOX_COMPILE 32768
#define BENDER_SANDBOX_ASM 65536
#define BENDER_SANDBOX_THREADS 131072
#define BENDER_SANDBOX_UNMARSHAL 262144
#define BENDER_SANDBOX_EXIT 524288
#define BENDER_SANDBOX_LOCALE 1048576
#define BENDER_SANDBOX_ALL (UINT32_MAX)
BENDER_API void bender_sandbox(uint32_t flags);
BENDER_API void bender_sandbox_assert(uint32_t forbidden_flags);

/* Scratch Memory API */
typedef void (*BenderScratchFinalizer)(void *);

BENDER_API void *bender_smalloc(size_t size);
BENDER_API void *bender_srealloc(void *mem, size_t size);
BENDER_API void *bender_scalloc(size_t nmemb, size_t size);
BENDER_API void bender_sfinalizer(void *mem, BenderScratchFinalizer finalizer);
BENDER_API void bender_sfree(void *mem);

/* C Library helpers */
typedef enum {
    BENDER_BINDING_NONE,
    BENDER_BINDING_DEF,
    BENDER_BINDING_VAR,
    BENDER_BINDING_MACRO,
    BENDER_BINDING_DYNAMIC_DEF,
    BENDER_BINDING_DYNAMIC_MACRO
} BenderBindingType;

typedef struct {
    BenderBindingType type;
    Bender value;
    enum {
        BENDER_BINDING_DEP_NONE,
        BENDER_BINDING_DEP_RELAXED,
        BENDER_BINDING_DEP_NORMAL,
        BENDER_BINDING_DEP_STRICT,
    } deprecation;
} BenderBinding;

BENDER_API void bender_def(BenderTable *env, const char *name, Bender val, const char *documentation);
BENDER_API void bender_var(BenderTable *env, const char *name, Bender val, const char *documentation);
BENDER_API void bender_cfuns(BenderTable *env, const char *regprefix, const BenderReg *cfuns);
BENDER_API void bender_cfuns_prefix(BenderTable *env, const char *regprefix, const BenderReg *cfuns);
BENDER_API BenderBindingType bender_resolve(BenderTable *env, BenderSymbol sym, Bender *out);
BENDER_API BenderBinding bender_resolve_ext(BenderTable *env, BenderSymbol sym);

/* Get values from the core environment. */
BENDER_API Bender bender_resolve_core(const char *name);

/* New C API
 *
 * The "New" C API is intended to make constructing good documentation and source maps
 * much more straightforward. This not only ensures doc strings for functions in native
 * modules, it also add source code mapping for C functions so that programmers can see which
 * file and line a native function that calls bender_panic came from.
 *
 * */

#if defined(BENDER_NANBOX_64) && (BENDER_NANBOX_64_POINTER_SHIFT != 0) && !defined(BENDER_MSVC)
#define BENDER_CFUNCTION_ALIGN __attribute__((aligned(1 << BENDER_NANBOX_64_POINTER_SHIFT)))
#else
#define BENDER_CFUNCTION_ALIGN
#endif

/* Shorthand for bender C function declarations */
#define BENDER_CFUN(name) BENDER_CFUNCTION_ALIGN Bender name (int32_t argc, Bender *argv)

/* Declare a C function with documentation and source mapping */
#define BENDER_REG_END {NULL, NULL, NULL, NULL, 0}

/* no docstrings or sourcemaps */
#define BENDER_REG_(JNAME, CNAME) {JNAME, CNAME, NULL, NULL, 0}
#define BENDER_FN_(CNAME, USAGE, DOCSTRING) \
    Bender CNAME (int32_t argc, Bender *argv)
#define BENDER_DEF_(ENV, JNAME, VAL, DOC) \
    bender_def(ENV, JNAME, VAL, NULL)

/* sourcemaps only */
#define BENDER_REG_S(JNAME, CNAME) {JNAME, CNAME, NULL, __FILE__, CNAME##_sourceline_}
#define BENDER_FN_S(CNAME, USAGE, DOCSTRING) \
    static const int32_t CNAME##_sourceline_ = __LINE__; \
    Bender BENDER_CFUNCTION_ALIGN CNAME (int32_t argc, Bender *argv)
#define BENDER_DEF_S(ENV, JNAME, VAL, DOC) \
    bender_def_sm(ENV, JNAME, VAL, NULL, __FILE__, __LINE__)

/* docstring only */
#define BENDER_REG_D(JNAME, CNAME) {JNAME, CNAME, CNAME##_docstring_, NULL, 0}
#define BENDER_FN_D(CNAME, USAGE, DOCSTRING) \
    static const char CNAME##_docstring_[] = USAGE "\n\n" DOCSTRING; \
    Bender BENDER_CFUNCTION_ALIGN CNAME (int32_t argc, Bender *argv)
#define BENDER_DEF_D(ENV, JNAME, VAL, DOC) \
    bender_def(ENV, JNAME, VAL, DOC)

/* sourcemaps and docstrings */
#define BENDER_REG_SD(JNAME, CNAME) {JNAME, CNAME, CNAME##_docstring_, __FILE__, CNAME##_sourceline_}
#define BENDER_FN_SD(CNAME, USAGE, DOCSTRING) \
    static const int32_t CNAME##_sourceline_ = __LINE__; \
    static const char CNAME##_docstring_[] = USAGE "\n\n" DOCSTRING; \
    Bender BENDER_CFUNCTION_ALIGN CNAME (int32_t argc, Bender *argv)
#define BENDER_DEF_SD(ENV, JNAME, VAL, DOC) \
    bender_def_sm(ENV, JNAME, VAL, DOC, __FILE__, __LINE__)

/* Choose defaults for source mapping and docstring based on config defs */
#if defined(BENDER_NO_SOURCEMAPS) && defined(BENDER_NO_DOCSTRINGS)
#define BENDER_REG BENDER_REG_
#define BENDER_FN BENDER_FN_
#define BENDER_DEF BENDER_DEF_
#elif defined(BENDER_NO_SOURCEMAPS) && !defined(BENDER_NO_DOCSTRINGS)
#define BENDER_REG BENDER_REG_D
#define BENDER_FN BENDER_FN_D
#define BENDER_DEF BENDER_DEF_D
#elif !defined(BENDER_NO_SOURCEMAPS) && defined(BENDER_NO_DOCSTRINGS)
#define BENDER_REG BENDER_REG_S
#define BENDER_FN BENDER_FN_S
#define BENDER_DEF BENDER_DEF_S
#elif !defined(BENDER_NO_SOURCEMAPS) && !defined(BENDER_NO_DOCSTRINGS)
#define BENDER_REG BENDER_REG_SD
#define BENDER_FN BENDER_FN_SD
#define BENDER_DEF BENDER_DEF_SD
#endif

/* Define things with source mapping information */
BENDER_API void bender_cfuns_ext(BenderTable *env, const char *regprefix, const BenderRegExt *cfuns);
BENDER_API void bender_cfuns_ext_prefix(BenderTable *env, const char *regprefix, const BenderRegExt *cfuns);
BENDER_API void bender_def_sm(BenderTable *env, const char *name, Bender val, const char *documentation, const char *source_file, int32_t source_line);
BENDER_API void bender_var_sm(BenderTable *env, const char *name, Bender val, const char *documentation, const char *source_file, int32_t source_line);

/* Legacy definition of C functions */
BENDER_API void bender_register(const char *name, BenderCFunction cfun);

/* Allow setting entry name for static libraries */
#ifdef __cplusplus
#define BENDER_MODULE_PREFIX extern "C"
#else
#define BENDER_MODULE_PREFIX
#endif
#ifndef BENDER_ENTRY_NAME
#define BENDER_MODULE_ENTRY \
    BENDER_MODULE_PREFIX BENDER_EXPORT BenderBuildConfig _bender_mod_config(void) { \
        return bender_config_current(); \
    } \
    BENDER_MODULE_PREFIX BENDER_EXPORT void _bender_init
#else
#define BENDER_MODULE_ENTRY BENDER_MODULE_PREFIX BENDER_API void BENDER_ENTRY_NAME
#endif

BENDER_NO_RETURN BENDER_API void bender_signalv(BenderSignal signal, Bender message);
BENDER_NO_RETURN BENDER_API void bender_panicv(Bender message);
BENDER_NO_RETURN BENDER_API void bender_panic(const char *message);
BENDER_NO_RETURN BENDER_API void bender_panics(BenderString message);
BENDER_NO_RETURN BENDER_API void bender_panicf(const char *format, ...);
BENDER_API void bender_dynprintf(const char *name, FILE *dflt_file, const char *format, ...);
#define bender_printf(...) bender_dynprintf("out", stdout, __VA_ARGS__)
#define bender_eprintf(...) bender_dynprintf("err", stderr, __VA_ARGS__)
BENDER_NO_RETURN BENDER_API void bender_panic_type(Bender x, int32_t n, int expected);
BENDER_NO_RETURN BENDER_API void bender_panic_abstract(Bender x, int32_t n, const BenderAbstractType *at);
BENDER_API void bender_arity(int32_t arity, int32_t min, int32_t max);
BENDER_API void bender_fixarity(int32_t arity, int32_t fix);

BENDER_API int bender_getmethod(BenderKeyword method, const BenderMethod *methods, Bender *out);
BENDER_API Bender bender_nextmethod(const BenderMethod *methods, Bender key);

BENDER_API double bender_getnumber(const Bender *argv, int32_t n);
BENDER_API BenderArray *bender_getarray(const Bender *argv, int32_t n);
BENDER_API BenderTuple bender_gettuple(const Bender *argv, int32_t n);
BENDER_API BenderTable *bender_gettable(const Bender *argv, int32_t n);
BENDER_API BenderStruct bender_getstruct(const Bender *argv, int32_t n);
BENDER_API BenderString bender_getstring(const Bender *argv, int32_t n);
BENDER_API const char *bender_getcstring(const Bender *argv, int32_t n);
BENDER_API const char *bender_getcbytes(const Bender *argv, int32_t n);
BENDER_API BenderSymbol bender_getsymbol(const Bender *argv, int32_t n);
BENDER_API BenderKeyword bender_getkeyword(const Bender *argv, int32_t n);
BENDER_API BenderBuffer *bender_getbuffer(const Bender *argv, int32_t n);
BENDER_API BenderFiber *bender_getfiber(const Bender *argv, int32_t n);
BENDER_API BenderFunction *bender_getfunction(const Bender *argv, int32_t n);
BENDER_API BenderCFunction bender_getcfunction(const Bender *argv, int32_t n);
BENDER_API int bender_getboolean(const Bender *argv, int32_t n);
BENDER_API void *bender_getpointer(const Bender *argv, int32_t n);

BENDER_API int32_t bender_getnat(const Bender *argv, int32_t n);
BENDER_API int32_t bender_getinteger(const Bender *argv, int32_t n);
BENDER_API int8_t bender_getinteger8(const Bender *argv, int32_t n);
BENDER_API int16_t bender_getinteger16(const Bender *argv, int32_t n);
BENDER_API int64_t bender_getinteger64(const Bender *argv, int32_t n);
BENDER_API uint32_t bender_getuinteger(const Bender *argv, int32_t n);
BENDER_API uint8_t bender_getuinteger8(const Bender *argv, int32_t n);
BENDER_API uint16_t bender_getuinteger16(const Bender *argv, int32_t n);
BENDER_API uint64_t bender_getuinteger64(const Bender *argv, int32_t n);
BENDER_API size_t bender_getsize(const Bender *argv, int32_t n);
BENDER_API BenderView bender_getindexed(const Bender *argv, int32_t n);
BENDER_API BenderByteView bender_getbytes(const Bender *argv, int32_t n);
BENDER_API BenderDictView bender_getdictionary(const Bender *argv, int32_t n);
BENDER_API void *bender_getabstract(const Bender *argv, int32_t n, const BenderAbstractType *at);
BENDER_API BenderRange bender_getslice(int32_t argc, const Bender *argv);
BENDER_API int32_t bender_gethalfrange(const Bender *argv, int32_t n, int32_t length, const char *which);
BENDER_API int32_t bender_getstartrange(const Bender *argv, int32_t argc, int32_t n, int32_t length);
BENDER_API int32_t bender_getendrange(const Bender *argv, int32_t argc, int32_t n, int32_t length);
BENDER_API int32_t bender_getargindex(const Bender *argv, int32_t n, int32_t length, const char *which);
BENDER_API uint64_t bender_getflags(const Bender *argv, int32_t n, const char *flags);

/* Optionals */
BENDER_API double bender_optnumber(const Bender *argv, int32_t argc, int32_t n, double dflt);
BENDER_API BenderTuple bender_opttuple(const Bender *argv, int32_t argc, int32_t n, BenderTuple dflt);
BENDER_API BenderStruct bender_optstruct(const Bender *argv, int32_t argc, int32_t n, BenderStruct dflt);
BENDER_API BenderString bender_optstring(const Bender *argv, int32_t argc, int32_t n, BenderString dflt);
BENDER_API const char *bender_optcstring(const Bender *argv, int32_t argc, int32_t n, const char *dflt);
BENDER_API const char *bender_optcbytes(const Bender *argv, int32_t argc, int32_t n, const char *dflt);
BENDER_API BenderSymbol bender_optsymbol(const Bender *argv, int32_t argc, int32_t n, BenderString dflt);
BENDER_API BenderKeyword bender_optkeyword(const Bender *argv, int32_t argc, int32_t n, BenderString dflt);
BENDER_API BenderFiber *bender_optfiber(const Bender *argv, int32_t argc, int32_t n, BenderFiber *dflt);
BENDER_API BenderFunction *bender_optfunction(const Bender *argv, int32_t argc, int32_t n, BenderFunction *dflt);
BENDER_API BenderCFunction bender_optcfunction(const Bender *argv, int32_t argc, int32_t n, BenderCFunction dflt);
BENDER_API int bender_optboolean(const Bender *argv, int32_t argc, int32_t n, int dflt);
BENDER_API void *bender_optpointer(const Bender *argv, int32_t argc, int32_t n, void *dflt);
BENDER_API int32_t bender_optnat(const Bender *argv, int32_t argc, int32_t n, int32_t dflt);
BENDER_API int32_t bender_optinteger(const Bender *argv, int32_t argc, int32_t n, int32_t dflt);
BENDER_API int64_t bender_optinteger64(const Bender *argv, int32_t argc, int32_t n, int64_t dflt);
BENDER_API size_t bender_optsize(const Bender *argv, int32_t argc, int32_t n, size_t dflt);
BENDER_API BenderAbstract bender_optabstract(const Bender *argv, int32_t argc, int32_t n, const BenderAbstractType *at, BenderAbstract dflt);
BENDER_API uint32_t bender_optuinteger(const Bender *argv, int32_t argc, int32_t n, uint32_t dflt);
BENDER_API uint64_t bender_optuinteger64(const Bender *argv, int32_t argc, int32_t n, uint64_t dflt);

/* Mutable optional types specify a size default, and construct a new value if none is provided */
BENDER_API BenderBuffer *bender_optbuffer(const Bender *argv, int32_t argc, int32_t n, int32_t dflt_len);
BENDER_API BenderTable *bender_opttable(const Bender *argv, int32_t argc, int32_t n, int32_t dflt_len);
BENDER_API BenderArray *bender_optarray(const Bender *argv, int32_t argc, int32_t n, int32_t dflt_len);

BENDER_API Bender bender_dyn(const char *name);
BENDER_API void bender_setdyn(const char *name, Bender value);

extern BENDER_API const BenderAbstractType bender_file_type;

#define BENDER_FILE_WRITE 1
#define BENDER_FILE_READ 2
#define BENDER_FILE_APPEND 4
#define BENDER_FILE_UPDATE 8
#define BENDER_FILE_NOT_CLOSEABLE 16
#define BENDER_FILE_CLOSED 32
#define BENDER_FILE_BINARY 64
#define BENDER_FILE_SERIALIZABLE 128
#define BENDER_FILE_NONIL 512

BENDER_API Bender bender_makefile(FILE *f, int32_t flags);
BENDER_API BenderFile *bender_makejfile(FILE *f, int32_t flags);
BENDER_API FILE *bender_getfile(const Bender *argv, int32_t n, int32_t *flags);
BENDER_API FILE *bender_dynfile(const char *name, FILE *def);
BENDER_API BenderFile *bender_getjfile(const Bender *argv, int32_t n);
BENDER_API BenderAbstract bender_checkfile(Bender j);
BENDER_API FILE *bender_unwrapfile(Bender j, int32_t *flags);
BENDER_API int bender_file_close(BenderFile *file);

BENDER_API int bender_cryptorand(uint8_t *out, size_t n);

/* Marshal API */
BENDER_API void bender_marshal_size(BenderMarshalContext *ctx, size_t value);
BENDER_API void bender_marshal_int(BenderMarshalContext *ctx, int32_t value);
BENDER_API void bender_marshal_int64(BenderMarshalContext *ctx, int64_t value);
BENDER_API void bender_marshal_ptr(BenderMarshalContext *ctx, const void *value);
BENDER_API void bender_marshal_byte(BenderMarshalContext *ctx, uint8_t value);
BENDER_API void bender_marshal_bytes(BenderMarshalContext *ctx, const uint8_t *bytes, size_t len);
BENDER_API void bender_marshal_bender(BenderMarshalContext *ctx, Bender x);
BENDER_API void bender_marshal_abstract(BenderMarshalContext *ctx, BenderAbstract abstract);
BENDER_API int bender_marshal_flags(BenderMarshalContext *ctx);

BENDER_API void bender_unmarshal_ensure(BenderMarshalContext *ctx, size_t size);
BENDER_API size_t bender_unmarshal_size(BenderMarshalContext *ctx);
BENDER_API int32_t bender_unmarshal_int(BenderMarshalContext *ctx);
BENDER_API int64_t bender_unmarshal_int64(BenderMarshalContext *ctx);
BENDER_API void *bender_unmarshal_ptr(BenderMarshalContext *ctx);
BENDER_API uint8_t bender_unmarshal_byte(BenderMarshalContext *ctx);
BENDER_API void bender_unmarshal_bytes(BenderMarshalContext *ctx, uint8_t *dest, size_t len);
BENDER_API Bender bender_unmarshal_bender(BenderMarshalContext *ctx);
BENDER_API BenderAbstract bender_unmarshal_abstract(BenderMarshalContext *ctx, size_t size);
BENDER_API BenderAbstract bender_unmarshal_abstract_threaded(BenderMarshalContext *ctx, size_t size);
BENDER_API void bender_unmarshal_abstract_reuse(BenderMarshalContext *ctx, void *p);
BENDER_API int bender_unmarshal_flags(BenderMarshalContext *ctx);

BENDER_API void bender_register_abstract_type(const BenderAbstractType *at);
BENDER_API const BenderAbstractType *bender_get_abstract_type(Bender key);

#ifdef BENDER_PEG

extern BENDER_API const BenderAbstractType bender_peg_type;

/* opcodes for peg vm */
typedef enum {
    RULE_LITERAL,      /* [len, bytes...] */
    RULE_NCHAR,        /* [n] */
    RULE_NOTNCHAR,     /* [n] */
    RULE_RANGE,        /* [lo | hi << 16 (1 word)] */
    RULE_SET,          /* [bitmap (8 words)] */
    RULE_LOOK,         /* [offset, rule] */
    RULE_CHOICE,       /* [len, rules...] */
    RULE_SEQUENCE,     /* [len, rules...] */
    RULE_IF,           /* [rule_a, rule_b (b if a)] */
    RULE_IFNOT,        /* [rule_a, rule_b (b if not a)] */
    RULE_NOT,          /* [rule] */
    RULE_BETWEEN,      /* [lo, hi, rule] */
    RULE_GETTAG,       /* [searchtag, tag] */
    RULE_CAPTURE,      /* [rule, tag] */
    RULE_POSITION,     /* [tag] */
    RULE_ARGUMENT,     /* [argument-index, tag] */
    RULE_CONSTANT,     /* [constant, tag] */
    RULE_ACCUMULATE,   /* [rule, tag] */
    RULE_GROUP,        /* [rule, tag] */
    RULE_REPLACE,      /* [rule, constant, tag] */
    RULE_MATCHTIME,    /* [rule, constant, tag] */
    RULE_ERROR,        /* [rule] */
    RULE_DROP,         /* [rule] */
    RULE_BACKMATCH,    /* [tag] */
    RULE_TO,           /* [rule] */
    RULE_THRU,         /* [rule] */
    RULE_LENPREFIX,    /* [rule_a, rule_b (repeat rule_b rule_a times)] */
    RULE_READINT,      /* [(signedness << 4) | (endianness << 5) | bytewidth, tag] */
    RULE_LINE,         /* [tag] */
    RULE_COLUMN,       /* [tag] */
    RULE_UNREF,        /* [rule, tag] */
    RULE_CAPTURE_NUM,  /* [rule, tag] */
    RULE_SUB,          /* [rule, rule] */
    RULE_TIL,          /* [rule, rule] */
    RULE_SPLIT,        /* [rule, rule] */
    RULE_NTH,          /* [nth, rule, tag] */
    RULE_ONLY_TAGS,    /* [rule] */
    RULE_MATCHSPLICE,  /* [rule, constant, tag] */
    RULE_DEBUG,        /* [] */
} BenderPegOpcode;

typedef struct {
    uint32_t *bytecode;
    Bender *constants;
    size_t bytecode_len;
    uint32_t num_constants;
    int has_backref;
} BenderPeg;

#endif

#ifdef BENDER_INT_TYPES

extern BENDER_API const BenderAbstractType bender_s64_type;
extern BENDER_API const BenderAbstractType bender_u64_type;

typedef enum {
    BENDER_INT_NONE,
    BENDER_INT_S64,
    BENDER_INT_U64
} BenderIntType;

BENDER_API BenderIntType bender_is_int(Bender x);
BENDER_API Bender bender_wrap_s64(int64_t x);
BENDER_API Bender bender_wrap_u64(uint64_t x);
BENDER_API int64_t bender_unwrap_s64(Bender x);
BENDER_API uint64_t bender_unwrap_u64(Bender x);
BENDER_API int bender_scan_int64(const uint8_t *str, int32_t len, int64_t *out);
BENDER_API int bender_scan_uint64(const uint8_t *str, int32_t len, uint64_t *out);

#endif

/* Custom allocator support */
BENDER_API void *(bender_malloc)(size_t);
BENDER_API void *(bender_realloc)(void *, size_t);
BENDER_API void *(bender_calloc)(size_t, size_t);
BENDER_API void (bender_free)(void *);
#ifndef bender_malloc
#define bender_malloc(X) malloc((X))
#endif
#ifndef bender_realloc
#define bender_realloc(X, Y) realloc((X), (Y))
#endif
#ifndef bender_calloc
#define bender_calloc(X, Y) calloc((X), (Y))
#endif
#ifndef bender_free
#define bender_free(X) free((X))
#endif

/***** END SECTION MAIN *****/

/* Re-enable popped variable length array warnings */
#ifdef _MSC_VER
#pragma warning( pop )
#endif

#ifdef __cplusplus
}
#endif

#endif /* BENDER_H_defined */
