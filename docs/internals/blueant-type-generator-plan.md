# Bender Type-Generator Extension — C Codegen for High-Level Types + Flip-Type Dispatch

> Historical design, reviewed against `5180da8a` on 2026-10-18. Phases 1–5
> landed with narrower scopes in `e8763040`, `9356e218`, `10ec22df`, `09eefd05`,
> and `cbdfffe6`. Phase 5 is local closed-shape table flattening, not general
> whole-program record-shape inference. Unanimous PGO samples are observations,
> not type proofs; specialization needs guards and fallback. The embedded-NUL
> “N/A” note below is historical and incorrect: Bender strings can contain NUL.
> Current scope/status: [ttfx-driven-compiler-work.md](ttfx-driven-compiler-work.md).

Grounded in direct code inspection (prior session): Bender/Bender has 16 canonical
types (`BenderType`, `bender.h:518-535`). The inference lattice and PGO collector
(`tools/infer.ben`, `vm.c:404-625`) are already type-agnostic and can PROVE
non-numeric monomorphic types from static call-site evidence or from unanimous
PGO observation. The gap is entirely in `tools/callgraph.ben`'s C-emission
layer: `emit-c-type-map` (`callgraph.ben:170-175`) has no entry, and no kernel
primitives exist, for 9 of the 16 types (`STRING, SYMBOL, KEYWORD, TABLE,
STRUCT, BUFFER, FUNCTION, CFUNCTION, FIBER`), and there is no switch/dispatch
codegen tier for genuinely polymorphic ("flip") slots — today's emitter only
ever unifies ambiguous numerics to `double` or refuses to compile.

This plan closes those gaps in five ordered, independently-gated phases, safest
and smallest first. Every phase gets its own new regression test suite file
(matching the existing `test/suite-infer-union.ben`/`suite-aot-*.ben`
pattern) so each capability is locked in permanently, not just verified once.

---

## Phase 0: shared discipline (applies to every phase below)

- Root-cause/design each phase by direct code reading before writing code —
  same discipline that caught the phantom-keyword bug and the false
  matrix-parity claim earlier in this engagement.
- Dual-target verification (interpreter + `bender --aot --native`) on every
  commit.
- One commit per phase, with measured before/after native-skip counts and a
  new, committed test suite file proving the capability.
- Never force an unsound representation: if a phase's safe scope turns out
  narrower than hoped (as A2.2/A2.5 found for ease/on-callback), say so
  honestly and land the narrower, correct version rather than overreaching.
- Coordinate on shared files: this all lives in `tools/infer.ben` and
  `tools/callgraph.ben`, the same files Stream A's A2.1 fix touched — land
  phases sequentially (not in parallel subagents on the same files) to avoid
  the kind of collision risk flagged throughout this engagement.

---

## Phase 1: quick win — `:nil` return → C `void` (do this first, smallest, safest)

Today `emit-fn-c`'s gate (`callgraph.ben:1679`) rejects any function whose
return type isn't numeric or `:array` — including functions PROVEN to always
return `:nil`, which is common (side-effecting functions). A `void`-returning
native kernel is simple and always correct for such functions.

- Change: accept `rt = :nil` in the native-eligibility gate; add `:nil →
  "void"` to `emit-c-type-map`; adjust the wrapper emitter so a `void`
  kernel returns `bender_wrap_nil()` to the boxed caller instead of trying to
  rebox a C value that doesn't exist.
- Test: new `test/suite-native-void.ben` — a handful of functions with
  provably-nil returns (no params, numeric params, array params) must compile
  native and be called successfully (both as entry point and as a callee from
  another native function).
- Gate: existing suites unchanged; re-run ttfx's AOT build and confirm some
  previously-:nil-skipped functions (e.g. from the effects that reported `ret
  nil` native-skips during Stream A) now compile.

## Phase 2: `:string` type — lattice leaf + C representation + core kernels

`BenderString` is already `typedef const uint8_t *` with a length-prefixed
header (`BenderStringHead`, `bender.h:497,585`) — i.e. already a flat C value;
no representation transformation needed, only a lattice leaf, a C type-map
entry, and kernel primitives for what code actually does with strings.

1. `tools/infer.ben`: add `:string` as a tracked leaf (mirror how `:array`/
   `:tuple` are tracked at `'ldc`/construction sites — string LITERALS get
   `:string` directly; `'call`-resolved string-returning core functions
   propagate it; conflicting evidence widens to `:unknown` as usual, never
   forced).
2. `tools/callgraph.ben`: `emit-c-type-map` gets `:string → "BenderString"`
   (or an equivalent typedef). Add native kernel primitives for at least:
   string equality (`bender_string_equal` or `memcmp`-based), string length,
   byte indexing. Do NOT attempt general string-building/concatenation in
   this phase — that needs buffer-growth semantics and is riskier; scope to
   read-only string operations first (equality/length/index), which is
   exactly what `ease`'s `case`-dispatch and similar code need.
3. Test: new `test/suite-native-string.ben` — functions taking/returning
   proven-`:string` params, string-equality-based branching, byte indexing;
   verify native compilation + byte-identical output vs interpreter for a
   representative set of inputs including edge cases (empty string, string
   with embedded nul... N/A for Bender strings which carry explicit length,
   but test multi-byte/UTF-8 content since Bender strings are byte-oriented).
4. Gate: re-run ttfx's AOT build; `ease`'s `name` param, once ALSO flip-
   dispatchable (Phase 4) or hinted, should no longer need to stay fully
   boxed purely for lack of a string C type.

## Phase 3: `:keyword`/`:symbol` types — interned-pointer equality

Confirmed: Bender interns both symbols and keywords (`src/core/symcache.c`),
so equal keywords/symbols are pointer-identical — equality is a raw pointer
compare, cheaper than string comparison.

1. `tools/infer.ben`: add `:keyword`/`:symbol` leaves (symmetric to
   `:string`'s treatment; keyword literals already appear in bytecode via
   `ldc`, this phase gives that existing data a real destination type
   instead of leaving it to flow into `:any`/`:unknown` as today).
2. `tools/callgraph.ben`: C type-map entries (reuse `BenderString`'s
   underlying representation — symbols/keywords share the string layout);
   kernel primitive for pointer-identity equality (`==` on the interned
   pointer, NOT a byte compare — this is the cheap path your message asked
   about).
3. Test: new `test/suite-native-keyword.ben` — keyword-switch-style
   dispatch functions (the exact shape `ease`'s `case` uses, but on keywords
   instead of strings, since ttfx's actual call site turned out to pass
   runtime-string values per the earlier investigation — this test proves
   the mechanism generally, independent of whether ttfx itself ends up using
   it for `ease` specifically).

## Phase 4: switch/dispatch codegen for flip-able (genuinely polymorphic) slots

This is the direct answer to "create internal switches for multiple types for
a thing that might flip." Builds on Phases 1-3 (needs concrete C types to
dispatch BETWEEN) and on PGO's existing type-mask collection (`vm.c:607-610`,
already records every observed type per slot, not just numeric).

1. Generalize the existing element-VALUE sampling (`vm.c:616-623`, built
   for `:array` elements, 1-in-8 sampled) to also sample concrete observed
   VALUES (not just types) for scalar string/keyword-typed slots — gives a
   finite, closed observed-value set per slot, not just an observed-type
   set. This is new C-side collection work in `vm.c`, gated the same way
   existing sampling is (first-16-calls-exact, then 1-in-8).
2. `tools/callgraph.ben`: when a slot's PGO-observed type mask has more
   than one bit set (genuinely polymorphic, not just unproven) AND every
   observed type has a concrete C representation (Phases 1-3), emit a
   dispatch wrapper: `switch (bender_type(x)) { case BENDER_LONG: ...
   specialized kernel...; case BENDER_KEYWORD: ...; default: fall back to
   fully boxed interpreter call }`. The `default` arm is mandatory and must
   be reachable at runtime (a value outside the observed closed set at
   runtime correctly falls back, never crashes) — this is the safety
   argument that must hold before this phase can be considered complete.
3. Test: new `test/suite-native-dispatch.ben` — a function exercised
   under PGO with genuinely mixed argument types across calls (e.g.
   sometimes long, sometimes double, sometimes keyword), confirm the
   generated dispatcher (a) chooses the right specialized path for each
   observed type, (b) correctly falls back to boxed for an UNOBSERVED type
   at runtime (construct a call with a type that was never in the PGO
   training run and confirm correctness, not a crash) — this out-of-sample
   fallback test is the single most important test in this whole plan.
4. Gate: full existing suite battery (this phase touches shared dispatch
   paths) plus the new suite; measure whether this closes any of Stream A's
   previously-found-ambiguous functions (the "conflicting call sites widen
   to :number" cases).

## Phase 5: automatic table/struct shape inference → flat C struct codegen

The biggest, riskiest phase — last on purpose. Bender already HAS the
manual mechanism (`ffi/defstruct` + `:kernel` metadata, `callgraph.ben:
184-209,1622+,2233`, confirmed working via direct read) for a hand-declared
flat struct with direct `:pread`/`:pwrite` native-memory access. What's
missing is automatically discovering that a table is eligible for this
treatment.

1. Whole-program shape inference: for every table/struct CONSTRUCTION site
   (literal `@{...}`/`{...}`, or a `put`-sequence immediately following a
   `@{}`/`table/new` that always writes the same fixed key set), record the
   key set and each key's inferred value type (recursively using Phases 1-4
   for each field). A table's shape is "closed" only if EVERY construction
   site reaching a given use shares the identical key set and per-key
   types — same monotone-widening discipline as the existing lattice (any
   site with a different key set or incompatible per-key type widens the
   whole table back to fully boxed, never forces a wrong layout).
2. For a proven-closed shape, auto-generate the equivalent of today's
   hand-written `:kernel` metadata (field name → offset/type mapping) and
   rewrite `(tbl :key)` gets / `(put tbl :key v)` puts that provably operate
   on that shape to direct `:pread`/`:pwrite` native-memory access,
   reusing the EXISTING, already-correct intrinsic-access codegen at
   `callgraph.ben:1622+` rather than inventing a new access mechanism.
3. Hard safety constraint: a shape-inferred table must still behave
   correctly if used in a context the shape inference didn't see (e.g.
   passed to `pairs`/`keys`/generic reflection, or merged with another
   table) — scope Phase 5 to START with tables that are ONLY ever accessed
   via known-constant-key get/put throughout the whole program (closed-
   world, same as Bender's `--aot` image already assumes); explicitly
   detect and exclude (fall back to boxed, never force) any table that
   escapes to generic table operations.
4. Test: new `test/suite-native-struct-shape.ben` — closed-shape tables
   get flattened and operate correctly; a deliberately-not-closed table
   (two different construction sites with different key sets reaching the
   same use, or one use via generic `pairs`) must NOT be flattened and must
   fall back to boxed — this negative test matters as much as the positive
   one, mirroring A2.2's honest no-go discipline.
5. Re-run the ttfx AOT build as the real-world validation: does this close
   `ctx-motion-move`/`gradient-map-get`/the `*-on-callback` closures (the
   specific functions Stream A found blocked exactly on table-typed
   params)? Report honestly if ttfx's actual tables turn out NOT closed-
   shape (as A2.2 found for `:ease` specifically, tracing to open runtime
   config input) — Phase 5's value may be realized on OTHER tables in ttfx
   or other programs even if not on the exact three functions already
   investigated.

---

## Sequencing and ownership

Phases 1-4 are low-to-medium risk and can proceed in strict sequence by
direct work or single-subagent delegation per phase (never two phases
touching `tools/infer.ben`/`tools/callgraph.ben` concurrently). Phase 5
is the biggest lift and should only start once Phases 1-4 are committed and
green — its own internal steps (1-5 above) are themselves sequential.

## Non-negotiable gates (carried over from the whole engagement)

- Every phase: dual-target verify, full existing regression battery stays
  green, new suite file committed alongside the capability (not after),
  honest reporting of any phase found narrower-than-hoped in scope.
- Never implement Phase 4's dispatch or Phase 5's shape-flattening without
  the fallback/escape-hatch path being tested as a FIRST-CLASS test case,
  not an afterthought — both are the exact kind of soundness risk that
  broke the project's one blind-patch attempt in Stream B's history.
