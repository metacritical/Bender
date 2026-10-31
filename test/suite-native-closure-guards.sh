#!/bin/sh
# Run from repository root against the already-built executor-enabled runtime.
# No runtime rebuild: emit C, compile one harness and link the existing archive.
set -eu
: "${BENDER_AOT_TEST_ROOT:?set a private scratch root}"
: "${XDG_CACHE_HOME:?set a private cache root}"
root=$(mktemp -d "$BENDER_AOT_TEST_ROOT/native-closure-guards-XXXXXXXX")
export BENDER_NO_PROFILE=1
BENDER_NATIVE_EXECUTORS=1 ./build/bender tools/callgraph.ben \
    test/fixtures/native-closure.ben --emit-native-lib "$root/generated" \
    > "$root/emitter.log" 2>&1
id=$(awk '$2 == "closure-reader" { print $1 }' "$root/generated/native-executors.txt")
test "$(printf '%s\n' "$id" | wc -w | tr -d ' ')" = 1
cc -O1 -g -ffp-contract=off -DBENDER_NATIVE_EXECUTORS \
    -Isrc/include -Isrc/conf -Isrc/core \
    "-DNATIVE_EXECUTOR_C=\"$root/generated/native.c\"" \
    "-DNATIVE_CLOSURE_CODE=nx_${id}_code" \
    test/c/test-native-closure-guards.c build/libbender.a \
    -lm -lpthread -ldl -o "$root/guards" > "$root/build.log" 2>&1
"$root/guards"
echo "native closure guard evidence: $root"
