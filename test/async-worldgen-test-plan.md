# Async worldgen interpreter tests
1. Inspect the existing isolated scheduler tests and supported Janet/Bender syntax.
2. Add deterministic coverage for full-queue nil without sequence loss and invalid finite-integral request fields before launch; retain priority and cancellation coverage.
3. Run the interpreter-only test when the source update is available, then report outcomes without committing or building native/game artifacts.