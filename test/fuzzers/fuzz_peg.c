#include <stdint.h>
#include <stddef.h>
#include <bender.h>

#define BENDER_PEG_FUZZ_MAX_INPUT (1U << 20)

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size < 2 || size > BENDER_PEG_FUZZ_MAX_INPUT)
        return 0;

    /* Byte 0 selects the split point for a Bender PEG pattern and subject. */
    size_t patlen = (size_t)data[0] % (size - 1);
    const uint8_t *patsrc = data + 1;
    const uint8_t *text = data + 1 + patlen;
    size_t textlen = size - 1 - patlen;

    bender_init();

    BenderParser parser;
    bender_parser_init(&parser);
    Bender matchfn = bender_resolve_core("peg/match");
    if (bender_checktype(matchfn, BENDER_CFUNCTION)) {
        BenderCFunction cfun = bender_unwrap_cfunction(matchfn);

        /* Stop after parser errors: consuming again would panic. */
        for (size_t i = 0; i < patlen; i++) {
            bender_parser_consume(&parser, patsrc[i]);
            if (bender_parser_status(&parser) == BENDER_PARSE_ERROR)
                break;
        }
        if (bender_parser_status(&parser) != BENDER_PARSE_ERROR)
            bender_parser_eof(&parser);

        if (bender_parser_status(&parser) != BENDER_PARSE_ERROR &&
                bender_parser_has_more(&parser)) {
            Bender pattern = bender_parser_produce(&parser);
            BenderTryState tstate;
            if (bender_try(&tstate) == BENDER_SIGNAL_OK) {
                Bender argv[2];
                argv[0] = pattern;
                argv[1] = bender_stringv(text, (int32_t)textlen);
                cfun(2, argv);
            }
            bender_restore(&tstate);
        }
    }

    bender_parser_deinit(&parser);
    bender_deinit();
    return 0;
}
