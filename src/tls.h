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

#ifndef MYON_TLS_H
#define MYON_TLS_H

/*
 * Phase5.1, Step6: a thin native TLS client wrapper over OpenSSL.
 *
 * This wraps an *already-connected* raw TCP socket fd (as produced by
 * myon.net.connect / net_raw_fd) in an OpenSSL TLS session, following the
 * usual OpenSSL client flow:
 *
 *     SSL_CTX_new -> SSL_new -> SSL_set_fd -> SSL_connect
 *     -> SSL_read / SSL_write -> SSL_shutdown -> SSL_free.
 *
 * The interpreter's myon.http.get / myon.http.post use this for https:// URLs.
 *
 * SECURITY NOTE: certificate verification is enabled against the system's
 * default trust store (SSL_CTX_set_default_verify_paths) with SNI-based
 * hostname checking, on a best-effort basis.  It is NOT a hardened TLS stack;
 * do not treat it as a substitute for a reviewed production TLS client.
 *
 * Return-code convention for tls_read / tls_write mirrors net_send/net_recv:
 *     >= 0  bytes transferred (0 from tls_read == clean EOF)
 *     -2    would block (retry after the socket becomes ready)
 *     -1    a real error (*err_msg set to a heap string, caller frees)
 */

typedef struct TlsConn TlsConn;

/* 1 if this build was compiled with OpenSSL TLS support, else 0. */
int tls_supported(void);

/* Perform a TLS handshake on an existing connected socket `raw_fd`.
 * `hostname` is used for SNI and certificate hostname verification.
 * On success returns a non-NULL TlsConn*.  On failure returns NULL and, if
 * `err_msg` is non-NULL, stores a heap-allocated error string in *err_msg
 * (caller frees). */
TlsConn *tls_connect(int raw_fd, const char *hostname, char **err_msg);

/* Read up to `len` bytes into `buf`.  See the return-code convention above. */
long long tls_read(TlsConn *conn, char *buf, long long len, char **err_msg);

/* Write up to `len` bytes from `data`.  See the return-code convention above. */
long long tls_write(TlsConn *conn, const char *data, long long len,
                    char **err_msg);

/* Cleanly shut down and free the TLS session (does NOT close raw_fd). */
void tls_close(TlsConn *conn);

#endif /* MYON_TLS_H */
