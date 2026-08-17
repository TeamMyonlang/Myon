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

#ifndef MYON_HTTP_H
#define MYON_HTTP_H

#include <stddef.h>

/*
 * Phase5, Step5: minimal HTTP helpers (myon.http).
 *
 * This header deliberately contains only *pure* HTTP plumbing — request
 * parsing, response building, content-type guessing and safe static-file
 * path resolution.  It does NOT touch sockets or the event loop; the
 * interpreter side (interpreter.c) owns those and drives net.c / event_loop.c
 * so module dependencies stay one-directional (interpreter.c -> http.c,
 * interpreter.c -> net.c, interpreter.c -> event_loop.c).
 *
 * Only HTTP/1.0-style, one-request-per-connection behaviour is supported
 * (no Keep-Alive, no chunked transfer).  See docs/myon_spec.md 10.6.
 */

/* A parsed request line + the few headers we care about. */
typedef struct {
    char  method[16];   /* "GET", "POST", ... (truncated if longer)         */
    char *path;         /* malloc'd, decoded request path (caller frees)    */
    char *body;         /* malloc'd request body (may be ""), caller frees  */
    long  content_length; /* Content-Length header value, or 0 if absent    */
} HttpRequest;

/* Parse a complete request buffer (headers up to and including the blank
 * line, plus `body_len` bytes of already-read body starting at `body_start`).
 * Returns 0 on success (fills *out; out->path / out->body are malloc'd),
 * or -1 on a malformed request line. */
int  http_parse_request(const char *buf, size_t header_len,
                        const char *body_start, size_t body_len,
                        HttpRequest *out);

void http_request_free(HttpRequest *r);

/* Find the end of the request headers ("\r\n\r\n" or "\n\n") within the first
 * `len` bytes of `buf`.  Returns the byte offset just past the terminator
 * (i.e. where the body begins) on success, or 0 if the terminator is not yet
 * present (caller should read more). */
size_t http_find_header_end(const char *buf, size_t len);

/* Guess a Content-Type from a file path's extension.  Never returns NULL. */
const char *http_content_type_for_path(const char *path);

/* Reason phrase for an HTTP status code (e.g. 404 -> "Not Found").  Returns a
 * static string; never NULL.  Unrecognised codes fall back to a per-class
 * phrase. */
const char *http_status_text(int status);

/* Build a full HTTP/1.0 response into a freshly malloc'd buffer.
 * `*out_len` receives the total byte length (headers + body).  Caller frees
 * the returned pointer.  Returns NULL only on allocation failure. */
char *http_build_response(int status, const char *status_text,
                          const char *content_type,
                          const char *body, size_t body_len,
                          size_t *out_len);

/* Resolve a request path against a root directory, rejecting directory
 * traversal ("..") and absolute escapes.  On success returns a malloc'd
 * filesystem path (caller frees); on rejection returns NULL. A trailing "/"
 * (or empty path) maps to "<root>/index.html". */
char *http_resolve_static_path(const char *root_dir, const char *req_path);

#endif /* MYON_HTTP_H */
