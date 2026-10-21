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

(defn tuple-alias-loop [n]
  (def pair (tuple 2 3))
  (var i 0)
  (var total 0)
  (while (< i n)
    (+= total (pair-sum pair))
    (++ i))
  total)
