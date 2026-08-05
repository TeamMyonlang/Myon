/*
 * Copyright 2026 nyan<(nyan4)
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

#include "env.h"
#include "common.h"

#include <string.h>
#include <stdlib.h>

Env *env_new(Env *parent) {
    Env *e = (Env *)myon_xmalloc(sizeof(Env));
    e->head = NULL;
    e->parent = parent;
    e->is_block = 0;
    return e;
}

void env_free(Env *env) {
    if (!env) return;
    Binding *b = env->head;
    while (b) {
        Binding *next = b->next;
        free(b->name);
        value_free(&b->value);
        free(b);
        b = next;
    }
    free(env);
}

static Binding *find_local(Env *env, const char *name) {
    for (Binding *b = env->head; b; b = b->next)
        if (strcmp(b->name, name) == 0) return b;
    return NULL;
}

int env_defined_local(Env *env, const char *name) {
    return find_local(env, name) != NULL;
}

int env_define(Env *env, const char *name, Value v) {
    if (find_local(env, name)) {
        value_free(&v);
        return 0; /* already defined in this scope */
    }
    Binding *b = (Binding *)myon_xmalloc(sizeof(Binding));
    b->name = myon_strdup(name);
    b->value = v;
    b->next = env->head;
    env->head = b;
    return 1;
}

int env_get(Env *env, const char *name, Value *out) {
    for (Env *e = env; e; e = e->parent) {
        Binding *b = find_local(e, name);
        if (b) {
            if (out) *out = value_copy(&b->value);
            return 1;
        }
    }
    return 0;
}

int env_set(Env *env, const char *name, Value v) {
    for (Env *e = env; e; e = e->parent) {
        Binding *b = find_local(e, name);
        if (b) {
            value_free(&b->value);
            b->value = v;
            return 1;
        }
    }
    value_free(&v);
    return 0;
}
