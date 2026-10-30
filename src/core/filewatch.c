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
#include "util.h"
#endif

#ifdef BENDER_EV
#ifdef BENDER_FILEWATCH

#ifdef BENDER_LINUX
#include <sys/inotify.h>
#include <unistd.h>
#endif

#ifdef BENDER_WINDOWS
#include <windows.h>
#endif

#if defined(BENDER_APPLE) || defined(BENDER_BSD)
#include <sys/event.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#endif

typedef struct {
    const char *name;
    uint32_t flag;
} BenderWatchFlagName;

typedef struct {
#ifndef BENDER_WINDOWS
    BenderStream *stream;
#endif
    BenderTable *watch_descriptors;
    BenderChannel *channel;
    uint32_t default_flags;
    int is_watching;
} BenderWatcher;

#ifdef BENDER_LINUX

#include <sys/inotify.h>
#include <unistd.h>

static const BenderWatchFlagName watcher_flags_linux[] = {
    {"access", IN_ACCESS},
    {"all", IN_ALL_EVENTS},
    {"attrib", IN_ATTRIB},
    {"close-nowrite", IN_CLOSE_NOWRITE},
    {"close-write", IN_CLOSE_WRITE},
    {"create", IN_CREATE},
    {"delete", IN_DELETE},
    {"delete-self", IN_DELETE_SELF},
    {"ignored", IN_IGNORED},
    {"modify", IN_MODIFY},
    {"move-self", IN_MOVE_SELF},
    {"moved-from", IN_MOVED_FROM},
    {"moved-to", IN_MOVED_TO},
    {"open", IN_OPEN},
    {"q-overflow", IN_Q_OVERFLOW},
    {"unmount", IN_UNMOUNT},
};

static uint32_t decode_watch_flags(const Bender *options, int32_t n) {
    uint32_t flags = 0;
    for (int32_t i = 0; i < n; i++) {
        if (!(bender_checktype(options[i], BENDER_KEYWORD))) {
            bender_panicf("expected keyword, got %v", options[i]);
        }
        BenderKeyword keyw = bender_unwrap_keyword(options[i]);
        const BenderWatchFlagName *result = bender_strbinsearch(watcher_flags_linux,
            sizeof(watcher_flags_linux) / sizeof(BenderWatchFlagName),
            sizeof(BenderWatchFlagName),
            keyw);
        if (!result) {
            bender_panicf("unknown linux flag %v", options[i]);
        }
        flags |= result->flag;
    }
    return flags;
}

static void bender_watcher_init(BenderWatcher *watcher, BenderChannel *channel, uint32_t default_flags) {
    int fd;
    do {
        fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    } while (fd == -1 && errno == EINTR);
    if (fd == -1) {
        bender_panicv(bender_ev_lasterr());
    }
    watcher->watch_descriptors = bender_table(0);
    watcher->channel = channel;
    watcher->default_flags = default_flags;
    watcher->is_watching = 0;
    watcher->stream = bender_stream(fd, BENDER_STREAM_READABLE, NULL);
}

static void bender_watcher_add(BenderWatcher *watcher, const char *path, uint32_t flags) {
    if (watcher->stream == NULL) bender_panic("watcher closed");
    int result;
    do {
        result = inotify_add_watch(watcher->stream->handle, path, flags);
    } while (result == -1 && errno == EINTR);
    if (result == -1) {
        bender_panicv(bender_ev_lasterr());
    }
    Bender name = bender_cstringv(path);
    Bender wd = bender_wrap_integer(result);
    bender_table_put(watcher->watch_descriptors, name, wd);
    bender_table_put(watcher->watch_descriptors, wd, name);
}

static void bender_watcher_remove(BenderWatcher *watcher, const char *path) {
    if (watcher->stream == NULL) bender_panic("watcher closed");
    Bender pathv = bender_cstringv(path);
    Bender check = bender_table_get(watcher->watch_descriptors, pathv);
    if (!bender_checktype(check, BENDER_NUMBER)) {
        bender_panic("bad watch descriptor");
    }
    int watch_handle = bender_unwrap_integer(check);
    int result;
    do {
        result = inotify_rm_watch(watcher->stream->handle, watch_handle);
    } while (result != -1 && errno == EINTR);
    if (result == -1) {
        bender_panicv(bender_ev_lasterr());
    }
    /*
    bender_table_put(watcher->watch_descriptors, pathv, bender_wrap_nil());
    bender_table_put(watcher->watch_descriptors, bender_wrap_integer(watch_handle), bender_wrap_nil());
    */
}

static void watcher_callback_read(BenderFiber *fiber, BenderAsyncEvent event) {
    BenderStream *stream = fiber->ev_stream;
    BenderWatcher *watcher = *((BenderWatcher **) fiber->ev_state);
    char buf[1024];
    switch (event) {
        default:
            break;
        case BENDER_ASYNC_EVENT_MARK:
            bender_mark(bender_wrap_abstract(watcher));
            break;
        case BENDER_ASYNC_EVENT_CLOSE:
            bender_schedule(fiber, bender_wrap_nil());
            bender_async_end(fiber);
            break;
        case BENDER_ASYNC_EVENT_ERR: {
            bender_schedule(fiber, bender_wrap_nil());
            bender_async_end(fiber);
            break;
        }
    read_more:
        case BENDER_ASYNC_EVENT_HUP:
        case BENDER_ASYNC_EVENT_INIT:
        case BENDER_ASYNC_EVENT_READ: {
            Bender name = bender_wrap_nil();

            /* Assumption - read will never return partial events *
             * From documentation:
             *
             * The behavior when the buffer given to read(2) is too small to
             * return information about the next event depends on the kernel
             * version: before Linux 2.6.21, read(2) returns 0; since Linux
             * 2.6.21, read(2) fails with the error EINVAL.  Specifying a buffer
             * of size
             *
             *     sizeof(struct inotify_event) + NAME_MAX + 1
             *
             * will be sufficient to read at least one event. */
            ssize_t nread;
            do {
                nread = read(stream->handle, buf, sizeof(buf));
            } while (nread == -1 && errno == EINTR);

            /* Check for errors - special case errors that can just be waited on to fix */
            if (nread == -1) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) {
                    break;
                }
                bender_cancel(fiber, bender_ev_lasterr());
                fiber->ev_state = NULL;
                bender_async_end(fiber);
                break;
            }
            if (nread < (ssize_t) sizeof(struct inotify_event)) break;

            /* Iterate through all events read from the buffer */
            char *cursor = buf;
            while (cursor < buf + nread) {
                struct inotify_event inevent;
                memcpy(&inevent, cursor, sizeof(inevent));
                cursor += sizeof(inevent);
                /* Read path of inevent */
                if (inevent.len) {
                    name = bender_cstringv(cursor);
                    cursor += inevent.len;
                }

                /* Got an event */
                Bender path = bender_table_get(watcher->watch_descriptors, bender_wrap_integer(inevent.wd));
                BenderKV *event = bender_struct_begin(6);
                bender_struct_put(event, bender_ckeywordv("wd"), bender_wrap_integer(inevent.wd));
                bender_struct_put(event, bender_ckeywordv("wd-path"), path);
                if (bender_checktype(name, BENDER_NIL)) {
                    /* We were watching a file directly, so path is the full path. Split into dirname / basename */
                    BenderString spath = bender_unwrap_string(path);
                    const uint8_t *cursor = spath + bender_string_length(spath);
                    const uint8_t *cursor_end = cursor;
                    while (cursor > spath && cursor[0] != '/') {
                        cursor--;
                    }
                    if (cursor == spath) {
                        bender_struct_put(event, bender_ckeywordv("dir-name"), path);
                        bender_struct_put(event, bender_ckeywordv("file-name"), name);
                    } else {
                        bender_struct_put(event, bender_ckeywordv("dir-name"), bender_wrap_string(bender_string(spath, (cursor - spath))));
                        bender_struct_put(event, bender_ckeywordv("file-name"), bender_wrap_string(bender_string(cursor + 1, (cursor_end - cursor - 1))));
                    }
                } else {
                    bender_struct_put(event, bender_ckeywordv("dir-name"), path);
                    bender_struct_put(event, bender_ckeywordv("file-name"), name);
                }
                bender_struct_put(event, bender_ckeywordv("cookie"), bender_wrap_integer(inevent.cookie));
                Bender etype = bender_ckeywordv("type");
                const BenderWatchFlagName *wfn_end = watcher_flags_linux + sizeof(watcher_flags_linux) / sizeof(watcher_flags_linux[0]);
                for (const BenderWatchFlagName *wfn = watcher_flags_linux; wfn < wfn_end; wfn++) {
                    if ((inevent.mask & wfn->flag) == wfn->flag) bender_struct_put(event, etype, bender_ckeywordv(wfn->name));
                }
                Bender eventv = bender_wrap_struct(bender_struct_end(event));

                bender_channel_give(watcher->channel, eventv);
            }

            /* Read some more if possible */
            goto read_more;
        }
        break;
    }
}

static void bender_watcher_listen(BenderWatcher *watcher) {
    if (watcher->is_watching) bender_panic("already watching");
    watcher->is_watching = 1;
    BenderFunction *thunk = bender_thunk_delay(bender_wrap_nil());
    BenderFiber *fiber = bender_fiber(thunk, 64, 0, NULL);
    BenderWatcher **state = bender_malloc(sizeof(BenderWatcher *)); /* Gross */
    *state = watcher;
    bender_async_start_fiber(fiber, watcher->stream, BENDER_ASYNC_LISTEN_READ, watcher_callback_read, state);
    bender_gcroot(bender_wrap_abstract(watcher));
}

static void bender_watcher_unlisten(BenderWatcher *watcher) {
    if (!watcher->is_watching) return;
    watcher->is_watching = 0;
    bender_stream_close(watcher->stream);
    bender_gcunroot(bender_wrap_abstract(watcher));
}

#elif BENDER_WINDOWS

#define WATCHFLAG_RECURSIVE 0x100000u

static const BenderWatchFlagName watcher_flags_windows[] = {
    {
        "all",
        FILE_NOTIFY_CHANGE_ATTRIBUTES |
        FILE_NOTIFY_CHANGE_CREATION |
        FILE_NOTIFY_CHANGE_DIR_NAME |
        FILE_NOTIFY_CHANGE_FILE_NAME |
        FILE_NOTIFY_CHANGE_LAST_ACCESS |
        FILE_NOTIFY_CHANGE_LAST_WRITE |
        FILE_NOTIFY_CHANGE_SECURITY |
        FILE_NOTIFY_CHANGE_SIZE |
        WATCHFLAG_RECURSIVE
    },
    {"attributes", FILE_NOTIFY_CHANGE_ATTRIBUTES},
    {"creation", FILE_NOTIFY_CHANGE_CREATION},
    {"dir-name", FILE_NOTIFY_CHANGE_DIR_NAME},
    {"file-name", FILE_NOTIFY_CHANGE_FILE_NAME},
    {"last-access", FILE_NOTIFY_CHANGE_LAST_ACCESS},
    {"last-write", FILE_NOTIFY_CHANGE_LAST_WRITE},
    {"recursive", WATCHFLAG_RECURSIVE},
    {"security", FILE_NOTIFY_CHANGE_SECURITY},
    {"size", FILE_NOTIFY_CHANGE_SIZE},
};

static uint32_t decode_watch_flags(const Bender *options, int32_t n) {
    uint32_t flags = 0;
    for (int32_t i = 0; i < n; i++) {
        if (!(bender_checktype(options[i], BENDER_KEYWORD))) {
            bender_panicf("expected keyword, got %v", options[i]);
        }
        BenderKeyword keyw = bender_unwrap_keyword(options[i]);
        const BenderWatchFlagName *result = bender_strbinsearch(watcher_flags_windows,
            sizeof(watcher_flags_windows) / sizeof(BenderWatchFlagName),
            sizeof(BenderWatchFlagName),
            keyw);
        if (!result) {
            bender_panicf("unknown windows filewatch flag %v", options[i]);
        }
        flags |= result->flag;
    }
    return flags;
}

static void bender_watcher_init(BenderWatcher *watcher, BenderChannel *channel, uint32_t default_flags) {
    watcher->watch_descriptors = bender_table(0);
    watcher->channel = channel;
    watcher->default_flags = default_flags;
    watcher->is_watching = 0;
}

/* Since the file info padding includes embedded file names, we want to include more space for data.
 * We also need to handle manually calculating changes if path names are too long, but ideally just avoid
 * that scenario as much as possible */
#define FILE_INFO_PADDING (4096 * 4)

typedef struct {
    BenderOverlapped overlapped;
    BenderStream *stream;
    BenderWatcher *watcher;
    BenderFiber *fiber;
    BenderString dir_path;
    uint32_t flags;
    uint64_t buf[FILE_INFO_PADDING / sizeof(uint64_t)]; /* Ensure alignment */
} OverlappedWatch;

#define NotifyChange FILE_NOTIFY_INFORMATION

static void read_dir_changes(OverlappedWatch *ow) {
    BOOL result = ReadDirectoryChangesW(ow->stream->handle,
                                        (NotifyChange *) ow->buf,
                                        FILE_INFO_PADDING,
                                        (ow->flags & WATCHFLAG_RECURSIVE) ? TRUE : FALSE,
                                        ow->flags & ~WATCHFLAG_RECURSIVE,
                                        NULL,
                                        (OVERLAPPED *) ow,
                                        NULL);
    if (!result) {
        bender_panicv(bender_ev_lasterr());
    }
}

static const char *watcher_actions_windows[] = {
    "unknown",
    "added",
    "removed",
    "modified",
    "renamed-old",
    "renamed-new",
};

static void watcher_callback_read(BenderFiber *fiber, BenderAsyncEvent event) {
    OverlappedWatch *ow = (OverlappedWatch *) fiber->ev_state;
    BenderWatcher *watcher = ow->watcher;
    switch (event) {
        default:
            break;
        case BENDER_ASYNC_EVENT_INIT:
            bender_async_in_flight(fiber);
            break;
        case BENDER_ASYNC_EVENT_MARK:
            bender_mark(bender_wrap_abstract(ow->stream));
            bender_mark(bender_wrap_fiber(ow->fiber));
            bender_mark(bender_wrap_abstract(watcher));
            bender_mark(bender_wrap_string(ow->dir_path));
            break;
        case BENDER_ASYNC_EVENT_CLOSE:
            bender_table_remove(ow->watcher->watch_descriptors, bender_wrap_string(ow->dir_path));
            break;
        case BENDER_ASYNC_EVENT_ERR:
        case BENDER_ASYNC_EVENT_FAILED:
            bender_stream_close(ow->stream);
            break;
        case BENDER_ASYNC_EVENT_COMPLETE: {
            if (!watcher->is_watching) {
                bender_stream_close(ow->stream);
                break;
            }

            NotifyChange *fni = (NotifyChange *) ow->buf;

            while (1) {
                /* Got an event */

                /* Extract name */
                Bender filename;
                if (fni->FileNameLength) {
                    int32_t nbytes = (int32_t) WideCharToMultiByte(CP_UTF8, 0, fni->FileName, fni->FileNameLength / sizeof(wchar_t), NULL, 0, NULL, NULL);
                    bender_assert(nbytes, "bad utf8 path");
                    uint8_t *into = bender_string_begin(nbytes);
                    WideCharToMultiByte(CP_UTF8, 0, fni->FileName, fni->FileNameLength / sizeof(wchar_t), (char *) into, nbytes, NULL, NULL);
                    filename = bender_wrap_string(bender_string_end(into));
                } else {
                    filename = bender_cstringv("");
                }

                BenderKV *event = bender_struct_begin(3);
                bender_struct_put(event, bender_ckeywordv("type"), bender_ckeywordv(watcher_actions_windows[fni->Action]));
                bender_struct_put(event, bender_ckeywordv("file-name"), filename);
                bender_struct_put(event, bender_ckeywordv("dir-name"), bender_wrap_string(ow->dir_path));
                Bender eventv = bender_wrap_struct(bender_struct_end(event));

                bender_channel_give(watcher->channel, eventv);

                /* Next event */
                if (!fni->NextEntryOffset) break;
                fni = (NotifyChange *)((char *)fni + fni->NextEntryOffset);
            }

            /* Make another call to read directory changes */
            read_dir_changes(ow);
            bender_async_in_flight(fiber);
        }
        break;
    }
}

static void start_listening_ow(OverlappedWatch *ow) {
    read_dir_changes(ow);
    BenderStream *stream = ow->stream;
    BenderFunction *thunk = bender_thunk_delay(bender_wrap_nil());
    BenderFiber *fiber = bender_fiber(thunk, 64, 0, NULL);
    fiber->supervisor_channel = bender_root_fiber()->supervisor_channel;
    ow->fiber = fiber;
    bender_async_start_fiber(fiber, stream, BENDER_ASYNC_LISTEN_READ, watcher_callback_read, ow);
}

static void bender_watcher_add(BenderWatcher *watcher, const char *path, uint32_t flags) {
    HANDLE handle = CreateFileA(path,
                                FILE_LIST_DIRECTORY | GENERIC_READ,
                                FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                NULL,
                                OPEN_EXISTING,
                                FILE_FLAG_OVERLAPPED | FILE_FLAG_BACKUP_SEMANTICS,
                                NULL);
    if (handle == INVALID_HANDLE_VALUE) {
        bender_panicv(bender_ev_lasterr());
    }
    BenderStream *stream = bender_stream(handle, BENDER_STREAM_READABLE, NULL);
    OverlappedWatch *ow = bender_malloc(sizeof(OverlappedWatch));
    memset(ow, 0, sizeof(OverlappedWatch));
    ow->stream = stream;
    ow->dir_path = bender_cstring(path);
    ow->fiber = NULL;
    Bender pathv = bender_wrap_string(ow->dir_path);
    ow->flags = flags | watcher->default_flags;
    ow->watcher = watcher;
    ow->overlapped.as.overlapped.hEvent = CreateEvent(NULL, FALSE, 0, NULL); /* Do we need this */
    Bender streamv = bender_wrap_pointer(ow);
    bender_table_put(watcher->watch_descriptors, pathv, streamv);
    if (watcher->is_watching) {
        start_listening_ow(ow);
    }
}

static void bender_watcher_remove(BenderWatcher *watcher, const char *path) {
    Bender pathv = bender_cstringv(path);
    Bender streamv = bender_table_get(watcher->watch_descriptors, pathv);
    if (bender_checktype(streamv, BENDER_NIL)) {
        bender_panicf("path %v is not being watched", pathv);
    }
    bender_table_remove(watcher->watch_descriptors, pathv);
    OverlappedWatch *ow = bender_unwrap_pointer(streamv);
    bender_stream_close(ow->stream);
}

static void bender_watcher_listen(BenderWatcher *watcher) {
    if (watcher->is_watching) bender_panic("already watching");
    watcher->is_watching = 1;
    for (int32_t i = 0; i < watcher->watch_descriptors->capacity; i++) {
        const BenderKV *kv = watcher->watch_descriptors->data + i;
        if (!bender_checktype(kv->value, BENDER_POINTER)) continue;
        OverlappedWatch *ow = bender_unwrap_pointer(kv->value);
        start_listening_ow(ow);
    }
    bender_gcroot(bender_wrap_abstract(watcher));
}

static void bender_watcher_unlisten(BenderWatcher *watcher) {
    if (!watcher->is_watching) return;
    watcher->is_watching = 0;
    for (int32_t i = 0; i < watcher->watch_descriptors->capacity; i++) {
        const BenderKV *kv = watcher->watch_descriptors->data + i;
        if (!bender_checktype(kv->value, BENDER_POINTER)) continue;
        OverlappedWatch *ow = bender_unwrap_pointer(kv->value);
        bender_stream_close(ow->stream);
    }
    bender_table_clear(watcher->watch_descriptors);
    bender_gcunroot(bender_wrap_abstract(watcher));
}

#elif defined(BENDER_APPLE) || defined(BENDER_BSD)

/* kqueue implementation */

/* Cribbed from ev.c */
#define EV_SETx(ev, a, b, c, d, e, f) EV_SET((ev), (a), (b), (c), (d), (e), ((__typeof__((ev)->udata))(f)))

/* Different BSDs define different NOTE_* constants for different kinds of events. Use ifdef to
   determine when they are available (assuming they are defines and not enums */
static const BenderWatchFlagName watcher_flags_kqueue[] = {
    {
        "all", NOTE_ATTRIB | NOTE_DELETE | NOTE_EXTEND | NOTE_RENAME | NOTE_REVOKE | NOTE_WRITE | NOTE_LINK
#ifdef NOTE_CLOSE
        | NOTE_CLOSE
#endif
#ifdef NOTE_CLOSE_WRITE
        | NOTE_CLOSE_WRITE
#endif
#ifdef NOTE_OPEN
        | NOTE_OPEN
#endif
#ifdef NOTE_READ
        | NOTE_READ
#endif
#ifdef NOTE_FUNLOCK
        | NOTE_FUNLOCK
#endif
#ifdef NOTE_TRUNCATE
        | NOTE_TRUNCATE
#endif
    },
    {"attrib", NOTE_ATTRIB},
#ifdef NOTE_CLOSE
    {"close", NOTE_CLOSE},
#endif
#ifdef NOTE_CLOSE_WRITE
    {"close-write", NOTE_CLOSE_WRITE},
#endif
    {"delete", NOTE_DELETE},
    {"extend", NOTE_EXTEND},
#ifdef NOTE_FUNLOCK
    {"funlock", NOTE_FUNLOCK},
#endif
    {"link", NOTE_LINK},
#ifdef NOTE_OPEN
    {"open", NOTE_OPEN},
#endif
#ifdef NOTE_READ
    {"read", NOTE_READ},
#endif
    {"rename", NOTE_RENAME},
    {"revoke", NOTE_REVOKE},
#ifdef NOTE_TRUNCATE
    {"truncate", NOTE_TRUNCATE},
#endif
    {"write", NOTE_WRITE},
};

static uint32_t decode_watch_flags(const Bender *options, int32_t n) {
    uint32_t flags = 0;
    for (int32_t i = 0; i < n; i++) {
        if (!(bender_checktype(options[i], BENDER_KEYWORD))) {
            bender_panicf("expected keyword, got %v", options[i]);
        }
        BenderKeyword keyw = bender_unwrap_keyword(options[i]);
        const BenderWatchFlagName *result = bender_strbinsearch(watcher_flags_kqueue,
            sizeof(watcher_flags_kqueue) / sizeof(BenderWatchFlagName),
            sizeof(BenderWatchFlagName),
            keyw);
        if (!result) {
            bender_panicf("unknown bsd flag %v", options[i]);
        }
        flags |= result->flag;
    }
    return flags;
}

static void bender_watcher_init(BenderWatcher *watcher, BenderChannel *channel, uint32_t default_flags) {
    int kq = kqueue();
    watcher->watch_descriptors = bender_table(0);
    watcher->channel = channel;
    watcher->default_flags = default_flags;
    watcher->is_watching = 0;
    watcher->stream = bender_stream(kq, BENDER_STREAM_READABLE, NULL);
    bender_stream_level_triggered(watcher->stream);
}

static void bender_watcher_add(BenderWatcher *watcher, const char *path, uint32_t flags) {
    if (watcher->stream == NULL) bender_panic("watcher closed");
    int kq = watcher->stream->handle;
    struct kevent kev = {0};
    /* Get file descriptor for path */
    int file_fd;
    do {
        file_fd = open(path, O_RDONLY);
    } while (file_fd == -1 && errno == EINTR);
    if (file_fd == -1) {
        bender_panicf("failed to open: %v", bender_ev_lasterr());
    }
    /* Watch for EVFILT_VNODE on the file descriptor */
    EV_SETx(&kev, file_fd, EVFILT_VNODE, EV_ADD | EV_ENABLE | EV_CLEAR, flags, 0, NULL);
    int status;
    do {
        status = kevent(kq, &kev, 1, NULL, 0, NULL);
    } while (status == -1 && errno == EINTR);
    if (status == -1) {
        close(file_fd);
        bender_panicf("failed to listen: %v", bender_ev_lasterr());
    }
    /* Bookkeeping */
    Bender name = bender_cstringv(path);
    Bender wd = bender_wrap_integer(file_fd);
    bender_table_put(watcher->watch_descriptors, name, wd);
    bender_table_put(watcher->watch_descriptors, wd, name);
}

static void bender_watcher_remove(BenderWatcher *watcher, const char *path) {
    if (watcher->stream == NULL) bender_panic("watcher closed");
    Bender pathv = bender_cstringv(path);
    Bender check = bender_table_get(watcher->watch_descriptors, pathv);
    if (!bender_checktype(check, BENDER_NUMBER)) {
        bender_panic("bad watch descriptor");
    }
    /* Closing the file descriptor will also remove it from the kqueue */
    int wd = bender_unwrap_integer(check);
    int result;
    do {
        result = close(wd);
    } while (result != -1 && errno == EINTR);
    if (result == -1) {
        bender_panicv(bender_ev_lasterr());
    }
    bender_table_put(watcher->watch_descriptors, pathv, bender_wrap_nil());
    bender_table_put(watcher->watch_descriptors, bender_wrap_integer(wd), bender_wrap_nil());
}

typedef struct {
    BenderWatcher *watcher;
    uint32_t cookie;
} KqueueWatcherState;

static void watcher_callback_read(BenderFiber *fiber, BenderAsyncEvent event) {
    BenderStream *stream = fiber->ev_stream;
    KqueueWatcherState *state = fiber->ev_state;
    BenderWatcher *watcher = state->watcher;
    switch (event) {
        case BENDER_ASYNC_EVENT_MARK:
            bender_mark(bender_wrap_abstract(watcher));
            break;
        case BENDER_ASYNC_EVENT_CLOSE:
            bender_schedule(fiber, bender_wrap_nil());
            bender_async_end(fiber);
            break;
        case BENDER_ASYNC_EVENT_ERR: {
            bender_schedule(fiber, bender_wrap_nil());
            bender_async_end(fiber);
            break;
        }
        case BENDER_ASYNC_EVENT_HUP:
        case BENDER_ASYNC_EVENT_INIT:
            break;
        case BENDER_ASYNC_EVENT_READ: {
            /* Pump events from the sub kqueue */
            const int num_events = 512; /* Extra will be pumped after another event loop rotation. */
            struct kevent events[num_events];
            int kq = stream->handle;
            int status;
            do {
                status = kevent(kq, NULL, 0, events, num_events, NULL);
            } while (status == -1 && errno == EINTR);
            if (status == -1) {
                bender_schedule(fiber, bender_wrap_nil());
                bender_async_end(fiber);
                break;
            }
            for (int i = 0; i < status; i++) {
                state->cookie += 6700417;
                struct kevent kev = events[i];
                /* TODO - avoid stat call here, maybe just when adding listener? */
                struct stat stat_buf = {0};
                int status;
                do {
                    status = fstat(kev.ident, &stat_buf);
                } while (status == -1 && errno == EINTR);
                if (status == -1) continue;
                int is_dir = S_ISDIR(stat_buf.st_mode);
                Bender ident = bender_wrap_integer(kev.ident);
                Bender path = bender_table_get(watcher->watch_descriptors, ident);
                for (unsigned int j = 1; j < (sizeof(watcher_flags_kqueue) / sizeof(watcher_flags_kqueue[0])); j++) {
                    uint32_t flagcheck = watcher_flags_kqueue[j].flag;
                    if (kev.fflags & flagcheck) {
                        BenderKV *event = bender_struct_begin(6);
                        bender_struct_put(event, bender_ckeywordv("wd"), ident);
                        bender_struct_put(event, bender_ckeywordv("wd-path"), path);
                        bender_struct_put(event, bender_ckeywordv("cookie"), bender_wrap_number((double) state->cookie));
                        bender_struct_put(event, bender_ckeywordv("type"), bender_ckeywordv(watcher_flags_kqueue[j].name));
                        if (is_dir) {
                            /* Pass in directly */
                            bender_struct_put(event, bender_ckeywordv("file-name"), bender_cstringv(""));
                            bender_struct_put(event, bender_ckeywordv("dir-name"), path);
                        } else {
                            /* Split path */
                            BenderString spath = bender_unwrap_string(path);
                            const uint8_t *cursor = spath + bender_string_length(spath);
                            const uint8_t *cursor_end = cursor;
                            while (cursor > spath && cursor[0] != '/') {
                                cursor--;
                            }
                            if (cursor == spath) {
                                /* No path separators */
                                bender_struct_put(event, bender_ckeywordv("dir-name"), bender_cstringv("."));
                                bender_struct_put(event, bender_ckeywordv("file-name"), bender_wrap_string(spath));
                            } else {
                                /* Found path separator */
                                bender_struct_put(event, bender_ckeywordv("dir-name"), bender_wrap_string(bender_string(spath, (cursor - spath))));
                                bender_struct_put(event, bender_ckeywordv("file-name"), bender_wrap_string(bender_string(cursor + 1, (cursor_end - cursor - 1))));
                            }
                        }
                        Bender eventv = bender_wrap_struct(bender_struct_end(event));
                        bender_channel_give(watcher->channel, eventv);
                    }
                }
            }
            break;
        }
        default:
            break;
    }
}

static void bender_watcher_listen(BenderWatcher *watcher) {
    if (watcher->is_watching) bender_panic("already watching");
    watcher->is_watching = 1;
    BenderFunction *thunk = bender_thunk_delay(bender_wrap_nil());
    BenderFiber *fiber = bender_fiber(thunk, 64, 0, NULL);
    KqueueWatcherState *state = bender_malloc(sizeof(KqueueWatcherState));
    state->watcher = watcher;
    bender_async_start_fiber(fiber, watcher->stream, BENDER_ASYNC_LISTEN_READ, watcher_callback_read, state);
    bender_gcroot(bender_wrap_abstract(watcher));
}

static void bender_watcher_unlisten(BenderWatcher *watcher) {
    if (!watcher->is_watching) return;
    watcher->is_watching = 0;
    bender_stream_close(watcher->stream);
    bender_gcunroot(bender_wrap_abstract(watcher));
}

#else

/* Default implementation */

static uint32_t decode_watch_flags(const Bender *options, int32_t n) {
    (void) options;
    (void) n;
    return 0;
}

static void bender_watcher_init(BenderWatcher *watcher, BenderChannel *channel, uint32_t default_flags) {
    (void) watcher;
    (void) channel;
    (void) default_flags;
    bender_panic("filewatch not supported on this platform");
}

static void bender_watcher_add(BenderWatcher *watcher, const char *path, uint32_t flags) {
    (void) watcher;
    (void) flags;
    (void) path;
    bender_panic("filewatch not supported on this platform");
}

static void bender_watcher_remove(BenderWatcher *watcher, const char *path) {
    (void) watcher;
    (void) path;
    bender_panic("filewatch not supported on this platform");
}

static void bender_watcher_listen(BenderWatcher *watcher) {
    (void) watcher;
    bender_panic("filewatch not supported on this platform");
}

static void bender_watcher_unlisten(BenderWatcher *watcher) {
    (void) watcher;
    bender_panic("filewatch not supported on this platform");
}

#endif

/* C Functions */

static int bender_filewatch_mark(void *p, size_t s) {
    BenderWatcher *watcher = (BenderWatcher *) p;
    (void) s;
    if (watcher->channel == NULL) return 0; /* Incomplete initialization */
#ifdef BENDER_WINDOWS
    for (int32_t i = 0; i < watcher->watch_descriptors->capacity; i++) {
        const BenderKV *kv = watcher->watch_descriptors->data + i;
        if (!bender_checktype(kv->value, BENDER_POINTER)) continue;
        OverlappedWatch *ow = bender_unwrap_pointer(kv->value);
        bender_mark(bender_wrap_fiber(ow->fiber));
        bender_mark(bender_wrap_abstract(ow->stream));
        bender_mark(bender_wrap_string(ow->dir_path));
    }
#else
    bender_mark(bender_wrap_abstract(watcher->stream));
#endif
    bender_mark(bender_wrap_abstract(watcher->channel));
    bender_mark(bender_wrap_table(watcher->watch_descriptors));
    return 0;
}

static const BenderAbstractType bender_filewatch_at = {
    "filewatch/watcher",
    NULL,
    bender_filewatch_mark,
    BENDER_ATEND_GCMARK
};

BENDER_CORE_FN(cfun_filewatch_make,
              "(filewatch/new chan & default-flags)",
              "Create a new filewatcher that will give events to a channel "
              "`chan`. See `filewatch/add` for available flags.\n"
              "\n"
              "When an event is triggered by the filewatcher, a struct "
              "containing information will be given to `chan` as with "
              "`ev/give`. The contents of `chan` depend on the OS, but will "
              "contain some common keys:\n"
              "\n"
              "* `:type` - type of the event that was raised\n"
              "* `:file-name` - base file name of file that triggered the "
              "event\n"
              "* `:dir-name` - directory name of file that triggered the "
              "event\n"
              "\n"
              "Events will also contain keys specific to the host OS.\n"
              "\n"
              "Windows has no extra properties on events.\n"
              "\n"
              "Linux and BSDs have the following extra properties on "
              "events:\n"
              "\n"
              "* `:wd` - integer key returned by `filewatch/add` for path "
              "that triggered this. This is a file descriptor integer on "
              "BSD and macos.\n"
              "\n"
              "* `:wd-path` - string path for watched directory of file. "
              "For files, same as `:file-name`, and for directories, same "
              "as `:dir-name`.\n"
              "\n"
              "* `:cookie` - a semi-randomized integer used to associate "
              "related events, such as `:moved-from` and `:moved-to`.") {
    bender_sandbox_assert(BENDER_SANDBOX_FS_READ);
    bender_arity(argc, 1, -1);
    BenderChannel *channel = bender_getchannel(argv, 0);
    BenderWatcher *watcher = bender_abstract(&bender_filewatch_at, sizeof(BenderWatcher));
    uint32_t default_flags = decode_watch_flags(argv + 1, argc - 1);
    bender_watcher_init(watcher, channel, default_flags);
    return bender_wrap_abstract(watcher);
}

BENDER_CORE_FN(cfun_filewatch_add,
              "(filewatch/add watcher path flags & more-flags)",
              "Add a path to the watcher. Available flags and event types "
              "depend on the current OS, and are as follows.\n"
              "\n"
              "Flags\n"
              "\n"
              "Windows/MINGW (correspond to `FILE_NOTIFY_CHANGE_*` flags in "
              "win32 documentation):\n"
              "\n"
              "* `:all` - trigger an event for all of the below triggers.\n"
              "* `:attributes` - `FILE_NOTIFY_CHANGE_ATTRIBUTES`\n"
              "* `:creation` - `FILE_NOTIFY_CHANGE_CREATION`\n"
              "* `:dir-name` - `FILE_NOTIFY_CHANGE_DIR_NAME`\n"
              "* `:last-access` - `FILE_NOTIFY_CHANGE_LAST_ACCESS`\n"
              "* `:last-write` - `FILE_NOTIFY_CHANGE_LAST_WRITE`\n"
              "* `:security` - `FILE_NOTIFY_CHANGE_SECURITY`\n"
              "* `:size` - `FILE_NOTIFY_CHANGE_SIZE`\n"
              "* `:recursive` - watch subdirectories recursively\n"
              "\n"
              "Linux (correspond to `IN_*` flags from `<sys/inotify.h>`):\n"
              "\n"
              "* `:access` - `IN_ACCESS`\n"
              "* `:all` - `IN_ALL_EVENTS`\n"
              "* `:attrib` - `IN_ATTRIB`\n"
              "* `:close-nowrite` - `IN_CLOSE_NOWRITE`\n"
              "* `:close-write` - `IN_CLOSE_WRITE`\n"
              "* `:create` - `IN_CREATE`\n"
              "* `:delete` - `IN_DELETE`\n"
              "* `:delete-self` - `IN_DELETE_SELF`\n"
              "* `:ignored` - `IN_IGNORED`\n"
              "* `:modify` - `IN_MODIFY`\n"
              "* `:move-self` - `IN_MOVE_SELF`\n"
              "* `:moved-from` - `IN_MOVED_FROM`\n"
              "* `:moved-to` - `IN_MOVED_TO`\n"
              "* `:open` - `IN_OPEN`\n"
              "* `:q-overflow` - `IN_Q_OVERFLOW`\n"
              "* `:unmount` - `IN_UNMOUNT`\n"
              "\n"
              "BSDs and macos (correspond to `NOTE_*` flags from "
              "`<sys/event.h>`). Not all flags are available on all "
              "systems:\n"
              "\n"
              "* `:all` - All available `NOTE_*` flags on the current platform\n"
              "* `:attrib` - `NOTE_ATTRIB`\n"
              "* `:close-write` - `NOTE_CLOSE_WRITE`\n"
              "* `:close` - `NOTE_CLOSE`\n"
              "* `:delete` - `NOTE_DELETE`\n"
              "* `:extend` - `NOTE_EXTEND`\n"
              "* `:funlock` - `NOTE_FUNLOCK`\n"
              "* `:link` - `NOTE_LINK`\n"
              "* `:open` - `NOTE_OPEN`\n"
              "* `:read` - `NOTE_READ`\n"
              "* `:rename` - `NOTE_RENAME`\n"
              "* `:revoke` - `NOTE_REVOKE`\n"
              "* `:truncate` - `NOTE_TRUNCATE`\n"
              "* `:write` - `NOTE_WRITE`\n"
              "\n"
              "Event Types\n"
              "\n"
              "On Windows, events will have the following possible types:\n"
              "\n"
              "* `:unknown`\n"
              "* `:added`\n"
              "* `:removed`\n"
              "* `:modified`\n"
              "* `:renamed-old`\n"
              "* `:renamed-new`\n"
              "\n"
              "On Linux and BSDs, events will have a `:type` corresponding "
              "to the possible flags, excluding `:all`.\n"
              "") {
    bender_sandbox_assert(BENDER_SANDBOX_FS_READ);
    bender_arity(argc, 2, -1);
    BenderWatcher *watcher = bender_getabstract(argv, 0, &bender_filewatch_at);
    const char *path = bender_getcstring(argv, 1);
    uint32_t flags = watcher->default_flags | decode_watch_flags(argv + 2, argc - 2);
    bender_watcher_add(watcher, path, flags);
    return argv[0];
}

BENDER_CORE_FN(cfun_filewatch_remove,
              "(filewatch/remove watcher path)",
              "Remove a path from the watcher.") {
    bender_fixarity(argc, 2);
    BenderWatcher *watcher = bender_getabstract(argv, 0, &bender_filewatch_at);
    /* TODO - pass string in directly to avoid extra allocation */
    const char *path = bender_getcstring(argv, 1);
    bender_watcher_remove(watcher, path);
    return argv[0];
}

BENDER_CORE_FN(cfun_filewatch_listen,
              "(filewatch/listen watcher)",
              "Listen for changes in the watcher.") {
    bender_fixarity(argc, 1);
    BenderWatcher *watcher = bender_getabstract(argv, 0, &bender_filewatch_at);
    bender_watcher_listen(watcher);
    return bender_wrap_nil();
}

BENDER_CORE_FN(cfun_filewatch_unlisten,
              "(filewatch/unlisten watcher)",
              "Stop listening for changes on a given watcher.") {
    bender_fixarity(argc, 1);
    BenderWatcher *watcher = bender_getabstract(argv, 0, &bender_filewatch_at);
    bender_watcher_unlisten(watcher);
    return bender_wrap_nil();
}

/* Module entry point */
void bender_lib_filewatch(BenderTable *env) {
    BenderRegExt cfuns[] = {
        BENDER_CORE_REG("filewatch/new", cfun_filewatch_make),
        BENDER_CORE_REG("filewatch/add", cfun_filewatch_add),
        BENDER_CORE_REG("filewatch/remove", cfun_filewatch_remove),
        BENDER_CORE_REG("filewatch/listen", cfun_filewatch_listen),
        BENDER_CORE_REG("filewatch/unlisten", cfun_filewatch_unlisten),
        BENDER_REG_END
    };
    bender_core_cfuns_ext(env, NULL, cfuns);
}

#endif
#endif
