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
#include "util.h"
#include "state.h"
#endif

/* Allow for managed buffers that cannot realloc/free their backing memory */
static void bender_buffer_can_realloc(BenderBuffer *buffer) {
    if (buffer->gc.flags & BENDER_BUFFER_FLAG_NO_REALLOC) {
        bender_panic("buffer cannot reallocate foreign memory");
    }
}

/* Initialize a buffer */
static BenderBuffer *bender_buffer_init_impl(BenderBuffer *buffer, int32_t capacity) {
    uint8_t *data = NULL;
    if (capacity < 4) capacity = 4;
    bender_gcpressure(capacity);
    data = array_allocate(sizeof(uint8_t), capacity);
    if (NULL == data) {
        BENDER_OUT_OF_MEMORY;
    }
    buffer->count = 0;
    buffer->capacity = capacity;
    buffer->data = data;
    return buffer;
}

/* Initialize a buffer */
BenderBuffer *bender_buffer_init(BenderBuffer *buffer, int32_t capacity) {
    bender_buffer_init_impl(buffer, capacity);
    buffer->gc.data.next = NULL;
    buffer->gc.flags = BENDER_MEM_DISABLED;
    return buffer;
}

/* Initialize an unmanaged buffer */
BenderBuffer *bender_pointer_buffer_unsafe(void *memory, int32_t capacity, int32_t count) {
    if (count < 0) bender_panic("count < 0");
    if (capacity < count) bender_panic("capacity < count");
    BenderBuffer *buffer = bender_gcalloc(BENDER_MEMORY_BUFFER, sizeof(BenderBuffer));
    buffer->gc.flags |= BENDER_BUFFER_FLAG_NO_REALLOC;
    buffer->capacity = capacity;
    buffer->count = count;
    buffer->data = (uint8_t *) memory;
    return buffer;
}

/* Deinitialize a buffer (free data memory) */
void bender_buffer_deinit(BenderBuffer *buffer) {
    if (!(buffer->gc.flags & BENDER_BUFFER_FLAG_NO_REALLOC)) {
        bender_free(buffer->data);
        buffer->data = NULL;
    }
}

/* Initialize a buffer */
BenderBuffer *bender_buffer(int32_t capacity) {
    BenderBuffer *buffer = bender_gcalloc(BENDER_MEMORY_BUFFER, sizeof(BenderBuffer));
    return bender_buffer_init_impl(buffer, capacity);
}

/* Ensure that the buffer has enough internal capacity */
void bender_buffer_ensure(BenderBuffer *buffer, int32_t capacity, int32_t growth) {
    uint8_t *new_data;
    uint8_t *old = buffer->data;
    if (capacity <= buffer->capacity) return;
    bender_buffer_can_realloc(buffer);
    int64_t big_capacity = ((int64_t) capacity) * growth;
    capacity = big_capacity > INT32_MAX ? INT32_MAX : (int32_t) big_capacity;
    bender_gcpressure(capacity - buffer->capacity);
    new_data = bender_realloc(old, (size_t) capacity * sizeof(uint8_t));
    if (NULL == new_data) {
        BENDER_OUT_OF_MEMORY;
    }
    buffer->data = new_data;
    buffer->capacity = capacity;
}

/* Ensure that the buffer has enough internal capacity */
void bender_buffer_setcount(BenderBuffer *buffer, int32_t count) {
    if (count < 0)
        return;
    if (count > buffer->count) {
        int32_t oldcount = buffer->count;
        bender_buffer_ensure(buffer, count, 1);
        memset(buffer->data + oldcount, 0, count - oldcount);
    }
    buffer->count = count;
}

/* Adds capacity for enough extra bytes to the buffer. Ensures that the
 * next n bytes pushed to the buffer will not cause a reallocation */
void bender_buffer_extra(BenderBuffer *buffer, int32_t n) {
    /* Check for buffer overflow */
    if ((int64_t)n + buffer->count > INT32_MAX) {
        bender_panic("buffer overflow");
    }
    int32_t new_size = buffer->count + n;
    if (new_size > buffer->capacity) {
        bender_buffer_can_realloc(buffer);
        int32_t new_capacity = (new_size > (INT32_MAX / 2)) ? INT32_MAX : (new_size * 2);
        uint8_t *new_data = bender_realloc(buffer->data, new_capacity * sizeof(uint8_t));
        bender_gcpressure(new_capacity - buffer->capacity);
        if (NULL == new_data) {
            BENDER_OUT_OF_MEMORY;
        }
        buffer->data = new_data;
        buffer->capacity = new_capacity;
    }
}

/* Push a cstring to buffer */
void bender_buffer_push_cstring(BenderBuffer *buffer, const char *cstring) {
    int32_t len = (int32_t) strlen(cstring);
    bender_buffer_push_bytes(buffer, (const uint8_t *) cstring, len);
}

/* Push multiple bytes into the buffer */
void bender_buffer_push_bytes(BenderBuffer *buffer, const uint8_t *string, int32_t length) {
    if (0 == length) return;
    bender_buffer_extra(buffer, length);
    memcpy(buffer->data + buffer->count, string, length);
    buffer->count += length;
}

void bender_buffer_push_string(BenderBuffer *buffer, const uint8_t *string) {
    bender_buffer_push_bytes(buffer, string, bender_string_length(string));
}

/* Push a single byte to the buffer */
void bender_buffer_push_u8(BenderBuffer *buffer, uint8_t byte) {
    bender_buffer_extra(buffer, 1);
    buffer->data[buffer->count] = byte;
    buffer->count++;
}

/* Push a 16 bit unsigned integer to the buffer */
void bender_buffer_push_u16(BenderBuffer *buffer, uint16_t x) {
    bender_buffer_extra(buffer, 2);
    buffer->data[buffer->count] = x & 0xFF;
    buffer->data[buffer->count + 1] = (x >> 8) & 0xFF;
    buffer->count += 2;
}

/* Push a 32 bit unsigned integer to the buffer */
void bender_buffer_push_u32(BenderBuffer *buffer, uint32_t x) {
    bender_buffer_extra(buffer, 4);
    buffer->data[buffer->count] = x & 0xFF;
    buffer->data[buffer->count + 1] = (x >> 8) & 0xFF;
    buffer->data[buffer->count + 2] = (x >> 16) & 0xFF;
    buffer->data[buffer->count + 3] = (x >> 24) & 0xFF;
    buffer->count += 4;
}

/* Push a 64 bit unsigned integer to the buffer */
void bender_buffer_push_u64(BenderBuffer *buffer, uint64_t x) {
    bender_buffer_extra(buffer, 8);
    buffer->data[buffer->count] = x & 0xFF;
    buffer->data[buffer->count + 1] = (x >> 8) & 0xFF;
    buffer->data[buffer->count + 2] = (x >> 16) & 0xFF;
    buffer->data[buffer->count + 3] = (x >> 24) & 0xFF;
    buffer->data[buffer->count + 4] = (x >> 32) & 0xFF;
    buffer->data[buffer->count + 5] = (x >> 40) & 0xFF;
    buffer->data[buffer->count + 6] = (x >> 48) & 0xFF;
    buffer->data[buffer->count + 7] = (x >> 56) & 0xFF;
    buffer->count += 8;
}

/* C functions */

BENDER_CORE_FN(cfun_buffer_new,
              "(buffer/new capacity)",
              "Creates a new, empty buffer with enough backing memory for `capacity` bytes. "
              "Returns a new buffer of length 0.") {
    bender_fixarity(argc, 1);
    int32_t cap = bender_getinteger(argv, 0);
    BenderBuffer *buffer = bender_buffer(cap);
    return bender_wrap_buffer(buffer);
}

BENDER_CORE_FN(cfun_buffer_new_filled,
              "(buffer/new-filled n &opt byte)",
              "Creates a new buffer of length `n` filled up with `byte`. "
              "By default, `byte` is 0. Returns the new buffer.") {
    bender_arity(argc, 1, 2);
    int32_t count = bender_getinteger(argv, 0);
    if (count < 0) count = 0;
    int32_t byte = 0;
    if (argc == 2) {
        byte = bender_getinteger(argv, 1) & 0xFF;
    }
    BenderBuffer *buffer = bender_buffer(count);
    if (buffer->data && count > 0)
        memset(buffer->data, byte, count);
    buffer->count = count;
    return bender_wrap_buffer(buffer);
}

BENDER_CORE_FN(cfun_buffer_frombytes,
              "(buffer/from-bytes & byte-vals)",
              "Creates a buffer from integer parameters with byte values. All integers "
              "will be coerced to the range of 1 byte 0-255.") {
    int32_t i;
    BenderBuffer *buffer = bender_buffer(argc);
    for (i = 0; i < argc; i++) {
        int32_t c = bender_getinteger(argv, i);
        buffer->data[i] = c & 0xFF;
    }
    buffer->count = argc;
    return bender_wrap_buffer(buffer);
}

BENDER_CORE_FN(cfun_buffer_fill,
              "(buffer/fill buf &opt byte)",
              "Fill up a buffer, `buf`, with bytes, defaulting to 0s. Does "
              "not change `buf`'s length. Returns `buf`.") {
    bender_arity(argc, 1, 2);
    BenderBuffer *buffer = bender_getbuffer(argv, 0);
    int32_t byte = 0;
    if (argc == 2) {
        byte = bender_getinteger(argv, 1) & 0xFF;
    }
    if (buffer->count) {
        memset(buffer->data, byte, buffer->count);
    }
    return argv[0];
}

BENDER_CORE_FN(cfun_buffer_trim,
              "(buffer/trim buf)",
              "Set the backing capacity of a buffer, `buf`, to its current "
              "length. Returns `buf`.") {
    bender_fixarity(argc, 1);
    BenderBuffer *buffer = bender_getbuffer(argv, 0);
    bender_buffer_can_realloc(buffer);
    if (buffer->count < buffer->capacity) {
        int32_t newcap = buffer->count > 4 ? buffer->count : 4;
        uint8_t *newData = bender_realloc(buffer->data, newcap);
        if (NULL == newData) {
            BENDER_OUT_OF_MEMORY;
        }
        buffer->data = newData;
        buffer->capacity = newcap;
    }
    return argv[0];
}

BENDER_CORE_FN(cfun_buffer_u8,
              "(buffer/push-byte buf & xs)",
              "Append bytes to a buffer, `buf`. Returns `buf`. Expands "
              "`buf` as needed. Throws an error if size limit is exceeded.") {
    int32_t i;
    bender_arity(argc, 1, -1);
    BenderBuffer *buffer = bender_getbuffer(argv, 0);
    for (i = 1; i < argc; i++) {
        bender_buffer_push_u8(buffer, (uint8_t)(bender_getinteger(argv, i) & 0xFF));
    }
    return argv[0];
}

BENDER_CORE_FN(cfun_buffer_word,
              "(buffer/push-word buf & xs)",
              "Append machine words to a buffer, `buf`. The 4 bytes of the "
              "integer are appended in two's complement, little endian "
              "order, unsigned for all x. Returns `buf`. Expands `buf` as "
              "needed. Throws an error if size limit is exceeded.") {
    int32_t i;
    bender_arity(argc, 1, -1);
    BenderBuffer *buffer = bender_getbuffer(argv, 0);
    for (i = 1; i < argc; i++) {
        double number = bender_getnumber(argv, i);
        uint32_t word = (uint32_t) number;
        if (word != number)
            bender_panicf("cannot convert %v to machine word", argv[i]);
        bender_buffer_push_u32(buffer, word);
    }
    return argv[0];
}

BENDER_CORE_FN(cfun_buffer_chars,
              "(buffer/push-string buf & xs)",
              "Push bytes type values onto the end of a buffer, `buf`. "
              "Each `xs` should be a bytes type. Returns `buf`. Expands "
              "`buf` as needed. Throws an error if size limit is exceeded.") {
    int32_t i;
    bender_arity(argc, 1, -1);
    BenderBuffer *buffer = bender_getbuffer(argv, 0);
    for (i = 1; i < argc; i++) {
        BenderByteView view = bender_getbytes(argv, i);
        if (view.bytes == buffer->data) {
            bender_buffer_ensure(buffer, buffer->count + view.len, 2);
            view.bytes = buffer->data;
        }
        bender_buffer_push_bytes(buffer, view.bytes, view.len);
    }
    return argv[0];
}

/* Native-executor fast path for the exact two-argument
 * (buffer/push-string buf bytes) call. `ready` performs no mutation and
 * returns 0 for every case that could grow/reallocate, alias the target,
 * panic on a non-bytes value, or use a different callee. `commit` then
 * does only the capacity-checked copy, so it cannot allocate, collect, or
 * signal. The executor polls between the two calls. */
BENDER_API int bender_native_buffer_push_string_ready(
    Bender callee, int32_t argc, const Bender *argv,
    const uint8_t **bytes, int32_t *len) {
    BenderBuffer *buffer;
    const uint8_t *view_bytes;
    int32_t view_len;
    if (argc != 2) return 0;
    if (!bender_checktype(callee, BENDER_CFUNCTION)) return 0;
    if (bender_unwrap_cfunction(callee) != cfun_buffer_chars) return 0;
    if (!bender_checktype(argv[0], BENDER_BUFFER)) return 0;
    buffer = bender_unwrap_buffer(argv[0]);
    if (!bender_bytes_view(argv[1], &view_bytes, &view_len)) return 0;
    if (view_len < 0) return 0;
    if (view_bytes == buffer->data) return 0;
    if ((int64_t)buffer->count + view_len > buffer->capacity) return 0;
    *bytes = view_bytes;
    *len = view_len;
    return 1;
}

BENDER_API Bender bender_native_buffer_push_string_commit(
    int32_t argc, const Bender *argv, const uint8_t *bytes, int32_t len) {
    BenderBuffer *buffer = bender_unwrap_buffer(argv[0]);
    (void)argc;
    if (len > 0) memcpy(buffer->data + buffer->count, bytes, (size_t)len);
    buffer->count += len;
    return argv[0];
}

static int should_reverse_bytes(const Bender *argv, int32_t argc) {
    BenderKeyword order_kw = bender_getkeyword(argv, argc);
    if (!bender_cstrcmp(order_kw, "le")) {
#if BENDER_BIG_ENDIAN
        return 1;
#endif
    } else if (!bender_cstrcmp(order_kw, "be")) {
#if BENDER_LITTLE_ENDIAN
        return 1;
#endif
    } else if (!bender_cstrcmp(order_kw, "native")) {
        return 0;
    } else {
        bender_panicf("expected endianness :le, :be or :native, got %v", argv[1]);
    }
    return 0;
}

static void reverse_u32(uint8_t bytes[4]) {
    uint8_t temp;
    temp = bytes[3];
    bytes[3] = bytes[0];
    bytes[0] = temp;
    temp = bytes[2];
    bytes[2] = bytes[1];
    bytes[1] = temp;
}

static void reverse_u64(uint8_t bytes[8]) {
    uint8_t temp;
    temp = bytes[7];
    bytes[7] = bytes[0];
    bytes[0] = temp;
    temp = bytes[6];
    bytes[6] = bytes[1];
    bytes[1] = temp;
    temp = bytes[5];
    bytes[5] = bytes[2];
    bytes[2] = temp;
    temp = bytes[4];
    bytes[4] = bytes[3];
    bytes[3] = temp;
}

BENDER_CORE_FN(cfun_buffer_push_uint16,
              "(buffer/push-uint16 buf order data)",
              "Push a 16 bit unsigned integer, `data`, onto the end of a "
              "buffer, `buf`. Returns `buf`. Expands `buf` as needed. "
              "Throws an error if size limit is exceeded.") {
    bender_fixarity(argc, 3);
    BenderBuffer *buffer = bender_getbuffer(argv, 0);
    int reverse = should_reverse_bytes(argv, 1);
    uint16_t data = bender_getuinteger16(argv, 2);
    uint8_t bytes[sizeof(data)];
    memcpy(bytes, &data, sizeof(bytes));
    if (reverse) {
        uint8_t temp = bytes[1];
        bytes[1] = bytes[0];
        bytes[0] = temp;
    }
    bender_buffer_push_bytes(buffer, bytes, sizeof(bytes));
    return argv[0];
}

BENDER_CORE_FN(cfun_buffer_push_uint32,
              "(buffer/push-uint32 buf order data)",
              "Push a 32 bit unsigned integer, `data` onto the end of a "
              "buffer, `buf`. Returns `buf`. Expands `buf` as needed. "
              "Throws an error if size limit is exceeded.") {
    bender_fixarity(argc, 3);
    BenderBuffer *buffer = bender_getbuffer(argv, 0);
    int reverse = should_reverse_bytes(argv, 1);
    uint32_t data = bender_getuinteger(argv, 2);
    uint8_t bytes[sizeof(data)];
    memcpy(bytes, &data, sizeof(bytes));
    if (reverse)
        reverse_u32(bytes);
    bender_buffer_push_bytes(buffer, bytes, sizeof(bytes));
    return argv[0];
}

BENDER_CORE_FN(cfun_buffer_push_uint64,
              "(buffer/push-uint64 buf order data)",
              "Push a 64 bit unsigned integer, `data`, onto the end of a "
              "buffer, `buf`. Returns `buf`. Expands `buf` as needed. "
              "Throws an error if size limit is exceeded.") {
    bender_fixarity(argc, 3);
    BenderBuffer *buffer = bender_getbuffer(argv, 0);
    int reverse = should_reverse_bytes(argv, 1);
    uint64_t data = bender_getuinteger64(argv, 2);
    uint8_t bytes[sizeof(data)];
    memcpy(bytes, &data, sizeof(bytes));
    if (reverse)
        reverse_u64(bytes);
    bender_buffer_push_bytes(buffer, bytes, sizeof(bytes));
    return argv[0];
}

BENDER_CORE_FN(cfun_buffer_push_float32,
              "(buffer/push-float32 buf order data)",
              "Push the underlying bytes of a 32 bit float, `data`, onto "
              "the end of the buffer, `buf`. Returns `buf`. Expands `buf` "
              "as needed. Throws an error if size limit is exceeded.") {
    bender_fixarity(argc, 3);
    BenderBuffer *buffer = bender_getbuffer(argv, 0);
    int reverse = should_reverse_bytes(argv, 1);
    float data = (float) bender_getnumber(argv, 2);
    uint8_t bytes[sizeof(data)];
    memcpy(bytes, &data, sizeof(bytes));
    if (reverse)
        reverse_u32(bytes);
    bender_buffer_push_bytes(buffer, bytes, sizeof(bytes));
    return argv[0];
}

BENDER_CORE_FN(cfun_buffer_push_float64,
              "(buffer/push-float64 buf order data)",
              "Push the underlying bytes of a 64 bit float, `data`, onto "
              "the end of the buffer, `buf`. Returns `buf`. Expands `buf` "
              "as needed. Throws an error if size limit is exceeded.") {
    bender_fixarity(argc, 3);
    BenderBuffer *buffer = bender_getbuffer(argv, 0);
    int reverse = should_reverse_bytes(argv, 1);
    double data = bender_getnumber(argv, 2);
    uint8_t bytes[sizeof(data)];
    memcpy(bytes, &data, sizeof(bytes));
    if (reverse)
        reverse_u64(bytes);
    bender_buffer_push_bytes(buffer, bytes, sizeof(bytes));
    return argv[0];
}

static void buffer_push_impl(BenderBuffer *buffer, Bender *argv, int32_t argc_offset, int32_t argc) {
    for (int32_t i = argc_offset; i < argc; i++) {
        if (bender_checktype(argv[i], BENDER_NUMBER)) {
            bender_buffer_push_u8(buffer, (uint8_t)(bender_getinteger(argv, i) & 0xFF));
        } else {
            BenderByteView view = bender_getbytes(argv, i);
            if (view.bytes == buffer->data) {
                bender_buffer_ensure(buffer, buffer->count + view.len, 2);
                view.bytes = buffer->data;
            }
            bender_buffer_push_bytes(buffer, view.bytes, view.len);
        }
    }
}

BENDER_CORE_FN(cfun_buffer_push_at,
              "(buffer/push-at buf index & xs)",
              "Same as `buffer/push`, but copies the new data into `buf` "
              "at index `index`.") {
    bender_arity(argc, 2, -1);
    BenderBuffer *buffer = bender_getbuffer(argv, 0);
    int32_t index = bender_getinteger(argv, 1);
    int32_t old_count = buffer->count;
    if (index < 0 || index > old_count) {
        bender_panicf("index out of range [0, %d)", old_count);
    }
    buffer->count = index;
    buffer_push_impl(buffer, argv, 2, argc);
    if (buffer->count < old_count) {
        buffer->count = old_count;
    }
    return argv[0];
}

BENDER_CORE_FN(cfun_buffer_push,
              "(buffer/push buf & xs)",
              "Push both single bytes and bytes type values to a buffer, "
              "`buf`. Values in `xs` that are bytes are pushed to `buf` as "
              "bytes, while bytes type values have their individual bytes "
              "pushed. Thus, this function behaves like both "
              "`buffer/push-byte` and `buffer/push-string`. Returns `buf`. "
              "Expands `buf` as needed. Throws an error if size limit is "
              "exceeded.") {
    bender_arity(argc, 1, -1);
    BenderBuffer *buffer = bender_getbuffer(argv, 0);
    buffer_push_impl(buffer, argv, 1, argc);
    return argv[0];
}

BENDER_CORE_FN(cfun_buffer_clear,
              "(buffer/clear buf)",
              "Sets the size of a buffer, `buf`, to 0 and empties it. `buf` "
              "retains its memory so it can be efficiently refilled. Returns "
              "`buf`.") {
    bender_fixarity(argc, 1);
    BenderBuffer *buffer = bender_getbuffer(argv, 0);
    buffer->count = 0;
    return argv[0];
}

BENDER_CORE_FN(cfun_buffer_popn,
              "(buffer/popn buf n)",
              "Removes the last `n` bytes from a buffer, `buf`. Returns "
              "`buf`.") {
    bender_fixarity(argc, 2);
    BenderBuffer *buffer = bender_getbuffer(argv, 0);
    int32_t n = bender_getinteger(argv, 1);
    if (n < 0) bender_panic("n must be non-negative");
    if (buffer->count < n) {
        buffer->count = 0;
    } else {
        buffer->count -= n;
    }
    return argv[0];
}

BENDER_CORE_FN(cfun_buffer_slice,
              "(buffer/slice bytes &opt start end)",
              "Same as `string/slice`, but returns a buffer.") {
    BenderByteView view = bender_getbytes(argv, 0);
    BenderRange range = bender_getslice(argc, argv);
    BenderBuffer *buffer = bender_buffer(range.end - range.start);
    if (buffer->data)
        memcpy(buffer->data, view.bytes + range.start, range.end - range.start);
    buffer->count = range.end - range.start;
    return bender_wrap_buffer(buffer);
}

static void bitloc(int32_t argc, Bender *argv, BenderBuffer **b, int32_t *index, int *bit) {
    bender_fixarity(argc, 2);
    BenderBuffer *buffer = bender_getbuffer(argv, 0);
    double x = bender_getnumber(argv, 1);
    int64_t bitindex = (int64_t) x;
    int64_t byteindex = bitindex >> 3;
    int which_bit = bitindex & 7;
    if (bitindex != x || bitindex < 0 || byteindex >= buffer->count)
        bender_panicf("invalid bit index %v", argv[1]);
    *b = buffer;
    *index = (int32_t) byteindex;
    *bit = which_bit;
}

BENDER_CORE_FN(cfun_buffer_bitset,
              "(buffer/bit-set buf bit-index)",
              "Sets the bit at `bit-index` in a buffer, `buf`. Returns "
              "`buf`.") {
    int bit;
    int32_t index;
    BenderBuffer *buffer;
    bitloc(argc, argv, &buffer, &index, &bit);
    buffer->data[index] |= 1 << bit;
    return argv[0];
}

BENDER_CORE_FN(cfun_buffer_bitclear,
              "(buffer/bit-clear buf bit-index)",
              "Clears the bit at `bit-index` in a buffer, `buf`. Returns "
              "`buf`.") {
    int bit;
    int32_t index;
    BenderBuffer *buffer;
    bitloc(argc, argv, &buffer, &index, &bit);
    buffer->data[index] &= ~(1 << bit);
    return argv[0];
}

BENDER_CORE_FN(cfun_buffer_bitget,
              "(buffer/bit buf bit-index)",
              "Gets the bit at `bit-index` in a buffer, `buf`. Returns true "
              "if the bit is set, false if not.") {
    int bit;
    int32_t index;
    BenderBuffer *buffer;
    bitloc(argc, argv, &buffer, &index, &bit);
    return bender_wrap_boolean(buffer->data[index] & (1 << bit));
}

BENDER_CORE_FN(cfun_buffer_bittoggle,
              "(buffer/bit-toggle buf bit-index)",
              "Toggles the bit at `bit-index` in a buffer, `buf`. Returns "
              "`buf`.") {
    int bit;
    int32_t index;
    BenderBuffer *buffer;
    bitloc(argc, argv, &buffer, &index, &bit);
    buffer->data[index] ^= (1 << bit);
    return argv[0];
}

BENDER_CORE_FN(cfun_buffer_blit,
              "(buffer/blit dest src &opt dest-start src-start src-end)",
              "Insert the contents of `src` into `dest`. Can optionally take indices that "
              "indicate which part of `src` to copy into which part of `dest`. Indices can be "
              "negative in order to index from the end of `src` or `dest`. Returns `dest`.") {
    bender_arity(argc, 2, 5);
    BenderBuffer *dest = bender_getbuffer(argv, 0);
    BenderByteView src = bender_getbytes(argv, 1);
    int same_buf = src.bytes == dest->data;
    int32_t offset_dest = 0;
    int32_t offset_src = 0;
    if (argc > 2 && !bender_checktype(argv[2], BENDER_NIL))
        offset_dest = bender_gethalfrange(argv, 2, dest->count, "dest-start");
    if (argc > 3 && !bender_checktype(argv[3], BENDER_NIL))
        offset_src = bender_gethalfrange(argv, 3, src.len, "src-start");
    int32_t length_src;
    if (argc > 4) {
        int32_t src_end = src.len;
        if (!bender_checktype(argv[4], BENDER_NIL))
            src_end = bender_gethalfrange(argv, 4, src.len, "src-end");
        length_src = src_end - offset_src;
        if (length_src < 0) length_src = 0;
    } else {
        length_src = src.len - offset_src;
    }
    int64_t last = (int64_t) offset_dest + length_src;
    if (last > INT32_MAX)
        bender_panic("buffer blit out of range");
    int32_t last32 = (int32_t) last;
    bender_buffer_ensure(dest, last32, 2);
    if (last32 > dest->count) dest->count = last32;
    if (length_src) {
        if (same_buf) {
            /* bender_buffer_ensure may have invalidated src */
            src.bytes = dest->data;
            memmove(dest->data + offset_dest, src.bytes + offset_src, length_src);
        } else {
            memcpy(dest->data + offset_dest, src.bytes + offset_src, length_src);
        }
    }
    return argv[0];
}

BENDER_CORE_FN(cfun_buffer_format,
              "(buffer/format buf fmt & args)",
              "Formats `fmt`, similar to `sprintf()`, with `args` into a "
              "buffer `buf`. Returns buf.") {
    bender_arity(argc, 2, -1);
    BenderBuffer *buffer = bender_getbuffer(argv, 0);
    const char *strfrmt = (const char *) bender_getstring(argv, 1);
    bender_buffer_format(buffer, strfrmt, 1, argc, argv);
    return argv[0];
}

BENDER_CORE_FN(cfun_buffer_format_at,
              "(buffer/format-at buf at fmt & args)",
              "Formats `fmt`, similar to `sprintf()`, with `args` into a "
              "buffer `buf`, starting at index `at`. Returns buf.") {
    bender_arity(argc, 2, -1);
    BenderBuffer *buffer = bender_getbuffer(argv, 0);
    int32_t at = bender_getinteger(argv, 1);
    if (at < 0) {
        at += buffer->count + 1;
    }
    if (at > buffer->count || at < 0) bender_panicf("expected index at to be in range [0, %d), got %d", buffer->count, at);
    int32_t oldcount = buffer->count;
    buffer->count = at;
    const char *strfrmt = (const char *) bender_getstring(argv, 2);
    bender_buffer_format(buffer, strfrmt, 2, argc, argv);
    if (buffer->count < oldcount) {
        buffer->count = oldcount;
    }
    return argv[0];
}

void bender_lib_buffer(BenderTable *env) {
    BenderRegExt buffer_cfuns[] = {
        BENDER_CORE_REG("buffer/new", cfun_buffer_new),
        BENDER_CORE_REG("buffer/new-filled", cfun_buffer_new_filled),
        BENDER_CORE_REG("buffer/from-bytes", cfun_buffer_frombytes),
        BENDER_CORE_REG("buffer/fill", cfun_buffer_fill),
        BENDER_CORE_REG("buffer/trim", cfun_buffer_trim),
        BENDER_CORE_REG("buffer/push-byte", cfun_buffer_u8),
        BENDER_CORE_REG("buffer/push-word", cfun_buffer_word),
        BENDER_CORE_REG("buffer/push-string", cfun_buffer_chars),
        BENDER_CORE_REG("buffer/push-uint16", cfun_buffer_push_uint16),
        BENDER_CORE_REG("buffer/push-uint32", cfun_buffer_push_uint32),
        BENDER_CORE_REG("buffer/push-uint64", cfun_buffer_push_uint64),
        BENDER_CORE_REG("buffer/push-float32", cfun_buffer_push_float32),
        BENDER_CORE_REG("buffer/push-float64", cfun_buffer_push_float64),
        BENDER_CORE_REG("buffer/push", cfun_buffer_push),
        BENDER_CORE_REG("buffer/push-at", cfun_buffer_push_at),
        BENDER_CORE_REG("buffer/popn", cfun_buffer_popn),
        BENDER_CORE_REG("buffer/clear", cfun_buffer_clear),
        BENDER_CORE_REG("buffer/slice", cfun_buffer_slice),
        BENDER_CORE_REG("buffer/bit-set", cfun_buffer_bitset),
        BENDER_CORE_REG("buffer/bit-clear", cfun_buffer_bitclear),
        BENDER_CORE_REG("buffer/bit", cfun_buffer_bitget),
        BENDER_CORE_REG("buffer/bit-toggle", cfun_buffer_bittoggle),
        BENDER_CORE_REG("buffer/blit", cfun_buffer_blit),
        BENDER_CORE_REG("buffer/format", cfun_buffer_format),
        BENDER_CORE_REG("buffer/format-at", cfun_buffer_format_at),
        BENDER_REG_END
    };
    bender_core_cfuns_ext(env, NULL, buffer_cfuns);
}
