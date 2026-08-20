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

#ifndef MYON_PKG_HASH_H
#define MYON_PKG_HASH_H

/*
 * SHA-256 for the package manager (spec §9).
 *
 * The archive integrity check computes a SHA-256 over the exact bytes of the
 * downloaded ZIP and compares it, constant-time, against the digest recorded
 * in myon.lock.  We reuse the OpenSSL that myon.http already links (see the
 * Makefile's -lssl -lcrypto) via the EVP API rather than the deprecated
 * low-level SHA256_* calls.
 *
 * The digest string form is fixed everywhere as exactly 64 lowercase hex
 * characters (spec §9), matching PKG_SHA256_HEX_LEN in package.h.
 */

#include <stddef.h>
#include <stdbool.h>

/*
 * Compute the SHA-256 of `len` bytes at `data` and write its lowercase-hex
 * form (64 chars + NUL) into `out_hex`, which must have room for at least 65
 * bytes.  Returns true on success, false if the digest could not be computed
 * (out of memory inside OpenSSL, unavailable algorithm, ...).  `data` may be
 * NULL only when `len` == 0.
 */
bool pkg_sha256_hex(const unsigned char *data, size_t len, char *out_hex);

/*
 * Constant-time comparison of two 64-char lowercase-hex SHA-256 strings.
 * Returns true iff they are byte-for-byte equal.  The running time does not
 * depend on where the first mismatch is (spec §9: "hash comparison は可能なら
 * constant-time comparison を使う").  Inputs are assumed NUL-terminated; a
 * length mismatch returns false without leaking the position.
 */
bool pkg_sha256_equal(const char *a, const char *b);

#endif /* MYON_PKG_HASH_H */
