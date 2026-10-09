#!/bin/bash
# Benchmarks Game runs for Janet/Bender: repeated timing, mean/min/max.
# Usage: ./run.sh [reps]   (default reps=3)
# Reports elapsed (real) seconds like the game does; also prints user+sys.
# Binaries must already be built (see build.sh / README).
set -u
REPS=${1:-3}
cd "$(dirname "$0")"
B=../../build/bender

machine() {
  sysctl -n machdep.cpu.brand_string 2>/dev/null || uname -m
  echo "bun: $(bun --version 2>/dev/null || echo n/a)"
}

# time_case <label> <cmd...>: run REPS times, print each + stats
time_case() {
  local label="$1"; shift
  echo "### $label ($REPS reps)"
  local times=()
  for ((i = 1; i <= REPS; i++)); do
    local t
    t=$(TIMEFORMAT=%R; { time "$@" > /dev/null 2>&1; } 2>&1)
    times+=("$t")
    echo "  rep $i: ${t}s"
  done
  local stats
  stats=$(printf "%s\n" "${times[@]}" | python3 -c "
import sys, statistics
ts = sorted(float(l) for l in sys.stdin if l.strip())
print(f'mean={statistics.mean(ts):.3f} min={ts[0]:.3f} max={ts[-1]:.3f} n={len(ts)}')")
  echo "  $stats"
}

echo "machine: $(machine)"
echo "date: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
echo

build_native() {
  local src="$1" out="$2"
  JANET_TOOLS=../../tools $B --aot "$src" --native -o "$out" > /dev/null 2>&1 \
    || { echo "BUILD FAILED: $src"; exit 1; }
  echo "built $out ($(cat "${out}-native/native-names.txt" 2>/dev/null | tr '\n' ' '))"
}

build_native nbody.janet /tmp/bg-nbody
build_native spectralnorm.janet /tmp/bg-spectral
build_native fannkuch.janet /tmp/bg-fannkuch
echo

# correctness first (fail fast on mismatch)
echo "--- correctness ---"
diff <($B nbody.janet 1000 2>/dev/null) <(printf -- '-0.169075164\n-0.169087605\n') && echo "nbody 1000 OK"
diff <($B spectralnorm.janet 100 2>/dev/null) <(printf -- '1.274219991\n') && echo "spectral 100 OK"
diff <($B fannkuch.janet 7 2>/dev/null) <(printf -- '228\nPfannkuchen(7) = 16\n') && echo "fannkuch 7 OK"
diff <(/tmp/bg-nbody 1000 2>/dev/null) <(printf -- '-0.169075164\n-0.169087605\n') && echo "nbody native OK"
diff <(/tmp/bg-spectral 100 2>/dev/null) <(printf -- '1.274219991\n') && echo "spectral native OK"
diff <(/tmp/bg-fannkuch 7 2>/dev/null) <(printf -- '228\nPfannkuchen(7) = 16\n') && echo "fannkuch native OK"
echo

echo "--- perf ---"
time_case "nbody 5M interp" $B nbody.janet 5000000
time_case "nbody 5M native" /tmp/bg-nbody 5000000
time_case "spectral 2000 interp" $B spectralnorm.janet 2000
time_case "spectral 2000 native" /tmp/bg-spectral 2000
time_case "fannkuch 10 interp" $B fannkuch.janet 10
time_case "fannkuch 10 native" /tmp/bg-fannkuch 10
