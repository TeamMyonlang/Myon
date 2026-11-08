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

#include "common.h"
#include <string.h>

char *myon_strndup(const char *s, size_t n) {
    char *p = (char *)myon_xmalloc(n + 1);
    memcpy(p, s, n);
    p[n] = '\0';
    return p;
}

char *myon_strdup(const char *s) {
    return myon_strndup(s, strlen(s));
}

bool myon_string_escape(const char **raw, char *out) {
    const char *s = *raw;
    if (s[0] != '\\') return false;
    switch (s[1]) {
        case 'n': *out = '\n'; break;
        case 'r': *out = '\r'; break;
        case 't': *out = '\t'; break;
        case '\\': *out = '\\'; break;
        case '"': *out = '"'; break;
        default: return false;
    }
    *raw += 2;
    return true;
}

struct MyonAllocScope {
    struct MyonAllocScope *parent;
    void **slots;
    size_t capacity, count, filled;
};
static _Thread_local MyonAllocScope *allocation_scope;
#define DELETED_SLOT ((void *)(uintptr_t)1)

static size_t allocation_hash(void *ptr) {
    uintptr_t h = (uintptr_t)ptr >> 3;
    h ^= h >> 16;
    return (size_t)(h * (uintptr_t)UINT32_C(0x9e3779b1));
}
static void allocation_insert(MyonAllocScope *scope, void *ptr);
static void allocation_rehash(MyonAllocScope *scope) {
    size_t capacity = scope->capacity ? scope->capacity : 64;
    if (scope->count >= capacity / 2 && !checked_mul_size(capacity, 2, &capacity)) exit(70);
    size_t bytes;
    if (!checked_mul_size(capacity, sizeof(void *), &bytes)) exit(70);
    void **old = scope->slots;
    size_t old_capacity = scope->capacity;
    scope->slots = calloc(1, bytes);
    if (!scope->slots) { fprintf(stderr, "myon: out of memory\n"); exit(70); }
    scope->capacity = capacity;
    scope->count = scope->filled = 0;
    for (size_t i = 0; i < old_capacity; i++)
        if (old[i] && old[i] != DELETED_SLOT) allocation_insert(scope, old[i]);
    free(old);
}
static void allocation_insert(MyonAllocScope *scope, void *ptr) {
    if (!scope->capacity || scope->filled >= scope->capacity - scope->capacity / 4)
        allocation_rehash(scope);
    size_t mask = scope->capacity - 1, index = allocation_hash(ptr) & mask;
    size_t deleted = SIZE_MAX;
    while (scope->slots[index]) {
        if (scope->slots[index] == ptr) return;
        if (scope->slots[index] == DELETED_SLOT && deleted == SIZE_MAX) deleted = index;
        index = (index + 1) & mask;
    }
    if (deleted != SIZE_MAX) index = deleted;
    else scope->filled++;
    scope->slots[index] = ptr;
    scope->count++;
}
static MyonAllocScope *allocation_remove(void *ptr) {
    if (!ptr) return NULL;
    for (MyonAllocScope *scope = allocation_scope; scope; scope = scope->parent) {
        if (!scope->capacity) continue;
        size_t mask = scope->capacity - 1, index = allocation_hash(ptr) & mask;
        while (scope->slots[index]) {
            if (scope->slots[index] == ptr) {
                scope->slots[index] = DELETED_SLOT;
                scope->count--;
                return scope;
            }
            index = (index + 1) & mask;
        }
    }
    return NULL;
}
void myon_alloc_track(void *ptr) {
    if (allocation_scope && ptr) allocation_insert(allocation_scope, ptr);
}
void myon_free(void *ptr) {
    allocation_remove(ptr);
    free(ptr);
}
void *myon_tracked_realloc(void *ptr, size_t n) {
    MyonAllocScope *owner = allocation_remove(ptr);
    void *out = realloc(ptr, n);
    if (!out) { fprintf(stderr, "myon: out of memory\n"); exit(70); }
    if (owner || allocation_scope) allocation_insert(owner ? owner : allocation_scope, out);
    return out;
}
MyonAllocScope *myon_alloc_scope_begin(void) {
    MyonAllocScope *scope = calloc(1, sizeof(*scope));
    if (!scope) { fprintf(stderr, "myon: out of memory\n"); exit(70); }
    scope->parent = allocation_scope;
    allocation_scope = scope;
    return scope;
}
void myon_alloc_scope_end(MyonAllocScope *scope, bool rollback) {
    if (scope != allocation_scope) abort();
    allocation_scope = scope->parent;
    for (size_t i = 0; i < scope->capacity; i++) {
        void *ptr = scope->slots[i];
        if (!ptr || ptr == DELETED_SLOT) continue;
        if (rollback) free(ptr);
        else if (scope->parent) allocation_insert(scope->parent, ptr);
    }
    free(scope->slots);
    free(scope);
}
