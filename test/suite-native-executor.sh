#!/bin/sh
# Run from repository root after make. All outputs stay in a unique scratch root.
set -eu
: "${JANET_AOT_TEST_ROOT:?set a private scratch root}"
: "${XDG_CACHE_HOME:?set a private cache root}"
root=$(mktemp -d "$JANET_AOT_TEST_ROOT/native-executor-XXXXXXXX")
export JANET_NO_PROFILE=1
echo "native executor evidence: $root"
JANET_NATIVE_EXECUTORS=1 ./build/bender tools/callgraph.janet test/fixtures/native-executor.janet --emit-native-lib "$root/generated" > "$root/emitter.log" 2>&1
./build/bender test/native-executor-opcodes.janet "$root/generated/native.c"
test ! -s "$root/generated/native-names.txt"
grep 'numeric-sum' "$root/generated/native-executors.txt"
grep 'numeric-nested' "$root/generated/native-executors.txt"
# Stage 2: mixed-op bodies (calls/pushes/collection ops/division as VM
# boundaries) are admitted, including through the imported mixed module.
grep 'boundary' "$root/generated/native-executors.txt"
grep 'unsupported-loop' "$root/generated/native-executors.txt"
if grep 'unsupported-bit' "$root/generated/native-executors.txt"; then exit 1; fi
JANET_NATIVE_EXECUTORS=1 ./build/bender --aot test/fixtures/native-executor.janet --native --no-auto-pgo -o "$root/image" > "$root/image-build.log" 2>&1
"$root/image" > "$root/image.log" 2>&1
grep 'image-executor' "$root/image.log"
build_case() {
    name=$1
    shift
    cc -O1 -g -ffp-contract=off -Isrc/include -Isrc/conf "$@" -c build/c/janet.c -o "$root/$name.o" > "$root/$name-build.log" 2>&1
    cc -O1 -g -ffp-contract=off -Isrc/include -Isrc/conf -Isrc/core "$@" \
       "-DNATIVE_EXECUTOR_C=\"$root/generated/native.c\"" test/native-executor.c "$root/$name.o" \
       -lm -lpthread -ldl -o "$root/$name" >> "$root/$name-build.log" 2>&1
    "$root/$name" test/fixtures/native-executor.janet > "$root/$name.log" 2>&1
    cat "$root/$name.log"
    cc -O1 -g -ffp-contract=off -Isrc/include -Isrc/conf -Isrc/core "$@" \
       "-DNATIVE_EXECUTOR_C=\"$root/generated/native.c\"" test/native-executor-mixed.c "$root/$name.o" \
       -lm -lpthread -ldl -o "$root/$name-mixed" >> "$root/$name-build.log" 2>&1
    "$root/$name-mixed" test/fixtures/native-executor.janet > "$root/$name-mixed.log" 2>&1
    cat "$root/$name-mixed.log"
}
build_case budget1 -DJANET_NATIVE_EXECUTORS -DJANET_QUANTUM_BUDGET=1
build_case budget2 -DJANET_NATIVE_EXECUTORS -DJANET_QUANTUM_BUDGET=2
build_case portable -DJANET_NATIVE_EXECUTORS -DJANET_QUANTUM_BUDGET=2 -DJANET_NO_COMPUTED_GOTOS
build_case nointerrupt -DJANET_NATIVE_EXECUTORS -DJANET_NO_INTERPRETER_INTERRUPT
build_case disabled -DTEST_NATIVE_DISABLED
build_case sanitized -DJANET_NATIVE_EXECUTORS -DJANET_QUANTUM_BUDGET=2 -fsanitize=address,undefined -fno-omit-frame-pointer
echo "native executor matrix passed: $root"
