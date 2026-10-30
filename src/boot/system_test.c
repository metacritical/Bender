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

#include <bender.h>
#include <assert.h>
#include <string.h>
#include <math.h>

#include "tests.h"

int system_test() {

#ifdef BENDER_32
    assert(sizeof(void *) == 4);
#else
    assert(sizeof(void *) == 8);
#endif

    /* Check the version defines are self consistent */
    char version_combined[256];
    snprintf(version_combined, sizeof(version_combined), "%d.%d.%d%s", BENDER_VERSION_MAJOR, BENDER_VERSION_MINOR, BENDER_VERSION_PATCH, BENDER_VERSION_EXTRA);
    assert(!strcmp(BENDER_VERSION, version_combined));

    /* Reflexive testing and nanbox testing */
    assert(bender_equals(bender_wrap_nil(), bender_wrap_nil()));
    assert(bender_equals(bender_wrap_false(), bender_wrap_false()));
    assert(bender_equals(bender_wrap_true(), bender_wrap_true()));
    assert(bender_equals(bender_wrap_integer(1), bender_wrap_integer(1)));
    assert(bender_equals(bender_wrap_integer(INT32_MAX), bender_wrap_integer(INT32_MAX)));
    assert(bender_equals(bender_wrap_integer(-2), bender_wrap_integer(-2)));
    assert(bender_equals(bender_wrap_integer(INT32_MIN), bender_wrap_integer(INT32_MIN)));
    assert(bender_equals(bender_wrap_number(1.4), bender_wrap_number(1.4)));
    assert(bender_equals(bender_wrap_number(3.14159265), bender_wrap_number(3.14159265)));
#ifdef NAN
#ifdef BENDER_PLAN9
    // Plan 9 traps NaNs by default; disable that.
    setfcr(0);
#endif
    assert(bender_checktype(bender_wrap_number(NAN), BENDER_NUMBER));
#else
    assert(bender_checktype(bender_wrap_number(0.0 / 0.0), BENDER_NUMBER));
#endif

    assert(NULL != &bender_wrap_nil);

    assert(bender_equals(bender_cstringv("a string."), bender_cstringv("a string.")));
    assert(bender_equals(bender_csymbolv("sym"), bender_csymbolv("sym")));

    Bender *t1 = bender_tuple_begin(3);
    t1[0] = bender_wrap_nil();
    t1[1] = bender_wrap_integer(4);
    t1[2] = bender_cstringv("hi");
    Bender tuple1 = bender_wrap_tuple(bender_tuple_end(t1));

    Bender *t2 = bender_tuple_begin(3);
    t2[0] = bender_wrap_nil();
    t2[1] = bender_wrap_integer(4);
    t2[2] = bender_cstringv("hi");
    Bender tuple2 = bender_wrap_tuple(bender_tuple_end(t2));

    assert(bender_equals(tuple1, tuple2));

    /* System test for GC bug #1888 */
    BenderTable *env = bender_core_env(NULL);
    bender_setdyn("probe", bender_wrap_integer(42));
    assert(!bender_dostring(env, "(gccollect)", "gc", NULL));
    Bender value = bender_dyn("probe");
    assert(bender_checktype(value, BENDER_NUMBER));
    assert(bender_unwrap_integer(value) == 42);

    return 0;
}
