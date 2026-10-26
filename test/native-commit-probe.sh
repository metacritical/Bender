#!/bin/sh
# Opt-in helper contract with private /tmp scratch; make build/bender also
# updates ordinary build products before generating the executor image.
set -eu
root=$(mktemp -d "${TMPDIR:-/tmp}/janet-commit-probe-XXXXXXXX")
trap 'rm -rf "$root"' EXIT HUP INT TERM
cc=${CC:-cc}
# Build the actual opt-in driver before emitting an image: its generated
# executor caller and linked runtime must share the same hook ABI flag.
make -s NATIVE_EXECUTORS=1 NATIVE_COMMIT_PROBE=1 build/bender
JANET_NATIVE_EXECUTORS=1 ./build/bender --aot \
    test/fixtures/native-executor.janet --native --no-auto-pgo \
    -o "$root/image" > "$root/image-build.log" 2>&1
"$root/image" > "$root/image.log" 2>&1
grep -q 'image shared-stack resumes: passed' "$root/image.log"
# The source-derived amalgamation is compiled independently below so the
# callback fixture exercises the same opt-in runtime, not a stale object.
"$cc" -O2 -DJANET_NATIVE_EXECUTORS -DJANET_NATIVE_COMMIT_PROBE=1 \
    -Isrc/include -Isrc/conf -c build/c/janet.c -o "$root/runtime.o"
"$cc" -O2 -DJANET_NATIVE_COMMIT_PROBE=1 -Isrc/include -Isrc/conf \
    test/native-commit-probe.c "$root/runtime.o" -lm -lpthread -ldl \
    -o "$root/probe"
"$root/probe"
