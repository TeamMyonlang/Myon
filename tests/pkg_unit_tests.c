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
 * Unit tests for the package-manager data model (spec §11.1): manifest and
 * package-manifest parsing, source/name/module validation, GitHub install-URL
 * parsing, lockfile parse + deterministic round-trip, and the manifest/lock
 * consistency check.
 *
 * These are pure, offline tests: no network, no ZIP, no interpreter.  The
 * harness links against src/package.c and src/common.c only.
 */

#include "../src/package.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond, msg) do {                                           \
    if (cond) { g_pass++; }                                             \
    else { g_fail++; fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); } \
} while (0)

/* Parse must succeed. */
static void ok_manifest(const char *label, const char *text) {
    PkgError err; pkg_error_init(&err);
    PkgManifest *m = pkg_manifest_parse(text, &err);
    CHECK(m != NULL, label);
    if (!m && err.message) fprintf(stderr, "  (got: %s)\n", err.message);
    pkg_manifest_free(m);
    pkg_error_reset(&err);
}

/* Parse must fail (with a diagnostic). */
static void bad_manifest(const char *label, const char *text) {
    PkgError err; pkg_error_init(&err);
    PkgManifest *m = pkg_manifest_parse(text, &err);
    CHECK(m == NULL, label);
    CHECK(err.code == PKG_ERR_MANIFEST, label);
    pkg_manifest_free(m);
    pkg_error_reset(&err);
}

static void bad_pkgmanifest(const char *label, const char *text) {
    PkgError err; pkg_error_init(&err);
    PkgPackageManifest *m = pkg_package_manifest_parse(text, &err);
    CHECK(m == NULL, label);
    pkg_package_manifest_free(m);
    pkg_error_reset(&err);
}

static void test_manifests(void) {
    ok_manifest("valid root manifest",
        "format = 1\n\n[project]\nname = \"app\"\nversion = \"0.1.0\"\n");

    ok_manifest("valid root with dep",
        "format = 1\n[project]\nname=\"app\"\nversion=\"0.1.0\"\n"
        "[dependencies]\nacme.json = \"github:a/b@4c2e5ad3d8e74f3f239d6b0d6c7ab2e5e7b8d910\"\n");

    ok_manifest("comment lines are ignored",
        "# a comment\nformat = 1  # trailing\n[project]\nname=\"app\" # x\nversion=\"1\"\n");

    ok_manifest("no dependencies section",
        "format = 1\n[project]\nname=\"app\"\nversion=\"1\"\n");

    bad_manifest("missing format",
        "[project]\nname=\"app\"\nversion=\"1\"\n");

    bad_manifest("wrong format version",
        "format = 2\n[project]\nname=\"app\"\nversion=\"1\"\n");

    bad_manifest("missing project.name",
        "format = 1\n[project]\nversion=\"1\"\n");

    bad_manifest("missing project.version",
        "format = 1\n[project]\nname=\"app\"\n");

    bad_manifest("duplicate key",
        "format = 1\n[project]\nname=\"app\"\nname=\"app2\"\nversion=\"1\"\n");

    bad_manifest("unknown section",
        "format = 1\n[bogus]\nx=1\n[project]\nname=\"app\"\nversion=\"1\"\n");

    bad_manifest("unknown key in project",
        "format = 1\n[project]\nname=\"app\"\nversion=\"1\"\nbogus=\"y\"\n");

    bad_manifest("malformed string (unterminated)",
        "format = 1\n[project]\nname=\"app\nversion=\"1\"\n");

    bad_manifest("invalid package name (uppercase)",
        "format = 1\n[project]\nname=\"app\"\nversion=\"1\"\n[dependencies]\n"
        "Acme = \"github:a/b@4c2e5ad3d8e74f3f239d6b0d6c7ab2e5e7b8d910\"\n");

    bad_manifest("branch ref rejected",
        "format = 1\n[project]\nname=\"app\"\nversion=\"1\"\n[dependencies]\n"
        "acme = \"github:a/b@main\"\n");

    bad_manifest("tag ref rejected",
        "format = 1\n[project]\nname=\"app\"\nversion=\"1\"\n[dependencies]\n"
        "acme = \"github:a/b@v1.0.0\"\n");

    bad_manifest("short sha rejected",
        "format = 1\n[project]\nname=\"app\"\nversion=\"1\"\n[dependencies]\n"
        "acme = \"github:a/b@4c2e5ad\"\n");

    bad_manifest("non-github source rejected",
        "format = 1\n[project]\nname=\"app\"\nversion=\"1\"\n[dependencies]\n"
        "acme = \"https://example.com/x.zip\"\n");

    bad_manifest("array-of-tables rejected in myon.toml",
        "format = 1\n[[project]]\nname=\"app\"\nversion=\"1\"\n");

    bad_manifest("duplicate dependency",
        "format = 1\n[project]\nname=\"app\"\nversion=\"1\"\n[dependencies]\n"
        "acme = \"github:a/b@4c2e5ad3d8e74f3f239d6b0d6c7ab2e5e7b8d910\"\n"
        "acme = \"github:a/c@0d76ff2717d93655c5d95f00b4f0cfca0e5b0a22\"\n");
}

static void test_pkgmanifest(void) {
    PkgError err; pkg_error_init(&err);
    PkgPackageManifest *m = pkg_package_manifest_parse(
        "format = 1\n[package]\nname=\"acme.json\"\nversion=\"1.2.0\"\n"
        "module=\"acme.json\"\n[dependencies]\n"
        "acme.text = \"github:acme-labs/myon-text@0d76ff2717d93655c5d95f00b4f0cfca0e5b0a22\"\n",
        &err);
    CHECK(m != NULL, "valid package manifest");
    if (m) {
        CHECK(strcmp(m->name, "acme.json") == 0, "package name parsed");
        CHECK(strcmp(m->module, "acme.json") == 0, "module parsed");
        CHECK(m->dep_count == 1, "one dependency parsed");
    }
    pkg_package_manifest_free(m);
    pkg_error_reset(&err);

    bad_pkgmanifest("package manifest missing module",
        "format = 1\n[package]\nname=\"acme.json\"\nversion=\"1.2.0\"\n");
    bad_pkgmanifest("package manifest invalid module name",
        "format = 1\n[package]\nname=\"x\"\nversion=\"1\"\nmodule=\"1bad\"\n");
    bad_pkgmanifest("package manifest unknown key",
        "format = 1\n[package]\nname=\"x\"\nversion=\"1\"\nmodule=\"x\"\nhook=\"y\"\n");
}

static void test_validators(void) {
    CHECK(pkg_validate_package_name("acme.json", 0, NULL), "valid pkg name acme.json");
    CHECK(pkg_validate_package_name("a-b.c", 0, NULL), "valid pkg name a-b.c");
    CHECK(!pkg_validate_package_name("", 0, NULL), "empty pkg name rejected");
    CHECK(!pkg_validate_package_name(".x", 0, NULL), "leading dot rejected");
    CHECK(!pkg_validate_package_name("x.", 0, NULL), "trailing dot rejected");
    CHECK(!pkg_validate_package_name("a..b", 0, NULL), "double dot rejected");
    CHECK(!pkg_validate_package_name("a/b", 0, NULL), "slash rejected");
    CHECK(!pkg_validate_package_name("a b", 0, NULL), "space rejected");
    CHECK(!pkg_validate_package_name("Abc", 0, NULL), "uppercase rejected");

    CHECK(pkg_validate_module_name("acme.json", 0, NULL), "valid module acme.json");
    CHECK(pkg_validate_module_name("example.tools", 0, NULL), "valid module example.tools");
    CHECK(!pkg_validate_module_name("1bad", 0, NULL), "module starting with digit rejected");
    CHECK(!pkg_validate_module_name("a..b", 0, NULL), "module empty segment rejected");
    CHECK(!pkg_validate_module_name("a.B", 0, NULL), "module uppercase rejected");

    CHECK(pkg_is_full_sha("4c2e5ad3d8e74f3f239d6b0d6c7ab2e5e7b8d910"), "full sha accepted");
    CHECK(!pkg_is_full_sha("4C2E5AD3D8E74F3F239D6B0D6C7AB2E5E7B8D910"), "uppercase sha rejected");
    CHECK(!pkg_is_full_sha("4c2e5ad"), "short sha rejected");
}

static void test_source(void) {
    PkgError err; pkg_error_init(&err);
    PkgSource s;
    CHECK(pkg_source_parse("github:acme-labs/myon-json@4c2e5ad3d8e74f3f239d6b0d6c7ab2e5e7b8d910", 0, &s, &err),
          "source parse ok");
    CHECK(strcmp(s.owner, "acme-labs") == 0, "source owner");
    CHECK(strcmp(s.repo, "myon-json") == 0, "source repo");
    char *url = pkg_source_archive_url(&s);
    CHECK(strcmp(url, "https://codeload.github.com/acme-labs/myon-json/zip/4c2e5ad3d8e74f3f239d6b0d6c7ab2e5e7b8d910") == 0,
          "archive url built correctly");
    free(url);
    char *cn = pkg_source_canonical_no_sha(&s);
    CHECK(strcmp(cn, "github:acme-labs/myon-json") == 0, "canonical no-sha");
    free(cn);
    pkg_source_reset(&s);
    pkg_error_reset(&err);

    CHECK(!pkg_source_parse("gitlab:a/b@x", 0, &s, &err), "non-github prefix rejected");
    pkg_source_reset(&s); pkg_error_reset(&err);
    CHECK(!pkg_source_parse("github:a@x", 0, &s, &err), "missing repo rejected");
    pkg_source_reset(&s); pkg_error_reset(&err);
}

static void test_install_url(void) {
    PkgError err; pkg_error_init(&err);
    PkgInstallUrl u;

    CHECK(pkg_install_url_parse("https://github.com/owner/repo", &u, &err), "plain url");
    CHECK(u.ref_kind == PKG_REF_DEFAULT, "plain url -> default ref");
    CHECK(strcmp(u.owner, "owner") == 0 && strcmp(u.repo, "repo") == 0, "owner/repo");
    pkg_install_url_reset(&u); pkg_error_reset(&err);

    CHECK(pkg_install_url_parse("https://github.com/owner/repo.git", &u, &err), ".git url");
    CHECK(strcmp(u.repo, "repo") == 0, ".git stripped");
    pkg_install_url_reset(&u); pkg_error_reset(&err);

    CHECK(pkg_install_url_parse("https://github.com/o/r/tree/main", &u, &err), "tree branch url");
    CHECK(u.ref_kind == PKG_REF_BRANCH && strcmp(u.ref, "main") == 0, "branch ref");
    pkg_install_url_reset(&u); pkg_error_reset(&err);

    CHECK(pkg_install_url_parse("https://github.com/o/r/tree/4c2e5ad3d8e74f3f239d6b0d6c7ab2e5e7b8d910", &u, &err),
          "tree commit url");
    CHECK(u.ref_kind == PKG_REF_COMMIT, "commit ref classified");
    pkg_install_url_reset(&u); pkg_error_reset(&err);

    CHECK(pkg_install_url_parse("https://github.com/o/r/releases/tag/v1.0.0", &u, &err), "releases/tag url");
    CHECK(u.ref_kind == PKG_REF_TAG && strcmp(u.ref, "v1.0.0") == 0, "tag ref");
    pkg_install_url_reset(&u); pkg_error_reset(&err);

    CHECK(!pkg_install_url_parse("http://github.com/o/r", &u, &err), "http rejected");
    pkg_install_url_reset(&u); pkg_error_reset(&err);
    CHECK(!pkg_install_url_parse("https://gitlab.com/o/r", &u, &err), "non-github host rejected");
    pkg_install_url_reset(&u); pkg_error_reset(&err);
    CHECK(!pkg_install_url_parse("https://user:pass@github.com/o/r", &u, &err), "credentials rejected");
    pkg_install_url_reset(&u); pkg_error_reset(&err);
    CHECK(!pkg_install_url_parse("https://github.com/o", &u, &err), "missing repo rejected");
    pkg_install_url_reset(&u); pkg_error_reset(&err);
    CHECK(!pkg_install_url_parse("https://github.com/o/r?x=1", &u, &err), "query rejected");
    pkg_install_url_reset(&u); pkg_error_reset(&err);
}

static const char *LOCK_TEXT =
    "format = 1\n"
    "\n[[package]]\n"
    "name = \"acme.json\"\nversion = \"1.2.0\"\nmodule = \"acme.json\"\n"
    "source = \"github:acme-labs/myon-json\"\n"
    "revision = \"4c2e5ad3d8e74f3f239d6b0d6c7ab2e5e7b8d910\"\n"
    "archive = \"https://codeload.github.com/acme-labs/myon-json/zip/4c2e5ad3d8e74f3f239d6b0d6c7ab2e5e7b8d910\"\n"
    "sha256 = \"0000000000000000000000000000000000000000000000000000000000000000\"\n"
    "dependencies = \"acme.text\"\n"
    "\n[[package]]\n"
    "name = \"acme.text\"\nversion = \"0.9.0\"\nmodule = \"acme.text\"\n"
    "source = \"github:acme-labs/myon-text\"\n"
    "revision = \"0d76ff2717d93655c5d95f00b4f0cfca0e5b0a22\"\n"
    "archive = \"https://codeload.github.com/acme-labs/myon-text/zip/0d76ff2717d93655c5d95f00b4f0cfca0e5b0a22\"\n"
    "sha256 = \"1111111111111111111111111111111111111111111111111111111111111111\"\n"
    "dependencies = \"\"\n";

static void test_lock(void) {
    PkgError err; pkg_error_init(&err);
    PkgLock *l = pkg_lock_parse(LOCK_TEXT, &err);
    CHECK(l != NULL, "lock parse ok");
    if (l) {
        CHECK(l->count == 2, "two locked packages");
        const PkgLockEntry *e = pkg_lock_find(l, "acme.json");
        CHECK(e != NULL, "find acme.json");
        CHECK(e && e->dep_count == 1 && strcmp(e->deps[0], "acme.text") == 0, "dep list parsed");

        /* Deterministic round-trip: render then re-parse then re-render must
         * be byte-identical. */
        char *r1 = pkg_lock_render(l);
        PkgLock *l2 = pkg_lock_parse(r1, &err);
        CHECK(l2 != NULL, "re-parse rendered lock");
        char *r2 = l2 ? pkg_lock_render(l2) : NULL;
        CHECK(r2 && strcmp(r1, r2) == 0, "deterministic lock round-trip");
        free(r1); free(r2);
        pkg_lock_free(l2);
    }
    pkg_lock_free(l);
    pkg_error_reset(&err);

    /* Deterministic ordering: entries inserted out of order come out sorted. */
    PkgLock *l3 = pkg_lock_new();
    PkgLockEntry z; memset(&z, 0, sizeof z);
    z.name = strdup("zeta"); z.version = strdup("1"); z.module = strdup("zeta");
    z.source.owner = strdup("o"); z.source.repo = strdup("r");
    memcpy(z.source.sha, "0d76ff2717d93655c5d95f00b4f0cfca0e5b0a22", 41);
    memcpy(z.sha256, "1111111111111111111111111111111111111111111111111111111111111111", 65);
    CHECK(pkg_lock_upsert(l3, &z, &err), "upsert zeta");
    free(z.name); free(z.version); free(z.module); free(z.source.owner); free(z.source.repo);

    PkgLockEntry a; memset(&a, 0, sizeof a);
    a.name = strdup("alpha"); a.version = strdup("1"); a.module = strdup("alpha");
    a.source.owner = strdup("o"); a.source.repo = strdup("r");
    memcpy(a.source.sha, "4c2e5ad3d8e74f3f239d6b0d6c7ab2e5e7b8d910", 41);
    memcpy(a.sha256, "2222222222222222222222222222222222222222222222222222222222222222", 65);
    CHECK(pkg_lock_upsert(l3, &a, &err), "upsert alpha");
    free(a.name); free(a.version); free(a.module); free(a.source.owner); free(a.source.repo);

    CHECK(l3->count == 2 && strcmp(l3->entries[0].name, "alpha") == 0, "entries sorted by name");
    pkg_lock_free(l3);
    pkg_error_reset(&err);

    /* Conflicting revisions for the same name => error. */
    PkgLock *l4 = pkg_lock_new();
    PkgLockEntry c1; memset(&c1, 0, sizeof c1);
    c1.name = strdup("dup"); c1.version = strdup("1"); c1.module = strdup("dup");
    c1.source.owner = strdup("o"); c1.source.repo = strdup("r");
    memcpy(c1.source.sha, "4c2e5ad3d8e74f3f239d6b0d6c7ab2e5e7b8d910", 41);
    memcpy(c1.sha256, "3333333333333333333333333333333333333333333333333333333333333333", 65);
    CHECK(pkg_lock_upsert(l4, &c1, &err), "upsert dup v1");
    PkgLockEntry c2 = c1;
    char sha2[] = "0d76ff2717d93655c5d95f00b4f0cfca0e5b0a22";
    memcpy(c2.source.sha, sha2, 41);
    pkg_error_reset(&err);
    CHECK(!pkg_lock_upsert(l4, &c2, &err), "conflicting revision rejected");
    free(c1.name); free(c1.version); free(c1.module); free(c1.source.owner); free(c1.source.repo);
    pkg_lock_free(l4);
    pkg_error_reset(&err);

    /* Bad lockfiles. */
    PkgLock *bad;
    bad = pkg_lock_parse("format = 1\n[[package]]\nname=\"x\"\n", &err);
    CHECK(bad == NULL, "lock missing source/revision rejected");
    pkg_lock_free(bad); pkg_error_reset(&err);

    bad = pkg_lock_parse(
        "format = 1\n[[package]]\nname=\"x\"\nversion=\"1\"\nmodule=\"x\"\n"
        "source=\"github:o/r\"\nrevision=\"main\"\n"
        "archive=\"https://codeload.github.com/o/r/zip/main\"\n"
        "sha256=\"0000000000000000000000000000000000000000000000000000000000000000\"\n"
        "dependencies=\"\"\n", &err);
    CHECK(bad == NULL, "lock non-sha revision rejected");
    pkg_lock_free(bad); pkg_error_reset(&err);

    bad = pkg_lock_parse(
        "format = 1\n[[package]]\nname=\"x\"\nversion=\"1\"\nmodule=\"x\"\n"
        "source=\"github:o/r\"\nrevision=\"4c2e5ad3d8e74f3f239d6b0d6c7ab2e5e7b8d910\"\n"
        "archive=\"https://codeload.github.com/o/r/zip/4c2e5ad3d8e74f3f239d6b0d6c7ab2e5e7b8d910\"\n"
        "sha256=\"short\"\n"
        "dependencies=\"\"\n", &err);
    CHECK(bad == NULL, "lock bad sha256 rejected");
    pkg_lock_free(bad); pkg_error_reset(&err);
}

static void test_manifest_lock_match(void) {
    PkgError err; pkg_error_init(&err);
    PkgManifest *m = pkg_manifest_parse(
        "format = 1\n[project]\nname=\"app\"\nversion=\"1\"\n[dependencies]\n"
        "acme.json = \"github:acme-labs/myon-json@4c2e5ad3d8e74f3f239d6b0d6c7ab2e5e7b8d910\"\n", &err);
    CHECK(m != NULL, "manifest for match test");
    PkgLock *l = pkg_lock_parse(LOCK_TEXT, &err);
    CHECK(l != NULL, "lock for match test");
    CHECK(pkg_lock_check_matches_manifest(l, m, &err), "manifest/lock consistent");

    /* Mismatch: change manifest sha. */
    pkg_error_reset(&err);
    PkgManifest *m2 = pkg_manifest_parse(
        "format = 1\n[project]\nname=\"app\"\nversion=\"1\"\n[dependencies]\n"
        "acme.json = \"github:acme-labs/myon-json@0d76ff2717d93655c5d95f00b4f0cfca0e5b0a22\"\n", &err);
    CHECK(!pkg_lock_check_matches_manifest(l, m2, &err), "manifest/lock revision mismatch detected");

    pkg_manifest_free(m); pkg_manifest_free(m2); pkg_lock_free(l);
    pkg_error_reset(&err);
}

static void test_shorthand(void) {
    /* Recognition. */
    CHECK(pkg_arg_is_shorthand("acme/json"), "shorthand: owner/repo recognised");
    CHECK(pkg_arg_is_shorthand("acme/json.git"), "shorthand: .git suffix recognised");
    CHECK(!pkg_arg_is_shorthand("https://github.com/acme/json"), "shorthand: URL not a shorthand");
    CHECK(!pkg_arg_is_shorthand("acme"), "shorthand: bare name rejected");
    CHECK(!pkg_arg_is_shorthand("acme/json/extra"), "shorthand: too many segments rejected");
    CHECK(!pkg_arg_is_shorthand("acme/"), "shorthand: trailing slash rejected");
    CHECK(!pkg_arg_is_shorthand("/json"), "shorthand: leading slash rejected");
    CHECK(!pkg_arg_is_shorthand("acme json"), "shorthand: whitespace rejected");

    /* Parse + validate. */
    PkgError err; pkg_error_init(&err);
    PkgShorthand sh;
    CHECK(pkg_shorthand_parse("acme/json", &sh, &err), "shorthand parse ok");
    CHECK(sh.owner && strcmp(sh.owner, "acme") == 0, "shorthand owner");
    CHECK(sh.repo && strcmp(sh.repo, "json") == 0, "shorthand repo");
    pkg_shorthand_reset(&sh);

    CHECK(pkg_shorthand_parse("acme/json.git", &sh, &err), "shorthand parse strips .git");
    CHECK(sh.repo && strcmp(sh.repo, "json") == 0, "shorthand repo without .git");
    pkg_shorthand_reset(&sh);

    pkg_error_reset(&err);
    CHECK(!pkg_shorthand_parse("acme/..", &sh, &err), "shorthand rejects '..'");
    pkg_shorthand_reset(&sh);
    pkg_error_reset(&err);
}

static void test_registry_json(void) {
    PkgError err; pkg_error_init(&err);

    /* Array form. */
    PkgRegistry *r = pkg_registry_parse(
        "[\n  \"acme/myon-json\",\n  \"owner/pkg\"\n]\n", &err);
    CHECK(r != NULL, "registry: array form parses");
    if (r) {
        CHECK(r->count == 2, "registry: two entries");
        CHECK(pkg_registry_find(r, "acme", "myon-json") != NULL, "registry: find acme/myon-json");
        CHECK(pkg_registry_find(r, "owner", "pkg") != NULL, "registry: find owner/pkg");
        CHECK(pkg_registry_find(r, "no", "such") == NULL, "registry: miss");
        pkg_registry_free(r);
    }
    pkg_error_reset(&err);

    /* Object (alias) form. */
    r = pkg_registry_parse(
        "{ \"json\": \"acme/myon-json\", \"text\": \"acme/myon-text\" }", &err);
    CHECK(r != NULL, "registry: object form parses");
    if (r) {
        CHECK(r->count == 2, "registry: object two entries");
        const PkgRegistryEntry *e = pkg_registry_find(r, "acme", "myon-json");
        CHECK(e != NULL, "registry: object find by owner/repo");
        CHECK(e && e->alias && strcmp(e->alias, "json") == 0, "registry: alias captured");
        /* alias-only lookup */
        CHECK(pkg_registry_find(r, NULL, "text") != NULL, "registry: find by alias");
        pkg_registry_free(r);
    }
    pkg_error_reset(&err);

    /* Escapes + empty array. */
    r = pkg_registry_parse("[]", &err);
    CHECK(r != NULL && r->count == 0, "registry: empty array ok");
    pkg_registry_free(r);
    pkg_error_reset(&err);

    /* Rejections. */
    CHECK(pkg_registry_parse("", &err) == NULL, "registry: empty doc rejected");
    pkg_error_reset(&err);
    CHECK(pkg_registry_parse("[\"nowhere\"]", &err) == NULL, "registry: non-shorthand value rejected");
    pkg_error_reset(&err);
    CHECK(pkg_registry_parse("[\"a/b\", 3]", &err) == NULL, "registry: non-string value rejected");
    pkg_error_reset(&err);
    CHECK(pkg_registry_parse("[\"a/b\"", &err) == NULL, "registry: unterminated array rejected");
    pkg_error_reset(&err);
    CHECK(pkg_registry_parse("42", &err) == NULL, "registry: scalar top-level rejected");
    pkg_error_reset(&err);
    CHECK(pkg_registry_parse("[\"a/b\"] junk", &err) == NULL, "registry: trailing data rejected");
    pkg_error_reset(&err);
}

static void test_packages_list(void) {
    PkgError err; pkg_error_init(&err);
    char **urls = NULL;
    long n = pkg_packages_list_parse(
        "# my registries\n"
        "https://example.com/xxxx.json\n"
        "\n"
        "   https://ohmygodwhhhhooooo.com/xxxx.json   \n"
        "# trailing comment\n", &urls, &err);
    CHECK(n == 2, "packages.list: two URLs parsed");
    if (n == 2) {
        CHECK(strcmp(urls[0], "https://example.com/xxxx.json") == 0, "packages.list: first URL");
        CHECK(strcmp(urls[1], "https://ohmygodwhhhhooooo.com/xxxx.json") == 0, "packages.list: second URL (trimmed)");
    }
    for (long i = 0; i < n; i++) free(urls[i]);
    free(urls);
    pkg_error_reset(&err);

    /* Non-https rejected. */
    urls = NULL;
    long m = pkg_packages_list_parse("http://insecure.example/x.json\n", &urls, &err);
    CHECK(m == -1, "packages.list: http:// rejected");
    for (long i = 0; i < (m > 0 ? m : 0); i++) free(urls[i]);
    free(urls);
    pkg_error_reset(&err);

    /* Empty / comment-only file. */
    urls = NULL;
    long z = pkg_packages_list_parse("# only a comment\n\n", &urls, &err);
    CHECK(z == 0 && urls == NULL, "packages.list: comment-only yields no URLs");
    free(urls);
    pkg_error_reset(&err);
}

int main(void) {
    test_manifests();
    test_pkgmanifest();
    test_validators();
    test_source();
    test_install_url();
    test_lock();
    test_manifest_lock_match();
    test_shorthand();
    test_registry_json();
    test_packages_list();

    printf("pkg unit tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
