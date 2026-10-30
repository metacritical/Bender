/*
* Copyright (c) 2026 Calvin Rose
*
* Permission is hereby granted, free of charge, to any person obtaining a copy
* of this software and associated documentation files (the "Software"), to
* deal in the Software without restriction, including without limitation the
* rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
* sell copies of the Software, and to permit persons to whom the Software is
* furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included in
* all copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
* AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
* FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
* IN THE SOFTWARE.
*/

#ifndef BENDER_AMALG
#include "features.h"
#include <bender.h>
#include "state.h"
#include "util.h"
#endif

#include <stdio.h>
#include <errno.h>

#ifndef BENDER_WINDOWS
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

static int cfun_io_gc(void *p, size_t len);
static int io_file_get(void *p, Bender key, Bender *out);
static void io_file_marshal(void *p, BenderMarshalContext *ctx);
static void *io_file_unmarshal(BenderMarshalContext *ctx);
static Bender io_file_next(void *p, Bender key);

#ifdef BENDER_WINDOWS
#include <io.h>
#define ftell _ftelli64
#define fseek _fseeki64
#endif

const BenderAbstractType bender_file_type = {
    "core/file",
    cfun_io_gc,
    NULL,
    io_file_get,
    NULL,
    io_file_marshal,
    io_file_unmarshal,
    NULL, /* tostring */
    NULL, /* compare */
    NULL, /* hash */
    io_file_next,
    BENDER_ATEND_NEXT
};

/* Check arguments to fopen */
static int32_t checkflags(const uint8_t *str) {
    int32_t flags = 0;
    int32_t i;
    int32_t len = bender_string_length(str);
    if (!len || len > 10)
        bender_panic("file mode must have a length between 1 and 10");
    switch (*str) {
        default:
            bender_panicf("invalid flag %c, expected w, a, or r", *str);
            break;
        case 'w':
            flags |= BENDER_FILE_WRITE;
            bender_sandbox_assert(BENDER_SANDBOX_FS_WRITE);
            break;
        case 'a':
            flags |= BENDER_FILE_APPEND;
            bender_sandbox_assert(BENDER_SANDBOX_FS);
            break;
        case 'r':
            flags |= BENDER_FILE_READ;
            bender_sandbox_assert(BENDER_SANDBOX_FS_READ);
            break;
    }
    for (i = 1; i < len; i++) {
        switch (str[i]) {
            default:
                bender_panicf("invalid flag %c, expected +, b, or n", str[i]);
                break;
            case '+':
                if (flags & BENDER_FILE_UPDATE) return -1;
                bender_sandbox_assert(BENDER_SANDBOX_FS_WRITE);
                flags |= BENDER_FILE_UPDATE;
                break;
            case 'b':
                if (flags & BENDER_FILE_BINARY) return -1;
                flags |= BENDER_FILE_BINARY;
                break;
            case 'n':
                if (flags & BENDER_FILE_NONIL) return -1;
                flags |= BENDER_FILE_NONIL;
                break;
        }
    }
    return flags;
}

static void *makef(FILE *f, int32_t flags, size_t bufsize) {
    BenderFile *iof = (BenderFile *) bender_abstract(&bender_file_type, sizeof(BenderFile));
    iof->file = f;
    iof->flags = flags;
    iof->vbufsize = bufsize;
#if !(defined(BENDER_WINDOWS) || defined(BENDER_PLAN9))
    /* While we would like fopen to set cloexec by default (like O_CLOEXEC) with the e flag, that is
     * not standard. */
    if (!(flags & BENDER_FILE_NOT_CLOSEABLE))
        fcntl(fileno(f), F_SETFD, FD_CLOEXEC);
#endif
    return iof;
}

BENDER_CORE_FN(cfun_io_temp,
              "(file/temp)",
              "Open an anonymous temporary file that is removed on close. "
              "Raises an error on failure.") {
    bender_sandbox_assert(BENDER_SANDBOX_FS_TEMP);
    (void)argv;
    bender_fixarity(argc, 0);
    // XXX use mkostemp when we can to avoid CLOEXEC race.
    FILE *tmp = tmpfile();
    if (!tmp)
        bender_panicf("unable to create temporary file - %s", bender_strerror(errno));
    return bender_makefile(tmp, BENDER_FILE_WRITE | BENDER_FILE_READ | BENDER_FILE_BINARY);
}

BENDER_CORE_FN(cfun_io_fopen,
              "(file/open path &opt mode buffer-size)",
              "Open a file. `path` is an absolute or relative path, and "
              "`mode` is a set of flags indicating the mode to open the "
              "file in. `mode` is a keyword where each character represents "
              "a flag. If the file cannot be opened, returns nil, otherwise "
              "returns the new file handle. Mode flags:\n"
              "\n"
              "* `r` - allow reading from file\n"
              "* `w` - allow writing to file\n"
              "* `a` - append to file\n"
              "\n"
              "After one of the initial flags, 0 or more of the following "
              "flags can be appended:\n"
              "\n"
              "* `b` - open file in binary mode (rather than text mode)\n"
              "* `+` - open file for both reading and writing\n"
              "* `n` - error if file cannot be opened instead of returning nil\n"
              "\n"
              "See `fopen()` (`<stdio.h>`, C99) for further details.") {
    bender_arity(argc, 1, 3);
    const uint8_t *fname = bender_getstring(argv, 0);
    const uint8_t *fmode;
    int32_t flags;
    if (argc == 2) {
        fmode = bender_getkeyword(argv, 1);
        flags = checkflags(fmode);
    } else {
        fmode = (const uint8_t *)"r";
        bender_sandbox_assert(BENDER_SANDBOX_FS_READ);
        flags = BENDER_FILE_READ;
    }
    FILE *f = fopen((const char *)fname, (const char *)fmode);
    size_t bufsize = BUFSIZ;
    if (f != NULL) {
#if !(defined(BENDER_WINDOWS) || defined(BENDER_PLAN9))
        struct stat st;
        fstat(fileno(f), &st);
        if (S_ISDIR(st.st_mode)) {
            fclose(f);
            bender_panicf("cannot open directory: %s", fname);
        }
#endif
        bufsize = bender_optsize(argv, argc, 2, BUFSIZ);
        if (bufsize != BUFSIZ) {
            int result = setvbuf(f, NULL, bufsize ? _IOFBF : _IONBF, bufsize);
            if (result) {
                bender_panic("failed to set buffer size for file");
            }
        }
    }
    return f ? bender_wrap_abstract(makef(f, flags, bufsize))
           : (flags & BENDER_FILE_NONIL) ? (bender_panicf("failed to open file %s: %s", fname, bender_strerror(errno)), bender_wrap_nil())
           : bender_wrap_nil();
}

/* Read up to n bytes into buffer. */
static void read_chunk(BenderFile *iof, BenderBuffer *buffer, int32_t nBytesMax) {
    if (!(iof->flags & (BENDER_FILE_READ | BENDER_FILE_UPDATE)))
        bender_panic("file is not readable");
    bender_buffer_extra(buffer, nBytesMax);
    size_t ntoread = nBytesMax;
    size_t nread = fread((char *)(buffer->data + buffer->count), 1, ntoread, iof->file);
    if (nread != ntoread && ferror(iof->file))
        bender_panic("could not read file");
    /* Auto-PGO replayability: a run that consumed stdin is only gateable if
     * those bytes can be captured (see bender_profile_note_stdin). */
    if (nread && iof->file == stdin) bender_profile_note_stdin();
    buffer->count += (int32_t) nread;
}

/* Read a certain number of bytes into memory */
BENDER_CORE_FN(cfun_io_fread,
              "(file/read f what &opt buf)",
              "Read a number of bytes from a file `f` into a buffer. A buffer `buf` can "
              "be provided as an optional third argument, otherwise a new buffer "
              "is created. `what` can either be an integer or a keyword. Returns the "
              "buffer with file contents. "
              "Values for `what`:\n\n"
              "* :all - read the whole file\n\n"
              "* :line - read up to and including the next newline character\n\n"
              "* n (integer) - read up to n bytes from the file") {
    bender_arity(argc, 2, 3);
    BenderFile *iof = bender_getabstract(argv, 0, &bender_file_type);
    if (iof->flags & BENDER_FILE_CLOSED) bender_panic("file is closed");
    BenderBuffer *buffer;
    if (argc == 2) {
        buffer = bender_buffer(0);
    } else {
        buffer = bender_getbuffer(argv, 2);
    }
    int32_t bufstart = buffer->count;
    if (bender_checktype(argv[1], BENDER_KEYWORD)) {
        const uint8_t *sym = bender_unwrap_keyword(argv[1]);
        if (!bender_cstrcmp(sym, "all")) {
            int32_t sizeBefore;
            do {
                sizeBefore = buffer->count;
                read_chunk(iof, buffer, 4096);
            } while (sizeBefore < buffer->count);
            /* Never return nil for :all */
            return bender_wrap_buffer(buffer);
        } else if (!bender_cstrcmp(sym, "line")) {
            for (;;) {
                int x = fgetc(iof->file);
                if (x != EOF) bender_buffer_push_u8(buffer, (uint8_t)x);
                if (x == EOF || x == '\n') break;
            }
        } else {
            bender_panicf("expected one of :all, :line, got %v", argv[1]);
        }
    } else {
        int32_t len = bender_getinteger(argv, 1);
        if (len < 0) bender_panic("expected positive integer");
        read_chunk(iof, buffer, len);
    }
    if (bufstart == buffer->count) return bender_wrap_nil();
    return bender_wrap_buffer(buffer);
}

/* Write bytes to a file */
BENDER_CORE_FN(cfun_io_fwrite,
              "(file/write f & bytes)",
              "Writes to a file `f`. Each value of `bytes` must be a "
              "string, buffer, symbol, or keyword. Returns the file.") {
    bender_arity(argc, 1, -1);
    BenderFile *iof = bender_getabstract(argv, 0, &bender_file_type);
    if (iof->flags & BENDER_FILE_CLOSED)
        bender_panic("file is closed");
    if (!(iof->flags & (BENDER_FILE_WRITE | BENDER_FILE_APPEND | BENDER_FILE_UPDATE)))
        bender_panic("file is not writeable");
    int32_t i;
    /* Verify all arguments before writing to file */
    for (i = 1; i < argc; i++)
        bender_getbytes(argv, i);
    for (i = 1; i < argc; i++) {
        BenderByteView view = bender_getbytes(argv, i);
        if (view.len) {
            if (!fwrite(view.bytes, view.len, 1, iof->file)) {
                bender_panic("error writing to file");
            }
            if (iof->file == stdout) bender_profile_note_stdout(view.bytes, view.len);
        }
    }
    return argv[0];
}

static void io_assert_writeable(BenderFile *iof) {
    if (iof->flags & BENDER_FILE_CLOSED)
        bender_panic("file is closed");
    if (!(iof->flags & (BENDER_FILE_WRITE | BENDER_FILE_APPEND | BENDER_FILE_UPDATE)))
        bender_panic("file is not writeable");
}

/* Flush the bytes in the file */
BENDER_CORE_FN(cfun_io_fflush,
              "(file/flush f)",
              "Flush any buffered bytes to the file system. In most files, writes are "
              "buffered for efficiency reasons. Returns the file handle.") {
    bender_fixarity(argc, 1);
    BenderFile *iof = bender_getabstract(argv, 0, &bender_file_type);
    io_assert_writeable(iof);
    if (fflush(iof->file))
        bender_panic("could not flush file");
    return argv[0];
}

#ifdef BENDER_WINDOWS
#define WEXITSTATUS(x) x
#endif

/* For closing files from C API */
int bender_file_close(BenderFile *file) {
    int ret = 0;
    if (!(file->flags & (BENDER_FILE_NOT_CLOSEABLE | BENDER_FILE_CLOSED))) {
        ret = fclose(file->file);
        file->flags |= BENDER_FILE_CLOSED;
        file->file = NULL; /* NULL dereference is easier to debug then other problems */
        return ret;
    }
    return 0;
}

/* Cleanup a file */
static int cfun_io_gc(void *p, size_t len) {
    (void) len;
    BenderFile *iof = (BenderFile *)p;
    bender_file_close(iof);
    return 0;
}

/* Close a file */
BENDER_CORE_FN(cfun_io_fclose,
              "(file/close f)",
              "Close a file and release all related resources. When you are "
              "done reading a file, close it to prevent a resource leak and let "
              "other processes read the file.") {
    bender_fixarity(argc, 1);
    BenderFile *iof = bender_getabstract(argv, 0, &bender_file_type);
    if (iof->flags & BENDER_FILE_CLOSED)
        return bender_wrap_nil();
    if (iof->flags & (BENDER_FILE_NOT_CLOSEABLE))
        bender_panic("file not closable");
    if (fclose(iof->file)) {
        iof->flags |= BENDER_FILE_NOT_CLOSEABLE;
        bender_panic("could not close file");
    }
    iof->flags |= BENDER_FILE_CLOSED;
    return bender_wrap_nil();
}

/* Seek a file */
BENDER_CORE_FN(cfun_io_fseek,
              "(file/seek f whence &opt n)",
              "Jump to a relative location in the file `f`. `whence` must be one of:\n\n"
              "* :cur - jump relative to the current file location\n\n"
              "* :set - jump relative to the beginning of the file\n\n"
              "* :end - jump relative to the end of the file\n\n"
              "By default, `n` is 0. Optionally a value `n` may be passed "
              "for the relative number of bytes to seek in the file. `n` may be a real "
              "number to handle large files of more than 4GB. Returns the file handle.") {
    bender_arity(argc, 2, 3);
    BenderFile *iof = bender_getabstract(argv, 0, &bender_file_type);
    if (iof->flags & BENDER_FILE_CLOSED)
        bender_panic("file is closed");
    int64_t offset = 0;
    int whence = SEEK_CUR;
    const uint8_t *whence_sym = bender_getkeyword(argv, 1);
    if (!bender_cstrcmp(whence_sym, "cur")) {
        whence = SEEK_CUR;
    } else if (!bender_cstrcmp(whence_sym, "set")) {
        whence = SEEK_SET;
    } else if (!bender_cstrcmp(whence_sym, "end")) {
        whence = SEEK_END;
    } else {
        bender_panicf("expected one of :cur, :set, :end, got %v", argv[1]);
    }
    if (argc == 3) {
        offset = (int64_t) bender_getinteger64(argv, 2);
    }
    if (fseek(iof->file, offset, whence)) bender_panic("error seeking file");
    return argv[0];
}

BENDER_CORE_FN(cfun_io_ftell,
              "(file/tell f)",
              "Get the current value of the file position for file `f`.") {
    bender_fixarity(argc, 1);
    BenderFile *iof = bender_getabstract(argv, 0, &bender_file_type);
    if (iof->flags & BENDER_FILE_CLOSED)
        bender_panic("file is closed");
    int64_t pos = ftell(iof->file);
    if (pos == -1) bender_panic("error getting position in file");
    return bender_wrap_number((double)pos);
}

static BenderMethod io_file_methods[] = {
    {"close", cfun_io_fclose},
    {"flush", cfun_io_fflush},
    {"read", cfun_io_fread},
    {"seek", cfun_io_fseek},
    {"tell", cfun_io_ftell},
    {"write", cfun_io_fwrite},
    {NULL, NULL}
};

static int io_file_get(void *p, Bender key, Bender *out) {
    (void) p;
    if (!bender_checktype(key, BENDER_KEYWORD))
        return 0;
    return bender_getmethod(bender_unwrap_keyword(key), io_file_methods, out);
}

static Bender io_file_next(void *p, Bender key) {
    (void) p;
    return bender_nextmethod(io_file_methods, key);
}

static void io_file_marshal(void *p, BenderMarshalContext *ctx) {
    BenderFile *iof = (BenderFile *)p;
    if (bender_marshal_flags(ctx) & BENDER_MARSHAL_UNSAFE) {
        bender_marshal_abstract(ctx, p);
        int fno = -1;
#ifdef BENDER_WINDOWS
        if (iof->flags & BENDER_FILE_NOT_CLOSEABLE) {
            fno = _fileno(iof->file);
        } else {
            fno = _dup(_fileno(iof->file));
        }
#else
        if (iof->flags & BENDER_FILE_NOT_CLOSEABLE) {
            fno = fileno(iof->file);
        } else {
#ifdef BENDER_PLAN9
            fno = dup(fileno(iof->file), -1);
#else
            fno = dup(fileno(iof->file));
#endif
        }
#endif
        bender_marshal_int(ctx, fno);
        bender_marshal_int(ctx, iof->flags);
        bender_marshal_size(ctx, iof->vbufsize);
    } else {
        bender_panic("cannot marshal file in safe mode");
    }
}

static void *io_file_unmarshal(BenderMarshalContext *ctx) {
    if (bender_unmarshal_flags(ctx) & BENDER_MARSHAL_UNSAFE) {
        BenderFile *iof = bender_unmarshal_abstract(ctx, sizeof(BenderFile));
        int32_t fd = bender_unmarshal_int(ctx);
        int32_t flags = bender_unmarshal_int(ctx);
        char fmt[4] = {0};
        int index = 0;
        if (flags & BENDER_FILE_READ) fmt[index++] = 'r';
        if (flags & BENDER_FILE_APPEND) {
            fmt[index++] = 'a';
        } else if (flags & BENDER_FILE_WRITE) {
            fmt[index++] = 'w';
        }
#ifdef BENDER_WINDOWS
        iof->file = _fdopen(fd, fmt);
#else
        iof->file = fdopen(fd, fmt);
#endif
        if (iof->file == NULL) {
            iof->flags = BENDER_FILE_CLOSED;
        } else {
            iof->flags = flags;
        }
        iof->vbufsize = bender_unmarshal_size(ctx);
        if (iof->vbufsize != BUFSIZ) {
            int result = setvbuf(iof->file, NULL, iof->vbufsize ? _IOFBF : _IONBF, iof->vbufsize);
            bender_assert(!result, "unmarshal setvbuf");
        }
        return iof;
    } else {
        bender_panic("cannot unmarshal file in safe mode");
    }
}

FILE *bender_dynfile(const char *name, FILE *def) {
    Bender x = bender_dyn(name);
    if (!bender_checktype(x, BENDER_ABSTRACT)) return def;
    void *abstract = bender_unwrap_abstract(x);
    if (bender_abstract_type(abstract) != &bender_file_type) return def;
    BenderFile *iofile = abstract;
    return iofile->file;
}

static Bender cfun_io_print_impl_x(int32_t argc, Bender *argv, int newline,
                                  FILE *dflt_file, int32_t offset, Bender x) {
    FILE *f;
    switch (bender_type(x)) {
        default:
            bender_panicf("cannot print to %v", x);
        case BENDER_BUFFER: {
            /* Special case buffer */
            BenderBuffer *buf = bender_unwrap_buffer(x);
            for (int32_t i = offset; i < argc; ++i) {
                bender_to_string_b(buf, argv[i]);
            }
            if (newline)
                bender_buffer_push_u8(buf, '\n');
            return bender_wrap_nil();
        }
        case BENDER_FUNCTION: {
            /* Special case function */
            BenderFunction *fun = bender_unwrap_function(x);
            BenderBuffer *buf = bender_buffer(0);
            /* Phase 2a root discipline: buf is held only in a C local
             * across bender_call, which can suspend the fiber and run
             * collections while suspended. Pin it explicitly. */
            bender_gcroot(bender_wrap_buffer(buf));
            for (int32_t i = offset; i < argc; ++i) {
                bender_to_string_b(buf, argv[i]);
            }
            if (newline)
                bender_buffer_push_u8(buf, '\n');
            Bender args[1] = { bender_wrap_buffer(buf) };
            bender_call(fun, 1, args);
            bender_gcunroot(bender_wrap_buffer(buf));
            return bender_wrap_nil();
        }
        case BENDER_NIL:
            f = dflt_file;
            if (f == NULL) bender_panic("cannot print to nil");
            break;
        case BENDER_ABSTRACT: {
            void *abstract = bender_unwrap_abstract(x);
            if (bender_abstract_type(abstract) != &bender_file_type)
                return bender_wrap_nil();
            BenderFile *iofile = abstract;
            io_assert_writeable(iofile);
            f = iofile->file;
            break;
        }
    }
    for (int32_t i = offset; i < argc; ++i) {
        int32_t len;
        const uint8_t *vstr;
        if (bender_checktype(argv[i], BENDER_BUFFER)) {
            BenderBuffer *b = bender_unwrap_buffer(argv[i]);
            vstr = b->data;
            len = b->count;
        } else {
            vstr = bender_to_string(argv[i]);
            len = bender_string_length(vstr);
        }
        if (len) {
            if (f == stdout) bender_profile_note_stdout(vstr, (size_t) len);
            if (1 != fwrite(vstr, len, 1, f)) {
                if (f == dflt_file) {
                    bender_panicf("cannot print %d bytes", len);
                } else {
                    bender_panicf("cannot print %d bytes to %v", len, x);
                }
            }
        }
    }
    if (newline)
        putc('\n', f);
    return bender_wrap_nil();
}

static Bender cfun_io_print_impl(int32_t argc, Bender *argv,
                                int newline, const char *name, FILE *dflt_file) {
    Bender x = bender_dyn(name);
    return cfun_io_print_impl_x(argc, argv, newline, dflt_file, 0, x);
}

BENDER_CORE_FN(cfun_io_print,
              "(print & xs)",
              "Print values to the console (standard out). Value are converted "
              "to strings if they are not already. After printing all values, a "
              "newline character is printed. Use the value of `(dyn :out stdout)` to determine "
              "what to push characters to. Expects `(dyn :out stdout)` to be either a core/file or "
              "a buffer. Returns nil.") {
    return cfun_io_print_impl(argc, argv, 1, "out", stdout);
}

BENDER_CORE_FN(cfun_io_prin,
              "(prin & xs)",
              "Same as `print`, but does not add trailing newline.") {
    return cfun_io_print_impl(argc, argv, 0, "out", stdout);
}

BENDER_CORE_FN(cfun_io_eprint,
              "(eprint & xs)",
              "Same as `print`, but uses `(dyn :err stderr)` instead of `(dyn :out stdout)`.") {
    return cfun_io_print_impl(argc, argv, 1, "err", stderr);
}

BENDER_CORE_FN(cfun_io_eprin,
              "(eprin & xs)",
              "Same as `prin`, but uses `(dyn :err stderr)` instead of `(dyn :out stdout)`.") {
    return cfun_io_print_impl(argc, argv, 0, "err", stderr);
}

BENDER_CORE_FN(cfun_io_xprint,
              "(xprint to & xs)",
              "Print to a file or other value explicitly (no dynamic bindings) with a trailing "
              "newline character. The value to print "
              "to is the first argument, and is otherwise the same as `print`. Returns nil.") {
    bender_arity(argc, 1, -1);
    return cfun_io_print_impl_x(argc, argv, 1, NULL, 1, argv[0]);
}

BENDER_CORE_FN(cfun_io_xprin,
              "(xprin to & xs)",
              "Print to a file or other value explicitly (no dynamic bindings). The value to print "
              "to is the first argument, and is otherwise the same as `prin`. Returns nil.") {
    bender_arity(argc, 1, -1);
    return cfun_io_print_impl_x(argc, argv, 0, NULL, 1, argv[0]);
}

static Bender cfun_io_printf_impl_x(int32_t argc, Bender *argv, int newline,
                                   FILE *dflt_file, int32_t offset, Bender x) {
    FILE *f;
    const char *fmt = bender_getcstring(argv, offset);
    switch (bender_type(x)) {
        default:
            bender_panicf("cannot print to %v", x);
        case BENDER_BUFFER: {
            /* Special case buffer */
            BenderBuffer *buf = bender_unwrap_buffer(x);
            bender_buffer_format(buf, fmt, offset, argc, argv);
            if (newline) bender_buffer_push_u8(buf, '\n');
            return bender_wrap_nil();
        }
        case BENDER_FUNCTION: {
            /* Special case function */
            BenderFunction *fun = bender_unwrap_function(x);
            BenderBuffer *buf = bender_buffer(0);
            bender_buffer_format(buf, fmt, offset, argc, argv);
            if (newline) bender_buffer_push_u8(buf, '\n');
            Bender args[1] = { bender_wrap_buffer(buf) };
            bender_call(fun, 1, args);
            return bender_wrap_nil();
        }
        case BENDER_NIL:
            f = dflt_file;
            if (f == NULL) bender_panic("cannot print to nil");
            break;
        case BENDER_ABSTRACT: {
            void *abstract = bender_unwrap_abstract(x);
            if (bender_abstract_type(abstract) != &bender_file_type)
                return bender_wrap_nil();
            BenderFile *iofile = abstract;
            if (iofile->flags & BENDER_FILE_CLOSED) {
                bender_panic("cannot print to closed file");
            }
            io_assert_writeable(iofile);
            f = iofile->file;
            break;
        }
    }
    BenderBuffer *buf = bender_buffer(10);
    bender_buffer_format(buf, fmt, offset, argc, argv);
    if (newline) bender_buffer_push_u8(buf, '\n');
    if (buf->count) {
        if (f == stdout) bender_profile_note_stdout(buf->data, (size_t) buf->count);
        if (1 != fwrite(buf->data, buf->count, 1, f)) {
            bender_panicf("could not print %d bytes to file", buf->count);
        }
    }
    /* Clear buffer to make things easier for GC */
    buf->count = 0;
    buf->capacity = 0;
    bender_free(buf->data);
    buf->data = NULL;
    return bender_wrap_nil();
}

static Bender cfun_io_printf_impl(int32_t argc, Bender *argv, int newline,
                                 const char *name, FILE *dflt_file) {
    bender_arity(argc, 1, -1);
    Bender x = bender_dyn(name);
    return cfun_io_printf_impl_x(argc, argv, newline, dflt_file, 0, x);

}

BENDER_CORE_FN(cfun_io_printf,
              "(printf fmt & xs)",
              "Prints output formatted as if with `(string/format fmt ;xs)` to `(dyn :out stdout)` with a trailing newline.") {
    return cfun_io_printf_impl(argc, argv, 1, "out", stdout);
}

BENDER_CORE_FN(cfun_io_prinf,
              "(prinf fmt & xs)",
              "Like `printf` but with no trailing newline.") {
    return cfun_io_printf_impl(argc, argv, 0, "out", stdout);
}

BENDER_CORE_FN(cfun_io_eprintf,
              "(eprintf fmt & xs)",
              "Prints output formatted as if with `(string/format fmt ;xs)` to `(dyn :err stderr)` with a trailing newline.") {
    return cfun_io_printf_impl(argc, argv, 1, "err", stderr);
}

BENDER_CORE_FN(cfun_io_eprinf,
              "(eprinf fmt & xs)",
              "Like `eprintf` but with no trailing newline.") {
    return cfun_io_printf_impl(argc, argv, 0, "err", stderr);
}

BENDER_CORE_FN(cfun_io_xprintf,
              "(xprintf to fmt & xs)",
              "Like `printf` but prints to an explicit file or value `to`. Returns nil.") {
    bender_arity(argc, 2, -1);
    return cfun_io_printf_impl_x(argc, argv, 1, NULL, 1, argv[0]);
}

BENDER_CORE_FN(cfun_io_xprinf,
              "(xprinf to fmt & xs)",
              "Like `prinf` but prints to an explicit file or value `to`. Returns nil.") {
    bender_arity(argc, 2, -1);
    return cfun_io_printf_impl_x(argc, argv, 0, NULL, 1, argv[0]);
}

static void bender_flusher(const char *name, FILE *dflt_file) {
    Bender x = bender_dyn(name);
    switch (bender_type(x)) {
        default:
            break;
        case BENDER_NIL:
            fflush(dflt_file);
            break;
        case BENDER_ABSTRACT: {
            void *abstract = bender_unwrap_abstract(x);
            if (bender_abstract_type(abstract) != &bender_file_type) break;
            BenderFile *iofile = abstract;
            fflush(iofile->file);
            break;
        }
    }
}

BENDER_CORE_FN(cfun_io_flush,
              "(flush)",
              "Flush `(dyn :out stdout)` if it is a file, otherwise do nothing.") {
    bender_fixarity(argc, 0);
    (void) argv;
    bender_flusher("out", stdout);
    return bender_wrap_nil();
}

BENDER_CORE_FN(cfun_io_eflush,
              "(eflush)",
              "Flush `(dyn :err stderr)` if it is a file, otherwise do nothing.") {
    bender_fixarity(argc, 0);
    (void) argv;
    bender_flusher("err", stderr);
    return bender_wrap_nil();
}

void bender_dynprintf(const char *name, FILE *dflt_file, const char *format, ...) {
    va_list args;
    va_start(args, format);
    BenderType xtype;
    Bender x;
    if (!name || name[0] == '\0') { /* Allow NULL or empty string to just use dflt_file directly */
        x = bender_wrap_nil();
        xtype = BENDER_NIL;
    } else {
        x = bender_dyn(name);
        xtype = bender_type(x);
    }
    switch (xtype) {
        default:
            /* Other values simply do nothing */
            break;
        case BENDER_NIL:
        case BENDER_ABSTRACT: {
            FILE *f = dflt_file;
            BenderBuffer buffer;
            int32_t len = 0;
            while (format[len]) len++;
            bender_buffer_init(&buffer, len);
            bender_formatbv(&buffer, format, args);
            if (xtype == BENDER_ABSTRACT) {
                void *abstract = bender_unwrap_abstract(x);
                if (bender_abstract_type(abstract) != &bender_file_type)
                    break;
                BenderFile *iofile = abstract;
                io_assert_writeable(iofile);
                f = iofile->file;
            }
            if (f == stdout) bender_profile_note_stdout(buffer.data, (size_t) buffer.count);
            fwrite(buffer.data, buffer.count, 1, f);
            bender_buffer_deinit(&buffer);
            break;
        }
        case BENDER_FUNCTION: {
            BenderFunction *fun = bender_unwrap_function(x);
            int32_t len = 0;
            while (format[len]) len++;
            BenderBuffer *buf = bender_buffer(len);
            bender_formatbv(buf, format, args);
            Bender args[1] = { bender_wrap_buffer(buf) };
            bender_call(fun, 1, args);
            break;
        }
        case BENDER_BUFFER:
            bender_formatbv(bender_unwrap_buffer(x), format, args);
            break;
    }
    va_end(args);
    return;
}

/* C API */

BenderFile *bender_getjfile(const Bender *argv, int32_t n) {
    return bender_getabstract(argv, n, &bender_file_type);
}

FILE *bender_getfile(const Bender *argv, int32_t n, int32_t *flags) {
    BenderFile *iof = bender_getabstract(argv, n, &bender_file_type);
    if (NULL != flags) *flags = iof->flags;
    return iof->file;
}

BenderFile *bender_makejfile(FILE *f, int32_t flags) {
    return makef(f, flags, BUFSIZ);
}

Bender bender_makefile(FILE *f, int32_t flags) {
    return bender_wrap_abstract(makef(f, flags, BUFSIZ));
}

BenderAbstract bender_checkfile(Bender j) {
    return bender_checkabstract(j, &bender_file_type);
}

FILE *bender_unwrapfile(Bender j, int32_t *flags) {
    BenderFile *iof = bender_unwrap_abstract(j);
    if (NULL != flags) *flags = iof->flags;
    return iof->file;
}

/* Module entry point */
void bender_lib_io(BenderTable *env) {
    BenderRegExt io_cfuns[] = {
        BENDER_CORE_REG("print", cfun_io_print),
        BENDER_CORE_REG("prin", cfun_io_prin),
        BENDER_CORE_REG("printf", cfun_io_printf),
        BENDER_CORE_REG("prinf", cfun_io_prinf),
        BENDER_CORE_REG("eprin", cfun_io_eprin),
        BENDER_CORE_REG("eprint", cfun_io_eprint),
        BENDER_CORE_REG("eprintf", cfun_io_eprintf),
        BENDER_CORE_REG("eprinf", cfun_io_eprinf),
        BENDER_CORE_REG("xprint", cfun_io_xprint),
        BENDER_CORE_REG("xprin", cfun_io_xprin),
        BENDER_CORE_REG("xprintf", cfun_io_xprintf),
        BENDER_CORE_REG("xprinf", cfun_io_xprinf),
        BENDER_CORE_REG("flush", cfun_io_flush),
        BENDER_CORE_REG("eflush", cfun_io_eflush),
        BENDER_CORE_REG("file/temp", cfun_io_temp),
        BENDER_CORE_REG("file/open", cfun_io_fopen),
        BENDER_CORE_REG("file/close", cfun_io_fclose),
        BENDER_CORE_REG("file/read", cfun_io_fread),
        BENDER_CORE_REG("file/write", cfun_io_fwrite),
        BENDER_CORE_REG("file/flush", cfun_io_fflush),
        BENDER_CORE_REG("file/seek", cfun_io_fseek),
        BENDER_CORE_REG("file/tell", cfun_io_ftell),
        BENDER_REG_END
    };
    bender_core_cfuns_ext(env, NULL, io_cfuns);
    bender_register_abstract_type(&bender_file_type);
    int default_flags = BENDER_FILE_NOT_CLOSEABLE | BENDER_FILE_SERIALIZABLE;
    /* stdout */
    BENDER_CORE_DEF(env, "stdout",
                   bender_makefile(stdout, BENDER_FILE_APPEND | default_flags),
                   "The standard output file.");
    /* stderr */
    BENDER_CORE_DEF(env, "stderr",
                   bender_makefile(stderr, BENDER_FILE_APPEND | default_flags),
                   "The standard error file.");
    /* stdin */
    BENDER_CORE_DEF(env, "stdin",
                   bender_makefile(stdin, BENDER_FILE_READ | default_flags),
                   "The standard input file.");

}
