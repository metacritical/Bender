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
#endif

/* Run a string of code. The return value is a set of error flags, BENDER_DO_ERROR_RUNTIME, BENDER_DO_ERROR_COMPILE, and BENDER_DOR_ERROR_PARSE if
 * any errors were encountered in those phases. More information is printed to stderr. */
int bender_dobytes(BenderTable *env, const uint8_t *bytes, int32_t len, const char *sourcePath, Bender *out) {
    BenderParser *parser;
    int errflags = 0, done = 0;
    int32_t index = 0;
    Bender ret = bender_wrap_nil();
    BenderFiber *fiber = NULL;
    const uint8_t *where = sourcePath ? bender_cstring(sourcePath) : NULL;

    if (where) bender_gcroot(bender_wrap_string(where));
    if (NULL == sourcePath) sourcePath = "<unknown>";
    parser = bender_abstract(&bender_parser_type, sizeof(BenderParser));
    bender_parser_init(parser);
    bender_gcroot(bender_wrap_abstract(parser));

    /* While we haven't seen an error */
    while (!done) {

        /* Evaluate parsed values */
        while (bender_parser_has_more(parser)) {
            Bender form = bender_parser_produce(parser);
            BenderCompileResult cres = bender_compile(form, env, where);
            if (cres.status == BENDER_COMPILE_OK) {
                BenderFunction *f = bender_thunk(cres.funcdef);
                fiber = bender_fiber(f, 64, 0, NULL);
                fiber->env = env;
                BenderSignal status;
                /* M4b-2: quantum-INTERRUPT is transparent here (bare run
                 * must complete) — resume again. Quiesce still unwinds. */
                do {
                    status = bender_continue(fiber, bender_wrap_nil(), &ret);
                } while (status == BENDER_SIGNAL_INTERRUPT && bender_vm.quantum &&
                        !bender_atomic_load_relaxed(&bender_vm.auto_suspend));
                if (status != BENDER_SIGNAL_OK && status != BENDER_SIGNAL_EVENT) {
                    bender_stacktrace_ext(fiber, ret, "");
                    errflags |= BENDER_DO_ERROR_RUNTIME;
                    done = 1;
                }
            } else {
                int32_t line = (int32_t) parser->line;
                int32_t col = (int32_t) parser->column;
                if ((cres.error_mapping.line > 0) &&
                        (cres.error_mapping.column > 0)) {
                    line = cres.error_mapping.line;
                    col = cres.error_mapping.column;
                }
                BenderString ctx = bender_formatc("%s:%d:%d: compile error",
                                                sourcePath, line, col);
                BenderString errstr = bender_formatc("%s: %s",
                                                   (const char *)ctx,
                                                   (const char *)cres.error);
                ret = bender_wrap_string(errstr);
                if (cres.macrofiber) {
                    bender_eprintf("%s", (const char *)ctx);
                    bender_stacktrace_ext(cres.macrofiber, ret, "");
                } else {
                    bender_eprintf("%s\n", (const char *)errstr);
                }
                errflags |= BENDER_DO_ERROR_COMPILE;
                done = 1;
            }
        }

        if (done) break;

        /* Dispatch based on parse state */
        switch (bender_parser_status(parser)) {
            case BENDER_PARSE_DEAD:
                done = 1;
                break;
            case BENDER_PARSE_ERROR: {
                errflags |= BENDER_DO_ERROR_PARSE;
                int32_t line = (int32_t) parser->line;
                int32_t col = (int32_t) parser->column;
                BenderString errstr = bender_formatc("%s:%d:%d: parse error: %s",
                                                   sourcePath, line, col,
                                                   bender_parser_error(parser));
                ret = bender_wrap_string(errstr);
                bender_eprintf("%s\n", (const char *)errstr);
                done = 1;
                break;
            }
            case BENDER_PARSE_ROOT:
            case BENDER_PARSE_PENDING:
                if (index >= len) {
                    bender_parser_eof(parser);
                } else {
                    bender_parser_consume(parser, bytes[index++]);
                }
                break;
        }

    }

    /* Clean up and return errors */
    bender_gcunroot(bender_wrap_abstract(parser));
    if (where) bender_gcunroot(bender_wrap_string(where));
#ifdef BENDER_EV
    /* Enter the event loop if we are not already in it */
    if (bender_vm.stackn == 0) {
        if (fiber) {
            bender_gcroot(bender_wrap_fiber(fiber));
        }
        bender_loop();
        if (fiber) {
            bender_gcunroot(bender_wrap_fiber(fiber));
            if (!errflags)
                ret = fiber->last_value;
        }
    }
#endif
    if (out) *out = ret;
    return errflags;
}

int bender_dostring(BenderTable *env, const char *str, const char *sourcePath, Bender *out) {
    int32_t len = 0;
    while (str[len]) ++len;
    return bender_dobytes(env, (const uint8_t *)str, len, sourcePath, out);
}

/* Run a fiber to completion (use event loop if enabled). Return the status. */
int bender_loop_fiber(BenderFiber *fiber) {
    int status;
#ifdef BENDER_EV
    bender_schedule(fiber, bender_wrap_nil());
    bender_loop();
    status = bender_fiber_status(fiber);
#else
    Bender out;
    status = bender_continue(fiber, bender_wrap_nil(), &out);
    if (status != BENDER_SIGNAL_OK && status != BENDER_SIGNAL_EVENT) {
        bender_stacktrace_ext(fiber, out, "");
    }
#endif
    return status;
}
