# tools/mkimage.janet — AOT spike step 1 (see docs/internals/aot-plan.md Phase 0).
#
# Compiles SRC.janet ahead of time to OUT.c, embedding a marshalled top-level
# thunk as a static byte array plus a main() that runs it. All values stay
# boxed; JOP_* ops still execute in the bytecode VM. Run under host janet
# (macros expand at build time, as today):
#
#   ./build/bender tools/mkimage.janet app.janet app.c
#
# Model: the whole file compiles as ONE implicit (do ...) thunk, so
# cross-form references resolve Naturally with no build-time execution.
# Globals referenced by the program bake in as constants (compile.c
# janetc_resolve); cfunctions resolve through a name registry against the
# runtime core (resolve_core). Known spike limitations (documented):
# image valid only for the same janet build that produced it; no argv
# forwarding yet; (dyn :args) use is out of scope for v0.
# Refusals (hard error here; --defer-fallback comes with the C driver):
# eval/compile/dobytes on runtime strings is NOT detected statically yet —
# programs using them build but keep interpreting at runtime.

# :args[0] is this script's path;
# SRC OUT [--native names.txt] [--dce-excluded dce-excluded.txt]
(def rawargs (tuple/slice (dyn :args) 1))
(var args @[])
(var native-names @{})
(var native-names-file nil)
# B7.2 Part B, Piece 1c: names callgraph.janet's dce-reachable gate
# proved unreachable under a :safe whole-program verdict. Absent flag
# == absent file == empty set == strip nothing, which is exactly the
# pre-Piece-1c behavior of this file.
(var dce-excluded-names @{})
(var dce-excluded-file nil)
(var nai 0)
(while (< nai (length rawargs))
  (def a (rawargs nai))
  (cond
    (and (= a "--native") (< (+ nai 1) (length rawargs)))
    (do (set native-names-file (rawargs (+ nai 1)))
        (++ nai))
    (and (= a "--dce-excluded") (< (+ nai 1) (length rawargs)))
    (do (set dce-excluded-file (rawargs (+ nai 1)))
        (++ nai))
    (array/push args a))
  (++ nai))
(when (< (length args) 2)
  (eprint "usage: mkimage.janet SRC.janet OUT.c [--native names.txt]"
          " [--dce-excluded dce-excluded.txt]")
  (os/exit 1))
(def srcpath (get args 0))
(def outpath (get args 1))

(def src (slurp srcpath))
(when (nil? src)
  (eprint "mkimage: cannot read " srcpath)
  (os/exit 1))


# Pre-execute top-level import/require/use forms into the unit env -- the
# same thing normal loading does (compile+exec each form in turn) before
# later forms compile. Single-thunk compilation otherwise never merges
# prefixed module bindings (WIPEMOD/GROUPS etc.), so multi-module programs
# fail with "unknown symbol". Modules load through the real loader
# (require: cached, resolves relatives recursively), then merge-module
# applies :as/:prefix/:only/:export exactly like import*. Runtime requires
# inside functions are out of scope (same as the one-thunk model).
# Keep in sync between tools/callgraph.janet and tools/mkimage.janet.
(defn preimport-unit-imports [env unitfile forms]
  (def prev-current-file (dyn :current-file))
  (setdyn :current-file unitfile)
  (each form forms
    (when (and (tuple? form) (>= (length form) 2))
      (def head (form 0))
      (when (and (symbol? head)
                 (or (= head (quote import))
                     (= head (quote use))
                     (= head (quote require))))
        (try
          (do (def path (string (form 1)))
            (def modenv (require path))
            (when (or (= head (quote import)) (= head (quote use)))
              (def kargs (table ;(tuple/slice form 2)))
              (def as (get kargs :as))
              (def prefix (get kargs :prefix))
              (def only (get kargs :only))
              (def ep (or (get kargs :export) (= head (quote use))))
              (def pfx (or (and as (string as "/")) prefix
                           (string (last (string/split "/" (string path))) "/")))
              (merge-module env modenv pfx ep only)))
          ([e] (eprint "aot: import pre-execution failed for " (form 1) ": " e)
               (os/exit 2))))))
  (setdyn :current-file prev-current-file))

# Fresh core env. make-env protos to root-env, so walk the whole chain for
# the pristine snapshot (keys sees own entries only).
(def env (make-env))
# Mirror the janet CLI: the script's directory joins the module syspath so
# relative imports (../src/...) resolve the way a normal `janet f.janet`
# run resolves them.
(do (def ssrc srcpath)
  (def slash-idx (string/find "/" (string/reverse ssrc)))
  (module/add-syspath
    (if (nil? slash-idx) "." (string/slice ssrc 0 (- (length ssrc) slash-idx 1)))))
(def snap @{})
(var t env)
(while t
  (each k (keys t)
    (when (nil? (in snap k))
      (def e (in t k))
      # Env entries are binding descriptors (@{:value v ...}); the compiler
      # bakes the unwrapped :value (util.c janet_binding_from_entry).
      (def v (if (= (type e) :table) (in e :value) e))
      (when (not (nil? v)) (put snap k v))))
  (set t (table/getproto t)))

(var forms
  (try (parse-all src)
    ([err] (eprint srcpath ": parse error: " err) (os/exit 1))))
(preimport-unit-imports env srcpath forms)

# --native names.txt (step 12 hybrid): names in the file were compiled to
# native kernels (native.c, sibling of output). Strip their defn forms —
# the boxed copies would overwrite the native wrappers at thunk-run — and
# prepend a prelude that rebinds each name from the env, where the app's
# native_init registered the wrapper cfunctions pre-unmarshal. Same-unit
# call sites then compile against the prelude bindings -> wrappers.
(when native-names-file
  (def nf (slurp native-names-file))
  (each l (string/split "\n" nf)
    (when (> (length l) 0) (put native-names (symbol l) true))))

# Phase 4 (type-generator plan): dispatch functions. Their defn is NOT
# stripped -- the C dispatcher's default arm needs the boxed body at
# runtime via janet_call. Instead the defn stays, renamed to
# <name>%boxed, and a registration form immediately after it installs
# the boxed fn into native.c's n_disp_boxed table (native/dispatch-set-boxed,
# registered by native_init pre-unmarshal -- hence the indirect
# (curenv) resolution, same reason as the prelude rebinds below).
(var dispatch-names @{})
(when native-names-file
  (def df (string/replace-all "native-names.txt" "native-dispatch.txt" native-names-file))
  (when (and (not= df native-names-file)
             (= :file (os/stat df :mode)))
    (each l (string/split "\n" (slurp df))
      (when (> (length l) 0) (put dispatch-names (symbol l) true)))))

# B7.2 Part B, Piece 1c: the dce-reachable exclusion set. Only written
# by callgraph.janet under --dce AND a :safe whole-program verdict, so
# reaching this point with a non-empty set already means the paranoid
# gate positively cleared every construct in the program. This file
# performs no reachability reasoning of its own: it is a pure consumer
# of that verdict, exactly as it is a pure consumer of native-names.txt.
(when dce-excluded-file
  (when (= :file (os/stat dce-excluded-file :mode))
    (each l (string/split "\n" (slurp dce-excluded-file))
      (when (> (length l) 0) (put dce-excluded-names (symbol l) true)))))

# The loop below now runs when EITHER set is non-empty. The two checks
# are independent and the native one has strict priority: a name in
# both sets MUST take the native path, because native stripping is
# paired with a prelude rebind (and possibly a %boxed dispatch form)
# that the unconditional DCE drop would leave dangling.
(when (or (next native-names) (next dce-excluded-names))
  # strip proven defn forms; keep everything else. Phase 4 exception:
  # a dispatch function's defn is kept RENAMED to <name>%boxed and
  # immediately followed by its boxed-registration form (must run
  # before any call site can reach the dispatcher; defn placement in
  # the original source is the earliest correct point).
  (def kept @[])
  # Piece 1c bookkeeping: names actually dropped (with their slot in
  # `kept`) and names the residual-reference audit below put back.
  (def dce-dropped @[])
  (def dce-restored @[])
  (each f forms
    (def top-defn?
      (and (tuple? f) (>= (length f) 2)
           (symbol? (get f 0))
           (or (= (string (get f 0)) "defn") (= (string (get f 0)) "defn-"))
           (symbol? (get f 1))))
    (cond
      (and top-defn? (get native-names (get f 1)))
      (do
        (when (tuple? f)
          (def orig (get f 1))
          (if (get dispatch-names orig)
            (do
              (def boxed-sym (symbol (string orig "%boxed")))
              (array/push kept (tuple (get f 0) boxed-sym ;(tuple/slice f 2)))
              (array/push kept
                          (tuple (tuple 'get
                                        (tuple 'get '(curenv)
                                               (tuple 'quote 'dispatch-set-boxed))
                                        ':value)
                                 (tuple 'quote (keyword orig))
                                 boxed-sym)))
            nil)))

      # B7.2 Part B, Piece 1c: the second, independent check. Unlike the
      # native case above there is no replacement of any kind -- the form
      # is dropped outright, no prelude rebind, no %boxed fallback. That
      # asymmetry is deliberate: native stripping removes a body that
      # something still calls (through a C wrapper), whereas a DCE drop
      # removes a body the gate proved nothing can reach at all.
      #
      # Dropped names are recorded rather than discarded: the residual-
      # reference audit below re-examines them against the surviving
      # forms and puts any name back that is still mentioned. A dropped
      # form therefore leaves a PLACEHOLDER in `kept` rather than
      # vanishing, so a restore can put the original defn back at its
      # exact source position. Order is load-bearing: a restored defn is
      # by definition still referenced, and in this one-thunk model it
      # must still precede its users.
      (and top-defn? (get dce-excluded-names (get f 1)))
      # NOTE: the placeholder MUST be a mutable array (@[...]), not a
      # bracket-tuple: parsed source forms are themselves tuples, so a
      # tuple placeholder would be indistinguishable from real code and
      # the audit below would silently treat it as a surviving form.
      (do (array/push dce-dropped @[(get f 1) (length kept)])
        (array/push kept @[:dce-dropped (get f 1) f]))

      (array/push kept f)))

  # --- Residual-reference audit (fail-closed, B7.2 Part B, Piece 1c) ---
  # The whole-program gate in callgraph.janet reasons over BYTECODE. This
  # file edits SOURCE FORMS, and the two views are not interchangeable:
  # this compiler's single-thunk model means a defn that survived into the
  # thunk at all necessarily still has a live symbol reference somewhere in
  # it, and silently deleting that binding yields "unknown symbol" at best
  # and a wrong image at worst. So nothing is trusted on the verdict alone:
  # every dropped name is re-checked against the forms that actually
  # survived, and any name still mentioned is RESTORED (strip nothing for
  # that name) rather than guessed about. Refusing to strip is always a
  # correct outcome here -- the design note's "V1 is allowed to strip
  # nothing" applies per name, not just per build.
  (when (next dce-excluded-names)
    (defn mentions-sym? [form sym]
      (cond
        (symbol? form) (= form sym)
        (or (tuple? form) (array? form))
        (do (var hit false)
          (each sub form (when (mentions-sym? sub sym) (set hit true)))
          hit)
        (or (table? form) (struct? form))
        (do (var hit false)
          (eachp [k v] form
            (when (or (mentions-sym? k sym) (mentions-sym? v sym)) (set hit true)))
          hit)
        false))
    # Fixpoint, not a single pass: restoring one defn makes its OWN body
    # a surviving form, which can in turn reference another dropped name.
    # Iterate until no further restore happens, so the kept set is closed
    # under reference -- a single pass would leave a dangling symbol.
    (var changed true)
    (while changed
      (set changed false)
      (each entry dce-dropped
        (def nm (entry 0))
        (def slot (entry 1))
        (when (and (array? (get kept slot))
                   (= :dce-dropped (get (get kept slot) 0)))
          (var referenced false)
          # Scan only the forms that genuinely survive: placeholders are
          # the dropped bodies themselves and must not count as references
          # to one another (a dead cluster is dropped as a unit, and its
          # internal cross-references are exactly what makes it dead).
          (each kf kept
            (unless (and (array? kf) (= :dce-dropped (get kf 0)))
              (when (mentions-sym? kf nm) (set referenced true))))
          (when referenced
            (eprint "mkimage: dce: refusing to strip " nm
                    " -- still referenced by a surviving form (keeping it)")
            (array/push dce-restored nm)
            # restore in place, preserving original source order
            (put kept slot ((get kept slot) 2))
            (set changed true))))))

  # Collapse: placeholders that were never restored disappear for real.
  (def kept-final @[])
  (each kf kept
    (unless (and (array? kf) (= :dce-dropped (get kf 0)))
      (array/push kept-final kf)))
  (when (next dce-excluded-names)
    (eprint "mkimage: dce: stripped " (- (length dce-dropped) (length dce-restored))
            " of " (length dce-dropped) " candidate defn forms"
            (if (empty? dce-restored) ""
              (string " (restored: " (string/join (map string dce-restored) " ") ")"))))

  (set forms kept-final)
  # prelude: rebind each native name from the env (wrapper cfunctions
  # registered by native_init before the thunk runs). NOTE: unwrap with
  # (get ... :value), NEVER (:value ...) -- the latter is method-invoke
  # and would CALL the wrapper with garbage instead of resolving it.
  (each nm (keys native-names)
    (array/insert forms 0
                (tuple 'def nm
                       (tuple 'get (tuple 'get '(curenv) (tuple 'quote nm))
                              ':value)))))

# AOT images bake modules in at build time: the preimport pass above
# already loaded every top-level import and merged its bindings, so the
# import/require/use forms themselves are stripped from the emitted thunk
# (at runtime there is no :current-file for :cur: resolution, and the
# module top-levels must not run twice). Runtime requires inside function
# bodies are left alone.
(do (def kept2 @[])
  (each f forms
    (if (and (tuple? f) (>= (length f) 1)
             (symbol? (get f 0))
             (or (= (get f 0) (quote import))
                 (= (get f 0) (quote require))
                 (= (get f 0) (quote use))))
      nil
      (array/push kept2 f)))
  (set forms kept2))

# run-main parity: if the script defines a top-level main, append an
# explicit export of it onto the fiber env — AOT chunks are one do-form,
# so main is a mere local and nothing else can reach it from C (probe:
# bare `main` resolves, (curenv) is nil, no funcenv, no table constant).
(def has-main
  (truthy?
    (find (fn [f] (and (tuple? f) (>= (length f) 2) (symbol? (get f 0))
                       (symbol? (get f 1))
                       (= (get f 1) 'main)
                       (has-value? [:def :defn :var :varfn] (keyword (get f 0)))))
          forms)))
(def forms2
  (if has-main (array/push (array ;forms) '(setdyn :aot-main main)) forms))

# One thunk for the whole file: correct cross-form refs, zero build-time run.
#
# (2026-10: a per-form-chunks variant was tried and reverted — see
# docs/internals/proc-lanes.md round 21. Top-level defs write into the
# COMPILE-TIME env table (a bytecode constant), and mkimage never runs
# forms at build time, so a later chunk folds an earlier def to its
# build-time value (nil). The interpreter only works per-form because
# run-context compiles each form AFTER the previous one ran. The one
# do-form keeps cross-form refs as frame locals — marshal-safe.)
(def program (tuple/slice @['do ;forms2] 0))
(def thunk
  (try (compile program env srcpath)
    ([err] (eprint srcpath ": compile error: " err) (os/exit 1))))
(when (not= (type thunk) :function)
  (eprint srcpath ": compile error: " (get thunk :error))
  (os/exit 1))

(def rreg @{})
(eachp [k v] snap (put rreg v k))
(def image
  (try (marshal thunk rreg)
    ([err] (eprint srcpath ": marshal error: " err
                   " (unmarshalable value reached top level?)")
           (os/exit 1))))
(def regnames (distinct (values rreg)))

# Emit C: byte array + registry names + runtime main.
(def out @"")
(buffer/push-string out "/* AOT image generated by tools/mkimage.janet — do not edit. */\n")
(buffer/push-string out (string "/* source: " srcpath
                              ", image bytes: " (length image)
                              ", registry names: " (length regnames) " */\n"))
(buffer/push-string out "#include <janet.h>\n#include <stdio.h>\n#include <stdlib.h>\n#include <string.h>\n")
(buffer/push-string out "void janet_profile_start(void);\nvoid janet_profile_auto_output(const char *path);\n")
(buffer/push-string out "#ifndef JANET_NO_PGO\n")
(buffer/push-string out
  ```
  #if defined(_WIN32)
  #include <direct.h>
  #include <io.h>
  #include <fcntl.h>
  #define getpid _getpid
  #define open _open
  #define close _close
  #define O_WRONLY _O_WRONLY
  #define O_CREAT _O_CREAT
  #define O_EXCL _O_EXCL
  #define JANET_AOT_MKDIR(d) _mkdir(d)
  #define JANET_AOT_PROBE(p) (_access(p, 0) == 0)
  #else
  #include <sys/stat.h>
  #include <sys/types.h>
  #include <unistd.h>
  #include <fcntl.h>
  #define JANET_AOT_MKDIR(d) mkdir(d, 0755)
  #define JANET_AOT_PROBE(p) (access(p, F_OK) == 0)
  #endif
  static void janet_aot_mkdirp(char *d) {
      /* best-effort recursive mkdir; failures are non-fatal */
      char *p = d;
      if (*p == '\0') return;
      for (; *p != '\0'; p++) {
          if (*p == '/' && p != d) {
              char save = *p;
              *p = '\0';
              JANET_AOT_MKDIR(d);
              *p = save;
          }
      }
      JANET_AOT_MKDIR(d);
  }
  static void janet_aot_key(char *base, size_t cap, const char *exe) {
      /* cache key from the binary basename, sanitized; never empty */
      const char *bn = (exe && exe[0] != '\0') ? exe : "janet-app";
      const char *sl = strrchr(bn, '/');
      size_t bi = 0, i;
      const char *bs = strrchr(bn, '\\');
      if (bs && (!sl || bs > sl)) sl = bs;
      bn = sl ? sl + 1 : bn;
      if (bn[0] == '\0') bn = "janet-app";
      for (i = 0; bn[i] != '\0' && bi + 1 < cap; i++) {
          char ch = bn[i];
          base[bi++] = ((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') ||
                         (ch >= '0' && ch <= '9') || ch == '_' || ch == '-' || ch == '.') ? ch : '_';
      }
      base[bi] = '\0';
      if (bi == 0) {
          const char *fb = "janet-app";
          size_t fi = 0;
          while (fb[fi] != '\0' && fi + 1 < cap) { base[fi] = fb[fi]; fi++; }
          base[fi] = '\0';
      }
  }
  ```)
(buffer/push-string out "\n#endif /* JANET_NO_PGO (auto-profile helpers) */\n")
(buffer/push-string out "static const unsigned char janet_aot_image[] = {")
(each b image
  (buffer/push-string out (string/format "0x%02x," b)))
(buffer/push-string out "};\n")
(buffer/push-string out "static const unsigned long janet_aot_image_len = sizeof(janet_aot_image);\n\n")
(buffer/push-string out "static const char *janet_aot_names[] = {\n")
(each nm regnames
  (when (and (not (string/find "\"" nm)) (not (string/find "\\" nm)))
    (buffer/push-string out (string "  \"" nm "\",\n"))))
(buffer/push-string out "};\n")
(buffer/push-string out (string "static const unsigned long janet_aot_names_len = "
                              "sizeof(janet_aot_names) / sizeof(janet_aot_names[0]);\n\n"))
(buffer/push-string out
  ```
  int main(int argc, char **argv) {
      janet_init();
#ifndef JANET_NO_PGO
      /* The VM owns its auto-PGO output across both normal and os/exit
       * teardown. No launcher-owned process environment is needed. */
      /* Auto-PGO collects the first qualifying run to the base cache.
       * JANET_AUTO_PGO=1 records further runs in unique sidecars,
       * merged fail-closed at the next native build. Never overwrite
       * existing evidence. JANET_NO_PROFILE disables collection;
       * failures here silently run clean.
       * Locked-down targets (iOS: no JIT/dynamic codegen) compile
       * the whole mechanism out with -DJANET_NO_PGO. */
      {
          const char *noprof = getenv("JANET_NO_PROFILE");
          const char *force = getenv("JANET_AUTO_PGO");
          const char *xdg = getenv("XDG_CACHE_HOME");
          const char *home = getenv("HOME");
          char base[64];
          char dir[4096];
          char path[4096];
          dir[0] = '\0';
          if (!noprof) {
              if (xdg && xdg[0] != '\0')
                  snprintf(dir, sizeof(dir), "%s/janet/pgo", xdg);
              else if (home && home[0] != '\0')
                  snprintf(dir, sizeof(dir), "%s/.cache/janet/pgo", home);
          }
          path[0] = '\0';
          if (dir[0] != '\0') {
              janet_aot_key(base, sizeof(base), (argc > 0) ? argv[0] : NULL);
              snprintf(path, sizeof(path), "%s/%s.jdn", dir, base);
          }
          if (path[0] != '\0' && (force || !JANET_AOT_PROBE(path))) {
              janet_aot_mkdirp(dir);
              { /* Reserve a private destination for every collector. */
                   char sidecar[4096];
                   unsigned attempt;
                   for (attempt = 0; attempt < 64; attempt++) {
                       int fd;
                       snprintf(sidecar, sizeof(sidecar), "%s.%s-%ld-%u.pending",
                                path, JANET_AOT_PROBE(path) ? "run" : "first",
                                 (long) getpid(), attempt);
                       fd = open(sidecar, O_WRONLY | O_CREAT | O_EXCL, 0600);
                       if (fd < 0) continue;
                       close(fd);
                       snprintf(path, sizeof(path), "%s", sidecar);
                       break;
                   }
                   if (attempt == 64) path[0] = 0;
               }
               if (path[0] != 0) {
                   janet_profile_start();
                   janet_profile_auto_output(path);
               }
          }
      }
#endif
      JanetTable *env = janet_core_env(NULL);
      janet_table_put(env, janet_ckeywordv("executable"), janet_cstringv(argv[0]));
#ifdef JANET_AOT_NATIVE_INIT
      /* hybrid binary: register native kernels before any janet code
       * runs (boxed or eval'd call sites resolve through env) */
      extern void native_init(JanetTable *env);
      native_init(env);
#endif
      /* CLI parity: install (dyn :args) as [program, ...argv], the way
       * run.c exposes script arguments (mkimage note: :args[0] is the
       * script path). */
      JanetArray *cliargs = janet_array(argc > 0 ? argc : 1);
      for (int ai = 0; ai < argc; ai++)
          janet_array_push(cliargs, janet_cstringv(argv[ai]));
      janet_table_put(env, janet_ckeywordv("args"), janet_wrap_array(cliargs));
      /* CLI parity: -e CODE, the way the interpreter binary handles it.
      ** The interpreter's os/spawn falls back to argv[0] for bare
      ** program names, so subprocess tests like suite-ev's
      ** [janet -e "(print ...)"] exec THIS binary — which must eval
      ** the code, not re-run the embedded image. */
      for (int ai = 1; ai < argc; ai++) {
          if (argv[ai][0] == '-' && argv[ai][1] == 'e' &&
                  argv[ai][2] == '\0') {
              const char *code = (ai + 1 < argc) ? argv[ai + 1] : "";
              Janet out;
              int esig = janet_dostring(env, code, "-e", &out);
              if (esig != JANET_SIGNAL_OK && esig != JANET_SIGNAL_EVENT) {
                  janet_eprintf("%v\n", out);
                  janet_deinit();
                  return 1;
              }
              janet_deinit();
              return 0;
          }
      }
      JanetTable *reg = janet_table((int32_t) janet_aot_names_len);
      for (unsigned long i = 0; i < janet_aot_names_len; i++) {
          Janet v = janet_resolve_core(janet_aot_names[i]);
          if (!janet_checktype(v, JANET_NIL))
              janet_table_put(reg, janet_csymbolv(janet_aot_names[i]), v);
      }
      janet_gcroot(janet_wrap_table(reg));
      /* Chunk env: one fresh table proto-linked to the module env.
      ** run-context gives script fibers an env whose proto is the
      ** module env; a bare core env has no proto, so scripts doing
      ** (table/getproto (curenv)) saw nil and broke (the shared-store
      ** escape test). Shared by all chunks so cross-form setdyn sees
      ** earlier forms, exactly like the interpreter's script runs. */
      JanetTable *chunk_env = janet_table(0);
      chunk_env->proto = env;
      janet_gcroot(janet_wrap_table(chunk_env));
      Janet top = janet_unmarshal(janet_aot_image, janet_aot_image_len, 0, reg, NULL);
      if (janet_checktype(top, JANET_NIL)) {
          fprintf(stderr, "aot: unmarshal failed\n");
          janet_deinit();
          return 2;
      }
      janet_gcroot(top);
      /* Per-form chunks: the image is a tuple of functions (one per
      ** top-level form), run in order — the way run.c executes a
      ** script form by form. A bare function means a legacy
      ** one-chunk image. Mirror run.c janet_dobytes per chunk: run
      ** to completion, then enter the event loop for ev programs
      ** (janet_loop_fiber as in shell.c: quantum-INTERRUPT resumes
      ** need the scheduler to requeue them, and ev programs pump in
      ** the same loop). An error aborts the script with status 1. */
      int status = 0;
      int chunks_ok = 1;
      if (janet_checktype(top, JANET_TUPLE)) {
          /* Chunks run from a rooted ARRAY with each slot cleared as
           * its chunk is taken (and again after it completes): a
           * finished chunk's function — and the constants its def
           * references — must become collectable, the way an
           * interpreter script's form functions die after their form.
           * Rooting all chunk functions for the whole run pinned def
           * constants forever; weak-table tests saw entries that
           * should have cleared post-gccollect. */
          JanetTuple chunks = janet_unwrap_tuple(top);
          JanetArray *chunk_arr = janet_array(janet_tuple_length(chunks));
          for (int32_t ci = 0; ci < janet_tuple_length(chunks); ci++)
              janet_array_push(chunk_arr, chunks[ci]);
          janet_gcunroot(top);
          janet_gcroot(janet_wrap_array(chunk_arr));
          /* ONE fiber for all chunks, re-primed per chunk — run-context
           * reuses a single evaluation context for a script's forms;
           * fresh-fiber-per-chunk left every completed chunk's STACK
           * live in retained dead fibers, pinning frame values so weak
           * tables never cleared their entries (K3 in the bisection).
           * janet_fiber_reset re-primes in place; later chunks
           * overwrite earlier stacks. */
          JanetFunction *first_fn =
              janet_unwrap_function(chunk_arr->data[0]);
          JanetFiber *fiber = janet_fiber(first_fn, 64, 0, NULL);
          fiber->env = chunk_env;
          janet_gcroot(janet_wrap_fiber(fiber));
          for (int32_t ci = 0; ci < chunk_arr->count; ci++) {
              if (!janet_checktype(chunk_arr->data[ci], JANET_FUNCTION)) {
                  chunk_arr->data[ci] = janet_wrap_nil();
                  continue;
              }
              JanetFunction *fn =
                  janet_unwrap_function(chunk_arr->data[ci]);
              chunk_arr->data[ci] = janet_wrap_nil();
              if (ci > 0) {
                  janet_fiber_reset(fiber, fn, 0, NULL);
                  fiber->env = chunk_env;
                  /* Zero the reused stack: an open funcenv marshal
                   * (ev/do-thread thunks) reads ALL length slots of
                   * the enclosing frame, including slots this chunk
                   * has not written yet — without the zero they hold
                   * stale values from PREVIOUS chunks (an os/process
                   * from an earlier test made the marshal refuse).
                   * Nil-filled slots are safe to walk. */
                  memset(fiber->data, 0,
                         sizeof(Janet) * (size_t) fiber->capacity);
              }
              int fstatus = janet_loop_fiber(fiber);
              if (fstatus != JANET_SIGNAL_OK && fstatus != JANET_SIGNAL_EVENT) {
                  janet_stacktrace_ext(fiber, fiber->last_value, "");
                  status = 1;
                  chunks_ok = 0;
                  break;
              }
          }
          janet_gcunroot(janet_wrap_fiber(fiber));
          janet_gcunroot(janet_wrap_array(chunk_arr));
      } else if (janet_checktype(top, JANET_FUNCTION)) {
          JanetFunction *fn = janet_unwrap_function(top);
          JanetFiber *fiber = janet_fiber(fn, 64, 0, NULL);
          fiber->env = chunk_env;
          janet_gcroot(janet_wrap_fiber(fiber));
          int fstatus = janet_loop_fiber(fiber);
          if (fstatus != JANET_SIGNAL_OK && fstatus != JANET_SIGNAL_EVENT) {
              janet_stacktrace_ext(fiber, fiber->last_value, "");
              status = 1;
          }
          janet_gcunroot(janet_wrap_fiber(fiber));
      }
      /* run-main parity: the interpreter's run-context calls
      ** (main ;subargs) after the script chunk completes (boot.janet
      ** run-main). AOT binaries skipped that step entirely — scripts
      ** that only define main ran as silent empty binaries. Mirror
      ** it: look up main in the script env and run it with argv[1..]
      ** (everything after the program name), same fiber-driving and
      ** ev-loop rules as the chunk. */
      if (chunks_ok) {
          /* mkimage appends (setdyn :aot-main main) when the script
          ** defines a top-level main — main is a mere local of its
          ** chunk, unreachable from C by name. setdyn lands in the
          ** chunk env (the current fiber env), so read it there and
          ** run it, exactly what run-main would have used. */
          Janet mainv = janet_table_get(chunk_env, janet_ckeywordv("aot-main"));
          JanetTable *script_env = chunk_env;
          if (janet_checktype(mainv, JANET_FUNCTION)) {
              JanetArray *ma = janet_array(argc > 1 ? argc - 1 : 0);
              for (int ai = 1; ai < argc; ai++)
                  janet_array_push(ma, janet_cstringv(argv[ai]));
              JanetFiber *mfb = janet_fiber(janet_unwrap_function(mainv),
                                            64, ma->count, ma->data);
              mfb->env = script_env;
              janet_gcroot(janet_wrap_fiber(mfb));
              int mstatus = janet_loop_fiber(mfb);
              if (mstatus != JANET_SIGNAL_OK && mstatus != JANET_SIGNAL_EVENT) {
                  janet_stacktrace_ext(mfb, mfb->last_value, "");
                  status = 1;
              }
              janet_gcunroot(janet_wrap_fiber(mfb));
          }
      }
      janet_gcunroot(janet_wrap_table(chunk_env));
      if (janet_checktype(top, JANET_TUPLE)) {
          /* already unrooted when the chunks moved into the array */
      } else {
          janet_gcunroot(top);
      }
      janet_deinit();
      return status;
  }
  ```
  )
(spit outpath out)
(print "mkimage: " (length image) " bytes, "
       (length regnames) " registry names -> " outpath)
