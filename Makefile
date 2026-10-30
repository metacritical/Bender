# Copyright (c) 2026 Calvin Rose
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to
# deal in the Software without restriction, including without limitation the
# rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
# sell copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in
# all copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
# FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
# IN THE SOFTWARE.

################################
##### Set global variables #####
################################
sinclude config.mk
PREFIX?=/usr/local

BENDERCONF_HEADER?=src/conf/benderconf.h
INCLUDEDIR?=$(PREFIX)/include
BINDIR?=$(PREFIX)/bin
LIBDIR?=$(PREFIX)/lib
BENDER_BUILD?="\"$(shell git log --pretty=format:'%h' -n 1 2> /dev/null || echo local)\""
CLIBS=-lm -lpthread
BENDER_TARGET=build/bender
BENDER_BOOT=build/bender_boot
BENDER_IMPORT_LIB=build/bender.lib
BENDER_LIBRARY_IMPORT_LIB=build/libbender.lib
BENDER_LIBRARY=build/libbender.so
BENDER_STATIC_LIBRARY=build/libbender.a
BENDER_PATH?=$(LIBDIR)/bender
BENDER_MANPATH?=$(PREFIX)/share/man/man1/
BENDER_PKG_CONFIG_PATH?=$(LIBDIR)/pkgconfig
BENDER_DIST_DIR?=bender-dist
BENDER_BOOT_FLAGS:=. BENDER_PATH '$(BENDER_PATH)'
BENDER_TARGET_OBJECTS=build/bender.o build/shell.o
JPM_TAG?=master
SPORK_TAG?=master
HAS_SHARED?=1
DEBUGGER=gdb
SONAME_SETTER=-Wl,-soname,
STRIPFLAGS=-x -S

# For cross compilation
HOSTCC?=$(CC)
HOSTAR?=$(AR)
# Symbols are (optionally) removed later, keep -g as default!
CFLAGS?=-O2 -g
LDFLAGS?=-rdynamic
LIBBENDER_LDFLAGS?=$(LDFLAGS)
RUN:=$(RUN)

COMMON_CFLAGS:=-std=c99 -Wall -Wextra -Isrc/include -Isrc/conf -fvisibility=hidden -fPIC
BOOT_CFLAGS:=-DBENDER_BOOTSTRAP -DBENDER_BUILD=$(BENDER_BUILD) -O0 $(COMMON_CFLAGS) -g
BUILD_CFLAGS:=$(CFLAGS) $(COMMON_CFLAGS)

# Native loop/leaf executor tier (experimental, opt-in at BOTH build and run
# time). `make NATIVE_EXECUTORS=1` compiles the tier in; the runtime still
# needs BENDER_NATIVE_EXECUTORS=1 in the environment. Without the compile-time
# define the tier is inert: every registration is refused and
# bender_native_stat reports zeros, so any measurement taken against a plain
# `make` build measures the interpreter only. See
# docs/internals/native-loop-executors.md and `make test-executors`.
ifdef NATIVE_EXECUTORS
BUILD_CFLAGS+=-DBENDER_NATIVE_EXECUTORS
NATIVE_EXECUTORS_MODE=enabled
else
NATIVE_EXECUTORS_MODE=disabled
endif

# Research-only fast-PUT commit protocol. Normal builds have no hook args,
# callback checks, or counters. This flag does not enable a census reporter.
ifdef NATIVE_COMMIT_PROBE
BUILD_CFLAGS+=-DBENDER_NATIVE_COMMIT_PROBE=1
NATIVE_EXECUTORS_MODE:=$(NATIVE_EXECUTORS_MODE)-commit-probe
endif

# Disable amalgamated build
ifeq ($(BENDER_NO_AMALG), 1)
	BENDER_TARGET_OBJECTS+=$(patsubst src/%.c,build/%.bin.o,$(BENDER_CORE_SOURCES))
	BENDER_BOOT_FLAGS+=image-only
endif

# For installation
LDCONFIG:=ldconfig "$(LIBDIR)"

# Check OS
UNAME?=$(shell uname -s)
ifeq ($(UNAME), Darwin)
	CLIBS:=$(CLIBS) -ldl
	SONAME_SETTER:=-Wl,-install_name,
	BENDER_LIBRARY=build/libbender.dylib
	LDCONFIG:=true
else ifeq ($(UNAME), Linux)
	CLIBS:=$(CLIBS) -lrt -ldl
else ifeq ($(UNAME), SunOS)
	BUILD_CFLAGS+=-D__EXTENSIONS__ -DBENDER_NO_NANBOX
	BOOT_CFLAGS+=-D__EXTENSIONS__ -DBENDER_NO_NANBOX
	CLIBS:=-lsocket -lm
	STRIPFLAGS=-x
	LDCONFIG:=false
endif

# Optional image slimming: build with `make NODOCSTRINGS=1` and/or
# `make NOSOURCEMAPS=1` to strip docstrings / source maps from the
# embedded core image (smaller libbender for shipped binaries).
# Both feed boot/config via boot.c; the default build keeps everything.
ifdef NODOCSTRINGS
	BOOT_CFLAGS+=-DBENDER_NO_DOCSTRINGS
endif
ifdef NOSOURCEMAPS
	BOOT_CFLAGS+=-DBENDER_NO_SOURCEMAPS
endif

# For other unix likes, add flags here!
ifeq ($(UNAME), Haiku)
	LDCONFIG:=true
	LDFLAGS=-Wl,--export-dynamic
endif
# For Android (termux)
ifeq ($(UNAME), Linux) # uname on Darwin doesn't recognise -o
ifeq ($(shell uname -o), Android)
	CLIBS:=$(CLIBS) -landroid-spawn
endif
endif

# Mingw
MINGW_COMPILER=
ifeq ($(findstring MINGW,$(UNAME)), MINGW)
	MINGW_COMPILER=gcc
	CLIBS:=-lws2_32 -lpsapi -lwsock32
	LDFLAGS:=-Wl,--out-implib,$(BENDER_IMPORT_LIB)
	LIBBENDER_LDFLAGS:=-Wl,--out-implib,$(BENDER_LIBRARY_IMPORT_LIB)
	BENDER_TARGET:=$(BENDER_TARGET).exe
	BENDER_BOOT:=$(BENDER_BOOT).exe
	COMPILER_VERSION:=$(shell $(CC) --version)
	ifeq ($(findstring clang,$(COMPILER_VERSION)), clang)
		MINGW_COMPILER=clang
	endif
endif


$(shell mkdir -p build/core build/c build/boot build/mainclient)
all: $(BENDER_TARGET) $(BENDER_STATIC_LIBRARY) build/bender.h
ifeq ($(HAS_SHARED), 1)
all: $(BENDER_LIBRARY)
endif

######################
##### Name Files #####
######################

BENDER_HEADERS=src/include/bender.h $(BENDERCONF_HEADER)

BENDER_LOCAL_HEADERS=src/core/features.h \
					src/core/util.h \
					src/core/state.h \
					src/core/gc.h \
					src/core/vector.h \
					src/core/fiber.h \
					src/core/regalloc.h \
					src/core/compile.h \
					src/core/emit.h \
					src/core/symcache.h

BENDER_CORE_SOURCES=src/core/abstract.c \
				   src/core/array.c \
				   src/core/asm.c \
				   src/core/buffer.c \
				   src/core/bytecode.c \
				   src/core/capi.c \
				   src/core/cfuns.c \
				   src/core/compile.c \
				   src/core/corelib.c \
				   src/core/debug.c \
				   src/core/emit.c \
				   src/core/ev.c \
				   src/core/ffi.c \
				   src/core/fiber.c \
				   src/core/filewatch.c \
				   src/core/gc.c \
				   src/core/inttypes.c \
				   src/core/io.c \
				   src/core/marsh.c \
				   src/core/math.c \
				   src/core/net.c \
				   src/core/os.c \
				   src/core/parse.c \
				   src/core/peg.c \
				   src/core/pp.c \
				   src/core/proc.c \
				   src/core/regalloc.c \
				   src/core/run.c \
				   src/core/specials.c \
				   src/core/state.c \
				   src/core/string.c \
				   src/core/strtod.c \
				   src/core/struct.c \
				   src/core/symcache.c \
				   src/core/table.c \
				   src/core/tuple.c \
				   src/core/util.c \
				   src/core/value.c \
				   src/core/vector.c \
				   src/core/vm.c \
				   src/core/wrap.c

BENDER_BOOT_SOURCES=src/boot/array_test.c \
				   src/boot/boot.c \
				   src/boot/buffer_test.c \
				   src/boot/number_test.c \
				   src/boot/system_test.c \
				   src/boot/table_test.c
BENDER_BOOT_HEADERS=src/boot/tests.h

##########################################################
##### The bootstrap interpreter that creates bender.c #####
##########################################################

BENDER_BOOT_OBJECTS=$(patsubst src/%.c,build/%.boot.o,$(BENDER_CORE_SOURCES) $(BENDER_BOOT_SOURCES))

$(BENDER_BOOT_OBJECTS): $(BENDER_BOOT_HEADERS)

build/%.boot.o: src/%.c $(BENDER_HEADERS) $(BENDER_LOCAL_HEADERS) Makefile
	$(CC) $(BOOT_CFLAGS) -o $@ -c $<

$(BENDER_BOOT): $(BENDER_BOOT_OBJECTS)
	$(CC) $(BOOT_CFLAGS) -o $@ $(BENDER_BOOT_OBJECTS) $(CLIBS)

# Now the reason we bootstrap in the first place
build/c/bender.c: $(BENDER_BOOT) src/boot/boot.ben
	$(RUN) $(BENDER_BOOT) $(BENDER_BOOT_FLAGS) > $@
	cksum $@

##################
##### Quicky #####
##################

# Make does not track changes to BUILD_CFLAGS. Check the effective native mode
# on every invocation, changing the stamp only when its value changes; all
# objects compiled with BUILD_CFLAGS must then rebuild in either direction.
# The phony prerequisite runs the check; the stamp recipe is a no-op when
# the value already matches, so same-mode builds remain incremental.
.PHONY: native-executors-mode-check
native-executors-mode-check:

build/.native-executors-mode: native-executors-mode-check
	@mode='$(NATIVE_EXECUTORS_MODE)'; stamp=$@; \
	 if test -f "$$stamp" && test "$$(cat "$$stamp")" = "$$mode"; then :; \
	 else printf '%s\n' "$$mode" > "$$stamp.tmp" && mv -f "$$stamp.tmp" "$$stamp"; fi

build/%.bin.o: src/%.c $(BENDER_HEADERS) $(BENDER_LOCAL_HEADERS) Makefile build/.native-executors-mode
	$(HOSTCC) $(BUILD_CFLAGS) -o $@ -c $<

########################
##### Amalgamation #####
########################

ifeq ($(UNAME), Darwin)
SONAME=libbender.1.42.dylib
else
SONAME=libbender.so.1.42
endif

ifeq ($(MINGW_COMPILER), clang)
	SONAME=
	SONAME_SETTER=
endif

build/c/shell.c: src/mainclient/shell.c
	cp $< $@

build/bender.h: $(BENDER_TARGET) src/include/bender.h $(BENDERCONF_HEADER)
	$(RUN) ./$(BENDER_TARGET) tools/patch-header.ben src/include/bender.h $(BENDERCONF_HEADER) $@

build/benderconf.h: $(BENDERCONF_HEADER)
	cp $< $@

build/bender.o: build/c/bender.c $(BENDERCONF_HEADER) src/include/bender.h build/.native-executors-mode
	$(HOSTCC) $(BUILD_CFLAGS) -c $< -o $@

build/shell.o: build/c/shell.c $(BENDERCONF_HEADER) src/include/bender.h build/.native-executors-mode
	$(HOSTCC) $(BUILD_CFLAGS) -c $< -o $@

$(BENDER_TARGET): $(BENDER_TARGET_OBJECTS)
	$(HOSTCC) $(LDFLAGS) $(BUILD_CFLAGS) -o $@ $^ $(CLIBS)

$(BENDER_LIBRARY): $(BENDER_TARGET_OBJECTS)
	$(HOSTCC) $(LIBBENDER_LDFLAGS) $(BUILD_CFLAGS) $(SONAME_SETTER)$(SONAME) -shared -o $@ $^ $(CLIBS)

$(BENDER_STATIC_LIBRARY): $(BENDER_TARGET_OBJECTS)
	$(HOSTAR) rcs $@ $^

###################
##### Testing #####
###################

# Testing assumes HOSTCC=CC

TEST_SCRIPTS=$(wildcard test/suite*.ben)
EXAMPLE_SCRIPTS=$(wildcard examples/*.ben)

repl: $(BENDER_TARGET)
	$(RUN) ./$(BENDER_TARGET)

debug: $(BENDER_TARGET)
	$(DEBUGGER) ./$(BENDER_TARGET)

VALGRIND_COMMAND=$(RUN) valgrind --leak-check=full --quiet
CALLGRIND_COMMAND=$(RUN) valgrind --tool=callgrind

valgrind: $(BENDER_TARGET)
	$(VALGRIND_COMMAND) ./$(BENDER_TARGET)

# Benchmarks: micro-benchmarks plus the cross-runtime comparison.
# BENCH_N sets the process count for the cross-runtime tables.
BENCH_N ?= 1000000
bench: $(BENDER_TARGET)
	$(RUN) ./$(BENDER_TARGET) bench/concurrency.ben
	@echo
	@sh bench/cross-runtime/run.sh $(BENCH_N)

# Only Bender's own micro-benchmarks: no Erlang/Go toolchain needed.
bench-bender: $(BENDER_TARGET)
	$(RUN) ./$(BENDER_TARGET) bench/concurrency.ben

test: $(BENDER_TARGET) $(TEST_SCRIPTS) $(EXAMPLE_SCRIPTS)
	for f in test/suite*.ben; do $(RUN) ./$(BENDER_TARGET) "$$f" || exit; done
	for f in examples/*.ben; do $(RUN) ./$(BENDER_TARGET) -k "$$f"; done

# Build-mode transitions run in a disposable source tree, independent of the
# caller's build products and safe alongside other suite processes.
test-build-mode:
	@sh test/native-executor-build-mode.sh

# The native executor suite is a shell script, so `make test` (which globs
# test/suite*.ben) never runs it. It asserts real executor entry counts and
# therefore REQUIRES a NATIVE_EXECUTORS=1 build; against a plain build it
# fails on inert counters rather than on a defect. Run it explicitly:
#   make NATIVE_EXECUTORS=1 test-executors
# Do not omit the flag in the second make invocation: mode switches now rebuild.
test-executors: $(BENDER_TARGET) test-build-mode test-forced-quantum
	@BENDER_AOT_TEST_ROOT=$${BENDER_AOT_TEST_ROOT:-$$(mktemp -d)} \
	 XDG_CACHE_HOME=$${XDG_CACHE_HOME:-$$(mktemp -d)} \
	 sh test/suite-native-executor.sh

# Keep the probe-only runtime and fixture under /tmp, without touching the
# caller's ordinary build products. The script builds both ABI modes.
test-native-commit-probe:
	@sh test/native-commit-probe.sh

# Explicit native opt-in must be on this same make invocation.
test-forced-quantum: $(BENDER_TARGET)
	@BENDER_AOT_TEST_ROOT=$${BENDER_AOT_TEST_ROOT:-$$(mktemp -d)} \
	 XDG_CACHE_HOME=$${XDG_CACHE_HOME:-$$(mktemp -d)} \
	 sh test/suite-forced-quantum.sh

# Phase 1.1b: C call-chain suspension proof (tools/fiber-c-test.c).
# Builds against the static library and runs the embedder assertions.
fibertest: $(BENDER_TARGET) $(BENDER_STATIC_LIBRARY)
	$(CC) -O2 -Ibuild -Isrc/include -Isrc/core tools/fiber-c-test.c $(BENDER_STATIC_LIBRARY) -lm -lpthread -ldl -o build/fiber-c-test
	$(RUN) ./build/fiber-c-test

valtest: $(BENDER_TARGET) $(TEST_SCRIPTS) $(EXAMPLE_SCRIPTS)
	for f in test/suite*.ben; do $(VALGRIND_COMMAND) ./$(BENDER_TARGET) "$$f" || exit; done
	for f in examples/*.ben; do $(VALGRIND_COMMAND) ./$(BENDER_TARGET) -k "$$f"; done

callgrind: $(BENDER_TARGET)
	$(CALLGRIND_COMMAND) ./$(BENDER_TARGET)

calltest: $(BENDER_TARGET) $(TEST_SCRIPTS) $(EXAMPLE_SCRIPTS)
	for f in test/suite*.ben; do $(CALLGRIND_COMMAND) ./$(BENDER_TARGET) "$$f" || exit; done
	for f in examples/*.ben; do $(CALLGRIND_COMMAND) ./$(BENDER_TARGET) -k "$$f"; done

########################
##### Distribution #####
########################

dist: build/bender-dist.tar.gz

build/bender-%.tar.gz: $(BENDER_TARGET) \
	build/bender.h \
	bender.1 LICENSE CONTRIBUTING.md $(BENDER_STATIC_LIBRARY) \
	README.md build/c/bender.c build/c/shell.c
	mkdir -p build/$(BENDER_DIST_DIR)/bin
	cp $(BENDER_TARGET) build/$(BENDER_DIST_DIR)/bin/
	strip $(STRIPFLAGS) 'build/$(BENDER_DIST_DIR)/bin/bender'
	mkdir -p build/$(BENDER_DIST_DIR)/include
	cp build/bender.h build/$(BENDER_DIST_DIR)/include/
	mkdir -p build/$(BENDER_DIST_DIR)/lib/
	cp $(BENDER_STATIC_LIBRARY) build/$(BENDER_DIST_DIR)/lib/
	cp $(BENDER_LIBRARY) build/$(BENDER_DIST_DIR)/lib/ || true
	mkdir -p build/$(BENDER_DIST_DIR)/man/man1/
	cp bender.1 build/$(BENDER_DIST_DIR)/man/man1/bender.1
	mkdir -p build/$(BENDER_DIST_DIR)/src/
	cp build/c/bender.c build/c/shell.c build/$(BENDER_DIST_DIR)/src/
	cp CONTRIBUTING.md LICENSE README.md build/$(BENDER_DIST_DIR)/
	cd build && tar -czvf ../$@ ./$(BENDER_DIST_DIR)
ifeq ($(HAS_SHARED), 1)
build/bender-%.tar.gz: $(BENDER_LIBRARY)
endif

#########################
##### Documentation #####
#########################

docs: build/doc.html

build/doc.html: $(BENDER_TARGET) tools/gendoc.ben
	$(RUN) $(BENDER_TARGET) tools/gendoc.ben > build/doc.html

########################
##### Installation #####
########################

.INTERMEDIATE: build/bender.pc
build/bender.pc: $(BENDER_TARGET)
	echo 'prefix=$(PREFIX)' > $@
	echo 'exec_prefix=$${prefix}' >> $@
	echo 'includedir=$(INCLUDEDIR)/bender' >> $@
	echo 'libdir=$(LIBDIR)' >> $@
	echo "" >> $@
	echo "Name: bender" >> $@
	echo "Url: https://bender-lang.org" >> $@
	echo "Description: Library for the Bender programming language." >> $@
	$(RUN) $(BENDER_TARGET) -e '(print "Version: " bender/version)' >> $@
	echo 'Cflags: -I$${includedir}' >> $@
	echo 'Libs: -L$${libdir} -lbender' >> $@
	echo 'Libs.private: $(CLIBS)' >> $@

install: $(BENDER_TARGET) $(BENDER_LIBRARY) $(BENDER_STATIC_LIBRARY) build/bender.pc build/bender.h
	$(eval BENDER_VERSION := $(shell $(BENDER_TARGET) -e '(print bender/version)'))
	mkdir -p '$(DESTDIR)$(BINDIR)'
	cp $(BENDER_TARGET) '$(DESTDIR)$(BINDIR)/bender'
	strip $(STRIPFLAGS) '$(DESTDIR)$(BINDIR)/bender'
	mkdir -p '$(DESTDIR)$(INCLUDEDIR)/bender'
	cp -r build/bender.h '$(DESTDIR)$(INCLUDEDIR)/bender'
	ln -sf ./bender/bender.h '$(DESTDIR)$(INCLUDEDIR)/bender.h'
	mkdir -p '$(DESTDIR)$(BENDER_PATH)'
	mkdir -p '$(DESTDIR)$(LIBDIR)'
	if test $(UNAME) = Darwin ; then \
		cp $(BENDER_LIBRARY) '$(DESTDIR)$(LIBDIR)/libbender.$(BENDER_VERSION).dylib' ; \
		ln -sf $(SONAME) '$(DESTDIR)$(LIBDIR)/libbender.dylib' ; \
		ln -sf libbender.$(BENDER_VERSION).dylib $(DESTDIR)$(LIBDIR)/$(SONAME) ; \
	else \
		cp $(BENDER_LIBRARY) '$(DESTDIR)$(LIBDIR)/libbender.so.$(BENDER_VERSION)' ; \
		ln -sf $(SONAME) '$(DESTDIR)$(LIBDIR)/libbender.so' ; \
		ln -sf libbender.so.$(BENDER_VERSION) $(DESTDIR)$(LIBDIR)/$(SONAME) ; \
	fi
	cp $(BENDER_STATIC_LIBRARY) '$(DESTDIR)$(LIBDIR)/libbender.a'
	mkdir -p '$(DESTDIR)$(BENDER_MANPATH)'
	cp bender.1 '$(DESTDIR)$(BENDER_MANPATH)'
	mkdir -p '$(DESTDIR)$(LIBDIR)/bender/tools'
	cp tools/callgraph.ben tools/mkimage.ben tools/infer.ben '$(DESTDIR)$(LIBDIR)/bender/tools'
	mkdir -p '$(DESTDIR)$(BENDER_PKG_CONFIG_PATH)'
	cp build/bender.pc '$(DESTDIR)$(BENDER_PKG_CONFIG_PATH)/bender.pc'
	cp '$(BENDER_IMPORT_LIB)' '$(DESTDIR)$(LIBDIR)' || echo 'no import lib to install (mingw only)'
	cp '$(BENDER_LIBRARY_IMPORT_LIB)' '$(DESTDIR)$(LIBDIR)' || echo 'no import lib to install (mingw only)'
	[ -z '$(DESTDIR)' ] && $(LDCONFIG) || echo "You can ignore this error for non-Linux systems or local installs"

install-jpm-git: $(BENDER_TARGET)
	mkdir -p build
	rm -rf build/jpm
	git clone --depth=1 --branch='$(JPM_TAG)' https://github.com/bender-lang/jpm.git build/jpm
	cd build/jpm && PREFIX='$(PREFIX)' \
		DESTDIR=$(DESTDIR) \
		BENDER_MANPATH='$(BENDER_MANPATH)' \
		BENDER_HEADERPATH='$(INCLUDEDIR)/bender' \
		BENDER_BINPATH='$(BINDIR)' \
		BENDER_LIBPATH='$(LIBDIR)' \
		$(RUN) ../../$(BENDER_TARGET) ./bootstrap.ben

install-spork-git: $(BENDER_TARGET)
	mkdir -p build
	rm -rf build/spork
	git clone --depth=1 --branch='$(SPORK_TAG)' https://github.com/bender-lang/spork.git build/spork
	$(BENDER_TARGET) -e '(bundle/install "build/spork")'

uninstall:
	-rm '$(DESTDIR)$(BINDIR)/bender'
	-rm -rf '$(DESTDIR)$(INCLUDEDIR)/bender'
	-rm -rf '$(DESTDIR)$(INCLUDEDIR)/bender.h'
	-rm -rf '$(DESTDIR)$(LIBDIR)'/libbender.*
	-rm '$(DESTDIR)$(BENDER_PKG_CONFIG_PATH)/bender.pc'
	-rm '$(DESTDIR)$(BENDER_MANPATH)/bender.1'
	-rm -rf '$(DESTDIR)$(LIBDIR)/bender/tools'
	# -rm -rf '$(DESTDIR)$(BENDER_PATH)'/* - err on the side of correctness here

#################
##### Other #####
#################

format:
	sh tools/format.sh

grammar: build/bender.tmLanguage
build/bender.tmLanguage: tools/tm_lang_gen.ben $(BENDER_TARGET)
	$(RUN) $(BENDER_TARGET) $< > $@

grammar-linguist: grammar/syntaxes/bender.tmLanguage.json
grammar/syntaxes/bender.tmLanguage.json: tools/linguist_grammar.ben $(BENDER_TARGET)
	$(RUN) $(BENDER_TARGET) $< > $@

compile-commands:
	# Requires pip install compiledb
	compiledb make

clean:
	-rm -rf build vgcore.* callgrind.*
	-rm -rf test/install/build test/install/modpath

help:
	@echo
	@echo 'Bender: A Dynamic Language & Bytecode VM'
	@echo
	@echo Usage:
	@echo '   make            Build Bender'
	@echo '   make repl       Start a REPL from a built Bender'
	@echo
	@echo '   make test       Test a built Bender'
	@echo '   make test-build-mode  Test plain/native build transitions in isolation'
	@echo '   make NATIVE_EXECUTORS=1 test-executors  Test native executors and build mode'
	@echo '   make valgrind   Assess Bender with Valgrind'
	@echo '   make callgrind  Assess Bender with Valgrind, using Callgrind'
	@echo '   make valtest    Run the test suite and examples with Valgrind to check for memory leaks'
	@echo '   make calltest   Run the test suite and examples with Callgrind'
	@echo '   make dist       Create a distribution tarball'
	@echo '   make docs       Generate documentation'
	@echo '   make debug      Run bender with GDB or LLDB'
	@echo '   make install    Install into the current filesystem'
	@echo '   make uninstall  Uninstall from the current filesystem'
	@echo '   make clean      Clean intermediate build artifacts'
	@echo "   make format     Format Bender's own source files"
	@echo '   make grammar    Generate a TextMate language grammar'
	@echo
	@echo '   make install-jpm-git   Install jpm into the current filesystem'
	@echo '   make install-spork-git Install spork into the current filesystem'
	@echo

.PHONY: clean install install-jpm-git install-spork-git repl debug valgrind test \
	test-build-mode test-executors valtest callgrind callgrind-test dist uninstall docs grammar grammar-linguist format help compile-commands
