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

## ISOLATED CONTINUATION EFFECT — same-binary toggle, the clean causal number
Both runs are the SAME binary with `JANET_NATIVE_NO_CONTINUATION` toggled, so executor set, generated C and layout are identical by construction. Workload: binarypath, 400 frames. Every ON/OFF stdout byte-identical (both images SHA256 `c96d7773…`).

| image | chains | interpreted calls | interpreted dispatches | VM exits | native ops |
|---|---:|---:|---:|---:|---:|
| decrypt-trained (96 exec) | 0 → 45,385 | −45,385 | 65,264,779 → 65,131,718 (**−133,061**) | 1,115,740 → 1,049,976 (**−5.9%**) | invariant 47,782,078 |
| binarypath-trained (107 exec) | 0 → 216,823 | −216,823 | 60,302,302 → 59,849,603 (**−452,699**) | 2,042,165 → 1,674,933 (**−18.0%**) | invariant 53,148,727 |

**What continuation actually does, now unambiguous:** it removes CALL-BOUNDARY work, not bulk interpretation. In the well-trained image it eliminates 216,823 interpreted calls and **18% of all VM exits**, while interpreted dispatches fall only 0.75% and native ops and executor entries are exactly invariant. Chain gain equals interpreted-call decrease exactly in both images — conservation holds on the isolated measurement too.

**Scope discipline:** each paired ON/OFF delta is attributable to the toggle alone. The DIFFERENCE between the two images (319,638 more dispatches and 301,468 more exits saved, 171,438 more chains) is conditional on their different 96/107 executor sets and is NOT a pure edge-selection estimate. OFF-state refusal counts (45,616 vs 218,723) merely count disabled attempts — never read as a success rate.

**Synthesis — why the 22× gap survives all of this:** continuation is now proven correct, cheap and genuinely effective at what it targets, and it is still nowhere near the bottleneck, because boundary crossings were never the bulk cost. ~60M interpreted dispatches remain on a single 400-frame binarypath run. That is the sixth independent confirmation that COVERAGE — how much code becomes native at all — is the frontier, and it is why the queue is `ctx-frame-string`/construction poison, SSA per-PC slot typing, and context-table parameters rather than any further dispatch work.

## VERIFICATION PASS (clean NATIVE_EXECUTORS build at 5fce278b) — and a brutal noise demonstration
All green: `make test` exit 0 (suite-vm 36/36; suite-shared skipped, `JANET_SHARED_STACKS` unset), `make test-executors` exit 0. CLI AOT build: 9 kernels, mkimage 357,448 bytes / 801 registry names; runtime `native_init: registered 9 wrappers, available 81 frame executors, REFUSED none` — tier verified LIVE. Gate: 37 effects / 436 cases = **368 PASS, 0 FAIL, 67 BLOCKED, 1 VACUOUS**; candidate and baseline were the SAME binary (SHA `0ea4ae75ab4a79a4`), both manifest 81 / bound 81.

**The timing table is the point.** Identical binary against itself, 3-round medians, geomean 0.9905 — yet the per-effect spread is:
`overflow 0.714` · `middleout 0.906` · `binarypath 0.926` · `burn 0.939` · … · `spotlights 1.123` · `smoke 1.153`
That is a **0.71–1.15 range from a binary compared to ITSELF** — no code difference whatsoever, not even a rebuild. This independently reproduces and strengthens the floor-audit finding: per-effect wall-clock on this harness is worthless below roughly ±15–30%, and even the geomean (0.9905) sits ~1% off unity. Every sub-5% per-effect claim ever made against this bench was noise. Deterministic work counters remain the only valid performance metric here.

**Absolute throughput (400 frames):** fastest `overflow` 22.9 ms; slowest `matrix` 15,609 ms; `thunderstorm` 12,181 ms. matrix+thunderstorm alone are ~28 s per timing pass and dominate any full sweep.

### GUI path
`bin/ttfx-gui.janet` builds cleanly through the current compiler: 7 kernels (2 FEWER than CLI), mkimage 334,232 bytes, and **83 frame executors (2 MORE than CLI)**, no refusals, 20 FFI aliases installable. Interface: `--list`, effect selection (`--random-effect`/`--include-effects`/`--exclude-effects`), `--seed`, `--text`/`--input-file`, font options, `--windowed`, `--frame-rate`, `--show-fps`, `--uncapped`, `--probe-seconds N` (1 s warmup then FPS/ticks-per-second), `--probe-shot`. **No headless or frame-limit mode**: the raylib backend calls `InitWindow` and requires a display; `DISPLAY`/`WAYLAND_DISPLAY` are empty here, so no FPS was measured. The differing kernel/executor counts (7/83 vs 9/81) are a real signal that the GUI workload mix selects a different native set — worth a work-vector comparison if the GUI ever becomes a target.

### Provenance of the gap — a half-finished commit
`efe625e5` admitted table/struct/string/tuple allocation opcodes as VM boundaries **in the emitter** and never made the parallel **verifier** allowlist update. One side of a two-sided contract was extended; nothing in the build or the suites noticed. That is the definitive support for the oversight verdict (alongside `0c051c35`, which documents the exact-word VM-boundary contract both sides are meant to honour).
Consequence worth stating: from `efe625e5` until `cbb8e716`, `mesh-chunk` and `raycast` were silently refused, so **every performance investigation on TrineXraft in that window was measuring a program with two of its heaviest kernels locked out of the native tier** — including the 1.3–2.9% Phase 10 result that motivated `trinexraft-workload-tasks.md` in the first place. The contract test added with `cbb8e716` exists precisely so a future half-finished commit fails loudly instead of silently refusing definitions.

## THE VERIFIER ALLOWLIST FIX — corrected record (eb78257e, c132175d)
Superseded the earlier combined `cbb8e716`. Split into two atomic commits on `c0e5085e`, and the split is what made the attribution below knowable — **the orchestrator argued against splitting and was wrong.**

**Current synthesis of the protected documentation record (`80348f69`, `6db1e3a0`, `c0e5085e`):** retain the original results, build-trap investigation and implementer's caveats below, but do not revive their superseded conclusions. The four VM-only constructors in `eb78257e` own the observed coverage/work gain (TrineXraft 2→4 live definitions; ttfx ID373 executes despite the unchanged 81-entry manifest) and observed mesh p50 ratio 0.8071; the historical nine guarded opcodes in `c132175d` brought emitter/verifier admission to 64/64 while yielding **zero new measured workload coverage**; follow-up `e39ddea5` deliberately narrows the verifier to five reviewed admissions (60/64), retaining four shifts as explicit emitter-only exceptions. The historical T2 comparison had 4→4 definitions, unchanged ttfx counters/top-10 and generated C, and mesh ratio 1.0126—not a benefit. This negative result was knowable only through the split. The earlier combined-commit attribution and “ttfx gained nothing” are false; a manifest count, like inferred “79/81 bound,” is not attachment proof. Per-definition entries/ops below establish actual native traversal. The original ~18% p50 reduction is **observed, NOT securely attributable as a causal speedup** across builds: counters measure WORK, not TIME. The +16.4M exits may be a real cost; the once-unattributed ~8M were subsequently identified as `JOP_CALL` exits (exact decomposition below). The historical “STILL UNFIXED” build-mode heading and “highest-value remaining work” recommendation below describe the earlier state, **superseded by the tracked-dependency fix `f6613dbb`**; before that fix, two repeat encounters with stale plain-mode objects demonstrated why `make clean && make NATIVE_EXECUTORS=1` was essential. The original per-opcode safety argument is preserved with the four shifts explicitly **unproven** (OPEN DECISION below), not certified by their `NX_INT32` guards or masked native counts.

**Root cause:** `efe625e5` (2026-10-22, "native: guard fast paths for table/array/buffer access and container calls") extended the EMITTER to treat allocation opcodes as VM boundaries and never made the parallel VERIFIER update. One side of a two-sided contract. Nothing in the build or the suites noticed, so `native_structure_matches`' allowlist silently refused ENTIRE DEFINITIONS containing one such opcode — no diagnostic, no error. TrineXraft's `mesh-chunk` (462 PCs) was rejected over a single trailing `JOP_MAKE_TABLE` at PC460 (`src/mesher.janet:231:3`). Every TrineXraft performance investigation between those two commits — including the 1.3–2.9% Phase 10 result that motivated `trinexraft-workload-tasks.md` — measured a program with its two heaviest kernels locked out of the native tier.

### Attribution: tranche 1 owns the entire win
**`eb78257e` — four VM-only constructors** (`mkstr`, `mkstu`, `mktab`, `mkbtp`):
| metric | baseline | T1 |
|---|---:|---:|
| TrineXraft live definitions | 2 (of 12 emitted) | **4** (+`nx28` mesh-chunk 11,148,347 entries / 263,759,471 ops; +`nx43` raycast 5,295,273 / 71,044,820) |
| interpreted dispatches | 726,510,340 | **401,401,294 (−325,109,046)** |
| native ops | 60,222,026 | **395,026,326 (+334,804,300)** |
| VM exits | 2,500,740 | 18,885,615 (+16,384,875 — the cost) |
| mesh-chunk p50 | 6.240 ms | **5.046 ms (ratio 0.8071, range 0.8023–0.8139)** |

**ttfx also gains** — correcting an earlier claim that it "gained nothing": the emitted manifest is unchanged at 81, but `nx373 canvas-build-neighbor-map` now ATTACHES and runs (4,038 entries / 55,974 ops), interpreted dispatches 11,480,396 → 11,428,417 (−51,979, ≈−0.45%) on an 8-frame decrypt profile; VM exits rise 831,964 → 835,962. **A manifest count is not a coverage measure** — the same inferred-vs-live distinction that made "79/81 bound" mean zero.

**`c132175d` — historical nine-op measurement; follow-up `e39ddea5` retains five** (`band bor bxor bnot`, `inim`) and deliberately excludes the four shifts (`sl slim sr srim`): the measured nine-op tranche delivered **ZERO measurable coverage on these workloads.** At `c132175d`, 64/64 emitter mnemonics were admitted; the narrowed verifier now admits **60/64**, with the four shifts explicitly emitter-only in the contract test. The measured nine-op comparison had TrineXraft 4→4 live, ttfx counters and top-10 unchanged, generated C identical, mesh p50 ratio 1.0126 (range 1.0073–1.0492; no benefit claimed). A test `bor` definition does attach and execute, so that capability is real — it simply buys no new workload coverage here. Separating this tranche from the four constructors is what made the negative attribution knowable: the latter own the measured win. The five bitwise/`inim` admissions have a reviewed guard/PC-fallback argument and the contract detects the next silent divergence; **the four signed shifts remain an unresolved exact-parity risk** (see the OPEN DECISION below), so correctness cannot justify them without a further decision. The deliberate `sru`/`sruim` uint32-domain exclusion is preserved.

### Why the timing is stated as observed, not causal
Generated `native.c` is byte-identical across all three arms (TrineXraft `c36d6357…`, ttfx `09f460ce…`), so there is no codegen confound, and the mechanism was measured independently. But it remains cross-build on one host where identical-binary A/A swung 0.71–1.15 per effect. **Report "observed ~19% p50 reduction", never "securely attributable speedup". Counters are WORK, not TIME.** The +16.4M VM exits may be a real cost; only ~8.4M are accounted for as generated boundary exits, leaving ~8M unattributed.

### Per-opcode safety argument
Constructors never execute natively: `NX_BOUNDARY` sets the original unexecuted PC before effects, and the VM performs allocation and GC exactly once. The four bitwise ops (`band`/`bor`/`bxor`/`bnot`) use `NX_INT32` on operands and narrow to the exact int32 domain; a failed guard leaves the original PC and an unmodified result slot so the boxed VM raises identically. `inim` reads only valid array/tuple indices (unsigned 8-bit index), all other routes exiting before effects at the original PC. All operate in GC-scanned fiber slots; polls, backedge materialisation and exact descriptor re-entry validation are unchanged.

> **⚠ CORRECTION — this paragraph previously said "bitwise/shift ops" together and thereby OVERCLAIMED shift parity.** The four shifts (`sl`/`slim`/`sr`/`srim`) are **NOT** proven exact-parity admissions and must never be described as safely proven. The VM evaluates raw signed int32 shifts with an unmasked register count / signed 8-bit immediate (`src/core/vm.c:688-704, 751-773, 2363-2373`), while the generator masks the count `&31u`, wraps left via unsigned, and explicitly sign-extends right (`tools/loop-executor.janet:547-565`). Negative or ≥32 counts, and negative or overflowing left operands, invoke **undefined** VM C semantics, so a same-compiler fixture cannot be a sound equivalence proof. The other five admissions were separately reviewed and DO carry the guard/PC-fallback argument above; `janet_wrap_integer(x)` is `janet_wrap_number((int32_t)x)` in `src/include/janet.h`, resolving the `bnot` boxing question for guarded int32 results. Follow-up `e39ddea5` excludes the four shifts from the verifier; see the historical OPEN DECISION and resolution at the end of this file.

### Corrected per-exit cost model (earlier text overstated it)
A boundary crossing does NOT pay per-exit descriptor revalidation (epoch-cached; full compare only on invalidation), does NOT bulk-materialise slots (generated code already uses fiber slots), and does NOT redispatch the loop head on successful reentry. The real cost is PC/frame/stack/func restore, the exit count, the boxed op plus conditional GC, and the gate check. **That is why removing 86.5% of VM exits in Task 1 bought only ~5.4%: those exits were cheap.** Also: tick-count invariance is NOT proof of identical machine work — in the baseline a guard tick precedes each failed put.

### Known evidence limits (do not overstate)
`nx28`'s explicit registration return was not re-probed post-fix; liveness is proven by counters, and explicit registration only for the matching fixture. Workload-level global attach/no-match deltas were NOT collected (`--work-profile` lacks them; the `-e` path reports initial state). Top-10 listings cannot establish an exhaustive active-count delta. The validator's "81 bound" is inference, not liveness.

### Gates (both tranches, isolated HOME/XDG cache, serial)
Clean `make NATIVE_EXECUTORS=1` + full `make test` green; clean plain `make` + full `make test` green; `make test-executors` matrix green incl. sanitizer and forced quantum; `suite-callgraph` 256/256; TrineXraft 12/12; **ttfx 37/37 effects, 368/368 runnable cases byte-exact, 0 FAIL** (67 common BLOCKED, 1 VACUOUS). A **contract test** now asserts emitter/verifier agreement with fixtures mirroring the real shapes and asserting nonzero entries/ops/exits/backedges — liveness proven, never inferred.

## ⚠ THE BUILD-MODE TRAP — the costliest defect of the session (STILL UNFIXED)
`NATIVE_EXECUTORS=1` injects `-DJANET_NATIVE_EXECUTORS` into `BUILD_CFLAGS`, but **make has no dependency on the flag's value.** After a plain build, `make NATIVE_EXECUTORS=1` recompiles nothing and silently links a **stub runtime** while appearing to honour the request. Only `make clean && make NATIVE_EXECUTORS=1` switches modes.
With the stub, `janet_native_register` returns 0 so EVERY registration fails — but only AOT entry-file definitions are registered eagerly and only the eager path PRINTS, so a wholly dead runtime reports a confident `available 81, REFUSED 188/190`, which reads as "79 of 81 bound" and actually means ZERO.

**Phantom results this one defect plausibly produced, all retracted or corrected:**
- the published ±3% / 0.9992 noise floor — measured on stub binaries, verified later by `otool` disassembly of `_janet_native_register`; withdrawn
- "79/81 bound" meaning zero bound
- the `--pgo-auto`/`--release` descriptor bug — bisected to the stub, never a pipeline transform; retracted
- `trinexraft-workload-tasks.md`'s "Tier B matched ZERO regions" premise — falsified by measuring 12 emitted executors
- Task 1's validation, which ran against a candidate-era interpreter and a post-revert binary
The implementer hit it twice more in one session, and `d5a24e5d` made the opt-in EXPRESSIBLE but not RELIABLE.

**FIX (highest-value remaining work):** make the mode a tracked dependency — write the flag into a generated header the objects already depend on, or stamp `build/.native-executors` that object rules depend on, or at minimum FAIL LOUDLY when the request cannot be honoured. Silence is the bug. Pairs with `591049a3`, which closed the same hole at run time.

## Test-infrastructure defects found while using the suites
Each turned a killed or concurrent run into a PERSISTENT failure that looked like a code defect and cost a rerun to disprove:
1. **Hardcoded `/tmp` scratch paths** — `/tmp/janet-dce-strip-suite`, `/tmp/cg-*` collide between concurrent harnesses. `test/suite-native-executor.sh` does it correctly via `JANET_AOT_TEST_ROOT` + `XDG_CACHE_HOME`; the rest inherited fixed paths. Derive every scratch root from `mktemp -d`.
2. **Unreclaimable mkdir-lock** — `~/.cache/janet/pgo/cg-nn-bin.lock` is a directory, so it holds no file descriptor and `lsof` can NEVER prove it held; a killed run orphans it and blocks every subsequent `suite-callgraph` forever. One sat orphaned for hours this session, failing two agents, until removed by process-age argument. Needs a timeout or reclaim.
3. **No PGO cache pruning** — `~/.cache/janet/pgo` holds **57,827 entries**; every PGO operation pays directory-lookup cost in there.
4. **`bench/kernels.janet` prints `os/clock` timings on stdout** (`:24-29,38-58`), so its stdout can NEVER be a byte-output oracle; a line-1 timing difference is not a semantic failure. A deterministic state-dump harness is needed for TrineXraft AOT-vs-interpreter byte comparison.

## Orchestration lessons (cost real time this session)
- **"Stand down" must propagate to the whole subtree.** Releasing a parent left ~30 descendants running; one grandchild kept writing into another agent's checkout for several cycles. Use `list_agents --scope descendants` and interrupt the actual mutator.
- **Never let a piped git command gate an `&&` chain.** `git checkout X | tail -1` masks the abort; a chained `git commit --amend` then rewrote the wrong commit's message (recorded in `8e87abb2`).
- **One owner per repo — including the orchestrator.** Documentation commits landing during another agent's history rewrite caused a reset that dropped the session's main fix off the branch.
- **Give every agent its own worktree up front**, and preserve recovered work as a COMMIT, not a patch in `tmp/` — a commit survives working-tree resets; a patch does not.
- **Cleanup against stale state is more dangerous than the untidiness.** Three separate near-losses came from well-intentioned restores and rewrites, not from the mess they targeted.

### Coverage counters — gap CLOSED, measured not inferred
Earlier text said workload-level attach/no-match deltas were unavailable. They were subsequently obtained with non-production AOT probes: each generated `native.c` copied to an ignored path with **only the exit-time JSON reporter modified** to emit `janet_native_stat(ATTACH_ATTEMPTS/ATTACHED/AMBIGUOUS/NOMATCH)` plus exhaustive all-descriptor execution rows, with the prefix and suffix around the reporter verified byte-identical to the original generated C, linked against native static libs from each archived commit, all six probe binaries hash-pinned.

| workload | arm | emitted | attempts | attached | no-match | executed |
|---|---|---:|---:|---:|---:|---:|
| TrineXraft | baseline | 12 | 40 | 2 | 38 | 2 |
| TrineXraft | **T1** | 12 | 40 | **4** | 36 | **4** (+`nx28` mesh, +`nx43` raycast) |
| TrineXraft | T2 | 12 | 40 | 4 | 36 | 4 (unchanged) |
| ttfx | baseline | 81 | 100 | 11 | 89 | 12 |
| ttfx | **T1** | 81 | 100 | **12** | 88 | **13** (+`nx373`) |
| ttfx | T2 | 81 | 100 | 12 | 88 | 13 (unchanged) |

**Report `attached` and `executed` SEPARATELY, never as equal** — they differ in ttfx (12 vs 13) because the lazy attach counter excludes eagerly explicit-registered definitions, while the execution rows prove actual execution. This is the same inferred-vs-live distinction as "81 bound", now resolved with real counters on both axes.
Validity checks: ttfx copied-probe frame dumps byte-identical across all arms AND against the original pinned images; ttfx deterministic work vectors equal the original pinned profiles. TrineXraft stdout differs only in timing fields between repeated runs, and its deterministic work numbers carry small run jitter (≤1,390 dispatches, ≤75 ops) while VM exits and active IDs are stable.
**Do NOT reuse the earlier 4→527 / 524 global figures** — they came from a different probe and run.

### Exit attribution — the +16.4M exits are now FULLY decomposed
Matched baseline → constructor (T1), TrineXraft, VM-global exit delta **+16,384,875** breaks down exactly as:
| opcode | delta |
|---|---:|
| `JOP_CALL` (53) | **+7,980,500** |
| `JOP_GET` (59) | **+7,924,250** |
| `JOP_DIVIDE` (12) | +320,000 |
| `JOP_MAKE_TABLE` (68) | +80,125 |
| `JOP_RETURN` (3) | +80,000 |
| `JOP_PUT` (60) | **unchanged** at 2,162,688 |
Opcode counts sum exactly to the VM-global exit total. The generated-boundary counter rose only **+8,404,375**, and the residual **+7,980,500 corresponds precisely to the CALL category** — so the previously "unattributed ~8M" are non-generated-boundary CALL exits. Note also that the voxel-count coincidence (delta ÷ 16,384 = 1000.053) is real arithmetic but is NOT evidence that buffer access dominates.

**Ops per generated-boundary exit:** `nx28` mesh-chunk 263,759,394 / 7,924,375 = **33.285**; `nx43` raycast 71,044,819 / 480,000 = **148.01**; baseline `generate-into` 25.266; `fbm-signed-2d` 82.517.

### Consequence: the stacked-experiment prediction is REVISED DOWNWARD
The prediction on record was: `nx28` exit-dense (~25–35 ops/exit) → Task 1's buffer admission gains on top of T1. `nx28` measures **33.285**, so that branch resolves TRUE — but the attribution adds a constraint that halves the expected benefit. Buffer admission addresses the **GET** half (+7,924,250) and leaves the **CALL** half (+7,980,500) untouched, since `JOP_CALL` exits are not a collection-access boundary. `JOP_PUT` is unchanged at 2,162,688, so the write path gains nothing further here either.
**So: the stack is still worth testing, but its remedy cannot be presumed to address CALL exits, and any gain should be expected at roughly half the naive estimate.** The CALL half is instead the province of native-to-native continuation — which was measured as a small net regression on ttfx (see Continuation v1/v2 above) but has NEVER been evaluated in this regime, where `mesh-chunk` and `raycast` are natively executing and generating ~8M CALL exits that did not previously exist.

### TRAP: never nest a git worktree under the main checkout
`profile-source-matches?` (`tools/callgraph.janet:1250-1270`) deliberately walks analyzed-source ancestors when a recorded profile path is relative. If a worktree lives INSIDE the main checkout (e.g. `janet/tmp/stack-worktree/`), the same fixture path exists in both the nested tree and its ancestor, producing **TWO realpath hits**; the ambiguity makes the matcher refuse profile provenance, so edge selection finds nothing and `native-continuations.txt` comes out EMPTY. The failure looks like a code regression in whatever you are testing — it reproduced identically on a clean pinned comparator with no candidate code at all.
**Always place throwaway worktrees at a NON-DESCENDANT path** (`/tmp/...`, never `<repo>/tmp/...`). Add this to the infrastructure list below; it is the fifth distinct way this harness converts environment into apparent defect, alongside hardcoded `/tmp` scratch collisions, the unreclaimable mkdir-lock, the unpruned 57,827-entry PGO cache, and `bench/kernels.janet` printing timings on stdout.

## ★ THE STACKED RESULT — Task 1's NO-GO is REVERSED when stacked on the allowlist fix
The experiment specified earlier (both arms at the allowlist fix, prediction stated in advance) has been run, after all correctness gates passed.

**Timing, five alternating pairs, allowlist-only vs allowlist+Task1:**
| kernel | allowlist-only p50 | stacked p50 | ratio |
|---|---:|---:|---:|
| **mesh** | 5.146 ms | **4.238 ms** | **0.81756** (range 0.81656–0.84389) |
| worldgen | — | — | 0.92719 |
| raycast | — | — | 0.99466 (unchanged, as expected) |
| palette | — | — | 0.99390 (unchanged) |
Pairs: 5.197→4.311, 5.190→4.241, 5.146→4.202, 5.022→4.238, 5.136→4.199.

**Cumulative on mesh-chunk: 6.240 ms (pre-allowlist) → 5.046 (T1) → 4.238 (stacked) ≈ 0.679, i.e. roughly a third faster than baseline.**

**The opcode histogram proves the mechanism exactly:** `JOP_GET` 7,924,250 → **0** and `JOP_PUT` 2,162,688 → **0**, while `JOP_CALL` remains 8,250,836 and `JOP_DIVIDE`/`JOP_MAKE_TABLE`/`JOP_RETURN` are unchanged. `nx28` generated boundary exits 7,924,375 → 125. Work: −12.422M interpreted dispatches, −10,086,938 VM exits (−53.4%), +2.336M native ops.

### What this overturns
**Task 1's NO-GO (worldgen −5.41%, mesh −0.57%) was an ARTIFACT of measuring buffer admission while `mesh-chunk` was not in the tier at all.** Its mesh figure could never have shown anything, as noted at the time. Stacked on the constructor fix — which is what admits `mesh-chunk` — the same six-file change is worth ~18% on the heaviest kernel. The two changes are **synergistic**: the allowlist fix brings a buffer-dominated kernel into the tier, and buffer admission then removes the boundary exits that admission creates.
This also retires my own caution that a large exit reduction would under-deliver on time, as it did for Task 1 alone (86.5% of exits → 5.4%). It did not here: 53.4% of exits → ~18% on mesh. The difference is qualitative — these are GET exits inside a hot, now-native region, not puts in a loop that was already cheap.

### Consequence
The artifact preserved on `task1-buffer-regions` (`1462595a`, fully validated — see its git note) is **no longer a parked null**; it is half of a combined ~32% improvement on TrineXraft's heaviest kernel, pending a decision to land it. Had it been deleted for failing to reach 10% in isolation, this result would have required re-deriving it from scratch.
Remaining exit population is dominated by `JOP_CALL` (8.25M), which buffer admission cannot touch. That is native-to-native continuation's territory — measured as a small net regression on ttfx, but **never evaluated in this regime**, where `mesh-chunk` and `raycast` run natively and generate ~8M CALL exits that did not previously exist.

### Precision correction: "no codegen OR layout confound" was overstated
Earlier text in this plan says the allowlist timing has "no codegen or layout confound". The accurate statement is narrower:
- **Generated `native.c` IS byte-identical** across baseline/T1/T2 — TrineXraft `c36d6357ae3464fe7b95eac0db795ba239a9881ff3c1b1a36524817a0d9a6dfa`, ttfx `09f460cef09e061e31bc7a1d87c4998a1a450d1a03f29ab5a60b0cc18d6b1502`. That isolates the emitter and the descriptor set, and establishes the change as purely verifier-driven attachment.
- **But the final AOT binaries are NOT identical**: the linked runtime source changed, so binary SHAs differ and a code-layout effect cannot be excluded. Say "no CODEGEN confound; layout confound not excluded", never "no confound".
- For the stacked experiment the generated C legitimately DIFFERS (stack `0957cab1` vs `c36d6357`), because Task 1 modifies the emitter itself — so that comparison carries both caveats and its ~18% is likewise "observed", not securely attributed.
This matters because the whole measurement discipline of this session rests on not overstating what a control actually controls for.

### Correction: the ttfx parity gates were NATIVE-vs-NATIVE, not against an inert baseline
The orchestrator earlier suggested these runs would compare a native candidate against the validator's inert DEFAULT baseline, and described "368/368 against an inert baseline" as the claim. **That is false for this evidence.** `--baseline` was passed explicitly on every run, comparing consecutive native-tier-ACTIVE arms:
- T1 `510e67…` vs archived pre-change native-tier-active baseline `a67e718…`
- T2 `c882853…` vs T1 `510e67…`
- stack `0a4148…` vs T2 `c882853…`
`runtime_available=81` on both arms of each pair, and exhaustive 8-frame exit-time counters establish actual execution (archived baseline 12 active descriptors / 3,173,094 native ops / 831,964 native VM exits; T1 and T2 13 active / 3,229,068 ops). So these gates validate the **changed tier paths themselves**, step by step — the correct regression test for an incremental change. The historical frozen 368/368 (default inert binaries) predates all of this and serves only as population precedent (436 sets, 368 runnable, 67 BLOCKED, 1 VACUOUS).
A native-vs-BOXED parity run would be a different and semantically stronger test, and it is not absent from the record: the Task 1 candidate has byte-equal boxed-vs-native guard output across all five invalid cases, and TrineXraft three-way parity (baseline AOT = candidate AOT = candidate interpreter, 4,897,880 bytes, SHA `be3b00a6…`). Semantic equivalence is therefore demonstrated; the incremental ttfx gates are not a substitute for it and were never claimed to be.

### Corroboration: two INDEPENDENT pair sets, and the native-op accounting closes to 3
The allowlist timing rests on two separately-built comparisons, not one:
| pair set | baseline p50s (ms) | candidate p50s (ms) | median ratio | separation |
|---|---|---|---|---|
| original `verifier-*` | 6.317 6.319 6.307 6.335 6.289 | 5.231 5.196 5.172 5.171 5.158 | **0.82016** (0.81626–0.82808) | min baseline 6.289 > max candidate 5.231 by 1.058 ms |
| clean `verifier-split-t1` | 6.309 6.234 6.240 6.237 6.277 | 5.064 5.074 5.046 5.034 5.036 | **0.80712** (0.80229–0.81392) | min baseline 6.234 > max candidate 5.074 by 1.160 ms |
Neither set overlaps at all, and both were produced from the **identical generated `native.c`** (`c36d6357…`), so verifier admission is the operative code-path change. Provenance caveat: the original `verifier-*` binaries came from differing compiler build stamps (`e64aa244` vs `8e87abb2`); the `verifier-split-*` set is the clean independently-pinned one. The layout caveat still cannot be erased — the linked runtimes differ.

**Native-op accounting closes almost exactly:** newly-live `nx28` mesh-chunk 263,759,412 ops + `nx43` raycast 71,044,820 ops = **334,804,232**, against a measured global increase of **+334,804,229** — a difference of 3 in 335 million. So the two newly-attached definitions account for essentially ALL new native execution, and `mesh-chunk` alone is **78.78%** of it, which matches the selective speedup appearing on mesh and not on raycast/palette.

**Limit to respect:** there is NO per-definition interpreted-dispatch counter, so the −44.75% dispatch figure is workload-GLOBAL. Do not convert it into a mesh-specific claim. The timing, native-execution and boundary counts corroborate direction and site; they do not constitute a quantitative wall-time model.

## ✅ BUILD-MODE TRAP — FIXED (f6613dbb)
Supersedes the "STILL UNFIXED / highest-value remaining work" status above.
`build/.native-executors-mode` is now a **normal prerequisite** of `janet.o`, `shell.o` and the non-amalgamated core objects, force-checked against the effective `ifdef NATIVE_EXECUTORS` value. Switching modes in either direction invalidates the objects and triggers the `bender`/static/shared relinks; same-mode builds are untouched, so incremental builds stay incremental.
Regression test `test/native-executor-build-mode.sh` archives HEAD, overlays the candidate Makefile in a private tmp, and builds **plain → native → plain at -j2 WITHOUT `make clean`**, asserting nanosecond timestamps on both objects and all three products at each transition, same-mode timestamp stability, and AOT fixture native registration versus the plain stub diagnostic. All three phases green.

**The test earned its keep before the commit existed:** the first attempt used an ORDER-ONLY prerequisite, which does not force a rebuild — native silently reused the plain object, i.e. the fix reproduced the exact bug it was written to prevent, and the test caught it.

**NEW INVOCATION REQUIREMENT (behaviour change, documented in the Makefile):** the mode is now sticky to the invocation, so the flag must be passed on EVERY invocation including test targets. `make NATIVE_EXECUTORS=1 && make test-executors` now deliberately switches back to plain on the second command and fails. Use a single invocation: **`make NATIVE_EXECUTORS=1 test-executors`**. Same for `make NATIVE_EXECUTORS=1 test`.

Together with `d5a24e5d` (opt-in expressible) and `591049a3` (a stub runtime announces itself in `native_init`), the hole is now closed from three directions: expressible, detectable at run time, and impossible to produce by accident at build time. None alone was sufficient.

**Attribution hedge, per the implementer:** the historical floor and Task 1 mismeasurements are *consistent with* stale build mode; they are not *proven* to have been entirely caused by it. Keep that wording.

### Stacked result REPLICATED at the exact `cbb8e716` base — magnitude is a BAND, not a point
**Source equivalence, not a new measurement request:** `cbb8e716:src` and historical split-T2 `c132175d:src` both have Git tree `d8d880dba1df13cdfdee6d0f7bd5cfa017f505dd`; the six-file Task 1 patch is the same SHA-256 `aab08cba909fcc649004ccf521f8a5577424b34a9bb5657fa3eb8762fce77f9d`. Thus the already-measured split-T2 stack answers the proposed `cbb8e716` source-level two-arm question; do not rerun it merely for that equivalence. The two independently built binary pairs have different mesh timing magnitudes below, so retain the observed band rather than transferring 0.81756 as an exact hardware result. Current HEAD has advanced beyond historical T2 and has a different `src` tree.
A second, independently-built two-arm experiment (both worktrees detached at `cbb8e716`; arm B = the six-file Task 1 patch `aab08cba…`; both clean-built with `-DJANET_NATIVE_EXECUTORS`; identical descriptor manifest `e9f332a9…`; images A `6269f993…` / B `e7d455d5…`):

**Counters replicate EXACTLY** — `nx28` 33.28457 → **2,128,763** ops per boundary exit; boundary exits 7,924,375 → **125**; global VM exits 18,885,615 → 8,798,677 (−10,086,938); mesh boundary −7,924,250 and `generate-into` −2,162,688, with `raycast`/`fbm` unchanged.

**Timing is directionally identical but SMALLER than the first stack run:**
| kernel | A p50 | B p50 | median pair ratio |
|---|---:|---:|---:|
| mesh | 5.229 ms | 4.535 ms | **0.86728 (−13.27%)**, all 5 pairs < 1, range 0.78731–0.89179 |
| worldgen | 5.810 ms | 5.477 ms | 0.93494 (−6.51%) |
| raycast | — | — | 1.03051 (noisy) |
| palette | — | — | 0.97035 (noisy) |

**So quote the stacked mesh effect as a BAND of roughly −13% to −18%, not as 18%.** The two runs differ only in which build pair was used (split-T2 arms vs consolidated-`cbb8e716` arms) on source-identical trees, which is exactly the residual binary-layout variance we have refused to claim away all session. What replicates robustly is the DIRECTION (every paired ratio < 1 in both runs) and the MECHANISM (identical counters to the opcode); the magnitude does not pin down better than that band on this hardware.
Also note aggregation labels precisely: ratio-of-medians and median-of-paired-ratios differ (e.g. 0.82355 vs 0.81756 on the first run, 0.6pp apart). The paired-ratio median is the better statistic here because the pairing is the control.
Correctness for both arms: TrineXraft suites pass; native executor matrices pass incl. forced quantum and sanitizer (B adding buffer tests); ttfx 37 effects / 368 byte-exact runnable cases on both. The `cbb8e716` base predates the build-mode fix `f6613dbb`, but both arms were clean-built and compiler flags were verified.

## OPEN DECISION: four shift opcodes assert an unprovable parity
`eb78257e`/`c132175d` admit `JOP_SHIFT_LEFT`, `JOP_SHIFT_LEFT_IMMEDIATE`, `JOP_SHIFT_RIGHT`, `JOP_SHIFT_RIGHT_IMMEDIATE` to the verifier allowlist. The implementer's own safety audit found their bit-for-bit parity **cannot be proven**: the native path masks shift counts `&31` with implementation-defined uint32/sign-extension, while the VM applies direct C signed shifts (`src/core/vm.c:688-704, 751-763, 2363-2373`) whose behaviour is UNDEFINED for negative counts, counts ≥32, and signed-left overflow. Two implementation-defined behaviours agreeing on today's compiler is not parity.
They also bought **nothing measurable**: the nine-op tranche was 4→4 TrineXraft definitions with ttfx vectors identical.

**Why this was NOT simply reverted (analysis, for whoever decides):** the contract test `test/native-executor-allowlist.py` requires an `EXCLUDED` opcode to be absent from BOTH the emitter and verifier sets (that is how `sru`/`sruim` are handled — the emitter never emits them). Removing the shifts from the verifier alone makes them *emitter-only*, which the test correctly reports as exactly the divergence signal it exists to catch. A correct narrowing therefore needs a THIRD category — "deliberately emitter-only" — tolerated in `emitted - verified` and asserted absent from `verified`. And it is a semantic change, not a cosmetic one: shift-containing definitions would stop attaching altogether, which is a coverage decision on workloads that use bitwise/shift code (hashes, RNG, voxel packing) even though it costs nothing on the two measured here.

**Three options, in order of preference:**
1. **Narrow** — remove the four from the verifier, add the third test category, re-gate. Conservative; restores pre-`cbb8e716` behaviour for shift-containing definitions; costs no measured coverage.
2. **Guard the domain** — reject negative or ≥32 counts and other undefined signed-shift cases before native execution; prove agreement only on the VM-defined subset and defer every other case to the VM. More work, but retains the defined-domain capability.
3. **Keep and document** — retain them with the limitation recorded. Only defensible if someone establishes that the emitter cannot produce an out-of-domain shift, which has NOT been shown.
Do NOT leave this implicit: today the repo asserts an invariant nobody has defended. Option 2 proves only defined-domain native agreement and preserves VM fallback elsewhere; it cannot make undefined VM behavior well-defined.

### Refinement to the shift OPEN DECISION — what option 2 can and cannot achieve
Option 2 ("guard the domain") was described above as making parity *provable*. More precisely: restricting the native shift fast path to the VM-defined domain and deopting otherwise **does NOT define the VM's behaviour in the undefined domain** — it merely guarantees the native path never executes there and the VM handles it, whatever it does. That is the best achievable outcome, and it is a genuine improvement over today (where both sides compute in a domain neither defines), but state it as "native never diverges inside the defined domain and always defers outside it", not as "parity proven everywhere".
Option 1 ("exclude via an explicit contract exception on both sides") remains the simpler conservative choice and costs no measured coverage.
Scope note: the historical nine guarded-native admissions were `band`/`bor`/`bxor`/`bnot` + `sl`/`slim`/`sr`/`srim` + `inim`; only the five non-shifts remain admitted after `e39ddea5`. `in` and `get` are separate pre-existing operations and must not be counted among them when designing fixtures. Any fixture work here must also distinguish SEMANTIC expected values from DEOPT parity — they are different properties and conflating them would produce a test that passes while proving neither.

### Resolved qualification on `bnot` boxing
The five non-shift guarded admissions (`band`/`bor`/`bxor`/`bnot`/`inim`) carry a source-level guard + original-PC-fallback argument. The earlier `bnot` result-boxing question is RESOLVED: `src/include/janet.h` defines `janet_wrap_integer(x)` as `janet_wrap_number((int32_t)(x))` (line 967), so guarded int32 native results use the same numeric representation as the VM. `inim` is bounded by its unsigned-byte immediate; the arbitrary register-key `in` path is a separate concern and out of scope for this tranche.
Also for the record on the shifts: signed-left UB persists **even at in-range counts** (negative or overflowing left operand), so restricting counts alone would not fully discharge option 2 — a complete domain guard must bound the operand as well as the count.

### Resolution — five-op guarded tranche (`e39ddea5`)
Option 1 above is now implemented as a follow-up, without rewriting `c132175d`: `native_structure_matches` no longer admits `sl`/`slim`/`sr`/`srim`. The contract test asserts **exactly these four** are deliberately emitter-only (present in the emitter, absent in the verifier), rejects any additional emitter-only or verifier-only opcode, and continues to exclude `sru`/`sruim` from both. Admission is **60/64 emitter mnemonics**; the five retained tranche operations are `band`/`bor`/`bxor`/`bnot`/`inim`. A descriptor containing any of the four shifts is refused as a whole, restoring the prior conservative coverage policy. This does **not** claim that signed shifts are intrinsically unsafe on every input: parity is unprovable from a VM reference whose C signed-shift behavior is undefined for negative or out-of-range counts and negative/overflowing left shifts, while native code masks/wraps. Against the measured zero new workload coverage from T2, asserting that invariant has no justified benefit. Future admission requires a guard for the **defined** VM subset (both count and left operand), with original-PC fallback to the VM elsewhere, not an assumption of compiler-specific agreement. The T1 constructor-only comparator and its measured attribution remain untouched. The historical nine-op measurement and its 64/64 count above describe `c132175d` before this follow-up, not current admission.

## ✅ SHIFT OPEN DECISION — RESOLVED by narrowing (e39ddea5, 11edd81d)
Option 1 was taken. `sl`/`slim`/`sr`/`srim` are **removed from the verifier allowlist** and asserted in `test/native-executor-allowlist.py` as exactly four DELIBERATE emitter-only exceptions; the contract now reports **60/64**, with `sru`/`sruim` still excluded bilaterally. Cost was zero — the nine-op tranche had already measured 4→4 TrineXraft definitions with ttfx counters, top-10 and generated C identical.
Nothing in this repository now asserts the unprovable signed-shift parity. The route back remains a defined-domain guard, and per the earlier qualification it must bound **both the shift count AND the left operand** (signed-left UB persists even at in-range counts), with VM fallback outside that domain.
**`bnot` qualification also RESOLVED:** `src/include/janet.h:967` defines `janet_wrap_integer(x)` as `janet_wrap_number((int32_t)(x))`, so the alias concern does not arise for int32-guarded native output. The five remaining guarded admissions (`band`/`bor`/`bxor`/`bnot`/`inim`) carry a complete guard + original-PC-fallback argument.
**Gates, serial on the exact narrowed HEAD `11edd81d`:** fresh-cache `make clean` + `make -j2 NATIVE_EXECUTORS=1` + `make NATIVE_EXECUTORS=1 test` exit 0 (callgraph 256/256, DCE strip 61/61); isolated `make NATIVE_EXECUTORS=1 test-executors` exit 0 incl. sanitizer and forced quantum; TrineXraft 12/12 at its HEAD `8b86a5af`; ttfx at `24d063ba` — full validator against the pinned T1 binary, **37/37 effects, 368/368 runnable byte-identical, 0 FAIL** (67 BLOCKED, 1 VACUOUS). Tranche 1 `eb78257e` untouched and still an ancestor.

### Arithmetic corrections to the stacked-result write-up
Two framings above were loose; the pinned JSON (`tmp/verifier-split-t1-trinecraft-work.json` vs `tmp/verifier-stack-trinecraft-work.json`) gives the exact form:
1. **The removals sum exactly, with no residual to explain.** `nx28` exits 7,924,375 → 125 = **−7,924,250**; `nx21 generate-into` 2,162,820 → 132 = **−2,162,688**; together **−10,086,938**, matching both the global VM-exit delta (18,885,615 − 8,798,677) and the generated-boundary delta (10,634,779 − 547,841) exactly. Earlier text predicted 7,924,375 + 2,162,688 = 10,087,063 and then explained away a 125 gap — that was wrong framing: 7,924,375 was ALL original `nx28` boundary exits, not all REMOVABLE ones. The 125 are simply non-removed boundary exits that remain.
2. **The entry drop is near one-to-one, NOT exact.** `nx28` entries 11,148,347 → 3,224,075 = −7,924,272, which is **22 more** than the exit reduction of 7,924,250. Do not claim an exact one-for-one exit↔re-entry correspondence from this; the direction and magnitude support the re-entry explanation, the precise equality does not hold.
Also reconfirmed: dispatches −12,422,161 (−3.0957%), native ops +2,335,978, VM exits −53.4%. And the CALL attribution of the +7,980,500 gap comes from the separate measured opcode histogram, NOT from these work JSONs, which do not serialize per-opcode counts.

## ★★ BUFFER ADMISSION LANDED ON BRANCH (02b32d24) — Task 1 shipped, three independent timings
The six-file Task 1 treatment was ported from the old nine-op worktree onto the current narrowed five-op HEAD and committed as `02b32d24 feat(native): admit guarded buffer reads and writes`. This retires the provenance gap noted above: the effect is now measured on the ACTUAL current base, not inferred to transfer.

**Three independent measurements of the stacked mesh effect, different bases and build pairs:**
| experiment | base | median paired ratio |
|---|---|---:|
| split-T2 arms | `c132175d` | 0.81756 (−18.24%) |
| `cbb8e716` arms | `cbb8e716` | 0.86728 (−13.27%) |
| **current narrowed base** | control `da207b6a` vs stack `02b32d24` | **0.84830 (−15.17%)**, pairs [.85956, .84830, .81564, .86270, .84194], range .81564–.86270 |
Worldgen paired median 0.96563 (−3.44%); raycast 1.01191 (no win, as expected). Ratio-of-arm-medians 0.84030 — again note the aggregation distinction. **Quote the stacked mesh effect as ≈−13% to −18%, central ≈−15%.**

**Counters reproduce exactly on the current base:** VM exits **−10,086,938** (identical to both prior runs), dispatches −3.0947%, native ops +0.5914%. ttfx: +20 exits, +211 native ops, −201 dispatches, parity exact — consistent with no hot buffer site in that profile, which is not proof of absence.

**Gates on the exact landed HEAD:** clean native build + full `make test` exit 0 (callgraph 256/256, DCE strip 61/61); native executor matrix exit 0 incl. sanitizer and forced quantum; TrineXraft 12/12 at HEAD `8b86a5af`; ttfx 37/37 effects, 368/368 runnable byte-exact against the narrowed baseline (67 BLOCKED, 1 VACUOUS). Matched control and stack compilers both clean-built, equal 12-ID manifests, no work profiling during timing.

**Cumulative on TrineXraft's heaviest kernel:** pre-allowlist 6.240 ms → constructors 5.046 → +buffer admission ≈4.28 (applying −15.17%), i.e. roughly **a third faster** than the pre-allowlist baseline, from two changes totalling ~50 lines of compiler source.

### FOLLOW-UP: the build-mode regression is not wired to any make target
`test/native-executor-build-mode.sh` is verified and passing (plain→native→plain with no clean, strictly-increasing mtimes on `janet.o`/`shell.o`/`bender`/`libjanet.a`/`libjanet.{dylib,so}`; same-mode repeats asserting stamp and every product mtime UNCHANGED; plain AOT stub diagnostic versus native AOT registration). But `grep` finds no reference to it in `Makefile` or `test/suite-native-executor.sh` — **it runs only when invoked explicitly.**
That is the same defect that hid the executor suite at the start of this session: `make test` globs `test/suite*.janet`, so a shell-based regression nobody calls will silently rot, and the protection it provides will quietly lapse. Wire it into a target (a `test-build-mode` target, or fold it into `test-executors`), mindful that it builds in its own isolated directory and must not collide with a concurrent suite run — see the `/tmp` scratch collisions recorded above. Low effort, and without it the fix for the session's costliest defect is guarded by a test that nothing runs.

### Working-tree note
`implementation_plan.md` shows as modified by an external actor and was deliberately never touched, staged, restored, or committed by any agent. Do not describe the tree as clean without that qualification; `git checkout -- implementation_plan.md` restores the committed pointer notice if ever wanted.
