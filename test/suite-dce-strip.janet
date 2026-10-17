# B7.2 Part B, Piece 1c: wiring the `dce-reachable` verdict (Piece 1b,
# tools/callgraph.janet) into tools/mkimage.janet's ACTUAL stripping
# decision, behind the opt-in `--dce` flag. See
# docs/internals/b7.2-part-b-design-note.md, "Piece 1".
#
# The channel is a file, exactly like the pre-existing native-names.txt
# convention: on a `:safe` whole-program verdict AND `--dce`,
# callgraph.janet writes `<ndir>/dce-excluded.txt` (one bare def name
# per line); mkimage.janet reads it via `--dce-excluded FILE` and drops
# those top-level `defn`/`defn-` forms outright. An ABSENT file is the
# canonical encoding of BOTH "flag not passed" and ":unsafe verdict",
# so those two cases are mechanically indistinguishable to mkimage --
# the design note's "V1 is allowed to strip nothing" made structural
# rather than merely documented.
#
# TWO STRUCTURAL FINDINGS established by direct probing while building
# this piece, which bound what it can honestly claim. Both are asserted
# below rather than left as prose, because each one is the reason a
# particular test looks the way it does:
#
#   (1) `roots` in callgraph.janet is every def named "thunk", and
#       `reachable-from` follows BOTH "calls" and "contains" edges.
#       "contains" edges are emitted from each def's STRUCTURAL
#       `(d :defs)` nesting, so every top-level defn that survives
#       compilation is a structural child of its unit's thunk and is
#       therefore ALWAYS reachable. Probed across many fixture shapes
#       (plain unused defn, chained unused, dead-branch reference,
#       nested inner fn, `def` alias, value use, cross-module import):
#       every `:safe` verdict prints "excluded -". So Piece 1b's
#       exclusion set is EMPTY for every safe program today, and the
#       end-to-end `--dce` path is correctly a no-op. Narrowing the
#       root set to make it non-empty is Piece 2's job (precise
#       supervision roots), explicitly out of scope here.
#
#   (2) In mkimage.janet's single-thunk compilation model, a defn that
#       survives into the marshalled payload at all necessarily still
#       has a live symbol reference in that thunk, so deleting its form
#       textually yields "unknown symbol". The pre-existing
#       native-names stripping only works because it is PAIRED with a
#       prelude rebind; the DCE drop has no such pairing by design.
#       mkimage.janet therefore performs its OWN fail-closed
#       residual-reference audit over the surviving forms (a fixpoint,
#       since restoring one defn can re-reference another) and RESTORES
#       any name still mentioned instead of emitting a broken image.
#       Refusing to strip is always a correct outcome.
#
# Consequently the mkimage half is proven DIRECTLY, by handing it a
# hand-written exclusion file -- the same technique Piece 1a's harness
# uses to simulate stripping -- rather than through a verdict that
# cannot produce a non-empty set yet.
(import ./helper :prefix "" :exit true)
(start-suite)

(def root "/tmp/janet-dce-strip-suite")
(os/execute ["/bin/rm" "-rf" root] :p)
(os/mkdir root)
(def benv (merge @{} (os/environ) {"JANET_NO_PROFILE" "1"}))

(defn write-src [name body]
  (def p (string root "/" name ".janet"))
  (spit p body)
  p)

(var sh-seq 0)
(defn sh [cmd]
  # returns [exit-code combined-output]. Each invocation gets its OWN
  # capture file: a single shared path was clobbered by nested calls
  # (cc/strings/run) and produced spurious failures.
  (++ sh-seq)
  (def outf (string root "/.sh." sh-seq ".out"))
  # The command is brace-grouped before redirection. Without the group,
  # a trailing `|| true` in the command would bind the redirect to
  # `true` rather than to the whole pipeline, and the capture file would
  # never be created at all.
  (def code (os/execute ["/bin/sh" "-c"
                         (string "{ " cmd " ; } > " outf " 2>&1")] :e benv))
  [code (string (slurp outf))])

(defn mkimage [src out & extra]
  (sh (string "./build/bender tools/mkimage.janet " src " "
              (string/join extra " ") " " out)))

(defn payload-bytes [out-text]
  # mkimage prints "mkimage: N bytes, ..." -- N is the marshalled thunk
  # size, the only size signal that reflects what was actually embedded
  # (the emitted C also contains an 800-name core registry whose ORDER
  # is nondeterministic run-to-run, so whole-file byte comparison is not
  # an available oracle here -- confirmed by building one source twice
  # without any new flag and getting differing C).
  # Scan for the " bytes," marker specifically: mkimage also prints
  # "mkimage: dce: ..." diagnostic lines BEFORE the size line, so a
  # first-match-on-"mkimage: " search would read the wrong line.
  (def m (string/find " bytes," out-text))
  (if (nil? m) nil
    (do (def head (string/slice out-text 0 m))
      (def sp (string/find-all " " head))
      (def lastsp (if (empty? sp) nil (last sp)))
      (if (nil? lastsp) nil
        (scan-number (string/slice head (+ lastsp 1)))))))

(defn cc-link [cfile exe]
  (sh (string "cc -O2 -fwrapv -I build -I src/include " cfile
              " build/libjanet.a -lm -lpthread -o " exe)))

# =====================================================================
# 1. REGRESSION -- the single most important check. mkimage.janet's
#    stripping loop is on the path of EVERY build, so absence of the
#    flag must be absolutely indistinguishable from pre-Piece-1c
#    behavior.
# =====================================================================

(def reg-src
  (write-src "regression" `
(defn used-fn [x] (* x 2))
(defn helper [x] (+ x 1))
(print (used-fn 21))
(print (helper 5))
`))

# 1a. Plain image build with NO new flag: builds, runs, produces the
#     expected output. The DCE loop must not engage at all.
(def [reg-code reg-out] (mkimage reg-src (string root "/reg.c")))
(assert (= 0 reg-code) "regression: plain mkimage build succeeds with no --dce-excluded")
(assert (nil? (string/find "dce:" reg-out))
        "regression: no --dce-excluded means mkimage prints no dce diagnostics at all")
(def reg-bytes (payload-bytes reg-out))
(assert (not (nil? reg-bytes)) "regression: plain build reports a payload size")
(assert (> (or reg-bytes 0) 0) "regression: that payload size is nonzero")

(def reg-cc ((cc-link (string root "/reg.c") (string root "/reg")) 0))
(assert (= 0 reg-cc) "regression: plain build links")
(def reg-run ((sh (string root "/reg")) 1))
(assert (= "42\n6\n" reg-run) "regression: plain build output unchanged")

# 1b. Passing an EMPTY exclusion file must be identical to passing none
#     -- this is the ":unsafe verdict" and "flag absent" equivalence,
#     checked on the payload size, which is the part mkimage controls.
(def empty-ex (string root "/empty-excluded.txt"))
(spit empty-ex "")
(def [emp-code emp-out]
  (mkimage reg-src (string root "/emp.c") "--dce-excluded" empty-ex))
(assert (= 0 emp-code) "regression: empty exclusion file builds")
(assert (= reg-bytes (payload-bytes emp-out))
        "regression: empty exclusion file yields a byte-identical payload to no flag")
(assert (nil? (string/find "dce:" emp-out))
        "regression: an empty exclusion set prints no dce diagnostics (loop stays dormant)")

# 1c. An exclusion file that does not exist is also a no-op, not an
#     error -- the AOT driver only passes the flag when the file is
#     present, but mkimage must not depend on that for correctness.
(def [miss-code miss-out]
  (mkimage reg-src (string root "/miss.c")
           "--dce-excluded" (string root "/does-not-exist.txt")))
(assert (= 0 miss-code) "regression: a missing exclusion file is a no-op, not an error")
(assert (= reg-bytes (payload-bytes miss-out))
        "regression: missing exclusion file yields the same payload as no flag")

# 1d. The pre-existing native-names stripping path is untouched. Its
#     own machinery (strip + prelude rebind) must still work when the
#     DCE set is empty -- the loop's gate changed from
#     (next native-names) to (or native dce), so this is the proof the
#     native branch still reaches its stripping arm.
(def nat-dir (string root "/nat"))
(os/mkdir nat-dir)
(def nat-src
  (write-src "native" `
(defn used-fn [x] (* x 2))
(print (used-fn 21))
`))
(def [ncg-code ncg-out]
  (sh (string "./build/bender tools/callgraph.janet " nat-src
              " --emit-native-lib " nat-dir)))
(assert (= 0 ncg-code) "regression: callgraph --emit-native-lib still succeeds")
(def nat-names (string nat-dir "/native-names.txt"))
(assert (= :file (os/stat nat-names :mode)) "regression: native-names.txt still written")
(assert (not (nil? (string/find "used-fn" (string (slurp nat-names)))))
        "regression: used-fn still selected as a native kernel")
(def [nat-code nat-out]
  (mkimage nat-src (string root "/nat.c") "--native" nat-names))
(assert (= 0 nat-code) "regression: native stripping path still builds")
(assert (nil? (string/find "dce:" nat-out))
        "regression: a native-only build prints no dce diagnostics")

# =====================================================================
# 2. NEGATIVE -- a program containing a disqualifying pattern. With
#    --dce set, the gate must return :unsafe, write NO exclusion file,
#    and leave the build identical to a non---dce build.
# =====================================================================

(def unsafe-dir (string root "/unsafe"))
(os/mkdir unsafe-dir)
(def unsafe-src
  (write-src "unsafe" `
(defn target-fn [x] (* x 10))
(def dispatch @{:doubler target-fn})
(print ((get dispatch :doubler) 5))
`))

# 2a. --dce on a pattern-1 program: unsafe verdict, no file written.
(def [ucg-code ucg-out]
  (sh (string "./build/bender tools/callgraph.janet " unsafe-src
              " --emit-native-lib " unsafe-dir " --dce")))
(assert (= 0 ucg-code) "negative: callgraph run itself succeeds on an unsafe program")
(assert (not (nil? (string/find "dce unsafe pattern 1" ucg-out)))
        "negative: pattern-1 program still reports the Piece 1b unsafe verdict")
(assert (nil? (os/stat (string unsafe-dir "/dce-excluded.txt")))
        "negative: an :unsafe verdict writes NO exclusion file even with --dce")
(assert (nil? (string/find "dce emit" ucg-out))
        "negative: no emission line is printed on an unsafe verdict")

# 2b. ...and because no file exists, the resulting mkimage build is
#     identical to one with no DCE involvement whatsoever.
(def [uno-code uno-out] (mkimage unsafe-src (string root "/u-none.c")))
(assert (= 0 uno-code) "negative: unsafe program builds with no flag")
(def [udc-code udc-out]
  (mkimage unsafe-src (string root "/u-dce.c")
           "--dce-excluded" (string unsafe-dir "/dce-excluded.txt")))
(assert (= 0 udc-code) "negative: unsafe program builds with the (absent) exclusion file")
(assert (= (payload-bytes uno-out) (payload-bytes udc-out))
        "negative: unsafe verdict strips nothing -- payload byte-identical to no-flag build")

(def u-cc ((cc-link (string root "/u-dce.c") (string root "/u-dce")) 0))
(assert (= 0 u-cc) "negative: unsafe-program binary links")
(def u-run ((sh (string root "/u-dce")) 1))
(assert (= "50\n" u-run) "negative: unsafe-program runtime output is correct and unchanged")

# =====================================================================
# 3. POSITIVE -- the mkimage stripping half, proven directly with a
#    hand-written exclusion file naming a genuinely dead cluster
#    (finding (1): no safe program yields a non-empty set through the
#    verdict today, so the verdict cannot supply this input yet).
#    The cluster is self-contained: zzdeadcaller calls zzdeadmarker and
#    NOTHING else calls either, so both are droppable as a unit.
# =====================================================================

(def dead-src
  (write-src "dead" `
(defn used-fn [x] (* x 2))
(defn zzdeadmarker [x] (+ x 123456))
(defn zzdeadcaller [x] (zzdeadmarker (* x 7)))
(print (used-fn 21))
`))

(def [dbase-code dbase-out] (mkimage dead-src (string root "/d-base.c")))
(assert (= 0 dbase-code) "positive: baseline (unstripped) build succeeds")
(def dbase-bytes (payload-bytes dbase-out))

(def dead-ex (string root "/dead-excluded.txt"))
(spit dead-ex "zzdeadmarker\nzzdeadcaller\n")
(def [dstr-code dstr-out]
  (mkimage dead-src (string root "/d-strip.c") "--dce-excluded" dead-ex))
(assert (= 0 dstr-code) "positive: stripped build succeeds")
(assert (not (nil? (string/find "dce: stripped 2 of 2" dstr-out)))
        "positive: both dead-cluster defns are actually stripped, none restored")
(assert (nil? (string/find "refusing to strip" dstr-out))
        "positive: the residual-reference audit finds nothing to restore in a true dead cluster")

(def dstr-bytes (payload-bytes dstr-out))
(assert (not (nil? dstr-bytes)) "positive: stripped build reports a payload size")
(assert (not (nil? dbase-bytes)) "positive: baseline build reports a payload size")
(assert (< (or dstr-bytes 0) (or dbase-bytes 0))
        "positive: the stripped payload is genuinely SMALLER than the baseline")

# 3a. The symbol is gone from the LINKED BINARY, not merely unreported.
#     `strings` is the right instrument: these are boxed bytecode
#     bodies, so the function name lives in the marshalled payload
#     rather than as an `nm` symbol -- checked both ways below so the
#     evidence is unambiguous either way.
(def db-cc ((cc-link (string root "/d-base.c") (string root "/d-base")) 0))
(assert (= 0 db-cc) "positive: baseline binary links")
(def ds-cc ((cc-link (string root "/d-strip.c") (string root "/d-strip")) 0))
(assert (= 0 ds-cc) "positive: stripped binary links")

(def base-strings
  ((sh (string "strings " root "/d-base | grep -c zzdead || true")) 1))
(def strip-strings
  ((sh (string "strings " root "/d-strip | grep -c zzdead || true")) 1))
(assert (> (scan-number (string/trim base-strings)) 0)
        "positive: the dead cluster's name IS present in the unstripped binary")
(assert (= 0 (scan-number (string/trim strip-strings)))
        "positive: the dead cluster's name is ABSENT from the stripped binary (strings)")

(def strip-nm
  ((sh (string "nm " root "/d-strip 2>/dev/null | grep -ci zzdead || true")) 1))
(assert (= 0 (scan-number (string/trim strip-nm)))
        "positive: no zzdead* symbol in the stripped binary's symbol table either")

# 3b. Runtime behavior is identical -- the property that actually
#     matters, and the one Piece 1a's harness exists to certify.
(def dbase-run ((sh (string root "/d-base")) 1))
(def dstrip-run ((sh (string root "/d-strip")) 1))
(assert (= "42\n" dbase-run) "positive: baseline binary output is correct")
(assert (= dbase-run dstrip-run)
        "positive: stripped binary output is byte-identical to the baseline")

# 3c. The Piece 1a cross-check harness, run as the independent oracle
#     on the SAME names -- it builds both variants itself and diffs
#     their output, so this is a second, independently-implemented
#     confirmation rather than a restatement of 3b.
(def xck-exit
  (os/execute ["./build/bender" "tools/dce-crosscheck.janet" dead-src
               (string root "/xcheck") "zzdeadmarker" "zzdeadcaller"] :e benv))
(assert (= 0 xck-exit)
        "positive: Piece 1a cross-check harness PASSes on the stripped dead cluster")

# =====================================================================
# 4. FAIL-CLOSED -- finding (2). An exclusion naming a name that IS
#    still referenced by a surviving form must be REFUSED and the defn
#    restored, producing a correct image rather than a broken one. This
#    is the guard that makes the stripping arm safe to have at all: it
#    converts a potential miscompile into a loud refusal.
# =====================================================================

(def live-src
  (write-src "live" `
(defn used-fn [x] (* x 2))
(defn zzdeadmarker [x] (+ x 123456))
(print (used-fn 21))
(print (zzdeadmarker 0))
`))

(def [lbase-code lbase-out] (mkimage live-src (string root "/l-base.c")))
(assert (= 0 lbase-code) "fail-closed: baseline build of the live-reference fixture succeeds")

(def live-ex (string root "/live-excluded.txt"))
(spit live-ex "zzdeadmarker\n")
(def [lstr-code lstr-out]
  (mkimage live-src (string root "/l-strip.c") "--dce-excluded" live-ex))
(assert (= 0 lstr-code)
        "fail-closed: a bad exclusion does NOT break the build -- it is refused")
(assert (not (nil? (string/find "refusing to strip zzdeadmarker" lstr-out)))
        "fail-closed: the refusal is reported loudly, naming the function")
(assert (not (nil? (string/find "dce: stripped 0 of 1" lstr-out)))
        "fail-closed: the accounting reports zero of one candidate actually stripped")
(assert (not (nil? (string/find "restored: zzdeadmarker" lstr-out)))
        "fail-closed: the restored name is named in the summary")
(assert (= (payload-bytes lbase-out) (payload-bytes lstr-out))
        "fail-closed: the refused strip yields a payload identical to the baseline")

(def l-cc ((cc-link (string root "/l-strip.c") (string root "/l-strip")) 0))
(assert (= 0 l-cc) "fail-closed: the restored build links (no dangling symbol)")
(def l-run ((sh (string root "/l-strip")) 1))
(assert (= "42\n123456\n" l-run)
        "fail-closed: the restored build produces fully correct output")

# =====================================================================
# 5. VERDICT PLUMBING -- the end-to-end `--dce` path on a SAFE program.
#    Finding (1) means the honest, expected result today is an EMPTY
#    exclusion file and zero functions stripped. Asserting that
#    explicitly is the point: it documents mechanically that the safe
#    path is currently a no-op, so a future change that makes it
#    non-empty (Piece 2) cannot slip in unnoticed.
# =====================================================================

(def safe-dir (string root "/safe"))
(os/mkdir safe-dir)
(def safe-src
  (write-src "safe" `
(defn used-fn [x] (* x 2))
(defn helper [x] (+ x 1))
(print (used-fn 21))
(print (helper 5))
`))

# 5a. Without --dce, callgraph still prints the Piece 1b verdict (that
#     behavior is unchanged) but writes nothing.
(def [s0-code s0-out]
  (sh (string "./build/bender tools/callgraph.janet " safe-src
              " --emit-native-lib " safe-dir)))
(assert (= 0 s0-code) "verdict: safe program analyses cleanly without --dce")
(assert (not (nil? (string/find "dce safe reachable" s0-out)))
        "verdict: the Piece 1b diagnostic verdict still prints without --dce")
(assert (nil? (os/stat (string safe-dir "/dce-excluded.txt")))
        "verdict: no --dce means no exclusion file is written at all")

# 5b. With --dce, the file IS written -- and is empty, because finding
#     (1) makes every top-level defn reachable through `contains`.
(def [s1-code s1-out]
  (sh (string "./build/bender tools/callgraph.janet " safe-src
              " --emit-native-lib " safe-dir " --dce")))
(assert (= 0 s1-code) "verdict: safe program analyses cleanly with --dce")
(assert (not (nil? (string/find "dce safe reachable" s1-out)))
        "verdict: --dce does not change the verdict itself, only emission")
(assert (not (nil? (string/find "excluded -" s1-out)))
        "verdict: finding (1) -- a safe program's exclusion set is empty (contains-reachable)")
(def safe-ex (string safe-dir "/dce-excluded.txt"))
(assert (= :file (os/stat safe-ex :mode))
        "verdict: a :safe verdict under --dce DOES write the exclusion file")
(assert (= 0 (length (string (slurp safe-ex))))
        "verdict: and that file is empty today -- the safe path is honestly a no-op")
(assert (not (nil? (string/find "dce emit 0 excluded names" s1-out)))
        "verdict: the emission line reports zero names, matching the empty file")

# 5c. Feeding that real, verdict-produced file to mkimage strips
#     nothing, exactly as a no-flag build would.
(def [s2-code s2-out] (mkimage safe-src (string root "/s-none.c")))
(def [s3-code s3-out]
  (mkimage safe-src (string root "/s-dce.c") "--dce-excluded" safe-ex))
(assert (and (= 0 s2-code) (= 0 s3-code)) "verdict: both safe builds succeed")
(assert (= (payload-bytes s2-out) (payload-bytes s3-out))
        "verdict: the real verdict file strips nothing -- payload identical to no-flag")

# 5d. The --dce flag is rejected without --native, rather than silently
#     ignored: the gate lives in the native-only callgraph pass.
(def [dn-code dn-out]
  (sh (string "./build/bender --aot " safe-src " --dce --no-auto-pgo -o "
              root "/should-not-exist")))
(assert (not= 0 dn-code) "verdict: --dce without --native is rejected, not silently dropped")
(assert (not (nil? (string/find "--dce requires --native" dn-out)))
        "verdict: the rejection explains why --dce needs --native")

(end-suite)
