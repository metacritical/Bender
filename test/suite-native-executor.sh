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
# Fast-path calls must be emitted, not just the generic VM boundaries.
grep -q 'janet_native_array_put_fast' "$root/generated/native.c"
grep -q 'janet_native_table_get_fast' "$root/generated/native.c"
grep -q 'janet_native_container_call_fast' "$root/generated/native.c"
grep -q 'janet_native_table_put_fast' "$root/generated/native.c"
grep -q 'janet_native_buffer_push_string_ready' "$root/generated/native.c"
grep -q 'janet_native_core_predicate' "$root/generated/native.c"
grep -q 'janet_native_register_predicates' "$root/generated/native.c"
# Continuation emission is profile-narrowed: with no profile, NOT ONE
# continuation attempt may be emitted, which is exactly the pre-feature
# shape (a plain NX_EXIT to the VM at every non-terminal call PC). An
# always-on attempt cost +0.6-1.0% geomean for no measurable benefit.
if grep -q 'janet_native_call_enter' "$root/generated/native.c"; then
    echo "profile-free build emitted a continuation attempt" >&2; exit 1
fi
if grep -q 'return JANET_NATIVE_FRAME' "$root/generated/native.c"; then
    echo "profile-free build emitted a continuation step" >&2; exit 1
fi
test ! -s "$root/generated/native-continuations.txt"
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
grep 'get-loop' "$root/ops-native-native/native-executors.txt"
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
# Native-to-native continuation: a 3-deep executor->executor chain, a
# side-effecting callee (exactly-once), a callee that boundary-exits inside
# its own body (stale-`func` detector), a panic three frames deep, recursion
# past the trampoline's chain budget, and an allocating callee.
#
# Two runs of the SAME binary, with the continuation path enabled and
# disabled. Every assertion inside the image must hold both times -- that is
# the differential -- and the counter identity
#   chained call edges + interpreted Janet JOP_CALLs
# must come out to the same total, which is what catches a call executed
# twice or skipped when the callee is pure enough for output to agree.
# The emission is narrowed by :calledges evidence and gated by
# JANET_EXECUTOR_EDGE_MIN_CALLS, so this leg needs a real profile: --pgo
# collects one, gates the binary against the interpreter, and installs only
# on byte parity.
JANET_NATIVE_EXECUTORS=1 JANET_EXECUTOR_EDGE_MIN_CALLS=5 \
    JANET_EXECUTOR_LEAF_MIN_CALLS=5 ./build/bender --aot \
    test/fixtures/native-continuation.janet \
    --native --pgo '' -o "$root/cont" > "$root/cont-build.log" 2>&1
# Every selected edge is reported, and the emitted C carries exactly one
# attempt per selected PC -- no more, no fewer.
test -s "$root/cont-native/native-continuations.txt"
sel=$(wc -l < "$root/cont-native/native-continuations.txt" | tr -d ' ')
emitted=$(grep -c 'janet_native_call_enter' "$root/cont-native/native.c")
echo "continuation emission: selected=$sel emitted=$emitted"
test "$sel" = "$emitted"
"$root/cont" > "$root/cont-on.log" 2>&1
JANET_NATIVE_NO_CONTINUATION=1 "$root/cont" > "$root/cont-off.log" 2>&1
grep 'native-continuation image passed' "$root/cont-on.log"
grep 'native-continuation image passed' "$root/cont-off.log"
cont_total() { sed -n 's/.*cont-identity total=\([0-9]*\) .*/\1/p' "$1"; }
cont_edges() { sed -n 's/.*continuations=\([0-9]*\) .*/\1/p' "$1"; }
on_total=$(cont_total "$root/cont-on.log")
off_total=$(cont_total "$root/cont-off.log")
on_edges=$(cont_edges "$root/cont-on.log")
off_edges=$(cont_edges "$root/cont-off.log")
echo "continuation identity: on=$on_total off=$off_total edges on=$on_edges off=$off_edges"
test -n "$on_total"
test "$on_total" = "$off_total"
test "$off_edges" = "0"
test "$on_edges" -gt 0
echo "native continuation chain and counter identity passed"
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
