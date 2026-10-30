# Durable frozen-workload evidence for current-game AOT/PGO (66192cee)

## Objective
Make the benchmark workload independent of `/tmp`: copy the verified frozen game
outside `/tmp`, package it as an immutable archive, record full identity
validation as committed evidence, and commit atomically without touching
unrelated work.

## Provenance of the workload
- Origin: `/Users/pankajdoharey/Development/Projects/ML/trinecraft`, frozen at
  HEAD `2c68438`, describe `2c68438-dirty`, 552 commits, **no remotes** — the
  freeze is the only reproducibility mechanism.
- Reference commit: `66192cee`, evidence dir
  `bench/results/current-game-6a4f008b-c07042a7`.
- Frozen bytes live at `/tmp/janet-current-game-ab-evidence/game-source`,
  pinned by three committed manifests: `SOURCE-MANIFEST.txt` (`dfc64b0e`),
  `runtime-source-manifest.json` (`8d3fc0c3`), `saved-world-manifest.json`
  (`2bb4e82a`).

## Steps and their gates
1. **Re-confirm frozen == committed manifests.** Block on any mismatch.
   Gate: 228/228 runtime, 214/214 world, source-hash unchanged.
2. **Copy to a NEW destination only**:
   `build/current-game-aot-pgo-66192cee/game-source`. Never overwrite; never
   copy the live worktree (it has drifted in 8 recorded files).
3. **Validate the copy** against all three manifests and `diff -r` it against
   the frozen original. Gate: 442 files, 61,328,480 bytes, trees identical.
4. **Package** as `bench/results/current-game-aot-pgo-66192cee/frozen-game-source-66192cee.tar.gz`.
   Must be byte-reproducible (bsdtar lacks `--sort/--mtime/--owner`, so use
   python `tarfile` + `gzip` with fixed mtime, uid/gid 0, sorted paths).
5. **Roundtrip verify**: extract to scratch, re-hash against both manifests,
   `diff -r` the extracted tree. Gate: byte-identical, 0 mismatches.
6. **Record** `bench/results/current-game-aot-pgo-66192cee/workload-verification.json`
   with runtime+world identities, original/reference hashes, paths, provenance,
   archive reproducibility, and an explicit live-source disclaimer.
7. **Commit atomically**, staging only my two artifacts.

## Non-negotiables
- No game run, no build, no benchmark.
- Do not interrupt other agents' processes.
- Preserve unrelated work: `M Makefile`, `?? grammar/`,
  `?? tools/linguist_grammar.janet`, and other agents' in-flight files.
- `build/` is gitignored: the copy is local durability, the tarball is the
  committed durability.

## Risks
- `/tmp` purge would destroy the only copy of the game bytes; the archive and
  the `build/` copy are the mitigations.
- `build/` being gitignored means the committed tarball, not the directory,
  is what survives a fresh clone.
- Determinism is produced in-process; a different gzip implementation would
  change the archive sha. The rebuild recipe is recorded for that reason.