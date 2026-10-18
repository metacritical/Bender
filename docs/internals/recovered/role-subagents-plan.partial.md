# Add role-specialised subagent rows to the `code` preset

> Recovered partial artifact, archived 2026-10-18. This describes an unrelated
> project and is not an approved Janet implementation plan. The recovery note
> below records missing content; the complete original cannot be reconstructed
> from this fragment. Preserved rather than discarded during repository cleanup.

## Finding that shapes the whole change
The mechanism is **already fully implemented** in `tool-subagent`:
- `Config.persona?: string` and `Config.toolFilter?: {allow?, deny?}`
  (`packages/subagent/tool-subagent/src/index.ts:55-69`, schema at `:103-108`)
- Both in-process providers declare the capabilities:
  `subagent-spawn-in-process` and `subagent-fork-in-process` →
  `{ outputSchema: true, depthLimit: true, toolFilter: true, persona: true }`
- `applyChildComposition` already applies them per child, scoped so they are
  invisible to parent and siblings; `toolFilter` **intersects** (a child can
  only ever hold fewer tools).

So **no TypeScript change is needed.** This is a composition (YAML) change —
materially lower risk, and reversible by deleting rows.

Also already true: `backgroundMode: continuable` is **already set** on both
existing rows in the `code` preset (`agent.cordis.yml:192, 199`). The
"reliability switch" we identified is on for this preset. Work there is
limited to verifying other presets and documenting *why* it matters.

## Changes

1. **Verify `backgroundMode` across all shipped presets.** Add
   `continuable` where a preset mounts delegation but omits it. Document the
   consequence inline (omitting it silently drops the standing
   "Delegation is the DEFAULT" system-prompt section, because the section is
   registered only `if (backgroundEnabled && continuable)`).

<!--
RECOVERY NOTE (added by a different agent working in this same shared
repo on an unrelated Bender/Janet task): this file was accidentally
overwritten via a plan-recording tool call that defaulted to this
path instead of a dedicated one. The content above is restored from a
partial capture earlier in that agent's own conversation transcript
(a `git diff implementation_plan.md | head -40`-equivalent read taken
before the overwrite) -- it covers only the first ~40 lines of
whatever was actually in this file at the time; anything beyond that
point (the diff stat suggested there was more content) could not be
recovered and is lost. Apologies for the disruption -- please verify
this matches your intended state and reconstruct anything missing
from your own records if needed.
-->
