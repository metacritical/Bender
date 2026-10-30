#!/bin/bash
# Build a counter probe for the unmodified ttfx app: the CLI source is
# copied with import paths rewritten to absolute strings (logic untouched)
# and extended with executor counter printing AFTER the normal run.
set -eu
T=/private/var/folders/v6/lmp2f1ws4jl76bjbhzm0tcwm0000gn/T/opencode
BENDER=/Users/pankajdoharey/Development/Projects/bender
APP=/Users/pankajdoharey/Development/Projects/ML/ttfx/bender-port
OUT=$T/stage2-omarchy-probe
CACHE=$T/stage2-omarchy-probe-cache
PROBE_SRC=$APP/bin/ttfx-probe-tmp.ben
rm -rf "$OUT" "$CACHE"; mkdir -p "$OUT" "$CACHE"
trap 'rm -f "$PROBE_SRC"' EXIT
export BENDER_NO_PROFILE=1
python3 - "$APP/bin/ttfx-cli.ben" "$PROBE_SRC" <<'PYEOF'
import sys
src, dst = sys.argv[1], sys.argv[2]
app = "/Users/pankajdoharey/Development/Projects/ML/ttfx/bender-port"
text = open(src).read()
# keep the app's relative imports intact by placing the probe beside
# the original in bin/ (temporary untracked file, removed after build)
probe_tail = '''
    # --- stage-2 counter probe (probe copy only; app source untouched) ---
    (when-let [entry (or (get (curenv) 'native/executor-stats) (get (curenv) 'executor-stats))]
      (def stats (if (function? entry) entry (entry :value)))
      (def gentry (or (get (curenv) 'native/executor-global-stats) (get (curenv) 'executor-global-stats)))
      (def gstats (if (function? gentry) gentry (gentry :value)))
      (def gs (gstats))
      (eprint "probe-global attempts=" (gs 0) " attached=" (gs 1)
              " ambiguous=" (gs 2) " nomatch=" (gs 3) " vmexits=" (gs 4))
      (loop [id :range [1 400]]
        (def [ok counts] (protect (stats id)))
        (when (and ok (or (> (counts 0) 0) (> (counts 3) 0)))
          (eprint "probe-executor " id " entries=" (counts 0) " backedges=" (counts 1)
                  " guards=" (counts 2) " ops=" (counts 3) " vmexits=" (counts 4)))))
'''
anchor = "    (S/faithful-dump ctx fx maxf)\n    (os/exit 0))"
assert anchor in text, "dump-frames anchor not found"
text = text.replace(anchor, "    (S/faithful-dump ctx fx maxf)\n" + probe_tail + "    (os/exit 0))")
open(dst, "w").write(text)
PYEOF
cd "$APP"
XDG_CACHE_HOME="$CACHE" BENDER_NATIVE_EXECUTORS=1 "$BENDER/build/bender" --aot "$PROBE_SRC" --native --no-auto-pgo -o "$OUT/ttfx-probe" > "$OUT/build.log" 2>&1
MANIFEST=$(find "$OUT" -maxdepth 2 -name native-executors.txt | while read m; do [ -s "$m" ] && echo "$m"; done | head -1)
echo "manifest: $MANIFEST ($(wc -l < "$MANIFEST" | tr -d ' ') entries)"
for effect in decrypt fireworks; do
  for seed in 7 42 314159; do
    echo "== $effect seed=$seed"
    "$OUT/ttfx-probe" --seed "$seed" --dump-frames --max-frames 40 --input-file "$T/cli-bench-text.txt" "$effect" > "$OUT/f-$effect-$seed.out" 2> "$OUT/f-$effect-$seed.err" || true
    grep -E "probe-global" "$OUT/f-$effect-$seed.err"
    grep -E "probe-executor" "$OUT/f-$effect-$seed.err" | sort -t' ' -k2 -n | awk '$5+0 > 100 || $9+0 > 0' | head -25
  done
done
echo DONE
