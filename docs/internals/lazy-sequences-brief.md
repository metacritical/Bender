# Lazy sequences in Bender — design brief and implementation prompt

Status: **SHIPPED.** The GC fix landed in `b169bdf0`, the lazy sequence API in
`08ef950a`, and the design record in `1a2a5b71`; `test/suite-lazy.ben` covers
all 37 behaviours below and the full suite is green. Every claim in this document
was verified on a live `build/bender` (arm64, macOS, `1.43.0-dev`) before it was
written; two probes crashed the process, and those crashes were the central
finding, not an aside.

What changed during implementation, versus the plan originally recorded here:

- **Only the closure-environment edge needed to be iterative.** The original plan
  called for a full worklist over every container. Measuring showed every other
  container marker recurses a fixed number of frames and is bounded by its own
  size, so the one unbounded edge was sufficient and far less invasive.
- **A VM-global worklist does not work.** Resetting it at collection entry let an
  inner collection (marking re-enters: `bender_env_maybe_detach` allocates) discard
  the outer pass's pending work and sweep live objects. It crashed `suite-lanes`
  in `bender_dict_find`. The shipped queue is thread-local and saved/restored
  around each drain.
- **`recur` remains unnecessary**, as predicted: tail calls already run in
  constant stack for consumers, and the collector no longer recurses for the
  structure.

---

## 1. The requirement

Lazy sequences must be **actually** lazy. A half-lazy `lazy-seq` is worse than no
`lazy-seq`, because Clojure programmers write `(take 5 (map f (range)))` against
an infinite source and expect no realization of element 6.

Three properties must all hold, in this order of importance:

1. **Laziness** — constructing `(map f (range))` realizes nothing; `n` consumed
   elements trigger `n` applications of `f`.
2. **Memoization** — a realized element is computed once. Re-traversal does not
   re-run side effects.
3. **Re-iterability** — the same sequence value can be walked more than once.

---

## 2. Why Bender's existing primitives do not satisfy this

### 2.1 A fiber is not a lazy seq

Fibers *are* pull-based: `(next fiber key)` advances and `(get fiber key)` reads.
`each` over a fiber works, and `take` correctly stops early. But a fiber is
**one-shot**:

```
first pass  => [1 2 3]
second pass => []        ; exhausted, yields nothing
```

So property 3 fails outright, and there is no memoization (property 2) either.
A fiber is a *cursor*, not a *sequence*. This is exactly the surprise the
requirement rules out.

Also confirmed: Bender's built-in `map` is **eager** over any source. It consumes a
fiber to exhaustion and returns an array. `map` over an infinite fiber never
returns — an earlier probe of mine hung for 60s and had to be killed. So the
existing `map` cannot be reused for the lazy variant.

### 2.2 An abstract type is not available from Bender source

The idiomatic C-backed answer would be a `BenderAbstractType` with `get`/`next`
slots, making a lazy seq a first-class iterable. Confirmed: `BenderAbstractType` is
a C struct
(`src/core/ev.c:6538`, `src/core/ffi.c:210`, `src/core/filewatch.c:830`) with no
Bender-level constructor (`abstract/new` does not exist; only `abstract?` is
exposed at `src/core/corelib.c:375`). Choosing this route means C work in
`corelib.c`/a new `lazyseq.c`, not a macro.

---

## 3. The design that works (verified)

A lazy seq is a **memoizing pull cell**: a closure that returns either `nil` (end)
or `(tuple value rest-cell)`, computing its body at most once.

```bender
(defn delay1 [f]
  (var v nil) (var done false)
  (fn [] (if done v (do (set done true) (set v (f)) v))))

(defn lcons [h t] (delay1 (fn [] (tuple h t))))
(defn lnil  [] (delay1 (fn [] nil)))
(defn lfirst [c] (def x (c)) (if x (get x 0)))
(defn lrest  [c] (def x (c)) (if x (get x 1)))

(defn lrange [a b]
  (delay1 (fn [] (if (>= a b) nil (tuple a (lrange (+ a 1) b))))))

(defn lmap [f c]
  (delay1 (fn []
    (def x (c))
    (if (not x) nil (tuple (f (get x 0)) (lmap f (get x 1)))))))

;; Short-circuit BEFORE realizing the cell. Clojure's `take` never pulls n+1.
(defn ltake [n c]
  (delay1 (fn []
    (if (<= n 0)
      nil
      (do
        (def x (c))
        (if (not x) nil (tuple (get x 0) (ltake (- n 1) (get x 1)))))))))

(defn ltoarray [c]
  (def a @[])
  (var x c)
  (while (def v (x)) (array/push a (get v 0)) (set x (get v 1)))
  a)
```

Measured results:

- `(lmap f (lnat 0))` over an **infinite** source: 0 evaluations on construction.
  Laziness holds.
- `(ltoarray (ltake 5 mapped))` → `0, 1, 4, 9, 16` with **exactly 5**
  evaluations — not 6. The `(<= n 0)` guard must come before `(c)`, or `take`
  realizes one element past its limit.
- Re-running `ltoarray` on the same sequence: evaluations stay at 5. Memoized.
- Iterative walk in `ltoarray` (a `while` over cells) is stack-safe by
  construction — the *consumer* does not recurse.

This is Clojure's shape and satisfies all three properties.

---

## 4. The blocker: the GC recurses and segfaults on deep chains

**This is a real defect in the runtime, independent of lazy seqs.**

```
lazy count of 180,000 elements  => SIGSEGV (exit 139)
plain array of 200,000 elements => fine
200,000 shallow (flat) closures => fine, GC survives
180,000-deep closure chain      => SIGSEGV
```

So the trigger is **depth, not count**. `lldb` backtrace:

```
stop reason: EXC_BAD_ACCESS (code=2, address=0x1067fffd0)   <- stack guard page
frame #0: bender_mark(...) at gc.c:165
```

`bender_mark` → `bender_mark_function` → `bender_mark_funcenv` → ... each link of the
chain is one more C stack frame. There is a recursion guard
(`BENDER_RECURSION_GUARD 1024`, `src/include/bender.h:296`, consulted at
`src/core/gc.c:166` via the `depth` counter), but it only diverts marking to
`bender_gcroot` — appending to `bender_vm.roots` — which **does not mark
transitively in that pass**. Depth is reset at `gc.c:2668`, `2956`, `3031`, `3083`.
The effect is that a chain deeper than the guard can leave reachable objects in an
inconsistent state, and the C stack itself overflows before any guard can help.

Consequences:

- A 150k-element lazy seq walks fine; 180k crashes. A lazy seq over a large file,
  a long list of rows, or a network stream is exactly the shape that reaches this.
- Any deep closure chain does it, not just lazy seqs: recursive builders,
  generated code with nested closures, deep tuple-of-thunk structures.

### 4.1 Required fix

Make GC marking **iterative** rather than recursive for the container edges that
can nest without bound — `funcenv`, `function`, `tuple`, `array`, `table`,
`struct`, `fiber`.

Three facts from the source that shape the fix:

- `bender_mark` (`gc.c:165`) decrements a `depth` counter and, at zero, defers the
  value to `bender_gcroot` — which only **appends** to `bender_vm.roots`. The drain
  loop at `gc.c:2435` then pops and re-marks those appended roots. So the existing
  machinery is already a gray list; it is simply entered only after 1024 nested C
  frames, by which point the stack is the problem.
- `bender_mark_many` (`gc.c:291`) is an iterative loop that calls the recursive
  `bender_mark` per element. It is the natural place to push onto a worklist
  instead.
- The iteration is one frame per edge: `bender_mark` → `bender_mark_function` →
  `bender_mark_funcenv` → `bender_mark_many` → `bender_mark`. A chain of length *n*
  costs roughly *4n* C frames. At ~16 KB per frame the default 8 MB stack dies
  near 150k–180k links — exactly the observed threshold.

The clean fix is to make the whole mark phase drain one explicit worklist, so the
C stack depth is constant regardless of object-graph depth:

```c
static void bender_mark_root(Bender seed) {
    bender_v_push(gray, seed);            /* raw realloc: a push must never
                                          * trigger the collection it protects */
    while (bender_v_count(gray)) {
        Bender x = bender_v_pop(gray);
        switch (bender_type(x)) {
            case BENDER_TUPLE:   /* mark header, push elements */ break;
            case BENDER_FUNCTION:/* mark def + push each env  */ break;
            ...
        }
    }
}
```

Requirements for the fix:

- The worklist must grow with **raw realloc** (`bender_realloc`), following the
  precedent in `bender_gcroot` (`gc.c:3143`) and `bender_gcshadow_push`
  (`gc.c:3178`), because a push must never be able to trigger the collection it is
  protecting against.
- `bender_mark` keeps its signature and semantics for C callers; it becomes the
  entry point that seeds and drains the worklist.
- The worklist must be reset per collection at the same four sites that reset
  `depth` today (`gc.c:2668`, `2956`, `3031`, `3083`), and freed or cleared so a
  long-lived VM does not accumulate.
- Do **not** simply raise `BENDER_RECURSION_GUARD`. It converts a segfault into a
  different silent inconsistency around the 1024-frame boundary, and no value of
  the constant makes unbounded depth safe.
- Keep `bender_gcroot` and the `gc.c:2435` drain loop until the iterative path is
  proven; then decide whether the deferred-root path is still needed.
- Add a regression test that builds a **1,000,000-deep** closure chain, calls
  `gccollect`, and asserts the process survives and the chain still walks.

  Verified today this crashes:
  ```
  (defn chain [n] (var c (fn [] nil))
    (forv i 0 n (let [prev c] (set c (fn [] (prev))))) c)
  (def deep (chain 1000000))
  (gccollect)          ; => SIGSEGV exit 139
  ```

**This fix is a prerequisite.** Ship it before `lazy-seq`, not after: a lazy seq
that crashes at 180k elements is not "actually lazy", it is a time bomb, and the
first person to point it at a large file will blame the sequence library.

---

## 5. Remaining design decisions to settle

### 5.1 Bender source vs C abstract type

| | Bender source (memoizing closure) | C `BenderAbstractType` |
|---|---|---|
| Effort | ~150 lines in `boot.ben` | new C type + `get`/`next`/`marshal`/`tostring` |
| `each` over it | needs an adapter (`ltoarray`, or a fiber bridge) | native |
| `length` | no | yes (must force, which is a trap) |
| Marshal / transmit | closure = **not marshallable** | marshallable with a custom marshal fn |
| Fibers/threads | fine within one VM | fine |

**Recommendation: start in Bender source.** It is the smaller, reversible step and
matches the stated preference for macro-level work. The marshal gap matters only
if a lazy seq must cross a thread boundary — and per the project's share-nothing
decision it must not; pass an array across, not a sequence.

Add an `each`-compatible bridge so ordinary Bender iteration works:

```bender
(defn ltofiber [c]
  (fiber/new (fn [] (var x c)
               (while (def v (x))
                 (yield (get v 0))
                 (set x (get v 1))))))
```

Then `(each v (ltofiber seq) ...)` reads naturally. Document that the fiber is a
one-shot **view**, distinct from the reusable sequence value.

### 5.2 Naming and shadowing

`map`, `filter`, `take`, `drop`, `range` are already taken with eager semantics and
are used pervasively in `boot.ben` itself. **Do not shadow them.** Options:

- `lazy-` prefix: `lazy-map`, `lazy-filter`, `lazy-take`, `lazy-drop`,
  `lazy-range`, `lazy-concat`, `lazy-seq`, `lazy-cat`, `realize`.
- This matches the existing `examples/lazyseqs.ben` convention
  (`lazy-map`, `lazy-take`, `lazy-drop`, `realize`), so promote that file's
  vocabulary rather than inventing a second one.

### 5.3 Relation to `examples/lazyseqs.ben`

That example already contains `dolazy`, `cons`, `lazy-range`, `lazy-map`,
`lazy-take`, `lazy-drop`, `randseq`, `realize`. It has the right shape but note
`lazy-map` there recurses through `map` (eager, wrong) and `lazy-drop` has a
broken `((lazy-drop ...))` double-call. **Rewrite rather than copy**, using §3's
verified implementation, and delete the example once core carries the API so
there is one definition.

### 5.4 `force` / `delay` / `promise`

- `delay` exists (`boot.ben:4705`); confirm its `force` counterpart returns the
  memoized value and is safe to call repeatedly.
- `promise`/`deliver` are one-shot write-once cells that block on deref. Given the
  Erlang-style decision, implement over fibers and state clearly whether `deref`
  blocks the fiber or returns one. Do not present them as thread-safe.

---

## 6. Implementation order

1. **Fix iterative GC marking** (§4.1) + the 1M-deep regression test. Blocks
   everything else.
2. **Core lazy sequence**, Bender source: `lazy-seq`, `lazy-cons`, `lazy-range`,
   `lazy-map`, `lazy-filter`, `lazy-take`, `lazy-drop`, `lazy-take-while`,
   `lazy-drop-while`, `lazy-concat`/`lazy-cat`, `realize`, `realize-into`.
   Reuse the §3 shapes; keep the `take` short-circuit and the iterative walk.
3. **`ltofiber` bridge** so `each` works, with the one-shot-view caveat documented.
4. **Docstrings** in house style; note the marshal limitation explicitly.
5. **Tests** (`test/suite-lazy.ben`): laziness (0 evals on construct, n on
   consume), memoization (impure `f` runs once), exact `take` count, infinite
   source does not hang, 1M-element walk does not crash, empty and
   immediately-false-predicate cases.
6. **Delete or rewrite `examples/lazyseqs.ben`** so there is one vocabulary.
7. **Update `docs/internals/clojure-surface.md`** — move laziness out of
   "deliberate omissions" and record the marshal and one-shot-fiber caveats.

---

## 7. Acceptance criteria

- `(realize (lazy-take 5 (lazy-map f (lazy-nat))))` returns 5 values with exactly
  5 invocations of `f`, for an `f` that increments a counter.
- The same sequence value traversed twice invokes `f` once per element total.
- Constructing a lazy seq over an infinite source neither hangs nor evaluates.
- `gccollect` after building a 1,000,000-deep chain returns normally (this is the
  §4 regression).
- `make test` green; `make` from clean produces `build/bender` without warnings.
- No eager builtin (`map`, `filter`, `take`) changes behavior — check by running
  the existing suite.
