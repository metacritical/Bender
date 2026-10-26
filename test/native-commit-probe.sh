#!/bin/sh
# Standalone opt-in helper contract, using source-derived amalgamation and
# private /tmp artifacts; neither generated executor counters nor a reporter.
set -eu
root=$(mktemp -d "${TMPDIR:-/tmp}/janet-commit-probe-XXXXXXXX")
trap 'rm -rf "$root"' EXIT HUP INT TERM
cc=${CC:-cc}
# The generated amalgamation is sourced from the current checkout. The regular
# runtime must be built first (make NATIVE_EXECUTORS=1 build/bender).
make -s NATIVE_EXECUTORS=1 build/bender
"$cc" -O2 -DJANET_NATIVE_EXECUTORS -DJANET_NATIVE_COMMIT_PROBE=1 \
    -Isrc/include -Isrc/conf -c build/c/janet.c -o "$root/runtime.o"
"$cc" -O2 -DJANET_NATIVE_COMMIT_PROBE=1 -Isrc/include -Isrc/conf \
    test/native-commit-probe.c "$root/runtime.o" -lm -lpthread -ldl \
    -o "$root/probe"
"$root/probe"
