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

#ifndef MYON_PKG_FS_H
#define MYON_PKG_FS_H

/*
 * Package-manager filesystem layer (spec §5 project-root discovery, §9 atomic
 * install transaction).
 *
 * Everything here is deliberately confined to a project-local ".myon"
 * directory tree under a discovered project root (spec §2.5): there is no
 * global cache, no user-wide directory and no PATH mutation.  The POSIX and
 * Windows differences in rename / directory-replacement are isolated in the
 * .c file so callers see one portable API.
 *
 * A single boolean-returning convention is used: functions return true on
 * success and, on failure, write a heap-allocated message into *err_msg (when
 * err_msg is non-NULL) that the caller frees.
 */

#include <stddef.h>
#include <stdbool.h>

/* Subdirectory names under the project root (spec §2.5, §9). */
#define PKG_DIR_DOTMYON   ".myon"
#define PKG_DIR_PACKAGES  "packages"
#define PKG_DIR_STAGING   ".staging"

/*
 * Walk up from the current working directory looking for a directory that
 * contains a "myon.toml".  On success returns a fresh heap string with the
 * absolute-ish path of that directory (caller frees) and leaves *err_msg NULL.
 * On failure returns NULL and sets *err_msg.  Bounded walk (no infinite loop
 * on odd filesystems).
 */
char *pkg_fs_find_project_root(char **err_msg);

/* Join "a/b" into a fresh heap string (caller frees).  Never returns NULL. */
char *pkg_fs_join(const char *a, const char *b);

/* True if `path` names an existing directory. */
bool pkg_fs_is_dir(const char *path);
/* True if `path` names an existing regular file. */
bool pkg_fs_is_file(const char *path);

/*
 * Create `path` and any missing parents (like `mkdir -p`).  An already-existing
 * directory is success.  Returns false + *err_msg on a real error.
 */
bool pkg_fs_mkdirs(const char *path, char **err_msg);

/*
 * Recursively remove `path` (a file or a directory tree).  A non-existent path
 * is treated as success.  Refuses to follow symlinks (it unlinks them).
 * Returns false + *err_msg on a real error.
 */
bool pkg_fs_rmtree(const char *path, char **err_msg);

/*
 * Validate one path component that came from OUTSIDE the program (a ZIP entry
 * name segment or a manifest-derived module segment).  Rejects: the empty
 * string, ".", "..", a leading '/', any '/' or '\\', a drive-letter colon, NUL
 * or other control bytes.  This is the per-segment guard that, together with
 * the ZIP layer's whole-path check, blocks ZIP-Slip / traversal (spec §6.2,
 * §8).  Returns true if the single segment is safe.
 */
bool pkg_fs_safe_component(const char *seg);

/*
 * Create a fresh, unpredictable staging directory under
 * "<root>/.myon/.staging/" and return its full path (caller frees).  The name
 * is derived from a CSPRNG (never a fixed or predictable name — spec §9).
 * Returns NULL + *err_msg on failure.
 */
char *pkg_fs_make_staging(const char *root, char **err_msg);

/*
 * Atomically (as close as the platform allows) replace the directory `final`
 * with the directory `staged` (spec §9):
 *
 *   - ensures the parent of `final` exists,
 *   - if `final` already exists it is moved aside to a temporary backup,
 *   - `staged` is renamed into place as `final`,
 *   - on success the backup is removed; on failure the backup is restored so an
 *     existing good install is never lost.
 *
 * `staged` and `final` must be on the same filesystem (they are, both under
 * <root>/.myon).  Returns false + *err_msg on failure (with the previous
 * install left intact).
 */
bool pkg_fs_promote(const char *staged, const char *final_dir, char **err_msg);

#endif /* MYON_PKG_FS_H */
