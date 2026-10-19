# ttfx unproven-type inventory (native codegen)

Question answered: **which types remain unproven when generating ttfx
code**, i.e. beyond the numeric/array kernels that already emit, what
still keeps functions boxed.

Snapshot for every number below:

- compiler `bafc7e48` (Stream F step 2 landed; the inventory was first
  taken at `545b18f7` and every section below is refreshed to
  `bafc7e48` unless stated)
- ttfx `janet-port/{src,bin}` aggregate sha256
  `f0253cf4cd8bb44594e187b4759ddb3771374871298466449a8c630b800a17cc`
  (includes the Stream F u64 kernel wiring, now emitting)
- entry `bin/ttfx-cli.janet`; PGO evidence
  `$T/kernel-fixed-build/ttfx-native/profile.jdn`

Reproduce (temp-only instrumented copy of `tools/callgraph.janet` that
adds a `ti` type/provenance line per def; the repo tool is untouched):

```sh
bender tools/callgraph.janet bin/ttfx-cli.janet \
  --profile <profile.jdn> --emit-native-lib /tmp/diag
```

## 1. Refusal census (CLI, 778 `native-skip` lines)

| reason | count |
|---|---:|
| types not proven (non-duplicate defs) | 450 |
| duplicate registration of an imported site | 156 |
| loop needs resumable VM quantum polls | 169 |
| u64 per-site emitter refusals (`xor64`/`or64`/`rotl64`) | 3 |

Type-refusal defs break down as (overlapping; one def can add several):

| blocker | count |
|---|---:|
| unproven param slots (`?`) | 488 |
| defs with *all* params unproven | 263 |
| unproven return (`?`) | 252 |
| zero-arity defs (emitter requires >= 1 param) | 115 |
| proven return but no ABI: `tuple` | 68 |
| proven return but no ABI: `bool` | 31 |
| param type conflict (`any`, widening) | 24 |
| proven param but no ABI: `nil` | 21 |
| proven param but no ABI: `bool` | 11 |
| proven return but no ABI: `function` | 9 |
| proven return but no ABI: `string` | 5 |

The already-emitting kernels prove the supported set: `:double`,
`:number`, `:long`, `:array` params; `:double`/`:number`/`:array`
returns. 8 kernels emit on this snapshot (`round-half-even`,
`floor-div`, `k-next-word-u64!`, `k-next-word!`, `k-next-pack!`,
`k-next-scale!`, `out-bounce`, `plain-length`); the u64-domain kernel
that initially refused with a slot mix now emits after `bafc7e48`.

## 2. Which types are not proven, and why

PGO observed types on the *unproven/conflict slots* of skipped
functions: `table` 28, `number` 14, `string` 9, `array` 7, `tuple` 6;
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
   snapshot reports **285 `table-open`, 0 `table-flat`** — every ttfx
   table escapes, so flattening never applies.
2. **Proven but no ABI in the emitter** (inference succeeds, emission
   refuses): `bool` params (11) and returns (31); `nil` params (21);
   `tuple` returns (68); `string` returns (5); `function` returns (9);
   `:u64` at the ABI (internal-only by design).
3. **Proof gaps on otherwise-supported types** (`number` 14, `string`
   9, `array` 7, `tuple` 6 observed): two sub-causes:
   - PGO seeding requires a slot's row type set to have *exactly one
     entry*; duplicate identical entries (`@[:string :string]`, seen
     in the SEED debug for `build-effect`) fail the seed, so observed
     numeric/string slots stay `:unknown`;
   - genuine conflicts widen to `:any` (24 slots), e.g. `frame-new`
     slot 1 `array|tuple`, `ctx-anim` slot 1 number conflict,
     `color-fg` slot 0 `array|tuple`.
4. **Zero-arity**: 115 defs (thunks and small void helpers) cannot
   emit because the candidate gate requires at least one param.
5. Not type-related: 169 loops wait on native continuations, 156
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

Counts by family: `src/effects/*` 275, engine (`scene/canvas/color/
rng/easing/...`) 153, `bin/` harness 22.

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
   engine type-skips (90% in `scene`+`canvas`) have an unproven slot 0
   — the ctx/scene/canvas/state receiver.
7. Zero-arity gate (116 defs) is a small independent fix.

## 5. `k-next-word-u64!`: canonical-copy divergence (resolved)

While the Stream F wiring was landing, both CLI and GUI entries
refused the new hinted kernel with `slot s23 mixes u64 with non-u64:
u64/u64ok/number` (diag ids 442/471, dup 791/807), even though a GUI
build a minute earlier had emitted `k_next_word_u64_bang` from the
same `rng-kernels.janet`. The flip tracked which registration channel
became canonical (analysis copy vs real module copy) and its per-copy
bytecode, not the kernel source. `bafc7e48` (u64-domain declaration +
arg-walk fixes) resolves it: the kernel now emits on both entries and
the census is the refreshed one above. The other three u64 site
refusals (`xor64`/`or64`/`rotl64`, "mixes unanalyzable u64load/array")
remain.
