# -Oz is a binary-only size mode, not a change to PGO parity semantics.
(import ./helper :prefix "" :exit true)
(start-suite)

(def source "/tmp/janet-aot-size-source.janet")
(def regular "/tmp/janet-aot-size-regular")
(def small "/tmp/janet-aot-size-small")
(def hybrid "/tmp/janet-aot-size-hybrid")
(def gated "/tmp/janet-aot-size-gated")
(spit source "(defn twice [x] (* x 2))\n(print (twice 42))\n")
(assert (= 0 (os/execute ["./build/bender" "--aot" source "-o" regular] :p))
        "ordinary AOT builds")
(assert (= 0 (os/execute ["./build/bender" "--aot" source "-Oz" "-o" small] :p))
        "size AOT builds")
(assert (= 0 (os/execute ["./build/bender" "--aot" source "--native" "-Oz"
                         "--no-auto-pgo" "-o" hybrid] :p))
        "size hybrid builds")
(assert (> (os/stat regular :size) (os/stat small :size))
        "size build shrinks plain AOT binary")
(assert (os/stat hybrid :size) "hybrid size binary installed")
(assert (= 0 (os/execute ["./build/bender" "--aot" source "--native" "-Oz"
                         "--pgo" "" "-o" gated] :p))
        "size build preserves explicit parity gate")
(assert (os/stat gated :size) "gated size binary installed")
(defn output [binary]
  (def path (string binary ".stdout"))
  (def file (file/open path :w))
  (def code (os/execute [binary] :e (merge @{} (os/environ) {:out file})))
  (file/close file)
  [code (string (slurp path))])
(assert (deep= (output regular) (output small)) "plain size output agrees")
(assert (deep= (output regular) (output hybrid)) "hybrid size output agrees")
(assert (deep= (output regular) (output gated)) "gated size output agrees")
(assert (not= 0 (os/execute ["./build/bender" "--aot" source "-Oz" "-c"
                              "-o" "/tmp/janet-aot-size-invalid"] :p))
        "size mode rejects source-only build")
# UPX cannot package Mach-O; do not install an unchecked output.
(when (= (os/which) :macos)
  (def packed (string regular ".upx"))
  (assert (not= 0 (os/execute ["./build/bender" "--aot" source "--upx"
                              "-o" packed] :p))
          "UPX rejects unsupported Mach-O target")
  (assert (nil? (os/stat packed)) "UPX rejection installs no binary"))
(end-suite)
