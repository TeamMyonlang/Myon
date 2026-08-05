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

#include "tls.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <sys/select.h>   /* fd_set, FD_ZERO, FD_SET, select() (Phase5.2) */

#include <openssl/ssl.h>
#include <openssl/err.h>

/* One shared client context for the whole process, created lazily. */
struct TlsConn {
    SSL *ssl;
    int fd;
};

static SSL_CTX *g_ctx = NULL;

int tls_supported(void) { return 1; }

static char *dup_msg(const char *s) {
    char *m = (char *)malloc(strlen(s) + 1);
    if (m) strcpy(m, s);
    return m;
}

/* Build a heap error string "<prefix>: <top OpenSSL error>" (caller frees). */
static char *dup_ssl_err(const char *prefix) {
    unsigned long e = ERR_get_error();
    char ebuf[256];
    if (e != 0) {
        ERR_error_string_n(e, ebuf, sizeof(ebuf));
    } else {
        snprintf(ebuf, sizeof(ebuf), "unknown TLS error");
    }
    size_t n = strlen(prefix) + strlen(ebuf) + 3;
    char *m = (char *)malloc(n);
    if (m) snprintf(m, n, "%s: %s", prefix, ebuf);
    return m;
}

static SSL_CTX *ensure_ctx(char **err_msg) {
    if (g_ctx) return g_ctx;
    /* OpenSSL 1.1.0+ initialises itself on first use; TLS_client_method()
     * negotiates the best mutually-supported protocol version. */
    const SSL_METHOD *method = TLS_client_method();
    SSL_CTX *ctx = SSL_CTX_new(method);
    if (!ctx) {
        if (err_msg) *err_msg = dup_ssl_err("SSL_CTX_new");
        return NULL;
    }
    /* Best-effort verification against the system default trust store.
     * We set VERIFY_PEER so SSL_connect fails on an untrusted chain; combined
     * with SSL_set1_host() below this gives basic MITM protection.  This is
     * still not a hardened TLS client — see the security note in tls.h. */
    SSL_CTX_set_default_verify_paths(ctx);
    SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, NULL);
    g_ctx = ctx;
    return g_ctx;
}

TlsConn *tls_connect(int raw_fd, const char *hostname, char **err_msg) {
    if (raw_fd < 0) {
        if (err_msg) *err_msg = dup_msg("tls_connect: invalid socket fd");
        return NULL;
    }
    SSL_CTX *ctx = ensure_ctx(err_msg);
    if (!ctx) return NULL;

    SSL *ssl = SSL_new(ctx);
    if (!ssl) { if (err_msg) *err_msg = dup_ssl_err("SSL_new"); return NULL; }

    if (SSL_set_fd(ssl, raw_fd) != 1) {
        if (err_msg) *err_msg = dup_ssl_err("SSL_set_fd");
        SSL_free(ssl);
        return NULL;
    }

    /* SNI + hostname verification (skip both for a literal IP or empty host). */
    if (hostname && hostname[0] != '\0') {
        SSL_set_tlsext_host_name(ssl, hostname);
        /* Enforce that the presented certificate matches the hostname. */
        SSL_set1_host(ssl, hostname);
    }

    /* Drive the handshake to completion.  The fd is non-blocking, so
     * SSL_connect may return WANT_READ/WANT_WRITE; we spin with a blocking
     * select on just this fd (the interpreter's synchronous fallback style)
     * until it completes or errors. */
    for (;;) {
        int rc = SSL_connect(ssl);
        if (rc == 1) break; /* handshake done */
        int se = SSL_get_error(ssl, rc);
        if (se == SSL_ERROR_WANT_READ || se == SSL_ERROR_WANT_WRITE) {
            fd_set fds; FD_ZERO(&fds); FD_SET(raw_fd, &fds);
            if (se == SSL_ERROR_WANT_WRITE)
                select(raw_fd + 1, NULL, &fds, NULL, NULL);
            else
                select(raw_fd + 1, &fds, NULL, NULL, NULL);
            continue;
        }
        if (err_msg) *err_msg = dup_ssl_err("SSL_connect (TLS handshake failed)");
        SSL_free(ssl);
        return NULL;
    }

    TlsConn *conn = (TlsConn *)malloc(sizeof(TlsConn));
    if (!conn) {
        if (err_msg) *err_msg = dup_msg("tls_connect: out of memory");
        SSL_shutdown(ssl);
        SSL_free(ssl);
        return NULL;
    }
    conn->ssl = ssl;
    conn->fd = raw_fd;
    return conn;
}

long long tls_read(TlsConn *conn, char *buf, long long len, char **err_msg) {
    if (!conn || !conn->ssl) {
        if (err_msg) *err_msg = dup_msg("tls_read: invalid connection");
        return -1;
    }
    int n = SSL_read(conn->ssl, buf, (int)len);
    if (n > 0) return (long long)n;
    int se = SSL_get_error(conn->ssl, n);
    if (se == SSL_ERROR_WANT_READ || se == SSL_ERROR_WANT_WRITE) return -2;
    if (se == SSL_ERROR_ZERO_RETURN) return 0; /* clean TLS close */
    if (se == SSL_ERROR_SYSCALL && n == 0)   return 0; /* EOF at transport */
    if (err_msg) *err_msg = dup_ssl_err("SSL_read");
    return -1;
}

long long tls_write(TlsConn *conn, const char *data, long long len,
                    char **err_msg) {
    if (!conn || !conn->ssl) {
        if (err_msg) *err_msg = dup_msg("tls_write: invalid connection");
        return -1;
    }
    int n = SSL_write(conn->ssl, data, (int)len);
    if (n > 0) return (long long)n;
    int se = SSL_get_error(conn->ssl, n);
    if (se == SSL_ERROR_WANT_READ || se == SSL_ERROR_WANT_WRITE) return -2;
    if (err_msg) *err_msg = dup_ssl_err("SSL_write");
    return -1;
}

void tls_close(TlsConn *conn) {
    if (!conn) return;
    if (conn->ssl) {
        /* One-way shutdown is fine for a client that is done sending. */
        SSL_shutdown(conn->ssl);
        SSL_free(conn->ssl);
    }
    free(conn);
}
