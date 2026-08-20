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

#ifndef MYON_PKG_FETCH_H
#define MYON_PKG_FETCH_H

/*
 * Package-manager network layer (spec §7): a small, binary-safe HTTPS GET
 * client built specifically for the package manager, deliberately separate
 * from myon.http (whose NUL-terminated string API is not binary-safe and whose
 * redirect/chunked/header policy does not match what a package fetch needs).
 *
 * The transport (socket connect + TLS) is reached through a swappable seam
 * (PkgTransport) so the HTTP framing — status line, headers, Content-Length,
 * chunked transfer-encoding, redirect handling, and every size/limit check —
 * can be unit-tested offline with a canned transport, exactly as spec §11.2
 * requires ("外部 GitHub への実通信を必須 CI にしてはならない").
 *
 * Security policy enforced here (spec §7):
 *   - HTTPS only; an http:// URL (or an https->http redirect) is refused,
 *   - TLS certificate + hostname verification delegated to tls.c (fail closed),
 *   - at most PKG_FETCH_MAX_REDIRECTS redirects,
 *   - Location headers with CR/LF/NUL/control bytes rejected,
 *   - redirects only to an allow-listed set of GitHub hosts,
 *   - only HTTP 200 is a success for the body fetch,
 *   - Content-Length (if present) and the running body size are both capped,
 *   - chunked transfer-encoding decoded safely, or refused if malformed,
 *   - a total download cap (PKG_FETCH_MAX_BODY) independent of Content-Length,
 *   - the body is held as a raw (data,len) buffer, never a C string.
 */

#include <stddef.h>
#include <stdbool.h>

/* Download cap regardless of any advertised Content-Length (spec §7). */
#define PKG_FETCH_MAX_BODY      (64u * 1024u * 1024u) /* 64 MiB */
#define PKG_FETCH_MAX_REDIRECTS 5
#define PKG_FETCH_TIMEOUT_SECS  30

/* ------------------------------------------------------------------ */
/* Transport seam                                                      */
/* ------------------------------------------------------------------ */

/*
 * A transport opens a TLS-protected byte stream to (host, port) and lets the
 * caller write the request and read the response.  The default implementation
 * (pkg_transport_default) uses net.c + tls.c; tests supply their own.
 *
 * connect(): returns an opaque handle (or NULL + *err_msg on failure).
 * write():   send all `len` bytes; return true on success.
 * read():    read up to `cap` bytes into buf; return count (>0), 0 at clean
 *            EOF, or -1 on error (*err_msg set).
 * close():   release the handle.
 */
typedef struct PkgTransport {
    void *(*connect)(const char *host, int port, char **err_msg, void *ctx);
    bool  (*write)(void *handle, const unsigned char *data, size_t len, char **err_msg);
    long  (*read)(void *handle, unsigned char *buf, size_t cap, char **err_msg);
    void  (*close)(void *handle);
    void  *ctx; /* opaque per-transport state (test fixtures use this) */
} PkgTransport;

/* The production transport (net.c + tls.c, HTTPS, fail-closed). */
const PkgTransport *pkg_transport_default(void);

/* ------------------------------------------------------------------ */
/* HTTPS GET                                                           */
/* ------------------------------------------------------------------ */

/*
 * Fetch `url` (which MUST be https://) with the given transport, following up
 * to PKG_FETCH_MAX_REDIRECTS safe redirects.  On success returns true and
 * stores a freshly-allocated body buffer + length in *out_data / *out_len
 * (caller frees *out_data).  On any failure returns false and sets *err_msg
 * (caller frees).  Never returns a partial/truncated body as success.
 *
 * Pass pkg_transport_default() for real network use.
 */
bool pkg_fetch_https_get(const PkgTransport *tr, const char *url,
                         unsigned char **out_data, size_t *out_len,
                         char **err_msg);

/*
 * Like pkg_fetch_https_get(), but the host is NOT restricted to the GitHub
 * allow-list.  This is used only for fetching package-registry list files
 * (`.myon/packages.list` entries), whose URLs legitimately point at arbitrary
 * third-party hosts.  Every other safety property is unchanged: HTTPS only, no
 * https->http downgrade on redirect, no embedded credentials, the redirect cap,
 * and the total body-size cap all still apply.  The fetched body is registry
 * metadata (JSON), never executed as code.
 */
bool pkg_fetch_https_get_any(const PkgTransport *tr, const char *url,
                             unsigned char **out_data, size_t *out_len,
                             char **err_msg);

/*
 * Parse an https:// URL into host / port / path (all heap, caller frees the
 * three via the single free of *host after using them — actually each is a
 * separate allocation; free host, path individually).  Rejects non-https,
 * control bytes, embedded credentials.  Exposed for unit tests.
 */
bool pkg_fetch_parse_url(const char *url, char **host, int *port, char **path,
                         char **err_msg);

/* ------------------------------------------------------------------ */
/* GitHub ref resolution (spec §2.3)                                   */
/* ------------------------------------------------------------------ */

/*
 * Resolve a GitHub ref (branch/tag/full-sha/HEAD-for-default) for
 * <owner>/<repo> to a full 40-hex commit SHA.  `ref` may be NULL/empty to mean
 * the repository default branch.  On success writes 40 lowercase hex chars +
 * NUL into `out_sha` (>= 41 bytes) and returns true; on failure returns
 * false + *err_msg.
 *
 * Resolution strategy (see pkg_fetch.c for the rationale):
 *
 *   0. A ref that is already a full 40-hex SHA is immutable and returned
 *      immediately with no network request.
 *   1. git "smart HTTP" ref discovery on github.com:
 *         GET https://github.com/<owner>/<repo>.git/info/refs
 *                 ?service=git-upload-pack
 *      This is the transport `git clone` uses; it lists every ref -> SHA in one
 *      response and is NOT subject to the very low unauthenticated
 *      api.github.com REST rate limit (60/hour, tightened 2025-05).  This is
 *      the primary path and avoids rate-limit failures in normal use.
 *   2. api.github.com REST fallback:
 *         GET https://api.github.com/repos/<owner>/<repo>/commits/<ref>
 *         Accept: application/vnd.github.sha
 *      Used only if strategy 1 does not yield a SHA.
 *
 * Uses the same PkgTransport seam so it is testable offline.
 */
bool pkg_fetch_resolve_ref(const PkgTransport *tr, const char *owner,
                           const char *repo, const char *ref,
                           char *out_sha, char **err_msg);

#endif /* MYON_PKG_FETCH_H */
