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

/*
 * Core HTTPS GET with safe redirect following.  `restrict_github` selects the
 * host policy: when true (the default archive/API path) only the GitHub
 * allow-list is permitted; when false (package registry lists, whose URLs point
 * at arbitrary third-party hosts) any host is allowed but every other safety
 * check — HTTPS-only, no https->http downgrade, no embedded credentials, size
 * caps, redirect cap — still applies.
 */
static bool https_get_core(const PkgTransport *tr, const char *url,
                           bool restrict_github, const char *accept,
                           unsigned char **out_data, size_t *out_len,
                           char **err_msg) {
    *out_data = NULL; *out_len = 0;
    char *cur = myon_strdup(url);

    for (int hop = 0; hop <= PKG_FETCH_MAX_REDIRECTS; hop++) {
        char *host = NULL, *path = NULL; int port = 443;
        if (!pkg_fetch_parse_url(cur, &host, &port, &path, err_msg)) { free(cur); return false; }
        if (restrict_github && !host_allowed(host)) {
            fset(err_msg, "fetch: refusing redirect to a non-GitHub host");
            free(host); free(path); free(cur); return false;
        }

        int status = 0; char *loc = NULL;
        Buf body; buf_init(&body);
        bool ok = http_once(tr, host, port, path, accept, &status, &loc, &body, err_msg);
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

bool pkg_fetch_https_get(const PkgTransport *tr, const char *url,
                         unsigned char **out_data, size_t *out_len,
                         char **err_msg) {
    return https_get_core(tr, url, /*restrict_github=*/true,
                          "application/zip, */*", out_data, out_len, err_msg);
}

bool pkg_fetch_https_get_any(const PkgTransport *tr, const char *url,
                             unsigned char **out_data, size_t *out_len,
                             char **err_msg) {
    return https_get_core(tr, url, /*restrict_github=*/false,
                          "application/json, text/plain, */*",
                          out_data, out_len, err_msg);
}

/* ================================================================== */
/* GitHub ref resolution                                               */
/* ================================================================== */
/*
 * Two strategies, tried in order:
 *
 *   1. git "smart HTTP" ref discovery on github.com (the transport used by
 *      `git clone`):
 *
 *         GET https://github.com/<owner>/<repo>.git/info/refs?service=git-upload-pack
 *
 *      This endpoint is served from the same github.com host we already fetch
 *      from and is NOT subject to the very low unauthenticated api.github.com
 *      REST rate limit (60 requests/hour, tightened further in 2025-05 per the
 *      GitHub changelog "Updated rate limits for unauthenticated requests").
 *      It returns every ref -> commit SHA in one response, so branch/tag/HEAD
 *      resolution needs no REST call at all.  Verified 2026-08-20 against the
 *      live service.
 *
 *   2. the classic api.github.com REST fallback (kept for robustness, e.g. if
 *      the git protocol response is ever unexpected):
 *
 *         GET https://api.github.com/repos/<owner>/<repo>/commits/<ref>
 *         Accept: application/vnd.github.sha
 *
 * A full 40-hex SHA passed as the ref short-circuits both: it is already
 * immutable, so it is validated locally and returned without any network call.
 */

static bool is_hex40(const unsigned char *p) {
    for (int i = 0; i < 40; i++) {
        unsigned char c = p[i];
        if (!((c>='0'&&c<='9')||(c>='a'&&c<='f'))) return false;
    }
    return true;
}

/*
 * A single pkt-line advertisement entry: "<40-hex-sha> <refname>".  We keep the
 * decoded refs in a small growable array while scanning, then pick the one that
 * matches the requested ref.
 */
typedef struct { char sha[41]; char *name; } AdvRef;

/*
 * Decode the git smart-HTTP ref-advertisement pkt-line stream (protocol v0/v1)
 * into (sha, refname) pairs.  Also captures the symref target of HEAD from the
 * capability list ("symref=HEAD:refs/heads/<default>") when present, so we can
 * resolve the default branch deterministically.
 *
 * Format (see git-scm.com/docs/http-protocol):
 *   - each pkt-line is a 4-hex length prefix (covering the 4 bytes too) + data,
 *   - "0000" is a flush packet,
 *   - the first data line is "# service=git-upload-pack\n" (then a flush),
 *   - the first ref line is "<sha> HEAD\0<capabilities...>\n",
 *   - subsequent ref lines are "<sha> <refname>\n".
 */
static bool parse_ref_advertisement(const unsigned char *data, size_t len,
                                    AdvRef **out_refs, size_t *out_n,
                                    char **out_head_symref) {
    *out_refs = NULL; *out_n = 0; *out_head_symref = NULL;
    AdvRef *refs = NULL; size_t n = 0, cap = 0;
    size_t i = 0;
    while (i + 4 <= len) {
        /* 4-hex length prefix. */
        char lenhex[5];
        memcpy(lenhex, data + i, 4); lenhex[4] = '\0';
        for (int k = 0; k < 4; k++) {
            char c = lenhex[k];
            if (!((c>='0'&&c<='9')||(c>='a'&&c<='f')||(c>='A'&&c<='F'))) goto done; /* not pkt-line */
        }
        unsigned long plen = strtoul(lenhex, NULL, 16);
        if (plen == 0) { i += 4; continue; }      /* flush pkt */
        if (plen < 4 || i + plen > len) break;     /* malformed / truncated */
        const unsigned char *payload = data + i + 4;
        size_t paylen = plen - 4;
        i += plen;

        /* Skip the "# service=..." banner line. */
        if (paylen >= 1 && payload[0] == '#') continue;

        /* A ref line needs at least "<40-hex> <c>". */
        if (paylen >= 42 && is_hex40(payload) && payload[40] == ' ') {
            /* refname runs until NUL (capabilities separator) or LF/end. */
            size_t j = 41;
            while (j < paylen && payload[j] != '\0' && payload[j] != '\n') j++;
            size_t namelen = j - 41;

            if (n == cap) {
                cap = cap ? cap * 2 : 16;
                refs = myon_xrealloc(refs, cap * sizeof(AdvRef));
            }
            memcpy(refs[n].sha, payload, 40); refs[n].sha[40] = '\0';
            refs[n].name = myon_strndup((const char*)payload + 41, namelen);
            n++;

            /* Capability section (after the first NUL) may carry symref=HEAD:. */
            if (!*out_head_symref) {
                size_t nul = 41 + namelen;
                if (nul < paylen && payload[nul] == '\0') {
                    const char *caps = (const char*)payload + nul + 1;
                    size_t capslen = paylen - (nul + 1);
                    /* Bounded search for "symref=HEAD:" within the caps blob. */
                    static const char key[] = "symref=HEAD:";
                    for (size_t s = 0; s + (sizeof(key)-1) <= capslen; s++) {
                        if (memcmp(caps + s, key, sizeof(key)-1) == 0) {
                            const char *t = caps + s + (sizeof(key)-1);
                            size_t tl = 0;
                            while ((size_t)(t - caps) + tl < capslen &&
                                   t[tl] != ' ' && t[tl] != '\n' && t[tl] != '\0') tl++;
                            *out_head_symref = myon_strndup(t, tl);
                            break;
                        }
                    }
                }
            }
        }
    }
done:
    *out_refs = refs; *out_n = n;
    return n > 0;
}

/* Free a decoded advertisement. */
static void free_refs(AdvRef *refs, size_t n) {
    for (size_t i = 0; i < n; i++) free(refs[i].name);
    free(refs);
}

/*
 * Given the decoded advertisement, resolve `ref` (branch / tag / "HEAD" /
 * default) to a 40-hex SHA.  Matching order (most specific first):
 *   - exact refname match ("refs/heads/x", "refs/tags/x", "HEAD"),
 *   - annotated-tag peeled object "refs/tags/<ref>^{}" (prefer the commit),
 *   - "refs/heads/<ref>" then "refs/tags/<ref>",
 *   - for the default branch: follow the HEAD symref, else the "HEAD" entry.
 * Returns true and writes out_sha on success.
 */
static bool select_ref_sha(AdvRef *refs, size_t n, const char *head_symref,
                           const char *ref, char *out_sha) {
    const char *want = (ref && *ref) ? ref : NULL;

    if (!want) {
        /* Default branch: prefer the HEAD symref target, else the HEAD entry. */
        if (head_symref) {
            for (size_t i = 0; i < n; i++)
                if (strcmp(refs[i].name, head_symref) == 0) { memcpy(out_sha, refs[i].sha, 41); return true; }
        }
        for (size_t i = 0; i < n; i++)
            if (strcmp(refs[i].name, "HEAD") == 0) { memcpy(out_sha, refs[i].sha, 41); return true; }
        return false;
    }

    /* Exact refname (lets callers pass fully-qualified refs if they want). */
    for (size_t i = 0; i < n; i++)
        if (strcmp(refs[i].name, want) == 0) { memcpy(out_sha, refs[i].sha, 41); return true; }

    char qualified[512];
    /* Annotated tag: the peeled "^{}" entry points at the underlying commit. */
    snprintf(qualified, sizeof(qualified), "refs/tags/%s^{}", want);
    for (size_t i = 0; i < n; i++)
        if (strcmp(refs[i].name, qualified) == 0) { memcpy(out_sha, refs[i].sha, 41); return true; }

    snprintf(qualified, sizeof(qualified), "refs/heads/%s", want);
    for (size_t i = 0; i < n; i++)
        if (strcmp(refs[i].name, qualified) == 0) { memcpy(out_sha, refs[i].sha, 41); return true; }

    snprintf(qualified, sizeof(qualified), "refs/tags/%s", want);
    for (size_t i = 0; i < n; i++)
        if (strcmp(refs[i].name, qualified) == 0) { memcpy(out_sha, refs[i].sha, 41); return true; }

    return false;
}

/*
 * Strategy 1: resolve via git smart-HTTP ref discovery on github.com.
 * Returns true (out_sha set) on a successful match, false otherwise; when
 * `*out_transport_ok` is set to false the transport itself failed (so the
 * REST fallback would likely fail too), while a true value with a false return
 * means "server answered but the ref was not found here".
 */
static bool resolve_ref_git_protocol(const PkgTransport *tr, const char *owner,
                                      const char *repo, const char *ref,
                                      char *out_sha, bool *out_transport_ok,
                                      char **err_msg) {
    *out_transport_ok = false;
    char path[1024];
    int pn = snprintf(path, sizeof(path),
                      "/%s/%s.git/info/refs?service=git-upload-pack", owner, repo);
    if (pn <= 0 || pn >= (int)sizeof(path)) { fset(err_msg, "resolve: ref path too long"); return false; }

    int status = 0; char *loc = NULL;
    Buf body; buf_init(&body);
    /* git advertises with this content-type; a plain Accept is fine. */
    bool ok = http_once(tr, "github.com", 443, path,
                        "*/*", &status, &loc, &body, err_msg);
    /* One safe redirect (github.com may 301 to add/strip ".git"). */
    int hops = 0;
    while (ok && (status == 301 || status == 302 || status == 307 || status == 308)
           && loc && strncmp(loc, "https://", 8) == 0 && hops < PKG_FETCH_MAX_REDIRECTS) {
        char *h2 = NULL, *p2 = NULL; int port2 = 443;
        if (!pkg_fetch_parse_url(loc, &h2, &port2, &p2, err_msg)) { ok = false; break; }
        if (!host_allowed(h2)) { free(h2); free(p2); fset(err_msg, "resolve: redirect to non-GitHub host"); ok = false; break; }
        buf_free(&body); buf_init(&body);
        char *loc2 = NULL; status = 0;
        ok = http_once(tr, h2, port2, p2, "*/*", &status, &loc2, &body, err_msg);
        free(h2); free(p2);
        free(loc); loc = loc2;
        hops++;
    }
    free(loc);
    if (!ok) { buf_free(&body); return false; } /* transport error */
    *out_transport_ok = true;
    if (status != 200) { buf_free(&body); return false; } /* not here (404 etc.) */

    AdvRef *refs = NULL; size_t n = 0; char *head_symref = NULL;
    bool parsed = parse_ref_advertisement(body.data, body.len, &refs, &n, &head_symref);
    buf_free(&body);
    if (!parsed) { free_refs(refs, n); free(head_symref); return false; }

    bool got = select_ref_sha(refs, n, head_symref, ref, out_sha);
    free_refs(refs, n);
    free(head_symref);
    return got;
}

/* Strategy 2: the classic api.github.com REST fallback. */
static bool resolve_ref_rest_api(const PkgTransport *tr, const char *owner,
                                 const char *repo, const char *ref,
                                 char *out_sha, char **err_msg) {
    const char *r = (ref && *ref) ? ref : "HEAD";
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
        else if (status == 403 || status == 429) snprintf(msg, sizeof(msg), "resolve: GitHub API rate limit or forbidden (HTTP %d)", status);
        else snprintf(msg, sizeof(msg), "resolve: GitHub API returned HTTP %d", status);
        fset(err_msg, msg);
        return false;
    }
    size_t n = body.len;
    while (n > 0 && (body.data[n-1] == '\n' || body.data[n-1] == '\r' || body.data[n-1] == ' ')) n--;
    if (n != 40) { buf_free(&body); fset(err_msg, "resolve: unexpected GitHub API response (not a commit SHA)"); return false; }
    if (!is_hex40(body.data)) { buf_free(&body); fset(err_msg, "resolve: API returned a non-hex SHA"); return false; }
    memcpy(out_sha, body.data, 40); out_sha[40] = '\0';
    buf_free(&body);
    return true;
}

bool pkg_fetch_resolve_ref(const PkgTransport *tr, const char *owner,
                           const char *repo, const char *ref,
                           char *out_sha, char **err_msg) {
    /* A full commit SHA is already immutable: no network needed. */
    if (ref && *ref && strlen(ref) == 40 && is_hex40((const unsigned char*)ref)) {
        for (int i = 0; i < 40; i++) {
            char c = ref[i];
            /* normalise to lowercase hex for the canonical source form */
            out_sha[i] = (c >= 'A' && c <= 'F') ? (char)(c - 'A' + 'a') : c;
        }
        out_sha[40] = '\0';
        return true;
    }

    /* Strategy 1: git smart-HTTP ref discovery (avoids the REST rate limit). */
    {
        char *gerr = NULL;
        bool transport_ok = false;
        if (resolve_ref_git_protocol(tr, owner, repo, ref, out_sha, &transport_ok, &gerr)) {
            free(gerr);
            return true;
        }
        free(gerr);
        /*
         * If the git endpoint answered but the ref was simply not present, the
         * repository exists yet the ref is wrong — a REST retry would only cost
         * a rate-limited request to return the same "not found".  Still, we let
         * the REST path produce the precise diagnostic below.
         */
        (void)transport_ok;
    }

    /* Strategy 2: REST API fallback (kept for robustness). */
    return resolve_ref_rest_api(tr, owner, repo, ref, out_sha, err_msg);
}
