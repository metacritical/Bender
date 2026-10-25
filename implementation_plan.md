# NOT THE PLAN — see docs/internals/ORCHESTRATOR-PLAN.md

This file is the default write target of the `plan_write` tool, so any agent
that records a plan overwrites it. It has been clobbered repeatedly and its
contents are ephemeral scratch, not project direction.

**The canonical plan is `docs/internals/ORCHESTRATOR-PLAN.md`** (committed, and
carrying the measurement rules, the evidence-ranked queue, and the retractions).

If you are an agent: write your own plan to the gitignored `tmp/` directory, not
here. If you find this file modified, `git checkout -- implementation_plan.md`
restores this notice; nothing of value is lost.
