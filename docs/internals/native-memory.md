# Native memory: pointers, allocation, executable pages

This module (`unsafe/ptr-add`, `unsafe/ptr-sub`, `unsafe/ptr-eq`, `unsafe/poke`,
`unsafe/calloc`, `unsafe/realloc`, `unsafe/exec-alloc`, `unsafe/exec-protect`,
alongside the pre-existing `unsafe/malloc`, `unsafe/free`, `unsafe/jitfn`,
`unsafe/trampoline`, `unsafe/pointer-buffer`, `unsafe/pointer-cfunction`,
`ffi/read`, `ffi/write`) exists for one reason: **C parity for
systems work.** Ordinary programs should use buffers, arrays, structs
and `ffi/defbind`. These primitives are high-risk, obscure, and not
for day-to-day use — wrong use corrupts memory with no safety net,
and the garbage collector cannot see raw pointers at all.

## 1. Pointers: declare, move, compare

A `:ptr` flows through signatures, `unsafe/malloc`, and `ffi/lookup`.
Three operations make it usable as a value:

```janet
(def p (unsafe/malloc 64))
(unsafe/ptr-eq p p)                  # => true
(def q (unsafe/ptr-add p 16))        # 16 bytes forward
(unsafe/ptr-eq (unsafe/ptr-sub q 16) p) # => true (round-trip)
(unsafe/ptr-eq (unsafe/ptr-add p -8) (unsafe/ptr-sub p 8)) # signed offsets
(unsafe/ptr-eq p nil)                # => false (nil is the null test)
(unsafe/free p)
```

Rules: offsets are signed byte counts; nothing is bounds-checked;
`nil` is the only null. Keep every malloc'd pointer reachable from a
Janet root (table, array, fiber slot) or accept leaks/crashes — the
GC never traces raw addresses.

## 2. Typed peek and poke through raw pointers

`ffi/read` already accepts raw pointers; `unsafe/poke` is its inverse
(any `ffi` type, optional byte offset):

```janet
(def p (unsafe/malloc 64))
(unsafe/poke :int p 0x12345678)
(unsafe/poke :double p 2.5 8)
(ffi/read :int p)            # => 0x12345678
(ffi/read :double p 8)       # => 2.5
# negative offsets don't exist on read -- derive first:
(ffi/read :int (unsafe/ptr-add p -16))
(unsafe/free p)
```

Struct values marshal as tuples, exactly like everywhere else:

```janet
(def vec2 (ffi/struct :float :float))
(unsafe/poke vec2 p [60.0 80.0] 16)
(ffi/read vec2 p 16)  # => [60.0 80.0]
```

Misaligned or wild writes corrupt the process. `unsafe/pointer-buffer`
wraps an address as a fixed-capacity buffer for the safer
buffer-function surface instead.

## 3. Allocation: malloc / calloc / realloc / free

```janet
(def c (unsafe/calloc 4 8))     # 4x8 zeroed bytes
(ffi/read :int c 24)          # => 0
(unsafe/poke :int c 99)
(def c2 (unsafe/realloc c 64))   # grows, preserves, may move
(ffi/read :int c2)            # => 99
(unsafe/free c2)                 # every path ends here
```

Ownership rules: `malloc`/`calloc`/`realloc(nil, n)` return fresh
blocks owned by you; `realloc` invalidates the old pointer even on
failure paths — always rebind; `free` exactly once; `ffi/free nil`
is a safe no-op. Never `free` memory you didn't allocate (no stack
addresses, no `unsafe/pointer-buffer` interiors, no exec mappings).

## 4. Executable memory: one-shot and hand-rolled JIT

Two shapes. One-shot — bytes in, callable function out:

```janet
# aarch64 only: movz x0, #42 ; ret
(def code (buffer/new-filled 8 0))
(put code 0 0x40) (put code 1 0x05) (put code 2 0x80) (put code 3 0xD2)
(put code 4 0xC0) (put code 5 0x03) (put code 6 0x5F) (put code 7 0xD6)
(def f (unsafe/jitfn code))                       # RW -> copy -> RX
(def sig (ffi/signature :default :int))
(ffi/call f sig)                               # => 42
```

Hand-rolled — for JITs that emit code incrementally:

```janet
(def page (unsafe/exec-alloc 4096))  # mapped read/write, NOT executable
# ... emit bytes via unsafe/poke on (unsafe/pointer-buffer page 4096) ...
(unsafe/exec-protect page 4096)      # flip to read/execute; writing now faults
# ... call through ffi/call or unsafe/pointer-cfunction ...
# mappings live until process exit; there is deliberately no free.
```

`unsafe/trampoline` covers the reverse direction (C calling back into
Janet). All three executable-memory bindings require the
`JANET_SANDBOX_FFI_JIT` capability; the rest require
`JANET_SANDBOX_FFI_USE`. Sizes round up to whole pages.

## 5. What can go wrong (read twice)

- GC blindness: raw pointers are invisible roots. A malloc'd block
  referenced only from C stays alive only if you also root it.
- No bounds, no alignment checks: overruns corrupt the heap, the
  stack, or the program text, silently.
- Use-after-free and double-free behave like C: anything, later.
- W^X violations: never write to an exec-protected page; never call
  a merely-writable page.
- Callbacks (`unsafe/trampoline`, Janet functions passed as `:ptr`)
  are gc-rooted on pass — that root is never released implicitly.

Test pins live in `test/suite-ffi.janet` (portable subset) and
`examples/ffi/` (arch-specific calls). Anything new here gets both.
