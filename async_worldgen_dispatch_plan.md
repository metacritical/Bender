# Async worldgen dispatch failure completion

1. Inspect the scheduler's launch, completion, polling, draining, priority, and stale-generation paths in the owned file.
2. On protected `proc/send` failure, retain the existing worker-index advance but attach a contextual error result and enqueue the request as completed rather than synchronous running work. Keep ordinary fallback exclusive to explicit scheduler configuration.
3. Run a narrow Bender interpreter smoke covering dispatch failure, no-running drain, and delivery; inspect the owned-file diff. Do not edit tests, build native/game targets, or commit.