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

#include "types.h"
#include "common.h"

#include <string.h>
#include <stdio.h>

const char *type_name(Type t) {
    switch (t) {
        case TYPE_UNKNOWN:   return "unknown";
        case TYPE_INT:       return "int";
        case TYPE_FLOAT:     return "float";
        case TYPE_STR:       return "str";
        case TYPE_CHAR:      return "char";
        case TYPE_BOOL:      return "bool";
        case TYPE_VOID:      return "void";
        case TYPE_ERROR:     return "error";
        case TYPE_NIL:       return "myon.nil";
        case TYPE_ARRAY:     return "myon.array";
        case TYPE_MAP:       return "myon.map";
        case TYPE_STRUCT:    return "struct";
        case TYPE_FUNC:      return "func";
        case TYPE_TYPEPARAM: return "typeparam";
        case TYPE_TASK:      return "task";
    }
    return "?";
}

int type_equal(Type a, Type b) {
    return a == b;
}

/* ------------------------------------------------------------------ */
/* TypeSpec                                                            */
/* ------------------------------------------------------------------ */

TypeSpec *typespec_new(Type base) {
    TypeSpec *t = (TypeSpec *)myon_xmalloc(sizeof(TypeSpec));
    t->base = base;
    t->name = NULL;
    t->elem = NULL;
    t->key = NULL;
    t->args = NULL;
    t->arg_count = 0;
    return t;
}

TypeSpec *typespec_prim(Type base) {
    return typespec_new(base);
}

TypeSpec *typespec_named(const char *name) {
    /* A bare identifier annotation: could be a struct name or a generic
     * type parameter. The parser tags it TYPE_STRUCT by default; the
     * interpreter treats unknown names as type parameters when appropriate. */
    TypeSpec *t = typespec_new(TYPE_STRUCT);
    t->name = myon_strdup(name);
    return t;
}

TypeSpec *typespec_clone(const TypeSpec *t) {
    if (!t) return NULL;
    TypeSpec *c = typespec_new(t->base);
    if (t->name) c->name = myon_strdup(t->name);
    c->elem = typespec_clone(t->elem);
    c->key  = typespec_clone(t->key);
    if (t->arg_count > 0) {
        c->args = (TypeSpec **)myon_xmalloc(sizeof(TypeSpec *) * t->arg_count);
        c->arg_count = t->arg_count;
        for (int i = 0; i < t->arg_count; i++)
            c->args[i] = typespec_clone(t->args[i]);
    }
    return c;
}

void typespec_free(TypeSpec *t) {
    if (!t) return;
    free(t->name);
    typespec_free(t->elem);
    typespec_free(t->key);
    for (int i = 0; i < t->arg_count; i++)
        typespec_free(t->args[i]);
    free(t->args);
    free(t);
}

char *typespec_to_cstr(const TypeSpec *t) {
    if (!t) return myon_strdup("<none>");
    char buf[256];
    switch (t->base) {
        case TYPE_ARRAY: {
            char *e = typespec_to_cstr(t->elem);
            snprintf(buf, sizeof(buf), "myon.array(%s)", e);
            free(e);
            return myon_strdup(buf);
        }
        case TYPE_MAP: {
            char *k = typespec_to_cstr(t->key);
            char *v = typespec_to_cstr(t->elem);
            snprintf(buf, sizeof(buf), "myon.map(%s, %s)", k, v);
            free(k); free(v);
            return myon_strdup(buf);
        }
        case TYPE_STRUCT:
        case TYPE_TYPEPARAM:
            if (t->arg_count > 0) {
                char tmp[256];
                size_t len = (size_t)snprintf(tmp, sizeof(tmp), "%s<",
                                              t->name ? t->name : "?");
                for (int i = 0; i < t->arg_count && len < sizeof(tmp); i++) {
                    char *a = typespec_to_cstr(t->args[i]);
                    len += (size_t)snprintf(tmp + len, sizeof(tmp) - len, "%s%s",
                                            i ? ", " : "", a);
                    free(a);
                }
                if (len < sizeof(tmp)) snprintf(tmp + len, sizeof(tmp) - len, ">");
                return myon_strdup(tmp);
            }
            return myon_strdup(t->name ? t->name : "?");
        default:
            return myon_strdup(type_name(t->base));
    }
}

int typespec_equal(const TypeSpec *a, const TypeSpec *b) {
    if (a == b) return 1;
    if (!a || !b) return 0;
    if (a->base != b->base) return 0;
    switch (a->base) {
        case TYPE_ARRAY:
            return typespec_equal(a->elem, b->elem);
        case TYPE_MAP:
            return typespec_equal(a->key, b->key) && typespec_equal(a->elem, b->elem);
        case TYPE_STRUCT:
        case TYPE_TYPEPARAM: {
            if (!a->name || !b->name || strcmp(a->name, b->name) != 0) return 0;
            if (a->arg_count != b->arg_count) return 0;
            for (int i = 0; i < a->arg_count; i++)
                if (!typespec_equal(a->args[i], b->args[i])) return 0;
            return 1;
        }
        default:
            return 1;
    }
}
