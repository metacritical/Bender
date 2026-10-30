#!/bin/sh
# Deterministic major scheduling regressions; uses an already-built runtime.
# Build: make NATIVE_EXECUTORS=1 build/libbender.a
# Run: sh test/suite-gc-major-pressure.sh
# RUNTIME_TREE may select a historical tree for baseline-failure verification.
# RIPWIRE_TEST_DEPS: src/core/vm.c src/core/gc.c src/core/state.h test/c/test-gc-major-pressure.c test/c/test-gc-budget-current-blocks.c
set -eu
here=$(cd "$(dirname "$0")/.." && pwd)
tree=${RUNTIME_TREE:-$here}
root=$(mktemp -d "${BENDER_AOT_TEST_ROOT:-${TMPDIR:-/tmp}}/gc-major-XXXXXXXX")
trap 'rm -rf "$root"' EXIT HUP INT TERM
for fixture in major-pressure budget-current-blocks; do
    "${CC:-cc}" -O2 -g -DBENDER_NATIVE_EXECUTORS \
        -I"$tree/src/include" -I"$tree/src/conf" -I"$tree/src/core" \
        "$here/test/c/test-gc-$fixture.c" "$tree/build/libbender.a" \
        -lm -lpthread -ldl -o "$root/$fixture"
    "$root/$fixture"
done
