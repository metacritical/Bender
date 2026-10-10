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
(`examples/future.janet`).

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
