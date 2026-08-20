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

/*
 * Myon package manager — data model, parsers, validators and CLI (spec §1–§5).
 *
 * This translation unit is self-contained: it does not include the
 * interpreter, MVM, or myon.http.  It only depends on the C standard library
 * (plus common.h for the abort-on-OOM allocators and strdup helpers).
 *
 * GitHub URL / API facts used here were verified on 2026-08-19 against the
 * live service and the official docs; see docs/package_manager.md for the
 * exact URLs and observed responses.  In summary:
 *
 *   - archive (direct):  https://codeload.github.com/<owner>/<repo>/zip/<sha>
 *                        -> HTTP 200, zip body, no redirect.
 *   - archive (github):  https://github.com/<owner>/<repo>/archive/<sha>.zip
 *                        -> HTTP 302 to the codeload URL above.
 *   - ref -> full SHA:   GET https://api.github.com/repos/<o>/<r>/commits/<ref>
 *                        with `Accept: application/vnd.github.sha` returns the
 *                        40-hex SHA as the plain-text body.
 *   - unauth rate limit: 60 requests/hour.
 *
 * The network resolution that consumes these facts belongs to a later phase;
 * this file only classifies URLs and never contacts the network.
 */

#include "platform.h"

#include "package.h"
#include "common.h"

#include <stdarg.h>
#include <string.h>
#include <ctype.h>

/*
 * getcwd(): <unistd.h> on POSIX, <direct.h> (as _getcwd) on Windows.  We keep
 * the POSIX name usable on both by aliasing on Windows.
 */
#if defined(MYON_OS_WINDOWS)
#  include <direct.h>
#  define getcwd _getcwd
#else
#  include <unistd.h>
#endif

/* ================================================================== */
/* small string helpers                                               */
/* ================================================================== */

static char *xstrdup0(const char *s) { return s ? myon_strdup(s) : NULL; }

/* Grow-only dynamic char buffer for deterministic serialisation. */
typedef struct {
    char  *data;
    size_t len;
    size_t cap;
} StrBuf;

static void sb_init(StrBuf *b) { b->data = myon_xmalloc(64); b->data[0] = '\0'; b->len = 0; b->cap = 64; }
static void sb_reserve(StrBuf *b, size_t extra) {
    size_t need;
    if (!checked_add_size(b->len, extra, &need) || !checked_add_size(need, 1, &need)) {
        fprintf(stderr, "myon: package buffer overflow\n"); exit(70);
    }
    if (need > b->cap) {
        while (b->cap < need) b->cap *= 2;
        b->data = myon_xrealloc(b->data, b->cap);
    }
}
static void sb_append(StrBuf *b, const char *s) {
    size_t n = strlen(s);
    sb_reserve(b, n);
    memcpy(b->data + b->len, s, n + 1);
    b->len += n;
}
static char *sb_take(StrBuf *b) { char *p = b->data; b->data = NULL; return p; }

/* ================================================================== */
/* error helpers                                                      */
/* ================================================================== */

void pkg_error_init(PkgError *err) {
    if (!err) return;
    err->code = PKG_OK;
    err->line = 0;
    err->message = NULL;
}

void pkg_error_reset(PkgError *err) {
    if (!err) return;
    free(err->message);
    err->message = NULL;
    err->code = PKG_OK;
    err->line = 0;
}

void pkg_error_set(PkgError *err, PkgErrorCode code, int line,
                   const char *fmt, ...) {
    if (!err) return;
    free(err->message);
    err->message = NULL;
    err->code = code;
    err->line = line;

    va_list ap;
    va_start(ap, fmt);
    va_list ap2;
    va_copy(ap2, ap);
    int n = vsnprintf(NULL, 0, fmt, ap);
    va_end(ap);
    if (n < 0) { va_end(ap2); err->message = myon_strdup("(error)"); return; }
    err->message = myon_xmalloc((size_t)n + 1);
    vsnprintf(err->message, (size_t)n + 1, fmt, ap2);
    va_end(ap2);
}

/* ================================================================== */
/* low-level character/identity validation                            */
/* ================================================================== */

static bool is_lower_hex(int c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
}

bool pkg_is_full_sha(const char *s) {
    if (!s) return false;
    size_t n = 0;
    for (; s[n]; n++)
        if (!is_lower_hex((unsigned char)s[n])) return false;
    return n == PKG_SHA_LEN;
}

bool pkg_is_sha256_hex(const char *s) {
    if (!s) return false;
    size_t n = 0;
    for (; s[n]; n++)
        if (!is_lower_hex((unsigned char)s[n])) return false;
    return n == PKG_SHA256_HEX_LEN;
}

/*
 * Package name (spec §3.1): ASCII lowercase letters, digits, '.', '-'.
 * Reject: empty, path separators, whitespace, control chars, "..", a leading
 * or trailing '.', and (defensively) a leading or trailing '-'.
 */
bool pkg_validate_package_name(const char *name, int line, PkgError *err) {
    if (!name || name[0] == '\0') {
        pkg_error_set(err, PKG_ERR_MANIFEST, line, "empty package name");
        return false;
    }
    size_t n = strlen(name);
    if (name[0] == '.' || name[n - 1] == '.') {
        pkg_error_set(err, PKG_ERR_MANIFEST, line,
                      "package name '%s' must not start or end with '.'", name);
        return false;
    }
    if (name[0] == '-' || name[n - 1] == '-') {
        pkg_error_set(err, PKG_ERR_MANIFEST, line,
                      "package name '%s' must not start or end with '-'", name);
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)name[i];
        bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                  c == '.' || c == '-';
        if (!ok) {
            pkg_error_set(err, PKG_ERR_MANIFEST, line,
                          "invalid character in package name '%s' "
                          "(allowed: lowercase a-z, 0-9, '.', '-')", name);
            return false;
        }
        if (c == '.' && i + 1 < n && name[i + 1] == '.') {
            pkg_error_set(err, PKG_ERR_MANIFEST, line,
                          "package name '%s' must not contain '..'", name);
            return false;
        }
    }
    return true;
}

/*
 * Module namespace (spec §3.2 / §6.2): dotted path of lowercase segments; each
 * segment starts with a letter and continues with letters/digits/'-'; segments
 * are joined by single dots.  Examples: "acme.json", "example.tools".
 */
bool pkg_validate_module_name(const char *name, int line, PkgError *err) {
    if (!name || name[0] == '\0') {
        pkg_error_set(err, PKG_ERR_MANIFEST, line, "empty module name");
        return false;
    }
    size_t seg_start = 0;
    size_t n = strlen(name);
    for (size_t i = 0; i <= n; i++) {
        char c = name[i];
        if (c == '.' || c == '\0') {
            size_t seglen = i - seg_start;
            if (seglen == 0) {
                pkg_error_set(err, PKG_ERR_MANIFEST, line,
                              "module name '%s' has an empty segment", name);
                return false;
            }
            unsigned char first = (unsigned char)name[seg_start];
            if (!(first >= 'a' && first <= 'z')) {
                pkg_error_set(err, PKG_ERR_MANIFEST, line,
                              "module name '%s': each segment must start with "
                              "a lowercase letter", name);
                return false;
            }
            seg_start = i + 1;
            continue;
        }
        unsigned char uc = (unsigned char)c;
        bool ok = (uc >= 'a' && uc <= 'z') || (uc >= '0' && uc <= '9') ||
                  uc == '-';
        if (!ok) {
            pkg_error_set(err, PKG_ERR_MANIFEST, line,
                          "invalid character in module name '%s' "
                          "(allowed: lowercase a-z, 0-9, '-', '.')", name);
            return false;
        }
    }
    return true;
}

/* ================================================================== */
/* GitHub source (github:<owner>/<repo>@<sha>)                         */
/* ================================================================== */

void pkg_source_init(PkgSource *s) {
    s->owner = NULL;
    s->repo = NULL;
    s->sha[0] = '\0';
}

void pkg_source_reset(PkgSource *s) {
    if (!s) return;
    free(s->owner); s->owner = NULL;
    free(s->repo);  s->repo = NULL;
    s->sha[0] = '\0';
}

/* owner/repo path segment: GitHub allows alnum, '-', '_', '.' (not "..").    */
static bool valid_repo_segment(const char *s, size_t n) {
    if (n == 0) return false;
    if ((n == 1 && s[0] == '.') || (n == 2 && s[0] == '.' && s[1] == '.'))
        return false;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.';
        if (!ok) return false;
    }
    return true;
}

bool pkg_source_parse(const char *text, int line, PkgSource *out, PkgError *err) {
    pkg_source_init(out);
    if (!text) {
        pkg_error_set(err, PKG_ERR_MANIFEST, line, "empty source");
        return false;
    }
    /* required prefix "github:" */
    const char *p = text;
    if (strncmp(p, "github:", 7) != 0) {
        pkg_error_set(err, PKG_ERR_MANIFEST, line,
                      "source '%s' must start with 'github:' "
                      "(only GitHub full-SHA sources are allowed)", text);
        return false;
    }
    p += 7;
    const char *slash = strchr(p, '/');
    if (!slash) {
        pkg_error_set(err, PKG_ERR_MANIFEST, line,
                      "source '%s' must be github:<owner>/<repo>@<sha>", text);
        return false;
    }
    const char *at = strchr(slash + 1, '@');
    if (!at) {
        pkg_error_set(err, PKG_ERR_MANIFEST, line,
                      "source '%s' is missing '@<commit-sha>'", text);
        return false;
    }
    size_t owner_len = (size_t)(slash - p);
    size_t repo_len  = (size_t)(at - (slash + 1));
    const char *sha  = at + 1;

    if (!valid_repo_segment(p, owner_len)) {
        pkg_error_set(err, PKG_ERR_MANIFEST, line,
                      "source '%s' has an invalid owner", text);
        return false;
    }
    if (!valid_repo_segment(slash + 1, repo_len)) {
        pkg_error_set(err, PKG_ERR_MANIFEST, line,
                      "source '%s' has an invalid repository", text);
        return false;
    }
    if (!pkg_is_full_sha(sha)) {
        pkg_error_set(err, PKG_ERR_MANIFEST, line,
                      "source '%s' must pin a 40-character lowercase hex commit "
                      "SHA (branches, tags, 'latest' and short SHAs are rejected)",
                      text);
        return false;
    }
    out->owner = myon_strndup(p, owner_len);
    out->repo  = myon_strndup(slash + 1, repo_len);
    memcpy(out->sha, sha, PKG_SHA_LEN);
    out->sha[PKG_SHA_LEN] = '\0';
    return true;
}

char *pkg_source_canonical_no_sha(const PkgSource *s) {
    StrBuf b; sb_init(&b);
    sb_append(&b, "github:");
    sb_append(&b, s->owner);
    sb_append(&b, "/");
    sb_append(&b, s->repo);
    return sb_take(&b);
}

char *pkg_source_archive_url(const PkgSource *s) {
    StrBuf b; sb_init(&b);
    sb_append(&b, "https://codeload.github.com/");
    sb_append(&b, s->owner);
    sb_append(&b, "/");
    sb_append(&b, s->repo);
    sb_append(&b, "/zip/");
    sb_append(&b, s->sha);
    return sb_take(&b);
}

/* Deep-copy a source (used by lock upsert). */
static void source_copy(PkgSource *dst, const PkgSource *src) {
    dst->owner = xstrdup0(src->owner);
    dst->repo  = xstrdup0(src->repo);
    memcpy(dst->sha, src->sha, sizeof(dst->sha));
}

/* ================================================================== */
/* GitHub install-URL parsing (`myon pkg install <GitHub URL>`)        */
/* ================================================================== */

void pkg_install_url_init(PkgInstallUrl *u) {
    u->owner = NULL;
    u->repo = NULL;
    u->ref_kind = PKG_REF_DEFAULT;
    u->ref = NULL;
}

void pkg_install_url_reset(PkgInstallUrl *u) {
    if (!u) return;
    free(u->owner); u->owner = NULL;
    free(u->repo);  u->repo = NULL;
    free(u->ref);   u->ref = NULL;
    u->ref_kind = PKG_REF_DEFAULT;
}

/* Reject control bytes / whitespace anywhere in a URL. */
static bool url_has_control_bytes(const char *s) {
    for (const char *p = s; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c < 0x20 || c == 0x7f) return true;
    }
    return false;
}

/*
 * Parse a user-facing GitHub URL.  Verified URL shapes (2026-08-19):
 *   https://github.com/<owner>/<repo>[.git]
 *   https://github.com/<owner>/<repo>/tree/<ref>
 *   https://github.com/<owner>/<repo>/commit/<sha>
 *   https://github.com/<owner>/<repo>/releases/tag/<tag>
 */
bool pkg_install_url_parse(const char *url, PkgInstallUrl *out, PkgError *err) {
    pkg_install_url_init(out);
    if (!url || url[0] == '\0') {
        pkg_error_set(err, PKG_ERR_USAGE, 0, "empty install URL");
        return false;
    }
    if (url_has_control_bytes(url)) {
        pkg_error_set(err, PKG_ERR_USAGE, 0,
                      "install URL contains illegal control characters");
        return false;
    }
    const char *p = url;
    if (strncmp(p, "https://", 8) != 0) {
        pkg_error_set(err, PKG_ERR_USAGE, 0,
                      "install URL must use https:// and point at github.com "
                      "(got '%s')", url);
        return false;
    }
    p += 8;
    /* Reject embedded credentials (user:pass@host). */
    const char *slash_after_host = strchr(p, '/');
    const char *at = memchr(p, '@', slash_after_host ? (size_t)(slash_after_host - p)
                                                     : strlen(p));
    if (at) {
        pkg_error_set(err, PKG_ERR_USAGE, 0,
                      "install URL must not embed credentials");
        return false;
    }
    /* host must be exactly github.com */
    static const char host[] = "github.com";
    size_t hlen = sizeof(host) - 1;
    if (strncmp(p, host, hlen) != 0 || (p[hlen] != '/' )) {
        pkg_error_set(err, PKG_ERR_USAGE, 0,
                      "install URL host must be github.com (got '%s')", url);
        return false;
    }
    p += hlen; /* now at the leading '/' of the path */

    /* Trim any query/fragment: refuse them rather than silently ignore. */
    if (strpbrk(p, "?#")) {
        pkg_error_set(err, PKG_ERR_USAGE, 0,
                      "install URL must not contain a query or fragment");
        return false;
    }

    /* Split the path into up to a handful of segments. */
    /* p starts with '/'. */
    p++;
    const char *seg[8];
    size_t seglen[8];
    int nseg = 0;
    while (*p && nseg < 8) {
        const char *s = p;
        while (*p && *p != '/') p++;
        seg[nseg] = s;
        seglen[nseg] = (size_t)(p - s);
        nseg++;
        if (*p == '/') p++;
    }
    if (*p) {
        pkg_error_set(err, PKG_ERR_USAGE, 0,
                      "install URL path is too deep to be a repository URL");
        return false;
    }
    /* Drop a trailing empty segment from a trailing slash. */
    if (nseg > 0 && seglen[nseg - 1] == 0) nseg--;

    if (nseg < 2) {
        pkg_error_set(err, PKG_ERR_USAGE, 0,
                      "install URL must be https://github.com/<owner>/<repo>...");
        return false;
    }

    /* owner + repo (strip a trailing ".git" on repo). */
    size_t owner_len = seglen[0];
    size_t repo_len  = seglen[1];
    if (repo_len > 4 && strncmp(seg[1] + repo_len - 4, ".git", 4) == 0)
        repo_len -= 4;

    if (!valid_repo_segment(seg[0], owner_len) ||
        !valid_repo_segment(seg[1], repo_len)) {
        pkg_error_set(err, PKG_ERR_USAGE, 0,
                      "install URL has an invalid owner/repository");
        return false;
    }
    out->owner = myon_strndup(seg[0], owner_len);
    out->repo  = myon_strndup(seg[1], repo_len);

    /* Classify any ref. */
    if (nseg == 2) {
        out->ref_kind = PKG_REF_DEFAULT;
        out->ref = NULL;
        return true;
    }

    /* .../tree/<ref>  or  .../commit/<sha> */
    if (nseg == 4 && seglen[2] == 4 && strncmp(seg[2], "tree", 4) == 0) {
        char *ref = myon_strndup(seg[3], seglen[3]);
        out->ref = ref;
        out->ref_kind = pkg_is_full_sha(ref) ? PKG_REF_COMMIT : PKG_REF_BRANCH;
        return true;
    }
    if (nseg == 4 && seglen[2] == 6 && strncmp(seg[2], "commit", 6) == 0) {
        out->ref = myon_strndup(seg[3], seglen[3]);
        out->ref_kind = PKG_REF_COMMIT;
        return true;
    }
    /* .../releases/tag/<tag> */
    if (nseg == 5 && seglen[2] == 8 && strncmp(seg[2], "releases", 8) == 0 &&
        seglen[3] == 3 && strncmp(seg[3], "tag", 3) == 0) {
        out->ref = myon_strndup(seg[4], seglen[4]);
        out->ref_kind = PKG_REF_TAG;
        return true;
    }

    pkg_install_url_reset(out);
    pkg_error_set(err, PKG_ERR_USAGE, 0,
                  "unsupported GitHub URL shape; use "
                  "https://github.com/<owner>/<repo>[/tree/<ref>|"
                  "/commit/<sha>|/releases/tag/<tag>]");
    return false;
}

/* ================================================================== */
/* Package-list registry ("myon pkg install <user>/<repo>")            */
/* ================================================================== */

/* A registry document is small metadata; refuse anything absurd. */
#define PKG_REGISTRY_MAX_BYTES   (4u * 1024u * 1024u) /* 4 MiB */
#define PKG_REGISTRY_MAX_ENTRIES 100000u
#define PKG_PKGLIST_MAX_URLS     4096u

void pkg_shorthand_init(PkgShorthand *s) { s->owner = NULL; s->repo = NULL; }
void pkg_shorthand_reset(PkgShorthand *s) {
    if (!s) return;
    free(s->owner); s->owner = NULL;
    free(s->repo);  s->repo = NULL;
}

bool pkg_arg_is_shorthand(const char *arg) {
    if (!arg || !*arg) return false;
    if (strstr(arg, "://")) return false;              /* it's a URL */
    /* exactly one '/', no control/whitespace, both sides non-empty */
    const char *slash = strchr(arg, '/');
    if (!slash || slash == arg) return false;
    if (strchr(slash + 1, '/')) return false;          /* more than one '/' */
    if (slash[1] == '\0') return false;                /* trailing slash */
    for (const char *p = arg; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c <= 0x20 || c == 0x7f) return false;      /* space/control */
    }
    return true;
}

bool pkg_shorthand_parse(const char *arg, PkgShorthand *out, PkgError *err) {
    pkg_shorthand_init(out);
    if (!pkg_arg_is_shorthand(arg)) {
        pkg_error_set(err, PKG_ERR_USAGE, 0,
                      "'%s' is not a valid <owner>/<repo> shorthand", arg ? arg : "");
        return false;
    }
    const char *slash = strchr(arg, '/');
    size_t owner_len = (size_t)(slash - arg);
    size_t repo_len  = strlen(slash + 1);
    /* strip a trailing ".git" the same way the URL parser does */
    if (repo_len > 4 && strncmp(slash + 1 + repo_len - 4, ".git", 4) == 0)
        repo_len -= 4;
    if (owner_len > 128 || repo_len > 128 || repo_len == 0) {
        pkg_error_set(err, PKG_ERR_USAGE, 0, "'%s' has an invalid owner/repository", arg);
        return false;
    }
    if (!valid_repo_segment(arg, owner_len) ||
        !valid_repo_segment(slash + 1, repo_len)) {
        pkg_error_set(err, PKG_ERR_USAGE, 0, "'%s' has an invalid owner/repository", arg);
        return false;
    }
    out->owner = myon_strndup(arg, owner_len);
    out->repo  = myon_strndup(slash + 1, repo_len);
    return true;
}

PkgRegistry *pkg_registry_new(void) {
    PkgRegistry *r = myon_xmalloc(sizeof(*r));
    r->entries = NULL; r->count = 0;
    return r;
}

void pkg_registry_free(PkgRegistry *r) {
    if (!r) return;
    for (size_t i = 0; i < r->count; i++) {
        free(r->entries[i].alias);
        free(r->entries[i].owner);
        free(r->entries[i].repo);
    }
    free(r->entries);
    free(r);
}

/* ---- minimal, strict JSON scanner (registry documents only) ---------- */
/*
 * We only need two JSON shapes, so this is a small hand-written scanner rather
 * than a general JSON parser.  It supports: whitespace, string literals with
 * the standard escapes (\" \\ \/ \b \f \n \r \t \uXXXX for the BMP), arrays of
 * strings, and objects with string keys and string values.  Anything else
 * (numbers, booleans, null, nested containers) is a hard error, because a
 * registry value is always a "<owner>/<repo>" string.  Control bytes inside
 * strings are rejected.
 */
typedef struct { const char *p; const char *end; } JScan;

static void j_skip_ws(JScan *j) {
    while (j->p < j->end) {
        char c = *j->p;
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') j->p++;
        else break;
    }
}

/* Append the UTF-8 encoding of a BMP code point to a StrBuf. */
static void j_append_utf8(StrBuf *b, unsigned cp) {
    char buf[4]; int n = 0;
    if (cp < 0x80) { buf[n++] = (char)cp; }
    else if (cp < 0x800) {
        buf[n++] = (char)(0xC0 | (cp >> 6));
        buf[n++] = (char)(0x80 | (cp & 0x3F));
    } else {
        buf[n++] = (char)(0xE0 | (cp >> 12));
        buf[n++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        buf[n++] = (char)(0x80 | (cp & 0x3F));
    }
    buf[n] = '\0';
    sb_append(b, buf);
}

/* Parse a JSON string literal at j->p (which must point at the opening '"').
 * Returns a fresh heap string on success, or NULL + *err. */
static char *j_parse_string(JScan *j, PkgError *err) {
    if (j->p >= j->end || *j->p != '"') {
        pkg_error_set(err, PKG_ERR_MANIFEST, 0, "registry JSON: expected a string");
        return NULL;
    }
    j->p++; /* opening quote */
    StrBuf b; sb_init(&b);
    while (j->p < j->end) {
        unsigned char c = (unsigned char)*j->p;
        if (c == '"') { j->p++; return sb_take(&b); }
        if (c < 0x20) { free(b.data); pkg_error_set(err, PKG_ERR_MANIFEST, 0, "registry JSON: control byte in string"); return NULL; }
        if (c == '\\') {
            j->p++;
            if (j->p >= j->end) break;
            char e = *j->p++;
            switch (e) {
                case '"':  sb_append(&b, "\""); break;
                case '\\': sb_append(&b, "\\"); break;
                case '/':  sb_append(&b, "/");  break;
                case 'b':  sb_append(&b, "\b"); break;
                case 'f':  sb_append(&b, "\f"); break;
                case 'n':  sb_append(&b, "\n"); break;
                case 'r':  sb_append(&b, "\r"); break;
                case 't':  sb_append(&b, "\t"); break;
                case 'u': {
                    if (j->end - j->p < 4) { free(b.data); pkg_error_set(err, PKG_ERR_MANIFEST, 0, "registry JSON: truncated \\u escape"); return NULL; }
                    unsigned cp = 0;
                    for (int k = 0; k < 4; k++) {
                        char h = j->p[k]; unsigned v;
                        if (h >= '0' && h <= '9') v = (unsigned)(h - '0');
                        else if (h >= 'a' && h <= 'f') v = (unsigned)(h - 'a' + 10);
                        else if (h >= 'A' && h <= 'F') v = (unsigned)(h - 'A' + 10);
                        else { free(b.data); pkg_error_set(err, PKG_ERR_MANIFEST, 0, "registry JSON: bad \\u escape"); return NULL; }
                        cp = (cp << 4) | v;
                    }
                    j->p += 4;
                    j_append_utf8(&b, cp);
                    break;
                }
                default:
                    free(b.data);
                    pkg_error_set(err, PKG_ERR_MANIFEST, 0, "registry JSON: invalid escape '\\%c'", e);
                    return NULL;
            }
        } else {
            char one[2] = { (char)c, '\0' };
            sb_append(&b, one);
            j->p++;
        }
    }
    free(b.data);
    pkg_error_set(err, PKG_ERR_MANIFEST, 0, "registry JSON: unterminated string");
    return NULL;
}

/* Split a validated "<owner>/<repo>" value into an entry (alias optional). */
static bool registry_add(PkgRegistry *r, const char *alias, const char *value,
                         PkgError *err) {
    PkgShorthand sh;
    if (!pkg_shorthand_parse(value, &sh, err)) {
        /* re-tag as manifest error: the registry document is malformed data */
        pkg_error_set(err, PKG_ERR_MANIFEST, 0,
                      "registry JSON: '%s' is not a valid <owner>/<repo>", value);
        return false;
    }
    if (r->count >= PKG_REGISTRY_MAX_ENTRIES) {
        pkg_shorthand_reset(&sh);
        pkg_error_set(err, PKG_ERR_MANIFEST, 0, "registry JSON: too many entries");
        return false;
    }
    r->entries = myon_xrealloc(r->entries, (r->count + 1) * sizeof(PkgRegistryEntry));
    r->entries[r->count].alias = alias ? myon_strdup(alias) : NULL;
    r->entries[r->count].owner = sh.owner; /* transfer ownership */
    r->entries[r->count].repo  = sh.repo;
    sh.owner = NULL; sh.repo = NULL;
    r->count++;
    return true;
}

PkgRegistry *pkg_registry_parse(const char *text, PkgError *err) {
    if (!text) { pkg_error_set(err, PKG_ERR_MANIFEST, 0, "registry JSON: empty document"); return NULL; }
    size_t len = strlen(text);
    if (len > PKG_REGISTRY_MAX_BYTES) {
        pkg_error_set(err, PKG_ERR_MANIFEST, 0, "registry JSON: document too large");
        return NULL;
    }
    JScan j = { text, text + len };
    PkgRegistry *r = pkg_registry_new();

    j_skip_ws(&j);
    if (j.p >= j.end) { pkg_registry_free(r); pkg_error_set(err, PKG_ERR_MANIFEST, 0, "registry JSON: empty document"); return NULL; }

    char open = *j.p;
    if (open == '[') {
        j.p++;
        j_skip_ws(&j);
        if (j.p < j.end && *j.p == ']') { j.p++; goto trailing; } /* empty array */
        for (;;) {
            j_skip_ws(&j);
            char *val = j_parse_string(&j, err);
            if (!val) { pkg_registry_free(r); return NULL; }
            bool ok = registry_add(r, NULL, val, err);
            free(val);
            if (!ok) { pkg_registry_free(r); return NULL; }
            j_skip_ws(&j);
            if (j.p < j.end && *j.p == ',') { j.p++; continue; }
            if (j.p < j.end && *j.p == ']') { j.p++; break; }
            pkg_registry_free(r);
            pkg_error_set(err, PKG_ERR_MANIFEST, 0, "registry JSON: expected ',' or ']' in array");
            return NULL;
        }
    } else if (open == '{') {
        j.p++;
        j_skip_ws(&j);
        if (j.p < j.end && *j.p == '}') { j.p++; goto trailing; } /* empty object */
        for (;;) {
            j_skip_ws(&j);
            char *key = j_parse_string(&j, err);
            if (!key) { pkg_registry_free(r); return NULL; }
            j_skip_ws(&j);
            if (j.p >= j.end || *j.p != ':') { free(key); pkg_registry_free(r); pkg_error_set(err, PKG_ERR_MANIFEST, 0, "registry JSON: expected ':' after key"); return NULL; }
            j.p++;
            j_skip_ws(&j);
            char *val = j_parse_string(&j, err);
            if (!val) { free(key); pkg_registry_free(r); return NULL; }
            bool ok = registry_add(r, key, val, err);
            free(key); free(val);
            if (!ok) { pkg_registry_free(r); return NULL; }
            j_skip_ws(&j);
            if (j.p < j.end && *j.p == ',') { j.p++; continue; }
            if (j.p < j.end && *j.p == '}') { j.p++; break; }
            pkg_registry_free(r);
            pkg_error_set(err, PKG_ERR_MANIFEST, 0, "registry JSON: expected ',' or '}' in object");
            return NULL;
        }
    } else {
        pkg_registry_free(r);
        pkg_error_set(err, PKG_ERR_MANIFEST, 0,
                      "registry JSON: top level must be an array or object");
        return NULL;
    }

trailing:
    j_skip_ws(&j);
    if (j.p != j.end) {
        pkg_registry_free(r);
        pkg_error_set(err, PKG_ERR_MANIFEST, 0, "registry JSON: trailing data after top-level value");
        return NULL;
    }
    return r;
}

const PkgRegistryEntry *pkg_registry_find(const PkgRegistry *r,
                                          const char *want_owner,
                                          const char *want_repo) {
    if (!r) return NULL;
    for (size_t i = 0; i < r->count; i++) {
        const PkgRegistryEntry *e = &r->entries[i];
        if (want_owner) {
            if (strcmp(e->owner, want_owner) == 0 && strcmp(e->repo, want_repo) == 0)
                return e;
        } else if (want_repo && e->alias) {
            if (strcmp(e->alias, want_repo) == 0) return e;
        }
    }
    return NULL;
}

long pkg_packages_list_parse(const char *text, char ***out_urls, PkgError *err) {
    *out_urls = NULL;
    if (!text) return 0;
    char **urls = NULL; size_t n = 0, cap = 0;
    const char *p = text;
    int line = 0;
    while (*p) {
        line++;
        const char *nl = strchr(p, '\n');
        size_t linelen = nl ? (size_t)(nl - p) : strlen(p);
        /* trim leading/trailing whitespace (incl. a trailing '\r') */
        const char *s = p;
        const char *e = p + linelen;
        while (s < e && (*s == ' ' || *s == '\t' || *s == '\r')) s++;
        while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r')) e--;
        size_t slen = (size_t)(e - s);
        if (slen > 0 && *s != '#') {
            /* validate: https:// only, no control bytes/whitespace inside */
            bool bad = false;
            for (const char *q = s; q < e; q++) {
                unsigned char c = (unsigned char)*q;
                if (c <= 0x20 || c == 0x7f) { bad = true; break; }
            }
            if (bad || slen < 8 || strncmp(s, "https://", 8) != 0) {
                for (size_t i = 0; i < n; i++) free(urls[i]);
                free(urls);
                pkg_error_set(err, PKG_ERR_MANIFEST, line,
                              "packages.list: each entry must be an https:// URL");
                return -1;
            }
            if (n >= PKG_PKGLIST_MAX_URLS) {
                for (size_t i = 0; i < n; i++) free(urls[i]);
                free(urls);
                pkg_error_set(err, PKG_ERR_MANIFEST, line, "packages.list: too many registries");
                return -1;
            }
            if (n == cap) { cap = cap ? cap * 2 : 8; urls = myon_xrealloc(urls, cap * sizeof(char*)); }
            urls[n++] = myon_strndup(s, slen);
        }
        if (!nl) break;
        p = nl + 1;
    }
    *out_urls = urls;
    return (long)n;
}

/* ================================================================== */
/* strict TOML-subset scanner                                         */
/* ================================================================== */
/*
 * The manifests and the lockfile all use a tiny, strict subset of TOML
 * (spec §3.1): integer scalars, quoted-string scalars, `[section]` headers,
 * `[[array-of-tables]]` headers (lockfile only), bare and dotted keys, one
 * assignment per line, '#' line comments, and blank lines.  Anything else is a
 * hard error with a line number.  We deliberately do NOT implement general
 * TOML (no multiline strings, no inline tables, no arrays-of-values except the
 * lockfile's simple string form we special-case, no datetimes).
 *
 * The scanner is line-oriented: it yields one logical line at a time with its
 * 1-based line number, trimming surrounding whitespace and stripping trailing
 * comments that are outside a string literal.
 */

typedef enum {
    TL_EOF = 0,
    TL_BLANK,        /* empty / comment-only line          */
    TL_SECTION,      /* [name]                             */
    TL_ARRAY_TABLE,  /* [[name]]                           */
    TL_ASSIGN        /* key = value                        */
} TLKind;

typedef struct {
    const char *cur;   /* cursor into the buffer            */
    int         line;  /* 1-based line number of `cur`      */
} TLScanner;

static void tl_init(TLScanner *sc, const char *text) {
    sc->cur = text;
    sc->line = 1;
}

/* Advance the cursor past exactly one physical line, bumping sc->line. */
static void tl_advance_line(TLScanner *sc, const char *nl) {
    if (nl) { sc->cur = nl + 1; sc->line++; }
    else    { sc->cur += strlen(sc->cur); }
}

/* True for TOML/whitespace we skip. */
static bool tl_is_space(char c) { return c == ' ' || c == '\t' || c == '\r'; }

/*
 * A parsed logical line.  `key`/`val` (for TL_ASSIGN) and `name` (for section
 * headers) are heap strings owned by the caller; free with tl_line_free.
 */
typedef struct {
    TLKind kind;
    int    line;
    char  *name;   /* section / array-table name           */
    char  *key;    /* assignment key                        */
    char  *val;    /* assignment raw value (unquoted for strings) */
    bool   val_is_string; /* true if value came from a quoted string */
} TLLine;

static void tl_line_free(TLLine *ln) {
    free(ln->name); ln->name = NULL;
    free(ln->key);  ln->key = NULL;
    free(ln->val);  ln->val = NULL;
}

/* Parse a double-quoted string starting at *pp (which points at the opening
 * quote).  Supports \" \\ \n \t \r \0 and \uXXXX-free basic escapes.  On
 * success returns the decoded heap string and advances *pp past the closing
 * quote.  On error returns NULL. */
static char *tl_parse_string(const char **pp, PkgError *err, int line) {
    const char *p = *pp;
    if (*p != '"') { pkg_error_set(err, PKG_ERR_MANIFEST, line, "expected string"); return NULL; }
    p++;
    StrBuf b; sb_init(&b);
    while (*p && *p != '"') {
        char c = *p;
        if (c == '\\') {
            p++;
            char e = *p;
            char out2[2] = {0,0};
            switch (e) {
                case '"':  out2[0] = '"';  break;
                case '\\': out2[0] = '\\'; break;
                case 'n':  out2[0] = '\n'; break;
                case 't':  out2[0] = '\t'; break;
                case 'r':  out2[0] = '\r'; break;
                default:
                    free(sb_take(&b));
                    pkg_error_set(err, PKG_ERR_MANIFEST, line,
                                  "unsupported string escape '\\%c'", e ? e : '0');
                    return NULL;
            }
            sb_append(&b, out2);
            p++;
            continue;
        }
        if ((unsigned char)c < 0x20) {
            free(sb_take(&b));
            pkg_error_set(err, PKG_ERR_MANIFEST, line,
                          "control character in string literal");
            return NULL;
        }
        char out2[2] = {c, 0};
        sb_append(&b, out2);
        p++;
    }
    if (*p != '"') {
        free(sb_take(&b));
        pkg_error_set(err, PKG_ERR_MANIFEST, line, "unterminated string literal");
        return NULL;
    }
    p++; /* past closing quote */
    *pp = p;
    return sb_take(&b);
}

/* A bare key/section-name char: lowercase/upper letters, digits, '-', '_', '.'. */
static bool tl_bare_char(char c) {
    unsigned char u = (unsigned char)c;
    return (u >= 'a' && u <= 'z') || (u >= 'A' && u <= 'Z') ||
           (u >= '0' && u <= '9') || u == '-' || u == '_' || u == '.';
}

/*
 * Read the next logical line.  Returns false only on a syntax error (with *err
 * set); at end of input it returns true with out->kind == TL_EOF.  Blank and
 * comment-only lines are returned as TL_BLANK so the caller can simply skip
 * them.
 */
static bool tl_next(TLScanner *sc, TLLine *out, PkgError *err) {
    memset(out, 0, sizeof(*out));
    if (*sc->cur == '\0') { out->kind = TL_EOF; return true; }

    int line = sc->line;
    out->line = line;
    const char *p = sc->cur;
    const char *nl = strchr(p, '\n');

    /* skip leading whitespace */
    while (tl_is_space(*p) && *p != '\n') p++;

    /* blank or comment line */
    if (*p == '\0' || *p == '\n' || *p == '#') {
        out->kind = TL_BLANK;
        tl_advance_line(sc, nl);
        return true;
    }

    /* section / array-of-tables header */
    if (*p == '[') {
        bool array_table = false;
        p++;
        if (*p == '[') { array_table = true; p++; }
        const char *name_start = p;
        while (tl_bare_char(*p)) p++;
        size_t nlen = (size_t)(p - name_start);
        if (nlen == 0) {
            pkg_error_set(err, PKG_ERR_MANIFEST, line, "empty section name");
            return false;
        }
        if (array_table) {
            if (p[0] != ']' || p[1] != ']') {
                pkg_error_set(err, PKG_ERR_MANIFEST, line, "expected ']]'");
                return false;
            }
            p += 2;
        } else {
            if (p[0] != ']') {
                pkg_error_set(err, PKG_ERR_MANIFEST, line, "expected ']'");
                return false;
            }
            p += 1;
        }
        /* only whitespace / comment may follow */
        while (tl_is_space(*p)) p++;
        if (*p != '\0' && *p != '\n' && *p != '#') {
            pkg_error_set(err, PKG_ERR_MANIFEST, line,
                          "unexpected text after section header");
            return false;
        }
        out->kind = array_table ? TL_ARRAY_TABLE : TL_SECTION;
        out->name = myon_strndup(name_start, nlen);
        tl_advance_line(sc, nl);
        return true;
    }

    /* key = value */
    const char *key_start = p;
    while (tl_bare_char(*p)) p++;
    size_t klen = (size_t)(p - key_start);
    if (klen == 0) {
        pkg_error_set(err, PKG_ERR_MANIFEST, line, "expected a key");
        return false;
    }
    out->key = myon_strndup(key_start, klen);
    while (tl_is_space(*p)) p++;
    if (*p != '=') {
        free(out->key); out->key = NULL;
        pkg_error_set(err, PKG_ERR_MANIFEST, line, "expected '=' after key '%.*s'",
                      (int)klen, key_start);
        return false;
    }
    p++;
    while (tl_is_space(*p)) p++;

    if (*p == '"') {
        char *s = tl_parse_string(&p, err, line);
        if (!s) { free(out->key); out->key = NULL; return false; }
        out->val = s;
        out->val_is_string = true;
    } else {
        /* bare value: an integer scalar in this subset */
        const char *v = p;
        while (*p && *p != '\n' && *p != '#' && !tl_is_space(*p)) p++;
        size_t vlen = (size_t)(p - v);
        if (vlen == 0) {
            free(out->key); out->key = NULL;
            pkg_error_set(err, PKG_ERR_MANIFEST, line, "missing value for key '%s'",
                          out->key ? out->key : "");
            return false;
        }
        out->val = myon_strndup(v, vlen);
        out->val_is_string = false;
    }

    /* trailing whitespace / comment only */
    while (tl_is_space(*p)) p++;
    if (*p != '\0' && *p != '\n' && *p != '#') {
        free(out->key); out->key = NULL;
        free(out->val); out->val = NULL;
        pkg_error_set(err, PKG_ERR_MANIFEST, line,
                      "unexpected text after value");
        return false;
    }
    out->kind = TL_ASSIGN;
    tl_advance_line(sc, nl);
    return true;
}

/* Parse an integer scalar (used only for `format`). */
static bool tl_val_as_int(const TLLine *ln, long *out, PkgError *err) {
    if (ln->val_is_string) {
        pkg_error_set(err, PKG_ERR_MANIFEST, ln->line,
                      "key '%s' expects an integer, got a string", ln->key);
        return false;
    }
    const char *s = ln->val;
    char *end = NULL;
    long v = strtol(s, &end, 10);
    if (end == s || (end && *end != '\0')) {
        pkg_error_set(err, PKG_ERR_MANIFEST, ln->line,
                      "key '%s' expects an integer, got '%s'", ln->key, ln->val);
        return false;
    }
    *out = v;
    return true;
}

/* ================================================================== */
/* file reader                                                         */
/* ================================================================== */

static char *pkg_read_file(const char *path, PkgError *err) {
    FILE *f = fopen(path, "rb");
    if (!f) {
        pkg_error_set(err, PKG_ERR_IO, 0, "cannot open '%s'", path);
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); pkg_error_set(err, PKG_ERR_IO, 0, "cannot read '%s'", path); return NULL; }
    long size = ftell(f);
    if (size < 0) { fclose(f); pkg_error_set(err, PKG_ERR_IO, 0, "cannot read '%s'", path); return NULL; }
    rewind(f);
    char *buf = myon_xmalloc((size_t)size + 1);
    size_t n = fread(buf, 1, (size_t)size, f);
    buf[n] = '\0';
    fclose(f);
    /* Reject embedded NULs: this is a text format. */
    if (strlen(buf) != n) {
        free(buf);
        pkg_error_set(err, PKG_ERR_MANIFEST, 0, "'%s' contains NUL bytes", path);
        return NULL;
    }
    return buf;
}

/* ================================================================== */
/* dependency-list helpers (shared)                                   */
/* ================================================================== */

/* Case-sensitive strcmp wrapper for qsort of names. */
static int cmp_dep_by_name(const void *a, const void *b) {
    const PkgDependency *da = (const PkgDependency *)a;
    const PkgDependency *db = (const PkgDependency *)b;
    return strcmp(da->name, db->name);
}

static void deps_free(PkgDependency *deps, size_t n) {
    for (size_t i = 0; i < n; i++) {
        free(deps[i].name);
        pkg_source_reset(&deps[i].source);
    }
    free(deps);
}

/*
 * Append a dependency to a growable array, taking ownership of `name`
 * (heap) and deep-copying `src`.  Rejects a duplicate name (spec §3.1:
 * duplicate dependency key).  Returns false + *err on duplicate.
 */
static bool deps_add(PkgDependency **deps, size_t *count, size_t *cap,
                     char *name, const PkgSource *src, int line, PkgError *err) {
    for (size_t i = 0; i < *count; i++) {
        if (strcmp((*deps)[i].name, name) == 0) {
            pkg_error_set(err, PKG_ERR_MANIFEST, line,
                          "duplicate dependency '%s'", name);
            free(name);
            return false;
        }
    }
    if (*count == *cap) {
        *cap = *cap ? *cap * 2 : 4;
        *deps = myon_xrealloc(*deps, *cap * sizeof(**deps));
    }
    PkgDependency *d = &(*deps)[(*count)++];
    d->name = name;
    pkg_source_init(&d->source);
    source_copy(&d->source, src);
    return true;
}

static void deps_sort(PkgDependency *deps, size_t n) {
    if (n > 1) qsort(deps, n, sizeof(*deps), cmp_dep_by_name);
}

/* ================================================================== */
/* root project manifest (myon.toml)                                   */
/* ================================================================== */

PkgManifest *pkg_manifest_new(void) {
    PkgManifest *m = myon_xmalloc(sizeof(*m));
    m->project_name = NULL;
    m->project_version = NULL;
    m->deps = NULL;
    m->dep_count = 0;
    return m;
}

void pkg_manifest_free(PkgManifest *m) {
    if (!m) return;
    free(m->project_name);
    free(m->project_version);
    deps_free(m->deps, m->dep_count);
    free(m);
}

/* Section state for the tiny state machine. */
typedef enum { SEC_NONE, SEC_TOP, SEC_PROJECT, SEC_PACKAGE, SEC_DEPS } SecKind;

PkgManifest *pkg_manifest_parse(const char *text, PkgError *err) {
    PkgManifest *m = pkg_manifest_new();
    TLScanner sc; tl_init(&sc, text);

    bool have_format = false;
    bool seen_project = false, seen_deps = false;
    SecKind sec = SEC_TOP; /* before any header: only `format` allowed */

    size_t dep_cap = 0;

    for (;;) {
        TLLine ln;
        if (!tl_next(&sc, &ln, err)) { pkg_manifest_free(m); return NULL; }
        if (ln.kind == TL_EOF) break;
        if (ln.kind == TL_BLANK) { tl_line_free(&ln); continue; }

        if (ln.kind == TL_ARRAY_TABLE) {
            pkg_error_set(err, PKG_ERR_MANIFEST, ln.line,
                          "array-of-tables '[[%s]]' is not allowed in myon.toml",
                          ln.name);
            tl_line_free(&ln); pkg_manifest_free(m); return NULL;
        }
        if (ln.kind == TL_SECTION) {
            if (strcmp(ln.name, "project") == 0) {
                if (seen_project) {
                    pkg_error_set(err, PKG_ERR_MANIFEST, ln.line,
                                  "duplicate [project] section");
                    tl_line_free(&ln); pkg_manifest_free(m); return NULL;
                }
                seen_project = true; sec = SEC_PROJECT;
            } else if (strcmp(ln.name, "dependencies") == 0) {
                if (seen_deps) {
                    pkg_error_set(err, PKG_ERR_MANIFEST, ln.line,
                                  "duplicate [dependencies] section");
                    tl_line_free(&ln); pkg_manifest_free(m); return NULL;
                }
                seen_deps = true; sec = SEC_DEPS;
            } else {
                pkg_error_set(err, PKG_ERR_MANIFEST, ln.line,
                              "unknown section '[%s]'", ln.name);
                tl_line_free(&ln); pkg_manifest_free(m); return NULL;
            }
            tl_line_free(&ln);
            continue;
        }

        /* TL_ASSIGN */
        if (sec == SEC_TOP) {
            if (strcmp(ln.key, "format") == 0) {
                if (have_format) {
                    pkg_error_set(err, PKG_ERR_MANIFEST, ln.line, "duplicate key 'format'");
                    tl_line_free(&ln); pkg_manifest_free(m); return NULL;
                }
                long v;
                if (!tl_val_as_int(&ln, &v, err)) { tl_line_free(&ln); pkg_manifest_free(m); return NULL; }
                if (v != PKG_FORMAT_VERSION) {
                    pkg_error_set(err, PKG_ERR_MANIFEST, ln.line,
                                  "unsupported format = %ld (expected %d)", v, PKG_FORMAT_VERSION);
                    tl_line_free(&ln); pkg_manifest_free(m); return NULL;
                }
                have_format = true;
            } else {
                pkg_error_set(err, PKG_ERR_MANIFEST, ln.line,
                              "unknown top-level key '%s' (expected 'format')", ln.key);
                tl_line_free(&ln); pkg_manifest_free(m); return NULL;
            }
        } else if (sec == SEC_PROJECT) {
            if (strcmp(ln.key, "name") == 0) {
                if (m->project_name) { pkg_error_set(err, PKG_ERR_MANIFEST, ln.line, "duplicate key 'name'"); tl_line_free(&ln); pkg_manifest_free(m); return NULL; }
                if (!ln.val_is_string) { pkg_error_set(err, PKG_ERR_MANIFEST, ln.line, "[project].name must be a string"); tl_line_free(&ln); pkg_manifest_free(m); return NULL; }
                m->project_name = xstrdup0(ln.val);
            } else if (strcmp(ln.key, "version") == 0) {
                if (m->project_version) { pkg_error_set(err, PKG_ERR_MANIFEST, ln.line, "duplicate key 'version'"); tl_line_free(&ln); pkg_manifest_free(m); return NULL; }
                if (!ln.val_is_string) { pkg_error_set(err, PKG_ERR_MANIFEST, ln.line, "[project].version must be a string"); tl_line_free(&ln); pkg_manifest_free(m); return NULL; }
                m->project_version = xstrdup0(ln.val);
            } else {
                pkg_error_set(err, PKG_ERR_MANIFEST, ln.line,
                              "unknown key '%s' in [project]", ln.key);
                tl_line_free(&ln); pkg_manifest_free(m); return NULL;
            }
        } else if (sec == SEC_DEPS) {
            /* key is a package name; value is a github source string */
            if (!pkg_validate_package_name(ln.key, ln.line, err)) { tl_line_free(&ln); pkg_manifest_free(m); return NULL; }
            if (!ln.val_is_string) { pkg_error_set(err, PKG_ERR_MANIFEST, ln.line, "dependency '%s' value must be a string source", ln.key); tl_line_free(&ln); pkg_manifest_free(m); return NULL; }
            PkgSource src;
            if (!pkg_source_parse(ln.val, ln.line, &src, err)) { tl_line_free(&ln); pkg_manifest_free(m); return NULL; }
            char *keyname = xstrdup0(ln.key);
            if (!deps_add(&m->deps, &m->dep_count, &dep_cap, keyname, &src, ln.line, err)) {
                pkg_source_reset(&src); tl_line_free(&ln); pkg_manifest_free(m); return NULL;
            }
            pkg_source_reset(&src);
        }
        tl_line_free(&ln);
    }

    if (!have_format) {
        pkg_error_set(err, PKG_ERR_MANIFEST, 0, "missing required 'format = 1'");
        pkg_manifest_free(m); return NULL;
    }
    if (!seen_project || !m->project_name) {
        pkg_error_set(err, PKG_ERR_MANIFEST, 0, "missing required [project].name");
        pkg_manifest_free(m); return NULL;
    }
    if (!m->project_version) {
        pkg_error_set(err, PKG_ERR_MANIFEST, 0, "missing required [project].version");
        pkg_manifest_free(m); return NULL;
    }
    deps_sort(m->deps, m->dep_count);
    return m;
}

PkgManifest *pkg_manifest_parse_file(const char *path, PkgError *err) {
    char *text = pkg_read_file(path, err);
    if (!text) return NULL;
    PkgManifest *m = pkg_manifest_parse(text, err);
    free(text);
    return m;
}

const PkgDependency *pkg_manifest_find_dep(const PkgManifest *m, const char *name) {
    for (size_t i = 0; i < m->dep_count; i++)
        if (strcmp(m->deps[i].name, name) == 0) return &m->deps[i];
    return NULL;
}

bool pkg_manifest_upsert_dep(PkgManifest *m, const char *name,
                             const PkgSource *source, PkgError *err) {
    if (!pkg_validate_package_name(name, 0, err)) return false;
    for (size_t i = 0; i < m->dep_count; i++) {
        if (strcmp(m->deps[i].name, name) == 0) {
            pkg_source_reset(&m->deps[i].source);
            source_copy(&m->deps[i].source, source);
            return true;
        }
    }
    size_t cap = m->dep_count; /* exact-grow */
    m->deps = myon_xrealloc(m->deps, (m->dep_count + 1) * sizeof(*m->deps));
    (void)cap;
    PkgDependency *d = &m->deps[m->dep_count++];
    d->name = xstrdup0(name);
    pkg_source_init(&d->source);
    source_copy(&d->source, source);
    deps_sort(m->deps, m->dep_count);
    return true;
}

char *pkg_manifest_render(const PkgManifest *m) {
    StrBuf b; sb_init(&b);
    sb_append(&b, "format = 1\n\n[project]\nname = \"");
    sb_append(&b, m->project_name ? m->project_name : "");
    sb_append(&b, "\"\nversion = \"");
    sb_append(&b, m->project_version ? m->project_version : "");
    sb_append(&b, "\"\n");
    if (m->dep_count > 0) {
        sb_append(&b, "\n[dependencies]\n");
        for (size_t i = 0; i < m->dep_count; i++) {
            char *src = pkg_source_canonical_no_sha(&m->deps[i].source);
            sb_append(&b, m->deps[i].name);
            sb_append(&b, " = \"");
            sb_append(&b, src);
            sb_append(&b, "@");
            sb_append(&b, m->deps[i].source.sha);
            sb_append(&b, "\"\n");
            free(src);
        }
    }
    return sb_take(&b);
}

/* ================================================================== */
/* package manifest (package.myon)                                     */
/* ================================================================== */

PkgPackageManifest *pkg_package_manifest_new(void) {
    PkgPackageManifest *m = myon_xmalloc(sizeof(*m));
    m->name = NULL;
    m->version = NULL;
    m->module = NULL;
    m->deps = NULL;
    m->dep_count = 0;
    return m;
}

void pkg_package_manifest_free(PkgPackageManifest *m) {
    if (!m) return;
    free(m->name);
    free(m->version);
    free(m->module);
    deps_free(m->deps, m->dep_count);
    free(m);
}

PkgPackageManifest *pkg_package_manifest_parse(const char *text, PkgError *err) {
    PkgPackageManifest *m = pkg_package_manifest_new();
    TLScanner sc; tl_init(&sc, text);

    bool have_format = false, seen_package = false, seen_deps = false;
    SecKind sec = SEC_TOP;
    size_t dep_cap = 0;

    for (;;) {
        TLLine ln;
        if (!tl_next(&sc, &ln, err)) { pkg_package_manifest_free(m); return NULL; }
        if (ln.kind == TL_EOF) break;
        if (ln.kind == TL_BLANK) { tl_line_free(&ln); continue; }

        if (ln.kind == TL_ARRAY_TABLE) {
            pkg_error_set(err, PKG_ERR_MANIFEST, ln.line,
                          "array-of-tables '[[%s]]' is not allowed in package.myon", ln.name);
            tl_line_free(&ln); pkg_package_manifest_free(m); return NULL;
        }
        if (ln.kind == TL_SECTION) {
            if (strcmp(ln.name, "package") == 0) {
                if (seen_package) { pkg_error_set(err, PKG_ERR_MANIFEST, ln.line, "duplicate [package] section"); tl_line_free(&ln); pkg_package_manifest_free(m); return NULL; }
                seen_package = true; sec = SEC_PACKAGE;
            } else if (strcmp(ln.name, "dependencies") == 0) {
                if (seen_deps) { pkg_error_set(err, PKG_ERR_MANIFEST, ln.line, "duplicate [dependencies] section"); tl_line_free(&ln); pkg_package_manifest_free(m); return NULL; }
                seen_deps = true; sec = SEC_DEPS;
            } else {
                pkg_error_set(err, PKG_ERR_MANIFEST, ln.line, "unknown section '[%s]'", ln.name);
                tl_line_free(&ln); pkg_package_manifest_free(m); return NULL;
            }
            tl_line_free(&ln);
            continue;
        }

        /* TL_ASSIGN */
        if (sec == SEC_TOP) {
            if (strcmp(ln.key, "format") == 0) {
                if (have_format) { pkg_error_set(err, PKG_ERR_MANIFEST, ln.line, "duplicate key 'format'"); tl_line_free(&ln); pkg_package_manifest_free(m); return NULL; }
                long v;
                if (!tl_val_as_int(&ln, &v, err)) { tl_line_free(&ln); pkg_package_manifest_free(m); return NULL; }
                if (v != PKG_FORMAT_VERSION) { pkg_error_set(err, PKG_ERR_MANIFEST, ln.line, "unsupported format = %ld (expected %d)", v, PKG_FORMAT_VERSION); tl_line_free(&ln); pkg_package_manifest_free(m); return NULL; }
                have_format = true;
            } else {
                pkg_error_set(err, PKG_ERR_MANIFEST, ln.line, "unknown top-level key '%s' (expected 'format')", ln.key);
                tl_line_free(&ln); pkg_package_manifest_free(m); return NULL;
            }
        } else if (sec == SEC_PACKAGE) {
            if (strcmp(ln.key, "name") == 0) {
                if (m->name) { pkg_error_set(err, PKG_ERR_MANIFEST, ln.line, "duplicate key 'name'"); tl_line_free(&ln); pkg_package_manifest_free(m); return NULL; }
                if (!ln.val_is_string) { pkg_error_set(err, PKG_ERR_MANIFEST, ln.line, "[package].name must be a string"); tl_line_free(&ln); pkg_package_manifest_free(m); return NULL; }
                if (!pkg_validate_package_name(ln.val, ln.line, err)) { tl_line_free(&ln); pkg_package_manifest_free(m); return NULL; }
                m->name = xstrdup0(ln.val);
            } else if (strcmp(ln.key, "version") == 0) {
                if (m->version) { pkg_error_set(err, PKG_ERR_MANIFEST, ln.line, "duplicate key 'version'"); tl_line_free(&ln); pkg_package_manifest_free(m); return NULL; }
                if (!ln.val_is_string) { pkg_error_set(err, PKG_ERR_MANIFEST, ln.line, "[package].version must be a string"); tl_line_free(&ln); pkg_package_manifest_free(m); return NULL; }
                m->version = xstrdup0(ln.val);
            } else if (strcmp(ln.key, "module") == 0) {
                if (m->module) { pkg_error_set(err, PKG_ERR_MANIFEST, ln.line, "duplicate key 'module'"); tl_line_free(&ln); pkg_package_manifest_free(m); return NULL; }
                if (!ln.val_is_string) { pkg_error_set(err, PKG_ERR_MANIFEST, ln.line, "[package].module must be a string"); tl_line_free(&ln); pkg_package_manifest_free(m); return NULL; }
                if (!pkg_validate_module_name(ln.val, ln.line, err)) { tl_line_free(&ln); pkg_package_manifest_free(m); return NULL; }
                m->module = xstrdup0(ln.val);
            } else {
                pkg_error_set(err, PKG_ERR_MANIFEST, ln.line, "unknown key '%s' in [package]", ln.key);
                tl_line_free(&ln); pkg_package_manifest_free(m); return NULL;
            }
        } else if (sec == SEC_DEPS) {
            if (!pkg_validate_package_name(ln.key, ln.line, err)) { tl_line_free(&ln); pkg_package_manifest_free(m); return NULL; }
            if (!ln.val_is_string) { pkg_error_set(err, PKG_ERR_MANIFEST, ln.line, "dependency '%s' value must be a string source", ln.key); tl_line_free(&ln); pkg_package_manifest_free(m); return NULL; }
            PkgSource src;
            if (!pkg_source_parse(ln.val, ln.line, &src, err)) { tl_line_free(&ln); pkg_package_manifest_free(m); return NULL; }
            char *keyname = xstrdup0(ln.key);
            if (!deps_add(&m->deps, &m->dep_count, &dep_cap, keyname, &src, ln.line, err)) {
                pkg_source_reset(&src); tl_line_free(&ln); pkg_package_manifest_free(m); return NULL;
            }
            pkg_source_reset(&src);
        }
        tl_line_free(&ln);
    }

    if (!have_format) { pkg_error_set(err, PKG_ERR_MANIFEST, 0, "missing required 'format = 1'"); pkg_package_manifest_free(m); return NULL; }
    if (!m->name)    { pkg_error_set(err, PKG_ERR_MANIFEST, 0, "missing required [package].name"); pkg_package_manifest_free(m); return NULL; }
    if (!m->version) { pkg_error_set(err, PKG_ERR_MANIFEST, 0, "missing required [package].version"); pkg_package_manifest_free(m); return NULL; }
    if (!m->module)  { pkg_error_set(err, PKG_ERR_MANIFEST, 0, "missing required [package].module"); pkg_package_manifest_free(m); return NULL; }
    deps_sort(m->deps, m->dep_count);
    return m;
}

PkgPackageManifest *pkg_package_manifest_parse_file(const char *path, PkgError *err) {
    char *text = pkg_read_file(path, err);
    if (!text) return NULL;
    PkgPackageManifest *m = pkg_package_manifest_parse(text, err);
    free(text);
    return m;
}

char *pkg_package_manifest_render(const PkgPackageManifest *m) {
    StrBuf b; sb_init(&b);
    sb_append(&b, "format = 1\n\n[package]\nname = \"");
    sb_append(&b, m->name ? m->name : "");
    sb_append(&b, "\"\nversion = \"");
    sb_append(&b, m->version ? m->version : "");
    sb_append(&b, "\"\nmodule = \"");
    sb_append(&b, m->module ? m->module : "");
    sb_append(&b, "\"\n");
    if (m->dep_count > 0) {
        sb_append(&b, "\n[dependencies]\n");
        for (size_t i = 0; i < m->dep_count; i++) {
            char *src = pkg_source_canonical_no_sha(&m->deps[i].source);
            sb_append(&b, m->deps[i].name);
            sb_append(&b, " = \"");
            sb_append(&b, src);
            sb_append(&b, "@");
            sb_append(&b, m->deps[i].source.sha);
            sb_append(&b, "\"\n");
            free(src);
        }
    }
    return sb_take(&b);
}

/* ================================================================== */
/* lockfile (myon.lock)                                                */
/* ================================================================== */

PkgLock *pkg_lock_new(void) {
    PkgLock *l = myon_xmalloc(sizeof(*l));
    l->entries = NULL;
    l->count = 0;
    return l;
}

static void lock_entry_reset(PkgLockEntry *e) {
    free(e->name);
    free(e->version);
    free(e->module);
    pkg_source_reset(&e->source);
    for (size_t i = 0; i < e->dep_count; i++) free(e->deps[i]);
    free(e->deps);
    memset(e, 0, sizeof(*e));
}

void pkg_lock_free(PkgLock *l) {
    if (!l) return;
    for (size_t i = 0; i < l->count; i++) lock_entry_reset(&l->entries[i]);
    free(l->entries);
    free(l);
}

static int cmp_str(const void *a, const void *b) {
    const char *const *pa = a;
    const char *const *pb = b;
    return strcmp(*pa, *pb);
}

/*
 * Parse a comma-separated dependency name list (the lockfile `dependencies`
 * string).  A single name and an empty string are both valid.  Names are
 * validated, deduplicated and sorted.  Returns true on success.
 */
static bool parse_dep_names(const char *s, int line, char ***out, size_t *out_n, PkgError *err) {
    *out = NULL; *out_n = 0;
    if (!s || s[0] == '\0') return true;
    char **names = NULL;
    size_t n = 0, cap = 0;
    const char *p = s;
    while (*p) {
        while (*p == ' ' || *p == ',') p++;
        if (!*p) break;
        const char *start = p;
        while (*p && *p != ',' ) p++;
        const char *end = p;
        while (end > start && end[-1] == ' ') end--;
        size_t len = (size_t)(end - start);
        char *name = myon_strndup(start, len);
        if (!pkg_validate_package_name(name, line, err)) { free(name); goto fail; }
        /* dedup */
        bool dup = false;
        for (size_t i = 0; i < n; i++) if (strcmp(names[i], name) == 0) { dup = true; break; }
        if (dup) { free(name); continue; }
        if (n == cap) { cap = cap ? cap * 2 : 4; names = myon_xrealloc(names, cap * sizeof(*names)); }
        names[n++] = name;
    }
    if (n > 1) qsort(names, n, sizeof(*names), cmp_str);
    *out = names; *out_n = n;
    return true;
fail:
    for (size_t i = 0; i < n; i++) free(names[i]);
    free(names);
    return false;
}

const PkgLockEntry *pkg_lock_find(const PkgLock *l, const char *name) {
    for (size_t i = 0; i < l->count; i++)
        if (strcmp(l->entries[i].name, name) == 0) return &l->entries[i];
    return NULL;
}

static int cmp_lock_by_name(const void *a, const void *b) {
    const PkgLockEntry *ea = a, *eb = b;
    return strcmp(ea->name, eb->name);
}

/* Deep-copy `src` entry into `dst` (dst assumed zeroed). */
static void lock_entry_copy(PkgLockEntry *dst, const PkgLockEntry *src) {
    dst->name = xstrdup0(src->name);
    dst->version = xstrdup0(src->version);
    dst->module = xstrdup0(src->module);
    pkg_source_init(&dst->source);
    source_copy(&dst->source, &src->source);
    memcpy(dst->sha256, src->sha256, sizeof(dst->sha256));
    dst->dep_count = src->dep_count;
    dst->deps = src->dep_count ? myon_xmalloc(src->dep_count * sizeof(char *)) : NULL;
    for (size_t i = 0; i < src->dep_count; i++) dst->deps[i] = xstrdup0(src->deps[i]);
}

/* True if two entries are byte-identical in every recorded field. */
static bool lock_entry_equal(const PkgLockEntry *a, const PkgLockEntry *b) {
    if (strcmp(a->name, b->name) != 0) return false;
    if (strcmp(a->version ? a->version : "", b->version ? b->version : "") != 0) return false;
    if (strcmp(a->module ? a->module : "", b->module ? b->module : "") != 0) return false;
    if (strcmp(a->source.owner, b->source.owner) != 0) return false;
    if (strcmp(a->source.repo, b->source.repo) != 0) return false;
    if (strcmp(a->source.sha, b->source.sha) != 0) return false;
    if (strcmp(a->sha256, b->sha256) != 0) return false;
    if (a->dep_count != b->dep_count) return false;
    for (size_t i = 0; i < a->dep_count; i++)
        if (strcmp(a->deps[i], b->deps[i]) != 0) return false;
    return true;
}

bool pkg_lock_upsert(PkgLock *l, const PkgLockEntry *entry, PkgError *err) {
    for (size_t i = 0; i < l->count; i++) {
        if (strcmp(l->entries[i].name, entry->name) == 0) {
            if (lock_entry_equal(&l->entries[i], entry)) return true; /* no-op */
            if (strcmp(l->entries[i].source.sha, entry->source.sha) != 0) {
                pkg_error_set(err, PKG_ERR_MANIFEST, 0,
                              "package '%s' locked to conflicting revisions "
                              "(%s vs %s)", entry->name,
                              l->entries[i].source.sha, entry->source.sha);
                return false;
            }
            /* same revision, different metadata: replace in place */
            lock_entry_reset(&l->entries[i]);
            lock_entry_copy(&l->entries[i], entry);
            return true;
        }
    }
    l->entries = myon_xrealloc(l->entries, (l->count + 1) * sizeof(*l->entries));
    memset(&l->entries[l->count], 0, sizeof(l->entries[l->count]));
    lock_entry_copy(&l->entries[l->count], entry);
    l->count++;
    qsort(l->entries, l->count, sizeof(*l->entries), cmp_lock_by_name);
    return true;
}

PkgLock *pkg_lock_parse(const char *text, PkgError *err) {
    PkgLock *l = pkg_lock_new();
    TLScanner sc; tl_init(&sc, text);

    bool have_format = false;
    /* current entry accumulator */
    PkgLockEntry cur;
    memset(&cur, 0, sizeof(cur));
    bool in_entry = false;
    char *pending_deps = NULL;    /* raw dependencies string for current entry */
    char *pending_source = NULL;  /* raw "github:o/r" (no sha)                  */
    char *pending_revision = NULL;

    #define LOCK_FAIL() do { \
        free(pending_deps); free(pending_source); free(pending_revision); \
        if (in_entry) lock_entry_reset(&cur); \
        pkg_lock_free(l); return NULL; \
    } while (0)

    /* Finalise the currently-open [[package]] entry into the lock. */
    #define FINALISE_ENTRY(line) do { \
        if (in_entry) { \
            if (!cur.name)   { pkg_error_set(err, PKG_ERR_MANIFEST, line, "lock package missing 'name'"); LOCK_FAIL(); } \
            if (!pending_source || !pending_revision) { pkg_error_set(err, PKG_ERR_MANIFEST, line, "lock package '%s' missing source/revision", cur.name); LOCK_FAIL(); } \
            if (cur.sha256[0] == '\0') { pkg_error_set(err, PKG_ERR_MANIFEST, line, "lock package '%s' missing sha256", cur.name); LOCK_FAIL(); } \
            /* build the full source string and parse it */ \
            StrBuf sbf; sb_init(&sbf); sb_append(&sbf, pending_source); sb_append(&sbf, "@"); sb_append(&sbf, pending_revision); \
            char *full = sb_take(&sbf); \
            if (!pkg_source_parse(full, line, &cur.source, err)) { free(full); LOCK_FAIL(); } \
            free(full); \
            if (!parse_dep_names(pending_deps, line, &cur.deps, &cur.dep_count, err)) LOCK_FAIL(); \
            if (!pkg_lock_upsert(l, &cur, err)) LOCK_FAIL(); \
            lock_entry_reset(&cur); \
            free(pending_deps); pending_deps = NULL; \
            free(pending_source); pending_source = NULL; \
            free(pending_revision); pending_revision = NULL; \
            in_entry = false; \
        } \
    } while (0)

    for (;;) {
        TLLine ln;
        if (!tl_next(&sc, &ln, err)) { LOCK_FAIL(); }
        if (ln.kind == TL_EOF) { tl_line_free(&ln); break; }
        if (ln.kind == TL_BLANK) { tl_line_free(&ln); continue; }

        if (ln.kind == TL_SECTION) {
            pkg_error_set(err, PKG_ERR_MANIFEST, ln.line,
                          "unexpected section '[%s]' in lockfile "
                          "(expected top-level 'format' and '[[package]]')", ln.name);
            tl_line_free(&ln); LOCK_FAIL();
        }
        if (ln.kind == TL_ARRAY_TABLE) {
            if (strcmp(ln.name, "package") != 0) {
                pkg_error_set(err, PKG_ERR_MANIFEST, ln.line,
                              "unknown array-of-tables '[[%s]]'", ln.name);
                tl_line_free(&ln); LOCK_FAIL();
            }
            FINALISE_ENTRY(ln.line);
            in_entry = true;
            memset(&cur, 0, sizeof(cur));
            tl_line_free(&ln);
            continue;
        }

        /* TL_ASSIGN */
        if (!in_entry) {
            if (strcmp(ln.key, "format") == 0) {
                if (have_format) { pkg_error_set(err, PKG_ERR_MANIFEST, ln.line, "duplicate key 'format'"); tl_line_free(&ln); LOCK_FAIL(); }
                long v; if (!tl_val_as_int(&ln, &v, err)) { tl_line_free(&ln); LOCK_FAIL(); }
                if (v != PKG_FORMAT_VERSION) { pkg_error_set(err, PKG_ERR_MANIFEST, ln.line, "unsupported lockfile format = %ld", v); tl_line_free(&ln); LOCK_FAIL(); }
                have_format = true;
            } else {
                pkg_error_set(err, PKG_ERR_MANIFEST, ln.line, "unexpected key '%s' before any [[package]]", ln.key);
                tl_line_free(&ln); LOCK_FAIL();
            }
            tl_line_free(&ln);
            continue;
        }

        /* keys inside a [[package]] entry */
        if (!ln.val_is_string) { pkg_error_set(err, PKG_ERR_MANIFEST, ln.line, "lock key '%s' must be a string", ln.key); tl_line_free(&ln); LOCK_FAIL(); }
        if (strcmp(ln.key, "name") == 0) {
            if (cur.name) { pkg_error_set(err, PKG_ERR_MANIFEST, ln.line, "duplicate 'name'"); tl_line_free(&ln); LOCK_FAIL(); }
            if (!pkg_validate_package_name(ln.val, ln.line, err)) { tl_line_free(&ln); LOCK_FAIL(); }
            cur.name = xstrdup0(ln.val);
        } else if (strcmp(ln.key, "version") == 0) {
            if (cur.version) { pkg_error_set(err, PKG_ERR_MANIFEST, ln.line, "duplicate 'version'"); tl_line_free(&ln); LOCK_FAIL(); }
            cur.version = xstrdup0(ln.val);
        } else if (strcmp(ln.key, "module") == 0) {
            if (cur.module) { pkg_error_set(err, PKG_ERR_MANIFEST, ln.line, "duplicate 'module'"); tl_line_free(&ln); LOCK_FAIL(); }
            if (!pkg_validate_module_name(ln.val, ln.line, err)) { tl_line_free(&ln); LOCK_FAIL(); }
            cur.module = xstrdup0(ln.val);
        } else if (strcmp(ln.key, "source") == 0) {
            if (pending_source) { pkg_error_set(err, PKG_ERR_MANIFEST, ln.line, "duplicate 'source'"); tl_line_free(&ln); LOCK_FAIL(); }
            if (strncmp(ln.val, "github:", 7) != 0) { pkg_error_set(err, PKG_ERR_MANIFEST, ln.line, "lock 'source' must be github:<owner>/<repo>"); tl_line_free(&ln); LOCK_FAIL(); }
            pending_source = xstrdup0(ln.val);
        } else if (strcmp(ln.key, "revision") == 0) {
            if (pending_revision) { pkg_error_set(err, PKG_ERR_MANIFEST, ln.line, "duplicate 'revision'"); tl_line_free(&ln); LOCK_FAIL(); }
            if (!pkg_is_full_sha(ln.val)) { pkg_error_set(err, PKG_ERR_MANIFEST, ln.line, "lock 'revision' must be a 40-hex commit SHA"); tl_line_free(&ln); LOCK_FAIL(); }
            pending_revision = xstrdup0(ln.val);
        } else if (strcmp(ln.key, "archive") == 0) {
            /* archive URL is derivable from source+revision; accept & ignore
             * the stored value but verify it is a codeload URL prefix. */
            if (strncmp(ln.val, "https://codeload.github.com/", 28) != 0) {
                pkg_error_set(err, PKG_ERR_MANIFEST, ln.line, "lock 'archive' must be a codeload.github.com URL");
                tl_line_free(&ln); LOCK_FAIL();
            }
        } else if (strcmp(ln.key, "sha256") == 0) {
            if (cur.sha256[0]) { pkg_error_set(err, PKG_ERR_MANIFEST, ln.line, "duplicate 'sha256'"); tl_line_free(&ln); LOCK_FAIL(); }
            if (!pkg_is_sha256_hex(ln.val)) { pkg_error_set(err, PKG_ERR_MANIFEST, ln.line, "lock 'sha256' must be 64 lowercase hex chars"); tl_line_free(&ln); LOCK_FAIL(); }
            memcpy(cur.sha256, ln.val, PKG_SHA256_HEX_LEN); cur.sha256[PKG_SHA256_HEX_LEN] = '\0';
        } else if (strcmp(ln.key, "dependencies") == 0) {
            if (pending_deps) { pkg_error_set(err, PKG_ERR_MANIFEST, ln.line, "duplicate 'dependencies'"); tl_line_free(&ln); LOCK_FAIL(); }
            pending_deps = xstrdup0(ln.val);
        } else {
            pkg_error_set(err, PKG_ERR_MANIFEST, ln.line, "unknown lock key '%s'", ln.key);
            tl_line_free(&ln); LOCK_FAIL();
        }
        tl_line_free(&ln);
    }
    FINALISE_ENTRY(0);

    if (!have_format) { pkg_error_set(err, PKG_ERR_MANIFEST, 0, "missing required 'format = 1'"); pkg_lock_free(l); return NULL; }

    #undef FINALISE_ENTRY
    #undef LOCK_FAIL
    return l;
}

PkgLock *pkg_lock_parse_file(const char *path, PkgError *err) {
    char *text = pkg_read_file(path, err);
    if (!text) return NULL;
    PkgLock *l = pkg_lock_parse(text, err);
    free(text);
    return l;
}

char *pkg_lock_render(const PkgLock *l) {
    /* entries are kept sorted by name on every upsert; be defensive. */
    StrBuf b; sb_init(&b);
    sb_append(&b, "format = 1\n");
    for (size_t i = 0; i < l->count; i++) {
        const PkgLockEntry *e = &l->entries[i];
        char *src = pkg_source_canonical_no_sha(&e->source);
        char *url = pkg_source_archive_url(&e->source);
        sb_append(&b, "\n[[package]]\nname = \"");
        sb_append(&b, e->name);
        sb_append(&b, "\"\nversion = \"");
        sb_append(&b, e->version ? e->version : "");
        sb_append(&b, "\"\nmodule = \"");
        sb_append(&b, e->module ? e->module : "");
        sb_append(&b, "\"\nsource = \"");
        sb_append(&b, src);
        sb_append(&b, "\"\nrevision = \"");
        sb_append(&b, e->source.sha);
        sb_append(&b, "\"\narchive = \"");
        sb_append(&b, url);
        sb_append(&b, "\"\nsha256 = \"");
        sb_append(&b, e->sha256);
        sb_append(&b, "\"\ndependencies = \"");
        for (size_t j = 0; j < e->dep_count; j++) {
            if (j) sb_append(&b, ", ");
            sb_append(&b, e->deps[j]);
        }
        sb_append(&b, "\"\n");
        free(src); free(url);
    }
    return sb_take(&b);
}

bool pkg_lock_check_matches_manifest(const PkgLock *l, const PkgManifest *m,
                                     PkgError *err) {
    /* Every root dependency must appear in the lockfile with the same SHA. */
    for (size_t i = 0; i < m->dep_count; i++) {
        const PkgDependency *d = &m->deps[i];
        const PkgLockEntry *e = pkg_lock_find(l, d->name);
        if (!e) {
            pkg_error_set(err, PKG_ERR_MANIFEST, 0,
                          "dependency '%s' is in myon.toml but not in myon.lock "
                          "(run `myon pkg lock`)", d->name);
            return false;
        }
        if (strcmp(e->source.sha, d->source.sha) != 0) {
            pkg_error_set(err, PKG_ERR_MANIFEST, 0,
                          "dependency '%s' revision differs between myon.toml "
                          "and myon.lock (run `myon pkg lock`)", d->name);
            return false;
        }
    }
    return true;
}

/* ================================================================== */
/* CLI                                                                 */
/* ================================================================== */
/*
 * `myon pkg <subcommand> [args]`.
 *
 * Exit-code policy (mirrors the interpreter, spec §5 "既存 exit code 方針"):
 *   0   success
 *   64  usage error            (EX_USAGE)
 *   65  manifest / data error  (EX_DATAERR)
 *   66  input file missing     (EX_NOINPUT)
 *   69  network unavailable    (EX_UNAVAILABLE)  [later phases]
 *   70  integrity error        (EX_SOFTWARE)     [later phases]
 *
 * This lets scripts and users tell usage / manifest / network / integrity
 * failures apart.
 */

static int exit_code_for(PkgErrorCode code) {
    switch (code) {
        case PKG_OK:              return 0;
        case PKG_ERR_USAGE:       return 64;
        case PKG_ERR_MANIFEST:    return 65;
        case PKG_ERR_IO:          return 66;
        case PKG_ERR_NETWORK:     return 69;
        case PKG_ERR_INTEGRITY:   return 70;
        case PKG_ERR_UNSUPPORTED: return 70;
    }
    return 1;
}

/* Print a PkgError to stderr in a consistent "myon pkg: ..." form. */
static void print_pkg_error(const PkgError *err, const char *ctx) {
    if (!err || err->code == PKG_OK) return;
    if (err->line > 0)
        fprintf(stderr, "myon pkg: %s: line %d: %s\n",
                ctx ? ctx : "error", err->line,
                err->message ? err->message : "(unknown error)");
    else
        fprintf(stderr, "myon pkg: %s: %s\n",
                ctx ? ctx : "error",
                err->message ? err->message : "(unknown error)");
}

static void pkg_usage(void) {
    fprintf(stderr,
        "usage: myon pkg <command> [arguments]\n"
        "\n"
        "commands:\n"
        "  install <github-url>   add a dependency from a GitHub URL, resolve it,\n"
        "                         and install it under .myon/packages/\n"
        "  install <owner>/<repo> resolve the shorthand via the registries listed\n"
        "                         in .myon/packages.list, then install it\n"
        "  install                reinstall everything from the existing myon.lock\n"
        "  lock                   resolve dependencies and (re)write myon.lock\n"
        "  verify                 check myon.toml, myon.lock and installed packages\n"
        "  tree                   print the locked dependency graph (no network)\n"
        "\n"
        "notes:\n"
        "  * Installing or importing a package runs ordinary Myon code with full\n"
        "    host privileges. Packages are NOT sandboxed: treat an untrusted\n"
        "    package as arbitrary code.\n"
        "  * Only GitHub public repositories pinned to a full commit SHA are\n"
        "    supported; branches/tags in a URL are resolved to an immutable SHA.\n"
        "  * Ref resolution uses git's smart-HTTP protocol on github.com first and\n"
        "    falls back to the REST API, so it does not hit the low unauthenticated\n"
        "    api.github.com rate limit in normal use.\n"
        "  * `.myon/packages.list` holds one https:// registry-JSON URL per line;\n"
        "    each registry maps \"<owner>/<repo>\" shorthands to GitHub repositories.\n");
}

/*
 * Locate the project root by walking up from the current directory looking for
 * a myon.toml.  Returns a heap path to the directory containing myon.toml, or
 * NULL if none is found (with *err set).  The caller frees the result.
 *
 * NOTE: for the scope of this phase we only need the discovered root for the
 * commands that already work offline (tree/verify's manifest+lock checks).  A
 * chdir-free absolute-root design keeps later phases free to add staging under
 * "<root>/.myon".
 */
static char *find_project_root(PkgError *err) {
    /* Bounded walk upward; avoids an unbounded loop on odd filesystems. */
    char cwd[4096];
    if (!getcwd(cwd, sizeof(cwd))) {
        pkg_error_set(err, PKG_ERR_IO, 0, "cannot determine current directory");
        return NULL;
    }
    char path[4096 + 16];
    /* Walk up by trimming trailing path components from a working copy. */
    char dir[4096];
    snprintf(dir, sizeof(dir), "%s", cwd);
    for (int depth = 0; depth < 256; depth++) {
        snprintf(path, sizeof(path), "%s/myon.toml", dir);
        FILE *f = fopen(path, "rb");
        if (f) { fclose(f); return myon_strdup(dir); }
        /* trim last component */
        char *slash = strrchr(dir, '/');
        if (!slash) break;
        if (slash == dir) { /* reached filesystem root "/" */
            /* try "/myon.toml" once, then stop */
            dir[1] = '\0';
            snprintf(path, sizeof(path), "%smyon.toml", dir);
            FILE *g = fopen(path, "rb");
            if (g) { fclose(g); return myon_strdup(dir); }
            break;
        }
        *slash = '\0';
    }
    pkg_error_set(err, PKG_ERR_USAGE, 0,
                  "no myon.toml found in this directory or any parent "
                  "(run `myon pkg install <github-url>` to start a project)");
    return NULL;
}

/* Build "<root>/<name>" into a fresh heap string. */
static char *join_path(const char *root, const char *name) {
    StrBuf b; sb_init(&b);
    sb_append(&b, root);
    sb_append(&b, "/");
    sb_append(&b, name);
    return sb_take(&b);
}

/* --- pkg tree: fully offline, reads myon.lock only --------------------- */

static void tree_print_node(const PkgLock *l, const char *name, int depth,
                            const char **stack, int stack_n) {
    for (int i = 0; i < depth; i++) fputs("  ", stdout);
    printf("%s", name);
    const PkgLockEntry *e = pkg_lock_find(l, name);
    if (!e) { printf(" (missing from lockfile)\n"); return; }
    printf(" @ %s\n", e->source.sha);

    /* cycle guard */
    for (int i = 0; i < stack_n; i++)
        if (strcmp(stack[i], name) == 0) { return; }

    const char *next_stack[256];
    int n = stack_n;
    if (n < 256) next_stack[n++] = name; else return;
    for (int i = 0; i < stack_n; i++) next_stack[i] = stack[i];
    next_stack[stack_n] = name;

    for (size_t i = 0; i < e->dep_count; i++)
        tree_print_node(l, e->deps[i], depth + 1, next_stack, n);
}

static int cmd_pkg_tree(void) {
    PkgError err; pkg_error_init(&err);
    char *root = find_project_root(&err);
    if (!root) { print_pkg_error(&err, "tree"); int rc = exit_code_for(err.code); pkg_error_reset(&err); return rc; }

    char *toml = join_path(root, "myon.toml");
    char *lock = join_path(root, "myon.lock");

    PkgManifest *m = pkg_manifest_parse_file(toml, &err);
    if (!m) { print_pkg_error(&err, "tree"); free(root); free(toml); free(lock); int rc = exit_code_for(err.code); pkg_error_reset(&err); return rc; }

    PkgLock *l = pkg_lock_parse_file(lock, &err);
    if (!l) {
        if (err.code == PKG_ERR_IO)
            fprintf(stderr, "myon pkg: tree: no myon.lock found (run `myon pkg lock`)\n");
        else
            print_pkg_error(&err, "tree");
        pkg_manifest_free(m); free(root); free(toml); free(lock);
        int rc = exit_code_for(err.code); pkg_error_reset(&err); return rc;
    }

    printf("%s %s\n", m->project_name, m->project_version);
    const char *stack[1];
    for (size_t i = 0; i < m->dep_count; i++)
        tree_print_node(l, m->deps[i].name, 1, stack, 0);

    pkg_lock_free(l);
    pkg_manifest_free(m);
    free(root); free(toml); free(lock);
    return 0;
}

/* --- pkg verify: offline manifest/lock consistency --------------------- */

static int cmd_pkg_verify(void) {
    PkgError err; pkg_error_init(&err);
    char *root = find_project_root(&err);
    if (!root) { print_pkg_error(&err, "verify"); int rc = exit_code_for(err.code); pkg_error_reset(&err); return rc; }

    char *toml = join_path(root, "myon.toml");
    char *lock = join_path(root, "myon.lock");
    int rc = 0;

    PkgManifest *m = pkg_manifest_parse_file(toml, &err);
    if (!m) { print_pkg_error(&err, "verify"); rc = exit_code_for(err.code); goto done; }

    PkgLock *l = pkg_lock_parse_file(lock, &err);
    if (!l) {
        if (err.code == PKG_ERR_IO)
            fprintf(stderr, "myon pkg: verify: no myon.lock found (run `myon pkg lock`)\n");
        else
            print_pkg_error(&err, "verify");
        rc = exit_code_for(err.code);
        pkg_manifest_free(m);
        goto done;
    }

    if (!pkg_lock_check_matches_manifest(l, m, &err)) {
        print_pkg_error(&err, "verify");
        rc = exit_code_for(err.code);
    } else {
        printf("myon pkg: myon.toml and myon.lock are consistent "
               "(%zu locked package%s)\n", l->count, l->count == 1 ? "" : "s");
        /* NOTE: on-disk installed-package checks (spec §5 `verify`) land with
         * the filesystem/ZIP phase; this reports the manifest/lock layer. */
    }
    pkg_lock_free(l);
    pkg_manifest_free(m);

done:
    free(root); free(toml); free(lock);
    pkg_error_reset(&err);
    return rc;
}

int pkg_cli_main(int argc, char **argv) {
    if (argc < 1) { pkg_usage(); return 64; }
    const char *sub = argv[0];

    if (strcmp(sub, "-h") == 0 || strcmp(sub, "--help") == 0) { pkg_usage(); return 0; }
    if (strcmp(sub, "tree") == 0) {
        if (argc != 1) { fprintf(stderr, "myon pkg: tree takes no arguments\n"); return 64; }
        return cmd_pkg_tree();
    }
    if (strcmp(sub, "verify") == 0) {
        if (argc != 1) { fprintf(stderr, "myon pkg: verify takes no arguments\n"); return 64; }
        return cmd_pkg_verify();
    }
    if (strcmp(sub, "lock") == 0) {
        if (argc != 1) { fprintf(stderr, "myon pkg: lock takes no arguments\n"); return 64; }
        return pkg_ops_lock();
    }
    if (strcmp(sub, "install") == 0) {
        if (argc == 1) return pkg_ops_install_locked();
        if (argc == 2) {
            /* "<owner>/<repo>" shorthand -> resolve via .myon/packages.list;
             * anything else is treated as an explicit GitHub URL. */
            if (pkg_arg_is_shorthand(argv[1]))
                return pkg_ops_install_shorthand(argv[1]);
            return pkg_ops_install_url(argv[1]);
        }
        fprintf(stderr, "myon pkg: install takes at most one <github-url> or <owner>/<repo>\n");
        return 64;
    }

    fprintf(stderr, "myon pkg: unknown command '%s'\n", sub);
    pkg_usage();
    return 64;
}
