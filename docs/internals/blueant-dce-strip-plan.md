# B7.2 Part B, Piece 1c — wire the `dce-reachable` verdict into `mkimage.ben`'s stripping decision

> Historical implementation plan, reviewed against `5180da8a` on 2026-10-18.
> Piece 1c landed in `a5c66600`; related 1a/1b are `edbeeb67`/`6cade6a0`.
> The structural `contains` description below is historical: later analysis-only
> retention follows surviving closure opcodes rather than rooting every retained
> child. Preserve the original result: synthetic exclusions tested plumbing while
> the conservative end-to-end example did not prove general dead-code removal.
> Old execution and workspace instructions are not current directives.

## Critical finding established before writing code (probed, not inferred)

`tools/callgraph.ben`'s `roots` = every def named `thunk`, and
`reachable-from` follows BOTH `calls` and `contains` edges.
`contains` edges are emitted from each def's structural `(d :defs)`
nesting (callgraph.ben ~line 658-664), so **every top-level `defn`
that survives compilation is a structural child of its unit's thunk
and is therefore ALWAYS reachable**. Probed exhaustively (7 fixture
shapes: plain unused defn, chained unused, dead-branch reference,
nested inner fn, `def` alias, value-use, cross-module import):
every `:safe` verdict prints `excluded -`. A genuinely unreferenced
top-level defn is dropped by the compiler *before* analysis (the
honest finding Piece 1b already documented); one made to survive
(by using it as a value) trips pattern 2 and yields `:unsafe`.

Consequence: Piece 1b's `excluded` set is **always empty for every
safe program**. The wiring must therefore be built and proven as
correct *plumbing* — it cannot be proven to strip a real function
through the unmodified Piece 1b verdict, because no safe program
produces a non-empty exclusion set. Inventing a different root set
to force an exclusion would be Piece 2's job (precise supervision
roots) and is explicitly out of scope here.

## Approach: land the full plumbing, prove it with a synthetic exclusion injection

1. **Flag**: `--dce` in `src/boot/boot.ben`'s `aot-compile` arg
   loop (same shape as `--native`), threaded to `callgraph.ben`
   as `--dce` and to `mkimage.ben` as `--dce-excluded FILE`.
2. **callgraph.ben**: when `--dce` is set AND the verdict is
   `:safe`, write `<ndir>/dce-excluded.txt`, one bare def NAME per
   line, excluding the thunk and any name that is not a plain defn
   name. When `:unsafe`, write nothing (file absent ⇒ identical to
   flag-absent from mkimage's perspective).
3. **mkimage.ben**: parse `--dce-excluded FILE`, load into
   `dce-excluded-names`, and add a SECOND, INDEPENDENT check in the
   existing `(each f forms ...)` stripping loop that drops a
   `defn`/`defn-` form entirely when its name is in that set. The
   existing `native-names` branch is not touched. The loop must now
   run when EITHER set is non-empty (today it is gated on
   `(when (next native-names) ...)`).
4. **Testing** (`test/suite-dce-strip.ben`, modeled on
   `test/suite-dce-reachable.ben`):
   - **Regression (most important)**: a normal `--native` build
     without `--dce` produces byte-identical C and binary output vs
     HEAD behavior; `dce-excluded.txt` is never created.
   - **Negative**: a pattern-1/2/3 fixture built with `--dce`
     creates no exclusion file and produces a binary byte-identical
     to the same source built without `--dce`.
   - **Positive (plumbing)**: because no safe program yields a real
     exclusion, prove the mkimage half directly by invoking
     `mkimage.ben` with a hand-written exclusion file naming a
     genuinely unused function, confirming (a) the emitted C no
     longer contains that function's body, (b) `nm`/`strings` on the
     linked binary shows the symbol/name absent, (c) the Piece 1a
     cross-check harness PASSes on that name.
   - **Verdict-plumbing**: a safe fixture built with `--dce` writes
     the (empty) exclusion file and strips nothing — honest
     confirmation that end-to-end the safe path is a no-op today.
5. **Verification battery**: all 16 suites must match HEAD
   (`6cade6a0`) pass counts exactly.

## Hygiene
- Never touch `implementation_plan.md`.
- Do not `git commit`.
- Confirm `implementation_plan.md` absent from `git status --short`.
