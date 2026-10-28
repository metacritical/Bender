# Matched script / plain AOT / native / trained-release matrix

## Artifact contract

All runtime and workload sources come from clean detached revision `35671edec04292e19a33b97cc3b18942e3e8c676` at `/tmp/janet-wave-controlled-35671ede`. Runtime pair was rebuilt with:

```
make clean
make -j4 NATIVE_EXECUTORS=1 build/bender build/libjanet.a
```

`build-native-mode.log` retains exact commands. Runtime amalgamation is compiled -O2 with debug symbols; bootstrap objects use -O0. Native executor support is built into the same runtime pair used by all four arms. No Clang PGO/LTO flags are applied here.

SHA256:

| Artifact | Hash |
|---|---|
| build/bender | 07e766fb28ab15eb78d563bbdc2e7c9a9b2adf86e187df0d6b705c7d6648151c |
| build/libjanet.a | 7ef37f3fc64672e5646e725d15fe5433877326c460a5d00a5b6c8ca6636734d8 |
| bench/cross-runtime/wave.janet | e07d1f258b05b8b4f26321d4e217dae3623ccce44b0abbf5d8b5ebcd7946feac |
| plain executable | e5494d5bae1c404fda5e7f3d9687e3dae20b604c98fc1409d4c614b781a5dfd0 |
| native-no-PGO executable | 97ccaae09a4db39fa1756386faea4ef2b8b6d781526ff9efb402e4e8e1303988 |
| trained release executable | 9d921b21e2bbf9c4345a3ab0173d801985fc2829c38de27c5da6846803fcc4d2 |

Build commands and environment are in `matrix-builds-cpath.jsonl`. The first plain AOT attempt failed because cc could not find janetconf.h; that failure is retained separately, NOT accepted as a successful build. Setting `CPATH=/tmp/janet-wave-controlled-35671ede/src/conf` explicitly fixed that header search for all subsequent AOT builds. Training and release use the same output basename (`trained`) and isolated XDG cache. The pre-release trained candidate has its own recorded hash and was copied aside; it is not the measured release artifact.

`otool -L` for script executable and all three generated executables lists only libSystem, not a system Janet dylib. Independent source audit of the AOT static library selection remains required before attributing any packaging difference to a particular runtime mechanism.

## Correctness and measurement

Workload is `bench/cross-runtime/wave.janet`, NOT legacy `bench/wave.janet`. It asserts result=N in-program, prints deterministic parity stdout, and prints separate spawn/wave/total milliseconds on stderr. N forwarding actors plus a sink perform real dependent mailbox handoffs. The program is not replaced by a loop.

Two sequential batches were run, never overlapping:

1. `matrix-trials.jsonl`: 48 trials, rotating variant order across rounds, sizes 1000/2000, lanes0/2, three repetitions per arm.
2. `runner-matrix/trials.jsonl`: a separate confirming 48-trial batch using committed `bench/wave-controlled-run.py`, manifest `runner-matrix.json`. Full run provenance and distributions are retained. `runner-logs.tar.gz` contains all 147 run-level and per-trial files, including stdout/stderr and trial metadata; archive members were verified byte-for-byte against the original run directory (archive SHA256 `1e7ec55ecfab065a1cfe4bda5c9f49378878f228881acc9be9925ca135ecc085`). The runner passed 27 synthetic tests independently before this batch. The manifest's per-arm order is fixed, so batch one supplies the rotated-order control.

All 96 trials returned correct result=N with successful status; no failures are dropped. Runtime overrides: JANET_NO_PROFILE=1, JANET_NATIVE_EXECUTORS=1, JANET_LANE_FASTPOST=0, explicit lanes0 or2; JANET_PROFILE removed. Other environment inherited, not hermetic. Machine state is not asserted idle. These small samples identify a reproducible difference; they are not a Go comparison or a million-actor capacity claim.

## Wave-only results: first rotated batch

Median [min,max] milliseconds, three samples each. All raw samples retained above.

| Placement | N | Script | Plain AOT | Native enabled/no PGO | Trained release |
|---|---:|---:|---:|---:|---:|
| local | 1,000 | 101 [99,109] | 281 [270,283] | 278 [271,285] | 278 [271,283] |
| local | 2,000 | 415 [404,438] | 777 [775,789] | 765 [753,796] | 766 [766,769] |
| two lanes | 1,000 | 8 [7,8] | 7 [6,10] | 6 [6,10] | 9 [8,9] |
| two lanes | 2,000 | 16 [15,19] | 14 [12,16] | 13 [12,15] | 11 [10,14] |

The historical roughly 2.7x local release/script difference is reproduced even by **plain AOT**, before native selection or PGO. It is therefore not uniquely a PGO regression. Matching revision and runtime support does not remove the local discrepancy. No meaningful native speedup is established by these data.

## Native refusal evidence

`nopgo-build-cpath.stdout`, `trained-build-cpath.stdout` and `release-build-cpath.stdout` all report `native-lib 0 kernels`. Actor functions are rejected with `types not proven: params [] ret ?`; thunk/while functions report nil. Training collected 31 profile rows and passed parity but did not turn the actor bodies into native kernels. This is observed compiler output, not proof of resumable-native feasibility or impossibility.

## Profiling the plain-AOT regression

A separate plain-AOT local N=4000 run asserted result=4000, exited0, and reported spawn4ms/wave2672ms/total2676ms. `plain4000.sample.txt` retains full stacks: 1652 of1657 main-thread samples under completion-time `janet_collect_fiber_minor`, traversing `janet_mark_fiber`, `janet_procat_mark` and `janet_drain_env_queue`.

This is strong evidence that completion-root traversal dominates the sampled local AOT wave. It does not by itself prove why the image changes the reachable graph or justify skipping cross-owner roots. The next fix must preserve GC reachability, escaped captures, mailbox values and supervisor events; another arbitrary depth cap is not an accepted remedy.

## Remaining work

Finish source-level runtime/static-link audit; explain image vs script root topology; implement and independently review the smallest proven safe collector improvement; rerun identical matrix and scheduler/GC correctness gates. Diagnose actor native refusal beyond the immediate printed reason. Evaluate Clang runtime PGO/LTO separately only after explaining baseline. Larger cross-lane trials require explicit phase markers and resource bounds. Goal remains active.
