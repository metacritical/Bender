# Model-facing agent queue tools
1. Inspect adjacent tool packages and test harness conventions; establish service call contract with parent.
2. Create only `packages/interaction/tool-agent-queue` with five tools, exact caller identity passed from execution context, bounded argument schemas and readable output.
3. Add focused tests covering registration, caller identity, routing, validation, and no-wakeup behavior; run package checks, review scoped diff, commit atomic tool package, and report wiring requirements.