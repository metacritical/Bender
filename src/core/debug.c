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
#include "gc.h"
#include "proc.h"
#include "state.h"
#include "util.h"
#include "vector.h"
#endif

/* Implements functionality to build a debugger from within bender.
 * The repl should also be able to serve as pretty featured debugger
 * out of the box. */

/* Add a break point to a function */
void bender_debug_break(BenderFuncDef *def, int32_t pc) {
    if (pc >= def->bytecode_length || pc < 0)
        bender_panic("invalid bytecode offset");
    def->bytecode[pc] |= 0x80;
    /* Breakpoint bits are exact-compare mismatches for native executors;
     * invalidate cached validation verdicts so the next native region
     * entry re-runs the full compare and stays boxed. */
    bender_native_invalidate();
}

/* Remove a break point from a function */
void bender_debug_unbreak(BenderFuncDef *def, int32_t pc) {
    if (pc >= def->bytecode_length || pc < 0)
        bender_panic("invalid bytecode offset");
    def->bytecode[pc] &= ~((uint32_t)0x80);
    bender_native_invalidate();
}

/*
 * Find a location for a breakpoint given a source file an
 * location.
 */
void bender_debug_find(
    BenderFuncDef **def_out, int32_t *pc_out,
    const uint8_t *source, int32_t sourceLine, int32_t sourceColumn) {
    /* Scan the heap for right func def */
    BenderGCObject *current = bender_vm.blocks;
    /* Keep track of the best source mapping we have seen so far */
    int32_t besti = -1;
    int32_t best_line = -1;
    int32_t best_column = -1;
    BenderFuncDef *best_def = NULL;
    while (NULL != current) {
        if ((current->flags & BENDER_MEM_TYPEBITS) == BENDER_MEMORY_FUNCDEF) {
            BenderFuncDef *def = (BenderFuncDef *)(current);
            if (def->sourcemap &&
                    def->source &&
                    !bender_string_compare(source, def->source)) {
                /* Correct source file, check mappings. The chosen
                 * pc index is the instruction closest to the given line column, but
                 * not after. */
                int32_t i;
                for (i = 0; i < def->bytecode_length; i++) {
                    int32_t line = def->sourcemap[i].line;
                    int32_t column = def->sourcemap[i].column;
                    if (line <= sourceLine && line >= best_line) {
                        if (column <= sourceColumn &&
                                (line > best_line || column > best_column)) {
                            best_line = line;
                            best_column = column;
                            besti = i;
                            best_def = def;
                        }
                    }
                }
            }
        }
        current = current->data.next;
    }
    if (best_def) {
        *def_out = best_def;
        *pc_out = besti;
    } else {
        bender_panic("could not find breakpoint");
    }
}

void bender_stacktrace(BenderFiber *fiber, Bender err) {
    const char *prefix = bender_checktype(err, BENDER_NIL) ? NULL : "";
    bender_stacktrace_ext(fiber, err, prefix);
}

/* Error reporting. This can be emulated from within Bender, but for
 * consistency with the top level code it is defined once. */
void bender_stacktrace_ext(BenderFiber *fiber, Bender err, const char *prefix) {

    int32_t fi;
    const char *errstr = (const char *)bender_to_string(err);
    BenderFiber **fibers = NULL;
    int wrote_error = !prefix;

    int print_color = bender_truthy(bender_dyn("err-color"));
    if (print_color) bender_eprintf("\x1b[31m");

    while (fiber) {
        bender_v_push(fibers, fiber);
        fiber = fiber->child;
    }

    for (fi = bender_v_count(fibers) - 1; fi >= 0; fi--) {
        fiber = fibers[fi];
        int32_t i = fiber->frame;
        while (i > 0) {
            BenderCFunRegistry *reg = NULL;
            BenderStackFrame *frame = (BenderStackFrame *)(fiber->data + i - BENDER_FRAME_SIZE);
            BenderFuncDef *def = NULL;
            i = frame->prevframe;

            /* Print prelude to stack frame */
            if (!wrote_error) {
                BenderFiberStatus status = bender_fiber_status(fiber);
                bender_eprintf("%s%s: %s\n",
                              prefix ? prefix : "",
                              bender_status_names[status],
                              errstr ? errstr : bender_status_names[status]);
                wrote_error = 1;
            }

            bender_eprintf("  in");

            if (frame->func) {
                def = frame->func->def;
                bender_eprintf(" %s", def->name ? (const char *)def->name : "<anonymous>");
                if (def->source) {
                    bender_eprintf(" [%s]", (const char *)def->source);
                }
            } else {
                BenderCFunction cfun = (BenderCFunction)(frame->pc);
                if (cfun) {
                    reg = bender_registry_get(cfun);
                    if (NULL != reg && NULL != reg->name) {
                        if (reg->name_prefix) {
                            bender_eprintf(" %s/%s", reg->name_prefix, reg->name);
                        } else {
                            bender_eprintf(" %s", reg->name);
                        }
                        if (NULL != reg->source_file) {
                            bender_eprintf(" [%s]", reg->source_file);
                        }
                    } else {
                        bender_eprintf(" <cfunction>");
                    }
                }
            }
            if (frame->flags & BENDER_STACKFRAME_TAILCALL)
                bender_eprintf(" (tail call)");
            if (frame->func && frame->pc) {
                int32_t off = (int32_t)(frame->pc - def->bytecode);
                if (def->sourcemap) {
                    BenderSourceMapping mapping = def->sourcemap[off];
                    bender_eprintf(" on line %d, column %d", mapping.line, mapping.column);
                } else {
                    bender_eprintf(" pc=%d", off);
                }
            } else if (NULL != reg) {
                /* C Function */
                if (reg->source_line > 0) {
                    bender_eprintf(" on line %d", (long) reg->source_line);
                }
            }
            bender_eprintf("\n");
            /* Print fiber points optionally. Clutters traces but provides info
            if (i <= 0 && fi > 0) {
                bender_eprintf("  in parent fiber\n");
            }
            */
        }
    }

    if (print_color) bender_eprintf("\x1b[0m");

    bender_v_free(fibers);
}

/*
 * CFuns
 */

/* Helper to find funcdef and bytecode offset to insert or remove breakpoints.
 * Takes a source file name and byte offset. */
static void helper_find(int32_t argc, Bender *argv, BenderFuncDef **def, int32_t *bytecode_offset) {
    bender_fixarity(argc, 3);
    const uint8_t *source = bender_getstring(argv, 0);
    int32_t line = bender_getinteger(argv, 1);
    int32_t col = bender_getinteger(argv, 2);
    bender_debug_find(def, bytecode_offset, source, line, col);
}

/* Helper to find funcdef and bytecode offset to insert or remove breakpoints.
 * Takes a function and byte offset*/
static void helper_find_fun(int32_t argc, Bender *argv, BenderFuncDef **def, int32_t *bytecode_offset) {
    bender_arity(argc, 1, 2);
    BenderFunction *func = bender_getfunction(argv, 0);
    int32_t offset = (argc == 2) ? bender_getinteger(argv, 1) : 0;
    *def = func->def;
    *bytecode_offset = offset;
}

BENDER_CORE_FN(cfun_debug_break,
              "(debug/break source line col)",
              "Sets a breakpoint in `source` at a given line and column. "
              "Will throw an error if the breakpoint location "
              "cannot be found. For example\n\n"
              "\t(debug/break \"core.ben\" 10 4)\n\n"
              "will set a breakpoint at line 10, 4th column of the file core.ben.") {
    BenderFuncDef *def;
    int32_t offset;
    helper_find(argc, argv, &def, &offset);
    bender_debug_break(def, offset);
    return bender_wrap_nil();
}

BENDER_CORE_FN(cfun_debug_unbreak,
              "(debug/unbreak source line col)",
              "Remove a breakpoint from `source` at a given `line` and "
              "`col`. Will throw an error if the breakpoint cannot be "
              "found.") {
    BenderFuncDef *def;
    int32_t offset = 0;
    helper_find(argc, argv, &def, &offset);
    bender_debug_unbreak(def, offset);
    return bender_wrap_nil();
}

BENDER_CORE_FN(cfun_debug_fbreak,
              "(debug/fbreak fun &opt pc)",
              "Set a breakpoint in a given function. pc is an optional offset, which "
              "is in bytecode instructions. fun is a function value. Will throw an error "
              "if the offset is too large or negative.") {
    BenderFuncDef *def;
    int32_t offset = 0;
    helper_find_fun(argc, argv, &def, &offset);
    bender_debug_break(def, offset);
    return bender_wrap_nil();
}

BENDER_CORE_FN(cfun_debug_unfbreak,
              "(debug/unfbreak fun &opt pc)",
              "Unset a breakpoint set with debug/fbreak.") {
    BenderFuncDef *def;
    int32_t offset;
    helper_find_fun(argc, argv, &def, &offset);
    bender_debug_unbreak(def, offset);
    return bender_wrap_nil();
}

BENDER_CORE_FN(cfun_debug_lineage,
              "(debug/lineage fib)",
              "Returns an array of all child fibers from a root fiber. This function "
              "is useful when a fiber signals or errors to an ancestor fiber. Using this function, "
              "the fiber handling the error can see which fiber raised the signal. This function should "
              "be used mostly for debugging purposes.") {
    bender_fixarity(argc, 1);
    BenderFiber *fiber = bender_getfiber(argv, 0);
    BenderArray *array = bender_array(0);
    while (fiber) {
        bender_array_push(array, bender_wrap_fiber(fiber));
        fiber = fiber->child;
    }
    return bender_wrap_array(array);
}

/* Extract info from one stack frame */
static Bender doframe(BenderStackFrame *frame) {
    int32_t off;
    BenderTable *t = bender_table(3);
    BenderFuncDef *def = NULL;
    if (frame->func) {
        bender_table_put(t, bender_ckeywordv("function"), bender_wrap_function(frame->func));
        def = frame->func->def;
        if (def->name) {
            bender_table_put(t, bender_ckeywordv("name"), bender_wrap_string(def->name));
        }
    } else {
        BenderCFunction cfun = (BenderCFunction)(frame->pc);
        if (cfun) {
            BenderCFunRegistry *reg = bender_registry_get(cfun);
            if (NULL != reg->name) {
                if (NULL != reg->name_prefix) {
                    bender_table_put(t, bender_ckeywordv("name"), bender_wrap_string(bender_formatc("%s/%s", reg->name_prefix, reg->name)));
                } else {
                    bender_table_put(t, bender_ckeywordv("name"), bender_cstringv(reg->name));
                }
                if (NULL != reg->source_file) {
                    bender_table_put(t, bender_ckeywordv("source"), bender_cstringv(reg->source_file));
                }
                if (reg->source_line > 0) {
                    bender_table_put(t, bender_ckeywordv("source-line"), bender_wrap_integer(reg->source_line));
                    bender_table_put(t, bender_ckeywordv("source-column"), bender_wrap_integer(1));
                }
            }
        }
        bender_table_put(t, bender_ckeywordv("c"), bender_wrap_true());
    }
    if (frame->flags & BENDER_STACKFRAME_TAILCALL) {
        bender_table_put(t, bender_ckeywordv("tail"), bender_wrap_true());
    }
    if (frame->func && frame->pc) {
        Bender *stack = (Bender *)frame + BENDER_FRAME_SIZE;
        BenderArray *slots;
        bender_assert(def != NULL, "def != NULL");
        off = (int32_t)(frame->pc - def->bytecode);
        bender_table_put(t, bender_ckeywordv("pc"), bender_wrap_integer(off));
        if (def->sourcemap) {
            BenderSourceMapping mapping = def->sourcemap[off];
            bender_table_put(t, bender_ckeywordv("source-line"), bender_wrap_integer(mapping.line));
            bender_table_put(t, bender_ckeywordv("source-column"), bender_wrap_integer(mapping.column));
        }
        if (def->source) {
            bender_table_put(t, bender_ckeywordv("source"), bender_wrap_string(def->source));
        }
        /* Add stack arguments */
        slots = bender_array(def->slotcount);
        safe_memcpy(slots->data, stack, sizeof(Bender) * def->slotcount);
        slots->count = def->slotcount;
        bender_table_put(t, bender_ckeywordv("slots"), bender_wrap_array(slots));
        /* Add local bindings */
        if (def->symbolmap) {
            BenderTable *local_bindings = bender_table(0);
            for (int32_t i = def->symbolmap_length - 1; i >= 0; i--) {
                BenderSymbolMap jsm = def->symbolmap[i];
                Bender value = bender_wrap_nil();
                uint32_t pc = (uint32_t)(frame->pc - def->bytecode);
                if (jsm.birth_pc == UINT32_MAX) {
                    /* death_pc has secondary meaning here, we use it to encode the environment index */
                    if (jsm.death_pc < (uint32_t) def->environments_length) {
                        /* death_pc is in valid range */
                        BenderFuncEnv *env = frame->func->envs[jsm.death_pc];
                        if (jsm.slot_index < (uint32_t) env->length) {
                            /* slot_index range good */
                            if (env->offset > 0) {
                                /* On stack */
                                value = env->as.fiber->data[env->offset + jsm.slot_index];
                            } else {
                                /* Off stack */
                                value = env->as.values[jsm.slot_index];
                            }
                        }
                    }
                } else if (pc >= jsm.birth_pc && pc < jsm.death_pc) {
                    if (jsm.slot_index < (uint32_t) def->slotcount) {
                        value = stack[jsm.slot_index];
                    }
                }
                bender_table_put(local_bindings, bender_wrap_symbol(jsm.symbol), value);
            }
            bender_table_put(t, bender_ckeywordv("locals"), bender_wrap_table(local_bindings));
        }
    }
    return bender_wrap_table(t);
}

BENDER_CORE_FN(cfun_debug_stack,
              "(debug/stack fib)",
              "Gets information about the stack as an array of tables. Each table "
              "in the array contains information about a stack frame. The top-most, current "
              "stack frame is the first table in the array, and the bottom-most stack frame "
              "is the last value. Each stack frame contains some of the following attributes:\n\n"
              "* :c - true if the stack frame is a c function invocation\n\n"
              "* :source-column - the current source column of the stack frame\n\n"
              "* :function - the function that the stack frame represents\n\n"
              "* :source-line - the current source line of the stack frame\n\n"
              "* :name - the human-friendly name of the function\n\n"
              "* :pc - integer indicating the location of the program counter\n\n"
              "* :source - string with the file path or other identifier for the source code\n\n"
              "* :slots - array of all values in each slot\n\n"
              "* :tail - boolean indicating a tail call") {
    bender_fixarity(argc, 1);
    BenderFiber *fiber = bender_getfiber(argv, 0);
    BenderArray *array = bender_array(0);
    {
        int32_t i = fiber->frame;
        BenderStackFrame *frame;
        while (i > 0) {
            frame = (BenderStackFrame *)(fiber->data + i - BENDER_FRAME_SIZE);
            bender_array_push(array, doframe(frame));
            i = frame->prevframe;
        }
    }
    return bender_wrap_array(array);
}

BENDER_CORE_FN(cfun_debug_stacktrace,
              "(debug/stacktrace fib &opt err prefix)",
              "Prints a nice looking stack trace for a fiber `fib`. "
              "Optionally provide an error value `err` to print the "
              "stack trace with. If `prefix` is nil or not provided, skips "
              "the error line. Returns `fib`.") {
    bender_arity(argc, 1, 3);
    BenderFiber *fiber = bender_getfiber(argv, 0);
    Bender x = argc == 1 ? bender_wrap_nil() : argv[1];
    const char *prefix = bender_optcstring(argv, argc, 2, NULL);
    bender_stacktrace_ext(fiber, x, prefix);
    return argv[0];
}

BENDER_CORE_FN(cfun_debug_argstack,
              "(debug/arg-stack fib)",
              "Gets all values currently on a fiber `fib`'s argument stack. "
              "Normally this is empty unless `fib` signals while pushing "
              "arguments to make a function call. Returns a new array.") {
    bender_fixarity(argc, 1);
    BenderFiber *fiber = bender_getfiber(argv, 0);
    BenderArray *array = bender_array(fiber->stacktop - fiber->stackstart);
    if (array->capacity) {
        memcpy(array->data, fiber->data + fiber->stackstart, array->capacity * sizeof(Bender));
        array->count = array->capacity;
    }
    return bender_wrap_array(array);
}

BENDER_CORE_FN(cfun_debug_step,
              "(debug/step fib &opt x)",
              "Run a fiber `fib` for one virtual instruction of the Bender "
              "machine. Optionally pass in a value `x` that will be passed "
              "as the resuming value. Returns the signal value, which is "
              "usually nil, as breakpoints raise nil signals.") {
    bender_arity(argc, 1, 2);
    BenderFiber *fiber = bender_getfiber(argv, 0);
    Bender out = bender_wrap_nil();
    bender_step(fiber, argc == 1 ? bender_wrap_nil() : argv[1], &out);
    return out;
}

/* PGO type-profile collector controls (see profile collection in
 * vm.c). BENDER_PROFILE=1 in the environment auto-starts collection for
 * script runs (see cli-main in boot.ben); dump with an explicit path. */
BENDER_CORE_FN(cfun_debug_profile_start,
              "(debug/profile-start)",
              "Start the VM type-profile collector (PGO phase 1). Records "
              "observed parameter and return types per function while "
              "active. Returns nil.") {
    bender_fixarity(argc, 0);
    (void) argv;
    bender_profile_start();
    return bender_wrap_nil();
}

BENDER_CORE_FN(cfun_debug_profile_stop,
              "(debug/profile-stop)",
              "Stop collection, keeping the data. Returns the profile "
              "table (nil if never started).") {
    bender_fixarity(argc, 0);
    (void) argv;
    return bender_profile_stop();
}

BENDER_CORE_FN(cfun_debug_profile_reset,
              "(debug/profile-reset)",
              "Clear collected data (keeps recording if active). Returns nil.") {
    bender_fixarity(argc, 0);
    (void) argv;
    bender_profile_reset();
    return bender_wrap_nil();
}

BENDER_CORE_FN(cfun_debug_profile_dump,
              "(debug/profile-dump path)",
              "Write the collected type profile to `path` as Bender data: "
              "an array of [[source line name] {:calls n :params [...] "
              ":rets [...]}] rows with observed types as keywords. "
              "Panics if profiling was never started. Returns nil.") {
    bender_fixarity(argc, 1);
    const char *path = bender_getcstring(argv, 0);
    bender_profile_dump(path);
    return bender_wrap_nil();
}

BENDER_CORE_FN(cfun_debug_slab_stats,
              "(debug/slab-stats)",
              "Step 1 (process heaps) introspection: per-owner slab page "
              "counts as @{:pages total :unowned n :owned @{id count}}. "
              "Owner ids are per-fiber numbers (0 means shared/host); "
              "fresh fibers get strictly new ids. Returns the table.") {
    bender_fixarity(argc, 0);
    (void) argv;
    return bender_wrap_table(bender_slab_owner_stats());
}

BENDER_CORE_FN(cfun_debug_fiber_owner,
              "(debug/fiber-owner)",
              "Step 1 (process heaps) introspection: the owner id of the "
              "calling fiber (nonzero; strictly increasing across fibers "
              "in a VM). Page claims record this id. Returns the integer.") {
    bender_fixarity(argc, 0);
    (void) argv;
    if (NULL == bender_vm.fiber) return bender_wrap_integer(0);
    return bender_wrap_integer((int64_t) bender_vm.fiber->gc_owner);
}

BENDER_CORE_FN(cfun_debug_fiber_nursery,
              "(debug/fiber-nursery)",
              "Step 1.2 introspection: lifetime slab bytes charged to the "
              "calling fiber's nursery (bump + recycled paths). Never "
              "reset (per-fiber minors in 1.3 will). Returns the integer.") {
    bender_fixarity(argc, 0);
    (void) argv;
    if (NULL == bender_vm.fiber) return bender_wrap_integer(0);
    return bender_wrap_integer((int64_t) bender_vm.fiber->gc_nursery_bytes);
}

BENDER_CORE_FN(cfun_debug_collect_fiber,
              "(debug/collect-fiber fiber)",
              "Step 1.3: run a minor collection for one fiber's nursery "
              "(frees only its young unmarked blocks; foreign young floats "
              "until a global collection). Falls back to a full minor for "
              "unowned fibers and stuck remembered sets. Returns nil.") {
    bender_fixarity(argc, 1);
    BenderFiber *fiber = bender_getfiber(argv, 0);
    bender_collect_fiber_minor(fiber);
    return bender_wrap_nil();
}

BENDER_CORE_FN(cfun_debug_collect_proc,
              "(debug/collect-proc p)",
              "P3.4: run a per-proc major for strict process `p` (frees "
              "only its young unmarked; foreign young floats). Skips "
              "foreign fiber stacks (sound under strict isolation: "
              "spawn-copy + export panics + copy sends; FFI/manual-C "
              "smuggling stays documented-unsound — audit with "
              "BENDER_PROC_VERIFY=1). Falls back to a full minor for "
              "non-strict/dead/running fibers. Returns nil.") {
    bender_fixarity(argc, 1);
    void *abst = bender_getabstract(argv, 0, &bender_proc_type);
    bender_collect_process((struct BenderProcess *) abst);
    return bender_wrap_nil();
}

/* Module entry point */
void bender_lib_debug(BenderTable *env) {
    BenderRegExt debug_cfuns[] = {
        BENDER_CORE_REG("debug/break", cfun_debug_break),
        BENDER_CORE_REG("debug/unbreak", cfun_debug_unbreak),
        BENDER_CORE_REG("debug/fbreak", cfun_debug_fbreak),
        BENDER_CORE_REG("debug/unfbreak", cfun_debug_unfbreak),
        BENDER_CORE_REG("debug/arg-stack", cfun_debug_argstack),
        BENDER_CORE_REG("debug/stack", cfun_debug_stack),
        BENDER_CORE_REG("debug/stacktrace", cfun_debug_stacktrace),
        BENDER_CORE_REG("debug/lineage", cfun_debug_lineage),
        BENDER_CORE_REG("debug/step", cfun_debug_step),
        BENDER_CORE_REG("debug/profile-start", cfun_debug_profile_start),
        BENDER_CORE_REG("debug/profile-stop", cfun_debug_profile_stop),
        BENDER_CORE_REG("debug/profile-reset", cfun_debug_profile_reset),
        BENDER_CORE_REG("debug/profile-dump", cfun_debug_profile_dump),
        BENDER_CORE_REG("debug/slab-stats", cfun_debug_slab_stats),
        BENDER_CORE_REG("debug/fiber-owner", cfun_debug_fiber_owner),
        BENDER_CORE_REG("debug/fiber-nursery", cfun_debug_fiber_nursery),
        BENDER_CORE_REG("debug/collect-fiber", cfun_debug_collect_fiber),
        BENDER_CORE_REG("debug/collect-proc", cfun_debug_collect_proc),
        BENDER_REG_END
    };
    bender_core_cfuns_ext(env, NULL, debug_cfuns);
}
