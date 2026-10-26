# Verify unsubscribed direct queue delivery
1. Inspect current direct-account creation, read/ack, and existing focused coverage.
2. Add a targeted regression for direct send after topic unsubscribe, verifying read and ack without resubscribe and persistence across restart if needed.
3. Run focused tests/typecheck with bundled Node, commit only service-package changes, and report the precise guarantee.