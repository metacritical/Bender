#!/bin/sh
# A bounded one-shot poll schedule against the same generated fixture image.
set -eu
: "${JANET_AOT_TEST_ROOT:?set a private scratch root}"
: "${XDG_CACHE_HOME:?set a private cache root}"
root=$(mktemp -d "$JANET_AOT_TEST_ROOT/forced-quantum-XXXXXXXX")
seed=${JANET_FORCED_QUANTUM_SEED:-17}
case "$seed" in *[!0-9]*|'') echo 'seed must be a nonnegative integer' >&2; exit 1;; esac
export JANET_NO_PROFILE=1
JANET_NATIVE_EXECUTORS=1 ./build/bender tools/callgraph.janet \
    test/fixtures/native-executor.janet --emit-native-lib "$root/generated" \
    > "$root/emitter.log" 2>&1
shasum -a 256 "$root/generated/native.c"
cc -O1 -g -ffp-contract=off -Isrc/include -Isrc/conf \
    -DJANET_NATIVE_EXECUTORS -DJANET_QUANTUM_BUDGET=1 \
    -c build/c/janet.c -o "$root/janet.o" > "$root/build.log" 2>&1
cc -O1 -g -ffp-contract=off -Isrc/include -Isrc/conf -Isrc/core \
    -DJANET_NATIVE_EXECUTORS -DJANET_QUANTUM_BUDGET=1 \
    "-DNATIVE_EXECUTOR_C=\"$root/generated/native.c\"" \
    test/native-executor-forced-quantum.c "$root/janet.o" \
    -lm -lpthread -ldl -o "$root/forced" >> "$root/build.log" 2>&1
shasum -a 256 "$root/forced"
"$root/forced" test/fixtures/native-executor.janet "$seed" > "$root/first.trace"
"$root/forced" test/fixtures/native-executor.janet "$seed" > "$root/second.trace"
cmp "$root/first.trace" "$root/second.trace"
cat "$root/first.trace"
echo "forced quantum one-shot replay passed seed=$seed scratch=$root"
