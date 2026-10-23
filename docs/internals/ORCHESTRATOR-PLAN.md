# Janet AOT Compiler — Coverage-First Strategy

CANONICAL PLAN. Lives here because repo-root `implementation_plan.md` is the default `plan_write` path and has been clobbered three times by subagents; ignore that file.

## North star
AOT VM compiler for games, no JIT (iOS/Switch), Lisp-family, 1–2 MB runtime — GOAL lineage. **Dynamic-first**: untyped code fast via inference/widening + PGO; types OPT-IN where a compiler report says they pay; mandatory static typing is a NON-GOAL. Baseline: interpreter 0.032× Rust; AOT+PGO release 0.47–0.48× LuaJIT; release `run_vm` share ≈42%. Target class IL2CPP/Static-Hermes (~2–3× native), not Rust parity.

## MEASUREMENT FLOOR — every claim must clear it
Executor-equal A/Bs with byte-identical generated C: **geomean 0.9992 (37 effects, per-effect 0.894–1.145) / 1.0025 (5 effects)** → **±3% per effect, ~0.1–0.3% geomean**. Gate `ML/ttfx/bench/validate-bender.py`; protocol `bench/bender-37-gate.md`; baselines `bench/baselines/`.

## Completed
- **Auto-PGO (Phases 1–3)**: sidecars carry replayable workload provenance; `--pgo-auto` merges/gates; `--release` consumes auto evidence; generated C byte-identical to explicit `--pgo`.
- **Stage 0 + 0.5** (ML/ttfx 3c9bcae): 37-effect gate; 368/368 runnable cases byte-exact; swarm = sole bare-protocol Rust divergence; CLI `flag-values` fix; executor equalization; noise floor frozen.
- **Continuation v1** (janet `ttfx-mixed-regions`): byte-exact, counter-identical (653,839 == 653,839; 90,041 chained edges) but a **small net regression** — chain benefit ≈0 (0.9996 toggled); always-on cost +0.6–1.0% (1.0064/1.0100). 10–25% estimate WITHDRAWN. Keep mechanism, do NOT default-enable.
- **Build opt-in** (d5a24e5d): plain `make` left the tier INERT. Use `make NATIVE_EXECUTORS=1` + `make test-executors`.
- **Loop feasibility** (40ee172e) and **annotation advisor spec** (cd371cab).

## Verdicts that redirect strategy
1. **Coverage, not dispatch**: 79% of refused call edges have a non-JANET_FUNCTION callee; 20% have no `native_code`. Nothing chains in the loop tier alone; leaf tier gated by `JANET_EXECUTOR_LEAF_MIN_CALLS` (unset).
2. **Tier-A loop veto STANDS.** Continuation is a call-edge mechanism; a Tier-A kernel has no fiber/PC to suspend into. And "loop codegen unreachable from --aot" was WRONG: `--emit-native-lib` emits TWO tiers; Tier B already ships a safe unboxed C `while` loop with in-loop `janet_native_poll`, entry guards, slot materialisation (incl. the comparison-result slot) and `NX_EXIT`, re-enterable via `switch (*pc - d->bytecode)`. Only raw-C-local loops are unreachable, correctly.
3. **Loop census inflated by veto ordering**: `loops` tested before the arity gate, so all 115 zero-param cases double-vetoed. 169 → ~54 → ~27 (~3.4%), under the floor; and those already run through Tier B, so the marginal win is per-call boxing, not per-iteration.

## In flight
- `639be9a5` (janet, sole mutator): `:calledges` + profile-narrowed emission (target: 1.0064–1.0100 → ≈1.000, keeping 90,041 edges) + `--pgo-auto`/`--release` descriptor bisect.
- `d4e64bfc` (ML/ttfx, sole mutator): typed pilot — typed variants as NEW files, kernel-count delta, bit-identical output, timing vs floor, blocked-refusal list.
- `71f8bc13` (read-only doc): typed/unboxed roadmap ranked by leverage.

## Queue (do not start where a repo is owned)
1. **Generalise Tier B `numeric-sum-pattern?`** (`tools/loop-executor.janet` ≈162) into a parameterised counted-loop / N-accumulator recognizer reusing existing guard/materialisation/NX_EXIT machinery — the loop win actually available (per-iteration). **STOP GATE**: measure ttfx runtime share of candidates from PGO evidence; under ~5% → publish negative result and stop.
2. **Leaf-tier defaults** (`JANET_EXECUTOR_LEAF_MIN_CALLS`) now that continuation can chain into it.
3. Typed/unboxed items per `71f8bc13` ranking (expect SSA/per-PC slot typing first — the per-slot lattice caps the 338 hint-addressable refusals).
4. **Forced-tiny-quantum test mode** — does not exist; without it incomplete materialisation and quantum starvation ship silently.
5. Cheap: `native.c` at -O3 links `libjanet.a` at -O2, no LTO.
6. Build the annotation advisor per `docs/internals/annotation-advisor-spec.md`.

## Parked
Continuation v2.2 (inline cache) / v3 (lazy frames) — the only paths attacking `funcframe`/`revalidate`. Packed animation state 1–3 (1–3%, at/below floor; value = clearing slots that block type proofs). Free win: dead `geom-lerp-point` call at ttfx `ctx-coord-on-line:289-292`. 26 Bender-vs-Rust engine divergences (app correctness). 67 gate-blocked cases = 256-colour palette gap at `src/color.janet:145` (deliberately unfixed — changes rendered colours). Serializer kernel DROPPED (construction poison + backward-jump veto; prior A/B 0.948×).

## Honest ceiling (from `typed-unboxed-roadmap.md`, 469c5260)
Amdahl on the ≈42% `run_vm` share: removing it ENTIRELY caps at **1.72×**; a realistic 25–50% reduction gives **1.12–1.27× total**. No roadmap item is a step change — the case for the typed path is cumulative coverage, and the 338 hint-addressable refusals are a CEILING, not a forecast. Ranked order: (0) safety/telemetry gate [prerequisite], (1) CFG/SSA per-PC typed value versions + separate emitted C temporaries, (2) scalar-pair return ABI then cross-function table-flat escape analysis (targets boxed motion coordinates), (3) narrowly-rooted native string/buffer constructors, (4) constant-offset ptr-add + guarded unsafe ops, (5) advisor, (6) zero-arg kernels, (7) build flags, (8) per-tick regions — rank 8 carries the highest disappointment risk absent allocation attribution.

**Queue item 5 is now a MEASURED NULL**: O2-vs-O3 runtime, 7 alternating interpreter microbenchmark runs, median 0.11s each, identical output — no credible O3 advantage. LTO/`-march` untested, deprioritised.

## Facts for any agent
- `make NATIVE_EXECUTORS=1`; export `JANET_NATIVE_EXECUTORS=1`. Plain `make` proves nothing. `make test` globs `test/suite*.janet` and never runs the shell executor suite — use `make test-executors`.
- Binary is `build/bender`, NOT `build/janet`. The 37-effect gate lives in ML/ttfx, not janet.
- PGO cache keyed by output basename. Always explicit `--seed`. Never combine `--bench` with `--dump-frames` (bench wins, stdout empty, parity vacuous).
- ONE OWNER PER REPO. A "stopped"/"failed" subagent may still be alive — verify before duplicating (a ~40-min gate run was duplicated). `job_output` clips `timeout_ms`; prefer short polls.
- Subagents: default route `GPT-6-Sol`/low is healthy; pass `reasoningEffort: high` for design/adversarial work. Tell them to put their own plans in gitignored `tmp/`, NOT `implementation_plan.md`.
- Honest numbers only: a null result is a null result; a regression is a regression.