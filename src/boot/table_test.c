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

#include "tests.h"

int table_test() {

    BenderTable *t1, *t2;

    t1 = bender_table(10);
    t2 = bender_table(0);

    bender_table_put(t1, bender_cstringv("hello"), bender_wrap_integer(2));
    bender_table_put(t1, bender_cstringv("akey"), bender_wrap_integer(5));
    bender_table_put(t1, bender_cstringv("box"), bender_wrap_boolean(0));
    bender_table_put(t1, bender_cstringv("square"), bender_cstringv("avalue"));

    assert(t1->count == 4);
    assert(t1->capacity >= t1->count);

    assert(bender_equals(bender_table_get(t1, bender_cstringv("hello")), bender_wrap_integer(2)));
    assert(bender_equals(bender_table_get(t1, bender_cstringv("akey")), bender_wrap_integer(5)));
    assert(bender_equals(bender_table_get(t1, bender_cstringv("box")), bender_wrap_boolean(0)));
    assert(bender_equals(bender_table_get(t1, bender_cstringv("square")), bender_cstringv("avalue")));

    bender_table_remove(t1, bender_cstringv("hello"));
    bender_table_put(t1, bender_cstringv("box"), bender_wrap_nil());

    assert(t1->count == 2);

    assert(bender_equals(bender_table_get(t1, bender_cstringv("hello")), bender_wrap_nil()));
    assert(bender_equals(bender_table_get(t1, bender_cstringv("box")), bender_wrap_nil()));

    bender_table_put(t2, bender_csymbolv("t2key1"), bender_wrap_integer(10));
    bender_table_put(t2, bender_csymbolv("t2key2"), bender_wrap_integer(100));
    bender_table_put(t2, bender_csymbolv("some key "), bender_wrap_integer(-2));
    bender_table_put(t2, bender_csymbolv("a thing"), bender_wrap_integer(10));

    assert(bender_equals(bender_table_get(t2, bender_csymbolv("t2key1")), bender_wrap_integer(10)));
    assert(bender_equals(bender_table_get(t2, bender_csymbolv("t2key2")), bender_wrap_integer(100)));

    assert(t2->count == 4);
    assert(bender_equals(bender_table_remove(t2, bender_csymbolv("t2key1")), bender_wrap_integer(10)));
    assert(t2->count == 3);
    assert(bender_equals(bender_table_remove(t2, bender_csymbolv("t2key2")), bender_wrap_integer(100)));
    assert(t2->count == 2);

    return 0;
}
