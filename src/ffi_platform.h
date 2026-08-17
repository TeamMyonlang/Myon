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

#ifndef MYON_FFI_PLATFORM_H
#define MYON_FFI_PLATFORM_H

/*
 * Phase3 C FFI — platform abstraction layer.
 *
 * A thin wrapper over the OS shared-library loader:
 *   POSIX   : dlopen / dlsym / dlclose
 *   Windows : LoadLibrary / GetProcAddress / FreeLibrary
 *
 * Linux and Windows carry real implementations.  macOS and any unknown
 * platform compile fine but fail at run time with an error message so the
 * interpreter can surface a clean Myon `error(...)` value.
 */

typedef struct FFILib FFILib; /* opaque library handle */

/*
 * Load a shared library.  On failure returns NULL and stores a heap-allocated
 * error message in *err_msg (the caller must free it).  On success *err_msg is
 * left untouched.
 */
FFILib *ffi_platform_load(const char *path, char **err_msg);

/* Look up a symbol.  Returns NULL if not found. */
void *ffi_platform_sym(FFILib *lib, const char *name);

/* Close a library. */
void ffi_platform_close(FFILib *lib);

/*
 * Whether FFI is usable on the current OS.  Returns 1 on Linux and Windows,
 * 0 on the macOS / unknown stubs.
 */
int ffi_platform_supported(void);

#endif /* MYON_FFI_PLATFORM_H */
