#include <stdint.h>
#include <bender.h>

/* Disable leak sanitizer */
int __lsan_is_turned_off(void) {
    return 1;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {

    /* init Bender */
    bender_init();

    /* fuzz the binary unmarshal (deserialization) path in src/core/marsh.c */
    BenderTable *reg = bender_env_lookup(bender_core_env(NULL));
    BenderTryState tstate;
    if (bender_try(&tstate) == BENDER_SIGNAL_OK) {
        const uint8_t *next = NULL;
        bender_unmarshal(data, size, 0, reg, &next);
    }
    bender_restore(&tstate);

    /* cleanup Bender */
    bender_deinit();

    return 0;
}
