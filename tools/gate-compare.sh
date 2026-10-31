#!/bin/sh
# Paired suite runner: run every suite under two binaries and print the
# result side by side, so a failure is attributable to the change rather
# than to a pre-existing condition. A suite that fails identically under
# both arms is reported as UNCHANGED, never as a pass.
#
#   tools/gate-compare.sh BASELINE_BIN CANDIDATE_BIN [suite ...]

set -u
BASE=$1; shift
CAND=$1; shift
if [ $# -eq 0 ]; then set -- test/suite*.ben; fi

run_one() {
    bin=$1; suite=$2
    out=$(BENDER_NO_PROFILE=1 "$bin" "$suite" 2>&1)
    rc=$?
    line=$(printf '%s\n' "$out" | grep -o '[0-9]* of [0-9]* tests passed' | tail -1)
    [ -z "$line" ] && line="no-summary"
    printf '%s|%s' "$line" "$rc"
}

printf '%-44s %-26s %-26s %s\n' SUITE BASELINE CANDIDATE VERDICT
status=0
for suite in "$@"; do
    [ -f "$suite" ] || continue
    b=$(run_one "$BASE" "$suite"); br=${b#*|}; bs=${b%|*}
    c=$(run_one "$CAND" "$suite"); cr=${c#*|}; cs=${c%|*}
    if [ "$bs" = "$cs" ] && [ "$br" = "$cr" ]; then
        if [ "$br" = "0" ]; then v="SAME (ok)"; else v="UNCHANGED (both fail rc=$br)"; fi
    else
        v="*** DIFFERS ***"; status=1
    fi
    printf '%-44s %-26s %-26s %s\n' "$(basename "$suite")" "$bs rc=$br" "$cs rc=$cr" "$v"
done
exit $status
