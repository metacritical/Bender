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

#include <math.h>

static int bender_rng_get(void *p, Bender key, Bender *out);
static Bender bender_rng_next(void *p, Bender key);

static void bender_rng_marshal(void *p, BenderMarshalContext *ctx) {
    BenderRNG *rng = (BenderRNG *)p;
    bender_marshal_abstract(ctx, p);
    bender_marshal_int(ctx, (int32_t) rng->a);
    bender_marshal_int(ctx, (int32_t) rng->b);
    bender_marshal_int(ctx, (int32_t) rng->c);
    bender_marshal_int(ctx, (int32_t) rng->d);
    bender_marshal_int(ctx, (int32_t) rng->counter);
}

static void *bender_rng_unmarshal(BenderMarshalContext *ctx) {
    BenderRNG *rng = bender_unmarshal_abstract(ctx, sizeof(BenderRNG));
    rng->a = (uint32_t) bender_unmarshal_int(ctx);
    rng->b = (uint32_t) bender_unmarshal_int(ctx);
    rng->c = (uint32_t) bender_unmarshal_int(ctx);
    rng->d = (uint32_t) bender_unmarshal_int(ctx);
    rng->counter = (uint32_t) bender_unmarshal_int(ctx);
    return rng;
}

const BenderAbstractType bender_rng_type = {
    "core/rng",
    NULL,
    NULL,
    bender_rng_get,
    NULL,
    bender_rng_marshal,
    bender_rng_unmarshal,
    NULL, /* tostring */
    NULL, /* compare */
    NULL, /* hash */
    bender_rng_next,
    BENDER_ATEND_NEXT
};

BenderRNG *bender_default_rng(void) {
    return &bender_vm.rng;
}

void bender_rng_seed(BenderRNG *rng, uint32_t seed) {
    rng->a = seed;
    rng->b = 0x97654321u;
    rng->c = 123871873u;
    rng->d = 0xf23f56c8u;
    rng->counter = 0u;
    /* First several numbers aren't that random. */
    for (int i = 0; i < 16; i++) bender_rng_u32(rng);
}

void bender_rng_longseed(BenderRNG *rng, const uint8_t *bytes, int32_t len) {
    uint8_t state[16] = {0};
    for (int32_t i = 0; i < len; i++)
        state[i & 0xF] ^= bytes[i];
    rng->a = state[0] + ((uint32_t) state[1] << 8) + ((uint32_t) state[2] << 16) + ((uint32_t) state[3] << 24);
    rng->b = state[4] + ((uint32_t) state[5] << 8) + ((uint32_t) state[6] << 16) + ((uint32_t) state[7] << 24);
    rng->c = state[8] + ((uint32_t) state[9] << 8) + ((uint32_t) state[10] << 16) + ((uint32_t) state[11] << 24);
    rng->d = state[12] + ((uint32_t) state[13] << 8) + ((uint32_t) state[14] << 16) + ((uint32_t) state[15] << 24);
    rng->counter = 0u;
    /* a, b, c, d can't all be 0 */
    if (rng->a == 0) rng->a = 1u;
    for (int i = 0; i < 16; i++) bender_rng_u32(rng);
}

uint32_t bender_rng_u32(BenderRNG *rng) {
    /* Algorithm "xorwow" from p. 5 of Marsaglia, "Xorshift RNGs" */
    uint32_t t = rng->d;
    uint32_t const s = rng->a;
    rng->d = rng->c;
    rng->c = rng->b;
    rng->b = s;
    t ^= t >> 2;
    t ^= t << 1;
    t ^= s ^ (s << 4);
    rng->a = t;
    rng->counter += 362437;
    return t + rng->counter;
}

double bender_rng_double(BenderRNG *rng) {
    uint32_t hi = bender_rng_u32(rng);
    uint32_t lo = bender_rng_u32(rng);
    uint64_t big = (uint64_t)(lo) | (((uint64_t) hi) << 32);
    return ldexp((double)(big >> (64 - 52)), -52);
}

BENDER_CORE_FN(cfun_rng_make,
              "(math/rng &opt seed)",
              "Creates a Pseudo-Random number generator, with an optional seed. "
              "The seed should be an unsigned 32 bit integer or a buffer. "
              "Do not use this for cryptography. Returns a core/rng abstract type."
             ) {
    bender_arity(argc, 0, 1);
    BenderRNG *rng = bender_abstract(&bender_rng_type, sizeof(BenderRNG));
    if (argc == 1) {
        if (bender_checkint(argv[0])) {
            uint32_t seed = (uint32_t)(bender_getinteger(argv, 0));
            bender_rng_seed(rng, seed);
        } else {
            BenderByteView bytes = bender_getbytes(argv, 0);
            bender_rng_longseed(rng, bytes.bytes, bytes.len);
        }
    } else {
        bender_rng_seed(rng, 0);
    }
    return bender_wrap_abstract(rng);
}

BENDER_CORE_FN(cfun_rng_uniform,
              "(math/rng-uniform rng)",
              "Extract a random number in the range [0, 1) from the RNG."
             ) {
    bender_fixarity(argc, 1);
    BenderRNG *rng = bender_getabstract(argv, 0, &bender_rng_type);
    return bender_wrap_number(bender_rng_double(rng));
}

BENDER_CORE_FN(cfun_rng_int,
              "(math/rng-int rng &opt lim)",
              "Get a random integer in the range [0, `lim`) for a positive "
              "integer `lim` using the RNG `rng`. If `lim` not given, the "
              "default is 2^31 - 1."
             ) {
    bender_arity(argc, 1, 2);
    BenderRNG *rng = bender_getabstract(argv, 0, &bender_rng_type);
    if (argc == 1) {
        uint32_t word = bender_rng_u32(rng) >> 1;
        return bender_wrap_integer(word);
    } else {
        int32_t max = bender_optnat(argv, argc, 1, INT32_MAX);
        if (max == 0) return bender_wrap_number(0.0);
        uint32_t modulo = (uint32_t) max;
        uint32_t maxgen = INT32_MAX;
        uint32_t maxword = maxgen - (maxgen % modulo);
        uint32_t word;
        do {
            word = bender_rng_u32(rng) >> 1;
        } while (word > maxword);
        return bender_wrap_integer(word % modulo);
    }
}

static void rng_get_4bytes(BenderRNG *rng, uint8_t *buf) {
    uint32_t word = bender_rng_u32(rng);
    buf[0] = word & 0xFF;
    buf[1] = (word >> 8) & 0xFF;
    buf[2] = (word >> 16) & 0xFF;
    buf[3] = (word >> 24) & 0xFF;
}

BENDER_CORE_FN(cfun_rng_buffer,
              "(math/rng-buffer rng n &opt buf)",
              "Get n random bytes and put them in a buffer. Creates a new buffer if no buffer is "
              "provided, otherwise appends to the given buffer. Returns the buffer."
             ) {
    bender_arity(argc, 2, 3);
    BenderRNG *rng = bender_getabstract(argv, 0, &bender_rng_type);
    int32_t n = bender_getnat(argv, 1);
    BenderBuffer *buffer = bender_optbuffer(argv, argc, 2, n);

    /* Split into first part (that is divisible by 4), and rest */
    int32_t first_part = n & ~3;
    int32_t second_part = n - first_part;

    /* Get first part in chunks of 4 bytes */
    bender_buffer_extra(buffer, n);
    uint8_t *buf = buffer->data + buffer->count;
    for (int32_t i = 0; i < first_part; i += 4) rng_get_4bytes(rng, buf + i);
    buffer->count += first_part;

    /* Get remaining 0 - 3 bytes */
    if (second_part) {
        uint8_t wordbuf[4] = {0};
        rng_get_4bytes(rng, wordbuf);
        bender_buffer_push_bytes(buffer, wordbuf, second_part);
    }

    return bender_wrap_buffer(buffer);
}

static const BenderMethod rng_methods[] = {
    {"uniform", cfun_rng_uniform},
    {"int", cfun_rng_int},
    {"buffer", cfun_rng_buffer},
    {NULL, NULL}
};

static int bender_rng_get(void *p, Bender key, Bender *out) {
    (void) p;
    if (!bender_checktype(key, BENDER_KEYWORD)) return 0;
    return bender_getmethod(bender_unwrap_keyword(key), rng_methods, out);
}

static Bender bender_rng_next(void *p, Bender key) {
    (void) p;
    return bender_nextmethod(rng_methods, key);
}

/* Get a random number */
BENDER_CORE_FN(bender_rand,
              "(math/random)",
              "Returns a uniformly distributed random number between 0 and 1.") {
    (void) argv;
    bender_fixarity(argc, 0);
    return bender_wrap_number(bender_rng_double(&bender_vm.rng));
}

/* Seed the random number generator */
BENDER_CORE_FN(bender_srand,
              "(math/seedrandom seed)",
              "Set the seed for the random number generator. `seed` should be "
              "an integer or a buffer."
             ) {
    bender_fixarity(argc, 1);
    if (bender_checkint(argv[0])) {
        uint32_t seed = (uint32_t)(bender_getinteger(argv, 0));
        bender_rng_seed(&bender_vm.rng, seed);
    } else {
        BenderByteView bytes = bender_getbytes(argv, 0);
        bender_rng_longseed(&bender_vm.rng, bytes.bytes, bytes.len);
    }
    return bender_wrap_nil();
}

#define BENDER_DEFINE_NAMED_MATHOP(bender_name, fop, doc)\
BENDER_CORE_FN(bender_##fop, "(math/" bender_name " x)", doc) {\
    bender_fixarity(argc, 1); \
    double x = bender_getnumber(argv, 0); \
    return bender_wrap_number(fop(x)); \
}

#define BENDER_DEFINE_MATHOP(fop, doc) BENDER_DEFINE_NAMED_MATHOP(#fop, fop, doc)

BENDER_DEFINE_MATHOP(acos, "Returns the arccosine of x.")
BENDER_DEFINE_MATHOP(asin, "Returns the arcsin of x.")
BENDER_DEFINE_MATHOP(atan, "Returns the arctangent of x.")
BENDER_DEFINE_MATHOP(cos, "Returns the cosine of x.")
BENDER_DEFINE_MATHOP(cosh, "Returns the hyperbolic cosine of x.")
BENDER_DEFINE_MATHOP(acosh, "Returns the hyperbolic arccosine of x.")
BENDER_DEFINE_MATHOP(sin, "Returns the sine of x.")
BENDER_DEFINE_MATHOP(sinh, "Returns the hyperbolic sine of x.")
BENDER_DEFINE_MATHOP(tan, "Returns the tangent of x.")
BENDER_DEFINE_MATHOP(tanh, "Returns the hyperbolic tangent of x.")
BENDER_DEFINE_MATHOP(exp, "Returns e to the power of x.")
BENDER_DEFINE_MATHOP(exp2, "Returns 2 to the power of x.")
BENDER_DEFINE_MATHOP(log1p, "Returns (log base e of x) + 1 more accurately than (+ (math/log x) 1)")
#ifndef BENDER_PLAN9
BENDER_DEFINE_MATHOP(expm1, "Returns e to the power of x minus 1.")
BENDER_DEFINE_MATHOP(cbrt, "Returns the cube root of x.")
BENDER_DEFINE_MATHOP(erf, "Returns the error function of x.")
BENDER_DEFINE_MATHOP(erfc, "Returns the complementary error function of x.")
BENDER_DEFINE_NAMED_MATHOP("log-gamma", lgamma, "Returns log-gamma(x).")
BENDER_DEFINE_NAMED_MATHOP("gamma", tgamma, "Returns gamma(x).")
BENDER_DEFINE_MATHOP(atanh, "Returns the hyperbolic arctangent of x.")
BENDER_DEFINE_MATHOP(asinh, "Returns the hyperbolic arcsine of x.")
#endif
BENDER_DEFINE_MATHOP(log, "Returns the natural logarithm of x.")
BENDER_DEFINE_MATHOP(log10, "Returns the log base 10 of x.")
BENDER_DEFINE_MATHOP(log2, "Returns the log base 2 of x.")
BENDER_DEFINE_MATHOP(sqrt, "Returns the square root of x.")
BENDER_DEFINE_MATHOP(ceil, "Returns the smallest integer value number that is not less than x.")
BENDER_DEFINE_MATHOP(floor, "Returns the largest integer value number that is not greater than x.")
BENDER_DEFINE_MATHOP(trunc, "Returns the integer between x and 0 nearest to x.")
BENDER_DEFINE_MATHOP(round, "Returns the integer nearest to x.")
BENDER_DEFINE_NAMED_MATHOP("abs", fabs, "Return the absolute value of x.")

#define BENDER_DEFINE_MATH2OP(name, fop, signature, doc)\
BENDER_CORE_FN(bender_##name, signature, doc) {\
    bender_fixarity(argc, 2); \
    double lhs = bender_getnumber(argv, 0); \
    double rhs = bender_getnumber(argv, 1); \
    return bender_wrap_number(fop(lhs, rhs)); \
}

BENDER_DEFINE_MATH2OP(atan2, atan2, "(math/atan2 y x)", "Returns the arctangent of y/x. Works even when x is 0.")
BENDER_DEFINE_MATH2OP(pow, pow, "(math/pow a x)", "Returns a to the power of x.")
BENDER_DEFINE_MATH2OP(hypot, hypot, "(math/hypot a b)", "Returns c from the equation c^2 = a^2 + b^2.")
#ifndef BENDER_PLAN9
BENDER_DEFINE_MATH2OP(nextafter, nextafter,  "(math/next x y)", "Returns the next representable floating point value after x in the direction of y.")
#endif

BENDER_CORE_FN(bender_not, "(not x)", "Returns the boolean inverse of x.") {
    bender_fixarity(argc, 1);
    return bender_wrap_boolean(!bender_truthy(argv[0]));
}

static double bender_gcd(double x, double y) {
    if (isnan(x) || isnan(y)) {
#ifdef NAN
        return NAN;
#else
        return 0.0 / 0.0;
#endif
    }
    if (isinf(x) || isinf(y)) return INFINITY;
    while (y != 0) {
        double temp = y;
        y = fmod(x, y);
        x = temp;
    }
    return x;
}

static double bender_lcm(double x, double y) {
    return (x / bender_gcd(x, y)) * y;
}

BENDER_CORE_FN(bender_cfun_gcd, "(math/gcd x y)",
              "Returns the greatest common divisor between x and y.") {
    bender_fixarity(argc, 2);
    double x = bender_getnumber(argv, 0);
    double y = bender_getnumber(argv, 1);
    return bender_wrap_number(bender_gcd(x, y));
}

BENDER_CORE_FN(bender_cfun_lcm, "(math/lcm x y)",
              "Returns the least common multiple of x and y.") {
    bender_fixarity(argc, 2);
    double x = bender_getnumber(argv, 0);
    double y = bender_getnumber(argv, 1);
    return bender_wrap_number(bender_lcm(x, y));
}

BENDER_CORE_FN(bender_cfun_frexp, "(math/frexp x)",
              "Returns a tuple of `[mantissa exponent]` from a number `x`.") {
    bender_fixarity(argc, 1);
    double x = bender_getnumber(argv, 0);
    int exp;
    x = frexp(x, &exp);
    Bender *result = bender_tuple_begin(2);
    result[0] = bender_wrap_number(x);
    result[1] = bender_wrap_number((double) exp);
    return bender_wrap_tuple(bender_tuple_end(result));
}

BENDER_CORE_FN(bender_cfun_ldexp, "(math/ldexp m e)",
              "Creates a new number from a mantissa and an exponent.") {
    bender_fixarity(argc, 2);
    double x = bender_getnumber(argv, 0);
    int32_t y = bender_getinteger(argv, 1);
    return bender_wrap_number(ldexp(x, y));
}

/* Module entry point */
void bender_lib_math(BenderTable *env) {
    BenderRegExt math_cfuns[] = {
        BENDER_CORE_REG("not", bender_not),
        BENDER_CORE_REG("math/random", bender_rand),
        BENDER_CORE_REG("math/seedrandom", bender_srand),
        BENDER_CORE_REG("math/cos", bender_cos),
        BENDER_CORE_REG("math/sin", bender_sin),
        BENDER_CORE_REG("math/tan", bender_tan),
        BENDER_CORE_REG("math/acos", bender_acos),
        BENDER_CORE_REG("math/asin", bender_asin),
        BENDER_CORE_REG("math/atan", bender_atan),
        BENDER_CORE_REG("math/exp", bender_exp),
        BENDER_CORE_REG("math/log", bender_log),
        BENDER_CORE_REG("math/log10", bender_log10),
        BENDER_CORE_REG("math/log2", bender_log2),
        BENDER_CORE_REG("math/sqrt", bender_sqrt),
#ifndef BENDER_PLAN9
        BENDER_CORE_REG("math/cbrt", bender_cbrt),
        BENDER_CORE_REG("math/gamma", bender_tgamma),
        BENDER_CORE_REG("math/log-gamma", bender_lgamma),
        BENDER_CORE_REG("math/erfc", bender_erfc),
        BENDER_CORE_REG("math/erf", bender_erf),
        BENDER_CORE_REG("math/expm1", bender_expm1),
        BENDER_CORE_REG("math/atanh", bender_atanh),
        BENDER_CORE_REG("math/asinh", bender_asinh),
        BENDER_CORE_REG("math/next", bender_nextafter),
#endif
        BENDER_CORE_REG("math/floor", bender_floor),
        BENDER_CORE_REG("math/ceil", bender_ceil),
        BENDER_CORE_REG("math/pow", bender_pow),
        BENDER_CORE_REG("math/abs", bender_fabs),
        BENDER_CORE_REG("math/sinh", bender_sinh),
        BENDER_CORE_REG("math/cosh", bender_cosh),
        BENDER_CORE_REG("math/tanh", bender_tanh),
        BENDER_CORE_REG("math/acosh", bender_acosh),
        BENDER_CORE_REG("math/atan2", bender_atan2),
        BENDER_CORE_REG("math/rng", cfun_rng_make),
        BENDER_CORE_REG("math/rng-uniform", cfun_rng_uniform),
        BENDER_CORE_REG("math/rng-int", cfun_rng_int),
        BENDER_CORE_REG("math/rng-buffer", cfun_rng_buffer),
        BENDER_CORE_REG("math/hypot", bender_hypot),
        BENDER_CORE_REG("math/exp2", bender_exp2),
        BENDER_CORE_REG("math/log1p", bender_log1p),
        BENDER_CORE_REG("math/trunc", bender_trunc),
        BENDER_CORE_REG("math/round", bender_round),
        BENDER_CORE_REG("math/gcd", bender_cfun_gcd),
        BENDER_CORE_REG("math/lcm", bender_cfun_lcm),
        BENDER_CORE_REG("math/frexp", bender_cfun_frexp),
        BENDER_CORE_REG("math/ldexp", bender_cfun_ldexp),
        BENDER_REG_END
    };
    bender_core_cfuns_ext(env, NULL, math_cfuns);
    bender_register_abstract_type(&bender_rng_type);
#ifdef BENDER_BOOTSTRAP
    BENDER_CORE_DEF(env, "math/pi", bender_wrap_number(3.1415926535897931),
                   "The value pi.");
    BENDER_CORE_DEF(env, "math/e", bender_wrap_number(2.7182818284590451),
                   "The base of the natural log.");
    BENDER_CORE_DEF(env, "math/inf", bender_wrap_number(INFINITY),
                   "The number representing positive infinity");
    BENDER_CORE_DEF(env, "math/-inf", bender_wrap_number(-INFINITY),
                   "The number representing negative infinity");
    BENDER_CORE_DEF(env, "math/int32-min", bender_wrap_number(INT32_MIN),
                   "The minimum contiguous integer representable by a 32 bit signed integer");
    BENDER_CORE_DEF(env, "math/int32-max", bender_wrap_number(INT32_MAX),
                   "The maximum contiguous integer representable by a 32 bit signed integer");
    BENDER_CORE_DEF(env, "math/int-min", bender_wrap_number(BENDER_INTMIN_DOUBLE),
                   "The minimum contiguous integer representable by a double (-(2^53))");
    BENDER_CORE_DEF(env, "math/int-max", bender_wrap_number(BENDER_INTMAX_DOUBLE),
                   "The maximum contiguous integer representable by a double (2^53)");
#ifdef NAN
    BENDER_CORE_DEF(env, "math/nan", bender_wrap_number(NAN), "Not a number (IEEE-754 NaN)");
#else
    BENDER_CORE_DEF(env, "math/nan", bender_wrap_number(0.0 / 0.0), "Not a number (IEEE-754 NaN)");
#endif
#endif
}
