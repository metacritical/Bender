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

#ifndef BENDER_FIBER_H_defined
#define BENDER_FIBER_H_defined

#ifndef BENDER_AMALG
#include <bender.h>
#endif

/* Fiber signal masks. */
#define BENDER_FIBER_MASK_ERROR 2
#define BENDER_FIBER_MASK_DEBUG 4
#define BENDER_FIBER_MASK_YIELD 8

#define BENDER_FIBER_MASK_USER0 (16 << 0)
#define BENDER_FIBER_MASK_USER1 (16 << 1)
#define BENDER_FIBER_MASK_USER2 (16 << 2)
#define BENDER_FIBER_MASK_USER3 (16 << 3)
#define BENDER_FIBER_MASK_USER4 (16 << 4)
#define BENDER_FIBER_MASK_USER5 (16 << 5)
#define BENDER_FIBER_MASK_USER6 (16 << 6)
#define BENDER_FIBER_MASK_USER7 (16 << 7)
#define BENDER_FIBER_MASK_USER8 (16 << 8)
#define BENDER_FIBER_MASK_USER9 (16 << 9)

#define BENDER_FIBER_MASK_USERN(N) (16 << (N))
#define BENDER_FIBER_MASK_USER 0x3FF0

#define BENDER_FIBER_STATUS_MASK 0x3F0000
#define BENDER_FIBER_RESUME_SIGNAL 0x400000
#define BENDER_FIBER_STATUS_OFFSET 16

#define BENDER_FIBER_BREAKPOINT       0x1000000
#define BENDER_FIBER_RESUME_NO_USEVAL 0x2000000
#define BENDER_FIBER_RESUME_NO_SKIP   0x4000000
#define BENDER_FIBER_DID_LONGJUMP     0x8000000
#define BENDER_FIBER_FLAG_MASK        0xF000000

/* Flags on the gc header (don't confuse flags with gc.flags!) */
/* Don't collide with BENDER_FIBER_STATUS_MASK to enable flag smuggling in vm.c */
#define BENDER_FIBER_EV_GCFLAG_CANCELED 0x1000000
#define BENDER_FIBER_EV_GCFLAG_SUSPENDED 0x2000000
#define BENDER_FIBER_EV_GCFLAG_ROOT 0x4000000

/* Not on gc header */
#define BENDER_FIBER_EV_FLAG_IN_FLIGHT 0x1

#define bender_fiber_set_status(f, s) do {\
    (f)->flags &= ~BENDER_FIBER_STATUS_MASK;\
    (f)->flags |= (s) << BENDER_FIBER_STATUS_OFFSET;\
} while (0)

#define bender_stack_frame(s) ((BenderStackFrame *)((s) - BENDER_FRAME_SIZE))
#define bender_fiber_frame(f) bender_stack_frame((f)->data + (f)->frame)
void bender_fiber_setcapacity(BenderFiber *fiber, int32_t n);
void bender_fiber_push(BenderFiber *fiber, Bender x);
void bender_fiber_push2(BenderFiber *fiber, Bender x, Bender y);
void bender_fiber_push3(BenderFiber *fiber, Bender x, Bender y, Bender z);
void bender_fiber_pushn(BenderFiber *fiber, const Bender *arr, int32_t n);
int bender_fiber_funcframe(BenderFiber *fiber, BenderFunction *func);
int bender_fiber_funcframe_tail(BenderFiber *fiber, BenderFunction *func);
void bender_fiber_cframe(BenderFiber *fiber, BenderCFunction cfun);
void bender_fiber_popframe(BenderFiber *fiber);
void bender_env_maybe_detach(BenderFuncEnv *env);
int bender_env_valid(BenderFuncEnv *env);

/* The interpreter-side trampoline (defined in vm.c, referenced by the
 * native-stack priming in fiber.c). Each VM resumption of a fiber runs this
 * loop on that fiber's own native stack: establish a signal-handling
 * jmp_buf in fiber-local scope, run_vm once, deliver the outcome, and
 * context-switch back to the resumer. */
void bender_fiber_trampoline(void);

/* Ensure a fiber has a native stack and a primed switch context. Used both
 * for fresh fibers and for fibers reconstituted by unmarshal, which bypass
 * fiber_alloc. Re-priming discards any parked position, so it must only be
 * used when the fiber will (re)start execution from its committed pc. */
void bender_fiber_ensure_native_stack(BenderFiber *fiber);

/* Free a fiber's native stack mapping (native_stack_base/size). Called
 * from the GC sweep in gc.c. */
void bender_fiber_native_stack_free(BenderFiber *fiber);

/* P3.6: lazily create the lane-shared native mapping (reserve + INIT
 * commit + guard, same shape as private stacks) and prime `fiber`'s
 * switch context onto it. No-op when already mapped. Shared fibers
 * re-prime on every resume (their parked position is always the
 * committed pc — suspend paths abandon C frames by discipline). */
void bender_fiber_shared_prime(BenderFiber *fiber);

/* Step 1 (process heaps): assign a fresh owner id from the VM counter
 * (0 reserved for shared). Used at every fiber birth site (alloc,
 * unmarshal); resets keep the id (same logical fiber). */
void bender_fiber_init_owner(BenderFiber *fiber);

/* Phase D: install the grow-on-fault SIGSEGV handler (process-once)
 * plus this thread's alternate signal stack. Called from bender_init
 * (every VM thread). POSIX only; no-op on Windows and under
 * BENDER_NO_GROWSTACK. Replaces growth if the user installs their own
 * SIGSEGV handler via os/sigaction afterwards (documented). */
void bender_fiber_growstack_init(void);

#ifdef BENDER_EV
void bender_fiber_did_resume(BenderFiber *fiber);
#endif

#endif
