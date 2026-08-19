/*
 * Copyright 2026 TeamMyonlang
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/* Feature-test macros before any system headers (glibc gates several
 * networking prototypes and non-blocking flags behind these). */
#ifndef _POSIX_C_SOURCE
#  define _POSIX_C_SOURCE 200809L
#endif
#ifndef _DEFAULT_SOURCE
#  define _DEFAULT_SOURCE 1
#endif
/* macOS: SO_NOSIGPIPE and the BSD socket extras live behind _DARWIN_C_SOURCE.
 * Must be set before <sys/socket.h> is pulled in. */
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#  define _DARWIN_C_SOURCE 1
#endif

#include "platform.h"
#include "net.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <limits.h>

/*
 * BSD-socket implementation is used on every POSIX target (Linux, macOS, the
 * BSDs).  The previous `#if defined(__linux__)` guard sent macOS/BSD to the
 * unsupported stub even though they share the exact same socket API, which
 * disabled myon.net entirely on those platforms.  See platform.h
 * (MYON_HAVE_POSIX_SOCKETS).
 */
#if defined(MYON_HAVE_POSIX_SOCKETS)
#  define MYON_NET_POSIX 1
#endif

#ifdef MYON_NET_POSIX

#include <sys/socket.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

#define NET_MAX_SOCKETS 256

struct NetState {
    int fds[NET_MAX_SOCKETS];   /* -1 if slot free */
    int kinds[NET_MAX_SOCKETS]; /* 0=TCP, 1=UDP */
};

/* ------------------------------------------------------------------ */
/* helpers                                                             */
/* ------------------------------------------------------------------ */

static char *dup_errno(const char *prefix) {
    const char *e = strerror(errno);
    size_t n = strlen(prefix) + strlen(e) + 3;
    char *m = (char *)malloc(n);
    if (m) snprintf(m, n, "%s: %s", prefix, e);
    return m;
}

static char *dup_msg(const char *s) {
    char *m = (char *)malloc(strlen(s) + 1);
    if (m) strcpy(m, s);
    return m;
}

static int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

/*
 * SIGPIPE suppression.
 *
 * Writing to a socket whose peer has closed raises SIGPIPE, which kills the
 * process by default.  There are two portable ways to avoid this:
 *
 *   Linux : pass MSG_NOSIGNAL to each send()          (per-call flag)
 *   macOS/: set the SO_NOSIGPIPE socket option once   (per-socket)
 *   BSD     at socket-creation time; MSG_NOSIGNAL does not exist there.
 *
 * MYON_SEND_FLAGS is the flag to OR into send(); net_suppress_sigpipe() sets
 * the socket option on the platforms that need it (a no-op elsewhere).  Between
 * the two, no send() on any supported platform can take the process down with
 * SIGPIPE. */
#if defined(MYON_HAVE_MSG_NOSIGNAL)
#  define MYON_SEND_FLAGS MSG_NOSIGNAL
#else
#  define MYON_SEND_FLAGS 0
#endif

static void net_suppress_sigpipe(int fd) {
#if defined(MYON_HAVE_SO_NOSIGPIPE)
    int on = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &on, sizeof(on));
#else
    (void)fd;
#endif
}

static int valid_id(NetState *st, int id) {
    return id >= 0 && id < NET_MAX_SOCKETS && st->fds[id] >= 0;
}

static int alloc_slot(NetState *st, int fd, int kind) {
    for (int i = 0; i < NET_MAX_SOCKETS; i++) {
        if (st->fds[i] < 0) {
            st->fds[i] = fd;
            st->kinds[i] = kind;
            return i;
        }
    }
    return -1;
}

/* Build a "host:port" string from a sockaddr_in (malloc'd). */
static char *addr_to_str(const struct sockaddr_in *sa) {
    char ip[INET_ADDRSTRLEN];
    if (!inet_ntop(AF_INET, &sa->sin_addr, ip, sizeof(ip))) return NULL;
    char buf[INET_ADDRSTRLEN + 8];
    snprintf(buf, sizeof(buf), "%s:%d", ip, (int)ntohs(sa->sin_port));
    return dup_msg(buf);
}

/* Resolve `host` (a literal IPv4 address or a DNS name) into `sa`.
 *
 * Phase5.1, Step4: `host` may now be a hostname (e.g. "example.com").
 * Literal IPv4 addresses keep the old fast path (no name resolution cost);
 * anything else is resolved with getaddrinfo().  `kind` (0=TCP, 1=UDP) picks
 * the ai_socktype hint so the resolver returns an address appropriate for the
 * caller's socket type. */
static int fill_addr(struct sockaddr_in *sa, const char *host, int port,
                     int kind, char **err_msg) {
    memset(sa, 0, sizeof(*sa));
    sa->sin_family = AF_INET;
    /* A TCP/UDP port is 16-bit.  Casting an out-of-range signed `port` straight
     * to unsigned short would silently wrap (e.g. 65536 -> 0, -1 -> 65535) and
     * connect/bind somewhere unintended.  Reject anything outside [0, 65535]
     * (0 is allowed so bind() can request an ephemeral port). */
    if (port < 0 || port > 65535) {
        if (err_msg) *err_msg = dup_msg("port out of range (0-65535)");
        return -1;
    }
    sa->sin_port = htons((unsigned short)port);
    if (!host || host[0] == '\0' || strcmp(host, "0.0.0.0") == 0) {
        sa->sin_addr.s_addr = INADDR_ANY;
        return 0;
    }
    /* fast path: already a literal IPv4 address */
    if (inet_pton(AF_INET, host, &sa->sin_addr) == 1) {
        return 0;
    }
    /* otherwise resolve the hostname via DNS (getaddrinfo) */
    struct addrinfo hints, *result = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = (kind == 1) ? SOCK_DGRAM : SOCK_STREAM;
    int rc = getaddrinfo(host, NULL, &hints, &result);
    if (rc != 0 || !result) {
        if (err_msg) {
            const char *gerr = gai_strerror(rc);
            size_t n = strlen(host) + strlen(gerr) + 32;
            char *m = (char *)malloc(n);
            if (m) snprintf(m, n, "cannot resolve host '%s': %s", host, gerr);
            *err_msg = m ? m : dup_msg("cannot resolve host");
        }
        if (result) freeaddrinfo(result);
        return -1;
    }
    /* use the first candidate (round-robin/fallback is out of scope) */
    const struct sockaddr_in *ra = (const struct sockaddr_in *)result->ai_addr;
    sa->sin_addr = ra->sin_addr;
    freeaddrinfo(result);
    return 0;
}

/* ------------------------------------------------------------------ */
/* lifecycle                                                           */
/* ------------------------------------------------------------------ */

NetState *net_state_create(void) {
    NetState *st = (NetState *)malloc(sizeof(NetState));
    if (!st) return NULL;
    for (int i = 0; i < NET_MAX_SOCKETS; i++) { st->fds[i] = -1; st->kinds[i] = 0; }
    return st;
}

void net_state_destroy(NetState *st) {
    if (!st) return;
    for (int i = 0; i < NET_MAX_SOCKETS; i++)
        if (st->fds[i] >= 0) close(st->fds[i]);
    free(st);
}

int net_supported(void) { return 1; }

/* ------------------------------------------------------------------ */
/* socket operations                                                   */
/* ------------------------------------------------------------------ */

int net_socket_create(NetState *st, int kind, char **err_msg) {
    int type = (kind == 1) ? SOCK_DGRAM : SOCK_STREAM;
    int fd = socket(AF_INET, type, 0);
    if (fd < 0) { if (err_msg) *err_msg = dup_errno("socket"); return -1; }
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    net_suppress_sigpipe(fd); /* macOS/BSD: block SIGPIPE on writes (no-op on Linux) */
    if (set_nonblocking(fd) < 0) {
        if (err_msg) *err_msg = dup_errno("fcntl(O_NONBLOCK)");
        close(fd);
        return -1;
    }
    int id = alloc_slot(st, fd, kind);
    if (id < 0) { if (err_msg) *err_msg = dup_msg("too many open sockets"); close(fd); return -1; }
    return id;
}

int net_bind(NetState *st, int sock_id, const char *host, int port, char **err_msg) {
    if (!valid_id(st, sock_id)) { if (err_msg) *err_msg = dup_msg("invalid socket id"); return -1; }
    struct sockaddr_in sa;
    if (fill_addr(&sa, host, port, st->kinds[sock_id], err_msg) < 0) return -1;
    if (bind(st->fds[sock_id], (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        if (err_msg) *err_msg = dup_errno("bind");
        return -1;
    }
    return 0;
}

int net_listen(NetState *st, int sock_id, int backlog, char **err_msg) {
    if (!valid_id(st, sock_id)) { if (err_msg) *err_msg = dup_msg("invalid socket id"); return -1; }
    if (backlog <= 0) backlog = 16;
    if (listen(st->fds[sock_id], backlog) < 0) {
        if (err_msg) *err_msg = dup_errno("listen");
        return -1;
    }
    return 0;
}

int net_local_port(NetState *st, int sock_id, char **err_msg) {
    if (!valid_id(st, sock_id)) { if (err_msg) *err_msg = dup_msg("invalid socket id"); return -1; }
    struct sockaddr_in sa;
    socklen_t len = sizeof(sa);
    if (getsockname(st->fds[sock_id], (struct sockaddr *)&sa, &len) < 0) {
        if (err_msg) *err_msg = dup_errno("getsockname");
        return -1;
    }
    return (int)ntohs(sa.sin_port);
}

int net_try_accept(NetState *st, int listen_sock_id, char **peer_addr_out,
                   char **err_msg) {
    if (!valid_id(st, listen_sock_id)) { if (err_msg) *err_msg = dup_msg("invalid socket id"); return -1; }
    struct sockaddr_in peer;
    socklen_t plen = sizeof(peer);
    int cfd = accept(st->fds[listen_sock_id], (struct sockaddr *)&peer, &plen);
    if (cfd < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return -2;
        if (err_msg) *err_msg = dup_errno("accept");
        return -1;
    }
    net_suppress_sigpipe(cfd); /* macOS/BSD: accepted fd also needs SO_NOSIGPIPE */
    if (set_nonblocking(cfd) < 0) {
        if (err_msg) *err_msg = dup_errno("fcntl(O_NONBLOCK)");
        close(cfd);
        return -1;
    }
    int id = alloc_slot(st, cfd, 0);
    if (id < 0) { if (err_msg) *err_msg = dup_msg("too many open sockets"); close(cfd); return -1; }
    if (peer_addr_out) *peer_addr_out = addr_to_str(&peer);
    return id;
}

int net_connect(NetState *st, int sock_id, const char *host, int port, char **err_msg) {
    if (!valid_id(st, sock_id)) { if (err_msg) *err_msg = dup_msg("invalid socket id"); return -1; }
    struct sockaddr_in sa;
    if (fill_addr(&sa, host, port, st->kinds[sock_id], err_msg) < 0) return -1;
    int rc = connect(st->fds[sock_id], (struct sockaddr *)&sa, sizeof(sa));
    if (rc == 0) return 0;
    if (errno == EINPROGRESS || errno == EWOULDBLOCK || errno == EALREADY) return -2;
    if (err_msg) *err_msg = dup_errno("connect");
    return -1;
}

int net_connect_check(NetState *st, int sock_id, char **err_msg) {
    if (!valid_id(st, sock_id)) { if (err_msg) *err_msg = dup_msg("invalid socket id"); return -1; }
    int soerr = 0;
    socklen_t len = sizeof(soerr);
    if (getsockopt(st->fds[sock_id], SOL_SOCKET, SO_ERROR, &soerr, &len) < 0) {
        if (err_msg) *err_msg = dup_errno("getsockopt(SO_ERROR)");
        return -1;
    }
    if (soerr == 0) return 0;
    if (soerr == EINPROGRESS || soerr == EALREADY) return -2;
    errno = soerr;
    if (err_msg) *err_msg = dup_errno("connect");
    return -1;
}

long long net_send(NetState *st, int sock_id, const char *data, long long len, char **err_msg) {
    if (!valid_id(st, sock_id)) { if (err_msg) *err_msg = dup_msg("invalid socket id"); return -1; }
    /* C-1 fix: len is a signed VM value.  A negative length would cast into a
     * huge size_t and let send() read far past `data`.  Reject len < 0 and clamp
     * to SSIZE_MAX so the size_t cast can never wrap. */
    if (len < 0) { if (err_msg) *err_msg = dup_msg("send: negative length"); return -1; }
    if (len > (long long)SSIZE_MAX) len = (long long)SSIZE_MAX;
    /* MYON_SEND_FLAGS is MSG_NOSIGNAL on Linux and 0 on macOS/BSD (where the
     * SO_NOSIGPIPE option set at socket creation suppresses SIGPIPE instead). */
    ssize_t n = send(st->fds[sock_id], data, (size_t)len, MYON_SEND_FLAGS);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return -2;
        if (err_msg) *err_msg = dup_errno("send");
        return -1;
    }
    return (long long)n;
}

long long net_recv(NetState *st, int sock_id, char *buf, long long buf_len, char **err_msg) {
    if (!valid_id(st, sock_id)) { if (err_msg) *err_msg = dup_msg("invalid socket id"); return -1; }
    /* C-1 fix: buf_len < 0 would become a huge size_t here, letting recv() write
     * past the interpreter-owned buffer.  Reject negatives and clamp to SSIZE_MAX. */
    if (buf_len < 0) { if (err_msg) *err_msg = dup_msg("recv: negative length"); return -1; }
    if (buf_len > (long long)SSIZE_MAX) buf_len = (long long)SSIZE_MAX;
    ssize_t n = recv(st->fds[sock_id], buf, (size_t)buf_len, 0);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return -2;
        if (err_msg) *err_msg = dup_errno("recv");
        return -1;
    }
    return (long long)n; /* 0 == peer closed (EOF) */
}

long long net_sendto(NetState *st, int sock_id, const char *data, long long len,
                     const char *host, int port, char **err_msg) {
    if (!valid_id(st, sock_id)) { if (err_msg) *err_msg = dup_msg("invalid socket id"); return -1; }
    struct sockaddr_in sa;
    if (fill_addr(&sa, host, port, st->kinds[sock_id], err_msg) < 0) return -1;
    /* C-1 fix: same signed-to-size_t bug as net_send(); reject negative len and
     * clamp to SSIZE_MAX before the cast. */
    if (len < 0) { if (err_msg) *err_msg = dup_msg("sendto: negative length"); return -1; }
    if (len > (long long)SSIZE_MAX) len = (long long)SSIZE_MAX;
    ssize_t n = sendto(st->fds[sock_id], data, (size_t)len, 0,
                       (struct sockaddr *)&sa, sizeof(sa));
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return -2;
        if (err_msg) *err_msg = dup_errno("sendto");
        return -1;
    }
    return (long long)n;
}

long long net_recvfrom(NetState *st, int sock_id, char *buf, long long buf_len,
                       char **from_addr_out, char **err_msg) {
    if (!valid_id(st, sock_id)) { if (err_msg) *err_msg = dup_msg("invalid socket id"); return -1; }
    struct sockaddr_in from;
    socklen_t flen = sizeof(from);
    /* C-1 fix: same signed-to-size_t bug as net_recv(); reject negative buf_len
     * and clamp to SSIZE_MAX before the cast. */
    if (buf_len < 0) { if (err_msg) *err_msg = dup_msg("recvfrom: negative length"); return -1; }
    if (buf_len > (long long)SSIZE_MAX) buf_len = (long long)SSIZE_MAX;
    ssize_t n = recvfrom(st->fds[sock_id], buf, (size_t)buf_len, 0,
                         (struct sockaddr *)&from, &flen);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return -2;
        if (err_msg) *err_msg = dup_errno("recvfrom");
        return -1;
    }
    if (from_addr_out) *from_addr_out = addr_to_str(&from);
    return (long long)n;
}

int net_raw_fd(NetState *st, int sock_id) {
    if (!valid_id(st, sock_id)) return -1;
    return st->fds[sock_id];
}

void net_sync_wait_fd(int fd, int for_write) {
    if (fd < 0) return;
    fd_set fds; FD_ZERO(&fds); FD_SET(fd, &fds);
    if (for_write) select(fd + 1, NULL, &fds, NULL, NULL);
    else           select(fd + 1, &fds, NULL, NULL, NULL);
}

void net_close(NetState *st, int sock_id) {
    if (!valid_id(st, sock_id)) return;
    close(st->fds[sock_id]);
    st->fds[sock_id] = -1;
}

#elif defined(_WIN32)

/* ================================================================== */
/* Windows (Winsock2) implementation                                  */
/* ================================================================== */
/*
 * Phase5, Step2 (Windows): a Winsock2-based port of the Linux socket layer
 * above.  The Linux block (#ifdef MYON_NET_POSIX) is left completely
 * unchanged; this branch only compiles on _WIN32 (native MSYS2/MinGW-w64 or a
 * MinGW-w64 cross build).
 *
 * The Winsock port follows the Win32 sockets specification (Microsoft Learn,
 * "Windows Sockets 2").  The notable differences from BSD/POSIX sockets that
 * shape this file are:
 *
 *   * winsock2.h MUST be included before windows.h, otherwise the older
 *     winsock.h gets pulled in via windows.h and the declarations clash.  We
 *     also define WIN32_LEAN_AND_MEAN so that including windows.h (which
 *     winsock2.h drags in) does not itself re-include winsock.h.
 *   * Sockets are of type SOCKET (an UINT_PTR), not int; the invalid value is
 *     INVALID_SOCKET (not -1) and failing calls return SOCKET_ERROR.
 *   * Winsock must be initialised with WSAStartup(MAKEWORD(2,2), ...) and torn
 *     down with a matching WSACleanup().  Both are reference counted by the
 *     Winsock DLL: only the final WSACleanup performs the real cleanup, so we
 *     mirror that with a simple process-wide init counter guarded by the calls
 *     being confined to net_state_create/net_state_destroy.
 *   * Non-blocking mode is selected with ioctlsocket(s, FIONBIO, &mode) where
 *     mode != 0, not fcntl(O_NONBLOCK).
 *   * A socket is closed with closesocket(), not close().
 *   * Per-call error codes come from WSAGetLastError() (WSAExxx values), not
 *     errno; "would block" is WSAEWOULDBLOCK.  We format them into text with
 *     FormatMessageA, mirroring the FFI Windows layer (ffi_platform.c).
 *   * send/recv/sendto/recvfrom take an int length and a char * buffer and
 *     return int, so the long long lengths coming from the interpreter are
 *     clamped to INT_MAX before the call.
 *   * getsockopt/getsockname take (char *, int *) rather than (void *,
 *     socklen_t *).
 *
 * getaddrinfo/freeaddrinfo behave the same as on POSIX (they come from
 * ws2tcpip.h), so the DNS-resolution logic is a near-verbatim port.
 */

#ifndef WIN32_LEAN_AND_MEAN
#  define WIN32_LEAN_AND_MEAN 1
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <limits.h>

#define NET_MAX_SOCKETS 256

/* Windows-specific NetState: the fd table must hold SOCKET (UINT_PTR) values,
 * which do not fit in the `int fds[]` the Linux struct uses.  net.h forward-
 * declares NetState as an opaque type, so a distinct definition here is fine. */
struct NetState {
    SOCKET socks[NET_MAX_SOCKETS]; /* INVALID_SOCKET if slot free */
    int    kinds[NET_MAX_SOCKETS]; /* 0=TCP, 1=UDP */
};

/* ------------------------------------------------------------------ */
/* Winsock global init (reference counted, matching WSAStartup/WSACleanup) */
/* ------------------------------------------------------------------ */
/*
 * WSAStartup/WSACleanup are themselves reference counted inside the Winsock
 * DLL, but we still keep our own counter so that:
 *   (a) we only call WSAStartup the first time a NetState is created (and can
 *       surface an init failure as a NULL from net_state_create), and
 *   (b) we call WSACleanup exactly once per successful WSAStartup, on the last
 *       NetState destruction — avoiding both a leaked init and a premature
 *       teardown while another NetState is still live.
 * Myon runs its interpreter single-threaded, so a plain counter is sufficient;
 * no locking is required here. */
static int net_wsa_refcount = 0;

static int wsa_global_init(void) {
    if (net_wsa_refcount == 0) {
        WSADATA wsa;
        int rc = WSAStartup(MAKEWORD(2, 2), &wsa);
        if (rc != 0) return -1; /* WSAStartup returns the error code directly */
    }
    net_wsa_refcount++;
    return 0;
}

static void wsa_global_shutdown(void) {
    if (net_wsa_refcount > 0) {
        net_wsa_refcount--;
        if (net_wsa_refcount == 0) WSACleanup();
    }
}

/* ------------------------------------------------------------------ */
/* helpers                                                             */
/* ------------------------------------------------------------------ */

static char *dup_msg(const char *s) {
    char *m = (char *)malloc(strlen(s) + 1);
    if (m) strcpy(m, s);
    return m;
}

/* Turn a specific WSA error code into a "<prefix>: <text>" heap string.
 *
 * Uses FormatMessageA the same way ffi_platform.c does for GetLastError: the
 * ALLOCATE_BUFFER flag makes the API LocalAlloc a buffer we must LocalFree,
 * and IGNORE_INSERTS is mandatory when formatting an arbitrary system code so
 * the formatter does not try to read insert arguments we never supply.  The
 * result is copied into a malloc'd string so the interpreter frees it with the
 * same free() it uses on the Linux path. */
static char *dup_wsa_error(const char *prefix, int err) {
    LPSTR sysbuf = NULL;
    DWORD len = FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER |
        FORMAT_MESSAGE_FROM_SYSTEM |
        FORMAT_MESSAGE_IGNORE_INSERTS,
        NULL,
        (DWORD)err,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        (LPSTR)&sysbuf,
        0,
        NULL);

    const char *text;
    char numbuf[64];
    if (len == 0 || sysbuf == NULL) {
        (void)snprintf(numbuf, sizeof(numbuf), "winsock error %d", err);
        text = numbuf;
    } else {
        /* trim the trailing "\r\n" / spaces / '.' Windows appends, so the text
         * lines up with the terse Linux strerror() strings */
        while (len > 0 &&
               (sysbuf[len - 1] == '\r' || sysbuf[len - 1] == '\n' ||
                sysbuf[len - 1] == ' '  || sysbuf[len - 1] == '.')) {
            sysbuf[--len] = '\0';
        }
        text = sysbuf;
    }

    size_t n = strlen(prefix) + strlen(text) + 3;
    char *m = (char *)malloc(n);
    if (m) snprintf(m, n, "%s: %s", prefix, text);

    if (sysbuf) LocalFree(sysbuf);
    return m;
}

/* Convenience: format the *current* WSAGetLastError() code. */
static char *dup_wsa_last(const char *prefix) {
    return dup_wsa_error(prefix, WSAGetLastError());
}

static int set_nonblocking(SOCKET s) {
    u_long mode = 1; /* non-zero => non-blocking (FIONBIO) */
    return (ioctlsocket(s, FIONBIO, &mode) == SOCKET_ERROR) ? -1 : 0;
}

static int valid_id(NetState *st, int id) {
    return id >= 0 && id < NET_MAX_SOCKETS &&
           st->socks[id] != INVALID_SOCKET;
}

static int alloc_slot(NetState *st, SOCKET s, int kind) {
    for (int i = 0; i < NET_MAX_SOCKETS; i++) {
        if (st->socks[i] == INVALID_SOCKET) {
            st->socks[i] = s;
            st->kinds[i] = kind;
            return i;
        }
    }
    return -1;
}

/* Build a "host:port" string from a sockaddr_in (malloc'd). */
static char *addr_to_str(const struct sockaddr_in *sa) {
    char ip[INET_ADDRSTRLEN];
    if (!inet_ntop(AF_INET, (void *)&sa->sin_addr, ip, sizeof(ip)))
        return NULL;
    char buf[INET_ADDRSTRLEN + 8];
    snprintf(buf, sizeof(buf), "%s:%d", ip, (int)ntohs(sa->sin_port));
    return dup_msg(buf);
}

/* Resolve `host` (literal IPv4 address or DNS name) into `sa`.  This mirrors
 * the Linux fill_addr(): literal addresses keep the fast inet_pton() path, and
 * everything else is resolved with getaddrinfo() (available via ws2tcpip.h). */
static int fill_addr(struct sockaddr_in *sa, const char *host, int port,
                     int kind, char **err_msg) {
    memset(sa, 0, sizeof(*sa));
    sa->sin_family = AF_INET;
    /* A TCP/UDP port is 16-bit.  Casting an out-of-range signed `port` straight
     * to unsigned short would silently wrap (e.g. 65536 -> 0, -1 -> 65535) and
     * connect/bind somewhere unintended.  Reject anything outside [0, 65535]
     * (0 is allowed so bind() can request an ephemeral port). */
    if (port < 0 || port > 65535) {
        if (err_msg) *err_msg = dup_msg("port out of range (0-65535)");
        return -1;
    }
    sa->sin_port = htons((unsigned short)port);
    if (!host || host[0] == '\0' || strcmp(host, "0.0.0.0") == 0) {
        sa->sin_addr.s_addr = INADDR_ANY;
        return 0;
    }
    /* fast path: already a literal IPv4 address */
    if (inet_pton(AF_INET, host, &sa->sin_addr) == 1) {
        return 0;
    }
    /* otherwise resolve the hostname via DNS (getaddrinfo) */
    struct addrinfo hints, *result = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = (kind == 1) ? SOCK_DGRAM : SOCK_STREAM;
    int rc = getaddrinfo(host, NULL, &hints, &result);
    if (rc != 0 || !result) {
        if (err_msg) {
            /* getaddrinfo failures report through WSAGetLastError() on Winsock
             * (gai_strerror is not thread-safe there), so format that. */
            char prefix[256];
            (void)snprintf(prefix, sizeof(prefix),
                           "cannot resolve host '%s'", host);
            *err_msg = dup_wsa_last(prefix);
        }
        if (result) freeaddrinfo(result);
        return -1;
    }
    /* use the first candidate (round-robin/fallback is out of scope) */
    const struct sockaddr_in *ra = (const struct sockaddr_in *)result->ai_addr;
    sa->sin_addr = ra->sin_addr;
    freeaddrinfo(result);
    return 0;
}

/* Clamp a long long length down to what the Winsock int-length calls accept. */
static int clamp_len(long long len) {
    if (len < 0) return 0;
    if (len > (long long)INT_MAX) return INT_MAX;
    return (int)len;
}

/* ------------------------------------------------------------------ */
/* lifecycle                                                           */
/* ------------------------------------------------------------------ */

NetState *net_state_create(void) {
    if (wsa_global_init() < 0) return NULL; /* WSAStartup failed */
    NetState *st = (NetState *)malloc(sizeof(NetState));
    if (!st) { wsa_global_shutdown(); return NULL; }
    for (int i = 0; i < NET_MAX_SOCKETS; i++) {
        st->socks[i] = INVALID_SOCKET;
        st->kinds[i] = 0;
    }
    return st;
}

void net_state_destroy(NetState *st) {
    if (!st) return;
    for (int i = 0; i < NET_MAX_SOCKETS; i++)
        if (st->socks[i] != INVALID_SOCKET) closesocket(st->socks[i]);
    free(st);
    wsa_global_shutdown();
}

int net_supported(void) { return 1; }

/* ------------------------------------------------------------------ */
/* socket operations                                                   */
/* ------------------------------------------------------------------ */

int net_socket_create(NetState *st, int kind, char **err_msg) {
    int type = (kind == 1) ? SOCK_DGRAM : SOCK_STREAM;
    SOCKET s = socket(AF_INET, type, 0);
    if (s == INVALID_SOCKET) {
        if (err_msg) *err_msg = dup_wsa_last("socket");
        return -1;
    }
    BOOL one = TRUE;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (const char *)&one, sizeof(one));
    if (set_nonblocking(s) < 0) {
        if (err_msg) *err_msg = dup_wsa_last("ioctlsocket(FIONBIO)");
        closesocket(s);
        return -1;
    }
    int id = alloc_slot(st, s, kind);
    if (id < 0) {
        if (err_msg) *err_msg = dup_msg("too many open sockets");
        closesocket(s);
        return -1;
    }
    return id;
}

int net_bind(NetState *st, int sock_id, const char *host, int port, char **err_msg) {
    if (!valid_id(st, sock_id)) { if (err_msg) *err_msg = dup_msg("invalid socket id"); return -1; }
    struct sockaddr_in sa;
    if (fill_addr(&sa, host, port, st->kinds[sock_id], err_msg) < 0) return -1;
    if (bind(st->socks[sock_id], (struct sockaddr *)&sa, sizeof(sa)) == SOCKET_ERROR) {
        if (err_msg) *err_msg = dup_wsa_last("bind");
        return -1;
    }
    return 0;
}

int net_listen(NetState *st, int sock_id, int backlog, char **err_msg) {
    if (!valid_id(st, sock_id)) { if (err_msg) *err_msg = dup_msg("invalid socket id"); return -1; }
    if (backlog <= 0) backlog = 16;
    if (listen(st->socks[sock_id], backlog) == SOCKET_ERROR) {
        if (err_msg) *err_msg = dup_wsa_last("listen");
        return -1;
    }
    return 0;
}

int net_local_port(NetState *st, int sock_id, char **err_msg) {
    if (!valid_id(st, sock_id)) { if (err_msg) *err_msg = dup_msg("invalid socket id"); return -1; }
    struct sockaddr_in sa;
    int len = (int)sizeof(sa); /* Winsock getsockname takes int *, not socklen_t * */
    if (getsockname(st->socks[sock_id], (struct sockaddr *)&sa, &len) == SOCKET_ERROR) {
        if (err_msg) *err_msg = dup_wsa_last("getsockname");
        return -1;
    }
    return (int)ntohs(sa.sin_port);
}

int net_try_accept(NetState *st, int listen_sock_id, char **peer_addr_out,
                   char **err_msg) {
    if (!valid_id(st, listen_sock_id)) { if (err_msg) *err_msg = dup_msg("invalid socket id"); return -1; }
    struct sockaddr_in peer;
    int plen = (int)sizeof(peer);
    SOCKET cs = accept(st->socks[listen_sock_id], (struct sockaddr *)&peer, &plen);
    if (cs == INVALID_SOCKET) {
        if (WSAGetLastError() == WSAEWOULDBLOCK) return -2;
        if (err_msg) *err_msg = dup_wsa_last("accept");
        return -1;
    }
    if (set_nonblocking(cs) < 0) {
        if (err_msg) *err_msg = dup_wsa_last("ioctlsocket(FIONBIO)");
        closesocket(cs);
        return -1;
    }
    int id = alloc_slot(st, cs, 0);
    if (id < 0) {
        if (err_msg) *err_msg = dup_msg("too many open sockets");
        closesocket(cs);
        return -1;
    }
    if (peer_addr_out) *peer_addr_out = addr_to_str(&peer);
    return id;
}

int net_connect(NetState *st, int sock_id, const char *host, int port, char **err_msg) {
    if (!valid_id(st, sock_id)) { if (err_msg) *err_msg = dup_msg("invalid socket id"); return -1; }
    struct sockaddr_in sa;
    if (fill_addr(&sa, host, port, st->kinds[sock_id], err_msg) < 0) return -1;
    int rc = connect(st->socks[sock_id], (struct sockaddr *)&sa, sizeof(sa));
    if (rc == 0) return 0;
    int e = WSAGetLastError();
    /* Non-blocking connect in progress.  Per the Winsock spec the first call
     * returns WSAEWOULDBLOCK; a repeated connect() while still pending returns
     * WSAEALREADY (or WSAEINVAL on some stacks), which we also treat as
     * "still in progress" to match the Linux EINPROGRESS/EALREADY handling. */
    if (e == WSAEWOULDBLOCK || e == WSAEALREADY || e == WSAEINVAL) return -2;
    if (err_msg) *err_msg = dup_wsa_error("connect", e);
    return -1;
}

int net_connect_check(NetState *st, int sock_id, char **err_msg) {
    if (!valid_id(st, sock_id)) { if (err_msg) *err_msg = dup_msg("invalid socket id"); return -1; }
    int soerr = 0;
    int len = (int)sizeof(soerr); /* Winsock getsockopt takes int * for optlen */
    if (getsockopt(st->socks[sock_id], SOL_SOCKET, SO_ERROR,
                   (char *)&soerr, &len) == SOCKET_ERROR) {
        if (err_msg) *err_msg = dup_wsa_last("getsockopt(SO_ERROR)");
        return -1;
    }
    if (soerr == 0) return 0;
    if (soerr == WSAEWOULDBLOCK || soerr == WSAEALREADY || soerr == WSAEINVAL)
        return -2;
    /* soerr is itself a WSA error code, so format it directly. */
    if (err_msg) *err_msg = dup_wsa_error("connect", soerr);
    return -1;
}

long long net_send(NetState *st, int sock_id, const char *data, long long len, char **err_msg) {
    if (!valid_id(st, sock_id)) { if (err_msg) *err_msg = dup_msg("invalid socket id"); return -1; }
    /* No MSG_NOSIGNAL on Windows: send() never raises SIGPIPE there, so the
     * POSIX-only flag is simply omitted. */
    int n = send(st->socks[sock_id], data, clamp_len(len), 0);
    if (n == SOCKET_ERROR) {
        if (WSAGetLastError() == WSAEWOULDBLOCK) return -2;
        if (err_msg) *err_msg = dup_wsa_last("send");
        return -1;
    }
    return (long long)n;
}

long long net_recv(NetState *st, int sock_id, char *buf, long long buf_len, char **err_msg) {
    if (!valid_id(st, sock_id)) { if (err_msg) *err_msg = dup_msg("invalid socket id"); return -1; }
    int n = recv(st->socks[sock_id], buf, clamp_len(buf_len), 0);
    if (n == SOCKET_ERROR) {
        if (WSAGetLastError() == WSAEWOULDBLOCK) return -2;
        if (err_msg) *err_msg = dup_wsa_last("recv");
        return -1;
    }
    return (long long)n; /* 0 == peer closed (EOF) */
}

long long net_sendto(NetState *st, int sock_id, const char *data, long long len,
                     const char *host, int port, char **err_msg) {
    if (!valid_id(st, sock_id)) { if (err_msg) *err_msg = dup_msg("invalid socket id"); return -1; }
    struct sockaddr_in sa;
    if (fill_addr(&sa, host, port, st->kinds[sock_id], err_msg) < 0) return -1;
    int n = sendto(st->socks[sock_id], data, clamp_len(len), 0,
                   (struct sockaddr *)&sa, sizeof(sa));
    if (n == SOCKET_ERROR) {
        if (WSAGetLastError() == WSAEWOULDBLOCK) return -2;
        if (err_msg) *err_msg = dup_wsa_last("sendto");
        return -1;
    }
    return (long long)n;
}

long long net_recvfrom(NetState *st, int sock_id, char *buf, long long buf_len,
                       char **from_addr_out, char **err_msg) {
    if (!valid_id(st, sock_id)) { if (err_msg) *err_msg = dup_msg("invalid socket id"); return -1; }
    struct sockaddr_in from;
    int flen = (int)sizeof(from);
    int n = recvfrom(st->socks[sock_id], buf, clamp_len(buf_len), 0,
                     (struct sockaddr *)&from, &flen);
    if (n == SOCKET_ERROR) {
        if (WSAGetLastError() == WSAEWOULDBLOCK) return -2;
        if (err_msg) *err_msg = dup_wsa_last("recvfrom");
        return -1;
    }
    if (from_addr_out) *from_addr_out = addr_to_str(&from);
    return (long long)n;
}

int net_raw_fd(NetState *st, int sock_id) {
    if (!valid_id(st, sock_id)) return -1;
    /* NOTE (Step3 hand-off): the public net_raw_fd() signature returns int, but
     * a Windows SOCKET is a UINT_PTR (64-bit on x64).  The cast below truncates
     * the handle to int.  It is currently safe *in practice* because Winsock
     * kernel handles are documented to fit in 32 bits (see "Socket Handles" in
     * the Winsock docs), and the interpreter only round-trips this value back
     * through net.c on the same platform where int is 32-bit.  However, this is
     * relied upon by event_loop.c's select() multiplexing, which Step3 will
     * port to Windows.  If Step3 needs to pass the raw SOCKET to a Windows
     * select()/fd_set directly (rather than re-looking it up by id), the
     * net_raw_fd() return type — and its callers in event_loop.c /
     * interpreter.c — should be widened to an intptr_t-sized type to avoid the
     * truncation.  See the Step3 hand-off note in docs/myon_spec.md (10.7). */
    return (int)st->socks[sock_id];
}

void net_sync_wait_fd(int fd, int for_write) {
    if (fd < 0) return;
    /* Reconstruct the SOCKET from the int fd (net_raw_fd truncated it; Winsock
     * socket handles are documented to fit in 32 bits — see the Step3 hand-off
     * note above and docs/myon_spec.md 10.7).  Zero-extend via (unsigned int)
     * so the value is not sign-extended into the 64-bit UINT_PTR SOCKET.
     *
     * Winsock select(): the fd_set is an array of SOCKETs and the first (nfds)
     * argument is ignored (kept only for BSD source compatibility), so pass 0. */
    SOCKET s = (SOCKET)(UINT_PTR)(unsigned int)fd;
    fd_set fds; FD_ZERO(&fds); FD_SET(s, &fds);
    if (for_write) select(0, NULL, &fds, NULL, NULL);
    else           select(0, &fds, NULL, NULL, NULL);
}

void net_close(NetState *st, int sock_id) {
    if (!valid_id(st, sock_id)) return;
    closesocket(st->socks[sock_id]);
    st->socks[sock_id] = INVALID_SOCKET;
}

#else /* !MYON_NET_POSIX && !_WIN32 : unsupported-platform stub */

struct NetState { int dummy; };

NetState *net_state_create(void) { return NULL; }
void      net_state_destroy(NetState *st) { (void)st; }
int       net_supported(void) { return 0; }

static char *unsupported(char **err_msg) {
    if (err_msg) {
        const char *m = "myon.net is only supported on Linux in this build";
        char *s = (char *)malloc(strlen(m) + 1);
        if (s) strcpy(s, m);
        if (err_msg) *err_msg = s;
    }
    return NULL;
}

int net_socket_create(NetState *st, int kind, char **err_msg) { (void)st; (void)kind; unsupported(err_msg); return -1; }
int net_bind(NetState *st, int sock_id, const char *host, int port, char **err_msg) { (void)st;(void)sock_id;(void)host;(void)port; unsupported(err_msg); return -1; }
int net_listen(NetState *st, int sock_id, int backlog, char **err_msg) { (void)st;(void)sock_id;(void)backlog; unsupported(err_msg); return -1; }
int net_local_port(NetState *st, int sock_id, char **err_msg) { (void)st;(void)sock_id; unsupported(err_msg); return -1; }
int net_try_accept(NetState *st, int listen_sock_id, char **peer_addr_out, char **err_msg) { (void)st;(void)listen_sock_id;(void)peer_addr_out; unsupported(err_msg); return -1; }
int net_connect(NetState *st, int sock_id, const char *host, int port, char **err_msg) { (void)st;(void)sock_id;(void)host;(void)port; unsupported(err_msg); return -1; }
int net_connect_check(NetState *st, int sock_id, char **err_msg) { (void)st;(void)sock_id; unsupported(err_msg); return -1; }
long long net_send(NetState *st, int sock_id, const char *data, long long len, char **err_msg) { (void)st;(void)sock_id;(void)data;(void)len; unsupported(err_msg); return -1; }
long long net_recv(NetState *st, int sock_id, char *buf, long long buf_len, char **err_msg) { (void)st;(void)sock_id;(void)buf;(void)buf_len; unsupported(err_msg); return -1; }
long long net_sendto(NetState *st, int sock_id, const char *data, long long len, const char *host, int port, char **err_msg) { (void)st;(void)sock_id;(void)data;(void)len;(void)host;(void)port; unsupported(err_msg); return -1; }
long long net_recvfrom(NetState *st, int sock_id, char *buf, long long buf_len, char **from_addr_out, char **err_msg) { (void)st;(void)sock_id;(void)buf;(void)buf_len;(void)from_addr_out; unsupported(err_msg); return -1; }
int net_raw_fd(NetState *st, int sock_id) { (void)st;(void)sock_id; return -1; }
void net_sync_wait_fd(int fd, int for_write) { (void)fd; (void)for_write; }
void net_close(NetState *st, int sock_id) { (void)st;(void)sock_id; }

#endif /* MYON_NET_POSIX / _WIN32 / stub */
