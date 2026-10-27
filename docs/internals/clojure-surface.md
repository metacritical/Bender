# Clojure-ward surface: status of each convergence item

Already present (verified live, no work needed): threading macros
(`->`, `->>`, `as->`), true tail calls (`JOP_TAILCALL` — 10M-deep
recursion returns exactly, constant stack; no `recur` needed),
closures with independent captures, positional/`&rest`/direct-key
destructuring in every binding form.

Shipped here: `puts` (Ruby line-per-arg printing, core); `:keys` /
`:or` struct destructuring in the compiler (all binding forms,
nested, literal defaults, nil-triggers-default); multimethods
(`defmulti`/`defmethod`/`methods`/`get-method`/`remove-method`,
exact dispatch + `:default`, single-arity, no hierarchies);
same-VM atoms (`atom`/`deref`/`swap!`/`reset!`/`compare-and-set!`
+ validators, no locking); futures/promises over the pool
(`examples/future.janet`); lazy sequences (`lazy-cell`,
`lazy-range`, `lazy-repeat`, `lazy-iterate`, `lazy-map`,
`lazy-filter`, `lazy-take`, `lazy-drop`, `lazy-take-while`,
`lazy-drop-while`, `lazy-cat`, `lazy-mapcat`, `lazy-from-array`,
`lazy-cons`, `lazy-first`, `lazy-rest`, `lazy-empty?`,
`lazy-to-array`/`realize`, `lazy-each`, `lazy-reduce`,
`lazy-to-fiber`, `delay1`).

Deliberate omissions with cause:

- No `:as` whole-binding: `{:as sym}` keeps its legacy meaning
  (bind `(get m :as)`), which `import*`'s calling convention depends
  on (`(def {:as as :prefix prefix ...} kargs)`). Redefining it broke
  every import (caught by suite-array); pinned in
  test/suite-destructure.janet.
- No cross-thread atoms (would need mutex + copy discipline —
  channels carry state across, futures/promises carry results).
- No agents/refs/STM yet (designed, unbuilt): single-VM STM (refs as
  versioned boxes, `dosync` validate-and-commit with retry) composes
  over ev tasks sharing one heap; cross-thread STM conflicts with
  share-nothing heaps and stays out.
- Multimethod macros emit single top-level `def`s (a `do`-wrapped
  `def` scopes invisibly — `do` opens a scope) and reference only
  public core names (private `-` bindings don't resolve at use
  sites).
- Test pins compare by content across marshal transports: identity
  of a pre-collect literal can't be pinned (marshal copy + cache
  residency; a well-timed major mints duplicates legitimately).

## Lazy sequences: why this design

A lazy sequence is a **memoizing pull cell** — a no-argument function that
returns either `nil` (end) or `[value rest-cell]`, computing its body at most
once. That shape was chosen after measuring the two obvious alternatives and
finding both unfit.

**A fiber was measured and rejected.** Fibers *are* pull-based: `(next fiber k)`
advances, `each` over one works, and `take` stops early. But a fiber is
**one-shot**. Traversing the same fiber twice gives the full sequence and then
nothing:

```
first pass  => [1 2 3]
second pass => []
```

It also does not memoize. A fiber is therefore a *cursor*, not a *sequence*, and
Clojure programmers re-traverse sequences routinely. Shipping a fiber as
`lazy-seq` would have produced exactly the surprise we set out to avoid.
`lazy-to-fiber` exposes a fiber *view* deliberately, documented as one-shot,
with the sequence value staying re-traversable so a fresh view per pass is the
idiom.

**Janet's `map` was rejected as a building block.** It is eager over every
source — it consumes a fiber to exhaustion and returns an array — so
`(map f (lazy-range))` realizes everything, and over an infinite source it
simply hangs. A probe against an infinite generator had to be killed.

**An abstract type was the third option and was declined for now.** A
`JanetAbstractType` with `get`/`next` slots would make a sequence a first-class
iterable with native `each`/`length`/marshal support, but abstract types are
defined in C only — there is no Janet-level constructor. That is a larger and
less reversible change than a boot.janet implementation, so it stays available
if a sequence ever needs to cross a process boundary. Today it must not:
closures do not marshal, so pass an array across, not a sequence.

**Two correctness details worth keeping in mind when extending this.**

1. `lazy-take` tests the remaining count *before* realizing the cell. Testing
   after realization computes element *n+1*, which is invisible until you take 5
   from an infinite sequence and watch a counter reach 6.
2. `deep=` distinguishes a tuple from an array, so tests comparing against
   `lazy-to-array` must build array literals (`@[...]`), not tuples.

## Why marking is iterative

Implementing lazy sequences exposed a collector defect that has nothing to do
with lazy sequences: a lazily-built chain is a deep chain of closures, and
marking recursed one C frame per edge
(`janet_mark` -> `mark_function` -> `mark_funcenv` -> `mark_many` ->
`janet_mark`). A deep chain exhausted the C stack inside a collection and
crashed on the guard page — lldb showed `EXC_BAD_ACCESS` in `janet_mark`. Onset
was ~150k–180k links; the same graph built shallowly, and a plain 200k tuple
chain, were both fine, so the trigger is **depth, not count**.

`JANET_RECURSION_GUARD` did not help: at 1024 nested marks it deferred values to
`janet_gcroot`, which only *appends* to the root list for the drain loop at
`gc.c:2435`. That is 1024 C frames too late, and it is near the point where an
8 MB stack is already gone.

The fix marks **closure environments** iteratively through a
mark-operation-scoped queue (`janet_drain_env_queue`). Only the environment edge
needs this: every other container marker recurses through a fixed number of
frames, and its children are bounded by that container's own size, whereas an
environment chain is unbounded by construction.

Three constraints shaped it, each learned the hard way:

- **The queue is saved and restored around every drain.** Marking re-enters:
  `janet_env_maybe_detach` allocates, which can itself collect. An earlier
  attempt kept one worklist in the VM and reset it at collection entry; an inner
  collection then discarded the outer pass's pending work and swept live
  objects. That corrupted reachability and crashed `suite-lanes` inside
  `janet_dict_find`.
- **Growth uses raw `realloc`**, following `janet_gcroot` and
  `janet_gcshadow_push`; a push must never be able to trigger the collection it
  protects.
- **The drain is owned by the outermost caller**; nested calls only enqueue, so
  C stack depth stays constant however long the chain is.

Consequence for the language surface: a lazy sequence of any length is safe to
build and walk, and `recur` remains unnecessary — the existing tail calls
already run in constant stack for the *consumer*, while the collector no longer
recurses for the *structure*.
