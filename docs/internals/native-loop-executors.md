# Generated VM-frame numeric executors (stage 1 and stage 2)

This tier is **off by default**, independent of scalar native wrappers. Build
the runtime with `-DJANET_NATIVE_EXECUTORS` in CFLAGS; set the environment variable
`JANET_NATIVE_EXECUTORS=1` when running callgraph or the `--aot --native` driver
to request emission. Both opt-ins are necessary. A runtime built without the
macro refuses registration and has no native-entry checks in its dispatch path.
Do not use fast-math. Generated code disables FP contraction (including GCC's
nonstandard contraction control) and rejects `__FAST_MATH__`.

## ABI, ownership, and registration

`JanetNativeCode` is linked, immutable, executable-lifetime storage containing
an entry function, exact original words, primitive constant descriptions, and
the exact flags/arity/slot shape. It contains no GC references. The definition's
`native_code` association is non-owning; unloading the linked code while any
definition retains the association is outside this ABI.

`janet_native_register(actual_function, descriptor)` checks all words (including
debug bits), primitive constant types and numeric bits, flags, shape, the opcode
allowlist, and the normal bytecode verifier's operands/branch bounds. Failure
clears the association. `janet_native_matches` repeats this validation at native
entry/re-entry. It is not called on generated internal label transitions.

All original slots remain boxed and GC scanned. C temporaries live only within
one operation. Executors cannot call Janet, allocate, or invoke overloaded
operations. A type guard failure returns `JANET_NATIVE_VM` at the exact current
original opcode; VM dispatch executes that opcode once before another native
entry can occur. Native return instructions also exit to the original VM opcode,
preserving frame removal, result delivery, profiling, and caller resume logic.

No allocation or callback is reachable between entry validation and native exit,
so synchronous bytecode/debugger mutation cannot happen during a generated loop.
After a boxed fallback, GC, or suspension, the next entry validates again and
reacquires slots. Concurrent unsynchronized C mutation is not supported, just as
it is not supported by the ordinary interpreter. No mutation epoch or per-inner-
iteration full-word scan is required.

## Scheduling and debugging

Every executed nonpositive backedge polls **before** changing the original PC.
Taken conditional edges poll; untaken/forward edges do not. The generated poll
and interpreter share exactly the same expression, with auto_suspend short
circuiting quantum decrement. On interrupt, the VM commits the unexecuted jump
PC and `RESUME_NO_USEVAL | RESUME_NO_SKIP`. Resume/cancel signals and longjmp
recovery run before native entry. There is no batching.

`quantum` is an enable flag, not the budget. Each resume resets remaining polls
to `JANET_QUANTUM_BUDGET` (default remains 200). This compile-time constant is now
overrideable for deterministic budget-1/2 tests. Budget 1 repeatedly interrupts
the same edge without progress, preserving the existing interpreter policy.

Any breakpoint in the definition prevents native entry. First-opcode breakpoint
bypass stays boxed. `janet_step` suppresses all native entries for its dynamic
extent. Generated labels never replace original PCs or debugger metadata.

## Images and emission

`tools/loop-executor.janet` lowers generic disassembled instructions directly to
C labels, independently of scalar type inference and PGO. Stage 1 allows
primitive loads, moves, numeric guarded add/subtract/multiply and comparisons,
jumps, and VM returns. Calls, allocations, fields, closures, environments,
varargs, division, and bit operations remain ineligible. Unknown input types are
allowed and guarded. All original instruction offsets are legal entry PCs.

Callgraph writes a separate `native-executors.txt` manifest. These definitions
never enter `native-names.txt`, scalar `emitted-names`, or direct C-to-C lookup.
The classic wrapper loop veto is unchanged. `mkimage` retains supported top-level
defns under their original names and inserts registration with the actual
runtime function immediately after each definition. This also works when there
are zero classic wrappers. Missing top-level targets and mismatches diagnose and
stay boxed. Imported/evaluated definitions are not automatically reattached.

`disasm` additionally exposes exact `:bytecode-words` and `:flags` for descriptors;
the decoded instructions remain the emitter's instruction view. Marshal still
writes only the original representation, never executor addresses. Both normal
allocation and unmarshal initialize the association to NULL. Parked fibers retain
boxed slots/original PCs, and newly deserialized definitions resume interpreted
unless explicitly registered again.

## Stage 2: mixed-operation regions

Stage 2 admits loop bodies whose instructions are either stage-1 native
operations or **explicit VM boundaries**: `call`, `tcall`, `push`,
`push2`, `push3`, `get`, `in`, `len`, `next`, `put`, `mkarr`, `mkbuf`,
`mktup` and the whole divide/modulo family (`div`, `divim`, `divf`,
`mod`, `rem`). Boundary words are still compared exactly in
`janet_native_matches`, but generated code never executes them: each
region exits with `JANET_NATIVE_VM` at the boundary's original
unexecuted PC and ordinary dispatch performs the effect exactly once.
No native local pointer or cached value survives the exit; PUSH stays
in the VM because argument-stack growth can invalidate pointers, and
CALL/TCALL use ordinary VM callable semantics (frame entry/replacement).

Constants use an explicit **opaque runtime load** policy. Primitive
(nil/boolean/finite-number) constants keep exact bit matching.
Everything else — functions, cfunctions, keywords, strings, tables,
arrays, buffers, non-finite numbers — is described as
`JANET_NATIVE_OPAQUE`: the generated `ldc` reads the current pool value
into a scanned frame slot at execution time and the descriptor assumes
nothing about identity or contents. Descriptors therefore contain no GC
pointers, and mutating or swapping an opaque constant is observed live.

**Useful-region reentry**: descriptors carry a per-PC entry bitmap. A PC
is a native entry only if at least three native operations execute
before the next boundary (small definitions of at most 16 instructions
are exempt; their whole-function validation is already bounded). This
keeps whole-definition validation from running per boundary exit or per
single instruction; consecutive VM-only instructions cost one bitmap
check. Returns and unsupported operations also exit before effects.

**Lazy structural attachment**: linked descriptors publish into a
process registry at `native_init`. Every definition's first dispatch
after publication tries the unique structurally matching descriptor
once per registry generation (prefiltered on shape; full
`janet_native_matches` decides). This covers imported image constants,
unmarshaled closures, and functions evaluated later, with no heap graph
traversal. Ambiguous matches (two descriptors matching one def) refuse;
explicit `executor-register` remains authoritative. callgraph registers
the real disassembly of imported module functions for emission — the
one-thunk analysis copies compile cross-form references as environment
captures and are not faithful descriptors — and dedupes descriptors
structurally instead of by name. The registry is appended at module
init and read on first dispatch; concurrent module loads from multiple
threads publishing while other threads attach are not synchronized
(the same class of limitation as stage 1's unsynchronized C mutation).

**Counters**: per executor entries/backedges/fallbacks plus `ops`
(native instructions executed) and `vmexits` (boundary/return exits);
per VM attach attempts/attached/ambiguous/nomatch, total VM exits, and
exits by opcode (the opcode at each native-to-VM region exit; boundary
ops reached only via boxed dispatch after another boundary are not
region exits).

## Verification (stage 2)

The suite adds `test/native-executor-mixed.c` to every configuration:
native-vs-boxed differential parity (results, interrupt PC traces,
exact-once shared-state effects), paired interrupt parking equality
across interleaved boundaries, opaque constant liveness with exact
primitive pinning, GC between resumes across boundaries,
breakpoint/step suppression, parked mixed-frame marshal round trip with
lazy re-attachment, later-evaluated function attachment, counters, and
ambiguous-descriptor refusal. Known baseline limitation (pre-existing,
reproduced with native execution disabled): marshal of a fiber parked
at a call poll drops values between `stackstart` and `stacktop`, so
parked-marshal coverage parks at a backedge with no pending pushes.

Unmodified ttfx acceptance: bounded (40-frame) decrypt and fireworks
across seeds 7, 13, 42, 99, 2026, 314159 produce byte-identical frame
dumps between a no-executor build and a mixed-executor build of the
same branch. Counter probes (a copy of the CLI with import paths
rewritten and counters printed after `faithful-dump`; app sources
untouched) show actual native execution on both effects: decrypt hits
`ctx-frame-string` (~154k entries, ~980k ops, ~152k VM exits),
`ctx-update` and `geom-bezier-length`; fireworks additionally hits
`geom-coord-on-bezier` (~89k entries, ~600k ops) and `ctx-path-step`.
`ctx-path-step` and `geom-coord-on-bezier` are honestly unhit on
decrypt (no path motion in that effect). A few definitions per build
refuse attachment as structurally ambiguous; they stay boxed. No
timing claim is made.

After an enabled build, run `sh test/suite-native-executor.sh` from the repository
root with private `JANET_AOT_TEST_ROOT` and `XDG_CACHE_HOME` set. It uses unique
scratch roots, emits genuine sum/nested/fractional executors, proves image-time
registration and entry/backedge counters through a boxed caller, and builds:

- budget 1 and budget 2;
- portable switch dispatch;
- interpreter interrupts disabled;
- default-off runtime;
- ASan + UBSan.

The C harness compares signals, original PCs, resume flags, and every frame slot;
tests guarded fallback through overloaded arithmetic exactly once (including GC),
wrong types, cancellation, auto_suspend short circuit, interleaved fibers,
parked-fiber marshal, breakpoints/step, bytecode/flag/constant mutation refusal,
taken/untaken conditional backedges, bounded self-edges, primitive loads, and 392
IEEE numeric input pairs (NaN, infinities, signed zero included). Counters count
native entries, attempted backedge polls (including interrupted attempts), and
failed numeric guards, per thread.

The generated image additionally runs two event-loop tasks through shared-stack
resumes, checking exact numeric results and increased native-entry counters,
without sleeps or timing assertions.

Existing PGO multiseed/release/autopgo, array-result, joint-dispatch, hinted-export,
hinted-callgraph, AOT quantum, and UBSan quantum suites passed in the integration
worktree. The existing event-loop quantum suite's ASan run aborts in shared-stack
re-priming (`janet_fiber_ctx_prime`, fiber.c:417), reproducing with executors
compiled out. The native executor harness's ASan run passes, but reports ASan's
alternate-stack warning. This is not a clean full shared-stack ASan validation.
No benchmark or application-specific lowering is part of this stage.
