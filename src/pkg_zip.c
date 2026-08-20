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
 * Security-first, self-contained ZIP reader (spec §8).  See pkg_zip.h for the
 * threat model.  The DEFLATE core below is a compact, from-scratch inflate
 * (RFC 1951) so the package manager pulls in NO new external library.
 */

#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#  define _POSIX_C_SOURCE 200809L
#endif

#include "platform.h"
#include "pkg_zip.h"
#include "pkg_fs.h"
#include "common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ================================================================== */
/* CRC-32 (IEEE, used to verify each entry — spec §8 "不正な CRC")     */
/* ================================================================== */

static uint32_t crc32_table[256];
static bool     crc32_ready = false;

static void crc32_init(void) {
    for (uint32_t i = 0; i < 256; i++) {
        uint32_t c = i;
        for (int k = 0; k < 8; k++)
            c = (c & 1) ? (0xedb88320u ^ (c >> 1)) : (c >> 1);
        crc32_table[i] = c;
    }
    crc32_ready = true;
}

static uint32_t crc32_bytes(const unsigned char *p, size_t n) {
    if (!crc32_ready) crc32_init();
    uint32_t c = 0xffffffffu;
    for (size_t i = 0; i < n; i++)
        c = crc32_table[(c ^ p[i]) & 0xff] ^ (c >> 8);
    return c ^ 0xffffffffu;
}

/* ================================================================== */
/* DEFLATE (inflate) — RFC 1951, self-contained                        */
/* ================================================================== */

typedef struct {
    const unsigned char *src;
    size_t               src_len;
    size_t               pos;      /* byte position */
    unsigned             bitbuf;
    int                  bitcnt;
    unsigned char       *out;
    size_t               out_cap;
    size_t               out_len;
    bool                 error;
} Inflate;

static int inf_getbit(Inflate *s) {
    if (s->bitcnt == 0) {
        if (s->pos >= s->src_len) { s->error = true; return 0; }
        s->bitbuf = s->src[s->pos++];
        s->bitcnt = 8;
    }
    int b = s->bitbuf & 1;
    s->bitbuf >>= 1;
    s->bitcnt--;
    return b;
}

static unsigned inf_getbits(Inflate *s, int n) {
    unsigned v = 0;
    for (int i = 0; i < n; i++) v |= (unsigned)inf_getbit(s) << i;
    return v;
}

static void inf_out_byte(Inflate *s, unsigned char b) {
    if (s->out_len >= s->out_cap) { s->error = true; return; }
    s->out[s->out_len++] = b;
}

/* A canonical Huffman decoding table built from a list of code lengths. */
typedef struct {
    unsigned short counts[16];   /* number of codes of each length      */
    unsigned short symbols[288]; /* symbols sorted by code              */
} HuffTree;

static void huff_build(HuffTree *t, const unsigned char *lengths, int n) {
    memset(t->counts, 0, sizeof(t->counts));
    for (int i = 0; i < n; i++) t->counts[lengths[i]]++;
    t->counts[0] = 0;
    unsigned short offs[16];
    offs[0] = 0; offs[1] = 0;
    for (int i = 1; i < 15; i++) offs[i + 1] = (unsigned short)(offs[i] + t->counts[i]);
    for (int i = 0; i < n; i++)
        if (lengths[i]) t->symbols[offs[lengths[i]]++] = (unsigned short)i;
}

static int huff_decode(Inflate *s, const HuffTree *t) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len <= 15; len++) {
        code |= inf_getbit(s);
        int count = t->counts[len];
        if (code - first < count) return t->symbols[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
        if (s->error) return -1;
    }
    s->error = true;
    return -1;
}

static const unsigned short LEN_BASE[29] = {
    3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258};
static const unsigned char  LEN_EXTRA[29] = {
    0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
static const unsigned short DIST_BASE[30] = {
    1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
static const unsigned char  DIST_EXTRA[30] = {
    0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};

static bool inf_block(Inflate *s, const HuffTree *lt, const HuffTree *dt) {
    for (;;) {
        int sym = huff_decode(s, lt);
        if (s->error) return false;
        if (sym == 256) return true;               /* end of block */
        if (sym < 256) { inf_out_byte(s, (unsigned char)sym); if (s->error) return false; continue; }
        sym -= 257;
        if (sym >= 29) { s->error = true; return false; }
        int length = LEN_BASE[sym] + (int)inf_getbits(s, LEN_EXTRA[sym]);
        int dsym = huff_decode(s, dt);
        if (s->error || dsym < 0 || dsym >= 30) { s->error = true; return false; }
        int dist = DIST_BASE[dsym] + (int)inf_getbits(s, DIST_EXTRA[dsym]);
        if ((size_t)dist > s->out_len) { s->error = true; return false; }
        size_t from = s->out_len - (size_t)dist;
        for (int i = 0; i < length; i++) {
            inf_out_byte(s, s->out[from + (size_t)i]);
            if (s->error) return false;
        }
    }
}

static bool inf_fixed(Inflate *s) {
    HuffTree lt, dt;
    unsigned char ll[288], dl[30];
    for (int i = 0;   i < 144; i++) ll[i] = 8;
    for (int i = 144; i < 256; i++) ll[i] = 9;
    for (int i = 256; i < 280; i++) ll[i] = 7;
    for (int i = 280; i < 288; i++) ll[i] = 8;
    for (int i = 0;   i < 30;  i++) dl[i] = 5;
    huff_build(&lt, ll, 288);
    huff_build(&dt, dl, 30);
    return inf_block(s, &lt, &dt);
}

static bool inf_dynamic(Inflate *s) {
    static const unsigned char ORDER[19] =
        {16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};
    int hlit  = (int)inf_getbits(s, 5) + 257;
    int hdist = (int)inf_getbits(s, 5) + 1;
    int hclen = (int)inf_getbits(s, 4) + 4;
    if (s->error || hlit > 286 || hdist > 30) { s->error = true; return false; }

    unsigned char cl[19];
    memset(cl, 0, sizeof(cl));
    for (int i = 0; i < hclen; i++) cl[ORDER[i]] = (unsigned char)inf_getbits(s, 3);
    if (s->error) return false;
    HuffTree clt;
    huff_build(&clt, cl, 19);

    unsigned char lengths[288 + 30];
    int n = 0, total = hlit + hdist;
    while (n < total) {
        int sym = huff_decode(s, &clt);
        if (s->error || sym < 0) { s->error = true; return false; }
        if (sym < 16) { lengths[n++] = (unsigned char)sym; }
        else if (sym == 16) {
            if (n == 0) { s->error = true; return false; }
            int rep = 3 + (int)inf_getbits(s, 2);
            unsigned char prev = lengths[n - 1];
            while (rep-- && n < total) lengths[n++] = prev;
        } else if (sym == 17) {
            int rep = 3 + (int)inf_getbits(s, 3);
            while (rep-- && n < total) lengths[n++] = 0;
        } else if (sym == 18) {
            int rep = 11 + (int)inf_getbits(s, 7);
            while (rep-- && n < total) lengths[n++] = 0;
        } else { s->error = true; return false; }
        if (s->error) return false;
    }
    HuffTree lt, dt;
    huff_build(&lt, lengths, hlit);
    huff_build(&dt, lengths + hlit, hdist);
    return inf_block(s, &lt, &dt);
}

static bool inf_stored(Inflate *s) {
    /* Align to byte boundary. */
    s->bitcnt = 0;
    if (s->pos + 4 > s->src_len) { s->error = true; return false; }
    unsigned len  = (unsigned)s->src[s->pos] | ((unsigned)s->src[s->pos + 1] << 8);
    s->pos += 4; /* skip LEN + NLEN */
    if (s->pos + len > s->src_len) { s->error = true; return false; }
    for (unsigned i = 0; i < len; i++) {
        inf_out_byte(s, s->src[s->pos++]);
        if (s->error) return false;
    }
    return true;
}

unsigned char *pkg_zip_inflate(const unsigned char *src, size_t src_len,
                               size_t expected_out, int method) {
    unsigned char *out = myon_xmalloc(expected_out ? expected_out : 1);

    if (method == 0) { /* stored */
        if (src_len != expected_out) { free(out); return NULL; }
        if (expected_out) memcpy(out, src, expected_out);
        return out;
    }
    if (method != 8) { free(out); return NULL; }

    Inflate s;
    memset(&s, 0, sizeof(s));
    s.src = src; s.src_len = src_len; s.pos = 0;
    s.out = out; s.out_cap = expected_out; s.out_len = 0;

    for (;;) {
        int final = inf_getbit(&s);
        int type  = (int)inf_getbits(&s, 2);
        if (s.error) { free(out); return NULL; }
        bool ok;
        if (type == 0)      ok = inf_stored(&s);
        else if (type == 1) ok = inf_fixed(&s);
        else if (type == 2) ok = inf_dynamic(&s);
        else                { free(out); return NULL; }
        if (!ok || s.error) { free(out); return NULL; }
        if (final) break;
    }
    if (s.out_len != expected_out) { free(out); return NULL; }
    return out;
}

/* ================================================================== */
/* ZIP central-directory parsing                                       */
/* ================================================================== */

static uint32_t rd32(const unsigned char *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static uint16_t rd16(const unsigned char *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static void zerr(char **err_msg, const char *msg) {
    if (err_msg) *err_msg = myon_strdup(msg);
}

/*
 * Validate a ZIP entry name and split it into the top-level directory plus the
 * remainder.  Rejects every path hazard in spec §8.  On success returns true
 * and sets *root (heap copy of the first segment) and *rest (heap copy of the
 * path after the first '/', or NULL if the entry IS the top dir itself).
 */
static bool split_entry_name(const char *name, char **root, char **rest,
                             char **err_msg) {
    *root = NULL; *rest = NULL;
    size_t len = strlen(name);
    if (len == 0 || len > PKG_ZIP_MAX_NAME) { zerr(err_msg, "zip: bad entry name length"); return false; }
    if (name[0] == '/' )               { zerr(err_msg, "zip: absolute path entry rejected"); return false; }
    if (strchr(name, '\\'))            { zerr(err_msg, "zip: backslash in entry name rejected"); return false; }
    /* Windows drive path like "C:..." */
    if (len >= 2 && name[1] == ':')    { zerr(err_msg, "zip: drive-letter path rejected"); return false; }

    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)name[i];
        if (c < 0x20 || c == 0x7f) { zerr(err_msg, "zip: control byte in entry name"); return false; }
    }

    /* Split on '/', validating each component with the shared guard. */
    char *dup = myon_strdup(name);
    /* A trailing '/' marks a directory entry; drop it for splitting. */
    size_t dl = strlen(dup);
    bool is_dir = (dl > 0 && dup[dl - 1] == '/');
    if (is_dir) dup[dl - 1] = '\0';

    /* first segment */
    char *slash = strchr(dup, '/');
    char *first, *remainder;
    if (slash) { *slash = '\0'; first = dup; remainder = slash + 1; }
    else       { first = dup; remainder = NULL; }

    if (!pkg_fs_safe_component(first)) {
        free(dup); zerr(err_msg, "zip: unsafe top-level component"); return false;
    }
    /* validate each remaining component */
    if (remainder && *remainder) {
        char *save = myon_strdup(remainder);
        char *tok = save;
        char *nl;
        bool bad = false;
        for (;;) {
            nl = strchr(tok, '/');
            if (nl) *nl = '\0';
            if (*tok && !pkg_fs_safe_component(tok)) { bad = true; break; }
            if (!nl) break;
            tok = nl + 1;
        }
        free(save);
        if (bad) { free(dup); zerr(err_msg, "zip: unsafe path component (traversal?)"); return false; }
    }

    *root = myon_strdup(first);
    *rest = (remainder && *remainder) ? myon_strdup(remainder) : NULL;
    free(dup);
    return true;
}

/* A parsed central-directory entry we keep for the two-pass extract. */
typedef struct {
    char    *name;
    uint16_t method;
    uint32_t crc;
    uint32_t comp_size;
    uint32_t uncomp_size;
    uint32_t local_offset;
    uint32_t ext_attrs;   /* upper 16 bits carry the Unix mode */
    uint16_t flags;
    bool     is_dir;
} ZEntry;

/* Write a decompressed file to <staging>/<relpath>, creating parents. */
static bool write_file(const char *staging, const char *relpath,
                       const unsigned char *data, size_t len, char **err_msg) {
    char *full = pkg_fs_join(staging, relpath);
    /* create parent dirs */
    char *parent = myon_strdup(full);
    char *slash = strrchr(parent, '/');
    if (slash) { *slash = '\0'; if (!pkg_fs_mkdirs(parent, err_msg)) { free(parent); free(full); return false; } }
    free(parent);

    FILE *f = fopen(full, "wb");
    if (!f) { if (err_msg) { size_t n = strlen(full) + 40; *err_msg = myon_xmalloc(n); snprintf(*err_msg, n, "zip: cannot create '%s'", full); } free(full); return false; }
    bool ok = (len == 0) || (fwrite(data, 1, len, f) == len);
    if (fclose(f) != 0) ok = false;
    if (!ok) zerr(err_msg, "zip: short write extracting entry");
    free(full);
    return ok;
}

bool pkg_zip_extract(const unsigned char *zip_data, size_t zip_len,
                     const char *staging_dir, char **out_root_name,
                     char **err_msg) {
    if (out_root_name) *out_root_name = NULL;
    if (!zip_data || zip_len < 22) { zerr(err_msg, "zip: archive too small"); return false; }

    /* Find the End-Of-Central-Directory (EOCD) record, scanning back from the
     * end for its signature 0x06054b50 (allowing a trailing comment). */
    const unsigned char *eocd = NULL;
    size_t max_back = zip_len - 22;
    if (max_back > 65557) max_back = 65557; /* 22 + max 65535 comment */
    for (size_t i = 0; i <= max_back; i++) {
        const unsigned char *p = zip_data + (zip_len - 22 - i);
        if (rd32(p) == 0x06054b50u) { eocd = p; break; }
    }
    if (!eocd) { zerr(err_msg, "zip: end-of-central-directory not found (corrupt archive)"); return false; }

    uint16_t total_entries = rd16(eocd + 10);
    uint32_t cd_size       = rd32(eocd + 12);
    uint32_t cd_offset     = rd32(eocd + 16);

    /* ZIP64 sentinel values -> unsupported in this release (spec §8). */
    if (total_entries == 0xffff || cd_size == 0xffffffffu || cd_offset == 0xffffffffu) {
        zerr(err_msg, "zip: ZIP64 archives are not supported in this release");
        return false;
    }
    if (total_entries > PKG_ZIP_MAX_ENTRIES) { zerr(err_msg, "zip: too many entries"); return false; }
    if ((size_t)cd_offset + cd_size > zip_len) { zerr(err_msg, "zip: central directory out of bounds"); return false; }

    ZEntry *entries = total_entries ? myon_xmalloc(sizeof(ZEntry) * total_entries) : NULL;
    size_t nent = 0;
    bool ok = true;
    char *unique_root = NULL;
    unsigned long long total_uncomp = 0;

    const unsigned char *p = zip_data + cd_offset;
    const unsigned char *cd_end = p + cd_size;

    for (uint16_t e = 0; e < total_entries && ok; e++) {
        if (p + 46 > cd_end) { zerr(err_msg, "zip: truncated central directory"); ok = false; break; }
        if (rd32(p) != 0x02014b50u) { zerr(err_msg, "zip: bad central-directory signature"); ok = false; break; }

        uint16_t flags    = rd16(p + 8);
        uint16_t method   = rd16(p + 10);
        uint32_t crc      = rd32(p + 16);
        uint32_t comp     = rd32(p + 20);
        uint32_t uncomp   = rd32(p + 24);
        uint16_t nlen     = rd16(p + 28);
        uint16_t elen     = rd16(p + 30);
        uint16_t clen     = rd16(p + 32);
        uint32_t ext_attr = rd32(p + 38);
        uint32_t loff     = rd32(p + 42);

        if (p + 46 + nlen + elen + clen > cd_end) { zerr(err_msg, "zip: central-directory entry out of bounds"); ok = false; break; }
        if (flags & 0x0001) { zerr(err_msg, "zip: encrypted entries are not supported"); ok = false; break; }

        char *name = myon_strndup((const char *)(p + 46), nlen);

        char *root = NULL, *rest = NULL;
        if (!split_entry_name(name, &root, &rest, err_msg)) { free(name); ok = false; break; }

        /* Enforce a single top-level directory (spec §3.3, §8). */
        if (!unique_root) unique_root = myon_strdup(root);
        else if (strcmp(unique_root, root) != 0) {
            zerr(err_msg, "zip: archive has more than one top-level directory");
            free(root); free(rest); free(name); ok = false; break;
        }

        bool is_dir = (nlen > 0 && ((const char *)(p + 46))[nlen - 1] == '/');

        /* Reject non-regular POSIX file types via the external attributes
         * (upper 16 bits = st_mode on Unix-created archives). */
        unsigned mode = (ext_attr >> 16) & 0xffff;
        if (mode) {
            unsigned fmt = mode & 0170000u; /* S_IFMT */
            if (!is_dir && fmt != 0 && fmt != 0100000u /*S_IFREG*/) {
                zerr(err_msg, "zip: entry is a symlink/device/special file (rejected)");
                free(root); free(rest); free(name); ok = false; break;
            }
        }

        if (!is_dir) {
            if (uncomp > PKG_ZIP_MAX_ENTRY_UNCOMP) { zerr(err_msg, "zip: entry too large"); free(root); free(rest); free(name); ok = false; break; }
            if (comp > 0 && (unsigned long long)uncomp / comp > PKG_ZIP_MAX_RATIO && uncomp > 4096) {
                zerr(err_msg, "zip: decompression ratio too high (possible zip bomb)");
                free(root); free(rest); free(name); ok = false; break;
            }
            total_uncomp += uncomp;
            if (total_uncomp > PKG_ZIP_MAX_TOTAL_UNCOMP) { zerr(err_msg, "zip: total uncompressed size too large"); free(root); free(rest); free(name); ok = false; break; }
        }

        entries[nent].name         = name;
        entries[nent].method       = method;
        entries[nent].crc          = crc;
        entries[nent].comp_size    = comp;
        entries[nent].uncomp_size  = uncomp;
        entries[nent].local_offset = loff;
        entries[nent].ext_attrs    = ext_attr;
        entries[nent].flags        = flags;
        entries[nent].is_dir       = is_dir;
        nent++;
        free(root); free(rest);

        p += 46 + nlen + elen + clen;
    }

    /* Duplicate normalized path detection (spec §8). */
    for (size_t i = 0; ok && i < nent; i++)
        for (size_t j = i + 1; ok && j < nent; j++)
            if (strcmp(entries[i].name, entries[j].name) == 0) {
                zerr(err_msg, "zip: duplicate entry path");
                ok = false;
            }

    /* Second pass: extract each file entry from its local header. */
    for (size_t i = 0; ok && i < nent; i++) {
        ZEntry *ze = &entries[i];
        /* Compute the path with the generated root stripped. */
        char *rel = NULL;
        {
            char *root = NULL, *rest = NULL;
            if (!split_entry_name(ze->name, &root, &rest, err_msg)) { ok = false; break; }
            rel = rest ? rest : NULL; /* rest may be NULL for the root dir */
            free(root);
        }
        if (!rel) { /* the top-level dir entry itself: nothing to write */ continue; }

        if (ze->is_dir) {
            char *full = pkg_fs_join(staging_dir, rel);
            bool mok = pkg_fs_mkdirs(full, err_msg);
            free(full); free(rel);
            if (!mok) { ok = false; break; }
            continue;
        }

        /* Parse the local file header to find the data start. */
        if ((size_t)ze->local_offset + 30 > zip_len) { zerr(err_msg, "zip: local header out of bounds"); free(rel); ok = false; break; }
        const unsigned char *lh = zip_data + ze->local_offset;
        if (rd32(lh) != 0x04034b50u) { zerr(err_msg, "zip: bad local header signature"); free(rel); ok = false; break; }
        uint16_t lnlen = rd16(lh + 26);
        uint16_t lelen = rd16(lh + 28);
        size_t data_off = (size_t)ze->local_offset + 30 + lnlen + lelen;
        if (data_off + ze->comp_size > zip_len) { zerr(err_msg, "zip: entry data out of bounds"); free(rel); ok = false; break; }

        unsigned char *plain = pkg_zip_inflate(zip_data + data_off, ze->comp_size,
                                               ze->uncomp_size, ze->method);
        if (!plain) { zerr(err_msg, "zip: entry decompression failed (corrupt or unsupported method)"); free(rel); ok = false; break; }

        uint32_t got = crc32_bytes(plain, ze->uncomp_size);
        if (got != ze->crc) { zerr(err_msg, "zip: CRC mismatch (corrupt entry)"); free(plain); free(rel); ok = false; break; }

        bool wok = write_file(staging_dir, rel, plain, ze->uncomp_size, err_msg);
        free(plain); free(rel);
        if (!wok) { ok = false; break; }
    }

    if (ok && out_root_name && unique_root) *out_root_name = myon_strdup(unique_root);

    for (size_t i = 0; i < nent; i++) free(entries[i].name);
    free(entries);
    free(unique_root);
    return ok;
}
