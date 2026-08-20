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

#include "pkg_hash.h"

#include <string.h>

#include <openssl/evp.h>

static const char HEX[] = "0123456789abcdef";

bool pkg_sha256_hex(const unsigned char *data, size_t len, char *out_hex) {
    if (!out_hex) return false;
    if (len != 0 && data == NULL) return false;

    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int dlen = 0;

    /*
     * EVP one-shot: create context, init SHA-256, feed the buffer, finalise.
     * Any failure path frees the context and reports false so the caller fails
     * closed (spec §9: a hash that cannot be computed must not "pass").
     */
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    if (!ctx) return false;

    bool ok = EVP_DigestInit_ex(ctx, EVP_sha256(), NULL) == 1;
    if (ok && len > 0) {
        ok = EVP_DigestUpdate(ctx, data, len) == 1;
    }
    if (ok) {
        ok = EVP_DigestFinal_ex(ctx, digest, &dlen) == 1;
    }
    EVP_MD_CTX_free(ctx);

    if (!ok || dlen != 32) return false;

    for (unsigned int i = 0; i < 32; i++) {
        out_hex[i * 2]     = HEX[(digest[i] >> 4) & 0x0f];
        out_hex[i * 2 + 1] = HEX[digest[i] & 0x0f];
    }
    out_hex[64] = '\0';
    return true;
}

bool pkg_sha256_equal(const char *a, const char *b) {
    if (!a || !b) return false;
    /*
     * Constant-time over the fixed 64-char digest width.  We first check the
     * NUL terminators are both at index 64 (a length mismatch is an immediate,
     * non-secret "not equal"), then fold every byte difference into an
     * accumulator so the loop time is independent of the mismatch position.
     */
    size_t la = strlen(a), lb = strlen(b);
    if (la != 64 || lb != 64) return false;

    unsigned char diff = 0;
    for (size_t i = 0; i < 64; i++) {
        diff |= (unsigned char)(a[i] ^ b[i]);
    }
    return diff == 0;
}
