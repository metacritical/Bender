#!/bin/sh
# Thin shim: the AOT driver lives in the binary now (bender --aot),
# same grammar 1:1. Kept for existing callers (ttfx READMEs, scripts).
# Prefer: bender --aot app.janet [--native] [-o app] ...
HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
if [ -x "$HERE/../build/bender" ]; then
  BENDER="$HERE/../build/bender"
elif command -v bender >/dev/null 2>&1; then
  BENDER="bender"
else
  BENDER="janet"
fi
exec "$BENDER" --aot "$@"
