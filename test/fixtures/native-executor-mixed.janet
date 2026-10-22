# Mixed-operation module for the stage-2 native executor tests. Imported by
# test/fixtures/native-executor.janet exactly like ttfx imports src/scene.janet,
# so intra-module references compile to constants (no environment capture).
(def shared @{"n" 0 :tag 7})
(def acc @[])
(defn callee [x] (* x 2))
(defn boundary [n]
  (def arr @[])
  (var ai 0)
  (while (< ai n) (array/push arr nil) (++ ai))
  (def b @"")
  (var s 0)
  (var i 0)
  (while (< i n)
    # Keep the loop head a useful native region: three native ops
    # (condition + two adds) before the first VM boundary.
    (+= s 0.25)
    (+= s 0.25)
    # Nil value forces janet_table_put's removal branch (a no-op for an
    # absent numeric key) and therefore a VM PUT boundary from a native
    # loop body, with no preceding call.
    (put shared i nil)
    (+= s (callee i))
    (+= s (get shared "n"))
    (+= s (in shared :tag 0))
    (+= s (length shared))
    (+= s (/ i 1))
    (put shared "n" i)
    # Non-primitive keys and nil values deliberately stay on the VM path;
    # keep one of each so the opcode-exit counters still prove the GET/PUT
    # fallbacks remain reachable after the fast paths are enabled.
    (put shared (tuple i) nil)
    (+= s (if (get shared (tuple i)) 1 0))
    (put arr i i)
    (+= s (arr i))
    (buffer/push-string b "x")
    (+= s (length b))
    (array/push acc (tuple i (+ i 1)))
    (if (nil? nil) (+= s 1))
    (if (nil? i) (+= s 1000))
    (if (empty? arr) (+= s 1000))
    (if (empty? []) (+= s 1))
    (if (symbol? 'x) (+= s 1))
    (if (cfunction? length) (+= s 1))
    (if (truthy? 0) (+= s 1))
    (if (truthy? nil) (+= s 1000))
    (if (true? true) (+= s 1))
    (if (false? false) (+= s 1))
    (+= s 0.5)
    (++ i))
  s)

(defn iterate-table [t]
  (var s 0)
  (var k (next t nil))
  (while (not (nil? k))
    (+= s 1)
    (+= s (length (string k)))
    (+= s 2)
    (+= s 3)
    (set k (next t k)))
  s)
(defn tail-boundary [n]
  (var i 0)
  (while (< i n) (++ i))
  (callee i))
(defn build-buffer [n]
  (var b @"")
  (var i 0)
  (while (< i n)
    (buffer/push-byte b (+ 97 (mod i 3)))
    (++ i))
  (length b))
(defn unsupported-bit [n]
  (var i 0)
  (while (< i n) (set i (bor i 1)) (++ i))
  i)
(defn reset []
  (put shared "n" 0)
  (put shared :tag 7)
  (array/clear acc))
(defn snapshot []
  (tuple (get shared "n") (get shared :tag) (length shared) (length acc)
         (if (empty? acc) nil (tuple ;acc))))

# Container-as-function call, kept separate from boundary so the callee is
# a runtime parameter and the function has no opaque literal constants.
# (table key), (struct key), (array index), (tuple index), (string index),
# and (buffer index) all execute through janet_native_container_call_fast
# when argc == 1. Composite/out-of-range keys fall back to the VM.
(defn container-call [n callee key]
  (var s 0)
  (var i 0)
  (while (< i n)
    (+= s 1)
    (+= s 1)
    (+= s (callee key))
    (++ i))
  s)
