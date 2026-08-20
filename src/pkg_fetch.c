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

#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#  define _POSIX_C_SOURCE 200809L
#endif

#include "platform.h"
#include "pkg_fetch.h"
#include "net.h"
#include "tls.h"
#include "common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>

#if defined(MYON_OS_POSIX)
#  include <sys/select.h>
#  include <unistd.h>
#endif

/* ================================================================== */
/* small buffer                                                        */
/* ================================================================== */

typedef struct { unsigned char *data; size_t len, cap; } Buf;
static void buf_init(Buf *b) { b->data = NULL; b->len = 0; b->cap = 0; }
static bool buf_append(Buf *b, const unsigned char *p, size_t n, size_t hard_cap) {
    size_t need;
    if (__builtin_add_overflow(b->len, n, &need)) return false;
    if (need > hard_cap) return false;
    if (need > b->cap) {
        size_t nc = b->cap ? b->cap : 4096;
        while (nc < need) { if (__builtin_mul_overflow(nc, 2, &nc)) return false; }
        b->data = myon_xrealloc(b->data, nc);
        b->cap = nc;
    }
    if (n) memcpy(b->data + b->len, p, n);
    b->len += n;
    return true;
}
static void buf_free(Buf *b) { free(b->data); b->data = NULL; b->len = b->cap = 0; }

static void fset(char **err_msg, const char *msg) { if (err_msg) *err_msg = myon_strdup(msg); }

/* ================================================================== */
/* URL parsing                                                         */
/* ================================================================== */

bool pkg_fetch_parse_url(const char *url, char **host, int *port, char **path,
                         char **err_msg) {
    *host = NULL; *path = NULL; *port = 443;
    if (!url) { fset(err_msg, "fetch: empty URL"); return false; }
    for (const char *p = url; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c < 0x20 || c == 0x7f) { fset(err_msg, "fetch: control byte in URL"); return false; }
    }
    if (strncmp(url, "https://", 8) != 0) {
        fset(err_msg, "fetch: only https:// URLs are allowed");
        return false;
    }
    const char *p = url + 8;
    const char *slash = strchr(p, '/');
    size_t hostlen = slash ? (size_t)(slash - p) : strlen(p);
    if (memchr(p, '@', hostlen)) { fset(err_msg, "fetch: URL must not embed credentials"); return false; }
    if (hostlen == 0) { fset(err_msg, "fetch: empty host"); return false; }

    /* optional :port */
    const char *colon = memchr(p, ':', hostlen);
    if (colon) {
        size_t hl = (size_t)(colon - p);
        long pv = strtol(colon + 1, NULL, 10);
        if (pv <= 0 || pv > 65535) { fset(err_msg, "fetch: bad port"); return false; }
        *port = (int)pv;
        *host = myon_strndup(p, hl);
    } else {
        *host = myon_strndup(p, hostlen);
    }
    *path = slash ? myon_strdup(slash) : myon_strdup("/");
    return true;
}

/* ================================================================== */
/* default transport: net.c + tls.c, blocking via select()             */
/* ================================================================== */

typedef struct {
    NetState *ns;
    int       sock;
    TlsConn  *tls;
} DefConn;

#if defined(MYON_OS_POSIX)
static bool wait_fd(myon_fd_t fd, bool for_write, int secs) {
    fd_set set;
    FD_ZERO(&set);
    FD_SET((int)fd, &set);
    struct timeval tv; tv.tv_sec = secs; tv.tv_usec = 0;
    int rc = select((int)fd + 1, for_write ? NULL : &set, for_write ? &set : NULL, NULL, &tv);
    return rc > 0;
}
#endif

static void *def_connect(const char *host, int port, char **err_msg, void *ctx) {
    (void)ctx;
    if (!net_supported()) { fset(err_msg, "fetch: sockets not supported on this build"); return NULL; }
    if (!tls_supported())  { fset(err_msg, "fetch: TLS not supported on this build"); return NULL; }

    DefConn *c = myon_xmalloc(sizeof(DefConn));
    c->ns = net_state_create();
    c->tls = NULL;
    c->sock = net_socket_create(c->ns, 0, err_msg);
    if (c->sock < 0) { net_state_destroy(c->ns); free(c); return NULL; }

#if defined(MYON_OS_POSIX)
    int rc = net_connect(c->ns, c->sock, host, port, err_msg);
    if (rc == -2) {
        myon_fd_t fd = net_raw_fd(c->ns, c->sock);
        if (!wait_fd(fd, true, PKG_FETCH_TIMEOUT_SECS)) { fset(err_msg, "fetch: connect timed out"); goto fail; }
        if (net_connect_check(c->ns, c->sock, err_msg) != 0) goto fail;
    } else if (rc != 0) {
        goto fail;
    }
    /* TLS handshake (verifies cert + hostname, fail-closed). */
    char *terr = NULL;
    c->tls = tls_connect((int)net_raw_fd(c->ns, c->sock), host, &terr);
    if (!c->tls) { if (err_msg) *err_msg = terr ? terr : myon_strdup("fetch: TLS handshake failed"); goto fail2; }
    return c;
fail:
    net_close(c->ns, c->sock);
    net_state_destroy(c->ns);
    free(c);
    return NULL;
fail2:
    net_close(c->ns, c->sock);
    net_state_destroy(c->ns);
    free(c);
    return NULL;
#else
    (void)host; (void)port;
    fset(err_msg, "fetch: network fetch is only implemented on POSIX in this build");
    net_close(c->ns, c->sock);
    net_state_destroy(c->ns);
    free(c);
    return NULL;
#endif
}

static bool def_write(void *handle, const unsigned char *data, size_t len, char **err_msg) {
    DefConn *c = handle;
    size_t off = 0;
    while (off < len) {
        char *werr = NULL;
        long long n = tls_write(c->tls, (const char *)data + off, (long long)(len - off), &werr);
        if (n == -2) {
#if defined(MYON_OS_POSIX)
            wait_fd(net_raw_fd(c->ns, c->sock), true, PKG_FETCH_TIMEOUT_SECS);
#endif
            continue;
        }
        if (n < 0) { if (err_msg) *err_msg = werr ? werr : myon_strdup("fetch: TLS write error"); return false; }
        off += (size_t)n;
    }
    return true;
}

static long def_read(void *handle, unsigned char *buf, size_t cap, char **err_msg) {
    DefConn *c = handle;
    for (;;) {
        char *rerr = NULL;
        long long n = tls_read(c->tls, (char *)buf, (long long)cap, &rerr);
        if (n == -2) {
#if defined(MYON_OS_POSIX)
            if (!wait_fd(net_raw_fd(c->ns, c->sock), false, PKG_FETCH_TIMEOUT_SECS)) { fset(err_msg, "fetch: read timed out"); return -1; }
#endif
            continue;
        }
        if (n < 0) { if (err_msg) *err_msg = rerr ? rerr : myon_strdup("fetch: TLS read error"); return -1; }
        return (long)n;
    }
}

static void def_close(void *handle) {
    DefConn *c = handle;
    if (!c) return;
    if (c->tls) tls_close(c->tls);
    net_close(c->ns, c->sock);
    net_state_destroy(c->ns);
    free(c);
}

const PkgTransport *pkg_transport_default(void) {
    static const PkgTransport tr = {
        def_connect, def_write, def_read, def_close, NULL
    };
    return &tr;
}

/* ================================================================== */
/* HTTP framing                                                        */
/* ================================================================== */

/* Case-insensitive header prefix test on a single header line. */
static bool hdr_is(const char *line, const char *name) {
    size_t n = strlen(name);
    for (size_t i = 0; i < n; i++)
        if (tolower((unsigned char)line[i]) != tolower((unsigned char)name[i])) return false;
    return line[n] == ':';
}
static const char *hdr_value(const char *line) {
    const char *c = strchr(line, ':');
    if (!c) return "";
    c++;
    while (*c == ' ' || *c == '\t') c++;
    return c;
}

/* Reject CR/LF/NUL/control bytes in a redirect target (spec §7). */
static bool location_is_clean(const char *v) {
    for (const char *p = v; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c < 0x20 || c == 0x7f) return false;
    }
    return true;
}

/* Allow-listed redirect hosts (GitHub archive/API surface, spec §7). */
static bool host_allowed(const char *host) {
    static const char *ok[] = {
        "github.com", "codeload.github.com", "api.github.com",
        "objects.githubusercontent.com", "raw.githubusercontent.com", NULL
    };
    for (int i = 0; ok[i]; i++) if (strcmp(host, ok[i]) == 0) return true;
    return false;
}

/*
 * One HTTP transaction (no redirect handling): connect, send GET, read status
 * + headers + body.  Fills *status; on 3xx sets *location (heap) if present.
 * Body (only meaningful for 200) is appended to *body.  `accept` sets the
 * Accept header (NULL -> a wildcard accept).  Returns false + *err_msg on a
 * transport or framing error.
 */
static bool http_once(const PkgTransport *tr, const char *host, int port,
                      const char *path, const char *accept,
                      int *status, char **location, Buf *body, char **err_msg) {
    *status = 0; *location = NULL;
    void *h = tr->connect(host, port, err_msg, tr->ctx);
    if (!h) return false;

    /* Build a minimal, fixed request. */
    char req[2048];
    int rn = snprintf(req, sizeof(req),
        "GET %s HTTP/1.1\r\n"
        "Host: %s\r\n"
        "User-Agent: myon-pkg/1\r\n"
        "Accept: %s\r\n"
        "Connection: close\r\n"
        "\r\n",
        path, host, accept ? accept : "*/*");
    if (rn <= 0 || rn >= (int)sizeof(req)) { tr->close(h); fset(err_msg, "fetch: request too long"); return false; }

    if (!tr->write(h, (const unsigned char *)req, (size_t)rn, err_msg)) { tr->close(h); return false; }

    /* Read the whole response into a raw buffer (bounded). */
    Buf raw; buf_init(&raw);
    unsigned char chunk[16384];
    for (;;) {
        long n = tr->read(h, chunk, sizeof(chunk), err_msg);
        if (n < 0) { buf_free(&raw); tr->close(h); return false; }
        if (n == 0) break; /* EOF (Connection: close) */
        if (!buf_append(&raw, chunk, (size_t)n, PKG_FETCH_MAX_BODY + (1u << 20))) {
            buf_free(&raw); tr->close(h); fset(err_msg, "fetch: response exceeds size limit");
            return false;
        }
    }
    tr->close(h);

    /* Locate end of headers. */
    size_t hdr_end = 0;
    for (size_t i = 0; i + 3 < raw.len; i++)
        if (raw.data[i]=='\r'&&raw.data[i+1]=='\n'&&raw.data[i+2]=='\r'&&raw.data[i+3]=='\n') { hdr_end = i + 4; break; }
    if (!hdr_end) { buf_free(&raw); fset(err_msg, "fetch: malformed response (no header terminator)"); return false; }

    /* Copy header block to a NUL-terminated scratch string for line parsing. */
    char *hs = myon_xmalloc(hdr_end + 1);
    memcpy(hs, raw.data, hdr_end); hs[hdr_end] = '\0';

    /* Status line. */
    if (strncmp(hs, "HTTP/1.", 7) != 0) { free(hs); buf_free(&raw); fset(err_msg, "fetch: not an HTTP/1.x response"); return false; }
    const char *sp = strchr(hs, ' ');
    if (!sp) { free(hs); buf_free(&raw); fset(err_msg, "fetch: malformed status line"); return false; }
    *status = (int)strtol(sp + 1, NULL, 10);

    /* Header scan. */
    long long content_length = -1;
    bool chunked = false;
    char *line = strtok(hs, "\r\n"); /* first token = status line */
    while ((line = strtok(NULL, "\r\n")) != NULL) {
        if (hdr_is(line, "Location")) {
            const char *v = hdr_value(line);
            if (!location_is_clean(v)) { free(hs); buf_free(&raw); fset(err_msg, "fetch: redirect Location has illegal characters"); return false; }
            free(*location); *location = myon_strdup(v);
        } else if (hdr_is(line, "Content-Length")) {
            content_length = strtoll(hdr_value(line), NULL, 10);
        } else if (hdr_is(line, "Transfer-Encoding")) {
            if (strstr(hdr_value(line), "chunked")) chunked = true;
        }
    }
    free(hs);

    if (content_length > (long long)PKG_FETCH_MAX_BODY) {
        buf_free(&raw); fset(err_msg, "fetch: Content-Length exceeds limit"); return false;
    }

    /* Only decode the body when the caller will use it (200). */
    const unsigned char *bp = raw.data + hdr_end;
    size_t blen = raw.len - hdr_end;

    if (chunked) {
        /* Decode chunked transfer-encoding. */
        size_t i = 0;
        for (;;) {
            /* read chunk-size line (hex) */
            size_t j = i;
            while (j + 1 < blen && !(bp[j] == '\r' && bp[j+1] == '\n')) j++;
            if (j + 1 >= blen) { buf_free(&raw); fset(err_msg, "fetch: malformed chunk header"); return false; }
            char sizebuf[32]; size_t sl = j - i;
            if (sl == 0 || sl >= sizeof(sizebuf)) { buf_free(&raw); fset(err_msg, "fetch: malformed chunk size"); return false; }
            memcpy(sizebuf, bp + i, sl); sizebuf[sl] = '\0';
            /* strip any chunk extension after ';' */
            char *semi = strchr(sizebuf, ';'); if (semi) *semi = '\0';
            char *endp = NULL;
            unsigned long csz = strtoul(sizebuf, &endp, 16);
            if (endp == sizebuf) { buf_free(&raw); fset(err_msg, "fetch: bad chunk size"); return false; }
            i = j + 2; /* past CRLF */
            if (csz == 0) break; /* last chunk */
            if (i + csz > blen) { buf_free(&raw); fset(err_msg, "fetch: truncated chunk body"); return false; }
            if (!buf_append(body, bp + i, csz, PKG_FETCH_MAX_BODY)) { buf_free(&raw); fset(err_msg, "fetch: body exceeds limit"); return false; }
            i += csz;
            if (i + 2 > blen || bp[i] != '\r' || bp[i+1] != '\n') { buf_free(&raw); fset(err_msg, "fetch: malformed chunk terminator"); return false; }
            i += 2;
        }
    } else {
        if (content_length >= 0 && (size_t)content_length > blen) {
            buf_free(&raw); fset(err_msg, "fetch: truncated response body"); return false;
        }
        size_t take = (content_length >= 0) ? (size_t)content_length : blen;
        if (!buf_append(body, bp, take, PKG_FETCH_MAX_BODY)) { buf_free(&raw); fset(err_msg, "fetch: body exceeds limit"); return false; }
    }

    buf_free(&raw);
    return true;
}

bool pkg_fetch_https_get(const PkgTransport *tr, const char *url,
                         unsigned char **out_data, size_t *out_len,
                         char **err_msg) {
    *out_data = NULL; *out_len = 0;
    char *cur = myon_strdup(url);

    for (int hop = 0; hop <= PKG_FETCH_MAX_REDIRECTS; hop++) {
        char *host = NULL, *path = NULL; int port = 443;
        if (!pkg_fetch_parse_url(cur, &host, &port, &path, err_msg)) { free(cur); return false; }
        if (!host_allowed(host)) {
            fset(err_msg, "fetch: refusing redirect to a non-GitHub host");
            free(host); free(path); free(cur); return false;
        }

        int status = 0; char *loc = NULL;
        Buf body; buf_init(&body);
        bool ok = http_once(tr, host, port, path, "application/zip, */*", &status, &loc, &body, err_msg);
        free(host); free(path);
        if (!ok) { buf_free(&body); free(loc); free(cur); return false; }

        if (status == 200) {
            *out_data = body.data; *out_len = body.len; /* transfer ownership */
            free(loc); free(cur);
            return true;
        }
        buf_free(&body);

        if (status == 301 || status == 302 || status == 303 ||
            status == 307 || status == 308) {
            if (!loc) { fset(err_msg, "fetch: redirect without a Location header"); free(cur); return false; }
            /* An https->http downgrade (or any non-https target) is refused. */
            if (strncmp(loc, "https://", 8) != 0) {
                fset(err_msg, "fetch: refusing redirect that is not https://");
                free(loc); free(cur); return false;
            }
            free(cur); cur = loc; /* follow */
            continue;
        }

        /* Any other status is a hard error. */
        char msg[96];
        snprintf(msg, sizeof(msg), "fetch: server returned HTTP %d", status);
        fset(err_msg, msg);
        free(loc); free(cur);
        return false;
    }
    fset(err_msg, "fetch: too many redirects");
    free(cur);
    return false;
}

/* ================================================================== */
/* GitHub ref resolution                                               */
/* ================================================================== */

bool pkg_fetch_resolve_ref(const PkgTransport *tr, const char *owner,
                           const char *repo, const char *ref,
                           char *out_sha, char **err_msg) {
    const char *r = (ref && *ref) ? ref : "HEAD";
    /* GET https://api.github.com/repos/<o>/<r>/commits/<ref>  Accept: sha */
    char path[1024];
    int pn = snprintf(path, sizeof(path), "/repos/%s/%s/commits/%s", owner, repo, r);
    if (pn <= 0 || pn >= (int)sizeof(path)) { fset(err_msg, "resolve: ref path too long"); return false; }

    int status = 0; char *loc = NULL;
    Buf body; buf_init(&body);
    bool ok = http_once(tr, "api.github.com", 443, path,
                        "application/vnd.github.sha", &status, &loc, &body, err_msg);
    free(loc);
    if (!ok) { buf_free(&body); return false; }
    if (status != 200) {
        buf_free(&body);
        char msg[160];
        if (status == 404) snprintf(msg, sizeof(msg), "resolve: repository or ref '%s' not found (or repo is private)", r);
        else if (status == 403) snprintf(msg, sizeof(msg), "resolve: GitHub API rate limit or forbidden (HTTP 403)");
        else snprintf(msg, sizeof(msg), "resolve: GitHub API returned HTTP %d", status);
        fset(err_msg, msg);
        return false;
    }
    /* Body should be exactly a 40-hex SHA (possibly with trailing whitespace). */
    size_t n = body.len;
    while (n > 0 && (body.data[n-1] == '\n' || body.data[n-1] == '\r' || body.data[n-1] == ' ')) n--;
    if (n != 40) { buf_free(&body); fset(err_msg, "resolve: unexpected GitHub API response (not a commit SHA)"); return false; }
    for (size_t i = 0; i < 40; i++) {
        unsigned char c = body.data[i];
        if (!((c>='0'&&c<='9')||(c>='a'&&c<='f'))) { buf_free(&body); fset(err_msg, "resolve: API returned a non-hex SHA"); return false; }
        out_sha[i] = (char)c;
    }
    out_sha[40] = '\0';
    buf_free(&body);
    return true;
}
