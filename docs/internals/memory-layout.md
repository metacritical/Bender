# Memory layout systems — tutorial with examples

Your mental model is exactly right, and this note extends it rather
than replacing it: **what the compiler can't prove stays in the VM;
hints move more into static kernels; higher-level values stay boxed.**
Memory-layout systems are about shrinking what "boxed" costs, and —
only where proven worth it — letting selected composites live
unboxed. Each level below shows the same ttfx scene example, what the
machine does today, what changes, and what it buys.

## 0. What "memory layout" means

A value's layout is where its bytes live and how the code finds them.
Every lookup step costs nanoseconds; frames do millions of them.

Today, a scene is a boxed table:

```janet
(def sc {:head 0 :queue [0 1 2] :estep 0})
(sc :head)  # => 0
```

Machine work for `(sc :head)`: check the tag (is it a table?) →
hash `:head` → probe the bucket array → follow the pointer to the
slot → check *its* tag → unbox the int. Roughly: 2 tag checks + 1
hash + 2 pointer chases ≈ tens of nanoseconds, every access, every
frame, every cell. That is the tax levels below remove, piece by
piece. Our profile says this tax class (interpreter dispatch) is 96%
of frame time — so the layout story *is* the performance story.

## Level 1 — scalar unboxing in kernels (SHIPPED)

```janet
(defn rng-next {:hint {:params [:long] :returns :long}} [s]
  (band (+ (* s 6364136223846793005) 1442695040888963407) 0xFFFFFFFF))
```

The compiler proves `:long` in/out and emits a C function with raw
`int64_t` locals. No tags, no dispatch. This works today for anything
numeric — RNG stream, easing, gradient lerp. Nothing about tables or
strings changes: they never enter a kernel.

## Level 2 — SoA arrays (SHIPPED, Step 11)

Instead of an array of tables (array-of-structs), parallel arrays of
numbers (struct-of-arrays). Our GUI frame cells already fit:

```janet
# boxed today: [{:r 1 :c 1 :cp 9608 :pack 4278190335} ...]
# SoA shape:  rows[] cols[] cps[] packs[]  -- four :array params
(defn draw-cells {:hint {:params [:array :array :array :array :long]
                         :returns :long}}
  [rows cols cps packs n]
  ...)
```

Kernels get `:array` params (opaque pointers, never arithmetized) with
`len`/`get`/`put` opcodes. The win: sequential numbers in cache-flat
buffers instead of pointer-chased table slots. Usable today wherever a
hot loop can be reshaped to columns.

## Level 3 — unboxed composite shapes (FUTURE, on demand)

The missing piece: a *declared* composite the compiler lays out like
C. Sketch of the surface (not implemented):

```janet
(defcell scene {:shape {:head :long :estep :long :layer :long}})
(def sc (scene 0 0 1))        # 24 bytes, contiguous, no tags
(sc :head)                    # compiles to *(base + 0) -- one load
```

What would change in our system, concretely:

- `tools/callgraph.janet`: a shape registry (name → field offsets,
  from the existing width system); field access on a shaped value
  proves to a constant offset instead of a hash probe.
- Emission: shaped locals become C struct locals; shaped arrays
  become flat buffers (this is Level 2 done structurally).
- `src/core/gc.c`: unboxed fields need no marking (numbers only at
  first; shaped values containing heap pointers need exact tracing —
  the hard part, which is why this waits for proof of need).
- Precedent that it works: `ffi/struct` already computes exact C
  layouts (our `Font` round-tripped 48 bytes byte-identically) and
  `ffi-struct` descriptors now marshal. A `{:shape}` is that machinery
  pointed inward.

When it pays: per-cell scene stepping, where each frame currently
pays hash+tags per field per cell. Estimated order: 5–10× on that
loop *if* the loop is dispatch-bound after scalar kernels land.

## Level 4 — escape analysis / frame-local allocation (FUTURE)

Half our frame time is *building strings* (75 ms / 200 frames). Those
strings never escape the frame: build, print, drop. Escape analysis
proves "this table/buffer dies in this function" and puts it in C
locals or a reused frame buffer — zero GC pressure, zero malloc:

```janet
# today:  fresh strings every frame (GC sees all of it)
# with EA: one reused output buffer, rewritten per frame
```

Cheaper cousin that needs no compiler work: hand-reuse a persistent
buffer in `ctx-frame-string` (plain Janet shaping). Do that first;
build EA only if allocation still dominates after.

## Level 5 — inline caches (FUTURE, boxed tier)

Where types *can't* be proven, cache the lookup shape at the call
site (polymorphic inline cache): first call hashes, subsequent calls
hit the cached offset. No unboxing, 2–5× cheaper dispatch in
monomorphic loops. VM work, benefits all boxed code at once.

## Do the benchmarks justify building any of this?

Current data (2026-10-07, M2 Pro) — **no, not yet:**

- Windowed GUI, uncapped: **1100–1900 fps** — frames are not blocked.
- Engine: 0.80 ms/frame steady-state; profile 96% dispatch / 3% GC.
- Split is 50/50 stepping vs string-building.

In that picture the cheap wins come first, in order: sparse rendering
(same bytes, fewer cells — plain shaping), scalar kernels for the
numeric leaves (shipped machinery), buffer reuse for frame strings
(shaping). Explicit go/no-go: build Level 3+ only if, after those, a
bench row still shows dispatch-or-GC dominating **and** the hot code
can't be reshaped onto arrays. Success is the bench table matching
Rust, not the systems themselves — if we're near Rust speeds without
them, we're done and that's a win, not a gap.
