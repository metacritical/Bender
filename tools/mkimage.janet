# tools/mkimage.janet — AOT spike step 1 (see docs/internals/aot-plan.md Phase 0).
#
# Compiles SRC.janet ahead of time to OUT.c, embedding a marshalled top-level
# thunk as a static byte array plus a main() that runs it. All values stay
# boxed; JOP_* ops still execute in the bytecode VM. Run under host janet
# (macros expand at build time, as today):
#
#   ./build/janet tools/mkimage.janet app.janet app.c
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
(var native-names @{})
(var native-names-file nil)
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
  # registered by native_init before the thunk runs)
  (each nm (keys native-names)
    (array/push forms 0
                (tuple 'def nm
                       (tuple ':value
                              (tuple 'get '(curenv) (tuple 'quote nm)))))))

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

# One thunk for the whole file: correct cross-form refs, zero build-time run.
(def program (tuple/slice @['do ;forms] 0))
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
(buffer/push-string out "#include <janet.h>\n#include <stdio.h>\n\n")
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
      Janet top = janet_unmarshal(janet_aot_image, janet_aot_image_len, 0, reg, NULL);
      if (janet_checktype(top, JANET_NIL)) {
          fprintf(stderr, "aot: unmarshal failed\n");
          janet_deinit();
          return 2;
      }
      janet_gcroot(top);
      JanetFunction *fn = janet_unwrap_function(top);
      /* Mirror run.c janet_dobytes: synchronous continue first, so error
      ** traces render exactly like the interpreter's; enter the event loop
      ** afterwards for ev programs. */
      JanetFiber *fiber = janet_fiber(fn, 64, 0, NULL);
      fiber->env = env;
      Janet out = janet_wrap_nil();
      JanetSignal sig = janet_continue(fiber, janet_wrap_nil(), &out);
      int status = 0;
      if (sig != JANET_SIGNAL_OK && sig != JANET_SIGNAL_EVENT) {
          janet_stacktrace_ext(fiber, out, "");
          status = 1;
      } else {
          janet_gcroot(janet_wrap_fiber(fiber));
          janet_loop();
          janet_gcunroot(janet_wrap_fiber(fiber));
      }
      janet_gcunroot(top);
      janet_deinit();
      return status;
  }
  ```
  )
(spit outpath out)
(print "mkimage: " (length image) " bytes, "
       (length regnames) " registry names -> " outpath)
