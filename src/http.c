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

#include "http.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <ctype.h>

/* ------------------------------------------------------------------ */
/* small helpers                                                       */
/* ------------------------------------------------------------------ */

static char *dupn(const char *s, size_t n) {
    char *m = (char *)malloc(n + 1);
    if (!m) return NULL;
    memcpy(m, s, n);
    m[n] = '\0';
    return m;
}

static char *dupstr(const char *s) { return dupn(s, strlen(s)); }

/* ------------------------------------------------------------------ */
/* header-end detection                                                */
/* ------------------------------------------------------------------ */

size_t http_find_header_end(const char *buf, size_t len) {
    for (size_t i = 0; i + 3 < len; i++) {
        if (buf[i] == '\r' && buf[i + 1] == '\n' &&
            buf[i + 2] == '\r' && buf[i + 3] == '\n')
            return i + 4;
    }
    /* also accept bare LF LF (lenient parser) */
    for (size_t i = 0; i + 1 < len; i++) {
        if (buf[i] == '\n' && buf[i + 1] == '\n')
            return i + 2;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* request parsing                                                     */
/* ------------------------------------------------------------------ */

static long parse_content_length(const char *headers, size_t hlen) {
    /* case-insensitive search for "content-length:" line */
    const char *key = "content-length:";
    size_t klen = strlen(key);
    for (size_t i = 0; i + klen < hlen; i++) {
        /* line start? (i==0 or preceded by '\n') */
        if (i != 0 && headers[i - 1] != '\n') continue;
        int match = 1;
        for (size_t j = 0; j < klen; j++) {
            if (tolower((unsigned char)headers[i + j]) != key[j]) { match = 0; break; }
        }
        if (!match) continue;
        const char *v = headers + i + klen;
        while (*v == ' ' || *v == '\t') v++;
        return strtol(v, NULL, 10);
    }
    return 0;
}

int http_parse_request(const char *buf, size_t header_len,
                       const char *body_start, size_t body_len,
                       HttpRequest *out) {
    memset(out, 0, sizeof(*out));

    /* request line: METHOD SP PATH SP VERSION CRLF */
    const char *sp1 = memchr(buf, ' ', header_len);
    if (!sp1) return -1;
    size_t mlen = (size_t)(sp1 - buf);
    if (mlen == 0) return -1;
    if (mlen >= sizeof(out->method)) mlen = sizeof(out->method) - 1;
    memcpy(out->method, buf, mlen);
    out->method[mlen] = '\0';

    const char *pstart = sp1 + 1;
    const char *sp2 = memchr(pstart, ' ', header_len - (size_t)(pstart - buf));
    const char *pend;
    if (sp2) {
        pend = sp2;
    } else {
        /* HTTP/0.9-ish: path runs to end of line */
        pend = memchr(pstart, '\r', header_len - (size_t)(pstart - buf));
        if (!pend) pend = memchr(pstart, '\n', header_len - (size_t)(pstart - buf));
        if (!pend) return -1;
    }
    out->path = dupn(pstart, (size_t)(pend - pstart));
    if (!out->path) return -1;

    out->content_length = parse_content_length(buf, header_len);

    /* body: we hand back whatever bytes were already read */
    out->body = body_len ? dupn(body_start, body_len) : dupstr("");
    if (!out->body) { free(out->path); out->path = NULL; return -1; }

    return 0;
}

void http_request_free(HttpRequest *r) {
    if (!r) return;
    free(r->path); r->path = NULL;
    free(r->body); r->body = NULL;
}

/* ------------------------------------------------------------------ */
/* content types                                                       */
/* ------------------------------------------------------------------ */

const char *http_content_type_for_path(const char *path) {
    const char *dot = strrchr(path, '.');
    if (!dot) return "application/octet-stream";
    if (strcmp(dot, ".html") == 0 || strcmp(dot, ".htm") == 0) return "text/html";
    if (strcmp(dot, ".css")  == 0) return "text/css";
    if (strcmp(dot, ".js")   == 0) return "application/javascript";
    if (strcmp(dot, ".json") == 0) return "application/json";
    if (strcmp(dot, ".png")  == 0) return "image/png";
    if (strcmp(dot, ".jpg")  == 0 || strcmp(dot, ".jpeg") == 0) return "image/jpeg";
    if (strcmp(dot, ".gif")  == 0) return "image/gif";
    if (strcmp(dot, ".svg")  == 0) return "image/svg+xml";
    if (strcmp(dot, ".txt")  == 0 || strcmp(dot, ".md") == 0) return "text/plain";
    return "application/octet-stream";
}

/* ------------------------------------------------------------------ */
/* response building                                                   */
/* ------------------------------------------------------------------ */

char *http_build_response(int status, const char *status_text,
                          const char *content_type,
                          const char *body, size_t body_len,
                          size_t *out_len) {
    char header[512];
    int hn = snprintf(header, sizeof(header),
        "HTTP/1.0 %d %s\r\n"
        "Content-Type: %s\r\n"
        "Content-Length: %zu\r\n"
        "Connection: close\r\n"
        "\r\n",
        status, status_text ? status_text : "OK",
        content_type ? content_type : "text/plain",
        body_len);
    if (hn < 0) return NULL;
    size_t total = (size_t)hn + body_len;
    char *buf = (char *)malloc(total + 1);
    if (!buf) return NULL;
    memcpy(buf, header, (size_t)hn);
    if (body_len) memcpy(buf + hn, body, body_len);
    buf[total] = '\0';
    if (out_len) *out_len = total;
    return buf;
}

/* ------------------------------------------------------------------ */
/* static-file path resolution (directory-traversal safe)             */
/* ------------------------------------------------------------------ */

char *http_resolve_static_path(const char *root_dir, const char *req_path) {
    if (!root_dir || !req_path) return NULL;

    /* strip a query string, if any */
    const char *q = strchr(req_path, '?');
    size_t plen = q ? (size_t)(q - req_path) : strlen(req_path);

    /* the request path must be absolute ("/....") */
    if (plen == 0 || req_path[0] != '/') return NULL;

    /* reject any ".." segment outright (traversal defence) */
    for (size_t i = 0; i + 1 < plen; i++) {
        if (req_path[i] == '.' && req_path[i + 1] == '.')
            return NULL;
    }
    /* reject NUL and control bytes */
    for (size_t i = 0; i < plen; i++) {
        unsigned char c = (unsigned char)req_path[i];
        if (c < 0x20) return NULL;
    }

    /* "/" (or "/dir/") maps to index.html */
    const char *rel = req_path; /* includes leading '/' */
    int add_index = (plen == 1 && rel[0] == '/');       /* exactly "/"      */
    if (!add_index && rel[plen - 1] == '/') add_index = 1; /* trailing slash */

    size_t rootlen = strlen(root_dir);
    /* trim a trailing '/' on root to avoid "root//path" */
    while (rootlen > 1 && root_dir[rootlen - 1] == '/') rootlen--;

    const char *index = "index.html";
    size_t need = rootlen + plen + (add_index ? strlen(index) : 0) + 2;
    char *full = (char *)malloc(need);
    if (!full) return NULL;
    memcpy(full, root_dir, rootlen);
    full[rootlen] = '\0';
    strncat(full, rel, plen);
    if (add_index) strcat(full, index);
    return full;
}
