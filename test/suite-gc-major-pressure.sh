#!/bin/sh
# GC threshold fixtures, linked against each tree's ALREADY built static
# library. Compiles fixtures only; never rebuilds a runtime.
#
#  1) major-pressure  -- check ORDER: baseline starves, candidate fires
#  2) interval-feedback -- threshold MOVES with block_count: passes on both,
#     because it documents a defect the priority patch does not address.
#  3) budget-current-blocks -- the real fix: budget from last major's
#     survivors. RED on a tree that uses the current block count, GREEN on the
#     survivor-budget tree. Set ALLOW_UNFIXED_BUDGET=1 to tolerate RED.
# RIPWIRE_TEST_DEPS: src/core/vm.c src/core/gc.c test/c/test-gc-major-pressure.c test/c/test-gc-interval-feedback.c test/c/test-gc-budget-current-blocks.c
set -eu
: "${JANET_AOT_TEST_ROOT:?set a private scratch root}"
here=$(cd "$(dirname "$0")/.." && pwd)
root=$(mktemp -d "$JANET_AOT_TEST_ROOT/gc-major-XXXXXXXX")
base=${BASELINE_TREE:-/tmp/janet-forward-latest-739994d4}
cand=${CANDIDATE_TREE:-/tmp/janet-forward-survivor-739994d4}

# build_and_run <tree> <fixture.c> <name>; 0 = passed, 3 = failed, else build
build_and_run() {
    tree=$1; fix=$2; name=$3
    if [ ! -f "$tree/build/libjanet.a" ]; then
        echo "SKIP $name: no archive at $tree"; return 1
    fi
    ${CC:-cc} -O2 -g -DJANET_NATIVE_EXECUTORS \
        -I"$tree/src/include" -I"$tree/src/conf" -I"$tree/src/core" \
        "$here/test/c/$fix" "$tree/build/libjanet.a" \
        -lm -lpthread -ldl -o "$root/$name" > "$root/$name.log" 2>&1 || {
        echo "BUILD-FAIL $name"; sed -n '1,12p' "$root/$name.log"; return 2; }
    if "$root/$name"; then echo "PASS $name"; return 0; fi
    echo "FAIL $name"; return 3
}

s_base=0; s_cand=0
build_and_run "$base" test-gc-major-pressure.c baseline || s_base=$?
build_and_run "$cand" test-gc-major-pressure.c candidate || s_cand=$?

echo "--- order fixture: baseline=$s_base candidate=$s_cand"
# Candidate must pass; baseline failure IS the proof, so it is not an error.
[ "$s_cand" -eq 0 ] || { echo "candidate did not pass"; exit 1; }
[ "$s_base" -ne 2 ] || { echo "baseline fixture did not build"; exit 1; }

# Threshold feedback: passes on BOTH trees. It is documentation of a separate
# defect (a moving limit), not a regression gate on the priority patch.
s_ifb_b=0; s_ifb_c=0
build_and_run "$base" test-gc-interval-feedback.c ifb-baseline || s_ifb_b=$?
build_and_run "$cand" test-gc-interval-feedback.c ifb-candidate || s_ifb_c=$?
echo "--- feedback fixture: baseline=$s_ifb_b candidate=$s_ifb_c"
[ "$s_ifb_b" -eq 0 ] && [ "$s_ifb_c" -eq 0 ] || {
    echo "threshold feedback behaved unexpectedly"; exit 1; }

# Survivor-budget gate: RED on a current-block-count tree, GREEN once the
# survivor budget is present. Only the candidate arm gates the run.
s_bud_b=0; s_bud_c=0
build_and_run "$base" test-gc-budget-current-blocks.c budget-baseline || s_bud_b=$?
build_and_run "$cand" test-gc-budget-current-blocks.c budget-candidate || s_bud_c=$?
echo "--- survivor-budget fixture: baseline=$s_bud_b candidate=$s_bud_c"
if [ "$s_bud_c" -ne 0 ]; then
    echo "survivor budget missing on the candidate tree"
    [ "${ALLOW_UNFIXED_BUDGET:-0}" = "1" ] || exit 1
fi
