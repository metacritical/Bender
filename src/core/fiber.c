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
#include "fiber.h"
#include "state.h"
#include "gc.h"
#include "util.h"
#endif

#ifndef BENDER_WINDOWS
#include <sys/mman.h>
#include <signal.h>
#include <pthread.h>
#include <unistd.h>
#endif

/* AddressSanitizer detection: GCC defines __SANITIZE_ADDRESS__, clang
 * does not — clang only offers __has_feature(address_sanitizer). The
 * grow-on-fault machinery (SIGSEGV handler + malloc'd sigaltstack)
 * conflicts with ASAN's own signal and allocator machinery, so it is
 * compiled out under either sanitizer flavor. */
#if defined(__SANITIZE_ADDRESS__)
#  define BENDER_ASAN 1
#elif defined(__has_feature)
#  if __has_feature(address_sanitizer)
#    define BENDER_ASAN 1
#  endif
#endif

/* Stack size for a fiber's native C stack. The fibers here back VM frames
 * (as of Phase 1 the VM values live in fiber->data, so this serves the C
 * calls made while running a fiber -- cfuns, bender_call children, ev), so
 * it can be substantially larger than the 256 KB a generated-code fiber uses
 * code with tight -O2 frames. */
#ifndef BENDER_FIBER_STACK_BYTES
#define BENDER_FIBER_STACK_BYTES (2 * 1024 * 1024)
#endif
/* Guard size, fully locked behind the usable region. */
#ifndef BENDER_FIBER_GUARD_BYTES
#define BENDER_FIBER_GUARD_BYTES (64 * 1024)
#endif
/* Phase D (segmented growth): reserve the full window up front but commit
 * only this much initially, extending downward in CHUNK steps on guard
 * faults. Both must stay multiples of any supported page size (4K/16K). */
#ifndef BENDER_FIBER_STACK_INIT_BYTES
#define BENDER_FIBER_STACK_INIT_BYTES (64 * 1024)
#endif
#ifndef BENDER_FIBER_STACK_GROW_BYTES
#define BENDER_FIBER_STACK_GROW_BYTES (64 * 1024)
#endif

#ifdef BENDER_WINDOWS
/* Flat malloc there, so init is a no-op (growth needs reserved address
 * space + fault interception; a VEH follow-up could add it). */
void bender_fiber_growstack_init(void) {}
#endif

/* Phase D: grow-on-fault for fiber native stacks (POSIX only; Windows
 * keeps the flat malloc, ucontext keeps its fixed makecontext region —
 * growth is transparent to both switchers since stacks extend downward
 * and the saved sp never moves).
 *
 * Layout: [mapping base][red zone: guard, PROT_NONE, forever][usable:
 * reserved PROT_NONE, committed top-down from INIT in CHUNK steps].
 * A fault anywhere in the uncommitted usable window commits down to
 * (and including) the faulting chunk and resumes; the faulting
 * instruction retries transparently. Single-step growth would miss
 * large single-frame jumps (deep GC/mark chains can move sp a long
 * way at once), so the commit target derives from the fault address
 * itself, rounded down to a chunk. A fault in the red zone is overflow
 * (slice 1: identified abort; catchable errors are the D3 follow-up).
 * Anything else chains to the previously installed action (ASan, user
 * os/sigaction handlers). macOS delivers below-mapping faults as
 * SIGBUS, so both signals are handled with the same logic; Linux
 * delivers both as SIGSEGV (the SIGBUS registration is harmless).
 *
 * Only async-safe operations run in the handler (plain reads, mprotect,
 * write); no locks, no allocation. The faulting thread's running fiber
 * (bender_vm.fiber) identifies the mapping — no global registry needed.
 * A stale fiber pointer on host code can only over-commit (harmless) or
 * mislabel a genuine crash (still dies). Installing our own SIGSEGV
 * handler via os/sigaction later replaces growth (documented). */

#ifndef BENDER_WINDOWS
#ifndef BENDER_NO_GROWSTACK

static struct sigaction bender_growstack_oldact;
static volatile BenderAtomicInt bender_growstack_installed = 0;
static BENDER_THREAD_LOCAL int bender_growstack_alt_ok = 0;

/* fork() keeps the handler disposition but (on some systems, notably
 * macOS) resets the signal stack to SS_DISABLE in the child, and all
 * threads but the forking one vanish. Reinstall a working alt stack
 * right here so forked children keep grow-on-fault transparently (the
 * parent's alt-stack mapping is inherited but no longer registered;
 * leaking its registration is virtual-only). Runs in normal context,
 * so malloc is fine. */
static void bender_growstack_atfork_child(void) {
#ifndef BENDER_ASAN
    size_t alt_size = 256 * 1024;
    void *alt_stack = malloc(alt_size);
    if (NULL != alt_stack) {
        stack_t ss;
        ss.ss_sp = alt_stack;
        ss.ss_size = alt_size;
        ss.ss_flags = 0;
        if (0 == sigaltstack(&ss, NULL)) {
            bender_growstack_alt_ok = 1;
        } else {
            free(alt_stack);
        }
    }
#else
    (void) 0;
#endif
}

static void bender_growstack_segv(int sig, siginfo_t *info, void *uctx) {
    (void) uctx;
    BenderFiber *fiber = bender_vm.fiber;
    void *fault = (NULL != info) ? info->si_addr : NULL;
    /* P3.6: resolve the faulting windows from the private mapping or,
     * for shared-stack fibers, the lane mapping (attributed to the
     * running fiber — only one runs per thread). Committed write-back
     * goes to the matching owner below. */
    char *base = NULL;
    char *usable = NULL;
    char *committed = NULL;
    int is_shared = 0;
    if (NULL != fiber && NULL != fault) {
        if (fiber->shared_stack && NULL != bender_vm.shared_stack_base &&
                NULL != bender_vm.shared_stack_committed) {
            base = (char *) bender_vm.shared_stack_base;
            usable = (char *) bender_vm.shared_stack;
            committed = (char *) bender_vm.shared_stack_committed;
            is_shared = 1;
        } else if (NULL != fiber->native_stack_base &&
                NULL != fiber->native_stack_committed) {
            base = (char *) fiber->native_stack_base;
            usable = (char *) fiber->native_stack;
            committed = (char *) fiber->native_stack_committed;
        }
    }
    if (NULL != base) {
        char *f = (char *) fault;
        size_t chunk = (size_t) BENDER_FIBER_STACK_GROW_BYTES;
        if (f >= usable && f < committed) {
            /* Growth fault anywhere in the uncommitted window: commit
             * down to the faulting chunk (jump-proof) and resume. */
            uintptr_t off = (uintptr_t) (f - usable);
            char *target = usable + (off / chunk) * chunk;
            size_t len = (size_t) (committed - target);
            if (0 == mprotect(target, len, PROT_READ | PROT_WRITE)) {
                if (is_shared) bender_vm.shared_stack_committed = target;
                else fiber->native_stack_committed = target;
                return;
            }
            /* Commit failed: fall through and die below. */
        } else if (f >= base && f < usable) {
            /* Red-zone entry (Phase D3): commit down to the faulting
             * chunk the same way (bounded by the mapping base) and arm
             * the overflow flag; a call-path poll converts it to a
             * catchable error and clears it. Repeated entries consume
             * the red zone progressively, so straight-line descents
             * without polls still terminate at the base instead of
             * livelocking. Shared fibers get the identical treatment
             * (flag is per-fiber). */
            uintptr_t off = (uintptr_t) (f - base);
            char *target = base + (off / chunk) * chunk;
            size_t len = (size_t) (committed - target);
            fiber->stack_overflowed = 1;
            if (len > 0 && 0 == mprotect(target, len, PROT_READ | PROT_WRITE)) {
                if (is_shared) bender_vm.shared_stack_committed = target;
                else fiber->native_stack_committed = target;
                return;
            }
            /* Nothing left (or commit failed): fall through and die. */
        } else if (NULL != fiber && f < base &&
                committed == base) {
            /* Below a fully consumed mapping: our overflow, out of red
             * space. Name it, then die via the old action. */
            static const char msg[] = "bender: fiber native stack overflow\n";
            (void) write(STDERR_FILENO, msg, sizeof(msg) - 1);
        } else {
            goto chain;
        }
    } else {
        goto chain;
    }
chain:
    if (bender_growstack_oldact.sa_flags & SA_SIGINFO) {
        if (NULL != bender_growstack_oldact.sa_sigaction) {
            bender_growstack_oldact.sa_sigaction(sig, info, uctx);
            return;
        }
    } else if (bender_growstack_oldact.sa_handler == SIG_DFL) {
        signal(sig, SIG_DFL);
        raise(sig);
        return;
    } else if (bender_growstack_oldact.sa_handler == SIG_IGN) {
        return;
    } else if (NULL != bender_growstack_oldact.sa_handler) {
        bender_growstack_oldact.sa_handler(sig);
        return;
    }
    signal(sig, SIG_DFL);
    raise(sig);
}

/* Process-once handler install + per-thread alternate stack. Called
 * from bender_init (every VM thread). The alt stack is one-time-per-
 * thread virtual (untouched pages cost no RSS); threads without a VM
 * simply chain. */
void bender_fiber_growstack_init(void) {
    if (1 == bender_atomic_inc(&bender_growstack_installed)) {
        struct sigaction act;
        memset(&act, 0, sizeof(act));
        act.sa_sigaction = bender_growstack_segv;
        sigemptyset(&act.sa_mask);
        act.sa_flags = SA_SIGINFO | SA_ONSTACK;
        sigaction(SIGSEGV, &act, &bender_growstack_oldact);
        /* macOS delivers below-mapping faults as SIGBUS: same handler.
         * Harmless on Linux (never fires there for our mappings). */
        sigaction(SIGBUS, &act, NULL);
        pthread_atfork(NULL, NULL, bender_growstack_atfork_child);
    }
    if (!bender_growstack_alt_ok) {
        /* Roomy: chained handlers (ASan) also run here. Skipped under
         * AddressSanitizer: its allocator CHECK-fails deallocating
         * around these mappings (suite-ev), and ASan prints its own
         * fault reports anyway. Growth still works without one (the
         * faulting sp stays in committed space); only the identified-
         * overflow message may be lost when sp is wild. */
#ifndef BENDER_ASAN
        size_t alt_size = 256 * 1024;
        void *alt_stack = malloc(alt_size);
        if (NULL != alt_stack) {
            stack_t ss;
            ss.ss_sp = alt_stack;
            ss.ss_size = alt_size;
            ss.ss_flags = 0;
            if (0 == sigaltstack(&ss, NULL)) {
                bender_growstack_alt_ok = 1;
            } else {
                free(alt_stack);
            }
        }
#endif
    }
}

#endif /* BENDER_NO_GROWSTACK */
#endif /* !BENDER_WINDOWS */

/* Reserve the full guard+usable window PROT_NONE; commit the top
 * window (all of it under BENDER_NO_GROWSTACK, INIT bytes otherwise —
 * the rest arrives on guard faults). Reports the mapping size, the
 * page-aligned guard, and the lowest committed address. */
static void *fiber_native_stack_alloc(size_t *out_total, size_t *out_guard, void **out_committed) {
#ifdef BENDER_WINDOWS
    size_t total = BENDER_FIBER_STACK_BYTES;
    void *p = malloc(total);
    if (NULL == p) BENDER_OUT_OF_MEMORY;
    *out_total = total;
    *out_guard = 0;
    *out_committed = p;
    return p;
#else
    long page = sysconf(_SC_PAGESIZE);
    if (page <= 0) page = 4096;
    size_t guard = ((size_t)BENDER_FIBER_GUARD_BYTES + (size_t)page - 1) /
                   (size_t)page * (size_t)page;
    size_t total = guard + BENDER_FIBER_STACK_BYTES;
    void *p = mmap(NULL, total, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (MAP_FAILED == p) BENDER_OUT_OF_MEMORY;
#ifdef BENDER_NO_GROWSTACK
    size_t commit = BENDER_FIBER_STACK_BYTES;
#else
    size_t commit = BENDER_FIBER_STACK_INIT_BYTES;
    if (commit > BENDER_FIBER_STACK_BYTES) commit = BENDER_FIBER_STACK_BYTES;
#endif
    if (mprotect((char *)p + total - commit, commit,
                 PROT_READ | PROT_WRITE)) {
        munmap(p, total);
        BENDER_OUT_OF_MEMORY;
    }
    *out_total = total;
    *out_guard = guard;
    *out_committed = (char *)p + total - commit;
    return p;
#endif
}

void bender_fiber_native_stack_free(BenderFiber *fiber) {
    if (fiber->native_stack_base) {
        /* Never unmap the stack we are currently executing on. This happens
         * at interpreter shutdown: bender_deinit -> bender_clear_memory frees
         * every fiber, but deinit itself runs inside a fiber (e.g. via
         * (os/exit) from a script). Leaking that one mapping is safe --
         * the process is exiting (and for embedders that re-init, it is
         * one stack mapping per deinit). */
        if (fiber == bender_vm.fiber) {
            fiber->native_stack_base = NULL;
            fiber->native_stack = NULL;
            fiber->native_stack_size = 0;
            fiber->native_stack_committed = NULL;
            return;
        }
#ifdef BENDER_WINDOWS
        free(fiber->native_stack_base);
#else
        /* native_stack_size is the total mapping size (guard + usable). */
        munmap(fiber->native_stack_base, fiber->native_stack_size);
#endif
        fiber->native_stack_base = NULL;
        fiber->native_stack = NULL;
        fiber->native_stack_size = 0;
        fiber->native_stack_committed = NULL;
    }
}

/* Portable coroutine context switch for native-stack fibers.
 *
 * On x86_64/aarch64 we use a tiny cooperative register switch written as
 * file-scope inline asm; it lands in .text (W^X-safe), saves only the
 * callee-saved registers + stack pointer, and makes no syscalls. The whole
 * "machine context" is the saved stack pointer: every other piece of state
 * lives on the coroutine's own stack. Other architectures fall back to
 * <ucontext.h> swapcontext. */
#if defined(__x86_64__) || defined(__aarch64__)
#if defined(__APPLE__)
  #define BENDER_CTX_SYM "_bender_fiber_ctx_swap"
#else
  #define BENDER_CTX_SYM "bender_fiber_ctx_swap"
#endif
/* P3.6 lazy mapping: re-prime helper, declared early for reset. */
static void bender_fiber_reprime(BenderFiber *fiber);
#if defined(__x86_64__)
__asm__(
  ".text\n"
  ".globl " BENDER_CTX_SYM "\n"
  BENDER_CTX_SYM ":\n"
  "  pushq %rbp\n  pushq %rbx\n  pushq %r12\n  pushq %r13\n  pushq %r14\n  pushq %r15\n"
  "  movq %rsp, (%rdi)\n"
  "  movq (%rsi), %rsp\n"
  "  popq %r15\n  popq %r14\n  popq %r13\n  popq %r12\n  popq %rbx\n  popq %rbp\n"
  "  ret\n"
);
#else
__asm__(
  ".text\n"
  ".globl " BENDER_CTX_SYM "\n"
  BENDER_CTX_SYM ":\n"
  "  stp x19, x20, [sp, #-160]!\n"
  "  stp x21, x22, [sp, #16]\n"
  "  stp x23, x24, [sp, #32]\n"
  "  stp x25, x26, [sp, #48]\n"
  "  stp x27, x28, [sp, #64]\n"
  "  stp x29, x30, [sp, #80]\n"
  "  stp d8,  d9,  [sp, #96]\n"
  "  stp d10, d11, [sp, #112]\n"
  "  stp d12, d13, [sp, #128]\n"
  "  stp d14, d15, [sp, #144]\n"
  "  mov x2, sp\n"
  "  str x2, [x0]\n"
  "  ldr x2, [x1]\n"
  "  mov sp, x2\n"
  "  ldp d14, d15, [sp, #144]\n"
  "  ldp d12, d13, [sp, #128]\n"
  "  ldp d10, d11, [sp, #112]\n"
  "  ldp d8,  d9,  [sp, #96]\n"
  "  ldp x29, x30, [sp, #80]\n"
  "  ldp x27, x28, [sp, #64]\n"
  "  ldp x25, x26, [sp, #48]\n"
  "  ldp x23, x24, [sp, #32]\n"
  "  ldp x21, x22, [sp, #16]\n"
  "  ldp x19, x20, [sp], #160\n"
  "  ret\n"
);
#endif

void bender_fiber_ctx_prime(BenderFiberCtx *ctx, void *base, size_t size,
                           void (*entry)(void)) {
  uintptr_t top = ((uintptr_t)base + size) & ~(uintptr_t)15;
#if defined(__x86_64__)
  /* entry's first instruction needs rsp%16 == 8 (as if reached by `call`).
   * sp_fiber ctx_swap pops 6 callee-saved regs then `ret`s: [entry][rbp]
   * [rbx][r12][r13][r14][r15<-sp]. */
  void **s = (void **)(top - 8);
  *--s = (void *)entry;
  for (int i = 0; i < 6; i++) *--s = 0;
  ctx->sp = s;
#else
  void **s = (void **)(top - 160);
  for (int i = 0; i < 20; i++) s[i] = 0;
  s[11] = (void *)entry;
  ctx->sp = s;
#endif
}
#else
void bender_fiber_ctx_swap(BenderFiberCtx *from, BenderFiberCtx *to) {
  swapcontext(&from->uc, &to->uc);
}

void bender_fiber_ctx_prime(BenderFiberCtx *ctx, void *base, size_t size,
                           void (*entry)(void)) {
  getcontext(&ctx->uc);
  ctx->uc.uc_stack.ss_sp = base;
  ctx->uc.uc_stack.ss_size = size;
  ctx->uc.uc_link = NULL;
  makecontext(&ctx->uc, entry, 0);
}
#endif

static void fiber_reset(BenderFiber *fiber) {
    fiber->maxstack = BENDER_STACK_MAX;
    fiber->frame = 0;
    fiber->stackstart = BENDER_FRAME_SIZE;
    fiber->stacktop = BENDER_FRAME_SIZE;
    fiber->child = NULL;
    fiber->flags = BENDER_FIBER_MASK_YIELD | BENDER_FIBER_RESUME_NO_USEVAL | BENDER_FIBER_RESUME_NO_SKIP;
    fiber->env = NULL;
    fiber->last_value = bender_wrap_nil();
    fiber->in_value = bender_wrap_nil();
    fiber->out_payload = bender_wrap_nil();
    fiber->out_signal = BENDER_SIGNAL_OK;
    fiber->resume_ctx = NULL;
    /* Phase 1.1: fresh fibers re-enter run_vm from the trampoline with
     * clean parked registers (loaded by bender_continue_no_check). */
    fiber->suspend_mode = 0;
    fiber->resume_fiber = NULL;
    fiber->saved_signal_buf = NULL;
    fiber->saved_return_reg = NULL;
    fiber->saved_coerce_error = 0;
    fiber->saved_stackn = 0;
    fiber->saved_gc_suspend = 0;
    /* Phase D3: a reset restarts execution from the top (re-primed
     * below). Clear any stale flag, and reclaim red-zone space when
     * unwound fibers consumed it (DEAD/ERROR have no live C frames; a
     * suspended fiber's parked position may still point into it).
     * Gated on position, not the flag: raises clear the flag, so a
     * flag gate would skip exactly the fibers that need reclaim. */
    if (bender_fiber_status(fiber) == BENDER_STATUS_DEAD ||
            bender_fiber_status(fiber) == BENDER_STATUS_ERROR) {
        fiber->stack_overflowed = 0;
#ifndef BENDER_WINDOWS
        if (NULL != fiber->native_stack_committed &&
                NULL != fiber->native_stack &&
                fiber->native_stack_committed < fiber->native_stack) {
            /* Uncommit the red zone only; grown usable space persists. */
            char *usable = (char *) fiber->native_stack;
            char *rebased = (char *) fiber->native_stack_base +
                fiber->native_stack_size - BENDER_FIBER_STACK_INIT_BYTES;
            if (rebased < usable) rebased = usable;
            mprotect(fiber->native_stack_base,
                     (size_t) (usable - (char *) fiber->native_stack_base),
                     PROT_NONE);
            fiber->native_stack_committed = rebased;
        }
#endif
    }
    /* Reset restarts from the top: re-prime mapped fibers, leave
     * unmapped ones for the resume path (P3.6 lazy mapping). */
    bender_fiber_reprime(fiber);
#ifdef BENDER_EV
    fiber->sched_id = 0;
    fiber->ev_callback = NULL;
    fiber->ev_state = NULL;
    fiber->ev_stream = NULL;
    fiber->supervisor_channel = NULL;
    fiber->interrupt_requested = 0;
    fiber->interrupt_value = bender_wrap_nil();
#endif
    bender_fiber_set_status(fiber, BENDER_STATUS_NEW);
}

static BenderFiber *fiber_alloc(int32_t capacity) {
    Bender *data;
    BenderFiber *fiber = bender_gcalloc(BENDER_MEMORY_FIBER, sizeof(BenderFiber));
    if (capacity < 32) {
        capacity = 32;
    }
    fiber->capacity = capacity;
    bender_fiber_init_owner(fiber);
    /* P3.3: strict heaps are opt-in per fiber (spawn-isolated sets it
     * after alloc; unmarshal leaves it clear). */
    fiber->heap_strict = 0;
    /* P3.5: no owning proc (proc spawn sets it). */
    fiber->proc = NULL;
    /* P3.6: private stacks by default (ev/go + proc spawn opt in). */
    fiber->shared_stack = 0;
    /* P3.5b: heap lists start empty (births file here). */
    {
        int hli;
        for (hli = 0; hli < 3; hli++)
            fiber->heap_lists[hli] = NULL;
    }
    /* Channel-pending registrations start empty (gcalloc does not
     * zero fresh bump slots — same rule as heap_lists above). */
    fiber->chan_pends = NULL;
    /* Census slot sentinel BEFORE census_add (which assigns the real
     * index): gcalloc hands back unzeroed bump slots, so without this
     * any fiber reaching census_remove without having been added would
     * carry a garbage index that could spuriously pass the fast-path
     * validation. Sentinel first, real index second. */
    fiber->census_idx = BENDER_CENSUS_UNTRACKED;
    bender_census_add(fiber);
    {
        /* Step 1.2: nursery starts empty (pages claimed on first bump). */
        int nci;
        for (nci = 0; nci < BENDER_SLAB_NCLASSES; nci++)
            fiber->nursery_cur[nci] = NULL;
        fiber->gc_nursery_bytes = 0;
    }
    data = array_allocate(sizeof(Bender), capacity);
    if (NULL == data) {
        BENDER_OUT_OF_MEMORY;
    }
    bender_vm.next_collection += sizeof(Bender) * capacity;
    fiber->data = data;
    /* This fiber's native mapping is created lazily on first resume
     * (see bender_continue_no_check) so fibers that never run map
     * nothing — and shared-stack fibers never map privately at all. */
    fiber->native_stack_base = NULL;
    fiber->native_stack = NULL;
    fiber->native_stack_size = 0;
    fiber->native_stack_committed = NULL;
    fiber->stack_overflowed = 0;
    return fiber;
}

/* Step 1 (process heaps): assign a fresh owner id from the VM counter
 * (0 reserved for shared). Used at every fiber birth site (alloc,
 * unmarshal); resets keep the id (same logical fiber). */
void bender_fiber_init_owner(BenderFiber *fiber) {
    uint32_t id = ++bender_vm.gc_owner_next;
    if (0 == id) id = ++bender_vm.gc_owner_next;
    fiber->gc_owner = id;
}

/* Allocate the fiber's native stack on first use and (re)prime its switch
 * context to enter the VM trampoline. See fiber.h for when re-priming
 * is valid. */
void bender_fiber_ensure_native_stack(BenderFiber *fiber) {
    /* P3.6: shared-stack fibers hold no private mapping (each resume
     * re-primes on the lane mapping instead). */
    if (fiber->shared_stack) return;
    if (!fiber->native_stack_base) {
        size_t guard = 0;
        void *committed = NULL;
        fiber->native_stack_base = fiber_native_stack_alloc(&fiber->native_stack_size,
                                                            &guard, &committed);
#ifndef BENDER_WINDOWS
        /* Usable start is fixed (base + guard); the committed window
         * starts at the top and grows down on faults. Priming spans the
         * full usable window: sp starts at the top (always committed),
         * and downward growth never moves it, so no re-prime is needed. */
        fiber->native_stack = (char *)fiber->native_stack_base + guard;
        fiber->native_stack_committed = committed;
#else
        fiber->native_stack = fiber->native_stack_base;
        fiber->native_stack_committed = NULL;
#endif
    }
        bender_fiber_reprime(fiber);
}

/* Re-prime a mapped fiber's switch context (trampoline entry).
 * Mapping must exist (no-op otherwise — resume paths map first). */
static void bender_fiber_reprime(BenderFiber *fiber) {
    if (fiber->native_stack) {
        size_t usable = fiber->native_stack_size -
            (size_t)((char *)fiber->native_stack -
                     (char *)fiber->native_stack_base);
        bender_fiber_ctx_prime(&fiber->ctx, fiber->native_stack, usable,
                              bender_fiber_trampoline);
    }
}

/* P3.6 lane-shared mapping: same reserve/commit/guard shape as private
 * stacks (fiber_native_stack_alloc), but VM-scoped and created lazily
 * so VMs without shared fibers map nothing. Growth/red-zone/overflow
 * handling is shared with the private path (see the fault handler —
 * shared faults attribute to the running fiber). */
void bender_fiber_shared_prime(BenderFiber *fiber) {
    if (NULL == bender_vm.shared_stack_base) {
        size_t guard = 0;
        void *committed = NULL;
        void *base = fiber_native_stack_alloc(&bender_vm.shared_stack_size,
                                              &guard, &committed);
        bender_vm.shared_stack_base = base;
        bender_vm.shared_stack = (char *)base + guard;
        bender_vm.shared_stack_committed = committed;
    }
    {
        size_t usable = bender_vm.shared_stack_size -
            (size_t)((char *)bender_vm.shared_stack -
                     (char *)bender_vm.shared_stack_base);
                bender_fiber_ctx_prime(&fiber->ctx, bender_vm.shared_stack, usable,
                              bender_fiber_trampoline);
    }
}
BenderFiber *bender_fiber_reset(BenderFiber *fiber, BenderFunction *callee, int32_t argc, const Bender *argv) {
    int32_t newstacktop;
    /* Phase 2a invariant: never start a fiber on a half-built function.
     * Caught real use-after-sweep construction bugs under GC stress (the
     * def must be fully populated before any fiber references it). */
    if (NULL == callee || NULL == callee->def || NULL == callee->def->bytecode) {
        BenderFiber *cur = bender_vm.fiber;
        fprintf(stderr, "bender_fiber_reset: fiber started on half-built function (callee=%p def=%p bytecode=%p)\n",
                (void *) callee,
                (void *) (callee ? (void *) callee->def : NULL),
                (void *) (callee && callee->def ? (void *) callee->def->bytecode : NULL));
        if (cur) {
            bender_stacktrace(cur, bender_wrap_nil());
        }
        abort();
    }
    fiber_reset(fiber);
    if (argc) {
        newstacktop = fiber->stacktop + argc;
        if (newstacktop >= fiber->capacity) {
            bender_fiber_setcapacity(fiber, 2 * newstacktop);
        }
        if (argv) {
            memcpy(fiber->data + fiber->stacktop, argv, argc * sizeof(Bender));
        } else {
            /* If argv not given, fill with nil */
            for (int32_t i = 0; i < argc; i++) {
                fiber->data[fiber->stacktop + i] = bender_wrap_nil();
            }
        }
        fiber->stacktop = newstacktop;
    }
    /* Don't panic on failure since we use this to implement bender_pcall */
    if (bender_fiber_funcframe(fiber, callee)) return NULL;
    bender_fiber_frame(fiber)->flags |= BENDER_STACKFRAME_ENTRANCE;
#ifdef BENDER_EV
    fiber->supervisor_channel = NULL;
    fiber->interrupt_requested = 0;
    fiber->interrupt_value = bender_wrap_nil();
#endif
    return fiber;
}

/* Create a new fiber with argn values on the stack. */
BenderFiber *bender_fiber(BenderFunction *callee, int32_t capacity, int32_t argc, const Bender *argv) {
    return bender_fiber_reset(fiber_alloc(capacity), callee, argc, argv);
}

#ifdef BENDER_DEBUG
/* Test for memory issues by reallocating fiber every time we push a stack frame */
static void bender_fiber_refresh_memory(BenderFiber *fiber) {
    int32_t n = fiber->capacity;
    if (n) {
        Bender *newData = array_allocate(sizeof(Bender), n);
        if (NULL == newData) {
            BENDER_OUT_OF_MEMORY;
        }
        memcpy(newData, fiber->data, n * sizeof(Bender));
        bender_free(fiber->data);
        fiber->data = newData;
    }
}
#endif

/* Ensure that the fiber has enough extra capacity */
void bender_fiber_setcapacity(BenderFiber *fiber, int32_t n) {
    int32_t old_size = fiber->capacity;
    int32_t diff = n - old_size;
    Bender *newData = bender_realloc(fiber->data, sizeof(Bender) * n);
    if (NULL == newData) {
        BENDER_OUT_OF_MEMORY;
    }
    fiber->data = newData;
    fiber->capacity = n;
    bender_vm.next_collection += sizeof(Bender) * diff;
}

/* Grow fiber if needed */
static void bender_fiber_grow(BenderFiber *fiber, int32_t needed) {
    int32_t cap = needed > (INT32_MAX / 2) ? INT32_MAX : 2 * needed;
    bender_fiber_setcapacity(fiber, cap);
}

/* Push a value on the next stack frame */
void bender_fiber_push(BenderFiber *fiber, Bender x) {
    if (fiber->stacktop == INT32_MAX) bender_panic("stack overflow");
    if (fiber->stacktop >= fiber->capacity) {
        bender_fiber_grow(fiber, fiber->stacktop);
    }
    fiber->data[fiber->stacktop++] = x;
}

/* Push 2 values on the next stack frame */
void bender_fiber_push2(BenderFiber *fiber, Bender x, Bender y) {
    if (fiber->stacktop >= INT32_MAX - 1) bender_panic("stack overflow");
    int32_t newtop = fiber->stacktop + 2;
    if (newtop > fiber->capacity) {
        bender_fiber_grow(fiber, newtop);
    }
    fiber->data[fiber->stacktop] = x;
    fiber->data[fiber->stacktop + 1] = y;
    fiber->stacktop = newtop;
}

/* Push 3 values on the next stack frame */
void bender_fiber_push3(BenderFiber *fiber, Bender x, Bender y, Bender z) {
    if (fiber->stacktop >= INT32_MAX - 2) bender_panic("stack overflow");
    int32_t newtop = fiber->stacktop + 3;
    if (newtop > fiber->capacity) {
        bender_fiber_grow(fiber, newtop);
    }
    fiber->data[fiber->stacktop] = x;
    fiber->data[fiber->stacktop + 1] = y;
    fiber->data[fiber->stacktop + 2] = z;
    fiber->stacktop = newtop;
}

/* Push an array on the next stack frame */
void bender_fiber_pushn(BenderFiber *fiber, const Bender *arr, int32_t n) {
    if (fiber->stacktop > INT32_MAX - n) bender_panic("stack overflow");
    int32_t newtop = fiber->stacktop + n;
    if (newtop > fiber->capacity) {
        bender_fiber_grow(fiber, newtop);
    }
    safe_memcpy(fiber->data + fiber->stacktop, arr, n * sizeof(Bender));
    fiber->stacktop = newtop;
}

/* Create a struct with n values. If n is odd, the last value is ignored. */
static Bender make_struct_n(const Bender *args, int32_t n) {
    int32_t i = 0;
    if (n & 1) n--;
    BenderKV *st = bender_struct_begin(n);
    for (; i < n; i += 2) {
        bender_struct_put(st, args[i], args[i + 1]);
    }
    return bender_wrap_struct(bender_struct_end(st));
}

/* Push a stack frame to a fiber */
int bender_fiber_funcframe(BenderFiber *fiber, BenderFunction *func) {
    BenderStackFrame *newframe;

    int32_t i;
    int32_t oldtop = fiber->stacktop;
    int32_t oldframe = fiber->frame;
    int32_t nextframe = fiber->stackstart;
    int32_t nextstacktop = nextframe + func->def->slotcount + BENDER_FRAME_SIZE;
    int32_t next_arity = fiber->stacktop - fiber->stackstart;

    /* Check strict arity before messing with state */
    if (next_arity < func->def->min_arity) return 1;
    if (next_arity > func->def->max_arity) return 1;

    if (fiber->capacity < nextstacktop) {
        bender_fiber_setcapacity(fiber, 2 * nextstacktop);
#ifdef BENDER_DEBUG
    } else {
        bender_fiber_refresh_memory(fiber);
#endif
    }

    /* Nil unset stack arguments (Needed for gc correctness) */
    for (i = fiber->stacktop; i < nextstacktop; ++i) {
        fiber->data[i] = bender_wrap_nil();
    }

    /* Set up the next frame */
    fiber->frame = nextframe;
    fiber->stacktop = fiber->stackstart = nextstacktop;
    newframe = bender_fiber_frame(fiber);
    newframe->prevframe = oldframe;
    newframe->pc = func->def->bytecode;
    newframe->func = func;
    newframe->env = NULL;
    newframe->flags = 0;

    /* Check varargs */
    if (func->def->flags & BENDER_FUNCDEF_FLAG_VARARG) {
        int32_t tuplehead = fiber->frame + func->def->arity;
        bender_assert(tuplehead > 0, "fiber stack overflow");
        int st = func->def->flags & BENDER_FUNCDEF_FLAG_STRUCTARG;
        if (tuplehead >= oldtop) {
            fiber->data[tuplehead] = st
                                     ? make_struct_n(NULL, 0)
                                     : bender_wrap_tuple(bender_tuple_n(NULL, 0));
        } else {
            fiber->data[tuplehead] = st
                                     ? make_struct_n(
                                         fiber->data + tuplehead,
                                         oldtop - tuplehead)
                                     : bender_wrap_tuple(bender_tuple_n(
                                             fiber->data + tuplehead,
                                             oldtop - tuplehead));
        }
    }

    /* Good return */
    return 0;
}

/* If a frame has a closure environment, detach it from
 * the stack and have it keep its own values */
static void bender_env_detach(BenderFuncEnv *env) {
    /* Check for closure environment */
    if (env) {
        bender_env_valid(env);
        int32_t len = env->length; /* Maximum is 256 */
        size_t s = sizeof(Bender) * (size_t) len;
        Bender *vmem = bender_malloc(s);
        bender_vm.next_collection += (uint32_t) s;
        if (NULL == vmem) {
            BENDER_OUT_OF_MEMORY;
        }
        Bender *values = env->as.fiber->data + env->offset;
        safe_memcpy(vmem, values, s);
        uint32_t *bitset = bender_stack_frame(values)->func->def->closure_bitset;
        if (bitset) {
            /* Clear unneeded references in closure environment */
            for (int32_t i = 0; i < len; i += 32) {
                uint32_t mask = ~(bitset[i >> 5]);
                int32_t maxj = i + 32 > len ? len : i + 32;
                for (int32_t j = i; j < maxj; j++) {
                    if (mask & 1) vmem[j] = bender_wrap_nil();
                    mask >>= 1;
                }
            }
        }
        env->offset = 0;
        env->as.values = vmem;
    }
}

/* Validate potentially untrusted func env (unmarshalled envs are difficult to verify) */
int bender_env_valid(BenderFuncEnv *env) {
    if (env->offset < 0) {
        int32_t real_offset = -(env->offset);
        BenderFiber *fiber = env->as.fiber;
        int32_t i = fiber->frame;
        while (i > 0) {
            BenderStackFrame *frame = (BenderStackFrame *)(fiber->data + i - BENDER_FRAME_SIZE);
            if (real_offset == i &&
                    frame->env == env &&
                    frame->func &&
                    frame->func->def->slotcount == env->length) {
                env->offset = real_offset;
                return 1;
            }
            i = frame->prevframe;
        }
        /* Invalid, set to empty off-stack variant. */
        env->offset = 0;
        env->length = 0;
        env->as.values = NULL;
        return 0;
    } else {
        return 1;
    }
}

/* Detach a fiber from the env if the target fiber has stopped mutating */
void bender_env_maybe_detach(BenderFuncEnv *env) {
    /* Check for detachable closure envs */
    bender_env_valid(env);
    if (env->offset > 0) {
        BenderFiberStatus s = bender_fiber_status(env->as.fiber);
        int isFinished = s == BENDER_STATUS_DEAD ||
                         s == BENDER_STATUS_ERROR ||
                         s == BENDER_STATUS_USER0 ||
                         s == BENDER_STATUS_USER1 ||
                         s == BENDER_STATUS_USER2 ||
                         s == BENDER_STATUS_USER3 ||
                         s == BENDER_STATUS_USER4;
        if (isFinished) {
            bender_env_detach(env);
        }
    }
}

/* Create a tail frame for a function */
int bender_fiber_funcframe_tail(BenderFiber *fiber, BenderFunction *func) {
    int32_t i;
    int32_t nextframetop = fiber->frame + func->def->slotcount;
    int32_t nextstacktop = nextframetop + BENDER_FRAME_SIZE;
    int32_t next_arity = fiber->stacktop - fiber->stackstart;
    int32_t stacksize;

    /* Check strict arity before messing with state */
    if (next_arity < func->def->min_arity) return 1;
    if (next_arity > func->def->max_arity) return 1;

    if (fiber->capacity < nextstacktop) {
        bender_fiber_setcapacity(fiber, 2 * nextstacktop);
#ifdef BENDER_DEBUG
    } else {
        bender_fiber_refresh_memory(fiber);
#endif
    }

    /* Detach old function */
    if (NULL != bender_fiber_frame(fiber)->func)
        bender_env_detach(bender_fiber_frame(fiber)->env);
    bender_fiber_frame(fiber)->env = NULL;

    /* Check varargs */
    if (func->def->flags & BENDER_FUNCDEF_FLAG_VARARG) {
        int32_t tuplehead = fiber->stackstart + func->def->arity;
        int st = func->def->flags & BENDER_FUNCDEF_FLAG_STRUCTARG;
        if (tuplehead >= fiber->stacktop) {
            if (tuplehead >= fiber->capacity) bender_fiber_setcapacity(fiber, 2 * (tuplehead + 1));
            for (i = fiber->stacktop; i < tuplehead; ++i) fiber->data[i] = bender_wrap_nil();
            fiber->data[tuplehead] = st
                                     ? make_struct_n(NULL, 0)
                                     : bender_wrap_tuple(bender_tuple_n(NULL, 0));
        } else {
            fiber->data[tuplehead] = st
                                     ? make_struct_n(
                                         fiber->data + tuplehead,
                                         fiber->stacktop - tuplehead)
                                     : bender_wrap_tuple(bender_tuple_n(
                                             fiber->data + tuplehead,
                                             fiber->stacktop - tuplehead));
        }
        stacksize = tuplehead - fiber->stackstart + 1;
    } else {
        stacksize = fiber->stacktop - fiber->stackstart;
    }

    if (stacksize) memmove(fiber->data + fiber->frame, fiber->data + fiber->stackstart, stacksize * sizeof(Bender));

    /* Nil unset locals (Needed for functional correctness) */
    for (i = fiber->frame + stacksize; i < nextframetop; ++i)
        fiber->data[i] = bender_wrap_nil();

    /* Set stack stuff */
    fiber->stacktop = fiber->stackstart = nextstacktop;

    /* Set frame stuff */
    bender_fiber_frame(fiber)->func = func;
    bender_fiber_frame(fiber)->pc = func->def->bytecode;
    bender_fiber_frame(fiber)->flags |= BENDER_STACKFRAME_TAILCALL;

    /* Good return */
    return 0;
}

/* Push a stack frame to a fiber for a c function */
void bender_fiber_cframe(BenderFiber *fiber, BenderCFunction cfun) {
    BenderStackFrame *newframe;

    int32_t oldframe = fiber->frame;
    int32_t nextframe = fiber->stackstart;
    int32_t nextstacktop = fiber->stacktop + BENDER_FRAME_SIZE;

    if (fiber->capacity < nextstacktop) {
        bender_fiber_setcapacity(fiber, 2 * nextstacktop);
#ifdef BENDER_DEBUG
    } else {
        bender_fiber_refresh_memory(fiber);
#endif
    }

    /* Set the next frame */
    fiber->frame = nextframe;
    fiber->stacktop = fiber->stackstart = nextstacktop;
    newframe = bender_fiber_frame(fiber);

    /* Set up the new frame */
    newframe->prevframe = oldframe;
    newframe->pc = (uint32_t *) cfun;
    newframe->func = NULL;
    newframe->env = NULL;
    newframe->flags = 0;
}

/* Pop a stack frame from the fiber. */
void bender_fiber_popframe(BenderFiber *fiber) {
    BenderStackFrame *frame = bender_fiber_frame(fiber);
    if (fiber->frame == 0) return;

    /* Clean up the frame (detach environments) */
    if (NULL != frame->func)
        bender_env_detach(frame->env);

    /* Shrink stack */
    fiber->stacktop = fiber->stackstart = fiber->frame;
    fiber->frame = frame->prevframe;
}

BenderFiberStatus bender_fiber_status(BenderFiber *f) {
    return ((f)->flags & BENDER_FIBER_STATUS_MASK) >> BENDER_FIBER_STATUS_OFFSET;
}

BenderFiber *bender_current_fiber(void) {
    return bender_vm.fiber;
}

BenderFiber *bender_root_fiber(void) {
    return bender_vm.root_fiber;
}

/* CFuns */

BENDER_CORE_FN(cfun_fiber_getenv,
              "(fiber/getenv fib)",
              "Gets the environment for a fiber `fib`. Returns nil if no "
              "such table is set yet.") {
    bender_fixarity(argc, 1);
    BenderFiber *fiber = bender_getfiber(argv, 0);
    return fiber->env ?
           bender_wrap_table(fiber->env) :
           bender_wrap_nil();
}

BENDER_CORE_FN(cfun_fiber_setenv,
              "(fiber/setenv fib tab)",
              "Sets the environment table for a fiber `fib` to `tab`. Set "
              "to nil to remove the current environment.") {
    bender_fixarity(argc, 2);
    BenderFiber *fiber = bender_getfiber(argv, 0);
    if (bender_checktype(argv[1], BENDER_NIL)) {
        fiber->env = NULL;
    } else {
        fiber->env = bender_gettable(argv, 1);
    }
    return argv[0];
}

BENDER_CORE_FN(cfun_fiber_new,
              "(fiber/new func &opt sigmask env)",
              "Create a new fiber with function body func. Can optionally "
              "take a set of signals `sigmask` to capture from child fibers, "
              "and an environment table `env`. The mask is specified as a keyword where each character "
              "is used to indicate a signal to block. If the ev module is enabled, and "
              "this fiber is used as an argument to `ev/go`, these \"blocked\" signals "
              "will result in messages being sent to the supervisor channel. "
              "The default sigmask is :y. "
              "For example,\n\n"
              "    (fiber/new myfun :e123)\n\n"
              "blocks error signals and user signals 1, 2 and 3. The signals are "
              "as follows:\n\n"
              "* :a - block all signals\n"
              "* :d - block debug signals\n"
              "* :e - block error signals\n"
              "* :t - block termination signals: error + user[0-4]\n"
              "* :u - block user signals\n"
              "* :y - block yield signals\n"
              "* :w - block await signals (user9)\n"
              "* :r - block interrupt signals (user8)\n"
              "* :0-9 - block a specific user signal\n\n"
              "The sigmask argument also can take environment flags. If any mutually "
              "exclusive flags are present, the last flag takes precedence.\n\n"
              "* :i - inherit the environment from the current fiber\n"
              "* :p - the environment table's prototype is the current environment table") {
    bender_arity(argc, 1, 3);
    BenderFunction *func = bender_getfunction(argv, 0);
    BenderFiber *fiber;
    if (func->def->min_arity > 1) {
        bender_panicf("fiber function must accept 0 or 1 arguments");
    }
    fiber = bender_fiber(func, 64, func->def->min_arity, NULL);
    bender_assert(fiber != NULL, "bad fiber arity check");
    if (argc == 3 && !bender_checktype(argv[2], BENDER_NIL)) {
        fiber->env = bender_gettable(argv, 2);
    }
    if (argc >= 2) {
        int32_t i;
        BenderByteView view = bender_getbytes(argv, 1);
        fiber->flags = BENDER_FIBER_RESUME_NO_USEVAL | BENDER_FIBER_RESUME_NO_SKIP;
        bender_fiber_set_status(fiber, BENDER_STATUS_NEW);
        for (i = 0; i < view.len; i++) {
            if (view.bytes[i] >= '0' && view.bytes[i] <= '9') {
                fiber->flags |= BENDER_FIBER_MASK_USERN(view.bytes[i] - '0');
            } else {
                switch (view.bytes[i]) {
                    default:
                        bender_panicf("invalid flag %c, expected a, t, d, e, u, y, w, r, i, or p", view.bytes[i]);
                        break;
                    case 'a':
                        fiber->flags |=
                            BENDER_FIBER_MASK_DEBUG |
                            BENDER_FIBER_MASK_ERROR |
                            BENDER_FIBER_MASK_USER |
                            BENDER_FIBER_MASK_YIELD;
                        break;
                    case 't':
                        fiber->flags |=
                            BENDER_FIBER_MASK_ERROR |
                            BENDER_FIBER_MASK_USER0 |
                            BENDER_FIBER_MASK_USER1 |
                            BENDER_FIBER_MASK_USER2 |
                            BENDER_FIBER_MASK_USER3 |
                            BENDER_FIBER_MASK_USER4;
                        break;
                    case 'd':
                        fiber->flags |= BENDER_FIBER_MASK_DEBUG;
                        break;
                    case 'e':
                        fiber->flags |= BENDER_FIBER_MASK_ERROR;
                        break;
                    case 'u':
                        fiber->flags |= BENDER_FIBER_MASK_USER;
                        break;
                    case 'y':
                        fiber->flags |= BENDER_FIBER_MASK_YIELD;
                        break;
                    case 'w':
                        fiber->flags |= BENDER_FIBER_MASK_USER9;
                        break;
                    case 'r':
                        fiber->flags |= BENDER_FIBER_MASK_USER8;
                        break;
                    case 'i':
                        if (!bender_vm.fiber->env) {
                            bender_vm.fiber->env = bender_table(0);
                        }
                        fiber->env = bender_vm.fiber->env;
                        break;
                    case 'p':
                        if (!bender_vm.fiber->env) {
                            bender_vm.fiber->env = bender_table(0);
                        }
                        fiber->env = bender_table(0);
                        fiber->env->proto = bender_vm.fiber->env;
                        break;
                }
            }
        }
    }
    return bender_wrap_fiber(fiber);
}

BENDER_CORE_FN(cfun_fiber_status,
              "(fiber/status fib)",
              "Get the status of a fiber. The status will be one of:\n\n"
              "* :dead - the fiber has finished\n"
              "* :error - the fiber has errored out\n"
              "* :debug - the fiber is suspended in debug mode\n"
              "* :pending - the fiber has been yielded\n"
              "* :user(0-7) - the fiber is suspended by a user signal\n"
              "* :interrupted - the fiber was interrupted\n"
              "* :suspended - the fiber is waiting to be resumed by the scheduler\n"
              "* :new - the fiber has just been created and not yet run\n"
              "* :alive - the fiber is currently running and cannot be resumed") {
    bender_fixarity(argc, 1);
    BenderFiber *fiber = bender_getfiber(argv, 0);
    uint32_t s = bender_fiber_status(fiber);
    return bender_ckeywordv(bender_status_names[s]);
}

BENDER_CORE_FN(cfun_fiber_current,
              "(fiber/current)",
              "Returns the currently running fiber.") {
    (void) argv;
    bender_fixarity(argc, 0);
    return bender_wrap_fiber(bender_vm.fiber);
}

BENDER_CORE_FN(cfun_fiber_root,
              "(fiber/root)",
              "Returns the current root fiber. The root fiber is the oldest "
              "ancestor that does not have a parent. Note that a root fiber "
              "is also a task fiber.") {
    (void) argv;
    bender_fixarity(argc, 0);
    return bender_wrap_fiber(bender_vm.root_fiber);
}

BENDER_CORE_FN(cfun_fiber_maxstack,
              "(fiber/maxstack fib)",
              "Gets the maximum stack size in bender values allowed for a fiber. While memory for "
              "the fiber's stack is not allocated up front, the fiber will not allocated more "
              "than this amount and will throw a stack-overflow error if more memory is needed. ") {
    bender_fixarity(argc, 1);
    BenderFiber *fiber = bender_getfiber(argv, 0);
    return bender_wrap_integer(fiber->maxstack);
}

BENDER_CORE_FN(cfun_fiber_setmaxstack,
              "(fiber/setmaxstack fib maxstack)",
              "Sets the maximum stack size in bender values for a fiber. By default, the "
              "maximum stack size is usually 8192.") {
    bender_fixarity(argc, 2);
    BenderFiber *fiber = bender_getfiber(argv, 0);
    int32_t maxs = bender_getinteger(argv, 1);
    if (maxs < 0) {
        bender_panic("expected positive integer");
    }
    fiber->maxstack = maxs;
    return argv[0];
}

int bender_fiber_can_resume(BenderFiber *fiber) {
    BenderFiberStatus s = bender_fiber_status(fiber);
    int isFinished = s == BENDER_STATUS_DEAD ||
                     s == BENDER_STATUS_ERROR ||
                     s == BENDER_STATUS_USER0 ||
                     s == BENDER_STATUS_USER1 ||
                     s == BENDER_STATUS_USER2 ||
                     s == BENDER_STATUS_USER3 ||
                     s == BENDER_STATUS_USER4;
    return !isFinished;
}

BENDER_CORE_FN(cfun_fiber_can_resume,
              "(fiber/can-resume? fib)",
              "Check if a fiber `fib` is finished and cannot be resumed.") {
    bender_fixarity(argc, 1);
    BenderFiber *fiber = bender_getfiber(argv, 0);
    return bender_wrap_boolean(bender_fiber_can_resume(fiber));
}

BENDER_CORE_FN(cfun_fiber_last_value,
              "(fiber/last-value fib)",
              "Get the last value returned or signaled from a fiber `fib`.") {
    bender_fixarity(argc, 1);
    BenderFiber *fiber = bender_getfiber(argv, 0);
    return fiber->last_value;
}

/* Module entry point */
void bender_lib_fiber(BenderTable *env) {
    BenderRegExt fiber_cfuns[] = {
        BENDER_CORE_REG("fiber/new", cfun_fiber_new),
        BENDER_CORE_REG("fiber/status", cfun_fiber_status),
        BENDER_CORE_REG("fiber/root", cfun_fiber_root),
        BENDER_CORE_REG("fiber/current", cfun_fiber_current),
        BENDER_CORE_REG("fiber/maxstack", cfun_fiber_maxstack),
        BENDER_CORE_REG("fiber/setmaxstack", cfun_fiber_setmaxstack),
        BENDER_CORE_REG("fiber/getenv", cfun_fiber_getenv),
        BENDER_CORE_REG("fiber/setenv", cfun_fiber_setenv),
        BENDER_CORE_REG("fiber/can-resume?", cfun_fiber_can_resume),
        BENDER_CORE_REG("fiber/last-value", cfun_fiber_last_value),
        BENDER_REG_END
    };
    bender_core_cfuns_ext(env, NULL, fiber_cfuns);
}
