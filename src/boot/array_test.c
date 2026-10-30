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

int array_test() {

    int i;
    BenderArray *array1, *array2;

    array1 = bender_array(10);
    array2 = bender_array(0);

    bender_array_push(array1, bender_cstringv("one"));
    bender_array_push(array1, bender_cstringv("two"));
    bender_array_push(array1, bender_cstringv("three"));
    bender_array_push(array1, bender_cstringv("four"));
    bender_array_push(array1, bender_cstringv("five"));
    bender_array_push(array1, bender_cstringv("six"));
    bender_array_push(array1, bender_cstringv("seven"));

    assert(array1->count == 7);
    assert(array1->capacity >= 7);
    assert(bender_equals(array1->data[0], bender_cstringv("one")));

    bender_array_push(array2, bender_cstringv("one"));
    bender_array_push(array2, bender_cstringv("two"));
    bender_array_push(array2, bender_cstringv("three"));
    bender_array_push(array2, bender_cstringv("four"));
    bender_array_push(array2, bender_cstringv("five"));
    bender_array_push(array2, bender_cstringv("six"));
    bender_array_push(array2, bender_cstringv("seven"));

    for (i = 0; i < array2->count; i++) {
        assert(bender_equals(array1->data[i], array2->data[i]));
    }

    bender_array_pop(array1);
    bender_array_pop(array1);

    assert(array1->count == 5);

    return 0;
}
