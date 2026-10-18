# Mixed-operation module for the stage-2 native executor tests. Imported by
# test/fixtures/native-executor.janet exactly like ttfx imports src/scene.janet,
# so intra-module references compile to constants (no environment capture).
(def shared @{"n" 0 :tag 7})
(def acc @[])
(defn callee [x] (* x 2))
(defn boundary [n]
  (var s 0)
  (var i 0)
  (while (< i n)
    (+= s (callee i))
    (+= s (get shared "n"))
    (+= s (in shared :tag 0))
    (+= s (length shared))
    (+= s (/ i 1))
    (put shared "n" i)
    (array/push acc (tuple i (+ i 1)))
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
