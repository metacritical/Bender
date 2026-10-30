# GC major-pressure regression evidence

See `docs/internals/game-gc-pressure-correction.md` for corrected attribution and results. Runtime baseline739994d4; historical matched endpoint455d5c4f; game6db06bc; fix7a7d7623. Both profiled endpoints exceed1GiB, so no post455first-bad claim is made. Priority-only candidate is retained as failed remediation; survivor-budget candidate completes6000/12000frames below231344KiB sampledRSS.

`evidence.tar.gz`:77 raw JSON/runner/build/test/stdout/stderr files, every member byte-verified. SHA256 `a87da3ea2ca500a6c71c48af25326b6c67e366cbad1299717b10073c70e8ffca`. `raw-manifest.json` contains per-memberSHA256. No executable binaries included. Raw path `/tmp/bender-forward-455-evidence/` retains frozen game source and isolated build artifacts; runner names `endpoints.py`, `fixed-endpoints.py`, `diagnostic-endpoint.py`, `survivor-endpoint.py`, `long-fixed.py` document resource caps and profiling environment.

Expected assertion failures in fixture-ablation-results.json are deliberate baseline discrimination, not green baseline suites. Final current regression runner succeeds. Prior causal page-size claims are superseded, not validated by this fix.
