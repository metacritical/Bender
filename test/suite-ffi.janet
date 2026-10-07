# Copyright (c) 2026 Calvin Rose & contributors
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to
# deal in the Software without restriction, including without limitation the
# rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
# sell copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in
# all copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
# FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
# IN THE SOFTWARE.

(import ./helper :prefix "" :exit true)
(start-suite)

(var has-ffi (dyn 'ffi/native))
(def has-full-ffi
  (and has-ffi
       (when-let [entry (dyn 'ffi/calling-conventions)]
         (def fficc (entry :value))
         (> (length (fficc)) 1)))) # all arches support :none

# FFI check
# d80356158
(compwhen has-ffi
  (ffi/context))

(compwhen has-ffi
  (ffi/defbind memcpy :ptr [dest :ptr src :ptr n :size]))
(compwhen has-full-ffi
  (def buffer1 @"aaaa")
  (def buffer2 @"bbbb")
  (memcpy buffer1 buffer2 4)
  (assert (= (string buffer1) "bbbb") "ffi 1 - memcpy"))

# cfaae47ce
(compwhen has-ffi
  (assert (= 8 (ffi/size [:int :char])) "size unpacked struct 1")
  (assert (= 5 (ffi/size [:pack :int :char])) "size packed struct 1")
  (assert (= 5 (ffi/size [:int :pack-all :char])) "size packed struct 2")
  (assert (= 4 (ffi/align [:int :char])) "align 1")
  (assert (= 1 (ffi/align [:pack :int :char])) "align 2")
  (assert (= 1 (ffi/align [:int :char :pack-all])) "align 3")
  (assert (= 26 (ffi/size [:char :pack :int @[:char 21]]))
          "array struct size"))

(compwhen has-ffi
  (assert-error "bad struct issue #1512" (ffi/struct :void)))

# native memory primitives: pointer arithmetic, typed poke, calloc,
# realloc, executable pages (allocation + protection only -- executing
# hand-written machine code is arch-specific, see docs).
(compwhen has-full-ffi
  (def nm-p (unsafe/malloc 64))
  (assert (unsafe/ptr-eq nm-p nm-p) "ptr-eq self")
  (assert (not (unsafe/ptr-eq nm-p nil)) "ptr-eq nil")
  (def nm-q (unsafe/ptr-add nm-p 16))
  (assert (unsafe/ptr-eq (unsafe/ptr-sub nm-q 16) nm-p) "ptr add/sub round-trip")
  (assert (unsafe/ptr-eq (unsafe/ptr-add nm-p -8) (unsafe/ptr-sub nm-p 8)) "signed offsets")
  (unsafe/poke :int nm-p 0x12345678)
  (unsafe/poke :double nm-p 2.5 8)
  (assert (= 0x12345678 (ffi/read :int nm-p)) "poke/peek int")
  (assert (= 2.5 (ffi/read :double nm-p 8)) "poke/peek double+offset")
  (assert (= 0x12345678 (ffi/read :int (unsafe/ptr-add nm-q -16))) "peek via derived ptr")
  (unsafe/free nm-p)
  (def nm-c (unsafe/calloc 4 8))
  (each i (range 4) (assert (= 0 (ffi/read :int nm-c (* i 8))) "calloc zeroed"))
  (unsafe/poke :int nm-c 99)
  (def nm-c2 (unsafe/realloc nm-c 64))
  (assert (= 99 (ffi/read :int nm-c2)) "realloc preserves")
  (unsafe/poke :int nm-c2 77 60)
  (assert (= 77 (ffi/read :int nm-c2 60)) "realloc extended")
  (unsafe/free nm-c2)
  (def nm-x (unsafe/exec-alloc 4096))
  (assert (not (unsafe/ptr-eq nm-x nil)) "exec-alloc")
  (assert (nil? (unsafe/exec-protect nm-x 100)) "exec-protect"))

(compwhen has-ffi
  (def buf @"")
  (ffi/write :u8 10 buf)
  (assert (= 1 (length buf)))
  (ffi/write :u8 10 buf)
  (assert (= 2 (length buf))))

(end-suite)
