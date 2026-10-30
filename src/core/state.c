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
#include "util.h"
#endif

#ifdef BENDER_WINDOWS
#include <windows.h>
#endif

BENDER_THREAD_LOCAL BenderVM bender_vm;

BenderVM *bender_local_vm(void) {
    return &bender_vm;
}

BenderVM *bender_vm_alloc(void) {
    BenderVM *mem = bender_malloc(sizeof(BenderVM));
    if (NULL == mem) {
        BENDER_OUT_OF_MEMORY;
    }
    return mem;
}

void bender_vm_free(BenderVM *vm) {
    bender_free(vm);
}

void bender_vm_save(BenderVM *into) {
    *into = bender_vm;
}

void bender_vm_load(BenderVM *from) {
    bender_vm = *from;
}

/* Trigger suspension of the Bender vm by trying to
 * exit the interpreter loop when convenient. You can optionally
 * use NULL to interrupt the current VM when convenient */
void bender_interpreter_interrupt(BenderVM *vm) {
    vm = vm ? vm : &bender_vm;
    bender_atomic_inc(&vm->auto_suspend);
}

void bender_interpreter_interrupt_handled(BenderVM *vm) {
    vm = vm ? vm : &bender_vm;
    bender_atomic_dec(&vm->auto_suspend);
}
