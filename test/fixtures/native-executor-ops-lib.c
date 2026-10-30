#include <stdint.h>

typedef struct { int32_t x, y; } BenderOpsPair;

int32_t bender_test_pair_sum(BenderOpsPair p) {
    return p.x + p.y;
}

BenderOpsPair bender_test_pair_bump(BenderOpsPair p) {
    return (BenderOpsPair){p.x + 1, p.y + 1};
}
