/*
* Copyright (c) 2026 Calvin Rose and contributors.
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

#include <stdlib.h>

#ifndef BENDER_REDUCED_OS

#include <time.h>
#include <fcntl.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <signal.h>
#include <locale.h>
#include <inttypes.h>

#ifdef BENDER_BSD
#include <sys/sysctl.h>
#endif

#ifdef BENDER_LINUX
#include <sched.h>
#endif

#ifdef BENDER_GNU_HURD
/* Ideally we would try to dynamically allocate */
#define PATH_MAX 8192
#endif

#ifdef BENDER_WINDOWS
#include <windows.h>
#include <direct.h>
#include <sys/utime.h>
#include <io.h>
#include <process.h>
#define BENDER_SPAWN_CHDIR
#else
#ifndef BENDER_PLAN9
#include <spawn.h>
#endif
#include <utime.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/types.h>
#include <sys/wait.h>
#ifdef BENDER_APPLE
#include <crt_externs.h>
#define environ (*_NSGetEnviron())
#include <AvailabilityMacros.h>
#include <sys/sysctl.h>
int chroot(const char *dirname);
#elif !defined(BENDER_PLAN9)
extern char **environ;
#endif
#ifdef BENDER_THREADS
#include <pthread.h>
#endif
#endif

/* Detect availability of posix_spawn_file_actions_addchdir_np. Since
 * this doesn't seem to follow any standard, just a common extension, we
 * must enumerate supported systems for availability. Define BENDER_SPAWN_NO_CHDIR
 * to disable this. */
#ifndef BENDER_SPAWN_NO_CHDIR
#ifdef __GLIBC__
#define BENDER_SPAWN_CHDIR
#elif defined(BENDER_APPLE)
/* The posix_spawn_file_actions_addchdir_np function
 * has only been implemented since macOS 10.15 */
#if defined(MAC_OS_X_VERSION_10_15) && (MAC_OS_X_VERSION_MIN_REQUIRED >= MAC_OS_X_VERSION_10_15)
#define BENDER_SPAWN_CHDIR
#else
#define BENDER_SPAWN_NO_CHDIR
#endif
#elif defined(__FreeBSD__) /* Not all BSDs work, for example openBSD doesn't seem to support this */
#define BENDER_SPAWN_CHDIR
#endif
#endif

/* Not POSIX, but all Unixes but Solaris have this function. */
#if defined(BENDER_POSIX) && !defined(__sun)
time_t timegm(struct tm *tm);
#elif defined(BENDER_WINDOWS)
#define timegm _mkgmtime
#endif

/* Access to some global variables should be synchronized if not in single threaded mode, as
 * setenv/getenv are not thread safe. */
#ifdef BENDER_THREADS
# ifdef BENDER_WINDOWS
static CRITICAL_SECTION env_lock;
static void bender_lock_environ(void) {
    EnterCriticalSection(&env_lock);
}
static void bender_unlock_environ(void) {
    LeaveCriticalSection(&env_lock);
}
# else
static pthread_mutex_t env_lock = PTHREAD_MUTEX_INITIALIZER;
static void bender_lock_environ(void) {
    pthread_mutex_lock(&env_lock);
}
static void bender_unlock_environ(void) {
    pthread_mutex_unlock(&env_lock);
}
# endif
#else
static void bender_lock_environ(void) {
}
static void bender_unlock_environ(void) {
}
#endif

#endif /* BENDER_REDCUED_OS */

/* Core OS functions */

/* Full OS functions */

#define bender_stringify1(x) #x
#define bender_stringify(x) bender_stringify1(x)

BENDER_CORE_FN(os_which,
              "(os/which &opt test)",
              "Check the current operating system. If `test` is nil or unset, Returns one of:\n\n"
              "* :windows\n\n"
              "* :mingw\n\n"
              "* :cygwin\n\n"
              "* :macos\n\n"
              "* :web - Web assembly (emscripten)\n\n"
              "* :linux\n\n"
              "* :hurd\n\n"
              "* :freebsd\n\n"
              "* :openbsd\n\n"
              "* :netbsd\n\n"
              "* :dragonfly\n\n"
              "* :bsd\n\n"
              "* :posix - A POSIX compatible system (default)\n\n"
              "May also return a custom keyword specified at build time. Is `test` is truthy, will check if the current operating system equals `test` and return true if they are the same, false otherwise.") {
    bender_arity(argc, 0, 1);
    if (argc == 1 && bender_truthy(argv[0])) {
        bender_getkeyword(argv, 0); /* Constrain to keywords */
        return bender_wrap_boolean(bender_equals(argv[0], os_which(0, NULL)));
    }
#if defined(BENDER_OS_NAME)
    return bender_ckeywordv(bender_stringify(BENDER_OS_NAME));
#elif defined(BENDER_MINGW)
    return bender_ckeywordv("mingw");
#elif defined(BENDER_CYGWIN)
    return bender_ckeywordv("cygwin");
#elif defined(BENDER_WINDOWS)
    return bender_ckeywordv("windows");
#elif defined(BENDER_APPLE)
    return bender_ckeywordv("macos");
#elif defined(__EMSCRIPTEN__)
    return bender_ckeywordv("web");
#elif defined(BENDER_LINUX)
    return bender_ckeywordv("linux");
#elif defined(BENDER_GNU_HURD)
    return bender_ckeywordv("hurd");
#elif defined(__FreeBSD__)
    return bender_ckeywordv("freebsd");
#elif defined(__NetBSD__)
    return bender_ckeywordv("netbsd");
#elif defined(__OpenBSD__)
    return bender_ckeywordv("openbsd");
#elif defined(__DragonFly__)
    return bender_ckeywordv("dragonfly");
#elif defined(BENDER_BSD)
    return bender_ckeywordv("bsd");
#elif defined(BENDER_ILLUMOS)
    return bender_ckeywordv("illumos");
#else
    return bender_ckeywordv("posix");
#endif
}

/* Detect the ISA we are compiled for */
BENDER_CORE_FN(os_arch,
              "(os/arch)",
              "Check the ISA that bender was compiled for. Returns one of:\n\n"
              "* :x86\n\n"
              "* :x64\n\n"
              "* :arm\n\n"
              "* :aarch64\n\n"
              "* :riscv32\n\n"
              "* :riscv64\n\n"
              "* :sparc\n\n"
              "* :wasm\n\n"
              "* :s390\n\n"
              "* :s390x\n\n"
              "* :unknown\n") {
    bender_fixarity(argc, 0);
    (void) argv;
    /* Check 64-bit vs 32-bit */
#if defined(BENDER_ARCH_NAME)
    return bender_ckeywordv(bender_stringify(BENDER_ARCH_NAME));
#elif defined(__EMSCRIPTEN__)
    return bender_ckeywordv("wasm");
#elif (defined(__x86_64__) || defined(_M_X64))
    return bender_ckeywordv("x64");
#elif defined(__i386) || defined(_M_IX86)
    return bender_ckeywordv("x86");
#elif defined(_M_ARM64) || defined(__aarch64__)
    return bender_ckeywordv("aarch64");
#elif defined(_M_ARM) || defined(__arm__)
    return bender_ckeywordv("arm");
#elif (defined(__riscv) && (__riscv_xlen == 64))
    return bender_ckeywordv("riscv64");
#elif (defined(__riscv) && (__riscv_xlen == 32))
    return bender_ckeywordv("riscv32");
#elif (defined(__sparc__))
    return bender_ckeywordv("sparc");
#elif (defined(__ppc__))
    return bender_ckeywordv("ppc");
#elif (defined(__ppc64__) || defined(_ARCH_PPC64) || defined(_M_PPC))
    return bender_ckeywordv("ppc64");
#elif (defined(__s390x__))
    return bender_ckeywordv("s390x");
#elif (defined(__s390__))
    return bender_ckeywordv("s390");
#else
    return bender_ckeywordv("unknown");
#endif
}

/* Detect the compiler used to build the interpreter */
BENDER_CORE_FN(os_compiler,
              "(os/compiler)",
              "Get the compiler used to compile the interpreter. Returns one of:\n\n"
              "* :gcc\n\n"
              "* :clang\n\n"
              "* :msvc\n\n"
              "* :kencc\n\n"
              "* :unknown\n\n") {
    bender_fixarity(argc, 0);
    (void) argv;
#if defined(_MSC_VER)
    return bender_ckeywordv("msvc");
#elif defined(__clang__)
    return bender_ckeywordv("clang");
#elif defined(__GNUC__)
    return bender_ckeywordv("gcc");
#elif defined(BENDER_PLAN9)
    return bender_ckeywordv("kencc");
#else
    return bender_ckeywordv("unknown");
#endif
}

#undef bender_stringify1
#undef bender_stringify

BENDER_CORE_FN(os_exit,
              "(os/exit &opt x force)",
              "Exit from bender with an exit code equal to x. If x is not an integer, "
              "the exit with status equal the hash of x. If `force` is truthy will exit immediately and "
              "skip cleanup code.") {
    bender_arity(argc, 0, 2);
    bender_sandbox_assert(BENDER_SANDBOX_EXIT);
    int status;
    if (argc == 0) {
        status = EXIT_SUCCESS;
    } else if (bender_checkint(argv[0])) {
        status = bender_unwrap_integer(argv[0]);
    } else {
        status = EXIT_FAILURE;
    }
    int force = (argc >= 2 && bender_truthy(argv[1]));
    bender_deinit();
    if (force) {
#ifdef BENDER_PLAN9
        exits(nil);
#else
        _Exit(status);
#endif
    } else {
        exit(status);
    }
    return bender_wrap_nil();
}

#ifndef BENDER_REDUCED_OS

BENDER_CORE_FN(os_cpu_count,
              "(os/cpu-count &opt dflt)",
              "Get an approximate number of CPUs available on for this process to use. If "
              "unable to get an approximation, will return a default value dflt.") {
    bender_arity(argc, 0, 1);
    (void) argv; /* Prevent unused argument warning */
#ifdef BENDER_WINDOWS
    SYSTEM_INFO info;
    GetSystemInfo(&info);
    return bender_wrap_integer(info.dwNumberOfProcessors);
#elif defined(BENDER_LINUX)
    cpu_set_t cs;
    CPU_ZERO(&cs);
    sched_getaffinity(0, sizeof(cs), &cs);
    int count = CPU_COUNT(&cs);
    return bender_wrap_integer(count);
#elif defined(BENDER_APPLE)
    /* macOS/iOS define BENDER_APPLE, NOT BENDER_BSD, so the two sysctl branches
     * below never compiled here and os/cpu-count silently returned nil -- which
     * is how a caller doing (- (os/cpu-count) 1) got a nil-arithmetic error
     * instead of a processor count. HW_NCPU is present on Darwin and reports
     * the online processor count. */
    {
        int name[2] = {CTL_HW, HW_NCPU};
        int result = 0;
        size_t len = sizeof(result);
        if (-1 == sysctl(name, 2, &result, &len, NULL, 0)) {
            return argc > 0 ? argv[0] : bender_wrap_nil();
        }
        return bender_wrap_integer(result);
    }
#elif defined(BENDER_BSD) && defined(HW_NCPUONLINE)
    const int name[2] = {CTL_HW, HW_NCPUONLINE};
    int result = 0;
    size_t len = sizeof(int);
    if (-1 == sysctl(name, 2, &result, &len, NULL, 0)) {
        return argc > 0 ? argv[0] : bender_wrap_nil();
    }
    return bender_wrap_integer(result);
#elif defined(BENDER_BSD) && defined(HW_NCPU)
    const int name[2] = {CTL_HW, HW_NCPU};
    int result = 0;
    size_t len = sizeof(int);
    if (-1 == sysctl(name, 2, &result, &len, NULL, 0)) {
        return argc > 0 ? argv[0] : bender_wrap_nil();
    }
    return bender_wrap_integer(result);
#elif defined(BENDER_ILLUMOS)
    long result = sysconf(_SC_NPROCESSORS_CONF);
    if (result < 0) {
        return argc > 0 ? argv[0] : bender_wrap_nil();
    }
    return bender_wrap_integer(result);
#elif defined(BENDER_PLAN9)
    return bender_wrap_integer(atoi(getenv("NPROC")));
#else
    return argc > 0 ? argv[0] : bender_wrap_nil();
#endif
}

#ifndef BENDER_NO_PROCESSES

/* Get env for os_execute */
#ifdef BENDER_WINDOWS
typedef char *EnvBlock;
#else
typedef char **EnvBlock;
#endif

/* Get env for os_execute */
static EnvBlock os_execute_env(int32_t argc, const Bender *argv) {
    if (argc <= 2) return NULL;
    BenderDictView dict = bender_getdictionary(argv, 2);
#ifdef BENDER_WINDOWS
    BenderBuffer *temp = bender_buffer(10);
    for (int32_t i = 0; i < dict.cap; i++) {
        const BenderKV *kv = dict.kvs + i;
        if (!bender_checktype(kv->key, BENDER_STRING)) continue;
        if (!bender_checktype(kv->value, BENDER_STRING)) continue;
        const uint8_t *keys = bender_unwrap_string(kv->key);
        const uint8_t *vals = bender_unwrap_string(kv->value);
        bender_buffer_push_bytes(temp, keys, bender_string_length(keys));
        bender_buffer_push_u8(temp, '=');
        bender_buffer_push_bytes(temp, vals, bender_string_length(vals));
        bender_buffer_push_u8(temp, '\0');
    }
    /* Windows environment blocks must be double-NULL terminated */
    if (temp->count == 0) bender_buffer_push_u8(temp, '\0');
    bender_buffer_push_u8(temp, '\0');
    char *ret = bender_smalloc(temp->count);
    memcpy(ret, temp->data, temp->count);
    return ret;
#else
    char **envp = bender_smalloc(sizeof(char *) * ((size_t)dict.len + 1));
    int32_t j = 0;
    for (int32_t i = 0; i < dict.cap; i++) {
        const BenderKV *kv = dict.kvs + i;
        if (!bender_checktype(kv->key, BENDER_STRING)) continue;
        if (!bender_checktype(kv->value, BENDER_STRING)) continue;
        const uint8_t *keys = bender_unwrap_string(kv->key);
        const uint8_t *vals = bender_unwrap_string(kv->value);
        int32_t klen = bender_string_length(keys);
        int32_t vlen = bender_string_length(vals);
        /* Check keys has no embedded 0s or =s. */
        int skip = 0;
        for (int32_t k = 0; k < klen; k++) {
            if (keys[k] == '\0' || keys[k] == '=') {
                skip = 1;
                break;
            }
        }
        if (skip) continue;
        char *envitem = bender_smalloc((size_t) klen + (size_t) vlen + 2);
        memcpy(envitem, keys, klen);
        envitem[klen] = '=';
        memcpy(envitem + klen + 1, vals, vlen);
        envitem[klen + vlen + 1] = 0;
        envp[j++] = envitem;
    }
    envp[j] = NULL;
    return envp;
#endif
}

static void os_execute_cleanup(EnvBlock envp, const char **child_argv) {
#ifdef BENDER_WINDOWS
    (void) child_argv;
    if (NULL != envp) bender_sfree(envp);
#else
    bender_sfree((void *)child_argv);
    if (NULL != envp) {
        char **envitem = envp;
        while (*envitem != NULL) {
            bender_sfree(*envitem);
            envitem++;
        }
    }
    bender_sfree(envp);
#endif
}

#ifdef BENDER_WINDOWS
/* Windows processes created via CreateProcess get only one command line argument string, and
 * must parse this themselves. Each processes is free to do this however they like, but the
 * standard parsing method is CommandLineToArgvW. We need to properly escape arguments into
 * a single string of this format. Returns a buffer that can be cast into a c string. */
static BenderBuffer *os_exec_escape(BenderView args) {
    BenderBuffer *b = bender_buffer(0);
    for (int32_t i = 0; i < args.len; i++) {
        const char *arg = bender_getcstring(args.items, i);

        /* Push leading space if not first */
        if (i) bender_buffer_push_u8(b, ' ');

        /* Find first special character */
        const char *first_spec = arg;
        while (*first_spec) {
            switch (*first_spec) {
                case ' ':
                case '\t':
                case '\v':
                case '\n':
                case '"':
                    goto found;
                case '\0':
                    bender_panic("embedded 0 not allowed in command line string");
                default:
                    first_spec++;
                    break;
            }
        }
    found:

        /* Check if needs escape */
        if (*first_spec == '\0') {
            /* No escape needed */
            bender_buffer_push_cstring(b, arg);
        } else {
            /* Escape */
            bender_buffer_push_u8(b, '"');
            for (const char *c = arg; ; c++) {
                unsigned numBackSlashes = 0;
                while (*c == '\\') {
                    c++;
                    numBackSlashes++;
                }
                if (*c == '"') {
                    /* Escape all backslashes and double quote mark */
                    int32_t n = 2 * numBackSlashes + 1;
                    bender_buffer_extra(b, n + 1);
                    memset(b->data + b->count, '\\', n);
                    b->count += n;
                    bender_buffer_push_u8(b, '"');
                } else if (*c) {
                    /* Don't escape backslashes. */
                    int32_t n = numBackSlashes;
                    bender_buffer_extra(b, n + 1);
                    memset(b->data + b->count, '\\', n);
                    b->count += n;
                    bender_buffer_push_u8(b, *c);
                } else {
                    /* we finished Escape all backslashes */
                    int32_t n = 2 * numBackSlashes;
                    bender_buffer_extra(b, n + 1);
                    memset(b->data + b->count, '\\', n);
                    b->count += n;
                    break;
                }
            }
            bender_buffer_push_u8(b, '"');
        }
    }
    bender_buffer_push_u8(b, 0);
    return b;
}
#endif

/* Process type for when running a subprocess and not immediately waiting */
static const BenderAbstractType ProcAT;
#define BENDER_PROC_CLOSED 1
#define BENDER_PROC_WAITED 2
#define BENDER_PROC_WAITING 4
#define BENDER_PROC_ERROR_NONZERO 8
#define BENDER_PROC_OWNS_STDIN 16
#define BENDER_PROC_OWNS_STDOUT 32
#define BENDER_PROC_OWNS_STDERR 64
#define BENDER_PROC_ALLOW_ZOMBIE 128
typedef struct {
    int flags;
#ifdef BENDER_WINDOWS
    HANDLE pHandle;
    HANDLE tHandle;
#else
    pid_t pid;
#endif
    int return_code;
#ifdef BENDER_EV
    BenderStream *in;
    BenderStream *out;
    BenderStream *err;
#else
    BenderFile *in;
    BenderFile *out;
    BenderFile *err;
#endif
} BenderProc;

#ifdef BENDER_EV

#ifdef BENDER_WINDOWS

static BenderEVGenericMessage bender_proc_wait_subr(BenderEVGenericMessage args) {
    BenderProc *proc = (BenderProc *) args.argp;
    WaitForSingleObject(proc->pHandle, INFINITE);
    DWORD exitcode = 0;
    GetExitCodeProcess(proc->pHandle, &exitcode);
    args.tag = (int32_t) exitcode;
    return args;
}

#else /* windows check */

static int proc_get_status(BenderProc *proc) {
    /* Use POSIX shell semantics for interpreting signals */
    int status = 0;
    pid_t result;
    do {
        result = waitpid(proc->pid, &status, 0);
    } while (result == -1 && errno == EINTR);
    if (WIFEXITED(status)) {
        status = WEXITSTATUS(status);
    } else if (WIFSTOPPED(status)) {
        status = WSTOPSIG(status) + 128;
    } else if (WIFSIGNALED(status)) {
        status = WTERMSIG(status) + 128;
    } else {
        /* Could possibly return -1 but for now, just panic */
        bender_panicf("Undefined status code for process termination, %d.", status);
    }
    return status;
}

/* Function that is called in separate thread to wait on a pid */
static BenderEVGenericMessage bender_proc_wait_subr(BenderEVGenericMessage args) {
    BenderProc *proc = (BenderProc *) args.argp;
    args.tag = proc_get_status(proc);
    return args;
}

#endif /* End windows check */

/* Callback that is called in main thread when subroutine completes. */
static void bender_proc_wait_cb(BenderEVGenericMessage args) {
    BenderProc *proc = (BenderProc *) args.argp;
    if (NULL != proc) {
        int status = args.tag;
        proc->return_code = (int32_t) status;
        proc->flags |= BENDER_PROC_WAITED;
        proc->flags &= ~BENDER_PROC_WAITING;
        bender_gcunroot(bender_wrap_abstract(proc));
        bender_gcunroot(bender_wrap_fiber(args.fiber));
        uint32_t sched_id = (uint32_t) args.argi;
        if (bender_fiber_can_resume(args.fiber) && args.fiber->sched_id == sched_id) {
            if ((status != 0) && (proc->flags & BENDER_PROC_ERROR_NONZERO)) {
                BenderString s = bender_formatc("command failed with non-zero exit code %d", status);
                bender_cancel(args.fiber, bender_wrap_string(s));
            } else {
                bender_schedule(args.fiber, bender_wrap_integer(status));
            }
        }
    }
}

#endif /* End ev check */

static int bender_proc_gc(void *p, size_t s) {
    (void) s;
    BenderProc *proc = (BenderProc *) p;
#ifdef BENDER_WINDOWS
    if (!(proc->flags & BENDER_PROC_CLOSED)) {
        if (!(proc->flags & BENDER_PROC_ALLOW_ZOMBIE)) {
            TerminateProcess(proc->pHandle, 1);
        }
        CloseHandle(proc->pHandle);
        CloseHandle(proc->tHandle);
    }
#else
    if (!(proc->flags & (BENDER_PROC_WAITED | BENDER_PROC_ALLOW_ZOMBIE))) {
        /* Kill and wait to prevent zombies */
        kill(proc->pid, SIGKILL);
        int status;
        if (!(proc->flags & BENDER_PROC_WAITING)) {
            waitpid(proc->pid, &status, 0);
        }
    }
#endif
    return 0;
}

static int bender_proc_mark(void *p, size_t s) {
    (void) s;
    BenderProc *proc = (BenderProc *)p;
    if (NULL != proc->in) bender_mark(bender_wrap_abstract(proc->in));
    if (NULL != proc->out) bender_mark(bender_wrap_abstract(proc->out));
    if (NULL != proc->err) bender_mark(bender_wrap_abstract(proc->err));
    return 0;
}

#ifdef BENDER_EV
static BENDER_NO_RETURN void
#else
static Bender
#endif
os_proc_wait_impl(BenderProc *proc) {
    if (proc->flags & (BENDER_PROC_WAITED | BENDER_PROC_WAITING)) {
        bender_panicf("cannot wait twice on a process");
    }
#ifdef BENDER_EV
    /* Event loop implementation - threaded call */
    proc->flags |= BENDER_PROC_WAITING;
    BenderEVGenericMessage targs;
    memset(&targs, 0, sizeof(targs));
    targs.argp = proc;
    targs.fiber = bender_root_fiber();
    targs.argi = (uint32_t) targs.fiber->sched_id;
    bender_gcroot(bender_wrap_abstract(proc));
    bender_gcroot(bender_wrap_fiber(targs.fiber));
    bender_ev_threaded_call(bender_proc_wait_subr, targs, bender_proc_wait_cb);
    bender_await();
#else
    /* Non evented implementation */
    proc->flags |= BENDER_PROC_WAITED;
    int status = 0;
#ifdef BENDER_WINDOWS
    WaitForSingleObject(proc->pHandle, INFINITE);
    GetExitCodeProcess(proc->pHandle, &status);
    if (!(proc->flags & BENDER_PROC_CLOSED)) {
        proc->flags |= BENDER_PROC_CLOSED;
        CloseHandle(proc->pHandle);
        CloseHandle(proc->tHandle);
    }
#else
    waitpid(proc->pid, &status, 0);
#endif
    proc->return_code = (int32_t) status;
    return bender_wrap_integer(proc->return_code);
#endif
}

BENDER_CORE_FN(os_proc_wait,
              "(os/proc-wait proc)",
              "Suspend the current fiber until the subprocess `proc` completes. Once `proc` "
              "completes, return the exit code of `proc`. If called more than once on the same "
              "core/process value, will raise an error. When creating subprocesses using "
              "`os/spawn`, this function should be called on the returned value to avoid zombie "
              "processes.") {
    bender_fixarity(argc, 1);
    BenderProc *proc = bender_getabstract(argv, 0, &ProcAT);
#ifdef BENDER_EV
    os_proc_wait_impl(proc);
#else
    return os_proc_wait_impl(proc);
#endif
}

struct keyword_signal {
    const char *keyword;
    int signal;
};

#ifndef BENDER_WINDOWS
static const struct keyword_signal signal_keywords[] = {
#ifdef SIGKILL
    {"kill", SIGKILL},
#endif
    {"int", SIGINT},
    {"abrt", SIGABRT},
    {"fpe", SIGFPE},
    {"ill", SIGILL},
    {"segv", SIGSEGV},
#ifdef SIGTERM
    {"term", SIGTERM},
#endif
#ifdef SIGALRM
    {"alrm", SIGALRM},
#endif
#ifdef SIGHUP
    {"hup", SIGHUP},
#endif
#ifdef SIGPIPE
    {"pipe", SIGPIPE},
#endif
#ifdef SIGQUIT
    {"quit", SIGQUIT},
#endif
#ifdef SIGUSR1
    {"usr1", SIGUSR1},
#endif
#ifdef SIGUSR2
    {"usr2", SIGUSR2},
#endif
#ifdef SIGCHLD
    {"chld", SIGCHLD},
#endif
#ifdef SIGCONT
    {"cont", SIGCONT},
#endif
#ifdef SIGSTOP
    {"stop", SIGSTOP},
#endif
#ifdef SIGTSTP
    {"tstp", SIGTSTP},
#endif
#ifdef SIGTTIN
    {"ttin", SIGTTIN},
#endif
#ifdef SIGTTOU
    {"ttou", SIGTTOU},
#endif
#ifdef SIGBUS
    {"bus", SIGBUS},
#endif
#ifdef SIGPOLL
    {"poll", SIGPOLL},
#endif
#ifdef SIGPROF
    {"prof", SIGPROF},
#endif
#ifdef SIGSYS
    {"sys", SIGSYS},
#endif
#ifdef SIGTRAP
    {"trap", SIGTRAP},
#endif
#ifdef SIGURG
    {"urg", SIGURG},
#endif
#ifdef SIGVTALRM
    {"vtlarm", SIGVTALRM},
#endif
#ifdef SIGXCPU
    {"xcpu", SIGXCPU},
#endif
#ifdef SIGXFSZ
    {"xfsz", SIGXFSZ},
#endif
    {NULL, 0},
};

static int get_signal_kw(const Bender *argv, int32_t n) {
    BenderKeyword signal_kw = bender_getkeyword(argv, n);
    const struct keyword_signal *ptr = signal_keywords;
    while (ptr->keyword) {
        if (!bender_cstrcmp(signal_kw, ptr->keyword)) {
            return ptr->signal;
        }
        ptr++;
    }
    bender_panicf("undefined signal %v", argv[n]);
}
#endif

BENDER_CORE_FN(os_proc_kill,
              "(os/proc-kill proc &opt wait which)",
              "Kill the subprocess `proc` by sending SIGKILL to it on POSIX "
              "systems, or by closing the process handle on Windows. If "
              "`proc` has already completed, raise an error. If `wait` is "
              "truthy, waits for `proc` to complete and return the exit "
              "code (this raises an error if `proc` is being waited for). "
              "Otherwise, return `proc`. Optional argument `which` is a "
              "keyword named after its C counterpart but in lowercase with "
              "the leading SIG stripped. If `which` is provided, the "
              "corresponding signal is sent instead of SIGKILL. `which` is "
              "ignored on Windows.") {
    bender_arity(argc, 1, 3);
    BenderProc *proc = bender_getabstract(argv, 0, &ProcAT);
    if (proc->flags & BENDER_PROC_WAITED) {
        bender_panicf("cannot kill process that has already finished");
    }
#ifdef BENDER_WINDOWS
    if (proc->flags & BENDER_PROC_CLOSED) {
        bender_panicf("cannot close process handle that is already closed");
    }
    proc->flags |= BENDER_PROC_CLOSED;
    TerminateProcess(proc->pHandle, 1);
    CloseHandle(proc->pHandle);
    CloseHandle(proc->tHandle);
#else
    int signal = -1;
    if (argc == 3) {
        signal = get_signal_kw(argv, 2);
    }
    int status = kill(proc->pid, signal == -1 ? SIGKILL : signal);
    if (status) {
        bender_panic(bender_strerror(errno));
    }
#endif
    /* After killing process we wait on it. */
    if (argc > 1 && bender_truthy(argv[1])) {
#ifdef BENDER_EV
        os_proc_wait_impl(proc);
#else
        return os_proc_wait_impl(proc);
#endif
    } else {
        return argv[0];
    }
}

BENDER_CORE_FN(os_proc_close,
              "(os/proc-close proc)",
              "Close pipes created for subprocess `proc` by `os/spawn` if they have not been "
              "closed. Then, if `proc` is not being waited for, wait. If this function waits, when "
              "`proc` completes, return the exit code of `proc`. Otherwise, return nil.") {
    bender_fixarity(argc, 1);
    BenderProc *proc = bender_getabstract(argv, 0, &ProcAT);
#ifdef BENDER_EV
    if (proc->flags & BENDER_PROC_OWNS_STDIN) bender_stream_close(proc->in);
    if (proc->flags & BENDER_PROC_OWNS_STDOUT) bender_stream_close(proc->out);
    if (proc->flags & BENDER_PROC_OWNS_STDERR) bender_stream_close(proc->err);
#else
    if (proc->flags & BENDER_PROC_OWNS_STDIN) bender_file_close(proc->in);
    if (proc->flags & BENDER_PROC_OWNS_STDOUT) bender_file_close(proc->out);
    if (proc->flags & BENDER_PROC_OWNS_STDERR) bender_file_close(proc->err);
#endif
    proc->flags &= ~(BENDER_PROC_OWNS_STDIN | BENDER_PROC_OWNS_STDOUT | BENDER_PROC_OWNS_STDERR);
    if (proc->flags & (BENDER_PROC_WAITED | BENDER_PROC_WAITING)) {
        return bender_wrap_nil();
    }
#ifdef BENDER_EV
    os_proc_wait_impl(proc);
#else
    return os_proc_wait_impl(proc);
#endif
}

BENDER_CORE_FN(os_proc_getpid,
              "(os/getpid)",
              "Get the process ID of the current process.") {
    bender_sandbox_assert(BENDER_SANDBOX_SUBPROCESS);
    bender_fixarity(argc, 0);
    (void) argv;
#ifdef BENDER_WINDOWS
    return bender_wrap_number((double) _getpid());
#else
    return bender_wrap_number((double) getpid());
#endif
}

static void swap_handles(BenderHandle *handles) {
    BenderHandle temp = handles[0];
    handles[0] = handles[1];
    handles[1] = temp;
}

static void close_handle(BenderHandle handle) {
#ifdef BENDER_WINDOWS
    CloseHandle(handle);
#else
    close(handle);
#endif
}

#ifdef BENDER_EV

#ifndef BENDER_WINDOWS
static void bender_signal_callback(BenderEVGenericMessage msg) {
    int sig = msg.tag;
    if (msg.argi) bender_interpreter_interrupt_handled(NULL);
    Bender handlerv = bender_table_get(&bender_vm.signal_handlers, bender_wrap_integer(sig));
    if (!bender_checktype(handlerv, BENDER_FUNCTION)) {
        /* Let another thread/process try to handle this */
        sigset_t set;
        sigemptyset(&set);
        sigaddset(&set, sig);
#ifdef BENDER_THREADS
        pthread_sigmask(SIG_BLOCK, &set, NULL);
#else
        sigprocmask(SIG_BLOCK, &set, NULL);
#endif
        raise(sig);
        return;
    }
    BenderFunction *handler = bender_unwrap_function(handlerv);
    BenderFiber *fiber = bender_fiber(handler, 64, 0, NULL);
    bender_schedule_soon(fiber, bender_wrap_nil(), BENDER_SIGNAL_OK);
}

static void bender_signal_trampoline_no_interrupt(int sig) {
    /* Do not interact with global bender state here except for bender_ev_post_event, unsafe! */
    BenderEVGenericMessage msg;
    memset(&msg, 0, sizeof(msg));
    msg.tag = sig;
    bender_ev_post_event(&bender_vm, bender_signal_callback, msg);
}

static void bender_signal_trampoline(int sig) {
    /* Do not interact with global bender state here except for bender_ev_post_event, unsafe! */
    BenderEVGenericMessage msg;
    memset(&msg, 0, sizeof(msg));
    msg.tag = sig;
    msg.argi = 1;
    bender_interpreter_interrupt(NULL);
    bender_ev_post_event(&bender_vm, bender_signal_callback, msg);
}
#endif

BENDER_CORE_FN(os_sigaction,
              "(os/sigaction which &opt handler interrupt-interpreter)",
              "Add a signal handler for a given action. Use nil for the `handler` argument to remove a signal handler. "
              "All signal handlers are the same as supported by `os/proc-kill`.") {
    bender_sandbox_assert(BENDER_SANDBOX_SIGNAL);
    bender_arity(argc, 1, 3);
#ifdef BENDER_WINDOWS
    (void) argv;
    bender_panic("unsupported on this platform");
#else
    /* TODO - per thread signal masks */
    int rc;
    int sig = get_signal_kw(argv, 0);
    BenderFunction *handler = bender_optfunction(argv, argc, 1, NULL);
    int can_interrupt = bender_optboolean(argv, argc, 2, 0);
    Bender oldhandler = bender_table_get(&bender_vm.signal_handlers, bender_wrap_integer(sig));
    if (!bender_checktype(oldhandler, BENDER_NIL)) {
        bender_gcunroot(oldhandler);
    }
    if (NULL != handler) {
        Bender handlerv = bender_wrap_function(handler);
        bender_gcroot(handlerv);
        bender_table_put(&bender_vm.signal_handlers, bender_wrap_integer(sig), handlerv);
    } else {
        bender_table_put(&bender_vm.signal_handlers, bender_wrap_integer(sig), bender_wrap_nil());
    }
    struct sigaction action;
    sigset_t mask;
    sigaddset(&mask, sig);
    memset(&action, 0, sizeof(action));
    action.sa_flags |= SA_RESTART;
    if (can_interrupt) {
#ifdef BENDER_NO_INTERPRETER_INTERRUPT
        bender_panic("interpreter interrupt not enabled");
#else
        action.sa_handler = bender_signal_trampoline;
#endif
    } else {
        action.sa_handler = bender_signal_trampoline_no_interrupt;
    }
    action.sa_mask = mask;
    RETRY_EINTR(rc, sigaction(sig, &action, NULL));
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, sig);
#ifdef BENDER_THREADS
    pthread_sigmask(SIG_UNBLOCK, &set, NULL);
#else
    sigprocmask(SIG_UNBLOCK, &set, NULL);
#endif
    return bender_wrap_nil();
#endif
}

#endif

/* Create piped file for os/execute and os/spawn. Need to be careful that we mark
   the error flag if we can't create pipe and don't leak handles. *handle will be cleaned
   up by the calling function. If everything goes well, *handle is owned by the calling function,
   (if it is set) and the returned handle owns the other end of the pipe, which will be closed
   on GC or fclose. */
static BenderHandle make_pipes(BenderHandle *handle, int reverse, int *errflag) {
    BenderHandle handles[2];
#ifdef BENDER_EV

    /* non-blocking pipes */
    if (bender_make_pipe(handles, reverse ? 2 : 1)) goto error;
    if (reverse) swap_handles(handles);
#ifdef BENDER_WINDOWS
    if (!SetHandleInformation(handles[0], HANDLE_FLAG_INHERIT, 0)) goto error;
#endif
    *handle = handles[1];
    return handles[0];

#else

    /* Normal blocking pipes */
#ifdef BENDER_WINDOWS
    SECURITY_ATTRIBUTES saAttr;
    memset(&saAttr, 0, sizeof(saAttr));
    saAttr.nLength = sizeof(saAttr);
    saAttr.bInheritHandle = TRUE;
    if (!CreatePipe(handles, handles + 1, &saAttr, 0)) goto error;
    if (reverse) swap_handles(handles);
    /* Don't inherit the side of the pipe owned by this process */
    if (!SetHandleInformation(handles[0], HANDLE_FLAG_INHERIT, 0)) goto error;
    *handle = handles[1];
    return handles[0];
#else
    if (pipe(handles)) goto error;
    if (reverse) swap_handles(handles);
    *handle = handles[1];
    return handles[0];
#endif

#endif
error:
    *errflag = 1;
    return BENDER_HANDLE_NONE;
}

static const BenderMethod proc_methods[] = {
    {"wait", os_proc_wait},
    {"kill", os_proc_kill},
    {"close", os_proc_close},
    /* dud methods for bender_proc_next */
    {"in", NULL},
    {"out", NULL},
    {"err", NULL},
    {NULL, NULL}
};

static int bender_proc_get(void *p, Bender key, Bender *out) {
    BenderProc *proc = (BenderProc *)p;
    if (bender_keyeq(key, "in")) {
        *out = (NULL == proc->in) ? bender_wrap_nil() : bender_wrap_abstract(proc->in);
        return 1;
    }
    if (bender_keyeq(key, "out")) {
        *out = (NULL == proc->out) ? bender_wrap_nil() : bender_wrap_abstract(proc->out);
        return 1;
    }
    if (bender_keyeq(key, "err")) {
        *out = (NULL == proc->err) ? bender_wrap_nil() : bender_wrap_abstract(proc->err);
        return 1;
    }
#ifndef BENDER_WINDOWS
    if (bender_keyeq(key, "pid")) {
        *out = bender_wrap_number(proc->pid);
        return 1;
    }
#endif
    if ((-1 != proc->return_code) && bender_keyeq(key, "return-code")) {
        *out = bender_wrap_integer(proc->return_code);
        return 1;
    }
    if (!bender_checktype(key, BENDER_KEYWORD)) return 0;
    return bender_getmethod(bender_unwrap_keyword(key), proc_methods, out);
}

static Bender bender_proc_next(void *p, Bender key) {
    (void) p;
    return bender_nextmethod(proc_methods, key);
}

static const BenderAbstractType ProcAT = {
    "core/process",
    bender_proc_gc,
    bender_proc_mark,
    bender_proc_get,
    NULL, /* put */
    NULL, /* marshal */
    NULL, /* unmarshal */
    NULL, /* tostring */
    NULL, /* compare */
    NULL, /* hash */
    bender_proc_next,
    BENDER_ATEND_NEXT
};

static BenderHandle bender_getjstream(Bender *argv, int32_t n, void **orig) {
#ifdef BENDER_EV
    BenderStream *stream = bender_checkabstract(argv[n], &bender_stream_type);
    if (stream != NULL) {
        if (stream->flags & BENDER_STREAM_CLOSED)
            bender_panic("stream is closed");
        *orig = stream;
        return stream->handle;
    }
#endif
    BenderFile *f = bender_checkabstract(argv[n], &bender_file_type);
    if (f != NULL) {
        if (f->flags & BENDER_FILE_CLOSED) {
            bender_panic("file is closed");
        }
        *orig = f;
#ifdef BENDER_WINDOWS
        return (HANDLE) _get_osfhandle(_fileno(f->file));
#else
        return fileno(f->file);
#endif
    }
    bender_panicf("expected file|stream, got %v", argv[n]);
}

#ifdef BENDER_EV
static BenderStream *get_stdio_for_handle(BenderHandle handle, void *orig, int iswrite) {
    if (orig == NULL) {
        return bender_stream(handle, iswrite ? BENDER_STREAM_WRITABLE : BENDER_STREAM_READABLE, NULL);
    } else if (bender_abstract_type(orig) == &bender_file_type) {
        BenderFile *jf = (BenderFile *)orig;
        uint32_t flags = 0;
        if (jf->flags & BENDER_FILE_WRITE) {
            flags |= BENDER_STREAM_WRITABLE;
        }
        if (jf->flags & BENDER_FILE_READ) {
            flags |= BENDER_STREAM_READABLE;
        }
        /* duplicate handle when converting file to stream */
#ifdef BENDER_WINDOWS
        HANDLE prochandle = GetCurrentProcess();
        HANDLE newHandle = INVALID_HANDLE_VALUE;
        if (!DuplicateHandle(prochandle, handle, prochandle, &newHandle, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
            return NULL;
        }
#else
        int newHandle = dup(handle);
        if (newHandle < 0) {
            return NULL;
        }
#endif
        return bender_stream(newHandle, flags, NULL);
    } else {
        return orig;
    }
}
#else
static BenderFile *get_stdio_for_handle(BenderHandle handle, void *orig, int iswrite) {
    if (NULL != orig) return (BenderFile *) orig;
#ifdef BENDER_WINDOWS
    int fd = _open_osfhandle((intptr_t) handle, iswrite ? _O_WRONLY : _O_RDONLY);
    if (-1 == fd) return NULL;
    FILE *f = _fdopen(fd, iswrite ? "w" : "r");
    if (NULL == f) {
        _close(fd);
        return NULL;
    }
#else
    FILE *f = fdopen(handle, iswrite ? "w" : "r");
    if (NULL == f) return NULL;
#endif
    return bender_makejfile(f, iswrite ? BENDER_FILE_WRITE : BENDER_FILE_READ);
}
#endif

typedef enum {
    BENDER_EXECUTE_EXECUTE,
    BENDER_EXECUTE_SPAWN,
    BENDER_EXECUTE_EXEC
} BenderExecuteMode;

static Bender os_execute_impl(int32_t argc, Bender *argv, BenderExecuteMode mode) {
    bender_sandbox_assert(BENDER_SANDBOX_SUBPROCESS);
    bender_arity(argc, 1, 3);

    /* Get flags */
    int is_spawn = mode == BENDER_EXECUTE_SPAWN;
    uint64_t flags = 0;
    if (argc > 1) {
        flags = bender_getflags(argv, 1, "epxd");
    }

    /* Get environment */
    int use_environ = !bender_flag_at(flags, 0);
    EnvBlock envp = os_execute_env(argc, argv);

    /* Get arguments */
    BenderView exargs = bender_getindexed(argv, 0);
    if (exargs.len < 1) {
        bender_panic("expected at least 1 command line argument");
    }

    /* Optional stdio redirections */
    BenderAbstract orig_in = NULL, orig_out = NULL, orig_err = NULL;
    BenderHandle new_in = BENDER_HANDLE_NONE, new_out = BENDER_HANDLE_NONE, new_err = BENDER_HANDLE_NONE;
    BenderHandle pipe_in = BENDER_HANDLE_NONE, pipe_out = BENDER_HANDLE_NONE, pipe_err = BENDER_HANDLE_NONE;
    int stderr_is_stdout = 0;
    int pipe_errflag = 0; /* Track errors setting up pipes */
    int pipe_owner_flags = (is_spawn && (flags & 0x8)) ? BENDER_PROC_ALLOW_ZOMBIE : 0;

    /* Get optional redirections */
    if (argc > 2 && (mode != BENDER_EXECUTE_EXEC)) {
        BenderDictView tab = bender_getdictionary(argv, 2);
        Bender maybe_stdin = bender_dictionary_get(tab.kvs, tab.cap, bender_ckeywordv("in"));
        Bender maybe_stdout = bender_dictionary_get(tab.kvs, tab.cap, bender_ckeywordv("out"));
        Bender maybe_stderr = bender_dictionary_get(tab.kvs, tab.cap, bender_ckeywordv("err"));
        if (is_spawn && bender_keyeq(maybe_stdin, "pipe")) {
            new_in = make_pipes(&pipe_in, 1, &pipe_errflag);
            pipe_owner_flags |= BENDER_PROC_OWNS_STDIN;
        } else if (!bender_checktype(maybe_stdin, BENDER_NIL)) {
            new_in = bender_getjstream(&maybe_stdin, 0, &orig_in);
        }
        if (is_spawn && bender_keyeq(maybe_stdout, "pipe")) {
            new_out = make_pipes(&pipe_out, 0, &pipe_errflag);
            pipe_owner_flags |= BENDER_PROC_OWNS_STDOUT;
        } else if (!bender_checktype(maybe_stdout, BENDER_NIL)) {
            new_out = bender_getjstream(&maybe_stdout, 0, &orig_out);
        }
        if (is_spawn && bender_keyeq(maybe_stderr, "pipe")) {
            new_err = make_pipes(&pipe_err, 0, &pipe_errflag);
            pipe_owner_flags |= BENDER_PROC_OWNS_STDERR;
        } else if (bender_keyeq(maybe_stderr, "out")) {
            stderr_is_stdout = 1;
        } else if (!bender_checktype(maybe_stderr, BENDER_NIL)) {
            new_err = bender_getjstream(&maybe_stderr, 0, &orig_err);
        }
    }

    /* Optional working directory. Available for both os/execute and os/spawn. */
    const char *chdir_path = NULL;
    if (argc > 2) {
        BenderDictView tab = bender_getdictionary(argv, 2);
        Bender workdir = bender_dictionary_get(tab.kvs, tab.cap, bender_ckeywordv("cd"));
        if (bender_checktype(workdir, BENDER_STRING)) {
            chdir_path = (const char *) bender_unwrap_string(workdir);
#ifndef BENDER_SPAWN_CHDIR
            bender_panicf(":cd argument not supported on this system - %s", chdir_path);
#endif
        } else if (!bender_checktype(workdir, BENDER_NIL)) {
            bender_panicf("expected string for :cd argumnet, got %v", workdir);
        }
    }

    /* Clean up if any of the pipes have any issues */
    if (pipe_errflag) {
        if (pipe_in != BENDER_HANDLE_NONE) close_handle(pipe_in);
        if (pipe_out != BENDER_HANDLE_NONE) close_handle(pipe_out);
        if (pipe_err != BENDER_HANDLE_NONE) close_handle(pipe_err);
        bender_panic("failed to create pipes");
    }

#ifdef BENDER_WINDOWS

    HANDLE pHandle, tHandle;
    SECURITY_ATTRIBUTES saAttr;
    PROCESS_INFORMATION processInfo;
    STARTUPINFO startupInfo;
    LPCSTR lpCurrentDirectory = NULL;
    memset(&saAttr, 0, sizeof(saAttr));
    memset(&processInfo, 0, sizeof(processInfo));
    memset(&startupInfo, 0, sizeof(startupInfo));
    startupInfo.cb = sizeof(startupInfo);
    startupInfo.dwFlags |= STARTF_USESTDHANDLES;
    saAttr.nLength = sizeof(saAttr);

    BenderBuffer *buf = os_exec_escape(exargs);
    if (buf->count > 8191) {
        if (pipe_in != BENDER_HANDLE_NONE) CloseHandle(pipe_in);
        if (pipe_out != BENDER_HANDLE_NONE) CloseHandle(pipe_out);
        if (pipe_err != BENDER_HANDLE_NONE) CloseHandle(pipe_err);
        bender_panic("command line string too long (max 8191 characters)");
    }
    const char *path = (const char *) bender_unwrap_string(exargs.items[0]);

    if (chdir_path != NULL) {
        lpCurrentDirectory = chdir_path;
    }

    /* Do IO redirection */

    if (pipe_in != BENDER_HANDLE_NONE) {
        startupInfo.hStdInput = pipe_in;
    } else if (new_in != BENDER_HANDLE_NONE) {
        startupInfo.hStdInput = new_in;
    } else {
        startupInfo.hStdInput = (HANDLE) _get_osfhandle(_fileno(stdin));
    }

    if (pipe_out != BENDER_HANDLE_NONE) {
        startupInfo.hStdOutput = pipe_out;
    } else if (new_out != BENDER_HANDLE_NONE) {
        startupInfo.hStdOutput = new_out;
    } else {
        startupInfo.hStdOutput = (HANDLE) _get_osfhandle(_fileno(stdout));
    }

    if (pipe_err != BENDER_HANDLE_NONE) {
        startupInfo.hStdError = pipe_err;
    } else if (new_err != NULL) {
        startupInfo.hStdError = new_err;
    } else if (stderr_is_stdout) {
        startupInfo.hStdError = startupInfo.hStdOutput;
    } else {
        startupInfo.hStdError = (HANDLE) _get_osfhandle(_fileno(stderr));
    }

    int cp_failed = 0;
    DWORD cp_error_code = 0;
    if (!CreateProcess(bender_flag_at(flags, 1) ? NULL : path,
                       (char *) buf->data, /* Single CLI argument */
                       &saAttr, /* no proc inheritance */
                       &saAttr, /* no thread inheritance */
                       TRUE, /* handle inheritance */
                       0, /* flags */
                       use_environ ? NULL : envp, /* pass in environment */
                       lpCurrentDirectory,
                       &startupInfo,
                       &processInfo)) {
        cp_failed = 1;
        cp_error_code = GetLastError();
    }

    if (pipe_in != BENDER_HANDLE_NONE) CloseHandle(pipe_in);
    if (pipe_out != BENDER_HANDLE_NONE) CloseHandle(pipe_out);
    if (pipe_err != BENDER_HANDLE_NONE) CloseHandle(pipe_err);

    os_execute_cleanup(envp, NULL);

    if (cp_failed)  {
        char msgbuf[256];
        msgbuf[0] = '\0';
        FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                       NULL,
                       cp_error_code,
                       MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
                       msgbuf,
                       sizeof(msgbuf),
                       NULL);
        if (!*msgbuf) snprintf(msgbuf, sizeof(msgbuf), "%" PRIu32, (uint32_t) cp_error_code);
        char *c = msgbuf;
        while (*c) {
            if (*c == '\n' || *c == '\r') {
                *c = '\0';
                break;
            }
            c++;
        }
        bender_panicf("failed to create process: %V", bender_cstringv(msgbuf));
    }

    pHandle = processInfo.hProcess;
    tHandle = processInfo.hThread;

#else

    /* Result */
    int status = 0;

    const char **child_argv = bender_smalloc(sizeof(char *) * ((size_t) exargs.len + 1));
    for (int32_t i = 0; i < exargs.len; i++)
        child_argv[i] = bender_getcstring(exargs.items, i);
    child_argv[exargs.len] = NULL;
    /* Coerce to form that works for spawn. I'm fairly confident no implementation
     * of posix_spawn would modify the argv array passed in. */
    char *const *cargv = (char *const *)child_argv;

    if (use_environ) {
        bender_lock_environ();
    }

    /* exec mode */
    if (mode == BENDER_EXECUTE_EXEC) {
        int status;
#ifdef BENDER_PLAN9
        status = exec(cargv[0], cargv);
#else
        if (!use_environ) {
            environ = envp;
        }
        do {
            if (bender_flag_at(flags, 1)) {
                status = execvp(cargv[0], cargv);
            } else {
                status = execv(cargv[0], cargv);
            }
        } while (status == -1 && errno == EINTR);
#endif
        bender_panicf("%p: %s", cargv[0], bender_strerror(errno ? errno : ENOENT));
    }

#ifndef BENDER_NO_SPAWN
    /* Use posix_spawn to spawn new process */

    /* Posix spawn setup */
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
#ifdef BENDER_SPAWN_CHDIR
    if (chdir_path != NULL) {
#ifdef BENDER_SPAWN_CHDIR_NO_NP
        posix_spawn_file_actions_addchdir(&actions, chdir_path);
#else
        posix_spawn_file_actions_addchdir_np(&actions, chdir_path);
#endif
    }
#endif
    if (pipe_in != BENDER_HANDLE_NONE) {
        posix_spawn_file_actions_adddup2(&actions, pipe_in, 0);
        posix_spawn_file_actions_addclose(&actions, pipe_in);
    } else if (new_in != BENDER_HANDLE_NONE && new_in != 0) {
        posix_spawn_file_actions_adddup2(&actions, new_in, 0);
        if (new_in != new_out && new_in != new_err)
            posix_spawn_file_actions_addclose(&actions, new_in);
    }
    if (pipe_out != BENDER_HANDLE_NONE) {
        posix_spawn_file_actions_adddup2(&actions, pipe_out, 1);
        posix_spawn_file_actions_addclose(&actions, pipe_out);
    } else if (new_out != BENDER_HANDLE_NONE && new_out != 1) {
        posix_spawn_file_actions_adddup2(&actions, new_out, 1);
        if (new_out != new_err)
            posix_spawn_file_actions_addclose(&actions, new_out);
    }
    if (pipe_err != BENDER_HANDLE_NONE) {
        posix_spawn_file_actions_adddup2(&actions, pipe_err, 2);
        posix_spawn_file_actions_addclose(&actions, pipe_err);
    } else if (new_err != BENDER_HANDLE_NONE && new_err != 2) {
        posix_spawn_file_actions_adddup2(&actions, new_err, 2);
        posix_spawn_file_actions_addclose(&actions, new_err);
    } else if (stderr_is_stdout) {
        posix_spawn_file_actions_adddup2(&actions, 1, 2);
    }

    pid_t pid;
    if (bender_flag_at(flags, 1)) {
        status = posix_spawnp(&pid,
                              child_argv[0], &actions, NULL, cargv,
                              use_environ ? environ : envp);
    } else {
        status = posix_spawn(&pid,
                             child_argv[0], &actions, NULL, cargv,
                             use_environ ? environ : envp);
    }

    posix_spawn_file_actions_destroy(&actions);

    if (pipe_in != BENDER_HANDLE_NONE) close(pipe_in);
    if (pipe_out != BENDER_HANDLE_NONE) close(pipe_out);
    if (pipe_err != BENDER_HANDLE_NONE) close(pipe_err);

    if (use_environ) {
        bender_unlock_environ();
    }

    os_execute_cleanup(envp, child_argv);
    if (status) {
        /* correct for macos bug where errno is not set */
        bender_panicf("%p: %s", argv[0], bender_strerror(errno ? errno : ENOENT));
    }

#endif
#endif
#ifndef BENDER_NO_SPAWN
    BenderProc *proc = bender_abstract(&ProcAT, sizeof(BenderProc));
    proc->return_code = -1;
#ifdef BENDER_WINDOWS
    proc->pHandle = pHandle;
    proc->tHandle = tHandle;
#else
    proc->pid = pid;
#endif
    proc->in = NULL;
    proc->out = NULL;
    proc->err = NULL;
    proc->flags = pipe_owner_flags;
    if (bender_flag_at(flags, 2)) {
        proc->flags |= BENDER_PROC_ERROR_NONZERO;
    }
    if (is_spawn) {
        /* Only set up pointers to stdin, stdout, and stderr if os/spawn. */
        if (new_in != BENDER_HANDLE_NONE) {
            proc->in = get_stdio_for_handle(new_in, orig_in, 1);
            if (NULL == proc->in) bender_panic("failed to construct proc");
        }
        if (new_out != BENDER_HANDLE_NONE) {
            proc->out = get_stdio_for_handle(new_out, orig_out, 0);
            if (NULL == proc->out) bender_panic("failed to construct proc");
        }
        if (new_err != BENDER_HANDLE_NONE) {
            proc->err = get_stdio_for_handle(new_err, orig_err, 0);
            if (NULL == proc->err) bender_panic("failed to construct proc");
        }
        return bender_wrap_abstract(proc);
    } else {
#ifdef BENDER_EV
        os_proc_wait_impl(proc);
#else
        return os_proc_wait_impl(proc);
#endif
    }
#endif
}

BENDER_CORE_FN(os_execute,
              "(os/execute args &opt flags env)",
              "Execute a program on the system and return the exit code. `args` is an array/tuple "
              "of strings. The first string is the name of the program and the remainder are "
              "arguments passed to the program. `flags` is a keyword made from the following "
              "characters that modifies how the program executes:\n"
              "* :e - enables passing an environment to the program. Without 'e', the "
              "current environment is inherited.\n"
              "* :p - allows searching the current PATH for the program to execute. "
              "Without this flag, the first element of `args` must be an absolute path.\n"
              "* :x - raises error if exit code is non-zero.\n"
              "* :d - prevents the garbage collector terminating the program (if still running) "
              "and calling the equivalent of `os/proc-wait` (allows zombie processes).\n"
              "`env` is a table/struct mapping environment variables to values. It can also "
              "contain the keys :in, :out, and :err, which allow redirecting stdio in the "
              "subprocess. :in, :out, and :err should be core/file or core/stream values. "
              "If core/stream values are used, the caller is responsible for ensuring pipes do not "
              "cause the program to block and deadlock.") {
    return os_execute_impl(argc, argv, BENDER_EXECUTE_EXECUTE);
}

BENDER_CORE_FN(os_spawn,
              "(os/spawn args &opt flags env)",
              "Execute a program on the system and return a core/process value representing the "
              "spawned subprocess. Takes the same arguments as `os/execute` but does not wait for "
              "the subprocess to complete. Unlike `os/execute`, the value `:pipe` can be used for "
              ":in, :out and :err keys in `env`. If used, the returned core/process will have a "
              "writable stream in the :in field and readable streams in the :out and :err fields. "
              "On non-Windows systems, the subprocess PID will be in the :pid field. The caller is "
              "responsible for waiting on the process (e.g. by calling `os/proc-wait` on the "
              "returned core/process value) to avoid creating zombie process. After the subprocess "
              "completes, the exit value is in the :return-code field. If `flags` includes 'x', a "
              "non-zero exit code will cause a waiting fiber to raise an error. The use of "
              "`:pipe` may fail if there are too many active file descriptors. The caller is "
              "responsible for closing pipes created by `:pipe` (either individually or using "
              "`os/proc-close`). Similar to `os/execute`, the caller is responsible for ensuring "
              "pipes do not cause the program to block and deadlock. As a special case, the stream passed to `:err` "
              "can be the keyword `:out` to redirect stderr to stdout in the subprocess.") {
    return os_execute_impl(argc, argv, BENDER_EXECUTE_SPAWN);
}

BENDER_CORE_FN(os_posix_exec,
              "(os/posix-exec args &opt flags env)",
              "Use the execvpe or execve system calls to replace the current process with an interface similar to os/execute. "
              "However, instead of creating a subprocess, the current process is replaced. Is not supported on Windows, and "
              "does not allow redirection of stdio.") {
#ifdef BENDER_WINDOWS
    (void) argc;
    (void) argv;
    bender_panic("not supported on Windows");
#else
    return os_execute_impl(argc, argv, BENDER_EXECUTE_EXEC);
#endif
}

BENDER_CORE_FN(os_posix_fork,
              "(os/posix-fork)",
              "Make a `fork` system call and create a new process. Return nil if in the new process, otherwise a core/process object (as returned by os/spawn). "
              "Not supported on all systems (POSIX and Plan 9 only).") {
    bender_sandbox_assert(BENDER_SANDBOX_SUBPROCESS);
    bender_fixarity(argc, 0);
    (void) argv;
#ifdef BENDER_WINDOWS
    bender_panic("not supported on Windows");
#else
    pid_t result;
#ifdef BENDER_PLAN9
    result = fork();
#else
    do {
        result = fork();
    } while (result == -1 && errno == EINTR);
#endif
    if (result == -1) {
        bender_panic(bender_strerror(errno));
    }
    if (result) {
        BenderProc *proc = bender_abstract(&ProcAT, sizeof(BenderProc));
        memset(proc, 0, sizeof(BenderProc));
        proc->pid = result;
        proc->flags = BENDER_PROC_ALLOW_ZOMBIE;
        return bender_wrap_abstract(proc);
    }
    return bender_wrap_nil();
#endif
}

BENDER_CORE_FN(os_posix_chroot,
              "(os/posix-chroot dir)",
              "Call `chroot` to change the root directory to a directory "
              "`dir`. Only supported on POSIX systems.") {
    bender_sandbox_assert(BENDER_SANDBOX_CHROOT);
    bender_fixarity(argc, 1);
#if defined(BENDER_WINDOWS) || defined(BENDER_PLAN9)
    (void) argv;
    bender_panic("not supported on Windows or Plan 9");
#else
    const char *root = bender_getcstring(argv, 0);
    int result;
    do {
        result = chroot(root);
    } while (result == -1 && errno == EINTR);
    if (result == -1) {
        bender_panic(bender_strerror(errno));
    }
    return bender_wrap_nil();
#endif
}

#ifdef BENDER_EV
/* Runs in a separate thread */
static BenderEVGenericMessage os_shell_subr(BenderEVGenericMessage args) {
    int stat = system((const char *) args.argp);
    bender_free(args.argp);
    args.argp = NULL;
    if (args.argi) {
        args.tag = BENDER_EV_TCTAG_INTEGER;
    } else {
        args.tag = BENDER_EV_TCTAG_BOOLEAN;
    }
    args.argi = stat;
    return args;
}
#endif

BENDER_CORE_FN(os_shell,
              "(os/shell str)",
              "Pass a command string `str` directly to the system shell. Prefer `os/execute` "
              "in most cases.") {
    bender_sandbox_assert(BENDER_SANDBOX_SUBPROCESS);
    bender_arity(argc, 0, 1);
    const char *cmd = argc
                      ? bender_getcstring(argv, 0)
                      : NULL;
#ifdef BENDER_EV
    char *cmd_copy = NULL;
    if (cmd != NULL) {
        size_t cmdlen = strlen(cmd);
        cmd_copy = bender_malloc(cmdlen + 1);
        memcpy(cmd_copy, cmd, cmdlen);
        cmd_copy[cmdlen] = '\0';
    }
    bender_ev_threaded_await(os_shell_subr, 0, argc, cmd_copy);
#else
    int stat = system(cmd);
    return argc
           ? bender_wrap_integer(stat)
           : bender_wrap_boolean(stat);
#endif
}

#endif /* BENDER_NO_PROCESSES */

#ifndef BENDER_PLAN9
BENDER_CORE_FN(os_environ,
              "(os/environ)",
              "Get a copy of the OS environment table.") {
    bender_sandbox_assert(BENDER_SANDBOX_ENV);
    (void) argv;
    bender_fixarity(argc, 0);
    int32_t nenv = 0;
    bender_lock_environ();
    char **env = environ;
    while (*env++)
        nenv += 1;
    BenderTable *t = bender_table(nenv);
    for (int32_t i = 0; i < nenv; i++) {
        char *e = environ[i];
        char *eq = strchr(e, '=');
        if (!eq) {
            bender_unlock_environ();
            bender_panic("no '=' in environ");
        }
        char *v = eq + 1;
        int32_t full_len = (int32_t) strlen(e);
        int32_t val_len = (int32_t) strlen(v);
        bender_table_put(
            t,
            bender_stringv((const uint8_t *)e, full_len - val_len - 1),
            bender_stringv((const uint8_t *)v, val_len)
        );
    }
    bender_unlock_environ();
    return bender_wrap_table(t);
}
#endif

BENDER_CORE_FN(os_getenv,
              "(os/getenv variable &opt dflt)",
              "Get the string value of an environment variable.") {
    bender_sandbox_assert(BENDER_SANDBOX_ENV);
    bender_arity(argc, 1, 2);
    const char *cstr = bender_getcstring(argv, 0);
    bender_lock_environ();
    const char *res = getenv(cstr);
    Bender ret = res
                ? bender_cstringv(res)
                : argc == 2
                ? argv[1]
                : bender_wrap_nil();
    bender_unlock_environ();
    return ret;
}

BENDER_CORE_FN(os_setenv,
              "(os/setenv variable value)",
              "Set an environment variable.") {
#ifdef BENDER_WINDOWS
#define SETENV(K,V) _putenv_s(K, V)
#define UNSETENV(K) _putenv_s(K, "")
#elif defined(BENDER_PLAN9)
#define SETENV(K,V) putenv(K, V)
#define UNSETENV(K) unsetenv(K)
#else
#define SETENV(K,V) setenv(K, V, 1)
#define UNSETENV(K) unsetenv(K)
#endif
    bender_sandbox_assert(BENDER_SANDBOX_ENV);
    bender_arity(argc, 1, 2);
    const char *ks = bender_getcstring(argv, 0);
    const char *vs = bender_optcstring(argv, argc, 1, NULL);
    bender_lock_environ();
    int env_status;
    if (NULL == vs) {
        env_status = UNSETENV(ks);
    } else {
        env_status = SETENV(ks, vs);
    }
    /* Diagnostic state normally changes through os/setenv. Keep its
     * once-per-VM fast path coherent without re-reading the process
     * environment on every allocation or write barrier. Foreign C code
     * mutating environ directly is picked up by the next bender_init. */
    if (!env_status && !strcmp(ks, "BENDER_TRACE_STRICT"))
        bender_vm.trace_strict = (NULL != vs);
    bender_unlock_environ();
    return bender_wrap_nil();
}

BENDER_CORE_FN(os_time,
              "(os/time)",
              "Get the current time expressed as the number of whole seconds since "
              "January 1, 1970, the Unix epoch. Returns a real number.") {
    bender_fixarity(argc, 0);
    (void) argv;
    double dtime = (double)(time(NULL));
    return bender_wrap_number(dtime);
}

BENDER_CORE_FN(os_clock,
              "(os/clock &opt clock as)",
              "Return the current time of the requested clock source.\n"
              "\n"
              "The `clock` argument selects the clock source to use; "
              "when not specified the default is `:realtime`:\n"
              "\n"
              "* `:realtime` - Return the real (i.e., wall-clock) time. "
              "This clock is affected by discontinuous jumps in the system "
              "time.\n"
              "* `:monotonic` - Return the number of whole + fractional "
              "seconds since some fixed point in time. The clock is "
              "guaranteed to be non-decreasing in real time.\n"
              "* `:cputime` - Return the CPU time consumed by this process "
              "(i.e. all threads in the process).\n"
              "\n"
              "The `as` argument selects the type of output; when not "
              "specified the default is `:double`:\n"
              "\n"
              "* `:double` - Return the number of seconds + fractional "
              "seconds as a double.\n"
              "* `:int` - Return the number of seconds as an integer.\n"
              "* `:tuple` - Return a 2-tuple of integers `[seconds "
              "nanoseconds]`.\n") {
    enum BenderTimeSource source;
    bender_sandbox_assert(BENDER_SANDBOX_HRTIME);
    bender_arity(argc, 0, 2);

    BenderKeyword sourcestr = bender_optkeyword(argv, argc, 0, NULL);
    if (sourcestr == NULL || bender_cstrcmp(sourcestr, "realtime") == 0) {
        source = BENDER_TIME_REALTIME;
    } else if (bender_cstrcmp(sourcestr, "monotonic") == 0) {
        source = BENDER_TIME_MONOTONIC;
    } else if (bender_cstrcmp(sourcestr, "cputime") == 0) {
        source = BENDER_TIME_CPUTIME;
    } else {
        bender_panicf("expected :realtime, :monotonic, or :cputime, got %v", argv[0]);
    }

    struct timespec tv;
    if (bender_gettime(&tv, source)) bender_panic("could not get time");

    BenderKeyword formatstr = bender_optkeyword(argv, argc, 1, NULL);
    if (formatstr == NULL || bender_cstrcmp(formatstr, "double") == 0) {
        double dtime = (double)(tv.tv_sec + (tv.tv_nsec / 1E9));
        return bender_wrap_number(dtime);
    } else if (bender_cstrcmp(formatstr, "int") == 0) {
        return bender_wrap_number((double)(tv.tv_sec));
    } else if (bender_cstrcmp(formatstr, "tuple") == 0) {
        Bender tup[2] = {bender_wrap_number((double)tv.tv_sec),
                        bender_wrap_number((double)tv.tv_nsec)
                       };
        return bender_wrap_tuple(bender_tuple_n(tup, 2));
    } else {
        bender_panicf("expected :double, :int, or :tuple, got %v", argv[1]);
    }
}

BENDER_CORE_FN(os_sleep,
              "(os/sleep sec)",
              "Suspend current thread for `sec` seconds. `sec` can be a "
              "number with a fractional part. Returns nil.") {
    bender_fixarity(argc, 1);
    double delay = bender_getnumber(argv, 0);
    if (delay < 0) bender_panic("invalid argument to sleep");
#ifdef BENDER_WINDOWS
    Sleep((DWORD)(delay * 1000));
#else
    int rc;
    struct timespec ts;
    ts.tv_sec = (time_t) delay;
    ts.tv_nsec = (delay <= UINT32_MAX)
                 ? (long)((delay - ((uint32_t)delay)) * 1000000000)
                 : 0;
    RETRY_EINTR(rc, nanosleep(&ts, &ts));
#endif
    return bender_wrap_nil();
}

BENDER_CORE_FN(os_isatty,
              "(os/isatty &opt file)",
              "Returns true if `file` is a terminal. If `file` is not specified, "
              "it will default to standard output.") {
    bender_arity(argc, 0, 1);
    FILE *f = (argc == 1) ? bender_getfile(argv, 0, NULL) : stdout;
#ifdef BENDER_WINDOWS
    int fd = _fileno(f);
    if (fd == -1) bender_panic("not a valid stream");
    return bender_wrap_boolean(_isatty(fd));
#else
    int fd = fileno(f);
    if (fd == -1) bender_panic(bender_strerror(errno));
    return bender_wrap_boolean(isatty(fd));
#endif
}

BENDER_CORE_FN(os_cwd,
              "(os/cwd)",
              "Returns the current working directory.") {
    bender_fixarity(argc, 0);
    (void) argv;
    char buf[FILENAME_MAX];
    char *ptr;
#ifdef BENDER_WINDOWS
    ptr = _getcwd(buf, FILENAME_MAX);
#else
    ptr = getcwd(buf, FILENAME_MAX);
#endif
    if (NULL == ptr) bender_panic("could not get current directory");
    return bender_cstringv(ptr);
}

BENDER_CORE_FN(os_cryptorand,
              "(os/cryptorand n &opt buf)",
              "Get or append `n` bytes of good quality random data provided by the OS. Returns a new buffer or `buf`.") {
    BenderBuffer *buffer;
    bender_arity(argc, 1, 2);
    int32_t offset;
    int32_t n = bender_getinteger(argv, 0);
    if (n < 0) bender_panic("expected positive integer");
    if (argc == 2) {
        buffer = bender_getbuffer(argv, 1);
        offset = buffer->count;
    } else {
        offset = 0;
        buffer = bender_buffer(n);
    }
    /* We could optimize here by adding setcount_uninit */
    bender_buffer_setcount(buffer, offset + n);

    if (bender_cryptorand(buffer->data + offset, n) != 0)
        bender_panic("unable to get sufficient random data");

    return bender_wrap_buffer(buffer);
}

/* Helper function to get given or current time as local or UTC struct tm.
 * - arg n+0: optional time_t to be converted, uses current time if not given
 * - arg n+1: optional truthy to indicate the convnersion uses local time */
static struct tm *time_to_tm(const Bender *argv, int32_t argc, int32_t n, struct tm *t_infos) {
    time_t t;
    if (argc > n && !bender_checktype(argv[n], BENDER_NIL)) {
        int64_t integer = bender_getinteger64(argv, n);
        t = (time_t) integer;
    } else {
        time(&t);
    }
    struct tm *t_info = NULL;
    if (argc > n + 1 && bender_truthy(argv[n + 1])) {
        /* local time */
#ifdef BENDER_WINDOWS
        _tzset();
        localtime_s(t_infos, &t);
        t_info = t_infos;
#elif defined(BENDER_PLAN9)
        t_info = localtime(&t);
#else
        tzset();
        t_info = localtime_r(&t, t_infos);
#endif
    } else {
        /* utc time */
#ifdef BENDER_WINDOWS
        gmtime_s(t_infos, &t);
        t_info = t_infos;
#elif defined(BENDER_PLAN9)
        t_info = gmtime(&t);
#else
        t_info = gmtime_r(&t, t_infos);
#endif
    }
    return t_info;
}

BENDER_CORE_FN(os_date,
              "(os/date &opt time local)",
              "Returns the given time as a date struct, or the current time "
              "if `time` is not given. Date is given in UTC unless `local` "
              "is truthy, in which case the date is formatted for the local "
              "timezone. Returns a struct with following key-value pairs. "
              "Note that all numbers are 0-indexed.\n"
              "\n"
              "* `:seconds` - number of seconds [0-61]\n"
              "* `:minutes` - number of minutes [0-59]\n"
              "* `:hours` - number of hours [0-23]\n"
              "* `:month-day` - day of month [0-30]\n"
              "* `:month` - month of year [0, 11]\n"
              "* `:year` - years since year 0 (e.g. 2019)\n"
              "* `:week-day` - day of the week [0-6]\n"
              "* `:year-day` - day of the year [0-365]\n"
              "* `:dst` - if Day Light Savings is in effect\n"
              "\n"
              "The local timezone can be set by setting the `TZ` "
              "environment variable. See `tzset()` (`<time.h>`) or "
              "`_tzset()` (`<time.h>`) for further details.") {
    bender_arity(argc, 0, 2);
    (void) argv;
    struct tm t_infos;
    struct tm *t_info = time_to_tm(argv, argc, 0, &t_infos);
    BenderKV *st = bender_struct_begin(9);
    bender_struct_put(st, bender_ckeywordv("seconds"), bender_wrap_number(t_info->tm_sec));
    bender_struct_put(st, bender_ckeywordv("minutes"), bender_wrap_number(t_info->tm_min));
    bender_struct_put(st, bender_ckeywordv("hours"), bender_wrap_number(t_info->tm_hour));
    bender_struct_put(st, bender_ckeywordv("month-day"), bender_wrap_number(t_info->tm_mday - 1));
    bender_struct_put(st, bender_ckeywordv("month"), bender_wrap_number(t_info->tm_mon));
    bender_struct_put(st, bender_ckeywordv("year"), bender_wrap_number(t_info->tm_year + 1900));
    bender_struct_put(st, bender_ckeywordv("week-day"), bender_wrap_number(t_info->tm_wday));
    bender_struct_put(st, bender_ckeywordv("year-day"), bender_wrap_number(t_info->tm_yday));
    bender_struct_put(st, bender_ckeywordv("dst"), bender_wrap_boolean(t_info->tm_isdst));
    return bender_wrap_struct(bender_struct_end(st));
}

#define SIZETIMEFMT 250

BENDER_CORE_FN(os_strftime,
              "(os/strftime fmt &opt time local)",
              "Format the given time as a string, or the current time if "
              "`time` is not given. The time is formatted according to the "
              "same rules as the ISO C89 function `strftime()`. The time is "
              "formatted in UTC unless `local` is truthy, in which case the "
              "date is formatted for the local timezone. The local timezone "
              "can be set by setting the `TZ` environment variable. See "
              "`tzset()` (`<time.h>`) or `_tzset()`(`<time.h>`) for further "
              "details.") {
    bender_arity(argc, 1, 3);
    const char *fmt = bender_getcstring(argv, 0);
    /* ANSI X3.159-1989, section 4.12.3.5 "The strftime function" */
    static const char *valid = "aAbBcdHIjmMpSUwWxXyYZ%";
    const char *p = fmt;
    while (*p) {
        if (*p++ == '%') {
            if (!*p) {
                bender_panic("invalid conversion specifier");
            }
            if (!strchr(valid, *p)) {
                bender_panicf("invalid conversion specifier '%%%c'", *p);
            }
            p++;
        }
    }
    struct tm t_infos;
    struct tm *t_info = time_to_tm(argv, argc, 1, &t_infos);
    char buf[SIZETIMEFMT];
    (void)strftime(buf, sizeof(buf), fmt, t_info);
    return bender_cstringv(buf);
}

static int entry_getdst(Bender env_entry) {
    Bender v;
    if (bender_checktype(env_entry, BENDER_TABLE)) {
        BenderTable *entry = bender_unwrap_table(env_entry);
        v = bender_table_get_keyword(entry, "dst");
    } else if (bender_checktype(env_entry, BENDER_STRUCT)) {
        const BenderKV *entry = bender_unwrap_struct(env_entry);
        v = bender_struct_get(entry, bender_ckeywordv("dst"));
    } else {
        v = bender_wrap_nil();
    }
    if (bender_checktype(v, BENDER_NIL)) {
        return -1;
    } else {
        return bender_truthy(v);
    }
}

#ifdef BENDER_WINDOWS
typedef int32_t timeint_t;
#else
typedef int64_t timeint_t;
#endif

static timeint_t entry_getint(Bender env_entry, char *field) {
    Bender i;
    if (bender_checktype(env_entry, BENDER_TABLE)) {
        BenderTable *entry = bender_unwrap_table(env_entry);
        i = bender_table_get_keyword(entry, field);
    } else if (bender_checktype(env_entry, BENDER_STRUCT)) {
        const BenderKV *entry = bender_unwrap_struct(env_entry);
        i = bender_struct_get(entry, bender_ckeywordv(field));
    } else {
        return 0;
    }

    if (bender_checktype(i, BENDER_NIL)) {
        return 0;
    }

#ifdef BENDER_WINDOWS
    if (!bender_checkint(i)) {
        bender_panicf("bad slot #%s, expected 32 bit signed integer, got %v",
                     field, i);
    }
#else
    if (!bender_checkint64(i)) {
        bender_panicf("bad slot #%s, expected 64 bit signed integer, got %v",
                     field, i);
    }
#endif

    return (timeint_t)bender_unwrap_number(i);
}

BENDER_CORE_FN(os_mktime,
              "(os/mktime date-struct &opt local)",
              "Get the broken down date-struct time expressed as the number "
              "of seconds since January 1, 1970, the Unix epoch. "
              "Returns a real number. "
              "Date is given in UTC unless `local` is truthy, in which case the "
              "date is computed for the local timezone.\n\n"
              "Inverse function to os/date.") {
    bender_arity(argc, 1, 2);
    time_t t;
    struct tm t_info;

    /* Use memset instead of = {0} to silence paranoid warning in macos */
    memset(&t_info, 0, sizeof(t_info));

    if (!bender_checktype(argv[0], BENDER_TABLE) &&
            !bender_checktype(argv[0], BENDER_STRUCT))
        bender_panic_type(argv[0], 0, BENDER_TFLAG_DICTIONARY);

    t_info.tm_sec = entry_getint(argv[0], "seconds");
    t_info.tm_min = entry_getint(argv[0], "minutes");
    t_info.tm_hour = entry_getint(argv[0], "hours");
    t_info.tm_mday = entry_getint(argv[0], "month-day") + 1;
    t_info.tm_mon = entry_getint(argv[0], "month");
    t_info.tm_year = entry_getint(argv[0], "year") - 1900;
    t_info.tm_isdst = entry_getdst(argv[0]);

    if (argc >= 2 && bender_truthy(argv[1])) {
        /* local time */
        t = mktime(&t_info);
    } else {
        /* utc time */
#ifdef BENDER_NO_UTC_MKTIME
        bender_panic("os/mktime UTC not supported on this platform");
#else
        t = timegm(&t_info);
#endif
    }

    if (t == (time_t) -1) {
        bender_panicf("%s", bender_strerror(errno));
    }

    return bender_wrap_number((double)t);
}

#ifdef BENDER_NO_SYMLINKS
#define j_symlink link
#else
#define j_symlink symlink
#endif

#ifndef BENDER_NO_LOCALES
BENDER_CORE_FN(os_setlocale,
              "(os/setlocale &opt locale category)",
              "Set the system locale, which affects how dates and numbers are formatted. "
              "Passing nil to locale will return the current locale. Category can be one of:\n\n"
              " * :all (default)\n"
              " * :collate\n"
              " * :ctype\n"
              " * :monetary\n"
              " * :numeric\n"
              " * :time\n\n"
              "Returns the new locale if set successfully, otherwise nil. Note that this will affect "
              "other functions such as `os/strftime` and even `printf`.") {
    bender_sandbox_assert(BENDER_SANDBOX_LOCALE);
    bender_arity(argc, 0, 2);
    const char *locale_name = bender_optcstring(argv, argc, 0, NULL);
    int category_int = LC_ALL;
    if (argc > 1 && !bender_checktype(argv[1], BENDER_NIL)) {
        if (bender_keyeq(argv[1], "all")) {
            category_int = LC_ALL;
        } else if (bender_keyeq(argv[1], "collate")) {
            category_int = LC_COLLATE;
        } else if (bender_keyeq(argv[1], "ctype")) {
            category_int = LC_CTYPE;
        } else if (bender_keyeq(argv[1], "monetary")) {
            category_int = LC_MONETARY;
        } else if (bender_keyeq(argv[1], "numeric")) {
            category_int = LC_NUMERIC;
        } else if (bender_keyeq(argv[1], "time")) {
            category_int = LC_TIME;
        } else {
            bender_panicf("expected one of :all, :collate, :ctype, :monetary, :numeric, or :time, got %v", argv[1]);
        }
    }
    const char *old = setlocale(category_int, locale_name);
    if (old == NULL) return bender_wrap_nil();
    return bender_cstringv(old);
}
#endif

BENDER_CORE_FN(os_link,
              "(os/link oldpath newpath &opt symlink)",
              "Create a link at newpath that points to oldpath and returns nil. "
              "Iff symlink is truthy, creates a symlink. "
              "Iff symlink is falsey or not provided, "
              "creates a hard link. Does not work on Windows or Plan 9.") {
    bender_sandbox_assert(BENDER_SANDBOX_FS_WRITE);
    bender_arity(argc, 2, 3);
#if defined(BENDER_WINDOWS) || defined(BENDER_PLAN9)
    (void) argc;
    (void) argv;
    bender_panic("not supported on Windows or Plan 9");
#else
    const char *oldpath = bender_getcstring(argv, 0);
    const char *newpath = bender_getcstring(argv, 1);
    int res = ((argc == 3 && bender_truthy(argv[2])) ? j_symlink : link)(oldpath, newpath);
    if (-1 == res) bender_panicf("%s: %s -> %s", bender_strerror(errno), oldpath, newpath);
    return bender_wrap_nil();
#endif
}

BENDER_CORE_FN(os_symlink,
              "(os/symlink oldpath newpath)",
              "Create a symlink from oldpath to newpath, returning nil. Same as `(os/link oldpath newpath true)`.") {
    bender_sandbox_assert(BENDER_SANDBOX_FS_WRITE);
    bender_fixarity(argc, 2);
#if defined(BENDER_WINDOWS) || defined(BENDER_PLAN9)
    (void) argc;
    (void) argv;
    bender_panic("not supported on Windows or Plan 9");
#else
    const char *oldpath = bender_getcstring(argv, 0);
    const char *newpath = bender_getcstring(argv, 1);
    int res = j_symlink(oldpath, newpath);
    if (-1 == res) bender_panicf("%s: %s -> %s", bender_strerror(errno), oldpath, newpath);
    return bender_wrap_nil();
#endif
}

#undef j_symlink

BENDER_CORE_FN(os_mkdir,
              "(os/mkdir path)",
              "Create a new directory. The path will be relative to the current directory if relative, otherwise "
              "it will be an absolute path. Returns true if the directory was created, false if the directory already exists, and "
              "errors otherwise.") {
    bender_sandbox_assert(BENDER_SANDBOX_FS_WRITE);
    bender_fixarity(argc, 1);
    const char *path = bender_getcstring(argv, 0);
#ifdef BENDER_WINDOWS
    int res = _mkdir(path);
#else
    int res = mkdir(path, S_IRUSR | S_IWUSR | S_IXUSR | S_IRGRP | S_IWGRP | S_IXGRP | S_IROTH | S_IXOTH);
#endif
    if (res == 0) return bender_wrap_true();
    if (errno == EEXIST) return bender_wrap_false();
#ifdef __FreeBSD__
    /* See kern/59739 at this point I believe its expected behaviour */
    if (errno == EISDIR && path && path[0] == '/' && path[1] == 0) return bender_wrap_false();
#endif
    bender_panicf("%s: %s", bender_strerror(errno), path);
}

BENDER_CORE_FN(os_rmdir,
              "(os/rmdir path)",
              "Delete a directory. The directory must be empty to succeed.") {
    bender_sandbox_assert(BENDER_SANDBOX_FS_WRITE);
    bender_fixarity(argc, 1);
    const char *path = bender_getcstring(argv, 0);
#ifdef BENDER_WINDOWS
    int res = _rmdir(path);
#elif defined(BENDER_PLAN9)
    int res = remove(path);
#else
    int res = rmdir(path);
#endif
    if (-1 == res) bender_panicf("%s: %s", bender_strerror(errno), path);
    return bender_wrap_nil();
}

BENDER_CORE_FN(os_cd,
              "(os/cd path)",
              "Change current directory to path. Returns nil on success, errors on failure.") {
    bender_sandbox_assert(BENDER_SANDBOX_FS_READ | BENDER_SANDBOX_FS_WRITE);
    bender_fixarity(argc, 1);
    const char *path = bender_getcstring(argv, 0);
#ifdef BENDER_WINDOWS
    int res = _chdir(path);
#else
    int res = chdir(path);
#endif
    if (-1 == res) bender_panicf("%s: %s", bender_strerror(errno), path);
    return bender_wrap_nil();
}

BENDER_CORE_FN(os_touch,
              "(os/touch path &opt actime modtime)",
              "Update the access time and modification times for a file. By default, sets "
              "times to the current time.") {
    bender_sandbox_assert(BENDER_SANDBOX_FS_WRITE);
    bender_arity(argc, 1, 3);
    const char *path = bender_getcstring(argv, 0);
    struct utimbuf timebuf, *bufp;
    if (argc >= 2) {
        bufp = &timebuf;
        timebuf.actime = (time_t) bender_getnumber(argv, 1);
        if (argc >= 3) {
            timebuf.modtime = (time_t) bender_getnumber(argv, 2);
        } else {
            timebuf.modtime = timebuf.actime;
        }
    } else {
        bufp = NULL;
    }
    int res = utime(path, bufp);
    if (-1 == res) bender_panic(bender_strerror(errno));
    return bender_wrap_nil();
}

BENDER_CORE_FN(os_remove,
              "(os/rm path)",
              "Delete a file. Returns nil.") {
    bender_sandbox_assert(BENDER_SANDBOX_FS_WRITE);
    bender_fixarity(argc, 1);
    const char *path = bender_getcstring(argv, 0);
    int status = remove(path);
    if (-1 == status) bender_panicf("%s: %s", bender_strerror(errno), path);
    return bender_wrap_nil();
}

#ifndef BENDER_NO_SYMLINKS
BENDER_CORE_FN(os_readlink,
              "(os/readlink path)",
              "Read the contents of a symbolic link. Does not work on Windows.\n") {
    bender_sandbox_assert(BENDER_SANDBOX_FS_READ);
    bender_fixarity(argc, 1);
#ifdef BENDER_WINDOWS
    (void) argc;
    (void) argv;
    bender_panic("not supported on Windows");
#else
    char buffer[PATH_MAX];
    const char *path = bender_getcstring(argv, 0);
    ssize_t len = readlink(path, buffer, sizeof buffer);
    if (len < 0 || (size_t)len >= sizeof buffer)
        bender_panicf("%s: %s", bender_strerror(errno), path);
    return bender_stringv((const uint8_t *)buffer, len);
#endif
}
#endif

#ifdef BENDER_WINDOWS

typedef struct _stat jstat_t;
typedef unsigned short jmode_t;

static int32_t bender_perm_to_unix(unsigned short m) {
    int32_t ret = 0;
    if (m & S_IEXEC) ret |= 0111;
    if (m & S_IWRITE) ret |= 0222;
    if (m & S_IREAD) ret |= 0444;
    return ret;
}

static unsigned short bender_perm_from_unix(int32_t x) {
    unsigned short m = 0;
    if (x & 111) m |= S_IEXEC;
    if (x & 222) m |= S_IWRITE;
    if (x & 444) m |= S_IREAD;
    return m;
}

static const uint8_t *bender_decode_mode(unsigned short m) {
    const char *str = "other";
    if (m & _S_IFREG) str = "file";
    else if (m & _S_IFDIR) str = "directory";
    else if (m & _S_IFCHR) str = "character";
    return bender_ckeyword(str);
}

static int32_t bender_decode_permissions(jmode_t mode) {
    return (int32_t)(mode & (S_IEXEC | S_IWRITE | S_IREAD));
}

#else

typedef struct stat jstat_t;
typedef mode_t jmode_t;

static int32_t bender_perm_to_unix(mode_t m) {
    return (int32_t) m;
}

static mode_t bender_perm_from_unix(int32_t x) {
    return (mode_t) x;
}

static const uint8_t *bender_decode_mode(mode_t m) {
    const char *str = "other";
    if (S_ISREG(m)) str = "file";
    else if (S_ISDIR(m)) str = "directory";
#ifndef BENDER_PLAN9
    else if (S_ISFIFO(m)) str = "fifo";
    else if (S_ISBLK(m)) str = "block";
    else if (S_ISSOCK(m)) str = "socket";
    else if (S_ISLNK(m)) str = "link";
    else if (S_ISCHR(m)) str = "character";
#endif
    return bender_ckeyword(str);
}

static int32_t bender_decode_permissions(jmode_t mode) {
    return (int32_t)(mode & 0777);
}

#endif

static int32_t os_parse_permstring(const uint8_t *perm) {
    int32_t m = 0;
    if (perm[0] == 'r') m |= 0400;
    if (perm[1] == 'w') m |= 0200;
    if (perm[2] == 'x') m |= 0100;
    if (perm[3] == 'r') m |= 0040;
    if (perm[4] == 'w') m |= 0020;
    if (perm[5] == 'x') m |= 0010;
    if (perm[6] == 'r') m |= 0004;
    if (perm[7] == 'w') m |= 0002;
    if (perm[8] == 'x') m |= 0001;
    return m;
}

static Bender os_make_permstring(int32_t permissions) {
    uint8_t bytes[9] = {0};
    bytes[0] = (permissions & 0400) ? 'r' : '-';
    bytes[1] = (permissions & 0200) ? 'w' : '-';
    bytes[2] = (permissions & 0100) ? 'x' : '-';
    bytes[3] = (permissions & 0040) ? 'r' : '-';
    bytes[4] = (permissions & 0020) ? 'w' : '-';
    bytes[5] = (permissions & 0010) ? 'x' : '-';
    bytes[6] = (permissions & 0004) ? 'r' : '-';
    bytes[7] = (permissions & 0002) ? 'w' : '-';
    bytes[8] = (permissions & 0001) ? 'x' : '-';
    return bender_stringv(bytes, sizeof(bytes));
}

static int32_t os_get_unix_mode(const Bender *argv, int32_t n) {
    int32_t unix_mode;
    if (bender_checkint(argv[n])) {
        /* Integer mode */
        int32_t x = bender_unwrap_integer(argv[n]);
        if (x < 0 || x > 0777) {
            bender_panicf("bad slot #%d, expected integer in range [0, 8r777], got %v", n, argv[n]);
        }
        unix_mode = x;
    } else {
        /* Bytes mode */
        BenderByteView bytes = bender_getbytes(argv, n);
        if (bytes.len != 9) {
            bender_panicf("bad slot #%d: expected byte sequence of length 9, got %v", n, argv[n]);
        }
        unix_mode = os_parse_permstring(bytes.bytes);
    }
    return unix_mode;
}

static jmode_t os_getmode(const Bender *argv, int32_t n) {
    return bender_perm_from_unix(os_get_unix_mode(argv, n));
}

/* Getters */
static Bender os_stat_dev(jstat_t *st) {
    return bender_wrap_number(st->st_dev);
}
static Bender os_stat_inode(jstat_t *st) {
    return bender_wrap_number(st->st_ino);
}
static Bender os_stat_mode(jstat_t *st) {
    return bender_wrap_keyword(bender_decode_mode(st->st_mode));
}
static Bender os_stat_int_permissions(jstat_t *st) {
    return bender_wrap_integer(bender_perm_to_unix(bender_decode_permissions(st->st_mode)));
}
static Bender os_stat_permissions(jstat_t *st) {
    return os_make_permstring(bender_perm_to_unix(bender_decode_permissions(st->st_mode)));
}
static Bender os_stat_uid(jstat_t *st) {
    return bender_wrap_number(st->st_uid);
}
static Bender os_stat_gid(jstat_t *st) {
    return bender_wrap_number(st->st_gid);
}
static Bender os_stat_nlink(jstat_t *st) {
    return bender_wrap_number(st->st_nlink);
}
static Bender os_stat_rdev(jstat_t *st) {
    return bender_wrap_number(st->st_rdev);
}
static Bender os_stat_size(jstat_t *st) {
    return bender_wrap_number(st->st_size);
}
static Bender os_stat_accessed(jstat_t *st) {
    return bender_wrap_number((double) st->st_atime);
}
static Bender os_stat_modified(jstat_t *st) {
    return bender_wrap_number((double) st->st_mtime);
}
static Bender os_stat_changed(jstat_t *st) {
    return bender_wrap_number((double) st->st_ctime);
}
#ifdef BENDER_WINDOWS
static Bender os_stat_blocks(jstat_t *st) {
    (void) st;
    return bender_wrap_number(0);
}
static Bender os_stat_blocksize(jstat_t *st) {
    (void) st;
    return bender_wrap_number(0);
}
#else
static Bender os_stat_blocks(jstat_t *st) {
    return bender_wrap_number(st->st_blocks);
}
static Bender os_stat_blocksize(jstat_t *st) {
    return bender_wrap_number(st->st_blksize);
}
#endif

struct OsStatGetter {
    const char *name;
    Bender(*fn)(jstat_t *st);
};

static const struct OsStatGetter os_stat_getters[] = {
    {"dev", os_stat_dev},
    {"inode", os_stat_inode},
    {"mode", os_stat_mode},
    {"int-permissions", os_stat_int_permissions},
    {"permissions", os_stat_permissions},
    {"uid", os_stat_uid},
    {"gid", os_stat_gid},
    {"nlink", os_stat_nlink},
    {"rdev", os_stat_rdev},
    {"size", os_stat_size},
    {"blocks", os_stat_blocks},
    {"blocksize", os_stat_blocksize},
    {"accessed", os_stat_accessed},
    {"modified", os_stat_modified},
    {"changed", os_stat_changed},
    {NULL, NULL}
};

static Bender os_stat_or_lstat(int do_lstat, int32_t argc, Bender *argv) {
    bender_sandbox_assert(BENDER_SANDBOX_FS_READ);
    bender_arity(argc, 1, 2);
    const char *path = bender_getcstring(argv, 0);
    BenderTable *tab = NULL;
    const uint8_t *key = NULL;
    if (argc == 2) {
        if (bender_checktype(argv[1], BENDER_KEYWORD)) {
            key = bender_getkeyword(argv, 1);
        } else {
            tab = bender_gettable(argv, 1);
        }
    } else {
        tab = bender_table(0);
    }

    /* Build result */
    jstat_t st;
#ifdef BENDER_WINDOWS
    (void) do_lstat;
    int res = _stat(path, &st);
#elif defined(BENDER_PLAN9)
    (void)do_lstat;
    int res = stat(path, &st);
#else
    int res;
    if (do_lstat) {
        res = lstat(path, &st);
    } else {
        res = stat(path, &st);
    }
#endif
    if (-1 == res) {
        return bender_wrap_nil();
    }

    if (NULL == key) {
        /* Put results in table */
        for (const struct OsStatGetter *sg = os_stat_getters; sg->name != NULL; sg++) {
            bender_table_put(tab, bender_ckeywordv(sg->name), sg->fn(&st));
        }
        return bender_wrap_table(tab);
    } else {
        /* Get one result */
        for (const struct OsStatGetter *sg = os_stat_getters; sg->name != NULL; sg++) {
            if (bender_cstrcmp(key, sg->name)) continue;
            return sg->fn(&st);
        }
        bender_panicf("unexpected keyword %v", bender_wrap_keyword(key));
    }
}

BENDER_CORE_FN(os_stat,
              "(os/stat path &opt tab-or-kwd)",
              "Gets information about a file or directory. Returns a table "
              "unless the second argument `tab-or-kwd` is a keyword, in "
              "which case it returns only that field/value from `stat()`. "
              "If `tab-or-kwd` is a table, reuses it as basis of the "
              "returned table. If the file or directory does not exist, "
              "returns nil. The keys are:\n"
              "\n"
              "* `:dev` - device that the file is on\n"
              "* `:mode` - type of file, one of `:file`, `:directory`, `:block`, `:character`, `:fifo`, `:socket`, `:link`, or `:other`\n"
              "* `:int-permissions` - a Unix permission integer like 8r744\n"
              "* `:permissions` - a Unix permission string like \"rwxr--r--\"\n"
              "* `:uid` - file uid\n"
              "* `:gid` - file gid\n"
              "* `:nlink` - number of links to file\n"
              "* `:rdev` - real device of file. 0 on Windows\n"
              "* `:size` - size of file in bytes\n"
              "* `:blocks` - number of blocks in file. 0 on Windows\n"
              "* `:blocksize` - size of blocks in file. 0 on Windows\n"
              "* `:accessed` - timestamp when file last accessed\n"
              "* `:changed` - timestamp when file last changed (permissions changed)\n"
              "* `:modified` - timestamp when file last modified (content changed)") {
    return os_stat_or_lstat(0, argc, argv);
}

BENDER_CORE_FN(os_lstat,
              "(os/lstat path &opt tab-or-kwd)",
              "Like `os/stat`, but doesn't follow symlinks.\n") {
    return os_stat_or_lstat(1, argc, argv);
}

BENDER_CORE_FN(os_chmod,
              "(os/chmod path mode)",
              "Change file permissions, where `mode` is a permission string as returned by "
              "`os/perm-string`, or an integer as returned by `os/perm-int`. "
              "When `mode` is an integer, it is interpreted as a Unix permission value, best specified in octal, like "
              "8r666 or 8r400. Windows will not differentiate between user, group, and other permissions, and thus will combine all of these permissions. Returns nil."
              "Unsupported on plan9.") {
    bender_sandbox_assert(BENDER_SANDBOX_FS_WRITE);
    bender_fixarity(argc, 2);
#ifdef BENDER_PLAN9
    bender_panic("not supported on Plan 9");
#else
    const char *path = bender_getcstring(argv, 0);
#ifdef BENDER_WINDOWS
    int res = _chmod(path, os_getmode(argv, 1));
#else
    int res = chmod(path, os_getmode(argv, 1));
#endif
    if (-1 == res) bender_panicf("%s: %s", bender_strerror(errno), path);
    return bender_wrap_nil();
#endif
}

#ifndef BENDER_NO_UMASK
BENDER_CORE_FN(os_umask,
              "(os/umask mask)",
              "Set a new umask, returns the old umask.") {
    bender_sandbox_assert(BENDER_SANDBOX_FS_WRITE);
    bender_fixarity(argc, 1);
    int mask = (int) os_getmode(argv, 0);
#ifdef BENDER_WINDOWS
    int res = _umask(mask);
#else
    int res = umask(mask);
#endif
    return bender_wrap_integer(bender_perm_to_unix(res));
}
#endif

BENDER_CORE_FN(os_dir,
              "(os/dir dir &opt arr)",
              "Enumerate files and subdirectories in a directory `dir`. "
              "Returns an array of paths relative to `dir` with only the "
              "file or directory name with no prefix. If an array `arr` "
              "is specified, append paths to it instead of creating a new "
              "array.") {
    bender_sandbox_assert(BENDER_SANDBOX_FS_READ);
    bender_arity(argc, 1, 2);
    const char *dir = bender_getcstring(argv, 0);
    BenderArray *paths = (argc == 2) ? bender_getarray(argv, 1) : bender_array(0);
#ifdef BENDER_WINDOWS
    /* Read directory items with FindFirstFile / FindNextFile / FindClose */
    struct _finddata_t afile;
    char pattern[MAX_PATH + 1];
    if (strlen(dir) > (sizeof(pattern) - 3))
        bender_panicf("path too long: %s", dir);
    snprintf(pattern, sizeof(pattern), "%s/*", dir);
    intptr_t res = _findfirst(pattern, &afile);
    if (-1 == res) bender_panicv(bender_cstringv(bender_strerror(errno)));
    do {
        if (strcmp(".", afile.name) && strcmp("..", afile.name)) {
            bender_array_push(paths, bender_cstringv(afile.name));
        }
    } while (_findnext(res, &afile) != -1);
    _findclose(res);
#else
    /* Read directory items with opendir / readdir / closedir */
    struct dirent *dp;
    DIR *dfd = opendir(dir);
    if (dfd == NULL) bender_panicf("cannot open directory %s: %s", dir, bender_strerror(errno));
    for (;;) {
        errno = 0;
        dp = readdir(dfd);
        if (dp == NULL) {
            if (errno) {
                int olderr = errno;
                closedir(dfd);
                bender_panicf("failed to read directory %s: %s", dir, bender_strerror(olderr));
            }
            break;
        }
        if (!strcmp(dp->d_name, ".") || !strcmp(dp->d_name, "..")) {
            continue;
        }
        bender_array_push(paths, bender_cstringv(dp->d_name));
    }
    closedir(dfd);
#endif
    return bender_wrap_array(paths);
}

BENDER_CORE_FN(os_rename,
              "(os/rename oldname newname)",
              "Rename a file on disk to a new path. Returns nil.") {
    bender_sandbox_assert(BENDER_SANDBOX_FS_WRITE);
    bender_fixarity(argc, 2);
    const char *src = bender_getcstring(argv, 0);
    const char *dest = bender_getcstring(argv, 1);
    int status = rename(src, dest);
    if (status) {
        bender_panic(bender_strerror(errno));
    }
    return bender_wrap_nil();
}

BENDER_CORE_FN(os_realpath,
              "(os/realpath path)",
              "Get the absolute path for a given path, following ../, ./, and symlinks. "
              "Returns an absolute path as a string.") {
    bender_sandbox_assert(BENDER_SANDBOX_FS_READ);
    bender_fixarity(argc, 1);
    const char *src = bender_getcstring(argv, 0);
#ifdef BENDER_NO_REALPATH
    bender_panic("os/realpath not enabled for this platform");
#else
#ifdef BENDER_WINDOWS
    char *dest = _fullpath(NULL, src, _MAX_PATH);
#else
    char *dest = realpath(src, NULL);
#endif
    if (NULL == dest) bender_panicf("%s: %s", bender_strerror(errno), src);
    Bender ret = bender_cstringv(dest);
#ifdef BENDER_WINDOWS
    DWORD attrib = GetFileAttributes(dest);
    free(dest); /* if bender_malloc is redefined, still use free to correspond with _fullpath */
    if (attrib == INVALID_FILE_ATTRIBUTES) {
        bender_panicf("path does not exist: %v", ret);
    }
#else
    free(dest);
#endif
    return ret;
#endif
}

BENDER_CORE_FN(os_permission_string,
              "(os/perm-string int)",
              "Convert a Unix octal permission value from a permission integer as returned by `os/stat` "
              "to a human readable string, that follows the formatting "
              "of Unix tools like `ls`. Returns the string as a 9-character string of r, w, x and - characters. Does not "
              "include the file/directory/symlink character as rendered by `ls`.") {
    bender_fixarity(argc, 1);
    return os_make_permstring(os_get_unix_mode(argv, 0));
}

BENDER_CORE_FN(os_permission_int,
              "(os/perm-int bytes)",
              "Parse a 9-character permission string and return an integer that can be used by chmod.") {
    bender_fixarity(argc, 1);
    return bender_wrap_integer(os_get_unix_mode(argv, 0));
}

#ifdef BENDER_EV

/*
 * Define a few functions on streams the require BENDER_EV to be defined.
 */

static jmode_t os_optmode(int32_t argc, const Bender *argv, int32_t n, int32_t dflt) {
    if (argc > n) return os_getmode(argv, n);
    return bender_perm_from_unix(dflt);
}

BENDER_CORE_FN(os_open,
              "(os/open path &opt flags mode)",
              "Create a stream from a file, like the POSIX open system "
              "call. Returns a new stream. `mode` should be a file mode "
              "as passed to `os/chmod`, but only if the create flag is "
              "given. The default mode is 8r666.\n"
              "\n"
              "Allowed flags are as follows:\n"
              "\n"
              "  * :r - open this file for reading\n"
              "  * :w - open this file for writing\n"
              "  * :c - create a new file (`O_CREATE`)\n"
              "  * :e - fail if the file exists (`O_EXCL`)\n"
              "  * :t - shorten an existing file to length 0 (`O_TRUNC`)\n"
              "  * :a - append to a file (`O_APPEND` on posix, `FILE_APPEND_DATA` on windows)\n"
              "\n"
              "Posix-only flags:\n"
              "\n"
              "  * :x - `O_SYNC`\n"
              "  * :C - `O_NOCTTY`\n"
              "  * :N - Turn off `O_NONBLOCK` and disable ev reading/writing\n"
              "\n"
              "Windows-only flags:\n"
              "\n"
              "  * :R - share reads (`FILE_SHARE_READ`)\n"
              "  * :W - share writes (`FILE_SHARE_WRITE`)\n"
              "  * :D - share deletes (`FILE_SHARE_DELETE`)\n"
              "  * :H - `FILE_ATTRIBUTE_HIDDEN`\n"
              "  * :O - `FILE_ATTRIBUTE_READONLY`\n"
              "  * :F - `FILE_ATTRIBUTE_OFFLINE`\n"
              "  * :T - `FILE_ATTRIBUTE_TEMPORARY`\n"
              "  * :d - `FILE_FLAG_DELETE_ON_CLOSE`\n"
              "  * :V - Turn off `FILE_FLAG_OVERLAPPED` and disable ev reading/writing\n"
              "  * :I - set `bInheritHandle` on the created file so it can be passed to other processes.\n"
              "  * :b - `FILE_FLAG_NO_BUFFERING`\n") {
    bender_arity(argc, 1, 3);
    const char *path = bender_getcstring(argv, 0);
    const uint8_t *opt_flags = bender_optkeyword(argv, argc, 1, (const uint8_t *) "r");
    jmode_t mode = os_optmode(argc, argv, 2, 0666);
    uint32_t stream_flags = 0;
    int disable_stream_mode = 0;
    BenderHandle fd;
#ifdef BENDER_WINDOWS
    (void) mode;
    int inherited_handle = 0;
    DWORD desiredAccess = 0;
    DWORD shareMode = 0;
    DWORD creationDisp = 0;
    DWORD fileFlags = FILE_FLAG_OVERLAPPED;
    DWORD fileAttributes = 0;
    /* We map unix-like open flags to the creationDisp parameter */
    int creatUnix = 0;
#define OCREAT 1
#define OEXCL 2
#define OTRUNC 4
    for (const uint8_t *c = opt_flags; *c; c++) {
        switch (*c) {
            default:
                break;
            case 'r':
                desiredAccess |= GENERIC_READ;
                stream_flags |= BENDER_STREAM_READABLE;
                bender_sandbox_assert(BENDER_SANDBOX_FS_READ);
                break;
            case 'w':
                desiredAccess |= GENERIC_WRITE;
                stream_flags |= BENDER_STREAM_WRITABLE;
                bender_sandbox_assert(BENDER_SANDBOX_FS_WRITE);
                break;
            case 'a':
                desiredAccess |= FILE_APPEND_DATA;
                stream_flags |= BENDER_STREAM_WRITABLE;
                bender_sandbox_assert(BENDER_SANDBOX_FS_WRITE);
                break;
            case 'c':
                creatUnix |= OCREAT;
                bender_sandbox_assert(BENDER_SANDBOX_FS_WRITE);
                break;
            case 'e':
                creatUnix |= OEXCL;
                break;
            case 't':
                creatUnix |= OTRUNC;
                bender_sandbox_assert(BENDER_SANDBOX_FS_WRITE);
                break;
            /* Windows only flags */
            case 'D':
                shareMode |= FILE_SHARE_DELETE;
                break;
            case 'R':
                shareMode |= FILE_SHARE_READ;
                break;
            case 'W':
                shareMode |= FILE_SHARE_WRITE;
                break;
            case 'H':
                fileAttributes |= FILE_ATTRIBUTE_HIDDEN;
                break;
            case 'O':
                fileAttributes |= FILE_ATTRIBUTE_READONLY;
                break;
            case 'F':
                fileAttributes |= FILE_ATTRIBUTE_OFFLINE;
                break;
            case 'T':
                fileAttributes |= FILE_ATTRIBUTE_TEMPORARY;
                break;
            case 'd':
                fileFlags |= FILE_FLAG_DELETE_ON_CLOSE;
                break;
            case 'b':
                fileFlags |= FILE_FLAG_NO_BUFFERING;
                break;
            case 'I':
                inherited_handle = 1;
                break;
            case 'V':
                fileFlags &= ~FILE_FLAG_OVERLAPPED;
                disable_stream_mode = 1;
                break;
                /* we could potentially add more here -
                 * https://docs.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-createfilea
                 */
        }
    }
    switch (creatUnix) {
        default:
            bender_panic("invalid creation flags");
        case 0:
            creationDisp = OPEN_EXISTING;
            break;
        case OCREAT:
            creationDisp = OPEN_ALWAYS;
            bender_sandbox_assert(BENDER_SANDBOX_FS_WRITE | BENDER_SANDBOX_FS_READ);
            break;
        case OCREAT + OEXCL:
            creationDisp = CREATE_NEW;
            bender_sandbox_assert(BENDER_SANDBOX_FS_WRITE);
            break;
        case OCREAT + OTRUNC:
            creationDisp = CREATE_ALWAYS;
            bender_sandbox_assert(BENDER_SANDBOX_FS_WRITE);
            break;
        case OTRUNC:
            creationDisp = TRUNCATE_EXISTING;
            bender_sandbox_assert(BENDER_SANDBOX_FS_WRITE);
            break;
    }
    if (fileAttributes == 0) {
        fileAttributes = FILE_ATTRIBUTE_NORMAL;
    }
    SECURITY_ATTRIBUTES saAttr;
    memset(&saAttr, 0, sizeof(saAttr));
    saAttr.nLength = sizeof(saAttr);
    if (inherited_handle) {
        saAttr.bInheritHandle = TRUE; /* Needed to do interesting things with file */
    }
    fd = CreateFileA(path, desiredAccess, shareMode, &saAttr, creationDisp, fileFlags | fileAttributes, NULL);
    if (fd == INVALID_HANDLE_VALUE) bender_panicv(bender_ev_lasterr());
#else
    int open_flags = O_NONBLOCK;
#ifdef BENDER_LINUX
    open_flags |= O_CLOEXEC;
#endif
    int read_flag = 0;
    int write_flag = 0;
    for (const uint8_t *c = opt_flags; *c; c++) {
        switch (*c) {
            default:
                break;
            case 'r':
                read_flag = 1;
                stream_flags |= BENDER_STREAM_READABLE;
                bender_sandbox_assert(BENDER_SANDBOX_FS_READ);
                break;
            case 'w':
                write_flag = 1;
                stream_flags |= BENDER_STREAM_WRITABLE;
                bender_sandbox_assert(BENDER_SANDBOX_FS_WRITE);
                break;
            case 'c':
                open_flags |= O_CREAT;
                bender_sandbox_assert(BENDER_SANDBOX_FS_WRITE);
                break;
            case 'e':
                open_flags |= O_EXCL;
                break;
            case 't':
                open_flags |= O_TRUNC;
                bender_sandbox_assert(BENDER_SANDBOX_FS_WRITE);
                break;
            /* posix only */
            case 'x':
                open_flags |= O_SYNC;
                break;
            case 'C':
                open_flags |= O_NOCTTY;
                break;
            case 'a':
                open_flags |= O_APPEND;
                bender_sandbox_assert(BENDER_SANDBOX_FS_WRITE);
                break;
            case 'N':
                open_flags &= ~O_NONBLOCK;
                disable_stream_mode = 1;
                break;
        }
    }
    /* If both read and write, fix up to O_RDWR */
    if (read_flag && !write_flag) {
        open_flags |= O_RDONLY;
        bender_sandbox_assert(BENDER_SANDBOX_FS_READ);
    } else if (write_flag && !read_flag) {
        open_flags |= O_WRONLY;
        bender_sandbox_assert(BENDER_SANDBOX_FS_WRITE);
    } else {
        open_flags |= O_RDWR;
        bender_sandbox_assert(BENDER_SANDBOX_FS_WRITE | BENDER_SANDBOX_FS_READ);
    }

    do {
        fd = open(path, open_flags, mode);
    } while (fd == -1 && errno == EINTR);
    if (fd == -1) bender_panicv(bender_ev_lasterr());
#endif
    return bender_wrap_abstract(bender_stream(fd, disable_stream_mode ? 0 : stream_flags, NULL));
}

BENDER_CORE_FN(os_pipe,
              "(os/pipe &opt flags)",
              "Create a readable stream and a writable stream that are connected. Returns a two-element "
              "tuple where the first element is a readable stream and the second element is the writable "
              "stream. `flags` is a keyword set of flags to disable non-blocking settings on the ends of the pipe. "
              "This may be desired if passing the pipe to a subprocess with `os/spawn`.\n\n"
              "* :W - sets the writable end of the pipe to a blocking stream.\n"
              "* :R - sets the readable end of the pipe to a blocking stream.\n\n"
              "By default, both ends of the pipe are non-blocking for use with the `ev` module.") {
    (void) argv;
    bender_sandbox_assert(BENDER_SANDBOX_FS_WRITE | BENDER_SANDBOX_FS_TEMP);
    bender_arity(argc, 0, 1);
    BenderHandle fds[2];
    int flags = 0;
    if (argc > 0 && !bender_checktype(argv[0], BENDER_NIL)) {
        flags = (int) bender_getflags(argv, 0, "WR");
    }
    if (bender_make_pipe(fds, flags)) bender_panicv(bender_ev_lasterr());
    BenderStream *reader = bender_stream(fds[0], (flags & 2) ? 0 : BENDER_STREAM_READABLE, NULL);
    BenderStream *writer = bender_stream(fds[1], (flags & 1) ? 0 : BENDER_STREAM_WRITABLE, NULL);
    Bender tup[2] = {bender_wrap_abstract(reader), bender_wrap_abstract(writer)};
    return bender_wrap_tuple(bender_tuple_n(tup, 2));
}

#endif

#endif /* BENDER_REDUCED_OS */

/* Module entry point */
void bender_lib_os(BenderTable *env) {
#if !defined(BENDER_REDUCED_OS) && defined(BENDER_WINDOWS) && defined(BENDER_THREADS)
    /* During start up, the top-most abstract machine (thread)
     * in the thread tree sets up the critical section. */
    static volatile long env_lock_initializing = 0;
    static volatile long env_lock_initialized = 0;
    if (!InterlockedExchange(&env_lock_initializing, 1)) {
        InitializeCriticalSection(&env_lock);
        InterlockedOr(&env_lock_initialized, 1);
    } else {
        while (!InterlockedOr(&env_lock_initialized, 0)) {
            Sleep(0);
        }
    }

#endif
#ifndef BENDER_NO_PROCESSES
#endif
    BenderRegExt os_cfuns[] = {
        BENDER_CORE_REG("os/exit", os_exit),
        BENDER_CORE_REG("os/which", os_which),
        BENDER_CORE_REG("os/arch", os_arch),
        BENDER_CORE_REG("os/compiler", os_compiler),
#ifndef BENDER_REDUCED_OS

        /* misc (un-sandboxed) */
        BENDER_CORE_REG("os/cpu-count", os_cpu_count),
        BENDER_CORE_REG("os/cwd", os_cwd),
        BENDER_CORE_REG("os/cryptorand", os_cryptorand),
        BENDER_CORE_REG("os/perm-string", os_permission_string),
        BENDER_CORE_REG("os/perm-int", os_permission_int),
        BENDER_CORE_REG("os/mktime", os_mktime),
        BENDER_CORE_REG("os/time", os_time), /* not high resolution */
        BENDER_CORE_REG("os/date", os_date), /* not high resolution */
        BENDER_CORE_REG("os/strftime", os_strftime),
        BENDER_CORE_REG("os/sleep", os_sleep),
        BENDER_CORE_REG("os/isatty", os_isatty),
#ifndef BENDER_NO_LOCALES
        BENDER_CORE_REG("os/setlocale", os_setlocale),
#endif

        /* env functions */
#ifndef BENDER_PLAN9
        BENDER_CORE_REG("os/environ", os_environ),
#endif
        BENDER_CORE_REG("os/getenv", os_getenv),
        BENDER_CORE_REG("os/setenv", os_setenv),

        /* fs read */
        BENDER_CORE_REG("os/dir", os_dir),
        BENDER_CORE_REG("os/stat", os_stat),
        BENDER_CORE_REG("os/lstat", os_lstat),
        BENDER_CORE_REG("os/realpath", os_realpath),
        BENDER_CORE_REG("os/cd", os_cd),
#ifndef BENDER_NO_UMASK
        BENDER_CORE_REG("os/umask", os_umask),
#endif
#ifndef BENDER_NO_SYMLINKS
        BENDER_CORE_REG("os/readlink", os_readlink),
#endif

        /* fs write */
        BENDER_CORE_REG("os/mkdir", os_mkdir),
        BENDER_CORE_REG("os/rmdir", os_rmdir),
        BENDER_CORE_REG("os/rm", os_remove),
        BENDER_CORE_REG("os/link", os_link),
        BENDER_CORE_REG("os/rename", os_rename),
        BENDER_CORE_REG("os/chmod", os_chmod),
        BENDER_CORE_REG("os/touch", os_touch),
#ifndef BENDER_NO_SYMLINKS
        BENDER_CORE_REG("os/symlink", os_symlink),
#endif

        /* processes */
#ifndef BENDER_NO_PROCESSES
        BENDER_CORE_REG("os/execute", os_execute),
        BENDER_CORE_REG("os/spawn", os_spawn),
        BENDER_CORE_REG("os/shell", os_shell),
        BENDER_CORE_REG("os/posix-fork", os_posix_fork),
        BENDER_CORE_REG("os/posix-exec", os_posix_exec),
        BENDER_CORE_REG("os/posix-chroot", os_posix_chroot),
        /* no need to sandbox process management if you can't create processes
         * (allows for limited functionality if use exposes C-functions to create specific processes) */
        BENDER_CORE_REG("os/proc-wait", os_proc_wait),
        BENDER_CORE_REG("os/proc-kill", os_proc_kill),
        BENDER_CORE_REG("os/proc-close", os_proc_close),
        BENDER_CORE_REG("os/getpid", os_proc_getpid),
#ifdef BENDER_EV
        BENDER_CORE_REG("os/sigaction", os_sigaction),
#endif
#endif

        /* high resolution timers */
        BENDER_CORE_REG("os/clock", os_clock),

#ifdef BENDER_EV
        BENDER_CORE_REG("os/open", os_open), /* fs read and write */
        BENDER_CORE_REG("os/pipe", os_pipe),
#endif
#endif
        BENDER_REG_END
    };
    bender_core_cfuns_ext(env, NULL, os_cfuns);
}
