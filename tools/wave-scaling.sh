#!/bin/sh
# Spawn-phase scaling harness for the proc wave benchmark.
#
# Reports min-of-R spawn/wave/total ms and us-per-spawn for each N, so a
# claim about scaling is read off a curve rather than a single point.
# Every run is a FRESH process (the live set is the thing being measured,
# so it must not carry over) and swap is sampled before each series
# because this box routinely sits near its swap ceiling and sub-2x
# differences are then unattributable.
#
#   tools/wave-scaling.sh [-b BINARY] [-r REPS] [-l LANES] [N ...]
#
# Env passthrough: BENDER_GC_STATS=1 prints the collector counters that
# bench/cross-runtime/wave.ben's spawn phase is being attributed to.

set -u

BIN=./build/bender
REPS=3
LANES=11
BENCH=bench/cross-runtime/wave.ben

while [ $# -gt 0 ]; do
    case "$1" in
        -b) BIN=$2; shift 2 ;;
        -r) REPS=$2; shift 2 ;;
        -l) LANES=$2; shift 2 ;;
        -f) BENCH=$2; shift 2 ;;
        *) break ;;
    esac
done

if [ $# -eq 0 ]; then
    set -- 20000 50000 100000 200000
fi

printf 'binary:  %s\n' "$BIN"
printf 'bench:   %s (lanes=%s, reps=%s)\n' "$BENCH" "$LANES" "$REPS"
printf 'swap:    %s\n' "$(sysctl -n vm.swapusage)"
printf 'commit:  %s\n' "$(git rev-parse --short HEAD 2>/dev/null || echo '?')"
printf '\n%10s %12s %12s %12s %10s %10s\n' N spawn_ms wave_ms total_ms us_spawn peak_rss_mb

for n in "$@"; do
    best_spawn=''
    best_wave=''
    best_total=''
    best_rss=''
    i=0
    while [ "$i" -lt "$REPS" ]; do
        i=$((i + 1))
        # /usr/bin/time -l gives maximum resident set size on macOS.
        out=$(BENDER_LANES="$LANES" BENDER_NO_PROFILE=1 \
              /usr/bin/time -l "$BIN" "$BENCH" "$n" 2>&1)
        line=$(printf '%s\n' "$out" | grep -o 'spawn_ms=[0-9.]* wave_ms=[0-9.]* total_ms=[0-9.]*')
        [ -z "$line" ] && { printf '%10s  RUN FAILED\n' "$n"; printf '%s\n' "$out" | tail -5; continue; }
        s=$(printf '%s' "$line" | sed -n 's/.*spawn_ms=\([0-9]*\).*/\1/p')
        w=$(printf '%s' "$line" | sed -n 's/.*wave_ms=\([0-9]*\).*/\1/p')
        t=$(printf '%s' "$line" | sed -n 's/.*total_ms=\([0-9]*\).*/\1/p')
        r=$(printf '%s\n' "$out" | awk '/maximum resident set size/ {print int($1/1048576)}')
        if [ -z "$best_spawn" ] || [ "$s" -lt "$best_spawn" ]; then
            best_spawn=$s; best_wave=$w; best_total=$t; best_rss=${r:-0}
        fi
    done
    [ -z "$best_spawn" ] && continue
    printf '%10s %12s %12s %12s %10s %10s\n' \
        "$n" "$best_spawn" "$best_wave" "$best_total" \
        "$(awk -v s="$best_spawn" -v n="$n" 'BEGIN{printf "%.2f", s*1000.0/n}')" \
        "$best_rss"
done
