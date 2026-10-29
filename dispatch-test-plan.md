# Failed dispatch regression test

1. Inspect the isolated scheduler test and current dispatch contract.
2. Add a deterministic test limited to `test/async-worldgen-test.janet`: temporarily invalidate the worker PID during submit, restore it, assert an error-only completed result and no synchronous fallback, check cancellation, then clean teardown.
3. Run the Janet interpreter test only, inspect the scoped diff, and report without modifying source or committing.