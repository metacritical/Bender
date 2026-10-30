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
#include "compile.h"
#endif

#include <setjmp.h>

/* Conditionally compile this file */
#ifdef BENDER_ASSEMBLER

/* Definition for an instruction in the assembler */
typedef struct BenderInstructionDef BenderInstructionDef;
struct BenderInstructionDef {
    const char *name;
    enum BenderOpCode opcode;
};

/* Hold all state needed during assembly */
typedef struct BenderAssembler BenderAssembler;
struct BenderAssembler {
    BenderAssembler *parent;
    BenderFuncDef *def;
    jmp_buf on_error;
    const uint8_t *errmessage;
    int32_t errindex;

    int32_t environments_capacity;
    int32_t defs_capacity;
    int32_t bytecode_count; /* Used for calculating labels */

    Bender name;
    BenderTable labels; /* keyword -> bytecode index */
    BenderTable slots; /* symbol -> slot index */
    BenderTable envs; /* symbol -> environment index */
    BenderTable defs; /* symbol -> funcdefs index */
};

/* Bender opcode descriptions in lexicographic order. This
 * allows a binary search over the elements to find the
 * correct opcode given a name. This works in reasonable
 * time and is easier to setup statically than a hash table or
 * prefix tree. */
static const BenderInstructionDef bender_ops[] = {
    {"add", JOP_ADD},
    {"addim", JOP_ADD_IMMEDIATE},
    {"band", JOP_BAND},
    {"bandim", JOP_BAND_IMMEDIATE},
    {"bnot", JOP_BNOT},
    {"bor", JOP_BOR},
    {"borim", JOP_BOR_IMMEDIATE},
    {"bxor", JOP_BXOR},
    {"bxorim", JOP_BXOR_IMMEDIATE},
    {"call", JOP_CALL},
    {"clo", JOP_CLOSURE},
    {"cmp", JOP_COMPARE},
    {"cncl", JOP_CANCEL},
    {"div", JOP_DIVIDE},
    {"divf", JOP_DIVIDE_FLOOR},
    {"divim", JOP_DIVIDE_IMMEDIATE},
    {"eq", JOP_EQUALS},
    {"eqim", JOP_EQUALS_IMMEDIATE},
    {"err", JOP_ERROR},
    {"get", JOP_GET},
    {"geti", JOP_GET_INDEX},
    {"gt", JOP_GREATER_THAN},
    {"gte", JOP_GREATER_THAN_EQUAL},
    {"gtim", JOP_GREATER_THAN_IMMEDIATE},
    {"in", JOP_IN},
    {"inim", JOP_IN_IMMEDIATE},
    {"jmp", JOP_JUMP},
    {"jmpif", JOP_JUMP_IF},
    {"jmpni", JOP_JUMP_IF_NIL},
    {"jmpnn", JOP_JUMP_IF_NOT_NIL},
    {"jmpno", JOP_JUMP_IF_NOT},
    {"ldc", JOP_LOAD_CONSTANT},
    {"ldf", JOP_LOAD_FALSE},
    {"ldi", JOP_LOAD_INTEGER},
    {"ldn", JOP_LOAD_NIL},
    {"lds", JOP_LOAD_SELF},
    {"ldt", JOP_LOAD_TRUE},
    {"ldu", JOP_LOAD_UPVALUE},
    {"len", JOP_LENGTH},
    {"lt", JOP_LESS_THAN},
    {"lte", JOP_LESS_THAN_EQUAL},
    {"ltim", JOP_LESS_THAN_IMMEDIATE},
    {"mkarr", JOP_MAKE_ARRAY},
    {"mkbtp", JOP_MAKE_BRACKET_TUPLE},
    {"mkbuf", JOP_MAKE_BUFFER},
    {"mkscr", JOP_MAKE_SCRATCH_TUPLE},
    {"mkstr", JOP_MAKE_STRING},
    {"mkstu", JOP_MAKE_STRUCT},
    {"mktab", JOP_MAKE_TABLE},
    {"mktup", JOP_MAKE_TUPLE},
    {"mod", JOP_MODULO},
    {"movf", JOP_MOVE_FAR},
    {"movn", JOP_MOVE_NEAR},
    {"mul", JOP_MULTIPLY},
    {"mulim", JOP_MULTIPLY_IMMEDIATE},
    {"neq", JOP_NOT_EQUALS},
    {"neqim", JOP_NOT_EQUALS_IMMEDIATE},
    {"next", JOP_NEXT},
    {"noop", JOP_NOOP},
    {"prop", JOP_PROPAGATE},
    {"push", JOP_PUSH},
    {"push2", JOP_PUSH_2},
    {"push3", JOP_PUSH_3},
    {"pusha", JOP_PUSH_ARRAY},
    {"put", JOP_PUT},
    {"puti", JOP_PUT_INDEX},
    {"rem", JOP_REMAINDER},
    {"res", JOP_RESUME},
    {"ret", JOP_RETURN},
    {"retn", JOP_RETURN_NIL},
    {"setu", JOP_SET_UPVALUE},
    {"sig", JOP_SIGNAL},
    {"sl", JOP_SHIFT_LEFT},
    {"slim", JOP_SHIFT_LEFT_IMMEDIATE},
    {"sr", JOP_SHIFT_RIGHT},
    {"srim", JOP_SHIFT_RIGHT_IMMEDIATE},
    {"sru", JOP_SHIFT_RIGHT_UNSIGNED},
    {"sruim", JOP_SHIFT_RIGHT_UNSIGNED_IMMEDIATE},
    {"sub", JOP_SUBTRACT},
    {"subim", JOP_SUBTRACT_IMMEDIATE},
    {"tcall", JOP_TAILCALL},
    {"tchck", JOP_TYPECHECK}
};

/* Typename aliases for tchck instruction */
typedef struct TypeAlias {
    const char *name;
    int32_t mask;
} TypeAlias;

static const TypeAlias type_aliases[] = {
    {"abstract", BENDER_TFLAG_ABSTRACT},
    {"array", BENDER_TFLAG_ARRAY},
    {"boolean", BENDER_TFLAG_BOOLEAN},
    {"buffer", BENDER_TFLAG_BUFFER},
    {"callable", BENDER_TFLAG_CALLABLE},
    {"cfunction", BENDER_TFLAG_CFUNCTION},
    {"dictionary", BENDER_TFLAG_DICTIONARY},
    {"fiber", BENDER_TFLAG_FIBER},
    {"function", BENDER_TFLAG_FUNCTION},
    {"indexed", BENDER_TFLAG_INDEXED},
    {"keyword", BENDER_TFLAG_KEYWORD},
    {"nil", BENDER_TFLAG_NIL},
    {"number", BENDER_TFLAG_NUMBER},
    {"pointer", BENDER_TFLAG_POINTER},
    {"string", BENDER_TFLAG_STRING},
    {"struct", BENDER_TFLAG_STRUCT},
    {"symbol", BENDER_TFLAG_SYMBOL},
    {"table", BENDER_TFLAG_TABLE},
    {"tuple", BENDER_TFLAG_TUPLE}
};

/* Deinitialize an Assembler. Does not deinitialize the parents. */
static void bender_asm_deinit(BenderAssembler *a) {
    bender_table_deinit(&a->slots);
    bender_table_deinit(&a->labels);
    bender_table_deinit(&a->envs);
    bender_table_deinit(&a->defs);
}

static void bender_asm_longjmp(BenderAssembler *a) {
#if defined(BENDER_BSD) || defined(BENDER_APPLE)
    _longjmp(a->on_error, 1);
#else
    longjmp(a->on_error, 1);
#endif
}

/* Throw some kind of assembly error */
static void bender_asm_error(BenderAssembler *a, const char *message) {
    if (a->errindex < 0) {
        a->errmessage = bender_formatc("%s", message);
    } else {
        a->errmessage = bender_formatc("%s, instruction %d", message, a->errindex);
    }
    bender_asm_longjmp(a);
}
#define bender_asm_assert(a, c, m) do { if (!(c)) bender_asm_error((a), (m)); } while (0)

/* Throw some kind of assembly error */
static void bender_asm_errorv(BenderAssembler *a, const uint8_t *m) {
    a->errmessage = m;
    bender_asm_longjmp(a);
}

/* Add a closure environment to the assembler. Sub funcdefs may need
 * to reference outer function environments, and may change the outer environment.
 * Returns the index of the environment in the assembler's environments, or -1
 * if not found. */
static int32_t bender_asm_addenv(BenderAssembler *a, Bender envname) {
    Bender check;
    BenderFuncDef *def = a->def;
    int32_t envindex;
    int32_t res;
    if (bender_equals(a->name, envname)) {
        return -1;
    }
    /* Check for memoized value */
    check = bender_table_get(&a->envs, envname);
    if (bender_checktype(check, BENDER_NUMBER)) {
        return (int32_t) bender_unwrap_number(check);
    }
    if (NULL == a->parent) return -2;
    res = bender_asm_addenv(a->parent, envname);
    if (res < -1) {
        return res;
    }
    envindex = def->environments_length;
    bender_table_put(&a->envs, envname, bender_wrap_number(envindex));
    if (envindex >= a->environments_capacity) {
        int32_t newcap = 2 * envindex;
        def->environments = bender_realloc(def->environments, newcap * sizeof(int32_t));
        if (NULL == def->environments) {
            BENDER_OUT_OF_MEMORY;
        }
        a->environments_capacity = newcap;
    }
    def->environments[envindex] = (int32_t) res;
    def->environments_length = envindex + 1;
    return envindex;
}

/* Parse an argument to an assembly instruction, and return the result as an
 * integer. This integer will need to be bounds checked. */
static int32_t doarg_1(
    BenderAssembler *a,
    enum BenderOpArgType argtype,
    Bender x) {
    int32_t ret = -1;
    BenderTable *c;
    switch (argtype) {
        default:
            c = NULL;
            break;
        case BENDER_OAT_SLOT:
            c = &a->slots;
            break;
        case BENDER_OAT_ENVIRONMENT:
            c = &a->envs;
            break;
        case BENDER_OAT_LABEL:
            c = &a->labels;
            break;
        case BENDER_OAT_FUNCDEF:
            c = &a->defs;
            break;
    }
    switch (bender_type(x)) {
        default:
            goto error;
            break;
        case BENDER_NUMBER: {
            double y = bender_unwrap_number(x);
            if (bender_checkintrange(y)) {
                ret = (int32_t) y;
            } else {
                goto error;
            }
            break;
        }
        case BENDER_TUPLE: {
            const Bender *t = bender_unwrap_tuple(x);
            if (argtype == BENDER_OAT_TYPE) {
                int32_t i = 0;
                ret = 0;
                for (i = 0; i < bender_tuple_length(t); i++) {
                    ret |= doarg_1(a, BENDER_OAT_SIMPLETYPE, t[i]);
                }
            } else {
                goto error;
            }
            break;
        }
        case BENDER_KEYWORD: {
            if (NULL != c && argtype == BENDER_OAT_LABEL) {
                Bender result = bender_table_get(c, x);
                if (bender_checktype(result, BENDER_NUMBER)) {
                    ret = bender_unwrap_integer(result) - a->bytecode_count;
                } else {
                    goto error;
                }
            } else if (argtype == BENDER_OAT_TYPE || argtype == BENDER_OAT_SIMPLETYPE) {
                const TypeAlias *alias = bender_strbinsearch(
                                             &type_aliases,
                                             sizeof(type_aliases) / sizeof(TypeAlias),
                                             sizeof(TypeAlias),
                                             bender_unwrap_keyword(x));
                if (alias) {
                    ret = alias->mask;
                } else {
                    bender_asm_errorv(a, bender_formatc("unknown type %v", x));
                }
            } else {
                goto error;
            }
            break;
        }
        case BENDER_SYMBOL: {
            if (NULL != c) {
                Bender result = bender_table_get(c, x);
                if (bender_checktype(result, BENDER_NUMBER)) {
                    ret = (int32_t) bender_unwrap_number(result);
                } else {
                    bender_asm_errorv(a, bender_formatc("unknown name %v", x));
                }
            } else {
                goto error;
            }
            if (argtype == BENDER_OAT_ENVIRONMENT && ret == -1) {
                /* Add a new env */
                ret = bender_asm_addenv(a, x);
                if (ret < -1) {
                    bender_asm_errorv(a, bender_formatc("unknown environment %v", x));
                }
            }
            break;
        }
    }
    if (argtype == BENDER_OAT_SLOT && ret >= a->def->slotcount)
        a->def->slotcount = (int32_t) ret + 1;
    return ret;

error:
    bender_asm_errorv(a, bender_formatc("error parsing instruction argument %v", x));
    return 0;
}

/* Parse a single argument to an instruction. Trims it as well as
 * try to convert arguments to bit patterns */
static uint32_t doarg(
    BenderAssembler *a,
    enum BenderOpArgType argtype,
    int nth,
    int nbytes,
    int hassign,
    Bender x) {
    int32_t arg = doarg_1(a, argtype, x);
    /* Calculate the min and max values that can be stored given
     * nbytes, and whether or not the storage is signed */
    int32_t max = (1 << ((nbytes << 3) - hassign)) - 1;
    int32_t min = hassign ? -max - 1 : 0;
    if (arg < min)
        bender_asm_errorv(a, bender_formatc("instruction argument %v is too small, must be %d byte%s",
                                          x, nbytes, nbytes > 1 ? "s" : ""));
    if (arg > max)
        bender_asm_errorv(a, bender_formatc("instruction argument %v is too large, must be %d byte%s",
                                          x, nbytes, nbytes > 1 ? "s" : ""));
    return ((uint32_t) arg) << (nth << 3);
}

/* Provide parsing methods for the different kinds of arguments */
static uint32_t read_instruction(
    BenderAssembler *a,
    const BenderInstructionDef *idef,
    const Bender *argt) {
    uint32_t instr = idef->opcode;
    enum BenderInstructionType type = bender_instructions[idef->opcode];
    switch (type) {
        case JINT_0: {
            if (bender_tuple_length(argt) != 1)
                bender_asm_error(a, "expected 0 arguments: (op)");
            break;
        }
        case JINT_S: {
            if (bender_tuple_length(argt) != 2)
                bender_asm_error(a, "expected 1 argument: (op, slot)");
            instr |= doarg(a, BENDER_OAT_SLOT, 1, 2, 0, argt[1]);
            break;
        }
        case JINT_L: {
            if (bender_tuple_length(argt) != 2)
                bender_asm_error(a, "expected 1 argument: (op, label)");
            instr |= doarg(a, BENDER_OAT_LABEL, 1, 3, 1, argt[1]);
            break;
        }
        case JINT_SS: {
            if (bender_tuple_length(argt) != 3)
                bender_asm_error(a, "expected 2 arguments: (op, slot, slot)");
            instr |= doarg(a, BENDER_OAT_SLOT, 1, 1, 0, argt[1]);
            instr |= doarg(a, BENDER_OAT_SLOT, 2, 2, 0, argt[2]);
            break;
        }
        case JINT_SL: {
            if (bender_tuple_length(argt) != 3)
                bender_asm_error(a, "expected 2 arguments: (op, slot, label)");
            instr |= doarg(a, BENDER_OAT_SLOT, 1, 1, 0, argt[1]);
            instr |= doarg(a, BENDER_OAT_LABEL, 2, 2, 1, argt[2]);
            break;
        }
        case JINT_ST: {
            if (bender_tuple_length(argt) != 3)
                bender_asm_error(a, "expected 2 arguments: (op, slot, type)");
            instr |= doarg(a, BENDER_OAT_SLOT, 1, 1, 0, argt[1]);
            instr |= doarg(a, BENDER_OAT_TYPE, 2, 2, 0, argt[2]);
            break;
        }
        case JINT_SI:
        case JINT_SU: {
            if (bender_tuple_length(argt) != 3)
                bender_asm_error(a, "expected 2 arguments: (op, slot, integer)");
            instr |= doarg(a, BENDER_OAT_SLOT, 1, 1, 0, argt[1]);
            instr |= doarg(a, BENDER_OAT_INTEGER, 2, 2, type == JINT_SI, argt[2]);
            break;
        }
        case JINT_SD: {
            if (bender_tuple_length(argt) != 3)
                bender_asm_error(a, "expected 2 arguments: (op, slot, funcdef)");
            instr |= doarg(a, BENDER_OAT_SLOT, 1, 1, 0, argt[1]);
            instr |= doarg(a, BENDER_OAT_FUNCDEF, 2, 2, 0, argt[2]);
            break;
        }
        case JINT_SSS: {
            if (bender_tuple_length(argt) != 4)
                bender_asm_error(a, "expected 3 arguments: (op, slot, slot, slot)");
            instr |= doarg(a, BENDER_OAT_SLOT, 1, 1, 0, argt[1]);
            instr |= doarg(a, BENDER_OAT_SLOT, 2, 1, 0, argt[2]);
            instr |= doarg(a, BENDER_OAT_SLOT, 3, 1, 0, argt[3]);
            break;
        }
        case JINT_SSI:
        case JINT_SSU: {
            if (bender_tuple_length(argt) != 4)
                bender_asm_error(a, "expected 3 arguments: (op, slot, slot, integer)");
            instr |= doarg(a, BENDER_OAT_SLOT, 1, 1, 0, argt[1]);
            instr |= doarg(a, BENDER_OAT_SLOT, 2, 1, 0, argt[2]);
            instr |= doarg(a, BENDER_OAT_INTEGER, 3, 1, type == JINT_SSI, argt[3]);
            break;
        }
        case JINT_SES: {
            BenderAssembler *b = a;
            uint32_t env;
            if (bender_tuple_length(argt) != 4)
                bender_asm_error(a, "expected 3 arguments: (op, slot, environment, envslot)");
            instr |= doarg(a, BENDER_OAT_SLOT, 1, 1, 0, argt[1]);
            env = doarg(a, BENDER_OAT_ENVIRONMENT, 0, 1, 0, argt[2]);
            instr |= env << 16;
            for (env += 1; env > 0; env--) {
                b = b->parent;
                if (NULL == b)
                    bender_asm_error(a, "invalid environment index");
            }
            instr |= doarg(b, BENDER_OAT_SLOT, 3, 1, 0, argt[3]);
            break;
        }
        case JINT_SC: {
            if (bender_tuple_length(argt) != 3)
                bender_asm_error(a, "expected 2 arguments: (op, slot, constant)");
            instr |= doarg(a, BENDER_OAT_SLOT, 1, 1, 0, argt[1]);
            instr |= doarg(a, BENDER_OAT_CONSTANT, 2, 2, 0, argt[2]);
            break;
        }
    }
    return instr;
}

/* Helper to get from a structure */
static Bender bender_get1(Bender ds, Bender key) {
    switch (bender_type(ds)) {
        default:
            return bender_wrap_nil();
        case BENDER_TABLE:
            return bender_table_get(bender_unwrap_table(ds), key);
        case BENDER_STRUCT:
            return bender_struct_get(bender_unwrap_struct(ds), key);
    }
}

/* Helper to assembly. Return the assembly result */
static BenderAssembleResult bender_asm1(BenderAssembler *parent, Bender source, int flags) {
    BenderAssembleResult result;
    BenderAssembler a;
    Bender s = source;
    BenderFuncDef *def;
    int32_t count, i;
    const Bender *arr;
    Bender x;

    /* Initialize funcdef */
    def = bender_funcdef_alloc();

    /* Initialize Assembler */
    a.def = def;
    a.parent = parent;
    a.errmessage = NULL;
    a.errindex = 0;
    a.environments_capacity = 0;
    a.bytecode_count = 0;
    a.defs_capacity = 0;
    a.name = bender_wrap_nil();
    bender_table_init(&a.labels, 0);
    bender_table_init(&a.slots, 0);
    bender_table_init(&a.envs, 0);
    bender_table_init(&a.defs, 0);

    /* Set error jump */
#if defined(BENDER_BSD) || defined(BENDER_APPLE)
    if (_setjmp(a.on_error)) {
#else
    if (setjmp(a.on_error)) {
#endif
        if (NULL != a.parent) {
            bender_asm_deinit(&a);
            a.parent->errmessage = a.errmessage;
            bender_asm_longjmp(a.parent);
        }
        result.funcdef = NULL;
        result.error = a.errmessage;
        result.status = BENDER_ASSEMBLE_ERROR;
        bender_asm_deinit(&a);
        return result;
    }

    bender_asm_assert(&a,
                     bender_checktype(s, BENDER_STRUCT) ||
                     bender_checktype(s, BENDER_TABLE),
                     "expected struct or table for assembly source");

    /* Check for function name */
    a.name = bender_get1(s, bender_ckeywordv("name"));
    if (!bender_checktype(a.name, BENDER_NIL)) {
        def->name = bender_to_string(a.name);
    }

    /* Set function arity */
    x = bender_get1(s, bender_ckeywordv("arity"));
    def->arity = bender_checkint(x) ? bender_unwrap_integer(x) : 0;
    bender_asm_assert(&a, def->arity >= 0, "arity must be non-negative");

    x = bender_get1(s, bender_ckeywordv("max-arity"));
    def->max_arity = bender_checkint(x) ? bender_unwrap_integer(x) : def->arity;
    bender_asm_assert(&a, def->max_arity >= def->arity, "max-arity must be greater than or equal to arity");

    x = bender_get1(s, bender_ckeywordv("min-arity"));
    def->min_arity = bender_checkint(x) ? bender_unwrap_integer(x) : def->arity;
    bender_asm_assert(&a, def->min_arity <= def->arity, "min-arity must be less than or equal to arity");

    /* Check vararg */
    x = bender_get1(s, bender_ckeywordv("vararg"));
    if (bender_truthy(x)) def->flags |= BENDER_FUNCDEF_FLAG_VARARG;

    /* Initialize slotcount */
    def->slotcount = !!(def->flags & BENDER_FUNCDEF_FLAG_VARARG) + def->arity;

    /* Check structarg */
    x = bender_get1(s, bender_ckeywordv("structarg"));
    if (bender_truthy(x)) def->flags |= BENDER_FUNCDEF_FLAG_STRUCTARG;

    /* Check namedarg */
    x = bender_get1(s, bender_ckeywordv("namedargs"));
    if (bender_checkint(x)) {
        def->flags |= BENDER_FUNCDEF_FLAG_NAMEDARGS;
        def->named_args_count = bender_unwrap_integer(x);
    }

    /* Check source */
    x = bender_get1(s, bender_ckeywordv("source"));
    if (bender_checktype(x, BENDER_STRING)) def->source = bender_unwrap_string(x);

    /* Create slot aliases */
    x = bender_get1(s, bender_ckeywordv("slots"));
    if (bender_indexed_view(x, &arr, &count)) {
        for (i = 0; i < count; i++) {
            Bender v = arr[i];
            if (bender_checktype(v, BENDER_TUPLE)) {
                const Bender *t = bender_unwrap_tuple(v);
                int32_t j;
                for (j = 0; j < bender_tuple_length(t); j++) {
                    if (!bender_checktype(t[j], BENDER_SYMBOL))
                        bender_asm_error(&a, "slot names must be symbols");
                    bender_table_put(&a.slots, t[j], bender_wrap_integer(i));
                }
            } else if (bender_checktype(v, BENDER_SYMBOL)) {
                bender_table_put(&a.slots, v, bender_wrap_integer(i));
            } else {
                bender_asm_error(&a, "slot names must be symbols or tuple of symbols");
            }
        }
    }

    /* Parse constants */
    x = bender_get1(s, bender_ckeywordv("constants"));
    if (bender_indexed_view(x, &arr, &count)) {
        def->constants_length = count;
        def->constants = array_allocate(sizeof(Bender), count);
        if (NULL == def->constants) {
            BENDER_OUT_OF_MEMORY;
        }
        for (i = 0; i < count; i++) {
            Bender ct = arr[i];
            def->constants[i] = ct;
        }
    } else {
        def->constants = NULL;
        def->constants_length = 0;
    }

    /* Parse sub funcdefs */
    x = bender_get1(s, bender_ckeywordv("closures"));
    if (bender_checktype(x, BENDER_NIL)) {
        x = bender_get1(s, bender_ckeywordv("defs"));
    }
    if (bender_indexed_view(x, &arr, &count)) {
        int32_t i;
        for (i = 0; i < count; i++) {
            BenderAssembleResult subres;
            Bender subname;
            int32_t newlen;
            subres = bender_asm1(&a, arr[i], flags);
            if (subres.status != BENDER_ASSEMBLE_OK) {
                bender_asm_errorv(&a, subres.error);
            }
            subname = bender_get1(arr[i], bender_ckeywordv("name"));
            if (!bender_checktype(subname, BENDER_NIL)) {
                bender_table_put(&a.defs, subname, bender_wrap_integer(def->defs_length));
            }
            newlen = def->defs_length + 1;
            if (a.defs_capacity < newlen) {
                int32_t newcap = newlen;
                def->defs = bender_realloc(def->defs, newcap * sizeof(BenderFuncDef *));
                if (NULL == def->defs) {
                    BENDER_OUT_OF_MEMORY;
                }
                a.defs_capacity = newcap;
            }
            def->defs[def->defs_length] = subres.funcdef;
            def->defs_length = newlen;
        }
    }

    /* Parse bytecode and labels */
    x = bender_get1(s, bender_ckeywordv("bytecode"));
    if (bender_indexed_view(x, &arr, &count)) {
        /* Do labels and find length */
        int32_t blength = 0;
        for (i = 0; i < count; ++i) {
            Bender instr = arr[i];
            if (bender_checktype(instr, BENDER_KEYWORD)) {
                bender_table_put(&a.labels, instr, bender_wrap_integer(blength));
            } else if (bender_checktype(instr, BENDER_TUPLE)) {
                blength++;
            } else {
                a.errindex = i;
                bender_asm_error(&a, "expected assembly instruction");
            }
        }
        /* Allocate bytecode array */
        def->bytecode_length = blength;
        def->bytecode = array_allocate(sizeof(uint32_t), blength);
        if (NULL == def->bytecode) {
            BENDER_OUT_OF_MEMORY;
        }
        /* Do bytecode */
        for (i = 0; i < count; ++i) {
            Bender instr = arr[i];
            if (bender_checktype(instr, BENDER_KEYWORD)) {
                continue;
            } else {
                uint32_t op;
                const BenderInstructionDef *idef;
                const Bender *t;
                a.errindex = i;
                bender_asm_assert(&a, bender_checktype(instr, BENDER_TUPLE), "expected tuple");
                t = bender_unwrap_tuple(instr);
                if (bender_tuple_length(t) == 0) {
                    op = 0;
                } else {
                    bender_asm_assert(&a, bender_checktype(t[0], BENDER_SYMBOL),
                                     "expected symbol in assembly instruction");
                    idef = bender_strbinsearch(
                               &bender_ops,
                               sizeof(bender_ops) / sizeof(BenderInstructionDef),
                               sizeof(BenderInstructionDef),
                               bender_unwrap_symbol(t[0]));
                    if (NULL == idef)
                        bender_asm_errorv(&a, bender_formatc("unknown instruction %v", t[0]));
                    op = read_instruction(&a, idef, t);
                }
                def->bytecode[a.bytecode_count++] = op;
            }
        }
    } else {
        bender_asm_error(&a, "bytecode expected");
    }
    a.errindex = -1;

    /* Check for source mapping */
    x = bender_get1(s, bender_ckeywordv("sourcemap"));
    if (bender_indexed_view(x, &arr, &count)) {
        bender_asm_assert(&a, count == def->bytecode_length, "sourcemap must have the same length as the bytecode");
        def->sourcemap = bender_malloc(sizeof(BenderSourceMapping) * (size_t) count);
        if (NULL == def->sourcemap) {
            BENDER_OUT_OF_MEMORY;
        }
        for (i = 0; i < count; i++) {
            const Bender *tup;
            Bender entry = arr[i];
            BenderSourceMapping mapping;
            if (!bender_checktype(entry, BENDER_TUPLE)) {
                bender_asm_error(&a, "expected tuple");
            }
            tup = bender_unwrap_tuple(entry);
            if (!bender_checkint(tup[0])) {
                bender_asm_error(&a, "expected integer");
            }
            if (!bender_checkint(tup[1])) {
                bender_asm_error(&a, "expected integer");
            }
            mapping.line = bender_unwrap_integer(tup[0]);
            mapping.column = bender_unwrap_integer(tup[1]);
            def->sourcemap[i] = mapping;
        }
    }

    /* Set symbolmap */
    def->symbolmap = NULL;
    def->symbolmap_length = 0;
    x = bender_get1(s, bender_ckeywordv("symbolmap"));
    if (bender_indexed_view(x, &arr, &count)) {
        def->symbolmap_length = count;
        def->symbolmap = bender_malloc(sizeof(BenderSymbolMap) * (size_t)count);
        if (NULL == def->symbolmap) {
            BENDER_OUT_OF_MEMORY;
        }
        for (i = 0; i < count; i++) {
            const Bender *tup;
            Bender entry = arr[i];
            BenderSymbolMap ss;
            if (!bender_checktype(entry, BENDER_TUPLE)) {
                bender_asm_error(&a, "expected tuple");
            }
            tup = bender_unwrap_tuple(entry);
            if (bender_keyeq(tup[0], "upvalue")) {
                ss.birth_pc = UINT32_MAX;
            } else if (!bender_checkint(tup[0])) {
                bender_asm_error(&a, "expected integer");
            } else {
                ss.birth_pc = bender_unwrap_integer(tup[0]);
            }
            if (!bender_checkint(tup[1])) {
                bender_asm_error(&a, "expected integer");
            }
            if (!bender_checkint(tup[2])) {
                bender_asm_error(&a, "expected integer");
            }
            if (!bender_checktype(tup[3], BENDER_SYMBOL)) {
                bender_asm_error(&a, "expected symbol");
            }
            ss.death_pc = bender_unwrap_integer(tup[1]);
            ss.slot_index = bender_unwrap_integer(tup[2]);
            ss.symbol = bender_unwrap_symbol(tup[3]);
            def->symbolmap[i] = ss;
        }
    }
    if (def->symbolmap_length) def->flags |= BENDER_FUNCDEF_FLAG_HASSYMBOLMAP;

    /* Set environments */
    x = bender_get1(s, bender_ckeywordv("environments"));
    if (bender_indexed_view(x, &arr, &count)) {
        def->environments_length = count;
        if (def->environments_length) {
            def->environments = bender_realloc(def->environments, def->environments_length * sizeof(int32_t));
        }
        for (int32_t i = 0; i < count; i++) {
            if (!bender_checkint(arr[i])) {
                bender_asm_error(&a, "expected integer");
            }
            def->environments[i] = bender_unwrap_integer(arr[i]);
        }
    }
    if (def->environments_length && NULL == def->environments) {
        BENDER_OUT_OF_MEMORY;
    }

    /* Verify the func def */
    int verify_status = bender_verify(def);
    if (verify_status) {
        bender_asm_errorv(&a, bender_formatc("invalid assembly (%d)", verify_status));
    }

    /* Optionally do optimization and a second verify step */
    if (flags & BENDER_ASSEMBLE_FLAG_OPTIMIZE) {
        int32_t optimization_level = 1; /* TODO */
        bender_bytecode_optimize(def, optimization_level);
        int verify_status = bender_verify(def);
        if (verify_status) {
            bender_asm_errorv(&a, bender_formatc("invalid assembly after optimization (%d)", verify_status));
        }
    }

    /* Add final flags */
    bender_def_addflags(def);

    /* Finish everything and return funcdef */
    bender_asm_deinit(&a);
    result.error = NULL;
    result.funcdef = def;
    result.status = BENDER_ASSEMBLE_OK;
    return result;
}

/* Assemble a function */
BenderAssembleResult bender_asm(Bender source, int flags) {
    return bender_asm1(NULL, source, flags);
}

/* Disassembly */

/* Find the definition of an instruction given the instruction word. Return
 * NULL if not found. */
static const BenderInstructionDef *bender_asm_reverse_lookup(uint32_t instr) {
    size_t i;
    uint32_t opcode = instr & 0x7F;
    for (i = 0; i < sizeof(bender_ops) / sizeof(BenderInstructionDef); i++) {
        const BenderInstructionDef *def = bender_ops + i;
        if (def->opcode == opcode)
            return def;
    }
    return NULL;
}

/* Create some constant sized tuples */
static const Bender *tup1(Bender x) {
    Bender *tup = bender_tuple_begin(1);
    tup[0] = x;
    return bender_tuple_end(tup);
}
static const Bender *tup2(Bender x, Bender y) {
    Bender *tup = bender_tuple_begin(2);
    tup[0] = x;
    tup[1] = y;
    return bender_tuple_end(tup);
}
static const Bender *tup3(Bender x, Bender y, Bender z) {
    Bender *tup = bender_tuple_begin(3);
    tup[0] = x;
    tup[1] = y;
    tup[2] = z;
    return bender_tuple_end(tup);
}
static const Bender *tup4(Bender w, Bender x, Bender y, Bender z) {
    Bender *tup = bender_tuple_begin(4);
    tup[0] = w;
    tup[1] = x;
    tup[2] = y;
    tup[3] = z;
    return bender_tuple_end(tup);
}

/* Given an argument, convert it to the appropriate integer or symbol */
Bender bender_asm_decode_instruction(uint32_t instr) {
    const BenderInstructionDef *def = bender_asm_reverse_lookup(instr);
    Bender name;
    if (NULL == def) {
        return bender_wrap_integer((int32_t)instr);
    }
    name = bender_csymbolv(def->name);
    const Bender *ret = NULL;
#define oparg(shift, mask) ((instr >> ((shift) << 3)) & (mask))
    switch (bender_instructions[def->opcode]) {
        case JINT_0:
            ret = tup1(name);
            break;
        case JINT_S:
            ret = tup2(name, bender_wrap_integer(oparg(1, 0xFFFFFF)));
            break;
        case JINT_L:
            ret = tup2(name, bender_wrap_integer((int32_t)instr >> 8));
            break;
        case JINT_SS:
        case JINT_ST:
        case JINT_SC:
        case JINT_SU:
        case JINT_SD:
            ret = tup3(name,
                       bender_wrap_integer(oparg(1, 0xFF)),
                       bender_wrap_integer(oparg(2, 0xFFFF)));
            break;
        case JINT_SI:
        case JINT_SL:
            ret =  tup3(name,
                        bender_wrap_integer(oparg(1, 0xFF)),
                        bender_wrap_integer((int32_t)instr >> 16));
            break;
        case JINT_SSS:
        case JINT_SES:
        case JINT_SSU:
            ret = tup4(name,
                       bender_wrap_integer(oparg(1, 0xFF)),
                       bender_wrap_integer(oparg(2, 0xFF)),
                       bender_wrap_integer(oparg(3, 0xFF)));
            break;
        case JINT_SSI:
            ret = tup4(name,
                       bender_wrap_integer(oparg(1, 0xFF)),
                       bender_wrap_integer(oparg(2, 0xFF)),
                       bender_wrap_integer((int32_t)instr >> 24));
            break;
    }
#undef oparg
    if (ret) {
        /* Check if break point set */
        if (instr & 0x80) {
            bender_tuple_flag(ret) |= BENDER_TUPLE_FLAG_BRACKETCTOR;
        }
        return bender_wrap_tuple(ret);
    }
    return bender_wrap_nil();
}

/*
 * Disasm sections
 */

static Bender bender_disasm_arity(BenderFuncDef *def) {
    return bender_wrap_integer(def->arity);
}

static Bender bender_disasm_min_arity(BenderFuncDef *def) {
    return bender_wrap_integer(def->min_arity);
}

static Bender bender_disasm_max_arity(BenderFuncDef *def) {
    return bender_wrap_integer(def->max_arity);
}

static Bender bender_disasm_slotcount(BenderFuncDef *def) {
    return bender_wrap_integer(def->slotcount);
}

static Bender bender_disasm_symbolslots(BenderFuncDef *def) {
    if (def->symbolmap == NULL) {
        return bender_wrap_nil();
    }
    BenderArray *symbolslots = bender_array(def->symbolmap_length);
    Bender upvaluekw = bender_ckeywordv("upvalue");
    for (int32_t i = 0; i < def->symbolmap_length; i++) {
        BenderSymbolMap ss = def->symbolmap[i];
        Bender *t = bender_tuple_begin(4);
        if (ss.birth_pc == UINT32_MAX) {
            t[0] = upvaluekw;
        } else {
            t[0] = bender_wrap_integer(ss.birth_pc);
        }
        t[1] = bender_wrap_integer(ss.death_pc);
        t[2] = bender_wrap_integer(ss.slot_index);
        t[3] = bender_wrap_symbol(ss.symbol);
        symbolslots->data[i] = bender_wrap_tuple(bender_tuple_end(t));
    }
    symbolslots->count = def->symbolmap_length;
    return bender_wrap_array(symbolslots);
}

static Bender bender_disasm_bytecode(BenderFuncDef *def) {
    BenderArray *bcode = bender_array(def->bytecode_length);
    for (int32_t i = 0; i < def->bytecode_length; i++) {
        bcode->data[i] = bender_asm_decode_instruction(def->bytecode[i]);
    }
    bcode->count = def->bytecode_length;
    return bender_wrap_array(bcode);
}

static Bender bender_disasm_source(BenderFuncDef *def) {
    if (def->source != NULL) return bender_wrap_string(def->source);
    return bender_wrap_nil();
}

static Bender bender_disasm_name(BenderFuncDef *def) {
    if (def->name != NULL) return bender_wrap_string(def->name);
    return bender_wrap_nil();
}

static Bender bender_disasm_vararg(BenderFuncDef *def) {
    return bender_wrap_boolean(def->flags & BENDER_FUNCDEF_FLAG_VARARG);
}

static Bender bender_disasm_structarg(BenderFuncDef *def) {
    return bender_wrap_boolean(def->flags & BENDER_FUNCDEF_FLAG_STRUCTARG);
}

static Bender bender_disasm_namedargs(BenderFuncDef *def) {
    if (def->flags & BENDER_FUNCDEF_FLAG_NAMEDARGS) {
        return bender_wrap_integer(def->named_args_count);
    } else {
        return bender_wrap_nil();
    }
}

static Bender bender_disasm_constants(BenderFuncDef *def) {
    BenderArray *constants = bender_array(def->constants_length);
    for (int32_t i = 0; i < def->constants_length; i++) {
        constants->data[i] = def->constants[i];
    }
    constants->count = def->constants_length;
    return bender_wrap_array(constants);
}

static Bender bender_disasm_sourcemap(BenderFuncDef *def) {
    if (NULL == def->sourcemap) return bender_wrap_nil();
    BenderArray *sourcemap = bender_array(def->bytecode_length);
    for (int32_t i = 0; i < def->bytecode_length; i++) {
        Bender *t = bender_tuple_begin(2);
        BenderSourceMapping mapping = def->sourcemap[i];
        t[0] = bender_wrap_integer(mapping.line);
        t[1] = bender_wrap_integer(mapping.column);
        sourcemap->data[i] = bender_wrap_tuple(bender_tuple_end(t));
    }
    sourcemap->count = def->bytecode_length;
    return bender_wrap_array(sourcemap);
}

static Bender bender_disasm_environments(BenderFuncDef *def) {
    BenderArray *envs = bender_array(def->environments_length);
    for (int32_t i = 0; i < def->environments_length; i++) {
        envs->data[i] = bender_wrap_integer(def->environments[i]);
    }
    envs->count = def->environments_length;
    return bender_wrap_array(envs);
}

static Bender bender_disasm_defs(BenderFuncDef *def) {
    BenderArray *defs = bender_array(def->defs_length);
    for (int32_t i = 0; i < def->defs_length; i++) {
        defs->data[i] = bender_disasm(def->defs[i]);
    }
    defs->count = def->defs_length;
    return bender_wrap_array(defs);
}

Bender bender_disasm(BenderFuncDef *def) {
    BenderTable *ret = bender_table(10);
    bender_table_put(ret, bender_ckeywordv("arity"), bender_disasm_arity(def));
    bender_table_put(ret, bender_ckeywordv("min-arity"), bender_disasm_min_arity(def));
    bender_table_put(ret, bender_ckeywordv("max-arity"), bender_disasm_max_arity(def));
    bender_table_put(ret, bender_ckeywordv("bytecode"), bender_disasm_bytecode(def));
    BenderArray *words = bender_array(def->bytecode_length);
    for (int32_t i = 0; i < def->bytecode_length; i++)
        bender_array_push(words, bender_wrap_number(def->bytecode[i]));
    bender_table_put(ret, bender_ckeywordv("bytecode-words"), bender_wrap_array(words));
    bender_table_put(ret, bender_ckeywordv("flags"), bender_wrap_integer(def->flags));
    bender_table_put(ret, bender_ckeywordv("source"), bender_disasm_source(def));
    bender_table_put(ret, bender_ckeywordv("vararg"), bender_disasm_vararg(def));
    bender_table_put(ret, bender_ckeywordv("structarg"), bender_disasm_structarg(def));
    bender_table_put(ret, bender_ckeywordv("namedargs"), bender_disasm_namedargs(def));
    bender_table_put(ret, bender_ckeywordv("name"), bender_disasm_name(def));
    bender_table_put(ret, bender_ckeywordv("slotcount"), bender_disasm_slotcount(def));
    bender_table_put(ret, bender_ckeywordv("symbolmap"), bender_disasm_symbolslots(def));
    bender_table_put(ret, bender_ckeywordv("constants"), bender_disasm_constants(def));
    bender_table_put(ret, bender_ckeywordv("sourcemap"), bender_disasm_sourcemap(def));
    bender_table_put(ret, bender_ckeywordv("environments"), bender_disasm_environments(def));
    bender_table_put(ret, bender_ckeywordv("defs"), bender_disasm_defs(def));
    return bender_wrap_struct(bender_table_to_struct(ret));
}

BENDER_CORE_FN(cfun_asm,
              "(asm assembly &opt flags)",
              "Returns a new function that is the compiled result of the assembly.\n"
              "The syntax for the assembly can be found on the Bender website, and should correspond\n"
              "to the return value of disasm. Will throw an\n"
              "error on invalid assembly. Supports a keyword `flags` to control assembly.\n"
              "The only supported flag is `:o`, which will optimize bytecode automatically.") {
    bender_sandbox_assert(BENDER_SANDBOX_ASM);
    bender_arity(argc, 1, 2);
    uint64_t flags = 0;
    if (argc >= 2) {
        flags = bender_getflags(argv, 1, "o");
    }
    BenderAssembleResult res;
    res = bender_asm(argv[0], (int32_t) flags);
    if (res.status != BENDER_ASSEMBLE_OK) {
        bender_panics(res.error ? res.error : bender_cstring("invalid assembly"));
    }
    return bender_wrap_function(bender_thunk(res.funcdef));
}

BENDER_CORE_FN(cfun_disasm,
              "(disasm func &opt field)",
              "Returns assembly that could be used to compile the given function. "
              "func must be a function, not a c function. Will throw on error on a badly "
              "typed argument. If given a field name, will only return that part of the function assembly. "
              "Possible fields are:\n\n"
              "* :arity - number of required and optional arguments.\n"
              "* :min-arity - minimum number of arguments function can be called with.\n"
              "* :max-arity - maximum number of arguments function can be called with.\n"
              "* :vararg - true if function can take a variable number of arguments.\n"
              "* :structarg - true if function can take a variable number of arguments using the &keys option.\n"
              "* :namedargs - if function can take a variable number of arguments using the &named option, this will be the number of named arguments.\n"
              "* :bytecode - array of parsed bytecode instructions. Each instruction is a tuple.\n"
              "* :source - name of source file that this function was compiled from.\n"
              "* :name - name of function.\n"
              "* :slotcount - how many virtual registers, or slots, this function uses. Corresponds to stack space used by function.\n"
              "* :symbolmap - all symbols and their slots.\n"
              "* :constants - an array of constants referenced by this function.\n"
              "* :sourcemap - a mapping of each bytecode instruction to a line and column in the source file.\n"
              "* :environments - an internal mapping of which enclosing functions are referenced for bindings.\n"
              "* :defs - other function definitions that this function may instantiate.\n") {
    bender_sandbox_assert(BENDER_SANDBOX_ASM);
    bender_arity(argc, 1, 2);
    BenderFunction *f = bender_getfunction(argv, 0);
    if (argc == 2) {
        BenderKeyword kw = bender_getkeyword(argv, 1);
        if (!bender_cstrcmp(kw, "arity")) return bender_disasm_arity(f->def);
        if (!bender_cstrcmp(kw, "min-arity")) return bender_disasm_min_arity(f->def);
        if (!bender_cstrcmp(kw, "max-arity")) return bender_disasm_max_arity(f->def);
        if (!bender_cstrcmp(kw, "bytecode")) return bender_disasm_bytecode(f->def);
        if (!bender_cstrcmp(kw, "source")) return bender_disasm_source(f->def);
        if (!bender_cstrcmp(kw, "name")) return bender_disasm_name(f->def);
        if (!bender_cstrcmp(kw, "vararg")) return bender_disasm_vararg(f->def);
        if (!bender_cstrcmp(kw, "structarg")) return bender_disasm_structarg(f->def);
        if (!bender_cstrcmp(kw, "namedargs")) return bender_disasm_namedargs(f->def);
        if (!bender_cstrcmp(kw, "slotcount")) return bender_disasm_slotcount(f->def);
        if (!bender_cstrcmp(kw, "symbolmap")) return bender_disasm_symbolslots(f->def);
        if (!bender_cstrcmp(kw, "constants")) return bender_disasm_constants(f->def);
        if (!bender_cstrcmp(kw, "sourcemap")) return bender_disasm_sourcemap(f->def);
        if (!bender_cstrcmp(kw, "environments")) return bender_disasm_environments(f->def);
        if (!bender_cstrcmp(kw, "defs")) return bender_disasm_defs(f->def);
        bender_panicf("unknown disasm key %v", argv[1]);
    } else {
        return bender_disasm(f->def);
    }
}

/* Load the library */
void bender_lib_asm(BenderTable *env) {
    BenderRegExt asm_cfuns[] = {
        BENDER_CORE_REG("asm", cfun_asm),
        BENDER_CORE_REG("disasm", cfun_disasm),
        BENDER_REG_END
    };
    bender_core_cfuns_ext(env, NULL, asm_cfuns);
}

#endif
