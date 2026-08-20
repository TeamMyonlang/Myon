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
 * Integration tests for the package-manager network-driven operations
 * (spec §4, §5, §11.2): the recursive resolver plus `pkg lock`,
 * `pkg install` (from lockfile) and `pkg install <github-url>`.
 *
 * These run entirely OFFLINE.  A canned PkgTransport (installed with
 * pkg_ops_set_transport) serves in-memory responses for the two endpoints the
 * pipeline touches:
 *
 *     GET api.github.com  /repos/<owner>/<repo>/commits/<ref>   -> 40-hex SHA
 *     GET codeload.github.com /<owner>/<repo>/zip/<sha>          -> a ZIP body
 *
 * The ZIP bodies are built in memory with a minimal stored-entry writer (the
 * same technique as pkg_zip_tests.c), so no external `zip`/`git`/network is
 * required — exactly as spec §11.2 mandates ("外部 GitHub への実通信を必須 CI
 * にしてはならない").
 *
 * Each test runs inside its own fresh temp project directory (chdir), because
 * the ops layer discovers the project root from the current working directory.
 */

#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#  define _POSIX_C_SOURCE 200809L
#endif

#include "../src/package.h"
#include "../src/pkg_fetch.h"
#include "../src/pkg_fs.h"
#include "../src/pkg_hash.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/stat.h>

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) g_pass++; \
    else { g_fail++; fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); } \
} while (0)

/* ================================================================== */
/* minimal stored-ZIP writer (same layout as pkg_zip_tests.c)          */
/* ================================================================== */

static uint32_t crc_tab[256];
static void crc_init(void){ for(uint32_t i=0;i<256;i++){uint32_t c=i;for(int k=0;k<8;k++)c=(c&1)?(0xedb88320u^(c>>1)):(c>>1);crc_tab[i]=c;} }
static uint32_t crc32b(const unsigned char*p,size_t n){uint32_t c=0xffffffffu;for(size_t i=0;i<n;i++)c=crc_tab[(c^p[i])&0xff]^(c>>8);return c^0xffffffffu;}

typedef struct { unsigned char *d; size_t len, cap; } Blob;
static void bput(Blob*b,const void*p,size_t n){ if(b->len+n>b->cap){b->cap=(b->len+n)*2+64;b->d=realloc(b->d,b->cap);} memcpy(b->d+b->len,p,n); b->len+=n; }
static void b16(Blob*b,uint16_t v){unsigned char t[2]={(unsigned char)(v&0xff),(unsigned char)(v>>8)};bput(b,t,2);}
static void b32(Blob*b,uint32_t v){unsigned char t[4]={(unsigned char)(v&0xff),(unsigned char)((v>>8)&0xff),(unsigned char)((v>>16)&0xff),(unsigned char)((v>>24)&0xff)};bput(b,t,4);}

typedef struct { const char *name; const char *data; } ZFile;

/* Build a stored ZIP whose single top-level directory is `root`; every file's
 * name is prefixed with "<root>/".  Returns a malloc'd buffer (caller frees). */
static unsigned char *build_pkg_zip(const char *root, const ZFile *files,
                                    size_t n, size_t *out_len) {
    crc_init();
    Blob body; memset(&body,0,sizeof(body));
    /* entries: the root dir marker + each file, all under "<root>/". */
    size_t total = n + 1;
    char **names = calloc(total, sizeof(char*));
    const char **datas = calloc(total, sizeof(char*));
    {
        size_t rl = strlen(root);
        names[0] = malloc(rl + 2); memcpy(names[0], root, rl); names[0][rl]='/'; names[0][rl+1]='\0';
        datas[0] = NULL;
        for (size_t i=0;i<n;i++){
            size_t fl = strlen(files[i].name);
            names[i+1] = malloc(rl + 1 + fl + 1);
            memcpy(names[i+1], root, rl); names[i+1][rl]='/';
            memcpy(names[i+1]+rl+1, files[i].name, fl); names[i+1][rl+1+fl]='\0';
            datas[i+1] = files[i].data;
        }
    }
    uint32_t *offsets = calloc(total, sizeof(uint32_t));
    uint32_t *crcs = calloc(total, sizeof(uint32_t));
    uint32_t *sizes = calloc(total, sizeof(uint32_t));
    for (size_t i=0;i<total;i++){
        offsets[i]=(uint32_t)body.len;
        size_t dlen = datas[i] ? strlen(datas[i]) : 0;
        crcs[i]=dlen?crc32b((const unsigned char*)datas[i],dlen):0;
        sizes[i]=(uint32_t)dlen;
        b32(&body,0x04034b50u);
        b16(&body,20); b16(&body,0); b16(&body,0);
        b16(&body,0); b16(&body,0);
        b32(&body,crcs[i]); b32(&body,sizes[i]); b32(&body,sizes[i]);
        b16(&body,(uint16_t)strlen(names[i])); b16(&body,0);
        bput(&body,names[i],strlen(names[i]));
        if(dlen)bput(&body,datas[i],dlen);
    }
    uint32_t cd_start=(uint32_t)body.len;
    for (size_t i=0;i<total;i++){
        b32(&body,0x02014b50u);
        b16(&body,20); b16(&body,20); b16(&body,0); b16(&body,0);
        b16(&body,0); b16(&body,0);
        b32(&body,crcs[i]); b32(&body,sizes[i]); b32(&body,sizes[i]);
        b16(&body,(uint16_t)strlen(names[i])); b16(&body,0); b16(&body,0);
        b16(&body,0); b16(&body,0);
        b32(&body,0);
        b32(&body,offsets[i]);
        bput(&body,names[i],strlen(names[i]));
    }
    uint32_t cd_size=(uint32_t)body.len-cd_start;
    b32(&body,0x06054b50u);
    b16(&body,0); b16(&body,0);
    b16(&body,(uint16_t)total); b16(&body,(uint16_t)total);
    b32(&body,cd_size); b32(&body,cd_start);
    b16(&body,0);
    for (size_t i=0;i<total;i++) free(names[i]);
    free(names); free(datas); free(offsets); free(crcs); free(sizes);
    *out_len=body.len;
    return body.d;
}

/* Build a package.myon body. */
static char *pkg_manifest_text(const char *name, const char *version,
                               const char *module, const ZFile *deps, size_t ndeps) {
    /* deps here reuse ZFile: .name = dep name, .data = "github:o/r@sha" */
    Blob b; memset(&b,0,sizeof(b));
    char hdr[512];
    int hn = snprintf(hdr,sizeof(hdr),
        "format = 1\n\n[package]\nname = \"%s\"\nversion = \"%s\"\nmodule = \"%s\"\n",
        name, version, module);
    bput(&b, hdr, (size_t)hn);
    if (ndeps) {
        bput(&b, "\n[dependencies]\n", 16);
        for (size_t i=0;i<ndeps;i++){
            char line[512];
            int ln = snprintf(line,sizeof(line),"%s = \"%s\"\n", deps[i].name, deps[i].data);
            bput(&b, line, (size_t)ln);
        }
    }
    char z = '\0'; bput(&b,&z,1);
    return (char*)b.d;
}

/* ================================================================== */
/* canned repository registry (in-memory "GitHub")                     */
/* ================================================================== */

typedef struct {
    char     owner[64];
    char     repo[64];
    char     ref[80];     /* the mutable ref the resolver will ask for */
    char     sha[41];     /* the commit SHA that ref resolves to       */
    unsigned char *zip;   /* archive body for /zip/<sha>               */
    size_t   zip_len;
    bool     corrupt_zip; /* if true, flip a byte so hash won't match a lock */
} Repo;

#define MAX_REPOS 16
static Repo g_repos[MAX_REPOS];
static size_t g_nrepos = 0;

static Repo *repo_add(const char *owner, const char *repo, const char *ref,
                      const char *sha) {
    Repo *r = &g_repos[g_nrepos++];
    memset(r, 0, sizeof(*r));
    snprintf(r->owner, sizeof(r->owner), "%s", owner);
    snprintf(r->repo, sizeof(r->repo), "%s", repo);
    snprintf(r->ref, sizeof(r->ref), "%s", ref);
    snprintf(r->sha, sizeof(r->sha), "%s", sha);
    return r;
}

static void repos_reset(void) {
    for (size_t i=0;i<g_nrepos;i++) free(g_repos[i].zip);
    memset(g_repos, 0, sizeof(g_repos));
    g_nrepos = 0;
}

static Repo *repo_find_by_sha(const char *sha) {
    for (size_t i=0;i<g_nrepos;i++) if (strncmp(g_repos[i].sha, sha, 40)==0) return &g_repos[i];
    return NULL;
}
static Repo *repo_find_by_ref(const char *owner, const char *repo, const char *ref) {
    for (size_t i=0;i<g_nrepos;i++)
        if (strcmp(g_repos[i].owner,owner)==0 && strcmp(g_repos[i].repo,repo)==0 &&
            strcmp(g_repos[i].ref,ref)==0) return &g_repos[i];
    return NULL;
}

/* ================================================================== */
/* mock transport                                                      */
/* ================================================================== */

typedef struct {
    char           host[128];
    int            port;
    unsigned char *resp;   /* full HTTP response, built on write()      */
    size_t         resp_len, resp_off;
} MockHandle;

static void *mock_connect(const char *host, int port, char **err_msg, void *ctx) {
    (void)err_msg; (void)ctx;
    MockHandle *h = calloc(1, sizeof(MockHandle));
    snprintf(h->host, sizeof(h->host), "%s", host);
    h->port = port;
    return h;
}

/* Build an HTTP/1.1 200 response with a binary body + Content-Length. */
static void mock_set_response_200(MockHandle *h, const unsigned char *body, size_t blen) {
    char hdr[128];
    int hn = snprintf(hdr, sizeof(hdr),
        "HTTP/1.1 200 OK\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n", blen);
    h->resp = malloc((size_t)hn + blen);
    memcpy(h->resp, hdr, (size_t)hn);
    if (blen) memcpy(h->resp + hn, body, blen);
    h->resp_len = (size_t)hn + blen;
    h->resp_off = 0;
}

static void mock_set_response_status(MockHandle *h, int status, const char *reason) {
    char buf[128];
    int n = snprintf(buf, sizeof(buf),
        "HTTP/1.1 %d %s\r\nContent-Length: 0\r\nConnection: close\r\n\r\n", status, reason);
    h->resp = malloc((size_t)n);
    memcpy(h->resp, buf, (size_t)n);
    h->resp_len = (size_t)n;
    h->resp_off = 0;
}

/* Emit one pkt-line ("<4-hex-len><payload>") into a growable buffer. */
static void pktline(unsigned char **buf, size_t *n, const char *payload, size_t plen) {
    size_t total = plen + 4;
    char hdr[5];
    snprintf(hdr, sizeof(hdr), "%04zx", total);
    *buf = realloc(*buf, *n + total);
    memcpy(*buf + *n, hdr, 4);
    if (plen) memcpy(*buf + *n + 4, payload, plen);
    *n += total;
}

/*
 * Build a git smart-HTTP ref-advertisement body for the repo, exposing:
 *   - HEAD (with a symref=HEAD:refs/heads/main capability),
 *   - refs/heads/main and refs/heads/HEAD-alias pointing at the repo's sha,
 * so the resolver can map HEAD / "main" -> the canned commit SHA.
 */
static unsigned char *build_info_refs(const Repo *r, size_t *out_len) {
    unsigned char *buf = NULL; size_t n = 0;
    pktline(&buf, &n, "# service=git-upload-pack\n", strlen("# service=git-upload-pack\n"));
    /* flush after banner */
    buf = realloc(buf, n + 4); memcpy(buf + n, "0000", 4); n += 4;
    /* First ref line: "<sha> HEAD\0<capabilities incl. symref=HEAD:...>\n".
     * The NUL after "HEAD" is significant (it separates refname from caps), so
     * the line is assembled by hand rather than with snprintf. */
    {
        char l2[512]; size_t p = 0;
        memcpy(l2 + p, r->sha, 40); p += 40;
        l2[p++] = ' ';
        memcpy(l2 + p, "HEAD", 4); p += 4;
        l2[p++] = '\0';
        const char *caps = "multi_ack symref=HEAD:refs/heads/main agent=git/test\n";
        memcpy(l2 + p, caps, strlen(caps)); p += strlen(caps);
        pktline(&buf, &n, l2, p);
    }
    /* refs/heads/main -> sha */
    { char l[128]; int k = snprintf(l, sizeof(l), "%s refs/heads/main\n", r->sha); pktline(&buf, &n, l, (size_t)k); }
    /* final flush */
    buf = realloc(buf, n + 4); memcpy(buf + n, "0000", 4); n += 4;
    *out_len = n;
    return buf;
}

/* Optional registry document served from a fake registry host. */
static char g_registry_host[128] = {0};
static char g_registry_path[256] = {0};
static char g_registry_json[1024] = {0};

/* Parse "GET <path> HTTP/1.1" out of the request and dispatch. */
static bool mock_write(void *handle, const unsigned char *data, size_t len, char **err_msg) {
    (void)err_msg;
    MockHandle *h = (MockHandle*)handle;
    /* Extract the request path. */
    char req[4096];
    size_t take = len < sizeof(req)-1 ? len : sizeof(req)-1;
    memcpy(req, data, take); req[take]='\0';
    /* "GET <path> HTTP/1.1" */
    char path[2048] = {0};
    if (sscanf(req, "GET %2047s", path) != 1) { mock_set_response_status(h, 400, "Bad Request"); return true; }

    /* Fake package-registry host (arbitrary third-party host). */
    if (g_registry_host[0] && strcmp(h->host, g_registry_host) == 0) {
        if (strcmp(path, g_registry_path) == 0) {
            mock_set_response_200(h, (const unsigned char*)g_registry_json, strlen(g_registry_json));
        } else {
            mock_set_response_status(h, 404, "Not Found");
        }
        return true;
    }

    /* git smart-HTTP ref discovery on github.com. */
    if (strcmp(h->host, "github.com") == 0) {
        /* Expect: /<owner>/<repo>.git/info/refs?service=git-upload-pack
         * Parse owner/repo by hand (repo names may contain dots). */
        Repo *r = NULL;
        if (path[0] == '/') {
            const char *o = path + 1;
            const char *slash = strchr(o, '/');
            const char *suffix = strstr(path, ".git/info/refs");
            if (slash && suffix && suffix > slash) {
                char owner[128]={0}, repo[128]={0};
                size_t ol = (size_t)(slash - o);
                size_t rl = (size_t)(suffix - (slash + 1));
                if (ol < sizeof(owner) && rl < sizeof(repo)) {
                    memcpy(owner, o, ol); owner[ol]='\0';
                    memcpy(repo, slash + 1, rl); repo[rl]='\0';
                    for (size_t i=0;i<g_nrepos;i++)
                        if (strcmp(g_repos[i].owner,owner)==0 && strcmp(g_repos[i].repo,repo)==0) { r=&g_repos[i]; break; }
                }
            }
        }
        if (r) {
            size_t bl = 0; unsigned char *body = build_info_refs(r, &bl);
            mock_set_response_200(h, body, bl);
            free(body);
        } else {
            mock_set_response_status(h, 404, "Not Found");
        }
        return true;
    }

    if (strcmp(h->host, "api.github.com") == 0) {
        /* /repos/<owner>/<repo>/commits/<ref> */
        char owner[128]={0}, repo[128]={0}, ref[256]={0};
        if (sscanf(path, "/repos/%127[^/]/%127[^/]/commits/%255s", owner, repo, ref) == 3) {
            Repo *r = repo_find_by_ref(owner, repo, ref);
            if (r) { mock_set_response_200(h, (const unsigned char*)r->sha, 40); return true; }
        }
        mock_set_response_status(h, 404, "Not Found");
        return true;
    }
    if (strcmp(h->host, "codeload.github.com") == 0) {
        /* /<owner>/<repo>/zip/<sha> */
        char owner[128]={0}, repo[128]={0}, sha[128]={0};
        if (sscanf(path, "/%127[^/]/%127[^/]/zip/%127s", owner, repo, sha) == 3) {
            Repo *r = repo_find_by_sha(sha);
            if (r && r->zip) {
                if (r->corrupt_zip) {
                    unsigned char *tmp = malloc(r->zip_len);
                    memcpy(tmp, r->zip, r->zip_len);
                    tmp[r->zip_len/2] ^= 0xffu; /* only changes bytes -> hash differs */
                    mock_set_response_200(h, tmp, r->zip_len);
                    free(tmp);
                } else {
                    mock_set_response_200(h, r->zip, r->zip_len);
                }
                return true;
            }
        }
        mock_set_response_status(h, 404, "Not Found");
        return true;
    }
    mock_set_response_status(h, 404, "Not Found");
    return true;
}

static long mock_read(void *handle, unsigned char *buf, size_t cap, char **err_msg) {
    (void)err_msg;
    MockHandle *h = (MockHandle*)handle;
    if (h->resp_off >= h->resp_len) return 0; /* EOF */
    size_t remain = h->resp_len - h->resp_off;
    size_t take = remain < cap ? remain : cap;
    memcpy(buf, h->resp + h->resp_off, take);
    h->resp_off += take;
    return (long)take;
}

static void mock_close(void *handle) {
    MockHandle *h = (MockHandle*)handle;
    if (h) { free(h->resp); free(h); }
}

static PkgTransport g_mock = {
    mock_connect, mock_write, mock_read, mock_close, NULL
};

/* ================================================================== */
/* temp-project helpers                                                */
/* ================================================================== */

static char g_prev_cwd[4096];
static char g_proj[4096];

/* Create a fresh empty temp project dir and chdir into it. */
static void project_enter(const char *tag) {
    getcwd(g_prev_cwd, sizeof(g_prev_cwd));
    snprintf(g_proj, sizeof(g_proj), "/tmp/myon_opstest_%s_%d", tag, (int)getpid());
    pkg_fs_rmtree(g_proj, NULL);
    pkg_fs_mkdirs(g_proj, NULL);
    if (chdir(g_proj) != 0) { perror("chdir"); }
}

static void project_leave(void) {
    if (chdir(g_prev_cwd) != 0) { /* ignore */ }
    pkg_fs_rmtree(g_proj, NULL);
}

static void write_file(const char *rel, const char *text) {
    FILE *f = fopen(rel, "wb");
    if (!f) { perror(rel); return; }
    fwrite(text, 1, strlen(text), f);
    fclose(f);
}

static bool file_contains(const char *rel, const char *needle) {
    FILE *f = fopen(rel, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)n + 1);
    size_t got = fread(buf, 1, (size_t)n, f); buf[got] = '\0';
    fclose(f);
    bool r = strstr(buf, needle) != NULL;
    free(buf);
    return r;
}

/* Register a repo whose archive contains a valid package.myon (name/module)
 * and the matching module file, plus optional dependencies. */
static Repo *make_repo(const char *owner, const char *repo, const char *ref,
                       const char *sha, const char *pkg_name, const char *module,
                       const ZFile *deps, size_t ndeps) {
    Repo *r = repo_add(owner, repo, ref, sha);
    char *pm = pkg_manifest_text(pkg_name, "1.0.0", module, deps, ndeps);
    /* module "a.b" -> modules/a/b.myon */
    char modpath[256];
    snprintf(modpath, sizeof(modpath), "modules/%s.myon", module);
    for (char *q=modpath; *q; q++) if (*q=='.' && q>modpath+8) { /* skip: handled below */ }
    /* Replace dots in the module portion (after "modules/") with '/'. */
    {
        char *base = modpath + strlen("modules/");
        char *dot_myon = strstr(base, ".myon");
        for (char *q = base; q < dot_myon; q++) if (*q=='.') *q='/';
    }
    ZFile files[] = {
        {"package.myon", pm},
        {modpath, "let answer = 42\n"},
    };
    r->zip = build_pkg_zip(sha, files, 2, &r->zip_len);
    free(pm);
    return r;
}

/* ================================================================== */
/* tests                                                               */
/* ================================================================== */

/* full commit shas (40 hex) */
#define SHA_A "1111111111111111111111111111111111111111"
#define SHA_B "2222222222222222222222222222222222222222"
#define SHA_C "3333333333333333333333333333333333333333"

static void test_install_url_fresh(void) {
    project_enter("fresh");
    repos_reset();
    make_repo("acme", "json", "HEAD", SHA_A, "acme-json", "acme.json", NULL, 0);

    int rc = pkg_ops_install_url("https://github.com/acme/json");
    CHECK(rc == 0, "install <url>: fresh install succeeds");
    CHECK(pkg_fs_is_file("myon.toml"), "install: created myon.toml");
    CHECK(pkg_fs_is_file("myon.lock"), "install: created myon.lock");
    CHECK(pkg_fs_is_file(".myon/packages/acme-json/package.myon"), "install: promoted package");
    CHECK(pkg_fs_is_file(".myon/packages/acme-json/modules/acme/json.myon"), "install: module present");
    CHECK(file_contains("myon.toml", "acme-json"), "install: manifest lists dep");
    CHECK(file_contains("myon.toml", SHA_A), "install: manifest pins full sha");
    CHECK(file_contains("myon.lock", SHA_A), "install: lock pins full sha");
    /* the sha256 of the archive must appear in the lock */
    {
        Repo *r = repo_find_by_sha(SHA_A);
        char h[65]; pkg_sha256_hex(r->zip, r->zip_len, h);
        CHECK(file_contains("myon.lock", h), "install: lock records archive sha256");
    }
    project_leave();
}

static void test_lock_then_install(void) {
    project_enter("lockinstall");
    repos_reset();
    make_repo("acme", "json", "main", SHA_A, "acme-json", "acme.json", NULL, 0);

    /* Author a manifest by hand pinned to the full sha. */
    write_file("myon.toml",
        "format = 1\n\n[project]\nname = \"demo\"\nversion = \"0.1.0\"\n\n"
        "[dependencies]\nacme-json = \"github:acme/json@" SHA_A "\"\n");

    int rc = pkg_ops_lock();
    CHECK(rc == 0, "lock: succeeds against pinned manifest");
    CHECK(pkg_fs_is_file("myon.lock"), "lock: wrote myon.lock");
    CHECK(!pkg_fs_is_dir(".myon/packages/acme-json"), "lock: does not install");

    int rc2 = pkg_ops_install_locked();
    CHECK(rc2 == 0, "install (locked): succeeds");
    CHECK(pkg_fs_is_file(".myon/packages/acme-json/package.myon"), "install (locked): promoted");
    project_leave();
}

static void test_transitive_deps(void) {
    project_enter("transitive");
    repos_reset();
    /* root -> mid -> leaf */
    make_repo("z", "leaf", "main", SHA_C, "leaf-pkg", "z.leaf", NULL, 0);
    ZFile mid_deps[] = {{"leaf-pkg", "github:z/leaf@" SHA_C}};
    make_repo("y", "mid", "HEAD", SHA_B, "mid-pkg", "y.mid", mid_deps, 1);

    int rc = pkg_ops_install_url("https://github.com/y/mid");
    CHECK(rc == 0, "transitive: install succeeds");
    CHECK(pkg_fs_is_file(".myon/packages/mid-pkg/package.myon"), "transitive: mid installed");
    CHECK(pkg_fs_is_file(".myon/packages/leaf-pkg/package.myon"), "transitive: leaf installed");
    CHECK(file_contains("myon.lock", "leaf-pkg"), "transitive: leaf locked");
    CHECK(file_contains("myon.lock", "mid-pkg"), "transitive: mid locked");
    project_leave();
}

static void test_hash_mismatch_preserves_existing(void) {
    project_enter("hashmismatch");
    repos_reset();
    make_repo("acme", "json", "HEAD", SHA_A, "acme-json", "acme.json", NULL, 0);

    /* First: a clean install. */
    CHECK(pkg_ops_install_url("https://github.com/acme/json") == 0, "hashmismatch: initial install ok");
    CHECK(pkg_fs_is_file(".myon/packages/acme-json/package.myon"), "hashmismatch: package present");

    /* Now corrupt the served archive and reinstall from the (valid) lock:
     * the recorded sha256 will not match, install must fail with integrity
     * error AND leave the existing install intact (spec §3.4 rollback). */
    repo_find_by_sha(SHA_A)->corrupt_zip = true;
    int rc = pkg_ops_install_locked();
    CHECK(rc == 70, "hashmismatch: integrity failure exit code 70");
    CHECK(pkg_fs_is_file(".myon/packages/acme-json/package.myon"),
          "hashmismatch: existing install left intact after failed reinstall");
    project_leave();
}

static void test_reinstall_replaces(void) {
    project_enter("reinstall");
    repos_reset();
    make_repo("acme", "json", "HEAD", SHA_A, "acme-json", "acme.json", NULL, 0);
    CHECK(pkg_ops_install_url("https://github.com/acme/json") == 0, "reinstall: first ok");
    /* second install of the same thing must be idempotent, still ok. */
    CHECK(pkg_ops_install_locked() == 0, "reinstall: second (locked) ok");
    CHECK(pkg_fs_is_file(".myon/packages/acme-json/modules/acme/json.myon"),
          "reinstall: module still present");
    project_leave();
}

static void test_namespace_collision(void) {
    project_enter("nscollide");
    repos_reset();
    /* two different packages both claim module namespace "dup.mod" */
    make_repo("a", "one", "HEAD", SHA_A, "pkg-one", "dup.mod", NULL, 0);
    make_repo("b", "two", "main", SHA_B, "pkg-two", "dup.mod", NULL, 0);

    /* install first, then a manifest that pulls both -> collision on lock */
    CHECK(pkg_ops_install_url("https://github.com/a/one") == 0, "nscollide: first ok");
    write_file("myon.toml",
        "format = 1\n\n[project]\nname = \"demo\"\nversion = \"0.1.0\"\n\n"
        "[dependencies]\n"
        "pkg-one = \"github:a/one@" SHA_A "\"\n"
        "pkg-two = \"github:b/two@" SHA_B "\"\n");
    int rc = pkg_ops_lock();
    CHECK(rc != 0, "nscollide: lock rejects module namespace collision");
    project_leave();
}

static void test_missing_module_file(void) {
    project_enter("nomodule");
    repos_reset();
    /* archive declares module acme.json but ships no modules/acme/json.myon */
    Repo *r = repo_add("acme", "json", "HEAD", SHA_A);
    char *pm = pkg_manifest_text("acme-json", "1.0.0", "acme.json", NULL, 0);
    ZFile files[] = { {"package.myon", pm} };
    r->zip = build_pkg_zip(SHA_A, files, 1, &r->zip_len);
    free(pm);

    int rc = pkg_ops_install_url("https://github.com/acme/json");
    CHECK(rc == 65, "nomodule: manifest error (missing module file) exit 65");
    CHECK(!pkg_fs_is_dir(".myon/packages/acme-json"), "nomodule: nothing installed");
    project_leave();
}

static void test_name_mismatch(void) {
    project_enter("namemismatch");
    repos_reset();
    /* We'll ask GitHub for owner=acme repo=json but the package.myon inside
     * declares a name that disagrees with the resolver's dependency key.
     * install <url> reads the name from package.myon, so instead exercise the
     * lock path where the dep key is fixed by the manifest. */
    make_repo("acme", "json", "main", SHA_A, "totally-different", "acme.json", NULL, 0);
    write_file("myon.toml",
        "format = 1\n\n[project]\nname = \"demo\"\nversion = \"0.1.0\"\n\n"
        "[dependencies]\nacme-json = \"github:acme/json@" SHA_A "\"\n");
    int rc = pkg_ops_lock();
    CHECK(rc == 65, "namemismatch: lock rejects package.myon name != dep key");
    project_leave();
}

static void test_ref_not_found(void) {
    project_enter("refnotfound");
    repos_reset();
    /* no repo registered -> api returns 404 -> network error */
    int rc = pkg_ops_install_url("https://github.com/nobody/nothing");
    CHECK(rc == 69, "refnotfound: unknown repo -> network error exit 69");
    project_leave();
}

/* Ref resolution now goes through the git smart-HTTP endpoint on github.com
 * first (avoiding the api.github.com rate limit). Prove it resolves without
 * ever contacting api.github.com by only serving github.com/info/refs. */
static void test_git_protocol_resolution(void) {
    project_enter("gitproto");
    repos_reset();
    /* ref "main" is advertised by build_info_refs via refs/heads/main. */
    make_repo("acme", "json", "main", SHA_A, "acme-json", "acme.json", NULL, 0);

    char sha[41] = {0}; char *err = NULL;
    bool ok = pkg_fetch_resolve_ref(&g_mock, "acme", "json", "main", sha, &err);
    CHECK(ok, "gitproto: resolve 'main' via git smart-HTTP");
    CHECK(strcmp(sha, SHA_A) == 0, "gitproto: resolved to the advertised sha");
    free(err); err = NULL;

    /* default branch (NULL ref) resolves via the HEAD symref. */
    char sha2[41] = {0};
    ok = pkg_fetch_resolve_ref(&g_mock, "acme", "json", NULL, sha2, &err);
    CHECK(ok, "gitproto: resolve default branch via HEAD symref");
    CHECK(strcmp(sha2, SHA_A) == 0, "gitproto: default branch sha");
    free(err); err = NULL;

    /* a full SHA short-circuits with no network at all. */
    char sha3[41] = {0};
    ok = pkg_fetch_resolve_ref(&g_mock, "acme", "json", SHA_B, sha3, &err);
    CHECK(ok && strcmp(sha3, SHA_B) == 0, "gitproto: full sha returned as-is");
    free(err);
    project_leave();
}

/* End-to-end `myon pkg install <owner>/<repo>` via .myon/packages.list. */
static void test_install_shorthand(void) {
    project_enter("shorthand");
    repos_reset();
    make_repo("acme", "json", "main", SHA_A, "acme-json", "acme.json", NULL, 0);

    /* Point the mock registry host at a JSON array listing acme/json. */
    snprintf(g_registry_host, sizeof(g_registry_host), "registry.example");
    snprintf(g_registry_path, sizeof(g_registry_path), "/packages.json");
    snprintf(g_registry_json, sizeof(g_registry_json), "[\"acme/json\", \"other/thing\"]");

    /* Create .myon/packages.list with the registry URL. */
    pkg_fs_mkdirs(".myon", NULL);
    write_file(".myon/packages.list",
               "# my registries\nhttps://registry.example/packages.json\n");

    int rc = pkg_ops_install_shorthand("acme/json");
    CHECK(rc == 0, "shorthand: install acme/json succeeds");
    CHECK(pkg_fs_is_file(".myon/packages/acme-json/package.myon"), "shorthand: package promoted");
    CHECK(file_contains("myon.toml", SHA_A), "shorthand: manifest pins resolved sha");

    /* A shorthand not present in any registry -> usage error (exit 64). */
    int rc2 = pkg_ops_install_shorthand("nope/missing");
    CHECK(rc2 == 64, "shorthand: unknown package -> usage error 64");

    g_registry_host[0] = '\0'; g_registry_path[0]='\0'; g_registry_json[0]='\0';
    project_leave();
}

/* Shorthand install with no packages.list at all -> clear usage error. */
static void test_install_shorthand_no_list(void) {
    project_enter("nolist");
    repos_reset();
    int rc = pkg_ops_install_shorthand("acme/json");
    CHECK(rc == 64, "shorthand: missing packages.list -> usage error 64");
    project_leave();
}

int main(void) {
    pkg_ops_set_transport(&g_mock);

    test_install_url_fresh();
    test_lock_then_install();
    test_transitive_deps();
    test_hash_mismatch_preserves_existing();
    test_reinstall_replaces();
    test_namespace_collision();
    test_missing_module_file();
    test_name_mismatch();
    test_ref_not_found();
    test_git_protocol_resolution();
    test_install_shorthand();
    test_install_shorthand_no_list();

    pkg_ops_set_transport(NULL);
    repos_reset();

    fprintf(stderr, "pkg_ops tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
