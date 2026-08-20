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
 * Package-manager network-driven operations (spec §5): `pkg lock`,
 * `pkg install` and `pkg install <github-url>`, plus the recursive dependency
 * resolver (spec §4) that ties the fetch / ZIP / hash / filesystem layers
 * together.
 *
 * SECURITY (spec §2.2, §3.2): none of this executes package code.  Manifests
 * are parsed as plain text; archives are only inspected and extracted.  The
 * resolver reads package.myon files purely as data.
 */

#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#  define _POSIX_C_SOURCE 200809L
#endif

#include "package.h"
#include "pkg_fetch.h"
#include "pkg_fs.h"
#include "pkg_zip.h"
#include "pkg_hash.h"
#include "common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(_WIN32)
#  include <unistd.h>
#else
#  include <direct.h>
#  define getcwd _getcwd
#endif

/* ------------------------------------------------------------------ */
/* transport seam (tests inject a mock; default is the real network)   */
/* ------------------------------------------------------------------ */

static const PkgTransport *g_transport = NULL;

void pkg_ops_set_transport(const struct PkgTransport *tr) {
    g_transport = (const PkgTransport *)tr;
}

static const PkgTransport *transport(void) {
    return g_transport ? g_transport : pkg_transport_default();
}

/* ------------------------------------------------------------------ */
/* exit-code policy (mirrors package.c)                                */
/* ------------------------------------------------------------------ */

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

static void report(const PkgError *err, const char *ctx) {
    if (!err || err->code == PKG_OK) return;
    if (err->line > 0)
        fprintf(stderr, "myon pkg: %s: line %d: %s\n", ctx, err->line,
                err->message ? err->message : "(unknown error)");
    else
        fprintf(stderr, "myon pkg: %s: %s\n", ctx,
                err->message ? err->message : "(unknown error)");
}

/* ------------------------------------------------------------------ */
/* project paths                                                       */
/* ------------------------------------------------------------------ */

typedef struct {
    char *root;      /* project root (dir containing myon.toml)         */
    char *toml;      /* <root>/myon.toml                                */
    char *lock;      /* <root>/myon.lock                                */
    char *packages;  /* <root>/.myon/packages                          */
} Paths;

static bool paths_discover(Paths *p, PkgError *err, bool require_manifest) {
    memset(p, 0, sizeof(*p));
    char *ferr = NULL;
    p->root = pkg_fs_find_project_root(&ferr);
    if (!p->root) {
        if (require_manifest) {
            pkg_error_set(err, PKG_ERR_USAGE, 0, "%s",
                          ferr ? ferr : "no myon.toml found");
            free(ferr);
            return false;
        }
        /* auto-init path: use the current directory as the root. */
        free(ferr);
        char cwd[4096];
        if (!getcwd(cwd, sizeof(cwd))) { pkg_error_set(err, PKG_ERR_IO, 0, "cannot get cwd"); return false; }
        p->root = myon_strdup(cwd);
    }
    p->toml = pkg_fs_join(p->root, "myon.toml");
    p->lock = pkg_fs_join(p->root, "myon.lock");
    char *dotmyon = pkg_fs_join(p->root, PKG_DIR_DOTMYON);
    p->packages = pkg_fs_join(dotmyon, PKG_DIR_PACKAGES);
    free(dotmyon);
    return true;
}

static void paths_free(Paths *p) {
    free(p->root); free(p->toml); free(p->lock); free(p->packages);
    memset(p, 0, sizeof(*p));
}

/* Write a whole buffer to `path` atomically-ish (temp + rename). */
static bool write_text_file(const char *path, const char *text, PkgError *err) {
    size_t n = strlen(path) + 8;
    char *tmp = myon_xmalloc(n);
    snprintf(tmp, n, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) { pkg_error_set(err, PKG_ERR_IO, 0, "cannot write '%s'", tmp); free(tmp); return false; }
    size_t len = strlen(text);
    bool ok = (len == 0) || (fwrite(text, 1, len, f) == len);
    if (fclose(f) != 0) ok = false;
    if (!ok) { pkg_error_set(err, PKG_ERR_IO, 0, "short write to '%s'", tmp); remove(tmp); free(tmp); return false; }
    if (rename(tmp, path) != 0) { pkg_error_set(err, PKG_ERR_IO, 0, "cannot replace '%s'", path); remove(tmp); free(tmp); return false; }
    free(tmp);
    return true;
}

/* ------------------------------------------------------------------ */
/* archive fetch + hash + staging extract                              */
/* ------------------------------------------------------------------ */

/*
 * Fetch the archive for `src`, optionally verifying it against
 * `expect_sha256` (NULL to skip).  On success returns the raw bytes
 * (caller frees *out_data) and writes the computed digest into out_hash
 * (>= 65 bytes).
 */
static bool fetch_archive(const PkgSource *src, const char *expect_sha256,
                          unsigned char **out_data, size_t *out_len,
                          char *out_hash, PkgError *err) {
    char *url = pkg_source_archive_url(src);
    char *ferr = NULL;
    unsigned char *data = NULL; size_t len = 0;
    if (!pkg_fetch_https_get(transport(), url, &data, &len, &ferr)) {
        pkg_error_set(err, PKG_ERR_NETWORK, 0, "%s (%s)", ferr ? ferr : "download failed", url);
        free(ferr); free(url);
        return false;
    }
    free(url);

    if (!pkg_sha256_hex(data, len, out_hash)) {
        pkg_error_set(err, PKG_ERR_INTEGRITY, 0, "cannot hash archive for %s/%s", src->owner, src->repo);
        free(data); return false;
    }
    if (expect_sha256 && !pkg_sha256_equal(out_hash, expect_sha256)) {
        pkg_error_set(err, PKG_ERR_INTEGRITY, 0,
                      "archive hash mismatch for %s/%s@%s (expected %s, got %s)",
                      src->owner, src->repo, src->sha, expect_sha256, out_hash);
        free(data); return false;
    }
    *out_data = data; *out_len = len;
    return true;
}

/*
 * Extract an in-memory archive to a fresh staging directory under the project
 * root.  Returns the staging path (caller frees + rmtrees) or NULL on error.
 */
static char *extract_to_staging(const Paths *p, const unsigned char *data,
                                size_t len, PkgError *err) {
    char *serr = NULL;
    char *staging = pkg_fs_make_staging(p->root, &serr);
    if (!staging) { pkg_error_set(err, PKG_ERR_IO, 0, "%s", serr ? serr : "cannot create staging dir"); free(serr); return NULL; }

    char *zerr = NULL, *rootname = NULL;
    if (!pkg_zip_extract(data, len, staging, &rootname, &zerr)) {
        pkg_error_set(err, PKG_ERR_INTEGRITY, 0, "%s", zerr ? zerr : "archive extraction failed");
        free(zerr); free(rootname);
        pkg_fs_rmtree(staging, NULL);
        free(staging);
        return NULL;
    }
    free(zerr); free(rootname);
    return staging;
}

/*
 * Validate a staged package layout (spec §3.3): package.myon present, its
 * declared module maps to an existing modules/<path>.myon file.  On success
 * returns the parsed package manifest (caller frees).
 */
static PkgPackageManifest *validate_staged(const char *staging,
                                           const char *expect_name,
                                           PkgError *err) {
    char *pm_path = pkg_fs_join(staging, "package.myon");
    if (!pkg_fs_is_file(pm_path)) {
        pkg_error_set(err, PKG_ERR_MANIFEST, 0, "archive is missing package.myon");
        free(pm_path);
        return NULL;
    }
    PkgPackageManifest *pm = pkg_package_manifest_parse_file(pm_path, err);
    free(pm_path);
    if (!pm) return NULL;

    if (expect_name && strcmp(pm->name, expect_name) != 0) {
        pkg_error_set(err, PKG_ERR_MANIFEST, 0,
                      "package.myon declares name '%s' but the dependency key is '%s'",
                      pm->name, expect_name);
        pkg_package_manifest_free(pm);
        return NULL;
    }

    /* module "a.b.c" -> modules/a/b/c.myon (every segment path-safe). */
    char *rel = myon_strdup(pm->module);
    for (char *q = rel; *q; q++) if (*q == '.') *q = '/';
    size_t n = strlen(staging) + strlen(rel) + 32;
    char *mod = myon_xmalloc(n);
    snprintf(mod, n, "%s/modules/%s.myon", staging, rel);
    bool present = pkg_fs_is_file(mod);
    free(rel);
    if (!present) {
        pkg_error_set(err, PKG_ERR_MANIFEST, 0,
                      "package '%s' declares module '%s' but modules/%s.myon is missing",
                      pm->name, pm->module, pm->module);
        free(mod);
        pkg_package_manifest_free(pm);
        return NULL;
    }
    free(mod);
    return pm;
}

/* ------------------------------------------------------------------ */
/* recursive resolver                                                  */
/* ------------------------------------------------------------------ */

typedef struct {
    char *name;
    char *staging; /* kept only when installing */
} StagedPkg;

typedef struct {
    PkgLock     *lock;
    const Paths *paths;
    /* module namespace -> package name, to detect collisions */
    char       **mod_ns;
    char       **mod_owner;
    size_t       mod_count;
    /* staged dirs for install (empty when just locking) */
    StagedPkg   *staged;
    size_t       staged_count;
    bool         keep_staging;
} Resolver;

static bool ns_register(Resolver *r, const char *ns, const char *owner, PkgError *err) {
    for (size_t i = 0; i < r->mod_count; i++) {
        if (strcmp(r->mod_ns[i], ns) == 0) {
            if (strcmp(r->mod_owner[i], owner) != 0) {
                pkg_error_set(err, PKG_ERR_MANIFEST, 0,
                              "module namespace collision: '%s' is declared by both "
                              "'%s' and '%s'", ns, r->mod_owner[i], owner);
                return false;
            }
            return true; /* same package re-registering: fine */
        }
    }
    r->mod_ns    = myon_xrealloc(r->mod_ns,    (r->mod_count + 1) * sizeof(char *));
    r->mod_owner = myon_xrealloc(r->mod_owner, (r->mod_count + 1) * sizeof(char *));
    r->mod_ns[r->mod_count]    = myon_strdup(ns);
    r->mod_owner[r->mod_count] = myon_strdup(owner);
    r->mod_count++;
    return true;
}

/* DFS visit of a single dependency edge. */
static bool resolve_dep(Resolver *r, const char *name, const PkgSource *src,
                        const char **stack, size_t stack_n, PkgError *err) {
    /* cycle detection: report the full path (spec §4). */
    for (size_t i = 0; i < stack_n; i++) {
        if (strcmp(stack[i], name) == 0) {
            char buf[1024]; size_t off = 0;
            for (size_t j = i; j < stack_n && off < sizeof(buf); j++)
                off += (size_t)snprintf(buf + off, sizeof(buf) - off, "%s -> ", stack[j]);
            if (off < sizeof(buf)) snprintf(buf + off, sizeof(buf) - off, "%s", name);
            pkg_error_set(err, PKG_ERR_MANIFEST, 0, "dependency cycle detected: %s", buf);
            return false;
        }
    }

    /* If already locked with the same revision, we are done (DAG sharing). */
    const PkgLockEntry *existing = pkg_lock_find(r->lock, name);
    if (existing) {
        if (strcmp(existing->source.sha, src->sha) != 0 ||
            strcmp(existing->source.owner, src->owner) != 0 ||
            strcmp(existing->source.repo, src->repo) != 0) {
            pkg_error_set(err, PKG_ERR_MANIFEST, 0,
                          "conflicting revisions requested for package '%s' "
                          "(%s/%s@%s vs %s/%s@%s)",
                          name, existing->source.owner, existing->source.repo,
                          existing->source.sha, src->owner, src->repo, src->sha);
            return false;
        }
        return true;
    }

    /* Fetch + hash + extract + validate. */
    unsigned char *data = NULL; size_t len = 0; char hash[65];
    if (!fetch_archive(src, NULL, &data, &len, hash, err)) return false;

    char *staging = extract_to_staging(r->paths, data, len, err);
    free(data);
    if (!staging) return false;

    PkgPackageManifest *pm = validate_staged(staging, name, err);
    if (!pm) { pkg_fs_rmtree(staging, NULL); free(staging); return false; }

    if (!ns_register(r, pm->module, name, err)) {
        pkg_package_manifest_free(pm); pkg_fs_rmtree(staging, NULL); free(staging);
        return false;
    }

    /* Build and insert the lock entry (upsert deep-copies everything). */
    PkgLockEntry e; memset(&e, 0, sizeof(e));
    e.name = (char *)name;
    e.version = pm->version;
    e.module = pm->module;
    e.source = *src;                     /* shallow: strings owned by caller  */
    memcpy(e.sha256, hash, sizeof(e.sha256));
    e.dep_count = pm->dep_count;
    if (pm->dep_count) {
        e.deps = myon_xmalloc(pm->dep_count * sizeof(char *));
        for (size_t i = 0; i < pm->dep_count; i++) e.deps[i] = pm->deps[i].name;
    }
    bool ins = pkg_lock_upsert(r->lock, &e, err);
    free(e.deps);
    if (!ins) { pkg_package_manifest_free(pm); pkg_fs_rmtree(staging, NULL); free(staging); return false; }

    /* Keep or drop the staging dir. */
    if (r->keep_staging) {
        r->staged = myon_xrealloc(r->staged, (r->staged_count + 1) * sizeof(StagedPkg));
        r->staged[r->staged_count].name = myon_strdup(name);
        r->staged[r->staged_count].staging = staging; /* transfer ownership */
        r->staged_count++;
    } else {
        pkg_fs_rmtree(staging, NULL);
        free(staging);
    }

    /* Recurse into this package's dependencies. */
    const char *next[256];
    size_t nn = 0;
    for (size_t i = 0; i < stack_n && nn < 255; i++) next[nn++] = stack[i];
    next[nn++] = name;
    bool ok = true;
    for (size_t i = 0; i < pm->dep_count && ok; i++)
        ok = resolve_dep(r, pm->deps[i].name, &pm->deps[i].source, next, nn, err);

    pkg_package_manifest_free(pm);
    return ok;
}

static void resolver_free(Resolver *r, bool rmtree_staged) {
    for (size_t i = 0; i < r->mod_count; i++) { free(r->mod_ns[i]); free(r->mod_owner[i]); }
    free(r->mod_ns); free(r->mod_owner);
    for (size_t i = 0; i < r->staged_count; i++) {
        if (rmtree_staged && r->staged[i].staging) pkg_fs_rmtree(r->staged[i].staging, NULL);
        free(r->staged[i].name); free(r->staged[i].staging);
    }
    free(r->staged);
}

/* Resolve the whole graph from a root manifest into a fresh lock. */
static PkgLock *resolve_all(const Paths *paths, const PkgManifest *m,
                            bool keep_staging, Resolver *out_res, PkgError *err) {
    Resolver r; memset(&r, 0, sizeof(r));
    r.lock = pkg_lock_new();
    r.paths = paths;
    r.keep_staging = keep_staging;

    bool ok = true;
    for (size_t i = 0; i < m->dep_count && ok; i++)
        ok = resolve_dep(&r, m->deps[i].name, &m->deps[i].source, NULL, 0, err);

    if (!ok) {
        pkg_lock_free(r.lock);
        resolver_free(&r, true);
        return NULL;
    }
    PkgLock *l = r.lock;
    if (out_res) *out_res = r; else resolver_free(&r, keep_staging);
    return l;
}

/* ------------------------------------------------------------------ */
/* pkg lock                                                            */
/* ------------------------------------------------------------------ */

int pkg_ops_lock(void) {
    PkgError err; pkg_error_init(&err);
    Paths p;
    if (!paths_discover(&p, &err, true)) { report(&err, "lock"); int rc = exit_code_for(err.code); pkg_error_reset(&err); return rc; }

    int rc = 0;
    PkgManifest *m = pkg_manifest_parse_file(p.toml, &err);
    if (!m) { report(&err, "lock"); rc = exit_code_for(err.code); goto done; }

    PkgLock *l = resolve_all(&p, m, /*keep_staging=*/false, NULL, &err);
    if (!l) { report(&err, "lock"); rc = exit_code_for(err.code); pkg_manifest_free(m); goto done; }

    if (!pkg_lock_check_matches_manifest(l, m, &err)) {
        report(&err, "lock"); rc = exit_code_for(err.code);
        pkg_lock_free(l); pkg_manifest_free(m); goto done;
    }

    char *text = pkg_lock_render(l);
    if (!write_text_file(p.lock, text, &err)) { report(&err, "lock"); rc = exit_code_for(err.code); }
    else printf("myon pkg: wrote %s (%zu package%s locked)\n", p.lock, l->count, l->count == 1 ? "" : "s");
    free(text);
    pkg_lock_free(l);
    pkg_manifest_free(m);

done:
    paths_free(&p);
    pkg_error_reset(&err);
    return rc;
}

/* ------------------------------------------------------------------ */
/* pkg install (no URL): reproduce from the lockfile                   */
/* ------------------------------------------------------------------ */

int pkg_ops_install_locked(void) {
    PkgError err; pkg_error_init(&err);
    Paths p;
    if (!paths_discover(&p, &err, true)) { report(&err, "install"); int rc = exit_code_for(err.code); pkg_error_reset(&err); return rc; }

    int rc = 0;
    PkgManifest *m = pkg_manifest_parse_file(p.toml, &err);
    if (!m) { report(&err, "install"); rc = exit_code_for(err.code); goto done; }

    PkgLock *l = pkg_lock_parse_file(p.lock, &err);
    if (!l) {
        if (err.code == PKG_ERR_IO)
            fprintf(stderr, "myon pkg: install: no myon.lock found (run `myon pkg lock` or `myon pkg install <github-url>`)\n");
        else report(&err, "install");
        rc = exit_code_for(err.code); pkg_manifest_free(m); goto done;
    }

    /* The lockfile must still describe exactly the manifest's direct deps.
     * If a user hand-edited myon.toml, require an explicit re-lock (spec §4). */
    if (!pkg_lock_check_matches_manifest(l, m, &err)) {
        report(&err, "install");
        fprintf(stderr, "myon pkg: install: myon.lock is stale; run `myon pkg lock`\n");
        rc = exit_code_for(err.code); pkg_lock_free(l); pkg_manifest_free(m); goto done;
    }

    /* Install each locked package: fetch, verify hash, extract, promote. */
    for (size_t i = 0; i < l->count; i++) {
        const PkgLockEntry *e = &l->entries[i];

        unsigned char *data = NULL; size_t len = 0; char hash[65];
        if (!fetch_archive(&e->source, e->sha256, &data, &len, hash, &err)) {
            report(&err, "install"); rc = exit_code_for(err.code); break;
        }
        char *staging = extract_to_staging(&p, data, len, &err);
        free(data);
        if (!staging) { report(&err, "install"); rc = exit_code_for(err.code); break; }

        PkgPackageManifest *pm = validate_staged(staging, e->name, &err);
        if (!pm) { report(&err, "install"); rc = exit_code_for(err.code); pkg_fs_rmtree(staging, NULL); free(staging); break; }
        pkg_package_manifest_free(pm);

        char *final_dir = pkg_fs_join(p.packages, e->name);
        char *perr = NULL;
        bool pok = pkg_fs_promote(staging, final_dir, &perr);
        if (!pok) {
            pkg_error_set(&err, PKG_ERR_IO, 0, "%s", perr ? perr : "promote failed");
            report(&err, "install"); rc = exit_code_for(err.code);
            free(perr); pkg_fs_rmtree(staging, NULL); free(staging); free(final_dir); break;
        }
        free(perr); free(staging); free(final_dir);
    }

    if (rc == 0)
        printf("myon pkg: installed %zu package%s under %s\n",
               l->count, l->count == 1 ? "" : "s", p.packages);

    pkg_lock_free(l);
    pkg_manifest_free(m);

done:
    paths_free(&p);
    pkg_error_reset(&err);
    return rc;
}

/* ------------------------------------------------------------------ */
/* pkg install <github-url>                                            */
/* ------------------------------------------------------------------ */

/* Load an existing root manifest, or synthesise a minimal one (spec §3.1). */
static PkgManifest *load_or_init_manifest(const Paths *p, PkgError *err, bool *created) {
    *created = false;
    if (pkg_fs_is_file(p->toml))
        return pkg_manifest_parse_file(p->toml, err);

    /* auto-init a minimal manifest named after the project directory. */
    PkgManifest *m = pkg_manifest_new();
    const char *base = strrchr(p->root, '/');
    base = base ? base + 1 : p->root;
    PkgError tmp; pkg_error_init(&tmp);
    char *nm = myon_strdup((base && *base) ? base : "app");
    for (char *q = nm; *q; q++) { char c = *q; if (!((c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='.'||c=='-')) *q = '-'; }
    if (!pkg_validate_package_name(nm, 0, &tmp)) { free(nm); nm = myon_strdup("app"); }
    pkg_error_reset(&tmp);
    m->project_name = nm;
    m->project_version = myon_strdup("0.1.0");
    *created = true;
    (void)err;
    return m;
}

int pkg_ops_install_url(const char *url) {
    PkgError err; pkg_error_init(&err);

    /* Parse + classify the URL first (offline, immediate diagnostics). */
    PkgInstallUrl u;
    if (!pkg_install_url_parse(url, &u, &err)) {
        report(&err, "install"); int rc = exit_code_for(err.code);
        pkg_install_url_reset(&u); pkg_error_reset(&err); return rc;
    }

    Paths p;
    if (!paths_discover(&p, &err, /*require_manifest=*/false)) {
        report(&err, "install"); int rc = exit_code_for(err.code);
        pkg_install_url_reset(&u); pkg_error_reset(&err); return rc;
    }

    int rc = 0;
    PkgManifest *m = NULL;
    char *pkg_name = NULL, *pkg_module = NULL;
    bool created = false;

    /* Resolve the ref to an immutable full commit SHA (spec §2.3). */
    char sha[41];
    {
        char *rerr = NULL;
        if (!pkg_fetch_resolve_ref(transport(), u.owner, u.repo, u.ref, sha, &rerr)) {
            pkg_error_set(&err, PKG_ERR_NETWORK, 0, "%s", rerr ? rerr : "ref resolution failed");
            free(rerr); report(&err, "install"); rc = exit_code_for(err.code); goto done;
        }
    }

    /* Build the canonical source github:<owner>/<repo>@<sha>. */
    PkgSource src; pkg_source_init(&src);
    src.owner = myon_strdup(u.owner);
    src.repo  = myon_strdup(u.repo);
    memcpy(src.sha, sha, 41);

    /* Fetch the archive once to read its package.myon (dependency key = name). */
    {
        unsigned char *data = NULL; size_t len = 0; char hash[65];
        if (!fetch_archive(&src, NULL, &data, &len, hash, &err)) {
            report(&err, "install"); rc = exit_code_for(err.code); pkg_source_reset(&src); goto done;
        }
        char *staging = extract_to_staging(&p, data, len, &err);
        free(data);
        if (!staging) { report(&err, "install"); rc = exit_code_for(err.code); pkg_source_reset(&src); goto done; }

        PkgPackageManifest *pm = validate_staged(staging, NULL, &err);
        pkg_fs_rmtree(staging, NULL); free(staging);
        if (!pm) { report(&err, "install"); rc = exit_code_for(err.code); pkg_source_reset(&src); goto done; }

        pkg_name = myon_strdup(pm->name);
        pkg_module = myon_strdup(pm->module);
        pkg_package_manifest_free(pm);
    }

    /* Load or create the root manifest, then upsert the dependency. */
    m = load_or_init_manifest(&p, &err, &created);
    if (!m) { report(&err, "install"); rc = exit_code_for(err.code); pkg_source_reset(&src); goto done; }

    if (!pkg_manifest_upsert_dep(m, pkg_name, &src, &err)) {
        report(&err, "install"); rc = exit_code_for(err.code); pkg_source_reset(&src); goto done;
    }
    pkg_source_reset(&src);

    /* Persist the (possibly new) manifest. */
    {
        char *mtext = pkg_manifest_render(m);
        bool wok = write_text_file(p.toml, mtext, &err);
        free(mtext);
        if (!wok) { report(&err, "install"); rc = exit_code_for(err.code); goto done; }
    }

    /* Resolve the full graph, keeping staged dirs so we can install them. */
    {
        Resolver res;
        PkgLock *l = resolve_all(&p, m, /*keep_staging=*/true, &res, &err);
        if (!l) { report(&err, "install"); rc = exit_code_for(err.code); goto done; }

        if (!pkg_lock_check_matches_manifest(l, m, &err)) {
            report(&err, "install"); rc = exit_code_for(err.code);
            pkg_lock_free(l); resolver_free(&res, true); goto done;
        }

        char *ltext = pkg_lock_render(l);
        bool wok = write_text_file(p.lock, ltext, &err);
        free(ltext);
        if (!wok) {
            report(&err, "install"); rc = exit_code_for(err.code);
            pkg_lock_free(l); resolver_free(&res, true); goto done;
        }

        /* Promote each staged package into .myon/packages/<name>/. */
        for (size_t i = 0; i < res.staged_count && rc == 0; i++) {
            char *final_dir = pkg_fs_join(p.packages, res.staged[i].name);
            char *perr = NULL;
            if (!pkg_fs_promote(res.staged[i].staging, final_dir, &perr)) {
                pkg_error_set(&err, PKG_ERR_IO, 0, "%s", perr ? perr : "promote failed");
                report(&err, "install"); rc = exit_code_for(err.code);
            }
            free(perr);
            free(res.staged[i].staging); res.staged[i].staging = NULL;
            free(final_dir);
        }

        if (rc == 0) {
            printf("myon pkg: installed '%s'\n", pkg_name);
            printf("          use it from Myon source with:\n");
            printf("              module %s as <alias>\n", pkg_module);
        }

        pkg_lock_free(l);
        resolver_free(&res, /*rmtree_staged=*/(rc != 0));
    }

done:
    if (m) pkg_manifest_free(m);
    free(pkg_name); free(pkg_module);
    pkg_install_url_reset(&u);
    paths_free(&p);
    pkg_error_reset(&err);
    return rc;
}
