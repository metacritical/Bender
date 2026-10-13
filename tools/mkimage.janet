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

# :args[0] is this script's path; SRC OUT [--native names.txt]
(def rawargs (tuple/slice (dyn :args) 1))
(var args @[])
(var native-names @{})
(var native-names-file nil)
(var nai 0)
(while (< nai (length rawargs))
  (def a (rawargs nai))
  (if (and (= a "--native") (< (+ nai 1) (length rawargs)))
    (do (set native-names-file (rawargs (+ nai 1)))
        (++ nai))
    (array/push args a))
  (++ nai))
(when (< (length args) 2)
  (eprint "usage: mkimage.janet SRC.janet OUT.c [--native names.txt]")
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

(when (next native-names)
  # strip proven defn forms; keep everything else
  (def kept @[])
  (each f forms
    (if (and (tuple? f) (>= (length f) 2)
             (symbol? (get f 0))
             (or (= (string (get f 0)) "defn") (= (string (get f 0)) "defn-"))
             (symbol? (get f 1))
             (get native-names (get f 1)))
      nil
      (array/push kept f)))
  (set forms kept)
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
(buffer/push-string out "void janet_profile_start(void);\n")
(buffer/push-string out "#ifndef JANET_NO_PGO\n")
(buffer/push-string out
  ```
  #if defined(_WIN32)
  #include <direct.h>
  #include <io.h>
  #define JANET_AOT_MKDIR(d) _mkdir(d)
  #define JANET_AOT_PROBE(p) (_access(p, 0) == 0)
  #else
  #include <sys/stat.h>
  #include <sys/types.h>
  #include <unistd.h>
  #define JANET_AOT_MKDIR(d) mkdir(d, 0755)
  #define JANET_AOT_PROBE(p) (access(p, F_OK) == 0)
  #endif
  static void janet_aot_setenv(const char *k, const char *v) {
      /* set-if-unset (explicit user env always wins) */
      if (getenv(k)) return;
  #if defined(_WIN32)
      { char buf[8192]; snprintf(buf, sizeof(buf), "%s=%s", k, v); _putenv(buf); }
  #else
      setenv(k, v, 0);
  #endif
  }
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
      /* Auto-PGO, first runs only (on demand, never always-on): when
       * no cached profile exists for this binary yet — or
       * JANET_AUTO_PGO=1 forces it — collect this run into the
       * standard cache. The next plain `bender --aot --native` build
       * widens from it and recompiles automatically; the binary after
       * that runs clean. Never when JANET_NO_PROFILE is set. Never
       * breaks the run: every failure here silently runs clean.
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
              janet_profile_start();
              janet_aot_setenv("JANET_PROFILE_OUT", path);
              janet_aot_setenv("JANET_AUTO_PGO_ACTIVE", "1");
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
          JanetTuple chunks = janet_unwrap_tuple(top);
          for (int32_t ci = 0; ci < janet_tuple_length(chunks); ci++) {
              if (!janet_checktype(chunks[ci], JANET_FUNCTION)) continue;
              JanetFunction *fn = janet_unwrap_function(chunks[ci]);
              JanetFiber *fiber = janet_fiber(fn, 64, 0, NULL);
              fiber->env = chunk_env;
              janet_gcroot(janet_wrap_fiber(fiber));
              int fstatus = janet_loop_fiber(fiber);
              if (fstatus != JANET_SIGNAL_OK && fstatus != JANET_SIGNAL_EVENT) {
                  janet_stacktrace_ext(fiber, fiber->last_value, "");
                  status = 1;
                  janet_gcunroot(janet_wrap_fiber(fiber));
                  chunks_ok = 0;
                  break;
              }
              janet_gcunroot(janet_wrap_fiber(fiber));
          }
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
      janet_gcunroot(top);
      janet_deinit();
      return status;
  }
  ```
  )
(spit outpath out)
(print "mkimage: " (length image) " bytes, "
       (length regnames) " registry names -> " outpath)
