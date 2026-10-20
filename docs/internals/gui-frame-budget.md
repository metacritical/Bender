# ttfx GUI frame budget (measured 2026-10-20)

Where does the remaining Janet-vs-Rust gap live? This note answers that
with main-thread samples of the two GUIs on the same effects, then states
the concrete budget for the "97% of Rust" target.

## Method

- Engines: canonical release Janet GUI
  (`ttfx/janet-port/build/ttfx-gui-rel`, PGO collect x2 + verified
  `--release`) and the Rust GUI (`ttfx/target/release/ttfx-gui`).
- Window: 1458x400, windowed, uncapped, `--show-fps`, seed 42,
  `ruby-port/logo.txt`, `--probe-seconds 6`.
- Profile: `sample <pid> 3 1` during the probe (1 ms, main thread).
  Subtree sample counts are converted to microseconds with the probe's
  own fps: `us = share * 1e6 / fps`.
- EndDrawing is the raylib swap/present subtree
  (`swapBuffersNSGL -> CGLFlushDrawable -> glSwap_Exec ->
  gldPresentFramebufferData -> SkyLight/WindowServer`); it is GPU
  driver + compositor time, not raylib batch building.
- Only paired same-sitting runs are comparable. Absolute fps drifts
  10-20% with machine load (the bouncyballs sample below ran at 2364 fps
  in the sampling sitting and ~2800 fps in a later A/B sitting).

## Frame table

Janet columns: EndDrawing and prep are disjoint and sum to the frame.
Pure VM and draw-text split the rest of prep (plus 1-2% other FFI);
"pure VM" is `run_vm` minus all FFI alias subtrees, and it includes GC
(GC's own share is shown separately). "draw-text" is the
`rl-draw-text-ex` alias subtree.

| effect | engine | fps | frame us | EndDrawing us | prep us | pure VM us | draw-text us | GC us |
|---|---|---:|---:|---:|---:|---:|---:|---:|
| blackhole | janet | 2594 | 386 | 187 (49%) | 198 | 131 | 58 | 19 |
| blackhole | rust | 3693 | 271 | 186 (69%) | 85 | - | - | - |
| scattered | janet | 1461 | 684 | 260 (38%) | 425 | 198 | 212 | 34 |
| scattered | rust | 2346 | 426 | 207 (49%) | 219 | - | - | - |
| laseretch | janet | 2294 | 436 | 280 (64%) | 156 | 92 | 52 | 17 |
| bouncyballs | janet | 2364 | 423 | 275 (65%) | 148 | 72 | 68 | 13 |

The Rust binary's own VM/frame-prep symbols are not broken out here; the
table only needs its EndDrawing floor and total prep.

## Findings

1. **The swap floor is shared.** On blackhole, EndDrawing is 187 us for
   Janet and 186 us for Rust. This is the Metal-backed GL present path
   and WindowServer, so it is the same for both engines and independent
   of VM speed. At this window size it caps blackhole near 5.3k fps even
   with zero prep.

2. **The whole gap is prep.** Janet's non-swap time is 198 us vs Rust's
   85 us on blackhole (2.33x); 425 vs 219 us on scattered (1.94x).
   Matching Rust at 3693 fps on blackhole means a 271 us frame, i.e.
   **prep <= 84 us** -- the concrete 97% budget. That is roughly a
   2.3x cut of everything outside the swap.

3. **Prep composition.** Pure VM is 34% of the blackhole frame and 29%
   of scattered; draw-text is 15% / 31%; GC is 3-5% (inside pure VM).
   Inside the draw-text subtree, raylib's `GetGlyphIndex` /
   `DrawTextCodepoint` is about 57% and the generated alias wrapper's
   marshal about 43% (48-byte Font struct decode + Vector2/Color decode
   per cell). Both halves matter; neither alone reaches the budget.

4. **The native tier cannot see the hot loops.** Every
   `*-next-frame` / `*-build` function skips with
   `native-skip <id> <name> loop requires resumable VM quantum polls`.
   A C kernel cannot yield a VM quantum mid-loop and later resume its
   native stack and locals, so the tier keeps every loop boxed. Those
   loops are exactly the boxed tick math behind the 16-34% pure-VM
   share. This is the wall, not type inference (the `*-new` skips are
   secondary).

5. **The alias shim frame is about 1%.** Commit `0e2729fa` calls the
   installed C wrapper directly from `JOP_CALL` instead of entering the
   shim frame. Paired A/B: blackhole +0.8%, scattered +1.7%,
   bouncyballs +1.5%, laseretch +1.1% (two of three rounds; the middle
   round was a cold outlier). Real, safe, and not the lever.

## Scratch-tuple FFI arguments (landed 2026-10-20)

Finding 3's per-call tuple allocation is addressed by a generic
compiler/VM change, not a ttfx kernel. When a tuple literal is passed
directly to an FFI alias (`ffi/defbind-alias`), the compiler now emits
`JOP_MAKE_SCRATCH_TUPLE` into a persistent hidden frame slot instead of
allocating a fresh tuple. The alias reads the array synchronously and
never retains it. The rewrite is only allowed when every argument after
the tuple is scratch-safe (constant, symbol, or another direct tuple
literal), so a later call/array/table/struct cannot force a GC that
retains or invalidates the scratch slot. The slot is reserved above the
scope's high-water mark (`ra.max + 1`, then `ra.max` is raised), survives
loop iterations, and is remapped by bytecode slot compaction like any
other slot. The new opcode is appended after all existing opcodes, so
existing images keep their numbering.

Measured effect (same compiler, scratch enabled vs disabled): non-PGO
pair blackhole **+34.0%**, scattered **+28.9%**; canonical release pair
`rel2` vs `rel` blackhole **+32.9%**, scattered **+34.4%**. Full
37-effect paired release vs Rust: geomean **0.866** (from 0.681),
median 0.847, 37/37 within 2x, 29/37 >= 0.8; worst `synthgrid` 0.740,
best `print` 0.998. `gui-draw-cells` now has two scratch ops and zero
tuple allocations for its FFI alias arguments. The remaining draw-text
marshal cost is decoding the boxed Vector2/Color values per call (the
values vary, so struct caching does not apply) and the scene-cell tuples
built by the effects.

## Consequence for the 97% target

Reaching ~0.97x Rust on the laggards needs roughly a 2-2.3x cut in prep:
about 2x on pure VM and about 2x on the draw-text marshal, while leaving
the shared swap floor alone. The staged plan:

- **Native loops with back-edge deopt.** Compile loops whose iteration
  bodies are yield-free (all callees are kernels or known non-yielding C
  functions). Poll the quantum at the back edge; if a suspension is
  requested, rebox the live slots and resume at the loop header, which
  is a normal bytecode pc. The header is never re-entered with side
  effects replayed, so preemption semantics are preserved. This is the
  only sound route to kernelize the tick loops and the largest single
  win available. Gate on the preemption suites and byte parity.
- **Alias call-site fusion (partly landed).** Tuple allocation is gone
  (scratch tuples, above); the remaining half is decoding the boxed
  Vector2/Color fields in the wrapper without a separate marshal pass.
  Targets the 15-31% draw-text share.
- **PGO-guided unboxing** for the remaining boxed hot ops, as
  incremental VM wins behind the existing 2x hard gate.

Keep the rules: no ttfx-specific kernels or optimizations, ttfx sources
unchanged, byte parity on every change, no weakening of preemption, and
paired same-sitting GUI benchmarks only.

## Repro notes

```
# run the release GUI in the app-bundle wrapper, then sample the main thread
sample $(pgrep -f '<app>/Contents/MacOS/exec' | head -1) 3 1 > sample.txt
```

`native-skip` lines come from any `--aot --native` build
(`--emit-native-lib`); the 20 GUI aliases and their indices are listed in
the build log as `ffi-alias-native <id> <name> ...`.
