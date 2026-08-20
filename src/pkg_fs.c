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

/* Feature-test macros for POSIX facilities (getcwd, mkdir, lstat, ...). */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#  define _POSIX_C_SOURCE 200809L
#endif
#if defined(__APPLE__)
#  define _DARWIN_C_SOURCE 1
#endif

/*
 * rand_s() (a CSPRNG on Windows) is only declared by <stdlib.h> when
 * _CRT_RAND_S is defined *before* <stdlib.h> is first included.  Without
 * this, a Windows/MinGW build fails with an implicit-declaration error
 * (see build log: "implicit declaration of function 'rand_s'").  Define it
 * unconditionally on Windows targets ahead of every standard header.
 */
#if defined(_WIN32) || defined(_WIN64)
#  ifndef _CRT_RAND_S
#    define _CRT_RAND_S
#  endif
#endif

#include "platform.h"
#include "pkg_fs.h"
#include "common.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#if defined(MYON_OS_WINDOWS)
#  include <direct.h>
#  include <windows.h>
#  define getcwd _getcwd
#else
#  include <unistd.h>
#  include <sys/stat.h>
#  include <sys/types.h>
#  include <dirent.h>
#endif

/* ------------------------------------------------------------------ */
/* small helpers                                                       */
/* ------------------------------------------------------------------ */

static void set_err(char **err_msg, const char *fmt, const char *a) {
    if (!err_msg) return;
    size_t n = strlen(fmt) + (a ? strlen(a) : 0) + 8;
    char *m = myon_xmalloc(n);
    snprintf(m, n, fmt, a ? a : "");
    *err_msg = m;
}

char *pkg_fs_join(const char *a, const char *b) {
    size_t la = strlen(a), lb = strlen(b);
    /* drop a trailing slash on `a` so we never produce "a//b". */
    bool trail = (la > 0 && (a[la - 1] == '/' || a[la - 1] == '\\'));
    size_t need = la + 1 + lb + 1;
    char *out = myon_xmalloc(need);
    if (trail) snprintf(out, need, "%.*s%s", (int)(la - 1), a, "/");
    else       snprintf(out, need, "%s/", a);
    /* append b */
    strncat(out, b, need - strlen(out) - 1);
    return out;
}

bool pkg_fs_is_dir(const char *path) {
#if defined(MYON_OS_WINDOWS)
    DWORD a = GetFileAttributesA(path);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
#else
    struct stat st;
    if (stat(path, &st) != 0) return false;
    return S_ISDIR(st.st_mode);
#endif
}

bool pkg_fs_is_file(const char *path) {
#if defined(MYON_OS_WINDOWS)
    DWORD a = GetFileAttributesA(path);
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
#else
    struct stat st;
    if (stat(path, &st) != 0) return false;
    return S_ISREG(st.st_mode);
#endif
}

/* Create a single directory; success if it already exists as a directory. */
static bool mkdir_one(const char *path, char **err_msg) {
#if defined(MYON_OS_WINDOWS)
    if (_mkdir(path) == 0) return true;
#else
    if (mkdir(path, 0755) == 0) return true;
#endif
    if (errno == EEXIST) {
        if (pkg_fs_is_dir(path)) return true;
        set_err(err_msg, "'%s' exists and is not a directory", path);
        return false;
    }
    set_err(err_msg, "cannot create directory '%s'", path);
    return false;
}

bool pkg_fs_mkdirs(const char *path, char **err_msg) {
    if (!path || !*path) { set_err(err_msg, "empty directory path%s", ""); return false; }
    /* Work on a mutable copy, creating each prefix in turn. */
    size_t n = strlen(path);
    char *buf = myon_xmalloc(n + 1);
    memcpy(buf, path, n + 1);

    for (size_t i = 1; i <= n; i++) {
        if (buf[i] == '/' || buf[i] == '\\' || buf[i] == '\0') {
            char save = buf[i];
            buf[i] = '\0';
            /* skip a bare root like "" or a drive prefix "C:" on Windows */
            if (buf[0] != '\0' && !(i == 1 && (buf[0] == '/' || buf[0] == '\\'))) {
                if (!mkdir_one(buf, err_msg)) { free(buf); return false; }
            }
            buf[i] = save;
        }
    }
    free(buf);
    return true;
}

/* ------------------------------------------------------------------ */
/* recursive remove                                                    */
/* ------------------------------------------------------------------ */

#if defined(MYON_OS_WINDOWS)
static bool rmtree_win(const char *path, char **err_msg) {
    DWORD a = GetFileAttributesA(path);
    if (a == INVALID_FILE_ATTRIBUTES) return true; /* absent == success */
    if (!(a & FILE_ATTRIBUTE_DIRECTORY)) {
        if (DeleteFileA(path)) return true;
        set_err(err_msg, "cannot remove file '%s'", path);
        return false;
    }
    /* directory: enumerate and recurse */
    char pattern[4096];
    snprintf(pattern, sizeof(pattern), "%s\\*", path);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pattern, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0)
                continue;
            char *child = pkg_fs_join(path, fd.cFileName);
            bool ok = rmtree_win(child, err_msg);
            free(child);
            if (!ok) { FindClose(h); return false; }
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    if (RemoveDirectoryA(path)) return true;
    set_err(err_msg, "cannot remove directory '%s'", path);
    return false;
}
#else
static bool rmtree_posix(const char *path, char **err_msg) {
    struct stat st;
    if (lstat(path, &st) != 0) {
        if (errno == ENOENT) return true; /* absent == success */
        set_err(err_msg, "cannot stat '%s'", path);
        return false;
    }
    /* Symlinks (and any non-directory) are unlinked, never followed. */
    if (!S_ISDIR(st.st_mode)) {
        if (unlink(path) == 0) return true;
        set_err(err_msg, "cannot remove '%s'", path);
        return false;
    }
    DIR *d = opendir(path);
    if (!d) { set_err(err_msg, "cannot open directory '%s'", path); return false; }
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        char *child = pkg_fs_join(path, e->d_name);
        bool ok = rmtree_posix(child, err_msg);
        free(child);
        if (!ok) { closedir(d); return false; }
    }
    closedir(d);
    if (rmdir(path) == 0) return true;
    set_err(err_msg, "cannot remove directory '%s'", path);
    return false;
}
#endif

bool pkg_fs_rmtree(const char *path, char **err_msg) {
    if (!path || !*path) return true;
#if defined(MYON_OS_WINDOWS)
    return rmtree_win(path, err_msg);
#else
    return rmtree_posix(path, err_msg);
#endif
}

/* ------------------------------------------------------------------ */
/* path-component safety                                               */
/* ------------------------------------------------------------------ */

bool pkg_fs_safe_component(const char *seg) {
    if (!seg || seg[0] == '\0') return false;
    if (strcmp(seg, ".") == 0 || strcmp(seg, "..") == 0) return false;
    for (const char *p = seg; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c < 0x20 || c == 0x7f) return false;   /* control / NUL region  */
        if (c == '/' || c == '\\') return false;   /* separators            */
        if (c == ':') return false;                 /* drive-letter / ADS   */
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* project-root discovery                                              */
/* ------------------------------------------------------------------ */

char *pkg_fs_find_project_root_from(const char *start_dir, char **err_msg) {
    char dir[4096];
    if (start_dir && start_dir[0]) {
        snprintf(dir, sizeof(dir), "%s", start_dir);
    } else {
        if (!getcwd(dir, sizeof(dir))) {
            set_err(err_msg, "cannot determine current directory%s", "");
            return NULL;
        }
    }
    for (int depth = 0; depth < 256; depth++) {
        char *probe = pkg_fs_join(dir, "myon.toml");
        bool found = pkg_fs_is_file(probe);
        free(probe);
        if (found) return myon_strdup(dir);

        char *slash = strrchr(dir, '/');
#if defined(MYON_OS_WINDOWS)
        char *bslash = strrchr(dir, '\\');
        if (bslash && (!slash || bslash > slash)) slash = bslash;
#endif
        if (!slash) break;
        if (slash == dir) { dir[1] = '\0'; break; } /* filesystem root */
        *slash = '\0';
    }
    set_err(err_msg,
            "no myon.toml found in this directory or any parent%s", "");
    return NULL;
}

char *pkg_fs_find_project_root(char **err_msg) {
    return pkg_fs_find_project_root_from(NULL, err_msg);
}

/* ------------------------------------------------------------------ */
/* staging directory + atomic promote                                  */
/* ------------------------------------------------------------------ */

/* Fill `out` (>= 17 bytes) with 16 random lowercase-hex characters + NUL. */
static bool random_token(char *out) {
    static const char HEX[] = "0123456789abcdef";
    unsigned char raw[8];
#if defined(MYON_OS_WINDOWS)
    /* rand_s is a CSPRNG on Windows. */
    for (int i = 0; i < 8; i++) {
        unsigned int v = 0;
        if (rand_s(&v) != 0) return false;
        raw[i] = (unsigned char)(v & 0xff);
    }
#else
    FILE *f = fopen("/dev/urandom", "rb");
    if (!f) return false;
    size_t got = fread(raw, 1, sizeof(raw), f);
    fclose(f);
    if (got != sizeof(raw)) return false;
#endif
    for (int i = 0; i < 8; i++) {
        out[i * 2]     = HEX[(raw[i] >> 4) & 0x0f];
        out[i * 2 + 1] = HEX[raw[i] & 0x0f];
    }
    out[16] = '\0';
    return true;
}

char *pkg_fs_make_staging(const char *root, char **err_msg) {
    char *dotmyon  = pkg_fs_join(root, PKG_DIR_DOTMYON);
    char *staging  = pkg_fs_join(dotmyon, PKG_DIR_STAGING);
    free(dotmyon);
    if (!pkg_fs_mkdirs(staging, err_msg)) { free(staging); return NULL; }

    /* Try a handful of random names to avoid a collision. */
    for (int attempt = 0; attempt < 8; attempt++) {
        char tok[17];
        if (!random_token(tok)) {
            set_err(err_msg, "cannot obtain randomness for staging dir%s", "");
            free(staging);
            return NULL;
        }
        char *cand = pkg_fs_join(staging, tok);
        /* mkdir fails if it already exists, giving us atomic uniqueness. */
        char *ignore = NULL;
        if (mkdir_one(cand, &ignore)) { free(staging); return cand; }
        free(ignore);
        free(cand);
    }
    set_err(err_msg, "could not create a unique staging directory%s", "");
    free(staging);
    return NULL;
}

bool pkg_fs_promote(const char *staged, const char *final_dir, char **err_msg) {
    /* Ensure the parent of final_dir exists (e.g. .myon/packages). */
    char *parent = myon_strdup(final_dir);
    char *slash = strrchr(parent, '/');
#if defined(MYON_OS_WINDOWS)
    char *bslash = strrchr(parent, '\\');
    if (bslash && (!slash || bslash > slash)) slash = bslash;
#endif
    if (slash) {
        *slash = '\0';
        if (!pkg_fs_mkdirs(parent, err_msg)) { free(parent); return false; }
    }
    free(parent);

    char *backup = NULL;
    bool had_existing = pkg_fs_is_dir(final_dir);
    if (had_existing) {
        /* Move the existing install aside to a sibling backup name. */
        size_t n = strlen(final_dir) + 16;
        backup = myon_xmalloc(n);
        snprintf(backup, n, "%s.bak-old", final_dir);
        /* Clear any stale backup first. */
        pkg_fs_rmtree(backup, NULL);
        if (rename(final_dir, backup) != 0) {
            set_err(err_msg, "cannot move existing install aside for '%s'", final_dir);
            free(backup);
            return false;
        }
    }

    if (rename(staged, final_dir) != 0) {
        /* Roll back: restore the backup so an existing good install survives. */
        if (had_existing && backup) {
            pkg_fs_rmtree(final_dir, NULL);
            rename(backup, final_dir);
        }
        set_err(err_msg, "cannot move staged package into place at '%s'", final_dir);
        free(backup);
        return false;
    }

    /* Success: drop the backup. */
    if (backup) { pkg_fs_rmtree(backup, NULL); free(backup); }
    return true;
}
