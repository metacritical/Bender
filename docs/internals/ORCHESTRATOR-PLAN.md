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

## Continuation v2 — narrowed emission WORKS (branch ttfx-mixed-regions, 45bd0c66..d928fd33)
`:calledges` per-PC callee attribution + two-pass profile-narrowed emission. Cross-binary vs pre-change `66a5cf9a`, identical 96/96 executor sets, `REFUSED: none` both sides: **v1 1.0064/1.0100 → v2 0.9881/0.9834**; same-binary toggle 0.9996 → **0.9924**. Mechanism: emitted `janet_native_call_enter` sites fell from EVERY non-terminal call PC to **5**, chained edges held at **89,392/90,041 (99.3%)**, runtime refusals collapsed **563,564 → 2**. Parity 37/37 exact (368/368), counter identity 653,836 == 653,836, `make test` green both build modes, `test-executors` green, `suite-callgraph` 242/242, and R11 closed (a profile-free build emits ZERO call_enter, asserted in the suite).
**Honest bound (the implementer's own caveat — keep it):** 0.9834–0.9881 is MORE than narrowing alone can buy; the candidate still pays an unconditional `native_interp_calls++` in `JOP_CALL` plus trampoline indirection, so part of the margin is build-to-build layout. The defensible claim is **"at or below the noise floor"**, NOT "1.2% faster". The regression is gone; a win is not proven.
**Still opt-in twice:** `JANET_EXECUTOR_EDGE_MIN_CALLS` is unset by default, so a default build selects nothing and emits no attempt. Selection used ONE decrypt profile and chose `ctx-anim`/`scene-new`/`ctx-scene`/`ctx-add-frame` — NOT design §5's motion chain, which therefore remains unverified; a binarypath/rings/waves profile would select different edges.

## RETRACTION — the "--pgo-auto/--release descriptor bug" DOES NOT EXIST
Bisected on one ttfx tree at `6cdb7857`: plain `make` + `--native` → 2 refusals; plain `make` + `--pgo`/`--release` → the same 2; `make` WITH the opt-in + `--native` → **0 refusals, binds 81/81**. Descriptor words/structs for ids 188/190 are byte-identical across builds. Root cause is the stub again: without `-DJANET_NATIVE_EXECUTORS`, `janet_native_register` returns 0 so EVERY registration fails, and `188:int-string?`/`190:to-i` are merely the only manifest entries whose `defn` lives in the AOT entry file — the only ones `mkimage` registers EAGERLY, and the eager path is the only one that prints. Two visible reports out of 81 silent failures read as two broken functions. **The `--pgo-auto` correlation in `ML/ttfx/bench/bender-37-gate.md` is coincidental** (those two binaries came from differently-configured janet builds) and that doc needs correcting. With the opt-in, `--native`, `--pgo`, `--pgo-auto` and `--release` ALL report `available=81, REFUSED: none`. Fixed in `591049a3`: a stub runtime now says so explicitly instead of printing a misleading count.

## ⚠ MEASUREMENT FLOOR IS PROVISIONAL — audit in flight (ML/ttfx bc49dae7)
The published floor (**geomean 0.9992 / 1.0025, ±3% per effect**) came from pairs including `ttfx-cli-s0a`/`s0b`, which were built by a compiler at `/tmp/janet-stage0` that was made with a PLAIN `make` — i.e. probably WITHOUT `-DJANET_NATIVE_EXECUTORS`. If so, the executor tier in those binaries was INERT and that floor describes interpreter-mode behaviour, not native-tier behaviour.
**Why this was invisible:** with the stub, every registration fails, but only entry-file `defn`s are registered EAGERLY and therefore PRINT. `validate-bender.py` infers `bound = manifest − printed_refusals`, so a totally inert runtime reports a confident "79/81 bound". `bench/bender-37-gate.md` is corrected (ML/ttfx `3e81a7f`): the 79/81-vs-81/81 pipeline story is marked SUPERSEDED, `REFUSED: none` is no longer treated as proof of binding, and the 1.0205 "executor inequality" explanation is withdrawn.
**Claims under review:** the floor itself; the typed pilot's null (0.9917 + control-test failure); the stop-gate's occupancy figures. Note the stop-gate build is probably SAFE — it reported non-zero `nx_*` counters and 81 active Tier-B executors, which is positive evidence of a live tier. Continuation v2 is also probably safe — `639be9a5` verified 96/96 with `REFUSED: none` on opted-in builds. Treat every other timing number as unqualified until the audit reports.
**Rule going forward: no performance claim without positive proof the tier is live** — non-zero per-definition executor counters at runtime, not an inferred binding count.

## FLOOR AUDIT RESULT (ML/ttfx 9f05751, `docs/floor-audit.md`) — the old floor was INTERPRETER-MODE
Verified by disassembly (`otool` on the actual `_janet_native_register`), not inference: **`ttfx-cli-s0a`, `s0b`, `autopgo-v2` and `packed-path-pgo2` all carry the STUB** — the registration function clears the native pointer and unconditionally returns 0, so **ZERO Tier-B executors were bound** in any of them, despite each printing a confident `available 81, REFUSED 188,190`. The frozen baseline hashes match those binaries. The original `packed-path-pgo` is non-stub (its registration calls the structure matcher/verifier), so capability is established there, but runtime activity was never counted — explicitly unverified.
**THE PUBLISHED ±3% / 0.9992 / 1.0025 FLOOR IS INVALID AS A NATIVE FLOOR.** It measured interpreter-mode binaries.

### The real verified-native floor is MUCH WIDER
New pair `ttfx-floor-a` / `ttfx-floor-b`: same source, same pinned compiler (d5a24e5d, `NATIVE_EXECUTORS=1`), independent caches, **identical generated native C**, `REFUSED: none`, non-stub registration, and liveness PROVEN by probe (`nx_92`: 1,248 entries / 47,524 ops on a two-frame decrypt, stdout byte-identical to the uninstrumented build). Four alternating paired rounds, 37/37 bare-effect byte parity:
- **37-effect geomean 1.0063**; historical five-effect subset **1.0218**.
- **Only 23/37 effects land within ±3%; 14 fall outside.** Range **0.8821 (slice) … 1.2256 (orbittingvolley)**.
So on live-tier builds the per-effect spread is roughly **±12–23%**, not ±3%, and even the geomean sits 0.6% off unity for two builds of identical C. One session is not a universal threshold, but the direction is unambiguous: this bench is far noisier than believed.

### Per-claim disposition
- **Floor (±3%/0.1–0.3%)**: INVALID for native work. Any sub-2% claim previously accepted on it is unsupported.
- **Continuation v2 (1.0064→0.9881/0.9834)**: the regression-removal claim now rests on a ~1.8% geomean delta against ~0.6% geomean build noise — SUGGESTIVE, NOT PROVEN. The implementer's own "at or below the floor, not a 1.2% win" caveat was the right instinct; it now needs a stronger protocol to confirm.
- **Typed pilot (0.9917, control faster than treatment)**: the conservative verdict — no attributable end-to-end win — SURVIVES on its own internal control, and its builds were opted-in. Its significance argument against the old floor is void.
- **Loop stop-gate**: SURVIVES. Its build had direct non-zero `nx_*` counters (1,632,623 entries / 47,116,287 ops on decrypt) and its NO-GO rests on SHAPE, not on a marginal timing delta. Only its citation of the old floor needs qualification.

### Consequence — measurement methodology is now the critical path
No further performance work is meaningful until a protocol exists that can resolve ~1% on this machine: CPU pinning/affinity, many more paired rounds, per-effect confidence intervals rather than single medians, outlier policy, and a declared minimum detectable effect. **Rule: no performance claim without (a) positive proof the tier is live — non-zero per-definition counters, never an inferred binding count — and (b) an effect larger than the protocol's stated MDE.**

## TIMING PROTOCOL RESULT (ML/ttfx c902e6d/012a4a1/4b87c02, `docs/timing-protocol.md`, `bench/paired-timing.py`)
**We cannot resolve 1% by wall-clock A/B on this machine, and the reason is structural, not statistical.**
- A/A control, two builds of IDENTICAL generated C, verified live, 16 ABBA rounds: wall geomean 1.00926 CI95 [0.99381, 1.02730] (conditional radius 1.79%); internal `time_ms` was WORSE (1.00736, radius 1.95%).
- **The killer: A/A `binarypath` came back 1.0404 CI [1.0048, 1.0882] — a confident FALSE POSITIVE against identical C.** Bootstrap CIs model within-run noise but not persistent BUILD-LAYOUT bias, so a per-effect CI can exclude 1.0 while the true effect is zero. Any per-effect claim from a cross-build A/B is therefore unsafe regardless of its interval.
- Variance decomposition: within-binary repeat CV **13–20%**; within-pair log SD 5.38–7.26%; residual ABBA SD 1.98–8.50%; early-vs-late drift −1.34 to +1.66 log%. ABBA interleaving genuinely helps (decrypt single-pair SD 6.58% → 1.98%); more rounds do NOT help monotonically; macOS gave no CPU affinity and denied priority elevation.
- Usable guardbands (illustrative, not formal power): **~4.6% on a four-effect geomean, ~8.7% worst individual.**
- Controls: a synthetic 5% arithmetic control was detected; a PHYSICAL positive control was not achieved — so the MDE is calibrated only against injected arithmetic, not against a real code change.

### Verdict on continuation v2 under the honest protocol
**NOT DISTINGUISHABLE.** v2 vs pre-change, 12 ABBA rounds, all three binaries rebuilt live-tier: wall geomean **0.99054 CI [0.97192, 1.01547]** — the interval spans 1.0, direction unresolved. (v1 vs v2 observed 1.03870 [1.02957, 1.04912] but is cross-build-confounded by exactly the bias the A/A control exposed.) So "narrowing removed the regression" remains **plausible but unproven**; the mechanism results (5 emitted PCs, 89,380 chains retained, refusals 563,564 → 2, counter identity 89,380 + 564,450 = 653,830, byte parity) all stand on their own and are NOT in question.

### Consequence — how to measure from here
Cross-build wall-clock A/B is a dead end below ~5%. Future performance claims must use one of:
1. **Same-binary toggles** (one build, feature switched at runtime, e.g. `JANET_NATIVE_NO_CONTINUATION`) — removes build-layout bias entirely. This is why v1's 0.9996/v2's 0.9924 toggle legs are the more trustworthy numbers.
2. **Deterministic counters instead of time**: native ops, VM exits, interpreted calls, instruction counts. These are exact, and the project already emits them.
3. Effects large enough to clear ~5%, or a dedicated quiesced machine.

### Hard-won operational facts (cost real time to rediscover)
- **Old commits silently ignore `make NATIVE_EXECUTORS=1`** — the Makefile option only exists from d5a24e5d. Building `66a5cf9a`/`3db27d20` with that variable yields a STUB runtime that still prints "96 available". For pre-d5a24e5d commits you MUST pass `CFLAGS='-O2 -g -DJANET_NATIVE_EXECUTORS'`. Always verify with non-zero per-definition counters.
- **`JANET_NO_PROFILE=1` is mandatory during counter probes**: without it v2 produced 89,849 refusals and ZERO chains — an A/B of a no-op.
- **`matrix` is unusable for timing**: its `--bench` frame count varies run to run (30,822–31,202). Use `rings` as the stable long effect.

## Hint fixes landed (06e35981, c582a594, 57e84b41)
1. **Private-hint fail-open CLOSED**: root cause was `src/core/specials.c:janetc_def` excluding `:private` from the compiler's hint-retention sidecar, so `defn-` hints never reached `register-def`/`extract-hints`. Hinted private defs now carry the same checked contract (verified `double priv(double p0, double p1)`), with an invalid-hint `hint-bad` regression.
2. **Dynamic `require` now DIAGNOSED**: `preimport-unit-imports` only queues top-level literal imports, so a require inside a function never reaches hint extraction. Emits `callgraph: dynamic require module not analyzed for hints: <module>` instead of silence. (Diagnosis, not support.)
3. **`math/hypot` lowered** via `tools/infer.janet:pure-calls` + C symbol reservation (so a kernel named `hypot` cannot shadow libm); differential test **4096/4096 bit-identical**, including `hypot(1e308,1e308)`.
Gate: 37/37 effects, 368/368 runnable cases byte-identical, both sides 81/81 bound, no refusals.

## DETERMINISTIC WORK COUNTERS — the primary performance metric now (9296ba62, 1ce43f9c, ac6bb708; `docs/internals/work-counters.md`)
Wall-clock A/B is retired for sub-5% questions. The metric is now exact, reproducible work attribution: `--work-profile PATH` (requires `JANET_NO_PROFILE=1` per invocation — NEVER export it globally, it legitimately breaks `suite-aot-profile-env`). Determinism proven: five repeats byte-identical (report SHA256 `558147b8…`), stdout identical with counting on vs off. Suites green: full native `make test`, `make test-executors`, `suite-callgraph` 256/256. **Limitation to quote every time: counters measure WORK, not TIME.** A change can cut counts and still be slower (cache, branch prediction) — this metric governs attribution and direction, not final speed claims.

### Baseline work vectors (seed 42, 400 frames, no-PGO builds — coverage at its floor)
| effect | interp dispatches | native ops | interp share | VM exits |
|---|---:|---:|---:|---:|
| rings | 114,391,082 | 50,387,115 | ~69% | 583,894 |
| binarypath | 66,300,646 | 46,671,003 | ~59% | 771,225 |
| decrypt | 19,614,209 | 20,387,628 | ~49% | 1,123,599 |
| beams | 18,313,371 | 24,466,465 | ~43% | 570,499 |
`rings` is the most interpreter-bound despite being longest-running; `decrypt` is exit-DENSE rather than work-dense (≈2× the exits on a fraction of the work), which is why it was the right continuation workload.

### CONTINUATION v2 — SETTLED IN WORK TERMS (what wall-clock could not do)
Same binary, feature toggled, byte-identical output, trained with `JANET_EXECUTOR_LEAF_MIN_CALLS=500` + `JANET_EXECUTOR_EDGE_MIN_CALLS=500` (5 PCs selected), 400-frame decrypt:
| counter | ON | OFF | delta |
|---|---:|---:|---:|
| chained edges | 89,380 | 0 | — |
| interpreted calls | 564,450 | 653,830 | **−89,380 (exact conservation)** |
| VM exits | 1,436,921 | 1,570,361 | **−133,440 (−8.5%)** |
| interpreter dispatches | 18,432,093 | 18,672,489 | **−240,396 (−1.3%)** |
| native ops | 21,506,520 | 21,506,520 | invariant |
**Verdict: the mechanism provably works and genuinely reduces work — and the magnitude EXPLAINS the wall-clock null rather than contradicting it.** A 1.3% dispatch reduction is far under the ~5% wall-clock can resolve here. Perfect edge/call conservation is strong evidence the accounting is sound; invariant native ops is the signature of work crossing the boundary rather than new work appearing. Both methods now agree.

### What the vectors say about strategy
Even PGO-trained, a 400-frame decrypt still retires ~18.4M interpreted dispatches against ~21.5M native ops. **Coverage remains the frontier** — this is the fifth independent line of evidence (continuation v1 refusal census, typed-pilot population mismatch, loop stop-gate shape, zero-fallback counter audit, and now work vectors). Next questions to put to the counters, not to a stopwatch: which definitions dominate `interpreted_dispatches` (the reporter emits top-N), and what share would move if `ctx-frame-string` (17–36% of executor occupancy, blocked by construction poison) became natively constructible.

## PINNED pre/v1/v2 WORK COMPARISON — v2's narrowing is TRAINING-DEPENDENT (settled with counters)
Historical counters retrofitted via measurement-only overlays in ignored `tmp/` (ABI-matched to each pinned image, output byte-exact; no pinned source mutated).

**Interpreted dispatches, 400 frames, seed 42:**
| effect | pre | v1 | v2 | v2−v1 |
|---|---:|---:|---:|---:|
| decrypt | 18,672,489 | 18,431,435 | 18,432,093 | +658 |
| rings | 113,188,121 | 113,125,945 | 113,127,919 | +1,974 |
| beams | 17,249,923 | 16,976,178 | 16,976,178 | 0 |
| binarypath | 65,264,779 | 65,100,716 | 65,131,718 | **+31,002** |
Native ops identical across all three revisions for every effect; output byte-exact.

**Root cause, proven by conservation** (lost chains == extra interpreted work, one-for-one): chains v1→v2 are decrypt 90,034→89,380 (−654), rings 19,344→17,378 (−1,966), beams 127,249→127,249 (0), binarypath 76,231→45,385 (**−30,846**). v2 selects only the PCs a DECRYPT profile nominated (`ctx-anim`/`scene-new`/`ctx-scene`/`ctx-add-frame`), so it under-serves untrained workloads; `beams` ties exactly because its hot edges happen to fall in that set, and motion/path-heavy `binarypath` suffers most — its hot edges are design §5's motion chain, which a decrypt profile never sees.

**Three-way conservation proof (validates the metric itself):** pre's interpreted calls — 653,830 / 2,408,002 / 534,828 / 2,272,883 — exactly equal v1's and v2's `interpreted_calls + chained_calls` for every effect. Total call work is invariant across revisions; continuation only MOVES calls from interpreted to chained.

**Refusal counts are NOT comparable between v1 and v2** (audited: v1 `loop-executor.janet:437-455` attempts a chain at every non-terminal call PC so a failure increments a refusal at `vm.c:523-525`; v2 `:456-471` attempts only at selected PCs, so an unselected call exits with no attempt and no refusal). v1's 602,846/410,297/249,662/446,192 versus v2's 2/2/3/2 is a DENOMINATOR change, not a success-rate improvement. Report side by side, never differenced.

### OPERATIONAL RULE (belongs with `JANET_EXECUTOR_EDGE_MIN_CALLS`)
**Profile-narrowed emission is only as good as its training workload's representativeness; single-workload training can make an untrained effect WORSE in work terms than unnarrowed emission.** v2 as trained is slightly worse than v1 on 3 of 4 effects and equal on the fourth, though both beat pre-change. The open question — whether multi-workload training (via `--pgo-auto`'s per-workload merge + `tools/profile-merge.janet`, possibly with a lower `EDGE_MIN_CALLS`) recovers v1's chain coverage while keeping v2's near-zero guard waste — is NOT yet answered and is the next experiment. If per-PC selection cannot cover several effects at once, that is a real limit of the approach and must go in the design doc.

## CAUSAL ATTRIBUTION — v2's regression localized to ONE unselected caller (3cff5732)
Exact, not inferred. v1 → decrypt-trained-v2 chain losses, each equal one-for-one to extra interpreted calls: decrypt −654, rings −1,966, beams 0, **binarypath −30,846**. Top-N localizes binarypath's entire loss to a single caller, **`travel-complete?` id664**, which decrypt's profile never nominated. Decrypt's five selected PCs are all scene/animation CONSTRUCTION edges: `ctx-new-scene` 2→`ctx-anim`, 8→`scene-new`; `ctx-scene` 2→`ctx-anim`; `ctx-add-frame` 2→`ctx-scene`; `ctx-apply-gradient` 44→`ctx-add-frame`.

**The design's edge prediction was right; decrypt was the wrong workload to validate it.** A binarypath-trained selection emits 10 PCs including `travel-complete?` 20→`ctx-anim` AND `ctx-coord-on-line` 3→`geom-lerp-point` — the motion chain `native-continuation-design.md` §5 named from the start — recovering binarypath chains **45,385 → 216,823** with byte-exact output.

**CONFOUND, stated and respected:** binarypath training also shifts the leaf executor set **96 → 107** and native ops **47,782,078 → 53,148,727**, so its **−5,282,115** dispatch delta must NOT be credited to edge selection alone, and no time claim follows. Leaf-tier selection being workload-sensitive from identical source is itself a previously unquantified property of `JANET_EXECUTOR_LEAF_MIN_CALLS`.

**Recorded caveats:** the threshold 500 is not universal; lowering it CANNOT select edges a profile never observed (coverage is bounded by the training workload, not by the threshold); v1/v2 refusal denominators differ so refusal counts must never be differenced.

### Shipping recommendation (now evidence-backed)
Narrow the emission, but train on a **merged multi-workload profile** — exactly what `--pgo-auto` + `tools/profile-merge.janet` (Phases 1–3) were built for. Single-workload training demonstrably makes untrained effects worse than unnarrowed emission.

### Open, cheap, and decisive
A same-image `JANET_NATIVE_NO_CONTINUATION` toggle on the binarypath-trained build would attribute its remaining dispatch delta with zero cross-build confound. Also still open: reconcile the gate's 68 BLOCKED against the frozen baseline's 67.

## CAPSTONE — merged multi-workload training BEATS unnarrowed v1 on coverage
`--pgo-auto` merged THREE isolated replayable 400-frame workloads, gating and publishing each (the Phase 1–3 machinery doing exactly the job it was built for). Result at `EDGE_MIN_CALLS=500`: **13 selected PCs / 111 executors**, covering all five decrypt edges PLUS `travel-complete?` 20→`ctx-anim`, `ctx-coord-on-line` 3→`geom-lerp-point` (the design §5 motion chain) and `rings-set-disperse-path` 24→`randrange`.

**Chained calls (higher = more work moved out of the interpreter):**
| effect | v1 (emit everywhere) | v2 decrypt-only | v2 merged 3-workload |
|---|---:|---:|---:|
| decrypt | 90,034 | 89,380 | 89,775 |
| rings | 19,344 | 17,378 | **136,743 (7.1× v1)** |
| beams | 127,249 | 127,249 | 128,975 |
| binarypath | 76,231 | 45,385 | **216,823 (2.8× v1)** |
Refusals: decrypt 397 / rings 3 / beams 4,003 / binarypath 792 — two to three orders of magnitude below v1's 602,846/410,297/249,662/446,192, but NOT uniformly near-zero (beams 4,003). Output byte-exact on all four effects; per-effect call-edge conservation still exact against measured pre calls (653,830/2,408,002/534,828/2,272,883).

**So merged training dominates both predecessors on coverage** — more chains than emit-everywhere v1 on every effect — while keeping guard waste far below v1. That is the shipping direction.

**CONFOUND, still respected:** the merged build selects 111 executors vs v1's 96, so part of the chain gain comes from broader leaf coverage, not edge selection alone; no isolated continuation effect and no time claim may be drawn from these numbers. A same-image `JANET_NATIVE_NO_CONTINUATION` toggle is the outstanding control.
**Threshold note:** lowering `EDGE_MIN_CALLS` below 500 is NOT indicated — the 13-PC selection already covers every named hot edge, and a lower threshold cannot select edges a profile never observed. Coverage is bounded by workload representativeness, not by the threshold.

### Merged-training work reduction — the headline number (ec621797)
Interpreted dispatches, 400 frames, seed 42, byte-identical output across all revisions:
| effect | pre | v1 | v2 decrypt-only | **v2 merged** | merged vs pre | merged vs v1 |
|---|---:|---:|---:|---:|---:|---:|
| decrypt | 18,672,489 | 18,431,435 | 18,432,093 | **18,421,082** | −1.3% | −10,353 |
| rings | 113,188,121 | 113,125,945 | 113,127,919 | **108,669,940** | **−4.0%** | −4,456,005 |
| beams | 17,249,923 | 16,976,178 | 16,976,178 | **16,916,670** | −1.9% | −59,508 |
| binarypath | 65,264,779 | 65,100,716 | 65,131,718 | **59,841,977** | **−8.3%** | −5,258,739 |
Merged training is the best configuration on every effect, against both pre-change and unnarrowed v1.

**How to state this honestly.** The merged build selects 111 executors (vs 96) AND 13 edge PCs, so this is the measured effect of the WHOLE auto-PGO package — representative training selecting more leaf executors *and* more chain edges — not an isolated continuation result. Stated that way it is sound and significant: **representative multi-workload PGO training removes 4–8% of interpreted work on motion-heavy effects.** No wall-time claim follows (counters measure work, not time), and decrypt still ends 259 chains short of v1.

**This validates the session's first deliverable with its last measurement:** `--pgo-auto` per-workload merge + gating (Phases 1–3) is what makes the narrowed-emission tier actually pay. Build training sets from several replayable workloads; never train selection on one.

### CLOSED: the 68-vs-67 BLOCKED discrepancy was a counting artifact
Not a regression. The "68" counted 67 per-case `BLOCKED effect set N` rows PLUS the aggregate summary line `BLOCKED 67 cases...`. Correct arithmetic on both the frozen baseline and the current run: **368 PASS + 67 BLOCKED + 1 VACUOUS = 436**. All 436 (effect, ordinal) keys were parsed and compared: current == frozen in membership AND status for every case, zero extra/missing/moved. The blocked-reason histogram matches exactly (43 `expected … got 0`; 9 generic invalid colour; 4 `invalid range inf-inf`; 3 `invalid float inf`; 3 colour '196'; 2 '244'; 1 each '250', '21', '45') — all the known 256-colour palette gap at `janet-port/src/color.janet:145`, documented at `bench/bender-37-gate.md:307-325`. No counter regression; no file change warranted.
