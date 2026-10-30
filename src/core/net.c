/*
* Copyright (c) 2026 Calvin Rose and contributors.
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
#include "fiber.h"
#endif

#ifdef BENDER_NET

#include <math.h>
#ifdef BENDER_WINDOWS
#include <winsock2.h>
#include <windows.h>
#include <ws2tcpip.h>
#include <mswsock.h>
#ifdef BENDER_MSVC
#pragma comment (lib, "Ws2_32.lib")
#pragma comment (lib, "Mswsock.lib")
#pragma comment (lib, "Advapi32.lib")
#endif
#else
#include <arpa/inet.h>
#include <unistd.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <netdb.h>
#include <fcntl.h>
#endif

const BenderAbstractType bender_address_type = {
    "core/socket-address",
    BENDER_ATEND_NAME
};

#ifdef BENDER_WINDOWS
#define JSOCKCLOSE(x) closesocket((SOCKET) x)
#define JSOCKDEFAULT INVALID_SOCKET
#define JSOCKVALID(x) ((x) != INVALID_SOCKET)
#define JSock SOCKET
#define JSOCKFLAGS 0
#else
#define JSOCKCLOSE(x) close(x)
#define JSOCKDEFAULT 0
#define JSOCKVALID(x) ((x) >= 0)
#define JSock int
#ifdef SOCK_CLOEXEC
#define JSOCKFLAGS SOCK_CLOEXEC
#else
#define JSOCKFLAGS 0
#endif
#endif

/* maximum number of bytes in a socket address host (post name resolution) */
#ifdef BENDER_WINDOWS
#ifdef BENDER_NO_IPV6
#define SA_ADDRSTRLEN (INET_ADDRSTRLEN + 1)
#else
#define SA_ADDRSTRLEN (INET6_ADDRSTRLEN + 1)
#endif
typedef unsigned short in_port_t;
#else
#define BENDER_SA_MAX(a, b) (((a) > (b))? (a) : (b))
#ifdef BENDER_NO_IPV6
#define SA_ADDRSTRLEN BENDER_SA_MAX(INET_ADDRSTRLEN + 1, (sizeof ((struct sockaddr_un *)0)->sun_path) + 1)
#else
#define SA_ADDRSTRLEN BENDER_SA_MAX(INET6_ADDRSTRLEN + 1, (sizeof ((struct sockaddr_un *)0)->sun_path) + 1)
#endif
#endif

static BenderStream *make_stream(JSock handle, uint32_t flags);

/* We pass this flag to all send calls to prevent sigpipe */
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

/* Make sure a socket doesn't block */
static void bender_net_socknoblock(JSock s) {
#ifdef BENDER_WINDOWS
    unsigned long arg = 1;
    ioctlsocket(s, FIONBIO, &arg);
#else
#if !defined(SOCK_CLOEXEC) && defined(O_CLOEXEC)
    int extra = O_CLOEXEC;
#else
    int extra = 0;
#endif
    fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK | extra);
#ifdef SO_NOSIGPIPE
    int enable = 1;
    setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &enable, sizeof(int));
#endif
#endif
}

/* Allow specifying IPV6 vs. IPV4 (or unix domain socket) */
static int net_get_address_family(Bender x) {
    if (bender_checktype(x, BENDER_NIL)) {
        return AF_UNSPEC;
    }
    if (bender_keyeq(x, "ipv4")) {
        return AF_INET;
    }
    if (bender_keyeq(x, "ipv6")) {
        return AF_INET6;
    }
#ifndef BENDER_WINDOWS
    if (bender_keyeq(x, "unix")) {
        return AF_UNIX;
    }
#endif
    return AF_UNSPEC;
}

/* State machine for async connect */
#ifdef BENDER_WINDOWS

typedef struct NetStateConnect {
    /* Only used for ConnectEx */
    BenderOverlapped overlapped;
} NetStateConnect;

static LPFN_CONNECTEX lazy_get_connectex(JSock sock) {
    /* Get ConnectEx */
    if (bender_vm.connect_ex_loaded) {
        return bender_vm.connect_ex;
    }
    GUID guid = WSAID_CONNECTEX;
    LPFN_CONNECTEX connect_ex_ptr = NULL;
    DWORD byte_len = 0;
    int success = WSAIoctl(sock, SIO_GET_EXTENSION_FUNCTION_POINTER,
                           (void *)&guid, sizeof(guid),
                           (void *)&connect_ex_ptr, sizeof(connect_ex_ptr),
                           &byte_len, NULL, NULL);
    if (success) {
        bender_vm.connect_ex = connect_ex_ptr;
    } else {
        bender_vm.connect_ex = NULL;
    }
    bender_vm.connect_ex_loaded = 1;
    return bender_vm.connect_ex;
}

#endif

void net_callback_connect(BenderFiber *fiber, BenderAsyncEvent event) {
    BenderStream *stream = fiber->ev_stream;
    switch (event) {
        default:
            break;
#ifndef BENDER_WINDOWS
        /* Wait until we have an actual event before checking.
         * Windows doesn't support async connect with this, just try immediately.*/
        case BENDER_ASYNC_EVENT_INIT:
#endif
        case BENDER_ASYNC_EVENT_DEINIT:
            return;
        case BENDER_ASYNC_EVENT_CLOSE:
            bender_cancel(fiber, bender_cstringv("stream closed"));
            bender_async_end(fiber);
            return;
    }
#ifdef BENDER_WINDOWS
    /* We should be using ConnectEx here */
    int res = 0;
    int size = sizeof(res);
    int r = getsockopt((SOCKET)stream->handle, SOL_SOCKET, SO_CONNECT_TIME, (char *)&res, &size);
    if (r == NO_ERROR && res == (~0)) {
        return; /* This apparently indicates we haven't yet gotten a connection */
    }
    const int no_error = NO_ERROR;
#else
    int res = 0;
    socklen_t size = sizeof(res);
    int r = getsockopt(stream->handle, SOL_SOCKET, SO_ERROR, &res, &size);
    const int no_error = 0;
#endif
    if (r == no_error) {
        if (res == 0) {
            bender_schedule(fiber, bender_wrap_abstract(stream));
        } else {
            bender_cancel(fiber, bender_cstringv(bender_strerror(res)));
            stream->flags |= BENDER_STREAM_TOCLOSE;
        }
    } else {
        bender_cancel(fiber, bender_ev_lasterr());
        stream->flags |= BENDER_STREAM_TOCLOSE;
    }
    bender_async_end(fiber);
}

static BENDER_NO_RETURN void net_sched_connect(BenderStream *stream, void *state) {
    bender_async_start(stream, BENDER_ASYNC_LISTEN_WRITE, net_callback_connect, state);
}

/* State machine for accepting connections. */

#ifdef BENDER_WINDOWS

typedef struct {
    BenderOverlapped overlapped;
    BenderFunction *function;
    BenderStream *lstream;
    BenderStream *astream;
    char buf[1024];
} NetStateAccept;

static int net_sched_accept_impl(NetStateAccept *state, BenderFiber *fiber, Bender *err);

void net_callback_accept(BenderFiber *fiber, BenderAsyncEvent event) {
    NetStateAccept *state = (NetStateAccept *)fiber->ev_state;
    switch (event) {
        default:
            break;
        case BENDER_ASYNC_EVENT_MARK: {
            if (state->lstream) bender_mark(bender_wrap_abstract(state->lstream));
            if (state->astream) bender_mark(bender_wrap_abstract(state->astream));
            if (state->function) bender_mark(bender_wrap_function(state->function));
            break;
        }
        case BENDER_ASYNC_EVENT_CLOSE:
            bender_schedule(fiber, bender_wrap_nil());
            bender_async_end(fiber);
            return;
        case BENDER_ASYNC_EVENT_COMPLETE: {
            if (state->astream->flags & BENDER_STREAM_CLOSED) {
                bender_cancel(fiber, bender_cstringv("failed to accept connection"));
                bender_async_end(fiber);
                return;
            }
            SOCKET lsock = (SOCKET) state->lstream->handle;
            if (NO_ERROR != setsockopt((SOCKET) state->astream->handle, SOL_SOCKET, SO_UPDATE_ACCEPT_CONTEXT,
                                       (char *) &lsock, sizeof(lsock))) {
                bender_cancel(fiber, bender_cstringv("failed to accept connection"));
                bender_async_end(fiber);
                return;
            }

            Bender streamv = bender_wrap_abstract(state->astream);
            if (state->function) {
                /* Schedule worker */
                BenderFiber *sub_fiber = bender_fiber(state->function, 64, 1, &streamv);
                sub_fiber->supervisor_channel = fiber->supervisor_channel;
                bender_schedule(sub_fiber, bender_wrap_nil());
                /* Now listen again for next connection */
                Bender err;
                if (net_sched_accept_impl(state, fiber, &err)) {
                    bender_cancel(fiber, err);
                    bender_async_end(fiber);
                    return;
                }
            } else {
                bender_schedule(fiber, streamv);
                bender_async_end(fiber);
                return;
            }
        }
    }
}

BENDER_NO_RETURN static void bender_sched_accept(BenderStream *stream, BenderFunction *fun) {
    Bender err;
    NetStateAccept *state = bender_malloc(sizeof(NetStateAccept));
    memset(&state->overlapped, 0, sizeof(BenderOverlapped));
    memset(&state->buf, 0, 1024);
    state->function = fun;
    state->lstream = stream;
    if (net_sched_accept_impl(state, bender_root_fiber(), &err)) {
        bender_free(state);
        bender_panicv(err);
    }
    bender_async_start(stream, BENDER_ASYNC_LISTEN_READ, net_callback_accept, state);
}

static int net_sched_accept_impl(NetStateAccept *state, BenderFiber *fiber, Bender *err) {
    SOCKET lsock = (SOCKET) state->lstream->handle;
#ifdef BENDER_NO_IPV6
    SOCKET asock = WSASocketW(AF_INET, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_OVERLAPPED);
#else
    SOCKET asock = WSASocketW(AF_INET6, SOCK_STREAM, IPPROTO_TCP, NULL, 0, WSA_FLAG_OVERLAPPED);
#endif
    if (asock == INVALID_SOCKET) {
        *err = bender_ev_lasterr();
        return 1;
    }
    BenderStream *astream = make_stream(asock, BENDER_STREAM_READABLE | BENDER_STREAM_WRITABLE);
    state->astream = astream;
    int socksize = sizeof(SOCKADDR_STORAGE) + 16;
    if (FALSE == AcceptEx(lsock, asock, state->buf, 0, socksize, socksize, NULL, &state->overlapped.as.wsaoverlapped)) {
        int code = WSAGetLastError();
        if (code == WSA_IO_PENDING) {
            /* indicates io is happening async */
            bender_async_in_flight(fiber);
            return 0;
        }
        *err = bender_ev_lasterr();
        return 1;
    }
    return 0;
}

#else

typedef struct {
    BenderFunction *function;
} NetStateAccept;

void net_callback_accept(BenderFiber *fiber, BenderAsyncEvent event) {
    BenderStream *stream = fiber->ev_stream;
    NetStateAccept *state = (NetStateAccept *)fiber->ev_state;
    switch (event) {
        default:
            break;
        case BENDER_ASYNC_EVENT_MARK: {
            if (state->function) bender_mark(bender_wrap_function(state->function));
            break;
        }
        case BENDER_ASYNC_EVENT_CLOSE:
            bender_schedule(fiber, bender_wrap_nil());
            bender_async_end(fiber);
            return;
        case BENDER_ASYNC_EVENT_INIT:
        case BENDER_ASYNC_EVENT_READ: {
#if defined(BENDER_LINUX)
            JSock connfd = accept4(stream->handle, NULL, NULL, SOCK_CLOEXEC);
#else
            /* On BSDs, CLOEXEC should be inherited from server socket */
            JSock connfd = accept(stream->handle, NULL, NULL);
#endif
            if (JSOCKVALID(connfd)) {
                bender_net_socknoblock(connfd);
                BenderStream *stream = make_stream(connfd, BENDER_STREAM_READABLE | BENDER_STREAM_WRITABLE);
                Bender streamv = bender_wrap_abstract(stream);
                if (state->function) {
                    BenderFiber *sub_fiber = bender_fiber(state->function, 64, 1, &streamv);
                    sub_fiber->supervisor_channel = fiber->supervisor_channel;
                    bender_schedule(sub_fiber, bender_wrap_nil());
                } else {
                    bender_schedule(fiber, streamv);
                    bender_async_end(fiber);
                    return;
                }
            }
            break;
        }
    }
}

BENDER_NO_RETURN static void bender_sched_accept(BenderStream *stream, BenderFunction *fun) {
    NetStateAccept *state = bender_malloc(sizeof(NetStateAccept));
    memset(state, 0, sizeof(NetStateAccept));
    state->function = fun;
    if (fun) bender_stream_level_triggered(stream);
    bender_async_start(stream, BENDER_ASYNC_LISTEN_READ, net_callback_accept, state);
}

#endif

/* Address info */

static int bender_get_sockettype(Bender *argv, int32_t argc, int32_t n) {
    BenderKeyword stype = bender_optkeyword(argv, argc, n, NULL);
    int socktype = SOCK_DGRAM;
    if ((NULL == stype) || !bender_cstrcmp(stype, "stream")) {
        socktype = SOCK_STREAM;
    } else if (bender_cstrcmp(stype, "datagram")) {
        bender_panicf("expected socket type as :stream or :datagram, got %v", argv[n]);
    }
    return socktype;
}

/* Needs argc >= offset + 2 */
/* For unix paths, just rertuns a single sockaddr and sets *is_unix to 1,
 * otherwise 0. Also, ignores is_bind when is a unix socket. */
static struct addrinfo *bender_get_addrinfo(Bender *argv, int32_t offset, int socktype, int passive, int *is_unix, socklen_t *sizeout) {
    /* Unix socket support - not yet supported on windows. */
#ifndef BENDER_WINDOWS
    if (bender_keyeq(argv[offset], "unix")) {
        const char *path = bender_getcstring(argv, offset + 1);
        struct sockaddr_un *saddr = bender_calloc(1, sizeof(struct sockaddr_un));
        if (saddr == NULL) {
            BENDER_OUT_OF_MEMORY;
        }
        saddr->sun_family = AF_UNIX;
        size_t path_size = sizeof(saddr->sun_path);
        snprintf(saddr->sun_path, path_size, "%s", path);
        *sizeout = sizeof(struct sockaddr_un);
#ifdef BENDER_LINUX
        if (path[0] == '@') {
            saddr->sun_path[0] = '\0';
            *sizeout = offsetof(struct sockaddr_un, sun_path) + bender_string_length(path);
        }
#endif
        *is_unix = 1;
        return (struct addrinfo *) saddr;
    }
#endif
    /* Get host and port */
    char *host = (char *)bender_getcstring(argv, offset);
    char *port = NULL;
    if (bender_checkint(argv[offset + 1])) {
        port = (char *)bender_to_string(argv[offset + 1]);
    } else {
        port = (char *)bender_optcstring(argv, offset + 2, offset + 1, NULL);
    }
    /* getaddrinfo */
    struct addrinfo *ai = NULL;
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = socktype;
    hints.ai_flags = passive ? AI_PASSIVE : 0;
    int status = getaddrinfo(host, port, &hints, &ai);
    if (status) {
        bender_panicf("could not get address info: %s", gai_strerror(status));
    }
    *is_unix = 0;
#ifdef BENDER_WINDOWS
    *sizeout = 0;
#else
    *sizeout = sizeof(struct sockaddr_un);
#endif
    return ai;
}

/*
 * C Funs
 */

BENDER_CORE_FN(cfun_net_sockaddr,
              "(net/address host port &opt cntype multi)",
              "Look up the connection information for a given `host`, "
              "`port`, and connection type `cntype`. Returns a handle that "
              "can be used to send datagrams over the network without "
              "establishing a connection. On POSIX platforms, you can use "
              "`:unix` for `host` to connect to a unix domain socket, where "
              "the name is given in the `port` argument. On Linux, abstract "
              "unix domain sockets are specified with a leading '@' "
              "character in `port`. If `multi` is truthy, returns all "
              "addresses that match in an array instead of just the first.") {
    bender_sandbox_assert(BENDER_SANDBOX_NET_CONNECT); /* connect OR listen */
    bender_arity(argc, 2, 4);
    int socktype = bender_get_sockettype(argv, argc, 2);
    int is_unix = 0;
    int make_arr = (argc >= 3 && bender_truthy(argv[3]));
    socklen_t addrsize = 0;
    struct addrinfo *ai = bender_get_addrinfo(argv, 0, socktype, 0, &is_unix, &addrsize);
#ifndef BENDER_WINDOWS
    /* no unix domain socket support on windows yet */
    if (is_unix) {
        void *abst = bender_abstract(&bender_address_type, addrsize);
        memcpy(abst, ai, addrsize);
        Bender ret = bender_wrap_abstract(abst);
        return make_arr ? bender_wrap_array(bender_array_n(&ret, 1)) : ret;
    }
#endif
    if (make_arr) {
        /* Select all */
        BenderArray *arr = bender_array(10);
        struct addrinfo *iter = ai;
        while (NULL != iter) {
            void *abst = bender_abstract(&bender_address_type, iter->ai_addrlen);
            memcpy(abst, iter->ai_addr, iter->ai_addrlen);
            bender_array_push(arr, bender_wrap_abstract(abst));
            iter = iter->ai_next;
        }
        freeaddrinfo(ai);
        return bender_wrap_array(arr);
    } else {
        /* Select first */
        if (NULL == ai) {
            bender_panic("no data for given address");
        }
        void *abst = bender_abstract(&bender_address_type, ai->ai_addrlen);
        memcpy(abst, ai->ai_addr, ai->ai_addrlen);
        freeaddrinfo(ai);
        return bender_wrap_abstract(abst);
    }
}

BENDER_CORE_FN(cfun_net_connect,
              "(net/connect host port &opt cntype bindhost bindport)",
              "Open a connection to communicate with a server at `host` "
              "`port`. Returns a duplex stream that can be used to "
              "communicate with the server. `cntype` is an optional keyword "
              "to specify a connection type, either `:stream` or "
              "`:datagram`; the default is `:stream`. `bindhost` and "
              "`bindport` are optional strings to select from what address "
              "to make the outgoing connection; the default is the same as "
              "using the OS's preferred address.") {
    bender_sandbox_assert(BENDER_SANDBOX_NET_CONNECT);
    bender_arity(argc, 2, 5);

    /* Check arguments */
    int socktype = bender_get_sockettype(argv, argc, 2);
    int is_unix = 0;
    char *bindhost = (char *) bender_optcstring(argv, argc, 3, NULL);
    char *bindport = NULL;
    if (argc >= 5 && bender_checkint(argv[4])) {
        bindport = (char *)bender_to_string(argv[4]);
    } else {
        bindport = (char *)bender_optcstring(argv, argc, 4, NULL);
    }

    /* Where we're connecting to */
    socklen_t addrlen = 0;
    struct addrinfo *ai = bender_get_addrinfo(argv, 0, socktype, 0, &is_unix, &addrlen);

    /* Check if we're binding address */
    struct addrinfo *binding = NULL;
    if (bindhost != NULL) {
        if (is_unix) {
            freeaddrinfo(ai);
            bender_panic("bindhost not supported for unix domain sockets");
        }
        /* getaddrinfo */
        struct addrinfo hints;
        memset(&hints, 0, sizeof(hints));
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = socktype;
        hints.ai_flags = 0;
        int status = getaddrinfo(bindhost, bindport, &hints, &binding);
        if (status) {
            freeaddrinfo(ai);
            bender_panicf("could not get address info for bindhost: %s", gai_strerror(status));
        }
    }

    /* Create socket */
    JSock sock = JSOCKDEFAULT;
    void *addr = NULL;
#ifndef BENDER_WINDOWS
    if (is_unix) {
        sock = socket(AF_UNIX, socktype | JSOCKFLAGS, 0);
        if (!JSOCKVALID(sock)) {
            Bender v = bender_ev_lasterr();
            bender_free(ai);
            bender_panicf("could not create socket: %V", v);
        }
        addr = (void *) ai;
    } else
#endif
    {
        struct addrinfo *rp = NULL;
        for (rp = ai; rp != NULL; rp = rp->ai_next) {
#ifdef BENDER_WINDOWS
            sock = WSASocketW(rp->ai_family, rp->ai_socktype, rp->ai_protocol, NULL, 0, WSA_FLAG_OVERLAPPED);
#else
            sock = socket(rp->ai_family, rp->ai_socktype | JSOCKFLAGS, rp->ai_protocol);
#endif
            if (JSOCKVALID(sock)) {
                addr = rp->ai_addr;
                addrlen = (socklen_t) rp->ai_addrlen;
                break;
            }
        }
        if (NULL == addr) {
            Bender v = bender_ev_lasterr();
            if (binding) freeaddrinfo(binding);
            freeaddrinfo(ai);
            bender_panicf("could not create socket: %V", v);
        }
    }

    /* Bind to bindhost and bindport if given */
    if (binding) {
        struct addrinfo *rp = NULL;
        int did_bind = 0;
        for (rp = binding; rp != NULL; rp = rp->ai_next) {
            if (bind(sock, rp->ai_addr, (int) rp->ai_addrlen) == 0) {
                did_bind = 1;
                break;
            }
        }
        if (!did_bind) {
            Bender v = bender_ev_lasterr();
            freeaddrinfo(binding);
            freeaddrinfo(ai);
            JSOCKCLOSE(sock);
            bender_panicf("could not bind outgoing address: %V", v);
        } else {
            freeaddrinfo(binding);
        }
    }

    /* Wrap socket in abstract type BenderStream */
    uint32_t udp_flag = 0;
    if (socktype == SOCK_DGRAM) udp_flag = BENDER_STREAM_UDPSERVER;
    BenderStream *stream = make_stream(sock, BENDER_STREAM_READABLE | BENDER_STREAM_WRITABLE | udp_flag);

    /* Connect to socket */
#ifdef BENDER_WINDOWS
    int status = 0;
    int err = 0;
    LPFN_CONNECTEX connect_ex = NULL;
    if (socktype == SOCK_STREAM && ((connect_ex = lazy_get_connectex(sock)))) {
        /* Prefer ConnecEx as it works well with overlapped IO. */
        bender_net_socknoblock(sock);
        NetStateConnect *state = bender_malloc(sizeof(NetStateConnect));
        memset(state, 0, sizeof(NetStateConnect));
        BOOL success = connect_ex(sock, addr, addrlen, NULL, 0, NULL, &state->overlapped.as.overlapped);
        freeaddrinfo(ai);
        if (success) {
            /* Did not fail */
        } else {
            int err = WSAGetLastError();
            if (err == ERROR_IO_PENDING) {
                /* Did not actually fail yet */
            } else {
                bender_free(state);
                Bender lasterr = bender_ev_lasterr();
                bender_panicf("could not connect socket (ConnectEx): %V", lasterr);
            }
        }

        net_sched_connect(stream, state);
    } else {
        /* Default to blocking connect if ConnectEx not available */
        status = WSAConnect(sock, addr, addrlen, NULL, NULL, NULL, NULL);
        err = WSAGetLastError();
        freeaddrinfo(ai);
        /* Set up the socket for non-blocking IO after connecting on windows by default */
        bender_net_socknoblock(sock);
    }

#else
    /* Set up the socket for non-blocking IO before connecting */
    bender_net_socknoblock(sock);
    int status;
    do {
        status = connect(sock, addr, addrlen);
    } while (status == -1 && errno == EINTR);
    int err = errno;
    if (is_unix) {
        bender_free(ai);
    } else {
        freeaddrinfo(ai);
    }
#endif

    if (status == 0) {
        /* Connect completed synchronously (common for unix domain sockets).
         * Return the stream directly without scheduling an async wait,
         * as edge-triggered kqueue may not signal EVFILT_WRITE if the socket
         * is already connected when registered. */
        return bender_wrap_abstract(stream);
    }

#ifdef BENDER_WINDOWS
    if (status == SOCKET_ERROR) {
        if (err != WSAEWOULDBLOCK) {
#else
    if (status == -1) {
        if (err != EINPROGRESS) {
#endif
            JSOCKCLOSE(sock);
            Bender lasterr = bender_ev_lasterr();
            bender_panicf("could not connect socket: %V", lasterr);
        }
    }

    net_sched_connect(stream, NULL);
}

BENDER_CORE_FN(cfun_net_socket,
              "(net/socket &opt cntype address-family)",
              "Creates a new unbound socket. `cntype` is an optional "
              "keyword, either `:stream` (usually TCP), or `:datagram` "
              "(usually UDP). The default is `:stream`. `address-family` "
              "should be `:ipv4` or `:ipv6`.") {
    bender_arity(argc, 0, 2);

    int socktype = bender_get_sockettype(argv, argc, 0);

    /* Create socket */
    JSock sfd = JSOCKDEFAULT;
    struct addrinfo *ai = NULL;
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = socktype;
#ifdef AI_NUMERICSERV
    hints.ai_flags = AI_NUMERICSERV; /* Explicitly prevent name resolution */
#else
    hints.ai_flags = 0;
#endif
    if (argc >= 2) {
        hints.ai_family = net_get_address_family(argv[1]);
    }
    int status = getaddrinfo(NULL, "0", &hints, &ai);
    if (status) {
        bender_panicf("could not get address info: %s", gai_strerror(status));
    }

    struct addrinfo *rp = NULL;
    for (rp = ai; rp != NULL; rp = rp->ai_next) {
#ifdef BENDER_WINDOWS
        sfd = WSASocketW(rp->ai_family, rp->ai_socktype | JSOCKFLAGS, rp->ai_protocol, NULL, 0, WSA_FLAG_OVERLAPPED);
#else
        sfd = socket(rp->ai_family, rp->ai_socktype | JSOCKFLAGS, rp->ai_protocol);
#endif
        if (JSOCKVALID(sfd)) {
            break;
        }
    }
    freeaddrinfo(ai);

    if (!JSOCKVALID(sfd)) {
        Bender v = bender_ev_lasterr();
        bender_panicf("could not create socket: %V", v);
    }

    /* Wrap socket in abstract type BenderStream */
    uint32_t udp_flag = 0;
    if (socktype == SOCK_DGRAM) udp_flag = BENDER_STREAM_UDPSERVER;
    BenderStream *stream = make_stream(sfd, BENDER_STREAM_READABLE | BENDER_STREAM_WRITABLE | udp_flag);

    /* Set up the socket for non-blocking IO */
    bender_net_socknoblock(sfd);

    return bender_wrap_abstract(stream);
}

static const char *serverify_socket(JSock sfd, int reuse_addr, int reuse_port) {
    /* Set various socket options */
    int enable = 1;
    if (reuse_addr) {
        if (setsockopt(sfd, SOL_SOCKET, SO_REUSEADDR, (char *) &enable, sizeof(int)) < 0) {
            return "setsockopt(SO_REUSEADDR) failed";
        }
    }
    if (reuse_port) {
#if defined(SO_REUSEPORT) && !BENDER_GNU_HURD
        if (setsockopt(sfd, SOL_SOCKET, SO_REUSEPORT, &enable, sizeof(int)) < 0) {
            return "setsockopt(SO_REUSEPORT) failed";
        }
#else
        (void) reuse_port;
#endif
    }
    bender_net_socknoblock(sfd);
    return NULL;
}

#ifdef BENDER_WINDOWS
#define BENDER_SHUTDOWN_RW SD_BOTH
#define BENDER_SHUTDOWN_R SD_RECEIVE
#define BENDER_SHUTDOWN_W SD_SEND
#else
#define BENDER_SHUTDOWN_RW SHUT_RDWR
#define BENDER_SHUTDOWN_R SHUT_RD
#define BENDER_SHUTDOWN_W SHUT_WR
#endif

BENDER_CORE_FN(cfun_net_shutdown,
              "(net/shutdown stream &opt mode)",
              "Stop communication on this socket in a graceful manner, either in both directions or just "
              "reading/writing from the stream. The `mode` parameter controls which communication to stop on the socket. "
              "\n\n* `:wr` is the default and prevents both reading new data from the socket and writing new data to the socket.\n"
              "* `:r` disables reading new data from the socket.\n"
              "* `:w` disable writing data to the socket.\n\n"
              "Returns the original socket.") {
    bender_arity(argc, 1, 2);
    BenderStream *stream = bender_getabstract(argv, 0, &bender_stream_type);
    bender_stream_flags(stream, BENDER_STREAM_SOCKET);
    int shutdown_type = BENDER_SHUTDOWN_RW;
    if (argc == 2) {
        const uint8_t *kw = bender_getkeyword(argv, 1);
        if (0 == bender_cstrcmp(kw, "rw")) {
            shutdown_type = BENDER_SHUTDOWN_RW;
        } else if (0 == bender_cstrcmp(kw, "r")) {
            shutdown_type = BENDER_SHUTDOWN_R;
        } else if (0 == bender_cstrcmp(kw, "w")) {
            shutdown_type = BENDER_SHUTDOWN_W;
        } else {
            bender_panicf("unexpected keyword %v", argv[1]);
        }
    }
    int status;
#ifdef BENDER_WINDOWS
    status = shutdown((SOCKET) stream->handle, shutdown_type);
#else
    do {
        status = shutdown(stream->handle, shutdown_type);
    } while (status == -1 && errno == EINTR);
#endif
    if (status) {
        bender_panicf("could not shutdown socket: %V", bender_ev_lasterr());
    }
    return argv[0];
}

BENDER_CORE_FN(cfun_net_listen,
              "(net/listen host port &opt cntype no-reuse)",
              "Creates a server. Returns a new stream that is neither "
              "readable nor writeable. Use `net/accept` or "
              "`net/accept-loop` to handle connections and start the "
              "server. The `cntype` parameter specifies the connection "
              "type, either `:stream` (usually TCP), or `:datagram` "
              "(usually UDP). If not specified, the default is `:stream`. "
              "The `host` and `port` arguments are the same as in "
              "`net/address`. The last boolean parameter `no-reuse` "
              "disables the use of `SO_REUSEADDR` and `SO_REUSEPORT` when "
              "creating a server on some operating systems.") {
    bender_sandbox_assert(BENDER_SANDBOX_NET_LISTEN);
    bender_arity(argc, 2, 4);

    /* Get host, port, and handler*/
    int socktype = bender_get_sockettype(argv, argc, 2);
    int is_unix = 0;
    socklen_t addrlen = 0;
    struct addrinfo *ai = bender_get_addrinfo(argv, 0, socktype, 1, &is_unix, &addrlen);
    int reuse = !(argc >= 4 && bender_truthy(argv[3]));

    JSock sfd = JSOCKDEFAULT;
#ifndef BENDER_WINDOWS
    if (is_unix) {
        sfd = socket(AF_UNIX, socktype | JSOCKFLAGS, 0);
        if (!JSOCKVALID(sfd)) {
            bender_free(ai);
            bender_panicf("could not create socket: %V", bender_ev_lasterr());
        }
        const char *err = serverify_socket(sfd, reuse, 0);
        if (NULL != err || bind(sfd, (struct sockaddr *)ai, addrlen)) {
            JSOCKCLOSE(sfd);
            bender_free(ai);
            if (err) {
                bender_panic(err);
            } else {
                bender_panicf("could not bind socket: %V", bender_ev_lasterr());
            }
        }
        bender_free(ai);
    } else
#endif
    {
        /* Check all addrinfos in a loop for the first that we can bind to. */
        struct addrinfo *rp = NULL;
        for (rp = ai; rp != NULL; rp = rp->ai_next) {
#ifdef BENDER_WINDOWS
            sfd = WSASocketW(rp->ai_family, rp->ai_socktype | JSOCKFLAGS, rp->ai_protocol, NULL, 0, WSA_FLAG_OVERLAPPED);
#else
            sfd = socket(rp->ai_family, rp->ai_socktype | JSOCKFLAGS, rp->ai_protocol);
#endif
            if (!JSOCKVALID(sfd)) continue;
            const char *err = serverify_socket(sfd, reuse, reuse);
            if (NULL != err) {
                JSOCKCLOSE(sfd);
                continue;
            }
            /* Bind */
            if (bind(sfd, rp->ai_addr, (int) rp->ai_addrlen) == 0) break;
            JSOCKCLOSE(sfd);
        }
        freeaddrinfo(ai);
        if (NULL == rp) {
            bender_panic("could not bind to any sockets");
        }
    }

    if (socktype == SOCK_DGRAM) {
        /* Datagram server (UDP) */
        BenderStream *stream = make_stream(sfd, BENDER_STREAM_UDPSERVER | BENDER_STREAM_READABLE);
        return bender_wrap_abstract(stream);
    } else {
        /* Stream server (TCP) */

        /* listen */
        int status = listen(sfd, 1024);
        if (status) {
            JSOCKCLOSE(sfd);
            bender_panicf("could not listen on file descriptor: %V", bender_ev_lasterr());
        }

        /* Put sfd on our loop */
        BenderStream *stream = make_stream(sfd, BENDER_STREAM_ACCEPTABLE);
        return bender_wrap_abstract(stream);
    }
}

/* Types of socket's we need to deal with - relevant type puns below.
struct sockaddr *sa;           // Common base structure
struct sockaddr_storage *ss;   // Size of largest socket address type
struct sockaddr_in *sin;       // IPv4 address + port
struct sockaddr_in6 *sin6;     // IPv6 address + port
struct sockaddr_un *sun;       // Unix Domain Socket Address
*/

/* Turn a socket address into a host, port pair.
 * For unix domain sockets, returned tuple will have only a single element, the path string. */
static Bender bender_so_getname(const void *sa_any) {
    const struct sockaddr *sa = sa_any;
    char buffer[SA_ADDRSTRLEN];
    switch (sa->sa_family) {
        default:
            bender_panic("unknown address family");
        case AF_INET: {
            const struct sockaddr_in *sai = sa_any;
            if (!inet_ntop(AF_INET, &(sai->sin_addr), buffer, sizeof(buffer))) {
                bender_panic("unable to decode ipv4 host address");
            }
            Bender pair[2] = {bender_cstringv(buffer), bender_wrap_integer(ntohs(sai->sin_port))};
            return bender_wrap_tuple(bender_tuple_n(pair, 2));
        }
#ifndef BENDER_NO_IPV6
        case AF_INET6: {
            const struct sockaddr_in6 *sai6 = sa_any;
            if (!inet_ntop(AF_INET6, &(sai6->sin6_addr), buffer, sizeof(buffer))) {
                bender_panic("unable to decode ipv4 host address");
            }
            Bender pair[2] = {bender_cstringv(buffer), bender_wrap_integer(ntohs(sai6->sin6_port))};
            return bender_wrap_tuple(bender_tuple_n(pair, 2));
        }
#endif
#ifndef BENDER_WINDOWS
        case AF_UNIX: {
            const struct sockaddr_un *sun = sa_any;
            Bender pathname;
            if (sun->sun_path[0] == '\0') {
                memcpy(buffer, sun->sun_path, sizeof(sun->sun_path));
                buffer[0] = '@';
                pathname = bender_cstringv(buffer);
            } else {
                pathname = bender_cstringv(sun->sun_path);
            }
            return bender_wrap_tuple(bender_tuple_n(&pathname, 1));
        }
#endif
    }
}

BENDER_CORE_FN(cfun_net_getsockname,
              "(net/localname stream)",
              "Gets the local address and port in a tuple in that order.") {
    bender_fixarity(argc, 1);
    BenderStream *js = bender_getabstract(argv, 0, &bender_stream_type);
    if (js->flags & BENDER_STREAM_CLOSED) bender_panic("stream closed");
    struct sockaddr_storage ss;
    socklen_t slen = sizeof(ss);
    memset(&ss, 0, slen);
    if (getsockname((JSock)js->handle, (struct sockaddr *) &ss, &slen)) {
        bender_panicf("Failed to get localname on %v: %V", argv[0], bender_ev_lasterr());
    }
    bender_assert(slen <= (socklen_t) sizeof(ss), "socket address truncated");
    return bender_so_getname(&ss);
}

BENDER_CORE_FN(cfun_net_getpeername,
              "(net/peername stream)",
              "Gets the remote peer's address and port in a tuple in that order.") {
    bender_fixarity(argc, 1);
    BenderStream *js = bender_getabstract(argv, 0, &bender_stream_type);
    if (js->flags & BENDER_STREAM_CLOSED) bender_panic("stream closed");
    struct sockaddr_storage ss;
    socklen_t slen = sizeof(ss);
    memset(&ss, 0, slen);
    if (getpeername((JSock)js->handle, (struct sockaddr *)&ss, &slen)) {
        bender_panicf("Failed to get peername on %v: %V", argv[0], bender_ev_lasterr());
    }
    bender_assert(slen <= (socklen_t) sizeof(ss), "socket address truncated");
    return bender_so_getname(&ss);
}

BENDER_CORE_FN(cfun_net_address_unpack,
              "(net/address-unpack address)",
              "Given an address returned by net/address, return a host, port pair. Unix domain sockets "
              "will have only the path in the returned tuple.") {
    bender_fixarity(argc, 1);
    struct sockaddr *sa = bender_getabstract(argv, 0, &bender_address_type);
    return bender_so_getname(sa);
}

BENDER_CORE_FN(cfun_stream_accept_loop,
              "(net/accept-loop stream handler)",
              "Shorthand for running a server stream that will continuously accept new connections. "
              "Blocks the current fiber until the stream is closed, and will return the stream.") {
    bender_fixarity(argc, 2);
    BenderStream *stream = bender_getabstract(argv, 0, &bender_stream_type);
    bender_stream_flags(stream, BENDER_STREAM_ACCEPTABLE | BENDER_STREAM_SOCKET);
    BenderFunction *fun = bender_getfunction(argv, 1);
    if (fun->def->min_arity < 1) bender_panic("handler function must take at least 1 argument");
    bender_sched_accept(stream, fun);
}

BENDER_CORE_FN(cfun_stream_accept,
              "(net/accept stream &opt timeout)",
              "Get the next connection on a server stream. This would usually be called in a loop in a dedicated fiber. "
              "Takes an optional timeout in seconds, after which will raise an error. "
              "The timeout belongs to THIS call: when it expires the waiting "
              "fiber is resumed with an error, and a completed wait retires its "
              "own deadline, so an expired poll cannot raise into unrelated "
              "code later. To poll without arming an exception, omit the "
              "timeout and drive the wait from a fiber you cancel yourself, or "
              "use `ev/pump` to sample listeners from a render loop. "
              "Returns a new duplex stream which represents a connection to the client.") {
    bender_arity(argc, 1, 2);
    BenderStream *stream = bender_getabstract(argv, 0, &bender_stream_type);
    bender_stream_flags(stream, BENDER_STREAM_ACCEPTABLE | BENDER_STREAM_SOCKET);
    double to = bender_optnumber(argv, argc, 1, INFINITY);
    if (to != INFINITY) bender_addtimeout(to);
    bender_sched_accept(stream, NULL);
}

BENDER_CORE_FN(cfun_stream_read,
              "(net/read stream nbytes &opt buf timeout)",
              "Read up to n bytes from a stream, suspending the current fiber until the bytes are available. "
              "`n` can also be the keyword `:all` to read into the buffer until end of stream. "
              "If less than n bytes are available (and more than 0), will push those bytes and return early. "
              "Takes an optional timeout in seconds, after which will raise an error for "
              "this read only. A completed read retires its own deadline, so polling "
              "repeatedly does not accumulate timers that fire into later, unrelated work. "
              "Returns a buffer with up to n more bytes in it, or raises an error if the read failed.") {
    bender_arity(argc, 2, 4);
    BenderStream *stream = bender_getabstract(argv, 0, &bender_stream_type);
    bender_stream_flags(stream, BENDER_STREAM_READABLE | BENDER_STREAM_SOCKET);
    BenderBuffer *buffer = bender_optbuffer(argv, argc, 2, 10);
    double to = bender_optnumber(argv, argc, 3, INFINITY);
    if (bender_keyeq(argv[1], "all")) {
        if (to != INFINITY) bender_addtimeout(to);
        bender_ev_recvchunk(stream, buffer, INT32_MAX, MSG_NOSIGNAL);
    } else {
        int32_t n = bender_getnat(argv, 1);
        if (to != INFINITY) bender_addtimeout(to);
        bender_ev_recv(stream, buffer, n, MSG_NOSIGNAL);
    }
}

BENDER_CORE_FN(cfun_stream_chunk,
              "(net/chunk stream nbytes &opt buf timeout)",
              "Same a net/read, but will wait for all n bytes to arrive rather than return early. "
              "Takes an optional timeout in seconds, after which will raise an error.") {
    bender_arity(argc, 2, 4);
    BenderStream *stream = bender_getabstract(argv, 0, &bender_stream_type);
    bender_stream_flags(stream, BENDER_STREAM_READABLE | BENDER_STREAM_SOCKET);
    int32_t n = bender_getnat(argv, 1);
    BenderBuffer *buffer = bender_optbuffer(argv, argc, 2, 10);
    double to = bender_optnumber(argv, argc, 3, INFINITY);
    if (to != INFINITY) bender_addtimeout(to);
    bender_ev_recvchunk(stream, buffer, n, MSG_NOSIGNAL);
}

BENDER_CORE_FN(cfun_stream_recv_from,
              "(net/recv-from stream nbytes buf &opt timeout)",
              "Receives data from a server stream and puts it into a buffer. Returns the socket-address the "
              "packet came from. Takes an optional timeout in seconds, after which will raise an error.") {
    bender_arity(argc, 3, 4);
    BenderStream *stream = bender_getabstract(argv, 0, &bender_stream_type);
    bender_stream_flags(stream, BENDER_STREAM_UDPSERVER | BENDER_STREAM_SOCKET);
    int32_t n = bender_getnat(argv, 1);
    BenderBuffer *buffer = bender_getbuffer(argv, 2);
    double to = bender_optnumber(argv, argc, 3, INFINITY);
    if (to != INFINITY) bender_addtimeout(to);
    bender_ev_recvfrom(stream, buffer, n, MSG_NOSIGNAL);
}

BENDER_CORE_FN(cfun_stream_write,
              "(net/write stream data &opt timeout)",
              "Write data to a stream, suspending the current fiber until the write "
              "completes. Takes an optional timeout in seconds, after which will raise an error. "
              "Returns nil, or raises an error if the write failed.") {
    bender_arity(argc, 2, 3);
    BenderStream *stream = bender_getabstract(argv, 0, &bender_stream_type);
    bender_stream_flags(stream, BENDER_STREAM_WRITABLE | BENDER_STREAM_SOCKET);
    double to = bender_optnumber(argv, argc, 2, INFINITY);
    if (bender_checktype(argv[1], BENDER_BUFFER)) {
        if (to != INFINITY) bender_addtimeout(to);
        bender_ev_send_buffer(stream, bender_getbuffer(argv, 1), MSG_NOSIGNAL);
    } else {
        BenderByteView bytes = bender_getbytes(argv, 1);
        if (to != INFINITY) bender_addtimeout(to);
        bender_ev_send_string(stream, bytes.bytes, MSG_NOSIGNAL);
    }
}

BENDER_CORE_FN(cfun_stream_send_to,
              "(net/send-to stream dest data &opt timeout)",
              "Writes a datagram to a server stream. dest is a the destination address of the packet. "
              "Takes an optional timeout in seconds, after which will raise an error. "
              "Returns stream.") {
    bender_arity(argc, 3, 4);
    BenderStream *stream = bender_getabstract(argv, 0, &bender_stream_type);
    bender_stream_flags(stream, BENDER_STREAM_UDPSERVER | BENDER_STREAM_SOCKET);
    void *dest = bender_getabstract(argv, 1, &bender_address_type);
    double to = bender_optnumber(argv, argc, 3, INFINITY);
    if (bender_checktype(argv[2], BENDER_BUFFER)) {
        if (to != INFINITY) bender_addtimeout(to);
        bender_ev_sendto_buffer(stream, bender_getbuffer(argv, 2), dest, MSG_NOSIGNAL);
    } else {
        BenderByteView bytes = bender_getbytes(argv, 2);
        if (to != INFINITY) bender_addtimeout(to);
        bender_ev_sendto_string(stream, bytes.bytes, dest, MSG_NOSIGNAL);
    }
}

BENDER_CORE_FN(cfun_stream_flush,
              "(net/flush stream)",
              "Make sure that a stream is not buffering any data. This temporarily disables Nagle's algorithm. "
              "Use this to make sure data is sent without delay. Returns stream.") {
    bender_fixarity(argc, 1);
    BenderStream *stream = bender_getabstract(argv, 0, &bender_stream_type);
    bender_stream_flags(stream, BENDER_STREAM_WRITABLE | BENDER_STREAM_SOCKET);
    /* Toggle no delay flag */
    int flag = 1;
    setsockopt((JSock) stream->handle, IPPROTO_TCP, TCP_NODELAY, (char *) &flag, sizeof(int));
    flag = 0;
    setsockopt((JSock) stream->handle, IPPROTO_TCP, TCP_NODELAY, (char *) &flag, sizeof(int));
    return argv[0];
}

struct sockopt_type {
    const char *name;
    int level;
    int optname;
    enum BenderType type;
};

/* List of supported socket options; The type BENDER_POINTER is used
 * for options that require special handling depending on the type. */
static const struct sockopt_type sockopt_type_list[] = {
    { "so-broadcast", SOL_SOCKET, SO_BROADCAST, BENDER_BOOLEAN },
    { "so-reuseaddr", SOL_SOCKET, SO_REUSEADDR, BENDER_BOOLEAN },
    { "so-keepalive", SOL_SOCKET, SO_KEEPALIVE, BENDER_BOOLEAN },
    { "ip-multicast-ttl", IPPROTO_IP, IP_MULTICAST_TTL, BENDER_NUMBER },
    { "ip-add-membership", IPPROTO_IP, IP_ADD_MEMBERSHIP, BENDER_POINTER },
    { "ip-drop-membership", IPPROTO_IP, IP_DROP_MEMBERSHIP, BENDER_POINTER },
#ifndef BENDER_NO_IPV6
    { "ipv6-join-group", IPPROTO_IPV6, IPV6_JOIN_GROUP, BENDER_POINTER },
    { "ipv6-leave-group", IPPROTO_IPV6, IPV6_LEAVE_GROUP, BENDER_POINTER },
    { "ipv6-multicast-hops", IPPROTO_IPV6, IPV6_MULTICAST_HOPS, BENDER_NUMBER },
    { "ipv6-unicast-hops", IPPROTO_IPV6, IPV6_UNICAST_HOPS, BENDER_NUMBER },
#endif
    { NULL, 0, 0, BENDER_POINTER }
};

BENDER_CORE_FN(cfun_net_setsockopt,
              "(net/setsockopt stream option val)",
              "Set socket options. Supported options and associated value "
              "types are:\n"
              "\n"
              "* `:so-broadcast` - boolean\n"
              "* `:so-reuseaddr` - boolean\n"
              "* `:so-keepalive` - boolean\n"
              "* `:ip-multicast-ttl` - number\n"
              "* `:ip-add-membership` - string\n"
              "* `:ip-drop-membership` - string\n"
              "* `:ipv6-join-group` - string\n"
              "* `:ipv6-leave-group` - string\n"
              "* `:ipv6-multicast-hops` - number\n"
              "* `:ipv6-unicast-hops` - number\n"
             ) {
    bender_arity(argc, 3, 3);
    BenderStream *stream = bender_getabstract(argv, 0, &bender_stream_type);
    bender_stream_flags(stream, BENDER_STREAM_SOCKET);
    BenderKeyword optstr = bender_getkeyword(argv, 1);

    const struct sockopt_type *st = sockopt_type_list;
    while (st->name) {
        if (bender_cstrcmp(optstr, st->name) == 0) {
            break;
        }
        st++;
    }

    if (st->name == NULL) {
        bender_panicf("unknown socket option %q", argv[1]);
    }

    union {
        unsigned char v_uchar;
        int v_int;
        struct ip_mreq v_mreq;
#ifndef BENDER_NO_IPV6
        struct ipv6_mreq v_mreq6;
#endif
    } val;

    void *optval = (void *)&val;
    socklen_t optlen = 0;

    if (st->type == BENDER_BOOLEAN) {
        val.v_int = bender_getboolean(argv, 2);
        optlen = sizeof(val.v_int);
    } else if (st->type == BENDER_NUMBER) {
#if defined(BENDER_BSD) || defined(BENDER_ILLUMOS)
        int v_int = bender_getinteger(argv, 2);
        if (st->optname == IP_MULTICAST_TTL) {
            val.v_uchar = v_int;
            optlen = sizeof(val.v_uchar);
        } else {
            val.v_int = v_int;
            optlen = sizeof(val.v_int);
        }
#else
        val.v_int = bender_getinteger(argv, 2);
        optlen = sizeof(val.v_int);
#endif
    } else if (st->optname == IP_ADD_MEMBERSHIP || st->optname == IP_DROP_MEMBERSHIP) {
        const char *addr = bender_getcstring(argv, 2);
        memset(&val.v_mreq, 0, sizeof val.v_mreq);
        val.v_mreq.imr_interface.s_addr = htonl(INADDR_ANY);
        inet_pton(AF_INET, addr, &val.v_mreq.imr_multiaddr.s_addr);
        optlen = sizeof(val.v_mreq);
#ifndef BENDER_NO_IPV6
    } else if (st->optname == IPV6_JOIN_GROUP || st->optname == IPV6_LEAVE_GROUP) {
        const char *addr = bender_getcstring(argv, 2);
        memset(&val.v_mreq6, 0, sizeof val.v_mreq6);
        val.v_mreq6.ipv6mr_interface = 0;
        inet_pton(AF_INET6, addr, &val.v_mreq6.ipv6mr_multiaddr);
        optlen = sizeof(val.v_mreq6);
#endif
    } else {
        bender_panicf("invalid socket option type");
    }

    bender_assert(optlen != 0, "invalid socket option value");

    int r = setsockopt((JSock) stream->handle, st->level, st->optname, optval, optlen);
    if (r == -1) {
        bender_panicf("setsockopt(%q): %s", argv[1], bender_strerror(errno));
    }

    return bender_wrap_nil();
}

static const BenderMethod net_stream_methods[] = {
    {"chunk", cfun_stream_chunk},
    {"close", bender_cfun_stream_close},
    {"read", cfun_stream_read},
    {"write", cfun_stream_write},
    {"flush", cfun_stream_flush},
    {"accept", cfun_stream_accept},
    {"accept-loop", cfun_stream_accept_loop},
    {"send-to", cfun_stream_send_to},
    {"recv-from", cfun_stream_recv_from},
    {"evread", bender_cfun_stream_read},
    {"evchunk", bender_cfun_stream_chunk},
    {"evwrite", bender_cfun_stream_write},
    {"shutdown", cfun_net_shutdown},
    {"setsockopt", cfun_net_setsockopt},
    {NULL, NULL}
};

static BenderStream *make_stream(JSock handle, uint32_t flags) {
    return bender_stream((BenderHandle) handle, flags | BENDER_STREAM_SOCKET | BENDER_STREAM_NODUPS, net_stream_methods);
}

void bender_lib_net(BenderTable *env) {
    BenderRegExt net_cfuns[] = {
        BENDER_CORE_REG("net/address", cfun_net_sockaddr),
        BENDER_CORE_REG("net/listen", cfun_net_listen),
        BENDER_CORE_REG("net/socket", cfun_net_socket),
        BENDER_CORE_REG("net/accept", cfun_stream_accept),
        BENDER_CORE_REG("net/accept-loop", cfun_stream_accept_loop),
        BENDER_CORE_REG("net/read", cfun_stream_read),
        BENDER_CORE_REG("net/chunk", cfun_stream_chunk),
        BENDER_CORE_REG("net/write", cfun_stream_write),
        BENDER_CORE_REG("net/send-to", cfun_stream_send_to),
        BENDER_CORE_REG("net/recv-from", cfun_stream_recv_from),
        BENDER_CORE_REG("net/flush", cfun_stream_flush),
        BENDER_CORE_REG("net/connect", cfun_net_connect),
        BENDER_CORE_REG("net/shutdown", cfun_net_shutdown),
        BENDER_CORE_REG("net/peername", cfun_net_getpeername),
        BENDER_CORE_REG("net/localname", cfun_net_getsockname),
        BENDER_CORE_REG("net/address-unpack", cfun_net_address_unpack),
        BENDER_CORE_REG("net/setsockopt", cfun_net_setsockopt),
        BENDER_REG_END
    };
    bender_core_cfuns_ext(env, NULL, net_cfuns);
}

void bender_net_init(void) {
#ifdef BENDER_WINDOWS
    WSADATA wsaData;
    bender_assert(!WSAStartup(MAKEWORD(2, 2), &wsaData), "could not start winsock");
    bender_vm.connect_ex_loaded = 0;
    bender_vm.connect_ex = NULL;
#endif
}

void bender_net_deinit(void) {
#ifdef BENDER_WINDOWS
    WSACleanup();
#endif
}

#endif
