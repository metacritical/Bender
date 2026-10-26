# Generic stage-2 read/iterator/scratch-tuple/native-alias fixture.
# The C library is built in the suite's private scratch root. No application
# source or process-global dependency is needed to exercise struct-by-value.
(compwhen (dyn 'ffi/native)
  (comptime (ffi/context (os/getenv "JANET_NATIVE_OPS_LIB") :lazy true)))

(def pair-type (ffi/struct :int :int))
(ffi/defbind-alias janet_test_pair_sum pair-sum :int [pair pair-type])
(ffi/defbind-alias janet_test_pair_bump pair-bump pair-type [pair pair-type])

(defn indexed-loop [xs]
  (var total 0)
  (each cell xs
    (def [n sym missing] cell)
    (when (= sym " ") (+= total n))
    (when (= missing nil) (++ total)))
  total)

(defn scratch-loop [n]
  (var i 0)
  (var total 0)
  (while (< i n)
    (+= total (pair-sum [i (+ i 1)]))
    # A struct-returning alias allocates a tuple. The executor must root its
    # arguments through the call and store the result BEFORE checking GC.
    (def next (pair-bump [i (+ i 1)]))
    (+= total (get next 0))
    (++ i))
  total)

# These reads deliberately use the register-key `get` opcode, not `geti`.
# The receiver alternates between arrays and tuples; a table forces the
# original VM getter at the exact PC after native arithmetic has run.
(defn get-loop [xs n]
  (var i 0)
  (var total 0)
  (while (< i n)
    (def row (xs (mod i (length xs))))
    (def key (mod i 3))
    (def value (get row key))
    (when value (+= total value))
    (++ i))
  total)

# Existing-index buffer/array stores are native; buffer growth, invalid keys
# and invalid values must fall back to the original JOP_PUT before mutation.
(defn buffer-loop [xs n]
  (var i 0)
  (while (< i n)
    (put xs i (+ i 257))
    (++ i))
  (var total 0)
  (set i 0)
  (while (< i n)
    (+= total (get xs i))
    (++ i))
  total)

(defn buffer-guard-loop [xs key value]
  (var i 0)
  (while (< i 3)
    (put xs key value)
    (++ i))
  xs)

(defn tuple-alias-loop [n]
  (def pair (tuple 2 3))
  (var i 0)
  (var total 0)
  (while (< i n)
    (+= total (pair-sum pair))
    (++ i))
  total)
