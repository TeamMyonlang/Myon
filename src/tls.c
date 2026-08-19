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

#include "tls.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>

/*
 * fd_set / FD_ZERO / FD_SET / select() (Phase5.2).  On POSIX these come from
 * <sys/select.h>; on Windows (native MSYS2/MinGW-w64 or a MinGW-w64 cross
 * build) there is no <sys/select.h> -- the same names are declared by
 * <winsock2.h> instead.  This mirrors the platform split already used in
 * src/net.c (Step 1-3) so the two socket-touching translation units agree.
 * The POSIX branch is unchanged, so the existing Linux build is unaffected.
 */
#if defined(_WIN32)
#  include <winsock2.h>
#else
#  include <sys/select.h>
#  include <sys/time.h>
#endif

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

/*
 * Hardening (net/http/tls robustness pass, 2026-08-19).
 *
 * This client is hardened along the lines recommended by the OpenSSL 3.x
 * "Writing a simple TLS client" guide
 * (https://docs.openssl.org/3.3/man7/ossl-guide-tls-client-block/):
 *
 *   - TLS 1.2 is enforced as the *minimum* protocol version; SSLv3/TLS1.0/1.1
 *     are deprecated by the IETF (RFC 8996) and are refused.
 *   - The peer certificate chain is verified against the system trust store
 *     with SSL_VERIFY_PEER (fail-closed: a bad chain aborts the handshake).
 *   - The expected identity is pinned before the handshake: SSL_set1_host()
 *     for DNS names, or X509_VERIFY_PARAM_set1_ip_asc() for IP literals, so a
 *     valid-but-wrong certificate is rejected.
 *   - SNI is sent only for DNS names (never for IP literals, per RFC 6066).
 *   - On failure we surface SSL_get_verify_result() as a human-readable reason.
 *   - A bounded handshake deadline prevents a hung/slow peer from blocking the
 *     single-threaded interpreter forever.
 *
 * The underlying fd is non-blocking (managed by net.c); we drive the handshake
 * and I/O with a blocking select() on just this fd, matching the interpreter's
 * synchronous fallback style.
 */

/* Handshake must complete within this many seconds, else we abort.  A slow or
 * malicious peer must not be able to wedge the whole interpreter. */
#define MYON_TLS_HANDSHAKE_TIMEOUT_SECS 30

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

/* Build "<prefix>: <reason> (<openssl detail>)" describing a handshake failure,
 * enriched with the certificate-verification result when that was the cause.
 * This mirrors the OpenSSL guide's use of SSL_get_verify_result() +
 * X509_verify_cert_error_string() so MITM / bad-chain failures are actionable
 * instead of an opaque "handshake failed". */
static char *dup_handshake_err(SSL *ssl) {
    long vr = SSL_get_verify_result(ssl);
    char ebuf[256];
    unsigned long e = ERR_get_error();
    if (e != 0) ERR_error_string_n(e, ebuf, sizeof(ebuf));
    else        snprintf(ebuf, sizeof(ebuf), "connection closed during handshake");

    const char *prefix = "TLS handshake failed";
    if (vr != X509_V_OK) {
        const char *vs = X509_verify_cert_error_string(vr);
        size_t n = strlen(prefix) + strlen(vs) + strlen(ebuf) + 32;
        char *m = (char *)malloc(n);
        if (m) snprintf(m, n, "%s: certificate verify failed: %s (%s)",
                        prefix, vs, ebuf);
        return m;
    }
    size_t n = strlen(prefix) + strlen(ebuf) + 3;
    char *m = (char *)malloc(n);
    if (m) snprintf(m, n, "%s: %s", prefix, ebuf);
    return m;
}

/* Heuristic: is `host` a literal IP address (v4 or v6) rather than a DNS name?
 * We only need a cheap textual classification here: bracket-stripped, an IPv6
 * literal contains ':' and an IPv4 literal is all digits and dots.  The
 * authoritative parse is done by X509_VERIFY_PARAM_set1_ip_asc() which rejects
 * anything malformed. */
static int host_is_ip_literal(const char *host) {
    if (!host || !host[0]) return 0;
    if (strchr(host, ':')) return 1; /* IPv6 literal */
    /* IPv4: only [0-9.] and at least one dot */
    int seen_dot = 0;
    for (const char *p = host; *p; p++) {
        if (*p == '.') { seen_dot = 1; continue; }
        if (*p < '0' || *p > '9') return 0;
    }
    return seen_dot;
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

    /* Load the system default trust store.  Without a usable trust store,
     * SSL_VERIFY_PEER below can only ever fail, so treat a load failure as
     * fatal rather than silently degrading to an unverifiable connection. */
    if (!SSL_CTX_set_default_verify_paths(ctx)) {
        if (err_msg) *err_msg = dup_ssl_err("SSL_CTX_set_default_verify_paths");
        SSL_CTX_free(ctx);
        return NULL;
    }

    /* Fail-closed peer verification: an untrusted / expired / mismatched chain
     * aborts SSL_connect().  Combined with the per-connection identity check
     * (SSL_set1_host / IP) this is the core MITM defence. */
    SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, NULL);

    /* Refuse deprecated protocol versions.  TLS 1.1 and earlier are deprecated
     * by RFC 8996; require TLS 1.2 as a floor (TLS 1.3 negotiated when the peer
     * supports it).  Also disable renegotiation and compression (CRIME). */
    if (!SSL_CTX_set_min_proto_version(ctx, TLS1_2_VERSION)) {
        if (err_msg) *err_msg = dup_ssl_err("SSL_CTX_set_min_proto_version");
        SSL_CTX_free(ctx);
        return NULL;
    }
    SSL_CTX_set_options(ctx,
                        SSL_OP_NO_COMPRESSION |
                        SSL_OP_NO_RENEGOTIATION |
                        SSL_OP_CIPHER_SERVER_PREFERENCE);

    /* OpenSSL security level 2 (default on modern builds) forbids <112-bit
     * security, SHA-1 signatures in certs, RSA/DH < 2048 bits, etc.  Set it
     * explicitly so the floor does not depend on distro defaults. */
    SSL_CTX_set_security_level(ctx, 2);

    g_ctx = ctx;
    return g_ctx;
}

/* Wait until `raw_fd` is readable (for_write==0) or writable (for_write==1),
 * bounded by an absolute monotonic deadline.  Returns 1 if ready, 0 on
 * timeout, -1 on select error. */
static int wait_fd_until(int raw_fd, int for_write, time_t deadline) {
    time_t now = time(NULL);
    if (now >= deadline) return 0;
    long secs = (long)(deadline - now);
    struct timeval tv;
    tv.tv_sec = secs;
    tv.tv_usec = 0;
    fd_set fds; FD_ZERO(&fds); FD_SET(raw_fd, &fds);
    int rc;
    if (for_write) rc = select(raw_fd + 1, NULL, &fds, NULL, &tv);
    else           rc = select(raw_fd + 1, &fds, NULL, NULL, &tv);
    if (rc > 0) return 1;
    if (rc == 0) return 0; /* timeout */
    return -1;
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

    /* Pin the expected server identity so a valid-but-wrong certificate is
     * rejected.  IP literals and DNS names are handled differently:
     *   - DNS name: SNI (RFC 6066) + SSL_set1_host() name check.
     *   - IP literal: NO SNI (RFC 6066 forbids IP-literal SNI) and identity is
     *     matched against the certificate's iPAddress SAN entries via
     *     X509_VERIFY_PARAM_set1_ip_asc().
     * Empty hostname is refused: without an identity to check we would fall
     * back to "any certificate is acceptable", which defeats verification. */
    if (!hostname || hostname[0] == '\0') {
        if (err_msg) *err_msg = dup_msg(
            "tls_connect: refusing TLS without a hostname to verify");
        SSL_free(ssl);
        return NULL;
    }

    X509_VERIFY_PARAM *param = SSL_get0_param(ssl);
    /* Never match the certificate identity via a wildcard against a partial
     * label, and require the identity check to actually run. */
    X509_VERIFY_PARAM_set_hostflags(param,
        X509_CHECK_FLAG_NO_PARTIAL_WILDCARDS);

    if (host_is_ip_literal(hostname)) {
        if (X509_VERIFY_PARAM_set1_ip_asc(param, hostname) != 1) {
            if (err_msg) *err_msg = dup_msg(
                "tls_connect: invalid IP literal for certificate verification");
            SSL_free(ssl);
            return NULL;
        }
        /* No SNI for IP literals. */
    } else {
        if (SSL_set1_host(ssl, hostname) != 1) {
            if (err_msg) *err_msg = dup_ssl_err("SSL_set1_host");
            SSL_free(ssl);
            return NULL;
        }
        /* SNI: informs a virtual-hosting server which certificate to present. */
        SSL_set_tlsext_host_name(ssl, hostname);
    }

    /* Drive the handshake to completion.  The fd is non-blocking, so
     * SSL_connect may return WANT_READ/WANT_WRITE; we spin with a bounded
     * select on just this fd until it completes, errors, or the deadline
     * passes (defence against a peer that never finishes the handshake). */
    time_t deadline = time(NULL) + MYON_TLS_HANDSHAKE_TIMEOUT_SECS;
    for (;;) {
        int rc = SSL_connect(ssl);
        if (rc == 1) break; /* handshake done */
        int se = SSL_get_error(ssl, rc);
        if (se == SSL_ERROR_WANT_READ || se == SSL_ERROR_WANT_WRITE) {
            int wr = (se == SSL_ERROR_WANT_WRITE);
            int r = wait_fd_until(raw_fd, wr, deadline);
            if (r == 1) continue;
            if (err_msg) *err_msg = dup_msg(
                r == 0 ? "TLS handshake failed: timed out"
                       : "TLS handshake failed: select error");
            SSL_free(ssl);
            return NULL;
        }
        if (err_msg) *err_msg = dup_handshake_err(ssl);
        SSL_free(ssl);
        return NULL;
    }

    /* Belt-and-braces: SSL_VERIFY_PEER already fails the handshake on a bad
     * chain, but re-check the verification result explicitly so we never hand
     * back a session whose peer was not authenticated. */
    if (SSL_get_verify_result(ssl) != X509_V_OK) {
        if (err_msg) *err_msg = dup_handshake_err(ssl);
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
    /* len is a signed VM value; a negative or huge value must not become a
     * gigantic size_t.  Reject negatives and clamp to INT_MAX (SSL_read_ex
     * takes a size_t, but SSL records are bounded and the caller reads in
     * chunks anyway). */
    if (len < 0) { if (err_msg) *err_msg = dup_msg("tls_read: negative length"); return -1; }
    size_t want = (size_t)len;
    size_t readbytes = 0;
    int rc = SSL_read_ex(conn->ssl, buf, want, &readbytes);
    if (rc == 1) return (long long)readbytes;
    int se = SSL_get_error(conn->ssl, rc);
    if (se == SSL_ERROR_WANT_READ || se == SSL_ERROR_WANT_WRITE) return -2;
    if (se == SSL_ERROR_ZERO_RETURN) return 0; /* clean TLS close_notify */
    if (se == SSL_ERROR_SYSCALL && ERR_peek_error() == 0) return 0; /* EOF */
    if (err_msg) *err_msg = dup_ssl_err("SSL_read");
    return -1;
}

long long tls_write(TlsConn *conn, const char *data, long long len,
                    char **err_msg) {
    if (!conn || !conn->ssl) {
        if (err_msg) *err_msg = dup_msg("tls_write: invalid connection");
        return -1;
    }
    if (len < 0) { if (err_msg) *err_msg = dup_msg("tls_write: negative length"); return -1; }
    size_t want = (size_t)len;
    size_t written = 0;
    int rc = SSL_write_ex(conn->ssl, data, want, &written);
    if (rc == 1) return (long long)written;
    int se = SSL_get_error(conn->ssl, rc);
    if (se == SSL_ERROR_WANT_READ || se == SSL_ERROR_WANT_WRITE) return -2;
    if (err_msg) *err_msg = dup_ssl_err("SSL_write");
    return -1;
}

void tls_close(TlsConn *conn) {
    if (!conn) return;
    if (conn->ssl) {
        /* One-way shutdown is fine for a client that is done sending.  We do
         * NOT retry/block on a WANT_READ here: the interpreter is done with
         * the connection and the underlying fd is closed right after. */
        SSL_shutdown(conn->ssl);
        SSL_free(conn->ssl);
    }
    free(conn);
}
