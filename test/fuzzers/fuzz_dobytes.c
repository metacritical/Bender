#include <stdint.h>
#include <bender.h>

/* Disable leak sanitizer */
int __lsan_is_turned_off(void) {
    return 1;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {

    /* init Bender */
    bender_init();

    /* fuzz the compile + bytecode VM path (parse, compile, run) in src/core/compile.c and src/core/vm.c */
    BenderTable *env = bender_core_env(NULL);
    BenderTryState tstate;
    if (bender_try(&tstate) == BENDER_SIGNAL_OK) {
        Bender out;
        bender_dobytes(env, data, (int32_t) size, "<fuzz>", &out);
    }
    bender_restore(&tstate);

    /* cleanup Bender */
    bender_deinit();

    return 0;
}
