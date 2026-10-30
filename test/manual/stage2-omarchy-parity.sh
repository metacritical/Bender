#!/bin/bash
# Stage-2 mixed-region acceptance: bounded unmodified ttfx Omarchy
# decrypt + fireworks, six seeds, byte parity between a no-executor build
# and a mixed-native-executor build from the same branch. No timing claims.
set -eu
T=/private/var/folders/v6/lmp2f1ws4jl76bjbhzm0tcwm0000gn/T/opencode
BENDER=/Users/pankajdoharey/Development/Projects/bender
APP=/Users/pankajdoharey/Development/Projects/ML/ttfx/bender-port
OUT=$T/stage2-omarchy
CACHE_OFF=$T/stage2-omarchy-cache-off
CACHE_ON=$T/stage2-omarchy-cache-on
rm -rf "$OUT" "$CACHE_OFF" "$CACHE_ON"; mkdir -p "$OUT" "$CACHE_OFF" "$CACHE_ON"
export BENDER_NO_PROFILE=1

echo "== reference build (no executors)"
(cd "$APP" && XDG_CACHE_HOME="$CACHE_OFF" "$BENDER/build/bender" --aot bin/ttfx-cli.ben --native --no-auto-pgo -o "$OUT/ttfx-off" > "$OUT/build-off.log" 2>&1)
echo "== mixed executor build"
(cd "$APP" && XDG_CACHE_HOME="$CACHE_ON" BENDER_NATIVE_EXECUTORS=1 "$BENDER/build/bender" --aot bin/ttfx-cli.ben --native --no-auto-pgo -o "$OUT/ttfx-on" > "$OUT/build-on.log" 2>&1)

grep -c . "$OUT"/../stage2-omarchy-on-native/native-executors.txt 2>/dev/null || true
MANIFEST=$(find "$OUT"-on-native "$OUT" -maxdepth 2 -name native-executors.txt 2>/dev/null | head -1)
echo "manifest: $MANIFEST"; cat "$MANIFEST" || true

STATUS=0
for effect in decrypt fireworks; do
  for seed in 7 13 42 99 2026 314159; do
    "$OUT/ttfx-off" --seed "$seed" --dump-frames --max-frames 40 --input-file "$T/cli-bench-text.txt" "$effect" > "$OUT/off-$effect-$seed.out" 2> "$OUT/off-$effect-$seed.err" || true
    "$OUT/ttfx-on"  --seed "$seed" --dump-frames --max-frames 40 --input-file "$T/cli-bench-text.txt" "$effect" > "$OUT/on-$effect-$seed.out"  2> "$OUT/on-$effect-$seed.err"  || true
    if cmp -s "$OUT/off-$effect-$seed.out" "$OUT/on-$effect-$seed.out"; then
      echo "PARITY $effect seed=$seed $(wc -c < "$OUT/on-$effect-$seed.out") bytes"
    else
      echo "MISMATCH $effect seed=$seed"; STATUS=1
    fi
  done
done
echo "DONE status=$STATUS"
