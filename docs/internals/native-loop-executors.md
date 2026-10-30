# Generated VM-frame numeric executors (stage 1 and stage 2)

This tier is **off by default**, independent of scalar native wrappers. Build
the runtime with `-DBENDER_NATIVE_EXECUTORS` in CFLAGS; set the environment variable
`BENDER_NATIVE_EXECUTORS=1` when running callgraph or the `--aot --native` driver
to request emission. Both opt-ins are necessary. A runtime built without the
macro refuses registration and has no native-entry checks in its dispatch path.
Do not use fast-math. Generated code disables FP contraction (including GCC's
nonstandard contraction control) and rejects `__FAST_MATH__`.

## ABI, ownership, and registration

`BenderNativeCode` is linked, immutable, executable-lifetime storage containing
an entry function, exact original words, primitive constant descriptions, and
the exact flags/arity/slot shape. It contains no GC references. The definition's
`native_code` association is non-owning; unloading the linked code while any
definition retains the association is outside this ABI.

`bender_native_register(actual_function, descriptor)` checks all words (including
debug bits), primitive constant types and numeric bits, flags, shape, the opcode
allowlist, and the normal bytecode verifier's operands/branch bounds. Failure
clears the association. At native entry/re-entry the exact structural compare
repeats, with the verdict cached per definition per validation epoch (bumped on
resume entry, breakpoint writes and publication) and the verifier gate
amortized per (definition, descriptor) pair — see stage 2. It is not called on
generated internal label transitions.

All original slots remain boxed and GC scanned. C temporaries live only within
one operation. Executors cannot call arbitrary Bender or invoke overloaded
operations. The one direct-call exception is an installed FFI alias wrapper
(stage 2). Its arguments remain on the GC-scanned fiber stack; it uses exactly
`JOP_CALL`'s C-frame discipline even when the C signature has no callback
parameter. The return value is stored in a scanned slot before the VM's
normal collection check. The executor then **exits at the next bytecode PC**
and bumps the validation epoch so any callback/finalizer mutation forces a
new structural comparison. All other callable types exit at the original
unexecuted PC. Native return instructions still exit to the original VM
opcode, preserving frame removal, result delivery, profiling, and caller
resume logic.

Outside that explicit alias-call boundary no allocation or callback is
reachable between entry validation and native exit. After a boxed fallback,
GC, or suspension, the next entry validates again and reacquires slots.
Concurrent unsynchronized C mutation remains unsupported, as in the VM.

## Scheduling and debugging

Every executed nonpositive backedge polls **before** changing the original PC.
Taken conditional edges poll; untaken/forward edges do not. A native direct
FFI-alias call polls before entering the wrapper. If the same call must fall
back to the VM (non-alias callee, trace/profiler/arity guard), the executor
does **not** poll first: ordinary `JOP_CALL` performs its own auto-suspend
poll, so one source-level call consumes exactly one poll in either mode. The
generated poll and interpreter share exactly the same expression, with
auto_suspend short circuiting quantum decrement. On interrupt, the VM commits
the unexecuted jump/call PC and `RESUME_NO_USEVAL | RESUME_NO_SKIP`. Direct
wrapper calls commit the call PC before entry so argument-validation panics
resume there. Resume/cancel signals and longjmp recovery run before native
entry. There is no batching.

`quantum` is an enable flag, not the budget. Each resume resets remaining polls
to `BENDER_QUANTUM_BUDGET` (default remains 200). This compile-time constant is now
overrideable for deterministic budget-1/2 tests. Budget 1 repeatedly interrupts
the same edge without progress, preserving the existing interpreter policy.

Any breakpoint in the definition prevents native entry. First-opcode breakpoint
bypass stays boxed. `bender_step` suppresses all native entries for its dynamic
extent. Generated labels never replace original PCs or debugger metadata.

## Images and emission

`tools/loop-executor.ben` lowers generic disassembled instructions directly to
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
operations, **native control operations**, or **explicit VM boundaries**.

Native control operations are `push`, `push2`, `push3`, `call`, `mkscr`,
`next`, `geti`, and primitive `eq`/`neq`. They have exact fast paths and fall
back to their original PC before effects:

- pushes append to the live fiber argument stack after capacity/max-stack
  checks;
- `call` uses `bender_native_alias_cfun` to recognize only the installed FFI
  alias shape the VM's own fast path accepts, polls once, and calls through a
  GC-scanned C frame. After storing the result and checking GC, it exits at
  the **next** PC for revalidation. A non-alias call exits before effects at
  the current PC for ordinary `JOP_CALL`;
- `mkscr` refills only an already-allocated reusable scratch array with enough
  capacity; first allocation/growth exits to the VM;
- `next` handles arrays, tuples, strings, and buffers with nil/integral keys;
  tables, structs, abstracts, fibers, and non-integral keys exit to
  `bender_next_impl`;
- `geti` handles arrays, tuples, string-like values, and buffers with the
  out-of-range nil rule;
- primitive equality handles numbers and same-typed primitive values;
  composites and mixed types exit to the VM.

Explicit VM boundaries remain `tcall`, `get`, `put`, `len`, `in` (boxed
exception path), `mkarr`, `mkbuf`, `mktup`, and the whole divide/modulo family
(`div`, `divim`, `divf`, `mod`, `rem`). Boundary words are still compared
exactly in `bender_native_matches`; generated code never executes them. Each
region exits with `BENDER_NATIVE_VM` at the boundary's original unexecuted PC
and ordinary dispatch performs the effect exactly once. No native local pointer
or cached value survives the exit. `PUSH` state is on the live scanned fiber
stack, so a fallback after pushes is ordinary VM state.

Constants use an explicit **opaque runtime load** policy. Primitive
(nil/boolean/finite-number) constants keep exact bit matching.
Everything else — functions, cfunctions, keywords, strings, tables,
arrays, buffers, non-finite numbers — is described as
`BENDER_NATIVE_OPAQUE`: the generated `ldc` reads the current pool value
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

The whole-definition compare at a native entry is cached per definition:
the descriptor whose exact match was last proven, plus the validation
epoch at which it was proven. The epoch is bumped on every VM resume
entry, every debugger breakpoint write, and every descriptor
publication, so one long resume pays validation once per definition
while any change made to a definition between resumes (a breakpoint
bit, a raw constant/bytecode edit while parked) forces the compare
again at the next region entry. At every epoch the exact structural
compare runs in full — scalars, every bytecode word (including debug
bits), primitive constant bits and the opcode allowlist. The
`bender_verify` well-formedness gate is amortized to the first
validation of a (definition, descriptor) pair: verify is deterministic
over exactly the fields the structural compare already checks, with
only debug symbolmap data falling outside them, and no supported path
mutates a definition after creation. Registration and lazy attachment
always run the full `bender_native_matches` (structure plus verify).
The cache never replaces the compare; it only skips re-proving a
verdict that nothing could have invalidated. Concurrent unsynchronized
C mutation during a resume remains unsupported, as before.

**Lazy structural attachment**: linked descriptors publish into a
process registry at `native_init`. The registry is indexed by a cheap
exact key — the scalar shape fields plus a rolling hash over every
bytecode word, computed once at publication. An attach attempt hashes
the definition once and runs the full `bender_native_matches` compare
only on a key hit; definitions with no key hit do zero verify work. A
64-bit key collision costs one wasted exact compare (the key is a
filter, never proof); the exact word compare decides. Constant entries
are deliberately not part of the key: opaque descriptor entries assume
nothing about the live pool value, so hashing a definition's actual
constant bits could diverge from an exactly-matching opaque descriptor
and could mask the primitive/opaque ambiguity the search must still
detect. Every definition's first dispatch after publication tries the
unique structurally-matching descriptor once per registry generation.
This covers imported image constants, unmarshaled closures, and
functions evaluated later, with no heap graph traversal. Ambiguous
matches (two descriptors matching one def) refuse; explicit
`executor-register` remains authoritative. `mkimage` additionally
emits direct `executor-register` calls for retained top-level defns,
skipping the search for those; the lazy path serves everything else.
callgraph registers the real disassembly of imported module functions
for emission — the one-thunk analysis copies compile cross-form
references as environment captures and are not faithful descriptors —
and dedupes descriptors structurally instead of by name. The registry
is appended at module init and read on first dispatch; concurrent
module loads from multiple threads publishing while other threads
attach are not synchronized (the same class of limitation as stage 1's
unsynchronized C mutation).

**Counters**: per executor entries/backedges/fallbacks plus `ops`
(native instructions executed) and `vmexits` (boundary/return exits);
per VM attach attempts/attached/ambiguous/nomatch, total VM exits, and
exits by opcode (the opcode at each native-to-VM region exit; boundary
ops reached only via boxed dispatch after another boundary are not
region exits). Counter increments are compiled only when
`BENDER_NATIVE_TELEMETRY` is nonzero; `--release` passes
`-DBENDER_NATIVE_TELEMETRY=0`, while ordinary profiling/AOT builds default
it to 1 so counters remain diagnostic. The stat readers still exist and
report zero when disabled.

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
root with private `BENDER_AOT_TEST_ROOT` and `XDG_CACHE_HOME` set. It uses unique
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
re-priming (`bender_fiber_ctx_prime`, fiber.c:417), reproducing with executors
compiled out. The native executor harness's ASan run passes, but reports ASan's
alternate-stack warning. This is not a clean full shared-stack ASan validation.
No benchmark or application-specific lowering is part of this stage.

## 2026-10-21 release CLI A/B: historical, not a shipping win

The expanded executor prototype (`ttfx-cli-rel6`) was built with
`BENDER_NATIVE_EXECUTORS`, two isolated PGO collections and verified
`--release`; executor telemetry was compiled out. It generated 76 frame
executor descriptors alongside nine scalar native helpers. A same-sitting,
seven-round paired A/B against the already-shipped `ttfx-cli-rel2` used the
unchanged logo, seed 42, 80×24 canvas, and bounded full frame dumps. Five of
five frame streams were byte-identical.

**Do not ship that binary:** the prototype had a signature-derived "leaf"
alias-call shortcut which cleared the scanned argument stack before calling
C. A C call can allocate or callback regardless of its signature. It has
since been removed in favor of the VM's C-frame discipline, GC check, and
post-call revalidation. The numbers below describe the old prototype only;
the corrected feature needs its own release A/B.

| Effect | rel2 ms | executor rel6 ms | rel2 / rel6 |
|---|---:|---:|---:|
| wipe | 49.59 | 51.43 | 0.964× |
| highlight | 50.46 | 51.95 | 0.971× |
| expand | 85.35 | 94.00 | 0.908× |
| randomsequence | 68.85 | 70.62 | 0.975× |
| scattered | 96.23 | 104.08 | 0.925× |

Geometric mean **rel2/rel6 = 0.948×**: the executor candidate delivered 5.2%
less throughput in this CLI sitting. This does not justify shipping it. The
artifacts and raw runs are outside the repositories under the session scratch root
(`ab-bender-cli.json`). The hot `ctx-frame-string` bytecode has 313
instructions, including 43 calls, six `put`, six `len` and three collection
allocations; `ctx-update` contains eight calls and a `put`. Current native
regions exit around most effectful operations, so the profile cannot turn
those loops into one continuous native program. Neither "PGO enabled" nor
"76 descriptors emitted" is evidence of substantial hot-loop coverage.

An independent same-sitting release CLI breakdown bounded output to one
frame and then the whole animation. `rel2` took **16.0→49.4 ms** on wipe,
**14.3→67.9 ms** on randomsequence, and **17.9→96.0 ms** on scattered;
Rust took **4.1→4.4**, **3.8→4.3**, and **4.1→5.4 ms**, respectively.
Bender's own `--bench` time, measured after effect construction and with no
frame dump, was approximately 33/53/76 ms. Thus neither process startup nor
writing the dump alone explains the gap; substantial time remains in scene
stepping and frame-string construction. This is not an isolated VM dispatch
measurement and should not be labeled one.

Closing the CLI gap needs a generic profile-guided AOT IR that can keep
typed/unboxed state across *effectful* calls and allocations, with live
boxed values rooted for GC and precise deopt snapshots at original bytecode
PCs. Calls and backedges must retain the VM's quantum/preemption polling
points. Specialization needs runtime type/shape guards and a boxed fallback;
it cannot rely on ttfx-specific kernels or weaken the parity/preemption
gates. Startup/image initialization is a separate target from playback.

## Corrected default-off runtime and safe release candidate

The unsafe signature-derived leaf shortcut above was removed. Every native
alias call now uses the scanned C frame, stores its return before the normal
GC check and exits at the following original PC with a new validation epoch.
`geti` and `mkscr` are in the runtime opcode allowlist. The six-mode
executor matrix now additionally builds a private shared library with two
struct-by-value aliases (one returns a GC-allocated tuple), compares boxed
and executor image bytes, exercises array/tuple/table reads and loops in
event fibers under GC pressure, and asserts that the executors actually
attach. It passes with ASan/UBSan; no assertion was removed.

The enabled runtime also has a cheap *no-descriptor* dispatch gate: it
starts dormant, activates on explicit registration or descriptor publication,
and accounts for linked registrations surviving `bender_deinit`/`bender_init`.
The same-compiler macro-enabled **default-off** CLI baseline gained
**1.067×** geometric-mean throughput across five effects in seven paired
release rounds. This reduces the cost of compiling executor support into
an otherwise ordinary runtime; it does not make hot loops native.

The **safe** `ttfx-cli-safeR8` candidate (two isolated PGO collections,
verified `--release`, telemetry off) produced identical frame dumps on
five effects. Seven paired release rounds gave **0.989×** throughput
relative to the same-compiler, macro-enabled, no-executor build: neutral
to slightly negative. Compared directly in its own paired sitting with
the previously shipped macro-free `rel2`, it was **0.927×**. Therefore
executor emission remains double opt-in; do not promote these prototypes
as a speedup. Raw samples and build/parity logs are in the approved
session scratch root (`ab-bender-safeR8-vs-base.json`,
`ab-bender-safeR8-vs-rel2.json`). A future compiler must keep type-stable
state across effectful operations rather than adding further per-opcode
entry/exit overhead.

The committed default-off build is `1.43.0-dev-0178fd1f`. Its
PGO-collected, verified `--release` CLI image matches the previous
`rel2` image byte-for-byte on 37 bounded effect dumps and 25 held-out
effect/seed runs. Seven paired rounds give **1.006×** `rel2` throughput
(noise), not a speed claim. The new default-off windowed GUI release
was subsequently verified after waking/holding the macOS display with
`caffeinate -d -u` for two **real** GUI PGO collections and `--release`.
An old `rel2` binary reproduced the earlier crash without that display
hold. This is not an executor speed measurement; executor emission
remains double opt-in.
