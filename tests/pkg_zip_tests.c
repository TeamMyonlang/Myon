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
 * Unit tests for the security-first ZIP reader (spec §8, §11.3) and the
 * SHA-256 helper (spec §9).  These are pure and offline: they build tiny
 * "stored" (uncompressed) ZIP archives in memory, run pkg_zip_extract against
 * a temp staging directory, and assert on the accept/reject decision.
 *
 * A minimal ZIP writer is included so the tests do not depend on an external
 * `zip` tool; it emits method-0 (stored) entries, which exercises the whole
 * central-directory / local-header / path-validation / limit path (the DEFLATE
 * core is covered separately by the round-trip test in the ops harness).
 */

#include "../src/pkg_zip.h"
#include "../src/pkg_fs.h"
#include "../src/pkg_hash.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static int g_pass = 0, g_fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) g_pass++; \
    else { g_fail++; fprintf(stderr, "FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__); } \
} while (0)

/* ---- tiny CRC-32 (matches the reader) ---- */
static uint32_t crc_tab[256];
static void crc_init(void){ for(uint32_t i=0;i<256;i++){uint32_t c=i;for(int k=0;k<8;k++)c=(c&1)?(0xedb88320u^(c>>1)):(c>>1);crc_tab[i]=c;} }
static uint32_t crc32b(const unsigned char*p,size_t n){uint32_t c=0xffffffffu;for(size_t i=0;i<n;i++)c=crc_tab[(c^p[i])&0xff]^(c>>8);return c^0xffffffffu;}

/* ---- minimal stored-ZIP writer ---- */
typedef struct { unsigned char *d; size_t len, cap; } Blob;
static void bput(Blob*b,const void*p,size_t n){ if(b->len+n>b->cap){b->cap=(b->len+n)*2+64;b->d=realloc(b->d,b->cap);} memcpy(b->d+b->len,p,n); b->len+=n; }
static void b16(Blob*b,uint16_t v){unsigned char t[2]={(unsigned char)(v&0xff),(unsigned char)(v>>8)};bput(b,t,2);}
static void b32(Blob*b,uint32_t v){unsigned char t[4]={(unsigned char)(v&0xff),(unsigned char)((v>>8)&0xff),(unsigned char)((v>>16)&0xff),(unsigned char)((v>>24)&0xff)};bput(b,t,4);}

typedef struct { const char *name; const char *data; uint32_t external_attr; } ZipItem;

/* Build a stored ZIP; returns malloc'd buffer (caller frees), sets *out_len. */
static unsigned char *build_zip(const ZipItem *items, size_t n, size_t *out_len) {
    crc_init();
    Blob body; memset(&body,0,sizeof(body));
    /* local headers + data, remember offsets */
    uint32_t *offsets = calloc(n, sizeof(uint32_t));
    uint32_t *crcs = calloc(n, sizeof(uint32_t));
    uint32_t *sizes = calloc(n, sizeof(uint32_t));
    for (size_t i=0;i<n;i++){
        offsets[i]=(uint32_t)body.len;
        size_t dlen = items[i].data ? strlen(items[i].data) : 0;
        crcs[i]=dlen?crc32b((const unsigned char*)items[i].data,dlen):0;
        sizes[i]=(uint32_t)dlen;
        b32(&body,0x04034b50u); /* local file header sig */
        b16(&body,20); b16(&body,0); b16(&body,0); /* ver,flags,method=0 */
        b16(&body,0); b16(&body,0); /* time,date */
        b32(&body,crcs[i]); b32(&body,sizes[i]); b32(&body,sizes[i]);
        b16(&body,(uint16_t)strlen(items[i].name)); b16(&body,0);
        bput(&body,items[i].name,strlen(items[i].name));
        if(dlen)bput(&body,items[i].data,dlen);
    }
    uint32_t cd_start=(uint32_t)body.len;
    for (size_t i=0;i<n;i++){
        b32(&body,0x02014b50u); /* central dir sig */
        b16(&body,20); b16(&body,20); b16(&body,0); b16(&body,0);
        b16(&body,0); b16(&body,0);
        b32(&body,crcs[i]); b32(&body,sizes[i]); b32(&body,sizes[i]);
        b16(&body,(uint16_t)strlen(items[i].name)); b16(&body,0); b16(&body,0);
        b16(&body,0); b16(&body,0);
        b32(&body,items[i].external_attr);
        b32(&body,offsets[i]);
        bput(&body,items[i].name,strlen(items[i].name));
    }
    uint32_t cd_size=(uint32_t)body.len-cd_start;
    b32(&body,0x06054b50u); /* EOCD */
    b16(&body,0); b16(&body,0);
    b16(&body,(uint16_t)n); b16(&body,(uint16_t)n);
    b32(&body,cd_size); b32(&body,cd_start);
    b16(&body,0);
    free(offsets);free(crcs);free(sizes);
    *out_len=body.len;
    return body.d;
}

static const char *STAGE = "/tmp/myon_ziptest_stage";

static bool try_extract(const ZipItem *items, size_t n, char **root_out) {
    size_t zlen=0;
    unsigned char *z=build_zip(items,n,&zlen);
    pkg_fs_rmtree(STAGE,NULL);
    pkg_fs_mkdirs(STAGE,NULL);
    char *err=NULL,*root=NULL;
    bool ok=pkg_zip_extract(z,zlen,STAGE,&root,&err);
    if(root_out)*root_out=root; else free(root);
    free(err);free(z);
    return ok;
}

int main(void) {
    /* --- SHA-256 known-answer (empty string) --- */
    {
        char h[65];
        CHECK(pkg_sha256_hex((const unsigned char*)"",0,h),"sha256 empty ok");
        CHECK(strcmp(h,"e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855")==0,"sha256 empty KAT");
        CHECK(pkg_sha256_hex((const unsigned char*)"abc",3,h),"sha256 abc ok");
        CHECK(strcmp(h,"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad")==0,"sha256 abc KAT");
        CHECK(pkg_sha256_equal("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
                               "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"),"sha256 equal");
        CHECK(!pkg_sha256_equal("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad",
                                "aa7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"),"sha256 not equal");
    }

    /* --- valid GitHub-style archive: single root, package.myon + module --- */
    {
        ZipItem items[] = {
            {"root-abc/", NULL, 0},
            {"root-abc/package.myon", "format=1\n", 0},
            {"root-abc/modules/acme/json.myon", "let x = 1\n", 0},
        };
        char *root=NULL;
        CHECK(try_extract(items,3,&root),"valid archive extracts");
        CHECK(root && strcmp(root,"root-abc")==0,"root name reported");
        free(root);
        /* file present with root stripped */
        CHECK(pkg_fs_is_file("/tmp/myon_ziptest_stage/package.myon"),"package.myon extracted");
        CHECK(pkg_fs_is_file("/tmp/myon_ziptest_stage/modules/acme/json.myon"),"module extracted");
    }

    /* --- ZIP-Slip: ../escape --- */
    {
        ZipItem items[] = {
            {"root/", NULL, 0},
            {"root/../escape.myon", "x", 0},
        };
        CHECK(!try_extract(items,2,NULL),"reject ../ traversal");
    }
    /* --- absolute path --- */
    {
        ZipItem items[] = {{"/etc/passwd", "x", 0}};
        CHECK(!try_extract(items,1,NULL),"reject absolute path");
    }
    /* --- windows drive path --- */
    {
        ZipItem items[] = {{"C:/root/x", "x", 0}};
        CHECK(!try_extract(items,1,NULL),"reject drive-letter path");
    }
    /* --- backslash bypass --- */
    {
        ZipItem items[] = {{"root\\evil.myon", "x", 0}};
        CHECK(!try_extract(items,1,NULL),"reject backslash path");
    }
    /* --- multiple top-level roots --- */
    {
        ZipItem items[] = {
            {"a/package.myon","x",0},
            {"b/package.myon","x",0},
        };
        CHECK(!try_extract(items,2,NULL),"reject multiple roots");
    }
    /* --- duplicate normalized path --- */
    {
        ZipItem items[] = {
            {"r/f.myon","x",0},
            {"r/f.myon","y",0},
        };
        CHECK(!try_extract(items,2,NULL),"reject duplicate path");
    }
    /* --- symlink (S_IFLNK in external attrs upper 16 bits) --- */
    {
        ZipItem items[] = {
            {"r/link", "target", (uint32_t)(0120777u) << 16}, /* S_IFLNK */
        };
        CHECK(!try_extract(items,1,NULL),"reject symlink entry");
    }
    /* --- control byte in name --- */
    {
        ZipItem items[] = {{"r/a\x01b.myon","x",0}};
        CHECK(!try_extract(items,1,NULL),"reject control byte in name");
    }

    fprintf(stderr, "pkg_zip tests: %d passed, %d failed\n", g_pass, g_fail);
    pkg_fs_rmtree(STAGE,NULL);
    return g_fail ? 1 : 0;
}
