#include <stdint.h>
#include <string.h>
#include <bender.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {

    /* init Bender */
    bender_init();

    /* fuzz the parser */
    BenderParser parser;
    bender_parser_init(&parser);
    for (int i = 0, done = 0; i < size; i++) {
        switch (bender_parser_status(&parser)) {
            case BENDER_PARSE_DEAD:
            case BENDER_PARSE_ERROR:
                done = 1;
                break;
            case BENDER_PARSE_PENDING:
                if (i == size) {
                    bender_parser_eof(&parser);
                } else {
                    bender_parser_consume(&parser, data[i]);
                }
                break;
            case BENDER_PARSE_ROOT:
                if (i >= size) {
                    bender_parser_eof(&parser);
                } else {
                    bender_parser_consume(&parser, data[i]);
                }
                break;
        }

        if (done == 1)
            break;
    }
    bender_parser_deinit(&parser);

    /* cleanup Bender */
    bender_deinit();

    return 0;
}

