#!/bin/sh
# Run from repository root after the parent-coordinated executor-enabled build.
set -eu
: "${BENDER_AOT_TEST_ROOT:?set a private scratch root}"
: "${XDG_CACHE_HOME:?set a private cache root}"
root=$(mktemp -d "$BENDER_AOT_TEST_ROOT/native-closure-XXXXXXXX")
export BENDER_NO_PROFILE=1
echo "native closure evidence: $root"
./build/bender test/fixtures/native-closure.ben > "$root/boxed.out" 2> "$root/boxed.err"
BENDER_NATIVE_EXECUTORS=1 ./build/bender --aot test/fixtures/native-closure.ben \
    --native --no-auto-pgo -o "$root/image" > "$root/build.log" 2>&1
manifest="$root/image-native/native-executors.txt"
reader_id=$(awk '$2 == "closure-reader" { print $1 }' "$manifest")
live_id=$(awk '$2 == "closure-live" { print $1 }' "$manifest")
# Exactly one descriptor per definition, never one per closure instance.
test "$(printf '%s\n' "$reader_id" | wc -w | tr -d ' ')" = 1
test "$(printf '%s\n' "$live_id" | wc -w | tr -d ' ')" = 1
if grep -q 'closure-mutable' "$manifest"; then
    echo "setu closure unexpectedly admitted" >&2; exit 1
fi
NATIVE_CLOSURE_READER_ID="$reader_id" NATIVE_CLOSURE_LIVE_ID="$live_id" \
    "$root/image" > "$root/native.out" 2> "$root/native.err"
cmp "$root/boxed.out" "$root/native.out"
grep -q 'native-closure entries verified' "$root/native.err"
if grep -q 'native executor registration refused' "$root/native.err"; then
    echo "executor registration refused" >&2; exit 1
fi
grep 'native-closure parity passed' "$root/native.out"
echo "native closure entry/parity/setu exclusion passed: $root"
