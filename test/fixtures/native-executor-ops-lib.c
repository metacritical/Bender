#include <stdint.h>

typedef struct { int32_t x, y; } JanetOpsPair;

int32_t janet_test_pair_sum(JanetOpsPair p) {
    return p.x + p.y;
}

JanetOpsPair janet_test_pair_bump(JanetOpsPair p) {
    return (JanetOpsPair){p.x + 1, p.y + 1};
}
