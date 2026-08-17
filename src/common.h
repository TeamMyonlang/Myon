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

#ifndef MYON_COMMON_H
#define MYON_COMMON_H

#include <stdlib.h>
#include <stdio.h>

/* xmalloc / xrealloc: allocate or abort. Interpreter-grade convenience. */
static inline void *myon_xmalloc(size_t n) {
    void *p = malloc(n);
    if (!p) {
        fprintf(stderr, "myon: out of memory\n");
        exit(70);
    }
    return p;
}

static inline void *myon_xrealloc(void *ptr, size_t n) {
    void *p = realloc(ptr, n);
    if (!p) {
        fprintf(stderr, "myon: out of memory\n");
        exit(70);
    }
    return p;
}

char *myon_strndup(const char *s, size_t n);
char *myon_strdup(const char *s);

#endif /* MYON_COMMON_H */
