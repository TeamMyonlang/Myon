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

#include "platform.h"
#include "ffi_platform.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Duplicate a C string onto the heap (self-contained so this file has no
 * dependency on the rest of the interpreter). */
static char *ffi_dup(const char *s) {
    size_t n = strlen(s) + 1;
    char *p = (char *)malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

/*
 * POSIX (Linux, macOS, the BSDs): the C FFI dynamic loader is the standard
 * dlopen/dlsym/dlclose family from <dlfcn.h>.  macOS ships exactly this API in
 * libSystem (no extra link flag), so it shares the Linux implementation rather
 * than the old "not supported on macOS yet" stub that used to sit here.  See
 * platform.h (MYON_HAVE_DLOPEN).
 */
#if defined(MYON_HAVE_DLOPEN)

#include <dlfcn.h>

struct FFILib {
    void *handle; /* dlopen handle */
};

FFILib *ffi_platform_load(const char *path, char **err_msg) {
    void *h = dlopen(path, RTLD_NOW | RTLD_GLOBAL);
    if (!h) {
        const char *e = dlerror();
        if (err_msg) *err_msg = ffi_dup(e ? e : "dlopen failed");
        return NULL;
    }
    FFILib *lib = (FFILib *)malloc(sizeof(FFILib));
    if (!lib) {
        dlclose(h);
        if (err_msg) *err_msg = ffi_dup("out of memory loading library");
        return NULL;
    }
    lib->handle = h;
    return lib;
}

void *ffi_platform_sym(FFILib *lib, const char *name) {
    if (!lib || !lib->handle) return NULL;
    /* Clear any stale error, then resolve. */
    dlerror();
    return dlsym(lib->handle, name);
}

void ffi_platform_close(FFILib *lib) {
    if (!lib) return;
    if (lib->handle) dlclose(lib->handle);
    free(lib);
}

int ffi_platform_supported(void) {
    return 1;
}

#elif defined(_WIN32)

/*
 * Windows implementation of the FFI platform layer.
 *
 * Maps onto the Win32 run-time dynamic-linking API:
 *   ffi_platform_load  -> LoadLibraryA   (kernel32, implicitly linked)
 *   ffi_platform_sym   -> GetProcAddress
 *   ffi_platform_close -> FreeLibrary
 *
 * <windows.h> is included only inside this _WIN32 branch so the Linux/macOS
 * builds are entirely unaffected.  LoadLibrary/GetProcAddress/FreeLibrary all
 * live in kernel32.dll, which the Windows loader links implicitly, so no extra
 * -l flag is required at link time (see the Makefile).
 */
#include <windows.h>

struct FFILib {
    HMODULE handle; /* LoadLibraryA module handle */
};

/*
 * Turn the current GetLastError() code into a human-readable, heap-allocated
 * string (owned by the caller, freed with free() — matching the Linux path).
 *
 * FormatMessageA with FORMAT_MESSAGE_ALLOCATE_BUFFER writes its result into a
 * buffer it allocates via LocalAlloc, so that buffer must be released with
 * LocalFree.  We copy it into a malloc'd string (via ffi_dup) so the rest of
 * the interpreter can keep using free() uniformly across platforms.
 *
 * FORMAT_MESSAGE_IGNORE_INSERTS is mandatory here: we are formatting an
 * arbitrary system error code, and per the Win32 documentation it is unsafe to
 * pass such a code with inserts enabled (the message may reference insert
 * arguments we do not supply).
 */
static char *ffi_win32_error(const char *fallback) {
    DWORD code = GetLastError();
    LPSTR sysbuf = NULL;
    DWORD len = FormatMessageA(
        FORMAT_MESSAGE_ALLOCATE_BUFFER |
        FORMAT_MESSAGE_FROM_SYSTEM |
        FORMAT_MESSAGE_IGNORE_INSERTS,
        NULL,
        code,
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT),
        (LPSTR)&sysbuf,
        0,
        NULL);

    char *result;
    if (len == 0 || sysbuf == NULL) {
        /* FormatMessageA failed (or produced nothing): fall back to a plain
         * message that still carries the numeric error code. */
        char buf[128];
        (void)snprintf(buf, sizeof(buf),
                       "%s (error %lu)", fallback, (unsigned long)code);
        result = ffi_dup(buf);
    } else {
        /* System messages usually end with a trailing "\r\n"; trim it so the
         * error text lines up with the terse Linux dlerror() strings. */
        while (len > 0 &&
               (sysbuf[len - 1] == '\r' || sysbuf[len - 1] == '\n' ||
                sysbuf[len - 1] == ' '  || sysbuf[len - 1] == '.')) {
            sysbuf[--len] = '\0';
        }
        result = ffi_dup(sysbuf);
    }

    if (sysbuf) LocalFree(sysbuf);
    return result;
}

FFILib *ffi_platform_load(const char *path, char **err_msg) {
    HMODULE h = LoadLibraryA(path);
    if (!h) {
        if (err_msg) *err_msg = ffi_win32_error("LoadLibraryA failed");
        return NULL;
    }
    FFILib *lib = (FFILib *)malloc(sizeof(FFILib));
    if (!lib) {
        FreeLibrary(h);
        if (err_msg) *err_msg = ffi_dup("out of memory loading library");
        return NULL;
    }
    lib->handle = h;
    return lib;
}

void *ffi_platform_sym(FFILib *lib, const char *name) {
    if (!lib || !lib->handle) return NULL;
    /*
     * GetProcAddress returns a FARPROC (a function pointer).  Converting a
     * function pointer to a data pointer (void *) is not permitted by strict
     * ISO C, but is well-defined on every Windows ABI Myon targets and is the
     * standard way FFI layers surface resolved symbols.  Route the cast
     * through a uintptr_t to keep -Wpedantic quiet while staying explicit
     * about the intent.
     */
    FARPROC proc = GetProcAddress(lib->handle, name);
    return (void *)(uintptr_t)proc;
}

void ffi_platform_close(FFILib *lib) {
    if (!lib) return;
    if (lib->handle) FreeLibrary(lib->handle);
    free(lib);
}

int ffi_platform_supported(void) {
    return 1;
}

#else

/* Unknown platform: behave like the macOS/Windows stubs. */
struct FFILib { int unused; };

FFILib *ffi_platform_load(const char *path, char **err_msg) {
    (void)path;
    if (err_msg) *err_msg = ffi_dup("FFI is not supported on this platform");
    return NULL;
}

void *ffi_platform_sym(FFILib *lib, const char *name) {
    (void)lib; (void)name;
    return NULL;
}

void ffi_platform_close(FFILib *lib) {
    (void)lib;
}

int ffi_platform_supported(void) {
    return 0;
}

#endif
