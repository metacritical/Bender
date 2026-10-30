# Copyright (c) 2026 Calvin Rose and contributors
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

project('bender', 'c',
  default_options : ['c_std=c99', 'build.c_std=c99', 'b_lundef=false', 'default_library=both'],
  version : '1.42.1')

# Global settings
bender_path = join_paths(get_option('prefix'), get_option('libdir'), 'bender')
header_path = join_paths(get_option('prefix'), get_option('includedir'), 'bender')

# Compilers
cc = meson.get_compiler('c')
native_cc = meson.get_compiler('c', native : true)

# Native deps
native_m_dep = native_cc.find_library('m', required : false)
native_dl_dep = native_cc.find_library('dl', required : false)
native_android_spawn_dep = native_cc.find_library('android-spawn', required : false)
native_thread_dep = dependency('threads', native : true)

# Deps
m_dep = cc.find_library('m', required : false)
dl_dep = cc.find_library('dl', required : false)

# for MINGW/MSYS2
native_ws2_dep = native_cc.find_library('ws2_32', required: false)
native_psapi_dep = native_cc.find_library('psapi', required: false)
native_wsock_dep = native_cc.find_library('wsock32', required: false)
ws2_dep = cc.find_library('ws2_32', required: false)
psapi_dep = cc.find_library('psapi', required: false)
wsock_dep = cc.find_library('wsock32', required: false)

android_spawn_dep = cc.find_library('android-spawn', required : false)
thread_dep = dependency('threads')

# Link options
if get_option('default_library') != 'static' and build_machine.system() != 'windows'
    add_project_link_arguments('-rdynamic', language : 'c')
endif

# Generate custom benderconf.h
conf = configuration_data()
version_parts = meson.project_version().split('.')
last_parts = version_parts[2].split('-')
if last_parts.length() > 1
  conf.set_quoted('BENDER_VERSION_EXTRA', '-' + last_parts[1])
else
  conf.set_quoted('BENDER_VERSION_EXTRA', '')
endif
conf.set('BENDER_VERSION_MAJOR', version_parts[0].to_int())
conf.set('BENDER_VERSION_MINOR', version_parts[1].to_int())
conf.set('BENDER_VERSION_PATCH', last_parts[0].to_int())
conf.set_quoted('BENDER_VERSION', meson.project_version())
# Use options
conf.set_quoted('BENDER_BUILD', get_option('git_hash'))
conf.set('BENDER_NO_NANBOX', not get_option('nanbox'))
if get_option('nanbox_pointer_shift') != -1 # -1 is auto-detect
  conf.set('BENDER_NANBOX_64_POINTER_SHIFT', get_option('nanbox_pointer_shift'))
endif
conf.set('BENDER_SINGLE_THREADED', get_option('single_threaded'))
conf.set('BENDER_NO_DYNAMIC_MODULES', not get_option('dynamic_modules'))
conf.set('BENDER_NO_DOCSTRINGS', not get_option('docstrings'))
conf.set('BENDER_NO_SOURCEMAPS', not get_option('sourcemaps'))
conf.set('BENDER_NO_ASSEMBLER', not get_option('assembler'))
conf.set('BENDER_NO_PEG', not get_option('peg'))
conf.set('BENDER_NO_NET', not get_option('net'))
conf.set('BENDER_NO_IPV6', not get_option('ipv6'))
conf.set('BENDER_NO_EV', not get_option('ev') or get_option('single_threaded'))
conf.set('BENDER_REDUCED_OS', get_option('reduced_os'))
conf.set('BENDER_NO_INT_TYPES', not get_option('int_types'))
conf.set('BENDER_PRF', get_option('prf'))
conf.set('BENDER_RECURSION_GUARD', get_option('recursion_guard'))
conf.set('BENDER_MAX_PROTO_DEPTH', get_option('max_proto_depth'))
conf.set('BENDER_MAX_MACRO_EXPAND', get_option('max_macro_expand'))
conf.set('BENDER_STACK_MAX', get_option('stack_max'))
conf.set('BENDER_NO_UMASK', not get_option('umask'))
conf.set('BENDER_NO_REALPATH', not get_option('realpath'))
conf.set('BENDER_NO_PROCESSES', not get_option('processes'))
conf.set('BENDER_SIMPLE_GETLINE', get_option('simple_getline'))
conf.set('BENDER_EV_NO_EPOLL', not get_option('epoll'))
conf.set('BENDER_EV_NO_KQUEUE', not get_option('kqueue'))
conf.set('BENDER_NO_INTERPRETER_INTERRUPT', not get_option('interpreter_interrupt'))
conf.set('BENDER_NO_FFI', not get_option('ffi'))
conf.set('BENDER_NO_FFI_JIT', not get_option('ffi_jit'))
conf.set('BENDER_NO_FILEWATCH', not get_option('filewatch'))
conf.set('BENDER_NO_CRYPTORAND', not get_option('cryptorand'))
if get_option('os_name') != ''
  conf.set('BENDER_OS_NAME', get_option('os_name'))
endif
if get_option('arch_name') != ''
  conf.set('BENDER_ARCH_NAME', get_option('arch_name'))
endif
if get_option('thread_local_prefix') != ''
  conf.set('BENDER_THREAD_LOCAL', get_option('thread_local_prefix'))
endif
jconf = configure_file(output : 'benderconf.h',
  configuration : conf)

# Include directories
incdir = include_directories(['src/include', '.'])

# Order is important here, as some headers
# depend on other headers for the amalg target
core_headers = [
  'src/core/features.h',
  'src/core/util.h',
  'src/core/state.h',
  'src/core/gc.h',
  'src/core/vector.h',
  'src/core/fiber.h',
  'src/core/regalloc.h',
  'src/core/compile.h',
  'src/core/emit.h',
  'src/core/symcache.h'
]

core_src = [
  'src/core/abstract.c',
  'src/core/array.c',
  'src/core/asm.c',
  'src/core/buffer.c',
  'src/core/bytecode.c',
  'src/core/capi.c',
  'src/core/cfuns.c',
  'src/core/compile.c',
  'src/core/corelib.c',
  'src/core/debug.c',
  'src/core/emit.c',
  'src/core/ev.c',
  'src/core/ffi.c',
  'src/core/fiber.c',
  'src/core/filewatch.c',
  'src/core/gc.c',
  'src/core/inttypes.c',
  'src/core/io.c',
  'src/core/marsh.c',
  'src/core/math.c',
  'src/core/net.c',
  'src/core/os.c',
  'src/core/parse.c',
  'src/core/peg.c',
  'src/core/pp.c',
  'src/core/regalloc.c',
  'src/core/run.c',
  'src/core/specials.c',
  'src/core/state.c',
  'src/core/string.c',
  'src/core/strtod.c',
  'src/core/struct.c',
  'src/core/symcache.c',
  'src/core/table.c',
  'src/core/tuple.c',
  'src/core/util.c',
  'src/core/value.c',
  'src/core/vector.c',
  'src/core/vm.c',
  'src/core/wrap.c'
]

boot_src = [
  'src/boot/array_test.c',
  'src/boot/boot.c',
  'src/boot/buffer_test.c',
  'src/boot/number_test.c',
  'src/boot/system_test.c',
  'src/boot/table_test.c',
]

mainclient_src = [
  'src/mainclient/shell.c'
]

bender_dependencies = [m_dep, dl_dep, android_spawn_dep, ws2_dep, psapi_dep, wsock_dep]
bender_native_dependencies = [native_m_dep, native_dl_dep, native_android_spawn_dep, native_ws2_dep, native_psapi_dep, native_wsock_dep]
if not get_option('single_threaded')
  bender_dependencies += thread_dep
  bender_native_dependencies += native_thread_dep
endif

# Build boot binary
bender_boot = executable('bender-boot', core_src, boot_src,
  include_directories : incdir,
  c_args : '-DBENDER_BOOTSTRAP',
  dependencies : bender_native_dependencies,
  native : true)

# Build bender.c
benderc = custom_target('benderc',
  input : [bender_boot, 'src/boot/boot.ben'],
  output : 'bender.c',
  capture : true,
  command : [
    bender_boot, meson.current_source_dir(),
    'BENDER_PATH', bender_path
  ])

# Allow building with no shared library
if cc.has_argument('-fvisibility=hidden')
  lib_cflags = ['-fvisibility=hidden']
else
  lib_cflags = []
endif
if get_option('shared')
  libbender = library('bender', benderc,
    include_directories : incdir,
    dependencies : bender_dependencies,
    version: meson.project_version(),
    soversion: version_parts[0] + '.' + version_parts[1],
    c_args : lib_cflags,
    install : true)
# Extra c flags - adding -fvisibility=hidden matches the Makefile and
# shaves off about 10k on linux x64, likely similar on other platforms.
  if cc.has_argument('-fvisibility=hidden')
    extra_cflags = ['-fvisibility=hidden', '-DBENDER_DLL_IMPORT']
  else
    extra_cflags = ['-DBENDER_DLL_IMPORT']
  endif
  bender_mainclient = executable('bender', mainclient_src,
    include_directories : incdir,
    dependencies : bender_dependencies,
    link_with: [libbender],
    c_args : extra_cflags,
    install : true)
else
  # No shared library
  bender_mainclient = executable('bender', mainclient_src, benderc,
    include_directories : incdir,
    dependencies : bender_dependencies,
    c_args : lib_cflags,
    install : true)
endif

if meson.is_cross_build()
  native_cc = meson.get_compiler('c', native: true)
  if native_cc.has_argument('-fvisibility=hidden')
    extra_native_cflags = ['-fvisibility=hidden']
  else
    extra_native_cflags = []
  endif
  bender_nativeclient = executable('bender-native', benderc, mainclient_src,
    include_directories : incdir,
    dependencies : bender_native_dependencies,
    c_args : extra_native_cflags,
    native : true)
else
  bender_nativeclient = bender_mainclient
endif

# Documentation
docs = custom_target('docs',
  input : ['tools/gendoc.ben'],
  output : ['doc.html'],
  capture : true,
  command : [bender_nativeclient, '@INPUT@'])

# Tests
test_files = [
  'test/suite-array.ben',
  'test/suite-asm.ben',
  'test/suite-boot.ben',
  'test/suite-buffer.ben',
  'test/suite-bundle.ben',
  'test/suite-capi.ben',
  'test/suite-cfuns.ben',
  'test/suite-compile.ben',
  'test/suite-corelib.ben',
  'test/suite-debug.ben',
  'test/suite-ev.ben',
  'test/suite-ev2.ben',
  'test/suite-ffi.ben',
  'test/suite-filewatch.ben',
  'test/suite-inttypes.ben',
  'test/suite-io.ben',
  'test/suite-marsh.ben',
  'test/suite-math.ben',
  'test/suite-net.ben',
  'test/suite-os.ben',
  'test/suite-parse.ben',
  'test/suite-peg.ben',
  'test/suite-pp.ben',
  'test/suite-specials.ben',
  'test/suite-string.ben',
  'test/suite-strtod.ben',
  'test/suite-struct.ben',
  'test/suite-symcache.ben',
  'test/suite-table.ben',
  'test/suite-tuple.ben',
  'test/suite-unknown.ben',
  'test/suite-value.ben',
  'test/suite-vm.ben'
]
foreach t : test_files
  test(t, bender_nativeclient, args : files([t]), workdir : meson.current_source_dir())
endforeach

# Repl
run_target('repl', command : [bender_nativeclient])

# For use as meson subproject (wrap)
if get_option('shared')
  bender_dep = declare_dependency(include_directories : incdir,
    link_with : libbender)
# pkgconfig
  pkg = import('pkgconfig')
  pkg.generate(libbender,
    subdirs: 'bender',
    description: 'Library for the Bender programming language.')
endif

# Installation
install_man('bender.1')
install_data(sources : ['tools/.keep'], install_dir : join_paths(get_option('libdir'), 'bender'))
patched_bender = custom_target('patched-benderh',
  input : ['tools/patch-header.ben', 'src/include/bender.h', jconf],
  install : true,
  install_dir : join_paths(get_option('includedir'), 'bender'),
  build_by_default : true,
  output : ['bender_' + meson.project_version() + '.h'],
  command : [bender_nativeclient, '@INPUT@', '@OUTPUT@'])

# Create a version of the bender.h header that matches what jpm often expects
if meson.version().version_compare('>=0.61')
  install_symlink('bender.h', pointing_to: 'bender/bender_' + meson.project_version() + '.h', install_dir: get_option('includedir'))
  install_symlink('bender.h', pointing_to: 'bender_' + meson.project_version() + '.h', install_dir: join_paths(get_option('includedir'), 'bender'))
endif

