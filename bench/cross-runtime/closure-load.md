# Capturing-loop load benchmark: native read-only captures

`closure-load.ben` creates two distinct closure objects by calling one factory
with captures 3 and 7. Both share the inner `capturing-loop` definition. Calls
alternate between environments; the 7/15/7 preflight catches stale environment
reuse. Each call sums `i + bias` for `i = 0..n-1`. No arrays or actors are created
in the timed loop. All comments use `#`.

Bounds: integer `1 <= n <= 1000000`, `1 <= rounds <= 1000`, and
`n * rounds <= 10000000` (at most 20 million inner iterations). The exact integer
checksum is `rounds * (n * (n - 1) + 10 * n)`, safely below double's exact range.

Expected stdout (one newline):

- `10000 100`: `n=10000 rounds=100 checksum=10009000000`
- `100000 100`: `n=100000 rounds=100 checksum=1000090000000`
- `2 1`: `n=2 rounds=1 checksum=22`

stderr reports `setup_ms`, `work_ms`, and `total_ms` using monotonic seconds
converted to milliseconds. Setup includes factory calls and three correctness
preflight calls. Work includes only the repeated closure calls and accumulation;
validation/output/counter reporting are outside it. External wall time includes
startup, image loading, validation and teardown, not just these phases.

## Parent-coordinated build, after ldu lands

Do not execute until the source owner signals readiness. Build the runtime ONCE
with `make all NATIVE_EXECUTORS=1 -j4` (parent owns this step). Do not rebuild
between either artifact or measurements. Record revision, dirty diff, flags,
SHA256 and size of `build/bender` and `build/libbender.a` before and after.
A dirty source tree means this is not an unqualified c2917f65 result.

From `/tmp/janet-clo`, using a fresh cache directory and identical output basename:

```sh
mkdir -p /tmp/clo-load/{boxed,native,logs,cache}
export XDG_CACHE_HOME=/tmp/clo-load/cache
./build/bender --aot bench/cross-runtime/closure-load.ben -o /tmp/clo-load/boxed/closure-load > /tmp/clo-load/logs/boxed-build.log 2>&1
BENDER_NATIVE_EXECUTORS=1 ./build/bender --aot bench/cross-runtime/closure-load.ben --native --no-auto-pgo -o /tmp/clo-load/native/closure-load > /tmp/clo-load/logs/native-build.log 2>&1
shasum -a 256 build/bender build/libbender.a /tmp/clo-load/boxed/closure-load /tmp/clo-load/native/closure-load
wc -c build/bender build/libbender.a /tmp/clo-load/boxed/closure-load /tmp/clo-load/native/closure-load
```

`NATIVE_EXECUTORS=1` at make time enables runtime support;
`BENDER_NATIVE_EXECUTORS=1` on the AOT compiler invocation enables executor
**emission**. Neither setting alone proves native execution. This pair is boxed
AOT versus native AOT, NOT script versus AOT or a PGO/release experiment.

## Attachment/entry evidence gate

Preserve the native build's `native-executors.txt` metadata and identify the exact
ID named `capturing-loop` (not `make-capturing-loop`, a wrapper, or another loop).
The file location is compiler-generated: record it rather than guessing an ID.
Retain `native-executor`, `native-skip`, `native-lib`, and registration refusal
lines from the complete build log. Kernel count alone cannot pass this gate.

With `ID` set to that verified integer, run the following commands through the
parent's bounded process runner (120 s wall cap per command, terminate only that
process group; a cap is a failure, not a timing). Do not run these bare on a loaded
machine without the cap:

```sh
/tmp/clo-load/boxed/closure-load 10000 100
/tmp/clo-load/native/closure-load 10000 100 "$ID"
```

Native stderr must show `executor-target id=ID entries>0 backedges>0 ops>0`;
the benchmark asserts those fields. Global `attached>0` plus no registration
refusal supports lazy attachment, but is NOT target-specific proof by itself.
The exact-ID entry counter is the target proof. If no target executor is emitted,
report refusal and do not invent an ID or claim acceleration. Preserve counters
and stdout for every evidence run. If future ldu counter APIs differ, adapt only
after confirming the actual contract with the source owner.

For clean performance samples omit ID from BOTH arms (same arguments), then run
the evidence command separately. Run at least three paired repetitions in
boxed/native, native/boxed, boxed/native order; sequentially, never concurrently.
Capture machine load/swap before each pair, wall time, stdout, stderr, exit code,
artifact hashes/sizes, and validate the expected stdout on EVERY run. Keep
`BENDER_NO_PROFILE` unset on both AOT arms. Optionally measure the script with
`BENDER_NO_PROFILE=1 ./build/bender bench/cross-runtime/closure-load.ben 10000 100`
using this same runtime, but report separately. Repeat with `100000 100` only if
the smaller case completes comfortably within the cap. Report median and min/max
wall and work phase timings separately. Do not interpret parity, emission count,
or startup differences as proof of loop acceleration.

## Acceptance and bounded measurement

Source implementation/regressions: `9fad7d73` (based on `c2917f65`). Executor ID
2 was emitted for `capturing-loop`. The 10000/100 evidence run passed checksum
10009000000 and reported 10253 entries, 2010056 backedges, 14010903 native ops,
zero guards, and 203 VM exits. This proves the target entered; that standalone
probe is not used as a performance sample.

`closure-load-run.py` measures already-built artifacts in three alternating
sequential pairs, checks exact stdout and exits, writes raw output files directly
(no pipe deadlock), captures load/swap per pair, and enforces a 120-second bound
per direct AOT child. Its output directory must not already exist, so it cannot
overwrite previous logs. Example:

```sh
python3 bench/cross-runtime/closure-load-run.py /tmp/clo-load/boxed/closure-load /tmp/clo-load/native/closure-load /tmp/clo-load/pairs-10000 10000 100
```

The two artifacts intentionally compare boxed AOT to native AOT with the new
capture path, NOT different runtime builds or PGO variants. Native telemetry is
enabled in this proof build; release performance is a separate question.

## Measured results

Three alternating sequential pairs per size, 100 rounds. Times in milliseconds:

| n | arm | median work (min–max) | median process wall (min–max) |
|---|---|---|---|
| 10000 | boxed AOT | 45.368 (44.835–65.400) | 77.020 (76.464–832.051) |
| 10000 | native captures | 19.337 (19.182–23.617) | 39.860 (38.269–41.177) |
| 100000 | boxed AOT | 450.683 (449.380–451.298) | 463.183 (458.501–505.425) |
| 100000 | native captures | 185.720 (184.727–186.312) | 242.423 (233.631–243.153) |

Median in-program work speedup: **2.35x** at 2 million iterations and **2.43x**
at 20 million. All 12 runs exited 0 and returned the exact expected checksum.
The 832ms first boxed process-wall sample is an observed outlier; its cause is
not established. Use the separately recorded work phase, not startup-inclusive
wall time, to assess loop acceleration. Machine load/swap per pair is in JSON;
other system activity is not controlled, and three pairs do not establish
performance across arbitrary game workloads.

Full raw evidence (stdout/stderr, individual timings, artifact hashes, build and
suite logs): `bench/results/closure-load-c2917f65/`; `sha256.json` pins retained
files. Artifacts themselves are not embedded.

## Verification and limits

- Allowlist contract passed: 61/65, with the same signed-shift/uint32 exclusions.
- Closure image suite passed: live-stack and detached array captures, distinct
  instances sharing one definition, GC across loads, actual entry deltas, and
  `setu` exclusion/interpreter parity.
- C guard suite passed: captured-slot index out of bounds, invalid negative env
  normalization, original-PC/destination-preserving fallback, environment-count
  verifier checks after epoch invalidation, and ordinary zero-environment control.
- `make NATIVE_EXECUTORS=1 test` exited 0 with normal profiling environment.
  An initial run with global `BENDER_NO_PROFILE=1` exited 2 because a suite tests
  automatic-profile publication; that failed log is retained, not hidden.
- `make NATIVE_EXECUTORS=1 test-executors` exited 0, including forced quantum,
  portable dispatch, disabled support and sanitizer configurations.
- Static test-gate discloses broad header blast radius/unmodelled callers;
  these passing suites are not exhaustive certification. Quality-delta flags
  added verifier bounds-check complexity/churn and the new long C test main;
  these are retained checks, not removed to improve the score.

This supports read-only `ldu` execution in otherwise eligible functions only.
Capture writes (`setu`), creation (`clo`), nested definitions/NEEDSENV, varargs
and new direct native-to-native closure continuations remain excluded. A
captured mutable container can still be read: read-only describes the upvalue
operation, not global immutability of its referent.
