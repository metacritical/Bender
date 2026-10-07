#!/bin/sh
# tools/janet-aot.sh — AOT spike driver (see docs/internals/aot-plan.md Phase 0).
#   tools/janet-aot.sh app.janet [-o app] [-c] [-S] [-E args...]
# -o FILE  output binary (default: ./app, sibling of input without extension)
# -c       emit C only (app.c next to output path)
# -S       print generated C to stdout (compile nothing)
# -E       build to a temp dir, run it with leftover args, discard binary
set -u
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
JANET="$ROOT/build/janet"
CC=${CC:-cc}

SRC=""
OUT=""
EXTERNS=""
EXTRAC=""
LDEXTRA=""
NATIVE=""
ONLY_C=0
PRINT_C=0
RUN_MODE=0
while [ $# -gt 0 ]; do
  case "$1" in
    -o) OUT="$2"; shift 2;;
    -c) ONLY_C=1; shift;;
    -S) PRINT_C=1; shift;;
    -E) RUN_MODE=1; shift; break;;
    --native) NATIVE=1; shift;;
    --externs) EXTERNS="$2"; shift 2;;
    -x) EXTRAC="$EXTRAC $2"; shift 2;;
    --ldflags) LDEXTRA="$2"; shift;;
    -*) echo "janet-aot: unknown flag $1" >&2; exit 2;;
    *) if [ -z "$SRC" ]; then SRC="$1"; else echo "janet-aot: one input only" >&2; exit 2; fi; shift;;
  esac
done
[ -n "$SRC" ] || { echo "usage: janet-aot.sh app.janet [-o app] [-c] [-S] [-E args...] [--externs FILE] [-x FILE] [--ldflags STR]" >&2; exit 2; }

[ -x "$JANET" ] || { echo "janet-aot: build janet first (make)" >&2; exit 2; }

if [ -z "$OUT" ]; then OUT="$(dirname -- "$SRC")/$(basename -- "$SRC" .janet)"; fi


if [ "$RUN_MODE" -eq 1 ]; then
  TMP=$(mktemp -d)
  trap 'rm -rf "$TMP"' EXIT INT TERM
  if [ -n "$NATIVE" ]; then
    "$JANET" "$ROOT/tools/callgraph.janet" "$SRC" --emit-native-lib "$TMP" ${EXTERNS:+--externs "$EXTERNS"}
    "$JANET" "$ROOT/tools/mkimage.janet" "$SRC" --native "$TMP/native-names.txt" "$TMP/app.c"
    "$CC" -O2 -w -DJANET_AOT_NATIVE_INIT -I"$ROOT/build" -I"$ROOT/src/include" "$TMP/app.c" \
      "$TMP/native.c" $EXTRAC "$ROOT/build/libjanet.a" $LDEXTRA -lm -lpthread -o "$TMP/app"
  else
    "$JANET" "$ROOT/tools/mkimage.janet" "$SRC" "$TMP/app.c"
    "$CC" -O2 -I"$ROOT/build" -I"$ROOT/src/include" "$TMP/app.c" \
      "$ROOT/build/libjanet.a" -lm -lpthread -o "$TMP/app"
  fi
  exec "$TMP/app" "$@"
fi

CFILE="${OUT}.c"
if [ -n "$NATIVE" ]; then
  NDIR="${OUT}-native"
  "$JANET" "$ROOT/tools/callgraph.janet" "$SRC" --emit-native-lib "$NDIR" ${EXTERNS:+--externs "$EXTERNS"}
  "$JANET" "$ROOT/tools/mkimage.janet" "$SRC" --native "$NDIR/native-names.txt" "$CFILE"
  if [ "$PRINT_C" -eq 1 ]; then cat "$CFILE" "$NDIR/native.c"; exit 0; fi
  if [ "$ONLY_C" -eq 1 ]; then echo "wrote $CFILE + $NDIR/native.c"; exit 0; fi
  "$CC" -O2 -w -DJANET_AOT_NATIVE_INIT -I"$ROOT/build" -I"$ROOT/src/include" "$CFILE" \
    "$NDIR/native.c" $EXTRAC "$ROOT/build/libjanet.a" $LDEXTRA -lm -lpthread -o "$OUT"
else
  "$JANET" "$ROOT/tools/mkimage.janet" "$SRC" "$CFILE"
  if [ "$PRINT_C" -eq 1 ]; then cat "$CFILE"; rm -f "$CFILE"; exit 0; fi
  if [ "$ONLY_C" -eq 1 ]; then echo "wrote $CFILE"; exit 0; fi
  "$CC" -O2 -I"$ROOT/build" -I"$ROOT/src/include" "$CFILE" \
    "$ROOT/build/libjanet.a" -lm -lpthread -o "$OUT"
fi
echo "built $OUT"
