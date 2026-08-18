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
#include <stdbool.h>
#include <stdint.h>

/*
 * Overflow-safe size arithmetic (flaw-and-Add B-3).
 *
 * The stdlib size / bounds calculations (string.repeat / substring / join,
 * array.slice, ...) used to reach for raw `+` / `*` on size_t and long long,
 * which either wraps around (allocating a too-small buffer -> heap overflow) or
 * triggers signed-integer UB.  These helpers wrap `__builtin_*_overflow` so
 * every size computation can go through one consistent, checked path — mirroring
 * how `int_arith` already guards the language's own integer operators.
 *
 * Each returns `true` on success (with the result stored through `out`) and
 * `false` if the operation would overflow SIZE_MAX; on overflow `*out` is left
 * unspecified and the caller is expected to surface an error value.
 */
static inline bool checked_add_size(size_t a, size_t b, size_t *out) {
    return !__builtin_add_overflow(a, b, out);
}

static inline bool checked_mul_size(size_t a, size_t b, size_t *out) {
    return !__builtin_mul_overflow(a, b, out);
}

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
