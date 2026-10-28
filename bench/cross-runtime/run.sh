#!/bin/sh
# bench/cross-runtime/run.sh -- build and run the cross-runtime comparison.
#
#   ./bench/cross-runtime/run.sh [N]        # default N=1000000
#
# Prints two tables:
#   1. trampoline wave  -- spawn + per-hop cost (a dependency chain; cores
#                          cannot help it)
#   2. live-process memory -- KB per process that STAYS RESIDENT
#
# Runtimes are skipped with a note if their toolchain is absent, so this runs
# on any machine.
#
# Ground rules this script enforces, learned the hard way:
#   - every run must report result == n, or it is marked WRONG
#   - the wave is a chain: do NOT read it as a parallel speedup
#   - memory is measured with processes ALIVE, never with ones that exit
#   - timings on a loaded machine are noise; check swap before believing them
set -u

N="${1:-1000000}"
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
JANET_BIN="${JANET_BIN:-$REPO/build/bender}"
TMP="${TMPDIR:-/tmp}/janet-cross-runtime.$$"
mkdir -p "$TMP"
trap 'rm -rf "$TMP"' EXIT

have() { command -v "$1" >/dev/null 2>&1; }

printf 'swap: %s\n' "$(sysctl -n vm.swapusage 2>/dev/null || echo 'n/a')"
printf 'load: %s\n\n' "$(uptime | sed 's/.*load averages*: //')"

# ---------------------------------------------------------------- wave ------
printf '%-10s %10s %10s %10s   %s\n' runtime procs spawn_ms wave_ms result
printf '%-10s %10s %10s %10s   %s\n' ------- ----- -------- ------- ------

run_wave_janet() {
    [ -x "$JANET_BIN" ] || { printf '%-10s %s\n' janet "skip: $JANET_BIN not built"; return; }
    # LANES ARE REQUIRED for the wave. On a single VM it is quadratic in chain
    # length (1000->95ms, 2000->377ms, 4000->1526ms) because every proc shares
    # one GC arena; across lanes each VM owns its arena and it is linear
    # (4000: 1506ms -> 27ms). Default 11 lanes, override with WAVE_LANES.
    out=$(JANET_NO_PROFILE=1 JANET_LANES="${WAVE_LANES:-11}" "$JANET_BIN" \
              "$HERE/wave.janet" "$N" 2>/dev/null | tr -d '\r')
    printf '%s\n' "$out" | awk -v rt=janet '{
        sp=""; wv=""; pr=""; rs=""
        for (i = 1; i <= NF; i++) {
            split($i, kv, "=")
            if (kv[1] == "procs")    pr = kv[2]
            if (kv[1] == "spawn_ms") sp = kv[2]
            if (kv[1] == "wave_ms")  wv = kv[2]
            if (kv[1] == "result")   rs = kv[2]
        }
        verdict = (rs == pr) ? "OK" : "WRONG"
        printf "%-10s %10s %10s %10s   %s %s\n", rt, pr, sp, wv, rs, verdict
    }'
}

run_wave_erlang() {
    have erlc || { printf '%-10s %s\n' erlang "skip: erlc not found"; return; }
    have erl  || { printf '%-10s %s\n' erlang "skip: erl not found"; return; }
    (cd "$HERE" && erlc -o "$TMP" wave.erl >/dev/null 2>&1) || {
        printf '%-10s %s\n' erlang "skip: wave.erl did not compile"; return; }
    out=$(erl -noshell +P 8000000 -pa "$TMP" \
              -eval "wave:main([\"$N\"]),halt()." 2>/dev/null | tr -d '\r')
    printf '%s\n' "$out" | awk -v rt=erlang '{
        sp=""; wv=""; pr=""; rs=""
        for (i = 1; i <= NF; i++) {
            split($i, kv, "=")
            if (kv[1] == "procs")    pr = kv[2]
            if (kv[1] == "spawn_ms") sp = kv[2]
            if (kv[1] == "wave_ms")  wv = kv[2]
            if (kv[1] == "result")   rs = kv[2]
        }
        if (pr == "") { printf "%-10s %s\n", rt, "skip: run failed"; exit }
        verdict = (rs == pr) ? "OK" : "WRONG"
        printf "%-10s %10s %10s %10s   %s %s\n", rt, pr, sp, wv, rs, verdict
    }'
}

run_wave_go() {
    have go || { printf '%-10s %s\n' go "skip: go not found"; return; }
    (cd "$HERE" && go build -o "$TMP/wave-go" wave.go) >/dev/null 2>&1 || {
        printf '%-10s %s\n' go "skip: wave.go did not build"; return; }
    out=$("$TMP/wave-go" "$N" 2>/dev/null | tr -d '\r')
    printf '%s\n' "$out" | awk -v rt=go '{
        sp=""; wv=""; pr=""; rs=""
        for (i = 1; i <= NF; i++) {
            split($i, kv, "=")
            if (kv[1] == "procs")    pr = kv[2]
            if (kv[1] == "spawn_ms") sp = kv[2]
            if (kv[1] == "wave_ms")  wv = kv[2]
            if (kv[1] == "result")   rs = kv[2]
        }
        verdict = (rs == pr) ? "OK" : "WRONG"
        printf "%-10s %10s %10s %10s   %s %s\n", rt, pr, sp, wv, rs, verdict
    }'
}

run_wave_janet
run_wave_erlang
run_wave_go

# ------------------------------------------------------- live memory --------
printf '\n\nlive-process memory (processes RESIDENT, blocked on recv)\n'
printf '%-10s %10s %12s %12s\n' runtime procs rss_mb kb_per_proc
printf '%-10s %10s %12s %12s\n' ------- ----- ------ -----------

peak_rss() {
    # macOS /usr/bin/time -l reports MAXIMUM RESIDENT SET SIZE IN BYTES
    # (name notwithstanding). Convert to KB here so callers stay sane.
    /usr/bin/time -l "$@" 2>&1 >/dev/null |
        awk '/maximum resident/{printf "%d\n", $1 / 1024; exit}'
}

mem_janet() {
    [ -x "$JANET_BIN" ] || { printf '%-10s %s\n' janet "skip: no binary"; return; }
    kb=$(peak_rss env JANET_NO_PROFILE=1 JANET_LANES=0 "$JANET_BIN" \
                    "$HERE/memory.janet" "$N")
    [ -z "$kb" ] && { printf '%-10s %s\n' janet "skip: no rss"; return; }
    awk -v rt=janet -v n="$N" -v kb="$kb" 'BEGIN{
        printf "%-10s %10d %12.0f %12.2f\n", rt, n, kb/1024, kb/n }'
}

mem_erlang() {
    have erlc || return
    (cd "$HERE" && erlc -o "$TMP" memory.erl >/dev/null 2>&1) || return
    out=$(erl -noshell +P 8000000 +Q 10000000 -pa "$TMP" \
              -eval "memory:main([\"$N\"]),halt()." 2>/dev/null | tr -d '\r')
    printf '%s\n' "$out" | awk -v rt=erlang '{
        mb=""; pr=""; base=""
        for (i = 1; i <= NF; i++) {
            split($i, kv, "=")
            if (kv[1] == "live_procs") pr = kv[2]
            if (kv[1] == "mem_mb")     mb = kv[2]
            if (kv[1] == "base_mb")    base = kv[2]
        }
        if (pr == "") { printf "%-10s %s\n", rt, "skip: run failed"; exit }
        # erlang:memory(total) includes the VM baseline; report MARGINAL cost of
        # the live set, else small N is dominated by the runtime itself.
        if (base == "") base = 0
        marg = mb - base
        if (marg < 0) marg = 0
        printf "%-10s %10s %12s %12.2f\n", rt, pr, marg, (marg*1024)/pr
    }'
}

mem_janet
mem_erlang

printf '\nNOTE: the wave is a dependency chain -- it measures spawn and per-hop\n'
printf 'cost. Cores cannot speed it up, so a low number here is not "more\n'
printf 'parallel". Memory is the axis where nesting lanes/procs/fibers pays.\n'
