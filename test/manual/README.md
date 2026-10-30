# Manual process/pool diagnostic fixtures

These files were recovered from untracked `test/tmp-*.ben` work on
2026-10-18 and organized by purpose. They are **not part of the automated
suite** and do not establish a feature-completion or performance claim.

- `pool-lifecycle.ben`: create a two-lane pool, call one process, stop it.
- `pool-reinitialization.ben`: round-robin placement, stop the first pool,
  create a second pool, call it and stop it. Both scripts import the same
  helpers/examples as the original probes; only relative paths changed.
- `archive/proc-cut.ben.txt` and `archive/proc-prefix.ben.txt`: preserved
  diagnostic prefixes of earlier process-suite work. Their original content
  is unchanged, including original relative import paths and lack of an
  `end-suite` footer. They are archival text, not runnable regression suites.
  Use maintained `test/suite-proc.ben` and `test/suite-pool.ben` for suite
  coverage instead of interpreting these snapshots as passing tests.

Run manual scripts from the repository root with a private cache and an
external timeout, for example:

```sh
BENDER_NO_PROFILE=1 XDG_CACHE_HOME="$PRIVATE_CACHE" \
  timeout 30 ./build/bender test/manual/pool-lifecycle.ben
```

The sequence probe creates sleeping processes and relies on `pool-stop` to
cancel them; a timeout or hang must be reported, not treated as success.

Validation on 2026-10-18 with the rebuilt `1.43.0-dev-5180da8a` compiler:
both manual scripts exited 0 under a 35-second timeout. These are smoke
checks, not a full process-runtime regression run. The two archived text
snapshots retain their original SHA-256 digests:

```
a1459954ebdf23d07f30abbbd28175601ea4266e0153d304ede2fa3244515fa3  proc-cut.ben.txt
b5e718d39166060fcade48c5c1aa3f55b0092c38bf5b71b0c999e01f18c1a94e  proc-prefix.ben.txt
```
