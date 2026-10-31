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
#include <sys/stat.h>
#include <sys/types.h>
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
/* Unsigned immediate: the C field as 0..255. JINT_SSU opcodes use this
 * so a mask like 0xff (255) is representable, which a signed int8
 * immediate cannot hold. */
#define CU ((*pc) >> 24)
#define DS (*((int32_t *)pc) >> 8)
#define ES (*((int32_t *)pc) >> 16)

/* Branch prediction for hot interpreter paths. Numbers are the common
 * case in ALU ops; GC thresholds rarely trip; errors are cold. */
#if defined(__GNUC__) || defined(__clang__)
#define BENDER_LIKELY(x) __builtin_expect(!!(x), 1)
#define BENDER_UNLIKELY(x) __builtin_expect(!!(x), 0)
#else
#define BENDER_LIKELY(x) (x)
#define BENDER_UNLIKELY(x) (x)
#endif

/* How we dispatch instructions. By default, we use
 * a switch inside an infinite loop. For GCC/clang, we use
 * computed gotos. */
#if defined(__GNUC__) && !defined(__EMSCRIPTEN__) && !defined(BENDER_NO_COMPUTED_GOTOS)
#define BENDER_USE_COMPUTED_GOTOS
#endif

/* Opcode histogram (diagnostic only, -DBENDER_OPCODE_HISTOGRAM). Counts
 * dispatches per opcode and PAIRS of consecutive opcodes, so fusion /
 * superinstruction candidates can be chosen from data rather than
 * guessed. Dumped at exit. Adds two increments per dispatch, so it is
 * never on in a normal build. */
#ifdef BENDER_OPCODE_HISTOGRAM
extern uint64_t bender_oph[256];
extern uint64_t bender_ophp[256][256];
extern int bender_oph_prev;
#define BENDER_OPH_TICK() do { int o_ = (int)(*pc & 0xFF); bender_oph[o_]++;     if (bender_oph_prev >= 0) bender_ophp[bender_oph_prev][o_]++; bender_oph_prev = o_; } while (0)
#else
#define BENDER_OPH_TICK() ((void)0)
#endif

#ifndef BENDER_NATIVE_TELEMETRY
#define BENDER_NATIVE_TELEMETRY 1
#endif
#if defined(BENDER_NATIVE_EXECUTORS) && BENDER_NATIVE_TELEMETRY
#define BENDER_DISPATCH_TICK() (++bender_vm.native_interp_dispatches)
#else
#define BENDER_DISPATCH_TICK() ((void)0)
#endif

#ifdef BENDER_USE_COMPUTED_GOTOS
#define VM_START() { BENDER_DISPATCH_TICK(); BENDER_SITE_TICK(first_opcode); goto *op_lookup[first_opcode];
#define VM_END() }
#define VM_OP(op) label_##op :
#define VM_DEFAULT() label_unknown_op:
#define vm_next() do { vm_native(); BENDER_DISPATCH_TICK(); BENDER_OPH_TICK(); BENDER_SITE_TICK(*pc & 0xFF); goto *op_lookup[*pc & 0xFF]; } while (0)
#define opcode (*pc & 0xFF)
#else
#define VM_START() uint8_t opcode = first_opcode; for (;;) {BENDER_DISPATCH_TICK(); BENDER_SITE_TICK(opcode); switch(opcode) {
#define VM_END() }}
#define VM_OP(op) case op :
#define VM_DEFAULT() default:
#define vm_next() vm_native(); opcode = *pc & 0xFF; continue
#endif

#ifdef BENDER_OPCODE_HISTOGRAM
uint64_t bender_oph[256];
uint64_t bender_ophp[256][256];
int bender_oph_prev = -1;
static void bender_oph_dump(void) {
    uint64_t total = 0;
    for (int i = 0; i < 256; i++) total += bender_oph[i];
    if (!total) return;
    fprintf(stderr, "opcode-histogram total=%llu\n", (unsigned long long) total);
    /* singles, descending, top 24 */
    for (int rank = 0; rank < 24; rank++) {
        int best = -1; uint64_t bv = 0;
        for (int i = 0; i < 256; i++) if (bender_oph[i] > bv) { bv = bender_oph[i]; best = i; }
        if (best < 0 || !bv) break;
        fprintf(stderr, "op %d count=%llu pct=%.2f\n", best,
                (unsigned long long) bv, 100.0 * (double) bv / (double) total);
        bender_oph[best] = 0;
    }
    /* consecutive PAIRS, descending, top 24 -- fusion candidates */
    for (int rank = 0; rank < 24; rank++) {
        int ba = -1, bb = -1; uint64_t bv = 0;
        for (int a = 0; a < 256; a++)
            for (int b = 0; b < 256; b++)
                if (bender_ophp[a][b] > bv) { bv = bender_ophp[a][b]; ba = a; bb = b; }
        if (ba < 0 || !bv) break;
        fprintf(stderr, "pair %d,%d count=%llu pct=%.2f\n", ba, bb,
                (unsigned long long) bv, 100.0 * (double) bv / (double) total);
        bender_ophp[ba][bb] = 0;
    }
}
#endif

/* Diagnostic-only per-original-PC VM dispatch counts, separate from PGO.
 * Use an interpreter-only binary: native executor instructions are NOT
 * counted. Compile-disabled builds remove every hook; compile-enabled builds
 * additionally require BENDER_TTFX_SITE_PROFILE in the environment. The
 * bounded fixed table avoids allocations and GC at dispatch. State is
 * per-thread: workers have separate VM-local dumps, never a shared counter
 * stream. Out-of-range PCs and capacity overflow report dropped events. */
#ifdef BENDER_TTFX_SITE_PROFILE
#define BENDER_SITE_MAX_DEFS 4096
#define BENDER_SITE_MAX_PC 1024
#define BENDER_SITE_NAME_BYTES 192
#ifndef BENDER_SITE_COUNTER_MAX
#define BENDER_SITE_COUNTER_MAX UINT64_MAX
#endif
#include <inttypes.h>
typedef struct {
    uint64_t id;
    uint64_t fingerprint;
    int32_t words;
    uint8_t op[BENDER_SITE_MAX_PC];
    char source[BENDER_SITE_NAME_BYTES];
    char name[BENDER_SITE_NAME_BYTES];
    uint64_t total;
    uint64_t pc[BENDER_SITE_MAX_PC];
    uint64_t call[BENDER_SITE_MAX_PC];
} BenderSiteRow;
static BENDER_THREAD_LOCAL BenderSiteRow bender_site_rows[BENDER_SITE_MAX_DEFS];
static BENDER_THREAD_LOCAL size_t bender_site_count;
static BENDER_THREAD_LOCAL uint64_t bender_site_dropped;
static BENDER_THREAD_LOCAL int bender_site_enabled;
static BENDER_THREAD_LOCAL BenderSiteRow *bender_site_last;
static BENDER_THREAD_LOCAL uint64_t bender_site_last_id;
static BENDER_THREAD_LOCAL uint64_t bender_site_next_id;
uint64_t bender_site_profile_new_id(void) {
    if (bender_site_next_id == UINT64_MAX) return 0;
    return ++bender_site_next_id;
}
static void bender_site_saturate(uint64_t *counter) {
    if (*counter >= BENDER_SITE_COUNTER_MAX) {
        if (&bender_site_dropped != counter && bender_site_dropped < UINT64_MAX)
            ++bender_site_dropped;
    } else ++*counter;
}
static void bender_site_name(char *out, BenderString name) {
    if (!name) { out[0] = 0; return; }
    int32_t n = bender_string_length(name);
    if (n >= BENDER_SITE_NAME_BYTES) n = BENDER_SITE_NAME_BYTES - 1;
    for (int32_t i = 0; i < n; ++i) {
        unsigned char ch = name[i];
        out[i] = (ch >= 32 && ch < 127 && ch != ',' && ch != '\\') ? (char)ch : '_';
    }
    out[n] = 0;
}
static uint64_t bender_site_fingerprint(BenderFuncDef *def) {
    uint64_t hash = UINT64_C(14695981039346656037);
    for (int32_t i = 0; i < def->bytecode_length; ++i) {
        uint32_t word = def->bytecode[i];
        for (int j = 0; j < 4; ++j) {
            hash ^= word & 255u;
            hash *= UINT64_C(1099511628211);
            word >>= 8;
        }
    }
    return hash;
}
static BenderSiteRow *bender_site_row(BenderFuncDef *def) {
    uint64_t id = def->site_profile_id;
    /* Boot-image and unmarshaled defs bypass bender_funcdef_alloc. Assign
     * their identity at first execution; this thread owns the VM heap. */
    if (!id) {
        id = bender_site_profile_new_id();
        if (!id) { bender_site_saturate(&bender_site_dropped); return NULL; }
        def->site_profile_id = id;
    }
    if (id == bender_site_last_id) return bender_site_last;
    for (size_t i = 0; i < bender_site_count; ++i)
        if (bender_site_rows[i].id == id) {
            bender_site_last_id = id;
            return bender_site_last = &bender_site_rows[i];
        }
    if (bender_site_count == BENDER_SITE_MAX_DEFS) {
        bender_site_saturate(&bender_site_dropped);
        bender_site_last_id = id;
        return bender_site_last = NULL;
    }
    BenderSiteRow *row = &bender_site_rows[bender_site_count++];
    row->id = id;
    row->words = def->bytecode_length;
    row->fingerprint = bender_site_fingerprint(def);
    for (int32_t i = 0; i < row->words && i < BENDER_SITE_MAX_PC; ++i)
        row->op[i] = (uint8_t)def->bytecode[i];
    bender_site_name(row->source, def->source);
    bender_site_name(row->name, def->name);
    bender_site_last_id = id;
    return bender_site_last = row;
}
static void bender_site_tick(BenderFunction *func, uint32_t *pc, int op_id) {
    if (!bender_site_enabled || !func || !func->def || !func->def->bytecode || op_id > 255) return;
    BenderFuncDef *def = func->def;
    ptrdiff_t offset = pc - def->bytecode;
    if (offset < 0 || offset >= def->bytecode_length || offset >= BENDER_SITE_MAX_PC) {
        bender_site_saturate(&bender_site_dropped);
        return;
    }
    BenderSiteRow *row = bender_site_row(def);
    if (!row) return;
    bender_site_saturate(&row->total);
    bender_site_saturate(&row->pc[offset]);
    if (op_id == JOP_CALL || op_id == JOP_TAILCALL)
        bender_site_saturate(&row->call[offset]);
}
#define BENDER_SITE_TICK(op) bender_site_tick(func, pc, (op))
static void bender_site_dump(void) {
    if (!bender_site_enabled) return;
    fprintf(stderr, "ttfx-site-profile defs=%zu dropped=%" PRIu64 "\n", bender_site_count, bender_site_dropped);
    for (size_t i = 0; i < bender_site_count; ++i) {
        BenderSiteRow *row = &bender_site_rows[i];
        if (!row->total) continue;
        fprintf(stderr, "site-def=%zu source=%s name=%s words=%d hash=%016" PRIx64 " total=%" PRIu64 "\n",
                i, row->source, row->name, row->words, row->fingerprint, row->total);
        for (int pc = 0; pc < row->words && pc < BENDER_SITE_MAX_PC; ++pc) {
            if (row->pc[pc]) fprintf(stderr, "site=%zu pc=%d op=%u count=%" PRIu64 " calls=%" PRIu64 "\n",
                i, pc, row->op[pc], row->pc[pc], row->call[pc]);
        }
    }
}
#else
#define BENDER_SITE_TICK(op) ((void)0)
#endif

/* Commit and restore VM state before possible longjmp */
#define vm_commit() do { bender_stack_frame(stack)->pc = pc; } while (0)
#define vm_restore() do { \
    stack = fiber->data + fiber->frame; \
    pc = bender_stack_frame(stack)->pc; \
    func = bender_stack_frame(stack)->func; \
} while (0)
#define vm_return(sig, val) do { \
    bender_vm.return_reg[0] = (val); \
    vm_commit(); \
    return (sig); \
} while (0)
#define vm_return_no_restore(sig, val) do { \
    bender_vm.return_reg[0] = (val); \
    return (sig); \
} while (0)

/* Next instruction variations */
#define BENDER_FIBER_MINOR_SCALE 256
#define BENDER_FULL_SCALE 1024
#define maybe_collect() do {\
    BenderFiber *mcf_ = bender_vm.fiber; \
    /* Check global major pressure before the scaled nursery branches. */ \
    if (BENDER_UNLIKELY(bender_vm.next_collection >= bender_vm.gc_interval \
            + bender_vm.gc_major_live_blocks * BENDER_FULL_SCALE)) bender_collect(); \
    else { \
        /* The scoped branch must not CONSUME the chain when its inner scaled
         * test declines: the outer test only says the fiber is above the BASE
         * threshold. Leaving it as an else-if made the global minor
         * unreachable during any spawn burst (measured: 0 calls at every N),
         * so pages never aged and every scoped minor re-marked the whole live
         * set. This is the same defect 7a7d7623 fixed for the major branch
         * above; mcf_scoped_ now records whether a collection actually ran. */ \
        /* The scaled term is proportional to the live set with NO ceiling,
         * and is read from the fiber's own SNAPSHOT (gc_minor_blocks, taken
         * at the end of its last scoped collection) rather than from a live
         * block_count. Both properties are load-bearing:
         *
         * No ceiling: a scoped minor costs Theta(live blocks reachable from
         * the collecting fiber), so bounding collection work per unit of
         * allocation -- the whole basis for amortising a collector --
         * requires the trigger to stay proportional to that live set. An
         * absolute clamp (this was 16 MiB) freezes the trigger while the
         * per-collection cost keeps growing, so the number of collections
         * stays proportional to total allocation and each costs Theta(N);
         * the product is the measured Theta(N^2) in proc spawn. Measured on
         * bench/cross-runtime/wave.ben with BENDER_LANES=11: every
         * lane-dispatcher minor fired at nursery == 16,777,216 bytes (the
         * clamp, exactly) while that lane's block_count reached 1,172,572 at
         * N=200,000 -- an unclamped trigger of ~300 MB, so the clamp was
         * truncating the amplifier by 18x and rising. The clamped build did
         * 5 whole-live-set walks at N=50,000 and 17 at N=200,000, which is
         * the quadratic term and 88% of all fiber-mark work.
         *
         * Snapshot, not live count: scaling by a block_count read HERE lets
         * a fiber that retains what it allocates grow the right-hand side at
         * least as fast as its own nursery grows the left (SCALE is 256
         * against a mean block of ~48 bytes), making the inequality
         * unsatisfiable and the scoped collector unreachable -- the defect
         * class b0b90b6c fixed for the global minor. suite-process-heaps:108
         * asserts exactly this reachability and does catch it. A snapshot is
         * constant between two collections while gc_nursery_bytes only
         * grows, so the trigger is always crossed after a bounded amount of
         * the fiber's own allocation.
         *
         * Only WHEN a collection runs changes; never what is marked or
         * swept. So this carries none of the soundness risk of the page-
         * aging and serials-pruning attempts recorded in
         * docs/internals/proc-spawn-scaling.md. The trade is memory
         * headroom, which stays proportional to the live set rather than
         * unbounded, and the global major branch above still bounds total
         * footprint; peak RSS is an acceptance gate. */ \
        int mcf_scoped_ = 0; \
        if (BENDER_UNLIKELY(NULL != mcf_ && mcf_->gc_nursery_bytes >= bender_vm.gc_minor_threshold)) { \
            size_t fth_ = bender_vm.gc_minor_threshold; \
            if (!(mcf_->heap_strict && NULL != mcf_->proc)) { \
                fth_ += (size_t) mcf_->gc_minor_blocks * BENDER_FIBER_MINOR_SCALE; } \
            if (mcf_->gc_nursery_bytes >= fth_) { \
                if (mcf_->heap_strict && NULL != mcf_->proc) bender_collect_process(mcf_->proc); \
                else bender_collect_fiber_minor(mcf_); \
                mcf_scoped_ = 1; } } \
        if (!mcf_scoped_ && BENDER_UNLIKELY(bender_vm.gc_young_bytes >= bender_vm.gc_minor_threshold \
                + (size_t) bender_vm.block_count * BENDER_FIBER_MINOR_SCALE)) bender_collect_minor(); } } while (0)
#define vm_checkgc_next() maybe_collect(); vm_next()
#define vm_pcnext() pc++; vm_next()
#define vm_checkgc_pcnext() maybe_collect(); vm_pcnext()

/* Handle certain errors in main vm loop */
#define vm_throw(e) do { vm_commit(); bender_panic(e); } while (0)
#define vm_assert(cond, e) do {if (!(cond)) vm_throw((e)); } while (0)
#define vm_assert_type(X, T) do { \
    if (!(bender_checktype((X), (T)))) { \
        vm_commit(); \
        bender_panicf("expected %T, got %v", (1 << (T)), (X)); \
    } \
} while (0)
#define vm_assert_types(X, TS) do { \
    if (!(bender_checktypes((X), (TS)))) { \
        vm_commit(); \
        bender_panicf("expected %T, got %v", (TS), (X)); \
    } \
} while (0)
#ifdef BENDER_NATIVE_EXECUTORS
static BENDER_THREAD_LOCAL int native_stepping;
/* Flat native-to-native trampoline (defined below, next to the call-enter
 * and native-return primitives it drives). It runs executors back to back
 * across pushed and popped FIBER frames without ever recursing in C, so a
 * chain of native calls still occupies exactly one C frame: no new
 * stack-overflow class, no bender_vm.stackn pressure, no pinned C frame
 * across a yield. On return, the current frame may be a different one than
 * on entry, which is why vm_native() below reloads `func` as well as
 * `stack` and `pc`. */
static BenderNativeStep bender_native_trampoline(BenderFiber *f, BenderFuncDef *d, uint32_t **pc);
/* Region gating and lazy structural attachment. Whole-definition
 * validation (bender_native_matches) runs at useful native region
 * starts recorded in the descriptor mask; consecutive VM-only
 * instructions after a boundary exit cost one bitmap check. The
 * verdict is cached per definition at the current validation epoch
 * (bumped at every resume entry, breakpoint write and publication),
 * so a long run pays full validation once per definition per resume
 * while any change to a definition between resumes is re-detected.
 * Lazy attachment tries the unique key-matching registered descriptor
 * once per registry generation per definition. */
#define vm_native() do { \
    if (BENDER_UNLIKELY(bender_vm.native_possible) && !native_stepping) { \
        BenderFuncDef *ndef_ = func->def; \
        if (NULL != ndef_->native_code) { \
            if (bender_native_region_allowed(ndef_->native_code, (uint32_t)(pc - ndef_->bytecode))) { \
                if ((ndef_->native_verified != ndef_->native_code || \
                        ndef_->native_verified_epoch != bender_vm.native_epoch) && \
                        !bender_native_revalidate(ndef_)) break; \
                uint32_t *npc = pc; \
                vm_commit(); \
                BenderNativeStep result = bender_native_trampoline(fiber, ndef_, &npc); \
                pc = npc; \
                stack = fiber->data + fiber->frame; \
                /* The trampoline can leave us in a DIFFERENT frame (it \
                 * chains calls and returns). `func` must be reloaded or \
                 * the interpreter would run the new frame's bytecode \
                 * against the old definition's constants. */ \
                func = bender_stack_frame(stack)->func; \
                vm_commit(); \
                if (result == BENDER_NATIVE_VM) { \
                    bender_vm.native_vm_exits++; \
                    bender_vm.native_exit_opcodes[*pc & 0xFF]++; \
                } else { \
                    fiber->flags |= BENDER_FIBER_RESUME_NO_USEVAL | BENDER_FIBER_RESUME_NO_SKIP; \
                    vm_return(BENDER_SIGNAL_INTERRUPT, bender_wrap_nil()); \
                } \
            } \
        } else if (bender_native_registry_count() && \
                ndef_->native_attach_gen != bender_native_registry_generation()) { \
            bender_native_try_attach(ndef_); \
        } \
    } \
} while (0)
#else
#define vm_native() ((void)0)
#endif

/* Keep the disabled interpreter path inline and unchanged. Both paths use
 * exactly this expression, including auto_suspend's short circuit. */
#define vm_poll_condition(COND) ((COND) && (bender_atomic_load_relaxed(&bender_vm.auto_suspend) || \
            (bender_vm.quantum && --bender_vm.quantum_remaining <= 0)))
int bender_native_poll(int condition) {
#ifdef BENDER_NO_INTERPRETER_INTERRUPT
    (void)condition;
    return 0;
#else
    return vm_poll_condition(condition);
#endif
}

/* Native executors may call an installed FFI alias wrapper directly only
 * under the exact conditions of the VM's alias fast path: function callee,
 * runtime alias marker, no trace hook, no active profiler, matching argc,
 * and a one-constant cfunction body. Return NULL for every other callee so
 * the executor exits at the call PC and ordinary JOP_CALL owns it. */
BENDER_API BenderCFunction bender_native_alias_cfun(Bender callee, int32_t argc) {
    if (!bender_checktype(callee, BENDER_FUNCTION)) return NULL;
    BenderFunction *func = bender_unwrap_function(callee);
    BenderFuncDef *def = func->def;
    if (func->gc.flags & BENDER_FUNCFLAG_TRACE) return NULL;
    if (NULL != bender_vm.profile_table) return NULL;
    if (!(def->flags & BENDER_FUNCDEF_FLAG_FFI_ALIAS)) return NULL;
    if (def->arity != argc) return NULL;
    if (def->constants_length != 1) return NULL;
    if (!bender_checktype(def->constants[0], BENDER_CFUNCTION)) return NULL;
    return bender_unwrap_cfunction(def->constants[0]);
}

/* Enter an already-validated alias wrapper with JOP_CALL's C-frame
 * discipline. The argument stack remains GC-scanned until the wrapper
 * returns; a signature without function arguments does NOT prove that
 * a C function cannot allocate or call back into Bender. */
BENDER_API Bender bender_native_alias_call(BenderFiber *fiber, BenderCFunction cfun,
                                        int32_t argc, uint32_t *pc) {
    bender_stack_frame(fiber->data + fiber->frame)->pc = pc;
    bender_fiber_cframe(fiber, cfun);
    Bender ret = cfun(argc, fiber->data + fiber->frame);
    bender_fiber_popframe(fiber);
    return ret;
}

/* Keep error/panic locations exact when a fast executor operation can
 * raise (notably the generational barrier's strict-heap checks). */
BENDER_API void bender_native_set_pc(BenderFiber *fiber, uint32_t *pc) {
    bender_stack_frame(fiber->data + fiber->frame)->pc = pc;
}

/* The caller has already stored the result in a scanned frame slot. Do the
 * same GC check as JOP_CALL before leaving the effectful native region.
 * Collection/finalizers and arbitrary C callbacks can invalidate a cached
 * structural verdict, so the next native entry must revalidate all words. */
BENDER_API void bender_native_alias_after_call(void) {
    maybe_collect();
    bender_vm.native_epoch++;
}

/* ---- Native-to-native continuation (v1): call-enter and native return ----
 *
 * The contract is stated in bender.h. Everything below exists to make
 * `bender_native_call_enter` ALL-OR-NOTHING: every refusal returns before the
 * first byte of fiber state changes, so the executor's ordinary exit at the
 * call PC leaves the interpreter to perform the call exactly once. */

BENDER_API int bender_native_call_enter(BenderFiber *fiber, Bender callee, int32_t argc,
                                      uint32_t *callpc, uint32_t **out_npc) {
#ifdef BENDER_NATIVE_EXECUTORS
    /* Runtime off-switch first: one load, and it makes the whole feature
     * disappear without rebuilding. */
    if (!bender_vm.native_continuation) goto refuse;
    /* The debugger bypasses executors entirely (see the resume entry);
     * a chained frame must not be able to step around a breakpoint. */
    if (fiber->flags & BENDER_FIBER_BREAKPOINT) goto refuse;
    /* Callee identity: keywords resolve through resolve_method, cfunctions
     * and non-functions take their own JOP_CALL branches. */
    if (!bender_checktype(callee, BENDER_FUNCTION)) goto refuse;
    {
        BenderFunction *callee_func = bender_unwrap_function(callee);
        BenderFuncDef *def = callee_func->def;
        /* Trace and profile hooks must observe every call. A profiling build
         * therefore never takes this path, which is also what keeps
         * auto-PGO evidence (and :calls) exact. */
        if (callee_func->gc.flags & BENDER_FUNCFLAG_TRACE) goto refuse;
        if (NULL != bender_vm.profile_table) goto refuse;
        /* No vararg tuple construction, no environment capture, no struct or
         * named argument rewriting: funcframe's remaining work is then a
         * capacity check, a nil fill and the frame header. */
        if (def->flags & (BENDER_FUNCDEF_FLAG_VARARG |
                          BENDER_FUNCDEF_FLAG_NEEDSENV |
                          BENDER_FUNCDEF_FLAG_STRUCTARG |
                          BENDER_FUNCDEF_FLAG_NAMEDARGS)) goto refuse;
        if (def->environments_length != 0) goto refuse;
        /* funcframe's arity gate, pre-checked so it cannot fail after we
         * commit. `&opt` parameters give min_arity < max_arity with no
         * VARARG flag and omitted optionals are nil-filled by funcframe,
         * exactly as JOP_CALL does -- so this must NOT demand fixed arity. */
        if (argc < def->min_arity || argc > def->max_arity) goto refuse;
        /* The callee must have a descriptor that is enterable at PC 0 and
         * proven for the current validation epoch. bender_native_revalidate
         * stays the sole authority; the cache only skips re-proving while
         * nothing can have changed. */
        if (NULL == def->native_code) goto refuse;
        if (!bender_native_region_allowed(def->native_code, 0)) goto refuse;
        if ((def->native_verified != def->native_code ||
                def->native_verified_epoch != bender_vm.native_epoch) &&
                !bender_native_revalidate(def)) goto refuse;
        /* Depth, exactly as JOP_CALL checks it (vm_maybe_overflow and the
         * maxstack throw). Both are VM-owned error paths: refuse and let the
         * interpreter raise them in its own place. */
        if (fiber->stack_overflowed) goto refuse;
        if (fiber->stacktop > fiber->maxstack) goto refuse;
        /* Everything is proven. From here the edge is ours, so poll exactly
         * once -- JOP_CALL's own vm_maybe_auto_suspend(1) is the poll this
         * replaces, and it is skipped on this path. Nothing has been
         * mutated yet, so an interrupt leaves the argument stack intact and
         * the VM re-executes the call at its own PC. */
        if (bender_native_poll(1)) return 2;
        /* Commit the caller's PC before the push: bender_native_ret_pop and
         * every stack trace read it from the frame, and a panic inside the
         * callee must report the call site, not a stale PC. */
        bender_stack_frame(fiber->data + fiber->frame)->pc = callpc;
        if (bender_fiber_funcframe(fiber, callee_func)) {
            /* Unreachable: the arity gate above is funcframe's own test and
             * funcframe checks arity before mutating any state. Treat it as
             * a refusal anyway rather than trusting the comment. */
            goto refuse;
        }
        bender_vm.native_continuations++;
        *out_npc = def->bytecode;
        /* JOP_CALL's vm_checkgc_next(). Without it a long native chain is an
         * unbounded nursery-growth window. */
        maybe_collect();
        return 1;
    }
refuse:
    bender_vm.native_continuation_refusals++;
    return 0;
#else
    (void) fiber;
    (void) callee;
    (void) argc;
    (void) callpc;
    (void) out_npc;
    return 0;
#endif
}

BENDER_API int bender_native_ret_pop(BenderFiber *fiber, uint32_t **pc) {
#ifdef BENDER_NATIVE_EXECUTORS
    if (!bender_vm.native_continuation) return 0;
    if (fiber->flags & BENDER_FIBER_BREAKPOINT) return 0;
    /* bender_profile_return must run inside the VM. */
    if (NULL != bender_vm.profile_table) return 0;
    {
        uint32_t word = **pc;
        Bender retval;
        if ((word & 0xFF) == JOP_RETURN) {
            retval = (fiber->data + fiber->frame)[(word >> 8) & 0xFFFFFF];
        } else if ((word & 0xFF) == JOP_RETURN_NIL) {
            retval = bender_wrap_nil();
        } else {
            return 0;
        }
        {
            Bender *s = fiber->data + fiber->frame;
            BenderStackFrame *frame = bender_stack_frame(s);
            /* Everything is decided BEFORE the pop, so a refusal leaves the
             * fiber untouched and JOP_RETURN performs the return itself.
             *
             * An ENTRANCE frame belongs to bender_call: returning from it is a
             * signal out of run_vm, not a frame pop. A TAILCALL frame's
             * caller word is a JOP_TAILCALL and its own caller frame is not
             * the one this return unwinds to. The root frame has no caller. */
            if (frame->flags & (BENDER_STACKFRAME_ENTRANCE | BENDER_STACKFRAME_TAILCALL)) return 0;
            if (fiber->frame == 0) return 0;
            /* prevframe 0 is "no caller frame": its header would sit at a
             * negative stack index. Valid frame bases are >= BENDER_FRAME_SIZE. */
            if (frame->prevframe < BENDER_FRAME_SIZE) return 0;
            {
                BenderStackFrame *cframe =
                    (BenderStackFrame *)(fiber->data + frame->prevframe - BENDER_FRAME_SIZE);
                uint32_t *cpc = cframe->pc;
                /* A C frame has no bytecode to resume into. */
                if (NULL == cframe->func || NULL == cpc) return 0;
                /* The caller's committed word decides the result slot. Refuse
                 * anything that is not a plain JOP_CALL -- a breakpoint bit,
                 * a resumed signal op, an unexpected shape: the VM owns it. */
                if ((*cpc & 0xFF) != JOP_CALL) return 0;
                bender_fiber_popframe(fiber);
                (fiber->data + fiber->frame)[(*cpc >> 8) & 0xFF] = retval;
                cframe->pc = cpc + 1;
                *pc = cpc + 1;
            }
        }
        bender_vm.native_returns++;
        return 1;
    }
#else
    (void) fiber;
    (void) pc;
    return 0;
#endif
}

#ifdef BENDER_NATIVE_EXECUTORS
/* Bound on frames chained inside one trampoline entry. A native recursion
 * must not be able to monopolize the quantum or build an unbounded chain
 * between interpreter dispatches; on exhaustion we simply return at the
 * current unexecuted PC in a real frame, which run_vm already accepts. */
#define BENDER_NATIVE_CHAIN_BUDGET 64

/* The loop referenced by vm_native(). Preconditions, established by the
 * caller: `d` is the current frame's definition, it has a descriptor, the
 * descriptor allows entry at *pc, and the structural verdict is proven for
 * the current epoch. It holds NO state across an executor call that a
 * longjmp would have to unwind (no gcroot, no lock, no counter needing a
 * decrement), so a panic inside a chained callee unwinds through this one C
 * frame as an ordinary two-frame Bender chain. */
static BenderNativeStep bender_native_trampoline(BenderFiber *f, BenderFuncDef *d, uint32_t **pc) {
    int budget = BENDER_NATIVE_CHAIN_BUDGET;
    for (;;) {
        BenderNativeStep step = d->native_code->execute(f, d, pc);
        if (step == BENDER_NATIVE_INTERRUPT) return step;
        if (step == BENDER_NATIVE_FRAME) {
            /* Chain DOWN: bender_native_call_enter already pushed the callee
             * frame and did JOP_CALL's GC check. */
        } else {
            /* Chain UP: the executor stopped at an unexecuted ret/retn.
             * ret_pop refuses (touching nothing) for anything the VM must
             * own, in which case this is an ordinary boundary exit. */
            if (!bender_native_ret_pop(f, pc)) return BENDER_NATIVE_VM;
            /* JOP_RETURN's vm_checkgc_pcnext(). */
            maybe_collect();
        }
        if (--budget <= 0) return BENDER_NATIVE_VM;
        /* Re-gate for the new current frame. The verdict cache makes this
         * two comparisons unless the epoch moved -- and if a collection or
         * finalizer moved it, re-proving is exactly what must happen. */
        d = bender_stack_frame(f->data + f->frame)->func->def;
        if (NULL == d->native_code) return BENDER_NATIVE_VM;
        if (!bender_native_region_allowed(d->native_code, (uint32_t)(*pc - d->bytecode)))
            return BENDER_NATIVE_VM;
        if ((d->native_verified != d->native_code ||
                d->native_verified_epoch != bender_vm.native_epoch) &&
                !bender_native_revalidate(d)) return BENDER_NATIVE_VM;
    }
}
#endif

#ifdef BENDER_NO_INTERPRETER_INTERRUPT
#define vm_maybe_auto_suspend(COND)
#else
#define vm_maybe_auto_suspend(COND) do { \
    if (vm_poll_condition(COND)) { \
        fiber->flags |= (BENDER_FIBER_RESUME_NO_USEVAL | BENDER_FIBER_RESUME_NO_SKIP); \
        vm_return(BENDER_SIGNAL_INTERRUPT, bender_wrap_nil()); \
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
        bender_panic("fiber native stack overflow"); \
    } \
} while (0)

/* Templates for certain patterns in opcodes */
#define vm_binop_immediate(op)\
    {\
        Bender op1 = stack[B];\
        if (BENDER_UNLIKELY(!bender_checktype(op1, BENDER_NUMBER))) {\
            vm_commit();\
            Bender _argv[2] = { op1, bender_wrap_number(CS) };\
            Bender a = bender_mcall(#op, 2, _argv);\
            stack = fiber->data + fiber->frame;\
            stack[A] = a;\
            vm_checkgc_pcnext();\
        } else {\
            double x1 = bender_unwrap_number(op1);\
            stack[A] = bender_wrap_number(x1 op CS);\
            vm_pcnext();\
        }\
    }
#define _vm_bitop_immediate(op, type1, rangecheck, msg)\
    {\
        Bender op1 = stack[B];\
        if (BENDER_UNLIKELY(!bender_checktype(op1, BENDER_NUMBER))) {\
            vm_commit();\
            Bender _argv[2] = { op1, bender_wrap_number(CS) };\
            Bender a = bender_mcall(#op, 2, _argv);\
            stack = fiber->data + fiber->frame;\
            stack[A] = a;\
            vm_checkgc_pcnext();\
        } else {\
            double y1 = bender_unwrap_number(op1);\
            if (!rangecheck(y1)) { vm_commit(); bender_panicf("value %v out of range for " msg, op1); }\
            type1 x1 = (type1) y1;\
            stack[A] = bender_wrap_number((type1) (x1 op CS));\
            vm_pcnext();\
        }\
    }
/* Unsigned-immediate bitop (JINT_SSU). Identical to
 * _vm_bitop_immediate except the immediate is read via CU (0..255)
 * instead of the sign-extended CS, so masks above 127 work. The
 * non-number path still goes through bender_mcall with the same wrapped
 * immediate, so a non-numeric operand behaves exactly as the register
 * form does. */
#define _vm_bitop_uimmediate(op, type1, rangecheck, msg)\
    {\
        Bender op1 = stack[B];\
        if (BENDER_UNLIKELY(!bender_checktype(op1, BENDER_NUMBER))) {\
            vm_commit();\
            Bender _argv[2] = { op1, bender_wrap_number(CU) };\
            Bender a = bender_mcall(#op, 2, _argv);\
            stack = fiber->data + fiber->frame;\
            stack[A] = a;\
            vm_checkgc_pcnext();\
        } else {\
            double y1 = bender_unwrap_number(op1);\
            if (!rangecheck(y1)) { vm_commit(); bender_panicf("value %v out of range for " msg, op1); }\
            type1 x1 = (type1) y1;\
            stack[A] = bender_wrap_number((type1) (x1 op (type1) CU));\
            vm_pcnext();\
        }\
    }
#define vm_bitop_uimmediate(op) _vm_bitop_uimmediate(op, int32_t, bender_checkintrange, "32-bit signed integers");
#define vm_bitop_immediate(op) _vm_bitop_immediate(op, int32_t, bender_checkintrange, "32-bit signed integers");
#define vm_bitopu_immediate(op) _vm_bitop_immediate(op, uint32_t, bender_checkuintrange, "32-bit unsigned integers");
#define _vm_binop(op, wrap)\
    {\
        Bender op1 = stack[B];\
        Bender op2 = stack[C];\
        if (BENDER_LIKELY(bender_checktype(op1, BENDER_NUMBER) && bender_checktype(op2, BENDER_NUMBER))) {\
            double x1 = bender_unwrap_number(op1);\
            double x2 = bender_unwrap_number(op2);\
            stack[A] = wrap(x1 op x2);\
            vm_pcnext();\
        } else {\
            vm_commit();\
            Bender a = bender_binop_call(#op, "r" #op, op1, op2);\
            stack = fiber->data + fiber->frame;\
            stack[A] = a;\
            vm_checkgc_pcnext();\
        }\
    }
#define vm_binop(op) _vm_binop(op, bender_wrap_number)
#define _vm_bitop(op, type1, rangecheck, msg)\
    {\
        Bender op1 = stack[B];\
        Bender op2 = stack[C];\
        if (BENDER_LIKELY(bender_checktype(op1, BENDER_NUMBER) && bender_checktype(op2, BENDER_NUMBER))) {\
            double y1 = bender_unwrap_number(op1);\
            double y2 = bender_unwrap_number(op2);\
            if (!rangecheck(y1)) { vm_commit(); bender_panicf("value %v out of range for " msg, op1); }\
            if (!bender_checkintrange(y2)) { vm_commit(); bender_panicf("rhs must be valid 32-bit signed integer, got %f", op2); }\
            type1 x1 = (type1) y1;\
            int32_t x2 = (int32_t) y2;\
            stack[A] = bender_wrap_number((type1) (x1 op x2));\
            vm_pcnext();\
        } else {\
            vm_commit();\
            Bender a = bender_binop_call(#op, "r" #op, op1, op2);\
            stack = fiber->data + fiber->frame;\
            stack[A] = a;\
            vm_checkgc_pcnext();\
        }\
    }
#define vm_bitop(op) _vm_bitop(op, int32_t, bender_checkintrange, "32-bit signed integers")
#define vm_bitopu(op) _vm_bitop(op, uint32_t, bender_checkuintrange, "32-bit unsigned integers")
#define vm_compop(op) \
    {\
        Bender op1 = stack[B];\
        Bender op2 = stack[C];\
        if (BENDER_LIKELY(bender_checktype(op1, BENDER_NUMBER) && bender_checktype(op2, BENDER_NUMBER))) {\
            double x1 = bender_unwrap_number(op1);\
            double x2 = bender_unwrap_number(op2);\
            stack[A] = bender_wrap_boolean(x1 op x2);\
            vm_pcnext();\
        } else {\
            vm_commit();\
            Bender a = bender_wrap_boolean(bender_compare(op1, op2) op 0);\
            stack = fiber->data + fiber->frame;\
            stack[A] = a;\
            vm_checkgc_pcnext();\
        }\
    }
#define vm_compop_imm(op) \
    {\
        Bender op1 = stack[B];\
        if (bender_checktype(op1, BENDER_NUMBER)) {\
            double x1 = bender_unwrap_number(op1);\
            double x2 = (double) CS; \
            stack[A] = bender_wrap_boolean(x1 op x2);\
            vm_pcnext();\
        } else {\
            vm_commit();\
            Bender a = bender_wrap_boolean(bender_compare(op1, bender_wrap_integer(CS)) op 0);\
            stack = fiber->data + fiber->frame;\
            stack[A] = a;\
            vm_checkgc_pcnext();\
        }\
    }

/* Trace a function call.
 * This is a macro to avoid stale argv if bender_eprintf resizes the stack
 */
#define vm_do_trace(func, argc, argv) do { \
    BenderFunction* _func = (func);\
    if (_func->def->name) {\
        bender_eprintf("trace (%S", _func->def->name);\
    } else {\
        bender_eprintf("trace (%p", bender_wrap_function(_func));\
    }\
    int32_t _argc = (argc);\
    for (int32_t i = 0; i < _argc; i++) {\
        bender_eprintf(" %p", (argv)[i]);\
    }\
    bender_eprintf(")\n");\
} while (0)

/* Invoke a method once we have looked it up */
static Bender bender_method_invoke(Bender method, int32_t argc, Bender *argv) {
    switch (bender_type(method)) {
        case BENDER_CFUNCTION:
            return (bender_unwrap_cfunction(method))(argc, argv);
        case BENDER_FUNCTION: {
            BenderFunction *fun = bender_unwrap_function(method);
            return bender_call(fun, argc, argv);
        }
        case BENDER_ABSTRACT: {
            BenderAbstract abst = bender_unwrap_abstract(method);
            const BenderAbstractType *at = bender_abstract_type(abst);
            if (NULL != at->call) {
                return at->call(abst, argc, argv);
            }
        }
        /* fallthrough */
        case BENDER_STRING:
        case BENDER_BUFFER:
        case BENDER_TABLE:
        case BENDER_STRUCT:
        case BENDER_ARRAY:
        case BENDER_TUPLE: {
            if (argc != 1) {
                bender_panicf("%v called with %d arguments, possibly expected 1", method, argc);
            }
            return bender_in(method, argv[0]);
        }
        default: {
            if (argc != 1) {
                bender_panicf("%v called with %d arguments, possibly expected 1", method, argc);
            }
            return bender_in(argv[0], method);
        }
    }
}

/* Call a non function type from a JOP_CALL or JOP_TAILCALL instruction.
 * Assumes that the arguments are on the fiber stack. */
static Bender call_nonfn(BenderFiber *fiber, Bender callee) {
    int32_t argc = fiber->stacktop - fiber->stackstart;
    fiber->stacktop = fiber->stackstart;
    return bender_method_invoke(callee, argc, fiber->data + fiber->stacktop);
}

/* Method lookup could potentially handle tables specially... */
static Bender method_to_fun(Bender method, Bender obj) {
    return bender_get(obj, method);
}

/* Get a callable from a keyword method name and ensure that it is valid. */
static Bender resolve_method(Bender name, BenderFiber *fiber) {
    int32_t argc = fiber->stacktop - fiber->stackstart;
    if (argc < 1) bender_panicf("method call (%v) takes at least 1 argument, got 0", name);
    Bender callee = method_to_fun(name, fiber->data[fiber->stackstart]);
    if (bender_checktype(callee, BENDER_NIL))
        bender_panicf("unknown method %v invoked on %v", name, fiber->data[fiber->stackstart]);
    return callee;
}

/* Lookup method on value x */
static Bender bender_method_lookup(Bender x, const char *name) {
    return method_to_fun(bender_ckeywordv(name), x);
}

static Bender bender_unary_call(const char *method, Bender arg) {
    Bender m = bender_method_lookup(arg, method);
    if (bender_checktype(m, BENDER_NIL)) {
        bender_panicf("could not find method :%s for %v", method, arg);
    } else {
        Bender argv[1] = { arg };
        return bender_method_invoke(m, 1, argv);
    }
}

/* Call a method first on the righthand side, and then on the left hand side with a prefix */
static Bender bender_binop_call(const char *lmethod, const char *rmethod, Bender lhs, Bender rhs) {
    Bender lm = bender_method_lookup(lhs, lmethod);
    if (bender_checktype(lm, BENDER_NIL)) {
        /* Invert order for rmethod */
        Bender lr = bender_method_lookup(rhs, rmethod);
        Bender argv[2] = { rhs, lhs };
        if (bender_checktype(lr, BENDER_NIL)) {
            bender_panicf("could not find method :%s for %v or :%s for %v",
                         lmethod, lhs,
                         rmethod, rhs);
        }
        return bender_method_invoke(lr, 2, argv);
    } else {
        Bender argv[2] = { lhs, rhs };
        return bender_method_invoke(lm, 2, argv);
    }
}

/* Forward declaration */
static BenderSignal bender_check_can_resume(BenderFiber *fiber, Bender *out, int is_cancel);
/* P3.6: walk the resume chain for shared-stack fibers parked mid-C
 * (child links verified intact — a stale link means that ancestor
 * already unwound normally). Self excluded (the suspender abandons
 * cleanly and re-primes). Bounded. */
static int bender_fiber_chain_has_shared(BenderFiber *fiber);static BenderSignal bender_continue_no_check(BenderFiber *fiber, Bender in, Bender *out);
static void bender_suspend_swap(BenderFiber *fiber, BenderSignal sig, Bender payload);

/* PGO type-profile collector (Phase 1). Records observed param/return
 * types per function while bender_vm.profile_table is non-NULL. Keyed by
 * BenderFuncDef* identity; each entry holds a strong function reference so
 * profiled defs survive. Masks are uint32 bit sets over BenderType (16
 * types fit). Zero cost when off: callers guard on the NULL table. */
#define BENDER_PROFILE_MAXPARAMS 8
#define BENDER_PROFILE_ELEMSAMPLE 8
/* Phase 4B: distinct observed VALUES per scalar atom param slot. A
 * slot observing more distinct values than this is marked open (the
 * consumer must refuse closed-set reasoning for it), never a huge
 * set. */
#define BENDER_PROFILE_VALSAMPLE 8
#define BENDER_PROFILE_SIGSAMPLE 8
/* Stream D step A: distinct observed table/struct KEY SETS per param
 * slot ("shapes"). ttfx-style record passing means most unprovable
 * params are tables carrying a fixed key set -- a struct wearing a
 * hash table's clothing. Recording the observed key sets lets the
 * build emit a real C struct with offset field access instead of
 * bender_table_get hash probes. Same discipline as VALSAMPLE above: a
 * slot observing more distinct shapes than this is marked OPEN and the
 * consumer must keep it a boxed BenderTable* -- exactly the correct
 * outcome for a genuine dictionary (dynamic keys), so real
 * dictionaries disqualify themselves automatically. */
#define BENDER_PROFILE_SHAPESAMPLE 4
/* Guard against pathological records: a key set wider than this is
 * treated as open rather than flattened (struct layout churn stops
 * being worth it, and it signals the value is used as a map). ttfx's
 * widest real record (canvas) is ~18 keys. */
#define BENDER_PROFILE_SHAPEMAXKEYS 24

/* Direct-mapped entry cache (auto-PGO): real programs alternate
 * between functions, so a last-entry cache thrashes (16x slowdown
 * observed on alternating calls); 16 lines keyed by def-pointer hash
 * hold the hot working set instead. Hits cost ~10 ALU ops (no
 * hashing); misses take the full path and refill (flushing the
 * evicted line first). Flush-all runs at every site that swaps or
 * reads the table (stop, reset, rows), so counts are never lost. The
 * non-moving collector keeps cached pointers valid while their table
 * is current, which the pt-identity check enforces. */
#define BENDER_PROFILE_CACHELINES 64
/* Bounded diagnostic branch evidence: no allocation or collection in a jump. */
#define BENDER_PROFILE_EDGE_MAXPC 256
typedef struct {
    BenderTable *pt;
    BenderFuncDef *def;
    BenderTable *e;
    int64_t calls;
    uint32_t rets;
    uint32_t tick;
    int live;
    BenderArray *pa;
    BenderArray *ea;
    BenderArray *mn;
    BenderArray *mx;
    BenderArray *va;
    BenderArray *sh;
    Bender sig;
    BenderArray *edges;
    BenderArray *receivers;
    BenderArray *pce;
} BenderProfileCache;
/* Phase C: the fast-path cache is per-thread. Worker VMs share this
 * translation unit across OS threads; a single global would let two
 * workers fill/use it concurrently (params landing in the wrong
 * function's arrays). Thread-local keeps every VM disjoint. Static
 * storage is zero-initialized. */
static BENDER_THREAD_LOCAL BenderProfileCache bender_profile_cache[BENDER_PROFILE_CACHELINES];

static uint32_t bender_profile_line(BenderFuncDef *def) {
    uintptr_t h = (uintptr_t) def;
    h ^= h >> 12;
    return (h >> 4) % BENDER_PROFILE_CACHELINES;
}

static void bender_profile_flush_line(uint32_t ln) {
    BenderProfileCache *c = &bender_profile_cache[ln];
    if (c->live) {
        bender_table_put(c->e, bender_ckeywordv("calls"),
                        bender_wrap_integer(c->calls));
        bender_table_put(c->e, bender_ckeywordv("rets"),
                        bender_wrap_integer((int64_t) c->rets));
        c->live = 0;
    }
}

static void bender_profile_flush(void) {
    uint32_t ln;
    for (ln = 0; ln < BENDER_PROFILE_CACHELINES; ln++)
        bender_profile_flush_line(ln);
}

static void bender_profile_cache_fill(BenderTable *pt, BenderFuncDef *def, BenderTable *e) {
    /* Miss path flushes first (per the collector contract): the live
     * entry's C-side calls/rets would otherwise be lost on eviction. */
    uint32_t ln = bender_profile_line(def);
    BenderProfileCache *c = &bender_profile_cache[ln];
    bender_profile_flush_line(ln);
    c->pt = pt;
    c->def = def;
    c->e = e;
    c->calls =
        bender_unwrap_integer(bender_table_get(e, bender_ckeywordv("calls")));
    c->rets =
        (uint32_t) bender_unwrap_integer(bender_table_get(e, bender_ckeywordv("rets")));
    c->pa = bender_unwrap_array(bender_table_get(e, bender_ckeywordv("params")));
    c->ea = bender_unwrap_array(bender_table_get(e, bender_ckeywordv("elems")));
    c->mn = bender_unwrap_array(bender_table_get(e, bender_ckeywordv("mins")));
    c->mx = bender_unwrap_array(bender_table_get(e, bender_ckeywordv("maxs")));
    c->va = bender_unwrap_array(bender_table_get(e, bender_ckeywordv("vals")));
    c->sh = bender_unwrap_array(bender_table_get(e, bender_ckeywordv("shapes")));
    c->sig = bender_table_get(e, bender_ckeywordv("signatures"));
    {
        Bender edge = bender_table_get(e, bender_ckeywordv("edges"));
        c->edges = bender_checktype(edge, BENDER_ARRAY) ? bender_unwrap_array(edge) : NULL;
        edge = bender_table_get(e, bender_ckeywordv("receivers"));
        c->receivers = bender_checktype(edge, BENDER_ARRAY) ? bender_unwrap_array(edge) : NULL;
        edge = bender_table_get(e, bender_ckeywordv("calledges"));
        c->pce = bender_checktype(edge, BENDER_ARRAY) ? bender_unwrap_array(edge) : NULL;
    }
    c->live = 1;
}

static BENDER_THREAD_LOCAL unsigned long long bender_profile_hits;
static BENDER_THREAD_LOCAL unsigned long long bender_profile_misses;

static BenderProfileCache *bender_profile_hit(BenderFunction *func) {
    BenderProfileCache *c = &bender_profile_cache[bender_profile_line(func->def)];
    if (c->live && c->pt == bender_vm.profile_table && c->def == func->def) {
        bender_profile_hits++;
        return c;
    }
    bender_profile_misses++;
    return NULL;
}

static BenderTable *bender_profile_entry(BenderFunction *func) {
    BenderTable *pt = bender_vm.profile_table;
    Bender key = bender_wrap_pointer(func->def);
    Bender v = bender_table_get(pt, key);
    if (bender_checktype(v, BENDER_NIL)) {
        int i;
        BenderTable *e = bender_table(4);
        BenderArray *pa;
        Bender eroot;
        /* Root until inserted: table_put can trigger a collection that
         * would otherwise sweep the still-floating entry. */
        eroot = bender_wrap_table(e);
        bender_gcroot(eroot);
        pa = bender_array(BENDER_PROFILE_MAXPARAMS);
        bender_gcroot(bender_wrap_array(pa));
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
        bender_table_put_raw(e, bender_ckeywordv("func"), bender_wrap_function(func));
        bender_table_put_raw(e, bender_ckeywordv("calls"), bender_wrap_integer(0));
        for (i = 0; i < BENDER_PROFILE_MAXPARAMS; i++)
            bender_array_push(pa, bender_wrap_integer(0));
        bender_table_put_raw(e, bender_ckeywordv("params"), bender_wrap_array(pa));
        {
            /* Parallel element-type masks: for array params, the OR of
             * the first elements' types (heuristic sample, see call
             * path). Zero for non-arrays. Evidence for SoA-kernel
             * shaping, not proof. */
            BenderArray *ea = bender_array(BENDER_PROFILE_ELEMSAMPLE);
            bender_gcroot(bender_wrap_array(ea));
            for (i = 0; i < BENDER_PROFILE_ELEMSAMPLE; i++)
                bender_array_push(ea, bender_wrap_integer(0));
            bender_table_put_raw(e, bender_ckeywordv("elems"), bender_wrap_array(ea));
            bender_gcunroot(bender_wrap_array(ea));
        }
        {
            /* Parallel min/max for number params (PGO range evidence:
             * unanimous min >= 0 seeds nonneg int-division shaping).
             * +inf/-inf sentinels mean unobserved (dumped as nil). */
            BenderArray *mn = bender_array(BENDER_PROFILE_MAXPARAMS);
            BenderArray *mx = bender_array(BENDER_PROFILE_MAXPARAMS);
            bender_gcroot(bender_wrap_array(mn));
            bender_gcroot(bender_wrap_array(mx));
            for (i = 0; i < BENDER_PROFILE_MAXPARAMS; i++) {
                bender_array_push(mn, bender_wrap_number(INFINITY));
                bender_array_push(mx, bender_wrap_number(-INFINITY));
            }
            bender_table_put_raw(e, bender_ckeywordv("mins"), bender_wrap_array(mn));
            bender_table_put_raw(e, bender_ckeywordv("maxs"), bender_wrap_array(mx));
            bender_gcunroot(bender_wrap_array(mn));
            bender_gcunroot(bender_wrap_array(mx));
        }
        {
            /* Phase 4B: closed-value-set evidence for scalar atom
             * (string/keyword/symbol) param slots. Per slot: nil =
             * unobserved, array = observed distinct values (bounded by
             * BENDER_PROFILE_VALSAMPLE), true = open (too many distinct
             * values observed -- closed-set reasoning must refuse).
             * Booleans can never be a recorded atom value, so `true`
             * is an unambiguous marker. Values stay alive through the
             * entry table exactly like the mask arrays. */
            BenderArray *va = bender_array(BENDER_PROFILE_MAXPARAMS);
            bender_gcroot(bender_wrap_array(va));
            for (i = 0; i < BENDER_PROFILE_MAXPARAMS; i++)
                bender_array_push(va, bender_wrap_nil());
            bender_table_put_raw(e, bender_ckeywordv("vals"), bender_wrap_array(va));
            bender_gcunroot(bender_wrap_array(va));
        }
        {
            /* Stream D step A: observed table/struct KEY SETS per slot.
             * Per slot: nil = unobserved, array of key-arrays = the
             * distinct observed shapes (bounded by
             * BENDER_PROFILE_SHAPESAMPLE), true = open (too many
             * distinct shapes, or a key set wider than
             * BENDER_PROFILE_SHAPEMAXKEYS, or a non-keyword key --
             * consumers must keep the slot a boxed table). Booleans can
             * never be a recorded shape, so `true` is unambiguous, the
             * same marker discipline as :vals above. */
            BenderArray *sh = bender_array(BENDER_PROFILE_MAXPARAMS);
            bender_gcroot(bender_wrap_array(sh));
            for (i = 0; i < BENDER_PROFILE_MAXPARAMS; i++)
                bender_array_push(sh, bender_wrap_nil());
            bender_table_put_raw(e, bender_ckeywordv("shapes"), bender_wrap_array(sh));
            bender_gcunroot(bender_wrap_array(sh));
        }
        bender_table_put_raw(e, bender_ckeywordv("rets"), bender_wrap_integer(0));
        /* Allocate once per profiled definition, never at a branch or poll.
         * Large definitions remain explicitly unobserved rather than having
         * their PCs aliased to unrelated sites. */
        if (func->def->bytecode_length <= BENDER_PROFILE_EDGE_MAXPC) {
            BenderArray *edges = bender_array(func->def->bytecode_length * 2);
            bender_gcroot(bender_wrap_array(edges));
            for (i = 0; i < func->def->bytecode_length * 2; i++)
                bender_array_push(edges, bender_wrap_integer(0));
            bender_table_put_raw(e, bender_ckeywordv("edges"), bender_wrap_array(edges));
            bender_gcunroot(bender_wrap_array(edges));
            /* One type-mask slot per original bytecode PC. This does not
             * infer a shape or authorize a specialization on its own. */
            BenderArray *receivers = bender_array(func->def->bytecode_length);
            bender_gcroot(bender_wrap_array(receivers));
            for (i = 0; i < func->def->bytecode_length; i++)
                bender_array_push(receivers, bender_wrap_integer(0));
            bender_table_put_raw(e, bender_ckeywordv("receivers"), bender_wrap_array(receivers));
            bender_gcunroot(bender_wrap_array(receivers));
            /* One call-edge attribution slot per original bytecode PC:
             * nil = unobserved, [calleeName total native] = exactly one
             * callee NAME observed at this PC, true = open (a second
             * distinct callee, or an unnamed one). Same discipline as
             * :elems above -- this is a per-PC heuristic sample that
             * decides WHERE a guard is emitted, never what a guard may
             * skip. It is NOT a type or identity proof: a name is not a
             * FuncDef, and the slot can be rebound between the profiled
             * run and the build. Consumers must still verify the callee
             * at run time (bender_native_call_enter does). */
            BenderArray *calledges = bender_array(func->def->bytecode_length);
            bender_gcroot(bender_wrap_array(calledges));
            for (i = 0; i < func->def->bytecode_length; i++)
                bender_array_push(calledges, bender_wrap_nil());
            bender_table_put_raw(e, bender_ckeywordv("calledges"), bender_wrap_array(calledges));
            bender_gcunroot(bender_wrap_array(calledges));
        }
        /* Joint, full-arity tag vectors; true denotes OPEN. The entry
         * owns this array and every nested vector across GC. */
        {
            BenderArray *hist = bender_array(0);
            bender_gcroot(bender_wrap_array(hist));
            bender_table_put_raw(e, bender_ckeywordv("signatures"), bender_wrap_array(hist));
            bender_gcunroot(bender_wrap_array(hist));
        }
        /* P3.4 bookkeeping put: pt is the profiler's identity map
         * (function-pointer keyed), the same class as the marshal
         * memo and threaded registry. Under strict procs the plain
         * put panicked (a child-allocated entry is isolated, pt is
         * VM-global shared) — killing every strict proc in AOT
         * binaries, which auto-PGO instruments. put_raw keeps
         * generational-dirty marking (minors stay sound) and skips
         * the barrier/export panic. */
        bender_table_put_raw(pt, key, bender_wrap_table(e));
        bender_gcunroot(bender_wrap_array(pa));
        bender_gcunroot(eroot);
        bender_profile_cache_fill(pt, func->def, e);
        return e;
    }
    bender_profile_cache_fill(pt, func->def, bender_unwrap_table(v));
    return bender_unwrap_table(v);
}

static void bender_profile_call(BenderFunction *func, const Bender *argv, int32_t argc) {
    int32_t n, i;
    BenderArray *pa, *ea, *mn, *mx, *va, *sh;
    BenderProfileCache *hit = bender_profile_hit(func);
    if (NULL != hit) {
        pa = hit->pa;
        ea = hit->ea;
        mn = hit->mn;
        mx = hit->mx;
        va = hit->va;
        sh = hit->sh;
        hit->calls++;
    } else {
        BenderTable *e = bender_profile_entry(func);
        (void) e;
        hit = bender_profile_hit(func);
        pa = hit->pa;
        ea = hit->ea;
        mn = hit->mn;
        mx = hit->mx;
        va = hit->va;
        sh = hit->sh;
        hit->calls++;
    }
    n = argc < BENDER_PROFILE_MAXPARAMS ? argc : BENDER_PROFILE_MAXPARAMS;
    if (!bender_checktype(hit->sig, BENDER_BOOLEAN)) {
        if (argc < 0 || argc > BENDER_PROFILE_MAXPARAMS) {
            hit->sig = bender_wrap_true();
            bender_table_put_raw(hit->e, bender_ckeywordv("signatures"), hit->sig);
        } else {
            BenderArray *hist = bender_unwrap_array(hit->sig);
            int32_t k, found = 0;
            for (k = 0; k < hist->count; k++) {
                BenderArray *entry = bender_unwrap_array(hist->data[k]);
                BenderArray *tags = bender_unwrap_array(entry->data[0]);
                int32_t j;
                if (tags->count != argc) continue;
                for (j = 0; j < argc; j++)
                    if ((uint32_t) bender_unwrap_integer(tags->data[j]) !=
                        (uint32_t) bender_type(argv[j])) break;
                if (j == argc) {
                    entry->data[1] = bender_wrap_integer(bender_unwrap_integer(entry->data[1]) + 1);
                    found = 1;
                    break;
                }
            }
            if (!found) {
                if (hist->count >= BENDER_PROFILE_SIGSAMPLE) {
                    hit->sig = bender_wrap_true();
                    bender_table_put_raw(hit->e, bender_ckeywordv("signatures"), hit->sig);
                } else {
                    BenderArray *entry = bender_array(2);
                    BenderArray *tags;
                    bender_gcroot(bender_wrap_array(entry));
                    tags = bender_array(argc);
                    bender_gcroot(bender_wrap_array(tags));
                    for (k = 0; k < argc; k++)
                        bender_array_push(tags, bender_wrap_integer(bender_type(argv[k])));
                    bender_array_push(entry, bender_wrap_array(tags));
                    bender_array_push(entry, bender_wrap_integer(1));
                    bender_array_push(hist, bender_wrap_array(entry));
                    bender_gcunroot(bender_wrap_array(tags));
                    bender_gcunroot(bender_wrap_array(entry));
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
        uint32_t at = (uint32_t) bender_type(argv[i]);
        uint32_t m = (uint32_t) bender_unwrap_integer(pa->data[i]);
        m |= (1u << at);
        pa->data[i] = bender_wrap_integer((int64_t) m);
        if (full && at == (uint32_t) BENDER_NUMBER) {
            double v = bender_unwrap_number(argv[i]);
            if (v < bender_unwrap_number(mn->data[i])) mn->data[i] = bender_wrap_number(v);
            if (v > bender_unwrap_number(mx->data[i])) mx->data[i] = bender_wrap_number(v);
        }
        if (full && at == (uint32_t) BENDER_ARRAY) {
            BenderArray *av = bender_unwrap_array(argv[i]);
            int32_t k, nk = av->count < BENDER_PROFILE_ELEMSAMPLE ? av->count : BENDER_PROFILE_ELEMSAMPLE;
            uint32_t em = (uint32_t) bender_unwrap_integer(ea->data[i]);
            for (k = 0; k < nk; k++)
                em |= (1u << (uint32_t) bender_type(av->data[k]));
            ea->data[i] = bender_wrap_integer((int64_t) em);
        }
        if (full && (at == (uint32_t) BENDER_STRING ||
                     at == (uint32_t) BENDER_SYMBOL ||
                     at == (uint32_t) BENDER_KEYWORD)) {
            /* Phase 4B: sample the concrete VALUE for scalar atom
             * params (same 16-exact-then-1-in-8 discipline as the
             * element/min-max evidence). bender_equals is content
             * equality for strings and pointer equality for interned
             * keywords/symbols -- exactly Bender's own equality. The
             * freshly built vs array is rooted across its pushes and
             * becomes reachable through the entry table on store. */
            Bender slotv = va->data[i];
            if (!bender_checktype(slotv, BENDER_BOOLEAN)) {
                BenderArray *vs = bender_checktype(slotv, BENDER_ARRAY)
                    ? bender_unwrap_array(slotv) : NULL;
                int32_t k, found = 0;
                if (NULL != vs)
                    for (k = 0; k < vs->count; k++)
                        if (bender_equals(vs->data[k], argv[i])) {
                            found = 1;
                            break;
                        }
                if (!found) {
                    if (NULL == vs) {
                        vs = bender_array(BENDER_PROFILE_VALSAMPLE);
                        bender_gcroot(bender_wrap_array(vs));
                        bender_array_push(vs, argv[i]);
                        va->data[i] = bender_wrap_array(vs);
                        bender_gcunroot(bender_wrap_array(vs));
                    } else if (vs->count < BENDER_PROFILE_VALSAMPLE) {
                        bender_array_push(vs, argv[i]);
                    } else {
                        /* Over the cap: the slot is open, not closed.
                         * Drop the set and mark it; consumers must
                         * fall back to boxed, never emit a huge set. */
                        va->data[i] = bender_wrap_true();
                    }
                }
            }
        }
        if (full && NULL != sh && i < sh->count &&
                (at == (uint32_t) BENDER_TABLE || at == (uint32_t) BENDER_STRUCT)) {
            /* Stream D step A: record this table's KEY SET. Only
             * all-keyword key sets are recordable: a struct field needs
             * a stable C identifier, and a non-keyword key (number,
             * string, computed value) is the signature of a real
             * dictionary, which must stay boxed. Any such key marks the
             * slot open immediately rather than recording a partial
             * shape. */
            Bender slotsh = sh->data[i];
            if (!bender_checktype(slotsh, BENDER_BOOLEAN)) {
                const BenderKV *kvs = NULL;
                int32_t cap = 0, cnt = 0;
                if (at == (uint32_t) BENDER_TABLE) {
                    BenderTable *t = bender_unwrap_table(argv[i]);
                    kvs = t->data;
                    cap = t->capacity;
                    cnt = t->count;
                } else {
                    const BenderKV *st = bender_unwrap_struct(argv[i]);
                    kvs = st;
                    cap = bender_struct_capacity(st);
                    cnt = bender_struct_length(st);
                }
                if (cnt > BENDER_PROFILE_SHAPEMAXKEYS) {
                    sh->data[i] = bender_wrap_true();
                } else {
                    /* Collect keys in the table's own slot order, then
                     * compare order-insensitively below: two tables with
                     * the same keys can hash them into different slots,
                     * so raw order is not canonical. */
                    Bender keybuf[BENDER_PROFILE_SHAPEMAXKEYS];
                    int32_t nk = 0, j;
                    int bad = 0;
                    for (j = 0; j < cap && nk <= BENDER_PROFILE_SHAPEMAXKEYS; j++) {
                        if (bender_checktype(kvs[j].key, BENDER_NIL)) continue;
                        if (!bender_checktype(kvs[j].key, BENDER_KEYWORD)) { bad = 1; break; }
                        if (nk == BENDER_PROFILE_SHAPEMAXKEYS) { bad = 1; break; }
                        keybuf[nk++] = kvs[j].key;
                    }
                    if (bad) {
                        sh->data[i] = bender_wrap_true();
                    } else {
                        BenderArray *shapes = bender_checktype(slotsh, BENDER_ARRAY)
                            ? bender_unwrap_array(slotsh) : NULL;
                        int found = 0;
                        int32_t si;
                        if (NULL != shapes) {
                            for (si = 0; si < shapes->count && !found; si++) {
                                BenderArray *cand = bender_unwrap_array(shapes->data[si]);
                                if (cand->count != nk) continue;
                                /* order-insensitive set compare */
                                int32_t a2, b2, matched = 0;
                                for (a2 = 0; a2 < nk; a2++) {
                                    for (b2 = 0; b2 < cand->count; b2++)
                                        if (bender_equals(keybuf[a2], cand->data[b2])) { matched++; break; }
                                }
                                if (matched == nk) found = 1;
                            }
                        }
                        if (!found) {
                            if (NULL != shapes && shapes->count >= BENDER_PROFILE_SHAPESAMPLE) {
                                sh->data[i] = bender_wrap_true();
                            } else {
                                BenderArray *ks = bender_array(nk);
                                bender_gcroot(bender_wrap_array(ks));
                                for (j = 0; j < nk; j++) bender_array_push(ks, keybuf[j]);
                                if (NULL == shapes) {
                                    shapes = bender_array(BENDER_PROFILE_SHAPESAMPLE);
                                    bender_gcroot(bender_wrap_array(shapes));
                                    bender_array_push(shapes, bender_wrap_array(ks));
                                    sh->data[i] = bender_wrap_array(shapes);
                                    bender_gcunroot(bender_wrap_array(shapes));
                                } else {
                                    bender_array_push(shapes, bender_wrap_array(ks));
                                }
                                bender_gcunroot(bender_wrap_array(ks));
                            }
                        }
                    }
                }
            }
        }
    }
}

/* Phase C: record a task-root invocation that bypasses CALL opcodes
 * (worker fibers are primed directly via bender_fiber, never through
 * JOP_CALL, so without this their rows show calls=0 and no params).
 * Mirrors the opcode hook exactly. Fiber-root tasks keep return-only
 * records (a resume is not a call). */
void bender_profile_task_call(BenderFunction *func, const Bender *argv, int32_t argc) {
    if (NULL != bender_vm.profile_table)
        bender_profile_call(func, argv, argc);
}

/* Record the executed successor after the original quantum poll. The frame
 * already has a profile entry from its call; cache misses only look up that
 * entry and never allocate or trigger a collection at the branch. */
static void bender_profile_edge(BenderFunction *func, int32_t pc, int taken) {
    if (pc < 0 || pc >= BENDER_PROFILE_EDGE_MAXPC ||
        func->def->bytecode_length > BENDER_PROFILE_EDGE_MAXPC) return;
    BenderProfileCache *hit = bender_profile_hit(func);
    BenderArray *edges = NULL;
    if (NULL != hit) {
        edges = hit->edges;
    } else {
        Bender value = bender_table_get(bender_vm.profile_table, bender_wrap_pointer(func->def));
        if (bender_checktype(value, BENDER_TABLE)) {
            Bender edge = bender_table_get(bender_unwrap_table(value), bender_ckeywordv("edges"));
            if (bender_checktype(edge, BENDER_ARRAY)) edges = bender_unwrap_array(edge);
        }
    }
    if (NULL != edges && pc < func->def->bytecode_length &&
        edges->count == func->def->bytecode_length * 2) {
        int32_t index = pc * 2 + !!taken;
        int64_t count = bender_unwrap_integer(edges->data[index]);
        if (count < INT32_MAX)
            edges->data[index] = bender_wrap_integer(count + 1);
    }
}

#define vm_profile_edge(taken) do { \
    if (BENDER_UNLIKELY(NULL != bender_vm.profile_table)) \
        bender_profile_edge(func, (int32_t)(pc - func->def->bytecode), (taken)); \
} while (0)

/* Observe the receiver BEFORE a potentially effectful read: bender_in/get
 * may raise or invoke user code. The observation itself is allocation-free,
 * uses only type bits (no raw GC pointer), and does not change the VM PC. */
static void bender_profile_receiver(BenderFunction *func, int32_t pc, Bender receiver) {
    if (pc < 0 || pc >= BENDER_PROFILE_EDGE_MAXPC ||
        func->def->bytecode_length > BENDER_PROFILE_EDGE_MAXPC) return;
    BenderProfileCache *hit = bender_profile_hit(func);
    BenderArray *sites = NULL;
    if (NULL != hit) {
        sites = hit->receivers;
    } else {
        Bender entry = bender_table_get(bender_vm.profile_table, bender_wrap_pointer(func->def));
        if (bender_checktype(entry, BENDER_TABLE)) {
            Bender field = bender_table_get(bender_unwrap_table(entry), bender_ckeywordv("receivers"));
            if (bender_checktype(field, BENDER_ARRAY)) sites = bender_unwrap_array(field);
        }
    }
    if (NULL != sites && sites->count == func->def->bytecode_length) {
        uint32_t mask = (uint32_t) bender_unwrap_integer(sites->data[pc]);
        mask |= 1u << (uint32_t) bender_type(receiver);
        sites->data[pc] = bender_wrap_integer((int64_t) mask);
    }
}

#define vm_profile_receiver(receiver) do { \
    if (BENDER_UNLIKELY(NULL != bender_vm.profile_table)) \
        bender_profile_receiver(func, (int32_t)(pc - func->def->bytecode), (receiver)); \
} while (0)

/* Record (callerDef, callPC) -> callee NAME attribution for the caller's
 * :calledges slot. Shaping evidence only, exactly like :elems: it tells the
 * build at WHICH call PCs a native continuation guard is worth emitting. It
 * authorizes nothing -- the emitted guard still proves the live callee
 * (§3 of docs/internals/native-continuation-design.md), so a stale or wrong
 * slot can only cost performance, never correctness.
 *
 * Bounded exactly like :edges/:receivers (BENDER_PROFILE_EDGE_MAXPC), and
 * lookup-only: it never creates a profile entry. A caller that has not been
 * profiled yet simply records nothing. The one allocation is the 3-element
 * monomorphic record, created at most once per PC. */
static void bender_profile_calledge(BenderFunction *func, int32_t pc, BenderFunction *callee) {
    if (pc < 0 || pc >= BENDER_PROFILE_EDGE_MAXPC ||
        func->def->bytecode_length > BENDER_PROFILE_EDGE_MAXPC) return;
    BenderProfileCache *hit = bender_profile_hit(func);
    BenderArray *sites = NULL;
    if (NULL != hit) {
        sites = hit->pce;
    } else {
        Bender entry = bender_table_get(bender_vm.profile_table, bender_wrap_pointer(func->def));
        if (bender_checktype(entry, BENDER_TABLE)) {
            Bender field = bender_table_get(bender_unwrap_table(entry), bender_ckeywordv("calledges"));
            if (bender_checktype(field, BENDER_ARRAY)) sites = bender_unwrap_array(field);
        }
    }
    if (NULL == sites || sites->count != func->def->bytecode_length) return;
    Bender slot = sites->data[pc];
    /* Already open: nothing more to learn at this PC. Booleans can never be
     * a recorded record, so `true` is an unambiguous marker (the same
     * discipline as :vals / :shapes). */
    if (bender_checktype(slot, BENDER_BOOLEAN)) return;
    /* An unnamed callee cannot be resolved by the build's name-keyed
     * selection, so it opens the slot rather than being recorded. */
    const uint8_t *name = callee->def->name;
    if (NULL == name) {
        sites->data[pc] = bender_wrap_true();
        return;
    }
    if (bender_checktype(slot, BENDER_ARRAY)) {
        BenderArray *rec = bender_unwrap_array(slot);
        if (rec->count != 3) return;
        if (bender_unwrap_string(rec->data[0]) != name &&
                !bender_string_equal(bender_unwrap_string(rec->data[0]), name)) {
            sites->data[pc] = bender_wrap_true();
            return;
        }
        int64_t total = bender_unwrap_integer(rec->data[1]);
        if (total < INT32_MAX) rec->data[1] = bender_wrap_integer(total + 1);
        if (NULL != callee->def->native_code) {
            int64_t nat = bender_unwrap_integer(rec->data[2]);
            if (nat < INT32_MAX) rec->data[2] = bender_wrap_integer(nat + 1);
        }
        return;
    }
    /* First observation at this PC. Root the floating record: the pushes
     * below can collect, and the slot store happens only afterwards. */
    BenderArray *rec = bender_array(3);
    bender_gcroot(bender_wrap_array(rec));
    bender_array_push(rec, bender_wrap_string(name));
    bender_array_push(rec, bender_wrap_integer(1));
    bender_array_push(rec, bender_wrap_integer(NULL != callee->def->native_code ? 1 : 0));
    sites->data[pc] = bender_wrap_array(rec);
    bender_gcunroot(bender_wrap_array(rec));
}

static void bender_profile_return(BenderFunction *func, Bender retval) {
    BenderProfileCache *hit = bender_profile_hit(func);
    if (NULL == hit) {
        bender_profile_entry(func);
        hit = bender_profile_hit(func);
    }
    hit->rets |= (1u << (uint32_t) bender_type(retval));
}

void bender_profile_start(void) {
    bender_profile_flush();
    bender_vm.profile_table = bender_table(0);
    memset(bender_profile_cache, 0, sizeof(bender_profile_cache));
}

static void bender_profile_maybe_stats(void) {
    if (NULL != getenv("BENDER_PROFILE_STATS")) {
        fprintf(stderr, "profile cache: %llu hits %llu misses (%.1f%% hit)\n",
                bender_profile_hits, bender_profile_misses,
                100.0 * bender_profile_hits /
                    (bender_profile_hits + bender_profile_misses + 1e-9));
        bender_profile_hits = bender_profile_misses = 0;
    }
}

Bender bender_profile_stop(void) {
    BenderTable *pt = bender_vm.profile_table;
    bender_profile_flush();
    bender_vm.profile_table = NULL;
    memset(bender_profile_cache, 0, sizeof(bender_profile_cache));
    bender_profile_maybe_stats();
    return NULL == pt ? bender_wrap_nil() : bender_wrap_table(pt);
}

void bender_profile_reset(void) {
    if (NULL != bender_vm.profile_table) {
        bender_profile_flush();
        bender_vm.profile_table = bender_table(0);
        memset(bender_profile_cache, 0, sizeof(bender_profile_cache));
    }
}

/* Serialize the profile as Bender data: @[[source line name]
 * {:calls n :params [[:number :string] ...] :rets [...] :mins [...]
 * :maxs [...]}], with one entry per observed function. Type sets
 * decode to keyword lists; mins/maxs carry observed number ranges
 * (nil where unobserved). */
static Bender bender_profile_mask_types(uint32_t m) {
    BenderArray *a = bender_array(4);
    int t;
    for (t = 0; t < 16; t++) {
        if (m & (1u << (uint32_t) t)) {
            bender_array_push(a, bender_ckeywordv(bender_type_names[t]));
        }
    }
    return bender_wrap_array(a);
}

/* Build the dump row array for this VM's live profile table (empty
 * when inactive). Caller roots the result across further allocation.
 * Shared by bender_profile_dump and cross-thread harvest (Phase C). */
BenderArray *bender_profile_rows(void) {
    BenderTable *pt = bender_vm.profile_table;
    BenderArray *out = bender_array(0);
    bender_profile_flush();
    if (NULL == pt) return out;
    bender_gcroot(bender_wrap_array(out));
    {
        const BenderKV *kvs;
        int32_t cap, count, i;
        bender_dictionary_view(bender_wrap_table(pt), &kvs, &count, &cap);
        for (i = 0; i < cap; i++) {
            const BenderKV *kv = kvs + i;
            BenderTable *e;
            BenderFunction *func;
            BenderFuncDef *def;
            BenderArray *row, *params;
            BenderArray *pa;
            int32_t j, n;
            if (bender_checktype(kv->key, BENDER_NIL)) continue;
            e = bender_unwrap_table(kv->value);
            func = bender_unwrap_function(bender_table_get(e, bender_ckeywordv("func")));
            def = func->def;
            row = bender_array(2);
            bender_gcroot(bender_wrap_array(row));
            {
                BenderArray *id = bender_array(3);
                int32_t defline = -1;
                bender_gcroot(bender_wrap_array(id));
                bender_array_push(id, NULL == def->source
                    ? bender_wrap_nil() : bender_wrap_string(def->source));
                if (NULL != def->sourcemap && def->bytecode_length > 0)
                    defline = def->sourcemap[0].line;
                bender_array_push(id, bender_wrap_integer((int64_t) defline));
                bender_array_push(id, NULL == def->name
                    ? bender_wrap_nil() : bender_wrap_string(def->name));
                bender_array_push(row, bender_wrap_array(id));
                bender_gcunroot(bender_wrap_array(id));
            }
            {
                BenderTable *info = bender_table(3);
                int64_t calls = bender_unwrap_integer(bender_table_get(e, bender_ckeywordv("calls")));
                uint32_t rm = (uint32_t) bender_unwrap_integer(bender_table_get(e, bender_ckeywordv("rets")));
                bender_gcroot(bender_wrap_table(info));
                bender_table_put(info, bender_ckeywordv("calls"), bender_wrap_integer(calls));
                {
                    Bender edges = bender_table_get(e, bender_ckeywordv("edges"));
                    if (bender_checktype(edges, BENDER_ARRAY))
                        bender_table_put(info, bender_ckeywordv("edges"), edges);
                    Bender receiver = bender_table_get(e, bender_ckeywordv("receivers"));
                    if (bender_checktype(receiver, BENDER_ARRAY)) {
                        BenderArray *raw = bender_unwrap_array(receiver);
                        BenderArray *decoded = bender_array(raw->count);
                        bender_gcroot(bender_wrap_array(decoded));
                        for (int32_t ri = 0; ri < raw->count; ri++) {
                            uint32_t mask = (uint32_t) bender_unwrap_integer(raw->data[ri]);
                            bender_array_push(decoded, mask ? bender_profile_mask_types(mask) : bender_wrap_nil());
                        }
                        bender_table_put(info, bender_ckeywordv("receivers"), bender_wrap_array(decoded));
                        bender_gcunroot(bender_wrap_array(decoded));
                    }
                    /* Call-edge attribution per original bytecode PC:
                     * nil = unobserved, [name total native] = the single
                     * observed callee name, :open = polymorphic/unnamed.
                     * Same dump discipline as :vals / :shapes. */
                    Bender ce = bender_table_get(e, bender_ckeywordv("calledges"));
                    if (bender_checktype(ce, BENDER_ARRAY)) {
                        BenderArray *raw = bender_unwrap_array(ce);
                        BenderArray *decoded = bender_array(raw->count);
                        bender_gcroot(bender_wrap_array(decoded));
                        for (int32_t ci = 0; ci < raw->count; ci++) {
                            Bender slot = raw->data[ci];
                            if (bender_checktype(slot, BENDER_BOOLEAN))
                                bender_array_push(decoded, bender_ckeywordv("open"));
                            else
                                bender_array_push(decoded, slot);
                        }
                        bender_table_put(info, bender_ckeywordv("calledges"), bender_wrap_array(decoded));
                        bender_gcunroot(bender_wrap_array(decoded));
                    }
                }
                {
                    Bender sig = bender_table_get(e, bender_ckeywordv("signatures"));
                    if (bender_checktype(sig, BENDER_BOOLEAN)) {
                        bender_table_put(info, bender_ckeywordv("signatures"), bender_ckeywordv("open"));
                    } else {
                        BenderArray *hist = bender_unwrap_array(sig);
                        BenderArray *decoded = bender_array(hist->count);
                        int32_t si;
                        bender_gcroot(bender_wrap_array(decoded));
                        for (si = 0; si < hist->count; si++) {
                            BenderArray *raw = bender_unwrap_array(hist->data[si]);
                            BenderArray *tags = bender_unwrap_array(raw->data[0]);
                            BenderArray *types = bender_array(tags->count);
                            BenderArray *pair;
                            int32_t ti;
                            bender_gcroot(bender_wrap_array(types));
                            pair = bender_array(2);
                            bender_gcroot(bender_wrap_array(pair));
                            for (ti = 0; ti < tags->count; ti++)
                                bender_array_push(types, bender_ckeywordv(
                                    bender_type_names[bender_unwrap_integer(tags->data[ti])]));
                            bender_array_push(pair, bender_wrap_array(types));
                            bender_array_push(pair, raw->data[1]);
                            bender_array_push(decoded, bender_wrap_array(pair));
                            bender_gcunroot(bender_wrap_array(pair));
                            bender_gcunroot(bender_wrap_array(types));
                        }
                        bender_table_put(info, bender_ckeywordv("signatures"), bender_wrap_array(decoded));
                        bender_gcunroot(bender_wrap_array(decoded));
                    }
                }
                params = bender_array(0);
                bender_gcroot(bender_wrap_array(params));
                pa = bender_unwrap_array(bender_table_get(e, bender_ckeywordv("params")));
                n = pa->count;
                for (j = 0; j < n; j++) {
                    uint32_t m = (uint32_t) bender_unwrap_integer(pa->data[j]);
                    if (m) bender_array_push(params, bender_profile_mask_types(m));
                }
                bender_table_put(info, bender_ckeywordv("params"), bender_wrap_array(params));
                bender_gcunroot(bender_wrap_array(params));
                {
                    BenderArray *elems = bender_array(0);
                    BenderArray *ea = bender_unwrap_array(bender_table_get(e, bender_ckeywordv("elems")));
                    n = ea->count;
                    for (j = 0; j < n; j++) {
                        uint32_t m = (uint32_t) bender_unwrap_integer(ea->data[j]);
                        if (m) bender_array_push(elems, bender_profile_mask_types(m));
                        else bender_array_push(elems, bender_wrap_nil());
                    }
                    bender_table_put(info, bender_ckeywordv("elems"), bender_wrap_array(elems));
                }
                {
                    /* Range evidence: observed min/max per param slot;
                     * unobserved slots serialize as nil (sentinels). */
                    BenderArray *mins = bender_array(0);
                    BenderArray *maxs = bender_array(0);
                    BenderArray *mn = bender_unwrap_array(bender_table_get(e, bender_ckeywordv("mins")));
                    BenderArray *mx = bender_unwrap_array(bender_table_get(e, bender_ckeywordv("maxs")));
                    n = mn->count;
                    for (j = 0; j < n; j++) {
                        double lo = bender_unwrap_number(mn->data[j]);
                        double hi = bender_unwrap_number(mx->data[j]);
                        if (lo <= hi) {
                            /* Non-finite bounds serialize as tagged
                             * strings: the printer emits bare inf/-inf,
                             * which parse reads back as a symbol and
                             * killed every profile re-load (unknown
                             * symbol inf). callgraph maps the strings
                             * back to float infinities. NaN never
                             * reaches here (lo <= hi fails -> nil). */
                            if (isinf(lo)) {
                                bender_array_push(mins,
                                    bender_cstringv(lo < 0 ? "-inf" : "inf"));
                            } else {
                                bender_array_push(mins, bender_wrap_number(lo));
                            }
                            if (isinf(hi)) {
                                bender_array_push(maxs,
                                    bender_cstringv(hi < 0 ? "-inf" : "inf"));
                            } else {
                                bender_array_push(maxs, bender_wrap_number(hi));
                            }
                        } else {
                            bender_array_push(mins, bender_wrap_nil());
                            bender_array_push(maxs, bender_wrap_nil());
                        }
                    }
                    bender_table_put(info, bender_ckeywordv("mins"), bender_wrap_array(mins));
                    bender_table_put(info, bender_ckeywordv("maxs"), bender_wrap_array(maxs));
                }
                {
                    /* Phase 4B value evidence: nil = unobserved, the
                     * value array itself (strings/keywords/symbols
                     * pretty-print cleanly), true = open (dumped as
                     * :open for readability). */
                    BenderArray *valsv = bender_array(0);
                    BenderArray *va = bender_unwrap_array(bender_table_get(e, bender_ckeywordv("vals")));
                    n = va->count;
                    for (j = 0; j < n; j++) {
                        Bender vv = va->data[j];
                        if (bender_checktype(vv, BENDER_NIL))
                            bender_array_push(valsv, bender_wrap_nil());
                        else if (bender_checktype(vv, BENDER_BOOLEAN))
                            bender_array_push(valsv, bender_ckeywordv("open"));
                        else
                            bender_array_push(valsv, vv);
                    }
                    bender_table_put(info, bender_ckeywordv("vals"), bender_wrap_array(valsv));
                }
                {
                    /* Stream D step A shape evidence: nil = unobserved,
                     * an array of key-arrays = the distinct observed
                     * key sets, :open = too many shapes / too wide /
                     * non-keyword keys (consumer must keep it boxed).
                     * Same dump discipline as :vals above. */
                    BenderArray *shv = bender_array(0);
                    BenderArray *sh = bender_unwrap_array(bender_table_get(e, bender_ckeywordv("shapes")));
                    n = sh->count;
                    for (j = 0; j < n; j++) {
                        Bender vv = sh->data[j];
                        if (bender_checktype(vv, BENDER_NIL))
                            bender_array_push(shv, bender_wrap_nil());
                        else if (bender_checktype(vv, BENDER_BOOLEAN))
                            bender_array_push(shv, bender_ckeywordv("open"));
                        else
                            bender_array_push(shv, vv);
                    }
                    bender_table_put(info, bender_ckeywordv("shapes"), bender_wrap_array(shv));
                }
                bender_table_put(info, bender_ckeywordv("rets"), bender_profile_mask_types(rm));
                bender_array_push(row, bender_wrap_table(info));
                bender_gcunroot(bender_wrap_table(info));
            }
            bender_array_push(out, bender_wrap_array(row));
            bender_gcunroot(bender_wrap_array(row));
        }
    }
    bender_gcunroot(bender_wrap_array(out));
    return out;
}

void bender_profile_dump(const char *path) {
    BenderArray *out;
    BenderBuffer *buf;
    FILE *f;
    if (NULL == bender_vm.profile_table) bender_panic("profiling not active");
    out = bender_profile_rows();
    bender_gcroot(bender_wrap_array(out));
    buf = bender_buffer(0);
    bender_gcroot(bender_wrap_buffer(buf));
    bender_pretty(buf, 8, BENDER_PRETTY_NOTRUNC, bender_wrap_array(out));
    f = fopen(path, "wb");
    if (NULL == f) bender_panicf("could not open profile output %s", path);
    fwrite(buf->data, 1, (size_t) buf->count, f);
    fputc('\n', f);
    fclose(f);
    bender_gcunroot(bender_wrap_buffer(buf));
    bender_gcunroot(bender_wrap_array(out));
}

/* Interpreter main loop */
static BenderSignal run_vm(BenderFiber *fiber, Bender in) {
#ifdef BENDER_NATIVE_EXECUTORS
    /* New resume: invalidate cached whole-definition validation verdicts.
     * Any change to a definition made between resumes (debugger
     * breakpoints, raw C edits while parked) is re-detected by the full
     * exact compare at this resume's first native region entry. */
    bender_vm.native_epoch++;
#endif

    /* opcode -> label lookup if using clang/GCC */
#ifdef BENDER_USE_COMPUTED_GOTOS
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
        &&label_JOP_BAND_IMMEDIATE,
        &&label_JOP_BOR_IMMEDIATE,
        &&label_JOP_BXOR_IMMEDIATE,
        &&label_JOP_MAKE_SCRATCH_TUPLE,
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
    register Bender *stack;
    register uint32_t *pc;
    register BenderFunction *func;

    /* NOTE: Flag smuggling! We "smuggle" the fiber sigal in the gc.flags field instead of the usual flags field */
    if (fiber->flags & BENDER_FIBER_RESUME_SIGNAL) {
        /* Get the signal */
        BenderSignal sig = (fiber->gc.flags & BENDER_FIBER_STATUS_MASK) >> BENDER_FIBER_STATUS_OFFSET;
        /* Clear the signal */
        fiber->gc.flags &= ~BENDER_FIBER_STATUS_MASK;
        fiber->flags &= ~BENDER_FIBER_RESUME_SIGNAL;
        bender_vm.return_reg[0] = in;
        return sig;
    }

    vm_restore();

    if (fiber->flags & BENDER_FIBER_DID_LONGJUMP) {
        if (bender_fiber_frame(fiber)->func == NULL) {
            /* Inside a c function */
            bender_fiber_popframe(fiber);
            vm_restore();
        }
        /* Check if we were at a tail call instruction. If so, do implicit return */
        if ((*pc & 0xFF) == JOP_TAILCALL) {
            /* Tail call resume */
            int entrance_frame = bender_stack_frame(stack)->flags & BENDER_STACKFRAME_ENTRANCE;
            bender_fiber_popframe(fiber);
            if (entrance_frame) {
                fiber->flags &= ~BENDER_FIBER_FLAG_MASK;
                vm_return(BENDER_SIGNAL_OK, in);
            }
            vm_restore();
        }
    }

    if (!(fiber->flags & BENDER_FIBER_RESUME_NO_USEVAL)) stack[A] = in;
    if (!(fiber->flags & BENDER_FIBER_RESUME_NO_SKIP)) pc++;

    uint8_t first_opcode = *pc & ((fiber->flags & BENDER_FIBER_BREAKPOINT) ? 0x7F : 0xFF);

#ifdef BENDER_NATIVE_EXECUTORS
    int native_debug_bypass = fiber->flags & BENDER_FIBER_BREAKPOINT;
#endif
    fiber->flags &= ~BENDER_FIBER_FLAG_MASK;
#ifdef BENDER_NATIVE_EXECUTORS
    if (!native_debug_bypass) {
        vm_native();
        first_opcode = *pc & 0xFF;
    }
#endif

    /* Main interpreter loop. Semantically is a switch on
     * (*pc & 0xFF) inside of an infinite loop. */
    VM_START();

    VM_DEFAULT();
    fiber->flags |= BENDER_FIBER_BREAKPOINT | BENDER_FIBER_RESUME_NO_USEVAL | BENDER_FIBER_RESUME_NO_SKIP;
    vm_return(BENDER_SIGNAL_DEBUG, bender_wrap_nil());

    VM_OP(JOP_NOOP)
    vm_pcnext();

    VM_OP(JOP_ERROR)
    vm_return(BENDER_SIGNAL_ERROR, stack[D]);

    VM_OP(JOP_TYPECHECK)
    vm_assert_types(stack[A], E);
    vm_pcnext();

    VM_OP(JOP_RETURN) {
        Bender retval = stack[D];
        int entrance_frame = bender_stack_frame(stack)->flags & BENDER_STACKFRAME_ENTRANCE;
        BenderFunction *retfunc = bender_stack_frame(stack)->func;
        bender_fiber_popframe(fiber);
        if (NULL != bender_vm.profile_table)
            bender_profile_return(retfunc, retval);
        if (entrance_frame) vm_return_no_restore(BENDER_SIGNAL_OK, retval);
        vm_restore();
        stack[A] = retval;
        vm_checkgc_pcnext();
    }

    VM_OP(JOP_RETURN_NIL) {
        Bender retval = bender_wrap_nil();
        int entrance_frame = bender_stack_frame(stack)->flags & BENDER_STACKFRAME_ENTRANCE;
        BenderFunction *retfunc = bender_stack_frame(stack)->func;
        bender_fiber_popframe(fiber);
        if (NULL != bender_vm.profile_table)
            bender_profile_return(retfunc, retval);
        if (entrance_frame) vm_return_no_restore(BENDER_SIGNAL_OK, retval);
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
        Bender op1 = stack[B];
        Bender op2 = stack[C];
        if (bender_checktype(op1, BENDER_NUMBER) && bender_checktype(op2, BENDER_NUMBER)) {
            double x1 = bender_unwrap_number(op1);
            double x2 = bender_unwrap_number(op2);
            stack[A] = bender_wrap_number(floor(x1 / x2));
            vm_pcnext();
        } else {
            vm_commit();
            Bender a = bender_binop_call("div", "rdiv", op1, op2);
            stack = fiber->data + fiber->frame;
            stack[A] = a;
            vm_checkgc_pcnext();
        }
    }

    VM_OP(JOP_MODULO) {
        Bender op1 = stack[B];
        Bender op2 = stack[C];
        if (bender_checktype(op1, BENDER_NUMBER) && bender_checktype(op2, BENDER_NUMBER)) {
            double x1 = bender_unwrap_number(op1);
            double x2 = bender_unwrap_number(op2);
            if (x2 == 0) {
                stack[A] = bender_wrap_number(x1);
            } else {
                double intres = x2 * floor(x1 / x2);
                stack[A] = bender_wrap_number(x1 - intres);
            }
            vm_pcnext();
        } else {
            vm_commit();
            Bender a = bender_binop_call("mod", "rmod", op1, op2);
            stack = fiber->data + fiber->frame;
            stack[A] = a;
            vm_checkgc_pcnext();
        }
    }

    VM_OP(JOP_REMAINDER) {
        Bender op1 = stack[B];
        Bender op2 = stack[C];
        if (bender_checktype(op1, BENDER_NUMBER) && bender_checktype(op2, BENDER_NUMBER)) {
            double x1 = bender_unwrap_number(op1);
            double x2 = bender_unwrap_number(op2);
            stack[A] = bender_wrap_number(fmod(x1, x2));
            vm_pcnext();
        } else {
            vm_commit();
            Bender a = bender_binop_call("%", "r%", op1, op2);
            stack = fiber->data + fiber->frame;
            stack[A] = a;
            vm_checkgc_pcnext();
        }
    }

    VM_OP(JOP_BAND)
    vm_bitop(&);

    /* Fused (band x <const 0..255>). Mirrors JOP_BAND exactly, including
     * the int32-range check and the bender_mcall path for a non-numeric
     * operand, with the mask taken from the unsigned immediate. */
    VM_OP(JOP_BAND_IMMEDIATE)
    vm_bitop_uimmediate(&);

    VM_OP(JOP_BOR_IMMEDIATE)
    vm_bitop_uimmediate(|);

    VM_OP(JOP_BXOR_IMMEDIATE)
    vm_bitop_uimmediate(^);

    VM_OP(JOP_BOR)
    vm_bitop( |);

    VM_OP(JOP_BXOR)
    vm_bitop(^);

    VM_OP(JOP_BNOT) {
        Bender op = stack[E];
        if (bender_checktype(op, BENDER_NUMBER)) {
            stack[A] = bender_wrap_integer(~bender_unwrap_integer(op));
            vm_pcnext();
        } else {
            vm_commit();
            Bender a = bender_unary_call("~", op);
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
    vm_profile_edge(1);
    pc += DS;
    vm_next();

    VM_OP(JOP_JUMP_IF)
    if (bender_truthy(stack[A])) {
        vm_maybe_auto_suspend(ES <= 0);
        vm_profile_edge(1);
        pc += ES;
    } else {
        vm_profile_edge(0);
        pc++;
    }
    vm_next();

    VM_OP(JOP_JUMP_IF_NOT)
    if (bender_truthy(stack[A])) {
        vm_profile_edge(0);
        pc++;
    } else {
        vm_maybe_auto_suspend(ES <= 0);
        vm_profile_edge(1);
        pc += ES;
    }
    vm_next();

    VM_OP(JOP_JUMP_IF_NIL)
    if (bender_checktype(stack[A], BENDER_NIL)) {
        vm_maybe_auto_suspend(ES <= 0);
        vm_profile_edge(1);
        pc += ES;
    } else {
        vm_profile_edge(0);
        pc++;
    }
    vm_next();

    VM_OP(JOP_JUMP_IF_NOT_NIL)
    if (bender_checktype(stack[A], BENDER_NIL)) {
        vm_profile_edge(0);
        pc++;
    } else {
        vm_maybe_auto_suspend(ES <= 0);
        vm_profile_edge(1);
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
    stack[A] = bender_wrap_boolean(bender_equals(stack[B], stack[C]));
    vm_pcnext();

    VM_OP(JOP_EQUALS_IMMEDIATE)
    stack[A] = bender_wrap_boolean(bender_checktype(stack[B], BENDER_NUMBER) && (bender_unwrap_number(stack[B]) == (double) CS));
    vm_pcnext();

    VM_OP(JOP_NOT_EQUALS)
    stack[A] = bender_wrap_boolean(!bender_equals(stack[B], stack[C]));
    vm_pcnext();

    VM_OP(JOP_NOT_EQUALS_IMMEDIATE)
    stack[A] = bender_wrap_boolean(!bender_checktype(stack[B], BENDER_NUMBER) || (bender_unwrap_number(stack[B]) != (double) CS));
    vm_pcnext();

    VM_OP(JOP_COMPARE) {
        Bender a = bender_wrap_integer(bender_compare(stack[B], stack[C]));
        stack = fiber->data + fiber->frame;
        stack[A] = a;
    }
    vm_pcnext();

    VM_OP(JOP_NEXT)
    vm_commit();
    {
        Bender temp = bender_next_impl(stack[B], stack[C], 1);
        vm_restore();
        stack[A] = temp;
    }
    vm_pcnext();

    VM_OP(JOP_LOAD_NIL)
    stack[D] = bender_wrap_nil();
    vm_pcnext();

    VM_OP(JOP_LOAD_TRUE)
    stack[D] = bender_wrap_true();
    vm_pcnext();

    VM_OP(JOP_LOAD_FALSE)
    stack[D] = bender_wrap_false();
    vm_pcnext();

    VM_OP(JOP_LOAD_INTEGER)
    stack[A] = bender_wrap_integer(ES);
    vm_pcnext();

    VM_OP(JOP_LOAD_CONSTANT) {
        int32_t cindex = (int32_t)E;
        vm_assert(cindex < func->def->constants_length, "invalid constant");
        stack[A] = func->def->constants[cindex];
        vm_pcnext();
    }

    VM_OP(JOP_LOAD_SELF)
    stack[D] = bender_wrap_function(func);
    vm_pcnext();

    VM_OP(JOP_LOAD_UPVALUE) {
        int32_t eindex = B;
        int32_t vindex = C;
        BenderFuncEnv *env;
        vm_assert(func->def->environments_length > eindex, "invalid upvalue environment");
        env = func->envs[eindex];
        vm_assert(env->length > vindex, "invalid upvalue index");
        vm_assert(bender_env_valid(env), "invalid upvalue environment");
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
        BenderFuncEnv *env;
        vm_assert(func->def->environments_length > eindex, "invalid upvalue environment");
        env = func->envs[eindex];
        vm_assert(env->length > vindex, "invalid upvalue index");
        vm_assert(bender_env_valid(env), "invalid upvalue environment");
        if (env->offset > 0) {
            env->as.fiber->data[env->offset + vindex] = stack[A];
        } else {
            env->as.values[vindex] = stack[A];
        }
        vm_pcnext();
    }

    VM_OP(JOP_CLOSURE) {
        BenderFuncDef *fd;
        BenderFunction *fn;
        int32_t elen;
        int32_t defindex = (int32_t)E;
        vm_assert(defindex < func->def->defs_length, "invalid funcdef");
        fd = func->def->defs[defindex];
        elen = fd->environments_length;
        fn = bender_gcalloc(BENDER_MEMORY_FUNCTION, sizeof(BenderFunction) + ((size_t) elen * sizeof(BenderFuncEnv *)));
        fn->def = fd;
        {
            int32_t i;
            for (i = 0; i < elen; ++i) {
                int32_t inherit = fd->environments[i];
                if (inherit == -1 || inherit >= func->def->environments_length) {
                    BenderStackFrame *frame = bender_stack_frame(stack);
                    if (!frame->env) {
                        /* Lazy capture of current stack frame */
                        BenderFuncEnv *env = bender_gcalloc(BENDER_MEMORY_FUNCENV, sizeof(BenderFuncEnv));
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
        stack[A] = bender_wrap_function(fn);
        vm_checkgc_pcnext();
    }

    VM_OP(JOP_PUSH)
    bender_fiber_push(fiber, stack[D]);
    stack = fiber->data + fiber->frame;
    vm_checkgc_pcnext();

    VM_OP(JOP_PUSH_2)
    bender_fiber_push2(fiber, stack[A], stack[E]);
    stack = fiber->data + fiber->frame;
    vm_checkgc_pcnext();

    VM_OP(JOP_PUSH_3)
    bender_fiber_push3(fiber, stack[A], stack[B], stack[C]);
    stack = fiber->data + fiber->frame;
    vm_checkgc_pcnext();

    VM_OP(JOP_PUSH_ARRAY) {
        const Bender *vals;
        int32_t len;
        if (bender_indexed_view(stack[D], &vals, &len)) {
            bender_fiber_pushn(fiber, vals, len);
        } else {
            bender_panicf("expected %T, got %v", BENDER_TFLAG_INDEXED, stack[D]);
        }
    }
    stack = fiber->data + fiber->frame;
    vm_checkgc_pcnext();

    VM_OP(JOP_CALL) {
        vm_maybe_overflow();
        vm_maybe_auto_suspend(1);
        Bender callee = stack[E];
        if (fiber->stacktop > fiber->maxstack) {
            vm_throw("stack overflow");
        }
        if (bender_checktype(callee, BENDER_KEYWORD)) {
            vm_commit();
            callee = resolve_method(callee, fiber);
        }
        if (bender_checktype(callee, BENDER_FUNCTION)) {
            BenderFunction *callee_func = bender_unwrap_function(callee);
            /* FFI alias shim fast path (bender_ffi_alias_install): the
             * installed body is exactly `push args; ldc wrapper; tcall`,
             * so calling the wrapper directly is the same call minus the
             * intermediate frame and tailcall. Only the exact installed
             * shape carries the runtime-only flag; arity mismatches,
             * traced runs and profiled runs fall through to the ordinary
             * path. The generated wrapper never mutates argv, so passing
             * the caller's argument slots is equivalent to the shim's
             * pushes. */
            if ((callee_func->def->flags & BENDER_FUNCDEF_FLAG_FFI_ALIAS) &&
                    (NULL == bender_vm.profile_table) &&
                    !(callee_func->gc.flags & BENDER_FUNCFLAG_TRACE) &&
                    ((fiber->stacktop - fiber->stackstart) == callee_func->def->arity) &&
                    (callee_func->def->constants_length == 1) &&
                    bender_checktype(callee_func->def->constants[0], BENDER_CFUNCTION)) {
                BenderCFunction alias_cfun = bender_unwrap_cfunction(callee_func->def->constants[0]);
                int32_t alias_argc = fiber->stacktop - fiber->stackstart;
                vm_commit();
                bender_fiber_cframe(fiber, alias_cfun);
                Bender ret = alias_cfun(alias_argc, fiber->data + fiber->frame);
                bender_fiber_popframe(fiber);
                /* `func` still names the CALLER here; the ordinary
                 * function path is what rebinds it to the callee. */
                stack = fiber->data + fiber->frame;
                stack[A] = ret;
                vm_checkgc_pcnext();
            }
            /* Caller identity for the :calledges attribution, captured
             * BEFORE `func` is rebound to the callee. `pc` still points at
             * this call word and is not moved by the frame push below, so
             * the slot index stays the caller's call PC. */
            BenderFunction *calledge_caller = func;
            int32_t calledge_pc = (int32_t)(pc - func->def->bytecode);
            func = callee_func;
            /* Counter-identity half: every Bender-function call the
             * interpreter dispatches here. bender_vm.native_continuations
             * counts the ones a native executor took instead, and the sum
             * must not change when continuation is enabled or disabled. */
            bender_vm.native_interp_calls++;
            if (func->gc.flags & BENDER_FUNCFLAG_TRACE) {
                vm_do_trace(func, fiber->stacktop - fiber->stackstart, fiber->data + fiber->stackstart);
            }
            vm_commit();
            {
                int32_t callargc = fiber->stacktop - fiber->stackstart;
                if (bender_fiber_funcframe(fiber, func)) {
                    int32_t n = fiber->stacktop - fiber->stackstart;
                    bender_panicf("%v called with %d argument%s, expected %d",
                                 callee, n, n == 1 ? "" : "s", func->def->arity);
                }
                /* UAF fix (Phase 4A): capture argv AFTER the frame push.
                 * bender_fiber_funcframe may realloc fiber->data (capacity
                 * growth), so a pointer captured before it dangles; the
                 * args live at fiber->data + fiber->frame afterwards.
                 * For vararg defs the trailing spread args are consolidated
                 * into one tuple at index def->arity, so sample arity+1. */
                if (NULL != bender_vm.profile_table) {
                    bender_profile_call(func, fiber->data + fiber->frame,
                                       (func->def->flags & BENDER_FUNCDEF_FLAG_VARARG)
                                       ? func->def->arity + 1
                                       : callargc);
                    bender_profile_calledge(calledge_caller, calledge_pc, func);
                }
            }
            stack = fiber->data + fiber->frame;
            pc = func->def->bytecode;
            vm_checkgc_next();
        } else if (bender_checktype(callee, BENDER_CFUNCTION)) {
            vm_commit();
            int32_t argc = fiber->stacktop - fiber->stackstart;
            bender_fiber_cframe(fiber, bender_unwrap_cfunction(callee));
            Bender ret = bender_unwrap_cfunction(callee)(argc, fiber->data + fiber->frame);
            bender_fiber_popframe(fiber);
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
        Bender callee = stack[D];
        if (fiber->stacktop > fiber->maxstack) {
            vm_throw("stack overflow");
        }
        if (bender_checktype(callee, BENDER_KEYWORD)) {
            vm_commit();
            callee = resolve_method(callee, fiber);
        }
        if (bender_checktype(callee, BENDER_FUNCTION)) {
            func = bender_unwrap_function(callee);
            if (func->gc.flags & BENDER_FUNCFLAG_TRACE) {
                vm_do_trace(func, fiber->stacktop - fiber->stackstart, fiber->data + fiber->stackstart);
            }
            {
                int32_t callargc = fiber->stacktop - fiber->stackstart;
                if (bender_fiber_funcframe_tail(fiber, func)) {
                    bender_stack_frame(fiber->data + fiber->frame)->pc = pc;
                    int32_t n = fiber->stacktop - fiber->stackstart;
                    bender_panicf("%v called with %d argument%s, expected %d",
                                 callee, n, n == 1 ? "" : "s", func->def->arity);
                }
                /* UAF fix (Phase 4A): same as JOP_CALL — funcframe_tail
                 * reallocs fiber->data on growth and additionally MEMMOVES
                 * the args down to fiber->data + fiber->frame, so both the
                 * old pointer and the old stackstart location are invalid. */
                if (NULL != bender_vm.profile_table)
                    bender_profile_call(func, fiber->data + fiber->frame,
                                       (func->def->flags & BENDER_FUNCDEF_FLAG_VARARG)
                                       ? func->def->arity + 1
                                       : callargc);
            }
            stack = fiber->data + fiber->frame;
            pc = func->def->bytecode;
            vm_checkgc_next();
        } else {
            Bender retreg;
            int entrance_frame = bender_stack_frame(stack)->flags & BENDER_STACKFRAME_ENTRANCE;
            vm_commit();
            if (bender_checktype(callee, BENDER_CFUNCTION)) {
                int32_t argc = fiber->stacktop - fiber->stackstart;
                bender_fiber_cframe(fiber, bender_unwrap_cfunction(callee));
                retreg = bender_unwrap_cfunction(callee)(argc, fiber->data + fiber->frame);
                bender_fiber_popframe(fiber);
            } else {
                retreg = call_nonfn(fiber, callee);
            }
            bender_fiber_popframe(fiber);
            if (entrance_frame) {
                vm_return_no_restore(BENDER_SIGNAL_OK, retreg);
            }
            vm_restore();
            stack[A] = retreg;
            vm_checkgc_pcnext();
        }
    }

    VM_OP(JOP_RESUME) {
        Bender retreg;
        vm_maybe_overflow();
        vm_maybe_auto_suspend(1);
        vm_assert_type(stack[B], BENDER_FIBER);
        BenderFiber *child = bender_unwrap_fiber(stack[B]);
        if (bender_check_can_resume(child, &retreg, 0)) {
            vm_commit();
            bender_panicv(retreg);
        }
        fiber->child = child;
        BenderSignal sig = bender_continue_no_check(child, stack[C], &retreg);
        stack = fiber->data + fiber->frame;
        if (sig != BENDER_SIGNAL_OK && !(child->flags & (1 << sig))) {
            /* P3.6: nested child suspended (not completed): parking
             * here would strand live C frames for others to clobber
             * if any parked party is shared. Fail loud instead
             * (nested-complete flows through untouched). */
            if ((sig == BENDER_SIGNAL_EVENT || sig == BENDER_SIGNAL_YIELD ||
                    sig == BENDER_SIGNAL_INTERRUPT) &&
                    bender_fiber_chain_has_shared(fiber)) {
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
        if (s > BENDER_SIGNAL_USER9) s = BENDER_SIGNAL_USER9;
        if (s < 0) s = 0;
        vm_return(s, stack[B]);
    }

    VM_OP(JOP_PROPAGATE) {
        Bender fv = stack[C];
        vm_assert_type(fv, BENDER_FIBER);
        BenderFiber *f = bender_unwrap_fiber(fv);
        BenderFiberStatus sub_status = bender_fiber_status(f);
        if (sub_status > BENDER_STATUS_USER9) {
            vm_commit();
            bender_panicf("cannot propagate from fiber with status :%s",
                         bender_status_names[sub_status]);
        }
        fiber->child = f;
        vm_return((int) sub_status, stack[B]);
    }

    VM_OP(JOP_CANCEL) {
        Bender retreg;
        vm_assert_type(stack[B], BENDER_FIBER);
        BenderFiber *child = bender_unwrap_fiber(stack[B]);
        if (bender_check_can_resume(child, &retreg, 1)) {
            vm_commit();
            bender_panicv(retreg);
        }
        fiber->child = child;
        BenderSignal sig = bender_continue_signal(child, stack[C], &retreg, BENDER_SIGNAL_ERROR);
        if (sig != BENDER_SIGNAL_OK && !(child->flags & (1 << sig))) {
            /* P3.6: same nested-suspend hazard as JOP_RESUME (a
             * cancelled child may catch and keep running). */
            if ((sig == BENDER_SIGNAL_EVENT || sig == BENDER_SIGNAL_YIELD ||
                    sig == BENDER_SIGNAL_INTERRUPT) &&
                    bender_fiber_chain_has_shared(fiber)) {
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
    fiber->flags |= BENDER_FIBER_RESUME_NO_USEVAL;
    bender_put(stack[A], stack[B], stack[C]);
    stack = fiber->data + fiber->frame;
    fiber->flags &= ~BENDER_FIBER_RESUME_NO_USEVAL;
    vm_checkgc_pcnext();

    VM_OP(JOP_PUT_INDEX)
    vm_commit();
    fiber->flags |= BENDER_FIBER_RESUME_NO_USEVAL;
    bender_putindex(stack[A], C, stack[B]);
    stack = fiber->data + fiber->frame;
    fiber->flags &= ~BENDER_FIBER_RESUME_NO_USEVAL;
    vm_checkgc_pcnext();

    VM_OP(JOP_IN)
    vm_profile_receiver(stack[B]);
    vm_commit();
    {
        Bender a = bender_in(stack[B], stack[C]);
        stack = fiber->data + fiber->frame;
        stack[A] = a;
    }
    vm_pcnext();

    /* Fused (in ds <const small int>): key comes from the UNSIGNED
     * immediate rather than a preceding load_integer into a slot.
     * Calls bender_in -- the SAME entry point as JOP_IN above -- so
     * behavior is identical in every case, including the raise on an
     * out-of-range string/symbol/keyword index where bender_getindex
     * (and therefore JOP_GET_INDEX) would instead yield nil. */
    VM_OP(JOP_IN_IMMEDIATE)
    vm_profile_receiver(stack[B]);
    vm_commit();
    {
        Bender a = bender_in(stack[B], bender_wrap_integer((int32_t)(uint8_t) C));
        stack = fiber->data + fiber->frame;
        stack[A] = a;
    }
    vm_pcnext();

    VM_OP(JOP_GET)
    vm_profile_receiver(stack[B]);
    vm_commit();
    {
        Bender a = bender_get(stack[B], stack[C]);
        stack = fiber->data + fiber->frame;
        stack[A] = a;
    }
    vm_pcnext();

    VM_OP(JOP_GET_INDEX)
    vm_profile_receiver(stack[B]);
    vm_commit();
    {
        Bender a = bender_getindex(stack[B], C);
        stack = fiber->data + fiber->frame;
        stack[A] = a;
    }
    vm_pcnext();

    VM_OP(JOP_LENGTH)
    vm_commit();
    {
        Bender a = bender_lengthv(stack[E]);
        stack = fiber->data + fiber->frame;
        stack[A] = a;
    }
    vm_pcnext();

    VM_OP(JOP_MAKE_ARRAY) {
        int32_t count = fiber->stacktop - fiber->stackstart;
        Bender *mem = fiber->data + fiber->stackstart;
        stack[D] = bender_wrap_array(bender_array_n(mem, count));
        fiber->stacktop = fiber->stackstart;
        vm_checkgc_pcnext();
    }

    VM_OP(JOP_MAKE_TUPLE)
    /* fallthrough */
    VM_OP(JOP_MAKE_BRACKET_TUPLE) {
        int32_t count = fiber->stacktop - fiber->stackstart;
        Bender *mem = fiber->data + fiber->stackstart;
        const Bender *tup = bender_tuple_n(mem, count);
        if (opcode == JOP_MAKE_BRACKET_TUPLE)
            bender_tuple_flag(tup) |= BENDER_TUPLE_FLAG_BRACKETCTOR;
        stack[D] = bender_wrap_tuple(tup);
        fiber->stacktop = fiber->stackstart;
        vm_checkgc_pcnext();
    }

    VM_OP(JOP_MAKE_SCRATCH_TUPLE) {
        int32_t count = fiber->stacktop - fiber->stackstart;
        Bender *mem = fiber->data + fiber->stackstart;
        Bender scratch = stack[E];
        BenderArray *arr;
        if (bender_checktype(scratch, BENDER_ARRAY)) {
            arr = bender_unwrap_array(scratch);
            if (arr->capacity < count)
                bender_array_ensure(arr, count, 1);
            if (count)
                memcpy(arr->data, mem, sizeof(Bender) * (size_t) count);
            arr->count = count;
        } else {
            arr = bender_array_n(mem, count);
            scratch = bender_wrap_array(arr);
            stack[E] = scratch;
        }
        stack[A] = scratch;
        fiber->stacktop = fiber->stackstart;
        vm_checkgc_pcnext();
    }

    VM_OP(JOP_MAKE_TABLE) {
        int32_t count = fiber->stacktop - fiber->stackstart;
        Bender *mem = fiber->data + fiber->stackstart;
        if (count & 1) {
            vm_commit();
            bender_panicf("expected even number of arguments to table constructor, got %d", count);
        }
        BenderTable *table = bender_table(count / 2);
        for (int32_t i = 0; i < count; i += 2)
            bender_table_put(table, mem[i], mem[i + 1]);
        stack[D] = bender_wrap_table(table);
        fiber->stacktop = fiber->stackstart;
        vm_checkgc_pcnext();
    }

    VM_OP(JOP_MAKE_STRUCT) {
        int32_t count = fiber->stacktop - fiber->stackstart;
        Bender *mem = fiber->data + fiber->stackstart;
        if (count & 1) {
            vm_commit();
            bender_panicf("expected even number of arguments to struct constructor, got %d", count);
        }
        BenderKV *st = bender_struct_begin(count / 2);
        for (int32_t i = 0; i < count; i += 2)
            bender_struct_put(st, mem[i], mem[i + 1]);
        stack[D] = bender_wrap_struct(bender_struct_end(st));
        fiber->stacktop = fiber->stackstart;
        vm_checkgc_pcnext();
    }

    VM_OP(JOP_MAKE_STRING) {
        int32_t count = fiber->stacktop - fiber->stackstart;
        Bender *mem = fiber->data + fiber->stackstart;
        BenderBuffer buffer;
        bender_buffer_init(&buffer, 10 * count);
        for (int32_t i = 0; i < count; i++)
            bender_to_string_b(&buffer, mem[i]);
        stack[D] = bender_stringv(buffer.data, buffer.count);
        bender_buffer_deinit(&buffer);
        fiber->stacktop = fiber->stackstart;
        vm_checkgc_pcnext();
    }

    VM_OP(JOP_MAKE_BUFFER) {
        int32_t count = fiber->stacktop - fiber->stackstart;
        Bender *mem = fiber->data + fiber->stackstart;
        BenderBuffer *buffer = bender_buffer(10 * count);
        for (int32_t i = 0; i < count; i++)
            bender_to_string_b(buffer, mem[i]);
        stack[D] = bender_wrap_buffer(buffer);
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
BenderSignal bender_step(BenderFiber *fiber, Bender in, Bender *out) {
    /* No finished or currently alive fibers. */
    BenderFiberStatus status = bender_fiber_status(fiber);
    if (status == BENDER_STATUS_ALIVE ||
            status == BENDER_STATUS_DEAD ||
            status == BENDER_STATUS_ERROR) {
        bender_panicf("cannot step fiber with status :%s", bender_status_names[status]);
    }

    /* Get PC for setting breakpoints */
    uint32_t *pc = bender_stack_frame(fiber->data + fiber->frame)->pc;

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
#ifdef BENDER_NATIVE_EXECUTORS
    native_stepping++;
#endif
    BenderSignal signal = bender_continue(fiber, in, out);
#ifdef BENDER_NATIVE_EXECUTORS
    native_stepping--;
#endif

    /* Restore */
    if (nexta) *nexta = olda;
    if (nextb) *nextb = oldb;

    return signal;
}

static Bender void_cfunction(int32_t argc, Bender *argv) {
    (void) argc;
    (void) argv;
    bender_panic("placeholder");
}

Bender bender_call(BenderFunction *fun, int32_t argc, const Bender *argv) {
    /* Check entry conditions */
    if (!bender_vm.fiber)
        bender_panic("bender_call failed because there is no current fiber");
    if (bender_vm.stackn >= BENDER_RECURSION_GUARD)
        bender_panic("C stack recursed too deeply");
    /* Phase D3: C-driven nesting deepens the native stack; convert a
     * pending fault-handler overflow the same way JOP_CALL does. */
    if (bender_vm.fiber->stack_overflowed) {
        bender_vm.fiber->stack_overflowed = 0;
        bender_panic("fiber native stack overflow");
    }

    /* Dirty stack */
    int32_t dirty_stack = bender_vm.fiber->stacktop - bender_vm.fiber->stackstart;
    if (dirty_stack) {
        bender_fiber_cframe(bender_vm.fiber, void_cfunction);
    }

    /* Tracing */
    if (fun->gc.flags & BENDER_FUNCFLAG_TRACE) {
        bender_vm.stackn++;
        vm_do_trace(fun, argc, argv);
        bender_vm.stackn--;
    }

    /* Push frame */
    bender_fiber_pushn(bender_vm.fiber, argv, argc);
    if (bender_fiber_funcframe(bender_vm.fiber, fun)) {
        int32_t min = fun->def->min_arity;
        int32_t max = fun->def->max_arity;
        Bender funv = bender_wrap_function(fun);
        if (min == max && min != argc)
            bender_panicf("arity mismatch in %v, expected %d, got %d", funv, min, argc);
        if (min >= 0 && argc < min)
            bender_panicf("arity mismatch in %v, expected at least %d, got %d", funv, min, argc);
        bender_panicf("arity mismatch in %v, expected at most %d, got %d", funv, max, argc);
    }
    bender_fiber_frame(bender_vm.fiber)->flags |= BENDER_STACKFRAME_ENTRANCE;

    /* Set up */
    int32_t oldn = bender_vm.stackn++;
    int handle = bender_gclock();

    /* Run vm */
    bender_vm.fiber->flags |= BENDER_FIBER_RESUME_NO_USEVAL | BENDER_FIBER_RESUME_NO_SKIP;
    int old_coerce_error = bender_vm.coerce_error;
    bender_vm.coerce_error = 1;
    BenderSignal signal = run_vm(bender_vm.fiber, bender_wrap_nil());
    bender_vm.coerce_error = old_coerce_error;

    /* Phase 1.1 (see docs/internals/native-fibers.md): a suspension signal
     * (yield, user signal, debug, ...) reached through these live C frames
     * must freeze the whole C chain with a context switch instead of being
     * coerced to an error. Each resume re-enters the nested VM below, so
     * the yielding function runs to completion and its C callers observe
     * an ordinary call. */
    while (signal != BENDER_SIGNAL_OK && signal != BENDER_SIGNAL_ERROR) {
        BenderFiber *susp_fiber = bender_vm.fiber;
        bender_suspend_swap(susp_fiber, signal, *bender_vm.return_reg);
        /* Resumed. A signal may have been smuggled in (e.g. cancel);
         * consume it the way run_vm entry does. */
        if (susp_fiber->flags & BENDER_FIBER_RESUME_SIGNAL) {
            signal = (susp_fiber->gc.flags & BENDER_FIBER_STATUS_MASK) >> BENDER_FIBER_STATUS_OFFSET;
            susp_fiber->gc.flags &= ~BENDER_FIBER_STATUS_MASK;
            susp_fiber->flags &= ~BENDER_FIBER_RESUME_SIGNAL;
            bender_vm.return_reg[0] = susp_fiber->in_value;
            if (signal == BENDER_SIGNAL_OK) {
                /* Cannot happen via continue_signal, but stay total: an OK
                 * smuggle is a plain resume. */
                continue;
            }
            if (signal != BENDER_SIGNAL_ERROR) {
                /* A new suspension was injected on resume: propagate it
                 * outward the same way. */
                continue;
            }
            break;
        }
        /* Plain resume: deliver the resume value as the yield result and
         * keep running the nested call. Clear any stale re-entry flags so
         * delivery behaves exactly like the trampoline's run_vm re-entry. */
        susp_fiber->flags &= ~(BENDER_FIBER_RESUME_NO_USEVAL | BENDER_FIBER_RESUME_NO_SKIP);
        signal = run_vm(susp_fiber, susp_fiber->in_value);
    }

    /* Teardown */
    bender_vm.stackn = oldn;
    bender_gcunlock(handle);
    if (dirty_stack) {
        bender_fiber_popframe(bender_vm.fiber);
        bender_vm.fiber->stacktop += dirty_stack;
    }

    if (signal != BENDER_SIGNAL_OK) {
        /* Should match logic in bender_signalv */
#ifdef BENDER_EV
        if (bender_vm.root_fiber != NULL && signal == BENDER_SIGNAL_EVENT) {
            bender_vm.root_fiber->sched_id++;
        }
#endif
        if (signal != BENDER_SIGNAL_ERROR) {
            *bender_vm.return_reg = bender_wrap_string(bender_formatc("%v coerced from %s to error", *bender_vm.return_reg, bender_signal_names[signal]));
        }
        bender_panicv(*bender_vm.return_reg);
    }

    return *bender_vm.return_reg;
}

/* P3.6: walk the resume chain for shared-stack fibers parked
 * mid-C (child links verified intact — a stale link means that
 * ancestor already unwound normally and is safe). Self excluded:
 * the suspender abandons cleanly and re-primes on wake. Bounded. */
static int bender_fiber_chain_has_shared(BenderFiber *fiber) {
    int depth = 0;
    BenderFiber *prev = fiber;
    BenderFiber *f = fiber->resume_fiber;
    while (NULL != f && depth < 1024) {
        if (f->child != prev) break;
        if (f->shared_stack) return 1;
        prev = f;
        f = f->resume_fiber;
        depth++;
    }
    return 0;
}

static BenderSignal bender_check_can_resume(BenderFiber *fiber, Bender *out, int is_cancel) {
    /* Check conditions */
    BenderFiberStatus old_status = bender_fiber_status(fiber);
    if (bender_vm.stackn >= BENDER_RECURSION_GUARD) {
        bender_fiber_set_status(fiber, BENDER_STATUS_ERROR);
        *out = bender_cstringv("C stack recursed too deeply");
        return BENDER_SIGNAL_ERROR;
    }
    /* If a "task" fiber is trying to be used as a normal fiber, detect that. See bug #920.
     * Fibers must be marked as root fibers manually, or by the ev scheduler. */
    if (bender_vm.fiber != NULL && (fiber->gc.flags & BENDER_FIBER_EV_GCFLAG_ROOT)) {
#ifdef BENDER_EV
        *out = bender_cstringv(is_cancel
                              ? "cannot cancel root fiber, use ev/cancel"
                              : "cannot resume root fiber, use ev/go");
#else
        *out = bender_cstringv(is_cancel
                              ? "cannot cancel root fiber"
                              : "cannot resume root fiber");
#endif
        return BENDER_SIGNAL_ERROR;
    }
    if (old_status == BENDER_STATUS_ALIVE ||
            old_status == BENDER_STATUS_DEAD ||
            (old_status >= BENDER_STATUS_USER0 && old_status <= BENDER_STATUS_USER4) ||
            old_status == BENDER_STATUS_ERROR) {
        const uint8_t *str = bender_formatc("cannot resume fiber with status :%s",
                                           bender_status_names[old_status]);
        *out = bender_wrap_string(str);
        return BENDER_SIGNAL_ERROR;
    }
    return BENDER_SIGNAL_OK;
}

void bender_try_init(BenderTryState *state) {
    state->stackn = bender_vm.stackn++;
    state->gc_handle = bender_vm.gc_suspend;
    state->vm_fiber = bender_vm.fiber;
    state->vm_jmp_buf = bender_vm.signal_buf;
    state->vm_return_reg = bender_vm.return_reg;
    state->coerce_error = bender_vm.coerce_error;
    bender_vm.return_reg = &(state->payload);
    bender_vm.signal_buf = &(state->buf);
    bender_vm.coerce_error = 0;
}

void bender_restore(BenderTryState *state) {
    bender_vm.stackn = state->stackn;
    bender_vm.gc_suspend = state->gc_handle;
    bender_vm.fiber = state->vm_fiber;
    bender_vm.signal_buf = state->vm_jmp_buf;
    bender_vm.return_reg = state->vm_return_reg;
    bender_vm.coerce_error = state->coerce_error;
}

/* Phase 1.1 swap-suspension (see docs/internals/native-fibers.md). The VM * dynamic registers always describe the RUNNING party. When a fiber
 * suspends with live C frames, its registers are parked in the fiber and
 * the resumer's parked registers loaded, so nothing points into the
 * frozen C stack while the resumer runs. bender_continue_no_check does the
 * inverse when resuming. Host (non-fiber) code parks in bender_vm.base_*. */

/* Park the live registers into a fiber (the suspending/yielding party). */
static void bender_vmregs_save(BenderFiber *f) {
    f->saved_signal_buf = bender_vm.signal_buf;
    f->saved_return_reg = bender_vm.return_reg;
    f->saved_coerce_error = bender_vm.coerce_error;
    f->saved_stackn = bender_vm.stackn;
    f->saved_gc_suspend = bender_vm.gc_suspend;
}

/* Load parked registers from a fiber (the party being switched to). */
static void bender_vmregs_load(BenderFiber *f) {
    bender_vm.signal_buf = f->saved_signal_buf;
    bender_vm.return_reg = f->saved_return_reg;
    bender_vm.coerce_error = f->saved_coerce_error;
    bender_vm.stackn = f->saved_stackn;
    bender_vm.gc_suspend = f->saved_gc_suspend;
}

/* Park the live registers into the host slots. */
static void bender_vmregs_save_base(void) {
    bender_vm.base_signal_buf = bender_vm.signal_buf;
    bender_vm.base_return_reg = bender_vm.return_reg;
    bender_vm.base_coerce_error = bender_vm.coerce_error;
    bender_vm.base_stackn = bender_vm.stackn;
    bender_vm.base_gc_suspend = bender_vm.gc_suspend;
}

/* Load parked registers from the host slots. */
static void bender_vmregs_load_base(void) {
    bender_vm.signal_buf = bender_vm.base_signal_buf;
    bender_vm.return_reg = bender_vm.base_return_reg;
    bender_vm.coerce_error = bender_vm.base_coerce_error;
    bender_vm.stackn = bender_vm.base_stackn;
    bender_vm.gc_suspend = bender_vm.base_gc_suspend;
}

/* Suspend the running fiber with live C frames on its native stack.
 * Publishes sig/payload exactly like the trampoline does on a returned
 * signal, parks this fiber's registers, loads the resumer's, and switches
 * out. On resume, execution continues after this call with this fiber's
 * registers live again (re-parked by bender_continue_no_check beforehand). */
static void bender_suspend_swap(BenderFiber *fiber, BenderSignal sig, Bender payload) {
    fiber->out_payload = payload;
    fiber->out_signal = sig;
    fiber->last_value = payload;
    bender_fiber_set_status(fiber, sig);
    /* The previous resume argument has been consumed. A parked fiber
     * receives a fresh input before its C frames run again. */
    fiber->in_value = bender_wrap_nil();
    fiber->suspend_mode = 1;
    bender_vmregs_save(fiber);
    BenderFiber *resumer = fiber->resume_fiber;
    if (resumer) {
        bender_vmregs_load(resumer);
    } else {
        bender_vmregs_load_base();
    }
    bender_vm.fiber = resumer;
    bender_fiber_ctx_swap(&fiber->ctx, fiber->resume_ctx);
}

/* Suspend the current fiber from C with a yield signal (see bender.h).
 * Works from any C code running on the fiber's native stack: cfunctions,
 * nested bender_call frames, or future AOT-compiled frames. */
Bender bender_fiber_yield(Bender v) {
    BenderFiber *fiber = bender_vm.fiber;
    if (NULL == fiber) bender_panic("bender_fiber_yield failed because there is no current fiber");
    if (NULL == fiber->resume_ctx) bender_panic("bender_fiber_yield failed because the fiber was never resumed");
    /* P3.6: raw yield needs its C frames preserved across the switch
     * (resume continues them), which shared stacks cannot do (others
     * clobber the region). Fail loud — coroutines stay private. */
    if (fiber->shared_stack) {
        bender_panic("cannot yield from a shared-stack fiber "
                    "(use private fibers for coroutines)");
    }
    BenderSignal sig = BENDER_SIGNAL_YIELD;
    for (;;) {
        bender_suspend_swap(fiber, sig, v);
        /* Resumed: consume a smuggled signal the way run_vm entry does. */
        if (fiber->flags & BENDER_FIBER_RESUME_SIGNAL) {
            BenderSignal psig = (fiber->gc.flags & BENDER_FIBER_STATUS_MASK) >> BENDER_FIBER_STATUS_OFFSET;
            fiber->gc.flags &= ~BENDER_FIBER_STATUS_MASK;
            fiber->flags &= ~BENDER_FIBER_RESUME_SIGNAL;
            bender_vm.return_reg[0] = fiber->in_value;
            if (psig == BENDER_SIGNAL_ERROR) {
                bender_panicv(fiber->in_value);
            }
            if (psig != BENDER_SIGNAL_OK) {
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
 * runs on the fiber's own native stack (primed via bender_fiber_ctx_prime).
 * Between resumes of a fiber, we sit at the ctx_swap waiting to be
 * switched in; on each resume, we run one VM entry, publish the result on
 * the fiber, and switch back to the resumer. VM values, signals and
 * longjmp targets never cross fiber boundaries: every signal handler
 * (jmp_buf) and return register lives in this loop's BenderTryState on the
 * fiber's stack. */
void bender_fiber_trampoline(void) {
    for (;;) {
        BenderFiber *fiber = bender_vm.fiber;
        BenderSignal sig;
        BenderTryState tstate;
        sig = bender_try(&tstate);
        if (!sig) {
            sig = run_vm(fiber, fiber->in_value);
        }
        bender_restore(&tstate);
        fiber->out_payload = tstate.payload;
        fiber->out_signal = sig;
        fiber->last_value = tstate.payload;
        /* run_vm consumed the previous input; the next resume installs
         * its own value. Do not retain the old input while parked. */
        fiber->in_value = bender_wrap_nil();
        /* The fiber returned to the trampoline (no frozen C stack above
         * this point): resume re-enters run_vm, and any stale swap-suspend
         * mode from an earlier nested suspension is over. */
        fiber->suspend_mode = 0;
        bender_fiber_set_status(fiber, sig);
        /* Hand control back with a full register exchange, mirroring
         * bender_suspend_swap: park this fiber, load the resumer. */
        bender_vmregs_save(fiber);
        BenderFiber *tr_resumer = fiber->resume_fiber;
        if (tr_resumer) {
            bender_vmregs_load(tr_resumer);
        } else {
            bender_vmregs_load_base();
        }
        bender_vm.fiber = tr_resumer;
        bender_fiber_ctx_swap(&fiber->ctx, fiber->resume_ctx);
    }
}

static BenderSignal bender_continue_no_check(BenderFiber *fiber, Bender in, Bender *out) {

    /* M4b-2 quantum driver: every resume — loop-scheduled or direct —
     * starts with a fresh poll budget. UNCONDITIONAL (not gated on
     * quantum): anyone may enable quanta mid-run, and a stale zero
     * budget would suspend at the next poll with zero progress. One
     * predictable store per resume. */
    bender_vm.quantum_remaining = BENDER_QUANTUM_BUDGET;

    BenderFiberStatus old_status = bender_fiber_status(fiber);

#ifdef BENDER_EV
    bender_fiber_did_resume(fiber);
#endif

    /* Clear last value */
    fiber->last_value = bender_wrap_nil();

    /* Phase D3: pending red-zone overflow surfaces here (check-and-
     * clear, mirroring the interrupt block below): the C stack already
     * unwound past the fault by suspension, so raising is safe. */
    if (fiber->stack_overflowed) {
        fiber->stack_overflowed = 0;
        Bender ov = bender_cstringv("fiber native stack overflow");
        fiber->last_value = ov;
        *out = ov;
        bender_fiber_set_status(fiber, BENDER_SIGNAL_ERROR);
        return BENDER_SIGNAL_ERROR;
    }

#ifdef BENDER_EV
    /* M4b-1 cooperative interrupt (see ev/interrupt): a pending
     * request suspends here — before a single instruction runs — so
     * delivery can never land mid-instruction, inside pcall, or in C.
     * A racing wakeup value in `in` is dropped (interrupt wins). */
    if (fiber->interrupt_requested) {
        fiber->interrupt_requested = 0;
        Bender iv = fiber->interrupt_value;
        fiber->interrupt_value = bender_wrap_nil();
        fiber->last_value = iv;
        *out = iv;
        bender_fiber_set_status(fiber, BENDER_SIGNAL_YIELD);
        return BENDER_SIGNAL_YIELD;
    }
#endif

    /* Continue child fiber if it exists */
    if (fiber->child) {
        if (bender_vm.root_fiber == NULL) bender_vm.root_fiber = fiber;
        BenderFiber *child = fiber->child;
        uint32_t instr = (bender_stack_frame(fiber->data + fiber->frame)->pc)[0];
        bender_vm.stackn++;
        BenderSignal sig = bender_continue(child, in, &in);
        bender_vm.stackn--;
        if (bender_vm.root_fiber == fiber) bender_vm.root_fiber = NULL;
        if (sig != BENDER_SIGNAL_OK && !(child->flags & (1 << sig))) {
            *out = in;
            bender_fiber_set_status(fiber, sig);
            fiber->last_value = child->last_value;
            return sig;
        }
        /* Check if we need any special handling for certain opcodes */
        switch (instr & 0x7F) {
            default:
                break;
            case JOP_NEXT: {
                if (sig == BENDER_SIGNAL_OK ||
                        sig == BENDER_SIGNAL_ERROR ||
                        sig == BENDER_SIGNAL_USER0 ||
                        sig == BENDER_SIGNAL_USER1 ||
                        sig == BENDER_SIGNAL_USER2 ||
                        sig == BENDER_SIGNAL_USER3 ||
                        sig == BENDER_SIGNAL_USER4) {
                    in = bender_wrap_nil();
                } else {
                    in = bender_wrap_integer(0);
                }
                break;
            }
        }
        fiber->child = NULL;
    }

    /* Handle new fibers being resumed with a non-nil value */
    if (old_status == BENDER_STATUS_NEW && !bender_checktype(in, BENDER_NIL)) {
        Bender *stack = fiber->data + fiber->frame;
        BenderFunction *func = bender_stack_frame(stack)->func;
        if (func) {
            if (func->def->arity > 0) {
                stack[0] = in;
            } else if (func->def->flags & BENDER_FUNCDEF_FLAG_VARARG) {
                stack[0] = bender_wrap_tuple(bender_tuple_n(&in, 1));
            }
        }
    }

    /* If this is a nested continue (root_fiber already set), root the fiber
     * so it survives GC. bender_collect only marks root_fiber, so without
     * this a nested fiber (e.g., from bender_pcall in a C function) would be
     * invisible to GC and could be collected while actively running. */
    int fiber_rooted = (bender_vm.root_fiber != NULL);
    if (fiber_rooted) {
        bender_gcroot(bender_wrap_fiber(fiber));
    }

    /* Switch onto the target fiber's own native stack. Phase 1.1: the VM
     * dynamic registers always describe the running party, so park the
     * caller's live registers and load the target's parked registers
     * (fresh fibers park zeroes at creation). The target's suspend path
     * (bender_suspend_swap or the trampoline publish above) inverts this
     * exchange on the way out. Fallible work (gcroot) stays above the
     * exchange, where the caller's signal chain is still live. */
    BenderFiber *caller = bender_vm.fiber;
    BenderFiberCtx *caller_ctx = caller ? &caller->ctx : &bender_vm.base_ctx;
    if (caller) {
        bender_vmregs_save(caller);
    } else {
        bender_vmregs_save_base();
    }
    bender_vmregs_load(fiber);

    if (bender_vm.root_fiber == NULL) bender_vm.root_fiber = fiber;
    bender_vm.fiber = fiber;
    bender_fiber_set_status(fiber, BENDER_STATUS_ALIVE);

    fiber->in_value = in;
    fiber->resume_ctx = caller_ctx;
    fiber->resume_fiber = caller;
    /* P3.6: stacks materialize on first resume (never at alloc, so
     * never-run fibers map nothing). Shared fibers re-prime on the
     * lane mapping every resume (parked position is always the
     * committed pc); private fibers map+prime once. One branch each. */
    if (fiber->shared_stack) {
        bender_fiber_shared_prime(fiber);
    } else if (NULL == fiber->native_stack_base) {
        bender_fiber_ensure_native_stack(fiber);
    }
    bender_fiber_ctx_swap(caller_ctx, &fiber->ctx);

    BenderSignal sig = fiber->out_signal;
    bender_vm.fiber = caller;
    if (bender_vm.root_fiber == fiber) bender_vm.root_fiber = NULL;
    bender_fiber_set_status(fiber, sig);
    if (fiber_rooted) {
        bender_gcunroot(bender_wrap_fiber(fiber));
    }
    fiber->last_value = fiber->out_payload;
    *out = fiber->out_payload;

    return sig;
}

/* Enter the main vm loop */
BenderSignal bender_continue(BenderFiber *fiber, Bender in, Bender *out) {
    /* Check conditions */
    BenderSignal tmp_signal = bender_check_can_resume(fiber, out, 0);
    if (tmp_signal) return tmp_signal;
    return bender_continue_no_check(fiber, in, out);
}

/* Enter the main vm loop but immediately raise a signal */
BenderSignal bender_continue_signal(BenderFiber *fiber, Bender in, Bender *out, BenderSignal sig) {
    BenderSignal tmp_signal = bender_check_can_resume(fiber, out, sig != BENDER_SIGNAL_OK);
    if (tmp_signal) return tmp_signal;
    if (sig != BENDER_SIGNAL_OK) {
        BenderFiber *child = fiber;
        while (child->child) child = child->child;
        /* NOTE: We are "smuggling" flags in for later use in an unusal place. This is odd but intentional and
         * saves a bit of memory per fiber rather than creating a new field. There is likely a better way to do this. */
        child->gc.flags &= ~BENDER_FIBER_STATUS_MASK;
        child->gc.flags |= sig << BENDER_FIBER_STATUS_OFFSET;
        child->flags |= BENDER_FIBER_RESUME_SIGNAL;
    }
    return bender_continue_no_check(fiber, in, out);
}

BenderSignal bender_pcall(
    BenderFunction *fun,
    int32_t argc,
    const Bender *argv,
    Bender *out,
    BenderFiber **f) {
    BenderFiber *fiber;
    if (f && *f) {
        fiber = bender_fiber_reset(*f, fun, argc, argv);
    } else {
        fiber = bender_fiber(fun, 64, argc, argv);
    }
    if (f) *f = fiber;
    if (NULL == fiber) {
        *out = bender_cstringv("arity mismatch");
        return BENDER_SIGNAL_ERROR;
    }
    return bender_continue(fiber, bender_wrap_nil(), out);
}

Bender bender_mcall(const char *name, int32_t argc, Bender *argv) {
    /* At least 1 argument */
    if (argc < 1) {
        bender_panicf("method :%s expected at least 1 argument", name);
    }
    /* Find method */
    Bender method = bender_method_lookup(argv[0], name);
    if (bender_checktype(method, BENDER_NIL)) {
        bender_panicf("could not find method :%s for %v", name, argv[0]);
    }
    /* Invoke method */
    return bender_method_invoke(method, argc, argv);
}

/* Setup VM */
int bender_init(void) {
#ifdef BENDER_TTFX_SITE_PROFILE
    bender_site_count = 0;
    bender_site_dropped = 0;
    bender_site_last = NULL;
    bender_site_last_id = 0;
    bender_site_enabled = getenv("BENDER_TTFX_SITE_PROFILE") != NULL;
#endif

    bender_vm.trace_strict = getenv("BENDER_TRACE_STRICT") != NULL;
#ifdef BENDER_NATIVE_EXECUTORS
    /* Linked descriptor registrations can survive bender_deinit; an explicit
     * registration or later publication sets this flag as soon as it can
     * affect dispatch. */
    bender_vm.native_possible = bender_native_registry_count() != 0;
#endif
    /* Native-to-native continuation is on unless explicitly disabled, so one
     * binary can run both halves of an A/B with an identical executor set. */
    bender_vm.native_continuation = getenv("BENDER_NATIVE_NO_CONTINUATION") == NULL;
    bender_vm.native_continuations = 0;
    bender_vm.native_continuation_refusals = 0;
    bender_vm.native_returns = 0;
    bender_vm.native_interp_calls = 0;
    bender_vm.native_interp_dispatches = 0;
    bender_vm.work_allocations = 0;
    bender_vm.work_alloc_bytes = 0;
    bender_vm.work_barrier_calls = 0;
    bender_vm.work_gc_major = 0;
    bender_vm.work_gc_minor = 0;
    bender_vm.work_gc_fiber_minor = 0;
    bender_vm.work_gc_process_minor = 0;

    /* Garbage collection */
    bender_vm.blocks = NULL;
    bender_vm.weak_blocks = NULL;
    bender_vm.string_blocks = NULL;
    bender_vm.sweep_pending[0] = NULL;
    bender_vm.sweep_pending[1] = NULL;
    bender_vm.sweep_pending[2] = NULL;
    bender_vm.gc_quarantine[0] = NULL;
    bender_vm.gc_quarantine[1] = NULL;
    bender_vm.gc_quarantine[2] = NULL;
    bender_vm.gc_quarantine_count = 0;
    bender_vm.next_collection = 0;
    bender_vm.gc_interval = 0x400000;
    bender_vm.block_count = 0;
    bender_vm.gc_major_live_blocks = 0;
    bender_vm.gc_mark_phase = 0;
    /* Generational policy (Phase 2b-iii): minors run on nursery pressure. */
    bender_vm.gc_minor_phase = 0;
    bender_vm.gc_young_bytes = 0;
    bender_vm.gc_minor_threshold = 256 * 1024;
    /* Step 1 (process heaps): owner-id source starts empty (0 reserved). */
    bender_vm.gc_owner_next = 0;
    /* Step P1 (proc processes): pid serials start empty (0 reserved). */
    bender_vm.proc_serial = 0;
    /* P3.3 gate: no strict fibers yet (spawn-isolated sets it). */
    bender_vm.heap_strict_live = 0;
    /* P3.5c allocation attribution starts clear (spawn sets it). */
    bender_vm.gc_alloc_ctx = NULL;
    /* P3.6: lane-shared stack unmapped until first shared resume;
     * opt-in read once (any set value enables). */
    bender_vm.shared_stack_base = NULL;
    bender_vm.shared_stack = NULL;
    bender_vm.shared_stack_size = 0;
    bender_vm.shared_stack_committed = NULL;
    /* P3.6 shared C stacks default ON for ev-task + proc fibers: the
     * spawn profile shows per-spawn stack malloc-large + OS zero-fill
     * as the dominant density cost (~1 stack per actor). Opt OUT with
     * BENDER_SHARED_STACKS=0 (generators and C-frame yields keep private
     * stacks regardless — suspend discipline, enforced at fiber_new). */
    {
        const char *ssflag = getenv("BENDER_SHARED_STACKS");
        bender_vm.shared_stacks_on = !(NULL != ssflag && 0 == strcmp(ssflag, "0"));
    }

    bender_symcache_init();

    /* Initialize gc roots */
    bender_vm.roots = NULL;
    bender_vm.root_count = 0;
    bender_vm.root_capacity = 0;

    /* Initialize shadow root stack (Phase 2b-i) */
    bender_vm.shadow_roots = NULL;
    bender_vm.shadow_count = 0;
    bender_vm.shadow_capacity = 0;

    /* Slab allocator state starts empty (Phase 2b-iii, lazy). */
    bender_vm.slab_state = NULL;

    /* Scratch memory */
    bender_vm.user = NULL;
    bender_vm.scratch_mem = NULL;
    bender_vm.scratch_len = 0;
    bender_vm.scratch_cap = 0;

    /* Sandbox flags */
    bender_vm.sandbox_flags = 0;

    /* Initialize registry */
    bender_vm.registry = NULL;
    bender_vm.registry_cap = 0;
    bender_vm.registry_count = 0;
    bender_vm.registry_dirty = 0;

    /* Initialize abstract registry */
    bender_vm.abstract_registry = bender_table(0);
    bender_gcroot(bender_wrap_table(bender_vm.abstract_registry));

    /* B7.1 named proc registry: lazy (NULL until proc/register runs;
     * NULL reads exactly like an empty table at every use site). */
    bender_vm.proc_registry = NULL;

    /* Traversal */
    bender_vm.traversal = NULL;
    bender_vm.traversal_base = NULL;
    bender_vm.traversal_top = NULL;

    /* Core env */
    bender_vm.core_env = NULL;

    /* Auto suspension */
    bender_vm.auto_suspend = 0;

    /* M4b-2 quantum driver: ON by default (Step 2 preemptive
     * reductions); BENDER_TIMESLICE=0 restores fully cooperative
     * scheduling per process, ev/timeslice toggles per VM. */
    bender_vm.quantum = 0;
    bender_vm.quantum_remaining = 0;
    {
        const char *tsenv = getenv("BENDER_TIMESLICE");
        if (NULL == tsenv || strcmp(tsenv, "0") != 0) {
            bender_vm.quantum = 1;
        }
    }

    /* Dynamic bindings */
    bender_vm.top_dyns = NULL;

    /* Seed RNG */
    bender_rng_seed(bender_default_rng(), 0);
#ifdef BENDER_OPCODE_HISTOGRAM
    atexit(bender_oph_dump);
#endif

    /* Fibers */
    bender_vm.fiber = NULL;
    bender_vm.root_fiber = NULL;
    bender_vm.stackn = 0;
    /* Phase D: fault handler (process-once) + this thread's alt stack,
     * before any fiber can run here. */
    bender_fiber_growstack_init();
    /* Phase 1.1/2a: no active compilation, clean host register park. */
    bender_vm.compiler_stack = NULL;
    bender_vm.base_signal_buf = NULL;
    bender_vm.base_return_reg = NULL;
    bender_vm.base_coerce_error = 0;
    bender_vm.base_stackn = 0;
    bender_vm.base_gc_suspend = 0;

#ifdef BENDER_EV
    bender_ev_init();
    bender_abstract_home_register();
#endif
#ifdef BENDER_NET
    bender_net_init();
#endif
    return 0;
}

/* Disable some features at runtime with no way to re-enable them */
void bender_sandbox(uint32_t flags) {
    bender_sandbox_assert(BENDER_SANDBOX_SANDBOX);
    bender_vm.sandbox_flags |= flags;
}

void bender_sandbox_assert(uint32_t forbidden_flags) {
    if (forbidden_flags & bender_vm.sandbox_flags) {
        bender_panic("operation forbidden by sandbox");
    }
}

/* Total recorded calls across the live table. Flushes first so
 * C-side cached counts land in the tables, then sums tables only. */
static int64_t bender_profile_total_calls(void) {
    int64_t total = 0;
    if (NULL != bender_vm.profile_table) {
        bender_profile_flush();
        {
            const BenderKV *kvs;
            int32_t cap, count, i;
            bender_dictionary_view(bender_wrap_table(bender_vm.profile_table), &kvs, &count, &cap);
            for (i = 0; i < cap; i++) {
                const BenderKV *kv = kvs + i;
                if (bender_checktype(kv->key, BENDER_NIL)) continue;
                total += bender_unwrap_integer(bender_table_get(
                    bender_unwrap_table(kv->value), bender_ckeywordv("calls")));
            }
        }
    }
    return total;
}

/* AOT launchers keep their auto-PGO destination in this VM thread rather
 * than process-wide env, so spawned interpreters cannot inherit it. */
static BENDER_THREAD_LOCAL char *bender_auto_profile_output;

void bender_profile_auto_output(const char *path) {
    char *copy = NULL;
    if (path && *path) {
        size_t len = strlen(path) + 1;
        copy = malloc(len);
        if (copy) memcpy(copy, path, len);
    }
    free(bender_auto_profile_output);
    bender_auto_profile_output = copy;
}

/* Auto-PGO workload provenance. An auto-profiled run is only useful as
 * release evidence if a build can replay it, so the AOT launcher hands its
 * exact argv here and the io.c choke points report stdin consumption and
 * stdout bytes. A companion <profile>.workload record is published beside
 * the profile, using the same vocabulary as the explicit --pgo cache record
 * (:args/:argv/:has-stdin/:stdin-bytes) plus an explicit :replayable
 * verdict. Everything below is inert unless the launcher armed it. */
#define BENDER_AUTO_WORKLOAD_STDIN_MAX (1024 * 1024)

static BENDER_THREAD_LOCAL char **bender_auto_workload_argv;
static BENDER_THREAD_LOCAL int bender_auto_workload_argc;
static BENDER_THREAD_LOCAL unsigned long bender_auto_workload_image;
static BENDER_THREAD_LOCAL int bender_auto_workload_armed;
static BENDER_THREAD_LOCAL int bender_auto_workload_stdin;
static BENDER_THREAD_LOCAL uint64_t bender_auto_workload_hash;
static BENDER_THREAD_LOCAL uint64_t bender_auto_workload_out;

void bender_profile_auto_workload(int argc, char **argv, unsigned long image_bytes) {
    int i;
    bender_auto_workload_armed = 0;
    bender_auto_workload_argc = 0;
    bender_auto_workload_hash = 1469598103934665603ULL; /* FNV-1a 64 offset */
    bender_auto_workload_out = 0;
    bender_auto_workload_stdin = 0;
    if (argc < 1 || NULL == argv) return;
    bender_auto_workload_argv = malloc(sizeof(char *) * (size_t) argc);
    if (NULL == bender_auto_workload_argv) return;
    for (i = 0; i < argc; i++) {
        size_t len = (NULL == argv[i]) ? 1 : strlen(argv[i]) + 1;
        char *copy = malloc(len);
        if (NULL == copy) return; /* keep what copied; the record stays honest */
        if (NULL == argv[i]) copy[0] = '\0';
        else memcpy(copy, argv[i], len);
        bender_auto_workload_argv[bender_auto_workload_argc++] = copy;
    }
    bender_auto_workload_image = image_bytes;
    bender_auto_workload_armed = 1;
}

void bender_profile_note_stdin(void) {
    if (bender_auto_workload_armed) bender_auto_workload_stdin = 1;
}

void bender_profile_note_stdout(const void *bytes, size_t len) {
    const unsigned char *p = (const unsigned char *) bytes;
    size_t i;
    if (!bender_auto_workload_armed || NULL == p) return;
    for (i = 0; i < len; i++) {
        bender_auto_workload_hash ^= (uint64_t) p[i];
        bender_auto_workload_hash *= 1099511628211ULL;
    }
    bender_auto_workload_out += (uint64_t) len;
}

static void bender_auto_workload_free(void) {
    int i;
    for (i = 0; i < bender_auto_workload_argc; i++)
        free(bender_auto_workload_argv[i]);
    free(bender_auto_workload_argv);
    bender_auto_workload_argv = NULL;
    bender_auto_workload_argc = 0;
    bender_auto_workload_armed = 0;
}

/* Write the companion record for a just-published profile. Non-replayable
 * runs are recorded with :replayable false and a reason: never silently
 * promoted, never silently dropped. */
static void bender_auto_workload_write(const char *profile) {
    BenderTable *rec;
    BenderArray *args;
    BenderBuffer *joined;
    BenderBuffer *text;
    Bender stdin_bytes = bender_wrap_nil();
    const char *reason = NULL;
    int has_stdin = 0;
    int i;
    char path[4096];
    FILE *f;
    if (!bender_auto_workload_armed || NULL == profile || '\0' == *profile) return;
    if (strlen(profile) + sizeof(".workload") > sizeof(path)) return;
    /* argv[0] is the binary; the workload is what follows, exactly as the
     * explicit --pgo path records it. */
    args = bender_array(bender_auto_workload_argc);
    bender_gcroot(bender_wrap_array(args));
    joined = bender_buffer(0);
    bender_gcroot(bender_wrap_buffer(joined));
    for (i = 1; i < bender_auto_workload_argc; i++) {
        const char *a = bender_auto_workload_argv[i];
        const char *c;
        bender_array_push(args, bender_cstringv(a));
        if (i > 1) bender_buffer_push_u8(joined, ' ');
        bender_buffer_push_cstring(joined, a);
        for (c = a; *c != '\0'; c++) {
            /* :args is a space-joined string parsed by aot-words, so an
             * argument carrying whitespace or quotes cannot round-trip. */
            if (*c == ' ' || *c == '\t' || *c == '\n' || *c == '\r' ||
                    *c == '"' || *c == '\'' || *c == '\\')
                reason = "unquotable-argv";
        }
    }
    if (bender_auto_workload_stdin) {
#if defined(_WIN32)
        if (NULL == reason) reason = "stdin-capture-unsupported";
#else
        struct stat st;
        if (0 == fstat(0, &st) && S_ISREG(st.st_mode) && st.st_size >= 0 &&
                st.st_size <= (off_t) BENDER_AUTO_WORKLOAD_STDIN_MAX) {
            BenderBuffer *sb = bender_buffer((int32_t) st.st_size + 1);
            ssize_t got;
            bender_gcroot(bender_wrap_buffer(sb));
            got = pread(0, sb->data, (size_t) st.st_size, 0);
            if (got == (ssize_t) st.st_size) {
                stdin_bytes = bender_stringv(sb->data, (int32_t) got);
                has_stdin = 1;
            } else if (NULL == reason) {
                reason = "stdin-unreadable";
            }
            bender_gcunroot(bender_wrap_buffer(sb));
        } else if (NULL == reason) {
            /* terminal, pipe, or oversized redirect: not reproducible */
            reason = "stdin-not-replayable";
        }
#endif
    }
    rec = bender_table(12);
    bender_gcroot(bender_wrap_table(rec));
    bender_table_put(rec, bender_ckeywordv("version"), bender_wrap_integer(1));
    bender_table_put(rec, bender_ckeywordv("source"), bender_ckeywordv("auto-pgo"));
    bender_table_put(rec, bender_ckeywordv("args"),
                    bender_stringv(joined->data, joined->count));
    bender_table_put(rec, bender_ckeywordv("argv"), bender_wrap_array(args));
    bender_table_put(rec, bender_ckeywordv("has-stdin"), bender_wrap_boolean(has_stdin));
    bender_table_put(rec, bender_ckeywordv("stdin-bytes"), stdin_bytes);
    bender_table_put(rec, bender_ckeywordv("replayable"),
                    bender_wrap_boolean(NULL == reason));
    bender_table_put(rec, bender_ckeywordv("reason"),
                    (NULL == reason) ? bender_wrap_nil() : bender_ckeywordv(reason));
    bender_table_put(rec, bender_ckeywordv("binary"),
                    (bender_auto_workload_argc > 0)
                    ? bender_cstringv(bender_auto_workload_argv[0])
                    : bender_wrap_nil());
    bender_table_put(rec, bender_ckeywordv("image-bytes"),
                    bender_wrap_number((double) bender_auto_workload_image));
    bender_table_put(rec, bender_ckeywordv("stdout-bytes"),
                    bender_wrap_number((double) bender_auto_workload_out));
    {   /* printable identity for the run's observed output */
        char hash[32];
        snprintf(hash, sizeof(hash), "%016llx",
                 (unsigned long long) bender_auto_workload_hash);
        bender_table_put(rec, bender_ckeywordv("stdout-hash"), bender_cstringv(hash));
    }
    text = bender_buffer(0);
    bender_gcroot(bender_wrap_buffer(text));
    bender_formatb(text, "%j", bender_wrap_table(rec));
    snprintf(path, sizeof(path), "%s.workload", profile);
    f = fopen(path, "wb");
    if (NULL != f) {
        fwrite(text->data, 1, (size_t) text->count, f);
        fputc('\n', f);
        fclose(f);
    }
    bender_gcunroot(bender_wrap_buffer(text));
    bender_gcunroot(bender_wrap_table(rec));
    bender_gcunroot(bender_wrap_buffer(joined));
    bender_gcunroot(bender_wrap_array(args));
}

/* Clear all memory associated with the VM */
void bender_deinit(void) {
#ifdef BENDER_TTFX_SITE_PROFILE
    bender_site_dump();
    bender_site_enabled = 0;
#endif
#ifdef BENDER_EV
    /* Publish shutdown before profile flushing or GC can stall the loop. */
    bender_abstract_home_unregister();
#endif
    /* The private AOT destination or explicit BENDER_PROFILE_OUT dumps on
     * every exit path, including os/exit, before VM teardown. Auto-PGO
     * applies the minimum-call threshold; explicit output preserves its
     * existing policy. Override the floor with BENDER_AUTO_PGO_MIN_CALLS. */
    if (NULL != bender_vm.profile_table) {
        const char *pout = bender_auto_profile_output ? bender_auto_profile_output : getenv("BENDER_PROFILE_OUT");
        bender_profile_maybe_stats();
        if (NULL != pout && '\0' != *pout) {
            int dump = 1;
            if (bender_auto_profile_output || NULL != getenv("BENDER_AUTO_PGO_ACTIVE")) {
                const char *minenv = getenv("BENDER_AUTO_PGO_MIN_CALLS");
                long mincalls = (NULL == minenv || '\0' == *minenv)
                    ? 10000L : strtol(minenv, NULL, 10);
                if (bender_profile_total_calls() < mincalls)
                    dump = 0;
            }
            if (dump) {
                bender_profile_dump(pout);
                /* A forced run first writes to an exclusively reserved
                 * .pending sidecar. Publish only its completed dump. */
                if (bender_auto_profile_output) {
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
                            if (MoveFileA(pout, completed)) bender_auto_workload_write(completed);
                            else remove(pout);
#else
                            /* Only the writer that actually published the
                             * profile describes it: a lost race must not
                             * attach this run's workload to another dump. */
                            if (0 == link(pout, completed))
                                bender_auto_workload_write(completed);
                            unlink(pout);
#endif
                            free(completed);
                        }
                    }
                }
            }
            bender_vm.profile_table = NULL;
        }
    }
    bender_profile_auto_output(NULL);
    bender_auto_workload_free();
    bender_clear_memory();
    bender_symcache_deinit();
    bender_free(bender_vm.roots);
    bender_vm.roots = NULL;
    bender_vm.root_count = 0;
    bender_vm.root_capacity = 0;
    bender_free(bender_vm.shadow_roots);
    bender_vm.shadow_roots = NULL;
    bender_vm.shadow_count = 0;
    bender_vm.shadow_capacity = 0;
    /* Slab pages went with clear_memory above; drop the state itself. */
    bender_free(bender_vm.slab_state);
    bender_vm.slab_state = NULL;
    bender_vm.abstract_registry = NULL;
    bender_vm.proc_registry = NULL;
    bender_vm.core_env = NULL;
    bender_vm.top_dyns = NULL;
    bender_vm.user = NULL;
    bender_free(bender_vm.traversal_base);
    bender_vm.fiber = NULL;
    bender_vm.root_fiber = NULL;
    bender_free(bender_vm.registry);
    bender_vm.registry = NULL;
#ifdef BENDER_EV
    bender_ev_deinit();
#endif
#ifdef BENDER_NET
    bender_net_deinit();
#endif
}
