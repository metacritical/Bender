# Phase 4: native wave-actor feasibility at `35671ede`

**Scope and verdict.** This is a read-only feasibility record, not an implementation or a claim of native actor parity. The matched workload is **`bench/cross-runtime/wave.ben`**, not `bench/wave.ben`: its sink receives and reports at line 23; each forwarding closure captures `downstream` and evaluates `(proc/send downstream (+ 1 (proc/recv)))` at line 28. The chain sends its initial value at line 33 and asserts `result = n` at line 37. The committed matrix identifies its clean detached source revision and build provenance in [`bench/results/phase4-controlled-35671ede/matrix/README.md`](../../bench/results/phase4-controlled-35671ede/matrix/README.md), lines 3–27, 29–38. No actor body was emitted as an unboxed native kernel in either observed build.

## Observed compiler refusal, not an inferred diagnostic

The committed [no-PGO compiler output](../../bench/results/phase4-controlled-35671ede/matrix/nopgo-build-cpath.stdout), lines 1–4 and 21–28, discovers four definitions: top-level `thunk` (0), sink `<anon-1>` (1, `proc/recv` and `ev/give`), `while` (2), and forwarder `<anon-3>` (3, `proc/recv` and `proc/send`). Lines 40–44 report exactly:

```text
native-skip 0 thunk types not proven: params [] ret nil
native-skip 1  types not proven: params [] ret ?
native-skip 2 while types not proven: params [] ret nil
native-skip 3  types not proven: params [] ret ?
native-lib 0 kernels
```

The committed [trained-release compiler output](../../bench/results/phase4-controlled-35671ede/matrix/release-build-cpath.stdout), lines 38–45, records `profile 31 rows`, the same four skips, and `native-lib 0 kernels`. The printed `types not proven` reason is the **scalar native-library** gate: `tools/callgraph.ben:5273–5297` requires concrete signatures, including at least one parameter, and `:5396–5398` prints the refusal. In particular, a reported `ret nil` does not make these zero-argument thunks scalar-library candidates. These logs do not provide a separate per-definition executor-ineligibility diagnostic; do not relabel the scalar `native-skip` line as one.

## Distinct executor and suspension boundaries

The boxed-frame executor is a separate tier. `tools/loop-executor.ben:59–82` requires accepted bytecode and **empty captured environments and nested definitions**; the source-level forwarder captures `downstream`, so its compiled capture shape must be checked before any executor claim. Leaf selection additionally requires a profile/call threshold and a usable entry region (`tools/callgraph.ben:5518–5566`); a profile row alone does not admit the body. Even in an admitted executor, an unsupported call exits at its *unexecuted* PC for ordinary VM execution (`tools/loop-executor.ben:409–416,458–496`). A selected native-to-native continuation applies to a runtime-verified `BENDER_FUNCTION`, with no captured environments, an allowed PC-0 region, valid arity and untraced/unprofiled state (`src/core/vm.c:462–529`); it is not a generic C-function or mailbox continuation. The trampoline chains real Bender frames with a bounded budget, revalidates on re-entry, and holds no roots or locks that a panic could leak (`src/core/vm.c:602–644`). The VM owns ordinary C-function `JOP_CALL` dispatch and its C frame (`src/core/vm.c:2605–2616,2685–2693`).

The mailbox calls are effectful and potentially suspending. `proc/send` can await a full local or forwarded channel (`src/core/proc.c:1313–1358`); `proc/recv` checks process identity and awaits after channel pop/parking (`src/core/proc.c:1395–1459`). `bender_await` raises an event signal (`src/core/ev.c:927–931`), rather than returning a pure number that can be folded into `(+ 1 ...)`. A native shortcut must preserve dispatch, channel capacity, lane routing, the receive value, and exactly-once effects across suspension; an ordinary native-to-native call does not establish those properties.

## Rooting and exception obligations for any later design

Current continuation pushes a genuine `BenderFiber` frame and commits the caller PC before entry; `bender_fiber_funcframe` checks arity before mutation, can resize the stack, nil-fills unset locals for GC, and records `func`/`pc` (`src/core/vm.c:509–529`; `src/core/fiber.c:765–800`). The collector marks resumable fiber argument slots, frame functions/environments and live frame values via `prevframe` (`src/core/gc.c:648–685`); it also marks proc identity and in-flight resume/payload fields (`:600–635`). Values surviving an await cannot merely live in unscanned C locals. `src/core/capi.c:70–92` delivers signals through the try/longjmp chain; `src/core/vm.c:2161–2184` restores frame/PC and handles resumed signals and longjump recovery. Any native suspension or error path must preserve the original call PC, return slot, once-only mailbox effect, rooted capture/arguments/payload, and existing panic/trace semantics. These are proof obligations, not verified support for the actor body.

## Bounded follow-up; no parity promise

1. **Characterize without extending:** pin the forwarder and sink disassembly, actual capture metadata, call PCs and result slots, plus the separate executor eligibility/selection reason. Keep the observed scalar-library `native-skip` distinct from executor selection.
2. **Specify a resumable boundary:** write the state transition for empty receive, full send, successful receive/send, cancellation and panic, identifying exactly where the VM commits the PC, roots `downstream` and message values, and re-enters after scheduler delivery. Retain VM fallback for any unproved shape; do not call a potentially suspending mailbox function as if it were an ordinary chainable native callee.
3. **Gate a later implementation independently:** require interpreter versus AOT correctness for local and cross-lane handoffs, full-mailbox backpressure, suspended GC stress, cancellation/panic stack traces, tracing/profiling and dynamically rebound calls, followed by the same four-mode matrix. No extension code or native-parity claim is authorized by this note.

The [committed matrix summary](../../bench/results/phase4-controlled-35671ede/matrix/README.md), lines 40–61, reports correct results for both batches and a local script-versus-*all*-AOT difference even with zero native kernels. Its separate plain-AOT sample shows completion-time GC root traversal, but does **not** establish why the image changes the reachable graph or identify a safe collector fix. Do not attribute that difference specifically to PGO or to the absent native actor kernel.
