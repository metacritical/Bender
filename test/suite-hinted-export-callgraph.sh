#!/bin/sh
set -eu
: "${JANET_AOT_TEST_ROOT:?set a private scratch root}"
: "${XDG_CACHE_HOME:?set a private cache root}"
scratch=$(mktemp -d "$JANET_AOT_TEST_ROOT/janet-hinted-callgraph-XXXXXXXX")
fixture=test/fixtures/hinted-export
export JANET_NO_PROFILE=1
./build/bender tools/callgraph.janet "$fixture/caller.janet" > "$scratch/caller" 2>&1
grep -Eq '^def [0-9]+ hot ' "$scratch/caller"
! grep -Eq '^def [0-9]+ (cold|secret) ' "$scratch/caller"
hot=$(awk '$1 == "def" && $3 == "hot" {print $2}' "$scratch/caller")
grep -q "^hint $hot " "$scratch/caller"
grep -q "^calls 1 $hot$" "$scratch/caller"
! grep -q "^contains 2 $hot$" "$scratch/caller"
./build/bender tools/callgraph.janet "$fixture/prefix.janet" > "$scratch/prefix" 2>&1
grep -q '^calls 1 3$' "$scratch/prefix"
./build/bender tools/callgraph.janet "$fixture/direct.janet" > "$scratch/direct" 2>&1
grep -q '^def 1 direct ' "$scratch/direct"
grep -q '^hint 1 long -> long$' "$scratch/direct"
! grep -q '^contains 0 1$' "$scratch/direct"

./build/bender tools/callgraph.janet "$fixture/duplicate.janet" > "$scratch/duplicate" 2>&1
grep -q '^calls 1 3$' "$scratch/duplicate"
! grep -q '^calls 1 5$' "$scratch/duplicate"
./build/bender tools/callgraph.janet "$fixture/alias-collision.janet" > "$scratch/alias" 2>&1
grep -q '^ambiguous 1 hot$' "$scratch/alias"
! grep -q '^calls 1 3$' "$scratch/alias"
./build/bender tools/callgraph.janet "$fixture/malformed.janet" > "$scratch/malformed" 2>&1
grep -q 'malformed hinted export:.*broken' "$scratch/malformed"
! grep -Eq '^hint [0-9]+ ' "$scratch/malformed"
./build/bender tools/callgraph.janet "$fixture/name-collision.janet" > "$scratch/collision" 2>&1
grep -q 'analysis retention refused for hinted def hot' "$scratch/collision"
grep -q 'hinted export not registered unambiguously:.*hot' "$scratch/collision"
! grep -Eq '^hint [0-9]+ ' "$scratch/collision"

./build/bender tools/callgraph.janet "$fixture/profile-caller.janet" \
  "$fixture/profile-duplicate.janet" --profile "$fixture/profile-positive.jdn" > "$scratch/profile" 2>&1
grep -q '^def 4 hot ' "$scratch/profile"
! grep -Eq '^def [0-9]+ (cold|secret) ' "$scratch/profile"
grep -q '^calls 1 4$' "$scratch/profile"
! grep -q '^contains 3 4$' "$scratch/profile"
./build/bender tools/callgraph.janet "$fixture/profile-module.janet" \
  --profile "$fixture/profile-merged.jdn" > "$scratch/profile-merged" 2>&1
grep -q '^def 1 hot ' "$scratch/profile-merged"
grep -q '^profile-seed hot 0 number home,worker$' "$scratch/profile-merged"
./build/bender tools/callgraph.janet "$fixture/profile-module.janet" \
  --profile "$fixture/profile-merged-conflict.jdn" > "$scratch/profile-merged-conflict" 2>&1
grep -q '^def 1 hot ' "$scratch/profile-merged-conflict"
grep -q '^profile-conflict hot 0 number:home string:worker$' "$scratch/profile-merged-conflict"
./build/bender tools/callgraph.janet "$fixture/profile-module.janet" \
  --profile "$fixture/profile-ambiguous.jdn" > "$scratch/profile-ambiguous" 2>&1
grep -q 'profile retention refused for .*hot: duplicate identity or definition' "$scratch/profile-ambiguous"
! grep -q '^def 1 hot ' "$scratch/profile-ambiguous"
./build/bender tools/callgraph.janet "$fixture/profile-module.janet" \
  --profile "$fixture/profile-wrong-line.jdn" > "$scratch/profile-wrong-line" 2>&1
grep -q 'profiled def hot: source line does not match child identity' "$scratch/profile-wrong-line"
! grep -q '^def 1 hot ' "$scratch/profile-wrong-line"
./build/bender tools/callgraph.janet "$fixture/profile-same-name.janet" \
  --profile "$fixture/profile-same-name.jdn" > "$scratch/profile-same-name" 2>&1
grep -q 'profile retention refused for .*hot: duplicate identity or definition' "$scratch/profile-same-name"
! grep -Eq '^def [0-9]+ hot ' "$scratch/profile-same-name"
printf 'hinted callgraph: qualified resolution, collision refusal, orphan edges, malformed warning OK (%s)\n' "$scratch"
