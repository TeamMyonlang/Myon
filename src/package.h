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

#ifndef MYON_PACKAGE_H
#define MYON_PACKAGE_H

/*
 * Myon package manager — data model, parsers, and CLI entry point.
 *
 * This header defines the C-native package-manager subsystem (spec §1–§5):
 *
 *   - the strict-subset TOML data model for the project manifest (myon.toml),
 *     the per-package manifest (package.myon) and the lockfile (myon.lock),
 *   - the parsers and the deterministic lockfile writer,
 *   - the identity validators (package name, module namespace, GitHub source),
 *   - the GitHub URL parser (repository identity + optional ref),
 *   - the `myon pkg ...` CLI dispatch entry point.
 *
 * SECURITY NOTE (spec §2.2): installing or importing a Myon package runs
 * ordinary Myon code with full host privileges (file I/O, network, FFI, ...).
 * The package manager does NOT sandbox package code.  Treat importing an
 * untrusted package as equivalent to running arbitrary code.
 *
 * This subsystem is deliberately self-contained: it does not depend on the
 * interpreter, the MVM, or myon.http.  Network fetch, ZIP inspection and
 * interpreter integration are separate translation units wired in by later
 * phases; the CLI commands that need them fail with a clear "not yet wired"
 * diagnostic until those phases land, and never fall back to unsafe behaviour.
 *
 * Ownership convention: every parser returns a heap-allocated struct that the
 * caller frees with the matching *_free().  Diagnostics are written to a
 * caller-provided PkgError (see below); on success err->code == PKG_OK.
 */

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

/* ------------------------------------------------------------------ */
/* Constants                                                           */
/* ------------------------------------------------------------------ */

/* A GitHub commit SHA is a 40-character lowercase hex string (SHA-1). */
#define PKG_SHA_LEN 40
/* A SHA-256 digest, as a lowercase hex string, is 64 characters. */
#define PKG_SHA256_HEX_LEN 64
/* Only manifest format version 1 exists in this initial release. */
#define PKG_FORMAT_VERSION 1

/* ------------------------------------------------------------------ */
/* Error / diagnostics                                                 */
/* ------------------------------------------------------------------ */

/*
 * Error categories.  These map onto the CLI exit-code policy in main.c so
 * that usage, manifest/integrity, and network failures can be told apart by
 * both users and scripts (spec §5 "既存 CLI との衝突").
 */
typedef enum {
    PKG_OK = 0,
    PKG_ERR_USAGE,     /* bad command line / arguments                        */
    PKG_ERR_MANIFEST,  /* malformed manifest / lockfile / identity mismatch   */
    PKG_ERR_IO,        /* filesystem read/write error                         */
    PKG_ERR_NETWORK,   /* download / TLS / HTTP error (later phases)          */
    PKG_ERR_INTEGRITY, /* hash mismatch / ZIP safety violation (later phases) */
    PKG_ERR_UNSUPPORTED/* explicitly-unsupported path (e.g. MVM package link) */
} PkgErrorCode;

/*
 * A diagnostic.  `message` is a heap-allocated, human-readable string owned by
 * the PkgError; call pkg_error_reset()/pkg_error_free() to release it.  `line`
 * is the 1-based manifest line the error refers to, or 0 when not applicable.
 */
typedef struct {
    PkgErrorCode code;
    int          line;
    char        *message;
} PkgError;

/* Initialise an error slot to PKG_OK / no message. */
void pkg_error_init(PkgError *err);
/* Free any owned message and reset the slot to PKG_OK. */
void pkg_error_reset(PkgError *err);
/*
 * Set the error to `code` with a printf-style message referring to `line`
 * (pass 0 for "no line").  Any previous message is freed first.  Safe to call
 * with err == NULL (the message is then discarded), so validators can be used
 * both with and without a diagnostic sink.
 */
void pkg_error_set(PkgError *err, PkgErrorCode code, int line,
                   const char *fmt, ...);

/* ------------------------------------------------------------------ */
/* GitHub source identity                                              */
/* ------------------------------------------------------------------ */

/*
 * A canonical, immutable package source:
 *
 *     github:<owner>/<repo>@<40-hex-lowercase-commit-sha>
 *
 * This is the ONLY source form accepted in manifests and lockfiles in the
 * initial release (spec §2.3).  Branch names, tags, "latest", short SHAs,
 * arbitrary URLs, git clone, non-GitHub hosts, private repos and submodules
 * are all rejected.
 */
typedef struct {
    char *owner;                    /* GitHub owner / org                     */
    char *repo;                     /* repository name (no ".git")            */
    char  sha[PKG_SHA_LEN + 1];     /* 40 lowercase hex chars + NUL           */
} PkgSource;

void pkg_source_init(PkgSource *s);
void pkg_source_reset(PkgSource *s);

/*
 * Parse a canonical internal source string
 * ("github:<owner>/<repo>@<sha>") into *out.  Returns true on success.  On
 * failure returns false and sets *err (PKG_ERR_MANIFEST).  `line` is used only
 * for the diagnostic.
 */
bool pkg_source_parse(const char *text, int line, PkgSource *out, PkgError *err);

/*
 * Serialise the "github:<owner>/<repo>" prefix (WITHOUT the @sha) into a fresh
 * heap string — this is the lockfile `source` field.  Caller frees.
 */
char *pkg_source_canonical_no_sha(const PkgSource *s);

/*
 * Build the deterministic GitHub archive URL for a source:
 *
 *     https://codeload.github.com/<owner>/<repo>/zip/<sha>
 *
 * Verified 2026-08-19 against the live service: this URL returns the zip
 * directly with HTTP 200 (no redirect).  See docs/package_manager.md.
 * Returns a fresh heap string; caller frees.
 */
char *pkg_source_archive_url(const PkgSource *s);

/* ------------------------------------------------------------------ */
/* GitHub install URL parsing (`myon pkg install <GitHub URL>`)        */
/* ------------------------------------------------------------------ */

/*
 * The kind of ref extracted from a user-supplied GitHub URL.  ref resolution
 * to an immutable commit SHA happens in a later (network) phase; this parser
 * only classifies the URL and extracts identity + ref text safely.
 */
typedef enum {
    PKG_REF_DEFAULT = 0, /* no ref given -> repository default branch          */
    PKG_REF_BRANCH,      /* .../tree/<branch>                                  */
    PKG_REF_TAG,         /* .../releases/tag/<tag>                             */
    PKG_REF_COMMIT       /* .../tree/<40-hex> or .../commit/<40-hex>           */
} PkgRefKind;

typedef struct {
    char       *owner;
    char       *repo;      /* ".git" suffix stripped                          */
    PkgRefKind  ref_kind;
    char       *ref;       /* NULL for PKG_REF_DEFAULT; else the ref text      */
} PkgInstallUrl;

void pkg_install_url_init(PkgInstallUrl *u);
void pkg_install_url_reset(PkgInstallUrl *u);

/*
 * Parse a user-facing GitHub URL into repository identity + ref.  Accepts:
 *
 *   https://github.com/<owner>/<repo>
 *   https://github.com/<owner>/<repo>.git
 *   https://github.com/<owner>/<repo>/tree/<ref>
 *   https://github.com/<owner>/<repo>/commit/<sha>
 *   https://github.com/<owner>/<repo>/releases/tag/<tag>
 *
 * Rejects: non-https schemes, hosts other than github.com, control bytes,
 * embedded credentials, and query/fragment noise.  Returns true on success;
 * on failure sets *err (PKG_ERR_USAGE) and returns false.
 *
 * NOTE: this does NOT contact the network; it only classifies the URL.  A
 * mutable ref must still be resolved to a full commit SHA before it is written
 * to the lockfile (spec §2.3, §4).
 */
bool pkg_install_url_parse(const char *url, PkgInstallUrl *out, PkgError *err);

/* ------------------------------------------------------------------ */
/* Package-list registry ("myon pkg install <user>/<repo>")            */
/* ------------------------------------------------------------------ */

/*
 * A GitHub "shorthand" reference "<owner>/<repo>" — the form a user types for
 * `myon pkg install user/repo`.  This is NOT a URL; it is looked up in the
 * package-list registries (spec: `.myon/packages.list`) to find the matching
 * GitHub repository, which is then installed exactly like an explicit URL.
 */
typedef struct {
    char *owner;
    char *repo;
} PkgShorthand;

void pkg_shorthand_init(PkgShorthand *s);
void pkg_shorthand_reset(PkgShorthand *s);

/*
 * True if `arg` looks like a "<owner>/<repo>" shorthand rather than a URL:
 * it contains exactly one '/', no scheme ("://"), no whitespace/control bytes,
 * and both segments are non-empty.  Used by the CLI to decide between the URL
 * path and the registry-lookup path.  Does not fully validate the segments;
 * pkg_shorthand_parse() does that.
 */
bool pkg_arg_is_shorthand(const char *arg);

/*
 * Parse and validate "<owner>/<repo>" into *out.  Segments must be valid
 * GitHub path segments (ASCII letters/digits/'.'/'_'/'-', no "..", not "."
 * alone, length-bounded).  A trailing ".git" on the repo is stripped.  Returns
 * true on success (caller frees via pkg_shorthand_reset); false + *err
 * (PKG_ERR_USAGE) otherwise.
 */
bool pkg_shorthand_parse(const char *arg, PkgShorthand *out, PkgError *err);

/*
 * One decoded registry entry: a repository shorthand "<owner>/<repo>", plus an
 * optional short alias/name (registry JSON object form maps alias -> shorthand).
 * `alias` is NULL for the array form.
 */
typedef struct {
    char *alias;   /* optional short name (object form); NULL otherwise */
    char *owner;
    char *repo;
} PkgRegistryEntry;

typedef struct {
    PkgRegistryEntry *entries;
    size_t            count;
} PkgRegistry;

PkgRegistry *pkg_registry_new(void);
void         pkg_registry_free(PkgRegistry *r);

/*
 * Parse a registry JSON document (NUL-terminated `text`) into *PkgRegistry.
 *
 * Two concrete, well-defined shapes are accepted (spec: package lists):
 *
 *   1. Array of shorthands:
 *          ["acme/myon-json", "owner/pkg", ...]
 *
 *   2. Object mapping a short alias to a shorthand:
 *          { "json": "acme/myon-json", "text": "acme/myon-text" }
 *
 * Every value must be a valid "<owner>/<repo>" shorthand.  This is a strict,
 * self-contained JSON scanner (no external dependency) that rejects malformed
 * JSON, non-string values, control bytes and oversized documents.  It NEVER
 * executes anything — the registry is pure data.  Returns NULL + *err on error.
 */
PkgRegistry *pkg_registry_parse(const char *text, PkgError *err);

/*
 * Look up a "<owner>/<repo>" shorthand in a parsed registry.  Matching is on
 * the owner/repo pair (case-sensitive, GitHub-style).  The registry alias, if
 * any, is also accepted as a match when `want_owner` is NULL (alias-only
 * lookup).  Returns the matching entry or NULL.
 */
const PkgRegistryEntry *pkg_registry_find(const PkgRegistry *r,
                                          const char *want_owner,
                                          const char *want_repo);

/*
 * Parse `.myon/packages.list` text into a heap array of registry URLs.  Each
 * non-empty, non-comment ('#') line is one URL and must be https://.  Returns
 * the number of URLs and stores a freshly-allocated array of heap strings in
 * *out_urls (caller frees each element and the array); on error returns -1 and
 * sets *err.  An empty/comment-only file yields 0 URLs and *out_urls == NULL.
 */
long pkg_packages_list_parse(const char *text, char ***out_urls, PkgError *err);

/* ------------------------------------------------------------------ */
/* Identity validators                                                 */
/* ------------------------------------------------------------------ */

/*
 * Validate a package name (spec §3.1): ASCII lowercase letters, digits, '.'
 * and '-'.  Rejects path separators, whitespace, control chars, "..", a
 * leading '.', a trailing '.', and the empty string.  The package name is the
 * install-directory identity.  Returns true if valid; else sets *err.
 */
bool pkg_validate_package_name(const char *name, int line, PkgError *err);

/*
 * Validate a module namespace (spec §3.2 / §6.2): a dotted path of ASCII
 * lowercase segments (letters/digits/'-', not starting with a digit or '-'),
 * e.g. "acme.json" or "example.tools".  This is the import identity.  Returns
 * true if valid; else sets *err.
 */
bool pkg_validate_module_name(const char *name, int line, PkgError *err);

/* True if `s` is exactly PKG_SHA_LEN lowercase hex characters. */
bool pkg_is_full_sha(const char *s);
/* True if `s` is exactly PKG_SHA256_HEX_LEN lowercase hex characters. */
bool pkg_is_sha256_hex(const char *s);

/* ------------------------------------------------------------------ */
/* Dependency edge                                                     */
/* ------------------------------------------------------------------ */

/* A single "[dependencies]" entry: a package name bound to a GitHub source. */
typedef struct {
    char      *name;    /* dependency package name (validated)                */
    PkgSource  source;  /* full-SHA GitHub source                             */
} PkgDependency;

/* ------------------------------------------------------------------ */
/* Root project manifest: myon.toml                                    */
/* ------------------------------------------------------------------ */

typedef struct {
    char          *project_name;    /* [project].name (required)              */
    char          *project_version; /* [project].version (required)           */
    PkgDependency *deps;            /* [dependencies], sorted by name         */
    size_t         dep_count;
} PkgManifest;

PkgManifest *pkg_manifest_new(void);
void         pkg_manifest_free(PkgManifest *m);

/*
 * Parse a root manifest from an in-memory buffer (NUL-terminated `text`).
 * Enforces the strict subset in spec §3.1: format=1 required; [project].name /
 * [project].version required; [dependencies] optional; unknown key/section and
 * duplicate keys rejected with a line number.  Returns a manifest on success
 * (caller frees) or NULL on error (with *err set).
 */
PkgManifest *pkg_manifest_parse(const char *text, PkgError *err);

/* Convenience: read a file, then pkg_manifest_parse().  Sets *err on I/O. */
PkgManifest *pkg_manifest_parse_file(const char *path, PkgError *err);

/*
 * Look up a dependency by name; returns NULL if absent.  O(log n): deps are
 * kept sorted by name.
 */
const PkgDependency *pkg_manifest_find_dep(const PkgManifest *m,
                                           const char *name);

/*
 * Add or replace a dependency, preserving all other project metadata and
 * dependencies (spec §3.1: never blindly overwrite an existing manifest).  If
 * a dependency with the same name exists with a *different* source, this is an
 * explicit update.  Returns true on success; false + *err on validation fail.
 */
bool pkg_manifest_upsert_dep(PkgManifest *m, const char *name,
                             const PkgSource *source, PkgError *err);

/*
 * Serialise the manifest back to canonical strict-subset TOML (deterministic:
 * dependencies sorted by name).  Returns a fresh heap string; caller frees.
 *
 * NOTE (spec §3.1): the round-trip writer is used only for manifests the
 * package manager itself created/owns.  When mutating a user-authored manifest
 * that may contain comments or unmanaged keys, callers should prefer a
 * minimal, surgical edit rather than a full rewrite.  A comment-preserving
 * editor is provided separately (pkg_manifest_render_min) for the auto-init
 * path.
 */
char *pkg_manifest_render(const PkgManifest *m);

/* ------------------------------------------------------------------ */
/* Package manifest: package.myon                                      */
/* ------------------------------------------------------------------ */

typedef struct {
    char          *name;    /* [package].name (required)                      */
    char          *version; /* [package].version (required)                   */
    char          *module;  /* [package].module (required, import namespace)  */
    PkgDependency *deps;    /* [dependencies], sorted by name                 */
    size_t         dep_count;
} PkgPackageManifest;

PkgPackageManifest *pkg_package_manifest_new(void);
void                pkg_package_manifest_free(PkgPackageManifest *m);

/*
 * Parse a package manifest (spec §3.2): format=1 required; [package].name /
 * version / module required; [dependencies] optional and in the same source
 * form as the root manifest.  Returns NULL + *err on failure.
 *
 * SECURITY: parsing must never execute package code.  This is a pure text
 * parse (no interpreter involvement), matching spec §3.2.
 */
PkgPackageManifest *pkg_package_manifest_parse(const char *text, PkgError *err);
PkgPackageManifest *pkg_package_manifest_parse_file(const char *path,
                                                    PkgError *err);

/* ------------------------------------------------------------------ */
/* Lockfile: myon.lock                                                 */
/* ------------------------------------------------------------------ */

/* One [[package]] entry in the lockfile (spec §4). */
typedef struct {
    char      *name;                          /* package name (unique key)    */
    char      *version;                       /* recorded version             */
    char      *module;                        /* module namespace             */
    PkgSource  source;                        /* github:<owner>/<repo>@<sha>  */
    char       sha256[PKG_SHA256_HEX_LEN + 1];/* archive SHA-256 (lowercase)  */
    char     **deps;                          /* direct dependency names,     */
    size_t     dep_count;                     /*   sorted, deduplicated       */
} PkgLockEntry;

typedef struct {
    PkgLockEntry *entries;   /* sorted by name for deterministic output       */
    size_t        count;
} PkgLock;

PkgLock *pkg_lock_new(void);
void     pkg_lock_free(PkgLock *l);

/*
 * Parse a lockfile from an in-memory buffer.  Enforces: format=1, unique
 * package names, valid source/sha/sha256/module, and deterministic field set.
 * Returns NULL + *err on failure.
 */
PkgLock *pkg_lock_parse(const char *text, PkgError *err);
PkgLock *pkg_lock_parse_file(const char *path, PkgError *err);

/* Look up a locked package by name; NULL if absent. */
const PkgLockEntry *pkg_lock_find(const PkgLock *l, const char *name);

/*
 * Insert (taking ownership of the heap strings inside `entry` via a deep copy)
 * a locked package, keeping entries sorted by name.  Rejects a duplicate name
 * whose source revision differs (spec §4: same name / different revision is a
 * conflict).  A byte-identical re-insert is a no-op success.  Returns true on
 * success; false + *err otherwise.
 */
bool pkg_lock_upsert(PkgLock *l, const PkgLockEntry *entry, PkgError *err);

/*
 * Render the lockfile to canonical, deterministic TOML (spec §4): entries
 * sorted by name, dependency lists sorted, fixed field order.  Returns a fresh
 * heap string; caller frees.
 */
char *pkg_lock_render(const PkgLock *l);

/*
 * Cross-check that the root manifest's direct dependencies and the lockfile's
 * top-level packages agree (spec §4: "root manifest と lockfile の直接依存が
 * 一致しなければならない").  Returns true if consistent; false + *err
 * (PKG_ERR_MANIFEST) describing the first mismatch.
 */
bool pkg_lock_check_matches_manifest(const PkgLock *l, const PkgManifest *m,
                                     PkgError *err);

/*
 * Serialise a package manifest's [package] identity to the canonical strict
 * subset used when the resolver needs to re-emit it.  (Used mainly by tests.)
 */
char *pkg_package_manifest_render(const PkgPackageManifest *m);

/* ------------------------------------------------------------------ */
/* Network-driven operations (spec §5) — implemented in pkg_ops.c       */
/* ------------------------------------------------------------------ */

/*
 * These are the commands that need the fetch / ZIP / hash layers.  They are
 * split into their own translation unit (pkg_ops.c) so package.c stays the
 * pure, offline data-model + parser + dispatch layer.  Each returns a process
 * exit code (0 == success) and prints diagnostics to stderr, mirroring the
 * exit-code policy in package.c.
 *
 * A test seam: pkg_ops_set_transport() installs an alternate PkgTransport (see
 * pkg_fetch.h) so the resolver/install pipeline can be exercised offline.
 * Passing NULL restores the default (real network) transport.
 */
struct PkgTransport; /* forward declaration (defined in pkg_fetch.h) */
void pkg_ops_set_transport(const struct PkgTransport *tr);

/* `myon pkg lock` — resolve dependencies and (re)write myon.lock. */
int pkg_ops_lock(void);

/* `myon pkg install` (no URL) — reproduce install from the existing lockfile. */
int pkg_ops_install_locked(void);

/* `myon pkg install <github-url>` — add a dependency, resolve, lock, install. */
int pkg_ops_install_url(const char *url);

/*
 * `myon pkg install <owner>/<repo>` — resolve the shorthand against the
 * package-list registries in `.myon/packages.list`, then install the matching
 * GitHub repository exactly like `pkg_ops_install_url`.  Fails with a clear
 * diagnostic if there is no packages.list, if no registry lists the shorthand,
 * or if a registry cannot be fetched/parsed.
 */
int pkg_ops_install_shorthand(const char *shorthand);

/* ------------------------------------------------------------------ */
/* CLI entry point                                                     */
/* ------------------------------------------------------------------ */

/*
 * Handle `myon pkg <subcommand> [args...]`.  `argc`/`argv` are the arguments
 * AFTER the "pkg" token (argv[0] == the subcommand, or argc == 0 for a bare
 * "myon pkg").  Returns a process exit code following the interpreter's
 * convention (0 success; 64 usage; other non-zero for manifest/integrity/
 * network errors).  Prints diagnostics to stderr.
 */
int pkg_cli_main(int argc, char **argv);

#endif /* MYON_PACKAGE_H */
