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

#ifndef MYON_ENV_H
#define MYON_ENV_H

#include "value.h"

/*
 * Lexically-scoped variable environment.
 * A chain of scopes; block scope groundwork is here for Step 8.
 */
typedef struct Binding {
    char           *name;
    Value           value;
    struct Binding *next;
} Binding;

typedef struct Env {
    Binding    *head;
    struct Env *parent;
    int         is_block;   /* explicit { } block scope (spec 9.2 shadowing) */
} Env;

Env  *env_new(Env *parent);
void  env_free(Env *env);   /* frees this scope's own bindings only */

/* Define a new binding in the *current* scope. Returns 0 if the name is
 * already defined in this exact scope (shadowing check groundwork). */
int   env_define(Env *env, const char *name, Value v);

/* Look up through the scope chain. Returns 1 and fills *out (a copy) on hit. */
int   env_get(Env *env, const char *name, Value *out);

/* Assign to an existing binding anywhere in the chain. Returns 0 if unbound. */
int   env_set(Env *env, const char *name, Value v);

/* Is `name` bound in this exact scope (not parents)? */
int   env_defined_local(Env *env, const char *name);

#endif /* MYON_ENV_H */
