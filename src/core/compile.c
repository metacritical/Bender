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
#include "compile.h"
#include "emit.h"
#include "vector.h"
#include "util.h"
#include "state.h"
#include "regalloc.h"
#endif

/* Core `string` cfunction (defined in corelib.c): identified by pointer
 * for call-site constant folding below. */
Bender bender_core_string(int32_t argc, Bender *argv);

BenderFopts benderc_fopts_default(BenderCompiler *c) {
    BenderFopts ret;
    ret.compiler = c;
    ret.flags = 0;
    ret.hint = benderc_cslot(bender_wrap_nil());
    return ret;
}

/* Throw an error with a bender string. */
void benderc_error(BenderCompiler *c, const uint8_t *m) {
    /* Don't override first error */
    if (c->result.status == BENDER_COMPILE_ERROR) {
        return;
    }
    c->result.status = BENDER_COMPILE_ERROR;
    c->result.error = m;
}

/* Throw an error with a message in a cstring */
void benderc_cerror(BenderCompiler *c, const char *m) {
    benderc_error(c, bender_cstring(m));
}

static const char *bender_lint_level_names[] = {
    "relaxed",
    "normal",
    "strict"
};

/* Emit compiler linter messages */
void benderc_lintf(BenderCompiler *c, BenderCompileLintLevel level, const char *format, ...) {
    if (NULL != c->lints) {
        /* format message */
        va_list args;
        BenderBuffer buffer;
        int32_t len = 0;
        while (format[len]) len++;
        bender_buffer_init(&buffer, len);
        va_start(args, format);
        bender_formatbv(&buffer, format, args);
        va_end(args);
        const uint8_t *str = bender_string(buffer.data, buffer.count);
        bender_buffer_deinit(&buffer);
        /* construct linting payload */
        Bender *payload = bender_tuple_begin(4);
        payload[0] = bender_ckeywordv(bender_lint_level_names[level]);
        payload[1] = c->current_mapping.line == -1 ? bender_wrap_nil() : bender_wrap_integer(c->current_mapping.line);
        payload[2] = c->current_mapping.column == -1 ? bender_wrap_nil() : bender_wrap_integer(c->current_mapping.column);
        payload[3] = bender_wrap_string(str);
        bender_array_push(c->lints, bender_wrap_tuple(bender_tuple_end(payload)));
    }
}

/* Free a slot */
void benderc_freeslot(BenderCompiler *c, BenderSlot s) {
    if (s.flags & (BENDER_SLOT_CONSTANT | BENDER_SLOT_REF | BENDER_SLOT_NAMED)) return;
    if (s.envindex >= 0) return;
    benderc_regalloc_free(&c->scope->ra, s.index);
}

/* Add a slot to a scope with a symbol associated with it (def or var). */
void benderc_nameslot(BenderCompiler *c, const uint8_t *sym, BenderSlot s, uint32_t flags) {
    if (!(flags & BENDER_DEFFLAG_NO_SHADOWCHECK)) {
        if (sym[0] != '_') {
            switch (benderc_shadowcheck(c, sym)) {
                default:
                    break;
                case BENDERC_SHADOW_MACRO:
                    benderc_lintf(c, BENDER_C_LINT_NORMAL, "binding %q is shadowing a macro", bender_wrap_symbol(sym));
                    break;
                case BENDERC_SHADOW_LOCAL_HIDES_LOCAL:
                    benderc_lintf(c, BENDER_C_LINT_STRICT, "binding %q is shadowing a binding", bender_wrap_symbol(sym));
                    break;
                case BENDERC_SHADOW_LOCAL_HIDES_GLOBAL:
                    benderc_lintf(c, BENDER_C_LINT_STRICT, "binding %q is shadowing a top-level binding", bender_wrap_symbol(sym));
                    break;
                case BENDERC_SHADOW_GLOBAL_HIDES_GLOBAL:
                    benderc_lintf(c, BENDER_C_LINT_STRICT, "top-level binding %q is shadowing another top-level binding", bender_wrap_symbol(sym));
                    break;
            }
        }
    }
    SymPair sp;
    int32_t cnt = bender_v_count(c->buffer);
    sp.sym = sym;
    sp.sym2 = sym;
    sp.slot = s;
    sp.keep = 0;
    if (flags & BENDER_DEFFLAG_NO_UNUSED) {
        sp.referenced = 1;
    } else {
        sp.referenced = sym[0] == '_'; /* Fake ref if symbol starts with _ to avoid lints */
    }
    sp.slot.flags |= BENDER_SLOT_NAMED;
    sp.birth_pc = cnt ? cnt - 1 : 0;
    sp.death_pc = UINT32_MAX;
    bender_v_push(c->scope->syms, sp);
}

/* Create a slot with a constant */
BenderSlot benderc_cslot(Bender x) {
    BenderSlot ret;
    ret.flags = (1 << bender_type(x)) | BENDER_SLOT_CONSTANT;
    ret.index = -1;
    ret.constant = x;
    ret.envindex = -1;
    return ret;
}

/* Get a local slot */
BenderSlot benderc_farslot(BenderCompiler *c) {
    BenderSlot ret;
    ret.flags = BENDER_SLOTTYPE_ANY;
    ret.index = benderc_allocfar(c);
    ret.constant = bender_wrap_nil();
    ret.envindex = -1;
    return ret;
}

/* Enter a new scope */
void benderc_scope(BenderScope *s, BenderCompiler *c, int flags, const char *name) {
    BenderScope scope;
    scope.name = name;
    scope.child = NULL;
    scope.consts = NULL;
    scope.syms = NULL;
    scope.envs = NULL;
    scope.defs = NULL;
    scope.bytecode_start = bender_v_count(c->buffer);
    scope.flags = flags;
    scope.parent = c->scope;
    benderc_regalloc_init(&scope.ua);
    /* Inherit slots */
    if ((!(flags & BENDER_SCOPE_FUNCTION)) && c->scope) {
        benderc_regalloc_clone(&scope.ra, &(c->scope->ra));
    } else {
        benderc_regalloc_init(&scope.ra);
    }
    /* Link parent and child and update pointer */
    if (c->scope)
        c->scope->child = s;
    c->scope = s;
    *s = scope;
}

/* Leave a scope. */
void benderc_popscope(BenderCompiler *c) {
    BenderScope *oldscope = c->scope;
    BenderScope *newscope = oldscope->parent;
    /* A failed/unused function may pop without producing a FuncDef. Never
     * leave its stack address available for a later sibling scope to reuse. */
    for (int32_t i = 0; i < bender_v_count(c->retained_defs); i++) {
        if (c->retained_defs[i].scope == oldscope)
            c->retained_defs[i].scope = NULL;
    }
    /* Move free slots to parent scope if not a new function.
     * We need to know the total number of slots used when compiling the function. */
    if (!(oldscope->flags & (BENDER_SCOPE_FUNCTION | BENDER_SCOPE_UNUSED)) && newscope) {
        /* Parent scopes inherit child's closure flag. Needed
         * for while loops. (if a while loop creates a closure, it
         * is compiled to a tail recursive iife) */
        if (oldscope->flags & BENDER_SCOPE_CLOSURE) {
            newscope->flags |= BENDER_SCOPE_CLOSURE;
        }
        if (newscope->ra.max < oldscope->ra.max) {
            newscope->ra.max = oldscope->ra.max;
        }

        /* Keep upvalue slots and symbols for debugging. */
        for (int32_t i = 0; i < bender_v_count(oldscope->syms); i++) {
            SymPair pair = oldscope->syms[i];
            /* Check for unused symbols */
            if (pair.referenced == 0 && pair.sym) {
                benderc_lintf(c, BENDER_C_LINT_STRICT, "binding %q is unused", bender_wrap_symbol(pair.sym));
            }
            /* The variable should not be lexically accessible */
            pair.sym = NULL;
            if (pair.death_pc == UINT32_MAX) {
                pair.death_pc = (uint32_t) bender_v_count(c->buffer);
            }
            if (pair.keep) {
                /* The variable should also not be included in the locals */
                pair.sym2 = NULL;
                benderc_regalloc_touch(&newscope->ra, pair.slot.index);
            }
            bender_v_push(newscope->syms, pair);
        }
    }

    /* Free the old scope */
    bender_v_free(oldscope->consts);
    bender_v_free(oldscope->syms);
    bender_v_free(oldscope->envs);
    bender_v_free(oldscope->defs);
    benderc_regalloc_deinit(&oldscope->ra);
    benderc_regalloc_deinit(&oldscope->ua);
    /* Update pointer */
    if (newscope)
        newscope->child = NULL;
    c->scope = newscope;
}

/* Leave a scope but keep a slot allocated. */
void benderc_popscope_keepslot(BenderCompiler *c, BenderSlot retslot) {
    BenderScope *scope;
    benderc_popscope(c);
    scope = c->scope;
    if (scope && retslot.envindex < 0 && retslot.index >= 0) {
        benderc_regalloc_touch(&scope->ra, retslot.index);
    }
}

static int lookup_missing(
    BenderCompiler *c,
    const uint8_t *sym,
    BenderFunction *handler,
    BenderBinding *out) {
    int32_t minar = handler->def->min_arity;
    int32_t maxar = handler->def->max_arity;
    if (minar > 1 || maxar < 1) {
        benderc_error(c, bender_cstring("missing symbol lookup handler must take 1 argument"));
        return 0;
    }
    Bender args[1] = { bender_wrap_symbol(sym) };
    BenderFiber *fiberp = bender_fiber(handler, 64, 1, args);
    if (NULL == fiberp) {
        benderc_error(c, bender_cstring("failed to call missing symbol lookup handler"));
        return 0;
    }
    fiberp->env = c->env;
    int lock = bender_gclock();
    Bender tempOut;
    BenderSignal status;
    /* M4b-2: quantum-INTERRUPT is transparent here (compile must
     * complete) — resume again. Quiesce still unwinds. */
    do {
        status = bender_continue(fiberp, bender_wrap_nil(), &tempOut);
    } while (status == BENDER_SIGNAL_INTERRUPT && bender_vm.quantum &&
            !bender_atomic_load_relaxed(&bender_vm.auto_suspend));
    bender_gcunlock(lock);
    if (status != BENDER_SIGNAL_OK) {
        benderc_error(c, bender_formatc("(lookup) %V", tempOut));
        return 0;
    }

    /* Convert return value as entry. */
    /* Alternative could use bender_resolve_ext(c->env, sym) to read result from environment. */
    *out = bender_binding_from_entry(tempOut);
    return 1;
}

/* Check if a binding is defined in an upper scope. This lets us check for
 * variable shadowing. */
Shadowing benderc_shadowcheck(BenderCompiler *c, const uint8_t *sym) {
    /* Check locals */
    BenderScope *scope = c->scope;
    int is_global = (scope->flags & BENDER_SCOPE_TOP);
    while (scope) {
        int32_t len = bender_v_count(scope->syms);
        for (int32_t i = len - 1; i >= 0; i--) {
            SymPair *pair = scope->syms + i;
            if (pair->sym == sym) {
                if (is_global) {
                    /* This can happen in a top level form like (def [x [x y]] [1 [2 3]])` where a symbol is reused in one expression */
                    /* bender_assert(!is_global, "shadowing analysis is incorrect. compiler bug"); */
                    return BENDERC_SHADOW_GLOBAL_HIDES_GLOBAL;
                }
                return BENDERC_SHADOW_LOCAL_HIDES_LOCAL;
            }
        }
        scope = scope->parent;
    }
    /* Check globals */
    BenderBinding binding = bender_resolve_ext(c->env, sym);
    if (binding.type == BENDER_BINDING_MACRO || binding.type == BENDER_BINDING_DYNAMIC_MACRO) {
        return BENDERC_SHADOW_MACRO;
    } else if (binding.type == BENDER_BINDING_NONE) {
        return BENDERC_SHADOW_NONE;
    } else {
        if (is_global) {
            return BENDERC_SHADOW_GLOBAL_HIDES_GLOBAL;
        } else {
            return BENDERC_SHADOW_LOCAL_HIDES_GLOBAL;
        }
    }
}

/* Allow searching for symbols. Return information about the symbol */
BenderSlot benderc_resolve(
    BenderCompiler *c,
    const uint8_t *sym) {

    BenderSlot ret = benderc_cslot(bender_wrap_nil());
    BenderScope *scope = c->scope;
    SymPair *pair;
    int foundlocal = 1;
    int unused = 0;

    /* Search scopes for symbol, starting from top */
    while (scope) {
        int32_t i, len;
        if (scope->flags & BENDER_SCOPE_UNUSED)
            unused = 1;
        len = bender_v_count(scope->syms);
        /* Search in reverse order */
        for (i = len - 1; i >= 0; i--) {
            pair = scope->syms + i;
            if (pair->sym == sym) {
                ret = pair->slot;
                pair->referenced = 1;
                goto found;
            }
        }
        if (scope->flags & BENDER_SCOPE_FUNCTION)
            foundlocal = 0;
        scope = scope->parent;
    }

    /* Symbol not found - check for global */
    {
        BenderBinding binding = bender_resolve_ext(c->env, sym);
        if (binding.type == BENDER_BINDING_NONE) {
            Bender handler = bender_table_get_keyword(c->env, "missing-symbol");
            switch (bender_type(handler)) {
                case BENDER_NIL:
                    break;
                case BENDER_FUNCTION:
                    if (!lookup_missing(c, sym, bender_unwrap_function(handler), &binding))
                        return benderc_cslot(bender_wrap_nil());
                    break;
                default:
                    benderc_error(c, bender_formatc("invalid lookup handler %V", handler));
                    return benderc_cslot(bender_wrap_nil());
            }
        }

        switch (binding.type) {
            default:
            case BENDER_BINDING_NONE:
                benderc_error(c, bender_formatc("unknown symbol %q", bender_wrap_symbol(sym)));
                return benderc_cslot(bender_wrap_nil());
            case BENDER_BINDING_DEF:
            case BENDER_BINDING_MACRO: /* Macro should function like defs when not in calling pos */
                ret = benderc_cslot(binding.value);
                break;
            case BENDER_BINDING_DYNAMIC_DEF:
            case BENDER_BINDING_DYNAMIC_MACRO:
                ret = benderc_cslot(binding.value);
                ret.flags |= BENDER_SLOT_REF | BENDER_SLOT_NAMED | BENDER_SLOTTYPE_ANY;
                ret.flags &= ~BENDER_SLOT_CONSTANT;
                break;
            case BENDER_BINDING_VAR: {
                ret = benderc_cslot(binding.value);
                ret.flags |= BENDER_SLOT_REF | BENDER_SLOT_NAMED | BENDER_SLOT_MUTABLE | BENDER_SLOTTYPE_ANY;
                ret.flags &= ~BENDER_SLOT_CONSTANT;
                break;
            }
        }
        BenderCompileLintLevel depLevel = BENDER_C_LINT_RELAXED;
        switch (binding.deprecation) {
            case BENDER_BINDING_DEP_NONE:
                break;
            case BENDER_BINDING_DEP_RELAXED:
                depLevel = BENDER_C_LINT_RELAXED;
                break;
            case BENDER_BINDING_DEP_NORMAL:
                depLevel = BENDER_C_LINT_NORMAL;
                break;
            case BENDER_BINDING_DEP_STRICT:
                depLevel = BENDER_C_LINT_STRICT;
                break;
        }
        if (binding.deprecation != BENDER_BINDING_DEP_NONE) {
            benderc_lintf(c, depLevel, "%q is deprecated", bender_wrap_symbol(sym));
        }
        return ret;
    }

    /* Symbol was found */
found:

    /* Constants can be returned immediately (they are stateless) */
    if (ret.flags & (BENDER_SLOT_CONSTANT | BENDER_SLOT_REF))
        return ret;

    /* Unused references and locals shouldn't add captured envs. */
    if (foundlocal) {
        ret.envindex = -1;
        return ret;
    }
    if (unused) {
        ret.index = 0; /* we must have at least 1 slot if we are calling resolve in scope */
        ret.envindex = -1;
        return ret;
    }

    /* Upvalue instructions encode the parent's slot in one byte. Reject a
     * far slot before recording the capture, rather than truncating it and
     * reading or writing an unrelated parent local at runtime. */
    if (ret.index > 0xFF) {
        benderc_error(c, bender_formatc("cannot capture parent local %q: captured upvalue slot %d exceeds bytecode limit 255",
                                      bender_wrap_symbol(sym), ret.index));
        return benderc_cslot(bender_wrap_nil());
    }

    /* non-local scope needs to expose its environment */
    BenderScope *original_scope = scope;
    pair->keep = 1;
    pair->referenced = 1;
    while (scope && !(scope->flags & BENDER_SCOPE_FUNCTION))
        scope = scope->parent;
    bender_assert(scope, "invalid scopes");
    scope->flags |= BENDER_SCOPE_ENV;

    /* In the function scope, allocate the slot as an upvalue */
    benderc_regalloc_touch(&scope->ua, ret.index);

    /* Iterate through child scopes and make sure environment is propagated */
    scope = scope->child;

    /* Propagate env up to current scope */
    int32_t envindex = -1;
    while (scope) {
        if (scope->flags & BENDER_SCOPE_FUNCTION) {
            int32_t j, len;
            int scopefound = 0;
            /* Check if scope already has env. If so, break */
            len = bender_v_count(scope->envs);
            for (j = 0; j < len; j++) {
                if (scope->envs[j].envindex == envindex) {
                    scopefound = 1;
                    envindex = j;
                    break;
                }
            }
            /* Add the environment if it is not already referenced */
            if (!scopefound) {
                len = bender_v_count(scope->envs);
                BenderEnvRef ref;
                ref.envindex = envindex;
                ref.scope = original_scope;
                bender_v_push(scope->envs, ref);
                envindex = len;
            }
        }
        scope = scope->child;
    }

    ret.envindex = envindex;
    return ret;
}

/* Generate the return instruction for a slot. */
BenderSlot benderc_return(BenderCompiler *c, BenderSlot s) {
    if (!(s.flags & BENDER_SLOT_RETURNED)) {
        if (s.flags & BENDER_SLOT_CONSTANT && bender_checktype(s.constant, BENDER_NIL))
            benderc_emit(c, JOP_RETURN_NIL);
        else
            benderc_emit_s(c, JOP_RETURN, s, 0);
        s.flags |= BENDER_SLOT_RETURNED;
    }
    return s;
}

/* Get a target slot for emitting an instruction. */
BenderSlot benderc_gettarget(BenderFopts opts) {
    BenderSlot slot;
    if ((opts.flags & BENDER_FOPTS_HINT) &&
            (opts.hint.envindex < 0) &&
            (opts.hint.index >= 0 && opts.hint.index <= 0xFF)) {
        slot = opts.hint;
    } else {
        slot.envindex = -1;
        slot.constant = bender_wrap_nil();
        slot.flags = 0;
        slot.index = benderc_allocfar(opts.compiler);
    }
    return slot;
}

/* Get a bunch of slots for function arguments */
BenderSlot *benderc_toslots(BenderCompiler *c, const Bender *vals, int32_t len) {
    int32_t i;
    BenderSlot *ret = NULL;
    BenderFopts subopts = benderc_fopts_default(c);
    subopts.flags |= BENDER_FOPTS_ACCEPT_SPLICE;
    for (i = 0; i < len; i++) {
        bender_v_push(ret, benderc_value(subopts, vals[i]));
    }
    return ret;
}

/* Get a bunch of slots for function arguments */
BenderSlot *benderc_toslotskv(BenderCompiler *c, Bender ds) {
    BenderSlot *ret = NULL;
    BenderFopts subopts = benderc_fopts_default(c);
    subopts.flags |= BENDER_FOPTS_ACCEPT_SPLICE;
    const BenderKV *kvs = NULL;
    int32_t cap = 0, len = 0;
    bender_dictionary_view(ds, &kvs, &len, &cap);
    /* Sort keys for stability of order? */
    int32_t *index_buf;
    int32_t index_buf_stack[32];
    int32_t *index_buf_heap = NULL;
    if (len < 32) {
        index_buf = index_buf_stack;
    } else {
        index_buf_heap = bender_smalloc(sizeof(int32_t) * len);
        index_buf = index_buf_heap;
    }
    if (len) bender_sorted_keys(kvs, cap, index_buf);
    for (int32_t i = 0; i < len; i++) {
        bender_v_push(ret, benderc_value(subopts, kvs[index_buf[i]].key));
        bender_v_push(ret, benderc_value(subopts, kvs[index_buf[i]].value));
    }
    if (index_buf_heap) bender_sfree(index_buf_heap);
    return ret;
}

/* Push slots loaded via benderc_toslots. Return the minimum number of slots pushed,
 * or -1 - min_arity if there is a splice. (if there is no splice, min_arity is also
 * the maximum possible arity). */
int32_t benderc_pushslots(BenderCompiler *c, BenderSlot *slots) {
    int32_t i;
    int32_t count = bender_v_count(slots);
    int32_t min_arity = 0;
    int has_splice = 0;
    for (i = 0; i < count;) {
        if (slots[i].flags & BENDER_SLOT_SPLICED) {
            benderc_emit_s(c, JOP_PUSH_ARRAY, slots[i], 0);
            i++;
            has_splice = 1;
        } else if (i + 1 == count) {
            benderc_emit_s(c, JOP_PUSH, slots[i], 0);
            i++;
            min_arity++;
        } else if (slots[i + 1].flags & BENDER_SLOT_SPLICED) {
            benderc_emit_s(c, JOP_PUSH, slots[i], 0);
            benderc_emit_s(c, JOP_PUSH_ARRAY, slots[i + 1], 0);
            i += 2;
            min_arity++;
            has_splice = 1;
        } else if (i + 2 == count) {
            benderc_emit_ss(c, JOP_PUSH_2, slots[i], slots[i + 1], 0);
            i += 2;
            min_arity += 2;
        } else if (slots[i + 2].flags & BENDER_SLOT_SPLICED) {
            benderc_emit_ss(c, JOP_PUSH_2, slots[i], slots[i + 1], 0);
            benderc_emit_s(c, JOP_PUSH_ARRAY, slots[i + 2], 0);
            i += 3;
            min_arity += 2;
            has_splice = 1;
        } else {
            benderc_emit_sss(c, JOP_PUSH_3, slots[i], slots[i + 1], slots[i + 2], 0);
            i += 3;
            min_arity += 3;
        }
    }
    return has_splice ? (-1 - min_arity) : min_arity;
}

/* Check if a list of slots has any spliced slots */
static int has_spliced(BenderSlot *slots) {
    int32_t i;
    for (i = 0; i < bender_v_count(slots); i++) {
        if (slots[i].flags & BENDER_SLOT_SPLICED)
            return 1;
    }
    return 0;
}

/* Free slots loaded via benderc_toslots */
void benderc_freeslots(BenderCompiler *c, BenderSlot *slots) {
    int32_t i;
    for (i = 0; i < bender_v_count(slots); i++) {
        benderc_freeslot(c, slots[i]);
    }
    bender_v_free(slots);
}

/* Compile some code that will be thrown away. Used to ensure
 * that dead code is well formed without including it in the final
 * bytecode. */
void benderc_throwaway(BenderFopts opts, Bender x) {
    BenderCompiler *c = opts.compiler;
    BenderScope unusedScope;
    int32_t bufstart = bender_v_count(c->buffer);
    int32_t mapbufstart = bender_v_count(c->mapbuffer);
    benderc_scope(&unusedScope, c, BENDER_SCOPE_UNUSED, "unused");
    benderc_value(opts, x);
    benderc_lintf(c, BENDER_C_LINT_STRICT, "dead code, consider removing %.4q", x);
    benderc_popscope(c);
    if (c->buffer) {
        bender_v__cnt(c->buffer) = bufstart;
        if (c->mapbuffer)
            bender_v__cnt(c->mapbuffer) = mapbufstart;
    }
}

static int function_can_inline(BenderFopts opts, BenderFunction *func, BenderSlot *slots) {
    BenderFuncDef *def = func->def;
    /* TODO - hoist this check to a flag on the func def */
    if (opts.compiler->optimize < 2) return 0;
    if (def->environments_length) return 0;
    if (def->defs_length) return 0;
    if (def->slotcount > 32) return 0;
    if (def->bytecode_length > 40) return 0;
    if (def->flags & (BENDER_FUNCDEF_FLAG_NAMEDARGS | BENDER_FUNCDEF_FLAG_STRUCTARG | BENDER_FUNCDEF_FLAG_VARARG)) return 0;
    if (bender_v_count(slots) != def->min_arity) return 0; /* TODO - optional arguments */
    if (bender_v_count(slots) != def->max_arity) return 0;
    /* Check for JOP_LOAD_SELF */
    for (int32_t i = 0; i < def->bytecode_length; i++) {
        if ((def->bytecode[i] & 0x7F) == JOP_LOAD_SELF) return 0;
    }
    return 1;
}

/* Fold (string "a" "b" ...) with all-literal string args into one
 * constant. Only unwraps BENDER_STRING constants (numbers, keywords
 * etc. format through `describe`/`to-string` paths whose exact output
 * is deliberately left to runtime). Uses a malloc'd scratch buffer so
 * no unrooted GC object is live across allocations; bender_stringv
 * copies into a fresh interned string. */
static int benderc_fold_string(BenderFopts opts, BenderSlot *slots, BenderSlot *out) {
    (void) opts;
    int32_t i, len = bender_v_count(slots);
    if (len < 1) return 0;
    int32_t total = 0;
    for (i = 0; i < len; i++) {
        if (!(slots[i].flags & BENDER_SLOT_CONSTANT)) return 0;
        if (!bender_checktype(slots[i].constant, BENDER_STRING)) return 0;
        int32_t slen = bender_string_length(bender_unwrap_string(slots[i].constant));
        if (total > INT32_MAX - slen) return 0;
        total += slen;
    }
    uint8_t *scratch = (uint8_t *) bender_smalloc((size_t) total ? (size_t) total : 1);
    int32_t off = 0;
    for (i = 0; i < len; i++) {
        BenderString s = bender_unwrap_string(slots[i].constant);
        int32_t slen = bender_string_length(s);
        if (slen) {
            memcpy(scratch + off, s, (size_t) slen);
            off += slen;
        }
    }
    *out = benderc_cslot(bender_stringv(scratch, total));
    bender_sfree(scratch);
    return 1;
}

/* Compile a call or tailcall instruction */
static BenderSlot benderc_call(BenderFopts opts, BenderSlot *slots, BenderSlot fun, const Bender *form) {
    BenderSlot retslot;
    BenderCompiler *c = opts.compiler;
    int specialized = 0;
    if (fun.flags & BENDER_SLOT_CONSTANT && !has_spliced(slots)) {
        if (bender_checktype(fun.constant, BENDER_FUNCTION)) {
            BenderFunction *f = bender_unwrap_function(fun.constant);
            const BenderFunOptimizer *o = benderc_funopt(f->def->flags);
            if (o && (!o->can_optimize || o->can_optimize(opts, slots))) {
                specialized = 1;
                retslot = o->optimize(opts, slots);
            } else if (function_can_inline(opts, f, slots)) {
                /* Function inlining */
                int is_tail;
                BenderSlot target;
                if ((opts.flags & BENDER_FOPTS_TAIL) &&
                        /* Prevent top level tail calls for better errors */
                        !(c->scope->flags & BENDER_SCOPE_TOP)) {
                    is_tail = 1;
                } else {
                    is_tail = 0;
                }
                /* contiguous chunk of fun->def->slotcount slots */
                if (!is_tail) target = benderc_gettarget(opts);
                int32_t nslots = f->def->slotcount;
                int32_t slot_delta = benderc_regalloc_n(&c->scope->ra, nslots);
                if (slot_delta + f->def->slotcount < 256) {
                    /*bender_eprintf("inlining %V\n", bender_wrap_function(f));*/
                    /* Limit inlining when too many slots */
                    BenderScope *scope = c->scope;
                    while (scope) {
                        if (scope->flags & BENDER_SCOPE_FUNCTION)
                            break;
                        scope = scope->parent;
                    }
                    /* Setup the function parameters */
                    BenderSlot dest;
                    dest.envindex = -1;
                    dest.flags = BENDER_SLOTTYPE_ANY;
                    dest.constant = bender_wrap_nil();
                    for (int32_t i = 0; i < bender_v_count(slots); i++) {
                        dest.index = slot_delta + i;
                        benderc_copy(c, dest, slots[i]);
                    }

                    /* Initialize other slots to nil just like in a normal function call */
                    /* TODO - add pass to remove these if needed so we can add this back.
                     * The current compiler doesn't rely on nil-initialization behavior but might in the future
                     * (e.g. removing redundant ldn at the beginning of a function.
                    for (int32_t i = bender_v_count(slots); i < nslots; i++) {
                        benderc_emit(c, JOP_LOAD_NIL | ((uint32_t)(i + slot_delta) << 8));
                    }
                    */

                    /* Do main inlining (patch the bytecode) */
                    int32_t const_delta = bender_v_count(scope->consts); /* Get const delta _after_ moving slots. benderc_copy can add constants here! */
                    uint32_t *insert_code = bender_bytecode_inline_chunk(f->def, is_tail ? -1 : target.index, slot_delta, const_delta);
                    for (int32_t i = 0; i < bender_v_count(insert_code); i++) {
                        /* TODO - better match up source code mapping - e.g. if inlined function
                         * is from the same source file */
                        benderc_emit(c, insert_code[i]);
                    }
                    /* Copy constants into our constants buffer */
                    for (int32_t i = 0; i < f->def->constants_length; i++) {
                        bender_v_push(scope->consts, f->def->constants[i]);
                    }
                    bender_v_free(insert_code);
                    specialized = 1;
                    if (is_tail) {
                        retslot = benderc_cslot(bender_wrap_nil());
                        retslot.flags = BENDER_SLOT_RETURNED;
                    } else {
                        retslot = target;
                    }
                } else {
                    /*bender_eprintf("inlining failed %V\n", bender_wrap_function(f));*/
                    ;
                }
                benderc_regalloc_free_n(&c->scope->ra, slot_delta, nslots);
            }
        } else if (bender_checktype(fun.constant, BENDER_CFUNCTION) &&
                   bender_unwrap_cfunction(fun.constant) == bender_core_string) {
            /* Fold (string "a" "b") with all-literal string args.
             * Pointer-compared against the core binding: user shadowing
             * resolves to a different value and never folds. */
            if (benderc_fold_string(opts, slots, &retslot)) {
                specialized = 1;
            }
        }
    }
    if (!specialized) {
        int32_t min_arity = benderc_pushslots(c, slots);
        /* Check for provably incorrect function calls */
        if (fun.flags & BENDER_SLOT_CONSTANT) {

            /* Check for bad arity type if fun is a constant */
            switch (bender_type(fun.constant)) {
                case BENDER_FUNCTION: {
                    BenderFunction *f = bender_unwrap_function(fun.constant);
                    int32_t min = f->def->min_arity;
                    int32_t max = f->def->max_arity;
                    int structarg = f->def->flags & BENDER_FUNCDEF_FLAG_STRUCTARG;
                    int namedarg = f->def->flags & BENDER_FUNCDEF_FLAG_NAMEDARGS;
                    if (min_arity < 0) {
                        /* Call has splices */
                        min_arity = -1 - min_arity;
                        if (min_arity > max && max >= 0) {
                            const uint8_t *es = bender_formatc(
                                                    "%v expects at most %d argument%s, got at least %d",
                                                    fun.constant, max, max == 1 ? "" : "s", min_arity);
                            benderc_error(c, es);
                        }
                    } else {
                        /* Call has no splices */
                        if (min_arity > max && max >= 0) {
                            const uint8_t *es = bender_formatc(
                                                    "%v expects at most %d argument%s, got %d",
                                                    fun.constant, max, max == 1 ? "" : "s", min_arity);
                            benderc_error(c, es);
                        }
                        if (min_arity < min) {
                            const uint8_t *es = bender_formatc(
                                                    "%v expects at least %d argument%s, got %d",
                                                    fun.constant, min, min == 1 ? "" : "s", min_arity);
                            benderc_error(c, es);
                        }
                        if (structarg && (min_arity > f->def->arity) && ((min_arity - f->def->arity) & 1)) {
                            /* If we have an odd number of variadic arguments to a `&keys` function, that is almost certainly wrong. */
                            if (namedarg) {
                                benderc_lintf(c, BENDER_C_LINT_NORMAL,
                                             "odd number of named arguments to `&named` function %v", fun.constant);
                            } else {
                                benderc_lintf(c, BENDER_C_LINT_NORMAL,
                                             "odd number of named arguments to `&keys` function %v", fun.constant);
                            }
                        }
                        if (namedarg && f->def->named_args_count > 0) {
                            /* For each argument passed in, check if it is one of the used named arguments
                             * by checking the list defined in the function def. If not, raise a normal compiler
                             * lint. We can also do a strict lint for _missing_ named arguments, although in many
                             * cases those are assumed to have some kind of default, or we have dynamic keys. */
                            int32_t first_arg_key_index = f->def->arity + 1;
                            for (int32_t i = first_arg_key_index; i < bender_tuple_length(form); i += 2) {
                                Bender argkey = form[i];
                                /* Assumption: The first N constants of a function are its named argument keys. This
                                 * may change if the compiler changes, but is true for all Bender generated functions. */
                                int found = 0;
                                if (bender_checktype(argkey, BENDER_KEYWORD)) {
                                    for (int32_t j = 0; j < f->def->named_args_count && j < f->def->constants_length; j++) {
                                        if (bender_equals(argkey, f->def->constants[j])) {
                                            found = 1;
                                            break;
                                        }
                                    }
                                } else if (bender_checktype(argkey, BENDER_TUPLE)) {
                                    /* Possible lint : too dynamic, be dumber
                                     * (defn f [&named x] [x])
                                     * (f (if (coin-flip) :x :w) 10)
                                     * A tuple could be a function call the evaluates to a valid key */
                                    found = 1;
                                }
                                if (!found) {
                                    benderc_lintf(c, BENDER_C_LINT_NORMAL,
                                                 "unused named argument %v to function %v", argkey, fun.constant);
                                }
                            }
                        }
                    }
                }
                break;
                case BENDER_CFUNCTION:
                case BENDER_ABSTRACT:
                case BENDER_NIL:
                    break;
                case BENDER_KEYWORD:
                    if (min_arity == 0) {
                        const uint8_t *es = bender_formatc("%v expects at least 1 argument, got 0",
                                                          fun.constant);
                        benderc_error(c, es);
                    }
                    break;
                default:
                    if (min_arity > 1 || min_arity == 0) {
                        const uint8_t *es = bender_formatc("%v expects 1 argument, got %d",
                                                          fun.constant, min_arity);
                        benderc_error(c, es);
                    }
                    if (min_arity < -2) {
                        const uint8_t *es = bender_formatc("%v expects 1 argument, got at least %d",
                                                          fun.constant, -1 - min_arity);
                        benderc_error(c, es);
                    }
                    break;
            }
        }

        if ((opts.flags & BENDER_FOPTS_TAIL) &&
                /* Prevent top level tail calls for better errors */
                !(c->scope->flags & BENDER_SCOPE_TOP)) {
            benderc_emit_s(c, JOP_TAILCALL, fun, 0);
            retslot = benderc_cslot(bender_wrap_nil());
            retslot.flags = BENDER_SLOT_RETURNED;
        } else {
            retslot = benderc_gettarget(opts);
            benderc_emit_ss(c, JOP_CALL, retslot, fun, 1);
        }
    }
    benderc_freeslots(c, slots);
    return retslot;
}

static BenderSlot benderc_maker(BenderFopts opts, BenderSlot *slots, int op) {
    BenderCompiler *c = opts.compiler;
    BenderSlot retslot;

    /* Check if this structure is composed entirely of constants */
    int can_inline = 1;
    for (int32_t i = 0; i < bender_v_count(slots); i++) {
        if (!(slots[i].flags & BENDER_SLOT_CONSTANT) ||
                (slots[i].flags & BENDER_SLOT_SPLICED)) {
            can_inline = 0;
            break;
        }
    }

    if (can_inline && (op == JOP_MAKE_STRUCT)) {
        BenderKV *st = bender_struct_begin(bender_v_count(slots) / 2);
        for (int32_t i = 0; i < bender_v_count(slots); i += 2) {
            Bender k = slots[i].constant;
            Bender v = slots[i + 1].constant;
            bender_struct_put(st, k, v);
        }
        retslot = benderc_cslot(bender_wrap_struct(bender_struct_end(st)));
        benderc_freeslots(c, slots);
    } else if (can_inline && (op == JOP_MAKE_TUPLE)) {
        Bender *tup = bender_tuple_begin(bender_v_count(slots));
        for (int32_t i = 0; i < bender_v_count(slots); i++) {
            tup[i] = slots[i].constant;
        }
        retslot = benderc_cslot(bender_wrap_tuple(bender_tuple_end(tup)));
        benderc_freeslots(c, slots);
    } else {
        benderc_pushslots(c, slots);
        benderc_freeslots(c, slots);
        retslot = benderc_gettarget(opts);
        benderc_emit_s(c, op, retslot, 1);
    }

    return retslot;
}

static BenderSlot benderc_array(BenderFopts opts, Bender x) {
    BenderCompiler *c = opts.compiler;
    BenderArray *a = bender_unwrap_array(x);
    return benderc_maker(opts,
                        benderc_toslots(c, a->data, a->count),
                        JOP_MAKE_ARRAY);
}

static BenderSlot benderc_tuple(BenderFopts opts, Bender x) {
    BenderCompiler *c = opts.compiler;
    const Bender *t = bender_unwrap_tuple(x);
    return benderc_maker(opts,
                        benderc_toslots(c, t, bender_tuple_length(t)),
                        JOP_MAKE_TUPLE);
}

/* Is this function one of the FFI aliases generated by
 * ffi/defbind-alias? The marker is set when the alias is compiled, so
 * the check is exact and cannot be fooled by a redefinition of the
 * binding: a redefined function is a different value with a different
 * FuncDef. */
static int benderc_is_alias_function(Bender x) {
    if (!bender_checktype(x, BENDER_FUNCTION)) return 0;
    return (bender_unwrap_function(x)->def->flags & BENDER_FUNCDEF_FLAG_FFI_ALIAS_DECL) != 0;
}

/* Can evaluating x re-enter the same alias call while a scratch tuple
 * is being populated? Only symbols and literal tuple constructors made
 * of scratch-safe forms are inert; anything else is a call or a
 * constructor that may evaluate a call. */
static int benderc_scratch_safe(Bender x) {
    switch (bender_type(x)) {
        case BENDER_SYMBOL:
            return 1;
        case BENDER_TUPLE: {
            const Bender *t = bender_unwrap_tuple(x);
            int32_t len = bender_tuple_length(t);
            if (!(bender_tuple_flag(t) & BENDER_TUPLE_FLAG_BRACKETCTOR))
                return 0;
            for (int32_t i = 0; i < len; i++)
                if (!benderc_scratch_safe(t[i]))
                    return 0;
            return 1;
        }
        case BENDER_ARRAY:
        case BENDER_TABLE:
        case BENDER_STRUCT:
            return 0;
        default:
            return 1;
    }
}

/* Compile a tuple literal into a mutable scratch array reused by this
 * call site's stack frame. The array is only handed to an FFI alias,
 * which reads it synchronously; it is never exposed to Bender code. */
static BenderSlot benderc_tuple_scratch(BenderFopts opts, Bender x) {
    BenderCompiler *c = opts.compiler;
    const Bender *t = bender_unwrap_tuple(x);
    int32_t len = bender_tuple_length(t);
    /* Reserve a slot above every register used so far in this scope.
     * No already-emitted instruction can write it, and the allocator
     * will not hand it out again, so the scratch array survives every
     * execution of this call site in the current frame -- including
     * loop iterations, where a recycled temporary slot would be
     * clobbered by earlier instructions in the loop body. */
    int32_t scratch = c->scope->ra.max + 1;
    if (scratch > 0xFFFF) {
        return benderc_maker(opts, benderc_toslots(c, t, len), JOP_MAKE_TUPLE);
    }
    benderc_regalloc_touch(&c->scope->ra, scratch);
    if (scratch > c->scope->ra.max)
        c->scope->ra.max = scratch;
    BenderSlot *slots = benderc_toslots(c, t, len);
    int spliced = 0;
    for (int32_t i = 0; i < len; i++) {
        if (slots[i].flags & BENDER_SLOT_SPLICED) {
            spliced = 1;
            break;
        }
    }
    if (spliced) {
        /* Rare shape keeps the normal tuple path. The reserved scratch
         * slot is simply unused. */
        return benderc_maker(opts, slots, JOP_MAKE_TUPLE);
    }
    benderc_pushslots(c, slots);
    benderc_freeslots(c, slots);
    BenderSlot ret = benderc_gettarget(opts);
    BenderSlot scratch_slot;
    scratch_slot.constant = bender_wrap_nil();
    scratch_slot.envindex = -1;
    scratch_slot.flags = 0;
    scratch_slot.index = scratch;
    /* The two-slot emitter keeps the near/far destination rules in one
     * place: a destination above 255 goes through a near temporary and
     * is moved back, while the scratch operand uses the full 16-bit E
     * field. */
    benderc_emit_ss(c, JOP_MAKE_SCRATCH_TUPLE, ret, scratch_slot, 1);
    return ret;
}

/* Compile call arguments, replacing tuple literals passed directly to
 * an FFI alias with per-frame scratch arrays when it is safe. */
static BenderSlot *benderc_toslots_alias(
    BenderCompiler *c,
    const Bender *vals,
    int32_t len,
    int alias) {
    BenderSlot *ret = NULL;
    for (int32_t i = 0; i < len; i++) {
        Bender v = vals[i];
        BenderFopts subopts = benderc_fopts_default(c);
        subopts.flags |= BENDER_FOPTS_ACCEPT_SPLICE;
        if (alias &&
                bender_checktype(v, BENDER_TUPLE) &&
                (bender_tuple_flag(bender_unwrap_tuple(v)) & BENDER_TUPLE_FLAG_BRACKETCTOR)) {
            int safe = 1;
            for (int32_t j = i + 1; j < len; j++) {
                if (!benderc_scratch_safe(vals[j])) {
                    safe = 0;
                    break;
                }
            }
            if (safe) {
                bender_v_push(ret, benderc_tuple_scratch(subopts, v));
                continue;
            }
        }
        bender_v_push(ret, benderc_value(subopts, v));
    }
    return ret;
}

static BenderSlot benderc_tablector(BenderFopts opts, Bender x, int op) {
    BenderCompiler *c = opts.compiler;
    return benderc_maker(opts,
                        benderc_toslotskv(c, x),
                        op);
}

static BenderSlot benderc_bufferctor(BenderFopts opts, Bender x) {
    BenderCompiler *c = opts.compiler;
    BenderBuffer *b = bender_unwrap_buffer(x);
    Bender onearg = bender_stringv(b->data, b->count);
    return benderc_maker(opts,
                        benderc_toslots(c, &onearg, 1),
                        JOP_MAKE_BUFFER);
}

/* Expand a macro one time. Also get the special form compiler if we
 * find that instead. */
static int macroexpand1(
    BenderCompiler *c,
    Bender x,
    Bender *out,
    const BenderSpecial **spec) {
    if (!bender_checktype(x, BENDER_TUPLE))
        return 0;
    const Bender *form = bender_unwrap_tuple(x);
    if (bender_tuple_length(form) == 0)
        return 0;
    /* Source map - only set when we get a tuple */
    if (bender_tuple_sm_line(form) >= 0) {
        c->current_mapping.line = bender_tuple_sm_line(form);
        c->current_mapping.column = bender_tuple_sm_column(form);
    }
    /* Bracketed tuples are not specials or macros! */
    if (bender_tuple_flag(form) & BENDER_TUPLE_FLAG_BRACKETCTOR)
        return 0;
    if (!bender_checktype(form[0], BENDER_SYMBOL))
        return 0;
    const uint8_t *name = bender_unwrap_symbol(form[0]);
    const BenderSpecial *s = benderc_special(name);
    if (s) {
        *spec = s;
        return 0;
    }
    Bender macroval;
    BenderBindingType btype = bender_resolve(c->env, name, &macroval);
    if (!(btype == BENDER_BINDING_MACRO || btype == BENDER_BINDING_DYNAMIC_MACRO) ||
            !bender_checktype(macroval, BENDER_FUNCTION))
        return 0;

    /* Evaluate macro */
    BenderFunction *macro = bender_unwrap_function(macroval);
    int32_t arity = bender_tuple_length(form) - 1;
    BenderFiber *fiberp = bender_fiber(macro, 64, arity, form + 1);
    if (NULL == fiberp) {
        int32_t minar = macro->def->min_arity;
        int32_t maxar = macro->def->max_arity;
        const uint8_t *es = NULL;
        if (minar >= 0 && arity < minar)
            es = bender_formatc("macro arity mismatch, expected at least %d, got %d", minar, arity);
        if (maxar >= 0 && arity > maxar)
            es = bender_formatc("macro arity mismatch, expected at most %d, got %d", maxar, arity);
        c->result.macrofiber = NULL;
        benderc_error(c, es);
        return 0;
    }
    /* Set env */
    fiberp->env = c->env;
    int lock = bender_gclock();
    Bender mf_kw = bender_ckeywordv("macro-form");
    bender_table_put(c->env, mf_kw, x);
    Bender ml_kw = bender_ckeywordv("macro-lints");
    if (c->lints) {
        bender_table_put(c->env, ml_kw, bender_wrap_array(c->lints));
    }
    Bender tempOut;
    /* Root the macro fiber explicitly: bender_continue only roots it when a
     * root fiber is already running, but host-driven compiles (no fiber)
     * would otherwise leave it exposed to collections mid-expansion. */
    bender_gcroot(bender_wrap_fiber(fiberp));
    BenderSignal status;
    /* M4b-2: quantum-INTERRUPT is transparent here (compile must
     * complete) — resume again. Quiesce still unwinds. */
    do {
        status = bender_continue(fiberp, bender_wrap_nil(), &tempOut);
    } while (status == BENDER_SIGNAL_INTERRUPT && bender_vm.quantum &&
            !bender_atomic_load_relaxed(&bender_vm.auto_suspend));
    bender_gcunroot(bender_wrap_fiber(fiberp));
    bender_table_put(c->env, mf_kw, bender_wrap_nil());
    bender_table_put(c->env, ml_kw, bender_wrap_nil());
    bender_gcunlock(lock);
    if (status != BENDER_SIGNAL_OK) {
        const uint8_t *es = bender_formatc("(macro) %V", tempOut);
        c->result.macrofiber = fiberp;
        benderc_error(c, es);
        return 0;
    } else {
        /* The expansion is consumed element-by-element by the emitter
         * across further nested allocations (and collections). Pin it for
         * the rest of the compilation; nothing else references it yet. */
        benderc_pin(c, tempOut);
        *out = tempOut;
    }

    return 1;
}

/* Compile a single value */
BenderSlot benderc_value(BenderFopts opts, Bender x) {
    BenderSlot ret;
    BenderCompiler *c = opts.compiler;
    BenderSourceMapping last_mapping = c->current_mapping;
    c->recursion_guard--;

    /* Guard against previous errors and unbounded recursion */
    if (c->result.status == BENDER_COMPILE_ERROR) return benderc_cslot(bender_wrap_nil());
    if (c->recursion_guard <= 0) {
        benderc_cerror(c, "recursed too deeply");
        return benderc_cslot(bender_wrap_nil());
    }

    /* Macro expand. Also gets possible special form and
     * refines source mapping cursor if possible. */
    const BenderSpecial *spec = NULL;
    int macroi = BENDER_MAX_MACRO_EXPAND;
    while (macroi &&
            c->result.status != BENDER_COMPILE_ERROR &&
            macroexpand1(c, x, &x, &spec))
        macroi--;
    if (macroi == 0) {
        benderc_cerror(c, "recursed too deeply in macro expansion");
        return benderc_cslot(bender_wrap_nil());
    }

    /* Special forms */
    if (spec) {
        const Bender *tup = bender_unwrap_tuple(x);
        ret = spec->compile(opts, bender_tuple_length(tup) - 1, tup + 1);
    } else {
        switch (bender_type(x)) {
            case BENDER_TUPLE: {
                BenderFopts subopts = benderc_fopts_default(c);
                const Bender *tup = bender_unwrap_tuple(x);
                /* Empty tuple is tuple literal */
                if (bender_tuple_length(tup) == 0) {
                    ret = benderc_cslot(bender_wrap_tuple(bender_tuple_n(NULL, 0)));
                } else if (bender_tuple_flag(tup) & BENDER_TUPLE_FLAG_BRACKETCTOR) { /* [] tuples are not function call */
                    ret = benderc_tuple(opts, x);
                } else {
                    /* Function calls. Alias calls can reuse scratch
                     * arrays for tuple literals instead of allocating
                     * one tuple per call. */
                    BenderSlot head = benderc_value(subopts, tup[0]);
                    int alias = (head.flags & BENDER_SLOT_CONSTANT) &&
                                benderc_is_alias_function(head.constant);
                    ret = benderc_call(opts,
                                      benderc_toslots_alias(c, tup + 1, bender_tuple_length(tup) - 1, alias),
                                      head,
                                      tup);
                    benderc_freeslot(c, head);
                }
                ret.flags &= ~BENDER_SLOT_SPLICED;
            }
            break;
            /* Data Constructors */
            case BENDER_SYMBOL:
                ret = benderc_resolve(c, bender_unwrap_symbol(x));
                break;
            case BENDER_ARRAY:
                ret = benderc_array(opts, x);
                break;
            case BENDER_STRUCT:
                ret = benderc_tablector(opts, x, JOP_MAKE_STRUCT);
                break;
            case BENDER_TABLE:
                ret = benderc_tablector(opts, x, JOP_MAKE_TABLE);
                break;
            case BENDER_BUFFER:
                ret = benderc_bufferctor(opts, x);
                break;
            default:
                ret = benderc_cslot(x);
                break;
        }
    }

    if (c->result.status == BENDER_COMPILE_ERROR)
        return benderc_cslot(bender_wrap_nil());
    if (opts.flags & BENDER_FOPTS_TAIL)
        ret = benderc_return(c, ret);
    if (opts.flags & BENDER_FOPTS_HINT) {
        benderc_copy(c, opts.hint, ret);
        ret = opts.hint;
    }
    c->current_mapping = last_mapping;
    c->recursion_guard++;
    return ret;
}

/* Add function flags to bender functions */
void bender_def_addflags(BenderFuncDef *def) {
    int32_t set_flags = 0;
    int32_t unset_flags = 0;
    /* pos checks */
    if (def->name)              set_flags |= BENDER_FUNCDEF_FLAG_HASNAME;
    if (def->source)            set_flags |= BENDER_FUNCDEF_FLAG_HASSOURCE;
    if (def->defs)              set_flags |= BENDER_FUNCDEF_FLAG_HASDEFS;
    if (def->environments)      set_flags |= BENDER_FUNCDEF_FLAG_HASENVS;
    if (def->sourcemap)         set_flags |= BENDER_FUNCDEF_FLAG_HASSOURCEMAP;
    if (def->closure_bitset)    set_flags |= BENDER_FUNCDEF_FLAG_HASCLOBITSET;
    if (def->named_args_count)  set_flags |= BENDER_FUNCDEF_FLAG_NAMEDARGS;
    /* negative checks */
    if (!def->name)             unset_flags |= BENDER_FUNCDEF_FLAG_HASNAME;
    if (!def->source)           unset_flags |= BENDER_FUNCDEF_FLAG_HASSOURCE;
    if (!def->defs)             unset_flags |= BENDER_FUNCDEF_FLAG_HASDEFS;
    if (!def->environments)     unset_flags |= BENDER_FUNCDEF_FLAG_HASENVS;
    if (!def->sourcemap)        unset_flags |= BENDER_FUNCDEF_FLAG_HASSOURCEMAP;
    if (!def->closure_bitset)   unset_flags |= BENDER_FUNCDEF_FLAG_HASCLOBITSET;
    if (!def->named_args_count) unset_flags |= BENDER_FUNCDEF_FLAG_NAMEDARGS;
    /* Update flags */
    def->flags |= set_flags;
    def->flags &= ~unset_flags;
}

/* Compile a funcdef */
/* Once the various other settings of the FuncDef have been tweaked,
 * call bender_def_addflags to set the proper flags for the funcdef */
BenderFuncDef *benderc_pop_funcdef(BenderCompiler *c, BenderString name) {
    BenderScope *scope = c->scope;
    BenderFuncDef *def = bender_funcdef_alloc();
    for (int32_t i = 0; i < bender_v_count(c->retained_defs); i++) {
        if (c->retained_defs[i].scope == scope) {
            int found = 0;
            for (int32_t j = 0; j < bender_v_count(scope->defs); j++) {
                if (scope->defs[j] == c->retained_defs[i].child) {
                    c->retained_defs[i].parent = def;
                    found = 1;
                    break;
                }
            }
            /* Clear even on failure: this address is transient stack storage. */
            c->retained_defs[i].scope = NULL;
            if (!found)
                bender_eprintf("analysis retention refused: hinted child missing from finalized scope %s\n",
                              scope->name);
        }
    }
    def->slotcount = scope->ra.max + 1;

    bender_assert(scope->flags & BENDER_SCOPE_FUNCTION, "expected function scope");

    /* Copy envs */
    def->environments_length = bender_v_count(scope->envs);
    def->environments = bender_malloc(sizeof(int32_t) * def->environments_length);
    for (int32_t i = 0; i < def->environments_length; i++) {
        def->environments[i] = scope->envs[i].envindex;
    }

    def->constants_length = bender_v_count(scope->consts);
    def->constants = bender_v_flatten(scope->consts);

    def->defs_length = bender_v_count(scope->defs);
    def->defs = bender_v_flatten(scope->defs);

    /* Copy bytecode (only last chunk) */
    def->bytecode_length = bender_v_count(c->buffer) - scope->bytecode_start;
    if (def->bytecode_length) {
        size_t s = sizeof(int32_t) * (size_t) def->bytecode_length;
        def->bytecode = bender_malloc(s);
        if (NULL == def->bytecode) {
            BENDER_OUT_OF_MEMORY;
        }
        safe_memcpy(def->bytecode, c->buffer + scope->bytecode_start, s);
        bender_v__cnt(c->buffer) = scope->bytecode_start;
        if (NULL != c->mapbuffer && c->source) {
            size_t s = sizeof(BenderSourceMapping) * (size_t) def->bytecode_length;
            def->sourcemap = bender_malloc(s);
            if (NULL == def->sourcemap) {
                BENDER_OUT_OF_MEMORY;
            }
            safe_memcpy(def->sourcemap, c->mapbuffer + scope->bytecode_start, s);
            bender_v__cnt(c->mapbuffer) = scope->bytecode_start;
        }
    }

    /* Get source from parser */
    def->source = c->source;

    def->arity = 0;
    def->min_arity = 0;
    def->flags = 0;
    if (scope->flags & BENDER_SCOPE_ENV) {
        def->flags |= BENDER_FUNCDEF_FLAG_NEEDSENV;
    }

    /* Copy upvalue bitset */
    if (scope->ua.count) {
        /* Number of u32s we need to create a bitmask for all slots */
        int32_t slotchunks = ((def->slotcount - 1) >> 5) + 1;
        /* numchunks is min of slotchunks and scope->ua.count */
        int32_t numchunks = slotchunks > scope->ua.count ? scope->ua.count : slotchunks;
        uint32_t *chunks = bender_calloc(slotchunks, sizeof(uint32_t));
        if (NULL == chunks) {
            BENDER_OUT_OF_MEMORY;
        }
        memcpy(chunks, scope->ua.chunks, sizeof(uint32_t) * numchunks);
        /* fprintf(stderr, "slot chunks: %d, scope->ua.count: %d, numchunks: %d\n", slotchunks, scope->ua.count, numchunks); */
        /* Register allocator preallocates some registers [240-255, high 16 bits of chunk index 7], we can ignore those. */
        if (scope->ua.count > 7 && slotchunks > 7) chunks[7] &= 0xFFFFU;
        def->closure_bitset = chunks;
    }

    /* Capture symbol to local mapping */
    BenderSymbolMap *locals = NULL;

    /* Symbol -> upvalue mapping */
    BenderScope *top = c->scope;
    while (top->parent) top = top->parent;
    for (BenderScope *s = top; s != NULL; s = s->child) {
        for (int32_t j = 0; j < bender_v_count(scope->envs); j++) {
            BenderEnvRef ref = scope->envs[j];
            BenderScope *upscope = ref.scope;
            if (upscope != s) continue;
            for (int32_t i = 0; i < bender_v_count(upscope->syms); i++) {
                SymPair pair = upscope->syms[i];
                if (pair.sym2) {
                    BenderSymbolMap jsm;
                    jsm.birth_pc = UINT32_MAX;
                    jsm.death_pc = j; /* Use death_pc to encode environment index */
                    jsm.slot_index = pair.slot.index;
                    jsm.symbol = pair.sym2;
                    bender_v_push(locals, jsm);
                }
            }
        }
    }

    /* Symbol -> slot mapping */
    for (int32_t i = 0; i < bender_v_count(scope->syms); i++) {
        SymPair pair = scope->syms[i];
        if (pair.sym2) {
            BenderSymbolMap jsm;
            /* Check for unused symbols */
            if (pair.referenced == 0 && pair.sym) {
                benderc_lintf(c, BENDER_C_LINT_STRICT, "binding %q is unused", bender_wrap_symbol(pair.sym));
            }
            if (pair.death_pc == UINT32_MAX) {
                jsm.death_pc = def->bytecode_length;
            } else {
                jsm.death_pc = pair.death_pc - scope->bytecode_start;
            }
            /* Handle birth_pc == 0 correctly */
            if ((uint32_t) scope->bytecode_start > pair.birth_pc) {
                jsm.birth_pc = 0;
            } else {
                jsm.birth_pc = pair.birth_pc - scope->bytecode_start;
            }
            bender_assert(jsm.birth_pc <= jsm.death_pc, "birth pc after death pc");
            bender_assert(jsm.birth_pc < (uint32_t) def->bytecode_length, "bad birth pc");
            bender_assert(jsm.death_pc <= (uint32_t) def->bytecode_length, "bad death pc");
            jsm.slot_index = pair.slot.index;
            jsm.symbol = pair.sym2;
            bender_v_push(locals, jsm);
        }
    }
    def->symbolmap_length = bender_v_count(locals);
    def->symbolmap = bender_v_flatten(locals);
    if (def->symbolmap_length) def->flags |= BENDER_FUNCDEF_FLAG_HASSYMBOLMAP;

    /* Pop the scope */
    benderc_popscope(c);

    /* Add a name _before_ optimization for debugging */
    def->name = name;

#ifdef BENDER_DEBUG
    bender_verify(def);
#endif

    return def;
}

static int32_t to_optimization_level(Bender x) {
    if (!bender_checkint(x)) return 1;
    int32_t int_level = bender_unwrap_integer(x);
    return int_level;
}

/* Initialize a compiler */
static void benderc_init(BenderCompiler *c, BenderTable *env, const uint8_t *where, BenderArray *lints) {
    c->scope = NULL;
    c->buffer = NULL;
    c->mapbuffer = NULL;
    c->recursion_guard = BENDER_RECURSION_GUARD;
    c->env = env;
    c->source = where;
    c->current_mapping.line = -1;
    c->current_mapping.column = -1;
    c->lints = lints;
    c->is_redef = bender_truthy(bender_table_get_keyword(c->env, "redef"));
    c->optimize = to_optimization_level(bender_table_get_keyword(c->env, "optimize"));
    Bender retain = bender_table_get_keyword(c->env, "retain-hinted-exports-for-analysis");
    c->retain_hinted_exports_for_analysis =
        bender_checktype(retain, BENDER_BOOLEAN) && bender_truthy(retain);
    c->retained_defs = NULL;
    /* Rooted pin array for compiler-held values (see benderc_pin). The
     * gcroot itself never triggers a collection, so creating and pinning
     * the empty array is atomic. */
    c->pins = bender_array(0);
    bender_gcroot(bender_wrap_array(c->pins));
    /* Publish on the VM compiler stack for GC marking. */
    c->compiler_next = bender_vm.compiler_stack;
    bender_vm.compiler_stack = c;
    /* Init result */
    c->result.error = NULL;
    c->result.status = BENDER_COMPILE_OK;
    c->result.funcdef = NULL;
    c->result.macrofiber = NULL;
    c->result.error_mapping.line = -1;
    c->result.error_mapping.column = -1;
}

/* Deinitialize a compiler struct */
static void benderc_deinit(BenderCompiler *c) {
    bender_v_free(c->buffer);
    bender_v_free(c->mapbuffer);
    bender_v_free(c->retained_defs);
    bender_gcunroot(bender_wrap_array(c->pins));
    bender_vm.compiler_stack = c->compiler_next;
    c->env = NULL;
}

void benderc_optimize_funcdef(BenderCompiler *c, BenderFuncDef *def) {
    if (c->retain_hinted_exports_for_analysis)
        bender_bytecode_optimize_for_analysis(def, c->optimize,
                                              c->retained_defs, bender_v_count(c->retained_defs));
    else
        bender_bytecode_optimize(def, c->optimize);
}

/* Publish only children demonstrably present in the final optimized tree.
 * This analysis-only sidecar identifies their exact disasm paths, never
 * their names; it is not part of a FuncDef or a marshaled image. */
static void benderc_record_retained_paths(BenderCompiler *c, BenderFuncDef *def,
                                         const uint8_t *path, BenderTable *paths,
                                         BenderTable *profile_paths) {
    for (int32_t j = 0; j < def->defs_length; j++) {
        BenderFuncDef *child = def->defs[j];
        BenderString childpath = bender_formatc("%s/%d", path, j);
        benderc_pin(c, bender_wrap_string(childpath));
        for (int32_t i = 0; i < bender_v_count(c->retained_defs); i++) {
            BenderRetainedDef *r = c->retained_defs + i;
            if (r->parent == def && r->child == child) {
                if (bender_truthy(r->hint))
                    bender_table_put(paths, bender_wrap_string(childpath), r->hint);
                if (r->profiled)
                    bender_table_put(profile_paths, bender_wrap_string(childpath), bender_wrap_true());
                break;
            }
        }
        benderc_record_retained_paths(c, child, childpath, paths, profile_paths);
    }
}

/* Pin a value for the duration of the compilation. The push itself can
 * trigger a collection (array growth), so root the value first (atomic),
 * then push, then drop the temporary root -- the pins array keeps it. */
void benderc_pin(BenderCompiler *c, Bender x) {
    bender_gcroot(x);
    bender_array_push(c->pins, x);
    bender_gcunroot(x);
}

/* Compile a form. */
BenderCompileResult bender_compile_lint(Bender source,
                                      BenderTable *env, const uint8_t *where, BenderArray *lints) {
    BenderCompiler c;
    BenderScope rootscope;
    BenderFopts fopts;

    benderc_init(&c, env, where, lints);

    /* Push a function scope */
    benderc_scope(&rootscope, &c, BENDER_SCOPE_FUNCTION | BENDER_SCOPE_TOP, "root");

    /* Set initial form options */
    fopts.compiler = &c;
    fopts.flags = BENDER_FOPTS_TAIL | BENDER_SLOTTYPE_ANY;
    fopts.hint = benderc_cslot(bender_wrap_nil());

    /* Compile the value */
    benderc_value(fopts, source);

    if (c.result.status == BENDER_COMPILE_OK) {
        BenderFuncDef *def = benderc_pop_funcdef(&c, bender_cstring("thunk"));
        bender_def_addflags(def);
        benderc_optimize_funcdef(&c, def);
        c.result.funcdef = def;
        if (c.retain_hinted_exports_for_analysis) {
            BenderTable *paths = bender_table(4);
            benderc_pin(&c, bender_wrap_table(paths));
            BenderTable *profile_paths = bender_table(4);
            benderc_pin(&c, bender_wrap_table(profile_paths));
            benderc_record_retained_paths(&c, def, (const uint8_t *) "", paths, profile_paths);
            bender_table_put(env, bender_ckeywordv("analysis-retained-hint-paths"),
                            bender_wrap_table(paths));
            bender_table_put(env, bender_ckeywordv("analysis-retained-profile-paths"),
                            bender_wrap_table(profile_paths));
        }
    } else {
        c.result.error_mapping = c.current_mapping;
        benderc_popscope(&c);
    }

    benderc_deinit(&c);

    return c.result;
}

BenderCompileResult bender_compile(Bender source, BenderTable *env, const uint8_t *where) {
    return bender_compile_lint(source, env, where, NULL);
}

/* C Function for compiling */
BENDER_CORE_FN(cfun_compile, "(compile ast &opt env source lints)",
              "Compiles an abstract syntax tree `ast` into a function. "
              "Returns a new function and does not modify `ast`. If "
              "compilation fails, returns an error struct with keys "
              "`:line`, `:column`, and `:error`. If a table `env` is "
              "given, specifies the environment of the fiber used during "
              "compilation. Optional argument `source` is a string or "
              "keyword that is a source path for better errors. If a "
              "`lints` array is given, linting messages will be appended to "
              "the array. Each message will be a tuple of the form `[level "
              "line col message]`.") {
    bender_sandbox_assert(BENDER_SANDBOX_COMPILE);
    bender_arity(argc, 1, 4);
    /* TEMPORARY 2b-iii: trace compiles under stress. */
    if (getenv("BENDER_COMPILE_TRACE")) {
        BenderTable *e = (argc > 1 && !bender_checktype(argv[1], BENDER_NIL))
                        ? bender_gettable(argv, 1) : bender_vm.fiber->env;
        const char *head = "?";
        if (bender_checktype(argv[0], BENDER_TUPLE)) {
            BenderTuple t = bender_unwrap_tuple(argv[0]);
            if (bender_tuple_length(t) > 0 && bender_checktype(t[0], BENDER_SYMBOL)) {
                head = (const char *) bender_unwrap_symbol(t[0]);
            } else {
                head = "(non-symbol-head)";
            }
        } else if (bender_checktype(argv[0], BENDER_SYMBOL)) {
            head = (const char *) bender_unwrap_symbol(argv[0]);
        }
        fprintf(stderr, "[compile env=%p head=%s]\n", (void *) e, head);
    }
    BenderTable *env = (argc > 1 && !bender_checktype(argv[1], BENDER_NIL))
                      ? bender_gettable(argv, 1) : bender_vm.fiber->env;
    if (NULL == env) {
        env = bender_table(0);
        bender_vm.fiber->env = env;
    }
    const uint8_t *source = NULL;
    if (argc >= 3) {
        Bender x = argv[2];
        if (bender_checktype(x, BENDER_STRING)) {
            source = bender_unwrap_string(x);
        } else if (bender_checktype(x, BENDER_KEYWORD)) {
            source = bender_unwrap_keyword(x);
        } else if (!bender_checktype(x, BENDER_NIL)) {
            bender_panic_type(x, 2, BENDER_TFLAG_STRING | BENDER_TFLAG_KEYWORD);
        }
    }
    BenderArray *lints = (argc >= 4 && !bender_checktype(argv[3], BENDER_NIL))
                        ? bender_getarray(argv, 3) : NULL;
    BenderCompileResult res = bender_compile_lint(argv[0], env, source, lints);
    if (res.status == BENDER_COMPILE_OK) {
        return bender_wrap_function(bender_thunk(res.funcdef));
    } else {
        BenderTable *t = bender_table(4);
        bender_table_put(t, bender_ckeywordv("error"), bender_wrap_string(res.error));
        if (res.error_mapping.line > 0) {
            bender_table_put(t, bender_ckeywordv("line"), bender_wrap_integer(res.error_mapping.line));
        }
        if (res.error_mapping.column > 0) {
            bender_table_put(t, bender_ckeywordv("column"), bender_wrap_integer(res.error_mapping.column));
        }
        if (res.macrofiber) {
            bender_table_put(t, bender_ckeywordv("fiber"), bender_wrap_fiber(res.macrofiber));
        }
        return bender_wrap_table(t);
    }
}

void bender_lib_compile(BenderTable *env) {
    BenderRegExt cfuns[] = {
        BENDER_CORE_REG("compile", cfun_compile),
        BENDER_REG_END
    };
    bender_core_cfuns_ext(env, NULL, cfuns);
}
