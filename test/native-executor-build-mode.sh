#!/bin/sh
# Run the real Makefile in a disposable source tree; never switch the caller's
# build artifacts while another test or agent might be using them.
set -eu
# A parent `make NATIVE_EXECUTORS=1 test-executors` propagates command-line
# assignments through MAKEFLAGS. Clear them so the inner plain phases really
# are plain, rather than silently inheriting the parent's native opt-in.
unset MAKEFLAGS MFLAGS NATIVE_EXECUTORS
root=$(mktemp -d "${TMPDIR:-/tmp}/janet-native-mode-XXXXXXXX")
trap 'rm -rf "$root"' EXIT HUP INT TERM
mkdir "$root/repo"
git archive HEAD | tar -x -C "$root/repo"
# Include the Makefile being tested before it is committed.
cp Makefile "$root/repo/Makefile"
cd "$root/repo"

previous=
for mode in plain native plain; do
    case "$mode" in
        native) make -j2 NATIVE_EXECUTORS=1 > "$root/$mode-build.log" 2>&1 ;;
        *) make -j2 > "$root/$mode-build.log" 2>&1 ;;
    esac
    # The two compiled objects and all three linked products must refresh,
    # even if a future compiler happens to emit identical object bytes.
    current=$(python3 -c 'import os, sys; print(" ".join(str(os.stat(p).st_mtime_ns) for p in sys.argv[1:]))' \
        build/janet.o build/shell.o build/bender build/libjanet.a "$(if test -e build/libjanet.dylib; then echo build/libjanet.dylib; else echo build/libjanet.so; fi)")
    if test -n "$previous" && test "$current" = "$previous"; then
        echo "native build mode $mode reused previous runtime products" >&2
        exit 1
    fi
    if test -n "$previous"; then
        python3 -c 'import sys; old, new = (list(map(int, x.split())) for x in sys.argv[1:]); assert all(b > a for a, b in zip(old, new)), "mode switch did not rebuild every object/product"' "$previous" "$current"
    fi
    previous=$current
    case "$mode" in
        native) expected=enabled ;;
        *) expected=disabled ;;
    esac
    test "$(cat build/.native-executors-mode)" = "$expected"
    # Repeating the same mode must not touch the stamp or rebuild any product.
    before=$(python3 -c 'import os, sys; print(" ".join(str(os.stat(p).st_mtime_ns) for p in sys.argv[1:]))' \
        build/.native-executors-mode build/janet.o build/shell.o build/bender build/libjanet.a "$(if test -e build/libjanet.dylib; then echo build/libjanet.dylib; else echo build/libjanet.so; fi)")
    case "$mode" in
        native) make -j2 NATIVE_EXECUTORS=1 > "$root/$mode-repeat.log" 2>&1 ;;
        *) make -j2 > "$root/$mode-repeat.log" 2>&1 ;;
    esac
    after=$(python3 -c 'import os, sys; print(" ".join(str(os.stat(p).st_mtime_ns) for p in sys.argv[1:]))' \
        build/.native-executors-mode build/janet.o build/shell.o build/bender build/libjanet.a "$(if test -e build/libjanet.dylib; then echo build/libjanet.dylib; else echo build/libjanet.so; fi)")
    test "$before" = "$after"
    JANET_NATIVE_EXECUTORS=1 ./build/bender --aot \
        test/fixtures/native-executor.janet --native --no-auto-pgo \
        -o "$root/image-$mode" > "$root/$mode-aot.log" 2>&1
    if "$root/image-$mode" > "$root/$mode-image.log" 2>&1; then result=0; else result=$?; fi
    case "$mode" in
        native)
            test "$result" -eq 0
            grep -Eq 'native_init: available [1-9][0-9]* frame executors' "$root/$mode-image.log"
            grep -q 'image shared-stack resumes: passed' "$root/$mode-image.log" ;;
        *) test "$result" -ne 0
           grep -q 'runtime was built WITHOUT JANET_NATIVE_EXECUTORS' "$root/$mode-image.log" ;;
    esac
    echo "native build mode $mode: object refresh and runtime registration verified"
done
