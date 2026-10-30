#include <stdint.h>
#include <bender.h>

/* Disable leak sanitizer */
int __lsan_is_turned_off(void) {
    return 1;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {

    /* init Bender */
    bender_init();

    /* fuzz the compiler in src/core/compile.c: parse the untrusted source and
       compile each produced form to bytecode */
    BenderTable *env = bender_core_env(NULL);
    BenderParser parser;
    bender_parser_init(&parser);

    BenderTryState tstate;
    if (bender_try(&tstate) == BENDER_SIGNAL_OK) {
        for (size_t i = 0; i < size; i++) {
            if (bender_parser_status(&parser) == BENDER_PARSE_ERROR)
                break;
            bender_parser_consume(&parser, data[i]);
            while (bender_parser_has_more(&parser)) {
                Bender form = bender_parser_produce(&parser);
                BenderCompileResult res = bender_compile(form, env, bender_cstring("fuzz"));
                (void) res;
            }
        }
    }
    bender_restore(&tstate);

    bender_parser_deinit(&parser);

    /* cleanup Bender */
    bender_deinit();

    return 0;
}
