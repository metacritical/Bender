# Post-Phase-4 gated execution

Execution of `docs/internals/post-phase4-execution-handoff.md`, pinned at `3ae1e1a858da358a9687a6d604b5832b0fb7fff8`. This directory records new work, not a rerun of completed Phase4. See `STATUS.md` for current gates; pending phases are not claimed complete.

## Baseline

- Fresh detached baseline `/tmp/bender-next-baseline-3ae1e1a8`.
- Fresh candidate branch `post-phase4-candidate-3ae1e1a8`, worktree `/tmp/bender-next-candidate-3ae1e1a8`.
- Apple Clang21.0.0, arm64-apple-darwin25.6.0, system make, Python resolved through the user's pyenv shim; exact preflight output retained.
- `make -j4 NATIVE_EXECUTORS=1 build/bender build/libbender.a`: exit0.
- `BENDER_LANES=0 BENDER_NO_PROFILE=1 ./build/bender test/suite-proc-spawn-return.ben`: exit0 under10s outer timeout, PASSED.
- Runtime SHA256 `71d75d31b8581249ba6da93278c80e0db84e309e70575fcf910f28d352340ba7`.
- Static library SHA256 `a49d3ce5b2363c008f24e94b8480aa6124223e57a5fa2a68dbda21b4e38d546c`.

`phase0-baseline.tar.gz`:7 raw records/logs, each member byte-verified against originals. SHA256 `e08f635c73bf17117db95bab34aea80ae1ac3fd27ef4d7c095c7d84badc19b8b`. Includes exact cwd/argv/environment overrides/status; full preflight, build and canonical-regression stdout/stderr; binary manifest. No portable benchmark or optimization conclusion follows from this baseline smoke.

Raw incremental evidence lives at `/tmp/bender-next-evidence-3ae1e1a8`; artifacts are versioned/archived as gates finish. Instrumentation and timings remain separate. Unrelated untracked files are preserved.
