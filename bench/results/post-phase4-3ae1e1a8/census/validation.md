# Pinned baseline executor/census fixture validation

Source3ae1e1a8, isolated `/tmp/janet-next-baseline-3ae1e1a8`; no tracked source changed.

`make NATIVE_EXECUTORS=1 test-executors` initial exit2: AOT compile could not locate janetconf.h. Earlier mode-refresh and forced-quantum sections had passed, but the whole initial command is a failure, not green. The exact same suite rerun with the worktree src/conf in CPATH exited0. Exact argv/environment/stdout/stderr/status retained; SDK selection/provenance in command records. No semantic patch was made to turn a failure green.

Successful retry includes plain/native/plain registration-mode checks, fixed-seed forced-quantum replay, numeric/nested and boundaries, native image parity, continuation on/off identity, opt-in partial per-PC census, budget1/2, portable/census/disabled/sanitized executor matrix and boundary GC/resume/marshal checks. Output explicitly reports fused numeric-sum original-PC2..6 coverage=partial gate=unavailable. Synthetic parity is not actual TrineXraft/ttfx forced-interrupt proof.

The mode-refresh suite leaves the shell in plain mode; final `make NATIVE_EXECUTORS=1 build/bender` restore exited0 and runtime SHA256 returned to `71d75d31b8581249ba6da93278c80e0db84e309e70575fcf910f28d352340ba7`. Tracked baseline remains clean.

`baseline-validation.tar.gz`:45 members, every member byte-verified against raw originals, including33 retained fixture logs/output (not generated executable binaries). SHA256 `e51d51b07c88465e5fe66a5ccf54a827ebe9b7a62cd585ba74e5dd4d8fda4549`.

## Gate status

Fixture validation passes, **inclusive opportunity gate remains unavailable**. The successful matrix does not close unclassified production retirement paths, source mapping/reporting, complete numerical producer+transport classification, per-PC type/liveness or real-workload interruption parity. See `audit.md`; do not derive a numeric candidate percentage from this fixture run.
