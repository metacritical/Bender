/* Exercise the actual emitted closure-reader executor at its ldu PC.
 * Deliberately bypass registration: malformed runtime envs must deopt before
 * touching the destination, not merely be rejected by the descriptor verifier. */
#include "state.h"
#include "fiber.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include NATIVE_EXECUTOR_C

static void refused(const BenderNativeCode *code, BenderFiber *f,
                    BenderFuncDef *d, int k, int dest) {
    Bender sentinel = bender_wrap_integer(1234567);
    Bender *slots = f->data + f->frame;
    slots[dest] = sentinel;
    uint32_t *pc = d->bytecode + k;
    assert(code->execute(f, d, &pc) == BENDER_NATIVE_VM);
    assert(pc == d->bytecode + k);
    assert(bender_equals(slots[dest], sentinel));
}
int main(void) {
    bender_init();
    const BenderNativeCode *code = &NATIVE_CLOSURE_CODE;
    int k = -1;
    for (int i = 0; i < code->length; i++)
        if ((code->words[i] & 255) == JOP_LOAD_UPVALUE) { k = i; break; }
    assert(k >= 0);
    uint32_t word = code->words[k];
    int dest = (word >> 8) & 255;
    int ei = (word >> 16) & 255;
    int vi = (word >> 24) & 255;
    BenderFuncDef *d = bender_funcdef_alloc();
    d->slotcount = code->slotcount;
    d->flags = code->flags;
    d->arity = code->arity;
    d->min_arity = code->min_arity;
    d->max_arity = code->max_arity;
    d->constants_length = code->constants_length;
    d->constants = calloc((size_t)(code->constants_length + 1), sizeof(Bender));
    for (int i = 0; i < code->constants_length; i++) {
        const BenderNativeConstant *c = code->constants + i;
        d->constants[i] = c->type == BENDER_NUMBER ? bender_wrap_number(c->number)
            : c->type == BENDER_BOOLEAN ? bender_wrap_boolean(c->number != 0)
            : bender_wrap_nil(); /* Opaque constants have no identity requirement. */
    }
    d->bytecode_length = code->length;
    d->bytecode = bender_malloc((size_t)code->length * sizeof(uint32_t));
    memcpy(d->bytecode, code->words, (size_t)code->length * sizeof(uint32_t));
    d->environments_length = ei + 1;
    assert(bender_verify(d) == 0);
    assert(bender_native_matches(d, code));
    d->native_code = code;
    assert(bender_native_revalidate(d));
    /* Revalidation must not reuse a verifier proof after an invalid count
     * change, even though environment count is not descriptor payload. */
    const int invalid_counts[] = {-1, 257, ei};
    for (int i = 0; i < 3; i++) {
        d->environments_length = invalid_counts[i];
        bender_vm.native_epoch++;
        assert(!bender_native_matches(d, code));
        assert(!bender_native_revalidate(d));
    }
    d->environments_length = ei + 1;
    bender_vm.native_epoch++;
    assert(bender_native_revalidate(d));
    /* Zero-environment non-ldu opcodes must not accidentally share the B check. */
    uint32_t plain_words[] = {JOP_LOAD_NIL, JOP_RETURN};
    uint64_t plain_mask = 3;
    BenderNativeCode plain_code = *code;
    plain_code.words = plain_words; plain_code.length = 2;
    plain_code.slotcount = 1; plain_code.arity = 0;
    plain_code.min_arity = 0; plain_code.max_arity = 0; plain_code.flags = 0;
    plain_code.constants_length = 0;
    plain_code.region_mask = &plain_mask; plain_code.region_mask_words = 1;
    BenderFuncDef *plain = bender_funcdef_alloc();
    plain->slotcount = 1; plain->bytecode_length = 2; plain->max_arity = 0;
    plain->bytecode = plain_words;
    assert(bender_native_matches(plain, &plain_code));
    BenderFunction *fn = calloc(1, sizeof(BenderFunction) +
                               (size_t)(ei + 1) * sizeof(BenderFuncEnv *));
    assert(fn);
    fn->def = d;
    BenderFiber storage;
    memset(&storage, 0, sizeof(storage));
    BenderFiber *f = &storage;
    f->frame = BENDER_FRAME_SIZE;
    f->capacity = f->frame + d->slotcount;
    f->data = calloc((size_t)f->capacity, sizeof(Bender));
    assert(f->data);
    bender_fiber_frame(f)->func = fn;
    BenderFuncEnv env;
    memset(&env, 0, sizeof(env));
    fn->envs[ei] = &env;
    /* C == length is invalid, with no off-stack values to dereference. */
    env.length = vi;
    refused(code, f, d, k, dest);
    /* Untrusted negative offset not corresponding to any frame. The VM's
     * validator normalizes invalid metadata; no ldu destination may commit. */
    env.length = vi + 1;
    env.offset = -1;
    env.as.fiber = f;
    refused(code, f, d, k, dest);
    assert(env.offset == 0 && env.length == 0 && env.as.values == NULL);
    puts("native closure emitted ldu guard fallback passed");
    /* fn/env are synthetic, not GC-owned; do not collect them. */
    return 0;
}
