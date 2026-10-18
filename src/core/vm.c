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

#ifndef JANET_AMALG
#include "features.h"
#include <janet.h>
#include "state.h"
#include "fiber.h"
#include "gc.h"
#include "symcache.h"
#include "util.h"
#endif

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#endif

/* Virtual registers
 *
 * One instruction word
 * CC | BB | AA | OP
 * DD | DD | DD | OP
 * EE | EE | AA | OP
 */
#define A ((*pc >> 8)  & 0xFF)
#define B ((*pc >> 16) & 0xFF)
#define C (*pc >> 24)
#define D (*pc >> 8)
#define E (*pc >> 16)

/* Signed interpretations of registers */
#define CS (*((int32_t *)pc) >> 24)
#define DS (*((int32_t *)pc) >> 8)
#define ES (*((int32_t *)pc) >> 16)

/* Branch prediction for hot interpreter paths. Numbers are the common
 * case in ALU ops; GC thresholds rarely trip; errors are cold. */
#if defined(__GNUC__) || defined(__clang__)
#define JANET_LIKELY(x) __builtin_expect(!!(x), 1)
#define JANET_UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#define JANET_LIKELY(x) (x)
#define JANET_UNLIKELY(x) (x)
#endif

/* How we dispatch instructions. By default, we use
 * a switch inside an infinite loop. For GCC/clang, we use
 * computed gotos. */
#if defined(__GNUC__) && !defined(__EMSCRIPTEN__) && !defined(JANET_NO_COMPUTED_GOTOS)
#define JANET_USE_COMPUTED_GOTOS
#endif

/* Opcode histogram (diagnostic only, -DJANET_OPCODE_HISTOGRAM). Counts
 * dispatches per opcode and PAIRS of consecutive opcodes, so fusion /
 * superinstruction candidates can be chosen from data rather than
 * guessed. Dumped at exit. Adds two increments per dispatch, so it is
 * never on in a normal build. */
#ifdef JANET_OPCODE_HISTOGRAM
extern uint64_t janet_oph[256];
extern uint64_t janet_ophp[256][256];
extern int janet_oph_prev;
#define JANET_OPH_TICK() do { int o_ = (int)(*pc & 0xFF); janet_oph[o_]++;     if (janet_oph_prev >= 0) janet_ophp[janet_oph_prev][o_]++; janet_oph_prev = o_; } while (0)
#else
#define JANET_OPH_TICK() ((void)0)
#endif

#ifdef JANET_USE_COMPUTED_GOTOS
#define VM_START() { goto *op_lookup[first_opcode];
#define VM_END() }
#define VM_OP(op) label_##op :
#define VM_DEFAULT() label_unknown_op:
#define vm_next() do { vm_native(); JANET_OPH_TICK(); goto *op_lookup[*pc & 0xFF]; } while (0)
#define opcode (*pc & 0xFF)
#else
#define VM_START() uint8_t opcode = first_opcode; for (;;) {switch(opcode) {
#define VM_END() }}
#define VM_OP(op) case op :
#define VM_DEFAULT() default:
#define vm_next() vm_native(); opcode = *pc & 0xFF; continue
#endif

#ifdef JANET_OPCODE_HISTOGRAM
uint64_t janet_oph[256];
uint64_t janet_ophp[256][256];
int janet_oph_prev = -1;
static void janet_oph_dump(void) {
    uint64_t total = 0;
    for (int i = 0; i < 256; i++) total += janet_oph[i];
    if (!total) return;
    fprintf(stderr, "opcode-histogram total=%llu\n", (unsigned long long) total);
    /* singles, descending, top 24 */
    for (int rank = 0; rank < 24; rank++) {
        int best = -1; uint64_t bv = 0;
        for (int i = 0; i < 256; i++) if (janet_oph[i] > bv) { bv = janet_oph[i]; best = i; }
        if (best < 0 || !bv) break;
        fprintf(stderr, "op %d count=%llu pct=%.2f\n", best,
                (unsigned long long) bv, 100.0 * (double) bv / (double) total);
        janet_oph[best] = 0;
    }
    /* consecutive PAIRS, descending, top 24 -- fusion candidates */
    for (int rank = 0; rank < 24; rank++) {
        int ba = -1, bb = -1; uint64_t bv = 0;
        for (int a = 0; a < 256; a++)
            for (int b = 0; b < 256; b++)
                if (janet_ophp[a][b] > bv) { bv = janet_ophp[a][b]; ba = a; bb = b; }
        if (ba < 0 || !bv) break;
        fprintf(stderr, "pair %d,%d count=%llu pct=%.2f\n", ba, bb,
                (unsigned long long) bv, 100.0 * (double) bv / (double) total);
        janet_ophp[ba][bb] = 0;
    }
}
#endif

/* Commit and restore VM state before possible longjmp */
#define vm_commit() do { janet_stack_frame(stack)->pc = pc; } while (0)
#define vm_restore() do { \
    stack = fiber->data + fiber->frame; \
    pc = janet_stack_frame(stack)->pc; \
    func = janet_stack_frame(stack)->func; \
} while (0)
#define vm_return(sig, val) do { \
    janet_vm.return_reg[0] = (val); \
    vm_commit(); \
    return (sig); \
} while (0)
#define vm_return_no_restore(sig, val) do { \
    janet_vm.return_reg[0] = (val); \
    return (sig); \
} while (0)

/* Next instruction variations */
#define JANET_FIBER_MINOR_SCALE 256
#define JANET_FIBER_MINOR_CAP (16 * 1024 * 1024)
#define JANET_FULL_SCALE 1024
#define maybe_collect() do {\
    JanetFiber *mcf_ = janet_vm.fiber; \
    if (JANET_UNLIKELY(NULL != mcf_ && mcf_->gc_nursery_bytes >= janet_vm.gc_minor_threshold)) { \
        size_t fth_ = janet_vm.gc_minor_threshold; \
        if (!(mcf_->heap_strict && NULL != mcf_->proc)) { \
            fth_ += (size_t) janet_vm.block_count * JANET_FIBER_MINOR_SCALE; \
            if (fth_ > JANET_FIBER_MINOR_CAP) fth_ = JANET_FIBER_MINOR_CAP; } \
        if (mcf_->gc_nursery_bytes >= fth_) { \
            if (mcf_->heap_strict && NULL != mcf_->proc) janet_collect_process(mcf_->proc); \
            else janet_collect_fiber_minor(mcf_); } } \
    else if (JANET_UNLIKELY(janet_vm.gc_young_bytes >= janet_vm.gc_minor_threshold \
            + (size_t) janet_vm.block_count * JANET_FIBER_MINOR_SCALE)) janet_collect_minor(); \
    else if (JANET_UNLIKELY(janet_vm.next_collection >= janet_vm.gc_interval \
            + (size_t) janet_vm.block_count * JANET_FULL_SCALE)) janet_collect(); } while (0)
#define vm_checkgc_next() maybe_collect(); vm_next()
#define vm_pcnext() pc++; vm_next()
#define vm_checkgc_pcnext() maybe_collect(); vm_pcnext()

/* Handle certain errors in main vm loop */
#define vm_throw(e) do { vm_commit(); janet_panic(e); } while (0)
#define vm_assert(cond, e) do {if (!(cond)) vm_throw((e)); } while (0)
#define vm_assert_type(X, T) do { \
    if (!(janet_checktype((X), (T)))) { \
        vm_commit(); \
        janet_panicf("expected %T, got %v", (1 << (T)), (X)); \
    } \
} while (0)
#define vm_assert_types(X, TS) do { \
    if (!(janet_checktypes((X), (TS)))) { \
        vm_commit(); \
        janet_panicf("expected %T, got %v", (TS), (X)); \
    } \
} while (0)
#ifdef JANET_NATIVE_EXECUTORS
static JANET_THREAD_LOCAL int native_stepping;
/* Region gating and lazy structural attachment. Whole-definition
 * validation (janet_native_matches) runs at useful native region
 * starts recorded in the descriptor mask; consecutive VM-only
 * instructions after a boundary exit cost one bitmap check. The
 * verdict is cached per definition at the current validation epoch
 * (bumped at every resume entry, breakpoint write and publication),
 * so a long run pays full validation once per definition per resume
 * while any change to a definition between resumes is re-detected.
 * Lazy attachment tries the unique key-matching registered descriptor
 * once per registry generation per definition. */
#define vm_native() do { \
    if (!native_stepping) { \
        JanetFuncDef *ndef_ = func->def; \
        if (NULL != ndef_->native_code) { \
            if (janet_native_region_allowed(ndef_->native_code, (uint32_t)(pc - ndef_->bytecode))) { \
                if ((ndef_->native_verified != ndef_->native_code || \
                        ndef_->native_verified_epoch != janet_vm.native_epoch) && \
                        !janet_native_revalidate(ndef_)) break; \
                uint32_t *npc = pc; \
                vm_commit(); \
                JanetNativeStep result = ndef_->native_code->execute(fiber, ndef_, &npc); \
                pc = npc; \
                stack = fiber->data + fiber->frame; \
                vm_commit(); \
                if (result == JANET_NATIVE_VM) { \
                    janet_vm.native_vm_exits++; \
                    janet_vm.native_exit_opcodes[*pc & 0xFF]++; \
                } else { \
                    fiber->flags |= JANET_FIBER_RESUME_NO_USEVAL | JANET_FIBER_RESUME_NO_SKIP; \
                    vm_return(JANET_SIGNAL_INTERRUPT, janet_wrap_nil()); \
                } \
            } \
        } else if (janet_native_registry_count() && \
                ndef_->native_attach_gen != janet_native_registry_generation()) { \
            janet_native_try_attach(ndef_); \
        } \
    } \
} while (0)
#else
#define vm_native() ((void)0)
#endif

/* Keep the disabled interpreter path inline and unchanged. Both paths use
 * exactly this expression, including auto_suspend's short circuit. */
#define vm_poll_condition(COND) ((COND) && (janet_atomic_load_relaxed(&janet_vm.auto_suspend) || \
            (janet_vm.quantum && --janet_vm.quantum_remaining <= 0)))
int janet_native_poll(int condition) {
#ifdef JANET_NO_INTERPRETER_INTERRUPT
    (void)condition;
    return 0;
#else
    return vm_poll_condition(condition);
#endif
}

#ifdef JANET_NO_INTERPRETER_INTERRUPT
#define vm_maybe_auto_suspend(COND)
#else
#define vm_maybe_auto_suspend(COND) do { \
    if (vm_poll_condition(COND)) { \
        fiber->flags |= (JANET_FIBER_RESUME_NO_USEVAL | JANET_FIBER_RESUME_NO_SKIP); \
        vm_return(JANET_SIGNAL_INTERRUPT, janet_wrap_nil()); \
    } \
} while (0)
#endif

/* Phase D3: catchable stack overflow. The fault handler arms
 * fiber->stack_overflowed and commits emergency space; the error
 * surfaces here, never in the handler. Check-and-clear, then panic
 * like any runtime error. Only call paths check: C depth grows solely
 * through nesting (a parked loop cannot deepen the C stack). One
 * predictable branch per call. */
#define vm_maybe_overflow() do { \
    if (fiber->stack_overflowed) { \
        fiber->stack_overflowed = 0; \
        janet_panic("fiber native stack overflow"); \
    } \
} while (0)

/* Templates for certain patterns in opcodes */
#define vm_binop_immediate(op)\
    {\
        Janet op1 = stack[B];\
        if (JANET_UNLIKELY(!janet_checktype(op1, JANET_NUMBER))) {\
            vm_commit();\
            Janet _argv[2] = { op1, janet_wrap_number(CS) };\
            Janet a = janet_mcall(#op, 2, _argv);\
            stack = fiber->data + fiber->frame;\
            stack[A] = a;\
            vm_checkgc_pcnext();\
        } else {\
            double x1 = janet_unwrap_number(op1);\
            stack[A] = janet_wrap_number(x1 op CS);\
            vm_pcnext();\
        }\
    }
#define _vm_bitop_immediate(op, type1, rangecheck, msg)\
    {\
        Janet op1 = stack[B];\
        if (JANET_UNLIKELY(!janet_checktype(op1, JANET_NUMBER))) {\
            vm_commit();\
            Janet _argv[2] = { op1, janet_wrap_number(CS) };\
            Janet a = janet_mcall(#op, 2, _argv);\
            stack = fiber->data + fiber->frame;\
            stack[A] = a;\
            vm_checkgc_pcnext();\
        } else {\
            double y1 = janet_unwrap_number(op1);\
            if (!rangecheck(y1)) { vm_commit(); janet_panicf("value %v out of range for " msg, op1); }\
            type1 x1 = (type1) y1;\
            stack[A] = janet_wrap_number((type1) (x1 op CS));\
            vm_pcnext();\
        }\
    }
#define vm_bitop_immediate(op) _vm_bitop_immediate(op, int32_t, janet_checkintrange, "32-bit signed integers");
#define vm_bitopu_immediate(op) _vm_bitop_immediate(op, uint32_t, janet_checkuintrange, "32-bit unsigned integers");
#define _vm_binop(op, wrap)\
    {\
        Janet op1 = stack[B];\
        Janet op2 = stack[C];\
        if (JANET_LIKELY(janet_checktype(op1, JANET_NUMBER) && janet_checktype(op2, JANET_NUMBER))) {\
            double x1 = janet_unwrap_number(op1);\
            double x2 = janet_unwrap_number(op2);\
            stack[A] = wrap(x1 op x2);\
            vm_pcnext();\
        } else {\
            vm_commit();\
            Janet a = janet_binop_call(#op, "r" #op, op1, op2);\
            stack = fiber->data + fiber->frame;\
            stack[A] = a;\
            vm_checkgc_pcnext();\
        }\
    }
#define vm_binop(op) _vm_binop(op, janet_wrap_number)
#define _vm_bitop(op, type1, rangecheck, msg)\
    {\
        Janet op1 = stack[B];\
        Janet op2 = stack[C];\
        if (JANET_LIKELY(janet_checktype(op1, JANET_NUMBER) && janet_checktype(op2, JANET_NUMBER))) {\
            double y1 = janet_unwrap_number(op1);\
            double y2 = janet_unwrap_number(op2);\
            if (!rangecheck(y1)) { vm_commit(); janet_panicf("value %v out of range for " msg, op1); }\
            if (!janet_checkintrange(y2)) { vm_commit(); janet_panicf("rhs must be valid 32-bit signed integer, got %f", op2); }\
            type1 x1 = (type1) y1;\
            int32_t x2 = (int32_t) y2;\
            stack[A] = janet_wrap_number((type1) (x1 op x2));\
            vm_pcnext();\
        } else {\
            vm_commit();\
            Janet a = janet_binop_call(#op, "r" #op, op1, op2);\
            stack = fiber->data + fiber->frame;\
            stack[A] = a;\
            vm_checkgc_pcnext();\
        }\
    }
#define vm_bitop(op) _vm_bitop(op, int32_t, janet_checkintrange, "32-bit signed integers")
#define vm_bitopu(op) _vm_bitop(op, uint32_t, janet_checkuintrange, "32-bit unsigned integers")
#define vm_compop(op) \
    {\
        Janet op1 = stack[B];\
        Janet op2 = stack[C];\
        if (JANET_LIKELY(janet_checktype(op1, JANET_NUMBER) && janet_checktype(op2, JANET_NUMBER))) {\
            double x1 = janet_unwrap_number(op1);\
            double x2 = janet_unwrap_number(op2);\
            stack[A] = janet_wrap_boolean(x1 op x2);\
            vm_pcnext();\
        } else {\
            vm_commit();\
            Janet a = janet_wrap_boolean(janet_compare(op1, op2) op 0);\
            stack = fiber->data + fiber->frame;\
            stack[A] = a;\
            vm_checkgc_pcnext();\
        }\
    }
#define vm_compop_imm(op) \
    {\
        Janet op1 = stack[B];\
        if (janet_checktype(op1, JANET_NUMBER)) {\
            double x1 = janet_unwrap_number(op1);\
            double x2 = (double) CS; \
            stack[A] = janet_wrap_boolean(x1 op x2);\
            vm_pcnext();\
        } else {\
            vm_commit();\
            Janet a = janet_wrap_boolean(janet_compare(op1, janet_wrap_integer(CS)) op 0);\
            stack = fiber->data + fiber->frame;\
            stack[A] = a;\
            vm_checkgc_pcnext();\
        }\
    }

/* Trace a function call.
 * This is a macro to avoid stale argv if janet_eprintf resizes the stack
 */
#define vm_do_trace(func, argc, argv) do { \
    JanetFunction* _func = (func);\
    if (_func->def->name) {\
        janet_eprintf("trace (%S", _func->def->name);\
    } else {\
        janet_eprintf("trace (%p", janet_wrap_function(_func));\
    }\
    int32_t _argc = (argc);\
    for (int32_t i = 0; i < _argc; i++) {\
        janet_eprintf(" %p", (argv)[i]);\
    }\
    janet_eprintf(")\n");\
} while (0)

/* Invoke a method once we have looked it up */
static Janet janet_method_invoke(Janet method, int32_t argc, Janet *argv) {
    switch (janet_type(method)) {
        case JANET_CFUNCTION:
            return (janet_unwrap_cfunction(method))(argc, argv);
        case JANET_FUNCTION: {
            JanetFunction *fun = janet_unwrap_function(method);
            return janet_call(fun, argc, argv);
        }
        case JANET_ABSTRACT: {
            JanetAbstract abst = janet_unwrap_abstract(method);
            const JanetAbstractType *at = janet_abstract_type(abst);
            if (NULL != at->call) {
                return at->call(abst, argc, argv);
            }
        }
        /* fallthrough */
        case JANET_STRING:
        case JANET_BUFFER:
        case JANET_TABLE:
        case JANET_STRUCT:
        case JANET_ARRAY:
        case JANET_TUPLE: {
            if (argc != 1) {
                janet_panicf("%v called with %d arguments, possibly expected 1", method, argc);
            }
            return janet_in(method, argv[0]);
        }
        default: {
            if (argc != 1) {
                janet_panicf("%v called with %d arguments, possibly expected 1", method, argc);
            }
            return janet_in(argv[0], method);
        }
    }
}

/* Call a non function type from a JOP_CALL or JOP_TAILCALL instruction.
 * Assumes that the arguments are on the fiber stack. */
static Janet call_nonfn(JanetFiber *fiber, Janet callee) {
    int32_t argc = fiber->stacktop - fiber->stackstart;
    fiber->stacktop = fiber->stackstart;
    return janet_method_invoke(callee, argc, fiber->data + fiber->stacktop);
}

/* Method lookup could potentially handle tables specially... */
static Janet method_to_fun(Janet method, Janet obj) {
    return janet_get(obj, method);
}

/* Get a callable from a keyword method name and ensure that it is valid. */
static Janet resolve_method(Janet name, JanetFiber *fiber) {
    int32_t argc = fiber->stacktop - fiber->stackstart;
    if (argc < 1) janet_panicf("method call (%v) takes at least 1 argument, got 0", name);
    Janet callee = method_to_fun(name, fiber->data[fiber->stackstart]);
    if (janet_checktype(callee, JANET_NIL))
        janet_panicf("unknown method %v invoked on %v", name, fiber->data[fiber->stackstart]);
    return callee;
}

/* Lookup method on value x */
static Janet janet_method_lookup(Janet x, const char *name) {
    return method_to_fun(janet_ckeywordv(name), x);
}

static Janet janet_unary_call(const char *method, Janet arg) {
    Janet m = janet_method_lookup(arg, method);
    if (janet_checktype(m, JANET_NIL)) {
        janet_panicf("could not find method :%s for %v", method, arg);
    } else {
        Janet argv[1] = { arg };
        return janet_method_invoke(m, 1, argv);
    }
}

/* Call a method first on the righthand side, and then on the left hand side with a prefix */
static Janet janet_binop_call(const char *lmethod, const char *rmethod, Janet lhs, Janet rhs) {
    Janet lm = janet_method_lookup(lhs, lmethod);
    if (janet_checktype(lm, JANET_NIL)) {
        /* Invert order for rmethod */
        Janet lr = janet_method_lookup(rhs, rmethod);
        Janet argv[2] = { rhs, lhs };
        if (janet_checktype(lr, JANET_NIL)) {
            janet_panicf("could not find method :%s for %v or :%s for %v",
                         lmethod, lhs,
                         rmethod, rhs);
        }
        return janet_method_invoke(lr, 2, argv);
    } else {
        Janet argv[2] = { lhs, rhs };
        return janet_method_invoke(lm, 2, argv);
    }
}

/* Forward declaration */
static JanetSignal janet_check_can_resume(JanetFiber *fiber, Janet *out, int is_cancel);
/* P3.6: walk the resume chain for shared-stack fibers parked mid-C
 * (child links verified intact — a stale link means that ancestor
 * already unwound normally). Self excluded (the suspender abandons
 * cleanly and re-primes). Bounded. */
static int janet_fiber_chain_has_shared(JanetFiber *fiber);static JanetSignal janet_continue_no_check(JanetFiber *fiber, Janet in, Janet *out);
static void janet_suspend_swap(JanetFiber *fiber, JanetSignal sig, Janet payload);

/* PGO type-profile collector (Phase 1). Records observed param/return
 * types per function while janet_vm.profile_table is non-NULL. Keyed by
 * JanetFuncDef* identity; each entry holds a strong function reference so
 * profiled defs survive. Masks are uint32 bit sets over JanetType (16
 * types fit). Zero cost when off: callers guard on the NULL table. */
#define JANET_PROFILE_MAXPARAMS 8
#define JANET_PROFILE_ELEMSAMPLE 8
/* Phase 4B: distinct observed VALUES per scalar atom param slot. A
 * slot observing more distinct values than this is marked open (the
 * consumer must refuse closed-set reasoning for it), never a huge
 * set. */
#define JANET_PROFILE_VALSAMPLE 8
#define JANET_PROFILE_SIGSAMPLE 8
/* Stream D step A: distinct observed table/struct KEY SETS per param
 * slot ("shapes"). ttfx-style record passing means most unprovable
 * params are tables carrying a fixed key set -- a struct wearing a
 * hash table's clothing. Recording the observed key sets lets the
 * build emit a real C struct with offset field access instead of
 * janet_table_get hash probes. Same discipline as VALSAMPLE above: a
 * slot observing more distinct shapes than this is marked OPEN and the
 * consumer must keep it a boxed JanetTable* -- exactly the correct
 * outcome for a genuine dictionary (dynamic keys), so real
 * dictionaries disqualify themselves automatically. */
#define JANET_PROFILE_SHAPESAMPLE 4
/* Guard against pathological records: a key set wider than this is
 * treated as open rather than flattened (struct layout churn stops
 * being worth it, and it signals the value is used as a map). ttfx's
 * widest real record (canvas) is ~18 keys. */
#define JANET_PROFILE_SHAPEMAXKEYS 24

/* Direct-mapped entry cache (auto-PGO): real programs alternate
 * between functions, so a last-entry cache thrashes (16x slowdown
 * observed on alternating calls); 16 lines keyed by def-pointer hash
 * hold the hot working set instead. Hits cost ~10 ALU ops (no
 * hashing); misses take the full path and refill (flushing the
 * evicted line first). Flush-all runs at every site that swaps or
 * reads the table (stop, reset, rows), so counts are never lost. The
 * non-moving collector keeps cached pointers valid while their table
 * is current, which the pt-identity check enforces. */
#define JANET_PROFILE_CACHELINES 64
typedef struct {
    JanetTable *pt;
    JanetFuncDef *def;
    JanetTable *e;
    int64_t calls;
    uint32_t rets;
    uint32_t tick;
    int live;
    JanetArray *pa;
    JanetArray *ea;
    JanetArray *mn;
    JanetArray *mx;
    JanetArray *va;
    JanetArray *sh;
    Janet sig;
} JanetProfileCache;
/* Phase C: the fast-path cache is per-thread. Worker VMs share this
 * translation unit across OS threads; a single global would let two
 * workers fill/use it concurrently (params landing in the wrong
 * function's arrays). Thread-local keeps every VM disjoint. Static
 * storage is zero-initialized. */
static JANET_THREAD_LOCAL JanetProfileCache janet_profile_cache[JANET_PROFILE_CACHELINES];

static uint32_t janet_profile_line(JanetFuncDef *def) {
    uintptr_t h = (uintptr_t) def;
    h ^= h >> 12;
    return (h >> 4) % JANET_PROFILE_CACHELINES;
}

static void janet_profile_flush_line(uint32_t ln) {
    JanetProfileCache *c = &janet_profile_cache[ln];
    if (c->live) {
        janet_table_put(c->e, janet_ckeywordv("calls"),
                        janet_wrap_integer(c->calls));
        janet_table_put(c->e, janet_ckeywordv("rets"),
                        janet_wrap_integer((int64_t) c->rets));
        c->live = 0;
    }
}

static void janet_profile_flush(void) {
    uint32_t ln;
    for (ln = 0; ln < JANET_PROFILE_CACHELINES; ln++)
        janet_profile_flush_line(ln);
}

static void janet_profile_cache_fill(JanetTable *pt, JanetFuncDef *def, JanetTable *e) {
    /* Miss path flushes first (per the collector contract): the live
     * entry's C-side calls/rets would otherwise be lost on eviction. */
    uint32_t ln = janet_profile_line(def);
    JanetProfileCache *c = &janet_profile_cache[ln];
    janet_profile_flush_line(ln);
    c->pt = pt;
    c->def = def;
    c->e = e;
    c->calls =
        janet_unwrap_integer(janet_table_get(e, janet_ckeywordv("calls")));
    c->rets =
        (uint32_t) janet_unwrap_integer(janet_table_get(e, janet_ckeywordv("rets")));
    c->pa = janet_unwrap_array(janet_table_get(e, janet_ckeywordv("params")));
    c->ea = janet_unwrap_array(janet_table_get(e, janet_ckeywordv("elems")));
    c->mn = janet_unwrap_array(janet_table_get(e, janet_ckeywordv("mins")));
    c->mx = janet_unwrap_array(janet_table_get(e, janet_ckeywordv("maxs")));
    c->va = janet_unwrap_array(janet_table_get(e, janet_ckeywordv("vals")));
    c->sh = janet_unwrap_array(janet_table_get(e, janet_ckeywordv("shapes")));
    c->sig = janet_table_get(e, janet_ckeywordv("signatures"));
    c->live = 1;
}

static JANET_THREAD_LOCAL unsigned long long janet_profile_hits;
static JANET_THREAD_LOCAL unsigned long long janet_profile_misses;

static JanetProfileCache *janet_profile_hit(JanetFunction *func) {
    JanetProfileCache *c = &janet_profile_cache[janet_profile_line(func->def)];
    if (c->live && c->pt == janet_vm.profile_table && c->def == func->def) {
        janet_profile_hits++;
        return c;
    }
    janet_profile_misses++;
    return NULL;
}

static JanetTable *janet_profile_entry(JanetFunction *func) {
    JanetTable *pt = janet_vm.profile_table;
    Janet key = janet_wrap_pointer(func->def);
    Janet v = janet_table_get(pt, key);
    if (janet_checktype(v, JANET_NIL)) {
        int i;
        JanetTable *e = janet_table(4);
        JanetArray *pa;
        Janet eroot;
        /* Root until inserted: table_put can trigger a collection that
         * would otherwise sweep the still-floating entry. */
        eroot = janet_wrap_table(e);
        janet_gcroot(eroot);
        pa = janet_array(JANET_PROFILE_MAXPARAMS);
        janet_gcroot(janet_wrap_array(pa));
        /* P3.4 bookkeeping puts: the profiler's entry table is
         * observation bookkeeping, not value-graph ownership — the
         * same class as pt itself (marshal memo, threaded registry).
         * The "func" put especially crosses heaps by design: profiling
         * a call made by a NON-strict fiber (e.g. protect's fresh
         * fiber inside a strict proc) stores a child-heap (isolated)
         * function into a caller-heap entry table; the plain put's
         * barrier panicked there, killing strict procs in AOT
         * binaries (auto-PGO instruments every call). put_raw keeps
         * generational-dirty marking and skips the export panic. */
        janet_table_put_raw(e, janet_ckeywordv("func"), janet_wrap_function(func));
        janet_table_put_raw(e, janet_ckeywordv("calls"), janet_wrap_integer(0));
        for (i = 0; i < JANET_PROFILE_MAXPARAMS; i++)
            janet_array_push(pa, janet_wrap_integer(0));
        janet_table_put_raw(e, janet_ckeywordv("params"), janet_wrap_array(pa));
        {
            /* Parallel element-type masks: for array params, the OR of
             * the first elements' types (heuristic sample, see call
             * path). Zero for non-arrays. Evidence for SoA-kernel
             * shaping, not proof. */
            JanetArray *ea = janet_array(JANET_PROFILE_ELEMSAMPLE);
            janet_gcroot(janet_wrap_array(ea));
            for (i = 0; i < JANET_PROFILE_ELEMSAMPLE; i++)
                janet_array_push(ea, janet_wrap_integer(0));
            janet_table_put_raw(e, janet_ckeywordv("elems"), janet_wrap_array(ea));
            janet_gcunroot(janet_wrap_array(ea));
        }
        {
            /* Parallel min/max for number params (PGO range evidence:
             * unanimous min >= 0 seeds nonneg int-division shaping).
             * +inf/-inf sentinels mean unobserved (dumped as nil). */
            JanetArray *mn = janet_array(JANET_PROFILE_MAXPARAMS);
            JanetArray *mx = janet_array(JANET_PROFILE_MAXPARAMS);
            janet_gcroot(janet_wrap_array(mn));
            janet_gcroot(janet_wrap_array(mx));
            for (i = 0; i < JANET_PROFILE_MAXPARAMS; i++) {
                janet_array_push(mn, janet_wrap_number(INFINITY));
                janet_array_push(mx, janet_wrap_number(-INFINITY));
            }
            janet_table_put_raw(e, janet_ckeywordv("mins"), janet_wrap_array(mn));
            janet_table_put_raw(e, janet_ckeywordv("maxs"), janet_wrap_array(mx));
            janet_gcunroot(janet_wrap_array(mn));
            janet_gcunroot(janet_wrap_array(mx));
        }
        {
            /* Phase 4B: closed-value-set evidence for scalar atom
             * (string/keyword/symbol) param slots. Per slot: nil =
             * unobserved, array = observed distinct values (bounded by
             * JANET_PROFILE_VALSAMPLE), true = open (too many distinct
             * values observed -- closed-set reasoning must refuse).
             * Booleans can never be a recorded atom value, so `true`
             * is an unambiguous marker. Values stay alive through the
             * entry table exactly like the mask arrays. */
            JanetArray *va = janet_array(JANET_PROFILE_MAXPARAMS);
            janet_gcroot(janet_wrap_array(va));
            for (i = 0; i < JANET_PROFILE_MAXPARAMS; i++)
                janet_array_push(va, janet_wrap_nil());
            janet_table_put_raw(e, janet_ckeywordv("vals"), janet_wrap_array(va));
            janet_gcunroot(janet_wrap_array(va));
        }
        {
            /* Stream D step A: observed table/struct KEY SETS per slot.
             * Per slot: nil = unobserved, array of key-arrays = the
             * distinct observed shapes (bounded by
             * JANET_PROFILE_SHAPESAMPLE), true = open (too many
             * distinct shapes, or a key set wider than
             * JANET_PROFILE_SHAPEMAXKEYS, or a non-keyword key --
             * consumers must keep the slot a boxed table). Booleans can
             * never be a recorded shape, so `true` is unambiguous, the
             * same marker discipline as :vals above. */
            JanetArray *sh = janet_array(JANET_PROFILE_MAXPARAMS);
            janet_gcroot(janet_wrap_array(sh));
            for (i = 0; i < JANET_PROFILE_MAXPARAMS; i++)
                janet_array_push(sh, janet_wrap_nil());
            janet_table_put_raw(e, janet_ckeywordv("shapes"), janet_wrap_array(sh));
            janet_gcunroot(janet_wrap_array(sh));
        }
        janet_table_put_raw(e, janet_ckeywordv("rets"), janet_wrap_integer(0));
        /* Joint, full-arity tag vectors; true denotes OPEN. The entry
         * owns this array and every nested vector across GC. */
        {
            JanetArray *hist = janet_array(0);
            janet_gcroot(janet_wrap_array(hist));
            janet_table_put_raw(e, janet_ckeywordv("signatures"), janet_wrap_array(hist));
            janet_gcunroot(janet_wrap_array(hist));
        }
        /* P3.4 bookkeeping put: pt is the profiler's identity map
         * (function-pointer keyed), the same class as the marshal
         * memo and threaded registry. Under strict procs the plain
         * put panicked (a child-allocated entry is isolated, pt is
         * VM-global shared) — killing every strict proc in AOT
         * binaries, which auto-PGO instruments. put_raw keeps
         * generational-dirty marking (minors stay sound) and skips
         * the barrier/export panic. */
        janet_table_put_raw(pt, key, janet_wrap_table(e));
        janet_gcunroot(janet_wrap_array(pa));
        janet_gcunroot(eroot);
        janet_profile_cache_fill(pt, func->def, e);
        return e;
    }
    janet_profile_cache_fill(pt, func->def, janet_unwrap_table(v));
    return janet_unwrap_table(v);
}

static void janet_profile_call(JanetFunction *func, const Janet *argv, int32_t argc) {
    int32_t n, i;
    JanetArray *pa, *ea, *mn, *mx, *va, *sh;
    JanetProfileCache *hit = janet_profile_hit(func);
    if (NULL != hit) {
        pa = hit->pa;
        ea = hit->ea;
        mn = hit->mn;
        mx = hit->mx;
        va = hit->va;
        sh = hit->sh;
        hit->calls++;
    } else {
        JanetTable *e = janet_profile_entry(func);
        (void) e;
        hit = janet_profile_hit(func);
        pa = hit->pa;
        ea = hit->ea;
        mn = hit->mn;
        mx = hit->mx;
        va = hit->va;
        sh = hit->sh;
        hit->calls++;
    }
    n = argc < JANET_PROFILE_MAXPARAMS ? argc : JANET_PROFILE_MAXPARAMS;
    if (!janet_checktype(hit->sig, JANET_BOOLEAN)) {
        if (argc < 0 || argc > JANET_PROFILE_MAXPARAMS) {
            hit->sig = janet_wrap_true();
            janet_table_put_raw(hit->e, janet_ckeywordv("signatures"), hit->sig);
        } else {
            JanetArray *hist = janet_unwrap_array(hit->sig);
            int32_t k, found = 0;
            for (k = 0; k < hist->count; k++) {
                JanetArray *entry = janet_unwrap_array(hist->data[k]);
                JanetArray *tags = janet_unwrap_array(entry->data[0]);
                int32_t j;
                if (tags->count != argc) continue;
                for (j = 0; j < argc; j++)
                    if ((uint32_t) janet_unwrap_integer(tags->data[j]) !=
                        (uint32_t) janet_type(argv[j])) break;
                if (j == argc) {
                    entry->data[1] = janet_wrap_integer(janet_unwrap_integer(entry->data[1]) + 1);
                    found = 1;
                    break;
                }
            }
            if (!found) {
                if (hist->count >= JANET_PROFILE_SIGSAMPLE) {
                    hit->sig = janet_wrap_true();
                    janet_table_put_raw(hit->e, janet_ckeywordv("signatures"), hit->sig);
                } else {
                    JanetArray *entry = janet_array(2);
                    JanetArray *tags;
                    janet_gcroot(janet_wrap_array(entry));
                    tags = janet_array(argc);
                    janet_gcroot(janet_wrap_array(tags));
                    for (k = 0; k < argc; k++)
                        janet_array_push(tags, janet_wrap_integer(janet_type(argv[k])));
                    janet_array_push(entry, janet_wrap_array(tags));
                    janet_array_push(entry, janet_wrap_integer(1));
                    janet_array_push(hist, janet_wrap_array(entry));
                    janet_gcunroot(janet_wrap_array(tags));
                    janet_gcunroot(janet_wrap_array(entry));
                }
            }
        }
    }
    /* Sample the expensive parts 1-in-8: type masks and call counts
     * stay exact every call (seeding unanimity must be exact), while
     * min/max ranges and element samples — heuristic evidence consumed
     * behind fail-loud guards — cost ~1/8th. The first 16 calls per
     * entry always record fully, so small tests and rare functions
     * keep exact evidence. */
    ++hit->tick;
    int full = (hit->calls < 16) || ((hit->tick & 7) == 0);
    for (i = 0; i < n; i++) {
        /* One type computation per arg (checktype would recompute). */
        uint32_t at = (uint32_t) janet_type(argv[i]);
        uint32_t m = (uint32_t) janet_unwrap_integer(pa->data[i]);
        m |= (1u << at);
        pa->data[i] = janet_wrap_integer((int64_t) m);
        if (full && at == (uint32_t) JANET_NUMBER) {
            double v = janet_unwrap_number(argv[i]);
            if (v < janet_unwrap_number(mn->data[i])) mn->data[i] = janet_wrap_number(v);
            if (v > janet_unwrap_number(mx->data[i])) mx->data[i] = janet_wrap_number(v);
        }
        if (full && at == (uint32_t) JANET_ARRAY) {
            JanetArray *av = janet_unwrap_array(argv[i]);
            int32_t k, nk = av->count < JANET_PROFILE_ELEMSAMPLE ? av->count : JANET_PROFILE_ELEMSAMPLE;
            uint32_t em = (uint32_t) janet_unwrap_integer(ea->data[i]);
            for (k = 0; k < nk; k++)
                em |= (1u << (uint32_t) janet_type(av->data[k]));
            ea->data[i] = janet_wrap_integer((int64_t) em);
        }
        if (full && (at == (uint32_t) JANET_STRING ||
                     at == (uint32_t) JANET_SYMBOL ||
                     at == (uint32_t) JANET_KEYWORD)) {
            /* Phase 4B: sample the concrete VALUE for scalar atom
             * params (same 16-exact-then-1-in-8 discipline as the
             * element/min-max evidence). janet_equals is content
             * equality for strings and pointer equality for interned
             * keywords/symbols -- exactly Janet's own equality. The
             * freshly built vs array is rooted across its pushes and
             * becomes reachable through the entry table on store. */
            Janet slotv = va->data[i];
            if (!janet_checktype(slotv, JANET_BOOLEAN)) {
                JanetArray *vs = janet_checktype(slotv, JANET_ARRAY)
                    ? janet_unwrap_array(slotv) : NULL;
                int32_t k, found = 0;
                if (NULL != vs)
                    for (k = 0; k < vs->count; k++)
                        if (janet_equals(vs->data[k], argv[i])) {
                            found = 1;
                            break;
                        }
                if (!found) {
                    if (NULL == vs) {
                        vs = janet_array(JANET_PROFILE_VALSAMPLE);
                        janet_gcroot(janet_wrap_array(vs));
                        janet_array_push(vs, argv[i]);
                        va->data[i] = janet_wrap_array(vs);
                        janet_gcunroot(janet_wrap_array(vs));
                    } else if (vs->count < JANET_PROFILE_VALSAMPLE) {
                        janet_array_push(vs, argv[i]);
                    } else {
                        /* Over the cap: the slot is open, not closed.
                         * Drop the set and mark it; consumers must
                         * fall back to boxed, never emit a huge set. */
                        va->data[i] = janet_wrap_true();
                    }
                }
            }
        }
        if (full && NULL != sh && i < sh->count &&
                (at == (uint32_t) JANET_TABLE || at == (uint32_t) JANET_STRUCT)) {
            /* Stream D step A: record this table's KEY SET. Only
             * all-keyword key sets are recordable: a struct field needs
             * a stable C identifier, and a non-keyword key (number,
             * string, computed value) is the signature of a real
             * dictionary, which must stay boxed. Any such key marks the
             * slot open immediately rather than recording a partial
             * shape. */
            Janet slotsh = sh->data[i];
            if (!janet_checktype(slotsh, JANET_BOOLEAN)) {
                const JanetKV *kvs = NULL;
                int32_t cap = 0, cnt = 0;
                if (at == (uint32_t) JANET_TABLE) {
                    JanetTable *t = janet_unwrap_table(argv[i]);
                    kvs = t->data;
                    cap = t->capacity;
                    cnt = t->count;
                } else {
                    const JanetKV *st = janet_unwrap_struct(argv[i]);
                    kvs = st;
                    cap = janet_struct_capacity(st);
                    cnt = janet_struct_length(st);
                }
                if (cnt > JANET_PROFILE_SHAPEMAXKEYS) {
                    sh->data[i] = janet_wrap_true();
                } else {
                    /* Collect keys in the table's own slot order, then
                     * compare order-insensitively below: two tables with
                     * the same keys can hash them into different slots,
                     * so raw order is not canonical. */
                    Janet keybuf[JANET_PROFILE_SHAPEMAXKEYS];
                    int32_t nk = 0, j;
                    int bad = 0;
                    for (j = 0; j < cap && nk <= JANET_PROFILE_SHAPEMAXKEYS; j++) {
                        if (janet_checktype(kvs[j].key, JANET_NIL)) continue;
                        if (!janet_checktype(kvs[j].key, JANET_KEYWORD)) { bad = 1; break; }
                        if (nk == JANET_PROFILE_SHAPEMAXKEYS) { bad = 1; break; }
                        keybuf[nk++] = kvs[j].key;
                    }
                    if (bad) {
                        sh->data[i] = janet_wrap_true();
                    } else {
                        JanetArray *shapes = janet_checktype(slotsh, JANET_ARRAY)
                            ? janet_unwrap_array(slotsh) : NULL;
                        int found = 0;
                        int32_t si;
                        if (NULL != shapes) {
                            for (si = 0; si < shapes->count && !found; si++) {
                                JanetArray *cand = janet_unwrap_array(shapes->data[si]);
                                if (cand->count != nk) continue;
                                /* order-insensitive set compare */
                                int32_t a2, b2, matched = 0;
                                for (a2 = 0; a2 < nk; a2++) {
                                    for (b2 = 0; b2 < cand->count; b2++)
                                        if (janet_equals(keybuf[a2], cand->data[b2])) { matched++; break; }
                                }
                                if (matched == nk) found = 1;
                            }
                        }
                        if (!found) {
                            if (NULL != shapes && shapes->count >= JANET_PROFILE_SHAPESAMPLE) {
                                sh->data[i] = janet_wrap_true();
                            } else {
                                JanetArray *ks = janet_array(nk);
                                janet_gcroot(janet_wrap_array(ks));
                                for (j = 0; j < nk; j++) janet_array_push(ks, keybuf[j]);
                                if (NULL == shapes) {
                                    shapes = janet_array(JANET_PROFILE_SHAPESAMPLE);
                                    janet_gcroot(janet_wrap_array(shapes));
                                    janet_array_push(shapes, janet_wrap_array(ks));
                                    sh->data[i] = janet_wrap_array(shapes);
                                    janet_gcunroot(janet_wrap_array(shapes));
                                } else {
                                    janet_array_push(shapes, janet_wrap_array(ks));
                                }
                                janet_gcunroot(janet_wrap_array(ks));
                            }
                        }
                    }
                }
            }
        }
    }
}

/* Phase C: record a task-root invocation that bypasses CALL opcodes
 * (worker fibers are primed directly via janet_fiber, never through
 * JOP_CALL, so without this their rows show calls=0 and no params).
 * Mirrors the opcode hook exactly. Fiber-root tasks keep return-only
 * records (a resume is not a call). */
void janet_profile_task_call(JanetFunction *func, const Janet *argv, int32_t argc) {
    if (NULL != janet_vm.profile_table)
        janet_profile_call(func, argv, argc);
}

static void janet_profile_return(JanetFunction *func, Janet retval) {
    JanetProfileCache *hit = janet_profile_hit(func);
    if (NULL == hit) {
        janet_profile_entry(func);
        hit = janet_profile_hit(func);
    }
    hit->rets |= (1u << (uint32_t) janet_type(retval));
}

void janet_profile_start(void) {
    janet_profile_flush();
    janet_vm.profile_table = janet_table(0);
    memset(janet_profile_cache, 0, sizeof(janet_profile_cache));
}

static void janet_profile_maybe_stats(void) {
    if (NULL != getenv("JANET_PROFILE_STATS")) {
        fprintf(stderr, "profile cache: %llu hits %llu misses (%.1f%% hit)\n",
                janet_profile_hits, janet_profile_misses,
                100.0 * janet_profile_hits /
                    (janet_profile_hits + janet_profile_misses + 1e-9));
        janet_profile_hits = janet_profile_misses = 0;
    }
}

Janet janet_profile_stop(void) {
    JanetTable *pt = janet_vm.profile_table;
    janet_profile_flush();
    janet_vm.profile_table = NULL;
    memset(janet_profile_cache, 0, sizeof(janet_profile_cache));
    janet_profile_maybe_stats();
    return NULL == pt ? janet_wrap_nil() : janet_wrap_table(pt);
}

void janet_profile_reset(void) {
    if (NULL != janet_vm.profile_table) {
        janet_profile_flush();
        janet_vm.profile_table = janet_table(0);
        memset(janet_profile_cache, 0, sizeof(janet_profile_cache));
    }
}

/* Serialize the profile as Janet data: @[[source line name]
 * {:calls n :params [[:number :string] ...] :rets [...] :mins [...]
 * :maxs [...]}], with one entry per observed function. Type sets
 * decode to keyword lists; mins/maxs carry observed number ranges
 * (nil where unobserved). */
static Janet janet_profile_mask_types(uint32_t m) {
    JanetArray *a = janet_array(4);
    int t;
    for (t = 0; t < 16; t++) {
        if (m & (1u << (uint32_t) t)) {
            janet_array_push(a, janet_ckeywordv(janet_type_names[t]));
        }
    }
    return janet_wrap_array(a);
}

/* Build the dump row array for this VM's live profile table (empty
 * when inactive). Caller roots the result across further allocation.
 * Shared by janet_profile_dump and cross-thread harvest (Phase C). */
JanetArray *janet_profile_rows(void) {
    JanetTable *pt = janet_vm.profile_table;
    JanetArray *out = janet_array(0);
    janet_profile_flush();
    if (NULL == pt) return out;
    janet_gcroot(janet_wrap_array(out));
    {
        const JanetKV *kvs;
        int32_t cap, count, i;
        janet_dictionary_view(janet_wrap_table(pt), &kvs, &count, &cap);
        for (i = 0; i < cap; i++) {
            const JanetKV *kv = kvs + i;
            JanetTable *e;
            JanetFunction *func;
            JanetFuncDef *def;
            JanetArray *row, *params;
            JanetArray *pa;
            int32_t j, n;
            if (janet_checktype(kv->key, JANET_NIL)) continue;
            e = janet_unwrap_table(kv->value);
            func = janet_unwrap_function(janet_table_get(e, janet_ckeywordv("func")));
            def = func->def;
            row = janet_array(2);
            janet_gcroot(janet_wrap_array(row));
            {
                JanetArray *id = janet_array(3);
                int32_t defline = -1;
                janet_gcroot(janet_wrap_array(id));
                janet_array_push(id, NULL == def->source
                    ? janet_wrap_nil() : janet_wrap_string(def->source));
                if (NULL != def->sourcemap && def->bytecode_length > 0)
                    defline = def->sourcemap[0].line;
                janet_array_push(id, janet_wrap_integer((int64_t) defline));
                janet_array_push(id, NULL == def->name
                    ? janet_wrap_nil() : janet_wrap_string(def->name));
                janet_array_push(row, janet_wrap_array(id));
                janet_gcunroot(janet_wrap_array(id));
            }
            {
                JanetTable *info = janet_table(3);
                int64_t calls = janet_unwrap_integer(janet_table_get(e, janet_ckeywordv("calls")));
                uint32_t rm = (uint32_t) janet_unwrap_integer(janet_table_get(e, janet_ckeywordv("rets")));
                janet_gcroot(janet_wrap_table(info));
                janet_table_put(info, janet_ckeywordv("calls"), janet_wrap_integer(calls));
                {
                    Janet sig = janet_table_get(e, janet_ckeywordv("signatures"));
                    if (janet_checktype(sig, JANET_BOOLEAN)) {
                        janet_table_put(info, janet_ckeywordv("signatures"), janet_ckeywordv("open"));
                    } else {
                        JanetArray *hist = janet_unwrap_array(sig);
                        JanetArray *decoded = janet_array(hist->count);
                        int32_t si;
                        janet_gcroot(janet_wrap_array(decoded));
                        for (si = 0; si < hist->count; si++) {
                            JanetArray *raw = janet_unwrap_array(hist->data[si]);
                            JanetArray *tags = janet_unwrap_array(raw->data[0]);
                            JanetArray *types = janet_array(tags->count);
                            JanetArray *pair;
                            int32_t ti;
                            janet_gcroot(janet_wrap_array(types));
                            pair = janet_array(2);
                            janet_gcroot(janet_wrap_array(pair));
                            for (ti = 0; ti < tags->count; ti++)
                                janet_array_push(types, janet_ckeywordv(
                                    janet_type_names[janet_unwrap_integer(tags->data[ti])]));
                            janet_array_push(pair, janet_wrap_array(types));
                            janet_array_push(pair, raw->data[1]);
                            janet_array_push(decoded, janet_wrap_array(pair));
                            janet_gcunroot(janet_wrap_array(pair));
                            janet_gcunroot(janet_wrap_array(types));
                        }
                        janet_table_put(info, janet_ckeywordv("signatures"), janet_wrap_array(decoded));
                        janet_gcunroot(janet_wrap_array(decoded));
                    }
                }
                params = janet_array(0);
                janet_gcroot(janet_wrap_array(params));
                pa = janet_unwrap_array(janet_table_get(e, janet_ckeywordv("params")));
                n = pa->count;
                for (j = 0; j < n; j++) {
                    uint32_t m = (uint32_t) janet_unwrap_integer(pa->data[j]);
                    if (m) janet_array_push(params, janet_profile_mask_types(m));
                }
                janet_table_put(info, janet_ckeywordv("params"), janet_wrap_array(params));
                janet_gcunroot(janet_wrap_array(params));
                {
                    JanetArray *elems = janet_array(0);
                    JanetArray *ea = janet_unwrap_array(janet_table_get(e, janet_ckeywordv("elems")));
                    n = ea->count;
                    for (j = 0; j < n; j++) {
                        uint32_t m = (uint32_t) janet_unwrap_integer(ea->data[j]);
                        if (m) janet_array_push(elems, janet_profile_mask_types(m));
                        else janet_array_push(elems, janet_wrap_nil());
                    }
                    janet_table_put(info, janet_ckeywordv("elems"), janet_wrap_array(elems));
                }
                {
                    /* Range evidence: observed min/max per param slot;
                     * unobserved slots serialize as nil (sentinels). */
                    JanetArray *mins = janet_array(0);
                    JanetArray *maxs = janet_array(0);
                    JanetArray *mn = janet_unwrap_array(janet_table_get(e, janet_ckeywordv("mins")));
                    JanetArray *mx = janet_unwrap_array(janet_table_get(e, janet_ckeywordv("maxs")));
                    n = mn->count;
                    for (j = 0; j < n; j++) {
                        double lo = janet_unwrap_number(mn->data[j]);
                        double hi = janet_unwrap_number(mx->data[j]);
                        if (lo <= hi) {
                            /* Non-finite bounds serialize as tagged
                             * strings: the printer emits bare inf/-inf,
                             * which parse reads back as a symbol and
                             * killed every profile re-load (unknown
                             * symbol inf). callgraph maps the strings
                             * back to float infinities. NaN never
                             * reaches here (lo <= hi fails -> nil). */
                            if (isinf(lo)) {
                                janet_array_push(mins,
                                    janet_cstringv(lo < 0 ? "-inf" : "inf"));
                            } else {
                                janet_array_push(mins, janet_wrap_number(lo));
                            }
                            if (isinf(hi)) {
                                janet_array_push(maxs,
                                    janet_cstringv(hi < 0 ? "-inf" : "inf"));
                            } else {
                                janet_array_push(maxs, janet_wrap_number(hi));
                            }
                        } else {
                            janet_array_push(mins, janet_wrap_nil());
                            janet_array_push(maxs, janet_wrap_nil());
                        }
                    }
                    janet_table_put(info, janet_ckeywordv("mins"), janet_wrap_array(mins));
                    janet_table_put(info, janet_ckeywordv("maxs"), janet_wrap_array(maxs));
                }
                {
                    /* Phase 4B value evidence: nil = unobserved, the
                     * value array itself (strings/keywords/symbols
                     * pretty-print cleanly), true = open (dumped as
                     * :open for readability). */
                    JanetArray *valsv = janet_array(0);
                    JanetArray *va = janet_unwrap_array(janet_table_get(e, janet_ckeywordv("vals")));
                    n = va->count;
                    for (j = 0; j < n; j++) {
                        Janet vv = va->data[j];
                        if (janet_checktype(vv, JANET_NIL))
                            janet_array_push(valsv, janet_wrap_nil());
                        else if (janet_checktype(vv, JANET_BOOLEAN))
                            janet_array_push(valsv, janet_ckeywordv("open"));
                        else
                            janet_array_push(valsv, vv);
                    }
                    janet_table_put(info, janet_ckeywordv("vals"), janet_wrap_array(valsv));
                }
                {
                    /* Stream D step A shape evidence: nil = unobserved,
                     * an array of key-arrays = the distinct observed
                     * key sets, :open = too many shapes / too wide /
                     * non-keyword keys (consumer must keep it boxed).
                     * Same dump discipline as :vals above. */
                    JanetArray *shv = janet_array(0);
                    JanetArray *sh = janet_unwrap_array(janet_table_get(e, janet_ckeywordv("shapes")));
                    n = sh->count;
                    for (j = 0; j < n; j++) {
                        Janet vv = sh->data[j];
                        if (janet_checktype(vv, JANET_NIL))
                            janet_array_push(shv, janet_wrap_nil());
                        else if (janet_checktype(vv, JANET_BOOLEAN))
                            janet_array_push(shv, janet_ckeywordv("open"));
                        else
                            janet_array_push(shv, vv);
                    }
                    janet_table_put(info, janet_ckeywordv("shapes"), janet_wrap_array(shv));
                }
                janet_table_put(info, janet_ckeywordv("rets"), janet_profile_mask_types(rm));
                janet_array_push(row, janet_wrap_table(info));
                janet_gcunroot(janet_wrap_table(info));
            }
            janet_array_push(out, janet_wrap_array(row));
            janet_gcunroot(janet_wrap_array(row));
        }
    }
    janet_gcunroot(janet_wrap_array(out));
    return out;
}

void janet_profile_dump(const char *path) {
    JanetArray *out;
    JanetBuffer *buf;
    FILE *f;
    if (NULL == janet_vm.profile_table) janet_panic("profiling not active");
    out = janet_profile_rows();
    janet_gcroot(janet_wrap_array(out));
    buf = janet_buffer(0);
    janet_gcroot(janet_wrap_buffer(buf));
    janet_pretty(buf, 8, JANET_PRETTY_NOTRUNC, janet_wrap_array(out));
    f = fopen(path, "wb");
    if (NULL == f) janet_panicf("could not open profile output %s", path);
    fwrite(buf->data, 1, (size_t) buf->count, f);
    fputc('\n', f);
    fclose(f);
    janet_gcunroot(janet_wrap_buffer(buf));
    janet_gcunroot(janet_wrap_array(out));
}

/* Interpreter main loop */
static JanetSignal run_vm(JanetFiber *fiber, Janet in) {
#ifdef JANET_NATIVE_EXECUTORS
    /* New resume: invalidate cached whole-definition validation verdicts.
     * Any change to a definition made between resumes (debugger
     * breakpoints, raw C edits while parked) is re-detected by the full
     * exact compare at this resume's first native region entry. */
    janet_vm.native_epoch++;
#endif

    /* opcode -> label lookup if using clang/GCC */
#ifdef JANET_USE_COMPUTED_GOTOS
    static void *op_lookup[255] = {
        &&label_JOP_NOOP,
        &&label_JOP_ERROR,
        &&label_JOP_TYPECHECK,
        &&label_JOP_RETURN,
        &&label_JOP_RETURN_NIL,
        &&label_JOP_ADD_IMMEDIATE,
        &&label_JOP_ADD,
        &&label_JOP_SUBTRACT_IMMEDIATE,
        &&label_JOP_SUBTRACT,
        &&label_JOP_MULTIPLY_IMMEDIATE,
        &&label_JOP_MULTIPLY,
        &&label_JOP_DIVIDE_IMMEDIATE,
        &&label_JOP_DIVIDE,
        &&label_JOP_DIVIDE_FLOOR,
        &&label_JOP_MODULO,
        &&label_JOP_REMAINDER,
        &&label_JOP_BAND,
        &&label_JOP_BOR,
        &&label_JOP_BXOR,
        &&label_JOP_BNOT,
        &&label_JOP_SHIFT_LEFT,
        &&label_JOP_SHIFT_LEFT_IMMEDIATE,
        &&label_JOP_SHIFT_RIGHT,
        &&label_JOP_SHIFT_RIGHT_IMMEDIATE,
        &&label_JOP_SHIFT_RIGHT_UNSIGNED,
        &&label_JOP_SHIFT_RIGHT_UNSIGNED_IMMEDIATE,
        &&label_JOP_MOVE_FAR,
        &&label_JOP_MOVE_NEAR,
        &&label_JOP_JUMP,
        &&label_JOP_JUMP_IF,
        &&label_JOP_JUMP_IF_NOT,
        &&label_JOP_JUMP_IF_NIL,
        &&label_JOP_JUMP_IF_NOT_NIL,
        &&label_JOP_GREATER_THAN,
        &&label_JOP_GREATER_THAN_IMMEDIATE,
        &&label_JOP_LESS_THAN,
        &&label_JOP_LESS_THAN_IMMEDIATE,
        &&label_JOP_EQUALS,
        &&label_JOP_EQUALS_IMMEDIATE,
        &&label_JOP_COMPARE,
        &&label_JOP_LOAD_NIL,
        &&label_JOP_LOAD_TRUE,
        &&label_JOP_LOAD_FALSE,
        &&label_JOP_LOAD_INTEGER,
        &&label_JOP_LOAD_CONSTANT,
        &&label_JOP_LOAD_UPVALUE,
        &&label_JOP_LOAD_SELF,
        &&label_JOP_SET_UPVALUE,
        &&label_JOP_CLOSURE,
        &&label_JOP_PUSH,
        &&label_JOP_PUSH_2,
        &&label_JOP_PUSH_3,
        &&label_JOP_PUSH_ARRAY,
        &&label_JOP_CALL,
        &&label_JOP_TAILCALL,
        &&label_JOP_RESUME,
        &&label_JOP_SIGNAL,
        &&label_JOP_PROPAGATE,
        &&label_JOP_IN,
        &&label_JOP_GET,
        &&label_JOP_PUT,
        &&label_JOP_GET_INDEX,
        &&label_JOP_PUT_INDEX,
        &&label_JOP_LENGTH,
        &&label_JOP_MAKE_ARRAY,
        &&label_JOP_MAKE_BUFFER,
        &&label_JOP_MAKE_STRING,
        &&label_JOP_MAKE_STRUCT,
        &&label_JOP_MAKE_TABLE,
        &&label_JOP_MAKE_TUPLE,
        &&label_JOP_MAKE_BRACKET_TUPLE,
        &&label_JOP_GREATER_THAN_EQUAL,
        &&label_JOP_LESS_THAN_EQUAL,
        &&label_JOP_NEXT,
        &&label_JOP_NOT_EQUALS,
        &&label_JOP_NOT_EQUALS_IMMEDIATE,
        &&label_JOP_CANCEL,
        &&label_JOP_IN_IMMEDIATE,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op,
        &&label_unknown_op
    };
#endif

    /* Interpreter state */
    register Janet *stack;
    register uint32_t *pc;
    register JanetFunction *func;

    /* NOTE: Flag smuggling! We "smuggle" the fiber sigal in the gc.flags field instead of the usual flags field */
    if (fiber->flags & JANET_FIBER_RESUME_SIGNAL) {
        /* Get the signal */
        JanetSignal sig = (fiber->gc.flags & JANET_FIBER_STATUS_MASK) >> JANET_FIBER_STATUS_OFFSET;
        /* Clear the signal */
        fiber->gc.flags &= ~JANET_FIBER_STATUS_MASK;
        fiber->flags &= ~JANET_FIBER_RESUME_SIGNAL;
        janet_vm.return_reg[0] = in;
        return sig;
    }

    vm_restore();

    if (fiber->flags & JANET_FIBER_DID_LONGJUMP) {
        if (janet_fiber_frame(fiber)->func == NULL) {
            /* Inside a c function */
            janet_fiber_popframe(fiber);
            vm_restore();
        }
        /* Check if we were at a tail call instruction. If so, do implicit return */
        if ((*pc & 0xFF) == JOP_TAILCALL) {
            /* Tail call resume */
            int entrance_frame = janet_stack_frame(stack)->flags & JANET_STACKFRAME_ENTRANCE;
            janet_fiber_popframe(fiber);
            if (entrance_frame) {
                fiber->flags &= ~JANET_FIBER_FLAG_MASK;
                vm_return(JANET_SIGNAL_OK, in);
            }
            vm_restore();
        }
    }

    if (!(fiber->flags & JANET_FIBER_RESUME_NO_USEVAL)) stack[A] = in;
    if (!(fiber->flags & JANET_FIBER_RESUME_NO_SKIP)) pc++;

    uint8_t first_opcode = *pc & ((fiber->flags & JANET_FIBER_BREAKPOINT) ? 0x7F : 0xFF);

#ifdef JANET_NATIVE_EXECUTORS
    int native_debug_bypass = fiber->flags & JANET_FIBER_BREAKPOINT;
#endif
    fiber->flags &= ~JANET_FIBER_FLAG_MASK;
#ifdef JANET_NATIVE_EXECUTORS
    if (!native_debug_bypass) {
        vm_native();
        first_opcode = *pc & 0xFF;
    }
#endif

    /* Main interpreter loop. Semantically is a switch on
     * (*pc & 0xFF) inside of an infinite loop. */
    VM_START();

    VM_DEFAULT();
    fiber->flags |= JANET_FIBER_BREAKPOINT | JANET_FIBER_RESUME_NO_USEVAL | JANET_FIBER_RESUME_NO_SKIP;
    vm_return(JANET_SIGNAL_DEBUG, janet_wrap_nil());

    VM_OP(JOP_NOOP)
    vm_pcnext();

    VM_OP(JOP_ERROR)
    vm_return(JANET_SIGNAL_ERROR, stack[D]);

    VM_OP(JOP_TYPECHECK)
    vm_assert_types(stack[A], E);
    vm_pcnext();

    VM_OP(JOP_RETURN) {
        Janet retval = stack[D];
        int entrance_frame = janet_stack_frame(stack)->flags & JANET_STACKFRAME_ENTRANCE;
        JanetFunction *retfunc = janet_stack_frame(stack)->func;
        janet_fiber_popframe(fiber);
        if (NULL != janet_vm.profile_table)
            janet_profile_return(retfunc, retval);
        if (entrance_frame) vm_return_no_restore(JANET_SIGNAL_OK, retval);
        vm_restore();
        stack[A] = retval;
        vm_checkgc_pcnext();
    }

    VM_OP(JOP_RETURN_NIL) {
        Janet retval = janet_wrap_nil();
        int entrance_frame = janet_stack_frame(stack)->flags & JANET_STACKFRAME_ENTRANCE;
        JanetFunction *retfunc = janet_stack_frame(stack)->func;
        janet_fiber_popframe(fiber);
        if (NULL != janet_vm.profile_table)
            janet_profile_return(retfunc, retval);
        if (entrance_frame) vm_return_no_restore(JANET_SIGNAL_OK, retval);
        vm_restore();
        stack[A] = retval;
        vm_checkgc_pcnext();
    }

    VM_OP(JOP_ADD_IMMEDIATE)
    vm_binop_immediate(+);

    VM_OP(JOP_ADD)
    vm_binop(+);

    VM_OP(JOP_SUBTRACT_IMMEDIATE)
    vm_binop_immediate(-);

    VM_OP(JOP_SUBTRACT)
    vm_binop(-);

    VM_OP(JOP_MULTIPLY_IMMEDIATE)
    vm_binop_immediate(*);

    VM_OP(JOP_MULTIPLY)
    vm_binop(*);

    VM_OP(JOP_DIVIDE_IMMEDIATE)
    vm_binop_immediate( /);

    VM_OP(JOP_DIVIDE)
    vm_binop( /);

    VM_OP(JOP_DIVIDE_FLOOR) {
        Janet op1 = stack[B];
        Janet op2 = stack[C];
        if (janet_checktype(op1, JANET_NUMBER) && janet_checktype(op2, JANET_NUMBER)) {
            double x1 = janet_unwrap_number(op1);
            double x2 = janet_unwrap_number(op2);
            stack[A] = janet_wrap_number(floor(x1 / x2));
            vm_pcnext();
        } else {
            vm_commit();
            Janet a = janet_binop_call("div", "rdiv", op1, op2);
            stack = fiber->data + fiber->frame;
            stack[A] = a;
            vm_checkgc_pcnext();
        }
    }

    VM_OP(JOP_MODULO) {
        Janet op1 = stack[B];
        Janet op2 = stack[C];
        if (janet_checktype(op1, JANET_NUMBER) && janet_checktype(op2, JANET_NUMBER)) {
            double x1 = janet_unwrap_number(op1);
            double x2 = janet_unwrap_number(op2);
            if (x2 == 0) {
                stack[A] = janet_wrap_number(x1);
            } else {
                double intres = x2 * floor(x1 / x2);
                stack[A] = janet_wrap_number(x1 - intres);
            }
            vm_pcnext();
        } else {
            vm_commit();
            Janet a = janet_binop_call("mod", "rmod", op1, op2);
            stack = fiber->data + fiber->frame;
            stack[A] = a;
            vm_checkgc_pcnext();
        }
    }

    VM_OP(JOP_REMAINDER) {
        Janet op1 = stack[B];
        Janet op2 = stack[C];
        if (janet_checktype(op1, JANET_NUMBER) && janet_checktype(op2, JANET_NUMBER)) {
            double x1 = janet_unwrap_number(op1);
            double x2 = janet_unwrap_number(op2);
            stack[A] = janet_wrap_number(fmod(x1, x2));
            vm_pcnext();
        } else {
            vm_commit();
            Janet a = janet_binop_call("%", "r%", op1, op2);
            stack = fiber->data + fiber->frame;
            stack[A] = a;
            vm_checkgc_pcnext();
        }
    }

    VM_OP(JOP_BAND)
    vm_bitop(&);

    VM_OP(JOP_BOR)
    vm_bitop( |);

    VM_OP(JOP_BXOR)
    vm_bitop(^);

    VM_OP(JOP_BNOT) {
        Janet op = stack[E];
        if (janet_checktype(op, JANET_NUMBER)) {
            stack[A] = janet_wrap_integer(~janet_unwrap_integer(op));
            vm_pcnext();
        } else {
            vm_commit();
            Janet a = janet_unary_call("~", op);
            stack = fiber->data + fiber->frame;
            stack[A] = a;
            vm_checkgc_pcnext();
        }
    }

    VM_OP(JOP_SHIFT_RIGHT_UNSIGNED)
    vm_bitopu( >>);

    VM_OP(JOP_SHIFT_RIGHT_UNSIGNED_IMMEDIATE)
    vm_bitopu_immediate( >>);

    VM_OP(JOP_SHIFT_RIGHT)
    vm_bitop( >>);

    VM_OP(JOP_SHIFT_RIGHT_IMMEDIATE)
    vm_bitop_immediate( >>);

    VM_OP(JOP_SHIFT_LEFT)
    vm_bitop( <<);

    VM_OP(JOP_SHIFT_LEFT_IMMEDIATE)
    vm_bitop_immediate( <<);

    VM_OP(JOP_MOVE_NEAR)
    stack[A] = stack[E];
    vm_pcnext();

    VM_OP(JOP_MOVE_FAR)
    stack[E] = stack[A];
    vm_pcnext();

    VM_OP(JOP_JUMP)
    vm_maybe_auto_suspend(DS <= 0);
    pc += DS;
    vm_next();

    VM_OP(JOP_JUMP_IF)
    if (janet_truthy(stack[A])) {
        vm_maybe_auto_suspend(ES <= 0);
        pc += ES;
    } else {
        pc++;
    }
    vm_next();

    VM_OP(JOP_JUMP_IF_NOT)
    if (janet_truthy(stack[A])) {
        pc++;
    } else {
        vm_maybe_auto_suspend(ES <= 0);
        pc += ES;
    }
    vm_next();

    VM_OP(JOP_JUMP_IF_NIL)
    if (janet_checktype(stack[A], JANET_NIL)) {
        vm_maybe_auto_suspend(ES <= 0);
        pc += ES;
    } else {
        pc++;
    }
    vm_next();

    VM_OP(JOP_JUMP_IF_NOT_NIL)
    if (janet_checktype(stack[A], JANET_NIL)) {
        pc++;
    } else {
        vm_maybe_auto_suspend(ES <= 0);
        pc += ES;
    }
    vm_next();

    VM_OP(JOP_LESS_THAN)
    vm_compop( <);

    VM_OP(JOP_LESS_THAN_EQUAL)
    vm_compop( <=);

    VM_OP(JOP_LESS_THAN_IMMEDIATE)
    vm_compop_imm( <);

    VM_OP(JOP_GREATER_THAN)
    vm_compop( >);

    VM_OP(JOP_GREATER_THAN_EQUAL)
    vm_compop( >=);

    VM_OP(JOP_GREATER_THAN_IMMEDIATE)
    vm_compop_imm( >);

    VM_OP(JOP_EQUALS)
    stack[A] = janet_wrap_boolean(janet_equals(stack[B], stack[C]));
    vm_pcnext();

    VM_OP(JOP_EQUALS_IMMEDIATE)
    stack[A] = janet_wrap_boolean(janet_checktype(stack[B], JANET_NUMBER) && (janet_unwrap_number(stack[B]) == (double) CS));
    vm_pcnext();

    VM_OP(JOP_NOT_EQUALS)
    stack[A] = janet_wrap_boolean(!janet_equals(stack[B], stack[C]));
    vm_pcnext();

    VM_OP(JOP_NOT_EQUALS_IMMEDIATE)
    stack[A] = janet_wrap_boolean(!janet_checktype(stack[B], JANET_NUMBER) || (janet_unwrap_number(stack[B]) != (double) CS));
    vm_pcnext();

    VM_OP(JOP_COMPARE) {
        Janet a = janet_wrap_integer(janet_compare(stack[B], stack[C]));
        stack = fiber->data + fiber->frame;
        stack[A] = a;
    }
    vm_pcnext();

    VM_OP(JOP_NEXT)
    vm_commit();
    {
        Janet temp = janet_next_impl(stack[B], stack[C], 1);
        vm_restore();
        stack[A] = temp;
    }
    vm_pcnext();

    VM_OP(JOP_LOAD_NIL)
    stack[D] = janet_wrap_nil();
    vm_pcnext();

    VM_OP(JOP_LOAD_TRUE)
    stack[D] = janet_wrap_true();
    vm_pcnext();

    VM_OP(JOP_LOAD_FALSE)
    stack[D] = janet_wrap_false();
    vm_pcnext();

    VM_OP(JOP_LOAD_INTEGER)
    stack[A] = janet_wrap_integer(ES);
    vm_pcnext();

    VM_OP(JOP_LOAD_CONSTANT) {
        int32_t cindex = (int32_t)E;
        vm_assert(cindex < func->def->constants_length, "invalid constant");
        stack[A] = func->def->constants[cindex];
        vm_pcnext();
    }

    VM_OP(JOP_LOAD_SELF)
    stack[D] = janet_wrap_function(func);
    vm_pcnext();

    VM_OP(JOP_LOAD_UPVALUE) {
        int32_t eindex = B;
        int32_t vindex = C;
        JanetFuncEnv *env;
        vm_assert(func->def->environments_length > eindex, "invalid upvalue environment");
        env = func->envs[eindex];
        vm_assert(env->length > vindex, "invalid upvalue index");
        vm_assert(janet_env_valid(env), "invalid upvalue environment");
        if (env->offset > 0) {
            /* On stack */
            stack[A] = env->as.fiber->data[env->offset + vindex];
        } else {
            /* Off stack */
            stack[A] = env->as.values[vindex];
        }
        vm_pcnext();
    }

    VM_OP(JOP_SET_UPVALUE) {
        int32_t eindex = B;
        int32_t vindex = C;
        JanetFuncEnv *env;
        vm_assert(func->def->environments_length > eindex, "invalid upvalue environment");
        env = func->envs[eindex];
        vm_assert(env->length > vindex, "invalid upvalue index");
        vm_assert(janet_env_valid(env), "invalid upvalue environment");
        if (env->offset > 0) {
            env->as.fiber->data[env->offset + vindex] = stack[A];
        } else {
            env->as.values[vindex] = stack[A];
        }
        vm_pcnext();
    }

    VM_OP(JOP_CLOSURE) {
        JanetFuncDef *fd;
        JanetFunction *fn;
        int32_t elen;
        int32_t defindex = (int32_t)E;
        vm_assert(defindex < func->def->defs_length, "invalid funcdef");
        fd = func->def->defs[defindex];
        elen = fd->environments_length;
        fn = janet_gcalloc(JANET_MEMORY_FUNCTION, sizeof(JanetFunction) + ((size_t) elen * sizeof(JanetFuncEnv *)));
        fn->def = fd;
        {
            int32_t i;
            for (i = 0; i < elen; ++i) {
                int32_t inherit = fd->environments[i];
                if (inherit == -1 || inherit >= func->def->environments_length) {
                    JanetStackFrame *frame = janet_stack_frame(stack);
                    if (!frame->env) {
                        /* Lazy capture of current stack frame */
                        JanetFuncEnv *env = janet_gcalloc(JANET_MEMORY_FUNCENV, sizeof(JanetFuncEnv));
                        env->offset = fiber->frame;
                        env->as.fiber = fiber;
                        env->length = func->def->slotcount;
                        frame->env = env;
                    }
                    fn->envs[i] = frame->env;
                } else {
                    fn->envs[i] = func->envs[inherit];
                }
            }
        }
        stack[A] = janet_wrap_function(fn);
        vm_checkgc_pcnext();
    }

    VM_OP(JOP_PUSH)
    janet_fiber_push(fiber, stack[D]);
    stack = fiber->data + fiber->frame;
    vm_checkgc_pcnext();

    VM_OP(JOP_PUSH_2)
    janet_fiber_push2(fiber, stack[A], stack[E]);
    stack = fiber->data + fiber->frame;
    vm_checkgc_pcnext();

    VM_OP(JOP_PUSH_3)
    janet_fiber_push3(fiber, stack[A], stack[B], stack[C]);
    stack = fiber->data + fiber->frame;
    vm_checkgc_pcnext();

    VM_OP(JOP_PUSH_ARRAY) {
        const Janet *vals;
        int32_t len;
        if (janet_indexed_view(stack[D], &vals, &len)) {
            janet_fiber_pushn(fiber, vals, len);
        } else {
            janet_panicf("expected %T, got %v", JANET_TFLAG_INDEXED, stack[D]);
        }
    }
    stack = fiber->data + fiber->frame;
    vm_checkgc_pcnext();

    VM_OP(JOP_CALL) {
        vm_maybe_overflow();
        vm_maybe_auto_suspend(1);
        Janet callee = stack[E];
        if (fiber->stacktop > fiber->maxstack) {
            vm_throw("stack overflow");
        }
        if (janet_checktype(callee, JANET_KEYWORD)) {
            vm_commit();
            callee = resolve_method(callee, fiber);
        }
        if (janet_checktype(callee, JANET_FUNCTION)) {
            func = janet_unwrap_function(callee);
            if (func->gc.flags & JANET_FUNCFLAG_TRACE) {
                vm_do_trace(func, fiber->stacktop - fiber->stackstart, fiber->data + fiber->stackstart);
            }
            vm_commit();
            {
                int32_t callargc = fiber->stacktop - fiber->stackstart;
                if (janet_fiber_funcframe(fiber, func)) {
                    int32_t n = fiber->stacktop - fiber->stackstart;
                    janet_panicf("%v called with %d argument%s, expected %d",
                                 callee, n, n == 1 ? "" : "s", func->def->arity);
                }
                /* UAF fix (Phase 4A): capture argv AFTER the frame push.
                 * janet_fiber_funcframe may realloc fiber->data (capacity
                 * growth), so a pointer captured before it dangles; the
                 * args live at fiber->data + fiber->frame afterwards.
                 * For vararg defs the trailing spread args are consolidated
                 * into one tuple at index def->arity, so sample arity+1. */
                if (NULL != janet_vm.profile_table)
                    janet_profile_call(func, fiber->data + fiber->frame,
                                       (func->def->flags & JANET_FUNCDEF_FLAG_VARARG)
                                       ? func->def->arity + 1
                                       : callargc);
            }
            stack = fiber->data + fiber->frame;
            pc = func->def->bytecode;
            vm_checkgc_next();
        } else if (janet_checktype(callee, JANET_CFUNCTION)) {
            vm_commit();
            int32_t argc = fiber->stacktop - fiber->stackstart;
            janet_fiber_cframe(fiber, janet_unwrap_cfunction(callee));
            Janet ret = janet_unwrap_cfunction(callee)(argc, fiber->data + fiber->frame);
            janet_fiber_popframe(fiber);
            stack = fiber->data + fiber->frame;
            stack[A] = ret;
            vm_checkgc_pcnext();
        } else {
            vm_commit();
            stack[A] = call_nonfn(fiber, callee);
            vm_pcnext();
        }
    }

    VM_OP(JOP_TAILCALL) {
        vm_maybe_overflow();
        vm_maybe_auto_suspend(1);
        Janet callee = stack[D];
        if (fiber->stacktop > fiber->maxstack) {
            vm_throw("stack overflow");
        }
        if (janet_checktype(callee, JANET_KEYWORD)) {
            vm_commit();
            callee = resolve_method(callee, fiber);
        }
        if (janet_checktype(callee, JANET_FUNCTION)) {
            func = janet_unwrap_function(callee);
            if (func->gc.flags & JANET_FUNCFLAG_TRACE) {
                vm_do_trace(func, fiber->stacktop - fiber->stackstart, fiber->data + fiber->stackstart);
            }
            {
                int32_t callargc = fiber->stacktop - fiber->stackstart;
                if (janet_fiber_funcframe_tail(fiber, func)) {
                    janet_stack_frame(fiber->data + fiber->frame)->pc = pc;
                    int32_t n = fiber->stacktop - fiber->stackstart;
                    janet_panicf("%v called with %d argument%s, expected %d",
                                 callee, n, n == 1 ? "" : "s", func->def->arity);
                }
                /* UAF fix (Phase 4A): same as JOP_CALL — funcframe_tail
                 * reallocs fiber->data on growth and additionally MEMMOVES
                 * the args down to fiber->data + fiber->frame, so both the
                 * old pointer and the old stackstart location are invalid. */
                if (NULL != janet_vm.profile_table)
                    janet_profile_call(func, fiber->data + fiber->frame,
                                       (func->def->flags & JANET_FUNCDEF_FLAG_VARARG)
                                       ? func->def->arity + 1
                                       : callargc);
            }
            stack = fiber->data + fiber->frame;
            pc = func->def->bytecode;
            vm_checkgc_next();
        } else {
            Janet retreg;
            int entrance_frame = janet_stack_frame(stack)->flags & JANET_STACKFRAME_ENTRANCE;
            vm_commit();
            if (janet_checktype(callee, JANET_CFUNCTION)) {
                int32_t argc = fiber->stacktop - fiber->stackstart;
                janet_fiber_cframe(fiber, janet_unwrap_cfunction(callee));
                retreg = janet_unwrap_cfunction(callee)(argc, fiber->data + fiber->frame);
                janet_fiber_popframe(fiber);
            } else {
                retreg = call_nonfn(fiber, callee);
            }
            janet_fiber_popframe(fiber);
            if (entrance_frame) {
                vm_return_no_restore(JANET_SIGNAL_OK, retreg);
            }
            vm_restore();
            stack[A] = retreg;
            vm_checkgc_pcnext();
        }
    }

    VM_OP(JOP_RESUME) {
        Janet retreg;
        vm_maybe_overflow();
        vm_maybe_auto_suspend(1);
        vm_assert_type(stack[B], JANET_FIBER);
        JanetFiber *child = janet_unwrap_fiber(stack[B]);
        if (janet_check_can_resume(child, &retreg, 0)) {
            vm_commit();
            janet_panicv(retreg);
        }
        fiber->child = child;
        JanetSignal sig = janet_continue_no_check(child, stack[C], &retreg);
        stack = fiber->data + fiber->frame;
        if (sig != JANET_SIGNAL_OK && !(child->flags & (1 << sig))) {
            /* P3.6: nested child suspended (not completed): parking
             * here would strand live C frames for others to clobber
             * if any parked party is shared. Fail loud instead
             * (nested-complete flows through untouched). */
            if ((sig == JANET_SIGNAL_EVENT || sig == JANET_SIGNAL_YIELD ||
                    sig == JANET_SIGNAL_INTERRUPT) &&
                    janet_fiber_chain_has_shared(fiber)) {
                vm_throw("nested fiber suspended under a shared-stack "
                         "ancestor (await outside protect, or use private "
                         "fibers for coroutines)");
            }
            vm_return(sig, retreg);
        }
        fiber->child = NULL;
        stack[A] = retreg;
        vm_checkgc_pcnext();
    }

    VM_OP(JOP_SIGNAL) {
        int32_t s = C;
        if (s > JANET_SIGNAL_USER9) s = JANET_SIGNAL_USER9;
        if (s < 0) s = 0;
        vm_return(s, stack[B]);
    }

    VM_OP(JOP_PROPAGATE) {
        Janet fv = stack[C];
        vm_assert_type(fv, JANET_FIBER);
        JanetFiber *f = janet_unwrap_fiber(fv);
        JanetFiberStatus sub_status = janet_fiber_status(f);
        if (sub_status > JANET_STATUS_USER9) {
            vm_commit();
            janet_panicf("cannot propagate from fiber with status :%s",
                         janet_status_names[sub_status]);
        }
        fiber->child = f;
        vm_return((int) sub_status, stack[B]);
    }

    VM_OP(JOP_CANCEL) {
        Janet retreg;
        vm_assert_type(stack[B], JANET_FIBER);
        JanetFiber *child = janet_unwrap_fiber(stack[B]);
        if (janet_check_can_resume(child, &retreg, 1)) {
            vm_commit();
            janet_panicv(retreg);
        }
        fiber->child = child;
        JanetSignal sig = janet_continue_signal(child, stack[C], &retreg, JANET_SIGNAL_ERROR);
        if (sig != JANET_SIGNAL_OK && !(child->flags & (1 << sig))) {
            /* P3.6: same nested-suspend hazard as JOP_RESUME (a
             * cancelled child may catch and keep running). */
            if ((sig == JANET_SIGNAL_EVENT || sig == JANET_SIGNAL_YIELD ||
                    sig == JANET_SIGNAL_INTERRUPT) &&
                    janet_fiber_chain_has_shared(fiber)) {
                vm_throw("nested fiber suspended under a shared-stack "
                         "ancestor (await outside protect, or use private "
                         "fibers for coroutines)");
            }
            vm_return(sig, retreg);
        }
        fiber->child = NULL;
        stack = fiber->data + fiber->frame;
        stack[A] = retreg;
        vm_checkgc_pcnext();
    }

    VM_OP(JOP_PUT)
    vm_commit();
    fiber->flags |= JANET_FIBER_RESUME_NO_USEVAL;
    janet_put(stack[A], stack[B], stack[C]);
    stack = fiber->data + fiber->frame;
    fiber->flags &= ~JANET_FIBER_RESUME_NO_USEVAL;
    vm_checkgc_pcnext();

    VM_OP(JOP_PUT_INDEX)
    vm_commit();
    fiber->flags |= JANET_FIBER_RESUME_NO_USEVAL;
    janet_putindex(stack[A], C, stack[B]);
    stack = fiber->data + fiber->frame;
    fiber->flags &= ~JANET_FIBER_RESUME_NO_USEVAL;
    vm_checkgc_pcnext();

    VM_OP(JOP_IN)
    vm_commit();
    {
        Janet a = janet_in(stack[B], stack[C]);
        stack = fiber->data + fiber->frame;
        stack[A] = a;
    }
    vm_pcnext();

    /* Fused (in ds <const small int>): key comes from the UNSIGNED
     * immediate rather than a preceding load_integer into a slot.
     * Calls janet_in -- the SAME entry point as JOP_IN above -- so
     * behavior is identical in every case, including the raise on an
     * out-of-range string/symbol/keyword index where janet_getindex
     * (and therefore JOP_GET_INDEX) would instead yield nil. */
    VM_OP(JOP_IN_IMMEDIATE)
    vm_commit();
    {
        Janet a = janet_in(stack[B], janet_wrap_integer((int32_t)(uint8_t) C));
        stack = fiber->data + fiber->frame;
        stack[A] = a;
    }
    vm_pcnext();

    VM_OP(JOP_GET)
    vm_commit();
    {
        Janet a = janet_get(stack[B], stack[C]);
        stack = fiber->data + fiber->frame;
        stack[A] = a;
    }
    vm_pcnext();

    VM_OP(JOP_GET_INDEX)
    vm_commit();
    {
        Janet a = janet_getindex(stack[B], C);
        stack = fiber->data + fiber->frame;
        stack[A] = a;
    }
    vm_pcnext();

    VM_OP(JOP_LENGTH)
    vm_commit();
    {
        Janet a = janet_lengthv(stack[E]);
        stack = fiber->data + fiber->frame;
        stack[A] = a;
    }
    vm_pcnext();

    VM_OP(JOP_MAKE_ARRAY) {
        int32_t count = fiber->stacktop - fiber->stackstart;
        Janet *mem = fiber->data + fiber->stackstart;
        stack[D] = janet_wrap_array(janet_array_n(mem, count));
        fiber->stacktop = fiber->stackstart;
        vm_checkgc_pcnext();
    }

    VM_OP(JOP_MAKE_TUPLE)
    /* fallthrough */
    VM_OP(JOP_MAKE_BRACKET_TUPLE) {
        int32_t count = fiber->stacktop - fiber->stackstart;
        Janet *mem = fiber->data + fiber->stackstart;
        const Janet *tup = janet_tuple_n(mem, count);
        if (opcode == JOP_MAKE_BRACKET_TUPLE)
            janet_tuple_flag(tup) |= JANET_TUPLE_FLAG_BRACKETCTOR;
        stack[D] = janet_wrap_tuple(tup);
        fiber->stacktop = fiber->stackstart;
        vm_checkgc_pcnext();
    }

    VM_OP(JOP_MAKE_TABLE) {
        int32_t count = fiber->stacktop - fiber->stackstart;
        Janet *mem = fiber->data + fiber->stackstart;
        if (count & 1) {
            vm_commit();
            janet_panicf("expected even number of arguments to table constructor, got %d", count);
        }
        JanetTable *table = janet_table(count / 2);
        for (int32_t i = 0; i < count; i += 2)
            janet_table_put(table, mem[i], mem[i + 1]);
        stack[D] = janet_wrap_table(table);
        fiber->stacktop = fiber->stackstart;
        vm_checkgc_pcnext();
    }

    VM_OP(JOP_MAKE_STRUCT) {
        int32_t count = fiber->stacktop - fiber->stackstart;
        Janet *mem = fiber->data + fiber->stackstart;
        if (count & 1) {
            vm_commit();
            janet_panicf("expected even number of arguments to struct constructor, got %d", count);
        }
        JanetKV *st = janet_struct_begin(count / 2);
        for (int32_t i = 0; i < count; i += 2)
            janet_struct_put(st, mem[i], mem[i + 1]);
        stack[D] = janet_wrap_struct(janet_struct_end(st));
        fiber->stacktop = fiber->stackstart;
        vm_checkgc_pcnext();
    }

    VM_OP(JOP_MAKE_STRING) {
        int32_t count = fiber->stacktop - fiber->stackstart;
        Janet *mem = fiber->data + fiber->stackstart;
        JanetBuffer buffer;
        janet_buffer_init(&buffer, 10 * count);
        for (int32_t i = 0; i < count; i++)
            janet_to_string_b(&buffer, mem[i]);
        stack[D] = janet_stringv(buffer.data, buffer.count);
        janet_buffer_deinit(&buffer);
        fiber->stacktop = fiber->stackstart;
        vm_checkgc_pcnext();
    }

    VM_OP(JOP_MAKE_BUFFER) {
        int32_t count = fiber->stacktop - fiber->stackstart;
        Janet *mem = fiber->data + fiber->stackstart;
        JanetBuffer *buffer = janet_buffer(10 * count);
        for (int32_t i = 0; i < count; i++)
            janet_to_string_b(buffer, mem[i]);
        stack[D] = janet_wrap_buffer(buffer);
        fiber->stacktop = fiber->stackstart;
        vm_checkgc_pcnext();
    }

    VM_END()
}

/*
 * Execute a single instruction in the fiber. Does this by inspecting
 * the fiber, setting a breakpoint at the next instruction, executing, and
 * resetting breakpoints to how they were prior. Yes, it's a bit hacky.
 */
JanetSignal janet_step(JanetFiber *fiber, Janet in, Janet *out) {
    /* No finished or currently alive fibers. */
    JanetFiberStatus status = janet_fiber_status(fiber);
    if (status == JANET_STATUS_ALIVE ||
            status == JANET_STATUS_DEAD ||
            status == JANET_STATUS_ERROR) {
        janet_panicf("cannot step fiber with status :%s", janet_status_names[status]);
    }

    /* Get PC for setting breakpoints */
    uint32_t *pc = janet_stack_frame(fiber->data + fiber->frame)->pc;

    /* Check current opcode (sans debug flag). This tells us where the next or next two candidate
     * instructions will be. Usually it's the next instruction in memory,
     * but for branching instructions it is also the target of the branch. */
    uint32_t *nexta = NULL, *nextb = NULL, olda = 0, oldb = 0;

    /* Set temporary breakpoints */
    switch (*pc & 0x7F) {
        default:
            nexta = pc + 1;
            break;
        /* These we just ignore for now. Supporting them means
         * we could step into and out of functions (including JOP_CALL). */
        case JOP_RETURN_NIL:
        case JOP_RETURN:
        case JOP_ERROR:
        case JOP_TAILCALL:
            break;
        case JOP_JUMP:
            nexta = pc + DS;
            break;
        case JOP_JUMP_IF:
        case JOP_JUMP_IF_NOT:
            nexta = pc + 1;
            nextb = pc + ES;
            break;
    }
    if (nexta) {
        olda = *nexta;
        *nexta |= 0x80;
    }
    if (nextb) {
        oldb = *nextb;
        *nextb |= 0x80;
    }

    /* Go */
#ifdef JANET_NATIVE_EXECUTORS
    native_stepping++;
#endif
    JanetSignal signal = janet_continue(fiber, in, out);
#ifdef JANET_NATIVE_EXECUTORS
    native_stepping--;
#endif

    /* Restore */
    if (nexta) *nexta = olda;
    if (nextb) *nextb = oldb;

    return signal;
}

static Janet void_cfunction(int32_t argc, Janet *argv) {
    (void) argc;
    (void) argv;
    janet_panic("placeholder");
}

Janet janet_call(JanetFunction *fun, int32_t argc, const Janet *argv) {
    /* Check entry conditions */
    if (!janet_vm.fiber)
        janet_panic("janet_call failed because there is no current fiber");
    if (janet_vm.stackn >= JANET_RECURSION_GUARD)
        janet_panic("C stack recursed too deeply");
    /* Phase D3: C-driven nesting deepens the native stack; convert a
     * pending fault-handler overflow the same way JOP_CALL does. */
    if (janet_vm.fiber->stack_overflowed) {
        janet_vm.fiber->stack_overflowed = 0;
        janet_panic("fiber native stack overflow");
    }

    /* Dirty stack */
    int32_t dirty_stack = janet_vm.fiber->stacktop - janet_vm.fiber->stackstart;
    if (dirty_stack) {
        janet_fiber_cframe(janet_vm.fiber, void_cfunction);
    }

    /* Tracing */
    if (fun->gc.flags & JANET_FUNCFLAG_TRACE) {
        janet_vm.stackn++;
        vm_do_trace(fun, argc, argv);
        janet_vm.stackn--;
    }

    /* Push frame */
    janet_fiber_pushn(janet_vm.fiber, argv, argc);
    if (janet_fiber_funcframe(janet_vm.fiber, fun)) {
        int32_t min = fun->def->min_arity;
        int32_t max = fun->def->max_arity;
        Janet funv = janet_wrap_function(fun);
        if (min == max && min != argc)
            janet_panicf("arity mismatch in %v, expected %d, got %d", funv, min, argc);
        if (min >= 0 && argc < min)
            janet_panicf("arity mismatch in %v, expected at least %d, got %d", funv, min, argc);
        janet_panicf("arity mismatch in %v, expected at most %d, got %d", funv, max, argc);
    }
    janet_fiber_frame(janet_vm.fiber)->flags |= JANET_STACKFRAME_ENTRANCE;

    /* Set up */
    int32_t oldn = janet_vm.stackn++;
    int handle = janet_gclock();

    /* Run vm */
    janet_vm.fiber->flags |= JANET_FIBER_RESUME_NO_USEVAL | JANET_FIBER_RESUME_NO_SKIP;
    int old_coerce_error = janet_vm.coerce_error;
    janet_vm.coerce_error = 1;
    JanetSignal signal = run_vm(janet_vm.fiber, janet_wrap_nil());
    janet_vm.coerce_error = old_coerce_error;

    /* Phase 1.1 (see docs/internals/native-fibers.md): a suspension signal
     * (yield, user signal, debug, ...) reached through these live C frames
     * must freeze the whole C chain with a context switch instead of being
     * coerced to an error. Each resume re-enters the nested VM below, so
     * the yielding function runs to completion and its C callers observe
     * an ordinary call. */
    while (signal != JANET_SIGNAL_OK && signal != JANET_SIGNAL_ERROR) {
        JanetFiber *susp_fiber = janet_vm.fiber;
        janet_suspend_swap(susp_fiber, signal, *janet_vm.return_reg);
        /* Resumed. A signal may have been smuggled in (e.g. cancel);
         * consume it the way run_vm entry does. */
        if (susp_fiber->flags & JANET_FIBER_RESUME_SIGNAL) {
            signal = (susp_fiber->gc.flags & JANET_FIBER_STATUS_MASK) >> JANET_FIBER_STATUS_OFFSET;
            susp_fiber->gc.flags &= ~JANET_FIBER_STATUS_MASK;
            susp_fiber->flags &= ~JANET_FIBER_RESUME_SIGNAL;
            janet_vm.return_reg[0] = susp_fiber->in_value;
            if (signal == JANET_SIGNAL_OK) {
                /* Cannot happen via continue_signal, but stay total: an OK
                 * smuggle is a plain resume. */
                continue;
            }
            if (signal != JANET_SIGNAL_ERROR) {
                /* A new suspension was injected on resume: propagate it
                 * outward the same way. */
                continue;
            }
            break;
        }
        /* Plain resume: deliver the resume value as the yield result and
         * keep running the nested call. Clear any stale re-entry flags so
         * delivery behaves exactly like the trampoline's run_vm re-entry. */
        susp_fiber->flags &= ~(JANET_FIBER_RESUME_NO_USEVAL | JANET_FIBER_RESUME_NO_SKIP);
        signal = run_vm(susp_fiber, susp_fiber->in_value);
    }

    /* Teardown */
    janet_vm.stackn = oldn;
    janet_gcunlock(handle);
    if (dirty_stack) {
        janet_fiber_popframe(janet_vm.fiber);
        janet_vm.fiber->stacktop += dirty_stack;
    }

    if (signal != JANET_SIGNAL_OK) {
        /* Should match logic in janet_signalv */
#ifdef JANET_EV
        if (janet_vm.root_fiber != NULL && signal == JANET_SIGNAL_EVENT) {
            janet_vm.root_fiber->sched_id++;
        }
#endif
        if (signal != JANET_SIGNAL_ERROR) {
            *janet_vm.return_reg = janet_wrap_string(janet_formatc("%v coerced from %s to error", *janet_vm.return_reg, janet_signal_names[signal]));
        }
        janet_panicv(*janet_vm.return_reg);
    }

    return *janet_vm.return_reg;
}

/* P3.6: walk the resume chain for shared-stack fibers parked
 * mid-C (child links verified intact — a stale link means that
 * ancestor already unwound normally and is safe). Self excluded:
 * the suspender abandons cleanly and re-primes on wake. Bounded. */
static int janet_fiber_chain_has_shared(JanetFiber *fiber) {
    int depth = 0;
    JanetFiber *prev = fiber;
    JanetFiber *f = fiber->resume_fiber;
    while (NULL != f && depth < 1024) {
        if (f->child != prev) break;
        if (f->shared_stack) return 1;
        prev = f;
        f = f->resume_fiber;
        depth++;
    }
    return 0;
}

static JanetSignal janet_check_can_resume(JanetFiber *fiber, Janet *out, int is_cancel) {
    /* Check conditions */
    JanetFiberStatus old_status = janet_fiber_status(fiber);
    if (janet_vm.stackn >= JANET_RECURSION_GUARD) {
        janet_fiber_set_status(fiber, JANET_STATUS_ERROR);
        *out = janet_cstringv("C stack recursed too deeply");
        return JANET_SIGNAL_ERROR;
    }
    /* If a "task" fiber is trying to be used as a normal fiber, detect that. See bug #920.
     * Fibers must be marked as root fibers manually, or by the ev scheduler. */
    if (janet_vm.fiber != NULL && (fiber->gc.flags & JANET_FIBER_EV_GCFLAG_ROOT)) {
#ifdef JANET_EV
        *out = janet_cstringv(is_cancel
                              ? "cannot cancel root fiber, use ev/cancel"
                              : "cannot resume root fiber, use ev/go");
#else
        *out = janet_cstringv(is_cancel
                              ? "cannot cancel root fiber"
                              : "cannot resume root fiber");
#endif
        return JANET_SIGNAL_ERROR;
    }
    if (old_status == JANET_STATUS_ALIVE ||
            old_status == JANET_STATUS_DEAD ||
            (old_status >= JANET_STATUS_USER0 && old_status <= JANET_STATUS_USER4) ||
            old_status == JANET_STATUS_ERROR) {
        const uint8_t *str = janet_formatc("cannot resume fiber with status :%s",
                                           janet_status_names[old_status]);
        *out = janet_wrap_string(str);
        return JANET_SIGNAL_ERROR;
    }
    return JANET_SIGNAL_OK;
}

void janet_try_init(JanetTryState *state) {
    state->stackn = janet_vm.stackn++;
    state->gc_handle = janet_vm.gc_suspend;
    state->vm_fiber = janet_vm.fiber;
    state->vm_jmp_buf = janet_vm.signal_buf;
    state->vm_return_reg = janet_vm.return_reg;
    state->coerce_error = janet_vm.coerce_error;
    janet_vm.return_reg = &(state->payload);
    janet_vm.signal_buf = &(state->buf);
    janet_vm.coerce_error = 0;
}

void janet_restore(JanetTryState *state) {
    janet_vm.stackn = state->stackn;
    janet_vm.gc_suspend = state->gc_handle;
    janet_vm.fiber = state->vm_fiber;
    janet_vm.signal_buf = state->vm_jmp_buf;
    janet_vm.return_reg = state->vm_return_reg;
    janet_vm.coerce_error = state->coerce_error;
}

/* Phase 1.1 swap-suspension (see docs/internals/native-fibers.md). The VM * dynamic registers always describe the RUNNING party. When a fiber
 * suspends with live C frames, its registers are parked in the fiber and
 * the resumer's parked registers loaded, so nothing points into the
 * frozen C stack while the resumer runs. janet_continue_no_check does the
 * inverse when resuming. Host (non-fiber) code parks in janet_vm.base_*. */

/* Park the live registers into a fiber (the suspending/yielding party). */
static void janet_vmregs_save(JanetFiber *f) {
    f->saved_signal_buf = janet_vm.signal_buf;
    f->saved_return_reg = janet_vm.return_reg;
    f->saved_coerce_error = janet_vm.coerce_error;
    f->saved_stackn = janet_vm.stackn;
    f->saved_gc_suspend = janet_vm.gc_suspend;
}

/* Load parked registers from a fiber (the party being switched to). */
static void janet_vmregs_load(JanetFiber *f) {
    janet_vm.signal_buf = f->saved_signal_buf;
    janet_vm.return_reg = f->saved_return_reg;
    janet_vm.coerce_error = f->saved_coerce_error;
    janet_vm.stackn = f->saved_stackn;
    janet_vm.gc_suspend = f->saved_gc_suspend;
}

/* Park the live registers into the host slots. */
static void janet_vmregs_save_base(void) {
    janet_vm.base_signal_buf = janet_vm.signal_buf;
    janet_vm.base_return_reg = janet_vm.return_reg;
    janet_vm.base_coerce_error = janet_vm.coerce_error;
    janet_vm.base_stackn = janet_vm.stackn;
    janet_vm.base_gc_suspend = janet_vm.gc_suspend;
}

/* Load parked registers from the host slots. */
static void janet_vmregs_load_base(void) {
    janet_vm.signal_buf = janet_vm.base_signal_buf;
    janet_vm.return_reg = janet_vm.base_return_reg;
    janet_vm.coerce_error = janet_vm.base_coerce_error;
    janet_vm.stackn = janet_vm.base_stackn;
    janet_vm.gc_suspend = janet_vm.base_gc_suspend;
}

/* Suspend the running fiber with live C frames on its native stack.
 * Publishes sig/payload exactly like the trampoline does on a returned
 * signal, parks this fiber's registers, loads the resumer's, and switches
 * out. On resume, execution continues after this call with this fiber's
 * registers live again (re-parked by janet_continue_no_check beforehand). */
static void janet_suspend_swap(JanetFiber *fiber, JanetSignal sig, Janet payload) {
    fiber->out_payload = payload;
    fiber->out_signal = sig;
    fiber->last_value = payload;
    janet_fiber_set_status(fiber, sig);
    /* The previous resume argument has been consumed. A parked fiber
     * receives a fresh input before its C frames run again. */
    fiber->in_value = janet_wrap_nil();
    fiber->suspend_mode = 1;
    janet_vmregs_save(fiber);
    JanetFiber *resumer = fiber->resume_fiber;
    if (resumer) {
        janet_vmregs_load(resumer);
    } else {
        janet_vmregs_load_base();
    }
    janet_vm.fiber = resumer;
    janet_fiber_ctx_swap(&fiber->ctx, fiber->resume_ctx);
}

/* Suspend the current fiber from C with a yield signal (see janet.h).
 * Works from any C code running on the fiber's native stack: cfunctions,
 * nested janet_call frames, or future AOT-compiled frames. */
Janet janet_fiber_yield(Janet v) {
    JanetFiber *fiber = janet_vm.fiber;
    if (NULL == fiber) janet_panic("janet_fiber_yield failed because there is no current fiber");
    if (NULL == fiber->resume_ctx) janet_panic("janet_fiber_yield failed because the fiber was never resumed");
    /* P3.6: raw yield needs its C frames preserved across the switch
     * (resume continues them), which shared stacks cannot do (others
     * clobber the region). Fail loud — coroutines stay private. */
    if (fiber->shared_stack) {
        janet_panic("cannot yield from a shared-stack fiber "
                    "(use private fibers for coroutines)");
    }
    JanetSignal sig = JANET_SIGNAL_YIELD;
    for (;;) {
        janet_suspend_swap(fiber, sig, v);
        /* Resumed: consume a smuggled signal the way run_vm entry does. */
        if (fiber->flags & JANET_FIBER_RESUME_SIGNAL) {
            JanetSignal psig = (fiber->gc.flags & JANET_FIBER_STATUS_MASK) >> JANET_FIBER_STATUS_OFFSET;
            fiber->gc.flags &= ~JANET_FIBER_STATUS_MASK;
            fiber->flags &= ~JANET_FIBER_RESUME_SIGNAL;
            janet_vm.return_reg[0] = fiber->in_value;
            if (psig == JANET_SIGNAL_ERROR) {
                janet_panicv(fiber->in_value);
            }
            if (psig != JANET_SIGNAL_OK) {
                /* A new suspension was injected on resume: send the
                 * injected signal outward with the resume value. */
                sig = psig;
                v = fiber->in_value;
                continue;
            }
        }
        return fiber->in_value;
    }
}

/* Per-fiber interpreter loop; see docs/internals/native-fibers.md. This
 * runs on the fiber's own native stack (primed via janet_fiber_ctx_prime).
 * Between resumes of a fiber, we sit at the ctx_swap waiting to be
 * switched in; on each resume, we run one VM entry, publish the result on
 * the fiber, and switch back to the resumer. VM values, signals and
 * longjmp targets never cross fiber boundaries: every signal handler
 * (jmp_buf) and return register lives in this loop's JanetTryState on the
 * fiber's stack. */
void janet_fiber_trampoline(void) {
    for (;;) {
        JanetFiber *fiber = janet_vm.fiber;
        JanetSignal sig;
        JanetTryState tstate;
        sig = janet_try(&tstate);
        if (!sig) {
            sig = run_vm(fiber, fiber->in_value);
        }
        janet_restore(&tstate);
        fiber->out_payload = tstate.payload;
        fiber->out_signal = sig;
        fiber->last_value = tstate.payload;
        /* run_vm consumed the previous input; the next resume installs
         * its own value. Do not retain the old input while parked. */
        fiber->in_value = janet_wrap_nil();
        /* The fiber returned to the trampoline (no frozen C stack above
         * this point): resume re-enters run_vm, and any stale swap-suspend
         * mode from an earlier nested suspension is over. */
        fiber->suspend_mode = 0;
        janet_fiber_set_status(fiber, sig);
        /* Hand control back with a full register exchange, mirroring
         * janet_suspend_swap: park this fiber, load the resumer. */
        janet_vmregs_save(fiber);
        JanetFiber *tr_resumer = fiber->resume_fiber;
        if (tr_resumer) {
            janet_vmregs_load(tr_resumer);
        } else {
            janet_vmregs_load_base();
        }
        janet_vm.fiber = tr_resumer;
        janet_fiber_ctx_swap(&fiber->ctx, fiber->resume_ctx);
    }
}

static JanetSignal janet_continue_no_check(JanetFiber *fiber, Janet in, Janet *out) {

    /* M4b-2 quantum driver: every resume — loop-scheduled or direct —
     * starts with a fresh poll budget. UNCONDITIONAL (not gated on
     * quantum): anyone may enable quanta mid-run, and a stale zero
     * budget would suspend at the next poll with zero progress. One
     * predictable store per resume. */
    janet_vm.quantum_remaining = JANET_QUANTUM_BUDGET;

    JanetFiberStatus old_status = janet_fiber_status(fiber);

#ifdef JANET_EV
    janet_fiber_did_resume(fiber);
#endif

    /* Clear last value */
    fiber->last_value = janet_wrap_nil();

    /* Phase D3: pending red-zone overflow surfaces here (check-and-
     * clear, mirroring the interrupt block below): the C stack already
     * unwound past the fault by suspension, so raising is safe. */
    if (fiber->stack_overflowed) {
        fiber->stack_overflowed = 0;
        Janet ov = janet_cstringv("fiber native stack overflow");
        fiber->last_value = ov;
        *out = ov;
        janet_fiber_set_status(fiber, JANET_SIGNAL_ERROR);
        return JANET_SIGNAL_ERROR;
    }

#ifdef JANET_EV
    /* M4b-1 cooperative interrupt (see ev/interrupt): a pending
     * request suspends here — before a single instruction runs — so
     * delivery can never land mid-instruction, inside pcall, or in C.
     * A racing wakeup value in `in` is dropped (interrupt wins). */
    if (fiber->interrupt_requested) {
        fiber->interrupt_requested = 0;
        Janet iv = fiber->interrupt_value;
        fiber->interrupt_value = janet_wrap_nil();
        fiber->last_value = iv;
        *out = iv;
        janet_fiber_set_status(fiber, JANET_SIGNAL_YIELD);
        return JANET_SIGNAL_YIELD;
    }
#endif

    /* Continue child fiber if it exists */
    if (fiber->child) {
        if (janet_vm.root_fiber == NULL) janet_vm.root_fiber = fiber;
        JanetFiber *child = fiber->child;
        uint32_t instr = (janet_stack_frame(fiber->data + fiber->frame)->pc)[0];
        janet_vm.stackn++;
        JanetSignal sig = janet_continue(child, in, &in);
        janet_vm.stackn--;
        if (janet_vm.root_fiber == fiber) janet_vm.root_fiber = NULL;
        if (sig != JANET_SIGNAL_OK && !(child->flags & (1 << sig))) {
            *out = in;
            janet_fiber_set_status(fiber, sig);
            fiber->last_value = child->last_value;
            return sig;
        }
        /* Check if we need any special handling for certain opcodes */
        switch (instr & 0x7F) {
            default:
                break;
            case JOP_NEXT: {
                if (sig == JANET_SIGNAL_OK ||
                        sig == JANET_SIGNAL_ERROR ||
                        sig == JANET_SIGNAL_USER0 ||
                        sig == JANET_SIGNAL_USER1 ||
                        sig == JANET_SIGNAL_USER2 ||
                        sig == JANET_SIGNAL_USER3 ||
                        sig == JANET_SIGNAL_USER4) {
                    in = janet_wrap_nil();
                } else {
                    in = janet_wrap_integer(0);
                }
                break;
            }
        }
        fiber->child = NULL;
    }

    /* Handle new fibers being resumed with a non-nil value */
    if (old_status == JANET_STATUS_NEW && !janet_checktype(in, JANET_NIL)) {
        Janet *stack = fiber->data + fiber->frame;
        JanetFunction *func = janet_stack_frame(stack)->func;
        if (func) {
            if (func->def->arity > 0) {
                stack[0] = in;
            } else if (func->def->flags & JANET_FUNCDEF_FLAG_VARARG) {
                stack[0] = janet_wrap_tuple(janet_tuple_n(&in, 1));
            }
        }
    }

    /* If this is a nested continue (root_fiber already set), root the fiber
     * so it survives GC. janet_collect only marks root_fiber, so without
     * this a nested fiber (e.g., from janet_pcall in a C function) would be
     * invisible to GC and could be collected while actively running. */
    int fiber_rooted = (janet_vm.root_fiber != NULL);
    if (fiber_rooted) {
        janet_gcroot(janet_wrap_fiber(fiber));
    }

    /* Switch onto the target fiber's own native stack. Phase 1.1: the VM
     * dynamic registers always describe the running party, so park the
     * caller's live registers and load the target's parked registers
     * (fresh fibers park zeroes at creation). The target's suspend path
     * (janet_suspend_swap or the trampoline publish above) inverts this
     * exchange on the way out. Fallible work (gcroot) stays above the
     * exchange, where the caller's signal chain is still live. */
    JanetFiber *caller = janet_vm.fiber;
    JanetFiberCtx *caller_ctx = caller ? &caller->ctx : &janet_vm.base_ctx;
    if (caller) {
        janet_vmregs_save(caller);
    } else {
        janet_vmregs_save_base();
    }
    janet_vmregs_load(fiber);

    if (janet_vm.root_fiber == NULL) janet_vm.root_fiber = fiber;
    janet_vm.fiber = fiber;
    janet_fiber_set_status(fiber, JANET_STATUS_ALIVE);

    fiber->in_value = in;
    fiber->resume_ctx = caller_ctx;
    fiber->resume_fiber = caller;
    /* P3.6: stacks materialize on first resume (never at alloc, so
     * never-run fibers map nothing). Shared fibers re-prime on the
     * lane mapping every resume (parked position is always the
     * committed pc); private fibers map+prime once. One branch each. */
    if (fiber->shared_stack) {
        janet_fiber_shared_prime(fiber);
    } else if (NULL == fiber->native_stack_base) {
        janet_fiber_ensure_native_stack(fiber);
    }
    janet_fiber_ctx_swap(caller_ctx, &fiber->ctx);

    JanetSignal sig = fiber->out_signal;
    janet_vm.fiber = caller;
    if (janet_vm.root_fiber == fiber) janet_vm.root_fiber = NULL;
    janet_fiber_set_status(fiber, sig);
    if (fiber_rooted) {
        janet_gcunroot(janet_wrap_fiber(fiber));
    }
    fiber->last_value = fiber->out_payload;
    *out = fiber->out_payload;

    return sig;
}

/* Enter the main vm loop */
JanetSignal janet_continue(JanetFiber *fiber, Janet in, Janet *out) {
    /* Check conditions */
    JanetSignal tmp_signal = janet_check_can_resume(fiber, out, 0);
    if (tmp_signal) return tmp_signal;
    return janet_continue_no_check(fiber, in, out);
}

/* Enter the main vm loop but immediately raise a signal */
JanetSignal janet_continue_signal(JanetFiber *fiber, Janet in, Janet *out, JanetSignal sig) {
    JanetSignal tmp_signal = janet_check_can_resume(fiber, out, sig != JANET_SIGNAL_OK);
    if (tmp_signal) return tmp_signal;
    if (sig != JANET_SIGNAL_OK) {
        JanetFiber *child = fiber;
        while (child->child) child = child->child;
        /* NOTE: We are "smuggling" flags in for later use in an unusal place. This is odd but intentional and
         * saves a bit of memory per fiber rather than creating a new field. There is likely a better way to do this. */
        child->gc.flags &= ~JANET_FIBER_STATUS_MASK;
        child->gc.flags |= sig << JANET_FIBER_STATUS_OFFSET;
        child->flags |= JANET_FIBER_RESUME_SIGNAL;
    }
    return janet_continue_no_check(fiber, in, out);
}

JanetSignal janet_pcall(
    JanetFunction *fun,
    int32_t argc,
    const Janet *argv,
    Janet *out,
    JanetFiber **f) {
    JanetFiber *fiber;
    if (f && *f) {
        fiber = janet_fiber_reset(*f, fun, argc, argv);
    } else {
        fiber = janet_fiber(fun, 64, argc, argv);
    }
    if (f) *f = fiber;
    if (NULL == fiber) {
        *out = janet_cstringv("arity mismatch");
        return JANET_SIGNAL_ERROR;
    }
    return janet_continue(fiber, janet_wrap_nil(), out);
}

Janet janet_mcall(const char *name, int32_t argc, Janet *argv) {
    /* At least 1 argument */
    if (argc < 1) {
        janet_panicf("method :%s expected at least 1 argument", name);
    }
    /* Find method */
    Janet method = janet_method_lookup(argv[0], name);
    if (janet_checktype(method, JANET_NIL)) {
        janet_panicf("could not find method :%s for %v", name, argv[0]);
    }
    /* Invoke method */
    return janet_method_invoke(method, argc, argv);
}

/* Setup VM */
int janet_init(void) {

    /* Garbage collection */
    janet_vm.blocks = NULL;
    janet_vm.weak_blocks = NULL;
    janet_vm.string_blocks = NULL;
    janet_vm.sweep_pending[0] = NULL;
    janet_vm.sweep_pending[1] = NULL;
    janet_vm.sweep_pending[2] = NULL;
    janet_vm.gc_quarantine[0] = NULL;
    janet_vm.gc_quarantine[1] = NULL;
    janet_vm.gc_quarantine[2] = NULL;
    janet_vm.gc_quarantine_count = 0;
    janet_vm.next_collection = 0;
    janet_vm.gc_interval = 0x400000;
    janet_vm.block_count = 0;
    janet_vm.gc_mark_phase = 0;
    /* Generational policy (Phase 2b-iii): minors run on nursery pressure. */
    janet_vm.gc_minor_phase = 0;
    janet_vm.gc_young_bytes = 0;
    janet_vm.gc_minor_threshold = 256 * 1024;
    /* Step 1 (process heaps): owner-id source starts empty (0 reserved). */
    janet_vm.gc_owner_next = 0;
    /* Step P1 (proc processes): pid serials start empty (0 reserved). */
    janet_vm.proc_serial = 0;
    /* P3.3 gate: no strict fibers yet (spawn-isolated sets it). */
    janet_vm.heap_strict_live = 0;
    /* P3.5c allocation attribution starts clear (spawn sets it). */
    janet_vm.gc_alloc_ctx = NULL;
    /* P3.6: lane-shared stack unmapped until first shared resume;
     * opt-in read once (any set value enables). */
    janet_vm.shared_stack_base = NULL;
    janet_vm.shared_stack = NULL;
    janet_vm.shared_stack_size = 0;
    janet_vm.shared_stack_committed = NULL;
    /* P3.6 shared C stacks default ON for ev-task + proc fibers: the
     * spawn profile shows per-spawn stack malloc-large + OS zero-fill
     * as the dominant density cost (~1 stack per actor). Opt OUT with
     * JANET_SHARED_STACKS=0 (generators and C-frame yields keep private
     * stacks regardless — suspend discipline, enforced at fiber_new). */
    {
        const char *ssflag = getenv("JANET_SHARED_STACKS");
        janet_vm.shared_stacks_on = !(NULL != ssflag && 0 == strcmp(ssflag, "0"));
    }

    janet_symcache_init();

    /* Initialize gc roots */
    janet_vm.roots = NULL;
    janet_vm.root_count = 0;
    janet_vm.root_capacity = 0;

    /* Initialize shadow root stack (Phase 2b-i) */
    janet_vm.shadow_roots = NULL;
    janet_vm.shadow_count = 0;
    janet_vm.shadow_capacity = 0;

    /* Slab allocator state starts empty (Phase 2b-iii, lazy). */
    janet_vm.slab_state = NULL;

    /* Scratch memory */
    janet_vm.user = NULL;
    janet_vm.scratch_mem = NULL;
    janet_vm.scratch_len = 0;
    janet_vm.scratch_cap = 0;

    /* Sandbox flags */
    janet_vm.sandbox_flags = 0;

    /* Initialize registry */
    janet_vm.registry = NULL;
    janet_vm.registry_cap = 0;
    janet_vm.registry_count = 0;
    janet_vm.registry_dirty = 0;

    /* Initialize abstract registry */
    janet_vm.abstract_registry = janet_table(0);
    janet_gcroot(janet_wrap_table(janet_vm.abstract_registry));

    /* B7.1 named proc registry: lazy (NULL until proc/register runs;
     * NULL reads exactly like an empty table at every use site). */
    janet_vm.proc_registry = NULL;

    /* Traversal */
    janet_vm.traversal = NULL;
    janet_vm.traversal_base = NULL;
    janet_vm.traversal_top = NULL;

    /* Core env */
    janet_vm.core_env = NULL;

    /* Auto suspension */
    janet_vm.auto_suspend = 0;

    /* M4b-2 quantum driver: ON by default (Step 2 preemptive
     * reductions); JANET_TIMESLICE=0 restores fully cooperative
     * scheduling per process, ev/timeslice toggles per VM. */
    janet_vm.quantum = 0;
    janet_vm.quantum_remaining = 0;
    {
        const char *tsenv = getenv("JANET_TIMESLICE");
        if (NULL == tsenv || strcmp(tsenv, "0") != 0) {
            janet_vm.quantum = 1;
        }
    }

    /* Dynamic bindings */
    janet_vm.top_dyns = NULL;

    /* Seed RNG */
    janet_rng_seed(janet_default_rng(), 0);
#ifdef JANET_OPCODE_HISTOGRAM
    atexit(janet_oph_dump);
#endif

    /* Fibers */
    janet_vm.fiber = NULL;
    janet_vm.root_fiber = NULL;
    janet_vm.stackn = 0;
    /* Phase D: fault handler (process-once) + this thread's alt stack,
     * before any fiber can run here. */
    janet_fiber_growstack_init();
    /* Phase 1.1/2a: no active compilation, clean host register park. */
    janet_vm.compiler_stack = NULL;
    janet_vm.base_signal_buf = NULL;
    janet_vm.base_return_reg = NULL;
    janet_vm.base_coerce_error = 0;
    janet_vm.base_stackn = 0;
    janet_vm.base_gc_suspend = 0;

#ifdef JANET_EV
    janet_ev_init();
    janet_abstract_home_register();
#endif
#ifdef JANET_NET
    janet_net_init();
#endif
    return 0;
}

/* Disable some features at runtime with no way to re-enable them */
void janet_sandbox(uint32_t flags) {
    janet_sandbox_assert(JANET_SANDBOX_SANDBOX);
    janet_vm.sandbox_flags |= flags;
}

void janet_sandbox_assert(uint32_t forbidden_flags) {
    if (forbidden_flags & janet_vm.sandbox_flags) {
        janet_panic("operation forbidden by sandbox");
    }
}

/* Total recorded calls across the live table. Flushes first so
 * C-side cached counts land in the tables, then sums tables only. */
static int64_t janet_profile_total_calls(void) {
    int64_t total = 0;
    if (NULL != janet_vm.profile_table) {
        janet_profile_flush();
        {
            const JanetKV *kvs;
            int32_t cap, count, i;
            janet_dictionary_view(janet_wrap_table(janet_vm.profile_table), &kvs, &count, &cap);
            for (i = 0; i < cap; i++) {
                const JanetKV *kv = kvs + i;
                if (janet_checktype(kv->key, JANET_NIL)) continue;
                total += janet_unwrap_integer(janet_table_get(
                    janet_unwrap_table(kv->value), janet_ckeywordv("calls")));
            }
        }
    }
    return total;
}

/* AOT launchers keep their auto-PGO destination in this VM thread rather
 * than process-wide env, so spawned interpreters cannot inherit it. */
static JANET_THREAD_LOCAL char *janet_auto_profile_output;

void janet_profile_auto_output(const char *path) {
    char *copy = NULL;
    if (path && *path) {
        size_t len = strlen(path) + 1;
        copy = malloc(len);
        if (copy) memcpy(copy, path, len);
    }
    free(janet_auto_profile_output);
    janet_auto_profile_output = copy;
}

/* Clear all memory associated with the VM */
void janet_deinit(void) {
#ifdef JANET_EV
    /* Publish shutdown before profile flushing or GC can stall the loop. */
    janet_abstract_home_unregister();
#endif
    /* The private AOT destination or explicit JANET_PROFILE_OUT dumps on
     * every exit path, including os/exit, before VM teardown. Auto-PGO
     * applies the minimum-call threshold; explicit output preserves its
     * existing policy. Override the floor with JANET_AUTO_PGO_MIN_CALLS. */
    if (NULL != janet_vm.profile_table) {
        const char *pout = janet_auto_profile_output ? janet_auto_profile_output : getenv("JANET_PROFILE_OUT");
        janet_profile_maybe_stats();
        if (NULL != pout && '\0' != *pout) {
            int dump = 1;
            if (janet_auto_profile_output || NULL != getenv("JANET_AUTO_PGO_ACTIVE")) {
                const char *minenv = getenv("JANET_AUTO_PGO_MIN_CALLS");
                long mincalls = (NULL == minenv || '\0' == *minenv)
                    ? 10000L : strtol(minenv, NULL, 10);
                if (janet_profile_total_calls() < mincalls)
                    dump = 0;
            }
            if (dump) {
                janet_profile_dump(pout);
                /* A forced run first writes to an exclusively reserved
                 * .pending sidecar. Publish only its completed dump. */
                if (janet_auto_profile_output) {
                    size_t len = strlen(pout);
                    if (len > 8 && !strcmp(pout + len - 8, ".pending")) {
                        char *completed = malloc(len - 8 + sizeof(".jdn"));
                        if (completed) {
                            const char *first = strstr(pout, ".jdn.first-");
                            if (first && first < pout + len - 8) {
                                size_t base_len = (size_t) (first - pout) + 4;
                                memcpy(completed, pout, base_len);
                                completed[base_len] = '\0';
                            } else {
                                memcpy(completed, pout, len - 8);
                                memcpy(completed + len - 8, ".jdn", sizeof(".jdn"));
                            }
                            /* No replacement: a parallel first run or builder
                             * may already have installed the base profile. */
#if defined(_WIN32)
                            if (!MoveFileA(pout, completed)) remove(pout);
#else
                            (void) link(pout, completed);
                            unlink(pout);
#endif
                            free(completed);
                        }
                    }
                }
            }
            janet_vm.profile_table = NULL;
        }
    }
    janet_profile_auto_output(NULL);
    janet_clear_memory();
    janet_symcache_deinit();
    janet_free(janet_vm.roots);
    janet_vm.roots = NULL;
    janet_vm.root_count = 0;
    janet_vm.root_capacity = 0;
    janet_free(janet_vm.shadow_roots);
    janet_vm.shadow_roots = NULL;
    janet_vm.shadow_count = 0;
    janet_vm.shadow_capacity = 0;
    /* Slab pages went with clear_memory above; drop the state itself. */
    janet_free(janet_vm.slab_state);
    janet_vm.slab_state = NULL;
    janet_vm.abstract_registry = NULL;
    janet_vm.proc_registry = NULL;
    janet_vm.core_env = NULL;
    janet_vm.top_dyns = NULL;
    janet_vm.user = NULL;
    janet_free(janet_vm.traversal_base);
    janet_vm.fiber = NULL;
    janet_vm.root_fiber = NULL;
    janet_free(janet_vm.registry);
    janet_vm.registry = NULL;
#ifdef JANET_EV
    janet_ev_deinit();
#endif
#ifdef JANET_NET
    janet_net_deinit();
#endif
}
