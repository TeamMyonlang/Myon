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

#ifndef MYON_NET_H
#define MYON_NET_H

/*
 * Phase5, Step4: native IPv4 TCP/UDP sockets (myon.net).
 *
 * This module wraps the POSIX socket API directly (no dlopen/FFI) because the
 * calls live in libc.  Sockets are always created non-blocking; the
 * interpreter side decides whether to yield to the event loop (inside a
 * coroutine) or block synchronously on a single fd (outside one).  net.c is
 * intentionally unaware of the event loop so module dependencies stay
 * one-directional (interpreter.c -> net.c, interpreter.c -> event_loop.c).
 *
 * Platform policy mirrors ffi_platform.c: Linux is the primary target; other
 * platforms compile as an unsupported stub that reports an error.
 *
 * Return-code convention for the would-block-capable calls
 * (net_try_accept / net_connect / net_connect_check / net_send / net_recv /
 *  net_sendto / net_recvfrom):
 *     >= 0  success (bytes, or a new socket id for accept)
 *     -2    EWOULDBLOCK/EAGAIN/EINPROGRESS — not an error, retry later
 *     -1    a real error (*err_msg set, caller frees)
 */

typedef struct NetState NetState;

NetState *net_state_create(void);
void      net_state_destroy(NetState *st);

/* 1 if this build has real socket support (Linux), else 0. */
int net_supported(void);

/* kind: 0 = TCP (SOCK_STREAM), 1 = UDP (SOCK_DGRAM). */
int net_socket_create(NetState *st, int kind, char **err_msg);

int net_bind(NetState *st, int sock_id, const char *host, int port,
             char **err_msg);
int net_listen(NetState *st, int sock_id, int backlog, char **err_msg);

/* The local port a socket is bound to (useful with bind(port=0)). */
int net_local_port(NetState *st, int sock_id, char **err_msg);

/* Non-blocking accept.  peer_addr_out (if non-NULL) receives a malloc'd
 * "host:port" string on success (caller frees). */
int net_try_accept(NetState *st, int listen_sock_id, char **peer_addr_out,
                   char **err_msg);

/* Non-blocking connect (returns -2 while in progress) + completion check. */
int net_connect(NetState *st, int sock_id, const char *host, int port,
                char **err_msg);
int net_connect_check(NetState *st, int sock_id, char **err_msg);

/* TCP send/recv (non-blocking). */
long long net_send(NetState *st, int sock_id, const char *data, long long len,
                   char **err_msg);
long long net_recv(NetState *st, int sock_id, char *buf, long long buf_len,
                   char **err_msg);

/* UDP sendto/recvfrom (non-blocking).  from_addr_out receives a malloc'd
 * "host:port" string (caller frees). */
long long net_sendto(NetState *st, int sock_id, const char *data,
                     long long len, const char *host, int port,
                     char **err_msg);
long long net_recvfrom(NetState *st, int sock_id, char *buf, long long buf_len,
                       char **from_addr_out, char **err_msg);

/* Raw fd (for the interpreter to register with the event loop's select). */
int net_raw_fd(NetState *st, int sock_id);

/*
 * Synchronous single-fd wait (Windows helper, Phase5 Step3).
 *
 * Blocks until `fd` (a value previously returned by net_raw_fd) is readable
 * (for_write == 0) or writable (for_write != 0).  This exists so the
 * interpreter's synchronous (non-coroutine) net fallback does not have to
 * include the Winsock headers itself: on Windows <winsock2.h> drags in
 * <windows.h>/<winnt.h>, whose `TokenType` enum collides with Myon's own
 * TokenType.  On Linux the interpreter still uses select(2) inline; this
 * helper's Linux/stub definitions are provided only for symmetry.
 */
void net_sync_wait_fd(int fd, int for_write);

void net_close(NetState *st, int sock_id);

#endif /* MYON_NET_H */
