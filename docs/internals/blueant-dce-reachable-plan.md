# Piece 1b: `dce-reachable` whole-program DCE safety gate

> Historical implementation plan, reviewed against `5180da8a` on 2026-10-18.
> Piece 1b landed in `6cade6a0` (related 1a/1c: `edbeeb67`, `a5c66600`).
> Instructions and source line numbers below reflect that session. A passing
> conservative safety gate is not evidence of effective whole-program stripping;
> see [blueant-dce-strip-plan.md](blueant-dce-strip-plan.md).

Implement a NEW function/block in `tools/callgraph.janet` (appended at end of script, after the existing fixpoint machinery) — never modifying `reachable-from` itself:

- **Pattern 4 (unrecognized op):** whitelist of ops confidently handled by this file's emission/inference walks; any other op in any defn's bytecode → `:unsafe`.
- **Pattern 1 (fn stored in container):** forward linear walk per def tracks function-value slots (producers: `ldc` of function/cfunction constant, `lds` self, `clo`). Violation when a fn slot is used as `put` value (ins 3) / `puti` value (ins 2), is pending-pushed when a container constructor (`mkarr`/`mktup`/`mkbtp`/`mktab`/`mkbuf`/`mkstr`/`mkstu`) consumes it, or is pending across any non-consuming, non-call op (conservative unknown use, incl. `ret`).
- **Pattern 2 (fn as call argument):** fn slot in pending pushes at a `call`/`tcall` (callee slot excluded — it resolves statically via `record-call`).
- **Pattern 3 (dynsite taint):** if the existing `tainted` fixpoint flags anything (any `dynsite` reachable backwards) → `:unsafe`.
- **Verdict:** any violation → print `dce unsafe ...` diagnostic citing pattern/def/pc, compute nothing more. Otherwise run the existing `reachable-from` on the existing `roots` and print `dce safe ...` + the reachable id set (excluded names listed).
- No wiring into `tools/mkimage.janet` (Piece 1c).

Testing: /tmp fixtures (safe+unused fn; fn stored in table; fn passed as arg), run via `./build/bender tools/callgraph.janet`, plus crosscheck harness on fixture 1's reachable set. Permanent suite `test/suite-dce-reachable.janet` modeled on `test/suite-dce-harness.janet`. Full regression battery must match HEAD (`edbeeb67`) pass counts. Never touch `implementation_plan.md`; no commits.
