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
# Stage 2's guarded geti/next/string equality, scratch tuple and FFI alias
# call require the *actual* executor-enabled runtime (the generic C harness
# below also checks budget-1/2 PCs). Build a tiny struct-by-value dylib in
# this private root: the alias wrapper allocates a tuple return under GC
# pressure, so a shortcut that drops argument roots cannot pass this gate.
case "$(uname -s)" in
    Darwin) ops_ext=dylib; ops_link=-dynamiclib ;;
    Linux) ops_ext=so; ops_link=-shared ;;
    *) echo "native-ops fixture requires a POSIX shared library" >&2; exit 1 ;;
esac
${CC:-cc} -O2 -fPIC "$ops_link" test/fixtures/native-executor-ops-lib.c \
    -o "$root/native-ops.$ops_ext"
export JANET_NATIVE_OPS_LIB="$root/native-ops.$ops_ext"
./build/bender --aot test/fixtures/native-executor-ops-main.janet --native \
    --no-auto-pgo -o "$root/ops-boxed" > "$root/ops-boxed-build.log" 2>&1
JANET_NATIVE_EXECUTORS=1 ./build/bender --aot \
    test/fixtures/native-executor-ops-main.janet --native --no-auto-pgo \
    -o "$root/ops-native" > "$root/ops-native-build.log" 2>&1
grep 'indexed-loop' "$root/ops-native-native/native-executors.txt"
grep 'scratch-loop' "$root/ops-native-native/native-executors.txt"
# The same generated primitive struct writer must accept the scratch array
# fast path and an immutable tuple, with other indexables still going through
# the ordinary janet_getindexed fallback.
grep -q 'if (janet_checktype(v, JANET_ARRAY))' "$root/ops-native-native/native.c"
grep -q 'else if (janet_checktype(v, JANET_TUPLE))' "$root/ops-native-native/native.c"
"$root/ops-boxed" > "$root/ops-boxed.out" 2> "$root/ops-boxed.err"
NATIVE_OPS_STATS=1 "$root/ops-native" > "$root/ops-native.out" 2> "$root/ops-native.err"
cmp "$root/ops-boxed.out" "$root/ops-native.out"
grep 'native-ops-global' "$root/ops-native.err"
echo "native geti/next/scratch/alias/GC image parity passed"
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
