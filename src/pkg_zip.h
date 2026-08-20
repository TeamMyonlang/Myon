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

#ifndef MYON_PKG_ZIP_H
#define MYON_PKG_ZIP_H

/*
 * Self-contained, security-first ZIP reader for the package manager (spec §8).
 *
 * We do NOT shell out to `unzip`/`git`, and we do NOT add a new external
 * dependency: the inflate (DEFLATE decompression) core is a compact,
 * self-contained implementation in pkg_zip.c so the same code builds on Linux
 * and Windows and can be run under ASan/UBSan.
 *
 * The reader parses the End-Of-Central-Directory record and the central
 * directory (the authoritative index of a ZIP), then for each entry validates
 * the name and limits BEFORE extracting.  It rejects the whole archive on any
 * of the spec §8 hazards:
 *
 *   - ZIP-Slip ("../"), absolute paths, Windows drive paths, backslash bypass,
 *   - NUL / control bytes in names, duplicate normalized paths,
 *   - symlink / hardlink / device / FIFO entries (via the external-attrs mode),
 *   - encrypted entries, ZIP64 (unsupported in this release),
 *   - more than one top-level directory,
 *   - too many entries, oversized (compressed or uncompressed) data,
 *   - decompression-bomb ratio, corrupt central directory, bad CRC.
 *
 * Extraction always targets a caller-provided staging directory (never the
 * final install directory — spec §9), and strips the single generated top-level
 * directory that GitHub archives wrap everything in (spec §3.3).
 */

#include <stddef.h>
#include <stdbool.h>

/* Hard limits (spec §8).  Chosen small; tune via these constants only. */
#define PKG_ZIP_MAX_ENTRIES        20000
#define PKG_ZIP_MAX_TOTAL_UNCOMP   (256u * 1024u * 1024u) /* 256 MiB */
#define PKG_ZIP_MAX_ENTRY_UNCOMP   (64u  * 1024u * 1024u) /* 64 MiB  */
#define PKG_ZIP_MAX_RATIO          200  /* uncompressed/compressed cap */
#define PKG_ZIP_MAX_NAME           1024

/*
 * Inspect + extract `zip_data`/`zip_len` into `staging_dir`.
 *
 * On success:
 *   - every entry has been written under staging_dir with the single generated
 *     top-level directory stripped,
 *   - *out_root_name (if non-NULL) receives a heap copy of the stripped
 *     top-level directory name (caller frees),
 *   - returns true.
 *
 * On any validation or I/O failure returns false and, when err_msg is non-NULL,
 * stores a heap-allocated diagnostic (caller frees).  A failure never leaves a
 * usable partial result the caller would mistake for success; the caller is
 * expected to rmtree the staging dir on failure.
 */
bool pkg_zip_extract(const unsigned char *zip_data, size_t zip_len,
                     const char *staging_dir, char **out_root_name,
                     char **err_msg);

/*
 * Raw DEFLATE / stored decompressor used by pkg_zip_extract, exposed for unit
 * tests.  Decompress `src_len` bytes of DEFLATE (method 8) or stored (method 0)
 * data into a fresh heap buffer of exactly `expected_out` bytes (the ZIP's
 * recorded uncompressed size).  Returns the buffer (caller frees) or NULL on
 * malformed input / size mismatch.  `method` is 0 (stored) or 8 (deflate).
 */
unsigned char *pkg_zip_inflate(const unsigned char *src, size_t src_len,
                               size_t expected_out, int method);

#endif /* MYON_PKG_ZIP_H */
