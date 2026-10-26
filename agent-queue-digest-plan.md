# Safe natural-step queue digest
1. Inspect pre-step waterfall ordering, rejection/cancellation semantics, and service read/ack behavior.
2. If safe without auto-ack, add a bounded per-agent step digest in tool-agent-queue by modifying accepted pre-step decisions only, with replay semantics explicit.
3. Test acceptance, rejected and cancelled proposals, and no wakeup; run focused checks and commit only the tool package. If safe semantics cannot be guaranteed, report the precise blocker without editing service or composition.