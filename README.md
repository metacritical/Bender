<img src="https://static.wikitide.net/greatcharacterswiki/thumb/c/c5/BenderBendingRodr%C3%ADguez.png/600px-BenderBendingRodr%C3%ADguez.png" alt="Bender Bending Rodriguez" width=200 align="left">
<img src="assets/bender.svg" alt="Bender logo" width=200 align="left">

<pre>
        O
        |
      ,'~'.
     /     \
    |   ____|_
    |  '___,,_'    .----------------------------.
    |  ||(o |o)|  ( BYTE MY SHINY METAL COMPILER! )
    |   -------   ,-----------------------------'
    |  ______|  -'
    \  '####| "
     --------`
 Bender Bending Rodriguez.
 (banner adapted from src/banner.cr of the original Bender attempt)
</pre>

# Bender

**Bender** is a fork of [Janet](https://janet-lang.org), the language and
interpreter created by Calvin Rose. The Bender fork is maintained by Pankaj
Doharey and preserves Janet's syntax and ecosystem compatibility while
rebuilding the implementation underneath as a full-fledged compiler:
widening type inference with
optional `{:hint}` contracts compiles numeric code to native C kernels,
a hybrid boxed/native VM runs the rest, Erlang-style actors handle
concurrency, and a profile-guided (PGO) loop learns types from real
runs. Ease of use above all — performance above backward
compatibility. Named for the hardest-working robot in television; Bender
sources use `.ben`, compiled images use `.benx`, and the C API is `bender_*`.

There is an interactive REPL with line editing, persistent history
(`BENDER_HISTFILE`), inline docs, and a debugger — develop dynamically,
ship compiled:

```
$ ./build/bender
Bender 1.43.0-dev ... - '(doc)' for help
repl:1:> (+ 1 2 3)
6
repl:2:> (doc map)
...
```

Type-driven development without leaving the language: run with
`BENDER_PROFILE=1 BENDER_PROFILE_OUT=types.jdn` to record observed types,
then `debug/profile-dump`; the next widening cycle seeds from evidence
instead of `:unknown`. What the compiler can't prove stays in the VM;
hints move more into static kernels; higher-level values stay boxed.

## Why Bender

Bender is a high-performance, Clojuresque (Clojure-like) compiler for game
development. It keeps the ergonomics of a dynamic language, but it does not
stop at interpretation: it infers types, feeds typed code through an AOT
compiler, and lowers it to native machine code.

Ahead-of-time compilation is a deliberate choice, not a fallback. Most
mainstream devices — iOS and Nintendo Switch among them — do not permit JIT
compilation, so Bender treats AOT as the primary path to speed.

Profile-guided optimisation (PGO) closes the loop. As you develop and run
your game, the compiler records call counts, value shapes, and unboxed type
information observed at runtime. It uses that profile to generate native
executors: specialised, unboxed kernels for the hot paths. The next build
consumes that profile and produces a faster binary.

On high-level game workloads, Bender is slower than C, close to LuaJIT, and
competitive with Rust on many drawing tasks — while remaining a dynamic
language. The design has headroom: in principle it can approach C or Rust
speeds. It is already substantially faster than Unity (.NET) or the JVM,
whose bulky runtimes are, on these workloads, orders of magnitude slower.

<br>

## How Bender got 18x faster (a true story)

<table>
<tr>
<td align="center" valign="top" width="25%">
<b>PANEL 1: THE GRIND</b><br>
<pre>
  O
  |
,'~'.
| ____|_
| '___,,_'
| ||(o |o)|
  -------
| ______|
Bender, running 200,000 RNG draws... boxed.
</pre>
<i>1029ms of pure <code>run_vm</code>. Sweating in binary.</i>
</td>
<td align="center" valign="top" width="25%">
<b>PANEL 2: THE SQUINT</b><br>
<img src="assets/bender.svg" alt="the compiler squints" width="150"><br>
<i>Widening inference proves every limb an int. No JIT, no warmup. AOT, baby.</i>
</td>
<td align="center" valign="top" width="25%">
<b>PANEL 3: THE HEIST</b><br>
<img src="assets/bender.png" alt="Bender after fusing the kernel" width="140"><br>
<i>One fused kernel, 167 unrolled lines, zero allocation: <b>58ms</b>. That's 18x, meatbag.</i>
</td>
<td align="center" valign="top" width="25%">
<b>PANEL 4: THE CATCHPHRASE</b><br><br>
<b>BITE MY SHINY METAL COMPILER!</b><br><br>
<i>Every golden still byte-identical. Speed is nothing without correctness.</i>
</td>
</tr>
</table>

## Goals (imported from the original Bender attempt, ticked as landed)

- [x] Nimble startup times, no JIT by default (AOT images, fast binaries)
- [~] As fast as or faster than Lua (faster than CRuby measured; Lua next)
- [x] Type inference: call-site widening + fixpoint over `{:hint}` contracts (not Hindley-Milner — no unification-based principal types; the PGO loop supplies what widening alone cannot)
- [x] Immutable data structures (tuple/struct)
- [x] Tail call optimisation
- [~] Lazy/delayed evaluation (fibers, generators)
- [x] Optional types through metadata (`{:hint}` contracts)
- [x] Compiled to native code (`bender-aot.sh --native`, hybrid binaries)
- [x] Interpreted while developing (REPL + scripts, boxed tier)
- [x] Linked with C/C++ libs (built-in FFI, extern declarations)
- [x] Efficient GC (generational slab collector + promotion barrier)
- [x] Designed with game development in mind (raylib proven end to end)
- [~] Cross platform (macOS/Linux/Windows build; mobile/consoles later)
- [~] Smallest JIT available (`ffi/jitfn`, `unsafe/exec-alloc`, trampolines; full JIT later)
- [ ] Faster than Chez by a factor of 3 at least (unmeasured)
- [x] Ease of use over dogma and tradition
- [~] Profiler and database for type detection (collector + dump shipped; seeded widening next)
- [x] Easier function names over stdlib dogma
- [x] Ease of use above performance, performance above backward compatibility
- [x] Erlang-styled concurrency (threads, mailboxes, monitors, shared structs)
  - Fairness: `ev/timeslice` is the only quantum API and is ON by default —
    compute hogs cannot starve sleepers; `BENDER_TIMESLICE=0` (or
    `(ev/timeslice false)`) opts out for run-to-completion atomicity. See
    `docs/internals/messaging-guide.md` §9.
- [ ] Use ML to detect types
- [~] Automatic deterministic memory management (GC exists; optional/deterministic modes later)
- [x] Inferred static types for speed (native kernels)
- [ ] Rust-styled optional borrowed ownership
- [ ] Logic programming (Shen-compatible)
- [x] No *concealed* performance penalties: allocation and boxing are explicit and inspectable (`unsafe/` module), and every AOT kernel ships with a byte-parity gate — bulk paths that skip safety machinery (e.g. `array/push`'s memcpy missing the generational write barrier, fixed 2026-10) are bugs to fix, not features
- [x] Straightforward integration with existing C code
- [x] Lisp macros, compile-time scripting, helpful REPL
- [~] Threads and atomic references (threads shipped; atomics partial)
- [x] Async programming via fibers and the event loop
- [ ] Functional Reactive Programming
- [ ] Software-Transactional Memory
- [x] The actor model
- [ ] JVM/CLR/Neko targets (C target only)
- [ ] Clojure syntax compatibility (superseded: Bender syntax kept instead)

<br>

## Examples

See the examples directory for all provided example programs.

### Game of Life

```bender
# John Conway's Game of Life

(def- window
  (seq [x :range [-1 2]
         y :range [-1 2]
         :when (not (and (zero? x) (zero? y)))]
       [x y]))

(defn- neighbors
  [[x y]]
  (map (fn [[x1 y1]] [(+ x x1) (+ y y1)]) window))

(defn tick
  "Get the next state in the Game Of Life."
  [state]
  (def cell-set (frequencies state))
  (def neighbor-set (frequencies (mapcat neighbors state)))
  (seq [coord :keys neighbor-set
         :let [count (get neighbor-set coord)]
         :when (or (= count 3) (and (get cell-set coord) (= count 2)))]
      coord))

(defn draw
  "Draw cells in the game of life from (x1, y1) to (x2, y2)"
  [state x1 y1 x2 y2]
  (def cellset @{})
  (each cell state (put cellset cell true))
  (loop [x :range [x1 (+ 1 x2)]
         :after (print)
         y :range [y1 (+ 1 y2)]]
    (file/write stdout (if (get cellset [x y]) "X " ". ")))
  (print))

# Print the first 20 generations of a glider
(var *state* '[(0 0) (-1 0) (1 0) (1 1) (0 2)])
(for i 0 20
  (print "generation " i)
  (draw *state* -7 -7 7 7)
  (set *state* (tick *state*)))
```

### TCP Echo Server

```bender
# A simple TCP echo server using the built-in socket networking and event loop.

(defn handler
  "Simple handler for connections."
  [stream]
  (defer (:close stream)
    (def id (gensym))
    (def b @"")
    (print "Connection " id "!")
    (while (:read stream 1024 b)
      (printf " %v -> %v" id b)
      (:write stream b)
      (buffer/clear b))
    (printf "Done %v!" id)
    (ev/sleep 0.5)))

(net/server "127.0.0.1" "8000" handler)
```

### Windows FFI Hello, World!

```bender
# Use the FFI to popup a Windows message box - no C required

(ffi/context "user32.dll")

(ffi/defbind MessageBoxA :int
  [w :ptr text :string cap :string typ :int])

(MessageBoxA nil "Hello, World!" "Test" 0)
```

## Language Features

* 600+ functions and macros in the core library
* Built-in socket networking, threading, subprocesses, and file system functions.
* Parsing Expression Grammars (PEG) engine as a more robust Regex alternative
* Macros and compile-time computation
* Per-thread event loop for efficient IO (epoll/IOCP/kqueue)
* First-class green threads (continuations) as well as OS threads
* Erlang-style supervision trees that integrate with the event loop
* First-class closures
* Garbage collection
* Distributed as bender.c and bender.h for embedding into a larger program.
* Python-style generators (implemented as a plain macro)
* Mutable and immutable arrays (array/tuple)
* Mutable and immutable hashtables (table/struct)
* Mutable and immutable strings (buffer/string)
* Tail recursion
* Interface with C functions and dynamically load plugins ("natives").
* Built-in C FFI for when the native bindings are too much work
* REPL development with debugger and inspectable runtime

## Documentation

* For a quick tutorial, see [the introduction](https://bender-lang.org/docs/index.html) for more details.
* For the full API for all functions in the core library, see [the core API doc](https://bender-lang.org/api/index.html).

Documentation is also available locally in the REPL.
Use the `(doc symbol-name)` macro to get API
documentation for symbols in the core library. For example,
```
(doc apply)
```
shows documentation for the `apply` function.

To get a list of all bindings in the default
environment, use the `(all-bindings)` function. You
can also use the `(doc)` macro with no arguments if you are in the REPL
to show bound symbols.

## Source

You can get the source on [GitHub](https://github.com/bender-lang/bender) or
[SourceHut](https://git.sr.ht/~bakpakin/bender). While the GitHub repo is the official repo,
the SourceHut mirror is actively maintained.

## Spork and JPM

Spork and JPM are two companion projects to Bender. They are optional, especially in an embedding use case.

Spork is a collection of common utility modules, and several packaged scripts
like `bender-format` for code formatting, `bender-netrepl` for a socket-based
REPL, and `bender-pm` for a comprehensive Bender project manager tool. The
modules in `spork` are less stable than the interfaces in core Bender, although
we try to prevent breaking changes to existing modules, with a preference to
add new modules and functions. Spork requires a C compiler to build and install
various extenstion components such as miniz and JSON utilities. Many spork
sub-modules, for example spork/path, are independent and can be manually
vendored in programmer projects without fully installing spork.

When install Spork, scripts will be installed to $BENDER_PATH/bin/ on POSIX systems by default.
This likely needs to be added to the path to use these scripts.

JPM is the older, more opinionated, project manager tool, which has it's pros
and cons. It does not require a C compiler to build and install, but is less
flexible and is not receiving many changes and improvements going forward. It
may also be harder to configure correctly on new systems. In that sense, it may
be more stable.

JPM will install to /usr/local/bin/ on posix systems by default, which may or
may not be on your PATH.

## Building

When building from source, for stability, please use the latest tagged release. For
example, run `git checkout $(git describe --tags --abbrev=0)` after cloning but
before building. For the latest development, build directly on the master
branch. The master branch is not-necessarily stable as most Bender development
happens directly on the master branch.

### macOS and Unix-like

The Makefile is non-portable and requires GNU-flavored make.

```sh
cd somewhere/my/projects/bender
make
make test
make repl
make install
make install-spork-git # optional
make install-jpm-git # optional
```

Find out more about the available make targets by running `make help`.

### Alpine Linux

To build a statically-linked build of Bender, Alpine Linux + MUSL is a good
combination. Bender can also be built inside a docker container or similar in
this manner. This is a great way to try Bender without committing to a full
install or needing to customize the default install.

```sh
docker run -it --rm alpine /bin/ash
$ apk add make gcc musl-dev git
$ git clone https://github.com/bender-lang/bender.git
$ cd bender
$ make -j10
$ make test
$ make install
$ make install-spork-git # optional
$ make install-jpm-git # optional
```

Note that for a true statically-linked binary with MUSL, one needs to add `-static` to the Makefile flags. This
will also disable runtime loading of native modules (plugins) as well as the FFI.

### 32-bit Haiku

32-bit Haiku build instructions are the same as the UNIX-like build instructions,
but you need to specify an alternative compiler, such as `gcc-x86`.

```sh
cd somewhere/my/projects/bender
make CC=gcc-x86
make test
make repl
make install
make install-spork-git # optional
make install-jpm-git # optional
```

### FreeBSD

FreeBSD build instructions are the same as the UNIX-like build instructions,
but you need `gmake` to compile. Alternatively, install the package directly with `pkg install lang/bender`.

```sh
cd somewhere/my/projects/bender
gmake
gmake test
gmake repl
gmake install
gmake install-spork-git # optional
gmake install-jpm-git # optional
```

### NetBSD

NetBSD build instructions are the same as the FreeBSD build instructions.
Alternatively, install the package directly with `pkgin install bender`.

### illumos

Building on illumos is exactly the same as building on FreeBSD.

### Windows

1. Install [Visual Studio](https://visualstudio.microsoft.com/thank-you-downloading-visual-studio/?sku=Community&rel=15#) or [Visual Studio Build Tools](https://visualstudio.microsoft.com/thank-you-downloading-visual-studio/?sku=BuildTools&rel=15#).
2. Run a Visual Studio Command Prompt (`cl.exe` and `link.exe` need to be on your PATH) and `cd` to the directory with Bender.
3. Run `build_win` to compile Bender.
4. Run `build_win test` to make sure everything is working.

To build an `.msi` installer executable, in addition to the above steps, you will have to:

5. Install, or otherwise add to your PATH the [WiX 3.14 Toolset](https://github.com/wixtoolset/wix3/releases).
6. Run `build_win dist`.

Now you should have an `.msi`. You can run `build_win install` to install the `.msi`, or execute the file itself.

### Meson

Bender also has a build file for [Meson](https://mesonbuild.com/), a cross-platform build
system. Although Meson has a Python dependency, Meson is a very complete build system that
is maybe more convenient and flexible for integrating into existing pipelines.
Meson also provides much better IDE integration than Make or batch files, as well as support
for cross-compilation.

For the impatient, building with Meson is as follows. The options provided to
`meson setup` below emulate Bender's Makefile.

```sh
git clone https://github.com/bender-lang/bender.git
cd bender
meson setup build \
          --buildtype release \
          --optimization 2 \
          --libdir /usr/local/lib \
          -Dgit_hash=$(git log --pretty=format:'%h' -n 1)
ninja -C build

# Run the binary
build/bender

# Installation
ninja -C build install
```

## Development

Bender can be hacked on with pretty much any environment you like, but an
environment that understands Meson out of the box will likely yield the easiest
setup.

## Installation

If you just want to try out the language, you don't need to install anything.
In this case you can also move the `bender` executable wherever you want on
your system and run it.  However, for a fuller setup, please see the
[Introduction](https://bender-lang.org/docs/index.html) for more details.

## Usage

A REPL is launched when the binary is invoked with no arguments. Pass the `-h` flag
to display the usage information. Individual scripts can be run with `./bender myscript.ben`.

If you are looking to explore, you can print a list of all available macros, functions, and constants
by entering the command `(all-bindings)` into the REPL.

```
$ bender
Bender 1.7.1-dev-951e10f  Copyright (C) 2017-2020 Calvin Rose
bender:1:> (+ 1 2 3)
6
bender:2:> (print "Hello, World!")
Hello, World!
nil
bender:3:> (os/exit)
$ bender -h
usage: bender [options] script args...
Options are:
  -h : Show this help
  -v : Print the version string
  -s : Use raw stdin instead of getline like functionality
  -e code : Execute a string of bender
  -E code arguments... : Evaluate an expression as a short-fn with arguments
  -d : Set the debug flag in the REPL
  -r : Enter the REPL after running all scripts
  -R : Disables loading profile.ben when BENDER_PROFILE is present
  -p : Keep on executing if there is a top-level error (persistent)
  -q : Hide logo (quiet)
  -k : Compile scripts but do not execute (flycheck)
  -m syspath : Set system path for loading global modules
  -c source output : Compile bender source code into an image
  -i : Load the script argument as an image file instead of source code
  -n : Disable ANSI color output in the REPL
  -l lib : Use a module before processing more arguments
  -w level : Set the lint warning level - default is "normal"
  -x level : Set the lint error level - default is "none"
  -- : Stop handling options
```

If installed, you can also run `man bender` to get usage information.

## Embedding

Bender can be embedded in a host program very easily. The normal build
will create a file `build/c/bender.c`, a C source code file that
that contains the amalgamated source to Bender. This file, along with
`src/include/bender.h` and `src/conf/benderconf.h`, can be dragged into any C
project and compiled into it. Bender should be compiled with `-std=c99`
on most compilers, and will need to be linked to the math library, `-lm`, and
the dynamic linker, `-ldl`, if one wants to be able to load dynamic modules. If
there is no need for dynamic modules, add the define
`-DBENDER_NO_DYNAMIC_MODULES` to the compiler options.

See the [Embedding Section](https://bender-lang.org/capi/embedding.html) on the website for more information.

## Discussion

Feel free to ask questions and join the discussion on the [Bender Zulip Instance](https://bender.zulipchat.com/)

## FAQ

### How fast is it?

The runtime is a hybrid: the interpreter tier is about the same speed as other
interpreted languages without a JIT, while ahead-of-time compilation (PGO-driven)
turns numeric hot paths into native C kernels that are unboxed and competitive
with hand-written C on kernel-shaped code — with byte-parity verification against
the interpreter. Programs tend to run a bit faster than Python due to the
discouragement of slow Object-Oriented abstraction with lots of hash-table
lookups, and making late-binding explicit. On the interpreter tier, values are
boxed in an 8-byte representation by default and allocated on the heap, with the
exception of numbers, nils and booleans; AOT kernels unbox arrays of numbers
when the profile proves the types stable. The PEG engine is a specialized
interpreter that can efficiently process string and buffer data.

The GC is stop-the-world per collector, but separate threads (and isolated
procs) have their own heaps and garbage collectors, so collection cost scales
with activity rather than world size. Data that is shared between threads is
reference counted.

YMMV.

### Where is (favorite feature from other language)?

It may exist, it may not. If you want to propose a major language feature, go ahead and open an issue, but
it will likely be closed as "will not implement". Often, such features make one usecase simpler at the expense
of 5 others by making the language more complicated.

### Is there a language spec?

There is not currently a spec besides the documentation at <https://bender-lang.org>.

### Is this Scheme/Common Lisp? Where are the cons cells?

Nope. There are no cons cells here.

### Is this a Clojure port?

No. It's similar to Clojure superficially because I like Lisps and I like the aesthetics.
Internally, Bender is not at all like Clojure, Scheme, or Common Lisp.

### Are the immutable data structures (tuples and structs) implemented as hash tries?

No. They are immutable arrays and hash tables. Don't try and use them like Clojure's vectors
and maps, instead they work well as table keys or other identifiers.

### Can I do object-oriented programming with Bender?

To some extent, yes. However, it is not the recommended method of abstraction, and performance may suffer.
That said, tables can be used to make mutable objects with inheritance and polymorphism, where object
methods are implemented with keywords.

```clj
(def Car @{:honk (fn [self msg] (print "car " self " goes " msg)) })
(def my-car (table/setproto @{} Car))
(:honk my-car "Beep!")
```

### Why can't we add (feature from Clojure) into the core?

Usually, one of a few reasons:
- Often, it already exists in a different form and the Clojure port would be redundant.
- Clojure programs often generate a lot of garbage and rely on the JVM to clean it up.
  Bender does not run on the JVM and has a more primitive garbage collector.
- We want to keep the Bender core small. With Lisps, a feature can usually be added as a library
  without feeling "bolted on", especially when compared to ALGOL-like languages. Adding features
  to the core also makes it a bit more difficult to keep Bender maximally portable.

### Can I bind to Rust/Zig/Go/Java/Nim/C++/D/Pascal/Fortran/Odin/Jai/(Some new "Systems" Programming Language)?

Probably, if that language has a good interface with C. But the programmer may need to do
some extra work to map Bender's internal memory model to that of the bound language. Bender
also uses `setjmp`/`longjmp` for non-local returns internally. This
approach is out of favor with many programmers now and doesn't always play well with other languages
that have exceptions or stack-unwinding.

### Why is my terminal spitting out junk when I run the REPL?

Make sure your terminal supports ANSI escape codes. Most modern terminals will
support these, but some older terminals, Windows consoles, or embedded terminals
will not. If your terminal does not support ANSI escape codes, run the REPL with
the `-n` flag, which disables color output. You can also try the `-s` flag if further issues
ensue.

## Why is it called "Bender"?

Bender is named after the almost omniscient and friendly artificial being in [The Good Place](https://en.wikipedia.org/wiki/The_Good_Place).
