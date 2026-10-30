# Clojure surface for Bender — gap specification

A self-contained brief for implementing the missing Clojure-compatible surface on
Bender. Everything in "Verified present" and "Missing" below was checked against a
live `build/bender` on 2026-10-27, not inferred from source reading.

---

## 0. Goal and non-goals

**Goal.** Make Clojure-shaped code readable and runnable on Bender without a
translator: a macro/function layer (`src/boot/boot.ben`) plus small runtime
helpers, so that ordinary Clojure idioms — data-first map/vector utilities,
threading, lazily-realized flows, error handling — behave as a Clojure programmer
expects.

**Non-goals**, restated from the user's decisions:

- **No JBVM classes for `deftype`.** Bender `struct` (immutable) and `table`
  (`@{}`, mutable) already model records. `deftype` is not required.
- **No persistent/immutable-by-default data structures.** No structural-sharing
  vectors or HAMTs. Concurrency is Erlang-style: fibers, callbacks,
  promises/`futures`, share-nothing heaps. This removes the entire reason
  Clojure's collections are persistent.
- **Metadata need not be first-class.** It is wanted as a compile-time input only
  — for unboxing hints and PGO — not as a value-level map on every object.
  (`defn` already threads `^type` hints into `{:hint ...}`; that path is
  sufficient. See §4.5.)
- **No `recur`.** `JOP_TAILCALL` gives true tail calls (verified: 10M-deep
  recursion returns in constant stack). `loop` + Bender `while`/`var` covers the
  iteration case. Adding `recur` would fight the compiler for no gain.

---

## 1. Verified present — do not reimplement

Confirmed by evaluating in `build/bender`. Reimplementing any of these is a
regression.

**Binding and definition**
`defn`, `defn-`, `defmacro`, `defmacro-`, `def-`, `var-`, `defdyn`, `defglobal`,
`varglobal`, `default`, `with-syms`, `toggle`.

**Control flow**
`if`, `when`, `unless`, `if-not`, `cond`, `case`, `and`, `or`, `do`, `let`,
`if-let`, `when-let`, `when-with`, `if-with`, `loop`, `for`, `forv`, `each`,
`eachp`, `eachk`, `while`, `repeat`, `forever`.

**Error handling**
`try` with the Bender catch-tuple form `(try body ([e] ...))`, `error` (throw),
`protect`, `defer`, `edefer`, `with`, `assert`, `assertf`, `errorf`, `label`,
`prompt`, `return`.

**Threading**
`->`, `->>`, `as->`, `-?>`, `-?>>`, `as?->`.

**Sequence and collection basics**
`map`, `filter`, `reduce`, `reduce2`, `accumulate`, `accumulate2`, `keep`,
`mapcat`, `find`, `find-index`, `index-of`, `sort`, `sort-by`, `sorted`,
`sorted-by`, `count`, `take`, `drop`, `take-while`, `drop-while`,
`take-until`, `drop-until`, `partition`, `partition-by`, `interleave`,
`interpose`, `distinct`, `flatten`, `frequencies`, `group-by`, `reverse`,
`range`, `repeat`, `generate`, `coro`, `fiber-fn`, `sum`, `mean`, `geomean`,
`product`, `extreme`, `max-of`, `min-of`.

**Dictionary basics**
`get` with a default (`(get m k d)`), `put`, `merge`, `update`, `get-in`,
`update-in`, `has-key?`, `has-value?`, `keys`, `pairs`.

**Function combinators**
`comp`, `identity`, `complement`, `juxt`, `partial`¹.

**Multimethods**
`defmulti`, `defmethod`, `methods`, `get-method`, `remove-method` —
exact dispatch value plus a `:default` fallback, single arity, no hierarchy.

**Concurrency (same VM)**
`atom`, `deref`, `swap!`, `reset!`, `compare-and-set!`, `delay`².

**Destructuring**
Positional, `&rest`, direct-key (`{:a a}`), `:keys`, `:or` with literal
defaults and nil-triggers-default, nested, in every binding form.
**`:as` is deliberately unavailable** — it holds its legacy Bender meaning and
`import*`'s calling convention depends on that. Do not "fix" it.

**Misc**
`match`, `varfn`, `short-fn`, `tracev`, `doc`, `import`, `use`, `comptime`,
`compif`, `compwhen`, `comment`, `as-macro`, `chr`, `keep-syntax`.

**Structural equality and records**
`struct` (immutable map), `table`/`@{}` (mutable map), `deep=` (structural
comparison), `tuple`/`array` for sequences.

¹ `partial` resolved present in the final probe but was absent from an earlier
one; verify it evaluates before assuming, and add it if the probe was stale.
² `delay` exists at `boot.ben:4705`; confirm `force` semantics against it.

---

## 2. Missing — the actual work

Grouped by cost and risk. Every name below returned "missing" from `dyn`.

### 2.1 Collections and dictionaries — mechanical, high value

Plain functions over Bender `table`/`struct`/`array`/`tuple`. No compiler
involvement, no new types.

| Name | Clojure contract | Bender shape |
|---|---|---|
| `assoc` | `(assoc m k v & kvs)` → new map | copy table, `put` each pair; return `struct` when input is a `struct` |
| `dissoc` | `(dissoc m & ks)` → map minus keys | copy, `put k nil` or build fresh excluding ks |
| `conj` | vector→append, list→prepend, map→entry | dispatch on input type: `array`/`tuple` push, `table`/`struct` put |
| `into` | `(into to from)` | fold `from` into `to` with `conj`, honoring `(into to xform from)` |
| `vals` | values of a dict | `(map (fn [[_ v]] v) (pairs d))` |
| `select-keys` | submap | filter `pairs` by key set |
| `merge-with` | merge with `(f old new)` on collision | fold `pairs`, apply f when key present |
| `merge-with-nil` | like `merge-with` but nil does not clobber | same, nil-guard |
| `assoc-in` | `(assoc-in m [k1 k2] v)` | recursive `update-in` with a constant |
| `dissoc-in` | remove at path | recursive |
| `update-keys` | `(update-keys m f)` | rebuild with `(f k)` |
| `update-vals` | `(update-vals m f)` | rebuild with `(f v)` |
| `reduce-kv` | `(reduce-kv f init m)` over k,v | iterate `pairs` |
| `zipmap` | `(zipmap ks vs)` → map | walk two seqs |
| `set` | build a set from a seq | Bender has no set type; model as `@{k true}` — **document the representation** |
| `vec` | coerce to vector | `(array ;xs)` |
| `sorted-set` | ordered unique | not meaningful without a set type; **defer or omit with a note** |
| `subvec` | slice | `(tuple/slice v start end)` |
| `rseq` | reverse | `(reverse xs)` already exists; `rseq` is an alias |

### 2.2 Sequence operations — mechanical

`butlast`, `nrest`, `nthrest`, `nthnext`, `drop-last`, `take-last`,
`last-index-of`, `dedupe`, `partition-all`, `take-nth`, `mapv` (`(vec (map f xs))`),
`filterv`, `map-indexed`, `keep-indexed`, `replace`, `shuffle`, `rand-nth`,
`max-key`, `min-key`, `distinct?`.

Notes:
- `mapv`/`filterv` need real vectors. Since the project rejected persistent
  vectors, they return Bender `array` (`@[]`). **This must be stated in the doc**,
  because Clojure's `mapv` returns a vector and code indexes it.
- `distinct?` is a predicate over varargs, not a seq op.
- `keep-indexed`/`map-indexed` should take `(fn [i x])`.

### 2.3 Function combinators — trivial

`constantly`, `fnil`, `memoize`, `trampoline`, `some-fn`, `every-pred`,
`not-any?`, `not-every?`.

- `memoize` must key on the argument list; Bender tables accept tuples as keys, so
  a plain `@{}` works. Note it is not thread-safe across VMs (consistent with the
  same-VM atoms decision).
- `trampoline` is nearly free because tail calls already do not grow the stack;
  implement it anyway for source compatibility.

### 2.4 Macros — low risk

`some->`, `some->>`, `doto`, `condp`, `if-some`, `when-some`, `when-first`,
`letfn`, `binding`, `with-redefs`, `with-redefs-fn`, `doseq`, `dotimes`.

- `doto`/`some->`/`some->>` are pure rewrites in the style of the existing `->`.
- `binding` maps onto Bender's `with-dyns` / `setdyn`; Clojure's rebinding of vars
  has no direct analogue, so **document what it actually does**.
- `with-redefs` needs Bender's `*redef*` dynamic (present at `boot.ben:1568`).
  Confirm behavior before promising Clojure parity.
- `letfn` expands to mutually-recursive `defn`s; Bender `def` opens scope, so this
  may need a `do`-free expansion — mind the note at the bottom of
  `docs/internals/clojure-surface.md` about `do`-wrapped `def` scoping invisibly.
- `doseq`/`dotimes` can delegate to the existing `loop`/`each`.

### 2.5 Laziness — design needed

`lazy-seq`, `lazy-cat`, `realize`, `force`, `promise`, `deliver`, `realized?`.

`examples/lazyseqs.ben` already implements lazy linked-list sequences with
memoized realization. **Decide**: promote that example into core, or keep it an
example and document that Clojure laziness is opt-in. Do not silently half-implement
`lazy-seq` — a Clojure programmer will assume `(take 5 (map f (range)))` never
realizes the tail.

`promise`/`deliver` are distinct from `delay`: they are one-shot write-once cells
that block on `deref`. Given the Erlang-style decision, implement them over
Bender fibers and say clearly whether `deref` blocks or returns a fiber.

### 2.6 Protocols and records — decide, then document

`defprotocol`, `deftype`, `defrecord`, `defstruct`, `extend-type`,
`extend-protocol`, `reify`, `definterface`, `proxy`.

Per the user's decision: **no JBVM classes.** The realistic mapping is:

- `defprotocol` → a table of multimethods keyed by method name, dispatching on
  `(type self)` or a `:type` field. Cheaper: emit one `defmulti` per method with a
  dispatch fn over the first argument.
- `defrecord`/`deftype` → `struct` with a `:type`/`:record` tag field, plus
  generated constructor, predicate (`Foo?`), and field accessors.
- `extend-type`/`extend-protocol` → `defmethod` installs on the tag.
- `reify` → a `table` with the method entries closed over.
- `definterface`/`proxy` → **omit**, and say so.

Important: Bender's `defmulti` supports **exact dispatch values only, no
hierarchy** (`isa?`, `derive`, `prefer-method` are all absent). Protocols that
rely on Java-style interface inheritance cannot be emulated faithfully. Scope this
as "single-level tagging", not Clojure protocols.

### 2.7 Testing and namespaces — optional

`deftest`, `is`, `are`, `testing`, `run-tests`, `use-fixtures`;
`ns`, `in-ns`, `refer`, `alias`.

Bender has its own test conventions and `import`/`use` already shadow some of these
names. **Recommendation: skip `ns`/`in-ns`/`refer`/`alias` entirely** — they
conflict with Bender's module system and buy nothing. Testing macros are worth it
only if the intent is to run real Clojure test suites.

---

## 3. Semantic traps — reconcile or document, do not ignore

These are the places where existing Bender behavior differs from Clojure and a
naive port will produce silent wrong answers. Each needs an explicit decision
recorded in `docs/internals/clojure-surface.md`.

### 3.1 `(:kw m)` returns nil — the highest-severity trap

Verified:

```
(:shape {:shape :square})   =>  nil        ; Clojure: :square
(get {:shape :square} :shape) => :square   ; works
({:shape :square} :shape)     => :square   ; works
```

In Bender a keyword call is a **method invocation on the object**, not a map
lookup. `(:kw m)` is the single most common access idiom in Clojure source.
Consequences: any ported Clojure code that uses keyword access silently reads
`nil`.

Options:
1. **Leave it and document loudly.** Cannot be fixed without changing keyword
   semantics globally, which would break Bender's method dispatch.
2. Provide a reader/wrapper shim (`kw-get`) and a lint rule flagging `(:kw x)`.
3. Compiler-level: make a keyword call fall back to a map lookup when the target
   is a table/struct and no method is found. **This is the tempting one and it is
   dangerous** — it makes every typo'd method call silently return a map field.

**Recommendation: option 1 plus a mechanical check.** Do not change the compiler.

### 3.2 `=` is identity on mutable containers

Verified:

```
(= {:a 1} {:a 1})     => true     ; structs: structural
(= @{:a 1} @{:a 1})   => false    ; mutable tables: identity
(= @[1 2] @[1 2])     => false    ; arrays: identity
(deep= @{:a 1} @{:a 1}) => true   ; structural
```

Clojure's `=` is structural everywhere. A ported test that compares two built
maps will fail on Bender. Decision needed: either document "use `deep=` for
mutable", or provide a `clj=` that always recurses. **Do not shadow `=`.**
(`differs-from-clojure` note belongs next to every collection helper.)

### 3.3 Multimethods lack hierarchy

`defmulti`/`defmethod` exist (good) but support exact dispatch values plus
`:default` only. No `derive`, `isa?`, `prefer-method`, `remove-all-methods`, no
multi-arity methods. Protocol emulation inherits this limit. State it.

### 3.4 Structs are immutable

`(put (struct :a 1) :b 2)` errors. Clojure's `assoc` returns a new map, so this
is compatible in spirit — but `put` is the Bender idiom and it fails on structs.
Every `assoc`-family helper must branch on `struct?` vs `table?`.

### 3.5 `^{}` metadata on `def` is rejected

```
(def ^{:doc "hello"} v 1)
=> compile error: cannot add metadata v to binding ^
```

Metadata only survives via `defn`'s hint path (`{:hint {...}}` on the binding).
Per the user's decision this is acceptable — metadata is wanted for unboxing/PGO,
not as a value-level map — but "add metadata to a var" has no equivalent and
should be documented as unsupported rather than half-built.

### 3.6 No `recur`

Confirmed absent (`unknown symbol recur`) and unnecessary: tail calls already run
in constant stack. `loop` + `var`/`while` covers iteration. Document the idiom
translation: `(recur ...)` → `(set local ...)` inside `loop`/`while`.

### 3.7 `get-in`/`update-in` mutate a table in place

Verified: `update-in` accepts an `array`, `table`, or `buffer` and returns the same
kind — it does not produce an immutable result. Given a `struct` it throws
(`expected array, table or buffer, got <struct>`). Clojure's `update-in` returns a
new value and accepts any map.

Consequence: `assoc-in`/`dissoc-in`/`update` in the §2.1 table must **not** be thin
aliases over `update-in`. Each must branch on `struct?`/`table?` and copy for the
immutable case, or the helper silently mutates its input.

---

## 4. Design constraints

### 4.1 Where the code goes

- Macros and pure functions: `src/boot/boot.ben`, in the existing style.
- Anything needing C (a real set type, blocking `promise`): `src/core/corelib.c`,
  but **prefer boot.ben** — the user's framing is "mostly macro work".
- Documentation: extend `docs/internals/clojure-surface.md`; it already has
  "Already present" and "Deliberate omissions with cause" sections. Add a
  "Semantic differences from Clojure" section per §3.

### 4.2 Naming discipline

Some Clojure names are already taken by Bender with different meaning
(`merge` exists; verify its arg order and nil handling before aliasing).
**Audit each name for a collision before defining it.** A duplicate top-level
`def` in `boot.ben` is a silent shadow at best and a runtime error at worst.

### 4.3 `def` scoping gotcha

From the existing surface doc: a `do`-wrapped `def` opens a scope and the binding
is invisible. Any macro that must produce a top-level binding (multimethods
already note this) must emit the `def` at top level, not inside a `do`.

### 4.4 Do not touch the compiler

The user's explicit constraint: "without messing up the compiler". Everything in
§2 is expressible as macros/functions. The only items that would want compiler
support are `recur` (§3.6, declined) and keyword-as-map-access (§3.1, declined).
If a feature seems to require a compiler change, stop and write the trade-off down
instead of changing `compile.c`.

### 4.5 Metadata direction

Keep the existing `defn` hint collection (`:hint` with `:params`/`:returns`).
If PGO/unboxing wants more, extend the hint vocabulary — not the value-level
metadata model.

---

## 5. Suggested implementation order

1. **§2.3 combinators** — no dependencies, immediately useful, zero risk.
2. **§2.1 dictionary helpers** — the largest real gap; `assoc`/`dissoc`/`conj`/
   `into`/`vals`/`select-keys`/`merge-with`/`reduce-kv`/`zipmap` cover most ports.
3. **§2.2 sequence ops** — mechanical, builds on 2.1's `vec`/`conj`.
4. **§2.4 macros** — `doto`, `some->`, `condp`, `doseq`, `dotimes` first;
   `binding`/`with-redefs`/`letfn` after their semantics are settled.
5. **§2.5 laziness** — decide promote-vs-example before writing code.
6. **§2.6 protocols** — scope to single-level tagging or decline.
7. **§2.7 test/namespaces** — decline `ns` family; testing macros only on demand.
8. **Documentation pass** — §3 trap list into `docs/internals/clojure-surface.md`.

---

## 6. Acceptance criteria

- Each new name resolves from a clean `build/bender` (not only `bender`) and has a
  docstring in the house style.
- A test file per group (`test/suite-clojure-core.ben` or extend
  `test/suite-destructure.ben`) proving the Clojure contract: arity, nil
  handling, and immutability behavior of each collection helper.
- Every entry in §3 has a written decision in
  `docs/internals/clojure-surface.md` under "Semantic differences from Clojure",
  including the two that will bite (`:kw` access, `=` on mutable containers).
- `make test` green; no change to `src/core/compile.c`, `bytecode.c`, `vm.c`, or
  `asm.c`.
- Boot image still builds: `make` from clean produces `build/bender` and
  `build/bender_boot` without warnings.
