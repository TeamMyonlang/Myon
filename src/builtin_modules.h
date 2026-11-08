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

#ifndef MYON_BUILTIN_MODULES_H
#define MYON_BUILTIN_MODULES_H
#include <stdint.h>
#include <string.h>

static const char *const myon_builtin_modules[] = {
    "myon.stdio", "myon.math", "myon.string", "myon.ffi", "myon.time",
    "myon.random", "myon.net", "myon.http", "myon.file", "myon.array", "myon.map"
};
static inline uint32_t myon_builtin_module_bit(const char *name) {
    for (unsigned int i = 0; i < sizeof(myon_builtin_modules)/sizeof(myon_builtin_modules[0]); i++)
        if (strcmp(name, myon_builtin_modules[i]) == 0) return UINT32_C(1) << i;
    return 0;
}
static inline const char *myon_call_module(const char *name) {
    for (unsigned int i = 0; i < sizeof(myon_builtin_modules)/sizeof(myon_builtin_modules[0]); i++) {
        size_t n = strlen(myon_builtin_modules[i]);
        if (strncmp(name, myon_builtin_modules[i], n) == 0 && name[n] == '.')
            return myon_builtin_modules[i];
    }
    static const char *const stdio[] = {"myon.print", "myon.println", "myon.write",
        "myon.eprint", "myon.flush", "myon.is_tty", "myon.argv", "myon.input"};
    for (unsigned int i = 0; i < sizeof(stdio)/sizeof(stdio[0]); i++)
        if (strcmp(name, stdio[i]) == 0) return "myon.stdio";
    return NULL;
}
#endif
