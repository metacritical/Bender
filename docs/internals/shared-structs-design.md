# M5 design review: shareable immutable values across VMs

Status: IMPLEMENTED 2026-10-06 (`core/shared` in src/core/ev.c +
test/suite-shared.janet, 34 pins). This document is the review that
preceded it; §5 verdicts below each carry their outcome.

## 1. GC facts (this tree)

- Heaps are per-VM arenas: `janet_vm.blocks`, `janet_vm.string_blocks`,
  `janet_vm.weak_blocks` (gc.c), backed by a per-VM slab directory
  (`JanetVM.slab_state`; 16 KiB pages, `JANET_SLAB_PAGE_MASK`).
- Mark resolves ownership through the CURRENT VM's directory only
  (`janet_slab_page_for` returns NULL for foreign pointers; mark
  functions additionally check page magic). Generations are
  page-granular with a remembered discipline (minor collections +
  barriers, `janet_gc_barrier`).
- Threaded abstracts live OUTSIDE this system: atomic refcount
  (`janet_atomic_inc/dec`, abstract.c/capi.c), type finalizer on
  zero (`janet_abstract_decref_maybe_free`), slab-aware release.
  Channels are the working proof: shared across threads, never
  GC-managed, transferred by pointer under UNSAFE marshal
  (`JANET_MEMORY_THREADED_ABSTRACT` check, marsh.c) with a transit
  incref. Sweep only touches them to drop references.

## 2. Why cross-heap pointers are unsound (no protocol)

If VM-B holds a raw pointer into VM-A's heap, everything breaks in
both directions: B's mark cannot establish ownership (A's directory
is invisible, magic check is per-page not per-owner); A's sweep
frees objects B still references (B's roots are invisible to A);
generational minors in either VM are blind to the cross edge. Any
design that leaves a GC-managed pointer visible to a foreign VM is
wrong by construction. There are two escapes: copy at the boundary
(what marshal already does — M5 must beat copying to matter), or
keep shared memory out of every GC heap.

## 3. Option (a): ownership transfer (destructive send) — REJECTED

Transfer without copy would mean unlinking the object from A's arena
and linking it into B's. Slab pages pack unrelated small objects
side by side, so single-object transfer is impossible; page-granular
transfer fails the moment a page is shared (always, for small
objects). A "transfer" would copy anyway and merely skip the sender
forgetting — saving nothing while adding protocol. Rejected: no
implementation should be attempted.

## 4. Option (b): refcounted off-heap flattened blobs — FEASIBLE

Follow the threaded-abstract pattern instead of integrating with
the GC at all (this is Erlang's refc-binary shape, not shared-heap):

- New abstract type, e.g. `core/shared`, allocated threaded
  (`janet_abstract_threaded`): `{atomic refcount, flattened payload
  in malloc'd memory}`. The payload is a SELF-CONTAINED encoding —
  never GC pointers.
- `shareable?` predicate at construction: allow nil/boolean/number,
  nested shared blobs, and strings copied out as (bytes, length);
  REJECT tables, arrays, buffers, functions, fibers, ordinary
  abstracts, and anything reachable through them (deep check —
  protos included). Anything else is a type error, fail-closed.
- Reads copy out: `(shared/get s k)` materializes fresh caller-heap
  values (numbers directly, strings as new strings). Interior
  pointers are never exposed as Janets.
- Lifetime: every marshal transfer increfs (free via the existing
  UNSAFE threaded passthrough — cross-thread movement needs NO new
  marshal code); every wrapper collection decrefs through the
  type's `gc` finalizer; payload frees at zero. No GC heap ever sees
  the payload, so no collector changes are required anywhere.
- Cost model: construction pays one deep copy + validation; sends
  pay one atomic inc; reads pay per-access copies. Wins exactly
  when a value is read in N VMs with N large (broadcast constants,
  static game data) — same break-even as refc binaries.

## 5. Open proofs required before M5 code — OUTCOMES

1. Wrapper-finalizer reliability: NOT NEEDED as designed — the
   payload is inline in the abstract allocation (no nested refs),
   so `gc`/`gcperthread` are both NULL and freeing is automatic
   with the allocation. Verified by collection + post-GC-read pins
   and ASan runs.
2. Refcount discipline audit: ntau — the abstract's own atomic
   refcount owns the lifetime; marshal-passthrough increfs pair
   with wrapper-collection decrefs (existing machinery, unchanged).
3. Marshal-passthrough coverage: CONFIRMED — cross-thread mailbox
   transfer works with zero new marshal code; safe-mode
   marshal/unmarshal implemented via payload bytes (plus abstract
   registry registration, which the first cut missed — safe
   unmarshal needs `janet_register_abstract_type`, and the
   framework requires `janet_unmarshal_abstract_reuse`).
4. Predicate edge cases: CONFIRMED with one real trap found in
   testing — structs are open-addressed hash tables, so iteration
   MUST go to `janet_struct_capacity` skipping nil keys (iterating
   to `length` silently skips entries whose slots hash past the
   front — single-entry structs validated vacuously and encoded
   garbage). Fixed + pinned. Protos rejected (fail-closed).

## 6. Explicit non-goals

No sharing of mutable state (by construction, not policy); no
zero-copy for heap objects (copying stays the default — M5 only
pays off past the break-even above); no GC integration (the whole
point is to avoid touching the collector); no cross-node form
(serialization stays marshal-based until distribution exists).
