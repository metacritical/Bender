#!/bin/sh
# Run from the repository root. No global cache or build artifact is changed.
set -eu
: "${BENDER_AOT_TEST_ROOT:?set a private scratch root}"
root=$(mktemp -d "$BENDER_AOT_TEST_ROOT/gc-trace-cache-XXXXXXXX")
${CC:-cc} -O1 -g -Isrc/include -Isrc/conf -Isrc/core \
    build/c/bender.c test/c/test-gc-trace-cache.c \
    -lm -lpthread -ldl -o "$root/test" > "$root/build.log" 2>&1
"$root/test"
