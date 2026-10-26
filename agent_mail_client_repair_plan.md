# Repair installed Agent Mail client artifact without restarting
1. Confirm source-to-bundle mismatch and determine the client build entry that actually populates browser `lib/client.js` (not only `lib/types/client/index.js`).
2. Rebuild that client artifact and Web shell if needed; verify generated gateway/authorized UI signatures and focused tests.
3. Restage and smoke-test the signed macOS candidate; install it by offline bundle swap preserving rollback without stopping or restarting running Anton.
4. Verify installed client bundle, signature, preset, and running GUI continuity; leave restart and live pairing to the user. Preserve unrelated plans.