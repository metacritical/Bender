# ttfx unproven-type inventory (native codegen)

Question answered: **which types remain unproven when generating ttfx
code**, i.e. beyond the numeric/array kernels that already emit, what
still keeps functions boxed.

Snapshot for every number below:

- compiler `545b18f7` (working tree; only `tools/infer.janet` had an
  unrelated debug hunk, gated by an env var, no effect on these runs)
- ttfx `janet-port/{src,bin}` aggregate sha256
  `f0253cf4cd8bb44594e187b4759ddb3771374871298466449a8c630b800a17cc`
  (includes the in-flight Stream F u64 kernel wiring by concurrent work)
- entry `bin/ttfx-cli.janet`; PGO evidence
  `$T/kernel-fixed-build/ttfx-native/profile.jdn`

Reproduce (temp-only instrumented copy of `tools/callgraph.janet` that
adds a `ti` type/provenance line per def; the repo tool is untouched):

```sh
bender tools/callgraph.janet bin/ttfx-cli.janet \
  --profile <profile.jdn> --emit-native-lib /tmp/diag
```

## 1. Refusal census (CLI, 796 `native-skip` lines)

| reason | count |
|---|---:|
| types not proven (non-duplicate defs) | 453 |
| duplicate registration of an imported site | 170 |
| loop needs resumable VM quantum polls | 169 |
| u64 / per-site emitter refusals | 4 |

Type-refusal defs break down as (overlapping; one def can add several):

| blocker | count |
|---|---:|
| unproven param slots (`?`) | 490 |
| defs with *all* params unproven | 265 |
| unproven return (`?`) | 253 |
| zero-arity defs (emitter requires >= 1 param) | 116 |
| proven return but no ABI: `tuple` | 69 |
| proven return but no ABI: `bool` | 31 |
| param type conflict (`any`, widening) | 24 |
| proven param but no ABI: `nil` | 21 |
| proven param but no ABI: `bool` | 11 |
| proven return but no ABI: `function` | 9 |
| proven return but no ABI: `string` | 5 |

The already-emitting kernels prove the supported set: `:double`,
`:number`, `:long`, `:array` params; `:double`/`:number`/`:array`
returns (7 kernels on this snapshot; `k-next-word-u64!` is wired but
refuses, see §5).

## 2. Which types are not proven, and why

PGO observed types on the *unproven/conflict slots* of skipped
functions: `table` 28, `number` 15, `string` 9, `tuple` 7, `array` 7;
453 slots have no observation at all in this workload (decrypt-heavy
profile). Across all 772 profiled functions the palette that reaches
native codegen in ttfx is: `table, number, string, array, tuple,
function, symbol, nil, boolean, buffer, struct, keyword, cfunction,
fiber, abstract`.

Root causes, in order of impact:

1. **No lattice representation** (always `:unknown`, never provable):
   `:table`, `:struct`, `:fiber`, `:abstract`, `:cfunction`,
   `:function`, `:buffer`. `mktab`/`mkstu`/`mkfun` set the slot
   `:unknown` by design; only numerics, opaque `array/tuple/pointer`,
   and literal constant types are tracked. Tables are the dominant
   runtime type (scene/ctx/canvas/effect state objects). The Phase 5
   closed-shape pass can flatten only zero-escape local tables: this
   snapshot reports **291 `table-open`, 0 `table-flat`** — every ttfx
   table escapes, so flattening never applies.
2. **Proven but no ABI in the emitter** (inference succeeds, emission
   refuses): `bool` params (11) and returns (31); `nil` params (21);
   `tuple` returns (69); `string` returns (5); `function` returns (9);
   `:u64` at the ABI (internal-only by design).
3. **Proof gaps on otherwise-supported types** (`number` 15, `string`
   9, `array` 7, `tuple` 7 observed): two sub-causes:
   - PGO seeding requires a slot's row type set to have *exactly one
     entry*; duplicate identical entries (`@[:string :string]`, seen
     in the SEED debug for `build-effect`) fail the seed, so observed
     numeric/string slots stay `:unknown`;
   - genuine conflicts widen to `:any` (24 slots), e.g. `frame-new`
     slot 1 `array|tuple`, `ctx-anim` slot 1 number conflict,
     `color-fg` slot 0 `array|tuple`.
4. **Zero-arity**: 116 defs (thunks and small void helpers) cannot
   emit because the candidate gate requires at least one param.
5. Not type-related: 169 loops wait on native continuations, 170
   duplicates collapse to a canonical def.

No `dynamic`/`ambiguous` call-graph edges exist in ttfx (0/0), so
dynamic-taint is *not* a cause here.

## 3. Hot blocked functions (profiled calls, CLI workload)

| calls | fn | inferred | observed |
|---:|---|---|---|
| 278118 | `ctx-char-active?` | `? ? -> bool` | table, number -> boolean |
| 278118 | `ctx-step-animation` | `? ? ? -> nil` | table, number, table |
| 91388 | `ctx-anim` | `? any -> ?` | table, number |
| 81908 | `ctx-scene` / `frame-new` / `ctx-add-frame` | tables + `array|tuple` slot | table, number, string |
| 70058 | `choose-symbol` | `nil -> ?` | table |
| 23700 | `color-clamp` | `? -> long` | number (seed gap only) |
| 8696 | `color-fg` | `? -> ?` | array+tuple (conflict) |
| 3950 | `scene-empty?` / `scene-activate` | table; `-> bool`/`tuple` | table |
| 3160 | `ctx-new-scene` / `scene-new` / `ctx-activate-scene` | + tables | table, number, string |
| 1792 | `ctx-register-event` | `?` | table, number, string |
| 1612 | `charanim-new` | longs + string | number, string (return table) |
| 1046 | `encode-codepoint` | `long -> ?` | number (seed gap) |

Counts by family: `src/effects/*` 276, engine (`scene/canvas/color/
rng/easing/...`) 155, `bin/` harness 22.

Emittable-observed but unproven (proof/seed gap only, no new ABI
needed): `color-clamp` (23700), `color-fg` (8696, conflict), `?`
`canvas.janet:179/320` (790), `encode-codepoint` (1046), `hex2` (24),
`effect-kind`, `flag-set?`, `color.janet:145` (1580, array/tuple).

## 4. Immediate levers, ranked

1. Seed dedup: accept a slot whose observed set contains one *distinct*
   type (duplicates are the same observation, not a conflict). Unlocks
   `color-clamp` (~24k calls, number -> long) and the other
   emittable-observed rows without touching the lattice.
2. `:bool` ABI (params + returns): 42 proven refusals including
   predicates (`scene-empty?`, `ctx-char-active?` once tables allow),
   `col-buckets`.
3. `:tuple` returns (69 defs): color triples, coordinates,
   `scene-activate`.
4. `:nil` params (21 defs): optional-last-arg shape.
5. `:string` returns (5).
6. Tables: the real prize, but needs a kernel-visible representation
   (interprocedural shape propagation or named-struct ABI). 68% of
   engine type-skips (91% in `scene`+`canvas`) have an unproven slot 0
   — the ctx/scene/canvas/state receiver.
7. Zero-arity gate (116 defs) is a small independent fix.

## 5. Live finding: `k-next-word-u64!` canonical-copy divergence

With the concurrent Stream F wiring in `src/rng.janet` (16:43:15), both
CLI and GUI entries refuse the new hinted kernel with
`slot s23 mixes u64 with non-u64: u64/u64ok/number` (diag ids 442/471,
dup 791/807). A GUI build at 16:43:01 — before that `rng.janet` save —
had emitted `k_next_word_u64_bang` successfully. Same
`rng-kernels.janet` throughout, so the flip tracks which registration
channel becomes canonical (analysis copy vs real module copy) and its
per-copy bytecode, not the kernel source. Worth resolving in the
duplicate-channel unification before trusting u64 emission.
