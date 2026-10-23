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

## Typed pilot verdict (ML/ttfx 0ffcaf1, `docs/typed-pilot.md`) — MECHANISM YES, SPEEDUP NO
Hints + slot-pressure reduction DID convert refused functions into emitted kernels (9 → 11 kernels; `radial_fraction`, `hue_to_rgb`), 37/37 byte-exact, both sides 81/81 bound. But: geomean **0.9917**, **no per-effect ratio clears ±3% in both runs**, and **attribution FAILS the control test** — the two effects calling NEITHER new kernel averaged 0.9826, *faster* than the eight that do (0.9940). A real kernel win must order those the other way. The isolated 3.5× for `radial-fraction` reproduces; it is ~1.6% of `colorshift`'s frame work. **Two numeric leaves are not enough: the hint-addressable population and the hot population barely intersect.**

### New blockers found (beyond the audit's four hard limits)
- **(5) Per-slot non-SSA lattice — largest practical blocker.** Blocks `xor64` (**399,197 calls, hottest function in the program**) and `or64` (159,674): loop-free, 2 params, non-recursive, yet `slot s2 mixes unanalyzable u64load/array`. Also forced full inlining of all ten locals in `radial-fraction`. Confirms roadmap rank 1 (CFG/SSA per-PC typing) as the top lever.
- **(6) Context-table parameters — ttfx's structural ceiling, a NEW rank 0.** `ctx-char-active?` (54,115 calls), `ctx-step-animation` (54,115), `ctx-anim` (44,904) all `mixes unanalyzable keyword/unknown`. `:ctx` is rejected by the closed vocabulary and `:table` does not unbox field reads, so **no hint can reach them** — only user types or field-level unboxing can.
- **(7) cfunction lowering gap**: `math/hypot` is not lowered while `sqrt`/`pow`/`floor`/`cos`/`sin` are.

### janet-side bugs to fix (queued; janet currently owned by 639be9a5)
1. **A hint on `defn-` is SILENTLY DROPPED** — no `hint` line, no `hint-bad`; inference then widens on its own. The one place a hint fails OPEN in an otherwise fail-closed design. HIGH priority.
2. Hints are invisible through a dynamic `require` (bodies compile, hints dropped, no diagnostic).
3. **PGO call-site evidence is keyed by file+line**, so moving a function silently loses a previously-proven kernel inference — this makes naive module-level A/B tests dishonest by default (it cost the pilot `color_clamp` until a hint restored it).
4. Lower `math/hypot`.

## Facts for any agent
- `make NATIVE_EXECUTORS=1`; export `JANET_NATIVE_EXECUTORS=1`. Plain `make` proves nothing. `make test` globs `test/suite*.janet` and never runs the shell executor suite — use `make test-executors`.
- Binary is `build/bender`, NOT `build/janet`. The 37-effect gate lives in ML/ttfx, not janet.
- PGO cache keyed by output basename. Always explicit `--seed`. Never combine `--bench` with `--dump-frames` (bench wins, stdout empty, parity vacuous).
- ONE OWNER PER REPO. A "stopped"/"failed" subagent may still be alive — verify before duplicating (a ~40-min gate run was duplicated). `job_output` clips `timeout_ms`; prefer short polls.
- Subagents: default route `GPT-6-Sol`/low is healthy; pass `reasoningEffort: high` for design/adversarial work. Tell them to put their own plans in gitignored `tmp/`, NOT `implementation_plan.md`.
- Honest numbers only: a null result is a null result; a regression is a regression.
## Loop stop-gate verdict (ML/ttfx 1df191e, `docs/loop-stopgate.md`)
**Gate GO on occupancy, implementation NO-GO.** Keep these apart: Tier-B executor time as a share of `--bench` playback is 17.6–48.2% (decrypt 48.2, beams 40.5, wipe 39.8, smoke 35.4, fireworks 34.0, matrix 17.6), which passes the ~5% gate — but the dominant occupant is NOT a shape the proposed counted-loop/N-accumulator recognizer could match, so DO NOT build it. Best honest candidates are `bitlen` (~5.9% decrypt, ~0.3% matrix) and `geom-coord-on-bezier` (~6.4% fireworks only); these already run natively, so the marginal gain is fusion/unboxing alone — unmeasured and likely under the ±3% resolution.
**Attribution caveat (quote it whenever citing these numbers):** the timed numerator includes construction while the `--bench` denominator covers playback, so the 17.6–48.2% ratios are UPPER BOUNDS, not exclusive playback shares and not expected savings. The tighter measure — whole-process CPU — puts Tier-B occupancy at **~17.4% (matrix) and ~28–33% (the other five effects)**; prefer these. Method: measurement-only generated-C copy, 1-in-32 sampled `mach_absolute_time`, 3 rounds, seed 42, pinned compiler d5a24e5d, corroborated by `sample <pid> 2` (top-of-stack counts reported raw, deliberately not converted to percentages).
**Counter audit — the bound executors run CLEAN: zero fallbacks everywhere.** decrypt 12/81 active (1.63M entries, 2.86M backedges, 47.1M native ops, 0.77M VM exits); fireworks 15/81 (1.14M entries, 3.54M backedges, 57.2M ops, 0.50M exits); matrix 13/81 (~12.5M entries, ~1.20B ops, ~9.78M exits). Only 12–15 of 81 executors are active per effect. So the deficit is COVERAGE — what never becomes native — not bailout from what does.

### The real target this measurement found
**`ctx-frame-string` is the single largest native-executor occupant: 17–36% of playback** (≈35–36% decrypt, ≈34% beams/wipe, ≈27% smoke, ≈24% fireworks, ≈17% matrix). The frame serializer — not motion, not arithmetic — is the biggest addressable block. Its shape is nested mutable containers/buffers with many VM boundaries, and the blocker is exactly the **construction poison** (`mkbuf/mkstr/mkstu/mktab` → `:unknown`, fail-closed), i.e. roadmap rank 3 (narrowly-rooted native string/buffer constructors), which this evidence argues should move UP. Counterweight: a prior expanded-executor A/B on this same function LOST at 0.948×, so the naive approach is already disproven.

### Census correction — supersedes every earlier figure
Measured by parsing `def ID ... arity N` from an actual successful PGO build and joining to the exact refusal IDs: **169 Tier-A loop refusals = 13 zero-arity + 156 positive-arity; 81 emitted in Tier B (all positive-arity); 75 positive-arity NOT emitted**, largely because `tools/loop-executor.janet` excludes closures (non-empty environments) — a distinct, previously unnamed coverage gap. This DISPROVES the "115 zero-param / at most ~54 positive" figure asserted independently by two earlier scouts and still repeated in `aot-loop-codegen-feasibility.md` and `typed-unboxed-roadmap.md`; trust the parsed-descriptor method over those documents.
