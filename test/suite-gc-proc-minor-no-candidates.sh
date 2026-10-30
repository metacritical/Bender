#!/bin/sh
# Run against an archive built with NATIVE_EXECUTORS=1 (counter assertions).
# RIPWIRE_TEST_DEPS: src/core/gc.c test/c/test-gc-proc-minor-no-candidates.c
set -eu
: "${BENDER_AOT_TEST_ROOT:?set a private scratch root}"
root=$(mktemp -d "$BENDER_AOT_TEST_ROOT/proc-minor-no-candidates-XXXXXXXX")
${CC:-cc} -O2 -g -DBENDER_NATIVE_EXECUTORS \
    -Isrc/include -Isrc/conf -Isrc/core \
    test/c/test-gc-proc-minor-no-candidates.c build/libbender.a \
    -lm -lpthread -ldl -o "$root/test" > "$root/build.log" 2>&1
"$root/test"
