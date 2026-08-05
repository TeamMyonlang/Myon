/*
 * Copyright 2026 nyan<(nyan4)
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

#include "net.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#if defined(__linux__)
#  define MYON_NET_POSIX 1
#endif

#ifdef MYON_NET_POSIX

#include <sys/socket.h>
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
    ssize_t n = send(st->fds[sock_id], data, (size_t)len, MSG_NOSIGNAL);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return -2;
        if (err_msg) *err_msg = dup_errno("send");
        return -1;
    }
    return (long long)n;
}

long long net_recv(NetState *st, int sock_id, char *buf, long long buf_len, char **err_msg) {
    if (!valid_id(st, sock_id)) { if (err_msg) *err_msg = dup_msg("invalid socket id"); return -1; }
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

void net_close(NetState *st, int sock_id) {
    if (!valid_id(st, sock_id)) return;
    close(st->fds[sock_id]);
    st->fds[sock_id] = -1;
}

#else /* !MYON_NET_POSIX : unsupported-platform stub */

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
void net_close(NetState *st, int sock_id) { (void)st;(void)sock_id; }

#endif /* MYON_NET_POSIX */
